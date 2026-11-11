// Organizer - sorts the loose files in a folder into category subfolders (Win32 + GDI, no third-party libraries)
// Build: x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows organizer.cpp res.o -o Organizer.exe
//        -static -s -lgdi32 -lmsimg32 -lshell32 -lole32 -luuid -ldwmapi

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
#include <set>
#include <map>
#include <numeric>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <cwchar>

typedef uint64_t u64;
typedef uint32_t u32;

// ---------------------------------------------------------------- categories
enum { K_IMAGES, K_VIDEOS, K_MUSIC, K_DOCS, K_CHAT, K_WEB, K_ARCH, K_DISK, K_PROGS, K_CODE, K_FONTS, K_3D, K_MAPS, K_BENCH,
       K_MCF, K_LOGS, K_LINKS, K_UGAME, K_UMC, K_UDRV, K_UPROJ, K_OTHER, NK };
// sub: may be pulled out of subfolders. Programs, code, logs and unknown files only ever move from the folder itself,
// because inside subfolders they're almost always parts of something (an app, a game, a server...).
struct CatDef { const wchar_t* name; const wchar_t* folder; COLORREF color; bool defOn; bool sub; const wchar_t* exts; };
static const CatDef kCat[NK] = {
    { L"Images", L"Images", RGB(72, 196, 164), true, true,
      L" jpg jpeg jfif png gif bmp webp heic heif tif tiff raw cr2 cr3 nef arw dng orf rw2 psd svg ico avif xcf kra ai eps jpg_large png_large jpg-large png-large jpeg_large " },
    { L"Videos", L"Videos", RGB(238, 164, 66), true, true,
      L" mp4 mkv avi mov wmv webm m4v flv mpg mpeg m2ts mts 3gp vob ogv srt ass ssa vtt " },
    { L"Music & audio", L"Music", RGB(232, 100, 170), true, true,
      L" mp3 flac wav ogg m4a aac opus wma aiff aif alac mid midi " },
    { L"Documents", L"Documents", RGB(226, 208, 96), true, true,
      L" pdf doc docx docm odt rtf txt md xls xlsx xlsm ods csv tsv ppt pptx odp epub mobi azw3 pages numbers key one xps oxps " },
    { L"Discord chat exports", L"Discord Exports", RGB(114, 137, 218), true, true, L"" },
    { L"Web pages", L"Web Pages", RGB(110, 200, 230), true, true, L" html htm xhtml mhtml mht " },
    { L"Archives", L"Archives", RGB(180, 136, 96), true, true,
      L" zip rar 7z tar gz tgz bz2 xz zst cab lz lzma " },
    { L"Disk images", L"Disk Images", RGB(150, 160, 190), true, true,
      L" iso img vhd vhdx vmdk vdi qcow2 dmg wim esd " },
    { L"Programs & installers", L"Programs", RGB(84, 146, 236), true, false,
      L" exe msi msix msixbundle appx appxbundle msp apk jar dll " },
    { L"Code & scripts", L"Code", RGB(116, 204, 104), true, false,
      L" c cpp cc h hpp cs py pyw js mjs jsx tsx java kt go rs rb php lua css scss json xml yaml yml toml ini cfg sh bat cmd ps1 vbs reg sql ipynb sln vcxproj " },
    { L"Fonts", L"Fonts", RGB(172, 124, 236), true, true,
      L" ttf otf woff woff2 fon ttc " },
    { L"3D models", L"3D Models", RGB(90, 200, 200), true, true,
      L" glb gltf obj fbx blend stl 3mf dae 3ds ply usdz vox " },
    { L"Game maps", L"Game Maps", RGB(200, 150, 90), true, true,
      L" bsp map vmf rmf jmf pk3 " },
    { L"Benchmark results", L"Benchmarks", RGB(130, 170, 240), true, true,
      L" 3dmark-result hwbot cvf " },
    { L"Minecraft files", L"Minecraft", RGB(110, 180, 90), true, true,
      L" schem schematic litematic mcworld mcpack mcaddon mcstructure mrpack " },
    { L"Logs & crash dumps", L"Logs", RGB(140, 140, 150), false, false,
      L" log dmp mdmp " },
    { L"Shortcuts & torrents", L"Shortcuts", RGB(170, 170, 120), false, true,
      L" lnk url torrent " },
    { L"Game & app folders", L"Games", RGB(236, 98, 98), false, true, L"" },
    { L"Minecraft worlds & servers", L"Minecraft", RGB(110, 180, 90), false, true, L"" },
    { L"Driver packages", L"Drivers", RGB(172, 124, 236), false, true, L"" },
    { L"Code projects", L"Projects", RGB(116, 204, 104), false, true, L"" },
    { L"Everything else", L"Other", RGB(120, 124, 134), false, false, L"" },
};
static bool IsUnitCat(int k) { return k >= K_UGAME && k <= K_UPROJ; }

static int ExtCat(const wchar_t* ext) {
    if (!ext || !*ext || wcslen(ext) > 15) return K_OTHER;
    wchar_t key[24] = L" ";
    wcscat(key, ext);
    wcscat(key, L" ");
    CharLowerW(key);
    for (int k = 0; k < K_OTHER; k++) if (kCat[k].exts[0] && wcsstr(kCat[k].exts, key)) return k;
    return K_OTHER;
}
static int CatOf(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot || dot == name || !dot[1]) return K_OTHER;
    int c = ExtCat(dot + 1);
    if (c == K_ARCH) {   // compressed maps like "dm1.bsp.zst" are maps, not archives
        std::wstring stem(name, dot - name);
        size_t d2 = stem.rfind(L'.');
        if (d2 != std::wstring::npos && d2 > 0 && ExtCat(stem.c_str() + d2 + 1) == K_MAPS) return K_MAPS;
    }
    return c;
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
static const COLORREF GOOD    = RGB(80, 214, 140);
static const COLORREF WARN    = RGB(240, 180, 70);
static const COLORREF BAD     = RGB(236, 98, 98);

// ---------------------------------------------------------------- state
struct Entry {
    std::wstring name;
    std::wstring rel;                     // folder it's in, relative to g_dir ("" = top)
    u64 size = 0; int cat = 0; bool skip = false; FILETIME mt = {};
    int group = -1;                       // web page that travels with its media folder(s)
    bool locked = false;                  // can't be moved without breaking it
    std::vector<std::wstring> comp;       // rel paths of folders it links into
    std::wstring lockNote;
    bool isDir = false;                   // a whole folder that moves as one unit
    u32 files = 0;
    std::wstring why;
};
struct LinkedDir { std::wstring rel, name; u64 size = 0; u32 files = 0; };
struct Group { std::vector<int> entries; std::vector<int> folders; int cat = 0; };
struct Kept { std::wstring rel, why; };   // subfolders deliberately left alone
static std::vector<LinkedDir> g_folders;
static std::vector<Group> g_groups;
static std::vector<Kept> g_kept;
enum { M_TOP, M_EACH, M_GATHER };
static int g_mode = M_TOP;
static DWORD g_scanMs = 0;
static std::map<std::wstring, FILETIME> g_dirTimes;
struct SniffRes { u64 size = 0; FILETIME t = {}; bool discord = false; std::vector<std::wstring> refs; };
static std::map<std::wstring, SniffRes> g_sniffCache;
struct SizeRes { FILETIME t; u64 size; u32 files; };
static std::map<std::wstring, SizeRes> g_sizeCache;
static std::vector<Entry> g_list;
static std::wstring g_dir;            // absolute, no trailing backslash unless drive root
static std::wstring g_self;
static const wchar_t* LOGNAME = L"_organizer_undo.log";
static bool g_catOn[NK];
static int g_catCount[NK];
static u64 g_catSize[NK];
enum { W_NONE, W_PROGRAM, W_BLOCKED, W_ROOT, W_MISSING };
static int g_warn = W_NONE;
static std::wstring g_msg;
static int g_msgKind = 0;             // 0 good, 1 warning, 2 error
static bool g_hasLog = false;

static HWND g_hwnd;
static int g_dpi = 96;
static inline int S(int v) { return MulDiv(v, g_dpi, 96); }
static HFONT g_fUI, g_fSemi, g_fSmall, g_fSmallSemi, g_fTitle, g_fHuge;

static RECT rcHead, rcBanner, rcSide, rcList, rcBody, rcFoot;
static int g_scroll = 0, g_sscroll = 0;
static bool g_dragThumb = false;
static int g_dragY = 0, g_dragScroll = 0;
static RECT g_thumb = { 0, 0, 0, 0 };
static bool g_tracking = false;

struct Click { RECT rc; int id; u32 data; };
static std::vector<Click> g_clicks;
static int g_hoverId = -1; static u32 g_hoverData = 0;

enum { ID_CHANGE = 1, ID_ORGANIZE, ID_UNDO, ID_OPEN, ID_DISMISS, ID_ALLON, ID_MODE = 50,
       ID_DUPFIND = 60, ID_DUPBACK, ID_DUPCANCEL, ID_DUPDELETE, ID_CAT = 100, ID_ROW = 1000, ID_DUPROW = 2000000, ID_DUPKEEP = 3000000 };

// ---------------------------------------------------------------- helpers
static void FmtSize(u64 b, wchar_t* out, int n) {
    static const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) _snwprintf(out, n, L"%llu B", (unsigned long long)b);
    else _snwprintf(out, n, v < 10 ? L"%.2f %ls" : (v < 100 ? L"%.1f %ls" : L"%.0f %ls"), v, u[i]);
    out[n - 1] = 0;
}
static COLORREF Mix(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t), (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
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
static const UINT TR = DT_SINGLELINE | DT_VCENTER | DT_RIGHT;
static const UINT TC = DT_SINGLELINE | DT_VCENTER | DT_CENTER;

static void AddClick(const RECT& r, int id, u32 data = 0) { g_clicks.push_back({ r, id, data }); }
static bool IsHover(int id, u32 data = 0) { return g_hoverId == id && g_hoverData == data; }

static void Check(HDC dc, int x, int y, bool on, COLORREF col, bool hover) {
    RECT b = { x, y, x + S(16), y + S(16) };
    if (on) RoundC(dc, b, S(5), col);
    else { RoundC(dc, b, S(5), hover ? PANEL3 : PANEL2); RoundOutline(dc, b, S(5), hover ? TEXT3 : RGB(70, 76, 88)); }
    if (on) {
        HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(14, 16, 20));
        HGDIOBJ op = SelectObject(dc, pen);
        POINT pts[3] = { { x + S(4), y + S(8) }, { x + S(7), y + S(11) }, { x + S(12), y + S(5) } };
        Polyline(dc, pts, 3);
        SelectObject(dc, op); DeleteObject(pen);
    }
}

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (!a.empty() && a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}
static std::wstring Long(const std::wstring& p) {   // \\?\ prefix so long paths work
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(Long(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(Long(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring UniqueName(const std::wstring& dir, const std::wstring& name) {
    if (!Exists(Join(dir, name))) return name;
    size_t dot = name.rfind(L'.');
    std::wstring stem = (dot == std::wstring::npos || dot == 0) ? name : name.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos || dot == 0) ? L"" : name.substr(dot);
    for (int i = 2; i < 100000; i++) {
        std::wstring n = stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!Exists(Join(dir, n))) return n;
    }
    return name;
}
static std::wstring ErrText(DWORD e) {
    wchar_t* buf = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0,
                   (LPWSTR)&buf, 0, NULL);
    std::wstring s = buf ? buf : L"unknown error";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s;
}
static bool StartsWithPath(const std::wstring& path, const wchar_t* base) {
    size_t n = wcslen(base);
    if (!n || path.size() < n || _wcsnicmp(path.c_str(), base, n) != 0) return false;
    return path.size() == n || path[n] == L'\\' || base[n - 1] == L'\\';
}

// ---------------------------------------------------------------- web page companions
static std::wstring LowerW(std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }
static std::wstring Utf8ToW(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string UrlDecode(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '%' && i + 2 < v.size() && isxdigit((unsigned char)v[i + 1]) && isxdigit((unsigned char)v[i + 2])) {
            o += (char)strtol(v.substr(i + 1, 2).c_str(), NULL, 16); i += 2;
        } else if (v.compare(i, 5, "&amp;") == 0) { o += '&'; i += 4; }
        else o += v[i];
    }
    return o;
}
// Reads the start of an HTML file: is it a Discord export, and which local folders does it link into?
static SniffRes SniffHtml(const std::wstring& path) {
    SniffRes r;
    HANDLE f = CreateFileW(Long(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return r;
    std::string buf(4u << 20, 0);
    DWORD rd = 0;
    ReadFile(f, &buf[0], (DWORD)buf.size(), &rd, NULL);
    CloseHandle(f);
    buf.resize(rd);
    std::string lo = buf;
    for (char& c : lo) if (c >= 'A' && c <= 'Z') c += 32;
    r.discord = lo.find("discordchatexporter") != std::string::npos || lo.find("cdn.discordapp.com") != std::string::npos ||
                lo.find("discord.com/channels") != std::string::npos;
    std::set<std::wstring> refs;
    for (const char* attr : { "src=", "href=" }) {
        size_t p = 0, al = strlen(attr);
        while ((p = lo.find(attr, p)) != std::string::npos && refs.size() < 64) {
            p += al;
            while (p < lo.size() && (lo[p] == ' ' || lo[p] == '\t')) p++;
            if (p >= lo.size()) break;
            char q = lo[p];
            size_t st, en;
            if (q == '"' || q == '\'') { st = p + 1; en = lo.find(q, st); }
            else { st = p; en = lo.find_first_of(" \t>", st); }
            if (en == std::string::npos) break;
            p = en;
            if (en - st > 2048 || en == st) continue;
            std::string v = buf.substr(st, en - st), vl = lo.substr(st, en - st);
            if (vl.find("://") != std::string::npos || vl[0] == '/' || vl[0] == '\\' || vl[0] == '#' ||
                vl.rfind("data:", 0) == 0 || vl.rfind("mailto:", 0) == 0 || vl.rfind("javascript:", 0) == 0) continue;
            if (v.rfind("./", 0) == 0) v = v.substr(2);
            size_t slash = v.find_first_of("/\\");
            if (slash == std::string::npos || slash == 0) continue;
            std::string seg = UrlDecode(v.substr(0, slash));
            if (seg == "." || seg == "..") continue;
            refs.insert(LowerW(Utf8ToW(seg)));
        }
    }
    r.refs.assign(refs.begin(), refs.end());
    return r;
}
static void WalkSize(const std::wstring& dir, u64& size, u32& files, int depth) {
    if (depth > 64) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) WalkSize(Join(dir, n), size, files, depth + 1);
        } else { size += ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; files++; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- scanning
static std::wstring BackupLogPath() {   // a second copy of the undo log, outside the organized folder
    wchar_t la[MAX_PATH] = L"";
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la);
    std::wstring key = g_dir;
    for (auto& c : key) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return std::wstring(la) + L"\\Organizer\\undo\\" + key + L".log";
}
static std::wstring ActiveLogPath() {
    std::wstring a = Join(g_dir, LOGNAME);
    if (Exists(a)) return a;
    std::wstring b = BackupLogPath();
    return Exists(b) ? b : a;
}
static void CheckLog() { g_hasLog = Exists(Join(g_dir, LOGNAME)) || Exists(BackupLogPath()); }

static int RowCount() { return (int)(g_list.size() + g_kept.size()); }
static void ClampScroll() {
    int rowH = S(32);
    int content = RowCount() * rowH;
    int view = rcBody.bottom - rcBody.top;
    int mx = std::max(0, content - view);
    g_scroll = std::max(0, std::min(g_scroll, mx));
}

static std::wstring RelJoin(const std::wstring& rel, const std::wstring& n) { return rel.empty() ? n : rel + L"\\" + n; }
static std::wstring Abs(const std::wstring& rel) { return rel.empty() ? g_dir : Join(g_dir, rel); }
static bool InList(const std::wstring& s, const wchar_t* const* l) { for (; *l; ++l) if (s == *l) return true; return false; }

static const wchar_t* const kSkipNames[] = { L"windows", L"program files", L"program files (x86)", L"programdata", L"appdata",
    L"$recycle.bin", L"system volume information", L"$windows.~bt", L"$windows.~ws", L"windows.old", L"recovery",
    L"perflogs", L"msocache", L"$sysreset", L"$winreagent", nullptr };
static const wchar_t* const kDevNames[] = { L"node_modules", L"__pycache__", L"venv", L".venv", L"site-packages",
    L"bower_components", nullptr };
static const wchar_t* const kProjFiles[] = { L"package.json", L"cargo.toml", L"cmakelists.txt", L"pyproject.toml",
    L"setup.py", L"go.mod", L"pom.xml", L"build.gradle", L"makefile", L"meson.build", L"requirements.txt", L"configure",
    L"configure.ac", L"makefile.am", L"makefile.in", L"build.ninja", L"gradlew", L"cargo.lock", L"package-lock.json",
    L"yarn.lock", L"pnpm-lock.yaml", L"composer.json", L"gemfile", nullptr };
static const wchar_t* const kProjExts[] = { L".sln", L".csproj", L".vcxproj", L".uproject", L".xcodeproj", nullptr };
static const wchar_t* const kGameDataExts[] = { L".pak", L".xnb", L".xna", L".rpgmvp", L".rpgmvo", L".rpgmvm", L".assets",
    L".ress", L".resource", L".pck", L".wad", L".vpk", L".gma", L".xp3", L".rgss3a", L".rgssad", L".rgss2a", L".upk",
    L".bnk", L".wem", L".fsb", L".bank", L".dnh", L".rpa", L".rpyc", L".wog2", L".cdsfx", L".cdmaterial", L".ucas", L".utoc", nullptr };
static const wchar_t* const kMcDirNames[] = { L".minecraft", L"gdlauncher_next", L"gdlauncher", L"curseforge", L"multimc",
    L"prismlauncher", L"polymc", L"atlauncher", L".technic", L"modrinthapp", L"com.modrinth.theseus", nullptr };
static const wchar_t* const kAppDataNames[] = { L"appdata", L"roaming", L"locallow", L"user data", L"vivaldi", L"google",
    L"chrome", L"mozilla", L"firefox", L"discord", L"spotify", L"steam", L"nvidia corporation", L"crashdumps", L"d3dscache",
    L"electron", L"bravesoftware", L"opera software", nullptr };
static const wchar_t* const kDocLikeExts[] = { L".exe", L".msi", L".txt", L".md", L".pdf", L".url", L".lnk", L".nfo",
    L".rtf", L".htm", L".html", L".ico", nullptr };
static const wchar_t* const kInfoNames[] = { L"readme", L"license", L"licence", L"eula", L"changelog", L"changes", L"credits",
    L"copying", L"authors", L"install", L"notice", L"third_party_notices", L"thirdpartynotices", nullptr };

// What kind of self-contained thing is this folder? Anything other than U_NONE is never split up.
enum { U_NONE, U_GAME, U_MC, U_DRV, U_PROJ, U_APPDATA, U_SYSTEM };
static int UnitKind(const std::wstring& dn, const std::vector<std::wstring>& fl, const std::vector<std::wstring>& dl, std::wstring& why) {
    if (InList(dn, kSkipNames)) { why = L"Windows or system folder"; return U_SYSTEM; }
    if (InList(dn, kAppDataNames)) { why = L"app data / browser profile"; return U_APPDATA; }
    if (InList(dn, kMcDirNames)) { why = L"Minecraft launcher data"; return U_MC; }
    bool exe = false, dll = false, proj = false, inf = false, drvbin = false, game = false, jar = false;
    bool level = false, mcsrv = false, other = false;
    size_t noext = 0;
    for (auto& f : fl) {
        size_t dot = f.rfind(L'.');
        std::wstring ext = (dot == std::wstring::npos || dot == 0) ? L"" : f.substr(dot);
        if (ext.empty()) noext++;
        if (ext == L".exe") exe = true;
        else if (ext == L".dll") dll = true;
        else if (ext == L".inf") inf = true;
        else if (ext == L".sys" || ext == L".cat") drvbin = true;
        else if (ext == L".jar") jar = true;
        if (InList(ext, kGameDataExts)) game = true;
        if (InList(f, kProjFiles) || InList(ext, kProjExts)) proj = true;
        if (f == L"level.dat") level = true;
        if (f == L"server.properties" || f == L"eula.txt") mcsrv = true;
        if (!InList(ext, kDocLikeExts)) other = true;
    }
    int appdirs = 0;
    bool modsDir = false, worldDir = false, gameDir = false;
    for (auto& d : dl) {
        if (d == L".git" || d == L".svn" || d == L".hg" || InList(d, kDevNames)) proj = true;
        if (d == L"mods" || d == L"plugins" || d == L"libraries" || d == L"versions") modsDir = true;
        if (d == L"world" || d == L"saves") worldDir = true;
        if (InList(d, kAppDataNames) || d == L"microsoft" || d == L"packages" || d == L"temp") appdirs++;
        if (d == L"renpy" || d == L"www" || (d.size() > 5 && d.compare(d.size() - 5, 5, L"_data") == 0)) gameDir = true;
    }
    if (level) { why = L"Minecraft world"; return U_MC; }
    if ((mcsrv && (jar || worldDir)) || (jar && modsDir) || (worldDir && modsDir)) { why = L"Minecraft server / modpack"; return U_MC; }
    if (proj) { why = L"code project"; return U_PROJ; }
    if (inf && drvbin) { why = L"driver package"; return U_DRV; }
    if (exe && (dll || game || gameDir || other || !dl.empty())) { why = L"program or game"; return U_GAME; }
    if (game) { why = L"game files"; return U_GAME; }
    if (appdirs >= 2) { why = L"copy of app data"; return U_APPDATA; }
    if (fl.size() >= 20 && noext * 2 > fl.size()) { why = L"app data / cache"; return U_APPDATA; }
    return U_NONE;
}
static int UnitCat(int u) { return u == U_GAME ? K_UGAME : u == U_MC ? K_UMC : u == U_DRV ? K_UDRV : u == U_PROJ ? K_UPROJ : -1; }
static std::wstring LeafName(const std::wstring& rel) { size_t k = rel.rfind(L'\\'); return k == std::wstring::npos ? rel : rel.substr(k + 1); }

struct ScanCtx {
    std::set<std::wstring> skipped, protectedNames;
    std::vector<std::wstring> blocked;
    bool topProgram = false, noRecurse = false, winLike = false;
};

static void AddUnit(const std::wstring& drel, int ucat, const std::wstring& why, ScanCtx& cx) {
    Entry e;
    size_t k = drel.rfind(L'\\');
    e.rel = k == std::wstring::npos ? L"" : drel.substr(0, k);
    e.name = k == std::wstring::npos ? drel : drel.substr(k + 1);
    e.isDir = true; e.cat = ucat; e.why = why;
    std::wstring key = LowerW(drel);
    FILETIME t = g_dirTimes[key];
    auto sc = g_sizeCache.find(key);
    if (sc == g_sizeCache.end() || CompareFileTime(&sc->second.t, &t) != 0) {
        u64 sz = 0; u32 nf = 0;
        WalkSize(Abs(drel), sz, nf, 0);
        g_sizeCache[key] = { t, sz, nf };
        sc = g_sizeCache.find(key);
    }
    e.size = sc->second.size; e.files = sc->second.files; e.mt = t;
    e.skip = cx.skipped.count(drel) > 0;
    g_list.push_back(std::move(e));
}

static void ListNames(const std::wstring& abs, std::vector<std::wstring>& fl, std::vector<std::wstring>& dl) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(abs, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        std::wstring ln = LowerW(n);
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir && (ln == L".git" || ln == L".svn" || ln == L".hg")) { dl.push_back(ln); continue; }
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (isDir) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) dl.push_back(ln); }
        else fl.push_back(ln);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// A folder whose subfolders are mostly the same kind of unit (e.g. "touhou" full of games, "saves" full of worlds)
// is a collection: it moves as one folder, so it ends up as Games\touhou\ instead of being split up.
static int CollectionKind(const std::wstring& abs, const std::vector<std::wstring>& fl, const std::vector<std::wstring>& dl,
                          std::wstring& why) {
    if (dl.size() < 2) return U_NONE;
    for (auto& f : fl) {   // only readmes, shortcuts, images and the like may sit next to the items
        size_t dot = f.rfind(L'.');
        std::wstring ext = (dot == std::wstring::npos || dot == 0) ? L"" : f.substr(dot);
        if (!InList(ext, kDocLikeExts) && ext != L".jpg" && ext != L".png" && ext != L".ini") return U_NONE;
    }
    int cnt[8] = {}, real = 0;
    for (auto& d : dl) {
        if (d == L".git" || d == L".svn" || d == L".hg") continue;
        real++;
        std::vector<std::wstring> f2, d2;
        ListNames(Join(abs, d), f2, d2);
        std::wstring w;
        int u = UnitKind(d, f2, d2, w);
        if (UnitCat(u) >= 0) cnt[u]++;
    }
    int best = U_NONE;
    for (int u = U_GAME; u <= U_PROJ; u++) if (cnt[u] > (best == U_NONE ? 0 : cnt[best])) best = u;
    if (best == U_NONE || cnt[best] < 2 || cnt[best] * 10 < real * 6) return U_NONE;
    static const wchar_t* noun[] = { L"", L"games / programs", L"Minecraft worlds / servers", L"driver packages", L"code projects" };
    why = L"collection of " + std::to_wstring(cnt[best]) + L" " + noun[best];
    return best;
}

static void ScanDir(const std::wstring& rel, int depth, ScanCtx& cx) {
    std::wstring abs = Abs(rel);
    std::vector<Entry> files;
    std::vector<std::pair<std::wstring, FILETIME>> dirs;
    std::vector<std::wstring> fl, dl;
    bool exe = false, dll = false;
    std::wstring logLower = LowerW(LOGNAME);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(abs, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            std::wstring ln = LowerW(n);
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir && (ln == L".git" || ln == L".svn" || ln == L".hg")) { dl.push_back(ln); continue; }
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (isDir) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { dirs.push_back({ n, fd.ftLastWriteTime }); dl.push_back(ln); }
                continue;
            }
            fl.push_back(ln);
            if (ln == logLower) continue;
            if (!_wcsicmp(Join(abs, n).c_str(), g_self.c_str())) continue;   // don't move ourselves
            size_t dot = ln.rfind(L'.');
            std::wstring ext = dot == std::wstring::npos ? L"" : ln.substr(dot);
            if (ext == L".exe") exe = true;
            if (ext == L".dll") dll = true;
            Entry e;
            e.name = n; e.rel = rel;
            e.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.cat = CatOf(n);
            e.mt = fd.ftLastWriteTime;
            e.skip = cx.skipped.count(RelJoin(rel, e.name)) > 0;
            files.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // a subfolder that is a program, game, Minecraft world/server, driver package, code project or app data is one unit:
    // it's either moved whole (if that category is ticked) or left completely alone - never pulled apart
    if (depth > 0) {
        std::wstring why;
        int u = UnitKind(LowerW(LeafName(rel)), fl, dl, why);
        if (u == U_NONE) u = CollectionKind(abs, fl, dl, why);
        if (u != U_NONE) {
            int uc = UnitCat(u);
            if (uc >= 0) AddUnit(rel, uc, why, cx);
            else g_kept.push_back({ rel, why });
            return;
        }
    } else if (exe && dll) cx.topProgram = true;
    for (auto& d : dirs) g_dirTimes[LowerW(RelJoin(rel, d.first))] = d.second;

    // Web pages: find the folders each page links into (saved-page "_files" folders, chat-export media
    // folders...) so the page and its folders always move together and its links keep working.
    std::map<std::wstring, size_t> dirIdx;
    for (size_t i = 0; i < dirs.size(); i++) dirIdx[LowerW(dirs[i].first)] = i;
    std::set<std::wstring> linked;
    for (auto& e : files) {
        if (e.cat != K_WEB) continue;
        std::wstring ln = LowerW(e.name);
        size_t dot = ln.rfind(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : ln.substr(dot);
        if (ext != L".html" && ext != L".htm" && ext != L".xhtml") continue;
        std::wstring stem = ln.substr(0, dot);
        std::set<std::wstring> comp;
        for (const std::wstring& c : { ln + L"_files", stem + L"_files", stem + L".files", stem + L"_media", stem + L"_assets" })
            if (dirIdx.count(c)) comp.insert(c);
        std::wstring key = LowerW(RelJoin(rel, e.name));
        auto it = g_sniffCache.find(key);
        if (it == g_sniffCache.end() || it->second.size != e.size || CompareFileTime(&it->second.t, &e.mt) != 0) {
            SniffRes r = SniffHtml(Join(abs, e.name));
            r.size = e.size; r.t = e.mt;
            g_sniffCache[key] = r;
            it = g_sniffCache.find(key);
        }
        if (it->second.discord) e.cat = K_CHAT;
        for (auto& rf : it->second.refs) if (dirIdx.count(rf)) comp.insert(rf);
        for (auto& c : comp) {
            const std::wstring& real = dirs[dirIdx[c]].first;
            linked.insert(c);
            if (cx.protectedNames.count(c)) { e.locked = true; e.lockNote = L"links into the \"" + real + L"\" folder, so it stays here"; }
            e.comp.push_back(RelJoin(rel, real));
        }
        if (e.locked) e.comp.clear();
    }
    for (auto& e : files) {
        if (depth > 0) {
            if (!kCat[e.cat].sub) continue;   // programs, code, logs, unknown files stay with their folder
            std::wstring ln = LowerW(e.name);
            if (InList(ln.substr(0, ln.find(L'.')), kInfoNames)) continue;   // readme/license files belong with their folder
        }
        g_list.push_back(std::move(e));
    }

    bool recurse = !(g_mode == M_TOP || cx.noRecurse || depth >= 40);
    for (auto& d : dirs) {
        std::wstring ln = LowerW(d.first), drel = RelJoin(rel, d.first);
        if (linked.count(ln)) continue;                                                     // travels with its page
        if (cx.protectedNames.count(ln) && (depth == 0 || g_mode == M_EACH)) continue;     // already sorted
        std::wstring a = Abs(drel);
        bool blocked = false;
        for (auto& b : cx.blocked) if (StartsWithPath(a, b.c_str())) { blocked = true; break; }
        if (blocked) { if (recurse) g_kept.push_back({ drel, L"Windows or app-data folder" }); continue; }
        if (recurse) { ScanDir(drel, depth + 1, cx); continue; }
        if (depth == 0) {   // "Top only": still offer whole game / Minecraft / driver / project folders as units
            std::vector<std::wstring> f2, d2;
            ListNames(a, f2, d2);
            std::wstring why;
            int u = UnitKind(ln, f2, d2, why);
            if (u == U_NONE) u = CollectionKind(a, f2, d2, why);
            int uc = UnitCat(u);
            if (uc >= 0) AddUnit(drel, uc, why, cx);
        }
    }
}

static void Rescan(bool keepSkips) {
    DWORD t0 = GetTickCount();
    ScanCtx cx;
    if (keepSkips) for (auto& e : g_list) if (e.skip) cx.skipped.insert(RelJoin(e.rel, e.name));
    g_list.clear(); g_kept.clear(); g_dirTimes.clear(); g_folders.clear(); g_groups.clear();
    for (int k = 0; k < NK; k++) { g_catCount[k] = 0; g_catSize[k] = 0; }
    g_warn = W_NONE;

    if (!IsDir(g_dir)) { g_warn = W_MISSING; CheckLog(); return; }

    // system / app-data folders are off limits
    static const int ids[] = { CSIDL_WINDOWS, CSIDL_PROGRAM_FILES, CSIDL_PROGRAM_FILESX86, CSIDL_COMMON_APPDATA,
                               CSIDL_LOCAL_APPDATA, CSIDL_APPDATA, CSIDL_PROGRAM_FILES_COMMON };
    for (int id : ids) {
        wchar_t p[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, id, NULL, 0, p))) {
            cx.blocked.push_back(p);
            if (StartsWithPath(g_dir, p)) g_warn = W_BLOCKED;
        }
    }
    if (g_warn == W_BLOCKED) cx.noRecurse = true;
    cx.winLike = g_dir.size() <= 3 || IsDir(Join(g_dir, L"Windows\\System32")) || IsDir(Join(g_dir, L"Program Files"));
    if (cx.winLike) cx.noRecurse = true;   // never dig through a whole drive or a Windows install
    for (int k = 0; k < NK; k++) cx.protectedNames.insert(LowerW(kCat[k].folder));

    ScanDir(L"", 0, cx);

    if (g_warn == W_NONE && cx.topProgram) g_warn = W_PROGRAM;
    if (g_warn == W_NONE && cx.winLike) g_warn = W_ROOT;

    auto sortList = [] {
        std::sort(g_list.begin(), g_list.end(), [](const Entry& a, const Entry& b) {
            if (a.cat != b.cat) return a.cat < b.cat;
            int r = lstrcmpiW(a.rel.c_str(), b.rel.c_str());
            if (r) return r < 0;
            return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0;
        });
    };
    // pages that share a folder form one group; returns true if a page's category had to change
    auto buildGroups = [&]() -> bool {
        g_folders.clear(); g_groups.clear();
        std::vector<int> par(g_list.size());
        std::iota(par.begin(), par.end(), 0);
        std::function<int(int)> find = [&](int x) { return par[x] == x ? x : par[x] = find(par[x]); };
        std::map<std::wstring, int> folderIdx;
        std::vector<int> owner;
        for (int i = 0; i < (int)g_list.size(); i++) {
            g_list[i].group = -1;
            for (auto& rp : g_list[i].comp) {
                std::wstring key = LowerW(rp);
                auto fi = folderIdx.find(key);
                if (fi == folderIdx.end()) {
                    LinkedDir F;
                    F.rel = rp;
                    size_t k = rp.rfind(L'\\');
                    F.name = k == std::wstring::npos ? rp : rp.substr(k + 1);
                    FILETIME t = g_dirTimes[key];
                    auto sc = g_sizeCache.find(key);
                    if (sc == g_sizeCache.end() || CompareFileTime(&sc->second.t, &t) != 0) {
                        u64 sz = 0; u32 nf = 0;
                        WalkSize(Abs(rp), sz, nf, 0);
                        g_sizeCache[key] = { t, sz, nf };
                        sc = g_sizeCache.find(key);
                    }
                    F.size = sc->second.size; F.files = sc->second.files;
                    folderIdx[key] = (int)g_folders.size();
                    g_folders.push_back(F);
                    owner.push_back(i);
                } else par[find(i)] = find(owner[fi->second]);
            }
        }
        std::map<int, int> rootGroup;
        for (int i = 0; i < (int)g_list.size(); i++) {
            if (g_list[i].comp.empty()) continue;
            int r = find(i);
            auto rg = rootGroup.find(r);
            int gid;
            if (rg == rootGroup.end()) { gid = (int)g_groups.size(); rootGroup[r] = gid; g_groups.push_back(Group()); }
            else gid = rg->second;
            g_groups[gid].entries.push_back(i);
            g_list[i].group = gid;
        }
        for (int f = 0; f < (int)g_folders.size(); f++) g_groups[g_list[owner[f]].group].folders.push_back(f);
        bool changed = false;
        for (auto& g : g_groups) {
            g.cat = K_WEB;
            for (int m : g.entries) if (g_list[m].cat == K_CHAT) g.cat = K_CHAT;
            for (int m : g.entries) if (g_list[m].cat != g.cat) { g_list[m].cat = g.cat; changed = true; }
        }
        return changed;
    };
    sortList();
    if (buildGroups()) { sortList(); buildGroups(); }

    for (auto& e : g_list) { g_catCount[e.cat]++; g_catSize[e.cat] += e.size; }
    for (auto& g : g_groups) for (int f : g.folders) g_catSize[g.cat] += g_folders[f].size;
    CheckLog();
    ClampScroll();
    g_scanMs = GetTickCount() - t0;
}

static void SaveMode() {
    DWORD v = (DWORD)g_mode;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Organizer", L"Mode", REG_DWORD, &v, sizeof(v));
}
static void LoadMode() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Organizer", L"Mode", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS && v <= M_GATHER)
        g_mode = (int)v;
}

static void ResetDupView();
static void SetFolder(const std::wstring& p) {
    wchar_t full[4096];
    DWORD n = GetFullPathNameW(p.c_str(), 4096, full, NULL);
    std::wstring d = (n && n < 4096) ? full : p;
    while (d.size() > 3 && d.back() == L'\\') d.pop_back();
    g_dir = d;
    g_scroll = 0;
    g_msg.clear();
    ResetDupView();
    Rescan(false);
    std::wstring title = L"Organizer \x2014 " + g_dir;
    SetWindowTextW(g_hwnd, title.c_str());
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static bool WillMove(const Entry& e) { return !e.skip && !e.locked && g_catOn[e.cat]; }

// ---------------------------------------------------------------- moving
static void LogWrite(HANDLE f, const std::wstring& a, const std::wstring& b) {
    std::wstring line = a + L"\t" + b + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), NULL, 0, NULL, NULL);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u[0], n, NULL, NULL);
    DWORD w; WriteFile(f, u.data(), (DWORD)u.size(), &w, NULL);
}

// ---------------------------------------------------------------- duplicates
#define WM_APP_DUPDONE (WM_APP + 11)
#define WM_APP_DELDONE (WM_APP + 12)
#define TIMER_DUP 7
enum { V_FILES, V_DUPES };
static int g_view = V_FILES;
enum { DS_IDLE, DS_SCANNING, DS_READY, DS_DELETING };
static volatile LONG g_dupState = DS_IDLE, g_dupCancel = 0, g_dupFilesSeen = 0;
static volatile LONG64 g_dupDone = 0, g_dupTotal = 0;
static HANDLE g_dupThread = NULL;
struct DupFile { std::wstring rel; u64 size; FILETIME mt; };
struct DupGroup { std::vector<DupFile> files; int keep = 0; std::vector<char> remove; };
static std::vector<DupGroup> g_dups, g_dupsNew;
static std::vector<std::pair<int, int>> g_dupRows;   // (group, file) - file -1 is the group's header row
static int g_dscroll = 0;
static int g_delOk = 0, g_delFail = 0, g_delMismatch = 0;
static u64 g_delBytes = 0;
static std::vector<std::wstring> g_dupBlocked;
static std::wstring g_dupRoot;
static void ResetDupView() { g_view = V_FILES; g_dupState = DS_IDLE; g_dups.clear(); g_dupRows.clear(); }
static bool DupBusy() { return g_dupState == DS_SCANNING || g_dupState == DS_DELETING; }

static bool CopySuffix(const std::wstring& name) {   // "photo (2).jpg", "photo - Copy.jpg", "Copy of photo.jpg"
    std::wstring ln = LowerW(name);
    size_t dot = ln.rfind(L'.');
    std::wstring stem = (dot == std::wstring::npos || dot == 0) ? ln : ln.substr(0, dot);
    if (stem.find(L" - copy") != std::wstring::npos || stem.rfind(L"copy of ", 0) == 0) return true;
    if (!stem.empty() && stem.back() == L')') {
        size_t o = stem.rfind(L" (");
        if (o != std::wstring::npos && o + 2 < stem.size() - 1) {
            for (size_t i = o + 2; i < stem.size() - 1; i++) if (!iswdigit(stem[i])) return false;
            return true;
        }
    }
    return false;
}
static void ChooseKeeper(DupGroup& g) {
    std::set<std::wstring> cats;
    for (int k = 0; k < NK; k++) cats.insert(LowerW(kCat[k].folder));
    auto score = [&](const DupFile& f) {
        size_t sl = f.rel.rfind(L'\\');
        std::wstring leaf = sl == std::wstring::npos ? f.rel : f.rel.substr(sl + 1);
        int sc = 0;
        if (CopySuffix(leaf)) sc += 1000;                                     // keep the original name
        sc += 10 * (int)std::count(f.rel.begin(), f.rel.end(), L'\\');         // then the least buried one
        size_t k = f.rel.find(L'\\');
        if (k != std::wstring::npos && cats.count(LowerW(f.rel.substr(0, k)))) sc -= 15;   // already sorted
        return sc;
    };
    int best = 0;
    for (int i = 1; i < (int)g.files.size(); i++) {
        int a = score(g.files[i]), b = score(g.files[best]);
        if (a < b || (a == b && CompareFileTime(&g.files[i].mt, &g.files[best].mt) < 0)) best = i;   // then the oldest
    }
    g.keep = best;
    g.remove.assign(g.files.size(), 1);
    g.remove[best] = 0;
}

static bool HashFile(const std::wstring& path, u64 limit, u64& h1, u64& h2) {
    HANDLE f = CreateFileW(Long(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> buf(1 << 20);
    h1 = 0x9E3779B97F4A7C15ull; h2 = 0xC2B2AE3D27D4EB4Full;
    u64 left = limit;
    bool ok = true;
    while (left && !g_dupCancel) {
        DWORD rd = 0, want = (DWORD)std::min<u64>(buf.size(), left);
        if (!ReadFile(f, buf.data(), want, &rd, NULL)) { ok = false; break; }
        if (!rd) break;
        size_t i = 0;
        for (; i + 8 <= rd; i += 8) {
            u64 w; memcpy(&w, &buf[i], 8);
            h1 = (h1 ^ w) * 0x100000001B3ull; h1 ^= h1 >> 29;
            h2 = (h2 + w) * 0xFF51AFD7ED558CCDull; h2 ^= h2 >> 31;
        }
        for (; i < rd; i++) { h1 = (h1 ^ buf[i]) * 0x100000001B3ull; h2 = (h2 + buf[i] + 1) * 0xC4CEB9FE1A85EC53ull; }
        left -= rd;
        InterlockedExchangeAdd64(&g_dupDone, rd);
    }
    CloseHandle(f);
    return ok && !g_dupCancel;
}
static bool SameBytes(const std::wstring& a, const std::wstring& b) {
    HANDLE fa = CreateFileW(Long(a).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (fa == INVALID_HANDLE_VALUE) return false;
    HANDLE fb = CreateFileW(Long(b).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (fb == INVALID_HANDLE_VALUE) { CloseHandle(fa); return false; }
    LARGE_INTEGER sa, sb; GetFileSizeEx(fa, &sa); GetFileSizeEx(fb, &sb);
    bool same = sa.QuadPart == sb.QuadPart;
    std::vector<unsigned char> ba(1 << 20), bb(1 << 20);
    while (same && !g_dupCancel) {
        DWORD ra = 0, rb = 0;
        if (!ReadFile(fa, ba.data(), (DWORD)ba.size(), &ra, NULL) || !ReadFile(fb, bb.data(), (DWORD)bb.size(), &rb, NULL)) { same = false; break; }
        if (ra != rb || memcmp(ba.data(), bb.data(), ra) != 0) { same = false; break; }
        InterlockedExchangeAdd64(&g_dupDone, (LONG64)ra * 2);
        if (!ra) break;
    }
    CloseHandle(fa); CloseHandle(fb);
    return same && !g_dupCancel;
}

struct DupCand { std::wstring rel; u64 size; FILETIME mt; };
static void DupWalk(const std::wstring& rel, int depth, std::vector<DupCand>& out) {
    if (g_dupCancel || depth > 40) return;
    std::wstring abs = rel.empty() ? g_dupRoot : Join(g_dupRoot, rel);
    struct FE { std::wstring name; u64 size; FILETIME mt; };
    std::vector<FE> files;
    std::vector<std::wstring> dirs;
    bool exe = false, dll = false, proj = false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Long(Join(abs, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        std::wstring ln = LowerW(n);
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir && (ln == L".git" || ln == L".svn" || ln == L".hg")) proj = true;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (isDir) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) dirs.push_back(n); continue; }
        if (InList(ln, kProjFiles)) proj = true;
        size_t dot = ln.rfind(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : ln.substr(dot);
        if (ext == L".exe") exe = true;
        if (ext == L".dll") dll = true;
        if (InList(ext, kProjExts)) proj = true;
        files.push_back({ n, ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, fd.ftLastWriteTime });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (depth > 0) {   // never touch programs, games, Minecraft, drivers, code projects or app data
        std::vector<std::wstring> fl, dl2;
        for (auto& f : files) fl.push_back(LowerW(f.name));
        for (auto& d : dirs) dl2.push_back(LowerW(d));
        if (proj) dl2.push_back(L".git");
        std::wstring why;
        if (UnitKind(LowerW(LeafName(rel)), fl, dl2, why) != U_NONE) return;
    }
    (void)exe; (void)dll;

    // folders that web pages / chat exports link into must stay complete
    std::set<std::wstring> dirLower, linked;
    for (auto& d : dirs) dirLower.insert(LowerW(d));
    for (auto& f : files) {
        std::wstring ln = LowerW(f.name);
        size_t dot = ln.rfind(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : ln.substr(dot);
        if (ext != L".html" && ext != L".htm" && ext != L".xhtml") continue;
        std::wstring stem = ln.substr(0, dot);
        for (const std::wstring& c : { ln + L"_files", stem + L"_files", stem + L".files", stem + L"_media", stem + L"_assets" })
            if (dirLower.count(c)) linked.insert(c);
        SniffRes r = SniffHtml(Join(abs, f.name));
        for (auto& rf : r.refs) if (dirLower.count(rf)) linked.insert(rf);
    }
    std::wstring logLower = LowerW(LOGNAME);
    for (auto& f : files) {
        if (f.size < 1024) continue;   // ignore tiny files
        if (LowerW(f.name) == logLower) continue;
        std::wstring r = rel.empty() ? f.name : rel + L"\\" + f.name;
        if (!_wcsicmp(Join(g_dupRoot, r).c_str(), g_self.c_str())) continue;
        out.push_back({ r, f.size, f.mt });
        InterlockedIncrement(&g_dupFilesSeen);
    }
    for (auto& d : dirs) {
        std::wstring ln = LowerW(d), drel = rel.empty() ? d : rel + L"\\" + d;
        if (linked.count(ln) || InList(ln, kSkipNames) || InList(ln, kDevNames)) continue;
        std::wstring a = Join(g_dupRoot, drel);
        bool blocked = false;
        for (auto& b : g_dupBlocked) if (StartsWithPath(a, b.c_str())) { blocked = true; break; }
        if (!blocked) DupWalk(drel, depth + 1, out);
    }
}

static DWORD WINAPI DupThread(LPVOID) {
    std::vector<DupCand> all;
    DupWalk(L"", 0, all);
    std::map<u64, std::vector<int>> bySize;
    for (int i = 0; i < (int)all.size(); i++) bySize[all[i].size].push_back(i);
    u64 total = 0;
    for (auto& kv : bySize) if (kv.second.size() > 1) total += kv.first * kv.second.size();
    g_dupTotal = (LONG64)total;
    std::vector<DupGroup> res;
    // same size -> same first 64 KB -> same full contents (two independent 64-bit hashes)
    for (auto& kv : bySize) {
        if (g_dupCancel) break;
        if (kv.second.size() < 2) continue;
        u64 sz = kv.first;
        std::map<std::pair<u64, u64>, std::vector<int>> part;
        for (int i : kv.second) { u64 a, b; if (HashFile(Join(g_dupRoot, all[i].rel), 65536, a, b)) part[{ a, b }].push_back(i); }
        for (auto& p : part) {
            if (p.second.size() < 2) continue;
            std::map<std::pair<u64, u64>, std::vector<int>> full;
            if (sz <= 65536) full[p.first] = p.second;
            else for (int i : p.second) { u64 a, b; if (HashFile(Join(g_dupRoot, all[i].rel), sz, a, b)) full[{ a, b }].push_back(i); }
            for (auto& q : full) {
                if (q.second.size() < 2) continue;
                DupGroup g;
                for (int i : q.second) g.files.push_back({ all[i].rel, all[i].size, all[i].mt });
                std::sort(g.files.begin(), g.files.end(), [](const DupFile& a, const DupFile& b) { return lstrcmpiW(a.rel.c_str(), b.rel.c_str()) < 0; });
                ChooseKeeper(g);
                res.push_back(std::move(g));
            }
        }
    }
    std::sort(res.begin(), res.end(), [](const DupGroup& a, const DupGroup& b) {
        return a.files[0].size * (a.files.size() - 1) > b.files[0].size * (b.files.size() - 1);
    });
    g_dupsNew = std::move(res);
    PostMessageW(g_hwnd, WM_APP_DUPDONE, 0, 0);
    return 0;
}

static DWORD WINAPI DelThread(LPVOID) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    std::vector<std::wstring> paths;
    std::vector<u64> sizes;
    for (auto& g : g_dups) {
        std::wstring keep = Join(g_dupRoot, g.files[g.keep].rel);
        for (int i = 0; i < (int)g.files.size() && !g_dupCancel; i++) {
            if (i == g.keep || !g.remove[i]) continue;
            std::wstring p = Join(g_dupRoot, g.files[i].rel);
            if (!SameBytes(p, keep)) { g_delMismatch++; continue; }   // changed since the scan: leave it alone
            paths.push_back(p); sizes.push_back(g.files[i].size);
        }
    }
    if (!paths.empty() && !g_dupCancel) {
        std::wstring list;
        for (auto& p : paths) { list += p; list.push_back(L'\0'); }
        list.push_back(L'\0');
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = list.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        SHFileOperationW(&op);
        for (size_t i = 0; i < paths.size(); i++) {
            if (GetFileAttributesW(Long(paths[i]).c_str()) == INVALID_FILE_ATTRIBUTES) { g_delOk++; g_delBytes += sizes[i]; }
            else g_delFail++;
        }
    }
    CoUninitialize();
    PostMessageW(g_hwnd, WM_APP_DELDONE, 0, 0);
    return 0;
}

static void BuildDupRows() {
    g_dupRows.clear();
    for (int gi = 0; gi < (int)g_dups.size(); gi++) {
        g_dupRows.push_back({ gi, -1 });
        for (int fi = 0; fi < (int)g_dups[gi].files.size(); fi++) g_dupRows.push_back({ gi, fi });
    }
}

static void StartDupScan() {
    if (g_warn == W_BLOCKED || g_warn == W_MISSING || DupBusy()) return;
    g_view = V_DUPES;
    g_dups.clear(); g_dupRows.clear(); g_dscroll = 0; g_msg.clear();
    g_dupRoot = g_dir;
    g_dupBlocked.clear();
    static const int ids[] = { CSIDL_WINDOWS, CSIDL_PROGRAM_FILES, CSIDL_PROGRAM_FILESX86, CSIDL_COMMON_APPDATA,
                               CSIDL_LOCAL_APPDATA, CSIDL_APPDATA, CSIDL_PROGRAM_FILES_COMMON };
    for (int id : ids) { wchar_t p[MAX_PATH]; if (SUCCEEDED(SHGetFolderPathW(NULL, id, NULL, 0, p))) g_dupBlocked.push_back(p); }
    g_dupCancel = 0; g_dupDone = 0; g_dupTotal = 0; g_dupFilesSeen = 0;
    InterlockedExchange(&g_dupState, DS_SCANNING);
    g_dupThread = CreateThread(NULL, 64u << 20, DupThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    SetTimer(g_hwnd, TIMER_DUP, 150, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void StartDupDelete() {
    if (DupBusy()) return;
    int n = 0; u64 bytes = 0;
    for (auto& g : g_dups) for (int i = 0; i < (int)g.files.size(); i++) if (i != g.keep && g.remove[i]) { n++; bytes += g.files[i].size; }
    if (!n) return;
    wchar_t sz[32]; FmtSize(bytes, sz, 32);
    wchar_t msg[512];
    _snwprintf(msg, 512, L"Move %d duplicate cop%ls (%ls) to the Recycle Bin?\n\n"
                         L"Each one is compared byte-for-byte with the copy you're keeping first \x2014 anything that doesn't match exactly is left alone.\n\n"
                         L"On drives without a Recycle Bin (some USB drives), Windows deletes them permanently.",
               n, n == 1 ? L"y" : L"ies", sz);
    msg[511] = 0;
    if (MessageBoxW(g_hwnd, msg, L"Organizer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    g_delOk = g_delFail = g_delMismatch = 0; g_delBytes = 0;
    g_dupCancel = 0; g_dupDone = 0; g_dupTotal = (LONG64)bytes * 2;
    InterlockedExchange(&g_dupState, DS_DELETING);
    g_dupThread = CreateThread(NULL, 0, DelThread, NULL, 0, NULL);
    SetTimer(g_hwnd, TIMER_DUP, 150, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void OnDupDone(bool deleting) {
    KillTimer(g_hwnd, TIMER_DUP);
    if (g_dupThread) { WaitForSingleObject(g_dupThread, INFINITE); CloseHandle(g_dupThread); g_dupThread = NULL; }
    if (!deleting) {
        if (g_dupCancel) { g_dupsNew.clear(); g_view = V_FILES; g_dupState = DS_IDLE; g_msg = L"Duplicate search cancelled."; g_msgKind = 1; Rescan(true); }
        else { g_dups = std::move(g_dupsNew); g_dupState = DS_READY; }
    } else {
        // drop removed copies; sets with only one file left are done
        std::vector<DupGroup> keep;
        for (auto& g : g_dups) {
            DupGroup n2;
            int newKeep = -1;
            for (int i = 0; i < (int)g.files.size(); i++) {
                if (GetFileAttributesW(Long(Join(g_dupRoot, g.files[i].rel)).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                if (i == g.keep) newKeep = (int)n2.files.size();
                n2.files.push_back(g.files[i]);
                n2.remove.push_back(i == g.keep ? 0 : g.remove[i]);
            }
            if (n2.files.size() < 2) continue;
            if (newKeep < 0) ChooseKeeper(n2); else n2.keep = newKeep;
            keep.push_back(std::move(n2));
        }
        g_dups.swap(keep);
        g_dupState = DS_READY;
        wchar_t sz[32]; FmtSize(g_delBytes, sz, 32);
        wchar_t b[512];
        _snwprintf(b, 512, L"Moved %d duplicate cop%ls (%ls) to the Recycle Bin.", g_delOk, g_delOk == 1 ? L"y" : L"ies", sz);
        b[511] = 0; g_msg = b; g_msgKind = 0;
        if (g_delMismatch) { _snwprintf(b, 512, L"  %d no longer matched the kept copy, so they were left alone.", g_delMismatch); b[511] = 0; g_msg += b; g_msgKind = 1; }
        if (g_delFail) { _snwprintf(b, 512, L"  %d couldn't be removed (open in another app?).", g_delFail); b[511] = 0; g_msg += b; g_msgKind = 1; }
        if (g_dupCancel) { g_msg += L"  Stopped early."; g_msgKind = 1; }
    }
    BuildDupRows();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static std::wstring DestRel(const Entry& e) {
    return g_mode == M_EACH ? RelJoin(e.rel, kCat[e.cat].folder) : std::wstring(kCat[e.cat].folder);
}

static void Organize() {
    if (g_warn == W_BLOCKED || g_warn == W_MISSING) return;
    int count = 0;
    for (auto& e : g_list) if (WillMove(e)) count++;
    if (!count) return;
    if (g_warn == W_PROGRAM) {
        int r = MessageBoxW(g_hwnd,
            L"This folder looks like an installed program (it has .exe and .dll files side by side).\n\n"
            L"Moving its files will probably stop that program from working.\n\nOrganize it anyway?",
            L"Organizer", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (r != IDYES) return;
    }
    std::wstring logPath = Join(g_dir, LOGNAME);
    if (!Exists(logPath) && Exists(BackupLogPath())) CopyFileW(Long(BackupLogPath()).c_str(), Long(logPath).c_str(), FALSE);
    HANDLE log = CreateFileW(Long(logPath).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_HIDDEN, NULL);
    if (log == INVALID_HANDLE_VALUE) {
        g_msg = L"Couldn't create the undo log in this folder (" + ErrText(GetLastError()) + L"), so nothing was moved.";
        g_msgKind = 2;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    SetFileAttributesW(Long(logPath).c_str(), FILE_ATTRIBUTE_HIDDEN);

    int moved = 0, movedDirs = 0, failed = 0, removed = 0;
    std::map<std::wstring, int> destState;          // lowercase dest folder -> 1 ready / -1 failed
    std::set<std::wstring> destsUsed, srcDirs;
    std::vector<bool> groupDone(g_groups.size(), false);
    std::wstring firstFail;
    auto fail = [&](const std::wstring& why, int n) { failed += n; if (firstFail.empty()) firstFail = why; };
    auto ensure = [&](const std::wstring& drel) -> bool {
        std::wstring key = LowerW(drel);
        auto it = destState.find(key);
        if (it != destState.end()) return it->second > 0;
        std::wstring a = Abs(drel);
        bool ok = true;
        if (!IsDir(a)) {
            if (Exists(a) || !CreateDirectoryW(Long(a).c_str(), NULL)) ok = false;
            else LogWrite(log, L"+", drel);   // remember we created it, so undo can remove it
        }
        destState[key] = ok ? 1 : -1;
        return ok;
    };
    for (size_t i = 0; i < g_list.size(); i++) {
        const Entry& e = g_list[i];
        if (!WillMove(e)) continue;
        if (e.group >= 0 && groupDone[e.group]) continue;
        int n = e.group >= 0 ? (int)g_groups[e.group].entries.size() : 1;
        if (e.group >= 0) groupDone[e.group] = true;
        std::wstring drel = DestRel(e);
        if (!ensure(drel)) { fail(L"couldn't create the \"" + drel + L"\" folder", n); continue; }
        std::wstring dabs = Abs(drel);
        if (e.group < 0) {
            std::wstring dname = UniqueName(dabs, e.name);
            if (MoveFileExW(Long(Abs(RelJoin(e.rel, e.name))).c_str(), Long(Join(dabs, dname)).c_str(), 0)) {
                LogWrite(log, RelJoin(drel, dname), RelJoin(e.rel, e.name));
                moved++;
                destsUsed.insert(LowerW(drel));
                if (!e.rel.empty()) srcDirs.insert(e.rel);
            } else fail(L"\"" + e.name + L"\": " + ErrText(GetLastError()), 1);
            continue;
        }
        // a page and the folders it links into move as one unit. The folders must keep their exact
        // names (the page's links point at them), so a name clash means the whole unit stays put.
        const Group& g = g_groups[e.group];
        std::wstring clash;
        for (int f : g.folders) if (Exists(Join(dabs, g_folders[f].name))) { clash = g_folders[f].name; break; }
        if (!clash.empty()) {
            fail(L"\"" + clash + L"\" already exists inside " + drel + L", so \"" + e.name + L"\" and its files stayed together where they were", n);
            continue;
        }
        std::vector<int> done;
        bool ok = true;
        for (int f : g.folders) {
            if (MoveFileExW(Long(Abs(g_folders[f].rel)).c_str(), Long(Join(dabs, g_folders[f].name)).c_str(), 0)) done.push_back(f);
            else { ok = false; fail(L"\"" + g_folders[f].name + L"\": " + ErrText(GetLastError()), n); break; }
        }
        if (!ok) {   // put back what already moved so the page stays connected to its files
            for (int f : done) MoveFileExW(Long(Join(dabs, g_folders[f].name)).c_str(), Long(Abs(g_folders[f].rel)).c_str(), 0);
            continue;
        }
        for (int f : done) { LogWrite(log, RelJoin(drel, g_folders[f].name), g_folders[f].rel); movedDirs++; }
        for (int m : g.entries) {
            const Entry& me = g_list[m];
            std::wstring dname = UniqueName(dabs, me.name);
            if (MoveFileExW(Long(Abs(RelJoin(me.rel, me.name))).c_str(), Long(Join(dabs, dname)).c_str(), 0)) {
                LogWrite(log, RelJoin(drel, dname), RelJoin(me.rel, me.name));
                moved++;
            } else fail(L"\"" + me.name + L"\": " + ErrText(GetLastError()), 1);
        }
        destsUsed.insert(LowerW(drel));
        if (!e.rel.empty()) srcDirs.insert(e.rel);
    }
    CloseHandle(log);

    // Gather mode: tidy away folders that were left completely empty (undo recreates them)
    if (g_mode == M_GATHER) {
        std::vector<std::wstring> ds(srcDirs.begin(), srcDirs.end());
        std::sort(ds.begin(), ds.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
        for (auto p : ds) {
            while (!p.empty()) {
                if (!RemoveDirectoryW(Long(Abs(p)).c_str())) break;   // fails unless empty
                removed++;
                size_t k = p.rfind(L'\\');
                p = k == std::wstring::npos ? L"" : p.substr(0, k);
            }
        }
    }

    int folders = (int)destsUsed.size();
    wchar_t buf[512];
    if (moved) _snwprintf(buf, 512, L"Moved %d file%ls into %d folder%ls.", moved, moved == 1 ? L"" : L"s", folders, folders == 1 ? L"" : L"s");
    else _snwprintf(buf, 512, L"No files were moved.");
    buf[511] = 0;
    g_msg = buf;
    if (movedDirs) {
        _snwprintf(buf, 512, L"  %d linked media folder%ls went along with %ls web pages.", movedDirs, movedDirs == 1 ? L"" : L"s",
                   movedDirs == 1 ? L"its" : L"their");
        buf[511] = 0; g_msg += buf;
    }
    if (removed) {
        _snwprintf(buf, 512, L"  Removed %d folder%ls that ended up empty.", removed, removed == 1 ? L"" : L"s");
        buf[511] = 0; g_msg += buf;
    }
    g_msgKind = 0;
    if (failed) {
        _snwprintf(buf, 512, L"  %d stayed put \x2014 ", failed); buf[511] = 0;
        g_msg += buf; g_msg += firstFail;
        if (failed > 1) g_msg += L" (and others)";
        g_msg += L". Files that are open in another app can't be moved.";
        g_msgKind = moved ? 1 : 2;
    }
    if (!moved && !movedDirs) {
        for (auto& d : destState) if (d.second > 0) RemoveDirectoryW(Long(Abs(d.first)).c_str());
        DeleteFileW(Long(logPath).c_str());
    } else {
        std::wstring b = BackupLogPath();   // second copy outside this folder, in case the one in here gets lost
        SHCreateDirectoryExW(NULL, b.substr(0, b.rfind(L'\\')).c_str(), NULL);
        CopyFileW(Long(logPath).c_str(), Long(b).c_str(), FALSE);
        SetFileAttributesW(Long(b).c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    Rescan(true);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static bool SafeRel(const std::wstring& r) {   // undo log paths must stay inside this folder
    if (r.empty() || r[0] == L'\\' || r[0] == L'/' || r.find(L':') != std::wstring::npos) return false;
    size_t p = 0;
    for (;;) {
        size_t e = r.find_first_of(L"\\/", p);
        if (e == std::wstring::npos) e = r.size();
        if (e - p == 2 && r.compare(p, 2, L"..") == 0) return false;
        if (e >= r.size()) break;
        p = e + 1;
    }
    return true;
}
static void MakeRelDirs(const std::wstring& rel) {
    size_t p = 0;
    for (;;) {
        size_t e = rel.find(L'\\', p);
        std::wstring part = e == std::wstring::npos ? rel : rel.substr(0, e);
        if (!IsDir(Abs(part))) CreateDirectoryW(Long(Abs(part)).c_str(), NULL);
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
}

static void Undo() {
    std::wstring logPath = ActiveLogPath();
    HANDLE f = CreateFileW(Long(logPath).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { CheckLog(); InvalidateRect(g_hwnd, NULL, FALSE); return; }
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    std::string raw((size_t)sz.QuadPart, 0);
    DWORD rd = 0;
    if (sz.QuadPart) ReadFile(f, &raw[0], (DWORD)sz.QuadPart, &rd, NULL);
    CloseHandle(f);
    raw.resize(rd);
    std::wstring text = Utf8ToW(raw);

    std::vector<std::pair<std::wstring, std::wstring>> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find(L'\n', pos);
        if (e == std::wstring::npos) e = text.size();
        std::wstring ln = text.substr(pos, e - pos);
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        size_t t = ln.find(L'\t');
        if (t != std::wstring::npos) lines.push_back({ ln.substr(0, t), ln.substr(t + 1) });
        pos = e + 1;
    }

    int back = 0, gone = 0;
    std::vector<std::pair<std::wstring, std::wstring>> keep;
    std::vector<std::wstring> created;
    for (size_t i = lines.size(); i-- > 0;) {
        const auto& L = lines[i];
        if (L.first == L"+") { if (SafeRel(L.second)) created.push_back(L.second); continue; }
        if (!SafeRel(L.first) || !SafeRel(L.second)) continue;
        std::wstring from = Abs(L.first);
        if (!Exists(from)) { gone++; continue; }
        size_t k = L.second.rfind(L'\\');
        std::wstring prel = k == std::wstring::npos ? L"" : L.second.substr(0, k);
        std::wstring leaf = k == std::wstring::npos ? L.second : L.second.substr(k + 1);
        if (!prel.empty()) MakeRelDirs(prel);   // gather mode may have removed it
        std::wstring pabs = Abs(prel);
        std::wstring name = UniqueName(pabs, leaf);
        if (MoveFileExW(Long(from).c_str(), Long(Join(pabs, name)).c_str(), 0)) back++;
        else keep.push_back(L);
    }
    for (auto& c : created) RemoveDirectoryW(Long(Abs(c)).c_str());   // only succeeds if it's empty
    if (keep.empty()) { DeleteFileW(Long(Join(g_dir, LOGNAME)).c_str()); DeleteFileW(Long(BackupLogPath()).c_str()); }
    else {
        HANDLE w = CreateFileW(Long(logPath).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (w != INVALID_HANDLE_VALUE) {
            for (size_t i = keep.size(); i-- > 0;) LogWrite(w, keep[i].first, keep[i].second);
            CloseHandle(w);
            std::wstring other = logPath == BackupLogPath() ? Join(g_dir, LOGNAME) : BackupLogPath();
            CopyFileW(Long(logPath).c_str(), Long(other).c_str(), FALSE);
        }
    }
    wchar_t buf[256];
    _snwprintf(buf, 256, L"Put %d item%ls back where they were.", back, back == 1 ? L"" : L"s");
    buf[255] = 0;
    g_msg = buf; g_msgKind = 0;
    if (!keep.empty()) {
        _snwprintf(buf, 256, L"  %d couldn't be moved back (probably open in another app) \x2014 close it and press Undo again.", (int)keep.size());
        buf[255] = 0; g_msg += buf; g_msgKind = 1;
    }
    if (gone) {
        _snwprintf(buf, 256, L"  %d had been moved or deleted since, so they were skipped.", gone);
        buf[255] = 0; g_msg += buf; if (!g_msgKind) g_msgKind = 1;
    }
    Rescan(false);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void PickFolder() {
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg))) return;
    DWORD o = 0; dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose a folder to organize");
    IShellItem* start = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(g_dir.c_str(), NULL, IID_IShellItem, (void**)&start))) {
        dlg->SetFolder(start); start->Release();
    }
    if (dlg->Show(g_hwnd) == S_OK) {
        IShellItem* res = NULL;
        if (SUCCEEDED(dlg->GetResult(&res))) {
            PWSTR p = NULL;
            if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p))) { SetFolder(p); CoTaskMemFree(p); }
            res->Release();
        }
    }
    dlg->Release();
}

// ---------------------------------------------------------------- drawing
static void Layout(const RECT& cr) {
    int head = S(68), foot = S(76), side = S(320);
    int banner = (g_warn != W_NONE || !g_msg.empty()) ? S(52) : 0;
    rcHead = { 0, 0, cr.right, head };
    rcBanner = { 0, head, cr.right, head + banner };
    rcFoot = { 0, cr.bottom - foot, cr.right, cr.bottom };
    rcSide = { 0, head + banner, side, rcFoot.top };
    rcList = { side + S(16), head + banner + S(16), cr.right - S(16), rcFoot.top - S(16) };
    rcBody = { rcList.left, rcList.top + S(36), rcList.right, rcList.bottom - S(4) };
}

static void Button(HDC dc, RECT r, const wchar_t* label, int id, bool primary, bool enabled = true) {
    bool hv = enabled && IsHover(id);
    COLORREF bg = primary ? (enabled ? (hv ? Mix(ACCENT, RGB(255, 255, 255), 0.12) : ACCENT) : PANEL3) : (hv ? PANEL3 : PANEL2);
    RoundC(dc, r, S(8), bg);
    if (!primary) RoundOutline(dc, r, S(8), hv ? RGB(70, 76, 88) : BORDER);
    Text(dc, label, r, g_fSemi, primary ? (enabled ? RGB(10, 14, 22) : TEXT3) : (enabled ? TEXT1 : TEXT3), TC);
    if (enabled) AddClick(r, id);
}

static void DrawLogo(HDC dc, int x, int y, int s) {
    int g = std::max(2, s / 10), h = (s - 2 * g) / 3;
    RECT a = { x, y, x + s, y + h };                          FillC(dc, a, kCat[K_IMAGES].color);
    RECT b = { x, y + h + g, x + s * 3 / 4, y + 2 * h + g };  FillC(dc, b, kCat[K_VIDEOS].color);
    RECT c = { x, y + 2 * (h + g), x + s / 2, y + s };        FillC(dc, c, kCat[K_PROGS].color);
}

static void DrawHead(HDC dc) {
    FillC(dc, rcHead, PANEL);
    RECT line = { 0, rcHead.bottom - 1, rcHead.right, rcHead.bottom }; FillC(dc, line, BORDER);
    int cy = (rcHead.top + rcHead.bottom) / 2;
    DrawLogo(dc, S(20), cy - S(11), S(22));
    RECT t = { S(54), rcHead.top, S(200), rcHead.bottom };
    Text(dc, L"Organizer", t, g_fTitle, TEXT1, TL);

    int bx = rcHead.right - S(16);
    RECT b1 = { bx - S(130), cy - S(17), bx, cy + S(17) };
    Button(dc, b1, L"Change folder", ID_CHANGE, false);
    RECT b2 = { b1.left - S(8) - S(136), cy - S(17), b1.left - S(8), cy + S(17) };
    Button(dc, b2, L"Open in Explorer", ID_OPEN, false, g_warn != W_MISSING);

    int px = S(190);
    RECT pill = { px, cy - S(17), b2.left - S(16), cy + S(17) };
    if (pill.right - pill.left > S(120)) {
        RoundC(dc, pill, S(8), BG);
        RoundOutline(dc, pill, S(8), BORDER);
        RECT lab = { pill.left + S(12), pill.top, pill.left + S(70), pill.bottom };
        Text(dc, L"FOLDER", lab, g_fSmallSemi, TEXT3, TL);
        RECT pr = { pill.left + S(68), pill.top, pill.right - S(12), pill.bottom };
        HGDIOBJ of = SelectObject(dc, g_fSemi); SetTextColor(dc, TEXT1);
        DrawTextW(dc, g_dir.c_str(), -1, &pr, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    }
}

static void DrawBanner(HDC dc) {
    if (rcBanner.bottom <= rcBanner.top) return;
    std::wstring s; COLORREF c;
    if (!g_msg.empty()) { s = g_msg; c = g_msgKind == 0 ? GOOD : (g_msgKind == 1 ? WARN : BAD); }
    else if (g_warn == W_BLOCKED) { s = L"This is a Windows or app-data folder. Rearranging it could break Windows or your apps, so organizing is turned off here."; c = BAD; }
    else if (g_warn == W_MISSING) { s = L"This folder doesn't exist anymore. Pick another one with Change folder."; c = BAD; }
    else if (g_warn == W_PROGRAM) { s = L"Heads up: this looks like an installed program's folder (.exe and .dll files side by side). Moving them will probably break it."; c = WARN; }
    else { s = g_mode == M_TOP ? L"This is the top of a drive or a Windows install. Only the loose files sitting right here will move."
                               : L"Subfolders are switched off at the top of a drive or a Windows install \x2014 pick a specific folder (like Downloads) to include its subfolders."; c = ACCENT; }
    RECT r = { S(16), rcBanner.top + S(10), rcBanner.right - S(16), rcBanner.bottom - S(2) };
    RoundC(dc, r, S(8), Mix(BG, c, 0.14));
    RECT st = { r.left, r.top + S(8), r.left + S(4), r.bottom - S(8) };
    FillC(dc, st, c);
    RECT tr = { r.left + S(16), r.top, r.right - S(40), r.bottom };
    Text(dc, s.c_str(), tr, g_fUI, TEXT1, TL);
    if (!g_msg.empty()) {
        RECT x = { r.right - S(34), r.top + S(6), r.right - S(8), r.bottom - S(6) };
        if (IsHover(ID_DISMISS)) RoundC(dc, x, S(6), Mix(BG, c, 0.3));
        int mx = (x.left + x.right) / 2, my = (x.top + x.bottom) / 2, k = S(4);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1) + 1 / 2), TEXT2);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, mx - k, my - k, NULL); LineTo(dc, mx + k + 1, my + k + 1);
        MoveToEx(dc, mx + k, my - k, NULL); LineTo(dc, mx - k - 1, my + k + 1);
        SelectObject(dc, op); DeleteObject(pen);
        AddClick(x, ID_DISMISS);
    }
}

static void DrawDupSide(HDC dc);
static void DrawDupList(HDC dc);

static void DrawSide(HDC dc) {
    if (g_view == V_DUPES) { DrawDupSide(dc); return; }
    FillC(dc, rcSide, PANEL);
    RECT line = { rcSide.right - 1, rcSide.top, rcSide.right, rcSide.bottom }; FillC(dc, line, BORDER);
    HRGN clip = CreateRectRgn(rcSide.left, rcSide.top, rcSide.right - 1, rcSide.bottom);
    SelectClipRgn(dc, clip);
    int L = S(22), R = rcSide.right - S(22), y = rcSide.top + S(18);

    RECT h0 = { L, y, R, y + S(18) };
    Text(dc, L"SUBFOLDERS", h0, g_fSmallSemi, TEXT3, TL);
    y += S(24);
    {
        RECT seg = { L, y, R, y + S(32) };
        RoundC(dc, seg, S(9), PANEL2);
        const wchar_t* lab[3] = { L"Top only", L"In place", L"Gather up" };
        int w = (R - L - S(6)) / 3;
        for (int t = 0; t < 3; t++) {
            RECT tr = { L + S(3) + t * w, y + S(3), L + S(3) + (t + 1) * w, y + S(29) };
            bool sel = g_mode == t, hv = IsHover(ID_MODE, (u32)t);
            if (sel) RoundC(dc, tr, S(7), PANEL3);
            Text(dc, lab[t], tr, g_fSemi, (sel || hv) ? TEXT1 : TEXT2, TC);
            if (!sel) AddClick(tr, ID_MODE, (u32)t);
        }
        y += S(40);
        const wchar_t* desc[3] = {
            L"Only files sitting directly in this folder are sorted. Subfolders aren't touched.",
            L"Every subfolder is sorted on its own \x2014 each gets its own Images, Videos\x2026 folders. Your folder layout stays the same.",
            L"Files from every subfolder are pulled up into one set of folders here. Folders left empty are removed (Undo brings them back)." };
        RECT d = { L, y, R, y };
        HGDIOBJ of = SelectObject(dc, g_fSmall);
        DrawTextW(dc, desc[g_mode], -1, &d, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, of);
        d.right = R;
        Text(dc, desc[g_mode], d, g_fSmall, TEXT2, DT_WORDBREAK);
        y = d.bottom + S(20);
    }

    RECT h = { L, y, R, y + S(18) };
    Text(dc, L"SORT INTO FOLDERS", h, g_fSmallSemi, TEXT3, TL);
    y += S(26);
    int shown = 0, listTop = y, listBottom = rcSide.bottom - S(8);
    int nrows = 0;
    for (int k = 0; k < NK; k++) if (g_catCount[k]) nrows++;
    int rowsH = nrows * S(42);
    g_sscroll = std::max(0, std::min(g_sscroll, std::max(0, rowsH - (listBottom - listTop))));
    HRGN rclip = CreateRectRgn(rcSide.left, listTop - S(2), rcSide.right - 1, listBottom);
    SelectClipRgn(dc, rclip);
    y = listTop - g_sscroll;
    for (int k = 0; k < NK; k++) {
        if (!g_catCount[k]) continue;
        shown++;
        RECT row = { L - S(10), y - S(2), R + S(10), y + S(38) };
        bool hv = IsHover(ID_CAT, (u32)k), on = g_catOn[k];
        if (hv) RoundC(dc, row, S(8), PANEL2);
        Check(dc, L, y + S(4), on, kCat[k].color, hv);
        RECT n = { L + S(28), y, R - S(70), y + S(20) };
        Text(dc, kCat[k].name, n, g_fSemi, on ? TEXT1 : TEXT2, TL);
        wchar_t cnt[32]; _snwprintf(cnt, 32, L"%d", g_catCount[k]); cnt[31] = 0;
        RECT cr = { R - S(70), y, R, y + S(20) };
        Text(dc, cnt, cr, g_fSemi, on ? TEXT1 : TEXT3, TR);
        wchar_t sz[32], sub[96]; FmtSize(g_catSize[k], sz, 32);
        if (on) _snwprintf(sub, 96, IsUnitCat(k) ? L"\x2192 %ls\\ (whole folders)   \x00B7   %ls" : L"\x2192 %ls\\   \x00B7   %ls", kCat[k].folder, sz);
        else _snwprintf(sub, 96, L"Stays where it is   \x00B7   %ls", sz);
        sub[95] = 0;
        RECT sr = { L + S(28), y + S(18), R, y + S(35) };
        Text(dc, sub, sr, g_fSmall, on ? TEXT2 : TEXT3, TL);
        RECT hit = row;
        hit.top = std::max<LONG>(hit.top, listTop - S(2)); hit.bottom = std::min<LONG>(hit.bottom, listBottom);
        if (hit.bottom > hit.top) AddClick(hit, ID_CAT, (u32)k);
        y += S(42);
    }
    SelectClipRgn(dc, clip);
    DeleteObject(rclip);
    if (rowsH > listBottom - listTop) {   // scroll hint
        int track = listBottom - listTop, th = std::max<int>(S(24), track * track / rowsH);
        int ty = listTop + (track - th) * g_sscroll / std::max(1, rowsH - track);
        RECT t = { rcSide.right - S(7), ty, rcSide.right - S(4), ty + th };
        RoundC(dc, t, S(3), RGB(70, 76, 88));
    }
    if (!shown) {
        RECT t = { L, y, R, y + S(20) };
        Text(dc, L"Nothing to sort.", t, g_fUI, TEXT2, TL);
        y += S(30);
    }
    y += S(8);
    RECT note = { L, y, R, rcSide.bottom - S(14) };
    if (rowsH <= listBottom - listTop && note.bottom - note.top > S(60)) {
        const wchar_t* msg =
            L"Nothing is ever deleted or overwritten \x2014 duplicates get \"(2)\" added to the name. Programs, games, Minecraft worlds and servers, "
            L"driver packages, code projects and app data are never split up \x2014 they move as whole folders or not at all. "
            L"Program and code files are never pulled out of subfolders.\n\n"
            L"Click a file on the right to leave it where it is. Undo puts everything back.";
        Text(dc, msg, note, g_fSmall, TEXT3, DT_WORDBREAK);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static void NameWithSuffix(HDC dc, RECT n, const std::wstring& name, const std::wstring& suf, COLORREF nc, COLORREF sc) {
    if (suf.empty()) { Text(dc, name.c_str(), n, g_fUI, nc, TL); return; }
    int nw = std::min<int>(TextW(dc, name.c_str(), g_fUI), (n.right - n.left) * 11 / 20);
    RECT n1 = n; n1.right = n.left + nw;
    Text(dc, name.c_str(), n1, g_fUI, nc, TL);
    RECT n2 = n; n2.left = n1.right + S(10);
    Text(dc, suf.c_str(), n2, g_fSmall, sc, TL);
}

static void DrawList(HDC dc) {
    if (g_view == V_DUPES) { DrawDupList(dc); return; }
    RoundC(dc, rcList, S(12), PANEL);
    RoundOutline(dc, rcList, S(12), BORDER);
    int L = rcList.left + S(16), R = rcList.right - S(20);
    int destW = S(170), sizeW = S(90);
    RECT hdr = { rcList.left + 1, rcList.top + 1, rcList.right - 1, rcList.top + S(36) };
    int hy = hdr.top;
    RECT h1 = { L + S(30), hy, R - destW - sizeW - S(20), hdr.bottom };
    Text(dc, L"FILE", h1, g_fSmallSemi, TEXT3, TL);
    RECT h2 = { R - destW - sizeW - S(10), hy, R - destW - S(16), hdr.bottom };
    Text(dc, L"SIZE", h2, g_fSmallSemi, TEXT3, TR);
    RECT h3 = { R - destW, hy, R, hdr.bottom };
    Text(dc, L"GOES TO", h3, g_fSmallSemi, TEXT3, TL);
    RECT hl = { rcList.left + 1, hdr.bottom - 1, rcList.right - 1, hdr.bottom }; FillC(dc, hl, BORDER);

    if (g_list.empty() && g_kept.empty()) {
        RECT t = rcBody; t.bottom = (rcBody.top + rcBody.bottom) / 2;
        Text(dc, g_warn == W_MISSING ? L"Folder not found" : L"Nothing to sort", t, g_fHuge, TEXT1, DT_SINGLELINE | DT_BOTTOM | DT_CENTER);
        RECT t2 = rcBody; t2.top = t.bottom + S(8); t2.bottom = t2.top + S(22);
        Text(dc, g_warn == W_MISSING ? L"Use Change folder to pick another one." :
                 (g_mode == M_TOP ? L"There are no loose files in this folder. Try \"In place\" or \"Gather up\" to include subfolders."
                                  : L"No files found here or in its subfolders. Drop a folder onto this window or use Change folder."),
             t2, g_fUI, TEXT2, TC);
        return;
    }

    int rowH = S(32);
    ClampScroll();
    int first = g_scroll / rowH;
    int total = RowCount();
    HRGN clip = CreateRectRgn(rcBody.left, rcBody.top, rcBody.right, rcBody.bottom);
    SelectClipRgn(dc, clip);
    for (int i = first; i < total; i++) {
        int y = rcBody.top + i * rowH - g_scroll;
        if (y >= rcBody.bottom) break;
        int cy = y + rowH / 2;
        RECT n = { L + S(30), y, R - destW - sizeW - S(20), y + rowH };
        if (i >= (int)g_list.size()) {   // folders left alone
            const Kept& k = g_kept[i - g_list.size()];
            if (i == (int)g_list.size() && i > 0) { RECT d = { L, y, R, y + 1 }; FillC(dc, d, RGB(60, 66, 78)); }
            RECT fg = { L + S(1), cy - S(5), L + S(15), cy + S(6) };
            RoundOutline(dc, fg, S(3), TEXT3);
            RECT tab = { L + S(1), cy - S(7), L + S(7), cy - S(4) }; FillC(dc, tab, TEXT3);
            NameWithSuffix(dc, n, k.rel + L"\\", k.why + L" \x2014 left as-is", TEXT2, TEXT3);
            RECT st = { R - destW, y, R, y + rowH };
            Text(dc, L"Not touched", st, g_fSmall, TEXT3, TL);
            continue;
        }
        const Entry& e = g_list[i];
        bool move = WillMove(e), catOn = g_catOn[e.cat];
        RECT row = { rcList.left + S(6), y + 1, rcList.right - S(14), y + rowH - 1 };
        bool hv = IsHover(ID_ROW, (u32)i);
        if (hv) RoundC(dc, row, S(6), PANEL2);
        if (i > 0 && g_list[i - 1].cat != e.cat) { RECT d = { L, y, R, y + 1 }; FillC(dc, d, BORDER); }
        Check(dc, L, cy - S(8), move, kCat[e.cat].color, hv && catOn && !e.locked);

        std::wstring extra; u64 shown = e.size;
        if (e.locked) extra = e.lockNote;
        else if (e.group >= 0) {
            const Group& g = g_groups[e.group];
            u32 nf = 0; u64 fs = 0;
            for (int f : g.folders) {
                if (!extra.empty()) extra += L", ";
                extra += g_folders[f].name + L"\\";
                nf += g_folders[f].files; fs += g_folders[f].size;
            }
            wchar_t t[64];
            _snwprintf(t, 64, L"  (%u file%ls%ls)", nf, nf == 1 ? L"" : L"s", g.entries.size() > 1 ? L", shared" : L"");
            t[63] = 0;
            extra = L"+ " + extra + t;
            if (g.entries.size() == 1) shown += fs;
        }
        std::wstring suf;
        if (e.isDir) suf = std::wstring(L"whole folder \x00B7 ") + std::to_wstring(e.files) + L" files \x00B7 " + e.why;
        if (!e.rel.empty()) suf += (suf.empty() ? L"" : L"   ") + (L"in " + e.rel + L"\\");
        if (!extra.empty()) suf += (suf.empty() ? L"" : L"   ") + extra;
        COLORREF sc = (move && (e.group >= 0 || e.isDir)) ? Mix(TEXT2, kCat[e.cat].color, 0.45) : TEXT3;
        NameWithSuffix(dc, n, e.isDir ? e.name + L"\\" : e.name, suf, move ? TEXT1 : TEXT3, sc);

        wchar_t sz[32]; FmtSize(shown, sz, 32);
        RECT s = { R - destW - sizeW - S(10), y, R - destW - S(16), y + rowH };
        Text(dc, sz, s, g_fSmall, move ? TEXT2 : TEXT3, TR);
        if (move) {
            std::wstring dest = std::wstring(kCat[e.cat].folder) + L"\\";
            int w = TextW(dc, dest.c_str(), g_fSmallSemi) + S(30);
            RECT pill = { R - destW, cy - S(11), std::min<int>(R - destW + w, R), cy + S(11) };
            RoundC(dc, pill, S(11), Mix(PANEL, kCat[e.cat].color, 0.22));
            RECT dot = { pill.left + S(8), cy - S(3), pill.left + S(14), cy + S(3) };
            RoundC(dc, dot, S(6), kCat[e.cat].color);
            RECT pt = { pill.left + S(18), pill.top, pill.right - S(6), pill.bottom };
            Text(dc, dest.c_str(), pt, g_fSmallSemi, TEXT1, TL);
        } else {
            RECT st = { R - destW, y, R, y + rowH };
            Text(dc, (catOn && !e.locked) ? L"Skipped \x2014 stays here" : L"Stays here", st, g_fSmall, TEXT3, TL);
        }
        RECT hit = row;
        if (hit.top < rcBody.top) hit.top = rcBody.top;
        if (hit.bottom > rcBody.bottom) hit.bottom = rcBody.bottom;
        if (catOn && !e.locked && hit.bottom > hit.top) AddClick(hit, ID_ROW, (u32)i);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);

    // scrollbar
    int content = total * rowH, view = rcBody.bottom - rcBody.top;
    g_thumb = { 0, 0, 0, 0 };
    if (content > view) {
        RECT track = { rcList.right - S(10), rcBody.top + S(2), rcList.right - S(5), rcBody.bottom - S(2) };
        int th = std::max<int>(S(30), (int)((long long)(track.bottom - track.top) * view / content));
        int ty = track.top + (int)((long long)(track.bottom - track.top - th) * g_scroll / (content - view));
        g_thumb = { track.left - S(2), ty, track.right + S(1), ty + th };
        RECT t = { track.left, ty, track.right, ty + th };
        RoundC(dc, t, S(5), g_dragThumb ? TEXT3 : RGB(70, 76, 88));
    }
}

static void DrawDupSide(HDC dc) {
    FillC(dc, rcSide, PANEL);
    RECT line = { rcSide.right - 1, rcSide.top, rcSide.right, rcSide.bottom }; FillC(dc, line, BORDER);
    int L = S(22), R = rcSide.right - S(22), y = rcSide.top + S(20);
    RECT h = { L, y, R, y + S(18) };
    Text(dc, L"DUPLICATES", h, g_fSmallSemi, TEXT3, TL);
    y += S(28);
    if (g_dupState == DS_READY && !g_dups.empty()) {
        int extra = 0, sel = 0; u64 wasted = 0, selBytes = 0;
        for (auto& g : g_dups) {
            extra += (int)g.files.size() - 1;
            wasted += g.files[0].size * (g.files.size() - 1);
            for (int i = 0; i < (int)g.files.size(); i++) if (i != g.keep && g.remove[i]) { sel++; selBytes += g.files[i].size; }
        }
        wchar_t big[32]; FmtSize(selBytes, big, 32);
        RECT br = { L, y, R, y + S(36) };
        Text(dc, big, br, g_fHuge, RGB(236, 98, 98), TL);
        y += S(38);
        RECT sr = { L, y, R, y + S(20) };
        Text(dc, L"will be freed by removing the ticked copies", sr, g_fSmall, TEXT2, TL);
        y += S(30);
        wchar_t ln1[128], w2[32]; FmtSize(wasted, w2, 32);
        _snwprintf(ln1, 128, L"%d set%ls of identical files", (int)g_dups.size(), g_dups.size() == 1 ? L"" : L"s"); ln1[127] = 0;
        RECT r1 = { L, y, R, y + S(20) }; Text(dc, ln1, r1, g_fUI, TEXT1, TL); y += S(22);
        _snwprintf(ln1, 128, L"%d extra cop%ls  \x00B7  %ls in total", extra, extra == 1 ? L"y" : L"ies", w2); ln1[127] = 0;
        RECT r2 = { L, y, R, y + S(20) }; Text(dc, ln1, r2, g_fUI, TEXT1, TL); y += S(34);
    }
    RECT note = { L, y, R, rcSide.bottom - S(14) };
    const wchar_t* msg =
        L"Only exact byte-for-byte copies are listed. In each set, the copy marked Keep stays and the ticked ones go to the Recycle Bin. "
        L"Each copy is compared against the kept one again right before it's removed.\n\n"
        L"Organizer keeps the original name over \"(2)\" or \"- Copy\" versions, and the copy that's already sorted. Click \"Keep this\" to pick a different one.\n\n"
        L"Never searched: program, game and code-project folders, Windows and app folders, web-page media folders, and files under 1 KB.";
    Text(dc, msg, note, g_fSmall, TEXT3, DT_WORDBREAK);
}

static void DrawDupList(HDC dc) {
    g_thumb = { 0, 0, 0, 0 };
    RoundC(dc, rcList, S(12), PANEL);
    RoundOutline(dc, rcList, S(12), BORDER);
    int L = rcList.left + S(16), R = rcList.right - S(20);
    int actW = S(170), sizeW = S(90);
    RECT hdr = { rcList.left + 1, rcList.top + 1, rcList.right - 1, rcList.top + S(36) };
    RECT h1 = { L + S(30), hdr.top, R - actW - sizeW - S(20), hdr.bottom };
    Text(dc, L"IDENTICAL FILES", h1, g_fSmallSemi, TEXT3, TL);
    RECT h2 = { R - actW - sizeW - S(10), hdr.top, R - actW - S(16), hdr.bottom };
    Text(dc, L"SIZE", h2, g_fSmallSemi, TEXT3, TR);
    RECT hl = { rcList.left + 1, hdr.bottom - 1, rcList.right - 1, hdr.bottom }; FillC(dc, hl, BORDER);

    if (DupBusy()) {
        int cx = (rcBody.left + rcBody.right) / 2, cy = (rcBody.top + rcBody.bottom) / 2;
        int W = std::min<int>(S(480), rcBody.right - rcBody.left - S(40));
        RECT t = { cx - W / 2, cy - S(70), cx + W / 2, cy - S(36) };
        Text(dc, g_dupState == DS_SCANNING ? L"Looking for duplicates\x2026" : L"Double-checking and removing\x2026", t, g_fHuge, TEXT1, TC);
        double frac = g_dupTotal ? std::min(1.0, (double)g_dupDone / g_dupTotal) : 0;
        RECT bar = { cx - W / 2, cy - S(16), cx + W / 2, cy - S(8) };
        RoundC(dc, bar, S(4), RGB(46, 51, 60));
        RECT fb = bar; fb.right = bar.left + std::max<LONG>(S(8), (LONG)((bar.right - bar.left) * frac));
        RoundC(dc, fb, S(4), ACCENT);
        wchar_t s1[160], a[32], b[32];
        FmtSize((u64)g_dupDone, a, 32); FmtSize((u64)g_dupTotal, b, 32);
        if (g_dupState == DS_SCANNING) _snwprintf(s1, 160, L"%ld files found  \x00B7  compared %ls of %ls", g_dupFilesSeen, a, b);
        else _snwprintf(s1, 160, L"Checked %ls of %ls", a, b);
        s1[159] = 0;
        RECT t2 = { cx - W / 2, cy, cx + W / 2, cy + S(22) };
        Text(dc, s1, t2, g_fUI, TEXT2, TC);
        RECT cb = { cx - S(55), cy + S(40), cx + S(55), cy + S(74) };
        Button(dc, cb, L"Cancel", ID_DUPCANCEL, false);
        return;
    }
    if (g_dups.empty()) {
        RECT t = rcBody; t.bottom = (rcBody.top + rcBody.bottom) / 2;
        Text(dc, L"No duplicates found", t, g_fHuge, GOOD, DT_SINGLELINE | DT_BOTTOM | DT_CENTER);
        RECT t2 = rcBody; t2.top = t.bottom + S(8); t2.bottom = t2.top + S(22);
        Text(dc, L"Every file in here (and its subfolders) is unique.", t2, g_fUI, TEXT2, TC);
        return;
    }
    int rowH = S(32);
    int content = (int)g_dupRows.size() * rowH, view = rcBody.bottom - rcBody.top;
    g_dscroll = std::max(0, std::min(g_dscroll, std::max(0, content - view)));
    HRGN clip = CreateRectRgn(rcBody.left, rcBody.top, rcBody.right, rcBody.bottom);
    SelectClipRgn(dc, clip);
    for (int r = g_dscroll / rowH; r < (int)g_dupRows.size(); r++) {
        int y = rcBody.top + r * rowH - g_dscroll;
        if (y >= rcBody.bottom) break;
        int cy = y + rowH / 2;
        const DupGroup& g = g_dups[g_dupRows[r].first];
        int fi = g_dupRows[r].second;
        if (fi < 0) {
            if (r > 0) { RECT d = { L, y + S(4), R, y + S(5) }; FillC(dc, d, BORDER); }
            wchar_t t[160], a[32], b[32];
            FmtSize(g.files[0].size, a, 32); FmtSize(g.files[0].size * (g.files.size() - 1), b, 32);
            _snwprintf(t, 160, L"%d identical copies  \x00B7  %ls each  \x00B7  %ls extra", (int)g.files.size(), a, b); t[159] = 0;
            RECT tr = { L, y + S(6), R, y + rowH };
            Text(dc, t, tr, g_fSmallSemi, TEXT3, TL);
            continue;
        }
        const DupFile& f = g.files[fi];
        bool keep = fi == g.keep, rem = !keep && g.remove[fi];
        RECT row = { rcList.left + S(6), y + 1, rcList.right - S(14), y + rowH - 1 };
        bool hv = IsHover(ID_DUPROW, (u32)r) || IsHover(ID_DUPKEEP, (u32)r);
        if (hv && !keep) RoundC(dc, row, S(6), PANEL2);
        if (keep) {
            RECT k = { L, cy - S(8), L + S(16), cy + S(8) };
            RoundC(dc, k, S(16), GOOD);
        } else Check(dc, L, cy - S(8), rem, RGB(236, 98, 98), hv);
        size_t sl = f.rel.rfind(L'\\');
        std::wstring leaf = sl == std::wstring::npos ? f.rel : f.rel.substr(sl + 1);
        std::wstring where = sl == std::wstring::npos ? L"in this folder" : L"in " + f.rel.substr(0, sl) + L"\\";
        RECT n = { L + S(30), y, R - actW - sizeW - S(20), y + rowH };
        NameWithSuffix(dc, n, leaf, where, keep ? TEXT1 : (rem ? TEXT1 : TEXT2), TEXT3);
        wchar_t sz[32]; FmtSize(f.size, sz, 32);
        RECT s2 = { R - actW - sizeW - S(10), y, R - actW - S(16), y + rowH };
        Text(dc, sz, s2, g_fSmall, TEXT2, TR);
        RECT act = { R - actW, y, R, y + rowH };
        if (keep) {
            RECT pill = { act.left, cy - S(11), act.left + S(56), cy + S(11) };
            RoundC(dc, pill, S(11), Mix(PANEL, GOOD, 0.25));
            Text(dc, L"Keep", pill, g_fSmallSemi, TEXT1, TC);
        } else {
            RECT st = { act.left, y, act.left + S(70), y + rowH };
            Text(dc, rem ? L"Remove" : L"Leave", st, g_fSmallSemi, rem ? RGB(236, 98, 98) : TEXT3, TL);
            RECT kb = { act.left + S(76), cy - S(11), R, cy + S(11) };
            bool kh = IsHover(ID_DUPKEEP, (u32)r);
            if (kh) RoundC(dc, kb, S(6), PANEL3);
            Text(dc, L"Keep this", kb, g_fSmall, kh ? TEXT1 : ACCENT, TC);
            RECT kc = kb;
            kc.top = std::max(kc.top, rcBody.top); kc.bottom = std::min(kc.bottom, rcBody.bottom);
            RECT hit = row; hit.right = kb.left - S(2);
            hit.top = std::max(hit.top, rcBody.top); hit.bottom = std::min(hit.bottom, rcBody.bottom);
            if (hit.bottom > hit.top) AddClick(hit, ID_DUPROW, (u32)r);
            if (kc.bottom > kc.top) AddClick(kc, ID_DUPKEEP, (u32)r);
        }
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
    if (content > view) {
        RECT track = { rcList.right - S(10), rcBody.top + S(2), rcList.right - S(5), rcBody.bottom - S(2) };
        int th = std::max<int>(S(30), (int)((long long)(track.bottom - track.top) * view / content));
        int ty = track.top + (int)((long long)(track.bottom - track.top - th) * g_dscroll / (content - view));
        RECT t = { track.left, ty, track.right, ty + th };
        RoundC(dc, t, S(5), RGB(70, 76, 88));
    }
}

static void DrawFoot(HDC dc) {
    FillC(dc, rcFoot, PANEL);
    RECT line = { 0, rcFoot.top, rcFoot.right, rcFoot.top + 1 }; FillC(dc, line, BORDER);
    int cy = (rcFoot.top + rcFoot.bottom) / 2;
    int bx = rcFoot.right - S(20);
    wchar_t s1[160], s2[200], sz[32];

    if (g_view == V_DUPES) {
        int n = 0; u64 bytes = 0;
        for (auto& g : g_dups) for (int i = 0; i < (int)g.files.size(); i++) if (i != g.keep && g.remove[i]) { n++; bytes += g.files[i].size; }
        bool busy = DupBusy();
        wchar_t lab[96];
        if (n) _snwprintf(lab, 96, L"Remove %d cop%ls", n, n == 1 ? L"y" : L"ies"); else wcscpy(lab, L"Remove copies");
        lab[95] = 0;
        int bw = std::max<int>(S(170), TextW(dc, lab, g_fSemi) + S(40));
        RECT b = { bx - bw, cy - S(20), bx, cy + S(20) };
        Button(dc, b, lab, ID_DUPDELETE, true, n > 0 && !busy && g_dupState == DS_READY);
        RECT bk = { b.left - S(10) - S(150), cy - S(20), b.left - S(10), cy + S(20) };
        Button(dc, bk, L"\x2190  Back to sorting", ID_DUPBACK, false, !busy);
        FmtSize(bytes, sz, 32);
        if (busy) { wcscpy(s1, L"Working\x2026"); wcscpy(s2, L"This can take a while for big video files"); }
        else if (n) { _snwprintf(s1, 160, L"%d extra cop%ls ticked  \x00B7  %ls", n, n == 1 ? L"y" : L"ies", sz); wcscpy(s2, L"They'll go to the Recycle Bin \x2014 the copy marked Keep stays"); }
        else { wcscpy(s1, g_dups.empty() ? L"No duplicates" : L"Nothing ticked"); wcscpy(s2, L"Tick the copies you want removed"); }
        s1[159] = 0; s2[199] = 0;
        RECT t1 = { S(22), cy - S(19), bk.left - S(20), cy + S(1) };
        Text(dc, s1, t1, g_fSemi, TEXT1, TL);
        RECT t2 = { S(22), cy + S(1), bk.left - S(20), cy + S(19) };
        Text(dc, s2, t2, g_fSmall, TEXT2, TL);
        return;
    }

    int count = 0, folders = 0; u64 bytes = 0;
    std::set<std::wstring> dests;
    std::vector<bool> gseen(g_groups.size(), false);
    for (auto& e : g_list) if (WillMove(e)) {
        count++; bytes += e.size; dests.insert(LowerW(DestRel(e)));
        if (e.group >= 0 && !gseen[e.group]) { gseen[e.group] = true; for (int f : g_groups[e.group].folders) bytes += g_folders[f].size; }
    }
    folders = (int)dests.size();
    bool usable = g_warn != W_BLOCKED && g_warn != W_MISSING;
    bool can = count > 0 && usable;
    wchar_t lab[64];
    if (count) _snwprintf(lab, 64, L"Organize %d file%ls", count, count == 1 ? L"" : L"s");
    else wcscpy(lab, L"Organize");
    lab[63] = 0;
    int bw = std::max<int>(S(170), TextW(dc, lab, g_fSemi) + S(40));
    RECT b = { bx - bw, cy - S(20), bx, cy + S(20) };
    Button(dc, b, lab, ID_ORGANIZE, true, can);
    if (g_hasLog) {
        RECT u = { b.left - S(10) - S(90), cy - S(20), b.left - S(10), cy + S(20) };
        Button(dc, u, L"Undo", ID_UNDO, false);
        b = u;
    }
    RECT d = { b.left - S(10) - S(150), cy - S(20), b.left - S(10), cy + S(20) };
    Button(dc, d, L"Find duplicates", ID_DUPFIND, false, usable);
    b = d;
    FmtSize(bytes, sz, 32);
    if (count) {
        _snwprintf(s1, 160, L"%d file%ls  \x00B7  %ls", count, count == 1 ? L"" : L"s", sz);
        _snwprintf(s2, 200, g_mode == M_GATHER ? L"will be gathered into %d folder%ls here" :
                            (g_mode == M_EACH ? L"will be sorted into %d folder%ls, each inside its own folder" :
                                                L"will move into %d folder%ls inside this one"),
                   folders, folders == 1 ? L"" : L"s");
    } else {
        wcscpy(s1, g_list.empty() ? L"Nothing to move" : L"Nothing selected");
        wcscpy(s2, g_hasLog ? L"Undo puts back the last files that were organized here" : L"Tick a category on the left to sort it");
    }
    if (g_warn == W_BLOCKED) wcscpy(s2, L"Organizing is turned off for this folder");
    s1[159] = 0; s2[199] = 0;
    RECT t1 = { S(22), cy - S(19), b.left - S(20), cy + S(1) };
    Text(dc, s1, t1, g_fSemi, TEXT1, TL);
    RECT t2 = { S(22), cy + S(1), b.left - S(20), cy + S(19) };
    Text(dc, s2, t2, g_fSmall, TEXT2, TL);
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
        DrawBanner(mdc);
        DrawSide(mdc);
        DrawList(mdc);
        DrawFoot(mdc);
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob); DeleteObject(bb); DeleteDC(mdc);
    }
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- input
static void UpdateHover(int mx, int my) {
    int id = -1; u32 data = 0;
    POINT p = { mx, my };
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--)
        if (PtInRect(&g_clicks[i].rc, p)) { id = g_clicks[i].id; data = g_clicks[i].data; break; }
    if (id != g_hoverId || data != g_hoverData) {
        g_hoverId = id; g_hoverData = data;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void OnClick(int mx, int my) {
    POINT p = { mx, my };
    if (PtInRect(&g_thumb, p)) {
        g_dragThumb = true; g_dragY = my; g_dragScroll = g_scroll;
        SetCapture(g_hwnd);
        return;
    }
    for (int i = (int)g_clicks.size() - 1; i >= 0; i--) {
        if (!PtInRect(&g_clicks[i].rc, p)) continue;
        Click c = g_clicks[i];
        switch (c.id) {
        case ID_CHANGE: if (!DupBusy()) PickFolder(); break;
        case ID_OPEN: ShellExecuteW(NULL, L"open", L"explorer.exe", (L"\"" + g_dir + L"\"").c_str(), NULL, SW_SHOWNORMAL); break;
        case ID_ORGANIZE: Organize(); break;
        case ID_UNDO: Undo(); break;
        case ID_DISMISS: g_msg.clear(); break;
        case ID_DUPFIND: StartDupScan(); break;
        case ID_DUPBACK: if (!DupBusy()) { g_view = V_FILES; g_dupState = DS_IDLE; g_dups.clear(); g_dupRows.clear(); Rescan(true); } break;
        case ID_DUPCANCEL: InterlockedExchange(&g_dupCancel, 1); break;
        case ID_DUPDELETE: StartDupDelete(); break;
        case ID_DUPROW:
            if (c.data < g_dupRows.size()) {
                DupGroup& g = g_dups[g_dupRows[c.data].first];
                int fi = g_dupRows[c.data].second;
                if (fi >= 0 && fi != g.keep) g.remove[fi] = !g.remove[fi];
            }
            break;
        case ID_DUPKEEP:
            if (c.data < g_dupRows.size()) {
                DupGroup& g = g_dups[g_dupRows[c.data].first];
                int fi = g_dupRows[c.data].second;
                if (fi >= 0) { g.remove[g.keep] = 1; g.keep = fi; g.remove[fi] = 0; }
            }
            break;
        case ID_MODE: g_mode = (int)c.data; SaveMode(); g_scroll = 0; Rescan(true); break;
        case ID_CAT: g_catOn[c.data] = !g_catOn[c.data]; break;
        case ID_ROW:
            if (c.data < g_list.size()) {
                Entry& e = g_list[c.data];
                bool ns = !e.skip;
                if (e.group >= 0) for (int m : g_groups[e.group].entries) g_list[m].skip = ns;
                else e.skip = ns;
            }
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
    g_fUI = mk(13, FW_NORMAL);
    g_fSemi = mk(13, FW_SEMIBOLD);
    g_fSmall = mk(12, FW_NORMAL);
    g_fSmallSemi = mk(12, FW_SEMIBOLD);
    g_fTitle = mk(18, FW_SEMIBOLD);
    g_fHuge = mk(26, FW_LIGHT);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(860); mm->ptMinTrackSize.y = S(560);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_dragThumb) {
            int rowH = S(32);
            int content = RowCount() * rowH, view = rcBody.bottom - rcBody.top;
            int track = (rcBody.bottom - rcBody.top) - (g_thumb.bottom - g_thumb.top);
            if (track > 0 && content > view) g_scroll = g_dragScroll + (int)((long long)(y - g_dragY) * (content - view) / track);
            ClampScroll();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme); g_tracking = true;
        }
        UpdateHover(x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        if (g_hoverId != -1) { g_hoverId = -1; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, g_hoverId >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN:
        OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (g_dragThumb) { g_dragThumb = false; ReleaseCapture(); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        { POINT mp = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hwnd, &mp);
          if (PtInRect(&rcSide, mp) && g_view == V_FILES) { g_sscroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(42) / WHEEL_DELTA; InvalidateRect(hwnd, NULL, FALSE); return 0; } }
        if (g_view == V_DUPES) g_dscroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(32) * 3 / WHEEL_DELTA;
        else { g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(32) * 3 / WHEEL_DELTA; ClampScroll(); }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F5) { Rescan(true); InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_HOME) { g_scroll = 0; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_END) { g_scroll = 1 << 30; ClampScroll(); InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_NEXT || wp == VK_PRIOR) {
            g_scroll += (wp == VK_NEXT ? 1 : -1) * (rcBody.bottom - rcBody.top - S(32));
            ClampScroll(); InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (wp && !g_dir.empty() && g_scanMs < 400 && g_view == V_FILES) { Rescan(true); InvalidateRect(hwnd, NULL, FALSE); }   // pick up changes made in Explorer
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        if (DupBusy()) { DragFinish(drop); return 0; }
        wchar_t p[4096];
        if (DragQueryFileW(drop, 0, p, 4096)) {
            std::wstring s = p;
            if (!IsDir(s)) { size_t k = s.rfind(L'\\'); if (k != std::wstring::npos) s = s.substr(0, k == 2 ? 3 : k); }
            SetFolder(s);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_TIMER: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_APP_DUPDONE: OnDupDone(false); return 0;
    case WM_APP_DELDONE: OnDupDone(true); return 0;
    case WM_DESTROY:
        if (g_dupThread) { InterlockedExchange(&g_dupCancel, 1); WaitForSingleObject(g_dupThread, 5000); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HDC sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    MakeFonts();
    for (int k = 0; k < NK; k++) g_catOn[k] = kCat[k].defOn;
    LoadMode();

    wchar_t exe[4096]; GetModuleFileNameW(NULL, exe, 4096);
    g_self = exe;
    std::wstring start = g_self.substr(0, g_self.rfind(L'\\'));
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1 && argv[1][0]) start = argv[1];
    if (argv) LocalFree(argv);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    wc.lpszClassName = L"OrganizerWnd";
    RegisterClassExW(&wc);

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int W = std::min<int>(S(1180), (wa.right - wa.left) * 9 / 10);
    int H = std::min<int>(S(780), (wa.bottom - wa.top) * 9 / 10);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Organizer", WS_OVERLAPPEDWINDOW,
                             wa.left + ((wa.right - wa.left) - W) / 2, wa.top + ((wa.bottom - wa.top) - H) / 2,
                             W, H, NULL, NULL, hi, NULL);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    SetFolder(start);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return 0;
}
