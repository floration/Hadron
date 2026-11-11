// core/host/cmdline.h — the one command-line parse, identical for standalone and suite.
// See docs/ARCHITECTURE.md "Command line".
#pragma once

#include "core/sys/win_compat.h"
#include "core/ui/theme.h"
#include <string>
#include <vector>

namespace hx {

struct CommandLine {
    std::wstring module;          // --module <id>: suite opens on that module
    std::wstring solo;            // --solo <id>: suite runs only that module, as if standalone
    bool elevated = false;        // --elevated marks a relaunch; kept for forward-compat, never relaunches (rule 4)
    bool selftest = false;        // --selftest: run the active module's checks, headless
    std::wstring screenshot;      // --screenshot <png>: render the main view and exit
    std::wstring fixture;         // --fixture <dir>: data the screenshot/selftest loads
    ui::ThemeId theme = ui::ThemeId::Dark;        // --theme light|dark|fun|boring
    ui::Layout  layout = ui::Layout::Baseline;    // --layout baseline|console
    bool     hasAccent = false;   // --accent RRGGBB: stand-in Windows accent for --screenshot (design review)
    COLORREF accent = 0;
    bool themeGiven = false, layoutGiven = false; // the flag was passed (overrides the saved choice)
    std::vector<std::wstring> rest;   // everything else, passed to RunCli

    // Parses GetCommandLineW()-style input. Paths in --screenshot and --fixture are made absolute.
    static CommandLine Parse(const wchar_t* raw);
};

}  // namespace hx
