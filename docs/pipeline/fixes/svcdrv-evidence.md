# svcdrv follow-ups 6-9: signer, event 7040, interface evidence, registry fallback

Branch `fix/svcdrv-evidence`. Small fixes thread. Design: docs/design/services.md (Files, Formats, "Registry
fallback" under Safety mechanics, Deviations). Decisions: docs/DECISIONS.md, rows tagged `proxy (Small fixes)`.

## What changed
| # | Follow-up | Where | What the user sees |
|---|---|---|---|
| 6 | Signer column | `evidence.*` (`ReadSigner`, `ItemSigner`), `evidence_view.*` (Worker pass) | A Signer column right of Company (Baseline and Console, sortable): "Signed: <CN>", "Catalog-signed", "Unsigned", "Not trusted: <why>" or "Can't check: <why>". The detail pane has "Signer (Authenticode)" with how it was checked |
| 7 | Event 7040 | `evidence.*` (`ReadLive7040`, `ParseEvent7040`, `NewestPerService`) | For an item whose start type differs from first.txt or from what Hadron set: who changed it (account and SID), when (UTC) and from/to, at the top of the detail pane; or "no 7040 event found (log may have rolled over)" |
| 8 | Hardware evidence | `hw.*` (`AddLiveIfaceEvidence`, `IfaceEvidence`, `HwEvidenceEx`, last-arrival date), `plan.cpp` `BuildPreset` | A device-interface trigger that names hardware present or seen before keeps the item; "present" and "seen before" carry "last arrived YYYY-MM-DD" |
| 9 | Registry fallback | `regfallback.*`, `plan.cpp` (`RunApply`, `BuildUndo`, `BuildResume`, `MarkTi`, `DumpPlan`), `backend*.{h,cpp}` (`ReadRegStart`, `WriteRegStart`), `module.cpp` (switch, F9, banner) | On a plan with rows the SCM refuses: a "Registry fallback: off" switch (Console F9 after the merge with svcdrv-presets, which took F7). On: those rows say "SCM refused; will write registry Start directly, takes effect after restart" |

`module.cpp` gets hooks only: one `EvidencePasses` member, two messages (`WM_MODULE_BASE + 20/21`, numbered away from
the others so a message added on another branch can't collide), the Signer column keys, three detail lines, the
switch and its banner, and `MarkRegFallback` before `MarkTi` in `BuildFrom`.

## Safety (hard rules 1, 4, 7)
- Signer and 7040 are read-only. The signer read refuses UNC and network-drive paths, any reparse point on the path,
  and `RECALL_ON_DATA_ACCESS` / `RECALL_ON_OPEN` / `OFFLINE` files; it opens with `FILE_FLAG_OPEN_REPARSE_POINT |
  FILE_FLAG_BACKUP_SEMANTICS`, re-checks the attributes on the handle, and verifies with `WTD_REVOKE_NONE` and
  `WTD_CACHE_ONLY_URL_RETRIEVAL`. Results are cached per path, size and write time.
- Interface triggers and arrival dates can only keep more. A selftest checks this on 3,000 random device pictures, and
  on every preset on both tabs (with the interface data removed, every ticked row stays ticked).
- Registry fallback: off by default, per apply, never persisted; its own plan line; written only after Apply. Before
  the write, the key is read again (drift skips the row) and the `I` record with the old values is written and
  flushed. After the write, the key is read back (a mismatch is `D fail`). The pre-apply snapshot, recovery folder and
  zip come first, as for every apply. It is refused for launch-protected, never-touch, boot-chain, named
  never-preset, and Boot/System-start items, at plan time and again at write time. Undo uses the same path.
  `RegSetValueExW` exists only in `backend_live.cpp`, behind `Arm()`.

## Tests
- `--selftest`: 340 checks (was 282), standalone and in the suite, under Wine. New: `selftest_evidence.cpp`:
  - signer states and labels from `signers.txt`; the attribute rule; UNC refused; an unsigned temp file; the cache;
  - 7040 parsing (single and double quotes, entities, wrong id or provider), newest-wins, match by display name, the
    no-event text;
  - interface and arrival evidence only tightening (random and whole-plan);
  - registry fallback on the fake backend:
    - off by default, the plan line, TI precedence;
    - refusals (protected, never-touch, boot path, System-start driver, never-preset set, pending change, missing
      Start, Boot/System target);
    - nothing written when the `I` flush fails, and exactly one write when it succeeds;
    - the snapshot; drift abort; read-back mismatch; write-time re-check; switch-off skip;
    - the Undo round trip;
    - the unarmed live `WriteRegStart` refused.
- One existing check moved from FrameServer to SCardSvr ("a never-seen hot-plug class is offered unticked"):
  FrameServer is now kept by its trigger in the fixture.
- `tests/tools/services_check.sh`: passes; `signers.txt` and `events7040.txt` added to the fixture comparison; eight
  new renders.

## Goldens changed (intended)
`plan-services-stripped.txt`, `plan-services-minimum.txt`, `plan-drivers-stripped.txt` and
`plan-drivers-minimum.txt`:
- FrameServer moves from unticked to "Kept: hardware in use or seen before. its device-interface trigger … matches
  USB\VID_046D&PID_085C\2D1A3F70 (seen before, not present now, last arrived 2026-03-02)". This comes from the
  fixture's new webcam under the Image class.
- The Bluetooth and scanner evidence gain "last arrived" dates.

No other golden moved: `default`, `bloat` and `stock` on both tabs, and `delayedlinger`, are byte-identical.

## Renders
In `/mnt/project-files/pipeline/fixes/svcdrv-evidence/`, dark theme, 1280x800:
- `services-signer.png`, `services-signer-svc.png`, `services-console-signer.png`: the Signer column, sorted by it;
- `services-drift-7040.png`, `services-console-drift-7040.png`: the drift detail with event 7040;
- `services-regfallback-plan.png`, `services-console-regfallback-plan.png`: the fallback plan;
- `services-iface-evidence.png`: FrameServer kept by its trigger.

## Review fixes (R1-R6)
- R1: `WriteRegStart` takes the values RunApply re-read and logged, and refuses under its write handle (writing
  nothing) when the key holds anything else, DelayedAutostart compared raw. The fake gains `regDriftAtWrite`, a write
  by another tool between the `I` record and Hadron's write; a selftest shows it is left as that tool wrote it.
- R2: a failed `Start` write after DelayedAutostart was written puts DelayedAutostart back on the same handle (deleted
  when it was absent). Fake fault `regStartFail`, with a selftest.
- R3: a failed `RegFlushKey` no longer fails a write that happened; it is noted in the `D` text and the read-back
  decides.
- R4: the signer and event passes never join on the UI thread. Each pass has its own Worker; a replaced one is
  cancelled, its results dropped, and it is joined once it has finished (or in Shutdown). A selftest replaces a
  400-item pass at once and gets only the new pass's results.
- R5: an untrusted root, a chaining error or `TRUST_E_SYSTEM_ERROR` is "Can't check: offline: the root certificate
  isn't in the local store"; a bad digest, explicit distrust, revocation, expiry or wrong usage stays "Not trusted".
- R6: "takes effect after restart" is labelled reasoning, with "check it after a restart; Windows Update Medic can
  put some of these back", in the plan note, the banner and the row detail.

## Open items
- The live 7040 scan stops after 20,000 events. If no event was found within them, the line still says "no 7040
  event found (log may have rolled over)".
- Live signer, event and interface reads can't run under Wine. They need a Windows run, for example CI's
  windows-latest job or the owner's PC (read-only).
