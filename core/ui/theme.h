// core/ui/theme.h — colour tokens, the four themes, DPI scaling and fonts.
//
// Every module renders in one of two layouts (Baseline, the default card look; Console, a dense
// terminal look) and one of four colour themes (Light, Dark, Fun, Boring). Colour comes from the
// theme; the layout changes widget metrics and fonts, not the palette. The main accent is the
// Windows accent colour, with three more accents at +90/+180/+270 hue (a square/tetradic split);
// when the Windows accent is achromatic (grey/black/white) each theme supplies its own accent.
// See docs/DECISIONS.md (2026-11-11) and docs/ARCHITECTURE.md.
#pragma once

#include "core/sys/win_compat.h"
#include <initializer_list>

namespace hx::ui {

enum class ThemeId { Light, Dark, Fun, Boring };
enum class Layout  { Baseline, Console };

constexpr int kThemeCount = 4;
const wchar_t* ThemeName(ThemeId id);   // "Light" / "Dark" / "Fun" / "Boring"
const wchar_t* LayoutName(Layout l);    // "Baseline" / "Console"

// A fully-resolved palette. Neutral tokens come from the ThemeId; the four accents are derived from
// one base accent (the Windows accent, or the theme's fallback). highContrast is set when Windows is
// in a high-contrast scheme, so widgets can switch to the system colours (CLAUDE.md "UI").
struct Theme {
    ThemeId  id = ThemeId::Dark;
    COLORREF bg, panel, panel2, panel3, border;
    COLORREF text1, text2, text3;
    COLORREF accent, accent2, accent3, accent4;   // base, +90, +180, +270 hue
    COLORREF warn, bad, good;
    bool     highContrast = false;
};

// The per-theme neutral palette and the accent each theme uses when the Windows accent is achromatic.
const Theme&  ThemeBase(ThemeId id);          // neutral tokens; accents are placeholders
COLORREF      FallbackAccent(ThemeId id);     // the theme's own accent for the achromatic case

// Colour maths (defined in theme.cpp). Hue is degrees [0,360).
bool     IsAchromatic(COLORREF c);            // grey/black/white: low saturation or extreme lightness
COLORREF RotateHue(COLORREF c, double degrees);

// The live Windows accent colour. Sets *achromatic when it is grey/black/white or can't be read
// (e.g. under Wine), so the caller knows to use FallbackAccent. Reads the DWM accent, then the
// colorization colour, then DwmGetColorizationColor.
COLORREF SystemAccent(bool* achromatic);

// Build a theme from a ThemeId and an explicit base accent (accent2/3/4 are hue-rotated from it).
// Deterministic — used by --screenshot/--selftest with FallbackAccent for stable goldens.
Theme Resolve(ThemeId id, COLORREF baseAccent);

// Build a theme using the live Windows accent, falling back to the theme's accent when achromatic.
Theme ResolveLive(ThemeId id);

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
        ui      = f(15, FW_NORMAL,   body);
        semi    = f(15, FW_SEMIBOLD, body);
        caption = f(12, FW_NORMAL,   body);
        title   = f(20, FW_SEMIBOLD, (layout == Layout::Console) ? L"Consolas" : L"Segoe UI");
        mono    = f(14, FW_NORMAL,   L"Consolas");
    }
    void Free() {
        for (HFONT* h : {&ui, &semi, &caption, &title, &mono})
            if (*h) { DeleteObject(*h); *h = nullptr; }
    }
    ~Fonts() { Free(); }
};

}  // namespace hx::ui
