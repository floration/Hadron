// modules/runas/launch.cpp — token/launch logic for Run As. See launch.h and docs/design/runas.md.
//
// SYSTEM / TrustedInstaller / another-user / limited launches cannot be exercised under Wine and
// are not run in CI beyond compiling; they are verified on the owner's PC (design "Testing"). The
// pure helpers below are what --selftest covers.
#include "modules/runas/launch.h"
#include "core/sys/sys.h"

#include <objbase.h>     // COM, before shlobj (WIN32_LEAN_AND_MEAN hides it otherwise)
#include <shellapi.h>    // ShellExecuteExW
#include <shlobj.h>
#include <shobjidl.h>    // IShellLinkW
#include <shlwapi.h>     // AssocQueryStringW
#include <lm.h>          // NetUserEnum
#include <tlhelp32.h>    // process snapshot (find winlogon)
#include <userenv.h>     // CreateEnvironmentBlock
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>   // _wtoi / _wtoi64
#include <cstring>   // _wcsicmp
#include <cwchar>    // swprintf

namespace hx::runas {
namespace {

// Well-known built-in accounts that NetUserEnum returns but we never offer as a launch identity.
bool IsServiceAccount(const std::wstring& name) {
    static const wchar_t* const kBlocked[] = {L"Guest", L"DefaultAccount", L"WDAGUtilityAccount"};
    for (const wchar_t* b : kBlocked)
        if (_wcsicmp(name.c_str(), b) == 0) return true;
    return false;
}

bool IsAbsolute(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;                 // C:\...
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') &&
        (p[1] == L'\\' || p[1] == L'/')) return true;              // \\server\share, \\?\...
    return false;
}

std::wstring DirOf(const std::wstring& path) {
    size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

std::wstring Ext(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    std::wstring e = path.substr(dot);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

std::wstring SystemDir() {
    wchar_t buf[MAX_PATH];
    UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return (n && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L"C:\\Windows\\System32");
}

// ---- token inspection ------------------------------------------------------------------------

std::wstring TokenUserName(HANDLE tok) {
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    if (!len) return L"";
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) return L"";
    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
    wchar_t name[256], dom[256];
    DWORD nl = 256, dl = 256;
    SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, tu->User.Sid, name, &nl, dom, &dl, &use)) {
        if (dl && dom[0]) return std::wstring(dom) + L"\\" + name;
        return std::wstring(name);
    }
    return L"";
}

DWORD TokenIntegrityRid(HANDLE tok) {
    DWORD len = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &len);
    if (!len) return 0;
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), len, &len)) return 0;
    auto* ml = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
    UCHAR* count = GetSidSubAuthorityCount(ml->Label.Sid);
    if (!count || *count == 0) return 0;
    return *GetSidSubAuthority(ml->Label.Sid, *count - 1);
}

void ReadBack(HANDLE hProc, LaunchResult* out) {
    if (!hProc) return;
    out->pid = GetProcessId(hProc);
    DWORD s = 0;
    if (ProcessIdToSessionId(out->pid, &s)) out->sessionId = s;
    HANDLE tok = nullptr;
    if (OpenProcessToken(hProc, TOKEN_QUERY, &tok)) {
        out->effectiveUser = TokenUserName(tok);
        out->integrityRid = TokenIntegrityRid(tok);
        out->integrityName = IntegrityName(out->integrityRid);
        CloseHandle(tok);
    }
}

DWORD ExpectedRid(const LaunchRequest& r) {
    if (r.mode == TokenMode::Limited) return 0x2000;   // SECURITY_MANDATORY_MEDIUM_RID
    switch (r.who) {
        case Who::System:
        case Who::TrustedInstaller: return 0x4000;     // SECURITY_MANDATORY_SYSTEM_RID
        default:                    return 0x3000;     // SECURITY_MANDATORY_HIGH_RID
    }
}

// Lower a primary token to medium integrity (for the Limited mode of SYSTEM/TI).
bool SetTokenIntegrityMedium(HANDLE tok, DWORD* err) {
    SID_IDENTIFIER_AUTHORITY mla = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID sid = nullptr;
    if (!AllocateAndInitializeSid(&mla, 1, SECURITY_MANDATORY_MEDIUM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &sid)) {
        if (err) *err = GetLastError();
        return false;
    }
    TOKEN_MANDATORY_LABEL ml{};
    ml.Label.Sid = sid;
    ml.Label.Attributes = SE_GROUP_INTEGRITY;
    BOOL ok = SetTokenInformation(tok, TokenIntegrityLevel, &ml,
                                  (DWORD)(sizeof(TOKEN_MANDATORY_LABEL) + GetLengthSid(sid)));
    if (!ok && err) *err = GetLastError();
    FreeSid(sid);
    return ok != FALSE;
}

// RAII: ImpersonateSelf, enable one privilege on the *thread* token only, RevertToSelf on scope
// exit. Modelled on diskmap's ThreadBackupPrivilege. Never touches the process token (a reviewer
// flagged exactly that leak in diskmap).
class ThreadPrivilege {
public:
    explicit ThreadPrivilege(const wchar_t* name) {
        if (!ImpersonateSelf(SecurityImpersonation)) { err_ = GetLastError(); return; }
        impersonating_ = true;
        HANDLE tok = nullptr;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &tok)) {
            err_ = GetLastError();
            return;
        }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
            err_ = GetLastError();
        } else {
            BOOL r = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
            DWORD gle = GetLastError();
            if (PrivilegeEnabled(r, gle)) ok_ = true;
            else err_ = (gle == ERROR_SUCCESS) ? ERROR_NOT_ALL_ASSIGNED : gle;
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

// ---- target resolution (non-exe targets need an exe for the CreateProcess* paths) -------------

struct Resolved {
    std::wstring app;       // may be empty: then cmdline carries the whole command
    std::wstring cmdline;   // mutable copy is made before CreateProcess (which may write to it)
    std::wstring workDir;
    bool ok = false;
    std::wstring error;
};

// Expand an association command template ("\"C:\\app.exe\" \"%1\"") with the target and args.
std::wstring ExpandAssoc(const std::wstring& tmpl, const std::wstring& file, const std::wstring& args) {
    std::wstring out;
    for (size_t i = 0; i < tmpl.size(); i++) {
        if (tmpl[i] == L'%' && i + 1 < tmpl.size()) {
            wchar_t c = tmpl[i + 1];
            if (c == L'1' || c == L'L' || c == L'l') { out += file; i++; continue; }
            if (c == L'*') { if (!args.empty()) out += args; i++; continue; }
            if (c == L'%') { out += L'%'; i++; continue; }
        }
        out += tmpl[i];
    }
    if (tmpl.find(L'%') == std::wstring::npos) {   // template had no placeholder: append the file
        out += L" " + QuoteArg(file);
        if (!args.empty()) out += L" " + args;
    }
    return out;
}

Resolved ResolveTarget(const LaunchRequest& req) {
    Resolved r;
    std::wstring target = AbsolutePath(req.path);
    std::wstring e = Ext(target);

    if (e == L".lnk") {
        IShellLinkW* link = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, reinterpret_cast<void**>(&link));
        if (SUCCEEDED(hr) && link) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf))) && pf) {
                if (SUCCEEDED(pf->Load(target.c_str(), STGM_READ))) {
                    wchar_t tpath[MAX_PATH] = {}, targ[2048] = {}, tdir[MAX_PATH] = {};
                    WIN32_FIND_DATAW fd{};
                    link->GetPath(tpath, MAX_PATH, &fd, SLGP_UNCPRIORITY);
                    link->GetArguments(targ, 2048);
                    link->GetWorkingDirectory(tdir, MAX_PATH);
                    if (tpath[0]) {
                        r.app = tpath;
                        std::wstring a = targ;
                        if (!req.args.empty()) a += (a.empty() ? L"" : L" ") + req.args;
                        r.cmdline = BuildCommandLine(tpath, a);
                        r.workDir = req.workingDir.empty() ? (tdir[0] ? std::wstring(tdir) : DirOf(tpath))
                                                           : req.workingDir;
                        r.ok = true;
                    }
                }
                pf->Release();
            }
            link->Release();
        }
        if (!r.ok) r.error = L"Could not resolve the shortcut target.";
        return r;
    }

    if (e == L".msc") {
        std::wstring mmc = SystemDir() + L"\\mmc.exe";
        std::wstring a = QuoteArg(target);
        if (!req.args.empty()) a += L" " + req.args;
        r.app = mmc;
        r.cmdline = BuildCommandLine(mmc, a);
        r.workDir = req.workingDir.empty() ? DirOf(target) : req.workingDir;
        r.ok = true;
        return r;
    }

    if (e == L".exe" || e == L".com" || e == L".bat" || e == L".cmd" || e == L".scr" || e.empty()) {
        r.app = target;
        r.cmdline = BuildCommandLine(target, req.args);
        r.workDir = req.workingDir.empty() ? DirOf(target) : req.workingDir;
        r.ok = (e == L".bat" || e == L".cmd") ? true : true;
        // .bat/.cmd: CreateProcess* needs an interpreter; wrap in cmd.exe.
        if (e == L".bat" || e == L".cmd") {
            std::wstring cmd = SystemDir() + L"\\cmd.exe";
            std::wstring a = L"/c " + QuoteArg(target);
            if (!req.args.empty()) a += L" " + req.args;
            r.app = cmd;
            r.cmdline = BuildCommandLine(cmd, a);
        }
        return r;
    }

    // Any other document: ask the association for its command, then substitute %1/%*.
    wchar_t cmd[2048];
    DWORD cch = 2048;
    HRESULT hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_COMMAND, e.c_str(), nullptr, cmd, &cch);
    if (SUCCEEDED(hr) && cmd[0]) {
        r.app.clear();
        r.cmdline = ExpandAssoc(cmd, target, req.args);
        r.workDir = req.workingDir.empty() ? DirOf(target) : req.workingDir;
        r.ok = true;
        return r;
    }
    r.error = L"No program is associated with " + e + L" for a token launch; use the Admin (full) "
              L"identity, which opens documents through the shell.";
    return r;
}

// Launch with an explicit primary token (CreateProcessWithTokenW), falling back to
// CreateProcessAsUserW while impersonating. hToken must be a primary token. Fills out on success.
bool LaunchWithToken(HANDLE hToken, const Resolved& res, LaunchResult* out) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    std::wstring cmd = res.cmdline;
    const wchar_t* app = res.app.empty() ? nullptr : res.app.c_str();
    const wchar_t* wd = res.workDir.empty() ? nullptr : res.workDir.c_str();

    void* env = nullptr;
    CreateEnvironmentBlock(&env, hToken, FALSE);
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;

    BOOL ok = CreateProcessWithTokenW(hToken, 0, app, cmd.empty() ? nullptr : &cmd[0],
                                      flags, env, wd, &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (!ok) {
        // Fallback: impersonate and CreateProcessAsUserW (needs SeAssignPrimaryTokenPrivilege).
        if (SetThreadToken(nullptr, hToken)) {
            ok = CreateProcessAsUserW(hToken, app, cmd.empty() ? nullptr : &cmd[0], nullptr, nullptr,
                                      FALSE, flags, env, wd, &si, &pi);
            if (!ok) err = GetLastError();
            SetThreadToken(nullptr, nullptr);
        }
    }
    if (env) DestroyEnvironmentBlock(env);

    if (!ok) {
        out->error = sys::ErrorText(err);
        return false;
    }
    ReadBack(pi.hProcess, out);
    out->ok = true;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Find a SYSTEM-token source process (winlogon.exe) in our own session. Returns its PID or 0.
DWORD FindWinlogonPid() {
    DWORD ourSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &ourSession);
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"winlogon.exe") != 0) continue;
            DWORD sess = 0;
            if (ProcessIdToSessionId(pe.th32ProcessID, &sess) && sess == ourSession) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Duplicate a process's token as a primary token. Caller closes the returned handle.
HANDLE DupPrimaryToken(DWORD pid, DWORD* err) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) { if (err) *err = GetLastError(); return nullptr; }
    HANDLE src = nullptr;
    if (!OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &src)) {
        if (err) *err = GetLastError();
        CloseHandle(proc);
        return nullptr;
    }
    HANDLE dup = nullptr;
    if (!DuplicateTokenEx(src, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &dup)) {
        if (err) *err = GetLastError();
        dup = nullptr;
    }
    CloseHandle(src);
    CloseHandle(proc);
    return dup;
}

void FailPrivilege(LaunchResult* out, const wchar_t* priv, DWORD err) {
    out->ok = false;
    out->error = std::wstring(L"Could not enable ") + priv + L" (" + sys::ErrorText(err) +
                 L"). This identity needs it; aborting.";
}

// ---- the identity paths ----------------------------------------------------------------------

void LaunchAdminFull(const LaunchRequest& req, LaunchResult* out) {
    std::wstring target = AbsolutePath(req.path);
    std::wstring wd = req.workingDir.empty() ? DirOf(target) : req.workingDir;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = nullptr;   // default verb: open/run, handles docs, .lnk and .msc natively
    sei.lpFile = target.c_str();
    sei.lpParameters = req.args.empty() ? nullptr : req.args.c_str();
    sei.lpDirectory = wd.empty() ? nullptr : wd.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        out->error = sys::ErrorText(GetLastError());
        return;
    }
    if (sei.hProcess) {
        ReadBack(sei.hProcess, out);
        CloseHandle(sei.hProcess);
    } else {
        // Some verbs (e.g. a document handed to a running app) return no process handle.
        out->effectiveUser = TokenUserName(GetCurrentProcessToken());
    }
    out->ok = true;
}

void LaunchAdminLimited(const LaunchRequest& req, LaunchResult* out) {
    Resolved res = ResolveTarget(req);
    if (!res.ok) { out->error = res.error; return; }

    // The shell runs at medium IL; its token is the reliable de-elevated token.
    HANDLE shellTok = nullptr;
    HWND shell = GetShellWindow();
    if (shell) {
        DWORD spid = 0;
        GetWindowThreadProcessId(shell, &spid);
        if (spid) {
            DWORD err = 0;
            shellTok = DupPrimaryToken(spid, &err);
        }
    }
    if (!shellTok) {
        // Fallback: our own UAC-linked (medium) token.
        HANDLE me = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &me)) {
            TOKEN_LINKED_TOKEN linked{};
            DWORD got = 0;
            if (GetTokenInformation(me, TokenLinkedToken, &linked, sizeof(linked), &got) &&
                linked.LinkedToken) {
                DuplicateTokenEx(linked.LinkedToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                                 TokenPrimary, &shellTok);
                CloseHandle(linked.LinkedToken);
            }
            CloseHandle(me);
        }
    }
    if (!shellTok) {
        out->error = L"Could not obtain a de-elevated (medium) token from the shell.";
        return;
    }
    LaunchWithToken(shellTok, res, out);
    CloseHandle(shellTok);
}

void LaunchAnotherUser(const LaunchRequest& req, const wchar_t* password, LaunchResult* out) {
    Resolved res = ResolveTarget(req);
    if (!res.ok) { out->error = res.error; return; }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = res.cmdline;
    const wchar_t* app = res.app.empty() ? nullptr : res.app.c_str();
    const wchar_t* wd = res.workDir.empty() ? nullptr : res.workDir.c_str();
    const wchar_t* pw = password ? password : L"";   // empty password => empty string

    BOOL ok = CreateProcessWithLogonW(req.account.c_str(), L".", pw, LOGON_WITH_PROFILE, app,
                                      cmd.empty() ? nullptr : &cmd[0], CREATE_UNICODE_ENVIRONMENT,
                                      nullptr, wd, &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCOUNT_RESTRICTION) {
            out->error = L"The account's policy blocks this logon (for example, a blank-password "
                         L"account not allowed to log on). " + sys::ErrorText(err);
        } else {
            out->error = sys::ErrorText(err);
        }
        return;
    }
    ReadBack(pi.hProcess, out);
    out->ok = true;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

void LaunchSystem(const LaunchRequest& req, LaunchResult* out) {
    Resolved res = ResolveTarget(req);
    if (!res.ok) { out->error = res.error; return; }

    ThreadPrivilege dbg(L"SeDebugPrivilege");   // thread token only; reverted on scope exit
    if (!dbg.ok()) { FailPrivilege(out, L"SeDebugPrivilege", dbg.err()); return; }

    DWORD pid = FindWinlogonPid();
    if (!pid) { out->error = L"Could not find winlogon.exe in this session."; return; }

    DWORD err = 0;
    HANDLE tok = DupPrimaryToken(pid, &err);
    if (!tok) { out->error = L"Could not duplicate the SYSTEM token: " + sys::ErrorText(err); return; }

    if (req.mode == TokenMode::Limited) {
        HANDLE restricted = nullptr;
        if (CreateRestrictedToken(tok, LUA_TOKEN, 0, nullptr, 0, nullptr, 0, nullptr, &restricted) &&
            restricted) {
            CloseHandle(tok);
            tok = restricted;
            SetTokenIntegrityMedium(tok, nullptr);
        }
    }
    LaunchWithToken(tok, res, out);
    CloseHandle(tok);
}

void LaunchTrustedInstaller(const LaunchRequest& req, LaunchResult* out) {
    Resolved res = ResolveTarget(req);
    if (!res.ok) { out->error = res.error; return; }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { out->error = L"OpenSCManager failed: " + sys::ErrorText(GetLastError()); return; }
    SC_HANDLE svc = OpenServiceW(scm, L"TrustedInstaller",
                                 SERVICE_QUERY_STATUS | SERVICE_START);
    if (!svc) {
        out->error = L"Could not open the TrustedInstaller service: " + sys::ErrorText(GetLastError());
        CloseServiceHandle(scm);
        return;
    }
    SERVICE_STATUS_PROCESS ssp{};
    DWORD need = 0;
    auto query = [&]() {
        return QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&ssp),
                                    sizeof(ssp), &need) != FALSE;
    };
    if (!query()) {
        out->error = L"QueryServiceStatusEx failed: " + sys::ErrorText(GetLastError());
        CloseServiceHandle(svc); CloseServiceHandle(scm);
        return;
    }
    if (ssp.dwCurrentState == SERVICE_STOPPED) {
        if (!StartServiceW(svc, 0, nullptr)) {
            out->error = L"Could not start TrustedInstaller: " + sys::ErrorText(GetLastError());
            CloseServiceHandle(svc); CloseServiceHandle(scm);
            return;
        }
        out->startedService = true;
    }
    for (int i = 0; i < 100 && ssp.dwCurrentState != SERVICE_RUNNING; i++) {
        Sleep(100);
        if (!query()) break;
    }
    DWORD tiPid = ssp.dwProcessId;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (ssp.dwCurrentState != SERVICE_RUNNING || !tiPid) {
        out->error = L"TrustedInstaller did not reach the running state.";
        return;
    }

    ThreadPrivilege dbg(L"SeDebugPrivilege");
    if (!dbg.ok()) { FailPrivilege(out, L"SeDebugPrivilege", dbg.err()); return; }

    DWORD err = 0;
    HANDLE tok = DupPrimaryToken(tiPid, &err);
    if (!tok) {
        out->error = L"Could not duplicate the TrustedInstaller token: " + sys::ErrorText(err);
        return;
    }

    // The TI token is in session 0. Moving it to our interactive session needs SeTcbPrivilege,
    // which winlogon's SYSTEM token holds; impersonate it to make SetTokenInformation succeed.
    DWORD ourSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &ourSession);
    DWORD winPid = FindWinlogonPid();
    HANDLE winTok = nullptr;
    if (winPid) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, winPid);
        if (proc) {
            HANDLE src = nullptr;
            if (OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY, &src)) {
                DuplicateTokenEx(src, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                                 TokenImpersonation, &winTok);
                CloseHandle(src);
            }
            CloseHandle(proc);
        }
    }
    bool impersonated = winTok && SetThreadToken(nullptr, winTok);
    SetTokenInformation(tok, TokenSessionId, &ourSession, sizeof(ourSession));
    if (impersonated) SetThreadToken(nullptr, nullptr);
    if (winTok) CloseHandle(winTok);

    if (req.mode == TokenMode::Limited) {
        HANDLE restricted = nullptr;
        if (CreateRestrictedToken(tok, LUA_TOKEN, 0, nullptr, 0, nullptr, 0, nullptr, &restricted) &&
            restricted) {
            CloseHandle(tok);
            tok = restricted;
            SetTokenIntegrityMedium(tok, nullptr);
        }
    }
    LaunchWithToken(tok, res, out);
    CloseHandle(tok);
}

}  // namespace

// ================================ public API ==================================================

const wchar_t* WhoName(Who w) {
    switch (w) {
        case Who::ThisAdmin:        return L"This admin";
        case Who::AnotherUser:      return L"Another user";
        case Who::System:           return L"SYSTEM";
        case Who::TrustedInstaller: return L"TrustedInstaller";
    }
    return L"";
}

std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0; ; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { backslashes++; i++; }
        if (i == arg.size()) { out.append(backslashes * 2, L'\\'); break; }
        if (arg[i] == L'"') { out.append(backslashes * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(backslashes, L'\\'); out += arg[i]; }
    }
    out += L'"';
    return out;
}

std::wstring BuildCommandLine(const std::wstring& exe, const std::wstring& args) {
    std::wstring cl = QuoteArg(exe);
    if (!args.empty()) cl += L" " + args;
    return cl;
}

std::wstring AbsolutePath(const std::wstring& path, const std::wstring& base) {
    if (path.empty()) return path;
    std::wstring candidate = path;
    if (!base.empty() && !IsAbsolute(path)) candidate = base + L"\\" + path;
    wchar_t buf[1024];
    wchar_t* filePart = nullptr;
    DWORD n = GetFullPathNameW(candidate.c_str(), 1024, buf, &filePart);
    if (n == 0) return candidate;
    if (n < 1024) return std::wstring(buf, n);
    std::vector<wchar_t> big(n);
    DWORD m = GetFullPathNameW(candidate.c_str(), n, big.data(), &filePart);
    return (m && m < n) ? std::wstring(big.data(), m) : candidate;
}

std::wstring IntegrityName(DWORD rid) {
    switch (rid) {
        case 0x0000: return L"Untrusted";
        case 0x1000: return L"Low";
        case 0x2000: return L"Medium";
        case 0x2100: return L"Medium Plus";
        case 0x3000: return L"High";
        case 0x4000: return L"System";
        case 0x5000: return L"Protected";
        default: {
            wchar_t b[16];
            swprintf(b, 16, L"0x%04X", rid);
            return b;
        }
    }
}

std::wstring MscCommand(const std::wstring& system32, const std::wstring& mscFile) {
    return QuoteArg(system32 + L"\\mmc.exe") + L" " + QuoteArg(mscFile);
}

bool PrivilegeEnabled(BOOL adjustReturn, DWORD lastError) {
    return adjustReturn != FALSE && lastError != ERROR_NOT_ALL_ASSIGNED;
}

std::vector<Account> FilterAccounts(const std::vector<Account>& all) {
    std::vector<Account> out;
    for (const Account& a : all) {
        if (a.flags & UF_ACCOUNTDISABLE) continue;
        if (!(a.flags & UF_NORMAL_ACCOUNT)) continue;   // skip trust / temp-duplicate accounts
        if (IsServiceAccount(a.name)) continue;
        out.push_back(a);
    }
    return out;
}

std::vector<Account> EnumerateLocalAccounts() {
    std::vector<Account> all;
    DWORD resume = 0;
    for (;;) {
        LPUSER_INFO_1 buf = nullptr;
        DWORD read = 0, total = 0;
        NET_API_STATUS st = NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT,
                                        reinterpret_cast<LPBYTE*>(&buf), MAX_PREFERRED_LENGTH,
                                        &read, &total, &resume);
        if ((st == NERR_Success || st == ERROR_MORE_DATA) && buf) {
            for (DWORD i = 0; i < read; i++) {
                Account a;
                a.name = buf[i].usri1_name ? buf[i].usri1_name : L"";
                a.flags = buf[i].usri1_flags;
                all.push_back(a);
            }
            NetApiBufferFree(buf);
            if (st == ERROR_MORE_DATA) continue;
        }
        break;
    }
    return FilterAccounts(all);
}

// ---- last-run serialisation ------------------------------------------------------------------

namespace {
constexpr wchar_t kSep = L'\x1f';   // unit separator between fields; escaped if it appears in data

std::wstring Esc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'\\': o += L"\\\\"; break;
            case kSep:  o += L"\\s";  break;
            case L'\n': o += L"\\n";  break;
            case L'\r': o += L"\\r";  break;
            default:    o += c;       break;
        }
    }
    return o;
}
std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            if (n == L'\\') o += L'\\';
            else if (n == L's') o += kSep;
            else if (n == L'n') o += L'\n';
            else if (n == L'r') o += L'\r';
            else o += n;
        } else {
            o += s[i];
        }
    }
    return o;
}
}  // namespace

std::wstring SerializeLastRun(const LastRun& r) {
    std::wstring o;
    o += Esc(r.path);        o += kSep;
    o += Esc(r.args);        o += kSep;
    o += Esc(r.account);     o += kSep;
    o += std::to_wstring(r.who);  o += kSep;
    o += std::to_wstring(r.mode); o += kSep;
    o += Esc(r.integrity);   o += kSep;
    o += std::to_wstring(r.when);
    return o;
}

bool ParseLastRun(const std::wstring& line, LastRun* out) {
    if (line.empty() || !out) return false;
    std::vector<std::wstring> f;
    std::wstring cur;
    for (wchar_t c : line) {
        if (c == kSep) { f.push_back(cur); cur.clear(); }
        else cur += c;
    }
    f.push_back(cur);
    if (f.empty() || f[0].empty()) return false;
    *out = LastRun{};
    out->path = Unesc(f[0]);
    if (f.size() > 1) out->args = Unesc(f[1]);
    if (f.size() > 2) out->account = Unesc(f[2]);
    if (f.size() > 3) out->who = _wtoi(f[3].c_str());
    if (f.size() > 4) out->mode = _wtoi(f[4].c_str());
    if (f.size() > 5) out->integrity = Unesc(f[5]);
    if (f.size() > 6) out->when = _wtoi64(f[6].c_str());
    return true;
}

std::wstring IdentityLabel(int who, int mode, const std::wstring& account) {
    std::wstring base = (who == (int)Who::AnotherUser && !account.empty())
                            ? account
                            : WhoName((Who)who);
    return base + (mode == (int)TokenMode::Limited ? L" (Limited)" : L" (Full)");
}

// ---- dispatch --------------------------------------------------------------------------------

void Launch(const LaunchRequest& req, const wchar_t* password, LaunchResult* out) {
    *out = LaunchResult{};
    out->requestedLabel = IdentityLabel((int)req.who, (int)req.mode, req.account);
    if (req.path.empty()) { out->error = L"No target chosen."; return; }

    if (req.who == Who::ThisAdmin && req.mode == TokenMode::Full)      LaunchAdminFull(req, out);
    else if (req.who == Who::ThisAdmin)                               LaunchAdminLimited(req, out);
    else if (req.who == Who::AnotherUser)                             LaunchAnotherUser(req, password, out);
    else if (req.who == Who::System)                                 LaunchSystem(req, out);
    else                                                             LaunchTrustedInstaller(req, out);

    if (out->ok && out->integrityRid && out->integrityRid < ExpectedRid(req))
        out->downgraded = true;
}

bool RunningUnderWine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

int RunLaunchTest(const std::wstring& mode) {
    if (RunningUnderWine()) {
        wprintf(L"launch-test %ls: SKIP (Wine)\n", mode.c_str());
        return 0;
    }
    if (mode != L"limited") {
        wprintf(L"launch-test: unknown mode '%ls'\n", mode.c_str());
        return 2;
    }
    LaunchRequest req;
    req.path = SystemDir() + L"\\cmd.exe";
    req.args = L"/c exit 0";
    req.who = Who::ThisAdmin;
    req.mode = TokenMode::Limited;
    LaunchResult r;
    Launch(req, nullptr, &r);
    if (!r.ok) {
        wprintf(L"launch-test limited: FAIL (%ls)\n", r.error.c_str());
        return 1;
    }
    wprintf(L"launch-test limited: pid=%lu session=%lu user=%ls integrity=%ls\n",
            (unsigned long)r.pid, (unsigned long)r.sessionId, r.effectiveUser.c_str(),
            r.integrityName.c_str());
    bool medium = (r.integrityRid == 0x2000);
    wprintf(L"launch-test limited: %ls\n", medium ? L"PASS (Medium)" : L"FAIL (not Medium)");
    return medium ? 0 : 1;
}

}  // namespace hx::runas
