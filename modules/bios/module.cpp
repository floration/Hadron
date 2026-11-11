// modules/bios/module.cpp — BIOS Settings Explorer (docs/design/bios.md, legacy notes
// docs/legacy/bios-explorer.md). Every setup setting of the board with its live value, default, menu
// path, help, explanation and per-goal advice; snapshots (save / load / compare); export of the changes a
// goal recommends.
//
// Hard rule 2: this module reads UEFI variables and SMBIOS, never flashes and never runs vendor setup
// tools. Its one write path, changing setup values of existing map variables, lives in fwwrite.cpp
// (docs/design/bios-write.md); this file only stages values, shows the plan and starts the Worker that
// runs fwwrite's Execute. The "Allow BIOS writes" switch is a member that starts false and is never
// persisted. Writes need live values, a verified board/BIOS guard and the switch on; the plan overlay
// confirms only by a click on its Write button (Enter never confirms).
//
// Window: like diskmap and runas, the module registers its own view class ("Hadron.bios.View") instead
// of core's CreateModuleContent, because that helper does not forward the worker's completion message,
// WM_MOUSELEAVE or WM_GETDLGCODE. Paint still goes through Module::Paint (WM_PAINT via a back buffer,
// WM_PRINTCLIENT and --screenshot) and input through OnInput.
//
// The settings list is drawn here rather than with core/ui ListView: the legacy list colours cells
// individually (orange changed values, green/red goal advice, grey hidden names, the tier-1 dot, context
// labels), and ListView only takes plain strings in two fixed colours. It keeps ListView's behaviour:
// wheel + draggable core ScrollBar, header-click sort that re-sorts the data, keyboard navigation, a
// focus rect and a high-contrast fallback.
#include "core/host/run.h"
#include "core/host/registry.h"
#include "core/host/cmdline.h"
#include "core/sys/sys.h"
#include "core/sys/worker.h"
#include "core/sys/settings.h"
#include "core/ui/draw.h"
#include "core/ui/widgets.h"
#include "modules/bios/db.h"
#include "modules/bios/firmware.h"
#include "modules/bios/fwwrite.h"
#include "modules/bios/snapshot.h"
#include "modules/bios/zip.h"
#include "core/sys/log.h"

#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <set>
#include <tuple>
#include <string>
#include <vector>

namespace hx {
using namespace hx::ui;
using namespace hx::bios;

namespace {

constexpr UINT WM_BIOS_DONE = WM_MODULE_BASE + 1;       // worker -> view
constexpr UINT WM_BIOS_PROGRESS = WM_MODULE_BASE + 2;   // write worker -> view: wParam done, lParam total
const wchar_t* const kViewClass = L"Hadron.bios.View";
const wchar_t* const kAppTitle = L"BIOS Settings Explorer";
const wchar_t* const kWriteWarning =
    L"Writes change your firmware settings. Hadron saves a backup first and verifies every write. They take "
    L"effect after a reboot. If the PC won't boot, Clear CMOS (JBAT1 jumper, see the board manual) to restore defaults.";

enum ClickId {
    ID_NONE = 0, ID_EXPORT = 1, ID_LOAD = 2, ID_SAVE = 3, ID_BANNER_LOAD = 4, ID_SEARCH = 5, ID_BANNER_TOGGLE = 6,
    ID_WRITE_SWITCH = 7, ID_RESTORE = 8, ID_REVIEW = 9, ID_STAGE_REC = 10, ID_STAGE = 11, ID_UNSTAGE = 12,
    ID_PLAN_CANCEL = 13, ID_PLAN_WRITE = 14, ID_OPEN_BACKUP = 15, ID_INCOMPLETE_RESTORE = 16, ID_NUMEDIT = 17,
    ID_UNSTAGE_ALL = 18, ID_PLAN_SURFACE = 19,
    ID_GOAL = 100,     // + goal + 1 (100 = None)
    ID_FILTER = 200,   // + filter
    ID_OPTION = 300,   // + option index of the selected setting (the "New value" chips)
};
enum Filter { F_ALL, F_KEY, F_CHANGED, F_HIDDEN, F_DIFFERS, F_SNAPDIFF, F_STAGED };
enum class Focus { List, Search, Detail, Edit };
enum class Src { None, Live, Snapshot };
enum class Job { None, Live, Load, Save, Write, RestoreLoad };
enum ColId { C_NAME = 0, C_CUR = 1, C_DEF = 2, C_GOAL = 3, C_SNAP = 4 };

struct Click { RECT rc; int id; };
// tone 0 info, 1 warn, 2 bad. action: a click id for the banner's button (0 = none). Text is capped at
// 3 lines unless the banner is expanded (maxLines 0).
struct Banner {
    std::wstring text; int tone; int action; std::wstring actionLabel; int maxLines;
    Banner(std::wstring t, int tn, int act = 0, std::wstring label = L"", int lines = 3)
        : text(std::move(t)), tone(tn), action(act), actionLabel(std::move(label)), maxLines(lines) {}
};

COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t), (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
int Lum(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }
COLORREF OnColor(COLORREF fill) { return Lum(fill) > 140 ? RGB(10, 20, 30) : RGB(245, 247, 250); }

// Palette over the theme (legacy §5 tokens), with the high-contrast fallback to system colours.
struct Pal {
    COLORREF bg, panel, hover, sel, selText, text, mute, faint, acc, onAcc, warn, bad, good, claim, chip, chipHot;
    explicit Pal(const Theme& t) {
        if (t.highContrast) {
            bg = panel = GetSysColor(COLOR_WINDOW);
            hover = chip = chipHot = GetSysColor(COLOR_BTNFACE);
            sel = GetSysColor(COLOR_HIGHLIGHT); selText = GetSysColor(COLOR_HIGHLIGHTTEXT);
            text = mute = faint = claim = GetSysColor(COLOR_WINDOWTEXT);
            acc = warn = bad = good = GetSysColor(COLOR_HOTLIGHT);
            onAcc = GetSysColor(COLOR_WINDOW);
            return;
        }
        bg = t.bg; panel = t.panel; hover = t.panel2;
        sel = Mix(t.panel, t.accent, 0.26); selText = t.text1;
        text = t.text1; mute = t.text2; faint = t.text3;
        acc = t.accent; onAcc = OnColor(t.accent);
        warn = t.warn; bad = t.bad; good = t.good;
        claim = Mix(t.accent2, t.text1, 0.35);
        chip = t.panel2; chipHot = t.panel3;
    }
};

std::wstring Shell(const std::wstring& p) { return sys::ToShell(p); }

// ============================================================================================
class BiosModule : public Module {
public:
    const ModuleInfo& Info() const override {
        static const ModuleInfo info{L"bios", L"BIOS Settings",   // sidebar name (spec); the view title is kAppTitle
                                     L"Every BIOS setup setting with its live value, default, explanation and "
                                     L"per-goal advice. Snapshots and compare. Guarded writes, off by default.",
                                     Admin::Required};
        return info;
    }
    HWND Create(IHost& host, HWND parent, const RECT& rc) override;
    void OnActivate() override { if (hwnd_) SetFocus(hwnd_); }
    void OnDpiChanged(UINT) override { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    bool PreTranslateMessage(MSG& m) override;
    bool OnInput(UINT msg, WPARAM w, LPARAM l) override;
    void Shutdown() override { worker_.Cancel(); worker_.Join(); }   // a write stops before its next variable
    bool CanClose() override {
        if (job_ != Job::Write) return true;
        if (host_) host_->SetStatus(L"A BIOS write is running; it stops after the current variable");
        return false;
    }
    int  RunCli(int argc, wchar_t** argv) override;
    void LoadFixture(const wchar_t* dir) override;
    void Paint(HDC dc, const RECT& rc) override;

private:
    static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnView(HWND, UINT, WPARAM, LPARAM);

    PaintCtx Ctx() const { return PaintCtx{host_->Theme(), host_->Fonts(), host_->Dpi(), host_->Layout()}; }
    int  Sc(int px) const { return S(px, host_ ? host_->Dpi() : 96); }
    bool Console() const { return host_ && host_->Layout() == Layout::Console; }
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void AddClick(const RECT& r, int id) { clicks_.push_back({r, id}); }

    // data
    bool LoadDb(std::wstring* err);
    void StartLive();
    void StartLoad(const std::wstring& path);
    void StartSave(const std::wstring& parent);
    void OnJobDone();
    void ApplyLive();
    void ApplySnapshot(SnapLoad&& s);
    void CheckGuardLive();
    void CheckGuardSnapshot();
    void Refilter();
    void SortV();
    void SetNotice(const std::wstring& s, int tone) { notice_ = s; noticeTone_ = tone; host_->SetStatus(s.c_str()); }
    std::vector<Banner> Banners() const;
    std::wstring SourceLine() const;

    // actions
    void OnClickId(int id);
    void DoExport();
    void DoLoadDialog();
    void DoSaveDialog();
    void Select(int row, bool center);
    void MoveSel(int delta);

    // writes (docs/design/bios-write.md)
    std::wstring WriteBlock() const;          // why writes can't happen now ("" = they can, switch aside)
    bool WritesPossible() const { return writeSwitch_ && WriteBlock().empty(); }
    bool Idle() const { return job_ == Job::None && !worker_.Running(); }   // no job of any kind running
    PlanGate Gate() const;
    void ToggleWriteSwitch();
    void SyncEditor();                        // reset the "New value" editor when the selection changes
    void OnNumEditChanged();
    bool StagedOverlap(int setting, std::wstring* other) const;
    void StageSelected();
    bool MovePick(int dir);   // Detail focus, Left/Right: the previous/next option chip
    void StageRecommended();
    void ReviewPlan();
    void StartWrite();
    void StartRestoreLoad(const std::wstring& dir);
    void DoRestoreDialog();
    void OpenBackupFolder();
    void RefreshAfterWrite();
    void SetupWriteDemo(const VarMap& vars);  // --screenshot --write-demo: MockBackend over the fixture
    bool PlanInput(UINT msg, WPARAM w, LPARAM l);
    void SelftestWrites(const std::function<void(bool, const std::wstring&)>& check, const std::wstring& root);
    void PaintPlan(HDC dc, const RECT& rc);
    int  PaintPlanList(HDC dc, bool draw);

    // painting
    void Chip(HDC dc, int& x, int y, const std::wstring& label, int id, bool on);
    int  Button(HDC dc, int right, int y, const std::wstring& label, int id, bool primary, bool enabled);
    int  Switch(HDC dc, int right, int y, const std::wstring& label, int id, bool on, bool enabled);
    void PaintHeader(HDC dc, const RECT& rc);
    int  PaintBanners(HDC dc, const RECT& rc);   // returns the y below the last banner
    int  PaintWriteBar(HDC dc, const RECT& rc, int y);
    void PaintList(HDC dc);
    int  PaintDetail(HDC dc, bool draw);
    int  TextW(HDC dc, const std::wstring& s, HFONT f) const;
    int  ColX(int col) const;
    bool HasCol(int col) const;

    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;

    // model
    Db db_;
    bool dbOk_ = false;
    std::wstring dbErr_;
    Src primary_ = Src::None;
    bool compare_ = false;          // a snapshot loaded on top of live values
    LiveResult live_;
    bool liveDone_ = false;
    std::map<VarKey, const FwVar*> liveState_;
    std::wstring snapLabel_, snapSummary_;
    int snapLoaded_ = 0;
    bool unverified_ = false;       // primary values: guard mismatched or couldn't run
    std::wstring guardText_;        // banner for a mismatch
    std::wstring guardUnknown_;     // why the guard could not run (values still marked unverified)
    bool snapUnverified_ = false;   // the compare snapshot: same rule, from its system.txt
    std::wstring snapGuardText_;
    bool bannerExpanded_ = false;
    bool centrePending_ = false;    // a selection made before the first layout still needs centring
    std::wstring selectPrompt_;     // --select <prompt>: applied at the first Refilter that lists it   // the per-variable error banner shows every variable

    // worker
    sys::Worker worker_;
    Job job_ = Job::None;
    LiveResult jobLive_;
    SnapLoad jobSnap_;
    SaveResult jobSave_;
    std::wstring jobPath_;
    ExecResult jobExec_;
    SnapLoad jobBackup_;

    // writes. writeSwitch_ is "Allow BIOS writes": false at every start, never persisted (rule 2).
    bool writeSwitch_ = false;
    std::unique_ptr<FwBackend> backend_;    // RealBackend, or a MockBackend for the screenshot demo
    bool demo_ = false;                     // --screenshot --write-demo: the mock drives everything
    std::wstring demoMode_;                 // "plan" (overlay open) or "edit"
    std::wstring backupRoot_;               // %ProgramData%\Hadron\bios-backups ("" in tests and the demo)
    std::wstring backupShown_;              // the same, as shown in the plan
    std::map<int, long long> staged_;       // setting index -> new value
    int editFor_ = -1;                      // the setting the editor below was set up for
    bool hasPick_ = false; long long pick_ = 0;   // the chip picked for a oneof/checkbox setting
    TextField numEdit_;                     // numeric value
    std::wstring editMsg_; int editTone_ = 0;
    RECT rcNumEdit_{};
    bool planOpen_ = false;
    Plan plan_;
    ScrollBar planSb_;
    RECT rcPlanList_{}, rcPlanSbTrack_{};
    int execDone_ = 0, execTotal_ = 0;
    std::wstring resultText_, resultFolder_; int resultTone_ = 0;
    IncompleteSession incomplete_;
    bool hasIncomplete_ = false;

    // view state
    int goal_ = -1, filter_ = F_ALL;
    int sortCol_ = -1; bool sortAsc_ = true;
    std::vector<int> V_;
    int sel_ = -1;                  // index into db_.S
    SearchBox search_;
    Focus focus_ = Focus::List;
    ScrollBar listSb_, detSb_;
    std::wstring notice_; int noticeTone_ = 0;

    // layout + hit testing (rebuilt each paint)
    RECT rcList_{}, rcDetail_{}, rcSearch_{}, rcListBody_{}, rcListSbTrack_{}, rcDetSbTrack_{}, rcListHdr_{};
    int bannerTop_ = 0;
    std::vector<Click> clicks_;
    int hoverId_ = 0, hoverRow_ = -1, hoverHdr_ = -1;
    bool tracking_ = false;
};

// ============================================================================================
// Window

LRESULT CALLBACK BiosModule::ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = reinterpret_cast<BiosModule*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->OnView(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT BiosModule::OnView(HWND h, UINT m, WPARAM w, LPARAM l) {
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
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
    case WM_SETFOCUS: case WM_KILLFOCUS: Invalidate(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, hoverId_ == ID_SEARCH || hoverId_ == ID_NUMEDIT
                                               ? IDC_IBEAM : (hoverId_ > 0 && hoverId_ != ID_PLAN_SURFACE ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hoverId_ || hoverRow_ >= 0 || hoverHdr_ >= 0) { hoverId_ = 0; hoverRow_ = -1; hoverHdr_ = -1; Invalidate(); }
        return 0;
    case WM_CAPTURECHANGED:
        if (listSb_.Dragging()) listSb_.OnMouse(WM_LBUTTONUP, 0, 0, rcListSbTrack_);
        if (detSb_.Dragging()) detSb_.OnMouse(WM_LBUTTONUP, 0, 0, rcDetSbTrack_);
        if (planSb_.Dragging()) planSb_.OnMouse(WM_LBUTTONUP, 0, 0, rcPlanSbTrack_);
        search_.CancelDrag();
        numEdit_.CancelDrag();
        Invalidate();
        return 0;
    case WM_BIOS_DONE:
        OnJobDone();
        return 0;
    case WM_BIOS_PROGRESS:   // kept out of the WM_LBUTTONDOWN fall-through chain below
        execDone_ = (int)w; execTotal_ = (int)l;
        if (job_ == Job::Write && host_) {
            std::wstring s = L"Writing BIOS settings: variable " + std::to_wstring(std::min(execDone_ + 1, std::max(1, execTotal_))) +
                             L" of " + std::to_wstring(execTotal_) + L"…";
            if (execDone_ < (int)plan_.vars.size()) s += L" (" + plan_.vars[(size_t)execDone_].name + L")";
            host_->SetStatus(s.c_str());
        }
        Invalidate();
        return 0;
    case WM_RBUTTONDOWN:
        SetFocus(h);
        return 0;
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

bool BiosModule::LoadDb(std::wstring* err) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(mod, L"BIOS_SETTINGS", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) { *err = L"The embedded settings map (BIOS_SETTINGS) is missing: " + sys::ErrorText(GetLastError()); return false; }
    DWORD sz = SizeofResource(mod, r);
    HGLOBAL g = LoadResource(mod, r);
    const char* p = g ? (const char*)LockResource(g) : nullptr;
    if (!p || !sz) { *err = L"Could not load the embedded settings map: " + sys::ErrorText(GetLastError()); return false; }
    if (!db_.Parse(std::string(p, sz), err)) return false;
    db_.AddContext();
    return true;
}

HWND BiosModule::Create(IHost& host, HWND parent, const RECT& rc) {
    host_ = &host;
    dbOk_ = LoadDb(&dbErr_);
    if (!dbOk_) host.Log(LogLevel::Error, dbErr_.c_str());
    search_.onChanged = [this](const std::wstring&) { Refilter(); };
    numEdit_.placeholder = L"value";
    numEdit_.onChanged = [this](const std::wstring&) { OnNumEditChanged(); };
    {
        // The real backend may write only the map's variables (it also refuses everything in test runs).
        std::set<VarKey> allowed;
        for (auto& v : db_.Variables()) allowed.insert(MakeVarKey(v.first, v.second));
        backend_ = std::make_unique<RealBackend>(std::move(allowed));
    }
    // Test and fixture runs never touch the real backup folder (the latch is set for exactly those).
    if (!sys::NoFirmwareWrites()) {
        backupRoot_ = DefaultBackupRoot();
        backupShown_ = backupRoot_;
        hasIncomplete_ = dbOk_ && FindIncompleteSession(backupRoot_, &incomplete_);
        if (hasIncomplete_) {
            std::wstring m = L"bios: incomplete write session found: " + incomplete_.dir;
            host.Log(LogLevel::Warn, m.c_str());
        }
    } else {
        backupShown_ = L"%ProgramData%\\Hadron\\bios-backups";
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &BiosModule::ViewProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kViewClass;
        if (!RegisterClassExW(&wc)) {
            std::wstring m = L"bios: RegisterClassEx failed: " + sys::ErrorText(GetLastError());
            host.Log(LogLevel::Error, m.c_str());
            return nullptr;
        }
        registered = true;
    }
    hwnd_ = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        std::wstring m = L"bios: CreateWindowEx failed: " + sys::ErrorText(GetLastError());
        host.Log(LogLevel::Error, m.c_str());
        return nullptr;
    }
    Refilter();

    // --snapshot <dir|zip>: open that snapshot instead of reading this PC. With --screenshot it loads
    // synchronously (deterministic render); otherwise on the worker. No dialog from Create (rule fix #9).
    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    std::wstring snapArg;
    for (size_t i = 0; i + 1 < cl.rest.size(); i++) {
        if (cl.rest[i] == L"--snapshot") snapArg = sys::ToExtended(cl.rest[i + 1]);
        // --goal <1-5>: start with a goal selected (Power saving .. Low latency), for screenshots.
        if (cl.rest[i] == L"--goal") {
            int g = _wtoi(cl.rest[i + 1].c_str());
            if (g >= 1 && g <= kGoalCount) { goal_ = g - 1; Refilter(); }
        }
    }
    std::wstring selectArg;
    for (size_t i = 0; i + 1 < cl.rest.size(); i++) if (cl.rest[i] == L"--select") selectArg = cl.rest[i + 1];
    if (!selectArg.empty()) selectPrompt_ = selectArg;
    // --write-demo plan|edit: only with --screenshot. LoadFixture then builds a MockBackend from the
    // fixture and stages a scripted plan over it; nothing real is read or written.
    if (!cl.screenshot.empty())
        for (size_t i = 0; i + 1 < cl.rest.size(); i++)
            if (cl.rest[i] == L"--write-demo") demoMode_ = cl.rest[i + 1];
    if (!cl.screenshot.empty() || cl.selftest) {
        if (!snapArg.empty()) {
            SnapLoad s;
            if (LoadSnapshot(snapArg, &s)) ApplySnapshot(std::move(s));
            else SetNotice(L"Could not load the snapshot: " + s.error, 2);
            Refilter();
        }
        return hwnd_;
    }
    if (!dbOk_) return hwnd_;
    if (!snapArg.empty()) StartLoad(snapArg);
    else StartLive();
    return hwnd_;
}

bool BiosModule::PreTranslateMessage(MSG& m) {
    // Keys typed while the host frame has focus (standalone start-up) still reach the list.
    if (!hwnd_ || !host_ || m.hwnd != host_->Frame()) return false;
    if (m.message == WM_KEYDOWN && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (!OnInput(m.message, m.wParam, m.lParam)) return false;
        SetFocus(hwnd_);
        Invalidate();
        return true;
    }
    return false;
}

// ============================================================================================
// Data: worker jobs

void BiosModule::StartLive() {
    if (worker_.Running() || !dbOk_) return;
    auto want = db_.Variables();
    LiveResult* out = &jobLive_;
    job_ = Job::Live;
    if (!worker_.Start(hwnd_, WM_BIOS_DONE, [want, out] {
            // An exception must not escape the worker thread (it would end the process).
            try { ReadLive(want, out); }
            catch (const std::bad_alloc&) { *out = LiveResult{}; out->error = L"Out of memory while reading firmware variables."; }
            catch (const std::exception& e) { *out = LiveResult{}; out->error = L"Firmware read failed: " + FromUtf8(e.what()); }
        })) {
        job_ = Job::None;
        SetNotice(L"Could not start the firmware read: " + sys::ErrorText(GetLastError()), 2);
        return;
    }
    host_->SetBusy(true);
    Invalidate();
}

void BiosModule::StartLoad(const std::wstring& path) {
    if (worker_.Running()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    jobPath_ = path;
    SnapLoad* out = &jobSnap_;
    job_ = Job::Load;
    if (!worker_.Start(hwnd_, WM_BIOS_DONE, [path, out] {
            try { LoadSnapshot(path, out); }
            catch (const std::bad_alloc&) { *out = SnapLoad{}; out->error = L"Out of memory while loading the snapshot."; }
            catch (const std::exception& e) { *out = SnapLoad{}; out->error = L"Loading failed: " + FromUtf8(e.what()); }
        })) {
        job_ = Job::None;
        SetNotice(L"Could not start loading the snapshot: " + sys::ErrorText(GetLastError()), 2);
        return;
    }
    host_->SetBusy(true);
    SetNotice(L"Loading snapshot " + Shell(path) + L"…", 0);
    Invalidate();
}

void BiosModule::StartSave(const std::wstring& parent) {
    if (worker_.Running()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    jobPath_ = parent;
    SaveResult* out = &jobSave_;
    sys::Worker* wk = &worker_;
    job_ = Job::Save;
    bool ok = worker_.Start(hwnd_, WM_BIOS_DONE, [parent, out, wk] {
      try {
        *out = SaveResult{};
        ThreadPrivilege priv(L"SeSystemEnvironmentPrivilege");   // this thread only
        std::vector<FwVar> vars;
        std::wstring err;
        if (!EnumerateAll(&vars, &err, [wk] { return wk->Cancelled(); })) {
            if (!priv.ok()) err += L" (SeSystemEnvironmentPrivilege: " + sys::ErrorText(priv.err()) + L")";
            out->error = err;
            return;
        }
        if (wk->Cancelled()) { out->error = L"Cancelled."; return; }
        SmbiosInfo smb;
        DWORD se = 0;
        ReadSmbios(&smb, &se);
        SaveSnapshot(parent, vars, smb, out);
      } catch (const std::bad_alloc&) {
        out->ok = false; out->error = L"Out of memory while saving the snapshot.";
      } catch (const std::exception& e) {
        out->ok = false; out->error = L"Saving failed: " + FromUtf8(e.what());
      }
    });
    if (!ok) {
        job_ = Job::None;
        SetNotice(L"Could not start saving the snapshot: " + sys::ErrorText(GetLastError()), 2);
        return;
    }
    host_->SetBusy(true);
    SetNotice(L"Reading every UEFI variable for the snapshot…", 0);
    Invalidate();
}

void BiosModule::OnJobDone() {
    worker_.Join();   // the thread has posted its last message; synchronise before reading results
    host_->SetBusy(false);
    Job j = job_;
    job_ = Job::None;
    if (j == Job::Live) {
        live_ = std::move(jobLive_);
        liveDone_ = true;
        ApplyLive();
    } else if (j == Job::Load) {
        if (jobSnap_.ok) {
            ApplySnapshot(std::move(jobSnap_));
            SetNotice(L"", 0);
            host_->SetStatus((L"Snapshot loaded: " + snapLabel_).c_str());
        } else {
            SetNotice(L"Could not load the snapshot. " + jobSnap_.error, 2);
        }
    } else if (j == Job::Save) {
        if (jobSave_.ok)
            SetNotice(L"Snapshot saved to " + jobSave_.folder + L" (" + std::to_wstring(jobSave_.read) + L" of " +
                      std::to_wstring(jobSave_.found) + L" variables read).", 0);
        else
            SetNotice(L"Snapshot not saved. " + jobSave_.error +
                      (jobSave_.folder.empty() ? L"" : L" Partial files are in " + jobSave_.folder + L"."), 2);
    } else if (j == Job::Write) {
        planOpen_ = false;
        planSb_.SetOffset(0);
        const ExecResult& r = jobExec_;
        // Written variables: their staged values are now current, so unstage them.
        for (size_t i = 0; i < r.vars.size() && i < plan_.vars.size(); i++)
            if (r.vars[i].state == VarState::Written)
                for (auto& ps : plan_.vars[i].settings) if (ps.setting >= 0) staged_.erase(ps.setting);
        resultText_ = (plan_.restore ? L"Restore: " : L"BIOS write: ") + r.Summary();
        if (r.ok) resultText_ += L" The changes take effect after a reboot.";
        resultTone_ = r.ok ? 0 : 2;
        resultFolder_ = r.backupDir;
        host_->Log(r.ok ? LogLevel::Info : LogLevel::Error, resultText_.c_str());
        notice_.clear();
        host_->SetStatus(r.ok ? L"BIOS write finished and verified." : L"BIOS write stopped; see the banner.");
        // This session is now the newest: its journal decides whether the "didn't finish" banner shows.
        if (!backupRoot_.empty()) hasIncomplete_ = FindIncompleteSession(backupRoot_, &incomplete_);
        if (r.started) RefreshAfterWrite();   // a fresh read so Current shows what the firmware holds now
    } else if (j == Job::RestoreLoad) {
        if (!jobBackup_.ok) {
            SetNotice(L"Restore refused: " + jobBackup_.error + L" Nothing was written.", 2);
        } else {
            std::wstring err;
            if (BuildRestorePlan(db_, jobBackup_.vars, jobBackup_.label, jobBackup_.sysBoard, jobBackup_.sysBios,
                                 live_.vars, Gate(), &plan_, &err)) {
                planOpen_ = true;
                planSb_.SetOffset(0);
                host_->Log(LogLevel::Info, (L"bios: restore planned\r\n" + plan_.Text()).c_str());
                SetNotice(L"", 0);
            } else {
                SetNotice(L"Restore refused: " + err, err.rfind(L"Nothing to restore", 0) == 0 ? 0 : 2);
            }
        }
    }
    Refilter();
    Invalidate();
}

void BiosModule::ApplyLive() {
    liveState_.clear();
    for (auto& v : live_.vars) liveState_[VarKey{v.name, v.guid}] = &v;
    if (live_.readOk == 0) return;   // nothing live: banners explain why; snapshot mode stays available
    VarMap vm;
    for (auto& v : live_.vars) if (v.err == 0) vm[VarKey{v.name, v.guid}] = v.data;
    if (primary_ == Src::Snapshot) {
        // A snapshot was opened first: it becomes the comparison, live becomes primary.
        for (auto& s : db_.S) { s.hasSnap = s.hasCur; s.snap = s.cur; }
        compare_ = true;
        snapUnverified_ = unverified_;
        snapGuardText_ = guardText_.empty() ? guardUnknown_ : guardText_;
    }
    if (primary_ == Src::None) sel_ = -1;
    db_.Apply(vm, /*snapshot=*/false);
    primary_ = Src::Live;
    CheckGuardLive();
}

// Version guard against a snapshot's own system.txt. Returns "" when it matches; otherwise the reason,
// and the values count as unverified (a mismatch, or no record to check against).
std::wstring SnapGuard(const Db& db, const SnapLoad& s, const wchar_t* what) {
    if (db.board.empty()) return L"This settings map has no board/BIOS guard (v1 file). Every value is unverified.";
    if (!s.hasSystem || (s.sysBoard.empty() && s.sysBios.empty()))
        return std::wstring(L"The ") + what + L" doesn't record its board or BIOS version (no system.txt), so the "
               L"map can't be checked against it. Its values are marked unverified.";
    if (!BoardMatches(db.board, s.sysBoard) || !BiosMatches(db.bios, s.sysBios))
        return L"This map was built for " + db.board + L" / " + db.bios + L"; the " + what + L" reports " +
               (s.sysBoard.empty() ? L"?" : s.sysBoard) + L" / " + (s.sysBios.empty() ? L"?" : s.sysBios) +
               L". Its values are unverified.";
    return L"";
}

void BiosModule::ApplySnapshot(SnapLoad&& s) {
    if (primary_ == Src::None) sel_ = -1;   // first values: select the top row, as legacy did at start-up
    snapLabel_ = s.label;
    snapLoaded_ = s.loaded;
    snapSummary_ = s.SkipSummary();
    if (primary_ == Src::Live) {
        db_.Apply(s.vars, /*snapshot=*/true);
        compare_ = true;
        snapGuardText_ = SnapGuard(db_, s, L"compared snapshot");   // reviewer 3.1: the compare side too
        snapUnverified_ = !snapGuardText_.empty();
    } else {
        db_.Apply(s.vars, /*snapshot=*/false);
        db_.ClearValues(true);
        compare_ = false;
        snapUnverified_ = false; snapGuardText_.clear();
        primary_ = Src::Snapshot;
        guardText_.clear(); guardUnknown_.clear();
        std::wstring g = SnapGuard(db_, s, L"snapshot");
        unverified_ = !g.empty();
        if (!s.hasSystem || (s.sysBoard.empty() && s.sysBios.empty())) guardUnknown_ = g;
        else guardText_ = g;
    }
}

void BiosModule::CheckGuardLive() {
    unverified_ = false; guardText_.clear(); guardUnknown_.clear();
    if (db_.board.empty()) { unverified_ = true; guardText_ = L"This settings map has no board/BIOS guard (v1 file). Every value is unverified."; return; }
    const SmbiosInfo& m = live_.smbios;
    if (!m.ok) {
        unverified_ = true;   // reviewer 3.2: a guard that can't run doesn't verify anything
        guardUnknown_ = L"Couldn't read SMBIOS (" + VarErrorText(live_.smbiosErr) +
                        L"), so the board and BIOS version can't be checked against the map. Values are marked unverified.";
        return;
    }
    if (!BoardMatches(db_.board, m.boardProduct) || !BiosMatches(db_.bios, m.biosVersion)) {
        unverified_ = true;
        guardText_ = L"This map was built for " + db_.board + L" / " + db_.bios + L"; your board reports " +
                     (m.boardProduct.empty() ? L"?" : m.boardProduct) + L" / " +
                     (m.biosVersion.empty() ? L"?" : m.biosVersion) + L". Every value is unverified.";
    }
}

// Legacy Refilter (237-263) + SortV (216-236).
void BiosModule::Refilter() {
    V_.clear();
    std::wstring q = Lower(search_.Text());
    if (filter_ == F_DIFFERS && goal_ < 0) filter_ = F_ALL;
    if (filter_ == F_SNAPDIFF && !compare_) filter_ = F_ALL;
    if (filter_ == F_STAGED && staged_.empty()) filter_ = F_ALL;
    if (sortCol_ == C_SNAP && !compare_) sortCol_ = -1;
    for (int i = 0; i < (int)db_.S.size(); i++) {
        const Setting& s = db_.S[i];
        if (filter_ == F_STAGED && !staged_.count(i)) continue;
        if (filter_ == F_KEY && !db_.IsKey(s)) continue;
        if (filter_ == F_CHANGED && !Changed(s)) continue;
        if (filter_ == F_HIDDEN && !s.hidden) continue;
        if (filter_ == F_DIFFERS && !Differs(db_, s, goal_)) continue;
        if (filter_ == F_SNAPDIFF && !SnapDiffers(s)) continue;
        if (!q.empty()) {
            const Kb* k = db_.KbOf(s);
            std::wstring hay = Lower(s.prompt + L" " + s.ctx + L" " + s.path + L" " + s.var + (k ? L" " + k->title : L""));
            if (hay.find(q) == std::wstring::npos) continue;
        }
        V_.push_back(i);
    }
    std::stable_sort(V_.begin(), V_.end(), [this](int a, int b) {
        auto rank = [this](int i) { return (db_.IsKey(db_.S[i]) ? 0 : 2) + (Changed(db_.S[i]) ? 0 : 1); };
        return rank(a) < rank(b);
    });
    SortV();
    if (!selectPrompt_.empty()) {
        for (int i : V_) if (db_.S[i].prompt == selectPrompt_) { sel_ = i; selectPrompt_.clear(); break; }
    }
    int prev = sel_;
    if (sel_ >= 0 && std::find(V_.begin(), V_.end(), sel_) == V_.end()) sel_ = V_.empty() ? -1 : V_[0];
    if (sel_ < 0 && !V_.empty()) sel_ = V_[0];
    if (sel_ != prev) detSb_.SetOffset(0);
    // Legacy recentres: top row 0, or the selection in the middle when it is below the first page.
    listSb_.SetOffset(0);
    auto it = std::find(V_.begin(), V_.end(), sel_);
    if (it != V_.end() && host_ && rcListBody_.bottom > rcListBody_.top) {   // not before the first layout
        int k = (int)(it - V_.begin());
        int pitch = Sc(Console() ? 20 : 26);
        int rows = std::max(1, (int)(rcListBody_.bottom - rcListBody_.top) / std::max(1, pitch));
        listSb_.Configure(rcListBody_.bottom - rcListBody_.top, (int)V_.size() * pitch);
        if (k >= rows) listSb_.SetOffset((k - rows / 2) * pitch);
    } else if (it != V_.end() && it != V_.begin()) {
        centrePending_ = true;   // --select before the first layout: centre it once the list has a size
    }
}

void BiosModule::SortV() {
    if (sortCol_ < 0) return;
    const int col = sortCol_;
    const bool asc = sortAsc_;
    const int goal = goal_;
    const Db& db = db_;
    std::stable_sort(V_.begin(), V_.end(), [&](int a, int b) {
        const Setting &x = db.S[a], &y = db.S[b];
        int c = 0;
        if (col == C_NAME) c = ICmp(x.prompt, y.prompt);
        else if (col == C_CUR || col == C_DEF || col == C_SNAP) {
            auto has = [col](const Setting& s) { return col == C_CUR ? s.hasCur : col == C_DEF ? s.hasDef : s.hasSnap; };
            auto val = [col](const Setting& s) { return col == C_CUR ? s.cur : col == C_DEF ? s.def : s.snap; };
            bool hx = has(x), hy = has(y);
            if (hx != hy) return hx;   // missing values always last, both directions
            if (!hx) return false;
            std::wstring tx = ValText(x, val(x)), ty = ValText(y, val(y));
            bool nx = !tx.empty() && iswdigit(tx[0]), ny = !ty.empty() && iswdigit(ty[0]);
            long long vx = val(x), vy = val(y);
            if (nx && ny) c = vx < vy ? -1 : (vx > vy ? 1 : 0);
            else if (nx != ny) c = nx ? 1 : -1;
            else c = ICmp(tx, ty);
        } else {
            int rx = GoalRank(db, x, goal), ry = GoalRank(db, y, goal);
            if ((rx == 4) != (ry == 4)) return ry == 4;   // no advice always last
            c = rx - ry;
            if (!c) c = ICmp(x.prompt, y.prompt);
        }
        return asc ? c < 0 : c > 0;
    });
}

std::wstring BiosModule::SourceLine() const {
    if (primary_ == Src::Live) {
        std::wstring s = L"Live values read from this PC";
        if (compare_) s += L"   •   compared with snapshot " + snapLabel_;
        return s;
    }
    if (primary_ == Src::Snapshot) return L"Snapshot: " + snapLabel_;
    if (job_ == Job::Live) return L"Reading firmware variables…";
    return L"No values loaded - use Load snapshot…";
}

std::vector<Banner> BiosModule::Banners() const {
    std::vector<Banner> b;
    if (!dbOk_) { b.push_back({dbErr_, 2, 0}); return b; }
    if (writeSwitch_) {
        std::wstring t = kWriteWarning;
        std::wstring why = WriteBlock();
        if (!why.empty()) t += L"\nWrites are blocked: " + why;
        b.push_back({t, 2, 0, L"", 4});
    }
    if (!resultText_.empty())
        b.push_back({resultText_, resultTone_, resultFolder_.empty() ? 0 : ID_OPEN_BACKUP, L"Open backup folder", 4});
    if (hasIncomplete_)
        b.push_back({L"A BIOS write on " + incomplete_.stamp + L" didn't finish (" +
                     std::to_wstring(incomplete_.vars.size()) + L" variable" + (incomplete_.vars.size() == 1 ? L"" : L"s") +
                     L" without a verified result). Restore that backup? Hadron shows the plan first.",
                     2, ID_INCOMPLETE_RESTORE, L"Restore…"});
    if (!notice_.empty()) b.push_back({notice_, noticeTone_, 0});
    if (!guardText_.empty()) b.push_back({guardText_, 2, 0});
    if (liveDone_ && !live_.error.empty()) b.push_back({live_.error, 2, 0});
    else if (liveDone_) {
        if (!live_.elevated)
            b.push_back({L"Not running as administrator, so live values can't be read (the manifest asks for "
                         L"admin; something ignored it). Load a snapshot instead.", 1, 0});
        else if (!live_.uefi)
            b.push_back({L"This PC reports firmware type \"" + live_.fwType + L"\": there are no UEFI variables to read "
                         L"(a legacy-BIOS boot, or Wine). Snapshot mode: load a snapshot to see values.", 1, 0});
        else {
            if (!live_.privOk)
                b.push_back({L"SeSystemEnvironmentPrivilege could not be enabled (" + sys::ErrorText(live_.privErr) +
                             L"). Firmware variable reads will fail with error 1314.", 1, 0});
            int bad = (int)live_.vars.size() - live_.readOk;
            if (bad > 0) {
                std::wstring t = std::to_wstring(bad) + L" of " + std::to_wstring(live_.vars.size()) +
                                 L" variables couldn't be read; settings stored in them show \"-\" (select one to "
                                 L"see why).";
                if (bannerExpanded_) {
                    for (auto& v : live_.vars)
                        if (v.err) t += L"\n\u2022 " + v.name + L" {" + v.guid + L"}: " + VarErrorText(v.err);
                } else {
                    for (auto& v : live_.vars) if (v.err) { t += L" First: " + v.name + L": " + VarErrorText(v.err) + L"."; break; }
                }
                b.push_back({t, live_.readOk ? 1 : 2, ID_BANNER_TOGGLE, bannerExpanded_ ? L"Hide list" : L"Show all",
                             bannerExpanded_ ? 0 : 2});
            }
        }
    }
    if (primary_ == Src::Snapshot) {
        std::wstring t = L"Snapshot mode: values come from " + snapLabel_ + L", not from this PC (" +
                         std::to_wstring(snapLoaded_) + L" variables).";
        if (!snapSummary_.empty()) t += L" " + snapSummary_ + L".";
        b.push_back({t, 0, 0});
    } else if (compare_) {
        std::wstring t = L"Comparing with snapshot " + snapLabel_ + L": its values are in the Snapshot column; "
                         L"\"Snapshot differs\" lists what changed.";
        if (!snapSummary_.empty()) t += L" " + snapSummary_ + L".";
        b.push_back({t, 0, 0});
    }
    if (!guardUnknown_.empty()) b.push_back({guardUnknown_, 1, 0});
    if (compare_ && !snapGuardText_.empty()) b.push_back({snapGuardText_, 1, 0});
    if (primary_ == Src::None && job_ != Job::Live && job_ != Job::Load)
        b.push_back({L"No values loaded. Every setting is listed with its default; load a snapshot (a UEFI_Dump "
                     L"folder or its .zip) to see values.", 1, ID_BANNER_LOAD, L"Load snapshot…"});
    return b;
}

// ============================================================================================
// Actions

void BiosModule::OnClickId(int id) {
    if (id == ID_EXPORT) DoExport();
    else if (id == ID_LOAD || id == ID_BANNER_LOAD) DoLoadDialog();
    else if (id == ID_SAVE) DoSaveDialog();
    else if (id == ID_BANNER_TOGGLE) bannerExpanded_ = !bannerExpanded_;
    else if (id == ID_SEARCH) focus_ = Focus::Search;
    else if (id == ID_WRITE_SWITCH) ToggleWriteSwitch();
    else if (id == ID_RESTORE) DoRestoreDialog();
    else if (id == ID_REVIEW) ReviewPlan();
    else if (id == ID_STAGE_REC) StageRecommended();
    else if (id == ID_STAGE) StageSelected();
    else if (id == ID_UNSTAGE) { if (sel_ >= 0) staged_.erase(sel_); editFor_ = -1; Refilter(); }
    else if (id == ID_UNSTAGE_ALL) {
        if (job_ == Job::Write) return;
        size_t n = staged_.size();
        staged_.clear();
        editFor_ = -1;
        Refilter();
        SetNotice(L"Nothing staged.", 0);
        host_->Log(LogLevel::Info, (L"bios: unstaged all (" + std::to_wstring(n) + L" staged change" + (n == 1 ? L")" : L"s)")).c_str());
    }
    else if (id == ID_OPEN_BACKUP) OpenBackupFolder();
    else if (id == ID_NUMEDIT) focus_ = Focus::Edit;
    else if (id == ID_INCOMPLETE_RESTORE) {
        if (!WritesPossible())
            SetNotice(L"To restore that backup, turn on \"Allow BIOS writes\" (it needs live values and a verified "
                      L"board/BIOS guard), then press Restore… again.", 1);
        else StartRestoreLoad(incomplete_.dir);
    } else if (id >= ID_OPTION && sel_ >= 0 && id < ID_OPTION + 2 + (int)db_.S[sel_].opts.size()) {
        const Setting& s = db_.S[sel_];
        int k = id - ID_OPTION;
        if (s.kind == L"checkbox" && s.opts.empty()) pick_ = k;   // chips 0 and 1
        else if (k < (int)s.opts.size()) pick_ = s.opts[(size_t)k].v;
        hasPick_ = true;
        editMsg_.clear();
    }
    else if (id >= ID_GOAL && id < ID_GOAL + 1 + kGoalCount) {
        goal_ = id - ID_GOAL - 1;
        if (goal_ < 0) {
            if (filter_ == F_DIFFERS) filter_ = F_ALL;
            if (sortCol_ == C_GOAL) sortCol_ = -1;
        }
        Refilter();
    } else if (id >= ID_FILTER && id <= ID_FILTER + F_STAGED) {
        filter_ = id - ID_FILTER;
        Refilter();
    }
}

void BiosModule::DoExport() {
    if (goal_ < 0) { SetNotice(L"Pick a goal first (Power saving, Balanced, ...).", 1); return; }
    std::wstring def = std::wstring(L"BIOS changes - ") + kGoalNames[goal_] + L".txt";
    std::vector<wchar_t> file(32768, 0);
    std::copy(def.begin(), def.end(), file.begin());
    std::wstring initDir = host_->Prefs().GetStr(L"exportDir");
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host_->Frame();
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrTitle = L"Export BIOS changes";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) {
        DWORD e = CommDlgExtendedError();
        if (e) SetNotice(L"The save dialog failed (error " + std::to_wstring(e) + L").", 2);
        return;
    }
    std::wstring path = file.data();
    host_->Prefs().SetStr(L"exportDir", path.substr(0, path.find_last_of(L"\\/")));
    std::wstring unv = !unverified_ ? L"" : (guardText_.empty() ? guardUnknown_ : guardText_);
    if (compare_ && snapUnverified_) unv += (unv.empty() ? L"" : L" ") + snapGuardText_;
    std::string data = "\xEF\xBB\xBF" + ToUtf8(ExportText(db_, goal_, SourceLine(), unv));
    // The user confirmed any overwrite in the dialog (OFN_OVERWRITEPROMPT), so CREATE_ALWAYS is theirs.
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetNotice(L"Could not write " + path + L": " + sys::ErrorText(GetLastError()), 2); return; }
    DWORD put = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    DWORD e = ok ? 0 : GetLastError();
    if (!CloseHandle(h) && ok) { ok = FALSE; e = GetLastError(); }
    if (!ok) { SetNotice(L"Could not write " + path + L": " + sys::ErrorText(e), 2); return; }
    SetNotice(L"Exported to " + path, 0);
    HINSTANCE r = ShellExecuteW(host_->Frame(), L"open", Shell(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) SetNotice(L"Exported to " + path + L" (couldn't open it: error " + std::to_wstring((INT_PTR)r) + L")", 1);
}

void BiosModule::DoLoadDialog() {
    if (worker_.Running()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    std::vector<wchar_t> file(32768, 0);
    std::wstring initDir = host_->Prefs().GetStr(L"snapshotDir");
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host_->Frame();
    ofn.lpstrFilter = L"UEFI snapshot (index.csv or .zip)\0index.csv;*.zip\0All files\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.lpstrTitle = L"Open a snapshot: the index.csv inside a UEFI_Dump folder, or its .zip";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) {
        DWORD e = CommDlgExtendedError();
        if (e) SetNotice(L"The open dialog failed (error " + std::to_wstring(e) + L").", 2);
        return;
    }
    std::wstring path = file.data();
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    host_->Prefs().SetStr(L"snapshotDir", dir);
    StartLoad(sys::ToExtended(path));
}

void BiosModule::DoSaveDialog() {
    if (worker_.Running()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg);
    if (FAILED(hr) || !dlg) { SetNotice(L"Could not open the folder picker: " + sys::ErrorText((DWORD)hr), 2); return; }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose where to save the snapshot (a new UEFI_Dump_<date> folder is created there)");
    std::wstring path;
    hr = dlg->Show(host_->Frame());
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        SetNotice(L"The folder picker failed: " + sys::ErrorText((DWORD)hr), 2);
    }
    dlg->Release();
    if (path.empty()) return;
    StartSave(sys::ToExtended(path));
}

void BiosModule::Select(int row, bool center) {
    if (V_.empty()) return;
    row = std::clamp(row, 0, (int)V_.size() - 1);
    if (V_[row] != sel_) detSb_.SetOffset(0);   // legacy: a new selection resets the detail scroll
    sel_ = V_[row];
    int pitch = Sc(Console() ? 20 : 26);
    int viewH = rcListBody_.bottom - rcListBody_.top;
    listSb_.Configure(viewH, (int)V_.size() * pitch);
    int top = row * pitch, bot = top + pitch;
    if (center) listSb_.SetOffset(top - viewH / 2);
    else if (top < listSb_.Offset()) listSb_.SetOffset(top);
    else if (bot > listSb_.Offset() + viewH) listSb_.SetOffset(bot - viewH);
}

void BiosModule::MoveSel(int delta) {
    auto it = std::find(V_.begin(), V_.end(), sel_);
    int k = it == V_.end() ? 0 : (int)(it - V_.begin());
    Select(it == V_.end() ? 0 : k + delta, false);
}

// ============================================================================================
// Writes (docs/design/bios-write.md). The UI side: gate, switch, staging, plan, job wiring.

std::wstring BiosModule::WriteBlock() const {
    if (!dbOk_) return L"the settings map didn't load.";
    if (!backend_) return L"no firmware access.";
    if (backend_->IsReal() && sys::NoFirmwareWrites())
        return L"this is a test or fixture run (--selftest, --screenshot, --fixture or --snapshot), where firmware writes are off.";
    if (primary_ == Src::Snapshot) return L"snapshot mode: the values come from a snapshot, not from this PC.";
    if (primary_ != Src::Live || !liveDone_ || live_.readOk == 0) return L"not live: no firmware variables were read from this PC.";
    if (!live_.elevated) return L"Hadron isn't running elevated.";
    if (!live_.privOk) return L"SeSystemEnvironmentPrivilege couldn't be enabled (" + sys::ErrorText(live_.privErr) + L").";
    if (!guardText_.empty()) return L"the board/BIOS guard doesn't match this PC.";
    if (unverified_ || !guardUnknown_.empty()) return L"the board/BIOS guard couldn't run, so the map is unverified.";
    if (job_ == Job::Write) return L"a write is running.";
    return L"";
}

PlanGate BiosModule::Gate() const {
    PlanGate g;
    g.live = primary_ == Src::Live && liveDone_ && live_.readOk > 0 && live_.elevated && live_.privOk &&
             !(backend_ && backend_->IsReal() && sys::NoFirmwareWrites());
    g.guardMatched = !unverified_ && guardText_.empty() && guardUnknown_.empty() && !db_.board.empty();
    g.switchOn = writeSwitch_;
    return g;
}

void BiosModule::ToggleWriteSwitch() {
    // Never persisted: no Prefs() call here or anywhere for this switch (rule 2).
    if (writeSwitch_) {
        writeSwitch_ = false;
        planOpen_ = false;
        host_->Log(LogLevel::Info, L"bios: \"Allow BIOS writes\" switched off");
        SetNotice(L"", 0);
        return;
    }
    if (job_ == Job::Write) return;
    if (backend_ && backend_->IsReal() && sys::NoFirmwareWrites()) {
        SetNotice(L"\"Allow BIOS writes\" can't be turned on in a test or fixture run.", 1);
        return;
    }
    writeSwitch_ = true;
    editFor_ = -1;
    host_->Log(LogLevel::Info, L"bios: \"Allow BIOS writes\" switched on");
}

void BiosModule::SyncEditor() {
    if (editFor_ == sel_) return;
    editFor_ = sel_;
    hasPick_ = false;
    editMsg_.clear();
    editTone_ = 0;
    if (sel_ < 0 || sel_ >= (int)db_.S.size()) { numEdit_.SetText(L""); return; }
    const Setting& s = db_.S[sel_];
    auto st = staged_.find(sel_);
    if (st != staged_.end()) { hasPick_ = true; pick_ = st->second; }
    numEdit_.SetText(st != staged_.end() ? std::to_wstring(st->second) : (s.hasCur ? std::to_wstring(s.cur) : L""));
}

// The values of the "New value" chips, in display order (a checkbox without options: 0 and 1).
std::vector<long long> ChipValues(const Setting& s) {
    std::vector<long long> v;
    if (s.kind == L"numeric") return v;
    if (s.kind == L"checkbox" && s.opts.empty()) return {0, 1};
    for (auto& o : s.opts) v.push_back(o.v);
    return v;
}

bool BiosModule::MovePick(int dir) {
    if (!WritesPossible() || sel_ < 0 || sel_ >= (int)db_.S.size()) return false;
    const Setting& s = db_.S[sel_];
    std::vector<long long> chips = ChipValues(s);
    if (!s.hasCur || chips.empty()) return false;
    SyncEditor();
    long long from = hasPick_ ? pick_ : s.cur;
    int at = -1;
    for (int k = 0; k < (int)chips.size(); k++) if (chips[(size_t)k] == from) { at = k; break; }
    int to = at < 0 ? (dir > 0 ? 0 : (int)chips.size() - 1) : std::clamp(at + dir, 0, (int)chips.size() - 1);
    pick_ = chips[(size_t)to];
    hasPick_ = true;
    editMsg_.clear();
    return true;
}

// Whole numbers only (decimal); anything else is refused, there is no hex or byte entry.
bool ParseWhole(const std::wstring& t, long long* v) {
    size_t a = 0, b = t.size();
    while (a < b && t[a] == L' ') a++;
    while (b > a && t[b - 1] == L' ') b--;
    if (a == b || b - a > 19) return false;
    unsigned long long u = 0;
    for (size_t i = a; i < b; i++) {
        if (t[i] < L'0' || t[i] > L'9') return false;
        u = u * 10 + (unsigned)(t[i] - L'0');
    }
    if (u > (unsigned long long)LLONG_MAX) return false;
    *v = (long long)u;
    return true;
}

void BiosModule::OnNumEditChanged() {
    editMsg_.clear();
    editTone_ = 0;
    if (sel_ < 0) return;
    const Setting& s = db_.S[sel_];
    long long v = 0;
    if (numEdit_.Text().empty()) return;
    if (!ParseWhole(numEdit_.Text(), &v)) { editMsg_ = L"Enter a whole number."; editTone_ = 2; return; }
    std::wstring why;
    uint8_t enc[8];
    if (!ValidateValue(s, v, &why)) { editMsg_ = L"Not allowed: " + why + L"."; editTone_ = 2; return; }
    if (!EncodeValue(v, s.width, enc)) { editMsg_ = L"Doesn't fit in " + std::to_wstring(s.width) + L" bytes."; editTone_ = 2; return; }
    editMsg_ = s.hasCur && v == s.cur ? L"That is the current value." : L"OK: " + ValText(s, v);
}

bool BiosModule::StagedOverlap(int setting, std::wstring* other) const {
    const Setting& s = db_.S[setting];
    for (auto& st : staged_) {
        if (st.first == setting) continue;
        const Setting& o = db_.S[st.first];
        if (o.var == s.var && Lower(o.guid) == Lower(s.guid) && o.offset < s.offset + s.width && s.offset < o.offset + o.width) {
            if (other) *other = o.prompt;
            return true;
        }
    }
    return false;
}

void BiosModule::StageSelected() {
    if (!WritesPossible() || sel_ < 0 || sel_ >= (int)db_.S.size()) return;
    const Setting& s = db_.S[sel_];
    long long v = 0;
    if (s.kind == L"numeric") {
        if (!ParseWhole(numEdit_.Text(), &v)) { editMsg_ = L"Enter a whole number."; editTone_ = 2; return; }
    } else {
        if (!hasPick_) { editMsg_ = L"Pick a value first."; editTone_ = 1; return; }
        v = pick_;
    }
    std::wstring why, other;
    uint8_t enc[8];
    if (!s.hasCur) { editMsg_ = L"No current value was read for this setting, so it can't be changed."; editTone_ = 2; return; }
    if (!ValidateValue(s, v, &why)) { editMsg_ = L"Not allowed: " + why + L"."; editTone_ = 2; return; }
    if (!EncodeValue(v, s.width, enc)) { editMsg_ = L"Doesn't fit in " + std::to_wstring(s.width) + L" bytes."; editTone_ = 2; return; }
    if (v == s.cur) {
        staged_.erase(sel_);
        editMsg_ = L"That is the current value; nothing staged.";
        editTone_ = 0;
        Refilter();
        return;
    }
    if (StagedOverlap(sel_, &other)) {
        editMsg_ = L"Stored in the same bytes as the staged \"" + other + L"\"; unstage that first.";
        editTone_ = 2;
        return;
    }
    staged_[sel_] = v;
    editMsg_.clear();
    std::wstring m = L"bios: staged " + s.prompt + L" (" + s.var + L" + " + std::to_wstring(s.offset) + L"): " +
                     ValText(s, s.cur) + L" -> " + ValText(s, v);
    host_->Log(LogLevel::Info, m.c_str());
    Refilter();
}

void BiosModule::StageRecommended() {
    if (!WritesPossible()) { SetNotice(L"Writes are blocked: " + WriteBlock(), 1); return; }
    if (!Idle()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    if (goal_ < 0) { SetNotice(L"Pick a goal first; \"Stage recommended\" stages that goal's exact values.", 1); return; }
    int n = 0, skipped = 0;
    for (int i = 0; i < (int)db_.S.size(); i++) {
        const Setting& s = db_.S[i];
        long long v = 0;
        if (!s.hasCur || !RecVal(db_, s, goal_, &v) || v == s.cur) continue;   // exact values only, never advice text
        uint8_t enc[8];
        if (!ValidateValue(s, v, nullptr) || !EncodeValue(v, s.width, enc) || StagedOverlap(i, nullptr)) { skipped++; continue; }
        staged_[i] = v;
        n++;
    }
    editFor_ = -1;
    std::wstring m = L"Staged " + std::to_wstring(n) + L" recommended change" + (n == 1 ? L"" : L"s") + L" for " +
                     kGoalNames[goal_] + L".";
    if (skipped) m += L" " + std::to_wstring(skipped) + L" skipped: stored in the same bytes as another staged setting, or "
                      L"the value isn't allowed.";
    if (n) m += L" Review them with \"Review and write…\".";
    SetNotice(m, 0);
    host_->Log(LogLevel::Info, (L"bios: " + m).c_str());
    if (n) { filter_ = F_STAGED; }
    Refilter();
}

void BiosModule::ReviewPlan() {
    if (!WritesPossible()) { SetNotice(L"Writes are blocked: " + WriteBlock(), 1); return; }
    if (!Idle()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    std::vector<StagedChange> st;
    for (auto& s : staged_) st.push_back({s.first, s.second});
    std::wstring err;
    if (!BuildPlan(db_, st, live_.vars, Gate(), &plan_, &err)) { SetNotice(L"No plan: " + err, 2); return; }
    planOpen_ = true;
    planSb_.SetOffset(0);
    host_->Log(LogLevel::Info, (L"bios: write planned\r\n" + plan_.Text()).c_str());
}

void BiosModule::StartWrite() {
    if (!planOpen_ || plan_.vars.empty()) { SetNotice(L"There is no plan to write. Nothing was written.", 1); return; }
    if (!Idle()) {
        SetNotice(job_ == Job::Write ? L"A BIOS write is already running." : L"Still busy with the previous job; press Write again in a moment. Nothing was written.", 1);
        return;
    }
    // The gate again, right before starting (the plan was built under it; nothing may have changed).
    if (!WritesPossible()) { SetNotice(L"Writes are blocked: " + WriteBlock(), 2); planOpen_ = false; return; }
    auto plan = std::make_shared<Plan>(plan_);   // the worker's own copy
    FwBackend* be = backend_.get();
    std::wstring root = backupRoot_;
    ExecResult* out = &jobExec_;
    sys::Worker* wk = &worker_;
    HWND hw = hwnd_;
    job_ = Job::Write;
    execDone_ = 0;
    execTotal_ = (int)plan->vars.size();
    resultText_.clear();
    host_->Log(LogLevel::Info, (L"bios: write confirmed by the user\r\n" + plan->Text()).c_str());
    bool ok = worker_.Start(hwnd_, WM_BIOS_DONE, [plan, be, root, out, wk, hw] {
        try {
            Execute(*be, *plan, root, [wk] { return wk->Cancelled(); },
                    [hw](int done, int total, const std::wstring&) { PostMessageW(hw, WM_BIOS_PROGRESS, (WPARAM)done, (LPARAM)total); },
                    out);
        } catch (const std::bad_alloc&) {
            out->ok = false; out->error = L"Out of memory during the write; check the journal in the backup folder.";
        } catch (const std::exception& e) {
            out->ok = false; out->error = L"The write stopped: " + FromUtf8(e.what()) + L"; check the journal in the backup folder.";
        }
    });
    if (!ok) {
        job_ = Job::None;
        SetNotice(L"Could not start the write: " + sys::ErrorText(GetLastError()) + L" Nothing was written.", 2);
        return;
    }
    host_->SetBusy(true);
    host_->SetStatus(L"Writing BIOS settings…");
}

void BiosModule::StartRestoreLoad(const std::wstring& dir) {
    if (worker_.Running()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    if (!WritesPossible()) { SetNotice(L"Writes are blocked: " + WriteBlock(), 1); return; }
    SnapLoad* out = &jobBackup_;
    std::wstring path = sys::ToExtended(dir);
    std::wstring root = backupRoot_;
    job_ = Job::RestoreLoad;
    // Only Hadron's own sessions under the backup root, every byte checked against manifest.txt.
    if (!worker_.Start(hwnd_, WM_BIOS_DONE, [path, root, out] {
            try { LoadBackupSession(root, path, out); }
            catch (const std::bad_alloc&) { *out = SnapLoad{}; out->error = L"Out of memory while reading the backup."; }
            catch (const std::exception& e) { *out = SnapLoad{}; out->error = L"Reading the backup failed: " + FromUtf8(e.what()); }
        })) {
        job_ = Job::None;
        SetNotice(L"Could not start reading the backup: " + sys::ErrorText(GetLastError()), 2);
        return;
    }
    host_->SetBusy(true);
    SetNotice(L"Reading backup " + Shell(path) + L"…", 0);
}

void BiosModule::DoRestoreDialog() {
    if (!WritesPossible()) { SetNotice(L"Writes are blocked: " + WriteBlock(), 1); return; }
    if (worker_.Running()) { SetNotice(L"Still busy with the previous job; try again in a moment.", 1); return; }
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg);
    if (FAILED(hr) || !dlg) { SetNotice(L"Could not open the folder picker: " + sys::ErrorText((DWORD)hr), 2); return; }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose a BIOS backup folder to restore (a <date-time> folder under bios-backups)");
    if (!backupRoot_.empty()) {
        IShellItem* def = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(Shell(backupRoot_).c_str(), nullptr, IID_PPV_ARGS(&def))) && def) {
            dlg->SetFolder(def);
            def->Release();
        }
    }
    std::wstring path;
    hr = dlg->Show(host_->Frame());
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        SetNotice(L"The folder picker failed: " + sys::ErrorText((DWORD)hr), 2);
    }
    dlg->Release();
    if (!path.empty()) StartRestoreLoad(path);
}

void BiosModule::OpenBackupFolder() {
    if (resultFolder_.empty()) return;
    HINSTANCE r = ShellExecuteW(host_->Frame(), L"open", Shell(resultFolder_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) SetNotice(L"Couldn't open " + Shell(resultFolder_) + L" (error " + std::to_wstring((INT_PTR)r) + L").", 1);
}

void BiosModule::RefreshAfterWrite() {
    if (!demo_) { StartLive(); return; }
    // The demo: read the mock the way ReadLive reads the firmware, synchronously.
    LiveResult lr;
    lr.elevated = true; lr.uefi = true; lr.fwType = L"UEFI"; lr.privOk = true;
    backend_->Smbios(&lr.smbios, &lr.smbiosErr);
    for (auto& w : db_.Variables()) {
        FwVar v;
        v.name = w.first; v.guid = w.second;
        backend_->Read(&v);
        if (v.err == 0) lr.readOk++;
        lr.vars.push_back(std::move(v));
    }
    live_ = std::move(lr);
    liveDone_ = true;
    ApplyLive();
}

void BiosModule::SetupWriteDemo(const VarMap& vars) {
    // --screenshot --write-demo: a MockBackend holding the fixture's bytes, with SMBIOS that matches the
    // map, so the gate opens and a scripted plan can be shown. Nothing here can reach the firmware.
    auto mock = std::make_unique<MockBackend>();
    for (auto& v : vars) mock->Put(v.first.first, v.first.second, v.second, kAttrRequired);
    mock->smbios.ok = true;
    mock->smbios.boardMaker = L"Micro-Star International Co., Ltd.";
    mock->smbios.boardProduct = L"PRO Z790-P WIFI (MS-7E06)";
    mock->smbios.biosVendor = L"American Megatrends International, LLC.";
    mock->smbios.biosVersion = L"A.F0";
    backend_ = std::move(mock);
    demo_ = true;
    backupRoot_.clear();   // the demo never executes; an empty root makes Execute refuse anyway
    RefreshAfterWrite();
    writeSwitch_ = true;
    auto find = [&](const wchar_t* prompt) {
        for (int i = 0; i < (int)db_.S.size(); i++) if (db_.S[i].prompt == prompt) return i;
        return -1;
    };
    auto opt = [&](int i, const wchar_t* text) {
        if (i < 0) return;
        for (auto& o : db_.S[i].opts) if (o.t == text) { staged_[i] = o.v; return; }
    };
    opt(find(L"Hyper-Threading"), L"Enabled");
    opt(find(L"Intel Turbo Boost"), L"Enabled");
    opt(find(L"CSM Support"), L"Disabled");
    int icc = find(L"IA ICC Max Current Limit Override");
    if (icc >= 0) staged_[icc] = 400;
    sel_ = icc;
    selectPrompt_.clear();
    filter_ = F_STAGED;
    Refilter();
    if (demoMode_ == L"plan") ReviewPlan();
}

// ============================================================================================
// Input

bool BiosModule::OnInput(UINT msg, WPARAM w, LPARAM l) {
    if (!host_) return false;
    PaintCtx c = Ctx();
    if (planOpen_) return PlanInput(msg, w, l);   // the plan overlay is modal inside the view
    if (focus_ == Focus::Edit && IsRectEmpty(&rcNumEdit_)) focus_ = Focus::Detail;   // the field isn't shown
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP ||
        msg == WM_RBUTTONUP) {
        // The search box and the value field take text selection, dragging and the right-click edit menu.
        int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
        POINT mp{mx, my};
        if (search_.Dragging() && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP)) {
            search_.OnMouse(hwnd_, msg, mx, my, rcSearch_, c);
            return true;
        }
        if (numEdit_.Dragging() && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP)) {
            numEdit_.OnMouse(hwnd_, msg, mx, my, rcNumEdit_, c);
            return true;
        }
        if (msg != WM_MOUSEMOVE && msg != WM_LBUTTONUP && PtInRect(&rcSearch_, mp)) {
            focus_ = Focus::Search;
            search_.OnMouse(hwnd_, msg, mx, my, rcSearch_, c);
            return true;
        }
        if (msg != WM_MOUSEMOVE && msg != WM_LBUTTONUP && PtInRect(&rcNumEdit_, mp)) {
            focus_ = Focus::Edit;
            numEdit_.OnMouse(hwnd_, msg, mx, my, rcNumEdit_, c);
            return true;
        }
        if (msg == WM_LBUTTONDBLCLK) msg = WM_LBUTTONDOWN;
        if (msg == WM_RBUTTONUP) return false;
    }
    if (msg == WM_KEYDOWN) {
        // AltGr reports Ctrl+Alt: not a Ctrl shortcut.
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
        int pitch = Sc(Console() ? 20 : 26);
        int page = std::max(1, (int)(rcListBody_.bottom - rcListBody_.top) / pitch - 1);
        if (w == VK_TAB) {
            // List -> Search -> Detail -> the value field (only while it is shown) -> List.
            bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int n = IsRectEmpty(&rcNumEdit_) ? 3 : 4;
            focus_ = (Focus)(((int)focus_ + (back ? n - 1 : 1)) % n);
            return true;
        }
        if (ctrl && w == 'F') { focus_ = Focus::Search; return true; }
        if (w == VK_ESCAPE) {
            if (focus_ == Focus::Edit) { focus_ = Focus::Detail; return true; }
            if (!search_.Text().empty()) { search_.SetText(L""); Refilter(); return true; }
            if (focus_ != Focus::List) { focus_ = Focus::List; return true; }
            return false;
        }
        if (focus_ == Focus::Edit) {
            // The value field: editing keys; Enter stages the value (staging writes nothing).
            if (w == VK_RETURN) { StageSelected(); return true; }
            return numEdit_.OnKey(w, hwnd_);
        }
        if (focus_ == Focus::Detail) {
            // The "New value" editor: Left/Right walk the option chips, Enter stages (staging writes nothing;
            // only the plan's Write button writes, and the plan ignores Enter).
            if (w == VK_LEFT || w == VK_RIGHT) return MovePick(w == VK_RIGHT ? 1 : -1);
            if (w == VK_RETURN) {
                if (!WritesPossible() || sel_ < 0 || sel_ >= (int)db_.S.size()) return false;
                StageSelected();
                return true;
            }
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
        if (focus_ == Focus::Search && w != VK_UP && w != VK_DOWN && w != VK_PRIOR && w != VK_NEXT) {
            // Editing, selection and clipboard keys (Ctrl+A/C/X/V/Z, Shift+arrows, Ctrl+Backspace…).
            bool r = search_.OnKey(w, hwnd_);
            if (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END || w == VK_DELETE || w == VK_BACK ||
                w == VK_INSERT || ctrl)
                return r;
        }
        switch (w) {   // the list (also while typing in the search box: legacy parity keys + Home/End)
            case VK_DOWN: MoveSel(1); return true;
            case VK_UP: MoveSel(-1); return true;
            case VK_NEXT: MoveSel(page); return true;
            case VK_PRIOR: MoveSel(-page); return true;
            case VK_HOME: Select(0, false); return true;
            case VK_END: Select((int)V_.size() - 1, false); return true;
        }
        return false;
    }
    if (msg == WM_CHAR) {
        wchar_t ch = (wchar_t)w;
        if (ch == 0x09 || ch == 0x1B || ch == 0x0D || ch == 0x06) return false;   // handled as keys
        if (focus_ == Focus::Edit) return numEdit_.OnChar(ch);
        if (focus_ != Focus::Search) {
            if (ch < 0x20 || ch == 0x7F) return false;
            focus_ = Focus::Search;   // typing anywhere starts a search
        }
        return search_.OnChar(ch);
    }
    if (msg == WM_MOUSEWHEEL) {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd_, &p);
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        // Proportional, so sub-120 deltas (touchpads, hi-res wheels) scroll too (rule fix #19).
        if (p.x >= rcDetail_.left) {
            int before = detSb_.Offset();
            detSb_.SetOffset(before - delta * Sc(60) / WHEEL_DELTA);
            return detSb_.Offset() != before;
        }
        int before = listSb_.Offset();
        listSb_.SetOffset(before - delta * 3 * Sc(Console() ? 20 : 26) / WHEEL_DELTA);
        return listSb_.Offset() != before;
    }
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    POINT pt{x, y};
    if (msg == WM_MOUSEMOVE) {
        bool changed = false;
        if (listSb_.Dragging()) return listSb_.OnMouse(msg, x, y, rcListSbTrack_);
        if (detSb_.Dragging()) return detSb_.OnMouse(msg, x, y, rcDetSbTrack_);
        int hid = 0;
        for (auto& ck : clicks_) if (PtInRect(&ck.rc, pt)) { hid = ck.id; break; }
        int row = -1, hdr = -1;
        if (PtInRect(&rcListBody_, pt) && !(listSb_.Visible() && x >= rcListSbTrack_.left)) {
            int pitch = Sc(Console() ? 20 : 26);
            int k = (y - rcListBody_.top + listSb_.Offset()) / pitch;
            if (k >= 0 && k < (int)V_.size()) row = k;
        }
        if (PtInRect(&rcListHdr_, pt)) {
            for (int col : {C_NAME, C_CUR, C_SNAP, C_DEF, C_GOAL}) {
                if (!HasCol(col)) continue;
                int nx = col == C_NAME ? rcList_.right : rcList_.right;
                for (int o : {C_CUR, C_SNAP, C_DEF, C_GOAL}) if (HasCol(o) && ColX(o) > ColX(col)) nx = std::min(nx, ColX(o));
                if (x >= ColX(col) && x < nx) hdr = col;
            }
        }
        if (hid != hoverId_ || row != hoverRow_ || hdr != hoverHdr_) changed = true;
        hoverId_ = hid; hoverRow_ = row; hoverHdr_ = hdr;
        return changed;
    }
    if (msg == WM_LBUTTONUP) {
        bool r = false;
        if (listSb_.Dragging()) r = listSb_.OnMouse(msg, x, y, rcListSbTrack_);
        if (detSb_.Dragging()) r = detSb_.OnMouse(msg, x, y, rcDetSbTrack_) || r;
        if (GetCapture() == hwnd_) ReleaseCapture();
        return r;
    }
    if (msg != WM_LBUTTONDOWN) return false;
    for (auto& ck : clicks_) {
        if (PtInRect(&ck.rc, pt)) {
            if (ck.id != ID_SEARCH && focus_ == Focus::Search) focus_ = Focus::List;
            if (ck.id != ID_NUMEDIT && ck.id != ID_STAGE && focus_ == Focus::Edit) focus_ = Focus::Detail;
            OnClickId(ck.id);
            return true;
        }
    }
    if (listSb_.Visible() && PtInRect(&rcListSbTrack_, pt)) {
        focus_ = Focus::List;
        SetCapture(hwnd_);
        return listSb_.OnMouse(msg, x, y, rcListSbTrack_) || true;
    }
    if (detSb_.Visible() && PtInRect(&rcDetSbTrack_, pt)) {
        focus_ = Focus::Detail;
        SetCapture(hwnd_);
        return detSb_.OnMouse(msg, x, y, rcDetSbTrack_) || true;
    }
    if (PtInRect(&rcListHdr_, pt) && hoverHdr_ >= 0) {
        // Legacy three-click cycle: ascending, descending, back to the default order.
        int col = hoverHdr_;
        if (col == sortCol_) { if (sortAsc_) sortAsc_ = false; else sortCol_ = -1; }
        else { sortCol_ = col; sortAsc_ = true; }
        focus_ = Focus::List;
        Refilter();
        return true;
    }
    if (PtInRect(&rcListBody_, pt)) {
        focus_ = Focus::List;
        int pitch = Sc(Console() ? 20 : 26);
        int k = (y - rcListBody_.top + listSb_.Offset()) / pitch;
        if (k >= 0 && k < (int)V_.size()) Select(k, false);
        return true;
    }
    if (PtInRect(&rcDetail_, pt)) { focus_ = Focus::Detail; return true; }
    (void)c;
    return false;
}

// ============================================================================================
// Painting

int BiosModule::TextW(HDC dc, const std::wstring& s, HFONT f) const {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

bool BiosModule::HasCol(int col) const {
    if (col == C_GOAL) return goal_ >= 0;
    if (col == C_SNAP) return compare_;
    return true;
}

int BiosModule::ColX(int col) const {
    int L = rcList_.left, W = rcList_.right - rcList_.left;
    if (col == C_NAME) return L + Sc(14);
    if (compare_) {
        switch (col) { case C_CUR: return L + W * 46 / 100; case C_SNAP: return L + W * 60 / 100;
                       case C_DEF: return L + W * 73 / 100; default: return L + W * 86 / 100; }
    }
    switch (col) { case C_CUR: return L + W * 52 / 100; case C_DEF: return L + W * 70 / 100; default: return L + W * 85 / 100; }
}

void BiosModule::Chip(HDC dc, int& x, int y, const std::wstring& label, int id, bool on) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int h = Sc(Console() ? 22 : 26);
    int w = TextW(dc, label, f.caption) + Sc(Console() ? 16 : 22);
    RECT r{x, y, x + w, y + h};
    COLORREF fill = on ? p.acc : (hoverId_ == id ? p.chipHot : p.chip);
    RoundFill(dc, r, Console() ? Sc(4) : h, fill);
    ui::DrawText(dc, label, r, f.caption, on ? p.onAcc : p.text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    AddClick(r, id);
    x += w + Sc(Console() ? 6 : 8);
}

int BiosModule::Button(HDC dc, int right, int y, const std::wstring& label, int id, bool primary, bool enabled) {
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

// A labelled on/off switch, right-aligned at `right`. Off at every start; the label says what it does.
int BiosModule::Switch(HDC dc, int right, int y, const std::wstring& label, int id, bool on, bool enabled) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    int h = Sc(con ? 24 : 30);
    int tw = Sc(con ? 30 : 36), th = Sc(con ? 16 : 18);
    int lw = TextW(dc, label, f.caption);
    int w = lw + Sc(10) + tw + Sc(12);
    RECT r{right - w, y, right, y + h};
    bool hot = enabled && hoverId_ == id;
    if (hot) RoundFill(dc, r, Sc(con ? 4 : 10), p.chip);
    RECT lr{r.left + Sc(6), y, r.left + Sc(6) + lw + Sc(4), y + h};
    TextLeft(dc, label, lr, f.caption, enabled ? (on ? p.bad : p.text) : p.faint);
    RECT tr{r.right - Sc(6) - tw, y + (h - th) / 2, r.right - Sc(6), y + (h - th) / 2 + th};
    COLORREF track = on ? p.bad : Mix(p.chipHot, p.bg, 0.2);
    if (!enabled) track = Mix(track, p.bg, 0.5);
    RoundFill(dc, tr, th, track, on ? kNoColor : p.faint);
    int k = th - Sc(6);
    int kx = on ? tr.right - Sc(3) - k : tr.left + Sc(3);
    RECT kr{kx, tr.top + Sc(3), kx + k, tr.top + Sc(3) + k};
    RoundFill(dc, kr, k, on ? OnColor(p.bad) : p.mute);
    if (enabled) AddClick(r, id);
    return w;
}

void BiosModule::PaintHeader(HDC dc, const RECT& rc) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    int pad = Sc(Console() ? 10 : 20);
    // Buttons, right-aligned (legacy: Export rightmost, accent).
    int right = rc.right - pad;
    int by = Sc(Console() ? 6 : 18);
    bool busy = worker_.Running();
    right -= Button(dc, right, by, L"Export changes", ID_EXPORT, true, dbOk_) + Sc(10);
    right -= Button(dc, right, by, L"Save snapshot…", ID_SAVE, false, dbOk_ && !busy) + Sc(10);
    right -= Button(dc, right, by, L"Load snapshot…", ID_LOAD, false, dbOk_ && !busy) + Sc(10);
    // The switch keeps its place when toggled; Restore appears to its left only while it is on.
    right -= Switch(dc, right, by, L"Allow BIOS writes", ID_WRITE_SWITCH, writeSwitch_, dbOk_ && job_ != Job::Write) + Sc(10);
    if (writeSwitch_) right -= Button(dc, right, by, L"Restore backup…", ID_RESTORE, false, WritesPossible() && !busy) + Sc(14);
    std::wstring sub = (db_.title.empty() ? L"" : db_.title + L"   •   ") + SourceLine();
    if (Console()) {
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
}

int BiosModule::PaintBanners(HDC dc, const RECT& rc) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    int pad = Sc(con ? 10 : 20);
    int y = bannerTop_;
    for (auto& b : Banners()) {
        COLORREF tone = b.tone == 2 ? p.bad : (b.tone == 1 ? p.warn : p.acc);
        int right = rc.right - pad;
        int actionW = b.action ? TextW(dc, b.actionLabel, f.caption) + Sc(26) + Sc(8) : 0;
        // Wrapped text: measure, then size the banner to fit (capped unless expanded).
        RECT tr{pad + Sc(14), 0, right - Sc(10) - actionW, 0};
        RECT m = tr; m.bottom = Sc(2000);
        SelectObject(dc, f.caption);
        DrawTextW(dc, b.text.c_str(), -1, &m, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        int textH = m.bottom - m.top;
        if (b.maxLines > 0) textH = std::min(textH, Sc(16) * b.maxLines);
        int h = std::max(Sc(con ? 24 : 30), textH + Sc(con ? 8 : 12));
        RECT r{pad, y, right, y + h};
        RoundFill(dc, r, Sc(con ? 3 : 8), Mix(p.panel, tone, 0.16));
        RECT bar{r.left, r.top, r.left + Sc(4), r.bottom};
        FillSolid(dc, bar, tone);
        tr.top = y + (h - textH) / 2; tr.bottom = tr.top + textH;
        ui::DrawText(dc, b.text, tr, f.caption, p.text, DT_WORDBREAK | DT_END_ELLIPSIS);
        if (b.action) {
            int bh = Sc(con ? 20 : 24);
            int bw = actionW - Sc(8);
            RECT ar{right - Sc(8) - bw, y + (h - bh) / 2, right - Sc(8), y + (h - bh) / 2 + bh};
            bool hot = hoverId_ == b.action;
            RoundFill(dc, ar, Sc(6), hot ? Mix(p.acc, RGB(255, 255, 255), 0.18) : p.acc);
            ui::DrawText(dc, b.actionLabel, ar, f.caption, p.onAcc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            AddClick(ar, b.action);
        }
        y += h + Sc(6);
    }
    return y == bannerTop_ ? y : y + Sc(2);
}

// The write bar, shown while "Allow BIOS writes" is on: staged count, Review and write, Stage recommended.
int BiosModule::PaintWriteBar(HDC dc, const RECT& rc, int y) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    int pad = Sc(con ? 10 : 20);
    int h = Sc(con ? 24 : 30);
    int labelW = Sc(50);
    RECT lr{pad, y, pad + labelW, y + h};
    TextLeft(dc, L"Write", lr, f.caption, p.mute);
    int x = pad + labelW;
    const bool can = WritesPossible() && Idle();   // disabled while any job runs
    const int n = (int)staged_.size();
    auto btn = [&](const std::wstring& label, int id, bool primary, bool enabled) {
        int w = TextW(dc, label, f.caption) + Sc(26);
        Button(dc, x + w, y, label, id, primary, enabled);
        x += w + Sc(8);
    };
    btn(n ? L"Review and write " + std::to_wstring(n) + L"…" : std::wstring(L"Review and write…"), ID_REVIEW, true, can && n > 0);
    std::wstring rec = goal_ >= 0 ? std::wstring(L"Stage recommended: ") + kGoalNames[goal_] : std::wstring(L"Stage recommended");
    btn(rec, ID_STAGE_REC, false, can && goal_ >= 0);
    if (n) btn(L"Unstage all", ID_UNSTAGE_ALL, false, job_ != Job::Write);
    std::wstring hint = n ? std::to_wstring(n) + L" staged. Nothing is written until you review the plan and press Write."
                          : (can ? L"Select a setting and pick a new value in the detail pane, or pick a goal and stage its "
                                   L"recommended values."
                                 : L"");
    RECT hr{x + Sc(6), y, rc.right - pad, y + h};
    TextLeft(dc, hint, hr, f.caption, p.mute);
    return y + h + Sc(con ? 6 : 10);
}

void BiosModule::Paint(HDC dc, const RECT& rc) {
    if (!host_) return;
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = Ctx();
    clicks_.clear();
    rcNumEdit_ = RECT{};
    SyncEditor();
    FillSolid(dc, rc, p.bg);
    const bool con = Console();
    int pad = Sc(con ? 10 : 20);

    PaintHeader(dc, rc);
    bannerTop_ = Sc(con ? 38 : 74);
    int y = PaintBanners(dc, rc);
    if (writeSwitch_) y = PaintWriteBar(dc, rc, y);

    // Goal row.
    int chipH = Sc(con ? 22 : 26);
    int labelW = Sc(50);
    RECT lr{pad, y, pad + labelW, y + chipH};
    TextLeft(dc, L"Goal", lr, f.caption, p.mute);
    int x = pad + labelW;
    Chip(dc, x, y, L"None", ID_GOAL, goal_ < 0);
    for (int g = 0; g < kGoalCount; g++) Chip(dc, x, y, kGoalNames[g], ID_GOAL + 1 + g, goal_ == g);
    y += chipH + Sc(con ? 6 : 10);

    // Show row + search box.
    lr = RECT{pad, y, pad + labelW, y + chipH};
    TextLeft(dc, L"Show", lr, f.caption, p.mute);
    x = pad + labelW;
    static const wchar_t* const kFilters[] = {L"All", L"Key settings", L"Changed from default", L"Hidden in menus",
                                              L"Differs from goal", L"Snapshot differs", L"Staged"};
    for (int i = 0; i <= F_STAGED; i++) {
        if (i == F_DIFFERS && goal_ < 0) continue;
        if (i == F_SNAPDIFF && !compare_) continue;
        if (i == F_STAGED && staged_.empty()) continue;
        std::wstring label = kFilters[i];
        if (i == F_STAGED) label += L" (" + std::to_wstring(staged_.size()) + L")";
        Chip(dc, x, y, label, ID_FILTER + i, filter_ == i);
    }
    int sw = Sc(con ? 260 : 320);
    rcSearch_ = RECT{std::max(x + Sc(8), (int)rc.right - pad - sw), y, rc.right - pad, y + chipH};
    search_.Paint(dc, rcSearch_, c, focus_ == Focus::Search && GetFocus() == hwnd_);
    AddClick(rcSearch_, ID_SEARCH);
    y += chipH + Sc(con ? 8 : 14);

    // Panels: list (55% / Console 60%) and detail.
    int listW = (int)(rc.right - 2 * pad) * (con ? 60 : 55) / 100;
    int gap = Sc(con ? 8 : 16);
    rcList_ = RECT{pad, y, pad + listW, rc.bottom - pad};
    rcDetail_ = RECT{rcList_.right + gap, y, rc.right - pad, rc.bottom - pad};
    if (rcList_.bottom - rcList_.top < Sc(80)) { rcList_.bottom = rcList_.top + Sc(80); rcDetail_.bottom = rcList_.bottom; }
    PaintList(dc);

    RoundFill(dc, rcDetail_, Sc(con ? 4 : 12), p.panel);
    int contentH = PaintDetail(dc, false);
    int viewH = rcDetail_.bottom - rcDetail_.top - Sc(con ? 12 : 18) - Sc(8);
    detSb_.Configure(viewH, contentH);
    PaintDetail(dc, true);
    rcDetSbTrack_ = RECT{rcDetail_.right - Sc(14), rcDetail_.top + Sc(8), rcDetail_.right, rcDetail_.bottom - Sc(8)};
    if (detSb_.Visible()) detSb_.Paint(dc, rcDetSbTrack_, c);
    if (focus_ == Focus::Detail && GetFocus() == hwnd_) {
        RECT fr = rcDetail_; InflateRect(&fr, -Sc(2), -Sc(2));
        FocusRectC(dc, fr, p.acc);
    }
    if (planOpen_) {
        clicks_.clear();   // modal: only the overlay's own targets take clicks
        rcNumEdit_ = RECT{};
        PaintPlan(dc, rc);
    }
}

void BiosModule::PaintList(HDC dc) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = Ctx();
    const bool con = Console();
    RoundFill(dc, rcList_, Sc(con ? 4 : 12), p.panel);
    int hdrH = Sc(con ? 24 : 34);
    int pitch = Sc(con ? 20 : 26);
    rcListHdr_ = RECT{rcList_.left, rcList_.top, rcList_.right, rcList_.top + hdrH};
    rcListBody_ = RECT{rcList_.left, rcList_.top + hdrH, rcList_.right, rcList_.bottom - Sc(6)};
    int viewH = rcListBody_.bottom - rcListBody_.top;
    listSb_.Configure(viewH, (int)V_.size() * pitch);
    if (centrePending_ && viewH > 0) {
        centrePending_ = false;
        auto it = std::find(V_.begin(), V_.end(), sel_);
        int k = it == V_.end() ? 0 : (int)(it - V_.begin());
        int rows = std::max(1, viewH / std::max(1, pitch));
        if (k >= rows) listSb_.SetOffset((k - rows / 2) * pitch);
    }
    int sbW = listSb_.Visible() ? Sc(14) : 0;
    rcListSbTrack_ = RECT{rcList_.right - Sc(14), rcListBody_.top, rcList_.right - Sc(2), rcListBody_.bottom};
    int right = rcList_.right - Sc(10) - sbW;

    // Header (legacy 362-376).
    HFONT hf = f.caption;
    for (int col : {C_NAME, C_CUR, C_SNAP, C_DEF, C_GOAL}) {
        if (!HasCol(col)) continue;
        std::wstring t;
        switch (col) {
            case C_NAME: t = L"Setting  (" + std::to_wstring(V_.size()) + L")    grey = hidden in BIOS menus"; break;
            case C_CUR: t = unverified_ ? L"Current (unverified)" : (compare_ ? L"Live" : L"Current"); break;
            case C_SNAP: t = snapUnverified_ ? L"Snapshot (unverified)" : L"Snapshot"; break;
            case C_DEF: t = L"Default"; break;
            default: t = L"Goal"; break;
        }
        int nx = right;
        for (int o : {C_CUR, C_SNAP, C_DEF, C_GOAL}) if (HasCol(o) && ColX(o) > ColX(col)) nx = std::min(nx, ColX(o) - Sc(6));
        RECT r{ColX(col), rcList_.top + Sc(4), nx, rcList_.top + hdrH};
        COLORREF tc = hoverHdr_ == col ? p.text : p.mute;
        TextLeft(dc, t, r, hf, tc);
        if (col == sortCol_) {
            int tw = TextW(dc, t, hf);
            RECT ar{r.left + tw, r.top, nx, r.bottom};
            TextLeft(dc, sortAsc_ ? L"  ▲" : L"  ▼", ar, f.semi, p.acc);
        }
    }

    // Rows (legacy 377-401), clipped to the body.
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, rcListBody_.left, rcListBody_.top, rcListBody_.right, rcListBody_.bottom);
    int first = listSb_.Offset() / pitch;
    int y0 = rcListBody_.top - (listSb_.Offset() % pitch);
    const bool focused = focus_ == Focus::List && GetFocus() == hwnd_;
    for (int i = first; i < (int)V_.size(); i++) {
        int ry = y0 + (i - first) * pitch;
        if (ry >= rcListBody_.bottom) break;
        const Setting& s = db_.S[V_[i]];
        RECT row{rcList_.left + Sc(6), ry + Sc(1), rcList_.right - Sc(10) - sbW + Sc(4), ry + pitch - Sc(1)};
        bool selected = V_[i] == sel_;
        if (selected) RoundFill(dc, row, Sc(con ? 3 : 8), p.sel);
        else if (i == hoverRow_) RoundFill(dc, row, Sc(con ? 3 : 8), p.hover);
        if (selected && focused) FocusRectC(dc, row, p.acc);
        int nameX = ColX(C_NAME);
        int curX = ColX(C_CUR);
        if (db_.IsKey(s)) {
            int d = Sc(7);
            HBRUSH br = CreateSolidBrush(p.acc);
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
            int dy = ry + (pitch - d) / 2;
            Ellipse(dc, nameX, dy, nameX + d + 1, dy + d + 1);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
        }
        HFONT body = con ? f.mono : f.ui;
        RECT nr{nameX + Sc(12), ry, curX - Sc(10), ry + pitch};
        COLORREF nameC = selected ? p.selText : (s.hidden ? p.mute : p.text);
        TextLeft(dc, s.prompt, nr, body, nameC);
        if (!s.ctx.empty()) {
            int nw = TextW(dc, s.prompt, body);
            if (nr.left + nw + Sc(30) < curX - Sc(10)) {
                RECT cr{nr.left + nw + Sc(8), ry, curX - Sc(10), ry + pitch};
                TextLeft(dc, L"·  " + s.ctx, cr, f.caption, p.faint);
            }
        }
        auto cell = [&](int col, const std::wstring& t, COLORREF col_c) {
            int nx = right;
            for (int o : {C_CUR, C_SNAP, C_DEF, C_GOAL}) if (HasCol(o) && ColX(o) > ColX(col)) nx = std::min(nx, ColX(o) - Sc(6));
            RECT r{ColX(col), ry, nx, ry + pitch};
            TextLeft(dc, t, r, body, col_c);
        };
        std::wstring cur = CurText(s);
        if (unverified_ && s.hasCur) cur += L" ?";
        auto stg = staged_.find(V_[i]);
        if (stg != staged_.end()) {   // staged: "old → new", or "→ new" when both don't fit
            std::wstring both = cur + L" → " + ValText(s, stg->second);
            int nx = right;
            for (int o : {C_SNAP, C_DEF, C_GOAL}) if (HasCol(o) && ColX(o) > ColX(C_CUR)) nx = std::min(nx, ColX(o) - Sc(6));
            cell(C_CUR, TextW(dc, both, body) <= nx - ColX(C_CUR) ? both : L"→ " + ValText(s, stg->second), p.acc);
        }
        else cell(C_CUR, cur, Changed(s) ? p.warn : p.text);
        if (compare_) {
            std::wstring st = SnapText(s);
            if (snapUnverified_ && s.hasSnap) st += L" ?";
            cell(C_SNAP, st, SnapDiffers(s) ? p.warn : p.mute);
        }
        cell(C_DEF, DefText(s), p.mute);
        if (goal_ >= 0 && db_.KbOf(s)) {
            long long v;
            std::wstring rt = RecText(db_, s, goal_);
            if (!rt.empty()) {
                COLORREF gc = p.mute;
                if (RecVal(db_, s, goal_, &v) && s.hasCur) gc = v == s.cur ? p.good : p.bad;   // legacy bug fixed: unknown current stays neutral
                cell(C_GOAL, rt, gc);
            }
        }
    }
    if (V_.empty()) {
        RECT er{rcListBody_.left + Sc(14), rcListBody_.top + Sc(8), rcListBody_.right - Sc(14), rcListBody_.top + Sc(40)};
        TextLeft(dc, dbOk_ ? L"No settings match the filter and search." : L"No settings map loaded.", er, f.ui, p.mute);
    }
    RestoreDC(dc, saved);
    if (listSb_.Visible()) listSb_.Paint(dc, rcListSbTrack_, c);
}

// The detail pane (legacy §4.8). Called twice per paint: draw=false measures the content height for the
// scrollbar, draw=true paints it with the scroll offset. Returns the content height.
int BiosModule::PaintDetail(HDC dc, bool draw) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    int pad = Sc(con ? 12 : 20);
    int sbW = Sc(14);
    int L = rcDetail_.left + pad, R = rcDetail_.right - pad - sbW / 2;
    int top = rcDetail_.top + Sc(con ? 12 : 18);
    int bottom = rcDetail_.bottom - Sc(8);
    int y = top - (draw ? detSb_.Offset() : 0);
    int saved = 0;
    if (draw) {
        saved = SaveDC(dc);
        IntersectClipRect(dc, rcDetail_.left, rcDetail_.top + Sc(6), rcDetail_.right - sbW, bottom);
    }
    HFONT body = con ? f.mono : f.ui;
    auto para = [&](const std::wstring& t, HFONT font, COLORREF col, int x0, int x1) {
        if (t.empty()) return 0;
        RECT m{x0, 0, x1, 0};
        SelectObject(dc, font);
        DrawTextW(dc, t.c_str(), -1, &m, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS);
        int h = m.bottom - m.top;
        if (draw && y + h > rcDetail_.top && y < bottom) {
            RECT r{x0, y, x1, y + h};
            ui::DrawText(dc, t, r, font, col, DT_WORDBREAK | DT_EXPANDTABS);
        }
        return h;
    };
    auto block = [&](const std::wstring& t, HFONT font, COLORREF col, int gapAfter = 4) {
        y += para(t, font, col, L, R) + Sc(gapAfter);
    };
    auto heading = [&](const std::wstring& t) {
        y += Sc(con ? 4 : 6);
        if (draw) { RECT r{L, y, R, y + Sc(22)}; TextLeft(dc, t, r, f.semi, p.acc); }
        y += Sc(con ? 20 : 24);
    };
    auto labelled = [&](const std::wstring& label, const std::wstring& t, COLORREF lc, HFONT lf, COLORREF tc) {
        int lw = Sc(con ? 110 : 130);
        int h = para(t, body, tc, L + lw, R);
        if (draw) { RECT r{L, y, L + lw - Sc(6), y + Sc(20)}; TextLeft(dc, label, r, lf, lc); }
        y += std::max(h, Sc(20)) + Sc(con ? 3 : 6);
    };
    auto finish = [&]() {
        if (draw) RestoreDC(dc, saved);
        return y + (draw ? detSb_.Offset() : 0) - top + Sc(12);
    };

    if (sel_ < 0 || sel_ >= (int)db_.S.size()) {
        if (draw) { RECT r{L, y, R, y + Sc(24)}; TextLeft(dc, L"No setting selected", r, body, p.mute); }
        y += Sc(24);
        return finish();
    }
    const Setting& s = db_.S[sel_];
    const Kb* k = db_.KbOf(s);
    // 1-2. Prompt and path.
    block(s.prompt, con ? f.semi : f.title, p.text, 2);
    if (!s.path.empty()) block(s.path, f.caption, p.mute, 6);
    // 3. Badges.
    {
        int bx = L;
        int bh = Sc(20);
        auto badge = [&](const std::wstring& t, COLORREF col) {
            int w = TextW(dc, t, f.caption) + Sc(14);
            if (bx + w > R && bx > L) { bx = L; y += bh + Sc(4); }
            if (draw) {
                RECT r{bx, y, bx + w, y + bh};
                RoundFill(dc, r, Sc(8), Mix(p.panel, col, 0.22));
                ui::DrawText(dc, t, r, f.caption, col, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
            bx += w + Sc(6);
        };
        if (s.hidden) badge(L"Hidden in BIOS menus", p.mute);
        else if (s.cond) badge(L"Shown depending on other settings", p.mute);
        if (Changed(s)) badge(L"Changed from default", p.warn);
        if (unverified_ && s.hasCur)
            badge(guardText_.empty() ? L"Unverified (board/BIOS not checked)" : L"Unverified (board/BIOS mismatch)", p.bad);
        if (compare_ && snapUnverified_ && s.hasSnap) badge(L"Snapshot unverified", p.bad);
        if (compare_ && SnapDiffers(s)) badge(L"Snapshot differs", p.warn);
        wchar_t loc[160];
        swprintf(loc, 160, L"%ls + 0x%X (%d byte%ls)", s.var.c_str(), (unsigned)s.offset, s.width, s.width == 1 ? L"" : L"s");
        badge(loc, p.mute);
        if (staged_.count(sel_)) badge(L"Staged", p.acc);
        y += bh + Sc(8);
    }
    // 3b. New value (docs/design/bios-write.md "UI"): only when writes are possible.
    if (WritesPossible()) {
        const int visTop = rcDetail_.top + Sc(6), visBot = bottom;
        auto clickIfVisible = [&](const RECT& r, int id) {
            if (draw && r.top >= visTop && r.bottom <= visBot) AddClick(r, id);
        };
        auto smallBtn = [&](int x, const std::wstring& label, int id, bool primary, bool enabled) {
            int w = TextW(dc, label, f.caption) + Sc(24), h = Sc(con ? 22 : 26);
            RECT r{x, y, x + w, y + h};
            if (draw) {
                bool hot = enabled && hoverId_ == id;
                COLORREF fill = primary ? (hot ? Mix(p.acc, RGB(255, 255, 255), 0.18) : p.acc) : (hot ? p.chipHot : p.chip);
                if (!enabled) fill = Mix(fill, p.panel, 0.5);
                RoundFill(dc, r, Sc(con ? 4 : 8), fill);
                COLORREF tc = primary ? p.onAcc : p.text;
                if (!enabled) tc = Mix(tc, fill, 0.5);
                ui::DrawText(dc, label, r, f.caption, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
                if (enabled) clickIfVisible(r, id);
            }
            return w;
        };
        heading(L"New value");
        auto st = staged_.find(sel_);
        if (!s.hasCur) {
            block(L"No current value was read for this setting, so it can't be changed.", f.caption, p.faint, 6);
        } else {
            if (st != staged_.end()) {
                int bw = TextW(dc, L"Unstage", f.caption) + Sc(24);
                if (draw) {
                    RECT r{L, y, R - bw - Sc(8), y + Sc(con ? 22 : 26)};
                    TextLeft(dc, L"Staged: " + ValText(s, s.cur) + L"  →  " + ValText(s, st->second), r, f.semi, p.acc);
                }
                smallBtn(R - bw, L"Unstage", ID_UNSTAGE, false, true);
                y += Sc(con ? 26 : 32);
            }
            const int ch = Sc(con ? 22 : 26);
            if (s.kind == L"numeric") {
                int fw = Sc(con ? 140 : 160);
                RECT fr{L, y, L + fw, y + ch};
                if (draw && fr.top >= visTop && fr.bottom <= visBot) {
                    numEdit_.Paint(dc, fr, Ctx(), focus_ == Focus::Edit && GetFocus() == hwnd_);
                    rcNumEdit_ = fr;
                    AddClick(fr, ID_NUMEDIT);
                }
                int bx = L + fw + Sc(8);
                bx += smallBtn(bx, L"Stage", ID_STAGE, true, true) + Sc(12);
                wchar_t rb[160];
                if (s.step > 0) swprintf(rb, 160, L"%lld to %lld, step %lld", s.mn, s.mx, s.step);
                else swprintf(rb, 160, L"%lld to %lld", s.mn, s.mx);
                if (draw) { RECT r{bx, y, R, y + ch}; TextLeft(dc, rb, r, f.caption, p.mute); }
                y += ch + Sc(6);
            } else {
                // Option chips (checkbox without options: 0 / 1). The current value is outlined and dotted.
                std::vector<std::pair<std::wstring, long long>> chips;
                if (s.kind == L"checkbox" && s.opts.empty()) chips = {{L"0 (off)", 0}, {L"1 (on)", 1}};
                else for (auto& o : s.opts) chips.push_back({o.t.empty() ? std::to_wstring(o.v) : o.t, o.v});
                int cx = L;
                for (size_t k = 0; k < chips.size(); k++) {
                    bool isCur = chips[k].second == s.cur;
                    bool picked = hasPick_ && pick_ == chips[k].second;
                    std::wstring label = (isCur ? L"● " : L"") + chips[k].first;
                    int w = TextW(dc, label, f.caption) + Sc(con ? 14 : 20);
                    if (cx + w > R && cx > L) { cx = L; y += ch + Sc(6); }
                    RECT r{cx, y, cx + w, y + ch};
                    if (draw && y + ch > rcDetail_.top && y < bottom) {
                        int id = ID_OPTION + (int)k;
                        COLORREF fill = picked ? p.acc : (hoverId_ == id ? p.chipHot : p.chip);
                        RoundFill(dc, r, con ? Sc(4) : ch, fill, isCur && !picked ? p.acc : kNoColor);
                        ui::DrawText(dc, label, r, f.caption, picked ? p.onAcc : p.text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
                        clickIfVisible(r, id);
                    }
                    cx += w + Sc(6);
                }
                y += ch + Sc(8);
                bool can = hasPick_ && pick_ != s.cur && (st == staged_.end() || st->second != pick_);
                smallBtn(L, L"Stage", ID_STAGE, true, can);
                y += ch + Sc(6);
            }
            if (!editMsg_.empty())
                block(editMsg_, f.caption, editTone_ == 2 ? p.bad : (editTone_ == 1 ? p.warn : p.mute), 4);
            if (s.hidden || s.cond)
                block(s.hidden ? L"Hidden in the BIOS menu: the firmware may ignore or reset it."
                               : L"Shown only when another setting allows it; changing it may have no effect.",
                      f.caption, p.warn, 4);
        }
        y += Sc(4);
    }
    // 4. What it does + basis (rule 7: the UI shows the basis).
    if (k) {
        heading(L"What it does");
        block(k->expl, body, p.text);
        COLORREF bc = k->basis == L"published" || k->basis == L"owner-measured" ? p.good
                    : k->basis == L"folklore" ? p.warn : (k->basis.empty() ? p.faint : p.mute);
        std::wstring bt = BasisLabel(k->basis);
        if (!k->basisNote.empty()) bt += L": " + k->basisNote;
        else if (k->basis.empty())
            bt += L": this settings map doesn't record where this text comes from. Treat its claims as unchecked "
                  L"and test on your own system.";
        labelled(L"Basis", bt, p.mute, f.semi, bc);
    }
    // 5. Real-world effect + claim check.
    if (k && !k->fx[0].empty()) {
        heading(L"Real-world effect");
        if (k->fx[0] == k->fx[1] && k->fx[1] == k->fx[2]) labelled(L"All workloads", k->fx[0], p.mute, f.semi, p.text);
        else {
            labelled(L"FPS / esports", k->fx[0], p.mute, f.semi, p.text);
            labelled(L"Other games", k->fx[1], p.mute, f.semi, p.text);
            labelled(L"Apps & work", k->fx[2], p.mute, f.semi, p.text);
        }
        if (!k->claim.empty()) { heading(L"Claim check"); block(k->claim, body, p.claim); }
    } else if (k && !k->claim.empty()) {   // legacy hid a claim with no effect text; show it
        heading(L"Claim check");
        block(k->claim, body, p.claim);
    }
    // Sources for numbers (K field 17).
    if (k && !k->sources.empty()) {
        heading(L"Sources for numbers");
        for (auto& src : k->sources)
            block(L"• " + src.number + L"  (" + BasisLabel(src.kind) + L"): " + src.ref, f.caption, p.mute, 2);
        y += Sc(4);
    }
    // 6. Help text.
    if (!s.help.empty()) {
        if (k) heading(L"BIOS help text");
        else heading(L"What it does (BIOS help text)");
        block(s.help, body, k ? p.mute : p.text);
    }
    // 7. Values.
    heading(L"Values");
    {
        std::wstring cv = L"Current: " + CurText(s);
        if (unverified_ && s.hasCur) cv = L"Current (unverified): " + CurText(s);
        std::wstring line = cv + L"      Default: " + DefText(s);
        if (compare_) line += (snapUnverified_ ? L"      Snapshot (unverified): " : L"      Snapshot: ") + SnapText(s);
        block(line, body, p.text, 4);
        if (!s.hasCur) {
            std::wstring why;
            if (primary_ == Src::Live) {
                auto it = liveState_.find(VarKey{s.var, s.guid});
                if (it != liveState_.end() && it->second->err)
                    why = L"Not read: variable " + s.var + L" " + VarErrorText(it->second->err) + L".";
                else if (it != liveState_.end())
                    why = L"Variable " + s.var + L" is only " + std::to_wstring(it->second->data.size()) +
                          L" bytes; this setting needs offset + width = " + std::to_wstring(s.offset + s.width) + L".";
            } else if (primary_ == Src::Snapshot) {
                why = L"Variable " + s.var + L" isn't in the snapshot, or is too short for this setting.";
            } else {
                why = L"No values loaded.";
            }
            if (!why.empty()) block(why, f.caption, p.faint, 4);
        }
        if (s.kind == L"numeric" && s.hasMin) {
            wchar_t b[160];
            swprintf(b, 160, L"Range %lld to %lld, step %lld", s.mn, s.mx, s.step ? s.step : 1LL);
            block(b, f.caption, p.mute, 4);
        }
        long long recV = 0;
        bool hasRec = goal_ >= 0 && RecVal(db_, s, goal_, &recV);
        int oh = Sc(con ? 20 : 24), op = Sc(con ? 22 : 26);
        for (auto& o : s.opts) {
            bool isCur = s.hasCur && o.v == s.cur;
            if (draw && y + oh > rcDetail_.top && y < bottom) {
                RECT r{L, y, R, y + oh};
                if (isCur) RoundFill(dc, r, Sc(con ? 3 : 6), p.sel);
                RECT tr{L + Sc(8), y, R - Sc(200), y + oh};
                std::wstring t = (isCur ? L"● " : L"   ") + (o.t.empty() ? std::to_wstring(o.v) : o.t);
                TextLeft(dc, t, tr, body, isCur ? p.selText : p.mute);
                std::wstring tag;
                if (s.hasDef && o.v == s.def) tag += L"default ";
                if (compare_ && s.hasSnap && o.v == s.snap) tag += L"snapshot ";
                if (hasRec && o.v == recV) tag += std::wstring(L"best for ") + kGoalNames[goal_];
                if (!tag.empty()) {
                    RECT gr{R - Sc(200), y, R - Sc(8), y + oh};
                    TextRight(dc, tag, gr, f.caption, hasRec && o.v == recV ? p.good : p.mute);
                }
            }
            y += op;
        }
        y += Sc(4);
    }
    // 8. Recommended by goal (its basis is the entry's basis, shown above).
    if (k) {
        heading(L"Recommended by goal");
        for (int g = 0; g < kGoalCount; g++) {
            std::wstring rt = RecText(db_, s, g);
            long long v;
            bool exactMatch = RecVal(db_, s, g, &v) && s.hasCur && v == s.cur;
            labelled(kGoalNames[g], rt.empty() ? L"no change needed" : rt, g == goal_ ? p.acc : p.mute,
                     g == goal_ ? f.semi : body, exactMatch ? p.good : p.text);
        }
        // 9. Note.
        if (!k->note.empty()) { heading(L"Note"); block(k->note, body, p.warn); }
    }
    return finish();
}

// ============================================================================================
// The plan overlay: modal inside the view. Esc or Cancel closes it; only a single click on its Write
// button confirms (Enter, Space and double-clicks never do).

bool BiosModule::PlanInput(UINT msg, WPARAM w, LPARAM l) {
    const bool running = job_ == Job::Write;
    if (msg == WM_KEYDOWN) {
        int before = planSb_.Offset();
        int view = rcPlanList_.bottom - rcPlanList_.top;
        switch (w) {
            case VK_ESCAPE:
                if (running) return false;
                planOpen_ = false;
                host_->Log(LogLevel::Info, L"bios: plan closed without writing");
                return true;
            case VK_UP: planSb_.SetOffset(before - Sc(40)); break;
            case VK_DOWN: planSb_.SetOffset(before + Sc(40)); break;
            case VK_PRIOR: planSb_.SetOffset(before - view * 9 / 10); break;
            case VK_NEXT: planSb_.SetOffset(before + view * 9 / 10); break;
            case VK_HOME: planSb_.SetOffset(0); break;
            case VK_END: planSb_.SetOffset(planSb_.MaxOffset()); break;
            default: return false;   // Enter included: it never confirms the plan
        }
        return planSb_.Offset() != before;
    }
    if (msg == WM_MOUSEWHEEL) {
        int before = planSb_.Offset();
        planSb_.SetOffset(before - GET_WHEEL_DELTA_WPARAM(w) * Sc(60) / WHEEL_DELTA);
        return planSb_.Offset() != before;
    }
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    POINT pt{x, y};
    if (msg == WM_MOUSEMOVE) {
        if (planSb_.Dragging()) return planSb_.OnMouse(msg, x, y, rcPlanSbTrack_);
        int hid = 0;
        for (auto& ck : clicks_) if (PtInRect(&ck.rc, pt)) { hid = ck.id; break; }
        bool changed = hid != hoverId_ || hoverRow_ >= 0 || hoverHdr_ >= 0;
        hoverId_ = hid; hoverRow_ = -1; hoverHdr_ = -1;
        return changed;
    }
    if (msg == WM_LBUTTONUP) {
        bool r = planSb_.Dragging() && planSb_.OnMouse(msg, x, y, rcPlanSbTrack_);
        if (GetCapture() == hwnd_) ReleaseCapture();
        return r;
    }
    if (msg != WM_LBUTTONDOWN) return false;   // WM_LBUTTONDBLCLK too: a confirm takes a fresh single click
    if (planSb_.Visible() && PtInRect(&rcPlanSbTrack_, pt)) {
        SetCapture(hwnd_);
        return planSb_.OnMouse(msg, x, y, rcPlanSbTrack_) || true;
    }
    for (auto& ck : clicks_) {
        if (!PtInRect(&ck.rc, pt)) continue;
        if (ck.id == ID_PLAN_CANCEL) {
            if (running) {
                worker_.Cancel();
                host_->SetStatus(L"Stopping after the current variable…");
            } else {
                planOpen_ = false;
                host_->Log(LogLevel::Info, L"bios: plan cancelled");
            }
        } else if (ck.id == ID_PLAN_WRITE) {
            StartWrite();
        }
        return true;   // ID_PLAN_SURFACE and the rest: swallowed
    }
    return false;
}

void BiosModule::PaintPlan(HDC dc, const RECT& rc) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    const bool running = job_ == Job::Write;
    // Dim the view behind (AlphaBlend from msimg32, a system library).
    if (HDC src = CreateCompatibleDC(dc)) {
        if (HBITMAP one = CreateCompatibleBitmap(dc, 1, 1)) {
            HGDIOBJ old = SelectObject(src, one);
            RECT px{0, 0, 1, 1};
            FillSolid(src, px, RGB(0, 0, 0));
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 170, 0};
            AlphaBlend(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, src, 0, 0, 1, 1, bf);
            SelectObject(src, old);
            DeleteObject(one);
        }
        DeleteDC(src);
    }
    int m = Sc(con ? 14 : 32);
    int pw = std::min((int)(rc.right - rc.left) - 2 * m, Sc(1100));
    RECT pr{rc.left + ((rc.right - rc.left) - pw) / 2, rc.top + m, rc.left + ((rc.right - rc.left) + pw) / 2, rc.bottom - m};
    RoundFill(dc, pr, Sc(con ? 4 : 14), p.panel, p.bad);
    int pad = Sc(con ? 14 : 24);
    int x0 = pr.left + pad, x1 = pr.right - pad;
    int y = pr.top + pad - Sc(4);
    {
        RECT tr{x0, y, x1, y + Sc(32)};
        TextLeft(dc, plan_.Title(), tr, con ? f.semi : f.title, p.text);
        y += Sc(con ? 28 : 36);
    }
    auto para = [&](const std::wstring& t, HFONT font, COLORREF col, int left, int right) {
        RECT mr{left, 0, right, 0};
        SelectObject(dc, font);
        DrawTextW(dc, t.c_str(), -1, &mr, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        RECT r{left, y, right, y + (mr.bottom - mr.top)};
        ui::DrawText(dc, t, r, font, col, DT_WORDBREAK);
        return (int)(mr.bottom - mr.top);
    };
    {
        // The warning, as a bad-tone banner.
        std::wstring warn = kWriteWarning;
        RECT mr{x0 + Sc(14), 0, x1 - Sc(10), 0};
        SelectObject(dc, f.caption);
        DrawTextW(dc, warn.c_str(), -1, &mr, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        int h = (mr.bottom - mr.top) + Sc(14);
        RECT br{x0, y, x1, y + h};
        RoundFill(dc, br, Sc(con ? 3 : 8), Mix(p.panel, p.bad, 0.18));
        RECT bar{br.left, br.top, br.left + Sc(4), br.bottom};
        FillSolid(dc, bar, p.bad);
        int ty = y;
        y += Sc(7);
        para(warn, f.caption, p.text, x0 + Sc(14), x1 - Sc(10));
        y = ty + h + Sc(10);
    }
    {
        std::wstring b = (plan_.restore ? L"Restoring from: " + plan_.restoreFrom + L"\n" : L"") +
                         L"Backup first: every variable below is saved to " + backupShown_ +
                         L"\\<date-time>\\ and flushed to disk before the first write. Each write is read back; a "
                         L"mismatch puts the old bytes back and stops.";
        y += para(b, f.caption, p.mute, x0, x1) + Sc(10);
    }
    // Buttons at the bottom: Cancel, then the one that writes.
    int bh = Sc(con ? 26 : 32);
    int by = pr.bottom - pad - bh;
    auto button = [&](int right, const std::wstring& label, int id, COLORREF fill, COLORREF text, bool enabled) {
        int w = TextW(dc, label, f.semi) + Sc(30);
        RECT r{right - w, by, right, by + bh};
        bool hot = enabled && hoverId_ == id;
        if (hot) fill = Mix(fill, RGB(255, 255, 255), 0.14);
        if (!enabled) { fill = Mix(fill, p.panel, 0.55); text = Mix(text, fill, 0.5); }
        RoundFill(dc, r, Sc(con ? 4 : 10), fill);
        ui::DrawText(dc, label, r, f.semi, text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        if (enabled) AddClick(r, id);
        return w;
    };
    int n = plan_.SettingCount(), vars = (int)plan_.vars.size();
    std::wstring go = plan_.restore ? L"Restore " + std::to_wstring(vars) + (vars == 1 ? L" variable" : L" variables")
                                    : L"Write " + std::to_wstring(n) + (n == 1 ? L" setting" : L" settings");
    if (running)
        go = L"Writing " + std::to_wstring(std::min(execDone_ + 1, std::max(1, execTotal_))) + L" of " + std::to_wstring(execTotal_) + L"…";
    int right = x1;
    right -= button(right, go, ID_PLAN_WRITE, p.bad, OnColor(p.bad), Idle() && WritesPossible()) + Sc(10);
    bool cancelling = running && worker_.Cancelled();
    button(right, running ? (cancelling ? L"Stopping…" : L"Stop after this variable") : L"Cancel", ID_PLAN_CANCEL, p.chip, p.text,
           !cancelling);
    {
        RECT hr{x0, by, right - Sc(220), by + bh};
        TextLeft(dc, running ? L"Writing. Each variable is verified before the next one starts."
                             : L"Esc or Cancel closes this without writing. Enter does not confirm.",
                 hr, f.caption, p.faint);
    }
    // The scrolling plan list.
    rcPlanList_ = RECT{x0, y, x1, by - Sc(12)};
    if (rcPlanList_.bottom - rcPlanList_.top < Sc(60)) rcPlanList_.bottom = rcPlanList_.top + Sc(60);
    RoundFill(dc, rcPlanList_, Sc(con ? 3 : 10), Mix(p.panel, p.bg, 0.55));
    int contentH = PaintPlanList(dc, false);
    planSb_.Configure(rcPlanList_.bottom - rcPlanList_.top - Sc(8), contentH);
    PaintPlanList(dc, true);
    rcPlanSbTrack_ = RECT{rcPlanList_.right - Sc(14), rcPlanList_.top + Sc(4), rcPlanList_.right - Sc(2), rcPlanList_.bottom - Sc(4)};
    if (planSb_.Visible()) planSb_.Paint(dc, rcPlanSbTrack_, Ctx());
    AddClick(rc, ID_PLAN_SURFACE);   // last: the buttons above win; everything else swallows clicks
}

// The plan rows. draw=false measures the content height. Returns it.
int BiosModule::PaintPlanList(HDC dc, bool draw) {
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    const bool con = Console();
    const RECT& lr = rcPlanList_;
    int L = lr.left + Sc(14), R = lr.right - Sc(22);
    int top = lr.top + Sc(8);
    int y = top - (draw ? planSb_.Offset() : 0);
    int saved = 0;
    if (draw) { saved = SaveDC(dc); IntersectClipRect(dc, lr.left, lr.top + Sc(2), lr.right - Sc(14), lr.bottom - Sc(2)); }
    auto visible = [&](int h) { return draw && y + h > lr.top && y < lr.bottom; };
    auto wrapped = [&](const std::wstring& t, HFONT font, COLORREF col, int left) {
        RECT mr{left, 0, R, 0};
        SelectObject(dc, font);
        DrawTextW(dc, t.c_str(), -1, &mr, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        int h = mr.bottom - mr.top;
        if (visible(h)) { RECT r{left, y, R, y + h}; ui::DrawText(dc, t, r, font, col, DT_WORDBREAK); }
        y += h;
    };
    HFONT body = con ? f.mono : f.ui;
    for (auto& v : plan_.vars) {
        int hh = Sc(con ? 24 : 28);
        if (visible(hh)) {
            RECT band{lr.left + Sc(6), y, lr.right - Sc(18), y + hh};
            RoundFill(dc, band, Sc(con ? 3 : 8), Mix(p.panel, p.acc, 0.14));
            wchar_t meta[160];
            int ns = 0;
            for (auto& s : v.settings) if (s.setting >= 0) ns++;
            swprintf(meta, 160, L"attributes 0x%lX (NV|BS|RT)   •   %zu bytes   •   %d setting%ls", (unsigned long)v.attr,
                     v.oldData.size(), ns, ns == 1 ? L"" : L"s");
            int mw = TextW(dc, meta, f.caption);
            RECT nr{L, y, R - mw - Sc(12), y + hh};
            TextLeft(dc, v.name + L"   {" + v.guid + L"}", nr, f.semi, p.text);
            RECT mr2{R - mw, y, R, y + hh};
            TextRight(dc, meta, mr2, f.caption, p.mute);
        }
        y += hh + Sc(6);
        for (auto& s : v.settings) {
            int rh = Sc(con ? 20 : 24);
            std::wstring change = s.oldText + L"  →  " + s.newText;
            if (visible(rh)) {
                int cw = std::min(TextW(dc, change, f.semi), (R - L) / 2);
                RECT pr2{L, y, R - cw - Sc(16), y + rh};
                TextLeft(dc, s.prompt, pr2, s.setting >= 0 ? body : f.caption, s.setting >= 0 ? p.text : p.mute);
                RECT cr{R - cw, y, R, y + rh};
                TextRight(dc, change, cr, f.semi, p.acc);
            }
            y += rh;
            int sh = Sc(con ? 16 : 18);
            if (!s.path.empty()) {
                if (visible(sh)) { RECT r{L, y, R, y + sh}; TextLeft(dc, s.path, r, f.caption, p.mute); }
                y += sh;
            }
            wchar_t loc[96];
            swprintf(loc, 96, L"offset 0x%X  •  %d byte%ls  •  ", (unsigned)s.offset, s.width, s.width == 1 ? L"" : L"s");
            std::wstring bytes = std::wstring(loc) + HexBytes(s.oldBytes) + L"  →  " + HexBytes(s.newBytes);
            if (visible(sh)) { RECT r{L, y, R, y + sh}; TextLeft(dc, bytes, r, f.mono, p.faint); }
            y += sh;
            if (!s.flags.empty()) wrapped(s.flags, f.caption, p.warn, L);
            if (!s.shares.empty()) wrapped(L"Same bytes as: " + s.shares, f.caption, p.warn, L);
            y += Sc(6);
            if (visible(1)) HLine(dc, L, R, y, Mix(p.panel, p.mute, 0.25));
            y += Sc(6);
        }
        y += Sc(6);
    }
    if (!plan_.notes.empty()) {
        if (visible(Sc(20))) { RECT r{L, y, R, y + Sc(20)}; TextLeft(dc, L"Notes", r, f.semi, p.mute); }
        y += Sc(22);
        for (auto& n : plan_.notes) { wrapped(L"• " + n, f.caption, p.mute, L); y += Sc(2); }
    }
    if (draw) RestoreDC(dc, saved);
    return y + (draw ? planSb_.Offset() : 0) - top + Sc(8);
}

// ============================================================================================
// Headless

void BiosModule::LoadFixture(const wchar_t* dir) {
    // Synchronous, no Worker, so --screenshot is deterministic. dir is a snapshot folder or zip.
    if (!dir || !*dir || !dbOk_) return;
    SnapLoad s;
    if (!LoadSnapshot(dir, &s)) SetNotice(L"Could not load the fixture snapshot: " + s.error, 2);
    else if (demoMode_ == L"plan" || demoMode_ == L"edit") SetupWriteDemo(s.vars);   // MockBackend only
    else ApplySnapshot(std::move(s));
    Refilter();
}

// An in-memory snapshot for the selftest (the folder and zip sources are tested separately).
struct MemSource : FileSource {
    std::map<std::wstring, std::string> files;
    int reads = 0;
    bool Read(const std::wstring& rel, std::string* out, size_t cap, std::wstring* err) override {
        reads++;
        auto it = files.find(rel);
        if (it == files.end()) { if (err) *err = L"missing"; return false; }
        if (it->second.size() > cap) { if (err) *err = L"too large"; return false; }
        *out = it->second;
        return true;
    }
};

const uint8_t kSampleZip[] = {
#include "modules/bios/selftest_zip.inc"
};

// The 9 settings tests/fixtures/make_bios_snapshot.py changes, from tests/golden/legacy/bios-snapshot-expected.json.
struct Expect { int id; const wchar_t* prompt; long long value, def; };
const Expect kExpected[] = {
    {417, L"Hyper-Threading", 0, 1}, {448, L"Intel Turbo Boost", 0, 1}, {439, L"Intel Speed Shift Technology", 0, 2},
    {437, L"EIST", 0, 1}, {1373, L"CSM Support", 1, 0}, {1290, L"Re-Size BAR Support", 0, 1},
    {1289, L"Above 4G memory/Crypto Currency mining", 0, 1}, {409, L"Hardware Prefetcher", 0, 1},
    {410, L"Adjacent Cache Line Prefetch", 0, 1},
};

// A host for the selftest's switch and plan-overlay checks. Prefs() counts its calls: the write switch
// must never touch settings (it is never persisted).
struct TestHost : IHost {
    sys::SettingsStore store;
    sys::Settings prefs{&store, L"bios"};
    ui::Fonts fonts;
    int prefsCalls = 0;
    std::wstring status;
    HWND Frame() override { return nullptr; }
    UINT Dpi() override { return 96; }
    void SetTitle(const wchar_t*) override {}
    void SetStatus(const wchar_t* t) override { status = t ? t : L""; }
    void SetBusy(bool) override {}
    bool IsElevated() override { return true; }
    sys::Settings& Prefs() override { prefsCalls++; return prefs; }
    void Log(LogLevel, const wchar_t*) override {}
    const ui::Theme& Theme() override { return ui::Baseline(); }
    const ui::Fonts& Fonts() override { return fonts; }
    ui::Layout Layout() override { return ui::Layout::Baseline; }
};

std::string ReadSmall(const std::wstring& path) {
    std::string s;
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;
    char buf[65536];
    DWORD got = 0;
    if (ReadFile(h, buf, sizeof(buf), &got, nullptr)) s.assign(buf, got);
    CloseHandle(h);
    return s;
}

// Runs Execute on its own thread (the write-lock selftest: a mutex is recursive within one thread).
struct ExecRun {
    MockBackend* m;
    const Plan* p;
    std::wstring root;
    ExecOptions opt;
    ExecResult r;
    bool ok = false;
};
DWORD WINAPI ExecRunThread(LPVOID a) {
    ExecRun* x = static_cast<ExecRun*>(a);
    x->ok = Execute(*x->m, *x->p, x->root, {}, {}, &x->r, x->opt);
    return 0;
}

// True under Wine (ntdll exports wine_get_version there, never on Windows).
bool IsWine() {
    HMODULE n = GetModuleHandleW(L"ntdll.dll");
    return n && GetProcAddress(n, "wine_get_version") != nullptr;
}

// The selftest's own admin-only descriptor (kBackupSddl), for fixtures it plants by hand.
struct TestSd {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    explicit TestSd(const wchar_t* sddl = kBackupSddl) {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) {
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
        } else {
            sd = nullptr;
        }
    }
    ~TestSd() { if (sd) LocalFree(sd); }
    TestSd(const TestSd&) = delete;
    TestSd& operator=(const TestSd&) = delete;
};
// Opens one of the selftest's own fixtures by handle (never following a link).
HANDLE OpenFixture(const std::wstring& path, DWORD access) {
    return CreateFileW(sys::ToExtended(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}
// Replaces a selftest fixture's DACL from SDDL, through a handle (nothing sets security by path);
// ERROR_SUCCESS or the error.
DWORD SetFixtureDacl(const std::wstring& path, const wchar_t* sddl) {
    TestSd t(sddl);
    if (!t.sd) return GetLastError();
    PACL dacl = nullptr;
    BOOL present = FALSE, def = FALSE;
    if (!GetSecurityDescriptorDacl(t.sd, &present, &dacl, &def)) return GetLastError();
    HANDLE h = OpenFixture(path, READ_CONTROL | WRITE_DAC);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD e = SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              dacl, nullptr);
    CloseHandle(h);
    return e;
}
// HandleTrusted on a fixture.
bool FixtureTrusted(const std::wstring& path, std::wstring* why) {
    HANDLE h = OpenFixture(path, READ_CONTROL);
    if (h == INVALID_HANDLE_VALUE) { *why = Shell(path) + L": " + sys::ErrorText(GetLastError()); return false; }
    bool ok = HandleTrusted(h, why);
    CloseHandle(h);
    return ok;
}
// "O:<this process's user SID>": a non-admin owner the token may assign.
std::wstring UserOwnerSddl() {
    HANDLE tok = nullptr;
    std::wstring r;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return r;
    BYTE buf[256];
    DWORD n = 0;
    LPWSTR s = nullptr;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &s) && s) {
        r = std::wstring(L"O:") + s;
        LocalFree(s);
    }
    CloseHandle(tok);
    return r;
}
// Takes the named mutex (created like Hadron's, with kLockSddl) and ends without releasing it: the
// mutex is then abandoned (the caller keeps its own handle open so the mutex outlives this thread).
DWORD WINAPI AbandonLockThread(LPVOID a) {
    TestSd sd(kLockSddl);
    HANDLE m = CreateMutexW(sd.sd ? &sd.sa : nullptr, FALSE, static_cast<const wchar_t*>(a));
    DWORD r = m && WaitForSingleObject(m, 0) == WAIT_OBJECT_0 ? 1 : 0;
    if (m) CloseHandle(m);   // closing doesn't release ownership
    return r;
}

// Removes the folder tree this selftest run created (its own fixtures, hard rule 1 doesn't cover them).
// Never follows a link: a reparse point anywhere stops the removal and leaves it in place.
bool RemoveSelftestTree(const std::wstring& dir, std::wstring* err, int depth = 0) {
    if (depth > 16) { *err = L"too deep: " + Shell(dir); return false; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(sys::ToExtended(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) { *err = Shell(dir) + L": " + sys::ErrorText(e); return false; }
    } else {
        bool ok = true;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { *err = L"a link was found, left in place: " + Shell(p); ok = false; break; }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!RemoveSelftestTree(p, err, depth + 1)) { ok = false; break; } }
            else if (!DeleteFileW(sys::ToExtended(p).c_str())) { *err = Shell(p) + L": " + sys::ErrorText(GetLastError()); ok = false; break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!ok) return false;
    }
    if (!RemoveDirectoryW(sys::ToExtended(dir).c_str())) { *err = Shell(dir) + L": " + sys::ErrorText(GetLastError()); return false; }
    return true;
}

// docs/design/bios-write.md "Selftests": everything through MockBackend, backups under root (a fresh
// folder in %TEMP% that RunCli removes afterwards), never %ProgramData%, never real firmware.
void BiosModule::SelftestWrites(const std::function<void(bool, const std::wstring&)>& check, const std::wstring& root) {
    std::wstring err;
    // A small map: Setup (8 bytes) with a oneof, a 2-byte numeric (step 5), a checkbox, a 1-byte numeric that
    // shares the numeric's high byte, and a oneof past the end; CpuSetup (4-byte numeric, no step); PchSetup
    // (hidden oneof); AuthVar (authenticated, refused).
    Db d;
    bool parsed = d.Parse(
        "V\tt\nB\tMS-7E06\t7E06vAF\nK\t0\tk\te\t\t\t\t\t\tn\nT\t1\n"
        "S\t0\tSetup\tg1\t0\t1\toneof\t\t0\t\t\t\tOneOf\t\tM > A\t0\x1fOff\x1e" "1\x1fOn\t0\n"
        "S\t1\tSetup\tg1\t2\t2\tnumeric\t\t0\t0\t1000\t5\tNum\t\tM > B\t\t0\n"
        "S\t2\tSetup\tg1\t1\t1\tcheckbox\t\t0\t\t\t\tBox\t\tM\t\t0\n"
        "S\t3\tCpuSetup\tg2\t0\t4\tnumeric\t\t0\t0\t100000\t0\tWide\t\tM\t\t0\n"
        "S\t4\tPchSetup\tg3\t0\t1\toneof\tH\t0\t\t\t\tHid\t\tM\t0\x1fOff\x1e" "1\x1fOn\t0\n"
        "S\t5\tSetup\tg1\t3\t1\tnumeric\t\t0\t0\t255\t1\tHighByte\t\tM\t\t0\n"
        "S\t6\tSetup\tg1\t10\t1\toneof\t\t0\t\t\t\tPast\t\tM\t0\x1fOff\x1e" "1\x1fOn\t0\n"
        "S\t7\tAuthVar\tg4\t0\t1\toneof\t\t0\t\t\t\tAuth\t\tM\t0\x1fOff\x1e" "1\x1fOn\t0\n",
        &err);
    check(parsed && d.S.size() == 8, L"write map: synthetic settings parse");
    if (!parsed || d.S.size() != 8) return;
    auto freshMock = [] {
        auto m = std::make_unique<MockBackend>();
        m->Put(L"Setup", L"g1", {0, 0, 0x0A, 0x00, 0x11, 0x22, 0x33, 0x44});   // Num = 10, Box = 0, OneOf = 0
        m->Put(L"CpuSetup", L"g2", {0x10, 0x27, 0, 0});                       // Wide = 10000
        m->Put(L"PchSetup", L"g3", {0, 9, 9, 9});
        m->Put(L"AuthVar", L"g4", {0, 0, 0, 0}, 0x27);
        m->smbios.ok = true;
        m->smbios.boardProduct = L"PRO Z790-P WIFI (MS-7E06)";
        m->smbios.biosVersion = L"A.F0";
        return m;
    };
    auto liveOf = [&](MockBackend& m) {
        std::vector<FwVar> lv;
        for (auto& v : d.Variables()) { FwVar f; f.name = v.first; f.guid = v.second; m.Read(&f); lv.push_back(f); }
        return lv;
    };
    const PlanGate open{true, true, true};
    std::wstring why;
    // Every Execute here takes this run's own lock, never the real Global\Hadron.bios.fwwrite.
    const std::wstring lockName = L"Local\\Hadron.bios.fwwrite.selftest-" + std::to_wstring(GetCurrentProcessId());
    check(std::wstring(kWriteMutexName) == L"Global\\Hadron.bios.fwwrite" && lockName != kWriteMutexName,
          L"lock: the write lock is Global\\Hadron.bios.fwwrite (firmware is machine-wide); this selftest uses its own");

    // --- trust policy, on descriptors built in memory ---
    {
        auto judge = [&](const wchar_t* sddl, std::wstring* w) {
            TestSd t(sddl);
            return t.sd && SdTrusted(t.sd, w);
        };
        std::wstring w1, w2, w3, w4, w5, w6, w7, w8, w9, w10;
        bool adminOnly = judge(kBackupSddl, &w1);
        bool extraRead = judge(L"O:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;WD)(D;;FA;;;AN)", &w2);
        bool usersWrite = !judge(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;FW;;;BU)", &w3);
        bool everyone = !judge(L"O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;WD)", &w4);
        bool nullDacl = false;
        {
            // Built in code: Wine's SDDL parser turns NO_ACCESS_CONTROL into an empty DACL, not a NULL one.
            SECURITY_DESCRIPTOR nd;
            BYTE ba[SECURITY_MAX_SID_SIZE];
            DWORD n = sizeof(ba);
            nullDacl = InitializeSecurityDescriptor(&nd, SECURITY_DESCRIPTOR_REVISION) &&
                       CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, ba, &n) &&
                       SetSecurityDescriptorOwner(&nd, ba, FALSE) && SetSecurityDescriptorDacl(&nd, TRUE, nullptr, FALSE) &&
                       !SdTrusted(&nd, &w5);
        }
        bool noDacl = !judge(L"O:BA", &w6);
        bool userOwner = !judge(L"O:S-1-5-21-1-2-3-1001D:P(A;;FA;;;SY)(A;;FA;;;BA)", &w7);
        bool genAll = !judge(L"O:BAD:P(A;;FA;;;BA)(A;IO;GA;;;AU)", &w8);   // inherit-only counts too
        bool del = !judge(L"O:BAD:P(A;;FA;;;BA)(A;;SD;;;AU)", &w9);
        bool dac = !judge(L"O:BAD:P(A;;FA;;;BA)(A;;WD;;;S-1-5-21-1-2-3-1001)", &w10);
        check(adminOnly && extraRead, L"trust: admin-only (and admin-only plus read for others) passes" +
              (adminOnly ? L"" : L" (" + w1 + L")") + (extraRead ? L"" : L" (" + w2 + L")"));
        check(usersWrite && everyone && nullDacl && noDacl && userOwner,
              L"trust: Users:write, Everyone:full, a NULL DACL, no DACL and a user owner are each refused (" + w3 + L"; " + w4 +
              L"; " + w5 + L"; " + w6 + L"; " + w7 + L")");
        check(genAll && del && dac, L"trust: GENERIC_ALL (inherit-only), DELETE and WRITE_DAC to others are refused (" + w8 + L"; " +
              w9 + L"; " + w10 + L")");
    }
    // On disk: Windows applies the descriptor; Wine may keep only part of it. The selftest probes once
    // and, where the OS doesn't keep ACLs, judges paths with an injected check that refuses only the
    // paths a test marks, so the rest of the restore logic still runs.
    std::set<std::wstring> distrusted;
    bool realAcl = false;
    const bool wine = IsWine();
    {
        // The real path: PrepareBackupRoot creates <run>\Hadron and \bios-backups admin-only (O:BA), then
        // checks both by handle; a file written with that descriptor passes, and fails once opened up.
        std::wstring w, e;
        TestSd t;
        realAcl = PrepareBackupRoot(root, &e);
        if (realAcl) {
            std::wstring f = root + L"\\acl-probe.txt";
            realAcl = WriteNewFile(f, "x", &e, &t.sa) && FixtureTrusted(f, &w) && SetFixtureDacl(f, L"D:P(A;;FA;;;WD)") == ERROR_SUCCESS &&
                      !FixtureTrusted(f, &w);
            if (realAcl) w.clear();
            DeleteFileW(sys::ToExtended(f).c_str());
        }
        // On Windows this must hold (elevated, as every Hadron exe is): a runner that can't create an
        // O:BA folder fails here, visibly. Only Wine, which doesn't keep file ACLs, may skip it.
        check(realAcl || wine, std::wstring(L"trust: file ACLs work here (admin-only root created and checked by handle)") +
              (realAcl ? L"" : (wine ? L" [Wine: not kept; the restore tests inject the check]" : L"") + std::wstring(L" (") + e +
                                    (w.empty() ? L"" : L"; " + w) + L")"));
    }
    TrustFn trust;   // empty = the real HandleTrusted
    if (!realAcl)
        trust = [&distrusted](HANDLE, const std::wstring& finalPath, std::wstring* w) {
            std::wstring s = sys::ToShell(finalPath);
            for (auto& d : distrusted)   // the marked path itself, or anything inside it
                if (s.size() >= d.size() && CompareStringOrdinal(s.c_str(), (int)d.size(), d.c_str(), (int)d.size(), TRUE) == CSTR_EQUAL &&
                    (s.size() == d.size() || s[d.size()] == L'\\')) {
                    *w = L"marked writable by the selftest";
                    return false;
                }
            return true;
        };
    auto XO = [&](const std::wstring& stamp) {
        ExecOptions o;
        o.stamp = stamp;
        o.lockName = lockName;
        o.trust = trust;
        return o;
    };
    // Makes path writable by Everyone (really, by handle, or by marking it for the injected check).
    auto makeWritable = [&](const std::wstring& path) {
        if (!realAcl) { distrusted.insert(sys::ToShell(sys::ToExtended(path))); return true; }
        return SetFixtureDacl(path, L"D:P(A;OICI;FA;;;WD)") == ERROR_SUCCESS;
    };
    // Undoes makeWritable: the backup DACL again (the owner was never changed).
    auto makeAdminOnly = [&](const std::wstring& path) {
        if (!realAcl) { distrusted.erase(sys::ToShell(sys::ToExtended(path))); return true; }
        return SetFixtureDacl(path, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;AU)") == ERROR_SUCCESS;
    };

    // --- ValidateValue ---
    check(ValidateValue(d.S[0], 1, &why) && ValidateValue(d.S[0], 0, &why) && !ValidateValue(d.S[0], 2, &why),
          L"ValidateValue: oneof takes option values only");
    check(ValidateValue(d.S[2], 0, &why) && ValidateValue(d.S[2], 1, &why) && !ValidateValue(d.S[2], 2, &why) &&
          !ValidateValue(d.S[2], -1, &why), L"ValidateValue: checkbox takes 0 and 1 only");
    check(ValidateValue(d.S[1], 0, &why) && ValidateValue(d.S[1], 5, &why) && ValidateValue(d.S[1], 1000, &why),
          L"ValidateValue: numeric range ends and steps accepted");
    check(!ValidateValue(d.S[1], 3, &why) && !ValidateValue(d.S[1], 1005, &why) && !ValidateValue(d.S[1], -5, &why) &&
          !ValidateValue(d.S[1], 1001, &why), L"ValidateValue: off-step, out of range and negative (mn = 0) refused");
    check(ValidateValue(d.S[3], 12345, &why) && !ValidateValue(d.S[3], 100001, &why), L"ValidateValue: step 0 = any value in range");
    {
        Setting odd = d.S[0]; odd.kind = L"string";
        Setting w3 = d.S[0]; w3.width = 3;
        check(!ValidateValue(odd, 0, &why) && !ValidateValue(w3, 0, &why), L"ValidateValue: unknown kind and width 3 refused");
    }
    // --- EncodeValue ---
    {
        uint8_t b[8] = {};
        bool w1 = EncodeValue(0xFF, 1, b) && b[0] == 0xFF;
        bool w1o = !EncodeValue(0x100, 1, b);
        bool w2 = EncodeValue(0x1234, 2, b) && b[0] == 0x34 && b[1] == 0x12;
        bool w2o = !EncodeValue(0x10000, 2, b);
        bool w4 = EncodeValue(0x12345678, 4, b) && b[0] == 0x78 && b[3] == 0x12;
        bool w4o = !EncodeValue(0x100000000LL, 4, b);
        bool w8 = EncodeValue(0x0102030405060708LL, 8, b) && b[0] == 0x08 && b[7] == 0x01;
        bool neg = !EncodeValue(-1, 8, b) && !EncodeValue(-1, 1, b);
        bool bad = !EncodeValue(1, 3, b) && !EncodeValue(1, 0, b);
        check(w1 && w2 && w4 && w8, L"EncodeValue: widths 1/2/4/8, little-endian");
        check(w1o && w2o && w4o && neg && bad, L"EncodeValue: overflow, negative and odd widths refused");
        bool rt = true;
        for (long long v : {0LL, 1LL, 255LL, 65535LL, 4294967295LL, 0x7FFFFFFFFFFFFFFFLL}) {
            for (int wdt : {1, 2, 4, 8}) {
                uint8_t e[8];
                if (!EncodeValue(v, wdt, e)) continue;
                long long back = -1;
                if (!DecodeValue(std::vector<uint8_t>(e, e + wdt), 0, wdt, &back) || back != v) rt = false;
            }
        }
        check(rt, L"EncodeValue: round-trips through DecodeValue");
    }
    // --- BuildPlan ---
    {
        auto m = freshMock();
        auto lv = liveOf(*m);
        Plan pl;
        std::vector<StagedChange> st = {{0, 1}};
        check(!BuildPlan(d, st, lv, PlanGate{false, true, true}, &pl, &why) &&
              !BuildPlan(d, st, lv, PlanGate{true, false, true}, &pl, &why) &&
              !BuildPlan(d, st, lv, PlanGate{true, true, false}, &pl, &why) && pl.vars.empty(),
              L"BuildPlan: refused when not live, guard unmatched, or switch off");
        int badAttrs = 0;
        for (DWORD bit : {0x8u, 0x10u, 0x20u, 0x40u, 0x80u, 0x100u}) {
            auto lv2 = lv;
            for (auto& v : lv2) if (v.name == L"Setup") v.attr = 0x7 | bit;
            if (!BuildPlan(d, st, lv2, open, &pl, &why)) badAttrs++;
        }
        for (DWORD bit : {0x1u, 0x2u, 0x4u}) {
            auto lv2 = lv;
            for (auto& v : lv2) if (v.name == L"Setup") v.attr = 0x7 & ~bit;
            if (!BuildPlan(d, st, lv2, open, &pl, &why)) badAttrs++;
        }
        check(badAttrs == 9, L"BuildPlan: attributes 0x8/0x10/0x20/0x40/0x80/unknown refused, missing NV/BS/RT refused (" +
              std::to_wstring(badAttrs) + L" of 9)");
        bool auth = !BuildPlan(d, {{7, 1}}, lv, open, &pl, &why) && why.find(L"time-based authenticated (0x20)") != std::wstring::npos;
        check(auth, L"BuildPlan: a time-based authenticated variable (attributes 0x27) is refused (" + why + L")");
        {
            auto lv2 = lv;
            lv2.erase(std::remove_if(lv2.begin(), lv2.end(), [](const FwVar& v) { return v.name == L"CpuSetup"; }), lv2.end());
            auto lv3 = lv;
            for (auto& v : lv3) if (v.name == L"CpuSetup") { v.err = ERROR_ENVVAR_NOT_FOUND; v.data.clear(); }
            check(!BuildPlan(d, {{3, 5}}, lv2, open, &pl, &why) && !BuildPlan(d, {{3, 5}}, lv3, open, &pl, &why),
                  L"BuildPlan: missing or unread variable refused");
        }
        bool overlap = !BuildPlan(d, {{1, 20}, {5, 7}}, lv, open, &pl, &why) && why.find(L"same bytes") != std::wstring::npos;
        check(overlap, L"BuildPlan: overlapping settings refused (" + why + L")");
        bool past = !BuildPlan(d, {{6, 1}}, lv, open, &pl, &why) && why.find(L"past the end") != std::wstring::npos;
        check(past, L"BuildPlan: offset + width past the end refused (" + why + L")");
        check(!BuildPlan(d, {{1, 3}}, lv, open, &pl, &why) && !BuildPlan(d, {{0, 7}}, lv, open, &pl, &why),
              L"BuildPlan: values outside the option list / step refused");
        check(!BuildPlan(d, {{0, 0}}, lv, open, &pl, &why) && why.find(L"Nothing to write") == 0,
              L"BuildPlan: a lone no-op is refused as nothing to write");
        bool ok = BuildPlan(d, {{0, 0}, {0 + 2, 1}, {1, 20}, {3, 50000}}, lv, open, &pl, &why);
        bool notes = ok && pl.notes.size() == 1 && pl.notes[0].find(L"OneOf") == 0;
        check(ok && notes, L"BuildPlan: no-op dropped with a note" + (ok ? L"" : L": " + why));
        bool oneVar = ok && pl.vars.size() == 2 && pl.vars[0].name == L"Setup" && pl.vars[0].settings.size() == 2 &&
                      pl.vars[1].name == L"CpuSetup";
        check(oneVar, L"BuildPlan: two settings in one variable -> one variable entry");
        bool onlyPlanned = ok;
        if (ok) {
            const PlanVar& s = pl.vars[0];
            std::vector<uint8_t> want = {0, 1, 20, 0, 0x11, 0x22, 0x33, 0x44};
            onlyPlanned = s.newData == want && s.oldData == m->vars[MakeVarKey(L"Setup", L"g1")].data &&
                          pl.vars[1].newData == std::vector<uint8_t>({0x50, 0xC3, 0, 0}) && s.attr == 0x7;
        }
        check(onlyPlanned, L"BuildPlan: newData differs from oldData only in the planned bytes");
        bool shares = ok && pl.vars[0].settings.size() == 2 && pl.vars[0].settings[1].prompt == L"Num" &&
                      pl.vars[0].settings[1].shares.find(L"HighByte") == 0;
        check(shares, L"BuildPlan: a setting sharing the written bytes is named in the plan");
        Plan hp;
        check(BuildPlan(d, {{4, 1}}, lv, open, &hp, &why) && hp.vars.size() == 1 && !hp.vars[0].settings[0].flags.empty(),
              L"BuildPlan: hidden setting flagged");
    }
    // --- Execute ---
    std::wstring happyDir;
    std::vector<uint8_t> setupOld, cpuOld;
    {
        auto m = freshMock();
        setupOld = m->vars[MakeVarKey(L"Setup", L"g1")].data;
        cpuOld = m->vars[MakeVarKey(L"CpuSetup", L"g2")].data;
        Plan pl;
        bool planned = BuildPlan(d, {{0, 1}, {2, 1}, {3, 50000}}, liveOf(*m), open, &pl, &why);
        ExecResult r;
        bool ok = planned && Execute(*m, pl, root, {}, {}, &r, XO(L"20260101-000000"));
        happyDir = r.backupDir;
        std::string j = ReadSmall(r.backupDir + L"\\journal.txt");
        check(ok && r.written == 2 && j == "BEGIN Setup g1\r\nOK Setup\r\nBEGIN CpuSetup g2\r\nOK CpuSetup\r\nEND\r\n",
              L"Execute: happy path, journal BEGIN/OK/END" + (ok ? L"" : L": " + r.error));
        check(ok && m->vars[MakeVarKey(L"Setup", L"g1")].data == pl.vars[0].newData &&
              m->vars[MakeVarKey(L"CpuSetup", L"g2")].data == pl.vars[1].newData && m->writes == 2,
              L"Execute: the mock variables now hold newData (2 writes)");
        SnapLoad bl;
        bool loaded = LoadSnapshot(r.backupDir, &bl);
        check(loaded && bl.vars[MakeVarKey(L"Setup", L"g1")] == setupOld && bl.vars[MakeVarKey(L"CpuSetup", L"g2")] == cpuOld &&
              bl.loaded == 2, L"Execute: backup bins equal the old data; Load snapshot reads the backup folder");
        check(loaded && BoardMatches(d.board, bl.sysBoard) && BiosMatches(d.bios, bl.sysBios),
              L"Execute: the backup's system.txt passes the board/BIOS guard");
        Db dd = d;
        dd.Apply(bl.vars, false);
        check(loaded && dd.S[0].hasCur && dd.S[0].cur == 0 && dd.S[3].cur == 10000, L"Execute: the backup decodes to the old values");
        check(ReadSmall(r.backupDir + L"\\plan.txt").find("Write 3 settings to 2 variables") != std::string::npos,
              L"Execute: plan.txt holds the human plan");
        check(r.backupDir.size() > 15 && r.backupDir.compare(r.backupDir.size() - 15, 15, L"20260101-000000") == 0,
              L"Execute: first session folder is <stamp>");
        // A second session in the same second gets -2; nothing is reused.
        auto m2 = freshMock();
        Plan p2;
        ExecResult r2;
        bool ok2 = BuildPlan(d, {{0, 1}}, liveOf(*m2), open, &p2, &why) &&
                   Execute(*m2, p2, root, {}, {}, &r2, XO(L"20260101-000000"));
        check(ok2 && r2.backupDir == r.backupDir + L"-2", L"Execute: same stamp twice -> \"-2\" folder (" + r2.backupDir + L")");
        std::vector<std::wstring> open1;
        check(!JournalIncomplete(j, &open1), L"Execute: a complete journal is not flagged");
    }
    {
        // Drift: one unplanned byte changes between plan and execute -> nothing written.
        auto m = freshMock();
        Plan pl;
        BuildPlan(d, {{0, 1}}, liveOf(*m), open, &pl, &why);
        m->vars[MakeVarKey(L"Setup", L"g1")].data[7] ^= 1;
        ExecResult r;
        bool ok = Execute(*m, pl, root, {}, {}, &r, XO(L"20260101-000001"));
        check(!ok && m->writes == 0 && !r.started && r.error.find(L"changed since it was read") != std::wstring::npos,
              L"Execute: drift in an unplanned byte -> nothing written, no backup session");
    }
    {
        auto m = freshMock();
        Plan pl;
        BuildPlan(d, {{0, 1}}, liveOf(*m), open, &pl, &why);
        m->smbios.biosVersion = L"A.H0";
        ExecResult r;
        bool mis = !Execute(*m, pl, root, {}, {}, &r, XO(L"")) && m->writes == 0;
        m->smbios.biosVersion = L"A.F0";
        m->smbiosErr = ERROR_INVALID_DATA;
        bool noSmb = !Execute(*m, pl, root, {}, {}, &r, XO(L"")) && m->writes == 0;
        check(mis && noSmb, L"Execute: SMBIOS mismatch or unreadable -> nothing written");
        m->smbiosErr = 0;
        m->privilege = false;
        bool noPriv = !Execute(*m, pl, root, {}, {}, &r, XO(L"")) && m->writes == 0 && r.error.find(L"SeSystemEnvironmentPrivilege") == 0;
        check(noPriv, L"Execute: without the thread privilege -> nothing written");
        m->privilege = true;
        bool cancelled = !Execute(*m, pl, root, [] { return true; }, {}, &r, XO(L"")) && m->writes == 0;
        check(cancelled, L"Execute: cancelled before writing -> nothing written");
    }
    {
        // Write error on variable 2 of 3.
        auto m = freshMock();
        Plan pl;
        bool planned = BuildPlan(d, {{0, 1}, {3, 5}, {4, 1}}, liveOf(*m), open, &pl, &why);
        m->failWriteOf = L"CpuSetup";
        m->failWriteErr = ERROR_WRITE_PROTECT;
        ExecResult r;
        bool ok = Execute(*m, pl, root, {}, {}, &r, XO(L"20260101-000002"));
        bool states = planned && r.vars.size() == 3 && r.vars[0].state == VarState::Written &&
                      r.vars[1].state == VarState::Failed && r.vars[1].err == ERROR_WRITE_PROTECT &&
                      r.vars[2].state == VarState::NotAttempted;
        bool bytes = m->vars[MakeVarKey(L"Setup", L"g1")].data == pl.vars[0].newData &&
                     m->vars[MakeVarKey(L"CpuSetup", L"g2")].data == cpuOld &&
                     m->vars[MakeVarKey(L"PchSetup", L"g3")].data == std::vector<uint8_t>({0, 9, 9, 9});
        std::string j = ReadSmall(r.backupDir + L"\\journal.txt");
        check(!ok && states && bytes, L"Execute: write error on var 2 of 3 -> 1 written, 2 failed, 3 not attempted");
        check(j == "BEGIN Setup g1\r\nOK Setup\r\nBEGIN CpuSetup g2\r\nFAILED CpuSetup 19\r\nEND\r\n" && !JournalIncomplete(j, nullptr),
              L"Execute: journal records FAILED with the error code");
    }
    {
        // Read-back mismatch: the mock corrupts the stored bytes once -> restored to oldData.
        auto m = freshMock();
        Plan pl;
        BuildPlan(d, {{0, 1}, {3, 5}}, liveOf(*m), open, &pl, &why);
        m->corruptWriteOf = L"Setup";
        ExecResult r;
        bool ok = Execute(*m, pl, root, {}, {}, &r, XO(L"20260101-000003"));
        std::string j = ReadSmall(r.backupDir + L"\\journal.txt");
        check(!ok && r.vars[0].state == VarState::Restored && m->vars[MakeVarKey(L"Setup", L"g1")].data == setupOld &&
              r.vars[1].state == VarState::NotAttempted && m->writes == 2,
              L"Execute: read-back mismatch -> old bytes written back and verified, the rest not attempted");
        check(j.find("BEGIN Setup g1\r\nRESTORED Setup\r\n") == 0 && !JournalIncomplete(j, nullptr), L"Execute: journal RESTORED");
    }
    // --- Restore ---
    if (!happyDir.empty()) {
        auto m = freshMock();
        Plan pl;
        ExecResult r;
        BuildPlan(d, {{0, 1}, {2, 1}, {3, 50000}}, liveOf(*m), open, &pl, &why);
        Execute(*m, pl, root, {}, {}, &r, XO(L"20260101-000004"));   // m now holds the new values
        std::string man = ReadSmall(r.backupDir + L"\\manifest.txt");
        std::wstring shaOld;
        Sha256Hex(setupOld, &shaOld, &err);
        check(man.find("Hadron BIOS backup manifest 2\r\nboard\tPRO Z790-P WIFI (MS-7E06)\r\nbios\tA.F0\r\nSetup\tg1\t8\t00000007\t" +
                       ToUtf8(shaOld) + "\r\n") == 0,
              L"Restore: the session's manifest.txt lists name, guid, size, attributes and SHA-256");
        std::wstring shaAbc;
        check(Sha256Hex({'a', 'b', 'c'}, &shaAbc, &err) &&
              shaAbc == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"SHA-256(\"abc\") is the FIPS 180-2 vector");
        SnapLoad bl;
        bool loaded = LoadBackupSession(root, r.backupDir, &bl, trust);
        check(loaded, L"Restore: a genuine session under the backup root loads" + (loaded ? L"" : L": " + bl.error));
        Plan rp;
        bool planned = loaded && BuildRestorePlan(d, bl.vars, bl.label, bl.sysBoard, bl.sysBios, liveOf(*m), open, &rp, &why);
        std::set<std::wstring> prompts;
        if (planned) for (auto& v : rp.vars) for (auto& s : v.settings) prompts.insert(s.prompt);
        check(planned && rp.restore && rp.vars.size() == 2 && prompts.count(L"OneOf") && prompts.count(L"Box") &&
              prompts.count(L"Wide") && !prompts.count(L"Num"),
              L"Restore: the plan lists the differing settings (OneOf, Box, Wide), not the unchanged ones" + (planned ? L"" : L": " + why));
        Plan other;
        check(!BuildRestorePlan(d, bl.vars, bl.label, L"MS-7D25", bl.sysBios, liveOf(*m), open, &other, &why),
              L"Restore: a backup from another board is refused");
        {
            VarMap big = bl.vars;
            big[MakeVarKey(L"Setup", L"g1")].push_back(0);
            Plan sp;
            check(!BuildRestorePlan(d, big, bl.label, bl.sysBoard, bl.sysBios, liveOf(*m), open, &sp, &why) &&
                  why.find(L"bytes") != std::wstring::npos, L"Restore: a size mismatch is refused");
            check(!BuildRestorePlan(d, bl.vars, bl.label, bl.sysBoard, bl.sysBios, liveOf(*m), PlanGate{true, true, false}, &sp, &why),
                  L"Restore: same gate as writes (switch off -> refused)");
        }
        ExecResult rr;
        bool restored = planned && Execute(*m, rp, root, {}, {}, &rr, XO(L"20260101-000005"));
        check(restored && m->vars[MakeVarKey(L"Setup", L"g1")].data == setupOld && m->vars[MakeVarKey(L"CpuSetup", L"g2")].data == cpuOld,
              L"Restore: execute puts the backup's bytes back (with its own backup session first)" +
              (restored ? L"" : L": " + rr.error));
        SnapLoad pre;
        check(restored && LoadSnapshot(rr.backupDir, &pre) && pre.vars[MakeVarKey(L"Setup", L"g1")] == pl.vars[0].newData,
              L"Restore: its own backup holds the bytes it replaced");

        // Only Hadron's own sessions: a hand-edited .bin, a folder outside the root and a plain Save
        // snapshot folder are refused.
        {
            auto m3 = freshMock();
            Plan p3;
            ExecResult r3;
            bool made = BuildPlan(d, {{0, 1}}, liveOf(*m3), open, &p3, &why) &&
                        Execute(*m3, p3, root, {}, {}, &r3, XO(L"20260101-000006"));
            bool edited = false;
            WIN32_FIND_DATAW fd;
            HANDLE fh = made ? FindFirstFileW(sys::ToExtended(r3.backupDir + L"\\vars\\0001_*.bin").c_str(), &fd) : INVALID_HANDLE_VALUE;
            if (fh != INVALID_HANDLE_VALUE) {
                FindClose(fh);
                HANDLE f = CreateFileW(sys::ToExtended(r3.backupDir + L"\\vars\\" + fd.cFileName).c_str(), GENERIC_WRITE, 0,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    const uint8_t x = 0x5A;   // this selftest's own fixture: one byte changed in place
                    DWORD put = 0;
                    edited = WriteFile(f, &x, 1, &put, nullptr) && put == 1;
                    CloseHandle(f);
                }
            }
            SnapLoad s3;
            bool refused = edited && !LoadBackupSession(root, r3.backupDir, &s3, trust) && s3.error.find(L"SHA-256") != std::wstring::npos &&
                           s3.vars.empty();
            check(refused, L"Restore: a hand-edited .bin is refused (" + s3.error + L")");
        }
        {
            auto m4 = freshMock();
            Plan p4;
            ExecResult r4;
            bool made = BuildPlan(d, {{0, 1}}, liveOf(*m4), open, &p4, &why) &&
                        Execute(*m4, p4, root + L"\\elsewhere", {}, {}, &r4, XO(L"20260101-000007"));
            SnapLoad s4, s5;
            bool outside = made && !LoadBackupSession(root, r4.backupDir, &s4, trust) && s4.error.find(L"directly inside") != std::wstring::npos;
            bool rootItself = !LoadBackupSession(root, root, &s5, trust) && !s5.error.empty();
            check(outside && rootItself, L"Restore: a genuine session outside the backup root (and the root itself) is refused (" + s4.error + L")");
        }
        {
            auto m5 = freshMock();
            SaveResult sr;
            SmbiosInfo smb;
            DWORD se = 0;
            m5->Smbios(&smb, &se);
            bool saved = SaveSnapshot(root, liveOf(*m5), smb, &sr);
            SnapLoad s6, s7;
            bool plainLoads = saved && LoadSnapshot(sr.folder, &s7);
            bool refused = saved && !LoadBackupSession(root, sr.folder, &s6, trust) && (s6.error.find(L"isn't a Hadron write backup") != std::wstring::npos || s6.error.find(L"isn't trusted") != std::wstring::npos);
            check(plainLoads && refused, L"Restore: a plain Save snapshot folder under the root loads as a snapshot but is refused as a backup (" +
                  s6.error + L")");
        }
        // The guard's board/BIOS come from the hashed manifest; a system.txt edited to another BIOS is refused.
        {
            auto m6 = freshMock();
            Plan p6;
            ExecResult r6;
            bool made = BuildPlan(d, {{0, 1}}, liveOf(*m6), open, &p6, &why) &&
                        Execute(*m6, p6, root, {}, {}, &r6, XO(L"20260101-000012"));
            std::string man6 = ReadSmall(r6.backupDir + L"\\manifest.txt");
            SnapLoad g6, e6;
            bool v2 = man6.find("Hadron BIOS backup manifest 2\r\nboard\tPRO Z790-P WIFI (MS-7E06)\r\nbios\tA.F0\r\n") == 0;
            bool fromManifest = made && LoadBackupSession(root, r6.backupDir, &g6, trust) && g6.sysBoard == L"PRO Z790-P WIFI (MS-7E06)" &&
                                g6.sysBios == L"A.F0";
            check(v2 && fromManifest, L"Restore: manifest v2 carries the board/BIOS read at backup time; the load takes them from it" +
                  (fromManifest ? L"" : L" (" + g6.error + L")"));
            std::string sys6 = ReadSmall(r6.backupDir + L"\\system.txt");
            size_t at = sys6.find("A.F0");
            bool edited = false;
            if (at != std::string::npos) {
                sys6.replace(at, 4, "A.H0");   // this selftest's own fixture, rewritten in place (its ACL stays)
                HANDLE f = CreateFileW(sys::ToExtended(r6.backupDir + L"\\system.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                                       TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD put = 0;
                    edited = WriteFile(f, sys6.data(), (DWORD)sys6.size(), &put, nullptr) && put == sys6.size();
                    CloseHandle(f);
                }
            }
            bool refused = edited && !LoadBackupSession(root, r6.backupDir, &e6, trust) &&
                           e6.error.find(L"system.txt doesn't match manifest.txt") == 0 && e6.vars.empty();
            check(refused, L"Restore: system.txt edited to another BIOS is refused (" + e6.error + L")");
        }
        // Admin-only: a session with any file a standard user could have changed is refused.
        {
            auto m7 = freshMock();
            Plan p7;
            ExecResult r7;
            bool made = BuildPlan(d, {{0, 1}}, liveOf(*m7), open, &p7, &why) &&
                        Execute(*m7, p7, root, {}, {}, &r7, XO(L"20260101-000013"));
            std::wstring w7;
            bool selfTrusted = made && (!realAcl || (FixtureTrusted(r7.backupDir, &w7) && FixtureTrusted(r7.backupDir + L"\\manifest.txt", &w7) &&
                                                     FixtureTrusted(r7.backupDir + L"\\journal.txt", &w7)));
            check(selfTrusted, std::wstring(L"trust: a session Execute writes is admin-only (owner Administrators, protected DACL)") +
                  (realAcl ? L"" : L" [not checkable here: the OS doesn't keep ACLs]") + (selfTrusted ? L"" : L" (" + w7 + L")"));
            SnapLoad s8;
            bool opened = made && makeWritable(r7.backupDir + L"\\manifest.txt");
            bool refused = opened && !LoadBackupSession(root, r7.backupDir, &s8, trust) &&
                           s8.error.find(L"This backup folder can be changed without admin rights, so it isn't trusted") == 0;
            check(refused, L"Restore: a session whose manifest.txt a non-admin can change is refused (" + s8.error + L")");
        }
    }
    // --- the write lock (one writer across Hadron windows) ---
    {
        using Run = ExecRun;
        auto onThread = [](Run* run) {
            HANDLE t = CreateThread(nullptr, 0, ExecRunThread, run, 0, nullptr);
            if (!t) return false;
            WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
            return true;
        };
        auto m = freshMock();
        Plan pl;
        bool planned = BuildPlan(d, {{0, 1}}, liveOf(*m), open, &pl, &why);
        TestSd lsd(kLockSddl);   // the test's holder creates the lock the way Hadron does (owner Administrators)
        HANDLE mx = CreateMutexW(lsd.sd ? &lsd.sa : nullptr, FALSE, lockName.c_str());
        bool held = mx && WaitForSingleObject(mx, 0) == WAIT_OBJECT_0;
        Run busy{m.get(), &pl, root, XO(L"20260101-000008"), ExecResult{}, false};
        bool ran = planned && held && onThread(&busy);
        if (held) ReleaseMutex(mx);
        check(ran && !busy.ok && m->writes == 0 && !busy.r.started &&
              busy.r.error == L"The BIOS write lock is held by another process (another Hadron window may be writing BIOS "
                              L"settings). Nothing was written.",
              L"lock: Execute refuses while another holder has the lock (" + busy.r.error + L")");
        Run good{m.get(), &pl, root, XO(L"20260101-000009"), ExecResult{}, false};
        bool ran2 = planned && onThread(&good);
        // Released by RAII when Execute returned: the thread has exited, so a mutex it still held would
        // come back WAIT_ABANDONED here.
        DWORD w = mx ? WaitForSingleObject(mx, 0) : WAIT_FAILED;
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ReleaseMutex(mx);
        check(ran2 && good.ok && m->writes == 1 && w == WAIT_OBJECT_0, L"lock: after the lock is free Execute writes and releases it on return");
        // A holder that ended without releasing (a crash mid-write): refuse, release, and the next run writes.
        auto m2 = freshMock();
        Plan p2;
        bool planned2 = BuildPlan(d, {{0, 1}}, liveOf(*m2), open, &p2, &why);
        HANDLE at = CreateThread(nullptr, 0, AbandonLockThread, (LPVOID)lockName.c_str(), 0, nullptr);
        DWORD took = 0;
        if (at) { WaitForSingleObject(at, INFINITE); GetExitCodeThread(at, &took); CloseHandle(at); }
        Run after{m2.get(), &p2, root, XO(L"20260101-000010"), ExecResult{}, false};
        bool ran3 = planned2 && took == 1 && onThread(&after);
        check(ran3 && !after.ok && m2->writes == 0 && !after.r.started &&
              after.r.error.find(L"The BIOS write lock was left by a process that ended while holding it") == 0 &&
              after.r.error.find(L"Nothing was written.") != std::wstring::npos,
              L"lock: an abandoned lock (holder ended mid-write) refuses the write (" + after.r.error + L")");
        Run next{m2.get(), &p2, root, XO(L"20260101-000011"), ExecResult{}, false};
        bool ran4 = planned2 && onThread(&next);
        check(ran4 && next.ok && m2->writes == 1, L"lock: the refusing run released it; the next write goes ahead" +
              (next.ok ? L"" : L" (" + next.r.error + L")"));
        if (mx) CloseHandle(mx);
        // A lock that already exists with a non-admin owner (pre-created to decide who writes): refused.
        const std::wstring userLock = lockName + L".user";
        std::wstring us = UserOwnerSddl();
        TestSd usd((us + L"D:P(A;;GA;;;WD)").c_str());
        HANDLE ux = usd.sd ? CreateMutexW(&usd.sa, FALSE, userLock.c_str()) : nullptr;
        ExecOptions uo = XO(L"20260101-000015");
        uo.lockName = userLock;
        Run squat{m2.get(), &p2, root, uo, ExecResult{}, false};
        bool ran5 = ux && onThread(&squat);
        check(ran5 && !squat.ok && m2->writes == 1 && !squat.r.started &&
              squat.r.error.find(L"The BIOS write lock was created by a non-admin process") == 0 &&
              squat.r.error.find(L"Nothing was written.") != std::wstring::npos,
              L"lock: a lock that exists with a non-admin owner is refused (" + squat.r.error + L")");
        if (ux) CloseHandle(ux);
    }
    // --- Incomplete sessions ---
    {
        std::vector<std::wstring> v;
        bool inc = JournalIncomplete("BEGIN Setup g1\r\nOK Setup\r\nBEGIN CpuSetup g2\r\n", &v);
        check(inc && v.size() == 1 && v[0] == L"CpuSetup", L"journal: BEGIN without OK/RESTORED is incomplete");
        check(JournalIncomplete("BEGIN Setup g1\r\nRESTORE-FAILED Setup 5\r\nEND\r\n", nullptr),
              L"journal: RESTORE-FAILED counts as incomplete");
        check(!JournalIncomplete("BEGIN Setup g1\r\nOK Setup\r\nEND\r\n", nullptr) && !JournalIncomplete("", nullptr),
              L"journal: complete and empty journals are not");
        IncompleteSession is;
        TestSd psd;   // planted by hand with the same admin-only descriptor Execute uses
        bool none = !FindIncompleteSession(root, &is, 8, trust);   // the newest session so far completed
        std::wstring dir;
        bool made = CreateBackupSession(root, L"29991231-235959", &dir, &err, trust) &&
                    WriteNewFile(dir + L"\\journal.txt", "BEGIN Setup g1\r\n", &err, &psd.sa);
        bool found = made && FindIncompleteSession(root, &is, 8, trust) && is.stamp == L"29991231-235959" && is.vars.size() == 1;
        check(none && found, L"journal: the newest session's unfinished journal is found on start; a completed one isn't");
        // "Newest" is the folder's creation time, not its name: a session created later under a name that
        // sorts first (the clock went back) and completed hides the older unfinished one.
        Sleep(50);
        std::wstring dir2;
        bool made2 = CreateBackupSession(root, L"00000101-000000", &dir2, &err, trust) &&
                     WriteNewFile(dir2 + L"\\journal.txt", "BEGIN Setup g1\r\nOK Setup\r\nEND\r\n", &err, &psd.sa);
        WIN32_FILE_ATTRIBUTE_DATA a1{}, a2{};
        bool times = GetFileAttributesExW(sys::ToExtended(dir).c_str(), GetFileExInfoStandard, &a1) &&
                     GetFileAttributesExW(sys::ToExtended(dir2).c_str(), GetFileExInfoStandard, &a2) &&
                     CompareFileTime(&a2.ftCreationTime, &a1.ftCreationTime) > 0;
        check(made2 && times && !FindIncompleteSession(root, &is, 8, trust),
              L"journal: the newest session is picked by folder creation time, not by name");
        // An unfinished journal a standard user could have written never raises the banner.
        Sleep(50);
        std::wstring dir3;
        bool made3 = CreateBackupSession(root, L"29991231-235960", &dir3, &err, trust) &&
                     WriteNewFile(dir3 + L"\\journal.txt", "BEGIN Setup g1\r\n", &err, &psd.sa);
        bool shown = made3 && FindIncompleteSession(root, &is, 8, trust) && is.stamp == L"29991231-235960";
        bool open3 = made3 && makeWritable(dir3 + L"\\journal.txt");
        bool hidden = open3 && !(FindIncompleteSession(root, &is, 8, trust) && is.stamp == L"29991231-235960");
        check(shown && hidden, L"journal: an unfinished session whose journal a non-admin can change is ignored (no banner)");
        // A root (or its Hadron parent) that isn't admin-only: no banner, and preparing it refuses without
        // touching its security (nothing re-secures an existing folder). A write is refused before writing.
        const std::wstring parent = root.substr(0, root.find_last_of(L'\\'));
        for (const std::wstring& target : {root, parent}) {
            const wchar_t* which = target == root ? L"bios-backups" : L"its Hadron parent";
            bool opened = makeWritable(target);
            bool noneAtAll = opened && !FindIncompleteSession(root, &is, 8, trust);
            std::wstring pe, tw;
            bool refused = opened && !PrepareBackupRoot(root, &pe, trust) && pe.find(L"isn't trusted") != std::wstring::npos;
            bool untouched = !realAcl || !FixtureTrusted(target, &tw);   // still open: nothing re-applied the DACL
            auto mw = freshMock();
            Plan pw;
            ExecResult rw;
            bool noWrite = BuildPlan(d, {{0, 1}}, liveOf(*mw), open, &pw, &why) &&
                           !Execute(*mw, pw, root, {}, {}, &rw, XO(L"20260101-000014")) && mw->writes == 0;
            bool back = makeAdminOnly(target);
            std::wstring pe2;
            bool okAgain = back && PrepareBackupRoot(root, &pe2, trust);
            check(noneAtAll && refused && untouched && noWrite && okAgain,
                  std::wstring(L"trust: when ") + which + L" isn't admin-only: no banner, the root is refused and left as it is, "
                  L"nothing is written (" + pe + L")" + (okAgain ? L"" : L" [after fixing it: " + pe2 + L"]"));
        }
    }
    // --- latch ---
    {
        RealBackend rb({MakeVarKey(L"Setup", L"ec87d643-eba4-4bb5-a1e5-3f3e36b20da9")});
        DWORD e = rb.Write(L"HadronSelftestBogus", L"00000000-0000-0000-0000-000000000000", {1}, 0x7);
        check(sys::NoFirmwareWrites() && e == ERROR_ACCESS_DENIED && rb.apiCalls == 0,
              L"latch: RealBackend::Write returns ERROR_ACCESS_DENIED without calling the API");
    }
    // --- switch and overlay (a second module instance on a test host) ---
    {
        TestHost th;
        BiosModule m;
        check(!m.writeSwitch_, L"switch: off on start");
        m.host_ = &th;
        m.dbOk_ = true;
        std::set<VarKey> allowed;
        m.backend_ = std::make_unique<RealBackend>(allowed);
        m.ToggleWriteSwitch();
        bool refused = !m.writeSwitch_;
        m.backend_ = freshMock();
        m.ToggleWriteSwitch();
        bool on = m.writeSwitch_;
        m.ToggleWriteSwitch();
        check(refused && on && !m.writeSwitch_ && th.prefsCalls == 0,
              L"switch: can't turn on in a test run with the real backend; toggling never touches settings.ini");
        m.planOpen_ = true;
        m.plan_ = Plan{};
        m.plan_.vars.resize(1);
        bool enter = !m.OnInput(WM_KEYDOWN, VK_RETURN, 0) && m.planOpen_ && m.job_ == Job::None;
        m.OnInput(WM_CHAR, L'\r', 0);
        m.OnInput(WM_KEYDOWN, VK_SPACE, 0);
        bool still = m.planOpen_ && m.job_ == Job::None;
        m.OnInput(WM_LBUTTONDBLCLK, 0, MAKELPARAM(10, 10));
        bool dbl = m.planOpen_ && m.job_ == Job::None;
        bool esc = m.OnInput(WM_KEYDOWN, VK_ESCAPE, 0) && !m.planOpen_;
        check(enter && still && dbl && esc, L"plan overlay: Enter, Space and double-click never confirm; Esc closes");

        // Keyboard editing on a live-looking instance (writes possible, mock backend).
        m.db_ = d;
        m.db_.S[0].hasCur = true;
        m.db_.S[0].cur = 0;
        m.primary_ = Src::Live;
        m.liveDone_ = true;
        m.live_.readOk = 1;
        m.live_.elevated = true;
        m.live_.privOk = true;
        m.unverified_ = false;
        m.guardText_.clear();
        m.guardUnknown_.clear();
        auto km = freshMock();
        MockBackend* kmp = km.get();
        m.backend_ = std::move(km);
        if (!m.writeSwitch_) m.ToggleWriteSwitch();
        m.sel_ = 0;
        m.focus_ = Focus::Detail;
        bool possible = m.WritesPossible();
        bool right = m.OnInput(WM_KEYDOWN, VK_RIGHT, 0) && m.hasPick_ && m.pick_ == 1;
        bool clampR = m.OnInput(WM_KEYDOWN, VK_RIGHT, 0) && m.pick_ == 1;
        bool stage = m.OnInput(WM_KEYDOWN, VK_RETURN, 0) && m.staged_.count(0) && m.staged_[0] == 1 && !m.planOpen_ &&
                     m.job_ == Job::None && kmp->writes == 0;
        bool left = m.OnInput(WM_KEYDOWN, VK_LEFT, 0) && m.pick_ == 0;
        check(possible && right && clampR && stage && left,
              L"keyboard: in Detail, Left/Right move between option chips and Enter stages (nothing written, no plan)");
        m.rcNumEdit_ = RECT{0, 0, 10, 10};   // the value field is shown
        m.focus_ = Focus::Detail;
        m.OnInput(WM_KEYDOWN, VK_TAB, 0);
        bool toEdit = m.focus_ == Focus::Edit;
        m.OnInput(WM_KEYDOWN, VK_TAB, 0);
        bool wrap = m.focus_ == Focus::List;
        m.rcNumEdit_ = RECT{};
        m.focus_ = Focus::Detail;
        m.OnInput(WM_KEYDOWN, VK_TAB, 0);
        bool skip = m.focus_ == Focus::List;
        check(toEdit && wrap && skip, L"keyboard: Tab goes Detail -> value field -> List, skipping the field when it isn't shown");

        // Nits: no silent early return from StartWrite; a refused close says why.
        m.planOpen_ = false;
        m.notice_.clear();
        m.StartWrite();
        bool told = m.job_ == Job::None && !m.notice_.empty() && kmp->writes == 0;
        m.job_ = Job::Write;
        bool refusedClose = !m.CanClose() && th.status == L"A BIOS write is running; it stops after the current variable";
        m.job_ = Job::None;
        check(told && refusedClose && m.CanClose(), L"StartWrite without a plan shows a notice; a close during a write is refused with a status");
    }
}

int BiosModule::RunCli(int argc, wchar_t** argv) {
    bool selftest = false;
    for (int i = 0; i < argc; i++) if (std::wstring(argv[i]) == L"--selftest") selftest = true;
    if (!selftest) return -1;
    int fails = 0, checks = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        checks++; if (!ok) fails++;
        wprintf(L"  [%ls] %ls\n", ok ? L"PASS" : L"FAIL", what.c_str());
    };
    wprintf(L"bios selftest\n");
    // The host sets the "no firmware writes" latch for --selftest before any module code runs. Set it
    // here too (idempotent), so nothing in this selftest can ever reach the firmware.
    const bool hostLatched = sys::NoFirmwareWrites();
    sys::SetNoFirmwareWrites();
    check(hostLatched, L"latch: the host set \"no firmware writes\" before the selftest ran");

    // --- settings.db v2 (embedded) ---
    std::wstring err;
    bool ok = LoadDb(&err);
    check(ok, L"embedded settings.db parses" + (ok ? L"" : L": " + err));
    if (!ok) { wprintf(L"bios selftest: FAIL\n"); fflush(stdout); return 1; }
    dbOk_ = true;
    check(db_.format == 2, L"embedded file is v2 (B line / 17-field K lines)");
    check(db_.S.size() == 5368, L"5,368 S lines (got " + std::to_wstring(db_.S.size()) + L")");
    check(db_.keyCount == 56, L"T = 56 tier-1 entries (got " + std::to_wstring(db_.keyCount) + L")");
    check(db_.K.size() == 213 && (int)db_.K.size() == db_.keyCount + 157,
          L"K = T + tier-2 = 56 + 157 = 213 (got " + std::to_wstring(db_.K.size()) + L")");
    check(db_.board == L"MS-7E06" && db_.bios == L"7E06vAF", L"B line: MS-7E06 / 7E06vAF");
    {
        int key = 0, bad = 0, noBasis = 0;
        for (auto& s : db_.S) { if (db_.IsKey(s)) key++; if (s.kb < 0) bad++; }
        for (auto& k : db_.K) if (k.basis.empty()) noBasis++;
        check(key == 501, L"501 settings matched by tier-1 entries (got " + std::to_wstring(key) + L")");
        check(bad == 0, L"every S line has a valid KB index");
        int tier1NoBasis = 0, sourced = 0;
        for (int i = 0; i < (int)db_.K.size(); i++) {
            if (i < db_.keyCount && db_.K[i].basis.empty()) tier1NoBasis++;
            if (!db_.K[i].sources.empty()) sourced++;
        }
        check(tier1NoBasis == 0, L"every tier-1 KB entry states a basis (generated file)");
        wprintf(L"  [info] %d of %zu KB entries without a basis (shown as \"%ls\"); %d entries carry number sources\n",
                noBasis, db_.K.size(), BasisLabel(L"").c_str(), sourced);
        int multi = 0, ctx = 0;
        std::map<std::pair<std::wstring, std::wstring>, int> amb;
        for (auto& s : db_.S) if (!s.ctx.empty()) { ctx++; amb[{s.prompt, s.ctx}]++; }
        for (auto& a : amb) if (a.second > 1) multi += a.second;
        wprintf(L"  [info] AddContext: %d settings labelled; %d still share prompt + label (legacy 4.13)\n", ctx, multi);
    }
    {
        // Legacy title/unescape: K 0 is Hyper-Threading; ACPI paths keep a single backslash.
        check(db_.K[0].title == L"Hyper-Threading", L"K 0 title");
        bool slash = false;
        for (auto& s : db_.S) if (s.help.find(L"\\_SB.") != std::wstring::npos) { slash = true; break; }
        check(slash, L"Unesc: \\\\_SB. becomes \\_SB. in help text");
        check(Unesc(L"a\\nb\\\\c\\td\\") == L"a\nb\\ctd\\", L"Unesc: \\n, \\\\, \\t -> t, trailing lone backslash kept");
    }
    // --- v1 (B line removed, K lines cut to 14 fields) ---
    {
        HMODULE mod = GetModuleHandleW(nullptr);
        HRSRC r = FindResourceW(mod, L"BIOS_SETTINGS", MAKEINTRESOURCEW(10));
        std::string raw((const char*)LockResource(LoadResource(mod, r)), SizeofResource(mod, r));
        std::string v1;
        size_t a = 0;
        while (a < raw.size()) {
            size_t b = raw.find('\n', a);
            if (b == std::string::npos) b = raw.size();
            std::string line = raw.substr(a, b - a);
            if (line.rfind("K\t", 0) == 0) {
                int tabs = 0; size_t cut = line.size();
                for (size_t i = 0; i < line.size(); i++) if (line[i] == '\t' && ++tabs == 14) { cut = i; break; }
                line = line.substr(0, cut);
            }
            if (line.rfind("B\t", 0) != 0) v1 += line + "\n";
            a = b + 1;
        }
        Db d1;
        bool p1 = d1.Parse(v1, &err);
        check(p1 && d1.format == 1 && d1.S.size() == 5368 && d1.K.size() == 213 && d1.board.empty(),
              L"v1 parse: format 1, 5,368 S, 213 K, no board guard");
        bool allEmpty = true;
        for (auto& k : d1.K) if (!k.basis.empty() || !k.sources.empty()) allEmpty = false;
        check(allEmpty && BasisLabel(d1.K[0].basis) == L"Not stated", L"v1: basis absent -> \"Not stated\"");
    }
    // --- synthetic v2 with basis and sources ---
    {
        std::string t = "V\tTest\nB\tMS-0000\t0000v10\nK\t0\tT\tExpl 10% faster\t\t\t\t\t\tnote\ta\tb\tc\tclaim\t"
                        "published\thttps://example.org/a\t10%\x1fpublished\x1fhttps://example.org/a\x1e" "5 ms\x1f"
                        "owner-measured\x1f" "9900K, Overwatch\nT\t1\n"
                        "S\t0\tSetup\tEC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9\t2\t2\tnumeric\tC\t5\t0\t100\t1\tP\\tq\tH\\nx\tA > B\t\t0\n";
        Db d;
        bool p = d.Parse(t, &err);
        check(p && d.format == 2 && d.board == L"MS-0000" && d.K.size() == 1 && d.K[0].basis == L"published" &&
              d.K[0].basisNote == L"https://example.org/a", L"v2 parse: B line, basis, basis_note");
        check(p && d.K[0].sources.size() == 2 && d.K[0].sources[1].number == L"5 ms" &&
              d.K[0].sources[1].kind == L"owner-measured" && d.K[0].sources[1].ref == L"9900K, Overwatch",
              L"v2 parse: field 17 sources (number 0x1F kind 0x1F ref, joined by 0x1E)");
        check(p && d.S[0].guid == L"ec87d643-eba4-4bb5-a1e5-3f3e36b20da9" && d.S[0].prompt == L"Ptq" &&
              d.S[0].help == L"H\nx" && d.S[0].cond, L"S fields: guid lowercased, prompt/help unescaped, flag C");
    }
    // --- value decode ---
    {
        std::vector<uint8_t> v = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xFF};
        long long r = 0;
        check(DecodeValue(v, 1, 1, &r) && r == 0x22, L"decode width 1 @1");
        check(DecodeValue(v, 2, 2, &r) && r == 0x4433, L"decode width 2 @2 (little-endian)");
        check(DecodeValue(v, 3, 4, &r) && r == 0x77665544, L"decode width 4 @3");
        check(DecodeValue(v, 1, 8, &r) && r == (long long)0x9988776655443322ULL && r < 0,
              L"decode width 8 @1 (top bit set -> negative, as legacy)");
        check(!DecodeValue(v, 7, 4, &r) && !DecodeValue(v, 10, 1, &r) && !DecodeValue(v, -1, 1, &r),
              L"decode out of range -> no value");
        Db d;
        d.Parse("V\tt\nK\t0\tk\te\t\t\t\t\t\tn\nT\t1\n"
                "S\t0\tSetup\tg1\t1\t1\toneof\t\t1\t\t\t\tA\t\tM\t0\x1f" "Off\x1e" "1\x1fOn\t0\n"
                "S\t1\tSetup\tg1\t2\t2\tnumeric\t\t0\t0\t9\t1\tB\t\tM\t\t0\n"
                "S\t2\tsetup\tg1\t0\t1\tnumeric\t\t0\t0\t9\t1\tC\t\tM\t\t0\n", &err);
        VarMap vm;
        vm[MakeVarKey(L"Setup", L"{G1}")] = {7, 1, 0x34, 0x12};
        int n = d.Apply(vm, false);
        check(n == 2 && d.S[0].hasCur && CurText(d.S[0]) == L"On" && d.S[1].cur == 0x1234 && Changed(d.S[1]),
              L"Apply: option text, width-2 value, Changed()");
        check(!d.S[2].hasCur && CurText(d.S[2]) == L"-", L"variable names match exactly: \"setup\" != \"Setup\" (rule fix #21)");
    }
    // --- ZeroIsAuto ---
    {
        Setting a; a.kind = L"numeric"; a.var = L"Setup"; a.help = L"Value. 0 = Auto."; a.mn = 1;
        Setting b; b.kind = L"numeric"; b.var = L"Setup"; b.path = L"OC > DRAM Timing"; b.mn = 0;
        Setting c2 = b; c2.mn = 1; c2.zeroAuto = -1;
        Setting d = a; d.opts.push_back({0, L"Zero"}); d.zeroAuto = -1;
        Setting e; e.kind = L"numeric"; e.var = L"Setup"; e.help = L"Use 0- Auto"; e.mn = 5;
        check(ValText(a, 0) == L"Auto" && ValText(a, 3) == L"3", L"ZeroIsAuto: help \"0 = auto\"");
        check(ValText(b, 0) == L"Auto", L"ZeroIsAuto: numeric in DRAM path with min 0");
        check(ValText(c2, 0) == L"0", L"ZeroIsAuto: DRAM path but min 1 -> 0");
        check(ValText(d, 0) == L"Zero", L"ZeroIsAuto: never when the setting has options");
        check(ValText(e, 0) == L"Auto", L"ZeroIsAuto: \"0- auto\" pattern");
    }
    // --- CSV ---
    {
        auto r = ParseCsv(L"﻿\"Name\",\"Guid\"\r\n\"a\"\"b\",\"x,y\"\r\n\"multi\nline\",\r\nplain,last\n\n");
        check(r.size() == 5 && r[0][0] == L"Name", L"CSV: BOM stripped, header row");
        check(r.size() > 1 && r[1].size() == 2 && r[1][0] == L"a\"b" && r[1][1] == L"x,y", L"CSV: \"\" escape and quoted comma");
        check(r.size() > 2 && r[2][0] == L"multi\nline" && r[2][1].empty(), L"CSV: quoted newline, empty last field");
        check(r.size() > 3 && r[3][0] == L"plain" && r[3][1] == L"last", L"CSV: LF line end, unquoted fields");
        auto r2 = ParseCsv(L"a,b\r\nc,d");
        check(r2.size() == 2 && r2[1][1] == L"d", L"CSV: no trailing newline");
        check(CsvQuote(L"a\"b") == L"\"a\"\"b\"", L"CSV: writer doubles quotes");
    }
    // --- zip + inflate ---
    {
        std::vector<uint8_t> out;
        const uint8_t stored[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
        check(Inflate(stored, sizeof(stored), &out, 3, &err) && out == std::vector<uint8_t>({'a', 'b', 'c'}),
              L"inflate: stored block");
        check(Crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, L"CRC-32 check value");
        std::vector<uint8_t> zipBytes(kSampleZip, kSampleZip + sizeof(kSampleZip));
        ZipReader z;
        bool zo = z.Open(zipBytes, &err);
        check(zo && z.Entries().size() == 4, L"zip: central directory, 4 entries");
        int methods[2] = {0, 0};
        for (auto& e : z.Entries()) methods[e.method == 8 ? 1 : 0]++;
        check(methods[0] == 1 && methods[1] == 3, L"zip sample has stored and deflated (fixed + dynamic Huffman) entries");
        SnapLoad sl;
        bool lz = LoadSnapshotZip(zipBytes, L"sample.zip", &sl);
        std::wstring skipText = sl.SkipSummary();
        check(lz && sl.loaded == 2 && sl.rows == 3 && sl.skipped == 1, L"zip snapshot: 3 rows, 2 loaded, 1 skipped (" +
              skipText + L")" + (lz ? L"" : L" " + sl.error));
        auto it = sl.vars.find(MakeVarKey(L"Setup", L"{EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9}"));
        check(it != sl.vars.end() && it->second == std::vector<uint8_t>({1, 0x34, 0x12, 0xFF}), L"zip: stored entry bytes");
        auto it2 = sl.vars.find(MakeVarKey(L"Big \"quoted\", name", L"ec87d643-eba4-4bb5-a1e5-3f3e36b20da9"));
        long sum = 0;
        if (it2 != sl.vars.end()) for (auto bb : it2->second) sum += bb;
        check(it2 != sl.vars.end() && it2->second.size() == 600 && sum == 3595,
              L"zip: deflated entry (dynamic Huffman index.csv, fixed Huffman var) + CSV name with \"\" and comma");
        check(BoardMatches(L"MS-7E06", sl.sysBoard) && BiosMatches(L"7E06vAF", sl.sysBios), L"zip: system.txt read; guard matches");
        std::vector<uint8_t> bad = zipBytes;
        bad[60] ^= 0x55;   // inside the first entry's compressed data
        SnapLoad sb;
        bool refused = !LoadSnapshotZip(bad, L"bad.zip", &sb);
        check(refused, L"zip: corrupt data is refused (" + sb.error + L")");

        // Hostile sizes (reviewer 2.1). A hand-built zip whose one deflated entry claims 0xFFFFFFF0 bytes.
        auto le16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); };
        auto le32 = [&](std::vector<uint8_t>& v, uint32_t x) { le16(v, x & 0xFFFF); le16(v, x >> 16); };
        auto mkzip = [&](uint32_t usize, const std::vector<uint8_t>& data) {
            std::vector<uint8_t> z;
            const std::string nm = "index.csv";
            le32(z, 0x04034b50); le16(z, 20); le16(z, 0); le16(z, 8); le16(z, 0); le16(z, 0);
            le32(z, 0); le32(z, (uint32_t)data.size()); le32(z, usize); le16(z, (uint32_t)nm.size()); le16(z, 0);
            z.insert(z.end(), nm.begin(), nm.end());
            z.insert(z.end(), data.begin(), data.end());
            uint32_t cd = (uint32_t)z.size();
            le32(z, 0x02014b50); le16(z, 20); le16(z, 20); le16(z, 0); le16(z, 8); le16(z, 0); le16(z, 0);
            le32(z, 0); le32(z, (uint32_t)data.size()); le32(z, usize); le16(z, (uint32_t)nm.size());
            le16(z, 0); le16(z, 0); le16(z, 0); le16(z, 0); le32(z, 0); le32(z, 0);
            z.insert(z.end(), nm.begin(), nm.end());
            uint32_t cdSize = (uint32_t)z.size() - cd;
            le32(z, 0x06054b50); le16(z, 0); le16(z, 0); le16(z, 1); le16(z, 1); le32(z, cdSize); le32(z, cd); le16(z, 0);
            return z;
        };
        {
            ZipReader hz;
            bool opened = hz.Open(mkzip(0xFFFFFFF0u, {0x03, 0x00}), &err);
            std::vector<uint8_t> outv;
            std::wstring why;
            bool ex = opened && hz.Extract(hz.Entries()[0], &outv, kMaxSnapText, &why);
            check(opened && !ex && outv.capacity() == 0,
                  L"zip: an entry claiming 0xFFFFFFF0 bytes is refused before allocating (" + why + L")");
            ZipReader hz2;
            std::vector<uint8_t> junk(64, 0x5A);
            bool o2 = hz2.Open(mkzip(0, junk), &err);
            std::wstring why2;
            bool ex2 = o2 && hz2.Extract(hz2.Entries()[0], &outv, kMaxSnapText, &why2);
            check(o2 && !ex2, L"zip: usize 0 with 64 bytes of deflate data is refused (" + why2 + L")");
        }
        {
            // The sample's 600-byte deflated variable, inflated with a 100-byte ceiling, must stop there.
            const ZipEntry* big = nullptr;
            for (auto& e : z.Entries()) if (e.method == 8 && e.usize == 600) big = &e;
            bool stopped = false;
            size_t got = 0;
            std::wstring why;
            if (big) {
                const uint8_t* lh = kSampleZip + big->localOffset;
                size_t body = big->localOffset + 30 + (lh[26] | (lh[27] << 8)) + (lh[28] | (lh[29] << 8));
                std::vector<uint8_t> o;
                stopped = !Inflate(kSampleZip + body, big->csize, &o, 100, &why);
                got = o.size();
            }
            check(big && stopped && got <= 100, L"inflate stops at its output cap (" + std::to_wstring(got) +
                  L" of 600 bytes, " + why + L")");
        }
        {
            // Caps, cache and device names through LoadSnapshotFrom.
            MemSource ms;
            ms.files[L"index.csv"] =
                "\"Name\",\"Guid\",\"Size\",\"Attributes\",\"Error\",\"File\"\r\n"
                "\"A\",\"{g}\",\"1\",\"0x7\",\"0\",\"same.bin\"\r\n"
                "\"B\",\"{g}\",\"1\",\"0x7\",\"0\",\"same.bin\"\r\n"
                "\"C\",\"{g}\",\"1\",\"0x7\",\"0\",\"SAME.bin\"\r\n"
                "\"D\",\"{g}\",\"1\",\"0x7\",\"0\",\"NUL.bin\"\r\n"
                "\"E\",\"{g}\",\"1\",\"0x7\",\"0\",\"com1.txt\"\r\n"
                "\"F\",\"{g}\",\"1\",\"0x7\",\"0\",\"Lpt9\"\r\n"
                "\"G\",\"{g}\",\"1\",\"0x7\",\"0\",\"console.bin\"\r\n"
                "\"H\",\"{g}\",\"1\",\"0x7\",\"0\",\"huge.bin\"\r\n";
            ms.files[L"vars/same.bin"] = "x";
            ms.files[L"vars/console.bin"] = "y";
            ms.files[L"vars/huge.bin"] = std::string(kMaxSnapVar + 1, 'z');
            SnapLoad sl2;
            bool l2 = LoadSnapshotFrom(ms, L"hostile", &sl2);
            check(l2 && sl2.loaded == 4 && sl2.skipReasons[L"with an unsafe file name"] == 3,
                  L"snapshot: NUL.bin, com1.txt, Lpt9 refused as device names; console.bin allowed");
            check(sl2.skipReasons[L"whose vars file can't be read"] == 1, L"snapshot: a variable over 4 MB is refused");
            // index.csv + same.bin + console.bin + huge.bin + system.txt: one read per distinct file name.
            check(ms.reads == 5, L"snapshot: one read per file name, not per row (" + std::to_wstring(ms.reads) + L" reads)");
        }
        {
            // Many rows pointing at one maximum-size file: the total cap counts every copy, not each read.
            MemSource ms;
            std::string idx = "\"Name\",\"Guid\",\"Size\",\"Attributes\",\"Error\",\"File\"\r\n";
            for (int i = 0; i < 70; i++) idx += "\"V" + std::to_string(i) + "\",\"{g}\",\"1\",\"0x7\",\"0\",\"big.bin\"\r\n";
            ms.files[L"index.csv"] = idx;
            ms.files[L"vars/big.bin"] = std::string(kMaxSnapVar, 'q');
            SnapLoad sl3;
            bool l3 = LoadSnapshotFrom(ms, L"repeat", &sl3);
            check(!l3 && sl3.vars.empty() && ms.reads <= 2,
                  L"snapshot: 70 rows x 4 MB from one file hit the 256 MB total cap (" + sl3.error + L")");
        }
    }
    // --- SMBIOS ---
    {
        std::vector<uint8_t> t = {0, 3, 0, 0, 0, 0, 0, 0};
        std::vector<uint8_t> s0 = {0, 24, 0, 0, 1, 2, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const char str0[] = "American Megatrends International, LLC.\0A.F0\0" "01/01/2024\0";
        std::vector<uint8_t> s1 = {1, 27, 1, 0, 1, 2, 0, 0};
        s1.resize(27, 0);
        static const char str1[] = "Micro-Star\0MS-7E06\0";
        std::vector<uint8_t> s2 = {2, 15, 2, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        static const char str2[] = "Micro-Star International Co., Ltd.\0PRO Z790-P WIFI (MS-7E06)\0";
        std::vector<uint8_t> s127 = {127, 4, 3, 0, 0, 0};
        std::vector<uint8_t> tab;
        auto add = [&](const std::vector<uint8_t>& h, const char* strs, size_t len) {
            tab.insert(tab.end(), h.begin(), h.end());
            tab.insert(tab.end(), strs, strs + len);
            tab.push_back(0);
        };
        add(s0, str0, sizeof(str0) - 1);
        add(s1, str1, sizeof(str1) - 1);
        add(s2, str2, sizeof(str2) - 1);
        tab.insert(tab.end(), s127.begin(), s127.end());
        uint32_t len = (uint32_t)tab.size();
        t[4] = len & 0xFF; t[5] = (len >> 8) & 0xFF;
        t.insert(t.end(), tab.begin(), tab.end());
        SmbiosInfo m;
        bool pm = ParseSmbios(t.data(), t.size(), &m);
        check(pm && m.biosVersion == L"A.F0" && m.biosDate == L"01/01/2024" && m.biosVendor.find(L"American") == 0,
              L"SMBIOS type 0: vendor, version, date (got \"" + m.biosVersion + L"\")");
        check(pm && m.boardProduct == L"PRO Z790-P WIFI (MS-7E06)" && m.boardMaker.find(L"Micro-Star") == 0,
              L"SMBIOS type 2: board product (type 1 skipped)");
        check(BoardMatches(L"MS-7E06", m.boardProduct) && BiosMatches(L"7E06vAF", m.biosVersion) &&
              BiosMatches(L"7E06vAF", L"7E06vAF") && !BiosMatches(L"7E06vAF", L"A.H0") &&
              !BoardMatches(L"MS-7E06", L"MS-7D25"), L"version guard: MSI 7E06vAF <-> A.F0; mismatches detected");
        SmbiosInfo bad;
        check(!ParseSmbios(t.data(), 6, &bad), L"SMBIOS: truncated table refused");
        // A Length that runs past the buffer is clamped to it.
        std::vector<uint8_t> t2 = t;
        t2[4] = 0xFF; t2[5] = 0xFF; t2[6] = 0xFF;
        SmbiosInfo m2;
        check(ParseSmbios(t2.data(), t2.size(), &m2) && m2.boardProduct == L"PRO Z790-P WIFI (MS-7E06)",
              L"SMBIOS: header Length past the buffer is clamped");
        // A string set with no terminator: type 0 then strings that run to the end of the buffer.
        std::vector<uint8_t> t3 = {0, 3, 0, 0, 0, 0, 0, 0};
        t3.insert(t3.end(), s0.begin(), s0.end());
        const char unterminated[] = {'V', 'e', 'n', 'd', 'o', 'r', 0, 'A', '.', 'F', '0'};
        t3.insert(t3.end(), unterminated, unterminated + sizeof(unterminated));
        uint32_t l3 = (uint32_t)(t3.size() - 8);
        t3[4] = l3 & 0xFF; t3[5] = (l3 >> 8) & 0xFF;
        SmbiosInfo m3;
        bool p3 = ParseSmbios(t3.data(), t3.size(), &m3);
        check(p3 && m3.biosVendor == L"Vendor" && m3.biosVersion == L"A.F0" && m3.boardProduct.empty(),
              L"SMBIOS: an unterminated string set stops at the buffer end");
        check(BiosMatches(L"7E06vAF", L"a.f0") && !BiosMatches(L"7E06vAF", L"A.F1") &&
              !BiosMatches(L"7E06vAF", L"7E06vAF1"), L"version guard: \"a.f0\" matches; \"A.F1\" and \"7E06vAF1\" don't");
        check(!BoardMatches(L"MS-7E06", L"MS-7E06x") && !BoardMatches(L"MS-7E06", L"PRO (XMS-7E06)") &&
              BoardMatches(L"MS-7E06", L"MS-7E06") && BoardMatches(L"MS-7E06", L"ms-7e06 rev 1.0"),
              L"version guard: board id must be a delimited token (\"MS-7E06x\" doesn't match)");
    }
    // --- naming ---
    {
        check(SanitiseName(L"Boot0001") == L"Boot0001" && SanitiseName(L"a b/c:d") == L"a_b_c_d", L"sanitise: legacy rule");
        check(SanitiseName(std::wstring(100, L'x')).size() == 80, L"sanitise: cut to 80 characters");
        std::wstring g = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
        std::wstring a = SnapFileName(1, L"Lang A", g), b = SnapFileName(2, L"Lang?A", g);
        check(a == L"0001_Lang_A__8be4df61-93ca-11d2-aa0d-00e098032b8c.bin", L"snapshot name: index prefix, guid lowercased: " + a);
        check(a != b && SanitiseName(L"Lang A") == SanitiseName(L"Lang?A"),
              L"names that sanitise alike get different files (legacy collision fixed)");
        check(SnapFileName(12345, L"x", L"g") == L"12345_x__g.bin", L"snapshot name: more than 4 digits");
    }
    // --- fixture values (port of make_bios_snapshot.py, in memory; --fixture <dir> also loads the real one) ---
    auto checkFixture = [&](const std::wstring& label) {
        int good = 0;
        for (auto& e : kExpected) {
            if (e.id < 0 || e.id >= (int)db_.S.size()) continue;
            const Setting& s = db_.S[e.id];
            if (s.prompt == e.prompt && s.hasCur && s.cur == e.value && s.hasDef && s.def == e.def && Changed(s)) good++;
        }
        check(good == 9, label + L": the 9 changed settings of bios-snapshot-expected.json (got " + std::to_wstring(good) + L")");
        std::set<std::tuple<std::wstring, int>> changedBytes;
        for (auto& e : kExpected) {
            const Setting& s = db_.S[e.id];
            for (int b = 0; b < s.width; b++) changedBytes.insert({s.var, s.offset + b});
        }
        int others = 0, missing = 0;
        for (auto& s : db_.S) {
            if (!s.hasCur) { missing++; continue; }
            bool overlaps = false;
            for (int b = 0; b < s.width; b++) if (changedBytes.count({s.var, s.offset + b})) overlaps = true;
            if (!overlaps && Changed(s)) others++;
        }
        check(missing == 0, label + L": every setting has a value");
        wprintf(L"  [info] %ls: %d settings outside the 9 changed byte ranges differ from default "
                L"(settings sharing bytes with a different default)\n", label.c_str(), others);
    };
    {
        // make_bios_snapshot.py: buffers sized to the highest offset+width, defaults written in id order,
        // then the 9 changes by prompt + option text.
        std::map<VarKey, std::vector<uint8_t>> bufs;
        for (auto& s : db_.S) {
            auto& b = bufs[VarKey{s.var, s.guid}];
            if ((int)b.size() < s.offset + s.width) b.resize(s.offset + s.width, 0);
        }
        auto put = [&](const Setting& s, long long v) {
            auto& b = bufs[VarKey{s.var, s.guid}];
            for (int i = 0; i < s.width; i++) b[s.offset + i] = (uint8_t)((unsigned long long)v >> (8 * i));
        };
        for (auto& s : db_.S) if (s.hasDef) put(s, s.def);
        const std::pair<const wchar_t*, const wchar_t*> changes[] = {
            {L"Hyper-Threading", L"Disabled"}, {L"Intel Turbo Boost", L"Disabled"},
            {L"Intel Speed Shift Technology", L"Disabled"}, {L"EIST", L"Disabled"}, {L"CSM Support", L"Enabled"},
            {L"Re-Size BAR Support", L"Disabled"}, {L"Above 4G memory/Crypto Currency mining", L"Disabled"},
            {L"Hardware Prefetcher", L"Disabled"}, {L"Adjacent Cache Line Prefetch", L"Disabled"}};
        for (auto& ch : changes)
            for (auto& s : db_.S)
                if (s.prompt == ch.first)
                    for (auto& o : s.opts) if (o.t == ch.second) { put(s, o.v); break; }
        MemSource ms;
        std::wstring csv = L"\"Name\",\"Guid\",\"Size\",\"Attributes\",\"Error\",\"File\"\r\n";
        int idx = 0;
        for (auto& b : bufs) {
            std::wstring file = SnapFileName(++idx, b.first.first, b.first.second);
            csv += CsvQuote(b.first.first) + L"," + CsvQuote(L"{" + b.first.second + L"}") + L"," +
                   CsvQuote(std::to_wstring(b.second.size())) + L",\"0x7\",\"0\"," + CsvQuote(file) + L"\r\n";
            ms.files[L"vars/" + file] = std::string(b.second.begin(), b.second.end());
        }
        ms.files[L"index.csv"] = "\xEF\xBB\xBF" + ToUtf8(csv);
        SnapLoad sl;
        bool lm = LoadSnapshotFrom(ms, L"memory", &sl);
        check(lm && sl.loaded == 20 && sl.skipped == 0, L"in-memory fixture: 20 variables (" + std::to_wstring(sl.loaded) + L")");
        db_.Apply(sl.vars, false);
        primary_ = Src::Snapshot;
        checkFixture(L"in-memory fixture");
        // Export (legacy §4.11) on these values.
        std::wstring ex = ExportText(db_, 4, L"Snapshot: memory", L"");
        check(ex.rfind(L"BIOS changes for goal: Low latency\r\n", 0) == 0 &&
              ex.find(L"\r\n=== Settings to change ===\r\n") != std::wstring::npos &&
              ex.find(L"\r\n=== Advice (check these by hand) ===\r\n") != std::wstring::npos &&
              ex.find(L"  ->  ") != std::wstring::npos, L"export text: legacy sections and separators");
        check(ex.find(L"\r\n    basis: ") != std::wstring::npos, L"export text: basis lines (rule 7)");
        {
            // A source line appears only when its number is in the printed text.
            Db d;
            d.Parse("V\tt\nB\tMS-0\t0000v10\nK\t0\tTitle\tE\t\t\t\t\t~Lower it by 5 W and test\tNote\ta\tb\tc\tcl\t"
                    "published\thttps://example.org/x\t5 W\x1fpublished\x1fhttps://example.org/x\x1e"
                    "9 ms\x1fpublished\x1fhttps://example.org/y\nT\t1\n"
                    "S\t0\tSetup\tg\t0\t1\toneof\t\t0\t\t\t\tP\t\tM\t0\x1fOff\x1e" "1\x1fOn\t0\n", &err);
            std::wstring e2 = ExportText(d, 4, L"src", L"");
            check(e2.find(L"    basis: Published: https://example.org/x\r\n") != std::wstring::npos &&
                  e2.find(L"    source: 5 W \u2014 published \u2014 https://example.org/x\r\n") != std::wstring::npos &&
                  e2.find(L"9 ms") == std::wstring::npos,
                  L"export text: basis + the source of each number printed, and only those");
        }
        // Goal rank on a fixture setting: Hyper-Threading (exact recs).
        const Setting& ht = db_.S[417];
        long long v;
        check(RecVal(db_, ht, 1, &v) && GoalRank(db_, ht, 1) == (v != ht.cur ? 0 : 2), L"goal rank for an exact rec");
    }
    CommandLine cl = CommandLine::Parse(GetCommandLineW());
    std::wstring fx = cl.fixture;
    for (size_t i = 0; i + 1 < cl.rest.size(); i++) if (cl.rest[i] == L"--snapshot") fx = sys::ToExtended(cl.rest[i + 1]);
    // --- guarded writes (docs/design/bios-write.md), all through MockBackend ---
    // Backups always go to a new folder under %TEMP% (never next to the fixture, never %ProgramData%),
    // laid out like the real root (<run>\Hadron\bios-backups), which is removed at the end; only the
    // folder this run created.
    {
        wchar_t t[MAX_PATH + 1] = {};
        DWORD n = GetTempPathW(MAX_PATH + 1, t);
        if (n == 0 || n > MAX_PATH) {
            DWORD e = n == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
            check(false, L"write selftest: GetTempPath failed (" + sys::ErrorText(e) + L"); the write selftests didn't run");
        } else {
            std::wstring base(t, n);
            while (!base.empty() && base.back() == L'\\') base.pop_back();
            wchar_t tag[64];
            swprintf(tag, 64, L"\\bios-write-selftest-%lu-%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
            std::wstring root = sys::ToExtended(base + tag);
            bool fresh = CreateDirectoryW(root.c_str(), nullptr) != 0;
            DWORD ce = fresh ? 0 : GetLastError();
            check(fresh, L"write selftest: new folder " + Shell(root) + (fresh ? L"" : L": " + sys::ErrorText(ce)));
            if (fresh) {
                SelftestWrites(check, root + L"\\Hadron\\bios-backups");
                std::wstring re;
                bool removed = RemoveSelftestTree(root, &re);
                check(removed, L"write selftest: removed its own folder " + Shell(root) + (removed ? L"" : L": " + re));
            }
        }
    }
    if (!fx.empty()) {
        SnapLoad sl;
        bool lf = LoadSnapshot(fx, &sl);
        check(lf, L"fixture " + Shell(fx) + L" loads" + (lf ? L"" : L": " + sl.error));
        if (lf) {
            db_.ClearValues(false);
            db_.Apply(sl.vars, false);
            check(sl.loaded == 20, L"fixture: 20 variables (bios-snapshot-expected.json \"variables\")");
            checkFixture(L"fixture " + Shell(fx));
            // --print-export <1-5>: print the export for that goal on the fixture values (review sample;
            // the GUI path goes through the Save dialog, which Wine can't drive headless).
            for (size_t i = 0; i + 1 < cl.rest.size(); i++) {
                if (cl.rest[i] != L"--print-export") continue;
                int g = _wtoi(cl.rest[i + 1].c_str()) - 1;
                if (g < 0 || g >= kGoalCount) continue;
                std::string u = ToUtf8(ExportText(db_, g, L"Snapshot: " + sl.label, SnapGuard(db_, sl, L"snapshot")));
                fwrite(u.data(), 1, u.size(), stdout);
            }
        }
    } else {
        wprintf(L"  [info] no --fixture/--snapshot given: on-disk fixture check skipped (in-memory port ran above)\n");
    }
    wprintf(L"bios selftest: %ls (%d checks, %d failed)\n", fails ? L"FAIL" : L"PASS", checks, fails);
    fflush(stdout);
    return fails ? 1 : 0;
}

}  // namespace

std::unique_ptr<Module> MakeBiosModule() { return std::make_unique<BiosModule>(); }

}  // namespace hx
