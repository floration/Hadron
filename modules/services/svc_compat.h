// modules/services/svc_compat.h — SDK constants and structs that some MinGW-w64 headers lack.
// Module-local on purpose (spec §3: no core/sys/win_compat.h edit). Every definition is guarded so a
// newer SDK that has it wins. Include after core/sys/win_compat.h.
#pragma once

#include "core/sys/win_compat.h"
#include <winsvc.h>

// Per-user service type bits (winnt.h in newer SDKs). Templates are 0x50/0x60; instances add 0x80.
#ifndef SERVICE_USER_SERVICE
#define SERVICE_USER_SERVICE 0x00000040
#endif
#ifndef SERVICE_USERSERVICE_INSTANCE
#define SERVICE_USERSERVICE_INSTANCE 0x00000080
#endif
#ifndef SERVICE_PKG_SERVICE
#define SERVICE_PKG_SERVICE 0x00000200
#endif

// Every type bit, including per-user services. MinGW's SERVICE_TYPE_ALL (0x13F) leaves out 0x40/0x80/0x200,
// so the module uses its own name (a #define of SERVICE_TYPE_ALL would rewrite the SDK's).
#define HXSVC_TYPE_ALL 0x000003FFu

#ifndef SERVICE_CONFIG_LAUNCH_PROTECTED
#define SERVICE_CONFIG_LAUNCH_PROTECTED 12
#endif
#ifndef SERVICE_CONFIG_TRIGGER_INFO
#define SERVICE_CONFIG_TRIGGER_INFO 8
#endif
#ifndef SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL
#define SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL 1
#endif

// SERVICE_LAUNCH_PROTECTED_INFO is a single DWORD; own name so it never clashes with an SDK typedef.
struct HxLaunchProtectedInfo { DWORD dwLaunchProtected; };

// SERVICE_TRIGGER_INFO / SERVICE_TRIGGER (winsvc.h, Windows 7+), missing from some MinGW-w64 headers.
// Own names with the documented layout, so they never clash with an SDK that has them.
struct HxTriggerData { DWORD dwDataType; DWORD cbData; PBYTE pData; };
struct HxTrigger {
    DWORD dwTriggerType;
    DWORD dwAction;
    GUID* pTriggerSubtype;
    DWORD cDataItems;
    HxTriggerData* pDataItems;
};
struct HxTriggerInfo { DWORD cTriggers; HxTrigger* pTriggers; PBYTE pReserved; };
#define HXSVC_TRIGGER_ACTION_STOP 2
