// modules/services/plan.h — plan building with every guard (spec §6), and the apply engine.
//
// Pure except the engine, which talks only to a Backend and the Store. Every change Hadron makes is a
// Plan: built here, shown in the in-view plan screen, confirmed by the user, then run by RunApply on a
// Worker. Undo and Resume are plans too, with their own confirm and log.
#pragma once

#include "modules/services/backend.h"
#include "modules/services/hw.h"
#include "modules/services/kb.h"
#include "modules/services/model.h"
#include "modules/services/recovery.h"
#include "modules/services/store.h"
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hx::services {

constexpr int LEVEL_DEFAULT = -1;   // with KbLevel 0..2 for the other three

struct Ctx {
    const Kb* kb = nullptr;
    const HwInfo* hw = nullptr;
    const Snapshot* first = nullptr;   // the first-run snapshot, when there is one
    std::wstring build;                // running build, e.g. "19045"
    std::wstring machine;              // machine GUID (first.txt must match)
    bool tiOn = false;
    // Filled by Prepare() from hw.
    std::set<std::wstring> filters, bootChain;
    void Prepare();
};

struct Guard {
    bool neverTouch = false;    // no preset, no single-item change
    bool neverPreset = false;   // single item allowed where Windows allows
    std::wstring reason;
};
Guard CheckGuard(const SvcItem& s, const Ctx& c);
bool IsStorageDriver(const std::wstring& name);   // never given a Windows-stock value (spec §0.5)
// The named never-in-preset set (Defender/Security, TrustedInstaller, VSS, the file-system filters Store apps and cloud
// files sit on): the registry fallback never writes them either.
bool IsNamedNeverPreset(const std::wstring& name);

enum class PlanKind { Preset, Single, Undo, Resume, Control };

struct PlanRow {
    std::wstring name, display;
    bool driver = false;
    StartCfg before, after;
    WriteOp op;
    std::vector<std::wstring> deps;    // the item's DependOnService, for ordering
    std::wstring group;                // the item's load-order group: a "+group" dependency orders it too (R17)
    bool include = true, canToggle = true;
    std::wstring blocked;              // non-empty: shown for information, never applied
    std::wstring reason, basis, basisNote, evidence, warning, contested;
    std::wstring depWarning;           // DependencyCheck's own warning, recomputed on every toggle
    bool depUnticked = false;          // DependencyCheck unticked it (ticked again when the reason goes)
    bool depKept = false;              // the user ticked it while it had a dependency warning: never unticked by the check
    bool needsTi = false;
    // Registry fallback (regfallback.h): the SCM refused this write and the per-apply toggle is on, or this row undoes
    // or resumes a registry write. Written to the service key directly; identity "reg" in the undo log.
    bool viaReg = false;
    std::wstring regNote;              // the plan's own line for it ("SCM refused; will write registry Start directly, ...")
    std::wstring regRefused;           // toggle on, but this row may not use it: why (shown with its TI warning)
    int seq = 0;                       // undo/resume: the original log sequence
    Op ctl = Op::Stop;                 // control plans: the operation for this row
};

struct Plan {
    PlanKind kind = PlanKind::Preset;
    int level = LEVEL_DEFAULT;
    int tab = TAB_SERVICES;
    bool stock = false;                // Default: Windows stock (else first snapshot)
    std::wstring source, title;
    std::vector<PlanRow> rows;
    int alreadySet = 0, unknown = 0;
    std::vector<std::wstring> notes;
    bool restartNeeded = true;         // start-type changes act at the next restart
    int what = -1;                     // control plans: 0 start, 1 stop, 2 restart
    DWORD tiPid = 0;                   // "ti-stop:" plans: the TrustedInstaller process Hadron started (R4)
    uint64_t tiPstart = 0;
    // What the plan screen shows instead of a bare "Nothing to change." when the plan has no rows for a reason the
    // user must see (R20: no Windows-stock data for this build). Also in notes.
    std::wstring emptyText;
    int Included() const;
    int Optional() const;
    int Blocked() const;
};

Plan BuildPreset(int level, int tab, const std::vector<SvcItem>& items, const Ctx& c);
// stock: Windows stock values from the KB (R20: an empty plan says Hadron has no data for the build). Else "as first
// seen": first.txt, only when it is from this PC (machine GUID) and this Windows build (R11).
Plan BuildDefault(bool stock, int tab, const std::vector<SvcItem>& items, const Ctx& c);
// "10.0.19045.5737" and "19045" -> "19045": the build number alone, for comparing builds.
std::wstring BuildNumber(const std::wstring& build);
Plan BuildSingle(const SvcItem& item, StartCfg target, const std::vector<SvcItem>& items, const Ctx& c);
// Stop / Start / Restart a service (spec §5.2): dependents that will stop are listed first.
// Never-touch items (and anything whose stop would stop one as a dependent) come back with every row blocked (R5).
Plan BuildControl(const SvcItem& item, int what /*0 start, 1 stop, 2 restart*/, Backend& b, const std::vector<SvcItem>& items,
                  const Ctx& c);
// Undo and Resume refuse a log whose machine GUID or build differs from the PC's (empty plan, the reason as a
// note), and run every row through the same guards as a plan (CheckGuard on the live item, no Boot start, no
// Boot/System on a service, System-start drivers only with sysok): a log has a CRC, not an author.
// Both run the dependency check against items (the loaded list) like every other plan source.
Plan BuildUndo(const UndoLog& log, Backend& b, const std::vector<SvcItem>& items, const Ctx& c);
Plan BuildResume(const UndoLog& log, Backend& b, const std::vector<SvcItem>& items, const Ctx& c);
// Marks rows whose admin write is refused (CanWrite) as "needs TrustedInstaller"; with the toggle off they
// are excluded with the reason shown. The TrustedInstaller service itself is blocked while the toggle is on.
// Rows it leaves out change what stays enabled, so it ends by running DependencyCheck again.
void MarkTi(Plan* p, Backend& b, bool tiOn, const std::vector<SvcItem>& items);
// Spec §5.3, both directions, against the start every item ends with if the ticked rows are applied:
// disabling X while an item that stays enabled depends on X, or raising X while a dependency of X stays
// Disabled: warning, row unticked. Iterates to a fixed point. Run again after every toggle (toggled = that
// row): warnings are recomputed, rows it unticked come back when their reason goes, the toggled row is the
// user's choice (a row the user ticks while it has a warning stays ticked, with the warning, until the warning
// goes). Plans that both raise and lower can go round in a circle (a pair whose warnings each exist only while
// the other row is ticked): when a state repeats, or at the round bound, it finishes with untick-only passes,
// which always settle, and adds a plan note starting with kDepNotSettled.
// Afterwards, for every row it may tick or untick: no ticked row disables an item that an item enabled at the
// end depends on, and no ticked row raises an item whose dependency ends Disabled (user-kept rows excepted;
// they show their warning).
extern const wchar_t* const kDepNotSettled;
void DependencyCheck(Plan* p, const std::vector<SvcItem>& items, int toggled = -1);
std::string DumpPlan(const Plan& p);   // the golden text form
// Included rows in apply order: lowering rows dependents-first, then raising rows dependencies-first.
std::vector<size_t> ApplyOrder(const Plan& p);

// ---- TrustedInstaller --------------------------------------------------------------------------------
class TiProvider {
public:
    virtual ~TiProvider() = default;
    // Once per apply: SeDebug, SYSTEM, TI token. cancel (may be null) is honoured by every wait, Release's included.
    virtual bool Acquire(const std::atomic<bool>* cancel, std::wstring* err) = 0;
    virtual bool StartedService() const = 0;            // Acquire started the TrustedInstaller service
    virtual bool Impersonate(std::wstring* identity, std::wstring* err) = 0;   // verified TI identity text
    virtual void Revert() = 0;
    virtual bool Release(std::wstring* note) = 0;       // stop TI only if Acquire started it; resets StartedService
};

// ---- apply ---------------------------------------------------------------------------------------------
struct ApplyEnv {
    Backend* backend = nullptr;
    Store* store = nullptr;
    TiProvider* ti = nullptr;
    bool tiOn = false;
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<uint64_t>* progress = nullptr;
    std::wstring lockName = L"Global\\Hadron.services.apply";
    std::wstring build, machine;
    int crashAfterBegin = -1;   // selftest: stop dead after the I record and the write of this seq
    int failFlushAfter = -1;    // selftest: the undo log's flush fails after this many records
    // R12: every plan but an Undo is refused while the data folder has no first.txt (it failed to write, or was
    // removed): a change with no record of the state before Hadron is never made. The selftest's fixtures turn it off.
    bool requireFirst = true;
    // The registry fallback's per-apply toggle (never persisted): rows marked viaReg are written only while it is on.
    bool regFallbackOn = false;
    // The guards' context for the registry rows' re-check at write time (never-touch, boot path); null skips that part
    // (the live item's protection and Boot/System start are re-checked regardless).
    const Ctx* ctx = nullptr;
};

struct ApplyResult {
    bool aborted = false, crashed = false, cancelled = false, nothing = false;
    int ok = 0, skipped = 0, failed = 0;
    std::wstring stamp, logPath, snapPath, recoveryDir, zipPath, error, tiNote;
    std::vector<std::wstring> lines;   // one per row: "ok X: Manual -> Disabled", "skip Y: changed since the plan"
};

bool RunApply(const Plan& p, const ApplyEnv& env, ApplyResult* r);
// Live start/stop/restart (control plans): confirm + log line, no undo entry (spec §5.2). A failed stop starts again,
// newest first, what this run already stopped (R13).
bool RunControl(const Plan& p, const ApplyEnv& env, ApplyResult* r);

// ---- logs, interrupted applies, drift ------------------------------------------------------------------
std::vector<UndoLog> LoadLogs(const Store& s, std::vector<std::wstring>* problems);
// B record sequence numbers. A new log gets 1 + the highest order before it, counting no log above kOrderCap; logs
// sort by OrderKey (the order clamped to kOrderCap + 1), then by stamp, so a forged huge order ties with the logs
// written after it instead of staying last for good (mini-analyze fixture run 4).
constexpr int kOrderCap = 1000000;
int OrderKey(int order);
int NextOrder(const std::vector<UndoLog>& logs);
int FindInterrupted(const std::vector<UndoLog>& logs);   // newest B-without-E log not resolved by a finished undo/resume
// The log an Undo of logs[k] should start from: for an interrupted "undo:X" log, or a resume of one, that is X itself
// (undoing the undo would re-apply X), otherwise k.
int UndoBase(const std::vector<UndoLog>& logs, int k);
// Newest finished log with changes, not undone yet. Never an undo's log or a resume of one (R8). A "resume:X" log is
// offered with X's rows (UndoLog::chain, R7).
int FindUndoable(const std::vector<UndoLog>& logs);
// A log stamp ("20261110-213012" or "20261110-213012-2", UTC) as the user reads it: "2026-11-10 21:30 UTC", like the
// header's dates (mini-build P3). Anything else is returned as it is.
std::wstring StampText(const std::wstring& stamp);
// "Stripped (Services)", "the undo of the apply of 2026-11-10 21:30 UTC": a log source in words, never a raw id.
std::wstring SourceLabel(const std::wstring& source);
// The interrupted-apply banner's text for logs[k] (stamp in UTC, source in words, a crash while starting TI noted).
std::wstring InterruptedText(const std::vector<UndoLog>& logs, int k);
// The user's Dismiss of an interrupted apply (R9): a resolution log "dismiss:<stamp>" (B, N, E; no rows), under the
// apply lock. FindInterrupted no longer returns that log. Nothing else changes; its log and recovery folder stay.
bool DismissInterrupted(Store& st, const UndoLog& log, const std::wstring& machine, const std::wstring& build,
                        const std::wstring& lockName, std::wstring* err);

// TrustedInstaller left running (R4). A log claims "Hadron started TrustedInstaller" when it has T started (or a
// T starting with no outcome after a crash) and no T stopped, and no "ti-resolved:<stamp>" log closes it. The claim is
// shown and acted on only while the running TrustedInstaller is that same process (pid and creation time from the
// T started record); a T started without them (older logs, or a start whose process couldn't be read) is never shown.
struct TiClaim {
    int log = -1;                 // index into logs; -1 = none
    std::wstring stamp, source, stopNote;
    bool finished = false;        // the apply has its E record
    bool verified = false;        // pid and creation time are recorded
    DWORD pid = 0;
    uint64_t pstart = 0;
};
TiClaim LiveTiClaim(const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items);
bool TiLeftRunning(const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items);
std::wstring TiLeftText(const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items);   // the banner, from data
// The banner's "Stop it": a control plan (BuildControl, stop TrustedInstaller) gets source "ti-stop:<stamp>", the
// claimed process and a reason from data; with no live claim every row is blocked. RunControl stops it only while it
// is still that process, then writes "ti-resolved:<stamp>".
void MarkTiStop(Plan* p, const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items);
// Load time: closes every open claim whose process is gone (TrustedInstaller stopped, or another process running) with
// a "ti-resolved:<stamp>" log. Returns how many it wrote; 0 when the apply lock is busy (the next load tries again).
// backend (the load passes it): TrustedInstaller is queried again under the lock and the logs re-read, and only what
// is still stale then is closed (mini-analyze A4). Without it, items and logs are trusted as given (tests).
int ResolveStaleTiClaims(Store& st, const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items, const std::wstring& machine,
                         const std::wstring& build, const std::wstring& lockName = L"Global\\Hadron.services.apply",
                         Backend* backend = nullptr);
// name (lower case) -> the value Hadron last wrote and verified, from finished logs in order.
std::map<std::wstring, StartCfg> LastApplied(const std::vector<UndoLog>& logs);

// First-run snapshot (spec §5.4): written once, atomically, read back; plus recovery\first and its zip.
struct FirstRunResult { bool wrote = false, existed = false, corrupt = false, foreign = false; std::wstring error, note; };
// c: the KB, hardware, build and machine GUID, for the guards FirstRestoreRows applies.
FirstRunResult EnsureFirstSnapshot(Store& s, const std::vector<SvcItem>& items, const Ctx& c, Snapshot* first);
// The rows recovery\first restores: the same exclusions as Default: restore first snapshot. *skipped gets the
// names left out (README section F).
std::vector<RestoreRow> FirstRestoreRows(const Snapshot& first, const Ctx& c, std::vector<std::wstring>* skipped);

}  // namespace hx::services
