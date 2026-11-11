// core/ui/widgets.cpp — implementations. All drawing goes through core/ui/draw.h (self-freeing GDI).
#include "core/ui/widgets.h"
#include "core/ui/draw.h"
#include <algorithm>

namespace hx::ui {

// High-contrast fallback: when the system is in a high-contrast scheme, widgets draw with the system
// colours instead of the theme so they stay legible (CLAUDE.md "UI"). Pick() returns the system
// colour in high contrast, else the theme colour.
namespace {
struct Pal {
    const Theme& t;
    COLORREF Pick(COLORREF themed, int sysIndex) const {
        return t.highContrast ? GetSysColor(sysIndex) : themed;
    }
    COLORREF bg()     const { return Pick(t.bg,     COLOR_WINDOW); }
    COLORREF panel()  const { return Pick(t.panel,  COLOR_WINDOW); }
    COLORREF panel2() const { return Pick(t.panel2, COLOR_BTNFACE); }
    COLORREF panel3() const { return Pick(t.panel3, COLOR_BTNFACE); }
    COLORREF border() const { return Pick(t.border, COLOR_BTNSHADOW); }
    COLORREF text1()  const { return Pick(t.text1,  COLOR_WINDOWTEXT); }
    COLORREF text2()  const { return Pick(t.text2,  COLOR_WINDOWTEXT); }
    COLORREF text3()  const { return Pick(t.text3,  COLOR_GRAYTEXT); }
    COLORREF accent() const { return Pick(t.accent, COLOR_HIGHLIGHT); }
    COLORREF selBg()  const { return Pick(t.accent, COLOR_HIGHLIGHT); }
    // Readable text on the selection fill: system colour in high contrast, else black or white by
    // the fill's luminance (Rec. 601), so a saturated accent of any theme stays legible.
    COLORREF selText()const {
        if (t.highContrast) return GetSysColor(COLOR_HIGHLIGHTTEXT);
        COLORREF c = t.accent;
        int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
        return lum > 140 ? RGB(16, 18, 22) : RGB(245, 247, 250);
    }
};
}  // namespace

// ================================ ScrollBar ===================================================

void ScrollBar::Configure(int viewportPx, int contentPx) {
    viewport_ = std::max(0, viewportPx);
    content_  = std::max(0, contentPx);
    SetOffset(offset_);   // re-clamp
}

void ScrollBar::SetOffset(int px) {
    offset_ = std::clamp(px, 0, MaxOffset());
}

RECT ScrollBar::ThumbRect(const RECT& track) const {
    if (!Visible()) return RECT{0, 0, 0, 0};
    int trackH = track.bottom - track.top;
    int thumbH = (int)((long long)trackH * viewport_ / content_);
    thumbH = std::clamp(thumbH, std::min(24, trackH), trackH);
    int travel = trackH - thumbH;
    int mx = MaxOffset();
    int y = track.top + (mx > 0 ? (int)((long long)offset_ * travel / mx) : 0);
    return RECT{track.left, y, track.right, y + thumbH};
}

void ScrollBar::Paint(HDC dc, const RECT& track, const PaintCtx& c) {
    if (!Visible()) return;
    FillSolid(dc, track, c.theme.panel2);
    RECT thumb = ThumbRect(track);
    int inset = std::max(2, S(2, c.dpi));
    RECT t{thumb.left + inset, thumb.top + inset, thumb.right - inset, thumb.bottom - inset};
    COLORREF col = dragging_ ? c.theme.accent : c.theme.panel3;
    RoundFill(dc, t, S(5, c.dpi), col);
}

bool ScrollBar::OnMouse(UINT msg, int x, int y, const RECT& track) {
    if (!Visible()) { if (dragging_) { dragging_ = false; return true; } return false; }
    if (msg == WM_LBUTTONDOWN) {
        RECT thumb = ThumbRect(track);
        if (x >= track.left && x < track.right && y >= track.top && y < track.bottom) {
            if (y >= thumb.top && y < thumb.bottom) {
                dragging_ = true; dragStartY_ = y; dragStartOffset_ = offset_;
            } else {
                // Page towards the click.
                SetOffset(offset_ + (y < thumb.top ? -viewport_ : viewport_));
            }
            return true;
        }
    } else if (msg == WM_MOUSEMOVE && dragging_) {
        int trackH = track.bottom - track.top;
        int thumbH = ThumbRect(track).bottom - ThumbRect(track).top;
        int travel = std::max(1, trackH - thumbH);
        int dy = y - dragStartY_;
        SetOffset(dragStartOffset_ + (int)((long long)dy * MaxOffset() / travel));
        return true;
    } else if (msg == WM_LBUTTONUP && dragging_) {
        dragging_ = false;
        return true;
    }
    return false;
}

// ================================ ListView ====================================================

int ListView::RowH(const PaintCtx& c) const    { return S(c.layout == Layout::Console ? 22 : 30, c.dpi); }
int ListView::HeaderH(const PaintCtx& c) const { return S(c.layout == Layout::Console ? 24 : 32, c.dpi); }

RECT ListView::BodyRect(const RECT& rc, const PaintCtx& c) const {
    RECT b = rc;
    b.top += HeaderH(c);
    return b;
}

int ListView::BodyRows(const RECT& rc, const PaintCtx& c) const {
    RECT b = BodyRect(rc, c);
    return std::max(0, (int)((b.bottom - b.top) / RowH(c)));
}

void ListView::SetRows(std::vector<std::vector<std::wstring>> rows) {
    rows_ = std::move(rows);
    if (sel_ >= (int)rows_.size()) sel_ = rows_.empty() ? -1 : (int)rows_.size() - 1;
}

void ListView::Select(int i) {
    sel_ = rows_.empty() ? -1 : std::clamp(i, 0, (int)rows_.size() - 1);
}

void ListView::EnsureVisible(const RECT& rc, const PaintCtx& c) {
    if (sel_ < 0) return;
    int rowH = RowH(c);
    RECT body = BodyRect(rc, c);
    int viewH = body.bottom - body.top;
    sb_.Configure(viewH, (int)rows_.size() * rowH);
    int top = sel_ * rowH, bot = top + rowH;
    if (top < sb_.Offset()) sb_.SetOffset(top);
    else if (bot > sb_.Offset() + viewH) sb_.SetOffset(bot - viewH);
}

void ListView::Paint(HDC dc, const RECT& rc, const PaintCtx& c, bool focused) {
    Pal p{c.theme};
    int rowH = RowH(c), headerH = HeaderH(c);
    bool hasSb = (int)rows_.size() * rowH > (rc.bottom - rc.top - headerH);
    int sbW = hasSb ? S(10, c.dpi) : 0;
    RECT body = BodyRect(rc, c);
    body.right -= sbW;
    int viewH = body.bottom - body.top;
    sb_.Configure(viewH, (int)rows_.size() * rowH);

    // Background.
    FillSolid(dc, rc, p.panel());

    // Header row.
    RECT hdr{rc.left, rc.top, rc.right - sbW, rc.top + headerH};
    FillSolid(dc, hdr, p.panel2());
    HLine(dc, rc.left, rc.right, hdr.bottom - 1, p.border());
    int x = rc.left;
    int pad = S(10, c.dpi);
    for (int i = 0; i < (int)cols_.size(); i++) {
        int w = S(cols_[i].width, c.dpi);
        RECT cell{x + pad, hdr.top, x + w - pad, hdr.bottom};
        bool sorted = (i == sortCol_);
        COLORREF hc = (i == hotHeader_ || sorted) ? p.text1() : p.text2();
        if (cols_[i].rightAlign) TextRight(dc, cols_[i].title, cell, c.fonts.semi, hc);
        else                     TextLeft(dc, cols_[i].title, cell, c.fonts.semi, hc);
        if (sorted) {
            // Sort triangle at the inner edge.
            int ax = cols_[i].rightAlign ? x + pad + S(4, c.dpi) : x + w - pad - S(9, c.dpi);
            int ay = hdr.top + (headerH - S(6, c.dpi)) / 2;
            int s = S(8, c.dpi), h = S(5, c.dpi);
            POINT tri[3];
            if (sortAsc_) { tri[0] = {ax, ay + h}; tri[1] = {ax + s, ay + h}; tri[2] = {ax + s / 2, ay}; }
            else          { tri[0] = {ax, ay};     tri[1] = {ax + s, ay};     tri[2] = {ax + s / 2, ay + h}; }
            HBRUSH b = CreateSolidBrush(p.accent()); HBRUSH ob = (HBRUSH)SelectObject(dc, b);
            HPEN pen = (HPEN)GetStockObject(NULL_PEN); HPEN op = (HPEN)SelectObject(dc, pen);
            Polygon(dc, tri, 3);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
        }
        if (i > 0) VLine(dc, x, hdr.top + S(6, c.dpi), hdr.bottom - S(6, c.dpi), p.border());
        x += w;
    }

    // Body rows, clipped to the body rect.
    HRGN clip = CreateRectRgn(body.left, body.top, body.right, body.bottom);
    SelectClipRgn(dc, clip);
    int first = sb_.Offset() / rowH;
    int y0 = body.top - (sb_.Offset() % rowH);
    for (int i = first; i < (int)rows_.size(); i++) {
        int ry = y0 + (i - first) * rowH;
        if (ry >= body.bottom) break;
        RECT row{rc.left, ry, rc.right - sbW, ry + rowH};
        bool selected = (i == sel_);
        if (selected) {
            RECT sel = row; sel.left += S(2, c.dpi); sel.right -= S(2, c.dpi);
            RoundFill(dc, sel, S(5, c.dpi), p.selBg());
        } else if (i % 2) {
            FillSolid(dc, row, p.panel2());
        }
        int cx = rc.left;
        for (int ci = 0; ci < (int)cols_.size() && ci < (int)rows_[i].size(); ci++) {
            int w = S(cols_[ci].width, c.dpi);
            RECT cell{cx + pad, row.top, cx + w - pad, row.bottom};
            COLORREF tc = selected ? p.selText() : (ci == 0 ? p.text1() : p.text2());
            HFONT font = (c.layout == Layout::Console) ? c.fonts.mono
                                                       : (ci == 0 ? c.fonts.ui : c.fonts.caption);
            if (cols_[ci].rightAlign) TextRight(dc, rows_[i][ci], cell, font, tc);
            else                      TextLeft(dc, rows_[i][ci], cell, font, tc);
            cx += w;
        }
        if (selected && focused) {
            RECT fr{row.left + S(2, c.dpi), row.top, row.right - S(2, c.dpi), row.bottom};
            FocusRectC(dc, fr, p.accent());
        }
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // Scrollbar.
    if (hasSb) {
        RECT track{rc.right - sbW, body.top, rc.right, body.bottom};
        sb_.Paint(dc, track, c);
    }

    // Outer border.
    FrameRectC(dc, rc, p.border());
}

bool ListView::OnWheel(int delta, const RECT& rc, const PaintCtx& c) {
    RECT body = BodyRect(rc, c);
    sb_.Configure(body.bottom - body.top, (int)rows_.size() * RowH(c));
    int before = sb_.Offset();
    sb_.SetOffset(before - (delta / WHEEL_DELTA) * 3 * RowH(c));
    return sb_.Offset() != before;
}

bool ListView::OnMouse(UINT msg, int x, int y, const RECT& rc, const PaintCtx& c) {
    int rowH = RowH(c), headerH = HeaderH(c);
    bool hasSb = (int)rows_.size() * rowH > (rc.bottom - rc.top - headerH);
    int sbW = hasSb ? S(10, c.dpi) : 0;

    // Header hover + click to sort.
    if (y >= rc.top && y < rc.top + headerH && x >= rc.left && x < rc.right - sbW) {
        int cx = rc.left, hit = -1;
        for (int i = 0; i < (int)cols_.size(); i++) {
            int w = S(cols_[i].width, c.dpi);
            if (x >= cx && x < cx + w) { hit = i; break; }
            cx += w;
        }
        if (msg == WM_MOUSEMOVE) { if (hit != hotHeader_) { hotHeader_ = hit; return true; } return false; }
        if (msg == WM_LBUTTONDOWN && hit >= 0) {
            if (hit == sortCol_) sortAsc_ = !sortAsc_;
            else { sortCol_ = hit; sortAsc_ = true; }
            if (onSortChanged) onSortChanged(sortCol_, sortAsc_);
            return true;
        }
        return false;
    }
    if (hotHeader_ != -1 && msg == WM_MOUSEMOVE) { hotHeader_ = -1; return true; }

    // Scrollbar.
    if (hasSb && x >= rc.right - sbW) {
        RECT track{rc.right - sbW, rc.top + headerH, rc.right, rc.bottom};
        return sb_.OnMouse(msg, x, y, track);
    }
    if (sb_.Dragging()) {   // keep dragging even when the cursor leaves the bar
        RECT track{rc.right - sbW, rc.top + headerH, rc.right, rc.bottom};
        return sb_.OnMouse(msg, x, y, track);
    }

    // Body: select / activate.
    RECT body = BodyRect(rc, c); body.right -= sbW;
    if (msg == WM_LBUTTONDOWN && y >= body.top && y < body.bottom && x >= body.left && x < body.right) {
        int idx = (y - body.top + sb_.Offset()) / rowH;
        if (idx >= 0 && idx < (int)rows_.size()) {
            sel_ = idx;
            DWORD now = GetTickCount();
            if (lastClickRow_ == idx && now - lastClickMs_ < GetDoubleClickTime()) {
                if (onActivate) onActivate(idx);
            }
            lastClickMs_ = now; lastClickRow_ = idx;
            return true;
        }
    }
    return false;
}

bool ListView::OnKey(WPARAM vk, const RECT& rc, const PaintCtx& c) {
    if (rows_.empty()) return false;
    int n = (int)rows_.size();
    int page = std::max(1, BodyRows(rc, c) - 1);
    int before = sel_;
    switch (vk) {
        case VK_UP:    sel_ = sel_ <= 0 ? 0 : sel_ - 1; break;
        case VK_DOWN:  sel_ = sel_ < 0 ? 0 : std::min(sel_ + 1, n - 1); break;
        case VK_PRIOR: sel_ = std::max((sel_ < 0 ? 0 : sel_) - page, 0); break;
        case VK_NEXT:  sel_ = std::min((sel_ < 0 ? 0 : sel_) + page, n - 1); break;
        case VK_HOME:  sel_ = 0; break;
        case VK_END:   sel_ = n - 1; break;
        case VK_RETURN: if (sel_ >= 0 && onActivate) onActivate(sel_); return true;
        default: return false;
    }
    EnsureVisible(rc, c);
    return sel_ != before || true;   // always repaint (scroll may have moved)
}

// ================================ SearchBox ===================================================

void SearchBox::Paint(HDC dc, const RECT& rc, const PaintCtx& c, bool focused) {
    Pal p{c.theme};
    RoundFill(dc, rc, S(6, c.dpi), p.panel2(), focused ? p.accent() : p.border());
    int pad = S(10, c.dpi);
    RECT tr{rc.left + pad, rc.top, rc.right - pad, rc.bottom};
    if (text_.empty()) {
        TextLeft(dc, L"Search…", tr, c.fonts.ui, p.text3());
    } else {
        TextLeft(dc, text_, tr, c.fonts.ui, p.text1());
    }
    if (focused) {
        // Caret after the text (approximate: measure the string width).
        SelectObject(dc, c.fonts.ui);
        SIZE sz{0, 0};
        std::wstring upto = text_.substr(0, caret_);
        GetTextExtentPoint32W(dc, upto.c_str(), (int)upto.size(), &sz);
        int cx = rc.left + pad + sz.cx;
        VLine(dc, cx, rc.top + S(6, c.dpi), rc.bottom - S(6, c.dpi), p.accent());
    }
}

bool SearchBox::OnChar(wchar_t ch) {
    if (ch == 0x08) {   // backspace
        if (caret_ > 0) { text_.erase(caret_ - 1, 1); caret_--; if (onChanged) onChanged(text_); return true; }
        return false;
    }
    if (ch < 0x20) return false;   // ignore other control chars
    text_.insert(text_.begin() + caret_, ch); caret_++;
    if (onChanged) onChanged(text_);
    return true;
}

bool SearchBox::OnKey(WPARAM vk) {
    switch (vk) {
        case VK_DELETE: if (caret_ < text_.size()) { text_.erase(caret_, 1); if (onChanged) onChanged(text_); return true; } return false;
        case VK_LEFT:   if (caret_ > 0) { caret_--; return true; } return false;
        case VK_RIGHT:  if (caret_ < text_.size()) { caret_++; return true; } return false;
        case VK_HOME:   if (caret_ != 0) { caret_ = 0; return true; } return false;
        case VK_END:    if (caret_ != text_.size()) { caret_ = text_.size(); return true; } return false;
    }
    return false;
}

// ================================ DetailPane ==================================================

void DetailPane::Paint(HDC dc, const RECT& rc, const PaintCtx& c) {
    Pal p{c.theme};
    FillSolid(dc, rc, p.panel());
    int pad = S(14, c.dpi);
    int rowH = S(c.layout == Layout::Console ? 20 : 24, c.dpi);
    int titleH = title_.empty() ? 0 : S(40, c.dpi);

    RECT inner{rc.left + pad, rc.top + titleH, rc.right - pad - S(10, c.dpi), rc.bottom - pad};
    int viewH = inner.bottom - inner.top;
    int contentH = (int)rows_.size() * rowH;
    sb_.Configure(viewH, contentH);
    bool hasSb = sb_.Visible();

    if (!title_.empty()) {
        RECT tr{rc.left + pad, rc.top, rc.right - pad, rc.top + titleH};
        TextLeft(dc, title_, tr, c.fonts.title, p.text1());
        HLine(dc, rc.left + pad, rc.right - pad, rc.top + titleH - 1, p.border());
    }

    HRGN clip = CreateRectRgn(inner.left, inner.top, rc.right - pad, inner.bottom);
    SelectClipRgn(dc, clip);
    int keyW = S(c.layout == Layout::Console ? 150 : 170, c.dpi);
    for (int i = 0; i < (int)rows_.size(); i++) {
        int ry = inner.top + i * rowH - sb_.Offset();
        if (ry + rowH <= inner.top || ry >= inner.bottom) continue;
        RECT kr{inner.left, ry, inner.left + keyW, ry + rowH};
        RECT vr{inner.left + keyW, ry, inner.right, ry + rowH};
        TextLeft(dc, rows_[i].first, kr, c.fonts.caption, p.text3());
        TextLeft(dc, rows_[i].second, vr, c.layout == Layout::Console ? c.fonts.mono : c.fonts.ui, p.text1());
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    if (hasSb) {
        RECT track{rc.right - S(10, c.dpi), inner.top, rc.right, inner.bottom};
        sb_.Paint(dc, track, c);
    }
    FrameRectC(dc, rc, p.border());
}

bool DetailPane::OnWheel(int delta, const RECT& rc, const PaintCtx& c) {
    (void)rc;
    int before = sb_.Offset();
    sb_.SetOffset(before - (delta / WHEEL_DELTA) * S(48, c.dpi));
    return sb_.Offset() != before;
}

bool DetailPane::OnMouse(UINT msg, int x, int y, const RECT& rc, const PaintCtx& c) {
    int pad = S(14, c.dpi);
    int titleH = title_.empty() ? 0 : S(40, c.dpi);
    RECT track{rc.right - S(10, c.dpi), rc.top + titleH, rc.right, rc.bottom - pad};
    if (!sb_.Visible()) return false;
    if ((x >= track.left && x < track.right) || sb_.Dragging())
        return sb_.OnMouse(msg, x, y, track);
    return false;
}

}  // namespace hx::ui
