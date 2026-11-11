// modules/bios/snapshot.cpp — see snapshot.h.
#include "modules/bios/snapshot.h"
#include "modules/bios/zip.h"
#include "core/sys/sys.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace hx::bios {

// ---- CSV ----------------------------------------------------------------------------------------
std::vector<std::vector<std::wstring>> ParseCsv(const std::wstring& t) {
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> row;
    std::wstring field;
    bool inQuotes = false, any = false;
    size_t i = 0;
    if (!t.empty() && t[0] == 0xFEFF) i = 1;   // BOM (Windows PowerShell 5.1 Export-Csv)
    auto endRow = [&] {
        row.push_back(field); field.clear();
        rows.push_back(std::move(row)); row.clear();
        any = false;
    };
    for (; i < t.size(); i++) {
        wchar_t c = t[i];
        if (inQuotes) {
            if (c == L'"') {
                if (i + 1 < t.size() && t[i + 1] == L'"') { field += L'"'; i++; }
                else inQuotes = false;
            } else {
                field += c;   // commas, CR and LF are data inside quotes
            }
            continue;
        }
        if (c == L'"') { inQuotes = true; any = true; }
        else if (c == L',') { row.push_back(field); field.clear(); any = true; }
        else if (c == L'\r') {
            if (i + 1 < t.size() && t[i + 1] == L'\n') i++;
            endRow();
        } else if (c == L'\n') endRow();
        else { field += c; any = true; }
    }
    if (any || !field.empty() || !row.empty()) endRow();
    return rows;
}

std::wstring CsvQuote(const std::wstring& f) {
    std::wstring o = L"\"";
    for (wchar_t c : f) { if (c == L'"') o += L'"'; o += c; }
    return o + L"\"";
}

// ---- naming -------------------------------------------------------------------------------------
std::wstring SanitiseName(const std::wstring& name) {
    std::wstring o;
    for (wchar_t c : name) {
        bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                  c == L'_' || c == L'-' || c == L'.';
        o += ok ? c : L'_';
    }
    if (o.size() > 80) o.resize(80);
    return o;
}

std::wstring SnapFileName(int index, const std::wstring& name, const std::wstring& guid) {
    wchar_t b[16];
    swprintf(b, 16, L"%04d_", index);
    std::wstring g = guid;
    if (!g.empty() && g.front() == L'{') g.erase(0, 1);
    if (!g.empty() && g.back() == L'}') g.pop_back();
    return b + SanitiseName(name) + L"__" + Lower(g) + L".bin";
}

// ---- file sources -------------------------------------------------------------------------------
namespace {

bool ReadWholeFile(const std::wstring& path, std::string* out, size_t cap, std::wstring* err) {
    out->clear();
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = sys::ErrorText(GetLastError()); return false; }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { DWORD e = GetLastError(); CloseHandle(h); if (err) *err = sys::ErrorText(e); return false; }
    if ((unsigned long long)sz.QuadPart > cap) {
        CloseHandle(h);
        if (err) *err = L"file is larger than " + std::to_wstring(cap >> 20) + L" MB";
        return false;
    }
    out->resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out->size()) {
        DWORD chunk = (DWORD)std::min<size_t>(out->size() - done, 1u << 20), got = 0;
        if (!ReadFile(h, &(*out)[done], chunk, &got, nullptr)) {
            DWORD e = GetLastError(); CloseHandle(h); if (err) *err = sys::ErrorText(e); return false;
        }
        if (got == 0) break;
        done += got;
    }
    CloseHandle(h);
    out->resize(done);
    return true;
}

struct FolderSource : FileSource {
    std::wstring root;
    explicit FolderSource(std::wstring r) : root(std::move(r)) {}
    bool Read(const std::wstring& rel, std::string* out, size_t cap, std::wstring* err) override {
        std::wstring p = root;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
        for (wchar_t c : rel) p += (c == L'/') ? L'\\' : c;
        return ReadWholeFile(p, out, cap, err);
    }
};

struct ZipSource : FileSource {
    ZipReader zip;
    std::wstring prefix;   // folder inside the zip that holds index.csv ("" at the root)
    bool Read(const std::wstring& rel, std::string* out, size_t cap, std::wstring* err) override {
        const ZipEntry* e = zip.Find(prefix + rel);
        if (!e) { if (err) *err = L"not in the zip"; return false; }
        std::vector<uint8_t> d;
        if (!zip.Extract(*e, &d, cap, err)) return false;
        out->assign(d.begin(), d.end());
        return true;
    }
};

bool EndsWithI(const std::wstring& s, const std::wstring& suf) {
    return s.size() >= suf.size() && ICmp(s.substr(s.size() - suf.size()), suf) == 0;
}

// The File column names a file in vars\. Anything that could escape that folder is refused.
bool SafeFileName(const std::wstring& f) {
    if (f.empty() || f == L"." || f == L"..") return false;
    for (wchar_t c : f) if (c == L'\\' || c == L'/' || c == L':' || c < 0x20) return false;
    // Reserved DOS device names with any extension ("NUL.bin", "com1.txt") open a device, not a file.
    std::wstring base = Lower(f.substr(0, f.find(L'.')));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul") return false;
    if (base.size() == 4 && (base.compare(0, 3, L"com") == 0 || base.compare(0, 3, L"lpt") == 0) &&
        base[3] >= L'1' && base[3] <= L'9')
        return false;
    return true;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\t')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == 0xFEFF)) a++;
    return s.substr(a);
}

}  // namespace

std::wstring SnapLoad::SkipSummary() const {
    if (!skipped) return L"";
    std::wstring s = std::to_wstring(skipped) + L" row" + (skipped == 1 ? L"" : L"s") + L" skipped: ";
    bool first = true;
    for (auto& r : skipReasons) {
        if (!first) s += L"; ";
        s += std::to_wstring(r.second) + L" " + r.first;
        first = false;
    }
    return s;
}

bool LoadSnapshotFrom(FileSource& src, const std::wstring& label, SnapLoad* out) {
    *out = SnapLoad{};
    out->label = label;
    std::string raw;
    std::wstring err;
    if (!src.Read(L"index.csv", &raw, kMaxSnapText, &err)) { out->error = L"No readable index.csv: " + err; return false; }
    size_t total = 0;
    auto rows = ParseCsv(FromUtf8(raw));
    if (rows.empty()) { out->error = L"index.csv is empty."; return false; }
    // Map columns by header name; fall back to the legacy order when there is no header row.
    int cName = 0, cGuid = 1, cErr = 4, cFile = 5;
    size_t first = 0;
    if (!rows[0].empty() && ICmp(Trim(rows[0][0]), L"Name") == 0) {
        first = 1;
        cName = cGuid = cErr = cFile = -1;
        for (int i = 0; i < (int)rows[0].size(); i++) {
            std::wstring h = Trim(rows[0][i]);
            if (!ICmp(h, L"Name")) cName = i;
            else if (!ICmp(h, L"Guid")) cGuid = i;
            else if (!ICmp(h, L"Error")) cErr = i;
            else if (!ICmp(h, L"File")) cFile = i;
        }
        if (cName < 0 || cGuid < 0 || cFile < 0) {
            out->error = L"index.csv has no Name, Guid or File column.";
            return false;
        }
    }
    int need = std::max(cName, std::max(cGuid, cFile)) + 1;
    struct Cached { bool ok = false; std::vector<uint8_t> data; };
    std::map<std::wstring, Cached> cache;
    auto skip = [&](const std::wstring& why) { out->skipped++; out->skipReasons[why]++; };
    for (size_t r = first; r < rows.size(); r++) {
        auto& c = rows[r];
        if (c.size() == 1 && c[0].empty()) continue;   // blank line
        out->rows++;
        if ((int)c.size() < need) { skip(L"with too few columns"); continue; }
        std::wstring file = c[cFile];
        if (file.empty()) {
            std::wstring e = (cErr >= 0 && cErr < (int)c.size()) ? c[cErr] : L"";
            skip(e.empty() || e == L"0" ? L"with no data file" : L"not read when saved (error " + e + L")");
            continue;
        }
        if (!SafeFileName(file)) { skip(L"with an unsafe file name"); continue; }
        // One read per file name, however many rows point at it (a hostile index can repeat one).
        auto hit = cache.find(Lower(file));
        if (hit == cache.end()) {
            std::string data;
            std::wstring why;
            Cached cd;
            cd.ok = src.Read(L"vars/" + file, &data, kMaxSnapVar, &why);
            if (cd.ok) cd.data.assign(data.begin(), data.end());
            hit = cache.emplace(Lower(file), std::move(cd)).first;
        }
        if (!hit->second.ok) { skip(L"whose vars file can't be read"); continue; }
        // Count every copy that goes into out->vars, not just each file read: a hostile index can
        // point many rows at one file, and each row stores its own copy.
        total += hit->second.data.size();
        if (total > kMaxSnapTotal) {
            out->error = L"The snapshot holds more than " + std::to_wstring(kMaxSnapTotal >> 20) +
                         L" MB of variable data; refusing to load it.";
            out->vars.clear();
            return false;
        }
        VarKey key = MakeVarKey(c[cName], c[cGuid]);
        if (out->vars.count(key)) skip(L"duplicate (the later row is used)");
        else out->loaded++;
        out->vars[key] = hit->second.data;
    }
    std::string sys;
    if (src.Read(L"system.txt", &sys, kMaxSnapText, &err)) {
        out->hasSystem = true;
        std::wstring t = FromUtf8(sys);
        size_t a = 0;
        while (a <= t.size()) {
            size_t b = t.find(L'\n', a);
            if (b == std::wstring::npos) b = t.size();
            std::wstring line = Trim(t.substr(a, b - a));
            if (line.rfind(L"Board:", 0) == 0) out->sysBoard = Trim(line.substr(6));
            else if (line.rfind(L"BIOS:", 0) == 0) out->sysBios = Trim(line.substr(5));
            a = b + 1;
        }
    }
    if (out->vars.empty()) {
        out->error = L"No variables could be loaded from the snapshot.";
        std::wstring s = out->SkipSummary();
        if (!s.empty()) out->error += L" " + s + L".";
        return false;
    }
    out->ok = true;
    return true;
}

bool LoadSnapshotZip(std::vector<uint8_t> bytes, const std::wstring& label, SnapLoad* out) {
    *out = SnapLoad{};
    out->label = label;
    ZipSource zs;
    std::wstring err;
    if (!zs.zip.Open(std::move(bytes), &err)) { out->error = label + L": " + err; return false; }
    // index.csv at the root (Compress-Archive <folder>\*) or inside one folder (a zipped folder).
    const ZipEntry* best = nullptr;
    for (auto& e : zs.zip.Entries()) {
        bool hit = !ICmp(e.name, L"index.csv") || EndsWithI(e.name, L"/index.csv");
        if (hit && (!best || e.name.size() < best->name.size())) best = &e;
    }
    if (!best) { out->error = label + L" has no index.csv."; return false; }
    zs.prefix = best->name.substr(0, best->name.size() - 9);
    return LoadSnapshotFrom(zs, label, out);
}

bool LoadSnapshot(const std::wstring& path, SnapLoad* out) {
    *out = SnapLoad{};
    out->label = sys::ToShell(path);
    DWORD a = GetFileAttributesW(sys::ToExtended(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { out->error = L"Can't open " + out->label + L": " + sys::ErrorText(GetLastError()); return false; }
    std::wstring shown = sys::ToShell(sys::ToExtended(path));
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) && EndsWithI(path, L".zip")) {
        std::string raw;
        std::wstring err;
        if (!ReadWholeFile(path, &raw, kMaxSnapTotal, &err)) { out->error = L"Can't read " + shown + L": " + err; return false; }
        return LoadSnapshotZip(std::vector<uint8_t>(raw.begin(), raw.end()), shown, out);
    }
    std::wstring dir = path;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {   // a file inside the dump folder: use its folder (legacy)
        size_t k = dir.find_last_of(L"\\/");
        dir = k == std::wstring::npos ? L"." : dir.substr(0, k);
        shown = sys::ToShell(sys::ToExtended(dir));
    }
    FolderSource fs(sys::ToExtended(dir));   // absolute \\?\ form, whatever the caller passed
    return LoadSnapshotFrom(fs, shown, out);
}

// ---- save ---------------------------------------------------------------------------------------
namespace {
bool WriteNew(const std::wstring& path, const std::string& data, std::wstring* err) {
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = sys::ToShell(path) + L": " + sys::ErrorText(GetLastError()); return false; }
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - done, 1u << 20), put = 0;
        if (!WriteFile(h, data.data() + done, chunk, &put, nullptr) || put == 0) {
            if (err) *err = sys::ToShell(path) + L": " + sys::ErrorText(GetLastError());
            ok = false;
            break;
        }
        done += put;
    }
    if (!CloseHandle(h) && ok) { if (err) *err = sys::ToShell(path) + L": " + sys::ErrorText(GetLastError()); ok = false; }
    return ok;
}

std::wstring CpuName() {
    wchar_t buf[256] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return L"unknown";
    std::wstring s = buf;
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}
}  // namespace

bool SaveSnapshot(const std::wstring& parent, const std::vector<FwVar>& vars, const SmbiosInfo& smb,
                  SaveResult* out) {
    *out = SaveResult{};
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u_%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    std::wstring base = parent;
    if (!base.empty() && base.back() != L'\\') base += L'\\';
    std::wstring folder;
    for (int n = 1; n < 1000; n++) {
        std::wstring cand = base + L"UEFI_Dump_" + stamp + (n > 1 ? L"-" + std::to_wstring(n) : L"");
        if (CreateDirectoryW(sys::ToExtended(cand).c_str(), nullptr)) { folder = cand; break; }
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) { out->error = L"Can't create " + sys::ToShell(cand) + L": " + sys::ErrorText(e); return false; }
    }
    if (folder.empty()) { out->error = L"Too many snapshots with the same timestamp in " + sys::ToShell(parent); return false; }
    out->folder = sys::ToShell(sys::ToExtended(folder));
    if (!CreateDirectoryW(sys::ToExtended(folder + L"\\vars").c_str(), nullptr)) {
        out->error = L"Can't create the vars folder: " + sys::ErrorText(GetLastError());
        return false;
    }
    std::wstring csv = L"\"Name\",\"Guid\",\"Size\",\"Attributes\",\"Error\",\"File\"\r\n";
    int idx = 0;
    for (auto& v : vars) {
        out->found++;
        idx++;
        std::wstring file;
        if (v.err == 0 && !v.data.empty()) {
            file = SnapFileName(idx, v.name, v.guid);
            std::wstring err;
            if (!WriteNew(folder + L"\\vars\\" + file, std::string(v.data.begin(), v.data.end()), &err)) {
                out->error = L"Writing a variable failed: " + err;
                return false;
            }
            out->read++;
        }
        wchar_t attr[16];
        swprintf(attr, 16, L"0x%lX", (unsigned long)(v.err == 0 ? v.attr : 0));
        csv += CsvQuote(v.name) + L"," + CsvQuote(L"{" + v.guid + L"}") + L"," +
               CsvQuote(std::to_wstring(v.err == 0 ? v.data.size() : 0)) + L"," + CsvQuote(attr) + L"," +
               CsvQuote(std::to_wstring(v.err)) + L"," + CsvQuote(file) + L"\r\n";
    }
    std::wstring err;
    if (!WriteNew(folder + L"\\index.csv", "\xEF\xBB\xBF" + ToUtf8(csv), &err)) { out->error = err; return false; }
    wchar_t when[32];
    swprintf(when, 32, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    std::wstring txt = std::wstring(L"Dumped: ") + when + L"\r\n" +
                       L"Board: " + smb.boardMaker + L" " + smb.boardProduct + L"\r\n" +
                       L"BIOS: " + smb.biosVendor + L" " + smb.biosVersion + L" " + smb.biosDate + L"\r\n" +
                       L"CPU: " + CpuName() + L"\r\n" +
                       L"Variables: " + std::to_wstring(out->found) + L" found, " + std::to_wstring(out->read) +
                       L" read\r\n" + L"Saved by: Hadron BIOS module (read-only)\r\n";
    if (!WriteNew(folder + L"\\system.txt", "\xEF\xBB\xBF" + ToUtf8(txt), &err)) { out->error = err; return false; }
    out->ok = true;
    return true;
}

}  // namespace hx::bios
