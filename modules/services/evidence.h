// modules/services/evidence.h — read-only evidence for the detail pane and the list (svcdrv follow-ups 6 and 7):
// the Authenticode signer of each item's file, and the System log's event 7040 (who changed a start type, when).
//
// Pure parts (labels, the fixture formats, the event XML parser) are testable under Wine; the live parts
// (WinVerifyTrust + CryptCATAdmin, EvtQuery + LookupAccountSid) only read, and run on Worker threads.
#pragma once

#include "modules/services/model.h"
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hx::services {

// ---- Signer (Authenticode) ------------------------------------------------------------------------------------
// Next to Company (the file's own claim in its version resource), the Signer column says who signed the file and
// whether Windows trusts that signature, checked offline: WTD_REVOKE_NONE, WTD_CACHE_ONLY_URL_RETRIEVAL.
enum class SignState {
    Pending,     // not read yet (the signer pass is still running), or no data in a fixture
    Signed,      // an embedded signature that verifies; subject = the signing certificate's CN
    Catalog,     // no valid embedded signature, but the file's hash is in a system catalog that verifies
    Unsigned,    // no embedded signature and in no catalog
    Untrusted,   // a signature that does not verify (bad digest, untrusted root, revoked, explicitly distrusted)
    CantCheck,   // not checked: why is in detail (network path, cloud file, reparse point, no file, a failed call)
};
struct SignerInfo {
    SignState state = SignState::Pending;
    std::wstring subject;   // CN of the signer (the embedded signature's, or the catalog's)
    std::wstring detail;    // the reason (Untrusted, CantCheck) or the catalog file (Catalog)
};
std::wstring SignerShort(const SignerInfo& s);   // the list cell: "Signed: Microsoft Windows", "Catalog-signed", ...
std::wstring SignerLong(const SignerInfo& s);    // the detail pane's line, with how it was checked
const wchar_t* SignStateName(SignState s);       // "signed", "catalog", ... (fixture format)
bool SignStateFromName(const std::wstring& n, SignState* out);

// Hard rule 1 for the signer read: "" when a file with these attributes may be opened and hashed, else why not. A
// recall-on-open, recall-on-data-access or offline file is never read (it would be downloaded); a reparse point is a
// leaf, as for the Company lookup.
std::wstring SignerAttrsProblem(DWORD attrs);
// The file whose signature counts for an item: its image, or ServiceDll for a svchost service. "" with *why when
// there is none (live: reads the registry with query rights).
std::wstring ItemSignedPath(const SvcItem& item, std::wstring* why);
// Live: the signer of one file, offline and read-only. Refuses (CantCheck) a UNC path, a network drive, a reparse
// point anywhere on the path, and a recall or offline file; the handle is opened with FILE_FLAG_OPEN_REPARSE_POINT and
// its attributes are checked again before WinVerifyTrust and the catalog hash read it. Cached per path, file size
// and last-write time for the life of the process, so a second pass costs nothing.
SignerInfo ReadSigner(const std::wstring& path);
SignerInfo ItemSigner(const SvcItem& item);   // ItemSignedPath + ReadSigner
// What a failed WinVerifyTrust status means when no catalog vouches for the file either: a chain the offline check
// can't finish (untrusted or unreachable root, chaining, system error) is Can't check, never "Not trusted"; a bad
// digest, explicit distrust, revocation, expiry or wrong usage is Not trusted.
SignerInfo SignerFromStatus(long status);

// Fixture file signers.txt: "HXSVC\t1\tsigners", then "G\tname\tstate\tsubject\tdetail" (bios escaping).
bool ParseSigners(const std::string& utf8, std::map<std::wstring, SignerInfo>* out, std::wstring* err);

// ---- Event 7040: who changed a start type -----------------------------------------------------------------------
// "The start type of the %1 service was changed from %2 to %3." The Service Control Manager writes it to the System
// log when a start type is changed through its API; param4 is the service name, Security/@UserID the account.
struct Ev7040 {
    std::wstring service, display, from, to;   // param4, param1, param2, param3 (from/to in Windows' words)
    std::wstring sid, account;                 // the event's user SID, and the account it resolves to ("" if not)
    std::wstring time;                         // System/TimeCreated/@SystemTime, UTC, ISO 8601 as Windows writes it
    uint64_t record = 0;                       // EventRecordID
};
// Parses one rendered event (EvtRender EvtRenderEventXml, or the XML Event Viewer shows; either quote style). False
// with *err when it is not a Service Control Manager event 7040 or a field is missing.
bool ParseEvent7040(const std::wstring& xml, Ev7040* out, std::wstring* err);
std::wstring XmlUnescape(const std::wstring& s);
// "2026-11-09 19:12 UTC" from "2026-11-09T19:12:03.1234567Z"; the input when it isn't that shape.
std::wstring EventTimeText(const std::wstring& systemTime);
// The newest event per item: names maps a lower-case service name to its display name (an event without param4 is
// matched by display name). Newest = latest TimeCreated, then the higher record id.
std::map<std::wstring, Ev7040> NewestPerService(const std::vector<Ev7040>& events, const std::map<std::wstring, std::wstring>& names);
// The detail line: who, when, from and to; or "no 7040 event found (log may have rolled over)".
std::wstring Ev7040Text(const Ev7040* e);
// Fixture file events7040.txt: "HXSVC\t1\tevents", "A\tsid\taccount" (how the fixture resolves a SID) and
// "E\t<escaped event XML>" lines.
bool ParseEventsFixture(const std::string& utf8, std::vector<Ev7040>* out, std::wstring* err);
// Live, read-only: EvtQuery of the System log for Service Control Manager event 7040, newest first, stopping when
// every name has its newest event or after a bounded number of events; SIDs resolved with LookupAccountSid.
bool ReadLive7040(const std::map<std::wstring, std::wstring>& names, const std::function<bool()>& cancelled,
                  std::vector<Ev7040>* out, std::wstring* err);

}  // namespace hx::services
