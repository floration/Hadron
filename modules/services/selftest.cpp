// modules/services/selftest.cpp — --selftest, --dump-plan and --live-test (see cli.h).
//
// The selftest runs against the fake backend and a fresh temp data dir only: it never touches real services
// or %ProgramData%. Its one live call is a read-only Enumerate, plus a check that the unarmed live backend
// refuses a write before reaching the SCM (on a service name that does not exist).
#include "modules/services/cli.h"
#include "core/sys/sys.h"
#include "modules/services/plan.h"
#include "modules/services/recovery.h"
#include "modules/services/ti.h"

#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <functional>

namespace hx::services {

// ---- shared helpers --------------------------------------------------------------------------------------
std::wstring ArgValue(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (size_t i = 0; i + 1 < args.size(); i++) if (args[i] == name) return args[i + 1];
    return L"";
}
bool HasArg(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (auto& a : args) if (a == name) return true;
    return false;
}

int LevelFromName(const std::wstring& s) {
    if (s == L"bloat") return LV_BLOAT;
    if (s == L"stripped") return LV_STRIPPED;
    if (s == L"minimum") return LV_MINIMUM;
    if (s == L"default") return LEVEL_DEFAULT;
    if (s == L"stock") return -2;
    return -3;
}

bool LoadFixtureDir(const std::wstring& dir, FixtureData* out, std::wstring* err) {
    if (!out->backend.Load(dir, err)) return false;
    Store s(dir);
    if (s.Exists(L"first.txt")) {
        std::string text;
        std::wstring e;
        if (s.Read(L"first.txt", &text, &e) && ParseSnapshot(text, &out->first, &e)) out->hasFirst = true;
        else out->problems.push_back(L"first.txt: " + e);
    }
    return true;
}

namespace {

std::wstring Parent(const std::wstring& p) {
    std::wstring s = p;
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"" : s.substr(0, k);
}

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(sys::ToExtended(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FindFixture(const std::vector<std::wstring>& args) {
    std::wstring f = ArgValue(args, L"--fixture");
    if (!f.empty()) return f;
    wchar_t cwd[MAX_PATH];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
    if (n && n < MAX_PATH) {
        std::wstring c = std::wstring(cwd) + L"\\tests\\fixtures\\services";
        if (DirExists(c)) return c;
    }
    std::wstring exe = Parent(sys::ModulePath());
    for (int up = 0; up < 3 && !exe.empty(); up++, exe = Parent(exe)) {
        std::wstring c = exe + L"\\tests\\fixtures\\services";
        if (DirExists(c)) return c;
    }
    return L"";
}

bool ReadAll(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out->clear();
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out->append(buf, got);
    CloseHandle(h);
    return true;
}

bool WriteAll(const std::wstring& path, const std::string& data) {   // test outputs only (goldens, --out)
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(h);
    return ok;
}

// A fresh, empty temp folder for one test's data dir. Left in place (the path is printed); nothing is deleted.
std::wstring NewTempDir(const std::wstring& tag) {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring base = (n && n < MAX_PATH) ? std::wstring(tmp) : L".\\";
    for (int i = 0; i < 1000; i++) {
        std::wstring d = base + L"HadronSvcTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + tag + L"-" + std::to_wstring(i);
        if (CreateDirectoryW(sys::ToExtended(d).c_str(), nullptr)) return d;
    }
    return L"";
}

std::vector<SvcItem> Folded(FakeBackend& b) {
    std::vector<SvcItem> v;
    std::wstring err;
    b.Enumerate(&v, &err);
    FoldInstances(&v);
    return v;
}

struct World {
    FixtureData fx;
    std::vector<SvcItem> items;
    HwInfo hw;
    Ctx ctx;
};

bool MakeWorld(const std::wstring& dir, const Kb* kb, World* w, std::wstring* err) {
    if (!LoadFixtureDir(dir, &w->fx, err)) return false;
    w->items = Folded(w->fx.backend);
    w->fx.backend.ReadHw(&w->hw);
    w->ctx = Ctx{};
    w->ctx.kb = kb;
    w->ctx.hw = w->hw.ok ? &w->hw : nullptr;
    w->ctx.first = w->fx.hasFirst ? &w->fx.first : nullptr;
    w->ctx.build = w->fx.backend.Build();
    w->ctx.machine = w->fx.backend.MachineGuid();
    w->ctx.Prepare();
    return true;
}

// What a user does on the plan screen: tick the rows offered unticked because their use cannot be detected.
void TickUndetectable(Plan* p) {
    for (auto& r : p->rows)
        if (r.canToggle && r.blocked.empty() && r.evidence.rfind(L"use cannot be detected", 0) == 0) r.include = true;
}

void Tick(Plan* p, const wchar_t* name) {
    for (auto& r : p->rows)
        if (r.name == name && r.canToggle) r.include = true;
}

Plan PlanFor(int level, int tab, World& w) {
    Plan p;
    if (level >= 0) p = BuildPreset(level, tab, w.items, w.ctx);
    else p = BuildDefault(level == -2, tab, w.items, w.ctx);
    MarkTi(&p, w.fx.backend, w.ctx.tiOn);
    return p;
}

const PlanRow* RowOf(const Plan& p, const wchar_t* name) {
    for (auto& r : p.rows) if (IEq(r.name, name)) return &r;
    return nullptr;
}

const SvcItem* ItemOf(const std::vector<SvcItem>& v, const wchar_t* name) {
    for (auto& i : v) if (IEq(i.name, name)) return &i;
    return nullptr;
}

std::map<std::wstring, StartCfg> CfgMap(FakeBackend& b) {
    std::map<std::wstring, StartCfg> m;
    for (auto& i : b.Items()) m[Lower(i.name)] = i.cfg;
    return m;
}

// The selftest's TrustedInstaller: records that it ran under the impersonation flag and that Hadron's own file
// writes are refused while it "impersonates".
struct FakeTi : TiProvider {
    Store* probe = nullptr;
    bool acquired = false, started = false, released = false, flagSeen = false, writeRefused = false;
    int impersonations = 0;
    bool Acquire(std::wstring*) override { acquired = true; started = true; return true; }
    bool StartedService() const override { return started; }
    bool Impersonate(std::wstring* identity, std::wstring*) override {
        impersonations++;
        flagSeen = ImpersonationFlag::Active();
        if (probe) {
            std::wstring e;
            writeRefused = !probe->WriteNew(L"probe-" + std::to_wstring(impersonations) + L".txt", "x", &e);
        }
        *identity = L"ti";
        return true;
    }
    void Revert() override {}
    bool Release(std::wstring* note) override { released = true; *note = L"fake TI stopped"; return true; }
};

std::wstring LockName() { return L"Local\\Hadron.services.selftest." + std::to_wstring(GetCurrentProcessId()); }

ApplyEnv EnvFor(Backend* b, Store* s) {
    ApplyEnv e;
    e.backend = b;
    e.store = s;
    e.lockName = LockName();
    e.build = L"19045";
    e.machine = L"6f1c2a4e-fixture-0000-0000-000000000001";
    return e;
}

}  // namespace

std::string DumpFixturePlan(const std::wstring& fixture, int level, int tab, std::wstring* err) {
    Kb kb;
    if (!LoadEmbeddedKb(&kb, err)) return "";
    World w;
    if (!MakeWorld(fixture, &kb, &w, err)) return "";
    return DumpPlan(PlanFor(level, tab, w));
}

// ============================================================================================
int RunSelftest(const std::vector<std::wstring>& args) {
    int fails = 0, checks = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        checks++;
        if (!ok) fails++;
        wprintf(L"  [%ls] %ls\n", ok ? L"PASS" : L"FAIL", what.c_str());
        fflush(stdout);
    };
    auto info = [&](const std::wstring& s) { wprintf(L"  [info] %ls\n", s.c_str()); fflush(stdout); };
    wprintf(L"services selftest\n");
    const bool update = HasArg(args, L"--update-golden");

    // ---- pure text ----------------------------------------------------------------------------------
    {
        StartCfg c;
        check(CfgText(StartCfg{2, 1}) == L"2/1" && CfgText(StartCfg{3, -1}) == L"3/-", L"CfgText: \"2/1\", \"3/-\"");
        check(ParseCfg(L"4/0", &c) && c.start == 4 && c.delayed == 0 && !ParseCfg(L"5/-", &c) && !ParseCfg(L"2/x", &c),
              L"ParseCfg accepts 4/0 and rejects 5/- and 2/x");
        check(StartLabel(StartCfg{2, 1}, false) == L"Auto (delayed)" && StartLabel(StartCfg{4, 1}, false) == L"Disabled" &&
                  StartLabel(StartCfg{3, -1}, true) == L"Manual +trigger",
              L"StartLabel: delayed only with Auto; +trigger marked");
        check(StartCfg{2, 0}.SameEffect(StartCfg{2, -1}) && !StartCfg{2, 1}.SameEffect(StartCfg{2, 0}) &&
                  StartCfg{4, 1}.SameEffect(StartCfg{4, 0}),
              L"SameEffect: an absent DelayedAutostart equals 0; delayed means nothing without Auto");
        check(TemplateName(L"CDPUserSvc_4f2a1", 0xE0) == L"CDPUserSvc" && TemplateName(L"Foo_1a2b", 0x10) == L"Foo_1a2b",
              L"TemplateName strips _LUID only with the instance type bit (r5 E1)");
        ImageInfo a = NormalizeImage(L"\\SystemRoot\\System32\\drivers\\x.sys", L"C:\\Windows", L"x", true);
        ImageInfo b = NormalizeImage(L"\\??\\C:\\Drv\\y.sys", L"C:\\Windows", L"y", true);
        ImageInfo c2 = NormalizeImage(L"System32\\drivers\\z.sys", L"C:\\Windows", L"z", true);
        ImageInfo d = NormalizeImage(L"", L"C:\\Windows", L"Beep", true);
        ImageInfo e = NormalizeImage(L"%SystemRoot%\\system32\\svchost.exe -k netsvcs -p", L"C:\\Windows", L"s", false);
        ImageInfo f = NormalizeImage(L"\"C:\\Program Files\\X\\x.exe\" /svc", L"C:\\Windows", L"x", false);
        ImageInfo g = NormalizeImage(L"C:\\Program Files\\X\\x.exe -run", L"C:\\Windows", L"x", false);
        check(a.path == L"C:\\Windows\\System32\\drivers\\x.sys" && b.path == L"C:\\Drv\\y.sys" &&
                  c2.path == L"C:\\Windows\\System32\\drivers\\z.sys" && d.path == L"C:\\Windows\\System32\\drivers\\Beep.sys",
              L"NormalizeImage: \\SystemRoot\\, \\??\\, system32-relative and empty driver paths");
        check(e.svchostGroup == L"netsvcs" && f.path == L"C:\\Program Files\\X\\x.exe" && f.args == L"/svc" &&
                  g.unquotedSpaces && g.args == L"-run",
              L"NormalizeImage: svchost -k group, quoted path + args, unquoted path with spaces flagged");
        check(Unesc(Esc(L"a\\b\tc\nd")) == L"a\\b c\nd", L"bios escaping round trip (TAB becomes a space)");
        check(Crc32("123456789", 9) == 0xCBF43926u, L"CRC-32 check value");
        std::vector<SvcItem> v(3);
        v[0].name = L"CDPUserSvc"; v[0].type = 0x60;
        v[1].name = L"CDPUserSvc_4f2a1"; v[1].type = 0xE0; v[1].state = SERVICE_RUNNING;
        v[2].name = L"Other"; v[2].type = 0x10;
        FoldInstances(&v);
        check(v.size() == 2 && v[0].instances.size() == 1 && v[0].instances[0] == L"CDPUserSvc_4f2a1 (Running)",
              L"FoldInstances folds a per-user instance under its template");
    }

    // ---- KB -------------------------------------------------------------------------------------------
    Kb kb;
    std::wstring err;
    bool kbOk = LoadEmbeddedKb(&kb, &err);
    check(kbOk, L"embedded services.db parses" + (kbOk ? L"" : L": " + err));
    if (!kbOk) { wprintf(L"services selftest: FAIL\n"); return 1; }
    {
        size_t n = kb.entries.size();
        bool basis = true, pbasis = true, neverNoLevel = true, drvSysok = true;
        int levelEntries = 0;
        for (auto& e : kb.entries) {
            if (e.basis.empty()) basis = false;
            if (e.JoinLevel() >= 0) {
                levelEntries++;
                if (e.pbasis.empty()) pbasis = false;
                if (e.never) neverNoLevel = false;
                if (e.kind == L"drv" && !e.sysok) drvSysok = false;
            }
        }
        check(n >= 60 && n <= 100, L"KB seed has 60-100 entries (" + std::to_wstring(n) + L")");
        check(kb.version == L"1" && kb.build == L"19045", L"KB header: V 1 services, B 19045");
        check(basis, L"every KB entry has a basis (rule 7)");
        check(pbasis, L"every preset membership has a pbasis (" + std::to_wstring(levelEntries) + L" entries)");
        check(neverNoLevel && drvSysok, L"no never entry has a level; every driver in a level has sysok");
        const KbEntry* d = kb.Find(L"diagtrack");
        check(d && d->TargetAt(LV_BLOAT) == 4 && d->TargetAt(LV_MINIMUM) == 4 && d->JoinLevel() == LV_BLOAT,
              L"Find is case-insensitive; membership is cumulative (bloat entries are in Minimum)");
        const KbEntry* s = kb.Find(L"SysMain");
        check(s && s->TargetAt(LV_STRIPPED) < 0 && s->TargetAt(LV_MINIMUM) == 4 && !s->contested.empty(),
              L"SysMain: Minimum only, contested with sources on both sides");
        bool urls = true;
        for (auto& e : kb.entries)
            if (e.basis == L"published" && e.basisNote.find(L"https://") == std::wstring::npos) urls = false;
        check(urls, L"every published basis note carries its https URL");
        int stock = 0;
        for (auto& e : kb.entries) if (e.stock >= 0) stock++;
        info(L"KB: " + std::to_wstring(n) + L" entries, " + std::to_wstring(stock) +
             L" with a Windows stock value (none has a measured or published stock basis yet)");
    }

    // ---- fixture: snapshot round trip, hw --------------------------------------------------------------
    std::wstring fixture = FindFixture(args);
    check(!fixture.empty(), L"fixture folder found (" + (fixture.empty() ? std::wstring(L"none") : fixture) + L")");
    if (fixture.empty()) { wprintf(L"services selftest: FAIL (%d checks, %d failed)\n", checks, fails); return 1; }
    std::wstring golden = ArgValue(args, L"--golden");
    if (golden.empty()) golden = Parent(Parent(fixture)) + L"\\golden\\services";
    World w;
    check(MakeWorld(fixture, &kb, &w, &err), L"fixture loads (snapshot.txt, hw.txt, first.txt)" + (err.empty() ? L"" : L": " + err));
    {
        std::string raw;
        Snapshot s;
        bool ok = ReadAll(fixture + L"\\snapshot.txt", &raw) && ParseSnapshot(raw, &s, &err);
        check(ok && WriteSnapshot(s, true) == raw, L"snapshot.txt: parse -> write reproduces the file byte for byte");
        check(ok && s.items.size() > 100, L"snapshot.txt has " + std::to_wstring(s.items.size()) + L" items");
        std::string bad = "HXSVC\t1\tsnapshot\nS\tA\tA\t10\t3/-\t0\t1\t1\t\t\t\t\t\t3\t0\t0\t0\nS\ta\tA\t10\t3/-\t0\t1\t1\t\t\t\t\t\t3\t0\t0\t0\n";
        Snapshot s2;
        check(!ParseSnapshot(bad, &s2, &err), L"a snapshot with a duplicate name (case-insensitive) is rejected");
        Snapshot s3;
        check(ParseSnapshot("HXSVC\t1\tsnapshot\nZ\tfuture line\n", &s3, &err) && s3.skippedLines == 1,
              L"unknown snapshot lines are skipped (forward compatible)");
        std::string hwRaw;
        HwInfo h;
        ok = ReadAll(fixture + L"\\hw.txt", &hwRaw) && ParseHw(hwRaw, &h, &err);
        check(ok && DumpHw(h) == hwRaw, L"hw.txt: parse -> dump reproduces the file");
        auto f = FilterServices(h);
        auto bc = BootChainServices(h);
        check(f.count(L"exampleaudioflt") && f.count(L"volsnap") && f.count(L"kbdclass") && f.count(L"ehstorclass"),
              L"FilterServices: class and device Upper/LowerFilters");
        check(bc.count(L"disk") && bc.count(L"stornvme") && bc.count(L"pci") && bc.count(L"acpi") && !bc.count(L"nvlddmkm"),
              L"BootChainServices: the boot disk's parent chain (disk, stornvme, pci, ACPI)");
        std::wstring ev;
        int hb = HwEvidence(h, L"class:Bluetooth", L"bthserv", &ev);
        check(hb == 0 && ev.find(L"present") != std::wstring::npos, L"hw evidence: Bluetooth present -> kept (" + ev + L")");
        int hi = HwEvidence(h, L"class:Image", L"stisvc", &ev);
        check(hi == 0 && ev.find(L"seen before") != std::wstring::npos, L"hw evidence: a phantom scanner -> kept (" + ev + L")");
        check(HwEvidence(h, L"class:Printer", L"Spooler", &ev) == 2, L"hw evidence: no printer ever, hot-plug -> unticked");
        check(HwEvidence(h, L"class:Sensor", L"SensorService", &ev) == 1, L"hw evidence: no sensor ever -> removable");
        check(HwEvidence(h, L"self", L"igfxn", &ev) == 1 && HwEvidence(h, L"svc:igfxn", L"cphs", &ev) == 1,
              L"hw evidence: no Intel graphics devnode -> igfxn and cphs removable");
        check(HwEvidence(h, L"vmguest", L"vmicheartbeat", &ev) == 1 && HwEvidence(h, L"hvhost", L"HvHost", &ev) == 1,
              L"hw evidence: not a VM, no hypervisor -> guest pieces removable");
        HwInfo vm = h;
        vm.isVm = true;
        vm.hypervisor = true;
        check(HwEvidence(vm, L"vmguest", L"x", &ev) == 0 && HwEvidence(vm, L"hvhost", L"x", &ev) == 0,
              L"hw evidence: a VM guest / running hypervisor keeps them");
        HwInfo none;
        check(HwEvidence(none, L"class:Sensor", L"x", &ev) == 0, L"hw evidence: no hardware list -> kept");
    }

    // ---- guards (spec §6) ----------------------------------------------------------------------------------
    {
        Ctx& c = w.ctx;
        auto guard = [&](const wchar_t* name) { const SvcItem* i = ItemOf(w.items, name); return i ? CheckGuard(*i, c) : Guard{}; };
        auto has = [](const Guard& g, const wchar_t* s) { return g.reason.find(s) != std::wstring::npos; };
        Guard g = guard(L"WdBoot");
        check(g.neverTouch && has(g, L"Boot-start"), L"guard: Boot-start driver -> never touch");
        g = guard(L"ExampleAudioFlt");
        check(g.neverTouch && has(g, L"Filters"), L"guard: a device LowerFilters entry -> never touch");
        SvcItem nvme = *ItemOf(w.items, L"stornvme");
        nvme.cfg.start = 3;
        g = CheckGuard(nvme, c);
        check(g.neverTouch && has(g, L"boot disk"), L"guard: function driver on the boot disk's chain -> never touch");
        g = guard(L"ExampleCriticalDrv");
        check(g.neverTouch && has(g, L"Critical"), L"guard: ErrorControl Critical -> never touch");
        g = guard(L"intelppm");
        check(g.neverTouch && has(g, L"processor"), L"guard: curated floor (intelppm) -> never touch");
        g = guard(L"RpcSs");
        check(g.neverTouch && has(g, L"core Windows service"), L"guard: core service (RpcSs) -> never touch");
        g = guard(L"hvservice");
        check(!g.neverTouch, L"guard: hypervisor driver is not floor while the hypervisor is off");
        HwInfo hv = w.hw;
        hv.hypervisor = true;
        Ctx c2 = c;
        c2.hw = &hv;
        c2.Prepare();
        g = CheckGuard(*ItemOf(w.items, L"hvservice"), c2);
        check(g.neverTouch && has(g, L"hypervisor"), L"guard: hypervisor driver while the hypervisor runs -> never touch");
        g = guard(L"WinDefend");
        check(!g.neverTouch && g.neverPreset && has(g, L"launch-protected"), L"guard: launch-protected (WinDefend) -> not in presets");
        g = guard(L"VSS");
        check(g.neverPreset && has(g, L"Volume Shadow Copy"), L"guard: VSS -> not in presets");
        g = guard(L"CldFlt");
        check(g.neverPreset && has(g, L"filter"), L"guard: CldFlt (named filter) -> not in presets");
        g = guard(L"ExampleUpdater");
        check(g.neverPreset && has(g, L"no Hadron KB entry"), L"guard: third-party -> not in presets");
        g = guard(L"wuauserv");
        check(g.neverPreset && has(g, L"keeps it out"), L"guard: KB never (wuauserv) -> not in presets");
        Ctx c3 = c;
        c3.build = L"22631";
        g = CheckGuard(*ItemOf(w.items, L"DiagTrack"), c3);
        check(g.neverPreset && has(g, L"22631"), L"guard: KB builds do not list the running build -> not in presets");
        Plan p3 = BuildPreset(LV_BLOAT, TAB_SERVICES, w.items, c3);
        check(p3.Included() == 0, L"guard: on another build the Bloat plan has no ticked rows");

        // System-start driver without sysok, against the live start type.
        Kb kb3;   // an in-memory KB whose igfxn entry has a level but no sysok
        {
            std::string db = "V\t1\tservices\nB\t19045\n"
                             "K\tigfxn\tdrv\t19045\tt\te\t\t\t\t4\t\t\twhy\t\t0\t0\treasoning\tn\treasoning\tn\t\t\t\tself\t\t\n";
            kb3.Parse(db, &err);
        }
        std::vector<SvcItem> sysItems = w.items;
        for (auto& i : sysItems) if (i.name == L"igfxn") i.cfg.start = 1;
        Ctx c4 = c;
        c4.kb = &kb3;
        Plan p4 = BuildPreset(LV_STRIPPED, TAB_DRIVERS, sysItems, c4);
        const PlanRow* ir = RowOf(p4, L"igfxn");
        check(ir && !ir->blocked.empty() && ir->blocked.find(L"System-start") != std::wstring::npos,
              L"guard: a System-start driver without sysok is blocked (live start type)");

        // Per-user instance, TI service with the toggle on, Boot/System by hand, drivers never controlled.
        SvcItem inst;
        inst.name = L"CDPUserSvc_4f2a1"; inst.type = 0xE0; inst.kind = Kind::UserInstance; inst.cfg = {2, -1};
        Plan ps = BuildSingle(inst, StartCfg{4, -1}, w.items, c);
        check(ps.rows.size() == 1 && !ps.rows[0].blocked.empty() && ps.Included() == 0, L"guard: a per-user instance is never written");
        Ctx cti = c;
        cti.tiOn = true;
        Plan pt = BuildSingle(*ItemOf(w.items, L"TrustedInstaller"), StartCfg{4, -1}, w.items, cti);
        check(pt.Included() == 0 && !pt.rows[0].blocked.empty(), L"guard: the TrustedInstaller service is blocked while the toggle is on");
        Plan pb = BuildSingle(*ItemOf(w.items, L"DiagTrack"), StartCfg{1, -1}, w.items, c);
        check(pb.Included() == 0, L"guard: Hadron never sets Boot or System start");
        Plan pn = BuildSingle(*ItemOf(w.items, L"RpcSs"), StartCfg{3, -1}, w.items, c);
        check(pn.Included() == 0, L"guard: never-touch items refuse single changes too");
        Plan pw = BuildSingle(*ItemOf(w.items, L"WinDefend"), StartCfg{3, -1}, w.items, c);
        check(pw.Included() == 1, L"guard: a never-in-preset item allows a single change (Windows may still refuse)");
        Plan pc = BuildControl(*ItemOf(w.items, L"igfxn"), 1, w.fx.backend, w.items);
        check(!pc.rows.empty() && !pc.rows[0].blocked.empty(), L"guard: drivers are never loaded or unloaded live");
        Plan pr = BuildControl(*ItemOf(w.items, L"Audiosrv"), 2, w.fx.backend, w.items);
        check(pr.rows.size() >= 2 && pr.rows.front().ctl == Op::Stop && pr.rows.back().ctl == Op::Start,
              L"control: restart stops (dependents first), then starts");
    }

    // ---- plans on the fixture ------------------------------------------------------------------------------
    {
        Plan bl = PlanFor(LV_BLOAT, TAB_SERVICES, w);
        Plan st = PlanFor(LV_STRIPPED, TAB_SERVICES, w);
        Plan mn = PlanFor(LV_MINIMUM, TAB_SERVICES, w);
        Plan dv = PlanFor(LV_STRIPPED, TAB_DRIVERS, w);
        Plan db = PlanFor(LV_BLOAT, TAB_DRIVERS, w);
        check(bl.alreadySet >= 2 && !RowOf(bl, L"DiagTrack") && RowOf(bl, L"MapsBroker") && RowOf(bl, L"MapsBroker")->include,
              L"Bloat: DiagTrack and XblGameSave already disabled; MapsBroker ticked");
        const PlanRow* dm = RowOf(st, L"dmwappushservice");
        check(dm && dm->needsTi && !dm->include && !dm->canToggle && dm->warning.find(L"TrustedInstaller") != std::wstring::npos,
              L"TI marking: an admin-refused row is excluded with the toggle off, with the reason");
        check(!RowOf(bl, L"dmwappushservice"), L"No guidance from IoT keeps dmwappushservice out of Pure bloat");
        const PlanRow* ud = RowOf(st, L"SSDPSRV");
        check(ud && ud->blocked.empty() && !ud->include && ud->canToggle && ud->evidence.rfind(L"use cannot be detected", 0) == 0,
              L"Stripped: a row whose use cannot be detected is offered unticked, with the reason");
        const PlanRow* fs = RowOf(st, L"FrameServer");
        check(fs && fs->blocked.empty() && !fs->include && fs->evidence.find(L"hot-plug") != std::wstring::npos,
              L"Stripped: a never-seen hot-plug class (camera) is offered unticked");
        const PlanRow* bt = RowOf(st, L"bthserv");
        check(bt && !bt->blocked.empty() && bt->blocked.find(L"Kept") == 0, L"Stripped: Bluetooth present -> kept");
        const PlanRow* sn = RowOf(st, L"SensorService");
        check(sn && sn->include, L"Stripped: never-seen sensors -> ticked, with evidence");
        const PlanRow* sp = RowOf(st, L"Spooler");
        check(sp && !sp->include && sp->warning.find(L"Fax") != std::wstring::npos,
              L"dependency check: disabling Spooler while Fax depends on it -> warning, unticked");
        const PlanRow* vm = RowOf(st, L"vmicheartbeat");
        check(vm && vm->include && !vm->contested.empty(), L"Stripped: Hyper-V guest pieces on hardware -> ticked, contested shown");
        check(RowOf(mn, L"SysMain") && !RowOf(st, L"SysMain") && RowOf(mn, L"MapsBroker"),
              L"Minimum adds the contested items and keeps Bloat's (cumulative)");
        check(RowOf(dv, L"igfxn") && RowOf(dv, L"igfxn")->include && RowOf(dv, L"BTHUSB") && !RowOf(dv, L"BTHUSB")->blocked.empty(),
              L"Drivers Stripped: igfxn (absent) ticked, BTHUSB (radio present) kept");
        check(db.Included() == 0, L"Drivers Bloat: near-empty, and the plan says so");
        bool noFloor = true;
        for (auto* p : {&bl, &st, &mn, &dv})
            for (auto& r : p->rows)
                if (r.include && (IEq(r.name, L"RpcSs") || IEq(r.name, L"VSS") || IEq(r.name, L"WinDefend") || IEq(r.name, L"TrustedInstaller")))
                    noFloor = false;
        check(noFloor, L"no preset ticks a floor or never-in-preset item");
        Plan df = PlanFor(LEVEL_DEFAULT, TAB_SERVICES, w);
        check(df.Included() == 3 && RowOf(df, L"WSearch") && RowOf(df, L"WSearch")->after.Delayed(),
              L"Default (first snapshot): 3 rows, WSearch back to Auto (delayed)");
        Plan sk = PlanFor(-2, TAB_SERVICES, w);
        check(sk.rows.empty() && !sk.notes.empty(), L"Default (Windows stock): no KB stock values yet -> no rows, with a note");
        // Stock mechanism with an in-memory KB that has one measured stock value (and one refused storage driver).
        Kb ks;
        ks.Parse("V\t1\tservices\nB\t19045\n"
                 "K\tWSearch\tsvc\t19045\tt\te\t\t\t\t\t\t\t\t\t0\t0\treasoning\tn\t\t\t2/1\towner-measured\tclean VM\t\t\t\n"
                 "K\tdisk\tdrv\t19045\tt\te\t\t\t\t\t\t\t\t\t0\t0\treasoning\tn\t\t\t3/-\towner-measured\tclean VM\t\t\t\n",
                 &err);
        Ctx cs = w.ctx;
        cs.kb = &ks;
        Plan sm = BuildDefault(true, TAB_SERVICES, w.items, cs);
        Plan sd = BuildDefault(true, TAB_DRIVERS, w.items, cs);
        check(sm.Included() == 1 && RowOf(sm, L"WSearch") && RowOf(sm, L"WSearch")->basis == L"owner-measured" && !RowOf(sd, L"disk"),
              L"Windows stock: applies a KB stock value with its basis; never for a storage driver");
        Ctx co = w.ctx;
        co.build = L"22631";
        Plan so = BuildDefault(true, TAB_SERVICES, w.items, co);
        check(so.rows.empty() && !so.notes.empty() && so.notes[0].find(L"19045") != std::wstring::npos,
              L"Windows stock on another build: refused with the reason");
        // Goldens: every level x tab.
        const wchar_t* lv[] = {L"default", L"bloat", L"stripped", L"minimum"};
        const wchar_t* tb[] = {L"services", L"drivers"};
        for (int t = 0; t < 2; t++)
            for (int l = 0; l < 4; l++) {
                std::string got = DumpPlan(PlanFor(LevelFromName(lv[l]), t, w));
                std::wstring gp = golden + L"\\plan-" + tb[t] + L"-" + lv[l] + L".txt";
                if (update) { check(WriteAll(gp, got), L"wrote golden " + gp); continue; }
                std::string want;
                bool have = ReadAll(gp, &want);
                check(have && want == got, std::wstring(L"golden plan-") + tb[t] + L"-" + lv[l] + L".txt" +
                                               (have ? (want == got ? L"" : L" differs") : L" missing"));
            }
    }

    // ---- store, logs, lock ---------------------------------------------------------------------------------
    std::wstring tmpRoot = NewTempDir(L"store");
    check(!tmpRoot.empty(), L"temp data dir " + tmpRoot);
    {
        Store s(tmpRoot);
        check(s.WriteNew(L"a.txt", "one", &err) && !s.WriteNew(L"a.txt", "two", &err), L"WriteNew is CREATE_NEW: never overwrites");
        check(s.WriteAtomicNew(L"b.txt", "one", &err) && !s.WriteAtomicNew(L"b.txt", "two", &err),
              L"WriteAtomicNew refuses an existing file");
        std::string back;
        check(s.Read(L"a.txt", &back, &err) && back == "one", L"the first write is intact");
        s.fixedStamp = L"20261111-101500";
        bool fresh = s.NewStamp() == L"20261111-101500";
        bool wrote = s.WriteNew(L"undo-20261111-101500.log", "x", &err);
        check(fresh && wrote && s.NewStamp() == L"20261111-101500-1",
              L"NewStamp adds -N when the stamp is taken");
        ImpersonationFlag::Active() = true;
        bool refused = !s.WriteNew(L"c.txt", "x", &err);
        ImpersonationFlag::Active() = false;
        check(refused, L"every Hadron file write refuses while impersonating");
        ApplyLock l1, l2;
        bool a1 = l1.Acquire(LockName(), &err);
        bool a2 = l2.Acquire(LockName(), &err);
        check(a1 && !a2, L"apply lock: a second holder is refused");
        l1.Release();
        check(l2.Acquire(LockName(), &err), L"apply lock: free again after release");
        // Torn and corrupted log records.
        std::string log;
        UndoLog ul;
        std::vector<std::wstring> logs = Store(fixture + L"\\interrupted").List(L"undo-*.log");
        bool ok = !logs.empty() && ReadAll(fixture + L"\\interrupted\\" + logs[0], &log) && ParseUndoLog(log, &ul, &err);
        check(ok && ul.badRecords == 1 && ul.rows.size() == 3 && ul.Row(2) && ul.Row(2)->hasI && !ul.Row(2)->hasD && !ul.hasE,
              L"undo log: a torn last record is ignored; row 2 was in progress");
        std::string flipped = log;
        size_t at = flipped.find("MapsBroker");
        if (at != std::string::npos) flipped[at] = 'N';
        UndoLog ul2;
        check(ParseUndoLog(flipped, &ul2, &err) && ul2.badRecords == 2, L"undo log: a record whose CRC does not match is ignored");
    }

    // ---- recovery -----------------------------------------------------------------------------------------
    {
        std::vector<RestoreRow> rows = {{L"DiagTrack", {2, -1}, false}, {L"MapsBroker", {2, 1}, true},
                                        {L"Spooler", {2, -1}, false}, {L"CDPUserSvc", {2, 0}, true},
                                        {L"Bad&Name", {3, -1}, false}, {L"D\u00e9mo", {3, -1}, false}};
        std::string cmd = RestoreCmd(L"20261111-101500", L"Stripped (Services)", rows);
        std::wstring gp = golden + L"\\restore.cmd";
        if (update) check(WriteAll(gp, cmd), L"wrote golden " + gp);
        else {
            std::string want;
            bool have = ReadAll(gp, &want);
            check(have && want == cmd, L"restore.cmd matches tests/golden/services/restore.cmd" + std::wstring(have ? L"" : L" (missing)"));
        }
        bool ascii = true;
        for (unsigned char ch : cmd) if (ch >= 0x80) ascii = false;
        check(ascii && cmd.find("Bad&Name") == std::string::npos && cmd.find("\r\n") != std::string::npos,
              L"restore.cmd is ASCII with CRLF and leaves cmd-unsafe names to the .reg files");
        check(CmdSafeName(L"DiagTrack") && !CmdSafeName(L"A&B") && !CmdSafeName(L"a%b") && !CmdSafeName(L"D\u00e9mo"),
              L"CmdSafeName rejects cmd metacharacters and non-ASCII");
        std::vector<ZipEntry> z = {{"restore.cmd", cmd}, {"README.txt", "readme"}};
        std::string zip = MakeZip(z, 0x5000, 0x5B6B);
        std::vector<ZipEntry> back;
        check(ParseZip(zip, &back, &err) && back.size() == 2 && back[0].name == "restore.cmd" && back[0].data == cmd,
              L"zip: write -> parse back (stored, CRC checked)");
        std::string broken = zip;
        size_t p = broken.find("@echo off");
        if (p != std::string::npos) broken[p] = '#';
        check(!ParseZip(broken, &back, &err), L"zip: a changed byte fails the CRC check");
        std::string reg = UnicodeReg(rows, 1);
        check(reg.size() > 2 && (unsigned char)reg[0] == 0xFF && (unsigned char)reg[1] == 0xFE,
              L"the Unicode .reg file is UTF-16LE with a BOM");
        Store s(tmpRoot);
        std::wstring dir, zp;
        check(WriteRecovery(s, L"rtest", L"test", rows, &dir, &zp, &err), L"WriteRecovery writes the folder and the zip");
        std::string zr;
        std::vector<ZipEntry> ze;
        bool parsed = s.Read(L"recovery-rtest.zip", &zr, &err) && ParseZip(zr, &ze, &err);
        bool hasCmd = false, hasReadme = false, hasReg = false;
        for (auto& e : ze) {
            if (e.name == "restore.cmd") hasCmd = true;
            if (e.name == "README.txt") hasReadme = true;
            if (e.name.find(".reg") != std::string::npos) hasReg = true;
        }
        check(parsed && hasCmd && hasReadme && hasReg, L"recovery zip parses back with restore.cmd, README.txt and the .reg files");
        check(!WriteRecovery(s, L"rtest", L"test", rows, &dir, &zp, &err), L"recovery files are never overwritten");
    }

    // ---- first-run snapshot ---------------------------------------------------------------------------------
    {
        std::wstring root = NewTempDir(L"first");
        Store s(root);
        Snapshot first;
        FirstRunResult r = EnsureFirstSnapshot(s, w.items, L"19045", L"machine-a", &first);
        check(r.wrote && r.error.empty() && s.Exists(L"recovery-first.zip") && s.Exists(L"recovery\\first\\restore.cmd"),
              L"first run: first.txt, recovery\\first and recovery-first.zip written" + (r.error.empty() ? L"" : L": " + r.error));
        Snapshot again;
        FirstRunResult r2 = EnsureFirstSnapshot(s, w.items, L"19045", L"machine-a", &again);
        check(r2.existed && !r2.wrote && again.items.size() == first.items.size(), L"first run: an existing first.txt is read, never rewritten");
        FirstRunResult r3 = EnsureFirstSnapshot(s, w.items, L"19045", L"machine-b", &again);
        check(r3.foreign, L"first run: a first.txt from another machine GUID is flagged");
        std::wstring root2 = NewTempDir(L"corrupt");
        Store s2(root2);
        s2.WriteNew(L"first.txt", "not a snapshot", &err);
        FirstRunResult r4 = EnsureFirstSnapshot(s2, w.items, L"19045", L"machine-a", &again);
        check(r4.corrupt && r4.wrote && s2.List(L"first.txt.corrupt-*").size() == 1,
              L"first run: a corrupt first.txt is renamed aside (kept), and a new one written");
    }

    // ---- apply -> undo round trip on the fake ----------------------------------------------------------------
    auto freshWorld = [&](World* fw) { std::wstring e; return MakeWorld(fixture, &kb, fw, &e); };
    {
        World a;
        freshWorld(&a);
        auto before = CfgMap(a.fx.backend);
        Store s(NewTempDir(L"apply"));
        s.fixedStamp = L"20261111-101500";
        Plan p = PlanFor(LV_STRIPPED, TAB_SERVICES, a);
        TickUndetectable(&p);
        ApplyEnv env = EnvFor(&a.fx.backend, &s);
        ApplyResult r;
        bool ok = RunApply(p, env, &r);
        check(ok && r.ok == p.Included() && r.failed == 0 && r.skipped == 0,
              L"apply (Stripped, fake): " + std::to_wstring(r.ok) + L" of " + std::to_wstring(p.Included()) + L" ok" +
                  (r.error.empty() ? L"" : L": " + r.error));
        check(s.Exists(L"snap-20261111-101500.txt") && s.Exists(L"recovery-20261111-101500.zip") &&
                  s.Exists(L"recovery\\20261111-101500\\restore.cmd"),
              L"apply wrote the snapshot, the recovery folder and its zip first");
        const SvcItem* ms = nullptr;
        for (auto& i : a.fx.backend.Items()) if (i.name == L"MapsBroker") ms = &i;
        check(ms && ms->cfg.start == 4 && ms->cfg.delayed == 1, L"MapsBroker is Disabled and its DelayedAutostart left alone");
        auto logs = LoadLogs(s, nullptr);
        check(logs.size() == 1 && logs[0].hasE && FindInterrupted(logs) < 0 && FindUndoable(logs) == 0,
              L"the undo log is complete (B ... E) and offers Undo");
        std::vector<size_t> order = ApplyOrder(p);
        int xa = -1, xg = -1;
        for (size_t k = 0; k < order.size(); k++) {
            if (p.rows[order[k]].name == L"SSDPSRV") xa = (int)k;
            if (p.rows[order[k]].name == L"upnphost") xg = (int)k;
        }
        check(xa >= 0 && xg >= 0 && xg < xa, L"apply order: a dependent (upnphost) is disabled before what it depends on (SSDPSRV)");
        Plan u = BuildUndo(logs[0], a.fx.backend, a.ctx);
        ApplyResult ru;
        ok = RunApply(u, env, &ru);
        auto after = CfgMap(a.fx.backend);
        check(ok && ru.failed == 0 && after == before, L"undo restores every start value exactly (" + std::to_wstring(ru.ok) + L" rows)");
        logs = LoadLogs(s, nullptr);
        check(logs.size() == 2 && FindUndoable(logs) < 0, L"after the undo, nothing is left to undo");
        // Drift: something else changes an item Hadron set.
        Plan p2 = PlanFor(LV_BLOAT, TAB_SERVICES, a);
        RunApply(p2, env, &r);
        logs = LoadLogs(s, nullptr);
        auto last = LastApplied(logs);
        check(last.count(L"mapsbroker") && last[L"mapsbroker"].start == 4, L"drift: LastApplied records what Hadron set");
        WriteOp back{3, -1};
        a.fx.backend.SetStart(L"MapsBroker", back, Identity::Admin, &err);
        Plan u2 = BuildUndo(logs[FindUndoable(logs)], a.fx.backend, a.ctx);
        const PlanRow* mr = RowOf(u2, L"MapsBroker");
        check(mr && !mr->include && mr->warning.find(L"changed since") != std::wstring::npos,
              L"undo: a row changed by something else since Hadron set it is unticked with a warning");
    }

    // ---- faults ----------------------------------------------------------------------------------------------
    {
        World a;
        freshWorld(&a);
        Store s(NewTempDir(L"faults"));
        s.fixedStamp = L"20261111-101600";
        Plan p = PlanFor(LV_STRIPPED, TAB_SERVICES, a);
        TickUndetectable(&p);
        a.fx.backend.faults.deny = {L"upnphost"};
        a.fx.backend.faults.vanish = {L"lfsvc"};
        a.fx.backend.faults.changeOnQuery = {L"retaildemo"};
        a.fx.backend.faults.verifyMismatch = {L"wisvc"};
        ApplyEnv env = EnvFor(&a.fx.backend, &s);
        ApplyResult r;
        RunApply(p, env, &r);
        auto line = [&](const std::wstring& prefix) {
            for (auto& l : r.lines) if (l.rfind(prefix, 0) == 0) return true;
            return false;
        };
        check(line(L"fail upnphost") && line(L"fail wisvc"), L"faults: access denied and a write Windows did not keep -> fail");
        check(line(L"skip lfsvc") && line(L"skip RetailDemo"), L"faults: vanished and changed-since-plan items -> skipped");
        check(line(L"skip SSDPSRV"), L"faults: SSDPSRV is skipped because its dependent (upnphost) could not be changed");
        auto logs = LoadLogs(s, nullptr);
        check(logs.size() == 1 && logs[0].hasE, L"faults: per-item errors do not stop the run; the log ends with E");

        World b;
        freshWorld(&b);
        Store s2(NewTempDir(L"flush"));
        Plan p2 = PlanFor(LV_BLOAT, TAB_SERVICES, b);
        ApplyEnv e2 = EnvFor(&b.fx.backend, &s2);
        e2.failFlushAfter = 3;
        ApplyResult r2;
        bool ok = RunApply(p2, e2, &r2);
        check(!ok && r2.aborted && b.fx.backend.faults.setStartCalls == 0, L"a log flush failure before any change aborts with nothing changed");
        World c;
        freshWorld(&c);
        Store s3(NewTempDir(L"flush2"));
        Plan p3 = PlanFor(LV_BLOAT, TAB_SERVICES, c);
        ApplyEnv e3 = EnvFor(&c.fx.backend, &s3);
        e3.failFlushAfter = 2 + (int)ApplyOrder(p3).size() + 2;   // header, B, P rows, then row 1's I and D
        ApplyResult r3;
        ok = RunApply(p3, e3, &r3);
        check(!ok && r3.aborted && c.fx.backend.faults.setStartCalls == 1,
              L"a log flush failure mid-run stops before the next write (no SCM call without its I record)");
        World d;
        freshWorld(&d);
        Store s4(NewTempDir(L"cancel"));
        Plan p4 = PlanFor(LV_BLOAT, TAB_SERVICES, d);
        ApplyEnv e4 = EnvFor(&d.fx.backend, &s4);
        std::atomic<bool> cancel{true};
        e4.cancel = &cancel;
        ApplyResult r4;
        RunApply(p4, e4, &r4);
        auto l4 = LoadLogs(s4, nullptr);
        check(r4.cancelled && r4.ok == 0 && l4.size() == 1 && l4[0].hasE, L"cancel between items: nothing more changes, the log is closed");
        World e;
        freshWorld(&e);
        Store s5(NewTempDir(L"locked"));
        ApplyLock held;
        held.Acquire(LockName(), &err);
        ApplyResult r5;
        ok = RunApply(PlanFor(LV_BLOAT, TAB_SERVICES, e), EnvFor(&e.fx.backend, &s5), &r5);
        check(!ok && r5.aborted && e.fx.backend.faults.setStartCalls == 0, L"apply lock held elsewhere: refused, nothing changed");
    }

    // ---- crash after BEGIN k -> Resume, -> Undo --------------------------------------------------------------
    for (int mode = 0; mode < 2; mode++) {
        World a;
        freshWorld(&a);
        auto before = CfgMap(a.fx.backend);
        Store s(NewTempDir(mode ? L"crash-undo" : L"crash-resume"));
        s.fixedStamp = L"20261111-101700";
        Plan p = PlanFor(LV_BLOAT, TAB_SERVICES, a);
        ApplyEnv env = EnvFor(&a.fx.backend, &s);
        env.crashAfterBegin = 3;
        ApplyResult r;
        bool ok = RunApply(p, env, &r);
        auto logs = LoadLogs(s, nullptr);
        int k = FindInterrupted(logs);
        check(!ok && r.crashed && k == 0 && logs[0].Row(3) && logs[0].Row(3)->hasI && !logs[0].Row(3)->hasD,
              L"crash after BEGIN 3: the log has I for row 3 and no E -> interrupted");
        std::vector<size_t> order = ApplyOrder(p);
        env.crashAfterBegin = -1;
        a.items = Folded(a.fx.backend);
        if (mode == 0) {
            Plan rp = BuildResume(logs[0], a.fx.backend, a.items, a.ctx);
            ApplyResult rr;
            ok = RunApply(rp, env, &rr);
            bool all = true;
            for (size_t i : order) {
                const PlanRow& row = p.rows[i];
                for (auto& it : a.fx.backend.Items()) if (it.name == row.name && it.cfg.start != row.after.start) all = false;
            }
            logs = LoadLogs(s, nullptr);
            check(ok && rr.failed == 0 && rp.alreadySet >= 1 && all && FindInterrupted(logs) < 0,
                  L"Resume: finishes the remaining rows (row 3 already applied), and the interrupted log is resolved");
        } else {
            Plan up = BuildUndo(logs[0], a.fx.backend, a.ctx);
            ApplyResult ru;
            ok = RunApply(up, env, &ru);
            logs = LoadLogs(s, nullptr);
            check(ok && ru.failed == 0 && up.rows.size() == 3 && CfgMap(a.fx.backend) == before && FindInterrupted(logs) < 0,
                  L"Undo after a crash: puts back rows 1-3 (including the one in progress); all values as before");
        }
    }

    // ---- TrustedInstaller marking and apply ------------------------------------------------------------------
    {
        World a;
        freshWorld(&a);
        Store s(NewTempDir(L"ti"));
        Plan p = BuildPreset(LV_STRIPPED, TAB_SERVICES, a.items, a.ctx);
        MarkTi(&p, a.fx.backend, true);
        Tick(&p, L"dmwappushservice");
        const PlanRow* dm = RowOf(p, L"dmwappushservice");
        check(dm && dm->needsTi && dm->include, L"TI marking: with the toggle on the row is included and marked");
        FakeTi ti;
        ti.probe = &s;
        ApplyEnv env = EnvFor(&a.fx.backend, &s);
        env.ti = &ti;
        env.tiOn = true;
        ApplyResult r;
        bool ok = RunApply(p, env, &r);
        auto logs = LoadLogs(s, nullptr);
        const UndoRow* ur = nullptr;
        for (auto& row : logs[0].rows) if (row.name == L"dmwappushservice") ur = &row;
        check(ok && r.failed == 0 && ti.acquired && ti.impersonations == 1 && ti.released,
              L"TI apply: the token is taken once, used for the marked row only, then released");
        check(ti.flagSeen && ti.writeRefused, L"TI apply: Hadron's own file writes are refused while impersonating");
        check(ur && ur->identity == L"ti" && ur->result == L"ok" && ur->text == L"ti" && logs[0].tiStarted && logs[0].tiStopped,
              L"TI apply: the log records the identity and that TrustedInstaller was started and stopped again");
        World b;
        freshWorld(&b);
        Store s2(NewTempDir(L"ti-off"));
        Plan p2 = BuildPreset(LV_STRIPPED, TAB_SERVICES, b.items, b.ctx);
        MarkTi(&p2, b.fx.backend, true);   // marked with the toggle on, then applied with it off
        Tick(&p2, L"dmwappushservice");
        ApplyResult r2;
        RunApply(p2, EnvFor(&b.fx.backend, &s2), &r2);
        bool failed = false;
        for (auto& l : r2.lines) if (l.rfind(L"fail dmwappushservice", 0) == 0) failed = true;
        check(failed, L"TI apply: a marked row with the toggle off fails instead of trying as admin");
        Plan p3 = BuildPreset(LV_STRIPPED, TAB_SERVICES, b.items, b.ctx);
        PlanRow tiRow;
        tiRow.name = L"TrustedInstaller";
        p3.rows.push_back(tiRow);
        MarkTi(&p3, b.fx.backend, true);
        check(RowOf(p3, L"TrustedInstaller") && !RowOf(p3, L"TrustedInstaller")->blocked.empty(),
              L"TI marking: the TrustedInstaller service itself is blocked while the toggle is on");
    }

    // ---- the interrupted fixture --------------------------------------------------------------------------
    {
        std::vector<std::wstring> problems;
        auto logs = LoadLogs(Store(fixture + L"\\interrupted"), &problems);
        check(logs.size() == 1 && FindInterrupted(logs) == 0 && TiLeftRunning(logs),
              L"interrupted fixture: offers Undo/Resume and notices TrustedInstaller left running");
    }

    // ---- live, read-only ------------------------------------------------------------------------------------
    {
        auto live = MakeLiveBackend();
        std::vector<SvcItem> items;
        bool ok = live->Enumerate(&items, &err);
        check(ok && !items.empty(), L"live backend (read-only): enumerated " + std::to_wstring(items.size()) + L" services and drivers" +
                                        (ok ? L"" : L": " + err));
        int drivers = 0, readErrors = 0;
        for (auto& i : items) { if (IsDriverType(i.type)) drivers++; if (!i.readError.empty()) readErrors++; }
        info(L"live: " + std::to_wstring(drivers) + L" drivers, " + std::to_wstring(readErrors) + L" items with partial read errors, build " +
             live->Build());
        std::wstring werr;
        bool wrote = live->SetStart(L"HadronSelftestNoSuchService", WriteOp{4, -1}, Identity::Admin, &werr);
        check(!wrote && werr.find(L"not armed") != std::wstring::npos, L"live backend refuses writes until a confirmed plan arms it");
    }

    wprintf(L"  [info] temp data dirs are under %ls (left in place)\n", Parent(tmpRoot).c_str());
    wprintf(L"services selftest: %ls (%d checks, %d failed)\n", fails ? L"FAIL" : L"PASS", checks, fails);
    fflush(stdout);
    return fails ? 1 : 0;
}

// ============================================================================================
int RunDumpPlan(const std::vector<std::wstring>& args) {
    std::wstring lv = ArgValue(args, L"--dump-plan"), tab = ArgValue(args, L"--tab"), fx = ArgValue(args, L"--fixture"),
                 out = ArgValue(args, L"--out");
    int level = LevelFromName(lv);
    if (level == -3 || (tab != L"services" && tab != L"drivers") || fx.empty()) {
        fwprintf(stderr, L"usage: --dump-plan <default|stock|bloat|stripped|minimum> --tab <services|drivers> --fixture <dir> [--out <file>]\n");
        return 2;
    }
    std::wstring err;
    std::string text = DumpFixturePlan(fx, level, tab == L"drivers" ? TAB_DRIVERS : TAB_SERVICES, &err);
    if (text.empty()) { fwprintf(stderr, L"dump-plan: %ls\n", err.c_str()); return 1; }
    if (!out.empty()) {
        if (!WriteAll(out, text)) { fwprintf(stderr, L"dump-plan: could not write %ls\n", out.c_str()); return 1; }
        return 0;
    }
    fflush(stdout);
    _setmode(_fileno(stdout), _O_BINARY);
    fwrite(text.data(), 1, text.size(), stdout);
    fflush(stdout);
    return 0;
}

// ============================================================================================
// --live-test: CI only, on a dummy service the workflow created with sc.exe (HadronTest*). Anything else is refused.
int RunLiveTest(const std::vector<std::wstring>& args) {
    std::wstring name = ArgValue(args, L"--live-test");
    bool useTi = HasArg(args, L"--ti");
    wprintf(L"services live test: %ls%ls\n", name.c_str(), useTi ? L" (TrustedInstaller)" : L"");
    if (name.rfind(L"HadronTest", 0) != 0 || name.size() > 64) {
        wprintf(L"refused: --live-test only runs on a dummy service named HadronTest* that the test created\n");
        return 2;
    }
    if (!sys::IsElevated()) { wprintf(L"refused: not elevated\n"); return 3; }
    auto b = MakeLiveBackend();
    SvcItem item;
    std::wstring err;
    if (!b->Query(name, &item, &err)) { wprintf(L"FAIL: %ls not found: %ls\n", name.c_str(), err.c_str()); return 1; }
    if (IsDriverType(item.type)) { wprintf(L"refused: %ls is a driver\n", name.c_str()); return 2; }
    std::wstring root = ArgValue(args, L"--data-dir");
    if (root.empty()) root = NewTempDir(L"live");
    Store st(root);
    if (!st.EnsureRoot(false, &err)) { wprintf(L"FAIL: data dir: %ls\n", err.c_str()); return 1; }
    wprintf(L"  data dir %ls\n", root.c_str());
    std::unique_ptr<TiProvider> ti = useTi ? MakeLiveTi(b.get()) : nullptr;
    Ctx c;
    c.build = b->Build();
    c.tiOn = useTi;
    const StartCfg original = item.cfg;
    int fails = 0;
    auto run = [&](const Plan& p, const wchar_t* what) {
        ApplyEnv env;
        env.backend = b.get();
        env.store = &st;
        env.ti = ti.get();
        env.tiOn = useTi;
        env.build = c.build;
        env.machine = b->MachineGuid();
        ApplyResult r;
        b->Arm();
        bool ok = RunApply(p, env, &r);
        b->Disarm();
        for (auto& l : r.lines) wprintf(L"    %ls\n", l.c_str());
        if (!r.tiNote.empty()) wprintf(L"    %ls\n", r.tiNote.c_str());
        if (!ok || r.failed) { wprintf(L"  FAIL %ls: %ls\n", what, r.error.c_str()); fails++; }
        return ok && !r.failed;
    };
    auto verify = [&](StartCfg want, const wchar_t* what) {
        SvcItem now;
        std::wstring e;
        bool ok = b->Query(name, &now, &e) && now.cfg.start == want.start && now.cfg.Delayed() == want.Delayed();
        wprintf(L"  [%ls] %ls: %ls\n", ok ? L"PASS" : L"FAIL", what, ok ? StartLabel(now.cfg, false).c_str() : e.c_str());
        if (!ok) fails++;
    };
    for (StartCfg target : {StartCfg{original.start == 4 ? 3 : 4, -1}, StartCfg{2, 1}}) {
        std::vector<SvcItem> items{item};
        Plan p = BuildSingle(item, target, items, c);
        if (useTi) for (auto& r : p.rows) r.needsTi = true;   // exercise the impersonation path
        if (p.Included() != 1) { wprintf(L"  FAIL: plan has %d rows: %ls\n", p.Included(), p.rows.empty() ? L"" : p.rows[0].blocked.c_str()); return 1; }
        if (!run(p, L"apply")) break;
        verify(target, L"applied");
        auto logs = LoadLogs(st, nullptr);
        int k = FindUndoable(logs);
        if (k < 0) { wprintf(L"  FAIL: nothing to undo\n"); fails++; break; }
        Plan u = BuildUndo(logs[k], *b, c);
        if (useTi) for (auto& r : u.rows) r.needsTi = true;
        if (!run(u, L"undo")) break;
        verify(original, L"undone");
        b->Query(name, &item, &err);
    }
    wprintf(L"services live test: %ls\n", fails ? L"FAIL" : L"PASS");
    fflush(stdout);
    return fails ? 1 : 0;
}

}  // namespace hx::services
