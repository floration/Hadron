// modules/bios/firmware.h — read-only firmware access for the bios module (hard rule 2: this module
// only ever reads UEFI variables and SMBIOS; there is no write path anywhere in it).
//
// Everything here that touches firmware runs on a core/sys Worker thread. SeSystemEnvironmentPrivilege
// is enabled on that thread's own impersonation token only (ThreadPrivilege), never on the process.
#pragma once

#include "core/sys/win_compat.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hx::bios {

// ---- SMBIOS (version guard) ---------------------------------------------------------------------
struct SmbiosInfo {
    bool ok = false;
    std::wstring biosVendor, biosVersion, biosDate;   // type 0
    std::wstring boardMaker, boardProduct;            // type 2
};
// Parses the RawSMBIOSData blob GetSystemFirmwareTable('RSMB') returns (8-byte header + table).
bool ParseSmbios(const uint8_t* raw, size_t n, SmbiosInfo* out);
bool ReadSmbios(SmbiosInfo* out, DWORD* err);

// ---- privilege ----------------------------------------------------------------------------------
// Impersonates self on the calling thread and enables one privilege on that thread token. Reverts on
// destruction. ok() only when AdjustTokenPrivileges succeeded and GLE != ERROR_NOT_ALL_ASSIGNED (rule 4).
class ThreadPrivilege {
public:
    explicit ThreadPrivilege(const wchar_t* name);
    ~ThreadPrivilege();
    ThreadPrivilege(const ThreadPrivilege&) = delete;
    ThreadPrivilege& operator=(const ThreadPrivilege&) = delete;
    bool ok() const { return ok_; }
    DWORD err() const { return err_; }
private:
    bool impersonating_ = false, ok_ = false;
    DWORD err_ = 0;
};

// ---- variables ----------------------------------------------------------------------------------
struct FwVar {
    std::wstring name, guid;      // guid lowercase, no braces
    DWORD err = 0;                // 0 ok; 203 not found, 1314 privilege missing, 5 protected, 122 too big ...
    DWORD attr = 0;
    std::vector<uint8_t> data;
};

// Reads one variable with GetFirmwareEnvironmentVariableExW, growing the buffer on error 122 (up to 4 MB).
void ReadVariable(FwVar* v);

// Firmware type as text ("UEFI", "Legacy BIOS", "Unknown"); *uefi set when it is UEFI.
std::wstring FirmwareTypeText(bool* uefi);

// The live read the module runs on start (spec "Live read"): elevation, firmware type, SMBIOS, then
// each (var, guid) pair of the map, with per-variable error codes.
struct LiveResult {
    bool elevated = false;
    bool uefi = false;
    std::wstring fwType;
    bool privOk = false; DWORD privErr = 0;
    SmbiosInfo smbios; DWORD smbiosErr = 0;
    std::vector<FwVar> vars;
    int readOk = 0;
    std::wstring error;   // set when the read itself failed (e.g. out of memory on the worker)
};
void ReadLive(const std::vector<std::pair<std::wstring, std::wstring>>& want, LiveResult* out);

// Enumerates every variable name with NtEnumerateSystemEnvironmentValuesEx (class 1, names only;
// loaded with GetProcAddress, read-only) and reads each one. For "Save snapshot". Must run with the
// privilege enabled on the calling thread. False with a reason when enumeration itself fails.
bool EnumerateAll(std::vector<FwVar>* out, std::wstring* err, const std::function<bool()>& cancelled = {});

// Human text for a per-variable error code (203 / 1314 / 5 explained, others via FormatMessage).
std::wstring VarErrorText(DWORD err);

}  // namespace hx::bios
