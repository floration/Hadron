// core/sys/log.h — session log + crash handler. See docs/ARCHITECTURE.md "Settings, logs, crashes".
#pragma once

#include "core/sys/win_compat.h"
#include "core/host/module.h"   // LogLevel
#include <string>

namespace hx::sys {

// Opens %LOCALAPPDATA%\Hadron\logs\<stamp>.log for the session. Safe to call once at startup.
void OpenLog();
void Write(hx::LogLevel level, const std::wstring& msg);
void CloseLog();

// SetUnhandledExceptionFilter + MiniDumpWriteDump to %LOCALAPPDATA%\Hadron\crash\.
void InstallCrashHandler();

}  // namespace hx::sys
