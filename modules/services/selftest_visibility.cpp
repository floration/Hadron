// modules/services/selftest_visibility.cpp — fix/svcdrv-visibility selftests (selftest_visibility.h). Fake backend and
// fixture only: nothing here reads or writes a real service, key or token.
#include "modules/services/selftest_visibility.h"
#include "modules/services/cli.h"
#include "modules/services/collect.h"
#include "modules/services/plan.h"
#include "modules/services/regfallback.h"
#include "modules/services/store.h"
#include <set>

namespace hx::services {
namespace {

// A read-pass TrustedInstaller for the fake: "impersonating" flips the fake's identity, Revert flips it back.
struct FakeTiRead : TiProvider {
    FakeBackend* b = nullptr;
    bool failImpersonate = false, failAcquire = false;
    int acquired = 0, impersonated = 0, reverted = 0, released = 0;
    bool flagDuring = false, controlDuring = false;
    bool Acquire(const std::atomic<bool>*, std::wstring* err) override {
        controlDuring = b->tiControlAllowed;
        if (failAcquire) { *err = L"fake: could not get the token"; return false; }
        acquired++;
        return true;
    }
    bool StartedService() const override { return false; }
    bool Impersonate(std::wstring* identity, std::wstring* err) override {
        if (failImpersonate) { *err = L"fake: impersonation refused"; return false; }
        impersonated++;
        flagDuring = ImpersonationFlag::Active();
        b->asTi = true;
        *identity = L"ti";
        return true;
    }
    void Revert() override { reverted++; b->asTi = false; }
    bool Release(std::wstring* note) override { released++; *note = L""; return true; }
};

int CountIf(const std::vector<SvcItem>& v, const std::function<bool(const SvcItem&)>& f) {
    int n = 0;
    for (auto& s : v) if (f(s)) n++;
    return n;
}
const SvcItem* FindIn(const std::vector<SvcItem>& v, const std::wstring& name) {
    for (auto& s : v) if (IEq(s.name, name)) return &s;
    return nullptr;
}

}  // namespace

void RunVisibilitySelftest(const std::wstring& fixture, const Kb& kb, const std::function<std::wstring(const std::wstring&)>& newTempDir,
                           const SelftestCheck& check, const SelftestInfo& info) {
    (void)info;
    FixtureData fx;
    std::wstring err;
    if (!LoadFixtureDir(fixture, &fx, &err)) { check(false, L"visibility: fixture loads: " + err); return; }
    FakeBackend& b = fx.backend;
    Ctx c;
    c.kb = &kb;
    HwInfo hw;
    b.ReadHw(&hw);
    c.hw = hw.ok ? &hw : nullptr;
    c.build = b.Build();
    c.machine = b.MachineGuid();
    c.Prepare();

    // 1. The registry cross-check on the fixture's regkeys.txt.
    std::vector<SvcItem> items;
    CollectStats st;
    bool ok = CollectItems(b, nullptr, false, nullptr, &items, &st, &err);
    const SvcItem* fsrec = FindIn(items, L"Fs_Rec");
    const SvcItem* ws2 = FindIn(items, L"Winsock2");
    const SvcItem* lnk = FindIn(items, L"ExampleLinkKey");
    check(ok && st.regOnly == 3 && st.direct == 0 && fsrec && fsrec->RegOnly() && OnTab(*fsrec, TAB_DRIVERS) && ws2 && ws2->RegOnly() &&
              ws2->regNoType && OnTab(*ws2, TAB_SERVICES) && lnk && lnk->RegOnly() && lnk->regLink && lnk->regStart < 0,
          L"visibility: Services keys the SCM doesn't have list as Registry only: by Type on the Drivers tab (Fs_Rec), no Type "
          L"on the Services tab (Winsock2), a link as a leaf with no values read");
    check(fsrec && fsrec->regStart == 1 && fsrec->cfgUnknown && fsrec->regWhy.find(L"never planned") != std::wstring::npos &&
              StateName(fsrec->state) == L"Registry only",
          L"visibility: a registry-only row shows its key's Start, says why, and carries cfgUnknown (no plan can use it)");

    // 2. The reported bug's shape: rows the SCM's enumeration did not return (here every Boot-start driver) were simply
    //    absent from the page. Reproduced with enum=miss on the fake; the cross-check reads them by name.
    int boot = 0;
    for (auto& i : b.Items()) if (IsDriverType(i.type) && i.cfg.start == 0) boot++;
    FakeBackend lossy = b;
    {
        std::vector<SvcItem> all = lossy.Items();
        for (auto& i : all) if (IsDriverType(i.type) && i.cfg.start == 0) i.fxEnumMiss = true;
        lossy.SetItems(all);
    }
    std::vector<SvcItem> plain;
    lossy.Enumerate(&plain, &err);
    int bootPlain = CountIf(plain, [](const SvcItem& s) { return IsDriverType(s.type) && s.cfg.start == 0; });
    std::vector<SvcItem> fixed;
    CollectStats st2;
    CollectItems(lossy, nullptr, false, nullptr, &fixed, &st2, &err);
    int bootFixed = CountIf(fixed, [](const SvcItem& s) {
        return IsDriverType(s.type) && !s.RegOnly() && !s.cfgUnknown && s.cfg.start == 0 && s.src == SvcItem::Source::ScmDirect;
    });
    check(boot > 0 && bootPlain == 0 && bootFixed == boot && st2.direct == boot,
          L"visibility: Boot-start drivers the SCM's list leaves out are read by name and shown with their real start type (" +
              std::to_wstring(bootFixed) + L" of " + std::to_wstring(boot) + L"; the bare enumeration showed " + std::to_wstring(bootPlain) + L")");

    // 3. Registry-only rows are never planned, written, recorded or restored.
    std::vector<SvcItem> folded = items;
    FoldInstances(&folded);
    bool inPlan = false;
    for (int lv = 0; lv < LV_COUNT; lv++)
        for (int tab : {TAB_SERVICES, TAB_DRIVERS})
            for (auto& r : BuildPreset(lv, tab, folded, c).rows)
                if (IEq(r.name, L"Fs_Rec") || IEq(r.name, L"Winsock2") || IEq(r.name, L"ExampleLinkKey")) inPlan = true;
    Plan single = fsrec ? BuildSingle(*fsrec, StartCfg{4, -1}, folded, c) : Plan{};
    CustomPreset cp;
    cp.name = L"reg";
    cp.tab = TAB_DRIVERS;
    cp.items.push_back({L"Fs_Rec", StartCfg{4, -1}});
    Plan cust = BuildCustom(cp, folded, c);
    bool noteSays = false;
    for (auto& n : cust.notes) if (n.find(L"registry only") != std::wstring::npos) noteSays = true;
    Guard g = fsrec ? CheckGuard(*fsrec, c) : Guard{};
    check(!inPlan && single.Included() == 0 && !single.rows.empty() && !single.rows[0].blocked.empty() && cust.rows.empty() && noteSays &&
              g.neverTouch && StockFor(*fsrec, c).kind == StockPick::None,
          L"visibility: registry-only rows are in no preset, blocked as a single change, dropped from a custom preset with a note, "
          L"and get no stock value");
    {
        Store s(newTempDir(L"vis-first"));
        Snapshot first;
        FirstRunResult r = EnsureFirstSnapshot(s, folded, c, &first);
        bool none = !first.Find(L"Fs_Rec") && !first.Find(L"Winsock2") && !first.Find(L"ExampleLinkKey");
        check(r.wrote && none && r.note.find(L"could not be read") == std::wstring::npos,
              L"visibility: registry-only rows are never recorded in first.txt (and not counted as unreadable)" +
                  (r.error.empty() ? L"" : L": " + r.error));
    }

    // 4. TrustedInstaller read pass: an item only TrustedInstaller sees and one whose configuration only it can read.
    FakeBackend tib = b;
    {
        std::vector<SvcItem> all = tib.Items();
        for (auto& i : all) {
            if (IEq(i.name, L"WSearch")) i.fxTiOnly = true;
            if (IEq(i.name, L"DiagTrack")) i.fxCfgTi = true;
        }
        tib.SetItems(all);
    }
    std::vector<SvcItem> adminOnly;
    CollectStats sa;
    CollectItems(tib, nullptr, false, nullptr, &adminOnly, &sa, &err);
    const SvcItem* wsA = FindIn(adminOnly, L"WSearch");
    const SvcItem* dtA = FindIn(adminOnly, L"DiagTrack");
    check(wsA && wsA->RegOnly() && dtA && dtA->cfgUnknown && !sa.tiAsked && TiCollectText(sa).empty(),
          L"visibility: as administrator a hidden service lists as registry only and an unreadable one as unknown");
    FakeTiRead ti;
    ti.b = &tib;
    std::vector<SvcItem> withTi;
    CollectStats stt;
    CollectItems(tib, &ti, true, nullptr, &withTi, &stt, &err);
    const SvcItem* wsT = FindIn(withTi, L"WSearch");
    const SvcItem* dtT = FindIn(withTi, L"DiagTrack");
    check(stt.tiUsed && stt.tiItems == 1 && stt.tiConfigs == 1 && wsT && !wsT->RegOnly() && wsT->viaTi && dtT && !dtT->cfgUnknown && dtT->viaTi &&
              withTi.size() == adminOnly.size(),
          L"visibility: the TrustedInstaller pass adds what only it can see or read, marks those rows, and counts them (1 item, 1 config)");
    check(ti.impersonated == 1 && ti.reverted == 1 && ti.released == 1 && !tib.asTi && !ImpersonationFlag::Active() && ti.flagDuring &&
              ti.controlDuring && !tib.tiControlAllowed,
          L"visibility: the TI pass reverts its token, clears the file-write refusal and the TrustedInstaller control permission on the way out");
    check(TiCollectText(stt) == L"TrustedInstaller showed 1 more item and 1 more config than administrator.",
          L"visibility: the status line says what TrustedInstaller added: \"" + TiCollectText(stt) + L"\"");
    FakeTiRead none;
    none.b = &b;
    std::vector<SvcItem> same;
    CollectStats s0;
    CollectItems(b, &none, true, nullptr, &same, &s0, &err);
    check(s0.tiUsed && s0.tiItems == 0 && s0.tiConfigs == 0 &&
              TiCollectText(s0).find(L"showed 0 more items and 0 more configs") != std::wstring::npos,
          L"visibility: when TrustedInstaller sees nothing more, the status line says 0, honestly");
    FakeTiRead bad;
    bad.b = &tib;
    bad.failImpersonate = true;
    std::vector<SvcItem> afterFail;
    CollectStats sf;
    CollectItems(tib, &bad, true, nullptr, &afterFail, &sf, &err);
    check(!sf.tiUsed && !sf.tiErr.empty() && afterFail.size() == adminOnly.size() && bad.released == 1 && !tib.asTi && !ImpersonationFlag::Active() &&
              TiCollectText(sf).find(L"did not run") != std::wstring::npos,
          L"visibility: a refused impersonation keeps the administrator's list, releases TrustedInstaller and says why");
    FakeTiRead noStart;
    noStart.b = &tib;
    std::vector<SvcItem> ns;
    CollectStats sn;
    tib.SetState(L"TrustedInstaller", SERVICE_STOPPED);
    CollectItems(tib, &noStart, false, nullptr, &ns, &sn, &err);
    tib.SetState(L"TrustedInstaller", SERVICE_RUNNING);
    FakeTiRead running;
    running.b = &tib;
    std::vector<SvcItem> rn;
    CollectStats sr;
    CollectItems(tib, &running, false, nullptr, &rn, &sr, &err);
    check(!sn.tiUsed && noStart.acquired == 0 && sn.tiErr.find(L"never starts a service") != std::wstring::npos && !tib.tiControlAllowed,
          L"visibility: --dump-list-live's TI read never starts TrustedInstaller: stopped means skipped, with the reason");
    check(sr.tiUsed && sr.tiItems == 1 && !tib.tiControlAllowed && running.controlDuring == false,
          L"visibility: --dump-list-live uses an already running TrustedInstaller without the control permission");

    // 5. --dump-list-live's text.
    std::vector<SvcItem> dumpRows = items;
    FoldInstances(&dumpRows);
    std::string tsv = DumpListTsv(dumpRows, st, L"19045");
    check(tsv.find("#enum_mask\t0x3FF\n") != std::string::npos && tsv.find("#registry_only\t3\n") != std::string::npos &&
              tsv.find("#scm_raw\t") != std::string::npos && tsv.find("#elapsed_ms\t") != std::string::npos &&
              tsv.find("#by_start\t") != std::string::npos &&
              tsv.find("name\ttab\tkind\ttype\tstart\tstate\tsource\treadError\n") != std::string::npos &&
              tsv.find("\nFs_Rec\tdrivers\tRegistry only (not loaded by the SCM)\t0x8\tSystem\tRegistry only\tregistry\t") != std::string::npos &&
              tsv.find("\nWinsock2\tservices\t") != std::string::npos,
          L"visibility: --dump-list-live TSV has the mask, raw count, per-tab/kind/start counts and one line per row with its source");
}

// ============================================================================================
// The protected-items override (DECISIONS 2026-11-11, owner), on the fake backend and temp data folders.
void RunOverrideSelftest(const std::wstring& fixture, const Kb& kb, const std::function<std::wstring(const std::wstring&)>& newTempDir,
                         const SelftestCheck& check, const SelftestInfo& info) {
    FixtureData fx;
    std::wstring err;
    if (!LoadFixtureDir(fixture, &fx, &err)) { check(false, L"override: fixture loads: " + err); return; }
    FakeBackend& b = fx.backend;
    HwInfo hw;
    b.ReadHw(&hw);
    Ctx off;
    off.kb = &kb;
    off.hw = hw.ok ? &hw : nullptr;
    off.first = fx.hasFirst ? &fx.first : nullptr;
    off.build = b.Build();
    off.machine = b.MachineGuid();
    off.Prepare();
    Ctx on = off;
    on.overrideProtected = true;
    std::vector<SvcItem> items;
    CollectStats cst;
    CollectItems(b, nullptr, false, nullptr, &items, &cst, &err);
    FoldInstances(&items);

    // A never-touch service (the floor) and a Boot-start driver, both readable and not launch-protected.
    const SvcItem* floorSvc = nullptr;
    const SvcItem* bootDrv = nullptr;
    for (auto& s : items) {
        if (s.RegOnly() || s.cfgUnknown || s.protect || s.kind == Kind::UserInstance) continue;
        Guard g = CheckGuard(s, off);
        if (!floorSvc && !IsDriverType(s.type) && g.neverTouch && s.cfg.start >= 2 && s.cfg.start < 4) floorSvc = &s;
        if (!bootDrv && IsDriverType(s.type) && s.cfg.start == 0 && g.neverTouch) bootDrv = &s;
    }
    check(floorSvc && bootDrv, L"override: the fixture has a never-touch service and a Boot-start driver (" +
                                   std::wstring(floorSvc ? floorSvc->name : L"-") + L", " + (bootDrv ? bootDrv->name : L"-") + L")");
    if (!floorSvc || !bootDrv) return;

    // 1. Off by default: a Ctx starts with it off, and a protected row stays blocked.
    Plan s0 = BuildSingle(*floorSvc, StartCfg{4, -1}, items, off);
    check(!Ctx{}.overrideProtected && s0.rows.size() == 1 && !s0.rows[0].blocked.empty() && !s0.rows[0].protectedRow,
          L"override: off by default; a never-touch service stays blocked as a single change");
    // 2. On: offered unticked, marked, with its guard reason, and a plan note.
    Plan s1 = BuildSingle(*floorSvc, StartCfg{4, -1}, items, on);
    Plan d1 = BuildSingle(*bootDrv, StartCfg{4, -1}, items, on);
    check(s1.rows.size() == 1 && s1.rows[0].blocked.empty() && s1.rows[0].protectedRow && !s1.rows[0].include && s1.rows[0].canToggle &&
              !s1.rows[0].protReason.empty() && d1.rows.size() == 1 && d1.rows[0].protectedRow && d1.rows[0].blocked.empty() &&
              !s1.notes.empty() && s1.notes[0].find(L"PROTECTED") != std::wstring::npos && OverrideOffered(s1) && s1.Protected() == 0,
          L"override: on, a never-touch service and a Boot-start driver become tickable rows, unticked, marked PROTECTED with the guard reason");

    // 3. Built-in presets, Default and Stock: byte-identical with it on.
    bool same = true;
    for (int tab : {TAB_SERVICES, TAB_DRIVERS}) {
        for (int lv = 0; lv < LV_COUNT; lv++) same = same && DumpPlan(BuildPreset(lv, tab, items, off)) == DumpPlan(BuildPreset(lv, tab, items, on));
        same = same && DumpPlan(BuildDefault(false, tab, items, off)) == DumpPlan(BuildDefault(false, tab, items, on));
        same = same && DumpPlan(BuildDefault(true, tab, items, off)) == DumpPlan(BuildDefault(true, tab, items, on));
    }
    check(same && !OverrideOffered(BuildPreset(LV_STRIPPED, TAB_SERVICES, items, on)) && !OverrideOffered(BuildDefault(true, TAB_DRIVERS, items, on)),
          L"override: built-in presets, Default and Stock are the same plans with it on, and never offer it");

    // 4. Refused even with it on: launch-protected, registry only, an unreadable configuration, Boot/System targets.
    const SvcItem* lp = FindIn(items, L"WinDefend");
    const SvcItem* reg = FindIn(items, L"Fs_Rec");
    SvcItem unk = *floorSvc;
    unk.cfgUnknown = true;
    unk.readError = std::wstring(kCfgUnreadable) + L": test";
    Plan lpP = lp ? BuildSingle(*lp, StartCfg{4, -1}, items, on) : Plan{};
    Plan regP = reg ? BuildSingle(*reg, StartCfg{4, -1}, items, on) : Plan{};
    Plan unkP = BuildSingle(unk, StartCfg{4, -1}, items, on);
    Plan sysUp = BuildSingle(*floorSvc, StartCfg{1, -1}, items, on);
    auto blockedRow = [](const Plan& p) { return p.rows.size() == 1 && !p.rows[0].blocked.empty() && !p.rows[0].protectedRow; };
    check(lp && blockedRow(lpP) && lpP.rows[0].blocked.find(L"Windows itself refuses") != std::wstring::npos && reg && blockedRow(regP) &&
              blockedRow(unkP) && !sysUp.rows.empty() && !sysUp.rows[0].blocked.empty(),
          L"override: launch-protected (Windows itself refuses), registry-only and unreadable items stay refused, and Boot/System targets too");
    CustomPreset cp;
    cp.name = L"prot";
    cp.tab = TAB_SERVICES;
    cp.items = {{floorSvc->name, StartCfg{4, -1}}, {L"WinDefend", StartCfg{4, -1}}};
    Plan c0 = BuildCustom(cp, items, off), c1 = BuildCustom(cp, items, on);
    const PlanRow* cr = nullptr;
    for (auto& r : c1.rows) if (IEq(r.name, floorSvc->name)) cr = &r;
    bool wdDropped = false;
    for (auto& n : c1.notes) if (n.find(L"WinDefend") != std::wstring::npos && n.find(L"Windows itself refuses") != std::wstring::npos) wdDropped = true;
    check(c0.rows.empty() && cr && cr->protectedRow && !cr->include && wdDropped && OverrideOffered(c1),
          L"override: a custom preset offers its protected rows unticked with it on, drops them with it off, and still drops launch-protected ones");

    // 5. The registry fallback never takes a protected row.
    {
        FakeBackend rb = b;
        std::vector<SvcItem> all = rb.Items();
        for (auto& i : all) if (IEq(i.name, floorSvc->name)) i.aclTi = true;   // the SCM refuses administrators
        rb.SetItems(all);
        Plan rp = BuildSingle(*floorSvc, StartCfg{4, -1}, items, on);
        MarkRegFallback(&rp, rb, true, false, items, on);
        check(rp.rows.size() == 1 && !rp.rows[0].viaReg && rp.rows[0].regRefused.find(L"protected row") != std::wstring::npos,
              L"override: a protected row is never written through the registry fallback (SCM path only)");
    }

    // 6. Apply and undo a protected row with it on: the Boot-start driver to Disabled and back to Boot, through the same
    //    snapshot, flushed undo log and recovery zip as every apply.
    FakeBackend ab = b;
    Store st(newTempDir(L"override"));
    ApplyEnv env;
    env.backend = &ab;
    env.store = &st;
    env.lockName = L"Local\\Hadron.services.selftest.override." + std::to_wstring(GetCurrentProcessId());
    env.build = b.Build();
    env.machine = b.MachineGuid();
    env.requireFirst = false;
    Plan ap = BuildSingle(*bootDrv, StartCfg{4, -1}, items, on);
    if (ap.rows.size() == 1) ap.rows[0].include = true;   // the user's tick
    ApplyResult ar;
    bool applied = ap.rows.size() == 1 && RunApply(ap, env, &ar);
    SvcItem now;
    ab.Query(bootDrv->name, &now, &err);
    check(applied && ar.ok == 1 && now.cfg.start == 4 && !ar.zipPath.empty() && !ar.logPath.empty() && !ar.snapPath.empty(),
          L"override: a ticked protected row is applied (" + bootDrv->name + L" Boot -> Disabled) with the snapshot, undo log and recovery zip" +
              (ar.error.empty() ? L"" : L": " + ar.error));
    std::vector<SvcItem> afterItems;
    ab.Enumerate(&afterItems, &err);
    FoldInstances(&afterItems);
    auto logs = LoadLogs(st, nullptr);
    int k = FindUndoable(logs);
    Plan u0 = k >= 0 ? BuildUndo(logs[(size_t)k], ab, afterItems, off) : Plan{};
    Plan u1 = k >= 0 ? BuildUndo(logs[(size_t)k], ab, afterItems, on) : Plan{};
    check(u0.rows.size() == 1 && !u0.rows[0].blocked.empty() && u1.rows.size() == 1 && u1.rows[0].protectedRow && u1.rows[0].blocked.empty() &&
              u1.rows[0].after.start == 0 && OverrideOffered(u1),
          L"override: its undo is refused with the override off and offered (a PROTECTED row back to Boot) with it on");
    bool undone = false;
    if (u1.rows.size() == 1) {
        u1.rows[0].include = true;
        ApplyResult ur;
        undone = RunApply(u1, env, &ur) && ur.ok == 1;
        ab.Query(bootDrv->name, &now, &err);
    }
    check(undone && now.cfg.start == 0, L"override: the undo puts the Boot-start driver back to Boot");
    info(L"override: applied and undone " + bootDrv->name + L" in " + st.Root());
}

}  // namespace hx::services

