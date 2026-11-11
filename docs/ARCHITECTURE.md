# Hadron architecture

## Layout
```
hadron/
  CMakeLists.txt            top-level; options HADRON_STANDALONE (each module exe) and HADRON_SUITE (Hadron.exe)
  core/                     static library shared by everything
    ui/                     theme tokens, fonts, double-buffered painting, widgets (chips, buttons, list view with
                            sortable columns + draggable scrollbar, detail pane, search box, progress bar, dialogs)
    host/                   Module interface, module registry, standalone runner, suite shell (sidebar)
    sys/                    elevation + relaunch, privileges, long paths, worker threads, file dialogs, zip writer
    fs/                     safe file operations: plan -> confirm -> execute -> undo log; reparse-point checks
  modules/
    <id>/                   one folder per module
      module.cpp            implements hx::Module
      standalone.cpp        5-line WinMain: return hx::RunStandalone(MakeXxxModule());
      resources/            icon, manifest fragment, embedded data
      tests/                selftest / unit tests (no GUI)
  app/
    hadron.cpp              WinMain for the suite: registers every module, runs the shell
  tools/                    offline generators (e.g. the BIOS settings pipeline in Python)
  legacy/                   the original single-file tools, kept as reference while porting
```

## The module contract
```cpp
namespace hx {
struct ModuleInfo {
    const wchar_t* id;          // "bios", "diskmap" ... used for --module <id> and exe names
    const wchar_t* name;        // "BIOS Settings Explorer"
    const wchar_t* summary;     // one line for the sidebar tooltip
    bool needsAdmin;            // host offers an elevated relaunch when true and not elevated
};

class Module {
public:
    virtual ~Module() = default;
    virtual const ModuleInfo& Info() const = 0;
    virtual HWND Create(HWND parent, const RECT& area) = 0;   // create the module's root child window
    virtual void OnActivate() {}                              // shown (suite tab switched to it)
    virtual void OnDeactivate() {}
    virtual bool CanClose() { return true; }                  // false while an operation is running / unsaved
    virtual int  RunCli(int argc, wchar_t** argv) { return -1; } // optional headless mode (--selftest, exports)
};

std::unique_ptr<Module> MakeBiosModule();   // one factory per module, declared in core/host/registry.h
int RunStandalone(std::unique_ptr<Module> m);   // top-level window that hosts exactly one module
int RunSuite(std::vector<std::unique_ptr<Module>> mods);   // sidebar + content area
}
```
- A module never creates top-level windows except dialogs. It draws inside the HWND the host gives it.
  That is what lets the same code run standalone or inside the suite.
- A module never calls `PostQuitMessage`. The host owns the message loop.
- Command line, identical for standalone and suite: `--module <id>` (suite only: open on that module),
  `--elevated` (marks a relaunch; never relaunch again), and module-specific arguments passed to `RunCli`.

## Build outputs
| Target | File | Manifest |
|---|---|---|
| Suite | `Hadron.exe` | asInvoker; elevated relaunch per module on demand |
| Each module | `Hadron<Name>.exe` (e.g. `HadronBios.exe`, `HadronDiskMap.exe`) | `requireAdministrator` only if `needsAdmin` |

Release zip: `Hadron-<version>.zip` containing the suite exe, every standalone exe, and README.txt.

## Data embedding
Large generated data (e.g. the BIOS settings map) is embedded as an `RCDATA` resource, produced by
`tools/` scripts and checked in as a generated file so a C++-only build works. Each generator documents
its inputs. Inputs that are copyrighted (vendor BIOS images) or personal (the owner's NVRAM dump) are **never committed**.
