# svcdrv Finalize: mini analyze (bugs the Finalize fixes introduced or left)

Reviewer subagent, read-only static pass over `git diff 907d435..e3cfd92 -- modules/services core` (saved by Finalize).
Its R3 symlink finding duplicated mini-review N1 and was fixed in 9e586ea.

| # | Cat | Where | Failure | Fix |
|---|---|---|---|---|
| A1 | 1 | store.cpp:440-456, 497-522 | Only the data folder (and its holder's owner) is checked; a non-admin who can rename an ancestor (e.g. D:\Tools under a D:\ granting Authenticated Users Modify) swaps in a writable folder after the check, then edits restore.cmd that the README says to run as SYSTEM in WinRE | Check every existing ancestor to the volume root for non-admin DELETE / FILE_DELETE_CHILD / WRITE_DAC / WRITE_OWNER, or hold the root handle without FILE_SHARE_DELETE for the operation and verify created files by final path |
| A2 | 2 | plan.cpp:1262-1272, 1230, 1360; module.cpp:1404-1408, 686-697 | Dismiss removes the only in-app Undo for rows an interrupted apply wrote; dismissing an interrupted resume:A brings A's banner back; Dismiss during a load does nothing visible | Keep a dismissed log undoable; resolve along the resume chain; take the dismiss request even while a load runs |
| A3 | 2 | plan.cpp:1473-1515 | EnsureFirstRecovery rewrites recovery\first-N without the R11 build/machine check: after an upgrade it writes the old build's values for WinRE. A truncated zip counts as complete | Write only when build and machine GUID match; else a note |
| A4 | 2 | module.cpp:702-753; plan.cpp:1408-1427 | ResolveStaleTiClaims uses a TI state read seconds earlier; another instance's live `T stopfailed` claim can be resolved wrongly | Re-query TrustedInstaller under the apply lock |
| A5 | 2 | ti.cpp:151; plan.cpp:1126 | Cancel turns sent cleanup requests (TI stop, R13 restarts) into reported failures | Short bounded wait without cancel for cleanup; word "requested, not confirmed" |
| A6 | 2 | plan.cpp:901 | The R12 gate accepts a first.txt from another PC | Gate on the load's machine-matched hasFirst, or re-check the GUID in RunApply |
| A7 | 3 | module.cpp:1396-1399, 1700-1701 | After an aborted apply, the result banner's Undo opens the previous apply | Offer it only when logs_[undoable_].stamp == result stamp |
| A8 | 6 | module.cpp:994, 1974 | An unreadable item shows its placeholder start type as fact | Show "unknown" when cfgUnknown |

Fixture runs owed: (1) interrupted log with D-ok rows + finished dismiss: log, `--plan undo-last`; (2) interrupted
resume:A + dismiss of the resume; (3) first.txt with another build and recovery-first.zip removed; (4) logs with
`order` > 1000000 sort order.

Held: TI claim lifecycle (R4, R14, R22), undo/resume chains (R7, R8), guards (R2, R5, R6, R13, R17), R10, threading.
Counts: cat 1: 1, cat 2: 5, cat 3: 1, cat 6: 1.
