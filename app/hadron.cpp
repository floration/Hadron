// app/hadron.cpp — Hadron.exe, the suite. Links every module and shows them behind the sidebar.
#include "core/host/run.h"
#include "core/host/registry.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return hx::RunSuite(hx::MakeAllModules());
}
