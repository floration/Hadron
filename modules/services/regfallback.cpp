// modules/services/regfallback.cpp — planning side of the registry fallback (regfallback.h). The write itself is
// RunApply's (plan.cpp) through Backend::WriteRegStart; the only live implementation is in backend_live.cpp.
#include "modules/services/regfallback.h"

namespace hx::services {

const wchar_t kRegLine[] = L"SCM refused; will write registry Start directly, takes effect after restart";
const wchar_t kRegUndoLine[] =
    L"Hadron wrote this to the registry directly; Undo writes the registry Start back the same way, takes effect after restart";
const wchar_t kRegRestartBasis[] =
    L"\"Takes effect after restart\" is reasoning (the service manager reads Start from the registry when Windows starts), "
    L"not tested here: check it after a restart; Windows Update Medic can put some of these back.";
const wchar_t kRegUndoOff[] =
    L"Hadron wrote this to the registry directly: turn the registry fallback on to put it back the same way";

std::wstring RegRowRefusal(const SvcItem& live, const RegStartVals& reg, const StartCfg& before, const StartCfg& target, const Ctx* c) {
    if (live.kind == Kind::UserInstance) return L"a per-user instance (change its template)";
    if (live.cfgUnknown) return L"its configuration could not be read";
    if (c) {
        Guard g = CheckGuard(live, *c);
        if (g.neverTouch) return L"protected: " + g.reason;
    }
    if (live.protect != 0) return L"launch-protected (" + ProtectName(live.protect) + L"): never written through the registry";
    if (IsNamedNeverPreset(TemplateName(live.name, live.type)))
        return L"Defender, Windows Security, TrustedInstaller, VSS or a file-system filter: never written through the registry";
    const bool drv = IsDriverType(live.type);
    if (drv && (live.cfg.start < 2 || (reg.start >= 0 && reg.start < 2))) return L"a Boot or System-start driver";
    if (!drv && reg.start >= 0 && reg.start < 2) return L"its registry Start is Boot or System";
    if (target.start < 2) return L"Hadron never writes Boot or System start";
    if (reg.start < 0) return L"its registry key has no Start value (Hadron never creates one)";
    if (reg.start != before.start || (before.start == 2 && (reg.delayed == 1) != (before.delayed == 1)))
        return L"a start-type change is already waiting for a restart (registry Start " + StartName(reg.start) + L"): restart first";
    return L"";
}

void MarkRegFallback(Plan* p, Backend& b, bool regOn, bool tiOn, const std::vector<SvcItem>& items, const Ctx& c) {
    (void)items;
    if (p->kind == PlanKind::Control) return;
    int marked = 0, refused = 0, undoOff = 0;
    for (auto& r : p->rows) {
        if (!r.blocked.empty()) continue;
        if (r.viaReg) {   // Undo or Resume of a registry write: the same path back, and only with the toggle on
            if (r.regNote.empty()) r.regNote = kRegUndoLine;
            if (!regOn) {
                r.include = false;
                r.canToggle = false;
                r.warning = kRegUndoOff;
                undoOff++;
            }
            continue;
        }
        if (!regOn || tiOn) continue;
        bool denied = false;
        std::wstring e;
        if (b.CanWrite(r.name, &denied, &e) || !denied) continue;   // the SCM takes it (or refuses for another reason)
        SvcItem live;
        RegStartVals reg;
        std::wstring why;
        if (!b.Query(r.name, &live, &e)) why = L"it could not be read again: " + e;
        else if (!b.ReadRegStart(r.name, &reg, &e)) why = L"its registry Start could not be read: " + e;
        else why = RegRowRefusal(live, reg, r.before, r.after, &c);
        if (!why.empty()) {
            r.regRefused = why;
            refused++;
            continue;
        }
        r.viaReg = true;
        r.regNote = kRegLine;
        marked++;
    }
    if (marked)
        p->notes.insert(p->notes.begin(),
                        std::to_wstring(marked) + L" row(s): the service manager refuses the change to administrators. With the registry "
                        L"fallback on, Hadron writes Start (and DelayedAutostart) in the service's registry key directly, only after you "
                        L"press Apply. The old values go into the undo log, flushed, before each write; the value is read again just "
                        L"before writing (a changed one is skipped) and read back after; Undo writes the old values back the same way. " +
                        std::wstring(kRegRestartBasis));
    if (refused)
        p->notes.push_back(std::to_wstring(refused) + L" row(s) the service manager refuses can't use the registry fallback either "
                           L"(launch-protected, protected, a Boot or System-start driver, or a change already waiting for a restart): "
                           L"left out, with the reason on the row.");
    if (undoOff)
        p->notes.insert(p->notes.begin(), std::to_wstring(undoOff) + L" row(s) were written to the registry directly by Hadron: they "
                                          L"stay unticked until you turn the registry fallback on, which puts them back the same way.");
}

bool IsRegRow(const UndoRow& r) { return r.identity == kRegIdentity || r.iIdentity == kRegIdentity; }

std::set<std::wstring> LastAppliedViaReg(const std::vector<UndoLog>& logs) {
    std::set<std::wstring> s;
    for (auto& l : logs) {
        if (!l.hasE) continue;
        for (auto& r : l.rows) {
            if (!r.hasD || r.result != L"ok") continue;
            if (IsRegRow(r)) s.insert(Lower(r.name));
            else s.erase(Lower(r.name));
        }
    }
    return s;
}

}  // namespace hx::services
