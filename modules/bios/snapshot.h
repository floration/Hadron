// modules/bios/snapshot.h — UEFI snapshots: load (folder or .zip, legacy dumps included), save
// (replaces legacy Dump-UEFI.ps1/Run-Dump.bat) and the RFC 4180 CSV parser for index.csv.
//
// Layout (docs/legacy/bios-explorer.md §3): index.csv (Name, Guid, Size, Attributes, Error, File),
// vars\<file>.bin, system.txt. Fixes: file names carry an index prefix (NNNN_<sanitised>__<guid>.bin) so
// two variables never share a file; nothing is ever overwritten; the CSV parser handles "" and quoted
// newlines. Saving writes files only (CREATE_NEW, flushed), never moves or deletes anything.
#pragma once

#include "modules/bios/db.h"
#include "modules/bios/firmware.h"
#include <map>
#include <string>
#include <vector>

namespace hx::bios {

// RFC 4180: comma-separated, "" inside quotes is a quote, quoted fields may contain commas and
// newlines; CRLF or LF ends a record, and a last line without a newline is still a record. An empty
// line comes back as a record with one empty field (callers skip those).
std::vector<std::vector<std::wstring>> ParseCsv(const std::wstring& text);
std::wstring CsvQuote(const std::wstring& field);   // always quoted, as Export-Csv writes

// Legacy naming rule (Dump-UEFI.ps1 123-125): characters outside [A-Za-z0-9_.-] become '_', cut to 80.
std::wstring SanitiseName(const std::wstring& name);
// New naming: "<NNNN>_<sanitised>__<guid>.bin" (index 1-based, at least 4 digits).
std::wstring SnapFileName(int index, const std::wstring& name, const std::wstring& guid);

// Size caps for snapshot input, so a hostile or corrupt snapshot can't exhaust memory.
constexpr size_t kMaxSnapVar = 4u << 20;      // one variable: ReadVariable's 4 MB ceiling
constexpr size_t kMaxSnapText = 16u << 20;    // index.csv, system.txt
constexpr size_t kMaxSnapTotal = 256u << 20;  // all variable data of one snapshot; also the .zip file

// Where a snapshot's files come from: a folder on disk, a zip, or memory (selftest).
struct FileSource {
    virtual ~FileSource() = default;
    // rel uses '/' separators, relative to the snapshot root. Refuses, without allocating, anything
    // larger than cap. false + *err when missing, unreadable or too large.
    virtual bool Read(const std::wstring& rel, std::string* out, size_t cap, std::wstring* err) = 0;
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
// Writes one new file: CREATE_NEW (never overwrites), every byte, then FlushFileBuffers before closing.
bool WriteNewFile(const std::wstring& path, const std::string& data, std::wstring* err);
// Writes the snapshot layout (vars\, index.csv, system.txt) into folder, which must already exist and
// be empty. savedBy is system.txt's "Saved by:" line. Used by SaveSnapshot and by the BIOS write
// backups (fwwrite.cpp), so Load snapshot reads both. Leaves out->folder as the caller set it.
bool WriteSnapshotFiles(const std::wstring& folder, const std::vector<FwVar>& vars, const SmbiosInfo& smbios,
                        const std::wstring& savedBy, SaveResult* out);
// Writes UEFI_Dump_<yyyyMMdd_HHmm>[-N]\ under parent (never overwrites: CREATE_NEW everywhere, and the
// folder gets -2, -3 ... if the name exists).
bool SaveSnapshot(const std::wstring& parent, const std::vector<FwVar>& vars, const SmbiosInfo& smbios,
                  SaveResult* out);

}  // namespace hx::bios
