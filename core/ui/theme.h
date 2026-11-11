// core/ui/theme.h — colour tokens, DPI scaling and fonts. Minimal in the skeleton; the full widget
// set, the four themes and the Windows-accent logic arrive in the core/ui PR. The Baseline palette
// below is DiskMap's legacy palette (docs/legacy/diskmap.md), which is the project default.
#pragma once

#include "core/sys/win_compat.h"

namespace hx::ui {

struct Theme {
    COLORREF bg, panel, panel2, panel3, border;
    COLORREF text1, text2, text3;
    COLORREF accent, warn, bad, good;
};

// Baseline (default look). TODO(core/ui PR): light/dark/fun/boring + accent from the Windows accent.
inline const Theme& Baseline() {
    static const Theme t{
        RGB(17, 19, 23),   RGB(24, 27, 33),  RGB(32, 36, 43), RGB(42, 47, 56), RGB(40, 44, 52),
        RGB(232, 235, 240), RGB(156, 163, 176), RGB(108, 115, 128),
        RGB(96, 160, 255), RGB(240, 180, 80), RGB(240, 110, 100), RGB(110, 200, 140),
    };
    return t;
}

inline int S(int px, UINT dpi) { return MulDiv(px, (int)dpi, 96); }

// Per-DPI fonts, rebuilt on WM_DPICHANGED (OnDpiChanged). Segoe UI, with Consolas for numbers.
struct Fonts {
    HFONT ui = nullptr, semi = nullptr, caption = nullptr, title = nullptr, mono = nullptr;

    void Build(UINT dpi) {
        Free();
        auto f = [&](int px, int weight, const wchar_t* face) {
            return CreateFontW(-S(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, face);
        };
        ui    = f(15, FW_NORMAL,   L"Segoe UI");
        semi  = f(15, FW_SEMIBOLD, L"Segoe UI");
        caption = f(12, FW_NORMAL,   L"Segoe UI");
        title = f(20, FW_SEMIBOLD, L"Segoe UI");
        mono  = f(14, FW_NORMAL,   L"Consolas");
    }
    void Free() {
        for (HFONT* h : {&ui, &semi, &caption, &title, &mono})
            if (*h) { DeleteObject(*h); *h = nullptr; }
    }
    ~Fonts() { Free(); }
};

}  // namespace hx::ui
