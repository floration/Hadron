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
    ID_SHOW,   // Console "show:" filter chip (data = Who, or -1 for all)
};

// Three-Pane NavNode ids: 1 + Who for the identities, kNavRecent + k for the recent launches.
constexpr int kNavRecent = 100;
constexpr size_t kNavRecentMax = 8;

// Sort keys, so the Baseline/Three-Pane list (4 columns) and the wider Console table share SortRuns.
enum SortKey { K_MARK, K_TYPE, K_PATH, K_ARGS, K_IDENT, K_TOKEN, K_INTEG, K_WHEN };
const SortKey kStdKeys[] = {K_PATH, K_IDENT, K_INTEG, K_WHEN};
const SortKey kConKeys[] = {K_MARK, K_TYPE, K_PATH, K_ARGS, K_IDENT, K_TOKEN, K_INTEG, K_WHEN};

// Where keyboard/typing focus sits.
enum class Focus { None, Path, Args, Work, Search, Click, List };

// Text fields are core/ui TextField (selection, clipboard, context menu).
using Edit = hx::ui::TextField;

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
    std::vector<NavNode> NavNodes() override;
    int  NavSelected() override { return 1 + (int)who_; }
    void OnNavSelect(int id) override;
    std::vector<FKey> FKeys() override;
    std::wstring ConsoleHeader() override;
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
    bool ThreePane() const { return host_ && host_->Layout() == Layout::ThreePane; }
    // One list per layout (Baseline's 4 columns, Three-Pane's fitted ones, Console's wider table), so
    // each keeps its own columns, sort and selection.
    ListView& List() { return Console() ? listCon_ : ThreePane() ? listTp_ : list_; }
    void Chrome() { if (host_) host_->ChromeChanged(); }
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
    bool Visible(const LastRun& r) const;   // search + Console "show:" filter (never in Baseline)
    void Refill(const LastRun& r);          // re-fill the form from a history row (never runs it)
    const LastRun* SelectedRun();
    bool Gate(Who w) {   // rule-4 gate; says why on refusal
        if (LaunchAllowed(w, allowElevated_)) return true;
        resultLine_ = L"Turn on \"Allow SYSTEM / TrustedInstaller\" first.";
        resultBad_ = true;
        Invalidate();
        return false;
    }

    void Button(HDC dc, const RECT& r, const std::wstring& label, int id, int data, bool primary,
                bool enabled, bool selected);
    void DrawForm(HDC dc, const RECT& rc);        // Baseline (unchanged)
    void DrawThreePane(HDC dc, const RECT& rc);
    void DrawConsole(HDC dc, const RECT& rc);
    void DrawFocusRing(HDC dc);
    // Square controls for Three-Pane and Console (radius 0; Baseline keeps its rounded Button).
    void SqButton(HDC dc, const RECT& r, const std::wstring& label, int id, int data, bool primary,
                  bool enabled, bool selected, HFONT font);
    void SqCheck(HDC dc, const RECT& r, const std::wstring& label, int id, bool on, HFONT font);
    std::wstring HowText() const;            // what Run does for the chosen identity (from launch.cpp)

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

    void CycleWho();
    void SetDetailWidthFromX(int x);
    bool WhoEnabled(Who w) const {
        return (w == Who::System || w == Who::TrustedInstaller) ? allowElevated_ : true;
    }

    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;
    bool demo_ = false;

    Edit path_, args_, work_;
    SearchBox search_;                     // filters the recent list (Three-Pane and Console)
    Edit* DragEdit() {
        for (Edit* e : {&path_, &args_, &work_, (Edit*)&search_}) if (e->Dragging()) return e;
        return nullptr;
    }
    const RECT& EditRect(const Edit* e) const {
        return e == &path_ ? rcPath_ : e == &args_ ? rcArgs_ : e == &work_ ? rcWork_ : rcSearch_;
    }
    Edit* EditAt(POINT p) {
        if (PtInRect(&rcPath_, p)) return &path_;
        if (PtInRect(&rcArgs_, p)) return &args_;
        if (PtInRect(&rcWork_, p)) return &work_;
        if (PtInRect(&rcSearch_, p)) return &search_;
        return nullptr;
    }
    Focus FocusOf(const Edit* e) const {
        return e == &path_ ? Focus::Path : e == &args_ ? Focus::Args : e == &work_ ? Focus::Work : Focus::Search;
    }
    Edit* FocusedEdit() {
        switch (focus_) {
        case Focus::Path: return &path_;
        case Focus::Args: return &args_;
        case Focus::Work: return &work_;
        case Focus::Search: return &search_;
        default: return nullptr;
        }
    }
    Who who_ = Who::ThisAdmin;
    TokenMode mode_ = TokenMode::Full;
    bool allowElevated_ = false;          // OFF at every start, never persisted (rule 4)
    std::vector<Account> accounts_;
    int accountIdx_ = 0;

    std::vector<Tool> tools_;
    std::vector<LastRun> runs_;
    std::vector<int> view_;                // list row -> runs_ index (after the filter)
    std::vector<LastRun> recentNav_;       // the runs behind the Three-Pane "RECENT" nodes
    ListView list_, listTp_, listCon_;
    int showWho_ = -1;                     // Console "show:" chip; -1 = all
    std::wstring userName_;                // for the Console header
    bool layoutKnown_ = false;
    Layout lastLayout_ = Layout::Baseline;
    int  detailW_ = 360;                   // Three-Pane detail pane width, DIPs (Prefs "detailw")
    bool splitDrag_ = false;
    int  splitGrab_ = 0;
    bool swallowSlash_ = false;            // the '/' that focused the search must not be typed into it

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

    RECT rcPath_{}, rcArgs_{}, rcWork_{}, rcList_{}, rcSearch_{}, rcSplit_{};
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
            POINT cp; GetCursorPos(&cp); ScreenToClient(h, &cp);
            bool inEdit = EditAt(cp) != nullptr;
            bool inSplit = splitDrag_ || PtInRect(&rcSplit_, cp);
            SetCursor(LoadCursorW(nullptr, inSplit ? IDC_SIZEWE : inEdit ? IDC_IBEAM
                                           : hoverId_ > 0 ? IDC_HAND : IDC_ARROW));
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
        Chrome();   // the recent-launch nodes and the Console header changed
        Invalidate();
        return 0;
    case WM_CAPTURECHANGED:
        path_.CancelDrag(); args_.CancelDrag(); work_.CancelDrag(); search_.CancelDrag();
        if (splitDrag_) {
            splitDrag_ = false;
            host_->Prefs().SetInt(L"detailw", detailW_);
            Invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        [[fallthrough]];
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN:
        SetFocus(h);
        if (m == WM_RBUTTONDOWN) return 0;
        [[fallthrough]];
    case WM_LBUTTONUP: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_CHAR: case WM_RBUTTONUP:
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

    // Column widths are DIPs: ListView scales them itself (they were Sc()'d twice before).
    list_.SetColumns({{L"Path", 360}, {L"Identity", 150}, {L"Integrity", 90}, {L"When", 130}});
    for (ListView* lv : {&list_, &listTp_, &listCon_}) {
        lv->onActivate = [this](int i) {
            if (i < 0 || i >= (int)view_.size()) return;
            Refill(runs_[view_[i]]);
        };
        lv->onSortChanged = [this](int, bool) { SortRuns(); RefillList(); Invalidate(); };
    }
    search_.onChanged = [this](const std::wstring&) { RefillList(); Invalidate(); };
    {
        wchar_t un[257] = {};
        DWORD n = 257;
        userName_ = GetUserNameW(un, &n) ? un : L"?";
    }
    detailW_ = std::clamp(host.Prefs().GetInt(L"detailw", 360), 240, 900);

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
    // Three-Pane and Console screenshots show a selected row and its detail; Baseline's list stays as it was.
    listTp_.Select(1);
    listCon_.Select(1);
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

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// "exe", "lnk", "msc"... for the Console TYPE column.
std::wstring ExtOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return Lower(path.substr(dot + 1));
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool Elevated(int who) { return who == (int)Who::System || who == (int)Who::TrustedInstaller; }

// Identity without the token mode: "This admin", "alice", "SYSTEM", "TrustedInstaller".
std::wstring WhoText(int who, const std::wstring& account) {
    if (who == (int)Who::AnotherUser && !account.empty()) return account;
    return WhoName((Who)who);
}

// Short identity for tree notes and Console chips.
const wchar_t* WhoKey(Who w) {
    switch (w) {
    case Who::ThisAdmin:        return L"this-admin";
    case Who::AnotherUser:      return L"another-user";
    case Who::System:           return L"system";
    case Who::TrustedInstaller: return L"trustedinstaller";
    }
    return L"?";
}

// Sort runs_ itself (stable) so a list row maps back to runs_ through view_ in onActivate.
void RunAsModule::SortRuns() {
    ListView& lv = List();
    int col = lv.SortColumn();
    if (col < 0) return;
    bool con = Console();
    if (col >= (con ? 8 : 4)) return;
    SortKey key = con ? kConKeys[col] : kStdKeys[col];
    bool asc = lv.SortAscending();
    std::stable_sort(runs_.begin(), runs_.end(), [&](const LastRun& a, const LastRun& b) {
        int c = 0;
        switch (key) {
            case K_MARK:  c = (int)Elevated(a.who) - (int)Elevated(b.who); break;
            case K_TYPE:  c = _wcsicmp(ExtOf(a.path).c_str(), ExtOf(b.path).c_str()); break;
            case K_PATH:  c = _wcsicmp(a.path.c_str(), b.path.c_str()); break;
            case K_ARGS:  c = _wcsicmp(a.args.c_str(), b.args.c_str()); break;
            case K_IDENT: c = _wcsicmp(IdentityLabel(a.who, a.mode, a.account).c_str(),
                                       IdentityLabel(b.who, b.mode, b.account).c_str()); break;
            case K_TOKEN: c = a.mode - b.mode; break;
            case K_INTEG: c = IntegrityRank(a.integrity) - IntegrityRank(b.integrity); break;
            case K_WHEN:  c = (a.when < b.when) ? -1 : (a.when > b.when ? 1 : 0); break;   // by timestamp
        }
        return asc ? c < 0 : c > 0;
    });
}

// The filter exists only where its controls are drawn: Baseline always lists every run.
bool RunAsModule::Visible(const LastRun& r) const {
    if (!host_ || host_->Layout() == Layout::Baseline) return true;
    if (host_->Layout() == Layout::Console && showWho_ >= 0 && r.who != showWho_) return false;
    std::wstring q = Lower(search_.Text());
    if (q.empty()) return true;
    for (const std::wstring* f : {&r.path, &r.args, &r.integrity})
        if (Lower(*f).find(q) != std::wstring::npos) return true;
    return Lower(IdentityLabel(r.who, r.mode, r.account)).find(q) != std::wstring::npos;
}

const LastRun* RunAsModule::SelectedRun() {
    int i = List().Selected();
    return (i >= 0 && i < (int)view_.size()) ? &runs_[view_[i]] : nullptr;
}

void RunAsModule::RefillList() {
    // Keep the selected run selected across a re-sort or a filter change.
    auto keyOf = [](const LastRun& r) {
        return r.path + L'\x1' + r.args + L'\x1' + r.account + L'\x1' + std::to_wstring(r.who) +
               L'\x1' + std::to_wstring(r.mode) + L'\x1' + std::to_wstring(r.when);
    };
    std::wstring selKey[3];
    ListView* lvs[3] = {&list_, &listTp_, &listCon_};
    for (int k = 0; k < 3; k++) {
        int i = lvs[k]->Selected();
        if (i >= 0 && i < (int)view_.size() && view_[i] < (int)runs_.size()) selKey[k] = keyOf(runs_[view_[i]]);
    }
    view_.clear();
    for (int i = 0; i < (int)runs_.size(); i++)
        if (Visible(runs_[i])) view_.push_back(i);

    std::vector<std::vector<std::wstring>> rows, con;
    std::vector<ListView::Ink> ink;
    rows.reserve(view_.size());
    con.reserve(view_.size());
    for (int idx : view_) {
        const LastRun& r = runs_[idx];
        std::wstring integ = r.integrity.empty() ? L"—" : r.integrity;
        rows.push_back({r.path, IdentityLabel(r.who, r.mode, r.account), integ, WhenText(r.when)});
        con.push_back({Elevated(r.who) ? L"!" : L"", ExtOf(r.path), r.path, r.args.empty() ? L"-" : r.args,
                       WhoText(r.who, r.account), r.mode == (int)TokenMode::Limited ? L"limited" : L"full",
                       integ, WhenText(r.when)});
        // Console colours SYSTEM/TI rows like changed values; the "!" column says it in text too.
        ink.push_back(Elevated(r.who) ? ListView::Ink::Warn : ListView::Ink::Normal);
    }
    listTp_.SetRows(rows);
    list_.SetRows(std::move(rows));
    listCon_.SetRows(std::move(con));
    listCon_.SetRowInk(std::move(ink));
    for (int k = 0; k < 3; k++) {
        if (selKey[k].empty()) continue;
        for (int i = 0; i < (int)view_.size(); i++)
            if (keyOf(runs_[view_[i]]) == selKey[k]) { lvs[k]->Select(i); break; }
    }
}

void RunAsModule::Refill(const LastRun& r) {
    path_.SetText(r.path); args_.SetText(r.args); work_.SetText(L"");
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
    Chrome();
    Invalidate();
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

// ============================================================================================
// Three-Pane and Console (board 7 and board 4). Baseline above is unchanged.

int TextW(HDC dc, HFONT f, const std::wstring& s) {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

// Height of word-wrapped text in a column of width w.
int WrapH(HDC dc, HFONT f, const std::wstring& s, int w) {
    RECT r{0, 0, w, 0};
    HGDIOBJ o = SelectObject(dc, f);
    DrawTextW(dc, s.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, o);
    return r.bottom;
}

void RunAsModule::SqButton(HDC dc, const RECT& r, const std::wstring& label, int id, int data,
                           bool primary, bool enabled, bool selected, HFONT font) {
    const Theme& t = host_->Theme();
    bool hv = enabled && IsHover(id, data);
    COLORREF bg, bc, tc;
    if (t.highContrast) {
        bool on = primary || selected;
        bg = GetSysColor(on ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
        bc = GetSysColor(COLOR_WINDOWTEXT);
        tc = enabled ? GetSysColor(on ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT) : GetSysColor(COLOR_GRAYTEXT);
    } else if (primary || (selected && Console())) {
        // Console fills a selected chip with the accent, like its selected row.
        bg = hv ? Mix(t.accent, RGB(255, 255, 255), 0.12) : t.accent;
        bc = bg;
        tc = enabled ? OnFill(t.accent) : Mix(OnFill(t.accent), t.accent, 0.45);
    } else if (selected) {
        bg = Mix(t.panel2, t.accent, 0.22);
        bc = t.accent;
        tc = t.text1;
    } else {
        bg = hv ? t.panel3 : t.panel2;
        bc = hv ? Mix(t.border, t.text3, 0.5) : t.border;
        tc = enabled ? t.text1 : t.text3;
    }
    FillSolid(dc, r, bg);
    FrameRectC(dc, r, bc);
    RECT tr{r.left + Sc(6), r.top, r.right - Sc(6), r.bottom};
    DrawText(dc, label, tr, font, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    if (enabled) AddClick(r, id, data);
}

// The "Allow SYSTEM / TrustedInstaller" switch as a square check. Same ID_ALLOW click as Baseline.
void RunAsModule::SqCheck(HDC dc, const RECT& r, const std::wstring& label, int id, bool on, HFONT font) {
    const Theme& t = host_->Theme();
    bool hv = IsHover(id);
    COLORREF bg = t.highContrast ? GetSysColor(COLOR_BTNFACE) : (hv ? t.panel3 : t.panel2);
    COLORREF bc = t.highContrast ? GetSysColor(COLOR_WINDOWTEXT) : (on ? t.warn : t.border);
    COLORREF ink = t.highContrast ? GetSysColor(COLOR_BTNTEXT) : t.text1;
    FillSolid(dc, r, bg);
    FrameRectC(dc, r, bc);
    int bs = Sc(12), bx = r.left + Sc(7), by = (r.top + r.bottom) / 2 - bs / 2;
    RECT box{bx, by, bx + bs, by + bs};
    FrameRectC(dc, box, t.highContrast ? ink : (on ? t.warn : t.text3));
    if (on) {
        RECT in{box.left + Sc(3), box.top + Sc(3), box.right - Sc(3), box.bottom - Sc(3)};
        FillSolid(dc, in, t.highContrast ? ink : t.warn);
    }
    RECT lt{box.right + Sc(7), r.top, r.right - Sc(6), r.bottom};
    DrawText(dc, label, lt, font, ink, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    AddClick(r, id);
}

// What Run does for the chosen identity, in the words of launch.cpp (no numbers, no promises: the result
// line shows the identity read back from the child's token).
std::wstring RunAsModule::HowText() const {
    bool lim = mode_ == TokenMode::Limited;
    switch (who_) {
    case Who::ThisAdmin:
        return lim ? L"Uses the shell's de-elevated token (or this admin's UAC-linked token), so the program "
                     L"runs without admin rights."
                   : L"Starts the target with ShellExecuteEx from this elevated process: your full admin token. "
                     L"Documents, shortcuts and .msc files open with their usual handler.";
    case Who::AnotherUser:
        return L"Logs on as the chosen local account with CreateProcessWithLogonW. Windows asks for its "
               L"password each time; it is never saved. The token is the one Windows gives that logon; the "
               L"Full/Limited choice does not change it.";
    case Who::System:
        return lim ? L"Duplicates the SYSTEM token of winlogon.exe in this session, restricts it (LUA) and "
                     L"lowers it to medium integrity."
                   : L"Duplicates the SYSTEM token of winlogon.exe in this session. More power than an "
                     L"administrator; it can damage Windows.";
    case Who::TrustedInstaller:
        return std::wstring(L"Starts the TrustedInstaller service if needed, takes its token while acting as "
                            L"SYSTEM and moves it to this session.") +
               (lim ? L" Then restricts it (LUA) and lowers it to medium integrity."
                    : L" It can change protected parts of Windows.");
    }
    return L"";
}

void RunAsModule::DrawThreePane(HDC dc, const RECT& rc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    const bool hc = t.highContrast;
    const COLORREF text1 = hc ? GetSysColor(COLOR_WINDOWTEXT) : t.text1;
    const COLORREF text2 = hc ? GetSysColor(COLOR_WINDOWTEXT) : t.text2;
    const COLORREF text3 = hc ? GetSysColor(COLOR_GRAYTEXT) : t.text3;
    const COLORREF border = hc ? GetSysColor(COLOR_WINDOWTEXT) : t.border;
    const COLORREF panel = hc ? GetSysColor(COLOR_WINDOW) : t.panel;
    clicks_.clear();
    const bool running = worker_.Running();

    // --- columns: list | 5px splitter | detail ----------------------------------------------
    int W = rc.right - rc.left;
    int minMid = Sc(380), minDet = Sc(240);
    int dw = Sc(detailW_);
    if (dw > W - minMid - Sc(5)) dw = W - minMid - Sc(5);
    if (dw < minDet) dw = minDet;
    RECT det{rc.right - dw, rc.top, rc.right, rc.bottom};
    rcSplit_ = {det.left - Sc(5), rc.top, det.left, rc.bottom};
    RECT mid{rc.left, rc.top, rcSplit_.left, rc.bottom};
    FillSolid(dc, rcSplit_, hc ? GetSysColor(COLOR_WINDOW) : t.bg);
    VLine(dc, rcSplit_.left, rc.top, rc.bottom, border);
    VLine(dc, rcSplit_.right - 1, rc.top, rc.bottom, border);

    // --- toolbar: square buttons, search at the right end -------------------------------------
    const int P = Sc(10), gap = Sc(6), tbH = Sc(26);
    RECT bar{mid.left, mid.top, mid.right, mid.top + Sc(40)};
    FillSolid(dc, bar, panel);
    HLine(dc, bar.left, bar.right, bar.bottom - 1, border);
    int x = mid.left + P, ty = bar.top + (Sc(40) - tbH) / 2;
    auto tb = [&](const std::wstring& s, int id, int data, bool primary, bool enabled, bool selected) {
        int w = TextW(dc, f.semi, s) + Sc(22);
        RECT r{x, ty, x + w, ty + tbH};
        SqButton(dc, r, s, id, data, primary, enabled, selected, f.semi);
        x = r.right + gap;
    };
    auto sep = [&]() { VLine(dc, x + gap / 2, ty + Sc(3), ty + tbH - Sc(3), border); x += gap * 2; };
    tb(running ? L"Running…" : L"Run", ID_RUN, 0, true, !running, false);
    tb(L"Browse…", ID_BROWSE, 0, false, true, false);
    sep();
    tb(L"Full", ID_MODE, (int)TokenMode::Full, false, true, mode_ == TokenMode::Full);
    tb(L"Limited", ID_MODE, (int)TokenMode::Limited, false, true, mode_ == TokenMode::Limited);
    sep();
    {
        std::wstring s = L"Allow SYSTEM / TI";
        int w = TextW(dc, f.semi, s) + Sc(36);
        RECT r{x, ty, x + w, ty + tbH};
        SqCheck(dc, r, s, ID_ALLOW, allowElevated_, f.semi);
        x = r.right + gap;
    }
    int sw = std::min(Sc(220), (int)(mid.right - P) - (x + Sc(8)));
    if (sw >= Sc(90)) {
        rcSearch_ = {mid.right - P - sw, ty, mid.right - P, ty + tbH};
        search_.Paint(dc, rcSearch_, Ctx(), focus_ == Focus::Search);
    }

    // --- breadcrumb + counts ----------------------------------------------------------------
    int L = mid.left + P, R = mid.right - P;
    int y = bar.bottom;
    {
        RECT crumb{L, y, R, y + Sc(30)};
        std::wstring who = WhoText((int)who_, who_ == Who::AnotherUser && !accounts_.empty()
                                                  ? accounts_[accountIdx_].name : L"");
        std::wstring path = L"Run As  ›  " + who + L"  ›  " +
                            (mode_ == TokenMode::Limited ? L"Limited token" : L"Full token");
        std::wstring counts = std::to_wstring(view_.size()) +
                              (view_.size() == runs_.size() ? L"" : L" of " + std::to_wstring(runs_.size())) +
                              L" recent  ·  " + std::to_wstring(accounts_.size()) + L" accounts";
        int cw = TextW(dc, f.caption, counts);
        RECT cr{R - cw, crumb.top, R, crumb.bottom};
        TextRight(dc, counts, cr, f.caption, text3);
        crumb.right = cr.left - Sc(12);
        TextLeft(dc, path, crumb, f.ui, text2);
        y = crumb.bottom;
        HLine(dc, mid.left, mid.right, y - 1, border);
    }
    y += Sc(10);

    // --- the form: label column + fields ------------------------------------------------------
    const int labW = Sc(84), fh = Sc(28);
    auto lab = [&](const wchar_t* s) {
        RECT lr{L, y, L + labW, y + fh};
        TextLeft(dc, s, lr, f.ui, text3);
    };
    lab(L"Program");
    rcPath_ = {L + labW, y, R, y + fh};
    path_.Paint(dc, rcPath_, Ctx(), focus_ == Focus::Path);
    y += fh + gap;
    lab(L"Arguments");
    rcArgs_ = {L + labW, y, R, y + fh};
    args_.Paint(dc, rcArgs_, Ctx(), focus_ == Focus::Args);
    y += fh + gap;
    lab(L"Folder");
    rcWork_ = {L + labW, y, R, y + fh};
    work_.Paint(dc, rcWork_, Ctx(), focus_ == Focus::Work);
    y += fh + gap;
    if (who_ == Who::AnotherUser) {
        lab(L"Account");
        std::wstring name = accounts_.empty() ? std::wstring(L"(no local accounts)")
                                              : accounts_[accountIdx_].name + L"   (click for the next)";
        RECT ar{L + labW, y, std::min(R, L + labW + Sc(300)), y + fh};
        SqButton(dc, ar, name, ID_ACCOUNT, 0, false, !accounts_.empty(), false, f.ui);
        y += fh + gap;
    }
    {
        RECT res{L + labW, y, R, y + Sc(22)};
        if (!resultLine_.empty()) {
            COLORREF c = hc ? GetSysColor(COLOR_WINDOWTEXT) : resultBad_ ? t.bad : t.good;
            TextLeft(dc, resultLine_, res, f.ui, c);
        } else {
            TextLeft(dc, L"Enter in a field runs it. The result shows the identity Windows actually gave.",
                     res, f.caption, text3);
        }
        y += Sc(30);
    }

    // --- recent launches: the list fills the rest --------------------------------------------
    {
        RECT hr{L, y, R, y + Sc(20)};
        TextRight(dc, L"Enter re-fills the form; it never runs", hr, f.caption, text3);
        TextLeft(dc, L"RECENT LAUNCHES", hr, f.caption, text3);
        y += Sc(24);
    }
    rcList_ = {L, y, R, rc.bottom - P};
    if (rcList_.bottom < rcList_.top + Sc(60)) rcList_.bottom = rcList_.top + Sc(60);
    {   // the path column takes what the middle pane has left (DIPs; ListView scales them)
        int wDip = MulDiv(R - L, 96, (int)host_->Dpi());
        listTp_.SetColumns({{L"Path", std::max(160, wDip - (130 + 80 + 130) - 12)}, {L"Identity", 130},
                            {L"Integrity", 80}, {L"When", 130}});
    }
    listTp_.Paint(dc, rcList_, Ctx(), focus_ == Focus::List);

    // --- detail pane ----------------------------------------------------------------------------
    FillSolid(dc, det, panel);
    const int save = SaveDC(dc);
    IntersectClipRect(dc, det.left, det.top, det.right, det.bottom);
    int X = det.left + Sc(14), XR = det.right - Sc(14);
    int dy = det.top + Sc(12);
    const int rowH = Sc(22), keyW = Sc(96);
    auto kv = [&](const std::wstring& k, const std::wstring& v, COLORREF vc) {
        RECT kr{X, dy, X + keyW, dy + rowH}, vr{X + keyW, dy, XR, dy + rowH};
        TextLeft(dc, k, kr, f.caption, text3);
        TextLeft(dc, v, vr, f.ui, vc);
        dy += rowH;
    };
    auto section = [&](const wchar_t* s) {
        dy += Sc(10);
        HLine(dc, X, XR, dy, border);
        RECT hr{X, dy + Sc(4), XR, dy + Sc(22)};
        TextLeft(dc, s, hr, f.caption, text3);
        dy += Sc(24);
    };
    {
        RECT tr{X, dy, XR, dy + Sc(26)};
        TextLeft(dc, L"Run request", tr, f.semi, text1);
        dy += Sc(30);
    }
    kv(L"Identity", WhoText((int)who_, who_ == Who::AnotherUser && !accounts_.empty()
                                           ? accounts_[accountIdx_].name : L""), text1);
    kv(L"Token", mode_ == TokenMode::Limited ? L"Limited" : L"Full", text1);
    kv(L"Target", path_.Text().empty() ? L"—" : FileNameOf(path_.Text()), text1);
    kv(L"Arguments", args_.Text().empty() ? L"—" : args_.Text(), text1);
    kv(L"Folder", work_.Text().empty() ? L"The target's folder" : work_.Text(), text1);
    kv(L"SYSTEM / TI", allowElevated_ ? L"Allowed until Hadron closes" : L"Off (turn on in the toolbar)",
       allowElevated_ && !hc ? t.warn : text1);

    section(L"HOW IT LAUNCHES");
    {
        std::wstring how = HowText();
        int h = WrapH(dc, f.caption, how, XR - X);
        RECT hr{X, dy, XR, dy + h};
        DrawText(dc, how, hr, f.caption, text2, DT_WORDBREAK | DT_LEFT);
        dy += h;
    }

    if (const LastRun* r = SelectedRun()) {
        section(L"SELECTED LAUNCH");
        kv(L"Path", r->path, text1);
        if (!r->args.empty()) kv(L"Arguments", r->args, text1);
        kv(L"Identity", IdentityLabel(r->who, r->mode, r->account), text1);
        kv(L"Integrity", r->integrity.empty() ? L"—" : r->integrity, text1);
        kv(L"When", WhenText(r->when), text1);
    }

    section(L"WINDOWS TOOLS");
    {
        RECT nr{X, dy, XR, dy + Sc(18)};
        TextLeft(dc, L"Launched at the identity above", nr, f.caption, text3);
        dy += Sc(22);
        int colW = (XR - X - gap) / 2, bh = Sc(26);
        for (size_t i = 0; i < tools_.size(); i++) {
            int cx = X + (int)(i % 2) * (colW + gap);
            RECT r{cx, dy, cx + colW, dy + bh};
            if (r.bottom <= det.bottom)   // never a focus stop the user can't see
                SqButton(dc, r, tools_[i].label, ID_TOOL, (int)i, false, !running, false, f.ui);
            if (i % 2 == 1 || i + 1 == tools_.size()) dy += bh + gap;
        }
    }
    RestoreDC(dc, save);
}

void RunAsModule::DrawConsole(HDC dc, const RECT& rc) {
    const Theme& t = host_->Theme();
    const Fonts& f = host_->Fonts();
    const bool hc = t.highContrast;
    const COLORREF text1 = hc ? GetSysColor(COLOR_WINDOWTEXT) : t.text1;
    const COLORREF text2 = hc ? GetSysColor(COLOR_WINDOWTEXT) : t.text2;
    const COLORREF text3 = hc ? GetSysColor(COLOR_GRAYTEXT) : t.text3;
    const COLORREF warn = hc ? GetSysColor(COLOR_WINDOWTEXT) : t.warn;
    const COLORREF border = hc ? GetSysColor(COLOR_WINDOWTEXT) : t.border;
    clicks_.clear();
    const bool running = worker_.Running();
    const HFONT mono = f.mono, cap = f.caption;

    const int P = Sc(8), gap = Sc(6), lh = Sc(22), labW = Sc(64);
    int L = rc.left + P, R = rc.right - P;
    int y = rc.top + Sc(6);
    auto lab = [&](const wchar_t* s, int x0, int w) {
        RECT r{x0, y, x0 + w, y + lh};
        TextLeft(dc, s, r, cap, text3);
    };
    // A chip sized to its text, left to right on the current line.
    int x = L;
    auto chip = [&](const std::wstring& s, int id, int data, bool primary, bool enabled, bool selected) {
        int w = TextW(dc, mono, s) + Sc(14);
        RECT r{x, y, x + w, y + lh};
        SqButton(dc, r, s, id, data, primary, enabled, selected, mono);
        x = r.right + Sc(4);
    };

    // --- TARGET / ARGS / CWD --------------------------------------------------------------------
    lab(L"TARGET", L, labW);
    {
        std::wstring b = L"F2 browse";
        int bw = TextW(dc, mono, b) + Sc(14);
        rcPath_ = {L + labW, y, R - bw - gap, y + lh};
        path_.Paint(dc, rcPath_, Ctx(), focus_ == Focus::Path);
        RECT br{R - bw, y, R, y + lh};
        SqButton(dc, br, b, ID_BROWSE, 0, false, true, false, mono);
    }
    y += lh + Sc(4);
    {
        int half = (R - L - labW) / 2;
        lab(L"ARGS", L, labW);
        rcArgs_ = {L + labW, y, L + labW + half - gap, y + lh};
        args_.Paint(dc, rcArgs_, Ctx(), focus_ == Focus::Args);
        int cx = L + labW + half + gap;
        lab(L"CWD", cx, Sc(40));
        rcWork_ = {cx + Sc(40), y, R, y + lh};
        work_.Paint(dc, rcWork_, Ctx(), focus_ == Focus::Work);
    }
    y += lh + Sc(4);

    // --- AS / TOKEN / ACCOUNT / allow switch ------------------------------------------------------
    lab(L"AS", L, labW);
    x = L + labW;
    const Who whoOrder[4] = {Who::ThisAdmin, Who::AnotherUser, Who::System, Who::TrustedInstaller};
    for (Who w : whoOrder) chip(WhoKey(w), ID_WHO, (int)w, false, WhoEnabled(w), who_ == w);
    x += Sc(14);
    {
        RECT r{x, y, x + Sc(50), y + lh};
        TextLeft(dc, L"TOKEN", r, cap, text3);
        x = r.right;
    }
    chip(L"full", ID_MODE, (int)TokenMode::Full, false, true, mode_ == TokenMode::Full);
    chip(L"limited", ID_MODE, (int)TokenMode::Limited, false, true, mode_ == TokenMode::Limited);
    if (who_ == Who::AnotherUser) {
        x += Sc(14);
        RECT r{x, y, x + Sc(64), y + lh};
        TextLeft(dc, L"ACCOUNT", r, cap, text3);
        x = r.right;
        chip(accounts_.empty() ? std::wstring(L"(none)") : accounts_[accountIdx_].name, ID_ACCOUNT, 0, false,
             !accounts_.empty(), false);
    }
    y += lh + Sc(4);
    x = L + labW;
    {
        std::wstring s = L"F8 allow SYSTEM / TrustedInstaller";
        int w = TextW(dc, mono, s) + Sc(32);
        RECT r{x, y, x + w, y + lh};
        SqCheck(dc, r, s, ID_ALLOW, allowElevated_, mono);
        RECT wr{r.right + Sc(10), y, R, y + lh};
        TextLeft(dc, allowElevated_ ? L"ON until Hadron closes: more power than an administrator; can damage Windows"
                                    : L"off: more power than an administrator; can damage Windows",
                 wr, cap, warn);
    }
    y += lh + Sc(6);

    // --- RUN + result -------------------------------------------------------------------------------
    {
        x = L + labW;
        chip(running ? L"F5 running…" : L"F5 run", ID_RUN, 0, true, !running, false);
        RECT res{x + Sc(8), y, R, y + lh};
        if (!resultLine_.empty()) {
            COLORREF c = hc ? text1 : resultBad_ ? t.bad : t.good;
            TextLeft(dc, L"> " + resultLine_, res, mono, c);
        } else {
            TextLeft(dc, L"> ready. Enter in a field runs it; the result is read back from the child's token.",
                     res, mono, text3);
        }
    }
    y += lh + Sc(6);
    HLine(dc, rc.left, rc.right, y, border);
    y += Sc(6);

    // --- filter line: / search, show: chips, row n/m ------------------------------------------------
    {
        x = L;
        RECT sl{x, y, x + Sc(14), y + lh};
        TextLeft(dc, L"/", sl, mono, hc ? text1 : t.accent);
        rcSearch_ = {x + Sc(14), y, x + Sc(14) + Sc(220), y + lh};
        search_.Paint(dc, rcSearch_, Ctx(), focus_ == Focus::Search);
        x = rcSearch_.right + Sc(16);
        RECT shl{x, y, x + Sc(44), y + lh};
        TextLeft(dc, L"show:", shl, cap, text3);
        x = shl.right;
        chip(L"all", ID_SHOW, -1, false, true, showWho_ < 0);
        chip(L"admin", ID_SHOW, (int)Who::ThisAdmin, false, true, showWho_ == (int)Who::ThisAdmin);
        chip(L"user", ID_SHOW, (int)Who::AnotherUser, false, true, showWho_ == (int)Who::AnotherUser);
        chip(L"system", ID_SHOW, (int)Who::System, false, true, showWho_ == (int)Who::System);
        chip(L"ti", ID_SHOW, (int)Who::TrustedInstaller, false, true, showWho_ == (int)Who::TrustedInstaller);
        int sel = listCon_.Selected();
        std::wstring rowTxt = L"row " + (sel >= 0 ? std::to_wstring(sel + 1) : std::wstring(L"-")) + L"/" +
                              std::to_wstring(view_.size());
        if (view_.size() != runs_.size()) rowTxt += L" of " + std::to_wstring(runs_.size());
        RECT rr{x + Sc(8), y, R, y + lh};
        TextRight(dc, rowTxt, rr, mono, text2);
    }
    y += lh + Sc(6);

    // --- bottom detail (about 30%), three columns ------------------------------------------------
    int detH = std::max(Sc(150), (int)((rc.bottom - rc.top) * 30 / 100));
    RECT det{rc.left, rc.bottom - detH, rc.right, rc.bottom};

    // --- the table: full width, every Console column ------------------------------------------------
    {
        int wDip = MulDiv(R - L, 96, (int)host_->Dpi());
        int fixed = 30 + 60 + 170 + 150 + 80 + 110 + 150;
        int pathW = std::max(200, wDip - fixed - 12);
        listCon_.SetColumns({{L"!", 30}, {L"TYPE", 60}, {L"PATH", pathW}, {L"ARGS", 170}, {L"AS", 150},
                             {L"TOKEN", 80}, {L"INTEGRITY", 110}, {L"WHEN", 150}});
    }
    rcList_ = {L, y, R, det.top - Sc(6)};
    if (rcList_.bottom < rcList_.top + Sc(60)) rcList_.bottom = rcList_.top + Sc(60);
    listCon_.Paint(dc, rcList_, Ctx(), focus_ == Focus::List);

    FillSolid(dc, det, hc ? GetSysColor(COLOR_WINDOW) : t.panel);
    HLine(dc, det.left, det.right, det.top, border);
    int colW = (det.right - det.left) / 3;
    VLine(dc, det.left + colW, det.top, det.bottom, border);
    VLine(dc, det.left + 2 * colW, det.top, det.bottom, border);
    const int rowH = Sc(18), keyW = Sc(56);
    const int save = SaveDC(dc);

    auto column = [&](int i, int* X, int* XR, int* dy) {
        *X = det.left + i * colW + Sc(10);
        *XR = det.left + (i + 1) * colW - Sc(10);
        *dy = det.top + Sc(6);
        RestoreDC(dc, save);
        SaveDC(dc);
        IntersectClipRect(dc, det.left + i * colW + 1, det.top + 1, det.left + (i + 1) * colW, det.bottom);
    };
    int X, XR, dy;
    auto head = [&](const std::wstring& s) {
        RECT r{X, dy, XR, dy + rowH};
        TextLeft(dc, s, r, cap, text3);
        dy += rowH + Sc(2);
    };
    auto kv = [&](const std::wstring& k, const std::wstring& v, COLORREF vc) {
        RECT kr{X, dy, X + keyW, dy + rowH}, vr{X + keyW, dy, XR, dy + rowH};
        TextLeft(dc, k, kr, mono, text3);
        TextLeft(dc, v, vr, mono, vc);
        dy += rowH;
    };

    // Column 1: the selected launch.
    column(0, &X, &XR, &dy);
    if (const LastRun* r = SelectedRun()) {
        RECT nr{X, dy, XR, dy + rowH};
        TextLeft(dc, FileNameOf(r->path), nr, f.semi, text1);
        dy += rowH + Sc(4);
        kv(L"path", r->path, text2);
        kv(L"args", r->args.empty() ? L"-" : r->args, text2);
        kv(L"as", IdentityLabel(r->who, r->mode, r->account), Elevated(r->who) ? warn : text1);
        kv(L"il", r->integrity.empty() ? L"-" : r->integrity, warn);
        kv(L"when", WhenText(r->when), text2);
        dy += Sc(4);
        RECT hr{X, dy, XR, dy + rowH};
        TextLeft(dc, L"Enter re-fills the form (does not run)", hr, cap, text3);
    } else {
        head(L"SELECTED LAUNCH");
        RECT hr{X, dy, XR, dy + rowH};
        TextLeft(dc, view_.empty() ? L"No launches match." : L"Select a row (Up/Down in the table).", hr, mono,
                 text3);
    }

    // Column 2: the request Run will make, and how.
    column(1, &X, &XR, &dy);
    head(L"REQUEST");
    kv(L"as", WhoKey(who_), Elevated((int)who_) ? warn : text1);
    kv(L"token", mode_ == TokenMode::Limited ? L"limited" : L"full", text1);
    if (who_ == Who::AnotherUser)
        kv(L"acct", accounts_.empty() ? L"(none)" : accounts_[accountIdx_].name, text1);
    kv(L"cwd", work_.Text().empty() ? L"(target's folder)" : work_.Text(), text1);
    kv(L"sys/ti", allowElevated_ ? L"ON" : L"off", allowElevated_ ? warn : text1);
    dy += Sc(4);
    {
        std::wstring how = HowText();
        int h = WrapH(dc, cap, how, XR - X);
        RECT hr{X, dy, XR, dy + h};
        DrawText(dc, how, hr, cap, text2, DT_WORDBREAK | DT_LEFT);
    }

    // Column 3: the Windows tools, launched at the selected identity.
    column(2, &X, &XR, &dy);
    head(L"WINDOWS TOOLS  (at the identity in REQUEST)");
    {
        int half = (XR - X - gap) / 2;
        int top = dy;
        for (size_t i = 0; i < tools_.size(); i++) {
            int rows = (int)(tools_.size() + 1) / 2;
            int c = (int)i / rows, r = (int)i % rows;
            RECT br{X + c * (half + gap), top + r * (rowH + Sc(3)), X + c * (half + gap) + half,
                    top + r * (rowH + Sc(3)) + rowH};
            if (br.bottom > det.bottom) continue;
            SqButton(dc, br, tools_[i].label, ID_TOOL, (int)i, false, !running, false, mono);
        }
    }
    RestoreDC(dc, save);
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
    Layout ly = host_->Layout();
    if (!layoutKnown_ || ly != lastLayout_) {
        // The filter and the sort column belong to the layout's own list: rebuild the rows for it.
        layoutKnown_ = true;
        lastLayout_ = ly;
        if (ly == Layout::Baseline && focus_ == Focus::Search) focus_ = Focus::Path;   // no search box there
        SortRuns();
        RefillList();
    }
    rcPath_ = rcArgs_ = rcWork_ = rcList_ = rcSearch_ = rcSplit_ = RECT{};
    FillSolid(dc, rc, host_->Theme().bg);
    if (ly == Layout::ThreePane) DrawThreePane(dc, rc);
    else if (ly == Layout::Console) DrawConsole(dc, rc);
    else DrawForm(dc, rc);
    DrawFocusRing(dc);
}

// ============================================================================================
// Actions

void RunAsModule::Browse() {
    std::vector<wchar_t> file(2048, 0);
    const std::wstring& init = path_.Text();
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
        path_.SetText(file.data());
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
    // The worker may be inside CreateProcessWithLogonW with pwd_: never touch the password while it runs.
    if (worker_.Running()) { resultLine_ = L"A launch is still running."; resultBad_ = true; Invalidate(); return; }
    if (path_.Text().empty()) {
        resultLine_ = L"Choose a program or file first.";
        resultBad_ = true;
        Invalidate();
        return;
    }
    if (!Gate(who_)) return;
    LaunchRequest req;
    req.path = path_.Text();
    // Explorer's "Copy as path" wraps the path in quotes; the launch wants it bare.
    if (req.path.size() >= 2 && req.path.front() == L'"' && req.path.back() == L'"')
        req.path = req.path.substr(1, req.path.size() - 2);
    req.args = args_.Text();
    req.workingDir = work_.Text();
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
        if (WhoEnabled((Who)data)) { who_ = (Who)data; Chrome(); Invalidate(); }
        break;
    case ID_MODE: mode_ = (TokenMode)data; Chrome(); Invalidate(); break;
    case ID_ALLOW:
        allowElevated_ = !allowElevated_;
        if (!allowElevated_ && (who_ == Who::System || who_ == Who::TrustedInstaller))
            who_ = Who::ThisAdmin;
        Chrome();
        Invalidate();
        break;
    case ID_ACCOUNT:
        if (!accounts_.empty()) {
            accountIdx_ = (accountIdx_ + 1) % (int)accounts_.size();
            Chrome();
            Invalidate();
        }
        break;
    case ID_RUN: RunTarget(); break;
    case ID_TOOL: RunTool(data); break;
    case ID_SHOW: showWho_ = data; RefillList(); Invalidate(); break;
    }
}

// F3: the next identity the switch allows (SYSTEM/TI are skipped while it is off).
void RunAsModule::CycleWho() {
    for (int k = 1; k <= 4; k++) {
        Who w = (Who)(((int)who_ + k) % 4);
        if (WhoEnabled(w)) { who_ = w; break; }
    }
    Chrome();
    Invalidate();
}

void RunAsModule::SetDetailWidthFromX(int x) {
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    int px = cr.right - (x - splitGrab_);
    int dip = MulDiv(px, 96, (int)host_->Dpi());
    int maxDip = std::max(240, MulDiv(cr.right, 96, (int)host_->Dpi()) - 385);
    detailW_ = std::clamp(dip, 240, maxDip);
}

// ============================================================================================
// Layout chrome: Three-Pane tree nodes, Console F-keys and header line

std::vector<NavNode> RunAsModule::NavNodes() {
    std::vector<NavNode> n;
    n.push_back({0, 0, L"RUN AS", L"", true, false});
    const Who order[4] = {Who::ThisAdmin, Who::AnotherUser, Who::System, Who::TrustedInstaller};
    for (Who w : order) {
        NavNode node;
        node.id = 1 + (int)w;
        node.label = WhoName(w);
        if (w == Who::AnotherUser) node.note = std::to_wstring(accounts_.size()) + L" accts";
        if (!WhoEnabled(w)) { node.note = L"locked"; node.dim = true; }
        n.push_back(node);
    }
    // The newest launches, newest first, whatever order the list is sorted in. Selecting one re-fills
    // the form (like Enter in the list); it never runs anything.
    recentNav_ = runs_;
    std::stable_sort(recentNav_.begin(), recentNav_.end(),
                     [](const LastRun& a, const LastRun& b) { return a.when > b.when; });
    if (recentNav_.size() > kNavRecentMax) recentNav_.resize(kNavRecentMax);
    n.push_back({0, 0, L"RECENT", runs_.empty() ? L"none" : std::to_wstring(runs_.size()), true, false});
    for (size_t k = 0; k < recentNav_.size(); k++) {
        const LastRun& r = recentNav_[k];
        NavNode node;
        node.id = kNavRecent + (int)k;
        node.label = FileNameOf(r.path);
        // Short: the host's tree column is narrow.
        node.note = r.who == (int)Who::ThisAdmin ? L"admin"
                  : r.who == (int)Who::TrustedInstaller ? L"TI" : WhoText(r.who, r.account);
        if (r.mode == (int)TokenMode::Limited) node.note += L" \u00B7 ltd";
        n.push_back(node);
    }
    return n;
}

void RunAsModule::OnNavSelect(int id) {
    if (id >= 1 && id <= 4) {
        Who w = (Who)(id - 1);
        if (!Gate(w)) return;   // says to turn the switch on; nothing else changes
        who_ = w;
        Invalidate();
        return;
    }
    int k = id - kNavRecent;
    if (k >= 0 && k < (int)recentNav_.size()) Refill(recentNav_[k]);
}

std::vector<FKey> RunAsModule::FKeys() {
    return {
        {VK_F5, L"F5", L"Run"},
        {VK_F2, L"F2", L"Browse"},
        {VK_F3, L"F3", L"Identity"},
        {VK_F4, L"F4", L"Full/Limited"},
        {VK_F6, L"F6", L"Account"},
        {VK_F8, L"F8", L"Allow SYSTEM/TI"},
        {VK_OEM_2, L"/", L"Search"},
        {VK_TAB, L"Tab", L"Next field"},
    };
}

std::wstring RunAsModule::ConsoleHeader() {
    std::wstring s = L"user=" + userName_ + L" elevated=" + (host_->IsElevated() ? L"yes" : L"NO") +
                     L" as=" + WhoKey(who_) + L" token=" + (mode_ == TokenMode::Limited ? L"limited" : L"full") +
                     L" sys/ti=" + (allowElevated_ ? L"ON" : L"off") +
                     L" accounts=" + std::to_wstring(accounts_.size()) + L" recent=" + std::to_wstring(runs_.size());
    if (demo_) s += L" src=demo";
    return s;
}

// ============================================================================================
// Input / focus

void RunAsModule::CycleFocusStops(bool back) {
    struct Stop { Focus f; int id; int data; };
    std::vector<Stop> stops;
    stops.push_back({Focus::Path, 0, 0});
    stops.push_back({Focus::Args, 0, 0});
    stops.push_back({Focus::Work, 0, 0});
    if (!IsRectEmpty(&rcSearch_)) stops.push_back({Focus::Search, 0, 0});
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
    ListView& lv = List();
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    POINT p{x, y};
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (splitDrag_) {
            if (!(w & MK_LBUTTON)) { ReleaseCapture(); return true; }
            SetDetailWidthFromX(x);
            return true;
        }
        if (Edit* e = DragEdit()) {
            if (!(w & MK_LBUTTON)) { e->CancelDrag(); return true; }   // button released elsewhere
            return e->OnMouse(hwnd_, msg, x, y, EditRect(e), c);
        }
        int nid = -1, nd = 0;
        for (int i = (int)clicks_.size() - 1; i >= 0; i--)
            if (PtInRect(&clicks_[i].rc, p)) { nid = clicks_[i].id; nd = clicks_[i].data; break; }
        bool ch = lv.OnMouse(msg, x, y, rcList_, c);
        if (nid != hoverId_ || nd != hoverData_) { hoverId_ = nid; hoverData_ = nd; ch = true; }
        return ch;
    }
    case WM_LBUTTONDOWN: {
        if (PtInRect(&rcSplit_, p)) {
            splitDrag_ = true;
            splitGrab_ = x - rcSplit_.right;
            SetCapture(hwnd_);
            return true;
        }
        if (Edit* e = EditAt(p)) { focus_ = FocusOf(e); e->OnMouse(hwnd_, msg, x, y, EditRect(e), c); return true; }
        if (PtInRect(&rcList_, p)) {
            focus_ = Focus::List;
            lv.OnMouse(msg, x, y, rcList_, c);
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
        if (splitDrag_) { ReleaseCapture(); return true; }   // WM_CAPTURECHANGED saves the width
        if (Edit* e = DragEdit()) return e->OnMouse(hwnd_, msg, x, y, EditRect(e), c);
        return lv.OnMouse(msg, x, y, rcList_, c);
    case WM_LBUTTONDBLCLK: case WM_RBUTTONUP: {
        Edit* e = EditAt(p);
        // A double-click on the history list is its re-fill gesture (the list times the second
        // button-down); anywhere else it is ignored, so a double-click can't run a button twice.
        if (!e) return msg == WM_LBUTTONDBLCLK && PtInRect(&rcList_, p) ? OnInput(WM_LBUTTONDOWN, w, l) : false;
        focus_ = FocusOf(e);
        if (msg == WM_RBUTTONUP) Invalidate();   // show the focus before the menu opens
        e->OnMouse(hwnd_, msg, x, y, EditRect(e), c);
        return true;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        POINT sp{x, y}; ScreenToClient(hwnd_, &sp);
        if (PtInRect(&rcList_, sp)) return lv.OnWheel(delta, rcList_, c);
        return false;
    }
    case WM_CHAR: {
        wchar_t ch = (wchar_t)w;
        if (swallowSlash_) { swallowSlash_ = false; if (ch == L'/') return false; }
        if (Edit* e = FocusedEdit()) return e->OnChar(ch);
        return false;
    }
    case WM_KEYDOWN: {
        WPARAM vk = w;
        swallowSlash_ = false;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        // Function keys work from any focus (and from the Console F-key bar, which sends them here).
        switch (vk) {
        case VK_F8:
            // The SYSTEM/TI switch from the keyboard only where the bar shows it (Console); it is still never persisted.
            if (!host_ || host_->Layout() != Layout::Console) return false;
            [[fallthrough]];
        case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
            if (hwnd_ && GetFocus() != hwnd_) SetFocus(hwnd_);
            if (vk == VK_F5) RunTarget();
            else if (vk == VK_F2) Browse();
            else if (vk == VK_F3) CycleWho();
            else if (vk == VK_F4) OnClickId(ID_MODE, (int)(mode_ == TokenMode::Full ? TokenMode::Limited
                                                                                     : TokenMode::Full));
            else if (vk == VK_F6) OnClickId(ID_ACCOUNT, 0);
            else OnClickId(ID_ALLOW, 0);
            return true;
        }
        Edit* fe = FocusedEdit();
        // '/' jumps to the search box from anywhere but a text field. l == 0 is the F-key bar's click,
        // which also works while a field has focus and is followed by no WM_CHAR.
        if (vk == VK_OEM_2 && !shift && host_->Layout() != Layout::Baseline && (!fe || l == 0)) {
            if (hwnd_ && GetFocus() != hwnd_) SetFocus(hwnd_);
            focus_ = Focus::Search;
            search_.SelectAll();
            swallowSlash_ = l != 0;   // a real key press is followed by WM_CHAR '/'
            return true;
        }
        if (vk == VK_TAB) { CycleFocusStops(shift); return true; }
        if (fe == &search_) {
            if (vk == VK_ESCAPE) { search_.SetText(L""); RefillList(); return true; }
            if (vk == VK_RETURN || vk == VK_DOWN) {
                focus_ = Focus::List;
                if (lv.Selected() < 0 && lv.RowCount() > 0) lv.Select(0);
                return true;
            }
            return search_.OnKey(vk, hwnd_);
        }
        if (fe) {   // path / args / working folder: Enter runs
            if (vk == VK_RETURN) { RunTarget(); return true; }
            return fe->OnKey(vk, hwnd_);
        }
        if (focus_ == Focus::List) {
            if (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
                vk == VK_HOME || vk == VK_END || vk == VK_RETURN) {
                lv.OnKey(vk, rcList_, c);
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
        SplitAccount(L"carol@corp.example", &u, &d); bool s3 = u == L"carol@corp.example" && d.empty();
        check(s1 && s2 && s3, L"SplitAccount: user -> '.', domain\\user split, UPN kept whole with NULL domain");
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
