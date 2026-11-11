# Real-world effect per workload + claim check, keyed by KB title.
# (fps shooters / esports, other games, apps & work, claim check)
# Plus overrides for explanations, recommendations and notes that were overstated.

NOFX = 'No measurable effect.'
VOLT = ('No direct fps effect. Voltage only decides stability, temperatures and wear. A lower stable voltage can help '
        'only if you are hitting thermal or power limits.')

IMPACT = {
 'Hyper-Threading': (
  'Often neutral, sometimes slightly better 1% lows with HT off on 12th-14th gen. Fewer threads then share each P-core\'s '
  'cache and execution units. It varies by title, so test it.',
  'Heavily threaded games (big open worlds, city builders, shader compilation) can lose a few percent or stutter more with '
  'HT off, especially if E-cores are also off.',
  'Rendering, compiling and encoding run roughly 10-30% slower with HT off.',
  'The vendor line "HT is free extra performance" holds for throughput work. On a 20-core CPU it is mostly a wash in games. '
  'The tweak-guide line "HT off always helps games" is just as overstated.'),
 'Intel Turbo Boost': (
  'Large effect at stock ratios (up to 5.5 GHz vs 3.4 GHz base). None when a fixed all-core ratio is set.',
  'Same as FPS titles.', 'Same as games.', 'Accurate.'),
 'Turbo Boost Max 3.0': (
  'Small. Games use more than the two favoured cores. None under a fixed ratio.',
  'Small.', 'Lightly threaded apps gain about 100 MHz on the top two cores.',
  'Marketing makes it sound bigger than it is. On a 14700K the favoured cores are about 100 MHz faster.'),
 'EIST (SpeedStep)': (
  'Under game load the busy cores already sit at max clock, so average fps barely changes. Turning it off can remove brief '
  'clock dips on lightly loaded threads, which is a small effect on lows at best.',
  'Negligible.', 'Negligible under load. Off raises idle power and heat noticeably.',
  '"Disable EIST for fps" is mostly folklore since Speed Shift arrived. With a fixed ratio it does nothing either way.'),
 'Intel Speed Shift (HWP)': (
  'Little effect under sustained game load. Helps bursty threads clock up fast when clocks are dynamic.',
  'Little effect.', 'Snappier desktop and bursty tasks when clocks are dynamic.',
  'Accurate for dynamic clocks. Irrelevant with a fixed ratio and EIST off.'),
 'CPU C-States': (
  'Off can shave microseconds of wake-up time from threads that briefly sleep. Some players measure slightly better 1% lows '
  'or input latency, but it is often within run-to-run noise. Busy game threads rarely reach deep C-states.',
  'Usually no measurable change.', 'No throughput change. Idle power and temperatures rise.',
  'Intel\'s "no performance cost" is true for averages and slightly overstated for latency. Tweak-guide claims of big fps '
  'gains are overstated.'),
 'C1E': (
  'Tiny wake-up gain at best.', 'Negligible.', 'None under load. Idle power goes up.', 'Low impact either way.'),
 'Package C-State Limit': (
  'Little to none. Deep package states only happen when the whole CPU is idle, which never happens in a running game.',
  'Same as FPS titles.',
  'None under load. Very deep states can add tiny wake-up delays for audio or USB at idle.',
  'Tweak guides overstate it for gaming.'),
 'Energy Efficient Turbo': (
  'Rarely engages under game load. Minimal effect.', 'Minimal.', 'Can trim clocks slightly under medium loads.',
  'Small either way.'),
 'Energy Efficient P-state': (
  'Minimal.', 'Minimal.', 'Slight power saving at light load.', 'Small either way.'),
 'Hardware Duty Cycling': (
  'Desktop CPUs almost never duty-cycle unless they hit a power limit. Minimal effect.', 'Minimal.', 'Minimal.',
  'Mainly a laptop feature. Turning it off on a desktop rarely changes anything.'),
 'Race To Halt': ('Minimal.', 'Minimal.', 'Slight efficiency gain.', 'Small either way.'),
 'Hardware Prefetcher': (
  'Can help lows a lot in some shooters (about +20% 1% lows reported in Overwatch on a 9900K) and hurt in others. Test it '
  'per game.',
  'Mixed. Streaming open-world games lean towards Enabled.',
  'Enabled usually wins for encoding, compression, compiling and loading. Off can help some bandwidth-bound all-core work.',
  'Intel ships it on because it wins on average across benchmarks. "Always better" is overstated for random-access game '
  'engines.'),
 'Adjacent Cache Line Prefetch': (
  'Off can improve lows in random-access-heavy games, and it is game-dependent. Test it separately from the Hardware '
  'Prefetcher.',
  'Mixed.', 'Enabled for most apps. Off is a common win for bandwidth-bound servers and HPC.',
  'Same as the Hardware Prefetcher: tuned for the average benchmark, not for every game.'),
 'Intel VT-x': (
  'VT-x itself costs nothing. Windows features that need it (Memory Integrity/VBS, Hyper-V, WSL2) can cost a few percent '
  'of fps and some 1% lows in CPU-bound games.',
  'Same as FPS titles.', 'Required for VMs, WSL2, Docker and Sandbox.',
  'Microsoft says VBS has minimal impact, but independent game tests have measured losses in some titles. Turning VT-x off '
  'only "helps" because it forces VBS off. Turning Memory Integrity off in Windows gets the same result.'),
 'Intel VT-d': (
  NOFX, NOFX, 'Needed for passing devices through to VMs.',
  'Disabling it for fps is folklore. There is no good evidence of a gain.'),
 'CPU Cooler Tuning (power limits)': (
  'Usually none. Games draw well under Intel\'s limits (often 100-200 W on a 14700K), so raising them adds no fps.',
  'Same, except very CPU-heavy titles and shader compilation, which load all cores.',
  'Large effect on all-core rendering and encoding: noticeably faster with higher limits, at much more power and heat.',
  'MSI\'s "Unlimited" implies more performance everywhere. In games it mostly removes a ceiling you never reach.'),
 'Enhanced Turbo (MSI)': (
  'None with a fixed ratio. Otherwise small, since games rarely load every core fully.',
  'Small.', 'Large gain in all-core work.', 'Mostly benefits all-core benchmarks.'),
 'Game Boost (MSI)': (
  'Depends on the preset. It is generic and usually requests high voltage, so a manual tune beats it.',
  'Same as FPS titles.', 'Same as games.',
  'One-click overclock marketing. It often overvolts, which is a bad idea on 14th gen.'),
 'CPU Ratio Mode': (
  'Fixed removes clock transitions entirely, which some players see as steadier lows. With Speed Shift on, busy cores already sit near max, so the gap is often small. Compare your fixed all-core ratio with the boost clock: a fixed 53x is below the 55-56x single-core boost.',
  'Small.', 'Same throughput at the same clock. Idle power is higher with Fixed.',
  'Small effect either way.'),
 'P-Core Ratio': (
  'One of the real levers: CPU-bound fps scales with clock. A 5.5-5.6 GHz single-core boost vs a lower fixed all-core '
  'ratio can go either way in games.',
  'Matters when CPU-bound. Little effect when GPU-bound.',
  'The all-core ratio drives multi-thread results.', 'Accurate.'),
 'E-Core Ratio': (
  'Small. Game threads mostly run on P-cores.', 'Small.', 'Matters for all-core work.', 'Accurate.'),
 'Ring (cache) ratio': (
  'One of the bigger knobs for CPU-bound, high-fps games: a higher ring clock lowers cache and memory latency. It can be '
  'worth a few percent.',
  'Smaller effect unless CPU-bound.', 'Small for most apps.',
  'Underrated by marketing, which focuses on core clocks.'),
 'Ring Down Bin': (
  'Disabled keeps the ring clock higher under load. Small but real gain in CPU-bound games.',
  'Small.', 'Small.', 'Accurate.'),
 'CPU Core Voltage Mode': (VOLT, VOLT, VOLT, 'Voltage is not performance. It only lets a clock be stable.'),
 'CPU Core Voltage': (VOLT, VOLT, VOLT, 'Voltage is not performance. It only lets a clock be stable.'),
 'CPU Lite Load (AC loadline)': (VOLT, VOLT, VOLT,
  'MSI\'s higher Lite Load modes overvolt for stability margin. Lower modes are often stable and run cooler.'),
 'CPU Lite Load Control': (VOLT, VOLT, VOLT, 'Accurate.'),
 'Loadline Calibration (LLC)': (VOLT, VOLT, VOLT, 'Strong LLC is not "more performance". It only trades droop for overshoot.'),
 'UnderVolt Protection': (NOFX, NOFX, NOFX, 'Security setting. No performance effect.'),
 'Current Excursion Protection': (
  'None when voltage is sufficient. With an aggressive undervolt, CEP quietly cuts clocks, so fps drops instead of the PC '
  'crashing.',
  'Same as FPS titles.', 'Same as games.',
  'Often described as protecting the CPU. It actually protects stability by throttling when voltage dips. It does not '
  'prevent degradation.'),
 'Thermal Velocity Boost': (
  'None with a fixed ratio. Otherwise about +100 MHz while the CPU is cool.', 'Same as FPS titles.', 'Same as games.',
  'The 14th-gen fix for TVB lives in the microcode (0x125 and newer), not in this switch.'),
 'IA ICCMax': (
  'None. Games do not reach the current limit.', 'None.',
  'Caps peak current in heavy all-core work.',
  'Raising it mainly lets current and voltage climb in all-core loads. No gaming benefit.'),
 'Thermal throttling protection': (
  'None unless the CPU reaches 100 °C.', 'Same as FPS titles.', 'Same as games.', 'Accurate.'),
 'TCC Activation Offset': (
  'None unless the CPU reaches the new limit.', 'Same as FPS titles.', 'Can lower all-core clocks if you hit the limit.',
  'Accurate.'),
 'AVX / AVX offset': (
  'Few games use heavy AVX, so the offset rarely matters.', 'Rarely matters.',
  'AVX-heavy work (encoding, Prime95, y-cruncher) clocks down by the offset.', 'Accurate.'),
 'Active E-Cores': (
  'Off can improve lows in some titles. This is mostly on Windows 10, where game and background threads can land on '
  'E-cores, and it can let the ring clock higher. The gain is smaller on Windows 11.',
  'Heavily threaded games and shader compilation can get worse with E-cores off.',
  'Large multi-core loss: a 14700K has 12 of its 20 cores as E-cores.',
  'Intel says Thread Director makes hybrid scheduling seamless. That needs Windows 11, and even there it is not perfect. '
  '"E-cores always hurt games" is overstated too.'),
 'XMP': (
  'One of the biggest levers: memory speed and latency strongly affect CPU-bound 1% lows, often 5-15% vs the JEDEC default.',
  'Noticeable when CPU-bound.', 'Helps memory-bound work.',
  'Accurate. Manual timing tuning goes further than XMP.'),
 'DRAM Power Down Mode': (
  'Tiny if any. RAM rarely idles under game load.', 'Tiny.', 'Tiny.', 'Tweak guides overstate it.'),
 'PCIe ASPM (link power saving)': (
  'The GPU link stays active under load, so fps rarely changes. A few devices have buggy wake-ups that cause hitches.',
  'Rarely measurable.', 'Minor. ASPM on saves a few watts at idle.',
  'Tweak guides overstate it. The vendor "no impact" is mostly true, though some devices are buggy.'),
 'Resizable BAR': (
  'Usually zero to small in esports shooters, and some titles lose fps. NVIDIA\'s driver only turns it on for whitelisted '
  'games, so enabling it in BIOS is low risk on NVIDIA. On AMD and Intel GPUs it applies to every game.',
  'Some AAA titles gain several percent (up to about 10%). A few lose.', 'None.',
  'Marketed as up to 10%+. The real average gain is a few percent and depends on the game.'),
 'Above 4G Decoding': (NOFX + ' It is only needed for Resizable BAR.', NOFX, NOFX, 'Accurate.'),
 'CSM (legacy boot)': (NOFX + ' Off is needed for Resizable BAR.', NOFX, NOFX, 'Accurate.'),
 'HPET': (
  'The BIOS switch has no proven fps effect, because Windows already uses the TSC timer by default. The real hit came from '
  'forcing HPET as the OS clock (bcdedit /set useplatformclock true).',
  NOFX, NOFX,
  'Tweak guides overstate the BIOS switch. They mix it up with the bcdedit setting.'),
 'Spread spectrum': (NOFX, NOFX, NOFX, 'Only matters for BCLK overclocking.'),
 'ErP Ready': (NOFX, NOFX, NOFX, 'Standby power only.'),
 'USB standby power': (NOFX, NOFX, NOFX, 'Standby power only.'),
 'Wake on LAN': (NOFX, NOFX, NOFX, 'Standby power only.'),
 'Onboard devices': (
  'Usually none. The exception is a buggy driver (some Wi-Fi, Bluetooth and audio drivers) causing DPC latency spikes. '
  'Check with LatencyMon before disabling.',
  'Usually none.', 'None.',
  'Overstated as a general tweak. It only helps when a specific driver misbehaves.'),
 'TPM': (
  'Intel PTT has no measured fps cost. The well-known fTPM stutter was an AMD bug.',
  NOFX, NOFX, 'Accurate.'),
 'Intel VMD': (NOFX + ' for normal NVMe setups.', NOFX, NOFX, 'Accurate.'),
 'DMI link equalization': (NOFX + ' while the link is stable.', NOFX, NOFX, 'Not a performance setting.'),
 'MONITOR/MWAIT': (NOFX, NOFX, NOFX, 'Accurate.'),
 'Memory Fast Boot': (NOFX + ' after Windows starts.', NOFX, NOFX, 'Only affects boot time.'),
 'PCIe LTR / PTM (per root port)': (
  'None measurable. Low override values only block deep package idle, which matters at idle, not under game load. With C-states off there is no effect at all.',
  NOFX, 'None on performance. Raising the values lets the CPU idle deeper and save power; lowering them raises idle power.',
  'Some tweak lists say to set Snoop/Non-Snoop latency to the minimum for lower input latency. The values are a power-management tolerance report: they cannot make a device or PCIe link respond faster. At most they keep the CPU out of deep idle, which disabling C-states already does.'),
 'Fast Boot': (NOFX + ' after Windows starts.', NOFX, NOFX, 'Accurate.'),
}

# replaced explanation / recommendation / note text for entries that overstated things
EXPL = {
 'Intel VT-x': 'CPU virtualization. Needed for virtual machines, WSL2, Docker, Windows Sandbox and Memory Integrity (VBS). '
               'VT-x itself costs nothing. Windows features that use it can.',
 'Current Excursion Protection': 'Lowers clocks briefly when the core voltage dips below what the current clock needs. '
                                 'It keeps an undervolted CPU from crashing, at the cost of quietly lower performance.',
 'Resizable BAR': 'Lets the CPU address all of the graphics card\'s memory at once. The gain is game-dependent: zero to '
                  'several percent, and a few games lose. Needs Above 4G Decoding and UEFI mode (CSM off).',
 'HPET': 'A hardware timer device. Windows uses the CPU\'s own timer (TSC) by default either way, so the BIOS switch rarely '
         'matters.',
}
REC = {
 'Intel Turbo Boost': {'latency': '~Either. No effect with a fixed ratio (recommended for this goal)'},
 'Turbo Boost Max 3.0': {'latency': '~Either. No effect with a fixed ratio (recommended for this goal)'},
 'Thermal Velocity Boost': {'latency': '~Either. No effect with a fixed ratio (recommended for this goal)'},
 'Energy Efficient Turbo': {'latency': '~Either. No effect with a fixed ratio (recommended for this goal)'},
 'Energy Efficient P-state': {'latency': '~Either. No effect with a fixed ratio (recommended for this goal)'},
 'Above 4G Decoding': {'latency': '~Enabled if you use Resizable BAR, otherwise either'},
 'Resizable BAR': {'latency': '~Enabled on NVIDIA (per-game whitelist); test per game on AMD/Intel GPUs'},
 'Hyper-Threading': {'latency': '~Test both. Off is often equal or better in high-fps shooters'},
 'Intel Speed Shift (HWP)': {'latency': '~Enabled (no effect with a fixed ratio and EIST off)'},
 'EIST (SpeedStep)': {'latency': '~Either. No effect with a fixed ratio'},
 'CPU Ratio Mode': {'latency': 'Fixed Mode'},
}
NOTE = {
 'CPU Ratio Mode': 'Fixed removes clock transitions, so EIST, Speed Shift, Turbo, Turbo Boost Max 3.0 and TVB stop mattering. Trade-offs: the fixed all-core ratio is usually below the 5.5-5.6 GHz single-core boost, so lightly threaded loads can be slower. With Override voltage, idle cores sit at load voltage, which adds heat and some wear. Thermal throttling and C-states still apply. Stability comes from enough voltage for the clock, not from fixed vs dynamic.',
 'CPU C-States': 'With C-states off, idle cores stay at load voltage and run warmer. The 13th/14th-gen damage came mainly '
                 'from very high voltage requests (about 1.4-1.5 V and up) during boost. A moderate fixed voltage at idle is '
                 'a smaller risk, but not zero.',
 'Current Excursion Protection': 'If you undervolt and scores drop instead of the PC crashing, CEP is throttling.',
 'Thermal Velocity Boost': 'Does nothing under a fixed all-core ratio.',
}
