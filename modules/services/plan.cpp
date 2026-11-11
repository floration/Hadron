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

// What a plan says about a driver the KB doesn't clear for changes (no "sysok", Hadron's internal name, in user text).
const wchar_t kSysStartKept[] = L"Protected: a System-start driver that Hadron's KB does not list as safe to lower";
const wchar_t kDriverKept[] = L"Protected: a driver that Hadron's KB does not list as safe to change";

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
        return kSysStartKept;
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
    // R2: an item whose configuration could not be read has a placeholder start type: never planned from it.
    if (s.cfgUnknown) return never(L"its configuration could not be read, and Hadron never changes what it can't read");
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
    if (e->never) { g.neverPreset = g.kbNever = true; g.reason = L"Hadron's KB keeps it out of every preset"; return g; }
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

std::wstring UnreadableNote(int n) {
    return std::to_wstring(n) + L" item(s) on this tab could not be read (see the detail pane): left alone, Hadron never "
                                L"changes or records a start type it can't read.";
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
    int stricter = 0, unreadable = 0;
    for (auto& s : items) {
        if (!OnTab(s, (Tab)tab) || s.kind == Kind::UserInstance) continue;
        if (s.cfgUnknown) { unreadable++; continue; }   // R2: its start type is not known
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
        r.deps = s.deps; r.group = s.group;
        r.reason = e->WhyAt(level);
        r.basis = e->pbasis;
        r.basisNote = e->pbasisNote;
        r.contested = e->contested;
        Guard g = CheckGuard(s, c);
        if (g.neverTouch || g.neverPreset) r.blocked = L"Protected: not in presets. " + g.reason;
        else if (r.driver && s.cfg.start == 1 && !e->sysok)
            r.blocked = kSysStartKept;
        else if (r.driver && !e->sysok)
            r.blocked = kDriverKept;
        if (r.blocked.empty() && !e->hw.empty()) {
            std::wstring ev;
            int h = c.hw ? HwEvidence(*c.hw, e->hw, s.name, &ev) : 0;
            if (!c.hw) ev = L"no hardware information: kept";
            r.evidence = ev;
            if (h == 0) r.blocked = L"Kept: hardware in use or seen before. " + ev;
            else if (h == 2) r.include = false;
        } else if (r.blocked.empty() && e->JoinLevel() == LV_STRIPPED) {
            // Spec 5.3: Stripped applies only where hardware or use is absent with evidence shown. Use without a
            // hardware hint cannot be detected, so at Stripped the row is offered unticked, like a hot-plug class.
            // R18 (proxy Q1): Minimum is "only what is needed to boot, network, play games and run Hadron", so there
            // the same row is ticked and says so; plan, confirm and undo still gate it.
            if (level == LV_STRIPPED) {
                r.evidence = L"use cannot be detected: tick if you don't use it";
                r.include = false;
            } else {
                r.evidence = L"use cannot be detected: ticked at Minimum, untick if you use it";
            }
        }
        if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
        p.rows.push_back(std::move(r));
    }
    DependencyCheck(&p, items);
    SortRows(&p);
    if (tab == TAB_DRIVERS && level == LV_BLOAT)
        p.notes.push_back(L"Pure bloat removal has no drivers: Hadron found no Microsoft source that names a stock driver as safe to "
                          L"disable.");
    if (p.unknown)
        p.notes.push_back(std::to_wstring(p.unknown) + L" item(s) on this tab have no KB entry (third-party or unknown): never in a preset.");
    if (unreadable) p.notes.push_back(UnreadableNote(unreadable));
    if (stricter)
        p.notes.push_back(std::to_wstring(stricter) + L" item(s) are already stricter than this level (for example Disabled where it says Manual): "
                                                       L"left alone, a preset never raises a start type.");
    if (std::wstring hn = HwNote(tab, c); !hn.empty()) p.notes.push_back(hn);
    if (c.kb && !c.kb->build.empty() && c.kb->build != c.build)
        p.notes.push_back(L"Hadron's KB is for build " + c.kb->build + L"; this PC is build " + c.build + L": entries for other builds are not used.");
    p.notes.push_back(L"Start types only. Changes take effect at the next restart; Hadron never restarts the PC.");
    return p;
}

std::wstring BuildNumber(const std::wstring& build) {
    auto parts = Split(build, L'.');
    return parts.size() >= 3 ? parts[2] : build;
}

Plan BuildDefault(bool stock, int tab, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Preset;
    p.level = LEVEL_DEFAULT;
    p.tab = tab;
    p.stock = stock;
    p.title = std::wstring(stock ? L"Default: Windows stock" : L"Default: as first seen") + L" (" + TabName(tab) + L")";
    p.source = std::wstring(stock ? L"default-stock:" : L"default-first:") + (tab == TAB_DRIVERS ? L"drivers" : L"services");
    if (!stock) {
        if (!c.first) { p.notes.push_back(L"No first-run snapshot yet: it is written the first time live data loads."); return p; }
        auto meta = [&](const wchar_t* k) { auto it = c.first->meta.find(k); return it == c.first->meta.end() ? std::wstring() : it->second; };
        const std::wstring fm = meta(L"machine"), fb = meta(L"build"), created = meta(L"created"), kind = meta(L"kind");
        // R11: only a snapshot provably from this PC and this Windows build: an in-place upgrade sets new start types,
        // and writing the old build's back is what Undo and Resume already refuse.
        if (fm.empty() || c.machine.empty() || fm != c.machine) {
            p.notes.push_back(fm.empty() ? L"The first-run snapshot records no machine GUID, so Hadron can't tell it is from this PC: nothing to restore."
                              : c.machine.empty() ? L"This PC's machine GUID could not be read, so Hadron can't tell the first-run snapshot is from this PC: nothing to restore."
                                                  : L"The first-run snapshot is from another PC (machine GUID differs): nothing to restore.");
            return p;
        }
        if (BuildNumber(fb).empty() || BuildNumber(fb) != BuildNumber(c.build)) {
            p.notes.push_back(L"The first-run snapshot was taken on Windows build " + (fb.empty() ? std::wstring(L"(missing)") : fb) +
                              L"; this PC runs build " + (c.build.empty() ? std::wstring(L"(unknown)") : c.build) +
                              L". Windows sets new start types when it upgrades, so nothing from the snapshot is offered.");
            return p;
        }
        // Q3: say what this is, before anything else.
        const std::wstring date = created.empty() ? std::wstring(L"an unknown date") : created.substr(0, 10);
        if (kind == L"first")
            p.notes.push_back(L"Start types as Hadron first saw this PC on " + date + L". This is not Windows' default.");
        else
            p.notes.push_back(L"Start types as Hadron recorded them on " + date + L", after earlier changes by Hadron: not how Hadron "
                              L"first saw this PC. This is not Windows' default.");
        int added = 0, unreadable = 0;
        for (auto& s : items) {
            if (!OnTab(s, (Tab)tab) || s.kind == Kind::UserInstance) continue;
            const SvcItem* f = c.first->Find(s.name);
            // R2: an item unreadable now, or recorded as unreadable (an older first.txt), has no real value to compare.
            if (s.cfgUnknown || (f && f->cfgUnknown)) { unreadable++; continue; }
            if (!f) { added++; continue; }
            if (SameStart(s.cfg, f->cfg)) { p.alreadySet++; continue; }
            PlanRow r;
            r.name = s.name; r.display = s.display; r.driver = IsDriverType(s.type);
            r.before = s.cfg; r.deps = s.deps; r.group = s.group;
            r.op = OpTo(s.cfg, f->cfg);
            r.after = AfterOf(s.cfg, r.op);
            auto cr = c.first->meta.find(L"created");
            r.reason = L"as recorded by the first-run snapshot" + (cr != c.first->meta.end() ? L" (" + cr->second + L")" : L"");
            r.basis = L"recorded";
            // mini-design 11: no internal file name in the plan's text.
            r.basisNote = L"Hadron's first-run snapshot of this PC" + (cr != c.first->meta.end() ? L" (" + cr->second + L")" : std::wstring()) +
                          L", read from its service manager";
            Guard g = CheckGuard(s, c);
            const wchar_t* named = Lookup(NeverPreset(), Lower(s.name));
            if (g.neverTouch) r.blocked = L"Protected: never touched by Hadron. " + g.reason;
            else if (named) r.blocked = L"Protected: not in presets. " + std::wstring(named);
            else if (s.protect) r.blocked = L"Protected: " + g.reason;
            else if (f->cfg.start < 2) r.blocked = L"Kept: Hadron never sets Boot or System start; Windows changes these itself";
            else if (r.driver && s.cfg.start < 2 && !SysOk(s, c))
                r.blocked = kSysStartKept + std::wstring(L" (its start type now)");
            if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
            p.rows.push_back(std::move(r));
        }
        if (added) p.notes.push_back(std::to_wstring(added) + L" item(s) are new since the first-run snapshot: left alone.");
        if (unreadable) p.notes.push_back(UnreadableNote(unreadable));
    } else {
        // R20 (proxy Q2): never a bare "Nothing to change." that reads as "already at stock".
        const std::wstring none = L"Hadron has no Windows-stock data for build " + (c.build.empty() ? std::wstring(L"(unknown)") : c.build) +
                                  L" yet: nothing will be changed.";
        if (c.build != L"19045") {
            p.notes.push_back(none);
            p.emptyText = none;
            return p;
        }
        int withStock = 0, total = 0;
        for (auto& s : items) {
            if (!OnTab(s, (Tab)tab) || s.kind == Kind::UserInstance) continue;
            total++;
            if (s.cfgUnknown) continue;   // R2: CheckGuard would refuse it anyway
            const KbEntry* e = c.kb ? c.kb->Find(s.name) : nullptr;
            if (!e || e->stock < 0 || !e->ForBuild(c.build) || IsStorageDriver(s.name)) continue;
            withStock++;
            StartCfg t{e->stock, e->stock == 2 ? e->stockDelayed : s.cfg.delayed};
            if (SameStart(s.cfg, t)) { p.alreadySet++; continue; }
            PlanRow r;
            r.name = s.name; r.display = s.display; r.driver = IsDriverType(s.type);
            r.before = s.cfg; r.deps = s.deps; r.group = s.group;
            r.op = OpTo(s.cfg, t);
            r.after = AfterOf(s.cfg, r.op);
            // kb-audit svcdrv-kb M2: only a measured value is a fact about this build; the published ones name no build.
            r.reason = e->stockBasis == L"owner-measured"
                           ? L"Windows stock start type, measured on build " + c.build
                           : std::wstring(L"Windows stock start type from a Microsoft page (it names no build)");
            r.basis = e->stockBasis;
            r.basisNote = e->stockNote;
            Guard g = CheckGuard(s, c);
            // DECISIONS 2026-11-11 (proxy, Small fixes): a KB `never` means "not in a preset level", not "never restore
            // to a sourced Microsoft default"; never-touch, protected and the named never-preset list still block.
            if (g.neverTouch || (g.neverPreset && !g.kbNever)) r.blocked = L"Protected: not in presets. " + g.reason;
            else if (t.start < 2) r.blocked = L"Kept: Hadron never sets Boot or System start";
            else if (r.driver && s.cfg.start < 2 && !SysOk(s, c))
                r.blocked = kSysStartKept + std::wstring(L" (its start type now)");
            if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
            p.rows.push_back(std::move(r));
        }
        if (withStock == 0) {   // reviewer F1: this build has stock values, only not on this tab
            const std::wstring tabNone = L"Hadron's KB has no Windows stock value for any item on this tab: nothing will be changed.";
            p.notes.push_back(tabNone);
            p.emptyText = tabNone;
            return p;
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

namespace {
// "a, b, c and 4 more": a note names what it is about without growing past a few lines.
std::wstring NameList(const std::vector<std::wstring>& v, size_t shown = 8) {
    std::vector<std::wstring> h(v.begin(), v.begin() + std::min(shown, v.size()));
    std::wstring s = Join(h, L", ");
    if (v.size() > shown) s += L" and " + std::to_wstring(v.size() - shown) + L" more";
    return s;
}
}  // namespace

Plan BuildCustom(const CustomPreset& cp, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Preset;
    p.level = LEVEL_DEFAULT;
    p.tab = cp.tab;
    p.title = L"Custom: " + cp.name + L" (" + TabName(cp.tab) + L")";
    p.source = std::wstring(L"custom:") + (cp.tab == TAB_DRIVERS ? L"drivers" : L"services") + L":" + cp.name;
    std::vector<std::wstring> missing, otherTab, dropped;
    int unreadable = 0;
    for (auto& ci : cp.items) {
        const SvcItem* s = nullptr;
        for (auto& i : items) if (IEq(i.name, ci.name) && i.kind != Kind::UserInstance) { s = &i; break; }
        if (!s) { missing.push_back(ci.name); continue; }
        if (!OnTab(*s, (Tab)cp.tab)) { otherTab.push_back(s->name); continue; }
        if (s->cfgUnknown) { unreadable++; continue; }   // R2
        // The guards every built-in preset gets: never-touch and never-in-a-preset items, and drivers without sysok.
        Guard g = CheckGuard(*s, c);
        const bool drv = IsDriverType(s->type);
        if (g.neverTouch || g.neverPreset) { dropped.push_back(s->name + L" (" + g.reason + L")"); continue; }
        if (drv && !SysOk(*s, c)) {
            dropped.push_back(s->name + L" (" + std::wstring(s->cfg.start == 1 ? L"a System-start driver" : L"a driver") +
                              L" that Hadron's KB does not list as safe to change)");
            continue;
        }
        if (SameStart(s->cfg, ci.target)) { p.alreadySet++; continue; }
        PlanRow r;
        r.name = s->name; r.display = s->display; r.driver = drv;
        r.before = s->cfg; r.deps = s->deps; r.group = s->group;
        r.op = OpTo(s->cfg, ci.target);
        r.after = AfterOf(s->cfg, r.op);
        r.reason = L"your custom preset \"" + cp.name + L"\"";
        r.basis = L"user";
        r.basisNote = L"your own list" + (cp.saved.empty() ? std::wstring(L", imported from a file") : L", saved " + cp.saved);
        if (s->kind == Kind::UserInstance) r.blocked = L"Per-user instance: change its template instead";
        else if (c.tiOn && IEq(s->name, L"TrustedInstaller")) r.blocked = L"Not while the TrustedInstaller toggle is on: the toggle needs this service";
        else if (ci.target.Delayed() && !s->group.empty())
            r.blocked = L"Windows doesn't allow a delayed auto-start service in a load-order group (" + s->group + L"): use Auto";
        // The hardware guard built-in presets get (BuildPreset): a stricter start for an item whose KB entry names
        // hardware is kept while that hardware is in use or was seen, and offered unticked for a hot-plug class.
        if (r.blocked.empty() && ci.target.start > s->cfg.start) {
            const KbEntry* e = c.kb ? c.kb->Find(s->name) : nullptr;
            if (e && !e->hw.empty()) {
                std::wstring ev;
                int h = c.hw ? HwEvidence(*c.hw, e->hw, s->name, &ev) : 0;
                if (!c.hw) ev = L"no hardware information: kept";
                r.evidence = ev;
                if (h == 0) r.blocked = L"Kept: hardware in use or seen before. " + ev;
                else if (h == 2) r.include = false;
            }
        }
        if (!r.blocked.empty()) { r.include = false; r.canToggle = false; }
        p.rows.push_back(std::move(r));
    }
    DependencyCheck(&p, items);
    SortRows(&p);
    p.notes.push_back(L"Your custom preset \"" + cp.name + L"\": nothing is changed until you press Apply, and the apply is "
                      L"snapshotted and undoable like every plan.");
    if (!dropped.empty())
        p.notes.push_back(L"Dropped " + std::to_wstring(dropped.size()) + L" item(s) that every preset leaves alone: " + NameList(dropped) + L".");
    if (!missing.empty())
        p.notes.push_back(std::to_wstring(missing.size()) + L" item(s) in the preset are not on this PC: " + NameList(missing) + L".");
    if (!otherTab.empty())
        p.notes.push_back(std::to_wstring(otherTab.size()) + L" item(s) are not on the " + TabName(cp.tab) + L" tab, so this preset leaves them: " +
                          NameList(otherTab) + L".");
    if (unreadable) p.notes.push_back(UnreadableNote(unreadable));
    if (std::wstring hn = HwNote(cp.tab, c); !hn.empty()) p.notes.push_back(hn);
    if (!cp.build.empty() && BuildNumber(cp.build) != BuildNumber(c.build))
        p.notes.push_back(L"The preset was made on Windows build " + cp.build + L"; this PC runs build " +
                          (c.build.empty() ? std::wstring(L"(unknown)") : c.build) + L". Check each row: services differ between builds.");
    p.notes.push_back(L"Start types only. Changes take effect at the next restart; Hadron never restarts the PC.");
    if (p.rows.empty()) p.emptyText = L"Nothing to change: every item is already set, protected or not on this PC.";
    return p;
}

CustomPreset PresetFromPlan(const Plan& p, const std::wstring& name, const std::wstring& build) {
    CustomPreset cp;
    cp.name = name;
    cp.hadron = HadronVersion();
    cp.build = build;
    cp.tab = p.tab;
    for (auto& r : p.rows) {
        if (!r.blocked.empty() || !r.include || r.after.start < 2) continue;
        cp.items.push_back({r.name, StartCfg{r.after.start, r.after.start == 2 ? (r.after.delayed == 1 ? 1 : 0) : -1}});
    }
    return cp;
}

Plan BuildSingle(const SvcItem& s, StartCfg target, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Single;
    p.tab = IsDriverType(s.type) ? TAB_DRIVERS : TAB_SERVICES;
    p.source = L"single";
    p.title = L"Change start type: " + s.name;
    PlanRow r;
    r.name = s.name; r.display = s.display; r.driver = IsDriverType(s.type);
    r.before = s.cfg; r.deps = s.deps; r.group = s.group;
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
    // R6: lowering a System-start driver could never be put back by Hadron (Undo, a single change and Default all
    // refuse to set System start without the KB's clearance), so a single change needs the same clearance a preset does.
    else if (r.driver && s.cfg.start < 2 && !SysOk(s, c)) r.blocked = kSysStartKept;
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

Plan BuildControl(const SvcItem& s, int what, Backend& b, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Control;
    p.tab = TAB_SERVICES;
    p.what = what;
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
    // R5: never-touch items (the floor: RPC, event log, networking, BFE and the firewall, audio, WMI, crypto ...) are not
    // started, stopped or restarted either (spec §6: no single-item change), and neither is anything whose stop would
    // stop one of them as a dependent. Every row is blocked, so nothing runs.
    std::wstring refuse;
    Guard g = CheckGuard(s, c);
    if (g.neverTouch) refuse = L"Protected: Hadron never starts or stops this. " + g.reason;
    for (auto& d : deps) {
        if (!refuse.empty()) break;
        const SvcItem* di = nullptr;
        for (auto& i : items) if (IEq(i.name, d)) di = &i;
        if (di && CheckGuard(*di, c).neverTouch)
            refuse = L"Protected: stopping it would also stop " + d + L", which Hadron never stops. " + CheckGuard(*di, c).reason;
    }
    if (!refuse.empty())
        for (auto& r : p.rows) { r.blocked = refuse; r.include = false; }
    if (what != 0 && !deps.empty() && refuse.empty())
        p.notes.push_back(L"If a stop fails, the services this plan already stopped are started again.");
    p.notes.push_back(L"Live control: logged, no undo entry. The start type is not changed.");
    return p;
}

Plan BuildUndo(const UndoLog& log, Backend& b, const std::vector<SvcItem>& items, const Ctx& c) {
    Plan p;
    p.kind = PlanKind::Undo;
    p.source = L"undo:" + log.stamp;
    p.title = L"Undo the apply of " + StampText(log.stamp);
    // R7: the undo of a resume also puts back what the interrupted apply it resumed had written. Every log of the chain,
    // oldest first; per item the value from before the oldest write, and the newest write for the drift check.
    std::vector<const UndoLog*> all;
    for (auto& cl : log.chain) all.push_back(&cl);
    all.push_back(&log);
    for (auto* l : all) {
        if (std::wstring why = LogRefusal(*l, c); !why.empty()) { p.notes.push_back(why); return p; }
        if (std::wstring bad = BadRecordsNote(*l); !bad.empty()) p.notes.push_back(bad);
    }
    if (!log.chain.empty())
        p.notes.push_back(L"This apply resumed " + StampText(log.chain.front().stamp) +
                          L", so the undo also puts back what that interrupted apply had already changed.");
    struct Touched { UndoRow oldest, newest; std::wstring file; };
    std::vector<Touched> rows;
    std::map<std::wstring, size_t> at;
    for (auto* l : all)
        for (auto& u : l->rows) {
            if (!u.hasI) continue;   // never reached the write: nothing changed
            auto it = at.find(Lower(u.name));
            if (it == at.end()) { at[Lower(u.name)] = rows.size(); rows.push_back({u, u, L"undo-" + l->stamp + L".log"}); }
            else rows[it->second].newest = u;
        }
    for (auto& t : rows) {
        const UndoRow& u0 = t.oldest;   // what to restore: the value before Hadron's first write in the chain
        const UndoRow& u = t.newest;    // Hadron's last write, for the reason and the drift check
        PlanRow r;
        r.name = u.name; r.seq = u.seq; r.after = u0.before;
        r.reason = u.hasD ? L"undo: Hadron set it to " + StartLabel(u.after, false) : L"undo: interrupted during the write";
        r.basis = L"undo log";
        r.basisNote = t.file;
        SvcItem live;
        std::wstring err;
        if (!b.Query(u.name, &live, &err)) {
            r.blocked = L"Gone: " + err; r.include = false; r.canToggle = false;
            p.rows.push_back(r);
            continue;
        }
        r.display = live.display; r.driver = IsDriverType(live.type); r.deps = live.deps; r.group = live.group;
        r.before = live.cfg;
        if (SameStart(live.cfg, u0.before)) { p.alreadySet++; continue; }
        r.op = OpTo(live.cfg, u0.before);
        r.after = AfterOf(live.cfg, r.op);
        if (r.op.delayed == 0 && u0.before.delayed < 0)
            r.evidence = L"DelayedAutostart was absent before; it is written as 0, which has the same effect";
        bool hadronWrote = u.hasD && u.result == L"ok";
        if (hadronWrote && !SameStart(live.cfg, u.afterObs)) {
            r.include = false;
            r.warning = L"changed since Hadron set it: now " + StartLabel(live.cfg, false) + L" (unticked; tick to restore anyway)";
        }
        if (std::wstring g = TargetGuard(live, u0.before, c); !g.empty()) { r.blocked = g; r.include = false; r.canToggle = false; }
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
    p.title = L"Resume the apply of " + StampText(log.stamp);
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
        r.display = live.display; r.driver = IsDriverType(live.type); r.deps = live.deps; r.group = live.group;
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
            r.warning = L"needs TrustedInstaller: Hadron writes it as TrustedInstaller (toggle on)";
        }
    }
    if (needTi) {
        std::wstring c = std::to_wstring(needTi) + L" row(s) need TrustedInstaller (Windows refuses the write to administrators): ";
        p->notes.insert(p->notes.begin(), c + (tiOn ? L"Hadron writes them as TrustedInstaller, starting it if needed and stopping it after."
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
        std::map<std::wstring, std::vector<size_t>> byGroup;   // R17: "+group" depends on every member of the group
        for (size_t i : set) {
            byName[Lower(p.rows[i].name)] = i;
            if (!p.rows[i].group.empty()) byGroup[Lower(p.rows[i].group)].push_back(i);
        }
        std::vector<size_t> out;
        std::set<size_t> done, active;
        std::function<void(size_t)> visit = [&](size_t i) {   // emits dependencies first
            if (done.count(i) || active.count(i)) return;
            active.insert(i);
            for (auto& d : p.rows[i].deps) {
                if (d.size() > 1 && d[0] == L'+') {
                    auto g = byGroup.find(Lower(d.substr(1)));
                    if (g != byGroup.end()) for (size_t m : g->second) visit(m);
                    continue;
                }
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
std::wstring Hex16(uint64_t v) { wchar_t b[24]; swprintf(b, 24, L"%016llX", (unsigned long long)v); return b; }
bool WriteResolution(Store& st, const std::wstring& source, const std::wstring& text, const std::wstring& machine,
                     const std::wstring& build, std::wstring* err);   // below, with the logs

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
    if (env.requireFirst && p.kind != PlanKind::Undo) {
        if (!st.Exists(L"first.txt")) {
            r->aborted = true;
            r->error = L"Hadron has no first-run snapshot (first.txt) in its data folder, so nothing was changed. It is written when "
                       L"Hadron reads the live services: press Refresh, and read the notice if it fails again.";
            return false;
        }
        // mini-analyze A6: it must be this PC's snapshot. The load only counts a machine-matched first.txt; this checks
        // the file itself again, under the lock.
        std::string ft;
        Snapshot fs;
        std::wstring fe;
        if (!st.Read(L"first.txt", &ft, &fe) || !ParseSnapshot(ft, &fs, &fe)) {
            r->aborted = true;
            r->error = L"Hadron's first-run snapshot (first.txt) could not be read, so nothing was changed: " + fe +
                       L". Press Refresh: a corrupt first.txt is renamed aside and recorded again.";
            return false;
        }
        auto fm = fs.meta.find(L"machine");
        if ((fm == fs.meta.end() ? std::wstring() : fm->second) != env.machine) {
            r->aborted = true;
            r->error = L"Hadron's first-run snapshot (first.txt) is not from this PC (its machine GUID differs), so nothing was "
                       L"changed: Hadron would have no record of how this PC started. Rename first.txt aside as an administrator "
                       L"and press Refresh, so Hadron records this PC first.";
            return false;
        }
    }
    r->stamp = st.NewStamp();
    const int order1 = NextOrder(LoadLogs(st, nullptr));   // the B record's sequence number: the order not taken from the clock

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
    if (!WriteRecovery(st, r->stamp, L"start types as they were before " + p.title + L", applied " + r->stamp + L" UTC",
                       RowsFor(p, order), &r->recoveryDir, &r->zipPath, &err)) {
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
        // R22: flushed before Acquire may start TrustedInstaller, so a crash in between still leaves a trace.
        if (!put(Record({L"T", L"starting"}))) return false;
        TiProvider* ti = env.ti;
        std::wstring* note = &r->tiNote;
        LogWriter* lw = &log;
        tiBack.f = [ti, note, lw]() {
            bool mine = ti->StartedService();
            std::wstring n, e;
            bool stopped = ti->Release(&n);
            if (!n.empty()) *note = n;
            // Best effort: this path follows a failed log write or flush, so the record may not get in (R4).
            if (mine) lw->Append(stopped ? Record({L"T", L"stopped"}) : Record({L"T", L"stopfailed", Esc(n)}), &e);
        };
        tiReady = env.ti->Acquire(env.cancel, &tiErr);
        if (env.ti->StartedService()) {
            // R4: the process Hadron started, by pid and creation time: the banner claims it only while those match.
            SvcItem t;
            std::wstring qe;
            DWORD pid = 0;
            uint64_t ps = 0;
            if (b.Query(L"TrustedInstaller", &t, &qe)) { pid = t.pid; ps = t.pstart; }
            if (!put(Record({L"T", L"started", std::to_wstring(pid), Hex16(ps)}))) return false;
        } else if (!put(Record({L"T", L"notstarted"}))) {
            return false;
        }
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
                    for (auto& d : o.deps)
                        // R17: a +group dependency protects every member of that group, like a named one.
                        if (IEq(d, row.name) || (d.size() > 1 && d[0] == L'+' && !live.group.empty() && IEq(d.substr(1), live.group)))
                            blocker = o.name;
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
        // R4: a failed stop is recorded too (with Hadron's note), not left as a bare "started". The apply itself is
        // finished (E is in), so a failure to write this record doesn't turn it into an aborted apply.
        std::wstring te;
        if (started) log.Append(stopped ? Record({L"T", L"stopped"}) : Record({L"T", L"stopfailed", Esc(r->tiNote)}), &te);
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
    // The services this run stopped, in order, while it is still stopping (R13): a failed stop starts them again, so a
    // Restart or a Stop that can't stop its target doesn't leave the dependents it already stopped down.
    std::vector<std::wstring> stopped;
    bool stopPhase = true;
    const bool tiStop = p.source.rfind(L"ti-stop:", 0) == 0;
    std::wstring tiResolved;   // R4: why the claim is closed, recorded below
    for (auto& row : p.rows) {
        if (!row.blocked.empty()) continue;
        if (env.cancel && env.cancel->load()) {
            r->cancelled = true;
            if (stopPhase && !stopped.empty())
                r->lines.push_back(L"cancelled: left stopped: " + Join(stopped, L", ") + L" (start them again from the list)");
            break;
        }
        if (env.progress) env.progress->store(k++);
        if (row.ctl == Op::Start) stopPhase = false;
        if (tiStop && row.ctl == Op::Stop && IEq(row.name, L"TrustedInstaller")) {
            // R4: only the very process Hadron started (pid and creation time), checked again now.
            SvcItem now;
            std::wstring qe;
            bool same = env.backend->Query(row.name, &now, &qe) && now.state == SERVICE_RUNNING && now.pid == p.tiPid &&
                        now.pstart != 0 && now.pstart == p.tiPstart;
            if (!same) {
                r->skipped++;
                r->lines.push_back(L"skip stop TrustedInstaller: it is no longer the process Hadron started, so it is left to Windows");
                tiResolved = L"the process Hadron started is gone (checked before stopping it)";
                continue;
            }
        }
        std::wstring e;
        bool ok = env.backend->Control(row.name, row.ctl, env.cancel, &e);
        std::wstring what = row.ctl == Op::Stop ? L"stop " : L"start ";
        r->lines.push_back((ok ? L"ok " : L"fail ") + what + row.name + (ok ? L"" : L": " + e));
        if (ok) {
            r->ok++;
            if (row.ctl == Op::Stop) stopped.push_back(row.name);
            if (tiStop && IEq(row.name, L"TrustedInstaller")) tiResolved = L"stopped by Hadron (the banner's Stop it)";
            continue;
        }
        r->failed++;
        if (row.ctl != Op::Stop) continue;
        // Never go on to start the target after a failed stop; put back what this run stopped, newest first.
        for (auto it = stopped.rbegin(); it != stopped.rend(); ++it) {
            std::wstring e2;
            // Cleanup (mini-analyze A5): sent and waited for even after Cancel, for a short bounded time.
            bool back = env.backend->Control(*it, Op::Start, nullptr, &e2, nullptr, kCleanupWaitMs);
            bool sent = !back && RequestSentNotConfirmed(e2);
            r->lines.push_back(back ? L"ok start " + *it + L" (started again after the failed stop)"
                               : sent ? L"fail start " + *it + L": requested, not confirmed (" + e2 + L")"
                                      : L"fail start " + *it + L": " + e2 + L" (left stopped)");
            if (!back) r->failed++;
        }
        break;
    }
    if (tiStop && !tiResolved.empty() && env.store) {
        std::wstring e;
        if (!WriteResolution(*env.store, L"ti-resolved:" + p.source.substr(8), tiResolved, env.machine, env.build, &e))
            r->lines.push_back(L"note: the TrustedInstaller record could not be written: " + e);
    }
    return r->failed == 0;
}

// ---- logs -----------------------------------------------------------------------------------------------
namespace {
int IndexOfStamp(const std::vector<UndoLog>& logs, const std::wstring& stamp) {
    for (size_t k = 0; k < logs.size(); k++) if (logs[k].stamp == stamp) return (int)k;
    return -1;
}

// The log that logs[k] names after prefix ("undo:", "resume:", ...), or -1.
int Target(const std::vector<UndoLog>& logs, int k, const wchar_t* prefix) {
    const std::wstring& src = logs[k].source;
    size_t n = wcslen(prefix);
    if (src.compare(0, n, prefix) != 0) return -1;
    int t = IndexOfStamp(logs, src.substr(n));
    return t == k ? -1 : t;
}

// Following "resume:" sources from k (k itself included) reaches i.
bool ResumeChainReaches(const std::vector<UndoLog>& logs, int k, int i) {
    for (int depth = 0; k >= 0 && depth <= 8; depth++) {
        if (k == i) return true;
        k = Target(logs, k, L"resume:");
    }
    return false;
}

// logs[k] is an undo: its source is "undo:", or it resumes (through any "resume:" chain) a log that is (R8).
bool IsUndoLog(const std::vector<UndoLog>& logs, int k) {
    for (int depth = 0; k >= 0 && depth <= 8; depth++) {
        if (logs[k].source.rfind(L"undo:", 0) == 0) return true;
        k = Target(logs, k, L"resume:");
    }
    return false;
}

bool IsResolutionLog(const UndoLog& l) {
    return l.source.rfind(L"dismiss:", 0) == 0 || l.source.rfind(L"ti-resolved:", 0) == 0;
}

// A finished "dismiss:<stamp of logs[i]>" log exists.
bool Dismissed(const std::vector<UndoLog>& logs, size_t i) {
    for (auto& o : logs)
        if (o.hasE && o.source == L"dismiss:" + logs[i].stamp) return true;
    return false;
}
}  // namespace

int OrderKey(int order) { return std::min(std::max(order, 0), kOrderCap + 1); }

int NextOrder(const std::vector<UndoLog>& logs) {
    int n = 1;
    for (auto& l : logs) n = std::max(n, std::min(l.order, kOrderCap) + 1);
    return n;
}

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
        // Clamped like NextOrder (mini-analyze fixture run 4): a log with a forged order above the cap ties with the
        // logs written after it and falls back to the stamp, instead of staying "the last apply" for good.
        return std::make_pair(OrderKey(l.order), base + L"#" + suf);
    };
    std::stable_sort(v.begin(), v.end(), [&](const UndoLog& a, const UndoLog& b) { return key(a) < key(b); });
    // R7: a resume carries the logs it resumed, oldest first, so its Undo also puts back what they had written.
    for (int k = 0; k < (int)v.size(); k++) {
        std::vector<UndoLog> chain;
        for (int t = Target(v, k, L"resume:"), depth = 0; t >= 0 && depth <= 8; t = Target(v, t, L"resume:"), depth++) {
            UndoLog c = v[t];
            c.chain.clear();
            chain.insert(chain.begin(), std::move(c));
        }
        v[k].chain = std::move(chain);
    }
    return v;
}

namespace {
// Log i is resolved when a finished undo of it exists (or, unless undoOnly, a finished resume or a dismissal), in any
// position: the order is not trusted for this. A finished resume of an undo of i also undoes i, and so does a finished
// undo of a resume of i (R7: that undo puts back i's rows too). Unless undoOnly, an unfinished undo or resume of a log
// that is itself resolved is resolved with it.
bool ResolvedBy(const std::vector<UndoLog>& logs, size_t i, bool undoOnly, int depth = 0) {
    if (depth > 8) return false;
    const std::wstring& st = logs[i].stamp;
    for (size_t j = 0; j < logs.size(); j++) {
        if (j == i || !logs[j].hasE) continue;
        const std::wstring& src = logs[j].source;
        if (src == L"undo:" + st) return true;
        // A finished resume or dismissal of i, or of a resume of i at any depth (mini-analyze A2: dismissing an
        // interrupted resume:i must not bring i's banner back; a finished resume of a resume of i finishes i's work).
        if (!undoOnly)
            for (const wchar_t* pre : {L"resume:", L"dismiss:"}) {
                int t = Target(logs, (int)j, pre);
                if (t >= 0 && ResumeChainReaches(logs, t, (int)i)) return true;
            }
        int k = Target(logs, (int)j, L"resume:");
        if (k >= 0 && logs[k].source == L"undo:" + st) return true;
        int u = Target(logs, (int)j, L"undo:");
        if (u >= 0 && u != (int)i && ResumeChainReaches(logs, u, (int)i)) return true;
    }
    if (!undoOnly && !logs[i].hasE) {
        for (const wchar_t* pre : {L"undo:", L"resume:"}) {
            int k = Target(logs, (int)i, pre);
            if (k >= 0 && ResolvedBy(logs, (size_t)k, false, depth + 1)) return true;
        }
    }
    return false;
}
}  // namespace

int UndoBase(const std::vector<UndoLog>& logs, int k) {
    if (k < 0 || k >= (int)logs.size()) return k;
    // An undo (or a resume of one, R8) is restarted from the apply it was undoing: undoing it would re-apply that.
    int u = k;
    for (int depth = 0; u >= 0 && depth <= 8 && logs[u].source.rfind(L"undo:", 0) != 0; depth++) u = Target(logs, u, L"resume:");
    if (u < 0 || logs[u].source.rfind(L"undo:", 0) != 0) return k;
    int b = Target(logs, u, L"undo:");
    return b >= 0 ? b : k;
}

int FindInterrupted(const std::vector<UndoLog>& logs) {
    for (int i = (int)logs.size() - 1; i >= 0; i--)
        if (logs[i].hasB && !logs[i].hasE && !IsResolutionLog(logs[i]) && !ResolvedBy(logs, i, false)) return i;
    return -1;
}

int FindUndoable(const std::vector<UndoLog>& logs) {
    for (int i = (int)logs.size() - 1; i >= 0; i--) {
        // An undo's own log is not offered for undo (no "undo the undo"), nor a resume of one (R8); it resolves the
        // apply it reverted. An interrupted log the user dismissed stays undoable (mini-analyze A2): Dismiss hid its
        // banner, which was the only other way to put back the rows it wrote.
        if (!(logs[i].hasE || Dismissed(logs, (size_t)i)) || ResolvedBy(logs, i, true) || IsUndoLog(logs, i)) continue;
        for (auto& r : logs[i].rows) if (r.hasD && r.result == L"ok") return i;
        for (auto& c : logs[i].chain)   // R7: a resume whose own rows were all set already still undoes its apply's
            for (auto& r : c.rows) if (r.hasD && r.result == L"ok") return i;
    }
    return -1;
}

// ---- TrustedInstaller claims (R4) -------------------------------------------------------------------------
namespace {
// logs[i] says Hadron may have started TrustedInstaller and has no record of stopping it, and no resolution log
// closes that.
bool OpenTiClaim(const std::vector<UndoLog>& logs, size_t i) {
    const UndoLog& l = logs[i];
    bool claimed = l.tiStarted || (l.tiStarting && !l.tiNotStarted);
    if (!claimed || l.tiStopped) return false;
    for (auto& o : logs)
        if (o.hasE && o.source == L"ti-resolved:" + l.stamp) return false;
    return true;
}

TiClaim ClaimOf(const std::vector<UndoLog>& logs, size_t i) {
    const UndoLog& l = logs[i];
    TiClaim c;
    c.log = (int)i;
    c.stamp = l.stamp;
    c.source = l.source;
    c.finished = l.hasE;
    c.pid = l.tiPid;
    c.pstart = l.tiPstart;
    c.verified = l.tiStarted && l.tiPid != 0 && l.tiPstart != 0;
    c.stopNote = l.tiStopNote;
    return c;
}

const SvcItem* TiItem(const std::vector<SvcItem>& items) {
    for (auto& s : items) if (IEq(s.name, L"TrustedInstaller")) return &s;
    return nullptr;
}

bool SameProcess(const TiClaim& c, const SvcItem& ti) {
    return c.verified && ti.state == SERVICE_RUNNING && ti.pid == c.pid && ti.pstart != 0 && ti.pstart == c.pstart;
}

// Writes a resolution log (B with source and no rows, N, E) without taking the apply lock: the caller holds it.
bool WriteResolution(Store& st, const std::wstring& source, const std::wstring& text, const std::wstring& machine,
                     const std::wstring& build, std::wstring* err) {
    if (!st.EnsureRoot(true, err)) return false;
    const int order = NextOrder(LoadLogs(st, nullptr));
    std::wstring stamp = st.NewStamp();
    LogWriter log;
    if (!log.Open(st.Path(L"undo-" + stamp + L".log"), err)) return false;
    return log.Append("HXSVC\t1\tundo\n", err) &&
           log.Append(Record({L"B", stamp, Esc(source), L"0", machine, build, std::to_wstring(order)}), err) &&
           log.Append(Record({L"N", Esc(text)}), err) && log.Append(Record({L"E", stamp, L"0", L"0", L"0", L"done"}), err);
}
}  // namespace

std::wstring StampText(const std::wstring& st) {
    if (st.size() < 15 || st[8] != L'-') return st;
    for (size_t i = 0; i < 15; i++) if (i != 8 && !iswdigit(st[i])) return st;
    if (st.size() > 15 && st[15] != L'-') return st;   // "-N": a second log in the same second; the minute is enough
    return st.substr(0, 4) + L"-" + st.substr(4, 2) + L"-" + st.substr(6, 2) + L" " + st.substr(9, 2) + L":" + st.substr(11, 2) + L" UTC";
}

std::wstring SourceLabel(const std::wstring& source) {
    auto after = [&](const wchar_t* pre) { size_t n = wcslen(pre); return source.compare(0, n, pre) == 0 ? source.substr(n) : std::wstring(); };
    if (!after(L"preset:").empty()) {
        std::wstring rest = after(L"preset:");
        auto parts = Split(rest, L':');
        int lv = parts[0] == L"bloat" ? LV_BLOAT : parts[0] == L"stripped" ? LV_STRIPPED : parts[0] == L"minimum" ? LV_MINIMUM : -1;
        std::wstring tab = parts.size() > 1 && parts[1] == L"drivers" ? L"Drivers" : L"Services";
        return (lv >= 0 ? LevelName(lv) : rest) + L" (" + tab + L")";
    }
    if (!after(L"default-first:").empty())
        return std::wstring(L"Default: as first seen (") + (after(L"default-first:") == L"drivers" ? L"Drivers" : L"Services") + L")";
    if (!after(L"default-stock:").empty())
        return std::wstring(L"Default: Windows stock (") + (after(L"default-stock:") == L"drivers" ? L"Drivers" : L"Services") + L")";
    if (!after(L"custom:").empty()) {
        std::wstring rest = after(L"custom:");
        size_t colon = rest.find(L':');
        std::wstring tab = rest.substr(0, colon) == L"drivers" ? L"Drivers" : L"Services";
        return L"the custom preset \"" + (colon == std::wstring::npos ? rest : rest.substr(colon + 1)) + L"\" (" + tab + L")";
    }
    if (source == L"single") return L"a single start-type change";
    if (!after(L"undo:").empty()) return L"the undo of the apply of " + StampText(after(L"undo:"));
    if (!after(L"resume:").empty()) return L"the resume of the apply of " + StampText(after(L"resume:"));
    return source;
}

std::wstring InterruptedText(const std::vector<UndoLog>& logs, int k) {
    if (k < 0 || k >= (int)logs.size()) return L"";
    const UndoLog& lg = logs[k];
    int done = 0, begun = 0;
    for (auto& r : lg.rows) { if (r.hasD) done++; if (r.hasI) begun++; }
    bool undoOf = IsUndoLog(logs, k);
    // mini-build P3 / mini-design 1: the stamp as the header shows dates, and no parentheses inside parentheses
    // (preset labels carry their tab in brackets, so the label goes outside them).
    const std::wstring when = StampText(lg.stamp);
    std::wstring head;
    if (undoOf) head = L"An undo did not finish (" + when + L"; " + SourceLabel(lg.source) + L")";
    else if (lg.source.rfind(L"resume:", 0) == 0) head = L"A resume of the apply of " + StampText(lg.source.substr(7)) + L" did not finish (" + when + L")";
    else head = L"An apply of " + SourceLabel(lg.source) + L" did not finish (" + when + L")";
    std::wstring t = head + L": " + std::to_wstring(done) + L" of " + std::to_wstring(lg.count) +
                     L" changes were recorded as done" + (begun > done ? L" and 1 was in progress" : L"") +
                     (undoOf ? L". Undo and Resume both finish undoing the apply it was undoing: Undo from that apply's log, "
                               L"Resume from this one."
                             : L". Undo puts back what it changed; Resume re-plans the rest against the live state.");
    if (lg.badRecords) t += L" " + std::to_wstring(lg.badRecords) + L" record(s) in its log failed their check and were ignored.";
    if (lg.tiStarting && !lg.tiStarted && !lg.tiNotStarted)
        t += L" It stopped while starting TrustedInstaller, so Hadron can't tell whether a TrustedInstaller running now is "
             L"the one it started; Windows stops TrustedInstaller on its own when idle (reasoning).";
    t += L" Dismiss hides this banner and keeps the log and its recovery folder.";
    return t;
}

TiClaim LiveTiClaim(const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items) {
    const SvcItem* ti = TiItem(items);
    if (!ti) return TiClaim{};
    for (int i = (int)logs.size() - 1; i >= 0; i--) {   // any log: a later apply doesn't hide it
        if (!OpenTiClaim(logs, (size_t)i)) continue;
        TiClaim c = ClaimOf(logs, (size_t)i);
        if (SameProcess(c, *ti)) return c;
    }
    return TiClaim{};
}

bool TiLeftRunning(const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items) { return LiveTiClaim(logs, items).log >= 0; }

std::wstring TiLeftText(const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items) {
    TiClaim c = LiveTiClaim(logs, items);
    const SvcItem* ti = TiItem(items);
    if (c.log < 0 || !ti) return L"";
    // Two banner lines: the start type and the stop error are in the Stop it plan and the log.
    return L"Hadron started TrustedInstaller (process " + std::to_wstring(c.pid) + L") for " + SourceLabel(c.source) + L", " +
           StampText(c.stamp) + (c.finished ? L"" : L", which did not finish") + L", and " +
           (c.stopNote.empty() ? L"has no record of stopping it" : L"its stop failed") +
           L". That process is still running. Windows stops it when idle (reasoning); you can also stop it now.";
}

void MarkTiStop(Plan* p, const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items) {
    TiClaim c = LiveTiClaim(logs, items);
    const SvcItem* ti = TiItem(items);
    if (c.log < 0 || !ti) {
        for (auto& r : p->rows) {
            r.blocked = L"Not the TrustedInstaller process Hadron started (or Hadron has no record of starting it): left to Windows";
            r.include = false;
        }
        return;
    }
    p->source = L"ti-stop:" + c.stamp;
    p->tiPid = c.pid;
    p->tiPstart = c.pstart;
    for (auto& r : p->rows)
        if (r.ctl == Op::Stop && IEq(r.name, L"TrustedInstaller") && r.blocked.empty())
            r.reason = L"Hadron started this process (" + std::to_wstring(c.pid) + L") for the apply of " + StampText(c.stamp) +
                       L" and has no record of stopping it. Its start type is " + StartLabel(ti->cfg, ti->trigger) +
                       L": Windows starts it again when it next installs updates or features (reasoning).";
}

int ResolveStaleTiClaims(Store& st, const std::vector<UndoLog>& logs, const std::vector<SvcItem>& items, const std::wstring& machine,
                         const std::wstring& build, const std::wstring& lockName, Backend* backend) {
    const SvcItem* ti = TiItem(items);
    if (!ti) return 0;
    auto stale = [](const std::vector<UndoLog>& lg, const SvcItem& t) {
        std::vector<std::pair<std::wstring, std::wstring>> v;   // stamp, why
        for (size_t i = 0; i < lg.size(); i++) {
            if (!OpenTiClaim(lg, i)) continue;
            TiClaim c = ClaimOf(lg, i);
            if (t.state == SERVICE_STOPPED)
                v.push_back({c.stamp, L"TrustedInstaller was seen stopped, so the process Hadron started is gone"});
            else if (c.verified && t.pid && t.pstart && (t.pid != c.pid || t.pstart != c.pstart))
                v.push_back({c.stamp, L"a different TrustedInstaller process (" + std::to_wstring(t.pid) +
                                          L") is running, so the one Hadron started (" + std::to_wstring(c.pid) + L") is gone"});
        }
        return v;
    };
    auto todo = stale(logs, *ti);
    if (todo.empty()) return 0;
    ApplyLock lock;
    std::wstring err;
    if (!lock.Acquire(lockName, &err)) return 0;   // an apply is running: the next load tries again
    // mini-analyze A4: items and logs were read before the lock, seconds ago. Another instance may have started
    // TrustedInstaller since and still holds a live claim, so decide again from its state and the logs now.
    if (backend) {
        SvcItem now;
        std::wstring qe;
        if (!backend->Query(L"TrustedInstaller", &now, &qe)) return 0;
        todo = stale(LoadLogs(st, nullptr), now);
    }
    int n = 0;
    for (auto& t : todo)
        if (WriteResolution(st, L"ti-resolved:" + t.first, t.second, machine, build, &err)) n++;
    return n;
}

bool DismissInterrupted(Store& st, const UndoLog& log, const std::wstring& machine, const std::wstring& build,
                        const std::wstring& lockName, std::wstring* err) {
    ApplyLock lock;
    if (!lock.Acquire(lockName, err)) return false;
    return WriteResolution(st, L"dismiss:" + log.stamp, L"dismissed by the user: the apply of " + log.stamp +
                                                           L" UTC stays as it is (its log and recovery folder are kept)",
                           machine, build, err);
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
        bool drop = g.neverTouch || s.cfgUnknown || s.cfg.start < 2 || s.protect != 0 || IsStorageDriver(s.name) ||
                    Lookup(NeverPreset(), Lower(TemplateName(s.name, s.type))) != nullptr ||
                    (IsDriverType(s.type) && s.cfg.start < 2 && !SysOk(s, c));
        if (drop) { if (skipped) skipped->push_back(s.name); continue; }
        v.push_back(RestoreRow{s.name, s.cfg, s.cfg.delayed >= 0, s.aclTi});
    }
    return v;
}

namespace {
// recovery\<st> and recovery-<st>.zip are complete: the zip reads back with every CRC right and restore.cmd and
// README.txt in it, and the folder holds each of its files byte for byte (mini-analyze A3: a truncated zip, or a
// folder a crash left short, is not a copy anyone can use).
bool RecoveryComplete(const Store& s, const std::wstring& st) {
    std::string z;
    std::wstring e;
    std::vector<ZipEntry> files;
    if (!s.Exists(L"recovery\\" + st) || !s.Read(L"recovery-" + st + L".zip", &z, &e) || !ParseZip(z, &files, &e)) return false;
    bool cmd = false, readme = false;
    for (auto& f : files) {
        std::string d;
        if (!s.Read(L"recovery\\" + st + L"\\" + FromUtf8(f.name), &d, &e) || d != f.data) return false;
        cmd = cmd || f.name == "restore.cmd";
        readme = readme || f.name == "README.txt";
    }
    return cmd && readme;
}

// R12: recovery\first and its zip, written again under first-2, first-3, ... when an earlier try left either one
// missing or incomplete. A partial folder is kept as it is (never overwritten or deleted). Written only for a first.txt
// from this PC and this Windows build (mini-analyze A3, as R11 for Default): after an upgrade the old build's values
// are not what WinRE should write back.
void EnsureFirstRecovery(Store& s, const Snapshot& first, const Ctx& c, FirstRunResult* r) {
    std::vector<std::wstring> stamps = {L"first"};
    for (int k = 2; k <= 9; k++) stamps.push_back(L"first-" + std::to_wstring(k));
    for (auto& st : stamps)
        if (RecoveryComplete(s, st)) return;   // one complete copy is enough
    auto meta = [&](const wchar_t* k) { auto it = first.meta.find(k); return it == first.meta.end() ? std::wstring() : it->second; };
    const std::wstring fm = meta(L"machine"), fb = meta(L"build");
    if (fm.empty() || c.machine.empty() || fm != c.machine || BuildNumber(fb).empty() || BuildNumber(fb) != BuildNumber(c.build)) {
        if (!r->note.empty()) r->note += L" ";
        r->note += L"recovery\\first is missing or incomplete, and first.txt is from " +
                   (fm != c.machine || fm.empty() ? std::wstring(L"another PC or one with no machine GUID")
                                                  : L"Windows build " + (fb.empty() ? std::wstring(L"(missing)") : fb) +
                                                        L" (this PC runs " + (c.build.empty() ? std::wstring(L"(unknown)") : c.build) + L")") +
                   L", so it is not written again: those values are not what WinRE should put back now. Each apply still writes "
                   L"its own recovery folder.";
        return;
    }
    auto kind = first.meta.find(L"kind");
    const bool isFirst = kind == first.meta.end() || kind->second == L"first";
    for (auto& st : stamps) {
        if (s.Exists(L"recovery\\" + st) || s.Exists(L"recovery-" + st + L".zip")) continue;
        std::vector<std::wstring> skipped;
        std::vector<RestoreRow> rows = FirstRestoreRows(first, c, &skipped);
        std::wstring dir, zip, err;
        if (!WriteRecovery(s, st, isFirst ? L"start types as recorded when Hadron first ran"
                                          : L"start types as recorded in first.txt (a baseline after earlier Hadron changes)",
                           rows, &dir, &zip, &err, skipped)) {
            if (!r->error.empty()) r->error += L" ";
            r->error += L"first.txt is in place, but its recovery folder failed: " + err;
        } else if (st != L"first") {
            r->note += (r->note.empty() ? L"" : L" ") + std::wstring(L"An earlier recovery folder for first.txt was incomplete, so it was "
                                                                      L"written again as recovery\\") + st + L".";
        }
        return;
    }
    if (!r->error.empty()) r->error += L" ";
    r->error += L"first.txt is in place, but none of 9 tries left a complete recovery folder for it.";
}
}  // namespace

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
                r.note = L"first.txt is from another PC (machine GUID differs): Default: as first seen is off.";
            }
            if (!r.foreign) EnsureFirstRecovery(s, *first, c, &r);
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
    // R12: with no first.txt but undo logs present, Hadron has changed this PC before: what is recorded now is a
    // baseline, not the first state, and Default says so.
    bool baseline = r.corrupt;
    if (!r.corrupt && !s.List(L"undo-*.log").empty()) {
        baseline = true;
        r.note = L"first.txt is missing, but Hadron's undo logs show it changed services before: the new first.txt is a baseline "
                 L"of today's start types, not how Hadron first saw this PC.";
    }
    Snapshot snap;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t when[40];
    swprintf(when, 40, L"%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    snap.meta[L"kind"] = baseline ? L"baseline (not first run)" : L"first";
    snap.meta[L"created"] = when;
    snap.meta[L"build"] = build;
    snap.meta[L"machine"] = machine;
    // R2: an item whose configuration could not be read is left out: its start type is a placeholder, and recording it
    // would make Default (first snapshot) and recovery\first write a value this PC never had.
    int unreadable = 0;
    for (auto& i : items) {
        if (i.cfgUnknown) { unreadable++; continue; }
        snap.items.push_back(i);
        snap.items.back().company.clear();   // the file's claim, not configuration
    }
    if (unreadable)
        r.note += (r.note.empty() ? L"" : L" ") + std::to_wstring(unreadable) +
                  L" item(s) could not be read and are not in first.txt, so Default: as first seen leaves them alone.";
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
    EnsureFirstRecovery(s, parsed, c, &r);
    return r;
}

}  // namespace hx::services
