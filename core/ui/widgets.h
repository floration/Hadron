// core/ui/widgets.h — the shared dark, custom-drawn widgets every module builds its view from.
//
// Widgets are plain objects, not HWNDs: the module owns them, paints them from its Module::Paint
// (so WM_PAINT and --screenshot render identically), and forwards WM_MOUSE*/WM_KEY*/WM_MOUSEWHEEL
// to them. An input method returns true when it changed state and the module should repaint. Every
// list scrolls with the wheel and a draggable scrollbar, sorts by clicking a header, is keyboard-
// navigable with a focus rect, and falls back to system colours in high contrast (CLAUDE.md "UI").
#pragma once

#include "core/ui/theme.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hx::ui {

// Everything a widget needs to size and colour itself. Passed by const ref to Paint and hit-testing.
struct PaintCtx {
    const Theme& theme;
    const Fonts& fonts;
    UINT         dpi;
    Layout       layout;
};

// Vertical, draggable scrollbar. The owner gives it the viewport and content heights; it reports an
// offset in pixels. Reusable by the list, the detail pane and any scrolling view.
class ScrollBar {
public:
    void Configure(int viewportPx, int contentPx);
    void SetOffset(int px);
    int  Offset() const { return offset_; }
    int  MaxOffset() const { return content_ > viewport_ ? content_ - viewport_ : 0; }
    bool Visible() const { return content_ > viewport_; }
    bool Dragging() const { return dragging_; }
    void CancelDrag() { dragging_ = false; }   // call on WM_CAPTURECHANGED

    void Paint(HDC dc, const RECT& track, const PaintCtx& c);
    // msg: WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP. Returns true if the offset changed.
    bool OnMouse(UINT msg, int x, int y, const RECT& track);

private:
    RECT ThumbRect(const RECT& track) const;
    int  viewport_ = 0, content_ = 0, offset_ = 0;
    bool dragging_ = false;
    int  dragStartY_ = 0, dragStartOffset_ = 0;
};

// A columned, sortable, scrollable, keyboard-navigable list. The list tracks the sort column and
// direction and the selection; it does not sort the data itself — it calls onSortChanged and the
// owner re-sorts its model and calls SetRows again. Cells are plain strings (owner formats them).
class ListView {
public:
    struct Column { std::wstring title; int width; bool rightAlign = false; };
    // Per-row text colour from the theme's status colours (Normal = the usual text1/text2). The selected
    // row always uses the selection text colour, and high contrast always uses the system text colour,
    // so ink never costs legibility. Never the only signal: say the status in the cell text too.
    enum class Ink : unsigned char { Normal, Warn, Bad, Good };

    // Row and header heights per layout, so modules that lay out around a list use the same numbers.
    static int RowHeight(Layout l, UINT dpi);
    static int HeaderHeight(Layout l, UINT dpi);

    void SetColumns(std::vector<Column> cols) { cols_ = std::move(cols); }
    void SetRows(std::vector<std::vector<std::wstring>> rows);
    // Optional, parallel to the rows of the last SetRows; missing entries are Normal. SetRows clears it,
    // so call SetRowInk after every SetRows (re-sort included).
    void SetRowInk(std::vector<Ink> ink) { ink_ = std::move(ink); }
    int  RowCount() const { return (int)rows_.size(); }

    int  Selected() const { return sel_; }
    void Select(int i);

    int  SortColumn() const { return sortCol_; }
    bool SortAscending() const { return sortAsc_; }

    std::function<void(int column, bool ascending)> onSortChanged;
    std::function<void(int row)> onActivate;   // Enter or double-click

    void Paint(HDC dc, const RECT& rc, const PaintCtx& c, bool focused);
    bool OnMouse(UINT msg, int x, int y, const RECT& rc, const PaintCtx& c);
    bool OnWheel(int delta, const RECT& rc, const PaintCtx& c);
    bool OnKey(WPARAM vk, const RECT& rc, const PaintCtx& c);

private:
    int  RowH(const PaintCtx& c) const;
    int  HeaderH(const PaintCtx& c) const;
    int  BodyRows(const RECT& rc, const PaintCtx& c) const;   // fully visible rows
    void EnsureVisible(const RECT& rc, const PaintCtx& c);
    RECT BodyRect(const RECT& rc, const PaintCtx& c) const;

    std::vector<Column> cols_;
    std::vector<std::vector<std::wstring>> rows_;
    std::vector<Ink> ink_;
    ScrollBar sb_;
    int  sel_ = -1;
    int  sortCol_ = -1;
    bool sortAsc_ = true;
    int  hotHeader_ = -1;   // header column under the cursor, for hover feedback
    DWORD lastClickMs_ = 0;
    int  lastClickRow_ = -1;
};

// A single-line editable text field: caret, selection (mouse drag, double-click selects a word,
// Shift+arrows/Home/End, Ctrl+A), clipboard (Ctrl+C/X/V, Shift+Insert/Delete, Ctrl+Insert, and a
// right-click Cut/Copy/Paste/Select all menu), Ctrl+Left/Right by word, Ctrl+Backspace/Delete, one
// level of Ctrl+Z, and horizontal scrolling so long text stays editable. The owner routes WM_CHAR,
// WM_KEYDOWN and the mouse messages to it (with the field's rect) and reads Text().
class TextField {
public:
    const std::wstring& Text() const { return text_; }
    void SetText(std::wstring s);          // caret to the end, no selection; does not fire onChanged
    void SelectAll() { anchor_ = 0; caret_ = text_.size(); }
    bool HasSelection() const { return anchor_ != caret_; }
    std::wstring placeholder;
    std::function<void(const std::wstring&)> onChanged;

    void Paint(HDC dc, const RECT& rc, const PaintCtx& c, bool focused);
    bool OnChar(wchar_t ch);                     // printable char or backspace (0x08)
    bool OnKey(WPARAM vk, HWND owner = nullptr); // editing/navigation keys and Ctrl/Shift shortcuts
    // WM_LBUTTONDOWN / WM_LBUTTONDBLCLK / WM_MOUSEMOVE / WM_LBUTTONUP / WM_RBUTTONUP, in the same
    // coordinates as rc. Takes mouse capture while dragging. WM_RBUTTONUP shows the context menu.
    bool OnMouse(HWND owner, UINT msg, int x, int y, const RECT& rc, const PaintCtx& c);
    bool Dragging() const { return dragging_; }
    void CancelDrag() { dragging_ = false; }   // call on WM_CAPTURECHANGED

private:
    size_t SelLo() const { return anchor_ < caret_ ? anchor_ : caret_; }
    size_t SelHi() const { return anchor_ < caret_ ? caret_ : anchor_; }
    void   Snapshot();                             // save undo state before an edit
    void   ReplaceSel(const std::wstring& s);      // insert s over the selection
    bool   DeleteSel();
    void   Changed() { if (onChanged) onChanged(text_); }
    bool   Copy(HWND owner) const;
    bool   Paste(HWND owner);
    size_t WordLeft(size_t i) const;
    size_t WordRight(size_t i) const;
    size_t HitIndex(HWND owner, int x, const RECT& rc, const PaintCtx& c) const;

    std::wstring text_;
    size_t caret_ = 0, anchor_ = 0;
    int    scrollX_ = 0;        // pixels of text scrolled off the left edge
    bool   dragging_ = false;
    std::wstring undoText_;
    size_t undoCaret_ = 0;
    bool   hasUndo_ = false;
};

// The search box every list uses: a TextField with the "Search…" placeholder.
class SearchBox : public TextField {
public:
    SearchBox() { placeholder = L"Search\u2026"; }
};

// A scrollable key/value pane for the selected item's details.
class DetailPane {
public:
    void SetTitle(std::wstring s) { title_ = std::move(s); }
    void SetRows(std::vector<std::pair<std::wstring, std::wstring>> rows) { rows_ = std::move(rows); }
    void Clear() { title_.clear(); rows_.clear(); sb_.SetOffset(0); }

    void Paint(HDC dc, const RECT& rc, const PaintCtx& c);
    bool OnWheel(int delta, const RECT& rc, const PaintCtx& c);
    bool OnMouse(UINT msg, int x, int y, const RECT& rc, const PaintCtx& c);

private:
    std::wstring title_;
    std::vector<std::pair<std::wstring, std::wstring>> rows_;
    ScrollBar sb_;
};

}  // namespace hx::ui
