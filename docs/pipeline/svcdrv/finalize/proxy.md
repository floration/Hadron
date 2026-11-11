# svcdrv Finalize: proxy answers (Q1-Q7 from 6-review.md)

Answered 2026-11-11 by the proxy subagent (PIPELINE.md "How it works"). Logged in docs/DECISIONS.md tagged `proxy`.

- **Q1 (R18): Minimum ticks undetectable-use rows by default.** Build the unticked rule from the plan's level
  (`level == LV_STRIPPED`), not `e->JoinLevel()` (plan.cpp:257-262). At Minimum they are ticked and keep their evidence
  text ("use cannot be detected"). Hot-plug classes stay unticked at every level. Why: the owner's words, Minimum is
  "only what is needed to boot, network, play games and run Hadron"; plan, confirm and undo still gate every change.
- **Q2 (R20): no published client source exists.** Label "Default: Windows stock (no data for this build yet)"; the
  chip and F8 open a plan whose empty state reads "Hadron has no Windows-stock data for build <build> yet: nothing will
  be changed." Never a bare "Nothing to change." Goldens plan-services-stock.txt and plan-drivers-stock.txt.
  ws2016-services.tsv is Server 2016 and is not client stock. Follow-up idea: the 19045 stock table.
- **Q3: yes.** Chip "Default: as first seen (YYYY-MM-DD)", Console F-key label "1st seen", plan header "Start types as
  Hadron first saw this PC on <date>. This is not Windows' default."
- **Q4: log the narrowing.** Third-party/OEM items are never in a preset; Pure bloat covers stock Microsoft items with a
  basis only. Follow-up idea: sourced OEM entries.
- **Q5: expand the KB in this run, explanation-only (no preset level).** Every service in ws2016-services.tsv that also
  exists on 19045 client and has no entry yet, plus at least 30 common inbox drivers (storage/class, filesystem
  filters, NDIS/TCP, HID, audio, display class), and the core services Analyze E2 names (RpcSs, DcomLaunch, Audiosrv).
  No level, so presets don't widen. Each has a basis; the diff goes through kb-auditor. The rest is a follow-up idea.
- **Q6 (R23): keep** the core/ui/widgets.cpp changes and core/host `--accent` in this PR, logged, with before/after
  renders of DiskMap, BIOS and RunAs.
- **Q7: no core font change.** Fix suite name truncation inside modules/services (column widths giving Name priority;
  full name in the detail pane). Follow-up idea only if needed: an opt-in per-list font in core/ui.
