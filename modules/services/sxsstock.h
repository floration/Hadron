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

// Precedence (probe round 2, which matched the known values): (1) the edition file's override
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
    Src StartSrc() const;
    Src DelayedSrc() const;      // NONE when nothing says (not delayed)
    bool Usable() const;
    bool LowConfidence() const;  // the start, or an Auto value's delay, comes from a setting default alone
    // delayed 0/1 for Auto, -1 otherwise (left alone).
    StartCfg Cfg() const;
};
std::wstring SxsSrcText(SxsValue::Src s);   // "the edition file's override", ...

// Probe round 2: start types Windows sets at runtime from the hardware or software it finds, not from any file
// (WlanSvc, stisvc, camsvc, IKEEXT, PolicyAgent, iphlpsvc, ssh-agent, WinHttpAutoProxySvc; DelayedAutostart on BITS,
// WinRM, dcsvc, dmwappushservice). Shown with the manifest value and a note, never applied.
bool SxsRuntimeDependent(const std::wstring& name);

struct SxsStock {
    bool ok = false;
    std::wstring build;          // "19045.2965": CurrentBuildNumber.UBR of the PC the manifests were read on
    std::wstring edition;        // EditionID, "" when unknown
    bool editionRead = false;    // the edition file was found and read
    std::wstring err;            // why there is no value (ok false), shown to the user
    int manifests = 0, failed = 0, skipped = 0, overrides = 0;
    bool fromCache = false;
    std::map<std::wstring, SxsValue> byName;   // lower-case service name
    // The value for a name; a per-user instance ("CDPUserSvc_4f2a1") falls back to its template. Null when none or
    // when the manifests conflict.
    const SxsValue* Find(const std::wstring& name) const;
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
void ScanManifestXml(const std::string& xml, SxsScan* acc);
// The edition file: each <component name="C"> child <S>value</S> whose "c|s" is a known setting overrides that
// service's value. Returns how many were applied.
int ApplyEditionXml(const std::string& xml, SxsScan* acc);

class SxsReader {
public:
    virtual ~SxsReader() = default;
    // Prepares the reader (live: finds and loads wcp.dll's dictionary and msdelta.dll) and lists the manifests.
    virtual bool List(std::vector<std::wstring>* names, std::wstring* err) = 0;
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

// The cache in the data folder: "sxs-<build>.<ubr>-<edition>.txt", written once (temp + rename, never overwritten).
std::wstring SxsCacheName(const std::wstring& buildUbr, const std::wstring& edition);   // "" when either is odd
std::string WriteSxsCache(const SxsStock& s);
bool ParseSxsCache(const std::string& utf8, SxsStock* out, std::wstring* err);

}  // namespace hx::services
