// modules/uikit/module.cpp — the core/ui gallery. It is a reference and a screenshot target, not a
// shipping tool, so it is not in the suite catalogue; it builds as HadronUikit.exe and renders the
// shared widgets in whatever --theme/--layout the host resolves. It proves the list (sortable,
// scrollable, keyboard-navigable), the search box, the detail pane, and the accent derivation.
#include "core/host/run.h"
#include "core/host/registry.h"
#include "core/ui/widgets.h"
#include "core/ui/draw.h"
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace hx {
using namespace hx::ui;

namespace {

struct Item { std::wstring name, kind; long long size; std::wstring modified; };

class UikitModule : public Module {
public:
    const ModuleInfo& Info() const override {
        static const ModuleInfo info{L"uikit", L"UI Kit", L"core/ui widget gallery", Admin::Required};
        return info;
    }

    HWND Create(IHost& host, HWND parent, const RECT& rc) override {
        host_ = &host;
        BuildData();
        Resort();
        list_.Select(3);
        SyncDetail();
        list_.onSortChanged = [this](int, bool) { Resort(); SyncDetail(); };
        list_.onActivate    = [this](int) { SyncDetail(); };
        host.SetStatus(L"core/ui gallery");
        hwnd_ = CreateModuleContent(host, parent, rc, this);
        return hwnd_;
    }

    // Deterministic screenshot state: a fixed selection and a sort, independent of any file.
    void LoadFixture(const wchar_t*) override {
        list_.Select(3);
        SyncDetail();
    }

    void Paint(HDC dc, const RECT& rc) override {
        PaintCtx c{host_->Theme(), host_->Fonts(), host_->Dpi(), host_->Layout()};
        const Theme& t = c.theme;
        UINT dpi = c.dpi;
        FillSolid(dc, rc, t.bg);

        int pad = S(16, dpi);
        // --- header: title, subtitle, accent swatches ---
        RECT title{rc.left + pad, rc.top + S(12, dpi), rc.right - pad, rc.top + S(44, dpi)};
        TextLeft(dc, L"UI Kit", title, c.fonts.title, t.text1);
        RECT sub{rc.left + pad, rc.top + S(46, dpi), rc.left + S(360, dpi), rc.top + S(66, dpi)};
        std::wstring subtitle = std::wstring(LayoutName(c.layout)) + L" layout  \u00B7  "
                              + ThemeName(t.id) + L" theme";
        TextLeft(dc, subtitle, sub, c.fonts.ui, t.text2);

        const COLORREF acc[4] = {t.accent, t.accent2, t.accent3, t.accent4};
        const wchar_t* lbl[4] = {L"Accent", L"+90", L"+180", L"+270"};
        int sw = S(54, dpi), sh = S(30, dpi), sx = rc.right - pad - 4 * (sw + S(8, dpi));
        for (int i = 0; i < 4; i++) {
            RECT r{sx + i * (sw + S(8, dpi)), rc.top + S(14, dpi), 0, 0};
            r.right = r.left + sw; r.bottom = r.top + sh;
            RoundFill(dc, r, S(6, dpi), acc[i], t.border);
            RECT lr{r.left, r.bottom + S(2, dpi), r.right, r.bottom + S(18, dpi)};
            DrawText(dc, lbl[i], lr, c.fonts.caption, t.text3, DT_SINGLELINE | DT_CENTER | DT_TOP);
        }

        // --- body: search + list on the left, detail on the right ---
        int top = rc.top + S(80, dpi);
        int rightW = S(360, dpi);
        int gap = S(14, dpi);
        RECT left{rc.left + pad, top, rc.right - pad - rightW - gap, rc.bottom - pad};
        RECT right{left.right + gap, top, rc.right - pad, rc.bottom - pad};

        int searchH = S(34, dpi);
        RECT searchR{left.left, left.top, left.right, left.top + searchH};
        search_.Paint(dc, searchR, c, focus_ == Focus::Search);

        RECT listR{left.left, searchR.bottom + S(10, dpi), left.right, left.bottom};
        listRect_ = listR;
        list_.Paint(dc, listR, c, focus_ == Focus::List);

        detailRect_ = right;
        detail_.Paint(dc, right, c);
    }

    bool OnInput(UINT msg, WPARAM w, LPARAM l) override {
        PaintCtx c{host_->Theme(), host_->Fonts(), host_->Dpi(), host_->Layout()};
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        switch (msg) {
            case WM_LBUTTONDOWN: {
                POINT p{x, y};
                if (PtInRect(&listRect_, p))        focus_ = Focus::List;
                else if (PtInRect(&detailRect_, p)) focus_ = Focus::Detail;
                else                                focus_ = Focus::Search;
                list_.OnMouse(msg, x, y, listRect_, c);
                detail_.OnMouse(msg, x, y, detailRect_, c);
                SyncDetail();
                return true;
            }
            case WM_LBUTTONUP:
            case WM_MOUSEMOVE: {
                bool ch = list_.OnMouse(msg, x, y, listRect_, c);
                ch |= detail_.OnMouse(msg, x, y, detailRect_, c);
                return ch;
            }
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(w);
                POINT p; GetCursorPos(&p); ScreenToClient(hwnd_, &p);
                if (PtInRect(&detailRect_, p)) return detail_.OnWheel(delta, detailRect_, c);
                return list_.OnWheel(delta, listRect_, c);
            }
            case WM_KEYDOWN:
                if (focus_ == Focus::List) {
                    bool ch = list_.OnKey(w, listRect_, c);
                    SyncDetail();
                    return ch;
                }
                if (focus_ == Focus::Search) return search_.OnKey(w);
                return false;
            case WM_CHAR:
                if (focus_ == Focus::Search) return search_.OnChar((wchar_t)w);
                return false;
        }
        return false;
    }

    int RunCli(int argc, wchar_t** argv) override {
        for (int i = 0; i < argc; i++) {
            if (std::wstring(argv[i]) == L"--selftest") {
                BuildData(); Resort();
                bool ok = list_.RowCount() == (int)data_.size() && RotateHue(RGB(96,160,255), 180) != RGB(96,160,255);
                wprintf(L"uikit selftest: %ls\n", ok ? L"PASS" : L"FAIL");
                return ok ? 0 : 1;
            }
        }
        return -1;
    }

private:
    enum class Focus { Search, List, Detail };

    void BuildData() {
        const wchar_t* kinds[] = {L"Folder", L"Document", L"Image", L"Archive", L"Binary"};
        const wchar_t* names[] = {
            L"core", L"modules", L"docs", L"tests", L"resources", L"README.md", L"CMakeLists.txt",
            L"host.cpp", L"theme.cpp", L"widgets.cpp", L"png.cpp", L"worker.h", L"settings.ini",
            L"hadron.ico", L"snapshot.bin", L"baseline.png", L"console.png", L"legacy.zip",
            L"diskmap.md", L"bios.md", L"notes.txt", L"plan.md", L"accent.json", L"fixtures",
            L"golden", L"build", L"cmake", L"app", L"sys", L"ui", L"run.h", L"module.h",
        };
        data_.clear();
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            Item it;
            it.name = names[i];
            it.kind = kinds[i % 5];
            it.size = (long long)((i * 8117 + 613) % 900000);
            wchar_t m[32];
            swprintf(m, 32, L"2026-11-%02d %02d:%02d", (int)(i % 28) + 1, (int)(i % 24), (int)(i * 7 % 60));
            it.modified = m;
            data_.push_back(it);
        }
    }

    void Resort() {
        int col = list_.SortColumn();
        bool asc = list_.SortAscending();
        if (col >= 0) {
            std::stable_sort(data_.begin(), data_.end(), [&](const Item& a, const Item& b) {
                int r = 0;
                switch (col) {
                    case 0: r = a.name.compare(b.name); break;
                    case 1: r = a.kind.compare(b.kind); break;
                    case 2: r = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0; break;
                    default: r = a.modified.compare(b.modified); break;
                }
                return asc ? r < 0 : r > 0;
            });
        }
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(data_.size());
        for (const auto& it : data_) {
            wchar_t sz[32]; swprintf(sz, 32, L"%lld B", it.size);
            rows.push_back({it.name, it.kind, sz, it.modified});
        }
        list_.SetColumns({{L"Name", 230}, {L"Type", 110}, {L"Size", 110, true}, {L"Modified", 170}});
        list_.SetRows(std::move(rows));
    }

    void SyncDetail() {
        int i = list_.Selected();
        if (i < 0 || i >= (int)data_.size()) { detail_.Clear(); return; }
        const Item& it = data_[i];
        wchar_t sz[32]; swprintf(sz, 32, L"%lld bytes", it.size);
        detail_.SetTitle(it.name);
        detail_.SetRows({
            {L"Type", it.kind},
            {L"Size", sz},
            {L"Modified", it.modified},
            {L"Basis", L"reasoning"},
            {L"Path", L"\\\\?\\C:\\Hadron\\" + it.name},
            {L"Note", L"Selected row, shown in the detail pane."},
        });
    }

    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;
    std::vector<Item> data_;
    ListView list_;
    SearchBox search_;
    DetailPane detail_;
    Focus focus_ = Focus::List;
    RECT listRect_{}, detailRect_{};
};

}  // namespace

std::unique_ptr<Module> MakeUikitModule() { return std::make_unique<UikitModule>(); }

}  // namespace hx
