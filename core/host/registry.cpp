// core/host/registry.cpp — the one list of modules in the suite. Explicit, in sidebar order.
#include "core/host/registry.h"

namespace hx {

std::vector<std::unique_ptr<Module>> MakeAllModules() {
    std::vector<std::unique_ptr<Module>> v;
    v.push_back(MakeHelloModule());
    // Ported modules append here: MakeDiskMapModule(), MakeBiosModule(), MakeRunAsModule().
    return v;
}

}  // namespace hx
