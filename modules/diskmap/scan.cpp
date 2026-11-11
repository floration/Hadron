// modules/diskmap/scan.cpp — directory enumeration, classification and the bottom-up roll-up.
//
// Each folder is opened once with CreateFileW(FILE_LIST_DIRECTORY, FILE_FLAG_BACKUP_SEMANTICS |
// FILE_FLAG_OPEN_REPARSE_POINT) and listed with GetFileInformationByHandleEx(FileIdBothDirectoryInfo).
// That one call gives what the approved deviations need without opening any file:
//   - FileId, for hard-link de-dup by (volume serial, FileId);
//   - attributes, for reparse (leaf) and cloud-placeholder (skip) handling;
//   - the reparse tag (EaSize holds it when FILE_ATTRIBUTE_REPARSE_POINT is set);
//   - backup semantics, so SeBackupPrivilege (enabled by the module) lets admin-only folders list.
// FindFirstFileExW(FindExInfoBasic, LARGE_FETCH) is the fallback when the FS rejects the info class;
// those folders are counted per link (DF_NOID) and tallied.
#include "modules/diskmap/scan.h"
#include "core/sys/sys.h"
#include <algorithm>
#include <cwchar>
#include <unordered_map>

namespace hx::diskmap {

namespace {

constexpr DWORD kCloudMask = FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                             FILE_ATTRIBUTE_OFFLINE;
constexpr size_t kDeniedSample = 32;

// Open-addressed set of 64-bit FileIds (0 = empty slot; FileId 0 is never inserted). ~16 bytes per
// file at the 50% load cap, far less than std::unordered_set on a multi-million-file drive.
class IdSet {
public:
    bool Insert(u64 id) {   // true if it was not there
        if ((count_ + 1) * 2 > slots_.size()) Grow();
        size_t mask = slots_.size() - 1, i = Mix(id) & mask;
        while (slots_[i]) { if (slots_[i] == id) return false; i = (i + 1) & mask; }
        slots_[i] = id; count_++;
        return true;
    }
private:
    static u64 Mix(u64 x) {   // splitmix64 finaliser
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; return x ^ (x >> 31);
    }
    void Grow() {
        std::vector<u64> old; old.swap(slots_);
        slots_.assign(old.empty() ? 1024 : old.size() * 2, 0);
        count_ = 0;
        for (u64 v : old) if (v) Insert(v);
    }
    std::vector<u64> slots_;
    size_t count_ = 0;
};

bool IsDots(const wchar_t* n, size_t len) {
    return (len == 1 && n[0] == L'.') || (len == 2 && n[0] == L'.' && n[1] == L'.');
}

std::wstring Join(const std::wstring& a, const wchar_t* b, size_t n) {
    std::wstring s = a;
    if (s.empty() || s.back() != L'\\') s += L'\\';
    s.append(b, n);
    return s;
}

// One entry as the scanner needs it, whichever API listed it.
struct Entry {
    const wchar_t* name; size_t len;
    DWORD attrs; u64 size; u64 fileId; u32 tag;
};

}  // namespace

u64 SafeSum(const u64* r) { u64 s = 0; for (int i = 1; i < NSAFE; i++) s += r[i]; return s; }

u64 Scan::SafeBytes(u32 d) const { return d < dirs.size() ? SafeSum(dirs[d].safeR) : 0; }

u32 Scan::AddName(const wchar_t* s, size_t n) {
    u32 off = (u32)names.size();
    names.insert(names.end(), s, s + n);
    names.push_back(0);
    return off;
}

std::wstring Scan::DirPath(u32 d) const {
    if (d >= dirs.size()) return L"";
    std::vector<u32> chain;
    for (u32 x = d; x != 0; x = dirs[x].parent) chain.push_back(x);
    std::wstring s = Name(dirs[0].name);
    for (size_t i = chain.size(); i-- > 0;) { const wchar_t* n = Name(dirs[chain[i]].name); s = Join(s, n, wcslen(n)); }
    return s;
}

std::wstring Scan::ExtendedDirPath(u32 d) const {
    if (d >= dirs.size()) return L"";
    std::vector<u32> chain;
    for (u32 x = d; x != 0; x = dirs[x].parent) chain.push_back(x);
    std::wstring s = rootExt_;
    for (size_t i = chain.size(); i-- > 0;) { const wchar_t* n = Name(dirs[chain[i]].name); s = Join(s, n, wcslen(n)); }
    return s;
}

std::wstring Scan::FilePath(u32 f) const {
    if (f >= files.size()) return L"";
    const wchar_t* n = Name(files[f].name);
    return Join(DirPath(files[f].dir), n, wcslen(n));
}

namespace {
// Enables SeBackupPrivilege on this thread only (deviation 5): ImpersonateSelf gives the thread its
// own copy of the process token, so the privilege never leaks to the process, other modules in the
// suite, or child processes. The destructor reverts, so every exit path of Run is covered.
class ThreadBackupPrivilege {
public:
    ThreadBackupPrivilege() {
        if (!ImpersonateSelf(SecurityImpersonation)) { err_ = GetLastError(); return; }
        impersonating_ = true;
        HANDLE tok = nullptr;
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &tok)) { err_ = GetLastError(); return; }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, L"SeBackupPrivilege", &tp.Privileges[0].Luid)) err_ = GetLastError();
        else if (!AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr)) err_ = GetLastError();
        else { DWORD e = GetLastError(); if (e == ERROR_NOT_ALL_ASSIGNED) err_ = e; else ok_ = true; }   // rule 4
        CloseHandle(tok);
    }
    ~ThreadBackupPrivilege() { if (impersonating_) RevertToSelf(); }
    ThreadBackupPrivilege(const ThreadBackupPrivilege&) = delete;
    ThreadBackupPrivilege& operator=(const ThreadBackupPrivilege&) = delete;
    bool ok() const { return ok_; }
    DWORD err() const { return err_; }
private:
    bool impersonating_ = false, ok_ = false;
    DWORD err_ = 0;
};
}  // namespace

bool Scan::Run(const std::wstring& root, const std::wstring& display,
               const std::function<bool()>& cancelled, ScanProgress* prog, bool streams) {
    streams_ = streams;
    ThreadBackupPrivilege priv;   // reverted when Run returns, on every path
    bool r = RunImpl(root, display, cancelled, prog);
    stats.backupOk = priv.ok();
    stats.backupErr = priv.err();
    stats.streams = streams;
    return r;
}

bool Scan::RunImpl(const std::wstring& root, const std::wstring& display,
                   const std::function<bool()>& cancelled, ScanProgress* prog) {
    files.clear(); dirs.clear(); kids.clear(); names.clear();
    stats = ScanStats{};
    ok = false; rootErr = 0;
    rootExt_ = root;
    ULONGLONG t0 = GetTickCount64();
    const Rules& rules = Rules::Get();

    DirE r;
    r.name = AddName(display.c_str(), display.size());
    r.ctx = NOCTX;
    dirs.push_back(r);

    std::unordered_map<DWORD, IdSet> seen;   // volume serial -> FileIds already counted
    struct Pending { u32 dir; std::wstring path; };
    std::vector<Pending> stack;
    stack.push_back({0, rootExt_});
    std::vector<std::wstring> segs;
    std::vector<std::wstring> subNames;
    std::vector<BYTE> buf(64 * 1024);   // FILE_ID_BOTH_DIR_INFO batches
    std::vector<u32> markerCands;       // root AMD/NVIDIA folders, decided after the scan

    while (!stack.empty()) {
        if (cancelled()) return false;
        Pending cur = std::move(stack.back());
        stack.pop_back();
        const u32 di = cur.dir;
        if (prog) prog->SetCurrent(sys::ToShell(cur.path));

        // Root-relative lower-case segments of this folder, for the drive-root-relative rules.
        segs.clear();
        for (u32 x = di; x != 0; x = dirs[x].parent) segs.push_back(LowerName(Name(dirs[x].name), wcslen(Name(dirs[x].name))));
        std::reverse(segs.begin(), segs.end());

        const u8 ctx = dirs[di].ctx, dsafe = dirs[di].safe;
        // Root AMD/NVIDIA: flagged only if setup.exe or Packages is somewhere inside. Not knowable
        // until its subtree is listed, so it is decided after the scan (MarkInstallerFolders).
        if (!dsafe && rules.NeedsInstallerMarker(segs)) markerCands.push_back(di);
        const int depth = (int)dirs[di].depth;
        const u32 fileFirst = (u32)files.size();
        subNames.clear();
        DWORD serial = 0;
        bool haveIds = false;

        auto onEntry = [&](const Entry& e) {
            if (IsDots(e.name, e.len)) return;
            const bool isDir = (e.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // Cloud placeholders: never enumerate into them, never count them as local (deviation 4).
            if (e.attrs & kCloudMask) {
                if (isDir) stats.cloudDirs++;
                else { stats.cloudFiles++; stats.cloudBytes += e.size; }
                return;
            }
            const bool reparse = (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (isDir && !reparse) { subNames.emplace_back(e.name, e.len); return; }

            // A file, or any reparse point (shown as a leaf at its reported size; deviation 3).
            std::wstring ln = LowerName(e.name, e.len);
            FileE f;
            f.size = isDir ? 0 : e.size;
            f.dir = di;
            f.tag = reparse ? e.tag : 0;
            f.flags = reparse ? (u8)(FF_REPARSE | (isDir ? FF_REPARSE_DIR : 0)) : (u8)0;
            if (isDir) { f.cat = ctx != NOCTX ? ctx : (u8)C_OTHER; stats.reparseDirs++; }
            else {
                f.cat = FileCat(ctx, ln, depth);
                if (reparse) {
                    stats.reparseFiles++;
                    // WOF-compressed and dedup files are data, counted at logical size; anything else
                    // (symlink, AppExecLink, WSL link, unknown tag) is a link we don't follow.
                    if (e.tag != 0x80000017u /*IO_REPARSE_TAG_WOF*/ && e.tag != 0x80000013u /*DEDUP*/) stats.linkFiles++;
                }
                // Hard links: count each (volume serial, FileId) once (deviation 2).
                if (haveIds && e.fileId != 0 && e.fileId != ~0ULL && !seen[serial].Insert(e.fileId)) {
                    stats.hardlinkDupFiles++;
                    stats.hardlinkDupBytes += f.size;
                    f.size = 0;
                    f.flags |= FF_HARDLINK_DUP;
                }
                // Optional alternate data streams: counted files only (never reparse, cloud or a
                // duplicate link). The stream list is metadata; no stream data is read.
                if (streams_ && !reparse && !(f.flags & FF_HARDLINK_DUP)) {
                    std::wstring fp = Join(cur.path, e.name, e.len);
                    WIN32_FIND_STREAM_DATA sd;
                    HANDLE sh = FindFirstStreamW(fp.c_str(), FindStreamInfoStandard, &sd, 0);
                    if (sh != INVALID_HANDLE_VALUE) {   // failure: count 0 for this file and carry on
                        u64 extra = 0;
                        do {
                            size_t nl = wcslen(sd.cStreamName);
                            if (wcscmp(sd.cStreamName, L"::$DATA") != 0 && nl > 6 &&
                                wcscmp(sd.cStreamName + nl - 6, L":$DATA") == 0)
                                extra += (u64)sd.StreamSize.QuadPart;
                        } while (FindNextStreamW(sh, &sd));
                        FindClose(sh);
                        if (extra) { stats.adsFiles++; stats.adsBytes += extra; f.size += extra; }
                    }
                }
            }
            f.safe = dsafe ? dsafe : rules.FileSafe(segs, ln);
            f.name = AddName(e.name, e.len);
            files.push_back(f);
            if (prog) { prog->files.fetch_add(1, std::memory_order_relaxed); prog->bytes.fetch_add(f.size, std::memory_order_relaxed); }
        };

        DWORD err = 0;
        bool listed = false, infoClassErr = false, becameLink = false;
        HANDLE h = CreateFileW(cur.path.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // Re-check at open time (CLAUDE.md rule 1): the folder may have become a link or a cloud
            // placeholder since its parent was listed. Then it is a leaf and is never enumerated.
            FILE_ATTRIBUTE_TAG_INFO ti{};
            if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof(ti)) &&
                (ti.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | kCloudMask))) {
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
                becameLink = true;
            }
        }
        if (becameLink) {
            if (di == 0) { rootErr = ERROR_CANT_ACCESS_FILE; return false; }   // the root itself is a link
            stats.reparseDirs++;
            listed = true;   // shown as an empty folder; nothing behind it was read
        } else if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION bi{};
            if (GetFileInformationByHandle(h, &bi)) serial = bi.dwVolumeSerialNumber;
            haveIds = true;
            bool first = true;
            for (;;) {
                if (!GetFileInformationByHandleEx(h, FileIdBothDirectoryInfo, buf.data(), (DWORD)buf.size())) {
                    DWORD e = GetLastError();
                    if (e == ERROR_NO_MORE_FILES) { listed = true; break; }
                    // An empty folder with no "." / ".." entries (FAT/exFAT roots) answers the first
                    // query with ERROR_FILE_NOT_FOUND: listed, empty, not an error.
                    if (first && e == ERROR_FILE_NOT_FOUND) { listed = true; break; }
                    if (first && (e == ERROR_INVALID_PARAMETER || e == ERROR_INVALID_LEVEL ||
                                  e == ERROR_NOT_SUPPORTED || e == ERROR_INVALID_FUNCTION)) { err = e; infoClassErr = true; break; }
                    err = e; listed = !first; break;   // mid-listing failure: keep what we got, tally it
                }
                first = false;
                auto* p = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buf.data());
                for (;;) {
                    Entry e{p->FileName, p->FileNameLength / sizeof(wchar_t), p->FileAttributes,
                            (u64)p->EndOfFile.QuadPart, (u64)p->FileId.QuadPart,
                            (p->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? (u32)p->EaSize : 0u};
                    onEntry(e);
                    if (cancelled()) { CloseHandle(h); return false; }   // latency: one entry
                    if (!p->NextEntryOffset) break;
                    p = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<BYTE*>(p) + p->NextEntryOffset);
                }
            }
            CloseHandle(h);
            if (!listed && err == 0) err = ERROR_GEN_FAILURE;
        } else {
            err = GetLastError();
        }

        // Fallback: FindFirstFileExW, no FileIds (per-link counting). Only when the folder opened but
        // the FS rejected the info class, and only if its attributes (metadata only, no recall) still
        // say plain folder: FindFirstFileExW would follow a final-component junction or symlink.
        bool fallbackOk = false;
        if (infoClassErr && !listed && files.size() == fileFirst && subNames.empty()) {
            DWORD a = GetFileAttributesW(cur.path.c_str());
            fallbackOk = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
                         !(a & (FILE_ATTRIBUTE_REPARSE_POINT | kCloudMask));
        }
        if (fallbackOk) {
            std::wstring pat = Join(cur.path, L"*", 1);
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                         FIND_FIRST_EX_LARGE_FETCH);
            if (fh != INVALID_HANDLE_VALUE) {
                haveIds = false;
                listed = true;
                dirs[di].flags |= DF_NOID;
                stats.noIdDirs++;
                do {
                    Entry e{fd.cFileName, wcslen(fd.cFileName), fd.dwFileAttributes,
                            ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, 0,
                            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? (u32)fd.dwReserved0 : 0u};
                    onEntry(e);
                    if (cancelled()) { FindClose(fh); return false; }
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
                err = 0;
            }
        }

        if (!listed || err) {
            dirs[di].flags |= DF_DENIED;
            dirs[di].err = err;
            stats.deniedDirs++;
            if (stats.deniedSample.size() < kDeniedSample)
                stats.deniedSample.push_back({sys::ToShell(cur.path), err});
            if (di == 0 && !listed) { rootErr = err; return false; }
        }

        dirs[di].fileFirst = fileFirst;
        dirs[di].fileCount = (u32)files.size() - fileFirst;
        dirs[di].kidFirst = (u32)kids.size();
        dirs[di].kidCount = (u32)subNames.size();
        for (auto& sn : subNames) {
            std::wstring ln = LowerName(sn);
            DirE c;
            c.name = AddName(sn.c_str(), sn.size());
            c.parent = di;
            c.depth = (u32)depth + 1;
            c.ctx = DirCtx(ctx, ln, depth + 1);
            segs.push_back(ln);
            c.safe = dsafe ? dsafe : rules.DirSafe(segs);
            segs.pop_back();
            kids.push_back((u32)dirs.size());
            dirs.push_back(c);
        }
        // Push in reverse so the first sub-folder is listed next (depth-first, like the legacy).
        for (size_t i = subNames.size(); i-- > 0;) {
            u32 ci = kids[dirs[di].kidFirst + (u32)i];
            stack.push_back({ci, Join(cur.path, subNames[i].c_str(), subNames[i].size())});
        }
    }

    if (cancelled()) return false;
    MarkInstallerFolders(markerCands);
    Finalize();
    stats.seconds = (double)(GetTickCount64() - t0) / 1000.0;
    ok = true;
    return true;
}

void Scan::MarkInstallerFolders(const std::vector<u32>& cands) {
    std::vector<u32> st, sub;
    for (u32 c : cands) {
        bool marker = false;
        sub.clear();
        st.assign(1, c);
        while (!st.empty()) {
            u32 d = st.back(); st.pop_back();
            sub.push_back(d);
            const DirE& de = dirs[d];
            for (u32 f = de.fileFirst; f < de.fileFirst + de.fileCount && !marker; f++) {
                if (files[f].flags & FF_REPARSE) continue;   // a link named setup.exe is not installer data
                const wchar_t* n = Name(files[f].name);
                marker = IsInstallerMarker(LowerName(n, wcslen(n)), false);
            }
            for (u32 k = 0; k < de.kidCount; k++) {
                u32 ch = kids[de.kidFirst + k];
                const wchar_t* n = Name(dirs[ch].name);   // kids are plain folders; links are files
                if (!marker) marker = IsInstallerMarker(LowerName(n, wcslen(n)), true);
                st.push_back(ch);
            }
        }
        if (!marker) continue;
        for (u32 d : sub) {
            dirs[d].safe = SF_DRVPKG;
            for (u32 f = dirs[d].fileFirst; f < dirs[d].fileFirst + dirs[d].fileCount; f++) files[f].safe = SF_DRVPKG;
        }
    }
}

// Legacy Finalize (511-541): per-folder sums, files by size desc, bottom-up roll-up (children always
// have larger indices than their parent), dominant category, kids by total desc.
void Scan::Finalize() {
    size_t nd = dirs.size();
    for (size_t i = 0; i < nd; i++) {
        DirE& d = dirs[i];
        for (u32 f = d.fileFirst; f < d.fileFirst + d.fileCount; f++) {
            d.cat[files[f].cat] += files[f].size;
            d.safeR[files[f].safe] += files[f].size;
            d.total += files[f].size;
        }
        d.nFiles = 0;   // folder links are leaves in the file list but not files
        for (u32 f = d.fileFirst; f < d.fileFirst + d.fileCount; f++) if (!(files[f].flags & FF_REPARSE_DIR)) d.nFiles++;
        std::stable_sort(files.begin() + d.fileFirst, files.begin() + d.fileFirst + d.fileCount,
                         [](const FileE& a, const FileE& b) { return a.size > b.size; });
    }
    for (size_t i = nd; i-- > 1;) {
        DirE& d = dirs[i];
        DirE& p = dirs[d.parent];
        p.total += d.total;
        for (int c = 0; c < NCAT; c++) p.cat[c] += d.cat[c];
        for (int c = 1; c < NSAFE; c++) p.safeR[c] += d.safeR[c];
        p.nFiles += d.nFiles;
        p.nDirs += d.nDirs + 1;
    }
    for (size_t i = 0; i < nd; i++) {
        DirE& d = dirs[i];
        int best = C_OTHER; u64 bv = 0;
        for (int c = 0; c < NCAT; c++) if (d.cat[c] > bv) { bv = d.cat[c]; best = c; }
        d.dom = (u8)best;
        std::stable_sort(kids.begin() + d.kidFirst, kids.begin() + d.kidFirst + d.kidCount,
                         [this](u32 a, u32 b) { return dirs[a].total > dirs[b].total; });
    }
}

}  // namespace hx::diskmap
