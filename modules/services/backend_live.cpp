// modules/services/backend_live.cpp — the live backend: the ONLY file in Hadron that calls SCM write APIs.
//
// Reads open the SCM with SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE and each service with
// SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS only (spec §5.1); the registry is opened KEY_QUERY_VALUE.
// Writes (ChangeServiceConfigW for the start type, ChangeServiceConfig2W for the delayed flag, and
// StartServiceW / ControlServiceExW for live control) refuse until Arm() was called. Only the UI's
// confirmed-plan path and --live-test arm it. Hadron never creates, deletes or re-registers a service,
// never changes an account, a failure action or a trigger, and never edits a key's DACL (CI guards this).
// The registry fallback (WriteRegStart, regfallback.h) is the one registry write: Start and DelayedAutostart of an
// existing service key, when the SCM refused the change and the user turned the per-apply toggle on; it refuses
// until Arm() too, and never creates a key or a value of another type.
#include "modules/services/backend.h"
#include "modules/services/svc_compat.h"
#include "core/sys/sys.h"
#include <algorithm>
#include <set>

namespace hx::services {

namespace {

const std::wstring kNotArmed = L"refused: the live backend is not armed (no confirmed plan)";

std::wstring GuidText(const GUID& g) {
    wchar_t b[64];
    swprintf(b, 64, L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

struct Sc {   // RAII service handle
    SC_HANDLE h = nullptr;
    explicit Sc(SC_HANDLE x) : h(x) {}
    ~Sc() { if (h) CloseServiceHandle(h); }
    Sc(const Sc&) = delete;
    Sc& operator=(const Sc&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

bool Config2(SC_HANDLE s, DWORD level, std::vector<BYTE>* buf) {
    DWORD need = 0;
    buf->assign(256, 0);
    for (int tries = 0; tries < 4; tries++) {   // the size can grow between calls (r5 R8)
        if (QueryServiceConfig2W(s, level, buf->data(), (DWORD)buf->size(), &need)) return true;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
        buf->assign(need + 64, 0);
    }
    return false;
}

int RegDword(const std::wstring& svc, const wchar_t* value) {
    std::wstring path = L"SYSTEM\\CurrentControlSet\\Services\\" + svc;
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), value, RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS)
        return -1;
    return (int)v;
}

std::wstring RegString(HKEY root, const std::wstring& path, const wchar_t* value, DWORD flags = 0) {
    DWORD cb = 0;
    if (RegGetValueW(root, path.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | flags, nullptr,
                     nullptr, &cb) != ERROR_SUCCESS || cb == 0)
        return {};
    std::vector<wchar_t> b(cb / sizeof(wchar_t) + 2, 0);
    if (RegGetValueW(root, path.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | flags, nullptr,
                     b.data(), &cb) != ERROR_SUCCESS)
        return {};
    return b.data();
}

std::wstring FailureText(const SERVICE_FAILURE_ACTIONSW* f) {
    if (!f || f->cActions == 0) return {};
    std::vector<std::wstring> parts;
    for (DWORD i = 0; i < f->cActions && i < 8; i++) {
        switch (f->lpsaActions[i].Type) {
            case SC_ACTION_NONE: parts.push_back(L"none"); break;
            case SC_ACTION_RESTART: parts.push_back(L"restart the service"); break;
            case SC_ACTION_REBOOT: parts.push_back(L"restart the computer"); break;
            case SC_ACTION_RUN_COMMAND: parts.push_back(L"run a command"); break;
            default: parts.push_back(L"action " + std::to_wstring(f->lpsaActions[i].Type)); break;
        }
    }
    return L"on failure: " + Join(parts, L", then ");
}

std::wstring TriggerTypeName(DWORD t) {
    switch (t) {
        case 1: return L"device arrival";
        case 2: return L"IP address";
        case 3: return L"domain join";
        case 4: return L"firewall port";
        case 5: return L"group policy";
        case 6: return L"network endpoint";
        case 20: return L"custom event";
    }
    return L"type " + std::to_wstring(t);
}

// Read one service's full configuration with query rights only. status may be null (then queried).
bool ReadOne(SC_HANDLE scm, const std::wstring& name, const SERVICE_STATUS_PROCESS* status, SvcItem* out, std::wstring* err) {
    Sc s(OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
    if (!s) { *err = sys::ErrorText(GetLastError()); return false; }
    SvcItem i;
    i.name = name;
    std::vector<BYTE> buf(1024);
    DWORD need = 0;
    bool got = false;
    for (int tries = 0; tries < 4 && !got; tries++) {
        if (QueryServiceConfigW(s.h, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data()), (DWORD)buf.size(), &need)) got = true;
        else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) buf.assign(need + 64, 0);
        else break;
    }
    if (!got) { *err = L"QueryServiceConfig: " + sys::ErrorText(GetLastError()); return false; }
    auto* qc = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    i.type = qc->dwServiceType;
    i.cfg.start = (int)qc->dwStartType;
    i.errctl = (int)qc->dwErrorControl;
    if (qc->lpBinaryPathName) i.image = qc->lpBinaryPathName;
    if (qc->lpLoadOrderGroup) i.group = qc->lpLoadOrderGroup;
    if (qc->lpServiceStartName) i.account = qc->lpServiceStartName;
    if (qc->lpDisplayName) i.display = qc->lpDisplayName;
    for (const wchar_t* d = qc->lpDependencies; d && *d; d += wcslen(d) + 1) i.deps.push_back(d);

    std::vector<BYTE> b2;
    if (Config2(s.h, SERVICE_CONFIG_DESCRIPTION, &b2)) {
        auto* d = reinterpret_cast<SERVICE_DESCRIPTIONW*>(b2.data());
        if (d->lpDescription) i.desc = d->lpDescription;
    }
    int regDelayed = RegDword(name, L"DelayedAutostart");
    i.cfg.delayed = regDelayed < 0 ? -1 : (regDelayed ? 1 : 0);
    if (regDelayed < 0 && Config2(s.h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &b2) &&
        reinterpret_cast<SERVICE_DELAYED_AUTO_START_INFO*>(b2.data())->fDelayedAutostart)
        i.cfg.delayed = 1;   // the SCM says delayed but the registry value could not be read
    if (Config2(s.h, SERVICE_CONFIG_TRIGGER_INFO, &b2)) {
        auto* t = reinterpret_cast<HxTriggerInfo*>(b2.data());
        std::vector<std::wstring> parts;
        for (DWORD k = 0; k < t->cTriggers && t->pTriggers; k++) {
            const HxTrigger& tr = t->pTriggers[k];
            std::wstring p = (tr.dwAction == HXSVC_TRIGGER_ACTION_STOP ? L"stop on " : L"start on ") +
                             TriggerTypeName(tr.dwTriggerType);
            if (tr.dwTriggerType == SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL && tr.pTriggerSubtype) {
                std::wstring g = Lower(GuidText(*tr.pTriggerSubtype));
                p += L" " + g;
                i.ifaces.push_back(g);
            }
            parts.push_back(p);
        }
        i.trigger = t->cTriggers > 0;
        i.triggers = Join(parts, L"; ");
    }
    if (Config2(s.h, SERVICE_CONFIG_FAILURE_ACTIONS, &b2))
        i.failure = FailureText(reinterpret_cast<SERVICE_FAILURE_ACTIONSW*>(b2.data()));
    if (Config2(s.h, SERVICE_CONFIG_LAUNCH_PROTECTED, &b2))
        i.protect = (int)reinterpret_cast<HxLaunchProtectedInfo*>(b2.data())->dwLaunchProtected;
    i.regStart = RegDword(name, L"Start");

    SERVICE_STATUS_PROCESS ssp{};
    if (status) ssp = *status;
    else if (!QueryServiceStatusEx(s.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&ssp), sizeof(ssp), &need))
        i.readError = L"status: " + sys::ErrorText(GetLastError());
    i.state = ssp.dwCurrentState ? ssp.dwCurrentState : SERVICE_STOPPED;
    i.controls = ssp.dwControlsAccepted;
    i.pid = ssp.dwProcessId;
    if (i.pid) {   // the process's creation time: with the pid it names one process instance (TrustedInstaller claim, R4)
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, i.pid);
        FILETIME c{}, x{}, k{}, u{};
        if (ph && GetProcessTimes(ph, &c, &x, &k, &u)) i.pstart = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
        if (ph) CloseHandle(ph);   // a process Hadron may not open (protected) keeps 0: never claimed
    }
    i.kind = KindOf(i.type);
    *out = std::move(i);
    return true;
}

bool WaitState(SC_HANDLE s, DWORD want, const std::atomic<bool>* cancel, DWORD capMs, std::wstring* err) {
    SERVICE_STATUS_PROCESS ssp{};
    DWORD need = 0;
    DWORD start = GetTickCount();
    bool moved = false;   // left the state it started in (a pending state) at least once
    for (;;) {
        if (!QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&ssp), sizeof(ssp), &need)) {
            *err = L"QueryServiceStatusEx: " + sys::ErrorText(GetLastError());
            return false;
        }
        if (ssp.dwCurrentState == want) return true;
        // A start that fell back to STOPPED (or a stop that came back RUNNING) will not reach `want`: report why
        // now instead of waiting out the cap.
        DWORD back = want == SERVICE_RUNNING ? SERVICE_STOPPED : SERVICE_RUNNING;
        if (ssp.dwCurrentState != back) moved = true;
        if (ssp.dwCurrentState == back && (moved || (want == SERVICE_RUNNING && GetTickCount() - start > 1000))) {
            DWORD code = ssp.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR ? ssp.dwServiceSpecificExitCode : ssp.dwWin32ExitCode;
            *err = std::wstring(want == SERVICE_RUNNING ? L"the service stopped again while starting" : L"the service is running again after the stop") +
                   (code ? L": " + sys::ErrorText(code) : L"");
            return false;
        }
        if (cancel && cancel->load()) { *err = L"cancelled while waiting; last state " + StateName(ssp.dwCurrentState); return false; }
        if (GetTickCount() - start > capMs) {   // overall cap (r1 §4; shorter for cleanup requests, A5)
            *err = L"timed out waiting; last state " + StateName(ssp.dwCurrentState);
            return false;
        }
        DWORD wait = ssp.dwWaitHint / 10;
        Sleep(std::min<DWORD>(std::max<DWORD>(wait, 250), 1000));
    }
}

class LiveBackend : public Backend {
public:
    bool IsLive() const override { return true; }
    void Arm() override { armed_ = true; }
    void Disarm() override { armed_ = false; }

    bool Enumerate(std::vector<SvcItem>* out, std::wstring* err) override {
        out->clear();
        Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
        if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
        // Ask for every type bit (templates are 0x50/0x60) and classify ourselves; fall back to the older
        // masks if this Windows (or Wine) rejects the full one.
        const DWORD types[] = {HXSVC_TYPE_ALL, SERVICE_TYPE_ALL, SERVICE_WIN32 | SERVICE_DRIVER};
        std::vector<std::pair<std::wstring, SERVICE_STATUS_PROCESS>> list;
        DWORD lastErr = 0;
        lastEnum = EnumStats{};
        bool accepted = false;
        for (DWORD type : types) {
            list.clear();
            lastEnum.raw = lastEnum.rawDrivers = lastEnum.pages = lastEnum.duplicates = 0;
            DWORD resume = 0, need = 0, count = 0;
            std::vector<BYTE> buf(64 * 1024);
            bool ok = true;
            std::set<std::wstring> seen;
            for (int guard = 0; guard < 1000; guard++) {
                count = 0;
                BOOL r = EnumServicesStatusExW(scm.h, SC_ENUM_PROCESS_INFO, type, SERVICE_STATE_ALL, buf.data(),
                                               (DWORD)buf.size(), &need, &count, &resume, nullptr);
                DWORD e = r ? 0 : GetLastError();
                auto* arr = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
                if (count) lastEnum.pages++;
                for (DWORD k = 0; k < count; k++) {
                    if (!arr[k].lpServiceName) continue;
                    lastEnum.raw++;
                    if (IsDriverType(arr[k].ServiceStatusProcess.dwServiceType)) lastEnum.rawDrivers++;
                    // A page boundary must never list a name twice (the resume handle is the SCM's; Hadron keeps the first).
                    if (!seen.insert(Lower(arr[k].lpServiceName)).second) { lastEnum.duplicates++; continue; }
                    list.push_back({arr[k].lpServiceName, arr[k].ServiceStatusProcess});
                }
                if (r) break;
                if (e != ERROR_MORE_DATA) { ok = false; lastErr = e; break; }
                // No entry fitted and the SCM asks for no more than this buffer: grow anyway, so the loop always moves.
                if (count == 0 && need <= buf.size() && buf.size() >= 256 * 1024) { ok = false; lastErr = ERROR_MORE_DATA; break; }
                if (need > buf.size() || count == 0) buf.assign(std::min<size_t>(std::max<size_t>(need + 1024, buf.size() * 2), 256 * 1024), 0);
            }
            wchar_t m[16];
            swprintf(m, 16, L"0x%X", (unsigned)type);
            lastEnum.masksTried += (lastEnum.masksTried.empty() ? L"" : L"; ") + std::wstring(m) +
                                   (ok ? L" ok" : L" failed (" + sys::ErrorText(lastErr) + L")");
            if (ok) { lastEnum.mask = type; accepted = true; break; }
            if (lastErr != ERROR_INVALID_PARAMETER) { *err = L"EnumServicesStatusEx: " + sys::ErrorText(lastErr); return false; }
        }
        if (!accepted) { *err = L"EnumServicesStatusEx: " + sys::ErrorText(lastErr); return false; }
        for (auto& e : list) {
            SvcItem i;
            std::wstring rerr;
            if (!ReadOne(scm.h, e.first, &e.second, &i, &rerr)) {
                i = SvcItem{};
                i.name = e.first;
                i.state = e.second.dwCurrentState;
                i.type = e.second.dwServiceType;
                i.kind = KindOf(i.type);
                i.readError = std::wstring(kCfgUnreadable) + L": " + rerr;
                i.cfgUnknown = true;   // cfg is the default, not this service's: never planned or recorded
                lastEnum.unreadable++;
            }
            out->push_back(std::move(i));
        }
        return true;
    }

    bool Query(const std::wstring& name, SvcItem* out, std::wstring* err) override {
        Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
        return ReadOne(scm.h, name, nullptr, out, err);
    }

    bool Dependents(const std::wstring& name, std::vector<std::wstring>* active, std::wstring* err) override {
        active->clear();
        Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
        Sc s(OpenServiceW(scm.h, name.c_str(), SERVICE_ENUMERATE_DEPENDENTS));
        if (!s) { *err = sys::ErrorText(GetLastError()); return false; }
        DWORD need = 0, count = 0;
        std::vector<BYTE> buf(4096);
        for (int tries = 0; tries < 4; tries++) {
            if (EnumDependentServicesW(s.h, SERVICE_ACTIVE, reinterpret_cast<ENUM_SERVICE_STATUSW*>(buf.data()),
                                       (DWORD)buf.size(), &need, &count)) {
                auto* arr = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buf.data());
                // Returned in reverse start order: stopping in array order stops dependents first.
                for (DWORD k = 0; k < count; k++) active->push_back(arr[k].lpServiceName);
                return true;
            }
            if (GetLastError() != ERROR_MORE_DATA) break;
            buf.assign(need + 256, 0);
        }
        *err = L"EnumDependentServices: " + sys::ErrorText(GetLastError());
        return false;
    }

    bool CanWrite(const std::wstring& name, bool* denied, std::wstring* err) override {
        // An access check only: the handle is opened for SERVICE_CHANGE_CONFIG and closed; nothing changes.
        *denied = false;
        Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
        Sc s(OpenServiceW(scm.h, name.c_str(), SERVICE_CHANGE_CONFIG));
        if (!s) {
            DWORD e = GetLastError();
            *denied = e == ERROR_ACCESS_DENIED;
            *err = sys::ErrorText(e);
            return false;
        }
        return true;
    }

    bool SetStart(const std::wstring& name, const WriteOp& w, Identity id, std::wstring* err) override {
        (void)id;   // the apply engine has already set the thread token for TrustedInstaller rows
        if (!armed_) { *err = kNotArmed; return false; }
        if (w.start < 0 || w.start > 4) { *err = L"invalid start type"; return false; }
        Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
        Sc s(OpenServiceW(scm.h, name.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG));
        if (!s) { *err = L"OpenService: " + sys::ErrorText(GetLastError()); return false; }
        // The delayed flag first: it is ignored unless the start type is Auto, so writing it before Start=2 is
        // harmless, and when the SCM refuses it (a service in a load-order group can't be delayed) nothing has
        // changed yet instead of leaving plain Auto behind.
        if (w.delayed >= 0) {
            SERVICE_DELAYED_AUTO_START_INFO d{w.delayed ? TRUE : FALSE};
            if (!ChangeServiceConfig2W(s.h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &d)) {
                *err = L"the delayed flag was refused, so nothing was changed: ChangeServiceConfig2: " + sys::ErrorText(GetLastError());
                return false;
            }
        }
        // Start type only: every other field is SERVICE_NO_CHANGE / null (account, path, group, deps untouched).
        if (!ChangeServiceConfigW(s.h, SERVICE_NO_CHANGE, (DWORD)w.start, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr)) {
            *err = L"ChangeServiceConfig: " + sys::ErrorText(GetLastError()) +
                   (w.delayed >= 0 ? L" (the delayed flag was already written; Undo puts it back)" : L"");
            return false;
        }
        return true;
    }

    bool Control(const std::wstring& name, Op op, const std::atomic<bool>* cancel, std::wstring* err, bool* ownStart,
                 DWORD waitMs) override {
        if (ownStart) *ownStart = false;
        // The TrustedInstaller read pass may start and stop TrustedInstaller itself, nothing else (AllowTiControl).
        if (!armed_ && !(tiCtl_.load() && name == L"TrustedInstaller")) { *err = kNotArmed; return false; }
        Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
        Sc s(OpenServiceW(scm.h, name.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
        if (!s) { *err = L"OpenService: " + sys::ErrorText(GetLastError()); return false; }
        SvcItem cur;
        if (!ReadOne(scm.h, name, nullptr, &cur, err)) return false;
        if (IsDriverType(cur.type)) { *err = L"Drivers are never loaded or unloaded live."; return false; }
        if (op == Op::Start) {
            if (StartServiceW(s.h, 0, nullptr)) {
                if (ownStart) *ownStart = true;   // this request started it
            } else if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                *err = L"StartService: " + sys::ErrorText(GetLastError());
                return false;
            }
            return WaitState(s.h, SERVICE_RUNNING, cancel, waitMs, err);
        }
        if (!(cur.controls & SERVICE_ACCEPT_STOP) && cur.state == SERVICE_RUNNING) {
            *err = L"This service does not accept a stop request.";
            return false;
        }
        SERVICE_CONTROL_STATUS_REASON_PARAMSW p{};
        p.dwReason = SERVICE_STOP_REASON_FLAG_PLANNED | SERVICE_STOP_REASON_MAJOR_NONE | SERVICE_STOP_REASON_MINOR_NONE;
        wchar_t comment[] = L"Hadron Services";
        p.pszComment = comment;
        if (!ControlServiceExW(s.h, SERVICE_CONTROL_STOP, SERVICE_CONTROL_STATUS_REASON_INFO, &p)) {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_NOT_ACTIVE) return true;
            if (e == ERROR_INVALID_PARAMETER) {
                // PLANNED|MAJOR_NONE|MINOR_NONE is a documented reason (Learn's page prints MINOR_NONE as 0x00060000, a
                // typo for winsvc.h's 0x17), but no CI step has run this stop on Windows: if it is refused as an
                // invalid reason, stop without one.
                SERVICE_STATUS ss{};
                if (!ControlService(s.h, SERVICE_CONTROL_STOP, &ss)) {
                    e = GetLastError();
                    if (e == ERROR_SERVICE_NOT_ACTIVE) return true;
                    *err = L"ControlService(stop): " + sys::ErrorText(e);
                    return false;
                }
                return WaitState(s.h, SERVICE_STOPPED, cancel, waitMs, err);
            }
            *err = L"ControlServiceEx(stop): " + sys::ErrorText(e);
            return false;
        }
        return WaitState(s.h, SERVICE_STOPPED, cancel, waitMs, err);
    }

    bool ReadHw(HwInfo* out) override { return ReadLiveHw(out); }

    bool ReadRegStart(const std::wstring& name, RegStartVals* out, std::wstring* err) override {
        *out = RegStartVals{};
        if (!KeyNameOk(name)) { *err = L"not a plain service name"; return false; }
        if (KeyPath(name).empty()) { *err = L"HKLM\\SYSTEM\\Select\\Current could not be read"; return false; }
        HKEY k = nullptr;
        LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyPath(name).c_str(), REG_OPTION_OPEN_LINK,
                                  KEY_QUERY_VALUE, &k);
        if (e != ERROR_SUCCESS) { *err = L"RegOpenKeyEx: " + sys::ErrorText((DWORD)e); return false; }
        bool ok = ReadValues(k, out, err);
        RegCloseKey(k);
        return ok;
    }

    bool WriteRegStart(const std::wstring& name, const WriteOp& w, const RegStartVals& expect, std::wstring* err) override {
        if (!armed_) { *err = kNotArmed; return false; }
        if (w.start < 2 || w.start > 4) { *err = L"refused: Hadron never writes Boot or System start through the registry"; return false; }
        if (w.delayed > 1) { *err = L"invalid DelayedAutostart value"; return false; }
        if (!KeyNameOk(name)) { *err = L"refused: not a plain service name"; return false; }
        {   // Launch protection, from the SCM itself (fail closed: unreadable counts as protected).
            Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
            if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
            Sc s(OpenServiceW(scm.h, name.c_str(), SERVICE_QUERY_CONFIG));
            if (!s) { *err = L"OpenService: " + sys::ErrorText(GetLastError()); return false; }
            std::vector<BYTE> b;
            if (!Config2(s.h, SERVICE_CONFIG_LAUNCH_PROTECTED, &b)) {
                *err = L"refused: its launch protection could not be read: " + sys::ErrorText(GetLastError());
                return false;
            }
            if (reinterpret_cast<HxLaunchProtectedInfo*>(b.data())->dwLaunchProtected != 0) {
                *err = L"refused: a launch-protected service is never written through the registry";
                return false;
            }
        }
        // The control set Windows booted from (Select\Current, as restore.cmd reads it), named directly rather than
        // through the CurrentControlSet link. REG_OPTION_OPEN_LINK: a service key that is itself a registry link is
        // opened as itself and refused below (a link is a leaf, as a reparse point is for files). RegOpenKeyEx never
        // creates a key.
        if (KeyPath(name).empty()) { *err = L"refused: HKLM\\SYSTEM\\Select\\Current could not be read"; return false; }
        HKEY k = nullptr;
        LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyPath(name).c_str(), REG_OPTION_OPEN_LINK,
                                  KEY_QUERY_VALUE | KEY_SET_VALUE, &k);
        if (e != ERROR_SUCCESS) { *err = L"RegOpenKeyEx: " + sys::ErrorText((DWORD)e); return false; }
        auto fail = [&](const std::wstring& t) { RegCloseKey(k); *err = t; return false; };
        DWORD lt = 0;
        if (RegQueryValueExW(k, L"SymbolicLinkValue", nullptr, &lt, nullptr, nullptr) == ERROR_SUCCESS)
            return fail(L"refused: the service key is a registry link; Hadron never follows it");
        RegStartVals now;
        std::wstring re;
        if (!ReadValues(k, &now, &re)) return fail(re);
        if (now.start < 0) return fail(L"refused: the key has no Start value (Hadron never creates one)");
        if (now.start < 2) return fail(L"refused: its registry Start is Boot or System");
        // The values under this handle must be the ones RunApply re-read and logged as "before" in the I record:
        // anything else (another tool wrote in between) is not overwritten. Raw: an absent DelayedAutostart is not 0.
        if (now.start != expect.start || now.delayed != expect.delayed)
            return fail(L"refused: changed since the re-read (now Start " + std::to_wstring(now.start) + L", DelayedAutostart " +
                        (now.delayed < 0 ? std::wstring(L"absent") : std::to_wstring(now.delayed)) + L"): nothing written");
        if (w.delayed >= 0) {
            DWORD v = (DWORD)w.delayed;
            e = RegSetValueExW(k, L"DelayedAutostart", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
            if (e != ERROR_SUCCESS) return fail(L"RegSetValueEx(DelayedAutostart), so nothing was changed: " + sys::ErrorText((DWORD)e));
        }
        DWORD v = (DWORD)w.start;
        e = RegSetValueExW(k, L"Start", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        if (e != ERROR_SUCCESS) {
            std::wstring t = L"RegSetValueEx(Start): " + sys::ErrorText((DWORD)e);
            if (w.delayed >= 0) {   // put DelayedAutostart back as it was, on the same handle: nothing half-written
                LSTATUS r2;
                if (now.delayed < 0) {
                    r2 = RegDeleteValueW(k, L"DelayedAutostart");   // it was absent: the value Hadron just created goes
                } else {
                    DWORD old = (DWORD)now.delayed;
                    r2 = RegSetValueExW(k, L"DelayedAutostart", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&old), sizeof(old));
                }
                t += r2 == ERROR_SUCCESS ? std::wstring(L"; DelayedAutostart was put back as it was, so nothing changed")
                                         : L"; DelayedAutostart could not be put back (" + sys::ErrorText((DWORD)r2) +
                                               L"): Undo writes the value recorded before";
            }
            return fail(t);
        }
        // Flush to disk. A failed flush leaves the written values in the live hive (the service manager reads them at
        // the next boot unless the hive write fails then too): the write counts, and the read-back decides.
        e = RegFlushKey(k);
        RegCloseKey(k);
        if (e != ERROR_SUCCESS) *err = L"RegFlushKey: " + sys::ErrorText((DWORD)e) + L" (written; the read-back decides)";
        return true;
    }

    std::wstring Build() override {
        return RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    }
    std::unique_ptr<SxsReader> MakeSxs() override { return MakeLiveSxsReader(); }
    std::wstring MachineGuid() override {
        return RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_SUBKEY_WOW6464KEY);
    }

private:
    // HKLM-relative path of the service key in the current control set; "" when Select\Current can't be read.
    static std::wstring KeyPath(const std::wstring& name) {
        DWORD cur = 0, cb = sizeof(cur);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Select", L"Current", RRF_RT_REG_DWORD, nullptr, &cur, &cb) != ERROR_SUCCESS ||
            cur == 0 || cur > 999)
            return {};
        wchar_t b[48];
        swprintf(b, 48, L"SYSTEM\\ControlSet%03lu\\Services\\", (unsigned long)cur);
        return b + name;
    }
    // One path component, as the SCM names a service: never a path that reaches another key.
    static bool KeyNameOk(const std::wstring& n) {
        return !n.empty() && n.size() < 256 && n.find_first_of(L"\\/") == std::wstring::npos;
    }
    static bool ReadValues(HKEY k, RegStartVals* out, std::wstring* err) {
        auto dword = [&](const wchar_t* name, int* v) {
            DWORD type = 0, data = 0, cb = sizeof(data);
            LSTATUS e = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(&data), &cb);
            if (e == ERROR_FILE_NOT_FOUND) { *v = -1; return true; }
            if (e != ERROR_SUCCESS) { *err = std::wstring(L"RegQueryValueEx(") + name + L"): " + sys::ErrorText((DWORD)e); return false; }
            if (type != REG_DWORD || cb != sizeof(data)) { *err = std::wstring(name) + L" is not a REG_DWORD"; return false; }
            *v = (int)data;
            return true;
        };
        return dword(L"Start", &out->start) && dword(L"DelayedAutostart", &out->delayed);
    }

public:
    void AllowTiControl(bool on) override { tiCtl_.store(on); }

    // Every subkey of Services in the current control set (Select\Current), each opened with REG_OPTION_OPEN_LINK and
    // KEY_QUERY_VALUE only. A key that is a link (SymbolicLinkValue of type REG_LINK) is a leaf: its values are never
    // read. Values are read from the opened handle, never by a path that could follow a link.
    bool ListServiceKeys(std::vector<RegSvcKey>* out, bool* partial, std::wstring* err) override {
        out->clear();
        *partial = false;
        std::wstring root = KeyPath(L"");
        if (root.empty()) { *err = L"HKLM\\SYSTEM\\Select\\Current could not be read"; return false; }
        root.pop_back();   // the trailing backslash
        HKEY svc = nullptr;
        LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, root.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &svc);
        if (e == ERROR_FILE_NOT_FOUND)   // Wine keeps only CurrentControlSet; Windows always has ControlSetNNN
            e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &svc);
        if (e != ERROR_SUCCESS) { *err = L"RegOpenKeyEx(" + root + L"): " + sys::ErrorText((DWORD)e); return false; }
        bool ok = true;
        for (DWORD idx = 0;; idx++) {
            wchar_t name[256];
            DWORD cch = 256;
            e = RegEnumKeyExW(svc, idx, name, &cch, nullptr, nullptr, nullptr, nullptr);
            if (e == ERROR_NO_MORE_ITEMS) break;
            if (e != ERROR_SUCCESS) {   // review R10: the keys so far are kept and the stop is reported, never silent
                *err = L"RegEnumKeyEx: " + sys::ErrorText((DWORD)e);
                ok = !out->empty();
                *partial = ok;
                break;
            }
            RegSvcKey k;
            k.name.assign(name, cch);
            HKEY h = nullptr;
            LSTATUS oe = RegOpenKeyExW(svc, k.name.c_str(), REG_OPTION_OPEN_LINK, KEY_QUERY_VALUE, &h);
            if (oe != ERROR_SUCCESS) {
                k.err = sys::ErrorText((DWORD)oe);
                out->push_back(std::move(k));
                continue;
            }
            DWORD lt = 0, lcb = 0;
            if (RegQueryValueExW(h, L"SymbolicLinkValue", nullptr, &lt, nullptr, &lcb) == ERROR_SUCCESS && lt == REG_LINK) {
                k.link = true;   // a leaf: nothing else is read
            } else {
                auto dword = [&](const wchar_t* v, long long* outv) {
                    DWORD t = 0, d = 0, cb = sizeof(d);
                    if (RegQueryValueExW(h, v, nullptr, &t, reinterpret_cast<BYTE*>(&d), &cb) == ERROR_SUCCESS && t == REG_DWORD &&
                        cb == sizeof(d))
                        *outv = d;
                };
                auto str = [&](const wchar_t* v) {
                    DWORD t = 0, cb = 0;
                    if (RegQueryValueExW(h, v, nullptr, &t, nullptr, &cb) != ERROR_SUCCESS || (t != REG_SZ && t != REG_EXPAND_SZ) ||
                        cb == 0 || cb > 64 * 1024)
                        return std::wstring();
                    std::vector<wchar_t> b(cb / sizeof(wchar_t) + 2, 0);
                    DWORD cb2 = cb;
                    if (RegQueryValueExW(h, v, nullptr, &t, reinterpret_cast<BYTE*>(b.data()), &cb2) != ERROR_SUCCESS) return std::wstring();
                    return std::wstring(b.data());
                };
                long long st = -1;
                dword(L"Type", &k.type);
                dword(L"Start", &st);
                k.start = st < 0 || st > 0x7fffffff ? -1 : (int)st;
                k.image = str(L"ImagePath");
                k.display = str(L"DisplayName");
                k.group = str(L"Group");
                // StartOverride (values 0, 1, 2..., REG_DWORD): opened the same way, read only; a link is a leaf.
                HKEY so = nullptr;
                if (RegOpenKeyExW(h, L"StartOverride", REG_OPTION_OPEN_LINK, KEY_QUERY_VALUE, &so) == ERROR_SUCCESS) {
                    DWORD st2 = 0, scb = 0;
                    bool soLink = RegQueryValueExW(so, L"SymbolicLinkValue", nullptr, &st2, nullptr, &scb) == ERROR_SUCCESS && st2 == REG_LINK;
                    for (DWORD vi = 0; !soLink && vi < 64; vi++) {
                        wchar_t vn[64];
                        DWORD vcch = 64, vt = 0, vd = 0, vcb = sizeof(vd);
                        LSTATUS ve = RegEnumValueW(so, vi, vn, &vcch, nullptr, &vt, reinterpret_cast<BYTE*>(&vd), &vcb);
                        if (ve == ERROR_NO_MORE_ITEMS) break;
                        if (ve != ERROR_SUCCESS || vt != REG_DWORD || vcb != sizeof(vd)) continue;
                        k.startOverride.push_back({std::wstring(vn, vcch), vd});
                    }
                    RegCloseKey(so);
                }
            }
            RegCloseKey(h);
            out->push_back(std::move(k));
        }
        RegCloseKey(svc);
        return ok;
    }

private:
    bool armed_ = false;
    std::atomic<bool> tiCtl_{false};
};

}  // namespace

std::unique_ptr<Backend> MakeLiveBackend() { return std::make_unique<LiveBackend>(); }

bool CompanyPathAllowed(const std::wstring& path) {
    // Only a plain local drive path: never a UNC path (a dead share stalls the load), never \\?\UNC\ or a device path.
    std::wstring p = path;
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    if (p.size() < 3 || !iswalpha(p[0]) || p[1] != L':' || p[2] != L'\\') return false;
    return true;
}

bool CompanyAttrsAllowed(DWORD attrs) {
    // Hard rule 1: never read data from a file that would be recalled from the cloud or from offline storage.
    const DWORD kRecall = FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;
    // A reparse point is a leaf (rule 1): its own attributes say nothing about the target that version.dll would
    // read, which may be a cloud placeholder or a network path.
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & kRecall) && !(attrs & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

namespace {
// Metadata only (no data is read, nothing is recalled): a local drive that is not a network drive, and a file
// without the recall or offline attributes.
// Attributes as directory enumeration reports them (WIN32_FIND_DATA, the source rule 1 names): RECALL_ON_OPEN is
// only guaranteed there, and FindFirstFileExW never opens the file, so nothing is recalled.
DWORD FindAttrs(const std::wstring& p) {
    if (p.find_first_of(L"*?") != std::wstring::npos) return INVALID_FILE_ATTRIBUTES;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(sys::ToExtended(p).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return INVALID_FILE_ATTRIBUTES;
    FindClose(h);
    return fd.dwFileAttributes;
}

bool CompanyReadable(const std::wstring& path) {
    if (!CompanyPathAllowed(path)) return false;
    std::wstring p = path.rfind(L"\\\\?\\", 0) == 0 ? path.substr(4) : path;
    std::wstring root = p.substr(0, 3);
    if (GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) return false;   // a mapped network drive: same as UNC
    // No folder on the way may be a reparse point either: a junction or symlinked folder can lead to a share or a
    // cloud folder that the drive-type check above can't see.
    for (size_t i = p.find(L'\\', 3); i != std::wstring::npos; i = p.find(L'\\', i + 1)) {
        DWORD a = FindAttrs(p.substr(0, i));
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return CompanyAttrsAllowed(FindAttrs(p));
}

// GetFileVersionInfoExW reads the version strings from the file's MUI satellites when there are any, so every MUI
// file must pass the same checks. GetFileMUIPath only tests that each exists; it reads no data.
bool CompanyMuiReadable(const std::wstring& path) {
    ULONGLONG en = 0;
    for (int guard = 0; guard < 64; guard++) {
        wchar_t lang[LOCALE_NAME_MAX_LENGTH] = {};
        ULONG cl = LOCALE_NAME_MAX_LENGTH;
        std::vector<wchar_t> mp(32768, 0);
        ULONG cm = (ULONG)mp.size();
        if (!GetFileMUIPath(MUI_LANGUAGE_NAME, path.c_str(), lang, &cl, mp.data(), &cm, &en)) {
            DWORD e = GetLastError();
            return e == ERROR_NO_MORE_FILES || e == ERROR_FILE_NOT_FOUND || e == ERROR_RESOURCE_TYPE_NOT_FOUND ||
                   e == ERROR_MUI_FILE_NOT_FOUND;
        }
        if (!CompanyReadable(mp.data())) return false;
    }
    return false;   // more satellites than any real file has: don't read it
}
}  // namespace

// VERSIONINFO CompanyName of a service's image (ServiceDll for svchost services). Read-only; "" if none, and "" without
// opening the file when it is on a network path or would be recalled from the cloud (R3).
std::wstring FileCompany(const SvcItem& item) {
    std::wstring win;
    {
        std::vector<wchar_t> w(MAX_PATH);
        UINT n = GetSystemWindowsDirectoryW(w.data(), (UINT)w.size());
        if (n >= w.size()) { w.assign(n + 1, 0); n = GetSystemWindowsDirectoryW(w.data(), (UINT)w.size()); }
        if (n == 0 || n >= w.size()) return {};   // no Windows folder to resolve \SystemRoot\ against: show nothing
        win.assign(w.data(), n);
    }
    ImageInfo im = NormalizeImage(item.image, win, item.name, IsDriverType(item.type));
    std::wstring path = im.path;
    if (!im.svchostGroup.empty()) {
        std::wstring dll = RegString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\" + item.name + L"\\Parameters", L"ServiceDll");
        if (dll.empty()) dll = RegString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\" + item.name, L"ServiceDll");
        if (!dll.empty()) path = dll;
    }
    if (path.empty()) return {};
    if (path.find(L'%') != std::wstring::npos) {
        // The return is the size needed (with the terminator), or 0 on failure: a too-small buffer is never used.
        DWORD need = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
        if (need == 0) return {};
        std::vector<wchar_t> exp(need + 1, 0);
        DWORD got = ExpandEnvironmentStringsW(path.c_str(), exp.data(), (DWORD)exp.size());
        if (got == 0 || got > exp.size()) return {};
        path = exp.data();
    }
    if (!CompanyReadable(path) || !CompanyMuiReadable(path)) return {};
    std::wstring ext = sys::ToExtended(path);
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, ext.c_str(), &dummy);
    if (!sz) return {};
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, ext.c_str(), 0, sz, buf.data())) return {};
    struct Lang { WORD lang, cp; }* langs = nullptr;
    UINT cb = 0;
    std::vector<std::wstring> keys;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &cb) && langs) {
        for (UINT k = 0; k < cb / sizeof(Lang) && k < 4; k++) {
            wchar_t key[64];
            swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", langs[k].lang, langs[k].cp);
            keys.push_back(key);
        }
    }
    keys.push_back(L"\\StringFileInfo\\040904B0\\CompanyName");
    for (auto& k : keys) {
        wchar_t* v = nullptr;
        UINT n = 0;
        if (VerQueryValueW(buf.data(), k.c_str(), reinterpret_cast<void**>(&v), &n) && v && n) return std::wstring(v);
    }
    return {};
}

}  // namespace hx::services
