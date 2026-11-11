// core/ui/widgets.cpp — implementations. All drawing goes through core/ui/draw.h (self-freeing GDI).
#include "core/ui/widgets.h"
#include "core/ui/draw.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

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
        // Whichever of near-black and near-white has the higher WCAG contrast with the fill. The better of
        // the two is never below about 4.5:1 for any fill; a Rec. 601 brightness threshold picked white on a
        // bright green accent (about 2:1).
        auto lin = [](int v) { double c = v / 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        auto lum = [&](COLORREF c) { return 0.2126 * lin(GetRValue(c)) + 0.7152 * lin(GetGValue(c)) + 0.0722 * lin(GetBValue(c)); };
        const COLORREF dark = RGB(16, 18, 22), light = RGB(245, 247, 250);
        double lf = lum(t.accent);
        double onDark = (lf + 0.05) / (lum(dark) + 0.05), onLight = (lum(light) + 0.05) / (lf + 0.05);
        return onDark > onLight ? dark : light;
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
        // The sort triangle gets its own space at the inner edge: the title ellipsises before it instead of
        // running under it on a narrow column.
        const int arrowW = S(8, c.dpi) + S(6, c.dpi);
        if (sorted) {
            if (cols_[i].rightAlign) cell.left = std::min(cell.right, cell.left + arrowW);
            else                     cell.right = std::max(cell.left, cell.right - arrowW);
        }
        COLORREF hc = (i == hotHeader_ || sorted) ? p.text1() : p.text2();
        if (cols_[i].rightAlign) TextRight(dc, cols_[i].title, cell, c.fonts.semi, hc);
        else                     TextLeft(dc, cols_[i].title, cell, c.fonts.semi, hc);
        if (sorted) {
            // Sort triangle at the inner edge.
            int ax = cols_[i].rightAlign ? x + pad : x + w - pad - S(8, c.dpi);
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

    // Body rows, clipped to the body rect. IntersectClipRect takes logical coordinates, so the clip
    // follows the host's SetViewportOrgEx offset in the suite (a region from CreateRectRgn is in
    // device units and lands off by the content origin). SaveDC/RestoreDC keeps any outer clip.
    const int savedClip = SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
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
            // One cell size per list (Segoe UI 9pt, the Windows body size): a larger first column cost about a
            // fifth of its width and left two type sizes on one baseline. text1 vs text2 keeps it the anchor.
            HFONT font = (c.layout == Layout::Console) ? c.fonts.mono : c.fonts.caption;
            if (cols_[ci].rightAlign) TextRight(dc, rows_[i][ci], cell, font, tc);
            else                      TextLeft(dc, rows_[i][ci], cell, font, tc);
            cx += w;
        }
        if (selected && focused) {
            // The focus row is always the selected row, which is filled with the accent: an accent outline
            // there is invisible. Draw it in the selection text colour (the best of black/white on the
            // fill, or the system highlight text in high contrast), inset inside the rounded fill.
            const int in = S(3, c.dpi);
            RECT fr{row.left + S(2, c.dpi) + in, row.top + in, row.right - S(2, c.dpi) - in, row.bottom - in};
            FocusRectC(dc, fr, p.selText());
        }
    }
    RestoreDC(dc, savedClip);

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

void TextField::SetText(std::wstring s) {
    text_ = std::move(s);
    caret_ = anchor_ = text_.size();
    scrollX_ = 0;
    hasUndo_ = false;
}

namespace {
int TextWidth(HDC dc, const std::wstring& s, size_t n) {
    if (n == 0) return 0;
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, s.c_str(), (int)n, &sz);
    return sz.cx;
}
// AltGr reports Ctrl+Alt; it types characters, so it is not a Ctrl shortcut.
bool CtrlDown() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0; }
bool ShiftDown() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }
bool IsWordChar(wchar_t ch) { return iswalnum(ch) || ch == L'_'; }
}  // namespace

void TextField::Paint(HDC dc, const RECT& rc, const PaintCtx& c, bool focused) {
    Pal p{c.theme};
    RoundFill(dc, rc, S(6, c.dpi), p.panel2(), focused ? p.accent() : p.border());
    const int pad = S(10, c.dpi);
    RECT tr{rc.left + pad, rc.top, rc.right - pad, rc.bottom};
    if (text_.empty()) {
        TextLeft(dc, placeholder, tr, c.fonts.ui, p.text3());
        if (focused) VLine(dc, tr.left, rc.top + S(6, c.dpi), rc.bottom - S(6, c.dpi), p.accent());
        scrollX_ = 0;
        return;
    }
    HGDIOBJ oldFont = SelectObject(dc, c.fonts.ui);
    const int viewW = tr.right - tr.left;
    const int full = TextWidth(dc, text_, text_.size());
    const int caretX = TextWidth(dc, text_, caret_);
    // Keep the caret in view; never scroll past the end of the text.
    if (caretX - scrollX_ > viewW - 1) scrollX_ = caretX - viewW + 1;
    if (caretX - scrollX_ < 0) scrollX_ = caretX;
    if (scrollX_ > full - viewW + 1) scrollX_ = full - viewW + 1;
    if (scrollX_ < 0) scrollX_ = 0;

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, tr.left, rc.top + 1, tr.right, rc.bottom - 1);
    const int x0 = tr.left - scrollX_;
    if (HasSelection() && focused) {
        int a = x0 + TextWidth(dc, text_, SelLo()), b = x0 + TextWidth(dc, text_, SelHi());
        RECT sr{a, rc.top + S(5, c.dpi), b, rc.bottom - S(5, c.dpi)};
        FillSolid(dc, sr, p.selBg());
    }
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int ty = rc.top + ((rc.bottom - rc.top) - tm.tmHeight) / 2;
    if (HasSelection() && focused) {
        // Unselected text, then the selected run over the highlight in the selection text colour.
        int a = x0 + TextWidth(dc, text_, SelLo()), b = x0 + TextWidth(dc, text_, SelHi());
        const int inner = SaveDC(dc);   // unselected text everywhere except the selection
        ExcludeClipRect(dc, a, rc.top, b, rc.bottom);
        SetTextColor(dc, p.text1());
        ExtTextOutW(dc, x0, ty, 0, nullptr, text_.c_str(), (UINT)text_.size(), nullptr);
        RestoreDC(dc, inner);
        SetTextColor(dc, p.selText());
        ExtTextOutW(dc, a, ty, 0, nullptr, text_.c_str() + SelLo(), (UINT)(SelHi() - SelLo()), nullptr);
    } else {
        SetTextColor(dc, p.text1());
        ExtTextOutW(dc, x0, ty, 0, nullptr, text_.c_str(), (UINT)text_.size(), nullptr);
    }
    if (focused) VLine(dc, x0 + caretX, rc.top + S(6, c.dpi), rc.bottom - S(6, c.dpi), p.accent());
    RestoreDC(dc, saved);
    SelectObject(dc, oldFont);
}

void TextField::Snapshot() {
    undoText_ = text_;
    undoCaret_ = caret_;
    hasUndo_ = true;
}

bool TextField::DeleteSel() {
    if (!HasSelection()) return false;
    size_t lo = SelLo();
    text_.erase(lo, SelHi() - lo);
    caret_ = anchor_ = lo;
    return true;
}

void TextField::ReplaceSel(const std::wstring& s) {
    DeleteSel();
    text_.insert(caret_, s);
    caret_ += s.size();
    anchor_ = caret_;
}

bool TextField::OnChar(wchar_t ch) {
    if (ch == 0x08) {   // backspace (Ctrl+Backspace arrives as 0x7F and is handled in OnKey)
        if (!HasSelection() && caret_ == 0) return false;   // nothing to delete: keep the undo state
        Snapshot();
        if (DeleteSel()) { Changed(); return true; }
        text_.erase(caret_ - 1, 1); caret_--; anchor_ = caret_; Changed();
        return true;
    }
    if (ch < 0x20 || ch == 0x7F) return false;   // control chars (Ctrl+letters) are handled in OnKey
    Snapshot();
    ReplaceSel(std::wstring(1, ch));
    Changed();
    return true;
}

size_t TextField::WordLeft(size_t i) const {
    while (i > 0 && !IsWordChar(text_[i - 1])) i--;
    while (i > 0 && IsWordChar(text_[i - 1])) i--;
    return i;
}

size_t TextField::WordRight(size_t i) const {
    size_t n = text_.size();
    while (i < n && IsWordChar(text_[i])) i++;
    while (i < n && !IsWordChar(text_[i])) i++;
    return i;
}

bool TextField::Copy(HWND owner) const {
    if (!HasSelection()) return false;
    std::wstring s = text_.substr(SelLo(), SelHi() - SelLo());
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        if (h) {
            void* mem = GlobalLock(h);
            if (mem) {
                memcpy(mem, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h)) ok = true;   // the clipboard owns h now
            }
            if (!ok) GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
}

bool TextField::Paste(HWND owner) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return false;
    std::wstring s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            size_t max = GlobalSize(h) / sizeof(wchar_t);
            size_t n = 0;
            while (n < max && p[n]) n++;
            s.assign(p, n);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    // Single-line field: a pasted line break becomes a space; trailing ones are dropped (text
    // copied from a terminal often ends with one). Capped at 32767 characters (the longest path),
    // so an accidental paste of a whole file can't stall hit-testing and painting.
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    std::wstring clean;
    clean.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t ch = s[i];
        if (ch == L'\r') { if (i + 1 < s.size() && s[i + 1] == L'\n') continue; ch = L' '; }
        if (ch == L'\n' || ch == L'\t') ch = L' ';
        if (ch < 0x20) continue;
        clean += ch;
    }
    const size_t kMaxLen = 32767;
    size_t room = kMaxLen > text_.size() - (SelHi() - SelLo()) ? kMaxLen - (text_.size() - (SelHi() - SelLo())) : 0;
    if (clean.size() > room) clean.resize(room);
    if (clean.empty() && !HasSelection()) return false;
    Snapshot();
    ReplaceSel(clean);
    Changed();
    return true;
}

bool TextField::OnKey(WPARAM vk, HWND owner) {
    const bool ctrl = CtrlDown(), shift = ShiftDown();
    auto move = [&](size_t to) {
        bool had = HasSelection();
        if (!shift && had) {
            // Plain Left/Right with a selection collapse it to that side.
            if (vk == VK_LEFT && to + 1 >= SelLo()) to = SelLo();
            else if (vk == VK_RIGHT && to <= SelHi() + 1) to = SelHi();
        }
        bool changed = to != caret_ || (!shift && had);
        caret_ = to;
        if (!shift) anchor_ = caret_;
        return changed;
    };
    switch (vk) {
    case VK_LEFT:  return move(ctrl ? WordLeft(caret_) : (caret_ > 0 ? caret_ - 1 : 0));
    case VK_RIGHT: return move(ctrl ? WordRight(caret_) : std::min(caret_ + 1, text_.size()));
    case VK_HOME:  return move(0);
    case VK_END:   return move(text_.size());
    case VK_DELETE:
        if (shift && !ctrl) { if (!Copy(owner)) return false; Snapshot(); DeleteSel(); Changed(); return true; }  // Shift+Del = cut
        if (!HasSelection() && caret_ >= text_.size()) return false;   // nothing to delete
        Snapshot();
        if (DeleteSel()) { Changed(); return true; }
        {
            size_t to = ctrl ? WordRight(caret_) : caret_ + 1;
            text_.erase(caret_, to - caret_);
            Changed();
            return true;
        }
    case VK_BACK:
        if (!ctrl) return false;   // plain backspace arrives as WM_CHAR 0x08
        if (!HasSelection() && caret_ == 0) return false;
        Snapshot();
        if (DeleteSel()) { Changed(); return true; }
        {
            size_t from = WordLeft(caret_);
            text_.erase(from, caret_ - from);
            caret_ = anchor_ = from;
            Changed();
            return true;
        }
    case VK_INSERT:
        if (ctrl && !shift) return Copy(owner), false;
        if (shift && !ctrl) return Paste(owner);
        return false;
    }
    if (!ctrl) return false;
    switch (vk) {
    case 'A': if (text_.empty() || (anchor_ == 0 && caret_ == text_.size())) return false; SelectAll(); return true;
    case 'C': Copy(owner); return false;
    case 'X': if (!Copy(owner)) return false; Snapshot(); DeleteSel(); Changed(); return true;
    case 'V': return Paste(owner);
    case 'Z':
        if (!hasUndo_) return false;
        std::swap(text_, undoText_);
        std::swap(caret_, undoCaret_);   // Ctrl+Z again redoes
        if (caret_ > text_.size()) caret_ = text_.size();
        anchor_ = caret_;
        Changed();
        return true;
    }
    return false;
}

size_t TextField::HitIndex(HWND owner, int x, const RECT& rc, const PaintCtx& c) const {
    if (text_.empty()) return 0;
    HDC dc = GetDC(owner);
    if (!dc) return caret_;
    HGDIOBJ old = SelectObject(dc, c.fonts.ui);
    // One call gives every prefix width (dx[i] = width of the first i+1 characters).
    std::vector<int> dx(text_.size(), 0);
    SIZE total{0, 0};
    // lpnFit null: nMaxExtent is ignored and alpDx is filled for every character.
    bool ok = GetTextExtentExPointW(dc, text_.c_str(), (int)text_.size(), 0, nullptr, dx.data(), &total) != FALSE;
    SelectObject(dc, old);
    ReleaseDC(owner, dc);
    if (!ok) return caret_;
    const int rel = x - (rc.left + S(10, c.dpi)) + scrollX_;
    // Index i sits at prefix width w(i) (w(0)=0, w(i)=dx[i-1]); pick the boundary nearest rel.
    size_t lo = 0, hi = text_.size();
    while (lo < hi) {   // first i with w(i) >= rel
        size_t mid = (lo + hi) / 2;
        int w = mid == 0 ? 0 : dx[mid - 1];
        if (w < rel) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return 0;
    int wl = lo - 1 == 0 ? 0 : dx[lo - 2], wr = dx[lo - 1];
    return (rel - wl) < (wr - rel) ? lo - 1 : lo;
}

bool TextField::OnMouse(HWND owner, UINT msg, int x, int y, const RECT& rc, const PaintCtx& c) {
    POINT pt{x, y};
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (!PtInRect(&rc, pt)) return false;
        size_t i = HitIndex(owner, x, rc, c);
        caret_ = i;
        if (!ShiftDown()) anchor_ = i;
        dragging_ = true;
        if (owner) SetCapture(owner);
        return true;
    }
    case WM_LBUTTONDBLCLK: {
        if (!PtInRect(&rc, pt)) return false;
        size_t i = HitIndex(owner, x, rc, c);
        size_t a = i, b = i;
        while (a > 0 && IsWordChar(text_[a - 1])) a--;
        while (b < text_.size() && IsWordChar(text_[b])) b++;
        if (a == b && b < text_.size()) b++;   // on punctuation: select that one character
        anchor_ = a; caret_ = b;
        return true;
    }
    case WM_MOUSEMOVE: {
        if (!dragging_) return false;
        size_t i = HitIndex(owner, x, rc, c);
        if (i == caret_) return false;
        caret_ = i;
        return true;
    }
    case WM_LBUTTONUP:
        if (!dragging_) return false;
        dragging_ = false;
        if (owner && GetCapture() == owner) ReleaseCapture();
        return true;
    case WM_RBUTTONUP: {
        if (!PtInRect(&rc, pt) || !owner) return false;
        HMENU m = CreatePopupMenu();
        if (!m) return false;
        const bool sel = HasSelection();
        const bool clip = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
        AppendMenuW(m, MF_STRING | (hasUndo_ ? 0 : MF_GRAYED), 5, L"Undo\tCtrl+Z");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), 1, L"Cut\tCtrl+X");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), 2, L"Copy\tCtrl+C");
        AppendMenuW(m, MF_STRING | (clip ? 0 : MF_GRAYED), 3, L"Paste\tCtrl+V");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), 6, L"Delete\tDel");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (text_.empty() ? MF_GRAYED : 0), 4, L"Select all\tCtrl+A");
        POINT sp{x, y};
        ClientToScreen(owner, &sp);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, sp.x, sp.y, 0, owner, nullptr);
        DestroyMenu(m);
        switch (cmd) {
        case 1: if (Copy(owner)) { Snapshot(); DeleteSel(); Changed(); } return true;
        case 2: Copy(owner); return true;
        case 3: Paste(owner); return true;
        case 4: SelectAll(); return true;
        case 5:
            if (hasUndo_) {
                std::swap(text_, undoText_);
                std::swap(caret_, undoCaret_);
                if (caret_ > text_.size()) caret_ = text_.size();
                anchor_ = caret_;
                Changed();
            }
            return true;
        case 6: Snapshot(); if (DeleteSel()) Changed(); return true;
        }
        return true;
    }
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

    // Logical-coordinate clip (see ListView::Paint): correct under the suite's viewport offset.
    const int savedClip = SaveDC(dc);
    IntersectClipRect(dc, inner.left, inner.top, rc.right - pad, inner.bottom);
    int keyW = S(c.layout == Layout::Console ? 150 : 170, c.dpi);
    for (int i = 0; i < (int)rows_.size(); i++) {
        int ry = inner.top + i * rowH - sb_.Offset();
        if (ry + rowH <= inner.top || ry >= inner.bottom) continue;
        RECT kr{inner.left, ry, inner.left + keyW, ry + rowH};
        RECT vr{inner.left + keyW, ry, inner.right, ry + rowH};
        TextLeft(dc, rows_[i].first, kr, c.fonts.caption, p.text3());
        TextLeft(dc, rows_[i].second, vr, c.layout == Layout::Console ? c.fonts.mono : c.fonts.ui, p.text1());
    }
    RestoreDC(dc, savedClip);

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
