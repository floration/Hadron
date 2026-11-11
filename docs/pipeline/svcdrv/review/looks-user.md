# svcdrv: Looks / user review (owner's-eye)

Reviewed: all 16 shots in design/final/, plus baseline/{plan-ti-stop, interrupted, drivers, bluetooth} and
console/{help, f9-bloat, drivers, interrupted}. Strings checked in modules/services/module.cpp and plan.cpp on
module/services-svcdrv.

Overall: dense, honest in tone, and the "nothing changes until you press Apply" message is clear in both plan
layouts. Console is the better power-user layout: one row per item with Basis and Image path visible, and the plan has a `->` arrow.
The first things I would trip over are the TI toggle looking like any other chip, "protected" in error-red,
four different "won't touch" words in the Hadron column, and the Windows-stock preset offered with no data behind it.

## Findings, most important first

1. **Cat 5 (borders on 2): TI toggle reads as harmless, and "on" looks like a selected tab.**
   All shots, top-right `TrustedInstaller: off` (Baseline) / `TrustedInstaller off` (Console), F-bar `F6 TI off`.
   module.cpp:2075-2079 draws it with the same `Chip` as Services/Drivers, and `on` uses the accent "selected" fill.
   So ON looks like a normal positive choice, with no sign that it's a SYSTEM-plus identity. `F6 TI off` in the F-bar
   also reads as an action ("press to turn TI off"). Fix: a distinct toggle style: off = outlined/muted with a
   lock glyph, on = warn/bad fill with the text "TrustedInstaller: ON (writes as TI)". Make the F-bar read `F6 TI:off`
   (the compact form already exists) or `F6 TI toggle`. Show a warn banner while it's on.

2. **Cat 2: the Start/Stop/Restart buttons stay live on items labelled "Protected: Hadron never changes this."**
   module.cpp:2171 `cen = can && kind != UserTemplate` does not check `g.neverTouch` (only the start-type row does,
   2153). Fixtures hide it, because everything is disabled. On live data you could Stop Dhcp, BFE, EventLog or AudioEndpointBuilder
   right under red text saying Hadron never changes it. Either gate live control on neverTouch, or reword the
   protection line to "Hadron never changes its start type or puts it in a preset".

3. **Cat 5: "protected" is drawn in error-red.** console-*.png Hadron column, and the detail line "Protected: ..." in
   every theme (module.cpp:2641 `badInk`, 1733 `badT`). Protected means *safe, left alone*, but red reads as
   failure or danger. With accent E81123 (console-light-accent-E81123.png) the selection bar, INFO tag, Disabled
   button and "protected" are all the same red, and the screen looks like an error page. Fix: show protected as mute/neutral text (it's
   the most common "nothing happens" state). Keep red for real hazards (TI on, failed checks). For a red accent, use
   a distinct hue for semantic bad.

4. **Cat 2/5: the Hadron column uses five different words for "won't be changed".** `protected`, `no preset`, `kept`, `—`
   (em dash), and in the plans "Kept:" / "Protected: not in presets". baseline-drivers-light.png: Beep `—`, CldFlt `no preset`,
   ExampleAudioFlt `protected`. console-drivers.png: nvlddmkm `—` although its detail says "Not in any preset". The help
   legend (console-help.png, module.cpp:2959) explains only "protected, no preset, or the lowest preset", and leaves out
   `kept` and `—`. A user can't tell `no preset` (KB says leave it) from `—` (not in KB) from `kept` (hardware
   present). Fix: rename the header to **Preset** (or "Lowest preset"). Use `kept (hw)` for a hardware-present item and
   `not in KB` instead of `—`, and add all five words to the F1 legend and to a Baseline tooltip/legend line.

5. **Cat 6/5: "Default: Windows stock" is offered, but the stock table is empty.** Every detail pane shows `Windows stock: no value
   with a measured or published basis in Hadron's KB yet` (module.cpp:1798), yet the chip/F8 is enabled like the others
   (2103). A user clicks it and expects a Windows reset. The plan's note does say "N of M items have a stock value",
   but the chip promises more than it can do. Fix: put the count on the chip ("Windows stock (0 known)") or disable
   it with a reason while the count is 0. Also align the chip text "Default: first snapshot" with the plan title "Default: restore
   first snapshot".

6. **Cat 2: wrong explanation text for third-party items in Console.** console-drivers.png nvlddmkm: the left column says
   "Not in any preset: third-party or unknown ... A single change is still possible". The right column says "No KB text for
   it; the protection above is why Hadron leaves it alone" (module.cpp:1785). That item has no protection, and in the
   Console split the text sits beside it, not "above". Fix: when neverPreset is set only because the item is third-party, use the 1782 wording.
   Otherwise say "the protection note (left/above)" with the reason repeated inline.

7. **Cat 5: the Baseline plan has no Basis column.** baseline-plan-stripped-dark.png columns: [x] / Name / Now / Planned / Why.
   Basis shows only for the selected row in the side pane, and "Why" is truncated ("Intel integrated graphics only; kept when the
   Int..."). Console's plan has Basis and `-> disabled`, and its Why fits. Fix: add a narrow Basis column (published/
   reasoning/folklore) and a `->` before Planned. Let Why take the space freed by narrowing Now/Planned.

8. **Cat 6: KB/basis prose is hard to read and quotes Microsoft's labels as if they were Hadron's words.** Detail
   "Basis" on DiagTrack: "...What breaks is Hadron's reasoning unless a source is named." (an awkward sentence that
   needs re-reading). "...gives No guidance" and "rates it Do not disable" (baseline-plan-ti-stop-light.png) have stray capitals
   because Microsoft's labels aren't quoted. Bare raw URLs make up most of the pane. Fix: quote the source labels ("No
   guidance", "Do not disable"). Rewrite the line as "The 'If it's off' effects are Hadron's reasoning unless a source is named."
   Show the URLs as short source names ("MS IoT Enterprise services page") with the full URL on hover or copy. Route this
   to kb-auditor.

9. **Cat 5: internal jargon in user text.** "System-start driver without a curated **sysok** entry" (plan.cpp:118, 247,
   320, 352) appears in plan Why. The interrupted banner shows the raw `preset:bloat:services` and stamp `20261110-213012`
   (baseline-interrupted-boring.png, console-interrupted.png). Fix: "System-start driver not on Hadron's safe list".
   Change the banner to "An apply of Pure bloat removal (Services) started 2026-11-10 21:30 did not finish".

10. **Cat 5: Name column font mismatch and truncation (Baseline).** All baseline-*.png: Name is drawn at 15px
    and the other columns at 12px, so the names look like a different widget. In the suite (baseline-suite-dark.png) this
    gives `AudioEndpointB...`, `BrokerInfrastruc...` and a header that reads `Display na...`. "Auto (delayed) ..." is cut in
    Start type even standalone (CDPSvc). This is the lead's dropped font fix, and as a user it's the first visual oddity I notice.
    Fix: take the core font change for all modules (its own core PR), or at least widen Start type so "Auto (delayed) +trigger" fits.

11. **Cat 5: the suite's Console drops the most important half of the TI caveat.** console-suite-dark.png shows only "Only for writes
    Windows refuses to administrators." and loses "Not Defender, PPL or Tamper Protection", which is exactly what a
    tuner will try TI for. Fix: keep the Defender/PPL clause and drop the first one when space is short
    ("TI can't touch Defender, PPL, Tamper Protection").

12. **Cat 5: "vs first" / "vs 1st" is cryptic.** The value `changed`/`new` means "differs from the first-run snapshot".
    Fix: header "vs snapshot" (Baseline has the room), and a tooltip or legend. In the suite, Console colours `changed`
    orange but the standalone doesn't (console-dark vs console-suite-dark). Pick one.

13. **Cat 5: `+t` and `*` are fine for Console but need the legend.** F1 explains them (good). In Baseline, "+trigger" is
    clear. Drift `*` doesn't appear in any shot, so I can't judge how visible it is.

14. **Cat 5: clipped rows and panes in Console.** console-dark/fun/boring/light: the last list row (HvHost) is
    half-drawn under the frame. In the bottom pane, "Depends on RpcSs" and "Preset basis" are cut mid-line. The scrollbar is there,
    but a half row looks like a render bug. Fix: snap the list to whole rows, and fade or clip the pane on a line boundary.

15. **Cat 5, minor:**
    - The "LIST" / "PLAN" mode tag at the bottom right of the Console F-bar isn't explained.
    - Disabled "Refresh" / "Undo last apply" in Baseline dark/fun look the same as the enabled chips.
    - The drivers start strip (ACPI, Boot) highlights nothing because there are no Boot/System buttons. Show the current
      "Boot" as a read-only pill.
    - "Company (file claim)" wraps onto two lines in the narrow key column.
    - "Fun" is only a purple tint of Dark, which is fine and nothing looks out of place in it.

## Rule 7 check
No numbers in user text except counts and dates. No estimates presented as measurements. Basis appears on KB entries
and (in Console) on plan rows. Two soft claims have no visible basis:
- "Windows stops it on its own when idle" (TI banner);
- "a desktop rarely needs it" (lfsvc Why).

Label both `reasoning`, or soften them. intelppm shows Basis `folklore` in orange next to `protected`, which is honest but
unexplained in Baseline (it has no Basis column).

## What reads well
- "Nothing changes until you press/click Apply" plus a disabled Apply count ("Apply 28 changes").
- "Start types only. Changes take effect at the next restart; Hadron never restarts the PC."
- Hardware evidence on Bluetooth ("kept by every preset: present: USB\VID_8087...").
- "Fixture data: applying is off" everywhere.
- Console's "No key applies: click Apply".
