// core/ui/theme.cpp — the four neutral palettes, the Windows-accent read, and the colour maths that
// turns one base accent into the square/tetradic set. No third-party code: own HSL conversion.
#include "core/ui/theme.h"
#include <string>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

namespace hx::ui {

const wchar_t* ThemeName(ThemeId id) {
    return id == ThemeId::Light ? L"Light" : L"Dark";
}

bool ParseTheme(const wchar_t* key, ThemeId* out) {
    std::wstring k = key ? key : L"";
    if (k == L"light") { *out = ThemeId::Light; return true; }
    if (k == L"dark") { *out = ThemeId::Dark; return true; }
    return false;   // the retired fun/boring values fall back to the caller's default (Dark)
}

const wchar_t* LayoutName(Layout l) {
    return l == Layout::Console ? L"Console" : l == Layout::ThreePane ? L"Three-Pane" : L"Baseline";
}

const wchar_t* LayoutKey(Layout l) {
    return l == Layout::Console ? L"console" : l == Layout::ThreePane ? L"threepane" : L"baseline";
}

bool ParseLayout(const wchar_t* key, Layout* out) {
    std::wstring k = key ? key : L"";
    if (k == L"baseline") { *out = Layout::Baseline; return true; }
    if (k == L"threepane" || k == L"three-pane") { *out = Layout::ThreePane; return true; }
    if (k == L"console") { *out = Layout::Console; return true; }
    return false;
}

// Palettes from the owner's UI directions canvas (owner, 2026-11-13: "only light or dark, and they take the
// colors from the hadron ui directions page; only accent colors are taken from your windows profile").
// Dark: each layout uses its own direction's board (1 Baseline, 7 Three-Pane, 4 Console). Light: direction 5
// "Paper", the canvas's only light board, for every layout. The accent fields hold the board's own accent,
// used only when the Windows accent is achromatic; Resolve() overwrites them otherwise.
// Field order: bg, panel, panel2, panel3, border; text1, text2, text3; accent x4; warn, bad, good.
const Theme& ThemeBase(ThemeId id, Layout layout) {
    static const Theme paper{   // 5 · Paper
        ThemeId::Light, Layout::Baseline,
        RGB(244,242,238), RGB(255,255,255), RGB(235,232,225), RGB(217,213,204), RGB(217,213,204),
        RGB(29,31,35),    RGB(58,61,68),    RGB(92,96,104),
        RGB(31,95,191), RGB(31,95,191), RGB(31,95,191), RGB(31,95,191),
        RGB(154,91,0),    RGB(179,38,30),   RGB(26,127,71),   // warn/bad: darkened for 4.5:1 on paper (reasoning)
        false,
    };
    static const Theme baseline{   // 1 · Baseline
        ThemeId::Dark, Layout::Baseline,
        RGB(17,19,23),    RGB(24,27,33),    RGB(31,35,42),    RGB(40,44,52),    RGB(40,44,52),
        RGB(232,235,240), RGB(200,204,212), RGB(156,163,176),
        RGB(96,160,255), RGB(96,160,255), RGB(96,160,255), RGB(96,160,255),
        RGB(238,164,66),  RGB(236,98,98),   RGB(80,214,140),
        false,
    };
    static const Theme threePane{   // 7 · Three-Pane
        ThemeId::Dark, Layout::ThreePane,
        RGB(30,30,30),    RGB(37,37,38),    RGB(45,45,45),    RGB(60,60,60),    RGB(60,60,60),
        RGB(230,230,230), RGB(208,208,208), RGB(168,168,168),
        RGB(53,197,240), RGB(53,197,240), RGB(53,197,240), RGB(53,197,240),
        RGB(240,180,80),  RGB(240,128,112), RGB(110,200,140),
        false,
    };
    static const Theme console{   // 4 · Console
        ThemeId::Dark, Layout::Console,
        RGB(11,13,11),    RGB(16,19,16),    RGB(20,24,20),    RGB(42,48,42),    RGB(42,48,42),
        RGB(214,220,210), RGB(196,204,192), RGB(148,160,148),
        RGB(255,182,72), RGB(255,182,72), RGB(255,182,72), RGB(255,182,72),
        RGB(255,208,138), RGB(255,123,107), RGB(111,220,140),
        false,
    };
    if (id == ThemeId::Light) {
        static Theme light[kLayoutCount];
        static bool init = false;
        if (!init) { for (int i = 0; i < kLayoutCount; i++) { light[i] = paper; light[i].layout = (Layout)i; } init = true; }
        return light[(int)layout];
    }
    return layout == Layout::Console ? console : layout == Layout::ThreePane ? threePane : baseline;
}

COLORREF FallbackAccent(ThemeId id, Layout layout) { return ThemeBase(id, layout).accent; }

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

namespace {
double ChannelLinear(int v) {
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double RelLuminance(COLORREF c) {
    return 0.2126 * ChannelLinear(GetRValue(c)) + 0.7152 * ChannelLinear(GetGValue(c)) + 0.0722 * ChannelLinear(GetBValue(c));
}
}  // namespace

double Contrast(COLORREF a, COLORREF b) {
    double la = RelLuminance(a), lb = RelLuminance(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

COLORREF OnFill(COLORREF fill, COLORREF light, COLORREF dark) {
    double onLight = Contrast(light, fill);
    return onLight >= 4.45 || onLight >= Contrast(dark, fill) ? light : dark;
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

COLORREF Mix(COLORREF a, COLORREF b, double t) {
    auto ch = [t](int x, int y) { return (int)(x * t + y * (1 - t) + 0.5); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

// --- resolve ----------------------------------------------------------------------------------

Theme Resolve(ThemeId id, COLORREF baseAccent, Layout layout) {
    Theme t = ThemeBase(id, layout);
    t.accent  = baseAccent;
    t.accent2 = RotateHue(baseAccent, 90);
    t.accent3 = RotateHue(baseAccent, 180);
    t.accent4 = RotateHue(baseAccent, 270);
    t.highContrast = SystemHighContrast();
    return t;
}

Theme ResolveLive(ThemeId id, Layout layout) {
    bool achromatic = false;
    COLORREF a = SystemAccent(&achromatic);
    if (achromatic) a = FallbackAccent(id, layout);
    return Resolve(id, a, layout);
}

}  // namespace hx::ui
