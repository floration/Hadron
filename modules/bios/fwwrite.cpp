// modules/bios/fwwrite.cpp — see fwwrite.h. Hard rule 2: this is the only file in the suite that may
// call the UEFI variable write API (RealBackend::Write), and CI enforces that.
#include "modules/bios/fwwrite.h"
#include "core/sys/log.h"
#include "core/sys/sys.h"
#include <shlobj.h>
#include <algorithm>
#include <cstdio>

namespace hx::bios {

namespace {

std::wstring Braced(const std::wstring& guid) {
    if (!guid.empty() && guid.front() == L'{') return guid;
    return L"{" + guid + L"}";
}

std::wstring Hex(unsigned long long v, int digits = 0) {
    wchar_t b[32];
    if (digits) swprintf(b, 32, L"0x%0*llX", digits, v);
    else swprintf(b, 32, L"0x%llX", v);
    return b;
}

void LogLine(LogLevel lv, const std::wstring& s) { sys::Write(lv, L"bios write: " + s); }

std::wstring AttrProblem(DWORD attr) {
    if ((attr & kAttrRequired) != kAttrRequired) {
        std::wstring m;
        if (!(attr & kAttrNV)) m += L" non-volatile (0x1)";
        if (!(attr & kAttrBS)) m += L" boot-service (0x2)";
        if (!(attr & kAttrRT)) m += L" runtime (0x4)";
        return L"its attributes " + Hex(attr) + L" lack" + m + L"; only NV|BS|RT setup variables are written";
    }
    DWORD extra = attr & ~kAttrRequired;
    if (!extra) return L"";
    std::wstring m;
    if (extra & 0x8) m += L" hardware-error-record (0x8)";
    if (extra & 0x10) m += L" authenticated (0x10)";
    if (extra & 0x20) m += L" time-based authenticated (0x20)";
    if (extra & 0x40) m += L" append-write (0x40)";
    if (extra & 0x80) m += L" enhanced-authenticated (0x80)";
    if (extra & ~0xF8u) m += L" unknown (" + Hex(extra & ~0xF8u) + L")";
    return L"its attributes " + Hex(attr) + L" include" + m + L"; such variables are never written";
}

// Write errors, worded for writes (VarErrorText words 5 as a refused read).
std::wstring WriteErrorText(DWORD e) {
    switch (e) {
        case ERROR_ACCESS_DENIED:
            return L"access denied (5): the firmware locked this variable, or this is a test run where writes are off";
        case ERROR_WRITE_PROTECT: return L"write-protected (19): the firmware refuses writes to this variable";
        case ERROR_PRIVILEGE_NOT_HELD: return L"privilege missing (1314): SeSystemEnvironmentPrivilege isn't enabled";
        case ERROR_INVALID_PARAMETER: return L"invalid parameter (87): refused before reaching the firmware";
        case ERROR_ENVVAR_NOT_FOUND: return L"not found (203): the variable doesn't exist";
    }
    return sys::ErrorText(e) + L" (" + std::to_wstring(e) + L")";
}

std::wstring SettingFlags(const Setting& s) {
    if (s.hidden) return L"Hidden in the BIOS menu";
    if (s.cond) return L"Shown only when another setting allows it; may have no effect";
    return L"";
}

bool Overlaps(int a0, int aw, int b0, int bw) { return a0 < b0 + bw && b0 < a0 + aw; }

// Other mapped settings stored in [off, off+w) of this variable, with what the plan does to them.
std::wstring SharedSettings(const Db& db, const PlanVar& pv, int self, int off, int w) {
    std::wstring out;
    int n = 0, more = 0;
    for (int i = 0; i < (int)db.S.size(); i++) {
        const Setting& o = db.S[i];
        if (i == self || o.var != pv.name || Lower(o.guid) != pv.guid) continue;
        if (!Overlaps(o.offset, o.width, off, w)) continue;
        long long a = 0, b = 0;
        bool ha = DecodeValue(pv.oldData, o.offset, o.width, &a), hb = DecodeValue(pv.newData, o.offset, o.width, &b);
        if (n >= 4) { more++; continue; }
        if (!out.empty()) out += L"; ";
        out += o.prompt + L" (" + (ha ? ValText(o, a) : L"?") + L" → " + (hb ? ValText(o, b) : L"?") + L")";
        n++;
    }
    if (more) out += L"; and " + std::to_wstring(more) + L" more";
    return out;
}

const FwVar* FindLive(const std::vector<FwVar>& live, const VarKey& k) {
    for (auto& v : live) if (MakeVarKey(v.name, v.guid) == k) return &v;
    return nullptr;
}

std::wstring GateProblem(const PlanGate& g) {
    if (!g.switchOn) return L"\"Allow BIOS writes\" is off.";
    if (!g.live) return L"Values aren't live (snapshot mode, or nothing was read from this PC).";
    if (!g.guardMatched) return L"The board/BIOS guard didn't match this PC (or couldn't run), so the map isn't verified.";
    return L"";
}

// Bytes of a and b (same size) that differ, as (offset, length) runs.
std::vector<std::pair<int, int>> DiffRanges(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<std::pair<int, int>> r;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n;) {
        if (a[i] == b[i]) { i++; continue; }
        size_t j = i;
        while (j < n && a[j] != b[j]) j++;
        r.push_back({(int)i, (int)(j - i)});
        i = j;
    }
    return r;
}

bool OnlyInRanges(const PlanVar& pv) {
    if (pv.oldData.size() != pv.newData.size()) return false;
    for (size_t i = 0; i < pv.oldData.size(); i++) {
        if (pv.oldData[i] == pv.newData[i]) continue;
        bool in = false;
        for (auto& r : pv.ranges) if ((int)i >= r.first && (int)i < r.first + r.second) { in = true; break; }
        if (!in) return false;
    }
    return true;
}

std::vector<uint8_t> Slice(const std::vector<uint8_t>& d, int off, int w) {
    if (off < 0 || w <= 0 || (size_t)off + (size_t)w > d.size()) return {};
    return std::vector<uint8_t>(d.begin() + off, d.begin() + off + w);
}

// journal.txt: opened once with CREATE_NEW; every line is flushed before the next step.
class Journal {
public:
    ~Journal() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    bool Open(const std::wstring& path, std::wstring* err) {
        h_ = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { *err = L"Can't create the journal: " + sys::ErrorText(GetLastError()); return false; }
        return true;
    }
    bool Line(const std::wstring& s, std::wstring* err) {
        std::string u = ToUtf8(s) + "\r\n";
        DWORD put = 0;
        if (!WriteFile(h_, u.data(), (DWORD)u.size(), &put, nullptr) || put != u.size() || !FlushFileBuffers(h_)) {
            *err = L"Writing the journal failed: " + sys::ErrorText(GetLastError());
            return false;
        }
        return true;
    }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

bool EnsureDir(const std::wstring& path, int depth = 0) {
    if (depth > 64 || path.empty()) return false;
    if (CreateDirectoryW(sys::ToExtended(path).c_str(), nullptr)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS) return true;
    if (e != ERROR_PATH_NOT_FOUND) { SetLastError(e); return false; }
    size_t k = path.find_last_of(L"\\/");
    if (k == std::wstring::npos || k == 0) { SetLastError(e); return false; }
    if (!EnsureDir(path.substr(0, k), depth + 1)) return false;
    if (CreateDirectoryW(sys::ToExtended(path).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

// ---- backends -----------------------------------------------------------------------------------
void RealBackend::Read(FwVar* v) { ReadVariable(v); }

DWORD RealBackend::Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data,
                         DWORD attr) {
    // Test runs never reach the firmware (docs/design/bios-write.md "Test mode latch").
    if (sys::NoFirmwareWrites()) return ERROR_ACCESS_DENIED;
    // Size 0 would delete the variable; anything but NV|BS|RT is refused; only map variables.
    if (data.empty() || data.size() > (4u << 20)) return ERROR_INVALID_PARAMETER;
    if (attr != kAttrRequired) return ERROR_INVALID_PARAMETER;
    if (!allowed_.count(MakeVarKey(name, guid))) return ERROR_INVALID_PARAMETER;
    std::wstring g = Braced(guid);
    apiCalls++;
    SetLastError(0);
    if (!SetFirmwareEnvironmentVariableExW(name.c_str(), g.c_str(), const_cast<uint8_t*>(data.data()),
                                           (DWORD)data.size(), attr)) {
        DWORD e = GetLastError();
        return e ? e : ERROR_GEN_FAILURE;
    }
    return 0;
}

bool RealBackend::Smbios(SmbiosInfo* out, DWORD* err) { return ReadSmbios(out, err); }

bool RealBackend::PrivilegeOk(const ThreadPrivilege& p, DWORD* err) {
    if (err) *err = p.ok() ? 0 : (p.err() ? p.err() : ERROR_PRIVILEGE_NOT_HELD);
    return p.ok();
}

void MockBackend::Put(const std::wstring& name, const std::wstring& guid, std::vector<uint8_t> data, DWORD attr) {
    VarKey k = MakeVarKey(name, guid);
    FwVar v;
    v.name = name;
    v.guid = k.second;
    v.attr = attr;
    v.data = std::move(data);
    vars[k] = std::move(v);
}

void MockBackend::Read(FwVar* v) {
    auto it = vars.find(MakeVarKey(v->name, v->guid));
    v->data.clear(); v->attr = 0; v->err = 0;
    if (it == vars.end()) { v->err = ERROR_ENVVAR_NOT_FOUND; return; }
    v->data = it->second.data;
    v->attr = it->second.attr;
}

DWORD MockBackend::Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data,
                         DWORD attr) {
    writes++;
    writeLog.push_back(name);
    if (data.empty()) return ERROR_INVALID_PARAMETER;   // a size-0 write would delete it: never
    auto it = vars.find(MakeVarKey(name, guid));
    if (it == vars.end()) return ERROR_ENVVAR_NOT_FOUND;   // never creates
    if (!failWriteOf.empty() && name == failWriteOf) return failWriteErr;
    if (attr != it->second.attr) return ERROR_INVALID_PARAMETER;
    it->second.data = data;
    if (!corruptWriteOf.empty() && name == corruptWriteOf) {
        it->second.data[it->second.data.size() / 2] ^= 0xA5;   // once: the restore that follows succeeds
        corruptWriteOf.clear();
    }
    return 0;
}

bool MockBackend::Smbios(SmbiosInfo* out, DWORD* err) {
    *out = smbios;
    if (err) *err = smbiosErr;
    return smbiosErr == 0 && smbios.ok;
}

bool MockBackend::PrivilegeOk(const ThreadPrivilege&, DWORD* err) {
    if (err) *err = privilege ? 0 : ERROR_PRIVILEGE_NOT_HELD;
    return privilege;
}

// ---- pure plan logic ----------------------------------------------------------------------------
bool ValidateValue(const Setting& s, long long v, std::wstring* why) {
    auto fail = [&](const std::wstring& w) { if (why) *why = w; return false; };
    if (s.width != 1 && s.width != 2 && s.width != 4 && s.width != 8)
        return fail(L"stored in " + std::to_wstring(s.width) + L" bytes, which Hadron doesn't write");
    if (s.kind == L"oneof") {
        if (s.opts.empty()) return fail(L"this setting has no option list");
        for (auto& o : s.opts) if (o.v == v) return true;
        return fail(L"not one of this setting's options");
    }
    if (s.kind == L"checkbox") {
        if (v == 0 || v == 1) return true;
        return fail(L"a checkbox takes 0 or 1");
    }
    if (s.kind == L"numeric") {
        if (!s.hasMin) return fail(L"the map has no range for this setting");
        if (s.mn > s.mx) return fail(L"the map's range for this setting is empty");
        if (v < s.mn || v > s.mx)
            return fail(L"outside the range " + std::to_wstring(s.mn) + L" to " + std::to_wstring(s.mx));
        if (s.step > 0 && ((unsigned long long)v - (unsigned long long)s.mn) % (unsigned long long)s.step != 0)
            return fail(L"not on the step: " + std::to_wstring(s.mn) + L" + a multiple of " + std::to_wstring(s.step));
        return true;
    }
    return fail(L"unknown setting kind \"" + s.kind + L"\"");
}

bool EncodeValue(long long v, int width, uint8_t* out) {
    if (width != 1 && width != 2 && width != 4 && width != 8) return false;
    if (v < 0) return false;
    unsigned long long u = (unsigned long long)v;
    if (width < 8 && (u >> (8 * width)) != 0) return false;
    std::vector<uint8_t> tmp((size_t)width);
    for (int i = 0; i < width; i++) tmp[(size_t)i] = (uint8_t)(u >> (8 * i));
    long long back = 0;
    if (!DecodeValue(tmp, 0, width, &back) || back != v) return false;
    std::copy(tmp.begin(), tmp.end(), out);
    return true;
}

int Plan::SettingCount() const {
    int n = 0;
    for (auto& v : vars) for (auto& s : v.settings) if (s.setting >= 0) n++;
    return n;
}

std::wstring Plan::Title() const {
    int n = SettingCount(), m = (int)vars.size();
    std::wstring vs = std::to_wstring(m) + (m == 1 ? L" variable" : L" variables");
    if (restore) return L"Restore " + vs + L" from a backup (" + std::to_wstring(n) + (n == 1 ? L" setting)" : L" settings)");
    return L"Write " + std::to_wstring(n) + (n == 1 ? L" setting" : L" settings") + L" to " + vs;
}

std::wstring HexBytes(const std::vector<uint8_t>& b, size_t max) {
    std::wstring s;
    for (size_t i = 0; i < b.size() && i < max; i++) {
        wchar_t x[4];
        swprintf(x, 4, L"%02X", b[i]);
        if (i) s += L' ';
        s += x;
    }
    if (b.size() > max) s += L" …";
    return s;
}

std::wstring Plan::Text() const {
    std::wstring t = L"Hadron BIOS " + std::wstring(restore ? L"restore" : L"write") + L" plan\r\n" + Title() + L"\r\n";
    t += L"Map: " + board + L" / " + bios + L"\r\n";
    if (restore) t += L"From backup: " + restoreFrom + L"\r\n";
    for (auto& v : vars) {
        t += L"\r\nVariable " + v.name + L" {" + v.guid + L"}  attributes " + Hex(v.attr) + L"  size " +
             std::to_wstring(v.oldData.size()) + L" bytes\r\n";
        for (auto& s : v.settings) {
            t += L"  " + s.prompt;
            if (!s.path.empty()) t += L"  [" + s.path + L"]";
            t += L"\r\n    " + s.oldText + L" -> " + s.newText + L"   offset " + Hex((unsigned)s.offset) + L", " +
                 std::to_wstring(s.width) + (s.width == 1 ? L" byte: " : L" bytes: ") + HexBytes(s.oldBytes) +
                 L" -> " + HexBytes(s.newBytes) + L"\r\n";
            if (!s.flags.empty()) t += L"    " + s.flags + L"\r\n";
            if (!s.shares.empty()) t += L"    Same bytes as: " + s.shares + L"\r\n";
        }
    }
    if (!notes.empty()) {
        t += L"\r\nNotes:\r\n";
        for (auto& n : notes) t += L"  " + n + L"\r\n";
    }
    return t;
}

bool BuildPlan(const Db& db, const std::vector<StagedChange>& staged, const std::vector<FwVar>& live,
               const PlanGate& gate, Plan* plan, std::wstring* err) {
    *plan = Plan{};
    auto fail = [&](const std::wstring& w) { if (err) *err = w; *plan = Plan{}; return false; };
    std::wstring g = GateProblem(gate);
    if (!g.empty()) return fail(g);
    if (db.board.empty() || db.bios.empty()) return fail(L"The settings map has no board/BIOS guard (v1 file).");
    if (staged.empty()) return fail(L"Nothing is staged.");
    plan->board = db.board;
    plan->bios = db.bios;
    std::vector<VarKey> order;
    for (auto& v : db.Variables()) order.push_back(MakeVarKey(v.first, v.second));
    std::map<VarKey, std::vector<StagedChange>> byVar;
    std::set<int> seen;
    std::vector<std::wstring> notes;
    for (auto& c : staged) {
        if (c.setting < 0 || c.setting >= (int)db.S.size()) return fail(L"A staged setting isn't in the map.");
        if (!seen.insert(c.setting).second) return fail(L"A setting is staged twice.");
        const Setting& s = db.S[c.setting];
        std::wstring why;
        if (!ValidateValue(s, c.value, &why)) return fail(s.prompt + L": " + std::to_wstring(c.value) + L" is refused (" + why + L").");
        uint8_t enc[8];
        if (!EncodeValue(c.value, s.width, enc))
            return fail(s.prompt + L": " + std::to_wstring(c.value) + L" doesn't fit in " + std::to_wstring(s.width) + L" bytes.");
        VarKey k = MakeVarKey(s.var, s.guid);
        if (std::find(order.begin(), order.end(), k) == order.end())
            return fail(s.prompt + L": variable " + s.var + L" isn't one of the map's variables.");
        const FwVar* v = FindLive(live, k);
        if (!v) return fail(s.prompt + L": variable " + s.var + L" wasn't read from this PC.");
        if (v->err) return fail(s.prompt + L": variable " + s.var + L" couldn't be read: " + VarErrorText(v->err) + L".");
        if (v->data.empty()) return fail(s.prompt + L": variable " + s.var + L" is empty.");
        std::wstring ap = AttrProblem(v->attr);
        if (!ap.empty()) return fail(s.prompt + L": variable " + s.var + L" is refused: " + ap + L".");
        if (s.offset < 0 || (size_t)s.offset + (size_t)s.width > v->data.size())
            return fail(s.prompt + L": offset " + Hex((unsigned)s.offset) + L" + " + std::to_wstring(s.width) +
                        L" bytes is past the end of " + s.var + L" (" + std::to_wstring(v->data.size()) + L" bytes).");
        long long cur = 0;
        DecodeValue(v->data, s.offset, s.width, &cur);
        if (cur == c.value) { notes.push_back(s.prompt + L": already " + ValText(s, cur) + L"; dropped."); continue; }
        byVar[k].push_back(c);
    }
    for (auto& k : order) {
        auto it = byVar.find(k);
        if (it == byVar.end()) continue;
        const FwVar* v = FindLive(live, k);
        PlanVar pv;
        pv.name = v->name;
        pv.guid = k.second;
        pv.attr = v->attr;
        pv.oldData = v->data;
        pv.newData = v->data;
        auto list = it->second;
        std::sort(list.begin(), list.end(), [&](const StagedChange& a, const StagedChange& b) {
            return db.S[a.setting].offset < db.S[b.setting].offset;
        });
        int prevEnd = -1;
        const Setting* prev = nullptr;
        for (auto& c : list) {
            const Setting& s = db.S[c.setting];
            if (prev && s.offset < prevEnd)
                return fail(L"\"" + prev->prompt + L"\" and \"" + s.prompt + L"\" are stored in the same bytes of " +
                            s.var + L"; stage only one of them.");
            prevEnd = s.offset + s.width;
            prev = &s;
            uint8_t enc[8];
            EncodeValue(c.value, s.width, enc);
            std::copy(enc, enc + s.width, pv.newData.begin() + s.offset);
            pv.ranges.push_back({s.offset, s.width});
        }
        for (auto& c : list) {
            const Setting& s = db.S[c.setting];
            PlanSetting ps;
            ps.setting = c.setting;
            ps.prompt = s.prompt;
            ps.path = s.path;
            DecodeValue(pv.oldData, s.offset, s.width, &ps.oldV);
            ps.newV = c.value;
            ps.oldText = ValText(s, ps.oldV);
            ps.newText = ValText(s, ps.newV);
            ps.offset = s.offset;
            ps.width = s.width;
            ps.oldBytes = Slice(pv.oldData, s.offset, s.width);
            ps.newBytes = Slice(pv.newData, s.offset, s.width);
            ps.flags = SettingFlags(s);
            ps.shares = SharedSettings(db, pv, c.setting, s.offset, s.width);
            pv.settings.push_back(std::move(ps));
        }
        if (!OnlyInRanges(pv)) return fail(L"Internal check failed: the new bytes of " + pv.name + L" differ outside the planned settings.");
        plan->vars.push_back(std::move(pv));
    }
    plan->notes = notes;
    if (plan->vars.empty()) {
        std::wstring n = L"Nothing to write: every staged value already matches.";
        *plan = Plan{};
        return fail(n);
    }
    return true;
}

bool BuildRestorePlan(const Db& db, const VarMap& backup, const std::wstring& backupDir,
                      const std::wstring& backupBoard, const std::wstring& backupBios,
                      const std::vector<FwVar>& live, const PlanGate& gate, Plan* plan, std::wstring* err) {
    *plan = Plan{};
    auto fail = [&](const std::wstring& w) { if (err) *err = w; *plan = Plan{}; return false; };
    std::wstring g = GateProblem(gate);
    if (!g.empty()) return fail(g);
    if (db.board.empty() || db.bios.empty()) return fail(L"The settings map has no board/BIOS guard (v1 file).");
    if (!BoardMatches(db.board, backupBoard) || !BiosMatches(db.bios, backupBios))
        return fail(L"This backup records " + (backupBoard.empty() ? std::wstring(L"no board") : backupBoard) + L" / " +
                    (backupBios.empty() ? std::wstring(L"no BIOS version") : backupBios) + L"; the map is for " + db.board +
                    L" / " + db.bios + L". Only a backup from this board and BIOS version can be restored.");
    plan->restore = true;
    plan->restoreFrom = backupDir;
    plan->board = db.board;
    plan->bios = db.bios;
    for (auto& mv : db.Variables()) {
        VarKey k = MakeVarKey(mv.first, mv.second);
        auto b = backup.find(k);
        if (b == backup.end()) continue;
        const FwVar* v = FindLive(live, k);
        if (!v) return fail(L"Variable " + mv.first + L" wasn't read from this PC.");
        if (v->err) return fail(L"Variable " + mv.first + L" couldn't be read: " + VarErrorText(v->err) + L".");
        std::wstring ap = AttrProblem(v->attr);
        if (!ap.empty()) return fail(L"Variable " + mv.first + L" is refused: " + ap + L".");
        if (b->second.size() != v->data.size() || v->data.empty())
            return fail(L"Variable " + mv.first + L" is " + std::to_wstring(v->data.size()) + L" bytes now but " +
                        std::to_wstring(b->second.size()) + L" bytes in the backup. A restore never changes a "
                        L"variable's size, so nothing will be restored.");
        if (b->second == v->data) { plan->notes.push_back(mv.first + L": already matches the backup."); continue; }
        PlanVar pv;
        pv.name = v->name;
        pv.guid = k.second;
        pv.attr = v->attr;
        pv.oldData = v->data;
        pv.newData = b->second;
        pv.ranges = DiffRanges(pv.oldData, pv.newData);
        std::vector<bool> covered(pv.oldData.size(), false);
        for (int i = 0; i < (int)db.S.size(); i++) {
            const Setting& s = db.S[i];
            if (s.var != pv.name || Lower(s.guid) != pv.guid) continue;
            bool hit = false;
            for (auto& r : pv.ranges) if (Overlaps(s.offset, s.width, r.first, r.second)) { hit = true; break; }
            if (!hit) continue;
            PlanSetting ps;
            ps.setting = i;
            ps.prompt = s.prompt;
            ps.path = s.path;
            bool ha = DecodeValue(pv.oldData, s.offset, s.width, &ps.oldV);
            bool hb = DecodeValue(pv.newData, s.offset, s.width, &ps.newV);
            ps.oldText = ha ? ValText(s, ps.oldV) : L"?";
            ps.newText = hb ? ValText(s, ps.newV) : L"?";
            ps.offset = s.offset;
            ps.width = s.width;
            ps.oldBytes = Slice(pv.oldData, s.offset, s.width);
            ps.newBytes = Slice(pv.newData, s.offset, s.width);
            ps.flags = SettingFlags(s);
            for (int j = 0; j < s.width; j++) if ((size_t)(s.offset + j) < covered.size()) covered[(size_t)(s.offset + j)] = true;
            pv.settings.push_back(std::move(ps));
        }
        int loose = 0, first = -1;
        for (auto& r : pv.ranges)
            for (int j = r.first; j < r.first + r.second; j++)
                if (!covered[(size_t)j]) { loose++; if (first < 0) first = j; }
        if (loose) {
            PlanSetting ps;
            ps.prompt = std::to_wstring(loose) + L" changed byte" + (loose == 1 ? L"" : L"s") + L" no mapped setting covers";
            ps.oldText = L"current";
            ps.newText = L"backup";
            ps.offset = first;
            ps.width = 1;
            ps.oldBytes = Slice(pv.oldData, first, 1);
            ps.newBytes = Slice(pv.newData, first, 1);
            ps.flags = L"Restored with the rest of the variable";
            pv.settings.push_back(std::move(ps));
        }
        std::sort(pv.settings.begin(), pv.settings.end(),
                  [](const PlanSetting& a, const PlanSetting& b) { return a.offset < b.offset; });
        plan->vars.push_back(std::move(pv));
    }
    for (auto& b : backup) {
        bool known = false;
        for (auto& mv : db.Variables()) if (MakeVarKey(mv.first, mv.second) == b.first) { known = true; break; }
        if (!known) plan->notes.push_back(b.first.first + L": not a variable of the settings map; not restored.");
    }
    if (plan->vars.empty()) {
        Plan keep = *plan;
        if (err) *err = L"Nothing to restore: every variable in the backup already matches this PC.";
        *plan = Plan{};
        plan->notes = keep.notes;
        return false;
    }
    return true;
}

// ---- execute ------------------------------------------------------------------------------------
std::wstring ExecResult::Summary() const {
    int restored = 0, failed = 0, restoreFailed = 0;
    for (auto& v : vars) {
        if (v.state == VarState::Restored) restored++;
        else if (v.state == VarState::Failed) failed++;
        else if (v.state == VarState::RestoreFailed) restoreFailed++;
    }
    std::wstring s;
    if (ok) s = std::to_wstring(written) + L" of " + std::to_wstring(vars.size()) + L" variables written and verified.";
    else s = std::to_wstring(written) + L" of " + std::to_wstring(vars.size()) + L" variables written and verified; stopped. " + error;
    if (restored) s += L" " + std::to_wstring(restored) + L" restored to the old bytes after a failed check.";
    if (failed) s += L" " + std::to_wstring(failed) + L" refused by the firmware (unchanged).";
    if (restoreFailed)
        s += L" " + std::to_wstring(restoreFailed) + L" COULD NOT BE RESTORED: restore the backup, or Clear CMOS (JBAT1) if the PC won't boot.";
    for (auto& v : vars) if (!v.text.empty()) s += L" " + v.name + L": " + v.text + L".";
    return s;
}

std::wstring DefaultBackupRoot() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) r = std::wstring(p) + L"\\Hadron\\bios-backups";
    if (p) CoTaskMemFree(p);
    return r;
}

bool CreateBackupSession(const std::wstring& root, const std::wstring& stamp, std::wstring* dir, std::wstring* err) {
    if (root.empty()) { *err = L"No backup folder (the local app data folder couldn't be resolved)."; return false; }
    if (!EnsureDir(root)) { *err = L"Can't create " + sys::ToShell(root) + L": " + sys::ErrorText(GetLastError()); return false; }
    std::wstring base = root;
    if (base.back() != L'\\') base += L'\\';
    for (int n = 1; n < 1000; n++) {
        std::wstring cand = base + stamp + (n > 1 ? L"-" + std::to_wstring(n) : L"");
        if (CreateDirectoryW(sys::ToExtended(cand).c_str(), nullptr)) { *dir = cand; return true; }
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) { *err = L"Can't create " + sys::ToShell(cand) + L": " + sys::ErrorText(e); return false; }
    }
    *err = L"Too many backup sessions with the stamp " + stamp + L".";
    return false;
}

namespace {
// Writes oldData back after a failed check and verifies it. True when the variable holds oldData again.
bool PutBack(FwBackend& be, const PlanVar& pv, DWORD* err) {
    DWORD e = be.Write(pv.name, pv.guid, pv.oldData, pv.attr);
    FwVar rb;
    rb.name = pv.name; rb.guid = pv.guid;
    be.Read(&rb);
    *err = e ? e : rb.err;
    return !e && !rb.err && rb.attr == pv.attr && rb.data == pv.oldData;
}
}  // namespace

bool Execute(FwBackend& be, const Plan& plan, const std::wstring& backupRoot, const std::function<bool()>& cancelled,
             const ProgressFn& progress, ExecResult* out, const ExecOptions& opt) {
    *out = ExecResult{};
    for (auto& pv : plan.vars) { VarOutcome o; o.name = pv.name; o.guid = pv.guid; out->vars.push_back(o); }
    const int total = (int)plan.vars.size();
    auto stop = [&](const std::wstring& e) {
        out->error = e;
        LogLine(LogLevel::Error, e);
        return false;
    };
    auto prog = [&](int done, const std::wstring& what) { if (progress) progress(done, total, what); };
    if (plan.vars.empty()) return stop(L"The plan is empty.");
    // Static checks again (defence in depth): same size, never 0, attributes exactly NV|BS|RT, only
    // the planned bytes change.
    for (auto& pv : plan.vars) {
        if (pv.oldData.empty() || pv.newData.size() != pv.oldData.size())
            return stop(L"Internal check failed for " + pv.name + L": the new data isn't the same size. Nothing was written.");
        if (pv.attr != kAttrRequired) return stop(L"Internal check failed for " + pv.name + L": attributes " + Hex(pv.attr) + L". Nothing was written.");
        if (!OnlyInRanges(pv)) return stop(L"Internal check failed for " + pv.name + L": bytes outside the plan change. Nothing was written.");
    }
    if (plan.board.empty() || plan.bios.empty()) return stop(L"The plan has no board/BIOS guard. Nothing was written.");

    // The privilege lives on this worker thread only, for the whole run, and is required.
    ThreadPrivilege priv(SE_SYSTEM_ENVIRONMENT_NAME);
    DWORD perr = 0;
    if (!be.PrivilegeOk(priv, &perr))
        return stop(L"SeSystemEnvironmentPrivilege couldn't be enabled on the write thread (" + sys::ErrorText(perr) +
                    L"). Nothing was written.");

    // 1. Board/BIOS guard, from SMBIOS now.
    prog(0, L"Checking the board and BIOS version");
    SmbiosInfo smb;
    DWORD se = 0;
    if (!be.Smbios(&smb, &se)) return stop(L"Couldn't read SMBIOS (" + VarErrorText(se) + L"), so the board can't be checked. Nothing was written.");
    if (!BoardMatches(plan.board, smb.boardProduct) || !BiosMatches(plan.bios, smb.biosVersion))
        return stop(L"This PC reports " + smb.boardProduct + L" / " + smb.biosVersion + L", not " + plan.board + L" / " +
                    plan.bios + L". Nothing was written.");
    // 2. Drift: every byte, the size and the attributes, against the bytes the plan was built from.
    std::vector<FwVar> current;
    for (auto& pv : plan.vars) {
        FwVar v;
        v.name = pv.name; v.guid = pv.guid;
        be.Read(&v);
        if (v.err) return stop(L"Couldn't re-read " + pv.name + L": " + VarErrorText(v.err) + L". Nothing was written.");
        if (v.attr != pv.attr || v.data.size() != pv.oldData.size() || v.data != pv.oldData)
            return stop(pv.name + L" changed since it was read; re-read and plan again. Nothing was written.");
        current.push_back(std::move(v));
    }
    if (cancelled && cancelled()) return stop(L"Cancelled before writing. Nothing was written.");
    // 3. Backup of every planned variable, flushed, before the first write; then read it back.
    prog(0, L"Saving the backup");
    std::wstring stamp = opt.stamp;
    if (stamp.empty()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t b[32];
        swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        stamp = b;
    }
    std::wstring dir, e;
    if (!CreateBackupSession(backupRoot, stamp, &dir, &e)) return stop(L"Backup failed: " + e + L" Nothing was written.");
    out->backupDir = sys::ToShell(sys::ToExtended(dir));
    out->started = true;
    SaveResult sr;
    sr.folder = out->backupDir;
    if (!WriteSnapshotFiles(dir, current, smb,
                            plan.restore ? L"Hadron BIOS module (backup before a restore)" : L"Hadron BIOS module (backup before a write)",
                            &sr))
        return stop(L"Backup failed: " + sr.error + L" Nothing was written.");
    if (!WriteNewFile(dir + L"\\plan.txt", "\xEF\xBB\xBF" + ToUtf8(plan.Text()), &e))
        return stop(L"Backup failed: " + e + L" Nothing was written.");
    {
        SnapLoad chk;
        if (!LoadSnapshot(dir, &chk)) return stop(L"The backup doesn't read back (" + chk.error + L"). Nothing was written.");
        for (auto& v : current) {
            auto it = chk.vars.find(MakeVarKey(v.name, v.guid));
            if (it == chk.vars.end() || it->second != v.data)
                return stop(L"The backup of " + v.name + L" doesn't read back equal. Nothing was written.");
        }
    }
    Journal j;
    if (!j.Open(dir + L"\\journal.txt", &e)) return stop(e + L" Nothing was written.");
    LogLine(LogLevel::Info, L"backup saved to " + out->backupDir + L"\r\n" + plan.Text());

    // 4-6, per variable, stopping at the first failure.
    bool ok = true;
    for (int i = 0; i < total; i++) {
        const PlanVar& pv = plan.vars[(size_t)i];
        VarOutcome& vo = out->vars[(size_t)i];
        if (cancelled && cancelled()) { ok = false; out->error = L"Cancelled; the remaining variables weren't written."; break; }
        prog(i, L"Writing " + pv.name);
        // Drift again, right before this write (the window since step 2 includes the backup).
        FwVar now;
        now.name = pv.name; now.guid = pv.guid;
        be.Read(&now);
        if (now.err || now.attr != pv.attr || now.data != pv.oldData) {
            ok = false;
            out->error = pv.name + L" changed since it was read; it and the rest weren't written. Re-read and plan again.";
            break;
        }
        if (!j.Line(L"BEGIN " + pv.name + L" " + pv.guid, &e)) { ok = false; out->error = e; break; }
        LogLine(LogLevel::Info, L"writing " + pv.name + L" {" + pv.guid + L"} " + std::to_wstring(pv.newData.size()) + L" bytes, attributes " + Hex(pv.attr));
        if (pv.newData.empty()) { ok = false; out->error = L"Internal check failed: size 0."; break; }   // never delete
        DWORD we = be.Write(pv.name, pv.guid, pv.newData, pv.attr);
        if (we) {
            vo.err = we;
            FwVar rb;
            rb.name = pv.name; rb.guid = pv.guid;
            be.Read(&rb);
            if (!rb.err && rb.attr == pv.attr && rb.data == pv.oldData) {
                vo.state = VarState::Failed;
                vo.text = L"write refused, " + WriteErrorText(we) + L"; the variable is unchanged";
                j.Line(L"FAILED " + pv.name + L" " + std::to_wstring(we), &e);
            } else {
                DWORD re = 0;
                bool back = PutBack(be, pv, &re);
                vo.state = back ? VarState::Restored : VarState::RestoreFailed;
                vo.text = L"write failed, " + WriteErrorText(we) + (back ? L"; the old bytes were restored and verified"
                                                                       : L"; restoring the old bytes failed (" + WriteErrorText(re) + L")");
                j.Line(back ? L"RESTORED " + pv.name : L"RESTORE-FAILED " + pv.name + L" " + std::to_wstring(re), &e);
            }
            LogLine(LogLevel::Error, pv.name + L": " + vo.text);
            ok = false;
            out->error = L"The write of " + pv.name + L" failed; the rest weren't written.";
            break;
        }
        FwVar rb;
        rb.name = pv.name; rb.guid = pv.guid;
        be.Read(&rb);
        if (rb.err || rb.attr != pv.attr || rb.data != pv.newData) {
            DWORD re = 0;
            bool back = PutBack(be, pv, &re);
            vo.state = back ? VarState::Restored : VarState::RestoreFailed;
            vo.err = rb.err;
            vo.text = std::wstring(L"the read-back didn't match the written bytes") +
                      (back ? L"; the old bytes were restored and verified" : L"; restoring the old bytes failed (" + WriteErrorText(re) + L")");
            j.Line(back ? L"RESTORED " + pv.name : L"RESTORE-FAILED " + pv.name + L" " + std::to_wstring(re), &e);
            LogLine(LogLevel::Error, pv.name + L": " + vo.text);
            ok = false;
            out->error = L"Verifying " + pv.name + L" failed; the rest weren't written.";
            break;
        }
        vo.state = VarState::Written;
        out->written++;
        LogLine(LogLevel::Info, pv.name + L" written and verified");
        if (!j.Line(L"OK " + pv.name, &e)) { ok = false; out->error = e; break; }
    }
    j.Line(L"END", &e);
    prog(total, L"Done");
    out->ok = ok && out->written == total;
    if (!out->ok && out->error.empty()) out->error = L"Stopped.";
    LogLine(out->ok ? LogLevel::Info : LogLevel::Error, out->Summary());
    return out->ok;
}

// ---- journals -----------------------------------------------------------------------------------
bool JournalIncomplete(const std::string& journal, std::vector<std::wstring>* vars) {
    std::wstring t = FromUtf8(journal);
    std::vector<std::wstring> open;
    size_t a = 0;
    while (a < t.size()) {
        size_t b = t.find(L'\n', a);
        if (b == std::wstring::npos) b = t.size();
        std::wstring line = t.substr(a, b - a);
        a = b + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);
        size_t sp = line.find(L' ');
        if (sp == std::wstring::npos) continue;
        std::wstring kw = line.substr(0, sp), rest = line.substr(sp + 1);
        auto lastWordCut = [](const std::wstring& r) {
            size_t k = r.find_last_of(L' ');
            return k == std::wstring::npos ? r : r.substr(0, k);
        };
        if (kw == L"BEGIN") open.push_back(lastWordCut(rest));
        else if (kw == L"OK" || kw == L"RESTORED" || kw == L"FAILED") {
            std::wstring name = kw == L"FAILED" ? lastWordCut(rest) : rest;
            auto it = std::find(open.begin(), open.end(), name);
            if (it != open.end()) open.erase(it);
        }
        // RESTORE-FAILED leaves the variable open: it needs a restore.
    }
    if (vars) *vars = open;
    return !open.empty();
}

bool FindIncompleteSession(const std::wstring& root, IncompleteSession* out, int newest) {
    *out = IncompleteSession{};
    if (root.empty()) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(sys::ToExtended(root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<std::wstring> names;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.rbegin(), names.rend());
    for (int i = 0; i < (int)names.size() && i < newest; i++) {
        std::wstring dir = root + L"\\" + names[(size_t)i];
        HANDLE f = CreateFileW(sys::ToExtended(dir + L"\\journal.txt").c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;   // a session that stopped before writing has no journal
        std::string text(64 * 1024, '\0');
        DWORD got = 0;
        BOOL r = ReadFile(f, &text[0], (DWORD)text.size(), &got, nullptr);
        CloseHandle(f);
        if (!r) return false;
        text.resize(got);
        // Only the newest session with a journal counts: a later write's backup already holds the state
        // an older incomplete session left, so offering the older backup would undo that later write.
        std::vector<std::wstring> vars;
        if (!JournalIncomplete(text, &vars)) return false;
        out->dir = dir;
        out->stamp = names[(size_t)i];
        out->vars = vars;
        return true;
    }
    return false;
}

}  // namespace hx::bios
