// core/host/host.cpp — the shared host. Implements IHost, owns the frame, the message loop, DPI,
// COM, logging and the deterministic --screenshot path. Standalone and suite differ only in whether
// the sidebar is drawn and how many modules are present.
#include <algorithm>
#include <cstdio>
#include "core/host/run.h"
#include "core/ui/draw.h"
#include "core/host/cmdline.h"
#include "core/host/registry.h"
#include "core/sys/sys.h"
#include "core/sys/settings.h"
#include "core/sys/log.h"
#include "core/sys/png.h"
#include "core/sys/update.h"
#include "core/sys/worker.h"
#include "core/ui/theme.h"
#include "core/ui/aa.h"
#include "core/ui/widgets.h"   // ScrollBar for the Three-Pane tree (widgets.h does not pull in draw.h)
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>   // CoInitializeEx/CoUninitialize: WIN32_LEAN_AND_MEAN excludes OLE on MSVC
#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace hx {
using namespace hx::ui;

namespace {

void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
void HLine(HDC dc, int x0, int x1, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c), old = (HPEN)SelectObject(dc, p);
    MoveToEx(dc, x0, y, nullptr); LineTo(dc, x1, y);
    SelectObject(dc, old); DeleteObject(p);
}
void TextLeft(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
}
void TextRight(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// user32 entry points that may be absent in older import libs: resolve at runtime.
using PFnSetDpiCtx = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
using PFnGetDpiWin = UINT (WINAPI*)(HWND);
UINT DpiOf(HWND h) {
    static PFnGetDpiWin fn = (PFnGetDpiWin)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && h) { UINT d = fn(h); if (d) return d; }
    HDC dc = GetDC(nullptr); UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

// --- module content window (the helper modules call from Create) -----------------------------
struct ContentCtx { Module* module; };

LRESULT CALLBACK ContentProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* ctx = reinterpret_cast<ContentCtx*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(h, m, w, l);
    }
    case WM_ERASEBKGND: return 1;   // we paint every pixel
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        FillSolid(mem, rc, Baseline().bg);
        if (ctx && ctx->module) ctx->module->Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT rc; GetClientRect(h, &rc);
        FillSolid((HDC)w, rc, Baseline().bg);
        if (ctx && ctx->module) ctx->module->Paint((HDC)w, rc);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);   // so the content window gets keyboard and wheel messages
        [[fallthrough]];
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_CHAR:
        if (ctx && ctx->module && ctx->module->OnInput(m, w, l))
            InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        delete ctx;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// --- the host ---------------------------------------------------------------------------------
class Host : public IHost {
public:
    Host(std::vector<std::unique_ptr<Module>> mods, bool suite)
        : modules_(std::move(mods)), content_(modules_.size(), nullptr), suite_(suite) {}

    int Run(const CommandLine& cl);

    HWND Frame() override { return frame_; }
    UINT Dpi() override { return dpi_; }
    void SetTitle(const wchar_t* t) override {
        caption_ = t ? t : L"Hadron";
        if (frame_) SetWindowTextW(frame_, caption_.c_str());
    }
    void SetStatus(const wchar_t* t) override { status_ = t ? t : L""; InvalidateChrome(); }
    void SetBusy(bool b) override { busy_ = b; InvalidateChrome(); }
    bool IsElevated() override { return elevated_; }
    sys::Settings& Prefs() override { return prefs_[active_ < 0 ? 0 : active_]; }
    void Log(LogLevel lv, const wchar_t* msg) override { sys::Write(lv, msg ? msg : L""); }
    const ui::Theme& Theme() override { return theme_; }
    const ui::Fonts& Fonts() override { return fonts_; }
    ui::Layout Layout() override { return layout_; }

private:
    // Smallest content area any module is laid out for, in 96-DPI units (scaled by the window's DPI). The
    // frame's minimum track size is this plus the sidebar and the status bar; modules degrade down to it.
    static constexpr int kMinContentW = 800, kMinContentH = 560;
    // Per-layout chrome (docs/ARCHITECTURE.md "Layouts"). Baseline: a module sidebar (suite only). Three-Pane:
    // a navigation tree (modules plus the active module's NavNodes) with a draggable splitter. Console: no
    // sidebar; a one-line module tab strip on top and an F-key bar above the status bar.
    bool Tree() const { return layout_ == ui::Layout::ThreePane; }
    bool Con() const { return layout_ == ui::Layout::Console; }
    int splitW() const { return S(5, dpi_); }
    int sidebarW() const {
        if (Tree()) return S(treeW_, dpi_) + splitW();
        if (Con()) return 0;
        return suite_ ? S(188, dpi_) : 0;
    }
    int topH() const { return Con() ? S(22, dpi_) : 0; }
    int fkeyH() const { return Con() && active_ >= 0 && !fkeys_.empty() ? S(22, dpi_) : 0; }
    int treeRowH() const { return S(21, dpi_); }   // board 7: dense tree rows
    int statusH() const { return S(28, dpi_); }
    RECT SidebarRect(const RECT& cl) const {
        return {0, 0, Tree() ? S(treeW_, dpi_) : sidebarW(), cl.bottom - statusH()};
    }
    RECT SplitRect(const RECT& cl) const { int x = S(treeW_, dpi_); return {x, 0, x + splitW(), cl.bottom - statusH()}; }
    RECT ContentRect(const RECT& cl) const {
        return {sidebarW(), topH(), cl.right, cl.bottom - statusH() - fkeyH()};
    }
    RECT RowRect(int i, const RECT& cl) const {
        (void)cl; int top = S(60, dpi_), rowH = S(38, dpi_), pad = S(8, dpi_);
        return {pad, top + i * rowH, sidebarW() - pad, top + (i + 1) * rowH};
    }

    // Three-Pane tree rows: every module, and under the active one its NavNodes.
    struct TreeRow { int module; int nav; int depth; std::wstring label, note; bool header, dim, expandable, expanded; };
    std::wstring treeFooter_;         // the active module's NavFooter()
    int treeFooterH() const {
        if (treeFooter_.empty()) return 0;
        int lines = 1;
        for (wchar_t c : treeFooter_) lines += c == L'\n';
        return S(10, dpi_) + lines * S(16, dpi_);
    }
    std::vector<TreeRow> treeRows_;
    int treeW_ = 264;                 // 96-DPI units; [host] tree_w
    int treeCursor_ = -1;             // last row Ctrl+Shift+Up/Down stepped to (-1: start from NavSelected)
    bool splitDrag_ = false;
    ui::ScrollBar treeSb_;
    std::vector<FKey> fkeys_;         // Console: the active module's F-key bar
    std::vector<RECT> fkeyRects_, tabRects_;
    void RebuildChromeModel();
    RECT TreeBody(const RECT& cl) const {
        RECT r = SidebarRect(cl); r.top += S(6, dpi_); r.bottom = std::max((int)r.top, (int)r.bottom - treeFooterH()); return r;
    }
    RECT TreeRowRect(int i, const RECT& cl) const {
        RECT b = TreeBody(cl);
        int y = b.top + i * treeRowH() - treeSb_.Offset();
        return {b.left, y, b.right - (treeSb_.Visible() ? S(8, dpi_) : 0), y + treeRowH()};
    }
    void PaintTree(HDC dc, const RECT& cl);
    void PaintConsoleTop(HDC dc, const RECT& cl);
    void PaintFKeys(HDC dc, const RECT& cl);
    void StepTree(int dir);
    void ChromeChanged() override { RebuildChromeModel(); LayoutContent(); InvalidateChrome(); }
    void Activate(int i);
    void LayoutContent() {
        if (active_ >= 0 && content_[active_]) {
            RECT cl; GetClientRect(frame_, &cl);
            RECT cr = ContentRect(cl);
            SetWindowPos(content_[active_], nullptr, cr.left, cr.top,
                         cr.right - cr.left, cr.bottom - cr.top, SWP_NOZORDER);
        }
    }
    void InvalidateChrome() { if (frame_) InvalidateRect(frame_, nullptr, FALSE); }
    // Layout and theme switch (status bar chips, Ctrl+Shift+L / Ctrl+Shift+T). Saved in [host] of
    // settings.ini; a --layout/--theme flag overrides the saved value for that run.
    void SetLook(ui::Layout l, ui::ThemeId t);
    void NextLayout(int dir) { SetLook((ui::Layout)((((int)layout_ + dir) % ui::kLayoutCount + ui::kLayoutCount) % ui::kLayoutCount), themeId_); }
    void NextTheme(int dir) { SetLook(layout_, (ui::ThemeId)((((int)themeId_ + dir) % ui::kThemeCount + ui::kThemeCount) % ui::kThemeCount)); }
    void ApplyCaptionColour();
    void PaintChrome(HDC dc, const RECT& cl);
    int Screenshot(const CommandLine& cl);
    int PickInitial(const CommandLine& cl) const;

    static LRESULT CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnFrame(HWND, UINT, WPARAM, LPARAM);

    std::vector<std::unique_ptr<Module>> modules_;
    std::vector<HWND> content_;
    bool suite_;
    int active_ = -1;
    HWND frame_ = nullptr;
    UINT dpi_ = 96;
    bool elevated_ = false, busy_ = false, shooting_ = false;
    std::wstring caption_ = L"Hadron", status_;
    sys::SettingsStore store_;
    std::vector<sys::Settings> prefs_;
    ui::Fonts fonts_;
    ui::ThemeId themeId_ = ui::ThemeId::Dark;
    ui::Layout  layout_  = ui::Layout::Baseline;
    ui::Theme   theme_   = Resolve(ui::ThemeId::Dark, FallbackAccent(ui::ThemeId::Dark));
    RECT chipLayout_{}, chipTheme_{};   // status-bar switch hit areas (interactive runs only)
    bool interactive_ = false;

    // Self-update (core/sys/update.h). [update] auto = on | off; missing => ask once in the status bar.
    enum class UpdMode { Ask, On, Off };
    UpdMode updMode_ = UpdMode::Ask;
    bool updReady_ = false;              // a new exe is in place; the chip offers a restart
    bool updPending_ = false;            // turned back On while a cancelled check was still winding down
    sys::Worker updWorker_;
    sys::update::Result updResult_;
    RECT chipUpd_{}, chipYes_{}, chipNo_{};
    void SetUpdateMode(UpdMode m);
    void StartUpdateCheck(bool install);
    void OnUpdateDone();
    void RestartForUpdate();

    // Build fonts for the active layout and resolve the palette. deterministic => use the theme's own
    // accent (stable goldens for --screenshot/--selftest); otherwise follow the live Windows accent.
    // accentOverride_ (--accent, screenshots only) stands in for the Windows accent, with the same
    // achromatic fallback the live path uses, so design review can show accent-following deterministically.
    void BuildTheme(bool deterministic) {
        fonts_.Build(dpi_, layout_);
        if (deterministic && hasAccentOverride_)
            theme_ = Resolve(themeId_, ui::IsAchromatic(accentOverride_) ? FallbackAccent(themeId_, layout_) : accentOverride_, layout_);
        else
            theme_ = deterministic ? Resolve(themeId_, FallbackAccent(themeId_, layout_), layout_) : ResolveLive(themeId_, layout_);
    }
    bool     hasAccentOverride_ = false;
    COLORREF accentOverride_ = 0;
};

const wchar_t* kFrameClass = L"Hadron.Frame";
constexpr UINT WM_HX_UPDATE_DONE = WM_APP + 1;   // host range (WM_APP+0..0xFF)

int Host::PickInitial(const CommandLine& cl) const {
    const std::wstring& want = !cl.solo.empty() ? cl.solo : cl.module;
    if (!want.empty())
        for (size_t i = 0; i < modules_.size(); i++)
            if (want == modules_[i]->Info().id) return (int)i;
    return modules_.empty() ? -1 : 0;
}

void Host::Activate(int i) {
    if (i < 0 || i >= (int)modules_.size() || i == active_) return;
    if (active_ >= 0) {
        modules_[active_]->OnDeactivate();
        if (content_[active_]) ShowWindow(content_[active_], SW_HIDE);
    }
    active_ = i;
    RECT cl; GetClientRect(frame_, &cl);
    RECT cr = ContentRect(cl);
    if (!content_[i]) {
        content_[i] = modules_[i]->Create(*this, frame_, cr);   // module calls CreateModuleContent
        modules_[i]->OnDpiChanged(dpi_);
    }
    if (content_[i]) {
        SetWindowPos(content_[i], nullptr, cr.left, cr.top, cr.right - cr.left, cr.bottom - cr.top,
                     SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    modules_[i]->OnActivate();
    SetTitle(modules_[i]->Info().name);
    RebuildChromeModel();
    LayoutContent();
    InvalidateChrome();
}

void Host::RebuildChromeModel() {
    treeRows_.clear();
    for (int i = 0; i < (int)modules_.size(); i++) {
        if (!suite_ && i != active_) continue;
        bool open = i == active_ && content_[i];
        std::vector<NavNode> nodes = open ? modules_[i]->NavNodes() : std::vector<NavNode>{};
        // Other modules show closed: their sections appear once they are active.
        treeRows_.push_back({i, -1, 0, modules_[i]->Info().name, open ? modules_[i]->NavNote() : L"", false, false,
                             !open || !nodes.empty(), open && !nodes.empty()});
        for (const NavNode& n : nodes)
            treeRows_.push_back({i, n.header ? -1 : n.id, n.depth + 1, n.label, n.note, n.header, n.dim, n.expandable, n.expanded});
    }
    treeFooter_ = (active_ >= 0 && content_[active_]) ? modules_[active_]->NavFooter() : std::wstring();
    fkeys_ = (active_ >= 0 && content_[active_]) ? modules_[active_]->FKeys() : std::vector<FKey>{};
}

void Host::StepTree(int dir) {
    if (treeRows_.empty() || active_ < 0) return;
    // Walk from the row we stepped to last (treeCursor_), not from NavSelected(): a row the module refuses
    // (a locked identity, a recent launch that only refills a form) must not trap the walk.
    int cur = -1;
    if (treeCursor_ >= 0 && treeCursor_ < (int)treeRows_.size() && treeRows_[treeCursor_].module == active_) cur = treeCursor_;
    if (cur < 0) {
        int sel = modules_[active_]->NavSelected();
        for (int i = 0; i < (int)treeRows_.size(); i++)
            if (treeRows_[i].module == active_ && (sel >= 0 ? treeRows_[i].nav == sel : treeRows_[i].depth == 0)) { cur = i; break; }
        if (cur < 0) cur = 0;
    }
    for (int i = cur + dir; i >= 0 && i < (int)treeRows_.size(); i += dir) {
        const TreeRow row = treeRows_[i];
        if (row.header) continue;
        bool moduleChange = row.module != active_;
        Activate(row.module);
        if (row.nav >= 0) modules_[row.module]->OnNavSelect(row.nav);
        if (content_[row.module]) InvalidateRect(content_[row.module], nullptr, FALSE);
        RebuildChromeModel();
        // the rows under a module change when it becomes active: find the same row again
        treeCursor_ = -1;
        for (int k = 0; k < (int)treeRows_.size(); k++)
            if (treeRows_[k].module == row.module && treeRows_[k].nav == row.nav && treeRows_[k].depth == row.depth &&
                treeRows_[k].label == row.label) { treeCursor_ = k; break; }
        if (treeCursor_ < 0) treeCursor_ = moduleChange ? 0 : i;
        RECT cl; GetClientRect(frame_, &cl);
        RECT b = TreeBody(cl);
        int y = treeCursor_ * treeRowH(), vh = b.bottom - b.top;
        if (y < treeSb_.Offset()) treeSb_.SetOffset(y);
        else if (y + treeRowH() > treeSb_.Offset() + vh) treeSb_.SetOffset(y + treeRowH() - vh);
        InvalidateChrome();
        return;
    }
}

void Host::PaintTree(HDC dc, const RECT& cl) {
    const ui::Theme& t = theme_;
    const bool hc = t.highContrast;
    RECT sb = SidebarRect(cl);
    FillSolid(dc, sb, t.panel);
    RECT sp = SplitRect(cl);
    FillSolid(dc, sp, t.bg);
    { RECT l{sp.left, sp.top, sp.left + 1, sp.bottom}, r{sp.right - 1, sp.top, sp.right, sp.bottom};
      FillSolid(dc, l, t.border); FillSolid(dc, r, t.border); }
    RECT body = TreeBody(cl);
    treeSb_.Configure(body.bottom - body.top, (int)treeRows_.size() * treeRowH());
    int sel = active_ >= 0 ? modules_[active_]->NavSelected() : -1;
    const bool focused = GetFocus() == frame_;
    int save = SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
    const int indent = S(14, dpi_), chevW = S(14, dpi_);
    for (int i = 0; i < (int)treeRows_.size(); i++) {
        const TreeRow& row = treeRows_[i];
        RECT r = TreeRowRect(i, cl);
        if (r.bottom <= body.top || r.top >= body.bottom) continue;
        if (row.header) {
            // Board 7: a group caption ("VIEWS") under a 1px rule, in small text3.
            RECT rule{r.left, r.top + S(4, dpi_), r.right, r.top + S(4, dpi_) + 1};
            FillSolid(dc, rule, t.border);
            RECT hr{r.left + S(10, dpi_), r.top + S(6, dpi_), r.right - S(8, dpi_), r.bottom};
            TextLeft(dc, row.label, hr, fonts_.caption, hc ? GetSysColor(COLOR_GRAYTEXT) : t.text3);
            continue;
        }
        bool on = row.module == active_ && (row.nav >= 0 ? row.nav == sel : (sel < 0 && row.depth == 0));
        if (on) {
            FillSolid(dc, r, hc ? GetSysColor(COLOR_HIGHLIGHT) : ui::Mix(t.accent, t.panel, 0.30));
            if (focused || treeCursor_ == i) {   // the keyboard row: dashed focus rect, as the lists draw it
                RECT fr = r; InflateRect(&fr, -1, 0);
                ui::FocusRectC(dc, fr, hc ? GetSysColor(COLOR_HIGHLIGHTTEXT) : t.accent);
            }
        }
        RECT tr = r;
        tr.left += S(8, dpi_) + row.depth * indent;
        tr.right -= S(8, dpi_);
        COLORREF ink = row.dim ? t.text3 : (on || row.depth == 0 ? t.text1 : t.text2);
        if (hc) ink = on ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
        if (row.expandable) {
            // A small chevron: › closed, ⌄ open, drawn as two lines so it needs no symbol font.
            int cx = tr.left + chevW / 2 - S(1, dpi_), cy = (r.top + r.bottom) / 2, a = S(3, dpi_);
            COLORREF cc = hc ? ink : t.text3;
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1, dpi_)), cc);
            HGDIOBJ op = SelectObject(dc, pen);
            if (row.expanded) { POINT pt[3] = {{cx - a, cy - a / 2}, {cx, cy + a / 2 + 1}, {cx + a + 1, cy - a / 2 - 1}}; Polyline(dc, pt, 3); }
            else { POINT pt[3] = {{cx - a / 2, cy - a}, {cx + a / 2 + 1, cy}, {cx - a / 2 - 1, cy + a + 1}}; Polyline(dc, pt, 3); }
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        tr.left += chevW;
        if (!row.note.empty()) {
            TextRight(dc, row.note, tr, fonts_.caption, hc ? ink : (on ? t.text1 : t.text3));
            SIZE ns{}; HGDIOBJ o = SelectObject(dc, fonts_.caption);
            GetTextExtentPoint32W(dc, row.note.c_str(), (int)row.note.size(), &ns); SelectObject(dc, o);
            tr.right -= ns.cx + S(8, dpi_);
        }
        HFONT f = (row.depth == 0 && row.module == active_) || on ? fonts_.semi : fonts_.ui;
        TextLeft(dc, row.label, tr, f, ink);
    }
    RestoreDC(dc, save);
    if (treeSb_.Visible()) {
        RECT track{body.right - S(8, dpi_), body.top, body.right, body.bottom};
        ui::PaintCtx pc{theme_, fonts_, dpi_, layout_};
        treeSb_.Paint(dc, track, pc);
    }
    if (!treeFooter_.empty()) {   // the module's machine line, pinned to the bottom of the tree
        RECT fr{sb.left + S(10, dpi_), body.bottom + S(6, dpi_), sb.right - S(8, dpi_), sb.bottom};
        ui::DrawText(dc, treeFooter_, fr, fonts_.caption, hc ? GetSysColor(COLOR_GRAYTEXT) : t.text3, DT_WORDBREAK);
    }
}

void Host::PaintConsoleTop(HDC dc, const RECT& cl) {
    const ui::Theme& t = theme_;
    RECT top{0, 0, cl.right, topH()};
    FillSolid(dc, top, t.panel);
    HLine(dc, top.left, top.right, top.bottom - 1, t.border);
    tabRects_.assign(modules_.size(), RECT{});
    int x = S(8, dpi_);
    for (int i = 0; i < (int)modules_.size(); i++) {
        if (!suite_ && i != active_) continue;
        std::wstring name = modules_[i]->Info().name;
        for (auto& c : name) c = (wchar_t)towupper(c);
        std::wstring label = suite_ ? std::to_wstring(i + 1) + L" " + name : name;
        SIZE sz{}; HGDIOBJ o = SelectObject(dc, fonts_.caption);
        GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &sz); SelectObject(dc, o);
        RECT r{x, top.top + S(3, dpi_), x + sz.cx + S(12, dpi_), top.bottom - S(3, dpi_)};
        bool on = i == active_;
        if (on) FillSolid(dc, r, t.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : t.accent);
        RECT tr = r;
        SelectObject(dc, fonts_.caption);
        SetTextColor(dc, t.highContrast ? GetSysColor(on ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)
                                        : on ? ui::OnFill(t.accent) : t.text3);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        tabRects_[i] = r;
        x = r.right + S(4, dpi_);
    }
    if (active_ >= 0) {
        RECT sr{x + S(12, dpi_), top.top, cl.right - S(8, dpi_), top.bottom};
        TextLeft(dc, content_[active_] ? modules_[active_]->ConsoleHeader() : std::wstring(), sr, fonts_.caption, t.text3);
    }
}

void Host::PaintFKeys(HDC dc, const RECT& cl) {
    const ui::Theme& t = theme_;
    int h = fkeyH();
    fkeyRects_.assign(fkeys_.size(), RECT{});
    if (!h) return;
    RECT bar{0, cl.bottom - statusH() - h, cl.right, cl.bottom - statusH()};
    FillSolid(dc, bar, t.panel);
    HLine(dc, bar.left, bar.right, bar.top, t.border);
    int x = S(8, dpi_);
    HGDIOBJ o = SelectObject(dc, fonts_.caption);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < fkeys_.size(); i++) {
        const FKey& k = fkeys_[i];
        SIZE ks{}, ls{};
        GetTextExtentPoint32W(dc, k.key.c_str(), (int)k.key.size(), &ks);
        GetTextExtentPoint32W(dc, k.label.c_str(), (int)k.label.size(), &ls);
        int w = ks.cx + S(5, dpi_) + ls.cx + S(2, dpi_);   // +2: GDI extents run short of the drawn glyphs under some fonts
        if (x + w > bar.right - S(8, dpi_)) break;
        RECT kr{x, bar.top, x + ks.cx, bar.bottom}, lr{x + ks.cx + S(5, dpi_), bar.top, x + w, bar.bottom};
        // The raw Windows accent can be unreadable on the panel (a pale accent on Paper): fall back to text1.
        SetTextColor(dc, t.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : ui::Contrast(t.accent, t.panel) >= 4.5 ? t.accent : t.text1);
        DrawTextW(dc, k.key.c_str(), -1, &kr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(dc, t.highContrast ? GetSysColor(COLOR_WINDOWTEXT) : t.text2);
        DrawTextW(dc, k.label.c_str(), -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        fkeyRects_[i] = RECT{x, bar.top, x + w, bar.bottom};
        x += w + S(16, dpi_);
    }
    SelectObject(dc, o);
}

void Host::PaintChrome(HDC dc, const RECT& cl) {
    const ui::Theme& t = theme_;
    FillSolid(dc, cl, t.bg);
    if (Tree()) PaintTree(dc, cl);
    else if (Con()) { PaintConsoleTop(dc, cl); PaintFKeys(dc, cl); }
    else if (suite_) {
        RECT sb = SidebarRect(cl);
        FillSolid(dc, sb, t.panel);
        HLine(dc, sb.right - 1, sb.right - 1, sb.bottom, t.border);   // 1px right edge
        { HPEN p = CreatePen(PS_SOLID, 1, t.border), o = (HPEN)SelectObject(dc, p);
          MoveToEx(dc, sb.right - 1, 0, nullptr); LineTo(dc, sb.right - 1, sb.bottom);
          SelectObject(dc, o); DeleteObject(p); }
        RECT title{S(16, dpi_), S(16, dpi_), sb.right, S(50, dpi_)};
        TextLeft(dc, L"Hadron", title, fonts_.title, t.text1);
        for (int i = 0; i < (int)modules_.size(); i++) {
            RECT r = RowRect(i, cl);
            bool on = (i == active_);
            if (on) {
                FillSolid(dc, r, t.panel2);
                RECT bar{r.left, r.top, r.left + S(3, dpi_), r.bottom};
                FillSolid(dc, bar, t.accent);
            }
            RECT tr = r; tr.left += S(14, dpi_);
            TextLeft(dc, modules_[i]->Info().name, tr, on ? fonts_.semi : fonts_.ui,
                     on ? t.text1 : t.text2);
        }
    }
    RECT st{0, cl.bottom - statusH(), cl.right, cl.bottom};
    FillSolid(dc, st, t.panel);
    HLine(dc, st.left, st.right, st.top, t.border);
    RECT sr = st; sr.left += S(12, dpi_);
    std::wstring s = status_.empty()
        ? (elevated_ ? L"Running as administrator" : L"Not elevated — some features are limited")
        : status_;
    int chipsLeft = st.right;
    if (interactive_) {
        // Two chips at the right of the status bar: click to switch, right-click to go back.
        auto chip = [&](const std::wstring& label, int right, RECT* out) {
            SIZE sz{}; HGDIOBJ o = SelectObject(dc, fonts_.caption);
            GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &sz);
            SelectObject(dc, o);
            int w = sz.cx + S(20, dpi_);
            RECT r{right - w, st.top + S(4, dpi_), right, st.bottom - S(4, dpi_)};
            ui::AaRoundRect(dc, r, S(8, dpi_) / 2.0, t.panel2, t.border);
            RECT tr = r;
            SelectObject(dc, fonts_.caption); SetTextColor(dc, t.text1); SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            *out = r;
            return r.left;
        };
        int right = st.right - S(10, dpi_);
        right = chip(std::wstring(L"Theme: ") + ui::ThemeName(themeId_), right, &chipTheme_) - S(6, dpi_);
        right = chip(std::wstring(L"Layout: ") + ui::LayoutName(layout_), right, &chipLayout_) - S(6, dpi_);
        chipUpd_ = chipYes_ = chipNo_ = RECT{};
        if (updMode_ == UpdMode::Ask) {
            // First run: ask once, in place. Nothing is checked until the user answers.
            right = chip(L"No", right, &chipNo_) - S(6, dpi_);
            right = chip(L"Yes", right, &chipYes_) - S(8, dpi_);
            const wchar_t* q = L"Update Hadron automatically from GitHub releases?";
            SIZE qs{}; SelectObject(dc, fonts_.caption);
            GetTextExtentPoint32W(dc, q, (int)wcslen(q), &qs);
            RECT qr{right - qs.cx - S(4, dpi_), st.top, right, st.bottom};
            SetTextColor(dc, t.accent); SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, q, -1, &qr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
            right = qr.left;
        } else {
            std::wstring u = updReady_ ? L"Restart to update"
                           : updWorker_.Running() ? L"Updates: checking…"
                           : updMode_ == UpdMode::On ? L"Updates: On" : L"Updates: Off";
            right = chip(u, right, &chipUpd_);
        }
        chipsLeft = right - S(10, dpi_);
    }
    sr.right = chipsLeft;
    TextLeft(dc, s, sr, fonts_.caption, t.text2);
    if (busy_) {
        RECT br = st; br.right = chipsLeft;
        SelectObject(dc, fonts_.caption); SetTextColor(dc, t.accent); SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Working…", -1, &br, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
}

void Host::ApplyCaptionColour() {
    if (!frame_) return;
    COLORREF bg = theme_.bg;
    BOOL dark = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000 < 128;
    DwmSetWindowAttribute(frame_, HX_DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // Windows 10 doesn't repaint a visible caption after the attribute changes until the frame is recalculated.
    SetWindowPos(frame_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void Host::SetLook(ui::Layout l, ui::ThemeId t) {
    if (l == layout_ && t == themeId_) return;
    bool layoutChanged = l != layout_, themeChanged = t != themeId_;
    layout_ = l;
    themeId_ = t;
    BuildTheme(/*deterministic=*/false);
    // Save only the key that changed, so a --theme/--layout flag for this run isn't made permanent by
    // switching the other one.
    sys::Settings look = store_.Section(L"host");
    if (layoutChanged) look.SetStr(L"layout", ui::LayoutKey(l));
    if (themeChanged) look.SetStr(L"theme", t == ui::ThemeId::Light ? L"light" : L"dark");
    DWORD serr = 0;
    if (!store_.Save(&serr)) {   // save now, so the choice survives a crash or a killed process
        sys::Write(LogLevel::Warn, L"look: settings.ini not saved: " + sys::ErrorText(serr));
        SetStatus((L"Look not saved: " + sys::ErrorText(serr)).c_str());
    }
    // Modules rebuild layout-dependent fonts and metrics the same way they do on a DPI change.
    for (size_t i = 0; i < modules_.size(); i++)
        if (content_[i]) modules_[i]->OnDpiChanged(dpi_);
    ApplyCaptionColour();
    if (frame_ && layoutChanged && !shooting_ && !IsZoomed(frame_)) {
        // Three-Pane's tree and Console's bars take room: grow the window so the content keeps its minimum.
        RECT cl; GetClientRect(frame_, &cl);
        RECT cr = ContentRect(cl);
        int needW = S(kMinContentW, dpi_) - (cr.right - cr.left), needH = S(kMinContentH, dpi_) - (cr.bottom - cr.top);
        if (needW > 0 || needH > 0) {
            RECT wr; GetWindowRect(frame_, &wr);
            SetWindowPos(frame_, nullptr, 0, 0, (wr.right - wr.left) + std::max(0, needW), (wr.bottom - wr.top) + std::max(0, needH),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    LayoutContent();
    if (frame_) RedrawWindow(frame_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
    sys::Write(LogLevel::Info, std::wstring(L"look: ") + ui::LayoutName(l) + L" / " + ui::ThemeName(t));
}

void Host::SetUpdateMode(UpdMode m) {
    updMode_ = m;
    store_.Section(L"update").SetStr(L"auto", m == UpdMode::On ? L"on" : L"off");
    DWORD serr = 0;
    if (!store_.Save(&serr)) SetStatus((L"Update setting not saved: " + sys::ErrorText(serr)).c_str());
    sys::Write(LogLevel::Info, m == UpdMode::On ? L"update: automatic updates on" : L"update: automatic updates off");
    if (m == UpdMode::On) StartUpdateCheck(/*install=*/true);
    else { updPending_ = false; updWorker_.Cancel(); sys::update::AbortNetwork(); }   // opt-out stops a download;
                                                                                       // a swap in progress still finishes
    InvalidateChrome();
}

void Host::StartUpdateCheck(bool install) {
    if (updReady_) return;
    if (updWorker_.Running()) {   // e.g. Off then On quickly: start again once the cancelled run ends
        if (updWorker_.Cancelled()) updPending_ = true;
        return;
    }
    if (!updWorker_.Start(frame_, WM_HX_UPDATE_DONE, [this, install] {
            updResult_ = sys::update::CheckAndInstall(install, updWorker_.CancelFlag());
        })) {
        SetStatus((L"Update check couldn't start: " + sys::ErrorText(GetLastError())).c_str());
        return;
    }
    InvalidateChrome();
}

void Host::OnUpdateDone() {
    updWorker_.Join();   // the result is complete once the thread has exited
    using sys::update::Outcome;
    const auto& r = updResult_;
    if (r.outcome != Outcome::Failed && r.outcome != Outcome::Cancelled) {
        store_.Section(L"update").SetStr(L"last", std::to_wstring(_time64(nullptr)));
        DWORD serr = 0;
        if (!store_.Save(&serr)) sys::Write(LogLevel::Warn, L"update: last-check time not saved: " + sys::ErrorText(serr));
    }
    if (r.outcome == Outcome::Installed) updReady_ = true;
    if (updPending_) {
        updPending_ = false;
        if (updMode_ == UpdMode::On && r.outcome == Outcome::Cancelled) { StartUpdateCheck(/*install=*/true); return; }
    }
    sys::Write(r.outcome == Outcome::Failed ? LogLevel::Warn : LogLevel::Info, L"update: " + r.message);
    SetStatus(r.message.c_str());
}

void Host::RestartForUpdate() {
    for (auto& mod : modules_)
        if (!mod->CanClose()) { SetStatus(L"Finish the running operation first, then restart to update"); return; }
    std::wstring exe = sys::ModulePath();
    // Same arguments as this run, plus our PID: the new process waits for this one to exit before it
    // loads settings, so the two never overlap (an older --wait-pid from a previous restart is harmless).
    std::wstring cmd = GetCommandLineW();
    for (size_t p; (p = cmd.find(L" --wait-pid=")) != std::wstring::npos;) {   // only this process's PID, never stale ones
        size_t e = cmd.find(L' ', p + 1);
        cmd.erase(p, e == std::wstring::npos ? std::wstring::npos : e - p);
    }
    cmd += L" --wait-pid=" + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        SetStatus((L"Restart failed: " + sys::ErrorText(GetLastError()) + L". Close and reopen Hadron").c_str());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(frame_);
}

LRESULT Host::OnFrame(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        LayoutContent();
        InvalidateChrome();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w);
        fonts_.Build(dpi_, layout_);
        RECT* sug = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (active_ >= 0) modules_[active_]->OnDpiChanged(dpi_);
        LayoutContent();
        InvalidateChrome();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        if (shooting_) break;   // --screenshot sets its own exact client size
        RECT r{0, 0, sidebarW() + S(kMinContentW, dpi_), S(kMinContentH, dpi_) + statusH() + topH() + (Con() ? S(22, dpi_) : 0)};
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE));
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        LONG minW = r.right - r.left, minH = r.bottom - r.top;
        // Never larger than the monitor's work area (mini-review N3: 1366x768 at 125% would put the minimum off-screen).
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            minW = std::min<LONG>(minW, mi.rcWork.right - mi.rcWork.left);
            minH = std::min<LONG>(minH, mi.rcWork.bottom - mi.rcWork.top);
        }
        mm->ptMinTrackSize.x = minW;
        mm->ptMinTrackSize.y = minH;
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cl; GetClientRect(h, &cl);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cl.right, cl.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        PaintChrome(mem, cl);
        BitBlt(dc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT cl; GetClientRect(h, &cl);
        if (splitDrag_) {
            int w = MulDiv(p.x, 96, (int)dpi_);
            // Never squeeze the content below the minimum the modules are laid out for.
            int roomFor = MulDiv(cl.right, 96, (int)dpi_) - 5 - kMinContentW;
            treeW_ = std::clamp(w, 180, std::max(180, std::min(480, roomFor)));
            LayoutContent();
            InvalidateChrome();
        } else if (treeSb_.Dragging()) {
            RECT body = TreeBody(cl);
            RECT track{body.right - S(8, dpi_), body.top, body.right, body.bottom};
            if (treeSb_.OnMouse(WM_MOUSEMOVE, p.x, p.y, track)) InvalidateChrome();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (splitDrag_) {
            splitDrag_ = false;
            store_.Section(L"host").SetStr(L"tree_w", std::to_wstring(treeW_));
            DWORD serr = 0;
            if (!store_.Save(&serr)) sys::Write(LogLevel::Warn, L"tree width not saved: " + sys::ErrorText(serr));
        }
        if (treeSb_.Dragging()) { RECT cl; GetClientRect(h, &cl); RECT b = TreeBody(cl);
            RECT track{b.right - S(8, dpi_), b.top, b.right, b.bottom};
            treeSb_.OnMouse(WM_LBUTTONUP, GET_X_LPARAM(l), GET_Y_LPARAM(l), track); }
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)l != h) {   // capture lost (Alt+Tab, a dialog): end any drag, keep the width reached so far
            if (splitDrag_) {
                splitDrag_ = false;
                store_.Section(L"host").SetStr(L"tree_w", std::to_wstring(treeW_));
            }
            treeSb_.CancelDrag();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        RECT cl; GetClientRect(h, &cl);
        RECT sb = SidebarRect(cl);
        if (Tree() && PtInRect(&sb, p)) {
            treeSb_.SetOffset(treeSb_.Offset() - MulDiv(GET_WHEEL_DELTA_WPARAM(w), 3 * treeRowH(), WHEEL_DELTA));   // touchpads send < 120
            InvalidateChrome();
            return 0;
        }
        break;
    }
    case WM_SETCURSOR:
        if (Tree() && LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT cl; GetClientRect(h, &cl);
            RECT sp = SplitRect(cl);
            if (PtInRect(&sp, p) || splitDrag_) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
        }
        break;
    case WM_RBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (interactive_ && PtInRect(&chipTheme_, p)) NextTheme(-1);
        if (interactive_ && PtInRect(&chipLayout_, p)) NextLayout(-1);
        if (interactive_ && PtInRect(&chipUpd_, p) && !updReady_) StartUpdateCheck(/*install=*/true);   // check now
        return 0;
    }
    case WM_HX_UPDATE_DONE:
        OnUpdateDone();
        return 0;
    case WM_LBUTTONDOWN:
        if (interactive_) {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&chipLayout_, p)) { NextLayout(+1); return 0; }
            if (PtInRect(&chipTheme_, p))  { NextTheme(+1); return 0; }
            if (PtInRect(&chipYes_, p))    { SetUpdateMode(UpdMode::On); return 0; }
            if (PtInRect(&chipNo_, p))     { SetUpdateMode(UpdMode::Off); return 0; }
            if (PtInRect(&chipUpd_, p)) {
                if (updReady_) RestartForUpdate();
                else SetUpdateMode(updMode_ == UpdMode::On ? UpdMode::Off : UpdMode::On);
                return 0;
            }
        }
        {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT cl; GetClientRect(h, &cl);
            if (Tree()) {
                RECT sp = SplitRect(cl);
                if (PtInRect(&sp, p)) { splitDrag_ = true; SetCapture(h); return 0; }
                RECT body = TreeBody(cl);
                RECT track{body.right - S(8, dpi_), body.top, body.right, body.bottom};
                if (treeSb_.Visible() && treeSb_.OnMouse(WM_LBUTTONDOWN, p.x, p.y, track)) {
                    SetCapture(h); InvalidateChrome(); return 0;
                }
                if (PtInRect(&body, p))
                    for (int i = 0; i < (int)treeRows_.size(); i++) {
                        RECT r = TreeRowRect(i, cl);
                        if (!PtInRect(&r, p) || treeRows_[i].header) continue;
                        TreeRow row = treeRows_[i];
                        treeCursor_ = -1;
                        Activate(row.module);
                        if (row.nav >= 0) {
                            modules_[row.module]->OnNavSelect(row.nav);
                            if (content_[row.module]) InvalidateRect(content_[row.module], nullptr, FALSE);
                        }
                        RebuildChromeModel();
                        InvalidateChrome();
                        break;
                    }
            } else if (Con()) {
                for (int i = 0; i < (int)tabRects_.size(); i++)
                    if (PtInRect(&tabRects_[i], p)) { Activate(i); return 0; }
                for (size_t i = 0; i < fkeyRects_.size() && i < fkeys_.size(); i++)
                    if (PtInRect(&fkeyRects_[i], p) && active_ >= 0) {
                        if (modules_[active_]->OnInput(WM_KEYDOWN, fkeys_[i].vk, 0) && content_[active_])
                            InvalidateRect(content_[active_], nullptr, FALSE);
                        return 0;
                    }
            } else if (suite_) {
                for (int i = 0; i < (int)modules_.size(); i++) {
                    RECT r = RowRect(i, cl);
                    if (PtInRect(&r, p)) { Activate(i); break; }
                }
            }
        }
        return 0;
    case WM_CLOSE:
        for (auto& mod : modules_) if (!mod->CanClose()) return 0;   // a plan is executing
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        updWorker_.Cancel();
        sys::update::AbortNetwork();                  // fail the blocking WinHTTP call now
        updWorker_.Join();                            // cancel is polled per read; each fetch has a 3-minute budget
        for (auto& mod : modules_) mod->Shutdown();   // joins workers before HWNDs die
        PostQuitMessage(0);                           // only the host does this (ARCHITECTURE.md)
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Host::FrameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = reinterpret_cast<Host*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->OnFrame(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

int Host::Screenshot(const CommandLine& cl) {
    // Deterministic: fixed 1280x800 client at 96 DPI, hidden frame, fixture loaded synchronously,
    // composed into a top-down DIB and written as PNG. No PrintWindow, so Wine and Windows match.
    const int W = 1280, H = 800;
    dpi_ = 96;
    shooting_ = true;
    themeId_ = cl.theme;
    layout_  = cl.layout;
    hasAccentOverride_ = cl.hasAccent;
    accentOverride_    = cl.accent;
    BuildTheme(/*deterministic=*/true);
    RECT want{0, 0, W, H};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&want, style, FALSE, 0);
    frame_ = CreateWindowExW(0, kFrameClass, caption_.c_str(), style, 0, 0,
                             want.right - want.left, want.bottom - want.top, nullptr, nullptr,
                             GetModuleHandleW(nullptr), this);
    if (!frame_) return 2;
    // Force the client to exactly WxH regardless of frame metrics.
    RECT now; GetClientRect(frame_, &now);
    SetWindowPos(frame_, nullptr, 0, 0,
                 (want.right - want.left) + (W - now.right), (want.bottom - want.top) + (H - now.bottom),
                 SWP_NOZORDER | SWP_NOMOVE | SWP_HIDEWINDOW);
    int init = PickInitial(cl);
    Activate(init);
    if (active_ >= 0 && !cl.fixture.empty()) modules_[active_]->LoadFixture(cl.fixture.c_str());
    RebuildChromeModel();   // the fixture fills the tree sections and F-keys
    LayoutContent();

    RECT cl2; GetClientRect(frame_, &cl2);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;       // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP old = (HBITMAP)SelectObject(mem, dib);
    PaintChrome(mem, cl2);
    if (active_ >= 0) {
        RECT cr = ContentRect(cl2);
        POINT o; SetViewportOrgEx(mem, cr.left, cr.top, &o);
        RECT local{0, 0, cr.right - cr.left, cr.bottom - cr.top};
        FillSolid(mem, local, theme_.bg);
        modules_[active_]->Paint(mem, local);
        SetViewportOrgEx(mem, o.x, o.y, nullptr);
    }
    GdiFlush();
    DWORD err = 0;
    bool ok = sys::SavePng(cl.screenshot, W, H, bits, &err);
    SelectObject(mem, old); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
    for (auto& mod : modules_) mod->Shutdown();
    DestroyWindow(frame_); frame_ = nullptr;
    return ok ? 0 : 3;
}

int Host::Run(const CommandLine& cl) {
    // Test and fixture runs can never write firmware (docs/design/bios-write.md "Test mode latch").
    // Set before any module code runs; never cleared.
    bool snapshotArg = false;
    for (auto& a : cl.rest) if (a == L"--snapshot") snapshotArg = true;
    if (cl.selftest || !cl.screenshot.empty() || !cl.fixture.empty() || snapshotArg) sys::SetNoFirmwareWrites();
    elevated_ = sys::IsElevated();
    for (auto& a : cl.rest)   // restart after an update: wait (up to 30 s) for the old process to finish exiting
        if (a.rfind(L"--wait-pid=", 0) == 0) {
            DWORD pid = (DWORD)wcstoul(a.c_str() + 11, nullptr, 10);
            if (HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr) { WaitForSingleObject(p, 30000); CloseHandle(p); }
        }

    for (auto& a : cl.rest)   // core self-update logic, no network (CI runs it on one exe)
        if (a == L"--selftest-update") return sys::update::SelfTest();
    if (cl.selftest) {
        // A GUI-subsystem exe has no console, so wprintf output vanishes when it's started from
        // cmd on Windows (Wine and CI pass real handles, so leave those alone). Attach to the
        // parent console, or open one (an elevated exe started from a non-admin prompt gets a new
        // process with no parent console), and point the C runtime's stdout/stderr at it.
        bool ownConsole = false;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN) {
            bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
            if (!attached) attached = ownConsole = AllocConsole() != FALSE;
            if (attached) {   // with no console at all, leave stdio alone (a failed reopen closes it)
                FILE* f = nullptr;
                bool ok = _wfreopen_s(&f, L"CONOUT$", L"w", stdout) == 0;
                _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
                if (ok && !ownConsole) wprintf(L"\n");   // cmd already printed its prompt
            }
        }
        int init = PickInitial(cl);
        if (init < 0) return 1;
        wchar_t a0[] = L"--selftest";
        wchar_t* argv[] = {a0};
        int rc = modules_[init]->RunCli(1, argv);
        fflush(stdout);
        if (ownConsole) {   // our own window would close at once: keep the result readable
            wprintf(L"\nPress Enter to close.");
            fflush(stdout);
            HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
            if (in != INVALID_HANDLE_VALUE) {
                wchar_t buf[8]; DWORD got = 0;
                ReadConsoleW(in, buf, 8, &got, nullptr);
                CloseHandle(in);
            }
        }
        return rc;
    }

    static PFnSetDpiCtx setDpi =
        (PFnSetDpiCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setDpi) setDpi(HX_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    sys::InstallCrashHandler();
    sys::OpenLog();
    store_.Load();
    prefs_.reserve(modules_.size());
    for (auto& mod : modules_) prefs_.push_back(store_.Section(mod->Info().id));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Host::FrameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kFrameClass;
    RegisterClassExW(&wc);

    if (!cl.screenshot.empty()) {
        int rc = Screenshot(cl);
        store_.Save();
        sys::CloseLog();
        return rc;
    }

    dpi_ = DpiOf(nullptr);
    themeId_ = cl.theme;
    layout_  = cl.layout;
    {
        sys::Settings look = store_.Section(L"host");
        if (!cl.layoutGiven) ui::ParseLayout(look.GetStr(L"layout").c_str(), &layout_);
        if (!cl.themeGiven) ui::ParseTheme(look.GetStr(L"theme").c_str(), &themeId_);   // old fun/boring => Dark
        int tw = _wtoi(look.GetStr(L"tree_w", L"264").c_str());
        if (tw >= 180 && tw <= 480) treeW_ = tw;
    }
    interactive_ = true;
    {
        std::wstring a = store_.Section(L"update").GetStr(L"auto");
        updMode_ = a == L"on" ? UpdMode::On : a == L"off" ? UpdMode::Off : UpdMode::Ask;
    }
    BuildTheme(/*deterministic=*/false);
    int cw = S(1200, dpi_), ch = S(800, dpi_);
    RECT wa{0, 0, 0, 0}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - cw) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - ch) / 2;
    frame_ = CreateWindowExW(0, kFrameClass, caption_.c_str(), WS_OVERLAPPEDWINDOW,
                             x, y, cw, ch, nullptr, nullptr, wc.hInstance, this);
    if (!frame_) return 2;
    ApplyCaptionColour();
    Activate(PickInitial(cl));
    ShowWindow(frame_, SW_SHOW);
    UpdateWindow(frame_);
    if (updMode_ == UpdMode::On && cl.fixture.empty() && !snapshotArg) {   // never in test/fixture runs; at most every 20 h
        long long last = _wtoi64(store_.Section(L"update").GetStr(L"last", L"0").c_str());
        if (_time64(nullptr) - last > 20 * 3600) StartUpdateCheck(/*install=*/true);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) &&
            !(GetKeyState(VK_MENU) & 0x8000) && (msg.wParam == 'L' || msg.wParam == 'T')) {
            if (!(msg.lParam & (1 << 30)))   // ignore autorepeat: one switch per press
                msg.wParam == 'L' ? NextLayout(+1) : NextTheme(+1);
            continue;
        }
        // Ctrl+1..9 switches module (suite); Alt+Up/Down walks the Three-Pane tree.
        if (msg.message == WM_KEYDOWN && suite_ && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000) &&
            msg.wParam >= '1' && msg.wParam <= '9' && (int)(msg.wParam - '1') < (int)modules_.size()) {
            Activate((int)(msg.wParam - '1'));
            continue;
        }
        // Ctrl+Shift+Up/Down walks the Three-Pane tree (Alt+Up stays with the modules, e.g. DiskMap's "up a folder").
        if (msg.message == WM_KEYDOWN && Tree() && (msg.wParam == VK_UP || msg.wParam == VK_DOWN) &&
            (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            StepTree(msg.wParam == VK_DOWN ? +1 : -1);
            continue;
        }
        if (active_ >= 0 && modules_[active_]->PreTranslateMessage(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DWORD serr = 0;
    if (!store_.Save(&serr)) sys::Write(LogLevel::Warn, L"settings.ini not saved at exit: " + sys::ErrorText(serr));
    sys::CloseLog();
    CoUninitialize();
    return (int)msg.wParam;
}

}  // namespace

// --- public entry points ----------------------------------------------------------------------
HWND CreateModuleContent(IHost& host, HWND parent, const RECT& rc, Module* module) {
    (void)host;
    static std::set<std::wstring> registered;
    std::wstring cls = std::wstring(L"Hadron.") + module->Info().id + L".Content";
    if (registered.insert(cls).second) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &ContentProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls.c_str();
        RegisterClassExW(&wc);
    }
    auto* ctx = new ContentCtx{module};
    return CreateWindowExW(0, cls.c_str(), L"", WS_CHILD | WS_CLIPCHILDREN,
                           rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                           parent, nullptr, GetModuleHandleW(nullptr), ctx);
}

int RunStandalone(std::unique_ptr<Module> module) {
    std::vector<std::unique_ptr<Module>> v;
    v.push_back(std::move(module));
    Host host(std::move(v), /*suite=*/false);
    return host.Run(CommandLine::Parse(GetCommandLineW()));
}

int RunSuite(std::vector<std::unique_ptr<Module>> modules) {
    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    bool suite = cl.solo.empty();   // --solo runs just that module, as if standalone
    if (!cl.solo.empty()) {
        std::vector<std::unique_ptr<Module>> one;
        for (auto& m : modules)
            if (m->Info().id == cl.solo) { one.push_back(std::move(m)); break; }
        if (!one.empty()) modules = std::move(one);
    }
    Host host(std::move(modules), suite);
    return host.Run(cl);
}

}  // namespace hx
