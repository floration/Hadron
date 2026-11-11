// core/ui/aa.h — anti-aliased shapes for GDI (which has none). Own code, no third-party (hard rule 3).
//
// Rounded rectangles, circles and small polygons are drawn with per-pixel coverage: the curved parts are
// rendered into a small premultiplied 32-bit DIB and AlphaBlend-ed onto the target; straight runs use plain
// FillRect, so a big rounded panel costs four tiny corner blits. Kept apart from draw.h so the host, which
// has its own FillSolid/TextLeft helpers, can include it without name clashes.
#pragma once

#include "core/sys/win_compat.h"

namespace hx::ui {

inline constexpr COLORREF kAaNoColor = 0xFFFFFFFFu;   // same value as draw.h kNoColor: "don't draw this part"

// Rounded rectangle covering [r.left, r.right) x [r.top, r.bottom). `radius` is the true corner radius in
// pixels (GDI RoundRect takes the ellipse *diameter*). The border, if any, is `borderW` px wide and lies
// inside r, like a GDI pen in RoundRect. fill or border may be kAaNoColor.
void AaRoundRect(HDC dc, const RECT& r, double radius, COLORREF fill, COLORREF border = kAaNoColor,
                 double borderW = 1.0);

// Circle or ellipse inscribed in r (a circle when r is square), same fill/border rules.
void AaEllipse(HDC dc, const RECT& r, COLORREF fill, COLORREF border = kAaNoColor);

// Filled polygon (any simple polygon; meant for small marks such as sort arrows), 4x4 supersampled.
// Vertex coordinates are pixel-corner coordinates, as GDI Polygon uses.
void AaPolygon(HDC dc, const POINT* pts, int n, COLORREF fill);

}  // namespace hx::ui
