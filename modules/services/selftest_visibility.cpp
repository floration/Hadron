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
    bool ok = CollectItems(b, nullptr, nullptr, nullptr, &items, &st, &err);
    const SvcItem* fsrec = FindIn(items, L"Fs_Rec");
    const SvcItem* ws2 = FindIn(items, L"Winsock2");
    const SvcItem* lnk = FindIn(items, L"ExampleLinkKey");
    check(ok && st.regOnly == 3 && st.direct == 0 && fsrec && fsrec->RegOnly() && OnTab(*fsrec, TAB_DRIVERS) && ws2 && ws2->RegOnly() &&
              ws2->regNoType && OnTab(*ws2, TAB_SERVICES) && lnk && lnk->RegOnly() && lnk->regLink && lnk->regStart < 0,
          L"visibility: Services keys the SCM doesn't have list as Registry only: by Type on the Drivers tab (Fs_Rec), no Type "
          L"on the Services tab (Winsock2), a link as a leaf with no values read");
    check(fsrec && fsrec->regStart == 0 && fsrec->cfgUnknown && fsrec->regWhy.find(L"never planned") != std::wstring::npos &&
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
    CollectItems(lossy, nullptr, nullptr, nullptr, &fixed, &st2, &err);
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
    CollectItems(tib, nullptr, nullptr, nullptr, &adminOnly, &sa, &err);
    const SvcItem* wsA = FindIn(adminOnly, L"WSearch");
    const SvcItem* dtA = FindIn(adminOnly, L"DiagTrack");
    check(wsA && wsA->RegOnly() && dtA && dtA->cfgUnknown && !sa.tiAsked && TiCollectText(sa).empty(),
          L"visibility: as administrator a hidden service lists as registry only and an unreadable one as unknown");
    FakeTiRead ti;
    ti.b = &tib;
    std::vector<SvcItem> withTi;
    CollectStats stt;
    CollectItems(tib, &ti, nullptr, nullptr, &withTi, &stt, &err);
    const SvcItem* wsT = FindIn(withTi, L"WSearch");
    const SvcItem* dtT = FindIn(withTi, L"DiagTrack");
    check(stt.tiUsed && stt.tiItems == 1 && stt.tiConfigs == 1 && wsT && !wsT->RegOnly() && wsT->viaTi && dtT && !dtT->cfgUnknown && dtT->viaTi &&
              withTi.size() == adminOnly.size(),
          L"visibility: the TrustedInstaller pass adds what only it can see or read, marks those rows, and counts them (1 item, 1 config)");
    check(ti.impersonated == 1 && ti.reverted == 1 && ti.released == 1 && !tib.asTi && !ImpersonationFlag::Active() && ti.flagDuring &&
              !ti.controlDuring && !tib.tiControlAllowed,
          L"visibility: the TI pass reverts its token and clears the file-write refusal; with TrustedInstaller running it never gets the control permission");
    check(TiCollectText(stt) == L"TrustedInstaller showed 1 more item and 1 more config than administrator.",
          L"visibility: the status line says what TrustedInstaller added: \"" + TiCollectText(stt) + L"\"");
    FakeTiRead none;
    none.b = &b;
    std::vector<SvcItem> same;
    CollectStats s0;
    CollectItems(b, &none, nullptr, nullptr, &same, &s0, &err);
    check(s0.tiUsed && s0.tiItems == 0 && s0.tiConfigs == 0 &&
              TiCollectText(s0).find(L"showed 0 more items and 0 more configs") != std::wstring::npos,
          L"visibility: when TrustedInstaller sees nothing more, the status line says 0, honestly");
    FakeTiRead bad;
    bad.b = &tib;
    bad.failImpersonate = true;
    std::vector<SvcItem> afterFail;
    CollectStats sf;
    CollectItems(tib, &bad, nullptr, nullptr, &afterFail, &sf, &err);
    check(!sf.tiUsed && !sf.tiErr.empty() && afterFail.size() == adminOnly.size() && bad.released == 1 && !tib.asTi && !ImpersonationFlag::Active() &&
              TiCollectText(sf).find(L"did not run") != std::wstring::npos,
          L"visibility: a refused impersonation keeps the administrator's list, releases TrustedInstaller and says why");
    FakeTiRead noStart;
    noStart.b = &tib;
    std::vector<SvcItem> ns;
    CollectStats sn;
    tib.SetState(L"TrustedInstaller", SERVICE_STOPPED);
    CollectItems(tib, &noStart, nullptr, nullptr, &ns, &sn, &err);
    tib.SetState(L"TrustedInstaller", SERVICE_RUNNING);
    FakeTiRead running;
    running.b = &tib;
    std::vector<SvcItem> rn;
    CollectStats sr;
    CollectItems(tib, &running, nullptr, nullptr, &rn, &sr, &err);
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
              tsv.find("name\ttab\tkind\ttype\tstart\tstate\tsource\treadError\traw_start\tstart_override\n") != std::string::npos &&
              tsv.find("\nFs_Rec\tdrivers\tRegistry only (not loaded by the SCM)\t0x8\tBoot\tRegistry only\tregistry\t") != std::string::npos &&
              tsv.find("\nWinsock2\tservices\t") != std::string::npos,
          L"visibility: --dump-list-live TSV has the mask, raw count, per-tab/kind/start counts and one line per row with its source");

    // 6. StartOverride (the owner's PC: 49 inbox boot drivers with registry Start=0 and StartOverride 0/1/2 = 3, which the
    //    SCM reports as Manual). The fixture's hyperkbd has that shape.
    const SvcItem* hk = FindIn(items, L"hyperkbd");
    check(hk && hk->startOverride.size() == 3 && hk->startOverride[0].first == L"0" && hk->startOverride[0].second == 3 &&
              hk->regStart == 0 && hk->cfg.start == 3 && hk->StartOverridden() && !hk->PendingRestart() && st.overridden == 1,
          L"visibility: a driver's StartOverride values are read with its key; registry Boot under SCM Manual is an override, not a pending change");
    Plan hkP = hk ? BuildSingle(*hk, StartCfg{4, -1}, folded, c) : Plan{};
    check(hkP.rows.size() == 1 && hkP.rows[0].before.start == 3 && hkP.rows[0].op.start == 4,
          L"visibility: plans start from the SCM's (effective) start type of an overridden driver, never its raw registry Start");
    check(tsv.find("\nhyperkbd\tdrivers\t") != std::string::npos && tsv.find("\tManual\t") != std::string::npos &&
              tsv.find("\t0\t0=3;1=3;2=3\n") != std::string::npos && tsv.find("#start_overridden\t1 ") != std::string::npos,
          L"visibility: --dump-list-live has raw_start and start_override columns and counts the overridden drivers");

    // 7. Review R10: a Services key listing that stops part way says so, with the count and the error.
    {
        FakeBackend pb = b;
        pb.keysStopAt = 5;
        std::vector<SvcItem> pi;
        CollectStats ps;
        CollectItems(pb, nullptr, nullptr, nullptr, &pi, &ps, &err);
        std::string pt = DumpListTsv(pi, ps, L"19045");
        check(ps.regPartial && ps.regKeys == 5 && VisibilityText(ps).find(L"The Services key list stopped at 5: RegEnumKeyEx") != std::wstring::npos &&
                  pt.find("the Services key list stopped at 5: RegEnumKeyEx") != std::string::npos,
              L"visibility: a partial Services key list is reported in the status text and the dump header, never silent");
    }

    // 8. Review R2: a load may start TrustedInstaller for the read only after a flushed claim, the same T records an apply
    //    writes, so the left-running banner and its Stop it plan cover the read too.
    {
        struct FakeTiStart : FakeTiRead {
            bool started = false, failStop = false;
            bool Acquire(const std::atomic<bool>* cc, std::wstring* e) override {
                SvcItem t;
                std::wstring qe;
                if (b->Query(L"TrustedInstaller", &t, &qe) && t.state != SERVICE_RUNNING) {
                    if (!b->tiControlAllowed) { *e = L"fake: not armed"; return false; }
                    b->SetState(L"TrustedInstaller", SERVICE_RUNNING);
                    started = true;
                }
                return FakeTiRead::Acquire(cc, e);
            }
            bool StartedService() const override { return started; }
            bool Release(std::wstring* note) override {
                released++;
                if (!started) { *note = L""; return true; }
                started = false;
                if (failStop) { *note = L"Hadron started TrustedInstaller and could not stop it again: fake"; return false; }
                b->SetState(L"TrustedInstaller", SERVICE_STOPPED);
                *note = L"Hadron started TrustedInstaller for this apply and stopped it again.";
                return true;
            }
        };
        const std::wstring lock = L"Local\\Hadron.services.selftest.tiread." + std::to_wstring(GetCurrentProcessId());
        FakeBackend cb = tib;
        cb.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        Store cs(newTempDir(L"ti-read"));
        FakeTiStart t1;
        t1.b = &cb;
        auto claim = MakeTiReadClaim(cs, cb.MachineGuid(), cb.Build(), lock);
        std::vector<SvcItem> ci;
        CollectStats cst1;
        CollectItems(cb, &t1, claim.get(), nullptr, &ci, &cst1, &err);
        claim.reset();
        auto l1 = LoadLogs(cs, nullptr);
        SvcItem tiNow;
        cb.Query(L"TrustedInstaller", &tiNow, &err);
        check(cst1.tiUsed && t1.controlDuring && !cb.tiControlAllowed && l1.size() == 1 && l1[0].source == L"ti-read" && l1[0].tiStarting &&
                  l1[0].tiStarted && l1[0].tiStopped && l1[0].hasE && l1[0].rows.empty() && FindInterrupted(l1) < 0 && FindUndoable(l1) < 0 &&
                  tiNow.state == SERVICE_STOPPED && cst1.tiNote.find(L"for the TrustedInstaller read") != std::wstring::npos,
              L"visibility (R2): a TI read that starts TrustedInstaller writes T starting/started/stopped in a flushed ti-read log first, "
              L"stops it again, and the log is never offered as an apply to undo or resume");
        // A failed stop persists as the banner.
        Store fs(newTempDir(L"ti-read-stopfail"));
        cb.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        FakeTiStart t2;
        t2.b = &cb;
        t2.failStop = true;
        auto claim2 = MakeTiReadClaim(fs, cb.MachineGuid(), cb.Build(), lock);
        std::vector<SvcItem> ci2;
        CollectStats cst2;
        CollectItems(cb, &t2, claim2.get(), nullptr, &ci2, &cst2, &err);
        claim2.reset();
        auto l2 = LoadLogs(fs, nullptr);
        std::vector<SvcItem> live2;
        cb.Enumerate(&live2, &err);
        std::wstring banner = TiLeftText(l2, live2);
        check(l2.size() == 1 && l2[0].tiStopFailed && TiLeftRunning(l2, live2) && banner.find(L"for the TrustedInstaller read") != std::wstring::npos,
              L"visibility (R2): a stop that failed after a TI read persists as the TrustedInstaller-left-running banner: \"" + banner + L"\"");
        // Running already: nothing to record, and no control permission.
        Store rs(newTempDir(L"ti-read-running"));
        FakeTiStart t3;
        t3.b = &cb;   // still running from the failed stop
        auto claim3 = MakeTiReadClaim(rs, cb.MachineGuid(), cb.Build(), lock);
        std::vector<SvcItem> ci3;
        CollectStats cst3;
        CollectItems(cb, &t3, claim3.get(), nullptr, &ci3, &cst3, &err);
        claim3.reset();
        check(cst3.tiUsed && !t3.controlDuring && LoadLogs(rs, nullptr).empty(),
              L"visibility (R2): with TrustedInstaller running the read records nothing and never gets the control permission");
        // The claim can't be written (another window holds the apply lock): TrustedInstaller is not started.
        cb.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        ApplyLock held;
        std::wstring le;
        held.Acquire(lock, &le);
        Store ls(newTempDir(L"ti-read-locked"));
        FakeTiStart t4;
        t4.b = &cb;
        auto claim4 = MakeTiReadClaim(ls, cb.MachineGuid(), cb.Build(), lock);
        std::vector<SvcItem> ci4;
        CollectStats cst4;
        CollectItems(cb, &t4, claim4.get(), nullptr, &ci4, &cst4, &err);
        claim4.reset();
        held.Release();
        cb.Query(L"TrustedInstaller", &tiNow, &err);
        check(!cst4.tiUsed && t4.acquired == 0 && tiNow.state == SERVICE_STOPPED && cst4.tiErr.find(L"could not be recorded first") != std::wstring::npos,
              L"visibility (R2): when the claim can't be written first, TrustedInstaller is not started and the status says why");
    }
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
    CollectItems(b, nullptr, nullptr, nullptr, &items, &cst, &err);
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
    check(k >= 0 && logs[(size_t)k].rows.size() == 1 && logs[(size_t)k].rows[0].prot,
          L"override (R1): an override apply marks its protected row \"prot\" in the undo log's P record");
    check(u0.rows.size() == 1 && !u0.rows[0].blocked.empty() && OverrideOffered(u0) && u1.rows.size() == 1 && u1.rows[0].protectedRow &&
              u1.rows[0].blocked.empty() && u1.rows[0].after.start == 0 && OverrideOffered(u1),
          L"override: its undo offers the box for the marked row, refuses it with the box off, and offers a PROTECTED row back to Boot with it on");
    if (k >= 0) {   // R1: a log row without the mark (forged, or from an ordinary apply) is never lifted, whatever the box says
        UndoLog forged = logs[(size_t)k];
        for (auto& rr : forged.rows) rr.prot = false;
        Plan fu = BuildUndo(forged, ab, afterItems, on);
        // A forward protected change is never resumed, even marked: only an undo's restore is.
        UndoLog half = logs[(size_t)k];
        half.hasE = false;
        for (auto& rr : half.rows) { rr.hasD = false; rr.hasI = false; rr.before = StartCfg{4, -1}; rr.after = StartCfg{3, -1}; }
        Plan fr = BuildResume(half, ab, afterItems, on);
        check(fu.rows.size() == 1 && !fu.rows[0].blocked.empty() && !fu.rows[0].protectedRow && fu.protMarked == 0 && !OverrideOffered(fu) &&
                  fr.rows.size() == 1 && !fr.rows[0].blocked.empty() && !fr.rows[0].protectedRow && !OverrideOffered(fr),
              L"override (R1): Undo and Resume lift only rows an override apply marked, only back to the logged before value; "
              L"an unmarked Boot restore and a protected forward resume stay refused with the box on");
    }
    {   // R1: Boot/System targets are checked before never-touch, so a never-touch item can't hide them.
        Plan sysT = BuildSingle(*bootDrv, StartCfg{1, -1}, items, on);
        check(sysT.rows.size() == 1 && !sysT.rows[0].blocked.empty() && !sysT.rows[0].protectedRow,
              L"override (R1): a System target on a never-touch Boot driver is refused, not offered, with the box on");
    }
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

