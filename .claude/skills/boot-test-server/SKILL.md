---
name: boot-test-server
description: Use when a CoreV2 change needs a headless (boot-only) game server test on the owner's PC, to check finders, hooks and start-up on one build. Enforces the safety rules (test backend port, one server, no clients, no copied builds) and lists the log lines to check.
---

# Boot a headless test server

Depth: `docs/agents/headless-server-testing.md`.

Never: a client launch, a full match (`-CoreBotMatch` without the owner's OK), port 5595, two servers at once,
copying or changing anything in `F:\Builds` or `D:\Fortnite Builds`, killing a process you didn't start.

1. Preferred: the core MCP's `boot_test_server` (build, mode `BattleRoyale` or Save the World, backend port), then
   `server_log_summary` (log path). Skip to step 7.
2. Find the installed build folder (`project_status`); check it exists. Don't copy it.
3. Build your worktree (`<worktree>\x64\Release\Core.dll` and `CoreRedirect.dll`).
4. Start a test backend in the background from a Core-Backend checkout and wait for `Listening on port 5711`:
   `node src/main.js --port 5711 --data H:\CoreWork\<task>-data --local-only --qos-ping-port 0 --vivox-sip-port 0`
5. Boot, gated, in one PowerShell command:
   ```
   if (Get-Process FortniteClient* -ErrorAction SilentlyContinue) { "A FortniteClient is running: not booting" } else { python -I C:\Users\tkenn\Documents\Core-Docs\Tools\run_server_dev.py "<build folder>" --dll "<worktree>\x64\Release\Core.dll" --redirect "<worktree>\x64\Release\CoreRedirect.dll" --timeout 900 --linger 20 -- -CoreMode=BattleRoyale }
   ```
   Save the World: leave out `-- -CoreMode=BattleRoyale`.
6. Read the full log: `C:\Users\tkenn\Documents\Core-Docs\Tools\run_server_dev.py.last.txt`.
7. Check:
   - `Server ready` present;
   - every `Finder: ... resolved, N missing` and `Reflection: ... N missing` has N = 0;
   - no new `[Fatal]`, `[Error]`, `slot none`, `not found`, `isn't hooked`;
   - Battle Royale: `Floor loot: N containers spawned M pickups` with N > 0, `World containers: X (Y with a loot tier)` with Y > 0;
   - your change's own lines.
8. Stop the test backend and anything else you started. Confirm no process of yours remains.

Done when: the lines above pass (or each failure is explained), and the report names the build, mode, result and
key lines, and says what a boot can't show (joining, building, eliminations, match end).
