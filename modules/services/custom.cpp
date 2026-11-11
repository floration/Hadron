// modules/services/custom.cpp — custom presets: the text format, its strict parser, the data-folder store and the
// export/import file I/O (custom.h). No plan logic here: BuildCustom in plan.cpp turns a preset into a plan.
#include "modules/services/custom.h"
#include "core/sys/sys.h"
#include <algorithm>
#include <cwctype>
#include <set>

#ifndef HX_HADRON_VERSION
#define HX_HADRON_VERSION "0.0.0"
#endif

namespace hx::services {

namespace {
const char kMagic[] = "HXPRESET";
const char kRemovedMagic[] = "HXPRESET-REMOVED\t1";

std::wstring LineNo(size_t n) { return L"line " + std::to_wstring(n); }

// Strict UTF-8 -> UTF-16 for one line; false on any invalid sequence.
bool Utf8Strict(const std::string& s, std::wstring* out) {
    out->clear();
    if (s.empty()) return true;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    out->assign((size_t)n, L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &(*out)[0], n) == n;
}

bool HasControl(const std::wstring& s) {
    for (wchar_t c : s) if (c < 0x20 || c == 0x7F) return true;
    return false;
}

// A short, printable form of a bad value for an error message (never a whole 512-byte line).
std::wstring Shown(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) o += (c < 0x20 || c == 0x7F) ? L'?' : c;
    if (o.size() > 40) o = o.substr(0, 40) + L"…";
    return o;
}

bool VersionText(const std::wstring& s, size_t max, bool dotsDigitsOnly) {
    if (s.empty() || s.size() > max) return false;
    for (wchar_t c : s) {
        bool ok = (c >= L'0' && c <= L'9') || c == L'.';
        if (!dotsDigitsOnly) ok = ok || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L'+';
        if (!ok) return false;
    }
    return true;
}

std::wstring UtcStamp() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

// "20261112-083100" or "20261112-083100-2" -> "2026-11-12 08:31 UTC"; anything else -> "".
std::wstring StampWords(const std::wstring& stem) {
    if (stem.size() < 15 || stem[8] != L'-') return L"";
    for (size_t i = 0; i < 15; i++) if (i != 8 && !iswdigit(stem[i])) return L"";
    return stem.substr(0, 4) + L"-" + stem.substr(4, 2) + L"-" + stem.substr(6, 2) + L" " + stem.substr(9, 2) + L":" +
           stem.substr(11, 2) + L" UTC";
}
}  // namespace

std::wstring HadronVersion() { return FromUtf8(HX_HADRON_VERSION); }

std::wstring StartWord(const StartCfg& c) {
    switch (c.start) {
        case 2: return c.delayed == 1 ? L"auto-delayed" : L"auto";
        case 3: return L"manual";
        case 4: return L"disabled";
    }
    return L"";
}

bool StartFromWord(const std::wstring& w, StartCfg* out) {
    if (w == L"auto") { *out = StartCfg{2, 0}; return true; }
    if (w == L"auto-delayed") { *out = StartCfg{2, 1}; return true; }
    if (w == L"manual") { *out = StartCfg{3, -1}; return true; }
    if (w == L"disabled") { *out = StartCfg{4, -1}; return true; }
    return false;
}

std::wstring PresetNameProblem(const std::wstring& name) {
    if (name.empty()) return L"the preset needs a name";
    if (name.size() > kPresetMaxName) return L"the name is longer than " + std::to_wstring(kPresetMaxName) + L" characters";
    if (HasControl(name)) return L"the name has a control character (a tab or a line break)";
    if (name.front() == L' ' || name.back() == L' ') return L"the name starts or ends with a space";
    return L"";
}

std::string WritePreset(const CustomPreset& p) {
    std::wstring o = L"HXPRESET\t" + std::to_wstring(kPresetFormat) + L"\n";
    o += L"hadron\t" + p.hadron + L"\n";
    o += L"build\t" + p.build + L"\n";
    o += std::wstring(L"tab\t") + (p.tab == TAB_DRIVERS ? L"drivers" : L"services") + L"\n";
    o += L"name\t" + p.name + L"\n";
    for (auto& i : p.items) o += i.name + L"\t" + StartWord(i.target) + L"\n";
    return ToUtf8(o);
}

bool ParsePreset(const std::string& bytesIn, CustomPreset* out, std::wstring* err) {
    *out = CustomPreset{};
    if (bytesIn.size() > kPresetMaxFile) {
        *err = L"the file is larger than " + std::to_wstring(kPresetMaxFile / 1024) + L" KiB; a preset file is never that big";
        return false;
    }
    std::string bytes = bytesIn;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);   // Notepad's UTF-8 BOM
    if (bytes.empty()) { *err = L"the file is empty"; return false; }
    // Physical lines; a missing final LF is fine. Trailing empty lines are dropped, any other empty line is refused.
    std::vector<std::string> lines;
    size_t a = 0;
    while (a < bytes.size()) {
        size_t b = bytes.find('\n', a);
        std::string l = bytes.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.empty()) { *err = L"the file is empty"; return false; }

    std::vector<std::wstring> w(lines.size());
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].size() > kPresetMaxLine) {
            *err = LineNo(i + 1) + L": the line is " + std::to_wstring(lines[i].size()) + L" bytes; the limit is " +
                   std::to_wstring(kPresetMaxLine);
            return false;
        }
        if (!Utf8Strict(lines[i], &w[i])) { *err = LineNo(i + 1) + L": not valid UTF-8 text"; return false; }
        if (w[i].find(L'\r') != std::wstring::npos) { *err = LineNo(i + 1) + L": a carriage return inside the line"; return false; }
    }

    // Line 1: the format and its version.
    {
        auto f = Split(w[0], L'\t');
        if (f.size() != 2 || f[0] != FromUtf8(kMagic)) {
            *err = L"line 1: not a Hadron services preset (it must start with HXPRESET, a tab and the format version)";
            return false;
        }
        bool digits = !f[1].empty() && f[1].size() <= 6 && std::all_of(f[1].begin(), f[1].end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        if (!digits || _wtoi(f[1].c_str()) != kPresetFormat) {
            *err = L"line 1: format version '" + Shown(f[1]) + L"' is not supported; this Hadron reads version " +
                   std::to_wstring(kPresetFormat) + L" only";
            return false;
        }
    }
    // Lines 2-5: the header, each key once, in this order.
    static const wchar_t* const kKeys[] = {L"hadron", L"build", L"tab", L"name"};
    for (size_t k = 0; k < 4; k++) {
        size_t n = k + 1;
        if (n >= w.size()) { *err = LineNo(n + 1) + L": missing; the header needs '" + kKeys[k] + L"' here"; return false; }
        size_t t = w[n].find(L'\t');
        std::wstring key = t == std::wstring::npos ? w[n] : w[n].substr(0, t);
        std::wstring val = t == std::wstring::npos ? L"" : w[n].substr(t + 1);
        if (t == std::wstring::npos || key != kKeys[k]) {
            *err = LineNo(n + 1) + L": expected the header line '" + kKeys[k] + L"', a tab and its value; found '" + Shown(w[n]) + L"'";
            return false;
        }
        if (k == 0) {
            if (!VersionText(val, 32, false)) { *err = LineNo(n + 1) + L": the Hadron version '" + Shown(val) + L"' is not a version"; return false; }
            out->hadron = val;
        } else if (k == 1) {
            if (!VersionText(val, 32, true)) { *err = LineNo(n + 1) + L": the Windows build '" + Shown(val) + L"' is not a build number"; return false; }
            out->build = val;
        } else if (k == 2) {
            if (val == L"services") out->tab = TAB_SERVICES;
            else if (val == L"drivers") out->tab = TAB_DRIVERS;
            else { *err = LineNo(n + 1) + L": tab '" + Shown(val) + L"' is neither services nor drivers"; return false; }
        } else {
            std::wstring why = PresetNameProblem(val);
            if (!why.empty()) { *err = LineNo(n + 1) + L": " + why; return false; }
            out->name = val;
        }
    }
    // The items.
    std::set<std::wstring> seen;
    for (size_t n = 5; n < w.size(); n++) {
        const std::wstring& l = w[n];
        if (l.empty()) { *err = LineNo(n + 1) + L": an empty line among the items"; return false; }
        auto f = Split(l, L'\t');
        if (f.size() != 2) {
            *err = LineNo(n + 1) + L": expected a name, one tab and a start type; found " + std::to_wstring(f.size()) + L" field(s)";
            return false;
        }
        const std::wstring& name = f[0];
        if (name.empty()) { *err = LineNo(n + 1) + L": the item has no name"; return false; }
        if (name.size() > kItemMaxName) { *err = LineNo(n + 1) + L": the item name is longer than " + std::to_wstring(kItemMaxName) + L" characters"; return false; }
        if (HasControl(name) || name.find_first_of(L"/\\") != std::wstring::npos || name.front() == L' ' || name.back() == L' ') {
            *err = LineNo(n + 1) + L": '" + Shown(name) + L"' is not a service or driver name (control character, slash, or a space at an end)";
            return false;
        }
        StartCfg cfg;
        if (!StartFromWord(f[1], &cfg)) {
            *err = LineNo(n + 1) + L": start type '" + Shown(f[1]) + L"' is not one of auto, auto-delayed, manual, disabled" +
                   (f[1] == L"boot" || f[1] == L"system" ? L" (Hadron never sets Boot or System start)" : L"");
            return false;
        }
        if (cfg.delayed == 1 && out->tab == TAB_DRIVERS) {
            *err = LineNo(n + 1) + L": auto-delayed is for services; a driver can't be delayed";
            return false;
        }
        if (!seen.insert(Lower(name)).second) { *err = LineNo(n + 1) + L": " + name + L" is listed twice"; return false; }
        if (out->items.size() >= kPresetMaxItems) {
            *err = LineNo(n + 1) + L": more than " + std::to_wstring(kPresetMaxItems) + L" items";
            return false;
        }
        out->items.push_back({name, cfg});
    }
    if (out->items.empty()) { *err = L"the preset lists no items (expected one name<TAB>start-type line each, after line 5)"; return false; }
    return true;
}

// ---- data folder -------------------------------------------------------------------------------------------
std::vector<CustomPreset> LoadCustomPresets(const Store& st, std::vector<std::wstring>* problems) {
    std::vector<CustomPreset> v;
    if (!st.Valid() || !st.Exists(L"presets")) return v;
    std::set<std::wstring> removed;
    for (auto& f : st.List(L"presets\\removed-*.txt")) {
        std::string text;
        std::wstring err;
        if (!st.Read(L"presets\\" + f, &text, &err)) { if (problems) problems->push_back(err); continue; }
        auto lines = Split(FromUtf8(text), L'\n');
        if (lines.size() < 2 || ToUtf8(lines[0]) != kRemovedMagic) {
            if (problems) problems->push_back(L"presets\\" + f + L": not a removal record, ignored");
            continue;
        }
        removed.insert(Lower(lines[1]));
    }
    for (auto& f : st.List(L"presets\\*.txt")) {
        if (f.rfind(L"removed-", 0) == 0 || removed.count(Lower(f))) continue;
        std::string text;
        std::wstring err;
        CustomPreset p;
        if (!st.Read(L"presets\\" + f, &text, &err) || !ParsePreset(text, &p, &err)) {
            if (problems) problems->push_back(L"custom preset presets\\" + f + L" skipped: " + err);
            continue;
        }
        p.file = f;
        p.saved = StampWords(f.substr(0, f.size() - 4));
        v.push_back(std::move(p));
    }
    std::stable_sort(v.begin(), v.end(), [](const CustomPreset& a, const CustomPreset& b) { return Lower(a.name) < Lower(b.name); });
    return v;
}

namespace {
std::wstring FreeName(const Store& st, const std::wstring& prefix) {
    std::wstring base = st.fixedStamp.empty() ? UtcStamp() : st.fixedStamp;
    for (int n = 0; n < 1000; n++) {
        std::wstring f = prefix + (n == 0 ? base : base + L"-" + std::to_wstring(n)) + L".txt";
        if (!st.Exists(L"presets\\" + f)) return f;
    }
    return L"";
}
}  // namespace

bool SaveCustomPreset(Store& st, CustomPreset* p, const std::vector<CustomPreset>& existing, std::wstring* err) {
    std::wstring why = PresetNameProblem(p->name);
    if (!why.empty()) { *err = L"Not saved: " + why + L"."; return false; }
    for (auto& e : existing)
        if (IEq(e.name, p->name)) { *err = L"Not saved: a custom preset named \"" + e.name + L"\" exists. Pick another name, or remove that one first."; return false; }
    if (p->items.empty()) { *err = L"Not saved: no ticked rows to save."; return false; }
    std::string text = WritePreset(*p);
    CustomPreset check;
    if (!ParsePreset(text, &check, err)) { *err = L"Not saved: " + *err; return false; }   // what Hadron writes, it must read back
    if (!st.MakeDir(L"presets", err)) return false;
    std::wstring f = FreeName(st, L"");
    if (f.empty()) { *err = L"Not saved: no free file name in presets"; return false; }
    if (!st.WriteNew(L"presets\\" + f, text, err)) return false;
    p->file = f;
    p->saved = StampWords(f.substr(0, f.size() - 4));
    return true;
}

bool RemoveCustomPreset(Store& st, const CustomPreset& p, std::wstring* err) {
    if (p.file.empty()) { *err = L"That preset is not saved in the data folder."; return false; }
    if (!st.MakeDir(L"presets", err)) return false;
    std::wstring f = FreeName(st, L"removed-");
    if (f.empty()) { *err = L"No free file name in presets"; return false; }
    // The preset's own file stays (nothing is deleted); this record hides it from the list.
    return st.WriteNew(L"presets\\" + f, std::string(kRemovedMagic) + "\n" + ToUtf8(p.file) + "\n", err);
}

// ---- user files ----------------------------------------------------------------------------------------------
bool ExportPresetFile(const std::wstring& path, const CustomPreset& p, bool* exists, std::wstring* err) {
    *exists = false;
    if (ImpersonationFlag::Active()) { *err = L"refused: a Hadron file write while impersonating"; return false; }
    std::string text = WritePreset(p);
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) { *exists = true; *err = path + L" exists. Hadron never replaces a file: export again under a new name."; }
        else *err = L"Could not create " + path + L": " + sys::ErrorText(e);
        return false;
    }
    DWORD put = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    DWORD e = ok ? 0 : GetLastError();
    if (ok && !FlushFileBuffers(h)) { ok = false; e = GetLastError(); }
    if (!CloseHandle(h) && ok) { ok = false; e = GetLastError(); }
    if (!ok) { *err = L"Could not write " + path + L": " + sys::ErrorText(e) + L" (the partial file is left as it is)"; return false; }
    return true;
}

bool ImportPresetFile(const std::wstring& path, CustomPreset* out, std::wstring* err) {
    WIN32_FILE_ATTRIBUTE_DATA ad{};
    if (!GetFileAttributesExW(sys::ToExtended(path).c_str(), GetFileExInfoStandard, &ad)) {
        *err = L"Could not read " + path + L": " + sys::ErrorText(GetLastError());
        return false;
    }
    if (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { *err = path + L" is a folder"; return false; }
    if (ad.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) {
        *err = path + L" is a cloud placeholder (not on this PC yet). Hadron doesn't download it: make it available offline first.";
        return false;
    }
    uint64_t size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
    if (size > kPresetMaxFile) { *err = path + L" is larger than " + std::to_wstring(kPresetMaxFile / 1024) + L" KiB; a preset file is never that big"; return false; }
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = L"Could not open " + path + L": " + sys::ErrorText(GetLastError()); return false; }
    std::string data(kPresetMaxFile + 1, '\0');
    DWORD got = 0;
    BOOL ok = ReadFile(h, &data[0], (DWORD)data.size(), &got, nullptr);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) { *err = L"Could not read " + path + L": " + sys::ErrorText(e); return false; }
    data.resize(got);
    if (!ParsePreset(data, out, err)) return false;
    out->file.clear();
    out->saved.clear();
    return true;
}

}  // namespace hx::services
