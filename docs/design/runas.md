# `runas` module — design (draft, owner-requested 2026-11-11)

A launcher that starts a program (or any file, via its shell verb) as a chosen identity and integrity
level. New module, approved by the owner on 2026-11-11. Not on the old ideas list; this is the "toggle
feature" that hard rule 4 anticipated.

**Admin, not SYSTEM, is Hadron's own level.** Hadron runs elevated (rule 4). SYSTEM and TrustedInstaller
launches are gated behind an explicit, off-by-default switch in this module, and the module refuses them
cleanly when a required privilege can't be enabled. Nothing here writes firmware or moves files, so hard
rules 1 and 2 don't apply; the risk it carries is privilege, so the design is about honesty and failing safe.

## What the owner asked for
- Pick a file / shortcut / program and run it as: **this admin**, **another user**, **TrustedInstaller**,
  or **SYSTEM** — with or without **full permissions**.
- Keep a **last-run** list.
- Quick buttons for Windows tools (regedit, services.msc, etc.).

## Identity levels and how each is launched
All paths make the target an absolute path first, pass the working dir and any args, and surface the exact
Win32 error on failure (rule: check every call).

| Target | Technique | Privilege needed | Notes |
|---|---|---|---|
| **This admin (full)** | `ShellExecuteExW` / `CreateProcessW` | none beyond our elevated token | the default |
| **This admin (limited)** | run under the UAC **linked** medium-IL token: `GetTokenInformation(TokenLinkedToken)`, else `CreateRestrictedToken(LUA_TOKEN)`; lower IL with `SetTokenInformation(TokenIntegrityLevel)` | — | "without full perms" = medium/low integrity, de-elevated |
| **Another user** | `CreateProcessWithLogonW` (asks for the account + password) | the credentials | password is read into a `CredUIPromptForWindowsCredentialsW`-style field, never logged, zeroed after use |
| **SYSTEM** | enable `SeDebugPrivilege`; open a known SYSTEM process (prefer `winlogon.exe` for its session; never `lsass`), `OpenProcessToken(TOKEN_DUPLICATE|QUERY)`, `DuplicateTokenEx(…, TokenPrimary)`, set the target session via `SetTokenInformation(TokenSessionId)`, then `CreateProcessAsUserW` (or `CreateProcessWithTokenW`) | `SeDebugPrivilege`, `SeImpersonatePrivilege`, `SeAssignPrimaryTokenPrivilege` | admin has these; if any won't enable (`ERROR_NOT_ALL_ASSIGNED`), abort and say which |
| **TrustedInstaller** | `OpenSCManagerW` → open/start the **`TrustedInstaller`** service, `QueryServiceStatusEx` for its PID, duplicate its token as above, launch | same set + service control | start the service only if stopped; leave its prior state as found |

"Full vs limited permissions" for SYSTEM/TI means: run with the full token, or with a token whose groups
are filtered and IL lowered. Default is whatever is minimal for what the owner picked.

## Elevation of integrity, honestly
The UI always shows the **effective** identity and integrity of the launched process (re-query the child's
token after `CreateProcess`), not the one requested. If the OS downgraded it, say so. Never claim SYSTEM
when the child came back as High.

## UI (fits the Module contract; both Baseline and Console layouts)
- A **target** row: path field + **Browse** (UIPI blocks drops into an elevated window — rule 4), plus
  working dir and arguments.
- An **identity** selector: Admin / Another user / TrustedInstaller / SYSTEM, and a **Full / Limited** toggle.
  SYSTEM and TI are visibly behind the off-by-default switch.
- A **Run** button; the result line shows the effective identity, PID, and any error text.
- A **last-run list** (core/ui `ListView`, sortable): path, identity, integrity, when. Persisted in
  `Settings` (`%LOCALAPPDATA%\Hadron\settings.ini`), paths only, no passwords. Double-click re-runs.
- A **Windows tools** strip of buttons, each a known target launched at the chosen identity:
  Registry Editor (`regedit.exe`), Services (`services.msc`), Task Manager (`taskmgr.exe`),
  Device Manager (`devmgmt.msc`), Disk Management (`diskmgmt.msc`), Event Viewer (`eventvwr.msc`),
  Computer Management (`compmgmt.msc`), Command Prompt (`cmd.exe`), PowerShell, Registry/Group Policy as
  available. `.msc` targets launch via `mmc.exe`.

## Admin level and manifest
`Admin::Required` (rule 4). The SYSTEM/TI switch is the per-feature toggle rule 4 calls for; it does not
change the manifest.

## Testing — the honest limits
SYSTEM and TrustedInstaller launches **cannot be tested under Wine** and are not safe to exercise in CI
beyond a compile. Verification is:
- a `--selftest` that checks token plumbing on mocks (privilege enable/disable, IL parsing, argument
  quoting, path absolutization) — runs anywhere;
- the real SYSTEM/TI launches tested **on the owner's PC** against a harmless target (e.g. `cmd /c whoami
  /groups`), reading back the child's identity;
- a `windows-latest` CI job that at least launches **This admin (limited)** and reads back medium IL.

## Open choices for the owner
1. "Another user" needs a password prompt. Include it, or drop non-admin users for v1 and keep
   Admin/SYSTEM/TI only?
2. Default integrity for a SYSTEM launch: full token, or filtered?
3. Which Windows-tool buttons, and in what order? The list above is my proposal.
