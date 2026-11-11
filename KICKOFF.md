# Hadron kickoff prompt

Paste everything below the line as the first message of the project thread. Pick **Claude Fable 5.2** in the model
picker and attach the floration/hadron repo.

---

You are the lead engineer on **Hadron**, my suite of native Windows utilities in raw C++ (Win32 + GDI, no
frameworks). Before anything else, read these in order:
1. `CLAUDE.md`: rules, team, precedence, and my PC.
2. `docs/HISTORY.md`: what exists, known rule violations in legacy, and the incidents behind the rules.
3. `docs/ARCHITECTURE.md`: the module contract.
4. `docs/MODULES.md`: the catalog.

**The goal:** every tool is a module that runs **standalone** (its own exe) or **inside Hadron.exe**, from the same code.
Don't trade that away for convenience. First release: `diskmap` and `bios` (with snapshots), both ways, matching
legacy behaviour except for approved fixes.

**How we work**
- You plan, own `core/` and the architecture, write specs, review and merge.
- Delegate scoped work to the subagents in `.claude/agents/`, using the spec template in CLAUDE.md. Run independent tasks in parallel.
- Keep `docs/PROGRESS.md` and `docs/DECISIONS.md` current. End every session with a handoff note in PROGRESS.md.
- Check that the config is honoured on the first delegation: the subagent model and effort (`/agents` or the run
  details), and your own model (`/status`). Note the result in DECISIONS.md.
- Check what the sandbox has: mingw-w64, cmake, wine, xvfb, ImageMagick, python3. Install what's missing.
- MSVC and real-Windows behaviour (UEFI variables, the USN journal, UAC, reparse points, DPI changes) can only be
  verified in a `windows-latest` CI job. Don't block on them locally.

**Check-ins: stop and wait for me at each one.** Between them, decide yourself and log the decision. Send a short
status at each merge: what merged, what's next, what's blocked.
1. **Phase 0 plan.** Include the core API you intend to commit to and anything in ARCHITECTURE.md you think is wrong.
2. **Core frozen.** `core/` plus the `diskmap` port running standalone and in the suite, with screenshots and golden comparisons.
3. **Deviation list for `bios`.** The rule fixes from HISTORY.md, plus anything else you found.
4. **Release candidate.**

**Phase 0: baseline.**
- Build each legacy tool unchanged.
- A subagent writes `docs/legacy/<tool>.md` for each one: function map, parity checklist, edge cases, rule violations.
- Write fixture generators in `tests/fixtures/`:
  - DiskMap: a tree of known size, with a junction, a symlink pointing outside the tree, long paths, and a hard link.
  - BIOS: a synthetic snapshot built from StdDefaults plus a few changed variables.
- Record the legacy outputs (DiskMap totals, BIOS decoded values, the export checklist) and screenshots in `tests/golden/`.

**Phase 1: foundation.**
- CMake that always builds every target, and CI:
  - Linux: MinGW cross-build, tests, Wine screenshots through `--screenshot`.
  - `windows-latest`: MSVC build plus real tests, including the junction-sentinel test for incident 2.
- Build `core/host`, `core/ui`, `core/sys` and `core/fs` per ARCHITECTURE.md, extracting the look from the legacy tools.
- Port `diskmap` as the proof. Check-in 2.

**Phase 2: `bios`.**
- Port the app.
- Move the pipeline into `tools/`, adding a `basis` to every tier-1 entry and number. `kb-auditor` reviews it.
- Add snapshots, snapshot compare and the SMBIOS version guard.

`rescue` waits for my decision.

**Phase 3: release.** `Hadron-<version>.zip` with the README (including the Unblock note). Copy it to my Downloads
if this session is linked to my PC; that is the one pre-approved action there.

Start by reading the docs, then give me the Phase 0 plan.
