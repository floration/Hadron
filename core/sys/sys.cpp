#include "core/sys/sys.h"
#include <atomic>
#include <vector>

namespace hx::sys {

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD got = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &got) != 0;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

bool EnablePrivilege(const wchar_t* name, DWORD* err) {
    if (err) *err = ERROR_SUCCESS;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        if (err) *err = GetLastError();
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        // AdjustTokenPrivileges can return TRUE yet leave the privilege unassigned; the real
        // result is in GetLastError() (CLAUDE.md hard rule 4).
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        DWORD gle = GetLastError();
        ok = (gle == ERROR_SUCCESS);
        if (!ok && err) *err = gle;
    } else if (err) {
        *err = GetLastError();
    }
    CloseHandle(token);
    return ok;
}

std::wstring ModulePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);   // truncated: ERROR_INSUFFICIENT_BUFFER, grow and retry
    }
}

std::wstring ToExtended(const std::wstring& path) {
    if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0) return path;  // already extended
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetFullPathNameW(path.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
        if (n == 0) return path;                 // can't normalise; hand back what we got
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(n);
    }
    std::wstring full(buf.data());
    if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\')
        return L"\\\\?\\UNC\\" + full.substr(2);  // \\server\share -> \\?\UNC\server\share
    return L"\\\\?\\" + full;
}

std::wstring ToShell(const std::wstring& path) {
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\\\" + path.substr(8);
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path.substr(4);
    return path;
}

std::wstring ErrorText(DWORD err) {
    LPWSTR msg = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, nullptr);
    std::wstring out;
    if (n && msg) {
        out.assign(msg, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    }
    if (msg) LocalFree(msg);
    if (out.empty()) out = L"Error " + std::to_wstring(err);
    return out;
}

namespace {
std::atomic<bool> g_noFirmwareWrites{false};
}

void SetNoFirmwareWrites() { g_noFirmwareWrites.store(true); }
bool NoFirmwareWrites() { return g_noFirmwareWrites.load(); }

}  // namespace hx::sys
