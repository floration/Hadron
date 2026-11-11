// modules/services/backend_live.cpp — the live backend: the ONLY file in Hadron that calls SCM write APIs.
//
// Reads open the SCM with SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE and each service with
// SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS only (spec §5.1); the registry is opened KEY_QUERY_VALUE.
// Writes (ChangeServiceConfigW for the start type, ChangeServiceConfig2W for the delayed flag, and
// StartServiceW / ControlServiceExW for live control) refuse until Arm() was called. Only the UI's
// confirmed-plan path and --live-test arm it. Hadron never creates, deletes or re-registers a service,
// never changes an account, a failure action or a trigger, and never edits a key's DACL (CI guards this).
#include "modules/services/backend.h"
#include "modules/services/svc_compat.h"
#include "core/sys/sys.h"
#include <algorithm>

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
    i.kind = KindOf(i.type);
    *out = std::move(i);
    return true;
}

bool WaitState(SC_HANDLE s, DWORD want, const std::atomic<bool>* cancel, std::wstring* err) {
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
        if (GetTickCount() - start > 30000) {   // overall cap (r1 §4)
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
        for (DWORD type : types) {
            list.clear();
            DWORD resume = 0, need = 0, count = 0;
            std::vector<BYTE> buf(64 * 1024);
            bool ok = true;
            for (int guard = 0; guard < 1000; guard++) {
                BOOL r = EnumServicesStatusExW(scm.h, SC_ENUM_PROCESS_INFO, type, SERVICE_STATE_ALL, buf.data(),
                                               (DWORD)buf.size(), &need, &count, &resume, nullptr);
                DWORD e = r ? 0 : GetLastError();
                auto* arr = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
                for (DWORD k = 0; k < count; k++) list.push_back({arr[k].lpServiceName, arr[k].ServiceStatusProcess});
                if (r) break;
                if (e != ERROR_MORE_DATA) { ok = false; lastErr = e; break; }
                if (need > buf.size()) buf.assign(std::min<size_t>(need + 1024, 256 * 1024), 0);
            }
            if (ok) break;
            if (lastErr != ERROR_INVALID_PARAMETER) { *err = L"EnumServicesStatusEx: " + sys::ErrorText(lastErr); return false; }
        }
        if (list.empty() && lastErr) { *err = L"EnumServicesStatusEx: " + sys::ErrorText(lastErr); return false; }
        for (auto& e : list) {
            SvcItem i;
            std::wstring rerr;
            if (!ReadOne(scm.h, e.first, &e.second, &i, &rerr)) {
                i = SvcItem{};
                i.name = e.first;
                i.state = e.second.dwCurrentState;
                i.type = e.second.dwServiceType;
                i.kind = KindOf(i.type);
                i.readError = L"configuration unreadable: " + rerr;
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

    bool Control(const std::wstring& name, Op op, const std::atomic<bool>* cancel, std::wstring* err) override {
        if (!armed_) { *err = kNotArmed; return false; }
        Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm) { *err = L"OpenSCManager: " + sys::ErrorText(GetLastError()); return false; }
        Sc s(OpenServiceW(scm.h, name.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
        if (!s) { *err = L"OpenService: " + sys::ErrorText(GetLastError()); return false; }
        SvcItem cur;
        if (!ReadOne(scm.h, name, nullptr, &cur, err)) return false;
        if (IsDriverType(cur.type)) { *err = L"Drivers are never loaded or unloaded live."; return false; }
        if (op == Op::Start) {
            if (!StartServiceW(s.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                *err = L"StartService: " + sys::ErrorText(GetLastError());
                return false;
            }
            return WaitState(s.h, SERVICE_RUNNING, cancel, err);
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
                // The stop reason code is not documented as accepted with MAJOR_NONE/MINOR_NONE: stop without one.
                SERVICE_STATUS ss{};
                if (!ControlService(s.h, SERVICE_CONTROL_STOP, &ss)) {
                    e = GetLastError();
                    if (e == ERROR_SERVICE_NOT_ACTIVE) return true;
                    *err = L"ControlService(stop): " + sys::ErrorText(e);
                    return false;
                }
                return WaitState(s.h, SERVICE_STOPPED, cancel, err);
            }
            *err = L"ControlServiceEx(stop): " + sys::ErrorText(e);
            return false;
        }
        return WaitState(s.h, SERVICE_STOPPED, cancel, err);
    }

    bool ReadHw(HwInfo* out) override { return ReadLiveHw(out); }

    std::wstring Build() override {
        return RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    }
    std::wstring MachineGuid() override {
        return RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_SUBKEY_WOW6464KEY);
    }

private:
    bool armed_ = false;
};

}  // namespace

std::unique_ptr<Backend> MakeLiveBackend() { return std::make_unique<LiveBackend>(); }

// VERSIONINFO CompanyName of a service's image (ServiceDll for svchost services). Read-only; "" if none.
std::wstring FileCompany(const SvcItem& item) {
    wchar_t win[MAX_PATH] = {};
    GetSystemWindowsDirectoryW(win, MAX_PATH);
    ImageInfo im = NormalizeImage(item.image, win, item.name, IsDriverType(item.type));
    std::wstring path = im.path;
    if (!im.svchostGroup.empty()) {
        std::wstring dll = RegString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\" + item.name + L"\\Parameters", L"ServiceDll");
        if (dll.empty()) dll = RegString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\" + item.name, L"ServiceDll");
        if (!dll.empty()) path = dll;
    }
    if (path.empty()) return {};
    wchar_t exp[MAX_PATH * 2] = {};
    if (ExpandEnvironmentStringsW(path.c_str(), exp, MAX_PATH * 2)) path = exp;
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
