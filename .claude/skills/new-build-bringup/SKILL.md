---
name: new-build-bringup
description: Use when the owner says "start supporting <Fortnite version>" (or "move to", "let's support") to run the full routine across the repos - promote the previous build, mark the new one in development, SDK and strip diffs, headless boots, fixes, read-only sweeps and the backend data.
---

# New-build bring-up

Depth: `docs/agents/new-build-bringup.md`. Rules: AGENTS.md sections 1 and 2 (one server at a time, test ports,
no clients, no full matches, never copy builds).

1. Read `D:\CoreWork\HANDOFF.md`. Run `project_status` (core MCP, if available) for the new build's folder, SDK,
   IDA port and CL. Else find them by hand (doc section 1).
2. Promotion (one commit per repo, each rebased and pushed):
   - Core-Launcher: `KnownBuilds.cs` const + `Builds` entry `InProgress` (BattleRoyale, Creative from 7.00); the
     previous build `Full`; tests' unsupported example and range strings; README table; run the tests; tag the next
     `v1.0.x` (Core-Launcher's `launcher-change-and-release` skill).
   - CoreV2 and Core-Backend: README status tables; `.github/ISSUE_TEMPLATE/bug_report.yml` build dropdowns.
3. SDK diffs vs the previous build's SDK (all four `sdk_*_check.py` tools, `docs/agents/finders.md`). Guard each
   dropped or changed member and UFunction.
4. `stripdiff.py --old <prev port> --new <new port> --out <scratch>\stripdiff.txt`. List new stubs Core doesn't restore.
5. Boot Save the World, then Battle Royale (`boot-test-server`). Fix only what fails. Re-boot to 0 missing and good
   loot lines. Regression-boot the previous build (plus a Season 1 build and 1.10 for shared changes).
6. Read-only sweeps: changed types and parameters (stack arguments of functions Core replaces or calls); new MCP
   commands and routes for the backend; patch-note features to server code and data; the first-match path.
7. Backend data: Core-Backend's `add-build-gamedata` skill.
8. Each fix: its own commit with the build's addresses and what was verified live vs statically.
9. Write the follow-ups you found but didn't build into `D:\CoreWork\HANDOFF.md` (or issues), and report: commits,
   boots with key lines, static-only checks, what a live match must confirm.

Done when: the new build boots STW and BR with 0 missing and loot lines passing, the previous build still does, all
three READMEs and the launcher agree on the range, and a launcher release is tagged.
