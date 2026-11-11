// core/ui/draw.h — tiny GDI drawing primitives shared by the host chrome and every widget.
// Header-only, no state. Each helper creates and frees its own GDI objects, so callers never leak.
// All of core/ui draws dark, custom, double-buffered (CLAUDE.md "UI"); these are the building blocks.
#pragma once

#include "core/sys/win_compat.h"
#include "core/ui/theme.h"
#include "core/ui/aa.h"
#include <string>

namespace hx::ui {

// Sentinel for "no colour" (e.g. a fill with no border). A real COLORREF is 0x00RRGGBB, so the top
// byte being set never collides with one. (Avoids depending on commctrl.h's CLR_NONE.)
inline constexpr COLORREF kNoColor = 0xFFFFFFFFu;

inline void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

inline void HLine(HDC dc, int x0, int x1, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c), old = (HPEN)SelectObject(dc, p);
    MoveToEx(dc, x0, y, nullptr); LineTo(dc, x1, y);
    SelectObject(dc, old); DeleteObject(p);
}

inline void VLine(HDC dc, int x, int y0, int y1, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c), old = (HPEN)SelectObject(dc, p);
    MoveToEx(dc, x, y0, nullptr); LineTo(dc, x, y1);
    SelectObject(dc, old); DeleteObject(p);
}

// 1px outline, no fill.
inline void FrameRectC(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

// Filled rounded rectangle with an optional 1px border (border == kNoColor for none), anti-aliased.
// `radius` keeps GDI RoundRect's meaning (the corner ellipse's diameter), so callers didn't change.
inline void RoundFill(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border = kNoColor) {
    AaRoundRect(dc, r, radius / 2.0, fill, border);
}

// DrawTextW wrappers. The caller owns the font (selected into dc); colour and transparent bg set here.
inline void DrawText(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_NOPREFIX);
}
inline void TextLeft(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c) {
    DrawText(dc, s, r, f, c, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
}
inline void TextRight(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c) {
    DrawText(dc, s, r, f, c, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
}

// Keyboard focus rectangle: a dotted outline in the accent colour, drawn inside r.
inline void FocusRectC(HDC dc, const RECT& r, COLORREF c) {
    HPEN p = CreatePen(PS_DOT, 1, c), old = (HPEN)SelectObject(dc, p);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    int om = SetBkMode(dc, TRANSPARENT);
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SetBkMode(dc, om);
    SelectObject(dc, ob); SelectObject(dc, old); DeleteObject(p);
}

}  // namespace hx::ui
