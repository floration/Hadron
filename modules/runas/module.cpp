// modules/runas/module.cpp — Run As: launch a file as a chosen identity and integrity level.
//
// Like diskmap, this module registers its own content window class ("Hadron.runas.View") instead
// of core's CreateModuleContent, because that helper does not forward the worker-done message, the
// editing keys or WM_CHAR. Paint still goes through Module::Paint and input through OnInput, so the
// module contract's shape holds (reported as the same core gap diskmap reported).
//
// All launches run on a core/sys Worker and post WM_RUNAS_DONE to the view; the only thing on the
// UI thread is the CredUI password prompt (which must not run from a worker). The UI shows the
// *effective* identity read back from the child token, never what was requested.
#include "core/host/run.h"
#include "core/host/registry.h"
#include "core/host/cmdline.h"
#include "core/sys/sys.h"
#include "core/sys/worker.h"
#include "core/sys/settings.h"
#include "core/ui/draw.h"
#include "core/ui/widgets.h"
#include "modules/runas/launch.h"

#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <commdlg.h>
#include <lm.h>          // UF_NORMAL_ACCOUNT / UF_ACCOUNTDISABLE for the sample/selftest accounts
#include <wincred.h>
#include <algorithm>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// MinGW's wincred.h ships CredPackAuthenticationBufferW but not this flag (SDK gap; hard-rule-3
// safe — it is a documented Windows constant, value fixed since Vista).
#ifndef CRED_PACK_GENERIC_CREDENTIALS
#define CRED_PACK_GENERIC_CREDENTIALS 0x00000004
#endif

namespace hx {
using namespace hx::ui;
using namespace hx::runas;

namespace {

constexpr UINT WM_RUNAS_DONE = WM_MODULE_BASE + 1;   // worker -> view (never WM_APP+1)
const wchar_t* const kViewClass = L"Hadron.runas.View";
constexpr size_t kMaxRuns = 50;

// Click/focus IDs.
enum {
    ID_NONE = 0,
    ID_BROWSE, ID_RUN, ID_ALLOW, ID_MODE, ID_WHO, ID_ACCOUNT, ID_TOOL,
};

// Where keyboard/typing focus sits.
enum class Focus { None, Path, Args, Work, Click, List };

// A minimal single-line edit painted like core/ui SearchBox, but with its own placeholder.
struct Edit {
    std::wstring text;
    size_t caret = 0;
    std::wstring placeholder;

    void Set(const std::wstring& s) { text = s; caret = text.size(); }
    bool OnChar(wchar_t ch) {
        if (ch == 0x08) {   // backspace
            if (caret > 0) { text.erase(caret - 1, 1); caret--; return true; }
            return false;
        }
        if (ch < 0x20) return false;
        text.insert(text.begin() + caret, ch); caret++;
        return true;
    }
    bool OnKey(WPARAM vk) {
        switch (vk) {
            case VK_DELETE: if (caret < text.size()) { text.erase(caret, 1); return true; } return false;
            case VK_LEFT:   if (caret > 0) { caret--; return true; } return false;
            case VK_RIGHT:  if (caret < text.size()) { caret++; return true; } return false;
            case VK_HOME:   if (caret != 0) { caret = 0; return true; } return false;
            case VK_END:    if (caret != text.size()) { caret = text.size(); return true; } return false;
        }
        return false;
    }
    void Paint(HDC dc, const RECT& rc, const PaintCtx& c, bool focused) const {
        RoundFill(dc, rc, S(6, c.dpi), c.theme.panel2, focused ? c.theme.accent : c.theme.border);
        int pad = S(10, c.dpi);
        RECT tr{rc.left + pad, rc.top, rc.right - pad, rc.bottom};
        if (text.empty()) {
            TextLeft(dc, placeholder, tr, c.fonts.ui, c.theme.text3);
        } else {
            TextLeft(dc, text, tr, c.fonts.ui, c.theme.text1);
        }
        if (focused && !text.empty()) {
            SelectObject(dc, c.fonts.ui);
            SIZE sz{0, 0};
            std::wstring upto = text.substr(0, caret);
            GetTextExtentPoint32W(dc, upto.c_str(), (int)upto.size(), &sz);
            VLine(dc, rc.left + pad + sz.cx, rc.top + S(6, c.dpi), rc.bottom - S(6, c.dpi), c.theme.accent);
        } else if (focused) {
            VLine(dc, rc.left + pad, rc.top + S(6, c.dpi), rc.bottom - S(6, c.dpi), c.theme.accent);
        }
    }
};

struct Tool { const wchar_t* label; std::wstring target; };
struct Click { RECT rc; int id; int data; };

// Two colour helpers Run As needs (diskmap keeps the same two locally). Declared here, defined
// below, so Button() can use them.
COLORREF Mix(COLORREF a, COLORREF b, double k);
int Lum(COLORREF c);

std::wstring WhenText(long long epoch) {
    if (epoch <= 0) return L"";
    time_t t = (time_t)epoch;
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t b[32];
    wcsftime(b, 32, L"%Y-%m-%d %H:%M", &lt);
    return b;
}

// ============================================================================================
class RunAsModule : public Module {
public:
    const ModuleInfo& Info() const override {
        static const ModuleInfo info{L"runas", L"Run As",
                                     L"Launch a file as this admin, another user, SYSTEM or "
                                     L"TrustedInstaller, full or limited token. Last-run list and "
                                     L"Windows-tool buttons.",
                                     Admin::Required};
        return info;
    }
    HWND Create(IHost&, HWND parent, const RECT& rc) override;
    void OnActivate() override { if (hwnd_) SetFocus(hwnd_); }
    void OnDpiChanged(UINT) override { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    bool OnInput(UINT msg, WPARAM w, LPARAM l) override;
    bool CanClose() override { return !worker_.Running(); }
    void Shutdown() override {
        worker_.Cancel();
        worker_.Join();
        ZeroPassword();
    }
    int  RunCli(int argc, wchar_t** argv) override;
    void LoadFixture(const wchar_t* dir) override { (void)dir; LoadDemo(); }
    void Paint(HDC dc, const RECT& rc) override;

private:
    static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnView(HWND, UINT, WPARAM, LPARAM);

    PaintCtx Ctx() const { return PaintCtx{host_->Theme(), host_->Fonts(), host_->Dpi(), host_->Layout()}; }
    int  Sc(int px) const { return S(px, host_ ? host_->Dpi() : 96); }
    bool Console() const { return host_ && host_->Layout() == Layout::Console; }
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void AddClick(const RECT& r, int id, int data = 0) { clicks_.push_back({r, id, data}); }
    bool IsHover(int id, int data = 0) const { return hoverId_ == id && hoverData_ == data; }

    void BuildTools();
    void LoadDemo();
    void LoadRuns();
    void SaveRuns();
    void RememberRun(const LaunchResult& res);
    void RefillList();
    void SortRuns();
    bool Gate(Who w) {   // rule-4 gate; says why on refusal
        if (LaunchAllowed(w, allowElevated_)) return true;
        resultLine_ = L"Turn on \"Allow SYSTEM / TrustedInstaller\" first.";
        resultBad_ = true;
        Invalidate();
        return false;
    }

    void Button(HDC dc, const RECT& r, const std::wstring& label, int id, int data, bool primary,
                bool enabled, bool selected);
    void DrawForm(HDC dc, const RECT& rc);
    void DrawFocusRing(HDC dc);

    void OnClickId(int id, int data);
    void DoRun(const LaunchRequest& req);
    void RunTarget();
    void RunTool(int idx);
    bool PromptPassword(const std::wstring& account, std::wstring* user, std::wstring* pwd);
    void Browse();
    void ZeroPassword() {
        if (!pwd_.empty()) { SecureZeroMemory(&pwd_[0], pwd_.size() * sizeof(wchar_t)); pwd_.clear(); }
    }
    void CycleFocusStops(bool back);

    bool SystemAllowed() const { return allowElevated_; }
    bool WhoEnabled(Who w) const {
        return (w == Who::System || w == Who::TrustedInstaller) ? allowElevated_ : true;
    }

    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;
    bool demo_ = false;

    Edit path_, args_, work_;
    Who who_ = Who::ThisAdmin;
    TokenMode mode_ = TokenMode::Full;
    bool allowElevated_ = false;          // OFF at every start, never persisted (rule 4)
    std::vector<Account> accounts_;
    int accountIdx_ = 0;

    std::vector<Tool> tools_;
    std::vector<LastRun> runs_;
    ListView list_;

    sys::Worker worker_;
    LaunchRequest req_;
    LaunchResult result_;
    std::wstring pwd_;                     // live only across one launch; zeroed after
    bool busyTool_ = false;
    std::wstring resultLine_;
    bool resultBad_ = false;

    std::vector<Click> clicks_;
    int hoverId_ = -1, hoverData_ = 0;
    Focus focus_ = Focus::Path;
    int focusId_ = -1, focusData_ = 0;

    RECT rcPath_{}, rcArgs_{}, rcWork_{}, rcList_{};
};

// ============================================================================================
// Window

LRESULT CALLBACK RunAsModule::ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = reinterpret_cast<RunAsModule*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->OnView(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT RunAsModule::OnView(HWND h, UINT m, WPARAM w, LPARAM l) {
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
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, hoverId_ > 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_RUNAS_DONE:
        worker_.Join();                 // synchronise with the worker before reading result_
        ZeroPassword();
        busyTool_ = false;
        host_->SetBusy(false);
        if (result_.ok) {
            RememberRun(result_);
            std::wstring s = L"Running as " + result_.effectiveUser + L"  •  " +
                             result_.integrityName + L" integrity  •  session " +
                             std::to_wstring(result_.sessionId) + L"  •  PID " +
                             std::to_wstring(result_.pid);
            if (result_.downgraded)
                s += L"   (requested " + result_.requestedLabel + L"; the OS returned the above)";
            if (result_.startedService) s += L"   (started the TrustedInstaller service)";
            resultLine_ = s;
            resultBad_ = false;
        } else {
            resultLine_ = L"Launch failed: " + result_.error;
            resultBad_ = true;
        }
        host_->SetStatus(resultLine_.c_str());
        Invalidate();
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        [[fallthrough]];
    case WM_LBUTTONUP: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_CHAR:
        if (OnInput(m, w, l)) Invalidate();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND RunAsModule::Create(IHost& host, HWND parent, const RECT& rc) {
    host_ = &host;
    path_.placeholder = L"Program, document, shortcut or .msc — type a path or use Browse";
    args_.placeholder = L"Arguments (optional)";
    work_.placeholder = L"Working folder (optional; defaults to the target's folder)";
    BuildTools();

    list_.SetColumns({{L"Path", Sc(360)}, {L"Identity", Sc(150)}, {L"Integrity", Sc(90)},
                      {L"When", Sc(130)}});
    list_.onActivate = [this](int i) {
        if (i < 0 || i >= (int)runs_.size()) return;
        const LastRun& r = runs_[i];
        path_.Set(r.path); args_.Set(r.args); work_.Set(L"");
        who_ = (Who)r.who; mode_ = (TokenMode)r.mode;
        if (who_ == Who::AnotherUser && !r.account.empty()) {
            for (size_t k = 0; k < accounts_.size(); k++)
                if (accounts_[k].name == r.account) { accountIdx_ = (int)k; break; }
        }
        resultLine_ = L"Re-filled from history — review, then Run.";
        resultBad_ = false;
        // A persisted SYSTEM/TI row must never bypass the off-by-default switch (rule 4).
        if (!LaunchAllowed(who_, allowElevated_)) {
            who_ = Who::ThisAdmin;
            resultLine_ = L"Re-filled from history. SYSTEM/TrustedInstaller needs the switch; set to This admin.";
        }
        Invalidate();
    };
    list_.onSortChanged = [this](int, bool) { SortRuns(); RefillList(); Invalidate(); };

    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    demo_ = !cl.screenshot.empty();
    if (demo_) {
        LoadDemo();
    } else {
        accounts_ = EnumerateLocalAccounts();
        LoadRuns();
    }
    RefillList();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &RunAsModule::ViewProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kViewClass;
        if (!RegisterClassExW(&wc)) {
            host.Log(LogLevel::Error, (L"runas: RegisterClassEx failed: " +
                                       sys::ErrorText(GetLastError())).c_str());
            return nullptr;
        }
        registered = true;
    }
    hwnd_ = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, parent, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        host.Log(LogLevel::Error, (L"runas: CreateWindowEx failed: " +
                                   sys::ErrorText(GetLastError())).c_str());
        return nullptr;
    }
    return hwnd_;
}

// ============================================================================================
// Data

void RunAsModule::BuildTools() {
    wchar_t win[MAX_PATH] = {}, sys[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring w = win, s = sys;
    tools_ = {
        {L"Registry Editor",     w + L"\\regedit.exe"},
        {L"Services",            s + L"\\services.msc"},
        {L"Task Manager",        s + L"\\taskmgr.exe"},
        {L"Device Manager",      s + L"\\devmgmt.msc"},
        {L"Disk Management",     s + L"\\diskmgmt.msc"},
        {L"Event Viewer",        s + L"\\eventvwr.msc"},
        {L"Computer Management", s + L"\\compmgmt.msc"},
        {L"Command Prompt",      s + L"\\cmd.exe"},
        {L"PowerShell",          s + L"\\WindowsPowerShell\\v1.0\\powershell.exe"},
    };
}

void RunAsModule::LoadDemo() {
    accounts_ = {{L"alice", UF_NORMAL_ACCOUNT}, {L"buildbot", UF_NORMAL_ACCOUNT}};
    accountIdx_ = 0;
    runs_ = {
        {L"C:\\Windows\\System32\\cmd.exe", L"/k whoami /groups", L"", (int)Who::ThisAdmin,
         (int)TokenMode::Full, L"High", 1762860000},
        {L"C:\\Tools\\patcher.exe", L"--repair", L"", (int)Who::TrustedInstaller,
         (int)TokenMode::Full, L"System", 1762846800},
        {L"D:\\games, demos\\launch.lnk", L"", L"", (int)Who::AnotherUser, (int)TokenMode::Full,
         L"Medium", 1762770600},
        {L"C:\\Windows\\regedit.exe", L"", L"", (int)Who::System, (int)TokenMode::Limited,
         L"Medium", 1762686000},
    };
    runs_[2].account = L"alice";
    RefillList();
}

void RunAsModule::LoadRuns() {
    runs_.clear();
    sys::Settings& p = host_->Prefs();
    int n = p.GetInt(L"runcount", 0);
    if (n > (int)kMaxRuns) n = (int)kMaxRuns;
    for (int i = 0; i < n; i++) {
        std::wstring line = p.GetStr((L"run" + std::to_wstring(i)).c_str(), L"");
        LastRun r;
        if (ParseLastRun(line, &r)) runs_.push_back(r);
    }
}

void RunAsModule::SaveRuns() {
    if (demo_) return;
    sys::Settings& p = host_->Prefs();
    int n = (int)std::min(runs_.size(), kMaxRuns);
    p.SetInt(L"runcount", n);
    for (int i = 0; i < n; i++)
        p.SetStr((L"run" + std::to_wstring(i)).c_str(), SerializeLastRun(runs_[i]));
    // The store is flushed by the host on exit; no password is ever written (SerializeLastRun has
    // no password field).
}

void RunAsModule::RememberRun(const LaunchResult& res) {
    LastRun r;
    r.path = req_.path;
    r.args = req_.args;
    r.account = req_.account;
    r.who = (int)req_.who;
    r.mode = (int)req_.mode;
    r.integrity = res.integrityName;
    r.when = (long long)time(nullptr);
    // Persistence and the 50-entry cap are newest-first, whatever column the view is sorted by.
    std::stable_sort(runs_.begin(), runs_.end(),
                     [](const LastRun& a, const LastRun& b) { return a.when > b.when; });
    // De-dup: drop any earlier identical target+identity, newest first.
    for (size_t i = 0; i < runs_.size(); ) {
        if (runs_[i].path == r.path && runs_[i].who == r.who && runs_[i].mode == r.mode &&
            runs_[i].account == r.account && runs_[i].args == r.args)
            runs_.erase(runs_.begin() + i);
        else ++i;
    }
    runs_.insert(runs_.begin(), r);
    if (runs_.size() > kMaxRuns) runs_.resize(kMaxRuns);
    SaveRuns();
    SortRuns();   // keep the user's chosen column order; persistence stays newest-first above
    RefillList();
}

int IntegrityRank(const std::wstring& s) {
    static const wchar_t* const order[] = {L"Untrusted", L"Low", L"Medium", L"Medium Plus", L"High",
                                           L"System", L"Protected"};
    for (int i = 0; i < 7; i++) if (s == order[i]) return i;
    return -1;
}

// Sort runs_ itself (stable) so a list row index still maps to runs_[i] in onActivate.
void RunAsModule::SortRuns() {
    int col = list_.SortColumn();
    if (col < 0) return;
    bool asc = list_.SortAscending();
    std::stable_sort(runs_.begin(), runs_.end(), [&](const LastRun& a, const LastRun& b) {
        int c = 0;
        switch (col) {
            case 0: c = _wcsicmp(a.path.c_str(), b.path.c_str()); break;
            case 1: c = _wcsicmp(IdentityLabel(a.who, a.mode, a.account).c_str(),
                                 IdentityLabel(b.who, b.mode, b.account).c_str()); break;
            case 2: c = IntegrityRank(a.integrity) - IntegrityRank(b.integrity); break;
            default: c = (a.when < b.when) ? -1 : (a.when > b.when ? 1 : 0); break;   // by timestamp
        }
        return asc ? c < 0 : c > 0;
    });
}

void RunAsModule::RefillList() {
    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(runs_.size());
    for (const LastRun& r : runs_) {
        rows.push_back({r.path, IdentityLabel(r.who, r.mode, r.account),
                        r.integrity.empty() ? L"—" : r.integrity, WhenText(r.when)});
    }
    list_.SetRows(std::move(rows));
}

// ============================================================================================
// Painting

void RunAsModule::Button(HDC dc, const RECT& r, const std::wstring& label, int id, int data,
                         bool primary, bool enabled, bool selected) {
    const Theme& t = host_->Theme();
    bool hv = enabled && IsHover(id, data);
    COLORREF bg;
    if (primary) bg = hv ? Mix(t.accent, RGB(255, 255, 255), 0.12) : t.accent;
    else if (selected) bg = Mix(t.panel2, t.accent, 0.22);
    else bg = hv ? t.panel3 : t.panel2;
    RoundFill(dc, r, Sc(8), bg);
    COLORREF bc = selected ? t.accent : (hv ? Mix(t.border, t.text3, 0.5) : t.border);
    if (!primary) RoundFill(dc, r, Sc(8), bg, bc);
    COLORREF tc = primary ? (Lum(t.accent) > 140 ? RGB(10, 14, 22) : RGB(245, 247, 250))
                          : (enabled ? t.text1 : t.text3);
    RECT tr{r.left + Sc(6), r.top, r.right - Sc(6), r.bottom};
    DrawText(dc, label, tr, host_->Fonts().semi, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    if (enabled) AddClick(r, id, data);
}

COLORREF Mix(COLORREF a, COLORREF b, double k) {
    auto mix = [&](int s, int d) { return (int)(s + (d - s) * k + 0.5); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}
int Lum(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }

void RunAsModule::DrawForm(HDC dc, const RECT& rc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    clicks_.clear();
    int P = Sc(20);
    int L = rc.left + P, R = rc.right - P;
    int y = rc.top + P;
    int fieldH = Sc(32), lblH = Sc(18), gap = Sc(10);

    auto label = [&](const wchar_t* s) {
        RECT lr{L, y, R, y + lblH};
        DrawText(dc, s, lr, f.caption, t.text3, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        y += lblH;
    };

    // --- Title line -------------------------------------------------------------------------
    {
        RECT hr{L, y, R, y + Sc(26)};
        DrawText(dc, L"Run a program as a chosen identity", hr, f.title, t.text1,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        y += Sc(34);
    }

    // --- Target -----------------------------------------------------------------------------
    label(L"Program or file");
    int browseW = Sc(96);
    rcPath_ = {L, y, R - browseW - gap, y + fieldH};
    path_.Paint(dc, rcPath_, Ctx(), focus_ == Focus::Path);
    RECT br{R - browseW, y, R, y + fieldH};
    Button(dc, br, L"Browse…", ID_BROWSE, 0, false, true, false);
    y += fieldH + gap;

    label(L"Arguments");
    rcArgs_ = {L, y, R, y + fieldH};
    args_.Paint(dc, rcArgs_, Ctx(), focus_ == Focus::Args);
    y += fieldH + gap;

    label(L"Working folder");
    rcWork_ = {L, y, R, y + fieldH};
    work_.Paint(dc, rcWork_, Ctx(), focus_ == Focus::Work);
    y += fieldH + Sc(16);

    // --- Identity ---------------------------------------------------------------------------
    label(L"Run as");
    int who4w = (R - L - 3 * gap) / 4;
    const Who whoOrder[4] = {Who::ThisAdmin, Who::AnotherUser, Who::System, Who::TrustedInstaller};
    for (int i = 0; i < 4; i++) {
        RECT r{L + i * (who4w + gap), y, L + i * (who4w + gap) + who4w, y + fieldH};
        Button(dc, r, WhoName(whoOrder[i]), ID_WHO, (int)whoOrder[i], false,
               WhoEnabled(whoOrder[i]), who_ == whoOrder[i]);
    }
    y += fieldH + gap;

    // Full / Limited toggle + (for Another user) the account selector, on one row.
    int half = (R - L - gap) / 2;
    {
        int segW = half / 2;
        RECT rf{L, y, L + segW, y + fieldH};
        RECT rl{L + segW, y, L + half, y + fieldH};
        Button(dc, rf, L"Full token", ID_MODE, (int)TokenMode::Full, false, true,
               mode_ == TokenMode::Full);
        Button(dc, rl, L"Limited token", ID_MODE, (int)TokenMode::Limited, false, true,
               mode_ == TokenMode::Limited);
    }
    if (who_ == Who::AnotherUser) {
        RECT ar{L + half + gap, y, R, y + fieldH};
        std::wstring name = accounts_.empty() ? std::wstring(L"(no local accounts)")
                                              : (L"Account:  " + accounts_[accountIdx_].name +
                                                 L"   (click to change)");
        Button(dc, ar, name, ID_ACCOUNT, 0, false, !accounts_.empty(), false);
    }
    y += fieldH + Sc(14);

    // --- Allow SYSTEM / TrustedInstaller switch --------------------------------------------
    {
        int boxH = Sc(26);
        RECT sw{L, y, L + Sc(300), y + boxH};
        bool hv = IsHover(ID_ALLOW);
        RoundFill(dc, sw, Sc(6), hv ? t.panel3 : t.panel2, allowElevated_ ? t.warn : t.border);
        int bs = Sc(14), bx = sw.left + Sc(9), by = (sw.top + sw.bottom) / 2 - bs / 2;
        RECT box{bx, by, bx + bs, by + bs};
        if (allowElevated_) {
            FillSolid(dc, box, t.warn);
            RECT in{box.left + Sc(3), box.top + Sc(3), box.right - Sc(3), box.bottom - Sc(3)};
            FillSolid(dc, in, t.panel);
        } else {
            FillSolid(dc, box, t.border);
            RECT in{box.left + 1, box.top + 1, box.right - 1, box.bottom - 1};
            FillSolid(dc, in, t.panel);
        }
        RECT lt{box.right + Sc(8), sw.top, sw.right - Sc(6), sw.bottom};
        DrawText(dc, L"Allow SYSTEM / TrustedInstaller", lt, f.semi, t.text1,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        AddClick(sw, ID_ALLOW);
        RECT warn{sw.right + Sc(12), y, R, y + boxH};
        DrawText(dc, L"Runs with more power than an administrator; can damage Windows.", warn,
                 f.caption, t.warn, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }
    y += Sc(26) + Sc(16);

    // --- Run button -------------------------------------------------------------------------
    {
        RECT run{L, y, L + Sc(180), y + Sc(38)};
        Button(dc, run, worker_.Running() ? L"Running…" : L"Run",
               ID_RUN, 0, true, !worker_.Running(), false);
        y += Sc(38) + gap;
    }

    // --- Result line ------------------------------------------------------------------------
    if (!resultLine_.empty()) {
        RECT res{L, y, R, y + Sc(22)};
        COLORREF c = resultBad_ ? t.bad : t.good;
        DrawText(dc, resultLine_, res, f.ui, c, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }
    y += Sc(28);

    // --- Windows tools strip (bottom) -------------------------------------------------------
    int toolH = Sc(30), toolMinW = Sc(150);
    int toolRows = 0;
    {
        int tx = L, ty = 0;
        // Measure rows to pin the strip to the bottom.
        for (size_t i = 0; i < tools_.size(); i++) {
            if (tx + toolMinW > R) { tx = L; toolRows++; }
            tx += toolMinW + gap;
        }
        toolRows++;
        ty = rc.bottom - P - toolRows * (toolH + gap) + gap;
        int stripTop = ty - Sc(26);
        RECT sl{L, stripTop, R, stripTop + Sc(18)};
        DrawText(dc, L"Windows tools — launched at the selected identity", sl, f.caption, t.text3,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        tx = L;
        for (size_t i = 0; i < tools_.size(); i++) {
            if (tx + toolMinW > R) { tx = L; ty += toolH + gap; }
            RECT r{tx, ty, tx + toolMinW, ty + toolH};
            Button(dc, r, tools_[i].label, ID_TOOL, (int)i, false, !worker_.Running(), false);
            tx += toolMinW + gap;
        }
        // The last-run list fills the gap between the result line and the tools strip.
        rcList_ = {L, y, R, stripTop - gap};
        if (rcList_.bottom < rcList_.top + Sc(60)) rcList_.bottom = rcList_.top + Sc(60);
    }

    // Recent header + list.
    RECT lh{L, rcList_.top, R, rcList_.top + Sc(18)};
    DrawText(dc, L"Recent launches — double-click or Enter to re-fill the form (does not auto-run)",
             lh, f.caption, t.text3, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    RECT listBody{L, rcList_.top + Sc(22), R, rcList_.bottom};
    rcList_ = listBody;
    FillSolid(dc, rcList_, t.panel);
    FrameRectC(dc, rcList_, t.border);
    list_.Paint(dc, rcList_, Ctx(), focus_ == Focus::List);
}

void RunAsModule::DrawFocusRing(HDC dc) {
    const Theme& t = host_->Theme();
    if (focus_ == Focus::Click) {
        for (const Click& c : clicks_)
            if (c.id == focusId_ && c.data == focusData_) {
                RECT r = c.rc; InflateRect(&r, Sc(2), Sc(2));
                FocusRectC(dc, r, t.accent);
                break;
            }
    }
}

void RunAsModule::Paint(HDC dc, const RECT& rc) {
    FillSolid(dc, rc, host_->Theme().bg);
    DrawForm(dc, rc);
    DrawFocusRing(dc);
}

// ============================================================================================
// Actions

void RunAsModule::Browse() {
    std::vector<wchar_t> file(2048, 0);
    const std::wstring& init = path_.text;
    size_t n = std::min(init.size(), (size_t)2046);
    std::copy(init.begin(), init.begin() + n, file.begin());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host_->Frame();
    ofn.lpstrFilter = L"All files\0*.*\0Programs\0*.exe;*.com;*.bat;*.cmd\0Shortcuts\0*.lnk\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = 2048;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        path_.Set(file.data());
        focus_ = Focus::Path;
        Invalidate();
    }
}

bool RunAsModule::PromptPassword(const std::wstring& account, std::wstring* user, std::wstring* pwd) {
    // Pre-fill the username with the chosen account.
    void* inBuf = nullptr;
    ULONG inSize = 0;
    CredPackAuthenticationBufferW(CRED_PACK_GENERIC_CREDENTIALS, const_cast<wchar_t*>(account.c_str()),
                                  const_cast<wchar_t*>(L""), nullptr, &inSize);
    std::vector<BYTE> inStore;
    if (inSize) {
        inStore.resize(inSize);
        inBuf = inStore.data();
        if (!CredPackAuthenticationBufferW(CRED_PACK_GENERIC_CREDENTIALS,
                                           const_cast<wchar_t*>(account.c_str()),
                                           const_cast<wchar_t*>(L""), inStore.data(), &inSize)) {
            inBuf = nullptr;
        }
    }

    std::wstring msg = L"Enter the password for " + account + L" (leave blank if it has none).";
    CREDUI_INFOW info{};
    info.cbSize = sizeof(info);
    info.hwndParent = host_->Frame();
    info.pszMessageText = msg.c_str();
    info.pszCaptionText = L"Run As — another user";

    ULONG authPackage = 0;
    void* outBuf = nullptr;
    ULONG outSize = 0;
    BOOL save = FALSE;
    DWORD res = CredUIPromptForWindowsCredentialsW(&info, 0, &authPackage, inBuf, inSize,
                                                   &outBuf, &outSize, &save, CREDUIWIN_GENERIC);
    if (!inStore.empty()) SecureZeroMemory(inStore.data(), inStore.size());
    if (res != ERROR_SUCCESS) {
        if (outBuf) { SecureZeroMemory(outBuf, outSize); CoTaskMemFree(outBuf); }
        return false;
    }
    wchar_t u[513] = {}, d[257] = {}, p[513] = {};
    DWORD ul = 513, dl = 257, pl = 513;
    bool ok = CredUnPackAuthenticationBufferW(0, outBuf, outSize, u, &ul, d, &dl, p, &pl) != FALSE;
    if (ok) {
        *user = (d[0] && _wcsicmp(d, L".") != 0) ? (std::wstring(d) + L"\\" + u) : std::wstring(u);
        *pwd = p;
    }
    SecureZeroMemory(p, sizeof(p));
    SecureZeroMemory(outBuf, outSize);
    CoTaskMemFree(outBuf);
    return ok;
}

void RunAsModule::DoRun(const LaunchRequest& req) {
    if (worker_.Running()) return;
    if (!Gate(req.who)) { ZeroPassword(); return; }   // defence in depth (rule 4)
    req_ = req;
    resultLine_.clear();
    host_->SetBusy(true);
    host_->SetStatus(L"Launching…");
    const wchar_t* pw = pwd_.empty() ? L"" : pwd_.c_str();
    // Capture the password pointer by copying into a stable member (pwd_) that outlives the worker;
    // it is zeroed in WM_RUNAS_DONE. The worker never opens a dialog. It gets its own COM apartment
    // because ResolveTarget uses IShellLinkW to resolve .lnk targets off the UI thread.
    bool ok = worker_.Start(hwnd_, WM_RUNAS_DONE, [this, pw]() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        Launch(req_, pw, &result_);
        if (SUCCEEDED(hr)) CoUninitialize();
    });
    if (!ok) {
        host_->SetBusy(false);
        ZeroPassword();
        resultLine_ = L"Could not start the launch worker.";
        resultBad_ = true;
        Invalidate();
    }
}

void RunAsModule::RunTarget() {
    if (path_.text.empty()) {
        resultLine_ = L"Choose a program or file first.";
        resultBad_ = true;
        Invalidate();
        return;
    }
    if (!Gate(who_)) return;
    LaunchRequest req;
    req.path = path_.text;
    req.args = args_.text;
    req.workingDir = work_.text;
    req.who = who_;
    req.mode = mode_;
    ZeroPassword();
    if (who_ == Who::AnotherUser) {
        if (accounts_.empty()) {
            resultLine_ = L"No local account to run as.";
            resultBad_ = true;
            Invalidate();
            return;
        }
        std::wstring user, pwd;
        if (!PromptPassword(accounts_[accountIdx_].name, &user, &pwd)) {
            SecureZeroMemory(&pwd[0], pwd.size() * sizeof(wchar_t));
            return;   // cancelled
        }
        req.account = user.empty() ? accounts_[accountIdx_].name : user;
        pwd_ = pwd;
        SecureZeroMemory(&pwd[0], pwd.size() * sizeof(wchar_t));
    }
    DoRun(req);
}

void RunAsModule::RunTool(int idx) {
    if (idx < 0 || idx >= (int)tools_.size() || worker_.Running()) return;
    if (!Gate(who_)) return;
    LaunchRequest req;
    req.path = tools_[idx].target;
    req.who = who_;
    req.mode = mode_;
    ZeroPassword();
    if (who_ == Who::AnotherUser) {
        if (accounts_.empty()) { resultLine_ = L"No local account to run as."; resultBad_ = true; Invalidate(); return; }
        std::wstring user, pwd;
        if (!PromptPassword(accounts_[accountIdx_].name, &user, &pwd)) {
            SecureZeroMemory(&pwd[0], pwd.size() * sizeof(wchar_t));
            return;
        }
        req.account = user.empty() ? accounts_[accountIdx_].name : user;
        pwd_ = pwd;
        SecureZeroMemory(&pwd[0], pwd.size() * sizeof(wchar_t));
    }
    busyTool_ = true;
    DoRun(req);
}

void RunAsModule::OnClickId(int id, int data) {
    switch (id) {
    case ID_BROWSE: Browse(); break;
    case ID_WHO:
        if (WhoEnabled((Who)data)) { who_ = (Who)data; Invalidate(); }
        break;
    case ID_MODE: mode_ = (TokenMode)data; Invalidate(); break;
    case ID_ALLOW:
        allowElevated_ = !allowElevated_;
        if (!allowElevated_ && (who_ == Who::System || who_ == Who::TrustedInstaller))
            who_ = Who::ThisAdmin;
        Invalidate();
        break;
    case ID_ACCOUNT:
        if (!accounts_.empty()) {
            accountIdx_ = (accountIdx_ + 1) % (int)accounts_.size();
            Invalidate();
        }
        break;
    case ID_RUN: RunTarget(); break;
    case ID_TOOL: RunTool(data); break;
    }
}

// ============================================================================================
// Input / focus

void RunAsModule::CycleFocusStops(bool back) {
    struct Stop { Focus f; int id; int data; };
    std::vector<Stop> stops;
    stops.push_back({Focus::Path, 0, 0});
    stops.push_back({Focus::Args, 0, 0});
    stops.push_back({Focus::Work, 0, 0});
    for (const Click& c : clicks_) stops.push_back({Focus::Click, c.id, c.data});
    stops.push_back({Focus::List, 0, 0});
    int cur = -1;
    for (int i = 0; i < (int)stops.size(); i++) {
        const Stop& s = stops[i];
        if (s.f == focus_ && (s.f != Focus::Click || (s.id == focusId_ && s.data == focusData_))) {
            cur = i; break;
        }
    }
    int n = (int)stops.size();
    int nx = cur < 0 ? (back ? n - 1 : 0) : (cur + (back ? n - 1 : 1)) % n;
    focus_ = stops[nx].f; focusId_ = stops[nx].id; focusData_ = stops[nx].data;
}

bool RunAsModule::OnInput(UINT msg, WPARAM w, LPARAM l) {
    if (!host_) return false;
    PaintCtx c = Ctx();
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    POINT p{x, y};
    switch (msg) {
    case WM_MOUSEMOVE: {
        int nid = -1, nd = 0;
        for (int i = (int)clicks_.size() - 1; i >= 0; i--)
            if (PtInRect(&clicks_[i].rc, p)) { nid = clicks_[i].id; nd = clicks_[i].data; break; }
        bool ch = list_.OnMouse(msg, x, y, rcList_, c);
        if (nid != hoverId_ || nd != hoverData_) { hoverId_ = nid; hoverData_ = nd; ch = true; }
        return ch;
    }
    case WM_LBUTTONDOWN: {
        if (PtInRect(&rcPath_, p)) { focus_ = Focus::Path; return true; }
        if (PtInRect(&rcArgs_, p)) { focus_ = Focus::Args; return true; }
        if (PtInRect(&rcWork_, p)) { focus_ = Focus::Work; return true; }
        if (PtInRect(&rcList_, p)) {
            focus_ = Focus::List;
            list_.OnMouse(msg, x, y, rcList_, c);
            return true;
        }
        for (int i = (int)clicks_.size() - 1; i >= 0; i--)
            if (PtInRect(&clicks_[i].rc, p)) {
                focus_ = Focus::Click; focusId_ = clicks_[i].id; focusData_ = clicks_[i].data;
                OnClickId(clicks_[i].id, clicks_[i].data);
                return true;
            }
        return false;
    }
    case WM_LBUTTONUP:
        return list_.OnMouse(msg, x, y, rcList_, c);
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        POINT sp{x, y}; ScreenToClient(hwnd_, &sp);
        if (PtInRect(&rcList_, sp)) return list_.OnWheel(delta, rcList_, c);
        return false;
    }
    case WM_CHAR: {
        wchar_t ch = (wchar_t)w;
        if (focus_ == Focus::Path) return path_.OnChar(ch);
        if (focus_ == Focus::Args) return args_.OnChar(ch);
        if (focus_ == Focus::Work) return work_.OnChar(ch);
        return false;
    }
    case WM_KEYDOWN: {
        WPARAM vk = w;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (vk == VK_TAB) { CycleFocusStops(shift); return true; }
        if (focus_ == Focus::Path) { if (vk == VK_RETURN) { RunTarget(); return true; } return path_.OnKey(vk); }
        if (focus_ == Focus::Args) { if (vk == VK_RETURN) { RunTarget(); return true; } return args_.OnKey(vk); }
        if (focus_ == Focus::Work) { if (vk == VK_RETURN) { RunTarget(); return true; } return work_.OnKey(vk); }
        if (focus_ == Focus::List) {
            if (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
                vk == VK_HOME || vk == VK_END || vk == VK_RETURN) {
                list_.OnKey(vk, rcList_, c);
                return true;
            }
            return false;
        }
        if (focus_ == Focus::Click && (vk == VK_RETURN || vk == VK_SPACE)) {
            OnClickId(focusId_, focusData_);
            return true;
        }
        return false;
    }
    }
    return false;
}

// ============================================================================================
// Headless selftest

int RunAsModule::RunCli(int argc, wchar_t** argv) {
    bool selftest = false;
    std::wstring launchMode;
    for (int i = 0; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--selftest") selftest = true;
        else if (a == L"--launch-test" && i + 1 < argc) launchMode = argv[i + 1];
    }
    if (!launchMode.empty()) return RunLaunchTest(launchMode);
    if (!selftest) return -1;

    int fails = 0, checks = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        checks++; if (!ok) fails++;
        wprintf(L"  [%ls] %ls\n", ok ? L"PASS" : L"FAIL", what.c_str());
    };
    wprintf(L"runas selftest\n");

    // --- argument quoting: round-trip through CommandLineToArgvW -----------------------------
    {
        const wchar_t* cases[] = {
            L"plain", L"has space", L"quote\"inside", L"trail\\", L"a\\\\b",
            L"back\\\"slash-quote", L"", L"tab\there", L"c:\\path with space\\app.exe",
        };
        for (const wchar_t* a : cases) {
            std::wstring cmd = L"prog.exe " + QuoteArg(a);
            int n = 0;
            LPWSTR* out = CommandLineToArgvW(cmd.c_str(), &n);
            bool ok = (n == 2) && (std::wstring(out[1]) == a);
            if (out) LocalFree(out);
            check(ok, L"QuoteArg round-trips [" + std::wstring(a) + L"]");
        }
    }

    // --- path absolutisation ----------------------------------------------------------------
    check(AbsolutePath(L"C:\\Windows\\notepad.exe") == L"C:\\Windows\\notepad.exe",
          L"AbsolutePath leaves an absolute path unchanged");
    {
        std::wstring abs = AbsolutePath(L"sub\\x.exe", L"C:\\base");
        check(abs == L"C:\\base\\sub\\x.exe", L"AbsolutePath joins a relative path onto base (" + abs + L")");
    }

    // --- .msc -> mmc command (parse it back: argv[0] = mmc, argv[1] = the .msc file) ----------
    {
        std::wstring cmd = MscCommand(L"C:\\Program Files\\MMC", L"D:\\snap ins\\services.msc");
        int n = 0;
        LPWSTR* out = CommandLineToArgvW(cmd.c_str(), &n);
        bool ok = out && n == 2 &&
                  std::wstring(out[0]) == L"C:\\Program Files\\MMC\\mmc.exe" &&
                  std::wstring(out[1]) == L"D:\\snap ins\\services.msc";
        if (out) LocalFree(out);
        check(ok, L"MscCommand builds a parseable mmc command line (quotes paths with spaces)");
    }

    // --- integrity RID -> name --------------------------------------------------------------
    check(IntegrityName(0x1000) == L"Low", L"IntegrityName(0x1000) = Low");
    check(IntegrityName(0x2000) == L"Medium", L"IntegrityName(0x2000) = Medium");
    check(IntegrityName(0x3000) == L"High", L"IntegrityName(0x3000) = High");
    check(IntegrityName(0x4000) == L"System", L"IntegrityName(0x4000) = System");
    check(IntegrityName(0x0000) == L"Untrusted", L"IntegrityName(0x0000) = Untrusted");

    // --- privilege helper reports ERROR_NOT_ALL_ASSIGNED as not-enabled ----------------------
    check(!PrivilegeEnabled(TRUE, ERROR_NOT_ALL_ASSIGNED), L"PrivilegeEnabled(TRUE, NOT_ALL_ASSIGNED) = false");
    check(PrivilegeEnabled(TRUE, ERROR_SUCCESS), L"PrivilegeEnabled(TRUE, SUCCESS) = true");
    check(!PrivilegeEnabled(FALSE, ERROR_SUCCESS), L"PrivilegeEnabled(FALSE, SUCCESS) = false");

    // --- account filter drops disabled and service accounts ---------------------------------
    {
        std::vector<Account> all = {
            {L"alice", UF_NORMAL_ACCOUNT},
            {L"disabled", UF_NORMAL_ACCOUNT | UF_ACCOUNTDISABLE},
            {L"Guest", UF_NORMAL_ACCOUNT},
            {L"DefaultAccount", UF_NORMAL_ACCOUNT},
            {L"WDAGUtilityAccount", UF_NORMAL_ACCOUNT},
            {L"MACHINE$", UF_WORKSTATION_TRUST_ACCOUNT},
            {L"bob", UF_NORMAL_ACCOUNT},
        };
        std::vector<Account> kept = FilterAccounts(all);
        bool ok = kept.size() == 2 && kept[0].name == L"alice" && kept[1].name == L"bob";
        check(ok, L"FilterAccounts keeps only alice and bob");
    }

    // --- last-run serialise/parse round-trip (commas, Unicode; no password field) -----------
    {
        LastRun r;
        r.path = L"D:\\games, demos\\naïve\\läuncher.exe";
        r.args = L"--flag \"a, b\"  café=1";
        r.account = L"αlice";
        r.who = (int)Who::TrustedInstaller;
        r.mode = (int)TokenMode::Limited;
        r.integrity = L"System";
        r.when = 1762860000;
        std::wstring line = SerializeLastRun(r);
        check(line.find(L'\n') == std::wstring::npos, L"SerializeLastRun stays on one line");
        LastRun r2;
        bool parsed = ParseLastRun(line, &r2);
        bool same = parsed && r2.path == r.path && r2.args == r.args && r2.account == r.account &&
                    r2.who == r.who && r2.mode == r.mode && r2.integrity == r.integrity &&
                    r2.when == r.when;
        check(same, L"ParseLastRun round-trips path/args/account/who/mode/integrity/when");
        // Belt-and-braces: the serialised line never mentions a password key.
        check(line.find(L"password") == std::wstring::npos && line.find(L"pwd") == std::wstring::npos,
              L"serialised run has no password field");
    }

    // --- rule-4 gate: SYSTEM/TI only with the switch on --------------------------------------
    check(!LaunchAllowed(Who::System, false) && !LaunchAllowed(Who::TrustedInstaller, false),
          L"LaunchAllowed: SYSTEM/TI refused with the switch off");
    check(LaunchAllowed(Who::System, true) && LaunchAllowed(Who::TrustedInstaller, true),
          L"LaunchAllowed: SYSTEM/TI allowed with the switch on");
    check(LaunchAllowed(Who::ThisAdmin, false) && LaunchAllowed(Who::AnotherUser, false) &&
          LaunchAllowed(Who::ThisAdmin, true) && LaunchAllowed(Who::AnotherUser, true),
          L"LaunchAllowed: This admin / Another user always allowed");

    // --- association template expansion -----------------------------------------------------
    {
        std::wstring f = L"C:\\My Docs\\a b.txt";
        std::wstring un = ExpandAssoc(L"C:\\Windows\\notepad.exe %1", f, L"");
        std::wstring q = ExpandAssoc(L"\"C:\\Windows\\notepad.exe\" \"%1\"", f, L"");
        int n1 = 0, n2 = 0;
        LPWSTR* a1 = CommandLineToArgvW(un.c_str(), &n1);
        LPWSTR* a2 = CommandLineToArgvW(q.c_str(), &n2);
        bool ok1 = a1 && n1 == 2 && std::wstring(a1[1]) == f;
        bool ok2 = a2 && n2 == 2 && std::wstring(a2[1]) == f;
        if (a1) LocalFree(a1);
        if (a2) LocalFree(a2);
        check(ok1, L"ExpandAssoc quotes a spaced path for an unquoted %1 (" + un + L")");
        check(ok2, L"ExpandAssoc does not double-quote an already quoted \"%1\" (" + q + L")");
    }

    // --- account split for CreateProcessWithLogonW ------------------------------------------
    {
        std::wstring u, d;
        SplitAccount(L"alice", &u, &d);       bool s1 = u == L"alice" && d == L".";
        SplitAccount(L"PC1\\bob", &u, &d);    bool s2 = u == L"bob" && d == L"PC1";
        SplitAccount(L"carol@corp.example", &u, &d); bool s3 = u == L"carol" && d == L"corp.example";
        check(s1 && s2 && s3, L"SplitAccount handles user, domain\\user and user@domain");
    }

    // --- identity label ---------------------------------------------------------------------
    check(IdentityLabel((int)Who::System, (int)TokenMode::Full, L"") == L"SYSTEM (Full)",
          L"IdentityLabel SYSTEM (Full)");
    check(IdentityLabel((int)Who::AnotherUser, (int)TokenMode::Limited, L"alice") == L"alice (Limited)",
          L"IdentityLabel another-user (Limited)");

    wprintf(L"runas selftest: %ls (%d checks, %d failed)\n", fails ? L"FAIL" : L"PASS", checks, fails);
    fflush(stdout);
    return fails ? 1 : 0;
}

}  // namespace

std::unique_ptr<Module> MakeRunAsModule() { return std::make_unique<RunAsModule>(); }

}  // namespace hx
