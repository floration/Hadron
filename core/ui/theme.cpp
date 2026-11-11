// core/ui/theme.cpp — the four neutral palettes, the Windows-accent read, and the colour maths that
// turns one base accent into the square/tetradic set. No third-party code: own HSL conversion.
#include "core/ui/theme.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

namespace hx::ui {

const wchar_t* ThemeName(ThemeId id) {
    switch (id) {
        case ThemeId::Light:  return L"Light";
        case ThemeId::Dark:   return L"Dark";
        case ThemeId::Fun:    return L"Fun";
        case ThemeId::Boring: return L"Boring";
    }
    return L"Dark";
}

const wchar_t* LayoutName(Layout l) {
    return l == Layout::Console ? L"Console" : L"Baseline";
}

// Neutral palettes. Accents are filled with the theme's fallback so ThemeBase is usable alone;
// Resolve() overwrites accent/accent2/3/4 from the chosen base accent.
const Theme& ThemeBase(ThemeId id) {
    static const Theme light{
        ThemeId::Light,
        RGB(246,247,249), RGB(255,255,255), RGB(238,240,243), RGB(226,229,234), RGB(214,218,224),
        RGB(24,27,33),    RGB(90,98,110),   RGB(140,147,158),
        RGB(0,120,215), RGB(0,120,215), RGB(0,120,215), RGB(0,120,215),
        RGB(180,120,20),  RGB(200,60,55),   RGB(40,150,90),
        false,
    };
    static const Theme dark{
        ThemeId::Dark,
        RGB(17,19,23),    RGB(24,27,33),    RGB(32,36,43),    RGB(42,47,56),    RGB(40,44,52),
        RGB(232,235,240), RGB(156,163,176), RGB(108,115,128),
        RGB(96,160,255), RGB(96,160,255), RGB(96,160,255), RGB(96,160,255),
        RGB(240,180,80),  RGB(240,110,100), RGB(110,200,140),
        false,
    };
    static const Theme fun{
        ThemeId::Fun,
        RGB(26,18,38),    RGB(38,26,56),    RGB(52,36,76),    RGB(68,48,98),    RGB(72,52,104),
        RGB(240,235,250), RGB(190,172,214), RGB(146,128,170),
        RGB(255,80,170), RGB(255,80,170), RGB(255,80,170), RGB(255,80,170),
        RGB(255,190,70),  RGB(255,96,120),  RGB(90,220,170),
        false,
    };
    static const Theme boring{
        ThemeId::Boring,
        RGB(234,228,214), RGB(244,239,228), RGB(224,216,198), RGB(210,201,180), RGB(198,188,166),
        RGB(54,48,38),    RGB(104,94,76),   RGB(148,138,118),
        RGB(150,110,70), RGB(150,110,70), RGB(150,110,70), RGB(150,110,70),
        RGB(170,120,30),  RGB(170,70,50),   RGB(90,130,70),
        false,
    };
    switch (id) {
        case ThemeId::Light:  return light;
        case ThemeId::Fun:    return fun;
        case ThemeId::Boring: return boring;
        case ThemeId::Dark:   break;
    }
    return dark;
}

COLORREF FallbackAccent(ThemeId id) {
    switch (id) {
        case ThemeId::Light:  return RGB(0,120,215);
        case ThemeId::Dark:   return RGB(96,160,255);
        case ThemeId::Fun:    return RGB(255,80,170);
        case ThemeId::Boring: return RGB(150,110,70);
    }
    return RGB(96,160,255);
}

// --- colour maths -----------------------------------------------------------------------------

static void RgbToHsl(COLORREF c, double& h, double& s, double& l) {
    double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2.0;
    double d = mx - mn;
    if (d < 1e-9) { h = 0; s = 0; return; }
    s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == r)      h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g) h = (b - r) / d + 2.0;
    else              h = (r - g) / d + 4.0;
    h *= 60.0;
}

static double Hue2Rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0/2.0) return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}

static COLORREF HslToRgb(double h, double s, double l) {
    if (s < 1e-9) {
        int v = (int)std::lround(l * 255.0);
        return RGB(v, v, v);
    }
    double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    double hk = h / 360.0;
    double r = Hue2Rgb(p, q, hk + 1.0/3.0);
    double g = Hue2Rgb(p, q, hk);
    double b = Hue2Rgb(p, q, hk - 1.0/3.0);
    auto to8 = [](double x) { return (int)std::lround(std::clamp(x, 0.0, 1.0) * 255.0); };
    return RGB(to8(r), to8(g), to8(b));
}

bool IsAchromatic(COLORREF c) {
    double h, s, l;
    RgbToHsl(c, h, s, l);
    return s < 0.12 || l < 0.06 || l > 0.94;   // grey, black or white
}

COLORREF RotateHue(COLORREF c, double degrees) {
    double h, s, l;
    RgbToHsl(c, h, s, l);
    h = std::fmod(h + degrees, 360.0);
    if (h < 0) h += 360.0;
    return HslToRgb(h, s, l);
}

// --- system state -----------------------------------------------------------------------------

static bool ReadDwordHKCU(const wchar_t* key, const wchar_t* value, DWORD& out) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, data = 0, sz = sizeof(data);
    LONG r = RegQueryValueExW(h, value, nullptr, &type, (LPBYTE)&data, &sz);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = data;
    return true;
}

COLORREF SystemAccent(bool* achromatic) {
    DWORD v = 0;
    COLORREF c = 0;
    bool found = false;
    // Personalisation accent: HKCU\...\DWM\AccentColor, stored 0xAABBGGRR (ABGR).
    if (ReadDwordHKCU(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", v)) {
        c = RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
        found = true;
    } else if (ReadDwordHKCU(L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", v)) {
        c = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);   // 0xAARRGGBB
        found = true;
    } else {
        DWORD col = 0; BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&col, &opaque))) {
            c = RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
            found = true;
        }
    }
    if (achromatic) *achromatic = !found || IsAchromatic(c);
    return c;
}

static bool SystemHighContrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    return false;
}

// --- resolve ----------------------------------------------------------------------------------

Theme Resolve(ThemeId id, COLORREF baseAccent) {
    Theme t = ThemeBase(id);
    t.accent  = baseAccent;
    t.accent2 = RotateHue(baseAccent, 90);
    t.accent3 = RotateHue(baseAccent, 180);
    t.accent4 = RotateHue(baseAccent, 270);
    t.highContrast = SystemHighContrast();
    return t;
}

Theme ResolveLive(ThemeId id) {
    bool achromatic = false;
    COLORREF a = SystemAccent(&achromatic);
    if (achromatic) a = FallbackAccent(id);
    return Resolve(id, a);
}

}  // namespace hx::ui
