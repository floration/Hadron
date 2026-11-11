// core/sys/settings.h — per-module settings. See docs/ARCHITECTURE.md "Settings, logs, crashes".
//
// Stored at %LOCALAPPDATA%\Hadron\settings.ini, or portable mode (Hadron.ini next to the exe) when
// that file exists. A module gets a `Settings` bound to its own [section] via IHost::Prefs().
#pragma once

#include "core/sys/win_compat.h"
#include <map>
#include <set>
#include <string>

namespace hx::sys {

class SettingsStore;

// A view onto one [section] of the store. Keys are case-insensitive. Writes are flushed by Save().
class Settings {
public:
    Settings(SettingsStore* store, std::wstring section) : store_(store), section_(std::move(section)) {}

    std::wstring GetStr(const wchar_t* key, const wchar_t* fallback = L"") const;
    void SetStr(const wchar_t* key, const std::wstring& value);
    int  GetInt(const wchar_t* key, int fallback = 0) const;
    void SetInt(const wchar_t* key, int value);
    bool GetBool(const wchar_t* key, bool fallback = false) const;
    void SetBool(const wchar_t* key, bool value);

private:
    SettingsStore* store_;
    std::wstring section_;
};

class SettingsStore {
public:
    // Picks the portable or per-user path and loads it if present. Missing file is not an error.
    void Load();
    // Writes only the keys this process changed: re-reads the file under a mutex, applies those keys and
    // replaces it atomically (per-process temp file + MoveFileEx). Several exes can share one file without
    // undoing each other's changes. Nothing changed => nothing written.
    bool Save(DWORD* err = nullptr);
    Settings Section(const std::wstring& name) { return Settings(this, name); }
    const std::wstring& Path() const { return path_; }

private:
    friend class Settings;
    // section -> (lowercased key -> value). A tiny INI; values are not quoted or escaped in v1.
    std::map<std::wstring, std::map<std::wstring, std::wstring>> data_;
    std::wstring path_;
    bool dirty_ = false;
    std::set<std::pair<std::wstring, std::wstring>> changed_;   // (section, lowercased key) set since Save
};

}  // namespace hx::sys
