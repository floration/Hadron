// modules/diskmap/classify.h — file categories, directory context and the safe-to-delete rules.
//
// Ported verbatim from legacy DiskMap (docs/legacy/diskmap.md §3): the 12 categories and their
// colours, the directory-context rules, the extension lists, and the 9 safe-to-delete reasons with
// their 25 directory rules and 3 file rules. Every name passed in here is already lower-cased with
// CharLowerBuffW (LowerName). Pure functions over strings; no I/O, so --selftest can check them.
#pragma once

#include "core/sys/win_compat.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hx::diskmap {

using u64 = uint64_t;
using u32 = uint32_t;
using u8  = uint8_t;

// ---- categories --------------------------------------------------------------------------------
enum Cat : u8 {
    C_GAMES, C_PROGRAMS, C_WINDOWS, C_DRIVERS, C_VIDEO, C_AUDIO, C_IMAGES, C_DOCS,
    C_ARCHIVES, C_DEV, C_TEMP, C_OTHER,
};
constexpr int NCAT = 12;
constexpr u8  NOCTX = 255;   // "this folder sets no category; classify its files by extension"

const wchar_t* CatName(int c);
COLORREF       CatColor(int c);

// ---- safe-to-delete ----------------------------------------------------------------------------
enum Safe : u8 {
    SF_NONE, SF_DUMPS, SF_TEMP, SF_UPDATE, SF_OLDWIN, SF_RECYCLE, SF_CACHE, SF_SHADER, SF_THUMB, SF_DRVPKG,
};
constexpr int NSAFE = 10;   // index 0 is "not flagged"
constexpr COLORREF SAFE_COL = RGB(80, 214, 140);

// CLAUDE.md rule 7: every claim carries a basis. These reasons contain no numbers, so they are all
// `reasoning` (from how Windows and the apps use the folders) until kb-auditor upgrades any of them
// to `published` with a URL. basisNote says what the reasoning is, and where the rule is broad.
struct SafeInfo {
    const wchar_t* name;
    const wchar_t* desc;       // user-facing text, verbatim from legacy
    const wchar_t* basis;      // "reasoning" | "published" | "owner-measured" | "folklore"
    const wchar_t* basisNote;  // one line: why we believe it
};
const SafeInfo& SafeReason(int r);   // r in 1..9; 0 returns an empty entry

struct SafeRule {
    u8 reason;
    const wchar_t* pattern;    // drive-root relative, lower case, `\` separated; `*` in-segment, `**` = 0+ segments
    const wchar_t* basis;
    const wchar_t* basisNote;
    const wchar_t* exclude = nullptr;   // '|'-separated patterns: a folder matching any of them is NOT flagged
    bool needsInstallerMarker = false;  // flag only if the folder holds setup.exe or a Packages folder
};
const std::vector<SafeRule>& SafeDirRules();
const std::vector<SafeRule>& SafeFileRules();

// Compiled rule tables. Build once (Rules::Get()) and use from any thread: read-only after build.
class Rules {
public:
    static const Rules& Get();
    // segs: lower-cased, root-relative path segments of a directory (root = empty). First match wins;
    // a rule whose exclusion matches, or that needs an installer marker the folder lacks, is skipped.
    // hasInstallerMarker: setup.exe or a Packages folder was found anywhere inside (IsInstallerMarker).
    u8 DirSafe(const std::vector<std::wstring>& segs, bool hasInstallerMarker = false) const;
    // True when the folder would be flagged if it held an installer marker but isn't without one: the
    // scanner then decides after the scan, from its in-memory subtree (Scan::MarkInstallerFolders).
    bool NeedsInstallerMarker(const std::vector<std::wstring>& segs) const;
    // A file in an unflagged directory: only tried at depths a file rule can match (legacy g_fileDepth).
    u8 FileSafe(std::vector<std::wstring>& segs, const std::wstring& lname) const;
    // The rule (table index) that DirSafe matched, for showing its basis; -1 when none.
    int DirRuleIndex(const std::vector<std::wstring>& segs, bool hasInstallerMarker = false) const;

private:
    Rules();
    struct Compiled {
        u8 reason; std::vector<std::wstring> segs;
        std::vector<std::vector<std::wstring>> exclude; bool marker;
    };
    std::vector<Compiled> dir_, file_;
    bool fileDepth_[64] = {};
};

// Glob and segment matching, exposed for --selftest.
bool Glob(const wchar_t* p, const wchar_t* s);
bool MatchSegs(const std::vector<std::wstring>& p, size_t pi, const std::vector<std::wstring>& s, size_t si);
std::vector<std::wstring> SplitPattern(const wchar_t* p);

// Installer-unpack marker for the root AMD/NVIDIA rule: a file setup.exe or a folder Packages anywhere
// in the folder's subtree (lname lower-cased; links never count).
bool IsInstallerMarker(const std::wstring& lname, bool isDir);

// ---- classification ----------------------------------------------------------------------------
// Lower-case with CharLowerBuffW (legacy Lower; no 260-char truncation, names are short anyway).
std::wstring LowerName(const wchar_t* s, size_t n);
inline std::wstring LowerName(const std::wstring& s) { return LowerName(s.c_str(), s.size()); }

// Context of a sub-folder from its parent's context, its lower-case name, and depth (1 = root child).
u8 DirCtx(u8 parentCtx, const std::wstring& lname, int depth);
// Category of a file: folder context wins, then root system files (dirDepth 0), then the extension.
u8 FileCat(u8 dirCtx, const std::wstring& lname, int dirDepth);

// ---- formats (legacy FmtSize / U64Str; English-only, `,` and `.` fixed) ------------------------
std::wstring U64Str(u64 v);
std::wstring FmtSize(u64 v);

}  // namespace hx::diskmap
