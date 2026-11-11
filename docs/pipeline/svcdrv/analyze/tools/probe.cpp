// Property / counterexample probes for the services plan engine. Scratch only; fake backend only.
#include "modules/services/plan.h"
#include "modules/services/backend.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>

using namespace hx::services;

static int fails = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) fails++;
    printf("  [%s] %s\n", ok ? "HOLDS" : "BROKEN", what);
}

// K name kind builds title expl ifOff test lv0 lv1 lv2 why0 why1 why2 sysok never basis basisNote pbasis pbasisNote
//   stock stockBasis stockNote hw contested sources  (26 fields)
static std::string K(const char* name, const char* kind, const char* lv0, const char* lv1, const char* lv2,
                     bool sysok = false, const char* hw = "") {
    std::string f[26] = {"K", name, kind, "19045", "t", "e", "o", "x", lv0, lv1, lv2, "w", "w", "w",
                         sysok ? "1" : "0", "0", "reasoning", "", "reasoning", "", "", "", "", hw, "", ""};
    std::string s;
    for (int i = 0; i < 26; i++) { if (i) s += "\t"; s += f[i]; }
    return s + "\n";
}

static SvcItem Svc(const wchar_t* name, int start, int delayed = -1, std::vector<std::wstring> deps = {}, DWORD type = 0x10) {
    SvcItem s;
    s.name = name; s.display = name; s.type = type; s.cfg = StartCfg{start, delayed}; s.deps = deps;
    s.kind = KindOf(type);
    return s;
}

static Ctx MakeCtx(const Kb* kb, const HwInfo* hw) {
    Ctx c; c.kb = kb; c.hw = hw; c.build = L"19045"; c.machine = L"m"; c.Prepare(); return c;
}

static const PlanRow* Row(const Plan& p, const wchar_t* n) {
    for (auto& r : p.rows) if (IEq(r.name, n)) return &r;
    return nullptr;
}

static std::wstring TempDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    for (int i = 0; i < 1000; i++) {
        std::wstring d = std::wstring(tmp) + L"svcprobe-" + tag + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(i);
        if (CreateDirectoryW(d.c_str(), nullptr)) return d;
    }
    return L"";
}

struct NoTi : TiProvider {
    bool Acquire(std::wstring*) override { return false; }
    bool StartedService() const override { return false; }
    bool Impersonate(std::wstring*, std::wstring*) override { return false; }
    void Revert() override {}
    bool Release(std::wstring*) override { return true; }
};

static ApplyEnv Env(Backend* b, Store* s) {
    ApplyEnv e; e.backend = b; e.store = s; e.lockName = L"Local\\svcprobe." + std::to_wstring(GetCurrentProcessId());
    e.build = L"19045"; e.machine = L"m"; return e;
}

static StartCfg CfgOf(FakeBackend& b, const wchar_t* n) {
    for (auto& i : b.Items()) if (IEq(i.name, n)) return i.cfg;
    return StartCfg{-1, -1};
}

int wmain(int argc, wchar_t** argv) {
    Store::AcceptTokenUserAsOwner() = true;   // Wine ignores SD owners (the selftest sets the same hook)
    HwInfo hw; hw.ok = true; hw.bootDisk = L"PCI\\BOOT";

    // ---------------------------------------------------------------------------------------------
    printf("P1 DependencyCheck: transitive chain, order of rows\n");
    {
        // A (bloat, Disabled) <- B depends on A (bloat, Disabled) <- C depends on B (no KB, stays Manual).
        Kb kb; std::wstring err;
        kb.Parse("V\t1\tservices\nB\t19045\n" + K("aaa", "svc", "4", "", "") + K("bbb", "svc", "4", "", ""), &err);
        std::vector<SvcItem> items = {Svc(L"aaa", 3), Svc(L"bbb", 3, -1, {L"aaa"}), Svc(L"ccc", 3, -1, {L"bbb"})};
        Ctx c = MakeCtx(&kb, &hw);
        Plan p = BuildPreset(LV_BLOAT, TAB_SERVICES, items, c);
        const PlanRow* a = Row(p, L"aaa"); const PlanRow* b = Row(p, L"bbb");
        printf("    aaa include=%d warn='%ls'\n    bbb include=%d warn='%ls'\n", a->include, a->warning.c_str(), b->include, b->warning.c_str());
        Check(!b->include, "bbb (ccc stays Manual and depends on it) is unticked");
        Check(!a->include, "aaa is unticked too, because bbb now stays enabled and depends on it");
        // apply what is ticked and look at the result
        FakeBackend fb; fb.SetItems(items);
        Store st(TempDir(L"p1")); st.fixedStamp = L"20261111-000000";
        ApplyResult r; RunApply(p, Env(&fb, &st), &r); if (r.aborted) printf("    APPLY ABORTED: %ls\n", r.error.c_str());
        printf("    after apply: aaa=%ls bbb=%ls ccc=%ls\n", CfgText(CfgOf(fb, L"aaa")).c_str(), CfgText(CfgOf(fb, L"bbb")).c_str(),
               CfgText(CfgOf(fb, L"ccc")).c_str());
        Check(!(CfgOf(fb, L"aaa").start == 4 && CfgOf(fb, L"bbb").start != 4),
              "after apply, no enabled service depends on a disabled one");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P2 User unticks a dependent; the plan is not re-checked\n");
    {
        Kb kb; std::wstring err;
        kb.Parse("V\t1\tservices\nB\t19045\n" + K("aaa", "svc", "4", "", "") + K("bbb", "svc", "4", "", ""), &err);
        std::vector<SvcItem> items = {Svc(L"aaa", 3), Svc(L"bbb", 3, -1, {L"aaa"})};
        Ctx c = MakeCtx(&kb, &hw);
        Plan p = BuildPreset(LV_BLOAT, TAB_SERVICES, items, c);
        for (size_t k = 0; k < p.rows.size(); k++) if (p.rows[k].name == L"bbb" && p.rows[k].canToggle) { p.rows[k].include = false; DependencyCheck(&p, items, (int)k); }   // TogglePlanRow (round 2)
        FakeBackend fb; fb.SetItems(items);
        Store st(TempDir(L"p2")); st.fixedStamp = L"20261111-000000";
        ApplyResult r; RunApply(p, Env(&fb, &st), &r); if (r.aborted) printf("    APPLY ABORTED: %ls\n", r.error.c_str());
        printf("    after apply: aaa=%ls bbb=%ls\n", CfgText(CfgOf(fb, L"aaa")).c_str(), CfgText(CfgOf(fb, L"bbb")).c_str());
        Check(!(CfgOf(fb, L"aaa").start == 4 && CfgOf(fb, L"bbb").start != 4),
              "unticking bbb re-warns/unticks aaa (no enabled dependent of a disabled service)");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P3 RunApply: a failed dependent blocks only one level\n");
    {
        Kb kb; std::wstring err;
        kb.Parse("V\t1\tservices\nB\t19045\n" + K("aaa", "svc", "4", "", "") + K("bbb", "svc", "4", "", "") + K("ccc", "svc", "4", "", ""), &err);
        std::vector<SvcItem> items = {Svc(L"aaa", 3), Svc(L"bbb", 3, -1, {L"aaa"}), Svc(L"ccc", 3, -1, {L"bbb"})};
        Ctx c = MakeCtx(&kb, &hw);
        Plan p = BuildPreset(LV_BLOAT, TAB_SERVICES, items, c);
        FakeBackend fb; fb.SetItems(items);
        fb.faults.verifyMismatch.insert(L"ccc");   // the top dependent's write does not stick
        Store st(TempDir(L"p3")); st.fixedStamp = L"20261111-000000";
        ApplyResult r; RunApply(p, Env(&fb, &st), &r); if (r.aborted) printf("    APPLY ABORTED: %ls\n", r.error.c_str());
        for (auto& l : r.lines) printf("    %ls\n", l.c_str());
        printf("    after apply: aaa=%ls bbb=%ls ccc=%ls\n", CfgText(CfgOf(fb, L"aaa")).c_str(), CfgText(CfgOf(fb, L"bbb")).c_str(),
               CfgText(CfgOf(fb, L"ccc")).c_str());
        Check(!(CfgOf(fb, L"aaa").start == 4 && CfgOf(fb, L"bbb").start != 4),
              "a skipped (not failed) dependent also protects what it depends on");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P4 Default (first snapshot) exactness with a stale DelayedAutostart\n");
    {
        Kb kb; std::wstring err;
        kb.Parse("V\t1\tservices\nB\t19045\n" + K("aaa", "svc", "4", "", ""), &err);
        Snapshot first; first.meta[L"machine"] = L"m";
        first.items = {Svc(L"aaa", 2, 0)};     // first run: Auto, not delayed (value 0)
        std::vector<SvcItem> items = {Svc(L"aaa", 4, 1)};   // now: Disabled with a leftover DelayedAutostart=1
        Ctx c = MakeCtx(&kb, &hw); c.first = &first;
        Plan p = BuildDefault(false, TAB_SERVICES, items, c);
        const PlanRow* a = Row(p, L"aaa");
        printf("    row: %ls -> %ls  op={%d,%d}\n", CfgText(a->before).c_str(), CfgText(a->after).c_str(), a->op.start, a->op.delayed);
        FakeBackend fb; fb.SetItems(items);
        Store st(TempDir(L"p4")); st.fixedStamp = L"20261111-000000";
        ApplyResult r; RunApply(p, Env(&fb, &st), &r); if (r.aborted) printf("    APPLY ABORTED: %ls\n", r.error.c_str());
        StartCfg now = CfgOf(fb, L"aaa");
        printf("    after apply: %ls (%ls); first.txt says 2/0 (Auto); result line: %ls\n", CfgText(now).c_str(), StartLabel(now, false).c_str(),
               r.lines.empty() ? L"" : r.lines[0].c_str());
        Check(now.SameEffect(first.items[0].cfg), "restore-first-snapshot gives the recorded effect (Auto, not Auto delayed)");

        // same for undo: Hadron set 2/- -> 4; something then set DelayedAutostart=1 while disabled
        UndoLog log; log.stamp = L"S1"; log.hasB = log.hasE = true; log.source = L"preset:bloat:services"; log.machine = L"m"; log.build = L"19045";
        UndoRow u; u.seq = 1; u.name = L"aaa"; u.before = StartCfg{2, -1}; u.after = StartCfg{4, -1};
        u.hasI = u.hasD = true; u.result = L"ok"; u.afterObs = StartCfg{4, -1}; log.rows = {u};
        FakeBackend fb2; fb2.SetItems({Svc(L"aaa", 4, 1)});
        Plan pu = BuildUndo(log, fb2, c);
        Store st2(TempDir(L"p4u")); st2.fixedStamp = L"20261111-000001";
        ApplyResult r2; RunApply(pu, Env(&fb2, &st2), &r2); if (r2.aborted) printf("    APPLY ABORTED: %ls\n", r2.error.c_str());
        printf("    undo: after apply %ls (before Hadron: 2/-)\n", CfgText(CfgOf(fb2, L"aaa")).c_str());
        Check(CfgOf(fb2, L"aaa").SameEffect(StartCfg{2, -1}), "undo restores the pre-Hadron effect (Auto, not Auto delayed)");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P5 Undo can write a now-Boot-start driver\n");
    {
        UndoLog log; log.stamp = L"S1"; log.hasB = log.hasE = true; log.source = L"preset:stripped:drivers"; log.machine = L"m"; log.build = L"19045";
        UndoRow u; u.seq = 1; u.name = L"drvx"; u.before = StartCfg{3, -1}; u.after = StartCfg{4, -1};
        u.hasI = u.hasD = true; u.result = L"ok"; u.afterObs = StartCfg{4, -1}; log.rows = {u};
        FakeBackend fb; fb.SetItems({Svc(L"drvx", 0, -1, {}, 0x1)});   // Windows made it Boot-start since
        Ctx c = MakeCtx(nullptr, &hw);
        Plan p = BuildUndo(log, fb, c);
        const PlanRow* a = Row(p, L"drvx");
        printf("    row: %ls -> %ls blocked='%ls' canToggle=%d include=%d warn='%ls'\n", CfgText(a->before).c_str(),
               CfgText(a->after).c_str(), a->blocked.c_str(), a->canToggle, a->include, a->warning.c_str());
        Check(!a->blocked.empty() || !a->canToggle, "a Boot-start driver is blocked (not toggleable) in an Undo plan");
        // Also a never-touch (ErrorControl Critical) item
        FakeBackend fb2; SvcItem crit = Svc(L"drvx", 4, -1, {}, 0x1); crit.errctl = 3; fb2.SetItems({crit});
        Plan p2 = BuildUndo(log, fb2, c);
        const PlanRow* b = Row(p2, L"drvx");
        Check(b && !b->blocked.empty(), "a never-touch item (ErrorControl Critical) is blocked in an Undo plan");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P6 Resume orphans the interrupted apply's finished rows from Undo\n");
    {
        UndoLog l1; l1.stamp = L"20261111-100000"; l1.hasB = true; l1.hasE = false; l1.source = L"preset:bloat:services";
        UndoRow a; a.seq = 1; a.name = L"aaa"; a.before = {3, -1}; a.after = {4, -1}; a.hasI = a.hasD = true; a.result = L"ok"; a.afterObs = {4, -1};
        UndoRow b; b.seq = 2; b.name = L"bbb"; b.before = {3, -1}; b.after = {4, -1};
        l1.rows = {a, b};
        UndoLog l2; l2.stamp = L"20261111-100500"; l2.hasB = l2.hasE = true; l2.source = L"resume:" + l1.stamp;
        UndoRow b2 = b; b2.seq = 1; b2.hasI = b2.hasD = true; b2.result = L"ok"; b2.afterObs = {4, -1};
        l2.rows = {b2}; l1.order = 1; l2.order = 2;
        std::vector<UndoLog> logs = {l1, l2};
        int ui = FindUndoable(logs), ii = FindInterrupted(logs);
        auto la = LastApplied(logs);
        printf("    FindInterrupted=%d FindUndoable=%d (%ls) LastApplied has aaa: %d\n", ii, ui, ui >= 0 ? logs[ui].stamp.c_str() : L"-",
               (int)la.count(L"aaa"));
        bool aaaReachable = false;
        if (ui >= 0) for (auto& r : logs[ui].rows) if (r.name == L"aaa") aaaReachable = true;
        Check(aaaReachable, "after Resume, Undo still covers aaa (written by the interrupted apply)");
        Check(la.count(L"aaa") == 1, "drift tracking still covers aaa after Resume");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P7 Log ordering by stamp text\n");
    {
        UndoLog l1; l1.stamp = L"20261027-014500"; l1.hasB = true; l1.source = L"preset:bloat:services";   // before DST fall-back
        UndoLog l2; l2.stamp = L"20261027-011000"; l2.hasB = l2.hasE = true; l2.source = L"resume:20261027-014500";   // after it
        std::vector<UndoLog> logs = {l2, l1};   // worst case: the resume sorted first
        Check(FindInterrupted(logs) < 0, "a finished resume resolves its interrupted log whatever the stamps' order");
        {   // LoadLogs ordering of -2 / -10 through a real store
            Store st(TempDir(L"p7"));
            auto mk = [&](const wchar_t* stamp, int order) {
                std::string t = "HXSVC\t1\tundo\n" + Record({L"B", stamp, L"preset:bloat:services", L"0", L"m", L"19045", std::to_wstring(order)}) +
                                Record({L"E", stamp, L"0", L"0", L"0", L"done"});
                std::wstring e; st.WriteNew(std::wstring(L"undo-") + stamp + L".log", t, &e);
            };
            mk(L"20261111-100000-10", 0); mk(L"20261111-100000-2", 0);
            auto lg = LoadLogs(st, nullptr);
            Check(lg.size() == 2 && lg[0].stamp == L"20261111-100000-2", "LoadLogs: stamp -2 sorts before -10");
        }
    }

    // ---------------------------------------------------------------------------------------------
    printf("P8 Presets only ever lower a start type\n");
    {
        Kb kb; std::wstring err;
        kb.Parse("V\t1\tservices\nB\t19045\n" + K("aaa", "svc", "", "", "3"), &err);
        std::vector<SvcItem> items = {Svc(L"aaa", 4)};   // the user had disabled it
        Ctx c = MakeCtx(&kb, &hw);
        Plan p = BuildPreset(LV_MINIMUM, TAB_SERVICES, items, c);
        const PlanRow* a = Row(p, L"aaa");
        if (a) printf("    row: %ls -> %ls include=%d\n", CfgText(a->before).c_str(), CfgText(a->after).c_str(), a->include);
        Check(!a || a->after.start >= a->before.start, "Minimum never raises Disabled -> Manual");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P9 Level nesting over the real KB + fixture (rows included or alreadySet)\n");
    {
        Kb kb; std::wstring err;
        LoadEmbeddedKb(&kb, &err);
        FakeBackend fxb; if (!fxb.Load(argc > 1 ? argv[1] : L"Z:\\home\\user\\Hadron\\tests\\fixtures\\services", &err)) printf("fixture: %ls\n", err.c_str());
        std::vector<SvcItem> items; fxb.Enumerate(&items, &err); FoldInstances(&items);
        HwInfo h; fxb.ReadHw(&h);
        Ctx c = MakeCtx(&kb, h.ok ? &h : nullptr);
        bool nested = true, bootNever = true, luid = true, sysok = true;
        for (int tab : {0, 1}) {
            Plan pl[3];
            for (int lv = 0; lv < 3; lv++) pl[lv] = BuildPreset(lv, tab, items, c);
            for (int lv = 1; lv < 3; lv++)
                for (auto& r : pl[lv - 1].rows) {
                    const PlanRow* up = Row(pl[lv], r.name.c_str());
                    if (!up) { nested = false; printf("    %ls in level %d, not %d\n", r.name.c_str(), lv - 1, lv); }
                }
            for (int lv = 0; lv < 3; lv++)
                for (auto& r : pl[lv].rows) {
                    const SvcItem* s = nullptr;
                    for (auto& i : items) if (IEq(i.name, r.name)) s = &i;
                    if (r.blocked.empty() && s && IsDriverType(s->type) && s->cfg.start == 0) bootNever = false;
                    if (r.blocked.empty() && s && s->kind == Kind::UserInstance) luid = false;
                    const KbEntry* e = kb.Find(r.name);
                    if (r.blocked.empty() && r.driver && (!e || !e->sysok)) sysok = false;
                }
            printf("    tab %d: rows %zu / %zu / %zu\n", tab, pl[0].rows.size(), pl[1].rows.size(), pl[2].rows.size());
        }
        Check(nested, "every row of a level is also a row of the next level");
        Check(bootNever, "no unblocked Boot-start driver in any preset plan");
        Check(luid, "no unblocked per-user instance in any preset plan");
        Check(sysok, "no unblocked driver without sysok in any preset plan");


        {
            int changed = 0;
            for (int tab : {0, 1}) for (int lv = 0; lv < 3; lv++) {
                Plan a = BuildPreset(lv, tab, items, c);
                Plan b = a;
                DependencyCheck(&b, items);
                for (size_t i = 0; i < a.rows.size(); i++) if (a.rows[i].include != b.rows[i].include) { changed++; printf("    fixture: second pass unticks %ls (level %d)\n", a.rows[i].name.c_str(), lv); }
            }
            Check(changed == 0, "fixture + real KB: DependencyCheck is already a fixed point (no hidden chain today)");
            // also: with the user ticking every optional row (what TickUndetectable does), is it still consistent?
            int bad = 0;
            for (int lv = 0; lv < 3; lv++) {
                Plan a = BuildPreset(lv, 0, items, c);
                for (auto& r : a.rows) if (r.canToggle && r.blocked.empty() && r.warning.empty() && r.depWarning.empty()) r.include = true;
                DependencyCheck(&a, items);
                FakeBackend fb2 = fxb;
                Store st(TempDir(L"p9")); st.fixedStamp = L"20261111-000000";
                ApplyResult ar; RunApply(a, Env(&fb2, &st), &ar); if (ar.aborted) printf("    APPLY ABORTED: %ls\n", ar.error.c_str());
                for (auto& x : fb2.Items()) {
                    if (x.cfg.start == 4 || KindOf(x.type) == Kind::UserInstance) continue;   // instances follow their template
                    for (auto& d : x.deps) for (auto& y : fb2.Items()) if (IEq(y.name, d) && y.cfg.start == 4) {
                        bool was = false; for (auto& z : items) if (IEq(z.name, y.name) && z.cfg.start == 4) was = true;
                        if (!was) { bad++; printf("    level %d: %ls (%d) depends on %ls, now Disabled\n", lv, x.name.c_str(), x.cfg.start, y.name.c_str()); }
                    }
                }
            }
            Check(bad == 0, "fixture: ticking every optional row without a warning never leaves an enabled dependent of a newly disabled item");
        }
        // ---- performance (Wine) ----
        printf("P10 timings under Wine (synthetic x5 fixture, %zu items)\n", items.size() * 5);
        std::vector<SvcItem> big;
        for (int k = 0; k < 5; k++)
            for (auto s : items) { if (k) s.name += L"_x" + std::to_wstring(k); big.push_back(s); }
        auto t0 = std::chrono::steady_clock::now();
        auto ms = [&](std::chrono::steady_clock::time_point a) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        };
        for (int lv = 0; lv < 3; lv++) BuildPreset(lv, 0, big, c);
        printf("    3x BuildPreset(services): %.2f ms\n", ms(t0));
        t0 = std::chrono::steady_clock::now();
        int n = 0;
        for (auto& s : big) { CheckGuard(s, c); n++; }
        printf("    CheckGuard x%d: %.2f ms\n", n, ms(t0));
        // Refilter's sort by the Hadron column: CheckGuard + KB lookups inside the comparator
        std::vector<int> V; for (int i = 0; i < (int)big.size(); i++) V.push_back(i);
        int calls = 0;
        auto kbcol = [&](const SvcItem& s) -> std::wstring {
            calls++;
            Guard g = CheckGuard(s, c);
            if (g.neverTouch) return L"protected";
            const KbEntry* e = kb.Find(s.name);
            if (g.neverPreset) return e ? L"no preset" : L"-";
            if (!e) return L"-";
            return std::to_wstring(e->JoinLevel());
        };
        t0 = std::chrono::steady_clock::now();
        std::stable_sort(V.begin(), V.end(), [&](int a, int b) {
            std::wstring ka = kbcol(big[a]), kb2 = kbcol(big[b]);
            if (ka == kb2) return Lower(big[a].name) < Lower(big[b].name);
            return ka < kb2;
        });
        printf("    sort by Hadron column (as Refilter): %.2f ms, %d CheckGuard calls\n", ms(t0), calls);
        t0 = std::chrono::steady_clock::now();
        std::vector<std::wstring> keys; for (auto& s : big) keys.push_back(kbcol(s));
        std::stable_sort(V.begin(), V.end(), [&](int a, int b) { return keys[a] < keys[b]; });
        printf("    same with keys computed once: %.2f ms\n", ms(t0));
        t0 = std::chrono::steady_clock::now();
        std::stable_sort(V.begin(), V.end(), [&](int a, int b) { return Lower(big[a].name) < Lower(big[b].name); });
        printf("    sort by name (Lower per comparison): %.2f ms\n", ms(t0));
        t0 = std::chrono::steady_clock::now();
        int hits = 0;
        for (auto& s : big) if (IContains(s.name, L"svc") || IContains(s.display, L"svc") || IContains(s.company, L"svc") ||
                                IContains(s.group, L"svc") || IContains(s.account, L"svc")) hits++;
        printf("    filter pass (5 IContains per item): %.2f ms\n", ms(t0));
        // DependencyCheck on a plan of every disable row
        Plan dp; for (auto& s : big) { PlanRow r; r.name = s.name; r.before = s.cfg; r.after = {4, -1}; r.deps = s.deps; dp.rows.push_back(r); }
        t0 = std::chrono::steady_clock::now();
        DependencyCheck(&dp, big);
        printf("    DependencyCheck, %zu disable rows x %zu items: %.2f ms\n", dp.rows.size(), big.size(), ms(t0));

        {   // toggle cost on a realistic plan: Minimum services over the x5 item list (UI thread, every Space/click)
            Plan pm = BuildPreset(LV_MINIMUM, 0, big, c);
            t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < 10; k++) { int i = k % (int)pm.rows.size(); pm.rows[i].include = !pm.rows[i].include; DependencyCheck(&pm, big, i); }
            printf("    toggle (flip + DependencyCheck) on a %zu-row Minimum plan, %zu items: %.2f ms each\n", pm.rows.size(), big.size(), ms(t0) / 10);
            Plan pm1 = BuildPreset(LV_MINIMUM, 0, items, c);
            t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < 10; k++) { int i = k % (int)pm1.rows.size(); pm1.rows[i].include = !pm1.rows[i].include; DependencyCheck(&pm1, items, i); }

            printf("    same on the fixture (%zu rows, %zu items): %.2f ms each\n", pm1.rows.size(), items.size(), ms(t0) / 10);
            {   // worst case: the same plan plus a non-converging pair (raise Y needing X; disable X) -> the loop runs to its bound
                std::vector<SvcItem> b2 = big; b2.push_back(Svc(L"oscx", 3)); b2.push_back(Svc(L"oscy", 4, -1, {L"oscx"}));
                Plan po = BuildPreset(LV_MINIMUM, 0, big, c);
                PlanRow q; q.name = L"oscx"; q.before = {3, -1}; q.after = {4, -1}; q.op = {4, -1};
                PlanRow r; r.name = L"oscy"; r.before = {4, -1}; r.after = {3, -1}; r.op = {3, -1}; r.deps = {L"oscx"};
                po.rows.push_back(q); po.rows.push_back(r);
                t0 = std::chrono::steady_clock::now();
                DependencyCheck(&po, b2);
                printf("    DependencyCheck on a %zu-row plan with a non-converging pair, %zu items: %.1f ms\n", po.rows.size(), b2.size(), ms(t0));
            }
        }
        t0 = std::chrono::steady_clock::now();
        auto ord = ApplyOrder(dp);
        printf("    ApplyOrder %zu rows: %.2f ms\n", ord.size(), ms(t0));

        // HwEvidence with an owner-sized devnode list (present + phantom), as BuildPreset and every detail paint call it
        {
            HwInfo hb = h;
            for (int k = 0; k < 3000; k++) {
                DevNode n; n.id = L"PCI\\VEN_8086&DEV_" + std::to_wstring(k) + L"\\3&11583659&0&" + std::to_wstring(k);
                n.service = L"svc" + std::to_wstring(k % 300); n.cls = k % 7 ? L"System" : L"HIDClass"; n.present = k % 3;
                n.upper = {L"filt" + std::to_wstring(k % 11)};
                hb.nodes.push_back(n);
            }
            std::wstring ev;
            t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < 100; k++) HwEvidence(hb, L"class:Bluetooth", L"BTHUSB", &ev);
            printf("    HwEvidence over %zu devnodes (class hint, miss): %.3f ms per call\n", hb.nodes.size(), ms(t0) / 100);
            t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < 100; k++) HwEvidence(hb, L"self", L"IntcDAud", &ev);
            printf("    HwEvidence over %zu devnodes (self hint, miss): %.3f ms per call\n", hb.nodes.size(), ms(t0) / 100);
            Ctx cb = MakeCtx(&kb, &hb);
            t0 = std::chrono::steady_clock::now();
            for (int lv = 0; lv < 3; lv++) BuildPreset(lv, 0, items, cb), BuildPreset(lv, 1, items, cb);
            printf("    6x BuildPreset with 3000 devnodes: %.2f ms\n", ms(t0));
        }
        // Live backend, read-only, inside this Wine prefix only
        {
            auto lb = MakeLiveBackend();
            std::vector<SvcItem> live;
            t0 = std::chrono::steady_clock::now();
            bool ok = lb->Enumerate(&live, &err);
            double te = ms(t0);
            t0 = std::chrono::steady_clock::now();
            for (auto& i : live) i.company = FileCompany(i);
            printf("    Wine live Enumerate: ok=%d %zu items, %.1f ms (%.3f ms/item); FileCompany %.1f ms (%.3f ms/item)\n", ok, live.size(), te,
                   live.empty() ? 0 : te / live.size(), ms(t0), live.empty() ? 0 : ms(t0) / live.size());
        }
        // ItemDoc's "Needed by" scan for one item
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 100; k++) {
            std::vector<std::wstring> dependents;
            for (auto& o : big) for (auto& dd : o.deps) if (IEq(dd, L"RpcSs")) dependents.push_back(o.name);
        }
        printf("    'Needed by' scan per paint: %.3f ms\n", ms(t0) / 100);
    }


    // =============================== round 2 ===============================
    auto consistent = [](const Plan& p, const std::vector<SvcItem>& items, std::wstring* why) {
        // end state if the ticked rows are applied: no enabled item depends on a Disabled one that the plan disables,
        // and no raised row depends on something Disabled at the end
        std::map<std::wstring, int> fin;
        for (auto& s : items) fin[Lower(s.name)] = s.cfg.start;
        for (auto& r : p.rows) if (r.blocked.empty() && r.include) fin[Lower(r.name)] = r.after.start;
        for (auto& s : items) {
            int e = fin[Lower(s.name)];
            if (e == 4) continue;
            for (auto& d : s.deps) {
                auto it = fin.find(Lower(d));
                if (it == fin.end() || it->second != 4) continue;
                bool planned = false;   // only conflicts the plan creates or a raised row creates
                for (auto& r : p.rows) if (r.blocked.empty() && r.include && (IEq(r.name, d) || IEq(r.name, s.name))) planned = true;
                if (planned) { *why = s.name + L" -> " + d; return false; }
            }
        }
        return true;
    };
    printf("R1 DependencyCheck: oscillation and cycles terminate in a consistent state\n");
    {
        // P raises Y (4->3) where Y depends on X; Q disables X (3->4). Each row's warning exists only while the other is ticked.
        std::vector<SvcItem> items = {Svc(L"xxx", 3), Svc(L"yyy", 4, -1, {L"xxx"})};
        Plan p; p.kind = PlanKind::Preset;
        PlanRow q; q.name = L"xxx"; q.before = {3, -1}; q.after = {4, -1}; q.op = {4, -1};
        PlanRow r; r.name = L"yyy"; r.before = {4, -1}; r.after = {3, -1}; r.op = {3, -1}; r.deps = {L"xxx"};
        p.rows = {q, r};
        DependencyCheck(&p, items);
        std::wstring why;
        printf("    xxx include=%d dep='%ls'\n    yyy include=%d dep='%ls'\n", p.rows[0].include, p.rows[0].depWarning.c_str(),
               p.rows[1].include, p.rows[1].depWarning.c_str());
        Check(consistent(p, items, &why), "oscillating pair ends without a conflict");
        bool warnedTicked = false;
        for (auto& x : p.rows) if (x.include && !x.depWarning.empty()) warnedTicked = true;
        Check(!warnedTicked, "oscillating pair: no ticked row carries a dependency warning the user didn't override");
        // three rows: plus an unrelated ticked row, to change the round count parity
        std::vector<SvcItem> items3 = items; items3.push_back(Svc(L"zzz", 3));
        Plan p3 = p; p3.rows[0].include = p3.rows[1].include = true; p3.rows[0].depWarning.clear(); p3.rows[1].depWarning.clear();
        p3.rows[0].depUnticked = p3.rows[1].depUnticked = false;
        PlanRow z; z.name = L"zzz"; z.before = {3, -1}; z.after = {4, -1}; z.op = {4, -1}; p3.rows.push_back(z);
        DependencyCheck(&p3, items3);
        printf("    with a 3rd row: xxx=%d yyy=%d\n", p3.rows[0].include, p3.rows[1].include);
        Check(consistent(p3, items3, &why), "oscillating pair + unrelated row ends without a conflict");
        // a true dependency cycle, all disabled, with an outside dependent
        std::vector<SvcItem> cyc = {Svc(L"c1", 3, -1, {L"c2"}), Svc(L"c2", 3, -1, {L"c1"}), Svc(L"out", 3, -1, {L"c1"})};
        Plan pc; pc.kind = PlanKind::Preset;
        for (auto* n : {L"c1", L"c2"}) { PlanRow x; x.name = n; x.before = {3, -1}; x.after = {4, -1}; x.op = {4, -1};
            x.deps = {std::wstring(n) == L"c1" ? L"c2" : L"c1"}; pc.rows.push_back(x); }
        DependencyCheck(&pc, cyc);
        printf("    cycle: c1=%d c2=%d\n", pc.rows[0].include, pc.rows[1].include);
        Check(consistent(pc, cyc, &why), "dependency cycle with an outside dependent ends without a conflict");
        auto ord = ApplyOrder(pc);
        Check(true, "ApplyOrder terminates on a cycle");
    }
    printf("R2 Toggle sequence (UI path: flip + DependencyCheck(row))\n");
    {
        Kb kb; std::wstring err;
        kb.Parse("V\t1\tservices\nB\t19045\n" + K("aaa", "svc", "4", "", "") + K("bbb", "svc", "4", "", "") + K("ccc", "svc", "4", "", ""), &err);
        std::vector<SvcItem> items = {Svc(L"aaa", 3), Svc(L"bbb", 3, -1, {L"aaa"}), Svc(L"ccc", 3, -1, {L"bbb"})};
        Ctx c = MakeCtx(&kb, &hw);
        Plan p = BuildPreset(LV_BLOAT, TAB_SERVICES, items, c);
        auto idx = [&](const wchar_t* n) { for (size_t k = 0; k < p.rows.size(); k++) if (p.rows[k].name == n) return (int)k; return -1; };
        auto tog = [&](const wchar_t* n) { int k = idx(n); p.rows[k].include = !p.rows[k].include; DependencyCheck(&p, items, k); };
        std::wstring why; bool ok = true;
        const wchar_t* seq[] = {L"ccc", L"bbb", L"aaa", L"ccc", L"bbb", L"ccc", L"aaa", L"bbb", L"ccc", L"aaa"};
        for (auto* n : seq) {
            tog(n);
            bool userOverride = false;
            for (auto& x : p.rows) if (x.include && !x.depWarning.empty()) userOverride = true;
            if (!userOverride && !consistent(p, items, &why)) { ok = false; printf("    after toggling %ls: conflict %ls\n", n, why.c_str()); }
        }
        Check(ok, "every toggle leaves either a consistent plan or a visible warning on a ticked (user-chosen) row");
        // stale-warning check: after any toggle, depWarning equals what a fresh evaluation says
        Plan q = p; for (auto& x : q.rows) { x.depWarning.clear(); x.depUnticked = false; }
        Plan q2 = q; DependencyCheck(&q2, items, -2);
        bool same = true;
        for (size_t k = 0; k < p.rows.size(); k++) if (p.rows[k].include && p.rows[k].depWarning != q2.rows[k].depWarning) same = false;
        Check(same, "warnings on ticked rows are current after a toggle sequence");
    }
    printf("R3 MarkTi after DependencyCheck: a TI-excluded dependent\n");
    {
        Kb kb; std::wstring err;
        kb.Parse("V\t1\tservices\nB\t19045\n" + K("aaa", "svc", "4", "", "") + K("bbb", "svc", "4", "", ""), &err);
        std::vector<SvcItem> items = {Svc(L"aaa", 3), Svc(L"bbb", 3, -1, {L"aaa"})};
        Ctx c = MakeCtx(&kb, &hw);
        Plan p = BuildPreset(LV_BLOAT, TAB_SERVICES, items, c);
        FakeBackend fb; fb.SetItems(items); fb.faults.deny.insert(L"bbb");   // admin write refused -> needs TI, toggle off
        MarkTi(&p, fb, false);   // what BuildFrom does last
        const PlanRow* a = Row(p, L"aaa"); const PlanRow* b = Row(p, L"bbb");
        printf("    aaa include=%d dep='%ls'; bbb include=%d canToggle=%d\n", a->include, a->depWarning.c_str(), b->include, b->canToggle);
        std::wstring why;
        Check(consistent(p, items, &why), "a TI-excluded dependent unticks what it depends on");
    }
    printf("R4 First-run recovery rows (fixture first.txt, real KB)\n");
    {
        Kb kb; std::wstring err; LoadEmbeddedKb(&kb, &err);
        FakeBackend fxb; fxb.Load(L"Z:\\home\\user\\Hadron\\tests\\fixtures\\services", &err);
        HwInfo h; fxb.ReadHw(&h);
        Store fs(L"Z:\\home\\user\\Hadron\\tests\\fixtures\\services"); std::string t; Snapshot first;
        fs.Read(L"first.txt", &t, &err); ParseSnapshot(t, &first, &err);
        Ctx c = MakeCtx(&kb, h.ok ? &h : nullptr);
        std::vector<std::wstring> skipped;
        auto rows = FirstRestoreRows(first, c, &skipped);
        bool bad = false;
        for (auto& r : rows) if (r.cfg.start < 2 || IsStorageDriver(r.name)) { bad = true; printf("    kept %ls %d\n", r.name.c_str(), r.cfg.start); }
        printf("    %zu rows written, %zu skipped (of %zu)\n", rows.size(), skipped.size(), first.items.size());
        Check(!bad, "no Boot/System value and no storage driver in the first-run recovery script");
        Ctx nohw = MakeCtx(&kb, nullptr);
        auto rows2 = FirstRestoreRows(first, nohw, nullptr);
        bool drv = false; for (auto& r : rows2) for (auto& i : first.items) if (IEq(i.name, r.name) && IsDriverType(i.type)) drv = true;
        Check(!drv, "without hardware info the first-run script carries no driver");
    }
    printf("R5 Undo: a stale delayed flag in the log's before value; resume ordering\n");
    {
        // M3 still: resume then undo coverage (re-run of P6 logic is above)
        UndoLog l1; l1.stamp = L"S1"; l1.hasB = true; l1.machine = L"m"; l1.build = L"19045"; l1.order = 1; l1.source = L"preset:bloat:services";
        UndoLog u; u.stamp = L"S2"; u.hasB = true; u.machine = L"m"; u.build = L"19045"; u.order = 2; u.source = L"undo:S1";   // interrupted undo
        std::vector<UndoLog> logs = {l1, u};
        int i = FindInterrupted(logs);
        printf("    interrupted original + interrupted undo of it: FindInterrupted -> %ls\n", i >= 0 ? logs[i].stamp.c_str() : L"none");
        Check(i >= 0, "an interrupted undo of an interrupted apply still leaves something offered");
    }
    printf("probe: %d checks, %d broken\n", checks, fails);
    return 0;
}
