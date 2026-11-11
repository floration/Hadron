#include "core/sys/settings.h"
#include "core/sys/sys.h"
#include <algorithm>
#include <cstdio>
#include <shlobj.h>
#include <vector>

namespace hx::sys {

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

std::wstring Settings::GetStr(const wchar_t* key, const wchar_t* fallback) const {
    auto sit = store_->data_.find(section_);
    if (sit != store_->data_.end()) {
        auto kit = sit->second.find(Lower(key));
        if (kit != sit->second.end()) return kit->second;
    }
    return fallback;
}
void Settings::SetStr(const wchar_t* key, const std::wstring& value) {
    std::wstring k = Lower(key);
    store_->data_[section_][k] = value;
    store_->changed_.insert({section_, k});
    store_->dirty_ = true;
}
int Settings::GetInt(const wchar_t* key, int fallback) const {
    std::wstring v = GetStr(key, L"");
    return v.empty() ? fallback : (int)wcstol(v.c_str(), nullptr, 10);
}
void Settings::SetInt(const wchar_t* key, int value) { SetStr(key, std::to_wstring(value)); }
bool Settings::GetBool(const wchar_t* key, bool fallback) const { return GetInt(key, fallback ? 1 : 0) != 0; }
void Settings::SetBool(const wchar_t* key, bool value) { SetInt(key, value ? 1 : 0); }

using IniMap = std::map<std::wstring, std::map<std::wstring, std::wstring>>;

// UTF-8, optional BOM. A tiny INI: [section] lines and key=value lines; ; or # is a comment.
static void ReadIni(const std::wstring& path, IniMap& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string bytes;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
    fclose(f);
    size_t start = (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF) ? 3 : 0;
    int wn = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, (int)(bytes.size() - start), nullptr, 0);
    std::wstring text(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, (int)(bytes.size() - start), text.data(), wn);

    std::wstring section;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t b = line.find_first_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        line = line.substr(b);
        if (line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            size_t c = line.find(L']');
            if (c != std::wstring::npos) section = line.substr(1, c - 1);
        } else {
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos)
                out[section][Lower(line.substr(0, eq))] = line.substr(eq + 1);
        }
    }
}

void SettingsStore::Load() {
    // Portable mode: Hadron.ini next to the exe. Otherwise %LOCALAPPDATA%\Hadron\settings.ini.
    std::wstring exe = ModulePath();
    std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
    std::wstring portable = dir + L"\\Hadron.ini";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) {
        path_ = portable;
    } else {
        wchar_t local[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, local))) {
            std::wstring d = std::wstring(local) + L"\\Hadron";
            CreateDirectoryW(d.c_str(), nullptr);
            path_ = d + L"\\settings.ini";
        }
    }
    data_.clear();
    changed_.clear();
    dirty_ = false;
    if (path_.empty()) return;
    ReadIni(path_, data_);
}

bool SettingsStore::Save(DWORD* err) {
    if (err) *err = ERROR_SUCCESS;
    if (changed_.empty()) return true;   // never rewrite the file from a stale snapshot
    if (path_.empty()) { if (err) *err = ERROR_PATH_NOT_FOUND; return false; }
    // One writer at a time across every Hadron exe of this user.
    HANDLE mx = CreateMutexW(nullptr, FALSE, L"Local\\Hadron.settings");
    if (mx) WaitForSingleObject(mx, 5000);
    struct Unlock { HANDLE h; ~Unlock() { if (h) { ReleaseMutex(h); CloseHandle(h); } } } unlock{mx};
    // Merge: start from what is on disk now, then apply only this process's changes.
    IniMap merged;
    ReadIni(path_, merged);
    for (auto& [sec, key] : changed_) {
        auto sit = data_.find(sec);
        if (sit == data_.end()) continue;
        auto kit = sit->second.find(key);
        if (kit != sit->second.end()) merged[sec][key] = kit->second;
    }
    std::wstring tmp = path_ + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { if (err) *err = GetLastError(); return false; }
    auto put = [&](const std::wstring& s) {
        int bn = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u8(bn, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u8.data(), bn, nullptr, nullptr);
        fwrite(u8.data(), 1, u8.size(), f);
    };
    for (auto& [section, keys] : merged) {
        put(L"[" + section + L"]\n");
        for (auto& [k, v] : keys) put(k + L"=" + v + L"\n");
        put(L"\n");
    }
    fflush(f);
    fclose(f);
    // Atomic replace so a crash never leaves a half-written ini.
    if (!MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = GetLastError();
        DeleteFileW(tmp.c_str());   // our own per-process temp file
        return false;
    }
    data_ = std::move(merged);
    changed_.clear();
    dirty_ = false;
    return true;
}

}  // namespace hx::sys
