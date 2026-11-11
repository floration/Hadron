// modules/bios/standalone.cpp — the HadronBios.exe entry point.
#include "core/host/run.h"
#include "core/host/registry.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return hx::RunStandalone(hx::MakeBiosModule());
}
