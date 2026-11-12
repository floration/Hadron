// modules/services/model.cpp — names, image-path normalisation and text helpers (pure).
#include "modules/services/model.h"
#include "modules/services/svc_compat.h"
#include <algorithm>
#include <cwctype>
#include <map>

namespace hx::services {

const wchar_t kCfgUnreadable[] = L"configuration unreadable";
const wchar_t kRegOnlyKind[] = L"Registry only (not loaded by the SCM)";

const wchar_t* SourceName(const SvcItem& s) {
    switch (s.src) {
        case SvcItem::Source::Scm: return L"scm";
        case SvcItem::Source::ScmDirect: return L"scm-direct";
        case SvcItem::Source::Registry: return L"registry";
    }
    return L"scm";
}

std::wstring CfgText(const StartCfg& c) {
    std::wstring s = std::to_wstring(c.start) + L"/";
    s += c.delayed < 0 ? L"-" : std::to_wstring(c.delayed);
    return s;
}

bool ParseCfg(const std::wstring& s, StartCfg* out) {
    if (s.size() != 3 || s[1] != L'/' || s[0] < L'0' || s[0] > L'4') return false;
    StartCfg c;
    c.start = s[0] - L'0';
    if (s[2] == L'-') c.delayed = -1;
    else if (s[2] == L'0' || s[2] == L'1') c.delayed = s[2] - L'0';
    else return false;
    *out = c;
    return true;
}

std::wstring StartName(int start) {
    switch (start) {
        case 0: return L"Boot";
        case 1: return L"System";
        case 2: return L"Auto";
        case 3: return L"Manual";
        case 4: return L"Disabled";
    }
    return L"Unknown (" + std::to_wstring(start) + L")";
}

std::wstring StartLabel(const StartCfg& c, bool trigger) {
    std::wstring s = c.Delayed() ? L"Auto (delayed)" : StartName(c.start);
    if (trigger) s += L" +trigger";
    return s;
}

std::wstring StateName(DWORD state) {
    switch (state) {
        case 0: return L"Registry only";   // a Services key the SCM doesn't have (collect.h): no SCM state
        case SERVICE_STOPPED: return L"Stopped";
        case SERVICE_START_PENDING: return L"Starting";
        case SERVICE_STOP_PENDING: return L"Stopping";
        case SERVICE_RUNNING: return L"Running";
        case SERVICE_CONTINUE_PENDING: return L"Continuing";
        case SERVICE_PAUSE_PENDING: return L"Pausing";
        case SERVICE_PAUSED: return L"Paused";
    }
    return L"State " + std::to_wstring(state);
}

Kind KindOf(DWORD type) {
    // Adapter (0x4) and recognizer (0x8, Fs_Rec) drivers are drivers too (Review R15).
    if (type & (SERVICE_KERNEL_DRIVER | SERVICE_ADAPTER)) return Kind::KernelDriver;
    if (type & (SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)) return Kind::FsDriver;
    if (type & SERVICE_USER_SERVICE)
        return (type & SERVICE_USERSERVICE_INSTANCE) ? Kind::UserInstance : Kind::UserTemplate;
    if (type & (SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS)) return Kind::Service;
    return Kind::Other;
}

bool IsDriverType(DWORD type) {
    return (type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_ADAPTER | SERVICE_RECOGNIZER_DRIVER)) != 0;
}

bool OnTab(const SvcItem& s, Tab t) {
    bool drv = IsDriverType(s.type);
    return t == TAB_DRIVERS ? drv : !drv;
}

std::wstring TypeName(DWORD type) {
    if (type == 0) return L"No Type value";   // a registry-only key without one
    switch (KindOf(type)) {
        case Kind::KernelDriver: return (type & SERVICE_KERNEL_DRIVER) ? L"Kernel driver" : L"Adapter driver";
        case Kind::FsDriver: return (type & SERVICE_FILE_SYSTEM_DRIVER) ? L"File system driver" : L"File system recognizer driver";
        case Kind::UserTemplate: return L"Per-user template";
        case Kind::UserInstance: return L"Per-user instance";
        case Kind::Service: return (type & SERVICE_WIN32_SHARE_PROCESS) ? L"Shared process" : L"Own process";
        case Kind::Other: break;
    }
    return L"Type 0x" + Hex8(type);
}

std::wstring ErrCtlName(int e) {
    switch (e) {
        case 0: return L"Ignore";
        case 1: return L"Normal";
        case 2: return L"Severe";
        case 3: return L"Critical";
    }
    return std::to_wstring(e);
}

std::wstring ProtectName(int p) {
    switch (p) {
        case 0: return L"none";
        case 1: return L"Windows (protected process)";
        case 2: return L"Windows light (protected process light)";
        case 3: return L"Antimalware light (protected process light)";
    }
    return L"level " + std::to_wstring(p);
}

std::wstring TemplateName(const std::wstring& name, DWORD type) {
    if (!(type & SERVICE_USERSERVICE_INSTANCE)) return name;
    size_t us = name.rfind(L'_');
    if (us == std::wstring::npos || us == 0 || us + 1 >= name.size()) return name;
    for (size_t i = us + 1; i < name.size(); i++)
        if (!iswxdigit(name[i])) return name;
    return name.substr(0, us);
}

void FoldInstances(std::vector<SvcItem>* items) {
    std::map<std::wstring, size_t> byName;
    for (size_t i = 0; i < items->size(); i++) {
        (*items)[i].kind = KindOf((*items)[i].type);
        (*items)[i].instances.clear();
        byName[Lower((*items)[i].name)] = i;
    }
    std::vector<SvcItem> out;
    out.reserve(items->size());
    std::vector<std::pair<size_t, std::wstring>> attach;
    for (auto& s : *items) {
        if (s.kind != Kind::UserInstance) continue;
        auto it = byName.find(Lower(TemplateName(s.name, s.type)));
        if (it != byName.end() && (*items)[it->second].kind == Kind::UserTemplate)
            attach.push_back({it->second, s.name + L" (" + StateName(s.state) + L")"});
    }
    for (auto& a : attach) (*items)[a.first].instances.push_back(a.second);
    for (auto& s : *items) {
        if (s.kind == Kind::UserInstance) {
            auto it = byName.find(Lower(TemplateName(s.name, s.type)));
            if (it != byName.end() && (*items)[it->second].kind == Kind::UserTemplate) continue;
        }
        out.push_back(std::move(s));
    }
    *items = std::move(out);
}

ImageInfo NormalizeImage(const std::wstring& raw, const std::wstring& windir, const std::wstring& name, bool driver) {
    ImageInfo r;
    std::wstring s = raw;
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (s.empty()) {
        if (driver) r.path = windir + L"\\System32\\drivers\\" + name + L".sys";
        return r;
    }
    std::wstring path;
    if (s[0] == L'"') {
        size_t q = s.find(L'"', 1);
        if (q == std::wstring::npos) { path = s.substr(1); }
        else { path = s.substr(1, q - 1); r.args = s.substr(q + 1); }
    } else if (driver) {
        path = s;   // driver paths carry no arguments
    } else {
        // Unquoted: the path ends at the first ".exe" followed by a space or the end (CreateProcess-like).
        std::wstring low = Lower(s);
        size_t e = low.find(L".exe");
        while (e != std::wstring::npos && e + 4 < low.size() && low[e + 4] != L' ') e = low.find(L".exe", e + 1);
        if (e != std::wstring::npos) { path = s.substr(0, e + 4); r.args = s.substr(e + 4); }
        else {
            size_t sp = s.find(L' ');
            path = sp == std::wstring::npos ? s : s.substr(0, sp);
            if (sp != std::wstring::npos) r.args = s.substr(sp);
        }
        r.unquotedSpaces = path.find(L' ') != std::wstring::npos;
    }
    while (!r.args.empty() && iswspace(r.args.front())) r.args.erase(r.args.begin());
    std::wstring low = Lower(path);
    if (low.rfind(L"\\??\\", 0) == 0) path = path.substr(4);
    else if (low.rfind(L"\\systemroot\\", 0) == 0) path = windir + path.substr(11);
    else if (low.rfind(L"system32\\", 0) == 0) path = windir + L"\\" + path;
    // svchost -k <group>
    if (Lower(path).find(L"svchost.exe") != std::wstring::npos) {
        std::wstring la = Lower(r.args);
        size_t k = la.find(L"-k ");
        if (k != std::wstring::npos) {
            size_t a = k + 3;
            size_t b = r.args.find(L' ', a);
            r.svchostGroup = r.args.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        }
    }
    r.path = path;
    return r;
}

// ---- text helpers --------------------------------------------------------------------------------
std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool IEq(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

bool IContains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

std::wstring Esc(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\t') o += L' ';
        else if (c == L'\r') continue;
        else if (c == L'\n') o += L"\\n";
        else o += c;
    }
    return o;
}

std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            i++;
            o += s[i] == L'n' ? L'\n' : s[i];
        } else {
            o += s[i];
        }
    }
    return o;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> v;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        if (b == std::wstring::npos) { v.push_back(s.substr(a)); break; }
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

std::wstring Join(const std::vector<std::wstring>& v, const std::wstring& sep) {
    std::wstring o;
    for (size_t i = 0; i < v.size(); i++) { if (i) o += sep; o += v[i]; }
    return o;
}

uint32_t Crc32(const void* data, size_t n, uint32_t crc) {
    // Function-local static with an initialiser: thread-safe (workers and the UI thread both call this).
    struct Table { uint32_t t[256]; Table() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        } } };
    static const Table table;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table.t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::wstring Hex8(uint32_t v) {
    wchar_t b[16];
    swprintf(b, 16, L"%08X", v);
    return b;
}

}  // namespace hx::services
