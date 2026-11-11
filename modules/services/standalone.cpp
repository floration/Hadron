// modules/services/standalone.cpp — HadronServices.exe entry point.
//
// --selftest and --screenshot go through the host (Module::RunCli / Module::Paint). --dump-plan (golden plan
// text from a fixture) and --live-test (CI's dummy-service round trip) are headless modes the host does not
// route, so they are intercepted here before the GUI starts, as runas does with --launch-test.
#include "core/host/run.h"
#include "core/host/registry.h"
#include "modules/services/cli.h"
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; i++) args.push_back(argv[i]);
    if (argv) LocalFree(argv);
    for (auto& a : args) {
        if (a == L"--dump-plan" || a == L"--live-test") {
            AttachConsole(ATTACH_PARENT_PROCESS);
            int rc = a == L"--dump-plan" ? hx::services::RunDumpPlan(args) : hx::services::RunLiveTest(args);
            fflush(stdout);
            return rc;
        }
    }
    return hx::RunStandalone(hx::MakeServicesModule());
}
