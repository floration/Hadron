// modules/services/hw.h — hardware evidence for the Stripped and Minimum presets (spec §5.5, r4 §7).
// The pure part works on a dump (tests/fixtures/services/hw.txt) so it is testable under Wine; the live
// part reads CfgMgr32 over every devnode, present and non-present, and is only called by the live backend.
#pragma once

#include "core/sys/win_compat.h"
#include "modules/services/model.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hx::services {

struct DevNode {
    std::wstring id, service, cls, classGuid, parent;
    bool present = false;
    std::vector<std::wstring> upper, lower;   // device-level filters
    uint64_t arrival = 0;                     // DEVPKEY_Device_LastArrivalDate (FILETIME, UTC); 0 when unknown
};

struct HwInfo {
    bool ok = false;
    std::wstring error;
    std::vector<DevNode> nodes;
    std::map<std::wstring, std::pair<std::vector<std::wstring>, std::vector<std::wstring>>> classFilters;  // guid -> upper, lower
    std::wstring bootDisk;                    // devnode id of the boot disk ("" when unknown)
    std::set<std::wstring> presentIfaces;     // device-interface class GUIDs present now (lower case)
    // Device-interface class GUID (lower case) -> the device instance ids that registered an interface of that class,
    // present or not (SetupDi without DIGCF_PRESENT). Filled for the GUIDs services name in their device-interface
    // arrival triggers (AddLiveIfaceEvidence); fixture lines "F guid devId".
    std::map<std::wstring, std::vector<std::wstring>> ifaceDevs;
    bool hypervisor = false;                  // the hypervisor is running (VBS / Hyper-V)
    bool isVm = false;                        // this Windows is itself a VM guest (firmware vendor strings)
};

bool ParseHw(const std::string& utf8, HwInfo* out, std::wstring* err);
std::string DumpHw(const HwInfo& hw);
bool ReadLiveHw(HwInfo* out);   // CfgMgr32 + registry, read-only; sets out->error on failure
// The devices behind each device-interface class the items name in their device-interface arrival triggers
// (SvcItem::ifaces), present or not: SetupDi over that class's interfaces, read-only. Fills hw->ifaceDevs.
void AddLiveIfaceEvidence(const std::vector<SvcItem>& items, HwInfo* hw);
// "2026-03-02": a FILETIME (UTC) as a date; "" for 0.
std::wstring FileTimeDate(uint64_t ft);

// Derived sets, all lower case.
std::set<std::wstring> FilterServices(const HwInfo& hw);      // in any class or device Upper/LowerFilters
std::set<std::wstring> BootChainServices(const HwInfo& hw);   // function drivers on the boot disk's parent chain

// Hot-plug classes are never auto-removed for being absent (spec §5.5).
bool IsHotPlugClass(const std::wstring& cls);

// Evaluates a KB hw hint for one item. Returns:
//   0 = hardware present or seen before (keep; evidence says which devnode),
//   1 = never seen and not hot-plug (may be removed),
//   2 = never seen but a hot-plug class (offer unticked),
//   3 = not hardware-bound (no hint) or the hint is unknown.
// Evidence text for a present or seen-before devnode names its last-arrival date when Windows recorded one.
int HwEvidence(const HwInfo& hw, const std::wstring& hint, const std::wstring& itemName, std::wstring* evidence);

// Device-interface trigger evidence (DECISIONS 2026-11-11 deviation, follow-up 8): 0 with *evidence when a device
// of a class the item's device-interface arrival triggers name is present now or was seen before (Windows would
// start the item for it), else -1 (no such evidence; *evidence untouched).
int IfaceEvidence(const HwInfo& hw, const SvcItem& item, std::wstring* evidence);
// HwEvidence, then tightened by IfaceEvidence: the result is HwEvidence's, or 0 (kept). It never turns a keep into
// a removal: interface triggers and arrival dates may only keep more (DECISIONS: "only make the guard stricter").
int HwEvidenceEx(const HwInfo& hw, const std::wstring& hint, const SvcItem& item, std::wstring* evidence);

}  // namespace hx::services
