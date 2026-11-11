// modules/uikit/standalone.cpp — HadronUikit.exe, the core/ui gallery. A reference and screenshot
// target, not a suite tool, so its factory is declared here rather than in the shared registry.
#include "core/host/run.h"
#include <memory>

namespace hx { std::unique_ptr<Module> MakeUikitModule(); }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return hx::RunStandalone(hx::MakeUikitModule());
}
