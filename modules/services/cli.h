// modules/services/cli.h — headless modes of the Services page (selftest.cpp).
//
// --selftest runs through Module::RunCli (the host routes it). --dump-plan and --live-test are not routed by
// the host, so standalone.cpp intercepts them before the GUI starts (runas --launch-test pattern).
#pragma once

#include "modules/services/backend.h"
#include "modules/services/kb.h"
#include "modules/services/store.h"
#include <functional>
#include <string>
#include <vector>

namespace hx::services {

// Everything a fixture folder holds: snapshot.txt (the machine), hw.txt, first.txt and any undo logs.
struct FixtureData {
    FakeBackend backend;
    Snapshot first;
    bool hasFirst = false;
    std::vector<std::wstring> problems;
};
bool LoadFixtureDir(const std::wstring& dir, FixtureData* out, std::wstring* err);

// The value of "--name value" in args, or "".
std::wstring ArgValue(const std::vector<std::wstring>& args, const wchar_t* name);
bool HasArg(const std::vector<std::wstring>& args, const wchar_t* name);

// The golden text of one preset plan on a fixture folder (MarkTi with the toggle off); "" and *err on failure.
std::string DumpFixturePlan(const std::wstring& fixture, int level, int tab, std::wstring* err);

// The page's own checks (module.cpp: plan ticks across a rebuild, the [x] click, the result banner's Undo, labels,
// layout at the minimum window size), run by RunSelftest with the fixture folder and its check reporter.
using UiChecks = std::function<void(const std::wstring& fixture, const std::function<void(bool, const std::wstring&)>& check)>;
int RunSelftest(const std::vector<std::wstring>& args, const UiChecks& ui = {});    // exit 0 = every check passed
// --dump-plan <bloat|stripped|minimum|default|stock> --tab <services|drivers> --fixture <dir> [--out <file>]
int RunDumpPlan(const std::vector<std::wstring>& args);
// --live-test <HadronTest*> [--ti] [--data-dir <dir>]: plan, apply, verify, undo, verify on one dummy
// service that the CI script created. Refuses any other name. Only in a -DHADRON_SERVICES_TEST_HOOKS=ON build (R25):
// a release build prints that it is not in this build and returns 2.
int RunLiveTest(const std::vector<std::wstring>& args);

// Preset level from its CLI name: 0..2 for the KB levels, LEVEL_DEFAULT for "default" (first snapshot),
// -2 for "stock", -3 when unknown.
int LevelFromName(const std::wstring& s);

}  // namespace hx::services
