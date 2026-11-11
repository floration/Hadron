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

    void SetColumns(std::vector<Column> cols) { cols_ = std::move(cols); }
    void SetRows(std::vector<std::vector<std::wstring>> rows);
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
    ScrollBar sb_;
    int  sel_ = -1;
    int  sortCol_ = -1;
    bool sortAsc_ = true;
    int  hotHeader_ = -1;   // header column under the cursor, for hover feedback
    DWORD lastClickMs_ = 0;
    int  lastClickRow_ = -1;
};

// A single-line text box for filtering. Holds its own text and caret; the owner routes WM_CHAR and
// the editing keys to it and reads Text() in onChanged.
class SearchBox {
public:
    const std::wstring& Text() const { return text_; }
    void SetText(std::wstring s) { text_ = std::move(s); caret_ = text_.size(); }
    std::function<void(const std::wstring&)> onChanged;

    void Paint(HDC dc, const RECT& rc, const PaintCtx& c, bool focused);
    bool OnChar(wchar_t ch);        // printable char or backspace (0x08)
    bool OnKey(WPARAM vk);          // Delete / Home / End / arrows

private:
    std::wstring text_;
    size_t caret_ = 0;
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
