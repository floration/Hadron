// modules/services/selftest.cpp — --selftest, --dump-plan and --live-test (see cli.h).
//
// The selftest runs against the fake backend and a fresh temp data dir only: it never touches real services
// or %ProgramData%. Its one live call is a read-only Enumerate, plus a check that the unarmed live backend
// refuses a write before reaching the SCM (on a service name that does not exist).
#include "modules/services/cli.h"
#include "core/sys/sys.h"
#include "modules/services/measure.h"
#include "modules/services/plan.h"
#include "modules/services/recovery.h"
#include "modules/services/selftest_evidence.h"
#include "modules/services/ti.h"

#include <fcntl.h>
#include <io.h>
#include <aclapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <functional>
#include <map>
#include <random>

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

// P1: logs[i] for a check that may already have failed (no data folder, e.g. a release build under Wine): an empty
// log instead of reading past the vector, so the selftest reports its failures instead of crashing.
template <class T>
T& At(std::vector<T>& v, int i) {
    static T none;
    if (i >= 0 && i < (int)v.size()) return v[(size_t)i];
    none = T{};
    return none;
}

// Review F1: --out of the headless modes runs in an elevated exe, so it never replaces or follows anything: CREATE_NEW
// with FILE_FLAG_OPEN_REPARSE_POINT, and an existing path is refused with that reason.
bool WriteNewOut(const std::wstring& path, const std::string& data, std::wstring* err) {
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        *err = e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS
                   ? path + L" already exists; Hadron never overwrites an --out file, so pick a new name"
                   : L"could not create " + path + L": " + sys::ErrorText(e);
        return false;
    }
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size() && FlushFileBuffers(h);
    if (!ok) *err = L"could not write " + path + L": " + sys::ErrorText(GetLastError());
    CloseHandle(h);
    return ok;
}

bool WriteAll(const std::wstring& path, const std::string& data) {   // test outputs only (goldens)
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(h);
    return ok;
}

// Where the test data dirs go: %SystemRoot%\Temp, whose ancestors only administrators can rename or re-permission, as
// EnsureRoot requires of every folder above a data folder (mini-analyze A1). %TEMP% sits in the user's profile, which
// the user (a non-admin principal) may rename, so EnsureRoot refuses it; it is the fallback only where there is no
// %SystemRoot%\Temp. No trailing backslash.
// True when a folder's own DACL passes the data folder's ancestor test (Store::RenameAccessProblem). Some machines
// (GitHub's windows-latest image) give BUILTIN\Users full control of %SystemRoot%\Temp, so it can't hold a test data
// folder there: EnsureRoot rightly refuses it.
static bool AncestorOk(const std::wstring& dir) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr,
                              &sd) != ERROR_SUCCESS)
        return false;
    bool ok = Store::RenameAccessProblem(sd).empty();
    LocalFree(sd);
    return ok;
}

std::wstring TestTempBase() {
    bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    wchar_t win[MAX_PATH];
    UINT w = GetSystemWindowsDirectoryW(win, MAX_PATH);
    if (w && w < MAX_PATH && DirExists(std::wstring(win) + L"\\Temp") && (wine || AncestorOk(std::wstring(win) + L"\\Temp")))
        return std::wstring(win) + L"\\Temp";
    // Else %ProgramData%, the parent of Hadron's own default data folder, which the same test must accept.
    PWSTR pd = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd)) && pd) {
        std::wstring b = pd;
        CoTaskMemFree(pd);
        if (DirExists(b)) return b;
    } else if (pd) {
        CoTaskMemFree(pd);
    }
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring base = (n && n < MAX_PATH) ? std::wstring(tmp) : L".\\";
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    return base;
}

// A fresh, empty temp folder for one test's data dir, made the way Hadron makes its own (Store::EnsureRoot: owner
// Administrators, protected DACL), so RunApply's owner check accepts it. Left in place; nothing is deleted.
std::wstring NewTempDir(const std::wstring& tag) {
    const std::wstring base = TestTempBase();
    for (int i = 0; i < 1000; i++) {
        // In a folder of its own, owned by Administrators as the holding-folder check requires (%TEMP% is owned by
        // the user, and an elevated token's default owner may be the user too).
        std::wstring outer = base + L"\\HadronSvcTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + tag + L"-" + std::to_wstring(i);
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BA", SDDL_REVISION_1, &sd, nullptr)) return L"";
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        BOOL made = CreateDirectoryW(sys::ToExtended(outer).c_str(), &sa);
        LocalFree(sd);
        if (!made) continue;
        std::wstring d = outer + L"\\data", e;
        if (Store(d).EnsureRoot(true, &e)) return d;
        wprintf(L"  [info] could not make the test data dir %ls: %ls\n", d.c_str(), e.c_str());
        return L"";
    }
    return L"";
}

// The echo-only variant of a generated restore.cmd that CI runs on real cmd.exe (windows-latest): every reg call
// becomes "call :dry reg ...", which prints it and sets errorlevel 0; Select\Current reads as 1; and one extra row
// with ( ) in its name is fed to :put, to prove the subroutine survives the characters that broke the old block
// form, although CmdSafeName keeps such names out of real scripts. Nothing touches the registry.
std::string DryRunCmd(const std::string& cmd) {
    std::string out;
    size_t a = 0;
    bool injected = false;
    while (a < cmd.size()) {
        size_t b = cmd.find("\r\n", a);
        std::string line = cmd.substr(a, b == std::string::npos ? std::string::npos : b - a);
        a = b == std::string::npos ? cmd.size() : b + 2;
        if (line.rfind("for /f ", 0) == 0) {
            size_t p = line.find("('"), q = line.find("')");
            // :put's Start read answers 0x%DRY% (3 unless a row sets it), Select\Current answers 1.
            const char* echo = line.find("/v Start") != std::string::npos ? "('echo Start REG_DWORD 0x%DRY%')" : "('echo Current REG_DWORD 0x1')";
            if (p != std::string::npos && q != std::string::npos) line = line.substr(0, p) + echo + line.substr(q + 2);
        }
        if (!injected && line == "reg unload HKLM\\HXOFF") {
            out += "set \"DRY=0\"\r\ncall :put \"Dry Boot Now\" 3 -\r\nset \"DRY=3\"\r\n";
            out += "call :put \"Intel(R) Dry Run (x86)\" 3 -\r\n";
            injected = true;
        }
        if (line == "setlocal EnableExtensions") { out += line + "\r\nset \"DRY=3\"\r\n"; continue; }
        if (line.rfind("reg ", 0) == 0) line = "call :dry " + line;
        out += line + "\r\n";
    }
    out += ":dry\r\necho DRYRUN %*\r\nexit /b 0\r\n";
    return out;
}

bool IsReparse(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool MakeJunction(const std::wstring& link, const std::wstring& target);

// A junction, or failing that a directory symbolic link, under dir -> target (an existing folder), that the
// system reports as a reparse point. For the EnsureRoot test only, inside the test's own temp folder. Returns its
// path, or "" when neither works here.
std::wstring MakeLink(const std::wstring& dir, const std::wstring& target, std::wstring* how) {
    std::wstring j = dir + L"\\junction";
    if (MakeJunction(j, target) && IsReparse(j)) { *how = L"junction"; return j; }
    std::wstring s = dir + L"\\symlink";
    if (CreateSymbolicLinkW(s.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2 /* ALLOW_UNPRIVILEGED_CREATE */) && IsReparse(s)) {
        *how = L"symbolic link";
        return s;
    }
    *how = L"neither a junction nor a symbolic link that reports FILE_ATTRIBUTE_REPARSE_POINT";
    return L"";
}

bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring sub = L"\\??\\" + target;
    std::vector<BYTE> buf(16 + (sub.size() + 1 + target.size() + 1) * 2, 0);
    auto put16 = [&](size_t at, size_t v) { buf[at] = (BYTE)(v & 0xFF); buf[at + 1] = (BYTE)(v >> 8); };
    DWORD tag = 0xA0000003;   // IO_REPARSE_TAG_MOUNT_POINT
    memcpy(buf.data(), &tag, 4);
    put16(4, buf.size() - 8);
    put16(8, 0);
    put16(10, sub.size() * 2);
    put16(12, (sub.size() + 1) * 2);
    put16(14, target.size() * 2);
    memcpy(buf.data() + 16, sub.c_str(), sub.size() * 2);
    memcpy(buf.data() + 16 + (sub.size() + 1) * 2, target.c_str(), target.size() * 2);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, 0x000900A4 /* FSCTL_SET_REPARSE_POINT */, buf.data(), (DWORD)buf.size(), nullptr, 0, &ret, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

// A TrustedInstaller provider that is only the service half (TiService) over the fake backend: what LiveTi does
// with the service, without the token work.
struct ServiceOnlyTi : TiProvider {
    TiService svc;
    explicit ServiceOnlyTi(Backend* b) : svc(b) {}
    bool Acquire(const std::atomic<bool>* cancel, std::wstring* err) override { svc.cancel = cancel; SvcItem t; return svc.Ensure(&t, err); }
    bool StartedService() const override { return svc.Started(); }
    bool Impersonate(std::wstring* identity, std::wstring*) override { *identity = L"ti"; return true; }
    void Revert() override {}
    bool Release(std::wstring* note) override { return svc.StopIfStarted(note); }
};

SvcItem Mk(const wchar_t* name, int start, std::vector<std::wstring> deps, DWORD type = 0x10) {
    SvcItem s;
    s.name = name;
    s.type = type;
    s.kind = KindOf(type);
    s.cfg = StartCfg{start, -1};
    s.regStart = start;
    s.deps = std::move(deps);
    return s;
}

// The dependency invariant, computed without DependencyCheck's code (Analyze A9): against the end state of the
// ticked rows, no ticked row disables an item that an item enabled at the end depends on (by name or +group), and
// no ticked row raises an item whose dependency (or every member of a +group) ends Disabled. Only the row the user
// just toggled (toggled) or a row the user kept (depKept) may break it, and then it must show a warning. A ticked
// row with a warning is a violation in every other case. "" when it holds, else what broke.
std::wstring DepViolation(const Plan& p, const std::vector<SvcItem>& items, int toggled = -1) {
    std::map<std::wstring, int> fin;
    for (auto& s : items) fin[Lower(s.name)] = s.cfg.start;
    for (auto& r : p.rows) if (r.include && r.blocked.empty()) fin[Lower(r.name)] = r.after.start;
    auto endOf = [&](const std::wstring& n) { auto it = fin.find(Lower(n)); return it == fin.end() ? -1 : it->second; };
    for (size_t k = 0; k < p.rows.size(); k++) {
        const PlanRow& r = p.rows[k];
        if (!r.include || !r.blocked.empty()) continue;
        bool userKept = (int)k == toggled || r.depKept;
        if (!r.depWarning.empty() && !userKept) return r.name + L": ticked with a dependency warning";
        bool bad = false;
        const SvcItem* self = nullptr;
        for (auto& s : items) if (IEq(s.name, r.name)) self = &s;
        if (r.after.start == 4) {
            for (auto& s : items) {
                if (s.kind == Kind::UserInstance || IEq(s.name, r.name) || endOf(s.name) == 4) continue;
                for (auto& d : s.deps) {
                    if (IEq(d, r.name)) bad = true;
                    if (self && !self->group.empty() && d.size() > 1 && d[0] == L'+' && IEq(d.substr(1), self->group)) bad = true;
                }
            }
        } else if (r.after.start < r.before.start) {
            for (auto& d : self ? self->deps : r.deps) {
                if (d.size() > 1 && d[0] == L'+') {
                    int m = 0, off = 0;
                    for (auto& s : items) if (IEq(s.group, d.substr(1))) { m++; if (endOf(s.name) == 4) off++; }
                    if (m && m == off) bad = true;
                } else if (endOf(d) == 4) {
                    bad = true;
                }
            }
        }
        if (bad && !(userKept && !r.depWarning.empty())) return r.name + L": ticked, breaks a dependency, no warning";
    }
    return L"";
}

PlanRow Lower4(const SvcItem& s, bool include = true) {
    PlanRow r;
    r.name = s.name;
    r.group = s.group;
    r.before = s.cfg;
    r.after = StartCfg{4, s.cfg.delayed};
    r.op = WriteOp{4, -1};
    r.deps = s.deps;
    r.include = include;
    return r;
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
    std::shared_ptr<const SxsStock> sxs;   // the fixture's sxs\ manifests, when it has them
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
    if (std::unique_ptr<SxsReader> r = w->fx.backend.MakeSxs()) w->sxs = std::make_shared<const SxsStock>(ScanSxs(*r));
    w->ctx.sxs = w->sxs.get();
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
    MarkTi(&p, w.fx.backend, w.ctx.tiOn, w.items);
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
    bool Acquire(const std::atomic<bool>*, std::wstring*) override { acquired = true; started = true; return true; }
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
    e.requireFirst = false;   // most temp data dirs have no first.txt; the R12 check turns it back on
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
int RunSelftest(const std::vector<std::wstring>& args, const UiChecks& ui) {
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
        std::wstring ev0;
        HwInfo hok;
        hok.ok = true;
        check(HwEvidence(hok, L"bogus:x", L"x", &ev0) == 0,
              L"hw evidence: an unknown hint fails closed (kept, never ticked)");
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
        check(CompanyPathAllowed(L"C:\\Windows\\System32\\svchost.exe") && CompanyPathAllowed(L"\\\\?\\D:\\x.sys") &&
                  !CompanyPathAllowed(L"\\\\srv\\share\\svc.exe") && !CompanyPathAllowed(L"\\\\?\\UNC\\srv\\share\\svc.exe") &&
                  !CompanyPathAllowed(L"\\\\.\\pipe\\x") && !CompanyPathAllowed(L"svc.exe"),
              L"R3: the Company lookup reads only local drive paths, never UNC, \\\\?\\UNC\\ or device paths");
        check(CompanyAttrsAllowed(FILE_ATTRIBUTE_ARCHIVE) && !CompanyAttrsAllowed(FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) &&
                  !CompanyAttrsAllowed(FILE_ATTRIBUTE_RECALL_ON_OPEN) && !CompanyAttrsAllowed(FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_ARCHIVE) &&
                  !CompanyAttrsAllowed(INVALID_FILE_ATTRIBUTES) &&
                  !CompanyAttrsAllowed(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_ARCHIVE),   // N1: a link is a leaf
              L"R3: the Company lookup never opens a cloud placeholder or offline file (RECALL_ON_DATA_ACCESS, RECALL_ON_OPEN, OFFLINE)");
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
        check(n >= 60, L"KB has at least the 60-entry seed (" + std::to_wstring(n) + L")");
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
        bool stockSourced = true;
        for (auto& e : kb.entries)
            if (e.stock >= 0) {
                stock++;
                if ((e.stockBasis != L"published" && e.stockBasis != L"owner-measured") ||
                    (e.stockBasis == L"published" && e.stockNote.find(L"https://") == std::wstring::npos) || IsStorageDriver(e.name))
                    stockSourced = false;
            }
        const KbEntry* ws = kb.Find(L"WSearch");
        check(stock > 0 && stockSourced && ws && ws->stock == 2 && ws->stockDelayed == 1,
              L"KB stock table: every stock value has a published (with URL) or owner-measured basis, none on a storage "
              L"driver; WSearch is Auto (delayed)");
        info(L"KB: " + std::to_wstring(n) + L" entries, " + std::to_wstring(stock) + L" with a Windows stock value");
    }

    // ---- A5 first: the data folder owner check (before any test makes a data dir) ----------------------------
    {
        bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        std::wstring d;
        bool made = false;
        for (int i = 0; i < 1000 && !made; i++) {   // default ACL, the token's default owner
            d = TestTempBase() + L"\\HadronSvcTest-" + std::to_wstring(GetCurrentProcessId()) + L"-owner-" + std::to_wstring(i);
            made = CreateDirectoryW(d.c_str(), nullptr) != FALSE;
        }
        // d has this token's default owner (Administrators when elevated on Windows). Inside it, a root and a holding
        // folder owned by this process's own user (allowed to set: it is the token user).
        auto ownByMe = [&](const std::wstring& path) {
            HANDLE tok = nullptr;
            bool ok = false;
            std::vector<BYTE> buf(256);
            DWORD need = 0;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
                if (GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &need))
                    ok = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                               reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
                CloseHandle(tok);
            }
            return ok;
        };
        std::wstring e, root = d + L"\\root", hold = d + L"\\hold";
        bool rootMade = made && CreateDirectoryW(root.c_str(), nullptr) && ownByMe(root);
        bool ok = rootMade && Store(root).EnsureRoot(true, &e);
        if (!made) info(L"A5: could not create " + d);
        else if (!rootMade) info(L"A5: could not give a test folder this process's own user as owner: owner checks not exercised");
        else if (ok) info(L"A5: a root owned by this process's user passes?! (" + root + L")");
        check(!rootMade || (!ok && e.find(L"owned by") != std::wstring::npos),
              L"A5: a data folder owned by someone other than Administrators or SYSTEM is refused: " + e);
        std::wstring e2;
        bool holdMade = made && CreateDirectoryW(hold.c_str(), nullptr) && ownByMe(hold);
        bool ok2 = holdMade && Store(hold + L"\\services").EnsureRoot(true, &e2);
        // Under Wine every folder, C:\ included, is owned by this process's user, so the refusal comes at the first
        // folder above the data folder (A1) instead of at the holding folder: still refused, still nothing made.
        check(!holdMade || (!ok2 &&
                            ((e2.find(L"the folder that holds") != std::wstring::npos && e2.find(hold) != std::wstring::npos) ||
                             (wine && e2.find(L"a folder above Hadron's data folder") != std::wstring::npos)) &&
                            GetFileAttributesW((hold + L"\\services").c_str()) == INVALID_FILE_ATTRIBUTES),
              L"A5 r2: a holding folder (like %ProgramData%\\Hadron) owned by someone else is refused before the data folder is made: " + e2);
        if (wine) {
#ifdef HX_SERVICES_TEST_HOOKS
            Store::AcceptTokenUserAsOwner() = true;
            info(L"A5: Wine ignores the owner in a security descriptor, so from here on this process's own user passes the owner check");
#else
            info(L"A5: under Wine every folder is owned by this process's user, and this build has no test hooks (CMake "
                 L"-DHADRON_SERVICES_TEST_HOOKS=ON): every check that needs a data folder will fail. Use a test build.");
#endif
        }
    }

    // ---- fixture: snapshot round trip, hw --------------------------------------------------------------
    std::wstring fixture = FindFixture(args);
    check(!fixture.empty(), L"fixture folder found (" + (fixture.empty() ? std::wstring(L"none") : fixture) + L")");
    if (fixture.empty()) { wprintf(L"services selftest: FAIL (%d checks, %d failed)\n", checks, fails); return 1; }
    std::wstring golden = ArgValue(args, L"--golden");
    if (golden.empty()) golden = Parent(Parent(fixture)) + L"\\golden\\services";
    World w;
    check(MakeWorld(fixture, &kb, &w, &err), L"fixture loads (snapshot.txt, hw.txt, first.txt)" + (err.empty() ? L"" : L": " + err));

    // ---- WinSxS manifest stock (fix/svcdrv-stock): the scanner on hand-written XML, the fake reader, the cache ----------
    {
        SxsScan a;
        ScanManifestXml("<assembly><assemblyIdentity name='C1'/>"
                        "<serviceData name=\"Svc&amp;A\" start='delayedAuto'/>"
                        "<serviceData name=\"B\" start=\"bogus\"/>"
                        "<!-- <serviceData name=\"Cm\" start=\"disabled\"/> -->"
                        "<registryKey keyName=\"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\Drv1\">"
                        "<registryValue name=\"Start\" valueType=\"REG_DWORD\" value=\"0x00000000\"/></registryKey>"
                        "<registryKey keyName=\"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\Drv1\\Parameters\">"
                        "<registryValue name=\"Start\" value=\"4\"/></registryKey>"
                        "<registryValue name=\"Start\" value=\"4\"/>"
                        "<registryKey keyName=\"HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\StorX\">"
                        "<registryValue name=\"DelayedAutostart\" value=\"1\"/></registryKey>"
                        "<serviceData name=\"StorX\" start=\"auto\"/></assembly>", &a);
        auto at = [&](const wchar_t* n) { auto it = a.byName.find(n); return it == a.byName.end() ? nullptr : &it->second; };
        check(at(L"svc&a") && at(L"svc&a")->Cfg().Delayed() && at(L"svc&a")->StartSrc() == SxsValue::SERVICE && !at(L"b") && !at(L"cm"),
              L"sxs scanner: serviceData start (delayedAuto -> Auto delayed), entities decoded, a bad start and a comment ignored");
        check(at(L"drv1") && at(L"drv1")->Cfg().start == 0 && at(L"drv1")->StartSrc() == SxsValue::REGISTRY && a.byName.size() == 3,
              L"sxs scanner: registryKey Services\\X Start; a subkey's Start and a value outside a key are ignored");
        check(at(L"storx") && at(L"storx")->Cfg().Delayed() && !at(L"storx")->LowConfidence(),
              L"sxs scanner: DelayedAutostart=1 from the registry value makes serviceData auto delayed (StorSvc's case)");
        ScanManifestXml("<assembly><serviceData name=\"svc&amp;a\" start=\"demand\"/></assembly>", &a);
        check(at(L"svc&a")->conflict && !at(L"svc&a")->Usable(), L"sxs scanner: two manifests with different values -> conflict, no value");
        SxsScan s;
        ScanManifestXml("<assembly><assemblyIdentity name=\"Comp-A\"/><dependency><dependentAssembly><assemblyIdentity name=\"Other\"/>"
                        "</dependentAssembly></dependency><serviceData name=\"Q\" start=\"demand\"/><xsd:element name=\"Start_Q\" default=\"3\" "
                        "wcm:handler=\"regkey('HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\Q')\" wcm:legacyName=\"Start\"/>"
                        "<xsd:element name=\"Start_T\" default=\"4\" wcm:handler=\"regkey('HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\T')\" "
                        "wcm:legacyName=\"Start\"/></assembly>", &s);
        const SxsValue* t = s.byName.count(L"t") ? &s.byName[L"t"] : nullptr;
        check(t && t->Usable() && t->LowConfidence() && t->Cfg().start == 4 && s.settings.count(L"comp-a|start_q"),
              L"sxs scanner: a setting default alone is a low-confidence value; settings keyed by the manifest's own component");
        int n = ApplyEditionXml("<unattend><component name=\"Comp-A\"><Start_Q>2</Start_Q></component>"
                                "<component name=\"Other\"><Start_Q>4</Start_Q></component></unattend>", &s);
        check(n == 1 && s.byName[L"q"].Cfg().start == 2 && s.byName[L"q"].StartSrc() == SxsValue::EDITION && s.byName[L"q"].Usable(),
              L"sxs edition file: overrides serviceData and the setting default; another component's setting is not mapped");

        const SxsStock* fs = w.sxs.get();
        check(fs && fs->ok && fs->manifests == 7 && fs->editionRead && fs->overrides == 2 && fs->build == L"19045.5737" &&
                  fs->edition == L"Professional",
              L"sxs fake reader: 7 fixture manifests, the Professional edition file (2 overrides), build 19045.5737");
        const SxsValue* au = fs ? fs->Find(L"Audiosrv") : nullptr;
        const SxsValue* dps = fs ? fs->Find(L"DPS") : nullptr;
        check(au && au->Cfg().start == 2 && au->StartSrc() == SxsValue::EDITION && dps && dps->LowConfidence() && dps->Cfg().Delayed(),
              L"sxs layers: AudioSrv demand + setting 3 + edition 2 -> Auto (edition); DPS from setting defaults only -> low confidence");
        check(fs && fs->Find(L"CDPUserSvc_4f2a1", true) && fs->Find(L"CDPUserSvc_4f2a1", true)->Cfg().start == 2 &&
                  !fs->Find(L"CDPUserSvc_4f2a1") && !fs->Find(L"Foo_zz", true) &&
                  !fs->Find(L"Commented") && !fs->Find(L"InCdata") && fs->Find(L"Beep") && fs->Find(L"Beep")->Cfg().start == 1,
              L"sxs: a per-user instance maps to its template (only when asked: review F6); comments and CDATA are not read; a "
              L"driver's registry Start is");
        if (fs) {
            SxsStock back;
            std::wstring e;
            std::string text = WriteSxsCache(*fs);
            bool rt = ParseSxsCache(text, &back, &e) && back.fromCache && WriteSxsCache(back) == text && back.Usable() == fs->Usable();
            SxsStock torn;
            bool tornFails = !ParseSxsCache(text.substr(0, text.size() - 4), &torn, &e) && !torn.ok;
            check(rt && tornFails, L"sxs cache: write -> parse -> write reproduces it; a torn file (no end line) is refused");
            std::string old = text;
            old.replace(0, old.find('\n'), "HXSVC\t1\tsxsstock\t1");
            SxsStock ov;
            std::wstring oe;
            check(!ParseSxsCache(old, &ov, &oe) && oe.find(L"another scanner version") != std::wstring::npos,
                  L"sxs cache (review F5): a cache from another scanner version is refused (the caller renames it aside)");
            SxsStock c1 = *fs;
            c1.fixture = false;
            SxsStock c2 = c1, c3 = c1, c4 = c1;
            c2.failed = 1;
            c3.editionRead = false;
            c4.truncated = true;
            check(fs->fixture && !fs->Cacheable() && c1.Cacheable() && !c2.Cacheable() && !c3.Cacheable() && !c4.Cacheable(),
                  L"sxs cache (review F3/F4): only a complete scan with the edition file read is cached; never the fixture's");
        }
        {   // review F3: an unreadable edition file: services its settings target are shown, not applied.
            struct NoEdition : SxsReader {
                bool List(std::vector<std::wstring>* n, bool* t, std::wstring*) override { *n = {L"a.manifest"}; *t = false; return true; }
                bool ReadXml(const std::wstring&, std::string* x, bool*, std::wstring*) override {
                    *x = "<assembly><assemblyIdentity name=\"Comp-A\"/><serviceData name=\"Audiosrv\" start=\"demand\"/>"
                         "<xsd:element name=\"Start_AudioSrv\" default=\"3\" wcm:handler=\"regkey('HKEY_LOCAL_MACHINE\\SYSTEM\\"
                         "CurrentControlSet\\Services\\AudioSrv')\" wcm:legacyName=\"Start\"/><serviceData name=\"PcaSvc\" "
                         "start=\"demand\"/></assembly>";
                    return true;
                }
                std::wstring BuildUbr() override { return L"19045.1"; }
                std::wstring Edition() override { return L"Professional"; }
                bool ReadEdition(std::string*, std::wstring* e) override { *e = L"access denied (test)"; return false; }
            } ne;
            SxsStock ns = ScanSxs(ne);
            Ctx nc = w.ctx;
            nc.sxs = &ns;
            StockPick au = {}, pc = {};
            for (auto& i : w.items) {
                if (IEq(i.name, L"Audiosrv")) au = StockFor(i, nc);
                if (IEq(i.name, L"PcaSvc")) pc = StockFor(i, nc);
            }
            Plan np = BuildDefault(true, TAB_SERVICES, w.items, nc);
            bool note = false;
            for (auto& n : np.notes) if (n.find(L"The edition file could not be read (access denied (test))") == 0) note = true;
            check(ns.ok && !ns.editionRead && !ns.Cacheable() && au.kind == StockPick::Show &&
                      au.why.find(L"the edition file could not be read") == 0 && pc.kind == StockPick::Apply && note,
                  L"sxs (review F3): without the edition file, services its settings target are shown, not applied; the plan says why");
            ns.failed = 2;
            StockPick pf = {};
            for (auto& i : w.items) if (IEq(i.name, L"PcaSvc")) pf = StockFor(i, nc);
            Plan fp = BuildDefault(true, TAB_SERVICES, w.items, nc);
            bool fnote = false;
            for (auto& n : fp.notes) if (n.find(L"2 component manifests could not be read and 0 were skipped") == 0) fnote = true;
            check(pf.kind == StockPick::Show && fnote, L"sxs (review F4): with failed manifests, manifest values are shown, not applied, "
                                                       L"and the plan gives the counts");
            Ctx pc2 = w.ctx;
            pc2.sxsPending = true;
            Plan pp = BuildDefault(true, TAB_SERVICES, w.items, pc2);
            check(pp.rows.empty() && pp.emptyText == L"still reading this PC's component manifests",
                  L"stock plan (review F2): waits while the manifests are being read, never a KB-only plan");
        }
        {   // review F1: --out never overwrites or follows an existing path.
            wchar_t tmp[MAX_PATH];
            DWORD n = GetTempPathW(MAX_PATH, tmp);
            std::wstring p = std::wstring(tmp, n) + L"hx-out-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(GetTickCount64()) + L".txt";
            std::wstring e1, e2;
            bool first = WriteNewOut(p, "a", &e1);
            bool second = WriteNewOut(p, "b", &e2);
            std::string back;
            bool same = ReadAll(p, &back) && back == "a";
            DeleteFileW(p.c_str());   // the test's own temp file
            check(first && !second && e2.find(L"already exists") != std::wstring::npos && same,
                  L"--out (review F1): written with CREATE_NEW; an existing path is refused with the reason and left as it was");
        }
        check(SxsCacheName(L"19045.5737", L"Professional") == L"sxs-v2-19045.5737-Professional.txt" && SxsCacheName(L"19045", L"Pro").empty() &&
                  SxsCacheName(L"19045.1", L"..\\x").empty() && SxsCacheName(L"", L"Pro").empty(),
              L"sxs cache name: build.UBR and an alphanumeric edition only");
        if (fs) {
            std::string tsv = SxsDumpTsv(*fs, 7);
            check(tsv.find("# build.UBR\t19045.5737\n") != std::string::npos && tsv.find("# elapsed ms\t7\n") != std::string::npos &&
                      tsv.find("\naudiosrv\t2\t-\tedition\t0\tProfessionalEdition.xml\n") != std::string::npos &&
                      tsv.find("\ndps\t2\t1\tsettingDefault\t0\tc-settings.manifest\n") != std::string::npos &&
                      tsv.find("\nbeep\t1\t-\tregistry\t0\td-driver.manifest\n") != std::string::npos,
                  L"--dump-stock-live TSV: header (build.UBR, elapsed), start, delayed, layer, conflict and source file per name");
            const SxsValue* dtc = fs->Find(L"HxDtc");
            check(tsv.find("\nhxdtc\t3\t1\tserviceData\t0\tf-live-cases.manifest\n") != std::string::npos && dtc &&
                      dtc->setDelayed == 1 && dtc->Cfg().start == 3,
                  L"sxs live run (a): a setting element named DelayedAutostart with no legacyName fills delayed as the lowest layer");
            check(tsv.find("\nhxclr\t-\t1\tnone\t0\t\n") != std::string::npos && !fs->Find(L"HxClr"),
                  L"sxs live run (b): the same DelayedAutostart in two manifests is no conflict, and with no Start gives no start value");
            check(tsv.find("\nhxauto\t2\t-\tserviceData\t0\tf-live-cases.manifest\n") != std::string::npos,
                  L"sxs live run (c): no DelayedAutostart in any file prints \"-\" (absent), not 0");
        }
        check(SxsRuntimeStart(L"WlanSvc") && !SxsRuntimeStart(L"BITS") && SxsRuntimeDelayed(L"BITS") && !SxsRuntimeDelayed(L"Spooler"),
              L"sxs: the after-setup lists: start type (WlanSvc, ...) and DelayedAutostart (BITS, ...)");
        auto pick = [&](const wchar_t* n) {
            for (auto& i : w.items) if (IEq(i.name, n)) return StockFor(i, w.ctx);
            return StockPick{};
        };
        StockPick pca = pick(L"PcaSvc"), one = pick(L"OneSyncSvc"), sm = pick(L"SysMain"), sti = pick(L"stisvc"), dp = pick(L"DPS"),
                  au2 = pick(L"Audiosrv"), bp = pick(L"Beep"), ws = pick(L"WSearch");
        check(pca.kind == StockPick::Apply && pca.basis == L"owner-measured" && pca.cfg.start == 3 &&
                  pca.basisNote.find(L"The start type the test fixture's hand-written manifests (not a real PC) give the service (build 19045.5737") == 0 &&
                  pca.basisCond == L"read from the test fixture's hand-written manifests (not a real PC), build 19045.5737" &&
                  BasisLabel(pca.basis, pca.basisCond) == pca.basisCond && BasisLabel(L"owner-measured") == L"measured by Hadron's author" &&
                  pca.basisNote.find(L"Two agreeing third-party tables agree") != std::wstring::npos,
              L"stock: manifest and tables agree -> applied, owner-measured kind labelled by its condition (fixture: not a real PC), the tables named");
        check(one.kind == StockPick::Apply && one.cfg.start == 3 && one.basis == L"owner-measured", L"stock: manifest only -> applied");
        check(sm.kind == StockPick::Show && sm.why.find(L"sources disagree: the test fixture's manifests say Manual; two agreeing third-party tables say Auto") == 0 &&
                  sm.basisNote.find(L"tweakhound.com") != std::wstring::npos,
              L"stock: manifest and tables disagree -> shown with both sources, not applied");
        check(sti.kind == StockPick::Show && sti.why.find(L"start type may be changed after setup") != std::wstring::npos, L"stock: a runtime-set service -> shown, not applied");
        check(dp.kind == StockPick::Show && dp.why.find(L"only a configuration-setting default") == 0,
              L"stock: a setting default alone that no table confirms -> shown, not applied");
        check(au2.kind == StockPick::Apply && au2.cfg.start == 2 && bp.kind == StockPick::None && ws.kind == StockPick::Apply &&
                  ws.reason == L"Windows stock start type from a Microsoft page (it names no build)",
              L"stock: AudioSrv's edition value agrees with the tables; a driver without sysok gets none; no manifest -> the KB's value");
        Ctx other = w.ctx;
        other.build = L"22631";
        StockPick o = StockFor(w.items.front(), other);
        bool anyKb = false;
        for (auto& i : w.items) {
            StockPick k = StockFor(i, other);
            if (k.kind == StockPick::Apply && k.basis != L"owner-measured") anyKb = true;
        }
        (void)o;
        check(!anyKb, L"stock: on another build only this PC's manifests give values (the KB's are for 19045)");
    }
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
        // R6: a single change may not lower a System-start driver without sysok either (nothing could put it back).
        Plan s6 = BuildSingle(*ItemOf(sysItems, L"igfxn"), StartCfg{4, -1}, sysItems, c4);
        Kb kb6;
        kb6.Parse("V\t1\tservices\nB\t19045\n"
                  "K\tigfxn\tdrv\t19045\tt\te\t\t\t\t4\t\t\twhy\t\t1\t0\treasoning\tn\treasoning\tn\t\t\t\tself\t\t\n", &err);
        Ctx c6 = c;
        c6.kb = &kb6;
        Plan s7 = BuildSingle(*ItemOf(sysItems, L"igfxn"), StartCfg{4, -1}, sysItems, c6);
        check(s6.Included() == 0 && RowOf(s6, L"igfxn") && RowOf(s6, L"igfxn")->blocked.find(L"System-start") != std::wstring::npos &&
                  s7.Included() == 1,
              L"R6: a single change refuses to lower a System-start driver the KB doesn't clear (sysok), and allows one it does");

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
        SvcItem sys1 = *ItemOf(w.items, L"MapsBroker");
        sys1.cfg.start = 1;
        check(CheckGuard(sys1, c).neverTouch, L"guard: a Win32 service whose registry Start is System -> never touch (logic M6)");
        Ctx nohw = c;
        nohw.hw = nullptr;
        nohw.Prepare();
        HwInfo noBoot = w.hw;
        noBoot.bootDisk.clear();
        Ctx nb = c;
        nb.hw = &noBoot;
        nb.Prepare();
        Plan pnh = BuildPreset(LV_STRIPPED, TAB_DRIVERS, w.items, nohw);
        bool noteHw = false;
        for (auto& n : pnh.notes) if (n.find(L"device list could not be read") != std::wstring::npos) noteHw = true;
        check(CheckGuard(*ItemOf(w.items, L"igfxn"), nohw).neverTouch && CheckGuard(*ItemOf(w.items, L"igfxn"), nb).neverTouch &&
                  pnh.Included() == 0 && noteHw && !CheckGuard(*ItemOf(w.items, L"DiagTrack"), nohw).neverTouch,
              L"guard: no device list or no boot disk -> every driver kept, with a note (fail closed); services unaffected");
        SvcItem grouped = *ItemOf(w.items, L"MapsBroker");
        grouped.group = L"TestGroup";
        Plan pg = BuildSingle(grouped, StartCfg{2, 1}, w.items, c);
        Plan pg2 = BuildSingle(grouped, StartCfg{2, 0}, w.items, c);
        check(pg.Included() == 0 && !pg.rows.empty() && pg.rows[0].blocked.find(L"load-order group") != std::wstring::npos &&
                  pg2.Included() <= 1,
              L"Auto (delayed) is never offered for a service in a load-order group (docs D1)");
        {   // M5: a preset never raises (Disabled -> Manual), even if the KB target is Manual.
            Kb k5;
            k5.Parse("V\t1\tservices\nB\t19045\n"
                     "K\tXblGameSave\tsvc\t19045\tt\te\t\t\t3\t\t\twhy\t\t\t0\t0\treasoning\tn\treasoning\tn\t\t\t\t\t\t\n", &err);
            Ctx c5 = c;
            c5.kb = &k5;
            Plan p5 = BuildPreset(LV_BLOAT, TAB_SERVICES, w.items, c5);
            bool note = false;
            for (auto& n : p5.notes) if (n.find(L"stricter") != std::wstring::npos) note = true;
            check(ItemOf(w.items, L"XblGameSave")->cfg.start == 4 && !RowOf(p5, L"XblGameSave") && note,
                  L"a preset never raises a start type: Disabled stays Disabled where the level says Manual (logic M5)");
        }
        {   // M6: Restore first snapshot can't lower a live System-start driver without sysok.
            std::vector<SvcItem> it6 = w.items;
            for (auto& i : it6) if (i.name == L"Beep") i.cfg = StartCfg{1, -1};
            Snapshot f6 = w.fx.first;
            for (auto& i : f6.items) if (i.name == L"Beep") i.cfg = StartCfg{3, -1};
            Ctx c6 = c;
            c6.first = &f6;
            Plan p6 = BuildDefault(false, TAB_DRIVERS, it6, c6);
            const PlanRow* br = RowOf(p6, L"Beep");
            check(br && !br->blocked.empty() && br->blocked.find(L"System-start") != std::wstring::npos && br->blocked.find(L"sysok") == std::wstring::npos,
                  L"Restore first snapshot: a live System-start driver without sysok is blocked (logic M6)");
        }
        Plan pc = BuildControl(*ItemOf(w.items, L"igfxn"), 1, w.fx.backend, w.items, w.ctx);
        check(!pc.rows.empty() && !pc.rows[0].blocked.empty(), L"guard: drivers are never loaded or unloaded live");
        Plan pr = BuildControl(*ItemOf(w.items, L"Spooler"), 2, w.fx.backend, w.items, w.ctx);
        check(pr.rows.size() >= 2 && pr.rows.front().ctl == Op::Stop && pr.rows.back().ctl == Op::Start && pr.Blocked() == 0,
              L"control: restart stops (dependents first), then starts");
        // R5: never-touch items are not started, stopped or restarted (BFE would take the firewall down).
        bool allBlocked = true;
        for (const wchar_t* n : {L"BFE", L"Audiosrv", L"EventLog", L"Dhcp", L"RpcSs"}) {
            const SvcItem* it = ItemOf(w.items, n);
            for (int what = 0; what < 3 && it; what++) {
                Plan pb = BuildControl(*it, what, w.fx.backend, w.items, w.ctx);
                if (pb.rows.empty()) allBlocked = false;
                for (auto& r : pb.rows) if (r.blocked.find(L"Protected") != 0) allBlocked = false;
            }
            if (!it) allBlocked = false;
        }
        check(allBlocked, L"R5: Start, Stop and Restart of never-touch items (BFE, Audiosrv, EventLog, Dhcp, RpcSs) are refused, every row blocked");
        FakeBackend fb;
        SvcItem base = Mk(L"HxBase", 2, {}), dep = Mk(L"Dhcp", 2, {L"HxBase"});
        base.state = dep.state = SERVICE_RUNNING;
        fb.SetItems({base, dep});
        std::vector<SvcItem> two{base, dep};
        Plan pdp = BuildControl(base, 1, fb, two, w.ctx);
        check(pdp.Included() == 0 && pdp.Blocked() == (int)pdp.rows.size() && !pdp.rows.empty() &&
                  pdp.rows[0].blocked.find(L"would also stop Dhcp") != std::wstring::npos,
              L"R5: stopping a service whose running dependent is never-touch (Dhcp) is refused");
        // R13: a failed stop starts again what the run already stopped.
        FakeBackend fr;
        SvcItem x = Mk(L"HxX", 2, {}), d1 = Mk(L"HxD1", 2, {L"HxX"}), d2 = Mk(L"HxD2", 2, {L"HxX"});
        x.state = d1.state = d2.state = SERVICE_RUNNING;
        x.controls = d1.controls = d2.controls = SERVICE_ACCEPT_STOP;
        fr.SetItems({x, d1, d2});
        std::vector<SvcItem> three{x, d1, d2};
        Plan rs = BuildControl(x, 2, fr, three, w.ctx);
        fr.faults.stopFail.insert(L"hxx");
        Store cs(NewTempDir(L"control"));
        ApplyEnv ce = EnvFor(&fr, &cs);
        ApplyResult cr;
        bool cok = RunControl(rs, ce, &cr);
        bool allUp = true;
        for (auto& i : fr.Items()) if (i.state != SERVICE_RUNNING) allUp = false;
        int again = 0;
        for (auto& l : cr.lines) if (l.find(L"started again after the failed stop") != std::wstring::npos) again++;
        check(!cok && allUp && again == 2, L"R13: Restart whose stop of the service fails starts the 2 dependents it stopped again");
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
        const PlanRow* um = RowOf(mn, L"SSDPSRV");
        const PlanRow* fm = RowOf(mn, L"FrameServer");
        check(um && um->blocked.empty() && um->include && um->evidence.find(L"ticked at Minimum") != std::wstring::npos &&
                  fm && !fm->include,
              L"R18: at Minimum a row whose use cannot be detected is ticked and says so; a hot-plug class stays unticked");
        // SCardSvr, not FrameServer: the fixture's FrameServer is now kept by its camera interface trigger (follow-up 8).
        const PlanRow* fs = RowOf(st, L"SCardSvr");
        check(fs && fs->blocked.empty() && !fs->include && fs->evidence.find(L"hot-plug") != std::wstring::npos,
              L"Stripped: a never-seen hot-plug class (smart-card reader) is offered unticked");
        const PlanRow* bt = RowOf(st, L"bthserv");
        check(bt && !bt->blocked.empty() && bt->blocked.find(L"Kept") == 0, L"Stripped: Bluetooth present -> kept");
        const PlanRow* sn = RowOf(st, L"SensorService");
        check(sn && sn->include, L"Stripped: never-seen sensors -> ticked, with evidence");
        const PlanRow* sp = RowOf(st, L"Spooler");
        check(sp && !sp->include && sp->depWarning.find(L"Fax") != std::wstring::npos,
              L"dependency check: disabling Spooler while Fax depends on it -> warning, unticked");
        const PlanRow* vm = RowOf(st, L"vmicheartbeat");
        check(vm && vm->include && !vm->contested.empty(), L"Stripped: Hyper-V guest pieces on hardware -> ticked, contested shown");
        check(RowOf(mn, L"SysMain") && !RowOf(st, L"SysMain") && RowOf(mn, L"MapsBroker"),
              L"Minimum adds the contested items and keeps Bloat's (cumulative)");
        check(RowOf(dv, L"igfxn") && RowOf(dv, L"igfxn")->include && RowOf(dv, L"BTHUSB") && !RowOf(dv, L"BTHUSB")->blocked.empty(),
              L"Drivers Stripped: igfxn (absent) ticked, BTHUSB (radio present) kept");
        bool dbNote = false;
        for (auto& n : db.notes) if (n.find(L"Pure bloat removal has no drivers") == 0) dbNote = true;
        check(db.rows.empty() && dbNote, L"Drivers Bloat: empty, and the plan says why");
        bool noFloor = true;
        for (auto* p : {&bl, &st, &mn, &dv})
            for (auto& r : p->rows)
                if (r.include && (IEq(r.name, L"RpcSs") || IEq(r.name, L"VSS") || IEq(r.name, L"WinDefend") || IEq(r.name, L"TrustedInstaller")))
                    noFloor = false;
        check(noFloor, L"no preset ticks a floor or never-in-preset item");
        Plan df = PlanFor(LEVEL_DEFAULT, TAB_SERVICES, w);
        check(df.Included() == 3 && RowOf(df, L"WSearch") && RowOf(df, L"WSearch")->after.Delayed(),
              L"Default (first snapshot): 3 rows, WSearch back to Auto (delayed)");
        {   // A7: a lingering DelayedAutostart=1 under Manual, restored to plain Auto (sniffer delayedlinger).
            World dl;
            std::wstring e;
            bool okw = MakeWorld(fixture + L"\\delayedlinger", &kb, &dl, &e);
            Plan pd = PlanFor(LEVEL_DEFAULT, TAB_SERVICES, dl);
            const PlanRow* sr = RowOf(pd, L"Spooler");
            check(okw && sr && sr->before.start == 3 && sr->before.delayed == 1 && sr->op.start == 2 && sr->op.delayed == 0 &&
                      sr->after.start == 2 && sr->after.delayed == 0 && !sr->after.Delayed(),
                  L"A7: Restore first snapshot writes DelayedAutostart=0 with Start=2 when 1 was left behind (3/1 -> 2/0, not 2/1)");
            std::string got = DumpPlan(pd);
            std::wstring gp = golden + L"\\plan-services-default-delayedlinger.txt";
            if (update) check(WriteAll(gp, got), L"wrote golden " + gp);
            else {
                std::string want;
                bool have = ReadAll(gp, &want);
                check(have && want == got, L"golden plan-services-default-delayedlinger.txt" + std::wstring(have ? (want == got ? L"" : L" differs") : L" missing"));
            }
            // The same through stock (in-memory KB with a stock value of plain Auto).
            Kb kd;
            kd.Parse("V\t1\tservices\nB\t19045\n"
                     "K\tSpooler\tsvc\t19045\tt\te\t\t\t\t\t\t\t\t\t0\t0\treasoning\tn\t\t\t2/0\towner-measured\tclean VM\t\t\t\n", &e);
            Ctx cd = dl.ctx;
            cd.kb = &kd;
            Plan ps = BuildDefault(true, TAB_SERVICES, dl.items, cd);
            const PlanRow* ss = RowOf(ps, L"Spooler");
            check(ss && ss->op.start == 2 && ss->op.delayed == 0, L"A7: Windows stock to plain Auto also writes DelayedAutostart=0 over a lingering 1");
        }
        {   // A2: recovery\first restores only what Hadron itself may change.
            std::vector<std::wstring> skipped;
            std::vector<RestoreRow> fr = FirstRestoreRows(w.fx.first, w.ctx, &skipped);
            auto has = [&](const wchar_t* n) { for (auto& r : fr) if (IEq(r.name, n)) return true; return false; };
            auto skip = [&](const wchar_t* n) { for (auto& s : skipped) if (IEq(s, n)) return true; return false; };
            bool noLow = true;
            for (auto& r : fr) if (r.cfg.start < 2) noLow = false;
            check(noLow && !has(L"stornvme") && !has(L"disk") && !has(L"pci") && !has(L"RpcSs") && !has(L"WdBoot") && !has(L"WinDefend") &&
                      !has(L"ExampleAudioFlt") && skip(L"stornvme") && skip(L"disk") && skip(L"RpcSs") && has(L"WSearch") && has(L"DiagTrack"),
                  L"A2: the first-run recovery leaves out Boot/System values, storage, the boot path, never-touch and protected items (" +
                      std::to_wstring(fr.size()) + L" rows, " + std::to_wstring(skipped.size()) + L" left out)");
            const std::wstring ftitle = L"start types as recorded when Hadron first ran";
            std::string fcmd = RestoreCmd(L"first", ftitle, fr);
            std::string frd = ReadmeTxt(L"first", L"\\ProgramData\\Hadron\\services\\recovery\\first", ftitle, fr, skipped);
            check(frd.find("F. Not in these scripts") != std::string::npos && frd.find("  stornvme\r\n") != std::string::npos,
                  L"A2: README.txt lists the names left out (section F)");
            // mini-finalize 15/16: a single change can reach a never-preset item, so F doesn't claim Hadron never
            // changes them, and says what puts such a change back; "Which folder to use" names first-2..first-9.
            check(frd.find("Hadron never changes these") == std::string::npos &&
                      frd.find("Presets and Default never change these") != std::string::npos &&
                      frd.find("changed one at a\r\ntime from Hadron's detail pane: Undo, or that change's own recovery folder") != std::string::npos &&
                      frd.find("\"first-2\" (up to \"first-9\")") != std::string::npos,
                  L"README F says presets and Default never change those items (a single change can) and how such a change is "
                  L"put back; Which folder to use names first-2..first-9");
            for (auto& g2 : {std::make_pair(std::wstring(L"first-restore.cmd"), fcmd), std::make_pair(std::wstring(L"first-README.txt"), frd)}) {
                std::wstring gp = golden + L"\\" + g2.first;
                if (update) { check(WriteAll(gp, g2.second), L"wrote golden " + gp); continue; }
                std::string want;
                bool have = ReadAll(gp, &want);
                check(have && want == g2.second, L"golden " + g2.first + (have ? (want == g2.second ? L"" : L" differs") : L" missing"));
            }
        }
        Plan sk = PlanFor(-2, TAB_SERVICES, w);
        Plan skd = PlanFor(-2, TAB_DRIVERS, w);
        check(RowOf(sk, L"WSearch") && RowOf(sk, L"WSearch")->basis == L"published" &&
                  RowOf(sk, L"WSearch")->after.start == 2 && RowOf(sk, L"WSearch")->after.delayed == 1 &&
                  skd.rows.empty() && !skd.notes.empty(),
              L"Default (Windows stock): the KB's published stock value for WSearch (fixture Manual -> Auto delayed); "
              L"no driver has one -> no rows, with a note");
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
        // A KB `never` (explanation-only) entry doesn't block its stock row; never-touch (floor), protected and the
        // named never-preset list still do (DECISIONS 2026-11-11, proxy Small fixes).
        {
            const PlanRow* tb = RowOf(sk, L"TimeBrokerSvc");
            check(tb && tb->blocked.empty() && tb->include && tb->after.start == 3,
                  L"Windows stock: TimeBrokerSvc (KB entry explanation-only) is a ticked row Disabled -> Manual");
            Kb kn;
            kn.Parse("V\t1\tservices\nB\t19045\n"
                     "K\tTimeBrokerSvc\tsvc\t19045\tt\te\t\t\t\t\t\t\t\t\t0\t1\treasoning\tn\t\t\t3/-\towner-measured\tclean VM\t\t\t\n"
                     "K\tRpcSs\tsvc\t19045\tt\te\t\t\t\t\t\t\t\t\t0\t1\treasoning\tn\t\t\t3/-\towner-measured\tclean VM\t\t\t\n"
                     "K\tSecurityHealthService\tsvc\t19045\tt\te\t\t\t\t\t\t\t\t\t0\t1\treasoning\tn\t\t\t2/0\towner-measured\tclean VM\t\t\t\n"
                     "K\tTrustedInstaller\tsvc\t19045\tt\te\t\t\t\t\t\t\t\t\t0\t1\treasoning\tn\t\t\t2/0\towner-measured\tclean VM\t\t\t\n",
                     &err);
            Ctx cn = w.ctx;
            cn.kb = &kn;
            Plan sn = BuildDefault(true, TAB_SERVICES, w.items, cn);
            const PlanRow *t2 = RowOf(sn, L"TimeBrokerSvc"), *rp = RowOf(sn, L"RpcSs"), *sh = RowOf(sn, L"SecurityHealthService"),
                          *ti = RowOf(sn, L"TrustedInstaller");
            check(t2 && t2->blocked.empty() && t2->include && rp && !rp->blocked.empty() && !rp->include && sh &&
                      !sh->blocked.empty() && ti && !ti->blocked.empty(),
                  L"Windows stock: a never entry's row is ticked; never-touch (RpcSs), protected (SecurityHealthService) "
                  L"and named never-preset (TrustedInstaller) rows with a stock value stay blocked");
            bool presetsSame = true;
            for (int lv : {LV_BLOAT, LV_STRIPPED, LV_MINIMUM}) {
                Plan pp = PlanFor(lv, TAB_SERVICES, w);
                const PlanRow* pr = RowOf(pp, L"TimeBrokerSvc");
                if (pr && pr->blocked.empty()) presetsSame = false;
            }
            Plan pd = PlanFor(LEVEL_DEFAULT, TAB_SERVICES, w);
            check(presetsSame && !RowOf(pd, L"TimeBrokerSvc"),L"presets never offer TimeBrokerSvc (KB never), and Default as first seen leaves it "
                                      L"(already as first seen)");
        }
        Ctx co = w.ctx;
        co.build = L"22631";
        co.sxs = nullptr;   // without this PC's manifests (they would give values on any build)
        Plan so = BuildDefault(true, TAB_SERVICES, w.items, co);
        check(so.rows.empty() && !so.notes.empty() && so.notes[0].find(L"no Windows-stock data for build 22631") != std::wstring::npos,
              L"Windows stock on another build: refused with the reason");
        // Goldens: every level x tab.
        const wchar_t* lv[] = {L"default", L"bloat", L"stripped", L"minimum", L"stock"};
        const wchar_t* tb[] = {L"services", L"drivers"};
        for (int t = 0; t < 2; t++)
            for (int l = 0; l < 5; l++) {
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
        bool ok = !logs.empty() && ReadAll(fixture + L"\\interrupted\\" + At(logs, 0), &log) && ParseUndoLog(log, &ul, &err);
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
                                        {L"Bad&Name", {3, -1}, false}, {L"D\u00e9mo", {3, -1}, false},
                                        {L"Intel(R) TPM Provisioning Service", {2, 1}, true, true}};
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
        check(CmdSafeName(L"DiagTrack") && !CmdSafeName(L"A&B") && !CmdSafeName(L"a%b") && !CmdSafeName(L"D\u00e9mo") &&
                  !CmdSafeName(L"Intel(R) X") && !CmdSafeName(L"a)b") && !CmdSafeName(L"a:b") && !CmdSafeName(L" lead") &&
                  CmdSafeName(L"NVDisplay.ContainerLocalSystem") && CmdSafeName(L"Some Service_1-2"),
              L"CmdSafeName is an allow-list: ( ) and every other cmd character go to the .reg files (A1)");
        bool blockFree = cmd.find("Intel(R)") == std::string::npos;
        std::string online = RestoreOnlineCmd(L"20261111-101500", rows);
        for (const std::string* s : {&cmd, &online}) {
            size_t a = 0;
            while (a < s->size()) {
                size_t b = s->find("\r\n", a);
                std::string line = s->substr(a, b == std::string::npos ? std::string::npos : b - a);
                a = b == std::string::npos ? s->size() : b + 2;
                std::string bare = line;   // a for /f may carry the name inside "..." within its ('...'): quotes protect it
                if (line.rfind("for /f ", 0) == 0)
                    for (size_t q1; (q1 = bare.find('"')) != std::string::npos;) {
                        size_t q2 = bare.find('"', q1 + 1);
                        if (q2 == std::string::npos) break;
                        bare.erase(q1, q2 - q1 + 1);
                    }
                bool usesName = bare.find("%~1") != std::string::npos || bare.find("%N%") != std::string::npos;
                if ((usesName && (line.find('(') != std::string::npos || line.find(')') != std::string::npos)) ||
                    (line.rfind("if ", 0) == 0 && line.find('(') != std::string::npos))
                    blockFree = false;
            }
        }
        check(blockFree && cmd.find("goto :baddelay") != std::string::npos && cmd.find("reg unload HKLM\\HXOFF\r\nif errorlevel 1 echo WARNING") != std::string::npos &&
                  online.find("goto :noadmin") != std::string::npos && online.find("goto :baddelay") != std::string::npos,
              L"restore scripts: no ( ) block near a name, every reg add and the unload checked (A1, win32 M1)");
        std::string dry = DryRunCmd(cmd);
        std::wstring dgp = golden + L"\\restore-dryrun.cmd";
        if (update) check(WriteAll(dgp, dry), L"wrote golden " + dgp);
        else {
            std::string want;
            bool have = ReadAll(dgp, &want);
            check(have && want == dry, L"restore-dryrun.cmd matches tests/golden/services (CI runs it on real cmd.exe)" + std::wstring(have ? L"" : L" (missing)"));
        }
        std::string readme = ReadmeTxt(L"20261111-101500", L"\\x", L"t", rows);
        check(readme.find("E. Rows that may print FAILED") != std::string::npos && readme.find("(needed TrustedInstaller: see E)") != std::string::npos,
              L"README.txt says rows that needed TrustedInstaller may fail in WinRE / Safe Mode (docs D9)");
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
        Store sp(L"\\\\?\\" + tmpRoot);
        std::string rd;
        bool wp = WriteRecovery(sp, L"rtest2", L"test", rows, &dir, &zp, &err) && sp.Read(L"recovery\\rtest2\\README.txt", &rd, &err);
        check(wp && rd.find("\\\\?\\") == std::string::npos && rd.find("\\recovery\\rtest2\\restore.cmd") != std::string::npos,
              L"a \\\\?\\-prefixed data dir gives a README path without the prefix (sniffer #8)");
    }

    // ---- first-run snapshot ---------------------------------------------------------------------------------
    {
        std::wstring root = NewTempDir(L"first");
        Store s(root);
        Snapshot first;
        Ctx ca = w.ctx, cb = w.ctx;
        ca.machine = L"machine-a";
        cb.machine = L"machine-b";
        FirstRunResult r = EnsureFirstSnapshot(s, w.items, ca, &first);
        check(r.wrote && r.error.empty() && s.Exists(L"recovery-first.zip") && s.Exists(L"recovery\\first\\restore.cmd"),
              L"first run: first.txt, recovery\\first and recovery-first.zip written" + (r.error.empty() ? L"" : L": " + r.error));
        Snapshot again;
        FirstRunResult r2 = EnsureFirstSnapshot(s, w.items, ca, &again);
        check(r2.existed && !r2.wrote && again.items.size() == first.items.size(), L"first run: an existing first.txt is read, never rewritten");
        FirstRunResult r3 = EnsureFirstSnapshot(s, w.items, cb, &again);
        check(r3.foreign, L"first run: a first.txt from another machine GUID is flagged");
        std::wstring root2 = NewTempDir(L"corrupt");
        Store s2(root2);
        s2.WriteNew(L"first.txt", "not a snapshot", &err);
        FirstRunResult r4 = EnsureFirstSnapshot(s2, w.items, ca, &again);
        check(r4.corrupt && r4.wrote && s2.List(L"first.txt.corrupt-*").size() == 1,
              L"first run: a corrupt first.txt is renamed aside (kept), and a new one written");
        std::string fc;
        check(s.Read(L"recovery\\first\\restore.cmd", &fc, &err) && fc.find("\"stornvme\"") == std::string::npos &&
                  fc.find("\"WSearch\"") != std::string::npos,
              L"first run: recovery\\first\\restore.cmd leaves out the storage drivers (A2)");
    }

    auto freshWorld = [&](World* fw) { std::wstring e; return MakeWorld(fixture, &kb, fw, &e); };

    // ---- R2: an item whose configuration could not be read; R15: adapter and recognizer drivers ---------------
    {
        World a;
        freshWorld(&a);
        std::vector<SvcItem> items = a.items;
        SvcItem* mb = nullptr;
        for (auto& i : items) if (i.name == L"MapsBroker") mb = &i;
        if (mb) { mb->cfg = StartCfg{3, -1}; mb->cfgUnknown = true; mb->readError = std::wstring(kCfgUnreadable) + L": Access is denied."; }
        Plan pm = BuildPreset(LV_MINIMUM, TAB_SERVICES, items, a.ctx);
        bool noted = false;
        for (auto& n : pm.notes) if (n.find(L"could not be read") != std::wstring::npos) noted = true;
        Plan ps = mb ? BuildSingle(*mb, StartCfg{4, -1}, items, a.ctx) : Plan{};
        check(mb && !RowOf(pm, L"MapsBroker") && noted && ps.Included() == 0 && RowOf(ps, L"MapsBroker") && !RowOf(ps, L"MapsBroker")->blocked.empty(),
              L"R2: an unreadable item is never planned (preset: left out with a note; single change: blocked)");
        Store s(NewTempDir(L"unreadable"));
        Snapshot first;
        FirstRunResult fr = EnsureFirstSnapshot(s, items, a.ctx, &first);
        std::string rc;
        s.Read(L"recovery\\first\\restore.cmd", &rc, &err);
        check(fr.wrote && !first.Find(L"MapsBroker") && fr.note.find(L"not in first.txt") != std::wstring::npos &&
                  rc.find("\"MapsBroker\"") == std::string::npos && rc.find("\"WSearch\"") != std::string::npos,
              L"R2: first.txt and recovery\\first leave out an unreadable item, and say so");
        Snapshot old;
        old.items = items;
        Snapshot back;
        bool parsed = ParseSnapshot(WriteSnapshot(old), &back, &err);
        check(parsed && back.Find(L"MapsBroker") && back.Find(L"MapsBroker")->cfgUnknown && !back.Find(L"WSearch")->cfgUnknown,
              L"R2: a snapshot keeps the unreadable mark (readError prefix) through write and parse");
        std::vector<std::wstring> skipped;
        std::vector<RestoreRow> rr = FirstRestoreRows(back, a.ctx, &skipped);
        bool inRows = false;
        for (auto& r : rr) if (r.name == L"MapsBroker") inRows = true;
        Ctx cf = a.ctx;
        cf.first = &back;
        Plan pd = BuildDefault(false, TAB_SERVICES, a.items, cf);
        check(!inRows && !RowOf(pd, L"MapsBroker"),
              L"R2: an older first.txt that recorded an unreadable item: never restored by recovery\\first or Default (first snapshot)");
        SvcItem rec = Mk(L"HxRecognizer", 3, {}, SERVICE_RECOGNIZER_DRIVER);
        SvcItem ad = Mk(L"HxAdapter", 3, {}, SERVICE_ADAPTER);
        check(IsDriverType(rec.type) && IsDriverType(ad.type) && OnTab(rec, TAB_DRIVERS) && !OnTab(ad, TAB_SERVICES) &&
                  TypeName(rec.type) == L"File system recognizer driver" && TypeName(ad.type) == L"Adapter driver",
              L"R15: recognizer (0x8, Fs_Rec) and adapter (0x4) drivers count as drivers (Drivers tab, driver guards)");
    }

    // ---- apply -> undo round trip on the fake ----------------------------------------------------------------
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
        check(logs.size() == 1 && At(logs, 0).hasE && FindInterrupted(logs) < 0 && FindUndoable(logs) == 0,
              L"the undo log is complete (B ... E) and offers Undo");
        std::vector<size_t> order = ApplyOrder(p);
        int xa = -1, xg = -1;
        for (size_t k = 0; k < order.size(); k++) {
            if (p.rows[order[k]].name == L"SSDPSRV") xa = (int)k;
            if (p.rows[order[k]].name == L"upnphost") xg = (int)k;
        }
        check(xa >= 0 && xg >= 0 && xg < xa, L"apply order: a dependent (upnphost) is disabled before what it depends on (SSDPSRV)");
        Plan u = BuildUndo(At(logs, 0), a.fx.backend, a.items, a.ctx);
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
        Plan u2 = BuildUndo(At(logs, FindUndoable(logs)), a.fx.backend, a.items, a.ctx);
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
        check(logs.size() == 1 && At(logs, 0).hasE, L"faults: per-item errors do not stop the run; the log ends with E");

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
        check(r4.cancelled && r4.ok == 0 && l4.size() == 1 && At(l4, 0).hasE, L"cancel between items: nothing more changes, the log is closed");
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
        check(!ok && r.crashed && k == 0 && At(logs, 0).Row(3) && At(logs, 0).Row(3)->hasI && !At(logs, 0).Row(3)->hasD,
              L"crash after BEGIN 3: the log has I for row 3 and no E -> interrupted");
        std::vector<size_t> order = ApplyOrder(p);
        env.crashAfterBegin = -1;
        a.items = Folded(a.fx.backend);
        if (mode == 0) {
            Plan rp = BuildResume(At(logs, 0), a.fx.backend, a.items, a.ctx);
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
            Plan up = BuildUndo(At(logs, 0), a.fx.backend, a.items, a.ctx);
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
        MarkTi(&p, a.fx.backend, true, a.items);
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
        for (auto& row : At(logs, 0).rows) if (row.name == L"dmwappushservice") ur = &row;
        check(ok && r.failed == 0 && ti.acquired && ti.impersonations == 1 && ti.released,
              L"TI apply: the token is taken once, used for the marked row only, then released");
        check(ti.flagSeen && ti.writeRefused, L"TI apply: Hadron's own file writes are refused while impersonating");
        check(ur && ur->identity == L"ti" && ur->result == L"ok" && ur->text == L"ti" && At(logs, 0).tiStarted && At(logs, 0).tiStopped,
              L"TI apply: the log records the identity and that TrustedInstaller was started and stopped again");
        World b;
        freshWorld(&b);
        Store s2(NewTempDir(L"ti-off"));
        Plan p2 = BuildPreset(LV_STRIPPED, TAB_SERVICES, b.items, b.ctx);
        MarkTi(&p2, b.fx.backend, true, b.items);   // marked with the toggle on, then applied with it off
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
        MarkTi(&p3, b.fx.backend, true, b.items);
        check(RowOf(p3, L"TrustedInstaller") && !RowOf(p3, L"TrustedInstaller")->blocked.empty(),
              L"TI marking: the TrustedInstaller service itself is blocked while the toggle is on");
    }

    // ---- the interrupted fixture --------------------------------------------------------------------------
    {
        std::vector<std::wstring> problems;
        auto logs = LoadLogs(Store(fixture + L"\\interrupted"), &problems);
        World iw;
        std::wstring ie;
        MakeWorld(fixture + L"\\interrupted", &kb, &iw, &ie);
        check(logs.size() == 1 && FindInterrupted(logs) == 0 && TiLeftRunning(logs, iw.items),
              L"interrupted fixture: offers Undo/Resume and notices TrustedInstaller left running (the same process)");
    }

    // ---- A4: undo and resume guards, foreign logs -------------------------------------------------------------
    {
        World f;
        std::wstring e;
        bool okw = MakeWorld(fixture + L"\\forged", &kb, &f, &e);
        auto logs = LoadLogs(Store(fixture + L"\\forged"), nullptr);
        int k = FindUndoable(logs);
        Plan u = k >= 0 ? BuildUndo(At(logs, k), f.fx.backend, f.items, f.ctx) : Plan{};
        MarkTi(&u, f.fx.backend, false, f.items);
        bool allBlocked = u.rows.size() == 3;
        for (auto& r : u.rows) if (r.blocked.empty() || r.include || r.canToggle) allBlocked = false;
        check(okw && k == 0 && allBlocked && u.Included() == 0 && RowOf(u, L"stornvme") && RowOf(u, L"disk") && RowOf(u, L"RpcSs"),
              L"A4: a forged undo log (valid CRCs) can't set stornvme, disk or RpcSs to Disabled: every row blocked");
        UndoLog other = logs.empty() ? UndoLog{} : At(logs, 0);
        other.machine = L"another-machine";
        Plan um = BuildUndo(other, f.fx.backend, f.items, f.ctx);
        Plan rm = BuildResume(other, f.fx.backend, f.items, f.ctx);
        UndoLog oldBuild = logs.empty() ? UndoLog{} : At(logs, 0);
        oldBuild.build = L"22631";
        Plan ub = BuildUndo(oldBuild, f.fx.backend, f.items, f.ctx);
        auto refused = [](const Plan& p) { return p.rows.empty() && !p.notes.empty() && p.notes[0].rfind(L"Refused", 0) == 0; };
        check(refused(um) && refused(rm) && refused(ub), L"A4: Undo and Resume refuse a log from another machine GUID or another build");
        // Resume through the same guards: a log that would raise a service to System start.
        UndoLog rs = logs.empty() ? UndoLog{} : At(logs, 0);
        rs.hasE = false;
        rs.rows.clear();
        UndoRow ur;
        ur.seq = 1; ur.name = L"MapsBroker"; ur.before = ItemOf(f.items, L"MapsBroker")->cfg; ur.after = StartCfg{1, -1};
        rs.rows.push_back(ur);
        Plan rp = BuildResume(rs, f.fx.backend, f.items, f.ctx);
        check(rp.Included() == 0 && RowOf(rp, L"MapsBroker") && !RowOf(rp, L"MapsBroker")->blocked.empty(),
              L"A4: Resume refuses a row that would give a service Boot or System start");
        // Records dropped for a bad CRC are shown on the plan (logic M9).
        World it;
        MakeWorld(fixture + L"\\interrupted", &kb, &it, &e);
        auto il = LoadLogs(Store(fixture + L"\\interrupted"), nullptr);
        Plan iu = il.empty() ? Plan{} : BuildUndo(il[0], it.fx.backend, it.items, it.ctx);
        bool bad = false;
        for (auto& n : iu.notes) if (n.find(L"failed their check") != std::wstring::npos) bad = true;
        check(bad, L"Undo shows how many log records failed their check and were ignored (logic M9)");
    }

    // ---- A7 in Undo and Resume ----------------------------------------------------------------------------------
    {
        World a;
        freshWorld(&a);
        a.fx.backend.SetStart(L"MapsBroker", WriteOp{3, 1}, Identity::Admin, &err);   // Manual, DelayedAutostart=1 left over
        UndoLog l;
        l.hasB = true; l.hasE = true; l.stamp = L"20261111-090000"; l.source = L"single"; l.count = 1;
        l.machine = a.ctx.machine; l.build = a.ctx.build;
        UndoRow r;
        r.seq = 1; r.name = L"MapsBroker"; r.before = StartCfg{2, -1}; r.after = StartCfg{3, -1};
        r.hasI = true; r.hasD = true; r.result = L"ok"; r.observed = StartCfg{2, -1}; r.afterObs = StartCfg{3, 1};
        l.rows.push_back(r);
        Plan u = BuildUndo(l, a.fx.backend, a.items, a.ctx);
        const PlanRow* m = RowOf(u, L"MapsBroker");
        check(m && m->op.start == 2 && m->op.delayed == 0 && m->after.delayed == 0,
              L"A7: Undo to plain Auto writes DelayedAutostart=0 over a lingering 1 (not Auto (delayed))");
        UndoLog rl = l;
        rl.hasE = false;
        rl.rows[0].hasD = false;
        rl.rows[0].before = StartCfg{3, 1};
        rl.rows[0].after = StartCfg{2, -1};
        Plan rp = BuildResume(rl, a.fx.backend, a.items, a.ctx);
        const PlanRow* mr = RowOf(rp, L"MapsBroker");
        check(mr && mr->op.start == 2 && mr->op.delayed == 0, L"A7: Resume compares the raw delayed value too");
    }

    // ---- A6: dependencies to a fixed point, both directions, on toggles, at apply time -----------------------------
    {
        std::vector<SvcItem> items = {Mk(L"aaa", 3, {}), Mk(L"bbb", 3, {L"aaa"}), Mk(L"ccc", 3, {L"bbb"})};
        Plan p;
        p.rows = {Lower4(items[0]), Lower4(items[1])};
        DependencyCheck(&p, items);
        check(!p.rows[0].include && !p.rows[1].include && !p.rows[0].depWarning.empty() && !p.rows[1].depWarning.empty(),
              L"A6: aaa <- bbb <- ccc, ccc stays enabled: bbb and aaa are both unticked (fixed point, any row order)");
        Plan q;
        q.rows = {Lower4(items[0]), Lower4(items[1]), Lower4(items[2], false)};
        DependencyCheck(&q, items);
        bool off = !q.rows[0].include && !q.rows[1].include;
        q.rows[2].include = true;   // the user ticks ccc
        DependencyCheck(&q, items, 2);
        bool back = q.rows[0].include && q.rows[1].include && q.rows[0].depWarning.empty() && q.rows[1].depWarning.empty();
        q.rows[1].include = false;  // the user unticks bbb
        DependencyCheck(&q, items, 1);
        check(off && back && !q.rows[0].include && !q.rows[0].depWarning.empty() && q.rows[2].include,
              L"A6: re-run on every toggle: ticking ccc clears and re-ticks bbb and aaa; unticking bbb unticks aaa with a warning");
        std::vector<SvcItem> up = {Mk(L"ddd", 4, {}), Mk(L"eee", 4, {L"ddd"})};
        Plan r;
        PlanRow re;
        re.name = L"eee"; re.before = up[1].cfg; re.after = StartCfg{3, -1}; re.op = WriteOp{3, -1}; re.deps = up[1].deps;
        r.rows = {re};
        DependencyCheck(&r, up);
        bool warned = !r.rows[0].include && r.rows[0].depWarning.find(L"ddd") != std::wstring::npos;
        PlanRow rd = re;
        rd.name = L"ddd"; rd.deps.clear();
        r.rows.push_back(rd);
        r.rows[1].include = true;
        DependencyCheck(&r, up, 1);
        check(warned && r.rows[0].include && r.rows[0].depWarning.empty(),
              L"A6: enable direction: raising eee while ddd stays Disabled warns and unticks; raising ddd too clears it");
        // Apply time (logic M1): ccc fails, so bbb is skipped, so aaa must be skipped too.
        for (int mode = 0; mode < 2; mode++) {
            FakeBackend fb;
            fb.SetItems(items);
            if (mode == 0) fb.faults.deny = {L"ccc"};
            else fb.faults.changeOnQuery = {L"ccc"};   // someone set ccc to Auto since the plan
            Plan pa;
            pa.rows = {Lower4(items[0]), Lower4(items[1]), Lower4(items[2])};
            Store s(NewTempDir(mode ? L"deps-changed" : L"deps-fail"));
            ApplyResult ar;
            RunApply(pa, EnvFor(&fb, &s), &ar);
            bool aSkipped = false, bSkipped = false;
            for (auto& l : ar.lines) {
                if (l.rfind(L"skip aaa", 0) == 0) aSkipped = true;
                if (l.rfind(L"skip bbb", 0) == 0) bSkipped = true;
            }
            int aStart = -1;
            for (auto& i : fb.Items()) if (i.name == L"aaa") aStart = i.cfg.start;
            check(aSkipped && bSkipped && aStart == 3,
                  mode ? L"A6 apply: ccc changed to enabled since the plan -> bbb and then aaa are skipped, not disabled"
                       : L"A6 apply: ccc fails -> bbb skipped -> aaa skipped too (a skipped dependent protects its dependencies)");
        }
    }

    // ---- A8: rows MarkTi leaves out count as staying enabled -------------------------------------------------------
    {
        std::vector<SvcItem> items = {Mk(L"aaa", 3, {}), Mk(L"bbb", 3, {L"aaa"})};
        items[1].aclTi = true;   // Windows refuses the admin write to bbb: it needs TrustedInstaller
        FakeBackend fb;
        fb.SetItems(items);
        // What the module does on every TI toggle change: build the plan again, then MarkTi with the toggle's state.
        auto build = [&](bool tiOn) {
            Plan p;
            p.rows = {Lower4(items[0]), Lower4(items[1])};
            DependencyCheck(&p, items);   // the builders' own check: both disabled, nothing to warn about
            MarkTi(&p, fb, tiOn, items);
            return p;
        };
        Plan off = build(false);
        const PlanRow* a = RowOf(off, L"aaa");
        const PlanRow* b = RowOf(off, L"bbb");
        bool offOk = a && b && !b->include && !b->canToggle && !a->include && a->depWarning.find(L"bbb") != std::wstring::npos;
        Plan on = build(true);
        a = RowOf(on, L"aaa");
        b = RowOf(on, L"bbb");
        bool onOk = a && b && a->include && b->include && a->depWarning.empty() && b->needsTi;
        Plan off2 = build(false);
        a = RowOf(off2, L"aaa");
        check(offOk && onOk && a && !a->include && DepViolation(off, items).empty() && DepViolation(on, items).empty() &&
                  DepViolation(off2, items).empty(),
              L"A8: aaa <- bbb(needs TI): toggle off leaves bbb out and unticks aaa with a warning; on ticks both; off again unticks aaa");
        // The user ticks aaa anyway: kept, with the warning; MarkTi re-running (a rebuild keeps nothing) is the module's job.
        int ka = -1;
        for (int k = 0; k < (int)off.rows.size(); k++) if (off.rows[k].name == L"aaa") ka = k;
        if (ka >= 0) { off.rows[ka].include = true; DependencyCheck(&off, items, ka); }
        check(ka >= 0 && off.rows[ka].include && off.rows[ka].depKept && !off.rows[ka].depWarning.empty() &&
                  DepViolation(off, items).empty(),
              L"A8: ticking aaa by hand keeps it ticked and shows the warning (the user's override)");
        // The same through a real plan source: Bloat on the fixture with bbb-style TI rows marked by the fixture's ACLs.
        World w;
        freshWorld(&w);
        bool all = true;
        for (int lv = 0; lv < 3; lv++)
            for (int tab : {TAB_SERVICES, TAB_DRIVERS})
                for (bool tiOn : {false, true}) {
                    w.ctx.tiOn = tiOn;
                    Plan p = PlanFor(lv, tab, w);
                    if (std::wstring v = DepViolation(p, w.items); !v.empty()) { all = false; info(L"A8 fixture: " + v); }
                }
        for (int lv : {-1, -2}) {
            w.ctx.tiOn = false;
            Plan p = PlanFor(lv, TAB_SERVICES, w);
            if (std::wstring v = DepViolation(p, w.items); !v.empty()) { all = false; info(L"A8 fixture: " + v); }
        }
        w.ctx.tiOn = false;
        check(all, L"A8: every fixture preset, Default and stock plan, TI toggle off and on, holds the dependency invariant after MarkTi");
    }

    // ---- A9: plans that raise and lower settle, and end consistent --------------------------------------------------
    {
        // The minimal circle: raising yyy needs xxx, and another row disables xxx. Each warning exists only while the
        // other row is ticked, so the old loop flipped both until the round limit.
        std::vector<SvcItem> items = {Mk(L"xxx", 3, {}), Mk(L"yyy", 4, {L"xxx"})};
        Plan p;
        PlanRow ry;
        ry.name = L"yyy"; ry.before = items[1].cfg; ry.after = StartCfg{2, -1}; ry.op = WriteOp{2, -1}; ry.deps = items[1].deps;
        p.rows = {Lower4(items[0]), ry};
        DependencyCheck(&p, items);
        bool note = false;
        for (auto& n : p.notes) if (n.rfind(kDepNotSettled, 0) == 0) note = true;
        bool pairOk = !p.rows[0].include && !p.rows[1].include && !p.rows[0].depWarning.empty() && !p.rows[1].depWarning.empty() &&
                      note && DepViolation(p, items).empty();
        p.rows[1].include = true;   // the user ticks yyy
        DependencyCheck(&p, items, 1);
        int notes = 0;
        for (auto& n : p.notes) if (n.rfind(kDepNotSettled, 0) == 0) notes++;
        check(pairOk && p.rows[1].include && p.rows[1].depWarning.empty() && !p.rows[0].include && notes <= 1 &&
                  DepViolation(p, items, 1).empty(),
              L"A9: a raise/disable circle ends with both unticked, warned, and a 'did not settle' note; ticking yyy then settles");

        // Analyze's fuzz case 53, every combination of initial ticks.
        std::vector<SvcItem> s53 = {Mk(L"s0", 4, {L"s1", L"s2"}), Mk(L"s1", 2, {L"s3"}), Mk(L"s2", 3, {L"s0"}), Mk(L"s3", 3, {L"s0"})};
        const int to[4] = {2, 4, 2, 2};
        bool ok53 = true;
        for (int mask = 0; mask < 16; mask++) {
            Plan q;
            for (int i = 0; i < 4; i++) {
                PlanRow r;
                r.name = s53[i].name; r.before = s53[i].cfg; r.after = StartCfg{to[i], -1}; r.op = WriteOp{to[i], -1};
                r.deps = s53[i].deps; r.include = (mask >> i) & 1;
                q.rows.push_back(r);
            }
            DependencyCheck(&q, s53);
            if (std::wstring v = DepViolation(q, s53); !v.empty()) { ok53 = false; info(L"A9 case 53 mask " + std::to_wstring(mask) + L": " + v); }
            for (int k = 0; k < 4; k++) {
                q.rows[k].include = !q.rows[k].include;
                DependencyCheck(&q, s53, k);
                if (std::wstring v = DepViolation(q, s53, k); !v.empty()) { ok53 = false; info(L"A9 case 53 toggle: " + v); }
            }
        }
        check(ok53, L"A9: Analyze's fuzz case 53 (s2 raised while s0 stays Disabled) holds the invariant for all 16 starting ticks and every toggle");

        // Analyze's fuzzer, built in: the same generator and seed (209 bad plans before the fix, 259 after a toggle),
        // checked with the stricter evaluator above, then a run with groups, user instances and toggle sequences.
        struct Fz { int plans = 0, bad = 0, unsettled = 0; double worstMs = 0; std::wstring first; };
        auto fuzz = [&](unsigned seed, int iters, bool rich, bool lowerOnly) {
            Fz f;
            std::mt19937 rng(seed);
            for (int it = 0; it < iters; it++) {
                int n = 2 + rng() % 5;
                std::vector<SvcItem> its(n);
                for (int i = 0; i < n; i++) {
                    its[i].name = L"s" + std::to_wstring(i); its[i].type = 0x10; its[i].kind = Kind::Service;
                    its[i].cfg = StartCfg{2 + (int)(rng() % 3), -1};
                    for (int j = 0; j < n; j++) if (j != i && rng() % 3 == 0) its[i].deps.push_back(L"s" + std::to_wstring(j));
                    if (rich) {
                        if (rng() % 3 == 0) its[i].group = L"g" + std::to_wstring(rng() % 2);
                        if (rng() % 4 == 0) its[i].deps.push_back(L"+g" + std::to_wstring(rng() % 2));
                        if (rng() % 8 == 0) its[i].kind = Kind::UserInstance;
                    }
                }
                Plan p2;
                for (int i = 0; i < n; i++) {
                    if (rng() % 3 == 0) continue;
                    int t = 2 + rng() % 3;
                    if (lowerOnly ? t <= its[i].cfg.start : t == its[i].cfg.start) continue;
                    PlanRow r;
                    r.name = its[i].name; r.before = its[i].cfg; r.after = {t, -1}; r.op = {t, -1}; r.deps = its[i].deps;
                    r.include = rng() % 4 != 0;
                    if (rich && rng() % 10 == 0) { r.blocked = L"blocked"; r.include = false; r.canToggle = false; }
                    p2.rows.push_back(r);
                }
                if (p2.rows.empty()) continue;
                f.plans++;
                auto t0 = std::chrono::steady_clock::now();
                DependencyCheck(&p2, its);
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (ms > f.worstMs) f.worstMs = ms;
                for (auto& nt : p2.notes) if (nt.rfind(kDepNotSettled, 0) == 0) f.unsettled++;
                std::wstring v = DepViolation(p2, its);
                for (int tg = 0; v.empty() && tg < (rich ? 4 : 1); tg++) {
                    int k = rng() % p2.rows.size();
                    if (!p2.rows[k].canToggle) continue;
                    p2.rows[k].include = !p2.rows[k].include;
                    DependencyCheck(&p2, its, k);
                    v = DepViolation(p2, its, k);
                }
                if (!v.empty()) { if (!f.bad) f.first = L"iteration " + std::to_wstring(it) + L": " + v; f.bad++; }
            }
            return f;
        };
        Fz a1 = fuzz(12345, 60000, false, false), a2 = fuzz(12345, 60000, false, true), a3 = fuzz(4242, 30000, true, false);
        for (const Fz* f : {&a1, &a2, &a3}) {
            wchar_t buf[200];
            swprintf(buf, 200, L"A9 fuzz: %d plans, %d broke the invariant, %d did not settle, worst %.2f ms", f->plans, f->bad,
                     f->unsettled, f->worstMs);
            info(buf + (f->first.empty() ? std::wstring() : L"; first: " + f->first));
        }
        check(a1.plans == 52177 && a1.bad == 0 && a2.plans == 36803 && a2.bad == 0 && a3.bad == 0,
              L"A9: Analyze's fuzzer (seed 12345, 52,177 mixed and 36,803 lower-only plans) and a 30,000-plan run with groups, "
              L"user instances, blocked rows and toggle sequences: no plan breaks the dependency invariant");

        // The cost case: a circle inside a 68-row plan over 757 items took 1.7 s per toggle under Wine.
        std::vector<SvcItem> big = items;
        for (int i = 0; i < 755; i++) big.push_back(Mk((L"f" + std::to_wstring(i)).c_str(), 3, {L"f" + std::to_wstring((i + 1) % 755)}));
        Plan bp;
        bp.rows = p.rows;
        for (auto& r : bp.rows) { r.include = true; r.depUnticked = r.depKept = false; r.depWarning.clear(); }
        for (int i = 0; i < 66; i++) bp.rows.push_back(Lower4(big[2 + i * 11]));
        auto t0 = std::chrono::steady_clock::now();
        DependencyCheck(&bp, big);
        bp.rows[3].include = !bp.rows[3].include;
        DependencyCheck(&bp, big, 3);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        info(L"A9: a circle in a 68-row plan over 757 items, build plus one toggle: " + std::to_wstring((int)ms) + L" ms");
        check(DepViolation(bp, big, 3).empty() && ms < 1000,
              L"A9: a circle inside a 68-row plan over 757 items settles (state repeat detected) and holds the invariant");

        // Undo plans run the check too: undoing yyy back to Manual while xxx is Disabled (set since, by hand).
        World u;
        freshWorld(&u);
        std::vector<SvcItem> ui = {Mk(L"xxx", 4, {}), Mk(L"yyy", 4, {L"xxx"})};
        FakeBackend ub;
        ub.SetItems(ui);
        UndoLog l;
        l.hasB = true; l.hasE = true; l.stamp = L"20261111-100000"; l.source = L"single"; l.count = 1;
        l.machine = u.ctx.machine; l.build = u.ctx.build;
        UndoRow r;
        r.seq = 1; r.name = L"yyy"; r.before = StartCfg{3, -1}; r.after = StartCfg{4, -1};
        r.hasI = true; r.hasD = true; r.result = L"ok"; r.observed = StartCfg{3, -1}; r.afterObs = StartCfg{4, -1};
        l.rows.push_back(r);
        Plan up = BuildUndo(l, ub, ui, u.ctx);
        const PlanRow* y = RowOf(up, L"yyy");
        check(y && !y->include && y->depWarning.find(L"xxx") != std::wstring::npos && DepViolation(up, ui).empty(),
              L"A9: Undo runs the dependency check: raising yyy back while xxx stays Disabled is unticked with a warning");
    }

    // ---- A3: TrustedInstaller is stopped only when this apply started it --------------------------------------------
    {
        World a;
        freshWorld(&a);
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        Store s(NewTempDir(L"ti-svc"));
        s.fixedStamp = L"20261111-120000";
        ServiceOnlyTi ti(&a.fx.backend);
        Plan p = BuildPreset(LV_STRIPPED, TAB_SERVICES, a.items, a.ctx);
        MarkTi(&p, a.fx.backend, true, a.items);
        Tick(&p, L"dmwappushservice");
        ApplyEnv env = EnvFor(&a.fx.backend, &s);
        env.ti = &ti;
        env.tiOn = true;
        ApplyResult r1;
        bool ok1 = RunApply(p, env, &r1);
        auto stateOf = [&](const wchar_t* n) { for (auto& i : a.fx.backend.Items()) if (i.name == n) return i.state; return (DWORD)0; };
        DWORD after1 = stateOf(L"TrustedInstaller");
        // Windows (or the user) starts TrustedInstaller before the next apply.
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_RUNNING);
        a.items = Folded(a.fx.backend);
        Plan p2 = BuildSingle(*ItemOf(a.items, L"dmwappushservice"), StartCfg{3, -1}, a.items, a.ctx);
        MarkTi(&p2, a.fx.backend, true, a.items);
        ApplyResult r2;
        bool ok2 = RunApply(p2, env, &r2);
        auto logs = LoadLogs(s, nullptr);
        int stops = 0;
        for (auto& c : a.fx.backend.controlLog) if (c == L"stop TrustedInstaller") stops++;
        check(ok1 && ok2 && r1.ok >= 1 && r2.ok == 1 && after1 == SERVICE_STOPPED && stateOf(L"TrustedInstaller") == SERVICE_RUNNING &&
                  stops == 1 && logs.size() == 2 && At(logs, 0).tiStarted && At(logs, 0).tiStopped && !At(logs, 1).tiStarted && !At(logs, 1).tiStopped,
              L"A3: two applies; TrustedInstaller already running before the second -> Hadron neither claims nor stops it");
        TiService pend(&a.fx.backend);
        pend.waitMs = 0;
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_START_PENDING);
        SvcItem t;
        std::wstring e;
        size_t before = a.fx.backend.controlLog.size();
        bool en = pend.Ensure(&t, &e);
        std::wstring note;
        check(!en && !pend.Started() && !pend.StopIfStarted(&note) && a.fx.backend.controlLog.size() == before,
              L"A3: START_PENDING is someone else's start: never started, claimed or stopped by Hadron");
        // Round 2: Windows starts TI between Ensure's Query and its start request (ERROR_SERVICE_ALREADY_RUNNING).
        TiService race(&a.fx.backend);
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        a.fx.backend.faults.startRace.insert(L"trustedinstaller");
        bool rOk = race.Ensure(&t, &e);
        std::wstring rNote;
        bool rClaimed = race.Started(), rStopped = race.StopIfStarted(&rNote);
        check(rOk && !rClaimed && !rStopped && t.state == SERVICE_RUNNING,
              L"A3 r2: a start that finds TrustedInstaller already running (Control: not our own start) is never claimed or stopped");
        // Round 2: STOP_PENDING is waited out, then Hadron starts it and owns that start.
        TiService sp(&a.fx.backend);
        sp.pollMs = 1;
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOP_PENDING);
        a.fx.backend.faults.stopPendingQueries[L"trustedinstaller"] = 2;
        bool sOk = sp.Ensure(&t, &e);
        bool sClaimed = sp.Started();
        std::wstring sNote;
        bool sStopped = sp.StopIfStarted(&sNote);
        check(sOk && sClaimed && sStopped && stateOf(L"TrustedInstaller") == SERVICE_STOPPED,
              L"A3 r2: STOP_PENDING TrustedInstaller is waited for, then started, claimed and stopped again (not a failed row)");
        // win32 M2: an undo-log failure after the token was taken still puts TrustedInstaller back.
        World b;
        freshWorld(&b);
        Store s2(NewTempDir(L"ti-flush"));
        Plan p3 = BuildPreset(LV_STRIPPED, TAB_SERVICES, b.items, b.ctx);
        MarkTi(&p3, b.fx.backend, true, b.items);
        Tick(&p3, L"dmwappushservice");
        FakeTi fti;
        ApplyEnv e3 = EnvFor(&b.fx.backend, &s2);
        e3.ti = &fti;
        e3.tiOn = true;
        e3.failFlushAfter = 3 + (int)ApplyOrder(p3).size();   // header, B, every P, T starting: then T started fails
        ApplyResult r3;
        bool ok3 = RunApply(p3, e3, &r3);
        check(!ok3 && r3.aborted && fti.acquired && fti.released && b.fx.backend.faults.setStartCalls == 0,
              L"TI: an undo-log flush failure after Acquire still releases TrustedInstaller (win32 M2), nothing changed");
    }

    // ---- logs: order, resolution, interrupted undo, TI marker -------------------------------------------------------
    {
        auto mkLog = [](const wchar_t* stamp, const wchar_t* source, bool end, int order) {
            UndoLog l;
            l.hasB = true; l.hasE = end; l.stamp = stamp; l.source = source; l.order = order;
            return l;
        };
        Store s(NewTempDir(L"order"));
        auto write = [&](const UndoLog& l) {
            std::string t = "HXSVC\t1\tundo\n" + Record({L"B", l.stamp, Esc(l.source), L"0", L"m", L"19045", std::to_wstring(l.order)});
            if (l.hasE) t += Record({L"E", l.stamp, L"0", L"0", L"0", L"done"});
            std::wstring e;
            s.WriteNew(L"undo-" + l.stamp + L".log", t, &e);
        };
        write(mkLog(L"20261111-101500-10", L"preset:bloat:services", true, 0));
        write(mkLog(L"20261111-101500-2", L"preset:bloat:services", true, 0));
        write(mkLog(L"20261101-000000", L"preset:stripped:services", true, 7));
        auto logs = LoadLogs(s, nullptr);
        check(logs.size() == 3 && At(logs, 0).stamp == L"20261111-101500-2" && At(logs, 1).stamp == L"20261111-101500-10" &&
                  At(logs, 2).stamp == L"20261101-000000",
              L"log order: the B record's sequence number first, then the stamp with -N compared as a number (-2 before -10)");
        std::vector<UndoLog> v = {mkLog(L"20261111-110000", L"undo:20261111-120000", true, 0), mkLog(L"20261111-120000", L"preset:bloat:services", false, 0)};
        check(FindInterrupted(v) < 0, L"an undo stamped earlier than its apply (clock change) still resolves it (logic M4)");
        std::vector<UndoLog> t2 = {mkLog(L"20261111-100000", L"preset:stripped:services", true, 1), mkLog(L"20261111-110000", L"preset:bloat:services", true, 2)};
        t2[0].tiStarted = true;
        t2[0].tiPid = 4242;
        t2[0].tiPstart = FakeProcStart(4242);
        SvcItem tiRun;
        tiRun.name = L"TrustedInstaller"; tiRun.state = SERVICE_RUNNING; tiRun.pid = 4242; tiRun.pstart = FakeProcStart(4242);
        check(TiLeftRunning(t2, {tiRun}), L"TiLeftRunning checks every log: a later apply doesn't hide a TrustedInstaller left running (win32 M15)");
        // The multi fixture: an interrupted apply A, then an interrupted undo of A.
        std::vector<std::wstring> probs;
        auto ml = LoadLogs(Store(fixture + L"\\multi"), &probs);
        int k = FindInterrupted(ml);
        int base = UndoBase(ml, k);
        bool shape = ml.size() == 2 && k == 1 && base == 0 && ml[1].source == L"undo:" + ml[0].stamp;
        World m;
        std::wstring e;
        MakeWorld(fixture + L"\\multi", &kb, &m, &e);
        Plan u = shape ? BuildUndo(ml[base], m.fx.backend, m.items, m.ctx) : Plan{};
        const PlanRow* mb = RowOf(u, L"MapsBroker");
        check(shape && mb && mb->include && mb->after.start == 2 && mb->after.delayed == 1,
              L"interrupted undo: the banner's Undo restarts undoing the apply (MapsBroker back to Auto (delayed)), never undoes the undo");
        std::vector<UndoLog> done = ml;
        done.push_back(mkLog(L"20261110-230000", (L"resume:" + (ml.size() > 1 ? ml[1].stamp : std::wstring())).c_str(), true, 3));
        std::vector<UndoLog> done2 = ml;
        done2.push_back(mkLog(L"20261110-230000", (L"undo:" + (ml.empty() ? std::wstring() : ml[0].stamp)).c_str(), true, 3));
        check(FindInterrupted(done) < 0 && FindInterrupted(done2) < 0,
              L"interrupted undo: a finished resume of the undo, or a finished undo of the apply, resolves both logs");
    }

    // ---- R4: TrustedInstaller claims by process (pid + creation time), resolutions; R14; R22 ------------------------
    {
        World a;
        freshWorld(&a);
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        Store s(NewTempDir(L"ti-claim"));
        s.fixedStamp = L"20261111-130000";
        ServiceOnlyTi ti(&a.fx.backend);
        Plan p = BuildPreset(LV_STRIPPED, TAB_SERVICES, a.items, a.ctx);
        MarkTi(&p, a.fx.backend, true, a.items);
        Tick(&p, L"dmwappushservice");
        ApplyEnv env = EnvFor(&a.fx.backend, &s);
        env.ti = &ti;
        env.tiOn = true;
        a.fx.backend.faults.stopFail.insert(L"trustedinstaller");   // the release fails: TI is left running
        ApplyResult r1;
        RunApply(p, env, &r1);
        a.fx.backend.faults.stopFail.clear();
        auto logs = LoadLogs(s, nullptr);
        auto items = Folded(a.fx.backend);
        const SvcItem* tiNow = ItemOf(items, L"TrustedInstaller");
        bool rec = logs.size() == 1 && tiNow && tiNow->state == SERVICE_RUNNING && At(logs, 0).tiStarting && At(logs, 0).tiStarted &&
                   At(logs, 0).tiPid == tiNow->pid && At(logs, 0).tiPid != 0 && At(logs, 0).tiPstart == tiNow->pstart && At(logs, 0).tiStopFailed &&
                   !At(logs, 0).tiStopped && !At(logs, 0).tiStopNote.empty();
        check(rec, L"R4: the log records T starting, the started process (pid and creation time) and the failed release (T stopfailed)");
        std::wstring bt = TiLeftText(logs, items);
        check(rec && TiLeftRunning(logs, items) && bt.find(L"process " + std::to_wstring(tiNow->pid)) != std::wstring::npos &&
                  bt.find(L"2026-11-11 13:00 UTC") != std::wstring::npos && bt.find(L"Stripped") != std::wstring::npos &&
                  bt.find(L"its stop failed") != std::wstring::npos && bt.find(L"preset:") == std::wstring::npos,
              L"R4: the banner claims TrustedInstaller while it is that process; its text (process, stamp, preset, stop failure) comes from data");
        // The banner's Stop it, while it is still that process: stopped, and the claim is closed by a ti-resolved log.
        Plan sp = BuildControl(*tiNow, 1, a.fx.backend, items, a.ctx);
        MarkTiStop(&sp, logs, items);
        const PlanRow* sr = RowOf(sp, L"TrustedInstaller");
        check(sp.source == L"ti-stop:20261111-130000" && sp.tiPid == tiNow->pid && sr && sr->blocked.empty() &&
                  sr->reason.find(L"(" + std::to_wstring(tiNow->pid) + L")") != std::wstring::npos,
              L"R4: Stop it plans the claimed process, with the reason from the log");
        // Windows restarts TrustedInstaller between the plan and the click: a different process, never stopped by Hadron.
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        a.fx.backend.SetState(L"TrustedInstaller", SERVICE_RUNNING);
        auto items2 = Folded(a.fx.backend);
        size_t stopsBefore = 0;
        for (auto& c : a.fx.backend.controlLog) if (c == L"stop TrustedInstaller") stopsBefore++;
        check(!TiLeftRunning(logs, items2), L"R4: a different TrustedInstaller process (same service, new pid/creation time) is never claimed");
        ApplyResult rc;
        RunControl(sp, env, &rc);
        size_t stopsAfter = 0;
        for (auto& c : a.fx.backend.controlLog) if (c == L"stop TrustedInstaller") stopsAfter++;
        auto logs2 = LoadLogs(s, nullptr);
        bool resolved = false;
        for (auto& l : logs2) if (l.source == L"ti-resolved:20261111-130000" && l.hasE && l.rows.empty() && !l.note.empty()) resolved = true;
        check(stopsAfter == stopsBefore && rc.skipped == 1 && resolved && !TiLeftRunning(logs2, items) &&
                  FindInterrupted(logs2) < 0,
              L"R4: Stop it re-checks the process at run time; a new process is left to Windows and the claim is recorded as resolved");
        // Same process at click time: stopped, and resolved.
        World b;
        freshWorld(&b);
        b.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        Store s2(NewTempDir(L"ti-stopit"));
        s2.fixedStamp = L"20261111-131000";
        ServiceOnlyTi ti2(&b.fx.backend);
        Plan p2 = BuildPreset(LV_STRIPPED, TAB_SERVICES, b.items, b.ctx);
        MarkTi(&p2, b.fx.backend, true, b.items);
        Tick(&p2, L"dmwappushservice");
        ApplyEnv env2 = EnvFor(&b.fx.backend, &s2);
        env2.ti = &ti2;
        env2.tiOn = true;
        b.fx.backend.faults.stopFail.insert(L"trustedinstaller");
        ApplyResult r2;
        RunApply(p2, env2, &r2);
        b.fx.backend.faults.stopFail.clear();
        auto bl = LoadLogs(s2, nullptr);
        auto bi = Folded(b.fx.backend);
        Plan sp2 = BuildControl(*ItemOf(bi, L"TrustedInstaller"), 1, b.fx.backend, bi, b.ctx);
        MarkTiStop(&sp2, bl, bi);
        ApplyResult rc2;
        bool okc = RunControl(sp2, env2, &rc2);
        auto bl2 = LoadLogs(s2, nullptr);
        auto bi2 = Folded(b.fx.backend);
        bool res2 = false;
        for (auto& l : bl2) if (l.source == L"ti-resolved:20261111-131000") res2 = true;
        check(okc && rc2.ok == 1 && ItemOf(bi2, L"TrustedInstaller")->state == SERVICE_STOPPED && res2,
              L"R4: Stop it on the process Hadron started stops it and records the resolution");
        // No live claim: every row of the Stop it plan is blocked.
        Plan sp3 = BuildControl(*ItemOf(items2, L"TrustedInstaller"), 1, a.fx.backend, items2, a.ctx);
        MarkTiStop(&sp3, logs, items2);
        check(sp3.Included() == 0 && RowOf(sp3, L"TrustedInstaller") && !RowOf(sp3, L"TrustedInstaller")->blocked.empty(),
              L"R4: with no live claim, Stop it plans nothing");
        // Load time: a claim whose process is gone is closed once, and not while an apply holds the lock.
        Store s3(NewTempDir(L"ti-stale"));
        std::string t = "HXSVC\t1\tundo\n" + Record({L"B", L"20261111-132000", L"preset:bloat:services", L"0", a.ctx.machine, a.ctx.build, L"1"}) +
                        Record({L"T", L"starting"}) + Record({L"T", L"started", L"4242", L"01DC000000001092"}) +
                        Record({L"E", L"20261111-132000", L"0", L"0", L"0", L"done"});
        std::wstring e;
        s3.WriteNew(L"undo-20261111-132000.log", t, &e);
        auto sl = LoadLogs(s3, nullptr);
        ApplyLock busy;
        busy.Acquire(LockName(), &e);
        int nBusy = ResolveStaleTiClaims(s3, sl, items2, a.ctx.machine, a.ctx.build, LockName());
        busy.Release();
        int n1 = ResolveStaleTiClaims(s3, sl, items2, a.ctx.machine, a.ctx.build, LockName());
        auto sl2 = LoadLogs(s3, nullptr);
        int n2 = ResolveStaleTiClaims(s3, sl2, items2, a.ctx.machine, a.ctx.build, LockName());
        check(nBusy == 0 && n1 == 1 && n2 == 0 && sl2.size() == 2 && At(sl2, 1).source == L"ti-resolved:20261111-132000" &&
                  At(sl2, 1).note.find(L"different TrustedInstaller process") != std::wstring::npos && FindUndoable(sl2) < 0,
              L"R4: load closes a claim whose process is gone with one ti-resolved log (not while the apply lock is busy)");
        // An older log's T started without pid and creation time is never claimed, even with TrustedInstaller running.
        UndoLog old;
        old.hasB = true; old.hasE = true; old.stamp = L"20261101-000000"; old.source = L"single"; old.tiStarted = true;
        check(!TiLeftRunning({old}, items2), L"R4: a T started record without the process identity is never shown as Hadron's");
        // R14: our start request was accepted but the wait for RUNNING failed: still claimed, so the release stops it.
        World c;
        freshWorld(&c);
        c.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        c.fx.backend.faults.startNoWait.insert(L"trustedinstaller");
        TiService nw(&c.fx.backend);
        SvcItem tt;
        bool en = nw.Ensure(&tt, &e);
        bool claimed = nw.Started();
        std::wstring note;
        bool stopped = nw.StopIfStarted(&note);
        check(!en && claimed && stopped && c.fx.backend.controlLog.back() == L"stop TrustedInstaller",
              L"R14: a TrustedInstaller start Hadron requested whose wait fails is still claimed and stopped by the release");
        // R22: Cancel cuts the STOP_PENDING wait short instead of waiting it out.
        World d;
        freshWorld(&d);
        d.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOP_PENDING);
        d.fx.backend.faults.stopPendingQueries[L"trustedinstaller"] = 1000000;
        std::atomic<bool> cancel{true};
        TiService cw(&d.fx.backend);
        cw.cancel = &cancel;
        cw.waitMs = 60000;
        cw.pollMs = 50;
        ULONGLONG t0 = GetTickCount64();
        std::wstring ce;
        bool cen = cw.Ensure(&tt, &ce);
        ULONGLONG dt = GetTickCount64() - t0;
        check(!cen && !cw.Started() && dt < 5000 && ce.find(L"cancelled") != std::wstring::npos,
              L"R22: TrustedInstaller's waits honour Cancel (no 30 s wait on a stuck service)");
        // R22: T starting is flushed before the start, so a crash in between still leaves a trace.
        World f;
        freshWorld(&f);
        f.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        Store s4(NewTempDir(L"ti-starting"));
        Plan p4 = BuildPreset(LV_STRIPPED, TAB_SERVICES, f.items, f.ctx);
        MarkTi(&p4, f.fx.backend, true, f.items);
        Tick(&p4, L"dmwappushservice");
        FakeTi fti;
        ApplyEnv e4 = EnvFor(&f.fx.backend, &s4);
        e4.ti = &fti;
        e4.tiOn = true;
        e4.failFlushAfter = 3 + (int)ApplyOrder(p4).size();   // header, B, every P, T starting: then T started fails
        ApplyResult r4;
        RunApply(p4, e4, &r4);
        auto l4 = LoadLogs(s4, nullptr);
        UndoLog crashed;   // power lost between T starting and T started
        crashed.hasB = true; crashed.stamp = L"20261111-133000"; crashed.source = L"preset:bloat:services"; crashed.tiStarting = true;
        check(fti.acquired && fti.released && l4.size() == 1 && At(l4, 0).tiStarting && !At(l4, 0).hasE &&
                  InterruptedText({crashed}, 0).find(L"while starting TrustedInstaller") != std::wstring::npos,
              L"R22: T starting is in the log before TrustedInstaller is started; the banner names a crash there");
    }

    // ---- R7, R8: resume chains in Undo; R9: Dismiss ---------------------------------------------------------------
    {
        World a;
        freshWorld(&a);
        std::wstring e;
        auto wlog = [&](Store& st, const std::wstring& stamp, const std::wstring& src, int order,
                        std::vector<std::vector<std::wstring>> rows, bool end) {
            std::string t = "HXSVC\t1\tundo\n" + Record({L"B", stamp, Esc(src), std::to_wstring(rows.size()), a.ctx.machine, a.ctx.build,
                                                         std::to_wstring(order)});
            int k = 1;
            for (auto& r : rows) t += Record({L"P", std::to_wstring(k++), r[0], L"start", r[1], r[2], L"admin"});
            k = 1;
            for (auto& r : rows) {
                t += Record({L"I", std::to_wstring(k), r[1], L"admin"});
                t += Record({L"D", std::to_wstring(k), L"ok", r[2], L"admin"});
                k++;
            }
            if (end) t += Record({L"E", stamp, std::to_wstring(rows.size()), L"0", L"0", L"done"});
            st.WriteNew(L"undo-" + stamp + L".log", t, &e);
        };
        // R7: apply A was interrupted after MapsBroker and lfsvc; its resume R changed MapsBroker again and RetailDemo.
        Store s(NewTempDir(L"resume-chain"));
        wlog(s, L"20261111-150000", L"preset:bloat:services", 1, {{L"MapsBroker", L"2/1", L"3/-"}, {L"lfsvc", L"3/-", L"4/-"}}, false);
        wlog(s, L"20261111-150500", L"resume:20261111-150000", 2, {{L"MapsBroker", L"3/-", L"4/-"}, {L"RetailDemo", L"3/-", L"4/-"}}, true);
        a.fx.backend.SetStart(L"MapsBroker", WriteOp{4, 0}, Identity::Admin, &e);
        a.fx.backend.SetStart(L"lfsvc", WriteOp{4, -1}, Identity::Admin, &e);
        a.fx.backend.SetStart(L"RetailDemo", WriteOp{4, -1}, Identity::Admin, &e);
        a.items = Folded(a.fx.backend);
        auto logs = LoadLogs(s, nullptr);
        int u = FindUndoable(logs);
        Plan up = u >= 0 ? BuildUndo(At(logs, u), a.fx.backend, a.items, a.ctx) : Plan{};
        const PlanRow* mb = RowOf(up, L"MapsBroker");
        const PlanRow* lf = RowOf(up, L"lfsvc");
        const PlanRow* rd = RowOf(up, L"RetailDemo");
        bool note = false;
        for (auto& n : up.notes) if (n.find(L"resumed 2026-11-11 15:00 UTC") != std::wstring::npos) note = true;
        check(FindInterrupted(logs) < 0 && u == 1 && mb && mb->after.start == 2 && mb->after.delayed == 1 && lf && lf->after.start == 3 &&
                  rd && rd->after.start == 3 && note,
              L"R7: the undo of a resume also puts back what the interrupted apply had changed (MapsBroker to its value before A)");
        // R8: apply X finished; its undo U was interrupted; a resume of U finished. Nothing is left to undo, and an
        // Undo from the resume starts from X.
        Store s2(NewTempDir(L"undo-resume"));
        wlog(s2, L"20261111-160000", L"preset:bloat:services", 1, {{L"lfsvc", L"3/-", L"4/-"}}, true);
        wlog(s2, L"20261111-160500", L"undo:20261111-160000", 2, {{L"lfsvc", L"4/-", L"3/-"}}, false);
        wlog(s2, L"20261111-161000", L"resume:20261111-160500", 3, {}, true);
        auto l2 = LoadLogs(s2, nullptr);
        check(l2.size() == 3 && FindUndoable(l2) < 0 && UndoBase(l2, 2) == 0 && UndoBase(l2, 1) == 0 && FindInterrupted(l2) < 0,
              L"R8: a resume of an undo is never offered for Undo, and Undo from it starts from the original apply");
        // R9: Dismiss records a dismiss log; the banner goes, the log and its rows stay.
        Store s3(NewTempDir(L"dismiss"));
        wlog(s3, L"20261111-170000", L"preset:stripped:services", 1, {{L"lfsvc", L"3/-", L"4/-"}}, false);
        auto l3 = LoadLogs(s3, nullptr);
        std::wstring bt = l3.empty() ? L"" : InterruptedText(l3, 0);
        ApplyLock busy;
        busy.Acquire(LockName(), &e);
        std::wstring be;
        bool dBusy = !l3.empty() && DismissInterrupted(s3, At(l3, 0), a.ctx.machine, a.ctx.build, LockName(), &be);
        busy.Release();
        bool dOk = !l3.empty() && DismissInterrupted(s3, At(l3, 0), a.ctx.machine, a.ctx.build, LockName(), &e);
        auto l4 = LoadLogs(s3, nullptr);
        check(FindInterrupted(l3) == 0 && !dBusy && dOk && l4.size() == 2 && FindInterrupted(l4) < 0 &&
                  At(l4, 1).source == L"dismiss:20261111-170000" && At(l4, 1).hasE && At(l4, 1).rows.empty() && At(l4, 0).rows.size() == 1 &&
                  bt.find(L"Dismiss") != std::wstring::npos && bt.find(L"Stripped (Services)") != std::wstring::npos &&
                  bt.find(L"preset:") == std::wstring::npos,
              L"R9: Dismiss writes a dismiss log under the apply lock; the banner goes and the apply's log stays");
    }

    // ---- Finalize round 2: mini-analyze A2-A6 and its four fixture runs (temp copies, fake backend only) -----------
    {
        World a;
        freshWorld(&a);
        std::wstring e;
        auto wlog = [&](Store& st, const std::wstring& stamp, const std::wstring& src, int order,
                        std::vector<std::vector<std::wstring>> rows, bool end) {
            std::string t = "HXSVC\t1\tundo\n" + Record({L"B", stamp, Esc(src), std::to_wstring(rows.size()), a.ctx.machine, a.ctx.build,
                                                         std::to_wstring(order)});
            int k = 1;
            for (auto& r : rows) t += Record({L"P", std::to_wstring(k++), r[0], L"start", r[1], r[2], L"admin"});
            k = 1;
            for (auto& r : rows) {
                t += Record({L"I", std::to_wstring(k), r[1], L"admin"});
                if (r.size() < 4) t += Record({L"D", std::to_wstring(k), L"ok", r[2], L"admin"});   // a 4th field: in progress
                k++;
            }
            if (end) t += Record({L"E", stamp, std::to_wstring(rows.size()), L"0", L"0", L"done"});
            st.WriteNew(L"undo-" + stamp + L".log", t, &e);
        };
        auto idx = [](const std::vector<UndoLog>& v, const wchar_t* stamp) {
            for (size_t k = 0; k < v.size(); k++) if (v[k].stamp == stamp) return (int)k;
            return -1;
        };
        // The live picture after those applies: what they wrote is in place.
        a.fx.backend.SetStart(L"MapsBroker", WriteOp{4, 1}, Identity::Admin, &e);
        a.fx.backend.SetStart(L"lfsvc", WriteOp{4, -1}, Identity::Admin, &e);
        a.items = Folded(a.fx.backend);
        // Run 1 (A2): an interrupted apply with D ok rows, then a finished dismiss: log. The banner goes, and Undo
        // (the header's Undo last apply, --plan undo-last) still puts back what it wrote.
        Store s1(NewTempDir(L"a2-dismiss-undo"));
        wlog(s1, L"20261110-213012", L"preset:bloat:services", 1, {{L"MapsBroker", L"2/1", L"4/1"}, {L"lfsvc", L"3/-", L"4/-"}}, false);
        auto l1 = LoadLogs(s1, nullptr);
        bool d1 = !l1.empty() && DismissInterrupted(s1, At(l1, 0), a.ctx.machine, a.ctx.build, LockName(), &e);
        l1 = LoadLogs(s1, nullptr);
        int u1 = FindUndoable(l1);
        Plan up1 = u1 >= 0 ? BuildUndo(At(l1, u1), a.fx.backend, a.items, a.ctx) : Plan{};
        check(d1 && FindInterrupted(l1) < 0 && u1 == idx(l1, L"20261110-213012") && RowOf(up1, L"MapsBroker") && RowOf(up1, L"lfsvc"),
              L"mini-analyze A2 (run 1): a dismissed interrupted apply keeps its Undo (undo-last opens it, both written rows in the plan)");
        // Run 2 (A2): apply A interrupted, its resume R interrupted, and R dismissed: A's banner does not come back, and
        // Undo from R puts back A's rows too (R7).
        Store s2(NewTempDir(L"a2-dismiss-resume"));
        wlog(s2, L"20261110-213012", L"preset:bloat:services", 1, {{L"MapsBroker", L"2/1", L"4/1"}, {L"lfsvc", L"3/-", L"4/-"}}, false);
        wlog(s2, L"20261110-221500", L"resume:20261110-213012", 2, {{L"RetailDemo", L"3/-", L"4/-", L"in progress"}}, false);
        auto l2 = LoadLogs(s2, nullptr);
        int r2 = idx(l2, L"20261110-221500");
        bool before2 = FindInterrupted(l2) == r2;
        bool d2 = r2 >= 0 && DismissInterrupted(s2, At(l2, r2), a.ctx.machine, a.ctx.build, LockName(), &e);
        l2 = LoadLogs(s2, nullptr);
        int u2 = FindUndoable(l2);
        Plan up2 = u2 >= 0 ? BuildUndo(At(l2, u2), a.fx.backend, a.items, a.ctx) : Plan{};
        check(before2 && d2 && FindInterrupted(l2) < 0 && u2 == idx(l2, L"20261110-221500") && RowOf(up2, L"MapsBroker") &&
                  RowOf(up2, L"lfsvc"),
              L"mini-analyze A2 (run 2): dismissing an interrupted resume:A also settles A (no banner comes back); its Undo covers A's rows");
        // Run 4: a log with a forged order far above the cap no longer stays "the last apply": the apply after it is.
        Store s4(NewTempDir(L"order-cap"));
        wlog(s4, L"20261101-100000", L"preset:bloat:services", 5000000, {{L"RetailDemo", L"3/-", L"4/-"}}, true);
        auto l4 = LoadLogs(s4, nullptr);
        int next = NextOrder(l4);
        wlog(s4, L"20261110-120000", L"preset:stripped:services", next, {{L"lfsvc", L"3/-", L"4/-"}}, true);
        l4 = LoadLogs(s4, nullptr);
        check(next == kOrderCap + 1 && l4.size() == 2 && At(l4, 1).stamp == L"20261110-120000" && FindUndoable(l4) == 1 &&
                  LastApplied(l4).count(L"lfsvc") && OrderKey(5000000) == OrderKey(next),
              L"mini-analyze run 4: logs with order > 1000000 sort by the cap, then the stamp: Undo last apply is the newest apply, not the forged log");
        // Run 3 (A3): first.txt from another Windows build, recovery\first and its zip missing: not written again.
        Store s3(NewTempDir(L"a3-first-build"));
        Snapshot old;
        old.meta[L"kind"] = L"first";
        old.meta[L"created"] = L"2026-01-01 10:00";
        old.meta[L"build"] = L"19044";
        old.meta[L"machine"] = a.ctx.machine;
        old.items = a.items;
        s3.WriteNew(L"first.txt", WriteSnapshot(old), &e);
        Snapshot f3;
        FirstRunResult fr3 = EnsureFirstSnapshot(s3, a.items, a.ctx, &f3);
        check(fr3.existed && !s3.Exists(L"recovery\\first") && !s3.Exists(L"recovery-first.zip") && !s3.Exists(L"recovery\\first-2") &&
                  fr3.note.find(L"19044") != std::wstring::npos,
              L"mini-analyze A3 (run 3): recovery\\first is not rewritten from a first.txt of another Windows build (a note says so)");
        // A3: a truncated zip is not a complete copy: the folder and zip are written again as first-2.
        Store s5(NewTempDir(L"a3-zip"));
        Snapshot f5;
        EnsureFirstSnapshot(s5, a.items, a.ctx, &f5);
        std::string z;
        std::wstring aside;
        bool cut = s5.Read(L"recovery-first.zip", &z, &e) && s5.RenameAside(L"recovery-first.zip", &aside, &e) &&
                   s5.WriteNew(L"recovery-first.zip", z.substr(0, z.size() / 2), &e);
        FirstRunResult fr5 = EnsureFirstSnapshot(s5, a.items, a.ctx, &f5);
        check(cut && fr5.existed && s5.Exists(L"recovery\\first-2\\restore.cmd") && s5.Exists(L"recovery-first-2.zip"),
              L"mini-analyze A3: a truncated recovery-first.zip doesn't count as complete; the recovery is written again as first-2");
        // A6: an apply whose first.txt is from another PC is refused before anything is written.
        Store s6(NewTempDir(L"a6-foreign"));
        Snapshot fo = old;
        fo.meta[L"build"] = a.ctx.build;
        fo.meta[L"machine"] = L"another-pc-0000";
        s6.WriteNew(L"first.txt", WriteSnapshot(fo), &e);
        World b;
        freshWorld(&b);
        Plan p6 = BuildSingle(*ItemOf(b.items, L"MapsBroker"), StartCfg{3, -1}, b.items, b.ctx);
        ApplyEnv env6 = EnvFor(&b.fx.backend, &s6);
        env6.requireFirst = true;
        ApplyResult ar6;
        bool ok6 = RunApply(p6, env6, &ar6);
        check(!ok6 && ar6.aborted && ar6.error.find(L"not from this PC") != std::wstring::npos && b.fx.backend.faults.setStartCalls == 0 &&
                  s6.List(L"snap-*.txt").empty() && s6.List(L"undo-*.log").empty(),
              L"mini-analyze A6: an apply is refused when first.txt is from another PC (machine GUID), before any file or service is touched");
        // A4: the load saw TrustedInstaller stopped, but another instance has since started it and holds a live claim:
        // under the lock TrustedInstaller is queried again and the claim is left alone.
        World c;
        freshWorld(&c);
        c.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        std::vector<SvcItem> seen = Folded(c.fx.backend);   // the load's picture: stopped
        c.fx.backend.SetState(L"TrustedInstaller", SERVICE_RUNNING);   // then the other instance's apply started it
        SvcItem tiNow;
        c.fx.backend.Query(L"TrustedInstaller", &tiNow, &e);
        Store s7(NewTempDir(L"a4-requery"));
        std::wstring hx = L"";
        {
            wchar_t b16[24];
            swprintf(b16, 24, L"%016llX", (unsigned long long)tiNow.pstart);
            hx = b16;
        }
        std::string t7 = "HXSVC\t1\tundo\n" + Record({L"B", L"20261111-140000", L"preset:stripped:services", L"0", a.ctx.machine, a.ctx.build, L"1"}) +
                         Record({L"T", L"starting"}) + Record({L"T", L"started", std::to_wstring(tiNow.pid), hx}) +
                         Record({L"T", L"stopfailed", L"access denied"}) + Record({L"E", L"20261111-140000", L"0", L"0", L"0", L"done"});
        s7.WriteNew(L"undo-20261111-140000.log", t7, &e);
        auto l7 = LoadLogs(s7, nullptr);
        int kept = ResolveStaleTiClaims(s7, l7, seen, a.ctx.machine, a.ctx.build, LockName(), &c.fx.backend);
        bool live = TiLeftRunning(LoadLogs(s7, nullptr), Folded(c.fx.backend));
        int blind = ResolveStaleTiClaims(s7, l7, seen, a.ctx.machine, a.ctx.build, LockName());   // the old way: stale picture
        check(tiNow.pid != 0 && kept == 0 && live && blind == 1,
              L"mini-analyze A4: a TrustedInstaller claim is closed only from its state re-read under the apply lock (a live claim of another "
              L"instance is kept)");
        // A5: Cancel doesn't turn cleanup into failures. TrustedInstaller Hadron started is stopped even after Cancel...
        World d;
        freshWorld(&d);
        d.fx.backend.SetState(L"TrustedInstaller", SERVICE_STOPPED);
        std::atomic<bool> cancel{false};
        TiService ts(&d.fx.backend);
        ts.cancel = &cancel;
        SvcItem tt;
        bool en = ts.Ensure(&tt, &e);
        cancel.store(true);
        std::wstring note;
        bool stopped = ts.StopIfStarted(&note);
        const SvcItem* tiAfter = ItemOf(Folded(d.fx.backend), L"TrustedInstaller");
        check(en && stopped && tiAfter && tiAfter->state == SERVICE_STOPPED && d.fx.backend.controlLog.back() == L"stop TrustedInstaller",
              L"mini-analyze A5: after Cancel, the release still stops the TrustedInstaller Hadron started (cleanup is not cut short)");
        // ...a stop that was sent but not seen to finish is "requested, not confirmed", not "could not stop"...
        TiService ts2(&d.fx.backend);
        bool en2 = ts2.Ensure(&tt, &e);
        d.fx.backend.faults.stopFail.insert(L"trustedinstaller");
        std::wstring note2;
        bool st2 = ts2.StopIfStarted(&note2);
        d.fx.backend.faults.stopFail.clear();
        check(en2 && !st2 && note2.find(L"requested, not confirmed") != std::wstring::npos,
              L"mini-analyze A5: a TrustedInstaller stop whose wait runs out says \"requested, not confirmed\": " + note2);
        // ...and R13's restarts after a failed stop still start what the run stopped when Cancel came meanwhile.
        FakeBackend fr;
        SvcItem x = Mk(L"HxX", 2, {}), x1 = Mk(L"HxD1", 2, {L"HxX"}), x2 = Mk(L"HxD2", 2, {L"HxX"});
        x.state = x1.state = x2.state = SERVICE_RUNNING;
        x.controls = x1.controls = x2.controls = SERVICE_ACCEPT_STOP;
        fr.SetItems({x, x1, x2});
        std::vector<SvcItem> three{x, x1, x2};
        Plan rs = BuildControl(x, 2, fr, three, a.ctx);
        std::atomic<bool> cancel2{false};
        fr.faults.stopFail.insert(L"hxx");
        fr.faults.cancelOnStop.insert(L"hxx");
        fr.faults.raise = &cancel2;
        Store cs(NewTempDir(L"a5-control"));
        ApplyEnv ce = EnvFor(&fr, &cs);
        ce.cancel = &cancel2;
        ApplyResult cr;
        RunControl(rs, ce, &cr);
        bool allUp = true;
        for (auto& i : fr.Items()) if (i.state != SERVICE_RUNNING) allUp = false;
        int again = 0;
        for (auto& l : cr.lines) if (l.find(L"started again after the failed stop") != std::wstring::npos) again++;
        check(cancel2.load() && allUp && again == 2,
              L"mini-analyze A5: Cancel during a Restart whose stop fails still starts the 2 dependents it stopped again (R13 cleanup)");
    }

    // ---- R11 and Q3: Default (as first seen); R12: first.txt missing, recovery retried, applies refused -------------
    {
        World a;
        freshWorld(&a);
        std::wstring e;
        Plan d0 = BuildDefault(false, TAB_SERVICES, a.items, a.ctx);
        check(d0.title == L"Default: as first seen (Services)" && !d0.notes.empty() &&
                  d0.notes[0] == L"Start types as Hadron first saw this PC on 2026-11-01. This is not Windows' default.",
              L"Q3: Default (as first seen) is titled so and says first what it is, and that it is not Windows' default");
        Snapshot up = *a.ctx.first;
        up.meta[L"build"] = L"10.0.19044.1288";   // taken before an in-place upgrade
        Ctx cu = a.ctx;
        cu.first = &up;
        Plan d1 = BuildDefault(false, TAB_SERVICES, a.items, cu);
        Snapshot nm = *a.ctx.first;
        nm.meta.erase(L"machine");
        Ctx cn = a.ctx;
        cn.first = &nm;
        Plan d2 = BuildDefault(false, TAB_SERVICES, a.items, cn);
        Ctx cm = a.ctx;
        cm.machine.clear();
        Plan d3 = BuildDefault(false, TAB_SERVICES, a.items, cm);
        check(d1.rows.empty() && !d1.notes.empty() && d1.notes[0].find(L"build 10.0.19044.1288") != std::wstring::npos &&
                  d2.rows.empty() && d2.notes[0].find(L"no machine GUID") != std::wstring::npos && d3.rows.empty() &&
                  BuildNumber(L"10.0.19045.5737") == L"19045" && BuildNumber(L"19045") == L"19045",
              L"R11: Default (as first seen) refuses a snapshot from another Windows build, or with no machine GUID on either side");
        // R12: undo logs but no first.txt -> a baseline, labelled so.
        Store s(NewTempDir(L"first-missing"));
        std::string lg = "HXSVC\t1\tundo\n" + Record({L"B", L"20261111-090000", L"single", L"0", a.ctx.machine, a.ctx.build, L"1"}) +
                         Record({L"E", L"20261111-090000", L"0", L"0", L"0", L"done"});
        s.WriteNew(L"undo-20261111-090000.log", lg, &e);
        Snapshot bf;
        FirstRunResult fr = EnsureFirstSnapshot(s, a.items, a.ctx, &bf);
        Ctx cb = a.ctx;
        cb.first = &bf;
        Plan db = BuildDefault(false, TAB_SERVICES, a.items, cb);
        check(fr.wrote && bf.meta[L"kind"] == L"baseline (not first run)" && fr.note.find(L"baseline") != std::wstring::npos &&
                  !db.notes.empty() && db.notes[0].find(L"not how Hadron first saw this PC") != std::wstring::npos,
              L"R12: first.txt missing while undo logs exist -> recorded as a baseline, and Default says so");
        // R12: recovery\first without its zip (an earlier failed try) -> written again as first-2; complete -> left alone.
        Store s2(NewTempDir(L"first-retry"));
        Snapshot f2;
        EnsureFirstSnapshot(s2, a.items, a.ctx, &f2);
        bool firstOk = s2.Exists(L"recovery\\first\\restore.cmd") && s2.Exists(L"recovery-first.zip");
        std::wstring aside;
        bool moved = s2.RenameAside(L"recovery-first.zip", &aside, &e);
        FirstRunResult r2 = EnsureFirstSnapshot(s2, a.items, a.ctx, &f2);
        FirstRunResult r3 = EnsureFirstSnapshot(s2, a.items, a.ctx, &f2);
        std::string rd2;
        s2.Read(L"recovery\\first-2\\README.txt", &rd2, &e);
        check(firstOk && moved && r2.existed && r2.error.empty() && s2.Exists(L"recovery\\first-2\\restore.cmd") &&
                  s2.Exists(L"recovery-first-2.zip") && r2.note.find(L"first-2") != std::wstring::npos && r3.note.empty() &&
                  !s2.Exists(L"recovery\\first-3") && rd2.find("Default: as first seen") != std::string::npos,
              L"R12: a recovery\\first left without its zip is written again as first-2 (the old one kept), once");
        // R12: no first.txt -> every apply but an Undo is refused, before anything is written.
        Store s3(NewTempDir(L"first-required"));
        Plan p = BuildSingle(*ItemOf(a.items, L"MapsBroker"), StartCfg{3, -1}, a.items, a.ctx);
        ApplyEnv env = EnvFor(&a.fx.backend, &s3);
        env.requireFirst = true;
        ApplyResult ar;
        bool ok = RunApply(p, env, &ar);
        Plan u = p;
        u.kind = PlanKind::Undo;
        ApplyResult ur;
        bool uok = RunApply(u, env, &ur);
        check(!ok && ar.aborted && ar.error.find(L"first.txt") != std::wstring::npos && s3.List(L"snap-*.txt").size() == 1 && uok,
              L"R12: with no first.txt an apply is refused before any write; an Undo still runs");
    }

    // ---- R20: Windows stock with no data; R17: a +group dependent whose write failed -------------------------------
    {
        World a;
        freshWorld(&a);
        a.ctx.sxs = nullptr;   // the KB's own wording (the manifest wording is checked with the sxs fixture)
        Plan s0 = PlanFor(-2, TAB_DRIVERS, a);   // no driver has a stock value in the KB
        Ctx co = a.ctx;
        co.build = L"22631";
        Plan s1 = BuildDefault(true, TAB_DRIVERS, a.items, co);
        const std::wstring want0 = L"Hadron's KB has no Windows stock value for any item on this tab: nothing will be changed.";
        check(s0.rows.empty() && s0.emptyText == want0 && s0.notes.size() == 1 && s0.notes[0] == want0 &&
                  s1.emptyText == L"Hadron has no Windows-stock data for build 22631 yet: nothing will be changed.",
              L"R20: Windows stock with no data says so (never a bare \"Nothing to change.\"): no value on this tab (19045), no data for another build");
        std::vector<SvcItem> gi = {Mk(L"gmem", 3, {}), Mk(L"gdep", 3, {L"+grp"})};
        gi[0].group = L"GRP";
        FakeBackend fb;
        fb.SetItems(gi);
        fb.faults.deny = {L"gdep"};
        Plan pg;
        pg.rows = {Lower4(gi[1]), Lower4(gi[0])};
        Store s(NewTempDir(L"group-dep"));
        ApplyResult ar;
        RunApply(pg, EnvFor(&fb, &s), &ar);
        int memStart = -1;
        for (auto& i : fb.Items()) if (i.name == L"gmem") memStart = i.cfg.start;
        bool skipped = false;
        for (auto& l : ar.lines) if (l.rfind(L"skip gmem", 0) == 0 && l.find(L"gdep") != std::wstring::npos) skipped = true;
        std::vector<size_t> go = ApplyOrder(pg);
        check(go.size() == 2 && pg.rows[go[0]].name == L"gdep",
              L"R17: apply order puts a dependent on +group before the group's members when lowering");
        check(skipped && memStart == 3, L"R17: a dependent on +group whose write failed keeps every member of the group from being lowered");
    }

    // ---- R16 and the recovery README (cat 6) -----------------------------------------------------------------------
    {
        std::vector<RestoreRow> rows = {{L"DiagTrack", {3, -1}, false}, {L"Evil\r\nreg add HKLM\\X", {4, -1}, false},
                                        {L"Br]acket", {3, -1}, false}, {L"D\u00e9mo", {3, -1}, false}};
        std::string cmd = RestoreCmd(L"20261111-101500", L"t\r\necho pwned", rows);
        std::string reg = UnicodeReg(rows, 1);
        std::wstring regW;
        for (size_t i = 2; i + 1 < reg.size(); i += 2) regW += (wchar_t)((unsigned char)reg[i] | ((unsigned char)reg[i + 1] << 8));
        std::string rd = ReadmeTxt(L"20261111-101500", L"C:\\ProgramData\\Hadron\\services\\recovery\\20261111-101500", L"t", rows);
        bool oneLineTitle = cmd.find("t echo pwned") != std::string::npos && cmd.find("\r\necho pwned") == std::string::npos;
        check(oneLineTitle && regW.find(L"Evil") == std::wstring::npos && regW.find(L"Br]acket") == std::wstring::npos &&
                  regW.find(L"D\u00e9mo") != std::wstring::npos && rd.find("Evil  reg add") != std::string::npos &&
                  rd.find("can't go in a .reg file either") != std::string::npos && rd.find("\nreg add") == std::string::npos &&
                  !RegSafeName(L"a\nb") && !RegSafeName(L"a]b") && RegSafeName(L"Intel(R) X"),
              L"R16: CR/LF and other control characters never start a line in restore.cmd, the .reg files or README.txt");
        check(rd.find("While Windows runs, this folder is C:\\ProgramData\\Hadron\\services\\recovery\\20261111-101500") != std::string::npos &&
                  rd.find("     D:\\ProgramData\\Hadron\\services\\recovery\\20261111-101500\\restore.cmd D:") != std::string::npos &&
                  rd.find("     C:\\ProgramData\\Hadron\\services\\recovery-20261111-101500.zip") != std::string::npos &&
                  rd.find("dir D:\\ProgramData\\Hadron\\services\\recovery") != std::string::npos &&
                  rd.find("Repair your computer") != std::string::npos && rd.find("Run the newest folder first") != std::string::npos &&
                  rd.find("WinRE has no Explorer to open a .zip (reasoning)") != std::string::npos &&
                  rd.find("use Undo.") != std::string::npos && rd.find("WinRE can't open") == std::string::npos,
              L"README: the folder with its drive, the zip's real path, how to choose a folder, the install-media fallback");
        std::string ru = ReadmeTxt(L"20261111-101500", L"\\\\nas\\share\\hadron\\recovery\\20261111-101500", L"t", rows);
        check(ru.find("on a network share") != std::string::npos && ru.find("D:\\\\nas") == std::string::npos &&
                  ru.find("Run the copy on the USB stick") != std::string::npos,
              L"README: a data folder on a UNC share says to copy it to a USB stick (no D:\\\\server path)");
        std::string nw = RestoreCmd(L"x", L"t", rows);
        check(nw.find("BitLocker-locked drive shows no Windows folder") != std::string::npos,
              L"restore.cmd: the no-Windows message names a BitLocker-locked drive");
    }

    // ---- A5: the data folder is never followed through a link, and is owned by Administrators or SYSTEM -----------
    {
        std::wstring base = NewTempDir(L"root");
        std::wstring real = base + L"\\real", how;
        CreateDirectoryW(real.c_str(), nullptr);
        std::wstring link = MakeLink(base, real, &how);
        bool tested = true;
        if (link.empty()) {
#ifdef HX_SERVICES_TEST_HOOKS
            info(L"A5: made " + how + L" here, so the reparse point is injected (Windows CI makes a real one)");
            link = real;
            how = L"reparse point (injected)";
            Store::InjectedReparsePoint() = real;
#else
            info(L"A5: made " + how + L" here, and this build has no test hooks: the reparse check is not exercised");
            tested = false;
#endif
        }
        {
            if (tested) {
                std::wstring e1, e2;
                bool root = Store(link).EnsureRoot(true, &e1);
                bool child = Store(link + L"\\Hadron\\services").EnsureRoot(true, &e2);
                DWORD ca = GetFileAttributesW((real + L"\\Hadron").c_str());
                check(!root && !child && e1.find(L"link") != std::wstring::npos && e2.find(L"link") != std::wstring::npos &&
                          ca == INVALID_FILE_ATTRIBUTES,
                      L"A5: EnsureRoot refuses a data folder that is, or sits below, a " + how + L", and creates nothing through it");
            }
        }
#ifdef HX_SERVICES_TEST_HOOKS
        Store::InjectedReparsePoint().clear();
#endif
        std::wstring e3;
        Store deep(L"\\\\?\\" + base + L"\\a\\b\\c");
        bool okDeep = deep.EnsureRoot(true, &e3);
        std::wstring e4;
        bool again = deep.EnsureRoot(true, &e4);
        check(okDeep && again && DirExists(base + L"\\a\\b\\c"),
              L"A5: a \\\\?\\-prefixed root with missing parents is created, then re-checked and accepted (owner, DACL)" +
                  (okDeep ? L"" : L": " + e3) + (again ? L"" : L": " + e4));
        // R1: an existing folder is never re-secured. One that lets Everyone write is refused and keeps its DACL; one that
        // is already admin-only is accepted and keeps its DACL too.
        auto daclOf = [](const std::wstring& path) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            std::wstring s;
            if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &sd) ==
                ERROR_SUCCESS) {
                LPWSTR t = nullptr;
                if (ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &t, nullptr)) { s = t; LocalFree(t); }
                LocalFree(sd);
            }
            return s;
        };
        auto mkWith = [](const std::wstring& path, const wchar_t* sddl) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
            SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
            BOOL ok = CreateDirectoryW(path.c_str(), &sa);
            LocalFree(sd);
            return ok != FALSE;
        };
        std::wstring open = base + L"\\open", tight = base + L"\\tight";
        bool mo = mkWith(open, L"O:BAD:(A;OICI;FA;;;WD)(A;OICI;FA;;;BA)");
        bool mt = mkWith(tight, L"O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
        std::wstring d0 = daclOf(open), t0 = daclOf(tight), e5, e6;
        bool okOpen = Store(open).EnsureRoot(true, &e5);
        bool okTight = Store(tight).EnsureRoot(true, &e6);
        if (mo && d0.find(L";FA;;;WD)") == std::wstring::npos) {
            info(L"R1: this system does not keep a folder's DACL (" + d0 + L"; Wine), so the existing-folder DACL check is not "
                 L"exercised here (Windows CI runs it)");
        } else {
            check(mo && mt && !okOpen && e5.find(L"already exists") != std::wstring::npos && daclOf(open) == d0,
                  L"R1: an existing data folder that lets Everyone write is refused and its permissions are left as they were (" +
                      d0 + L"): " + e5);
        }
        check(mo && mt && okTight && daclOf(tight) == t0, L"R1: an existing admin-only data folder is accepted, its permissions unchanged" +
                                                              (okTight ? L"" : L": " + e6));
        // The DACL test itself, on descriptors built from SDDL (runs under Wine too).
        auto problem = [](const wchar_t* sddl) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return std::wstring(L"(bad sddl)");
            std::wstring w = Store::WriteAccessProblem(sd);
            LocalFree(sd);
            return w;
        };
        check(problem(L"O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)").empty() &&
                  problem(L"O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;BU)").empty() &&
                  !problem(L"O:BAD:(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)").empty() &&
                  !problem(L"O:BAD:(A;OICI;FA;;;BA)(A;OICIIO;GA;;;CO)").empty() &&
                  !problem(L"O:BAD:(A;OICI;FA;;;BA)(A;CI;0x2;;;BU)").empty(),
              L"R1: the existing-folder test accepts admin-only (plus read) DACLs and refuses Users write and an inherit-only "
              L"CREATOR OWNER entry (as under C:\\ProgramData)");
        SECURITY_DESCRIPTOR nullSd;
        bool nullOk = InitializeSecurityDescriptor(&nullSd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&nullSd, TRUE, nullptr, FALSE);
        check(nullOk && Store::WriteAccessProblem(&nullSd).find(L"no DACL") != std::wstring::npos,
              L"R1: the existing-folder test refuses a null DACL (everyone has full access)");
        // mini-analyze A1: every folder above the data folder must let nobody but admins rename, delete or re-permission
        // it. The rule itself, on descriptors built from SDDL (runs under Wine too):
        auto moveProblem = [](const wchar_t* sddl) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return std::wstring(L"(bad sddl)");
            std::wstring w = Store::RenameAccessProblem(sd);
            LocalFree(sd);
            return w;
        };
        // C:\ (Authenticated Users: create folders here, Modify inherit-only) and C:\ProgramData (Users: create files and
        // folders, CREATOR OWNER inherit-only) pass; D:\Tools inheriting "Authenticated Users: Modify", a delete-child
        // right, WRITE_DAC and a null DACL do not.
        check(moveProblem(L"D:(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;BU)(A;OICIIO;0x1301bf;;;AU)(A;;0x4;;;AU)").empty() &&
                  moveProblem(L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIIO;GA;;;CO)(A;OICI;0x1200a9;;;BU)(A;CI;0x116;;;BU)").empty() &&
                  moveProblem(L"D:(A;OICI;FA;;;BA)(A;OICIID;0x1301bf;;;AU)").find(L"rename") != std::wstring::npos &&
                  !moveProblem(L"D:(A;OICI;FA;;;BA)(A;;0x40;;;BU)").empty() && !moveProblem(L"D:(A;OICI;FA;;;BA)(A;;0x40000;;;BU)").empty() &&
                  nullOk && Store::RenameAccessProblem(&nullSd).find(L"no DACL") != std::wstring::npos,
              L"mini-analyze A1: the ancestor rule passes C:\\ and C:\\ProgramData as Windows ships them and refuses a folder others "
              L"may rename (AU Modify, FILE_DELETE_CHILD, WRITE_DAC, no DACL)");
        // ...and EnsureRoot applies it to every folder above the data folder, before creating anything below it.
        std::wstring anc = base + L"\\renamable", e7;
        bool ma = mkWith(anc, L"O:BAD:(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;;0x10000;;;AU)");
        if (ma && daclOf(anc).find(L";;;AU)") == std::wstring::npos) {
            info(L"mini-analyze A1: this system does not keep a folder's DACL (Wine), so EnsureRoot's ancestor check is exercised "
                 L"on the SDDL rule above only (Windows CI runs this part)");
        } else {
            bool okAnc = Store(anc + L"\\Hadron\\services").EnsureRoot(true, &e7);
            check(ma && !okAnc && e7.find(anc) != std::wstring::npos && e7.find(L"rename") != std::wstring::npos &&
                      GetFileAttributesW((anc + L"\\Hadron").c_str()) == INVALID_FILE_ATTRIBUTES,
                  L"mini-analyze A1: a data folder below a folder Authenticated Users may delete is refused, and nothing is created: " + e7);
        }
        check(Store::Plain(L"\\\\?\\UNC\\srv\\share\\x") == L"\\\\srv\\share\\x" && Store::Plain(L"\\\\?\\C:\\x") == L"C:\\x",
              L"Store::Plain strips \\\\?\\ and \\\\?\\UNC\\ (--data-dir is kept without the prefix)");
    }

    // ---- follow-ups 6-9: signer, event 7040, interface evidence, registry fallback (selftest_evidence.cpp) ----
    RunEvidenceSelftest(fixture, kb, [](const std::wstring& tag) { return NewTempDir(tag); }, check, info);

    // ---- custom presets: format, strict import, guards, store; measured before/after (fix/svcdrv-presets) ----------
    {
        auto has = [](const std::wstring& s, const wchar_t* sub) { return s.find(sub) != std::wstring::npos; };
        err.clear();
        // The fixture's stored preset: parse -> write reproduces it byte for byte.
        std::string raw;
        CustomPreset gp;
        bool ok = ReadAll(fixture + L"\\presets\\20261111-090000.txt", &raw) && ParsePreset(raw, &gp, &err);
        check(ok && WritePreset(gp) == raw && gp.name == L"Gaming rig" && gp.items.size() == 13 && gp.tab == TAB_SERVICES,
              L"custom preset: the fixture's presets file parses (13 items) and writes back byte for byte" + (ok ? L"" : L": " + err));
        {   // As the page loads it from the data folder: with its saved time from the file name.
            Store fs(fixture);
            std::vector<std::wstring> probs;
            auto lp = LoadCustomPresets(fs, &probs);
            for (auto& c : lp) if (c.name == L"Gaming rig") gp = c;
            check(lp.size() == 2 && probs.empty() && gp.saved == L"2026-11-11 09:00 UTC" && gp.file == L"20261111-090000.txt",
                  L"custom preset: the fixture's presets folder lists 2 presets, with their saved time from the file name");
        }

        // export -> import -> export through real files: byte-identical, and CREATE_NEW never replaces a file.
        std::wstring dir = NewTempDir(L"presets");
        CustomPreset mine;
        mine.name = L"Round trip éè 中";
        mine.hadron = HadronVersion();
        mine.build = L"19045";
        mine.items = {{L"DiagTrack", StartCfg{4, -1}}, {L"WSearch", StartCfg{2, 1}}, {L"Spooler", StartCfg{2, 0}}, {L"Fax", StartCfg{3, -1}}};
        bool exists = false;
        std::string a1, a2;
        CustomPreset back;
        bool e1 = ExportPresetFile(dir + L"\\one.txt", mine, &exists, &err);
        bool i1 = e1 && ImportPresetFile(dir + L"\\one.txt", &back, &err);
        bool e2 = i1 && ExportPresetFile(dir + L"\\two.txt", back, &exists, &err);
        check(e1 && i1 && e2 && ReadAll(dir + L"\\one.txt", &a1) && ReadAll(dir + L"\\two.txt", &a2) && a1 == a2 && !a1.empty() &&
                  back.name == mine.name && back.items.size() == 4 && back.items[1].target == StartCfg{2, 1} && back.file.empty(),
              L"custom preset: export -> import -> export is byte-identical (UTF-8 name, auto-delayed kept)" + (err.empty() ? L"" : L": " + err));
        err.clear();
        CustomPreset other = mine;
        other.name = L"Something else";
        bool e3 = ExportPresetFile(dir + L"\\one.txt", other, &exists, &err);
        std::string a3;
        check(!e3 && exists && ReadAll(dir + L"\\one.txt", &a3) && a3 == a1 && has(err, L"never replaces"),
              L"custom preset: export to an existing file is refused (CREATE_NEW) and the file is unchanged");
        err.clear();

        // Strict import: every refusal names its line.
        const std::string head = "HXPRESET\t1\nhadron\t0.1.0\nbuild\t19045\ntab\tservices\nname\tX\n";
        struct Bad { std::string text; const wchar_t* want; const wchar_t* what; };
        const Bad bads[] = {
            {"HXPRESET\t2\nhadron\t0.1.0\nbuild\t19045\ntab\tservices\nname\tX\nDiagTrack\tdisabled\n", L"line 1: format version '2' is not supported", L"an unknown format version"},
            {"HXPRESET\t1.0\nhadron\t0.1.0\nbuild\t19045\ntab\tservices\nname\tX\nDiagTrack\tdisabled\n", L"line 1: format version", L"a malformed version"},
            {"HXSVC\t1\tpreset\nDiagTrack\tdisabled\n", L"line 1: not a Hadron services preset", L"another file's header"},
            {head + std::string(600, 'A') + "\tdisabled\n", L"line 6: the line is 609 bytes; the limit is 512", L"an overlong line"},
            {head + "DiagTrack\tdisabled\nSysMain\toff\n", L"line 7: start type 'off' is not one of auto, auto-delayed, manual, disabled", L"a bad start type"},
            {head + "Beep\tsystem\n", L"Hadron never sets Boot or System start", L"System start"},
            {head + "DiagTrack\tDisabled\n", L"line 6: start type 'Disabled'", L"a start type in the wrong case"},
            {"HXPRESET\t1\nhadron\t0.1.0\nbuild\t19045\nname\tX\nDiagTrack\tdisabled\n", L"line 4: expected the header line 'tab'", L"a missing header line"},
            {"HXPRESET\t1\nhadron\t0.1.0\nbuild\t19045\ntab\tservices\n", L"line 5: missing; the header needs 'name'", L"a truncated header"},
            {"HXPRESET\t1\nhadron\t0.1.0\nbuild\tlatest\ntab\tservices\nname\tX\nDiagTrack\tdisabled\n", L"line 3: the Windows build", L"a bad build"},
            {"HXPRESET\t1\nhadron\t0.1.0\nbuild\t19045\ntab\tboth\nname\tX\nDiagTrack\tdisabled\n", L"line 4: tab 'both'", L"a bad tab"},
            {"HXPRESET\t1\nhadron\t0.1.0\nbuild\t19045\ntab\tservices\nname\t\nDiagTrack\tdisabled\n", L"line 5: the preset needs a name", L"an empty name"},
            {head + "DiagTrack\tdisabled\ndiagtrack\tmanual\n", L"line 7: diagtrack is listed twice", L"a duplicate item"},
            {head + "DiagTrack\tdisabled\n\nSysMain\tmanual\n", L"line 7: an empty line among the items", L"an empty line among the items"},
            {head + "DiagTrack\tdisabled\textra\n", L"line 6: expected a name, one tab and a start type; found 3 field(s)", L"an extra field"},
            {head + "Diag\\Track\tdisabled\n", L"line 6: 'Diag\\Track' is not a service or driver name", L"a backslash in a name"},
            {head + "Diag\xff\xfeTrack\tdisabled\n", L"line 6: not valid UTF-8 text", L"invalid UTF-8"},
            {"HXPRESET\t1\nhadron\t0.1.0\nbuild\t19045\ntab\tdrivers\nname\tX\nbeep\tauto-delayed\n", L"line 6: auto-delayed is for services", L"auto-delayed on the drivers tab"},
            {head, L"the preset lists no items", L"no items"},
            {"", L"the file is empty", L"an empty file"},
            {head + std::string(kPresetMaxFile, 'x'), L"larger than", L"a file over the size cap"},
        };
        for (auto& b : bads) {
            CustomPreset x;
            std::wstring e;
            bool okp = ParsePreset(b.text, &x, &e);
            check(!okp && has(e, b.want), std::wstring(L"custom preset import refuses ") + b.what + L" (\"" + e + L"\")");
        }
        {
            std::string many = head;
            for (size_t i = 0; i <= kPresetMaxItems; i++) many += "Svc" + std::to_string(i) + "\tmanual\n";
            CustomPreset x;
            std::wstring e;
            bool okm = ParsePreset(many, &x, &e);
            check(!okm && has(e, L"more than 4096 items"), L"custom preset import refuses more than 4096 items (\"" + e + L"\")");
        }
        {   // Notepad's form is read: a UTF-8 BOM, CRLF and trailing empty lines.
            CustomPreset x;
            std::wstring e;
            std::string np = "\xEF\xBB\xBFHXPRESET\t1\r\nhadron\t0.1.0\r\nbuild\t19045\r\ntab\tservices\r\nname\tX\r\nDiagTrack\tdisabled\r\n\r\n\r\n";
            check(ParsePreset(np, &x, &e) && x.items.size() == 1, L"custom preset import takes a UTF-8 BOM, CRLF line ends and trailing empty lines" +
                                                                          (e.empty() ? L"" : L": " + e));
        }
        {   // A refused import file through the same path the Import button takes, with the line named.
            CustomPreset x;
            std::wstring e;
            bool oki = ImportPresetFile(fixture + L"\\import\\bad-start.txt", &x, &e);
            check(!oki && has(e, L"line 8: start type 'off'"),
                  L"custom preset: importing import\\bad-start.txt is refused at line 8 (\"" + e + L"\")");
        }
        {   // R1 (review): the attribute check that runs on the open handle, for every attribute that refuses.
            bool all = ImportAttrProblem(FILE_ATTRIBUTE_ARCHIVE, L"x").empty() && ImportAttrProblem(FILE_ATTRIBUTE_NORMAL, L"x").empty() &&
                       has(ImportAttrProblem(FILE_ATTRIBUTE_DIRECTORY, L"x"), L"is a folder") &&
                       has(ImportAttrProblem(FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_ARCHIVE, L"x"), L"cloud placeholder") &&
                       has(ImportAttrProblem(FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, L"x"), L"cloud placeholder") &&
                       has(ImportAttrProblem(FILE_ATTRIBUTE_OFFLINE, L"x"), L"cloud placeholder") &&
                       has(ImportAttrProblem(FILE_ATTRIBUTE_REPARSE_POINT, L"x"), L"is a link") &&
                       has(ImportAttrProblem(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, L"x"), L"cloud placeholder");
            check(all, L"custom preset import refuses a folder, a link (reparse point) and a cloud placeholder (recall on open, on data access, offline), and takes a plain file");
            std::wstring e;
            CustomPreset x;
            bool okd = ImportPresetFile(dir, &x, &e);
            check(!okd && has(e, L"is a folder"), L"custom preset import refuses a folder through its handle (\"" + e + L"\")");
            std::wstring big = dir + L"\\big.txt";
            std::string bigText = "HXPRESET\t1\n" + std::string(kPresetMaxFile, 'x');
            bool wrote = WriteAll(big, bigText);
            bool okb = wrote && ImportPresetFile(big, &x, &e);
            check(wrote && !okb && has(e, L"larger than"), L"custom preset import stops reading one byte past the cap and refuses the file (\"" + e + L"\")");
        }
        {   // A cloud placeholder is refused unread (the attribute is set on a temp copy where the file system takes it).
            std::wstring cp = dir + L"\\offline.txt";
            std::wstring e;
            bool made = ExportPresetFile(cp, mine, &exists, &e);
            bool marked = made && SetFileAttributesW(sys::ToExtended(cp).c_str(), FILE_ATTRIBUTE_OFFLINE) &&
                          (GetFileAttributesW(sys::ToExtended(cp).c_str()) & FILE_ATTRIBUTE_OFFLINE);
            CustomPreset x;
            if (marked) check(!ImportPresetFile(cp, &x, &e) && has(e, L"cloud placeholder"), L"custom preset: an offline (cloud) file is refused unread");
            else info(L"custom preset: this file system doesn't keep FILE_ATTRIBUTE_OFFLINE; the cloud-placeholder refusal is not exercised here");
        }

        // Guards: protected, never-in-a-preset and third-party items are dropped with a note; plans go through RunApply.
        World cw;
        freshWorld(&cw);
        Plan p = BuildCustom(gp, cw.items, cw.ctx);
        MarkTi(&p, cw.fx.backend, false, cw.items);
        std::wstring notes = Join(p.notes, L" | ");
        check(!RowOf(p, L"WinDefend") && !RowOf(p, L"RpcSs") && !RowOf(p, L"Fax") && has(notes, L"Dropped 3 item(s) that every preset leaves alone") &&
                  has(notes, L"WinDefend") && has(notes, L"RpcSs") && has(notes, L"Fax"),
              L"custom preset guard: WinDefend (never in a preset), RpcSs (never touched) and Fax (no KB entry) are dropped with a note naming them");
        check(has(notes, L"not on this PC: HxNotOnThisPc") && has(notes, L"not on the Services tab, so this preset leaves them: nvlddmkm") &&
                  p.alreadySet == 1 && !RowOf(p, L"DiagTrack"),
              L"custom preset: an item not on this PC and a driver in a services preset are named in notes; DiagTrack is already set");
        {   // R2 (review): the hardware guard of built-in presets. bthserv: Bluetooth is present, so it is kept (blocked);
            // FrameServer: a camera class never seen is a hot-plug class, offered unticked with its evidence.
            const PlanRow* bt = RowOf(p, L"bthserv");
            const PlanRow* fs = RowOf(p, L"FrameServer");
            const PlanRow* sbt = RowOf(BuildPreset(LV_STRIPPED, TAB_SERVICES, cw.items, cw.ctx), L"bthserv");
            check(bt && bt->blocked.find(L"Kept: hardware in use or seen before.") == 0 && !bt->include && !bt->canToggle && !bt->evidence.empty() &&
                      sbt && sbt->blocked == bt->blocked && fs && !fs->include && fs->blocked.empty() && !fs->evidence.empty(),
                  L"custom preset hardware guard: bthserv is kept as by Stripped (\"" + (bt ? bt->blocked : std::wstring(L"no row")) +
                      L"\"), FrameServer is offered unticked with its evidence");
            World nh;
            freshWorld(&nh);
            nh.ctx.hw = nullptr;
            const PlanRow* bn = RowOf(BuildCustom(gp, nh.items, nh.ctx), L"bthserv");
            check(bn && !bn->blocked.empty() && bn->evidence == L"no hardware information: kept",
                  L"custom preset hardware guard: without hardware information a hardware-bound item is kept");
        }
        const PlanRow* ws = RowOf(p, L"WSearch");
        const PlanRow* sm = RowOf(p, L"SysMain");
        check(ws && ws->after == StartCfg{2, 1} && ws->op.delayed == 1 && sm && sm->after.start == 4 && sm->include && sm->basis == L"user" &&
                  p.source == L"custom:services:Gaming rig" && SourceLabel(p.source) == L"the custom preset \"Gaming rig\" (Services)",
              L"custom preset plan: WSearch is raised to Auto (delayed), SysMain lowered to Disabled; source and its label name the preset");
        {
            std::string drvRaw;
            CustomPreset dp;
            bool okd = ReadAll(fixture + L"\\presets\\20261111-093000.txt", &drvRaw) && ParsePreset(drvRaw, &dp, &err);
            Plan pd = okd ? BuildCustom(dp, cw.items, cw.ctx) : Plan{};
            std::wstring dn = Join(pd.notes, L" | ");
            check(okd && pd.rows.empty() && !pd.emptyText.empty() && has(dn, L"Beep") && has(dn, L"nvlddmkm") && has(dn, L"build 19041"),
                  L"custom preset guard (drivers): Beep (System start, no sysok) and nvlddmkm (no KB entry) are dropped; the build note is shown");
        }
        {   // The golden text of the fixture's custom plan.
            std::string got = DumpPlan(p);
            std::wstring gpth = golden + L"\\plan-services-custom.txt";
            if (update) check(WriteAll(gpth, got), L"wrote golden " + gpth);
            else {
                std::string want;
                bool have = ReadAll(gpth, &want);
                check(have && want == got, L"golden plan-services-custom.txt" + std::wstring(have ? (want == got ? L"" : L" differs") : L" missing"));
            }
        }
        {   // Ticks -> custom preset -> plan: the ticked rows come back with the same targets.
            Plan st = PlanFor(LV_STRIPPED, TAB_SERVICES, cw);
            TickUndetectable(&st);
            CustomPreset fromTicks = PresetFromPlan(st, L"My stripped", L"19045");
            std::string txt = WritePreset(fromTicks);
            CustomPreset re;
            bool okr = ParsePreset(txt, &re, &err);
            Plan again = okr ? BuildCustom(re, cw.items, cw.ctx) : Plan{};
            MarkTi(&again, cw.fx.backend, false, cw.items);
            bool same = okr && (int)fromTicks.items.size() == st.Included();
            for (auto& r : st.rows) {
                if (!r.include || !r.blocked.empty()) continue;
                const PlanRow* q = RowOf(again, r.name.c_str());
                if (!q || !q->include || q->after.start != r.after.start || q->after.Delayed() != r.after.Delayed()) same = false;
            }
            check(same, L"custom preset: the Stripped plan's " + std::to_wstring(st.Included()) +
                            L" ticked rows saved as a preset come back as the same ticked rows and targets");
        }
        {   // The plan a custom preset fills goes through the unchanged RunApply, snapshot and undo path.
            World aw;
            freshWorld(&aw);
            auto before = CfgMap(aw.fx.backend);
            Store s(NewTempDir(L"custom-apply"));
            s.fixedStamp = L"20261111-111500";
            Plan cp = BuildCustom(gp, aw.items, aw.ctx);
            MarkTi(&cp, aw.fx.backend, false, aw.items);
            ApplyEnv env = EnvFor(&aw.fx.backend, &s);
            ApplyResult r;
            bool oka = RunApply(cp, env, &r);
            auto logs = LoadLogs(s, nullptr);
            check(oka && r.ok == cp.Included() && r.failed == 0 && s.Exists(L"snap-20261111-111500.txt") && s.Exists(L"recovery-20261111-111500.zip") &&
                      logs.size() == 1 && At(logs, 0).source == L"custom:services:Gaming rig",
                  L"custom preset apply (fake): " + std::to_wstring(r.ok) + L" rows, snapshot and recovery first, the log names the source");
            Plan u = BuildUndo(At(logs, 0), aw.fx.backend, aw.items, aw.ctx);
            ApplyResult ru;
            bool oku = RunApply(u, env, &ru);
            // Start values exactly; an absent DelayedAutostart comes back as 0, as every Undo writes it (design note).
            bool same = true;
            for (auto& kv : CfgMap(aw.fx.backend)) {
                const StartCfg& b0 = before[kv.first];
                if (kv.second.start != b0.start || kv.second.Delayed() != b0.Delayed() || (b0.delayed >= 0 && kv.second.delayed != b0.delayed)) same = false;
            }
            check(oku && ru.failed == 0 && same, L"custom preset apply: Undo restores every start value (and every recorded delayed flag) exactly");
        }
        {   // The data folder store: CREATE_NEW files, a name taken refused, Remove hides without deleting.
            Store s(NewTempDir(L"custom-store"));
            s.fixedStamp = L"20261111-120000";
            CustomPreset a = gp, b = gp;
            std::wstring e1s, e2s, e3s;
            bool sa = SaveCustomPreset(s, &a, {}, &e1s);
            std::vector<std::wstring> probs;
            auto list = LoadCustomPresets(s, &probs);
            b.name = L"GAMING RIG";
            bool sb = SaveCustomPreset(s, &b, list, &e2s);
            check(sa && a.file == L"20261111-120000.txt" && list.size() == 1 && At(list, 0).name == L"Gaming rig" &&
                      At(list, 0).saved == L"2026-11-11 12:00 UTC" && probs.empty() && !sb && has(e2s, L"exists"),
                  L"custom preset store: saved as presets\\20261111-120000.txt, listed back, and a second \"GAMING RIG\" is refused");
            bool rm = RemoveCustomPreset(s, At(list, 0), &e3s);
            auto list2 = LoadCustomPresets(s, &probs);
            check(rm && list2.empty() && s.Exists(L"presets\\20261111-120000.txt"),
                  L"custom preset store: Remove hides it from the list and deletes nothing (its file stays)");
        }

        // ---- measured before/after ----
        Measurement live;
        std::wstring me;
        bool took = TakeMeasurement(L"before", L"19045", L"machine-x", L"before Stripped (Services)", &live, &me);
        check(took && live.ramTotal > 0 && live.ramUsed > 0 && live.ramUsed <= live.ramTotal && live.processes > 0 && live.created.size() == 19 &&
                  live.boot.size() == 19 && live.boot < live.created,
              L"measure: live values read (RAM, commit, " + std::to_wstring(live.processes) + L" processes, " + std::to_wstring(live.svchost) +
                  L" svchost)" + (me.empty() ? L"" : L": " + me));
        {
            Store s(NewTempDir(L"measure"));
            Measurement m = live;
            bool sv = SaveMeasurement(s, &m, &me);
            std::vector<std::wstring> probs;
            auto ms = LoadMeasurements(s, &probs);
            std::string text;
            Measurement rp;
            bool rd = s.Read(L"measure\\" + m.file, &text, &me) && ParseMeasurement(text, &rp, &me);
            check(sv && ms.size() == 1 && probs.empty() && rd && WriteMeasurement(rp) == text && At(ms, 0).ramUsed == live.ramUsed &&
                      At(ms, 0).commit == live.commit && At(ms, 0).processes == live.processes && At(ms, 0).svchost == live.svchost &&
                      At(ms, 0).uptimeMin == live.uptimeMin && At(ms, 0).created == live.created && At(ms, 0).plan == live.plan,
                  L"measure: record -> data folder -> read back is the same record, byte for byte" + (me.empty() ? L"" : L": " + me));
        }
        {   // Pairing: an after counts only from a boot that started after its before.
            auto mk = [](const wchar_t* phase, const wchar_t* created, const wchar_t* boot) {
                Measurement m;
                m.phase = phase; m.created = created; m.boot = boot; m.machine = L"m";
                return m;
            };
            std::vector<Measurement> ms = {mk(L"before", L"2026-11-11 08:12:31", L"2026-11-11 08:00:05"),
                                           mk(L"after", L"2026-11-11 08:20:00", L"2026-11-11 08:00:05")};   // same boot
            MeasurePair same = LatestPair(ms);
            int pendSame = PendingBefore(ms, L"2026-11-11 08:00:05");
            int pendNew = PendingBefore(ms, L"2026-11-11 09:05:40");
            ms.push_back(mk(L"after", L"2026-11-11 09:18:02", L"2026-11-11 09:05:40"));
            MeasurePair pr = LatestPair(ms);
            check(same.before < 0 && pendSame < 0 && pendNew == 0 && pr.before == 0 && pr.after == 2 && PendingBefore(ms, L"2026-11-11 09:05:40") < 0,
                  L"measure: an after from the same boot is not paired; after a restart the before is pending until its after is taken");
            std::string bad = WriteMeasurement(At(ms, 0));
            bad = bad.substr(0, bad.find("V\tprocesses"));
            Measurement x;
            std::wstring e;
            bool okb = ParseMeasurement(bad, &x, &e);
            check(!okb && has(e, L"processes"), L"measure: a record missing a value is refused (\"" + e + L"\")");
        }
        {   // The fixture's pair.
            Store fs(fixture);
            auto ms = LoadMeasurements(fs, nullptr);
            MeasurePair pr = LatestPair(ms);
            check(ms.size() == 2 && pr.before == 0 && pr.after == 1, L"measure: the fixture's before and after (one restart apart) pair up");
        }
        {   // R6 (review): which Hadron applies fall between a before and its after (from the undo logs).
            Store fs(fixture);
            auto ms = LoadMeasurements(fs, nullptr);
            if (ms.size() == 2) {
                const Measurement& b = ms[0];   // created 2026-11-11 08:12:31
                const Measurement& a = ms[1];   // booted 2026-11-11 09:05:40
                auto log = [&](const wchar_t* stamp, const wchar_t* source, bool rows, const std::wstring& machine) {
                    UndoLog l;
                    l.stamp = stamp; l.source = source; l.hasB = true; l.machine = machine;
                    if (rows) l.rows.push_back(UndoRow{});
                    return l;
                };
                std::vector<UndoLog> logs = {
                    log(L"20261111-081000", L"preset:bloat:services", true, b.machine),           // before the before
                    log(L"20261111-084500", L"custom:services:Gaming rig", true, b.machine),      // between: counted
                    log(L"20261111-085000", L"preset:stripped:services", false, b.machine),       // no rows written
                    log(L"20261111-085500-2", L"single", true, L"another-machine"),               // another PC
                    log(L"20261111-090000", L"preset:minimum:services", true, b.machine),         // between: counted
                    log(L"20261111-091000", L"preset:minimum:drivers", true, b.machine),          // after the after's boot
                };
                auto ap = AppliesBetween(b, a, logs);
                check(ap.size() == 2 && ap[0].source == L"custom:services:Gaming rig" && ap[0].when == L"2026-11-11 08:45:00" &&
                          ap[1].source == L"preset:minimum:services" && AppliesBetween(b, a, {}).empty(),
                      L"measure: only applies of this machine with written rows between the before and the after's boot are named");
            }
        }
    }

    // ---- the page (module.cpp), on the fixture ---------------------------------------------------------------
    if (ui) ui(fixture, check);

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
// --dump-stock-live [--out <file>]: this PC's WinSxS stock values, read with the live reader only. Read only: no
// backend (nothing armed, no SCM or registry writes), no data folder, no cache. Exit 1 with the reason when the
// reader can't start (wcp.dll or msdelta.dll) or no manifest could be read.
int RunDumpStockLive(const std::vector<std::wstring>& args) {
    const std::wstring out = ArgValue(args, L"--out");
    std::unique_ptr<SxsReader> r = MakeLiveSxsReader();
    ULONGLONG t0 = GetTickCount64();
    SxsStock s = ScanSxs(*r);
    std::string text = SxsDumpTsv(s, GetTickCount64() - t0);
    bool wrote = true;
    std::wstring werr;
    if (!out.empty()) wrote = WriteNewOut(out, text, &werr);
    else {
        fflush(stdout);
        _setmode(_fileno(stdout), _O_BINARY);
        fwrite(text.data(), 1, text.size(), stdout);
        fflush(stdout);
    }
    if (!wrote) { fwprintf(stderr, L"dump-stock-live: %ls\n", werr.c_str()); return 1; }
    if (!s.ok) { fwprintf(stderr, L"dump-stock-live: %ls\n", s.err.c_str()); return 1; }
    fwprintf(stderr, L"dump-stock-live: %d manifests, %d failures, %d skipped, %d values\n", s.manifests, s.failed, s.skipped,
             s.Usable());
    return 0;
}

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
        std::wstring werr;
        if (!WriteNewOut(out, text, &werr)) { fwprintf(stderr, L"dump-plan: %ls\n", werr.c_str()); return 1; }
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
#ifndef HX_SERVICES_TEST_HOOKS
    // R25: a release build has no command-line write path. CI's live test uses a -DHADRON_SERVICES_TEST_HOOKS=ON build.
    (void)args;
    wprintf(L"--live-test is not in this build (it exists only in a -DHADRON_SERVICES_TEST_HOOKS=ON test build)\n");
    fflush(stdout);
    return 2;
#else
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
    if (!st.EnsureRoot(true, &err)) { wprintf(L"FAIL: data dir: %ls\n", err.c_str()); return 1; }
    wprintf(L"  data dir %ls\n", root.c_str());
    std::unique_ptr<TiProvider> ti = useTi ? MakeLiveTi(b.get()) : nullptr;
    Ctx c;
    c.build = b->Build();
    c.machine = b->MachineGuid();
    c.tiOn = useTi;
    {   // R12: an apply needs first.txt, as in the module (here it records the dummy service only)
        Snapshot first;
        FirstRunResult fr = EnsureFirstSnapshot(st, {item}, c, &first);
        if (!fr.error.empty()) { wprintf(L"FAIL: first.txt: %ls\n", fr.error.c_str()); return 1; }
    }
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
        Plan u = BuildUndo(At(logs, k), *b, items, c);
        if (useTi) for (auto& r : u.rows) r.needsTi = true;
        if (!run(u, L"undo")) break;
        verify(original, L"undone");
        b->Query(name, &item, &err);
    }
    wprintf(L"services live test: %ls\n", fails ? L"FAIL" : L"PASS");
    fflush(stdout);
    return fails ? 1 : 0;
#endif
}

}  // namespace hx::services
