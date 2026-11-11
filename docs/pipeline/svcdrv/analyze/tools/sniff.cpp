// sniffer harness (scratch copy only): adversarial apply/undo/recovery tests on the fake backend.
#include "modules/services/cli.h"
#include "modules/services/plan.h"
#include "modules/services/recovery.h"
#include "core/sys/sys.h"
#include <cstdio>
#include <random>
#include <thread>

namespace hx::services {

static int g_fail = 0;
#define CHK(c, ...) do { if (!(c)) { g_fail++; wprintf(L"FAIL " __VA_ARGS__); wprintf(L"\n"); } } while (0)

static std::wstring TmpDir(const std::wstring& tag) {
    wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
    for (int i = 0;; i++) {
        std::wstring d = std::wstring(t) + L"sniff-" + std::to_wstring(GetCurrentProcessId()) + L"-" + tag + L"-" + std::to_wstring(i);
        if (CreateDirectoryW(d.c_str(), nullptr)) return d;
    }
}

static SvcItem Svc(const std::wstring& n, StartCfg c, DWORD type = 0x10) {
    SvcItem s; s.name = n; s.display = n; s.type = type; s.cfg = c; s.state = 1; s.kind = KindOf(type); s.regStart = c.start; return s;
}

// A backend wrapper that flips cancel after N SetStart calls, or that changes another service under our feet.
struct Hook : FakeBackend {
    std::atomic<bool>* cancel = nullptr; int cancelAfter = -1;
    bool SetStart(const std::wstring& n, const WriteOp& w, Identity id, std::wstring* e) override {
        bool ok = FakeBackend::SetStart(n, w, id, e);
        if (cancel && faults.setStartCalls == cancelAfter) cancel->store(true);
        return ok;
    }
};

static ApplyEnv Env(Backend* b, Store* s) {
    ApplyEnv e; e.backend = b; e.store = s; e.lockName = L"Local\\sniff." + std::to_wstring(GetCurrentProcessId());
    e.build = L"19045"; e.machine = L"m"; return e;
}

static std::map<std::wstring, StartCfg> Cfgs(FakeBackend& b) { std::map<std::wstring, StartCfg> m; for (auto& i : b.Items()) m[i.name] = i.cfg; return m; }
static std::wstring CfgsText(const std::map<std::wstring, StartCfg>& m) { std::wstring o; for (auto& kv : m) o += kv.first + L"=" + CfgText(kv.second) + L" "; return o; }

// 1. Default-first round trips over every (current, first) combination of start 2..4 x delayed -1/0/1.
static void DefaultRoundTrip() {
    const int starts[] = {2, 3, 4}, dl[] = {-1, 0, 1};
    int n = 0, wrong = 0, undoWrong = 0;
    for (int a : starts) for (int ad : dl) for (int f : starts) for (int fd : dl) {
        StartCfg cur{a, ad}, fst{f, fd};
        Hook b; b.SetItems({Svc(L"S1", cur)});
        Snapshot first; first.meta[L"machine"] = L"m"; first.items = {Svc(L"S1", fst)};
        Ctx c; c.build = L"19045"; c.machine = L"m"; c.first = &first;
        std::vector<SvcItem> items = b.Items();
        Plan p = BuildDefault(false, TAB_SERVICES, items, c);
        Store st(TmpDir(L"rt"));
        ApplyResult r;
        if (p.Included()) RunApply(p, Env(&b, &st), &r);
        StartCfg got = b.Items()[0].cfg;
        n++;
        if (!got.SameEffect(fst)) { wrong++; wprintf(L"  default-first: current %ls first %ls -> wrote %ls (plan after %ls)\n", CfgText(cur).c_str(), CfgText(fst).c_str(), CfgText(got).c_str(), p.rows.empty() ? L"-" : CfgText(p.rows[0].after).c_str()); }
        if (!p.Included()) continue;
        auto logs = LoadLogs(st, nullptr);
        int k = FindUndoable(logs);
        if (k < 0) continue;
        Plan u = BuildUndo(logs[k], b, c);
        RunApply(u, Env(&b, &st), &r);
        StartCfg back = b.Items()[0].cfg;
        if (!back.SameEffect(cur)) { undoWrong++; wprintf(L"  undo: original %ls -> after undo %ls\n", CfgText(cur).c_str(), CfgText(back).c_str()); }
    }
    wprintf(L"default-first round trips: %d cases, %d wrote a different effect than first.txt, %d undo mismatches\n", n, wrong, undoWrong);
}

// 2. Single changes across all combos then undo: exact restore?
static void SingleRoundTrip() {
    const int starts[] = {2, 3, 4}, dl[] = {-1, 0, 1};
    int bad = 0, n = 0;
    for (int a : starts) for (int ad : dl) for (int t : starts) for (int td : {0, 1}) {
        StartCfg cur{a, ad}; StartCfg tgt{t, t == 2 ? td : -1};
        Hook b; b.SetItems({Svc(L"S1", cur)});
        Ctx c; c.build = L"19045";
        std::vector<SvcItem> items = b.Items();
        Plan p = BuildSingle(items[0], tgt, items, c);
        if (!p.Included()) continue;
        Store st(TmpDir(L"single"));
        ApplyResult r; RunApply(p, Env(&b, &st), &r);
        StartCfg got = b.Items()[0].cfg;
        n++;
        if (!(got.start == tgt.start && (t != 2 || got.Delayed() == (td == 1)))) { bad++; wprintf(L"  single: %ls -> target %ls wrote %ls\n", CfgText(cur).c_str(), CfgText(tgt).c_str(), CfgText(got).c_str()); }
        auto logs = LoadLogs(st, nullptr);
        Plan u = BuildUndo(logs[FindUndoable(logs)], b, c);
        RunApply(u, Env(&b, &st), &r);
        if (!b.Items()[0].cfg.SameEffect(cur)) { bad++; wprintf(L"  single undo: %ls -> %ls -> undo %ls\n", CfgText(cur).c_str(), CfgText(got).c_str(), CfgText(b.Items()[0].cfg).c_str()); }
    }
    wprintf(L"single round trips: %d, %d bad\n", n, bad);
}

static Plan ManyPlan(Hook& b, int count, std::vector<StartCfg>* orig) {
    std::vector<SvcItem> v;
    std::mt19937 rng(7);
    for (int i = 0; i < count; i++) {
        StartCfg c{2 + (int)(rng() % 2), (int)(rng() % 3) - 1};
        orig->push_back(c);
        SvcItem s = Svc(L"Svc" + std::to_wstring(i), c);
        if (i > 0 && i % 3 == 0) s.deps = {L"Svc" + std::to_wstring(i - 1)};
        v.push_back(s);
    }
    b.SetItems(v);
    Ctx c; c.build = L"19045";
    // Build a plan by hand: every item -> Disabled (like a preset).
    Plan p; p.source = L"preset:bloat:services"; p.title = L"many";
    for (auto& s : v) { PlanRow r; r.name = s.name; r.before = s.cfg; r.after = StartCfg{4, s.cfg.delayed}; r.op = WriteOp{4, -1}; r.deps = s.deps; p.rows.push_back(r); }
    return p;
}

// 3. Crash after every seq, with and without a torn tail; then Undo from the interrupted log must restore everything.
static void CrashUndo() {
    const int N = 12;
    int bad = 0;
    for (int crash = 1; crash <= N; crash++) for (int torn = 0; torn < 3; torn++) {
        Hook b; std::vector<StartCfg> orig; Plan p = ManyPlan(b, N, &orig);
        Store st(TmpDir(L"crash"));
        ApplyEnv e = Env(&b, &st); e.crashAfterBegin = crash;
        ApplyResult r; RunApply(p, e, &r);
        if (torn) {   // power loss mid-record: append half of a D record (no LF), or garbage
            HANDLE h = CreateFileW(r.logPath.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            std::string junk = torn == 1 ? std::string("D\t") + std::to_string(crash) + "\tok\t4/-\tadm" : std::string("\0\0\0\0\0\0", 6);
            DWORD w; WriteFile(h, junk.data(), (DWORD)junk.size(), &w, nullptr); CloseHandle(h);
        }
        auto logs = LoadLogs(st, nullptr);
        int k = FindInterrupted(logs);
        if (k < 0) { bad++; wprintf(L"  crash %d torn %d: no interrupted log found\n", crash, torn); continue; }
        Ctx c; c.build = L"19045";
        Plan u = BuildUndo(logs[k], b, c);
        // the user ticks everything offered
        for (auto& row : u.rows) if (row.blocked.empty()) row.include = true;
        RunApply(u, Env(&b, &st), &r);
        for (int i = 0; i < N; i++) {
            StartCfg now = b.Items()[i].cfg;
            if (!now.SameEffect(orig[i])) { bad++; wprintf(L"  crash %d torn %d: Svc%d %ls, was %ls\n", crash, torn, i, CfgText(now).c_str(), CfgText(orig[i]).c_str()); }
        }
        logs = LoadLogs(st, nullptr);
        if (FindInterrupted(logs) >= 0) { bad++; wprintf(L"  crash %d torn %d: still interrupted after undo\n", crash, torn); }
    }
    wprintf(L"crash+undo: %d problems\n", bad);
}

// 4. Flush failure after every record count.
static void FlushFail() {
    int bad = 0;
    for (int ff = 0; ff < 40; ff++) {
        Hook b; std::vector<StartCfg> orig; Plan p = ManyPlan(b, 6, &orig);
        Store st(TmpDir(L"flush"));
        ApplyEnv e = Env(&b, &st); e.failFlushAfter = ff;
        ApplyResult r; RunApply(p, e, &r);
        // Every changed item must be recoverable from the log: count items changed vs rows with I.
        auto logs = LoadLogs(st, nullptr);
        int changed = 0; for (int i = 0; i < 6; i++) if (!b.Items()[i].cfg.SameEffect(orig[i])) changed++;
        int withI = 0; for (auto& l : logs) for (auto& row : l.rows) if (row.hasI) withI++;
        if (changed > withI) { bad++; wprintf(L"  flush fail after %d: %d changed, only %d with I record\n", ff, changed, withI); }
        if (r.aborted && changed && FindInterrupted(logs) < 0) { bad++; wprintf(L"  flush fail after %d: changes made, no interrupted log\n", ff); }
    }
    wprintf(L"flush failures: %d problems\n", bad);
}

// 5. Cancel after every k.
static void Cancel() {
    int bad = 0;
    for (int k = 1; k <= 8; k++) {
        Hook b; std::vector<StartCfg> orig; Plan p = ManyPlan(b, 8, &orig);
        std::atomic<bool> cancel{false}; b.cancel = &cancel; b.cancelAfter = k;
        Store st(TmpDir(L"cancel"));
        ApplyEnv e = Env(&b, &st); e.cancel = &cancel;
        ApplyResult r; RunApply(p, e, &r);
        if (!r.cancelled || r.ok != k) { bad++; wprintf(L"  cancel %d: cancelled=%d ok=%d\n", k, r.cancelled, r.ok); }
        auto logs = LoadLogs(st, nullptr);
        Ctx c; c.build = L"19045";
        int u = FindUndoable(logs);
        if (u < 0) { bad++; wprintf(L"  cancel %d: not undoable\n", k); continue; }
        if (FindInterrupted(logs) >= 0) { bad++; wprintf(L"  cancel %d: shows as interrupted\n", k); }
        b.cancel = nullptr;
        Plan up = BuildUndo(logs[u], b, c);
        RunApply(up, Env(&b, &st), &r);
        for (int i = 0; i < 8; i++) if (!b.Items()[i].cfg.SameEffect(orig[i])) { bad++; wprintf(L"  cancel %d: Svc%d not restored\n", k, i); }
    }
    wprintf(L"cancel: %d problems\n", bad);
}

// 6. A P record lost to a bad CRC: does undo notice the change it can no longer see?
static void LostP() {
    Hook b; std::vector<StartCfg> orig; Plan p = ManyPlan(b, 4, &orig);
    Store st(TmpDir(L"lostp"));
    ApplyResult r; RunApply(p, Env(&b, &st), &r);
    std::string text; std::wstring err; st.Read(L"undo-" + r.stamp + L".log", &text, &err);
    size_t at = text.find("\nP\t2\t"); text[at + 6] ^= 1;   // flip a byte in P 2's name
    Store st2(TmpDir(L"lostp2")); st2.WriteNew(L"undo-" + r.stamp + L".log", text, &err);
    auto logs = LoadLogs(st2, nullptr);
    Ctx c; c.build = L"19045";
    Plan u = BuildUndo(logs[FindUndoable(logs)], b, c);
    wprintf(L"lost P record: log count %d, rows %zu, badRecords %d, undo plan rows %zu (changed items 4)\n", logs[0].count, logs[0].rows.size(), logs[0].badRecords, u.rows.size());
    RunApply(u, Env(&b, &st2), &r);
    int left = 0; for (int i = 0; i < 4; i++) if (!b.Items()[i].cfg.SameEffect(orig[i])) left++;
    CHK(left == 0, L"lost P record: %d item(s) silently left changed after Undo", left);
}

// 7. Locks: same process twice, and a cross-process holder (see --sniff-hold).
static void Locks(const std::wstring& name) {
    ApplyLock a, b2; std::wstring e1, e2;
    bool x = a.Acquire(name, &e1); bool y = b2.Acquire(name, &e2);
    wprintf(L"lock: first %d, second(same proc) %d (%ls)\n", x, y, e2.c_str());
    a.Release();
    bool z = b2.Acquire(name, &e2);
    wprintf(L"lock: after release, second %d\n", z);
}

// 8. Recovery text for hostile names.
static void Recovery(const std::wstring& dir) {
    std::vector<std::wstring> names = {L"Intel(R) TPM Provisioning Service", L"paren)x", L"paren(x", L"sp ace", L"a*b?c", L"x'y`z", L"back]br[",
                                       L"bang!x", L"pct%x", L"caret^x", L"amp&x", L"\x05e9\x05dc", L"tab\tx", L"nl\nx", L"trail\\", L"brk]x\x00e9"};
    std::vector<RestoreRow> rows;
    for (auto& n : names) rows.push_back(RestoreRow{n, StartCfg{2, 1}, true});
    Store st(dir);
    std::wstring d, z, err;
    bool ok = WriteRecovery(st, L"20261111-101500", L"Hostile (names) & title %PATH% ^ !x!", rows, &d, &z, &err);
    wprintf(L"recovery: ok %d %ls -> %ls\n", ok, err.c_str(), d.c_str());
}

// 9. Snapshot round trip for hostile names (first.txt path).
static void SnapRoundTrip() {
    std::vector<std::wstring> names = {L"tab\tname", L"tab name", L"nl\nname", L"trail\\", L"\\\\double", L"x\\ny"};
    Snapshot s;
    for (auto& n : names) s.items.push_back(Svc(n, StartCfg{3, -1}));
    Snapshot back; std::wstring err;
    bool ok = ParseSnapshot(WriteSnapshot(s), &back, &err);
    wprintf(L"snapshot round trip: parse %d (%ls)\n", ok, err.c_str());
    if (ok) for (size_t i = 0; i < names.size() && i < back.items.size(); i++)
        if (back.items[i].name != names[i]) wprintf(L"  name changed: [%ls] -> [%ls]\n", names[i].c_str(), back.items[i].name.c_str());
}

// 10. Long and UNC data dirs.
static void LongDirs(const std::wstring& base) {
    std::wstring d = base;
    while (d.size() < 400) d += L"\\" + std::wstring(40, L'd');
    for (const std::wstring& root : {d, sys::ToExtended(d)}) {
        Store st(root);
        std::wstring err;
        bool ok = st.EnsureRoot(true, &err);
        Hook b; std::vector<StartCfg> orig; Plan p = ManyPlan(b, 3, &orig);
        ApplyResult r;
        bool ap = ok && RunApply(p, Env(&b, &st), &r);
        wprintf(L"long data dir (%zu chars, prefixed %d): ensure %d (%ls) apply %d (%ls) recovery dir %ls\n", root.size(), root.rfind(L"\\\\?\\", 0) == 0, ok, err.c_str(), ap, r.error.c_str(), r.recoveryDir.c_str());
        std::string readme; std::wstring e2;
        if (ap && st.Read(L"recovery\\" + r.stamp + L"\\README.txt", &readme, &e2)) {
            size_t k = readme.find("restore.cmd D:"); size_t a = readme.rfind('\n', k);
            wprintf(L"  README line: %hs\n", readme.substr(a + 1, k - a + 13).c_str());
        }
    }
}

int RunSniff(const std::vector<std::wstring>& args) {
    std::wstring what = ArgValue(args, L"--sniff");
    if (what == L"hold") {   // hold the real apply lock name for N ms
        ApplyLock l; std::wstring e;
        bool ok = l.Acquire(L"Global\\Hadron.services.apply", &e);
        wprintf(L"holder: %d %ls\n", ok, e.c_str()); fflush(stdout);
        Sleep(8000);
        return 0;
    }
    if (what == L"try") {
        ApplyLock l; std::wstring e;
        bool ok = l.Acquire(L"Global\\Hadron.services.apply", &e);
        wprintf(L"second process: %d %ls\n", ok, e.c_str()); fflush(stdout);
        return 0;
    }
    if (what == L"recovery") { Recovery(ArgValue(args, L"--dir")); return 0; }
    if (what == L"long") { LongDirs(ArgValue(args, L"--dir")); return 0; }
    DefaultRoundTrip();
    SingleRoundTrip();
    CrashUndo();
    FlushFail();
    Cancel();
    LostP();
    Locks(L"Local\\sniff.lock");
    SnapRoundTrip();
    wprintf(L"sniff: %d hard failures\n", g_fail);
    fflush(stdout);
    return 0;
}

}  // namespace hx::services
