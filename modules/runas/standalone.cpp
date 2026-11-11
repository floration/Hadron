// modules/runas/standalone.cpp — HadronRunAs.exe entry point.
//
// --selftest and --screenshot are handled by the host (via Module::RunCli / Module::Paint).
// --launch-test is a headless check the host does not route, so it is intercepted here before the
// GUI starts (it must not open a window), exactly as CI invokes `HadronRunAs.exe --launch-test …`.
#include "core/host/run.h"
#include "core/host/registry.h"
#include "modules/runas/launch.h"
#include <shellapi.h>
#include <string>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (std::wstring(argv[i]) == L"--launch-test") {
            std::wstring mode = (i + 1 < argc) ? argv[i + 1] : L"limited";
            AttachConsole(ATTACH_PARENT_PROCESS);
            int rc = hx::runas::RunLaunchTest(mode);
            fflush(stdout);
            LocalFree(argv);
            return rc;
        }
    }
    if (argv) LocalFree(argv);
    return hx::RunStandalone(hx::MakeRunAsModule());
}
