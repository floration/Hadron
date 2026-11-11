// modules/hello/module.cpp — the trivial module that proves the contract works standalone and in
// the suite before any real port. It draws a card, reads/writes one setting, and has a --selftest.
#include "core/host/run.h"
#include "core/host/registry.h"
#include "core/sys/settings.h"
#include "core/ui/theme.h"
#include <cstdio>
#include <string>

namespace hx {
using namespace hx::ui;

namespace {

class HelloModule : public Module {
public:
    const ModuleInfo& Info() const override {
        static const ModuleInfo info{L"hello", L"Hello", L"Proves the module contract", Admin::Required};
        return info;
    }

    HWND Create(IHost& host, HWND parent, const RECT& rc) override {
        host_ = &host;
        fonts_.Build(host.Dpi());
        // One setting, to prove per-module Settings round-trips.
        int n = host.Prefs().GetInt(L"opened", 0) + 1;
        host.Prefs().SetInt(L"opened", n);
        opened_ = n;
        host.SetStatus(L"hello ready");
        hwnd_ = CreateModuleContent(host, parent, rc, this);
        return hwnd_;
    }

    void OnDpiChanged(UINT dpi) override { fonts_.Build(dpi); }

    void Paint(HDC dc, const RECT& rc) override {
        const Theme& t = Baseline();
        UINT dpi = host_ ? host_->Dpi() : 96;
        // A centred card, to show the shared look and that content paints inside the given rect.
        int cw = S(520, dpi), chh = S(260, dpi);
        RECT card{rc.left + (rc.right - rc.left - cw) / 2, rc.top + (rc.bottom - rc.top - chh) / 2, 0, 0};
        card.right = card.left + cw; card.bottom = card.top + chh;
        HBRUSH b = CreateSolidBrush(t.panel2);
        HPEN pen = CreatePen(PS_SOLID, 1, t.border), oldPen = (HPEN)SelectObject(dc, pen);
        HBRUSH oldB = (HBRUSH)SelectObject(dc, b);
        RoundRect(dc, card.left, card.top, card.right, card.bottom, S(14, dpi), S(14, dpi));
        SelectObject(dc, oldPen); SelectObject(dc, oldB); DeleteObject(pen); DeleteObject(b);

        auto line = [&](const std::wstring& s, HFONT f, COLORREF c, int y, UINT fmt) {
            RECT r{card.left + S(28, dpi), card.top + S(y, dpi), card.right - S(28, dpi), card.top + S(y + 40, dpi)};
            SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, s.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | fmt | DT_NOPREFIX);
        };
        line(L"Hello from Hadron", fonts_.title, t.text1, 30, DT_LEFT);
        line(L"The module contract works, standalone and in the suite.", fonts_.ui, t.text2, 86, DT_LEFT);
        line(host_ && host_->IsElevated() ? L"Elevated: yes" : L"Elevated: no", fonts_.ui, t.text2, 128, DT_LEFT);
        line(L"Opened " + std::to_wstring(opened_) + L" time(s) (saved in settings)", fonts_.caption, t.text3, 164, DT_LEFT);
        line(L"HadronHello.exe  ·  Hadron.exe --solo hello", fonts_.mono, t.accent, 206, DT_LEFT);
    }

    int RunCli(int argc, wchar_t** argv) override {
        for (int i = 0; i < argc; i++) {
            if (std::wstring(argv[i]) == L"--selftest") {
                // A real check: the theme scales with DPI and the Baseline tokens are populated.
                bool ok = (S(10, 192) == 20) && (Baseline().accent == RGB(96, 160, 255));
                wprintf(L"hello selftest: %ls\n", ok ? L"PASS" : L"FAIL");
                return ok ? 0 : 1;
            }
        }
        return -1;
    }

private:
    IHost* host_ = nullptr;
    HWND hwnd_ = nullptr;
    Fonts fonts_;
    int opened_ = 0;
};

}  // namespace

std::unique_ptr<Module> MakeHelloModule() { return std::make_unique<HelloModule>(); }

}  // namespace hx
