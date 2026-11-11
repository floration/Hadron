// Property / counterexample probes for the services plan engine. Scratch only; fake backend only.
#include "modules/services/plan.h"
#include "modules/services/backend.h"
#include <chrono>
#include <cstdio>
#include <functional>

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
    HwInfo hw; hw.ok = true;

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
        ApplyResult r; RunApply(p, Env(&fb, &st), &r);
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
        for (auto& r : p.rows) if (r.name == L"bbb" && r.canToggle) r.include = false;   // TogglePlanRow does only this
        FakeBackend fb; fb.SetItems(items);
        Store st(TempDir(L"p2")); st.fixedStamp = L"20261111-000000";
        ApplyResult r; RunApply(p, Env(&fb, &st), &r);
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
        ApplyResult r; RunApply(p, Env(&fb, &st), &r);
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
        ApplyResult r; RunApply(p, Env(&fb, &st), &r);
        StartCfg now = CfgOf(fb, L"aaa");
        printf("    after apply: %ls (%ls); first.txt says 2/0 (Auto); result line: %ls\n", CfgText(now).c_str(), StartLabel(now, false).c_str(),
               r.lines.empty() ? L"" : r.lines[0].c_str());
        Check(now.SameEffect(first.items[0].cfg), "restore-first-snapshot gives the recorded effect (Auto, not Auto delayed)");

        // same for undo: Hadron set 2/- -> 4; something then set DelayedAutostart=1 while disabled
        UndoLog log; log.stamp = L"S1"; log.hasB = log.hasE = true; log.source = L"preset:bloat:services";
        UndoRow u; u.seq = 1; u.name = L"aaa"; u.before = StartCfg{2, -1}; u.after = StartCfg{4, -1};
        u.hasI = u.hasD = true; u.result = L"ok"; u.afterObs = StartCfg{4, -1}; log.rows = {u};
        FakeBackend fb2; fb2.SetItems({Svc(L"aaa", 4, 1)});
        Plan pu = BuildUndo(log, fb2, c);
        Store st2(TempDir(L"p4u")); st2.fixedStamp = L"20261111-000001";
        ApplyResult r2; RunApply(pu, Env(&fb2, &st2), &r2);
        printf("    undo: after apply %ls (before Hadron: 2/-)\n", CfgText(CfgOf(fb2, L"aaa")).c_str());
        Check(CfgOf(fb2, L"aaa").SameEffect(StartCfg{2, -1}), "undo restores the pre-Hadron effect (Auto, not Auto delayed)");
    }

    // ---------------------------------------------------------------------------------------------
    printf("P5 Undo can write a now-Boot-start driver\n");
    {
        UndoLog log; log.stamp = L"S1"; log.hasB = log.hasE = true; log.source = L"preset:stripped:drivers";
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
        l2.rows = {b2};
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
        std::vector<UndoLog> logs = {l1, l2};
        std::sort(logs.begin(), logs.end(), [](const UndoLog& a, const UndoLog& b) { return a.stamp < b.stamp; });
        Check(FindInterrupted(logs) < 0, "a finished resume resolves its interrupted log whatever the stamps' order");
        std::vector<std::wstring> s = {L"20261111-100000-2", L"20261111-100000-10"};
        std::sort(s.begin(), s.end());
        Check(s[0] == L"20261111-100000-2", "stamp -2 sorts before -10");
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
                for (auto& r : a.rows) if (r.canToggle && r.blocked.empty() && r.warning.empty()) r.include = true;
                FakeBackend fb2 = fxb;
                Store st(TempDir(L"p9")); st.fixedStamp = L"20261111-000000";
                ApplyResult ar; RunApply(a, Env(&fb2, &st), &ar);
                for (auto& x : fb2.Items()) {
                    if (x.cfg.start == 4) continue;
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

    printf("probe: %d checks, %d broken\n", checks, fails);
    return 0;
}
