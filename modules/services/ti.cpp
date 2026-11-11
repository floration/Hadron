// modules/services/ti.cpp — see ti.h. Token work only; no SCM writes here.
#include "modules/services/ti.h"
#include "core/sys/sys.h"
#include <sddl.h>
#include <tlhelp32.h>

namespace hx::services {

const wchar_t kTiSid[] = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

namespace {

std::wstring Err(const wchar_t* what, DWORD e) { return std::wstring(what) + L": " + sys::ErrorText(e); }

// ImpersonateSelf + one privilege enabled on the thread token; RevertToSelf on scope exit. A privilege
// counts only if AdjustTokenPrivileges succeeded and GetLastError() != ERROR_NOT_ALL_ASSIGNED (rule 4).
class ThreadPrivilege {
public:
    explicit ThreadPrivilege(const wchar_t* name) {
        if (!ImpersonateSelf(SecurityImpersonation)) { err_ = GetLastError(); return; }
        impersonating_ = true;
        HANDLE tok = nullptr;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &tok)) { err_ = GetLastError(); return; }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) err_ = GetLastError();
        else {
            BOOL r = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
            DWORD gle = GetLastError();
            if (r && gle != ERROR_NOT_ALL_ASSIGNED) ok_ = true;
            else err_ = r ? ERROR_NOT_ALL_ASSIGNED : gle;
        }
        CloseHandle(tok);
    }
    ~ThreadPrivilege() { if (impersonating_) RevertToSelf(); }
    ThreadPrivilege(const ThreadPrivilege&) = delete;
    ThreadPrivilege& operator=(const ThreadPrivilege&) = delete;
    bool ok() const { return ok_; }
    DWORD err() const { return err_; }
private:
    bool impersonating_ = false, ok_ = false;
    DWORD err_ = 0;
};

// Saves the thread's impersonation token, sets tok, restores the saved one on scope exit (RevertToSelf if
// that fails, never staying on the swapped-in token).
class ThreadTokenSwap {
public:
    explicit ThreadTokenSwap(HANDLE tok) {
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &prev_)) {
            DWORD e = GetLastError();
            prev_ = nullptr;
            if (e != ERROR_NO_TOKEN) { err_ = e; return; }
        }
        if (SetThreadToken(nullptr, tok)) ok_ = true;
        else err_ = GetLastError();
    }
    ~ThreadTokenSwap() {
        if (ok_ && !SetThreadToken(nullptr, prev_)) RevertToSelf();
        if (prev_) CloseHandle(prev_);
    }
    ThreadTokenSwap(const ThreadTokenSwap&) = delete;
    ThreadTokenSwap& operator=(const ThreadTokenSwap&) = delete;
    bool ok() const { return ok_; }
    DWORD err() const { return err_; }
private:
    HANDLE prev_ = nullptr;
    bool ok_ = false;
    DWORD err_ = 0;
};

DWORD FindWinlogonPid() {
    DWORD ours = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ours)) return 0;
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD sess = 0;
            if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0 && ProcessIdToSessionId(pe.th32ProcessID, &sess) && sess == ours) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Opens pid's process token and duplicates it as an impersonation token (SecurityImpersonation).
HANDLE DupImpersonation(DWORD pid, DWORD* err) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) { *err = GetLastError(); return nullptr; }
    HANDLE src = nullptr, dup = nullptr;
    if (!OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY, &src)) *err = GetLastError();
    else {
        if (!DuplicateTokenEx(src, TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_DUPLICATE, nullptr, SecurityImpersonation,
                              TokenImpersonation, &dup)) {
            *err = GetLastError();
            dup = nullptr;
        }
        CloseHandle(src);
    }
    CloseHandle(proc);
    return dup;
}

}  // namespace

bool TiService::Ensure(SvcItem* ti, std::wstring* err) {
    started_ = false;
    std::wstring e;
    if (!b_->Query(L"TrustedInstaller", ti, &e)) { *err = L"could not read the TrustedInstaller service: " + e; return false; }
    // Stopping (Windows Update finishing, or its idle timeout): wait for STOPPED, then start it like a stopped one.
    DWORD s0 = GetTickCount();
    while (ti->state == SERVICE_STOP_PENDING && GetTickCount() - s0 < waitMs) {
        Sleep(pollMs);
        if (!b_->Query(L"TrustedInstaller", ti, &e)) { *err = L"could not read the TrustedInstaller service: " + e; return false; }
    }
    if (ti->state == SERVICE_STOPPED) {
        bool own = false;
        if (!b_->Control(L"TrustedInstaller", Op::Start, nullptr, &e, &own)) { *err = L"could not start TrustedInstaller: " + e; return false; }
        // Claimed only when our own start request started it. ERROR_SERVICE_ALREADY_RUNNING means Windows started it
        // between the Query and the start: it stays Windows's.
        started_ = own;
        if (!b_->Query(L"TrustedInstaller", ti, &e)) { *err = L"could not read TrustedInstaller after starting it: " + e; return false; }
    }
    // Someone else is starting it: wait for RUNNING, but it stays theirs.
    DWORD t0 = GetTickCount();
    while (ti->state == SERVICE_START_PENDING && GetTickCount() - t0 < waitMs) {
        Sleep(pollMs);
        if (!b_->Query(L"TrustedInstaller", ti, &e)) { *err = L"could not read the TrustedInstaller service: " + e; return false; }
    }
    if (ti->state != SERVICE_RUNNING) { *err = L"TrustedInstaller is not running (" + StateName(ti->state) + L")"; return false; }
    return true;
}

bool TiService::StopIfStarted(std::wstring* note) {
    if (!started_) return false;
    started_ = false;
    std::wstring e;
    if (!b_->Control(L"TrustedInstaller", Op::Stop, nullptr, &e)) {
        *note = L"Hadron started TrustedInstaller and could not stop it again: " + e;
        return false;
    }
    *note = L"Hadron started TrustedInstaller for this apply and stopped it again.";
    return true;
}

namespace {

class LiveTi : public TiProvider {
public:
    explicit LiveTi(Backend* b) : svc_(b) {}
    ~LiveTi() override { if (tok_) CloseHandle(tok_); }

    bool Acquire(std::wstring* err) override {
        if (tok_) { CloseHandle(tok_); tok_ = nullptr; }   // never reuse a token across applies
        // Start TrustedInstaller only if it is stopped (through the Backend); the claim is reset here.
        SvcItem ti;
        if (!svc_.Ensure(&ti, err)) return false;
        if (!ti.pid) { *err = L"TrustedInstaller is running but reports no process id"; return false; }
        // SeDebugPrivilege on this thread only.
        ThreadPrivilege dbg(L"SeDebugPrivilege");
        if (!dbg.ok()) { *err = Err(L"SeDebugPrivilege is not available", dbg.err()); return false; }
        // SYSTEM from winlogon in this session (TI's token DACL admits SYSTEM, not Administrators).
        DWORD wpid = FindWinlogonPid(), de = 0;
        if (!wpid) { *err = L"could not find winlogon.exe in this session"; return false; }
        HANDLE system = DupImpersonation(wpid, &de);
        if (!system) { *err = Err(L"could not get a SYSTEM token from winlogon", de); return false; }
        {
            ThreadTokenSwap swap(system);
            if (!swap.ok()) { CloseHandle(system); *err = Err(L"could not impersonate SYSTEM", swap.err()); return false; }
            tok_ = DupImpersonation(ti.pid, &de);   // TI's process token, duplicated for impersonation
        }
        CloseHandle(system);
        if (!tok_) { *err = Err(L"could not duplicate the TrustedInstaller token", de); return false; }
        return true;
    }

    bool StartedService() const override { return svc_.Started(); }

    bool Impersonate(std::wstring* identity, std::wstring* err) override {
        if (!tok_) { *err = L"no TrustedInstaller token"; return false; }
        if (!SetThreadToken(nullptr, tok_)) { *err = Err(L"SetThreadToken", GetLastError()); return false; }
        on_ = true;
        // Verify: impersonation can silently drop to Identification, and then the SCM sees no TI SID.
        HANDLE t = nullptr;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t)) { *err = Err(L"OpenThreadToken", GetLastError()); Revert(); return false; }
        SECURITY_IMPERSONATION_LEVEL lvl = SecurityAnonymous;
        DWORD got = 0;
        BOOL okLvl = GetTokenInformation(t, TokenImpersonationLevel, &lvl, sizeof(lvl), &got);
        DWORD lvlErr = okLvl ? 0 : GetLastError();
        CloseHandle(t);
        if (!okLvl) { *err = Err(L"reading the impersonation level", lvlErr); Revert(); return false; }
        if (lvl != SecurityImpersonation) { *err = L"the TrustedInstaller token is not at SecurityImpersonation level"; Revert(); return false; }
        PSID sid = nullptr;
        if (!ConvertStringSidToSidW(kTiSid, &sid)) { *err = Err(L"ConvertStringSidToSid", GetLastError()); Revert(); return false; }
        BOOL member = FALSE;
        BOOL okM = CheckTokenMembership(nullptr, sid, &member);
        DWORD mErr = okM ? 0 : GetLastError();
        LocalFree(sid);
        if (!okM) { *err = Err(L"CheckTokenMembership", mErr); Revert(); return false; }
        if (!member) { *err = L"the impersonated token does not carry the TrustedInstaller SID"; Revert(); return false; }
        *identity = L"ti";
        return true;
    }

    void Revert() override {
        if (on_ && !SetThreadToken(nullptr, nullptr)) RevertToSelf();
        on_ = false;
    }

    bool Release(std::wstring* note) override {
        if (tok_) { CloseHandle(tok_); tok_ = nullptr; }
        return svc_.StopIfStarted(note);
    }

private:
    TiService svc_;
    HANDLE tok_ = nullptr;
    bool on_ = false;
};

}  // namespace

std::unique_ptr<TiProvider> MakeLiveTi(Backend* backend) { return std::make_unique<LiveTi>(backend); }

}  // namespace hx::services
