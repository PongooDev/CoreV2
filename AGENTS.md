# CoreV2: instructions for coding agents

CoreV2 is a Fortnite gameserver (`Core.dll`) that runs inside the game's own exe, plus a client DLL
(`CoreClient.dll`) and an HTTP redirect DLL (`CoreRedirect.dll`). One DLL serves every supported build, from 1.1
onwards. This file is for any coding agent working here (Freebuff, Codebuff, Claude Code or another). Read all of
it before the first change. Open the linked `docs/agents/*.md` files when a task touches their topic.

Hard rules are marked **must** or **never**. If a rule here and a request seem to conflict, follow the rule and say
why in your report.

Paths: `%USERPROFILE%` is the owner's Windows user folder. In PowerShell write it as `$env:USERPROFILE`, as the
command examples do.

## 0. Start of every session

1. Read `H:\CoreWork\HANDOFF.md` (local, shared by every agent, not committed). Don't redo or undo work listed in it.
2. Run the core MCP's `project_status` tool (if available). It says which build is in development, the supported
   range, the installed builds, the SDKs and which IDA port holds which exe. Without it, read the README status
   table, Core-Launcher's `src/Core.Launcher.Hosting/Builds/KnownBuilds.cs`, and the knowledge base below.
3. Work in your own git worktree under `H:\CoreWork` (see section 5). Never edit the main checkout at
   `%USERPROFILE%\Documents\GitHub\CoreV2` directly.
4. If you can't finish in this session, update `H:\CoreWork\HANDOFF.md` before you stop: what is in progress, the
   branch or worktree, what was verified (live or static), what's next.

**Read-only knowledge base for per-build facts** (paths, IDA ports, CLs, past findings):
`%USERPROFILE%\.claude\projects\<the CoreV2 project folder>\memory\MEMORY.md` and the files it links. The folder name
is the CoreV2 checkout's path with `\` and `:` turned into `-`. Agents may read it. **Never edit it.**

## 1. Project rules (copy of CLAUDE.md)

The block between the markers is a byte-for-byte copy of the owner's `CLAUDE.md` (local and untracked; agents that
find `AGENTS.md` don't read `CLAUDE.md`). **This block must stay in sync with CLAUDE.md:** when a rule changes,
change both in the same session.

<!-- BEGIN CLAUDE.md copy -->
# CoreV2: doing it the right way

The owner wants this gameserver built the way Epic built Fortnite's server. Shortcuts that work aren't good enough. Every change, by me or an agent, follows these rules.

## Where code goes and how it runs
- **Epic's place, Epic's name.** Put code in the class and file where Epic has it, with Epic's function name, signature and flow. The reference is the 21.40 PDB exports at `D:\ResortWork\IDA\21.40`, plus IDA of the build in question.
- **The game's own trigger.** Make behaviour run from the moment the game itself runs it:
  - restore the stripped function the game calls there (vtable swap, native replacement, Core's stripped-call redirect);
  - or bind the delegate or event the game broadcasts;
  - or use the engine's FTimerManager timer;
  - or an async callback.
- **No polling loops.** No GameThread::Enqueue loops that re-queue themselves to watch state every frame or every second. If the build truly cut the call site out, a fallback is allowed only with a comment saying what Epic calls and why it can't be used.
- **No hooks on intact engine code.** Only stripped functions are restored. Intact code is reached by calling its APIs.

## Accuracy and code style
- **Accuracy first.** Preserve each build as Epic shipped it. Keep the build's own bugs; fix them only if they make the game unplayable. From 8.30 the player bots are Epic's own (Phoebe), so Core's bot backport only gets needed fixes.
- **Unreal Engine code.** Write it like Epic's code: TArray, TMap, TSet, FString, FName, TFunction, TOptional, not std:: containers. Convert std:: containers in code you touch. Keep it clean and readable.
- **Comments only where they help the next agent:** addresses, why a gate exists, non-obvious engine facts. No explanations written for the owner.
- **Doubtful reports get little effort.** Build behaviour, things never in the game, and things not confirmed broken get a short comment asking the reporter to confirm on Core with the build and logs, or are closed as the build's own behaviour. Spend agent time only on confirmed, Core-caused problems.

## Safety
- **Don't break older builds.** Every change must keep 1.1+ working: gate on classes, properties and finders existing, keep finders additive (old patterns first), and keep fallbacks.
- **No CoreClient.dll fixes.**
- **No Fortnite client launches** unless the owner OKs it. **No full-match runs.** Headless server boots via `Core-Docs/Tools/run_server_dev.py` on a test backend port are fine; never use 5595.
- **Report honestly.** Say what was only checked statically and what a live match still has to confirm.
<!-- END CLAUDE.md copy -->

## 2. Operating rules on the owner's PC

These protect the owner's machine, builds and live setup. They apply to every agent and every tool.

- **Never** launch a Fortnite client. **Never** run a full match (no `-CoreBotMatch` matches played out, no
  multi-player tests). Boot-only headless servers are allowed (section 4).
- Headless servers only against a **test backend port** (5711 or another free one). **Never use port 5595**: it is
  the owner's live backend.
- Run **one game server at a time**: never while another test server runs. Building and booting while the owner
  plays is allowed (owner, 2026-11-23).
  Put the check and the launch in one command (see `docs/agents/headless-server-testing.md`).
- **Never** modify, copy, hard-link or overlay anything in `F:\Builds` or `D:\Fortnite Builds`. Copied builds raise
  invisible Windows Firewall prompts that get stuck and dim the owner's screen. Run servers straight from the
  installed build folders. Core's own logs next to the exe (`Server_log_<pid>.txt`, `Redirect_log_<pid>.txt`) are the
  only files that may appear there.
- **Never** kill a process you didn't start. Stop every process you did start (servers, test backends, IDA servers,
  test runs) before you finish. The one exception is restarting the owner's Core-Backend on 5595 after a backend
  change on main, by the exact procedure in Core-Backend's `docs/agents/restarting-the-live-backend.md`.
- **Pre-commit hooks:** `tools/githooks/pre-commit` runs on every commit (enabled per clone with
  `git config core.hooksPath tools/githooks`; this checkout has it on). It blocks pak AES keys, private keys, files
  over 5 MB, build output and dependency folders, binaries, game files, agent-local files (`.claude/settings.local.json`,
  `.claude/worktrees`, `.freebuff/project-id`) and project files saved with a newer toolset than v143 (Visual
  Studio 18), and warns when a new source file isn't in the projects. **Never** bypass it (`--no-verify`); fix what it
  flags.
- **No CoreClient.dll changes** unless the owner explicitly OKs that specific change. Fix it on the server
  (`Core.dll`) or in Core-Backend instead. If only a client change would fix it, say so on the issue.
- **Never** print, copy or commit pak AES keys.
- Run any Python that reads downloaded or untrusted files with `python -I`.
- Work in git worktrees under `H:\CoreWork`; build only there, never in the main checkout. The owner's launcher
  loads the DLLs in the main checkout's `x64\Release` (`%USERPROFILE%\Documents\GitHub\CoreV2\x64\Release`), so
  **after every clean full build, copy `Core.dll`, `CoreClient.dll` and `CoreRedirect.dll` there** (and their
  `.pdb` files when no `FortniteClient*` process is running), before you finish. Copy only after the build has all
  three outputs and 0 errors.
- Rebase onto `origin/main` right before you push. Other agents push to this repo too.
- Never change Windows, firewall or security settings.
- Report honestly: what was checked live (a boot, a log line) and what was only checked statically.
- Close an issue only with a comment that names the fixing commit (section 7).

## 3. Repo map

All three DLLs compile the same shared tree, the way Epic builds one source tree as several targets. Each target
adds its own folder and defines (`UE_SERVER=1;WITH_SERVER_CODE=1` for the server only).

| Path | What lives there |
| --- | --- |
| `Core/Public`, `Core/Private` | Core's own runtime, shared by all DLLs: `Finder` (code search primitives), `Resolve.cpp` (every finder, about 18k lines), `Hooks` (Detour, SwapVTable, RedirectCall, RedirectAddress, WriteBytes, SkipBranch, HoldCode/ReleaseCode), `Reflection` (DefineUnrealClass, DefineUProperty, DefineUFunction...), `Natives.h` (resolved addresses and slots), `ObjectLayout` (per-engine layouts), `Packed` (packed 1.9.1+ exes), `GameThread`, `Log`, `CrashLog`, `BackendRequest`, `Configuration` |
| `Engine/Source/Runtime/<Module>/{Classes,Public,Private}` | Unreal Engine classes, in UE4's own layout (e.g. `Engine/Source/Runtime/Engine/Private/TimerManager.cpp`) |
| `Engine/Plugins/Runtime/...` | GameplayAbilities, ReplicationGraph |
| `FortniteGame/Source/FortniteGame/{Public,Private}/<Area>` | Fortnite's game classes: `Athena` (Battle Royale), `Athena/AI` (player bots), `Athena/Mutators`, `Building`, `Creative`, `Items`, `Missions`, `Player`, `Pawns`, `Abilities`, `AI`, `Quests`, `Minigame`, `Online` |
| `FortniteGame/Plugins/...` | OnlineSubsystem, OnlineSubsystemMcp, McpProfileSys, SidecarSys, Vivox |
| `Server/` | `Core.dll` target: `Boot.cpp` (start-up, `InstallHooks`) |
| `Client/` | `CoreClient.dll` target. No changes without the owner's OK |
| `Redirect/` | `CoreRedirect.dll` target: HTTP redirect to Core-Backend, XMPP |
| `ThirdParty/` | MinHook (git submodule), Memcury |
| `.github/workflows/build.yml` | CI: builds Release x64 for pull requests and pushes; a push to `main` also publishes a `build-N` release |
| `tools/githooks/pre-commit` | the pre-commit guardrails (section 2) |
| `.github/ISSUE_TEMPLATE/` | Issue forms (their build dropdown gets each new build) |

**Epic-style placement.** A method of `AFortGameModeAthena` goes in
`FortniteGame/Source/FortniteGame/Private/Athena/FortGameModeAthena.cpp`, declared in
`Public/Athena/FortGameModeAthena.h`, not in a helper elsewhere. Look the class up in the 21.40 PDB exports to find
its module and name. Core-only glue (finders, hook installers, cheats, bots) still follows Unreal naming.

**Adding a source file:** add it to all three projects and their filters (`Server/Server.vcxproj`,
`Client/Client.vcxproj`, `Redirect/Redirect.vcxproj` and each `.vcxproj.filters`).
`python -I $env:USERPROFILE\Documents\Core-Docs\Tools\sync_vcxproj.py <worktree>` regenerates them from the folder tree.

## 4. Build and test commands

Prefer the core MCP's tools (if available). The hand-typed commands are the fallback.

| Task | core MCP tool | Fallback |
| --- | --- | --- |
| Learn the current state | `project_status` | README table, `KnownBuilds.cs`, MEMORY.md |
| Build a worktree | `build_corev2` (`worktree` required) | MSBuild below |
| Boot a headless test server | `boot_test_server` (`build_path`, `mode`, `backend_port`, `dll_dir` required) | `docs/agents/headless-server-testing.md` |
| Read a server log | `server_log_summary` (log path) | grep for the lines in section 6 |
| Name crash frames | `symbolize_crash` (DLL path, offsets) | `docs/agents/crash-symbolising.md` |
| Find a type or member in an SDK | `sdk_find` (query, build or CL) | grep `C:\Dumper-7\<engine>-<CL>+++...\CppSDK\SDK` |
| Find a 21.40 name or layout | `pdb2140_find` (query) | grep `D:\ResortWork\IDA\21.40\symbols_21.40.txt`, `types_21.40_layout64.h` |
| Put main's build where the owner plays it | `deploy_main_build` | `docs/agents/building.md` |

**Long-running tools return a `job_id`.** `build_corev2`, `deploy_main_build`, `boot_test_server` (and Core-Backend's
`backend_tests` and `restart_backend`, Core-Launcher's `launcher_tests`) run in a worker process. A call waits up to
about 100 s; if the job isn't done, it returns a `job_id`: call the same tool again with just `job_id` (the other
arguments can be left out) until it reports the result. Jobs survive a restart of the MCP server. `build_corev2` and
`deploy_main_build` share one build slot, so builds run one at a time.

**Build (always the Visual Studio 2022 MSBuild, by full path, Release x64):**

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Core.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
```

- While iterating, build only the server: add `-t:Server`. Build the full `Core.sln` once before the final push.
- **Never** use `vswhere -latest` or any other MSBuild. The PC also has Visual Studio 18, whose compiler doesn't
  match the precompiled headers and fails every file with C1853.
- After any failed link, check that `x64\Release\Core.dll`, `CoreClient.dll` and `CoreRedirect.dll` still exist. A
  failed link (LNK1201) deletes them.
- More build pitfalls (stale objects after a header change, restored files with old timestamps):
  `docs/agents/building.md`.

**Tests:** there is no unit test suite. Verification is: a clean Release build, reading the code paths against the
build's IDB and SDK, and a headless boot of the affected builds (`docs/agents/headless-server-testing.md`).

## 5. How a change is done here

1. Read `H:\CoreWork\HANDOFF.md` and run `project_status`.
2. Read the issue in full: build, mode, log, Core commit. If it's the build's own behaviour (the game did it that
   way in that version), it's rejected, not fixed (`docs/agents/issues-and-prs.md`).
3. Make a worktree from a fresh `origin/main`:
   ```
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 fetch origin
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree add -b fix-<issue> H:\CoreWork\wt-<issue> origin/main
   ```
4. Find the cause with evidence, not names alone:
   - symbolise a crash offset against the matching commit's PDB (`docs/agents/crash-symbolising.md`);
   - read the code at that moment in the build's IDB, read-only (`docs/agents/reverse-engineering.md`);
   - check the SDK for every member, UFunction and delegate you will touch, on the build in question and on old ones.
   Stop as soon as the cause is proven. Don't chase every lead.
5. Decide where the fix goes: Epic's class and file, Epic's names (21.40 PDB). Restore the stripped function the
   game calls at that moment (`docs/agents/restoring-stripped-functions.md`). No polling loops, no hooks on intact
   engine code, no client fix.
6. Write it so every older build keeps working: gate on the class, member, UFunction or finder existing; old finder
   patterns first, new ones added after (`docs/agents/finders.md`).
7. Build `-t:Server` while iterating, then the full `Core.sln`.
8. Boot-test (one server at a time, test backend port, installed build): the build the issue is about, plus the
   regression boots in the definition of done. Read the log lines in section 6.
9. Run the self-review: the `pre-commit-review` skill, or section 7 and section 8 of this file by hand.
10. Commit only your own paths (never `git add -A`), with the message rules in section 7.
11. `git fetch origin && git rebase origin/main`, build again if the rebase brought changes, then
    `git push origin HEAD:main`.
12. Comment on the issue with the commit and what was verified; close it if it's fixed.
13. Run `deploy_main_build` (if available) so the owner's launcher gets the fix. Without it, note in
    `H:\CoreWork\HANDOFF.md` that main needs a build and deploy.
14. Copy the latest DLLs to the main checkout's `x64\Release` (the owner's launcher loads them): the three DLLs from
    your worktree's `x64\Release`, and the PDBs when no `FortniteClient*` is running. Never run MSBuild in the main
    checkout.
15. Remove your worktree and branch: `git worktree remove H:\CoreWork\wt-<issue>` and `git branch -D fix-<issue>`.

## 6. Definition of done

A change is done only when all of these hold:

- [ ] The cause was proven from the build's own code, log or crash offsets, and the commit message says how.
- [ ] The code is in Epic's class and file with Epic's names, and runs from the game's own trigger.
- [ ] Nothing new hooks intact engine code, polls, or touches CoreClient.
- [ ] Every new member, UFunction, class or finder is gated, so builds without it skip the code instead of hitting
      `Log::Fatal`. Build-specific UFunctions use `FindStaticFunction_X()` (returns null), never
      `StaticFunction_X()` (fatal when missing). Members use `IsAvailable()`.
- [ ] Full `Core.sln` builds clean with the VS 2022 MSBuild, and the three DLLs exist.
- [ ] Headless boots, one at a time: the affected build, plus for shared code one Season 1 build (1.7.2 or 1.8,
      where installed), 1.10, and the previous build. Builds that aren't installed are listed as "not booted".
- [ ] Each boot log shows: `Server ready`, every `Finder: ... resolved, 0 missing` and `Reflection: ... 0 missing`,
      no `[Fatal]`, and no new `slot none`, `not found` or `isn't hooked` line. Battle Royale boots also show
      `Floor loot: N containers spawned M pickups` with N > 0 and `World containers: X (Y with a loot tier)` with
      Y > 0. "0 missing" alone isn't enough: optional finders that resolve to none don't count as missing.
- [ ] Every new finder's own boot line was read and shows the address or slot it found.
- [ ] The report and commit message say what was verified live (which boots, which lines) and what only statically.
- [ ] Rebased on `origin/main`, pushed, issue commented, worktree removed, no process of yours still running.
- [ ] The three DLLs from the full build are copied to `%USERPROFILE%\Documents\GitHub\CoreV2\x64\Release` (and the
      PDBs when no `FortniteClient*` was running), so the owner's launcher has the latest build.

## 7. Commits, issues and hand-off

- Subject: `<build or area> (<#issue>): <what changed, in plain words>`, e.g.
  `6.31 Team Rumble (#460): score eliminations through the mutators' OnPostEarnedKill`.
- Body: the cause and the evidence (addresses with the build's CL, SDK offsets, log lines), what the fix does, how
  older builds are gated, and **what was verified live versus statically**.
- **Trailer:** commits made by Freebuff or Codebuff end with the line `Agent: Freebuff`. They must not use Claude's
  `Co-Authored-By` line. Each Freebuff commit is reviewed later, so the message must say what was verified live and
  what only statically.
- Close an issue only with a comment that names the fixing commit and what was verified. Never close one with
  "should work now", without a commit, or without a comment. Reopen it if the reporter says it still happens.
- Rejected issues (the build's own behaviour, out of scope) get a short comment saying why, then close as not planned.
- **One issue, several problems** (in its body or later comments): don't ask the reporter to refile. Keep the original
  for its first problem. Open a new issue for each extra problem under the owner's account (gh is logged in as the
  owner). Start it "Split out of #N (reported by @name)" and give the build, the mode, what happened and what should
  happen, with the reporter's screenshots and log links. Comment the new numbers on the original, then fix each one
  (owner's rule, 2026-11-19).
- **MrPodge6's decisions count as the owner's** on issues and PRs, from 2026-11-17 on (not earlier ones): reopening,
  closing, approving a change, including a CoreClient change on his issue. The safety rules above still apply.
- Unfinished work goes in `H:\CoreWork\HANDOFF.md`, never in a pushed WIP commit or branch.

**Pull requests: review, fix, merge.** Agents review every open PR, make it right and merge it themselves (the
owner's rule). Check it out in a worktree (`gh pr checkout`), review it against sections 1 and 6 (Epic's names,
places and flow from the 21.40 PDB and IDA; only stripped functions restored; no polling; additive finders; 1.1+ kept
working), rework anything that isn't Epic-accurate or is wrong (push to the PR branch when maintainers can edit it;
otherwise merge first and fix on main right after, in the same session), build and boot-test where relevant, merge
with `gh pr merge`, and comment on the PR with what was changed and why. Reject a PR only when it adds something the
project has ruled out (bot features beyond the Phoebe scope, CoreClient changes without the owner's OK), with a
comment. Steps: the `review-and-merge-pr` skill and `docs/agents/issues-and-prs.md`.

## 8. Mistakes made here before (don't repeat them)

Each of these happened in this repo and had to be reverted or redone.

1. **Wrong diagnosis, fixed-offset fallback (#457).** 0999899e called a 6.31 crash a missing-member fatal and read
   `NetUpdateFrequency` at hard-coded offsets behind a `NetDormancy == 0x119` gate. The log had no `[Fatal]` line
   and reflection resolved both members. The real cause was `FNetworkObjectInfo`'s layout moving in 6.30
   (307ec90a, which reverted it). **Do** symbolise the faulting offset and read the faulting instruction first. **Don't**
   claim "missing member" without a `[Fatal]` line naming it, or gate on values you have no evidence for.
2. **"Fixed" without the build's binary (#460 Team Rumble).** f938877e and 9ff21c11 wrote `TeamScore` from Core.
   The HUD reads `MutatorGenericInt_0/1`, the game's own `AFortGameModeAthena::Tick` writes `TeamScore`, and the
   kill is scored in the TDM mutator's `OnPostEarnedKill`. Reverted in 43bfff04, fixed in 4a9510da by restoring the
   stripped call. **Do** find what the game does at that moment in that build's IDB (and FModel for Blueprints).
   **Don't** write replicated state the game maintains itself.
3. **Restoring something that isn't stripped (#461 mounted turret).** 95bf975d "restored"
   `AFortAthenaVehicle::ServerStartFire_Implementation`, which is intact in 6.22, 6.30 and 6.31, with 21.40's body.
   It dropped a base-class gate (the shared empty function is also what deliberately empty overrides fold into),
   and ec1cf3d7 read a component and bound delegates that no supported build's SDK has. Both reverted (1720a462,
   e414c4fc). The real fix was one line of the stripped `EquipAbilityWeapon` (5fd5282a: load the weapon's clip).
   **Do** prove the slot holds the stub in that build before restoring, and check every member in the SDK.
   **Don't** paste 21.40 bodies unchanged, and don't remove existing gates.
4. **"Stripped" vs "doesn't exist yet" (196e80c8).** It called `K2_ProjectPointToNavigation` stripped on 1.8.2. It
   doesn't exist before 1.10 (UE 4.19 added it), so its fallback treated every point as on the navmesh on every
   pre-1.10 build. It also guessed `PlaylistName`'s type at runtime. Fixed in 80dbe254. **Do** check old builds'
   SDKs and exes for whether a function or member exists at all, and declare members on the class the SDKs show.
5. **Issues closed without a fix or a comment.** #457 was closed with no comment; #463 with "should work now" and
   no commit; #460 and #461 were closed as fixed and had to be reopened. **Do** close only with a comment naming
   the commit and what was verified.
6. **Build-specific lookups that are fatal on old builds.** 8cce04a used `StaticFunction_X()` for a 1.10-only
   UFunction and crashed every 1.9 Battle Royale server at boot (a43e1b5). 2.2 dropped
   `AFortPlayerControllerAthena::DefaultHeroes` and the server died at the first join, which a boot never shows.
   **Do** use `FindStaticFunction_X()` and `IsAvailable()`, and run the SDK diff tools on every new build.
7. **Regression boots too narrow.** Shared changes for 4.1 and 4.2 were only booted on 3.3 to 4.1 and broke
   1.7.2 to 1.9.1 and 1.10 (#311). **Do** boot a Season 1 build and 1.10 as well as the previous build.
8. **Hooking intact engine code and changing timing.** cb1f33a hooked `UEngine::GetMaxTickRate`; it was replaced
   by `UGameUserSettings::SetFrameRateLimit` (3cce1bb). bb23616 uncapped the server tick and dropped clients to
   0-1 packets/s (reverted in 1fb259a). **Don't** hook intact engine code, and keep the server at 30 Hz.
9. **Polling loops.** `WatchGetaway`, `WatchScoring` and others re-queued themselves every frame. All were
   converted to restored functions, delegates and timers in the 2026-11-11 audit. **Don't** add new ones.
10. **A rule that matched offline but never at runtime.** f6e39c62's 7.00 rule needed a function start, but the
    target is a leaf with no `.pdata`, so `Finder::FunctionStart` returned 0 (fixed in 247c7710). **Do** read each
    new finder's boot line before calling it done.
11. **The wrong MSBuild.** `vswhere -latest` picked Visual Studio 18 and every file failed with C1853 (precompiled
    header from a different compiler). **Always** use the 2022 MSBuild path above.
12. **Stale builds.** A pull that changed a header signature left an old object file (LNK2001); a source restored
    with `Copy-Item` kept its old timestamp and a throwaway diagnostic shipped in the "final" DLL. **Do** touch the
    changed `.cpp` and check the DLL's timestamp.
13. **Leftover WIP branches and worktrees.** Unmerged branches and worktrees were left behind. **Do** remove your
    worktree and branch when done; record unfinished work in `HANDOFF.md`, never in a pushed WIP branch.
14. **Unsafe test runs.** An agent launched a client unasked; two agents booted test servers at once; copied
    build folders raised stuck firewall prompts. **Do** follow section 2 exactly.

## 9. Tools: MCP servers

The servers are configured in `%USERPROFILE%\.agents\mcp.json`: `core`, `ida-13337` to `ida-13341`, and `fmodel`.

- **core MCP** (if available): the tools in section 4. They enforce the PC rules (ports, one server, no clients),
  so prefer them over hand-typed commands. Long-running ones return a `job_id` to poll (section 4).
  `symbolize_crash` refuses a PDB whose GUID and age don't both match the DLL; `pdb2140_find` reads the 64-bit
  layouts (`types_21.40_layout64.h`).
- **IDA MCPs** `ida-13337` to `ida-13341` (`http://127.0.0.1:<port>/mcp`), one per open IDB. Run
  `project_status` first to learn which port holds which build; ports move when the owner opens a new build.
  **Read-only:** decompile, disasm, xrefs, find, lookups, and `py_eval` only for reading. **Never** patch, rename,
  define, set types or comments, or `idb_save` in these shared IDBs.
- **fmodel MCP:** read assets and Blueprints of the build's paks (instance per build). Read-only.

## 10. Facts that change: check, don't assume

The build in development, the supported range, build folder paths, IDA ports, SDK folder names and server-assets
CLs change often. Only one build is in development at a time. Check them each session with `project_status`, the
README status table, Core-Launcher's `KnownBuilds.cs`, and the knowledge base (MEMORY.md, read-only). Build folders
have moved before (`F:\Builds` holds recent builds, some older ones are in `D:\Fortnite Builds`, some are gone): check
a folder exists before planning a boot on it.

## 11. Deeper docs

| Doc | Read it when |
| --- | --- |
| [docs/agents/finders.md](docs/agents/finders.md) | adding or changing a finder, a declared member or UFunction |
| [docs/agents/restoring-stripped-functions.md](docs/agents/restoring-stripped-functions.md) | making game behaviour run (vtable swap, stripped-call redirect, delegates, timers) |
| [docs/agents/reverse-engineering.md](docs/agents/reverse-engineering.md) | using the 21.40 PDB, IDA, SDK dumps and FModel |
| [docs/agents/headless-server-testing.md](docs/agents/headless-server-testing.md) | booting a test server and reading its log |
| [docs/agents/crash-symbolising.md](docs/agents/crash-symbolising.md) | a crash log with `Core.dll+0x...` frames |
| [docs/agents/building.md](docs/agents/building.md) | build failures, deploying main's build |
| [docs/agents/new-build-bringup.md](docs/agents/new-build-bringup.md) | the owner says "start supporting X" |
| [docs/agents/issues-and-prs.md](docs/agents/issues-and-prs.md) | triaging, commenting, closing; reviewing, fixing and merging PRs |

Skills in `.claude/skills/` (loaded by name): `fix-gameserver-issue`, `review-and-merge-pr`, `add-finder`,
`restore-stripped-function`, `boot-test-server`, `symbolize-crash`, `new-build-bringup`, `pre-commit-review`.

Related repos: [Core-Backend](https://github.com/PongooDev/Core-Backend) (Node backend),
[Core-Launcher](https://github.com/PongooDev/Core-Launcher) (C# launcher, `KnownBuilds.cs`),
[Core-ServerAssets](https://github.com/PongooDev/Core-ServerAssets) (server-only asset paks). Local notes and tools:
`%USERPROFILE%\Documents\Core-Docs` (not a git repo; `Tools\` has the scripts named in these docs).
