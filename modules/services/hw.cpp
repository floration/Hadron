// modules/services/hw.cpp — hardware evidence: hw.txt parser/dumper (pure) and the live CfgMgr32 reader.
// The live reader only reads (CfgMgr32, SetupAPI, registry KEY_QUERY_VALUE, a volume handle opened with
// no access rights for two IOCTLs). It never changes device state.
#include "modules/services/hw.h"
#include "modules/services/model.h"
#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>
#include <intrin.h>
#include <objbase.h>
#include <algorithm>
#include <functional>

namespace hx::services {

namespace {
// GUID_DEVINTERFACE_DISK, own name so no ntddstor.h/initguid ordering games.
const GUID kDiskIface = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> v;
    if (s.empty()) return v;
    for (auto& x : Split(s, L';')) if (!x.empty()) v.push_back(x);
    return v;
}

std::wstring Multi(const wchar_t* p, size_t cch) {   // REG_MULTI_SZ -> "a;b"
    std::vector<std::wstring> v;
    size_t i = 0;
    while (i < cch && p[i]) {
        std::wstring s(p + i);
        i += s.size() + 1;
        v.push_back(s);
    }
    return Join(v, L";");
}
}  // namespace

bool ParseHw(const std::string& utf8, HwInfo* out, std::wstring* err) {
    *out = HwInfo{};
    std::vector<std::wstring> lines = Split(FromUtf8(utf8), L'\n');
    if (lines.empty() || lines[0].rfind(L"HXSVC\t1\thw", 0) != 0) { *err = L"hw dump: missing HXSVC 1 hw header"; return false; }
    for (size_t i = 1; i < lines.size(); i++) {
        std::wstring l = lines[i];
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        if (l.empty() || l[0] == L'#') continue;
        auto f = Split(l, L'\t');
        if (f[0] == L"N" && f.size() >= 9) {
            DevNode n;
            n.id = Unesc(f[1]); n.service = f[2]; n.cls = f[3]; n.classGuid = Lower(f[4]);
            n.present = f[5] == L"1"; n.parent = Unesc(f[6]);
            n.upper = SplitList(f[7]); n.lower = SplitList(f[8]);
            if (f.size() >= 10 && !f[9].empty()) n.arrival = wcstoull(f[9].c_str(), nullptr, 16);
            out->nodes.push_back(std::move(n));
        } else if (f[0] == L"C" && f.size() >= 4) {
            out->classFilters[Lower(f[1])] = {SplitList(f[2]), SplitList(f[3])};
        } else if (f[0] == L"B" && f.size() >= 2) {
            out->bootDisk = Unesc(f[1]);
        } else if (f[0] == L"I" && f.size() >= 2) {
            out->presentIfaces.insert(Lower(f[1]));
        } else if (f[0] == L"F" && f.size() >= 3) {
            out->ifaceDevs[Lower(f[1])].push_back(Unesc(f[2]));
        } else if (f[0] == L"V" && f.size() >= 3) {
            if (f[1] == L"hypervisor") out->hypervisor = f[2] == L"1";
            if (f[1] == L"vm") out->isVm = f[2] == L"1";
        }
    }
    out->ok = true;
    return true;
}

std::string DumpHw(const HwInfo& hw) {
    std::wstring o = L"HXSVC\t1\thw\n";
    o += L"V\thypervisor\t" + std::wstring(hw.hypervisor ? L"1" : L"0") + L"\n";
    o += L"V\tvm\t" + std::wstring(hw.isVm ? L"1" : L"0") + L"\n";
    if (!hw.bootDisk.empty()) o += L"B\t" + Esc(hw.bootDisk) + L"\n";
    for (auto& c : hw.classFilters) o += L"C\t" + c.first + L"\t" + Join(c.second.first, L";") + L"\t" + Join(c.second.second, L";") + L"\n";
    for (auto& i : hw.presentIfaces) o += L"I\t" + i + L"\n";
    for (auto& g : hw.ifaceDevs)
        for (auto& d : g.second) o += L"F\t" + g.first + L"\t" + Esc(d) + L"\n";
    for (auto& n : hw.nodes) {
        o += L"N\t" + Esc(n.id) + L"\t" + n.service + L"\t" + n.cls + L"\t" + n.classGuid + L"\t" + (n.present ? L"1" : L"0") +
             L"\t" + Esc(n.parent) + L"\t" + Join(n.upper, L";") + L"\t" + Join(n.lower, L";");
        if (n.arrival) {   // only when known, so older dumps (and the fixture's other nodes) keep 9 fields
            wchar_t b[24];
            swprintf(b, 24, L"%016llX", (unsigned long long)n.arrival);
            o += L"\t" + std::wstring(b);
        }
        o += L"\n";
    }
    return ToUtf8(o);
}

std::set<std::wstring> FilterServices(const HwInfo& hw) {
    std::set<std::wstring> s;
    for (auto& c : hw.classFilters) {
        for (auto& x : c.second.first) s.insert(Lower(x));
        for (auto& x : c.second.second) s.insert(Lower(x));
    }
    for (auto& n : hw.nodes) {
        for (auto& x : n.upper) s.insert(Lower(x));
        for (auto& x : n.lower) s.insert(Lower(x));
    }
    return s;
}

std::set<std::wstring> BootChainServices(const HwInfo& hw) {
    std::set<std::wstring> s;
    if (hw.bootDisk.empty()) return s;
    std::map<std::wstring, const DevNode*> byId;
    for (auto& n : hw.nodes) byId[Lower(n.id)] = &n;
    std::wstring cur = Lower(hw.bootDisk);
    for (int guard = 0; guard < 64 && !cur.empty(); guard++) {
        auto it = byId.find(cur);
        if (it == byId.end()) break;
        const DevNode* n = it->second;
        if (!n->service.empty()) s.insert(Lower(n->service));
        for (auto& x : n->upper) s.insert(Lower(x));
        for (auto& x : n->lower) s.insert(Lower(x));
        cur = Lower(n->parent);
    }
    return s;
}

bool IsHotPlugClass(const std::wstring& cls) {
    static const wchar_t* const k[] = {L"bluetooth", L"printer", L"printqueue", L"image", L"camera", L"hidclass",
                                       L"xnacomposite", L"xboxcomposite", L"usb", L"wpd", L"media", L"smartcardreader",
                                       L"diskdrive", L"usbdevice"};
    std::wstring l = Lower(cls);
    for (auto* x : k) if (l == x) return true;
    return false;
}

int HwEvidence(const HwInfo& hw, const std::wstring& hint, const std::wstring& itemName, std::wstring* ev) {
    if (hint.empty()) { *ev = L"not hardware-bound"; return 3; }
    if (!hw.ok) { *ev = L"hardware list unavailable (" + hw.error + L"): kept"; return 0; }
    if (hint == L"vmguest") {
        if (hw.isVm) { *ev = L"this Windows runs as a virtual machine guest: kept"; return 0; }
        *ev = L"not a virtual machine (firmware vendor strings): guest integration is unused";
        return 1;
    }
    if (hint == L"hvhost") {
        if (hw.hypervisor) { *ev = L"the hypervisor is running (Hyper-V or VBS): kept"; return 0; }
        *ev = L"the hypervisor is not running";
        return 1;
    }
    std::wstring cls, svc;
    if (hint.rfind(L"class:", 0) == 0) cls = Lower(hint.substr(6));
    else if (hint.rfind(L"svc:", 0) == 0) svc = Lower(hint.substr(4));
    else if (hint == L"self") svc = Lower(itemName);
    else { *ev = L"unknown hardware hint '" + hint + L"': kept"; return 0; }   // fail closed
    const DevNode* present = nullptr;
    const DevNode* phantom = nullptr;
    for (auto& n : hw.nodes) {
        bool hit = false;
        if (!cls.empty()) hit = Lower(n.cls) == cls;
        else {
            hit = Lower(n.service) == svc;
            for (auto& x : n.upper) if (Lower(x) == svc) hit = true;
            for (auto& x : n.lower) if (Lower(x) == svc) hit = true;
        }
        if (!hit) continue;
        if (n.present) { present = &n; break; }
        if (!phantom) phantom = &n;
    }
    std::wstring what = cls.empty() ? L"service " + hint.substr(hint == L"self" ? 0 : 4) : L"class " + hint.substr(6);
    if (hint == L"self") what = L"service " + itemName;
    auto arrived = [](const DevNode* n) {
        std::wstring d = FileTimeDate(n->arrival);
        return d.empty() ? std::wstring() : L", last arrived " + d;
    };
    if (present) { *ev = L"present: " + present->id + L" (" + what + arrived(present) + L")"; return 0; }
    if (phantom) { *ev = L"seen before, not present now: " + phantom->id + L" (" + what + arrived(phantom) + L"): kept"; return 0; }
    bool hot = !cls.empty() && IsHotPlugClass(cls);
    if (hot) { *ev = L"no devnode for " + what + L" ever seen, but it is a hot-plug class: left unticked"; return 2; }
    *ev = L"no devnode for " + what + L" ever seen";
    return 1;
}

std::wstring FileTimeDate(uint64_t ft) {
    if (!ft) return {};
    FILETIME f{(DWORD)(ft & 0xFFFFFFFFu), (DWORD)(ft >> 32)};
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&f, &st)) return {};
    wchar_t b[16];
    swprintf(b, 16, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return b;
}

int IfaceEvidence(const HwInfo& hw, const SvcItem& item, std::wstring* ev) {
    if (item.ifaces.empty()) return -1;
    std::map<std::wstring, const DevNode*> byId;
    for (auto& n : hw.nodes) byId[Lower(n.id)] = &n;
    std::wstring seen;   // the first seen-before match, used when nothing is present
    for (auto& g0 : item.ifaces) {
        const std::wstring g = Lower(g0);
        auto it = hw.ifaceDevs.find(g);
        if (it != hw.ifaceDevs.end()) {
            for (auto& d : it->second) {
                auto n = byId.find(Lower(d));
                bool present = n != byId.end() && n->second->present;
                std::wstring date = n != byId.end() ? FileTimeDate(n->second->arrival) : std::wstring();
                std::wstring when = date.empty() ? std::wstring() : L", last arrived " + date;
                if (present) {
                    *ev = L"its device-interface trigger " + g + L" matches " + d + L" (present" + when + L"): kept";
                    return 0;
                }
                if (seen.empty())
                    seen = L"its device-interface trigger " + g + L" matches " + d + L" (seen before, not present now" + when + L"): kept";
            }
        }
        if (hw.presentIfaces.count(g)) {
            *ev = L"its device-interface trigger " + g + L" has a device present now: kept";
            return 0;
        }
    }
    if (seen.empty()) return -1;
    *ev = seen;
    return 0;
}

int HwEvidenceEx(const HwInfo& hw, const std::wstring& hint, const SvcItem& item, std::wstring* ev) {
    int h = HwEvidence(hw, hint, item.name, ev);
    if (h == 0) return 0;   // already kept: nothing can loosen it
    std::wstring iev;
    if (IfaceEvidence(hw, item, &iev) == 0) {
        *ev = h == 3 ? iev : iev + L"; by the hint alone: " + *ev;
        return 0;
    }
    return h;
}

// ---- live (CfgMgr32, read-only) -------------------------------------------------------------------
namespace {
std::wstring RegProp(DEVINST dn, ULONG prop, bool multi) {
    ULONG type = 0, len = 0;
    CONFIGRET cr = CM_Get_DevNode_Registry_PropertyW(dn, prop, &type, nullptr, &len, 0);
    if (cr != CR_BUFFER_SMALL || len == 0) return {};
    std::vector<wchar_t> buf(len / sizeof(wchar_t) + 2, 0);
    len = (ULONG)(buf.size() * sizeof(wchar_t));
    if (CM_Get_DevNode_Registry_PropertyW(dn, prop, &type, buf.data(), &len, 0) != CR_SUCCESS) return {};
    if (multi) return Multi(buf.data(), len / sizeof(wchar_t));
    return std::wstring(buf.data());
}

std::wstring RegMulti(HKEY root, const std::wstring& path, const wchar_t* value) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return {};
    DWORD type = 0, cb = 0;
    std::wstring out;
    if (RegQueryValueExW(k, value, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS && type == REG_MULTI_SZ && cb) {
        std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 2, 0);
        if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &cb) == ERROR_SUCCESS)
            out = Multi(buf.data(), cb / sizeof(wchar_t));
    }
    RegCloseKey(k);
    return out;
}

std::wstring RegStr(HKEY root, const wchar_t* path, const wchar_t* value) {
    wchar_t buf[512] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, path, value, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return {};
    return buf;
}

// DEVPKEY_Device_LastArrivalDate, own copy (devpkey.h needs initguid ordering): when the device last arrived. Read-only;
// 0 when Windows has no value (older devices, or a property this Windows doesn't keep).
const DEVPROPKEY kLastArrival = {{0x83da6326, 0x97a6, 0x4088, {0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29}}, 102};

uint64_t LastArrival(DEVINST dn) {
    DEVPROPTYPE type = 0;
    FILETIME ft{};
    ULONG cb = sizeof(ft);
    if (CM_Get_DevNode_PropertyW(dn, &kLastArrival, &type, reinterpret_cast<PBYTE>(&ft), &cb, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_FILETIME || cb != sizeof(ft))
        return 0;
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

std::wstring DevId(DEVINST dn) {
    wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
    if (CM_Get_Device_IDW(dn, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
    return id;
}

// The boot disk's devnode: the system volume's first extent disk number, matched against every disk interface.
std::wstring BootDiskId(std::wstring* err) {
    wchar_t win[MAX_PATH] = {};
    if (!GetSystemWindowsDirectoryW(win, MAX_PATH) || win[1] != L':') { *err = L"no Windows drive"; return {}; }
    std::wstring vol = L"\\\\.\\" + std::wstring(win, 2);
    HANDLE h = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = L"open volume failed"; return {}; }
    BYTE ext[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {};
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, ext, sizeof(ext), &got, nullptr);
    CloseHandle(h);
    if (!ok) { *err = L"volume extents unavailable"; return {}; }
    DWORD disk = reinterpret_cast<VOLUME_DISK_EXTENTS*>(ext)->Extents[0].DiskNumber;
    HDEVINFO set = SetupDiGetClassDevsW(&kDiskIface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { *err = L"disk interfaces unavailable"; return {}; }
    std::wstring found;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &kDiskIface, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need + 8);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &dev)) continue;
        HANDLE d = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (d == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER num{};
        if (DeviceIoControl(d, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &num, sizeof(num), &got, nullptr) &&
            num.DeviceNumber == disk)
            found = DevId(dev.DevInst);
        CloseHandle(d);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (found.empty()) *err = L"boot disk devnode not found";
    return found;
}
}  // namespace

bool ReadLiveHw(HwInfo* out) {
    *out = HwInfo{};
    // A device can arrive between the size and the list call: retry on CR_BUFFER_SMALL.
    std::vector<wchar_t> ids;
    CONFIGRET cr = CR_BUFFER_SMALL;
    for (int tries = 0; tries < 5 && cr == CR_BUFFER_SMALL; tries++) {
        ULONG len = 0;
        cr = CM_Get_Device_ID_List_SizeW(&len, nullptr, CM_GETIDLIST_FILTER_NONE);
        if (cr != CR_SUCCESS || !len) {
            out->error = L"CM_Get_Device_ID_List_Size failed (CONFIGRET " + std::to_wstring(cr) + L")";
            return false;
        }
        ids.assign(len + 2, 0);
        cr = CM_Get_Device_ID_ListW(nullptr, ids.data(), len, CM_GETIDLIST_FILTER_NONE);
    }
    if (cr != CR_SUCCESS) {
        out->error = L"CM_Get_Device_ID_List failed (CONFIGRET " + std::to_wstring(cr) + L")";
        return false;
    }
    std::set<std::wstring> guids;
    for (size_t i = 0; i < ids.size() && ids[i];) {
        std::wstring id(&ids[i]);
        i += id.size() + 1;
        DEVINST dn = 0;
        DevNode n;
        n.id = id;
        n.present = CM_Locate_DevNodeW(&dn, &id[0], CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
        if (!n.present && CM_Locate_DevNodeW(&dn, &id[0], CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS) continue;
        n.service = RegProp(dn, CM_DRP_SERVICE, false);
        n.cls = RegProp(dn, CM_DRP_CLASS, false);
        n.classGuid = Lower(RegProp(dn, CM_DRP_CLASSGUID, false));
        n.upper = Split(RegProp(dn, CM_DRP_UPPERFILTERS, true), L';');
        n.lower = Split(RegProp(dn, CM_DRP_LOWERFILTERS, true), L';');
        n.upper.erase(std::remove(n.upper.begin(), n.upper.end(), L""), n.upper.end());
        n.lower.erase(std::remove(n.lower.begin(), n.lower.end(), L""), n.lower.end());
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, dn, 0) == CR_SUCCESS) n.parent = DevId(parent);
        n.arrival = LastArrival(dn);
        if (!n.classGuid.empty()) guids.insert(n.classGuid);
        out->nodes.push_back(std::move(n));
    }
    for (auto& g : guids) {
        std::wstring path = L"SYSTEM\\CurrentControlSet\\Control\\Class\\" + g;
        auto up = Split(RegMulti(HKEY_LOCAL_MACHINE, path, L"UpperFilters"), L';');
        auto lo = Split(RegMulti(HKEY_LOCAL_MACHINE, path, L"LowerFilters"), L';');
        up.erase(std::remove(up.begin(), up.end(), L""), up.end());
        lo.erase(std::remove(lo.begin(), lo.end(), L""), lo.end());
        if (!up.empty() || !lo.empty()) out->classFilters[g] = {up, lo};
    }
    std::wstring berr;
    out->bootDisk = BootDiskId(&berr);
    int regs[4] = {};
    __cpuid(regs, 1);
    out->hypervisor = (regs[2] & (1 << 31)) != 0;
    std::wstring man = Lower(RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer"));
    std::wstring prod = Lower(RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName"));
    out->isVm = (man.find(L"microsoft") != std::wstring::npos && prod.find(L"virtual") != std::wstring::npos) ||
                man.find(L"vmware") != std::wstring::npos || man.find(L"qemu") != std::wstring::npos ||
                man.find(L"innotek") != std::wstring::npos || prod.find(L"virtualbox") != std::wstring::npos ||
                man.find(L"xen") != std::wstring::npos || man.find(L"parallels") != std::wstring::npos;
    out->ok = true;
    if (out->bootDisk.empty()) out->error = L"boot disk: " + berr;
    return true;
}

// Interface-trigger evidence: for each device-interface class GUID a service's arrival trigger names, every device
// that registered an interface of that class, present or not (no DIGCF_PRESENT). Read-only SetupDi enumeration; a
// GUID that fails to enumerate just adds nothing (the evidence can only keep more, so missing data never loosens).
void AddLiveIfaceEvidence(const std::vector<SvcItem>& items, HwInfo* hw) {
    std::set<std::wstring> guids;
    for (auto& i : items) for (auto& g : i.ifaces) guids.insert(Lower(g));
    for (auto& g : guids) {
        GUID guid{};
        if (CLSIDFromString(g.c_str(), &guid) != NOERROR) continue;
        HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring>& devs = hw->ifaceDevs[g];
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        for (DWORD k = 0; k < 4096 && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, k, &ifd); k++) {
            SP_DEVINFO_DATA dev{};
            dev.cbSize = sizeof(dev);
            DWORD need = 0;
            // Only the device data is wanted: a null detail buffer with the size call fills dev on the documented
            // ERROR_INSUFFICIENT_BUFFER path.
            if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, &dev) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                continue;
            if (!dev.DevInst) continue;
            std::wstring id = DevId(dev.DevInst);
            if (!id.empty() && std::find(devs.begin(), devs.end(), id) == devs.end()) devs.push_back(id);
        }
        SetupDiDestroyDeviceInfoList(set);
        if (devs.empty()) hw->ifaceDevs.erase(g);
    }
}

}  // namespace hx::services
