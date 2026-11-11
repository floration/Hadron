// modules/bios/db.cpp — see db.h. Rules ported from legacy bsx.cpp (line refs in docs/legacy/bios-explorer.md).
#include "core/sys/win_compat.h"
#include "modules/bios/db.h"
#include <cwchar>
#include <cwctype>
#include <set>

namespace hx::bios {

const wchar_t* const kGoalNames[kGoalCount] = {L"Power saving", L"Balanced", L"Single-core", L"Multi-core",
                                               L"Low latency"};

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string o((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n, nullptr, nullptr);
    return o;
}

// gen.py esc inverse (legacy 56-63): "\n" -> newline, "\x" -> x, a trailing lone "\" is kept.
std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) { wchar_t c = s[++i]; o += (c == L'n') ? L'\n' : c; }
        else o += s[i];
    }
    return o;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

int ICmp(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()); }

namespace {
std::vector<std::wstring> Split(const std::wstring& s, wchar_t d) {
    std::vector<std::wstring> r;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); i++)
        if (i == s.size() || s[i] == d) { r.push_back(s.substr(a, i - a)); a = i + 1; }
    return r;
}
long long ToLL(const std::wstring& s) { return s.empty() ? 0 : _wtoi64(s.c_str()); }
}  // namespace

VarKey MakeVarKey(const std::wstring& name, const std::wstring& guid) {
    std::wstring g = guid;
    if (!g.empty() && g.front() == L'{') g.erase(0, 1);
    if (!g.empty() && g.back() == L'}') g.pop_back();
    return {name, Lower(g)};
}

bool Db::Parse(const std::string& utf8, std::wstring* err) {
    K.clear(); S.clear(); title.clear(); board.clear(); bios.clear();
    keyCount = 1 << 30;
    format = 1;
    std::wstring all = FromUtf8(utf8);
    if (all.empty()) { if (err) *err = L"settings.db is empty or not UTF-8"; return false; }
    for (auto& line : Split(all, L'\n')) {
        if (line.size() < 2) continue;
        auto f = Split(line, L'\t');
        if (f[0] == L"V" && f.size() >= 2) title = Unesc(f[1]);
        else if (f[0] == L"B" && f.size() >= 3) { board = Unesc(f[1]); bios = Unesc(f[2]); format = 2; }
        else if (f[0] == L"T" && f.size() >= 2) keyCount = (int)ToLL(f[1]);
        else if (f[0] == L"K" && f.size() >= 10) {
            Kb k;
            k.title = Unesc(f[2]); k.expl = Unesc(f[3]);
            for (int g = 0; g < kGoalCount; g++) k.rec[g] = Unesc(f[4 + g]);
            k.note = Unesc(f[9]);
            if (f.size() >= 14) { for (int j = 0; j < 3; j++) k.fx[j] = Unesc(f[10 + j]); k.claim = Unesc(f[13]); }
            if (f.size() >= 17) {
                format = 2;
                k.basis = Unesc(f[14]);
                k.basisNote = Unesc(f[15]);
                if (!f[16].empty()) {
                    for (auto& e : Split(f[16], 0x1e)) {
                        auto p = Split(e, 0x1f);
                        if (p.size() == 3) k.sources.push_back({Unesc(p[0]), Unesc(p[1]), Unesc(p[2])});
                    }
                }
            }
            K.push_back(std::move(k));
        } else if (f[0] == L"S" && f.size() >= 17) {
            Setting s;
            s.id = (int)ToLL(f[1]); s.var = f[2]; s.guid = Lower(f[3]);
            s.offset = (int)ToLL(f[4]); s.width = (int)ToLL(f[5]);
            s.kind = f[6];
            s.hidden = f[7].find(L'H') != std::wstring::npos;
            s.cond = f[7].find(L'C') != std::wstring::npos;
            s.hasDef = !f[8].empty(); s.def = ToLL(f[8]);
            s.hasMin = !f[9].empty(); s.mn = ToLL(f[9]); s.mx = ToLL(f[10]); s.step = ToLL(f[11]);
            s.prompt = Unesc(f[12]); s.help = Unesc(f[13]); s.path = Unesc(f[14]);
            if (!f[15].empty()) {
                for (auto& o : Split(f[15], 0x1e)) {
                    auto kv = Split(o, 0x1f);
                    if (kv.size() == 2) s.opts.push_back({ToLL(kv[0]), Unesc(kv[1])});
                }
            }
            s.kb = (int)ToLL(f[16]);
            S.push_back(std::move(s));
        }
    }
    // Edge case §6: S field 16 was never bounds-checked; a bad index now means "no KB entry".
    for (auto& s : S) if (s.kb < 0 || s.kb >= (int)K.size()) s.kb = -1;
    if (S.empty()) { if (err) *err = L"settings.db has no settings (S lines)"; return false; }
    return true;
}

// Legacy AddContext (72-79): label repeated prompts with the last segment of their first menu path.
void Db::AddContext() {
    std::map<std::wstring, int> cnt;
    for (auto& s : S) cnt[s.prompt]++;
    for (auto& s : S) {
        s.ctx.clear();
        if (cnt[s.prompt] > 1 && !s.path.empty()) {
            std::wstring p = s.path.substr(0, s.path.find(L" | "));
            size_t k = p.rfind(L" > ");
            s.ctx = k == std::wstring::npos ? p : p.substr(k + 3);
            if (s.ctx == s.prompt) s.ctx.clear();
        }
    }
}

std::vector<std::pair<std::wstring, std::wstring>> Db::Variables() const {
    std::vector<std::pair<std::wstring, std::wstring>> v;
    std::set<VarKey> seen;
    for (auto& s : S) {
        VarKey k{s.var, s.guid};
        if (seen.insert(k).second) v.push_back(k);
    }
    return v;
}

bool DecodeValue(const std::vector<uint8_t>& data, int offset, int width, long long* out) {
    if (offset < 0 || width < 1 || width > 8 || (size_t)offset + (size_t)width > data.size()) return false;
    unsigned long long v = 0;
    for (int i = 0; i < width; i++) v |= (unsigned long long)data[(size_t)offset + i] << (8 * i);
    *out = (long long)v;   // legacy: unsigned bytes into a signed long long (width 8 can go negative)
    return true;
}

int Db::Apply(const VarMap& vars, bool snapshot) {
    int n = 0;
    for (auto& s : S) {
        bool& has = snapshot ? s.hasSnap : s.hasCur;
        long long& val = snapshot ? s.snap : s.cur;
        has = false; val = 0;
        auto it = vars.find(VarKey{s.var, s.guid});
        if (it == vars.end()) continue;
        if (DecodeValue(it->second, s.offset, s.width, &val)) { has = true; n++; }
        else val = 0;
    }
    return n;
}

void Db::ClearValues(bool snapshot) {
    for (auto& s : S) {
        if (snapshot) { s.hasSnap = false; s.snap = 0; }
        else { s.hasCur = false; s.cur = 0; }
    }
}

// Legacy ZeroIsAuto (173-181).
bool ZeroIsAuto(const Setting& s) {
    if (s.zeroAuto >= 0) return s.zeroAuto != 0;
    std::wstring h = Lower(s.help);
    bool r = false;
    static const wchar_t* const pats[] = {L"0: auto", L"0 = auto", L"0=auto", L"0 means auto", L"0 - auto",
                                          L"0 = no override", L"0- auto", L"0 - mrc auto", L"0-auto"};
    for (auto p : pats) if (h.find(p) != std::wstring::npos) { r = true; break; }
    if (!r && s.kind == L"numeric" && !s.var.empty() &&
        (s.path.find(L"DRAM") != std::wstring::npos || s.path.find(L"XMP User Profile") != std::wstring::npos ||
         s.path.find(L"Memory Overclocking") != std::wstring::npos) && s.mn == 0)
        r = true;   // MSI memory timing fields: 0 = Auto (trained at boot, not stored)
    s.zeroAuto = r ? 1 : 0;
    return r;
}

std::wstring ValText(const Setting& s, long long v) {
    for (auto& o : s.opts) if (o.v == v) return o.t.empty() ? std::to_wstring(v) : o.t;
    if (v == 0 && s.opts.empty() && ZeroIsAuto(s)) return L"Auto";
    return std::to_wstring(v);
}

// Legacy RecVal (193-200): an exact rec is a case-insensitive prefix of some option's text.
bool RecVal(const Db& db, const Setting& s, int goal, long long* v) {
    const Kb* k = db.KbOf(s);
    if (!k || goal < 0 || goal >= kGoalCount) return false;
    const std::wstring& r = k->rec[goal];
    if (r.empty() || r[0] == L'~') return false;
    std::wstring rl = Lower(r);
    for (auto& o : s.opts) {
        std::wstring ol = Lower(o.t);
        if (ol.compare(0, rl.size(), rl) == 0) { *v = o.v; return true; }
    }
    return false;
}

std::wstring RecText(const Db& db, const Setting& s, int goal) {
    const Kb* k = db.KbOf(s);
    if (!k || goal < 0 || goal >= kGoalCount) return L"";
    std::wstring r = k->rec[goal];
    if (!r.empty() && r[0] == L'~') r = r.substr(1);
    long long v;
    if (RecVal(db, s, goal, &v)) return ValText(s, v);
    return r;
}

int GoalRank(const Db& db, const Setting& s, int goal) {
    const Kb* k = db.KbOf(s);
    if (goal < 0 || !k || k->rec[goal].empty()) return 4;
    long long v;
    if (RecVal(db, s, goal, &v)) return !s.hasCur ? 3 : (v != s.cur ? 0 : 2);
    return 1;
}

std::wstring BasisLabel(const std::wstring& b) {
    if (b == L"owner-measured") return L"Owner-measured";
    if (b == L"published") return L"Published";
    if (b == L"reasoning") return L"Reasoning (from how the hardware works, no data)";
    if (b == L"folklore") return L"Folklore (repeated, no evidence)";
    if (b.empty()) return L"Not stated";
    return b;
}

bool BoardMatches(const std::wstring& dbBoard, const std::wstring& reported) {
    if (dbBoard.empty() || reported.empty()) return false;
    // A delimited token: bounded by whitespace, parentheses or the string edges, so "MS-7E06" matches
    // "PRO Z790-P WIFI (MS-7E06)" but not "MS-7E06x" or "XMS-7E06".
    std::wstring r = Lower(reported), b = Lower(dbBoard);
    auto delim = [](wchar_t c) { return iswspace(c) || c == L'(' || c == L')'; };
    for (size_t pos = r.find(b); pos != std::wstring::npos; pos = r.find(b, pos + 1)) {
        bool left = pos == 0 || delim(r[pos - 1]);
        size_t e = pos + b.size();
        bool right = e == r.size() || delim(r[e]);
        if (left && right) return true;
    }
    return false;
}

bool BiosMatches(const std::wstring& dbBios, const std::wstring& reported) {
    if (dbBios.empty() || reported.empty()) return false;
    // MSI: "<4-char board>v<X><Y>" is the download name of SMBIOS version "X.Y0".
    std::wstring msi;
    if (dbBios.size() == 7 && (dbBios[4] == L'v' || dbBios[4] == L'V'))
        msi = Lower(std::wstring(1, dbBios[5]) + L"." + dbBios[6] + L"0");
    std::wstring want = Lower(dbBios);
    size_t a = 0;
    while (a < reported.size()) {
        while (a < reported.size() && iswspace(reported[a])) a++;
        size_t b = a;
        while (b < reported.size() && !iswspace(reported[b])) b++;
        std::wstring tok = Lower(reported.substr(a, b - a));
        if (!tok.empty() && (tok == want || (!msi.empty() && tok == msi))) return true;
        a = b;
    }
    return false;
}

// Rule 7 in the export: the basis of the KB entry behind a line, then the field-17 source of every
// number that appears in the text printed for it.
static std::wstring BasisLines(const Kb& k, const std::wstring& printed) {
    std::wstring o = L"    basis: " + BasisLabel(k.basis) + (k.basisNote.empty() ? L"" : L": " + k.basisNote) + L"\r\n";
    std::set<std::wstring> done;
    for (auto& src : k.sources) {
        if (src.number.empty() || printed.find(src.number) == std::wstring::npos) continue;
        std::wstring line = L"    source: " + src.number + L" \u2014 " + src.kind + L" \u2014 " + src.ref + L"\r\n";
        if (done.insert(line).second) o += line;
    }
    return o;
}

// Legacy Export (266-295) content; the caller writes it (Save dialog, UTF-8 BOM).
std::wstring ExportText(const Db& db, int goal, const std::wstring& source, const std::wstring& unverified) {
    if (goal < 0 || goal >= kGoalCount) return L"";
    std::wstring o = L"BIOS changes for goal: " + std::wstring(kGoalNames[goal]) + L"\r\n" + db.title + L"\r\n" +
                     source + L"\r\n";
    if (!unverified.empty()) o += L"UNVERIFIED: " + unverified + L"\r\n";
    o += L"\r\n";
    o += L"Read-only report: apply these yourself in the BIOS (F7 advanced mode). Change a few at a time and test.\r\n\r\n";
    o += L"=== Settings to change ===\r\n";
    int n = 0;
    for (auto& s : db.S) {
        if (!Differs(db, s, goal)) continue;
        o += L"- " + s.prompt + L": " + CurText(s) + L"  ->  " + RecText(db, s, goal) +
             (s.hidden ? L"   [hidden in BIOS menus]" : L"") + L"\r\n";
        if (!s.path.empty()) o += L"    where: " + s.path + L"\r\n";
        const Kb* k = db.KbOf(s);
        if (k && !k->note.empty()) o += L"    note: " + k->note + L"\r\n";
        if (k) o += BasisLines(*k, RecText(db, s, goal) + L"\n" + k->note);
        n++;
    }
    if (!n) o += L"(none - everything with an exact recommendation already matches)\r\n";
    o += L"\r\n=== Advice (check these by hand) ===\r\n";
    std::set<int> seen;
    for (auto& s : db.S) {
        const Kb* k = db.KbOf(s);
        if (!k || seen.count(s.kb)) continue;
        long long v;
        const std::wstring& r = k->rec[goal];
        if (r.empty() || RecVal(db, s, goal, &v)) continue;
        seen.insert(s.kb);
        std::wstring rt = RecText(db, s, goal);
        o += L"- " + k->title + L" (now: " + CurText(s) + L"): " + rt + L"\r\n";
        o += BasisLines(*k, rt);
    }
    // Normalise embedded newlines (KB text may contain \n) to CRLF.
    std::wstring crlf;
    crlf.reserve(o.size() + 64);
    for (size_t i = 0; i < o.size(); i++) {
        if (o[i] == L'\n' && (i == 0 || o[i - 1] != L'\r')) crlf += L'\r';
        crlf += o[i];
    }
    return crlf;
}

}  // namespace hx::bios
