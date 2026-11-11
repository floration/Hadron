// modules/services/regfallback.h — the registry-write fallback for start-type changes the SCM refuses (svcdrv
// follow-up 9; docs/design/services.md "Registry fallback").
//
// Some services' security descriptors refuse SERVICE_CHANGE_CONFIG to administrators (WaaSMedicSvc on Windows 10 is
// the usual example). With the per-apply toggle on (off by default, never persisted, off again after every apply),
// such a row is planned as a direct write of HKLM\SYSTEM\ControlSet00N\Services\<name>\Start (and DelayedAutostart),
// shown in the plan as its own line, written only after the user confirms, with the undo-log I record (the old
// values) flushed before the write, the value re-read right before writing and read back after. The SCM reads Start
// at boot, so the change takes effect after a restart. Never for launch-protected services, never-touch items (the
// floor, the boot path, filters, Critical, Boot start), the named never-in-preset set, Boot/System-start drivers or a
// Boot/System target. Undo of such a row writes the registry back the same way (and needs the toggle too).
#pragma once

#include "modules/services/plan.h"
#include <set>
#include <string>
#include <vector>

namespace hx::services {

extern const wchar_t kRegLine[];       // the plan line of a forward row
extern const wchar_t kRegUndoLine[];   // the plan line of an undo or resume row of a registry write
extern const wchar_t kRegUndoOff[];    // that row's warning while the toggle is off
constexpr wchar_t kRegIdentity[] = L"reg";   // the undo log's identity field (P and I records)

// "" when the row may use the registry fallback, else why not. live: the service manager's view of the item; reg: its
// registry values now; before/target: the plan row's. c (may be null) adds the never-touch guards.
std::wstring RegRowRefusal(const SvcItem& live, const RegStartVals& reg, const StartCfg& before, const StartCfg& target, const Ctx* c);
// Runs before MarkTi. With regOn (and the TrustedInstaller toggle off: TI goes through the SCM and wins), every row the
// SCM refuses to an administrator (CanWrite) that passes RegRowRefusal becomes viaReg with the plan line kRegLine;
// one that doesn't keeps MarkTi's treatment, with the reason in regRefused. Rows already viaReg (Undo, Resume) are
// left out while the toggle is off. MarkTi skips viaReg rows and runs the dependency check again.
void MarkRegFallback(Plan* p, Backend& b, bool regOn, bool tiOn, const std::vector<SvcItem>& items, const Ctx& c);
// Names (lower case) whose last verified write, in finished logs, went through the registry: until a restart their
// service-manager start type still shows the old value, so drift is judged against the registry Start instead.
std::set<std::wstring> LastAppliedViaReg(const std::vector<UndoLog>& logs);
bool IsRegRow(const UndoRow& r);   // its P or I record names the registry identity

}  // namespace hx::services
