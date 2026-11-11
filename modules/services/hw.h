// modules/services/hw.h — hardware evidence for the Stripped and Minimum presets (spec §5.5, r4 §7).
// The pure part works on a dump (tests/fixtures/services/hw.txt) so it is testable under Wine; the live
// part reads CfgMgr32 over every devnode, present and non-present, and is only called by the live backend.
#pragma once

#include "core/sys/win_compat.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hx::services {

struct DevNode {
    std::wstring id, service, cls, classGuid, parent;
    bool present = false;
    std::vector<std::wstring> upper, lower;   // device-level filters
};

struct HwInfo {
    bool ok = false;
    std::wstring error;
    std::vector<DevNode> nodes;
    std::map<std::wstring, std::pair<std::vector<std::wstring>, std::vector<std::wstring>>> classFilters;  // guid -> upper, lower
    std::wstring bootDisk;                    // devnode id of the boot disk ("" when unknown)
    std::set<std::wstring> presentIfaces;     // device-interface class GUIDs present now (lower case)
    bool hypervisor = false;                  // the hypervisor is running (VBS / Hyper-V)
    bool isVm = false;                        // this Windows is itself a VM guest (firmware vendor strings)
};

bool ParseHw(const std::string& utf8, HwInfo* out, std::wstring* err);
std::string DumpHw(const HwInfo& hw);
bool ReadLiveHw(HwInfo* out);   // CfgMgr32 + registry, read-only; sets out->error on failure

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
int HwEvidence(const HwInfo& hw, const std::wstring& hint, const std::wstring& itemName, std::wstring* evidence);

}  // namespace hx::services
