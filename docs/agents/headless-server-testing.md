# Headless server testing

A headless boot starts the game's exe as a dedicated server (`-nullrhi`, no window) with your `Core.dll`, against a
**test** backend, until Core logs `Server ready`, then stops it. It proves finders, hooks and start-up code. It does
**not** run a match: joining, inventory, building, eliminations and match end stay unconfirmed and must be reported
as such.

## Rules (the owner's PC)

- Boot-only. **Never** launch a client. **Never** play out a match. Don't pass `-CoreBotMatch` (it plays a bots-only
  match) unless the owner asked for it.
- **One game server at a time**, and only while no `FortniteClient*` process you didn't start is running. Put the
  check and the launch in **one** command (an agent once printed the process list and booted anyway).
- **Test backend port only** (5711 by default). **Never 5595.**
- Run straight from the installed build folder. **Never** copy, hard-link, junction or overlay a build, and never
  add, change or delete files in `F:\Builds` or `D:\Fortnite Builds`. New exe paths raise Windows Firewall prompts
  that get stuck invisibly and dim the owner's screen. If one appears, don't touch firewall settings; tell the owner.
- Boot at milestones, not after every edit. Keep `--linger` short and stop as soon as the log shows the result.
- Stop the test backend and anything else you started before you finish.
- The PC has 16 GB of RAM and often little free. A packed build can take many minutes to reach `Server ready`; give
  it a long `--timeout` instead of booting a second server.

## Preferred: the core MCP

`boot_test_server` (build, mode, backend port) enforces the rules above and returns the log. Then
`server_log_summary` (log path) pulls out the lines below.

## Fallback: by hand

1. Find the installed build folder (`project_status`, or the build notes in the knowledge base). Check it exists.
   The folder is the one holding `FortniteGame` and `Engine`.
2. Build your worktree (`build_corev2`, or MSBuild `-t:Server`). Your DLLs are in `<worktree>\x64\Release`.
3. Start a test backend from a Core-Backend checkout, in the background, with its own data folder:
   ```
   node src/main.js --port 5711 --data H:\CoreWork\<task>-data --local-only --qos-ping-port 0 --vivox-sip-port 0
   ```
   Wait for `Listening on port 5711`. If 5711 is taken by another agent's backend, pick another free port and use a
   copy of `run_server_dev.py` in your own scratch folder with its two `5711`s changed.
4. Boot, gated, in one PowerShell command (build folder first; game arguments after `--`):
   ```
   if (Get-Process FortniteClient* -ErrorAction SilentlyContinue) { "A FortniteClient is running: not booting" } else { python -I C:\Users\tkenn\Documents\Core-Docs\Tools\run_server_dev.py "<build folder>" --dll "<worktree>\x64\Release\Core.dll" --redirect "<worktree>\x64\Release\CoreRedirect.dll" --timeout 900 --linger 20 -- -CoreMode=BattleRoyale }
   ```
   - Battle Royale: `-- -CoreMode=BattleRoyale`. Without it the server stays on `FortniteEmptyDedicated` and the
     Battle Royale code (loot, POIs) never runs.
   - Save the World: no `-CoreMode`.
   - The script loads `CoreRedirect.dll` first and passes `-httpproxy=127.0.0.1:5711` and
     `-CoreBackend=http://127.0.0.1:5711`, so nothing reaches Epic or the 5595 backend.
   - It prints the last 3000 characters of the Core log, writes the whole log to
     `C:\Users\tkenn\Documents\Core-Docs\Tools\run_server_dev.py.last.txt`, and terminates the server.
5. Read the full log (`run_server_dev.py.last.txt`, or `Server_log_<pid>.txt` next to the exe). The engine's own
   log is `%LOCALAPPDATA%\FortniteGame\Saved_CoreServer\Logs\FortniteGame.log` (other agents' servers share that
   folder, so check the timestamp).
6. Stop your test backend. Check no process of yours is left.

## What to check in the log

| Line | Pass |
| --- | --- |
| `Server ready: ...` | present (else: timeout or exit code printed by the script) |
| `Finder: runtime resolved, N missing`, `Finder: server sites resolved, N missing`, the other `... resolved, N missing` groups | N = 0 |
| `Reflection: ... types, ... members, ... functions, ... enum values, N missing` | N = 0 |
| `[Fatal]`, `[Error]` | no new ones compared with the previous build's boot |
| `slot none`, `not found`, `isn't hooked` | no new ones; each existing one explained |
| `Floor loot: N containers spawned M pickups (...)` (Battle Royale) | N > 0 and M > 0 |
| `World containers: X (Y with a loot tier); ...` (Battle Royale) | Y > 0 |
| your new finder's or restoration's own line | present, with an address or slot |

"0 missing" alone isn't enough. 4.1 once booted with 0 missing while the container finder logged
`InitializeTierGroupData slot none`, and it had no floor or chest loot (#288).

Compare against a boot of the previous build or of `origin/main` when a number looks off (hook counts, loot
counts). Server-assets paks change some lines (POI volumes "has its brush (server assets)" vs "not placed").

## Regression boots

For changes to shared code, boot the affected build plus: one Season 1 build (1.7.2 or 1.8), 1.10, and the
previous build. Builds move between drives and some are gone; check each folder exists, and list the builds you
couldn't boot in the report instead of copying anything.
