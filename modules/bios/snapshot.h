// modules/bios/snapshot.h — UEFI snapshots: load (folder or .zip, legacy dumps included), save
// (replaces legacy Dump-UEFI.ps1/Run-Dump.bat) and the RFC 4180 CSV parser for index.csv.
//
// Layout (docs/legacy/bios-explorer.md §3): index.csv (Name, Guid, Size, Attributes, Error, File),
// vars\<file>.bin, system.txt. Fixes: file names carry an index prefix (NNNN_<sanitised>__<guid>.bin) so
// two variables never share a file; nothing is ever overwritten; the CSV parser handles "" and quoted
// newlines. Saving writes files only (CREATE_NEW), never moves or deletes anything.
#pragma once

#include "modules/bios/db.h"
#include "modules/bios/firmware.h"
#include <map>
#include <string>
#include <vector>

namespace hx::bios {

// RFC 4180: comma-separated, "" inside quotes is a quote, quoted fields may contain commas and
// newlines; CRLF or LF ends a record. A trailing empty line is not a record.
std::vector<std::vector<std::wstring>> ParseCsv(const std::wstring& text);
std::wstring CsvQuote(const std::wstring& field);   // always quoted, as Export-Csv writes

// Legacy naming rule (Dump-UEFI.ps1 123-125): characters outside [A-Za-z0-9_.-] become '_', cut to 80.
std::wstring SanitiseName(const std::wstring& name);
// New naming: "<NNNN>_<sanitised>__<guid>.bin" (index 1-based, at least 4 digits).
std::wstring SnapFileName(int index, const std::wstring& name, const std::wstring& guid);

// Where a snapshot's files come from: a folder on disk, a zip, or memory (selftest).
struct FileSource {
    virtual ~FileSource() = default;
    // rel uses '/' separators, relative to the snapshot root. false + *err when missing/unreadable.
    virtual bool Read(const std::wstring& rel, std::string* out, std::wstring* err) = 0;
};

struct SnapLoad {
    bool ok = false;
    std::wstring label;               // what was opened (folder or zip path)
    std::wstring error;               // why it failed (ok == false)
    VarMap vars;
    int rows = 0, loaded = 0, skipped = 0;
    std::map<std::wstring, int> skipReasons;   // reason -> count
    bool hasSystem = false;
    std::wstring sysBoard, sysBios;   // system.txt "Board:" and "BIOS:" lines, when present
    std::wstring SkipSummary() const;  // "3 skipped: 2 no data file, 1 ..." or empty
};

bool LoadSnapshotFrom(FileSource& src, const std::wstring& label, SnapLoad* out);
// A zip already in memory: index.csv at the root or inside one folder (the shortest such path wins).
bool LoadSnapshotZip(std::vector<uint8_t> bytes, const std::wstring& label, SnapLoad* out);
// path: a snapshot folder, any file inside one (e.g. its index.csv), or a .zip.
bool LoadSnapshot(const std::wstring& path, SnapLoad* out);

struct SaveResult {
    bool ok = false;
    std::wstring folder, error;
    int found = 0, read = 0;
};
// Writes UEFI_Dump_<yyyyMMdd_HHmm>[-N]\ under parent (never overwrites: CREATE_NEW everywhere, and the
// folder gets -2, -3 ... if the name exists).
bool SaveSnapshot(const std::wstring& parent, const std::vector<FwVar>& vars, const SmbiosInfo& smbios,
                  SaveResult* out);

}  // namespace hx::bios
