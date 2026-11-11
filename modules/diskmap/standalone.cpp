// modules/diskmap/standalone.cpp — the HadronDiskMap.exe entry point.
#include "core/host/run.h"
#include "core/host/registry.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return hx::RunStandalone(hx::MakeDiskMapModule());
}
