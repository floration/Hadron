# Module catalog

Status: `legacy` = working single-file tool to port, `planned` = idea agreed with the owner, `idea` = suggestion, needs his OK.

| id | Name | Status | Admin | Summary |
|---|---|---|---|---|
| `bios` | BIOS Settings Explorer | legacy | yes | Every setup setting with live value, default, explanation, per-goal advice; snapshot (UEFI dump) and snapshot diff |
| `diskmap` | DiskMap | legacy | no | Drive space treemap and folder list |
| `organizer` | Organizer | legacy | no | Sort loose files into category folders, with plan, skip, undo |
| `rescue` | Organizer Rescue | legacy | yes | Undo moves using the NTFS change journal |
| `savefinder` | SaveFinder | legacy | no | Find saves and personal data in a Steam library before deleting it |
| `uefidump` | UEFI Snapshot | legacy (PowerShell) | yes | Merge into `bios` as "Save snapshot" / "Compare snapshots" |

## Ideas to raise with the owner (do not build without his OK)
- **BIOS version compare**: build `settings.db` for 7E06vAH / vAK and show what changed in options, defaults
  and microcode. He wants to test BIOS versions before committing to one.
- **Latency checker**: DPC/ISR latency per driver (what LatencyMon shows), using ETW. Read-only.
- **Power plan inspector**: show hidden power-plan settings (IDLEDISABLE, core parking, EPP) with the same
  explanation and claim-check format as the BIOS module, and apply only on explicit confirmation.
- **Frame-time logger**: PresentMon-style ETW capture, with 1% / 0.1% lows for A/B testing BIOS changes.
- **Leftovers finder**: folders left behind by uninstalled programs (list only; delete through the safe fs layer).
