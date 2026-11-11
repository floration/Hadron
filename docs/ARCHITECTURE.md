# Hadron architecture

## Layout
```
hadron/
  CMakeLists.txt           project(Hadron VERSION x.y.z); always builds every target (suite + each standalone + tests)
  core/
    host/                  IHost, Module, registry (explicit factories), RunStandalone, RunSuite, command line
    ui/                    theme, per-DPI fonts, double buffering, widgets (chips, buttons, list view with sortable
                           columns + draggable scrollbar + keyboard nav, detail pane, search box, progress), Confirm()
    sys/                   Worker, elevation/relaunch, privileges, long paths, settings, logging, crash dumps,
                           zip writer (UTF-8 flag), firmware.h (IFirmwareVars), usn.h (IUsnSource), win_compat.h
    fs/                    plan -> confirm -> execute -> undo log, trash, folder lock, reparse/cloud/hard-link checks
  modules/<id>/
    module.cpp             implements hx::Module
    standalone.cpp         int wWinMain(...) { return hx::RunStandalone(hx::MakeXxxModule()); }
    resources/<id>.rc      resource fragment; every resource name is prefixed with the module id (BIOS_SETTINGS)
    tests/                 console test exe (hadron_<id>_tests.exe), fixtures, golden comparisons
  app/hadron.cpp           suite WinMain
  tools/                   offline generators (BIOS pipeline, Python)
  tests/fixtures, tests/golden
  legacy/                  original single-file tools, kept as the reference while porting
```

## The module contract
```cpp
namespace hx {
enum class Admin { None, Optional, Required };   // Optional: degraded mode without admin (e.g. BIOS loads a snapshot)

struct ModuleInfo {
    const wchar_t* id;       // "bios": used by --solo/--module and in exe/class/resource names
    const wchar_t* name;     // "BIOS Settings Explorer"
    const wchar_t* summary;
    Admin admin;
};

struct IHost {                                   // implemented by both RunStandalone and RunSuite
    virtual HWND Frame() = 0;                    // owner for dialogs; never create other top-level windows
    virtual UINT Dpi() = 0;
    virtual void SetTitle(const wchar_t*) = 0;   // replaces legacy SetWindowTextW(g_hwnd, ...)
    virtual void SetStatus(const wchar_t*) = 0;
    virtual void SetBusy(bool) = 0;              // the host blocks Close and shows activity
    virtual bool IsElevated() = 0;
    virtual bool RequestElevation() = 0;         // relaunch elevated with --elevated; false if cancelled or failed
    virtual Settings& Settings() = 0;            // per-module section
    virtual void Log(LogLevel, const wchar_t*) = 0;
};

class Module {
public:
    virtual ~Module() = default;                       // constructors do no I/O
    virtual const ModuleInfo& Info() const = 0;
    virtual HWND Create(IHost&, HWND parent, const RECT&) = 0;  // called lazily on first activation
    virtual void OnActivate() {}
    virtual void OnDeactivate() {}                     // workers keep running while hidden
    virtual void OnDpiChanged(UINT dpi) {}             // before the host resizes; rebuild fonts/metrics
    virtual bool PreTranslateMessage(MSG&) { return false; }   // active module only (accelerators)
    virtual bool CanClose() { return true; }           // asked of every module; false while a plan executes
    virtual void Shutdown() {}                         // cancel; must not return while any worker is alive
    virtual int  RunCli(int argc, wchar_t** argv) { return -1; }   // headless: --selftest, --screenshot, exports
};
std::unique_ptr<Module> MakeBiosModule();   // explicit factories in core/host/registry.h; no static self-registration
int RunStandalone(std::unique_ptr<Module>);
int RunSuite(std::vector<std::unique_ptr<Module>>);
}
```

### Host and module rules
- **Windows.** A module draws only inside the HWND the host gives it and never calls `PostQuitMessage`. Its window
  classes are named `Hadron.<id>.<name>`.
- **Messages.** `WM_APP+0..0xFF` belong to the host; modules start at `WM_APP+0x100`. Workers post completion
  to the module HWND, and the host keeps that HWND alive until `Shutdown()` returns.
- **COM.** The host initialises COM (STA, `COINIT_DISABLE_OLE1DDE`). Modules never call `CoInitialize*` on the UI thread.
- **Focus and input.** The host saves `GetFocus()` on deactivate and restores it on activate, and forwards
  `WM_MOUSEWHEEL` to the active module. It owns Ctrl+Tab and Ctrl+1..9.
- **Dialogs.** Every dialog is owned by `IHost::Frame()` and disables the whole suite. Use `hx::ui::Confirm()`.
- **DPI.** The host handles `WM_DPICHANGED` using the suggested rect, then calls `OnDpiChanged`. Every metric
  goes through `S(px)` with the current DPI. All legacy tools are system-DPI only, so this is real port work.
- **Worker.** `core/sys` `Worker`: an atomic progress counter, read by the module on a timer, plus cancel and join.
  Its destructor calls `Cancel()` then `Join()` with no timeout. This is the legacy pattern made safe.
- **Drag and drop.** UIPI blocks drag and drop from Explorer into elevated windows, so every drop target also has a Browse button.

## Command line (same for standalone and suite)
| Argument | Meaning |
|---|---|
| `--module <id>` | Suite: open on that module |
| `--solo <id>` | Suite: run only that module, as if standalone |
| `--elevated` | Marks a relaunch; never relaunch again |
| `--selftest` | Run the module's checks |
| `--screenshot <png> --fixture <dir>` | Load the fixture, render the main view with `PrintWindow`, save, exit. This makes CI and tester screenshots deterministic on Windows and Wine |
| other | Passed to `RunCli`. GUI-subsystem exes call `AttachConsole(ATTACH_PARENT_PROCESS)` before writing output |

## Firmware access (`core/sys/firmware.h`)
- Call `GetFirmwareType()` first. Read variables with `GetFirmwareEnvironmentVariableExW`, and grow the buffer on
  `ERROR_INSUFFICIENT_BUFFER`. The legacy app used a fixed 64 KB buffer.
- Report three distinct states: 203 = the variable doesn't exist (map mismatch), 1314 = privilege missing, and access denied = protected.
- Enumerate with `NtEnumerateSystemEnvironmentValuesEx` (class 1, grow on `STATUS_BUFFER_TOO_SMALL`), loaded with
  `GetProcAddress`. This is the one allowed undocumented API.
- **Version guard.** Read SMBIOS types 0 and 2 through `GetSystemFirmwareTable('RSMB')`. The settings map stores
  board, BIOS version and the expected varstore sizes. On a mismatch, show a banner and mark values "unverified".
- `IFirmwareVars` has a live implementation and a snapshot implementation. Snapshot format: `index.csv` + `vars/`,
  as in legacy, with these fixes:
  - file names get an index number, so long names no longer collide;
  - the CSV parser handles `""` escapes;
  - zips set UTF-8 flag bit 11.

## USN journal (`core/sys/usn.h`)
`IUsnSource` has a live implementation (volume handle, needs admin) and a fixture implementation (a recorded record buffer).
The analysis code stays platform-independent, like legacy `rescue_core.h`. NTFS only; ReFS is not supported.
128-bit file IDs are truncated to 64 bits, which is valid on NTFS.

## Build outputs and resources
| Target | File | Manifest execution level |
|---|---|---|
| Suite | `Hadron.exe` | requireAdministrator |
| Module with `Admin::None` or `Admin::Optional` | `Hadron<Name>.exe` | asInvoker |
| Module with `Admin::Required` (rescue) | `Hadron<Name>.exe` | requireAdministrator |

- Each exe's `.rc` and manifest are generated with `configure_file`. They contain:
  - VERSIONINFO from `project(VERSION)`;
  - the execution level;
  - `PerMonitorV2` + `dpiAware true/pm`;
  - `longPathAware`;
  - Windows 10 `supportedOS`;
  - Common Controls v6.
- Only the exe `.rc` owns `ICON 1` and `RT_MANIFEST`. Module resource fragments use prefixed names only.
- Release zip `Hadron-<version>.zip`: the suite, every standalone exe and README.txt.
  - The exes are unsigned. Explorer passes the zip's Mark-of-the-Web on, so SmartScreen will prompt. The README
    says: right-click the zip > Properties > Unblock before extracting.
  - Antivirus heuristics may flag static MinGW binaries that read firmware or volume handles. Mitigation: full
    VERSIONINFO, no packers, and false-positive submissions when needed.

## Settings, logs, crashes
- Settings: `%LOCALAPPDATA%\Hadron\settings.ini`, or portable mode when `Hadron.ini` sits next to the exe.
- Logs: `%LOCALAPPDATA%\Hadron\logs\` (session log).
- Crash dumps: `SetUnhandledExceptionFilter` + `MiniDumpWriteDump`.
- Exports go through a Save dialog. Never write "next to the exe", which fails when run from inside a zip.

## Data embedding
Generated data (e.g. the BIOS settings map) is embedded as module-prefixed `RCDATA`. Generators live in `tools/`,
and their output is committed so a C++-only build works. Copyrighted inputs (vendor BIOS images) and personal data
(the owner's NVRAM snapshot) are never committed.
