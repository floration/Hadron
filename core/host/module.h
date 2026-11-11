// core/host/module.h — the module contract. See docs/ARCHITECTURE.md.
//
// A module draws only inside the HWND the host gives it, never creates another top-level window,
// and never calls PostQuitMessage. Its window classes are named "Hadron.<id>.<name>". Worker
// threads post completion to the module HWND; the host keeps that HWND alive until Shutdown() returns.
#pragma once

#include "core/sys/win_compat.h"
#include <memory>
#include <vector>

namespace hx {

// Required: every exe is requireAdministrator (CLAUDE.md hard rule 4, rewritten 2026-11-11).
// None/Optional remain only so a future non-admin tool can declare intent; the build no longer
// emits an asInvoker manifest. A module that needs more than admin (SYSTEM/TrustedInstaller) does
// it behind its own off-by-default toggle, not through this enum.
enum class Admin { None, Optional, Required };

enum class LogLevel { Debug, Info, Warn, Error };

struct ModuleInfo {
    const wchar_t* id;       // "hello": used by --module/--solo and in class/resource names
    const wchar_t* name;     // "Hello"
    const wchar_t* summary;  // one line for the sidebar/catalog
    Admin admin;
};

namespace sys { class Settings; }   // core/sys/settings.h

// Implemented by both RunStandalone and RunSuite. A module talks to the shell only through this.
struct IHost {
    virtual ~IHost() = default;
    virtual HWND Frame() = 0;                     // owner for dialogs; never make another top-level window
    virtual UINT Dpi() = 0;
    virtual void SetTitle(const wchar_t*) = 0;    // replaces legacy SetWindowTextW(hwnd, ...)
    virtual void SetStatus(const wchar_t*) = 0;
    virtual void SetBusy(bool) = 0;               // drives the activity indicator only; CanClose() is the close gate
    virtual bool IsElevated() = 0;
    virtual sys::Settings& Prefs() = 0;           // per-module section (renamed from Settings(): see plan)
    virtual void Log(LogLevel, const wchar_t*) = 0;
};

class Module {
public:
    virtual ~Module() = default;                       // constructors do no I/O
    virtual const ModuleInfo& Info() const = 0;
    virtual HWND Create(IHost&, HWND parent, const RECT&) = 0;  // lazy, on first activation
    virtual void OnActivate() {}
    virtual void OnDeactivate() {}                     // workers keep running while hidden
    virtual void OnDpiChanged(UINT dpi) {}             // before the host resizes; rebuild fonts/metrics
    virtual bool PreTranslateMessage(MSG&) { return false; }   // active module only (accelerators)
    virtual bool CanClose() { return true; }           // false while a plan executes
    virtual void Shutdown() {}                         // cancel; must not return while any worker is alive
    virtual int  RunCli(int argc, wchar_t** argv) { return -1; }   // exports and other headless modes

    // Load fixture data synchronously, for --screenshot and --selftest (no worker thread, so the
    // render is deterministic). Live modules load their real data in Create()/OnActivate() instead.
    virtual void LoadFixture(const wchar_t* dir) { (void)dir; }

    // Paint the module's whole view into dc within rc. The host calls this for WM_PAINT (through a
    // back buffer) and for WM_PRINTCLIENT / --screenshot, so the two always match. Modules override
    // this instead of handling WM_PAINT themselves.
    virtual void Paint(HDC dc, const RECT& rc) { (void)dc; (void)rc; }
};

// Host message-ID split (docs/ARCHITECTURE.md): the host owns WM_APP+0..0xFF; modules start here.
constexpr UINT WM_MODULE_BASE = WM_APP + 0x100;

}  // namespace hx
