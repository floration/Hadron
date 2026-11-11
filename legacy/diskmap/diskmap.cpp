// DiskMap - drive space visualizer (Win32 + GDI, no third-party libraries)
// Build: x86_64-w64-mingw32-g++ -O2 -municode -mwindows diskmap.cpp res.o -o DiskMap.exe
//        -static -lgdi32 -lmsimg32 -lshell32 -ldwmapi

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
#include <dwmapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cmath>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif

#define WM_APP_SCANDONE (WM_APP + 1)
#define TIMER_PROGRESS 1

// ---------------------------------------------------------------- categories
enum Cat { C_GAMES, C_PROGRAMS, C_WINDOWS, C_DRIVERS, C_VIDEO, C_AUDIO, C_IMAGES,
           C_DOCS, C_ARCHIVES, C_DEV, C_TEMP, C_OTHER, NCAT };
static const u8 NOCTX = 255;

static const wchar_t* kCatName[NCAT] = {
    L"Games", L"Programs", L"Windows", L"Drivers", L"Video", L"Audio", L"Images",
    L"Documents", L"Archives & disk images", L"Developer", L"Temp & cache", L"Other" };

static const COLORREF kCatColor[NCAT] = {
    RGB(236, 98, 98),   // games
    RGB(84, 146, 236),  // programs
    RGB(140, 152, 172), // windows
    RGB(172, 124, 236), // drivers
    RGB(238, 164, 66),  // video
    RGB(232, 100, 170), // audio
    RGB(72, 196, 164),  // images
    RGB(226, 208, 96),  // documents
    RGB(180, 136, 96),  // archives
    RGB(116, 204, 104), // developer
    RGB(98, 112, 134),  // temp
    RGB(120, 124, 134)  // other
};

// ---------------------------------------------------------------- safe-to-delete rules
enum { SF_NONE, SF_DUMPS, SF_TEMP, SF_UPDATE, SF_OLDWIN, SF_RECYCLE, SF_CACHE, SF_SHADER, SF_THUMB, SF_DRVPKG, NSAFE };
static const COLORREF SAFE_COL = RGB(80, 214, 140);
struct SafeInfo { const wchar_t* name; const wchar_t* desc; };
static const SafeInfo kSafe[NSAFE] = {
    { L"", L"" },
    { L"Crash & error reports", L"Memory dumps and error reports Windows saves after a crash or driver reset (LiveKernelReports, Minidump, WER). Only useful for debugging \x2014 glance at the file names first to see what crashed." },
    { L"Temporary files", L"Scratch files that apps and installers left behind. Anything still in use will refuse to delete \x2014 just skip those." },
    { L"Windows Update downloads", L"Update packages that are already installed, plus the Delivery Optimization cache. Easiest to clear with Disk Cleanup \x2192 Clean up system files." },
    { L"Previous Windows install", L"A backup of your previous Windows version. Removing it means you can't roll back. Use Disk Cleanup \x2192 Clean up system files \x2192 Previous Windows installation(s)." },
    { L"Recycle Bin", L"Files you already deleted. Empty the Recycle Bin to get this space back." },
    { L"App & browser caches", L"Browsers and apps rebuild these automatically. Close the app first; the next launch or page load may be a little slower." },
    { L"Shader caches", L"Compiled GPU shaders. Games and drivers rebuild them \x2014 the first launch afterwards may stutter briefly." },
    { L"Thumbnail & icon cache", L"Explorer's thumbnail and icon cache. Rebuilt automatically as you browse folders (Disk Cleanup can clear it too)." },
    { L"Driver installer leftovers", L"Files AMD/NVIDIA installers extracted during past driver installs. The installed driver doesn't use them." },
};
struct SafeRule { u8 reason; const wchar_t* pat; };
// paths are relative to the drive root, lowercase; '*' matches within a folder name, '**' spans folders
static const SafeRule kSafeDirRules[] = {
    { SF_DUMPS, L"windows\\livekernelreports" }, { SF_DUMPS, L"windows\\minidump" },
    { SF_DUMPS, L"programdata\\microsoft\\windows\\wer" }, { SF_DUMPS, L"users\\*\\appdata\\local\\crashdumps" },
    { SF_DUMPS, L"users\\*\\appdata\\local\\microsoft\\windows\\wer" },
    { SF_TEMP, L"windows\\temp" }, { SF_TEMP, L"users\\*\\appdata\\local\\temp" },
    { SF_UPDATE, L"windows\\softwaredistribution\\download" },
    { SF_UPDATE, L"windows\\**\\deliveryoptimization\\cache" },
    { SF_OLDWIN, L"windows.old" }, { SF_OLDWIN, L"$windows.~bt" }, { SF_OLDWIN, L"$windows.~ws" },
    { SF_RECYCLE, L"$recycle.bin" },
    { SF_SHADER, L"users\\*\\appdata\\local\\d3dscache" }, { SF_SHADER, L"users\\*\\appdata\\**\\dxcache" },
    { SF_SHADER, L"users\\*\\appdata\\**\\glcache" }, { SF_SHADER, L"**\\steamapps\\shadercache" },
    { SF_CACHE, L"users\\*\\appdata\\local\\microsoft\\windows\\inetcache" },
    { SF_CACHE, L"users\\*\\appdata\\**\\cache" }, { SF_CACHE, L"users\\*\\appdata\\**\\cache2" },
    { SF_CACHE, L"users\\*\\appdata\\**\\code cache" }, { SF_CACHE, L"users\\*\\appdata\\**\\gpucache" },
    { SF_DRVPKG, L"amd" }, { SF_DRVPKG, L"nvidia" },
    { SF_DRVPKG, L"programdata\\nvidia corporation\\downloader" },
};
static const SafeRule kSafeFileRules[] = {
    { SF_DUMPS, L"windows\\memory.dmp" },
    { SF_THUMB, L"users\\*\\appdata\\local\\microsoft\\windows\\explorer\\thumbcache_*.db" },
    { SF_THUMB, L"users\\*\\appdata\\local\\microsoft\\windows\\explorer\\iconcache_*.db" },
};
struct CompiledRule { u8 reason; std::vector<std::wstring> segs; };
static std::vector<CompiledRule> g_dirRules, g_fileRules;
static bool g_fileDepth[64];

static std::vector<std::wstring> SplitPat(const wchar_t* p) {
    std::vector<std::wstring> v; std::wstring cur;
    for (; *p; p++) { if (*p == L'\\') { v.push_back(cur); cur.clear(); } else cur += *p; }
    v.push_back(cur);
    return v;
}
static void CompileRules() {
    for (auto& r : kSafeDirRules) g_dirRules.push_back({ r.reason, SplitPat(r.pat) });
    for (auto& r : kSafeFileRules) {
        g_fileRules.push_back({ r.reason, SplitPat(r.pat) });
        size_t d = g_fileRules.back().segs.size() - 1;
        if (d < 64) g_fileDepth[d] = true;
    }
}
static bool Glob(const wchar_t* p, const wchar_t* s) {
    if (*p == 0) return *s == 0;
    if (*p == L'*') { for (;; s++) { if (Glob(p + 1, s)) return true; if (!*s) return false; } }
    return *s && *p == *s && Glob(p + 1, s + 1);
}
static bool MatchSegs(const std::vector<std::wstring>& p, size_t pi, const std::vector<std::wstring>& s, size_t si) {
    if (pi == p.size()) return si == s.size();
    if (p[pi] == L"**") {
        for (size_t k = si; k <= s.size(); k++) if (MatchSegs(p, pi + 1, s, k)) return true;
        return false;
    }
    if (si == s.size()) return false;
    return Glob(p[pi].c_str(), s[si].c_str()) && MatchSegs(p, pi + 1, s, si + 1);
}
static u8 DirSafe(const std::vector<std::wstring>& segs) {
    for (auto& r : g_dirRules) if (MatchSegs(r.segs, 0, segs, 0)) return r.reason;
    return SF_NONE;
}
static u8 FileSafe(std::vector<std::wstring>& segs, const wchar_t* lname) {
    if (segs.size() >= 64 || !g_fileDepth[segs.size()]) return SF_NONE;
    segs.push_back(lname);
    u8 res = SF_NONE;
    for (auto& r : g_fileRules) if (MatchSegs(r.segs, 0, segs, 0)) { res = r.reason; break; }
    segs.pop_back();
    return res;
}

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

// ---------------------------------------------------------------- data
struct FileE { u64 size; u32 name; u32 dir; u8 cat; u8 safe; };
struct DirE {
    u32 name = 0, parent = 0, fileBegin = 0, fileCount = 0;
    std::vector<u32> kids;
    u64 total = 0;
    u64 cat[NCAT] = {};
    u64 safeR[NSAFE] = {};
    u8 safe = 0;
    u32 nFiles = 0, nDirs = 0;
    u8 ctx = NOCTX, dom = C_OTHER;
};

static std::vector<FileE> g_files;
static std::vector<DirE> g_dirs;
static std::vector<wchar_t> g_names;

static u32 AddName(const wchar_t* s) {
    u32 o = (u32)g_names.size();
    g_names.insert(g_names.end(), s, s + wcslen(s) + 1);
    return o;
}
static inline const wchar_t* NameOf(u32 o) { return &g_names[o]; }

// ---------------------------------------------------------------- app state
enum { ST_IDLE, ST_SCANNING, ST_DONE };
static volatile LONG g_state = ST_IDLE;
static volatile LONG g_cancel = 0;
static volatile LONG64 g_scanFiles = 0, g_scanBytes = 0;
static CRITICAL_SECTION g_curLock;
static wchar_t g_curPath[1024];
static HANDLE g_thread = NULL;
static HWND g_hwnd;
static wchar_t g_scanLetter = 0;
static u64 g_scanDriveUsed = 0, g_scanDriveTotal = 0;
static DWORD g_scanStartTick = 0, g_scanMs = 0;

static u32 g_view = 0;
static int g_filter = -1;          // -1 none, 0..NCAT-1 category, FILTER_SAFE(+reason) safe-to-delete
static const int FILTER_SAFE = 100;
static int g_sideTab = 0;          // 0 categories, 1 safe to delete
static bool SafeMatch(u8 s) {
    if (!s) return false;
    return g_filter == FILTER_SAFE || g_filter == FILTER_SAFE + s;
}
static u64 SafeSum(const u64* r) { u64 t = 0; for (int i = 1; i < NSAFE; i++) t += r[i]; return t; }
static bool g_isAdmin = false;

struct Drive { wchar_t letter; wchar_t label[64]; u64 total, freeb; UINT type; };
static std::vector<Drive> g_drives;

struct Click { RECT rc; int id; u32 data; };
static std::vector<Click> g_clicks;
static int g_hoverClick = -1;   // 1 when hovering a clickable, else -1
static int g_hoverId = -1; static u32 g_hoverData = 0;

struct Hit { RECT r; u32 id; u32 top; u8 kind; u8 topKind; }; // kind 0 file, 1 dir, 2 lump
static std::vector<Hit> g_hits;
static int g_hoverHit = -1;
static RECT g_hoverTopRect = {0, 0, 0, 0};
static bool g_hoverTopValid = false;

struct Lump { u32 count; u64 size; u64 cat[NCAT]; u64 safeR[NSAFE]; u8 dom; };
static std::vector<Lump> g_lumps;

struct ListE { u32 id; u8 kind; u64 size; };  // kind 0 file, 1 folder
static std::vector<ListE> g_largest;

static int g_dpi = 96;
static inline int S(int v) { return MulDiv(v, g_dpi, 96); }
static HFONT g_fUI, g_fSemi, g_fSmall, g_fSmallSemi, g_fTitle, g_fHuge, g_fBigNum;

static RECT rcTool, rcSide, rcCrumb, rcMap, rcStatus;
static HDC g_mapDC = NULL;
static HBITMAP g_mapBmp = NULL, g_mapOld = NULL;
static int g_mapW = 0, g_mapH = 0;
static bool g_mapDirty = true;
static bool g_tracking = false;

enum {
    ID_RESCAN = 1, ID_CANCEL, ID_ADMIN, ID_UP,
    ID_DRIVE = 100, ID_CRUMB = 200, ID_CAT = 300, ID_FILE = 400, ID_CLEARFILTER = 500, ID_TAB = 600, ID_SAFE = 700
};

// ---------------------------------------------------------------- helpers
static void U64Str(u64 v, wchar_t* out) {  // with thousands separators
    wchar_t tmp[40]; int n = 0;
    if (v == 0) tmp[n++] = L'0';
    int digits = 0;
    while (v) {
        if (digits && digits % 3 == 0) tmp[n++] = L',';
        tmp[n++] = (wchar_t)(L'0' + v % 10); v /= 10; digits++;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static void FmtSize(u64 b, wchar_t* out, int n) {
    static const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    if (b < 1024) { wchar_t t[40]; U64Str(b, t); _snwprintf(out, n, L"%ls B", t); out[n - 1] = 0; return; }
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    _snwprintf(out, n, v < 10 ? L"%.2f %ls" : (v < 100 ? L"%.1f %ls" : L"%.0f %ls"), v, u[i]);
    out[n - 1] = 0;
}

static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t),
               (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
static COLORREF Lighten(COLORREF c, double t) { return Mix(c, RGB(255, 255, 255), t); }
static COLORREF Darken(COLORREF c, double t) { return Mix(c, RGB(0, 0, 0), t); }

static void FillC(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
static void RoundC(HDC dc, const RECT& r, int rad, COLORREF c) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, c); SetDCPenColor(dc, c);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
}
static void RoundOutline(HDC dc, const RECT& r, int rad, COLORREF c, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(pen);
}
static void Outline(HDC dc, RECT r, COLORREF c, int width) {
    for (int i = 0; i < width; i++) {
        SetDCBrushColor(dc, c);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
        InflateRect(&r, -1, -1);
    }
}
static void GradV(HDC dc, const RECT& r, COLORREF top, COLORREF bot) {
    TRIVERTEX v[2];
    v[0].x = r.left; v[0].y = r.top;
    v[0].Red = GetRValue(top) << 8; v[0].Green = GetGValue(top) << 8; v[0].Blue = GetBValue(top) << 8; v[0].Alpha = 0;
    v[1].x = r.right; v[1].y = r.bottom;
    v[1].Red = GetRValue(bot) << 8; v[1].Green = GetGValue(bot) << 8; v[1].Blue = GetBValue(bot) << 8; v[1].Alpha = 0;
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
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
static const UINT TR = DT_SINGLELINE | DT_VCENTER | DT_RIGHT;
static const UINT TC = DT_SINGLELINE | DT_VCENTER | DT_CENTER;

static void AddClick(const RECT& r, int id, u32 data = 0) { g_clicks.push_back({ r, id, data }); }
static bool IsHover(int id, u32 data = 0) {
    return g_hoverClick >= 0 && g_hoverId == id && g_hoverData == data;
}

static std::wstring DirPath(u32 d) {
    std::vector<u32> chain;
    for (u32 x = d; ; x = g_dirs[x].parent) { chain.push_back(x); if (x == 0) break; }
    std::wstring p;
    for (size_t i = chain.size(); i-- > 0;) {
        p += NameOf(g_dirs[chain[i]].name);
        if (i > 0 && chain[i] != 0) p += L"\\";
    }
    return p;
}
static std::wstring FilePath(u32 f) {
    std::wstring p = DirPath(g_files[f].dir);
    if (!p.empty() && p.back() != L'\\') p += L"\\";
    p += NameOf(g_files[f].name);
    return p;
}

// ---------------------------------------------------------------- classification
static bool In(const wchar_t* s, const wchar_t* const* list) {
    for (; *list; ++list) if (wcscmp(s, *list) == 0) return true;
    return false;
}
static void Lower(const wchar_t* s, wchar_t* out, size_t cap) {
    size_t n = wcslen(s); if (n >= cap) n = cap - 1;
    memcpy(out, s, n * sizeof(wchar_t)); out[n] = 0;
    CharLowerBuffW(out, (DWORD)n);
}

static const wchar_t* const kTempDirs[] = { L"temp", L"tmp", L"cache", L"caches", L"cache2", L"code cache",
    L"gpucache", L"shadercache", L"dxcache", L"glcache", L"inetcache", L"crashdumps", L"crashpad",
    L"$recycle.bin", L"d3dscache", L"nv_cache", L"webcache", L"cachestorage", L"service worker",
    L"softwaredistribution", L"package cache", nullptr };
static const wchar_t* const kGameDirs[] = { L"steamapps", L"steamlibrary", L"epic games", L"gog games",
    L"gog galaxy games", L"riot games", L"ea games", L"origin games", L"xboxgames", L"games", L"my games",
    L".minecraft", L"ubisoft game launcher", L"battlestate games", L"rockstar games", L"blizzard games",
    L"bethesda.net launcher", L"amazon games", L"itch", L"wargaming.net", L"square enix", nullptr };
static const wchar_t* const kRootWin[] = { L"windows", L"windows.old", L"system volume information",
    L"recovery", L"$windows.~bt", L"$windows.~ws", L"$winreagent", L"$sysreset", L"boot", L"efi",
    L"perflogs", L"$getcurrent", L"onedrivetemp", nullptr };
static const wchar_t* const kRootProg[] = { L"program files", L"program files (x86)", L"programdata", nullptr };
static const wchar_t* const kRootDrv[] = { L"amd", L"nvidia", L"intel", L"drivers", L"swsetup", L"dell",
    L"realtek", nullptr };
static const wchar_t* const kDevDirs[] = { L"node_modules", L".git", L"venv", L".venv", L"__pycache__",
    L".cargo", L".rustup", L".nuget", L".gradle", L".m2", L"site-packages", L".vscode", L".android",
    L"android", L"sdk", L"miniconda3", L"anaconda3", L".conda", L"source", L"repos", L"msys64",
    L"cygwin64", L".ollama", L"huggingface", L".cache", nullptr };

static u8 DirCtx(u8 pc, const wchar_t* n, int depth) {
    if (pc == C_TEMP) return C_TEMP;
    if (In(n, kTempDirs)) return C_TEMP;
    if (pc == C_DRIVERS) return C_DRIVERS;
    if (pc == C_WINDOWS) {
        if (!wcscmp(n, L"driverstore") || !wcscmp(n, L"drivers")) return C_DRIVERS;
        return C_WINDOWS;
    }
    if (pc == C_GAMES) return C_GAMES;
    if (In(n, kGameDirs)) return C_GAMES;
    if (depth == 1) {
        if (In(n, kRootWin)) return C_WINDOWS;
        if (In(n, kRootProg)) return C_PROGRAMS;
        if (In(n, kRootDrv)) return C_DRIVERS;
    }
    if (pc == C_PROGRAMS) return C_PROGRAMS;
    if (pc == C_DEV) return C_DEV;
    if (In(n, kDevDirs)) return C_DEV;
    if (!wcscmp(n, L"windowsapps") || !wcscmp(n, L"programs")) return C_PROGRAMS;
    return NOCTX;
}

static const wchar_t* const kVideo[] = { L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"m4v", L"flv",
    L"mpg", L"mpeg", L"ts", L"m2ts", L"3gp", L"vob", nullptr };
static const wchar_t* const kAudio[] = { L"mp3", L"flac", L"wav", L"ogg", L"m4a", L"aac", L"opus", L"wma",
    L"aiff", L"alac", L"mid", L"midi", nullptr };
static const wchar_t* const kImage[] = { L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"heif",
    L"tif", L"tiff", L"raw", L"cr2", L"cr3", L"nef", L"arw", L"dng", L"psd", L"svg", L"ico", L"avif",
    L"kra", L"xcf", nullptr };
static const wchar_t* const kDocs[] = { L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt",
    L"md", L"odt", L"ods", L"odp", L"rtf", L"csv", L"epub", L"mobi", L"pages", L"one", L"pst", L"ost",
    nullptr };
static const wchar_t* const kArch[] = { L"zip", L"rar", L"7z", L"tar", L"gz", L"tgz", L"bz2", L"xz",
    L"zst", L"iso", L"img", L"vhd", L"vhdx", L"vmdk", L"vdi", L"qcow2", L"wim", L"esd", L"cab", L"dmg",
    nullptr };
static const wchar_t* const kDev[] = { L"c", L"cpp", L"cc", L"h", L"hpp", L"py", L"js", L"ts", L"jsx",
    L"tsx", L"java", L"cs", L"go", L"rs", L"rb", L"php", L"lua", L"sln", L"vcxproj", L"pdb", L"obj",
    L"o", L"lib", L"a", L"ipynb", L"pt", L"pth", L"safetensors", L"gguf", L"ckpt", L"onnx", L"bin",
    L"whl", L"jar", nullptr };
static const wchar_t* const kProg[] = { L"exe", L"dll", L"msi", L"msix", L"appx", L"msp", L"ocx",
    nullptr };
static const wchar_t* const kDrv[] = { L"sys", L"inf", L"cat", L"drv", nullptr };
static const wchar_t* const kTmp[] = { L"tmp", L"temp", L"log", L"dmp", L"etl", L"bak", L"old",
    L"crdownload", L"part", nullptr };

static u8 FileCat(u8 ctx, const wchar_t* n, int depth) {
    if (ctx != NOCTX) return ctx;
    if (depth == 0) {
        if (!wcscmp(n, L"pagefile.sys") || !wcscmp(n, L"hiberfil.sys") || !wcscmp(n, L"swapfile.sys") ||
            !wcscmp(n, L"dumpstack.log") || !wcscmp(n, L"dumpstack.log.tmp") || !wcscmp(n, L"bootmgr"))
            return C_WINDOWS;
    }
    const wchar_t* dot = wcsrchr(n, L'.');
    if (!dot || !dot[1]) return C_OTHER;
    const wchar_t* e = dot + 1;
    if (In(e, kVideo)) return C_VIDEO;
    if (In(e, kAudio)) return C_AUDIO;
    if (In(e, kImage)) return C_IMAGES;
    if (In(e, kDocs)) return C_DOCS;
    if (In(e, kArch)) return C_ARCHIVES;
    if (In(e, kProg)) return C_PROGRAMS;
    if (In(e, kDrv)) return C_DRIVERS;
    if (In(e, kTmp)) return C_TEMP;
    if (In(e, kDev)) return C_DEV;
    return C_OTHER;
}

// ---------------------------------------------------------------- scanner
static void ScanDir(std::wstring& path, u32 di, int depth, std::vector<std::wstring>& segs) {
    if (g_cancel) return;
    size_t base = path.size();
    EnterCriticalSection(&g_curLock);
    const wchar_t* shown = path.c_str() + (path.compare(0, 4, L"\\\\?\\") == 0 ? 4 : 0);
    wcsncpy(g_curPath, shown, 1023); g_curPath[1023] = 0;
    LeaveCriticalSection(&g_curLock);

    path += L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(path.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    path.resize(base);
    u32 fb = (u32)g_files.size();
    std::vector<std::wstring> subs;
    u8 ctx = g_dirs[di].ctx;
    u8 dsafe = g_dirs[di].safe;
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t ln[MAX_PATH + 1];
        do {
            const wchar_t* fn = fd.cFileName;
            if (fn[0] == L'.' && (fn[1] == 0 || (fn[1] == L'.' && fn[2] == 0))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; // junctions/symlinks
                subs.push_back(fn);
            } else {
                // cloud placeholders (OneDrive "online-only") don't take up disk space
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE))
                    continue;
                FileE f;
                f.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                Lower(fn, ln, MAX_PATH + 1);
                f.cat = FileCat(ctx, ln, depth);
                f.safe = dsafe ? dsafe : FileSafe(segs, ln);
                f.name = AddName(fn);
                f.dir = di;
                g_files.push_back(f);
                InterlockedIncrement64(&g_scanFiles);
                InterlockedExchangeAdd64(&g_scanBytes, (LONG64)f.size);
            }
        } while (!g_cancel && FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_dirs[di].fileBegin = fb;
    g_dirs[di].fileCount = (u32)g_files.size() - fb;

    wchar_t ln[MAX_PATH + 1];
    for (auto& s : subs) {
        if (g_cancel) return;
        Lower(s.c_str(), ln, MAX_PATH + 1);
        DirE d;
        d.name = AddName(s.c_str());
        d.parent = di;
        d.ctx = DirCtx(ctx, ln, depth + 1);
        segs.push_back(ln);
        d.safe = dsafe ? dsafe : DirSafe(segs);
        u32 ni = (u32)g_dirs.size();
        g_dirs.push_back(std::move(d));
        g_dirs[di].kids.push_back(ni);
        path += s; path += L"\\";
        ScanDir(path, ni, depth + 1, segs);
        segs.pop_back();
        path.resize(base);
    }
}

static void Finalize() {
    size_t nd = g_dirs.size();
    for (size_t i = 0; i < nd; i++) {
        DirE& d = g_dirs[i];
        for (u32 f = d.fileBegin; f < d.fileBegin + d.fileCount; f++) {
            d.cat[g_files[f].cat] += g_files[f].size;
            d.safeR[g_files[f].safe] += g_files[f].size;
            d.total += g_files[f].size;
        }
        d.nFiles = d.fileCount;
        std::sort(g_files.begin() + d.fileBegin, g_files.begin() + d.fileBegin + d.fileCount,
                  [](const FileE& a, const FileE& b) { return a.size > b.size; });
    }
    // children always have larger indices than their parent (pre-order), so a reverse sweep works
    for (size_t i = nd; i-- > 1;) {
        DirE& d = g_dirs[i];
        DirE& p = g_dirs[d.parent];
        p.total += d.total;
        for (int c = 0; c < NCAT; c++) p.cat[c] += d.cat[c];
        for (int c = 1; c < NSAFE; c++) p.safeR[c] += d.safeR[c];
        p.nFiles += d.nFiles;
        p.nDirs += d.nDirs + 1;
    }
    for (size_t i = 0; i < nd; i++) {
        DirE& d = g_dirs[i];
        int best = C_OTHER; u64 bv = 0;
        for (int c = 0; c < NCAT; c++) if (d.cat[c] > bv) { bv = d.cat[c]; best = c; }
        d.dom = (u8)best;
        std::sort(d.kids.begin(), d.kids.end(), [](u32 a, u32 b) { return g_dirs[a].total > g_dirs[b].total; });
    }
}

static DWORD WINAPI ScanThread(LPVOID) {
    wchar_t root[4] = { g_scanLetter, L':', L'\\', 0 };
    DirE r;
    r.name = AddName(root);
    r.parent = 0;
    r.ctx = NOCTX;
    g_dirs.push_back(std::move(r));
    std::wstring path = L"\\\\?\\";
    path += root;
    std::vector<std::wstring> segs;
    ScanDir(path, 0, 0, segs);
    if (!g_cancel) Finalize();
    PostMessageW(g_hwnd, WM_APP_SCANDONE, 0, 0);
    return 0;
}

static void ClearData() {
    std::vector<FileE>().swap(g_files);
    std::vector<DirE>().swap(g_dirs);
    std::vector<wchar_t>().swap(g_names);
    g_hits.clear(); g_lumps.clear(); g_largest.clear();
    g_hoverHit = -1; g_hoverTopValid = false;
}

static void RefreshDrives() {
    g_drives.clear();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        Drive d = {};
        d.letter = root[0]; d.type = t;
        ULARGE_INTEGER fa, tot, fr;
        if (!GetDiskFreeSpaceExW(root, &fa, &tot, &fr)) continue;
        d.total = tot.QuadPart; d.freeb = fr.QuadPart;
        if (!GetVolumeInformationW(root, d.label, 64, NULL, NULL, NULL, NULL, 0) || !d.label[0])
            wcscpy(d.label, t == DRIVE_REMOVABLE ? L"Removable" : L"Local Disk");
        g_drives.push_back(d);
    }
}

static void ComputeLargest() {
    g_largest.clear();
    if (g_state != ST_DONE || g_dirs.empty()) return;
    const size_t K = 14;
    if (g_sideTab == 1) {
        // biggest "spots": the top-most flagged folders, plus individually flagged files
        std::vector<ListE> all;
        const DirE& v = g_dirs[g_view];
        if (v.safe) {
            if (SafeMatch(v.safe)) {
                for (u32 k : v.kids) if (g_dirs[k].total) all.push_back({ k, 1, g_dirs[k].total });
                for (u32 f = v.fileBegin; f < v.fileBegin + v.fileCount; f++)
                    if (g_files[f].size) all.push_back({ f, 0, g_files[f].size });
            }
        } else {
            std::vector<u32> st{ g_view };
            while (!st.empty()) {
                u32 d = st.back(); st.pop_back();
                const DirE& de = g_dirs[d];
                for (u32 f = de.fileBegin; f < de.fileBegin + de.fileCount; f++)
                    if (SafeMatch(g_files[f].safe) && g_files[f].size) all.push_back({ f, 0, g_files[f].size });
                for (u32 k : de.kids) {
                    const DirE& c = g_dirs[k];
                    if (c.safe) { if (SafeMatch(c.safe) && c.total) all.push_back({ k, 1, c.total }); }
                    else if (SafeSum(c.safeR)) st.push_back(k);
                }
            }
        }
        size_t n = std::min(K, all.size());
        std::partial_sort(all.begin(), all.begin() + n, all.end(), [](const ListE& a, const ListE& b) { return a.size > b.size; });
        all.resize(n);
        g_largest = all;
        return;
    }
    auto cmp = [](u32 a, u32 b) { return g_files[a].size > g_files[b].size; }; // min-heap on size
    std::vector<u32> heap;
    std::vector<u32> stack{ g_view };
    while (!stack.empty()) {
        u32 d = stack.back(); stack.pop_back();
        const DirE& de = g_dirs[d];
        for (u32 k = 0; k < de.fileCount && k < K; k++) {  // files are sorted desc
            u32 f = de.fileBegin + k;
            if (heap.size() < K) { heap.push_back(f); std::push_heap(heap.begin(), heap.end(), cmp); }
            else if (g_files[f].size > g_files[heap.front()].size) {
                std::pop_heap(heap.begin(), heap.end(), cmp); heap.back() = f;
                std::push_heap(heap.begin(), heap.end(), cmp);
            } else break;
        }
        for (u32 k : de.kids) stack.push_back(k);
    }
    std::sort(heap.begin(), heap.end(), cmp);
    for (u32 f : heap) g_largest.push_back({ f, 0, g_files[f].size });
}

static void SetView(u32 d) {
    if (g_state != ST_DONE || d >= g_dirs.size()) return;
    g_view = d;
    g_mapDirty = true;
    g_hoverHit = -1; g_hoverTopValid = false;
    ComputeLargest();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void StartScan(wchar_t letter) {
    if (g_thread) {
        InterlockedExchange(&g_cancel, 1);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread); g_thread = NULL;
        MSG m; while (PeekMessageW(&m, g_hwnd, WM_APP_SCANDONE, WM_APP_SCANDONE, PM_REMOVE)) {}
    }
    ClearData();
    g_scanLetter = letter;
    g_scanDriveUsed = 0; g_scanDriveTotal = 0;
    for (auto& d : g_drives) if (d.letter == letter) { g_scanDriveUsed = d.total - d.freeb; g_scanDriveTotal = d.total; }
    g_cancel = 0; g_scanFiles = 0; g_scanBytes = 0;
    g_curPath[0] = 0;
    g_view = 0; g_filter = g_sideTab == 1 ? FILTER_SAFE : -1;
    g_files.reserve(1 << 20);
    g_dirs.reserve(1 << 17);
    g_names.reserve(1 << 24);
    g_scanStartTick = GetTickCount();
    InterlockedExchange(&g_state, ST_SCANNING);
    g_thread = CreateThread(NULL, 256u << 20, ScanThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    SetTimer(g_hwnd, TIMER_PROGRESS, 100, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------------------------------------------------------------- treemap layout
struct Item { double size; u32 id; u8 kind; };
struct RF { double x, y, w, h; };

static void Squarify(const std::vector<Item>& it, double x, double y, double w, double h, std::vector<RF>& out) {
    out.assign(it.size(), RF{ 0, 0, 0, 0 });
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
            for (size_t i = start; i < end; i++) {
                double ih = it[i].size * scale / cw;
                out[i] = { x, cy, cw, ih }; cy += ih;
            }
            x += cw; w -= cw;
        } else {
            double rh = rowSum / w, cx = x;
            for (size_t i = start; i < end; i++) {
                double iw = it[i].size * scale / rh;
                out[i] = { cx, y, iw, rh }; cx += iw;
            }
            y += rh; h -= rh;
        }
        start = end;
    }
}

static bool FileMatch(const FileE& f) {
    if (g_filter < 0) return true;
    if (g_filter < NCAT) return f.cat == g_filter;
    return SafeMatch(f.safe);
}
static COLORREF FileColor(const FileE& f) {
    if (!FileMatch(f)) return Mix(kCatColor[f.cat], BG, 0.84);
    return g_filter >= FILTER_SAFE ? SAFE_COL : kCatColor[f.cat];
}
static COLORREF MixColor(const u64* cats, const u64* safeR, u64 total, u8 dom) {
    if (g_filter < 0) return kCatColor[dom];
    u64 m; COLORREF hc;
    if (g_filter < NCAT) { m = cats[g_filter]; hc = kCatColor[g_filter]; }
    else { m = g_filter == FILTER_SAFE ? SafeSum(safeR) : safeR[g_filter - FILTER_SAFE]; hc = SAFE_COL; }
    double f = total ? (double)m / (double)total : 0;
    return Mix(Mix(kCatColor[dom], BG, 0.84), hc, std::sqrt(f));
}

static void Leaf(HDC dc, RECT r, COLORREF c) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (w >= 3 && h >= 3) { r.right--; r.bottom--; w--; h--; }
    if (w * h >= 40) GradV(dc, r, Lighten(c, 0.14), Darken(c, 0.16));
    else FillC(dc, r, c);
}

static RECT ToRect(double x, double y, double w, double h) {
    RECT r = { (LONG)std::lround(x), (LONG)std::lround(y), (LONG)std::lround(x + w), (LONG)std::lround(y + h) };
    return r;
}

static void LeafLabel(HDC dc, RECT r, const wchar_t* name, u64 size, bool dim) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < S(70) || h < S(32) || dim) return;
    wchar_t sz[32]; FmtSize(size, sz, 32);
    RECT t = { r.left + S(6), r.top + S(4), r.right - S(6), r.top + S(4) + S(15) };
    Text(dc, name, t, g_fSmallSemi, RGB(16, 18, 22), TL);
    OffsetRect(&t, 0, S(14));
    if (t.bottom <= r.bottom - S(2)) Text(dc, sz, t, g_fSmall, RGB(36, 40, 48), TL);
}

static void DrawDir(HDC dc, u32 di, double x, double y, double w, double h, int depth, u32 top, u8 topKind) {
    RECT r = ToRect(x, y, w, h);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw <= 0 || rh <= 0) return;
    const DirE& d = g_dirs[di];
    if (depth > 0) g_hits.push_back({ r, di, top, 1, topKind });

    if (depth > 0 && (rw < 6 || rh < 6)) { Leaf(dc, r, MixColor(d.cat, d.safeR, d.total, d.dom)); return; }

    RECT in = r;
    if (depth > 0) {
        RECT fr = r; fr.right--; fr.bottom--;
        COLORREF frame = (g_filter >= FILTER_SAFE && SafeMatch(d.safe)) ? Mix(PANEL2, SAFE_COL, 0.28)
                       : Mix(PANEL2, kCatColor[d.dom], g_filter >= 0 ? 0.06 : 0.16);
        FillC(dc, fr, frame);
        int hdr = S(17);
        if (rw > S(64) && rh > S(40)) {
            wchar_t sz[32]; FmtSize(d.total, sz, 32);
            RECT t = { r.left + S(5), r.top, r.right - S(5), r.top + hdr };
            int sw = TextW(dc, sz, g_fSmall);
            RECT ts = t;
            if (rw > S(140)) { Text(dc, sz, ts, g_fSmall, TEXT3, TR); t.right -= sw + S(6); }
            Text(dc, NameOf(d.name), t, g_fSmallSemi, TEXT2, TL);
            in.top += hdr; in.left += S(2); in.right -= S(3); in.bottom -= S(3);
        } else {
            in.left += 1; in.top += 1; in.right -= 2; in.bottom -= 2;
        }
    }
    int iw = in.right - in.left, ih = in.bottom - in.top;
    if (iw < 3 || ih < 3 || d.total == 0) {
        if (d.total) Leaf(dc, in, MixColor(d.cat, d.safeR, d.total, d.dom));
        return;
    }

    // collect items; anything that would get a sliver of area is folded into one "small items" block
    double thr = (double)d.total / ((double)iw * ih) * 30.0;
    std::vector<Item> items;
    Lump lump = {}; bool haveLump = false;
    for (u32 k : d.kids) {
        const DirE& c = g_dirs[k];
        if (!c.total) continue;
        if ((double)c.total >= thr) items.push_back({ (double)c.total, k, 1 });
        else {
            haveLump = true; lump.count += c.nFiles; lump.size += c.total;
            for (int q = 0; q < NCAT; q++) lump.cat[q] += c.cat[q];
            for (int q = 1; q < NSAFE; q++) lump.safeR[q] += c.safeR[q];
        }
    }
    for (u32 f = d.fileBegin; f < d.fileBegin + d.fileCount; f++) {
        const FileE& fe = g_files[f];
        if (!fe.size) continue;
        if ((double)fe.size >= thr) items.push_back({ (double)fe.size, f, 0 });
        else { haveLump = true; lump.count++; lump.size += fe.size; lump.cat[fe.cat] += fe.size; lump.safeR[fe.safe] += fe.size; }
    }
    if (haveLump && lump.size) {
        int best = C_OTHER; u64 bv = 0;
        for (int q = 0; q < NCAT; q++) if (lump.cat[q] > bv) { bv = lump.cat[q]; best = q; }
        lump.dom = (u8)best;
        g_lumps.push_back(lump);
        items.push_back({ (double)lump.size, (u32)(g_lumps.size() - 1), 2 });
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.size > b.size; });

    std::vector<RF> rects;
    Squarify(items, in.left, in.top, iw, ih, rects);
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i]; const RF& rf = rects[i];
        u32 t = depth == 0 ? it.id : top;
        u8 tk = depth == 0 ? it.kind : topKind;
        if (it.kind == 1) {
            DrawDir(dc, it.id, rf.x, rf.y, rf.w, rf.h, depth + 1, t, tk);
        } else if (it.kind == 0) {
            RECT lr = ToRect(rf.x, rf.y, rf.w, rf.h);
            const FileE& fe = g_files[it.id];
            Leaf(dc, lr, FileColor(fe));
            LeafLabel(dc, lr, NameOf(fe.name), fe.size, !FileMatch(fe));
            g_hits.push_back({ lr, it.id, t, 0, tk });
        } else {
            RECT lr = ToRect(rf.x, rf.y, rf.w, rf.h);
            const Lump& L = g_lumps[it.id];
            Leaf(dc, lr, Mix(MixColor(L.cat, L.safeR, L.size, L.dom), PANEL2, 0.35));
            // at the top level the lump has no folder to drill into
            g_hits.push_back({ lr, it.id, depth == 0 ? di : top, 2, depth == 0 ? (u8)255 : topKind });
        }
    }
}

static void RenderMap() {
    int w = rcMap.right - rcMap.left, h = rcMap.bottom - rcMap.top;
    if (w <= 0 || h <= 0) return;
    if (!g_mapDC || w != g_mapW || h != g_mapH) {
        if (g_mapDC) { SelectObject(g_mapDC, g_mapOld); DeleteObject(g_mapBmp); DeleteDC(g_mapDC); }
        HDC sdc = GetDC(g_hwnd);
        g_mapDC = CreateCompatibleDC(sdc);
        g_mapBmp = CreateCompatibleBitmap(sdc, w, h);
        g_mapOld = (HBITMAP)SelectObject(g_mapDC, g_mapBmp);
        ReleaseDC(g_hwnd, sdc);
        SetBkMode(g_mapDC, TRANSPARENT);
        g_mapW = w; g_mapH = h;
    }
    RECT all = { 0, 0, w, h };
    FillC(g_mapDC, all, BG);
    g_hits.clear(); g_lumps.clear();
    g_hoverHit = -1; g_hoverTopValid = false;
    if (g_state == ST_DONE && !g_dirs.empty())
        DrawDir(g_mapDC, g_view, 0, 0, w, h, 0, 0, 0);
    g_mapDirty = false;
}

// ---------------------------------------------------------------- UI drawing
static void Layout(const RECT& cr) {
    int tb = S(60), sb = S(300), st = S(28), bc = S(40);
    rcTool = { 0, 0, cr.right, tb };
    rcSide = { 0, tb, sb, cr.bottom - st };
    rcStatus = { 0, cr.bottom - st, cr.right, cr.bottom };
    rcCrumb = { sb, tb, cr.right, tb + bc };
    rcMap = { sb + S(10), tb + bc, cr.right - S(12), cr.bottom - st - S(10) };
}

static void Button(HDC dc, RECT r, const wchar_t* label, int id, bool primary, bool enabled = true) {
    bool hv = enabled && IsHover(id);
    COLORREF bg = primary ? (hv ? Lighten(ACCENT, 0.12) : ACCENT) : (hv ? PANEL3 : PANEL2);
    RoundC(dc, r, S(8), bg);
    if (!primary) RoundOutline(dc, r, S(8), hv ? RGB(70, 76, 88) : BORDER);
    Text(dc, label, r, g_fSemi, primary ? RGB(10, 14, 22) : (enabled ? TEXT1 : TEXT3), TC);
    if (enabled) AddClick(r, id);
}

static void DrawLogo(HDC dc, int x, int y, int s) {
    int h = s / 2;
    RECT a = { x, y, x + h + S(3), y + s };            FillC(dc, a, kCatColor[C_GAMES]);
    RECT b = { a.right + 2, y, x + s, y + h };        FillC(dc, b, kCatColor[C_PROGRAMS]);
    RECT c = { a.right + 2, y + h + 2, x + s - S(5), y + s }; FillC(dc, c, kCatColor[C_IMAGES]);
    RECT d = { c.right + 2, y + h + 2, x + s, y + s }; FillC(dc, d, kCatColor[C_VIDEO]);
}

static void DrawToolbar(HDC dc) {
    FillC(dc, rcTool, PANEL);
    RECT line = { rcTool.left, rcTool.bottom - 1, rcTool.right, rcTool.bottom }; FillC(dc, line, BORDER);
    int cy = (rcTool.top + rcTool.bottom) / 2;
    DrawLogo(dc, S(18), cy - S(11), S(22));
    RECT t = { S(50), rcTool.top, S(170), rcTool.bottom };
    Text(dc, L"DiskMap", t, g_fTitle, TEXT1, TL);

    int x = S(160);
    for (size_t i = 0; i < g_drives.size(); i++) {
        const Drive& d = g_drives[i];
        RECT r = { x, cy - S(23), x + S(160), cy + S(23) };
        if (r.right > rcTool.right - S(260)) break;
        bool active = (g_state != ST_IDLE && d.letter == g_scanLetter);
        bool hv = IsHover(ID_DRIVE, (u32)i);
        RoundC(dc, r, S(8), active ? RGB(30, 44, 66) : (hv ? PANEL3 : PANEL2));
        RoundOutline(dc, r, S(8), active ? ACCENT : (hv ? RGB(70, 76, 88) : BORDER));
        wchar_t top[96]; _snwprintf(top, 96, L"%lc:  %ls", d.letter, d.label); top[95] = 0;
        RECT tr = { r.left + S(10), r.top + S(4), r.right - S(8), r.top + S(21) };
        Text(dc, top, tr, g_fSemi, TEXT1, TL);
        // mini usage bar
        RECT bar = { r.left + S(10), r.bottom - S(9), r.right - S(10), r.bottom - S(6) };
        FillC(dc, bar, RGB(52, 57, 67));
        double used = d.total ? (double)(d.total - d.freeb) / d.total : 0;
        RECT ub = bar; ub.right = bar.left + (LONG)((bar.right - bar.left) * used);
        FillC(dc, ub, used > 0.9 ? RGB(236, 98, 98) : ACCENT);
        wchar_t fs[32], line2[64]; FmtSize(d.freeb, fs, 32);
        _snwprintf(line2, 64, L"%ls free", fs); line2[63] = 0;
        RECT fr2 = { r.left + S(10), r.top + S(21), r.right - S(8), r.top + S(36) };
        Text(dc, line2, fr2, g_fSmall, TEXT2, TL);
        AddClick(r, ID_DRIVE, (u32)i);
        x = r.right + S(8);
    }

    int rx = rcTool.right - S(16);
    if (!g_isAdmin) {
        RECT r = { rx - S(132), cy - S(16), rx, cy + S(16) };
        Button(dc, r, L"Run as admin", ID_ADMIN, false);
        rx = r.left - S(8);
    }
    if (g_state == ST_SCANNING) {
        RECT r = { rx - S(96), cy - S(16), rx, cy + S(16) };
        Button(dc, r, L"Cancel", ID_CANCEL, false);
    } else if (g_state == ST_DONE) {
        RECT r = { rx - S(96), cy - S(16), rx, cy + S(16) };
        Button(dc, r, L"Rescan", ID_RESCAN, true);
    }
}

static void SectionLabel(HDC dc, const wchar_t* s, int& y) {
    RECT r = { rcSide.left + S(20), y, rcSide.right - S(20), y + S(18) };
    Text(dc, s, r, g_fSmallSemi, TEXT3, TL);
    y += S(24);
}

static void DrawSidebar(HDC dc) {
    FillC(dc, rcSide, PANEL);
    RECT line = { rcSide.right - 1, rcSide.top, rcSide.right, rcSide.bottom }; FillC(dc, line, BORDER);
    int L = rcSide.left + S(20), R = rcSide.right - S(20);
    int y = rcSide.top + S(18);

    if (g_state != ST_DONE) {
        SectionLabel(dc, L"CATEGORIES", y);
        for (int c = 0; c < NCAT; c++) {
            RECT sw = { L, y + S(6), L + S(10), y + S(16) };
            RoundC(dc, sw, S(4), kCatColor[c]);
            RECT tr = { L + S(20), y, R, y + S(22) };
            Text(dc, kCatName[c], tr, g_fUI, TEXT2, TL);
            y += S(26);
        }
        return;
    }

    const DirE& v = g_dirs[g_view];
    SectionLabel(dc, L"CURRENT VIEW", y);
    std::wstring vp = DirPath(g_view);
    RECT nr = { L, y, R, y + S(22) };
    Text(dc, g_view == 0 ? vp.c_str() : NameOf(v.name), nr, g_fSemi, TEXT1, TL);
    y += S(24);
    wchar_t big[32]; FmtSize(v.total, big, 32);
    RECT br = { L, y, R, y + S(34) };
    Text(dc, big, br, g_fBigNum, TEXT1, TL);
    y += S(36);
    wchar_t nf[40], nd[40], info[128];
    U64Str(v.nFiles, nf); U64Str(v.nDirs, nd);
    _snwprintf(info, 128, L"%ls files  \x00B7  %ls folders", nf, nd); info[127] = 0;
    RECT ir = { L, y, R, y + S(18) };
    Text(dc, info, ir, g_fSmall, TEXT2, TL);
    y += S(26);

    // drive breakdown bar (only meaningful at the root)
    if (g_view == 0 && g_scanDriveTotal && g_sideTab == 0) {
        RECT bar = { L, y, R, y + S(8) };
        RoundC(dc, bar, S(4), RGB(46, 51, 60));
        double W = bar.right - bar.left;
        u64 used = g_scanDriveUsed;
        u64 scanned = std::min<u64>(g_dirs[0].total, used);
        u64 hidden = used > scanned ? used - scanned : 0;
        RECT a = bar; a.right = bar.left + (LONG)(W * scanned / g_scanDriveTotal);
        RECT b = a; b.left = a.right; b.right = b.left + (LONG)(W * hidden / g_scanDriveTotal);
        if (a.right > a.left) FillC(dc, a, ACCENT);
        if (b.right > b.left) FillC(dc, b, RGB(110, 118, 136));
        y += S(14);
        wchar_t s1[32], s2[32], s3[32];
        FmtSize(scanned, s1, 32); FmtSize(hidden, s2, 32); FmtSize(g_scanDriveTotal - used, s3, 32);
        struct { COLORREF c; const wchar_t* n; wchar_t* v; } rows[] = {
            { ACCENT, L"Mapped", s1 }, { RGB(110, 118, 136), L"Not reachable (protected/system)", s2 },
            { RGB(46, 51, 60), L"Free", s3 } };
        for (auto& rw : rows) {
            RECT sw = { L, y + S(5), L + S(8), y + S(13) }; FillC(dc, sw, rw.c);
            RECT t1 = { L + S(14), y, R - S(70), y + S(18) }; Text(dc, rw.n, t1, g_fSmall, TEXT2, TL);
            RECT t2 = { R - S(70), y, R, y + S(18) }; Text(dc, rw.v, t2, g_fSmall, TEXT2, TR);
            y += S(18);
        }
        y += S(12);
    }

    // tabs
    {
        RECT seg = { L, y, R, y + S(32) };
        RoundC(dc, seg, S(9), PANEL2);
        int mid = (L + R) / 2;
        const wchar_t* lab[2] = { L"Categories", L"Safe to delete" };
        for (int t = 0; t < 2; t++) {
            RECT tr = { t == 0 ? L + S(3) : mid + S(1), y + S(3), t == 0 ? mid - S(1) : R - S(3), y + S(29) };
            bool sel = g_sideTab == t, hv = IsHover(ID_TAB, (u32)t);
            if (sel) RoundC(dc, tr, S(7), PANEL3);
            Text(dc, lab[t], tr, g_fSemi, (sel || hv) ? TEXT1 : TEXT2, TC);
            if (!sel) AddClick(tr, ID_TAB, (u32)t);
        }
        y += S(46);
    }

    if (g_sideTab == 0) {
        RECT hr = { L, y, R, y + S(18) };
        Text(dc, L"CATEGORIES", hr, g_fSmallSemi, TEXT3, TL);
        if (g_filter >= 0 && g_filter < NCAT) {
            RECT cr = { R - S(60), y - S(2), R, y + S(18) };
            Text(dc, L"Show all", cr, g_fSmallSemi, IsHover(ID_CLEARFILTER) ? TEXT1 : ACCENT, TR);
            AddClick(cr, ID_CLEARFILTER);
        }
        y += S(24);
        int order[NCAT]; for (int i = 0; i < NCAT; i++) order[i] = i;
        std::sort(order, order + NCAT, [&](int a, int b) { return v.cat[a] > v.cat[b]; });
        for (int oi = 0; oi < NCAT; oi++) {
            int c = order[oi];
            if (!v.cat[c]) continue;
            RECT row = { L - S(8), y, R + S(8), y + S(32) };
            bool sel = g_filter == c, hv = IsHover(ID_CAT, (u32)c);
            if (sel || hv) RoundC(dc, row, S(8), sel ? PANEL3 : PANEL2);
            bool dim = g_filter >= 0 && !sel;
            RECT sw = { L, y + S(8), L + S(10), y + S(18) };
            RoundC(dc, sw, S(4), dim ? Mix(kCatColor[c], PANEL, 0.6) : kCatColor[c]);
            wchar_t sz[32]; FmtSize(v.cat[c], sz, 32);
            RECT tr = { L + S(18), y + S(2), R - S(70), y + S(24) };
            Text(dc, kCatName[c], tr, sel ? g_fSemi : g_fUI, dim ? TEXT3 : TEXT1, TL);
            RECT sr = { R - S(80), y + S(2), R, y + S(24) };
            Text(dc, sz, sr, g_fUI, dim ? TEXT3 : TEXT2, TR);
            RECT track = { L + S(18), y + S(25), R, y + S(27) };
            FillC(dc, track, RGB(40, 44, 52));
            RECT fill = track;
            fill.right = track.left + (LONG)((track.right - track.left) * (v.total ? (double)v.cat[c] / v.total : 0));
            if (fill.right > fill.left) FillC(dc, fill, dim ? Mix(kCatColor[c], PANEL, 0.6) : kCatColor[c]);
            AddClick(row, ID_CAT, (u32)c);
            y += S(34);
        }
        y += S(12);
    } else {
        u64 tot = SafeSum(v.safeR);
        RECT hr = { L, y, R, y + S(18) };
        Text(dc, L"YOU COULD FREE UP", hr, g_fSmallSemi, TEXT3, TL);
        if (g_filter > FILTER_SAFE) {
            RECT cr = { R - S(60), y - S(2), R, y + S(18) };
            Text(dc, L"Show all", cr, g_fSmallSemi, IsHover(ID_CLEARFILTER) ? TEXT1 : ACCENT, TR);
            AddClick(cr, ID_CLEARFILTER);
        }
        y += S(20);
        wchar_t ts[32]; FmtSize(tot, ts, 32);
        RECT nr2 = { L, y, R, y + S(34) };
        Text(dc, tot ? ts : L"0 B", nr2, g_fBigNum, tot ? SAFE_COL : TEXT2, TL);
        y += S(42);
        int order[NSAFE]; int no = 0;
        for (int r = 1; r < NSAFE; r++) if (v.safeR[r]) order[no++] = r;
        for (int i = 1; i < no; i++) for (int j = i; j > 0 && v.safeR[order[j]] > v.safeR[order[j - 1]]; j--) std::swap(order[j], order[j - 1]);
        int hoverReason = -1;
        for (int oi = 0; oi < no; oi++) {
            int r = order[oi];
            RECT row = { L - S(8), y, R + S(8), y + S(30) };
            bool sel = g_filter == FILTER_SAFE + r, hv = IsHover(ID_SAFE, (u32)r);
            if (hv) hoverReason = r;
            if (sel || hv) RoundC(dc, row, S(8), sel ? PANEL3 : PANEL2);
            bool dim = g_filter > FILTER_SAFE && !sel;
            COLORREF col = dim ? Mix(SAFE_COL, PANEL, 0.6) : SAFE_COL;
            RECT sw = { L, y + S(7), L + S(10), y + S(17) };
            RoundC(dc, sw, S(4), col);
            wchar_t sz[32]; FmtSize(v.safeR[r], sz, 32);
            RECT tr = { L + S(18), y + S(1), R - S(70), y + S(23) };
            Text(dc, kSafe[r].name, tr, sel ? g_fSemi : g_fUI, dim ? TEXT3 : TEXT1, TL);
            RECT sr = { R - S(80), y + S(1), R, y + S(23) };
            Text(dc, sz, sr, g_fUI, dim ? TEXT3 : TEXT2, TR);
            RECT track = { L + S(18), y + S(23), R, y + S(25) };
            FillC(dc, track, RGB(40, 44, 52));
            RECT fill = track;
            fill.right = track.left + (LONG)((track.right - track.left) * (tot ? (double)v.safeR[r] / tot : 0));
            if (fill.right > fill.left) FillC(dc, fill, col);
            AddClick(row, ID_SAFE, (u32)r);
            y += S(32);
        }
        // explanation for the hovered / selected group
        int dr = hoverReason > 0 ? hoverReason : (g_filter > FILTER_SAFE ? g_filter - FILTER_SAFE : 0);
        std::wstring desc;
        if (dr > 0) desc = kSafe[dr].desc;
        else if (tot) desc = L"Green = things Windows or apps rebuild on their own, or only keep for troubleshooting. Hover a group for details. DiskMap never deletes anything itself.";
        else desc = L"Nothing in this folder matched the safe-to-delete rules.";
        if (!g_isAdmin && dr == 0) desc += L"\n\nRun as admin to find more \x2014 some of these folders are admin-only.";
        RECT box = { L + S(12), y + S(8), R - S(10), y + S(8) };
        HGDIOBJ of = SelectObject(dc, g_fSmall);
        DrawTextW(dc, desc.c_str(), -1, &box, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, of);
        box.right = R - S(10);
        RECT bg = { L, y, R, box.bottom + S(8) };
        RoundC(dc, bg, S(8), Mix(PANEL2, SAFE_COL, 0.06));
        RECT stripe = { L, y + S(6), L + S(3), bg.bottom - S(6) };
        FillC(dc, stripe, SAFE_COL);
        Text(dc, desc.c_str(), box, g_fSmall, TEXT2, DT_WORDBREAK);
        y = bg.bottom + S(18);
    }

    // largest files / biggest spots
    if (!g_largest.empty() && y < rcSide.bottom - S(80)) {
        SectionLabel(dc, g_sideTab == 1 ? L"BIGGEST SPOTS TO CLEAN" : L"LARGEST FILES", y);
        for (size_t i = 0; i < g_largest.size(); i++) {
            if (y + S(36) > rcSide.bottom - S(8)) break;
            const ListE& e = g_largest[i];
            const wchar_t* nm; std::wstring where; COLORREF col;
            if (e.kind == 0) {
                const FileE& f = g_files[e.id];
                nm = NameOf(f.name); where = DirPath(f.dir);
                col = g_sideTab == 1 ? SAFE_COL : kCatColor[f.cat];
            } else {
                const DirE& d = g_dirs[e.id];
                nm = NameOf(d.name); where = DirPath(d.parent);
                col = SAFE_COL;
            }
            RECT row = { L - S(8), y, R + S(8), y + S(34) };
            if (IsHover(ID_FILE, (u32)i)) RoundC(dc, row, S(8), PANEL2);
            RECT sw = { L, y + S(7), L + S(4), y + S(27) };
            FillC(dc, sw, col);
            wchar_t sz[32]; FmtSize(e.size, sz, 32);
            int szw = TextW(dc, sz, g_fSmall);
            std::wstring label = nm;
            if (e.kind == 1) label += L"\\";
            RECT t1 = { L + S(12), y + S(2), R - szw - S(8), y + S(18) };
            Text(dc, label.c_str(), t1, e.kind == 1 ? g_fSemi : g_fUI, TEXT1, TL);
            RECT t2 = { R - szw - S(4), y + S(2), R, y + S(18) };
            Text(dc, sz, t2, g_fSmall, TEXT2, TR);
            RECT t3 = { L + S(12), y + S(17), R, y + S(32) };
            HGDIOBJ of = SelectObject(dc, g_fSmall); SetTextColor(dc, TEXT3);
            DrawTextW(dc, where.c_str(), -1, &t3, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, of);
            AddClick(row, ID_FILE, (u32)i);
            y += S(36);
        }
    }
}

static void DrawCrumb(HDC dc) {
    if (g_state != ST_DONE) return;
    int cy = (rcCrumb.top + rcCrumb.bottom) / 2;
    int x = rcMap.left;
    RECT up = { x, cy - S(14), x + S(64), cy + S(14) };
    Button(dc, up, L"\x2190  Up", ID_UP, false, g_view != 0);
    x = up.right + S(14);
    std::vector<u32> chain;
    for (u32 d = g_view; ; d = g_dirs[d].parent) { chain.push_back(d); if (d == 0) break; }
    std::reverse(chain.begin(), chain.end());
    // if too long, drop from the left (keep root)
    int sep = TextW(dc, L"  \x203A  ", g_fUI);
    size_t first = 1;
    for (;;) {
        int tw = TextW(dc, NameOf(g_dirs[chain[0]].name), g_fUI) + S(12);
        if (first > 1) tw += TextW(dc, L"\x2026", g_fUI) + sep;
        for (size_t i = first; i < chain.size(); i++) tw += sep + TextW(dc, NameOf(g_dirs[chain[i]].name), g_fSemi) + S(12);
        if (x + tw < rcCrumb.right - S(20) || first + 1 >= chain.size()) break;
        first++;
    }
    for (size_t i = 0; i < chain.size(); i++) {
        if (i > 0 && i < first) continue;
        if (i > 0) {
            const wchar_t* sp = (i == first && first > 1) ? L"  \x203A  \x2026  \x203A  " : L"  \x203A  ";
            int w = TextW(dc, sp, g_fUI);
            RECT r = { x, rcCrumb.top, x + w, rcCrumb.bottom };
            Text(dc, sp, r, g_fUI, TEXT3, TL);
            x += w;
        }
        bool last = i + 1 == chain.size();
        const wchar_t* nm = NameOf(g_dirs[chain[i]].name);
        HFONT f = last ? g_fSemi : g_fUI;
        int w = TextW(dc, nm, f) + S(12);
        RECT r = { x, cy - S(13), std::min<int>(x + w, rcCrumb.right - S(12)), cy + S(13) };
        bool hv = !last && IsHover(ID_CRUMB, chain[i]);
        if (hv) RoundC(dc, r, S(6), PANEL2);
        RECT tr = r; tr.left += S(6);
        Text(dc, nm, tr, f, last ? TEXT1 : (hv ? TEXT1 : TEXT2), TL);
        if (!last) AddClick(r, ID_CRUMB, chain[i]);
        x = r.right;
    }
}

static void DrawProgress(HDC dc) {
    int cx = (rcMap.left + rcMap.right) / 2, cy = (rcMap.top + rcMap.bottom) / 2;
    int W = std::min<int>(S(560), rcMap.right - rcMap.left - S(40));
    RECT card = { cx - W / 2, cy - S(110), cx + W / 2, cy + S(110) };
    RoundC(dc, card, S(14), PANEL);
    RoundOutline(dc, card, S(14), BORDER);
    int L = card.left + S(28), R = card.right - S(28), y = card.top + S(24);
    wchar_t title[64]; _snwprintf(title, 64, L"Mapping %lc:\\", g_scanLetter); title[63] = 0;
    RECT t = { L, y, R, y + S(30) }; Text(dc, title, t, g_fTitle, TEXT1, TL);
    y += S(40);
    u64 bytes = (u64)g_scanBytes, files = (u64)g_scanFiles;
    double frac = g_scanDriveUsed ? std::min(0.99, (double)bytes / g_scanDriveUsed) : 0;
    RECT bar = { L, y, R, y + S(8) };
    RoundC(dc, bar, S(4), RGB(46, 51, 60));
    RECT fb = bar; fb.right = bar.left + std::max<LONG>(S(8), (LONG)((bar.right - bar.left) * frac));
    RoundC(dc, fb, S(4), ACCENT);
    y += S(22);
    wchar_t fs[40], bs[32], pc[16], line[160];
    U64Str(files, fs); FmtSize(bytes, bs, 32);
    _snwprintf(pc, 16, L"%d%%", (int)(frac * 100)); pc[15] = 0;
    _snwprintf(line, 160, L"%ls files  \x00B7  %ls", fs, bs); line[159] = 0;
    RECT l1 = { L, y, R - S(60), y + S(22) }; Text(dc, line, l1, g_fUI, TEXT1, TL);
    RECT l2 = { R - S(60), y, R, y + S(22) }; Text(dc, pc, l2, g_fSemi, ACCENT, TR);
    y += S(32);
    wchar_t cur[1024];
    EnterCriticalSection(&g_curLock); wcscpy(cur, g_curPath); LeaveCriticalSection(&g_curLock);
    RECT l3 = { L, y, R, y + S(20) };
    HGDIOBJ of = SelectObject(dc, g_fSmall); SetTextColor(dc, TEXT3);
    DrawTextW(dc, cur, -1, &l3, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    y += S(30);
    DWORD secs = (GetTickCount() - g_scanStartTick) / 1000;
    wchar_t el[64]; _snwprintf(el, 64, L"Elapsed %u:%02u", secs / 60, secs % 60); el[63] = 0;
    RECT l4 = { L, y, R, y + S(18) }; Text(dc, el, l4, g_fSmall, TEXT2, TL);
}

static void DrawEmpty(HDC dc) {
    int cx = (rcMap.left + rcMap.right) / 2, cy = (rcMap.top + rcMap.bottom) / 2;
    DrawLogo(dc, cx - S(24), cy - S(110), S(48));
    RECT t = { rcMap.left, cy - S(46), rcMap.right, cy - S(6) };
    Text(dc, L"Pick a drive to map", t, g_fHuge, TEXT1, TC);
    RECT t2 = { rcMap.left, cy, rcMap.right, cy + S(24) };
    Text(dc, L"Every file becomes a block sized by how much space it uses, colored by what it is.", t2, g_fUI, TEXT2, TC);
    if (!g_isAdmin) {
        RECT t3 = { rcMap.left, cy + S(28), rcMap.right, cy + S(50) };
        Text(dc, L"Tip: Run as admin to include protected Windows and user folders.", t3, g_fSmall, TEXT3, TC);
    }
    int x = cx - (int)g_drives.size() * S(84) / 2;
    for (size_t i = 0; i < g_drives.size() && i < 8; i++) {
        RECT r = { x, cy + S(70), x + S(76), cy + S(120) };
        bool hv = IsHover(ID_DRIVE, (u32)i);
        RoundC(dc, r, S(10), hv ? PANEL3 : PANEL2);
        RoundOutline(dc, r, S(10), hv ? ACCENT : BORDER);
        wchar_t s[8]; _snwprintf(s, 8, L"%lc:", g_drives[i].letter); s[7] = 0;
        Text(dc, s, r, g_fTitle, TEXT1, TC);
        AddClick(r, ID_DRIVE, (u32)i);
        x += S(84);
    }
}

static void HoverText(wchar_t* out, int n) {
    out[0] = 0;
    if (g_hoverHit < 0 || g_hoverHit >= (int)g_hits.size()) return;
    const Hit& h = g_hits[g_hoverHit];
    wchar_t sz[32], extra[160] = L"";
    auto safeInside = [&](const u64* r) {
        u64 t = SafeSum(r);
        if (t) { wchar_t s2[32]; FmtSize(t, s2, 32); _snwprintf(extra, 160, L"   \x00B7   %ls safe to delete inside", s2); extra[159] = 0; }
    };
    if (h.kind == 0) {
        const FileE& f = g_files[h.id];
        FmtSize(f.size, sz, 32);
        if (f.safe) { _snwprintf(extra, 160, L"   \x00B7   Safe to delete: %ls", kSafe[f.safe].name); extra[159] = 0; }
        std::wstring p = FilePath(h.id);
        _snwprintf(out, n, L"%ls   \x00B7   %ls   \x00B7   %ls%ls", p.c_str(), sz, kCatName[f.cat], extra);
    } else if (h.kind == 1) {
        const DirE& d = g_dirs[h.id];
        FmtSize(d.total, sz, 32);
        if (d.safe) { _snwprintf(extra, 160, L"   \x00B7   Safe to delete: %ls", kSafe[d.safe].name); extra[159] = 0; }
        else safeInside(d.safeR);
        wchar_t nf[40]; U64Str(d.nFiles, nf);
        std::wstring p = DirPath(h.id);
        _snwprintf(out, n, L"%ls   \x00B7   %ls   \x00B7   %ls files   \x00B7   mostly %ls%ls", p.c_str(), sz, nf, kCatName[d.dom], extra);
    } else {
        const Lump& L = g_lumps[h.id];
        FmtSize(L.size, sz, 32);
        safeInside(L.safeR);
        wchar_t nf[40]; U64Str(L.count, nf);
        _snwprintf(out, n, L"%ls small files grouped together   \x00B7   %ls   \x00B7   mostly %ls%ls", nf, sz, kCatName[L.dom], extra);
    }
    out[n - 1] = 0;
}

static void DrawStatus(HDC dc) {
    FillC(dc, rcStatus, PANEL);
    RECT line = { rcStatus.left, rcStatus.top, rcStatus.right, rcStatus.top + 1 }; FillC(dc, line, BORDER);
    const wchar_t* hint = L"Click: open folder   Right-click / Backspace: up   Ctrl+click: show in Explorer";
    int hw = TextW(dc, hint, g_fSmall);
    RECT hr = { rcStatus.right - hw - S(16), rcStatus.top, rcStatus.right - S(16), rcStatus.bottom };
    wchar_t buf[2048];
    if (g_state == ST_DONE) {
        Text(dc, hint, hr, g_fSmall, TEXT3, TR);
        HoverText(buf, 2048);
        if (!buf[0]) {
            wchar_t a[32], f[40]; FmtSize(g_dirs[0].total, a, 32); U64Str(g_dirs[0].nFiles, f);
            wchar_t sf[32]; FmtSize(SafeSum(g_dirs[0].safeR), sf, 32);
            _snwprintf(buf, 2048, L"Mapped %ls in %ls files  \x00B7  %ls safe to delete  \x00B7  scan took %.1fs", a, f, sf, g_scanMs / 1000.0);
            buf[2047] = 0;
        }
        RECT r = { rcStatus.left + S(16), rcStatus.top, hr.left - S(16), rcStatus.bottom };
        HGDIOBJ of = SelectObject(dc, g_fSmall); SetTextColor(dc, TEXT2);
        DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    } else {
        RECT r = { rcStatus.left + S(16), rcStatus.top, rcStatus.right - S(16), rcStatus.bottom };
        Text(dc, g_isAdmin ? L"Running as administrator" : L"Running as standard user \x2014 some system folders can't be read",
             r, g_fSmall, TEXT3, TL);
    }
}

static void DrawMapArea(HDC dc) {
    if (g_state == ST_SCANNING) { DrawProgress(dc); return; }
    if (g_state == ST_IDLE) { DrawEmpty(dc); return; }
    if (g_mapDirty || !g_mapDC || g_mapW != rcMap.right - rcMap.left || g_mapH != rcMap.bottom - rcMap.top) RenderMap();
    BitBlt(dc, rcMap.left, rcMap.top, g_mapW, g_mapH, g_mapDC, 0, 0, SRCCOPY);
    if (g_hoverHit >= 0 && g_hoverHit < (int)g_hits.size()) {
        if (g_hoverTopValid) {
            RECT t = g_hoverTopRect; OffsetRect(&t, rcMap.left, rcMap.top);
            InflateRect(&t, 1, 1);
            Outline(dc, t, ACCENT, 2);
        }
        RECT r = g_hits[g_hoverHit].r; OffsetRect(&r, rcMap.left, rcMap.top);
        Outline(dc, r, RGB(255, 255, 255), 1);
    }
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
        DrawToolbar(mdc);
        DrawSidebar(mdc);
        DrawCrumb(mdc);
        DrawMapArea(mdc);
        DrawStatus(mdc);
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob); DeleteObject(bb); DeleteDC(mdc);
    }
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- input
static void UpdateHover(int mx, int my) {
    int hc = -1;
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--) {
        POINT p = { mx, my };
        if (PtInRect(&g_clicks[i].rc, p)) { hc = i; break; }
    }
    int hh = -1;
    if (g_state == ST_DONE) {
        POINT p = { mx - rcMap.left, my - rcMap.top };
        if (mx >= rcMap.left && mx < rcMap.right && my >= rcMap.top && my < rcMap.bottom)
            for (int i = (int)g_hits.size() - 1; i >= 0; i--)
                if (PtInRect(&g_hits[i].r, p)) { hh = i; break; }
    }
    int nid = hc >= 0 ? g_clicks[hc].id : -1;
    u32 ndata = hc >= 0 ? g_clicks[hc].data : 0;
    if (nid != g_hoverId || ndata != g_hoverData || hh != g_hoverHit) {
        if (hh != g_hoverHit) {
            g_hoverTopValid = false;
            if (hh >= 0) {
                const Hit& h = g_hits[hh];
                if (h.topKind != 255) {
                    for (const Hit& o : g_hits)
                        if (o.kind == h.topKind && o.id == h.top) { g_hoverTopRect = o.r; g_hoverTopValid = true; break; }
                }
            }
        }
        g_hoverClick = hc >= 0 ? 1 : -1; g_hoverId = nid; g_hoverData = ndata; g_hoverHit = hh;
        SetCursor(LoadCursor(NULL, (hc >= 0 || (hh >= 0 && g_hits[hh].topKind == 1)) ? IDC_HAND : IDC_ARROW));
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void ShowInExplorer(const std::wstring& path) {
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
}

static void OpenFolder(const std::wstring& path) {
    std::wstring args = L"\"" + path + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
}

static void GoUp() { if (g_state == ST_DONE && g_view != 0) SetView(g_dirs[g_view].parent); }

static void RelaunchAdmin() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    HINSTANCE r = ShellExecuteW(NULL, L"runas", exe, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static void OnClick(int mx, int my, bool ctrl) {
    POINT p = { mx, my };
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--) {
        if (!PtInRect(&g_clicks[i].rc, p)) continue;
        const Click c = g_clicks[i];
        switch (c.id) {
        case ID_RESCAN: StartScan(g_scanLetter); break;
        case ID_CANCEL: InterlockedExchange(&g_cancel, 1); break;
        case ID_ADMIN: RelaunchAdmin(); break;
        case ID_UP: GoUp(); break;
        case ID_DRIVE: if (c.data < g_drives.size()) StartScan(g_drives[c.data].letter); break;
        case ID_CRUMB: SetView(c.data); break;
        case ID_CAT: g_filter = (g_filter == (int)c.data) ? -1 : (int)c.data; g_mapDirty = true; InvalidateRect(g_hwnd, NULL, FALSE); break;
        case ID_CLEARFILTER:
            g_filter = g_sideTab == 1 ? FILTER_SAFE : -1;
            g_mapDirty = true; ComputeLargest(); InvalidateRect(g_hwnd, NULL, FALSE); break;
        case ID_TAB:
            g_sideTab = (int)c.data; g_filter = g_sideTab == 1 ? FILTER_SAFE : -1;
            g_mapDirty = true; ComputeLargest(); InvalidateRect(g_hwnd, NULL, FALSE); break;
        case ID_SAFE:
            g_filter = (g_filter == FILTER_SAFE + (int)c.data) ? FILTER_SAFE : FILTER_SAFE + (int)c.data;
            g_mapDirty = true; ComputeLargest(); InvalidateRect(g_hwnd, NULL, FALSE); break;
        case ID_FILE:
            if (c.data < g_largest.size()) {
                const ListE& e = g_largest[c.data];
                if (e.kind == 0) ShowInExplorer(FilePath(e.id)); else OpenFolder(DirPath(e.id));
            }
            break;
        }
        return;
    }
    if (g_state != ST_DONE || g_hoverHit < 0 || g_hoverHit >= (int)g_hits.size()) return;
    const Hit h = g_hits[g_hoverHit];
    if (ctrl) {
        if (h.kind == 0) ShowInExplorer(FilePath(h.id));
        else if (h.kind == 1) ShowInExplorer(DirPath(h.id));
        return;
    }
    if (h.topKind == 1) SetView(h.top);
}

// ---------------------------------------------------------------- window
static void MakeFonts() {
    auto mk = [](int px, int weight) {
        return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    g_fUI = mk(13, FW_NORMAL);
    g_fSemi = mk(13, FW_SEMIBOLD);
    g_fSmall = mk(12, FW_NORMAL);
    g_fSmallSemi = mk(12, FW_SEMIBOLD);
    g_fTitle = mk(18, FW_SEMIBOLD);
    g_fHuge = mk(28, FW_LIGHT);
    g_fBigNum = mk(30, FW_LIGHT);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: g_mapDirty = true; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(900); mm->ptMinTrackSize.y = S(600);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme); g_tracking = true;
        }
        UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        if (g_hoverClick != -1 || g_hoverHit != -1) {
            g_hoverClick = -1; g_hoverId = -1; g_hoverHit = -1; g_hoverTopValid = false;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            bool hand = g_hoverClick >= 0 || (g_hoverHit >= 0 && g_hoverHit < (int)g_hits.size() && g_hits[g_hoverHit].topKind == 1);
            SetCursor(LoadCursor(NULL, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_CONTROL) != 0);
        UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_RBUTTONDOWN: GoUp(); return 0;
    case WM_XBUTTONDOWN: if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) GoUp(); return TRUE;
    case WM_KEYDOWN:
        if (wp == VK_BACK || wp == VK_ESCAPE) GoUp();
        else if (wp == VK_F5 && g_state == ST_DONE) StartScan(g_scanLetter);
        else if (wp == VK_HOME && g_state == ST_DONE) SetView(0);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_PROGRESS) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_APP_SCANDONE: {
        KillTimer(hwnd, TIMER_PROGRESS);
        if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
        g_scanMs = GetTickCount() - g_scanStartTick;
        if (g_cancel) { ClearData(); InterlockedExchange(&g_state, ST_IDLE); }
        else { InterlockedExchange(&g_state, ST_DONE); g_view = 0; ComputeLargest(); }
        g_mapDirty = true;
        RefreshDrives();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY:
        if (g_thread) { InterlockedExchange(&g_cancel, 1); WaitForSingleObject(g_thread, 5000); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int show) {
    InitializeCriticalSection(&g_curLock);
    HDC sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    g_isAdmin = IsUserAnAdmin() != FALSE;
    MakeFonts();
    CompileRules();
    RefreshDrives();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    wc.lpszClassName = L"DiskMapWnd";
    wc.hbrBackground = NULL;
    RegisterClassExW(&wc);

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int W = std::min<int>(S(1400), (wa.right - wa.left) * 9 / 10);
    int H = std::min<int>(S(880), (wa.bottom - wa.top) * 9 / 10);
    std::wstring title = g_isAdmin ? L"DiskMap (Administrator)" : L"DiskMap";
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                             wa.left + ((wa.right - wa.left) - W) / 2, wa.top + ((wa.bottom - wa.top) - H) / 2,
                             W, H, NULL, NULL, hi, NULL);
    BOOL dark = TRUE;  // dark title bar (Windows 10 20H1+ / 22H2)
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    // optional: "DiskMap.exe D" starts mapping drive D: right away
    if (cmd && cmd[0]) {
        wchar_t c = (wchar_t)towupper(cmd[0] == L'"' ? cmd[1] : cmd[0]);
        if (c >= L'A' && c <= L'Z') StartScan(c);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
