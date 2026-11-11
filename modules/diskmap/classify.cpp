// modules/diskmap/classify.cpp — tables and rules ported verbatim from legacy DiskMap
// (docs/legacy/diskmap.md §3). Changing a list here changes what users see: keep it in step with
// the legacy notes, and send any change to the safe-to-delete text through kb-auditor (rule 7).
#include "modules/diskmap/classify.h"
#include <cstdio>
#include <cwchar>

namespace hx::diskmap {

namespace {

const wchar_t* const kCatName[NCAT] = {
    L"Games", L"Programs", L"Windows", L"Drivers", L"Video", L"Audio", L"Images",
    L"Documents", L"Archives & disk images", L"Developer", L"Temp & cache", L"Other"};

const COLORREF kCatColor[NCAT] = {
    RGB(236, 98, 98),   RGB(84, 146, 236),  RGB(140, 152, 172), RGB(172, 124, 236),
    RGB(238, 164, 66),  RGB(232, 100, 170), RGB(72, 196, 164),  RGB(226, 208, 96),
    RGB(180, 136, 96),  RGB(116, 204, 104), RGB(98, 112, 134),  RGB(120, 124, 134)};

// Reason texts are legacy-verbatim (\x2014 = em dash, \x2192 = arrow). The basis fields are new
// (approved deviation 7); they are pending kb-auditor.
const SafeInfo kSafe[NSAFE] = {
    {L"", L"", L"", L""},
    {L"Crash & error reports",
     L"Memory dumps and error reports Windows saves after a crash or driver reset (LiveKernelReports, Minidump, WER). Only useful for debugging \x2014 glance at the file names first to see what crashed.",
     L"reasoning", L"Windows writes these after a crash for later debugging; nothing reads them in normal operation."},
    {L"Temporary files",
     L"Scratch files that apps and installers left behind. Files an app still has open usually refuse to delete \x2014 skip those. Don't clear it while an install is running or waiting for a restart.",
     L"reasoning", L"A file held open is usually locked against deletion."},
    {L"Windows Update downloads",
     L"Downloaded update packages, plus the Delivery Optimization cache. Most are already installed; anything pending is downloaded again. Delivery Optimization files clear with Disk Cleanup \x2192 Clean up system files. For the Download folder, stop the Windows Update service first and don't clear it while an update waits for a restart.",
     L"reasoning", L"Windows Update keeps downloaded payloads after installing them and fetches them again if it needs them."},
    {L"Previous Windows install",
     L"Windows.old is your previous Windows install, kept so you can roll back; removing it means you can't. $Windows.~BT and ~WS are Setup's working folders: leave them alone while an upgrade is in progress or waiting for a restart. Use Disk Cleanup \x2192 Clean up system files \x2192 Previous Windows installation(s) / Temporary Windows installation files.",
     L"reasoning", L"Setup keeps Windows.old for rollback and uses $Windows.~BT/~WS as working space during upgrades."},
    {L"Recycle Bin",
     L"Files you already deleted. Empty the Recycle Bin to get this space back.",
     L"reasoning", L"Everything here is a file someone already chose to delete."},
    {L"App & browser caches",
     L"Folders named Cache, cache2, Code Cache or GPUCache under AppData. Browsers and most apps rebuild these. DiskMap skips caches inside installed programs (Local\\Programs, node_modules, site-packages), so it won't flag part of an app; still, close the app first \x2014 the next launch or page load may be slower.",
     L"reasoning", L"Matched by folder name (cache, cache2, Code Cache, GPUCache) under AppData, excluding per-user installs (Local\\Programs, node_modules, site-packages); not checked per app."},
    {L"Shader caches",
     L"Compiled GPU shaders. Games and drivers rebuild them; until they do, loads can be longer and gameplay can stutter, by an amount that depends on the game. Test it on a game you play.",
     L"reasoning", L"Drivers and games compile shaders on demand when no cached copy exists."},
    {L"Thumbnail & icon cache",
     L"Explorer's thumbnail and icon cache. Rebuilt automatically as you browse folders (Disk Cleanup can clear it too).",
     L"reasoning", L"Explorer regenerates thumbnails and icons from the files themselves."},
    {L"Driver installer leftovers",
     L"A root folder named AMD or NVIDIA where those driver installers unpack. The installed driver runs from Windows' driver store, not from here. DiskMap flags it only when it holds a Setup.exe or a Packages folder; check it's installer files and nothing of yours.",
     L"reasoning", L"The AMD and NVIDIA installers unpack to X:\\AMD and X:\\NVIDIA by default; flagged only when the folder holds setup.exe or a Packages folder."},
};

const wchar_t* const kR = L"reasoning";

// Directory rules, table order (first match wins). Legacy lines 81-97.
const std::vector<SafeRule> kDirRules = {
    {SF_DUMPS,   L"windows\\livekernelreports", kR, L"Kernel live dumps written after a driver or GPU reset."},
    {SF_DUMPS,   L"windows\\minidump", kR, L"Small crash dumps written after a bugcheck."},
    {SF_DUMPS,   L"programdata\\microsoft\\windows\\wer", kR, L"Windows Error Reporting queue and archive."},
    {SF_DUMPS,   L"users\\*\\appdata\\local\\crashdumps", kR, L"Per-user application crash dumps."},
    {SF_DUMPS,   L"users\\*\\appdata\\local\\microsoft\\windows\\wer", kR, L"Per-user Windows Error Reporting files."},
    {SF_TEMP,    L"windows\\temp", kR, L"System temp folder."},
    {SF_TEMP,    L"users\\*\\appdata\\local\\temp", kR, L"Per-user %TEMP%."},
    {SF_UPDATE,  L"windows\\softwaredistribution\\download", kR, L"Windows Update download cache; may include updates not yet installed."},
    {SF_UPDATE,  L"windows\\**\\deliveryoptimization\\cache", kR, L"Delivery Optimization peer cache."},
    {SF_OLDWIN,  L"windows.old", kR, L"Previous installation kept for rollback."},
    {SF_OLDWIN,  L"$windows.~bt", kR, L"Upgrade staging folder."},
    {SF_OLDWIN,  L"$windows.~ws", kR, L"Upgrade staging folder."},
    {SF_RECYCLE, L"$recycle.bin", kR, L"Recycle Bin of this drive."},
    {SF_SHADER,  L"users\\*\\appdata\\local\\d3dscache", kR, L"Direct3D shader cache."},
    {SF_SHADER,  L"users\\*\\appdata\\**\\dxcache", kR, L"Driver DirectX shader cache (folder name match)."},
    {SF_SHADER,  L"users\\*\\appdata\\**\\glcache", kR, L"Driver OpenGL shader cache (folder name match)."},
    {SF_SHADER,  L"**\\steamapps\\shadercache", kR, L"Steam's per-game shader pre-cache."},
    {SF_CACHE,   L"users\\*\\appdata\\local\\microsoft\\windows\\inetcache", kR, L"WinINet / Edge legacy web cache."},
    {SF_CACHE,   L"users\\*\\appdata\\**\\cache", kR,
     L"Any folder named exactly \"cache\" under AppData, except inside per-user installs (Local\\Programs, node_modules, site-packages).",
     L"users\\*\\appdata\\local\\programs\\**|**\\node_modules\\**|**\\site-packages\\**"},
    {SF_CACHE,   L"users\\*\\appdata\\**\\cache2", kR, L"Firefox-style disk cache (folder name match)."},
    {SF_CACHE,   L"users\\*\\appdata\\**\\code cache", kR, L"Chromium/Electron compiled-script cache (folder name match)."},
    {SF_CACHE,   L"users\\*\\appdata\\**\\gpucache", kR, L"Chromium/Electron GPU cache (folder name match)."},
    {SF_DRVPKG,  L"amd", kR, L"Root AMD folder holding setup.exe or a Packages folder (installer unpack).", nullptr, true},
    {SF_DRVPKG,  L"nvidia", kR, L"Root NVIDIA folder holding setup.exe or a Packages folder (installer unpack).", nullptr, true},
    {SF_DRVPKG,  L"programdata\\nvidia corporation\\downloader", kR, L"NVIDIA app/GeForce Experience driver downloads."},
};

// File rules. Legacy lines 98-102.
const std::vector<SafeRule> kFileRules = {
    {SF_DUMPS, L"windows\\memory.dmp", kR, L"Full kernel dump from the last bugcheck."},
    {SF_THUMB, L"users\\*\\appdata\\local\\microsoft\\windows\\explorer\\thumbcache_*.db", kR, L"Explorer thumbnail cache database."},
    {SF_THUMB, L"users\\*\\appdata\\local\\microsoft\\windows\\explorer\\iconcache_*.db", kR, L"Explorer icon cache database."},
};

// Directory-name lists, verbatim (legacy 358-375).
const wchar_t* const kTempDirs[] = {L"temp", L"tmp", L"cache", L"caches", L"cache2", L"code cache",
    L"gpucache", L"shadercache", L"dxcache", L"glcache", L"inetcache", L"crashdumps", L"crashpad",
    L"$recycle.bin", L"d3dscache", L"nv_cache", L"webcache", L"cachestorage", L"service worker",
    L"softwaredistribution", L"package cache", nullptr};
const wchar_t* const kGameDirs[] = {L"steamapps", L"steamlibrary", L"epic games", L"gog games",
    L"gog galaxy games", L"riot games", L"ea games", L"origin games", L"xboxgames", L"games", L"my games",
    L".minecraft", L"ubisoft game launcher", L"battlestate games", L"rockstar games", L"blizzard games",
    L"bethesda.net launcher", L"amazon games", L"itch", L"wargaming.net", L"square enix", nullptr};
const wchar_t* const kRootWin[] = {L"windows", L"windows.old", L"system volume information",
    L"recovery", L"$windows.~bt", L"$windows.~ws", L"$winreagent", L"$sysreset", L"boot", L"efi",
    L"perflogs", L"$getcurrent", L"onedrivetemp", nullptr};
const wchar_t* const kRootProg[] = {L"program files", L"program files (x86)", L"programdata", nullptr};
const wchar_t* const kRootDrv[] = {L"amd", L"nvidia", L"intel", L"drivers", L"swsetup", L"dell",
    L"realtek", nullptr};
const wchar_t* const kDevDirs[] = {L"node_modules", L".git", L"venv", L".venv", L"__pycache__",
    L".cargo", L".rustup", L".nuget", L".gradle", L".m2", L"site-packages", L".vscode", L".android",
    L"android", L"sdk", L"miniconda3", L"anaconda3", L".conda", L"source", L"repos", L"msys64",
    L"cygwin64", L".ollama", L"huggingface", L".cache", nullptr};

// Extension lists, verbatim (legacy 399-420). Checked in FileCat's order, not this one.
const wchar_t* const kVideo[] = {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"m4v", L"flv",
    L"mpg", L"mpeg", L"ts", L"m2ts", L"3gp", L"vob", nullptr};
const wchar_t* const kAudio[] = {L"mp3", L"flac", L"wav", L"ogg", L"m4a", L"aac", L"opus", L"wma",
    L"aiff", L"alac", L"mid", L"midi", nullptr};
const wchar_t* const kImage[] = {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"heif",
    L"tif", L"tiff", L"raw", L"cr2", L"cr3", L"nef", L"arw", L"dng", L"psd", L"svg", L"ico", L"avif",
    L"kra", L"xcf", nullptr};
const wchar_t* const kDocs[] = {L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt",
    L"md", L"odt", L"ods", L"odp", L"rtf", L"csv", L"epub", L"mobi", L"pages", L"one", L"pst", L"ost",
    nullptr};
const wchar_t* const kArch[] = {L"zip", L"rar", L"7z", L"tar", L"gz", L"tgz", L"bz2", L"xz",
    L"zst", L"iso", L"img", L"vhd", L"vhdx", L"vmdk", L"vdi", L"qcow2", L"wim", L"esd", L"cab", L"dmg",
    nullptr};
const wchar_t* const kDev[] = {L"c", L"cpp", L"cc", L"h", L"hpp", L"py", L"js", L"ts", L"jsx",
    L"tsx", L"java", L"cs", L"go", L"rs", L"rb", L"php", L"lua", L"sln", L"vcxproj", L"pdb", L"obj",
    L"o", L"lib", L"a", L"ipynb", L"pt", L"pth", L"safetensors", L"gguf", L"ckpt", L"onnx", L"bin",
    L"whl", L"jar", nullptr};
const wchar_t* const kProg[] = {L"exe", L"dll", L"msi", L"msix", L"appx", L"msp", L"ocx", nullptr};
const wchar_t* const kDrv[] = {L"sys", L"inf", L"cat", L"drv", nullptr};
const wchar_t* const kTmp[] = {L"tmp", L"temp", L"log", L"dmp", L"etl", L"bak", L"old",
    L"crdownload", L"part", nullptr};

bool In(const wchar_t* s, const wchar_t* const* list) {
    for (; *list; ++list) if (wcscmp(s, *list) == 0) return true;
    return false;
}

}  // namespace

const wchar_t* CatName(int c) { return (c >= 0 && c < NCAT) ? kCatName[c] : L""; }
COLORREF CatColor(int c) { return (c >= 0 && c < NCAT) ? kCatColor[c] : kCatColor[C_OTHER]; }
const SafeInfo& SafeReason(int r) { return (r > 0 && r < NSAFE) ? kSafe[r] : kSafe[0]; }
const std::vector<SafeRule>& SafeDirRules() { return kDirRules; }
const std::vector<SafeRule>& SafeFileRules() { return kFileRules; }

std::vector<std::wstring> SplitPattern(const wchar_t* p) {
    std::vector<std::wstring> v; std::wstring cur;
    for (; *p; p++) { if (*p == L'\\') { v.push_back(cur); cur.clear(); } else cur += *p; }
    v.push_back(cur);
    return v;
}

bool Glob(const wchar_t* p, const wchar_t* s) {
    if (*p == 0) return *s == 0;
    if (*p == L'*') { for (;; s++) { if (Glob(p + 1, s)) return true; if (!*s) return false; } }
    return *s && *p == *s && Glob(p + 1, s + 1);
}

bool MatchSegs(const std::vector<std::wstring>& p, size_t pi, const std::vector<std::wstring>& s, size_t si) {
    if (pi == p.size()) return si == s.size();
    if (p[pi] == L"**") {
        for (size_t k = si; k <= s.size(); k++) if (MatchSegs(p, pi + 1, s, k)) return true;
        return false;
    }
    if (si == s.size()) return false;
    return Glob(p[pi].c_str(), s[si].c_str()) && MatchSegs(p, pi + 1, s, si + 1);
}

bool IsInstallerMarker(const std::wstring& ln, bool isDir) {
    return isDir ? ln == L"packages" : ln == L"setup.exe";
}

Rules::Rules() {
    for (auto& r : kDirRules) {
        Compiled c{r.reason, SplitPattern(r.pattern), {}, r.needsInstallerMarker};
        if (r.exclude) {
            std::wstring all = r.exclude;
            size_t start = 0;
            for (;;) {
                size_t bar = all.find(L'|', start);
                c.exclude.push_back(SplitPattern(all.substr(start, bar - start).c_str()));
                if (bar == std::wstring::npos) break;
                start = bar + 1;
            }
        }
        dir_.push_back(std::move(c));
    }
    for (auto& r : kFileRules) {
        file_.push_back({r.reason, SplitPattern(r.pattern), {}, false});
        size_t d = file_.back().segs.size() - 1;   // depth of the folder that holds the file
        if (d < 64) fileDepth_[d] = true;
    }
}

const Rules& Rules::Get() {
    static const Rules r;   // C++11 magic static: thread-safe init; read-only afterwards
    return r;
}

int Rules::DirRuleIndex(const std::vector<std::wstring>& segs, bool marker) const {
    for (size_t i = 0; i < dir_.size(); i++) {
        const Compiled& c = dir_[i];
        if (c.marker && !marker) continue;
        if (!MatchSegs(c.segs, 0, segs, 0)) continue;
        bool excluded = false;
        for (auto& ex : c.exclude) if (MatchSegs(ex, 0, segs, 0)) { excluded = true; break; }
        if (!excluded) return (int)i;
    }
    return -1;
}

u8 Rules::DirSafe(const std::vector<std::wstring>& segs, bool marker) const {
    int i = DirRuleIndex(segs, marker);
    return i < 0 ? (u8)SF_NONE : dir_[i].reason;
}

bool Rules::NeedsInstallerMarker(const std::vector<std::wstring>& segs) const {
    return DirRuleIndex(segs, false) < 0 && DirRuleIndex(segs, true) >= 0;
}

u8 Rules::FileSafe(std::vector<std::wstring>& segs, const std::wstring& lname) const {
    if (segs.size() >= 64 || !fileDepth_[segs.size()]) return SF_NONE;
    segs.push_back(lname);
    u8 res = SF_NONE;
    for (auto& r : file_) if (MatchSegs(r.segs, 0, segs, 0)) { res = r.reason; break; }
    segs.pop_back();
    return res;
}

std::wstring LowerName(const wchar_t* s, size_t n) {
    std::wstring out(s, n);
    if (n) CharLowerBuffW(&out[0], (DWORD)n);
    return out;
}

u8 DirCtx(u8 pc, const std::wstring& ln, int depth) {
    const wchar_t* n = ln.c_str();
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

u8 FileCat(u8 ctx, const std::wstring& ln, int depth) {
    if (ctx != NOCTX) return ctx;
    const wchar_t* n = ln.c_str();
    if (depth == 0) {
        if (!wcscmp(n, L"pagefile.sys") || !wcscmp(n, L"hiberfil.sys") || !wcscmp(n, L"swapfile.sys") ||
            !wcscmp(n, L"dumpstack.log") || !wcscmp(n, L"dumpstack.log.tmp") || !wcscmp(n, L"bootmgr"))
            return C_WINDOWS;
    }
    const wchar_t* dot = wcsrchr(n, L'.');
    if (!dot || !dot[1]) return C_OTHER;
    const wchar_t* e = dot + 1;
    if (In(e, kVideo)) return C_VIDEO;      // before Developer: ".ts" is Video (legacy order)
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

std::wstring U64Str(u64 v) {
    wchar_t tmp[40]; int n = 0;
    if (v == 0) tmp[n++] = L'0';
    int digits = 0;
    while (v) {
        if (digits && digits % 3 == 0) tmp[n++] = L',';
        tmp[n++] = (wchar_t)(L'0' + v % 10); v /= 10; digits++;
    }
    std::wstring out;
    for (int i = n - 1; i >= 0; i--) out += tmp[i];
    return out;
}

std::wstring FmtSize(u64 b) {
    static const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    if (b < 1024) return U64Str(b) + L" B";
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    wchar_t out[32];
    swprintf(out, 32, v < 10 ? L"%.2f %ls" : (v < 100 ? L"%.1f %ls" : L"%.0f %ls"), v, u[i]);
    return out;
}

}  // namespace hx::diskmap
