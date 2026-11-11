// core/host/host.cpp — the shared host. Implements IHost, owns the frame, the message loop, DPI,
// COM, logging and the deterministic --screenshot path. Standalone and suite differ only in whether
// the sidebar is drawn and how many modules are present.
#include "core/host/run.h"
#include "core/host/cmdline.h"
#include "core/host/registry.h"
#include "core/sys/sys.h"
#include "core/sys/settings.h"
#include "core/sys/log.h"
#include "core/sys/png.h"
#include "core/ui/theme.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>   // CoInitializeEx/CoUninitialize: WIN32_LEAN_AND_MEAN excludes OLE on MSVC
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

private:
    int sidebarW() const { return suite_ ? S(188, dpi_) : 0; }
    int statusH() const { return S(28, dpi_); }
    RECT SidebarRect(const RECT& cl) const { return {0, 0, sidebarW(), cl.bottom - statusH()}; }
    RECT ContentRect(const RECT& cl) const { return {sidebarW(), 0, cl.right, cl.bottom - statusH()}; }
    RECT RowRect(int i, const RECT& cl) const {
        (void)cl; int top = S(60, dpi_), rowH = S(38, dpi_), pad = S(8, dpi_);
        return {pad, top + i * rowH, sidebarW() - pad, top + (i + 1) * rowH};
    }

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
    bool elevated_ = false, busy_ = false;
    std::wstring caption_ = L"Hadron", status_;
    sys::SettingsStore store_;
    std::vector<sys::Settings> prefs_;
    Fonts fonts_;
};

const wchar_t* kFrameClass = L"Hadron.Frame";

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
    InvalidateChrome();
}

void Host::PaintChrome(HDC dc, const RECT& cl) {
    const Theme& t = Baseline();
    FillSolid(dc, cl, t.bg);
    if (suite_) {
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
    TextLeft(dc, s, sr, fonts_.caption, t.text2);
    if (busy_) {
        RECT br = st; br.right -= S(12, dpi_);
        SelectObject(dc, fonts_.caption); SetTextColor(dc, t.accent); SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Working…", -1, &br, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
}

LRESULT Host::OnFrame(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        LayoutContent();
        InvalidateChrome();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w);
        fonts_.Build(dpi_);
        RECT* sug = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (active_ >= 0) modules_[active_]->OnDpiChanged(dpi_);
        LayoutContent();
        InvalidateChrome();
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
    case WM_LBUTTONDOWN:
        if (suite_) {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT cl; GetClientRect(h, &cl);
            for (int i = 0; i < (int)modules_.size(); i++) {
                RECT r = RowRect(i, cl);
                if (PtInRect(&r, p)) { Activate(i); break; }
            }
        }
        return 0;
    case WM_CLOSE:
        for (auto& mod : modules_) if (!mod->CanClose()) return 0;   // a plan is executing
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
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
    fonts_.Build(dpi_);
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
        FillSolid(mem, local, Baseline().bg);
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
    elevated_ = sys::IsElevated();

    if (cl.selftest) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        int init = PickInitial(cl);
        if (init < 0) return 1;
        wchar_t a0[] = L"--selftest";
        wchar_t* argv[] = {a0};
        return modules_[init]->RunCli(1, argv);
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
    fonts_.Build(dpi_);
    int cw = S(1200, dpi_), ch = S(800, dpi_);
    RECT wa{0, 0, 0, 0}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - cw) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - ch) / 2;
    frame_ = CreateWindowExW(0, kFrameClass, caption_.c_str(), WS_OVERLAPPEDWINDOW,
                             x, y, cw, ch, nullptr, nullptr, wc.hInstance, this);
    if (!frame_) return 2;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(frame_, HX_DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    Activate(PickInitial(cl));
    ShowWindow(frame_, SW_SHOW);
    UpdateWindow(frame_);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (active_ >= 0 && modules_[active_]->PreTranslateMessage(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    store_.Save();
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
