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

enum class PlanKind { Preset, Single, Undo, Resume, Control };

struct PlanRow {
    std::wstring name, display;
    bool driver = false;
    StartCfg before, after;
    WriteOp op;
    std::vector<std::wstring> deps;    // the item's DependOnService, for ordering
    bool include = true, canToggle = true;
    std::wstring blocked;              // non-empty: shown for information, never applied
    std::wstring reason, basis, basisNote, evidence, warning, contested;
    std::wstring depWarning;           // DependencyCheck's own warning, recomputed on every toggle
    bool depUnticked = false;          // DependencyCheck unticked it (ticked again when the reason goes)
    bool needsTi = false;
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
    int Included() const;
    int Optional() const;
    int Blocked() const;
};

Plan BuildPreset(int level, int tab, const std::vector<SvcItem>& items, const Ctx& c);
Plan BuildDefault(bool stock, int tab, const std::vector<SvcItem>& items, const Ctx& c);
Plan BuildSingle(const SvcItem& item, StartCfg target, const std::vector<SvcItem>& items, const Ctx& c);
// Stop / Start / Restart a service (spec §5.2): dependents that will stop are listed first.
Plan BuildControl(const SvcItem& item, int what /*0 start, 1 stop, 2 restart*/, Backend& b, const std::vector<SvcItem>& items);
// Undo and Resume refuse a log whose machine GUID or build differs from the PC's (empty plan, the reason as a
// note), and run every row through the same guards as a plan (CheckGuard on the live item, no Boot start, no
// Boot/System on a service, System-start drivers only with sysok): a log has a CRC, not an author.
Plan BuildUndo(const UndoLog& log, Backend& b, const Ctx& c);
Plan BuildResume(const UndoLog& log, Backend& b, const std::vector<SvcItem>& items, const Ctx& c);
// Marks rows whose admin write is refused (CanWrite) as "needs TrustedInstaller"; with the toggle off they
// are excluded with the reason shown. The TrustedInstaller service itself is blocked while the toggle is on.
void MarkTi(Plan* p, Backend& b, bool tiOn);
// Spec §5.3, both directions, against the start every item ends with if the ticked rows are applied:
// disabling X while an item that stays enabled depends on X, or raising X while a dependency of X stays
// Disabled: warning, row unticked. Iterates to a fixed point. Run again after every toggle (toggled = that
// row): warnings are recomputed, rows it unticked come back when their reason goes, the toggled row is the
// user's choice.
void DependencyCheck(Plan* p, const std::vector<SvcItem>& items, int toggled = -1);
std::string DumpPlan(const Plan& p);   // the golden text form
// Included rows in apply order: lowering rows dependents-first, then raising rows dependencies-first.
std::vector<size_t> ApplyOrder(const Plan& p);

// ---- TrustedInstaller --------------------------------------------------------------------------------
class TiProvider {
public:
    virtual ~TiProvider() = default;
    virtual bool Acquire(std::wstring* err) = 0;        // once per apply: SeDebug, SYSTEM, TI token
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
};

struct ApplyResult {
    bool aborted = false, crashed = false, cancelled = false, nothing = false;
    int ok = 0, skipped = 0, failed = 0;
    std::wstring stamp, logPath, snapPath, recoveryDir, zipPath, error, tiNote;
    std::vector<std::wstring> lines;   // one per row: "ok X: Manual -> Disabled", "skip Y: changed since the plan"
};

bool RunApply(const Plan& p, const ApplyEnv& env, ApplyResult* r);
// Live start/stop/restart (control plans): confirm + log line, no undo entry (spec §5.2).
bool RunControl(const Plan& p, const ApplyEnv& env, ApplyResult* r);

// ---- logs, interrupted applies, drift ------------------------------------------------------------------
std::vector<UndoLog> LoadLogs(const Store& s, std::vector<std::wstring>* problems);
int FindInterrupted(const std::vector<UndoLog>& logs);   // newest B-without-E log not resolved by a finished undo/resume
// The log an Undo of logs[k] should start from: for an interrupted "undo:X" log that is X itself (undoing the
// undo would re-apply X), otherwise k.
int UndoBase(const std::vector<UndoLog>& logs, int k);
int FindUndoable(const std::vector<UndoLog>& logs);      // newest finished log with changes, not undone yet
bool TiLeftRunning(const std::vector<UndoLog>& logs);    // a log says Hadron started TI and never stopped it
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
