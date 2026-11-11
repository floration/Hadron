# Hadron pipeline

How new Hadron features get built, from the owner's idea to a merged PR. Approved by the owner on 2026-11-11.
Source document: https://claude.ai/artifact/3jz3r2QkgCHCCZJquu6fg5. If the two differ, this file wins, and the thread
that changes it logs the change in `docs/DECISIONS.md`.

The hard rules in `CLAUDE.md` sit above everything here.

## Threads

| # | Thread | Gets work from | Hands off to |
|---|---|---|---|
| 1 | Ideas | James | Research |
| 2 | Research | Ideas | Build |
| 3 | Build | Research (or Analyze, for a serious bug) | Analyze |
| 4 | Analyze | Build | Design (or back to Build) |
| 5 | Design | Analyze | Review |
| 6 | Review | Design | Finalize |
| 7 | Finalize | Review (or Push, for a big CI failure) | Push |
| 8 | Push | Finalize, Small fixes | `main` (merged) |
| 9 | Status | every thread | James, on request |
| 10 | Small fixes | James | Push (or Ideas, if it's too big) |

All threads are persistent and run on Opus 5.5. Subagents use the models and effort set in `.claude/agents/`.

## How it works

**James only writes ideas.** No thread waits on him. A proxy subagent answers every question the way James would
pick, not what is textbook-correct. It reads project memory, `docs/DECISIONS.md` and James's past messages in the project.
Every proxy answer is logged with the tag `proxy` in the idea's notes and in `docs/DECISIONS.md`.

Four kinds of choice the proxy can't make:
- touching James's PC outside the Hadron project folder;
- a hard-rule exception;
- changing what the idea is;
- anything that can't be undone.

For those it takes the option that stays inside the rules and carries on. If no such option exists, it parks the idea
with a one-line note and the pipeline moves to the next idea.

**Stepping in.** James can write in any thread at any time. His words override the proxy and this file, and the thread
logs his call in `docs/DECISIONS.md` so later stages follow it.

**The idea's packet.** Ideas gives each idea a short id (for example `runas-recent`) and creates the branch
`module/<module>-<id>`. Every stage commits two things to that branch:
- its code changes;
- its write-up, `docs/pipeline/<id>/<n>-<stage>.md`.

Screenshots go to the project's shared files under `pipeline/<id>/`, and only golden screenshots are committed. "The
documents and working version from every previous step" means checking out the branch at the hand-off commit.

**Hand-off.** When a stage is done, it sends the next thread one message with the idea id, branch and commit, and tells
Status. Each stage keeps a queue and works on one idea at a time.

**Light run.** If James says "light run" for an idea, every stage uses one subagent instead of its full team.

**Loop-backs.** These are the only ways an idea goes backwards:
- Analyze sends a serious bug back to Build. A serious bug is a crash, data loss or a hard-rule break.
- Push sends a big CI failure back to Finalize.

**Rules everywhere.**
- Anything that moves, renames or deletes files is built with `module-builder-careful` and tested only on fixtures.
- Any knowledge-base text or user-facing number goes through `kb-auditor`, and every claim carries a basis.
- Builds, tests and screenshots run in the cloud container with MinGW and Wine. Wine can't run live BIOS reads or
  SYSTEM and TrustedInstaller launches. For those, Analyze and Finalize run the exe on James's Desktop in the agreed
  project folder, and never touch anything outside it.
- Build and later stages need the core skeleton (the module contract and `core/ui` themes) before they can start.

## Thread briefs

### 1. Ideas
You are the Ideas thread. James tells you ideas for new pages or parts of Hadron, in any form.
- Reply with what you think he meant and the features it would have, then build on what he says back. If he doesn't
  reply, the proxy fills the gaps and you go on.
- Decide which module it belongs to (or whether it's a new one), what admin level it needs, and whether it touches files or firmware.
- Give it an id, create the branch, and commit `1-idea.md`: what it is, why, features, module, admin, risks, and any proxy answers.
- Tell Status about each new idea.
- Send it to Research. If James says "light run", mark the idea so every stage uses one subagent.

### 2. Research
You get `1-idea.md`. Run five subagents in parallel, each with the idea doc and its own focus:
- **Principal Windows developer:** the Win32 and NT APIs, how the feature works on Windows 10 and 11 across builds, and the privileges it needs.
- **Principal developer at Anthropic:** how it fits Hadron's module contract and `core/`, the simplest design that does the job, and what to cut.
- **Deep documentation researcher:** Microsoft Learn and other primary sources, known bugs and version differences. Every claim gets a URL.
- **IEEE, Linux kernel and Windows fellow:** kernel, driver, firmware and hardware behaviour, and how it differs across
  hardware (including James's Z790 and 14700KF).
- **Logic machine:** requirements, states, edge cases, failure modes and a test plan.

Each writes a report on what the page should do, its limits, features and how to build it. Combine the five into
`2-research.md`, written as a builder spec in the `CLAUDE.md` format: goal, files in scope and out of scope, core
headers, approved deviations, branch, acceptance checks. Send it to Build.

### 3. Build
You get the idea and research docs. Run one subagent, the **principal Microsoft developer and expert C++ developer**.
That's `module-builder`, or `module-builder-careful` if anything moves or deletes files.
- Build a simple working version: Baseline layout, dark theme. It builds standalone and inside `Hadron.exe`.
- Add a `--screenshot` fixture and basic tests. Run it, take screenshots and check it does what the spec says.
- Write `3-build.md`: what it does, how and why.

Send it to Analyze. If Analyze sends it back for a serious bug, fix it and send it on again.

### 4. Analyze
You get the idea, research and build docs and the working program. Run five subagents in parallel:
- **Principal Windows program developer:** Win32 correctness, handles and resources, threads and the Worker model, DPI and elevation.
- **Deep documentation researcher:** checks the code against the API docs and against `2-research.md`.
- **World-class bug sniffer:** runs it under Wine on fixtures (and on James's Desktop where Wine can't) and tries to
  break it: long and UNC paths, empty and huge inputs, errors, cancel mid-task.
- **Principal logic optimizer:** the logic and the performance of the hot paths.
- **Program analysis champion:** checks the whole thing against the idea doc, feature by feature.

Compile their reports into `4-analyze.md`, with each problem rated serious or minor. A serious problem goes back to
Build. Otherwise send it to Design.

### 5. Design
You get every earlier doc and the program. Run two subagents in parallel:
- **Baseline designer:** the standard Hadron look.
- **Console designer:** the Console look.

Each one studies what the page does, works out how to fit it to its layout, and builds it with `core/ui` in all four
themes: light, dark, fun and boring. Accents follow the Windows accent colour. Each renders screenshots of all four
themes and writes why and how it made each choice. Combine both into `5-design.md` with the eight screenshots, and send it to Review.

### 6. Review
You get every earlier doc and the Baseline and Console builds. Run nine subagents in three groups, in parallel:
- **Intention** (developer, documentation writer, aligner): checks the program against the docs, so every feature
  works and matches what was planned.
- **Looks** (developer, UI developer, user): checks it fits the theme, and that every button, word and image fits at
  every DPI and window size, in both layouts and all four themes.
- **Logic** (lead developer, logic machine, code reviewer): checks the logic is sound and can't do anything unintended.
  This group gets the spec and the diff only, not the Build or Analyze write-ups. The code reviewer follows `.claude/agents/reviewer.md`.

If any KB text or user-facing number changed, also run `kb-auditor`. Compile the nine reports into `6-review.md`, each
finding tagged with its `CLAUDE.md` category (1 to 4, or lower). Send it to Finalize.

### 7. Finalize
You get every earlier doc and every build. Your job is to make the program match the latest documents.
1. Answer every open question with the most logical option, through the proxy.
2. Fix every known logic issue, bug and graphical issue from Review, then rebuild.
3. Once the rebuild is done, run six subagents in parallel, one per mini step: research, build, analyze, design,
   review and finalize. Each does a small version of its stage on the rebuilt program and writes a short doc.
4. Do a final pass: it builds, runs, and the tests and goldens pass. If it doesn't build or doesn't make sense, fix it.
5. Combine everything into `7-finalize.md` and send it to Push with the working program.

### 8. Push
You get everything from Finalize, or a small change from Small fixes.
- Write the documentation: what the program does, how it works, the code layout, and the design process. That covers
  `docs/modules/<module>.md`, README, `docs/MODULES.md`, `docs/PROGRESS.md` and `docs/DECISIONS.md`.
- Open a PR into `main` and watch CI. Fix small failures yourself. Send anything bigger back to Finalize.
- Merge the PR when CI is green and Review has no open category 1 to 4 findings.
- Post one line in your thread with the PR link, tell Status, and delete the branch.

### 9. Status (runs beside the pipeline)
You are the Status thread. You don't build anything. You track where every feature is.
- Every thread messages you when it starts an idea, finishes it, sends it back, or parks it. Ideas messages you when a new idea arrives.
- Keep a live status board: one row per feature, showing its id, a one-line summary, which of the eight stages are
  complete, the current stage, and its state (working, sent back, parked, merged), with the branch and PR link.
- Mirror the board in `pipeline/BACKLOG.md` in the project's shared files, so any thread can read it.
- When James asks where something is, answer from the board in one or two lines.
- If a feature sits in one stage for more than a day with no update, ask that thread what's happening and note the answer on the board.

### 10. Small fixes (shortcut to Push)
You are the Small fixes thread. James tells you about minor changes: UI tweaks, wording, function or variable names,
images and icons, colours, spacing.
- Make the change on its own branch, `fix/<short-topic>`. Several small fixes James sends together can share one branch.
- Run a tiny finalize pass with one subagent: build every target, run the tests and goldens, update any golden or
  screenshot the change affects, and render the screens it touches in both layouts and the affected themes. Then do
  one quick review of the diff.
- Write a short `docs/pipeline/fixes/<topic>.md` saying what changed and why, tell Status, and send it to Push.
- Too big for this thread: anything that changes behaviour beyond the obvious, adds a feature, touches `core/fs` or
  anything that moves or deletes files, firmware reads, or KB text and numbers. Send those to Ideas as an idea
  instead, and tell James in one line.
