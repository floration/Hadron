// SaveFinder - checks a Steam library for saves, worlds and other personal data before you delete it
// Win32 + GDI only, no third-party libraries.
// Build: x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows savefinder.cpp res.o -o SaveFinder.exe
//        -static -s -lgdi32 -lmsimg32 -lshell32 -lole32 -luuid -ldwmapi -ladvapi32

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
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>

typedef uint64_t u64;
typedef uint32_t u32;

#define WM_APP_DONE (WM_APP + 1)
#define WM_APP_COPIED (WM_APP + 2)
#define TIMER_PROGRESS 1

// ---------------------------------------------------------------- theme
static const COLORREF BG      = RGB(17, 19, 23);
static const COLORREF PANEL   = RGB(24, 27, 33);
static const COLORREF PANEL2  = RGB(32, 36, 43);
static const COLORREF PANEL3  = RGB(42, 47, 56);
static const COLORREF BORDER  = RGB(40, 44, 52);
static const COLORREF TEXT1   = RGB(232, 235, 240);
static const COLORREF TEXT2   = RGB(156, 163, 176);
static const COLORREF TEXT3   = RGB(108, 115, 128);
static const COLORREF ACCENT  = RGB(96, 160, 255);
static const COLORREF GOOD    = RGB(80, 214, 140);
static const COLORREF WARN    = RGB(240, 180, 70);
static const COLORREF BAD     = RGB(236, 98, 98);

// ---------------------------------------------------------------- what we look for
enum { K_SAVE, K_WORLD, K_PROTON, K_CLOUD, K_SHOTS, K_REPLAY, K_MODS, K_CHANGED, NKIND };
struct KindDef { const wchar_t* name; const wchar_t* plural; COLORREF color; int sev; bool backup; };
static const KindDef kKind[NKIND] = {
    { L"Save",          L"saves",          RGB(236, 98, 98),   2, true  },
    { L"World",         L"worlds",         RGB(238, 140, 66),  2, true  },
    { L"Proton save",   L"Proton saves",   RGB(236, 98, 98),   2, true  },
    { L"Steam data",    L"Steam data",     RGB(240, 180, 70),  2, true  },
    { L"Screenshots",   L"screenshots",    RGB(72, 196, 164),  1, true  },
    { L"Replays",       L"replays",        RGB(84, 146, 236),  1, true  },
    { L"Mods",          L"mods",           RGB(172, 124, 236), 1, false },
    { L"Changed file",  L"changed files",  RGB(120, 128, 142), 0, false },
};
static const COLORREF kSevColor[3] = { RGB(120, 128, 142), WARN, BAD };

struct Game {
    std::wstring name, dir, appid;
    bool installed = true, hasTime = false;
    u64 lastUpd = 0;              // FILETIME of Steam's last update of the game
    u64 size = 0; u32 files = 0;
    int cnt[NKIND] = {};
    int sev = -1;
    std::vector<int> finds;
};
struct Find {
    int game = 0, kind = 0;
    std::wstring path, shown, why;
    u64 size = 0, mt = 0; u32 files = 0;
    bool isDir = false;
};
static std::vector<Game> g_games;
static std::vector<Find> g_finds;

struct Library { std::wstring path; int games; };
static std::vector<Library> g_libs;

enum { ST_IDLE, ST_SCANNING, ST_DONE, ST_COPYING };
static volatile LONG g_state = ST_IDLE;
static volatile LONG g_cancel = 0;
static volatile LONG g_pFiles = 0, g_pGame = 0, g_pGames = 0;
static volatile LONG64 g_cDone = 0, g_cTotal = 0;
static CRITICAL_SECTION g_lock;
static std::wstring g_pName;
static std::wstring g_root, g_rootShown;
static bool g_steamLib = false;
static DWORD g_t0 = 0, g_scanMs = 0;
static HANDLE g_thread = NULL;
static std::wstring g_msg; static int g_msgKind = 0;
static std::wstring g_backupDir;
static int g_copied = 0, g_copyFail = 0;

static HWND g_hwnd;
static int g_dpi = 96;
static inline int S(int v) { return MulDiv(v, g_dpi, 96); }
static HFONT g_fUI, g_fSemi, g_fSmall, g_fSmallSemi, g_fTitle, g_fHuge, g_fBig;

static RECT rcHead, rcSum, rcGames, rcDetail, rcGBody, rcDBody, rcFoot;
static int g_sel = -1, g_gscroll = 0, g_dscroll = 0;
static bool g_tracking = false;

struct Click { RECT rc; int id; u32 data; };
static std::vector<Click> g_clicks;
static int g_hoverId = -1; static u32 g_hoverData = 0;
enum { ID_CHOOSE = 1, ID_BACKUP, ID_RESCAN, ID_CANCEL, ID_OPENGAME, ID_DISMISS, ID_LIB = 100, ID_GAME = 1000, ID_FIND = 1000000 };

// ---------------------------------------------------------------- helpers
static void FmtSize(u64 b, wchar_t* out, int n) {
    static const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) _snwprintf(out, n, L"%llu B", (unsigned long long)b);
    else _snwprintf(out, n, v < 10 ? L"%.2f %ls" : (v < 100 ? L"%.1f %ls" : L"%.0f %ls"), v, u[i]);
    out[n - 1] = 0;
}
static std::wstring SizeStr(u64 b) { wchar_t t[32]; FmtSize(b, t, 32); return t; }
static std::wstring NumStr(u64 v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L",");
    return s;
}
static std::wstring DateStr(u64 ft) {
    if (!ft) return L"";
    FILETIME f, lf; f.dwLowDateTime = (DWORD)ft; f.dwHighDateTime = (DWORD)(ft >> 32);
    SYSTEMTIME st; FileTimeToLocalFileTime(&f, &lf); FileTimeToSystemTime(&lf, &st);
    static const wchar_t* mon[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    wchar_t b[40]; _snwprintf(b, 40, L"%ls %u, %u", mon[(st.wMonth + 11) % 12], st.wDay, st.wYear); b[39] = 0;
    return b;
}
static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t), (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
static void FillC(HDC dc, const RECT& r, COLORREF c) { SetDCBrushColor(dc, c); FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH)); }
static void RoundC(HDC dc, const RECT& r, int rad, COLORREF c) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, c); SetDCPenColor(dc, c);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
}
static void RoundOutline(HDC dc, const RECT& r, int rad, COLORREF c) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, c);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
}
static void Text(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}
static int TextW(HDC dc, const wchar_t* s, HFONT f) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz; GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, of);
    return sz.cx;
}
static const UINT TL = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS;
static const UINT TLP = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_PATH_ELLIPSIS;
static const UINT TR = DT_SINGLELINE | DT_VCENTER | DT_RIGHT;
static const UINT TC = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
static void AddClick(const RECT& r, int id, u32 data = 0) { g_clicks.push_back({ r, id, data }); }
static bool IsHover(int id, u32 data = 0) { return g_hoverId == id && g_hoverData == data; }

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (!a.empty() && a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}
static std::wstring Long(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(Long(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring LowerW(std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }
static bool InList(const std::wstring& s, const wchar_t* const* l) { for (; *l; ++l) if (s == *l) return true; return false; }
static bool AllDigits(const std::wstring& s) { if (s.empty()) return false; for (wchar_t c : s) if (!iswdigit(c)) return false; return true; }
static std::wstring Leaf(const std::wstring& p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? p : p.substr(k + 1); }
static std::wstring Parent(const std::wstring& p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? L"" : p.substr(0, k); }
static u64 FT(const FILETIME& f) { return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static std::wstring ReadText(const std::wstring& path) {
    HANDLE f = CreateFileW(Long(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    if (sz.QuadPart > (8 << 20)) { CloseHandle(f); return L""; }
    std::string b((size_t)sz.QuadPart, 0);
    DWORD rd = 0;
    if (sz.QuadPart) ReadFile(f, &b[0], (DWORD)b.size(), &rd, NULL);
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, b.data(), (int)rd, NULL, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, b.data(), (int)rd, &w[0], n);
    return w;
}
// Valve KeyValues text (.acf / .vdf): value of the next "key" "value" pair after `from`
static std::wstring VdfVal(const std::wstring& t, const std::wstring& tl, const wchar_t* key, size_t from = 0, size_t* at = nullptr) {
    std::wstring k = L"\"" + std::wstring(key) + L"\"";
    size_t p = from;
    while ((p = tl.find(k, p)) != std::wstring::npos) {
        size_t q = p + k.size();
        while (q < t.size() && iswspace(t[q])) q++;
        if (q < t.size() && t[q] == L'"') {
            std::wstring v; q++;
            while (q < t.size() && t[q] != L'"') {
                if (t[q] == L'\\' && q + 1 < t.size()) q++;
                v += t[q]; q++;
            }
            if (at) *at = q;
            return v;
        }
        p = q;
    }
    if (at) *at = std::wstring::npos;
    return L"";
}

// ---------------------------------------------------------------- Steam library discovery
static std::wstring SteamPath() {
    wchar_t buf[1024]; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS) {
        std::wstring s = buf; for (auto& c : s) if (c == L'/') c = L'\\';
        return s;
    }
    sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS)
        return buf;
    return L"";
}
static int CountManifests(const std::wstring& lib) {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Long(Join(lib, L"steamapps\\appmanifest_*.acf")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do n++; while (FindNextFileW(h, &fd)); FindClose(h); }
    return n;
}
static void FindLibraries() {
    g_libs.clear();
    std::vector<std::wstring> paths;
    std::wstring sp = SteamPath();
    if (!sp.empty()) {
        paths.push_back(sp);
        std::wstring t = ReadText(Join(sp, L"steamapps\\libraryfolders.vdf"));
        std::wstring tl = LowerW(t);
        size_t at = 0;
        for (;;) {
            std::wstring v = VdfVal(t, tl, L"path", at, &at);
            if (at == std::wstring::npos) break;
            for (auto& c : v) if (c == L'/') c = L'\\';
            if (!v.empty()) paths.push_back(v);
        }
    }
    std::set<std::wstring> seen;
    for (auto& p : paths) {
        std::wstring q = p; while (q.size() > 3 && q.back() == L'\\') q.pop_back();
        if (seen.count(LowerW(q)) || !IsDir(Join(q, L"steamapps"))) continue;
        seen.insert(LowerW(q));
        g_libs.push_back({ q, CountManifests(q) });
    }
}

// ---------------------------------------------------------------- scanning
static const u64 GRACE = 2ull * 3600 * 10000000;   // 2 hours: files Steam itself wrote during the update
static const wchar_t* const kQuietDirs[] = { L"logs", L"log", L"crashes", L"crashdumps", L"crashreports", L"crashreporter",
    L"shadercache", L"_commonredist", L"commonredist", L"redist", L"redistributables", L"directx", L"vcredist", L"dotnet",
    L"__installer", L"installers", L"__pycache__", L"pythonlib", nullptr };
static const wchar_t* const kWeakSaveDirs[] = { L"world", L"worlds", L"profiles", L"profile", L"playerdata", L"players",
    L"characters", L"userdata", L"user data", L"persistent", L"slots", L"universes", L"campaigns", L"user", nullptr };
static const wchar_t* const kModDirs[] = { L"mods", L"mod", L"bepinex", L"melonloader", L"user mods", L"usermods", nullptr };
static const wchar_t* const kSaveExts[] = { L"sav", L"save", L"savegame", L"sl2", L"ess", L"fos", L"rpgsave", L"rmmzsave",
    L"wld", L"plr", L"lsv", L"srm", L"es3", L"sav2", L"dsv", nullptr };
static const wchar_t* const kNotSaveExts[] = { L"dll", L"exe", L"pdb", L"lib", L"so", L"pak", L"uasset", L"uexp", L"assets",
    L"bundle", L"resource", L"ress", L"cs", L"js", L"lua", L"py", L"pyc", L"png", L"jpg", L"dds", L"ogg", L"wav", L"mp3",
    L"bank", L"fsb", L"bnk", L"wem", L"shader", L"mat", L"prefab", L"txt", L"rpy", L"rpyc", L"ttf", nullptr };
static const wchar_t* const kLogExts[] = { L"log", L"dmp", L"mdmp", L"tmp", L"etl", nullptr };
static const wchar_t* const kCfgExts[] = { L"ini", L"cfg", L"json", L"xml", L"conf", L"config", L"vdf", L"yaml", L"yml", L"toml", L"txt", nullptr };

struct Stats { u64 size = 0; u32 files = 0; u64 newest = 0; };
static void WalkStats(const std::wstring& dir, Stats& st, int depth) {
    if (depth > 40 || g_cancel) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) WalkStats(Join(dir, n), st, depth + 1);
        } else {
            st.size += ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            st.files++;
            st.newest = std::max(st.newest, FT(fd.ftLastWriteTime));
            InterlockedIncrement(&g_pFiles);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void AddFind(int gi, int kind, const std::wstring& path, const std::wstring& shown, bool isDir,
                    u64 size, u32 files, u64 mt, const std::wstring& why) {
    Find f;
    f.game = gi; f.kind = kind; f.path = path; f.shown = shown; f.isDir = isDir;
    f.size = size; f.files = files; f.mt = mt; f.why = why;
    g_finds.push_back(f);
}

static std::wstring ExtOf(const std::wstring& ln) {
    size_t d = ln.rfind(L'.');
    return d == std::wstring::npos ? L"" : ln.substr(d + 1);
}
static bool IsSaveFile(const std::wstring& ln) {
    std::wstring ext = ExtOf(ln);
    if (InList(ext, kSaveExts)) return true;
    if (InList(ext, kNotSaveExts)) return false;
    return ln.rfind(L"save", 0) == 0 || ln.rfind(L"quicksave", 0) == 0 || ln.rfind(L"autosave", 0) == 0 ||
           ln.find(L"savegame") != std::wstring::npos;
}

static int g_changedCap = 0;

static void WalkGame(int gi, const std::wstring& abs, const std::wstring& rel, int depth, bool quiet) {
    if (g_cancel || depth > 40) return;
    struct FE { std::wstring name; u64 size, mt; };
    std::vector<FE> files;
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(abs, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) dirs.push_back(n);
        } else files.push_back({ n, ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, FT(fd.ftLastWriteTime) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    auto relOf = [&](const std::wstring& n) { return rel.empty() ? n : rel + L"\\" + n; };
    bool world = false;
    for (auto& f : files) {
        g_games[gi].size += f.size; g_games[gi].files++;
        InterlockedIncrement(&g_pFiles);
        if (LowerW(f.name) == L"level.dat") world = true;
    }
    // a folder with level.dat is a world (Minecraft-style): report it whole
    if (world && depth > 0 && !quiet) {
        Stats st; WalkStats(abs, st, 0);
        AddFind(gi, K_WORLD, abs, rel, true, st.size, st.files, st.newest, L"Contains level.dat \x2014 this is a world save");
        for (auto& d : dirs) WalkGame(gi, Join(abs, d), relOf(d), depth + 1, true);
        return;
    }
    const Game& G = g_games[gi];
    if (!quiet) {
        for (auto& f : files) {
            std::wstring ln = LowerW(f.name);
            if (IsSaveFile(ln)) {
                AddFind(gi, K_SAVE, Join(abs, f.name), relOf(f.name), false, f.size, 1, f.mt, L"Save file");
            } else if (G.hasTime && f.mt > G.lastUpd + GRACE && !InList(ExtOf(ln), kLogExts) && g_changedCap < 400) {
                g_changedCap++;
                AddFind(gi, K_CHANGED, Join(abs, f.name), relOf(f.name), false, f.size, 1, f.mt,
                        InList(ExtOf(ln), kCfgExts) ? L"Settings/config file changed after the game's last update"
                                                    : L"Changed after the game's last update");
            }
        }
    }
    for (auto& d : dirs) {
        std::wstring ln = LowerW(d), cabs = Join(abs, d), crel = relOf(d);
        if (quiet || InList(ln, kQuietDirs)) { WalkGame(gi, cabs, crel, depth + 1, true); continue; }
        int kind = -1; bool weak = false; const wchar_t* why = L"";
        if (ln.find(L"save") != std::wstring::npos) { kind = K_SAVE; why = L"Save folder"; }
        else if (InList(ln, kWeakSaveDirs)) { kind = K_SAVE; weak = true; why = L"Folder for worlds / profiles / player data"; }
        else if (ln.find(L"screenshot") != std::wstring::npos) { kind = K_SHOTS; why = L"Screenshots folder"; }
        else if (ln.find(L"replay") != std::wstring::npos || ln == L"demos") { kind = K_REPLAY; why = L"Replays / recordings folder"; }
        else if (InList(ln, kModDirs)) { kind = K_MODS; weak = true; why = L"Mods folder"; }
        if (kind < 0) { WalkGame(gi, cabs, crel, depth + 1, false); continue; }
        Stats st; WalkStats(cabs, st, 0);
        bool changed = G.hasTime && st.newest > G.lastUpd + GRACE;
        // weak names (world, profiles, mods...) are also used for the game's own data, so they only
        // count when something inside changed after Steam installed/updated the game
        if (st.files == 0 || (weak && G.hasTime && !changed)) {
            if (st.files) { g_pFiles -= st.files; WalkGame(gi, cabs, crel, depth + 1, false); }
            continue;
        }
        g_games[gi].size += st.size; g_games[gi].files += st.files;
        std::wstring w = why;
        if (changed) w += L" \x2014 has files changed after the last update";
        else if (!G.hasTime && weak) w += L" (might just be game data)";
        AddFind(gi, kind, cabs, crel, true, st.size, st.files, st.newest, w);
    }
}

static std::map<std::wstring, int> g_byApp;
static int GameForApp(const std::wstring& appid, const std::wstring& fallbackDir) {
    auto it = g_byApp.find(appid);
    if (it != g_byApp.end()) return it->second;
    Game g;
    g.appid = appid; g.name = L"App " + appid + L" (not installed)"; g.dir = fallbackDir; g.installed = false;
    g_games.push_back(g);
    g_byApp[appid] = (int)g_games.size() - 1;
    return (int)g_games.size() - 1;
}

static void ListDirs(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Proton (Linux / Steam Deck) keeps each game's Windows-style save folders inside steamapps\compatdata
static void ScanProton(const std::wstring& compat) {
    std::vector<std::wstring> apps; ListDirs(compat, apps);
    static const wchar_t* parents[] = { L"Documents", L"Documents\\My Games", L"Saved Games", L"AppData\\Local", L"AppData\\LocalLow", L"AppData\\Roaming" };
    static const wchar_t* const skip[] = { L"microsoft", L"temp", L"my games", L"openvr", L"d3dscache", L"nvidia", L"cef",
        L"wine", L"vkd3d-proton", L"crashdumps", L"unity", nullptr };
    for (auto& a : apps) {
        if (g_cancel) return;
        if (!AllDigits(a)) continue;
        std::wstring base = Join(compat, a + L"\\pfx\\drive_c\\users\\steamuser");
        if (!IsDir(base)) continue;
        for (auto* p : parents) {
            std::vector<std::wstring> kids; ListDirs(Join(base, p), kids);
            for (auto& k : kids) {
                if (InList(LowerW(k), skip)) continue;
                Stats st; WalkStats(Join(base, std::wstring(p) + L"\\" + k), st, 0);
                if (!st.files) continue;
                int gi = GameForApp(a, Join(compat, a));
                AddFind(gi, K_PROTON, Join(base, std::wstring(p) + L"\\" + k), L"Proton\\" + std::wstring(p) + L"\\" + k, true,
                        st.size, st.files, st.newest, L"Save data the game wrote while running through Proton (Linux / Steam Deck)");
            }
        }
    }
}

// Steam's own install folder: userdata\<account>\<appid> holds cloud-save copies and local game data
static void ScanUserdata(const std::wstring& ud) {
    std::vector<std::wstring> ids; ListDirs(ud, ids);
    for (auto& id : ids) {
        if (!AllDigits(id)) continue;
        std::vector<std::wstring> apps; ListDirs(Join(ud, id), apps);
        for (auto& a : apps) {
            if (g_cancel) return;
            if (!AllDigits(a) || a == L"7" || a == L"241100") continue;   // Steam client settings / controller configs
            std::wstring adir = Join(ud, id + L"\\" + a);
            if (a == L"760") {   // Steam screenshots, per game
                std::vector<std::wstring> sapps; ListDirs(Join(adir, L"remote"), sapps);
                for (auto& sa : sapps) {
                    if (!AllDigits(sa)) continue;
                    std::wstring sd = Join(adir, L"remote\\" + sa + L"\\screenshots");
                    Stats st; WalkStats(sd, st, 0);
                    if (!st.files) continue;
                    AddFind(GameForApp(sa, sd), K_SHOTS, sd, L"userdata\\" + id + L"\\760\\remote\\" + sa + L"\\screenshots", true,
                            st.size, st.files, st.newest, L"Screenshots you took with Steam (F12)");
                }
                continue;
            }
            Stats st; WalkStats(adir, st, 0);
            if (!st.files) continue;
            AddFind(GameForApp(a, adir), K_CLOUD, adir, L"userdata\\" + id + L"\\" + a, true, st.size, st.files, st.newest,
                    L"Steam's local copy of this game's cloud saves / settings \x2014 deleting the Steam folder removes it");
        }
    }
}

static void SetProgressName(const std::wstring& n) { EnterCriticalSection(&g_lock); g_pName = n; LeaveCriticalSection(&g_lock); }

static DWORD WINAPI ScanThread(LPVOID) {
    g_games.clear(); g_finds.clear(); g_byApp.clear();
    std::wstring root = g_root;
    std::wstring sa = Join(root, L"steamapps");
    if (g_steamLib) {
        // installed games from Steam's manifests (gives real names and the last-update time)
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Long(Join(sa, L"appmanifest_*.acf")).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring t = ReadText(Join(sa, fd.cFileName)), tl = LowerW(t);
                Game g;
                g.appid = VdfVal(t, tl, L"appid");
                g.name = VdfVal(t, tl, L"name");
                std::wstring inst = VdfVal(t, tl, L"installdir");
                std::wstring lu = VdfVal(t, tl, L"lastupdated");
                if (inst.empty()) continue;
                g.dir = Join(sa, L"common\\" + inst);
                if (!IsDir(g.dir)) continue;
                if (g.name.empty()) g.name = inst;
                long long unixT = _wtoi64(lu.c_str());
                if (unixT > 0) { g.hasTime = true; g.lastUpd = ((u64)unixT + 11644473600ull) * 10000000ull; }
                if (!g.appid.empty()) g_byApp[g.appid] = (int)g_games.size();
                g_games.push_back(g);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        // folders in common\ without a manifest (leftovers, manually copied games)
        std::set<std::wstring> known;
        for (auto& g : g_games) known.insert(LowerW(Leaf(g.dir)));
        std::vector<std::wstring> dirs; ListDirs(Join(sa, L"common"), dirs);
        for (auto& d : dirs) {
            if (known.count(LowerW(d))) continue;
            Game g; g.name = d; g.dir = Join(sa, L"common\\" + d);
            g_games.push_back(g);
        }
    } else {
        std::vector<std::wstring> dirs; ListDirs(root, dirs);
        for (auto& d : dirs) { Game g; g.name = d; g.dir = Join(root, d); g_games.push_back(g); }
        if (dirs.empty()) { Game g; g.name = Leaf(root); g.dir = root; g_games.push_back(g); }
    }
    g_pGames = (LONG)g_games.size();
    size_t n = g_games.size();
    for (size_t i = 0; i < n && !g_cancel; i++) {
        g_pGame = (LONG)i + 1;
        SetProgressName(g_games[i].name);
        g_changedCap = 0;
        WalkGame((int)i, g_games[i].dir, L"", 0, false);
    }
    if (g_steamLib && !g_cancel && IsDir(Join(sa, L"compatdata"))) { SetProgressName(L"Proton save folders"); ScanProton(Join(sa, L"compatdata")); }
    if (!g_cancel && IsDir(Join(root, L"userdata"))) { SetProgressName(L"Steam userdata"); ScanUserdata(Join(root, L"userdata")); }

    // finalize: severity per game, sort games (worst first), sort each game's findings
    for (auto& f : g_finds) {
        Game& g = g_games[f.game];
        g.cnt[f.kind]++;
        g.sev = std::max(g.sev, kKind[f.kind].sev);
    }
    std::vector<int> order(g_games.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [](int a, int b) {
        const Game &A = g_games[a], &B = g_games[b];
        if (A.sev != B.sev) return A.sev > B.sev;
        return lstrcmpiW(A.name.c_str(), B.name.c_str()) < 0;
    });
    std::vector<int> newIdx(order.size());
    std::vector<Game> sorted;
    for (size_t i = 0; i < order.size(); i++) { newIdx[order[i]] = (int)i; sorted.push_back(std::move(g_games[order[i]])); }
    g_games.swap(sorted);
    for (auto& f : g_finds) f.game = newIdx[f.game];
    for (size_t i = 0; i < g_finds.size(); i++) g_games[g_finds[i].game].finds.push_back((int)i);
    for (auto& g : g_games)
        std::sort(g.finds.begin(), g.finds.end(), [](int a, int b) {
            const Find &A = g_finds[a], &B = g_finds[b];
            if (kKind[A.kind].sev != kKind[B.kind].sev) return kKind[A.kind].sev > kKind[B.kind].sev;
            if (A.kind != B.kind) return A.kind < B.kind;
            return A.size > B.size;
        });
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void StartScan(const std::wstring& chosen) {
    if (g_state == ST_SCANNING || g_state == ST_COPYING) return;
    std::wstring p = chosen;
    wchar_t full[4096];
    if (GetFullPathNameW(p.c_str(), 4096, full, NULL)) p = full;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    g_rootShown = p;
    // accept the library root, its steamapps folder, or steamapps\common
    std::wstring lp = LowerW(Leaf(p)), pp = LowerW(Leaf(Parent(p)));
    if (IsDir(Join(p, L"steamapps"))) g_root = p;
    else if (lp == L"steamapps") g_root = Parent(p);
    else if (lp == L"common" && pp == L"steamapps") g_root = Parent(Parent(p));
    else g_root = p;
    if (g_root.size() == 2) g_root += L"\\";
    g_steamLib = IsDir(Join(g_root, L"steamapps"));
    g_cancel = 0; g_pFiles = 0; g_pGame = 0; g_pGames = 0;
    g_sel = -1; g_gscroll = 0; g_dscroll = 0;
    g_msg.clear();
    g_t0 = GetTickCount();
    InterlockedExchange(&g_state, ST_SCANNING);
    g_thread = CreateThread(NULL, 64u << 20, ScanThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    SetTimer(g_hwnd, TIMER_PROGRESS, 120, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------------------------------------------------------------- backup
static std::wstring SafeName(std::wstring s) {
    for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"_" : s;
}
static void MakeDirs(const std::wstring& p) {
    if (p.empty() || IsDir(p)) return;
    MakeDirs(Parent(p));
    CreateDirectoryW(Long(p).c_str(), NULL);
}
static void CopyTree(const std::wstring& src, const std::wstring& dst, int depth) {
    if (depth > 40 || g_cancel) return;
    MakeDirs(dst);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(src, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) CopyTree(Join(src, n), Join(dst, n), depth + 1);
        } else {
            if (CopyFileW(Long(Join(src, n)).c_str(), Long(Join(dst, n)).c_str(), FALSE)) g_copied++; else g_copyFail++;
            InterlockedExchangeAdd64(&g_cDone, (LONG64)(((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow));
        }
    } while (FindNextFileW(h, &fd) && !g_cancel);
    FindClose(h);
}
static DWORD WINAPI CopyThread(LPVOID) {
    for (auto& f : g_finds) {
        if (g_cancel) break;
        if (!kKind[f.kind].backup) continue;
        std::wstring dst = Join(g_backupDir, SafeName(g_games[f.game].name) + L"\\" + f.shown);
        SetProgressName(g_games[f.game].name);
        if (f.isDir) CopyTree(f.path, dst, 0);
        else {
            MakeDirs(Parent(dst));
            if (CopyFileW(Long(f.path).c_str(), Long(dst).c_str(), FALSE)) g_copied++; else g_copyFail++;
            InterlockedExchangeAdd64(&g_cDone, (LONG64)f.size);
        }
    }
    PostMessageW(g_hwnd, WM_APP_COPIED, 0, 0);
    return 0;
}

static bool PickFolder(const wchar_t* title, const std::wstring& start, std::wstring& out) {
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg))) return false;
    DWORD o = 0; dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    IShellItem* si = NULL;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), NULL, IID_IShellItem, (void**)&si))) { dlg->SetFolder(si); si->Release(); }
    bool ok = false;
    if (dlg->Show(g_hwnd) == S_OK) {
        IShellItem* res = NULL;
        if (SUCCEEDED(dlg->GetResult(&res))) {
            PWSTR p = NULL;
            if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); ok = true; }
            res->Release();
        }
    }
    dlg->Release();
    return ok;
}

static void StartBackup() {
    u64 total = 0; int items = 0;
    for (auto& f : g_finds) if (kKind[f.kind].backup) { total += f.size; items++; }
    if (!items) return;
    wchar_t desk[MAX_PATH] = L"";
    SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk);
    std::wstring dest;
    if (!PickFolder(L"Where should the backup go?", desk, dest)) return;
    // don't back up into the library you're about to delete
    std::wstring dl = LowerW(dest) + L"\\", rl = LowerW(g_root);
    if (rl.back() != L'\\') rl += L"\\";
    if (dl.compare(0, rl.size(), rl) == 0) {
        MessageBoxW(g_hwnd, L"That folder is inside the library you're checking \x2014 if you delete the library, the backup goes with it.\n\nPick a folder somewhere else.",
                    L"SaveFinder", MB_OK | MB_ICONWARNING);
        return;
    }
    ULARGE_INTEGER fa;
    if (GetDiskFreeSpaceExW(dest.c_str(), &fa, NULL, NULL) && fa.QuadPart < total + (64ull << 20)) {
        std::wstring m = L"Not enough free space there: the backup needs about " + SizeStr(total) + L".";
        MessageBoxW(g_hwnd, m.c_str(), L"SaveFinder", MB_OK | MB_ICONWARNING);
        return;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t nm[96];
    _snwprintf(nm, 96, L"Steam saves backup %04u-%02u-%02u %02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    nm[95] = 0;
    g_backupDir = Join(dest, nm);
    MakeDirs(g_backupDir);
    g_copied = 0; g_copyFail = 0; g_cDone = 0; g_cTotal = (LONG64)total; g_cancel = 0;
    g_msg.clear();
    InterlockedExchange(&g_state, ST_COPYING);
    g_thread = CreateThread(NULL, 0, CopyThread, NULL, 0, NULL);
    SetTimer(g_hwnd, TIMER_PROGRESS, 120, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------------------------------------------------------------- drawing
static void Layout(const RECT& cr) {
    int head = S(68), sum = S(96), foot = S(30), left = S(380);
    rcHead = { 0, 0, cr.right, head };
    rcSum = { S(16), head + S(14), cr.right - S(16), head + S(14) + sum };
    rcFoot = { 0, cr.bottom - foot, cr.right, cr.bottom };
    int top = rcSum.bottom + S(14);
    rcGames = { S(16), top, S(16) + left, rcFoot.top - S(14) };
    rcDetail = { rcGames.right + S(14), top, cr.right - S(16), rcFoot.top - S(14) };
    rcGBody = { rcGames.left + S(6), rcGames.top + S(40), rcGames.right - S(6), rcGames.bottom - S(6) };
    rcDBody = { rcDetail.left + S(6), rcDetail.top + S(70), rcDetail.right - S(6), rcDetail.bottom - S(6) };
}

static void Button(HDC dc, RECT r, const wchar_t* label, int id, bool primary, bool enabled = true, COLORREF pc = ACCENT) {
    bool hv = enabled && IsHover(id);
    COLORREF bg = primary ? (enabled ? (hv ? Mix(pc, RGB(255, 255, 255), 0.12) : pc) : PANEL3) : (hv ? PANEL3 : PANEL2);
    RoundC(dc, r, S(8), bg);
    if (!primary) RoundOutline(dc, r, S(8), hv ? RGB(70, 76, 88) : BORDER);
    Text(dc, label, r, g_fSemi, primary ? (enabled ? RGB(10, 14, 22) : TEXT3) : (enabled ? TEXT1 : TEXT3), TC);
    if (enabled) AddClick(r, id);
}

static void DrawLogo(HDC dc, int x, int y, int s) {
    // a little floppy disk
    RECT body = { x, y, x + s, y + s }; RoundC(dc, body, S(4), ACCENT);
    RECT lab = { x + s / 5, y + s / 2, x + s * 4 / 5, y + s - S(2) }; FillC(dc, lab, RGB(232, 240, 255));
    RECT sh = { x + s / 4, y + S(2), x + s * 3 / 4 - S(2), y + s * 3 / 10 }; FillC(dc, sh, RGB(30, 50, 90));
}

static void DrawHead(HDC dc) {
    FillC(dc, rcHead, PANEL);
    RECT line = { 0, rcHead.bottom - 1, rcHead.right, rcHead.bottom }; FillC(dc, line, BORDER);
    int cy = (rcHead.top + rcHead.bottom) / 2;
    DrawLogo(dc, S(20), cy - S(11), S(22));
    RECT t = { S(54), rcHead.top, S(200), rcHead.bottom };
    Text(dc, L"SaveFinder", t, g_fTitle, TEXT1, TL);
    bool busy = g_state == ST_SCANNING || g_state == ST_COPYING;
    int rx = rcHead.right - S(16);
    RECT b = { rx - S(130), cy - S(17), rx, cy + S(17) };
    Button(dc, b, L"Choose folder", ID_CHOOSE, false, !busy);
    int x = S(184);
    for (size_t i = 0; i < g_libs.size(); i++) {
        const Library& L = g_libs[i];
        std::wstring top = L.path;
        int w = std::max<int>(S(150), std::min<int>(S(260), TextW(dc, top.c_str(), g_fSemi) + S(28)));
        RECT r = { x, cy - S(21), x + w, cy + S(21) };
        if (r.right > b.left - S(12)) break;
        bool active = g_state != ST_IDLE && !LowerW(L.path).compare(LowerW(g_root));
        bool hv = !busy && IsHover(ID_LIB, (u32)i);
        RoundC(dc, r, S(8), active ? RGB(30, 44, 66) : (hv ? PANEL3 : PANEL2));
        RoundOutline(dc, r, S(8), active ? ACCENT : (hv ? RGB(70, 76, 88) : BORDER));
        RECT t1 = { r.left + S(12), r.top + S(3), r.right - S(10), r.top + S(22) };
        HGDIOBJ of = SelectObject(dc, g_fSemi); SetTextColor(dc, TEXT1);
        DrawTextW(dc, top.c_str(), -1, &t1, TLP | DT_NOPREFIX);
        SelectObject(dc, of);
        wchar_t sub[48]; _snwprintf(sub, 48, L"Steam library \x00B7 %d game%ls", L.games, L.games == 1 ? L"" : L"s"); sub[47] = 0;
        RECT t2 = { r.left + S(12), r.top + S(21), r.right - S(10), r.bottom - S(3) };
        Text(dc, sub, t2, g_fSmall, TEXT2, TL);
        if (!busy) AddClick(r, ID_LIB, (u32)i);
        x = r.right + S(8);
    }
}

static void Progress(HDC dc, RECT bar, double frac, COLORREF c) {
    RoundC(dc, bar, S(4), RGB(46, 51, 60));
    RECT f = bar; f.right = bar.left + std::max<LONG>(S(8), (LONG)((bar.right - bar.left) * std::min(1.0, frac)));
    RoundC(dc, f, S(4), c);
}

static void DrawSummary(HDC dc) {
    int hi = 0, mid = 0, lo = 0; int backupItems = 0; u64 backupBytes = 0;
    if (g_state == ST_DONE)
        for (auto& g : g_games) { if (g.sev == 2) hi++; else if (g.sev == 1) mid++; else if (g.sev == 0) lo++; }
    for (auto& f : g_finds) if (kKind[f.kind].backup) { backupItems++; backupBytes += f.size; }

    COLORREF c = ACCENT;
    std::wstring title, sub;
    if (g_state == ST_SCANNING) {
        wchar_t b[128]; _snwprintf(b, 128, L"Checking game %ld of %ld\x2026", g_pGame, g_pGames); b[127] = 0;
        title = b;
        EnterCriticalSection(&g_lock); sub = g_pName; LeaveCriticalSection(&g_lock);
        sub += L"   \x00B7   " + NumStr((u64)g_pFiles) + L" files looked at";
    } else if (g_state == ST_COPYING) {
        title = L"Backing up\x2026";
        EnterCriticalSection(&g_lock); sub = g_pName; LeaveCriticalSection(&g_lock);
        sub += L"   \x00B7   " + SizeStr((u64)g_cDone) + L" of " + SizeStr((u64)g_cTotal);
    } else if (g_state == ST_DONE) {
        if (hi) {
            c = BAD;
            title = L"Found saves or personal data in " + std::to_wstring(hi) + (hi == 1 ? L" game" : L" games");
            sub = L"Back them up before deleting this folder \x2014 they'd be gone with it.";
        } else if (mid) {
            c = WARN;
            title = L"No saves found \x2014 but " + std::to_wstring(mid) + (mid == 1 ? L" game has" : L" games have") + L" screenshots, replays or mods";
            sub = L"Check those below. Everything else looks like plain game files Steam can re-download.";
        } else {
            c = GOOD;
            title = L"No saves or worlds found in here";
            sub = lo ? L"Only some changed settings files turned up (listed below). Looks safe to delete."
                     : L"Everything here looks like game files Steam can re-download. Looks safe to delete.";
        }
    } else {
        title = L"Pick a Steam library to check";
        sub = g_libs.empty() ? L"Use Choose folder (or drop a folder on this window) \x2014 a library folder, its steamapps folder, or any folder of games."
                             : L"Click one of your libraries above, or use Choose folder for any other folder of games.";
    }
    RoundC(dc, rcSum, S(12), Mix(PANEL, c, 0.10));
    RoundOutline(dc, rcSum, S(12), Mix(BORDER, c, 0.35));
    RECT stripe = { rcSum.left, rcSum.top + S(14), rcSum.left + S(4), rcSum.bottom - S(14) }; FillC(dc, stripe, c);
    int L = rcSum.left + S(24);
    int R = rcSum.right - S(20);

    // right side: buttons
    if (g_state == ST_SCANNING || g_state == ST_COPYING) {
        RECT b = { R - S(110), rcSum.top + S(28), R, rcSum.top + S(64) };
        Button(dc, b, L"Cancel", ID_CANCEL, false);
        R = b.left - S(24);
    } else if (g_state == ST_DONE) {
        RECT b2 = { R - S(100), rcSum.top + S(28), R, rcSum.top + S(64) };
        Button(dc, b2, L"Rescan", ID_RESCAN, false);
        R = b2.left - S(10);
        if (backupItems) {
            std::wstring lab = L"Back up " + SizeStr(backupBytes);
            int w = TextW(dc, lab.c_str(), g_fSemi) + S(40);
            RECT b = { R - w, rcSum.top + S(28), R, rcSum.top + S(64) };
            Button(dc, b, lab.c_str(), ID_BACKUP, true, true, hi ? BAD : ACCENT);
            R = b.left - S(24);
        }
    }
    RECT tr = { L, rcSum.top + S(14), R, rcSum.top + S(48) };
    Text(dc, title.c_str(), tr, g_fBig, TEXT1, TL);
    RECT sr = { L, rcSum.top + S(46), R, rcSum.top + S(66) };
    Text(dc, sub.c_str(), sr, g_fUI, TEXT2, TL);
    if (g_state == ST_SCANNING || g_state == ST_COPYING) {
        double frac = g_state == ST_SCANNING ? (g_pGames ? (double)(g_pGame) / g_pGames : 0) : (g_cTotal ? (double)g_cDone / g_cTotal : 0);
        RECT bar = { L, rcSum.top + S(74), R, rcSum.top + S(80) };
        Progress(dc, bar, frac, ACCENT);
    } else {
        const wchar_t* note = !g_msg.empty() ? g_msg.c_str() :
            L"Most games keep saves in Documents, AppData or Steam Cloud instead \x2014 those aren't in this folder and won't be deleted with it.";
        RECT nr = { L, rcSum.top + S(66), R, rcSum.bottom - S(8) };
        Text(dc, note, nr, g_fSmall, !g_msg.empty() ? (g_msgKind ? WARN : GOOD) : TEXT3, TL);
    }
}

static std::wstring GameBadges(const Game& g) {
    if (g.sev < 0) return L"Nothing found";
    std::wstring s;
    for (int k = 0; k < NKIND; k++) {
        if (!g.cnt[k]) continue;
        if (!s.empty()) s += L"  \x00B7  ";
        if (k == K_SHOTS || k == K_MODS || k == K_CLOUD) s += kKind[k].name;
        else s += std::to_wstring(g.cnt[k]) + L" " + (g.cnt[k] == 1 ? LowerW(kKind[k].name) : std::wstring(kKind[k].plural));
    }
    return s;
}

static void ScrollBar(HDC dc, const RECT& body, int content, int scroll) {
    int view = body.bottom - body.top;
    if (content <= view) return;
    RECT track = { body.right - S(6), body.top + S(2), body.right - S(2), body.bottom - S(2) };
    int th = std::max<int>(S(30), (int)((long long)(track.bottom - track.top) * view / content));
    int ty = track.top + (int)((long long)(track.bottom - track.top - th) * scroll / (content - view));
    RECT t = { track.left, ty, track.right, ty + th };
    RoundC(dc, t, S(4), RGB(70, 76, 88));
}
static int Clamp(int v, int content, const RECT& body) { return std::max(0, std::min(v, std::max(0, content - (int)(body.bottom - body.top)))); }

static void DrawGames(HDC dc) {
    RoundC(dc, rcGames, S(12), PANEL);
    RoundOutline(dc, rcGames, S(12), BORDER);
    int hits = 0; for (auto& g : g_games) if (g.sev >= 0) hits++;
    std::wstring h = L"GAMES";
    if (g_state == ST_DONE) h += L"  \x00B7  " + std::to_wstring(g_games.size()) + L" checked";
    RECT hr = { rcGames.left + S(18), rcGames.top + S(6), rcGames.right - S(18), rcGames.top + S(36) };
    Text(dc, h.c_str(), hr, g_fSmallSemi, TEXT3, TL);
    if (g_state != ST_DONE) return;
    int rowH = S(50);
    g_gscroll = Clamp(g_gscroll, (int)g_games.size() * rowH, rcGBody);
    HRGN clip = CreateRectRgn(rcGBody.left, rcGBody.top, rcGBody.right, rcGBody.bottom);
    SelectClipRgn(dc, clip);
    for (int i = g_gscroll / rowH; i < (int)g_games.size(); i++) {
        int y = rcGBody.top + i * rowH - g_gscroll;
        if (y >= rcGBody.bottom) break;
        const Game& g = g_games[i];
        RECT row = { rcGBody.left, y + S(2), rcGBody.right - S(8), y + rowH - S(2) };
        bool sel = g_sel == i, hv = IsHover(ID_GAME, (u32)i);
        if (sel || hv) RoundC(dc, row, S(8), sel ? PANEL3 : PANEL2);
        COLORREF dc2 = g.sev >= 0 ? kSevColor[g.sev] : RGB(56, 62, 72);
        RECT dot = { row.left + S(12), y + rowH / 2 - S(5), row.left + S(22), y + rowH / 2 + S(5) };
        RoundC(dc, dot, S(10), dc2);
        RECT n = { row.left + S(34), y + S(6), row.right - S(80), y + S(26) };
        Text(dc, g.name.c_str(), n, g_fSemi, g.sev >= 0 ? TEXT1 : TEXT2, TL);
        RECT sz = { row.right - S(80), y + S(6), row.right - S(10), y + S(26) };
        if (g.size) Text(dc, SizeStr(g.size).c_str(), sz, g_fSmall, TEXT3, TR);
        RECT b = { row.left + S(34), y + S(25), row.right - S(10), y + S(44) };
        Text(dc, GameBadges(g).c_str(), b, g_fSmall, g.sev >= 0 ? Mix(TEXT2, dc2, 0.4) : TEXT3, TL);
        RECT hit = row;
        hit.top = std::max(hit.top, rcGBody.top); hit.bottom = std::min(hit.bottom, rcGBody.bottom);
        if (hit.bottom > hit.top) AddClick(hit, ID_GAME, (u32)i);
    }
    SelectClipRgn(dc, NULL); DeleteObject(clip);
    ScrollBar(dc, rcGBody, (int)g_games.size() * rowH, g_gscroll);
}

static void DrawDetail(HDC dc) {
    RoundC(dc, rcDetail, S(12), PANEL);
    RoundOutline(dc, rcDetail, S(12), BORDER);
    int L = rcDetail.left + S(20), R = rcDetail.right - S(20);
    if (g_state != ST_DONE || g_sel < 0 || g_sel >= (int)g_games.size()) {
        RECT t = rcDetail;
        const wchar_t* s = g_state == ST_DONE ? (g_games.empty() ? L"No games found in this folder" : L"Pick a game on the left to see what was found")
                                              : L"Results show up here";
        Text(dc, s, t, g_fUI, TEXT3, TC);
        return;
    }
    const Game& g = g_games[g_sel];
    RECT ob = { R - S(140), rcDetail.top + S(16), R, rcDetail.top + S(48) };
    Button(dc, ob, L"Open game folder", ID_OPENGAME, false, IsDir(g.dir));
    RECT n = { L, rcDetail.top + S(12), ob.left - S(16), rcDetail.top + S(38) };
    Text(dc, g.name.c_str(), n, g_fTitle, TEXT1, TL);
    std::wstring info = g.installed ? g.dir : L"Not installed in this library (data left behind)";
    if (g.hasTime) info += L"   \x00B7   last updated " + DateStr(g.lastUpd);
    RECT ir = { L, rcDetail.top + S(38), ob.left - S(16), rcDetail.top + S(58) };
    HGDIOBJ of = SelectObject(dc, g_fSmall); SetTextColor(dc, TEXT3);
    DrawTextW(dc, info.c_str(), -1, &ir, TLP | DT_NOPREFIX);
    SelectObject(dc, of);
    RECT hl = { rcDetail.left + 1, rcDetail.top + S(66), rcDetail.right - 1, rcDetail.top + S(67) }; FillC(dc, hl, BORDER);

    if (g.finds.empty()) {
        RECT t = rcDBody; t.bottom = (rcDBody.top + rcDBody.bottom) / 2;
        Text(dc, L"Nothing found", t, g_fHuge, GOOD, DT_SINGLELINE | DT_BOTTOM | DT_CENTER);
        RECT t2 = rcDBody; t2.top = t.bottom + S(8); t2.bottom = t2.top + S(40);
        Text(dc, g.hasTime ? L"No save files or folders, and nothing in here changed since Steam's last update."
                           : L"No save files or folders in here.",
             t2, g_fUI, TEXT2, DT_CENTER | DT_WORDBREAK);
        return;
    }
    int rowH = S(52);
    int content = (int)g.finds.size() * rowH;
    g_dscroll = Clamp(g_dscroll, content, rcDBody);
    HRGN clip = CreateRectRgn(rcDBody.left, rcDBody.top, rcDBody.right, rcDBody.bottom);
    SelectClipRgn(dc, clip);
    for (int i = g_dscroll / rowH; i < (int)g.finds.size(); i++) {
        int y = rcDBody.top + i * rowH - g_dscroll;
        if (y >= rcDBody.bottom) break;
        const Find& f = g_finds[g.finds[i]];
        const KindDef& K = kKind[f.kind];
        RECT row = { rcDBody.left, y + S(2), rcDBody.right - S(8), y + rowH - S(2) };
        bool hv = IsHover(ID_FIND, (u32)i);
        if (hv) RoundC(dc, row, S(8), PANEL2);
        int pw = TextW(dc, K.name, g_fSmallSemi) + S(20);
        RECT pill = { row.left + S(12), y + S(8), row.left + S(12) + pw, y + S(28) };
        RoundC(dc, pill, S(10), Mix(PANEL, K.color, 0.25));
        Text(dc, K.name, pill, g_fSmallSemi, Mix(TEXT1, K.color, 0.3), TC);
        int sx = row.left + S(132);
        std::wstring path = f.shown + (f.isDir ? L"\\" : L"");
        RECT pr = { std::max<int>(pill.right + S(12), sx), y + S(6), row.right - S(100), y + S(28) };
        HGDIOBJ o2 = SelectObject(dc, g_fSemi); SetTextColor(dc, TEXT1);
        DrawTextW(dc, path.c_str(), -1, &pr, TLP | DT_NOPREFIX);
        SelectObject(dc, o2);
        RECT szr = { row.right - S(100), y + S(6), row.right - S(12), y + S(28) };
        Text(dc, SizeStr(f.size).c_str(), szr, g_fSmall, TEXT2, TR);
        std::wstring sub = f.why;
        if (f.isDir) sub += L"   \x00B7   " + NumStr(f.files) + (f.files == 1 ? L" file" : L" files");
        if (f.mt) sub += L"   \x00B7   last changed " + DateStr(f.mt);
        RECT su = { pr.left, y + S(27), row.right - S(12), y + S(46) };
        Text(dc, sub.c_str(), su, g_fSmall, TEXT3, TL);
        RECT hit = row;
        hit.top = std::max(hit.top, rcDBody.top); hit.bottom = std::min(hit.bottom, rcDBody.bottom);
        if (hit.bottom > hit.top) AddClick(hit, ID_FIND, (u32)i);
    }
    SelectClipRgn(dc, NULL); DeleteObject(clip);
    ScrollBar(dc, rcDBody, content, g_dscroll);
}

static void DrawFoot(HDC dc) {
    FillC(dc, rcFoot, PANEL);
    RECT line = { 0, rcFoot.top, rcFoot.right, rcFoot.top + 1 }; FillC(dc, line, BORDER);
    std::wstring s;
    if (g_state == ST_DONE) {
        u64 files = 0, bytes = 0;
        for (auto& g : g_games) { files += g.files; bytes += g.size; }
        wchar_t t[64]; _snwprintf(t, 64, L"%.1fs", g_scanMs / 1000.0); t[63] = 0;
        s = L"Checked " + std::to_wstring(g_games.size()) + L" games  \x00B7  " + NumStr(files) + L" files  \x00B7  " + SizeStr(bytes) +
            L"  \x00B7  " + t + L"   \x2014   " + g_rootShown;
    } else s = L"Nothing is ever changed or deleted \x2014 SaveFinder only looks (and copies, if you back up).";
    RECT r = { S(16), rcFoot.top, rcFoot.right - S(300), rcFoot.bottom };
    HGDIOBJ of = SelectObject(dc, g_fSmall); SetTextColor(dc, TEXT3);
    DrawTextW(dc, s.c_str(), -1, &r, TLP | DT_NOPREFIX);
    SelectObject(dc, of);
    RECT r2 = { rcFoot.right - S(300), rcFoot.top, rcFoot.right - S(16), rcFoot.bottom };
    if (g_state == ST_DONE) Text(dc, L"Click a finding to show it in Explorer", r2, g_fSmall, TEXT3, TR);
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr; GetClientRect(hwnd, &cr);
    if (cr.right > 0 && cr.bottom > 0) {
        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP bb = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mdc, bb);
        SetBkMode(mdc, TRANSPARENT);
        FillC(mdc, cr, BG);
        Layout(cr);
        g_clicks.clear();
        DrawHead(mdc);
        DrawSummary(mdc);
        DrawGames(mdc);
        DrawDetail(mdc);
        DrawFoot(mdc);
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob); DeleteObject(bb); DeleteDC(mdc);
    }
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- input
static void ShowInExplorer(const std::wstring& path) {
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
}
static void UpdateHover(int mx, int my) {
    int id = -1; u32 data = 0;
    POINT p = { mx, my };
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--)
        if (PtInRect(&g_clicks[i].rc, p)) { id = g_clicks[i].id; data = g_clicks[i].data; break; }
    if (id != g_hoverId || data != g_hoverData) { g_hoverId = id; g_hoverData = data; InvalidateRect(g_hwnd, NULL, FALSE); }
}
static void OnClick(int mx, int my) {
    POINT p = { mx, my };
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--) {
        if (!PtInRect(&g_clicks[i].rc, p)) continue;
        Click c = g_clicks[i];
        switch (c.id) {
        case ID_CHOOSE: { std::wstring d; if (PickFolder(L"Choose a Steam library (or any folder of games)", g_rootShown, d)) StartScan(d); break; }
        case ID_LIB: if (c.data < g_libs.size()) StartScan(g_libs[c.data].path); break;
        case ID_RESCAN: StartScan(g_rootShown); break;
        case ID_CANCEL: InterlockedExchange(&g_cancel, 1); break;
        case ID_BACKUP: StartBackup(); break;
        case ID_OPENGAME:
            if (g_sel >= 0) ShellExecuteW(NULL, L"open", L"explorer.exe", (L"\"" + g_games[g_sel].dir + L"\"").c_str(), NULL, SW_SHOWNORMAL);
            break;
        case ID_GAME: g_sel = (int)c.data; g_dscroll = 0; break;
        case ID_FIND:
            if (g_sel >= 0 && c.data < g_games[g_sel].finds.size()) ShowInExplorer(g_finds[g_games[g_sel].finds[c.data]].path);
            break;
        }
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
}

static void MakeFonts() {
    auto mk = [](int px, int weight) {
        return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    g_fUI = mk(13, FW_NORMAL); g_fSemi = mk(13, FW_SEMIBOLD);
    g_fSmall = mk(12, FW_NORMAL); g_fSmallSemi = mk(12, FW_SEMIBOLD);
    g_fTitle = mk(18, FW_SEMIBOLD); g_fHuge = mk(26, FW_LIGHT); g_fBig = mk(20, FW_SEMIBOLD);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(940); mm->ptMinTrackSize.y = S(600); return 0; }
    case WM_MOUSEMOVE:
        if (!g_tracking) { TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme); g_tracking = true; }
        UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        g_tracking = false;
        if (g_hoverId != -1) { g_hoverId = -1; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, g_hoverId >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hwnd, &p);
        int d = GET_WHEEL_DELTA_WPARAM(wp) * S(50) * 3 / WHEEL_DELTA;
        if (PtInRect(&rcGames, p)) g_gscroll -= d; else if (PtInRect(&rcDetail, p)) g_dscroll -= d;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (g_state == ST_DONE && (wp == VK_DOWN || wp == VK_UP) && !g_games.empty()) {
            g_sel = std::max(0, std::min((int)g_games.size() - 1, g_sel + (wp == VK_DOWN ? 1 : -1)));
            int rowH = S(50), y = g_sel * rowH;
            if (y < g_gscroll) g_gscroll = y;
            if (y + rowH > g_gscroll + (rcGBody.bottom - rcGBody.top)) g_gscroll = y + rowH - (rcGBody.bottom - rcGBody.top);
            g_dscroll = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == VK_F5 && g_state == ST_DONE) StartScan(g_rootShown);
        return 0;
    case WM_TIMER: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_APP_DONE:
        KillTimer(hwnd, TIMER_PROGRESS);
        if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
        g_scanMs = GetTickCount() - g_t0;
        if (g_cancel) { g_games.clear(); g_finds.clear(); InterlockedExchange(&g_state, ST_IDLE); }
        else { InterlockedExchange(&g_state, ST_DONE); g_sel = g_games.empty() ? -1 : 0; }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_APP_COPIED: {
        KillTimer(hwnd, TIMER_PROGRESS);
        if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
        InterlockedExchange(&g_state, ST_DONE);
        wchar_t b[256];
        if (g_cancel) { _snwprintf(b, 256, L"Backup cancelled after %d files.", g_copied); g_msgKind = 1; }
        else if (g_copyFail) { _snwprintf(b, 256, L"Backed up %d files \x2014 %d couldn't be copied (in use?). Check the backup before deleting.", g_copied, g_copyFail); g_msgKind = 1; }
        else { _snwprintf(b, 256, L"Backed up %d files to \"%ls\".", g_copied, Leaf(g_backupDir).c_str()); g_msgKind = 0; }
        b[255] = 0;
        g_msg = b;
        if (!g_cancel) ShellExecuteW(NULL, L"open", L"explorer.exe", (L"\"" + g_backupDir + L"\"").c_str(), NULL, SW_SHOWNORMAL);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t p[4096];
        if (DragQueryFileW(drop, 0, p, 4096) && IsDir(p)) StartScan(p);
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        if (g_thread) { InterlockedExchange(&g_cancel, 1); WaitForSingleObject(g_thread, 5000); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    InitializeCriticalSection(&g_lock);
    HDC sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    MakeFonts();
    FindLibraries();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    wc.lpszClassName = L"SaveFinderWnd";
    RegisterClassExW(&wc);

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int W = std::min<int>(S(1240), (wa.right - wa.left) * 9 / 10);
    int H = std::min<int>(S(800), (wa.bottom - wa.top) * 9 / 10);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"SaveFinder", WS_OVERLAPPEDWINDOW,
                             wa.left + ((wa.right - wa.left) - W) / 2, wa.top + ((wa.bottom - wa.top) - H) / 2,
                             W, H, NULL, NULL, hi, NULL);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    // start right away if launched with a folder, or from inside a library
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring start;
    if (argv && argc > 1 && argv[1][0]) start = argv[1];
    if (argv) LocalFree(argv);
    if (start.empty()) {
        wchar_t exe[4096]; GetModuleFileNameW(NULL, exe, 4096);
        std::wstring d = Parent(exe), ld = LowerW(d);
        if (IsDir(Join(d, L"steamapps")) || LowerW(Leaf(d)) == L"steamapps" || ld.find(L"\\steamapps\\common") != std::wstring::npos) start = d;
        if (ld.size() > 16 && ld.compare(ld.size() - 16, 16, L"\\steamapps\\common") != 0 && ld.find(L"\\steamapps\\common\\") != std::wstring::npos)
            start = d.substr(0, ld.find(L"\\steamapps\\common\\") + 17);   // dropped inside a game folder: check the whole library
    }
    if (!start.empty() && IsDir(start)) StartScan(start);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return 0;
}
