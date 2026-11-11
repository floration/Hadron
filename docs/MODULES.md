# Module catalog

Status: `legacy` = a working tool to port. `idea` = needs his OK before anyone builds it.

| id | Name | Status | Admin | Summary |
|---|---|---|---|---|
| `diskmap` | DiskMap | legacy | Required | Drive-space treemap and folder list. First port, proves the contract |
| `bios` | BIOS Settings Explorer | legacy | Required | Every setup setting with live value, default, explanation and per-goal advice. Includes snapshots (absorbs `legacy/uefi-dump`) and snapshot compare |
| `runas` | Run As | new (owner-approved) | Required | Launch any file as this admin / another user / TrustedInstaller / SYSTEM, full or limited token; keeps a last-run list and quick buttons for Windows tools. SYSTEM/TI behind an off-by-default switch (rule 4). Design: docs/design/runas.md |

Dropped by the owner: Organizer, OrganizerRescue, SaveFinder.

## Ideas to raise with the owner (do not build without his OK)
- **BIOS version compare**: a `settings.db` for 7E06vAH / vAK, showing changed options, defaults and microcode. He wants to test BIOS versions before committing to one.
- **Latency checker**: DPC/ISR latency per driver through ETW, as LatencyMon shows. Read-only.
- **Power plan inspector**: hidden power-plan settings (IDLEDISABLE, core parking, EPP), using the BIOS module's
  explanation and claim-check format. Changes are applied only after explicit confirmation.
- **Frame-time logger**: PresentMon-style ETW capture with 1% / 0.1% lows, for A/B-testing BIOS changes.
