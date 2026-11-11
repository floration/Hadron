# svcdrv research R1: Win32/NT APIs, OS behaviour, privileges

Stage 2 (Research), 2026-11-11. Scope: the API surface for the `services` module (idea:
`docs/pipeline/svcdrv/1-idea.md`), behaviour on Win10 22H2 (19045) and Win11 21H2..24H2
(22000, 22621, 22631, 26100), and the privilege each operation needs.

**Sourcing.** No web access was used for this report. Everything below comes from memory of the Windows SDK
docs, `winsvc.h`, and how Windows behaves in practice. Lines marked **[mem]** are from memory and are
specific enough to be wrong; they need a check on a real Win10 22H2 / Win11 24H2 VM before the spec relies
on them. Lines with no tag are standard documented behaviour that I'm confident of. Lines marked
**[verify]** must be tested before a decision depends on them.

---

## 0. Headline findings (for the spec writer)

1. **Read everything through the SCM, but compare with the registry.** The SCM loads its service database
   at boot and caches it. `ChangeServiceConfigW` updates both the cache and the registry. A direct registry
   write (the only route that works for keys protected by DACL or TI) changes **only the registry**, so the SCM
   (and services.msc, and `QueryServiceConfigW`) keep reporting the old start type until reboot **[mem, high
   confidence]**. The page must read both and show "pending reboot" when they differ.
2. **Prefer `SERVICE_DISABLED` to removing triggers.** Disabled overrides trigger start and demand start, and
   undoing it is one DWORD. Removing `TriggerInfo` means serialising and restoring binary trigger data
   exactly. Presets should never strip triggers. At most, show "trigger-start" as a flag.
3. **"Admin can't change it" has four different causes, and TI fixes only one of them.**
   (a) The SCM object's security descriptor denies `SERVICE_CHANGE_CONFIG` to Administrators.
   (b) The registry key's DACL denies `KEY_SET_VALUE` to Administrators.
   (c) The service is launch-protected (PPL, `SERVICE_CONFIG_LAUNCH_PROTECTED`).
   (d) Defender Tamper Protection: a kernel registry callback in WdFilter blocks writes from any
   unprotected process, including SYSTEM and TI.
   TI impersonation (or `SeRestorePrivilege` with `REG_OPTION_BACKUP_RESTORE`) gets past (b). It sometimes
   gets past (a), when the SCM descriptor grants TI or SYSTEM. It never gets past (c) or (d), and WaaSMedic
   and Windows Update re-apply some settings later anyway. **Recommendation: leave the Defender stack
   (WinDefend, WdNisSvc, WdNisDrv, WdFilter, WdBoot, Sense, SecurityHealthService, wscsvc) out of every
   preset**, and show it as "protected by Windows; not changeable by Hadron".
4. **Detect protection per item. Don't hard-code a list.** Open the service with `SERVICE_CHANGE_CONFIG`
   (`ERROR_ACCESS_DENIED` = (a)). Open the key with `KEY_SET_VALUE` (denied = (b)). Query
   `SERVICE_CONFIG_LAUNCH_PROTECTED` (non-zero = (c)). Show the result as a "Who can change this" column:
   `Admin` / `TI` / `No (protected)`.
5. **Win11 24H2 hides kernel addresses unless the caller holds `SeDebugPrivilege`.** `EnumDeviceDrivers`
   returns zeroed bases, so `GetDeviceDriverBaseNameW` (which looks names up by base address) fails
   **[mem, high confidence; verify on 26100]**. Enable SeDebug on the thread before you enumerate loaded
   modules. Better still, take loaded/not-loaded for drivers from the SCM state (`SERVICE_RUNNING`
   = loaded) and use the module list only to find loaded images that have no service key.
6. **Offline recovery has to resolve `Select\Current`.** Never hard-code `ControlSet001`. Last Known Good has
   been off by default since Windows 8, so `ControlSet002` may be stale or missing. The recovery .bat finds
   the Windows drive (in WinRE it usually isn't C:), loads the hive, reads `Select\Current`, and writes the
   saved values with `reg add`/`reg delete`.
7. **Fixtures must come from client Windows.** GitHub `windows-latest` runs Windows Server. Its service and
   driver set is different (no Xbox services, no per-user consumer services, server roles), so it can't be
   the source for the Win10 22H2 stock table or for goldens. Use a Win10 22H2 / Win11 VM, or a **read-only**
   export from the owner's PC, with his OK.

---

## 1. Enumeration

### 1.1 Services and drivers list
```
SC_HANDLE scm = OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASEW,
                               SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, type, SERVICE_STATE_ALL,
                      buf, cb, &needed, &returned, &resume, /*group*/ nullptr);
```
- Loop while it fails with `ERROR_MORE_DATA` (234), keeping `resume`. The docs cap one call's buffer at
  256 KB, so a single "allocate `needed` and retry" isn't enough. You need the resume loop.
- Ask for `type = SERVICE_TYPE_ALL` and classify yourself. With `SERVICE_WIN32 | SERVICE_DRIVER` the per-user
  *template* entries may not appear, because their type is `0x50`/`0x60`. **[mem]**: whether templates come
  back under `SERVICE_WIN32` alone needs checking. `SERVICE_TYPE_ALL` (0x3FF in current SDKs) is the safe
  choice.
- Type bits (`winsvc.h`/`winnt.h`):
  `SERVICE_KERNEL_DRIVER 0x1`, `SERVICE_FILE_SYSTEM_DRIVER 0x2`, `SERVICE_ADAPTER 0x4`,
  `SERVICE_RECOGNIZER_DRIVER 0x8`, `SERVICE_WIN32_OWN_PROCESS 0x10`, `SERVICE_WIN32_SHARE_PROCESS 0x20`,
  `SERVICE_USER_SERVICE 0x40`, `SERVICE_USERSERVICE_INSTANCE 0x80`, `SERVICE_INTERACTIVE_PROCESS 0x100`,
  `SERVICE_PKG_SERVICE 0x200`.
- `group = nullptr` means all services. `L""` means only services in no group. Don't mix the two up.
- `ENUM_SERVICE_STATUS_PROCESSW` gives name, display name (already resolved from `@dll,-id`), and
  `SERVICE_STATUS_PROCESS` (state, controls accepted, PID, `dwServiceFlags & SERVICE_RUNS_IN_SYSTEM_PROCESS`).
- **MinGW headers** may lack `SERVICE_USER_SERVICE`, `SERVICE_USERSERVICE_INSTANCE`, `SERVICE_PKG_SERVICE`,
  `SERVICE_CONFIG_LAUNCH_PROTECTED`/`SERVICE_LAUNCH_PROTECTED_INFO` and some `SERVICE_TRIGGER_*` constants,
  depending on the mingw-w64 version. Put `#ifndef` guards in `core/sys/win_compat.h` (that is a `core/`
  change, so the lead owns it). I couldn't check, because no MinGW is installed in this container.

### 1.2 Per-service config
Open each service with only what reading needs: `SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS`, plus
`SERVICE_ENUMERATE_DEPENDENTS` and `READ_CONTROL` where needed. Those open even for WinDefend and
WaaSMedicSvc. Asking for more (or `SERVICE_ALL_ACCESS`) makes protected services fail with
`ERROR_ACCESS_DENIED` and drop out of the list.

- `QueryServiceConfigW` (call once with a null buffer, get `ERROR_INSUFFICIENT_BUFFER` (122), max 8 KB)
  returns `QUERY_SERVICE_CONFIGW`: type, start type (0 Boot, 1 System, 2 Auto, 3 Demand, 4 Disabled),
  ErrorControl, `lpBinaryPathName`, `lpLoadOrderGroup`, `dwTagId`, `lpDependencies` (MULTI_SZ; a group
  dependency has the `SC_GROUP_IDENTIFIERW` `'+'` prefix), `lpServiceStartName` (account), display name.
- `QueryServiceConfig2W` levels (handle `ERROR_INVALID_LEVEL` 124 and `ERROR_NOT_SUPPORTED` per level;
  Wine returns them often):
  | Level | Value | Struct | Notes |
  |---|---|---|---|
  | `SERVICE_CONFIG_DESCRIPTION` | 1 | `SERVICE_DESCRIPTIONW` | SCM resolves MUI `@file,-id` strings **[mem]** |
  | `SERVICE_CONFIG_FAILURE_ACTIONS` | 2 | `SERVICE_FAILURE_ACTIONSW` | reset period, reboot msg, command, `SC_ACTION[]` |
  | `SERVICE_CONFIG_DELAYED_AUTO_START_INFO` | 3 | `SERVICE_DELAYED_AUTO_START_INFO` | only means anything with Start=2 |
  | `SERVICE_CONFIG_FAILURE_ACTIONS_FLAG` | 4 | `SERVICE_FAILURE_ACTIONS_FLAG` | also fail on non-crash stop with error |
  | `SERVICE_CONFIG_SERVICE_SID_INFO` | 5 | `SERVICE_SID_INFO` | NONE / UNRESTRICTED / RESTRICTED |
  | `SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO` | 6 | `SERVICE_REQUIRED_PRIVILEGES_INFOW` | MULTI_SZ |
  | `SERVICE_CONFIG_PRESHUTDOWN_INFO` | 7 | `SERVICE_PRESHUTDOWN_INFO` | timeout ms |
  | `SERVICE_CONFIG_TRIGGER_INFO` | 8 | `SERVICE_TRIGGER_INFO` | any `cTriggers > 0` sets the "trigger start" flag |
  | `SERVICE_CONFIG_PREFERRED_NODE` | 9 | | NUMA; ignore |
  | `SERVICE_CONFIG_LAUNCH_PROTECTED` | 12 | `SERVICE_LAUNCH_PROTECTED_INFO` | 0 none, 1 Windows, 2 WindowsLight, 3 AntimalwareLight |
- Trigger-start label: Windows shows "Manual (Trigger Start)" / "Automatic (Delayed Start, Trigger Start)".
  Copy that wording.
- Trigger types for the detail pane: `DEVICE_INTERFACE_ARRIVAL 1`, `IP_ADDRESS_AVAILABILITY 2`,
  `DOMAIN_JOIN 3`, `FIREWALL_PORT_EVENT 4`, `GROUP_POLICY 5`, `NETWORK_ENDPOINT 6`, `CUSTOM 20`. Subtype is a
  GUID; data items are `SERVICE_TRIGGER_SPECIFIC_DATA_ITEM` (type BINARY/STRING/LEVEL/KEYWORD_ANY/ALL).
  The raw form is in the registry under `Services\<x>\TriggerInfo\<n>`.
- Security descriptor: `QueryServiceObjectSecurity(DACL_SECURITY_INFORMATION)` with `READ_CONTROL`. Use it to
  explain *why* admin is denied (show the ACEs as SDDL via `ConvertSecurityDescriptorToStringSecurityDescriptorW`).

### 1.3 Per-user services (Win10 1709+, all Win11)
- **Template**: `Services\<Name>`, type `0x50` (USER_OWN) or `0x60` (USER_SHARE). It never runs.
- **Instance**: `<Name>_<LUID hex>` (e.g. `CDPUserSvc_4a3b1`), type `0xD0`/`0xE0` (adds `SERVICE_USERSERVICE_INSTANCE`).
  One per interactive logon session, created at logon and deleted at logoff, so the suffix changes every
  session and reboot.
- Configuration that lasts lives on the **template**. Changing an instance's Start doesn't survive the
  next logon **[mem]**. Microsoft's "Per-user services in Windows" page describes disabling by setting the
  template's `Start=4`. It also documents `UserServiceFlags` (REG_DWORD on the template; 0 stops instances
  being created) **[mem: check the value semantics against that page before using it]**.
- The UI should collapse instances under their template ("CDPUserSvc (1 instance: _4a3b1, running)").
  Presets, snapshots and undo key on the template name only.
- `ChangeServiceConfigW` on a template **[verify]**: I'm not sure the SCM accepts it. Plan for the
  registry route (Start on the template, effective after logoff/reboot).

### 1.4 svchost grouping and image path
- `lpBinaryPathName` / `ImagePath` (REG_EXPAND_SZ) comes in several forms:
  1. `"C:\Program Files\X\svc.exe" -arg`: quoted path, then args.
  2. `C:\Program Files\X\svc.exe -arg`: unquoted with spaces. This is the "unquoted service path"
     weakness. Resolve the way CreateProcess would (try each space-delimited prefix plus `.exe`), and flag it
     in the detail pane.
  3. `%SystemRoot%\System32\svchost.exe -k netsvcs -p` (`-s <svc>` appears when split). The real code is
     `Services\<x>\Parameters\ServiceDll` (sometimes directly on `Services\<x>`), REG_EXPAND_SZ.
  4. Drivers: `\SystemRoot\System32\drivers\x.sys`, `System32\drivers\x.sys` (relative to `%SystemRoot%`),
     `\??\C:\...`, `\SystemRoot\System32\DriverStore\FileRepository\...\x.sys`, or **empty** (the default is
     `%SystemRoot%\System32\drivers\<name>.sys`).
  Map `\SystemRoot` to `GetWindowsDirectoryW()` and strip `\??\`. `ExpandEnvironmentStringsW` for `%..%`.
- Group membership: `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Svchost\<group>` (MULTI_SZ list).
  The live process for a service is `SERVICE_STATUS_PROCESS.dwProcessId`. Several services can share one
  PID.
- **svchost split (Win10 1703+)**: when installed RAM exceeds
  `HKLM\SYSTEM\CurrentControlSet\Control\SvcHostSplitThresholdInKB` (default 0x380000 = 3.5 GB), most
  share-process services run in their own svchost. `SvcHostSplitDisable=1` on a service key opts it out. This
  is why the owner's PC has 70–100 svchosts. **Process-count numbers move a lot with this setting, not with
  service count**, so the measurement section must say so. Changing the threshold is a common "tweak"
  (folklore basis). It's out of scope unless a later stage adds it.
- 64-bit only: a 32-bit build would see `System32` redirected to `SysWOW64` and get wrong paths, versions
  and signatures. Hadron's release toolchain is x64 MinGW. Keep it that way, or call
  `Wow64DisableWow64FsRedirection` around file reads.

### 1.5 Company and signer (all Windows APIs; rule 3 OK)
- Version: `GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, ...)`, `GetFileVersionInfoExW`,
  `VerQueryValueW(L"\\VarFileInfo\\Translation")`, then `\\StringFileInfo\\<lang><cp>\\CompanyName`,
  `FileDescription` and `FileVersion`. Fall back to `040904B0`.
- Authenticode: `WinVerifyTrust(INVALID_HANDLE_VALUE, &WINTRUST_ACTION_GENERIC_VERIFY_V2, &wtd)` with
  `dwUIChoice=WTD_UI_NONE`, `fdwRevocationChecks=WTD_REVOKE_NONE`,
  `dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL` (**no network**: an elevated tool shouldn't fetch CRLs), then
  `dwStateAction=WTD_STATEACTION_CLOSE`.
- **Most Windows binaries and inbox drivers are catalog-signed, not embedded-signed.** WinVerifyTrust on the
  file returns `TRUST_E_NOSIGNATURE` (0x800B0100). Do this instead: `CryptCATAdminAcquireContext2(&h, nullptr,
  BCRYPT_SHA256_ALGORITHM, nullptr, 0)`, then `CryptCATAdminCalcHashFromFileHandle2`, then
  `CryptCATAdminEnumCatalogFromHash`, then `CryptCATCatalogInfoFromContext`, then `WinVerifyTrust` with
  `WTD_CHOICE_CATALOG` (member tag = hash as uppercase hex). Retry with SHA1 if SHA256 finds nothing.
- Signer name: `WTHelperProvDataFromStateData`, `WTHelperGetProvSignerFromChain(...,0,FALSE,0)`,
  `pasCertChain[0].pCert`, `CertGetNameStringW(CERT_NAME_SIMPLE_DISPLAY_TYPE)`.
- Cost: catalog lookups take ~ms per file **[mem]**. ~800 items is seconds, so run it on a `Worker` and
  fill the column in progressively (CLAUDE.md: the UI never freezes).
- Show "Microsoft Windows" vs "Microsoft Windows Publisher" vs third party as data. Never treat a
  signature as a judgement of whether something is safe.

---

## 2. Drivers: loaded vs configured

- **Configured**: the SCM list with `SERVICE_DRIVER` types (kernel 0x1, file system 0x2). The SCM reports
  `SERVICE_RUNNING` for a loaded driver and `SERVICE_STOPPED` otherwise. That is the primary "state" column.
- **Loaded images**: `EnumDeviceDrivers` + `GetDeviceDriverFileNameW` / `GetDeviceDriverBaseNameW` (psapi,
  documented). This catches loaded kernel images with no service key (ntoskrnl, hal, kdcom/kd, CI.dll,
  PSHED, BOOTVID, clipsp, export drivers such as `msrpc.sys`, `cng.sys` loaded as imports **[mem]**).
  - **24H2**: without `SeDebugPrivilege` the addresses come back as 0, and the name lookup by address then
    fails **[mem, verify]**. Use `ThreadPrivilege(L"SeDebugPrivilege")` (already in `modules/runas/launch.cpp`)
    for the duration of the call.
  - `NtQuerySystemInformation(SystemModuleInformation=11)` returns names even with zeroed bases, but it is
    undocumented. ARCHITECTURE.md allows exactly one undocumented API (`NtEnumerateSystemEnvironmentValuesEx`),
    so **don't use it** unless the lead approves a second one.
- **Start types for drivers**: 0 Boot (winload loads it with the boot-start set; needed for the boot volume),
  1 System (IoInitSystem), 2 Auto (SCM), 3 Demand. **Most PnP function drivers are Demand and the PnP manager
  loads them when matching hardware is present**, so "Manual + not loaded" means "no device" and is normal.
  Disabling a PnP driver makes the device fail with Code 32 (start type disabled) **[mem]**.
- **ErrorControl**: 0 Ignore, 1 Normal, 2 Severe, 3 Critical. Severe/Critical used to fall back to Last
  Known Good, and Critical bugchecks if LKG fails too. LKG is off by default since Win8 **[mem]**
  (`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Configuration Manager\LastKnownGood\Enabled`).
  So a driver with ErrorControl ≥2 that fails can stop boot outright. Show it, and gate presets on it.
- **Group/Tag/load order**: `HKLM\SYSTEM\CurrentControlSet\Control\ServiceGroupOrder\List` (MULTI_SZ, group
  order), `...\Control\GroupOrderList\<group>` (REG_BINARY: count + DWORD tags, tag order inside the group),
  `dwTagId` from `QueryServiceConfigW`. Show it read-only in the detail pane. Hadron never changes Group or Tag.
- **Filter drivers (the main unbootable-PC risk besides Boot start)**:
  - Device/class filters: `HKLM\SYSTEM\CurrentControlSet\Control\Class\{GUID}\UpperFilters|LowerFilters` and
    per-devnode `Enum\...\UpperFilters|LowerFilters`. If a filter service is Disabled but still listed there,
    the whole device stack fails to start: keyboard gone (kbdclass class), disk/volume gone (`{4d36e967-...}`
    disk, `{71a27cdd-812a-11d0-bec7-00a0c91efb8b}` volume) which means `INACCESSIBLE_BOOT_DEVICE` (0x7B)
    **[mem: GUIDs from memory, check]**. **The plan must cross-check every driver it disables against all
    class and devnode filter lists, and refuse (or need a separate per-item confirm) when it's listed for a
    present device.**
  - File-system minifilters: `Services\<x>\Instances\*\Altitude`, groups `FSFilter *`. Live list:
    `FilterFindFirst`/`FilterFindNext` (`fltlib.dll`, `fltuser.h`, documented, needs admin). Show altitude
    and frame.
  - Device presence for "only where hardware is absent" (Stripped preset): `SetupDiGetClassDevsW(nullptr,
    nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT)` + `SetupDiGetDeviceRegistryPropertyW(SPDRP_SERVICE,
    SPDRP_UPPERFILTERS, SPDRP_LOWERFILTERS)`, or `CM_Get_Device_ID_ListW` + `CM_Get_DevNode_Registry_PropertyW`.
    That gives a map from service to present devnodes. "No present devnode uses this driver" is real evidence
    (a `reasoning` basis); a guess isn't.
- **Safe-mode list** (`HKLM\SYSTEM\CurrentControlSet\Control\SafeBoot\Minimal|Network\<name>`): anything
  listed there is something Microsoft considers needed for safe mode. Good extra "do not touch in Minimum"
  signal.
- **No live unload.** The idea already says this. For the record: `ControlService(SERVICE_CONTROL_STOP)` on a
  driver calls `NtUnloadDriver`. Most drivers have no `DriverUnload` and return `ERROR_INVALID_SERVICE_CONTROL`
  (1052). The ones that do can bugcheck if references remain.

---

## 3. Changing configuration

### 3.1 The SCM route (preferred)
```
SC_HANDLE s = OpenServiceW(scm, name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
ChangeServiceConfigW(s, SERVICE_NO_CHANGE, newStart, SERVICE_NO_CHANGE,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
SERVICE_DELAYED_AUTO_START_INFO d{ TRUE/FALSE };
ChangeServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &d);
```
- Takes effect immediately in the SCM. The SCM writes the registry and logs **System event 7040** ("The start
  type of the X service was changed from A to B", with the caller's user SID). Useful for drift (section 7).
- `SERVICE_BOOT_START`/`SERVICE_SYSTEM_START` are valid only for drivers: `ERROR_INVALID_PARAMETER` (87).
- Order for "Automatic (Delayed)": set Start=2 first, then delayed=TRUE. For "not delayed", write
  delayed=FALSE explicitly. The SCM ignores the delayed flag unless Start=2.
- Errors to surface: `ERROR_ACCESS_DENIED` 5, `ERROR_INVALID_PARAMETER` 87,
  `ERROR_SERVICE_MARKED_FOR_DELETE` 1072, `ERROR_INVALID_HANDLE` 6, `ERROR_DUPLICATE_SERVICE_NAME` 1078 (n/a).
- Removing triggers (not recommended, see 0.2): `SERVICE_TRIGGER_INFO t{0, nullptr, nullptr};
  ChangeServiceConfig2W(s, SERVICE_CONFIG_TRIGGER_INFO, &t)`. Restoring them needs the full serialised
  triggers (type, action, subtype GUID, and each data item's type and bytes).
- Failure actions: setting an `SC_ACTION_REBOOT` action needs `SeShutdownPrivilege` enabled, and
  `SC_ACTION_RESTART` needs `SERVICE_START` access on the handle. In `SERVICE_FAILURE_ACTIONSW`,
  `lpCommand`/`lpRebootMsg` of **nullptr means "unchanged"** and `L""` means "delete". That is an easy undo
  bug. Hadron has no reason to change failure actions. Read and show them only.

### 3.2 Direct registry route
`HKLM\SYSTEM\CurrentControlSet\Services\<x>`: `Start` (REG_DWORD), `DelayedAutostart` (REG_DWORD 0/1,
**often absent**), `TriggerInfo\` subkey.
- **The SCM doesn't notice.** `QueryServiceConfigW` returns the cached value until reboot, so StartService still
  works on a "registry-disabled" service until then **[mem, high confidence; verify]**. The UI shows
  `Registry: Disabled · SCM: Manual · applies after reboot`.
- Use this route only when the SCM route returned `ERROR_ACCESS_DENIED` and the TI toggle is on.
- **Undo must tell "value absent" from "value = 0"**. Restore by `RegDeleteValueW` when the snapshot recorded
  absence.
- Use `RegSetKeyValueW`/`RegSetValueExW` on a key opened with `KEY_SET_VALUE | KEY_QUERY_VALUE |
  KEY_WOW64_64KEY`. Then `RegFlushKey` before you record "done" in the undo log. It is expensive, but this is
  a boot-critical hive.

### 3.3 Who refuses, and why
Lists are **[mem]**. Detect at runtime (0.4), and use these only to sanity-check the detection on a VM.

| Service | (a) SCM SD denies admin | (b) Key DACL denies admin | (c) Launch-protected | (d) Tamper Protection | Practical answer |
|---|---|---|---|---|---|
| WinDefend | yes | yes | AntimalwareLight | yes | out of scope; show "managed by Windows Security" |
| WdNisSvc, WdNisDrv, WdFilter, WdBoot | yes | yes | WdNisSvc yes | yes | out of scope; WdBoot is ELAM boot-start: never touch |
| Sense (Defender for Endpoint) | yes | yes | yes | yes | out of scope |
| SecurityHealthService, wscsvc | yes | likely | ? | partly | out of scope |
| WaaSMedicSvc | yes | yes | no **[mem]** | no | TI/SeRestore registry write works and persists across reboot in my recollection; feature updates reset it |
| UsoSvc, wuauserv, BITS, DoSvc | wuauserv/UsoSvc/BITS: no; DoSvc: yes on newer builds **[mem]** | DoSvc yes | no | no | admin works for most; WaaSMedic remediates them (section 7) |
| AppXSvc, ClipSVC, sppsvc, gpsvc, Schedule, RpcSs, DcomLaunch, RpcEptMapper, BrokerInfrastructure, CoreMessagingRegistrar, StateRepository | grey in services.msc (SD denies change) **[mem]** | mostly yes | sppsvc: protected **[mem]** | no | Minimum must keep these anyway; disabling RpcSs/DcomLaunch/BrokerInfrastructure makes the PC unbootable or unusable |

Why (a)/(b) exist: since Win8.1/10, servicing (TrustedInstaller) owns the keys of components Microsoft wants
to keep consistent. Their DACL gives Administrators read only, and TI (`NT SERVICE\TrustedInstaller`,
`S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464`) or SYSTEM full control.

### 3.4 What TI impersonation actually unlocks
The idea calls for in-process **impersonation for writes only**. You don't need to move the token to a
session or create a process. Starting from the runas code:
1. Same front half as `LaunchTrustedInstaller` (`modules/runas/launch.cpp:510-623`): open the
   `TrustedInstaller` service (`SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP`), start it if stopped
   **and remember that you did**, poll to `SERVICE_RUNNING`, then `ThreadPrivilege(SeDebugPrivilege)` and
   impersonate winlogon's SYSTEM token.
2. Under SYSTEM: `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, tiPid)`, then
   `OpenProcessToken(TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE)`, then
   `DuplicateTokenEx(MAXIMUM_ALLOWED, SecurityImpersonation, TokenImpersonation)`. **No** `SeTcbPrivilege` and
   no `TokenSessionId` change: an impersonation token doesn't need a session.
3. `SetThreadToken(nullptr, tiImp)` on a dedicated worker thread (never the UI thread).
4. **Check what you got.** Without `SeImpersonatePrivilege`, `SetThreadToken` *succeeds* but the
   impersonation is silently downgraded to Identification. Registry opens then fail with
   `ERROR_BAD_IMPERSONATION_LEVEL` (1346). Admin holds SeImpersonate by default, but the owner tunes his
   policies, so check: `OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t)`, then
   `GetTokenInformation(TokenImpersonationLevel) == SecurityImpersonation` and
   `CheckTokenMembership(t, tiSid, &isMember) && isMember`. The TI process runs as **LocalSystem**; the TI
   SID is a *group* in its token, not `TokenUser`. Showing "TokenUser = SYSTEM" isn't proof of TI.
5. Do the registry writes, then `RevertToSelf()` (RAII, like `ThreadTokenSwap`).
6. If Hadron started the TrustedInstaller service, stop it with `ControlService(SERVICE_CONTROL_STOP)`. If it
   was already running (Windows Update / servicing in progress), leave it alone. **Note: `runas` currently sets
   `startedService` but, from the code I read, never stops the service.** svcdrv must, because the proxy answer
   says "restored to the state it was found". TI also stops itself after an idle timeout **[mem]**, so the PID
   can go away between calls. The duplicated token stays valid after the process exits, but re-query the PID
   each time you need it.
7. Privileges needed: `SeDebugPrivilege` (open winlogon's token via SeDebug; runas comments that the TI
   token DACL admits SYSTEM and TI, not Administrators), `SeImpersonatePrivilege` (impersonation level), and
   service START/STOP on TrustedInstaller (admin has that). Per rule 4, each counts as enabled only if
   `AdjustTokenPrivileges` succeeded **and** `GetLastError() != ERROR_NOT_ALL_ASSIGNED`. Use the existing
   `PrivilegeEnabled` / `ThreadPrivilege` helpers.

**What it unlocks:**
- Registry keys whose DACL grants TI (cause (b)): yes.
- SCM changes where the service SD grants SYSTEM or TI change-config (cause (a)): **probably yes**, because the
  SCM RPC call (ncalrpc) carries the caller's thread impersonation token **[mem, verify]**. Try the SCM call
  under TI first. It has immediate effect and logs 7040. Fall back to the registry only if the SCM still says
  `ERROR_ACCESS_DENIED`.

**What it cannot unlock:**
- **Launch-protected (PPL) services (c)**: the SCM checks the caller's *process* protection level, and
  impersonation doesn't change that. Config change and stop are refused **[mem: exact operations gated by the
  SCM need a VM check]**.
- **Tamper Protection (d)**: WdFilter's registry callback blocks writes to Defender keys from unprotected
  processes regardless of token. With TP off, Defender Group Policy and the `Windows Defender` policy keys
  work, but that's a different feature.
- **WaaSMedic remediation / Windows Update re-applying settings**: the write succeeds and Windows may undo it
  later (section 7).
- **Feature updates** (in-place upgrade, e.g. 22H2 to 23H2 or 24H2) rebuild most of the service config.

### 3.5 Alternative to TI: `SeRestorePrivilege` and `REG_OPTION_BACKUP_RESTORE`
`RegCreateKeyExW(HKLM, path, 0, nullptr, REG_OPTION_BACKUP_RESTORE, 0 /*ignored*/, nullptr, &k, nullptr)`
with `SeRestorePrivilege` enabled on the thread opens the key for write **regardless of its DACL**, without
taking ownership or changing the DACL. It's admin-level (no TI service, no SeDebug), simpler and more
robust. **Recommendation:** use it as the mechanism for "protected writes" behind the *same* off-by-default
toggle. Bypassing a DACL is the same power whichever way you do it, and rule 4's toggle is about intent. Label
the toggle honestly ("Write protected service settings (bypasses Windows' permissions)"). Keep the TI path
for the SCM-call case in 3.4, if the VM check shows it helps.
**Never** `SeTakeOwnershipPrivilege` + `SetNamedSecurityInfoW` to rewrite owners or DACLs. That permanently
changes Windows' security state, an undo would have to restore the SD exactly, and servicing can then fail.

---

## 4. Start / stop / restart (services only)

- Open with `SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS`.
- **Stop**: `ControlServiceExW(s, SERVICE_CONTROL_STOP, SERVICE_CONTROL_STATUS_REASON_INFO, &reason)` with
  `SERVICE_CONTROL_STATUS_REASON_PARAMSW{ dwReason = SERVICE_STOP_REASON_FLAG_PLANNED |
  SERVICE_STOP_REASON_MAJOR_NONE | SERVICE_STOP_REASON_MINOR_NONE, pszComment = L"Hadron" }`. The reason is
  logged, and the call returns the post-control status in `ServiceStatus`.
  - Check `dwControlsAccepted & SERVICE_ACCEPT_STOP` first. If it's missing, grey out Stop
    (RpcSs, DcomLaunch, RpcEptMapper, LSM, BrokerInfrastructure, Power, PlugPlay... **[mem]**).
  - Dependents: `EnumDependentServicesW(s, SERVICE_ACTIVE, ...)`. The docs say the array comes back in reverse
    start order, so stop dependents in array order, then the target. Show the whole list in the plan.
    Without this you get `ERROR_DEPENDENT_SERVICES_RUNNING` (1051).
  - Wait: poll `QueryServiceStatusEx` using the docs' sample pattern. Sleep `dwWaitHint/10` clamped to
    1–10 s. Give up when `dwCheckPoint` hasn't advanced within `dwWaitHint`. Use an overall cap (30 s) on a
    `Worker` and post progress.
  - Errors: `ERROR_SERVICE_NOT_ACTIVE` 1062 (already stopped: success), `ERROR_INVALID_SERVICE_CONTROL` 1052,
    `ERROR_SERVICE_CANNOT_ACCEPT_CTRL` 1061 (start/stop pending; retry later), `ERROR_DEPENDENT_SERVICES_RUNNING`
    1051, `ERROR_ACCESS_DENIED` 5 (protected), `ERROR_SHUTDOWN_IN_PROGRESS` 1115.
- **Start**: `StartServiceW(s, 0, nullptr)`. `ERROR_SERVICE_ALREADY_RUNNING` 1056 counts as OK.
  `ERROR_SERVICE_DISABLED` 1058, `ERROR_SERVICE_DEPENDENCY_FAIL` 1068 / `ERROR_SERVICE_DEPENDENCY_DELETED`
  1075, `ERROR_SERVICE_REQUEST_TIMEOUT` 1053 (the SCM waited `ServicesPipeTimeout`, default 30 s),
  `ERROR_SERVICE_LOGON_FAILED` 1069, `ERROR_PATH_NOT_FOUND` 3. After it returns, poll until RUNNING as for stop.
- **Restart** = stop (with dependents), then start the target, then start the dependents that were running
  before. services.msc does this; people expect it.
- **Shared svchost**: stopping one service doesn't end the process if other services live in it. The PID
  column shows that.
- **Per-user instances**: stop/start work on the instance (`CDPUserSvc_4a3b1`). The template can't be started.
- **Critical services** (failure action = reboot, e.g. on some core services **[mem]**): a *requested* stop
  isn't a failure, so failure actions don't fire. But stopping some core services (e.g. by killing the
  process) will log the user off or bugcheck. Hadron only uses `ControlService` and never terminates
  processes.
- `ERROR_SERVICE_MARKED_FOR_DELETE` 1072: a service whose handles are still open after `DeleteService` (third
  party uninstallers). Show it as state "pending deletion".

---

## 5. Privileges and rule 4 summary

| Operation | Needs | Under admin (Hadron's level) |
|---|---|---|
| Enumerate + query config/status | `SC_MANAGER_ENUMERATE_SERVICE`, per-service `SERVICE_QUERY_*` | yes; works for protected services too if you ask only for query rights |
| Loaded driver images (24H2) | `SeDebugPrivilege` on the thread | yes (admin holds SeDebug, disabled by default) |
| Minifilter list | admin | yes |
| Signer/version | read access to files | yes |
| Change Start/Delayed via SCM | `SERVICE_CHANGE_CONFIG` granted by the service SD | most services; not (a)-class |
| Start/stop | `SERVICE_START`/`SERVICE_STOP` | most; not PPL/(a)-class |
| Protected registry write, route A | `SeRestorePrivilege` + `REG_OPTION_BACKUP_RESTORE` | admin privilege, **behind the toggle** |
| Protected write, route B (TI) | `SeDebugPrivilege` + `SeImpersonatePrivilege` + TI service start/stop | admin privileges, **behind the toggle** |
| Event log 7040 query | `EvtQuery` on `System` | yes |

Rule 4 points: detect elevation with `GetTokenInformation(TokenElevation)`. If the process isn't elevated
(Wine, manifest ignored), the page still lists (read-only works for many items) but shows "not elevated:
changes disabled" and never relaunches. Every privilege enable goes on the **thread** token
(`ImpersonateSelf` + adjust), as runas does, so the process token is never left with SeDebug or SeRestore enabled.

---

## 6. Snapshot, undo and offline recovery

### 6.1 What describes a service's start behaviour
For Hadron's purposes, the "start state" is the tuple:
`Start`, `DelayedAutostart` (absent | 0 | 1), trigger set (present/count + serialised bytes, only if Hadron
ever touches triggers), and for per-user templates `UserServiceFlags` (absent | value) if Hadron uses it.
Plus, for information and diagnosis, not for restore: Type, ErrorControl, ImagePath, ObjectName, Group, Tag,
DependOnService/DependOnGroup, FailureActions, RequiredPrivileges, ServiceSidType, LaunchProtected, Security SD
(as SDDL).

**Restore exactly = write back only the fields Hadron changed, from the values captured before the change,
deleting values that were absent.** Don't restore the whole key (`RegSaveKeyExW`/`RegRestoreKeyW` need
SeBackup/SeRestore and would clobber unrelated changes Windows made since).

Record **both** views per item at snapshot time: SCM (`QueryServiceConfigW`) and registry (raw values from
`CurrentControlSet`). They can already differ (a pending reboot from some other tool), and the undo should
put back the *registry* values while noting the SCM view.

Record header: Hadron version, date, `CurrentBuildNumber.UBR` + `DisplayVersion` (registry
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion`), `Select\Current` number, computer name hash (not the
name, which is personal data), and the route used per item (SCM / registry-SeRestore / registry-TI).

Write order per item: append the undo entry, then `FlushFileBuffers`, then make the change, then append
"done" and flush. On startup, an entry without "done" means it was interrupted: offer Undo or Resume.
Same contract as `core/fs`, but this isn't a file op, so it's a module-local log. Format: plain text, written
and parsed by Hadron (rule 3), under `%LOCALAPPDATA%\Hadron\services\`.

### 6.2 First-run snapshot ("Restore snapshot" preset)
Captured the first time the page loads, before any change. Same per-item tuple, for **every** service and
driver (≈ 250–300 services + 400–600 drivers on Win10/11 **[mem]**). Immutable once written. Later snapshots
are separate files.

### 6.3 Offline recovery (.zip per rule 6)
Contents: `restore.bat`, `values.txt` (or the values inlined in the .bat), `README.txt`. Possibly a
`.reg`, but `.reg` files can't resolve `Select\Current` or the Windows drive letter. **Make the .bat the
primary**, and offer the `.reg` only for the online case (Windows boots to Safe Mode: just run Hadron's Undo
there instead; the SCM works in Safe Mode).

WinRE flow the .bat must handle (WinRE: Troubleshoot > Advanced > Command Prompt; you get X:\ as system,
`reg.exe` is available):
```bat
@echo off
setlocal EnableDelayedExpansion
rem 1. find the Windows install: WinRE often assigns D: or E: to it
for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
  if exist %%D:\Windows\System32\config\SYSTEM if not "%%D"=="X" set "WIN=%%D:" & echo Found Windows on %%D:
)
rem (if more than one found, list and ask; never guess silently)
reg load HKLM\HX_SYS "!WIN!\Windows\System32\config\SYSTEM" || goto :fail
for /f "tokens=3" %%V in ('reg query HKLM\HX_SYS\Select /v Current') do set /a CS=%%V
set "CSN=00!CS!" & set "CSN=ControlSet!CSN:~-3!"
rem 2. per item, from the snapshot:
reg add "HKLM\HX_SYS\!CSN!\Services\DiagTrack" /v Start /t REG_DWORD /d 2 /f
reg delete "HKLM\HX_SYS\!CSN!\Services\DiagTrack" /v DelayedAutostart /f
...
reg unload HKLM\HX_SYS
```
Pitfalls:
- `reg query` prints `0x1`; `set /a` parses hex. Pad to three digits: `ControlSet001`.
- Under WinRE, BitLocker volumes are locked. The README must say "unlock with `manage-bde -unlock C: -rp
  <recovery key>` first" (or WinRE prompts for it). Without that the drive letter loop finds nothing.
- `reg load` fails if the hive is in use (it never is in WinRE) or if `config\SYSTEM` needs log replay. reg
  load replays the transaction logs **[mem]**.
- Write the values as **absolute** targets, not deltas, so running it twice is safe.
- Also write a `restore-online.bat` (same values, `HKLM\SYSTEM\CurrentControlSet`, plus `sc config` where
  allowed) for Safe Mode with Command Prompt. It checks admin with `fltmc >nul 2>&1 || (echo Run as
  administrator & exit /b 1)` (CLAUDE.md rule 4: not `net session`).
- Keep a copy outside `%LOCALAPPDATA%`? From WinRE the user can browse to
  `C:\Users\<name>\AppData\Local\Hadron\...`, but it's hidden and hard to find. Offer "Save recovery zip
  to..." through a Save dialog (ARCHITECTURE: exports via Save dialog), default Desktop or Downloads via
  `SHGetKnownFolderPath(FOLDERID_Downloads)` (rule 5: never hard-code). Recommend the owner copies it to a USB
  stick. Say that in the UI.
- Zip: `core/sys` zip writer with the UTF-8 flag (bit 11). Use CRLF line endings in .bat/.txt.

---

## 7. Drift: how Windows reverts changes

Sources **[mem; each needs a VM check before the KB claims it]**:
1. **WaaSMedicSvc** (Windows Update Medic Service, Win10 1803+): its plugins (`WaaSMedicAgent.exe` + plugin
   DLLs) check and repair the Windows Update stack. They re-enable `wuauserv`, `UsoSvc`, `BITS`, `DoSvc`
   **[mem: exact plugin list varies by build]**, and repair the related scheduled tasks. It runs on demand,
   triggered by the update stack and UsoSvc tasks.
2. **UsoSvc / Update Orchestrator scheduled tasks** (`\Microsoft\Windows\UpdateOrchestrator\*`) and
   `\Microsoft\Windows\WaaSMedic\PerformRemediation`: they wake the update stack, which then repairs.
3. **Cumulative updates** sometimes reset start types of components they service (DiagTrack and
   dmwappushservice are common reports, folklore-grade). **Feature updates** (in-place upgrade) rebuild the
   OS and reset most service config to stock.
4. **Group Policy / security templates** (`secedit` "System Services" policy) re-apply every 90 min ±30 for
   GPO refresh, or 16 h for the security engine **[mem]**. Unlikely on the owner's standalone PC, but detect:
   `HKLM\SOFTWARE\Policies\...` and the per-service SD set by policy.
5. **OEM/third-party updaters** (MSI Center, game launchers, anti-cheat) re-enable or recreate their own services.
6. **Per-user instances** are recreated at every logon from the template, so instance changes always
   "drift".

Detection Hadron can do with documented APIs:
- Keep an **applied-state file** (what Hadron last set, per item). On each load, compare live state with it
  and show "Changed since Hadron applied it: was Disabled, now Manual".
- Attribution: query System event **7040** with `EvtQuery(nullptr, L"System",
  L"*[System[Provider[@Name='Service Control Manager'] and (EventID=7040)]]", EvtQueryReverseDirection)`.
  Each has the service name, old/new type and the `UserID` SID (S-1-5-18 = SYSTEM: Windows itself, WaaSMedic,
  an updater). Direct registry writes **don't** log 7040, so "no event" doesn't mean "no change".
- A new `CurrentBuildNumber`/`UBR` since the last apply is a strong hint: "Windows updated on <date>; 14 items
  drifted."
- Don't fight drift automatically (no background re-apply). Show it, and offer "re-apply" as a normal
  plan + confirm.

---

## 8. Measured before/after (proxy answer 8: measured, never predicted)

APIs (documented):
- `GetPerformanceInfo(&pi, sizeof pi)` (psapi): `PhysicalTotal`, `PhysicalAvailable`, `CommitTotal`,
  `CommitLimit`, `SystemCache`, `KernelPaged`, `KernelNonpaged`, `ProcessCount`, `ThreadCount`,
  `HandleCount`, all in pages (× `PageSize`).
- `GlobalMemoryStatusEx`: `ullTotalPhys`, `ullAvailPhys`, `dwMemoryLoad`. "In use" as Task Manager shows it is
  roughly `Total − Available` (Available = free + zeroed + standby). Task Manager's own figure also excludes
  hardware-reserved memory and counts compressed pages specially **[mem]**. Label it as Hadron's
  definition, not "Task Manager's number".
- Running service count from the enumeration (SERVICE_RUNNING, Win32 types, instances counted once per
  instance). svchost process count via `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` matching `svchost.exe`,
  or `EnumProcesses` + `QueryFullProcessImageNameW` (PROCESS_QUERY_LIMITED_INFORMATION works on PPL processes).
- Per-service process memory (optional): `GetProcessMemoryInfo` with `PROCESS_MEMORY_COUNTERS_EX`
  (`PrivateUsage`, `WorkingSetSize`). It needs `PROCESS_QUERY_LIMITED_INFORMATION` (+VM_READ on old OSes
  **[mem]**). Protected processes may refuse; show "n/a".

Honesty pitfalls (they belong in the UI next to the numbers):
- **Live "before vs right after stop" isn't the same as "after reboot".** Stopping a service frees its
  private memory, but the standby cache, SysMain prefetch, memory compression and boot-time trimming make RAM
  in use drift for minutes. Comparable numbers need: a reboot, the same idle time (e.g. 5 min after logon, no
  apps), several samples (median of N over 60 s). Offer a guided "measure after next reboot" flow: record
  the conditions, ask the user to reboot, re-measure on first open after N minutes of uptime
  (`GetTickCount64`).
- Process count depends on the svchost split (1.4). A preset that disables 20 services can cut 20 processes
  on a 32 GB PC and fewer on a 4 GB one. Show the counts and the split threshold as context.
- Rule 7: the numbers are `owner-measured` only when the owner runs them on his PC. Hadron shows "measured on
  this PC at <time>, conditions: ..." and never extrapolates.

---

## 9. Wine (screenshots, fixtures)

**[mem]**: based on Wine 8/9 behaviour, needs a check against the CI Wine version.
- Wine has its own `services.exe` with a small set (e.g. `RpcSs`, `PlugPlay`, `MountMgr`, `Spooler`,
  `StiSvc`, `winebus`, `wineusb`, `nsiproxy`, `TermService`-ish). `EnumServicesStatusExW` and
  `QueryServiceConfigW` work. `QueryServiceConfig2W` supports description, failure actions, preshutdown and
  maybe delayed-auto. Others return `ERROR_INVALID_LEVEL`. No triggers, no launch-protected, no per-user
  services.
- `ChangeServiceConfigW` works (Wine's registry). `StartServiceW` works for Wine's builtins.
- No TrustedInstaller service, no winlogon SYSTEM token, so TI fails. Wine's `ImpersonateSelf`/`SetThreadToken`
  are partial. `REG_OPTION_BACKUP_RESTORE` probably ignored.
- `EnumDeviceDrivers` returns a stub or tiny list. No WdFilter, no `fltlib` minifilters. `WinVerifyTrust`
  exists, but there are no catalogs, so no signer.
- `GetPerformanceInfo` and `GlobalMemoryStatusEx` work (host numbers).
- Elevation: Wine reports elevated or not depending on prefix and version. The page must handle both
  (rule 4: say so, degrade).

**So:** screenshots and goldens use `LoadFixture(dir)` with a captured dataset (services.txt + drivers.txt +
config2 dump + signer cache + perf sample) taken on a real **client** Win10 22H2 (and Win11 24H2) VM, through
Hadron's own `RunCli` export mode (`HadronServices.exe --export-fixture <dir>`, read-only). Live SCM writes,
the TI path, the SeRestore route, the 24H2 SeDebug behaviour, and the protected-service matrix in 3.3 need a
real Windows VM. **Never the owner's PC for writes** (idea, rule 5). A read-only `--export-fixture` on his PC
is fine with his OK, and is the best source for the Z790/MSI driver set (MSI Center services and so on).
A `windows-latest` CI job can still exercise SCM read paths, start/stop of a harmless service it installs
itself (`CreateServiceW` of a dummy own-process service, then delete it), and the snapshot/undo round-trip
on that dummy. That is a real integration test that doesn't touch Windows' own services.

---

## 10. Build differences to key the stock table on

- Identify: `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` → `CurrentBuildNumber`, `UBR`,
  `DisplayVersion` (22H2/23H2/24H2), `EditionID` (Professional/Core/Enterprise; the service set differs by
  edition, e.g. Enterprise has extra services). `RtlGetVersion` also works. `GetVersionExW` is fine because the
  manifest declares Win10 `supportedOS`, but it gives no UBR.
- 19045 (Win10 22H2), 22000, 22621/22631 (same base; 23H2 = enablement package, so the service set is nearly
  identical **[mem]**), 26100 (24H2: new base, many service changes, e.g. AI/Recall-related services on
  Copilot+ PCs, SeDebug for kernel addresses).
- Unknown build: no explanations, not in any preset (as the idea says). Unknown edition on a known build:
  explanations OK, presets apply only to items present in the table **and** on the machine.

---

## 11. Suggested build plan (API-level)

1. `svc_enum.{h,cpp}`: SCM enumeration plus config2 levels plus registry raw view plus per-user collapsing.
   Pure functions over a `ServiceRecord` struct, so fixtures and live data share one path.
2. `drv_state.{h,cpp}`: SCM driver state, `EnumDeviceDrivers` (thread SeDebug), fltlib minifilters,
   class/devnode filter map, SetupAPI devnode-to-service map, SafeBoot membership.
3. `sign.{h,cpp}`: version resources plus WinVerifyTrust (embedded, then catalog), cached by path+size+mtime.
4. `protect.{h,cpp}`: per-item "who can change" probe (SCM open CHANGE_CONFIG, registry KEY_SET_VALUE,
   LaunchProtected). Read-only: opening with write access changes nothing, but close immediately.
5. `apply.{h,cpp}`: plan builder (dependency check, filter cross-check, Boot/System gating, ErrorControl
   gating), undo log (flush), SCM route, protected route (SeRestore or TI on a dedicated worker thread),
   restore. **This is the `module-builder-careful` part, and the lead reviews it personally.**
6. `recovery.{h,cpp}`: .bat/.txt generator plus zip.
7. `measure.{h,cpp}`: GetPerformanceInfo / GlobalMemoryStatusEx / process counts, sampling on a Worker.
8. `drift.{h,cpp}`: applied-state comparison plus EvtQuery 7040.
9. If the TI token code moves from `modules/runas` to `core/sys` (e.g. `sys::TrustedInstallerImpersonation`
   RAII), that is a `core/` API change: the lead owns it. Otherwise copy the needed part into the module and
   note it.

## Open questions for VM verification (before Build)
1. Does `EnumServicesStatusExW(SERVICE_WIN32)` return per-user templates, or only with `SERVICE_TYPE_ALL`?
2. Does `ChangeServiceConfigW` on a per-user template succeed, and does `UserServiceFlags` behave as the MS doc says?
3. Is the SCM's cached start type stale after a direct registry write until reboot (expected yes)?
4. Does an SCM call from a TI-impersonating thread succeed on WaaSMedicSvc / DoSvc / AppXSvc?
5. Exact set of services whose SCM SD denies admin `SERVICE_CHANGE_CONFIG` on 19045 and 26100.
6. `EnumDeviceDrivers` on 26100 with and without SeDebug.
7. Does WaaSMedic revert `wuauserv`/`UsoSvc` Disabled within a day on 22H2, and does it touch anything else?
8. `REG_OPTION_BACKUP_RESTORE` write to `Services\WaaSMedicSvc` with SeRestore only: works? Persists across reboot?
