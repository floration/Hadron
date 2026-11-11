// modules/bios/firmware.cpp — see firmware.h. Read-only: UEFI variable reads, variable-name
// enumeration and the SMBIOS table. Hard rule 2: nothing in this module writes firmware.
#include "modules/bios/firmware.h"
#include "core/sys/sys.h"
#include <cstdio>
#include <cstring>

namespace hx::bios {

// ---- SMBIOS -------------------------------------------------------------------------------------
namespace {
std::wstring SmbiosString(const uint8_t* fmtEnd, const uint8_t* end, int index) {
    if (index <= 0) return L"";
    const uint8_t* p = fmtEnd;
    for (int i = 1; p < end && *p; i++) {
        const uint8_t* s = p;
        while (p < end && *p) p++;
        if (i == index) {
            std::wstring w;
            for (const uint8_t* q = s; q < p; q++) w += (wchar_t)*q;   // SMBIOS strings are ASCII in practice
            while (!w.empty() && w.back() == L' ') w.pop_back();
            return w;
        }
        p++;   // past the NUL
    }
    return L"";
}
}  // namespace

bool ParseSmbios(const uint8_t* raw, size_t n, SmbiosInfo* out) {
    *out = SmbiosInfo{};
    if (n < 8) return false;
    uint32_t len = raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((uint32_t)raw[7] << 24);
    const uint8_t* p = raw + 8;
    const uint8_t* end = raw + 8 + (len < n - 8 ? len : n - 8);
    bool have0 = false, have2 = false;
    while (p + 4 <= end) {
        uint8_t type = p[0], flen = p[1];
        if (flen < 4 || p + flen > end) break;
        const uint8_t* strs = p + flen;
        if (type == 0 && !have0 && flen >= 9) {
            out->biosVendor = SmbiosString(strs, end, p[4]);
            out->biosVersion = SmbiosString(strs, end, p[5]);
            out->biosDate = SmbiosString(strs, end, p[8]);
            have0 = true;
        } else if (type == 2 && !have2 && flen >= 6) {
            out->boardMaker = SmbiosString(strs, end, p[4]);
            out->boardProduct = SmbiosString(strs, end, p[5]);
            have2 = true;
        }
        if (type == 127) break;   // end-of-table
        // Skip the string set: up to and including the double NUL.
        const uint8_t* q = strs;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
        p = q + 2;
    }
    out->ok = have0 || have2;
    return out->ok;
}

bool ReadSmbios(SmbiosInfo* out, DWORD* err) {
    *out = SmbiosInfo{};
    const DWORD sig = 'R' << 24 | 'S' << 16 | 'M' << 8 | 'B';
    UINT need = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    if (need == 0) { if (err) *err = GetLastError(); return false; }
    std::vector<uint8_t> buf(need);
    UINT got = GetSystemFirmwareTable(sig, 0, buf.data(), need);
    if (got == 0 || got > need) { if (err) *err = got ? ERROR_INSUFFICIENT_BUFFER : GetLastError(); return false; }
    if (!ParseSmbios(buf.data(), got, out)) { if (err) *err = ERROR_INVALID_DATA; return false; }
    return true;
}

// ---- privilege ----------------------------------------------------------------------------------
// diskmap's ThreadBackupPrivilege pattern: the privilege lives on this thread's impersonation token
// only, so it never reaches the process token, other modules or child processes.
ThreadPrivilege::ThreadPrivilege(const wchar_t* name) {
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
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) err_ = GetLastError();
    else if (!AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr)) err_ = GetLastError();
    else { DWORD e = GetLastError(); if (e == ERROR_NOT_ALL_ASSIGNED) err_ = e; else ok_ = true; }   // rule 4
    CloseHandle(tok);
}

ThreadPrivilege::~ThreadPrivilege() { if (impersonating_) RevertToSelf(); }

// ---- variables ----------------------------------------------------------------------------------
namespace {
std::wstring Braced(const std::wstring& guid) {
    if (!guid.empty() && guid.front() == L'{') return guid;
    return L"{" + guid + L"}";
}
std::wstring GuidText(const GUID& g) {
    wchar_t b[48];
    swprintf(b, 48, L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}
}  // namespace

void ReadVariable(FwVar* v) {
    v->data.clear(); v->err = 0; v->attr = 0;
    std::wstring g = Braced(v->guid);
    DWORD size = 4096;
    for (;;) {
        std::vector<uint8_t> buf(size);
        DWORD attr = 0;
        SetLastError(0);
        DWORD got = GetFirmwareEnvironmentVariableExW(v->name.c_str(), g.c_str(), buf.data(), size, &attr);
        if (got > 0) { buf.resize(got); v->data = std::move(buf); v->attr = attr; return; }
        DWORD e = GetLastError();
        if (e == ERROR_INSUFFICIENT_BUFFER && size < (4u << 20)) { size *= 4; continue; }   // grow on 122
        v->err = e ? e : ERROR_INVALID_DATA;   // 0 bytes with no error: treat as empty/unreadable
        return;
    }
}

std::wstring FirmwareTypeText(bool* uefi) {
    *uefi = false;
    using Fn = BOOL(WINAPI*)(PFIRMWARE_TYPE);
    Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareType");
    FIRMWARE_TYPE t = FirmwareTypeUnknown;
    if (!fn || !fn(&t)) return L"Unknown";
    if (t == FirmwareTypeUefi) { *uefi = true; return L"UEFI"; }
    if (t == FirmwareTypeBios) return L"Legacy BIOS";
    return L"Unknown";
}

void ReadLive(const std::vector<std::pair<std::wstring, std::wstring>>& want, LiveResult* out) {
    *out = LiveResult{};
    out->elevated = sys::IsElevated();
    out->fwType = FirmwareTypeText(&out->uefi);
    ReadSmbios(&out->smbios, &out->smbiosErr);
    if (!out->uefi) return;   // no UEFI variables to read (legacy boot, or Wine)
    ThreadPrivilege priv(L"SeSystemEnvironmentPrivilege");
    out->privOk = priv.ok();
    out->privErr = priv.err();
    for (auto& w : want) {
        FwVar v;
        v.name = w.first;
        v.guid = w.second;
        ReadVariable(&v);
        if (v.err == 0) out->readOk++;
        out->vars.push_back(std::move(v));
    }
}

bool EnumerateAll(std::vector<FwVar>* out, std::wstring* err, const std::function<bool()>& cancelled) {
    out->clear();
    // Read-only enumeration (names only, class 1). The one allowed undocumented API (ARCHITECTURE.md).
    using Fn = LONG(NTAPI*)(ULONG, PVOID, PULONG);
    Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtEnumerateSystemEnvironmentValuesEx");
    if (!fn) { if (err) *err = L"NtEnumerateSystemEnvironmentValuesEx is not available on this system."; return false; }
    ULONG size = 64 * 1024;
    std::vector<uint8_t> buf;
    LONG st = 0;
    for (int tries = 0; tries < 8; tries++) {
        buf.assign(size, 0);
        ULONG len = size;
        st = fn(1, buf.data(), &len);
        if (st == (LONG)0xC0000023L /* STATUS_BUFFER_TOO_SMALL */) {
            size = (len > size ? len : size * 2) + 4096;
            continue;
        }
        break;
    }
    if (st < 0) {
        wchar_t b[96];
        swprintf(b, 96, L"Could not list UEFI variables (NTSTATUS 0x%08lX).", (unsigned long)st);
        std::wstring m = b;
        if ((unsigned long)st == 0xC0000061UL) m += L" The privilege to read firmware variables is not held.";
        if (err) *err = m;
        return false;
    }
    size_t off = 0;
    while (off + 20 <= buf.size()) {
        if (cancelled && cancelled()) break;
        const uint8_t* e = buf.data() + off;
        ULONG next = *(const ULONG*)e;
        GUID g;
        memcpy(&g, e + 4, sizeof(g));
        const wchar_t* nm = (const wchar_t*)(e + 20);
        size_t maxChars = (buf.size() - off - 20) / sizeof(wchar_t);
        size_t len = 0;
        while (len < maxChars && nm[len]) len++;
        FwVar v;
        v.name.assign(nm, len);
        v.guid = GuidText(g);
        ReadVariable(&v);
        out->push_back(std::move(v));
        if (next == 0) break;
        off += next;
    }
    return true;
}

std::wstring VarErrorText(DWORD err) {
    switch (err) {
        case 0: return L"read";
        case ERROR_ENVVAR_NOT_FOUND: return L"not found (203): this PC has no such variable, so the map doesn't match it";
        case ERROR_PRIVILEGE_NOT_HELD: return L"privilege missing (1314): SeSystemEnvironmentPrivilege isn't enabled";
        case ERROR_ACCESS_DENIED: return L"protected (5): the firmware refused the read";
        case ERROR_INSUFFICIENT_BUFFER: return L"larger than 4 MB (122)";
        case ERROR_INVALID_FUNCTION: return L"not supported (1): no UEFI variable service";
    }
    return sys::ErrorText(err) + L" (" + std::to_wstring(err) + L")";
}

}  // namespace hx::bios
