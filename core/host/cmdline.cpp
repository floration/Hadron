#include "core/host/cmdline.h"
#include "core/sys/sys.h"
#include <shellapi.h>
#include <vector>
#include <cwchar>

namespace hx {

CommandLine CommandLine::Parse(const wchar_t* raw) {
    CommandLine cl;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(raw ? raw : L"", &argc);
    if (!argv) return cl;

    auto next = [&](int& i) -> std::wstring {
        return (i + 1 < argc) ? std::wstring(argv[++i]) : std::wstring();
    };
    for (int i = 1; i < argc; i++) {   // argv[0] is the exe
        std::wstring a = argv[i];
        if (a == L"--module")          cl.module = next(i);
        else if (a == L"--solo")       cl.solo = next(i);
        else if (a == L"--elevated")   cl.elevated = true;
        else if (a == L"--selftest")   cl.selftest = true;
        else if (a == L"--screenshot") cl.screenshot = sys::ToExtended(next(i));
        else if (a == L"--fixture")    cl.fixture = sys::ToExtended(next(i));
        else if (a == L"--theme") {
            std::wstring v = next(i);
            if (v == L"light")       cl.theme = ui::ThemeId::Light;
            else if (v == L"dark")   cl.theme = ui::ThemeId::Dark;
            else if (v == L"fun")    cl.theme = ui::ThemeId::Fun;
            else if (v == L"boring") cl.theme = ui::ThemeId::Boring;
        } else if (a == L"--layout") {
            std::wstring v = next(i);
            cl.layout = (v == L"console") ? ui::Layout::Console : ui::Layout::Baseline;
        } else if (a == L"--accent") {
            std::wstring v = next(i);
            wchar_t* end = nullptr;
            unsigned long rgb = wcstoul(v.c_str(), &end, 16);
            if (v.size() == 6 && end && *end == 0) {
                cl.hasAccent = true;
                cl.accent = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
            }
        } else                         cl.rest.push_back(a);
    }
    LocalFree(argv);
    return cl;
}

}  // namespace hx
