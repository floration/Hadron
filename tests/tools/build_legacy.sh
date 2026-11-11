#!/bin/sh
# Builds the legacy tools unchanged (Phase 0 baseline). Output: build/legacy/*.exe
# Link libraries were not recorded in HISTORY.md; these are the ones the sources need.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/legacy
CXX=x86_64-w64-mingw32-g++; RC=x86_64-w64-mingw32-windres
FLAGS="-O2 -std=c++17 -municode -mwindows -static -s"
(cd legacy/diskmap && $RC res.rc -O coff -o ../../build/legacy/dm_res.o)
$CXX $FLAGS legacy/diskmap/diskmap.cpp build/legacy/dm_res.o -o build/legacy/DiskMap.exe -ldwmapi -lshell32 -lole32 -lmsimg32
(cd legacy/bios-explorer/app && $RC app.rc -O coff -o ../../../build/legacy/bsx_res.o)
$CXX $FLAGS legacy/bios-explorer/app/bsx.cpp build/legacy/bsx_res.o -o build/legacy/BiosSettingsExplorer.exe -lcomdlg32 -lshell32
