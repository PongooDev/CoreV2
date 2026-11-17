---
name: fix-gameserver-issue
description: Use when fixing a CoreV2 gameserver (Core.dll) GitHub issue or a reported in-game bug, from reading the issue to pushing the fix and closing the issue. Covers the worktree, finding the cause with evidence, Epic-accurate placement, old-build gating, building, boot-testing, committing and commenting.
---

# Fix a CoreV2 gameserver issue

Rules: AGENTS.md sections 1 and 2 apply in full. Depth: `docs/agents/*.md`.

1. Read `H:\CoreWork\HANDOFF.md`. If the issue is listed there, continue from it; don't redo or undo listed work.
2. Run the core MCP's `project_status` (if available) to learn the build in development and the IDA ports.
3. Read the issue and every comment: `gh issue view <n> -R PongooDev/CoreV2 --comments`. Note build, CL, mode,
   Core commit, log.
4. If it is the build's own behaviour, out of scope (bot features beyond Phoebe), or client-only: comment why and
   close as not planned (`docs/agents/issues-and-prs.md`). Stop.
5. Make a worktree:
   ```
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 fetch origin
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree add -b fix-<n> H:\CoreWork\wt-<n> origin/main
   git -C H:\CoreWork\wt-<n> submodule update --init
   ```
6. Find the cause with evidence:
   - crash offsets: the `symbolize-crash` skill;
   - the build's code at that moment: IDA MCP, read-only (`docs/agents/reverse-engineering.md`);
   - members and UFunctions: `sdk_find` or the SDK folders, on this build and older ones.
   Write down addresses with the build's CL. Stop once the cause is proven.
7. Fix it in Epic's class and file with Epic's names (21.40 PDB: `pdb2140_find`). Make it run from the game's own
   trigger (`restore-stripped-function` skill). No polling loops, no hooks on intact engine code, no `Client/` change.
8. Gate every new member, UFunction (`FindStaticFunction_X`, `IsAvailable()`) and finder (`add-finder` skill).
9. Build: `build_corev2` with `worktree` (poll its `job_id` until done; builds run one at a time), else
   `"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Core.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo -t:Server`,
   then the full `Core.sln` once.
10. Boot-test the affected build and the regression builds (`boot-test-server` skill). One server at a time.
11. Run the `pre-commit-review` skill. Fix anything it finds.
12. Stage only your files (`git add <paths>`, never `-A`) and commit. Message: cause, evidence, fix, gating, what was
    verified live vs statically. Freebuff/Codebuff commits end with `Agent: Freebuff` (no Claude Co-Authored-By
    line); Claude uses its own Co-Authored-By line. The pre-commit hook (`tools/githooks/pre-commit`) runs: never
    `--no-verify`; fix what it flags.
13. `git fetch origin`, `git rebase origin/main`, rebuild if the rebase brought changes, `git push origin HEAD:main`.
14. Comment and close: `gh issue close <n> -R PongooDev/CoreV2 --comment "Fixed in <commit>: <cause, fix, verified live/static>"`.
15. Deploy for the owner: `deploy_main_build` (if available), else note it in `HANDOFF.md`.
16. Clean up: stop your processes, `git worktree remove H:\CoreWork\wt-<n>`, `git branch -D fix-<n>`.
    If anything is unfinished, write it in `H:\CoreWork\HANDOFF.md` (what, branch/worktree, verified, next).

Done when: AGENTS.md section 6 (definition of done) is all ticked.
