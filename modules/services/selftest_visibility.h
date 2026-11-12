// modules/services/selftest_visibility.h — selftest parts for fix/svcdrv-visibility (the registry cross-check, rows the
// SCM's list missed, the TrustedInstaller read pass, --dump-list-live's text) and for the protected-items override.
// RunSelftest (selftest.cpp) calls each once. Everything runs on the fake backend, fixture data and temp folders.
#pragma once

#include "modules/services/selftest_evidence.h"

namespace hx::services {

void RunVisibilitySelftest(const std::wstring& fixture, const Kb& kb, const std::function<std::wstring(const std::wstring&)>& newTempDir,
                           const SelftestCheck& check, const SelftestInfo& info);
void RunOverrideSelftest(const std::wstring& fixture, const Kb& kb, const std::function<std::wstring(const std::wstring&)>& newTempDir,
                         const SelftestCheck& check, const SelftestInfo& info);

}  // namespace hx::services
