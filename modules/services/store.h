// modules/services/store.h — the data dir, the text formats (snapshot, undo log) and the writers.
//
// Data dir: %ProgramData%\Hadron\services\ (spec §5.4), DACL Administrators + SYSTEM only. Every file is
// created with CREATE_NEW and flushed; nothing is ever overwritten. In selftest and screenshot mode the
// root is injected (a temp dir or the fixture dir) and ProgramData is never touched.
//
// Formats (UTF-8, LF, TAB-separated, bios escaping, line 1 "HXSVC\t1\t<kind>", unknown lines skipped,
// 16 MB cap): see docs/design/services.md.
#pragma once

#include "modules/services/model.h"
#include <map>
#include <string>
#include <vector>

namespace hx::services {

constexpr size_t kMaxFile = 16u << 20;

struct Snapshot {
    std::map<std::wstring, std::wstring> meta;   // M lines: created, build, machine, kind, hadron
    std::vector<SvcItem> items;
    int skippedLines = 0;
    const SvcItem* Find(const std::wstring& name) const;
};
std::string WriteSnapshot(const Snapshot& s, bool withFixtureExtras = false);
bool ParseSnapshot(const std::string& utf8, Snapshot* out, std::wstring* err);

// ---- undo log ---------------------------------------------------------------------------------------
// HXSVC 1 undo
// B  stamp  source  count  machine  build  order   (begin; order: 1 + the highest order of the logs before it)
// P  seq  name  start  before  after  identity     (every plan row, up front)
// T  started|stopped                               (TrustedInstaller service started / stopped by Hadron)
// I  seq  observed  identity-verified              (flushed BEFORE the SCM call)
// D  seq  ok|skip|fail  observed-after  text       (flushed after the call and a re-read)
// E  stamp  ok  skipped  failed                    (end)
// Every record ends with a TAB and the CRC-32 (hex) of the text before it. A record with a bad or
// missing CRC (a torn last write) is ignored and counted.
struct UndoRow {
    int seq = 0;
    std::wstring name, identity;
    StartCfg before, after;
    bool hasI = false, hasD = false;
    StartCfg observed, afterObs;
    std::wstring result, text, iIdentity;
};
struct UndoLog {
    std::wstring file, stamp, source, machine, build;
    int count = 0, badRecords = 0, order = 0;
    bool hasB = false, hasE = false, tiStarted = false, tiStopped = false;
    std::vector<UndoRow> rows;
    UndoRow* Row(int seq);
};
bool ParseUndoLog(const std::string& utf8, UndoLog* out, std::wstring* err);
std::string Record(const std::vector<std::wstring>& fields);   // joined with TABs + CRC field + LF

// Set while the apply thread impersonates TrustedInstaller. Every Hadron file write refuses while it is
// set (spec §5.6: never impersonate while writing Hadron's own files). thread_local, so only that thread.
struct ImpersonationFlag { static bool& Active(); };

// An append-only log file: CREATE_NEW, share mode 0 (the apply lock's second half), every Append is
// WriteFile + FlushFileBuffers. failAfter >= 0 makes the (failAfter+1)-th flush fail (selftest).
class LogWriter {
public:
    ~LogWriter() { Close(); }
    bool Open(const std::wstring& path, std::wstring* err);
    bool Append(const std::string& utf8, std::wstring* err);
    void Close();
    int failAfter = -1;
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    int writes_ = 0;
};

// Named mutex Global\Hadron.services.apply: suite and standalone can't both apply.
class ApplyLock {
public:
    ~ApplyLock() { Release(); }
    bool Acquire(const std::wstring& name, std::wstring* err);
    void Release();
private:
    HANDLE m_ = nullptr;
    std::wstring name_;
};

class Store {
public:
    Store() = default;
    explicit Store(std::wstring root) : root_(std::move(root)) {}
    const std::wstring& Root() const { return root_; }
    void SetRoot(std::wstring r) { root_ = std::move(r); }
    bool Valid() const { return !root_.empty(); }

    static std::wstring DefaultRoot(std::wstring* err);    // %ProgramData%\Hadron\services
    // Creates the root (and missing parents, up to the volume or share root). secure: the root gets a protected
    // DACL, Administrators + SYSTEM full control, inherited. On every call (the folder may have been made by
    // someone else): the root and each parent must not be a reparse point; with secure, the root's owner must be
    // Administrators or SYSTEM and the protected DACL is (re)applied. Any failure: false, with the reason, and
    // the caller writes nothing there.
    bool EnsureRoot(bool secure, std::wstring* err);
    // The path without a \\?\ prefix (\\?\UNC\x -> \\x), for display and for the README.
    static std::wstring Plain(const std::wstring& path);
    // Selftest under Wine only: Wine ignores the owner in a security descriptor (every file is owned by the
    // user running Wine), so there the selftest lets this process's own user pass the owner check. The module
    // never sets it; on Windows the selftest runs the strict check.
    static bool& AcceptTokenUserAsOwner();
    // Selftest only, where the system can't make a reparse point (Wine): EnsureRoot treats this exact path (no
    // \\?\ prefix) as one. Empty, the default, injects nothing.
    static std::wstring& InjectedReparsePoint();
    std::wstring Path(const std::wstring& rel) const;
    bool Exists(const std::wstring& rel) const;
    bool MakeDir(const std::wstring& rel, std::wstring* err);
    bool WriteNew(const std::wstring& rel, const std::string& data, std::wstring* err);      // CREATE_NEW + flush
    // temp (CREATE_NEW) + flush + MoveFileExW(WRITE_THROUGH, no replace, no copy): fails if rel exists.
    bool WriteAtomicNew(const std::wstring& rel, const std::string& data, std::wstring* err);
    bool Read(const std::wstring& rel, std::string* out, std::wstring* err) const;
    // Renames a corrupt file aside ("<rel>.corrupt-<stamp>"), never deleting or overwriting it.
    bool RenameAside(const std::wstring& rel, std::wstring* newRel, std::wstring* err);
    std::vector<std::wstring> List(const std::wstring& pattern) const;   // file names, sorted
    // yyyyMMdd-HHmmss UTC, with -N when any snap-/undo-/recovery file with that stamp exists.
    std::wstring NewStamp() const;
    std::wstring fixedStamp;   // selftest: deterministic stamps (then -N suffixes)

private:
    std::wstring root_;
};

}  // namespace hx::services
