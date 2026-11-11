// BIOS Settings Explorer - read-only viewer for AMI/MSI UEFI setup settings.
// Reads the live UEFI variables (or a saved dump), decodes them with an embedded settings map
// extracted from the official BIOS file, and explains/recommends values per goal.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>

using namespace std;

// ---------------------------------------------------------------- data
struct Opt { long long v; wstring t; };
struct Kb { wstring title, expl, rec[5], note, fx[3], claim; };
struct Setting {
    int id; wstring var, guid; int offset, width; wstring kind; bool hidden, cond;
    bool hasDef; long long def; bool hasMin; long long mn, mx, step;
    wstring prompt, help, path; vector<Opt> opts; int kb;
    bool hasCur; long long cur; wstring ctx;
};
static vector<Setting> S;
static vector<Kb> K;
static wstring gTitle, gSource;
static int gKeyCount = 1 << 30;   // KB entries below this index are the in-depth ones
static const wchar_t* GOALS[5] = { L"Power saving", L"Balanced", L"Single-core", L"Multi-core", L"Low latency" };
static int gGoal = -1;            // -1 = none
static int gFilter = 0;           // 0 all,1 explained,2 changed,3 hidden,4 differs from goal
static wstring gSearch;
static vector<int> V;             // visible indices
static int gSel = -1, gTop = 0;
static bool gLive = false;
static int gSortCol = -1; static bool gSortAsc = true;   // -1 default order, 0 name, 1 current, 2 default, 3 goal
static int gDrag = 0, gDragOff = 0;                     // 1 list thumb, 2 detail thumb
static RECT rcListTrack, rcListThumb, rcDetTrack, rcDetThumb;
static int detContentH = 0, detViewH = 0, gRowsVis = 25;

static wstring W(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
static string U8(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, 0, 0); return s;
}
static wstring Unesc(const wstring& s) {
    wstring o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) { wchar_t c = s[++i]; o += (c == L'n') ? L'\n' : c; }
        else o += s[i];
    }
    return o;
}
static vector<wstring> Split(const wstring& s, wchar_t d) {
    vector<wstring> r; size_t a = 0;
    for (size_t i = 0; i <= s.size(); i++) if (i == s.size() || s[i] == d) { r.push_back(s.substr(a, i - a)); a = i + 1; }
    return r;
}
static long long ToLL(const wstring& s) { return s.empty() ? 0 : _wtoi64(s.c_str()); }
static wstring Lower(wstring s) { for (auto& c : s) c = towlower(c); return s; }

static void AddContext() {   // label repeated names (one per PCIe port, core, etc.) with the menu they belong to
    map<wstring, int> cnt; for (auto& s : S) cnt[s.prompt]++;
    for (auto& s : S) if (cnt[s.prompt] > 1 && !s.path.empty()) {
        wstring p = s.path.substr(0, s.path.find(L" | "));
        size_t k = p.rfind(L" > "); s.ctx = k == wstring::npos ? p : p.substr(k + 3);
        if (s.ctx == s.prompt) s.ctx.clear();
    }
}
static void LoadDb() {
    HRSRC r = FindResourceW(0, L"SETTINGS", (LPCWSTR)RT_RCDATA);
    if (!r) return;
    DWORD sz = SizeofResource(0, r);
    const char* p = (const char*)LockResource(LoadResource(0, r));
    wstring all = W(string(p, sz));
    for (auto& line : Split(all, L'\n')) {
        if (line.size() < 2) continue;
        auto f = Split(line, L'\t');
        if (f[0] == L"V") gTitle = Unesc(f[1]);
        else if (f[0] == L"T") gKeyCount = (int)ToLL(f[1]);
        else if (f[0] == L"K" && f.size() >= 10) {
            Kb k; k.title = Unesc(f[2]); k.expl = Unesc(f[3]);
            for (int g = 0; g < 5; g++) k.rec[g] = Unesc(f[4 + g]);
            k.note = Unesc(f[9]);
            if (f.size() >= 14) { for (int j = 0; j < 3; j++) k.fx[j] = Unesc(f[10 + j]); k.claim = Unesc(f[13]); }
            K.push_back(k);
        } else if (f[0] == L"S" && f.size() >= 17) {
            Setting s{};
            s.id = (int)ToLL(f[1]); s.var = f[2]; s.guid = f[3]; s.offset = (int)ToLL(f[4]); s.width = (int)ToLL(f[5]);
            s.kind = f[6]; s.hidden = f[7].find(L'H') != wstring::npos; s.cond = f[7].find(L'C') != wstring::npos;
            s.hasDef = !f[8].empty(); s.def = ToLL(f[8]);
            s.hasMin = !f[9].empty(); s.mn = ToLL(f[9]); s.mx = ToLL(f[10]); s.step = ToLL(f[11]);
            s.prompt = Unesc(f[12]); s.help = Unesc(f[13]); s.path = Unesc(f[14]);
            if (!f[15].empty()) for (auto& o : Split(f[15], 0x1e)) {
                auto kv = Split(o, 0x1f); if (kv.size() == 2) s.opts.push_back({ ToLL(kv[0]), Unesc(kv[1]) });
            }
            s.kb = (int)ToLL(f[16]);
            S.push_back(s);
        }
    }
}

// ---------------------------------------------------------------- reading values
typedef DWORD(WINAPI* PGetFwEx)(LPCWSTR, LPCWSTR, PVOID, DWORD, PDWORD);
static bool EnablePriv() {
    HANDLE t; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return false;
    TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(0, L"SeSystemEnvironmentPrivilege", &tp.Privileges[0].Luid);
    AdjustTokenPrivileges(t, FALSE, &tp, 0, 0, 0);
    bool ok = GetLastError() == ERROR_SUCCESS; CloseHandle(t); return ok;
}
static void ApplyVars(const map<wstring, vector<BYTE>>& vars) {   // key: lower(name)+"|"+lower(guid)
    for (auto& s : S) {
        auto it = vars.find(Lower(s.var) + L"|" + Lower(s.guid));
        s.hasCur = false;
        if (it != vars.end() && s.offset + s.width <= (int)it->second.size()) {
            long long v = 0;
            for (int i = s.width - 1; i >= 0; i--) v = (v << 8) | it->second[s.offset + i];
            s.cur = v; s.hasCur = true;
        }
    }
}
static int ReadLive() {
    EnablePriv();
    PGetFwEx fn = (PGetFwEx)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareEnvironmentVariableExW");
    if (!fn) return 0;
    map<wstring, vector<BYTE>> vars;
    vector<BYTE> buf(1 << 16);
    for (auto& s : S) {
        wstring key = Lower(s.var) + L"|" + Lower(s.guid);
        if (vars.count(key)) continue;
        wstring g = L"{" + s.guid + L"}"; DWORD attr = 0;
        DWORD n = fn(s.var.c_str(), g.c_str(), buf.data(), (DWORD)buf.size(), &attr);
        vars[key] = n ? vector<BYTE>(buf.begin(), buf.begin() + n) : vector<BYTE>();
    }
    int ok = 0; for (auto& kv : vars) if (!kv.second.empty()) ok++;
    if (ok) { ApplyVars(vars); gLive = true; gSource = L"Live values read from this PC"; }
    return ok;
}
static bool ReadDump(const wstring& dir) {        // folder containing index.csv and vars\*.bin (from Dump-UEFI.ps1)
    FILE* f = _wfopen((dir + L"\\index.csv").c_str(), L"rb");
    if (!f) return false;
    string txt; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) txt.append(b, n); fclose(f);
    map<wstring, vector<BYTE>> vars;
    bool first = true;
    for (auto& line : Split(W(txt), L'\n')) {
        if (first) { first = false; continue; }
        vector<wstring> c; wstring cur; bool q = false;
        for (wchar_t ch : line) { if (ch == L'"') q = !q; else if (ch == L',' && !q) { c.push_back(cur); cur.clear(); } else if (ch != L'\r') cur += ch; }
        c.push_back(cur);
        if (c.size() < 6 || c[5].empty()) continue;
        wstring guid = c[1]; if (!guid.empty() && guid[0] == L'{') guid = guid.substr(1, guid.size() - 2);
        FILE* vf = _wfopen((dir + L"\\vars\\" + c[5]).c_str(), L"rb");
        if (!vf) continue;
        vector<BYTE> data; while ((n = fread(b, 1, sizeof b, vf)) > 0) data.insert(data.end(), b, b + n); fclose(vf);
        vars[Lower(c[0]) + L"|" + Lower(guid)] = data;
    }
    if (vars.empty()) return false;
    ApplyVars(vars); gLive = false; gSource = L"Saved dump: " + dir; return true;
}

// ---------------------------------------------------------------- logic
static bool ZeroIsAuto(const Setting& s) {
    static map<int, int> cache; auto it = cache.find(s.id); if (it != cache.end()) return it->second;
    wstring h = Lower(s.help); bool r = false;
    const wchar_t* pats[] = { L"0: auto", L"0 = auto", L"0=auto", L"0 means auto", L"0 - auto", L"0 = no override", L"0- auto", L"0 - mrc auto", L"0-auto" };
    for (auto p : pats) if (h.find(p) != wstring::npos) { r = true; break; }
    if (!r && s.kind == L"numeric" && !s.var.empty() && (s.path.find(L"DRAM") != wstring::npos || s.path.find(L"XMP User Profile") != wstring::npos ||
        s.path.find(L"Memory Overclocking") != wstring::npos) && s.mn == 0) r = true;    // MSI memory timing fields: 0 = Auto
    cache[s.id] = r; return r;
}
static wstring ValText(const Setting& s, long long v) {
    for (auto& o : s.opts) if (o.v == v) return o.t.empty() ? to_wstring(v) : o.t;
    if (v == 0 && s.opts.empty() && ZeroIsAuto(s)) return L"Auto";
    wchar_t b[64];
    if (s.kind == L"numeric" && s.width >= 2 && v > 9) swprintf(b, 64, L"%lld", v); else swprintf(b, 64, L"%lld", v);
    return b;
}
static wstring CurText(const Setting& s) { return s.hasCur ? ValText(s, s.cur) : L"-"; }
static wstring DefText(const Setting& s) { return s.hasDef ? ValText(s, s.def) : L"-"; }
static bool Changed(const Setting& s) { return s.hasCur && s.hasDef && s.cur != s.def; }
// recommended exact option for goal g; returns true and the value if the advice maps to one option
static bool RecVal(const Setting& s, int g, long long& v) {
    if (s.kb < 0 || g < 0) return false;
    const wstring& r = K[s.kb].rec[g];
    if (r.empty() || r[0] == L'~') return false;
    wstring rl = Lower(r);
    for (auto& o : s.opts) { wstring ol = Lower(o.t); if (ol.compare(0, rl.size(), rl) == 0) { v = o.v; return true; } }
    return false;
}
static wstring RecText(const Setting& s, int g) {
    if (s.kb < 0 || g < 0) return L"";
    wstring r = K[s.kb].rec[g];
    if (!r.empty() && r[0] == L'~') r = r.substr(1);
    long long v; if (RecVal(s, g, v)) return ValText(s, v);
    return r;
}
static bool Differs(const Setting& s) { long long v; return s.hasCur && RecVal(s, gGoal, v) && v != s.cur; }

static bool IsKey(const Setting& s) { return s.kb >= 0 && s.kb < gKeyCount; }
static int GoalRank(const Setting& s) {
    if (gGoal < 0 || s.kb < 0 || K[s.kb].rec[gGoal].empty()) return 4;
    long long v; if (RecVal(s, gGoal, v)) return !s.hasCur ? 3 : (v != s.cur ? 0 : 2);
    return 1;
}
static void SortV() {
    if (gSortCol < 0) return;
    stable_sort(V.begin(), V.end(), [](int a, int b) {
        const Setting &x = S[a], &y = S[b]; int c = 0;
        if (gSortCol == 0) c = _wcsicmp(x.prompt.c_str(), y.prompt.c_str());
        else if (gSortCol == 1 || gSortCol == 2) {
            bool hx = gSortCol == 1 ? x.hasCur : x.hasDef, hy = gSortCol == 1 ? y.hasCur : y.hasDef;
            if (hx != hy) return hx;                       // missing values always last
            wstring tx = gSortCol == 1 ? CurText(x) : DefText(x), ty = gSortCol == 1 ? CurText(y) : DefText(y);
            long long vx = gSortCol == 1 ? x.cur : x.def, vy = gSortCol == 1 ? y.cur : y.def;
            bool nx = iswdigit(tx[0]) != 0, ny = iswdigit(ty[0]) != 0;
            if (nx && ny) c = vx < vy ? -1 : vx > vy; else if (nx != ny) c = nx ? 1 : -1; else c = _wcsicmp(tx.c_str(), ty.c_str());
        } else {
            int rx = GoalRank(x), ry = GoalRank(y);
            if ((rx == 4) != (ry == 4)) return ry == 4;    // no advice always last
            c = rx - ry;
            if (!c) c = _wcsicmp(x.prompt.c_str(), y.prompt.c_str());
        }
        return gSortAsc ? c < 0 : c > 0;
    });
}
static void Refilter() {
    V.clear();
    wstring q = Lower(gSearch);
    for (int i = 0; i < (int)S.size(); i++) {
        const Setting& s = S[i];
        if (gFilter == 1 && !IsKey(s)) continue;
        if (gFilter == 2 && !Changed(s)) continue;
        if (gFilter == 3 && !s.hidden) continue;
        if (gFilter == 4 && !Differs(s)) continue;
        if (!q.empty()) {
            wstring hay = Lower(s.prompt + L" " + s.ctx + L" " + s.path + L" " + s.var + (s.kb >= 0 ? L" " + K[s.kb].title : L""));
            if (hay.find(q) == wstring::npos) continue;
        }
        V.push_back(i);
    }
    // explained settings first, then changed ones, then the rest (stable)
    stable_sort(V.begin(), V.end(), [](int a, int b) {
        int ra = (IsKey(S[a]) ? 0 : 2) + (Changed(S[a]) ? 0 : 1), rb = (IsKey(S[b]) ? 0 : 2) + (Changed(S[b]) ? 0 : 1);
        return ra < rb;
    });
    SortV();
    if (gSel >= 0 && find(V.begin(), V.end(), gSel) == V.end()) gSel = V.empty() ? -1 : V[0];
    if (gSel < 0 && !V.empty()) gSel = V[0];
    gTop = 0;
    auto it = find(V.begin(), V.end(), gSel);
    if (it != V.end()) { int k = (int)(it - V.begin()), rows = max(1, gRowsVis); if (k >= rows) gTop = k - rows / 2; }
}

// ---------------------------------------------------------------- export
static void Export(HWND h) {
    if (gGoal < 0) { MessageBoxW(h, L"Pick a goal first (Power saving, Balanced, ...).", L"Export", MB_ICONINFORMATION); return; }
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(0, exe, MAX_PATH);
    wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
    wstring path = dir + L"\\BIOS changes - " + GOALS[gGoal] + L".txt";
    wstring o = L"BIOS changes for goal: " + wstring(GOALS[gGoal]) + L"\r\n" + gTitle + L"\r\n" + gSource + L"\r\n\r\n";
    o += L"Read-only report: apply these yourself in the BIOS (F7 advanced mode). Change a few at a time and test.\r\n\r\n";
    o += L"=== Settings to change ===\r\n";
    int n = 0;
    for (auto& s : S) if (Differs(s)) {
        o += L"- " + s.prompt + L": " + CurText(s) + L"  ->  " + RecText(s, gGoal) + (s.hidden ? L"   [hidden in BIOS menus]" : L"") + L"\r\n";
        if (!s.path.empty()) o += L"    where: " + s.path + L"\r\n";
        if (!K[s.kb].note.empty()) o += L"    note: " + K[s.kb].note + L"\r\n";
        n++;
    }
    if (!n) o += L"(none - everything with an exact recommendation already matches)\r\n";
    o += L"\r\n=== Advice (check these by hand) ===\r\n";
    map<int, bool> seen;
    for (auto& s : S) if (s.kb >= 0 && !seen[s.kb]) {
        long long v; const wstring& r = K[s.kb].rec[gGoal];
        if (r.empty() || RecVal(s, gGoal, v)) continue;
        seen[s.kb] = true;
        o += L"- " + K[s.kb].title + L" (now: " + CurText(s) + L"): " + RecText(s, gGoal) + L"\r\n";
    }
    string u = "\xEF\xBB\xBF" + U8(o);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { MessageBoxW(h, (L"Could not write " + path).c_str(), L"Export", MB_ICONERROR); return; }
    fwrite(u.data(), 1, u.size(), f); fclose(f);
    ShellExecuteW(h, L"open", path.c_str(), 0, 0, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- UI
static HFONT fTitle, fBody, fSmall, fBold, fMono;
static HWND hSearch;
static HBRUSH brEdit;
static const COLORREF cBg = RGB(24, 25, 28), cPanel = RGB(32, 34, 38), cRow = RGB(38, 40, 45), cSel = RGB(46, 70, 96),
    cText = RGB(228, 230, 233), cMute = RGB(140, 145, 152), cAcc = RGB(86, 170, 255), cWarn = RGB(240, 180, 80),
    cBad = RGB(240, 110, 100), cGood = RGB(110, 200, 140), cLine = RGB(52, 55, 60), cChip = RGB(44, 47, 52);
struct Hit { RECT r; int kind, data; };       // kinds: 1 goal, 2 filter, 3 row, 4 load, 5 export
static vector<Hit> hits;
static int gHover = -1, rowH = 26, listX, listY, listW, listH, detX, detScroll = 0;

static void Fill(HDC d, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(d, &r, b); DeleteObject(b); }
static void RRect(HDC d, RECT r, COLORREF c, int rad = 10) {
    HBRUSH b = CreateSolidBrush(c); HPEN p = CreatePen(PS_SOLID, 1, c);
    SelectObject(d, b); SelectObject(d, p); RoundRect(d, r.left, r.top, r.right, r.bottom, rad, rad);
    DeleteObject(b); DeleteObject(p);
}
static void Txt(HDC d, const wstring& s, RECT r, HFONT f, COLORREF c, UINT fl = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX) {
    SelectObject(d, f); SetTextColor(d, c); SetBkMode(d, TRANSPARENT); DrawTextW(d, s.c_str(), -1, &r, fl);
}
static int TxtW(HDC d, const wstring& s, HFONT f) { SelectObject(d, f); SIZE z; GetTextExtentPoint32W(d, s.c_str(), (int)s.size(), &z); return z.cx; }
// wrapped paragraph, returns height used
static int Para(HDC d, const wstring& s, int x, int y, int w, HFONT f, COLORREF c) {
    RECT r{ x, y, x + w, y + 10 }; SelectObject(d, f);
    DrawTextW(d, s.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    r.right = x + w; SetTextColor(d, c); SetBkMode(d, TRANSPARENT);
    DrawTextW(d, s.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX);
    return r.bottom - r.top;
}
static int Chip(HDC d, int x, int y, const wstring& s, bool on, int kind, int data) {
    int w = TxtW(d, s, fSmall) + 22; RECT r{ x, y, x + w, y + 26 };
    int id = (int)hits.size(); hits.push_back({ r, kind, data });
    RRect(d, r, on ? cAcc : (gHover == id ? RGB(58, 62, 68) : cChip), 26);
    Txt(d, s, r, fSmall, on ? RGB(10, 20, 30) : cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w + 8;
}
static void Badge(HDC d, int& x, int y, const wstring& s, COLORREF c) {
    int w = TxtW(d, s, fSmall) + 14; RECT r{ x, y, x + w, y + 20 };
    RRect(d, r, RGB(GetRValue(c) / 4 + 20, GetGValue(c) / 4 + 20, GetBValue(c) / 4 + 20), 8);
    Txt(d, s, r, fSmall, c, DT_CENTER | DT_VCENTER | DT_SINGLELINE); x += w + 6;
}

static void Paint(HWND h, HDC d, int W_, int H_) {
    hits.clear();
    RECT all{ 0, 0, W_, H_ }; Fill(d, all, cBg);
    // header
    Txt(d, L"BIOS Settings Explorer", RECT{ 20, 12, 600, 44 }, fTitle, cText);
    Txt(d, gTitle + L"   •   " + gSource, RECT{ 20, 44, W_ - 330, 66 }, fSmall, cMute);
    int bx = W_ - 20;
    { wstring s = L"Export changes"; int w = TxtW(d, s, fSmall) + 26; RECT r{ bx - w, 18, bx, 48 }; hits.push_back({ r, 5, 0 });
      RRect(d, r, gHover == (int)hits.size() - 1 ? RGB(110, 190, 255) : cAcc, 10); Txt(d, s, r, fSmall, RGB(10, 20, 30), DT_CENTER | DT_VCENTER | DT_SINGLELINE); bx -= w + 10; }
    { wstring s = L"Load dump..."; int w = TxtW(d, s, fSmall) + 26; RECT r{ bx - w, 18, bx, 48 }; hits.push_back({ r, 4, 0 });
      RRect(d, r, gHover == (int)hits.size() - 1 ? RGB(64, 68, 75) : cChip, 10); Txt(d, s, r, fSmall, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
    // goals
    int x = 20, y = 76;
    Txt(d, L"Goal", RECT{ x, y, x + 50, y + 26 }, fSmall, cMute); x += 50;
    x += Chip(d, x, y, L"None", gGoal < 0, 1, -1);
    for (int g = 0; g < 5; g++) x += Chip(d, x, y, GOALS[g], gGoal == g, 1, g);
    // filters + search
    y = 112; x = 20;
    Txt(d, L"Show", RECT{ x, y, x + 50, y + 26 }, fSmall, cMute); x += 50;
    const wchar_t* FL[5] = { L"All", L"Key settings", L"Changed from default", L"Hidden in menus", L"Differs from goal" };
    for (int i = 0; i < 5; i++) { if (i == 4 && gGoal < 0) continue; x += Chip(d, x, y, FL[i], gFilter == i, 2, i); }
    MoveWindow(hSearch, W_ - 340, y + 1, 320, 24, FALSE);
    if (GetWindowTextLengthW(hSearch) == 0 && GetFocus() != hSearch) Txt(d, L"", RECT{ 0, 0, 0, 0 }, fSmall, cMute);
    // list
    listX = 20; listY = 152; listW = (int)(W_ * 0.55); listH = H_ - listY - 20;
    RECT lp{ listX, listY, listX + listW, listY + listH }; RRect(d, lp, cPanel, 12);
    int cName = listX + 14, cCur = listX + (int)(listW * 0.52), cDef = listX + (int)(listW * 0.70), cRec = listX + (int)(listW * 0.85);
    {
        int cx[5] = { cName, cCur, cDef, cRec, listX + listW - 10 };
        wstring nm[4] = { L"Setting  (" + to_wstring(V.size()) + L")    grey = hidden in BIOS menus", L"Current", L"Default", L"Goal" };
        for (int c = 0; c < 4; c++) {
            if (c == 3 && gGoal < 0) continue;
            RECT r{ cx[c] - 4, listY + 6, cx[c + 1] - 8, listY + 30 };
            int id = (int)hits.size(); hits.push_back({ r, 6, c });
            wstring t = nm[c] + (gSortCol == c ? (gSortAsc ? L"  \u25B2" : L"  \u25BC") : L"");
            Txt(d, t, RECT{ cx[c], r.top, r.right, r.bottom }, gSortCol == c ? fBold : fSmall, gSortCol == c ? cAcc : (gHover == id ? cText : cMute));
        }
    }
    int ry = listY + 34, rows = (listH - 40) / rowH; gRowsVis = rows;
    if (gTop > (int)V.size() - rows) gTop = max(0, (int)V.size() - rows);
    for (int k = gTop; k < (int)V.size() && k < gTop + rows; k++) {
        const Setting& s = S[V[k]];
        RECT r{ listX + 6, ry, listX + listW - 10, ry + rowH - 2 };
        int id = (int)hits.size(); hits.push_back({ r, 3, V[k] });
        if (V[k] == gSel) RRect(d, r, cSel, 8); else if (gHover == id) RRect(d, r, cRow, 8);
        int nx = cName;
        if (IsKey(s)) { RECT dot{ nx, ry + 9, nx + 7, ry + 16 }; RRect(d, dot, cAcc, 7); }
        nx += 12;
        COLORREF nc = s.hidden ? cMute : cText;
        Txt(d, s.prompt, RECT{ nx, ry, cCur - 10, ry + rowH - 2 }, fBody, nc);
        if (!s.ctx.empty()) {
            int tw = TxtW(d, s.prompt, fBody);
            if (nx + tw + 30 < cCur - 10) Txt(d, L"\u00B7  " + s.ctx, RECT{ nx + tw + 8, ry, cCur - 10, ry + rowH - 2 }, fSmall, RGB(105, 110, 118));
        }
        Txt(d, CurText(s), RECT{ cCur, ry, cDef - 8, ry + rowH - 2 }, fBody, Changed(s) ? cWarn : cText);
        Txt(d, DefText(s), RECT{ cDef, ry, cRec - 8, ry + rowH - 2 }, fBody, cMute);
        if (gGoal >= 0 && s.kb >= 0) {
            long long v; bool ex = RecVal(s, gGoal, v);
            Txt(d, RecText(s, gGoal), RECT{ cRec, ry, listX + listW - 14, ry + rowH - 2 }, fBody,
                ex ? (v == s.cur ? cGood : cBad) : cMute);
        }
        ry += rowH;
    }
    rcListTrack = RECT{ 0, 0, 0, 0 };
    if ((int)V.size() > rows && rows > 0) {   // scrollbar (draggable)
        int trackT = listY + 34, trackH = listH - 44;
        int th = max(30, trackH * rows / (int)V.size()), ty = trackT + (trackH - th) * gTop / max(1, (int)V.size() - rows);
        rcListTrack = RECT{ listX + listW - 14, trackT, listX + listW, trackT + trackH };
        rcListThumb = RECT{ listX + listW - 11, ty, listX + listW - 3, ty + th };
        bool hot = gDrag == 1 || gHover == -2;
        RRect(d, rcListThumb, hot ? RGB(110, 116, 125) : RGB(70, 74, 80), 6);
    }
    // details
    detX = listX + listW + 16; int dw = W_ - detX - 20;
    RECT dp{ detX, listY, detX + dw, listY + listH }; RRect(d, dp, cPanel, 12);
    if (gSel < 0) { Txt(d, L"No setting selected", RECT{ detX + 20, listY + 20, detX + dw, listY + 50 }, fBody, cMute); return; }
    const Setting& s = S[gSel];
    HRGN clip = CreateRectRgn(detX + 2, listY + 2, detX + dw - 2, listY + listH - 2); SelectClipRgn(d, clip);
    int px = detX + 20, pw = dw - 40, py = listY + 18 - detScroll;
    py += Para(d, s.prompt, px, py, pw, fTitle, cText) + 4;
    if (!s.path.empty()) py += Para(d, s.path, px, py, pw, fSmall, cMute) + 6;
    int bxx = px;
    if (s.hidden) Badge(d, bxx, py, L"Hidden in BIOS menus", cMute);
    else if (s.cond) Badge(d, bxx, py, L"Shown depending on other settings", cMute);
    if (Changed(s)) Badge(d, bxx, py, L"Changed from default", cWarn);
    wchar_t loc[128]; swprintf(loc, 128, L"%s + 0x%X (%d byte%s)", s.var.c_str(), s.offset, s.width, s.width > 1 ? L"s" : L"");
    Badge(d, bxx, py, loc, cMute);
    py += 30;
    auto H = [&](const wstring& t) { py += 6; Txt(d, t, RECT{ px, py, px + pw, py + 22 }, fBold, cAcc); py += 24; };
    if (s.kb >= 0) { H(L"What it does"); py += Para(d, K[s.kb].expl, px, py, pw, fBody, cText) + 6; }
    if (s.kb >= 0 && !K[s.kb].fx[0].empty()) {
        H(L"Real-world effect");
        const wchar_t* FX[3] = { L"FPS / esports", L"Other games", L"Apps & work" };
        bool same = K[s.kb].fx[1] == K[s.kb].fx[0] && K[s.kb].fx[2] == K[s.kb].fx[0];
        if (same) FX[0] = L"All workloads";
        for (int j = 0; j < (same ? 1 : 3); j++) {
            Txt(d, FX[j], RECT{ px, py, px + 130, py + 22 }, fBold, cMute, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            py += max(22, Para(d, K[s.kb].fx[j], px + 130, py, pw - 130, fBody, cText)) + 8;
        }
        if (!K[s.kb].claim.empty()) { H(L"Claim check"); py += Para(d, K[s.kb].claim, px, py, pw, fBody, RGB(200, 170, 255)) + 6; }
    }
    if (!s.help.empty()) { H(s.kb >= 0 ? L"BIOS help text" : L"What it does (BIOS help text)"); py += Para(d, s.help, px, py, pw, fBody, s.kb >= 0 ? cMute : cText) + 6; }
    H(L"Values");
    wstring cv = L"Current: " + CurText(s) + L"      Default: " + DefText(s);
    py += Para(d, cv, px, py, pw, fBody, cText) + 6;
    if (s.kind == L"numeric" && s.hasMin) {
        wchar_t b[160]; swprintf(b, 160, L"Range %lld to %lld, step %lld", s.mn, s.mx, s.step ? s.step : 1);
        py += Para(d, b, px, py, pw, fSmall, cMute) + 6;
    }
    long long rv = 0; bool hasRv = RecVal(s, gGoal, rv);
    for (auto& o : s.opts) {
        bool isCur = s.hasCur && o.v == s.cur, isDef = s.hasDef && o.v == s.def, isRec = hasRv && o.v == rv;
        RECT r{ px, py, px + pw, py + 24 };
        if (isCur) RRect(d, r, cSel, 8);
        wstring mark = wstring(isCur ? L"● " : L"   ");
        Txt(d, mark + (o.t.empty() ? to_wstring(o.v) : o.t), RECT{ px + 8, py, px + pw - 200, py + 24 }, fBody, isCur ? cText : cMute);
        wstring tags = wstring(isDef ? L"default " : L"") + (isRec ? L"best for " + wstring(GOALS[gGoal]) : L"");
        Txt(d, tags, RECT{ px + pw - 200, py, px + pw - 8, py + 24 }, fSmall, isRec ? cGood : cMute, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        py += 26;
    }
    if (s.kb >= 0) {
        H(L"Recommended by goal");
        for (int g = 0; g < 5; g++) {
            wstring r = RecText(s, g); if (r.empty()) r = L"no change needed";
            long long v; bool ex = RecVal(s, g, v);
            Txt(d, GOALS[g], RECT{ px, py, px + 130, py + 24 }, g == gGoal ? fBold : fBody, g == gGoal ? cAcc : cMute);
            int hgt = Para(d, r, px + 130, py + 3, pw - 130, fBody, ex ? (s.hasCur && v == s.cur ? cGood : cText) : cText);
            py += max(24, hgt + 6);
        }
        if (!K[s.kb].note.empty()) { H(L"Note"); py += Para(d, K[s.kb].note, px, py, pw, fBody, cWarn) + 6; }
    }
    SelectClipRgn(d, 0); DeleteObject(clip);
    detContentH = py + detScroll - (listY + 18) + 30; detViewH = listH - 4;
    if (detScroll > max(0, detContentH - detViewH)) detScroll = max(0, detContentH - detViewH);
    rcDetTrack = RECT{ 0, 0, 0, 0 };
    if (detContentH > detViewH) {
        int trackT = listY + 8, trackH = listH - 16;
        int th = max(30, trackH * detViewH / detContentH), ty = trackT + (trackH - th) * detScroll / max(1, detContentH - detViewH);
        rcDetTrack = RECT{ detX + dw - 14, trackT, detX + dw, trackT + trackH };
        rcDetThumb = RECT{ detX + dw - 11, ty, detX + dw - 3, ty + th };
        RRect(d, rcDetThumb, gDrag == 2 ? RGB(110, 116, 125) : RGB(70, 74, 80), 6);
    }
}

static void Invalidate(HWND h) { InvalidateRect(h, 0, FALSE); }
static void LoadDumpDialog(HWND h) {
    wchar_t file[MAX_PATH] = L"index.csv";
    OPENFILENAMEW of{ sizeof of }; of.hwndOwner = h; of.lpstrFilter = L"UEFI dump index (index.csv)\0index.csv\0All files\0*.*\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH; of.lpstrTitle = L"Open the index.csv inside a UEFI_Dump folder"; of.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    wstring dir = file; dir = dir.substr(0, dir.find_last_of(L'\\'));
    if (!ReadDump(dir)) MessageBoxW(h, L"Could not read that dump.", L"Load dump", MB_ICONERROR);
    Refilter(); Invalidate(h);
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        hSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)100, 0, 0);
        SendMessageW(hSearch, WM_SETFONT, (WPARAM)fBody, TRUE);
        SendMessageW(hSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search settings...");
        brEdit = CreateSolidBrush(RGB(44, 47, 52));
        return 0; }
    case WM_CTLCOLOREDIT: SetTextColor((HDC)w, cText); SetBkColor((HDC)w, RGB(44, 47, 52)); return (LRESULT)brEdit;
    case WM_COMMAND:
        if (LOWORD(w) == 100 && HIWORD(w) == EN_CHANGE) {
            wchar_t b[256]; GetWindowTextW(hSearch, b, 256); gSearch = b; Refilter(); Invalidate(h);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC d = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC m2 = CreateCompatibleDC(d); HBITMAP bm = CreateCompatibleBitmap(d, rc.right, rc.bottom); SelectObject(m2, bm);
        Paint(h, m2, rc.right, rc.bottom);
        BitBlt(d, 0, 0, rc.right, rc.bottom, m2, 0, 0, SRCCOPY); DeleteObject(bm); DeleteDC(m2); EndPaint(h, &ps);
        return 0; }
    case WM_MOUSEMOVE: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; int hv = -1;
        if (gDrag == 1) {
            int rows = (listH - 40) / rowH, n = (int)V.size() - rows;
            int th = rcListThumb.bottom - rcListThumb.top, span = (rcListTrack.bottom - rcListTrack.top) - th;
            if (span > 0 && n > 0) gTop = max(0, min(n, (int)((long long)(p.y - gDragOff - rcListTrack.top) * n / span)));
            Invalidate(h); return 0;
        }
        if (gDrag == 2) {
            int n = detContentH - detViewH, th = rcDetThumb.bottom - rcDetThumb.top, span = (rcDetTrack.bottom - rcDetTrack.top) - th;
            if (span > 0 && n > 0) detScroll = max(0, min(n, (int)((long long)(p.y - gDragOff - rcDetTrack.top) * n / span)));
            Invalidate(h); return 0;
        }
        if (PtInRect(&rcListTrack, p)) { if (gHover != -2) { gHover = -2; Invalidate(h); } return 0; }
        for (int i = 0; i < (int)hits.size(); i++) if (PtInRect(&hits[i].r, p)) { hv = i; break; }
        if (hv != gHover) { gHover = hv; Invalidate(h); }
        TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, h }; TrackMouseEvent(&t);
        return 0; }
    case WM_MOUSELEAVE: gHover = -1; Invalidate(h); return 0;
    case WM_LBUTTONUP: if (gDrag) { gDrag = 0; ReleaseCapture(); Invalidate(h); } return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        for (int which = 1; which <= 2; which++) {
            RECT& tr = which == 1 ? rcListTrack : rcDetTrack; RECT& tb = which == 1 ? rcListThumb : rcDetThumb;
            if (!PtInRect(&tr, p)) continue;
            int th = tb.bottom - tb.top;
            gDragOff = (p.y >= tb.top && p.y < tb.bottom) ? p.y - tb.top : th / 2;   // click on track jumps there
            gDrag = which; SetCapture(h);
            SendMessageW(h, WM_MOUSEMOVE, w, l);
            return 0;
        }
        for (auto& ht : hits) if (PtInRect(&ht.r, p)) {
            if (ht.kind == 1) { gGoal = ht.data; if (gGoal < 0 && gFilter == 4) gFilter = 0; if (gGoal < 0 && gSortCol == 3) gSortCol = -1; Refilter(); }
            else if (ht.kind == 2) { gFilter = ht.data; Refilter(); }
            else if (ht.kind == 3) { gSel = ht.data; detScroll = 0; }
            else if (ht.kind == 4) { LoadDumpDialog(h); return 0; }
            else if (ht.kind == 5) { Export(h); return 0; }
            else if (ht.kind == 6) {
                if (gSortCol == ht.data) { if (gSortAsc) gSortAsc = false; else gSortCol = -1; }   // asc -> desc -> default
                else { gSortCol = ht.data; gSortAsc = true; }
                Refilter();
            }
            Invalidate(h); SetFocus(h); break;
        }
        return 0; }
    case WM_MOUSEWHEEL: {
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        int dz = GET_WHEEL_DELTA_WPARAM(w) / 120;
        if (p.x >= detX) detScroll = max(0, detScroll - dz * 60);
        else gTop = max(0, min((int)V.size() - 1, gTop - dz * 3));
        Invalidate(h); return 0; }
    case WM_KEYDOWN: {
        auto it = find(V.begin(), V.end(), gSel); int k = it == V.end() ? 0 : (int)(it - V.begin());
        int rows = (listH - 40) / rowH;
        if (w == VK_DOWN) k++; else if (w == VK_UP) k--; else if (w == VK_NEXT) k += rows; else if (w == VK_PRIOR) k -= rows; else return 0;
        k = max(0, min((int)V.size() - 1, k)); if (V.empty()) return 0;
        gSel = V[k]; detScroll = 0;
        if (k < gTop) gTop = k; if (k >= gTop + rows) gTop = k - rows + 1;
        Invalidate(h); return 0; }
    case WM_SIZE: Invalidate(h); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)l)->ptMinTrackSize = { 1000, 640 }; return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int show) {
    SetProcessDPIAware();
    LoadDb(); AddContext();
    auto F = [](int sz, int wt, const wchar_t* face) { return CreateFontW(-sz, 0, 0, 0, wt, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face); };
    fTitle = F(22, FW_SEMIBOLD, L"Segoe UI"); fBody = F(15, FW_NORMAL, L"Segoe UI"); fSmall = F(13, FW_NORMAL, L"Segoe UI");
    fBold = F(15, FW_SEMIBOLD, L"Segoe UI"); fMono = F(13, FW_NORMAL, L"Consolas");
    // command line: optional path to a UEFI dump folder
    int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool ok = false;
    if (argc > 1) ok = ReadDump(argv[1]);
    if (!ok) ok = ReadLive() > 0;
    if (!ok) gSource = L"No values loaded - run as administrator, or use Load dump...";
    Refilter();
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"BSX"; wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, L"APPICON");
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"BSX", L"BIOS Settings Explorer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1440, 880, 0, 0, hi, 0);
    BOOL dark = TRUE; typedef HRESULT(WINAPI* DWMSA)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dm = LoadLibraryW(L"dwmapi.dll")) { DWMSA f = (DWMSA)GetProcAddress(dm, "DwmSetWindowAttribute"); if (f) { f(h, 20, &dark, sizeof dark); f(h, 19, &dark, sizeof dark); } }
    ShowWindow(h, show); UpdateWindow(h);
    if (!ok) MessageBoxW(h, L"Couldn't read the BIOS settings from this PC.\n\nRun BIOS Settings Explorer as administrator, or click \"Load dump...\" and open the index.csv from a UEFI_Dump folder.", L"BIOS Settings Explorer", MB_ICONINFORMATION);
    MSG msg; while (GetMessageW(&msg, 0, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
