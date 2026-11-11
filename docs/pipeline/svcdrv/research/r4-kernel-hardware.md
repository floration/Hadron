# svcdrv research R4: kernel, drivers, firmware and hardware

Scope: what the Services/Drivers page (docs/pipeline/svcdrv/1-idea.md) must know about the boot path, hardware-dependent
drivers, power/hybrid CPUs, filter stacks, VBS/Hyper-V and anti-cheat, hardware detection, and how honest any
performance claim can be. Target box: Win10 22H2 (19045), MSI PRO Z790-P WIFI (MS-7E06), i7-14700KF (no iGPU), dGPU.
Research only: no repo changes.

Basis tags follow CLAUDE.md rule 7: **[published]** with a URL, **[reasoning]** from how the system works with no data
quoted, **[folklore]** repeated but without evidence. Where something must be checked on the owner's PC I say **verify on device**.

---

## 1. Boot driver load sequence (what actually happens)

1. **Firmware → bootmgfw.efi → winload.efi.** Winload loads the SYSTEM hive, reads `Select\Current` to pick
   `ControlSet00n`, and loads into memory every driver with `Start=0` (SERVICE_BOOT_START) plus the boot file system
   driver, together with HAL, kernel and boot-start dependencies (CI.dll, etc.). These run before the kernel has any
   disk I/O of its own. If the driver for the boot disk path isn't loaded here, the kernel can't mount the system
   volume and stops with **0x7B INACCESSIBLE_BOOT_DEVICE**. [published]
   https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x7b--inaccessible-boot-device
2. **Kernel phase 1 / IoInitSystem.** The boot-start drivers are initialised in **group order**
   (`HKLM\SYSTEM\CurrentControlSet\Control\ServiceGroupOrder\List`), and within a group in **Tag** order
   (`Control\GroupOrderList\<group>`, matching each service's `Tag` value). PnP enumerates the device tree
   (ACPI → PCI → storage → disk → partmgr → volmgr → volumes), attaches the class filters, and the boot volume is mounted. [published]
   https://learn.microsoft.com/en-us/windows-hardware/drivers/install/hklm-system-currentcontrolset-services-registry-tree ,
   https://winreg-kb.readthedocs.io/en/latest/sources/system-keys/Services-and-drivers.html ,
   https://techshelps.github.io/MSDN/KB/Source/win32ddk/q115486.htm (classic "control device driver load order")
3. **System-start (`Start=1`).** The I/O manager loads these after the boot drivers, again in group/Tag order. This
   covers much of networking (NDIS, tcpip, afd), the Filter Manager's minifilters in the FSFilter groups, and many class drivers. [published, same refs]
4. **SMSS → wininit → services.exe (SCM).** SCM starts `Start=2` auto services (in group order for `Group`
   members, then the rest), then delayed-auto (about 2 minutes later), and trigger-start services on their triggers. [published]
   https://learn.microsoft.com/en-us/windows/win32/services/automatically-starting-services
5. **PnP demand-loads `Start=3` drivers** when it finds a matching device, whatever their start value: a PnP function
   driver is loaded because a device needs it. `Start=4` stops even that, so the device shows Code 32/Code 31. [reasoning,
   consistent with Microsoft's AddService docs https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addservice-directive]

### ErrorControl and why it no longer protects you
- `0` Ignore, `1` Normal (log and continue), `2` Severe (switch to LastKnownGood and reboot, else continue),
  `3` Critical (switch to LKG; if LKG already failed, bugcheck). [published, ref above/Win32_SystemDriver docs
  https://msdn.microsoft.com/en-us/library/aa389394(v=vs.85)]
- Since Windows 8 the **LastKnownGood** boot option is hidden and not maintained by default. Do not count on LKG
  to save a bad preset. [reasoning, from Windows Internals; verify on device whether
  `Control\Session Manager\Configuration Manager\LastKnownGood\Enabled` exists. I did not find a Learn page to cite]
- Safe Mode is **not** a dependable way back either. A `Start=4` driver is still not loaded in Safe Mode. Microsoft's
  0x7B article says Safe Mode only reverses the start-type *demotion that Windows itself made* (see §1.1). [published]
  https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/inaccessible-boot-device-stop-error
- **Consequence for the recovery .zip:** restore offline from WinRE. Run `reg load HKLM\OFF <win>\System32\config\SYSTEM`,
  read `OFF\Select\Current` to find the active `ControlSet00n` (there is no `CurrentControlSet` offline), and write
  the snapshot values into that set. In WinRE the Windows volume is often not `C:`, so the script must find it by
  probing for `\Windows\System32\config\SYSTEM` on each letter. Scripts check admin with `fltmc` (rule 4), but WinRE is
  already SYSTEM. [reasoning]

### 1.1 Windows rewrites boot-driver start types itself (important for the "Windows stock" table)
Windows demotes boot-start drivers it judges unnecessary from Boot to Demand, which is "by design". So on one PC
`storahci` may be `0` and on another `3`, depending on which controller holds the boot disk. [published, MS 0x7B article above]
**Implication:** the per-build "Windows stock" table **must not** set storage/boot-class start values. For those,
the first-run snapshot is the only truth. If the stock table "restores" `stornvme=3` on an NVMe-boot box, the next boot
fails with 0x7B. [reasoning]

---

## 2. "Never touch" list (no preset, no per-item override without a separate hard-confirm; ideally hard-blocked)

Rule for the page: **a driver is never-touch if any of these holds** (all can be checked on the device):
(a) `Start=0` (already in the idea); (b) its service name appears in any `UpperFilters`/`LowerFilters` value (class
key `HKLM\SYSTEM\CurrentControlSet\Control\Class\{guid}` or a device's hardware key); (c) it is the function driver
(`Service`) of a **started** device on the boot path (the boot disk's ancestors in the devnode tree); (d) it is on the
curated list below. Rule (b) matters most and is the least known. If a filter listed in a class's `UpperFilters` can't
load, every device of that class fails to start. For the Volume, Disk or SCSIAdapter classes that means 0x7B; for
Keyboard/Mouse it means no input at the logon screen. [reasoning; many incident reports exist (e.g. disabling `rdyboost` or
uninstalling a third-party keyboard filter that left `kbdclass` filters behind) but I found no Microsoft page stating it
outright. kb-auditor should label it reasoning.] Filter properties: https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-deviceclass-upperfilters ,
https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-upperfilters

| Driver(s) | Why never | Basis |
|---|---|---|
| `ACPI`, `acpiex`, `pci`, `pdc`, `msisadrv`, `intelpep`/`acpipagr` (if present) | Root of the device tree / power dependency coordinator; no ACPI or PCI means no devices at all | reasoning |
| `storport`, `stornvme`, `storahci`, `iaStorAC`/`iaStorAVC`/`iaStorVD` (RST, only if installed), `iaVROC` / VMD driver | Boot disk path; disabling one in use gives 0x7B. On Z790 VMD is a BIOS option; if it's on, the boot disk sits behind `iaStorVD` and `stornvme` is unused. **Verify on device** which is the boot path | published (0x7B), reasoning for mapping |
| `disk`, `partmgr`, `volmgr`, `volmgrx`, `volsnap`, `mountmgr`, `fvevol`, `rdyboost`, `iorate`, `EhStorClass`, `spaceport` | Disk/volume stack. `fvevol`, `rdyboost`, `volsnap`, `iorate` are in Volume/Disk class `UpperFilters` on stock Win10. `fvevol` is needed even with BitLocker off because it sits in the stack | reasoning (verify the filter lists on device) |
| `Ntfs`, `FltMgr`, `Fs_Rec`, `Wof`, `CLFS`, `CimFS` (if present) | Boot FS; Filter Manager hosts every minifilter. `Wof` is required on CompactOS/compressed-system installs; without it the system files are unreadable | reasoning |
| `cng`, `ksecdd`, `ksecpkg`, `CI` (not a service), `Wdf01000`, `WdfLdr`(older), `msrpc`, `tm`, `Null`, `Beep`(harmless), `Npfs`, `Msfs`, `Mup` | Crypto, KMDF runtime (most modern drivers are KMDF), named pipes and mailslots (nearly all IPC), UNC provider | reasoning |
| `NDIS`, `tcpip`, `tcpip6`(stub), `afd`, `NetIO`, `nsiproxy`, `tdx`, `WFPLWFS`, `mpsdrv`, `bfe` (service) | Networking. `afd` is the Winsock kernel side; without it no sockets, so Hadron's own updates, Battle.net etc. fail. Firewall drivers: disabling BFE removes WFP, which leaves no firewall and breaks IPsec/some VPNs | reasoning |
| `usbxhci`, `USBHUB3`, `UsbHub3`/`usbhub`, `hidusb`, `HidUsb`, `kbdhid`, `mouhid`, `kbdclass`, `mouclass`, `i8042prt` (if a PS/2 port exists) | Keyboard and mouse. Disabling these at logon leaves the owner unable to do anything without WinRE | reasoning |
| `DXGKrnl`, `BasicDisplay`, `BasicRender`, the GPU's driver (`nvlddmkm` / `amdkmdag`) | Display. The dGPU driver is third-party, so it's never in a preset anyway | reasoning |
| `intelppm` (and `amdppm`, `processr` as present), `Power` service, `pdc` | Processor P/C-state driver; see §4 | reasoning + published refs in §4 |
| `WdBoot`, `WdFilter`, `WdNisDrv` | Defender ELAM/minifilter. Protected (PPL/ELAM): admin can't change them; TI can, but tampering here belongs to its own explicit flow, not a preset | reasoning |
| `FileInfo`, `FileCrypt`, `bindflt`, `wcifs`, `CldFlt` (if OneDrive Files On-Demand is in use) | Filters other components depend on; see §5 | reasoning |
| `hvservice`, `vmbus`, `Vid`, `hvsocket`/`hvcrash`, `vmgencounter` **while VBS/HVCI or the hypervisor is running** | Pulling hypervisor drivers while `hypervisorlaunchtype=Auto` and VBS is on is not a supported way to turn VBS off; see §6 | reasoning |
| `mssmbios`, `MEIx64`/`HECI` (see §3), `Smb`/SMBus — **also Hadron's own deps** | Hadron's BIOS/SMBIOS modules read SMBIOS through `mssmbios`; don't let "Minimum" break Hadron itself | reasoning |

Hard rule 2 note: nothing here writes firmware. The page's only interaction with firmware is reading SMBIOS to
identify the board. That's allowed.

---

## 3. Hardware-dependent drivers on Z790 + Raptor Lake + Win10 22H2

Board facts (MSI spec page: Intel 2.5G LAN, Intel Wi-Fi 6E M.2 Key-E module with BT 5.3, Realtek ALC897, 6× SATA,
4× M.2 with RAID, no VMD mentioned): https://www.msi.com/Motherboard/PRO-Z790-P-WIFI/Specification [published].
Exact chip SKUs (I226-V? AX211 CNVi or AX210 PCIe?) aren't on the page, so **verify on device** by enumeration. Never
hard-code service names below. Treat them as likely names and map them through the devnode → `Service` lookup (§7).

| Function | Likely service(s) | What breaks if disabled | Notes |
|---|---|---|---|
| NVMe (M.2_1 CPU-attached, M.2_2–4 chipset) | `stornvme` (inbox) or vendor (`secnvme`, etc.); `iaStorVD` if VMD/RST | Boot (0x7B) if it's the boot disk; otherwise that drive disappears | never-touch when it serves the boot disk |
| SATA | `storahci` (inbox) | Same, for SATA disks | Windows may have demoted it to `3` (§1.1) |
| USB xHCI (Z790 PCH + CPU Type-C?) | `USBXHCI`, `USBHUB3`, `UCX01000`, `UcmUcsiCx`/`UcmUcsiAcpiClient` (Type-C, if present) | All USB, so keyboard, mouse, headset and BT | never-touch |
| Intel MEI / CSME | `MEIx64` (Intel driver) or inbox `HECIx64`, plus services `LMS`, `jhi_service`, `Intel(R) Management Engine WMI`, `WMIRegistrationService` | Disabling the **driver**: firmware-update tools and Intel PTT (fTPM) related user-mode tools stop working; the TPM itself is CSME firmware, not this driver, so BitLocker/TPM keep working [reasoning]. Disabling **LMS/AMT services** on a consumer board without AMT: nothing visible [reasoning] | LMS/jhi are legitimately "Stripped" candidates; the MEI driver is not (low gain, BIOS update tools need it) |
| Intel Serial IO (I²C/GPIO/UART/SPI of the PCH) | `iaLPSS2_GPIO2_ADL`, `iaLPSS2_I2C_ADL`, `iaLPSS2_UART2_ADL`, `iaLPSS2_SPI_ADL` (names vary with package) | Desktop boards rarely hang anything user-facing off LPSS I²C, but GPIO can back ACPI events/buttons. Disabling usually just leaves "unknown device" [reasoning] | only if the devnode shows no children; **verify on device** |
| Intel chipset INF devices | No driver ("Intel SMBus", "SPI flash", "PCI Express Root Port" use inbox `pci`) | n/a | the chipset INF only names devices |
| Intel 2.5G LAN | `e2f68`/`e2fexpress`/`e2fn` (I225/I226 family; name depends on driver version) | Wired networking | never in Minimum (Minimum must keep "network") |
| Intel Wi-Fi 6E | `Netwtw10`/`Netwtw12`/`Netwtw14` (version-dependent), service `WlanSvc` | Wi-Fi. If the owner is wired, Stripped may disable **WlanSvc** (service) after detecting no Wi-Fi profile in use; leave the driver alone (device-level disable is the right tool) | **verify which link he uses** |
| Intel Bluetooth (USB-attached on the M.2 module) | `ibtusb`, `BTHUSB`, `BthEnum`, `BthLEEnum`, `BthMini`, `BthA2dp`, `BthHFEnum`, `RFCOMM`, `BthPan`; services `bthserv`, `BTAGService`, `BluetoothUserService_*` | BT keyboard/mouse/headset/controller. If he uses a BT headset or controller, Stripped must not touch these | detection: a BT radio being *present* is not the same as BT being *used*. Look for paired-device devnodes under `BTHENUM` (phantoms count as "used before") |
| HD Audio | `HDAudBus`, `HdAudAddService`/Realtek `IntcAzAudAddService` (RTKVHD64), `IntcDAud` (Intel display audio, absent here: no iGPU), NVIDIA `NVHDA` for HDMI audio; services `Audiosrv`, `AudioEndpointBuilder`, `MMCSS` | Sound. `AudioEndpointBuilder`/`Audiosrv` are never-touch for a gaming "Minimum" | `IntcDAud` absent on KF is a clean example of hardware-absent |
| iGPU | `igfx*`, `cphs`, `igccservice`, Intel GNA `gna` | **Absent on 14700KF**; present only as stale packages | clean "hardware absent" Stripped example |
| TPM | `TPM` (inbox), `tbs` service | BitLocker, Windows Hello, some anti-cheats (Vanguard on Win11 checks TPM) [reasoning] | never-touch |
| Thunderbolt / USB4 | none on this board (no TBT header in the spec) | n/a | hardware-absent if no devnode |

**Hybrid-specific Win10 caveat:** see §4. Windows 10 has no Thread Director support, so there is no "hybrid driver"
to keep or remove. The scheduling difference sits in the kernel (ntoskrnl), not in a service.

---

## 4. Hybrid P/E cores, power management, and why Minimum keeps them

- **Thread Director on Win10:** Intel and Microsoft present Windows 11 as the OS that uses Thread Director's HFI
  feedback; Windows 10 schedules hybrid parts with older logic (efficiency classes but no per-thread HFI hints). [published]
  https://at-web1.www.anandtech.com/show/16959/intel-innovation-alder-lake-november-4th/3 ,
  https://techpowerup.com/review/alder-lake-windows-10-performance . Raptor Lake on Win10 **22H2 vs 21H2**: one review
  measured big gains in a few titles (CS:GO up to 57% avg fps), around 1% elsewhere, and some workloads still not using
  P-cores properly [published, 13900K, not the owner's CPU]:
  https://www.hwcooling.net/en/older-windows-10-versions-than-22h2-slow-down-raptor-lake-cpus/6/
- **What actually carries P/E behaviour on Win10:** the kernel's power manager (PPM) plus `intelppm.sys`, which drives
  HWP/EPP (Speed Shift) and C-state entry, and the active power scheme's processor settings (core parking, heterogeneous
  policy, EPP), applied by the **Power** service (`Power`, hosted in svchost). [reasoning; documented knobs are in the
  processor power management settings https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/configure-processor-power-management-options]
- **Disabling `intelppm`** has been passed around as a "latency tweak". With it disabled, Windows falls back to the
  generic `processr` path. HWP/EPP hints and the deeper C-states it handles may stop being managed as designed.
  Results are hardware- and BIOS-dependent: higher idle power and temperature, possibly lower turbo residency, and on
  hybrid parts the effect on core parking/scheduling isn't predictable. [folklore for the latency benefit;
  reasoning for the risks.] If the owner wants fewer C-state exits, the honest tool is the power plan or BIOS C-state
  options, measured, not removing the driver. Minimum **keeps** `intelppm`.
- **Disabling the `Power` service** breaks power plans, sleep/hibernate policy and battery/AC transitions, and any
  later change to processor policy. Minimum keeps it. [reasoning]
- **HPET / timers:** Windows QPC uses the invariant TSC on modern Intel CPUs. `bcdedit /set useplatformclock true`
  forces the HPET as the QPC source, which makes QPC calls far slower. "Disabling the HPET device" in Device Manager
  changes nothing about QPC on TSC systems. [published for QPC sources:
  https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps ; folklore: "HPET
  off/on gives more FPS" (community runs disagree, e.g. https://www.techpowerup.com/forums/threads/hpet-on-off-capframex-analysis.270732
  is a forum measurement, not controlled)]. None of this is a service/driver toggle (it's BCD and device state), so
  it's out of scope for this page; the KB should say so if asked.
- Other common "timer" folklore that touches drivers: disabling `ACPI`'s "High precision event timer" device,
  `amdppm`/`intelppm`, `MMCSS`, `Beep`. Only `Beep` is harmless and pointless. Label the rest folklore with
  "test it: PresentMon/CapFrameX 1% lows + WPR DPC/ISR trace, 3 runs each, same scene".

---

## 5. Filter drivers and the dependency trap

Filter Manager minifilters load in FSFilter groups at fixed altitudes. Several stock components look optional
but other features sit on top of them. Altitude/group list: [published]
https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/load-order-groups-and-altitudes-for-minifilter-drivers ,
https://learn.microsoft.com/en-us/archive/blogs/ntdebugging/understanding-file-system-minifilter-and-legacy-filter-load-order

| Filter | What depends on it | Disable? |
|---|---|---|
| `FileInfo` | Prefetcher/Superfetch (SysMain), and Defender, which query file metadata through it [reasoning] | never-touch |
| `FileCrypt` | Store/UWP app packaging encryption (packaged apps, sandboxed containers) [reasoning] | never-touch |
| `bindflt` (bind filter) | MSIX/Store app layering, Windows Sandbox, containers; some Store games/Xbox app [reasoning] | never-touch unless the owner explicitly drops all Store/MSIX use. Not in any preset |
| `wcifs` (Windows Container Isolation) | Containers, some MSIX virtualisation [reasoning] | as bindflt |
| `luafv` (UAC file virtualisation) | Legacy 32-bit apps that write to Program Files / Windows without a manifest. Disabling makes those writes fail with access denied [reasoning] | Stripped at most, with an explicit note. Not "bloat" |
| `storqosflt` | Storage QoS (Hyper-V/cluster). Inert on a desktop [reasoning] | Stripped if no Hyper-V (detected) |
| `WdFilter` | Defender real-time. Protected; tamper protection reverts it | out of presets; TI-toggle territory, owner's call |
| `CldFlt` | OneDrive/cloud Files On-Demand placeholders. Disabling makes placeholder files unreadable | Hadron's rule 1 itself cares about cloud files; never in a preset unless no sync root is registered (detect via `HKLM\...\SyncRootManager`) |
| `Wof` | Compressed/CompactOS system files | never-touch |
| `npsvctrig`, `Ndu`, `bam` | Named-pipe service triggers; network data usage; background activity moderator | `Ndu`: had real memory-leak reports on older builds [folklore for current builds]; `npsvctrig`: disabling breaks trigger-start services that wait on named pipes [reasoning]. Keep out of Bloat; Stripped only with basis |

**Implementation rule:** before planning a filter change, enumerate live attachments (`FilterFindFirst`/`FilterInstanceFindFirst`
from fltlib, which is part of Windows, so rule 3 allows it), so the plan shows the volumes the filter is attached to. Never
disable a filter that also appears in a class `UpperFilters`/`LowerFilters` value (§2 rule b).

---

## 6. VBS, HVCI, Hyper-V and anti-cheat

- **State detection:** read `Win32_DeviceGuard` (WMI `root\Microsoft\Windows\DeviceGuard`) or
  `HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity\Enabled`, and BCD
  `hypervisorlaunchtype`. On a Win10 22H2 upgrade install, Memory Integrity is usually off by default [reasoning; verify on device].
- **Performance evidence:** Microsoft itself advises that turning off Memory Integrity and Virtual Machine Platform can
  improve gaming on Win11 [published, reported: https://www.tomshardware.com/news/microsoft-windows-11-gaming-boost-tips ;
  https://www.techspot.com/news/96246-microsoft-releases-guide-optimize-windows-11-gaming-performance.html].
  Tom's Hardware measured the VBS/HVCI cost as small on average with per-title variance (Win11, their rigs, not the owner's) [published]:
  https://www.tomshardware.com/news/windows-11-gaming-benchmarks-performance-vbs-hvci-security/ . Workload-dependent,
  so the KB says "test it" with the owner's game and metric.
- **The service page is the wrong lever for this.** VBS/HVCI is controlled by the Windows Security toggle / DeviceGuard
  registry plus BCD `hypervisorlaunchtype`, and Hyper-V by optional features. Disabling `hvservice`, `Vid`, `vmbus`,
  `vmcompute` (HCS), `hvhost` etc. while the hypervisor still launches leaves features half-dead (WSL2, Windows Sandbox,
  Docker, Android Subsystem, VBS-based Credential Guard) and can make VBS fail to start at boot [reasoning]. Recommendation: Stripped may
  disable Hyper-V *guest/integration* services (`vmicheartbeat`, `vmickvpexchange`, `vmicshutdown`, `vmictimesync`,
  `vmicvss`, `vmicguestinterface`, `vmicrdv`, `vmicvmsession`). They're for when Windows is itself a VM, and are inert on bare
  metal [reasoning]. Hypervisor-host drivers: Stripped shows "use Windows Features / Memory Integrity toggle" with a
  link, and changes nothing while `hypervisorlaunchtype != Off` or VBS is running.
- **Detect "is this a VM":** SMBIOS system manufacturer/product (Hadron already parses SMBIOS) and CPUID hypervisor bit
  (leaf 1 ECX bit 31; note the bit is **also set on bare metal when VBS/Hyper-V is running**, because the host OS is then a root partition. Use the SMBIOS manufacturer, not CPUID alone) [reasoning].
- **Anti-cheat:** third-party anti-cheat drivers (EAC, BattlEye, Vanguard `vgk`, FACEIT, Ricochet) are never in any
  preset (they're third-party, so they get no explanation and no preset by design). Overwatch 2's anti-cheat: I could not
  find a primary source saying it uses a kernel driver; Blizzard forum threads only discuss user-mode/Linux compatibility
  (https://us.forums.blizzard.com/en/overwatch/t/linux-anti-cheat-reference/937155). **Verify on device**: enumerate loaded
  drivers (`EnumDeviceDrivers`) while OW2 runs and diff. Anti-cheats in general refuse to run with test-signing,
  kernel debugging or HVCI-incompatible tampering. Disabling VBS does not break them, but some (Vanguard on Win11, FACEIT)
  have demanded TPM 2.0/Secure Boot or fixed specific VBS settings over time [reasoning; the requirements change, so state the date].
  Disabling `TPM`/`tbs` can therefore lock him out of such games; another reason they're never-touch.
- HVCI and drivers: Memory Integrity refuses to load incompatible drivers. If the owner later turns HVCI on, old
  third-party drivers (some RGB/fan tools, WinRing0 users) fail to load [published]
  https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/driver-compatibility-with-device-guard . Not
  this page's job, but the detail pane could show "HVCI-blocked" for drivers in Code 39 with event 3089 [reasoning].

---

## 7. Hardware detection: how Stripped knows the hardware is absent

Goal: map every driver service to the devices it serves, and every trigger-start service to the device interfaces
that trigger it, then decide **present / present-but-unused / seen before (phantom) / never seen / not hardware-bound**.

1. **Enumerate all devnodes, including phantoms:** `CM_Get_Device_ID_List_SizeW` / `CM_Get_Device_ID_ListW` with no filter (all
   instances in the registry), then `CM_Locate_DevNodeW(..., CM_LOCATE_DEVNODE_PHANTOM)`. A devnode that locates only
   with PHANTOM is non-present. SetupAPI equivalent: `SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES)` with
   and without `DIGCF_PRESENT`. [published]
   https://learn.microsoft.com/en-us/windows-hardware/drivers/install/enumerating-installed-devices ,
   https://learn.microsoft.com/windows/win32/api/setupapi/nf-setupapi-setupdigetclassdevsw ,
   https://learn.microsoft.com/en-us/windows-hardware/drivers/install/porting-from-setupapi-to-cfgmgr32 (prefer CfgMgr32: lighter, no class installers)
2. **Per devnode read** via `CM_Get_DevNode_PropertyW`: `DEVPKEY_Device_Service` (function driver service name), `DEVPKEY_Device_UpperFilters`,
   `DEVPKEY_Device_LowerFilters`, `DEVPKEY_Device_ClassGuid` (then the class's `DEVPKEY_DeviceClass_UpperFilters/LowerFilters`),
   `DEVPKEY_Device_Parent` (to walk the boot-disk path), `DEVPKEY_Device_ProblemCode`, `CM_Get_DevNode_Status` (`DN_STARTED`),
   and `DEVPKEY_Device_LastArrivalDate` / `LastRemovalDate` to show "last seen". [published]
   https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-service ,
   https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-upperfilters ,
   https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-deviceclass-upperfilters
3. **Trigger-start services:** `QueryServiceConfig2W(SERVICE_CONFIG_TRIGGER_INFO)`. Triggers of type
   `SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL` carry an interface-class GUID and often a hardware-ID string.
   Match these against present interfaces (`CM_Get_Device_Interface_ListW` with `CM_GET_DEVICE_INTERFACE_LIST_PRESENT`).
   This is how a *service* (e.g. Bluetooth, sensor, biometric, smart card) is tied to hardware. [published]
   https://learn.microsoft.com/windows/win32/api/winsvc/ns-winsvc-service_trigger ,
   https://learn.microsoft.com/en-ie/Windows/Win32/services/service-trigger-events
4. **Boot path:** `GetVolumeNameForVolumeMountPointW(%SystemDrive%)` gives the volume, then `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`
   gives the disk number, then `CM_Get_Device_Interface_List(GUID_DEVINTERFACE_DISK)` matched with `IOCTL_STORAGE_GET_DEVICE_NUMBER`
   gives the disk devnode. Walk `DEVPKEY_Device_Parent` to the root. Every `Service` and filter on that chain is never-touch. [reasoning, standard APIs]
5. **Classification for Stripped:**
   - *Not hardware-bound* (no devnode references the service and it has no device triggers, e.g. `bindflt`, `luafv`, `Ndu`):
     hardware absence doesn't apply, so they need a separate "use absent" rule with its own basis.
   - *Never seen*: no devnode, not even a phantom, references the service. This is the only class Stripped removes automatically.
   - *Seen before (phantom only)*: show the last arrival date and leave it off by default. **Hot-plug classes** (USBSTOR,
     `usbaudio`, `xboxgip`/`xinputhid`, `HidUsb` game controllers, BT, printers, `WUDFRd`-based devices, MTP/`WpdUpFltr`)
     are "absent now, used sometimes". Stripped must never disable them on absence alone. Otherwise a flash drive or
     controller plugged in tomorrow silently fails (Code 32). [reasoning]
   - *Present but unused* (e.g. Wi-Fi radio while wired): not absent. Offer per-device disable through `CM_Disable_DevNode`?
     **No**: that changes device state and is outside the idea's scope. Show it and leave it to the owner.
6. **Wine:** CfgMgr32 in Wine returns a thin tree. Golden tests need exported fixtures (a JSON/plain-text dump of devnodes,
   properties and services captured on a real Win10 VM), not live Wine enumeration. [reasoning]

---

## 8. Latency and gaming performance: what can honestly be said

- **No published controlled measurement I could find** shows FPS or 1%-low gains from disabling stock Windows services or
  drivers on a modern 20-core/28-thread CPU. Idle service CPU time is near zero; the plausible effects are fewer
  periodic wakeups (DPC/ISR and timer activity) and less RAM in use. [reasoning; "services tweak = more FPS" is folklore]
- **Real, measurable latency sources are specific drivers:** NIC (interrupt moderation, RSS), Wi-Fi (`Netwtw*` is often
  high in LatencyMon), audio, GPU, storage, ACPI (`ACPI.sys` DPCs from EC/thermal polling). The right tool is a WPR/xperf
  DPC/ISR trace or LatencyMon, before and after, and the fix is usually a driver update or device setting, not
  `Start=4`. [reasoning; LatencyMon is a third-party *test tool*, fine under rule 3 as long as it isn't shipped]
- **Things that are measured (published):** VBS/HVCI cost (Tom's, §6); Win10 22H2 vs 21H2 on Raptor Lake (§4). The owner's
  9900K prefetcher +20% 1% lows result is his measurement on a different CPU (CLAUDE.md) and isn't about services.
- **SysMain/Superfetch, Search indexer, Windows Update** cause *intermittent* disk/CPU bursts that can show as stutter on
  HDDs. On NVMe the effect is unproven [folklore on NVMe; reasoning on HDD]. Label accordingly.
- **What Hadron measures (idea feature 9):** RAM in use and process/service count, labelled as measured. I'd add
  **DPC/ISR count per second** (`NtQuerySystemInformation(SystemInterruptInformation)` / perf counter
  `\Processor(_Total)\DPCs Queued/sec`) and **timer resolution** (`NtQueryTimerResolution`) as optional measured
  numbers, because they're the honest proxy for "latency". [reasoning]
- KB template for every perf-flavoured entry: "Effect depends on workload. Test it: PresentMon/CapFrameX, same map/scene,
  3× 60 s runs each before/after, compare avg and 1% lows; and a 60 s WPR `-start GeneralProfile` DPC/ISR trace."

---

## 9. Concrete recommendations for the spec

1. Never-touch = `Start=0` ∪ anything in any Upper/LowerFilters ∪ boot-disk devnode chain ∪ curated list (§2). Compute it
   live; the curated list is a floor, not the whole set.
2. The Windows-stock table excludes storage/boot-class start values (Windows demotes them per machine, §1.1). Default
   restore uses the snapshot for those.
3. Stripped hardware gating uses CfgMgr32 including phantoms and the service device-interface triggers (§7). Only
   "never seen" is automatic; hot-plug classes are never removed on absence alone.
4. Minimum keeps: `intelppm`, `Power`, audio stack (`Audiosrv`, `AudioEndpointBuilder`, `MMCSS`), networking stack
   for the active link, USB/HID, TPM/`tbs`, `mssmbios`, Defender drivers (protected), BFE/WFP, Store filters (`bindflt`,
   `wcifs`, `FileCrypt`, `FileInfo`) because Battle.net/Xbox/Store paths and Defender sit on them.
5. Hypervisor and VBS: the page reports state and links to the right toggle. It never disables host hypervisor drivers
   while the hypervisor launches. Hyper-V guest `vmic*` services are fine to disable on bare metal (SMBIOS-detected).
6. Recovery .zip: an offline-hive restore that resolves `Select\Current` and finds the Windows volume in WinRE. Don't
   rely on LKG or Safe Mode.
7. Basis labels: everything in §2–§5 marked reasoning should go to kb-auditor as reasoning, not published.
8. Verify on the owner's device (read-only, inside the project folder): boot path (NVMe vs VMD), the exact LAN/Wi-Fi/BT
   service names, Volume/Disk/Keyboard/Mouse class filter lists, VBS/HVCI state, and the drivers loaded while Overwatch 2 runs.

## Sources (all cited above)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x7b--inaccessible-boot-device
- https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/inaccessible-boot-device-stop-error
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/hklm-system-currentcontrolset-services-registry-tree
- https://learn.microsoft.com/en-us/windows/win32/services/automatically-starting-services
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addservice-directive
- https://winreg-kb.readthedocs.io/en/latest/sources/system-keys/Services-and-drivers.html
- https://techshelps.github.io/MSDN/KB/Source/win32ddk/q115486.htm
- https://msdn.microsoft.com/en-us/library/aa389394(v=vs.85)
- https://www.msi.com/Motherboard/PRO-Z790-P-WIFI/Specification
- https://at-web1.www.anandtech.com/show/16959/intel-innovation-alder-lake-november-4th/3
- https://techpowerup.com/review/alder-lake-windows-10-performance
- https://www.hwcooling.net/en/older-windows-10-versions-than-22h2-slow-down-raptor-lake-cpus/6/
- https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/configure-processor-power-management-options
- https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
- https://www.techpowerup.com/forums/threads/hpet-on-off-capframex-analysis.270732
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/load-order-groups-and-altitudes-for-minifilter-drivers
- https://learn.microsoft.com/en-us/archive/blogs/ntdebugging/understanding-file-system-minifilter-and-legacy-filter-load-order
- https://www.tomshardware.com/news/microsoft-windows-11-gaming-boost-tips
- https://www.techspot.com/news/96246-microsoft-releases-guide-optimize-windows-11-gaming-performance.html
- https://www.tomshardware.com/news/windows-11-gaming-benchmarks-performance-vbs-hvci-security/
- https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/driver-compatibility-with-device-guard
- https://us.forums.blizzard.com/en/overwatch/t/linux-anti-cheat-reference/937155
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/enumerating-installed-devices
- https://learn.microsoft.com/windows/win32/api/setupapi/nf-setupapi-setupdigetclassdevsw
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/porting-from-setupapi-to-cfgmgr32
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-service
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-upperfilters
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-deviceclass-upperfilters
- https://learn.microsoft.com/windows/win32/api/winsvc/ns-winsvc-service_trigger
- https://learn.microsoft.com/en-ie/Windows/Win32/services/service-trigger-events

Caveat: the power-management settings URL and a few Learn paths were built from known Learn structure rather than
returned by search; kb-auditor should click each before any of them become a KB `basis`.
