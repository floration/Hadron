// modules/services/kb.cpp — parser for the embedded services.db (own parser, rule 3).
#include "modules/services/kb.h"
#include "modules/services/model.h"
#include "core/sys/sys.h"

namespace hx::services {

namespace {
constexpr size_t kKbFields = 26;
constexpr size_t kMaxKb = 16u << 20;

int Target(const std::wstring& s) {
    if (s.empty()) return -1;
    if (s == L"3") return 3;
    if (s == L"4") return 4;
    return -2;   // invalid
}
}  // namespace

bool Kb::Parse(const std::string& utf8, std::wstring* err) {
    entries.clear();
    index_.clear();
    if (utf8.size() > kMaxKb) { *err = L"services.db is larger than 16 MB"; return false; }
    std::wstring all = FromUtf8(utf8);
    std::vector<std::wstring> lines = Split(all, L'\n');
    int ln = 0;
    bool haveV = false;
    for (auto& raw : lines) {
        ln++;
        std::wstring line = raw;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::wstring> f = Split(line, L'\t');
        if (f[0] == L"V") {
            if (f.size() < 3 || f[1] != L"1" || f[2] != L"services") {
                *err = L"services.db line " + std::to_wstring(ln) + L": unknown version";
                return false;
            }
            version = f[1];
            haveV = true;
        } else if (f[0] == L"B" && f.size() >= 2) {
            build = f[1];
        } else if (f[0] == L"K") {
            if (f.size() != kKbFields) {
                *err = L"services.db line " + std::to_wstring(ln) + L": K line has " + std::to_wstring(f.size()) +
                       L" fields, expected " + std::to_wstring(kKbFields);
                return false;
            }
            KbEntry e;
            e.name = f[1];
            e.kind = f[2];
            for (auto& b : Split(f[3], L',')) if (!b.empty()) e.builds.push_back(b);
            e.title = Unesc(f[4]);
            e.expl = Unesc(f[5]);
            e.ifOff = Unesc(f[6]);
            e.test = Unesc(f[7]);
            for (int i = 0; i < LV_COUNT; i++) {
                e.lv[i] = Target(f[8 + i]);
                if (e.lv[i] == -2) {
                    *err = L"services.db line " + std::to_wstring(ln) + L": level target must be 3 or 4";
                    return false;
                }
                e.why[i] = Unesc(f[11 + i]);
            }
            e.sysok = f[14] == L"1";
            e.never = f[15] == L"1";
            e.basis = f[16];
            e.basisNote = Unesc(f[17]);
            e.pbasis = f[18];
            e.pbasisNote = Unesc(f[19]);
            if (!f[20].empty()) {
                StartCfg c;
                if (ParseCfg(f[20], &c)) { e.stock = c.start; e.stockDelayed = c.delayed == 1 ? 1 : 0; }
            }
            e.stockBasis = f[21];
            e.stockNote = Unesc(f[22]);
            e.hw = f[23];
            e.contested = Unesc(f[24]);
            e.sources = Unesc(f[25]);
            if (e.name.empty() || (e.kind != L"svc" && e.kind != L"drv")) {
                *err = L"services.db line " + std::to_wstring(ln) + L": bad name or kind";
                return false;
            }
            index_[Lower(e.name)] = entries.size();
            entries.push_back(std::move(e));
        }
        // Unknown line kinds are skipped (forward compatibility).
    }
    if (!haveV) { *err = L"services.db has no V line"; return false; }
    return true;
}

const KbEntry* Kb::Find(const std::wstring& name) const {
    auto it = index_.find(Lower(name));
    return it == index_.end() ? nullptr : &entries[it->second];
}

bool LoadEmbeddedKb(Kb* kb, std::wstring* err) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(mod, L"SERVICES_KB", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) { *err = L"The embedded knowledge base (SERVICES_KB) is missing: " + sys::ErrorText(GetLastError()); return false; }
    DWORD sz = SizeofResource(mod, r);
    HGLOBAL g = LoadResource(mod, r);
    const char* p = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    if (!p || !sz) { *err = L"Could not load the embedded knowledge base: " + sys::ErrorText(GetLastError()); return false; }
    return kb->Parse(std::string(p, sz), err);
}

std::wstring LevelName(int level) {
    switch (level) {
        case LV_BLOAT: return L"Pure bloat removal";
        case LV_STRIPPED: return L"Stripped";
        case LV_MINIMUM: return L"Minimum";
    }
    return L"Default";
}

std::wstring BasisLabel(const std::wstring& basis) {
    if (basis == L"owner-measured") return L"measured by Hadron's author";
    if (basis == L"published") return L"published";
    if (basis == L"reasoning") return L"reasoning (how Windows works, no data)";
    if (basis == L"folklore") return L"folklore (repeated, no evidence)";
    return basis.empty() ? L"none" : basis;
}

}  // namespace hx::services
