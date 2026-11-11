// Random small plans: does DependencyCheck always end consistent? Fake data only.
#include "modules/services/plan.h"
#include <cstdio>
#include <map>
#include <random>
#include <chrono>
using namespace hx::services;
int wmain() {
    std::mt19937 rng(12345);
    int bad = 0, total = 0, badTog = 0, maxRoundsCase = 0, badDis = 0, badRaise = 0;
    double worst = 0;
    for (int it = 0; it < 60000; it++) { try {
        int n = 2 + rng() % 5;
        std::vector<SvcItem> items(n);
        for (int i = 0; i < n; i++) {
            items[i].name = L"s" + std::to_wstring(i); items[i].type = 0x10; items[i].kind = Kind::Service;
            items[i].cfg = StartCfg{2 + (int)(rng() % 3), -1};
            for (int j = 0; j < n; j++) if (j != i && rng() % 3 == 0) items[i].deps.push_back(L"s" + std::to_wstring(j));
        }
        Plan p; p.kind = PlanKind::Preset;
        for (int i = 0; i < n; i++) {
            if (rng() % 3 == 0) continue;
            int t = 2 + rng() % 3;
            if (t <= items[i].cfg.start) continue;
            PlanRow r; r.name = items[i].name; r.before = items[i].cfg; r.after = {t, -1}; r.op = {t, -1}; r.deps = items[i].deps;
            r.include = rng() % 4 != 0;
            p.rows.push_back(r);
        }
        if (p.rows.empty()) continue;
        total++;
        auto t0 = std::chrono::steady_clock::now();
        try { DependencyCheck(&p, items); } catch (std::exception& e) { printf("EXC in DependencyCheck at it=%d: %s\n", it, e.what()); return 1; }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > worst) worst = ms;
        // consistency: recompute the warnings on the final state; a ticked row must have none
        // Independent evaluator: the end state of the ticked rows; a ticked row is inconsistent if it disables something an
        // enabled-at-the-end item depends on, or raises something whose dependency ends Disabled.
        auto check = [&](const Plan& q, int toggled) {
            std::map<std::wstring, int> fin;
            for (auto& s : items) fin[s.name] = s.cfg.start;
            for (auto& r : q.rows) if (r.include) fin[r.name] = r.after.start;
            for (size_t k = 0; k < q.rows.size(); k++) {
                const PlanRow& r = q.rows[k];
                if (!r.include || (int)k == toggled || !r.depWarning.empty()) continue;   // user overrides carry a warning
                if (r.after.start == 4)
                    for (auto& s : items) { if (s.name == r.name) continue; for (auto& d : s.deps) if (d == r.name && fin[s.name] != 4) { badDis++; return false; } }
                if (r.after.start < r.before.start)
                    for (auto& s : items) if (s.name == r.name) for (auto& d : s.deps) if (fin[d] == 4) { badRaise++; return false; }
            }
            return true;
        };
        if (!check(p, -1)) {
            bad++;
            if (false) {
                printf("case %d: n=%d\n", it, n);
                for (auto& s : items) { printf("  item %ls start %d deps:", s.name.c_str(), s.cfg.start); for (auto& d : s.deps) printf(" %ls", d.c_str()); printf("\n"); }
                for (auto& r : p.rows) printf("  row %ls %d->%d include=%d dep='%ls'\n", r.name.c_str(), r.before.start, r.after.start, r.include, r.depWarning.c_str());
            }
        }
        // a toggle afterwards
        int k = rng() % p.rows.size();
        p.rows[k].include = !p.rows[k].include;
        try { DependencyCheck(&p, items, k); } catch (std::exception& e) { printf("EXC in toggle at it=%d: %s\n", it, e.what()); return 1; }
        if (!check(p, k)) badTog++;
    } catch (std::exception& e) { printf("EXC elsewhere at it=%d: %s\n", it, e.what()); return 1; } }
    printf("disable-side %d raise-side %d plans %d build %d toggle %d\n", badDis, badRaise, total, bad, badTog); return 0;
}
