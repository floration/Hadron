// modules/services/custom.h — custom presets: the user's own start-type lists, stored in the data folder and
// exchanged as plain UTF-8 text (docs/design/services.md "Custom presets").
//
// A custom preset never applies anything by itself. It only fills a plan (BuildCustom in plan.cpp), which goes
// through the same plan, confirm, snapshot and undo path as every other plan; the guards built-in presets get
// drop its protected items with a visible note.
//
// File format (the stored file and the exported file are the same bytes; Hadron's own parser, hard rule 3):
//   HXPRESET<TAB>1                 line 1: the format and its version (only 1 is read)
//   hadron<TAB><version>           the Hadron version that wrote it
//   build<TAB><windows build>      the Windows build it was made on, e.g. 19045
//   tab<TAB>services|drivers       which list it belongs to
//   name<TAB><preset name>         1..kPresetMaxName characters, no control characters
//   <item><TAB><start type>        one line per item: auto, auto-delayed, manual or disabled
// UTF-8 without a BOM, LF line ends. The parser also takes a UTF-8 BOM, CRLF line ends and trailing empty lines
// (Notepad writes them), and refuses everything else, naming the line: another version, an unknown or missing
// header line, a line over kPresetMaxLine bytes, a bad start type (Boot and System included: Hadron never sets
// them), a duplicate or malformed name, invalid UTF-8, more than kPresetMaxItems items or kPresetMaxFile bytes.
#pragma once

#include "modules/services/model.h"
#include "modules/services/store.h"
#include <string>
#include <vector>

namespace hx::services {

constexpr int kPresetFormat = 1;
constexpr size_t kPresetMaxLine = 512;        // bytes, without the line end
constexpr size_t kPresetMaxFile = 1u << 20;   // 1 MiB
constexpr size_t kPresetMaxItems = 4096;
constexpr size_t kPresetMaxName = 80;         // characters of the preset's own name
constexpr size_t kItemMaxName = 256;          // characters of a service or driver name (the SCM's limit)

struct CustomItem {
    std::wstring name;
    StartCfg target;   // {2,0} auto, {2,1} auto-delayed, {3,-1} manual, {4,-1} disabled
};

struct CustomPreset {
    std::wstring name, hadron, build;
    int tab = TAB_SERVICES;
    std::vector<CustomItem> items;
    std::wstring file;    // presets\<file> in the data folder; empty for a preset read from an import
    std::wstring saved;   // when it was saved (from the file name's UTC stamp, as "YYYY-MM-DD HH:MM UTC"); empty if not
};

// The Hadron version this build writes into presets ("0.1.0").
std::wstring HadronVersion();
// "auto", "auto-delayed", "manual", "disabled"; "" for Boot/System (never in a preset file).
std::wstring StartWord(const StartCfg& c);
bool StartFromWord(const std::wstring& w, StartCfg* out);

std::string WritePreset(const CustomPreset& p);
// Strict: on any problem returns false and *err names the line ("line 7: start type 'off' is not one of ...").
bool ParsePreset(const std::string& bytes, CustomPreset* out, std::wstring* err);
// The name rules ParsePreset applies to a preset's own name, for the Save field: "" when fine, else why not.
std::wstring PresetNameProblem(const std::wstring& name);

// ---- the data folder ---------------------------------------------------------------------------------------
// presets\<stamp>.txt holds one preset each (CREATE_NEW, flushed, never overwritten). Removing one writes
// presets\removed-<stamp>.txt naming the file; nothing is deleted. Callers run Store::EnsureRoot first (the A1
// ancestor rule), as for every other file in the data folder.
std::vector<CustomPreset> LoadCustomPresets(const Store& st, std::vector<std::wstring>* problems);
// Refuses a name already taken (case-insensitive) by a preset in `existing`. Fills p->file and p->saved.
bool SaveCustomPreset(Store& st, CustomPreset* p, const std::vector<CustomPreset>& existing, std::wstring* err);
bool RemoveCustomPreset(Store& st, const CustomPreset& p, std::wstring* err);

// ---- files the user picks (export and import, Browse buttons) ------------------------------------------------
// Export: CREATE_NEW only. An existing file is never replaced: *exists is set and nothing is written.
bool ExportPresetFile(const std::wstring& path, const CustomPreset& p, bool* exists, std::wstring* err);
// Import: reads at most kPresetMaxFile bytes and parses. A cloud placeholder (recall on open or on data access,
// offline) is refused unread, so picking it never starts a download.
bool ImportPresetFile(const std::wstring& path, CustomPreset* out, std::wstring* err);

}  // namespace hx::services
