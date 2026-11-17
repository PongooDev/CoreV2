# New-build bring-up

When the owner says "start supporting X" (or "move to X", "let's support X"), do the whole routine below without
asking for the steps. The owner sets up the build folder, the SDK, the IDA instance and the FModel instance
beforehand. **Only one build is in development at a time:** starting X promotes the previous in-development build
to full support everywhere.

Core moves forward one build at a time and skips builds nobody has (no 3.4, 4.3, 5.20 or 7.01). Every change made
for X must keep 1.1 onwards working.

## 1. Find the resources

- `project_status` (core MCP) lists them. Fallback:
  - build folder: under `F:\Builds\<version>-CL-<CL>\...` or `D:\Fortnite Builds\<version>` (the one holding
    `FortniteGame` and `Engine`);
  - SDK: `C:\Dumper-7\<UE>-<CL>+++Fortnite+Release-<version>-FortniteGame`;
  - IDA: probe ports 13337 to 13342 (`ida.py --port=<p> health`, or a read-only `py_eval` of
    `ida_nalt.get_input_file_path()`);
  - FModel: the `fmodel` MCP's instance list.
- The CL and engine version are in the exe's version string (`++Fortnite+Release-7.20-CL-4727874`).

## 2. Promote the previous build, mark X in development

| Repo | Change |
| --- | --- |
| Core-Launcher | `src/Core.Launcher.Hosting/Builds/KnownBuilds.cs`: a `FortniteXY = <CL>` const with a factual one-line summary (UE version, season, release date, patch-notes link); a `Builds` entry `BuildSupport.InProgress` with `BattleRoyale: true` (and `Creative: true` from 7.00); the previous build becomes `BuildSupport.Full`. Tests: move the "unsupported" example to a build Core doesn't know and update the range strings (`BuildSupportTests`). README status table. Then build, run the tests, commit, push and tag the next `v1.0.x` (see Core-Launcher's AGENTS.md) |
| CoreV2 | README status table (range row and the per-build `<details>` row); `.github/ISSUE_TEMPLATE/bug_report.yml` build dropdown |
| Core-Backend | README status table; `.github/ISSUE_TEMPLATE/bug_report.yml` build dropdown; `src/builds/detect.js` `buildVersions` (and `buildReleases` where dated) as part of its game data step |

## 3. Gameserver bring-up (this repo)

1. **SDK diffs** against the previous build's SDK, before the first boot (`finders.md`, "SDK diff tools"): member
   names, members per class, UFunctions, and `sdk_type_check.py` for types, sizes, parameters and enums. Guard every
   dropped or changed item.
2. **Strip diff:** `stripdiff.py --old <prev port> --new <new port> --out <scratch>\stripdiff.txt`. New stubs that
   Core doesn't restore yet, and who calls them, are the likely first-match failures.
3. **Boot once** headless for Save the World and once for Battle Royale (`headless-server-testing.md`). Fix only what
   fails: missing finders, fatals, errors. Re-boot until every group says 0 missing and the loot lines pass.
4. **Regression boots:** the previous build, plus a Season 1 build and 1.10 for any change to shared code.
5. **Read-only sweeps** for what a boot never shows:
   - functions that became stubs, or new stubs, and their callers (new features calling stripped code);
   - members Core reads whose type, size or layout changed, and UFunctions whose parameters changed (Core's
     replaced natives must read every parameter); a stack-argument sweep of the functions Core replaces or calls;
   - new client-to-backend requests: MCP command names and route strings in the new exe that Core-Backend doesn't
     answer (hand these to Core-Backend);
   - each patch-note feature traced to the server code and data it needs: new items, LTMs and their playlist
     mutators, map events and their calendar flags, server-only Blueprints that must be in the server-assets pak;
   - the first-match player path, read statically: join, loadout and inventory, quickbar, interact, quests,
     replication with a client connected, match start, elimination, results; Save the World login and mission start
     and end.
6. Commit each fix on its own, with the build's addresses (`7.20 0x...`) and what was verified live or statically.

## 4. Backend data (Core-Backend)

The build's game data (`gamedata/<CL>`), version name, events and patch-note changes: see Core-Backend's
`docs/agents/gamedata.md` and its `add-build-gamedata` skill. Once it is on main, restart the owner's 5595 backend
(core MCP `restart_backend`, or Core-Backend's `docs/agents/restarting-the-live-backend.md`).

## 5. Server assets

Check Core-ServerAssets' `manifest.json` for the CL. A boot without the pak uses Core's fallbacks (POI volumes "not
placed", embedded loot tables). Don't copy a pak into the build folder; the launcher installs it.

## 6. Limits

- Server boots strictly one at a time; test backend ports only; never 5595; no client launches; no full matches.
- Read-only sweeps can run in parallel, but the IDA instances are shared: keep it to a few at once.
- Every writer works in its own worktree and stages only its own paths.

## 7. Report

List: commits per repo; each boot (build, mode, result, the key log lines); what was only checked statically; the
follow-ups found but not built (they become issues or `HANDOFF.md` items). A bring-up can't replace a live match:
say what stays unconfirmed. Per-build facts for the knowledge base go to the owner (or `HANDOFF.md`); agents don't
edit the knowledge base.
