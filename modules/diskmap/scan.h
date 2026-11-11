// modules/diskmap/scan.h — the DiskMap scan engine and its flat-array data model.
//
// Read-only: the scan opens directories for listing only (FILE_LIST_DIRECTORY with backup
// semantics) and never opens a file or reads file data. Approved deviations from the legacy
// (docs/design/diskmap.md): hard links are counted once per (volume serial, FileId); every reparse
// point is a leaf; cloud placeholders are skipped and tallied; SeBackupPrivilege is used so
// admin-only folders size; access-denied folders are tallied instead of silently empty.
//
// No UI and no Worker dependency: Run() takes a cancel predicate and an optional progress sink, so
// --selftest and --fixture scan synchronously and the module runs it on a core/sys Worker.
#pragma once

#include "modules/diskmap/classify.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace hx::diskmap {

// FileE.flags
enum : u8 {
    FF_HARDLINK_DUP = 1,   // another link to this FileId was already counted: size recorded as 0
    FF_REPARSE      = 2,   // reparse point, never followed (symlink, junction, mount, AppExecLink...)
    FF_REPARSE_DIR  = 4,   // ... and it was a directory (a link to a folder)
};

struct FileE {
    u64 size = 0;          // logical size (EndOfFile) as counted; 0 for a hard-link duplicate
    u32 name = 0;          // offset into Scan::names
    u32 dir = 0;           // owning DirE
    u32 tag = 0;           // reparse tag when FF_REPARSE (0 if the FS doesn't report one)
    u8  cat = C_OTHER;
    u8  safe = SF_NONE;
    u8  flags = 0;
};

// DirE.flags
enum : u8 {
    DF_DENIED  = 1,        // could not be listed (access denied or another error): recorded empty
    DF_NOID    = 2,        // listed without FileIds (fallback): hard links here counted per link
};

struct DirE {
    u32 name = 0, parent = 0;
    u32 fileFirst = 0, fileCount = 0;   // range in Scan::files (sorted by size desc after Finalize)
    u32 kidFirst = 0, kidCount = 0;     // range in Scan::kids (sorted by total desc after Finalize)
    u64 total = 0;                      // recursive bytes
    u64 cat[NCAT] = {};                 // recursive bytes per category
    u64 safeR[NSAFE] = {};              // recursive bytes per safe reason (index 0 = not flagged)
    u32 nFiles = 0, nDirs = 0;          // recursive counts
    u32 depth = 0;                      // 0 = root
    DWORD err = 0;                      // Win32 error when DF_DENIED
    u8  safe = SF_NONE;                 // own (inherited) reason
    u8  ctx = NOCTX;
    u8  dom = C_OTHER;                  // dominant category
    u8  flags = 0;
};

// What the scan left out or could not see, so the UI can say so instead of hiding it (rule 7).
struct ScanStats {
    u64 hardlinkDupFiles = 0, hardlinkDupBytes = 0;   // extra links not counted again
    u64 reparseFiles = 0, reparseDirs = 0;            // reparse points shown as leaves, never followed
    u64 linkFiles = 0;                                 // reparse files that are links (not WOF/dedup)
    u64 cloudFiles = 0, cloudBytes = 0, cloudDirs = 0; // placeholders skipped (bytes = logical size)
    u64 deniedDirs = 0;                                // folders that could not be listed
    u64 noIdDirs = 0;                                  // folders listed without FileIds (fallback)
    u64 adsFiles = 0, adsBytes = 0;                    // named :$DATA streams counted (streams option)
    bool streams = false;                              // the scan ran with the streams option on
    std::vector<std::pair<std::wstring, DWORD>> deniedSample;   // first few, display paths
    double seconds = 0;
    bool backupOk = false;          // SeBackupPrivilege was enabled on the scan thread
    DWORD backupErr = 0;            // why not, when !backupOk
};

// Live counters for the progress card. Written by the scan thread, read by the UI on a timer.
struct ScanProgress {
    std::atomic<u64> files{0}, bytes{0};
    void SetCurrent(const std::wstring& p) { AcquireSRWLockExclusive(&lock_); cur_ = p; ReleaseSRWLockExclusive(&lock_); }
    std::wstring Current() { AcquireSRWLockShared(&lock_); std::wstring s = cur_; ReleaseSRWLockShared(&lock_); return s; }
    void Reset() { files = 0; bytes = 0; SetCurrent(L""); }
private:
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::wstring cur_;
};

class Scan {
public:
    std::vector<FileE> files;
    std::vector<DirE> dirs;
    std::vector<u32> kids;
    std::vector<wchar_t> names;
    ScanStats stats;
    bool ok = false;          // root listed and Finalize ran
    DWORD rootErr = 0;        // why the root could not be listed

    // root: the folder to scan in \\?\ form (a drive root like \\?\C:\ or any folder). display: the
    // root's name as shown ("C:\"). The root is treated as a drive root for the safe-to-delete rules,
    // which are drive-root relative. Returns false if cancelled or the root can't be listed.
    // SeBackupPrivilege is enabled on the calling thread only (ImpersonateSelf), and reverted on return.
    // streams: also sum each counted file's named :$DATA streams (FindFirstStreamW) into its size.
    // Off by default: it opens every file's stream list, which is slow on large drives.
    bool Run(const std::wstring& root, const std::wstring& display,
             const std::function<bool()>& cancelled, ScanProgress* progress, bool streams = false);

    // Root AMD/NVIDIA candidates: stamp SF_DRVPKG on each candidate folder and its whole subtree when
    // a descendant non-reparse file setup.exe or a descendant folder Packages exists. In-memory only
    // (no I/O). Called by Run before Finalize; public so --selftest can drive it on a synthetic tree.
    void MarkInstallerFolders(const std::vector<u32>& candidates);
    u32 AddName(const wchar_t* s, size_t n);

    const wchar_t* Name(u32 off) const { return names.data() + off; }
    std::wstring DirPath(u32 d) const;            // display path, no \\?\ prefix
    std::wstring FilePath(u32 f) const;
    std::wstring ExtendedDirPath(u32 d) const;    // \\?\ form, for Win32 calls
    const u32* KidsOf(const DirE& d) const { return kids.data() + d.kidFirst; }

    // Hard-coded fixture expectations live in module.cpp's selftest; these helpers sum what it checks.
    u64 TotalBytes() const { return dirs.empty() ? 0 : dirs[0].total; }
    u64 SafeBytes(u32 d = 0) const;

private:
    bool RunImpl(const std::wstring& root, const std::wstring& display,
                 const std::function<bool()>& cancelled, ScanProgress* progress);
    void Finalize();
    std::wstring rootExt_;
    bool streams_ = false;
};

u64 SafeSum(const u64* safeR);   // reasons 1..9

}  // namespace hx::diskmap
