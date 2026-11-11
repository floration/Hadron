// core/sys/sys.h — elevation, privileges, paths, error text. See docs/ARCHITECTURE.md.
#pragma once

#include "core/sys/win_compat.h"
#include <string>

namespace hx::sys {

// GetTokenInformation(TokenElevation). CLAUDE.md hard rule 4.
bool IsElevated();

// AdjustTokenPrivileges for one privilege. Returns true only if it succeeded AND
// GetLastError() != ERROR_NOT_ALL_ASSIGNED. On failure, *err (if non-null) gets the Win32 code.
bool EnablePrivilege(const wchar_t* name, DWORD* err = nullptr);

// GetModuleFileNameW looped until the path fits (CLAUDE.md "Long paths").
std::wstring ModulePath();

// GetFullPathNameW, then the \\?\ prefix (\\?\UNC\ for UNC). For Win32 file APIs.
std::wstring ToExtended(const std::wstring& path);

// Strips the \\?\ or \\?\UNC\ prefix, for shell APIs (ShellExecute, dialogs, IFileOperation).
std::wstring ToShell(const std::wstring& path);

// FormatMessageW for a Win32 error, trimmed. Used wherever an error is surfaced to the user.
std::wstring ErrorText(DWORD err);

}  // namespace hx::sys
