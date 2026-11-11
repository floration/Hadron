// modules/bios/db.h — settings.db (v1 and v2) model and the value/advice rules from legacy bsx.cpp.
//
// Format: docs/legacy/bios-explorer.md §2 (v1) and docs/design/bios.md "settings.db v2" (B line, K fields
// 15-17). Everything here is pure data + rules, no Win32 beyond string conversion, so the selftest can
// exercise it headless.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hx::bios {

constexpr int kGoalCount = 5;
extern const wchar_t* const kGoalNames[kGoalCount];   // Power saving, Balanced, Single-core, Multi-core, Low latency

struct NumberSource { std::wstring number, kind, ref; };   // K field 17, one per number in the text

struct Kb {
    std::wstring title, expl, rec[kGoalCount], note, fx[3], claim;
    std::wstring basis, basisNote;          // v2 fields 15/16; empty = "not stated"
    std::vector<NumberSource> sources;      // v2 field 17
};

struct Option { long long v = 0; std::wstring t; };

struct Setting {
    int id = 0;
    std::wstring var, guid;                 // guid lowercase, no braces (as in the db)
    int offset = 0, width = 1;
    std::wstring kind;                      // oneof / numeric / checkbox
    bool hidden = false, cond = false;
    bool hasDef = false; long long def = 0;
    bool hasMin = false; long long mn = 0, mx = 0, step = 0;
    std::wstring prompt, help, path, ctx;
    std::vector<Option> opts;
    int kb = -1;
    // Values. cur = the primary source (live, or a snapshot when nothing live); snap = a snapshot
    // compared against live values.
    bool hasCur = false; long long cur = 0;
    bool hasSnap = false; long long snap = 0;
    mutable signed char zeroAuto = -1;      // ZeroIsAuto cache
};

// Variable data keyed by exact name + lowercase GUID (rule fix #21: names are case-sensitive).
using VarKey = std::pair<std::wstring, std::wstring>;
using VarMap = std::map<VarKey, std::vector<uint8_t>>;
VarKey MakeVarKey(const std::wstring& name, const std::wstring& guid);   // strips braces, lowercases guid

class Db {
public:
    bool Parse(const std::string& utf8, std::wstring* err);
    void AddContext();

    int format = 1;                         // 2 when a B line or 17-field K lines are present
    std::wstring title, board, bios;        // V; B board product + BIOS version (empty on v1)
    int keyCount = 1 << 30;                 // T; legacy default when the line is missing
    std::vector<Kb> K;
    std::vector<Setting> S;

    const Kb* KbOf(const Setting& s) const { return (s.kb >= 0 && s.kb < (int)K.size()) ? &K[s.kb] : nullptr; }
    bool IsKey(const Setting& s) const { return s.kb >= 0 && s.kb < keyCount && s.kb < (int)K.size(); }

    // Distinct (var, guid) pairs the map uses, in first-use order.
    std::vector<std::pair<std::wstring, std::wstring>> Variables() const;

    // Decodes every setting from vars into cur (snapshot=false) or snap (true). Returns how many
    // settings got a value.
    int Apply(const VarMap& vars, bool snapshot);
    void ClearValues(bool snapshot);
};

std::wstring FromUtf8(const std::string& s);
std::string  ToUtf8(const std::wstring& s);
std::wstring Unesc(const std::wstring& s);
std::wstring Lower(std::wstring s);
int  ICmp(const std::wstring& a, const std::wstring& b);   // case-insensitive, like _wcsicmp

// Little-endian, width bytes at offset, into a signed long long (legacy). False if out of range.
bool DecodeValue(const std::vector<uint8_t>& data, int offset, int width, long long* out);

// Legacy §4.12 / §4.6-4.8 rules.
bool ZeroIsAuto(const Setting& s);
std::wstring ValText(const Setting& s, long long v);
inline std::wstring CurText(const Setting& s) { return s.hasCur ? ValText(s, s.cur) : L"-"; }
inline std::wstring DefText(const Setting& s) { return s.hasDef ? ValText(s, s.def) : L"-"; }
inline std::wstring SnapText(const Setting& s) { return s.hasSnap ? ValText(s, s.snap) : L"-"; }
inline bool Changed(const Setting& s) { return s.hasCur && s.hasDef && s.cur != s.def; }
inline bool SnapDiffers(const Setting& s) { return s.hasCur != s.hasSnap || (s.hasCur && s.cur != s.snap); }
bool RecVal(const Db& db, const Setting& s, int goal, long long* v);
std::wstring RecText(const Db& db, const Setting& s, int goal);
inline bool Differs(const Db& db, const Setting& s, int goal) {
    long long v; return s.hasCur && RecVal(db, s, goal, &v) && v != s.cur;
}
// 0 change this, 1 advice text, 2 already matches, 3 exact rec but current unknown, 4 no advice.
int GoalRank(const Db& db, const Setting& s, int goal);

// Basis display (rule 7). "Not stated" when the field is empty (v1 files and the temporary v2 file).
std::wstring BasisLabel(const std::wstring& basis);

// Version guard (spec "Version guard"). Board: the SMBIOS type-2 product contains the B board id as a
// token delimited by whitespace, parentheses or the string edges (MSI reports "PRO Z790-P WIFI (MS-7E06)"). BIOS: equal ignoring case, or MSI's mapping between the
// download name and the SMBIOS type-0 version ("7E06vAF" <-> "A.F0"). reported may be a whole line
// (system.txt "BIOS: <vendor> <version> <date>"): any whitespace-separated token can match.
bool BoardMatches(const std::wstring& dbBoard, const std::wstring& reported);
bool BiosMatches(const std::wstring& dbBios, const std::wstring& reported);

// Export text (legacy §4.11). unverified adds a warning line under the header.
std::wstring ExportText(const Db& db, int goal, const std::wstring& source, const std::wstring& unverified);

}  // namespace hx::bios
