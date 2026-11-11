// modules/services/module.cpp — the Services and drivers page (docs/design/services.md,
// spec docs/pipeline/svcdrv/2-research.md). UI only: every decision lives in plan.cpp, every write in
// backend_live.cpp (inert until Arm(), which only the confirmed-plan path below calls).
//
// Window: like bios/diskmap/runas, an own view class ("Hadron.services.View") instead of core's
// CreateModuleContent, because that helper does not forward the workers' completion messages,
// WM_MOUSELEAVE or WM_GETDLGCODE. Paint goes through Module::Paint (WM_PAINT via a back buffer,
// WM_PRINTCLIENT and --screenshot), input through OnInput.
//
// Lists use core/ui ListView (string cells, header-click sort, wheel + draggable bar, keyboard, focus
// rect). The detail pane is drawn here because core's DetailPane is single-line and KB text wraps.
#include "core/host/cmdline.h"
#include "core/host/registry.h"
#include "core/host/run.h"
#include "core/sys/settings.h"
#include "core/sys/sys.h"
#include "core/sys/worker.h"
#include "core/ui/draw.h"
#include "core/ui/widgets.h"
#include "modules/services/backend.h"
#include "modules/services/cli.h"
#include "modules/services/hw.h"
#include "modules/services/kb.h"
#include "modules/services/model.h"
#include "modules/services/plan.h"
#include "modules/services/store.h"
#include "modules/services/ti.h"

#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace hx {
using namespace hx::ui;
using namespace hx::services;

namespace {

constexpr UINT WM_SVC_LOADED = WM_MODULE_BASE + 1;    // load worker -> view
constexpr UINT WM_SVC_APPLIED = WM_MODULE_BASE + 2;   // apply worker -> view
constexpr UINT_PTR kProgressTimer = 1;
const wchar_t* const kViewClass = L"Hadron.services.View";
const wchar_t* const kAppTitle = L"Services and drivers";

enum ClickId {
    ID_NONE = 0, ID_REFRESH, ID_UNDO_LAST, ID_TAB_SVC, ID_TAB_DRV, ID_TI, ID_SEARCH,
    ID_APPLY, ID_CANCEL_PLAN, ID_CANCEL_APPLY, ID_INT_UNDO, ID_INT_RESUME, ID_RESULT_UNDO, ID_DISMISS, ID_TI_STOP,
    ID_PRESET = 100,   // + 0 first snapshot, 1 Windows stock, 2 bloat, 3 stripped, 4 minimum
    ID_SET = 200,      // + start type 2..4, + 5 for Auto (delayed)
    ID_CTL = 300,      // + 0 start, 1 stop, 2 restart
};
enum class Focus { List, Search, Detail };
enum class Mode { List, Plan };

struct Click { RECT rc; int id; };
struct Banner {
    std::wstring text;
    int tone;   // 0 info, 1 warn, 2 bad
    std::vector<std::pair<int, std::wstring>> actions;
};

COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t), (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
int Lum(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }
COLORREF OnColor(COLORREF fill) { return Lum(fill) > 140 ? RGB(10, 20, 30) : RGB(245, 247, 250); }

// Palette over the theme, with the high-contrast fallback to system colours.
struct Pal {
    COLORREF bg, panel, hover, text, mute, faint, acc, onAcc, warn, bad, good, chip, chipHot, border;
    explicit Pal(const Theme& t) {
        if (t.highContrast) {
            bg = panel = GetSysColor(COLOR_WINDOW);
            hover = chip = chipHot = GetSysColor(COLOR_BTNFACE);
            text = mute = faint = GetSysColor(COLOR_WINDOWTEXT);
            acc = warn = bad = good = GetSysColor(COLOR_HOTLIGHT);
            onAcc = GetSysColor(COLOR_WINDOW);
            border = GetSysColor(COLOR_WINDOWTEXT);
            return;
        }
        bg = t.bg; panel = t.panel; hover = t.panel2;
        text = t.text1; mute = t.text2; faint = t.text3;
        acc = t.accent; onAcc = OnColor(t.accent);
        warn = t.warn; bad = t.bad; good = t.good;
        chip = t.panel2; chipHot = t.panel3; border = t.border;
    }
};

// One paragraph of the wrapped detail document.
enum ParaKind { P_TITLE, P_SUB, P_HEAD, P_TEXT, P_KV, P_GAP };
struct Para { ParaKind kind; std::wstring label, text; COLORREF color; };

// The request a plan was built from, so it can be rebuilt (TI toggle, refresh) with the same intent.
struct PlanReq {
    enum Type { None, Preset, Default, Stock, Single, Control, Undo, Resume } type = None;
    int level = 0, tab = 0;
    std::wstring item;
    StartCfg target;
    int what = 0;
    int logIndex = -1;
};

struct LoadOut {
    bool ok = false;
    std::wstring err, storeErr;
    std::vector<SvcItem> items;
    HwInfo hw;
    std::wstring build, machine;
    bool hasFirst = false;
    Snapshot first;
    FirstRunResult fr;
    std::vector<UndoLog> logs;
    std::vector<std::wstring> logProblems;
};

struct Job {
    Plan plan;
    bool control = false;
    size_t total = 0;
    ApplyResult res;
};

std::wstring ShortLevel(int lv) {
    switch (lv) { case LV_BLOAT: return L"bloat"; case LV_STRIPPED: return L"stripped"; case LV_MINIMUM: return L"minimum"; }
    return L"";
}

// ============================================================================================
class ServicesModule : public Module {
public:
    const ModuleInfo& Info() const override {
        static const ModuleInfo i{L"services", L"Services", L"Windows services and drivers: start types, presets, undo",
                                  Admin::Required};
        return i;
    }
    HWND Create(IHost& host, HWND parent, const RECT& rc) override;
    void OnDpiChanged(UINT) override { Invalidate(); }
    bool OnInput(UINT msg, WPARAM w, LPARAM l) override;
    bool CanClose() override {
        if (!applyW_.Running()) return true;
        if (host_) host_->SetStatus(L"Services: an apply is running; wait for it to finish (or press Cancel) before closing.");
        return false;
    }
    void Shutdown() override {
        cancelApply_.store(true);
        loadW_.Cancel(); applyW_.Cancel();
        loadW_.Join(); applyW_.Join();
        if (hwnd_) KillTimer(hwnd_, kProgressTimer);
    }
    int RunCli(int argc, wchar_t** argv) override;
    void LoadFixture(const wchar_t* dir) override;
    void Paint(HDC dc, const RECT& rc) override;

private:
    static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT OnView(HWND h, UINT m, WPARAM w, LPARAM l);
    PaintCtx PCtx() const { return PaintCtx{host_->Theme(), host_->Fonts(), host_->Dpi(), host_->Layout()}; }
    int Sc(int px) const { return S(px, host_ ? host_->Dpi() : 96); }
    bool Console() const { return host_ && host_->Layout() == Layout::Console; }
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void AddClick(const RECT& r, int id) { clicks_.push_back({r, id}); }
    void Status(const std::wstring& s) { if (host_) host_->SetStatus(s.c_str()); }

    // data
    void StartLoad();
    void OnLoaded();
    void TakeData(std::vector<SvcItem> items, const HwInfo& hw, std::wstring build, std::wstring machine);
    void PrepareCtx();
    void Refilter();
    const SvcItem* Selected() const;
    void SelectName(const std::wstring& name);
    std::wstring KbColumn(const SvcItem& s) const;
    std::wstring ChangedText(const SvcItem& s) const;   // vs the first-run snapshot
    std::wstring BasisShort(const SvcItem& s) const;
    bool CanApply(std::wstring* why) const;
    int WriteAccess(const std::wstring& name) const;   // 0 admin can write, 1 refused (needs TI), -1 unknown

    // plans
    void OpenPlan(const PlanReq& r);
    Plan BuildFrom(const PlanReq& r);
    void ClosePlan() { mode_ = Mode::List; planReq_ = PlanReq{}; focus_ = Focus::List; }
    void RefreshPlanList();
    void TogglePlanRow(int row);
    void StartApply();
    void OnApplied();
    void OnClickId(int id);

    // paint
    int TextW(HDC dc, const std::wstring& s, HFONT f) const;
    void Chip(HDC dc, int& x, int y, const std::wstring& label, int id, bool on, bool enabled = true);
    int Button(HDC dc, int right, int y, const std::wstring& label, int id, bool primary, bool enabled);
    int ButtonL(HDC dc, int left, int y, const std::wstring& label, int id, bool on, bool enabled);
    std::vector<Banner> Banners() const;
    int PaintBanners(HDC dc, const RECT& rc, int y);
    void PaintListMode(HDC dc, const RECT& rc, int y);
    void PaintPlanMode(HDC dc, const RECT& rc, int y);
    int PaintDoc(HDC dc, const RECT& r, const std::vector<Para>& doc, int offset, bool draw);
    void PaintDetailPanel(HDC dc, const RECT& r, const std::vector<Para>& doc);
    std::vector<Para> ItemDoc(const SvcItem& s) const;
    std::vector<Para> PlanRowDoc(const PlanRow& r) const;
    bool SetListColumns(int widthPx);   // true when the visible columns changed
    void SetPlanColumns(int widthPx);

    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;

    Kb kb_;
    bool kbOk_ = false;
    std::wstring kbErr_;
    std::unique_ptr<Backend> backend_;
    std::unique_ptr<FakeBackend> fake_;
    std::unique_ptr<TiProvider> ti_;
    Store store_;
    bool fixture_ = false, readOnly_ = false, elevated_ = true, loaded_ = false, writesOff_ = false;
    std::wstring fixtureDir_, loadErr_, storeNote_;
    std::vector<SvcItem> items_;
    HwInfo hw_;
    std::wstring build_, machine_;
    Snapshot first_;
    bool hasFirst_ = false;
    std::vector<UndoLog> logs_;
    int interrupted_ = -1, undoable_ = -1;
    bool tiLeft_ = false;
    std::map<std::wstring, StartCfg> lastApplied_;
    std::set<std::wstring> drift_;
    services::Ctx ctx_;

    // view state
    Mode mode_ = Mode::List;
    int tab_ = TAB_SERVICES;
    bool tiOn_ = false;   // never persisted (spec §5.6)
    Focus focus_ = Focus::List;
    SearchBox search_;
    ListView list_;
    std::vector<int> V_;   // items_ indices on the current tab, filtered and sorted
    int sortCol_ = -1;
    bool sortAsc_ = true;
    std::wstring selName_;
    ScrollBar detSb_;
    RECT rcSearch_{}, rcList_{}, rcDetail_{}, rcDetSb_{}, rcPlanList_{};
    std::vector<Click> clicks_;
    int hoverId_ = 0;
    bool tracking_ = false;
    std::vector<std::wstring> colKeys_;   // what each visible list column shows
    mutable std::map<std::wstring, int> writeAccess_;   // CanWrite results, per name, until the next load

    // plan screen
    Plan plan_;
    PlanReq planReq_;
    ListView plist_;
    std::wstring planErr_;

    // workers
    sys::Worker loadW_, applyW_;
    LoadOut loadOut_;
    Job job_;
    std::atomic<bool> cancelApply_{false};
    bool haveResult_ = false;
    ApplyResult result_;
    std::wstring resultTitle_;
    bool resultControl_ = false;
    std::vector<std::wstring> notices_;

    // --screenshot / --fixture arguments applied after the fixture loads
    std::wstring argSelect_, argPlan_;
};

// ============================================================================================
// Window

LRESULT CALLBACK ServicesModule::ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = reinterpret_cast<ServicesModule*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->OnView(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT ServicesModule::OnView(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, rc.right, rc.bottom) : nullptr;
            if (mem && bmp) {
                HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
                Paint(mem, rc);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
            } else {
                Paint(dc, rc);
            }
            if (bmp) DeleteObject(bmp);
            if (mem) DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: { RECT rc; GetClientRect(h, &rc); Paint((HDC)w, rc); return 0; }
    case WM_SIZE: Invalidate(); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
    case WM_SETFOCUS: case WM_KILLFOCUS: Invalidate(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, hoverId_ == ID_SEARCH ? IDC_IBEAM : (hoverId_ > 0 ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hoverId_) { hoverId_ = 0; Invalidate(); }
        return 0;
    case WM_CAPTURECHANGED:
        if (detSb_.Dragging()) detSb_.OnMouse(WM_LBUTTONUP, 0, 0, rcDetSb_);
        search_.CancelDrag();
        Invalidate();
        return 0;
    case WM_TIMER:
        if (w == kProgressTimer) Invalidate();
        return 0;
    case WM_SVC_LOADED: OnLoaded(); return 0;
    case WM_SVC_APPLIED: OnApplied(); return 0;
    case WM_RBUTTONDOWN: SetFocus(h); return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(h);
        [[fallthrough]];
    case WM_LBUTTONUP: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_CHAR: case WM_RBUTTONUP:
        if (m == WM_MOUSEMOVE && !tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        if (OnInput(m, w, l)) Invalidate();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND ServicesModule::Create(IHost& host, HWND parent, const RECT& rc) {
    host_ = &host;
    elevated_ = host.IsElevated();
    kbOk_ = LoadEmbeddedKb(&kb_, &kbErr_);
    if (!kbOk_) host.Log(LogLevel::Error, (L"services: " + kbErr_).c_str());
    search_.placeholder = L"Filter by name, display name, company or group";
    search_.onChanged = [this](const std::wstring&) { Refilter(); };
    list_.onSortChanged = [this](int col, bool asc) {
        sortCol_ = col; sortAsc_ = asc;
        Refilter();
        if (!fixture_) { host_->Prefs().SetInt(L"sort", col); host_->Prefs().SetBool(L"sortAsc", asc); }
    };
    plist_.onSortChanged = [this](int col, bool asc) {
        auto key = [&](const PlanRow& r) -> std::wstring {
            switch (col) {
                case 0: return !r.blocked.empty() ? L"2" : (r.include ? L"0" : L"1");
                case 2: return std::to_wstring(r.before.start);
                case 3: return std::to_wstring(r.after.start);
                case 4: return Lower(!r.blocked.empty() ? r.blocked : !r.warning.empty() ? r.warning : r.reason);
            }
            return Lower(r.name);
        };
        std::stable_sort(plan_.rows.begin(), plan_.rows.end(),
                         [&](const PlanRow& a, const PlanRow& b) { return asc ? key(a) < key(b) : key(b) < key(a); });
        RefreshPlanList();
    };

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &ServicesModule::ViewProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kViewClass;
        if (!RegisterClassExW(&wc)) {
            host.Log(LogLevel::Error, (L"services: RegisterClassEx failed: " + sys::ErrorText(GetLastError())).c_str());
            return nullptr;
        }
        registered = true;
    }
    hwnd_ = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        host.Log(LogLevel::Error, (L"services: CreateWindowEx failed: " + sys::ErrorText(GetLastError())).c_str());
        return nullptr;
    }

    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    if (ArgValue(cl.rest, L"--tab") == L"drivers") tab_ = TAB_DRIVERS;
    argSelect_ = ArgValue(cl.rest, L"--select");
    argPlan_ = ArgValue(cl.rest, L"--plan");
    if (ArgValue(cl.rest, L"--ti") == L"on") tiOn_ = true;
    readOnly_ = HasArg(cl.rest, L"--readonly");
    std::wstring dataDir = ArgValue(cl.rest, L"--data-dir");
    fixture_ = !cl.screenshot.empty() || cl.selftest || !cl.fixture.empty();
    if (fixture_) {
        // Fixture mode: the fake backend, the fixture folder as a read-only data dir. Nothing is written,
        // nothing touches real services or %ProgramData%. --screenshot loads through LoadFixture (host).
        if (cl.screenshot.empty() && !cl.fixture.empty()) LoadFixture(cl.fixture.c_str());
        return hwnd_;
    }
    tab_ = host.Prefs().GetInt(L"tab", tab_) == TAB_DRIVERS ? TAB_DRIVERS : TAB_SERVICES;
    sortCol_ = host.Prefs().GetInt(L"sort", -1);
    sortAsc_ = host.Prefs().GetBool(L"sortAsc", true);
    search_.SetText(host.Prefs().GetStr(L"filter", L""));
    backend_ = MakeLiveBackend();
    if (!dataDir.empty()) store_.SetRoot(sys::ToExtended(dataDir));
    else if (!readOnly_) {
        std::wstring err;
        std::wstring root = Store::DefaultRoot(&err);
        if (root.empty()) storeNote_ = L"No data folder: " + err + L". Changes are off.";
        else store_.SetRoot(root);
    }
    if (!elevated_) readOnly_ = true;
    writesOff_ = readOnly_ || !store_.Valid();
    StartLoad();
    return hwnd_;
}

// ============================================================================================
// Data

void ServicesModule::StartLoad() {
    if (!backend_ || loadW_.Running() || applyW_.Running()) return;
    loaded_ = false;
    loadErr_.clear();
    Backend* b = backend_.get();
    Store st = store_;
    bool mayWrite = !readOnly_ && st.Valid();
    LoadOut* out = &loadOut_;
    bool started = loadW_.Start(hwnd_, WM_SVC_LOADED, [b, st, mayWrite, out]() mutable {
        *out = LoadOut{};
        std::wstring err;
        if (!b->Enumerate(&out->items, &err)) { out->err = L"Could not read the services: " + err; return; }
        for (auto& i : out->items) i.company = FileCompany(i);
        b->ReadHw(&out->hw);
        out->build = b->Build();
        out->machine = b->MachineGuid();
        FoldInstances(&out->items);
        if (st.Valid()) {
            if (mayWrite) {
                if (!st.EnsureRoot(true, &err)) out->storeErr = L"Could not create the data folder: " + err;
                else {
                    out->fr = EnsureFirstSnapshot(st, out->items, out->build, out->machine, &out->first);
                    out->hasFirst = (out->fr.wrote || out->fr.existed) && !out->fr.foreign;
                }
            } else if (st.Exists(L"first.txt")) {
                std::string text;
                if (st.Read(L"first.txt", &text, &err) && ParseSnapshot(text, &out->first, &err)) out->hasFirst = true;
            }
            out->logs = LoadLogs(st, &out->logProblems);
        }
        out->ok = true;
    });
    if (!started) loadErr_ = L"Could not start the worker thread: " + sys::ErrorText(GetLastError());
    Invalidate();
}

void ServicesModule::OnLoaded() {
    loadW_.Join();
    LoadOut& o = loadOut_;
    notices_.clear();
    if (!o.ok) {
        loadErr_ = o.err;
        loaded_ = true;
        Invalidate();
        return;
    }
    hasFirst_ = o.hasFirst;
    first_ = std::move(o.first);
    logs_ = std::move(o.logs);
    if (!o.storeErr.empty()) notices_.push_back(o.storeErr);
    if (!o.fr.error.empty()) notices_.push_back(L"First-run snapshot: " + o.fr.error);
    if (!o.fr.note.empty()) notices_.push_back(o.fr.note);
    if (o.fr.wrote && !o.fr.corrupt)
        notices_.push_back(L"Recorded this PC's start types in first.txt, with a recovery folder and zip, before any change.");
    for (auto& p : o.logProblems) notices_.push_back(L"Undo log skipped: " + p);
    TakeData(std::move(o.items), o.hw, o.build, o.machine);
    if (planReq_.type != PlanReq::None && !applyW_.Running()) OpenPlan(planReq_);
}

void ServicesModule::TakeData(std::vector<SvcItem> items, const HwInfo& hw, std::wstring build, std::wstring machine) {
    items_ = std::move(items);
    hw_ = hw;
    build_ = std::move(build);
    machine_ = std::move(machine);
    loaded_ = true;
    interrupted_ = FindInterrupted(logs_);
    undoable_ = FindUndoable(logs_);
    tiLeft_ = TiLeftRunning(logs_);
    lastApplied_ = LastApplied(logs_);
    writeAccess_.clear();
    drift_.clear();
    for (auto& s : items_) {
        auto it = lastApplied_.find(Lower(s.name));
        if (it != lastApplied_.end() && !s.cfg.SameEffect(it->second)) drift_.insert(Lower(s.name));
    }
    if (tiLeft_) {   // only worth a banner while TrustedInstaller is actually running
        bool running = false;
        for (auto& s : items_) if (IEq(s.name, L"TrustedInstaller")) running = s.state == SERVICE_RUNNING;
        tiLeft_ = running;
    }
    PrepareCtx();
    Refilter();
    Status(L"Services: loaded " + std::to_wstring(items_.size()) + L" services and drivers");
    Invalidate();
}

void ServicesModule::PrepareCtx() {
    ctx_ = services::Ctx{};
    ctx_.kb = kbOk_ ? &kb_ : nullptr;
    ctx_.hw = hw_.ok ? &hw_ : nullptr;
    ctx_.first = hasFirst_ ? &first_ : nullptr;
    ctx_.build = build_;
    ctx_.machine = machine_;
    ctx_.tiOn = tiOn_;
    ctx_.Prepare();
}

void ServicesModule::LoadFixture(const wchar_t* dir) {
    fixture_ = true;
    fixtureDir_ = dir ? dir : L"";
    FixtureData fx;
    std::wstring err;
    if (!LoadFixtureDir(fixtureDir_, &fx, &err)) {
        loadErr_ = L"Could not load the fixture: " + err;
        loaded_ = true;
        Invalidate();
        return;
    }
    fake_ = std::make_unique<FakeBackend>(std::move(fx.backend));
    hasFirst_ = fx.hasFirst;
    first_ = std::move(fx.first);
    store_.SetRoot(fixtureDir_);
    logs_ = LoadLogs(store_, nullptr);
    store_.SetRoot(L"");   // read once; fixture mode never writes
    writesOff_ = true;
    for (auto& p : fx.problems) notices_.push_back(p);
    std::vector<SvcItem> items;
    fake_->Enumerate(&items, &err);
    FoldInstances(&items);
    HwInfo hw;
    fake_->ReadHw(&hw);
    TakeData(std::move(items), hw, fake_->Build(), fake_->MachineGuid());
    if (!argSelect_.empty()) SelectName(argSelect_);
    if (!argPlan_.empty()) {
        int lv = LevelFromName(argPlan_);
        PlanReq r;
        r.tab = tab_;
        if (lv >= 0) { r.type = PlanReq::Preset; r.level = lv; }
        else if (lv == LEVEL_DEFAULT) r.type = PlanReq::Default;
        else if (lv == -2) r.type = PlanReq::Stock;
        else if (argPlan_ == L"undo" && FindInterrupted(logs_) >= 0) { r.type = PlanReq::Undo; r.logIndex = FindInterrupted(logs_); }
        else if (argPlan_ == L"resume" && FindInterrupted(logs_) >= 0) { r.type = PlanReq::Resume; r.logIndex = FindInterrupted(logs_); }
        if (r.type != PlanReq::None) OpenPlan(r);
    }
}

std::wstring ServicesModule::KbColumn(const SvcItem& s) const {
    Guard g = CheckGuard(s, ctx_);
    if (g.neverTouch) return L"protected";
    const KbEntry* e = kbOk_ ? kb_.Find(s.name) : nullptr;
    if (g.neverPreset) return e ? L"no preset" : L"\u2014";
    if (!e) return L"\u2014";
    int j = e->JoinLevel();
    return j >= 0 ? ShortLevel(j) : L"KB";
}

std::wstring ServicesModule::ChangedText(const SvcItem& s) const {
    if (!hasFirst_) return L"";
    const SvcItem* f = first_.Find(s.name);
    if (!f) return L"new";
    return f->cfg.SameEffect(s.cfg) ? L"" : L"changed";
}

std::wstring ServicesModule::BasisShort(const SvcItem& s) const {
    const KbEntry* e = kbOk_ ? kb_.Find(s.name) : nullptr;
    if (!e) return L"";
    const std::wstring& b = e->JoinLevel() >= 0 ? e->pbasis : e->basis;
    return b == L"owner-measured" ? L"measured" : b;
}

void ServicesModule::Refilter() {
    std::wstring keep = selName_;
    if (colKeys_.empty()) colKeys_ = {L"name", L"display", L"state", L"start", L"kb"};
    V_.clear();
    const std::wstring q = Lower(search_.Text());
    for (int i = 0; i < (int)items_.size(); i++) {
        const SvcItem& s = items_[i];
        if (!OnTab(s, (Tab)tab_)) continue;
        if (!q.empty() && !IContains(s.name, q) && !IContains(s.display, q) && !IContains(s.company, q) &&
            !IContains(s.group, q) && !IContains(s.account, q))
            continue;
        V_.push_back(i);
    }
    // Sort by the visible column's key; the default order is the name.
    std::wstring key = sortCol_ >= 0 && sortCol_ < (int)colKeys_.size() ? colKeys_[sortCol_] : L"name";
    auto text = [&](const SvcItem& s) -> std::wstring {
        if (key == L"display") return Lower(s.display);
        if (key == L"state") return std::to_wstring(s.state == SERVICE_RUNNING ? 0 : 1) + StateName(s.state);
        if (key == L"start") return std::to_wstring(s.cfg.start) + (s.cfg.Delayed() ? L"1" : L"0");
        if (key == L"kb") return KbColumn(s);
        if (key == L"company") return Lower(s.company);
        if (key == L"account") return Lower(s.account);
        if (key == L"group") return Lower(s.group);
        if (key == L"type") return TypeName(s.type);
        if (key == L"changed") return ChangedText(s);
        if (key == L"basis") return BasisShort(s);
        return Lower(s.name);
    };
    bool asc = sortCol_ < 0 ? true : sortAsc_;
    std::stable_sort(V_.begin(), V_.end(), [&](int a, int b) {
        std::wstring ka = text(items_[a]), kb = text(items_[b]);
        if (ka == kb) return Lower(items_[a].name) < Lower(items_[b].name);
        return asc ? ka < kb : kb < ka;
    });
    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(V_.size());
    for (int i : V_) {
        const SvcItem& s = items_[i];
        std::vector<std::wstring> r;
        for (auto& k : colKeys_) {
            if (k == L"name") r.push_back(s.name);
            else if (k == L"display") r.push_back(s.display);
            else if (k == L"state") r.push_back(StateName(s.state));
            else if (k == L"start") r.push_back(StartLabel(s.cfg, s.trigger) + (drift_.count(Lower(s.name)) ? L"  *" : L""));
            else if (k == L"kb") r.push_back(KbColumn(s));
            else if (k == L"company") r.push_back(s.company);
            else if (k == L"account") r.push_back(s.account);
            else if (k == L"group") r.push_back(s.group);
            else if (k == L"type") r.push_back(s.kind == Kind::FsDriver ? L"File system" : TypeName(s.type));
            else if (k == L"changed") r.push_back(ChangedText(s));
            else if (k == L"basis") r.push_back(BasisShort(s));
            else r.push_back(L"");
        }
        rows.push_back(std::move(r));
    }
    list_.SetRows(std::move(rows));
    if (!keep.empty()) SelectName(keep);
    else if (!V_.empty() && list_.Selected() < 0) { list_.Select(0); selName_ = items_[V_[0]].name; }
    if (!fixture_ && host_) {
        host_->Prefs().SetStr(L"filter", search_.Text());
        host_->Prefs().SetInt(L"tab", tab_);
    }
}

const SvcItem* ServicesModule::Selected() const {
    for (auto& s : items_) if (IEq(s.name, selName_)) return &s;
    return nullptr;
}

void ServicesModule::SelectName(const std::wstring& name) {
    for (int k = 0; k < (int)V_.size(); k++) {
        if (!IEq(items_[V_[k]].name, name)) continue;
        // ListView::Select does not scroll; select the row above and step down so it scrolls into view.
        if (k == 0) list_.Select(0);
        else if (host_ && rcList_.right > rcList_.left) { list_.Select(k - 1); list_.OnKey(VK_DOWN, rcList_, PCtx()); }
        else list_.Select(k);
        if (!IEq(selName_, name)) detSb_.SetOffset(0);
        selName_ = items_[V_[k]].name;
        return;
    }
    if (!V_.empty() && list_.Selected() >= 0 && list_.Selected() < (int)V_.size()) selName_ = items_[V_[list_.Selected()]].name;
}

int ServicesModule::WriteAccess(const std::wstring& name) const {
    auto it = writeAccess_.find(Lower(name));
    if (it != writeAccess_.end()) return it->second;
    Backend* b = fake_ ? (Backend*)fake_.get() : backend_.get();
    int r = -1;
    if (b && !applyW_.Running()) {
        bool denied = false;
        std::wstring err;
        if (b->CanWrite(name, &denied, &err)) r = 0;
        else if (denied) r = 1;
    }
    writeAccess_[Lower(name)] = r;
    return r;
}

bool ServicesModule::CanApply(std::wstring* why) const {
    if (fixture_) { *why = L"Fixture data: applying is off."; return false; }
    if (!elevated_) { *why = L"Not running as administrator: read-only."; return false; }
    if (readOnly_) { *why = L"Read-only mode (--readonly)."; return false; }
    if (!store_.Valid()) { *why = L"No data folder for the undo log: changes are off."; return false; }
    if (applyW_.Running() || loadW_.Running()) { *why = L"Busy."; return false; }
    return true;
}

// ============================================================================================
// Plans

Plan ServicesModule::BuildFrom(const PlanReq& r) {
    PrepareCtx();
    Backend* b = fake_ ? (Backend*)fake_.get() : backend_.get();
    Plan p;
    const SvcItem* it = nullptr;
    for (auto& s : items_) if (IEq(s.name, r.item)) it = &s;
    switch (r.type) {
        case PlanReq::Preset: p = BuildPreset(r.level, r.tab, items_, ctx_); break;
        case PlanReq::Default: p = BuildDefault(false, r.tab, items_, ctx_); break;
        case PlanReq::Stock: p = BuildDefault(true, r.tab, items_, ctx_); break;
        case PlanReq::Single:
            if (it) p = BuildSingle(*it, r.target, items_, ctx_);
            break;
        case PlanReq::Control:
            if (it && b) p = BuildControl(*it, r.what, *b, items_);
            break;
        case PlanReq::Undo:
            if (b && r.logIndex >= 0 && r.logIndex < (int)logs_.size()) p = BuildUndo(logs_[r.logIndex], *b, ctx_);
            break;
        case PlanReq::Resume:
            if (b && r.logIndex >= 0 && r.logIndex < (int)logs_.size()) p = BuildResume(logs_[r.logIndex], *b, items_, ctx_);
            break;
        case PlanReq::None: break;
    }
    if (b && p.kind != PlanKind::Control) MarkTi(&p, *b, tiOn_);
    return p;
}

void ServicesModule::OpenPlan(const PlanReq& r) {
    planReq_ = r;
    plan_ = BuildFrom(r);
    mode_ = Mode::Plan;
    focus_ = Focus::List;
    detSb_.SetOffset(0);
    RefreshPlanList();
    // Start on the item selected in the list when the plan has it, so the row detail answers "what about this one?".
    int k0 = 0;
    for (int k = 0; k < (int)plan_.rows.size(); k++)
        if (IEq(plan_.rows[k].name, selName_)) { k0 = k; break; }
    if (k0 > 0 && rcPlanList_.right > rcPlanList_.left) { plist_.Select(k0 - 1); plist_.OnKey(VK_DOWN, rcPlanList_, PCtx()); }
    else plist_.Select(k0);
    Invalidate();
}

void ServicesModule::RefreshPlanList() {
    std::vector<std::vector<std::wstring>> rows;
    for (auto& r : plan_.rows) {
        std::wstring box = !r.blocked.empty() ? L"[-]" : (r.include ? L"[x]" : L"[ ]");
        std::wstring note = !r.blocked.empty() ? r.blocked : !r.warning.empty() ? r.warning : r.reason;
        if (plan_.kind == PlanKind::Control)
            rows.push_back({box, r.name, r.ctl == Op::Stop ? L"stop" : L"start", L"", note});
        else
            rows.push_back({box, r.name, StartLabel(r.before, false), StartLabel(r.after, false), note});
    }
    plist_.SetRows(std::move(rows));
}

void ServicesModule::TogglePlanRow(int row) {
    if (row < 0 || row >= (int)plan_.rows.size()) return;
    PlanRow& r = plan_.rows[row];
    if (!r.canToggle || !r.blocked.empty()) {
        Status(L"Services: " + r.name + L" can't be included: " + (r.blocked.empty() ? r.warning : r.blocked));
        return;
    }
    r.include = !r.include;
    RefreshPlanList();
}

void ServicesModule::StartApply() {
    std::wstring why;
    if (!CanApply(&why)) { Status(L"Services: " + why); return; }
    if (plan_.kind != PlanKind::Control && plan_.Included() == 0) return;
    job_ = Job{};
    job_.plan = plan_;
    job_.control = plan_.kind == PlanKind::Control;
    job_.total = job_.control ? plan_.rows.size() : ApplyOrder(plan_).size();
    resultTitle_ = plan_.title;
    resultControl_ = job_.control;
    cancelApply_.store(false);
    bool needTi = false;
    for (auto& r : plan_.rows) if (r.include && r.needsTi) needTi = true;
    if (needTi && tiOn_ && !ti_) ti_ = MakeLiveTi(backend_.get());
    ApplyEnv env;
    env.backend = backend_.get();
    env.store = &store_;
    env.ti = tiOn_ ? ti_.get() : nullptr;
    env.tiOn = tiOn_;
    env.cancel = &cancelApply_;
    env.progress = &applyW_.progress;
    env.build = build_;
    env.machine = machine_;
    backend_->Arm();   // the only place the UI arms the live backend: a plan the user confirmed
    Job* job = &job_;
    bool ok = applyW_.Start(hwnd_, WM_SVC_APPLIED, [job, env]() {
        if (job->control) RunControl(job->plan, env, &job->res);
        else RunApply(job->plan, env, &job->res);
    });
    if (!ok) {
        backend_->Disarm();
        Status(L"Services: could not start the worker thread: " + sys::ErrorText(GetLastError()));
        return;
    }
    host_->SetBusy(true);
    SetTimer(hwnd_, kProgressTimer, 200, nullptr);
    Invalidate();
}

void ServicesModule::OnApplied() {
    applyW_.Join();
    if (backend_) backend_->Disarm();
    KillTimer(hwnd_, kProgressTimer);
    host_->SetBusy(false);
    result_ = job_.res;
    haveResult_ = true;
    ClosePlan();
    std::wstring s = result_.aborted ? L"stopped: " + result_.error
                                     : std::to_wstring(result_.ok) + L" changed, " + std::to_wstring(result_.skipped) +
                                           L" skipped, " + std::to_wstring(result_.failed) + L" failed";
    Status(L"Services: " + resultTitle_ + L": " + s);
    host_->Log(result_.aborted || result_.failed ? LogLevel::Warn : LogLevel::Info, (L"services apply: " + s).c_str());
    for (auto& l : result_.lines) host_->Log(LogLevel::Info, (L"services: " + l).c_str());
    StartLoad();
}

void ServicesModule::OnClickId(int id) {
    if (id == ID_REFRESH) { if (!fixture_) StartLoad(); return; }
    if (id == ID_TAB_SVC || id == ID_TAB_DRV) {
        int t = id == ID_TAB_SVC ? TAB_SERVICES : TAB_DRIVERS;
        if (t != tab_) { tab_ = t; selName_.clear(); detSb_.SetOffset(0); Refilter(); }
        return;
    }
    if (id == ID_TI) {
        tiOn_ = !tiOn_;
        Status(tiOn_ ? L"Services: TrustedInstaller toggle on for this session (never remembered)."
                     : L"Services: TrustedInstaller toggle off.");
        if (mode_ == Mode::Plan) OpenPlan(planReq_);
        return;
    }
    if (id == ID_SEARCH) { focus_ = Focus::Search; return; }
    if (id >= ID_PRESET && id < ID_PRESET + 5) {
        PlanReq r;
        r.tab = tab_;
        int k = id - ID_PRESET;
        if (k == 0) r.type = PlanReq::Default;
        else if (k == 1) r.type = PlanReq::Stock;
        else { r.type = PlanReq::Preset; r.level = k - 2; }
        OpenPlan(r);
        return;
    }
    if (id >= ID_SET && id < ID_SET + 6) {
        const SvcItem* s = Selected();
        if (!s) return;
        PlanReq r;
        r.type = PlanReq::Single;
        r.item = s->name;
        int k = id - ID_SET;
        r.target = k == 5 ? StartCfg{2, 1} : StartCfg{k, k == 2 ? 0 : -1};
        OpenPlan(r);
        return;
    }
    if (id >= ID_CTL && id < ID_CTL + 3) {
        const SvcItem* s = Selected();
        if (!s) return;
        PlanReq r;
        r.type = PlanReq::Control;
        r.item = s->name;
        r.what = id - ID_CTL;
        OpenPlan(r);
        return;
    }
    if (id == ID_APPLY) { StartApply(); return; }
    if (id == ID_CANCEL_PLAN) { ClosePlan(); return; }
    if (id == ID_CANCEL_APPLY) { cancelApply_.store(true); applyW_.Cancel(); Status(L"Services: cancelling after the current item…"); return; }
    if (id == ID_INT_UNDO || id == ID_INT_RESUME) {
        PlanReq r;
        r.type = id == ID_INT_UNDO ? PlanReq::Undo : PlanReq::Resume;
        r.logIndex = interrupted_;
        OpenPlan(r);
        return;
    }
    if (id == ID_UNDO_LAST || id == ID_RESULT_UNDO) {
        if (undoable_ < 0) return;
        PlanReq r;
        r.type = PlanReq::Undo;
        r.logIndex = undoable_;
        OpenPlan(r);
        return;
    }
    if (id == ID_DISMISS) { haveResult_ = false; notices_.clear(); return; }
    if (id == ID_TI_STOP) {
        PlanReq r;
        r.type = PlanReq::Control;
        r.item = L"TrustedInstaller";
        r.what = 1;
        OpenPlan(r);
        return;
    }
}

// ============================================================================================
// Input

bool ServicesModule::OnInput(UINT msg, WPARAM w, LPARAM l) {
    if (!host_) return false;
    PaintCtx c = PCtx();
    ListView& lv = mode_ == Mode::Plan ? plist_ : list_;
    RECT lrc = mode_ == Mode::Plan ? rcPlanList_ : rcList_;
    auto syncSel = [&]() {
        if (mode_ == Mode::List) {
            int k = list_.Selected();
            if (k >= 0 && k < (int)V_.size() && !IEq(items_[V_[k]].name, selName_)) {
                selName_ = items_[V_[k]].name;
                detSb_.SetOffset(0);
            }
        }
    };

    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) {
        int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
        POINT mp{mx, my};
        if (search_.Dragging() && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP)) {
            search_.OnMouse(hwnd_, msg, mx, my, rcSearch_, c);
            return true;
        }
        if (mode_ == Mode::List && msg != WM_MOUSEMOVE && msg != WM_LBUTTONUP && PtInRect(&rcSearch_, mp)) {
            focus_ = Focus::Search;
            search_.OnMouse(hwnd_, msg, mx, my, rcSearch_, c);
            return true;
        }
        if (msg == WM_LBUTTONDBLCLK) msg = WM_LBUTTONDOWN;
        if (msg == WM_RBUTTONUP) return false;
    }
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
        if (w == VK_TAB) {
            bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (mode_ == Mode::Plan) focus_ = focus_ == Focus::List ? Focus::Detail : Focus::List;
            else focus_ = (Focus)(((int)focus_ + (back ? 2 : 1)) % 3);
            return true;
        }
        if (ctrl && w == 'F' && mode_ == Mode::List) { focus_ = Focus::Search; return true; }
        if (w == VK_ESCAPE) {
            if (mode_ == Mode::Plan && !applyW_.Running()) { ClosePlan(); return true; }
            if (!search_.Text().empty()) { search_.SetText(L""); Refilter(); return true; }
            if (focus_ != Focus::List) { focus_ = Focus::List; return true; }
            return false;
        }
        if (focus_ == Focus::Detail) {
            int before = detSb_.Offset();
            int view = rcDetail_.bottom - rcDetail_.top;
            switch (w) {
                case VK_UP: detSb_.SetOffset(before - Sc(40)); break;
                case VK_DOWN: detSb_.SetOffset(before + Sc(40)); break;
                case VK_PRIOR: detSb_.SetOffset(before - view * 9 / 10); break;
                case VK_NEXT: detSb_.SetOffset(before + view * 9 / 10); break;
                case VK_HOME: detSb_.SetOffset(0); break;
                case VK_END: detSb_.SetOffset(detSb_.MaxOffset()); break;
                default: return false;
            }
            return detSb_.Offset() != before;
        }
        if (focus_ == Focus::Search && mode_ == Mode::List && w != VK_UP && w != VK_DOWN && w != VK_PRIOR && w != VK_NEXT) {
            bool r = search_.OnKey(w, hwnd_);
            if (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END || w == VK_DELETE || w == VK_BACK ||
                w == VK_INSERT || ctrl)
                return r;
        }
        if (mode_ == Mode::Plan && w == VK_SPACE) { TogglePlanRow(plist_.Selected()); return true; }
        if (mode_ == Mode::Plan && w == VK_RETURN) return false;   // never apply from a key press
        bool r = lv.OnKey(w, lrc, c);
        if (r) { syncSel(); detSb_.SetOffset(mode_ == Mode::Plan ? 0 : detSb_.Offset()); }
        return r;
    }
    if (msg == WM_CHAR) {
        wchar_t ch = (wchar_t)w;
        if (mode_ == Mode::Plan) return false;
        if (ch == 0x09 || ch == 0x1B || ch == 0x0D || ch == 0x06) return false;
        if (focus_ != Focus::Search) {
            if (ch < 0x20 || ch == 0x7F) return false;
            focus_ = Focus::Search;
        }
        return search_.OnChar(ch);
    }
    if (msg == WM_MOUSEWHEEL) {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd_, &p);
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (PtInRect(&rcDetail_, p)) {
            int before = detSb_.Offset();
            detSb_.SetOffset(before - delta * Sc(60) / WHEEL_DELTA);
            return detSb_.Offset() != before;
        }
        return lv.OnWheel(delta, lrc, c);
    }
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    POINT pt{x, y};
    if (msg == WM_MOUSEMOVE) {
        if (detSb_.Dragging()) return detSb_.OnMouse(msg, x, y, rcDetSb_);
        bool r = lv.OnMouse(msg, x, y, lrc, c);
        int hid = 0;
        for (auto& ck : clicks_) if (PtInRect(&ck.rc, pt)) { hid = ck.id; break; }
        if (hid != hoverId_) { hoverId_ = hid; r = true; }
        return r;
    }
    if (msg == WM_LBUTTONUP) {
        bool r = lv.OnMouse(msg, x, y, lrc, c);
        if (detSb_.Dragging()) r = detSb_.OnMouse(msg, x, y, rcDetSb_) || r;
        if (GetCapture() == hwnd_) ReleaseCapture();
        return r;
    }
    if (msg != WM_LBUTTONDOWN) return false;
    for (auto& ck : clicks_) {
        if (PtInRect(&ck.rc, pt)) {
            if (ck.id != ID_SEARCH && focus_ == Focus::Search) focus_ = Focus::List;
            OnClickId(ck.id);
            return true;
        }
    }
    if (detSb_.Visible() && PtInRect(&rcDetSb_, pt)) {
        focus_ = Focus::Detail;
        SetCapture(hwnd_);
        detSb_.OnMouse(msg, x, y, rcDetSb_);
        return true;
    }
    if (PtInRect(&lrc, pt)) {
        focus_ = Focus::List;
        SetCapture(hwnd_);   // the list's scrollbar keeps dragging outside the list
        int before = lv.Selected();
        bool r = lv.OnMouse(msg, x, y, lrc, c);
        if (mode_ == Mode::Plan) {
            // A click in the first column toggles the row (the [x] box).
            int boxW = Sc(52);
            if (x < lrc.left + boxW && lv.Selected() >= 0 && y > lrc.top + Sc(Console() ? 24 : 32)) TogglePlanRow(lv.Selected());
            if (lv.Selected() != before) detSb_.SetOffset(0);
        } else {
            syncSel();
        }
        return r || true;
    }
    if (PtInRect(&rcDetail_, pt)) { focus_ = Focus::Detail; return true; }
    return false;
}

// ============================================================================================
// Paint helpers

int ServicesModule::TextW(HDC dc, const std::wstring& s, HFONT f) const {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

void ServicesModule::Chip(HDC dc, int& x, int y, const std::wstring& label, int id, bool on, bool enabled) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int h = Sc(Console() ? 22 : 26);
    int w = TextW(dc, label, f.caption) + Sc(Console() ? 16 : 22);
    RECT r{x, y, x + w, y + h};
    COLORREF fill = on ? p.acc : (enabled && hoverId_ == id ? p.chipHot : p.chip);
    if (!enabled) fill = Mix(fill, p.bg, 0.5);
    RoundFill(dc, r, Console() ? Sc(4) : h, fill);
    COLORREF tc = on ? p.onAcc : p.text;
    if (!enabled) tc = Mix(tc, fill, 0.5);
    ui::DrawText(dc, label, r, f.caption, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    if (enabled) AddClick(r, id);
    x += w + Sc(Console() ? 6 : 8);
}

int ServicesModule::Button(HDC dc, int right, int y, const std::wstring& label, int id, bool primary, bool enabled) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int h = Sc(Console() ? 24 : 30);
    int w = TextW(dc, label, f.caption) + Sc(26);
    RECT r{right - w, y, right, y + h};
    bool hot = enabled && hoverId_ == id;
    COLORREF fill = primary ? (hot ? Mix(p.acc, RGB(255, 255, 255), 0.18) : p.acc) : (hot ? p.chipHot : p.chip);
    if (!enabled) fill = Mix(fill, p.bg, 0.5);
    RoundFill(dc, r, Sc(Console() ? 4 : 10), fill);
    COLORREF tc = primary ? p.onAcc : p.text;
    if (!enabled) tc = Mix(tc, fill, 0.5);
    ui::DrawText(dc, label, r, f.caption, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    if (enabled) AddClick(r, id);
    return w;
}

// Left-anchored small button for the detail pane's action strip; `on` marks the current start type.
int ServicesModule::ButtonL(HDC dc, int left, int y, const std::wstring& label, int id, bool on, bool enabled) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int h = Sc(Console() ? 22 : 26);
    int w = TextW(dc, label, f.caption) + Sc(20);
    RECT r{left, y, left + w, y + h};
    bool hot = enabled && hoverId_ == id;
    COLORREF fill = on ? Mix(p.panel, p.acc, 0.35) : (hot ? p.chipHot : p.chip);
    if (!enabled && !on) fill = Mix(fill, p.bg, 0.5);
    RoundFill(dc, r, Sc(Console() ? 4 : 8), fill, on ? p.acc : kNoColor);
    COLORREF tc = p.text;
    if (!enabled && !on) tc = Mix(tc, fill, 0.55);
    ui::DrawText(dc, label, r, f.caption, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    if (enabled && !on) AddClick(r, id);
    return w;
}

std::vector<Banner> ServicesModule::Banners() const {
    std::vector<Banner> v;
    if (!kbOk_) v.push_back({L"The embedded knowledge base could not be read (" + kbErr_ + L"). Presets are off; every item shows as unknown.", 2, {}});
    if (fixture_ && !fixtureDir_.empty())
        v.push_back({L"Fixture data from " + sys::ToShell(fixtureDir_) + L": no live services are read or changed.", 0, {}});
    if (!fixture_ && !elevated_)
        v.push_back({L"Hadron is not running as administrator (the manifest was ignored), so this page is read-only: "
                     L"nothing is written, not even the first-run snapshot.", 1, {}});
    else if (!fixture_ && readOnly_)
        v.push_back({L"Read-only mode (--readonly): nothing is written and nothing can be applied.", 0, {}});
    if (!storeNote_.empty()) v.push_back({storeNote_, 1, {}});
    if (applyW_.Running()) {
        uint64_t k = applyW_.progress.load();
        std::wstring t = (resultControl_ ? L"Running " : L"Applying ") + resultTitle_ + L": item " +
                         std::to_wstring(std::min<uint64_t>(k + 1, job_.total)) + L" of " + std::to_wstring(job_.total) +
                         (cancelApply_.load() ? L". Cancelling after the current item…" : L". Each change is logged and flushed before it is made.");
        v.push_back({t, 0, {{ID_CANCEL_APPLY, L"Cancel"}}});
    }
    if (interrupted_ >= 0 && interrupted_ < (int)logs_.size()) {
        const UndoLog& lg = logs_[interrupted_];
        int done = 0, begun = 0;
        for (auto& r : lg.rows) { if (r.hasD) done++; if (r.hasI) begun++; }
        std::wstring t = L"An apply did not finish (" + lg.stamp + L", " + Unesc(lg.source) + L"): " + std::to_wstring(done) +
                         L" of " + std::to_wstring(lg.count) + L" changes were recorded as done" +
                         (begun > done ? L" and 1 was in progress" : L"") +
                         L". Undo puts back what it changed; Resume re-plans the rest against the live state.";
        bool can = !fixture_ && elevated_ && !readOnly_ && store_.Valid();
        if (can || fixture_) v.push_back({t, 1, {{ID_INT_UNDO, L"Undo"}, {ID_INT_RESUME, L"Resume"}}});
        else v.push_back({t, 1, {}});
    }
    if (tiLeft_)
        v.push_back({L"Hadron started TrustedInstaller during an apply that did not finish, and it is still running. "
                     L"Windows stops it on its own when idle; you can also stop it now.", 1, {{ID_TI_STOP, L"Stop it"}}});
    if (haveResult_ && !applyW_.Running()) {
        const ApplyResult& r = result_;
        std::wstring t;
        int tone = 0;
        if (r.nothing) t = L"Nothing to change.";
        else if (r.aborted) {
            tone = 2;
            t = resultTitle_ + L" stopped: " + r.error;
        } else if (resultControl_) {
            tone = r.failed ? 2 : 0;
            t = resultTitle_ + L": " + Join(r.lines, L"; ") + L".";
        } else {
            tone = r.failed ? 1 : 0;
            t = resultTitle_ + L": " + std::to_wstring(r.ok) + L" changed, " + std::to_wstring(r.skipped) + L" skipped, " +
                std::to_wstring(r.failed) + L" failed" + (r.cancelled ? L" (cancelled)" : L"") +
                L". Start-type changes take effect after a restart; Hadron never restarts the PC.";
            if (!r.zipPath.empty()) t += L" Offline recovery: " + sys::ToShell(r.zipPath) + L".";
            for (auto& l : r.lines) if (l.rfind(L"fail", 0) == 0 || l.rfind(L"skip", 0) == 0) t += L" " + l + L".";
            if (!r.tiNote.empty()) t += L" " + r.tiNote;
        }
        std::vector<std::pair<int, std::wstring>> acts;
        if (!resultControl_ && r.ok > 0 && undoable_ >= 0) acts.push_back({ID_RESULT_UNDO, L"Undo"});
        acts.push_back({ID_DISMISS, L"Dismiss"});
        v.push_back({t, tone, acts});
    }
    if (!drift_.empty() && mode_ == Mode::List) {
        std::vector<std::wstring> n;
        for (auto& s : items_) if (drift_.count(Lower(s.name))) n.push_back(s.name);
        if (n.size() > 6) { n.resize(6); n.push_back(L"…"); }
        v.push_back({std::to_wstring(drift_.size()) + L" item(s) changed since Hadron set them (marked * in Start type): " +
                         Join(n, L", ") + L". Windows updates and other tools do this; nothing is changed back on its own.",
                     0, {}});
    }
    for (auto& n : notices_) v.push_back({n, 0, {{ID_DISMISS, L"Dismiss"}}});
    return v;
}

int ServicesModule::PaintBanners(HDC dc, const RECT& rc, int y0) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    int pad = Sc(con ? 10 : 20);
    int y = y0;
    for (auto& b : Banners()) {
        COLORREF tone = b.tone == 2 ? p.bad : (b.tone == 1 ? p.warn : p.acc);
        int right = rc.right - pad;
        int actionsW = 0;
        for (auto& a : b.actions) actionsW += TextW(dc, a.second, f.caption) + Sc(26) + Sc(8);
        RECT tr{pad + Sc(14), 0, right - Sc(10) - actionsW, 0};
        RECT m = tr;
        m.bottom = Sc(2000);
        SelectObject(dc, f.caption);
        DrawTextW(dc, b.text.c_str(), -1, &m, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        int textH = std::min<int>(m.bottom - m.top, Sc(16) * 4);
        int h = std::max(Sc(con ? 24 : 30), textH + Sc(con ? 8 : 12));
        RECT r{pad, y, right, y + h};
        RoundFill(dc, r, Sc(con ? 3 : 8), Mix(p.panel, tone, 0.16));
        RECT bar{r.left, r.top, r.left + Sc(4), r.bottom};
        FillSolid(dc, bar, tone);
        tr.top = y + (h - textH) / 2;
        tr.bottom = tr.top + textH;
        ui::DrawText(dc, b.text, tr, f.caption, p.text, DT_WORDBREAK | DT_END_ELLIPSIS);
        int ax = right - Sc(8);
        for (auto it = b.actions.rbegin(); it != b.actions.rend(); ++it) {
            int bh = Sc(con ? 20 : 24);
            int bw = TextW(dc, it->second, f.caption) + Sc(26);
            RECT ar{ax - bw, y + (h - bh) / 2, ax, y + (h - bh) / 2 + bh};
            bool hot = hoverId_ == it->first;
            bool primary = it->first != ID_DISMISS;
            COLORREF fill = primary ? (hot ? Mix(p.acc, RGB(255, 255, 255), 0.18) : p.acc) : (hot ? p.chipHot : p.chip);
            RoundFill(dc, ar, Sc(6), fill);
            ui::DrawText(dc, it->second, ar, f.caption, primary ? p.onAcc : p.text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            AddClick(ar, it->first);
            ax -= bw + Sc(8);
        }
        y += h + Sc(6);
    }
    return y == y0 ? y : y + Sc(2);
}

namespace {
// Word wrap that never overflows: breaks at spaces, and inside a word wider than the line (a URL) after
// '/', '-', '.', '?' or '&', else at the last character that fits. DrawText's DT_WORDBREAK can leave such a
// word running past the right edge, where the pane's clip cuts it off.
std::vector<std::wstring> WrapLines(HDC dc, HFONT font, const std::wstring& text, int width, int* lineH) {
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    *lineH = tm.tmHeight;
    auto fits = [&](size_t from, size_t n) -> size_t {   // how many chars of text[from, from+n) fit in width
        // A little slack: some GDI implementations draw antialiased glyphs a few pixels wider than they measure.
        int fit = 0;
        SIZE sz{};
        if (!GetTextExtentExPointW(dc, text.c_str() + from, (int)n, std::max(1, width - width / 40 - 2), &fit, nullptr, &sz)) return n;
        return (size_t)std::max(0, fit);
    };
    std::vector<std::wstring> out;
    size_t pos = 0;
    const size_t n = text.size();
    while (pos < n) {
        size_t nl = text.find(L'\n', pos);
        size_t end = nl == std::wstring::npos ? n : nl;
        if (pos == end) out.push_back(L"");
        while (pos < end) {
            size_t f = fits(pos, end - pos);
            if (f >= end - pos) { out.push_back(text.substr(pos, end - pos)); pos = end; break; }
            size_t cut = 0;
            for (size_t i = pos + f; i > pos; i--) if (text[i] == L' ') { cut = i; break; }
            if (cut > pos) {
                out.push_back(text.substr(pos, cut - pos));
                pos = cut;
                while (pos < end && text[pos] == L' ') pos++;
                continue;
            }
            size_t brk = 0;
            for (size_t i = pos + f; i > pos + 1; i--)
                if (wcschr(L"/-.?&", text[i - 1])) { brk = i; break; }
            if (!brk) brk = pos + std::max<size_t>(1, f);
            out.push_back(text.substr(pos, brk - pos));
            pos = brk;
        }
        pos = end + 1;
    }
    SelectObject(dc, old);
    return out;
}
}  // namespace

// Measures (draw=false) or draws the wrapped document inside r, scrolled by offset. Returns its height.
int ServicesModule::PaintDoc(HDC dc, const RECT& r, const std::vector<Para>& doc, int offset, bool draw) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int y = r.top - offset;
    int W = r.right - r.left;
    int labelW = std::min(Sc(120), W / 3);
    auto measure = [&](const std::wstring& t, HFONT font, int width) {
        if (t.empty()) return 0;
        int lh = 0;
        return (int)WrapLines(dc, font, t, width, &lh).size() * lh;
    };
    auto draw1 = [&](const std::wstring& t, HFONT font, COLORREF col, int x0, int width, int h) {
        if (!draw || t.empty() || y + h < r.top || y > r.bottom) return;
        int lh = 0;
        std::vector<std::wstring> lines = WrapLines(dc, font, t, width, &lh);
        for (size_t i = 0; i < lines.size(); i++) {
            RECT tr{x0, y + (int)i * lh, x0 + width, y + (int)(i + 1) * lh};
            ui::DrawText(dc, lines[i], tr, font, col, DT_SINGLELINE | DT_NOCLIP);
        }
    };
    for (auto& para : doc) {
        COLORREF col = para.color == kNoColor ? p.text : para.color;
        switch (para.kind) {
            case P_TITLE: {
                int h = measure(para.text, f.semi, W);
                draw1(para.text, f.semi, col, r.left, W, h);
                y += h + Sc(2);
                break;
            }
            case P_SUB: {
                int h = measure(para.text, f.caption, W);
                draw1(para.text, f.caption, para.color == kNoColor ? p.mute : col, r.left, W, h);
                y += h + Sc(6);
                break;
            }
            case P_HEAD: {
                y += Sc(8);
                int h = measure(para.text, f.semi, W);
                draw1(para.text, f.semi, para.color == kNoColor ? p.acc : col, r.left, W, h);
                y += h + Sc(3);
                break;
            }
            case P_TEXT: {
                int h = measure(para.text, f.caption, W);
                draw1(para.text, f.caption, col, r.left, W, h);
                y += h + Sc(4);
                break;
            }
            case P_KV: {
                int hl = measure(para.label, f.caption, labelW - Sc(8));
                int hv = measure(para.text, f.caption, W - labelW);
                int h = std::max(hl, hv);
                draw1(para.label, f.caption, p.mute, r.left, labelW - Sc(8), hl);
                draw1(para.text, f.caption, col, r.left + labelW, W - labelW, hv);
                y += h + Sc(3);
                break;
            }
            case P_GAP: y += Sc(6); break;
        }
    }
    return y + offset - r.top;
}

void ServicesModule::PaintDetailPanel(HDC dc, const RECT& r, const std::vector<Para>& doc) {
    PaintCtx c = PCtx();
    int sbW = Sc(10);
    RECT body{r.left + Sc(14), r.top + Sc(10), r.right - Sc(14) - sbW, r.bottom - Sc(10)};
    rcDetail_ = r;
    int total = PaintDoc(dc, body, doc, 0, false);
    detSb_.Configure(body.bottom - body.top, total);
    rcDetSb_ = RECT{r.right - sbW - Sc(2), r.top + Sc(4), r.right - Sc(2), r.bottom - Sc(4)};
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
    PaintDoc(dc, body, doc, detSb_.Offset(), true);
    RestoreDC(dc, saved);
    if (detSb_.Visible()) detSb_.Paint(dc, rcDetSb_, c);
    if (focus_ == Focus::Detail && GetFocus() == hwnd_) FocusRectC(dc, r, Pal(host_->Theme()).acc);
}

std::vector<Para> ServicesModule::ItemDoc(const SvcItem& s) const {
    Pal p(host_->Theme());
    std::vector<Para> d;
    auto kv = [&](const std::wstring& k, const std::wstring& v, COLORREF col = kNoColor) {
        if (!v.empty()) d.push_back({P_KV, k, v, col});
    };
    d.push_back({P_TITLE, L"", s.display.empty() ? s.name : s.display, kNoColor});
    d.push_back({P_SUB, L"", s.name + L"   \u2022   " + TypeName(s.type) + L"   \u2022   " + StateName(s.state), kNoColor});
    Guard g = CheckGuard(s, ctx_);
    if (g.neverTouch) d.push_back({P_TEXT, L"", L"Protected: Hadron never changes this. " + g.reason + L".", p.bad});
    else if (g.neverPreset) d.push_back({P_TEXT, L"", L"Not in any preset: " + g.reason + L". A single change is still possible where Windows allows it.", p.warn});
    if (s.kind == Kind::UserTemplate)
        d.push_back({P_TEXT, L"", L"Per-user service template: Windows creates one instance per signed-in user from it. "
                                  L"Hadron changes only the template; instances follow at the next sign-in.", p.mute});
    if (drift_.count(Lower(s.name))) {
        auto it = lastApplied_.find(Lower(s.name));
        d.push_back({P_TEXT, L"", L"Changed since Hadron set it: Hadron set " + StartLabel(it->second, false) + L", now " +
                                      StartLabel(s.cfg, false) + L".", p.warn});
    }
    if (s.regStart >= 0 && s.regStart != s.cfg.start)
        d.push_back({P_TEXT, L"", L"The registry Start value (" + StartName(s.regStart) +
                                      L") differs from what the service manager reports: a change is pending a restart.", p.warn});
    if (!s.readError.empty()) d.push_back({P_TEXT, L"", L"Could not read everything: " + s.readError, p.warn});

    const KbEntry* e = kbOk_ ? kb_.Find(s.name) : nullptr;
    if (e) {
        bool sameTitle = e->title.empty() || Lower(e->title) == Lower(s.display.empty() ? s.name : s.display);
        d.push_back({P_HEAD, L"", sameTitle ? std::wstring(L"What it does (Hadron's KB)") : e->title + L" (Hadron's KB)", kNoColor});
        if (!e->expl.empty()) d.push_back({P_TEXT, L"", e->expl, kNoColor});
        kv(L"If it's off", e->ifOff);
        kv(L"How to test", e->test);
        kv(L"Basis", BasisLabel(e->basis) + (e->basisNote.empty() ? L"" : L": " + e->basisNote), p.mute);
        bool any = false;
        for (int lv = 0; lv < LV_COUNT; lv++) {
            if (e->lv[lv] < 0) continue;
            if (!any) d.push_back({P_HEAD, L"", L"Presets", kNoColor});
            any = true;
            kv(LevelName(lv), StartName(e->lv[lv]) + (e->why[lv].empty() ? L"" : L": " + e->why[lv]));
        }
        if (any) {
            kv(L"Preset basis", BasisLabel(e->pbasis) + (e->pbasisNote.empty() ? L"" : L": " + e->pbasisNote), p.mute);
            if (!e->contested.empty()) kv(L"Contested", e->contested, p.warn);
            if (!e->hw.empty()) {
                std::wstring ev;
                int h = hw_.ok ? HwEvidence(hw_, e->hw, s.name, &ev) : 0;
                if (!hw_.ok) ev = L"no hardware information (" + (hw_.error.empty() ? std::wstring(L"not read") : hw_.error) + L"): kept";
                const wchar_t* verdict = h == 0 ? L"kept by every preset: " : h == 1 ? L"never seen on this PC: " : h == 2 ? L"never seen, hot-plug (offered unticked): " : L"";
                kv(L"Hardware", verdict + ev);
            }
            if (e->never) kv(L"Note", L"the KB keeps it out of every preset");
        } else if (e->never) {
            d.push_back({P_TEXT, L"", L"Hadron's KB keeps this out of every preset.", p.mute});
        }
        if (!e->ForBuild(build_))
            d.push_back({P_TEXT, L"", L"This KB entry is for build " + Join(e->builds, L", ") + L"; this PC is build " + build_ + L".", p.warn});
    } else {
        d.push_back({P_HEAD, L"", L"Not in Hadron's KB", kNoColor});
        if (!g.neverTouch && !g.neverPreset)
            d.push_back({P_TEXT, L"", L"Third-party or unknown to Hadron: never in a preset. The company below is what the file says "
                                      L"about itself (its version resource), not a verified signature.", p.mute});
        else
            d.push_back({P_TEXT, L"", L"No KB text for it; the protection above is why Hadron leaves it alone.", p.mute});
    }
    d.push_back({P_HEAD, L"", L"Configuration", kNoColor});
    kv(L"Start type", StartLabel(s.cfg, s.trigger) + (s.cfg.delayed >= 0 && s.cfg.start != 2 ? L" (DelayedAutostart " + std::to_wstring(s.cfg.delayed) + L")" : L""));
    if (hasFirst_) {
        const SvcItem* f = first_.Find(s.name);
        auto cr = first_.meta.find(L"created");
        kv(L"First snapshot", f ? StartLabel(f->cfg, false) + (cr != first_.meta.end() ? L" (" + cr->second + L")" : L"")
                                : L"not present when Hadron first ran");
    }
    if (e && e->stock >= 0)
        kv(L"Windows stock", StartLabel(StartCfg{e->stock, e->stockDelayed}, false) + L" (" + BasisLabel(e->stockBasis) + L")");
    else if (!IsStorageDriver(s.name))
        kv(L"Windows stock", L"no value with a measured or published basis in Hadron's KB yet", p.mute);
    if (s.trigger) kv(L"Triggers", s.triggers.empty() ? L"yes" : s.triggers);
    kv(L"Company (file claim)", s.company);
    std::wstring windir;
    {
        wchar_t buf[MAX_PATH];
        UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
        windir = (n && n < MAX_PATH) ? std::wstring(buf, n) : L"C:\\Windows";
    }
    ImageInfo im = NormalizeImage(s.image, windir, s.name, IsDriverType(s.type));
    kv(L"Image", im.path + (im.args.empty() ? L"" : L"  " + im.args));
    if (!im.svchostGroup.empty()) kv(L"svchost group", im.svchostGroup);
    if (im.unquotedSpaces) kv(L"Path warning", L"unquoted path with spaces", p.warn);
    if (!IsDriverType(s.type)) kv(L"Account", s.account);
    kv(L"Load group", s.group);
    kv(L"Error control", ErrCtlName(s.errctl));
    {
        std::wstring prot;
        if (s.protect)
            prot = L"launch-protected (" + ProtectName(s.protect) +
                   L"): only the protected service itself can change it; the TrustedInstaller toggle does not help";
        else {
            int wa = WriteAccess(s.name);
            if (wa == 1) prot = L"Windows refuses this write to administrators (service security descriptor); the TrustedInstaller toggle can help";
            else if (wa == 0) prot = g.neverTouch ? L"Windows lets administrators change it; Hadron does not (see above)" : L"administrators can change the start type";
        }
        static const wchar_t* const kDefender[] = {L"WinDefend", L"WdNisSvc", L"WdNisDrv", L"WdFilter", L"WdBoot", L"Sense",
                                                   L"SecurityHealthService", L"wscsvc"};
        for (auto* n : kDefender)
            if (IEq(s.name, n)) prot += (prot.empty() ? L"" : L"; ") + std::wstring(L"Tamper Protection blocks changes to Defender");
        kv(L"Protection", prot, s.protect ? p.warn : kNoColor);
    }
    kv(L"Depends on", Join(s.deps, L", "));
    std::vector<std::wstring> dependents;
    for (auto& o : items_)
        for (auto& dd : o.deps) if (IEq(dd, s.name)) dependents.push_back(o.name);
    kv(L"Needed by", Join(dependents, L", "));
    kv(L"Failure actions", s.failure);
    kv(L"Instances", Join(s.instances, L", "));
    if (!s.desc.empty()) {
        d.push_back({P_HEAD, L"", L"From the service's registration (not Hadron)", kNoColor});
        d.push_back({P_TEXT, L"", s.desc, p.mute});
    }
    return d;
}

std::vector<Para> ServicesModule::PlanRowDoc(const PlanRow& r) const {
    Pal p(host_->Theme());
    std::vector<Para> d;
    auto kv = [&](const std::wstring& k, const std::wstring& v, COLORREF col = kNoColor) {
        if (!v.empty()) d.push_back({P_KV, k, v, col});
    };
    d.push_back({P_TITLE, L"", r.display.empty() ? r.name : r.display, kNoColor});
    d.push_back({P_SUB, L"", r.name, kNoColor});
    if (plan_.kind == PlanKind::Control) kv(L"Operation", r.ctl == Op::Stop ? L"stop" : L"start");
    else {
        kv(L"Now", StartLabel(r.before, false));
        kv(L"Planned", StartLabel(r.after, false));
    }
    if (!r.blocked.empty()) d.push_back({P_TEXT, L"", r.blocked, p.bad});
    kv(L"Why", r.reason);
    if (!r.basis.empty()) kv(L"Basis", BasisLabel(r.basis) + (r.basisNote.empty() ? L"" : L": " + r.basisNote), p.mute);
    kv(L"Contested", r.contested, p.warn);
    kv(L"Hardware", r.evidence);
    kv(L"Warning", r.warning, p.warn);
    if (r.needsTi) kv(L"Identity", L"needs TrustedInstaller: Windows refuses this write to administrators", p.warn);
    const KbEntry* e = kbOk_ ? kb_.Find(r.name) : nullptr;
    if (e) {
        bool sameTitle = e->title.empty() || Lower(e->title) == Lower(r.display.empty() ? r.name : r.display);
        d.push_back({P_HEAD, L"", sameTitle ? std::wstring(L"What it does (Hadron's KB)") : e->title + L" (Hadron's KB)", kNoColor});
        if (!e->expl.empty()) d.push_back({P_TEXT, L"", e->expl, kNoColor});
        kv(L"If it's off", e->ifOff);
        kv(L"How to test", e->test);
    }
    return d;
}

bool ServicesModule::SetListColumns(int widthPx) {
    UINT dpi = host_->Dpi();
    int W = MulDiv(widthPx, 96, (int)dpi) - 12;   // 96-dpi units, less the scrollbar
    struct C { const wchar_t* key; const wchar_t* title; int w; int drop; };
    // w 0 = the flexible column (at least kFlexMin). drop 0 = always shown; otherwise columns are dropped in
    // drop order (1 first) until the rest fit, so a narrow list (the suite, a small window) never overflows.
    static const C kSvc[] = {{L"name", L"Name", 156, 0}, {L"display", L"Display name", 0, 0}, {L"state", L"State", 70, 0},
                             {L"start", L"Start type", 120, 0}, {L"changed", L"vs first", 74, 5}, {L"kb", L"Hadron", 80, 0},
                             {L"basis", L"Basis", 80, 4}, {L"type", L"Type", 104, 3}, {L"account", L"Account", 130, 2},
                             {L"company", L"Company", 140, 1}};
    static const C kDrv[] = {{L"name", L"Name", 130, 0}, {L"display", L"Display name", 0, 0}, {L"state", L"State", 70, 0},
                             {L"start", L"Start type", 96, 0}, {L"changed", L"vs first", 74, 5}, {L"kb", L"Hadron", 80, 0},
                             {L"basis", L"Basis", 80, 4}, {L"type", L"Type", 100, 3}, {L"group", L"Load group", 130, 2},
                             {L"company", L"Company", 140, 1}};
    const int kFlexMin = 110;
    const C* all = tab_ == TAB_SERVICES ? kSvc : kDrv;
    const size_t n = tab_ == TAB_SERVICES ? sizeof(kSvc) / sizeof(kSvc[0]) : sizeof(kDrv) / sizeof(kDrv[0]);
    std::vector<bool> on(n, true);
    int fixed = 0;
    for (size_t i = 0; i < n; i++) fixed += all[i].w;
    for (int rank = 1; rank <= 5 && fixed + kFlexMin > W; rank++)
        for (size_t i = 0; i < n; i++)
            if (all[i].drop == rank && on[i]) { on[i] = false; fixed -= all[i].w; }
    std::vector<ListView::Column> cols;
    std::vector<std::wstring> keys;
    for (size_t i = 0; i < n; i++) {
        if (!on[i]) continue;
        const C& c = all[i];
        int w = c.w ? c.w : std::max(kFlexMin, W - fixed);
        cols.push_back({c.title, w});
        keys.push_back(c.key);
    }
    list_.SetColumns(std::move(cols));
    if (keys == colKeys_) return false;
    colKeys_ = std::move(keys);
    return true;
}

void ServicesModule::SetPlanColumns(int widthPx) {
    int W = MulDiv(widthPx, 96, (int)host_->Dpi()) - 12;
    bool ctl = plan_.kind == PlanKind::Control;
    int name = 150, cur = ctl ? 70 : 120, tgt = ctl ? 0 : 120, box = 52;
    plist_.SetColumns({{L"", box}, {L"Name", name}, {ctl ? L"Op" : L"Now", cur}, {ctl ? L"" : L"Planned", tgt},
                       {L"Why / protection", std::max(120, W - box - name - cur - tgt)}});
}

// ============================================================================================
// Paint

void ServicesModule::Paint(HDC dc, const RECT& rc) {
    if (!host_) return;
    clicks_.clear();
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    FillSolid(dc, rc, p.bg);
    int pad = Sc(con ? 10 : 20);

    // Header: title, source line, Refresh and Undo last apply.
    int right = rc.right - pad;
    int by = Sc(con ? 6 : 18);
    bool busy = applyW_.Running() || loadW_.Running();
    std::wstring dummy;
    bool canApply = CanApply(&dummy);
    right -= Button(dc, right, by, L"Undo last apply", ID_UNDO_LAST, false, undoable_ >= 0 && canApply && mode_ == Mode::List) + Sc(10);
    right -= Button(dc, right, by, L"Refresh", ID_REFRESH, false, !fixture_ && !busy && mode_ == Mode::List) + Sc(10);
    int nSvc = 0, nDrv = 0;
    for (auto& s : items_) (IsDriverType(s.type) ? nDrv : nSvc)++;
    std::wstring sub;
    if (!loaded_) sub = L"Reading services and drivers…";
    else {
        sub = (fixture_ ? L"Fixture" : L"This PC") + std::wstring(L"   \u2022   build ") + (build_.empty() ? L"?" : build_) +
              L"   \u2022   " + std::to_wstring(nSvc) + L" services, " + std::to_wstring(nDrv) + L" drivers";
        if (!fixture_ && store_.Valid()) sub += L"   \u2022   data: " + sys::ToShell(store_.Root());
        if (kbOk_) sub += L"   \u2022   KB " + std::to_wstring(kb_.entries.size()) + L" entries (build " + kb_.build + L")";
    }
    if (con) {
        RECT tr{pad, by, right - Sc(10), by + Sc(24)};
        int tw = TextW(dc, kAppTitle, f.semi);
        TextLeft(dc, kAppTitle, tr, f.semi, p.text);
        tr.left += tw + Sc(14);
        TextLeft(dc, sub, tr, f.caption, p.mute);
    } else {
        RECT tr{pad, Sc(12), right - Sc(10), Sc(44)};
        TextLeft(dc, kAppTitle, tr, f.title, p.text);
        RECT sr{pad, Sc(44), right - Sc(10), Sc(66)};
        TextLeft(dc, sub, sr, f.caption, p.mute);
    }
    int y = PaintBanners(dc, rc, Sc(con ? 36 : 76));
    if (mode_ == Mode::Plan) PaintPlanMode(dc, rc, y);
    else PaintListMode(dc, rc, y);
}

void ServicesModule::PaintListMode(HDC dc, const RECT& rc, int y) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = PCtx();
    const bool con = Console();
    int pad = Sc(con ? 10 : 20);
    int rowH = Sc(con ? 22 : 26);

    // Row 1: tabs on the left, the TrustedInstaller switch on the right.
    int nSvc = 0, nDrv = 0;
    for (auto& s : items_) (IsDriverType(s.type) ? nDrv : nSvc)++;
    int x = pad;
    Chip(dc, x, y, L"Services (" + std::to_wstring(nSvc) + L")", ID_TAB_SVC, tab_ == TAB_SERVICES);
    Chip(dc, x, y, L"Drivers (" + std::to_wstring(nDrv) + L")", ID_TAB_DRV, tab_ == TAB_DRIVERS);
    {
        std::wstring lbl = tiOn_ ? L"TrustedInstaller: on" : L"TrustedInstaller: off";
        int w = TextW(dc, lbl, f.caption) + Sc(con ? 16 : 22);
        int tx = rc.right - pad - w;
        int tx0 = tx;
        Chip(dc, tx, y, lbl, ID_TI, tiOn_);
        // The hint is drawn only where it fits whole: a clipped hint about what TI can't do would mislead.
        RECT cr{x + Sc(12), y, tx0 - Sc(10), y + rowH};
        static const wchar_t* const kCaps[] = {
            L"Only for writes Windows refuses to administrators. Not Defender, PPL or Tamper Protection.",
            L"Only for writes Windows refuses to administrators.", L"For admin-refused writes."};
        for (const wchar_t* cap : kCaps) {
            int need = TextW(dc, cap, f.caption) * 105 / 100 + Sc(4);
            if (need <= cr.right - cr.left) {   // left-aligned at right - need: a wider render runs into the gap
                RECT tr{cr.right - need, cr.top, cr.right + Sc(8), cr.bottom};
                ui::DrawText(dc, cap, tr, f.caption, p.mute, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOCLIP);
                break;
            }
        }
    }
    y += rowH + Sc(8);

    // Row 2: presets, then the filter box.
    x = pad;
    RECT lr{x, y, x + Sc(60), y + rowH};
    TextLeft(dc, L"Presets", lr, f.caption, p.mute);
    x += Sc(60);
    bool canPlan = loaded_ && !applyW_.Running() && !items_.empty();
    Chip(dc, x, y, L"Default: first snapshot", ID_PRESET + 0, false, canPlan);
    Chip(dc, x, y, L"Default: Windows stock", ID_PRESET + 1, false, canPlan);
    Chip(dc, x, y, L"Pure bloat removal", ID_PRESET + 2, false, canPlan && kbOk_);
    Chip(dc, x, y, L"Stripped", ID_PRESET + 3, false, canPlan && kbOk_);
    Chip(dc, x, y, L"Minimum", ID_PRESET + 4, false, canPlan && kbOk_);
    int sw = std::min(Sc(320), (int)rc.right - pad - x - Sc(8));
    rcSearch_ = RECT{rc.right - pad - std::max(sw, Sc(140)), y, rc.right - pad, y + rowH};
    search_.Paint(dc, rcSearch_, c, focus_ == Focus::Search && GetFocus() == hwnd_);
    AddClick(rcSearch_, ID_SEARCH);
    y += rowH + Sc(10);

    // List (left) and detail (right).
    int bottom = rc.bottom - pad;
    int total = rc.right - 2 * pad;
    int listW = total * 60 / 100;
    rcList_ = RECT{pad, y, pad + listW, bottom};
    RECT det{pad + listW + Sc(12), y, rc.right - pad, bottom};
    if (SetListColumns(listW)) Refilter();
    if (!loaded_ || !loadErr_.empty() || items_.empty()) {
        FillSolid(dc, rcList_, p.panel);
        FrameRectC(dc, rcList_, p.border);
        std::wstring t = !loadErr_.empty() ? loadErr_ : !loaded_ ? L"Reading services and drivers…" : L"No data.";
        RECT tr = rcList_;
        InflateRect(&tr, -Sc(20), -Sc(20));
        ui::DrawText(dc, t, tr, f.ui, loadErr_.empty() ? p.mute : p.bad, DT_WORDBREAK | DT_CENTER);
    } else {
        // Column texts depend on the columns that fit: rebuild once the widths are known.
        list_.Paint(dc, rcList_, c, focus_ == Focus::List && GetFocus() == hwnd_);
        if (V_.empty()) {
            RECT tr{rcList_.left, rcList_.top + Sc(60), rcList_.right, rcList_.top + Sc(90)};
            ui::DrawText(dc, L"Nothing matches the filter.", tr, f.ui, p.mute, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
    }

    // Detail panel: actions strip (not scrolled) + the wrapped document.
    RoundFill(dc, det, Sc(con ? 3 : 8), p.panel, p.border);
    const SvcItem* s = Selected();
    if (!s) {
        rcDetail_ = det;
        RECT tr = det;
        InflateRect(&tr, -Sc(16), -Sc(16));
        ui::DrawText(dc, L"Select an item to see what it does, why a preset would change it, and its configuration.", tr,
                     f.caption, p.mute, DT_WORDBREAK);
        return;
    }
    std::wstring why;
    bool can = CanApply(&why);
    Guard g = CheckGuard(*s, ctx_);
    bool drv = IsDriverType(s->type);
    bool writable = !g.neverTouch && s->kind != Kind::UserInstance && s->cfg.start >= 2;
    int ax = det.left + Sc(14), ay = det.top + Sc(12);
    RECT capR{ax, ay, ax + Sc(70), ay + Sc(26)};
    TextLeft(dc, L"Start type", capR, f.caption, p.mute);
    ax += Sc(74);
    bool en = can && writable;
    ax += ButtonL(dc, ax, ay, L"Auto", ID_SET + 2, s->cfg.start == 2 && !s->cfg.Delayed(), en) + Sc(6);
    if (!drv) ax += ButtonL(dc, ax, ay, L"Auto (delayed)", ID_SET + 5, s->cfg.Delayed(), en) + Sc(6);
    ax += ButtonL(dc, ax, ay, L"Manual", ID_SET + 3, s->cfg.start == 3, en) + Sc(6);
    ax += ButtonL(dc, ax, ay, L"Disabled", ID_SET + 4, s->cfg.start == 4, en) + Sc(6);
    int stripH = Sc(con ? 30 : 36);
    if (!drv) {
        int ay2 = ay + Sc(32);
        int bx = det.left + Sc(14);
        RECT cap2{bx, ay2, bx + Sc(70), ay2 + Sc(26)};
        TextLeft(dc, L"Now", cap2, f.caption, p.mute);
        bx += Sc(74);
        bool running = s->state == SERVICE_RUNNING;
        bool stoppable = running && (s->controls & SERVICE_ACCEPT_STOP);
        bool cen = can && s->kind != Kind::UserTemplate;
        bx += ButtonL(dc, bx, ay2, L"Start", ID_CTL + 0, false, cen && !running && s->cfg.start != 4) + Sc(6);
        bx += ButtonL(dc, bx, ay2, L"Stop", ID_CTL + 1, false, cen && stoppable) + Sc(6);
        bx += ButtonL(dc, bx, ay2, L"Restart", ID_CTL + 2, false, cen && stoppable) + Sc(6);
        stripH += Sc(32);
    }
    int ny = det.top + Sc(12) + stripH - Sc(4);
    if (!can) {
        RECT nr{det.left + Sc(14), ny, det.right - Sc(14), ny + Sc(18)};
        TextLeft(dc, why, nr, f.caption, p.faint);
        ny += Sc(20);
    }
    HLine(dc, det.left + Sc(10), det.right - Sc(10), ny + Sc(2), p.border);
    RECT body{det.left, ny + Sc(4), det.right, det.bottom};
    PaintDetailPanel(dc, body, ItemDoc(*s));
}

void ServicesModule::PaintPlanMode(HDC dc, const RECT& rc, int y) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = PCtx();
    const bool con = Console();
    int pad = Sc(con ? 10 : 20);

    // Title line with Apply / Cancel on the right.
    std::wstring why;
    bool can = CanApply(&why);
    bool ctl = plan_.kind == PlanKind::Control;
    int n = ctl ? (int)std::count_if(plan_.rows.begin(), plan_.rows.end(), [](const PlanRow& r) { return r.blocked.empty(); })
                : plan_.Included();
    int right = rc.right - pad;
    std::wstring applyLbl = ctl ? L"Run" : (n == 1 ? L"Apply 1 change" : L"Apply " + std::to_wstring(n) + L" changes");
    right -= Button(dc, right, y, applyLbl, ID_APPLY, true, can && n > 0) + Sc(10);
    right -= Button(dc, right, y, L"Cancel", ID_CANCEL_PLAN, false, !applyW_.Running()) + Sc(10);
    RECT tr{pad, y, right - Sc(10), y + Sc(30)};
    TextLeft(dc, L"Plan: " + plan_.title, tr, f.semi, p.text);
    y += Sc(34);
    std::wstring sum;
    if (ctl) sum = std::to_wstring(n) + L" operation(s), in this order. Nothing happens until you press Run.";
    else
        sum = std::to_wstring(plan_.Included()) + L" ticked, " + std::to_wstring(plan_.Optional()) + L" unticked, " +
              std::to_wstring(plan_.Blocked()) + L" kept or protected; " + std::to_wstring(plan_.alreadySet) +
              L" already set. Nothing changes until you press Apply. Click [x] or press Space to tick or untick.";
    if (!can) sum += L"  (" + why + L")";
    RECT sr{pad, y, rc.right - pad, y + Sc(20)};
    TextLeft(dc, sum, sr, f.caption, can ? p.mute : p.warn);
    y += Sc(22);
    for (auto& note : plan_.notes) {
        RECT m{pad + Sc(12), 0, rc.right - pad, 0};
        SelectObject(dc, f.caption);
        DrawTextW(dc, note.c_str(), -1, &m, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        int h = std::min<int>(m.bottom - m.top, Sc(32));
        RECT nr{pad + Sc(12), y, rc.right - pad, y + h};
        RECT dot{pad + Sc(2), y + Sc(6), pad + Sc(6), y + Sc(10)};
        FillSolid(dc, dot, p.faint);
        ui::DrawText(dc, note, nr, f.caption, p.mute, DT_WORDBREAK | DT_END_ELLIPSIS);
        y += h + Sc(3);
    }
    y += Sc(8);
    int bottom = rc.bottom - pad;
    int total = rc.right - 2 * pad;
    int listW = total * 60 / 100;
    rcPlanList_ = RECT{pad, y, pad + listW, bottom};
    SetPlanColumns(listW);
    plist_.Paint(dc, rcPlanList_, c, focus_ == Focus::List && GetFocus() == hwnd_);
    if (plan_.rows.empty()) {
        RECT er{rcPlanList_.left, rcPlanList_.top + Sc(60), rcPlanList_.right, rcPlanList_.top + Sc(90)};
        ui::DrawText(dc, L"Nothing to change.", er, f.ui, p.mute, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    RECT det{pad + listW + Sc(12), y, rc.right - pad, bottom};
    RoundFill(dc, det, Sc(con ? 3 : 8), p.panel, p.border);
    int sel = plist_.Selected();
    if (sel >= 0 && sel < (int)plan_.rows.size()) PaintDetailPanel(dc, det, PlanRowDoc(plan_.rows[sel]));
    else rcDetail_ = det;
}

// ============================================================================================
// CLI

int ServicesModule::RunCli(int argc, wchar_t** argv) {
    bool selftest = false;
    for (int i = 0; i < argc; i++) if (std::wstring(argv[i]) == L"--selftest") selftest = true;
    if (!selftest) return -1;
    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    std::vector<std::wstring> args = cl.rest;
    if (!cl.fixture.empty()) { args.push_back(L"--fixture"); args.push_back(cl.fixture); }
    return RunSelftest(args);
}

}  // namespace

std::unique_ptr<Module> MakeServicesModule() { return std::make_unique<ServicesModule>(); }

}  // namespace hx
