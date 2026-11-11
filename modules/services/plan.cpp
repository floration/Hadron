// modules/services/plan.cpp — guards, plan building and the apply engine (spec §5.3, §5.4, §6).
#include "modules/services/plan.h"
#include "modules/services/recovery.h"
#include "core/sys/sys.h"
#include <algorithm>
#include <functional>
#include <map>

namespace hx::services {

namespace {

// ---- the curated floor (r4 §2) and the named never-in-preset set (spec §6) ---------------------------
struct Group { const wchar_t* reason; std::vector<const wchar_t*> names; };

const std::vector<Group>& Floor() {
    static const std::vector<Group> g = {
        {L"device-tree root (ACPI, PCI, power dependency coordinator): no devices without it",
         {L"acpi", L"acpiex", L"pci", L"pdc", L"msisadrv", L"intelpep", L"acpipagr"}},
        {L"storage path to the disks: disabling the one in use stops Windows booting (0x7B)",
         {L"storport", L"stornvme", L"storahci", L"iastorac", L"iastoravc", L"iastorvd", L"iastorv", L"iavroc",
          L"atapi", L"pciide", L"pciidex", L"scsiport", L"classpnp"}},
        {L"disk and volume stack (several are Volume/Disk class filters)",
         {L"disk", L"partmgr", L"volmgr", L"volmgrx", L"volsnap", L"mountmgr", L"fvevol", L"rdyboost", L"iorate",
          L"ehstorclass", L"spaceport", L"volume"}},
        {L"file system stack (Filter Manager hosts every minifilter; Wof serves compressed system files)",
         {L"ntfs", L"refs", L"fltmgr", L"fs_rec", L"wof", L"clfs", L"cimfs"}},
        {L"crypto and kernel driver framework (most modern drivers use KMDF)",
         {L"cng", L"ksecdd", L"ksecpkg", L"wdf01000", L"wdfldr", L"msrpc", L"tm", L"null", L"pcw"}},
        {L"named pipes, mailslots and the UNC provider (nearly all local IPC)", {L"npfs", L"msfs", L"mup"}},
        {L"networking, WFP and the firewall",
         {L"ndis", L"tcpip", L"tcpip6", L"afd", L"netio", L"nsiproxy", L"tdx", L"wfplwfs", L"mpsdrv", L"bfe", L"mpssvc"}},
        {L"USB and HID input: no keyboard or mouse at sign-in without these",
         {L"usbxhci", L"usbhub3", L"usbhub", L"ucx01000", L"usbport", L"usbehci", L"usbccgp", L"hidusb", L"hidparse",
          L"kbdhid", L"mouhid", L"kbdclass", L"mouclass", L"i8042prt"}},
        {L"display stack", {L"dxgkrnl", L"basicdisplay", L"basicrender"}},
        {L"processor power management (P-states, C-states, power plans)", {L"intelppm", L"amdppm", L"processr", L"power"}},
        {L"TPM (BitLocker, Windows Hello, some anti-cheats)", {L"tpm", L"tbs"}},
        {L"SMBIOS and Intel MEI (Hadron's own BIOS pages read SMBIOS)", {L"mssmbios", L"meix64", L"heci", L"hecix64"}},
        {L"core Windows service: boot, sign-in, RPC, audio, network or Hadron itself needs it",
         {L"rpcss", L"rpceptmapper", L"dcomlaunch", L"lsm", L"samss", L"plugplay", L"brokerinfrastructure",
          L"staterepository", L"coremessagingregistrar", L"profsvc", L"winmgmt", L"eventlog", L"cryptsvc",
          L"audioendpointbuilder", L"audiosrv", L"dhcp", L"dnscache", L"nsi"}},
    };
    return g;
}

const std::vector<const wchar_t*>& HypervisorDrivers() {
    static const std::vector<const wchar_t*> v = {L"hvservice", L"vmbus", L"vid", L"hvsocket", L"hvcrash", L"vmgencounter", L"winhvr", L"winhv"};
    return v;
}

const std::vector<Group>& NeverPreset() {
    static const std::vector<Group> g = {
        {L"Defender and Windows Security: Microsoft says not to disable, stop or modify them",
         {L"windefend", L"wdnissvc", L"wdnisdrv", L"wdfilter", L"wdboot", L"sense", L"securityhealthservice", L"wscsvc"}},
        {L"TrustedInstaller: Windows servicing, and what Hadron's TrustedInstaller toggle needs", {L"trustedinstaller"}},
        {L"Volume Shadow Copy: System Restore needs it, and restore points are Microsoft's registry recovery path (Microsoft's RegBack page)", {L"vss", L"swprv"}},
        {L"file-system filter that Store apps, containers, Defender or cloud files sit on",
         {L"fileinfo", L"filecrypt", L"bindflt", L"wcifs", L"cldflt"}},
    };
    return g;
}

const wchar_t* Lookup(const std::vector<Group>& gs, const std::wstring& lower) {
    for (auto& g : gs)
        for (auto* n : g.names)
            if (lower == n) return g.reason;
    return nullptr;
}

std::wstring Arrow(const StartCfg& a, const StartCfg& b) {
    return StartLabel(a, false) + L" -> " + StartLabel(b, false);
}

int StateRank(const PlanRow& r) { return !r.blocked.empty() ? 2 : (r.include ? 0 : 1); }

void SortRows(Plan* p) {
    std::stable_sort(p->rows.begin(), p->rows.end(), [](const PlanRow& a, const PlanRow& b) {
        int ra = StateRank(a), rb = StateRank(b);
        if (ra != rb) return ra < rb;
        return Lower(a.name) < Lower(b.name);
    });
}

std::wstring TabName(int tab) { return tab == TAB_DRIVERS ? L"Drivers" : L"Services"; }

// A7: restores compare and write the raw DelayedAutostart value, as BuildSingle does. Delayed() is false for any
// start but Auto, so a lingering 1 under Manual or Disabled would otherwise turn a restore to plain Auto into
// Auto (delayed). The flag only matters when the target is Auto; then 1 is "delayed", anything else is not.
bool SameStart(const StartCfg& have, const StartCfg& want) {
    if (have.start != want.start) return false;
    return want.start != 2 || (have.delayed == 1) == (want.delayed == 1);
}
WriteOp OpTo(const StartCfg& have, const StartCfg& want) {
    WriteOp op{want.start, -1};
    if (want.start == 2 && (have.delayed == 1) != (want.delayed == 1)) op.delayed = want.delayed == 1 ? 1 : 0;
    return op;
}
StartCfg AfterOf(const StartCfg& have, const WriteOp& op) { return StartCfg{op.start, op.delayed >= 0 ? op.delayed : have.delayed}; }

bool SysOk(const SvcItem& s, const Ctx& c) {
    const KbEntry* e = c.kb ? c.kb->Find(TemplateName(s.name, s.type)) : nullptr;
    return e && e->sysok && e->ForBuild(c.build);
}

// A4: what an undo or resume row may write. Undo logs are integrity-checked (CRC), not authenticated, so every
// row passes the same guards a plan would: never-touch items, Boot start, a Boot/System value on a service,
// and a System-start driver without a curated sysok entry are refused whatever the log says.
std::wstring TargetGuard(const SvcItem& live, const StartCfg& target, const Ctx& c) {
    if (live.kind == Kind::UserInstance) return L"Per-user instance: change its template instead";
    Guard g = CheckGuard(live, c);
    if (g.neverTouch) return L"Protected: never touched by Hadron. " + g.reason;
    bool drv = IsDriverType(live.type);
    if (target.start == 0) return L"Refused: Hadron never sets Boot start";
    if (!drv && target.start < 2) return L"Refused: Boot and System start are for drivers only";
    if (drv && (target.start < 2 || live.cfg.start < 2) && !SysOk(live, c))
        return L"Protected: System-start driver without a curated sysok entry";
    return L"";
}

std::wstring LogRefusal(const UndoLog& log, const Ctx& c) {
    if (c.machine.empty()) return L"Refused: this PC's machine GUID could not be read, so the log can't be matched to it.";
    if (log.machine != c.machine)
        return L"Refused: this undo log was not written on this PC (its machine GUID " +
               std::wstring(log.machine.empty() ? L"is missing" : L"differs") + L"). Nothing from it is offered.";
    if (log.build.empty() || log.build != c.build)
        return L"Refused: this undo log was written on Windows build " + (log.build.empty() ? std::wstring(L"(missing)") : log.build) +
               L"; this PC runs build " + (c.build.empty() ? std::wstring(L"(unknown)") : c.build) +
               L". Service names and defaults change between builds, so nothing from it is offered.";
    return L"";
}

std::wstring BadRecordsNote(const UndoLog& log) {
    std::wstring t;
    if (log.badRecords)
        t = std::to_wstring(log.badRecords) + L" record(s) in this log failed their check (torn or changed) and were ignored.";
    if (log.count > 0 && (int)log.rows.size() != log.count)
        t += (t.empty() ? L"" : L" ") + std::wstring(L"The log planned ") + std::to_wstring(log.count) + L" row(s) but only " +
             std::to_wstring(log.rows.size()) + L" could be read: an item it lost may still be changed; compare with the snapshot.";
    return t;
}

}  // namespace

bool IsStorageDriver(const std::wstring& name) {
    std::wstring l = Lower(name);
    const auto& f = Floor();
    for (int gi : {1, 2})
        for (auto* n : f[gi].names) if (l == n) return true;
    return false;
}

void Ctx::Prepare() {
    filters.clear();
    bootChain.clear();
    if (hw && hw->ok) { filters = FilterServices(*hw); bootChain = BootChainServices(*hw); }
}

Guard CheckGuard(const SvcItem& s, const Ctx& c) {
    Guard g;
    std::wstring l = Lower(TemplateName(s.name, s.type));
    bool drv = IsDriverType(s.type);
    auto never = [&](const std::wstring& r) { g.neverTouch = true; g.reason = r; return g; };
    if (drv && s.cfg.start == 0) return never(L"Boot-start driver: loaded by the boot loader before Windows can read the disk");
    if (!drv && s.cfg.start < 2 && s.kind != Kind::UserInstance)
        return never(L"a service whose registry Start is " + StartName(s.cfg.start) + L": not a start type Windows uses for services, so Hadron leaves it");
    // Fail closed: without the device list or the boot disk the filter and boot-path checks can't run.
    if (drv && (!c.hw || !c.hw->ok))
        return never(L"the device list could not be read, so the filter and boot-path checks can't run: every driver is kept");
    if (drv && c.hw->bootDisk.empty())
        return never(L"the boot disk could not be found, so the boot-path check can't run: every driver is kept");
    if (c.filters.count(l)) return never(L"named in a class or device UpperFilters/LowerFilters value: disabling it stops every device that lists it (0x7B for disks, no input for keyboards)");
    if (c.bootChain.count(l)) return never(L"serves a device on the boot disk's path");
    if (s.errctl == 3) return never(L"ErrorControl is Critical: Windows can't start without it");
    if (const wchar_t* r = Lookup(Floor(), l)) return never(r);
    if (c.hw && c.hw->hypervisor)
        for (auto* n : HypervisorDrivers())
            if (l == n) return never(L"hypervisor driver while the hypervisor is running (use Windows Features or Memory Integrity instead)");
    if (s.protect != 0) {
        g.neverPreset = true;
        g.reason = L"launch-protected (" + ProtectName(s.protect) + L"): only a protected process can change it";
        return g;
    }
    if (const wchar_t* r = Lookup(NeverPreset(), l)) { g.neverPreset = true; g.reason = r; return g; }
    const KbEntry* e = c.kb ? c.kb->Find(l) : nullptr;
    if (!e) { g.neverPreset = true; g.reason = L"third-party or unknown: no Hadron KB entry"; return g; }
    if (!e->ForBuild(c.build)) {
        g.neverPreset = true;
        g.reason = L"Hadron's KB entry is for build " + Join(e->builds, L", ") + L", not " + (c.build.empty() ? L"this one" : c.build);
        return g;
    }
    if (e->never) { g.neverPreset = true; g.reason = L"Hadron's KB keeps it out of every preset"; return g; }
    return g;
}

namespace {
std::wstring HwNote(int tab, const Ctx& c) {
    if (tab != TAB_DRIVERS) return L"";
    if (!c.hw || !c.hw->ok)
        return L"The device list could not be read" + (c.hw && !c.hw->error.empty() ? L" (" + c.hw->error + L")" : std::wstring()) +
               L", so every driver is kept: the filter and boot-path checks can't run without it.";
    if (c.hw->bootDisk.empty())
        return L"The boot disk could not be found" + (c.hw->error.empty() ? std::wstring() : L" (" + c.hw->error + L")") +
               L", so every driver is kept: the boot-path check can't run without it.";
    return L"";
}
}  // namespace

int Plan::Included() const { int n = 0; for (auto& r : rows) if (r.blocked.empty() && r.include) n++; return n; }
int Plan::Optional() const { int n = 0; for (auto& r : rows) if (r.blocked.empty() && !r.include) n++; return n; }
int Plan::Blocked() const { int n = 0; for (auto& r : rows) if (!r.blocked.empty()) n++; return n; }

// ---- builders ------------------------------------------------------------------------------------------
Plan BuildPreset(int level, int tab, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Preset;
    p.level = level;
    p.tab = tab;
    p.title = LevelName(level) + L" (" + TabName(tab) + L")";
    p.source = L"preset:" + std::wstring(level == LV_BLOAT ? L"bloat" : level == LV_STRIPPED ? L"stripped" : L"minimum") +
               L":" + (tab == TAB_DRIVERS ? L"drivers" : L"services");
    int stricter = 0;
    for (auto& s : items) {
        if (!OnTab(s, (Tab)tab) || s.kind == Kind::UserInstance) continue;
        const KbEntry* e = c.kb ? c.kb->Find(s.name) : nullptr;
        if (!e) { p.unknown++; continue; }
        int t = e->TargetAt(level);
        if (t < 0) continue;
        if (s.cfg.start == t) { p.alreadySet++; continue; }
        if (s.cfg.start > t) { stricter++; continue; }   // presets only lower: never Disabled -> Manual
        PlanRow r;
        r.name = s.name;
        r.display = s.display;
        r.driver = IsDriverType(s.type);
        r.before = s.cfg;
        r.after = StartCfg{t, s.cfg.delayed};
        r.op = WriteOp{t, -1};
        r.deps = s.deps;
        r.reason = e->WhyAt(level);
        r.basis = e->pbasis;
        r.basisNote = e->pbasisNote;
        r.contested = e->contested;
        Guard g = CheckGuard(s, c);
        if (g.neverTouch || g.neverPreset) r.blocked = L"Protected: not in presets. " + g.reason;
        else if (r.driver && s.cfg.start == 1 && !e->sysok)
            r.blocked = L"Protected: System-start driver without a curated sysok entry";
        else if (r.driver && !e->sysok)
            r.blocked = L"Protected: driver without a curated sysok entry";
        if (r.blocked.empty() && !e->hw.empty()) {
            std::wstring ev;
            int h = c.hw ? HwEvidence(*c.hw, e->hw, s.name, &ev) : 0;
            if (!c.hw) ev = L"no hardware information: kept";
            r.evidence = ev;
            if (h == 0) r.blocked = L"Kept: hardware in use or seen before. " + ev;
            else if (h == 2) r.include = false;
        } else if (r.blocked.empty() && e->JoinLevel() == LV_STRIPPED) {
            // Spec 5.3: Stripped applies only where hardware or use is absent with evidence shown. Use without a
            // hardware hint cannot be detected, so the row is offered unticked, like a hot-plug class.
            r.evidence = L"use cannot be detected: tick if you don't use it";
            r.include = false;
        }
        if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
        p.rows.push_back(std::move(r));
    }
    DependencyCheck(&p, items);
    SortRows(&p);
    if (tab == TAB_DRIVERS && level == LV_BLOAT)
        p.notes.push_back(L"Pure bloat removal on the Drivers tab is expected to be near-empty: Hadron found no Microsoft source that names a "
                          L"stock driver as safe to disable.");
    if (p.unknown)
        p.notes.push_back(std::to_wstring(p.unknown) + L" item(s) on this tab have no KB entry (third-party or unknown): never in a preset.");
    if (stricter)
        p.notes.push_back(std::to_wstring(stricter) + L" item(s) are already stricter than this level (for example Disabled where it says Manual): "
                                                       L"left alone, a preset never raises a start type.");
    if (std::wstring hn = HwNote(tab, c); !hn.empty()) p.notes.push_back(hn);
    if (c.kb && !c.kb->build.empty() && c.kb->build != c.build)
        p.notes.push_back(L"Hadron's KB is for build " + c.kb->build + L"; this PC is build " + c.build + L": entries for other builds are not used.");
    p.notes.push_back(L"Start types only. Changes take effect at the next restart; Hadron never restarts the PC.");
    return p;
}

Plan BuildDefault(bool stock, int tab, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Preset;
    p.level = LEVEL_DEFAULT;
    p.tab = tab;
    p.stock = stock;
    p.title = std::wstring(stock ? L"Default: Windows stock" : L"Default: restore first snapshot") + L" (" + TabName(tab) + L")";
    p.source = std::wstring(stock ? L"default-stock:" : L"default-first:") + (tab == TAB_DRIVERS ? L"drivers" : L"services");
    if (!stock) {
        if (!c.first) { p.notes.push_back(L"No first-run snapshot yet: it is written the first time live data loads."); return p; }
        auto m = c.first->meta.find(L"machine");
        if (m != c.first->meta.end() && !c.machine.empty() && m->second != c.machine) {
            p.notes.push_back(L"The first-run snapshot is from another PC (machine GUID differs): nothing to restore.");
            return p;
        }
        int added = 0;
        for (auto& s : items) {
            if (!OnTab(s, (Tab)tab) || s.kind == Kind::UserInstance) continue;
            const SvcItem* f = c.first->Find(s.name);
            if (!f) { added++; continue; }
            if (SameStart(s.cfg, f->cfg)) { p.alreadySet++; continue; }
            PlanRow r;
            r.name = s.name; r.display = s.display; r.driver = IsDriverType(s.type);
            r.before = s.cfg; r.deps = s.deps;
            r.op = OpTo(s.cfg, f->cfg);
            r.after = AfterOf(s.cfg, r.op);
            auto cr = c.first->meta.find(L"created");
            r.reason = L"as recorded by the first-run snapshot" + (cr != c.first->meta.end() ? L" (" + cr->second + L")" : L"");
            r.basis = L"recorded";
            r.basisNote = L"first.txt, read from this PC's service manager when Hadron first ran";
            Guard g = CheckGuard(s, c);
            const wchar_t* named = Lookup(NeverPreset(), Lower(s.name));
            if (g.neverTouch) r.blocked = L"Protected: never touched by Hadron. " + g.reason;
            else if (named) r.blocked = L"Protected: not in presets. " + std::wstring(named);
            else if (s.protect) r.blocked = L"Protected: " + g.reason;
            else if (f->cfg.start < 2) r.blocked = L"Kept: Hadron never sets Boot or System start; Windows changes these itself";
            else if (r.driver && s.cfg.start < 2 && !SysOk(s, c))
                r.blocked = L"Protected: System-start driver without a curated sysok entry (live start type)";
            if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
            p.rows.push_back(std::move(r));
        }
        if (added) p.notes.push_back(std::to_wstring(added) + L" item(s) are new since the first-run snapshot: left alone.");
    } else {
        if (c.build != L"19045") {
            p.notes.push_back(L"Windows stock values exist only for Windows 10 22H2 (build 19045); this PC is build " +
                              (c.build.empty() ? std::wstring(L"unknown") : c.build) + L". Use Restore first snapshot instead.");
            return p;
        }
        int withStock = 0, total = 0;
        for (auto& s : items) {
            if (!OnTab(s, (Tab)tab) || s.kind == Kind::UserInstance) continue;
            total++;
            const KbEntry* e = c.kb ? c.kb->Find(s.name) : nullptr;
            if (!e || e->stock < 0 || !e->ForBuild(c.build) || IsStorageDriver(s.name)) continue;
            withStock++;
            StartCfg t{e->stock, e->stock == 2 ? e->stockDelayed : s.cfg.delayed};
            if (SameStart(s.cfg, t)) { p.alreadySet++; continue; }
            PlanRow r;
            r.name = s.name; r.display = s.display; r.driver = IsDriverType(s.type);
            r.before = s.cfg; r.deps = s.deps;
            r.op = OpTo(s.cfg, t);
            r.after = AfterOf(s.cfg, r.op);
            r.reason = L"Windows stock start type for build 19045";
            r.basis = e->stockBasis;
            r.basisNote = e->stockNote;
            Guard g = CheckGuard(s, c);
            if (g.neverTouch || g.neverPreset) r.blocked = L"Protected: not in presets. " + g.reason;
            else if (t.start < 2) r.blocked = L"Kept: Hadron never sets Boot or System start";
            else if (r.driver && s.cfg.start < 2 && !SysOk(s, c))
                r.blocked = L"Protected: System-start driver without a curated sysok entry (live start type)";
            if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
            p.rows.push_back(std::move(r));
        }
        p.notes.push_back(std::to_wstring(withStock) + L" of " + std::to_wstring(total) +
                          L" items have a Windows stock value with a measured or published basis in Hadron's KB; the rest are left alone. "
                          L"Storage drivers never get a stock value: Windows sets those per PC.");
    }
    if (std::wstring hn = HwNote(tab, c); !hn.empty()) p.notes.push_back(hn);
    DependencyCheck(&p, items);
    SortRows(&p);
    p.notes.push_back(L"Start types only. Changes take effect at the next restart; Hadron never restarts the PC.");
    return p;
}

Plan BuildSingle(const SvcItem& s, StartCfg target, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Single;
    p.tab = IsDriverType(s.type) ? TAB_DRIVERS : TAB_SERVICES;
    p.source = L"single";
    p.title = L"Change start type: " + s.name;
    PlanRow r;
    r.name = s.name; r.display = s.display; r.driver = IsDriverType(s.type);
    r.before = s.cfg; r.deps = s.deps;
    r.after = StartCfg{target.start, s.cfg.delayed};
    r.op = WriteOp{target.start, -1};
    if (target.start == 2) {
        bool want = target.delayed == 1, have = s.cfg.Delayed() || s.cfg.delayed == 1;
        if (want != have) { r.op.delayed = want ? 1 : 0; r.after.delayed = r.op.delayed; }
    }
    r.reason = L"your choice in the detail pane";
    r.basis = L"user";
    Guard g = CheckGuard(s, c);
    if (s.kind == Kind::UserInstance) r.blocked = L"Per-user instance: change its template instead";
    else if (g.neverTouch) r.blocked = L"Protected: " + g.reason;
    else if (target.start < 2) r.blocked = L"Hadron never sets Boot or System start";
    else if (c.tiOn && IEq(s.name, L"TrustedInstaller")) r.blocked = L"Not while the TrustedInstaller toggle is on: the toggle needs this service";
    else if (target.start == 2 && target.delayed == 1 && !s.group.empty())
        r.blocked = L"Windows doesn't allow a delayed auto-start service in a load-order group (" + s.group + L"): use Auto";
    if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
    if (r.blocked.empty() && r.before.start == r.after.start && r.before.Delayed() == r.after.Delayed()) {
        p.alreadySet = 1;
        p.notes.push_back(L"Nothing to change: it is already " + StartLabel(s.cfg, false) + L".");
        return p;
    }
    const bool drv = r.driver;
    p.rows.push_back(std::move(r));
    DependencyCheck(&p, items);
    if (!drv) p.notes.push_back(L"Manual doesn't stop trigger starts; Disabled does. Hadron never removes triggers.");
    p.notes.push_back(L"Takes effect at the next restart (or the next start of the service); Hadron never restarts the PC.");
    return p;
}

Plan BuildControl(const SvcItem& s, int what, Backend& b, const std::vector<SvcItem>& items) {
    (void)items;
    Plan p;
    p.kind = PlanKind::Control;
    p.tab = TAB_SERVICES;
    p.restartNeeded = false;
    static const wchar_t* const kWhat[] = {L"Start", L"Stop", L"Restart"};
    p.title = std::wstring(kWhat[what]) + L" " + s.name;
    p.source = L"control";
    auto row = [&](const std::wstring& name, Op op, const std::wstring& why) {
        PlanRow r;
        r.name = name;
        r.ctl = op;
        r.reason = why;
        r.canToggle = false;
        p.rows.push_back(r);
    };
    if (IsDriverType(s.type)) {
        PlanRow r; r.name = s.name; r.blocked = L"Drivers are never loaded or unloaded live"; r.include = false; r.canToggle = false;
        p.rows.push_back(r);
        return p;
    }
    std::vector<std::wstring> deps;
    std::wstring err;
    if (what != 0) {
        if (!b.Dependents(s.name, &deps, &err)) p.notes.push_back(L"Could not list running dependents: " + err);
        for (auto& d : deps) row(d, Op::Stop, L"running dependent: stops first");
        row(s.name, Op::Stop, what == 1 ? L"stop" : L"stop, then start again");
    }
    if (what != 1) {
        row(s.name, Op::Start, what == 0 ? L"start" : L"start again");
        if (what == 2) for (auto it = deps.rbegin(); it != deps.rend(); ++it) row(*it, Op::Start, L"was running before: started again");
    }
    if (what != 0 && !deps.empty())
        p.notes.push_back(std::to_wstring(deps.size()) + L" running service(s) depend on it and stop first.");
    p.notes.push_back(L"Live control: logged, no undo entry. The start type is not changed.");
    return p;
}

Plan BuildUndo(const UndoLog& log, Backend& b, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Undo;
    p.source = L"undo:" + log.stamp;
    p.title = L"Undo the apply of " + log.stamp;
    if (std::wstring why = LogRefusal(log, c); !why.empty()) { p.notes.push_back(why); return p; }
    if (std::wstring bad = BadRecordsNote(log); !bad.empty()) p.notes.push_back(bad);
    std::vector<UndoRow> rows = log.rows;
    std::sort(rows.begin(), rows.end(), [](const UndoRow& a, const UndoRow& b2) { return a.seq > b2.seq; });
    for (auto& u : rows) {
        if (!u.hasI) continue;   // never reached the write: nothing changed
        PlanRow r;
        r.name = u.name; r.seq = u.seq; r.after = u.before;
        r.reason = u.hasD ? L"undo: Hadron set it to " + StartLabel(u.after, false) : L"undo: interrupted during the write";
        r.basis = L"undo log";
        r.basisNote = L"undo-" + log.stamp + L".log";
        SvcItem live;
        std::wstring err;
        if (!b.Query(u.name, &live, &err)) {
            r.blocked = L"Gone: " + err; r.include = false; r.canToggle = false;
            p.rows.push_back(r);
            continue;
        }
        r.display = live.display; r.driver = IsDriverType(live.type); r.deps = live.deps;
        r.before = live.cfg;
        if (SameStart(live.cfg, u.before)) { p.alreadySet++; continue; }
        r.op = OpTo(live.cfg, u.before);
        r.after = AfterOf(live.cfg, r.op);
        if (r.op.delayed == 0 && u.before.delayed < 0)
            r.evidence = L"DelayedAutostart was absent before; it is written as 0, which has the same effect";
        bool hadronWrote = u.hasD && u.result == L"ok";
        if (hadronWrote && !SameStart(live.cfg, u.afterObs)) {
            r.include = false;
            r.warning = L"changed since Hadron set it: now " + StartLabel(live.cfg, false) + L" (unticked; tick to restore anyway)";
        }
        if (std::wstring g = TargetGuard(live, u.before, c); !g.empty()) { r.blocked = g; r.include = false; r.canToggle = false; }
        p.rows.push_back(r);
    }
    // Undo raises rows back; a row blocked or unticked for drift can leave a raised dependent without its
    // dependency, and a lowering row can sit under something enabled since.
    DependencyCheck(&p, items);
    SortRows(&p);
    p.notes.push_back(L"Undo restores only the start values Hadron changed, from the values recorded before the change.");
    return p;
}

Plan BuildResume(const UndoLog& log, Backend& b, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Resume;
    p.source = L"resume:" + log.stamp;
    p.title = L"Resume the apply of " + log.stamp;
    if (std::wstring why = LogRefusal(log, c); !why.empty()) { p.notes.push_back(why); return p; }
    if (std::wstring bad = BadRecordsNote(log); !bad.empty()) p.notes.push_back(bad);
    for (auto& u : log.rows) {
        if (u.hasD) continue;
        PlanRow r;
        r.name = u.name; r.seq = u.seq;
        r.reason = L"resume: planned " + Arrow(u.before, u.after);
        r.basis = L"undo log";
        r.basisNote = L"undo-" + log.stamp + L".log";
        SvcItem live;
        std::wstring err;
        if (!b.Query(u.name, &live, &err)) {
            r.blocked = L"Gone: " + err; r.include = false; r.canToggle = false;
            p.rows.push_back(r);
            continue;
        }
        r.display = live.display; r.driver = IsDriverType(live.type); r.deps = live.deps;
        r.before = live.cfg;
        if (SameStart(live.cfg, u.after)) { p.alreadySet++; continue; }
        r.op = OpTo(live.cfg, u.after);
        r.after = AfterOf(live.cfg, r.op);
        if (!SameStart(live.cfg, u.before)) {
            r.include = false;
            r.warning = L"changed since the plan: now " + StartLabel(live.cfg, false) + L" (unticked)";
        }
        if (std::wstring g = TargetGuard(live, u.after, c); !g.empty()) { r.blocked = g; r.include = false; r.canToggle = false; }
        p.rows.push_back(r);
    }
    DependencyCheck(&p, items);
    SortRows(&p);
    p.notes.push_back(L"Resume re-plans the rows the interrupted apply never finished, against the live state.");
    return p;
}

void MarkTi(Plan* p, Backend& b, bool tiOn, const std::vector<SvcItem>& items) {
    int needTi = 0;
    for (auto& r : p->rows) {
        if (!r.blocked.empty() || p->kind == PlanKind::Control) continue;
        if (tiOn && IEq(r.name, L"TrustedInstaller")) {
            r.blocked = L"Not while the TrustedInstaller toggle is on: the toggle needs this service";
            r.include = false; r.canToggle = false;
            continue;
        }
        bool denied = false;
        std::wstring err;
        if (b.CanWrite(r.name, &denied, &err) || !denied) continue;
        r.needsTi = true;
        needTi++;
        if (!tiOn) {
            r.include = false;
            r.canToggle = false;
            r.warning = L"needs TrustedInstaller: Windows refuses this write to administrators, and the toggle is off";
        } else {
            r.warning = L"needs TrustedInstaller: written while impersonating it (toggle on)";
        }
    }
    if (needTi) {
        std::wstring c = std::to_wstring(needTi) + L" row(s) need TrustedInstaller (Windows refuses the write to administrators): ";
        p->notes.insert(p->notes.begin(), c + (tiOn ? L"written while impersonating it; Hadron starts it if needed and stops it after."
                                                    : L"left out while the TrustedInstaller toggle is off."));
    }
    // A row left out here (or the TrustedInstaller row blocked) keeps its item at its current start, which can
    // leave a ticked row disabling something that now stays enabled: check again (Analyze A8).
    DependencyCheck(p, items);
    SortRows(p);
}

namespace {
// Who depends on what, built once per DependencyCheck so a round costs rows x (dependents + deps), not
// rows x items x deps.
struct DepIndex {
    const std::vector<SvcItem>& items;
    std::map<std::wstring, int> byName;                          // lower(name) -> item index
    std::map<std::wstring, std::vector<int>> dependents;         // lower(name) -> items naming it in DependOnService
    std::map<std::wstring, std::vector<int>> groupDependents;    // lower(group) -> items depending on +group
    std::map<std::wstring, std::vector<int>> members;            // lower(group) -> items in the group
    explicit DepIndex(const std::vector<SvcItem>& it) : items(it) {
        for (int i = 0; i < (int)items.size(); i++) {
            const SvcItem& s = items[i];
            byName.emplace(Lower(s.name), i);
            if (!s.group.empty()) members[Lower(s.group)].push_back(i);
            if (s.kind == Kind::UserInstance) continue;   // never counted as a dependent
            for (auto& d : s.deps) {
                if (d.size() > 1 && d[0] == L'+') groupDependents[Lower(d.substr(1))].push_back(i);
                else dependents[Lower(d)].push_back(i);
            }
        }
    }
    const SvcItem* Find(const std::wstring& name) const {
        auto it = byName.find(Lower(name));
        return it == byName.end() ? nullptr : &items[it->second];
    }
};

// One row's dependency warning against the start every item ends with if the ticked rows are applied.
std::wstring DepWarning(const PlanRow& r, const DepIndex& ix, const std::map<std::wstring, int>& fin) {
    auto end = [&](const std::wstring& name) { auto it = fin.find(Lower(name)); return it == fin.end() ? -1 : it->second; };
    std::wstring out;
    const SvcItem* self = ix.Find(r.name);
    if (r.after.start == 4) {   // disabling: does anything that stays enabled depend on it?
        std::vector<int> dep;
        if (auto it = ix.dependents.find(Lower(r.name)); it != ix.dependents.end()) dep = it->second;
        std::wstring group = self ? Lower(self->group) : L"";
        if (!group.empty())
            if (auto it = ix.groupDependents.find(group); it != ix.groupDependents.end()) dep.insert(dep.end(), it->second.begin(), it->second.end());
        std::sort(dep.begin(), dep.end());   // item order, as the list shows them
        dep.erase(std::unique(dep.begin(), dep.end()), dep.end());
        std::vector<std::wstring> who;
        for (int i : dep) {
            const SvcItem& s = ix.items[i];
            if (IEq(s.name, r.name)) continue;
            int e = end(s.name);
            if (e != 4) who.push_back(s.name + L" (" + StartName(e) + L")");
        }
        if (!who.empty())
            out = Join(who, L", ") + (who.size() == 1 ? L" depends on it and stays enabled" : L" depend on it and stay enabled") +
                  L": unticked by default";
    } else if (r.after.start < r.before.start) {   // raising: does it need something that stays Disabled?
        std::vector<std::wstring> need;
        const std::vector<std::wstring>& deps = self ? self->deps : r.deps;
        for (auto& d : deps) {
            if (d.size() > 1 && d[0] == L'+') {
                int members = 0, off = 0;
                if (auto it = ix.members.find(Lower(d.substr(1))); it != ix.members.end())
                    for (int i : it->second) { members++; if (end(ix.items[i].name) == 4) off++; }
                if (members && members == off) need.push_back(L"group " + d.substr(1));
            } else if (end(d) == 4) {
                need.push_back(d);
            }
        }
        if (!need.empty())
            out = L"needs " + Join(need, L", ") + L", which " + (need.size() == 1 ? L"stays" : L"stay") +
                  L" Disabled: Windows can't start it: unticked by default";
    }
    return out;
}
}  // namespace

const wchar_t* const kDepNotSettled = L"The dependency check did not settle";

void DependencyCheck(Plan* p, const std::vector<SvcItem>& items, int toggled) {
    if (p->kind == PlanKind::Control) return;
    std::vector<PlanRow>& rows = p->rows;
    const std::wstring tag = kDepNotSettled;
    p->notes.erase(std::remove_if(p->notes.begin(), p->notes.end(),
                                  [&](const std::wstring& n) { return n.compare(0, tag.size(), tag) == 0; }),
                   p->notes.end());
    const bool hasToggled = toggled >= 0 && toggled < (int)rows.size();
    if (hasToggled) { rows[toggled].depUnticked = false; rows[toggled].depKept = false; }   // the user's choice now
    const DepIndex ix(items);
    std::map<std::wstring, int> base;
    for (auto& s : items) base[Lower(s.name)] = s.cfg.start;
    auto kept = [&](size_t k) { return (int)k == toggled || rows[k].depKept; };
    // One round against the end state of the ticked rows. untickOnly never ticks a row again, so the ticked set
    // only shrinks and repeated rounds always settle.
    auto round = [&](bool untickOnly) {
        std::map<std::wstring, int> fin = base;
        for (auto& r : rows) if (r.blocked.empty() && r.include) fin[Lower(r.name)] = r.after.start;
        bool changed = false;
        for (size_t k = 0; k < rows.size(); k++) {
            PlanRow& r = rows[k];
            std::wstring w = r.blocked.empty() ? DepWarning(r, ix, fin) : L"";
            if (!untickOnly && w.empty() && r.depUnticked && r.blocked.empty() && r.canToggle) {
                r.include = true;
                r.depUnticked = false;
                changed = true;
            }
            if (!w.empty() && r.include && r.blocked.empty() && !kept(k)) {
                r.include = false;
                r.depUnticked = true;
                changed = true;
            }
            r.depWarning = w;
        }
        return changed;
    };
    // Iterate to a fixed point: unticking one row can leave a dependency of it with an enabled dependent (aaa <- bbb
    // <- ccc), and ticking one can clear another's warning. Rows the check unticked come back when the reason goes.
    // A state seen before means it would go round forever (Analyze A9): stop there.
    std::vector<std::vector<char>> seen;
    bool settled = false;
    for (size_t n = 0; n <= 2 * rows.size() + 2; n++) {
        std::vector<char> st;
        st.reserve(rows.size() * 2);
        for (auto& r : rows) { st.push_back(r.include); st.push_back(r.depUnticked); }
        if (std::find(seen.begin(), seen.end(), st) != seen.end()) break;
        seen.push_back(std::move(st));
        if (!round(false)) { settled = true; break; }
    }
    if (!settled) {
        int before = 0, after = 0;
        for (auto& r : rows) before += r.include && r.blocked.empty();
        while (round(true)) {}
        for (auto& r : rows) after += r.include && r.blocked.empty();
        // A row unticked in the circle may have no warning against the final state (its partner is unticked too).
        for (auto& r : rows)
            if (r.depUnticked && !r.include && r.blocked.empty() && r.depWarning.empty())
                r.depWarning = L"left unticked: its dependencies conflict with another row's in a circle (see the note at the top)";
        p->notes.insert(p->notes.begin(),
                        tag + L": some rows' dependency warnings depend on each other (raising one needs another that a "
                        L"second row disables), so every row with a dependency problem was left unticked (" +
                        std::to_wstring(before - after) + L" more than usual). Read their warnings before ticking any.");
    }
    if (hasToggled) rows[toggled].depKept = rows[toggled].include && !rows[toggled].depWarning.empty();
    for (auto& r : rows) if (r.depWarning.empty()) r.depKept = false;
}

std::vector<size_t> ApplyOrder(const Plan& p) {
    std::vector<size_t> lower, raise;
    for (size_t i = 0; i < p.rows.size(); i++) {
        const PlanRow& r = p.rows[i];
        if (!r.blocked.empty() || !r.include) continue;
        (r.after.start > r.before.start ? lower : raise).push_back(i);
    }
    // Depth-first topological order over the plan's own rows: for lowering, a dependent before what it
    // depends on; for raising, the reverse.
    auto order = [&](const std::vector<size_t>& set, bool dependentsFirst) {
        std::map<std::wstring, size_t> byName;
        for (size_t i : set) byName[Lower(p.rows[i].name)] = i;
        std::vector<size_t> out;
        std::set<size_t> done, active;
        std::function<void(size_t)> visit = [&](size_t i) {   // emits dependencies first
            if (done.count(i) || active.count(i)) return;
            active.insert(i);
            for (auto& d : p.rows[i].deps) {
                auto it = byName.find(Lower(d));
                if (it != byName.end()) visit(it->second);
            }
            active.erase(i);
            done.insert(i);
            out.push_back(i);
        };
        std::vector<size_t> sorted = set;
        std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) { return Lower(p.rows[a].name) < Lower(p.rows[b].name); });
        for (size_t i : sorted) visit(i);
        if (dependentsFirst) std::reverse(out.begin(), out.end());
        return out;
    };
    std::vector<size_t> all = order(lower, true);
    for (size_t i : order(raise, false)) all.push_back(i);
    return all;
}

std::string DumpPlan(const Plan& p) {
    std::wstring o;
    o += L"Plan: " + p.title + L"\n";
    o += L"Source: " + p.source + L"\n";
    o += L"Rows: " + std::to_wstring(p.Included()) + L" ticked, " + std::to_wstring(p.Optional()) + L" unticked, " +
         std::to_wstring(p.Blocked()) + L" kept or protected; already set: " + std::to_wstring(p.alreadySet) + L"\n";
    for (auto& n : p.notes) o += L"Note: " + n + L"\n";
    for (auto& r : p.rows) {
        std::wstring box = !r.blocked.empty() ? L"[-]" : (r.include ? L"[x]" : L"[ ]");
        if (p.kind == PlanKind::Control) {
            o += box + L" " + (r.ctl == Op::Stop ? L"stop  " : L"start ") + r.name + L"  (" + r.reason + L")\n";
            if (!r.blocked.empty()) o += L"    " + r.blocked + L"\n";
            continue;
        }
        o += box + L" " + r.name + L"  " + Arrow(r.before, r.after) + L"  (" + CfgText(r.before) + L" -> " + CfgText(r.after) + L")\n";
        if (!r.blocked.empty()) o += L"    " + r.blocked + L"\n";
        if (!r.reason.empty()) o += L"    why: " + r.reason + L"\n";
        if (!r.basis.empty()) o += L"    basis: " + r.basis + (r.basisNote.empty() ? L"" : L" -- " + r.basisNote) + L"\n";
        if (!r.contested.empty()) o += L"    contested: " + r.contested + L"\n";
        if (!r.evidence.empty()) o += L"    evidence: " + r.evidence + L"\n";
        if (!r.warning.empty()) o += L"    warning: " + r.warning + L"\n";
        if (!r.depWarning.empty()) o += L"    warning: " + r.depWarning + L"\n";
        if (r.needsTi) o += L"    needs TrustedInstaller\n";
    }
    return ToUtf8(o);
}

// ---- apply ---------------------------------------------------------------------------------------------
namespace {
std::wstring Num(int v) { return std::to_wstring(v); }

struct OnExit {   // runs f on every return path unless it was cleared
    std::function<void()> f;
    ~OnExit() { if (f) f(); }
};

std::vector<RestoreRow> RowsFor(const Plan& p, const std::vector<size_t>& order) {
    std::vector<RestoreRow> v;
    for (size_t i : order) {
        const PlanRow& r = p.rows[i];
        v.push_back(RestoreRow{r.name, r.before, r.op.delayed >= 0, r.needsTi});
    }
    return v;
}
}  // namespace

bool RunApply(const Plan& p, const ApplyEnv& env, ApplyResult* r) {
    *r = ApplyResult{};
    Backend& b = *env.backend;
    Store& st = *env.store;
    std::vector<size_t> order = ApplyOrder(p);
    if (order.empty()) { r->nothing = true; r->error = L"Nothing to change."; return true; }
    std::wstring err;

    // 1. Lock: the named mutex now, the log (share mode 0) below.
    ApplyLock lock;
    if (!lock.Acquire(env.lockName, &err)) { r->aborted = true; r->error = err; return false; }
    if (!st.EnsureRoot(true, &err)) { r->aborted = true; r->error = err; return false; }
    r->stamp = st.NewStamp();
    int order1 = 1;   // the B record's sequence number: the log order that does not depend on the clock
    for (auto& l : LoadLogs(st, nullptr)) order1 = std::max(order1, l.order + 1);

    // 2. A full snapshot before anything changes.
    Snapshot snap;
    snap.meta[L"kind"] = L"pre-apply";
    snap.meta[L"created"] = r->stamp;
    snap.meta[L"build"] = env.build;
    snap.meta[L"machine"] = env.machine;
    snap.meta[L"source"] = p.source;
    if (!b.Enumerate(&snap.items, &err)) { r->aborted = true; r->error = L"Could not read the services for the snapshot: " + err; return false; }
    std::wstring snapRel = L"snap-" + r->stamp + L".txt";
    if (!st.WriteNew(snapRel, WriteSnapshot(snap), &err)) { r->aborted = true; r->error = err; return false; }
    r->snapPath = st.Path(snapRel);

    // 3. The recovery folder and zip for this apply's "before" values. Any failure: change nothing.
    if (!WriteRecovery(st, r->stamp, p.title, RowsFor(p, order), &r->recoveryDir, &r->zipPath, &err)) {
        r->aborted = true;
        r->error = L"Could not write the recovery files, so nothing was changed: " + err;
        return false;
    }

    // 4. The undo log: header, B, every P row, each flushed.
    LogWriter log;
    log.failAfter = env.failFlushAfter;
    std::wstring logRel = L"undo-" + r->stamp + L".log";
    r->logPath = st.Path(logRel);
    if (!log.Open(r->logPath, &err)) { r->aborted = true; r->error = err; return false; }
    auto put = [&](const std::string& rec) {
        if (log.Append(rec, &err)) return true;
        r->aborted = true;
        r->error = L"The undo log could not be written, so the apply stopped: " + err;
        return false;
    };
    if (!put("HXSVC\t1\tundo\n")) return false;
    if (!put(Record({L"B", r->stamp, Esc(p.source), Num((int)order.size()), env.machine, env.build, Num(order1)}))) return false;
    for (size_t k = 0; k < order.size(); k++) {
        const PlanRow& row = p.rows[order[k]];
        if (!put(Record({L"P", Num((int)k + 1), Esc(row.name), L"start", CfgText(row.before), CfgText(row.after),
                         row.needsTi ? L"ti" : L"admin"})))
            return false;
    }

    // TrustedInstaller, once, only if a ticked row needs it and the toggle is on.
    bool needTi = false;
    for (size_t i : order) if (p.rows[i].needsTi) needTi = true;
    bool tiReady = false;
    std::wstring tiErr;
    // From Acquire on, every return path puts TrustedInstaller back: a failed log write or flush below must not
    // leave a service Hadron started running (or the token open). The normal path releases it explicitly at E.
    OnExit tiBack;
    if (needTi && env.tiOn && env.ti) {
        TiProvider* ti = env.ti;
        std::wstring* note = &r->tiNote;
        tiBack.f = [ti, note]() { std::wstring n; ti->Release(&n); if (!n.empty()) *note = n; };
        tiReady = env.ti->Acquire(&tiErr);
        if (env.ti->StartedService() && !put(Record({L"T", L"started"}))) return false;
    }

    // 5. Each row, in dependency order.
    std::set<std::wstring> failedLowering;
    for (size_t k = 0; k < order.size(); k++) {
        if (env.cancel && env.cancel->load()) { r->cancelled = true; break; }
        if (env.progress) env.progress->store(k);
        const PlanRow& row = p.rows[order[k]];
        int seq = (int)k + 1;
        auto done = [&](const std::wstring& res, const StartCfg& obs, const std::wstring& text) {
            std::wstring line = res + L" " + row.name + L": " + (res == L"ok" ? Arrow(row.before, obs) : text);
            r->lines.push_back(line);
            if (res == L"ok") r->ok++; else if (res == L"skip") r->skipped++; else r->failed++;
            return put(Record({L"D", Num(seq), res, CfgText(obs), Esc(text)}));
        };
        SvcItem live;
        bool lowering = row.after.start > row.before.start;
        if (!b.Query(row.name, &live, &err)) { if (!done(L"skip", row.before, L"vanished since the plan: " + err)) return false; continue; }
        if (!SameStart(live.cfg, row.before) || (live.cfg.delayed == 1) != (row.before.delayed == 1)) {
            // A dependent that is not Disabled after all keeps protecting what it depends on (M1).
            if (lowering && live.cfg.start != 4) failedLowering.insert(Lower(row.name));
            if (!done(L"skip", live.cfg, L"changed since the plan: now " + StartLabel(live.cfg, false))) return false;
            continue;
        }
        if (lowering) {
            std::wstring blocker;
            for (auto& o : p.rows)
                if (failedLowering.count(Lower(o.name)))
                    for (auto& d : o.deps) if (IEq(d, row.name)) blocker = o.name;
            if (!blocker.empty()) {
                failedLowering.insert(Lower(row.name));   // stays enabled, so it protects its own dependencies too
                if (!done(L"skip", live.cfg, L"its dependent " + blocker + L" could not be changed")) return false;
                continue;
            }
        }
        // I is flushed BEFORE the write: a crash after this line leaves "unknown, re-read" for recovery.
        if (!put(Record({L"I", Num(seq), CfgText(live.cfg), row.needsTi ? L"ti" : L"admin"}))) return false;
        bool ok = false;
        std::wstring werr, ident = L"admin";
        if (row.needsTi) {
            if (!env.tiOn) werr = L"needs TrustedInstaller and the toggle is off";
            else if (!tiReady) werr = L"TrustedInstaller unavailable: " + tiErr;
            else {
                ImpersonationFlag::Active() = true;   // every Hadron file write refuses until Revert
                if (env.ti->Impersonate(&ident, &werr)) {
                    ok = b.SetStart(row.name, row.op, Identity::TrustedInstaller, &werr);
                    env.ti->Revert();
                }
                ImpersonationFlag::Active() = false;
            }
        } else {
            ok = b.SetStart(row.name, row.op, Identity::Admin, &werr);
        }
        if (env.crashAfterBegin == seq) { tiBack.f = nullptr; r->crashed = true; log.Close(); return false; }   // simulated power loss
        SvcItem after;
        std::wstring qerr;
        bool reread = b.Query(row.name, &after, &qerr);
        StartCfg obs = reread ? after.cfg : live.cfg;
        if (ok && reread && obs.start == row.op.start && (row.op.delayed < 0 || obs.Delayed() == (row.op.delayed == 1 && obs.start == 2) ||
                                                           (row.op.delayed >= 0 && obs.delayed == row.op.delayed))) {
            if (!done(L"ok", obs, ident)) return false;
        } else {
            if (lowering) failedLowering.insert(Lower(row.name));
            std::wstring why = !ok ? werr : (!reread ? L"re-read failed: " + qerr
                                                     : L"Windows did not keep this change (now " + StartLabel(obs, false) + L")");
            if (!done(L"fail", obs, why)) return false;
        }
    }
    if (env.progress) env.progress->store(order.size());

    // 6. E, then put TrustedInstaller back the way it was found.
    if (!put(Record({L"E", r->stamp, Num(r->ok), Num(r->skipped), Num(r->failed), r->cancelled ? L"cancelled" : L"done"})))
        return false;
    if (needTi && env.tiOn && env.ti) {
        tiBack.f = nullptr;
        bool started = env.ti->StartedService();   // read before Release resets it
        bool stopped = env.ti->Release(&r->tiNote);
        if (started && stopped) put(Record({L"T", L"stopped"}));
    }
    if (!tiErr.empty()) r->tiNote = L"TrustedInstaller: " + tiErr + (r->tiNote.empty() ? L"" : L"; " + r->tiNote);
    log.Close();
    return true;
}

bool RunControl(const Plan& p, const ApplyEnv& env, ApplyResult* r) {
    *r = ApplyResult{};
    ApplyLock lock;
    std::wstring err;
    if (!lock.Acquire(env.lockName, &err)) { r->aborted = true; r->error = err; return false; }
    size_t k = 0;
    for (auto& row : p.rows) {
        if (!row.blocked.empty()) continue;
        if (env.cancel && env.cancel->load()) { r->cancelled = true; break; }
        if (env.progress) env.progress->store(k++);
        std::wstring e;
        bool ok = env.backend->Control(row.name, row.ctl, env.cancel, &e);
        std::wstring what = row.ctl == Op::Stop ? L"stop " : L"start ";
        r->lines.push_back((ok ? L"ok " : L"fail ") + what + row.name + (ok ? L"" : L": " + e));
        if (ok) r->ok++;
        else {
            r->failed++;
            if (row.ctl == Op::Stop) break;   // never start things again after a failed stop
        }
    }
    return r->failed == 0;
}

// ---- logs -----------------------------------------------------------------------------------------------
std::vector<UndoLog> LoadLogs(const Store& s, std::vector<std::wstring>* problems) {
    std::vector<UndoLog> v;
    for (auto& f : s.List(L"undo-*.log")) {
        std::string text;
        std::wstring err;
        UndoLog l;
        if (!s.Read(f, &text, &err) || !ParseUndoLog(text, &l, &err)) {
            if (problems) problems->push_back(f + L": " + err);
            continue;
        }
        l.file = f;
        v.push_back(std::move(l));
    }
    // The B record's sequence number first (clock changes can't reorder it), then the UTC stamp with its -N suffix
    // compared as a number ("-10" after "-2"). Logs without a sequence number (older format) sort first.
    auto key = [](const UndoLog& l) {
        std::wstring s = l.stamp;
        size_t dash = s.find(L'-', 9);
        std::wstring base = dash == std::wstring::npos ? s : s.substr(0, dash);
        int n = dash == std::wstring::npos ? 0 : _wtoi(s.c_str() + dash + 1);
        wchar_t suf[16];
        swprintf(suf, 16, L"%08d", n);
        return std::make_pair(l.order, base + L"#" + suf);
    };
    std::stable_sort(v.begin(), v.end(), [&](const UndoLog& a, const UndoLog& b) { return key(a) < key(b); });
    return v;
}

namespace {
int IndexOfStamp(const std::vector<UndoLog>& logs, const std::wstring& stamp) {
    for (size_t k = 0; k < logs.size(); k++) if (logs[k].stamp == stamp) return (int)k;
    return -1;
}

// Log i is resolved when a finished undo of it exists (or, unless undoOnly, a finished resume), in any position:
// the order is not trusted for this. A finished resume of an undo of i also undoes i. Unless undoOnly, an
// unfinished undo or resume of a log that is itself resolved is resolved with it.
bool ResolvedBy(const std::vector<UndoLog>& logs, size_t i, bool undoOnly, int depth = 0) {
    if (depth > 8) return false;
    const std::wstring& st = logs[i].stamp;
    for (size_t j = 0; j < logs.size(); j++) {
        if (j == i || !logs[j].hasE) continue;
        const std::wstring& src = logs[j].source;
        if (src == L"undo:" + st) return true;
        if (!undoOnly && src == L"resume:" + st) return true;
        if (src.rfind(L"resume:", 0) == 0) {
            int k = IndexOfStamp(logs, src.substr(7));
            if (k >= 0 && logs[k].source == L"undo:" + st) return true;
        }
    }
    if (!undoOnly && !logs[i].hasE) {
        const std::wstring& src = logs[i].source;
        for (const wchar_t* pre : {L"undo:", L"resume:"}) {
            size_t n = wcslen(pre);
            if (src.compare(0, n, pre) != 0) continue;
            int k = IndexOfStamp(logs, src.substr(n));
            if (k >= 0 && k != (int)i && ResolvedBy(logs, (size_t)k, false, depth + 1)) return true;
        }
    }
    return false;
}
}  // namespace

int UndoBase(const std::vector<UndoLog>& logs, int k) {
    if (k < 0 || k >= (int)logs.size()) return k;
    const std::wstring& src = logs[k].source;
    if (src.rfind(L"undo:", 0) != 0) return k;
    int b = IndexOfStamp(logs, src.substr(5));
    return b >= 0 ? b : k;
}

int FindInterrupted(const std::vector<UndoLog>& logs) {
    for (int i = (int)logs.size() - 1; i >= 0; i--)
        if (logs[i].hasB && !logs[i].hasE && !ResolvedBy(logs, i, false)) return i;
    return -1;
}

int FindUndoable(const std::vector<UndoLog>& logs) {
    for (int i = (int)logs.size() - 1; i >= 0; i--) {
        // An undo's own log is not offered for undo (no "undo the undo"); it resolves the apply it reverted.
        if (!logs[i].hasE || ResolvedBy(logs, i, true) || logs[i].source.rfind(L"undo:", 0) == 0) continue;
        for (auto& r : logs[i].rows) if (r.hasD && r.result == L"ok") return i;
    }
    return -1;
}

bool TiLeftRunning(const std::vector<UndoLog>& logs) {
    for (auto& l : logs) if (l.tiStarted && !l.tiStopped) return true;   // any log: a later apply doesn't hide it
    return false;
}

std::map<std::wstring, StartCfg> LastApplied(const std::vector<UndoLog>& logs) {
    std::map<std::wstring, StartCfg> m;
    for (auto& l : logs) {
        if (!l.hasE) continue;
        for (auto& r : l.rows) if (r.hasD && r.result == L"ok") m[Lower(r.name)] = r.afterObs;
    }
    return m;
}

std::vector<RestoreRow> FirstRestoreRows(const Snapshot& first, const Ctx& c, std::vector<std::wstring>* skipped) {
    // The same exclusions as Default: restore first snapshot (A2). The offline scripts never write what Hadron
    // itself never changes: never-touch items (Boot-start, floor, filters, boot path, Critical), Boot/System start
    // values, storage drivers, named never-in-preset items, launch-protected services, and every driver when the
    // hardware checks couldn't run.
    std::vector<RestoreRow> v;
    for (auto& s0 : first.items) {
        SvcItem s = s0;
        s.kind = KindOf(s.type);
        if (s.kind == Kind::UserInstance) continue;
        Guard g = CheckGuard(s, c);
        bool drop = g.neverTouch || s.cfg.start < 2 || s.protect != 0 || IsStorageDriver(s.name) ||
                    Lookup(NeverPreset(), Lower(TemplateName(s.name, s.type))) != nullptr ||
                    (IsDriverType(s.type) && s.cfg.start < 2 && !SysOk(s, c));
        if (drop) { if (skipped) skipped->push_back(s.name); continue; }
        v.push_back(RestoreRow{s.name, s.cfg, s.cfg.delayed >= 0, s.aclTi});
    }
    return v;
}

FirstRunResult EnsureFirstSnapshot(Store& s, const std::vector<SvcItem>& items, const Ctx& c, Snapshot* first) {
    const std::wstring& build = c.build;
    const std::wstring& machine = c.machine;
    FirstRunResult r;
    std::wstring err;
    if (s.Exists(L"first.txt")) {
        std::string text;
        if (s.Read(L"first.txt", &text, &err) && ParseSnapshot(text, first, &err)) {
            r.existed = true;
            auto m = first->meta.find(L"machine");
            if (m != first->meta.end() && !machine.empty() && m->second != machine) {
                r.foreign = true;
                r.note = L"first.txt is from another PC (machine GUID differs): Restore first snapshot is off.";
            }
            return r;
        }
        // Corrupt: renamed aside, never overwritten or deleted. A new first.txt is written below.
        std::wstring aside;
        r.corrupt = true;
        if (!s.RenameAside(L"first.txt", &aside, &r.error)) return r;
        r.note = L"first.txt could not be read (" + err + L"); it was renamed to " + aside + L".";
        if (!s.List(L"undo-*.log").empty())
            r.note += L" Hadron changed services before, so the new snapshot is not the true first state.";
    }
    Snapshot snap;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t when[40];
    swprintf(when, 40, L"%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    snap.meta[L"kind"] = r.corrupt ? L"baseline (not first run)" : L"first";
    snap.meta[L"created"] = when;
    snap.meta[L"build"] = build;
    snap.meta[L"machine"] = machine;
    snap.items = items;
    for (auto& i : snap.items) i.company.clear();   // the file's claim, not configuration
    std::string text = WriteSnapshot(snap);
    if (!s.WriteAtomicNew(L"first.txt", text, &r.error)) return r;
    std::string back;
    Snapshot parsed;
    if (!s.Read(L"first.txt", &back, &r.error) || back != text || !ParseSnapshot(back, &parsed, &r.error)) {
        if (r.error.empty()) r.error = L"first.txt did not read back identically";
        return r;
    }
    *first = parsed;
    r.wrote = true;
    std::wstring dir, zip;
    if (!s.Exists(L"recovery\\first")) {
        std::vector<std::wstring> skipped;
        std::vector<RestoreRow> rows = FirstRestoreRows(parsed, c, &skipped);
        if (!WriteRecovery(s, L"first", L"start types as recorded when Hadron first ran", rows, &dir, &zip, &err, skipped))
            r.error = L"first.txt was written, but its recovery folder failed: " + err;
    }
    return r;
}

}  // namespace hx::services
