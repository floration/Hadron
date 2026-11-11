// core/host/run.h — entry points and the module content-window helper.
#pragma once

#include "core/host/module.h"
#include <memory>
#include <vector>

namespace hx {

// Standalone exe: run one module with no sidebar. Owns the message loop.
int RunStandalone(std::unique_ptr<Module> module);

// Suite exe: run every module behind a sidebar. The vector order is the sidebar order.
int RunSuite(std::vector<std::unique_ptr<Module>> modules);

// Helper a module may call from Create() to get a back-buffered content child window whose paint
// routes to Module::Paint (so WM_PAINT and --screenshot render identical pixels). The host owns the
// window class "Hadron.<id>.Content" and keeps the HWND alive until Shutdown() returns. A module can
// create its own children (search box, list) as children of this window instead.
HWND CreateModuleContent(IHost& host, HWND parent, const RECT& rc, Module* module);

}  // namespace hx
