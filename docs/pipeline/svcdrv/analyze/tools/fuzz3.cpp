// Round 3 fuzz of DependencyCheck: several seeds, long chains, cycles, groups, toggle sequences. Fake data only.
#include "modules/services/plan.h"
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <algorithm>
using namespace hx::services;

struct Stats { long plans = 0, badBuild = 0, badTog = 0, warnedTickedNoUser = 0, tickedFromNowhere = 0, fallback = 0, afterChanged = 0; double worst = 0; };

// Violations against the final end state: a ticked row without a dependency warning that disables something an
// end-enabled item depends on (directly or via +group), or raises something whose dependency (or whole group) ends Disabled.
static bool Violates(const Plan& q, const std::vector<SvcItem>& items, const std::set<int>& userRows) {
    std::map<std::wstring, int> fin;
    for (auto& s : items) fin[s.name] = s.cfg.start;
    for (auto& r : q.rows) if (r.blocked.empty() && r.include) fin[r.name] = r.after.start;
    auto groupOf = [&](const std::wstring& n) { for (auto& s : items) if (s.name == n) return s.group; return std::wstring(); };
    for (size_t k = 0; k < q.rows.size(); k++) {
        const PlanRow& r = q.rows[k];
        if (!r.include || !r.blocked.empty()) continue;
        if (!r.depWarning.empty()) { if (!userRows.count((int)k)) return true; continue; }   // a warned ticked row must be the user's
        if (r.after.start == 4) {
            std::wstring g = groupOf(r.name);
            for (auto& s : items) {
                if (s.name == r.name || fin[s.name] == 4) continue;
                for (auto& d : s.deps) {
                    if (d == r.name) return true;
                    if (!g.empty() && d == L"+" + g) return true;
                }
            }
        }
        if (r.after.start < r.before.start) {
            for (auto& s : items) if (s.name == r.name)
                for (auto& d : s.deps) {
                    if (d[0] == L'+') {
                        int mem = 0, off = 0;
                        for (auto& t : items) if (L"+" + t.group == d) { mem++; if (fin[t.name] == 4) off++; }
                        if (mem && mem == off) return true;
                    } else if (fin[d] == 4) return true;
                }
        }
    }
    return false;
}

static void Run(unsigned seed, int maxN, bool lowerOnly, bool chains, int iters, Stats* st) {
    std::mt19937 rng(seed);
    for (int it = 0; it < iters; it++) {
        int n = 2 + rng() % (maxN - 1);
        std::vector<SvcItem> items(n);
        int groups = 1 + rng() % 3;
        for (int i = 0; i < n; i++) {
            items[i].name = L"s" + std::to_wstring(i); items[i].type = 0x10; items[i].kind = Kind::Service;
            items[i].cfg = StartCfg{2 + (int)(rng() % 3), -1};
            if (rng() % 4 == 0) items[i].group = L"g" + std::to_wstring(rng() % groups);
            if (chains) {
                if (i + 1 < n && rng() % 5) items[i].deps.push_back(L"s" + std::to_wstring(i + 1));   // long chains
                if (rng() % 6 == 0) items[i].deps.push_back(L"s" + std::to_wstring(rng() % n));       // back edges -> cycles
            } else {
                for (int j = 0; j < n; j++) if (j != i && rng() % 3 == 0) items[i].deps.push_back(L"s" + std::to_wstring(j));
            }
            if (rng() % 8 == 0) items[i].deps.push_back(L"+g" + std::to_wstring(rng() % groups));
            // no self-deps
            items[i].deps.erase(std::remove(items[i].deps.begin(), items[i].deps.end(), items[i].name), items[i].deps.end());
        }
        Plan p; p.kind = PlanKind::Preset;
        for (int i = 0; i < n; i++) {
            if (rng() % 4 == 0) continue;
            int t = 2 + rng() % 3;
            if (lowerOnly ? t <= items[i].cfg.start : t == items[i].cfg.start) continue;
            PlanRow r; r.name = items[i].name; r.before = items[i].cfg; r.after = {t, -1}; r.op = {t, -1}; r.deps = items[i].deps;
            r.include = rng() % 4 != 0;
            if (rng() % 15 == 0) { r.blocked = L"blocked"; r.include = false; r.canToggle = false; }
            p.rows.push_back(r);
        }
        if (p.rows.empty()) continue;
        st->plans++;
        std::vector<bool> inc0; for (auto& r : p.rows) inc0.push_back(r.include);
        std::vector<StartCfg> after0; for (auto& r : p.rows) after0.push_back(r.after);
        auto t0 = std::chrono::steady_clock::now();
        DependencyCheck(&p, items);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > st->worst) st->worst = ms;
        bool fb = false; for (auto& nt : p.notes) if (nt.find(L"did not settle") != std::wstring::npos) fb = true;
        if (fb) st->fallback++;
        for (size_t k = 0; k < p.rows.size(); k++) {
            if (p.rows[k].include && !inc0[k]) st->tickedFromNowhere++;   // a fresh plan: the check must never tick a row the builder left unticked
            if (!(p.rows[k].after == after0[k])) st->afterChanged++;
        }
        std::set<int> user;
        if (Violates(p, items, user)) st->badBuild++;
        for (int t = 0; t < 6; t++) {
            int k = rng() % p.rows.size();
            if (!p.rows[k].canToggle || !p.rows[k].blocked.empty()) continue;
            p.rows[k].include = !p.rows[k].include;
            std::vector<bool> before; for (auto& r : p.rows) before.push_back(r.include);
            std::vector<bool> du; for (auto& r : p.rows) du.push_back(r.depUnticked);
            t0 = std::chrono::steady_clock::now();
            DependencyCheck(&p, items, k);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > st->worst) st->worst = ms;
            for (size_t j = 0; j < p.rows.size(); j++) if (p.rows[j].include && !before[j] && !du[j]) st->tickedFromNowhere++;
            user.clear();
            for (size_t j = 0; j < p.rows.size(); j++) if (p.rows[j].depKept || (int)j == k) user.insert((int)j);
            if (Violates(p, items, user)) st->badTog++;
        }
    }
}

int wmain() {
    unsigned seeds[] = {12345, 1, 777, 20261111};
    for (unsigned sd : seeds)
        for (int mode = 0; mode < 4; mode++) {
            Stats s;
            bool lower = mode & 1, chains = mode & 2;
            Run(sd, chains ? 14 : 7, lower, chains, 15000, &s);
            printf("seed %-9u %-10s %-6s plans %6ld  bad-build %ld  bad-toggle %ld  ticked-from-nowhere %ld  after-changed %ld  fallback %ld  worst %.2f ms\n",
                   sd, lower ? "lower-only" : "mixed", chains ? "chains" : "random", s.plans, s.badBuild, s.badTog, s.tickedFromNowhere,
                   s.afterChanged, s.fallback, s.worst);
            fflush(stdout);
        }
    return 0;
}
