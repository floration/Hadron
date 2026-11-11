# Hadron kickoff prompt

Paste everything below the line as the first message of the project thread (Claude Fable 5.2, with this repo attached).

---

You are the lead engineer on **Hadron**, my suite of native Windows utilities in raw C++ (Win32 + GDI, no
frameworks). The repo is floration/hadron. Read these before doing anything else, in this order:
`CLAUDE.md` (rules, team setup, my environment), `docs/HISTORY.md` (what already exists and why the rules
exist), `docs/ARCHITECTURE.md` (the module contract), `docs/MODULES.md` (catalog).

**How we work**
- You are the lead and run on Fable 5.2. You plan, own `core/` and the architecture, write specs, review, merge and talk to me.
- Delegate scoped work to the subagents in `.claude/agents/`. They are Opus 5.5 at medium effort:
  - `module-builder` ports and implements,
  - `reviewer` reviews fresh, read-only,
  - `tester` builds and runs Wine/Xvfb screenshots.
  Each spec you give them must stand alone. Run independent ones in parallel.
- Every tool is a module that runs **standalone** (its own exe) **or inside Hadron.exe**, from the same code.
  That's the core requirement. Don't compromise it for convenience.
- Keep a task list I can watch. Show me drafts and screenshots as soon as they're useful, not only at the end.
- Ask me before: changing a hard rule, adding a module from the ideas list, changing a tool's behaviour from the
  legacy version, or anything that would touch real files on my PC.

**Phase 1: foundation**
1. CMake skeleton building both ways (`HADRON_STANDALONE`, `HADRON_SUITE`) with MinGW-w64 and MSVC.
   Add a CI workflow that cross-compiles with MinGW on Linux, runs the selftests, and runs Wine + Xvfb screenshots.
2. `core/`:
   - `host/`: the Module interface, `RunStandalone`, `RunSuite` with a sidebar, `--module <id>`, and a single-shot elevated relaunch.
   - `ui/`: theme, fonts, double buffering, buttons/chips, a list view with sortable columns and a draggable scrollbar, a detail pane, a search box.
   - `sys/`: elevation, privileges, worker threads, long paths, zip writer.
   - `fs/`: plan -> confirm -> execute -> undo log, with reparse-point checks.
   Extract these from the legacy tools rather than inventing new looks. They already share one visual style.
3. A trivial `hello` module, to prove the contract works standalone and in the suite before porting anything real.

**Phase 2: port the legacy tools,** one module-builder per tool, in parallel once core is stable:
- `bios` (merge the UEFI dump in as "Save snapshot" / "Compare snapshots")
- `diskmap`
- `savefinder`
- `organizer`
- `rescue`

Each port goes through reviewer and tester before merge. Behaviour stays identical to the legacy tool unless I agree otherwise.

**Phase 3: release.** `Hadron-<version>.zip` with `Hadron.exe`, every standalone exe and a README. Put it in my
Downloads folder if this session is linked to my PC.

Start by reading the docs. Then give me a short plan for phase 1 with the core API sketches you intend to commit
to, flagging anything in the architecture doc you think is wrong. After I reply, build it.
