// core/ui/aa.cpp — anti-aliased shapes for GDI. See aa.h.
#include "core/ui/aa.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hx::ui {

namespace {

// One premultiplied BGRA pixel from a colour and alpha in [0,1].
inline uint32_t Pre(COLORREF c, double a) {
    if (a <= 0) return 0;
    if (a > 1) a = 1;
    auto ch = [a](int v) { return (uint32_t)(v * a + 0.5); };
    return (uint32_t)(a * 255 + 0.5) << 24 | ch(GetRValue(c)) << 16 | ch(GetGValue(c)) << 8 | ch(GetBValue(c));
}

// Blend a w x h block of premultiplied pixels at (x, y). `px` is row-major, top-down.
void Blit(HDC dc, int x, int y, int w, int h, const std::vector<uint32_t>& px) {
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return; }
    memcpy(bits, px.data(), (size_t)w * h * 4);
    HDC mem = CreateCompatibleDC(dc);
    if (!mem) { DeleteObject(bmp); return; }
    HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
}

void Fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    if (r <= l || b <= t) return;
    RECT rc{l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

// Signed distance from point (px, py) to a rounded box centred at (cx, cy) with half-size (hx, hy) and
// corner radius rad. Negative inside.
inline double Sdf(double px, double py, double cx, double cy, double hx, double hy, double rad) {
    double qx = std::fabs(px - cx) - (hx - rad), qy = std::fabs(py - cy) - (hy - rad);
    double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - rad;
}

struct Shape {
    double cx, cy, hx, hy, rad, bw;
    COLORREF fill, border;
    uint32_t Pixel(int x, int y) const {   // x, y: pixel indices; sample the centre
        double d = Sdf(x + 0.5, y + 0.5, cx, cy, hx, hy, rad);
        double out = std::clamp(0.5 - d, 0.0, 1.0);
        if (out <= 0) return 0;
        if (border == kAaNoColor) return Pre(fill, out);
        double in = std::clamp(0.5 - (d + bw), 0.0, 1.0);   // coverage of the inside of the border
        double ab = out - in;                                // border ring coverage
        if (fill == kAaNoColor) return Pre(border, ab);
        // border over fill, both opaque colours: mix by coverage, total alpha = out
        double a = out;
        double r = (GetRValue(border) * ab + GetRValue(fill) * in) / a;
        double g = (GetGValue(border) * ab + GetGValue(fill) * in) / a;
        double b = (GetBValue(border) * ab + GetBValue(fill) * in) / a;
        return Pre(RGB((int)(r + 0.5), (int)(g + 0.5), (int)(b + 0.5)), a);
    }
    void Render(HDC dc, int l, int t, int r, int b) const {   // the block [l,r) x [t,b) via a DIB
        int w = r - l, h = b - t;
        if (w <= 0 || h <= 0) return;
        std::vector<uint32_t> px((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) px[(size_t)y * w + x] = Pixel(l + x, t + y);
        Blit(dc, l, t, w, h, px);
    }
};

}  // namespace

void AaRoundRect(HDC dc, const RECT& rc, double radius, COLORREF fill, COLORREF border, double borderW) {
    int l = rc.left, t = rc.top, r = rc.right, b = rc.bottom;
    int w = r - l, h = b - t;
    if (w <= 0 || h <= 0 || (fill == kAaNoColor && border == kAaNoColor)) return;
    double rad = std::clamp(radius, 0.0, std::min(w, h) / 2.0);
    if (border != kAaNoColor) borderW = std::clamp(borderW, 0.0, std::min(w, h) / 2.0);
    Shape s{l + w / 2.0, t + h / 2.0, w / 2.0, h / 2.0, rad, borderW, fill, border};
    int R = (int)std::ceil(rad);
    if (R <= 0 || 2 * R >= w || 2 * R >= h) {
        if (R <= 0 && (border == kAaNoColor || borderW == std::floor(borderW))) {
            // Square corners with whole-pixel borders: plain GDI is exact.
            int bwI = border == kAaNoColor ? 0 : (int)borderW;
            if (fill != kAaNoColor) Fill(dc, l + bwI, t + bwI, r - bwI, b - bwI, fill);
            if (bwI) {
                Fill(dc, l, t, r, t + bwI, border); Fill(dc, l, b - bwI, r, b, border);
                Fill(dc, l, t + bwI, l + bwI, b - bwI, border); Fill(dc, r - bwI, t + bwI, r, b - bwI, border);
            }
            return;
        }
        s.Render(dc, l, t, r, b);   // pills and small shapes: the whole thing is curved
        return;
    }
    // Four curved corners through the DIB; everything else is straight and pixel-exact.
    s.Render(dc, l, t, l + R, t + R);
    s.Render(dc, r - R, t, r, t + R);
    s.Render(dc, l, b - R, l + R, b);
    s.Render(dc, r - R, b - R, r, b);
    bool whole = border == kAaNoColor || borderW == std::floor(borderW);
    if (!whole) {   // fractional border widths: render the edge strips too
        int e = (int)std::ceil(borderW);
        if (fill != kAaNoColor) Fill(dc, l + e, t + R, r - e, b - R, fill), Fill(dc, l + R, t + e, r - R, t + R, fill),
                                Fill(dc, l + R, b - R, r - R, b - e, fill);
        s.Render(dc, l + R, t, r - R, t + e); s.Render(dc, l + R, b - e, r - R, b);
        s.Render(dc, l, t + R, l + e, b - R); s.Render(dc, r - e, t + R, r, b - R);
        return;
    }
    int bwI = border == kAaNoColor ? 0 : (int)borderW;
    if (fill != kAaNoColor) {
        Fill(dc, l + R, t + bwI, r - R, b - bwI, fill);       // middle columns
        Fill(dc, l + bwI, t + R, l + R, b - R, fill);          // left band
        Fill(dc, r - R, t + R, r - bwI, b - R, fill);          // right band
    }
    if (bwI) {
        Fill(dc, l + R, t, r - R, t + bwI, border);
        Fill(dc, l + R, b - bwI, r - R, b, border);
        Fill(dc, l, t + R, l + bwI, b - R, border);
        Fill(dc, r - bwI, t + R, r, b - R, border);
    }
}

void AaEllipse(HDC dc, const RECT& rc, COLORREF fill, COLORREF border) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (w == h) { AaRoundRect(dc, rc, w / 2.0, fill, border); return; }
    // Non-circular ellipse: scale the distance by the smaller semi-axis (good enough for small marks).
    double a = w / 2.0, b = h / 2.0, cx = rc.left + a, cy = rc.top + b, m = std::min(a, b);
    std::vector<uint32_t> px((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double nx = (rc.left + x + 0.5 - cx) / a, ny = (rc.top + y + 0.5 - cy) / b;
            double d = (std::sqrt(nx * nx + ny * ny) - 1.0) * m;
            double out = std::clamp(0.5 - d, 0.0, 1.0);
            double in = border == kAaNoColor ? out : std::clamp(0.5 - (d + 1), 0.0, 1.0);
            uint32_t p = 0;
            if (fill != kAaNoColor && border != kAaNoColor) {
                double ab = out - in;
                double r = out > 0 ? (GetRValue(border) * ab + GetRValue(fill) * in) / out : 0;
                double g = out > 0 ? (GetGValue(border) * ab + GetGValue(fill) * in) / out : 0;
                double bb = out > 0 ? (GetBValue(border) * ab + GetBValue(fill) * in) / out : 0;
                p = Pre(RGB((int)(r + .5), (int)(g + .5), (int)(bb + .5)), out);
            } else if (fill != kAaNoColor) p = Pre(fill, out);
            else if (border != kAaNoColor) p = Pre(border, out - in);
            px[(size_t)y * w + x] = p;
        }
    Blit(dc, rc.left, rc.top, w, h, px);
}

void AaPolygon(HDC dc, const POINT* pts, int n, COLORREF fill) {
    if (n < 3) return;
    long l = pts[0].x, t = pts[0].y, r = l, b = t;
    for (int i = 1; i < n; i++) {
        l = std::min(l, pts[i].x); r = std::max(r, pts[i].x);
        t = std::min(t, pts[i].y); b = std::max(b, pts[i].y);
    }
    int w = (int)(r - l), h = (int)(b - t);
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;   // meant for small marks
    auto inside = [&](double x, double y) {               // even-odd crossing test
        bool in = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            double xi = pts[i].x, yi = pts[i].y, xj = pts[j].x, yj = pts[j].y;
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
        }
        return in;
    };
    std::vector<uint32_t> px((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    hits += inside(l + x + (sx + 0.5) / 4.0, t + y + (sy + 0.5) / 4.0);
            px[(size_t)y * w + x] = Pre(fill, hits / 16.0);
        }
    Blit(dc, (int)l, (int)t, w, h, px);
}

}  // namespace hx::ui
