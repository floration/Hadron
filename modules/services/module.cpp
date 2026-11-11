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
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <cwctype>
#include <functional>
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
constexpr UINT WM_SVC_PLANNED = WM_MODULE_BASE + 3;   // plan worker -> view
constexpr UINT_PTR kProgressTimer = 1;
const wchar_t* const kViewClass = L"Hadron.services.View";
const wchar_t* const kAppTitle = L"Services and drivers";

enum ClickId {
    ID_NONE = 0, ID_REFRESH, ID_UNDO_LAST, ID_TAB_SVC, ID_TAB_DRV, ID_TI, ID_SEARCH,
    ID_APPLY, ID_CANCEL_PLAN, ID_CANCEL_APPLY, ID_INT_UNDO, ID_INT_RESUME, ID_RESULT_UNDO, ID_DISMISS, ID_TI_STOP,
    ID_PRESET = 100,   // + 0 first snapshot, 1 Windows stock, 2 bloat, 3 stripped, 4 minimum
    ID_SET = 200,      // + start type 2..4, + 5 for Auto (delayed)
    ID_CTL = 300,      // + 0 start, 1 stop, 2 restart
    ID_FOCUS_LIST = 900, ID_FOCUS_DETAIL,   // tab stops that are not click targets
    ID_CONKEY = 400,   // + a Console action (CA_*), the F-key bar's click targets
};
// Keyboard focus. Control = a chip or button (focusId_), reached with Tab/Shift+Tab in reading order and
// activated with Enter or Space. Apply is never a tab stop: a plan is applied by a mouse click only.
enum class Focus { List, Search, Detail, Control };
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
// WCAG 2 relative luminance and contrast ratio (docs/pipeline/svcdrv/design/baseline.md, "Contrast").
double Lin(int v) { double c = v / 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double RelLum(COLORREF c) { return 0.2126 * Lin(GetRValue(c)) + 0.7152 * Lin(GetGValue(c)) + 0.0722 * Lin(GetBValue(c)); }
double Contrast(COLORREF a, COLORREF b) {
    double la = RelLum(a), lb = RelLum(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}
// Text on a filled control: white while it reaches 4.5:1 to one decimal (4.50 on the default blue 0078D7), else
// black (then above 4.6:1). A brightness threshold picked white on light greens; near-white was 4.2:1 on 0078D7.
COLORREF OnColor(COLORREF fill) {
    const COLORREF dark = RGB(0, 0, 0), light = RGB(255, 255, 255);
    return Contrast(light, fill) >= 4.45 || Contrast(light, fill) >= Contrast(dark, fill) ? light : dark;   // white while it passes
}
// The same hue as fg, moved towards black (light backgrounds) or white (dark ones) in small steps until it
// reaches `ratio` against every background given. Used for accent and semantic colours drawn as text: the
// theme's fill colours stay as they are, the text forms get the contrast (owner's rule: accent lightness may
// be adjusted per theme for contrast; semantic colours stay fixed per theme, and so do these text forms).
COLORREF Readable(COLORREF fg, std::initializer_list<COLORREF> bgs, double ratio) {
    COLORREF darkest = *bgs.begin();
    for (COLORREF b : bgs) if (RelLum(b) < RelLum(darkest)) darkest = b;
    const COLORREF toward = RelLum(darkest) > 0.18 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    COLORREF c = fg;
    for (int i = 1; i <= 20; i++) {
        bool ok = true;
        for (COLORREF b : bgs) if (Contrast(c, b) < ratio) ok = false;
        if (ok) return c;
        c = Mix(fg, toward, i * 0.05);
    }
    return c;
}

// Palette over the theme, with the high-contrast fallback to system colours. The *T members are the text
// forms of the accent and the semantic colours: at least 4.5:1 on the panel and the alternate row/chip fill.
struct Pal {
    COLORREF bg, panel, hover, text, mute, faint, acc, onAcc, warn, bad, good, chip, chipHot, border;
    COLORREF accT, warnT, badT, goodT, ring;
    explicit Pal(const Theme& t) {
        if (t.highContrast) {
            bg = panel = GetSysColor(COLOR_WINDOW);
            hover = chip = chipHot = GetSysColor(COLOR_BTNFACE);
            text = mute = faint = GetSysColor(COLOR_WINDOWTEXT);
            acc = warn = bad = good = GetSysColor(COLOR_HOTLIGHT);
            accT = warnT = badT = goodT = acc;
            onAcc = GetSysColor(COLOR_WINDOW);
            border = ring = GetSysColor(COLOR_WINDOWTEXT);
            return;
        }
        bg = t.bg; panel = t.panel; hover = t.panel2;
        text = t.text1; mute = t.text2;
        acc = t.accent; onAcc = OnColor(t.accent);
        warn = t.warn; bad = t.bad; good = t.good;
        chip = t.panel2; chipHot = t.panel3; border = t.border;
        // text3 is below 4.5:1 on the light, boring and dark panels; it is used for real sentences here
        // ("Fixture data: applying is off."), so it gets the same treatment as the accent.
        faint = Readable(t.text3, {t.panel, t.panel2, t.bg}, 4.5);
        accT = Readable(t.accent, {t.panel, t.panel2, t.bg}, 4.5);
        warnT = Readable(t.warn, {t.panel, t.panel2, t.bg}, 4.5);
        badT = Readable(t.bad, {t.panel, t.panel2, t.bg}, 4.5);
        goodT = Readable(t.good, {t.panel, t.panel2, t.bg}, 4.5);
        ring = t.text1;   // keyboard focus ring around chips and buttons: text1 is >= 10:1 on bg and panel in every theme
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
    bool tiStop = false;   // the "Stop it" action of the TrustedInstaller-left-running banner
};

struct LoadOut {
    bool ok = false, storeRefused = false;
    std::wstring err, storeErr;
    std::map<std::wstring, int> access;   // CanWrite per name (lower case): 0 admin can write, 1 refused, -1 unknown
    std::vector<SvcItem> items;
    HwInfo hw;
    std::wstring build, machine;
    bool hasFirst = false;
    Snapshot first;
    FirstRunResult fr;
    std::vector<UndoLog> logs;
    std::vector<std::wstring> logProblems;
};

// Everything a plan is built from, copied, so the plan worker never reads what the UI thread may replace.
struct PlanJob {
    PlanReq req;
    std::vector<SvcItem> items;
    std::vector<UndoLog> logs;
    HwInfo hw;
    bool hasFirst = false;
    Snapshot first;
    std::wstring build, machine;
    bool tiOn = false;
    Plan out;
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
        loadW_.Cancel(); applyW_.Cancel(); planW_.Cancel();
        loadW_.Join(); applyW_.Join(); planW_.Join();
        if (hwnd_) KillTimer(hwnd_, kProgressTimer);
    }
    int RunCli(int argc, wchar_t** argv) override;
    void LoadFixture(const wchar_t* dir) override;
    void Paint(HDC dc, const RECT& rc) override;
    void PaintPage(HDC dc, const RECT& rc);

private:
    static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT OnView(HWND h, UINT m, WPARAM w, LPARAM l);
    PaintCtx PCtx() const { return PaintCtx{host_->Theme(), host_->Fonts(), host_->Dpi(), host_->Layout()}; }
    int Sc(int px) const { return S(px, host_ ? host_->Dpi() : 96); }
    bool Console() const { return host_ && host_->Layout() == Layout::Console; }
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    bool HasFocus() const { return shotFocus_ || (hwnd_ && GetFocus() == hwnd_); }
    int FocusTarget() const;              // focus_ as a tab-stop id
    void FocusTo(int id);                 // the reverse
    void MoveFocus(bool back);            // Tab / Shift+Tab
    void BuildTabStops(HDC dc);           // after a paint: tabStops_ from the click targets, plus the focus ring
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
    void RevealSelection(bool centre);   // scroll the list so its selected row is on screen (centred after a sort)
    std::wstring SortKeyShown(bool* asc) const;   // the key the list is sorted by now: the chosen one, or name if it doesn't fit
    void SyncListSort();                 // put the header's sort arrow on that key's column
    struct Verdict { std::wstring label; int rank; };
    Verdict KbVerdict(const SvcItem& s) const;
    std::wstring ChangedText(const SvcItem& s) const;   // vs the first-run snapshot
    std::wstring BasisShort(const SvcItem& s) const;
    bool CanApply(std::wstring* why) const;
    int WriteAccess(const std::wstring& name) const;   // 0 admin can write, 1 refused (needs TI), -1 unknown

    // plans
    void OpenPlan(const PlanReq& r);
    void OnPlanned();
    static Plan BuildFrom(PlanJob& j, Backend* b, const Kb* kb);
    void ClosePlan() { mode_ = Mode::List; planReq_ = PlanReq{}; focus_ = Focus::List; replan_ = false; }
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

    // Console layout (docs/pipeline/svcdrv/design/console.md): its own dense grid (18 px rows; core's ListView
    // is fixed at 22), a bottom detail pane in two columns, and an F-key bar. Selection still lives in
    // list_/plist_ so the shared code (Refilter, SelectName, plans) works unchanged in both layouts.
    struct ConCol { std::wstring key, title; int w; bool path; };
    struct ConCell { std::wstring text; COLORREF color; };
    struct ConGrid {
        ScrollBar sb;
        RECT rc{}, body{}, track{};
        int rowH = 18, lastSel = -2;
        std::vector<int> colL, colR;   // header cells in client pixels, for sort clicks
        int Rows() const { return rowH > 0 ? std::max(1, (int)(body.bottom - body.top) / rowH) : 1; }
        void Ensure(int sel) {
            if (sel < 0 || rowH <= 0) return;
            int top = sel * rowH, view = body.bottom - body.top;
            if (top < sb.Offset()) sb.SetOffset(top);
            else if (top + rowH > sb.Offset() + view) sb.SetOffset(top + rowH - view);
        }
    };
    struct FKey { std::wstring cap, label, shortLabel; int act; bool enabled, on; };
    void PaintConsole(HDC dc, const RECT& rc);
    int PaintConsoleBanners(HDC dc, const RECT& rc, int y);
    void PaintConsoleList(HDC dc, const RECT& rc, int y, int bottom);
    void PaintConsolePlan(HDC dc, const RECT& rc, int y, int bottom);
    void PaintConsoleGrid(HDC dc, ConGrid& g, const RECT& rc, const std::vector<ConCol>& cols, int n, int sel,
                          int sortCol, bool asc, const std::function<void(int, std::vector<ConCell>&)>& cellsOf);
    void PaintConsoleDetail(HDC dc, const RECT& r, const std::wstring& name, const std::wstring& info,
                            const std::vector<Para>& doc, bool actions);
    void PaintConsoleHelp(HDC dc, const RECT& r);
    void PaintFKeyBar(HDC dc, const RECT& r);
    int ConButton(HDC dc, int x, int y, const std::wstring& label, int id, bool on, bool enabled, bool primary = false);
    std::vector<ConCol> ConsoleListColumns(HDC dc, int widthPx);
    std::vector<ConCol> ConsolePlanColumns(HDC dc, int widthPx);
    void ConsoleListCells(int row, std::vector<ConCell>& out);
    void ConsolePlanCells(int row, std::vector<ConCell>& out);
    std::vector<FKey> ConsoleKeys() const;
    bool OnConsoleInput(UINT msg, WPARAM w, LPARAM l, bool* handled);
    bool OnConsoleKey(UINT vk);
    void OnConsoleAction(int act);
    void ConsoleSortPlan(int col);
    void ConsoleStatus();
    void ReplayKeys(const std::wstring& keys);

    ConGrid conList_, conPlan_;
    ScrollBar conLeftSb_;              // the detail pane's left column; detSb_ scrolls the right one
    RECT conLeftRc_{}, conLeftTrack_{};
    std::vector<std::wstring> conPlanKeys_;
    int conPlanSort_ = -1;
    bool conPlanAsc_ = true;
    bool conHelp_ = false, conItemBar_ = false;
    std::wstring conWindir_;

    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;

    Kb kb_;
    bool kbOk_ = false;
    std::wstring kbErr_;
    std::unique_ptr<Backend> backend_;
    std::unique_ptr<FakeBackend> fake_;
    std::unique_ptr<TiProvider> ti_;
    Store store_;
    bool fixture_ = false, readOnly_ = false, elevated_ = true, loaded_ = false, writesOff_ = false, storeRefused_ = false;
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
    std::wstring sortKey_;   // the column key the list is sorted by ("" = name); kept by key, not by visible index
    bool sortAsc_ = true;
    bool syncingSort_ = false;   // SyncListSort is driving the header: not a user's click
    std::wstring selName_;
    ScrollBar detSb_;
    RECT rcSearch_{}, rcList_{}, rcDetail_{}, rcDetSb_{}, rcPlanList_{};
    std::vector<Click> clicks_;
    std::vector<Click> tabStops_;   // this paint's focusable targets in reading order (top, then left)
    int focusId_ = 0;               // the focused chip or button when focus_ == Focus::Control
    bool swallowChar_ = false;      // the WM_CHAR of a Space/Enter that activated a control
    bool shotFocus_ = false;        // --focus in fixture mode: draw focus as if the window had it (design screenshots)
    int pendingStop_ = -1;
    bool centrePending_ = false;    // --select: centre the selected row at the first paint          // --focus <n>: focus the n-th tab stop once the first paint has listed them
    std::wstring argFocus_, argSort_;
    int hoverId_ = 0;
    bool tracking_ = false;
    std::vector<std::wstring> colKeys_;   // what each visible list column shows
    std::vector<int> colW_;               // their widths (96-dpi units), for SyncListSort's header hit
    mutable std::map<std::wstring, int> writeAccess_;   // CanWrite results, per name, until the next load

    // plan screen
    Plan plan_;
    PlanReq planReq_;
    ListView plist_;
    std::wstring planErr_;
    bool planBusy_ = false, replan_ = false;   // the plan worker is reading the live state; a newer request waits

    // workers
    sys::Worker loadW_, applyW_, planW_;
    PlanJob planJob_;
    LoadOut loadOut_;
    Job job_;
    std::atomic<bool> cancelApply_{false};
    bool haveResult_ = false;
    ApplyResult result_;
    std::wstring resultTitle_;
    bool resultControl_ = false;
    std::vector<std::wstring> notices_;

    // --screenshot / --fixture arguments applied after the fixture loads
    std::wstring argSelect_, argPlan_, argKeys_;
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
    case WM_SVC_PLANNED: OnPlanned(); return 0;
    case WM_RBUTTONDOWN: SetFocus(h); return 0;
    case WM_SYSKEYDOWN:   // F10 arrives as a system key; in Console it is an F-key, not the menu key
        if (w == VK_F10 && Console()) { if (OnInput(WM_KEYDOWN, w, l)) Invalidate(); return 0; }
        break;
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
    search_.placeholder = L"Filter by name, company or group";   // also matches display name and account
    search_.onChanged = [this](const std::wstring&) { Refilter(); };
    list_.onSortChanged = [this](int col, bool asc) {
        if (syncingSort_) return;
        if (col < 0 || col >= (int)colKeys_.size()) return;
        sortKey_ = colKeys_[col];
        sortAsc_ = asc;
        Refilter();
        RevealSelection(true);   // the selected row stays on screen wherever the new order puts it
        if (!fixture_) { host_->Prefs().SetStr(L"sortKey", sortKey_); host_->Prefs().SetBool(L"sortAsc", asc); }
    };
    plist_.onSortChanged = [this](int col, bool asc) {
        auto key = [&](const PlanRow& r) -> std::wstring {
            switch (col) {
                case 0: return !r.blocked.empty() ? L"2" : (r.include ? L"0" : L"1");
                case 2: return std::to_wstring(r.before.start);
                case 3: return std::to_wstring(r.after.start);
                case 4: return Lower(!r.blocked.empty() ? r.blocked : !r.warning.empty() ? r.warning : !r.depWarning.empty() ? r.depWarning : r.reason);
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
    argFocus_ = ArgValue(cl.rest, L"--focus");
    argSort_ = ArgValue(cl.rest, L"--sort");   // fixture renders: <column key>[:desc], e.g. kb:desc
    argKeys_ = ArgValue(cl.rest, L"--keys");   // fixture only: keys replayed after load (Console screenshots)
    readOnly_ = HasArg(cl.rest, L"--readonly");
    std::wstring dataDir = ArgValue(cl.rest, L"--data-dir");
    fixture_ = !cl.screenshot.empty() || cl.selftest || !cl.fixture.empty();
    // --ti on is for fixture renders only: on live services the toggle is off at every launch (spec 5.6).
    if (fixture_ && ArgValue(cl.rest, L"--ti") == L"on") tiOn_ = true;
    if (fixture_) {
        // Fixture mode: the fake backend, the fixture folder as a read-only data dir. Nothing is written,
        // nothing touches real services or %ProgramData%. --screenshot loads through LoadFixture (host).
        if (cl.screenshot.empty() && !cl.fixture.empty()) LoadFixture(cl.fixture.c_str());
        return hwnd_;
    }
    tab_ = host.Prefs().GetInt(L"tab", tab_) == TAB_DRIVERS ? TAB_DRIVERS : TAB_SERVICES;
    // By column key: the old "sort" value was a visible-column index, which pointed at another column whenever the
    // width changed which columns fit. It is ignored.
    sortKey_ = host.Prefs().GetStr(L"sortKey", L"");
    sortAsc_ = host.Prefs().GetBool(L"sortAsc", true);
    search_.SetText(host.Prefs().GetStr(L"filter", L""));
    backend_ = MakeLiveBackend();
    // The full path without a \\?\ prefix: the README shows it, and Store adds the prefix for each call.
    if (!dataDir.empty()) store_.SetRoot(Store::Plain(sys::ToExtended(dataDir)));
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
    const Kb* kb = kbOk_ ? &kb_ : nullptr;
    sys::Worker* w = &loadW_;
    bool started = loadW_.Start(hwnd_, WM_SVC_LOADED, [b, st, mayWrite, out, kb, w]() mutable {
        *out = LoadOut{};
        std::wstring err;
        auto cancelled = [&]() { if (w->Cancelled()) { out->err = L"Loading was cancelled."; return true; } return false; };
        if (!b->Enumerate(&out->items, &err)) { out->err = L"Could not read the services: " + err; return; }
        for (auto& i : out->items) {
            if (cancelled()) return;
            i.company = FileCompany(i);
        }
        b->ReadHw(&out->hw);
        out->build = b->Build();
        out->machine = b->MachineGuid();
        FoldInstances(&out->items);
        // Write access per item here, on the worker: the detail pane never calls the SCM from paint.
        for (auto& i : out->items) {
            if (cancelled()) return;
            if (i.kind == Kind::UserInstance) continue;
            bool denied = false;
            std::wstring e;
            out->access[Lower(i.name)] = b->CanWrite(i.name, &denied, &e) ? 0 : (denied ? 1 : -1);
        }
        if (cancelled()) return;
        if (st.Valid()) {
            if (mayWrite) {
                if (!st.EnsureRoot(true, &err)) {
                    // Nothing is read from or written to a folder Hadron can't trust: no first.txt, no undo logs.
                    out->storeRefused = true;
                    out->storeErr = L"The data folder can't be used, so changes are off: " + err;
                    out->ok = true;
                    return;
                }
                Ctx c;
                c.kb = kb;
                c.hw = out->hw.ok ? &out->hw : nullptr;
                c.build = out->build;
                c.machine = out->machine;
                c.Prepare();
                out->fr = EnsureFirstSnapshot(st, out->items, c, &out->first);
                out->hasFirst = (out->fr.wrote || out->fr.existed) && !out->fr.foreign;
            } else if (st.Exists(L"first.txt")) {
                std::string text;
                if (st.Read(L"first.txt", &text, &err) && ParseSnapshot(text, &out->first, &err)) out->hasFirst = true;
            }
            if (cancelled()) return;
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
    writeAccess_ = std::move(o.access);
    if (o.storeRefused) {
        storeRefused_ = true;
        writesOff_ = true;
        storeNote_ = o.storeErr;   // a lasting banner, not a dismissable notice
    } else {
        if (storeRefused_) {   // fixed since the last load (Refresh)
            storeRefused_ = false;
            storeNote_.clear();
            writesOff_ = readOnly_ || !store_.Valid();
        }
        if (!o.storeErr.empty()) notices_.push_back(o.storeErr);
    }
    if (!o.fr.error.empty()) notices_.push_back(L"First-run snapshot: " + o.fr.error);
    if (!o.fr.note.empty()) notices_.push_back(o.fr.note);
    if (o.fr.wrote && !o.fr.corrupt)
        notices_.push_back(L"Recorded this PC's start types in first.txt, with a recovery folder and zip, before any change.");
    for (auto& p : o.logProblems) notices_.push_back(L"Undo log skipped: " + p);
    auto keep = std::move(writeAccess_);
    TakeData(std::move(o.items), o.hw, o.build, o.machine);
    writeAccess_ = std::move(keep);
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
    // CLAUDE.md: v1 has no screen-reader support, and the UI says so.
    Status(L"Services: loaded " + std::to_wstring(items_.size()) + L" services and drivers. " +
           (Console() ? L"Keyboard: F1 lists the keys." : L"Keyboard: Tab and Shift+Tab move, Enter or Space activates.") +
           L" No screen-reader support in v1.");
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
    for (auto& s : items_) {
        bool denied = false;
        std::wstring e;
        writeAccess_[Lower(s.name)] = fake_->CanWrite(s.name, &denied, &e) ? 0 : (denied ? 1 : -1);
    }
    if (!argSort_.empty()) {
        size_t colon = argSort_.find(L':');
        sortKey_ = argSort_.substr(0, colon);
        sortAsc_ = colon == std::wstring::npos || argSort_.substr(colon + 1) != L"desc";
        Refilter();
    }
    if (!argSelect_.empty()) { SelectName(argSelect_); centrePending_ = true; }   // centred once the list has a size
    if (!argPlan_.empty()) {
        int lv = LevelFromName(argPlan_);
        PlanReq r;
        r.tab = tab_;
        if (lv >= 0) { r.type = PlanReq::Preset; r.level = lv; }
        else if (lv == LEVEL_DEFAULT) r.type = PlanReq::Default;
        else if (lv == -2) r.type = PlanReq::Stock;
        else if (argPlan_ == L"undo" && FindInterrupted(logs_) >= 0) { r.type = PlanReq::Undo; r.logIndex = UndoBase(logs_, FindInterrupted(logs_)); }
        else if (argPlan_ == L"undo-last" && FindUndoable(logs_) >= 0) { r.type = PlanReq::Undo; r.logIndex = FindUndoable(logs_); }
        else if (argPlan_ == L"resume" && FindInterrupted(logs_) >= 0) { r.type = PlanReq::Resume; r.logIndex = FindInterrupted(logs_); }
        else if (argPlan_ == L"ti-stop") { r.type = PlanReq::Control; r.item = L"TrustedInstaller"; r.what = 1; r.tiStop = true; }
        if (r.type != PlanReq::None) OpenPlan(r);
    }
    if (!argFocus_.empty()) {   // design screenshots: show a focus state without a real keyboard
        shotFocus_ = true;
        if (argFocus_ == L"list") focus_ = Focus::List;
        else if (argFocus_ == L"search") focus_ = Focus::Search;
        else if (argFocus_ == L"detail") focus_ = Focus::Detail;
        else pendingStop_ = _wtoi(argFocus_.c_str());
    }
    if (!argKeys_.empty()) ReplayKeys(argKeys_);
}

// What the presets do with this item, by the same tests BuildPreset runs per row (guards, the sysok rule for
// drivers, the hardware evidence), so the column agrees with the detail pane and with the plans. rank orders the
// column from "goes first" to "never touched": bloat < stripped < minimum < kept < no preset < protected <
// not in the KB. Current start types don't matter here (a preset that finds it already set skips it).
ServicesModule::Verdict ServicesModule::KbVerdict(const SvcItem& s) const {
    Guard g = CheckGuard(s, ctx_);
    if (g.neverTouch) return {L"protected", 5};
    const KbEntry* e = kbOk_ ? kb_.Find(s.name) : nullptr;
    if (!e) return {L"\u2014", 6};
    if (g.neverPreset) return {L"no preset", 4};
    int j = e->JoinLevel();
    if (j < 0) return {L"no preset", 4};
    if (IsDriverType(s.type) && !e->sysok) return {L"no preset", 4};
    if (!e->hw.empty()) {
        std::wstring ev;
        int h = ctx_.hw ? HwEvidence(*ctx_.hw, e->hw, s.name, &ev) : 0;
        if (h == 0) return {L"kept", 3};   // hardware in use or seen before: "kept by every preset" in the detail
    }
    return {ShortLevel(j), j};
}

// The list column drops a trailing legal form ("Microsoft Corporation" -> "Microsoft") so the name fits a narrow
// column; the detail pane and the filter use the full text the file claims.
std::wstring CompanyShort(const std::wstring& c) {
    static const wchar_t* const kSuffix[] = {L" Corporation", L" Corp.", L", Inc.", L" Inc.", L", Inc", L" Inc",
                                             L" Co., Ltd.", L" Co.,Ltd.", L" Ltd.", L" Ltd", L" GmbH", L" AG", L" LLC"};
    for (const wchar_t* suf : kSuffix) {
        size_t n = wcslen(suf);
        if (c.size() > n + 1 && c.compare(c.size() - n, n, suf) == 0) return c.substr(0, c.size() - n);
    }
    return c;
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
    bool asc = true;
    const std::wstring key = SortKeyShown(&asc);
    auto text = [&](const SvcItem& s) -> std::wstring {
        if (key == L"display") return Lower(s.display);
        if (key == L"state") return std::to_wstring(s.state == SERVICE_RUNNING ? 0 : 1) + StateName(s.state);
        if (key == L"start") return std::to_wstring(s.cfg.start) + (s.cfg.Delayed() ? L"1" : L"0");
        if (key == L"kb") { Verdict v = KbVerdict(s); return std::to_wstring(v.rank) + v.label; }
        if (key == L"company") return Lower(CompanyShort(s.company));
        if (key == L"account") return Lower(s.account);
        if (key == L"group") return Lower(s.group);
        if (key == L"type") return TypeName(s.type);
        if (key == L"changed") return ChangedText(s);
        if (key == L"basis") return BasisShort(s);
        if (key == L"image") return Lower(s.image);
        return Lower(s.name);
    };
    // Keys once per row (the Hadron column runs CheckGuard), not once per comparison.
    std::vector<std::pair<std::wstring, std::wstring>> keys(items_.size());
    for (int i : V_) keys[i] = {text(items_[i]), Lower(items_[i].name)};
    std::stable_sort(V_.begin(), V_.end(), [&](int a, int b) {
        const auto& ka = keys[a];
        const auto& kb = keys[b];
        if (ka.first == kb.first) return ka.second < kb.second;
        return asc ? ka.first < kb.first : kb.first < ka.first;
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
            else if (k == L"kb") r.push_back(KbVerdict(s).label);
            else if (k == L"company") r.push_back(CompanyShort(s.company));
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
    else if (!V_.empty()) {
        // Nothing chosen yet (first load, or a tab switch cleared it): the first row, scrolled to, so the highlight
        // and the detail pane always show the same item. The list keeps its old index otherwise (U1).
        list_.Select(0);
        RevealSelection(false);
        selName_ = items_[V_[0]].name;
    }
    if (!fixture_ && host_) {
        host_->Prefs().SetStr(L"filter", search_.Text());
        host_->Prefs().SetInt(L"tab", tab_);
    }
}

const SvcItem* ServicesModule::Selected() const {
    for (auto& s : items_) if (IEq(s.name, selName_)) return &s;
    return nullptr;
}

void ServicesModule::RevealSelection(bool centre) {
    int k = list_.Selected(), n = list_.RowCount();
    if (k < 0 || n == 0 || !host_ || rcList_.right <= rcList_.left) return;
    PaintCtx c = PCtx();
    // ListView::Select does not scroll and core has no scroll setter: select the row above and step down (or Home
    // for row 0), which runs the list's own EnsureVisible.
    auto show = [&](int i) {
        i = std::clamp(i, 0, n - 1);
        if (i == 0) list_.OnKey(VK_HOME, rcList_, c);
        else { list_.Select(i - 1); list_.OnKey(VK_DOWN, rcList_, c); }
    };
    if (centre) {
        // Rows below and above first, so the selection lands mid-list with context, not on the last line.
        int rows = (rcList_.bottom - rcList_.top - Sc(Console() ? 24 : 32)) / std::max(1, Sc(Console() ? 22 : 30));
        int half = std::max(0, rows / 2);
        show(k + half);
        show(k - half);
    }
    show(k);
}

std::wstring ServicesModule::SortKeyShown(bool* asc) const {
    // Sorted by a column that doesn't fit at this width: name order until it fits again (the choice is kept), so
    // the order on screen always matches the header's arrow.
    bool shown = !sortKey_.empty() && std::find(colKeys_.begin(), colKeys_.end(), sortKey_) != colKeys_.end();
    *asc = shown ? sortAsc_ : true;
    return shown ? sortKey_ : std::wstring(L"name");
}

void ServicesModule::SyncListSort() {
    bool asc = true;
    std::wstring key = SortKeyShown(&asc);
    int col = -1;
    for (int i = 0; i < (int)colKeys_.size(); i++) if (colKeys_[i] == key) col = i;
    if (col < 0 || col >= (int)colW_.size() || rcList_.right <= rcList_.left) return;
    if (list_.SortColumn() == col && list_.SortAscending() == asc) return;
    // Core's ListView has no sort setter (proposed in the design write-up): click its header, which sets the column
    // ascending, or toggles the direction when it is already that column. The callback ignores these clicks.
    int x = rcList_.left + 1;
    for (int i = 0; i < col; i++) x += Sc(colW_[i]);
    PaintCtx c = PCtx();
    syncingSort_ = true;
    for (int tries = 0; tries < 2 && !(list_.SortColumn() == col && list_.SortAscending() == asc); tries++)
        list_.OnMouse(WM_LBUTTONDOWN, x + 1, rcList_.top + 1, rcList_, c);
    syncingSort_ = false;
}

void ServicesModule::SelectName(const std::wstring& name) {
    for (int k = 0; k < (int)V_.size(); k++) {
        if (!IEq(items_[V_[k]].name, name)) continue;
        list_.Select(k);
        RevealSelection(false);
        if (!IEq(selName_, name)) detSb_.SetOffset(0);
        selName_ = items_[V_[k]].name;
        return;
    }
    if (!V_.empty() && list_.Selected() >= 0 && list_.Selected() < (int)V_.size()) selName_ = items_[V_[list_.Selected()]].name;
}

int ServicesModule::WriteAccess(const std::wstring& name) const {
    // Read on the load worker (or from the fake); paint never calls the SCM.
    auto it = writeAccess_.find(Lower(name));
    return it != writeAccess_.end() ? it->second : -1;
}

bool ServicesModule::CanApply(std::wstring* why) const {
    if (fixture_) { *why = L"Fixture data: applying is off."; return false; }
    if (!elevated_) { *why = L"Not running as administrator: read-only."; return false; }
    if (readOnly_) { *why = L"Read-only mode (--readonly)."; return false; }
    if (!store_.Valid()) { *why = L"No data folder for the undo log: changes are off."; return false; }
    if (storeRefused_) { *why = L"The data folder can't be used: changes are off (see the banner)."; return false; }
    if (planBusy_) { *why = L"Reading the live state for this plan."; return false; }
    if (applyW_.Running() || loadW_.Running()) { *why = L"Busy."; return false; }
    return true;
}

// ============================================================================================
// Plans

// Runs on the plan worker for live services (MarkTi's write checks, Undo/Resume re-reads and Dependents are SCM
// calls), on the UI thread for the fake. Reads only the job's own copies.
Plan ServicesModule::BuildFrom(PlanJob& j, Backend* b, const Kb* kb) {
    Ctx c;
    c.kb = kb;
    c.hw = j.hw.ok ? &j.hw : nullptr;
    c.first = j.hasFirst ? &j.first : nullptr;
    c.build = j.build;
    c.machine = j.machine;
    c.tiOn = j.tiOn;
    c.Prepare();
    const PlanReq& r = j.req;
    Plan p;
    const SvcItem* it = nullptr;
    for (auto& s : j.items) if (IEq(s.name, r.item)) it = &s;
    switch (r.type) {
        case PlanReq::Preset: p = BuildPreset(r.level, r.tab, j.items, c); break;
        case PlanReq::Default: p = BuildDefault(false, r.tab, j.items, c); break;
        case PlanReq::Stock: p = BuildDefault(true, r.tab, j.items, c); break;
        case PlanReq::Single:
            if (it) p = BuildSingle(*it, r.target, j.items, c);
            break;
        case PlanReq::Control:
            if (it && b) p = BuildControl(*it, r.what, *b, j.items, c);
            break;
        case PlanReq::Undo:
            if (b && r.logIndex >= 0 && r.logIndex < (int)j.logs.size()) p = BuildUndo(j.logs[r.logIndex], *b, j.items, c);
            break;
        case PlanReq::Resume:
            if (b && r.logIndex >= 0 && r.logIndex < (int)j.logs.size()) p = BuildResume(j.logs[r.logIndex], *b, j.items, c);
            break;
        case PlanReq::None: break;
    }
    if (b && p.kind != PlanKind::Control) MarkTi(&p, *b, j.tiOn, j.items);   // re-runs the dependency check
    if (r.tiStop)   // BuildControl's generic "stop" says nothing about why this one is offered
        for (auto& row : p.rows)
            if (row.ctl == Op::Stop && IEq(row.name, L"TrustedInstaller") && row.blocked.empty())
                row.reason = L"Hadron started it for an apply that did not finish, and nothing stopped it. It is a Manual "
                             L"(on-demand) service: Windows starts it again when it next installs updates or features.";
    return p;
}

void ServicesModule::OpenPlan(const PlanReq& r) {
    planReq_ = r;
    mode_ = Mode::Plan;
    focus_ = Focus::List;
    detSb_.SetOffset(0);
    if (planBusy_) { replan_ = true; Invalidate(); return; }   // the running build finishes first, then this one
    PlanJob j;
    j.req = r;
    j.items = items_;
    j.logs = logs_;
    j.hw = hw_;
    j.hasFirst = hasFirst_;
    j.first = first_;
    j.build = build_;
    j.machine = machine_;
    j.tiOn = tiOn_;
    const Kb* kb = kbOk_ ? &kb_ : nullptr;
    if (fake_ || !backend_) {   // the fake answers at once: build here
        plan_ = BuildFrom(j, fake_.get(), kb);
        OnPlanned();
        return;
    }
    planJob_ = std::move(j);
    planBusy_ = true;
    plan_ = Plan{};
    plan_.title = L"reading the live state…";
    RefreshPlanList();
    PlanJob* job = &planJob_;
    Backend* b = backend_.get();
    if (!planW_.Start(hwnd_, WM_SVC_PLANNED, [job, b, kb]() { job->out = BuildFrom(*job, b, kb); })) {
        planBusy_ = false;
        plan_.title = L"could not start the worker thread";
        Status(L"Services: could not start the worker thread: " + sys::ErrorText(GetLastError()));
    }
    Invalidate();
}

void ServicesModule::OnPlanned() {
    if (planBusy_) {
        planW_.Join();
        planBusy_ = false;
        plan_ = std::move(planJob_.out);
        if (replan_) {
            replan_ = false;
            if (mode_ == Mode::Plan && planReq_.type != PlanReq::None) { OpenPlan(planReq_); return; }
        }
    }
    if (mode_ != Mode::Plan) return;   // closed while it was being built
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
        std::wstring note = !r.blocked.empty() ? r.blocked : !r.warning.empty() ? r.warning : !r.depWarning.empty() ? r.depWarning : r.reason;
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
    // Dependencies again, for every row: unticking a dependent can leave what it depends on with an enabled
    // dependent, and ticking one can clear another row's warning.
    if (plan_.kind != PlanKind::Control) DependencyCheck(&plan_, items_, row);
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
    if (id >= ID_CONKEY && id < ID_CONKEY + 100) { OnConsoleAction(id - ID_CONKEY); return; }
    if (id == ID_REFRESH) { if (!fixture_) StartLoad(); return; }
    if (id == ID_TAB_SVC || id == ID_TAB_DRV) {
        int t = id == ID_TAB_SVC ? TAB_SERVICES : TAB_DRIVERS;
        if (t != tab_) { tab_ = t; selName_.clear(); detSb_.SetOffset(0); Refilter(); }   // Refilter picks row 0 (U1)
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
        // Undo of an interrupted undo restarts that undo from the original apply's log, never "undoes the undo".
        r.logIndex = id == ID_INT_UNDO ? UndoBase(logs_, interrupted_) : interrupted_;
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
        r.tiStop = true;
        OpenPlan(r);
        return;
    }
}

// ============================================================================================
// Input

bool ServicesModule::OnInput(UINT msg, WPARAM w, LPARAM l) {
    if (!host_) return false;
    if (Console()) { bool handled = false; bool r = OnConsoleInput(msg, w, l, &handled); if (handled) return r; }
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
            MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return true;
        }
        if (focus_ == Focus::Control && (w == VK_RETURN || w == VK_SPACE)) {
            swallowChar_ = true;   // its WM_CHAR must not reach the filter box
            int id = focusId_;
            if (id != ID_APPLY) OnClickId(id);   // never a tab stop; checked again so no key can ever apply
            return true;
        }
        if (focus_ == Focus::Control && (w == VK_LEFT || w == VK_RIGHT)) {   // along a row of chips, like a toolbar
            MoveFocus(w == VK_LEFT);
            return true;
        }
        if (focus_ == Focus::Control && w != VK_ESCAPE && !(ctrl && w == 'F')) return false;   // keys don't leak into the list
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
        if (swallowChar_) { swallowChar_ = false; return false; }
        if (focus_ == Focus::Control && ch == L' ') return false;   // typing anything else still goes to the filter
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
            if (ck.id != ID_SEARCH && (focus_ == Focus::Search || focus_ == Focus::Control)) focus_ = Focus::List;
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
    if (!storeNote_.empty()) v.push_back({storeNote_, storeRefused_ ? 2 : 1, {}});
    if (loaded_ && loadErr_.empty() && (!hw_.ok || hw_.bootDisk.empty()))
        v.push_back({std::wstring(!hw_.ok ? L"The device list could not be read" : L"The boot disk could not be found") +
                         (hw_.error.empty() ? L"" : L" (" + hw_.error + L")") +
                         L". The filter and boot-path checks can't run without it, so every driver is kept: none is offered in a plan "
                         L"or as a single change.",
                     1, {}});
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
        bool undoOf = lg.source.rfind(L"undo:", 0) == 0;
        std::wstring t = std::wstring(undoOf ? L"An undo did not finish (" : L"An apply did not finish (") + lg.stamp + L", " +
                         Unesc(lg.source) + L"): " + std::to_wstring(done) + L" of " + std::to_wstring(lg.count) +
                         L" changes were recorded as done" + (begun > done ? L" and 1 was in progress" : L"") +
                         (undoOf ? L". Undo and Resume both finish undoing " + lg.source.substr(5) + L": Undo from that apply's log, "
                                   L"Resume from this one."
                                 : L". Undo puts back what it changed; Resume re-plans the rest against the live state.");
        if (lg.badRecords) t += L" " + std::to_wstring(lg.badRecords) + L" record(s) in its log failed their check and were ignored.";
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
                draw1(para.text, f.semi, para.color == kNoColor ? p.accT : col, r.left, W, h);
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
    if (focus_ == Focus::Detail && HasFocus()) FocusRectC(dc, r, Pal(host_->Theme()).accT);
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
    if (g.neverTouch) d.push_back({P_TEXT, L"", L"Protected: Hadron never changes this. " + g.reason + L".", p.badT});
    else if (g.neverPreset) d.push_back({P_TEXT, L"", L"Not in any preset: " + g.reason + L". A single change is still possible where Windows allows it.", p.warnT});
    if (s.kind == Kind::UserTemplate)
        d.push_back({P_TEXT, L"", L"Per-user service template: Windows creates one instance per signed-in user from it. "
                                  L"Hadron changes only the template; instances follow at the next sign-in.", p.mute});
    if (drift_.count(Lower(s.name))) {
        auto it = lastApplied_.find(Lower(s.name));
        d.push_back({P_TEXT, L"", L"Changed since Hadron set it: Hadron set " + StartLabel(it->second, false) + L", now " +
                                      StartLabel(s.cfg, false) + L".", p.warnT});
    }
    if (s.regStart >= 0 && s.regStart != s.cfg.start)
        d.push_back({P_TEXT, L"", L"The registry Start value (" + StartName(s.regStart) +
                                      L") differs from what the service manager reports: a change is pending a restart.", p.warnT});
    if (!s.readError.empty()) d.push_back({P_TEXT, L"", L"Could not read everything: " + s.readError, p.warnT});

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
            if (!e->contested.empty()) kv(L"Contested", e->contested, p.warnT);
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
            d.push_back({P_TEXT, L"", L"This KB entry is for build " + Join(e->builds, L", ") + L"; this PC is build " + build_ + L".", p.warnT});
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
    if (im.unquotedSpaces) kv(L"Path warning", L"unquoted path with spaces", p.warnT);
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
        // Tamper Protection guards Defender Antivirus's settings; the other Defender items carry their own reason.
        if (IEq(s.name, L"WinDefend"))
            prot += (prot.empty() ? L"" : L"; ") +
                    std::wstring(L"when Tamper Protection is on, it blocks registry changes to Defender Antivirus settings");
        kv(L"Protection", prot, s.protect ? p.warnT : kNoColor);
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
    if (!r.blocked.empty()) d.push_back({P_TEXT, L"", r.blocked, p.badT});
    kv(L"Why", r.reason);
    if (!r.basis.empty()) kv(L"Basis", BasisLabel(r.basis) + (r.basisNote.empty() ? L"" : L": " + r.basisNote), p.mute);
    kv(L"Contested", r.contested, p.warnT);
    kv(L"Evidence", r.evidence);
    kv(L"Warning", r.warning, p.warnT);
    kv(L"Dependencies", r.depWarning, p.warnT);
    if (r.needsTi) kv(L"Identity", L"needs TrustedInstaller: Windows refuses this write to administrators", p.warnT);
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
    // Column plan (docs/pipeline/svcdrv/design/baseline.md): what to change and whether Hadron would is always
    // shown; Company next (it is how third-party and OEM items stand out, and they are never in the KB); then
    // vs first; Basis, Account and Type last, since the detail pane shows each of them in full. 1280x800
    // standalone shows vs first and Company, the suite Company; every column needs about 1670 px of window.
    static const C kSvc[] = {{L"name", L"Name", 140, 0}, {L"display", L"Display name", 0, 0}, {L"state", L"State", 72, 0},
                             {L"start", L"Start type", 116, 0}, {L"kb", L"Hadron", 84, 0}, {L"basis", L"Basis", 76, 3},
                             {L"changed", L"vs first", 70, 4}, {L"company", L"Company", 100, 5},
                             {L"account", L"Account", 110, 2}, {L"type", L"Type", 96, 1}};
    static const C kDrv[] = {{L"name", L"Name", 124, 0}, {L"display", L"Display name", 0, 0}, {L"state", L"State", 72, 0},
                             {L"start", L"Start type", 90, 0}, {L"kb", L"Hadron", 84, 0}, {L"basis", L"Basis", 76, 2},
                             {L"changed", L"vs first", 70, 3}, {L"company", L"Company", 100, 5},
                             {L"group", L"Load group", 116, 4}, {L"type", L"Type", 96, 1}};
    // Console keeps the build's table as it was (its Consolas cells are wider; the Console design owns them).
    static const C kSvcCon[] = {{L"name", L"Name", 156, 0}, {L"display", L"Display name", 0, 0}, {L"state", L"State", 70, 0},
                                {L"start", L"Start type", 120, 0}, {L"changed", L"vs first", 74, 5}, {L"kb", L"Hadron", 80, 0},
                                {L"basis", L"Basis", 80, 4}, {L"type", L"Type", 104, 3}, {L"account", L"Account", 130, 2},
                                {L"company", L"Company", 140, 1}};
    static const C kDrvCon[] = {{L"name", L"Name", 130, 0}, {L"display", L"Display name", 0, 0}, {L"state", L"State", 70, 0},
                                {L"start", L"Start type", 96, 0}, {L"changed", L"vs first", 74, 5}, {L"kb", L"Hadron", 80, 0},
                                {L"basis", L"Basis", 80, 4}, {L"type", L"Type", 100, 3}, {L"group", L"Load group", 130, 2},
                                {L"company", L"Company", 140, 1}};
    const bool con = Console();
    const int kFlexMin = con ? 110 : 100;
    const bool svc = tab_ == TAB_SERVICES;
    const C* all = con ? (svc ? kSvcCon : kDrvCon) : (svc ? kSvc : kDrv);
    const size_t n = con ? (svc ? std::size(kSvcCon) : std::size(kDrvCon)) : (svc ? std::size(kSvc) : std::size(kDrv));
    std::vector<bool> on(n, true);
    int fixed = 0;
    for (size_t i = 0; i < n; i++) fixed += all[i].w;
    for (int rank = 1; rank <= 5 && fixed + kFlexMin > W; rank++)
        for (size_t i = 0; i < n; i++)
            if (all[i].drop == rank && on[i]) { on[i] = false; fixed -= all[i].w; }
    std::vector<ListView::Column> cols;
    std::vector<std::wstring> keys;
    colW_.clear();
    for (size_t i = 0; i < n; i++) {
        if (!on[i]) continue;
        const C& c = all[i];
        int w = c.w ? c.w : std::max(kFlexMin, W - fixed);
        cols.push_back({c.title, w});
        keys.push_back(c.key);
        colW_.push_back(w);
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
    bool pending = pendingStop_ >= 0;
    PaintPage(dc, rc);
    if (pending) PaintPage(dc, rc);   // --focus <n>: the first pass listed the tab stops and moved the focus
}

void ServicesModule::PaintPage(HDC dc, const RECT& rc) {
    clicks_.clear();
    if (Console()) { PaintConsole(dc, rc); BuildTabStops(dc); return; }   // Tab reach and the focus ring in both layouts
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
    BuildTabStops(dc);
}

// ============================================================================================
// Keyboard focus across the page

int ServicesModule::FocusTarget() const {
    switch (focus_) {
        case Focus::List: return ID_FOCUS_LIST;
        case Focus::Search: return ID_SEARCH;
        case Focus::Detail: return ID_FOCUS_DETAIL;
        case Focus::Control: return focusId_;
    }
    return ID_FOCUS_LIST;
}

void ServicesModule::FocusTo(int id) {
    if (id == ID_FOCUS_LIST) focus_ = Focus::List;
    else if (id == ID_SEARCH) focus_ = Focus::Search;
    else if (id == ID_FOCUS_DETAIL) focus_ = Focus::Detail;
    else { focus_ = Focus::Control; focusId_ = id; }
}

void ServicesModule::MoveFocus(bool back) {
    if (tabStops_.empty()) {   // nothing painted yet
        focus_ = Focus::List;
        return;
    }
    int n = (int)tabStops_.size(), cur = -1, cid = FocusTarget();
    for (int i = 0; i < n; i++) if (tabStops_[i].id == cid) cur = i;
    if (cur < 0)   // the focused control went away (disabled, plan closed): continue from the list
        for (int i = 0; i < n; i++) if (tabStops_[i].id == ID_FOCUS_LIST) cur = i;
    int next = cur < 0 ? 0 : (cur + (back ? n - 1 : 1)) % n;
    FocusTo(tabStops_[next].id);
}

// Tab order is reading order, taken from what this paint drew: header buttons, banner actions, tabs and the
// TrustedInstaller switch, presets and the filter, the list, the detail pane's buttons, then the detail text.
// Disabled controls register no click target, so they are skipped. Apply is never a stop.
void ServicesModule::BuildTabStops(HDC dc) {
    tabStops_.clear();
    for (auto& ck : clicks_)
        if (ck.id != ID_APPLY) tabStops_.push_back(ck);
    RECT lr = mode_ == Mode::Plan ? rcPlanList_ : rcList_;
    if (lr.right > lr.left) tabStops_.push_back({lr, ID_FOCUS_LIST});
    if (rcDetail_.right > rcDetail_.left) tabStops_.push_back({rcDetail_, ID_FOCUS_DETAIL});
    std::stable_sort(tabStops_.begin(), tabStops_.end(), [](const Click& a, const Click& b) {
        return a.rc.top != b.rc.top ? a.rc.top < b.rc.top : a.rc.left < b.rc.left;
    });
    if (pendingStop_ >= 0 && pendingStop_ < (int)tabStops_.size()) FocusTo(tabStops_[pendingStop_].id);
    pendingStop_ = -1;
    if (focus_ != Focus::Control) return;
    const RECT* r = nullptr;
    for (auto& ck : clicks_) if (ck.id == focusId_) r = &ck.rc;
    if (!r) { focus_ = Focus::List; return; }   // disabled or gone since: back to the list
    if (!HasFocus()) return;
    // A 2px ring outside the control, in text1: >= 10:1 on the page and panel in every theme, and it doesn't
    // depend on the control's own fill (accent, chip or hover).
    Pal p(host_->Theme());
    const int g = Sc(2), w = std::max(2, Sc(2));
    HPEN pen = CreatePen(PS_SOLID, w, p.ring);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int rad = Console() ? Sc(6) : (r->bottom - r->top) + 2 * g;
    RoundRect(dc, r->left - g - 1, r->top - g - 1, r->right + g + 1, r->bottom + g + 1, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
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
    search_.Paint(dc, rcSearch_, c, focus_ == Focus::Search && HasFocus());
    AddClick(rcSearch_, ID_SEARCH);
    y += rowH + Sc(10);

    // List (left) and detail (right).
    int bottom = rc.bottom - pad;
    int total = rc.right - 2 * pad;
    int listW = total * 60 / 100;
    rcList_ = RECT{pad, y, pad + listW, bottom};
    RECT det{pad + listW + Sc(12), y, rc.right - pad, bottom};
    if (SetListColumns(listW)) Refilter();
    SyncListSort();
    if (centrePending_) { centrePending_ = false; RevealSelection(true); }
    if (!loaded_ || !loadErr_.empty() || items_.empty()) {
        FillSolid(dc, rcList_, p.panel);
        FrameRectC(dc, rcList_, p.border);
        std::wstring t = !loadErr_.empty() ? loadErr_ : !loaded_ ? L"Reading services and drivers…" : L"No data.";
        RECT tr = rcList_;
        InflateRect(&tr, -Sc(20), -Sc(20));
        ui::DrawText(dc, t, tr, f.ui, loadErr_.empty() ? p.mute : p.badT, DT_WORDBREAK | DT_CENTER);
    } else {
        // Column texts depend on the columns that fit: rebuild once the widths are known.
        list_.Paint(dc, rcList_, c, focus_ == Focus::List && HasFocus());
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
        bool cen = can && s->kind != Kind::UserTemplate && !g.neverTouch;   // R5: never-touch: no start/stop either
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
    TextLeft(dc, sum, sr, f.caption, can ? p.mute : p.warnT);
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
    plist_.Paint(dc, rcPlanList_, c, focus_ == Focus::List && HasFocus());
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
// Console layout (docs/pipeline/svcdrv/design/console.md)
//
// Everything below runs only when host_->Layout() == Layout::Console; Paint and OnInput dispatch here in
// one line each. Same data, same plans, same safety: every key either changes the view or opens a plan, and no
// key applies one (Apply stays a click on the plan screen, exactly as in Baseline).

namespace {
enum ConAct {
    CA_NONE = 0, CA_HELP, CA_TAB, CA_FILTER, CA_ITEM, CA_REFRESH, CA_TI,
    CA_PRESET,                 // + 0 first snapshot, 1 Windows stock, 2 bloat, 3 stripped, 4 minimum
    CA_UNDO = CA_PRESET + 5,
    CA_SET,                    // + 0 Auto, 1 Auto (delayed), 2 Manual, 3 Disabled
    CA_CTL = CA_SET + 4,       // + 0 start, 1 stop, 2 restart
    CA_BACK = CA_CTL + 3, CA_TICK, CA_CANCEL,
};

// WCAG 2 relative luminance and contrast ratio.
double ChanLin(int v) { double c = v / 255.0; return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double RelLum(COLORREF c) { return 0.2126 * ChanLin(GetRValue(c)) + 0.7152 * ChanLin(GetGValue(c)) + 0.0722 * ChanLin(GetBValue(c)); }
double Contrast(COLORREF a, COLORREF b) {
    double x = RelLum(a), y = RelLum(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}
// A filled block with text on it (the selection, tags): white text, as Windows draws on its accent; black text when
// white is well short (under 4:1) and black reaches 4.5:1. If the chosen text is still under 4.5:1, the fill is
// moved away from it just enough (Windows blue 0078D7 gives white 4.50 and is darkened about 3%). The hue stays.
void InverseVideo(COLORREF base, COLORREF* fill, COLORREF* ink) {
    const COLORREF white = RGB(255, 255, 255), black = RGB(0, 0, 0);
    double w = Contrast(white, base);
    *ink = (w < 4.0 && Contrast(black, base) >= 4.5) ? black : white;
    *fill = base;
    for (int i = 1; i <= 12 && Contrast(*ink, *fill) < 4.5; i++) *fill = Mix(base, *ink == white ? black : white, i * 0.03);
}
// The colour, moved toward `toward` (the theme's body text) only as far as it takes to reach `target` on both
// surfaces. Keeps the hue of an accent or a semantic colour while making it legible as text.
COLORREF Ink(COLORREF c, COLORREF s1, COLORREF s2, COLORREF toward, double target) {
    for (int i = 0; i <= 20; i++) {
        COLORREF m = Mix(c, toward, i / 20.0);
        if (Contrast(m, s1) >= target && Contrast(m, s2) >= target) return m;
    }
    return toward;
}

// Console colours over Pal: text-safe inks for the accent and the semantic colours, a 3:1 rule colour for
// boundaries (the theme's border token is ~1.2-1.6:1), and an inverse-video selection that keeps 4.5:1.
struct CPal : Pal {
    COLORREF zebra, rule, accInk, warnInk, badInk, goodInk, barAccInk, selFill, selInk, dim;
    explicit CPal(const Theme& t) : Pal(t) {
        if (t.highContrast) {
            zebra = panel; rule = border; accInk = warnInk = badInk = goodInk = barAccInk = text; dim = GetSysColor(COLOR_GRAYTEXT);
            selFill = GetSysColor(COLOR_HIGHLIGHT); selInk = GetSysColor(COLOR_HIGHLIGHTTEXT);
            return;
        }
        zebra = Mix(panel, hover, 0.55);
        rule = Ink(border, panel, bg, mute, 3.0);
        accInk = Ink(acc, panel, zebra, text, 4.5);
        warnInk = Ink(warn, panel, zebra, text, 4.5);
        badInk = Ink(bad, panel, zebra, text, 4.5);
        goodInk = Ink(good, panel, zebra, text, 4.5);
        barAccInk = Ink(acc, hover, hover, text, 4.5);
        dim = faint;   // disabled controls only (WCAG exempts inactive components)
        InverseVideo(acc, &selFill, &selInk);
    }
};

std::wstring ShortStart(const StartCfg& c) {
    if (c.Delayed()) return L"auto-delay";
    switch (c.start) {
        case 0: return L"boot";
        case 1: return L"system";
        case 2: return L"auto";
        case 3: return L"manual";
        case 4: return L"disabled";
    }
    return L"start " + std::to_wstring(c.start);
}
std::wstring ShortState(DWORD st) {
    switch (st) {
        case SERVICE_STOPPED: return L"stopped";
        case SERVICE_START_PENDING: return L"starting";
        case SERVICE_STOP_PENDING: return L"stopping";
        case SERVICE_RUNNING: return L"running";
        case SERVICE_CONTINUE_PENDING: return L"resuming";
        case SERVICE_PAUSE_PENDING: return L"pausing";
        case SERVICE_PAUSED: return L"paused";
    }
    return L"state " + std::to_wstring(st);
}
std::wstring ShortAccount(const std::wstring& a) {
    const std::wstring nt = L"nt authority\\";
    if (Lower(a.substr(0, nt.size())) == nt) return a.substr(nt.size());
    return a;
}
std::wstring ShortBasis(const std::wstring& b) { return b == L"owner-measured" ? L"measured" : b; }
// The image column: a shared svchost shows as "svchost -k group" (the group is what tells them apart); any other
// image keeps its full path, shortened in the middle by DT_PATH_ELLIPSIS so the file name stays readable.
std::wstring ConsoleImage(const SvcItem& s, const std::wstring& windir) {
    ImageInfo im = NormalizeImage(s.image, windir, s.name, IsDriverType(s.type));
    size_t slash = im.path.find_last_of(L"\\/");
    std::wstring file = slash == std::wstring::npos ? im.path : im.path.substr(slash + 1);
    if (IEq(file, L"svchost.exe")) return L"svchost " + im.args;
    return im.path + (im.args.empty() ? L"" : L" " + im.args);
}

// Detail document into two columns: identity, warnings and configuration on the left; Hadron's KB text
// (what it does, presets, basis) on the right. Title and subtitle go to the pane's header band instead.
// Colours are moved to the console inks: the documents are built with the raw accent and semantic colours,
// which miss 4.5:1 as text in some themes (accent on dark, warn on light/boring).
void SplitDoc(const std::vector<Para>& d, const CPal& c, std::vector<Para>* left, std::vector<Para>* right) {
    std::vector<Para>* cur = left;
    for (Para p : d) {
        if (p.kind == P_TITLE || p.kind == P_SUB) continue;
        if (p.kind == P_HEAD) {
            cur = (p.text.rfind(L"Configuration", 0) == 0 || p.text.rfind(L"From the service", 0) == 0) ? left : right;
            if (p.color == kNoColor) p.color = c.accInk;
        }
        if (p.color == c.acc) p.color = c.accInk;
        else if (p.color == c.warn) p.color = c.warnInk;
        else if (p.color == c.bad) p.color = c.badInk;
        else if (p.color == c.good) p.color = c.goodInk;
        cur->push_back(p);
    }
    if (left->empty()) left->swap(*right);
}
}  // namespace

int ServicesModule::ConButton(HDC dc, int x, int y, const std::wstring& label, int id, bool on, bool enabled, bool primary) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int h = Sc(18);
    int w = TextW(dc, label, f.caption) + Sc(14);
    RECT r{x, y, x + w, y + h};
    bool hot = enabled && hoverId_ == id;
    COLORREF fill, ink, edge;
    if (primary && enabled) { fill = hot ? Mix(p.selFill, p.selInk, 0.12) : p.selFill; ink = p.selInk; edge = fill; }
    else if (on) { fill = Mix(p.panel, p.acc, 0.22); ink = p.text; edge = p.accInk; }
    else if (enabled) { fill = hot ? p.chipHot : p.chip; ink = p.text; edge = p.rule; }
    else { fill = p.panel; ink = p.dim; edge = Mix(p.rule, p.panel, 0.5); }
    FillSolid(dc, r, fill);
    FrameRectC(dc, r, edge);
    ui::DrawText(dc, label, r, f.caption, ink, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    if (enabled && !on) AddClick(r, id);
    return w;
}

std::vector<ServicesModule::FKey> ServicesModule::ConsoleKeys() const {
    std::vector<FKey> k;
    bool busy = applyW_.Running() || loadW_.Running();
    if (mode_ == Mode::Plan) {
        k.push_back({L"F1", conHelp_ ? L"Close" : L"Keys", L"Keys", CA_HELP, true, conHelp_});
        bool ctl = plan_.kind == PlanKind::Control;
        k.push_back({L"Spc", L"Tick", L"Tick", CA_TICK, !ctl && !plan_.rows.empty(), false});
        k.push_back({L"F6", tiOn_ ? L"TI on" : L"TI off", tiOn_ ? L"TI:on" : L"TI:off", CA_TI, !applyW_.Running(), tiOn_});
        if (applyW_.Running()) k.push_back({L"Esc", L"Stop after this item", L"Stop", CA_CANCEL, !cancelApply_.load(), false});
        else k.push_back({L"Esc", L"Cancel", L"Cancel", CA_CANCEL, true, false});
        return k;
    }
    if (conItemBar_) {
        const SvcItem* s = Selected();
        std::wstring why;
        bool can = CanApply(&why);
        bool drv = s && IsDriverType(s->type);
        bool en = false, cen = false, running = false, stoppable = false;
        if (s) {
            Guard g = CheckGuard(*s, ctx_);
            en = can && !g.neverTouch && s->kind != Kind::UserInstance && s->cfg.start >= 2;
            cen = can && s->kind != Kind::UserTemplate && !g.neverTouch;   // R5
            running = s->state == SERVICE_RUNNING;
            stoppable = running && (s->controls & SERVICE_ACCEPT_STOP);
        }
        auto on = [&](int i) {
            if (!s) return false;
            return i == 0 ? (s->cfg.start == 2 && !s->cfg.Delayed()) : i == 1 ? s->cfg.Delayed() : s->cfg.start == i + 1;
        };
        static const wchar_t* const kSet[] = {L"Auto", L"Delayed", L"Manual", L"Disabled"};
        for (int i = 0; i < 4; i++) {
            if (i == 1 && drv) continue;
            k.push_back({L"F" + std::to_wstring(i + 1), kSet[i], kSet[i], CA_SET + i, en && !on(i), on(i)});
        }
        if (s && !drv) {
            k.push_back({L"F5", L"Start", L"Start", CA_CTL + 0, cen && !running && s->cfg.start != 4, false});
            k.push_back({L"F6", L"Stop", L"Stop", CA_CTL + 1, cen && stoppable, false});
            k.push_back({L"F7", L"Restart", L"Restart", CA_CTL + 2, cen && stoppable, false});
        }
        k.push_back({L"Esc", L"Back", L"Back", CA_BACK, true, false});
        return k;
    }
    bool canPlan = loaded_ && !applyW_.Running() && !items_.empty();
    std::wstring dummy;
    bool canApply = CanApply(&dummy);
    k.push_back({L"F1", conHelp_ ? L"Close" : L"Keys", L"Keys", CA_HELP, true, conHelp_});
    k.push_back({L"F2", tab_ == TAB_SERVICES ? L"Drivers" : L"Services", L"Tab", CA_TAB, true, false});
    k.push_back({L"F3", L"Filter", L"Find", CA_FILTER, true, focus_ == Focus::Search});
    k.push_back({L"F4", L"Item", L"Item", CA_ITEM, Selected() != nullptr, false});
    k.push_back({L"F5", L"Refresh", L"Rfsh", CA_REFRESH, !fixture_ && !busy, false});
    k.push_back({L"F6", tiOn_ ? L"TI on" : L"TI off", tiOn_ ? L"TI:on" : L"TI:off", CA_TI, true, tiOn_});
    k.push_back({L"F7", L"Snapshot", L"Snap", CA_PRESET + 0, canPlan, false});
    k.push_back({L"F8", L"Stock", L"Stock", CA_PRESET + 1, canPlan, false});
    k.push_back({L"F9", L"Bloat", L"Bloat", CA_PRESET + 2, canPlan && kbOk_, false});
    k.push_back({L"F10", L"Stripped", L"Strip", CA_PRESET + 3, canPlan && kbOk_, false});
    k.push_back({L"F11", L"Minimum", L"Min", CA_PRESET + 4, canPlan && kbOk_, false});
    k.push_back({L"^Z", L"Undo last", L"Undo", CA_UNDO, undoable_ >= 0 && canApply, false});
    return k;
}

void ServicesModule::PaintFKeyBar(HDC dc, const RECT& r) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    FillSolid(dc, r, p.hover);
    HLine(dc, r.left, r.right, r.top, p.rule);
    std::vector<FKey> keys = ConsoleKeys();
    const int gap = Sc(4), pad = Sc(8);
    std::wstring tag = mode_ == Mode::Plan ? L"PLAN" : conItemBar_ ? L"ITEM" : L"LIST";
    std::wstring hint;
    if (mode_ == Mode::Plan) hint = L"No key applies: click Apply";
    else if (conItemBar_) {
        std::wstring why;
        if (!CanApply(&why)) hint = why;
        else if (const SvcItem* s = Selected()) hint = L"opens a plan for " + s->name;
    }
    auto cellW = [&](const FKey& k, bool shortL) {
        return TextW(dc, k.cap, f.semi) + Sc(5) + TextW(dc, shortL ? k.shortLabel : k.label, f.mono) + Sc(10);
    };
    int avail = r.right - r.left - 2 * pad - TextW(dc, tag, f.semi) - Sc(12);
    int full = 0;
    for (auto& k : keys) full += cellW(k, false) + gap;
    bool shortL = full > avail;
    int x = r.left + pad;
    for (auto& k : keys) {
        int w = cellW(k, shortL);
        RECT cell{x, r.top + 1, x + w, r.bottom};
        int id = ID_CONKEY + k.act;
        if (k.enabled && hoverId_ == id) FillSolid(dc, cell, p.chipHot);
        if (k.on) { RECT u{cell.left, cell.bottom - Sc(2), cell.right, cell.bottom}; FillSolid(dc, u, p.acc); }
        RECT cr{cell.left + Sc(5), cell.top, cell.right, cell.bottom};
        TextLeft(dc, k.cap, cr, f.semi, k.enabled ? p.barAccInk : p.dim);
        cr.left += TextW(dc, k.cap, f.semi) + Sc(5);
        TextLeft(dc, shortL ? k.shortLabel : k.label, cr, f.mono, k.enabled ? p.text : p.dim);
        if (k.enabled) AddClick(cell, id);
        x += w + gap;
    }
    // Right end: the mode, and a hint while it fits.
    int tw = TextW(dc, tag, f.semi);
    RECT tr{r.right - pad - tw, r.top + 1, r.right - pad, r.bottom};
    TextLeft(dc, tag, tr, f.semi, p.mute);
    if (!hint.empty()) {
        int hw = TextW(dc, hint, f.caption);
        if (x + Sc(12) + hw < tr.left - Sc(12)) {
            RECT hr{tr.left - Sc(12) - hw, r.top + 1, tr.left - Sc(12), r.bottom};
            TextLeft(dc, hint, hr, f.caption, p.mute);
        }
    }
}

int ServicesModule::PaintConsoleBanners(HDC dc, const RECT& rc, int y0) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int pad = Sc(8);
    int y = y0;
    for (auto& b : Banners()) {
        COLORREF tone = b.tone == 2 ? p.bad : (b.tone == 1 ? p.warn : p.acc);
        const wchar_t* tagText = b.tone == 2 ? L"FAIL" : (b.tone == 1 ? L"WARN" : L"INFO");
        int tagW = TextW(dc, L"WARN", f.semi) + Sc(10);
        int actionsW = 0;
        for (auto& a : b.actions) actionsW += TextW(dc, a.second, f.caption) + Sc(14) + Sc(6);
        int right = rc.right - pad;
        RECT tr{pad + tagW + Sc(8), 0, right - Sc(6) - actionsW, 0};
        int lh = 0;
        std::vector<std::wstring> lines = WrapLines(dc, f.caption, b.text, tr.right - tr.left, &lh);
        if (lines.size() > 3) { lines.resize(3); lines[2] += L"\u2026"; }
        int h = std::max(Sc(20), (int)lines.size() * lh + Sc(4));
        RECT r{pad, y, right, y + h};
        FillSolid(dc, r, Mix(p.panel, tone, 0.10));
        RECT tagR{r.left, r.top, r.left + tagW, r.bottom};
        COLORREF tagFill = tone, tagInk = p.text;
        if (!host_->Theme().highContrast) InverseVideo(tone, &tagFill, &tagInk);
        else { tagFill = p.selFill; tagInk = p.selInk; }
        FillSolid(dc, tagR, tagFill);
        ui::DrawText(dc, tagText, tagR, f.semi, tagInk, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        int ty = y + (h - (int)lines.size() * lh) / 2;
        for (auto& ln : lines) {
            RECT lr{tr.left, ty, tr.right, ty + lh};
            ui::DrawText(dc, ln, lr, f.caption, p.text, DT_SINGLELINE | DT_NOCLIP);
            ty += lh;
        }
        int ax = right - Sc(4);
        for (auto it = b.actions.rbegin(); it != b.actions.rend(); ++it) {
            int bw = TextW(dc, it->second, f.caption) + Sc(14);
            ax -= bw;
            ConButton(dc, ax, y + (h - Sc(18)) / 2, it->second, it->first, false, true, it->first != ID_DISMISS);
            ax -= Sc(6);
        }
        y += h + Sc(3);
    }
    return y == y0 ? y : y + Sc(3);
}

void ServicesModule::PaintConsole(HDC dc, const RECT& rc) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    FillSolid(dc, rc, p.bg);
    if (conWindir_.empty()) {
        wchar_t buf[MAX_PATH];
        UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
        conWindir_ = (n && n < MAX_PATH) ? std::wstring(buf, n) : L"C:\\Windows";
    }
    int pad = Sc(8);
    RECT bar{rc.left, rc.bottom - Sc(22), rc.right, rc.bottom};
    PaintFKeyBar(dc, bar);

    // Header: title on the left, source facts on the right.
    RECT hr{pad, Sc(4), rc.right - pad, Sc(24)};
    TextLeft(dc, kAppTitle, hr, f.semi, p.text);
    int tw = TextW(dc, kAppTitle, f.semi);
    std::wstring sub;
    if (!loaded_) sub = L"reading services and drivers\u2026";
    else {
        int nSvc = 0, nDrv = 0;
        for (auto& s : items_) (IsDriverType(s.type) ? nDrv : nSvc)++;
        sub = (fixture_ ? L"fixture" : L"this PC") + std::wstring(L" \u00B7 build ") + (build_.empty() ? L"?" : build_) + L" \u00B7 " +
              std::to_wstring(nSvc) + L" svc \u00B7 " + std::to_wstring(nDrv) + L" drv";
        if (kbOk_) sub += L" \u00B7 KB " + std::to_wstring(kb_.entries.size()) + L" (build " + kb_.build + L")";
        if (!fixture_ && store_.Valid()) sub += L" \u00B7 data " + sys::ToShell(store_.Root());
    }
    RECT sr{hr.left + tw + Sc(16), hr.top, hr.right, hr.bottom};
    ui::DrawText(dc, sub, sr, f.mono, p.mute, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_PATH_ELLIPSIS);
    HLine(dc, pad, rc.right - pad, Sc(27), p.rule);
    int y = PaintConsoleBanners(dc, rc, Sc(32));
    int bottom = bar.top - Sc(6);
    if (mode_ == Mode::Plan) PaintConsolePlan(dc, rc, y, bottom);
    else PaintConsoleList(dc, rc, y, bottom);
}

std::vector<ServicesModule::ConCol> ServicesModule::ConsoleListColumns(HDC dc, int widthPx) {
    const int cw = TextW(dc, L"0", host_->Fonts().mono);
    struct D { const wchar_t* key; const wchar_t* title; int ch; int drop; bool path; };   // ch 0 = flexible
    static const D kSvc[] = {{L"name", L"Name", 22, 0, false}, {L"state", L"State", 9, 0, false}, {L"start", L"Start", 14, 0, false},
                             {L"changed", L"vs 1st", 8, 3, false}, {L"kb", L"Hadron", 10, 0, false}, {L"basis", L"Basis", 10, 2, false},
                             {L"account", L"Account", 15, 1, false}, {L"display", L"Display name", 0, 0, false},
                             {L"image", L"Image path", 0, 4, true}};
    static const D kDrv[] = {{L"name", L"Name", 22, 0, false}, {L"state", L"State", 9, 0, false}, {L"start", L"Start", 14, 0, false},
                             {L"changed", L"vs 1st", 8, 3, false}, {L"kb", L"Hadron", 10, 0, false}, {L"basis", L"Basis", 10, 2, false},
                             {L"type", L"Type", 7, 1, false}, {L"group", L"Load group", 15, 1, false},
                             {L"display", L"Display name", 0, 0, false}, {L"image", L"Image path", 0, 4, true}};
    const D* all = tab_ == TAB_SERVICES ? kSvc : kDrv;
    const size_t n = tab_ == TAB_SERVICES ? sizeof(kSvc) / sizeof(kSvc[0]) : sizeof(kDrv) / sizeof(kDrv[0]);
    const int flexMin = 18 * cw;
    std::vector<bool> on(n, true);
    auto need = [&]() {
        int fixed = 0, flex = 0;
        for (size_t i = 0; i < n; i++) if (on[i]) { if (all[i].ch) fixed += all[i].ch * cw; else flex++; }
        return fixed + flex * flexMin;
    };
    for (int rank = 1; rank <= 4 && need() > widthPx; rank++)
        for (size_t i = 0; i < n; i++) if (all[i].drop == rank) on[i] = false;
    int fixed = 0, flex = 0;
    for (size_t i = 0; i < n; i++) if (on[i]) { if (all[i].ch) fixed += all[i].ch * cw; else flex++; }
    int spare = std::max(0, widthPx - fixed);
    std::vector<ConCol> cols;
    std::vector<std::wstring> keys;
    int flexDone = 0;
    for (size_t i = 0; i < n; i++) {
        if (!on[i]) continue;
        int w = all[i].ch * cw;
        if (!all[i].ch) {   // display name gets 45% of the spare room, the image path the rest
            w = flex == 1 ? spare : (flexDone == 0 ? spare * 45 / 100 : spare - spare * 45 / 100);
            w = std::max(w, flexMin);
            flexDone++;
        }
        cols.push_back({all[i].key, all[i].title, w, all[i].path});
        keys.push_back(all[i].key);
    }
    if (keys != colKeys_) { colKeys_ = keys; Refilter(); }
    return cols;
}

void ServicesModule::ConsoleListCells(int row, std::vector<ConCell>& out) {
    CPal p(host_->Theme());
    const SvcItem& s = items_[V_[row]];
    for (auto& k : colKeys_) {
        if (k == L"name") out.push_back({s.name, p.text});
        else if (k == L"display") out.push_back({s.display, p.mute});
        else if (k == L"state")
            out.push_back({ShortState(s.state), s.state == SERVICE_RUNNING ? p.goodInk : s.state == SERVICE_STOPPED ? p.mute : p.warnInk});
        else if (k == L"start") {
            bool drift = drift_.count(Lower(s.name)) != 0;
            out.push_back({ShortStart(s.cfg) + (s.trigger ? L"+t" : L"") + (drift ? L" *" : L""), drift ? p.warnInk : p.text});
        } else if (k == L"changed") {
            std::wstring c = ChangedText(s);
            out.push_back({c, c == L"new" ? p.accInk : p.warnInk});
        } else if (k == L"kb") {
            std::wstring c = KbVerdict(s).label;   // same verdict as Baseline's column and the detail pane
            out.push_back({c, c == L"protected" ? p.badInk : (c == L"no preset" || c == L"KB" || c == L"\u2014") ? p.mute : p.text});
        } else if (k == L"basis") {
            std::wstring b = BasisShort(s);
            out.push_back({b, b == L"folklore" ? p.warnInk : p.mute});
        } else if (k == L"account") out.push_back({ShortAccount(s.account), p.mute});
        else if (k == L"group") out.push_back({s.group, p.mute});
        else if (k == L"type") out.push_back({s.kind == Kind::FsDriver ? L"fs" : s.kind == Kind::KernelDriver ? L"kernel" : L"other", p.mute});
        else if (k == L"image") out.push_back({ConsoleImage(s, conWindir_), p.mute}); else out.push_back({L"", p.mute});
    }
}

void ServicesModule::PaintConsoleGrid(HDC dc, ConGrid& g, const RECT& rc, const std::vector<ConCol>& cols, int n, int sel,
                                      int sortCol, bool asc, const std::function<void(int, std::vector<ConCell>&)>& cellsOf) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = PCtx();
    g.rc = rc;
    g.rowH = Sc(18);
    const int headH = Sc(20), inset = Sc(6), sbW = Sc(9);
    FillSolid(dc, rc, p.panel);
    RECT body{rc.left + 1, rc.top + 1 + headH, rc.right - 1, rc.bottom - 1};
    bool hasSb = n * g.rowH > body.bottom - body.top;
    if (hasSb) body.right -= sbW;
    g.body = body;
    g.track = RECT{body.right, body.top, rc.right - 1, body.bottom};
    g.sb.Configure(body.bottom - body.top, n * g.rowH);
    if (sel != g.lastSel) {
        // First show (--select, a new plan): centre the row; later moves scroll only as far as needed.
        if (g.lastSel == -2 && sel >= 0) g.sb.SetOffset(std::max(0, sel - g.Rows() / 2) * g.rowH);
        else g.Ensure(sel);
        g.lastSel = sel;
    }

    // Header: bold titles, the sorted one in the accent with a small triangle right after its text (never over it).
    RECT hdr{rc.left + 1, rc.top + 1, rc.right - 1, rc.top + 1 + headH};
    FillSolid(dc, hdr, p.hover);
    HLine(dc, hdr.left, hdr.right, hdr.bottom - 1, p.rule);
    g.colL.clear(); g.colR.clear();
    int x = body.left + inset;
    const int tri = Sc(7), gapCh = TextW(dc, L"0", f.mono);
    for (int i = 0; i < (int)cols.size(); i++) {
        int w = cols[i].w;
        int right = std::min<int>(x + w, body.right);
        g.colL.push_back(x - (i == 0 ? inset : 0));
        g.colR.push_back(right);
        bool sorted = i == sortCol;
        RECT tr{x, hdr.top, right - gapCh, hdr.bottom};
        int tw = TextW(dc, cols[i].title, f.semi);
        if (sorted) tr.right = std::max<int>(tr.left, std::min<int>(tr.right, right - gapCh - tri - Sc(4)));
        TextLeft(dc, cols[i].title, tr, f.semi, sorted ? p.barAccInk : p.mute);
        if (sorted) {
            int ax = std::min<int>(x + tw + Sc(5), right - gapCh - tri);
            int ay = hdr.top + (headH - Sc(5)) / 2;
            POINT pt[3];
            if (asc) { pt[0] = {ax, ay + Sc(5)}; pt[1] = {ax + tri, ay + Sc(5)}; pt[2] = {ax + tri / 2, ay}; }
            else { pt[0] = {ax, ay}; pt[1] = {ax + tri, ay}; pt[2] = {ax + tri / 2, ay + Sc(5)}; }
            HBRUSH b = CreateSolidBrush(p.barAccInk);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, pt, 3);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
        }
        x += w;
    }

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
    int first = g.sb.Offset() / g.rowH;
    int y0 = body.top - g.sb.Offset() % g.rowH;
    std::vector<ConCell> cells;
    bool listFocus = focus_ == Focus::List;
    for (int i = first; i < n; i++) {
        int ry = y0 + (i - first) * g.rowH;
        if (ry >= body.bottom) break;
        RECT row{body.left, ry, body.right, ry + g.rowH};
        bool selected = i == sel;
        COLORREF fill = i % 2 ? p.zebra : p.panel;
        if (selected) fill = listFocus ? p.selFill : Mix(p.panel, p.acc, 0.22);
        FillSolid(dc, row, fill);
        if (selected && !listFocus) { RECT bar{row.left, row.top, row.left + Sc(3), row.bottom}; FillSolid(dc, bar, p.acc); }
        cells.clear();
        cellsOf(i, cells);
        int cx = body.left + inset;
        for (size_t ci = 0; ci < cols.size() && ci < cells.size(); ci++) {
            RECT cr{cx, row.top, std::min<int>(cx + cols[ci].w - gapCh, body.right - Sc(2)), row.bottom};
            COLORREF col = selected && listFocus ? p.selInk : (selected ? p.text : cells[ci].color);
            if (cr.right > cr.left)
                ui::DrawText(dc, cells[ci].text, cr, f.mono, col,
                             DT_SINGLELINE | DT_VCENTER | DT_LEFT | (cols[ci].path && cells[ci].text.find(L'\\') != std::wstring::npos ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
            cx += cols[ci].w;
        }
        if (selected && listFocus && GetFocus() == hwnd_) {
            RECT fr{row.left + 1, row.top, row.right - 1, row.bottom};
            FocusRectC(dc, fr, p.selInk);   // in the selection's own ink: an accent focus rect on an accent fill vanishes
        }
    }
    RestoreDC(dc, saved);
    if (hasSb) g.sb.Paint(dc, g.track, c);
    FrameRectC(dc, rc, p.rule);
}

void ServicesModule::PaintConsoleList(HDC dc, const RECT& rc, int y, int bottom) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = PCtx();
    int pad = Sc(8);
    const int lineH = Sc(20);

    // Tabs, the TrustedInstaller switch and its scope note, then the filter on the right.
    int nSvc = 0, nDrv = 0;
    for (auto& s : items_) (IsDriverType(s.type) ? nDrv : nSvc)++;
    int x = pad;
    auto tab = [&](const std::wstring& label, int id, bool on) {
        int w = TextW(dc, label, f.mono) + Sc(16);
        RECT r{x, y, x + w, y + lineH};
        if (on) FillSolid(dc, r, p.selFill);
        else if (hoverId_ == id) FillSolid(dc, r, p.chipHot);
        else FrameRectC(dc, r, p.rule);
        ui::DrawText(dc, label, r, f.mono, on ? p.selInk : p.text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        if (!on) AddClick(r, id);
        x += w + Sc(4);
    };
    tab(L"Services " + std::to_wstring(nSvc), ID_TAB_SVC, tab_ == TAB_SERVICES);
    tab(L"Drivers " + std::to_wstring(nDrv), ID_TAB_DRV, tab_ == TAB_DRIVERS);
    x += Sc(10);
    {
        std::wstring lbl = tiOn_ ? L"TrustedInstaller ON" : L"TrustedInstaller off";
        int w = TextW(dc, lbl, f.mono) + Sc(16);
        RECT r{x, y, x + w, y + lineH};
        FillSolid(dc, r, tiOn_ ? Mix(p.panel, p.warn, 0.22) : (hoverId_ == ID_TI ? p.chipHot : p.panel));
        FrameRectC(dc, r, tiOn_ ? p.warnInk : p.rule);
        ui::DrawText(dc, lbl, r, f.mono, tiOn_ ? p.text : p.mute, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        AddClick(r, ID_TI);
        x += w + Sc(8);
    }
    int sw = std::min(Sc(300), std::max(Sc(160), (int)(rc.right - pad - x) / 3));
    rcSearch_ = RECT{rc.right - pad - sw, y, rc.right - pad, y + lineH};
    {
        // The note is drawn only where it fits whole: a clipped note about what TI can't do would mislead.
        static const wchar_t* const kCaps[] = {
            L"Only for writes Windows refuses to administrators. Not Defender, PPL or Tamper Protection.",
            L"Only for writes Windows refuses to administrators.", L"For admin-refused writes."};
        int room = rcSearch_.left - Sc(10) - x;
        for (const wchar_t* cap : kCaps) {
            int need = TextW(dc, cap, f.caption) * 105 / 100;
            if (need <= room) {
                RECT tr{x, y, x + need + Sc(8), y + lineH};
                ui::DrawText(dc, cap, tr, f.caption, p.mute, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOCLIP);
                break;
            }
        }
    }
    search_.Paint(dc, rcSearch_, c, focus_ == Focus::Search && GetFocus() == hwnd_);
    if (focus_ == Focus::Search) FrameRectC(dc, rcSearch_, p.accInk);
    AddClick(rcSearch_, ID_SEARCH);
    y += lineH + Sc(6);

    // Grid on top, the detail pane under it (about 40%: the KB text wants width more than height).
    int avail = bottom - y;
    int detH = std::max(Sc(150), avail * 40 / 100);
    if (avail - detH < Sc(120)) detH = std::max(Sc(80), avail - Sc(120));
    RECT grid{pad, y, rc.right - pad, bottom - detH - Sc(6)};
    RECT det{pad, grid.bottom + Sc(6), rc.right - pad, bottom};
    rcList_ = grid;
    std::vector<ConCol> cols = ConsoleListColumns(dc, grid.right - grid.left - Sc(9) - Sc(8));
    if (!loaded_ || !loadErr_.empty() || items_.empty()) {
        FillSolid(dc, grid, p.panel);
        FrameRectC(dc, grid, p.rule);
        std::wstring t = !loadErr_.empty() ? loadErr_ : !loaded_ ? L"Reading services and drivers\u2026" : L"No data.";
        RECT tr = grid;
        InflateRect(&tr, -Sc(16), -Sc(16));
        ui::DrawText(dc, t, tr, f.mono, loadErr_.empty() ? p.mute : p.badInk, DT_WORDBREAK);
    } else {
        bool shownAsc = true;
        std::wstring shownKey = SortKeyShown(&shownAsc);
        int sortCol = -1;
        for (int i = 0; i < (int)cols.size(); i++) if (cols[i].key == shownKey) sortCol = i;
        PaintConsoleGrid(dc, conList_, grid, cols, (int)V_.size(), list_.Selected(), sortCol, shownAsc,
                         [this](int row, std::vector<ConCell>& out) { ConsoleListCells(row, out); });
        if (V_.empty()) {
            RECT tr{grid.left, grid.top + Sc(40), grid.right, grid.top + Sc(64)};
            ui::DrawText(dc, L"Nothing matches the filter (Esc clears it).", tr, f.mono, p.mute, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
    }

    if (conHelp_) { PaintConsoleHelp(dc, det); return; }
    const SvcItem* s = Selected();
    if (!s) {
        FillSolid(dc, det, p.panel);
        FrameRectC(dc, det, p.rule);
        rcDetail_ = det;
        conLeftRc_ = RECT{};
        RECT tr = det;
        InflateRect(&tr, -Sc(12), -Sc(10));
        ui::DrawText(dc, L"Select an item (Up/Down) to see what it does, why a preset would change it, and its configuration.",
                     tr, f.mono, p.mute, DT_WORDBREAK);
        return;
    }
    std::wstring info = (s->display.empty() || s->display == s->name ? L"" : s->display + L"  \u00B7  ") + TypeName(s->type) +
                        L"  \u00B7  " + StateName(s->state);
    PaintConsoleDetail(dc, det, s->name, info, ItemDoc(*s), true);
}

void ServicesModule::PaintConsoleDetail(HDC dc, const RECT& det, const std::wstring& name, const std::wstring& info,
                                        const std::vector<Para>& doc, bool actions) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = PCtx();
    FillSolid(dc, det, p.panel);
    // Band: the item's ID in bold, then what it is.
    RECT band{det.left + 1, det.top + 1, det.right - 1, det.top + 1 + Sc(22)};
    FillSolid(dc, band, p.hover);
    RECT nr{band.left + Sc(8), band.top, band.right - Sc(8), band.bottom};
    TextLeft(dc, name, nr, f.semi, p.text);
    nr.left += TextW(dc, name, f.semi) + Sc(12);
    TextLeft(dc, info, nr, f.mono, p.mute);
    int top = band.bottom;

    // Actions: the same start-type and control buttons as Baseline (each opens a plan), with F4 for the keys.
    if (actions) {
        const SvcItem* s = Selected();
        std::wstring why;
        bool can = CanApply(&why);
        Guard g = CheckGuard(*s, ctx_);
        bool drv = IsDriverType(s->type);
        bool en = can && !g.neverTouch && s->kind != Kind::UserInstance && s->cfg.start >= 2;
        int ay = top + Sc(4);
        int ax = det.left + Sc(8);
        RECT lr{ax, ay, ax + Sc(200), ay + Sc(18)};
        TextLeft(dc, L"start", lr, f.mono, p.mute);
        ax += TextW(dc, L"start ", f.mono);
        ax += ConButton(dc, ax, ay, L"Auto", ID_SET + 2, s->cfg.start == 2 && !s->cfg.Delayed(), en) + Sc(3);
        if (!drv) ax += ConButton(dc, ax, ay, L"Auto (delayed)", ID_SET + 5, s->cfg.Delayed(), en) + Sc(3);
        ax += ConButton(dc, ax, ay, L"Manual", ID_SET + 3, s->cfg.start == 3, en) + Sc(3);
        ax += ConButton(dc, ax, ay, L"Disabled", ID_SET + 4, s->cfg.start == 4, en) + Sc(14);
        if (!drv) {
            bool running = s->state == SERVICE_RUNNING;
            bool stoppable = running && (s->controls & SERVICE_ACCEPT_STOP);
            bool cen = can && s->kind != Kind::UserTemplate && !g.neverTouch;   // R5: never-touch: no start/stop either
            lr = RECT{ax, ay, ax + Sc(100), ay + Sc(18)};
            TextLeft(dc, L"now", lr, f.mono, p.mute);
            ax += TextW(dc, L"now ", f.mono);
            ax += ConButton(dc, ax, ay, L"Start", ID_CTL + 0, false, cen && !running && s->cfg.start != 4) + Sc(3);
            ax += ConButton(dc, ax, ay, L"Stop", ID_CTL + 1, false, cen && stoppable) + Sc(3);
            ax += ConButton(dc, ax, ay, L"Restart", ID_CTL + 2, false, cen && stoppable) + Sc(14);
        }
        RECT wr{ax, ay, det.right - Sc(8), ay + Sc(18)};
        TextLeft(dc, can ? std::wstring(L"F4: keys for these; each opens a plan") : why, wr, f.caption, p.mute);
        top = ay + Sc(18) + Sc(4);
    }
    HLine(dc, det.left + 1, det.right - 1, top, p.rule);
    top += 1;

    std::vector<Para> left, right;
    SplitDoc(doc, p, &left, &right);
    const int sbW = Sc(9);
    int mid = right.empty() ? det.right : det.left + (det.right - det.left) * 44 / 100;
    auto column = [&](ScrollBar& sb, const RECT& area, const std::vector<Para>& d, RECT* track) {
        RECT body{area.left + Sc(10), area.top + Sc(4), area.right - Sc(8) - sbW, area.bottom - Sc(6)};
        int total = PaintDoc(dc, body, d, 0, false);
        sb.Configure(body.bottom - body.top, total);
        *track = RECT{area.right - sbW - 1, area.top + 1, area.right - 1, area.bottom - 1};
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
        PaintDoc(dc, body, d, sb.Offset(), true);
        RestoreDC(dc, saved);
        if (sb.Visible()) sb.Paint(dc, *track, c);
    };
    RECT la{det.left, top, mid, det.bottom};
    conLeftRc_ = la;
    column(conLeftSb_, la, left, &conLeftTrack_);
    if (!right.empty()) {
        VLine(dc, mid, top + Sc(4), det.bottom - Sc(4), p.rule);
        RECT ra{mid + 1, top, det.right, det.bottom};
        rcDetail_ = ra;
        column(detSb_, ra, right, &rcDetSb_);
    } else {
        rcDetail_ = RECT{};
        rcDetSb_ = RECT{};
    }
    FrameRectC(dc, det, p.rule);
    if (focus_ == Focus::Detail) {
        RECT fr = right.empty() ? la : rcDetail_;
        InflateRect(&fr, -2, -2);
        if (GetFocus() == hwnd_) FocusRectC(dc, fr, p.accInk);
        else FrameRectC(dc, fr, Mix(p.panel, p.acc, 0.4));
    }
}

void ServicesModule::PaintConsoleHelp(HDC dc, const RECT& det) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    FillSolid(dc, det, p.panel);
    FrameRectC(dc, det, p.rule);
    rcDetail_ = RECT{};
    conLeftRc_ = RECT{};
    RECT band{det.left + 1, det.top + 1, det.right - 1, det.top + 1 + Sc(22)};
    FillSolid(dc, band, p.hover);
    RECT tr{band.left + Sc(8), band.top, band.right - Sc(8), band.bottom};
    TextLeft(dc, L"Keys", tr, f.semi, p.text);
    tr.left += TextW(dc, L"Keys", f.semi) + Sc(12);
    TextLeft(dc, L"F1 or Esc closes. Every key changes the view or opens a plan; no key applies one: Apply is a click on the plan.",
             tr, f.caption, p.mute);
    typedef std::pair<const wchar_t*, const wchar_t*> KV;
    static const KV kList[] = {
        {L"F2", L"Services / Drivers tab"}, {L"F3", L"Filter (or just type); Esc clears it"},
        {L"F4", L"Item keys for the selected row: start type, start/stop"}, {L"F5", L"Refresh (live data only)"},
        {L"F6", L"TrustedInstaller toggle, this session only"}, {L"F7", L"Plan: Default, first snapshot"},
        {L"F8", L"Plan: Default, Windows stock"}, {L"F9", L"Plan: Pure bloat removal"}, {L"F10", L"Plan: Stripped"},
        {L"F11", L"Plan: Minimum"}, {L"Ctrl+Z", L"Plan: undo the last apply"},
        {L"Up Down", L"Move; PgUp PgDn Home End"}, {L"Tab", L"List, filter, detail"}};
    static const KV kPlan[] = {
        {L"Space", L"Tick or untick the row (or click [x])"}, {L"F6", L"TrustedInstaller toggle; rebuilds the plan"},
        {L"Esc", L"Cancel the plan"}, {L"Enter", L"Nothing: no key applies"}, {L"Up Down", L"Move; PgUp PgDn Home End"},
        {L"Tab", L"List, detail"}};
    static const KV kCols[] = {
        {L"State", L"running, stopped, starting, stopping, paused"},
        {L"Start", L"+t trigger-start; * changed since Hadron set it"},
        {L"vs 1st", L"changed or new against the first-run snapshot"},
        {L"Hadron", L"protected, no preset, or the lowest preset that changes it"},
        {L"Basis", L"published, measured, reasoning or folklore (folklore: no evidence)"}};
    std::vector<KV> a, b;
    if (mode_ == Mode::Plan) a.assign(std::begin(kPlan), std::end(kPlan));
    else a.assign(std::begin(kList), std::end(kList));
    b.assign(std::begin(kCols), std::end(kCols));
    b.push_back({L"Note", L"English only; no screen-reader support in v1."});
    int top = band.bottom + Sc(6);
    int lh = Sc(17);
    int colW = (det.right - det.left - Sc(16)) / 2;
    auto list = [&](const std::vector<KV>& v, int x0, int keyW) {
        int yy = top;
        for (auto& kv : v) {
            if (yy + lh > det.bottom - Sc(4)) break;
            RECT kr{x0, yy, x0 + keyW, yy + lh};
            TextLeft(dc, kv.first, kr, f.semi, p.accInk);
            RECT vr{x0 + keyW, yy, x0 + colW - Sc(10), yy + lh};
            TextLeft(dc, kv.second, vr, f.mono, p.text);
            yy += lh;
        }
    };
    int cw = TextW(dc, L"0", f.mono);
    list(a, det.left + Sc(10), cw * 9);
    VLine(dc, det.left + Sc(8) + colW, top, det.bottom - Sc(6), p.rule);
    list(b, det.left + Sc(18) + colW, cw * 8);
}

std::vector<ServicesModule::ConCol> ServicesModule::ConsolePlanColumns(HDC dc, int widthPx) {
    const int cw = TextW(dc, L"0", host_->Fonts().mono);
    bool ctl = plan_.kind == PlanKind::Control;
    std::vector<ConCol> cols;
    cols.push_back({L"box", L"", 5 * cw, false});
    cols.push_back({L"name", L"Name", 22 * cw, false});
    if (ctl) cols.push_back({L"op", L"Op", 7 * cw, false});
    else {
        cols.push_back({L"now", L"Now", 12 * cw, false});
        cols.push_back({L"after", L"Planned", 13 * cw, false});
        cols.push_back({L"basis", L"Basis", 10 * cw, false});
    }
    int used = 0;
    for (auto& c : cols) used += c.w;
    cols.push_back({L"why", L"Why / protection", std::max(20 * cw, widthPx - used), false});
    conPlanKeys_.clear();
    for (auto& c : cols) conPlanKeys_.push_back(c.key);
    return cols;
}

void ServicesModule::ConsolePlanCells(int row, std::vector<ConCell>& out) {
    CPal p(host_->Theme());
    const PlanRow& r = plan_.rows[row];
    for (auto& k : conPlanKeys_) {
        if (k == L"box") {
            if (!r.blocked.empty()) out.push_back({L"[-]", p.badInk});
            else out.push_back({r.include ? L"[x]" : L"[ ]", r.include ? p.accInk : p.mute});
        } else if (k == L"name") out.push_back({r.name, r.include || !r.blocked.empty() ? p.text : p.mute});
        else if (k == L"op") out.push_back({r.ctl == Op::Stop ? L"stop" : L"start", p.text});
        else if (k == L"now") out.push_back({ShortStart(r.before), p.mute});
        else if (k == L"after") out.push_back({L"\u2192 " + ShortStart(r.after), r.blocked.empty() ? p.text : p.mute});
        else if (k == L"basis") {
            std::wstring b = ShortBasis(r.basis);
            out.push_back({b, b == L"folklore" ? p.warnInk : p.mute});
        } else if (k == L"why") {
            if (!r.blocked.empty()) out.push_back({r.blocked, p.badInk});
            else if (!r.warning.empty()) out.push_back({r.warning, p.warnInk});
            else if (!r.depWarning.empty()) out.push_back({r.depWarning, p.warnInk});
            else out.push_back({r.reason, p.mute});
        } else out.push_back({L"", p.mute});
    }
}

void ServicesModule::ConsoleSortPlan(int col) {
    if (col < 0 || col >= (int)conPlanKeys_.size()) return;
    if (conPlanSort_ == col) conPlanAsc_ = !conPlanAsc_;
    else { conPlanSort_ = col; conPlanAsc_ = true; }
    const std::wstring key = conPlanKeys_[col];
    auto text = [&](const PlanRow& r) -> std::wstring {
        if (key == L"box") return !r.blocked.empty() ? L"2" : (r.include ? L"0" : L"1");
        if (key == L"now") return std::to_wstring(r.before.start) + (r.before.Delayed() ? L"1" : L"0");
        if (key == L"after") return std::to_wstring(r.after.start) + (r.after.Delayed() ? L"1" : L"0");
        if (key == L"op") return r.ctl == Op::Stop ? L"1" : L"0";
        if (key == L"basis") return r.basis;
        if (key == L"why") return Lower(!r.blocked.empty() ? r.blocked : !r.warning.empty() ? r.warning : !r.depWarning.empty() ? r.depWarning : r.reason);
        return Lower(r.name);
    };
    std::wstring keep;
    int sel = plist_.Selected();
    if (sel >= 0 && sel < (int)plan_.rows.size()) keep = plan_.rows[sel].name;
    bool asc = conPlanAsc_;
    std::stable_sort(plan_.rows.begin(), plan_.rows.end(), [&](const PlanRow& a, const PlanRow& b) {
        std::wstring ka = text(a), kb = text(b);
        if (ka == kb) return Lower(a.name) < Lower(b.name);
        return asc ? ka < kb : kb < ka;
    });
    RefreshPlanList();
    for (int k = 0; k < (int)plan_.rows.size(); k++)
        if (plan_.rows[k].name == keep) { plist_.Select(k); conPlan_.lastSel = -2; break; }   // re-shown at the next paint
}

void ServicesModule::PaintConsolePlan(HDC dc, const RECT& rc, int y, int bottom) {
    CPal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int pad = Sc(8);
    std::wstring why;
    bool can = CanApply(&why);
    bool ctl = plan_.kind == PlanKind::Control;
    int n = ctl ? (int)std::count_if(plan_.rows.begin(), plan_.rows.end(), [](const PlanRow& r) { return r.blocked.empty(); })
                : plan_.Included();

    // Title line: a PLAN tag, the plan's name, then Cancel and Apply on the right. Apply is a click only.
    RECT tag{pad, y, pad + TextW(dc, L"PLAN", f.semi) + Sc(12), y + Sc(20)};
    FillSolid(dc, tag, p.selFill);
    ui::DrawText(dc, L"PLAN", tag, f.semi, p.selInk, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    std::wstring applyLbl = ctl ? L"Run" : (n == 1 ? L"Apply 1 change" : L"Apply " + std::to_wstring(n) + L" changes");
    int aw = TextW(dc, applyLbl, f.caption) + Sc(14);
    int cw2 = TextW(dc, L"Cancel (Esc)", f.caption) + Sc(14);
    int bx = rc.right - pad - aw;
    ConButton(dc, bx, y + 1, applyLbl, ID_APPLY, false, can && n > 0, true);
    ConButton(dc, bx - Sc(6) - cw2, y + 1, L"Cancel (Esc)", ID_CANCEL_PLAN, false, !applyW_.Running());
    RECT tr{tag.right + Sc(10), y, bx - Sc(12) - cw2, y + Sc(20)};
    TextLeft(dc, plan_.title, tr, f.semi, p.text);
    if (tiOn_) {
        tr.left += TextW(dc, plan_.title, f.semi) + Sc(14);
        TextLeft(dc, L"TrustedInstaller on", tr, f.mono, p.warnInk);
    }
    y += Sc(24);
    std::wstring sum;
    if (ctl) sum = std::to_wstring(n) + L" operation(s), in this order. Nothing happens until you click Run.";
    else
        sum = std::to_wstring(plan_.Included()) + L" ticked, " + std::to_wstring(plan_.Optional()) + L" unticked, " +
              std::to_wstring(plan_.Blocked()) + L" kept or protected; " + std::to_wstring(plan_.alreadySet) +
              L" already set. Nothing changes until you click Apply. Space or a click on [x] ticks.";
    if (!can) sum += L"  (" + why + L")";
    RECT sr{pad, y, rc.right - pad, y + Sc(16)};
    TextLeft(dc, sum, sr, f.caption, can ? p.mute : p.warnInk);
    y += Sc(18);
    for (auto& note : plan_.notes) {
        int lh = 0;
        std::vector<std::wstring> lines = WrapLines(dc, f.caption, note, rc.right - 2 * pad - Sc(14), &lh);
        if (lines.size() > 2) { lines.resize(2); lines[1] += L"\u2026"; }
        RECT dr{pad, y, pad + Sc(14), y + lh};
        TextLeft(dc, L"\u00B7", dr, f.caption, p.mute);
        for (auto& ln : lines) {
            RECT lr{pad + Sc(14), y, rc.right - pad, y + lh};
            TextLeft(dc, ln, lr, f.caption, p.mute);
            y += lh;
        }
        y += Sc(2);
    }
    y += Sc(4);

    int avail = bottom - y;
    int detH = std::max(Sc(130), avail * 34 / 100);
    if (avail - detH < Sc(120)) detH = std::max(Sc(80), avail - Sc(120));
    RECT grid{pad, y, rc.right - pad, bottom - detH - Sc(6)};
    RECT det{pad, grid.bottom + Sc(6), rc.right - pad, bottom};
    rcPlanList_ = grid;
    std::vector<ConCol> cols = ConsolePlanColumns(dc, grid.right - grid.left - Sc(9) - Sc(8));
    PaintConsoleGrid(dc, conPlan_, grid, cols, (int)plan_.rows.size(), plist_.Selected(), conPlanSort_, conPlanAsc_,
                     [this](int row, std::vector<ConCell>& out) { ConsolePlanCells(row, out); });
    if (plan_.rows.empty()) {
        RECT er{grid.left, grid.top + Sc(40), grid.right, grid.top + Sc(64)};
        ui::DrawText(dc, planBusy_ ? L"Reading the live state\u2026" : L"Nothing to change.", er, f.mono, p.mute,
                     DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    if (conHelp_) { PaintConsoleHelp(dc, det); return; }
    int sel = plist_.Selected();
    if (sel >= 0 && sel < (int)plan_.rows.size()) {
        const PlanRow& r = plan_.rows[sel];
        PaintConsoleDetail(dc, det, r.name, r.display.empty() || r.display == r.name ? L"" : r.display, PlanRowDoc(r), false);
    } else {
        FillSolid(dc, det, p.panel);
        FrameRectC(dc, det, p.rule);
        rcDetail_ = det;
        conLeftRc_ = RECT{};
    }
}

void ServicesModule::ConsoleStatus() {
    if (!host_ || mode_ != Mode::List) return;
    const SvcItem* s = Selected();
    if (!s) return;
    if (conWindir_.empty()) return;
    ImageInfo im = NormalizeImage(s->image, conWindir_, s->name, IsDriverType(s->type));
    std::wstring t = s->name + L"   " + im.path + (im.args.empty() ? L"" : L" " + im.args);
    if (!IsDriverType(s->type) && !s->account.empty()) t += L"   \u00B7   " + s->account;
    Status(t);
}

void ServicesModule::OnConsoleAction(int act) {
    bool plan = mode_ == Mode::Plan;
    if (act != CA_HELP) conHelp_ = false;
    switch (act) {
        case CA_HELP: conHelp_ = !conHelp_; return;
        case CA_TAB: if (!plan) { conItemBar_ = false; OnClickId(tab_ == TAB_SERVICES ? ID_TAB_DRV : ID_TAB_SVC); } return;
        case CA_FILTER: if (!plan) { conItemBar_ = false; focus_ = Focus::Search; search_.SelectAll(); } return;
        case CA_ITEM: if (!plan && Selected()) conItemBar_ = !conItemBar_; return;
        case CA_BACK: conItemBar_ = false; return;
        case CA_REFRESH: if (!plan && !fixture_ && !applyW_.Running() && !loadW_.Running()) StartLoad(); return;
        case CA_TI: if (!applyW_.Running()) OnClickId(ID_TI); return;
        case CA_UNDO: {
            std::wstring why;
            if (!plan && undoable_ >= 0 && CanApply(&why)) OnClickId(ID_UNDO_LAST);   // opens the undo plan; never applies
            return;
        }
        case CA_TICK: if (plan && plan_.kind != PlanKind::Control) TogglePlanRow(plist_.Selected()); return;
        case CA_CANCEL:
            // During an apply, Esc stops it after the current item (the banner's Cancel); cancelling is the safe way.
            if (applyW_.Running()) { if (!cancelApply_.load()) OnClickId(ID_CANCEL_APPLY); }
            else if (plan) ClosePlan();
            return;
    }
    if (act >= CA_PRESET && act < CA_PRESET + 5) {
        if (plan || !loaded_ || applyW_.Running() || items_.empty()) return;
        if (act >= CA_PRESET + 2 && !kbOk_) return;
        conItemBar_ = false;
        OnClickId(ID_PRESET + (act - CA_PRESET));   // opens the plan, exactly like the Baseline chip
        return;
    }
    if (act >= CA_SET && act < CA_CTL + 3) {
        // The same enable rules as the buttons; each opens a single-item plan, never an apply.
        if (plan || !conItemBar_) return;
        for (auto& k : ConsoleKeys()) {
            if (k.act != act || !k.enabled) continue;
            conItemBar_ = false;
            static const int kSetIds[] = {ID_SET + 2, ID_SET + 5, ID_SET + 3, ID_SET + 4};
            OnClickId(act < CA_CTL ? kSetIds[act - CA_SET] : ID_CTL + (act - CA_CTL));
            return;
        }
        return;
    }
}

bool ServicesModule::OnConsoleKey(UINT vk) {
    int act = CA_NONE;
    if (mode_ == Mode::Plan) {
        if (vk == VK_F1) act = CA_HELP;
        else if (vk == VK_F6) act = CA_TI;
    } else if (conItemBar_) {
        // Inside the item bar F1..F7 are the item keys the bar shows (so F1 there is Auto, not help).
        static const int kItem[] = {CA_SET + 0, CA_SET + 1, CA_SET + 2, CA_SET + 3, CA_CTL + 0, CA_CTL + 1, CA_CTL + 2};
        if (vk >= VK_F1 && vk <= VK_F7) act = kItem[vk - VK_F1];
    } else {
        switch (vk) {
            case VK_F1: act = CA_HELP; break;
            case VK_F2: act = CA_TAB; break;
            case VK_F3: act = CA_FILTER; break;
            case VK_F4: act = CA_ITEM; break;
            case VK_F5: act = CA_REFRESH; break;
            case VK_F6: act = CA_TI; break;
            case VK_F7: act = CA_PRESET + 0; break;
            case VK_F8: act = CA_PRESET + 1; break;
            case VK_F9: act = CA_PRESET + 2; break;
            case VK_F10: act = CA_PRESET + 3; break;
            case VK_F11: act = CA_PRESET + 4; break;
        }
    }
    if (act == CA_NONE) return false;
    OnConsoleAction(act);
    return true;
}

bool ServicesModule::OnConsoleInput(UINT msg, WPARAM w, LPARAM l, bool* handled) {
    *handled = false;
    const bool plan = mode_ == Mode::Plan;
    ConGrid& g = plan ? conPlan_ : conList_;
    ListView& lv = plan ? plist_ : list_;
    const int n = plan ? (int)plan_.rows.size() : (int)V_.size();
    auto setSel = [&](int k) {
        if (n <= 0) return false;
        k = std::clamp(k, 0, n - 1);
        g.Ensure(k);
        g.lastSel = k;
        if (k == lv.Selected()) return true;
        lv.Select(k);
        detSb_.SetOffset(0);
        conLeftSb_.SetOffset(0);
        if (!plan) { selName_ = items_[V_[k]].name; ConsoleStatus(); }
        return true;
    };

    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
        if (w >= VK_F1 && w <= VK_F12) {
            *handled = true;
            return OnConsoleKey((UINT)w);
        }
        if (w == VK_ESCAPE && (conItemBar_ || conHelp_)) { conItemBar_ = conHelp_ = false; *handled = true; return true; }
        if (w == VK_ESCAPE && applyW_.Running()) { *handled = true; OnConsoleAction(CA_CANCEL); return true; }
        if (ctrl && w == 'Z' && !plan && focus_ != Focus::Search) { *handled = true; OnConsoleAction(CA_UNDO); return true; }
        bool nav = w == VK_UP || w == VK_DOWN || w == VK_PRIOR || w == VK_NEXT || w == VK_HOME || w == VK_END;
        bool fromSearch = focus_ == Focus::Search && !plan && w != VK_HOME && w != VK_END;
        if (nav && (focus_ == Focus::List || fromSearch)) {
            *handled = true;
            int sel = lv.Selected(), page = std::max(1, g.Rows() - 1);
            int k = sel;
            switch (w) {
                case VK_UP: k = sel < 0 ? 0 : sel - 1; break;
                case VK_DOWN: k = sel + 1; break;
                case VK_PRIOR: k = sel - page; break;
                case VK_NEXT: k = sel + page; break;
                case VK_HOME: k = 0; break;
                case VK_END: k = n - 1; break;
            }
            return setSel(k);
        }
        if (w == VK_TAB || w == VK_ESCAPE) conItemBar_ = false;
        return false;   // Tab, Esc, Ctrl+F, Space, typing and detail scrolling: the shared handler
    }
    if (msg == WM_MOUSEWHEEL) {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd_, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (PtInRect(&g.rc, pt)) {
            *handled = true;
            int before = g.sb.Offset();
            g.sb.SetOffset(before - delta * 3 * g.rowH / WHEEL_DELTA);
            return g.sb.Offset() != before;
        }
        if (PtInRect(&conLeftRc_, pt)) {
            *handled = true;
            int before = conLeftSb_.Offset();
            conLeftSb_.SetOffset(before - delta * Sc(54) / WHEEL_DELTA);
            return conLeftSb_.Offset() != before;
        }
        return false;   // the right column (rcDetail_/detSb_): the shared handler
    }
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    POINT pt{x, y};
    if (msg == WM_MOUSEMOVE) {
        for (ScrollBar* sb : {&g.sb, &conLeftSb_}) {
            if (!sb->Dragging()) continue;
            *handled = true;
            const RECT& tr = sb == &g.sb ? g.track : conLeftTrack_;
            if (!(w & MK_LBUTTON)) { sb->OnMouse(WM_LBUTTONUP, x, y, tr); return true; }
            return sb->OnMouse(msg, x, y, tr);
        }
        return false;
    }
    if (msg == WM_LBUTTONUP) {
        bool any = false;
        if (g.sb.Dragging()) { g.sb.OnMouse(msg, x, y, g.track); any = true; }
        if (conLeftSb_.Dragging()) { conLeftSb_.OnMouse(msg, x, y, conLeftTrack_); any = true; }
        if (!any) return false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        *handled = true;
        return true;
    }
    if (msg != WM_LBUTTONDOWN && msg != WM_LBUTTONDBLCLK) return false;
    for (auto& ck : clicks_) if (PtInRect(&ck.rc, pt)) return false;   // buttons, F-key bar, tabs, filter: shared
    if (g.sb.Visible() && PtInRect(&g.track, pt)) {
        *handled = true;
        focus_ = Focus::List;
        SetCapture(hwnd_);
        g.sb.OnMouse(WM_LBUTTONDOWN, x, y, g.track);
        return true;
    }
    if (conLeftSb_.Visible() && PtInRect(&conLeftTrack_, pt)) {
        *handled = true;
        focus_ = Focus::Detail;
        SetCapture(hwnd_);
        conLeftSb_.OnMouse(WM_LBUTTONDOWN, x, y, conLeftTrack_);
        return true;
    }
    if (PtInRect(&g.rc, pt)) {
        *handled = true;
        focus_ = Focus::List;
        conItemBar_ = conItemBar_ && !plan;
        if (y < g.body.top) {   // header: sort by that column; a second click reverses
            for (size_t i = 0; i < g.colL.size(); i++) {
                if (x < g.colL[i] || x >= g.colR[i]) continue;
                if (plan) ConsoleSortPlan((int)i);
                else if (list_.onSortChanged) {
                    bool asc = true;
                    bool same = i < colKeys_.size() && SortKeyShown(&asc) == colKeys_[i];
                    list_.onSortChanged((int)i, same ? !asc : true);
                }
                g.lastSel = -2;
                return true;
            }
            return true;
        }
        if (y >= g.body.bottom || x >= g.body.right) return true;
        int k = (y - g.body.top + g.sb.Offset()) / std::max(1, g.rowH);
        if (k < 0 || k >= n) return true;
        setSel(k);
        if (plan && !g.colR.empty() && x < g.colR[0] && msg == WM_LBUTTONDOWN) TogglePlanRow(k);
        return true;
    }
    if (PtInRect(&conLeftRc_, pt)) { *handled = true; focus_ = Focus::Detail; return true; }
    return false;
}

void ServicesModule::ReplayKeys(const std::wstring& keys) {
    // Fixture only (LoadFixture is never reached with live data): drives the same OnInput path a key press does.
    if (!fixture_) return;
    for (auto& k : Split(keys, L',')) {
        std::wstring u = Lower(k);
        UINT vk = 0;
        if (u.size() >= 2 && u[0] == L'f' && iswdigit(u[1])) vk = VK_F1 + (UINT)_wtoi(u.c_str() + 1) - 1;
        else if (u == L"up") vk = VK_UP;
        else if (u == L"down") vk = VK_DOWN;
        else if (u == L"pgup") vk = VK_PRIOR;
        else if (u == L"pgdn") vk = VK_NEXT;
        else if (u == L"home") vk = VK_HOME;
        else if (u == L"end") vk = VK_END;
        else if (u == L"tab") vk = VK_TAB;
        else if (u == L"esc") vk = VK_ESCAPE;
        else if (u == L"space") vk = VK_SPACE;
        else if (u == L"enter") vk = VK_RETURN;
        else if (u.rfind(L"text:", 0) == 0) { for (wchar_t ch : k.substr(5)) OnInput(WM_CHAR, ch, 0); continue; }
        if (vk) OnInput(WM_KEYDOWN, vk, 0);
    }
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
