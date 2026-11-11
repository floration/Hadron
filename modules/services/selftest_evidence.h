// modules/services/selftest_evidence.h — the selftest part for svcdrv follow-ups 6-9 (signer, event 7040, interface
// evidence, registry fallback). RunSelftest (selftest.cpp) calls it once; everything runs on the fake backend, fixture
// data and temp folders the selftest makes. Nothing here writes a real registry value or a real service.
#pragma once

#include "modules/services/kb.h"
#include <functional>
#include <string>

namespace hx::services {

using SelftestCheck = std::function<void(bool, const std::wstring&)>;
using SelftestInfo = std::function<void(const std::wstring&)>;
// newTempDir(tag): a fresh data folder the way RunSelftest makes them (Administrators-owned, so RunApply accepts it).
void RunEvidenceSelftest(const std::wstring& fixture, const Kb& kb, const std::function<std::wstring(const std::wstring&)>& newTempDir,
                         const SelftestCheck& check, const SelftestInfo& info);

}  // namespace hx::services
