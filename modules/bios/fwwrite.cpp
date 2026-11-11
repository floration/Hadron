// modules/bios/fwwrite.cpp — see fwwrite.h. Hard rule 2: this is the only file in the suite that may
// call the UEFI variable write API (RealBackend::Write), and CI enforces that.
#include "modules/bios/fwwrite.h"
#include "core/sys/log.h"
#include "core/sys/sys.h"
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace hx::bios {

namespace {

std::wstring Braced(const std::wstring& guid) {
    if (!guid.empty() && guid.front() == L'{') return guid;
    return L"{" + guid + L"}";
}

std::wstring Hex(unsigned long long v, int digits = 0) {
    wchar_t b[32];
    if (digits) swprintf(b, 32, L"0x%0*llX", digits, v);
    else swprintf(b, 32, L"0x%llX", v);
    return b;
}

void LogLine(LogLevel lv, const std::wstring& s) { sys::Write(lv, L"bios write: " + s); }

std::wstring AttrProblem(DWORD attr) {
    if ((attr & kAttrRequired) != kAttrRequired) {
        std::wstring m;
        if (!(attr & kAttrNV)) m += L" non-volatile (0x1)";
        if (!(attr & kAttrBS)) m += L" boot-service (0x2)";
        if (!(attr & kAttrRT)) m += L" runtime (0x4)";
        return L"its attributes " + Hex(attr) + L" lack" + m + L"; only NV|BS|RT setup variables are written";
    }
    DWORD extra = attr & ~kAttrRequired;
    if (!extra) return L"";
    std::wstring m;
    if (extra & 0x8) m += L" hardware-error-record (0x8)";
    if (extra & 0x10) m += L" authenticated (0x10)";
    if (extra & 0x20) m += L" time-based authenticated (0x20)";
    if (extra & 0x40) m += L" append-write (0x40)";
    if (extra & 0x80) m += L" enhanced-authenticated (0x80)";
    if (extra & ~0xF8u) m += L" unknown (" + Hex(extra & ~0xF8u) + L")";
    return L"its attributes " + Hex(attr) + L" include" + m + L"; such variables are never written";
}

// Write errors, worded for writes (VarErrorText words 5 as a refused read).
std::wstring WriteErrorText(DWORD e) {
    switch (e) {
        case ERROR_ACCESS_DENIED:
            return L"access denied (5): the firmware locked this variable, or this is a test run where writes are off";
        case ERROR_WRITE_PROTECT: return L"write-protected (19): the firmware refuses writes to this variable";
        case ERROR_PRIVILEGE_NOT_HELD: return L"privilege missing (1314): SeSystemEnvironmentPrivilege isn't enabled";
        case ERROR_INVALID_PARAMETER: return L"invalid parameter (87): refused before reaching the firmware";
        case ERROR_ENVVAR_NOT_FOUND: return L"not found (203): the variable doesn't exist";
    }
    return sys::ErrorText(e) + L" (" + std::to_wstring(e) + L")";
}

std::wstring SettingFlags(const Setting& s) {
    if (s.hidden) return L"Hidden in the BIOS menu";
    if (s.cond) return L"Shown only when another setting allows it; may have no effect";
    return L"";
}

bool Overlaps(int a0, int aw, int b0, int bw) { return a0 < b0 + bw && b0 < a0 + aw; }

// Other mapped settings stored in [off, off+w) of this variable, with what the plan does to them.
std::wstring SharedSettings(const Db& db, const PlanVar& pv, int self, int off, int w) {
    std::wstring out;
    int n = 0, more = 0;
    for (int i = 0; i < (int)db.S.size(); i++) {
        const Setting& o = db.S[i];
        if (i == self || o.var != pv.name || Lower(o.guid) != pv.guid) continue;
        if (!Overlaps(o.offset, o.width, off, w)) continue;
        long long a = 0, b = 0;
        bool ha = DecodeValue(pv.oldData, o.offset, o.width, &a), hb = DecodeValue(pv.newData, o.offset, o.width, &b);
        if (n >= 4) { more++; continue; }
        if (!out.empty()) out += L"; ";
        out += o.prompt + L" (" + (ha ? ValText(o, a) : L"?") + L" → " + (hb ? ValText(o, b) : L"?") + L")";
        n++;
    }
    if (more) out += L"; and " + std::to_wstring(more) + L" more";
    return out;
}

const FwVar* FindLive(const std::vector<FwVar>& live, const VarKey& k) {
    for (auto& v : live) if (MakeVarKey(v.name, v.guid) == k) return &v;
    return nullptr;
}

std::wstring GateProblem(const PlanGate& g) {
    if (!g.switchOn) return L"\"Allow BIOS writes\" is off.";
    if (!g.live) return L"Values aren't live (snapshot mode, or nothing was read from this PC).";
    if (!g.guardMatched) return L"The board/BIOS guard didn't match this PC (or couldn't run), so the map isn't verified.";
    return L"";
}

// Bytes of a and b (same size) that differ, as (offset, length) runs.
std::vector<std::pair<int, int>> DiffRanges(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<std::pair<int, int>> r;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n;) {
        if (a[i] == b[i]) { i++; continue; }
        size_t j = i;
        while (j < n && a[j] != b[j]) j++;
        r.push_back({(int)i, (int)(j - i)});
        i = j;
    }
    return r;
}

bool OnlyInRanges(const PlanVar& pv) {
    if (pv.oldData.size() != pv.newData.size()) return false;
    for (size_t i = 0; i < pv.oldData.size(); i++) {
        if (pv.oldData[i] == pv.newData[i]) continue;
        bool in = false;
        for (auto& r : pv.ranges) if ((int)i >= r.first && (int)i < r.first + r.second) { in = true; break; }
        if (!in) return false;
    }
    return true;
}

std::vector<uint8_t> Slice(const std::vector<uint8_t>& d, int off, int w) {
    if (off < 0 || w <= 0 || (size_t)off + (size_t)w > d.size()) return {};
    return std::vector<uint8_t>(d.begin() + off, d.begin() + off + w);
}

// journal.txt: opened once with CREATE_NEW; every line is flushed before the next step.
class Journal {
public:
    ~Journal() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    bool Open(const std::wstring& path, SECURITY_ATTRIBUTES* sa, std::wstring* err) {
        h_ = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, sa, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { *err = L"Can't create the journal: " + sys::ErrorText(GetLastError()); return false; }
        return true;
    }
    bool Line(const std::wstring& s, std::wstring* err) {
        std::string u = ToUtf8(s) + "\r\n";
        DWORD put = 0;
        if (!WriteFile(h_, u.data(), (DWORD)u.size(), &put, nullptr) || put != u.size() || !FlushFileBuffers(h_)) {
            *err = L"Writing the journal failed: " + sys::ErrorText(GetLastError());
            return false;
        }
        return true;
    }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

bool EnsureDir(const std::wstring& path, int depth = 0) {
    if (depth > 64 || path.empty()) return false;
    if (CreateDirectoryW(sys::ToExtended(path).c_str(), nullptr)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS) return true;
    if (e != ERROR_PATH_NOT_FOUND) { SetLastError(e); return false; }
    size_t k = path.find_last_of(L"\\/");
    if (k == std::wstring::npos || k == 0) { SetLastError(e); return false; }
    if (!EnsureDir(path.substr(0, k), depth + 1)) return false;
    if (CreateDirectoryW(sys::ToExtended(path).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// Holds the write-lock mutex from Take() until destruction (released whenever it was acquired, the
// abandoned case included).
// An owned Win32 handle (INVALID_HANDLE_VALUE / null = none).
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { Reset(); }
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { Reset(); h_ = o.h_; o.h_ = INVALID_HANDLE_VALUE; }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return h_; }
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    void Reset() { if (Ok()) CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

bool IsWellKnown(PSID sid, WELL_KNOWN_SID_TYPE type) {
    BYTE buf[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof(buf);
    if (!CreateWellKnownSid(type, nullptr, buf, &n)) return false;
    return EqualSid(sid, buf) != 0;
}
bool IsAdminOrSystem(PSID sid) {
    return sid && IsValidSid(sid) && (IsWellKnown(sid, WinBuiltinAdministratorsSid) || IsWellKnown(sid, WinLocalSystemSid));
}
std::wstring SidText(PSID sid) {
    LPWSTR s = nullptr;
    if (!sid || !ConvertSidToStringSidW(sid, &s) || !s) return L"(unreadable SID)";
    std::wstring r = s;
    LocalFree(s);
    return r;
}

// An SDDL string as a self-relative descriptor plus the SECURITY_ATTRIBUTES that carry it.
class SddlSd {
public:
    ~SddlSd() { if (sd_) LocalFree(sd_); }
    bool Make(const wchar_t* sddl, std::wstring* err) {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd_, nullptr)) {
            sd_ = nullptr;
            *err = L"Couldn't build a security descriptor: " + sys::ErrorText(GetLastError());
            return false;
        }
        sa_.nLength = sizeof(sa_);
        sa_.lpSecurityDescriptor = sd_;
        sa_.bInheritHandle = FALSE;
        return true;
    }
    SECURITY_ATTRIBUTES* Sa() { return &sa_; }
private:
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    SECURITY_ATTRIBUTES sa_{};
};
// kBackupSddl: every backup folder and file.
class BackupSd : public SddlSd {
public:
    bool Make(std::wstring* err) { return SddlSd::Make(kBackupSddl, err); }
};

// Holds the write-lock mutex from Take() until destruction (released whenever it was acquired, the
// abandoned case included). Created with kLockSddl; an existing one must be owned by Administrators or
// SYSTEM, so a non-admin process can't pre-create it and decide who writes.
class WriteLock {
public:
    ~WriteLock() {
        if (owned_) ReleaseMutex(h_);
        if (h_) CloseHandle(h_);
    }
    bool Take(const std::wstring& name, std::wstring* err) {
        SddlSd sd;
        std::wstring e;
        if (!sd.Make(kLockSddl, &e)) { *err = e + L" Nothing was written."; return false; }
        h_ = CreateMutexW(sd.Sa(), FALSE, name.c_str());
        DWORD ce = GetLastError();
        if (!h_) {
            *err = L"Couldn't create or open the BIOS write lock " + name + L": " + sys::ErrorText(ce) + L" Nothing was written.";
            return false;
        }
        if (ce == ERROR_ALREADY_EXISTS) {
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR psd = nullptr;
            DWORD ge = GetSecurityInfo(h_, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &psd);
            bool admin = ge == ERROR_SUCCESS && IsAdminOrSystem(owner);
            std::wstring who = ge == ERROR_SUCCESS ? SidText(owner) : std::wstring();
            if (psd) LocalFree(psd);
            if (ge != ERROR_SUCCESS) {
                *err = L"Couldn't read the owner of the BIOS write lock: " + sys::ErrorText(ge) + L" Nothing was written.";
                return false;
            }
            if (!admin) {
                *err = L"The BIOS write lock was created by a non-admin process (owner " + who + L"). Nothing was written.";
                return false;
            }
        }
        DWORD w = WaitForSingleObject(h_, 0);
        if (w == WAIT_OBJECT_0) { owned_ = true; return true; }
        if (w == WAIT_ABANDONED) {
            // Its last holder ended while holding it: possibly a write stopped between BEGIN and OK. Don't
            // write over that; the "didn't finish" banner offers that session's backup if its journal says so.
            owned_ = true;
            *err = L"The BIOS write lock was left by a process that ended while holding it (a Hadron window may have "
                   L"stopped in the middle of a BIOS write). Nothing was written. Check the newest backup.";
            return false;
        }
        if (w == WAIT_TIMEOUT) {
            *err = L"The BIOS write lock is held by another process (another Hadron window may be writing BIOS settings). "
                   L"Nothing was written.";
            return false;
        }
        *err = L"Couldn't take the BIOS write lock: " + sys::ErrorText(GetLastError()) + L" Nothing was written.";
        return false;
    }
private:
    HANDLE h_ = nullptr;
    bool owned_ = false;
};

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}
std::wstring ParentOf(const std::wstring& p) {
    size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos ? std::wstring() : p.substr(0, k);
}

bool HandleFinalPath(HANDLE h, std::wstring* out, DWORD* err) {
    std::vector<wchar_t> buf(512);
    DWORD n = 0;
    for (;;) {
        n = GetFinalPathNameByHandleW(h, buf.data(), (DWORD)buf.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0 || n < buf.size()) break;
        buf.resize((size_t)n + 1);
    }
    if (n == 0) { *err = GetLastError(); return false; }
    *out = std::wstring(buf.data(), n);
    while (out->size() > 1 && out->back() == L'\\') out->pop_back();
    return true;
}

// Why OpenChecked refused.
enum class OpenFail { None, Open, Kind, Link, Place, Trust };

// Opens path once, never following a link at its last component (FILE_FLAG_OPEN_REPARSE_POINT; plus
// BACKUP_SEMANTICS for a folder), sharing read only (no write or delete sharing while it is held), and
// checks the handle itself: the expected kind and not a reparse point; its final path equals
// expectFinal (when given) and its parent's final path equals expectParent (when given); and trust.
bool OpenChecked(const std::wstring& path, bool dir, const std::wstring& expectFinal, const std::wstring& expectParent,
                 const TrustFn& trust, Handle* out, std::wstring* finalOut, std::wstring* why, OpenFail* fail = nullptr,
                 DWORD* openErr = nullptr) {
    OpenFail dummy;
    if (!fail) fail = &dummy;
    *fail = OpenFail::None;
    std::wstring shown = sys::ToShell(sys::ToExtended(path));
    DWORD access = dir ? (READ_CONTROL | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE) : GENERIC_READ;
    Handle h(CreateFileW(sys::ToExtended(path).c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | (dir ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
    if (!h.Ok()) {
        DWORD e = GetLastError();
        if (openErr) *openErr = e;
        *fail = OpenFail::Open;
        *why = L"Can't open " + shown + L": " + sys::ErrorText(e);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h.Get(), &fi)) {
        *fail = OpenFail::Open;
        *why = L"Can't read " + shown + L": " + sys::ErrorText(GetLastError());
        return false;
    }
    if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        *fail = OpenFail::Link;
        *why = shown + L" is a link (reparse point), so it isn't trusted.";
        return false;
    }
    if (((fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != dir) {
        *fail = OpenFail::Kind;
        *why = shown + (dir ? L" is a file, not a folder." : L" is a folder, not a file.");
        return false;
    }
    std::wstring fin;
    DWORD fe = 0;
    if (!HandleFinalPath(h.Get(), &fin, &fe)) {
        *fail = OpenFail::Open;
        *why = L"Can't resolve " + shown + L": " + sys::ErrorText(fe);
        return false;
    }
    if (!expectFinal.empty() && !SamePath(fin, expectFinal)) {
        *fail = OpenFail::Place;
        *why = shown + L" resolves to " + sys::ToShell(fin) + L", not " + sys::ToShell(expectFinal) + L".";
        return false;
    }
    if (!expectParent.empty() && !SamePath(ParentOf(fin), expectParent)) {
        *fail = OpenFail::Place;
        *why = sys::ToShell(fin) + L" isn't directly inside " + sys::ToShell(expectParent) + L".";
        return false;
    }
    std::wstring tw;
    if (!trust(h.Get(), fin, &tw)) {
        *fail = OpenFail::Trust;
        *why = L"This backup folder can be changed without admin rights, so it isn't trusted (" + sys::ToShell(fin) + L": " + tw + L").";
        return false;
    }
    *out = std::move(h);
    if (finalOut) *finalOut = fin;
    return true;
}

// Reads every byte through the handle (refusing anything over cap).
bool ReadAllFrom(HANDLE h, size_t cap, std::string* out, std::wstring* why) {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { *why = sys::ErrorText(GetLastError()); return false; }
    if (sz.QuadPart < 0 || (unsigned long long)sz.QuadPart > cap) { *why = L"the file is too large"; return false; }
    out->assign((size_t)sz.QuadPart, '\0');
    size_t done = 0;
    while (done < out->size()) {
        DWORD chunk = (DWORD)std::min<size_t>(out->size() - done, 1u << 20), got = 0;
        if (!ReadFile(h, &(*out)[done], chunk, &got, nullptr)) { *why = sys::ErrorText(GetLastError()); return false; }
        if (got == 0) { *why = L"the file ended early"; return false; }
        done += got;
    }
    return true;
}

struct DirEntry {
    std::wstring name;
    DWORD attr = 0;
    long long created = 0;
};
// Lists a folder through its handle (FileIdBothDirectoryInfo), so the listing is of the folder that was
// checked, not of whatever a path names now.
bool ListDir(HANDLE h, std::vector<DirEntry>* out, std::wstring* why, size_t max = 4096) {
    out->clear();
    std::vector<unsigned long long> buf(64 * 1024 / sizeof(unsigned long long));
    bool first = true;
    for (;;) {
        BOOL r = GetFileInformationByHandleEx(h, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buf.data(),
                                              (DWORD)(buf.size() * sizeof(buf[0])));
        first = false;
        if (!r) {
            DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_FILES) return true;
            *why = L"Can't list a backup folder: " + sys::ErrorText(e);
            return false;
        }
        const BYTE* p = reinterpret_cast<const BYTE*>(buf.data());
        const BYTE* end = p + buf.size() * sizeof(buf[0]);
        for (;;) {
            if (p + sizeof(FILE_ID_BOTH_DIR_INFO) > end) break;
            const FILE_ID_BOTH_DIR_INFO* fi = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
            if ((const BYTE*)fi->FileName + fi->FileNameLength > end) break;
            DirEntry de;
            de.name.assign(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
            de.attr = fi->FileAttributes;
            de.created = fi->CreationTime.QuadPart;
            if (de.name != L"." && de.name != L"..") {
                if (out->size() >= max) { *why = L"A backup folder holds too many entries."; return false; }
                out->push_back(std::move(de));
            }
            if (!fi->NextEntryOffset) break;
            p += fi->NextEntryOffset;
        }
    }
}

// The backup root is <anchor>\<parent>\<root> (in use: %ProgramData%\Hadron\bios-backups). Opens and
// checks parent and root by handle: real folders (not links) at exactly <anchor's final path>\<parent>
// and ...\<root>, each passing trust. With create, each missing one is first created with kBackupSddl
// (the parent is checked before the root is created inside it). Never changes the security of anything
// that already exists. *parentH and *rootH stay open (no write/delete sharing) while the caller works.
bool OpenRoot(const std::wstring& root, bool create, const TrustFn& trust, Handle* parentH, Handle* rootH,
              std::wstring* rootFinal, std::wstring* why, OpenFail* fail = nullptr, DWORD* openErr = nullptr) {
    std::wstring r = sys::ToExtended(root);
    while (r.size() > 1 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    size_t k2 = r.find_last_of(L'\\');
    size_t k1 = k2 == std::wstring::npos || k2 == 0 ? std::wstring::npos : r.find_last_of(L'\\', k2 - 1);
    if (k1 == std::wstring::npos || k1 + 1 >= k2 || k2 + 1 >= r.size()) {
        *why = L"The backup folder " + sys::ToShell(r) + L" isn't a <folder>\\<name>\\<name> path.";
        return false;
    }
    std::wstring anchor = r.substr(0, k1), parent = r.substr(0, k2);
    std::wstring n1 = r.substr(k1 + 1, k2 - k1 - 1), n2 = r.substr(k2 + 1);
    if (!anchor.empty() && anchor.back() == L':') anchor += L'\\';   // a drive root
    std::wstring anchorFinal;
    {
        // The anchor (%ProgramData%) is only resolved, not trusted: the trust starts at <parent>.
        if (create && !EnsureDir(anchor)) { *why = L"Can't create " + sys::ToShell(anchor) + L": " + sys::ErrorText(GetLastError()); return false; }
        Handle a(CreateFileW(anchor.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        DWORD fe = GetLastError();
        if (!a.Ok() || !HandleFinalPath(a.Get(), &anchorFinal, &fe)) {
            if (fail) *fail = OpenFail::Open;
            if (openErr) *openErr = fe;
            *why = L"Can't open " + sys::ToShell(anchor) + L": " + sys::ErrorText(fe);
            return false;
        }
    }
    BackupSd sd;
    if (create && !sd.Make(why)) return false;
    auto mk = [&](const std::wstring& p) {
        if (!create || CreateDirectoryW(p.c_str(), sd.Sa())) return true;
        DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS) return true;   // checked by handle next, never re-secured
        *why = L"Can't create " + sys::ToShell(p) + L": " + sys::ErrorText(e);
        return false;
    };
    std::wstring parentFinal;
    if (!mk(parent) || !OpenChecked(parent, true, anchorFinal + L"\\" + n1, L"", trust, parentH, &parentFinal, why, fail, openErr))
        return false;
    if (!mk(r) || !OpenChecked(r, true, parentFinal + L"\\" + n2, L"", trust, rootH, rootFinal, why, fail, openErr)) return false;
    return true;
}

// FileSource over files already read into memory (keys: lower-case, '/'-separated, relative).
struct MemSource : FileSource {
    std::map<std::wstring, std::string> files;
    bool Read(const std::wstring& rel, std::string* out, size_t cap, std::wstring* err) override {
        auto it = files.find(Lower(rel));
        if (it == files.end()) { *err = rel + L": not in the backup"; return false; }
        if (it->second.size() > cap) { *err = rel + L": too large"; return false; }
        *out = it->second;
        return true;
    }
};

std::wstring TrimLine(std::wstring s) {   // the same trimming as snapshot.cpp's system.txt reader
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\t')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == 0xFEFF)) a++;
    return s.substr(a);
}

constexpr const char* kManifestHeader = "Hadron BIOS backup manifest 2";


}  // namespace

// ---- backends -----------------------------------------------------------------------------------
void RealBackend::Read(FwVar* v) { ReadVariable(v); }

DWORD RealBackend::Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data,
                         DWORD attr) {
    // Test runs never reach the firmware (docs/design/bios-write.md "Test mode latch").
    if (sys::NoFirmwareWrites()) return ERROR_ACCESS_DENIED;
    // Size 0 would delete the variable; anything but NV|BS|RT is refused; only map variables.
    if (data.empty() || data.size() > (4u << 20)) return ERROR_INVALID_PARAMETER;
    if (attr != kAttrRequired) return ERROR_INVALID_PARAMETER;
    if (!allowed_.count(MakeVarKey(name, guid))) return ERROR_INVALID_PARAMETER;
    std::wstring g = Braced(guid);
    apiCalls++;
    SetLastError(0);
    if (!SetFirmwareEnvironmentVariableExW(name.c_str(), g.c_str(), const_cast<uint8_t*>(data.data()),
                                           (DWORD)data.size(), attr)) {
        DWORD e = GetLastError();
        return e ? e : ERROR_GEN_FAILURE;
    }
    return 0;
}

bool RealBackend::Smbios(SmbiosInfo* out, DWORD* err) { return ReadSmbios(out, err); }

bool RealBackend::PrivilegeOk(const ThreadPrivilege& p, DWORD* err) {
    if (err) *err = p.ok() ? 0 : (p.err() ? p.err() : ERROR_PRIVILEGE_NOT_HELD);
    return p.ok();
}

void MockBackend::Put(const std::wstring& name, const std::wstring& guid, std::vector<uint8_t> data, DWORD attr) {
    VarKey k = MakeVarKey(name, guid);
    FwVar v;
    v.name = name;
    v.guid = k.second;
    v.attr = attr;
    v.data = std::move(data);
    vars[k] = std::move(v);
}

void MockBackend::Read(FwVar* v) {
    auto it = vars.find(MakeVarKey(v->name, v->guid));
    v->data.clear(); v->attr = 0; v->err = 0;
    if (it == vars.end()) { v->err = ERROR_ENVVAR_NOT_FOUND; return; }
    v->data = it->second.data;
    v->attr = it->second.attr;
}

DWORD MockBackend::Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data,
                         DWORD attr) {
    writes++;
    writeLog.push_back(name);
    if (data.empty()) return ERROR_INVALID_PARAMETER;   // a size-0 write would delete it: never
    auto it = vars.find(MakeVarKey(name, guid));
    if (it == vars.end()) return ERROR_ENVVAR_NOT_FOUND;   // never creates
    if (!failWriteOf.empty() && name == failWriteOf) return failWriteErr;
    if (attr != it->second.attr) return ERROR_INVALID_PARAMETER;
    it->second.data = data;
    if (!corruptWriteOf.empty() && name == corruptWriteOf) {
        it->second.data[it->second.data.size() / 2] ^= 0xA5;   // once: the restore that follows succeeds
        corruptWriteOf.clear();
    }
    return 0;
}

bool MockBackend::Smbios(SmbiosInfo* out, DWORD* err) {
    *out = smbios;
    if (err) *err = smbiosErr;
    return smbiosErr == 0 && smbios.ok;
}

bool MockBackend::PrivilegeOk(const ThreadPrivilege&, DWORD* err) {
    if (err) *err = privilege ? 0 : ERROR_PRIVILEGE_NOT_HELD;
    return privilege;
}

// ---- pure plan logic ----------------------------------------------------------------------------
bool ValidateValue(const Setting& s, long long v, std::wstring* why) {
    auto fail = [&](const std::wstring& w) { if (why) *why = w; return false; };
    if (s.width != 1 && s.width != 2 && s.width != 4 && s.width != 8)
        return fail(L"stored in " + std::to_wstring(s.width) + L" bytes, which Hadron doesn't write");
    if (s.kind == L"oneof") {
        if (s.opts.empty()) return fail(L"this setting has no option list");
        for (auto& o : s.opts) if (o.v == v) return true;
        return fail(L"not one of this setting's options");
    }
    if (s.kind == L"checkbox") {
        if (v == 0 || v == 1) return true;
        return fail(L"a checkbox takes 0 or 1");
    }
    if (s.kind == L"numeric") {
        if (!s.hasMin) return fail(L"the map has no range for this setting");
        if (s.mn > s.mx) return fail(L"the map's range for this setting is empty");
        if (v < s.mn || v > s.mx)
            return fail(L"outside the range " + std::to_wstring(s.mn) + L" to " + std::to_wstring(s.mx));
        if (s.step > 0 && ((unsigned long long)v - (unsigned long long)s.mn) % (unsigned long long)s.step != 0)
            return fail(L"not on the step: " + std::to_wstring(s.mn) + L" + a multiple of " + std::to_wstring(s.step));
        return true;
    }
    return fail(L"unknown setting kind \"" + s.kind + L"\"");
}

bool EncodeValue(long long v, int width, uint8_t* out) {
    if (width != 1 && width != 2 && width != 4 && width != 8) return false;
    if (v < 0) return false;
    unsigned long long u = (unsigned long long)v;
    if (width < 8 && (u >> (8 * width)) != 0) return false;
    std::vector<uint8_t> tmp((size_t)width);
    for (int i = 0; i < width; i++) tmp[(size_t)i] = (uint8_t)(u >> (8 * i));
    long long back = 0;
    if (!DecodeValue(tmp, 0, width, &back) || back != v) return false;
    std::copy(tmp.begin(), tmp.end(), out);
    return true;
}

int Plan::SettingCount() const {
    int n = 0;
    for (auto& v : vars) for (auto& s : v.settings) if (s.setting >= 0) n++;
    return n;
}

std::wstring Plan::Title() const {
    int n = SettingCount(), m = (int)vars.size();
    std::wstring vs = std::to_wstring(m) + (m == 1 ? L" variable" : L" variables");
    if (restore) return L"Restore " + vs + L" from a backup (" + std::to_wstring(n) + (n == 1 ? L" setting)" : L" settings)");
    return L"Write " + std::to_wstring(n) + (n == 1 ? L" setting" : L" settings") + L" to " + vs;
}

std::wstring HexBytes(const std::vector<uint8_t>& b, size_t max) {
    std::wstring s;
    for (size_t i = 0; i < b.size() && i < max; i++) {
        wchar_t x[4];
        swprintf(x, 4, L"%02X", b[i]);
        if (i) s += L' ';
        s += x;
    }
    if (b.size() > max) s += L" …";
    return s;
}

std::wstring Plan::Text() const {
    std::wstring t = L"Hadron BIOS " + std::wstring(restore ? L"restore" : L"write") + L" plan\r\n" + Title() + L"\r\n";
    t += L"Map: " + board + L" / " + bios + L"\r\n";
    if (restore) t += L"From backup: " + restoreFrom + L"\r\n";
    for (auto& v : vars) {
        t += L"\r\nVariable " + v.name + L" {" + v.guid + L"}  attributes " + Hex(v.attr) + L"  size " +
             std::to_wstring(v.oldData.size()) + L" bytes\r\n";
        for (auto& s : v.settings) {
            t += L"  " + s.prompt;
            if (!s.path.empty()) t += L"  [" + s.path + L"]";
            t += L"\r\n    " + s.oldText + L" -> " + s.newText + L"   offset " + Hex((unsigned)s.offset) + L", " +
                 std::to_wstring(s.width) + (s.width == 1 ? L" byte: " : L" bytes: ") + HexBytes(s.oldBytes) +
                 L" -> " + HexBytes(s.newBytes) + L"\r\n";
            if (!s.flags.empty()) t += L"    " + s.flags + L"\r\n";
            if (!s.shares.empty()) t += L"    Same bytes as: " + s.shares + L"\r\n";
        }
    }
    if (!notes.empty()) {
        t += L"\r\nNotes:\r\n";
        for (auto& n : notes) t += L"  " + n + L"\r\n";
    }
    return t;
}

bool BuildPlan(const Db& db, const std::vector<StagedChange>& staged, const std::vector<FwVar>& live,
               const PlanGate& gate, Plan* plan, std::wstring* err) {
    *plan = Plan{};
    auto fail = [&](const std::wstring& w) { if (err) *err = w; *plan = Plan{}; return false; };
    std::wstring g = GateProblem(gate);
    if (!g.empty()) return fail(g);
    if (db.board.empty() || db.bios.empty()) return fail(L"The settings map has no board/BIOS guard (v1 file).");
    if (staged.empty()) return fail(L"Nothing is staged.");
    plan->board = db.board;
    plan->bios = db.bios;
    std::vector<VarKey> order;
    for (auto& v : db.Variables()) order.push_back(MakeVarKey(v.first, v.second));
    std::map<VarKey, std::vector<StagedChange>> byVar;
    std::set<int> seen;
    std::vector<std::wstring> notes;
    for (auto& c : staged) {
        if (c.setting < 0 || c.setting >= (int)db.S.size()) return fail(L"A staged setting isn't in the map.");
        if (!seen.insert(c.setting).second) return fail(L"A setting is staged twice.");
        const Setting& s = db.S[c.setting];
        std::wstring why;
        if (!ValidateValue(s, c.value, &why)) return fail(s.prompt + L": " + std::to_wstring(c.value) + L" is refused (" + why + L").");
        uint8_t enc[8];
        if (!EncodeValue(c.value, s.width, enc))
            return fail(s.prompt + L": " + std::to_wstring(c.value) + L" doesn't fit in " + std::to_wstring(s.width) + L" bytes.");
        VarKey k = MakeVarKey(s.var, s.guid);
        if (std::find(order.begin(), order.end(), k) == order.end())
            return fail(s.prompt + L": variable " + s.var + L" isn't one of the map's variables.");
        const FwVar* v = FindLive(live, k);
        if (!v) return fail(s.prompt + L": variable " + s.var + L" wasn't read from this PC.");
        if (v->err) return fail(s.prompt + L": variable " + s.var + L" couldn't be read: " + VarErrorText(v->err) + L".");
        if (v->data.empty()) return fail(s.prompt + L": variable " + s.var + L" is empty.");
        std::wstring ap = AttrProblem(v->attr);
        if (!ap.empty()) return fail(s.prompt + L": variable " + s.var + L" is refused: " + ap + L".");
        if (s.offset < 0 || (size_t)s.offset + (size_t)s.width > v->data.size())
            return fail(s.prompt + L": offset " + Hex((unsigned)s.offset) + L" + " + std::to_wstring(s.width) +
                        L" bytes is past the end of " + s.var + L" (" + std::to_wstring(v->data.size()) + L" bytes).");
        long long cur = 0;
        DecodeValue(v->data, s.offset, s.width, &cur);
        if (cur == c.value) { notes.push_back(s.prompt + L": already " + ValText(s, cur) + L"; dropped."); continue; }
        byVar[k].push_back(c);
    }
    for (auto& k : order) {
        auto it = byVar.find(k);
        if (it == byVar.end()) continue;
        const FwVar* v = FindLive(live, k);
        PlanVar pv;
        pv.name = v->name;
        pv.guid = k.second;
        pv.attr = v->attr;
        pv.oldData = v->data;
        pv.newData = v->data;
        auto list = it->second;
        std::sort(list.begin(), list.end(), [&](const StagedChange& a, const StagedChange& b) {
            return db.S[a.setting].offset < db.S[b.setting].offset;
        });
        int prevEnd = -1;
        const Setting* prev = nullptr;
        for (auto& c : list) {
            const Setting& s = db.S[c.setting];
            if (prev && s.offset < prevEnd)
                return fail(L"\"" + prev->prompt + L"\" and \"" + s.prompt + L"\" are stored in the same bytes of " +
                            s.var + L"; stage only one of them.");
            prevEnd = s.offset + s.width;
            prev = &s;
            uint8_t enc[8];
            EncodeValue(c.value, s.width, enc);
            std::copy(enc, enc + s.width, pv.newData.begin() + s.offset);
            pv.ranges.push_back({s.offset, s.width});
        }
        for (auto& c : list) {
            const Setting& s = db.S[c.setting];
            PlanSetting ps;
            ps.setting = c.setting;
            ps.prompt = s.prompt;
            ps.path = s.path;
            DecodeValue(pv.oldData, s.offset, s.width, &ps.oldV);
            ps.newV = c.value;
            ps.oldText = ValText(s, ps.oldV);
            ps.newText = ValText(s, ps.newV);
            ps.offset = s.offset;
            ps.width = s.width;
            ps.oldBytes = Slice(pv.oldData, s.offset, s.width);
            ps.newBytes = Slice(pv.newData, s.offset, s.width);
            ps.flags = SettingFlags(s);
            ps.shares = SharedSettings(db, pv, c.setting, s.offset, s.width);
            pv.settings.push_back(std::move(ps));
        }
        if (!OnlyInRanges(pv)) return fail(L"Internal check failed: the new bytes of " + pv.name + L" differ outside the planned settings.");
        plan->vars.push_back(std::move(pv));
    }
    plan->notes = notes;
    if (plan->vars.empty()) {
        std::wstring n = L"Nothing to write: every staged value already matches.";
        *plan = Plan{};
        return fail(n);
    }
    return true;
}

bool BuildRestorePlan(const Db& db, const VarMap& backup, const std::wstring& backupDir,
                      const std::wstring& backupBoard, const std::wstring& backupBios,
                      const std::vector<FwVar>& live, const PlanGate& gate, Plan* plan, std::wstring* err) {
    *plan = Plan{};
    auto fail = [&](const std::wstring& w) { if (err) *err = w; *plan = Plan{}; return false; };
    std::wstring g = GateProblem(gate);
    if (!g.empty()) return fail(g);
    if (db.board.empty() || db.bios.empty()) return fail(L"The settings map has no board/BIOS guard (v1 file).");
    if (!BoardMatches(db.board, backupBoard) || !BiosMatches(db.bios, backupBios))
        return fail(L"This backup records " + (backupBoard.empty() ? std::wstring(L"no board") : backupBoard) + L" / " +
                    (backupBios.empty() ? std::wstring(L"no BIOS version") : backupBios) + L"; the map is for " + db.board +
                    L" / " + db.bios + L". Only a backup from this board and BIOS version can be restored.");
    plan->restore = true;
    plan->restoreFrom = backupDir;
    plan->board = db.board;
    plan->bios = db.bios;
    for (auto& mv : db.Variables()) {
        VarKey k = MakeVarKey(mv.first, mv.second);
        auto b = backup.find(k);
        if (b == backup.end()) continue;
        const FwVar* v = FindLive(live, k);
        if (!v) return fail(L"Variable " + mv.first + L" wasn't read from this PC.");
        if (v->err) return fail(L"Variable " + mv.first + L" couldn't be read: " + VarErrorText(v->err) + L".");
        std::wstring ap = AttrProblem(v->attr);
        if (!ap.empty()) return fail(L"Variable " + mv.first + L" is refused: " + ap + L".");
        if (b->second.size() != v->data.size() || v->data.empty())
            return fail(L"Variable " + mv.first + L" is " + std::to_wstring(v->data.size()) + L" bytes now but " +
                        std::to_wstring(b->second.size()) + L" bytes in the backup. A restore never changes a "
                        L"variable's size, so nothing will be restored.");
        if (b->second == v->data) { plan->notes.push_back(mv.first + L": already matches the backup."); continue; }
        PlanVar pv;
        pv.name = v->name;
        pv.guid = k.second;
        pv.attr = v->attr;
        pv.oldData = v->data;
        pv.newData = b->second;
        pv.ranges = DiffRanges(pv.oldData, pv.newData);
        std::vector<bool> covered(pv.oldData.size(), false);
        for (int i = 0; i < (int)db.S.size(); i++) {
            const Setting& s = db.S[i];
            if (s.var != pv.name || Lower(s.guid) != pv.guid) continue;
            bool hit = false;
            for (auto& r : pv.ranges) if (Overlaps(s.offset, s.width, r.first, r.second)) { hit = true; break; }
            if (!hit) continue;
            PlanSetting ps;
            ps.setting = i;
            ps.prompt = s.prompt;
            ps.path = s.path;
            bool ha = DecodeValue(pv.oldData, s.offset, s.width, &ps.oldV);
            bool hb = DecodeValue(pv.newData, s.offset, s.width, &ps.newV);
            ps.oldText = ha ? ValText(s, ps.oldV) : L"?";
            ps.newText = hb ? ValText(s, ps.newV) : L"?";
            ps.offset = s.offset;
            ps.width = s.width;
            ps.oldBytes = Slice(pv.oldData, s.offset, s.width);
            ps.newBytes = Slice(pv.newData, s.offset, s.width);
            ps.flags = SettingFlags(s);
            for (int j = 0; j < s.width; j++) if ((size_t)(s.offset + j) < covered.size()) covered[(size_t)(s.offset + j)] = true;
            pv.settings.push_back(std::move(ps));
        }
        int loose = 0, first = -1;
        for (auto& r : pv.ranges)
            for (int j = r.first; j < r.first + r.second; j++)
                if (!covered[(size_t)j]) { loose++; if (first < 0) first = j; }
        if (loose) {
            PlanSetting ps;
            ps.prompt = std::to_wstring(loose) + L" changed byte" + (loose == 1 ? L"" : L"s") + L" no mapped setting covers";
            ps.oldText = L"current";
            ps.newText = L"backup";
            ps.offset = first;
            ps.width = 1;
            ps.oldBytes = Slice(pv.oldData, first, 1);
            ps.newBytes = Slice(pv.newData, first, 1);
            ps.flags = L"Restored with the rest of the variable";
            pv.settings.push_back(std::move(ps));
        }
        std::sort(pv.settings.begin(), pv.settings.end(),
                  [](const PlanSetting& a, const PlanSetting& b) { return a.offset < b.offset; });
        plan->vars.push_back(std::move(pv));
    }
    for (auto& b : backup) {
        bool known = false;
        for (auto& mv : db.Variables()) if (MakeVarKey(mv.first, mv.second) == b.first) { known = true; break; }
        if (!known) plan->notes.push_back(b.first.first + L": not a variable of the settings map; not restored.");
    }
    if (plan->vars.empty()) {
        Plan keep = *plan;
        if (err) *err = L"Nothing to restore: every variable in the backup already matches this PC.";
        *plan = Plan{};
        plan->notes = keep.notes;
        return false;
    }
    return true;
}

// ---- execute ------------------------------------------------------------------------------------
std::wstring ExecResult::Summary() const {
    int restored = 0, failed = 0, restoreFailed = 0;
    for (auto& v : vars) {
        if (v.state == VarState::Restored) restored++;
        else if (v.state == VarState::Failed) failed++;
        else if (v.state == VarState::RestoreFailed) restoreFailed++;
    }
    std::wstring s;
    if (ok) s = std::to_wstring(written) + L" of " + std::to_wstring(vars.size()) + L" variables written and verified.";
    else s = std::to_wstring(written) + L" of " + std::to_wstring(vars.size()) + L" variables written and verified; stopped. " + error;
    if (restored) s += L" " + std::to_wstring(restored) + L" restored to the old bytes after a failed check.";
    if (failed) s += L" " + std::to_wstring(failed) + L" refused by the firmware (unchanged).";
    if (restoreFailed)
        s += L" " + std::to_wstring(restoreFailed) + L" COULD NOT BE RESTORED: restore the backup, or Clear CMOS (JBAT1) if the PC won't boot.";
    for (auto& v : vars) if (!v.text.empty()) s += L" " + v.name + L": " + v.text + L".";
    return s;
}

std::wstring DefaultBackupRoot() {
    // %ProgramData%, not %LOCALAPPDATA%: the user owns everything under their profile, so a non-admin
    // could redirect or swap a folder there; ProgramData\Hadron is created admin-only by Hadron.
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p)) && p) r = std::wstring(p) + L"\\Hadron\\bios-backups";
    if (p) CoTaskMemFree(p);
    return r;
}

bool SdTrusted(PSECURITY_DESCRIPTOR sd, std::wstring* why) {
    std::wstring dummy;
    if (!why) why = &dummy;
    if (!sd || !IsValidSecurityDescriptor(sd)) { *why = L"no valid security descriptor"; return false; }
    PSID owner = nullptr;
    BOOL ownerDef = FALSE;
    if (!GetSecurityDescriptorOwner(sd, &owner, &ownerDef)) { *why = L"the owner can't be read: " + sys::ErrorText(GetLastError()); return false; }
    if (!owner) { *why = L"it has no owner"; return false; }   // not an API failure: no GetLastError here
    if (!IsAdminOrSystem(owner)) { *why = L"it is owned by " + SidText(owner) + L", not Administrators or SYSTEM"; return false; }
    BOOL present = FALSE, daclDef = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &daclDef)) { *why = L"the DACL can't be read: " + sys::ErrorText(GetLastError()); return false; }
    if (!present || !dacl) { *why = L"it has no DACL (everyone has full access)"; return false; }   // likewise
    // Every right that changes, adds, removes or re-permissions something.
    const ACCESS_MASK kWrite = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD |
                               DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) { *why = L"the DACL can't be read: " + sys::ErrorText(GetLastError()); return false; }
    for (DWORD i = 0; i < info.AceCount; i++) {
        void* p = nullptr;
        if (!GetAce(dacl, i, &p) || !p) { *why = L"an ACE can't be read"; return false; }
        const ACE_HEADER* h = static_cast<const ACE_HEADER*>(p);
        const BYTE t = h->AceType;
        const bool allowed = t == ACCESS_ALLOWED_ACE_TYPE || t == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                             t == ACCESS_ALLOWED_OBJECT_ACE_TYPE || t == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE ||
                             t == ACCESS_ALLOWED_COMPOUND_ACE_TYPE;
        if (!allowed) continue;   // deny and audit ACEs can't grant anything
        // Every allowed ACE layout has the mask right after the header.
        ACCESS_MASK mask = *reinterpret_cast<const ACCESS_MASK*>(static_cast<const BYTE*>(p) + sizeof(ACE_HEADER));
        if (!(mask & kWrite)) continue;
        if (t == ACCESS_ALLOWED_ACE_TYPE || t == ACCESS_ALLOWED_CALLBACK_ACE_TYPE) {
            PSID sid = (PSID)&static_cast<ACCESS_ALLOWED_ACE*>(p)->SidStart;
            if (IsAdminOrSystem(sid)) continue;
            *why = L"it lets " + SidText(sid) + L" change it (access mask " + Hex(mask, 8) + L")";
            return false;
        }
        *why = L"an object or compound ACE grants write access (access mask " + Hex(mask, 8) + L")";
        return false;
    }
    return true;
}

bool HandleTrusted(HANDLE h, std::wstring* why) {
    std::wstring dummy;
    if (!why) why = &dummy;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD e = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              nullptr, nullptr, &sd);
    if (e != ERROR_SUCCESS) { *why = L"its permissions can't be read (" + sys::ErrorText(e) + L")"; return false; }
    bool ok = SdTrusted(sd, why);
    LocalFree(sd);
    return ok;
}

namespace {
const TrustFn& DefaultTrust() {
    static const TrustFn f = [](HANDLE h, const std::wstring&, std::wstring* why) { return HandleTrusted(h, why); };
    return f;
}
const TrustFn& Pick(const TrustFn& t) { return t ? t : DefaultTrust(); }
}  // namespace

bool PrepareBackupRoot(const std::wstring& root, std::wstring* err, const TrustFn& trust) {
    if (root.empty()) { *err = L"No backup folder (the ProgramData folder couldn't be resolved)."; return false; }
    Handle ph, rh;
    std::wstring rf, why;
    if (!OpenRoot(root, true, Pick(trust), &ph, &rh, &rf, &why)) {
        *err = L"The backup folder can't be used: " + why;
        return false;
    }
    return true;
}

bool CreateBackupSession(const std::wstring& root, const std::wstring& stamp, std::wstring* dir, std::wstring* err,
                         const TrustFn& trust) {
    if (root.empty()) { *err = L"No backup folder (the ProgramData folder couldn't be resolved)."; return false; }
    // The checked parent and root stay open (no delete sharing) while the session folder is made in them.
    Handle ph, rh;
    std::wstring rootFinal, why;
    if (!OpenRoot(root, true, Pick(trust), &ph, &rh, &rootFinal, &why)) { *err = L"The backup folder can't be used: " + why; return false; }
    BackupSd sd;
    if (!sd.Make(err)) return false;
    std::wstring base = sys::ToExtended(root);
    while (base.size() > 1 && base.back() == L'\\') base.pop_back();
    for (int n = 1; n < 1000; n++) {
        std::wstring name = stamp + (n > 1 ? L"-" + std::to_wstring(n) : L"");
        std::wstring cand = base + L"\\" + name;
        if (CreateDirectoryW(cand.c_str(), sd.Sa())) {
            Handle sh;
            if (!OpenChecked(cand, true, rootFinal + L"\\" + name, L"", Pick(trust), &sh, nullptr, &why)) {
                *err = L"The new backup folder can't be used: " + why;
                return false;
            }
            *dir = cand;
            return true;
        }        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) { *err = L"Can't create " + sys::ToShell(cand) + L": " + sys::ErrorText(e); return false; }
    }
    *err = L"Too many backup sessions with the stamp " + stamp + L".";
    return false;
}

bool Sha256Hex(const std::vector<uint8_t>& data, std::wstring* hex, std::wstring* err) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    UCHAR dig[32] = {};
    const wchar_t* step = L"BCryptOpenAlgorithmProvider";
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (BCRYPT_SUCCESS(st)) { step = L"BCryptCreateHash"; st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0); }
    if (BCRYPT_SUCCESS(st) && !data.empty()) {
        step = L"BCryptHashData";
        st = BCryptHashData(h, const_cast<PUCHAR>(data.data()), (ULONG)data.size(), 0);
    }
    if (BCRYPT_SUCCESS(st)) { step = L"BCryptFinishHash"; st = BCryptFinishHash(h, dig, sizeof(dig), 0); }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) {
        *err = std::wstring(L"SHA-256 failed in ") + step + L" (NTSTATUS " + Hex((unsigned long)st, 8) + L").";
        return false;
    }
    static const wchar_t* kDig = L"0123456789abcdef";
    hex->clear();
    for (UCHAR c : dig) { *hex += kDig[c >> 4]; *hex += kDig[c & 15]; }
    return true;
}

std::wstring SmbiosBoardLine(const SmbiosInfo& smb) { return TrimLine(smb.boardMaker + L" " + smb.boardProduct); }
std::wstring SmbiosBiosLine(const SmbiosInfo& smb) {
    return TrimLine(smb.biosVendor + L" " + smb.biosVersion + L" " + smb.biosDate);
}

bool ManifestText(const std::vector<FwVar>& vars, const SmbiosInfo& smb, std::string* text, std::wstring* err) {
    auto oneLine = [](std::wstring s) {
        for (auto& c : s) if (c == L'\r' || c == L'\n') c = L' ';
        return s;
    };
    std::wstring t = FromUtf8(kManifestHeader) + L"\r\n" + L"board\t" + oneLine(SmbiosBoardLine(smb)) + L"\r\n" +
                     L"bios\t" + oneLine(SmbiosBiosLine(smb)) + L"\r\n";
    for (auto& v : vars) {
        std::wstring sha;
        if (!Sha256Hex(v.data, &sha, err)) return false;
        wchar_t attr[16];
        swprintf(attr, 16, L"%08lX", (unsigned long)v.attr);
        t += v.name + L"\t" + v.guid + L"\t" + std::to_wstring(v.data.size()) + L"\t" + attr + L"\t" + sha + L"\r\n";
    }
    *text = ToUtf8(t);
    return true;
}

bool LoadBackupSession(const std::wstring& backupRoot, const std::wstring& dir, SnapLoad* out, const TrustFn& trust) {
    const TrustFn& check = Pick(trust);
    *out = SnapLoad{};
    out->label = sys::ToShell(sys::ToExtended(dir));
    auto refuse = [&](const std::wstring& why) {
        VarMap none;
        out->vars.swap(none);
        out->ok = false;
        out->error = why;
        LogLine(LogLevel::Warn, L"restore refused for " + out->label + L": " + why);
        return false;
    };
    if (backupRoot.empty()) return refuse(L"There is no backup folder to restore from (the ProgramData folder couldn't be resolved).");
    // Every folder and file is opened once, checked by its handle (not a link, where it should be,
    // admin-only) and read through that same handle; the folder handles stay open, without write or
    // delete sharing, until everything is read. Nothing is reopened by path after its check.
    Handle parentH, rootH, dirH, varsH;
    std::wstring rootFinal, dirFinal, e;
    if (!OpenRoot(backupRoot, false, check, &parentH, &rootH, &rootFinal, &e)) return refuse(L"The backup folder can't be used: " + e);
    OpenFail fail = OpenFail::None;
    if (!OpenChecked(dir, true, L"", rootFinal, check, &dirH, &dirFinal, &e, &fail)) {
        if (fail == OpenFail::Place || (fail == OpenFail::Kind && SamePath(ParentOf(sys::ToExtended(dir)), rootFinal)))
            return refuse(L"Only Hadron's own backup sessions can be restored: choose a folder directly inside " +
                          sys::ToShell(rootFinal) + L". Builds from before the "
                          L"ProgramData move kept backups in %LOCALAPPDATA%\\Hadron\\bios-backups; those aren't trusted, "
                          L"because any program running as you could have changed them.");
        return refuse(e);
    }
    MemSource mem;
    size_t total = 0;
    auto readInto = [&](const std::wstring& path, const std::wstring& parentFinal, const std::wstring& rel) {
        Handle fh;
        if (!OpenChecked(path, false, L"", parentFinal, check, &fh, nullptr, &e)) return false;
        std::string bytes;
        std::wstring why;
        if (!ReadAllFrom(fh.Get(), kMaxSnapText, &bytes, &why)) { e = sys::ToShell(sys::ToExtended(path)) + L": " + why; return false; }
        total += bytes.size();
        if (total > kMaxSnapTotal) { e = L"The backup is larger than a snapshot may be."; return false; }
        mem.files[Lower(rel)] = std::move(bytes);
        return true;
    };
    std::vector<DirEntry> top, vars;
    if (!ListDir(dirH.Get(), &top, &e)) return refuse(e);
    const std::wstring dirPath = sys::ToExtended(dir);
    for (auto& en : top) {
        std::wstring p = dirPath + L"\\" + en.name;
        if (en.attr & FILE_ATTRIBUTE_REPARSE_POINT) return refuse(sys::ToShell(p) + L" is a link (reparse point), so it isn't trusted.");
        if (!(en.attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!readInto(p, dirFinal, en.name)) return refuse(e);
            continue;
        }
        if (Lower(en.name) != L"vars") return refuse(L"The backup has an unexpected folder " + sys::ToShell(p) + L".");
        std::wstring varsFinal;
        if (!OpenChecked(p, true, dirFinal + L"\\" + en.name, L"", check, &varsH, &varsFinal, &e)) return refuse(e);
        if (!ListDir(varsH.Get(), &vars, &e)) return refuse(e);
        for (auto& v : vars) {
            std::wstring vp = p + L"\\" + v.name;
            if (v.attr & FILE_ATTRIBUTE_REPARSE_POINT) return refuse(sys::ToShell(vp) + L" is a link (reparse point), so it isn't trusted.");
            if (v.attr & FILE_ATTRIBUTE_DIRECTORY) return refuse(L"The backup has an unexpected folder " + sys::ToShell(vp) + L".");
            if (!readInto(vp, varsFinal, L"vars/" + v.name)) return refuse(e);
        }
    }
    if (!mem.files.count(L"journal.txt")) return refuse(L"This folder has no journal.txt, so it isn't a Hadron write backup.");
    auto mit = mem.files.find(L"manifest.txt");
    if (mit == mem.files.end()) return refuse(L"This folder has no manifest.txt, so it isn't a Hadron write backup.");
    const std::string man = mit->second;
    const std::wstring label = out->label;
    if (!LoadSnapshotFrom(mem, label, out)) return refuse(out->error.empty() ? L"The backup doesn't load." : out->error);
    out->label = label;
    if (out->skipped) return refuse(L"The backup lists variables it can't load (" + out->SkipSummary() + L").");
    // Parse and compare the manifest.
    std::wstring t = FromUtf8(man);
    std::vector<std::wstring> lines;
    for (size_t p = 0; p < t.size();) {
        size_t q = t.find(L'\n', p);
        if (q == std::wstring::npos) q = t.size();
        std::wstring l = t.substr(p, q - p);
        p = q + 1;
        while (!l.empty() && l.back() == L'\r') l.pop_back();
        if (!l.empty()) lines.push_back(l);
    }
    if (lines.empty() || lines[0] != FromUtf8(kManifestHeader))
        return refuse(L"manifest.txt isn't a version 2 Hadron backup manifest (\"" + FromUtf8(kManifestHeader) + L"\").");
    auto keyed = [&](size_t i, const wchar_t* key, std::wstring* v) {
        if (i >= lines.size()) return false;
        size_t tab = lines[i].find(L'\t');
        if (tab == std::wstring::npos || lines[i].substr(0, tab) != key) return false;
        *v = lines[i].substr(tab + 1);
        return true;
    };
    std::wstring mBoard, mBios;
    if (!keyed(1, L"board", &mBoard) || !keyed(2, L"bios", &mBios)) return refuse(L"manifest.txt has no board/BIOS lines.");
    std::set<VarKey> seen;
    for (size_t i = 3; i < lines.size(); i++) {
        std::vector<std::wstring> f;
        for (size_t p = 0;;) {
            size_t q = lines[i].find(L'\t', p);
            f.push_back(lines[i].substr(p, q == std::wstring::npos ? std::wstring::npos : q - p));
            if (q == std::wstring::npos) break;
            p = q + 1;
        }
        if (f.size() != 5) return refuse(L"manifest.txt line " + std::to_wstring(i + 1) + L" is malformed.");
        VarKey k = MakeVarKey(f[0], f[1]);
        if (!seen.insert(k).second) return refuse(L"manifest.txt lists " + f[0] + L" twice.");
        wchar_t* end = nullptr;
        unsigned long long size = wcstoull(f[2].c_str(), &end, 10);
        if (f[2].empty() || *end) return refuse(L"manifest.txt has a bad size for " + f[0] + L".");
        unsigned long attr = wcstoul(f[3].c_str(), &end, 16);
        if (f[3].empty() || *end) return refuse(L"manifest.txt has bad attributes for " + f[0] + L".");
        if (attr != kAttrRequired) return refuse(L"manifest.txt gives " + f[0] + L" attributes " + Hex(attr) + L", not NV|BS|RT.");
        auto it = out->vars.find(k);
        if (it == out->vars.end()) return refuse(L"The backup is missing " + f[0] + L", which manifest.txt lists.");
        if (it->second.size() != size)
            return refuse(f[0] + L" in the backup is " + std::to_wstring(it->second.size()) + L" bytes; manifest.txt says " + f[2] + L".");
        std::wstring sha;
        if (!Sha256Hex(it->second, &sha, &e)) return refuse(e);
        if (!SamePath(sha, f[4]))   // case-insensitive hex compare
            return refuse(f[0] + L" in the backup doesn't match manifest.txt (SHA-256 differs): the file was changed.");
    }
    if (seen.size() != out->vars.size()) return refuse(L"The backup holds variables manifest.txt doesn't list.");
    if (seen.empty()) return refuse(L"manifest.txt lists no variables.");
    // The guard reads the verified manifest; system.txt must agree with it.
    if (!out->hasSystem || out->sysBoard != mBoard || out->sysBios != mBios)
        return refuse(L"system.txt doesn't match manifest.txt (board \"" + out->sysBoard + L"\" / BIOS \"" + out->sysBios +
                      L"\" against \"" + mBoard + L"\" / \"" + mBios + L"\"): the file was changed.");
    out->sysBoard = mBoard;
    out->sysBios = mBios;
    return true;
}

namespace {
// Writes oldData back after a failed check and verifies it. True when the variable holds oldData again.
bool PutBack(FwBackend& be, const PlanVar& pv, DWORD* err) {
    DWORD e = be.Write(pv.name, pv.guid, pv.oldData, pv.attr);
    FwVar rb;
    rb.name = pv.name; rb.guid = pv.guid;
    be.Read(&rb);
    *err = e ? e : rb.err;
    return !e && !rb.err && rb.attr == pv.attr && rb.data == pv.oldData;
}
}  // namespace

bool Execute(FwBackend& be, const Plan& plan, const std::wstring& backupRoot, const std::function<bool()>& cancelled,
             const ProgressFn& progress, ExecResult* out, const ExecOptions& opt) {
    *out = ExecResult{};
    for (auto& pv : plan.vars) { VarOutcome o; o.name = pv.name; o.guid = pv.guid; out->vars.push_back(o); }
    const int total = (int)plan.vars.size();
    auto stop = [&](const std::wstring& e) {
        out->error = e;
        LogLine(LogLevel::Error, e);
        return false;
    };
    auto prog = [&](int done, const std::wstring& what) { if (progress) progress(done, total, what); };
    // One writer at a time across every Hadron process in this session, for the whole run.
    WriteLock lock;
    std::wstring lockErr;
    if (!lock.Take(opt.lockName.empty() ? std::wstring(kWriteMutexName) : opt.lockName, &lockErr)) return stop(lockErr);
    if (plan.vars.empty()) return stop(L"The plan is empty.");
    // Static checks again (defence in depth): same size, never 0, attributes exactly NV|BS|RT, only
    // the planned bytes change.
    for (auto& pv : plan.vars) {
        if (pv.oldData.empty() || pv.newData.size() != pv.oldData.size())
            return stop(L"Internal check failed for " + pv.name + L": the new data isn't the same size. Nothing was written.");
        if (pv.attr != kAttrRequired) return stop(L"Internal check failed for " + pv.name + L": attributes " + Hex(pv.attr) + L". Nothing was written.");
        if (!OnlyInRanges(pv)) return stop(L"Internal check failed for " + pv.name + L": bytes outside the plan change. Nothing was written.");
    }
    if (plan.board.empty() || plan.bios.empty()) return stop(L"The plan has no board/BIOS guard. Nothing was written.");

    // The privilege lives on this worker thread only, for the whole run, and is required.
    ThreadPrivilege priv(SE_SYSTEM_ENVIRONMENT_NAME);
    DWORD perr = 0;
    if (!be.PrivilegeOk(priv, &perr))
        return stop(L"SeSystemEnvironmentPrivilege couldn't be enabled on the write thread (" + sys::ErrorText(perr) +
                    L"). Nothing was written.");

    // 1. Board/BIOS guard, from SMBIOS now.
    prog(0, L"Checking the board and BIOS version");
    SmbiosInfo smb;
    DWORD se = 0;
    if (!be.Smbios(&smb, &se)) return stop(L"Couldn't read SMBIOS (" + VarErrorText(se) + L"), so the board can't be checked. Nothing was written.");
    if (!BoardMatches(plan.board, smb.boardProduct) || !BiosMatches(plan.bios, smb.biosVersion))
        return stop(L"This PC reports " + smb.boardProduct + L" / " + smb.biosVersion + L", not " + plan.board + L" / " +
                    plan.bios + L". Nothing was written.");
    // 2. Drift: every byte, the size and the attributes, against the bytes the plan was built from.
    std::vector<FwVar> current;
    for (auto& pv : plan.vars) {
        FwVar v;
        v.name = pv.name; v.guid = pv.guid;
        be.Read(&v);
        if (v.err) return stop(L"Couldn't re-read " + pv.name + L": " + VarErrorText(v.err) + L". Nothing was written.");
        if (v.attr != pv.attr || v.data.size() != pv.oldData.size() || v.data != pv.oldData)
            return stop(pv.name + L" changed since it was read; re-read and plan again. Nothing was written.");
        current.push_back(std::move(v));
    }
    if (cancelled && cancelled()) return stop(L"Cancelled before writing. Nothing was written.");
    // 3. Backup of every planned variable, flushed, before the first write; then read it back.
    prog(0, L"Saving the backup");
    std::wstring stamp = opt.stamp;
    if (stamp.empty()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t b[32];
        swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        stamp = b;
    }
    std::wstring dir, e;
    // Every folder and file of the session gets the admin-only descriptor (owner Administrators), so a
    // restore and the start-up banner can trust it.
    BackupSd sd;
    if (!sd.Make(&e)) return stop(L"Backup failed: " + e + L" Nothing was written.");
    if (!CreateBackupSession(backupRoot, stamp, &dir, &e, opt.trust)) return stop(L"Backup failed: " + e + L" Nothing was written.");
    out->backupDir = sys::ToShell(sys::ToExtended(dir));
    out->started = true;
    SaveResult sr;
    sr.folder = out->backupDir;
    if (!WriteSnapshotFiles(dir, current, smb,
                            plan.restore ? L"Hadron BIOS module (backup before a restore)" : L"Hadron BIOS module (backup before a write)",
                            &sr, sd.Sa()))
        return stop(L"Backup failed: " + sr.error + L" Nothing was written.");
    if (!WriteNewFile(dir + L"\\plan.txt", "\xEF\xBB\xBF" + ToUtf8(plan.Text()), &e, sd.Sa()))
        return stop(L"Backup failed: " + e + L" Nothing was written.");
    {
        SnapLoad chk;
        if (!LoadSnapshot(dir, &chk)) return stop(L"The backup doesn't read back (" + chk.error + L"). Nothing was written.");
        for (auto& v : current) {
            auto it = chk.vars.find(MakeVarKey(v.name, v.guid));
            if (it == chk.vars.end() || it->second != v.data)
                return stop(L"The backup of " + v.name + L" doesn't read back equal. Nothing was written.");
        }
    }
    // manifest.txt marks the folder as Hadron's own session; a restore checks every byte against it.
    {
        std::string man;
        if (!ManifestText(current, smb, &man, &e) || !WriteNewFile(dir + L"\\manifest.txt", man, &e, sd.Sa()))
            return stop(L"Backup failed: " + e + L" Nothing was written.");
    }
    Journal j;
    if (!j.Open(dir + L"\\journal.txt", sd.Sa(), &e)) return stop(e + L" Nothing was written.");
    LogLine(LogLevel::Info, L"backup saved to " + out->backupDir + L"\r\n" + plan.Text());

    // 4-6, per variable, stopping at the first failure.
    bool ok = true;
    for (int i = 0; i < total; i++) {
        const PlanVar& pv = plan.vars[(size_t)i];
        VarOutcome& vo = out->vars[(size_t)i];
        if (cancelled && cancelled()) { ok = false; out->error = L"Cancelled; the remaining variables weren't written."; break; }
        prog(i, L"Writing " + pv.name);
        // Drift again, right before this write (the window since step 2 includes the backup).
        FwVar now;
        now.name = pv.name; now.guid = pv.guid;
        be.Read(&now);
        if (now.err || now.attr != pv.attr || now.data != pv.oldData) {
            ok = false;
            out->error = pv.name + L" changed since it was read; it and the rest weren't written. Re-read and plan again.";
            break;
        }
        if (!j.Line(L"BEGIN " + pv.name + L" " + pv.guid, &e)) { ok = false; out->error = e; break; }
        LogLine(LogLevel::Info, L"writing " + pv.name + L" {" + pv.guid + L"} " + std::to_wstring(pv.newData.size()) + L" bytes, attributes " + Hex(pv.attr));
        if (pv.newData.empty()) { ok = false; out->error = L"Internal check failed: size 0."; break; }   // never delete
        DWORD we = be.Write(pv.name, pv.guid, pv.newData, pv.attr);
        if (we) {
            vo.err = we;
            FwVar rb;
            rb.name = pv.name; rb.guid = pv.guid;
            be.Read(&rb);
            if (!rb.err && rb.attr == pv.attr && rb.data == pv.oldData) {
                vo.state = VarState::Failed;
                vo.text = L"write refused, " + WriteErrorText(we) + L"; the variable is unchanged";
                j.Line(L"FAILED " + pv.name + L" " + std::to_wstring(we), &e);
            } else {
                DWORD re = 0;
                bool back = PutBack(be, pv, &re);
                vo.state = back ? VarState::Restored : VarState::RestoreFailed;
                vo.text = L"write failed, " + WriteErrorText(we) + (back ? L"; the old bytes were restored and verified"
                                                                       : L"; restoring the old bytes failed (" + WriteErrorText(re) + L")");
                j.Line(back ? L"RESTORED " + pv.name : L"RESTORE-FAILED " + pv.name + L" " + std::to_wstring(re), &e);
            }
            LogLine(LogLevel::Error, pv.name + L": " + vo.text);
            ok = false;
            out->error = L"The write of " + pv.name + L" failed; the rest weren't written.";
            break;
        }
        FwVar rb;
        rb.name = pv.name; rb.guid = pv.guid;
        be.Read(&rb);
        if (rb.err || rb.attr != pv.attr || rb.data != pv.newData) {
            DWORD re = 0;
            bool back = PutBack(be, pv, &re);
            vo.state = back ? VarState::Restored : VarState::RestoreFailed;
            vo.err = rb.err;
            vo.text = std::wstring(L"the read-back didn't match the written bytes") +
                      (back ? L"; the old bytes were restored and verified" : L"; restoring the old bytes failed (" + WriteErrorText(re) + L")");
            j.Line(back ? L"RESTORED " + pv.name : L"RESTORE-FAILED " + pv.name + L" " + std::to_wstring(re), &e);
            LogLine(LogLevel::Error, pv.name + L": " + vo.text);
            ok = false;
            out->error = L"Verifying " + pv.name + L" failed; the rest weren't written.";
            break;
        }
        vo.state = VarState::Written;
        out->written++;
        LogLine(LogLevel::Info, pv.name + L" written and verified");
        if (!j.Line(L"OK " + pv.name, &e)) { ok = false; out->error = e; break; }
    }
    j.Line(L"END", &e);
    prog(total, L"Done");
    out->ok = ok && out->written == total;
    if (!out->ok && out->error.empty()) out->error = L"Stopped.";
    LogLine(out->ok ? LogLevel::Info : LogLevel::Error, out->Summary());
    return out->ok;
}

// ---- journals -----------------------------------------------------------------------------------
bool JournalIncomplete(const std::string& journal, std::vector<std::wstring>* vars) {
    std::wstring t = FromUtf8(journal);
    std::vector<std::wstring> open;
    size_t a = 0;
    while (a < t.size()) {
        size_t b = t.find(L'\n', a);
        if (b == std::wstring::npos) b = t.size();
        std::wstring line = t.substr(a, b - a);
        a = b + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);
        size_t sp = line.find(L' ');
        if (sp == std::wstring::npos) continue;
        std::wstring kw = line.substr(0, sp), rest = line.substr(sp + 1);
        auto lastWordCut = [](const std::wstring& r) {
            size_t k = r.find_last_of(L' ');
            return k == std::wstring::npos ? r : r.substr(0, k);
        };
        if (kw == L"BEGIN") open.push_back(lastWordCut(rest));
        else if (kw == L"OK" || kw == L"RESTORED" || kw == L"FAILED") {
            std::wstring name = kw == L"FAILED" ? lastWordCut(rest) : rest;
            auto it = std::find(open.begin(), open.end(), name);
            if (it != open.end()) open.erase(it);
        }
        // RESTORE-FAILED leaves the variable open: it needs a restore.
    }
    if (vars) *vars = open;
    return !open.empty();
}

bool FindIncompleteSession(const std::wstring& root, IncompleteSession* out, int newest, const TrustFn& trust) {
    *out = IncompleteSession{};
    if (root.empty()) return false;
    const TrustFn& check = Pick(trust);
    // Handle-based like LoadBackupSession: the root, each session folder and its journal are opened once,
    // checked by handle and read through it.
    Handle parentH, rootH;
    std::wstring rootFinal, why;
    OpenFail fail = OpenFail::None;
    DWORD oe = 0;
    if (!OpenRoot(root, false, check, &parentH, &rootH, &rootFinal, &why, &fail, &oe)) {
        bool none = fail == OpenFail::Open && (oe == ERROR_FILE_NOT_FOUND || oe == ERROR_PATH_NOT_FOUND);
        if (!none) LogLine(LogLevel::Warn, L"unfinished-write check skipped: " + why);   // no backups yet is not worth a line
        return false;
    }
    std::vector<DirEntry> entries;
    if (!ListDir(rootH.Get(), &entries, &why)) { LogLine(LogLevel::Warn, L"unfinished-write check skipped: " + why); return false; }
    // Newest first by the folder's creation time (a stamp in the name follows the local clock, which can
    // jump), the name breaking ties.
    std::vector<DirEntry> sessions;
    for (auto& en : entries)
        if ((en.attr & FILE_ATTRIBUTE_DIRECTORY) && !(en.attr & FILE_ATTRIBUTE_REPARSE_POINT)) sessions.push_back(en);
    std::sort(sessions.begin(), sessions.end(), [](const DirEntry& a, const DirEntry& b) {
        return a.created != b.created ? a.created > b.created : a.name > b.name;
    });
    const std::wstring base = sys::ToExtended(root);
    for (int i = 0; i < (int)sessions.size() && i < newest; i++) {
        std::wstring dir = base + L"\\" + sessions[(size_t)i].name;
        Handle dh, jh;
        std::wstring dirFinal;
        if (!OpenChecked(dir, true, rootFinal + L"\\" + sessions[(size_t)i].name, L"", check, &dh, &dirFinal, &why)) {
            // Not one of Hadron's own sessions (or changed without admin rights): never a banner.
            LogLine(LogLevel::Warn, L"ignored " + sys::ToShell(dir) + L": " + why);
            continue;
        }
        DWORD je = 0;
        if (!OpenChecked(dir + L"\\journal.txt", false, dirFinal + L"\\journal.txt", L"", check, &jh, nullptr, &why, &fail, &je)) {
            if (fail == OpenFail::Open && je == ERROR_FILE_NOT_FOUND) continue;   // stopped before writing: no journal
            // Another Hadron window holds this journal open: a write is running now. Offering an older
            // session would undo it, so no banner this time.
            if (fail == OpenFail::Open && je == ERROR_SHARING_VIOLATION) return false;
            LogLine(LogLevel::Warn, L"ignored " + sys::ToShell(dir) + L": " + why);
            continue;
        }
        std::string text;
        if (!ReadAllFrom(jh.Get(), 1u << 20, &text, &why)) {
            LogLine(LogLevel::Warn, L"ignored " + sys::ToShell(dir) + L": journal.txt: " + why);
            continue;
        }
        // Only the newest trusted session with a journal counts: a later write's backup already holds the
        // state an older incomplete session left, so offering the older backup would undo that later write.
        std::vector<std::wstring> vars;
        if (!JournalIncomplete(text, &vars)) return false;
        out->dir = sys::ToShell(dir);
        out->stamp = sessions[(size_t)i].name;
        out->vars = vars;
        return true;
    }
    return false;
}

}  // namespace hx::bios
