// modules/bios/module.cpp — BIOS Settings Explorer (docs/design/bios.md, legacy notes
// docs/legacy/bios-explorer.md). Every setup setting of the board with its live value, default, menu
// path, help, explanation and per-goal advice; snapshots (save / load / compare); export of the changes a
// goal recommends.
//
// Hard rule 2: read-only firmware. This module reads UEFI variables and SMBIOS and nothing else; it has
// no firmware write path, never flashes and never runs vendor setup tools.
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
#include "modules/bios/snapshot.h"
#include "modules/bios/zip.h"

#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <algorithm>
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

constexpr UINT WM_BIOS_DONE = WM_MODULE_BASE + 1;   // worker -> view
const wchar_t* const kViewClass = L"Hadron.bios.View";
const wchar_t* const kAppTitle = L"BIOS Settings Explorer";

enum ClickId {
    ID_NONE = 0, ID_EXPORT = 1, ID_LOAD = 2, ID_SAVE = 3, ID_BANNER_LOAD = 4, ID_SEARCH = 5, ID_BANNER_TOGGLE = 6,
    ID_GOAL = 100,     // + goal + 1 (100 = None)
    ID_FILTER = 200,   // + filter
};
enum Filter { F_ALL, F_KEY, F_CHANGED, F_HIDDEN, F_DIFFERS, F_SNAPDIFF };
enum class Focus { List, Search, Detail };
enum class Src { None, Live, Snapshot };
enum class Job { None, Live, Load, Save };
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
                                     L"per-goal advice. Snapshots and compare. Read-only.",
                                     Admin::Required};
        return info;
    }
    HWND Create(IHost& host, HWND parent, const RECT& rc) override;
    void OnActivate() override { if (hwnd_) SetFocus(hwnd_); }
    void OnDpiChanged(UINT) override { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    bool PreTranslateMessage(MSG& m) override;
    bool OnInput(UINT msg, WPARAM w, LPARAM l) override;
    void Shutdown() override { worker_.Cancel(); worker_.Join(); }
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

    // painting
    void Chip(HDC dc, int& x, int y, const std::wstring& label, int id, bool on);
    int  Button(HDC dc, int right, int y, const std::wstring& label, int id, bool primary, bool enabled);
    void PaintHeader(HDC dc, const RECT& rc);
    int  PaintBanners(HDC dc, const RECT& rc);   // returns the y below the last banner
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
            SetCursor(LoadCursorW(nullptr, hoverId_ == ID_SEARCH ? IDC_IBEAM : (hoverId_ > 0 ? IDC_HAND : IDC_ARROW)));
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
        Invalidate();
        return 0;
    case WM_BIOS_DONE:
        OnJobDone();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(h);
        [[fallthrough]];
    case WM_LBUTTONUP: case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_CHAR:
        if (m == WM_MOUSEMOVE && !tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        if (OnInput(m == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : m, w, l)) Invalidate();
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
    if (sortCol_ == C_SNAP && !compare_) sortCol_ = -1;
    for (int i = 0; i < (int)db_.S.size(); i++) {
        const Setting& s = db_.S[i];
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
    else if (id >= ID_GOAL && id < ID_GOAL + 1 + kGoalCount) {
        goal_ = id - ID_GOAL - 1;
        if (goal_ < 0) {
            if (filter_ == F_DIFFERS) filter_ = F_ALL;
            if (sortCol_ == C_GOAL) sortCol_ = -1;
        }
        Refilter();
    } else if (id >= ID_FILTER && id <= ID_FILTER + F_SNAPDIFF) {
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
// Input

bool BiosModule::OnInput(UINT msg, WPARAM w, LPARAM l) {
    if (!host_) return false;
    PaintCtx c = Ctx();
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        int pitch = Sc(Console() ? 20 : 26);
        int page = std::max(1, (int)(rcListBody_.bottom - rcListBody_.top) / pitch - 1);
        if (w == VK_TAB) {
            bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int f = ((int)focus_ + (back ? 2 : 1)) % 3;
            focus_ = (Focus)f;
            return true;
        }
        if (ctrl && w == 'F') { focus_ = Focus::Search; return true; }
        if (w == VK_ESCAPE) {
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
        if (focus_ == Focus::Search && (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END || w == VK_DELETE))
            return search_.OnKey(w);
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
        if (focus_ != Focus::Search) {
            if (ch < 0x20) return false;
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

void BiosModule::Paint(HDC dc, const RECT& rc) {
    if (!host_) return;
    Pal p(host_->Theme());
    const Fonts& f = host_->Fonts();
    PaintCtx c = Ctx();
    clicks_.clear();
    FillSolid(dc, rc, p.bg);
    const bool con = Console();
    int pad = Sc(con ? 10 : 20);

    PaintHeader(dc, rc);
    bannerTop_ = Sc(con ? 38 : 74);
    int y = PaintBanners(dc, rc);

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
                                              L"Differs from goal", L"Snapshot differs"};
    for (int i = 0; i <= F_SNAPDIFF; i++) {
        if (i == F_DIFFERS && goal_ < 0) continue;
        if (i == F_SNAPDIFF && !compare_) continue;
        Chip(dc, x, y, kFilters[i], ID_FILTER + i, filter_ == i);
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
        cell(C_CUR, cur, Changed(s) ? p.warn : p.text);
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
        y += bh + Sc(8);
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
// Headless

void BiosModule::LoadFixture(const wchar_t* dir) {
    // Synchronous, no Worker, so --screenshot is deterministic. dir is a snapshot folder or zip.
    if (!dir || !*dir || !dbOk_) return;
    SnapLoad s;
    if (LoadSnapshot(dir, &s)) ApplySnapshot(std::move(s));
    else SetNotice(L"Could not load the fixture snapshot: " + s.error, 2);
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
