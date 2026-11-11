#include "modules/services/plan.h"
#include <chrono>
#include <cstdio>
#include <random>
using namespace hx::services;
static double Ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }
int wmain() {
    // 750 items; 100 oscillating pairs (raise Y needs X; disable X) + a 200-row disable chain; every row in one plan.
    std::vector<SvcItem> items; Plan p; p.kind = PlanKind::Preset;
    auto add = [&](std::wstring n, int st, std::vector<std::wstring> d) { SvcItem s; s.name = n; s.type = 0x10; s.kind = Kind::Service; s.cfg = {st, -1}; s.deps = d; items.push_back(s); };
    auto row = [&](std::wstring n, int b, int a, std::vector<std::wstring> d) { PlanRow r; r.name = n; r.before = {b, -1}; r.after = {a, -1}; r.op = {a, -1}; r.deps = d; p.rows.push_back(r); };
    for (int i = 0; i < 100; i++) {
        std::wstring x = L"x" + std::to_wstring(i), y = L"y" + std::to_wstring(i);
        add(x, 3, {}); add(y, 4, {x}); row(x, 3, 4, {}); row(y, 4, 3, {x});
    }
    for (int i = 0; i < 200; i++) { std::wstring c = L"c" + std::to_wstring(i); std::vector<std::wstring> d; if (i > 0) d.push_back(L"c" + std::to_wstring(i - 1));
        add(c, 3, d); row(c, 3, 4, d); }
    add(L"top", 3, {L"c199"}); for (int i = 0; i < 350; i++) add(L"pad" + std::to_wstring(i), 3, {});   // keeps the whole chain from being disabled -> cascade one link per round
    for (int i = 0; i < 0; i++) add(L"pad" + std::to_wstring(i), 3, {L"c" + std::to_wstring(i % 200)});
    auto t0 = std::chrono::steady_clock::now();
    DependencyCheck(&p, items);
    printf("build check: %zu rows, %zu items: %.1f ms, note: %ls\n", p.rows.size(), items.size(), Ms(t0), p.notes.empty() ? L"(none)" : p.notes[0].substr(0, 40).c_str());
    std::mt19937 rng(5);
    double worst = 0;
    for (int t = 0; t < 40; t++) {
        int k = rng() % p.rows.size();
        p.rows[k].include = !p.rows[k].include;
        t0 = std::chrono::steady_clock::now();
        DependencyCheck(&p, items, k);
        double m = Ms(t0); if (m > worst) worst = m;
    }
    printf("toggle worst of 40: %.1f ms\n", worst);
    return 0;
}
