# Module catalog

Status: `legacy` = a working tool to port. `idea` = needs his OK before anyone builds it.

| id | Name | Status | Admin | Summary |
|---|---|---|---|---|
| `diskmap` | DiskMap | legacy | Required | Read-only drive-space visualiser: squarified treemap (Baseline) or dense sortable folder list (Console), 12 categories, safe-to-delete hints with a basis per rule; hard links counted once, links never followed, cloud placeholders skipped |
| `bios` | BIOS Settings Explorer | legacy | Required | Every setup setting with live value, default, explanation and per-goal advice. Includes snapshots (absorbs `legacy/uefi-dump`) and snapshot compare |
| `runas` | Run As | new (owner-approved) | Required | Launch any file (program, document, `.lnk`, `.msc`) as this admin / another user / SYSTEM / TrustedInstaller, with a full or limited token; shows the *effective* identity read back from the child token and flags any OS downgrade. Last-run list (paths/args/identity only, never passwords, ≤50) and quick buttons for the Windows admin tools. SYSTEM/TI behind an off-by-default, never-persisted switch (rule 4); passwords read via CredUI, zeroed after use. Token logic in `launch.cpp` is headless-testable. Design: docs/design/runas.md |

Dropped by the owner: Organizer, OrganizerRescue, SaveFinder.

## Ideas to raise with the owner (do not build without his OK)
- **BIOS version compare**: a `settings.db` for 7E06vAH / vAK, showing changed options, defaults and microcode. He wants to test BIOS versions before committing to one.
- **Latency checker**: DPC/ISR latency per driver through ETW, as LatencyMon shows. Read-only.
- **Power plan inspector**: hidden power-plan settings (IDLEDISABLE, core parking, EPP), using the BIOS module's
  explanation and claim-check format. Changes are applied only after explicit confirmation.
- **Frame-time logger**: PresentMon-style ETW capture with 1% / 0.1% lows, for A/B-testing BIOS changes.
