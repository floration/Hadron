// core/ui/theme.h — colour tokens, the two themes, DPI scaling and fonts.
//
// Every module renders in one of three layouts (Baseline, the default card look; Three-Pane, tree | list |
// detail; Console, a dense terminal look) and one of two colour themes (Light, Dark). Neutral colours come
// from the UI directions canvas: in Dark each layout uses its own board, in Light every layout uses board 5
// "Paper". The main accent is the Windows accent colour, with three more accents at +90/+180/+270 hue (a
// square/tetradic split); when the Windows accent is achromatic (grey/black/white) the board's own accent is
// used. See docs/DECISIONS.md (2026-11-13) and docs/ARCHITECTURE.md.
#pragma once

#include "core/sys/win_compat.h"
#include <initializer_list>

namespace hx::ui {

// Three-Pane (direction 7, owner 2026-11-13): a navigation tree (modules and their sections) | the list |
// a detail pane on the right, with draggable splitters; square corners, 24px rows. Modules that have no
// Three-Pane code of their own draw their Baseline body in the middle pane.
enum class Layout  { Baseline, ThreePane, Console };
constexpr int kLayoutCount = 3;
// Owner, 2026-11-13: only Light and Dark. Neutral colours come from the UI directions canvas (Dark: the
// active layout's own board; Light: board 5 "Paper"); only the accent follows Windows.
enum class ThemeId { Light, Dark };

constexpr int kThemeCount = 2;
const wchar_t* ThemeName(ThemeId id);   // "Light" / "Dark"
bool ParseTheme(const wchar_t* key, ThemeId* out);   // "light" / "dark"
const wchar_t* LayoutName(Layout l);    // "Baseline" / "Three-Pane" / "Console"
const wchar_t* LayoutKey(Layout l);     // "baseline" / "threepane" / "console": settings.ini and --layout
bool ParseLayout(const wchar_t* key, Layout* out);   // accepts LayoutKey values (and "three-pane")

// A fully-resolved palette. Neutral tokens come from the ThemeId; the four accents are derived from
// one base accent (the Windows accent, or the theme's fallback). highContrast is set when Windows is
// in a high-contrast scheme, so widgets can switch to the system colours (CLAUDE.md "UI").
struct Theme {
    ThemeId  id = ThemeId::Dark;
    Layout   layout = Layout::Baseline;   // the palette depends on the layout in Dark (one board each)
    COLORREF bg, panel, panel2, panel3, border;
    COLORREF text1, text2, text3;
    COLORREF accent, accent2, accent3, accent4;   // base, +90, +180, +270 hue
    COLORREF warn, bad, good;
    bool     highContrast = false;
};

// The per-theme neutral palette and the accent each theme uses when the Windows accent is achromatic.
const Theme&  ThemeBase(ThemeId id, Layout layout = Layout::Baseline);          // neutral tokens; accents are placeholders
COLORREF      FallbackAccent(ThemeId id, Layout layout = Layout::Baseline);     // the theme's own accent for the achromatic case

// Colour maths (defined in theme.cpp). Hue is degrees [0,360).
bool     IsAchromatic(COLORREF c);            // grey/black/white: low saturation or extreme lightness
COLORREF RotateHue(COLORREF c, double degrees);

// WCAG 2 contrast ratio of two colours (1..21), from sRGB relative luminance.
double   Contrast(COLORREF a, COLORREF b);
COLORREF Mix(COLORREF a, COLORREF b, double t);   // t*a + (1-t)*b per channel
// Text on a filled control (a selection, an accent button, a chip): `light` while it reaches 4.5:1 on the fill
// (to one decimal, so >= 4.45: white on Windows' default blue 0078D7 is 4.50), else whichever of light and dark
// contrasts more. One rule for every module, so the suite agrees with itself on any Windows accent.
COLORREF OnFill(COLORREF fill, COLORREF light = RGB(255, 255, 255), COLORREF dark = RGB(0, 0, 0));

// The live Windows accent colour. Sets *achromatic when it is grey/black/white or can't be read
// (e.g. under Wine), so the caller knows to use FallbackAccent. Reads the DWM accent, then the
// colorization colour, then DwmGetColorizationColor.
COLORREF SystemAccent(bool* achromatic);

// Build a theme from a ThemeId and an explicit base accent (accent2/3/4 are hue-rotated from it).
// Deterministic — used by --screenshot/--selftest with FallbackAccent for stable goldens.
Theme Resolve(ThemeId id, COLORREF baseAccent, Layout layout = Layout::Baseline);

// Build a theme using the live Windows accent, falling back to the theme's accent when achromatic.
Theme ResolveLive(ThemeId id, Layout layout = Layout::Baseline);

// Dark theme with its fallback accent: the project default, and what the skeleton host draws.
inline const Theme& Baseline() {
    static const Theme t = Resolve(ThemeId::Dark, FallbackAccent(ThemeId::Dark));
    return t;
}

inline int S(int px, UINT dpi) { return MulDiv(px, (int)dpi, 96); }

// Per-DPI fonts, rebuilt on WM_DPICHANGED (OnDpiChanged). Segoe UI, with Consolas for numbers and
// for the whole Console layout. Build(dpi, layout): Console uses the monospace face for body text.
struct Fonts {
    HFONT ui = nullptr, semi = nullptr, caption = nullptr, title = nullptr, mono = nullptr;

    void Build(UINT dpi, Layout layout = Layout::Baseline) {
        Free();
        auto f = [&](int px, int weight, const wchar_t* face) {
            return CreateFontW(-S(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, face);
        };
        const wchar_t* body = (layout == Layout::Console) ? L"Consolas" : L"Segoe UI";
        // Boards 7 and 4 are dense: 13px body text. Baseline (board 1) keeps its 15px.
        const bool dense = layout != Layout::Baseline;
        ui      = f(dense ? 13 : 15, FW_NORMAL,   body);
        semi    = f(dense ? 13 : 15, FW_SEMIBOLD, body);
        caption = f(dense ? 12 : 12, FW_NORMAL,   body);
        title   = f(dense ? 16 : 20, FW_SEMIBOLD, body);
        mono    = f(dense ? 13 : 14, FW_NORMAL,   L"Consolas");
    }
    void Free() {
        for (HFONT* h : {&ui, &semi, &caption, &title, &mono})
            if (*h) { DeleteObject(*h); *h = nullptr; }
    }
    ~Fonts() { Free(); }
};

}  // namespace hx::ui
