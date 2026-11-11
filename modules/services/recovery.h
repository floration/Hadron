// modules/services/recovery.h — offline recovery: restore.cmd / README.txt text and a stored-only zip.
//
// Generation is pure (golden-tested, tests/golden/services/restore.cmd). restore.cmd runs from WinRE's
// Command Prompt: it loads the offline SYSTEM hive of the Windows volume it sits on (%~d0, or the drive
// given as %1), reads Select\Current, and writes back only Start (and DelayedAutostart where it was
// changed) for keys that exist. It never creates keys and never loops over drive letters (r1 §6.3,
// r2 §5, r3 §4, r4 §1). Spec §5.4.
#pragma once

#include "modules/services/model.h"
#include "modules/services/store.h"
#include <string>
#include <vector>

namespace hx::services {

struct RestoreRow {
    std::wstring name;
    StartCfg cfg;               // the value to restore
    bool writeDelayed = false;  // also restore DelayedAutostart (it was changed, or was recorded)
    bool ti = false;            // Hadron needed TrustedInstaller for it: the scripts may fail on it (README E)
};

// True when a name can go into the ASCII batch file as-is: an allow-list of ASCII letters, digits, space and
// _ - . { } # $ @ + ~. Anything else, ( ) included, goes to the .reg files and README section C.
bool CmdSafeName(const std::wstring& name);

// folder: the recovery folder as it will look from WinRE without the drive, e.g.
// "\ProgramData\Hadron\services\recovery\20261111-101500". CRLF, ASCII only.
std::string RestoreCmd(const std::wstring& stamp, const std::wstring& title, const std::vector<RestoreRow>& rows);
std::string RestoreOnlineCmd(const std::wstring& stamp, const std::vector<RestoreRow>& rows);   // Safe Mode variant
// notRestored: names deliberately left out of the scripts (README section F), e.g. the first-run recovery's
// Boot/System-start, storage and never-touch items.
std::string ReadmeTxt(const std::wstring& stamp, const std::wstring& folder, const std::wstring& title,
                      const std::vector<RestoreRow>& rows, const std::vector<std::wstring>& notRestored = {});
// UTF-16LE .reg (with BOM) for names restore.cmd can't carry; imported by hand (README says how).
std::string UnicodeReg(const std::vector<RestoreRow>& rows, int controlSet);

struct ZipEntry { std::string name, data; };
std::string MakeZip(const std::vector<ZipEntry>& entries, uint16_t dosTime, uint16_t dosDate);
bool ParseZip(const std::string& zip, std::vector<ZipEntry>* out, std::wstring* err);   // stored only; checks CRCs

// Writes recovery\<stamp>\{restore.cmd, restore-online.cmd, README.txt[, restore-unicode-*.reg]} and
// recovery-<stamp>.zip into the store, each CREATE_NEW and flushed. *dir and *zip get the paths.
bool WriteRecovery(Store& store, const std::wstring& stamp, const std::wstring& title, const std::vector<RestoreRow>& rows,
                   std::wstring* dir, std::wstring* zip, std::wstring* err, const std::vector<std::wstring>& notRestored = {});

}  // namespace hx::services
