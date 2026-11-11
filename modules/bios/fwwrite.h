// modules/bios/fwwrite.h — the one guarded firmware write path (hard rule 2, docs/design/bios-write.md).
//
// Only fwwrite.cpp calls the UEFI variable write API. Everything else here is either pure plan logic
// (no Win32) or the Execute/Restore engine that runs on a Worker through an FwBackend, so the
// selftest drives all of it with MockBackend and never touches real firmware.
//
// What a write may do, enforced here (and again in RealBackend::Write):
// - only variables of the settings map, only ones that already exist with attributes exactly
//   NV|BS|RT (0x7): authenticated, time-based authenticated, append and hardware-error-record
//   variables (0x8/0x10/0x20/0x40, and 0x80 or any unknown bit) are refused;
// - the same size and attributes as read, never size 0 (that would delete the variable);
// - only the bytes of the planned settings change, and only to values from the option list,
//   0/1 for a checkbox, or the numeric range/step;
// - SMBIOS board/BIOS guard re-checked, every byte re-read and compared before writing (drift),
//   whole-variable backups of every planned variable flushed to disk before the first write,
//   journal line flushed before and after each write, read-back verify and automatic restore,
//   stop at the first failure.
#pragma once

#include "modules/bios/db.h"
#include "modules/bios/firmware.h"
#include "modules/bios/snapshot.h"
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace hx::bios {

// UEFI variable attributes.
constexpr DWORD kAttrNV = 0x1, kAttrBS = 0x2, kAttrRT = 0x4;
constexpr DWORD kAttrRequired = kAttrNV | kAttrBS | kAttrRT;
// 0x8 hardware error record, 0x10 authenticated write, 0x20 time-based authenticated write,
// 0x40 append write, 0x80 enhanced authenticated access. Any of these (or any unknown bit) refuses.

// ---- backends -----------------------------------------------------------------------------------
struct FwBackend {
    virtual ~FwBackend() = default;
    virtual void Read(FwVar* v) = 0;     // fills data/attr/err for v->name + v->guid
    // Writes the whole variable. 0 on success, else a Win32 error code.
    virtual DWORD Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data,
                        DWORD attr) = 0;
    virtual bool Smbios(SmbiosInfo* out, DWORD* err) = 0;
    // Whether the privilege the calling thread holds is enough to write. Real: the ThreadPrivilege
    // succeeded. Mock: configurable, so the selftest proves Execute refuses without it.
    virtual bool PrivilegeOk(const ThreadPrivilege& p, DWORD* err) = 0;
    virtual bool IsReal() const = 0;
};

// The real firmware. Write refuses with ERROR_ACCESS_DENIED when the process-wide test latch is set
// (sys::NoFirmwareWrites), and with ERROR_INVALID_PARAMETER for size 0, attributes other than 0x7, or
// a (name, guid) outside the allowed set (the settings map's variables).
class RealBackend : public FwBackend {
public:
    explicit RealBackend(std::set<VarKey> allowed) : allowed_(std::move(allowed)) {}
    void Read(FwVar* v) override;
    DWORD Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data,
                DWORD attr) override;
    bool Smbios(SmbiosInfo* out, DWORD* err) override;
    bool PrivilegeOk(const ThreadPrivilege& p, DWORD* err) override;
    bool IsReal() const override { return true; }
    int apiCalls = 0;   // times the write API was reached (selftest: stays 0 under the latch)
private:
    std::set<VarKey> allowed_;
};

// In-memory variables for the selftest and the --screenshot plan demo. Fault injection hooks.
class MockBackend : public FwBackend {
public:
    std::map<VarKey, FwVar> vars;          // key = MakeVarKey(name, guid)
    SmbiosInfo smbios;
    DWORD smbiosErr = 0;
    bool privilege = true;
    std::wstring failWriteOf;              // Write of this variable fails with failWriteErr
    DWORD failWriteErr = ERROR_WRITE_PROTECT;
    std::wstring corruptWriteOf;           // Write of this variable stores data with one byte flipped
    int writes = 0;
    std::vector<std::wstring> writeLog;    // variable names, in write order
    void Put(const std::wstring& name, const std::wstring& guid, std::vector<uint8_t> data, DWORD attr = 0x7);
    void Read(FwVar* v) override;
    DWORD Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data,
                DWORD attr) override;
    bool Smbios(SmbiosInfo* out, DWORD* err) override;
    bool PrivilegeOk(const ThreadPrivilege&, DWORD* err) override;
    bool IsReal() const override { return false; }
};

// ---- pure plan logic ----------------------------------------------------------------------------
// True when v is allowed for s: an option value (oneof), 0/1 (checkbox), or mn <= v <= mx and
// (v - mn) % step == 0 when step > 0 (numeric). *why gets the reason on false.
bool ValidateValue(const Setting& s, long long v, std::wstring* why);
// Little-endian into out[0..width). Fails for width not 1/2/4/8, a negative v, a v that doesn't fit in
// width bytes unsigned, or when DecodeValue of the result doesn't give v back.
bool EncodeValue(long long v, int width, uint8_t* out);

struct PlanGate { bool live = false, guardMatched = false, switchOn = false; };
struct StagedChange { int setting = -1; long long value = 0; };

struct PlanSetting {
    int setting = -1;                       // index into Db::S (-1: bytes no mapped setting covers)
    std::wstring prompt, path, oldText, newText;
    long long oldV = 0, newV = 0;
    int offset = 0, width = 0;
    std::vector<uint8_t> oldBytes, newBytes;
    std::wstring flags;                     // "Hidden in the BIOS menu" / "Shown only when ..." or empty
    std::wstring shares;                    // other mapped settings stored in the same bytes, or empty
};
struct PlanVar {
    std::wstring name, guid;                // guid lowercase, no braces
    DWORD attr = 0;
    std::vector<uint8_t> oldData, newData;  // whole variable; newData differs only inside ranges
    std::vector<std::pair<int, int>> ranges;   // (offset, length) of the planned bytes
    std::vector<PlanSetting> settings;
};
struct Plan {
    bool restore = false;
    std::wstring restoreFrom;               // restore: the backup folder
    std::wstring board, bios;               // the map's B line, re-checked by Execute
    std::vector<PlanVar> vars;
    std::vector<std::wstring> notes;        // dropped no-ops, skipped variables
    int SettingCount() const;
    std::wstring Title() const;             // "Write N settings to M variables" / "Restore M variables ..."
    std::wstring Text() const;              // the human plan (plan.txt, the log)
};

// The plan for staged changes over the last live read. Refuses (false + *err) on a closed gate, an
// invalid value, a missing/unread variable, bad attributes, overlapping settings or bytes past the end.
bool BuildPlan(const Db& db, const std::vector<StagedChange>& staged, const std::vector<FwVar>& live,
               const PlanGate& gate, Plan* plan, std::wstring* err);
// The plan that puts a backup's bytes back. Every backed-up variable the map knows is compared with
// the live read; a size or attribute mismatch refuses the whole restore; equal variables are skipped.
// backupBoard/backupBios: the backup's system.txt lines, which must match the map's B line.
bool BuildRestorePlan(const Db& db, const VarMap& backup, const std::wstring& backupDir,
                      const std::wstring& backupBoard, const std::wstring& backupBios,
                      const std::vector<FwVar>& live, const PlanGate& gate, Plan* plan, std::wstring* err);

// ---- execute ------------------------------------------------------------------------------------
enum class VarState { NotAttempted, Written, Restored, Failed, RestoreFailed };
struct VarOutcome {
    std::wstring name, guid;
    VarState state = VarState::NotAttempted;
    DWORD err = 0;
    std::wstring text;
};
struct ExecResult {
    bool ok = false;                        // every planned variable written and verified
    bool started = false;                   // the backup session exists (something may have been written)
    std::wstring backupDir;                 // shell form, no \\?\ prefix
    std::wstring error;                     // why it stopped, when !ok
    std::vector<VarOutcome> vars;
    int written = 0;
    std::wstring Summary() const;
};
struct ExecOptions {
    std::wstring stamp;                     // backup folder name; empty = local time yyyyMMdd-HHmmss
    std::wstring lockName;                  // write-lock mutex; empty = kWriteMutexName (selftests use their own)
};
using ProgressFn = std::function<void(int done, int total, const std::wstring& what)>;

// ---- trust: the backup folders are admin-only ------------------------------------------------------
// The restore and the "didn't finish" banner trust only folders and files a non-elevated process can't
// have changed: otherwise a standard user could plant a "backup" (with a matching manifest) and have the
// elevated Hadron write its bytes. The policy (SdTrusted): the owner is BUILTIN\Administrators or
// SYSTEM; there is a DACL and it isn't NULL; no access-allowed ACE (any allowed type, inherit-only ones
// included) grants a write, append, attribute/EA write, delete, delete-child, WRITE_DAC, WRITE_OWNER,
// GENERIC_WRITE, GENERIC_ALL or MAXIMUM_ALLOWED right to any SID other than SYSTEM or Administrators.
bool SdTrusted(PSECURITY_DESCRIPTOR sd, std::wstring* why);
// Reads path's owner and DACL (GetNamedSecurityInfoW) and applies SdTrusted.
bool PathTrusted(const std::wstring& path, std::wstring* why);
// What LoadBackupSession / FindIncompleteSession use to judge a path; empty = PathTrusted.
using TrustFn = std::function<bool(const std::wstring& path, std::wstring* why)>;
// The descriptor every backup folder and file is created with: owner Administrators, a protected DACL
// with SYSTEM and Administrators full control and Authenticated Users read/execute, inherited by children.
constexpr const wchar_t* kBackupSddl = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;AU)";
// Creates root's parents (plain) and root itself with kBackupSddl; if root already exists (a folder, not
// a link), re-applies that owner and protected DACL to it (SetNamedSecurityInfoW).
bool PrepareBackupRoot(const std::wstring& root, std::wstring* err);

// Creates <root>\<stamp>[-N]\ with kBackupSddl (never reuses a folder: -2, -3 ... on ERROR_ALREADY_EXISTS),
// preparing root first (PrepareBackupRoot). *dir gets the full path.
bool CreateBackupSession(const std::wstring& root, const std::wstring& stamp, std::wstring* dir, std::wstring* err);

// SHA-256 (BCrypt) as 64 lowercase hex digits. false + *err when BCrypt fails.
bool Sha256Hex(const std::vector<uint8_t>& data, std::wstring* hex, std::wstring* err);
// The board / BIOS strings a snapshot's system.txt carries for this SMBIOS read (what SnapLoad's
// sysBoard / sysBios come back as).
std::wstring SmbiosBoardLine(const SmbiosInfo& smb);
std::wstring SmbiosBiosLine(const SmbiosInfo& smb);
// manifest.txt of a backup session, tab-separated lines: "Hadron BIOS backup manifest 2"; "board", the
// board line; "bios", the BIOS line (both from the SMBIOS read at backup time); then one line per
// variable: name, guid, size, attributes (hex), SHA-256 of the bytes. Written CREATE_NEW + flushed by
// Execute before the journal opens, so only Hadron's own sessions have one.
bool ManifestText(const std::vector<FwVar>& vars, const SmbiosInfo& smb, std::string* text, std::wstring* err);

// Loads a backup session for a restore, refusing (false + out->error, no vars) unless it is one of
// Hadron's own: dir is a folder (not a link) directly under backupRoot (both resolved with
// GetFinalPathNameByHandleW); the root, the session folder and everything in it pass trust (no links);
// it has manifest.txt (v2) and journal.txt; it loads as a snapshot; its variables are exactly the
// manifest's, each with the manifest's size, NV|BS|RT attributes and SHA-256; and system.txt agrees with
// the manifest's board/BIOS lines. sysBoard/sysBios come from the manifest, for BuildRestorePlan's guard.
// Worker thread: reads files only.
bool LoadBackupSession(const std::wstring& backupRoot, const std::wstring& dir, SnapLoad* out, const TrustFn& trust = {});

// The named mutex Execute holds for its whole run. Global: firmware is machine-wide, so two Hadron
// windows in any session (suite and standalone, two copies, two users) never write at the same time.
constexpr const wchar_t* kWriteMutexName = L"Global\\Hadron.bios.fwwrite";

// Runs on the Worker. Holds the write lock (opt.lockName, or kWriteMutexName) for the whole run and
// refuses when it can't be created or opened (access denied, say), when another thread or process holds
// it, or when its last holder ended without releasing it (WAIT_ABANDONED: a write stopped midway).
// Holds SeSystemEnvironmentPrivilege on this thread only (ThreadPrivilege) and
// refuses without it. Steps 1-3 (SMBIOS guard, drift check, backup of every planned variable) run for
// all variables before the first write; then per variable: journal BEGIN, re-check drift, write,
// read back, journal OK, or restore and stop. cancelled() is polled before each variable only.
bool Execute(FwBackend& be, const Plan& plan, const std::wstring& backupRoot, const std::function<bool()>& cancelled,
             const ProgressFn& progress, ExecResult* out, const ExecOptions& opt = {});

// ---- journals -----------------------------------------------------------------------------------
// True when the journal text has a BEGIN for a variable with no OK / RESTORED / FAILED after it
// (a write that may not have finished, or a restore that failed). *vars gets those variable names.
bool JournalIncomplete(const std::string& journal, std::vector<std::wstring>* vars);
struct IncompleteSession { std::wstring dir, stamp; std::vector<std::wstring> vars; };
// Looks at the newest trusted session under root (by the folder's creation time, name breaking ties,
// among the newest `newest`) that has a journal and reports it when that journal is incomplete. Nothing
// is reported when the root fails trust; a session folder or journal that fails trust is skipped and
// logged. UI thread: reads one small file.
bool FindIncompleteSession(const std::wstring& root, IncompleteSession* out, int newest = 8, const TrustFn& trust = {});

// %LOCALAPPDATA%\Hadron\bios-backups (not created here). Empty when the folder can't be resolved.
std::wstring DefaultBackupRoot();

std::wstring HexBytes(const std::vector<uint8_t>& b, size_t max = 16);

}  // namespace hx::bios
