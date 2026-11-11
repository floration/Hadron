// core/sys/win_compat.h — one place for SDK gaps and the common Windows setup.
// Include this first in every translation unit, before other Windows headers.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10
#endif

#include <windows.h>

// Cloud-file attributes are not in every SDK (CLAUDE.md "Code standards").
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif

// DWM dark-mode attribute (20 on Windows 10 20H1+). We use our own name: newer MinGW/Windows
// SDKs declare DWMWA_USE_IMMERSIVE_DARK_MODE as an enumerator of DWMWINDOWATTRIBUTE, which an
// #ifndef can't detect, so a plain #define would rewrite that enumerator to a literal and break
// <dwmapi.h>. The numeric value is stable (Win10 20H1+), so pass this to DwmSetWindowAttribute.
#define HX_DWMWA_USE_IMMERSIVE_DARK_MODE 20

// Per-Monitor-V2 DPI awareness context, for SetProcessDpiAwarenessContext. Own name for the same
// reason; the handle value ((DPI_AWARENESS_CONTEXT)-4) is fixed by the API.
#define HX_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
