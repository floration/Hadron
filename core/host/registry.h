// core/host/registry.h — explicit module factories. No static self-registration (ARCHITECTURE.md).
#pragma once

#include "core/host/module.h"
#include <memory>
#include <vector>

namespace hx {

std::unique_ptr<Module> MakeDiskMapModule();
std::unique_ptr<Module> MakeRunAsModule();
std::unique_ptr<Module> MakeBiosModule();
std::unique_ptr<Module> MakeServicesModule();

// Every module the suite shows, in sidebar order. app/hadron.cpp and the suite tests use this.
std::vector<std::unique_ptr<Module>> MakeAllModules();

}  // namespace hx
