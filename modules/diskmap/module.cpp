// modules/diskmap/module.cpp — DiskMap: a read-only disk-space visualiser (docs/design/diskmap.md).
//
// Three layouts over one scan (owner, 2026-11-11 and 2026-11-13):
//   Baseline   — the legacy look: toolbar, sidebar (current view, drive breakdown, Categories / Safe to
//                delete tabs, largest-files list), breadcrumb, squarified treemap.
//   Three-Pane — board 7: Categories and Safe to delete are NavNodes in the host's tree (selecting one
//                filters and highlights like the Baseline sidebar click); the middle is a slim toolbar,
//                a breadcrumb, the treemap and the current folder's list; the right pane (draggable
//                splitter, width saved as tp_detail_w) is the selection's detail plus the largest files.
//   Console    — board 4: no treemap and no cards. A filter line (/ search, show/cat chips, row n/N), a
//                path line, the current folder as a full-width table (NAME TYPE SIZE % FILES CATEGORY
//                SAFE) and a three-column detail along the bottom. The host draws the F-key bar
//                (FKeys) and the machine line (ConsoleHeader).
// The scan runs on a core/sys Worker (scan.cpp); the module never moves, deletes or reads a file.
//
// Window: the module registers its own content class "Hadron.diskmap.View" instead of calling
// CreateModuleContent, because that helper's window does not forward the worker's completion message
// (WM_MODULE_BASE+n), WM_TIMER, right/X-button clicks, WM_MOUSELEAVE or WM_SETCURSOR. Paint still goes
// through Module::Paint (WM_PAINT via a back buffer, WM_PRINTCLIENT and --screenshot), and input still
// goes through OnInput, so the contract's shape holds. Reported as a core gap.
#include "core/host/run.h"
#include "core/host/registry.h"
#include "core/host/cmdline.h"
#include "core/sys/settings.h"
#include "core/sys/sys.h"
#include "core/sys/worker.h"
#include "core/ui/draw.h"
#include "core/ui/widgets.h"
#include "modules/diskmap/classify.h"
#include "modules/diskmap/scan.h"
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace hx {
using namespace hx::ui;
using namespace hx::diskmap;

namespace {

constexpr UINT WM_DM_SCANDONE = WM_MODULE_BASE + 1;   // worker -> module (never WM_APP+1)
constexpr UINT_PTR TIMER_PROGRESS = 1;
constexpr int FILTER_SAFE = 100;                      // legacy filter encoding: 100 = all safe, 100+r one reason
constexpr size_t kLargestK = 14;
const wchar_t* const kViewClass = L"Hadron.diskmap.View";

// ---- colour and GDI helpers (legacy 265-323, over the theme) -----------------------------------
COLORREF Mix(COLORREF a, COLORREF b, double t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t),
               (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
COLORREF Lighten(COLORREF c, double t) { return Mix(c, RGB(255, 255, 255), t); }
COLORREF Darken(COLORREF c, double t) { return Mix(c, RGB(0, 0, 0), t); }
int Lum(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }

void FillC(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
void Outline(HDC dc, RECT r, COLORREF c, int width) {
    for (int i = 0; i < width; i++) { FrameRectC(dc, r, c); InflateRect(&r, -1, -1); }
}
void GradV(HDC dc, const RECT& r, COLORREF top, COLORREF bot) {
    TRIVERTEX v[2] = {
        {r.left, r.top, (COLOR16)(GetRValue(top) << 8), (COLOR16)(GetGValue(top) << 8), (COLOR16)(GetBValue(top) << 8), 0},
        {r.right, r.bottom, (COLOR16)(GetRValue(bot) << 8), (COLOR16)(GetGValue(bot) << 8), (COLOR16)(GetBValue(bot) << 8), 0}};
    GRADIENT_RECT g{0, 1};
    GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
}
constexpr UINT TL = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS;
constexpr UINT TR = DT_SINGLELINE | DT_VCENTER | DT_RIGHT;
constexpr UINT TC = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
constexpr UINT TP = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_PATH_ELLIPSIS;
void Text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) { DrawText(dc, s, r, f, c, fmt); }
int TextW(HDC dc, const std::wstring& s, HFONT f) {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

// ---- treemap layout (legacy Squarify 677-713, verbatim) ----------------------------------------
struct Item { double size; u32 id; u8 kind; };   // kind 0 file, 1 dir, 2 lump
struct RF { double x, y, w, h; };

void Squarify(const std::vector<Item>& it, double x, double y, double w, double h, std::vector<RF>& out) {
    out.assign(it.size(), RF{0, 0, 0, 0});
    double total = 0;
    for (auto& i : it) total += i.size;
    if (total <= 0 || w <= 0 || h <= 0) return;
    double scale = (w * h) / total;
    size_t start = 0;
    while (start < it.size()) {
        double side = std::min(w, h);
        double ss = side * side;
        size_t end = start; double rowSum = 0, best = 1e300;
        double rmax = it[start].size * scale;
        while (end < it.size()) {
            double s = it[end].size * scale;
            double nsum = rowSum + s;
            double worst = std::max(ss * rmax / (nsum * nsum), (nsum * nsum) / (ss * s));
            if (end > start && worst > best) break;
            best = worst; rowSum = nsum; end++;
        }
        if (w >= h) {
            double cw = rowSum / h, cy = y;
            for (size_t i = start; i < end; i++) { double ih = it[i].size * scale / cw; out[i] = {x, cy, cw, ih}; cy += ih; }
            x += cw; w -= cw;
        } else {
            double rh = rowSum / w, cx = x;
            for (size_t i = start; i < end; i++) { double iw = it[i].size * scale / rh; out[i] = {cx, y, iw, rh}; cx += iw; }
            y += rh; h -= rh;
        }
        start = end;
    }
}
RECT ToRect(double x, double y, double w, double h) {
    return RECT{(LONG)std::lround(x), (LONG)std::lround(y), (LONG)std::lround(x + w), (LONG)std::lround(y + h)};
}

// ---- small data types --------------------------------------------------------------------------
struct Drive { wchar_t letter; std::wstring label; u64 total, freeb; UINT type; };
struct Target { wchar_t letter = 0; std::wstring root, display; u64 used = 0, total = 0; bool fixture = false; };

// total is lpTotalNumberOfBytes (quota-limited) and freeb lpTotalNumberOfFreeBytes (volume-wide), so
// with disk quotas total < free is possible: clamp instead of wrapping a u64.
u64 UsedOf(const Drive& d) { return d.total > d.freeb ? d.total - d.freeb : 0; }
u64 FreeOf(const Target& t) { return t.total > t.used ? t.total - t.used : 0; }

// What "Not mapped" contains (deviation 6). Dehydrated cloud placeholders use almost no clusters, so
// they are not part of it; compressed/sparse/WOF files counted at logical size push it the other way.
// Alternate data streams are in it unless the scan ran with "Check for data streams".
std::wstring NotMappedWhy(bool streams) {
    return std::wstring(L"Not mapped = used space minus mapped bytes: NTFS metadata, unreadable folders, ") +
           (streams ? L"" : L"alternate data streams, ") +
           L"and the gap between file sizes and allocated clusters. Compressed, sparse and WOF files are counted "
           L"at full logical size, so this can read low or 0 B even though NTFS metadata exists. Hard links count "
           L"once; links aren't followed.";
}
// --streams: the "Check for data streams" option on from the start (headless runs included).
bool StreamsArg() {
    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    for (auto& a : cl.rest) if (a == L"--streams") return true;
    return false;
}
struct Click { RECT rc; int id; u32 data; };
struct Hit { RECT r; u32 id; u32 top; u8 kind; u8 topKind; };
struct Lump { u64 size = 0, count = 0; u64 cat[NCAT] = {}; u64 safeR[NSAFE] = {}; u8 dom = C_OTHER; };
struct ListE { u32 id; u8 kind; u64 size; };   // kind 0 file, 1 folder
struct RowRef { u8 kind; u32 id; };            // Console list row: 0 file, 1 folder

enum ClickId { ID_RESCAN = 1, ID_CANCEL = 2, ID_UP = 4, ID_DRIVE = 100, ID_CRUMB = 200, ID_CAT = 300,
               ID_CLEARFILTER = 500, ID_TAB = 600, ID_SAFE = 700, ID_STREAMS = 800, ID_SHOW = 900 };
enum class St { Idle, Scanning, Done };
enum class Focus { None, Click, Largest, List, Detail, Search };

// Three-Pane NavNode ids: the two sections, then one node per category / safe reason.
constexpr int NAV_CATS = 1, NAV_SAFE = 2, NAV_CAT0 = 10, NAV_REASON0 = 200;

// One row of a detail pane: a key/value pair, or a small-caps section header. Values wrap.
struct KV {
    enum Style : u8 { Plain, Head, Num, Good };
    std::wstring k, v;
    Style style = Plain;
};
// The selection's detail, grouped so Three-Pane can stack it and Console can split it into columns.
struct DetailModel {
    std::wstring title, kind;
    std::vector<KV> main, comp, safe, view;
};
// A scrollable, wrapping key/value area (Three-Pane detail, Console detail columns).
struct KVPane {
    ScrollBar sb;
    RECT rc{};
};

// Fonts the host doesn't supply (legacy g_fSmallSemi 12, g_fHuge 28, g_fBigNum 30), per DPI/layout.
struct ExtraFonts {
    HFONT smallSemi = nullptr, huge = nullptr, bigNum = nullptr;
    void Build(UINT dpi, Layout l) {
        Free();
        const wchar_t* face = l == Layout::Console ? L"Consolas" : L"Segoe UI";
        auto f = [&](int px, int w) {
            return CreateFontW(-S(px, dpi), 0, 0, 0, w, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        };
        smallSemi = f(12, FW_SEMIBOLD);
        huge = f(28, FW_LIGHT);
        bigNum = f(l == Layout::Console ? 24 : 30, FW_LIGHT);
    }
    void Free() { for (HFONT* h : {&smallSemi, &huge, &bigNum}) if (*h) { DeleteObject(*h); *h = nullptr; } }
    ~ExtraFonts() { Free(); }
};

std::wstring Pct(u64 part, u64 whole) {
    wchar_t b[24];
    swprintf(b, 24, L"%.1f%%", whole ? 100.0 * (double)part / (double)whole : 0.0);
    return b;
}

// ============================================================================================
class DiskMapModule : public Module {
public:
    ~DiskMapModule() override { FreeMap(); }

    const ModuleInfo& Info() const override {
        static const ModuleInfo info{L"diskmap", L"DiskMap",
                                     L"Drive-space treemap and folder list, with safe-to-delete hints. Read-only.",
                                     Admin::Required};
        return info;
    }

    HWND Create(IHost& host, HWND parent, const RECT& rc) override;
    void OnActivate() override { if (hwnd_) SetFocus(hwnd_); }
    void OnDpiChanged(UINT dpi) override {   // also called on a layout switch
        if (host_) extra_.Build(dpi, host_->Layout());
        mapDirty_ = true;
        if ((Console() || Three()) && (focus_ == Focus::None || focus_ == Focus::Click)) focus_ = Focus::List;
        if (!Console() && !Three() && (focus_ == Focus::List || focus_ == Focus::Search || focus_ == Focus::Detail)) focus_ = Focus::None;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    bool PreTranslateMessage(MSG& m) override;
    bool OnInput(UINT msg, WPARAM w, LPARAM l) override;
    std::vector<NavNode> NavNodes() override;
    int  NavSelected() override;
    void OnNavSelect(int id) override;
    std::vector<FKey> FKeys() override;
    std::wstring ConsoleHeader() override;
    void Shutdown() override {
        worker_.Cancel();
        worker_.Join();   // must not return while the scan thread is alive (contract)
        if (hwnd_) KillTimer(hwnd_, TIMER_PROGRESS);
    }
    int  RunCli(int argc, wchar_t** argv) override;
    void LoadFixture(const wchar_t* dir) override;
    void Paint(HDC dc, const RECT& rc) override;

private:
    static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnView(HWND, UINT, WPARAM, LPARAM);

    PaintCtx Ctx() const { return PaintCtx{host_->Theme(), host_->Fonts(), host_->Dpi(), host_->Layout()}; }
    int  Sc(int px) const { return S(px, host_ ? host_->Dpi() : 96); }
    double AreaScale() const { double k = (host_ ? host_->Dpi() : 96) / 96.0; return k * k; }   // px² thresholds
    bool Console() const { return host_ && host_->Layout() == Layout::Console; }
    bool Three() const { return host_ && host_->Layout() == Layout::ThreePane; }
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    // The host's tree, F-key bar and header line follow our state; Baseline shows none of them.
    void Chrome() { if (host_ && hwnd_ && host_->Layout() != Layout::Baseline) host_->ChromeChanged(); }

    // data
    void RefreshDrives();
    void StartScan(const Target& t);
    void LaunchWorker();
    void OnScanDone(bool cancelled);
    void EnterDone();
    void SetView(u32 d, u32 selectChild = UINT32_MAX);
    void GoUp() { if (st_ == St::Done && view_ != 0) SetView(data_->dirs[view_].parent, view_); }
    void ComputeLargest();
    void FillLargestList();
    void BuildConsoleRows(u32 selectDir = UINT32_MAX);
    void SortConsoleRows();
    bool RowVisible(const RowRef& r) const;
    void SyncDetail();
    DetailModel BuildDetail() const;
    void SetFilter(int tab, int filter);
    std::wstring NoticeText() const;
    void OpenSelected(bool explorer);
    bool SafeMatch(u8 r) const {
        if (filter_ == FILTER_SAFE) return r != SF_NONE;
        if (filter_ > FILTER_SAFE) return r == filter_ - FILTER_SAFE;
        return false;
    }
    void ShowInExplorer(const std::wstring& path, bool select);
    void UpdateStatus();
    std::wstring SummaryText() const;
    std::wstring HoverText() const;

    // painting
    void LayoutRects(const RECT& rc);
    void Button(HDC dc, RECT r, const wchar_t* label, int id, bool primary, bool enabled = true);
    void DrawLogo(HDC dc, int x, int y, int s);
    void DrawToolbar(HDC dc);
    void SectionLabel(HDC dc, const wchar_t* s, int& y);
    void DrawSidebar(HDC dc);
    void DrawNotice(HDC dc, int& y, int L, int R);
    void DrawWrapped(HDC dc, const std::wstring& s, int L, int R, int& y, COLORREF c);
    std::wstring SkippedText() const;
    void DrawCrumb(HDC dc, const RECT& strip, int x);
    void DrawProgress(HDC dc, const RECT& area);
    void DrawEmpty(HDC dc, const RECT& area);
    void DrawMapArea(HDC dc);
    void DrawConsole(HDC dc, const RECT& rc);
    void DrawThree(HDC dc);
    void DrawToolbarTP(HDC dc);
    void DrawDetailTP(HDC dc);
    void DrawFilterLine(HDC dc);
    int  DrawPathCrumb(HDC dc, const RECT& strip, int x, int right, HFONT f, const wchar_t* sep);
    void SqButton(HDC dc, RECT r, const std::wstring& label, int id, u32 data, bool on, bool enabled = true);
    void Chip(HDC dc, int& x, int cy, const std::wstring& label, bool on, int id, u32 data);
    int  PaintKV(HDC dc, const std::vector<KV>& rows, const RECT& area, int scroll, bool draw);
    void PaintPane(HDC dc, KVPane& p, const std::vector<KV>& rows);
    RECT PaneTrack(const KVPane& p) const { return RECT{p.rc.right - Sc(8), p.rc.top, p.rc.right, p.rc.bottom}; }
    COLORREF Acc() const { const Theme& t = host_->Theme(); return t.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : t.accent; }
    COLORREF OnAcc() const { const Theme& t = host_->Theme(); return t.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : OnFill(t.accent); }
    void DrawFocus(HDC dc);
    void AddClick(const RECT& r, int id, u32 data = 0) { clicks_.push_back({r, id, data}); }
    bool IsHover(int id, u32 data = 0) const { return hoverId_ == id && hoverData_ == data; }

    // treemap
    bool FileMatch(const FileE& f) const {
        if (filter_ < 0) return true;
        if (filter_ < NCAT) return f.cat == filter_;
        return SafeMatch(f.safe);
    }
    COLORREF FileColor(const FileE& f) const {
        if (!FileMatch(f)) return Mix(CatColor(f.cat), host_->Theme().bg, 0.84);
        return filter_ >= FILTER_SAFE ? SAFE_COL : CatColor(f.cat);
    }
    COLORREF MixColor(const u64* cats, const u64* safeR, u64 total, u8 dom) const;
    void Leaf(HDC dc, RECT r, COLORREF c);
    void LeafLabel(HDC dc, RECT r, const wchar_t* name, u64 size, bool dim);
    void DrawDir(HDC dc, u32 di, double x, double y, double w, double h, int depth, u32 top, u8 topKind);
    void RenderMap(HDC ref);
    void FreeMap() {
        if (mapDC_) { SelectObject(mapDC_, mapOld_); DeleteObject(mapBmp_); DeleteDC(mapDC_); }
        mapDC_ = nullptr; mapBmp_ = nullptr; mapOld_ = nullptr; mapW_ = mapH_ = 0;
    }

    // input
    void UpdateHover(int x, int y);
    void OnClick(int x, int y, bool ctrl);
    void ActivateClick(const Click& c, bool ctrl);
    bool OnKey(WPARAM vk, bool alt);
    void CycleFocus(bool back);

    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;
    ExtraFonts extra_;
    bool elevated_ = true;
    bool streams_ = false;   // "Check for data streams" (off by default; --streams turns it on)

    St st_ = St::Idle;
    sys::Worker worker_;
    ScanProgress prog_;
    std::unique_ptr<Scan> pending_;   // owned by the worker while scanning; UI touches it only after Join
    std::unique_ptr<Scan> data_;      // the finished scan the UI shows
    Target target_, queued_;
    bool haveQueued_ = false;
    ULONGLONG scanStart_ = 0;
    std::wstring lastError_;
    std::vector<Drive> drives_;

    u32 view_ = 0;
    int filter_ = -1;
    int sideTab_ = 0;

    std::vector<Click> clicks_;
    int hoverId_ = -1; u32 hoverData_ = 0; bool hoverClick_ = false;
    std::vector<Hit> hits_;
    std::vector<Lump> lumps_;
    int hoverHit_ = -1;
    bool hoverTopValid_ = false; RECT hoverTopRect_{};
    bool tracking_ = false;
    std::wstring status_;

    RECT rcTool_{}, rcSide_{}, rcCrumb_{}, rcMap_{}, rcLargest_{}, rcList_{}, rcDetail_{}, rcBody_{};
    RECT rcSearch_{}, rcSplit_{}, rcFilter_{}, rcPath_{};
    HDC mapDC_ = nullptr; HBITMAP mapBmp_ = nullptr, mapOld_ = nullptr; int mapW_ = 0, mapH_ = 0;
    bool mapDirty_ = true;

    std::vector<ListE> largest_;
    ListView largestList_;
    std::vector<RowRef> rows_;
    ListView list_;
    DetailModel dm_;                 // detail of the list selection (Three-Pane and Console)
    KVPane tpPane_, conPane_[3];
    SearchBox search_;               // name filter over the current folder's list (Three-Pane, Console)
    bool skipSlash_ = false;         // the '/' that focused the search box must not be typed into it
    int detailW96_ = 360;            // Three-Pane detail width (96-DPI px), Prefs tp_detail_w
    bool splitDrag_ = false; int splitX0_ = 0, splitW0_ = 0;
    ULONGLONG chromeTick_ = 0;       // last Console header refresh while scanning
    Focus focus_ = Focus::None;
    int focusId_ = -1; u32 focusData_ = 0;
};

// ============================================================================================
// Window

LRESULT CALLBACK DiskMapModule::ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = reinterpret_cast<DiskMapModule*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->OnView(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT DiskMapModule::OnView(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, rc.right, rc.bottom) : nullptr;
            if (mem && bmp) {
                HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
                Paint(mem, rc);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
            } else {
                Paint(dc, rc);   // GDI exhausted: paint unbuffered rather than blit an empty 1x1 bitmap
            }
            if (bmp) DeleteObject(bmp);
            if (mem) DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: { RECT rc; GetClientRect(h, &rc); Paint((HDC)w, rc); return 0; }
    case WM_SIZE: mapDirty_ = true; Invalidate(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT cp; GetCursorPos(&cp); ScreenToClient(h, &cp);
            if (Three() && (splitDrag_ || PtInRect(&rcSplit_, cp))) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            if (PtInRect(&rcSearch_, cp) && (Three() || Console()) && st_ == St::Done) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM)); return TRUE;
            }
            bool hand = hoverClick_ || (hoverHit_ >= 0 && hoverHit_ < (int)hits_.size() && hits_[hoverHit_].topKind == 1);
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hoverId_ != -1 || hoverHit_ != -1) {
            hoverId_ = -1; hoverData_ = 0; hoverClick_ = false; hoverHit_ = -1; hoverTopValid_ = false;
            UpdateStatus(); Invalidate();
        }
        return 0;
    case WM_TIMER:
        if (w == TIMER_PROGRESS && st_ == St::Scanning) {
            Invalidate();
            if (Console() && GetTickCount64() - chromeTick_ >= 1000) { chromeTick_ = GetTickCount64(); Chrome(); }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (splitDrag_) { splitDrag_ = false; host_->Prefs().SetInt(L"tp_detail_w", detailW96_); }
        search_.CancelDrag();
        return 0;
    case WM_DM_SCANDONE:
        OnScanDone(w != 0);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SYSKEYDOWN:   // Alt+Home / Alt+Up / Alt+Left; anything else (Alt+F4...) goes to DefWindowProc
        if (OnInput(m, w, l)) { Invalidate(); return 0; }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(h);
        [[fallthrough]];
    case WM_LBUTTONUP: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_CHAR:
    case WM_RBUTTONDOWN: case WM_XBUTTONDOWN:
        if (m == WM_MOUSEMOVE && !tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        if (OnInput(m == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : m, w, l)) Invalidate();
        return m == WM_XBUTTONDOWN ? TRUE : 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND DiskMapModule::Create(IHost& host, HWND parent, const RECT& rc) {
    host_ = &host;
    elevated_ = host.IsElevated();
    extra_.Build(host.Dpi(), host.Layout());
    // SeBackupPrivilege (deviation 5) is enabled per scan, on the scan thread only (Scan::Run), so it
    // never reaches the process token, other modules or child processes.
    RefreshDrives();

    detailW96_ = std::max(240, std::min(900, host.Prefs().GetInt(L"tp_detail_w", 360)));
    search_.onChanged = [this](const std::wstring&) { BuildConsoleRows(); UpdateStatus(); Invalidate(); };
    list_.onSortChanged = [this](int, bool) { SortConsoleRows(); SyncDetail(); };
    list_.onActivate = [this](int i) {
        if (i >= 0 && i < (int)rows_.size() && rows_[i].kind == 1) SetView(rows_[i].id);
    };
    largestList_.onSortChanged = [this](int, bool) { FillLargestList(); };
    largestList_.onActivate = [this](int i) {
        if (i < 0 || i >= (int)largest_.size() || !data_) return;
        const ListE& e = largest_[i];
        if (e.kind == 0) ShowInExplorer(data_->FilePath(e.id), true);
        else ShowInExplorer(data_->DirPath(e.id), false);
    };

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &DiskMapModule::ViewProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kViewClass;
        if (!RegisterClassExW(&wc)) {
            std::wstring m = L"diskmap: RegisterClassEx failed: " + sys::ErrorText(GetLastError());
            host.Log(LogLevel::Error, m.c_str());
            return nullptr;
        }
        registered = true;
    }
    hwnd_ = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, parent, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        std::wstring m = L"diskmap: CreateWindowEx failed: " + sys::ErrorText(GetLastError());
        host.Log(LogLevel::Error, m.c_str());
        return nullptr;
    }
    if (Console() || Three()) focus_ = Focus::List;
    streams_ = StreamsArg();

    // Optional "--drive X": start scanning that drive (validated against the drive list). Not in
    // --screenshot/--selftest, which load a fixture instead.
    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    if (cl.screenshot.empty() && !cl.selftest) {
        for (size_t i = 0; i + 1 < cl.rest.size(); i++) {
            if (cl.rest[i] != L"--drive" || cl.rest[i + 1].empty()) continue;
            wchar_t want = (wchar_t)towupper(cl.rest[i + 1][0]);
            for (auto& d : drives_) {
                if (d.letter != want) continue;
                Target t; t.letter = d.letter;
                t.display = std::wstring(1, d.letter) + L":\\";
                t.root = L"\\\\?\\" + t.display;
                t.used = UsedOf(d); t.total = d.total;
                StartScan(t);
            }
            if (st_ == St::Idle) lastError_ = L"No fixed or removable drive " + std::wstring(1, want) + L": to scan.";
        }
    }
    return hwnd_;
}

bool DiskMapModule::PreTranslateMessage(MSG& m) {
    // Keys typed while the host frame has focus (standalone start-up) still reach DiskMap. Ctrl
    // combinations are left alone: the host owns Ctrl+Tab and Ctrl+1..9.
    if (!hwnd_ || !host_ || m.hwnd != host_->Frame()) return false;
    // Only keys DiskMap actually handles are consumed; anything else still reaches the host.
    if ((m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (!OnInput(m.message, m.wParam, m.lParam)) return false;
        SetFocus(hwnd_);
        Invalidate();
        return true;
    }
    return false;
}

// ============================================================================================
// Data

void DiskMapModule::RefreshDrives() {
    drives_.clear();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER fa, tot, fr;
        if (!GetDiskFreeSpaceExW(root, &fa, &tot, &fr)) continue;   // e.g. empty card reader
        wchar_t label[64] = {};
        std::wstring lab;
        if (GetVolumeInformationW(root, label, 64, nullptr, nullptr, nullptr, nullptr, 0)) lab = label;
        if (lab.empty()) lab = t == DRIVE_REMOVABLE ? L"Removable" : L"Local Disk";
        drives_.push_back({root[0], lab, tot.QuadPart, fr.QuadPart, t});
    }
}

void DiskMapModule::StartScan(const Target& t) {
    // A scan is running: ask it to stop and start the new one when its completion arrives. The UI
    // thread never waits for the worker (legacy blocked with INFINITE here).
    if (worker_.Running()) {
        queued_ = t; haveQueued_ = true;
        worker_.Cancel();
        target_ = t;
        Invalidate();
        return;
    }
    target_ = t;
    LaunchWorker();
}

void DiskMapModule::LaunchWorker() {
    data_.reset();
    hits_.clear(); lumps_.clear(); largest_.clear(); rows_.clear();
    hoverHit_ = -1; hoverTopValid_ = false;
    lastError_.clear();
    view_ = 0;
    filter_ = sideTab_ == 1 ? FILTER_SAFE : -1;
    pending_ = std::make_unique<Scan>();
    prog_.Reset();
    scanStart_ = GetTickCount64();
    Scan* scan = pending_.get();
    std::wstring root = target_.root, display = target_.display;
    sys::Worker* wk = &worker_;
    ScanProgress* pg = &prog_;
    bool streams = streams_;
    bool ok = worker_.Start(hwnd_, WM_DM_SCANDONE, [scan, root, display, wk, pg, streams] {
        scan->Run(root, display, [wk] { return wk->Cancelled(); }, pg, streams);
    }, 64u << 20);
    if (!ok) {
        lastError_ = L"Could not start the scan thread: " + sys::ErrorText(GetLastError());
        pending_.reset();
        st_ = St::Idle;
        Invalidate();
        return;
    }
    st_ = St::Scanning;
    host_->SetBusy(true);
    SetTimer(hwnd_, TIMER_PROGRESS, 100, nullptr);
    mapDirty_ = true;
    UpdateStatus();
    Invalidate();
    Chrome();
}

void DiskMapModule::OnScanDone(bool cancelled) {
    worker_.Join();   // the thread has posted its last message; this returns at once
    KillTimer(hwnd_, TIMER_PROGRESS);
    host_->SetBusy(false);
    std::unique_ptr<Scan> s = std::move(pending_);
    if (haveQueued_) {   // a drive was clicked mid-scan: start it now
        haveQueued_ = false;
        target_ = queued_;
        LaunchWorker();
        return;
    }
    RefreshDrives();
    if (cancelled || !s) { st_ = St::Idle; UpdateStatus(); Invalidate(); Chrome(); return; }   // partial data discarded
    if (!s->ok) {
        lastError_ = L"Could not read " + target_.display + L": " + sys::ErrorText(s->rootErr);
        st_ = St::Idle; UpdateStatus(); Invalidate(); Chrome();
        return;
    }
    data_ = std::move(s);
    EnterDone();
}

void DiskMapModule::EnterDone() {
    st_ = St::Done;
    view_ = 0;
    mapDirty_ = true;
    search_.SetText(L"");
    ComputeLargest();
    BuildConsoleRows();
    UpdateStatus();
    Invalidate();
    Chrome();
}

void DiskMapModule::SetView(u32 d, u32 selectChild) {
    if (st_ != St::Done || !data_ || d >= data_->dirs.size()) return;
    view_ = d;
    mapDirty_ = true;
    hoverHit_ = -1; hoverTopValid_ = false;
    search_.SetText(L"");   // the name filter is per folder; SetText does not fire onChanged
    if (focus_ == Focus::Search) focus_ = Focus::List;
    ComputeLargest();
    BuildConsoleRows(selectChild);
    UpdateStatus();
    Invalidate();
    Chrome();   // the tree's category and safe sizes are the view's
}

// Category / safe-reason filter, shared by the Baseline sidebar, the Three-Pane tree and Console chips.
void DiskMapModule::SetFilter(int tab, int filter) {
    // The largest-files list depends on the tab and the safe filter only, so a category click leaves it
    // (and its scroll and selection) alone, as the Baseline sidebar always did.
    bool largest = tab != sideTab_ || tab == 1 || filter < 0;
    sideTab_ = tab;
    filter_ = filter;
    mapDirty_ = true;
    if (largest) ComputeLargest();
    BuildConsoleRows();
    UpdateStatus();
    Invalidate();
    Chrome();
}

// Enter opens the selected folder; F3 / Ctrl+Enter shows the selection in Explorer.
void DiskMapModule::OpenSelected(bool explorer) {
    int i = list_.Selected();
    if (st_ != St::Done || !data_ || i < 0 || i >= (int)rows_.size()) return;
    const RowRef r = rows_[i];
    if (explorer) ShowInExplorer(r.kind ? data_->DirPath(r.id) : data_->FilePath(r.id), true);
    else if (r.kind == 1) SetView(r.id);
}

// Legacy ComputeLargest (586-638).
void DiskMapModule::ComputeLargest() {
    largest_.clear();
    if (st_ != St::Done || !data_ || data_->dirs.empty()) { FillLargestList(); return; }
    const Scan& s = *data_;
    if (sideTab_ == 1) {
        std::vector<ListE> all;
        const DirE& v = s.dirs[view_];
        if (v.safe) {
            if (SafeMatch(v.safe)) {
                for (u32 k = 0; k < v.kidCount; k++) { u32 c = s.KidsOf(v)[k]; if (s.dirs[c].total) all.push_back({c, 1, s.dirs[c].total}); }
                for (u32 f = v.fileFirst; f < v.fileFirst + v.fileCount; f++)
                    if (s.files[f].size) all.push_back({f, 0, s.files[f].size});
            }
        } else {
            std::vector<u32> st{view_};
            while (!st.empty()) {
                u32 d = st.back(); st.pop_back();
                const DirE& de = s.dirs[d];
                for (u32 f = de.fileFirst; f < de.fileFirst + de.fileCount; f++)
                    if (SafeMatch(s.files[f].safe) && s.files[f].size) all.push_back({f, 0, s.files[f].size});
                for (u32 k = 0; k < de.kidCount; k++) {
                    u32 ci = s.KidsOf(de)[k];
                    const DirE& c = s.dirs[ci];
                    if (c.safe) { if (SafeMatch(c.safe) && c.total) all.push_back({ci, 1, c.total}); }
                    else if (SafeSum(c.safeR)) st.push_back(ci);
                }
            }
        }
        size_t n = std::min(kLargestK, all.size());
        std::partial_sort(all.begin(), all.begin() + n, all.end(), [](const ListE& a, const ListE& b) { return a.size > b.size; });
        all.resize(n);
        largest_ = all;
    } else {
        auto cmp = [&s](u32 a, u32 b) { return s.files[a].size > s.files[b].size; };   // min-heap on size
        std::vector<u32> heap, stack{view_};
        while (!stack.empty()) {
            u32 d = stack.back(); stack.pop_back();
            const DirE& de = s.dirs[d];
            for (u32 k = 0; k < de.fileCount && k < kLargestK; k++) {   // files are sorted desc
                u32 f = de.fileFirst + k;
                if (!s.files[f].size) break;
                if (heap.size() < kLargestK) { heap.push_back(f); std::push_heap(heap.begin(), heap.end(), cmp); }
                else if (s.files[f].size > s.files[heap.front()].size) {
                    std::pop_heap(heap.begin(), heap.end(), cmp); heap.back() = f;
                    std::push_heap(heap.begin(), heap.end(), cmp);
                } else break;
            }
            for (u32 k = 0; k < de.kidCount; k++) stack.push_back(s.KidsOf(de)[k]);
        }
        std::sort(heap.begin(), heap.end(), cmp);
        for (u32 f : heap) largest_.push_back({f, 0, s.files[f].size});
    }
    FillLargestList();
}

void DiskMapModule::FillLargestList() {
    // The sidebar list is a core/ui ListView (sortable, scrollable, keyboard) instead of the legacy's
    // fixed 14 rows; it keeps the same items. Default order: size descending.
    int col = largestList_.SortColumn(); bool asc = largestList_.SortAscending();
    if (col >= 0 && data_) {
        const Scan& s = *data_;
        auto name = [&](const ListE& e) { return std::wstring(e.kind ? s.Name(s.dirs[e.id].name) : s.Name(s.files[e.id].name)); };
        std::stable_sort(largest_.begin(), largest_.end(), [&](const ListE& a, const ListE& b) {
            int r = col == 0 ? _wcsicmp(name(a).c_str(), name(b).c_str()) : (a.size < b.size ? -1 : a.size > b.size ? 1 : 0);
            return asc ? r < 0 : r > 0;
        });
    }
    std::vector<std::vector<std::wstring>> rows;
    if (data_) {
        for (auto& e : largest_) {
            std::wstring n = e.kind ? std::wstring(data_->Name(data_->dirs[e.id].name)) + L"\\" : data_->Name(data_->files[e.id].name);
            rows.push_back({n, FmtSize(e.size)});
        }
    }
    largestList_.SetRows(std::move(rows));
}

void DiskMapModule::BuildConsoleRows(u32 selectDir) {
    rows_.clear();
    if (st_ == St::Done && data_) {
        const DirE& v = data_->dirs[view_];
        for (u32 k = 0; k < v.kidCount; k++) { RowRef r{1, data_->KidsOf(v)[k]}; if (RowVisible(r)) rows_.push_back(r); }
        for (u32 f = v.fileFirst; f < v.fileFirst + v.fileCount; f++) { RowRef r{0, f}; if (RowVisible(r)) rows_.push_back(r); }
    }
    SortConsoleRows();
    // Reset the list to the top (ListView has no public scroll reset: Home does it), then move the
    // selection to the folder we came up from, if any.
    PaintCtx c = Ctx();
    RECT lr = rcList_;
    if (lr.bottom <= lr.top) lr = RECT{0, 0, 800, 600};
    list_.Select(0);
    list_.OnKey(VK_HOME, lr, c);
    if (selectDir != UINT32_MAX) {
        for (size_t i = 0; i < rows_.size(); i++) {
            if (rows_[i].kind == 1 && rows_[i].id == selectDir) {
                for (size_t k = 0; k < i; k++) list_.OnKey(VK_DOWN, lr, c);
                break;
            }
        }
    }
    SyncDetail();
}

// List rows (Three-Pane, Console): the search text, then the category / safe filter. A folder passes a
// category filter when it holds any bytes of that category, and a safe filter when it matches or holds
// matching bytes, so the filter never hides where the highlighted space is.
bool DiskMapModule::RowVisible(const RowRef& r) const {
    const Scan& s = *data_;
    const std::wstring& q = search_.Text();
    if (!q.empty()) {
        std::wstring n = r.kind ? s.Name(s.dirs[r.id].name) : s.Name(s.files[r.id].name), lq = q;
        for (auto& ch : n) ch = (wchar_t)towlower(ch);
        for (auto& ch : lq) ch = (wchar_t)towlower(ch);
        if (n.find(lq) == std::wstring::npos) return false;
    }
    if (filter_ < 0) return true;
    if (r.kind == 0) return FileMatch(s.files[r.id]);
    const DirE& d = s.dirs[r.id];
    if (filter_ < NCAT) return d.cat[filter_] > 0;
    if (SafeMatch(d.safe)) return true;
    return (filter_ == FILTER_SAFE ? SafeSum(d.safeR) : d.safeR[filter_ - FILTER_SAFE]) > 0;
}

void DiskMapModule::SortConsoleRows() {
    RowRef keep{255, 0};
    int sel = list_.Selected();
    if (sel >= 0 && sel < (int)rows_.size()) keep = rows_[sel];
    if (data_) {
        const Scan& s = *data_;
        auto size = [&](const RowRef& r) { return r.kind ? s.dirs[r.id].total : s.files[r.id].size; };
        auto name = [&](const RowRef& r) { return r.kind ? s.Name(s.dirs[r.id].name) : s.Name(s.files[r.id].name); };
        auto type = [&](const RowRef& r) -> std::wstring {
            if (r.kind) return L"Folder";
            const FileE& f = s.files[r.id];
            if (f.flags & FF_REPARSE_DIR) return L"Link to folder";
            return CatName(f.cat);
        };
        // Console-only columns: FILES (recursive count), CATEGORY (a folder's dominant one) and SAFE (the
        // rule's name, or the matched bytes inside a folder).
        auto files = [&](const RowRef& r) -> u64 { return r.kind ? s.dirs[r.id].nFiles : 0; };
        auto cat = [&](const RowRef& r) -> std::wstring { return CatName(r.kind ? s.dirs[r.id].dom : s.files[r.id].cat); };
        auto safe = [&](const RowRef& r) -> std::wstring {
            u8 own = r.kind ? s.dirs[r.id].safe : s.files[r.id].safe;
            if (own) return SafeReason(own).name;
            if (r.kind && SafeSum(s.dirs[r.id].safeR)) return FmtSize(SafeSum(s.dirs[r.id].safeR)) + L" inside";
            return L"";
        };
        auto safeKey = [&](const RowRef& r) -> u64 { return r.kind ? SafeSum(s.dirs[r.id].safeR) : (s.files[r.id].safe ? s.files[r.id].size : 0); };
        auto cmp3 = [](u64 a, u64 b) { return a < b ? -1 : a > b ? 1 : 0; };
        int col = list_.SortColumn(); bool asc = list_.SortAscending();
        std::stable_sort(rows_.begin(), rows_.end(), [&](const RowRef& a, const RowRef& b) {
            int r;
            switch (col) {
                case 0: r = _wcsicmp(name(a), name(b)); break;
                case 1: r = type(a).compare(type(b)); break;
                case -1: return size(a) > size(b);   // default: biggest first, folders and files mixed
                case 4: r = cmp3(files(a), files(b)); break;
                case 5: r = cat(a).compare(cat(b)); break;
                case 6: r = cmp3(safeKey(a), safeKey(b)); break;
                default: r = cmp3(size(a), size(b)); break;
            }
            return asc ? r < 0 : r > 0;
        });
        std::vector<std::vector<std::wstring>> cells;
        std::vector<ListView::Ink> ink;
        u64 tot = s.dirs[view_].total;
        for (auto& r : rows_) {
            std::wstring n = name(r);
            if (r.kind) n += L"\\";
            std::wstring t = type(r);
            if (!r.kind) {
                const FileE& f = s.files[r.id];
                if (f.flags & FF_HARDLINK_DUP) t += L" (hard link)";
                else if ((f.flags & FF_REPARSE) && !(f.flags & FF_REPARSE_DIR)) t += L" (link)";
            } else if (s.dirs[r.id].flags & DF_DENIED) t = L"Folder (unreadable)";
            cells.push_back({n, t, FmtSize(size(r)), Pct(size(r), tot), r.kind ? U64Str(files(r)) : std::wstring(), cat(r), safe(r)});
            u8 own = r.kind ? s.dirs[r.id].safe : s.files[r.id].safe;
            ink.push_back(own ? ListView::Ink::Good : (r.kind && (s.dirs[r.id].flags & DF_DENIED)) ? ListView::Ink::Warn : ListView::Ink::Normal);
        }
        list_.SetRows(std::move(cells));
        if (Console()) list_.SetRowInk(std::move(ink));   // the SAFE column says it in text too
    } else {
        list_.SetRows({});
    }
    if (keep.kind != 255)
        for (size_t i = 0; i < rows_.size(); i++)
            if (rows_[i].kind == keep.kind && rows_[i].id == keep.id) { list_.Select((int)i); break; }
}

void DiskMapModule::SyncDetail() {
    dm_ = BuildDetail();
    tpPane_.sb.SetOffset(0);
    for (auto& p : conPane_) p.sb.SetOffset(0);
}

// The detail of the list selection (or of the current folder when nothing is selected), in four groups:
// main facts, composition by category, the safe-to-delete verdict with its basis (rule 7), and the
// current view (drive split, skipped items, the degraded-mode notice).
DetailModel DiskMapModule::BuildDetail() const {
    DetailModel m;
    if (st_ != St::Done || !data_) return m;
    const Scan& s = *data_;
    auto bytes = [](u64 v) { return FmtSize(v) + L"  (" + U64Str(v) + L" bytes)"; };
    auto safeRows = [&](u8 reason, const u64* safeR) {
        if (reason) {
            const SafeInfo& si = SafeReason(reason);
            m.safe.push_back({L"Rule", si.name, KV::Good});
            m.safe.push_back({L"Why", si.desc});
            m.safe.push_back({L"Basis", std::wstring(si.basis) + L" \x2014 " + si.basisNote});
        } else if (safeR && SafeSum(safeR)) {
            m.safe.push_back({L"Inside", FmtSize(SafeSum(safeR)) + L" matched", KV::Good});
            for (int r = 1; r < NSAFE; r++)
                if (safeR[r]) m.safe.push_back({SafeReason(r).name, FmtSize(safeR[r]), KV::Num});
        } else {
            m.safe.push_back({L"Rule", L"No rule matched"});
        }
        m.safe.push_back({L"", L"Rules match by folder and file name, so check before deleting. DiskMap never deletes anything itself."});
    };
    auto comp = [&](const u64* cat, u64 total) {
        int order[NCAT]; for (int i = 0; i < NCAT; i++) order[i] = i;
        std::stable_sort(order, order + NCAT, [&](int a, int b) { return cat[a] > cat[b]; });
        for (int i = 0; i < NCAT && cat[order[i]]; i++)
            m.comp.push_back({CatName(order[i]), FmtSize(cat[order[i]]) + L"  " + Pct(cat[order[i]], total), KV::Num});
    };
    const DirE& v = s.dirs[view_];
    int sel = list_.Selected();
    if (sel < 0 || sel >= (int)rows_.size()) {
        m.title = view_ == 0 ? s.DirPath(0) : s.Name(v.name);
        m.kind = L"Current folder";
        m.main.push_back({L"Size", bytes(v.total), KV::Num});
        m.main.push_back({L"Files", U64Str(v.nFiles), KV::Num});
        m.main.push_back({L"Folders", U64Str(v.nDirs), KV::Num});
        comp(v.cat, v.total);
        safeRows(v.safe, v.safeR);
    } else if (rows_[sel].kind == 1) {
        u32 id = rows_[sel].id;
        const DirE& d = s.dirs[id];
        m.title = std::wstring(s.Name(d.name)) + L"\\";
        m.kind = L"Folder";
        m.main.push_back({L"Path", s.DirPath(id)});
        m.main.push_back({L"Size", bytes(d.total), KV::Num});
        m.main.push_back({L"Share of view", Pct(d.total, v.total), KV::Num});
        m.main.push_back({L"Files", U64Str(d.nFiles), KV::Num});
        m.main.push_back({L"Folders", U64Str(d.nDirs), KV::Num});
        m.main.push_back({L"Mostly", CatName(d.dom)});
        if (d.flags & DF_DENIED) m.main.push_back({L"Unreadable", sys::ErrorText(d.err)});
        comp(d.cat, d.total);
        safeRows(d.safe, d.safeR);
    } else {
        u32 id = rows_[sel].id;
        const FileE& f = s.files[id];
        m.title = s.Name(f.name);
        m.kind = L"File";
        m.main.push_back({L"Path", s.FilePath(id)});
        m.main.push_back({L"Size", bytes(f.size), KV::Num});
        m.main.push_back({L"Share of view", Pct(f.size, v.total), KV::Num});
        m.main.push_back({L"Category", CatName(f.cat)});
        if (f.flags & FF_HARDLINK_DUP)
            m.main.push_back({L"Hard link", L"Same file as another link already counted; shown as 0 B so it counts once"});
        if (f.flags & FF_REPARSE) {
            wchar_t tag[32]; swprintf(tag, 32, L"0x%08X", f.tag);
            m.main.push_back({L"Reparse point", std::wstring(f.flags & FF_REPARSE_DIR ? L"Folder link, not followed" : L"Counted at its reported size") +
                                                L" (tag " + tag + L")"});
        }
        m.comp.push_back({CatName(f.cat), FmtSize(f.size), KV::Num});
        safeRows(f.safe, nullptr);
    }
    // Current view: what the Baseline sidebar shows above its tabs.
    std::wstring notice = NoticeText();
    if (!notice.empty()) m.view.push_back({L"Notice", notice});
    m.view.push_back({L"Folder", s.DirPath(view_)});
    m.view.push_back({L"Size", FmtSize(v.total), KV::Num});
    m.view.push_back({L"Contents", U64Str(v.nFiles) + L" files  \x00B7  " + U64Str(v.nDirs) + L" folders"});
    m.view.push_back({L"Safe inside", FmtSize(SafeSum(v.safeR)), KV::Good});
    if (view_ == 0) {
        if (target_.total) {
            u64 mapped = std::min<u64>(v.total, target_.used), hidden = target_.used > mapped ? target_.used - mapped : 0;
            m.view.push_back({L"Mapped", FmtSize(mapped), KV::Num});
            m.view.push_back({L"Not mapped", FmtSize(hidden), KV::Num});
            m.view.push_back({L"", NotMappedWhy(s.stats.streams)});
            m.view.push_back({L"Free", FmtSize(FreeOf(target_)), KV::Num});
        }
        if (s.stats.deniedDirs) m.view.push_back({L"Unreadable", U64Str(s.stats.deniedDirs) + L" folders couldn't be read"});
        std::wstring sk = SkippedText();
        if (!sk.empty()) m.view.push_back({L"Skipped", sk});
    }
    return m;
}

void DiskMapModule::ShowInExplorer(const std::wstring& path, bool select) {
    // Legacy: explorer.exe /select,"path" (file) or explorer.exe "path" (folder). Plain paths, no \\?\.
    std::wstring p = sys::ToShell(path);
    std::wstring args = select ? L"/select,\"" + p + L"\"" : L"\"" + p + L"\"";
    // ShellExecuteExW sets the thread's last error on failure (plain ShellExecuteW does not reliably).
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = host_ ? host_->Frame() : nullptr;
    sei.lpVerb = L"open";
    sei.lpFile = L"explorer.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        std::wstring m = L"Could not open Explorer: " +
                         (e ? sys::ErrorText(e) : std::wstring(L"unknown error (code ") + std::to_wstring((INT_PTR)sei.hInstApp) + L")");
        status_ = m;
        if (host_) host_->SetStatus(m.c_str());
    }
}

std::wstring DiskMapModule::SummaryText() const {
    if (!data_) return L"";
    const DirE& r = data_->dirs[0];
    std::wstring s = L"Mapped " + FmtSize(r.total) + L" in " + U64Str(r.nFiles) + L" files  \x00B7  " +
                     FmtSize(SafeSum(r.safeR)) + L" safe to delete";
    if (target_.fixture) s += L"  \x00B7  fixture";
    else { wchar_t b[48]; swprintf(b, 48, L"  \x00B7  scan took %.1fs", data_->stats.seconds); s += b; }
    if (data_->stats.deniedDirs) s += L"  \x00B7  " + U64Str(data_->stats.deniedDirs) + L" folders unreadable";
    return s;
}

std::wstring DiskMapModule::HoverText() const {
    if (!data_ || hoverHit_ < 0 || hoverHit_ >= (int)hits_.size()) return L"";
    const Scan& s = *data_;
    const Hit& h = hits_[hoverHit_];
    const wchar_t* sep = L"   \x00B7   ";
    auto inside = [&](const u64* r) { u64 t = SafeSum(r); return t ? sep + FmtSize(t) + L" safe to delete inside" : std::wstring(); };
    if (h.kind == 0) {
        const FileE& f = s.files[h.id];
        std::wstring x = s.FilePath(h.id) + sep + FmtSize(f.size) + sep + CatName(f.cat);
        if (f.safe) x += sep + std::wstring(L"Safe to delete: ") + SafeReason(f.safe).name;
        return x;
    }
    if (h.kind == 1) {
        const DirE& d = s.dirs[h.id];
        std::wstring x = s.DirPath(h.id) + sep + FmtSize(d.total) + sep + U64Str(d.nFiles) + L" files" + sep + L"mostly " + CatName(d.dom);
        x += d.safe ? sep + std::wstring(L"Safe to delete: ") + SafeReason(d.safe).name : inside(d.safeR);
        return x;
    }
    const Lump& L = lumps_[h.id];
    return U64Str(L.count) + L" small files grouped together" + sep + FmtSize(L.size) + sep + L"mostly " + CatName(L.dom) + inside(L.safeR);
}

void DiskMapModule::UpdateStatus() {
    if (!host_) return;
    std::wstring s;
    if (st_ == St::Done) {
        if (Console() || (Three() && hoverHit_ < 0)) {
            int sel = list_.Selected();
            if (data_ && sel >= 0 && sel < (int)rows_.size())
                s = rows_[sel].kind ? data_->DirPath(rows_[sel].id) : data_->FilePath(rows_[sel].id);
            s = s.empty() ? SummaryText() : s + L"   \x00B7   " + SummaryText();
        } else {
            s = HoverText();
            if (s.empty() && focus_ == Focus::Largest && data_) {
                int i = largestList_.Selected();
                if (i >= 0 && i < (int)largest_.size())
                    s = largest_[i].kind ? data_->DirPath(largest_[i].id) : data_->FilePath(largest_[i].id);
            }
            if (s.empty()) s = SummaryText();
        }
    } else if (!lastError_.empty()) {
        s = lastError_;
    }
    // Empty -> the host shows its own elevation line, as the legacy status bar did while idle.
    if (s != status_) { status_ = s; host_->SetStatus(s.c_str()); }
}

// ============================================================================================
// Painting — Baseline (legacy 866-1358, metrics through S(), colours from the theme)

void DiskMapModule::LayoutRects(const RECT& rc) {
    rcSearch_ = rcSplit_ = rcFilter_ = rcPath_ = rcLargest_ = RECT{0, 0, 0, 0};
    if (Console()) {   // board 4: filter line, path line, full-width table, bottom detail (~30%)
        rcTool_ = rcSide_ = rcCrumb_ = RECT{0, 0, 0, 0};
        rcFilter_ = {rc.left, rc.top, rc.right, rc.top + Sc(26)};
        rcPath_ = {rc.left, rcFilter_.bottom, rc.right, rcFilter_.bottom + Sc(22)};
        int dh = std::max<int>(Sc(150), (int)(rc.bottom - rcPath_.bottom) * 30 / 100);
        rcBody_ = {rc.left, rcPath_.bottom, rc.right, rc.bottom};
        rcList_ = {rc.left, rcPath_.bottom, rc.right, rc.bottom - dh};
        rcDetail_ = {rc.left, rcList_.bottom, rc.right, rc.bottom};
        rcMap_ = rc;   // the empty state / progress card when there is no scan
        return;
    }
    if (Three()) {     // board 7: toolbar, breadcrumb, treemap over the list | splitter | detail
        rcSide_ = {0, 0, 0, 0};
        rcTool_ = {rc.left, rc.top, rc.right, rc.top + Sc(36)};
        rcCrumb_ = {rc.left, rcTool_.bottom, rc.right, rcTool_.bottom + Sc(28)};
        int cy = (rcTool_.top + rcTool_.bottom) / 2;
        rcSearch_ = {rc.right - Sc(10) - Sc(220), cy - Sc(12), rc.right - Sc(10), cy + Sc(12)};
        rcBody_ = {rc.left, rcCrumb_.bottom, rc.right, rc.bottom};
        if (st_ != St::Done || !data_) {
            rcSearch_ = rcList_ = rcDetail_ = RECT{0, 0, 0, 0};
            rcMap_ = {rc.left, rcTool_.bottom, rc.right, rc.bottom};
            return;
        }
        int W = rc.right - rc.left;
        int dw = std::max<int>(Sc(240), std::min<int>(Sc(detailW96_), W - Sc(380)));
        rcDetail_ = {rc.right - dw, rcBody_.top, rc.right, rc.bottom};
        rcSplit_ = {rcDetail_.left - Sc(5), rcBody_.top, rcDetail_.left, rc.bottom};
        int mh = rc.bottom - rcBody_.top;
        rcMap_ = {rc.left, rcBody_.top, rcSplit_.left, rcBody_.top + mh * 55 / 100};
        rcList_ = {rc.left, rcMap_.bottom + Sc(1), rcSplit_.left, rc.bottom};
        return;
    }
    int tb = Sc(60), sb = Sc(300), bc = Sc(40);
    rcTool_ = {rc.left, rc.top, rc.right, rc.top + tb};
    rcSide_ = {rc.left, rc.top + tb, rc.left + sb, rc.bottom};   // host owns the status bar below us
    rcCrumb_ = {rc.left + sb, rc.top + tb, rc.right, rc.top + tb + bc};
    rcMap_ = {rc.left + sb + Sc(10), rc.top + tb + bc, rc.right - Sc(12), rc.bottom - Sc(10)};
}

void DiskMapModule::Button(HDC dc, RECT r, const wchar_t* label, int id, bool primary, bool enabled) {
    const Theme& t = host_->Theme();
    bool hv = enabled && IsHover(id);
    COLORREF bg = primary ? (hv ? Lighten(t.accent, 0.12) : t.accent) : (hv ? t.panel3 : t.panel2);
    // One pass for fill and outline, so their anti-aliased edges don't blend twice.
    RoundFill(dc, r, Sc(8), bg, primary ? kNoColor : (hv ? Mix(t.border, t.text3, 0.5) : t.border));
    COLORREF tc = primary ? (Lum(t.accent) > 140 ? RGB(10, 14, 22) : RGB(245, 247, 250)) : (enabled ? t.text1 : t.text3);
    Text(dc, label, r, host_->Fonts().semi, tc, TC);
    if (enabled) AddClick(r, id);
}

void DiskMapModule::DrawLogo(HDC dc, int x, int y, int s) {
    int h = s / 2, g = std::max(1, Sc(2));
    RECT a = {x, y, x + h + Sc(3), y + s};                 FillC(dc, a, CatColor(C_GAMES));
    RECT b = {a.right + g, y, x + s, y + h};               FillC(dc, b, CatColor(C_PROGRAMS));
    RECT c = {a.right + g, y + h + g, x + s - Sc(5), y + s}; FillC(dc, c, CatColor(C_IMAGES));
    RECT d = {c.right + g, y + h + g, x + s, y + s};       FillC(dc, d, CatColor(C_VIDEO));
}

void DiskMapModule::DrawToolbar(HDC dc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    bool con = Console();
    FillC(dc, rcTool_, t.panel);
    RECT line = {rcTool_.left, rcTool_.bottom - 1, rcTool_.right, rcTool_.bottom}; FillC(dc, line, t.border);
    int cy = (rcTool_.top + rcTool_.bottom) / 2;
    int logo = con ? Sc(16) : Sc(22);
    DrawLogo(dc, rcTool_.left + Sc(18), cy - logo / 2, logo);
    RECT tt = {rcTool_.left + Sc(con ? 44 : 50), rcTool_.top, rcTool_.left + Sc(170), rcTool_.bottom};
    Text(dc, L"DiskMap", tt, f.title, t.text1, TL);

    int x = rcTool_.left + Sc(160);
    int bh = con ? Sc(34) : Sc(46), bw = con ? Sc(150) : Sc(160);
    if (target_.fixture) {
        // --fixture: one deterministic "drive" button for the scanned folder.
        RECT r = {x, cy - bh / 2, x + Sc(220), cy + bh / 2};
        RoundFill(dc, r, Sc(8), Mix(t.panel2, t.accent, 0.18), t.accent);
        RECT tr = {r.left + Sc(10), r.top, r.right - Sc(8), r.bottom};
        size_t cut = target_.display.find_last_of(L'\\');
        std::wstring leaf = cut == std::wstring::npos ? target_.display : target_.display.substr(cut + 1);
        Text(dc, L"Fixture: " + leaf, tr, f.semi, t.text1, TL);
    } else {
        for (size_t i = 0; i < drives_.size(); i++) {
            const Drive& d = drives_[i];
            RECT r = {x, cy - bh / 2, x + bw, cy + bh / 2};
            if (r.right > rcTool_.right - Sc(500)) break;   // legacy: no overflow menu; room for the toggle
            bool active = (st_ != St::Idle && d.letter == target_.letter);
            bool hv = IsHover(ID_DRIVE, (u32)i);
            RoundFill(dc, r, Sc(8), active ? Mix(t.panel2, t.accent, 0.18) : (hv ? t.panel3 : t.panel2),
                      active ? t.accent : (hv ? Mix(t.border, t.text3, 0.5) : t.border));
            double used = d.total ? (double)UsedOf(d) / (double)d.total : 0;
            if (con) {
                RECT tr = {r.left + Sc(8), r.top, r.right - Sc(8), r.bottom};
                Text(dc, std::wstring(1, d.letter) + L":  " + FmtSize(d.freeb) + L" free", tr, f.caption, t.text1, TL);
                RECT bar = {r.left + Sc(8), r.bottom - Sc(6), r.right - Sc(8), r.bottom - Sc(4)};
                FillC(dc, bar, t.panel3);
                RECT ub = bar; ub.right = bar.left + (LONG)((bar.right - bar.left) * used);
                FillC(dc, ub, used > 0.9 ? t.bad : t.accent);
            } else {
                RECT tr = {r.left + Sc(10), r.top + Sc(4), r.right - Sc(8), r.top + Sc(21)};
                Text(dc, std::wstring(1, d.letter) + L":  " + d.label, tr, f.semi, t.text1, TL);
                RECT bar = {r.left + Sc(10), r.bottom - Sc(9), r.right - Sc(10), r.bottom - Sc(6)};
                FillC(dc, bar, t.panel3);
                RECT ub = bar; ub.right = bar.left + (LONG)((bar.right - bar.left) * used);
                FillC(dc, ub, used > 0.9 ? t.bad : t.accent);
                RECT fr2 = {r.left + Sc(10), r.top + Sc(21), r.right - Sc(8), r.top + Sc(36)};
                Text(dc, FmtSize(d.freeb) + L" free", fr2, f.caption, t.text2, TL);
            }
            AddClick(r, ID_DRIVE, (u32)i);
            x = r.right + Sc(8);
        }
    }
    // No "Run as admin" button (approved deviation 1: every exe is requireAdministrator).
    int rx = rcTool_.right - Sc(16);
    RECT r = {rx - Sc(96), cy - Sc(16), rx, cy + Sc(16)};
    if (st_ == St::Scanning) Button(dc, r, L"Cancel", ID_CANCEL, false);
    else if (st_ == St::Done && !target_.fixture) Button(dc, r, L"Rescan", ID_RESCAN, true);

    // "Check for data streams" toggle (off by default). On: a one-line cost note under it.
    {
        int tr = r.left - Sc(10), tl = tr - Sc(220);
        int top = streams_ ? cy - Sc(21) : cy - Sc(13);
        RECT b = {tl, top, tr, top + Sc(26)};
        bool hv = IsHover(ID_STREAMS);
        RoundFill(dc, b, Sc(6), hv ? t.panel3 : t.panel2,
                  streams_ ? t.accent : (hv ? Mix(t.border, t.text3, 0.5) : t.border));
        int bx = b.left + Sc(9), bs = Sc(12), by = (b.top + b.bottom) / 2 - bs / 2;
        RECT box = {bx, by, bx + bs, by + bs};
        if (streams_) {
            FillC(dc, box, t.accent);
            RECT in = {box.left + Sc(3), box.top + Sc(3), box.right - Sc(3), box.bottom - Sc(3)};
            FillC(dc, in, Lum(t.accent) > 140 ? RGB(10, 14, 22) : RGB(245, 247, 250));
        } else {
            FillC(dc, box, t.border);
            RECT in = {box.left + 1, box.top + 1, box.right - 1, box.bottom - 1}; FillC(dc, in, t.panel);
        }
        RECT lt = {box.right + Sc(8), b.top, b.right - Sc(6), b.bottom};
        Text(dc, L"Check for data streams", lt, f.caption, t.text1, TL);
        AddClick(b, ID_STREAMS);
        if (streams_) {
            RECT nt = {tl, b.bottom + Sc(1), tr, b.bottom + Sc(17)};
            Text(dc, L"slower: reads each file's stream list", nt, f.caption, t.text3, TL);
        }
    }
}

void DiskMapModule::SectionLabel(HDC dc, const wchar_t* s, int& y) {
    RECT r = {rcSide_.left + Sc(20), y, rcSide_.right - Sc(20), y + Sc(18)};
    Text(dc, s, r, extra_.smallSemi, host_->Theme().text3, TL);
    y += Sc(24);
}

void DiskMapModule::DrawWrapped(HDC dc, const std::wstring& s, int L, int R, int& y, COLORREF c) {
    HFONT f = host_->Fonts().caption;
    RECT wr = {L, y + Sc(4), R, y + Sc(4)};
    HGDIOBJ of = SelectObject(dc, f);
    DrawTextW(dc, s.c_str(), -1, &wr, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, of);
    wr.right = R;
    Text(dc, s, wr, f, c, DT_WORDBREAK);
    y = wr.bottom + Sc(12);
}

// Deviation 4: report what the scan left out or counted once. Empty when there is nothing to say.
std::wstring DiskMapModule::SkippedText() const {
    if (!data_) return L"";
    const ScanStats& st = data_->stats;
    std::wstring s;
    auto add = [&](const std::wstring& part) { if (!s.empty()) s += L"  \x00B7  "; s += part; };
    auto n = [](u64 v, const wchar_t* one, const wchar_t* many) { return U64Str(v) + (v == 1 ? one : many); };
    if (st.cloudFiles) add(n(st.cloudFiles, L" cloud-only file (", L" cloud-only files (") + FmtSize(st.cloudBytes) + L", not on disk) skipped");
    if (st.cloudDirs) add(n(st.cloudDirs, L" cloud-only folder skipped", L" cloud-only folders skipped"));
    if (st.hardlinkDupFiles)
        add(n(st.hardlinkDupFiles, L" hard link counted once (", L" hard links counted once (") + FmtSize(st.hardlinkDupBytes) + L")");
    if (st.reparseDirs + st.linkFiles) add(n(st.reparseDirs + st.linkFiles, L" link not followed", L" links not followed"));
    if (st.adsFiles)
        add(n(st.adsFiles, L" file with data streams (", L" files with data streams (") + FmtSize(st.adsBytes) + L")");
    if (st.noIdDirs)
        add(n(st.noIdDirs, L" folder listed without file IDs (hard links there counted per link)",
                           L" folders listed without file IDs (hard links there counted per link)"));
    return s;
}

// The one degraded-mode notice (deviation 1/5): unelevated, or SeBackupPrivilege refused.
std::wstring DiskMapModule::NoticeText() const {
    if (!elevated_) return L"Not running elevated, so protected folders can't be read and totals will be lower. Hadron asks for admin at start; it does not relaunch.";
    if (data_ && !data_->stats.backupOk)   // known after a scan: the privilege is enabled per scan thread
        return L"SeBackupPrivilege could not be enabled for the scan (" + sys::ErrorText(data_->stats.backupErr) +
               L"), so some admin-only folders may be unreadable.";
    return L"";
}

void DiskMapModule::DrawNotice(HDC dc, int& y, int L, int R) {
    std::wstring msg = NoticeText();
    if (msg.empty()) return;
    const Theme& t = host_->Theme();
    RECT box = {L + Sc(12), y + Sc(8), R - Sc(10), y + Sc(8)};
    HGDIOBJ of = SelectObject(dc, host_->Fonts().caption);
    DrawTextW(dc, msg.c_str(), -1, &box, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, of);
    box.right = R - Sc(10);
    RECT bg = {L, y, R, box.bottom + Sc(8)};
    RoundFill(dc, bg, Sc(8), Mix(t.panel2, t.warn, 0.08));
    RECT stripe = {L, y + Sc(6), L + Sc(3), bg.bottom - Sc(6)}; FillC(dc, stripe, t.warn);
    Text(dc, msg, box, host_->Fonts().caption, t.text2, DT_WORDBREAK);
    y = bg.bottom + Sc(14);
}

void DiskMapModule::DrawSidebar(HDC dc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    FillC(dc, rcSide_, t.panel);
    RECT line = {rcSide_.right - 1, rcSide_.top, rcSide_.right, rcSide_.bottom}; FillC(dc, line, t.border);
    int L = rcSide_.left + Sc(20), R = rcSide_.right - Sc(20);
    int y = rcSide_.top + Sc(18);
    rcLargest_ = {0, 0, 0, 0};
    DrawNotice(dc, y, L, R);

    if (st_ != St::Done || !data_) {
        SectionLabel(dc, L"CATEGORIES", y);
        for (int c = 0; c < NCAT; c++) {
            RECT sw = {L, y + Sc(6), L + Sc(10), y + Sc(16)};
            RoundFill(dc, sw, Sc(4), CatColor(c));
            RECT tr = {L + Sc(20), y, R, y + Sc(22)};
            Text(dc, CatName(c), tr, f.ui, t.text2, TL);
            y += Sc(26);
        }
        return;
    }

    const Scan& s = *data_;
    const DirE& v = s.dirs[view_];
    SectionLabel(dc, L"CURRENT VIEW", y);
    RECT nr = {L, y, R, y + Sc(22)};
    Text(dc, view_ == 0 ? s.DirPath(0) : std::wstring(s.Name(v.name)), nr, f.semi, t.text1, view_ == 0 ? TP : TL);
    y += Sc(24);
    RECT br = {L, y, R, y + Sc(34)};
    Text(dc, FmtSize(v.total), br, extra_.bigNum, t.text1, TL);
    y += Sc(36);
    RECT ir = {L, y, R, y + Sc(18)};
    Text(dc, U64Str(v.nFiles) + L" files  \x00B7  " + U64Str(v.nDirs) + L" folders", ir, f.caption, t.text2, TL);
    y += Sc(26);

    // Drive breakdown (root, Categories tab, drive size known). "Not reachable" is relabelled
    // "Not mapped" and explained (deviation 6): it is used space the scan did not attribute.
    if (view_ == 0 && target_.total && sideTab_ == 0) {
        RECT bar = {L, y, R, y + Sc(8)};
        RoundFill(dc, bar, Sc(4), t.panel3);
        double W = bar.right - bar.left;
        u64 used = target_.used;
        u64 mapped = std::min<u64>(s.dirs[0].total, used);
        u64 hidden = used > mapped ? used - mapped : 0;
        RECT a = bar; a.right = bar.left + (LONG)(W * mapped / target_.total);
        RECT b = a; b.left = a.right; b.right = b.left + (LONG)(W * hidden / target_.total);
        if (a.right > a.left) FillC(dc, a, t.accent);
        if (b.right > b.left) FillC(dc, b, t.text3);
        y += Sc(14);
        struct { COLORREF c; const wchar_t* n; std::wstring v; } rows[] = {
            {t.accent, L"Mapped", FmtSize(mapped)},
            {t.text3, L"Not mapped", FmtSize(hidden)},
            {t.panel3, L"Free", FmtSize(FreeOf(target_))}};
        for (auto& rw : rows) {
            RECT sw = {L, y + Sc(5), L + Sc(8), y + Sc(13)}; FillC(dc, sw, rw.c);
            RECT t1 = {L + Sc(14), y, R - Sc(70), y + Sc(18)}; Text(dc, rw.n, t1, f.caption, t.text2, TL);
            RECT t2 = {R - Sc(70), y, R, y + Sc(18)}; Text(dc, rw.v, t2, f.caption, t.text2, TR);
            y += Sc(18);
        }
        DrawWrapped(dc, NotMappedWhy(s.stats.streams), L, R, y, t.text3);
    }
    if (view_ == 0 && s.stats.deniedDirs) {
        RECT dr = {L, y, R, y + Sc(18)};
        Text(dc, U64Str(s.stats.deniedDirs) + L" folders couldn't be read", dr, f.caption, t.warn, TL);
        y += Sc(24);
    }
    if (view_ == 0) {   // deviation 4: say what the scan skipped or counted once
        std::wstring sk = SkippedText();
        if (!sk.empty()) DrawWrapped(dc, sk, L, R, y, t.text2);
    }

    // Tabs.
    {
        RECT seg = {L, y, R, y + Sc(32)};
        RoundFill(dc, seg, Sc(9), t.panel2);
        int mid = (L + R) / 2;
        const wchar_t* lab[2] = {L"Categories", L"Safe to delete"};
        for (int i = 0; i < 2; i++) {
            RECT tr = {i == 0 ? L + Sc(3) : mid + Sc(1), y + Sc(3), i == 0 ? mid - Sc(1) : R - Sc(3), y + Sc(29)};
            bool sel = sideTab_ == i, hv = IsHover(ID_TAB, (u32)i);
            if (sel) RoundFill(dc, tr, Sc(7), t.panel3);
            Text(dc, lab[i], tr, f.semi, (sel || hv) ? t.text1 : t.text2, TC);
            if (!sel) AddClick(tr, ID_TAB, (u32)i);
        }
        y += Sc(46);
    }

    if (sideTab_ == 0) {
        RECT hr = {L, y, R, y + Sc(18)};
        Text(dc, L"CATEGORIES", hr, extra_.smallSemi, t.text3, TL);
        if (filter_ >= 0 && filter_ < NCAT) {
            RECT cr = {R - Sc(60), y - Sc(2), R, y + Sc(18)};
            Text(dc, L"Show all", cr, extra_.smallSemi, IsHover(ID_CLEARFILTER) ? t.text1 : t.accent, TR);
            AddClick(cr, ID_CLEARFILTER);
        }
        y += Sc(24);
        int order[NCAT]; for (int i = 0; i < NCAT; i++) order[i] = i;
        std::stable_sort(order, order + NCAT, [&](int a, int b) { return v.cat[a] > v.cat[b]; });
        for (int oi = 0; oi < NCAT; oi++) {
            int c = order[oi];
            if (!v.cat[c]) continue;
            RECT row = {L - Sc(8), y, R + Sc(8), y + Sc(32)};
            bool sel = filter_ == c, hv = IsHover(ID_CAT, (u32)c);
            if (sel || hv) RoundFill(dc, row, Sc(8), sel ? t.panel3 : t.panel2);
            bool dim = filter_ >= 0 && !sel;
            COLORREF col = dim ? Mix(CatColor(c), t.panel, 0.6) : CatColor(c);
            RECT sw = {L, y + Sc(8), L + Sc(10), y + Sc(18)};
            RoundFill(dc, sw, Sc(4), col);
            RECT tr = {L + Sc(18), y + Sc(2), R - Sc(70), y + Sc(24)};
            Text(dc, CatName(c), tr, sel ? f.semi : f.ui, dim ? t.text3 : t.text1, TL);
            RECT sr = {R - Sc(80), y + Sc(2), R, y + Sc(24)};
            Text(dc, FmtSize(v.cat[c]), sr, f.ui, dim ? t.text3 : t.text2, TR);
            RECT track = {L + Sc(18), y + Sc(25), R, y + Sc(27)};
            FillC(dc, track, t.border);
            RECT fill = track;
            fill.right = track.left + (LONG)((track.right - track.left) * (v.total ? (double)v.cat[c] / (double)v.total : 0));
            if (fill.right > fill.left) FillC(dc, fill, col);
            AddClick(row, ID_CAT, (u32)c);
            y += Sc(34);
        }
        y += Sc(12);
    } else {
        u64 tot = SafeSum(v.safeR);
        RECT hr = {L, y, R, y + Sc(18)};
        Text(dc, L"MATCHED BY THE RULES", hr, extra_.smallSemi, t.text3, TL);   // rule 7 over the legacy header
        if (filter_ > FILTER_SAFE) {
            RECT cr = {R - Sc(60), y - Sc(2), R, y + Sc(18)};
            Text(dc, L"Show all", cr, extra_.smallSemi, IsHover(ID_CLEARFILTER) ? t.text1 : t.accent, TR);
            AddClick(cr, ID_CLEARFILTER);
        }
        y += Sc(20);
        RECT nr2 = {L, y, R, y + Sc(34)};
        Text(dc, tot ? FmtSize(tot) : L"0 B", nr2, extra_.bigNum, tot ? SAFE_COL : t.text2, TL);
        y += Sc(34);
        DrawWrapped(dc, L"Scanned size of the matched files. Space actually freed can be less (files in use, compressed or hard-linked files).",
                    L, R, y, t.text3);
        int order[NSAFE]; int no = 0;
        for (int r = 1; r < NSAFE; r++) if (v.safeR[r]) order[no++] = r;
        for (int i = 1; i < no; i++) for (int j = i; j > 0 && v.safeR[order[j]] > v.safeR[order[j - 1]]; j--) std::swap(order[j], order[j - 1]);
        int hoverReason = -1;
        for (int oi = 0; oi < no; oi++) {
            int r = order[oi];
            RECT row = {L - Sc(8), y, R + Sc(8), y + Sc(30)};
            bool sel = filter_ == FILTER_SAFE + r, hv = IsHover(ID_SAFE, (u32)r);
            if (hv) hoverReason = r;
            if (sel || hv) RoundFill(dc, row, Sc(8), sel ? t.panel3 : t.panel2);
            bool dim = filter_ > FILTER_SAFE && !sel;
            COLORREF col = dim ? Mix(SAFE_COL, t.panel, 0.6) : SAFE_COL;
            RECT sw = {L, y + Sc(7), L + Sc(10), y + Sc(17)};
            RoundFill(dc, sw, Sc(4), col);
            RECT tr = {L + Sc(18), y + Sc(1), R - Sc(70), y + Sc(23)};
            Text(dc, SafeReason(r).name, tr, sel ? f.semi : f.ui, dim ? t.text3 : t.text1, TL);
            RECT sr = {R - Sc(80), y + Sc(1), R, y + Sc(23)};
            Text(dc, FmtSize(v.safeR[r]), sr, f.ui, dim ? t.text3 : t.text2, TR);
            RECT track = {L + Sc(18), y + Sc(23), R, y + Sc(25)};
            FillC(dc, track, t.border);
            RECT fill = track;
            fill.right = track.left + (LONG)((track.right - track.left) * (tot ? (double)v.safeR[r] / (double)tot : 0));
            if (fill.right > fill.left) FillC(dc, fill, col);
            AddClick(row, ID_SAFE, (u32)r);
            y += Sc(32);
        }
        // Explanation box: hovered reason, else selected, else the general line. Each reason now
        // carries its basis (rule 7), shown under the description.
        int dr = hoverReason > 0 ? hoverReason : (filter_ > FILTER_SAFE ? filter_ - FILTER_SAFE : 0);
        std::wstring desc;
        if (dr > 0) desc = std::wstring(SafeReason(dr).desc) + L"\n\nBasis: " + SafeReason(dr).basis + L" \x2014 " + SafeReason(dr).basisNote;
        else if (tot) desc = L"Green = things Windows or apps rebuild, keep only for troubleshooting or rollback, or that were already deleted. Rules match by folder and file name, so check before deleting. Hover a group for details. DiskMap never deletes anything itself.";
        else desc = L"Nothing in this folder matched the safe-to-delete rules.";
        RECT box = {L + Sc(12), y + Sc(8), R - Sc(10), y + Sc(8)};
        HGDIOBJ of = SelectObject(dc, f.caption);
        DrawTextW(dc, desc.c_str(), -1, &box, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, of);
        box.right = R - Sc(10);
        RECT bg = {L, y, R, box.bottom + Sc(8)};
        RoundFill(dc, bg, Sc(8), Mix(t.panel2, SAFE_COL, 0.06));
        RECT stripe = {L, y + Sc(6), L + Sc(3), bg.bottom - Sc(6)};
        FillC(dc, stripe, SAFE_COL);
        Text(dc, desc, box, f.caption, t.text2, DT_WORDBREAK);
        y = bg.bottom + Sc(18);
    }

    // Largest files / biggest spots: a core/ui ListView (scroll, sort, keyboard) — legacy had none.
    if (!largest_.empty() && y < rcSide_.bottom - Sc(110)) {
        SectionLabel(dc, sideTab_ == 1 ? L"BIGGEST SPOTS TO CLEAN" : L"LARGEST FILES", y);
        rcLargest_ = {L - Sc(8), y, R + Sc(8), rcSide_.bottom - Sc(10)};
        int w96 = MulDiv(rcLargest_.right - rcLargest_.left, 96, (int)host_->Dpi());
        largestList_.SetColumns({{L"Name", std::max(60, w96 - 84)}, {L"Size", 84, true}});
        largestList_.Paint(dc, rcLargest_, Ctx(), focus_ == Focus::Largest);
    }
}

void DiskMapModule::DrawCrumb(HDC dc, const RECT& strip, int x) {
    if (st_ != St::Done || !data_) return;
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    const Scan& s = *data_;
    int cy = (strip.top + strip.bottom) / 2;
    RECT up = {x, cy - Sc(14), x + Sc(64), cy + Sc(14)};
    Button(dc, up, L"\x2190  Up", ID_UP, false, view_ != 0);
    x = up.right + Sc(14);

    // Right-aligned key hint (legacy status-bar hint; the host owns the status bar now).
    const wchar_t* hint = Console() ? L"Enter: open   Backspace: up   Ctrl+Enter: show in Explorer   F5: rescan"
                                    : L"Click: open folder   Right-click / Backspace: up   Ctrl+click: show in Explorer";
    int hw = TextW(dc, hint, f.caption);
    int right = strip.right - Sc(12);
    std::vector<u32> chain;
    for (u32 d = view_;; d = s.dirs[d].parent) { chain.push_back(d); if (d == 0) break; }
    std::reverse(chain.begin(), chain.end());
    auto crumbW = [&](size_t first) {
        int sep = TextW(dc, L"  \x203A  ", f.ui);
        int tw = TextW(dc, s.Name(s.dirs[chain[0]].name), f.ui) + Sc(12);
        if (first > 1) tw += TextW(dc, L"\x2026", f.ui) + sep;
        for (size_t i = first; i < chain.size(); i++) tw += sep + TextW(dc, s.Name(s.dirs[chain[i]].name), f.semi) + Sc(12);
        return tw;
    };
    if (x + crumbW(1) + hw + Sc(40) < right) {
        RECT hr = {right - hw, strip.top, right, strip.bottom};
        Text(dc, hint, hr, f.caption, t.text3, TR);
        right = hr.left - Sc(20);
    }
    size_t first = 1;
    while (!(x + crumbW(first) < right - Sc(8) || first + 1 >= chain.size())) first++;
    for (size_t i = 0; i < chain.size(); i++) {
        if (i > 0 && i < first) continue;
        if (i > 0) {
            const wchar_t* sp = (i == first && first > 1) ? L"  \x203A  \x2026  \x203A  " : L"  \x203A  ";
            int w = TextW(dc, sp, f.ui);
            RECT r = {x, strip.top, x + w, strip.bottom};
            Text(dc, sp, r, f.ui, t.text3, TL);
            x += w;
        }
        bool last = i + 1 == chain.size();
        std::wstring nm = s.Name(s.dirs[chain[i]].name);
        HFONT fo = last ? f.semi : f.ui;
        int w = TextW(dc, nm, fo) + Sc(12);
        RECT r = {x, cy - Sc(13), (LONG)std::min<int>(x + w, right), cy + Sc(13)};
        bool hv = !last && IsHover(ID_CRUMB, chain[i]);
        if (hv) RoundFill(dc, r, Sc(6), t.panel2);
        RECT tr = r; tr.left += Sc(6);
        Text(dc, nm, tr, fo, last ? t.text1 : (hv ? t.text1 : t.text2), TL);
        if (!last) AddClick(r, ID_CRUMB, chain[i]);
        x = r.right;
    }
}

void DiskMapModule::DrawProgress(HDC dc, const RECT& area) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    int cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
    int W = std::min<int>(Sc(560), area.right - area.left - Sc(40));
    RECT card = {cx - W / 2, cy - Sc(110), cx + W / 2, cy + Sc(110)};
    RoundFill(dc, card, Sc(14), t.panel, t.border);
    int L = card.left + Sc(28), R = card.right - Sc(28), y = card.top + Sc(24);
    RECT tt = {L, y, R, y + Sc(30)}; Text(dc, L"Mapping " + target_.display, tt, f.title, t.text1, TL);
    y += Sc(40);
    u64 bytes = prog_.bytes.load(), files = prog_.files.load();
    // An estimate (logical bytes found / drive used space), so it is labelled approximate (deviation 6).
    double frac = target_.used ? std::min(0.99, (double)bytes / (double)target_.used) : 0;
    RECT bar = {L, y, R, y + Sc(8)};
    RoundFill(dc, bar, Sc(4), t.panel3);
    RECT fb = bar; fb.right = bar.left + std::max<LONG>(Sc(8), (LONG)((bar.right - bar.left) * frac));
    RoundFill(dc, fb, Sc(4), t.accent);
    y += Sc(22);
    wchar_t pc[24] = {};
    if (target_.used) swprintf(pc, 24, L"~%d%%", (int)(frac * 100));
    RECT l1 = {L, y, R - Sc(70), y + Sc(22)}; Text(dc, U64Str(files) + L" files  \x00B7  " + FmtSize(bytes), l1, f.ui, t.text1, TL);
    RECT l2 = {R - Sc(70), y, R, y + Sc(22)}; Text(dc, pc, l2, f.semi, t.accent, TR);
    y += Sc(32);
    RECT l3 = {L, y, R, y + Sc(20)};
    Text(dc, prog_.Current(), l3, f.caption, t.text3, TP);
    y += Sc(30);
    DWORD secs = (DWORD)((GetTickCount64() - scanStart_) / 1000);
    wchar_t el[128];
    swprintf(el, 128, L"Elapsed %u:%02u  \x00B7  %% is approximate: bytes found \x00F7 drive used space", (unsigned)(secs / 60), (unsigned)(secs % 60));
    RECT l4 = {L, y, R, y + Sc(18)}; Text(dc, el, l4, f.caption, t.text2, TL);
}

void DiskMapModule::DrawEmpty(HDC dc, const RECT& area) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    int cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
    DrawLogo(dc, cx - Sc(24), cy - Sc(110), Sc(48));
    RECT t1 = {area.left, cy - Sc(46), area.right, cy - Sc(6)};
    Text(dc, L"Pick a drive to map", t1, extra_.huge, t.text1, TC);
    RECT t2 = {area.left, cy, area.right, cy + Sc(24)};
    Text(dc, L"Every file becomes a block sized by how much space it uses, colored by what it is.", t2, f.ui, t.text2, TC);
    if (!lastError_.empty()) {
        RECT t3 = {area.left, cy + Sc(28), area.right, cy + Sc(50)};
        Text(dc, lastError_, t3, f.caption, t.bad, TC);
    }
    size_t n = std::min<size_t>(drives_.size(), 8);
    int x = cx - (int)n * Sc(84) / 2 + Sc(4);   // centred on the tiles shown (legacy used the full count)
    for (size_t i = 0; i < n; i++) {
        RECT r = {x, cy + Sc(70), x + Sc(76), cy + Sc(120)};
        bool hv = IsHover(ID_DRIVE, (u32)i);
        RoundFill(dc, r, Sc(10), hv ? t.panel3 : t.panel2, hv ? t.accent : t.border);
        Text(dc, std::wstring(1, drives_[i].letter) + L":", r, f.title, t.text1, TC);
        AddClick(r, ID_DRIVE, (u32)i);
        x += Sc(84);
    }
}

void DiskMapModule::DrawMapArea(HDC dc) {
    if (st_ == St::Scanning) { DrawProgress(dc, rcMap_); return; }
    if (st_ == St::Idle || !data_) { DrawEmpty(dc, rcMap_); return; }
    int w = rcMap_.right - rcMap_.left, h = rcMap_.bottom - rcMap_.top;
    if (w <= 0 || h <= 0) return;
    if (mapDirty_ || !mapDC_ || mapW_ != w || mapH_ != h) RenderMap(dc);
    BitBlt(dc, rcMap_.left, rcMap_.top, mapW_, mapH_, mapDC_, 0, 0, SRCCOPY);
    if (hoverHit_ >= 0 && hoverHit_ < (int)hits_.size()) {
        if (hoverTopValid_) {
            RECT tr = hoverTopRect_; OffsetRect(&tr, rcMap_.left, rcMap_.top);
            InflateRect(&tr, Sc(1), Sc(1));
            Outline(dc, tr, host_->Theme().accent, Sc(2));
        }
        RECT r = hits_[hoverHit_].r; OffsetRect(&r, rcMap_.left, rcMap_.top);
        Outline(dc, r, host_->Theme().id == ThemeId::Light ? host_->Theme().text1 : RGB(255, 255, 255), Sc(1));
    }
    // Three-Pane: the list selection's block, so the list and the map read as one view.
    int sel = list_.Selected();
    if (Three() && sel >= 0 && sel < (int)rows_.size()) {
        const RowRef& rr = rows_[sel];
        for (const Hit& h : hits_)
            if (h.kind == rr.kind && h.id == rr.id && h.top == rr.id && h.topKind == rr.kind) {
                RECT r = h.r; OffsetRect(&r, rcMap_.left, rcMap_.top);
                Outline(dc, r, Acc(), Sc(2));
                break;
            }
    }
}

COLORREF DiskMapModule::MixColor(const u64* cats, const u64* safeR, u64 total, u8 dom) const {
    if (filter_ < 0) return CatColor(dom);
    u64 m; COLORREF hc;
    if (filter_ < NCAT) { m = cats[filter_]; hc = CatColor(filter_); }
    else { m = filter_ == FILTER_SAFE ? SafeSum(safeR) : safeR[filter_ - FILTER_SAFE]; hc = SAFE_COL; }
    double fr = total ? (double)m / (double)total : 0;
    return Mix(Mix(CatColor(dom), host_->Theme().bg, 0.84), hc, std::sqrt(fr));
}

void DiskMapModule::Leaf(HDC dc, RECT r, COLORREF c) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (w >= Sc(3) && h >= Sc(3)) { r.right--; r.bottom--; w--; h--; }
    if ((double)w * h >= 40.0 * AreaScale()) GradV(dc, r, Lighten(c, 0.14), Darken(c, 0.16));
    else FillC(dc, r, c);
}

void DiskMapModule::LeafLabel(HDC dc, RECT r, const wchar_t* name, u64 size, bool dim) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < Sc(70) || h < Sc(32) || dim) return;
    RECT tr = {r.left + Sc(6), r.top + Sc(4), r.right - Sc(6), r.top + Sc(4) + Sc(15)};
    Text(dc, name, tr, extra_.smallSemi, RGB(16, 18, 22), TL);
    OffsetRect(&tr, 0, Sc(14));
    if (tr.bottom <= r.bottom - Sc(2)) Text(dc, FmtSize(size), tr, host_->Fonts().caption, RGB(36, 40, 48), TL);
}

// Legacy DrawDir (756-841); the unscaled pixel constants (6, 3, 30 px², 1-px insets) now go through S().
void DiskMapModule::DrawDir(HDC dc, u32 di, double x, double y, double w, double h, int depth, u32 top, u8 topKind) {
    const Theme& t = host_->Theme();
    const Scan& s = *data_;
    RECT r = ToRect(x, y, w, h);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw <= 0 || rh <= 0) return;
    const DirE& d = s.dirs[di];
    if (depth > 0) hits_.push_back({r, di, top, 1, topKind});

    if (depth > 0 && (rw < Sc(6) || rh < Sc(6))) { Leaf(dc, r, MixColor(d.cat, d.safeR, d.total, d.dom)); return; }

    RECT in = r;
    if (depth > 0) {
        RECT fr = r; fr.right--; fr.bottom--;
        COLORREF frame = (filter_ >= FILTER_SAFE && SafeMatch(d.safe)) ? Mix(t.panel2, SAFE_COL, 0.28)
                       : Mix(t.panel2, CatColor(d.dom), filter_ >= 0 ? 0.06 : 0.16);
        FillC(dc, fr, frame);
        int hdr = Sc(17);
        if (rw > Sc(64) && rh > Sc(40)) {
            std::wstring sz = FmtSize(d.total);
            RECT tx = {r.left + Sc(5), r.top, r.right - Sc(5), r.top + hdr};
            if (rw > Sc(140)) {
                Text(dc, sz, tx, host_->Fonts().caption, t.text3, TR);
                tx.right -= TextW(dc, sz, host_->Fonts().caption) + Sc(6);
            }
            Text(dc, s.Name(d.name), tx, extra_.smallSemi, t.text2, TL);
            in.top += hdr; in.left += Sc(2); in.right -= Sc(3); in.bottom -= Sc(3);
        } else {
            in.left += Sc(1); in.top += Sc(1); in.right -= Sc(2); in.bottom -= Sc(2);
        }
    }
    int iw = in.right - in.left, ih = in.bottom - in.top;
    if (iw < Sc(3) || ih < Sc(3) || d.total == 0) {
        if (d.total) Leaf(dc, in, MixColor(d.cat, d.safeR, d.total, d.dom));
        return;
    }

    // Items that would get under ~30 px² (scaled) fold into one "small items" lump per folder.
    double thr = (double)d.total / ((double)iw * ih) * 30.0 * AreaScale();
    std::vector<Item> items;
    Lump lump; bool haveLump = false;
    for (u32 k = 0; k < d.kidCount; k++) {
        u32 ci = s.KidsOf(d)[k];
        const DirE& c = s.dirs[ci];
        if (!c.total) continue;
        if ((double)c.total >= thr) items.push_back({(double)c.total, ci, 1});
        else {
            haveLump = true; lump.count += c.nFiles; lump.size += c.total;
            for (int q = 0; q < NCAT; q++) lump.cat[q] += c.cat[q];
            for (int q = 1; q < NSAFE; q++) lump.safeR[q] += c.safeR[q];
        }
    }
    for (u32 fi = d.fileFirst; fi < d.fileFirst + d.fileCount; fi++) {
        const FileE& fe = s.files[fi];
        if (!fe.size) continue;
        if ((double)fe.size >= thr) items.push_back({(double)fe.size, fi, 0});
        else { haveLump = true; lump.count++; lump.size += fe.size; lump.cat[fe.cat] += fe.size; lump.safeR[fe.safe] += fe.size; }
    }
    if (haveLump && lump.size) {
        int best = C_OTHER; u64 bv = 0;
        for (int q = 0; q < NCAT; q++) if (lump.cat[q] > bv) { bv = lump.cat[q]; best = q; }
        lump.dom = (u8)best;
        lumps_.push_back(lump);
        items.push_back({(double)lump.size, (u32)(lumps_.size() - 1), 2});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.size > b.size; });

    std::vector<RF> rects;
    Squarify(items, in.left, in.top, iw, ih, rects);
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i]; const RF& rf = rects[i];
        u32 tp = depth == 0 ? it.id : top;
        u8 tk = depth == 0 ? it.kind : topKind;
        if (it.kind == 1) {
            DrawDir(dc, it.id, rf.x, rf.y, rf.w, rf.h, depth + 1, tp, tk);
        } else if (it.kind == 0) {
            RECT lr = ToRect(rf.x, rf.y, rf.w, rf.h);
            const FileE& fe = s.files[it.id];
            Leaf(dc, lr, FileColor(fe));
            LeafLabel(dc, lr, s.Name(fe.name), fe.size, !FileMatch(fe));
            hits_.push_back({lr, it.id, tp, 0, tk});
        } else {
            RECT lr = ToRect(rf.x, rf.y, rf.w, rf.h);
            const Lump& Lm = lumps_[it.id];
            Leaf(dc, lr, Mix(MixColor(Lm.cat, Lm.safeR, Lm.size, Lm.dom), t.panel2, 0.35));
            // At the top level a lump has no folder to drill into (topKind 255).
            hits_.push_back({lr, it.id, depth == 0 ? di : top, 2, depth == 0 ? (u8)255 : topKind});
        }
    }
}

void DiskMapModule::RenderMap(HDC ref) {
    int w = rcMap_.right - rcMap_.left, h = rcMap_.bottom - rcMap_.top;
    if (w <= 0 || h <= 0) return;
    if (!mapDC_ || w != mapW_ || h != mapH_) {
        FreeMap();
        // Own 32-bpp DIB: independent of whether ref is a window DC or the --screenshot DIB.
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        mapDC_ = CreateCompatibleDC(ref);
        mapBmp_ = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!mapDC_ || !mapBmp_) {
            if (host_) { std::wstring m = L"diskmap: map bitmap failed: " + sys::ErrorText(GetLastError()); host_->Log(LogLevel::Error, m.c_str()); }
            if (mapDC_) DeleteDC(mapDC_);
            if (mapBmp_) DeleteObject(mapBmp_);
            mapDC_ = nullptr; mapBmp_ = nullptr;
            return;
        }
        mapOld_ = (HBITMAP)SelectObject(mapDC_, mapBmp_);
        SetBkMode(mapDC_, TRANSPARENT);
        mapW_ = w; mapH_ = h;
    }
    RECT all = {0, 0, w, h};
    FillC(mapDC_, all, host_->Theme().bg);
    hits_.clear(); lumps_.clear();
    hoverHit_ = -1; hoverTopValid_ = false;
    if (st_ == St::Done && data_ && !data_->dirs.empty())
        DrawDir(mapDC_, view_, 0, 0, w, h, 0, 0, 0);
    // mapDC_ outlives the fonts LeafLabel selected into it (the host rebuilds them on a DPI, layout or
    // theme change, and DeleteObject fails on a font still selected into a DC), so hand back a stock font.
    SelectObject(mapDC_, GetStockObject(DEFAULT_GUI_FONT));
    mapDirty_ = false;
}

// ---- shared Three-Pane / Console pieces ---------------------------------------------------------
// Square button (board 7: 2px radius at most; we draw none).
void DiskMapModule::SqButton(HDC dc, RECT r, const std::wstring& label, int id, u32 data, bool on, bool enabled) {
    const Theme& t = host_->Theme();
    bool hv = enabled && IsHover(id, data);
    COLORREF fill = on ? Acc() : (hv ? t.panel3 : t.panel2);
    FillC(dc, r, fill);
    if (!on) FrameRectC(dc, r, hv ? Mix(t.border, t.text3, 0.5) : t.border);
    Text(dc, label, r, host_->Fonts().ui, on ? OnAcc() : (enabled ? t.text1 : t.text3), TC);
    if (enabled) AddClick(r, id, data);
}

// Small toggle chip for the Console filter line.
void DiskMapModule::Chip(HDC dc, int& x, int cy, const std::wstring& label, bool on, int id, u32 data) {
    const Theme& t = host_->Theme();
    HFONT f = host_->Fonts().caption;
    RECT r = {x, cy - Sc(9), x + TextW(dc, label, f) + Sc(12), cy + Sc(9)};
    bool hv = IsHover(id, data);
    if (on) FillC(dc, r, Acc());
    else { FillC(dc, r, hv ? t.panel3 : t.panel2); FrameRectC(dc, r, t.border); }
    Text(dc, label, r, f, on ? OnAcc() : (hv ? t.text1 : t.text2), TC);
    AddClick(r, id, data);
    x = r.right + Sc(4);
}

// Clickable path segments from the scan root to the view; early segments collapse to an ellipsis when
// the strip is too narrow. Returns the x after the last segment.
int DiskMapModule::DrawPathCrumb(HDC dc, const RECT& strip, int x, int right, HFONT f, const wchar_t* sep) {
    const Theme& t = host_->Theme();
    const Scan& s = *data_;
    std::vector<u32> chain;
    for (u32 d = view_;; d = s.dirs[d].parent) { chain.push_back(d); if (d == 0) break; }
    std::reverse(chain.begin(), chain.end());
    auto label = [&](size_t i) { return i == 0 ? s.DirPath(0) : std::wstring(s.Name(s.dirs[chain[i]].name)); };
    int sw = TextW(dc, sep, f), ell = TextW(dc, L"\x2026", f);
    auto width = [&](size_t first) {
        int w = TextW(dc, label(0), f);
        if (first > 1) w += sw + ell;
        for (size_t i = first; i < chain.size(); i++) w += sw + TextW(dc, label(i), f);
        return w;
    };
    size_t first = 1;
    while (first + 1 < chain.size() && x + width(first) > right) first++;
    int cy = (strip.top + strip.bottom) / 2;
    for (size_t i = 0; i < chain.size(); i++) {
        if (i > 0 && i < first) continue;
        if (i > 0) {
            if (i == first && first > 1) {
                RECT er = {x, strip.top, x + sw + ell, strip.bottom};
                Text(dc, std::wstring(sep) + L"\x2026", er, f, t.text3, TL);
                x = er.right;
            }
            RECT sr = {x, strip.top, x + sw, strip.bottom};
            Text(dc, sep, sr, f, t.text3, TL);
            x = sr.right;
        }
        bool last = i + 1 == chain.size();
        std::wstring nm = label(i);
        RECT r = {x, cy - Sc(10), std::min<int>(x + TextW(dc, nm, f), right), cy + Sc(10)};
        bool hv = !last && IsHover(ID_CRUMB, chain[i]);
        Text(dc, nm, r, f, last || hv ? t.text1 : t.text2, i == 0 ? TP : TL);
        if (hv) HLine(dc, r.left, r.right, r.bottom - 1, t.text2);
        if (!last) AddClick(r, ID_CRUMB, chain[i]);
        x = r.right;
    }
    return x;
}

int TextH(HDC dc, const std::wstring& s, int width, HFONT f) {
    RECT r = {0, 0, std::max(1, width), 0};
    HGDIOBJ o = SelectObject(dc, f);
    DrawTextW(dc, s.c_str(), -1, &r, DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, o);
    return r.bottom - r.top;
}

// Wrapping key/value rows with small-caps section headers. Returns the content height.
int DiskMapModule::PaintKV(HDC dc, const std::vector<KV>& rows, const RECT& area, int scroll, bool draw) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    bool con = Console();
    int W = area.right - area.left;
    int keyW = std::min<int>(Sc(con ? 120 : 112), W * 2 / 5);
    int rowH = Sc(con ? 19 : 22), y = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const KV& kv = rows[i];
        if (kv.style == KV::Head) {
            if (i) y += Sc(10);
            if (draw) {
                RECT hr = {area.left, area.top + y - scroll, area.right, area.top + y - scroll + Sc(18)};
                Text(dc, kv.k, hr, extra_.smallSemi, t.text3, TL);
                HLine(dc, area.left + TextW(dc, kv.k, extra_.smallSemi) + Sc(8), area.right, (hr.top + hr.bottom) / 2, t.border);
            }
            y += Sc(20);
            continue;
        }
        int vx = kv.k.empty() ? 0 : keyW;
        int vh = TextH(dc, kv.v, W - vx, f.ui);
        int h = std::max(rowH, vh + Sc(4));
        if (draw) {
            int top = area.top + y - scroll;
            if (top + h >= area.top && top < area.bottom) {
                COLORREF ink = kv.k.empty() ? t.text2
                             : kv.style == KV::Good ? (t.highContrast ? t.text1 : t.good)
                             : (kv.style == KV::Num && con) ? t.warn : t.text1;
                if (!kv.k.empty()) { RECT kr = {area.left, top, area.left + keyW - Sc(8), top + rowH}; Text(dc, kv.k, kr, f.caption, t.text3, TL); }
                RECT vr = {area.left + vx, top + (rowH - std::min(rowH, vh)) / 2, area.right, top + h};
                Text(dc, kv.v, vr, f.ui, ink, DT_WORDBREAK | DT_EDITCONTROL);   // long paths break mid-word
            }
        }
        y += h;
    }
    return y;
}

void DiskMapModule::PaintPane(HDC dc, KVPane& p, const std::vector<KV>& rows) {
    RECT area = p.rc;
    int full = PaintKV(dc, rows, area, 0, false);
    bool sb = full > area.bottom - area.top;
    if (sb) area.right -= Sc(12);
    int content = sb ? PaintKV(dc, rows, area, 0, false) : full;
    p.sb.Configure(area.bottom - area.top, content);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, p.rc.left, p.rc.top, p.rc.right, p.rc.bottom);
    PaintKV(dc, rows, area, p.sb.Offset(), true);
    RestoreDC(dc, saved);
    if (p.sb.Visible()) p.sb.Paint(dc, PaneTrack(p), Ctx());
}

// ---- Three-Pane layout (board 7) ---------------------------------------------------------------
void DiskMapModule::DrawToolbarTP(HDC dc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    FillC(dc, rcTool_, t.panel);
    HLine(dc, rcTool_.left, rcTool_.right, rcTool_.bottom - 1, t.border);
    int cy = (rcTool_.top + rcTool_.bottom) / 2, bh = Sc(26), x = rcTool_.left + Sc(10);
    auto btn = [&](const std::wstring& label, int id, bool enabled) {
        RECT r = {x, cy - bh / 2, x + TextW(dc, label, f.ui) + Sc(20), cy + bh / 2};
        SqButton(dc, r, label, id, 0, false, enabled);
        x = r.right + Sc(6);
    };
    btn(L"\x2190 Up", ID_UP, st_ == St::Done && view_ != 0);
    if (st_ == St::Scanning) btn(L"Cancel", ID_CANCEL, true);
    else if (!target_.fixture) btn(L"Rescan", ID_RESCAN, st_ == St::Done);
    x += Sc(6);
    int limit = (rcSearch_.right > rcSearch_.left ? rcSearch_.left : rcTool_.right) - Sc(200);
    if (target_.fixture) {
        size_t cut = target_.display.find_last_of(L'\\');
        std::wstring leaf = cut == std::wstring::npos ? target_.display : target_.display.substr(cut + 1);
        RECT r = {x, cy - bh / 2, x + TextW(dc, L"Fixture: " + leaf, f.ui) + Sc(20), cy + bh / 2};
        FillC(dc, r, t.panel2); FrameRectC(dc, r, Acc());
        Text(dc, L"Fixture: " + leaf, r, f.ui, t.text1, TC);
        x = r.right + Sc(6);
    } else {
        for (size_t i = 0; i < drives_.size(); i++) {
            const Drive& d = drives_[i];
            std::wstring lab = std::wstring(1, d.letter) + L":  " + FmtSize(d.freeb) + L" free";
            int w = TextW(dc, lab, f.caption) + Sc(20);
            if (x + w > limit) break;
            RECT r = {x, cy - bh / 2, x + w, cy + bh / 2};
            bool active = st_ != St::Idle && d.letter == target_.letter, hv = IsHover(ID_DRIVE, (u32)i);
            FillC(dc, r, active ? Mix(t.panel2, t.accent, 0.18) : (hv ? t.panel3 : t.panel2));
            FrameRectC(dc, r, active ? Acc() : t.border);
            RECT tr = {r.left, r.top, r.right, r.bottom - Sc(3)};
            Text(dc, lab, tr, f.caption, t.text1, TC);
            double used = d.total ? (double)UsedOf(d) / (double)d.total : 0;
            RECT bar = {r.left + 1, r.bottom - Sc(3), r.right - 1, r.bottom - 1};
            FillC(dc, bar, t.panel3);
            bar.right = bar.left + (LONG)((bar.right - bar.left) * used);
            FillC(dc, bar, used > 0.9 ? t.bad : Acc());
            AddClick(r, ID_DRIVE, (u32)i);
            x = r.right + Sc(6);
        }
    }
    // "Check for data streams" (off by default): a square checkbox button.
    {
        std::wstring lab = streams_ ? L"Data streams: on (slower)" : L"Check for data streams";
        int w = TextW(dc, lab, f.caption) + Sc(36);
        RECT r = {x + Sc(4), cy - bh / 2, x + Sc(4) + w, cy + bh / 2};
        if (r.right < limit + Sc(200)) {
            bool hv = IsHover(ID_STREAMS);
            FillC(dc, r, hv ? t.panel3 : t.panel2);
            FrameRectC(dc, r, streams_ ? Acc() : t.border);
            RECT box = {r.left + Sc(9), cy - Sc(6), r.left + Sc(21), cy + Sc(6)};
            if (streams_) { FillC(dc, box, Acc()); RECT in = {box.left + Sc(3), box.top + Sc(3), box.right - Sc(3), box.bottom - Sc(3)}; FillC(dc, in, OnAcc()); }
            else FrameRectC(dc, box, t.text3);
            RECT lt = {box.right + Sc(7), r.top, r.right - Sc(4), r.bottom};
            Text(dc, lab, lt, f.caption, t.text1, TL);
            AddClick(r, ID_STREAMS);
        }
    }
    if (rcSearch_.right > rcSearch_.left) search_.Paint(dc, rcSearch_, Ctx(), focus_ == Focus::Search);
}

void DiskMapModule::DrawDetailTP(HDC dc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    RECT d = rcDetail_;
    FillC(dc, d, t.panel);
    int L = d.left + Sc(14), R = d.right - Sc(10);
    int y = d.top + Sc(10);
    RECT tr = {L, y, R, y + Sc(22)};
    Text(dc, dm_.title, tr, f.semi, t.text1, TP);
    y += Sc(22);
    RECT kr = {L, y, R, y + Sc(16)};
    Text(dc, dm_.kind, kr, f.caption, t.text3, TL);
    y += Sc(22);
    HLine(dc, L, R, y, t.border);
    y += Sc(8);
    // Largest files / biggest spots to clean at the bottom of the pane (the Baseline sidebar's list).
    int lh = largest_.empty() ? 0 : std::min<int>(Sc(26 + 24 * 7 + 30), (d.bottom - y) * 45 / 100);
    if (lh > Sc(80)) {
        RECT cap = {L, d.bottom - lh, R, d.bottom - lh + Sc(20)};
        Text(dc, sideTab_ == 1 ? L"BIGGEST SPOTS TO CLEAN" : L"LARGEST FILES", cap, extra_.smallSemi, t.text3, TL);
        rcLargest_ = {d.left + Sc(6), cap.bottom + Sc(2), d.right - Sc(6), d.bottom - Sc(6)};
        int w96 = MulDiv(rcLargest_.right - rcLargest_.left, 96, (int)host_->Dpi());
        largestList_.SetColumns({{L"Name", std::max(60, w96 - 84)}, {L"Size", 84, true}});
        largestList_.Paint(dc, rcLargest_, Ctx(), focus_ == Focus::Largest);
    } else {
        lh = 0;
        rcLargest_ = {0, 0, 0, 0};
    }
    std::vector<KV> rows = dm_.main;
    auto section = [&](const wchar_t* head, const std::vector<KV>& part) {
        if (part.empty()) return;
        rows.push_back({head, L"", KV::Head});
        rows.insert(rows.end(), part.begin(), part.end());
    };
    section(L"COMPOSITION", dm_.comp);
    section(L"SAFE TO DELETE", dm_.safe);
    section(L"CURRENT VIEW", dm_.view);
    tpPane_.rc = {L, y, d.right - Sc(4), d.bottom - lh - Sc(8)};
    PaintPane(dc, tpPane_, rows);
    if (focus_ == Focus::Detail) FocusRectC(dc, RECT{d.left + Sc(2), d.top + Sc(2), d.right - Sc(2), tpPane_.rc.bottom}, Acc());
}

void DiskMapModule::DrawThree(HDC dc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    DrawToolbarTP(dc);
    if (st_ != St::Done || !data_) {
        int y = rcMap_.top + Sc(10);
        DrawNotice(dc, y, rcMap_.left + Sc(12), rcMap_.left + Sc(420));
        DrawMapArea(dc);
        return;
    }
    // Breadcrumb with counts on the right.
    FillC(dc, rcCrumb_, t.bg);
    HLine(dc, rcCrumb_.left, rcCrumb_.right, rcCrumb_.bottom - 1, t.border);
    const DirE& v = data_->dirs[view_];
    std::wstring counts = U64Str(rows_.size()) + L" items  \x00B7  " + FmtSize(v.total) + L"  \x00B7  " + U64Str(v.nFiles) +
                          L" files  \x00B7  " + FmtSize(SafeSum(v.safeR)) + L" safe to delete";
    int cw = TextW(dc, counts, f.caption);
    RECT cr = {rcCrumb_.right - Sc(16) - cw, rcCrumb_.top, rcCrumb_.right - Sc(12), rcCrumb_.bottom};
    Text(dc, counts, cr, f.caption, t.text3, TR | DT_NOCLIP);
    DrawPathCrumb(dc, rcCrumb_, rcCrumb_.left + Sc(12), cr.left - Sc(20), f.ui, L"  \x203A  ");

    DrawMapArea(dc);
    HLine(dc, rcMap_.left, rcMap_.right, rcMap_.bottom, t.border);
    int w96 = MulDiv(rcList_.right - rcList_.left, 96, (int)host_->Dpi()) - 12;
    int sizeW = 84, pctW = 64, typeW = std::min(180, std::max(100, w96 / 4));
    list_.SetColumns({{L"Name", std::max(80, w96 - typeW - sizeW - pctW)}, {L"Type", typeW},
                      {L"Size", sizeW, true}, {L"%", pctW, true}});
    list_.Paint(dc, rcList_, Ctx(), focus_ == Focus::List);
    // Splitter: 5px, 1px lines on both edges (the host's tree splitter look).
    FillC(dc, rcSplit_, t.bg);
    VLine(dc, rcSplit_.left, rcSplit_.top, rcSplit_.bottom, t.border);
    VLine(dc, rcSplit_.right - 1, rcSplit_.top, rcSplit_.bottom, t.border);
    DrawDetailTP(dc);
}

// ---- Console layout (board 4) -----------------------------------------------------------------
// Short lower-case category label for a chip: the first word ("Archives & disk images" -> "archives").
std::wstring ShortCat(int c) {
    std::wstring n = CatName(c);
    size_t cut = n.find_first_of(L" &");
    if (cut != std::wstring::npos) n = n.substr(0, cut);
    for (auto& ch : n) ch = (wchar_t)towlower(ch);
    return n;
}

void DiskMapModule::DrawFilterLine(HDC dc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    RECT r = rcFilter_;
    FillC(dc, r, t.panel);
    HLine(dc, r.left, r.right, r.bottom - 1, t.border);
    int cy = (r.top + r.bottom) / 2, x = r.left + Sc(10);
    auto word = [&](const std::wstring& s, COLORREF c) {
        int w = TextW(dc, s, f.caption);
        RECT wr = {x, r.top, x + w, r.bottom};
        Text(dc, s, wr, f.caption, c, TL);
        x = wr.right + Sc(6);
    };
    // Row counter on the right; everything else stops short of it.
    int sel = list_.Selected();
    std::wstring rowc = L"row " + std::to_wstring(sel >= 0 ? sel + 1 : 0) + L"/" + std::to_wstring(rows_.size());
    int rw = TextW(dc, rowc, f.caption);
    RECT rr = {r.right - Sc(10) - rw, r.top, r.right - Sc(10), r.bottom};
    Text(dc, rowc, rr, f.caption, t.text2, TR | DT_NOCLIP);
    int limit = rr.left - Sc(16);

    word(L"/", Acc());
    rcSearch_ = {x, cy - Sc(10), x + Sc(200), cy + Sc(10)};
    search_.Paint(dc, rcSearch_, Ctx(), focus_ == Focus::Search);
    x = rcSearch_.right + Sc(16);
    word(L"show:", t.text3);
    Chip(dc, x, cy, L"all", filter_ < 0, ID_SHOW, 0);
    Chip(dc, x, cy, L"safe", filter_ >= FILTER_SAFE, ID_SHOW, 1);
    x += Sc(12);
    std::wstring st = streams_ ? L"streams:on" : L"streams:off";
    int stw = TextW(dc, st, f.caption) + Sc(12);
    word(L"cat:", t.text3);
    const DirE& v = data_->dirs[view_];
    int order[NCAT]; for (int i = 0; i < NCAT; i++) order[i] = i;
    std::stable_sort(order, order + NCAT, [&](int a, int b) { return v.cat[a] > v.cat[b]; });
    for (int oi = 0; oi < NCAT; oi++) {
        int c = order[oi];
        if (!v.cat[c] && filter_ != c) continue;
        if (x + TextW(dc, ShortCat(c), f.caption) + Sc(12) > limit - stw - Sc(16)) break;
        Chip(dc, x, cy, ShortCat(c), filter_ == c, ID_CAT, (u32)c);
    }
    x += Sc(12);
    if (x + stw <= limit) Chip(dc, x, cy, st, streams_, ID_STREAMS, 0);
}

void DiskMapModule::DrawConsole(HDC dc, const RECT& rc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    if (st_ != St::Done || !data_) {
        int y = rc.top + Sc(12);
        DrawNotice(dc, y, rc.left + Sc(12), rc.left + Sc(420));
        if (st_ == St::Scanning) DrawProgress(dc, rcMap_); else DrawEmpty(dc, rcMap_);
        rcList_ = rcDetail_ = rcSearch_ = RECT{0, 0, 0, 0};
        return;
    }
    const Scan& s = *data_;
    DrawFilterLine(dc);

    // Path line: clickable segments, the view's numbers, and the drives (live runs).
    RECT pr = rcPath_;
    FillC(dc, pr, t.bg);
    int cy = (pr.top + pr.bottom) / 2, rx = pr.right - Sc(10);
    if (!target_.fixture) {
        for (int i = (int)drives_.size() - 1; i >= 0; i--) {
            std::wstring lab = std::wstring(1, drives_[i].letter) + L":";
            int w = TextW(dc, lab, f.caption) + Sc(12);
            int x0 = rx - w;
            Chip(dc, x0, cy, lab, st_ != St::Idle && drives_[i].letter == target_.letter, ID_DRIVE, (u32)i);
            rx -= w + Sc(4);
        }
        RECT dl = {rx - TextW(dc, L"drive:", f.caption), pr.top, rx, pr.bottom};
        Text(dc, L"drive:", dl, f.caption, t.text3, TR | DT_NOCLIP);
        rx = dl.left - Sc(16);
    }
    const DirE& v = s.dirs[view_];
    std::wstring nums = FmtSize(v.total) + L"  files=" + U64Str(v.nFiles) + L"  folders=" + U64Str(v.nDirs) +
                        L"  safe=" + FmtSize(SafeSum(v.safeR));
    int nw = TextW(dc, nums, f.caption);
    RECT nr = {rx - nw, pr.top, rx, pr.bottom};
    Text(dc, nums, nr, f.caption, t.warn, TR | DT_NOCLIP);
    RECT pl = {pr.left + Sc(10), pr.top, pr.left + Sc(10) + TextW(dc, L"path:", f.caption), pr.bottom};
    Text(dc, L"path:", pl, f.caption, t.text3, TL);
    DrawPathCrumb(dc, pr, pl.right + Sc(8), nr.left - Sc(20), f.ui, L"\\");

    // Full-width table with the Console-only columns.
    int w96 = MulDiv(rcList_.right - rcList_.left, 96, (int)host_->Dpi()) - 12;
    int typeW = 150, sizeW = 90, pctW = 62, filesW = 70, catW = 170, safeW = std::min(240, std::max(150, w96 / 6));
    list_.SetColumns({{L"NAME", std::max(120, w96 - typeW - sizeW - pctW - filesW - catW - safeW)}, {L"TYPE", typeW},
                      {L"SIZE", sizeW, true}, {L"%", pctW, true}, {L"FILES", filesW, true}, {L"CATEGORY", catW},
                      {L"SAFE", safeW}});
    list_.Paint(dc, rcList_, Ctx(), focus_ == Focus::List);

    // Bottom detail: three columns split by 1px lines.
    RECT d = rcDetail_;
    FillC(dc, d, t.panel);
    HLine(dc, d.left, d.right, d.top, t.border);
    int W = d.right - d.left;
    int x1 = d.left + W * 38 / 100, x2 = d.left + W * 69 / 100;
    VLine(dc, x1, d.top + 1, d.bottom, t.border);
    VLine(dc, x2, d.top + 1, d.bottom, t.border);
    int pad = Sc(10), top = d.top + Sc(6);
    // Column 1: name + kind, then the main facts.
    RECT tr = {d.left + pad, top, x1 - pad, top + Sc(20)};
    Text(dc, dm_.title, tr, f.semi, t.text1, TP);
    RECT kr = {d.left + pad, top + Sc(20), x1 - pad, top + Sc(38)};
    Text(dc, dm_.kind, kr, f.caption, t.text3, TL);
    conPane_[0].rc = {d.left + pad, top + Sc(42), x1 - Sc(4), d.bottom - Sc(4)};
    PaintPane(dc, conPane_[0], dm_.main);
    // Column 2: composition and the current view.
    std::vector<KV> c2{{L"COMPOSITION", L"", KV::Head}};
    c2.insert(c2.end(), dm_.comp.begin(), dm_.comp.end());
    c2.push_back({L"CURRENT VIEW", L"", KV::Head});
    c2.insert(c2.end(), dm_.view.begin(), dm_.view.end());
    conPane_[1].rc = {x1 + pad, top, x2 - Sc(4), d.bottom - Sc(4)};
    PaintPane(dc, conPane_[1], c2);
    // Column 3: the safe-to-delete verdict, with the rule's basis (rule 7).
    std::vector<KV> c3{{L"SAFE TO DELETE", L"", KV::Head}};
    c3.insert(c3.end(), dm_.safe.begin(), dm_.safe.end());
    conPane_[2].rc = {x2 + pad, top, d.right - Sc(4), d.bottom - Sc(4)};
    PaintPane(dc, conPane_[2], c3);
    if (focus_ == Focus::Detail) FocusRectC(dc, RECT{d.left + 1, d.top + 1, d.right - 1, d.bottom - 1}, Acc());
}

void DiskMapModule::DrawFocus(HDC dc) {
    if (focus_ != Focus::Click) return;
    for (auto& c : clicks_)
        if (c.id == focusId_ && c.data == focusData_) {
            RECT r = c.rc; InflateRect(&r, Sc(2), Sc(2));
            FocusRectC(dc, r, host_->Theme().accent);
            return;
        }
}

void DiskMapModule::Paint(HDC dc, const RECT& rc) {
    if (!host_) return;
    const Theme& t = host_->Theme();
    SetBkMode(dc, TRANSPARENT);
    FillC(dc, rc, t.bg);
    LayoutRects(rc);
    clicks_.clear();
    if (Console()) {
        DrawConsole(dc, rc);
    } else if (Three()) {
        DrawThree(dc);
    } else {
        DrawToolbar(dc);
        DrawSidebar(dc);
        DrawCrumb(dc, rcCrumb_, rcMap_.left);
        DrawMapArea(dc);
    }
    DrawFocus(dc);
}

// ============================================================================================
// Input

void DiskMapModule::UpdateHover(int mx, int my) {
    POINT p = {mx, my};
    int hc = -1;
    for (int i = (int)clicks_.size() - 1; i >= 0; i--) if (PtInRect(&clicks_[i].rc, p)) { hc = i; break; }
    int hh = -1;
    if (st_ == St::Done && !Console() && PtInRect(&rcMap_, p)) {
        POINT q = {mx - rcMap_.left, my - rcMap_.top};
        for (int i = (int)hits_.size() - 1; i >= 0; i--) if (PtInRect(&hits_[i].r, q)) { hh = i; break; }
    }
    int nid = hc >= 0 ? clicks_[hc].id : -1;
    u32 ndata = hc >= 0 ? clicks_[hc].data : 0;
    if (nid != hoverId_ || ndata != hoverData_ || hh != hoverHit_) {
        if (hh != hoverHit_) {
            hoverTopValid_ = false;
            if (hh >= 0 && hits_[hh].topKind != 255) {
                const Hit& h = hits_[hh];
                for (const Hit& o : hits_)
                    if (o.kind == h.topKind && o.id == h.top) { hoverTopRect_ = o.r; hoverTopValid_ = true; break; }
            }
        }
        hoverClick_ = hc >= 0; hoverId_ = nid; hoverData_ = ndata; hoverHit_ = hh;
        bool hand = hoverClick_ || (hh >= 0 && hits_[hh].topKind == 1);
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
        UpdateStatus();
        Invalidate();
    }
}

void DiskMapModule::ActivateClick(const Click& c, bool ctrl) {
    (void)ctrl;
    switch (c.id) {
    case ID_RESCAN: if (st_ == St::Done && !target_.fixture) StartScan(target_); break;
    case ID_STREAMS:   // flips the option and rescans what is shown (or being scanned) with it
        streams_ = !streams_;
        if (target_.fixture) { if (st_ == St::Done) { std::wstring d = target_.display; LoadFixture(d.c_str()); } }
        else if (st_ != St::Idle) StartScan(target_);
        break;
    case ID_CANCEL: worker_.Cancel(); haveQueued_ = false; break;
    case ID_UP: GoUp(); break;
    case ID_DRIVE:
        if (c.data < drives_.size()) {
            const Drive& d = drives_[c.data];
            Target t; t.letter = d.letter;
            t.display = std::wstring(1, d.letter) + L":\\";
            t.root = L"\\\\?\\" + t.display;
            t.used = UsedOf(d); t.total = d.total;
            StartScan(t);   // during a scan: cancels and restarts on that drive, even the same one
        }
        break;
    case ID_CRUMB: SetView(c.data); break;
    case ID_CAT: SetFilter(0, filter_ == (int)c.data ? -1 : (int)c.data); break;
    case ID_CLEARFILTER: SetFilter(sideTab_, sideTab_ == 1 ? FILTER_SAFE : -1); break;
    case ID_TAB: SetFilter((int)c.data, c.data == 1 ? FILTER_SAFE : -1); break;
    case ID_SAFE: SetFilter(1, filter_ == FILTER_SAFE + (int)c.data ? FILTER_SAFE : FILTER_SAFE + (int)c.data); break;
    case ID_SHOW: SetFilter(c.data ? 1 : 0, c.data ? FILTER_SAFE : -1); break;   // Console "show: all safe"
    }
    UpdateStatus();
    Invalidate();
}

void DiskMapModule::OnClick(int mx, int my, bool ctrl) {
    POINT p = {mx, my};
    for (int i = (int)clicks_.size() - 1; i >= 0; i--) {
        if (!PtInRect(&clicks_[i].rc, p)) continue;
        Click c = clicks_[i];
        ActivateClick(c, ctrl);
        return;
    }
    // Map clicks use the hover hit from the last mouse move, not the click point (legacy).
    if (st_ != St::Done || hoverHit_ < 0 || hoverHit_ >= (int)hits_.size()) return;
    const Hit h = hits_[hoverHit_];
    if (ctrl) {
        if (h.kind == 0) ShowInExplorer(data_->FilePath(h.id), true);
        else if (h.kind == 1) ShowInExplorer(data_->DirPath(h.id), true);
        return;
    }
    if (h.topKind == 1) SetView(h.top);
}

void DiskMapModule::CycleFocus(bool back) {
    // Focus ring: every clickable in paint order, then the search box and the list widgets. Tab /
    // Shift+Tab move it.
    struct Stop { Focus f; int id; u32 data; };
    std::vector<Stop> stops;
    for (auto& c : clicks_) stops.push_back({Focus::Click, c.id, c.data});
    if (Console() || Three()) {
        if (st_ == St::Done) {
            stops.push_back({Focus::Search, 0, 0});
            stops.push_back({Focus::List, 0, 0});
            if (Three() && rcLargest_.bottom > rcLargest_.top) stops.push_back({Focus::Largest, 0, 0});
            stops.push_back({Focus::Detail, 0, 0});
        }
    } else if (rcLargest_.bottom > rcLargest_.top) {
        stops.push_back({Focus::Largest, 0, 0});
    }
    if (stops.empty()) { focus_ = Focus::None; return; }
    int cur = -1;
    for (int i = 0; i < (int)stops.size(); i++) {
        const Stop& s = stops[i];
        if (s.f == focus_ && (s.f != Focus::Click || (s.id == focusId_ && s.data == focusData_))) { cur = i; break; }
    }
    int n = (int)stops.size();
    int nx = cur < 0 ? (back ? n - 1 : 0) : (cur + (back ? n - 1 : 1)) % n;
    focus_ = stops[nx].f; focusId_ = stops[nx].id; focusData_ = stops[nx].data;
    UpdateStatus();
}

bool DiskMapModule::OnKey(WPARAM vk, bool alt) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool modern = Console() || Three();   // Three-Pane and Console: list, search and detail keys
    PaintCtx c = Ctx();
    if (alt) {   // Alt+Home = root, Alt+Up / Alt+Left = up (Explorer's keys), in either layout
        if (vk == VK_HOME) { if (st_ == St::Done) SetView(0); return true; }
        GoUp();
        return true;
    }
    if (vk == VK_TAB) { CycleFocus(shift); return true; }
    // The search box owns editing keys while focused; F-keys still work. Backspace is swallowed here
    // (it is not Up) and arrives as WM_CHAR 0x08.
    if (focus_ == Focus::Search && modern && !(vk >= VK_F1 && vk <= VK_F24)) {
        if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_DOWN) { focus_ = Focus::List; return true; }
        search_.OnKey(vk, hwnd_);
        return true;
    }
    // Lists first: a focused list owns the navigation keys (Up/Down/PgUp/PgDn/Home/End/Enter).
    if (focus_ == Focus::List && st_ == St::Done &&
        (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_RETURN)) {
        if (vk == VK_RETURN && ctrl) { OpenSelected(true); return true; }
        list_.OnKey(vk, rcList_, c);
        if (vk != VK_RETURN) { SyncDetail(); UpdateStatus(); }
        return true;
    }
    if (focus_ == Focus::Detail && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT)) {
        int d = (vk == VK_UP ? 1 : vk == VK_DOWN ? -1 : vk == VK_PRIOR ? 4 : -4) * Sc(40);
        bool ch = false;
        for (KVPane* p : {&tpPane_, &conPane_[0], &conPane_[1], &conPane_[2]}) {
            int before = p->sb.Offset(); p->sb.SetOffset(before - d); ch |= p->sb.Offset() != before;
        }
        return ch;
    }
    if (focus_ == Focus::Largest && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
                                     vk == VK_HOME || vk == VK_END || vk == VK_RETURN)) {
        largestList_.OnKey(vk, rcLargest_, c);
        UpdateStatus();
        return true;
    }
    if (focus_ == Focus::Click && (vk == VK_RETURN || vk == VK_SPACE)) {
        for (auto& k : clicks_) if (k.id == focusId_ && k.data == focusData_) { Click cc = k; ActivateClick(cc, ctrl); break; }
        return true;
    }
    if (modern) {   // the Console F-key bar (FKeys) and its Three-Pane equivalents
        switch (vk) {
        case VK_RETURN: OpenSelected(ctrl); return true;
        case VK_F3: OpenSelected(true); return true;
        case VK_F6: SetFilter(filter_ >= FILTER_SAFE ? 0 : 1, filter_ >= FILTER_SAFE ? -1 : FILTER_SAFE); return true;
        case VK_F7: {   // next category present in the view, then back to all
            if (st_ != St::Done || !data_) return true;
            const DirE& v = data_->dirs[view_];
            int nf = -1;
            for (int k = (filter_ >= 0 && filter_ < NCAT) ? filter_ + 1 : 0; k < NCAT; k++) if (v.cat[k]) { nf = k; break; }
            SetFilter(0, nf);
            return true;
        }
        case VK_F8: { Click cc{RECT{}, ID_STREAMS, 0}; ActivateClick(cc, false); return true; }
        case VK_OEM_2: case VK_DIVIDE:   // '/': focus the search box
            if (st_ != St::Done) return false;
            focus_ = Focus::Search;
            skipSlash_ = true;
            return true;
        }
    }
    switch (vk) {   // legacy keys (the complete legacy list): Backspace/Escape = Up, F5 = rescan, Home = root
    case VK_BACK: case VK_ESCAPE: GoUp(); return true;   // Escape does not cancel a scan (legacy)
    case VK_F5: if (st_ == St::Done && !target_.fixture) StartScan(target_); return true;
    case VK_HOME: if (st_ == St::Done) SetView(0); return true;
    }
    return false;
}

bool DiskMapModule::OnInput(UINT msg, WPARAM w, LPARAM l) {
    if (!host_) return false;
    PaintCtx c = Ctx();
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    POINT p{x, y};
    bool modern = Console() || Three();
    bool done = st_ == St::Done;
    auto paneAt = [&](POINT q) -> KVPane* {
        if (Three()) return PtInRect(&tpPane_.rc, q) ? &tpPane_ : nullptr;
        if (Console()) for (auto& pn : conPane_) if (PtInRect(&pn.rc, q)) return &pn;
        return nullptr;
    };
    auto paneMouse = [&](UINT m) {
        bool ch = false;
        for (KVPane* pn : {&tpPane_, &conPane_[0], &conPane_[1], &conPane_[2]}) {
            if (!pn->sb.Visible()) continue;
            RECT tr = PaneTrack(*pn);
            if (pn->sb.Dragging() || (m == WM_LBUTTONDOWN && PtInRect(&tr, p))) ch |= pn->sb.OnMouse(m, x, y, tr) || m == WM_LBUTTONDOWN;
        }
        return ch;
    };
    switch (msg) {
    case WM_MOUSEMOVE: {
        bool ch = false;
        if (splitDrag_) {
            int px = splitW0_ + (splitX0_ - x);
            int nw = std::max(240, std::min(900, MulDiv(px, 96, (int)host_->Dpi())));
            if (nw != detailW96_) { detailW96_ = nw; mapDirty_ = true; ch = true; }
            return ch;
        }
        if (search_.Dragging()) return search_.OnMouse(hwnd_, msg, x, y, rcSearch_, c);
        if (modern) {
            ch |= list_.OnMouse(msg, x, y, rcList_, c);
            ch |= paneMouse(msg);
            if (Three() && rcLargest_.bottom > rcLargest_.top) ch |= largestList_.OnMouse(msg, x, y, rcLargest_, c);
        } else if (rcLargest_.bottom > rcLargest_.top) {
            ch |= largestList_.OnMouse(msg, x, y, rcLargest_, c);
        }
        UpdateHover(x, y);
        return ch;
    }
    case WM_LBUTTONDOWN: {
        bool ctrl = (w & MK_CONTROL) != 0;
        if (modern && done) {
            if (Three() && PtInRect(&rcSplit_, p)) {
                splitDrag_ = true; splitX0_ = x; splitW0_ = rcDetail_.right - rcDetail_.left;
                SetCapture(hwnd_);
                return true;
            }
            if (PtInRect(&rcSearch_, p)) {
                focus_ = Focus::Search;
                search_.OnMouse(hwnd_, msg, x, y, rcSearch_, c);
                return true;
            }
            if (PtInRect(&rcList_, p)) {
                focus_ = Focus::List;
                list_.OnMouse(msg, x, y, rcList_, c);   // may fire onActivate (double-click) -> SetView
                SyncDetail(); UpdateStatus();
                return true;
            }
            if (paneMouse(msg) || (PtInRect(&rcDetail_, p) && !PtInRect(&rcLargest_, p) && paneAt(p))) {
                focus_ = Focus::Detail;
                return true;
            }
        }
        if (PtInRect(&rcLargest_, p)) {
            focus_ = Focus::Largest;
            largestList_.OnMouse(msg, x, y, rcLargest_, c);
            UpdateStatus();
            return true;
        }
        if (!modern || (focus_ != Focus::List && focus_ != Focus::Search)) focus_ = Focus::None;
        OnClick(x, y, ctrl);
        UpdateHover(x, y);
        return true;
    }
    case WM_LBUTTONUP: {
        if (splitDrag_) {
            splitDrag_ = false;
            ReleaseCapture();
            host_->Prefs().SetInt(L"tp_detail_w", detailW96_);   // saved with the host's settings
            return true;
        }
        if (search_.Dragging()) return search_.OnMouse(hwnd_, msg, x, y, rcSearch_, c);
        bool ch = false;
        if (modern) ch |= list_.OnMouse(msg, x, y, rcList_, c) | paneMouse(msg);
        if (rcLargest_.bottom > rcLargest_.top) ch |= largestList_.OnMouse(msg, x, y, rcLargest_, c);
        return ch;
    }
    case WM_MOUSEWHEEL: {
        // Legacy ignored the wheel; every list scrolls with it now. lParam is in screen coordinates.
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        POINT sp{x, y}; ScreenToClient(hwnd_, &sp);
        if (PtInRect(&rcLargest_, sp)) return largestList_.OnWheel(delta, rcLargest_, c);
        if (modern) {
            if (KVPane* pn = paneAt(sp)) {
                int before = pn->sb.Offset();
                pn->sb.SetOffset(before - (delta / WHEEL_DELTA) * Sc(48));
                return pn->sb.Offset() != before;
            }
            if (PtInRect(&rcList_, sp) || Console()) return list_.OnWheel(delta, rcList_, c);
        }
        return false;
    }
    case WM_RBUTTONDOWN: if (modern && PtInRect(&rcSearch_, p)) return false; GoUp(); return true;   // right-click anywhere: Up
    case WM_XBUTTONDOWN: if (GET_XBUTTON_WPARAM(w) == XBUTTON1) GoUp(); return true;
    case WM_KEYDOWN: {
        if (l == 0 && focus_ == Focus::Search) focus_ = Focus::List;   // an F-key bar click acts on the list
        if ((w == VK_OEM_2 || w == VK_DIVIDE) && focus_ != Focus::Search) {
            bool r = OnKey(w, false);
            if (r && l == 0) skipSlash_ = false;   // an F-key bar click: no WM_CHAR follows
            return r;
        }
        return OnKey(w, false);
    }
    case WM_SYSKEYDOWN:   // Alt+Home = root, Alt+Up / Alt+Left = up; other Alt keys are not ours
        if (w == VK_HOME || w == VK_UP || w == VK_LEFT) return OnKey(w, true);
        return false;
    case WM_CHAR:
        if (focus_ != Focus::Search || !modern) return false;
        if (skipSlash_ && w == L'/') { skipSlash_ = false; return true; }
        skipSlash_ = false;
        if (w == 0x1B || w == L'\r' || w == L'\t') return false;   // handled as keys
        return search_.OnChar((wchar_t)w);
    }
    return false;
}

// ============================================================================================
// Layout chrome: Three-Pane tree sections, Console F-keys and machine line

std::vector<NavNode> DiskMapModule::NavNodes() {
    std::vector<NavNode> out;
    bool have = st_ == St::Done && data_;
    const DirE* v = have ? &data_->dirs[view_] : nullptr;
    NavNode cats; cats.id = NAV_CATS; cats.label = L"Categories";
    if (v) cats.note = FmtSize(v->total);
    out.push_back(cats);
    int order[NCAT]; for (int i = 0; i < NCAT; i++) order[i] = i;
    if (v) std::stable_sort(order, order + NCAT, [&](int a, int b) { return v->cat[a] > v->cat[b]; });
    for (int oi = 0; oi < NCAT; oi++) {
        int c = order[oi];
        if (v && !v->cat[c]) continue;
        NavNode n; n.id = NAV_CAT0 + c; n.depth = 1; n.label = CatName(c);
        if (v) n.note = FmtSize(v->cat[c]);
        out.push_back(n);
    }
    NavNode safe; safe.id = NAV_SAFE; safe.label = L"Safe to delete";
    if (v) safe.note = FmtSize(SafeSum(v->safeR));
    out.push_back(safe);
    if (v) {
        int ro[NSAFE]; int no = 0;
        for (int r = 1; r < NSAFE; r++) if (v->safeR[r]) ro[no++] = r;
        std::stable_sort(ro, ro + no, [&](int a, int b) { return v->safeR[a] > v->safeR[b]; });
        for (int i = 0; i < no; i++) {
            NavNode n; n.id = NAV_REASON0 + ro[i]; n.depth = 1; n.label = SafeReason(ro[i]).name;
            n.note = FmtSize(v->safeR[ro[i]]);
            out.push_back(n);
        }
    }
    return out;
}

int DiskMapModule::NavSelected() {
    if (sideTab_ == 1) return filter_ > FILTER_SAFE ? NAV_REASON0 + (filter_ - FILTER_SAFE) : NAV_SAFE;
    return (filter_ >= 0 && filter_ < NCAT) ? NAV_CAT0 + filter_ : NAV_CATS;
}

void DiskMapModule::OnNavSelect(int id) {
    if (id == NAV_CATS) SetFilter(0, -1);
    else if (id == NAV_SAFE) SetFilter(1, FILTER_SAFE);
    else if (id >= NAV_CAT0 && id < NAV_CAT0 + NCAT) SetFilter(0, id - NAV_CAT0);
    else if (id > NAV_REASON0 && id < NAV_REASON0 + NSAFE) SetFilter(1, FILTER_SAFE + (id - NAV_REASON0));
}

std::vector<FKey> DiskMapModule::FKeys() {
    return {{VK_F5, L"F5", L"Rescan"},        {VK_BACK, L"Bksp", L"Up"},
            {VK_RETURN, L"Enter", L"Open"},   {VK_F3, L"F3", L"Show in Explorer"},
            {VK_HOME, L"Home", L"Root"},      {VK_F6, L"F6", L"Safe only"},
            {VK_F7, L"F7", L"Next category"}, {VK_F8, L"F8", L"Data streams"},
            {VK_OEM_2, L"/", L"Search"},      {VK_TAB, L"Tab", L"Pane"}};
}

// "C:\ used=566GB/931GB files=1,234,567 mapped=540GB safe=12.3GB src=live mode=READ-ONLY".
std::wstring DiskMapModule::ConsoleHeader() {
    auto tight = [](std::wstring s) { s.erase(std::remove(s.begin(), s.end(), L' '), s.end()); return s; };
    std::wstring h;
    if (target_.fixture) {
        size_t cut = target_.display.find_last_of(L'\\');
        h = L"fixture=" + (cut == std::wstring::npos ? target_.display : target_.display.substr(cut + 1));
    } else if (st_ != St::Idle && target_.letter) {
        h = target_.display;
        if (target_.total) h += L" used=" + tight(FmtSize(target_.used)) + L"/" + tight(FmtSize(target_.total));
    } else {
        h = L"drives=";
        for (size_t i = 0; i < drives_.size(); i++) h += (i ? L"," : L"") + std::wstring(1, drives_[i].letter) + L":";
        if (drives_.empty()) h += L"none";
    }
    if (st_ == St::Scanning) {
        h += L" scanning files=" + U64Str(prog_.files.load()) + L" found=" + tight(FmtSize(prog_.bytes.load()));
    } else if (st_ == St::Done && data_) {
        const DirE& r = data_->dirs[0];
        h += L" files=" + U64Str(r.nFiles) + L" mapped=" + tight(FmtSize(r.total)) + L" safe=" + tight(FmtSize(SafeSum(r.safeR)));
        if (data_->stats.deniedDirs) h += L" unreadable=" + U64Str(data_->stats.deniedDirs);
    }
    h += target_.fixture ? L" src=fixture" : L" src=live";
    if (!elevated_) h += L" elevated=no";
    h += L" mode=READ-ONLY";
    return h;
}

// ============================================================================================
// Headless: fixture load and selftest

void DiskMapModule::LoadFixture(const wchar_t* dir) {
    // Synchronous, no Worker, so --screenshot is deterministic. The fixture root is treated as a
    // drive root so the drive-root-relative safe-to-delete rules fire.
    if (!host_ || !dir) return;
    worker_.Cancel(); worker_.Join();
    Target t;
    t.fixture = true;
    t.root = sys::ToExtended(dir);   // Scan::Run wants \\?\ form (long paths; relative paths made absolute)
    t.display = sys::ToShell(dir);
    target_ = t;
    auto s = std::make_unique<Scan>();
    s->Run(t.root, t.display, [] { return false; }, nullptr, streams_);
    if (!s->ok) {
        lastError_ = L"Could not read fixture " + t.display + L": " + sys::ErrorText(s->rootErr);
        st_ = St::Idle;
        UpdateStatus();
        return;
    }
    data_ = std::move(s);
    EnterDone();
    if (Console() && !rows_.empty()) { list_.Select(0); SyncDetail(); UpdateStatus(); }
}

// Build the make_diskmap_tree.py tree under %TEMP% when --selftest gets no --fixture. Never deletes:
// it reuses a complete earlier copy, or builds into a new folder name.
std::wstring BuildSelftestFixture(std::wstring* note) {
    wchar_t tmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (!n || n > MAX_PATH) { *note = L"GetTempPath failed: " + sys::ErrorText(GetLastError()); return L""; }
    std::wstring base = sys::ToExtended(tmp);
    if (base.back() != L'\\') base += L'\\';
    static const struct { const wchar_t* rel; u64 size; } kFiles[] = {
        {L"Games\\SteamLibrary\\steamapps\\common\\BigGame\\data.pak", 5000000},
        {L"Games\\SteamLibrary\\steamapps\\common\\BigGame\\game.exe", 400000},
        {L"Games\\SteamLibrary\\steamapps\\common\\SmallGame\\level1.bin", 1200000},
        {L"Videos\\Captures\\clip1.mp4", 3000000}, {L"Videos\\Captures\\clip2.mkv", 2000000},
        {L"Music\\album\\track01.flac", 900000}, {L"Pictures\\holiday\\img001.jpg", 250000},
        {L"Documents\\report.pdf", 120000}, {L"Documents\\notes.txt", 4096},
        {L"dev\\project\\src\\main.cpp", 8192}, {L"dev\\project\\node_modules\\pkg\\index.js", 16384},
        {L"Temp\\cache\\blob.tmp", 700000}, {L"Windows\\Temp\\setup.log", 60000},
        {L"CrashDumps\\app.dmp", 1500000}, {L"Archives\\backup.zip", 2500000},
    };
    std::wstring longDir;
    for (int i = 0; i < 14; i++) { wchar_t b[32]; swprintf(b, 32, L"deep_folder_name_%02d", i); longDir += (i ? L"\\" : L"") + std::wstring(b); }
    for (int attempt = 0; attempt < 50; attempt++) {
        std::wstring root = base + L"hadron-diskmap-selftest" + (attempt ? L"-" + std::to_wstring(attempt) : L"");
        if (GetFileAttributesW((root + L"\\complete.marker").c_str()) != INVALID_FILE_ATTRIBUTES) return root + L"\\tree";
        if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) continue;   // partial: leave it alone
        auto mkdirs = [](const std::wstring& path) {
            for (size_t i = 7; i <= path.size(); i++)
                if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        };
        auto write = [&](const std::wstring& path, u64 size) {
            mkdirs(path.substr(0, path.rfind(L'\\')));
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            std::vector<BYTE> blk(65536);
            for (size_t i = 0; i < blk.size(); i++) blk[i] = (BYTE)((i * 31 + 7) & 0xFF);
            bool ok = true;
            for (u64 left = size; left && ok;) {
                DWORD k = (DWORD)std::min<u64>(left, blk.size()), wr = 0;
                ok = WriteFile(h, blk.data(), k, &wr, nullptr) && wr == k;
                left -= k;
            }
            CloseHandle(h);
            return ok;
        };
        std::wstring tree = root + L"\\tree", outside = root + L"\\outside_target";
        mkdirs(tree); mkdirs(outside);
        bool ok = write(outside + L"\\SENTINEL_must_not_be_counted.bin", 10000000);
        for (auto& f : kFiles) ok = ok && write(tree + L"\\" + f.rel, f.size);
        ok = ok && write(tree + L"\\" + longDir + L"\\long_path_file.dat", 333333);
        ok = ok && CreateHardLinkW((tree + L"\\Documents\\report-hardlink.pdf").c_str(), (tree + L"\\Documents\\report.pdf").c_str(), nullptr);
        mkdirs(tree + L"\\Links");
        // 0x2 = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (not in every SDK).
        if (!CreateSymbolicLinkW((tree + L"\\Links\\outside-symlink").c_str(), L"..\\..\\outside_target",
                                 SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2))
            *note = L"symlink not created: " + sys::ErrorText(GetLastError());
        if (!ok) { *note = L"could not build the fixture in " + sys::ToShell(root) + L": " + sys::ErrorText(GetLastError()); return L""; }
        write(root + L"\\complete.marker", 0);
        return tree;
    }
    *note = L"no free fixture folder name under %TEMP%";
    return L"";
}

bool ProcessHasPrivilegeEnabled(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    LUID want{};
    bool on = false;
    DWORD need = 0;
    GetTokenInformation(tok, TokenPrivileges, nullptr, 0, &need);
    std::vector<BYTE> buf(need ? need : 1);
    if (LookupPrivilegeValueW(nullptr, name, &want) && GetTokenInformation(tok, TokenPrivileges, buf.data(), need, &need)) {
        auto* tp = reinterpret_cast<TOKEN_PRIVILEGES*>(buf.data());
        for (DWORD i = 0; i < tp->PrivilegeCount; i++)
            if (tp->Privileges[i].Luid.LowPart == want.LowPart && tp->Privileges[i].Luid.HighPart == want.HighPart)
                on = (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) != 0;
    }
    CloseHandle(tok);
    return on;
}

// The outside symlink is one link; the self-built fixture may have none (Wine creates no symlink).
u64 linkExpectedCount(bool own, const std::wstring& root) {
    if (!own) return 1;
    DWORD a = GetFileAttributesW((root + L"\\Links\\outside-symlink").c_str());
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) ? 1 : 0;
}

int DiskMapModule::RunCli(int argc, wchar_t** argv) {
    bool selftest = false;
    for (int i = 0; i < argc; i++) if (std::wstring(argv[i]) == L"--selftest") selftest = true;
    if (!selftest) return -1;

    int fails = 0, checks = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        checks++; if (!ok) fails++;
        wprintf(L"  [%ls] %ls\n", ok ? L"PASS" : L"FAIL", what.c_str());
    };
    auto eq = [&](u64 got, u64 want, const std::wstring& what) {
        check(got == want, what + L" = " + U64Str(got) + (got == want ? L"" : L" (want " + U64Str(want) + L")"));
    };
    wprintf(L"diskmap selftest\n");

    // --- formats and rules (pure) ---
    check(FmtSize(1023) == L"1,023 B", L"FmtSize(1023) = \"1,023 B\"");
    check(FmtSize(1536) == L"1.50 KB", L"FmtSize(1536) = \"1.50 KB\"");
    check(FmtSize(1048575) == L"1024 KB", L"FmtSize(1048575) = \"1024 KB\" (legacy edge)");
    check(U64Str(17992005) == L"17,992,005", L"U64Str thousands separators");
    eq(SafeDirRules().size(), 25, L"safe dir rules");
    eq(SafeFileRules().size(), 3, L"safe file rules");
    bool basisOk = true;
    for (auto& r : SafeDirRules()) basisOk = basisOk && r.basis && *r.basis;
    for (auto& r : SafeFileRules()) basisOk = basisOk && r.basis && *r.basis;
    for (int r = 1; r < NSAFE; r++) basisOk = basisOk && *SafeReason(r).basis;
    check(basisOk, L"every safe reason and rule carries a basis (rule 7)");
    const Rules& R = Rules::Get();
    std::vector<std::wstring> sg = {L"users", L"bob", L"appdata", L"local", L"temp"};
    check(R.DirSafe(sg) == SF_TEMP, L"users\\*\\appdata\\local\\temp -> Temporary files");
    sg = {L"steamapps", L"shadercache"};
    check(R.DirSafe(sg) == SF_SHADER, L"**\\steamapps\\shadercache matches at the root (** = 0 segments)");
    sg = {L"users", L"bob", L"appdata", L"roaming", L"x", L"cache"};
    check(R.DirSafe(sg) == SF_CACHE, L"users\\*\\appdata\\**\\cache still flags a plain AppData cache (any depth)");
    // Narrowed cache rule: caches inside per-user installs are part of the app, never flagged.
    sg = {L"users", L"bob", L"appdata", L"local", L"programs", L"app", L"cache"};
    check(R.DirSafe(sg) == SF_NONE, L"AppData\\Local\\Programs\\...\\cache is not flagged");
    sg = {L"users", L"bob", L"appdata", L"roaming", L"x", L"node_modules", L"pkg", L"cache"};
    check(R.DirSafe(sg) == SF_NONE, L"...\\node_modules\\...\\cache is not flagged");
    sg = {L"users", L"bob", L"appdata", L"local", L"python", L"lib", L"site-packages", L"foo", L"cache"};
    check(R.DirSafe(sg) == SF_NONE, L"...\\site-packages\\...\\cache is not flagged");
    sg = {L"users", L"bob", L"appdata", L"local", L"programs", L"app", L"gpucache"};
    check(R.DirSafe(sg) == SF_CACHE, L"GPUCache rule is unchanged (still flagged under Local\\Programs)");
    // Narrowed root AMD/NVIDIA rule: only with an installer marker in the folder's own listing.
    for (const wchar_t* v : {L"amd", L"nvidia"}) {
        sg = {v};
        check(R.DirSafe(sg) == SF_NONE && R.NeedsInstallerMarker(sg),
              std::wstring(L"root ") + v + L" without setup.exe/Packages is not flagged");
        check(R.DirSafe(sg, true) == SF_DRVPKG, std::wstring(L"root ") + v + L" with setup.exe/Packages -> Driver installer leftovers");
    }
    check(IsInstallerMarker(L"setup.exe", false) && IsInstallerMarker(L"packages", true) &&
          !IsInstallerMarker(L"setup.exe", true) && !IsInstallerMarker(L"packages", false),
          L"installer marker = a file setup.exe or a folder Packages (links never count)");
    sg = {L"programdata", L"nvidia corporation", L"downloader"};
    check(R.DirSafe(sg) == SF_DRVPKG && !R.NeedsInstallerMarker(sg), L"ProgramData\\NVIDIA Corporation\\Downloader unchanged (no marker needed)");
    sg = {L"users", L"bob", L"appdata", L"local", L"microsoft", L"windows", L"explorer"};
    check(R.FileSafe(sg, L"thumbcache_256.db") == SF_THUMB, L"thumbcache_*.db file rule");
    sg = {L"windows"};
    check(R.FileSafe(sg, L"memory.dmp") == SF_DUMPS, L"windows\\memory.dmp file rule");
    check(FileCat(NOCTX, L"app.ts", 3) == C_VIDEO, L".ts is Video before Developer (legacy order)");
    check(FileCat(NOCTX, L"pagefile.sys", 0) == C_WINDOWS && FileCat(NOCTX, L"pagefile.sys", 1) == C_DRIVERS,
          L"root system files only at depth 0");
    check(DirCtx(C_WINDOWS, L"driverstore", 2) == C_DRIVERS, L"Windows\\DriverStore -> Drivers");
    check(DirCtx(C_PROGRAMS, L"temp", 2) == C_TEMP, L"temp beats Programs context");

    // --- AMD/NVIDIA installer marker anywhere in the subtree (in-memory tree, no I/O) ---
    {
        Scan m;
        auto D = [&](const wchar_t* n, u32 parent, u32 depth, u32 fileFirst, u32 fileCount, u32 kidFirst, u32 kidCount) {
            DirE d; d.name = m.AddName(n, wcslen(n)); d.parent = parent; d.depth = depth;
            d.fileFirst = fileFirst; d.fileCount = fileCount; d.kidFirst = kidFirst; d.kidCount = kidCount;
            m.dirs.push_back(d);
        };
        auto F = [&](const wchar_t* n, u32 dir, u8 flags) {
            FileE f; f.name = m.AddName(n, wcslen(n)); f.dir = dir; f.flags = flags; f.size = 100; m.files.push_back(f);
        };
        // 0 root: 1 amd, 2 nvidia, 6 nvidia2 | amd\x(3)\y(4)\Setup.exe | nvidia\foo(5): readme.txt, a
        // setup.exe that is a link, and a folder link named Packages | nvidia2\sub(7)\Packages(8)
        D(L"root", 0, 0, 0, 0, 0, 3);
        D(L"amd", 0, 1, 0, 1, 3, 1);
        D(L"nvidia", 0, 1, 1, 1, 4, 1);
        D(L"x", 1, 2, 2, 0, 5, 1);
        D(L"y", 3, 3, 2, 1, 6, 0);
        D(L"foo", 2, 2, 3, 2, 6, 0);
        D(L"nvidia2", 0, 1, 5, 0, 6, 1);
        D(L"sub", 6, 2, 5, 0, 7, 1);
        D(L"Packages", 7, 3, 5, 0, 8, 0);
        m.kids = {1, 2, 6, 3, 5, 4, 7, 8};
        F(L"other.bin", 1, 0);                              // 0: amd
        F(L"Packages", 2, FF_REPARSE | FF_REPARSE_DIR);     // 1: nvidia (folder link)
        F(L"Setup.exe", 4, 0);                              // 2: amd\x\y
        F(L"readme.txt", 5, 0);                             // 3: nvidia\foo
        F(L"setup.exe", 5, FF_REPARSE);                     // 4: nvidia\foo (file link)
        m.MarkInstallerFolders({1, 2, 6});
        check(m.dirs[1].safe == SF_DRVPKG && m.dirs[3].safe == SF_DRVPKG && m.dirs[4].safe == SF_DRVPKG &&
              m.files[0].safe == SF_DRVPKG && m.files[2].safe == SF_DRVPKG,
              L"nested marker (amd\\x\\y\\setup.exe) flags root amd and every folder and file under it");
        check(m.dirs[2].safe == SF_NONE && m.dirs[5].safe == SF_NONE &&
              m.files[1].safe == SF_NONE && m.files[3].safe == SF_NONE && m.files[4].safe == SF_NONE,
              L"root nvidia with no marker is not flagged (a setup.exe link and a Packages link don't count)");
        check(m.dirs[6].safe == SF_DRVPKG && m.dirs[7].safe == SF_DRVPKG && m.dirs[8].safe == SF_DRVPKG,
              L"nested Packages folder flags the candidate and its subtree");
    }

    // --- marker + data streams on disk: a small tree the selftest builds (never deleted, reused) ---
    {
        wchar_t tmp[MAX_PATH + 1];
        DWORD tn = GetTempPathW(MAX_PATH + 1, tmp);
        std::wstring mr = (tn && tn <= MAX_PATH) ? sys::ToExtended(tmp) : std::wstring();
        if (!mr.empty() && mr.back() != L'\\') mr += L'\\';
        mr += L"hadron-diskmap-marker-selftest";
        auto mk = [](const std::wstring& path) {
            for (size_t i = 7; i <= path.size(); i++)
                if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        };
        auto put = [&](const std::wstring& path, DWORD size, DWORD disp) {
            mk(path.substr(0, path.rfind(L'\\')));
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
            std::vector<BYTE> b(size, 0x5A);
            DWORD wr = 0;
            bool ok = !size || (WriteFile(h, b.data(), size, &wr, nullptr) && wr == size);
            CloseHandle(h);
            return ok;
        };
        bool built = !mr.empty() && put(mr + L"\\AMD\\x\\y\\Setup.exe", 1000, CREATE_NEW) &&
                     put(mr + L"\\AMD\\x\\data.bin", 2000, CREATE_NEW) &&
                     put(mr + L"\\NVIDIA\\foo\\readme.txt", 500, CREATE_NEW) &&
                     put(mr + L"\\streams\\host.bin", 3000, CREATE_NEW);
        check(built, L"marker/stream tree available");
        if (built) {
            // Named stream host.bin:hx (5,000 B). Only trusted if the OS lists it as a stream: Wine has
            // no NTFS streams and may create nothing, or a plain file, instead.
            put(mr + L"\\streams\\host.bin:hx", 5000, CREATE_ALWAYS);
            bool adsThere = false;
            WIN32_FIND_STREAM_DATA sd;
            HANDLE sh = FindFirstStreamW((mr + L"\\streams\\host.bin").c_str(), FindStreamInfoStandard, &sd, 0);
            if (sh != INVALID_HANDLE_VALUE) {
                do { if (wcscmp(sd.cStreamName, L":hx:$DATA") == 0 && sd.StreamSize.QuadPart == 5000) adsThere = true; }
                while (FindNextStreamW(sh, &sd));
                FindClose(sh);
            }
            auto scanTree = [&](bool streams, Scan& t) { return t.Run(mr, sys::ToShell(mr), [] { return false; }, nullptr, streams) && t.ok; };
            auto findDir = [](const Scan& t, const wchar_t* n) {
                for (u32 i = 1; i < t.dirs.size(); i++) if (t.dirs[i].depth == 1 && _wcsicmp(t.Name(t.dirs[i].name), n) == 0) return i;
                return 0u;
            };
            auto hostSize = [](const Scan& t) {
                for (const FileE& f : t.files) if (wcscmp(t.Name(f.name), L"host.bin") == 0) return f.size;
                return (u64)0;
            };
            Scan off, on;
            bool okOff = scanTree(false, off), okOn = scanTree(true, on);
            check(okOff && okOn, L"marker/stream tree scanned (streams off and on)");
            if (okOff && okOn) {
                u32 amd = findDir(off, L"amd"), nv = findDir(off, L"nvidia");
                check(amd && off.dirs[amd].safe == SF_DRVPKG && off.dirs[amd].safeR[SF_DRVPKG] == 3000,
                      L"scan: root AMD with a nested Setup.exe -> Driver installer leftovers, whole subtree (3,000 B)");
                check(nv && off.dirs[nv].safe == SF_NONE && off.dirs[nv].safeR[SF_DRVPKG] == 0,
                      L"scan: root NVIDIA with no marker anywhere is not flagged");
                eq(hostSize(off), 3000, L"streams off: host.bin counted without its named stream");
                eq(off.stats.adsFiles, 0, L"streams off: no stream tally");
                if (adsThere) {
                    eq(hostSize(on), 8000, L"streams on: host.bin counted with its 5,000 B named stream");
                    eq(on.stats.adsBytes, 5000, L"streams on: stream bytes tallied");
                } else {
                    wprintf(L"  [SKIP] named data stream: this OS created no host.bin:hx stream (no NTFS streams); run on Windows/NTFS\n");
                }
            }
        }
    }

    // --- fixture scan ---
    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    std::wstring note, root = cl.fixture.empty() ? std::wstring() : sys::ToExtended(cl.fixture);
    bool own = root.empty();
    if (own) root = BuildSelftestFixture(&note);
    if (root.empty()) {
        check(false, L"fixture available: " + note);
    } else {
        wprintf(L"  fixture: %ls%ls\n", sys::ToShell(root).c_str(), own ? L" (built by selftest)" : L"");
        if (own)   // honest about the ~38 MB it leaves behind (never deleted; reused next run)
            wprintf(L"  fixture left at %ls (about 38 MB; reused by the next --selftest, never deleted)\n",
                    sys::ToShell(root.substr(0, root.rfind(L'\\'))).c_str());
        if (!note.empty()) wprintf(L"  note: %ls\n", note.c_str());
        Scan s;
        bool ran = s.Run(root, sys::ToShell(root), [] { return false; }, nullptr, StreamsArg());
        check(ran && s.ok, L"scan completed");
        if (ran && s.ok) {
            const DirE& r0 = s.dirs[0];
            eq(s.TotalBytes(), 17992005, L"total bytes, hard link counted once (bytes_unique_fileid)");
            eq(s.stats.hardlinkDupFiles, 1, L"hard-link duplicates found");
            eq(s.stats.hardlinkDupBytes, 120000, L"hard-link bytes not counted twice");
            bool sentinel = false, longOk = false, linkLeaf = false;
            u64 plainFiles = 0;
            for (u32 i = 0; i < s.files.size(); i++) {
                const FileE& f = s.files[i];
                std::wstring n = s.Name(f.name);
                if (n == L"SENTINEL_must_not_be_counted.bin") sentinel = true;
                if (n == L"long_path_file.dat" && f.size == 333333) longOk = s.FilePath(i).size() > MAX_PATH;
                if (n == L"outside-symlink" && (f.flags & FF_REPARSE_DIR)) linkLeaf = true;
                if (!(f.flags & FF_REPARSE)) plainFiles++;
            }
            check(!sentinel, L"10,000,000 B sentinel behind the outside symlink is excluded");
            // The self-built fixture may lack the link: Wine 9's CreateSymbolicLinkW reports success
            // but creates nothing. Assert only when a reparse point is really there (always with
            // --fixture from make_diskmap_tree.py).
            DWORD la = GetFileAttributesW((root + L"\\Links\\outside-symlink").c_str());
            bool linkThere = la != INVALID_FILE_ATTRIBUTES && (la & FILE_ATTRIBUTE_REPARSE_POINT);
            if (!own || linkThere) check(linkLeaf, L"outside symlink is a leaf (reparse, not traversed)");
            else wprintf(L"  [SKIP] outside symlink is a leaf: this OS created no symlink in the self-built fixture; run with --fixture <make_diskmap_tree.py tree>\n");
            check(longOk, L"long path (> 260 chars) file found at 333,333 B");
            eq(plainFiles, 17, L"file entries, every link included (file_count_per_link)");
            eq(r0.cat[C_GAMES], 6600000, L"Games");
            eq(r0.cat[C_VIDEO], 5000000, L"Video");
            eq(r0.cat[C_TEMP], 2260000, L"Temp & cache (Temp\\cache, Windows\\Temp, CrashDumps)");
            eq(r0.cat[C_DOCS], 124096, L"Documents (hard link once)");
            eq(r0.cat[C_DEV], 24576, L"Developer (node_modules context + .cpp)");
            eq(r0.cat[C_ARCHIVES], 2500000, L"Archives & disk images");
            eq(r0.cat[C_OTHER], 333333, L"Other");
            eq(r0.safeR[SF_TEMP], 60000, L"safe: Temporary files (windows\\temp at the fixture root)");
            eq(SafeSum(r0.safeR), 60000, L"safe: total");
            eq(s.stats.deniedDirs, 0, L"unreadable folders");
            eq(s.stats.reparseDirs + s.stats.linkFiles, linkExpectedCount(own, root), L"links not followed (reported in the UI)");
            // Finding 1: the privilege lives on the scan thread only. After Run the thread is not
            // impersonating and the process token does not have SeBackupPrivilege enabled.
            HANDLE tt = nullptr;
            bool noThreadToken = !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tt) && GetLastError() == ERROR_NO_TOKEN;
            if (tt) CloseHandle(tt);
            check(noThreadToken, L"scan reverted its impersonation (no thread token after Run)");
            check(!ProcessHasPrivilegeEnabled(L"SeBackupPrivilege"), L"SeBackupPrivilege not enabled on the process token");
            wprintf(L"  info: SeBackupPrivilege on the scan thread: %ls\n",
                    s.stats.backupOk ? L"enabled" : (L"not enabled (" + sys::ErrorText(s.stats.backupErr) + L")").c_str());
        }
    }
    wprintf(L"diskmap selftest: %ls (%d checks, %d failed)\n", fails ? L"FAIL" : L"PASS", checks, fails);
    fflush(stdout);
    return fails ? 1 : 0;
}

}  // namespace

std::unique_ptr<Module> MakeDiskMapModule() { return std::make_unique<DiskMapModule>(); }

}  // namespace hx
