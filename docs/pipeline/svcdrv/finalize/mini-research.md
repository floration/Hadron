# svcdrv Finalize: mini research (Win32 docs vs the Finalize fixes)

Branch module/services-svcdrv at e3cfd92. Read-only pass, nothing built. Every row was checked against the Microsoft Learn page
it cites (fetched 2026-11-11). L = https://learn.microsoft.com/en-us/windows/win32

| Item | Verdict | file:line | URL | Note |
|---|---|---|---|---|
| R1 GetSecurityInfo by handle (owner, DACL) | ok | store.cpp:347, 377 | L/api/aclapi/nf-aclapi-getsecurityinfo | Handle opened with READ_CONTROL (required). The return is an error code, not GetLastError (handled so). sd is freed with LocalFree, and dacl is not freed separately. |
| R1 owner SID BA / SY / TI | ok | store.cpp:343-355, 367-370 | (same) | IsWellKnownSid plus the TI service SID. TI is allowed only for the holding folder (allowTi). |
| R1 DACL walk: null DACL, no DACL | ok | store.cpp:387-394 | L/api/securitybaseapi/nf-securitybaseapi-getsecuritydescriptordacl | Docs: present+NULL means a NULL DACL that "implicitly allows all access", and the app "should fail securely". The code refuses both NULL and not-present. |
| R1 inherit-only ACEs, ACE types | ok | store.cpp:399-414 | L/api/winnt/ns-winnt-ace_header | Docs: INHERIT_ONLY_ACE doesn't control access to the folder but is inherited by its children, which are Hadron's files, so counting it is correct. The 5 allowed types match the page (compound is "Reserved"). Mask follows the header in every layout. Skipping deny ACEs only makes the check stricter. A CREATOR OWNER (IO) ACE is refused, which is correct: the code notes an elevated token's default owner can be the user. |
| R1 rule 1: never re-secure an existing folder | ok | store.cpp:497-524 | - | The DACL is set only through CreateDirectoryW(kRootSddl) on a root this call created. An existing folder is only read. |
| R3 attribute bits and values | ok | backend_live.cpp:397-400; win_compat.h:24-28 | L/fileio/file-attribute-constants | OFFLINE 0x1000, RECALL_ON_OPEN 0x40000, RECALL_ON_DATA_ACCESS 0x400000: the values match. |
| R3 attributes read with GetFileAttributesW (no open, no recall) | **issue I2** | backend_live.cpp:411 | L/fileio/file-attribute-constants; L/api/fileapi/nf-fileapi-getfileattributesw | Docs: RECALL_ON_OPEN "only appears in directory enumeration classes", so a path query can miss it. On a symbolic link, GetFileAttributes "returns attributes for the symbolic link", but version.dll then follows the link to the target. |
| R3 GetFileVersionInfo(Size)ExW + FILE_VER_GET_NEUTRAL | **issue I1** | backend_live.cpp:446-449 | L/api/winver/nf-winver-getfileversioninfoexw; L/api/winver/nf-winver-getfileversioninfosizeexw | Yes, it still opens the data. NEUTRAL "loads the version resource strings from the corresponding MUI file", and LOCALISED loads the whole resource from it. CompanyName is a string, so it is read from `<dir>\<lang>\<name>.mui`, a file whose attributes are never checked. The flags match between Size and Get (required). dwHandle is ignored (ok). |
| R3 ExpandEnvironmentStringsW sizing | ok | backend_live.cpp:436-441 | L/api/processenv/nf-processenv-expandenvironmentstringsw | Docs: the return includes the null, is the required size when the buffer is too small, and is 0 on failure. need+1 and `got > size` covers it. |
| R3 GetSystemWindowsDirectoryW | ok | backend_live.cpp:419-425 | L/api/sysinfoapi/nf-sysinfoapi-getsystemwindowsdirectoryw | The return excludes the null, gives the required size if too small, and is 0 on failure. The code retries with n+1 and rejects n>=size. It is the shared (not per-user TS) directory, which is the right one. |
| R3 local-only path, DRIVE_REMOTE skip | ok | backend_live.cpp:389-411 | - | Rejects UNC, `\\?\UNC\` and device paths before any file call. |
| R4 creation time via GetProcessTimes | ok | backend_live.cpp:160-165 | L/api/processthreadsapi/nf-processthreadsapi-getprocesstimes | PROCESS_QUERY_LIMITED_INFORMATION is enough. FILETIME is 100 ns since 1601. If the open fails, pstart stays 0 and the process is never claimed (SameProcess plan.cpp:1306-1307 requires pstart != 0). |
| R4 pid from SERVICE_STATUS_PROCESS / QueryServiceStatusEx | ok | backend_live.cpp:153-159; plan.cpp:1307 | L/api/winsvc/nf-winsvc-queryservicestatusex; L/api/winsvc/ns-winsvc-service_status_process | Docs: the pid is valid only in RUNNING / PAUSE* / CONTINUE_PENDING, "may not be valid" in START/STOP_PENDING, and "never valid" when STOPPED. SameProcess and the run-time re-check (plan.cpp:1101-1102) require RUNNING. A pid recorded after an R14 failed wait (plan.cpp:977-982) may be invalid, but then it never matches, and tiBack still stops the service. |
| R4 pid reuse | ok | plan.cpp:1099-1108 | (GetProcessTimes page) | Pid + creation time identifies one process instance. If a reused pid is read between QueryServiceStatusEx and OpenProcess, it gives a different creation time, so the claim fails (the safe direction). Accepted residual: ControlService stops by service name, so a TI restart between the re-check (1101) and the stop (1109) is unguarded. No API stops a single process instance. |
| R14 started_ = own before the result | ok | ti.cpp:127-133 | L/api/winsvc/nf-winsvc-controlservice | Ownership comes from StartServiceW succeeding (ERROR_SERVICE_ALREADY_RUNNING leaves it unclaimed). |
| R22 cancellable waits | ok | ti.cpp:119-142; backend_live.cpp:171-200 | L/api/winsvc/ns-winsvc-service_status_process | The cancel check comes after the request is sent, so the stop is always delivered. Poll interval is dwWaitHint/10 clamped to 250-1000 ms (docs: dwWaitHint is the estimated pending time). A release cut short by Cancel logs `T stopfailed` although the stop was sent. The banner shows only while RUNNING, so this is harmless. |
| R5/R13 stop with running dependents | ok | plan.cpp:484-512, 1111-1121; backend_live.cpp:263-279, 352-369 | L/api/winsvc/nf-winsvc-controlservice; L/api/winsvc/nf-winsvc-controlserviceexw; L/api/winsvc/nf-winsvc-enumdependentservicesw | Docs: a stop with running dependents fails with ERROR_DEPENDENT_SERVICES_RUNNING, so the SCM never cascades. Even if Dependents() fails (note only, plan.cpp:487), a never-touch dependent cannot be stopped implicitly. EnumDependentServices with SERVICE_ACTIVE returns the reverse start order, documented as the order to stop in. The handle has SERVICE_ENUMERATE_DEPENDENTS / SERVICE_STOP as required. |
| ControlServiceExW reason | ok | backend_live.cpp:347-364 | L/api/winsvc/ns-winsvc-service_control_status_reason_paramsw | Docs require general+major+minor. PLANNED\|MAJOR_NONE\|MINOR_NONE is valid. Learn lists MINOR_NONE as 0x00060000, a doc typo (winsvc.h: 0x17). The comment at :356 says NONE is "not documented", but it is listed. The ERROR_INVALID_PARAMETER fallback is still sound. The comment string is under 128 chars. |
| R15 SERVICE_ADAPTER 0x4, SERVICE_RECOGNIZER_DRIVER 0x8 | ok | model.cpp:59-71 | L/api/winsvc/nf-winsvc-enumservicesstatusexw | Learn's current pages list neither bit (SERVICE_DRIVER = 0xB there). The values come from the SDK winnt.h (mingw winnt.h:9664-9665: 0x4 and 0x8). Treating both as drivers is the conservative side, since drivers are never controlled live. |
| Hard rule 4 | ok | module.cpp:428, 1290-1295; ti.cpp:17-44; core/sys/sys.cpp:7-14 | - | TI is off by default and never persisted. SeDebugPrivilege is enabled on the thread only (ImpersonateSelf), with the ERROR_NOT_ALL_ASSIGNED check. Elevation is detected via TokenElevation. No relaunch. |
| Hard rule 1 (these paths) | issue (I1, I2) | backend_live.cpp:404-449 | - | Data folder and SCM paths are ok. The Company lookup can still read a recall-flagged file (see I1, I2). |

## Issues

**I1, category 1 (reading cloud placeholder files), backend_live.cpp:446-449.** GetFileVersionInfoSizeExW/ExW with
FILE_VER_GET_NEUTRAL read the CompanyName string from the MUI satellite (`...\en-US\foo.dll.mui`). CompanyReadable checks
only the image path. If the MUI file is an OneDrive/ProjFS placeholder or OFFLINE, reading it recalls it.
Fix: before the call, enumerate the satellites with `GetFileMUIPath(MUI_LANGUAGE_NAME | MUI_USER_PREFERRED_UI_LANGUAGES, path, ...)`
(L/api/winnls/nf-winnls-getfilemuipath: it checks existence and never reads the .mui contents). Loop to ERROR_NO_MORE_FILES,
clearing the language buffer each call. Return "" unless every returned path passes CompanyReadable. Add a selftest that
uses a fake attribute source for the MUI path.

**I2, category 1, backend_live.cpp:397-411.** (a) RECALL_ON_OPEN is documented as reported only by directory enumeration,
so GetFileAttributesW may never show it. (b) On a symlink, GetFileAttributesW returns the link's attributes, while version.dll
follows the link to the target. Fix: read attributes with `FindFirstFileExW(path, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0)`
(no wildcard: metadata from the parent's enumeration), and use `fd.dwFileAttributes`, which is the WIN32_FIND_DATA source CLAUDE.md
rule 1 names. Add FILE_ATTRIBUTE_REPARSE_POINT to the refused set in CompanyAttrsAllowed, which covers symlinks and cloud-file
reparse tags. Service images in System32/Program Files are never reparse points, so only odd third-party images lose their Company text.
Extend the "R3: ... never opens a cloud placeholder" check with REPARSE_POINT.

Practical likelihood is low (service images live in System32 / Program Files). Both are still category 1 by the reviewer's list,
because R3 claims "never opens a cloud placeholder".

Not issues (optional): update the stale comment at backend_live.cpp:356. The R4 stop-by-name residual is noted above.
