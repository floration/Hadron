// core/host/registry.cpp — the one list of modules in the suite. Explicit, in sidebar order.
#include "core/host/registry.h"

namespace hx {

std::vector<std::unique_ptr<Module>> MakeAllModules() {
    std::vector<std::unique_ptr<Module>> v;
    v.push_back(MakeHelloModule());
    v.push_back(MakeDiskMapModule());
    v.push_back(MakeRunAsModule());
    // Ported modules append here: MakeBiosModule().
    return v;
}

}  // namespace hx
