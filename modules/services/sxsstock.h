// modules/services/sxsstock.h — Windows-stock start types read from this PC's own WinSxS component manifests
// (fix/svcdrv-stock; DECISIONS 2026-11-11, owner: stock values come first from the PC's own manifests,
// cross-checked with third-party tables).
//
// Every component manifest in %windir%\WinSxS\Manifests is a "DCM\x01" delta against an XML dictionary held in the
// servicing stack's wcp.dll (resource type 614, name 1). The live reader (LiveSxsReader) loads wcp.dll as a data
// file only and msdelta.dll by LoadLibraryEx/GetProcAddress (both Windows system DLLs: rule 3), applies each delta
// and hands back the XML. ScanManifestXml, Hadron's own small scanner, takes from each:
//   <serviceData name="X" start="auto|delayedAuto|demand|disabled|boot|system" ...>
//   <registryKey keyName="HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\X"> <registryValue name="Start"
//   value="..."/> <registryValue name="DelayedAutostart" value="..."/> </registryKey>
// A name given two different values by two manifests is a conflict and gets no value.
//
// Read only. A manifest file with a reparse-point, recall or offline attribute is skipped without being opened
// (hard rule 1), and the attributes are checked again on the open handle. The fake reader (selftest, screenshots)
// reads plain XML fixtures; live decompression is checked on a real Windows PC only.
#pragma once

#include "modules/services/model.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hx::services {

// Precedence (probe round 2; checked against the probe session's expected values, not a source): (1) the edition
// file's override
// (%windir%\servicing\Editions\<EditionID>Edition.xml, <component name=".."><Start_X>2</Start_X></component>);
// (2) the component's <serviceData start> or its registryKey Services\X Start value; (3) a configuration-setting
// default (<xsd:element name="Start_X" default="3" wcm:handler="regkey('...\Services\X')" wcm:legacyName="Start">).
// Setting defaults can be Server values (Themes default 4, Auto on a client), so a value from them alone is low
// confidence and is applied only where Hadron's KB agrees.
struct SxsValue {
    enum Src { NONE, EDITION, SERVICE, REGISTRY, SETTING };
    int svcStart = -1;           // a <serviceData> start (0..4), -1 none
    int regStart = -1;           // a registryKey ...\Services\X Start value, -1 none
    int delayed = -1;            // 1 for delayedAuto, else a registryValue DelayedAutostart (0/1), -1 none
    int setStart = -1, setDelayed = -1;   // configuration-setting defaults
    int edStart = -1, edDelayed = -1;     // the edition file's override
    bool conflict = false;       // two manifests (or serviceData and the registry value) disagree: no value
    bool editionUnread = false;  // a setting the edition file can override targets it, and that file wasn't read
    // The file that first set each layer's start (for --dump-stock-live; not cached).
    std::wstring fileService, fileRegistry, fileSetting, fileEdition;
    Src StartSrc() const;
    const std::wstring& StartFile() const;   // the file of StartSrc()'s layer
    Src DelayedSrc() const;      // NONE when nothing says (not delayed)
    bool Usable() const;
    bool LowConfidence() const;  // the start, or an Auto value's delay, comes from a setting default alone
    // delayed 0/1 for Auto, -1 otherwise (left alone).
    StartCfg Cfg() const;
    // The DelayedAutostart value by layer (edition, then serviceData/registry, then a setting default); -1 when no
    // file sets it (owner's live run: print "-" for absent, never 0).
    int RawDelayed() const { return edDelayed >= 0 ? edDelayed : delayed >= 0 ? delayed : setDelayed; }
};
std::wstring SxsSrcText(SxsValue::Src s);   // "the edition file's override", ...

// Services the probe flagged as changed after setup (probe-session judgement, not a source; kb-audit svcdrv-stock M2).
// Shown with their values and a note, never applied.
bool SxsRuntimeStart(const std::wstring& name);     // start type: WlanSvc, stisvc, camsvc, IKEEXT, PolicyAgent, iphlpsvc,
                                                    // ssh-agent, WinHttpAutoProxySvc
bool SxsRuntimeDelayed(const std::wstring& name);   // DelayedAutostart: BITS, WinRM, dcsvc, dmwappushservice

// The cache's scanner version (review F5): a cache written by another version is renamed aside and read again.
constexpr int kSxsScannerVersion = 2;

struct SxsStock {
    bool ok = false;
    std::wstring build;          // "19045.2965": CurrentBuildNumber.UBR of the PC the manifests were read on
    std::wstring edition;        // EditionID, "" when unknown
    bool editionRead = false;    // the edition file was found and read
    std::wstring err;            // why there is no value (ok false), shown to the user
    std::wstring editionErr;     // why the edition file wasn't read
    int manifests = 0, failed = 0, skipped = 0, overrides = 0;
    bool truncated = false;      // the manifest listing ended on an error (review F4)
    bool fromCache = false;
    bool fixture = false;        // read from the test fixture's hand-written XML, not a real PC (kb-audit s3)
    // Some manifests failed or were skipped, or the listing was cut short: manifest values are shown, not applied.
    bool Incomplete() const { return failed > 0 || skipped > 0 || truncated; }
    // Only a complete scan with the edition file read is cached (review F3, F4).
    bool Cacheable() const { return ok && !Incomplete() && editionRead && !fixture; }
    std::map<std::wstring, SxsValue> byName;   // lower-case service name
    // The value for a name; with instance, a per-user instance ("CDPUserSvc_4f2a1") falls back to its template
    // (review F6: only for Kind::UserInstance). Null when none or when the manifests conflict.
    const SxsValue* Find(const std::wstring& name, bool instance = false) const;
    int Usable() const;
};

// What a scan collects: values per service, and each configuration setting's service, keyed "component|setting"
// (lower case), so the edition file's overrides can be mapped.
struct SxsScan {
    std::map<std::wstring, SxsValue> byName;
    std::map<std::wstring, std::pair<std::wstring, bool>> settings;   // -> {service, isDelayedAutostart}
};

// Hadron's own scanner (no XML library: rule 3). Adds every serviceData start and Services\X Start /
// DelayedAutostart registry value found in xml to out, marking conflicts.
void ScanManifestXml(const std::string& xml, SxsScan* acc, const std::wstring& file = L"");
// The edition file: each <component name="C"> child <S>value</S> whose "c|s" is a known setting overrides that
// service's value. Returns how many were applied.
int ApplyEditionXml(const std::string& xml, SxsScan* acc, const std::wstring& file = L"");

class SxsReader {
public:
    virtual ~SxsReader() = default;
    // Prepares the reader (live: finds and loads wcp.dll's dictionary and msdelta.dll) and lists the manifests.
    // *truncated: the listing ended on an error (some names may be missing).
    virtual bool List(std::vector<std::wstring>* names, bool* truncated, std::wstring* err) = 0;
    virtual bool IsFixture() const { return false; }
    // The manifest's XML. *skipped = true (and false returned) when its attributes forbid reading it.
    virtual bool ReadXml(const std::wstring& name, std::string* xml, bool* skipped, std::wstring* err) = 0;
    virtual std::wstring BuildUbr() = 0;   // "19045.2965"; "" when unknown (no cache then)
    virtual std::wstring Edition() = 0;    // EditionID ("Professional"); "" when unknown
    // The edition file's XML (live: %windir%\servicing\Editions\<EditionID>Edition.xml). false when absent.
    virtual bool ReadEdition(std::string* xml, std::wstring* err) = 0;
};

std::unique_ptr<SxsReader> MakeLiveSxsReader();
// dir\*.manifest (plain XML), dir\build.txt ("19045.2965"), dir\edition.txt (EditionID), dir\<EditionID>Edition.xml
std::unique_ptr<SxsReader> MakeFakeSxsReader(const std::wstring& dir);

// Reads every manifest (cancel: stop early, ok false).
SxsStock ScanSxs(SxsReader& r, const std::function<bool()>& cancelled = nullptr);

// The words for manifest values: this PC's (read on this PC by Hadron) or the test fixture's (not a real PC).
struct SxsWords {
    std::wstring who;        // "this PC's manifests"
    std::wstring shortSrc;   // the detail pane's source, in brackets after the value
    std::wstring label;      // the Basis label of an applied row (BasisLabel's condition)
    std::wstring note;       // the start of a basis note, up to "(... build N.UBR Edition"
    std::wstring uses;       // "Hadron uses it as the Windows stock value."
    std::wstring reason;     // a plan row's reason
    std::wstring planSrc;    // the plan note's source
};
SxsWords SxsWordsFor(const SxsStock* s);

// --dump-stock-live's UTF-8 TSV: "#" header lines (build.UBR, EditionID, manifests, failures, skipped, elapsed ms),
// then name, start, delayed, layer (edition|serviceData|registry|settingDefault), conflict, source file per name.
std::string SxsDumpTsv(const SxsStock& s, unsigned long long elapsedMs);

// The cache in the data folder: "sxs-<build>.<ubr>-<edition>.txt", written once (temp + rename, never overwritten).
std::wstring SxsCacheName(const std::wstring& buildUbr, const std::wstring& edition);   // "" when either is odd
std::string WriteSxsCache(const SxsStock& s);
bool ParseSxsCache(const std::string& utf8, SxsStock* out, std::wstring* err);

}  // namespace hx::services
