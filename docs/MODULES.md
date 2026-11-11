# Module catalog

Status: `legacy` = a working tool to port. `pending` = waiting on the owner's decision. `idea` = needs his OK before anyone builds it.

| id | Name | Status | Admin | Summary |
|---|---|---|---|---|
| `diskmap` | DiskMap | legacy | None | Drive-space treemap and folder list. First port, proves the contract |
| `bios` | BIOS Settings Explorer | legacy | Optional | Every setup setting with live value, default, explanation and per-goal advice. Includes snapshots (absorbs `legacy/uefi-dump`) and snapshot compare |
| `rescue` | Organizer Rescue | pending | Required | Undo file moves using the NTFS change journal. Built for the dropped Organizer tool; the owner decides whether it stays, gets generalised, or goes |

Dropped by the owner: Organizer, SaveFinder.

## Ideas to raise with the owner (do not build without his OK)
- **BIOS version compare**: a `settings.db` for 7E06vAH / vAK, showing changed options, defaults and microcode. He wants to test BIOS versions before committing to one.
- **Latency checker**: DPC/ISR latency per driver through ETW, as LatencyMon shows. Read-only.
- **Power plan inspector**: hidden power-plan settings (IDLEDISABLE, core parking, EPP), using the BIOS module's
  explanation and claim-check format. Changes are applied only after explicit confirmation.
- **Frame-time logger**: PresentMon-style ETW capture with 1% / 0.1% lows, for A/B-testing BIOS changes.
