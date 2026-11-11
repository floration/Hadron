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

// The TrustedInstaller *service* half of the live provider, through the Backend only (testable on the fake).
// Ensure starts the service only when it is STOPPED (STOP_PENDING is waited out first), and claims it ("Hadron
// started it") only when Control reports that its own request started it (not ERROR_SERVICE_ALREADY_RUNNING);
// START_PENDING is someone else's start: waited for, never claimed. StopIfStarted stops it only when
// claimed. Both drop the claim, so one apply's start is never carried into the next apply.
class TiService {
public:
    explicit TiService(Backend* b) : b_(b) {}
    bool Ensure(SvcItem* ti, std::wstring* err);   // true when TrustedInstaller is RUNNING
    bool Started() const { return started_; }
    bool StopIfStarted(std::wstring* note);        // true only when it was claimed and the stop succeeded
    DWORD waitMs = 30000, pollMs = 250;            // the START_PENDING wait
    // Checked by every wait in Ensure and passed to Control (R22): Cancel and module Shutdown never wait out a stuck
    // TrustedInstaller start. A stop request is still sent when cancelled; only the wait for STOPPED is cut short.
    const std::atomic<bool>* cancel = nullptr;
private:
    Backend* b_;
    bool started_ = false;
};

// Live provider: Acquire takes the token once per apply (SeDebug on the thread, SYSTEM from winlogon in this
// session, TI's process token duplicated as an impersonation token). Impersonate swaps it onto the calling
// thread and verifies SecurityImpersonation and the TI SID group before any write.
std::unique_ptr<TiProvider> MakeLiveTi(Backend* backend);

}  // namespace hx::services
