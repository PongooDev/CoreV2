---
name: review-and-merge-pr
description: Use for every open CoreV2 pull request - check it out in a worktree, review it against the Epic-accuracy and old-build rules, rework what is wrong (on the PR branch or on main right after), build and boot-test, merge it, and comment on what was changed. Reject only what the project has ruled out.
---

# Review, fix and merge a CoreV2 pull request

The owner's rule: agents review every open PR, make it right and merge it themselves. Depth:
`docs/agents/issues-and-prs.md`. Rules: AGENTS.md sections 1, 2, 6 and 8.

1. Read `D:\CoreWork\HANDOFF.md` (skip PRs someone is already on). `gh pr list -R PongooDev/CoreV2 --state open`.
2. Read the PR: `gh pr view <n> -R PongooDev/CoreV2 --comments`, `gh pr diff <n> -R PongooDev/CoreV2`.
3. Reject only if it adds something ruled out (player-bot features beyond the Phoebe scope, CoreClient changes
   without the owner's OK): `gh pr close <n> -R PongooDev/CoreV2 --comment "<why>"`. Stop.
4. Worktree and checkout:
   ```
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 fetch origin
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree add --detach D:\CoreWork\pr-<n> origin/main
   cd D:\CoreWork\pr-<n>
   gh pr checkout <n> -R PongooDev/CoreV2
   git submodule update --init
   ```
5. Review each change:
   - Epic's name, class, file and flow (`pdb2140_find`, the build's IDB read-only);
   - restores only functions that are stripped in that build, from the game's own trigger; no polling loop; no hook on
     intact engine code;
   - finders additive, old pattern first; every new member, UFunction and finder gated; 1.1+ still works;
   - nothing in `Client/` without the owner's OK; no keys, secrets or build output.
6. Rework what is wrong or not Epic-accurate. If `gh pr view <n> -R PongooDev/CoreV2 --json maintainerCanModify`
   is true, commit on the PR branch and `git push`. If not, merge first (step 8) and fix on main right after, in this
   session. Commit messages and trailers as AGENTS.md section 7 (Freebuff/Codebuff: `Agent: Freebuff`).
7. Build the full `Core.sln` (`build_corev2`, or the VS 2022 MSBuild) and boot-test where it touches start-up,
   finders or shared code (`boot-test-server`, with the regression builds).
8. Merge: `gh pr merge <n> -R PongooDev/CoreV2 --merge`.
9. Comment on the PR: what you changed and why (with the evidence), what was verified live and what statically.
10. Fixes still due on main: make them now (`fix-gameserver-issue` steps from 5 on). Then `deploy_main_build`.
11. Remove the worktree (`git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree remove D:\CoreWork\pr-<n>`).
    Write anything unfinished in `HANDOFF.md`.

Done when: the PR is merged (or rejected with a reason), every rework is on main, the build is clean, boots where
relevant pass, and the PR comment lists what changed and what was verified.
