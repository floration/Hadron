// modules/services/ti.h — module-local TrustedInstaller impersonation (spec §5.6, step B6).
//
// Written fresh from the technique in modules/runas/launch.cpp, impersonation only: no SeTcbPrivilege, no
// session move, no process launch. Starting and stopping the TrustedInstaller service goes through the
// Backend (Control), so backend_live.cpp stays the only file that calls SCM write/control APIs. Moving the
// token helpers into core/sys/token.h is a lead follow-up.
#pragma once

#include "modules/services/plan.h"
#include <memory>

namespace hx::services {

// The TrustedInstaller service SID (NT SERVICE\TrustedInstaller).
extern const wchar_t kTiSid[];

// Live provider: Acquire takes the token once per apply (SeDebug on the thread, SYSTEM from winlogon in this
// session, TI's process token duplicated as an impersonation token). Impersonate swaps it onto the calling
// thread and verifies SecurityImpersonation and the TI SID group before any write.
std::unique_ptr<TiProvider> MakeLiveTi(Backend* backend);

}  // namespace hx::services
