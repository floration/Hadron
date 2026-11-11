# Module catalog

Status: `legacy` = a working tool to port. `idea` = needs his OK before anyone builds it.

| id | Name | Status | Admin | Summary |
|---|---|---|---|---|
| `diskmap` | DiskMap | legacy | Required | Read-only drive-space visualiser: squarified treemap (Baseline) or dense sortable folder list (Console), 12 categories, safe-to-delete hints with a basis per rule; hard links counted once, links never followed, cloud placeholders skipped |
| `bios` | BIOS Settings | ported (package A, `module/bios`; KB package B pending) | Required | Guarded writes, off by default (see rule 2): setup values change only behind the never-persisted "Allow BIOS writes" switch, with a full plan, backups and read-back (`fwwrite.cpp`, docs/design/bios-write.md). Every setup setting of MS-7E06 / 7E06vAF (5,368, embedded `settings.db` v2) with its live value (UEFI variables read on a Worker, per-variable 203/1314/5 states), default, menu path, help, explanation, per-goal advice and the **basis** of each KB entry. SMBIOS version guard: a board/BIOS mismatch marks values unverified. Snapshots replace `legacy/uefi-dump`: save (index-prefixed files, never overwrites), load a folder or `.zip` (own inflate), compare with live. Export via a Save dialog. `--snapshot <dir\|zip>`, `--goal <1-5>`. Design: docs/design/bios.md |
| `runas` | Run As | new (owner-approved) | Required | Launch any file (program, document, `.lnk`, `.msc`) as this admin / another user / SYSTEM / TrustedInstaller, with a full or limited token; shows the *effective* identity read back from the child token and flags any OS downgrade. Last-run list (paths/args/identity only, never passwords, ≤50) and quick buttons for the Windows admin tools. SYSTEM/TI behind an off-by-default, never-persisted switch (rule 4); passwords read via CredUI, zeroed after use. Token logic in `launch.cpp` is headless-testable. Design: docs/design/runas.md |

Dropped by the owner: Organizer, OrganizerRescue, SaveFinder.

## Ideas to raise with the owner (do not build without his OK)
- **BIOS version compare**: a `settings.db` for 7E06vAH / vAK, showing changed options, defaults and microcode. He wants to test BIOS versions before committing to one.
- **Latency checker**: DPC/ISR latency per driver through ETW, as LatencyMon shows. Read-only.
- **Power plan inspector**: hidden power-plan settings (IDLEDISABLE, core parking, EPP), using the BIOS module's
  explanation and claim-check format. Changes are applied only after explicit confirmation.
- **Frame-time logger**: PresentMon-style ETW capture with 1% / 0.1% lows, for A/B-testing BIOS changes.
