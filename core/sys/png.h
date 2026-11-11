// core/sys/png.h — a tiny, dependency-free PNG writer for --screenshot (CLAUDE.md hard rule 3:
// no third-party code). 8-bit RGB, stored (uncompressed) DEFLATE, own CRC-32 and Adler-32.
#pragma once

#include "core/sys/win_compat.h"
#include <string>

namespace hx::sys {

// Writes `width`x`height` pixels as an 8-bit RGB PNG. `bgraTopDown` is width*height 32-bit pixels,
// top row first, in GDI's BGRA order (the bytes a top-down 32-bit DIB section gives). Alpha is
// dropped. Returns false (and sets *err) on any file error.
bool SavePng(const std::wstring& path, int width, int height, const void* bgraTopDown, DWORD* err = nullptr);

}  // namespace hx::sys
