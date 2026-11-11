// OrganizerRescue - puts back files Organizer moved, using the NTFS change journal (no undo log needed).
// Win32 + GDI only. Build:
// x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows rescue.cpp res.o -o OrganizerRescue.exe -static -s
//   -lgdi32 -lmsimg32 -lshell32 -ldwmapi

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cwchar>
#include "rescue_core.h"

using rc::u64; using rc::u32;

#define WM_APP_SCANNED (WM_APP + 1)
#define WM_APP_RESTORED (WM_APP + 2)

// ---------------------------------------------------------------- theme
static const COLORREF BG = RGB(17, 19, 23), PANEL = RGB(24, 27, 33), PANEL2 = RGB(32, 36, 43), PANEL3 = RGB(42, 47, 56);
static const COLORREF BORDER = RGB(40, 44, 52), TEXT1 = RGB(232, 235, 240), TEXT2 = RGB(156, 163, 176), TEXT3 = RGB(108, 115, 128);
static const COLORREF ACCENT = RGB(96, 160, 255), GOOD = RGB(80, 214, 140), WARN = RGB(240, 180, 70), BAD = RGB(236, 98, 98);

enum { S_READY, S_BIN, S_BACK, S_TAKEN, S_MISSING, NSTAT };
static const wchar_t* kStatName[NSTAT] = { L"Ready", L"In Recycle Bin", L"Already back", L"Spot taken", L"Gone" };
static const COLORREF kStatColor[NSTAT] = { RGB(96, 160, 255), RGB(240, 180, 70), RGB(80, 214, 140), RGB(236, 98, 98), RGB(120, 128, 142) };

struct Item { rc::Move m; int status = S_READY; std::wstring cur; bool restored = false; };
static std::vector<Item> g_items;
struct Session { u64 start = 0, end = 0; int count = 0; bool on = true; };
static std::vector<Session> g_sessions;

enum { ST_IDLE, ST_SCANNING, ST_READY, ST_RESTORING, ST_DONE, ST_ERROR };
static volatile LONG g_state = ST_IDLE, g_cancel = 0, g_pRecs = 0, g_pDone = 0, g_pTotal = 0;
static wchar_t g_drive = 0;
static std::wstring g_err, g_doneMsg;
static u64 g_journalFrom = 0;
static int g_okCount = 0, g_failCount = 0, g_skipCount = 0;
static HANDLE g_thread = NULL;
static std::vector<wchar_t> g_drives;

static HWND g_hwnd;
static int g_dpi = 96;
static inline int S(int v) { return MulDiv(v, g_dpi, 96); }
static HFONT g_fUI, g_fSemi, g_fSmall, g_fSmallSemi, g_fTitle, g_fBig;
static RECT rcHead, rcCard, rcSess, rcList, rcLBody, rcFoot;
static int g_lscroll = 0;
static bool g_tracking = false;
struct Click { RECT rc; int id; u32 data; };
static std::vector<Click> g_clicks;
static int g_hoverId = -1; static u32 g_hoverData = 0;
enum { ID_DRIVE = 100, ID_SCAN = 1, ID_RESTORE, ID_CANCEL, ID_OPEN, ID_SESSION = 1000 };

// ---------------------------------------------------------------- helpers
static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t), (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t), (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
static void FillC(HDC dc, const RECT& r, COLORREF c) { SetDCBrushColor(dc, c); FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH)); }
static void RoundC(HDC dc, const RECT& r, int rad, COLORREF c) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, c); SetDCPenColor(dc, c);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
}
static void RoundOutline(HDC dc, const RECT& r, int rad, COLORREF c) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, c);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
}
static void Text(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f); SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}
static int TextW(HDC dc, const wchar_t* s, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f); SIZE sz; GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz); SelectObject(dc, of); return sz.cx;
}
static const UINT TL = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS;
static const UINT TLP = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_PATH_ELLIPSIS;
static const UINT TC = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
static void AddClick(const RECT& r, int id, u32 data = 0) { g_clicks.push_back({ r, id, data }); }
static bool IsHover(int id, u32 data = 0) { return g_hoverId == id && g_hoverData == data; }
static std::wstring NumStr(u64 v) { std::wstring s = std::to_wstring(v); for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L","); return s; }
static std::wstring Root() { return std::wstring(1, g_drive) + L":\\"; }
static std::wstring Abs(const std::wstring& rel) { return rel.empty() ? Root() : Root() + rel; }
static std::wstring JoinRel(const std::wstring& a, const std::wstring& b) { return a.empty() ? b : a + L"\\" + b; }
static std::wstring Long(const std::wstring& p) { return p.compare(0, 4, L"\\\\?\\") == 0 ? p : L"\\\\?\\" + p; }
static bool Exists(const std::wstring& p) { return GetFileAttributesW(Long(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring TimeStr(u64 ft, bool withDate = true) {
    FILETIME f, lf; f.dwLowDateTime = (DWORD)ft; f.dwHighDateTime = (DWORD)(ft >> 32);
    SYSTEMTIME st; FileTimeToLocalFileTime(&f, &lf); FileTimeToSystemTime(&lf, &st);
    static const wchar_t* mon[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    int h = st.wHour % 12; if (!h) h = 12;
    wchar_t b[64];
    if (withDate) _snwprintf(b, 64, L"%ls %u, %u  %d:%02u %ls", mon[(st.wMonth + 11) % 12], st.wDay, st.wYear, h, st.wMinute, st.wHour < 12 ? L"AM" : L"PM");
    else _snwprintf(b, 64, L"%d:%02u %ls", h, st.wMinute, st.wHour < 12 ? L"AM" : L"PM");
    b[63] = 0;
    return b;
}
static void MakeDirs(const std::wstring& rel) {
    if (rel.empty()) return;
    size_t p = 0;
    for (;;) {
        size_t e = rel.find(L'\\', p);
        std::wstring part = e == std::wstring::npos ? rel : rel.substr(0, e);
        if (!Exists(Abs(part))) CreateDirectoryW(Long(Abs(part)).c_str(), NULL);
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
}
static void WriteUtf8(HANDLE f, const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], n, NULL, NULL);
    DWORD w; WriteFile(f, u.data(), (DWORD)u.size(), &w, NULL);
}

// ---------------------------------------------------------------- journal
static HANDLE g_rootH = INVALID_HANDLE_VALUE;
static bool CurrentRel(u64 frn, std::wstring& out) {
    FILE_ID_DESCRIPTOR d = {};
    d.dwSize = sizeof(d); d.Type = FileIdType; d.FileId.QuadPart = (LONGLONG)frn;
    HANDLE h = OpenFileById(g_rootH, &d, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, FILE_FLAG_BACKUP_SEMANTICS);
    if (h == INVALID_HANDLE_VALUE) return false;
    wchar_t buf[33000];
    DWORD n = GetFinalPathNameByHandleW(h, buf, 33000, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= 33000) return false;
    std::wstring p = buf;
    if (p.compare(0, 4, L"\\\\?\\") == 0) p = p.substr(4);
    if (p.size() < 2 || towupper(p[0]) != towupper(g_drive) || p[1] != L':') return false;
    p = p.size() > 3 ? p.substr(3) : L"";
    out = p;
    return true;
}

static bool ReadJournal(std::vector<rc::Rec>& out, std::wstring& err) {
    wchar_t vp[] = L"\\\\.\\X:"; vp[4] = g_drive;
    HANDLE vol = CreateFileW(vp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (vol == INVALID_HANDLE_VALUE) {
        err = GetLastError() == ERROR_ACCESS_DENIED ? L"Windows refused access to the drive. Run this as administrator." : L"Couldn't open the drive.";
        return false;
    }
    BYTE jd[256] = {};
    DWORD br = 0;
    if (!DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, NULL, 0, jd, sizeof(jd), &br, NULL)) {
        DWORD e = GetLastError();
        err = (e == ERROR_JOURNAL_NOT_ACTIVE || e == ERROR_JOURNAL_DELETE_IN_PROGRESS)
                  ? L"This drive has no change journal turned on, so Windows has no record of where the files came from."
                  : L"This drive's file system doesn't keep a change journal (it needs to be NTFS).";
        CloseHandle(vol);
        return false;
    }
    u64 journalId = *(u64*)(jd + 0);
    LONGLONG firstUsn = *(LONGLONG*)(jd + 8), nextUsn = *(LONGLONG*)(jd + 16);
#pragma pack(push, 1)
    struct ReadV0 { LONGLONG StartUsn; DWORD ReasonMask; DWORD ReturnOnlyOnClose; ULONGLONG Timeout; ULONGLONG BytesToWaitFor; ULONGLONG UsnJournalID; };
#pragma pack(pop)
    ReadV0 rd = { firstUsn, 0xFFFFFFFF, 0, 0, 0, journalId };
    std::vector<BYTE> buf(1 << 20);
    bool first = true;
    while (!g_cancel) {
        if (!DeviceIoControl(vol, FSCTL_READ_USN_JOURNAL, &rd, sizeof(rd), buf.data(), (DWORD)buf.size(), &br, NULL)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            err = L"Reading the change journal failed (error " + std::to_wstring(GetLastError()) + L").";
            CloseHandle(vol);
            return false;
        }
        if (br <= 8) break;
        BYTE* p = buf.data() + 8;
        BYTE* end = buf.data() + br;
        while (p + 8 <= end) {
            DWORD len = *(DWORD*)p;
            if (len < 8 || p + len > end) break;
            WORD major = *(WORD*)(p + 4);
            rc::Rec r;
            bool ok = true;
            WORD fnl, fno;
            if (major == 2) {
                r.frn = *(u64*)(p + 8); r.parent = *(u64*)(p + 16); r.usn = *(u64*)(p + 24); r.time = *(u64*)(p + 32);
                r.reason = *(DWORD*)(p + 40); r.attr = *(DWORD*)(p + 52); fnl = *(WORD*)(p + 56); fno = *(WORD*)(p + 58);
            } else if (major == 3) {
                r.frn = *(u64*)(p + 8); r.parent = *(u64*)(p + 24); r.usn = *(u64*)(p + 40); r.time = *(u64*)(p + 48);
                r.reason = *(DWORD*)(p + 56); r.attr = *(DWORD*)(p + 68); fnl = *(WORD*)(p + 72); fno = *(WORD*)(p + 74);
            } else ok = false;
            if (ok) {
                if (first) { g_journalFrom = r.time; first = false; }
                if ((r.attr & rc::A_DIRECTORY) || (r.reason & (rc::R_RENAME_OLD | rc::R_RENAME_NEW))) {
                    if ((DWORD)fno + fnl <= len) r.name.assign((const wchar_t*)(p + fno), fnl / 2);
                    out.push_back(std::move(r));
                }
                InterlockedIncrement(&g_pRecs);
            }
            p += len;
        }
        rd.StartUsn = *(LONGLONG*)buf.data();
        if (rd.StartUsn >= nextUsn) break;
    }
    CloseHandle(vol);
    return true;
}

static DWORD WINAPI ScanThread(LPVOID) {
    g_items.clear(); g_sessions.clear(); g_err.clear(); g_journalFrom = 0;
    std::vector<rc::Rec> recs;
    g_rootH = CreateFileW(Root().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION fi = {};
    if (g_rootH == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(g_rootH, &fi)) {
        g_err = L"Couldn't open the drive's root folder.";
    } else if (ReadJournal(recs, g_err) && !g_cancel) {
        rc::Analyzer a;
        a.rootFrn = ((u64)fi.nFileIndexHigh << 32) | fi.nFileIndexLow;
        a.currentPath = CurrentRel;
        auto moves = a.Run(recs, 15ull * 60 * 10000000);
        recs.clear(); recs.shrink_to_fit();
        g_pTotal = (LONG)moves.size(); g_pDone = 0;
        for (auto& m : moves) {
            if (g_cancel) break;
            Item it; it.m = m;
            std::wstring target = JoinRel(m.origDir, m.origName);
            if (!CurrentRel(m.frn, it.cur)) it.status = S_MISSING;
            else if (rc::Lower(it.cur) == rc::Lower(target)) it.status = S_BACK;
            else if (Exists(Abs(target))) it.status = S_TAKEN;
            else if (rc::Lower(it.cur).compare(0, 12, L"$recycle.bin") == 0) it.status = S_BIN;
            else it.status = S_READY;
            g_items.push_back(std::move(it));
            InterlockedIncrement(&g_pDone);
        }
        for (auto& it : g_items) {
            int s = it.m.session;
            if ((int)g_sessions.size() <= s) g_sessions.resize(s + 1);
            Session& S2 = g_sessions[s];
            if (!S2.count) S2.start = it.m.time;
            S2.end = it.m.time; S2.count++;
        }
        // a plain-text copy of the plan, next to the drive's other files, so it can be checked before anything moves
        HANDLE f = CreateFileW(Long(Root() + L"_organizer_rescue_preview.txt").c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteUtf8(f, L"Organizer Rescue preview - nothing has been changed yet\r\nstatus\tsession\tmoved at\tnow at\toriginal location\r\n");
            for (auto& it : g_items)
                WriteUtf8(f, std::wstring(kStatName[it.status]) + L"\t" + std::to_wstring(it.m.session + 1) + L"\t" + TimeStr(it.m.time) + L"\t" +
                                 (it.status == S_MISSING ? L"-" : it.cur) + L"\t" + JoinRel(it.m.origDir, it.m.origName) + (it.m.isDir ? L"\\" : L"") + L"\r\n");
            CloseHandle(f);
        }
    }
    if (g_rootH != INVALID_HANDLE_VALUE) { CloseHandle(g_rootH); g_rootH = INVALID_HANDLE_VALUE; }
    PostMessageW(g_hwnd, WM_APP_SCANNED, 0, 0);
    return 0;
}

static DWORD WINAPI RestoreThread(LPVOID) {
    g_okCount = g_failCount = g_skipCount = 0;
    g_rootH = CreateFileW(Root().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE log = CreateFileW(Long(Root() + L"_organizer_rescue_log.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (log != INVALID_HANDLE_VALUE) WriteUtf8(log, L"--- rescue run " + TimeStr([] { FILETIME f; GetSystemTimeAsFileTime(&f); return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; }()) + L"\r\nput back to\tfrom\r\n");
    std::set<std::wstring> destDirs;
    g_pDone = 0;
    for (auto& it : g_items) {
        if (g_cancel) break;
        if (!g_sessions[it.m.session].on || (it.status != S_READY && it.status != S_BIN)) continue;
        InterlockedIncrement(&g_pDone);
        std::wstring cur;
        if (!CurrentRel(it.m.frn, cur)) { g_failCount++; it.status = S_MISSING; continue; }   // look again: it may have moved
        std::wstring target = JoinRel(it.m.origDir, it.m.origName);
        if (Exists(Abs(target))) { g_skipCount++; it.status = S_TAKEN; continue; }
        MakeDirs(it.m.origDir);
        if (MoveFileExW(Long(Abs(cur)).c_str(), Long(Abs(target)).c_str(), 0)) {
            g_okCount++; it.status = S_BACK; it.restored = true; it.cur = target;
            destDirs.insert(it.m.destDir);
            if (log != INVALID_HANDLE_VALUE) WriteUtf8(log, target + L"\t" + cur + L"\r\n");
        } else g_failCount++;
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    // tidy up Organizer's folders if they ended up empty (only works on empty folders)
    std::vector<std::wstring> dd(destDirs.begin(), destDirs.end());
    std::sort(dd.begin(), dd.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (auto& d : dd) RemoveDirectoryW(Long(Abs(d)).c_str());
    if (g_rootH != INVALID_HANDLE_VALUE) { CloseHandle(g_rootH); g_rootH = INVALID_HANDLE_VALUE; }
    PostMessageW(g_hwnd, WM_APP_RESTORED, 0, 0);
    return 0;
}

static void StartScan() {
    if (!g_drive || g_state == ST_SCANNING || g_state == ST_RESTORING) return;
    g_cancel = 0; g_pRecs = 0; g_pDone = 0; g_pTotal = 0; g_lscroll = 0; g_doneMsg.clear();
    InterlockedExchange(&g_state, ST_SCANNING);
    g_thread = CreateThread(NULL, 64u << 20, ScanThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    SetTimer(g_hwnd, 1, 150, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static int CountSelected(int* bin = nullptr) {
    int n = 0, b = 0;
    for (auto& it : g_items)
        if (g_sessions[it.m.session].on && (it.status == S_READY || it.status == S_BIN)) { n++; if (it.status == S_BIN) b++; }
    if (bin) *bin = b;
    return n;
}
static void StartRestore() {
    int bin = 0, n = CountSelected(&bin);
    if (!n || g_state != ST_READY) return;
    std::wstring msg = L"Put " + NumStr(n) + L" item" + (n == 1 ? L"" : L"s") + L" back where Organizer found them?\n\n";
    if (bin) msg += NumStr(bin) + L" of them are in the Recycle Bin and will be taken out of it.\n\n";
    msg += L"Nothing is overwritten: if something else is already sitting in an item's original spot, that item is skipped.\n"
           L"Every move is written to _organizer_rescue_log.txt on the drive.";
    if (MessageBoxW(g_hwnd, msg.c_str(), L"Organizer Rescue", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_cancel = 0; g_pDone = 0; g_pTotal = n;
    InterlockedExchange(&g_state, ST_RESTORING);
    g_thread = CreateThread(NULL, 0, RestoreThread, NULL, 0, NULL);
    SetTimer(g_hwnd, 1, 150, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------------------------------------------------------------- drawing
static void Layout(const RECT& cr) {
    rcHead = { 0, 0, cr.right, S(68) };
    rcCard = { S(16), rcHead.bottom + S(14), cr.right - S(16), rcHead.bottom + S(14) + S(100) };
    rcFoot = { 0, cr.bottom - S(30), cr.right, cr.bottom };
    int top = rcCard.bottom + S(14);
    rcSess = { S(16), top, S(16) + S(300), rcFoot.top - S(14) };
    rcList = { rcSess.right + S(14), top, cr.right - S(16), rcFoot.top - S(14) };
    rcLBody = { rcList.left + S(6), rcList.top + S(40), rcList.right - S(6), rcList.bottom - S(6) };
}
static void Button(HDC dc, RECT r, const wchar_t* label, int id, bool primary, bool enabled = true) {
    bool hv = enabled && IsHover(id);
    COLORREF bg = primary ? (enabled ? (hv ? Mix(ACCENT, RGB(255, 255, 255), 0.12) : ACCENT) : PANEL3) : (hv ? PANEL3 : PANEL2);
    RoundC(dc, r, S(8), bg);
    if (!primary) RoundOutline(dc, r, S(8), hv ? RGB(70, 76, 88) : BORDER);
    Text(dc, label, r, g_fSemi, primary ? (enabled ? RGB(10, 14, 22) : TEXT3) : (enabled ? TEXT1 : TEXT3), TC);
    if (enabled) AddClick(r, id);
}
static void Check(HDC dc, int x, int y, bool on, bool hover) {
    RECT b = { x, y, x + S(16), y + S(16) };
    if (on) RoundC(dc, b, S(5), ACCENT);
    else { RoundC(dc, b, S(5), hover ? PANEL3 : PANEL2); RoundOutline(dc, b, S(5), hover ? TEXT3 : RGB(70, 76, 88)); }
    if (on) {
        HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(14, 16, 20)); HGDIOBJ op = SelectObject(dc, pen);
        POINT pts[3] = { { x + S(4), y + S(8) }, { x + S(7), y + S(11) }, { x + S(12), y + S(5) } };
        Polyline(dc, pts, 3); SelectObject(dc, op); DeleteObject(pen);
    }
}

static void DrawHead(HDC dc) {
    FillC(dc, rcHead, PANEL);
    RECT line = { 0, rcHead.bottom - 1, rcHead.right, rcHead.bottom }; FillC(dc, line, BORDER);
    int cy = (rcHead.top + rcHead.bottom) / 2;
    RECT logo = { S(20), cy - S(11), S(42), cy + S(11) }; RoundC(dc, logo, S(6), GOOD);
    RECT t = { S(54), rcHead.top, S(260), rcHead.bottom };
    Text(dc, L"Organizer Rescue", t, g_fTitle, TEXT1, TL);
    bool busy = g_state == ST_SCANNING || g_state == ST_RESTORING;
    int x = S(240);
    RECT lab = { x, rcHead.top, x + S(50), rcHead.bottom };
    Text(dc, L"Drive", lab, g_fSmallSemi, TEXT3, TL);
    x += S(46);
    for (size_t i = 0; i < g_drives.size(); i++) {
        RECT r = { x, cy - S(17), x + S(52), cy + S(17) };
        bool sel = g_drives[i] == g_drive, hv = !busy && IsHover(ID_DRIVE, (u32)i);
        RoundC(dc, r, S(8), sel ? RGB(30, 44, 66) : (hv ? PANEL3 : PANEL2));
        RoundOutline(dc, r, S(8), sel ? ACCENT : BORDER);
        wchar_t s[4] = { g_drives[i], L':', 0 };
        Text(dc, s, r, g_fSemi, TEXT1, TC);
        if (!busy) AddClick(r, ID_DRIVE, (u32)i);
        x = r.right + S(6);
    }
}

static void DrawCard(HDC dc) {
    std::wstring title, sub; COLORREF c = ACCENT;
    wchar_t d[4] = { g_drive ? g_drive : L'?', L':', 0 };
    int bin = 0, n = g_state == ST_READY || g_state == ST_DONE ? CountSelected(&bin) : 0;
    int counts[NSTAT] = {};
    for (auto& it : g_items) counts[it.status]++;
    switch (g_state) {
    case ST_IDLE:
        title = std::wstring(L"Find what Organizer moved on ") + d;
        sub = L"Reads Windows' change journal for the drive to see where each file came from. Nothing is changed until you press Put back.";
        break;
    case ST_SCANNING:
        title = L"Reading the change journal\x2026";
        sub = NumStr((u64)g_pRecs) + L" journal entries read" + (g_pTotal ? L"  \x00B7  checking " + NumStr((u64)g_pDone) + L" of " + NumStr((u64)g_pTotal) + L" moved items" : L"");
        break;
    case ST_RESTORING:
        title = L"Putting things back\x2026";
        sub = NumStr((u64)g_pDone) + L" of " + NumStr((u64)g_pTotal);
        break;
    case ST_ERROR: title = L"Couldn't read the journal"; sub = g_err; c = BAD; break;
    case ST_READY:
    case ST_DONE:
        if (g_items.empty()) {
            title = L"No Organizer moves found on this drive"; c = WARN;
            sub = L"The journal starts on " + (g_journalFrom ? TimeStr(g_journalFrom) : std::wstring(L"?")) + L". If Organizer ran before that, Windows has already recycled those entries.";
        } else if (g_state == ST_DONE) {
            title = g_doneMsg; c = g_failCount || g_skipCount ? WARN : GOOD;
            sub = L"Moves are listed in _organizer_rescue_log.txt on the drive.";
        } else {
            title = L"Found " + NumStr(g_items.size()) + L" items Organizer moved";
            sub = NumStr(counts[S_READY]) + L" ready to put back";
            if (counts[S_BIN]) sub += L"  \x00B7  " + NumStr(counts[S_BIN]) + L" in the Recycle Bin (can be put back)";
            if (counts[S_TAKEN]) sub += L"  \x00B7  " + NumStr(counts[S_TAKEN]) + L" whose spot is taken";
            if (counts[S_BACK]) sub += L"  \x00B7  " + NumStr(counts[S_BACK]) + L" already back";
            if (counts[S_MISSING]) sub += L"  \x00B7  " + NumStr(counts[S_MISSING]) + L" gone for good";
        }
        break;
    }
    RoundC(dc, rcCard, S(12), Mix(PANEL, c, 0.10));
    RoundOutline(dc, rcCard, S(12), Mix(BORDER, c, 0.35));
    RECT stripe = { rcCard.left, rcCard.top + S(14), rcCard.left + S(4), rcCard.bottom - S(14) }; FillC(dc, stripe, c);
    int L = rcCard.left + S(24), R = rcCard.right - S(20);
    RECT b = { R - S(170), rcCard.top + S(30), R, rcCard.top + S(68) };
    if (g_state == ST_SCANNING || g_state == ST_RESTORING) { b.left = R - S(110); Button(dc, b, L"Cancel", ID_CANCEL, false); }
    else if (g_state == ST_READY && n) {
        std::wstring lab = L"Put back " + NumStr(n);
        b.left = R - std::max<int>(S(170), TextW(dc, lab.c_str(), g_fSemi) + S(40));
        Button(dc, b, lab.c_str(), ID_RESTORE, true);
        RECT b2 = { b.left - S(10) - S(100), b.top, b.left - S(10), b.bottom };
        Button(dc, b2, L"Rescan", ID_SCAN, false);
        b = b2;
    } else {
        std::wstring lab = std::wstring(g_state == ST_IDLE ? L"Scan " : L"Rescan ") + d;
        Button(dc, b, lab.c_str(), ID_SCAN, g_state == ST_IDLE || g_state == ST_ERROR, g_drive != 0);
    }
    RECT tr = { L, rcCard.top + S(16), b.left - S(20), rcCard.top + S(50) };
    Text(dc, title.c_str(), tr, g_fBig, TEXT1, TL);
    RECT sr = { L, rcCard.top + S(50), b.left - S(20), rcCard.bottom - S(10) };
    Text(dc, sub.c_str(), sr, g_fUI, TEXT2, DT_WORDBREAK);
    if (g_state == ST_SCANNING || g_state == ST_RESTORING) {
        double frac = g_pTotal ? (double)g_pDone / g_pTotal : 0.05;
        RECT bar = { L, rcCard.bottom - S(16), b.left - S(20), rcCard.bottom - S(10) };
        RoundC(dc, bar, S(3), RGB(46, 51, 60));
        RECT f = bar; f.right = bar.left + std::max<LONG>(S(8), (LONG)((bar.right - bar.left) * std::min(1.0, frac)));
        RoundC(dc, f, S(3), ACCENT);
    }
}

static void DrawSessions(HDC dc) {
    RoundC(dc, rcSess, S(12), PANEL); RoundOutline(dc, rcSess, S(12), BORDER);
    int L = rcSess.left + S(18), R = rcSess.right - S(18), y = rcSess.top + S(12);
    RECT h = { L, y, R, y + S(20) };
    Text(dc, L"ORGANIZER RUNS FOUND", h, g_fSmallSemi, TEXT3, TL);
    y += S(32);
    if (g_state != ST_READY && g_state != ST_DONE) return;
    for (size_t i = 0; i < g_sessions.size(); i++) {
        const Session& s = g_sessions[i];
        if (y + S(46) > rcSess.bottom) break;
        RECT row = { L - S(8), y, R + S(8), y + S(46) };
        bool hv = g_state == ST_READY && IsHover(ID_SESSION, (u32)i);
        if (hv) RoundC(dc, row, S(8), PANEL2);
        Check(dc, L, y + S(8), s.on, hv);
        RECT t1 = { L + S(26), y + S(3), R, y + S(23) };
        Text(dc, TimeStr(s.start).c_str(), t1, g_fSemi, s.on ? TEXT1 : TEXT2, TL);
        std::wstring sub = NumStr(s.count) + L" items  \x00B7  until " + TimeStr(s.end, false);
        RECT t2 = { L + S(26), y + S(22), R, y + S(40) };
        Text(dc, sub.c_str(), t2, g_fSmall, TEXT3, TL);
        if (g_state == ST_READY) AddClick(row, ID_SESSION, (u32)i);
        y += S(50);
    }
    y += S(10);
    RECT note = { L, y, R, rcSess.bottom - S(12) };
    Text(dc, L"Each run of Organizer shows up here. Untick a run to leave its files where they are now.\n\n"
             L"Items are put back under their original names. If something new already sits in an item's old spot, that item is skipped rather than overwriting anything.",
         note, g_fSmall, TEXT3, DT_WORDBREAK);
}

static void DrawList(HDC dc) {
    RoundC(dc, rcList, S(12), PANEL); RoundOutline(dc, rcList, S(12), BORDER);
    RECT h = { rcList.left + S(18), rcList.top + S(8), rcList.right - S(18), rcList.top + S(34) };
    Text(dc, L"WHERE EACH ITEM GOES BACK TO", h, g_fSmallSemi, TEXT3, TL);
    if (g_state != ST_READY && g_state != ST_DONE) return;
    int rowH = S(46);
    int content = (int)g_items.size() * rowH, view = rcLBody.bottom - rcLBody.top;
    g_lscroll = std::max(0, std::min(g_lscroll, std::max(0, content - view)));
    HRGN clip = CreateRectRgn(rcLBody.left, rcLBody.top, rcLBody.right, rcLBody.bottom);
    SelectClipRgn(dc, clip);
    for (int i = g_lscroll / rowH; i < (int)g_items.size(); i++) {
        int y = rcLBody.top + i * rowH - g_lscroll;
        if (y >= rcLBody.bottom) break;
        const Item& it = g_items[i];
        bool on = g_sessions[it.m.session].on;
        int pw = S(108);
        RECT pill = { rcLBody.left + S(10), y + S(8), rcLBody.left + S(10) + pw, y + S(28) };
        RoundC(dc, pill, S(10), Mix(PANEL, kStatColor[it.status], on ? 0.28 : 0.10));
        Text(dc, kStatName[it.status], pill, g_fSmallSemi, on ? TEXT1 : TEXT3, TC);
        std::wstring orig = JoinRel(it.m.origDir, it.m.origName) + (it.m.isDir ? L"\\" : L"");
        RECT t1 = { pill.right + S(14), y + S(4), rcLBody.right - S(14), y + S(24) };
        HGDIOBJ of = SelectObject(dc, g_fSemi); SetTextColor(dc, on ? TEXT1 : TEXT3);
        DrawTextW(dc, orig.c_str(), -1, &t1, TLP | DT_NOPREFIX);
        std::wstring now = it.status == S_MISSING ? L"permanently deleted since \x2014 can't be recovered from here"
                         : (it.restored ? L"put back" : L"now at " + it.cur);
        RECT t2 = { pill.right + S(14), y + S(23), rcLBody.right - S(14), y + S(41) };
        SelectObject(dc, g_fSmall); SetTextColor(dc, TEXT3);
        DrawTextW(dc, now.c_str(), -1, &t2, TLP | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    SelectClipRgn(dc, NULL); DeleteObject(clip);
    if (content > view) {
        RECT track = { rcLBody.right - S(6), rcLBody.top + S(2), rcLBody.right - S(2), rcLBody.bottom - S(2) };
        int th = std::max<int>(S(30), (int)((long long)(track.bottom - track.top) * view / content));
        int ty = track.top + (int)((long long)(track.bottom - track.top - th) * g_lscroll / (content - view));
        RECT t = { track.left, ty, track.right, ty + th }; RoundC(dc, t, S(4), RGB(70, 76, 88));
    }
}

static void DrawFoot(HDC dc) {
    FillC(dc, rcFoot, PANEL);
    RECT line = { 0, rcFoot.top, rcFoot.right, rcFoot.top + 1 }; FillC(dc, line, BORDER);
    std::wstring s = L"Only moves into Organizer's folders (Images, Programs, Code, Documents\x2026) are listed.";
    if (g_journalFrom) s += L"   Journal goes back to " + TimeStr(g_journalFrom) + L".";
    RECT r = { S(16), rcFoot.top, rcFoot.right - S(16), rcFoot.bottom };
    Text(dc, s.c_str(), r, g_fSmall, TEXT3, TL);
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    if (cr.right > 0 && cr.bottom > 0) {
        HDC m = CreateCompatibleDC(hdc);
        HBITMAP bb = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(m, bb);
        SetBkMode(m, TRANSPARENT);
        FillC(m, cr, BG);
        Layout(cr);
        g_clicks.clear();
        DrawHead(m); DrawCard(m); DrawSessions(m); DrawList(m); DrawFoot(m);
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, ob); DeleteObject(bb); DeleteDC(m);
    }
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- window
static void UpdateHover(int x, int y) {
    int id = -1; u32 data = 0; POINT p = { x, y };
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--) if (PtInRect(&g_clicks[i].rc, p)) { id = g_clicks[i].id; data = g_clicks[i].data; break; }
    if (id != g_hoverId || data != g_hoverData) { g_hoverId = id; g_hoverData = data; InvalidateRect(g_hwnd, NULL, FALSE); }
}
static void OnClick(int x, int y) {
    POINT p = { x, y };
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--) {
        if (!PtInRect(&g_clicks[i].rc, p)) continue;
        Click c = g_clicks[i];
        switch (c.id) {
        case ID_DRIVE: if (c.data < g_drives.size()) { g_drive = g_drives[c.data]; g_items.clear(); g_sessions.clear(); g_state = ST_IDLE; } break;
        case ID_SCAN: StartScan(); break;
        case ID_RESTORE: StartRestore(); break;
        case ID_CANCEL: InterlockedExchange(&g_cancel, 1); break;
        case ID_SESSION: if (c.data < g_sessions.size()) g_sessions[c.data].on = !g_sessions[c.data].on; break;
        }
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(900); mm->ptMinTrackSize.y = S(560); return 0; }
    case WM_MOUSEMOVE:
        if (!g_tracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t); g_tracking = true; }
        UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MOUSELEAVE: g_tracking = false; if (g_hoverId != -1) { g_hoverId = -1; InvalidateRect(hwnd, NULL, FALSE); } return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, g_hoverId >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MOUSEWHEEL: g_lscroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(46) * 3 / WHEEL_DELTA; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_TIMER: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_APP_SCANNED:
        KillTimer(hwnd, 1);
        if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
        InterlockedExchange(&g_state, g_cancel ? ST_IDLE : (g_err.empty() ? ST_READY : ST_ERROR));
        InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_APP_RESTORED: {
        KillTimer(hwnd, 1);
        if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
        g_doneMsg = L"Put back " + NumStr(g_okCount) + L" item" + (g_okCount == 1 ? L"" : L"s");
        if (g_skipCount) g_doneMsg += L"  \x00B7  " + NumStr(g_skipCount) + L" skipped (spot taken)";
        if (g_failCount) g_doneMsg += L"  \x00B7  " + NumStr(g_failCount) + L" failed";
        InterlockedExchange(&g_state, ST_DONE);
        InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_DESTROY:
        if (g_thread) { InterlockedExchange(&g_cancel, 1); WaitForSingleObject(g_thread, 10000); }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    HDC sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    auto mk = [](int px, int w) { return CreateFontW(-S(px), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"); };
    g_fUI = mk(13, FW_NORMAL); g_fSemi = mk(13, FW_SEMIBOLD); g_fSmall = mk(12, FW_NORMAL); g_fSmallSemi = mk(12, FW_SEMIBOLD);
    g_fTitle = mk(18, FW_SEMIBOLD); g_fBig = mk(20, FW_SEMIBOLD);

    // NTFS drives only (the change journal is an NTFS feature)
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 }, fs[32] = L"";
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        if (GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32) && !_wcsicmp(fs, L"NTFS")) g_drives.push_back(root[0]);
    }
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    for (wchar_t d : g_drives) if (towupper(exe[0]) == d) g_drive = d;
    if (!g_drive) for (wchar_t d : g_drives) if (d == L'H') g_drive = d;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.lpszClassName = L"OrganizerRescueWnd";
    RegisterClassExW(&wc);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int W = std::min<int>(S(1200), (wa.right - wa.left) * 9 / 10), H = std::min<int>(S(780), (wa.bottom - wa.top) * 9 / 10);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Organizer Rescue", WS_OVERLAPPEDWINDOW,
                             wa.left + (wa.right - wa.left - W) / 2, wa.top + (wa.bottom - wa.top - H) / 2, W, H, NULL, NULL, hi, NULL);
    BOOL dark = TRUE; DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
    ShowWindow(g_hwnd, show); UpdateWindow(g_hwnd);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
