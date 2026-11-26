---
name: pre-commit-review
description: Use before every CoreV2 commit (and before pushing) to self-review the staged change against the project rules and the list of past mistakes - placement, triggers, polling, engine hooks, client changes, old-build gating, evidence, testing claims, commit message and trailer.
---

# Pre-commit self-review (CoreV2)

Run `git diff --cached` (and `git diff` for anything unstaged) and answer every question. Any "no" blocks the commit
until fixed or explained in the message. Rules: AGENTS.md sections 1, 2 and 8.

**Scope**
1. Does the diff contain only this change's files? (No `git add -A`; no other agent's files; no `.claude/settings.local.json`, `.freebuff/`, logs or scratch.)
2. Was `D:\CoreWork\HANDOFF.md` read, and does this commit not undo work listed there?

**Evidence**
3. Was the cause proven from this build's code, log or symbolised crash, with addresses and the CL in the message?
4. If the message says "missing member", does the log have a `[Fatal]` line naming it?
5. If something is called "stripped", was the stub seen in this build's IDB, per class, and does the function exist
   at all on the older builds it runs on (not just "not reflected")?

**Placement and trigger**
6. Is the code in Epic's class and file, with Epic's names and signature (21.40 PDB)?
7. Does it run from the game's own trigger (restored stripped function, delegate, FTimerManager, async callback)?
8. No `GameThread::Enqueue` loop that re-queues itself? No hook or patch on intact engine code? Nothing in `Client/`?
9. Does it avoid writing state the game itself maintains?

**Old builds**
10. Is every new member, UFunction, class and finder gated (`IsAvailable()`, `FindStaticFunction_X()`, null checks)?
    No `StaticFunction_X()` for something a build may lack?
11. Are finder changes additive (old pattern first)? No removed base-class gate or fallback?
12. Are parameters read or passed in full for every build's signature?
13. No hard-coded offsets or build-number gates without evidence for each build covered?

**Testing**
14. Did the full `Core.sln` build with the VS 2022 MSBuild, and do the three DLLs exist?
15. Were the affected build and the regression builds booted (one at a time, test port), and the log lines checked?
16. Does the message say exactly what was verified live and what only statically?

**Message**
17. Subject `<build or area> (<#issue>): <change>`; body: cause, evidence, fix, gating, verification.
18. Trailer: Freebuff/Codebuff end with `Agent: Freebuff` and no Claude Co-Authored-By line; Claude uses its own.

**The hook.** `tools/githooks/pre-commit` runs on `git commit` (`git config core.hooksPath tools/githooks` turns it on
in a clone that lacks it). It blocks keys, secrets, files over 5 MB, build output, binaries, game files, agent-local
files and VS 18 project changes. Never commit with `--no-verify`; fix what it reports and commit again. Its warning
about a source file missing from the projects means: run `sync_vcxproj.py` and stage the project files.

After the commit: rebase on `origin/main`, push, comment on the issue naming the commit, and update `HANDOFF.md` if
anything is left.
