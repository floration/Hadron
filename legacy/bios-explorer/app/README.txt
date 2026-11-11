BIOS Settings Explorer - MSI PRO Z790-P WIFI (MS-7E06), BIOS 7E06vAF (A.F0)

Double-click BiosSettingsExplorer.exe and accept the admin prompt. Admin is needed to read the BIOS settings.
It only READS. It never writes anything to the BIOS.

- 5,368 settings pulled from the official MSI BIOS file. That includes the ones MSI hides from the menus.
- Current value (orange = changed from the BIOS defaults), the default, and every option.
- Every setting has an explanation. About 500 key settings (blue dot, "Key settings" filter) have an in-depth one:
  real-world effect in FPS games / other games / apps, a claim check, and a recommendation for each goal:
  Power saving / Balanced / Single-core / Multi-core / Low latency.
- Memory timings showing "Auto" are trained at boot. The BIOS doesn't store the actual values; HWiNFO shows them.
- Pick a goal, then click "Differs from goal" to see what you'd change.
- Click a column header (Setting, Current, Default, Goal) to sort. Click again to reverse, a third time to reset.
  Sorting by Goal puts settings to change first, then "test both" advice, then settings that already match.
- Grey rows are settings MSI hides in the BIOS menus. Repeated names show which port or core they belong to.
- "Export changes" writes a text list with the BIOS menu path for each change, saved next to the exe.
- "Load dump..." opens a saved UEFI_Dump folder (pick its index.csv). Useful for comparing before and after.

The settings map is built for BIOS 7E06vAF. If you flash another BIOS version, offsets can move.
Re-dump and ask for a map for that version before trusting the values.
