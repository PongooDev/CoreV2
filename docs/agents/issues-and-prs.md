# Issues and pull requests

Repos: `PongooDev/CoreV2` (gameserver), `PongooDev/Core-Backend`, `PongooDev/Core-Launcher`,
`PongooDev/Core-ServerAssets`. All are public: never put tokens, keys, personal data or pak AES keys in a comment.

## Listing

Use the REST API, newest-updated first (it includes PRs). The search index (`gh issue list --search`) lags and has
hidden new issues for an hour.

```
gh api "repos/PongooDev/CoreV2/issues?state=all&sort=updated&direction=desc&per_page=10" --jq ".[] | [.number, .state, .updated_at, .title] | @tsv"
gh issue view <n> -R PongooDev/CoreV2 --comments
```

For "what's new since the last check", pass `since=<the previous check's start time minus 10 minutes>`. An overlap
is free; a gap hides comments.

## Triage

1. Read the whole issue and every comment, and the attached logs: build and CL, mode, Core commit, server log.
2. Decide which repo owns it: gameserver behaviour (CoreV2), lobby, profile, shop, calendar, matchmaking
   (Core-Backend), hosting and installing (Core-Launcher), server-only assets (Core-ServerAssets).
3. **Reject** (comment why, close as not planned) when:
   - it is the build's own behaviour: the game did it that way in that version (examples: the lobby resetting to
     Solo after a match on 1.10, party ready state, the Battle Bus HUD, 2.5's leave-party button, 2.2+ STW mode text);
   - it is a player-bot feature beyond what Epic's Phoebe bots did (bot work is bug fixes plus Chapter 2-style
     building only);
   - only a CoreClient.dll change could fix it: explain that instead of patching the client.
4. Ask for missing information once, plainly (build, mode, log). The issue templates already ask for these.
5. Duplicates: link the original and close.

## Fixing

One issue per change and per commit. Follow AGENTS.md section 5. Stop once the cause is proven.

## Commenting and closing

- Comment when the fix is pushed:
  - `Fixed in <commit>.`
  - what was wrong (the cause, in plain words);
  - what changed;
  - what was verified live (boots, log lines) and what only statically;
  - what a live match still has to confirm.
- Close the issue **only** with that comment, naming the fixing commit. Don't wait for the reporter to test unless
  the fix itself can't be made without their test.
- Never close with "should work now", without a commit, or without a comment.
- Reopen when the reporter says it still happens, say you're looking again, and treat the earlier fix as suspect.
  If the earlier fix was wrong, revert it in the same commit as the real fix and say so.

```
gh issue comment <n> -R PongooDev/CoreV2 --body-file <comment.md>
gh issue close <n> -R PongooDev/CoreV2 --comment "Fixed in <commit>: <one line>."
gh issue close <n> -R PongooDev/CoreV2 --reason "not planned" --comment "<why>"
gh issue reopen <n> -R PongooDev/CoreV2 --comment "<why>"
```

## Pull requests: review, fix, merge

The owner's rule: agents review every open PR, make it right, and merge it themselves. Don't leave a PR waiting on
"changes requested". The `review-and-merge-pr` skill has the same steps.

1. List open PRs: `gh pr list -R PongooDev/CoreV2 --state open`. Read each: `gh pr view <n> -R PongooDev/CoreV2 --comments`,
   `gh pr diff <n> -R PongooDev/CoreV2`. Check `D:\CoreWork\HANDOFF.md` that nobody is already on it.
2. Check it out in a worktree under `D:\CoreWork`:
   ```
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree add --detach D:\CoreWork\pr-<n> origin/main
   cd D:\CoreWork\pr-<n>
   gh pr checkout <n> -R PongooDev/CoreV2
   git submodule update --init
   ```
3. Review it against the rules (AGENTS.md sections 1, 6 and 8):
   - Epic's names, places (class and file) and flow, checked in the 21.40 PDB and the build's IDB;
   - only stripped functions restored, from the game's own trigger; no polling loops; no hooks on intact engine code;
   - finders additive (old patterns first); members and UFunctions gated; 1.1+ kept working;
   - no CoreClient change without the owner's OK; security (downloads, paths, process launches, secrets).
4. Rework anything that isn't Epic-accurate or is wrong:
   - if maintainers can edit the PR's branch (`gh pr view <n> --json maintainerCanModify`), commit the fixes on it and
     `git push` (gh set the branch's remote);
   - otherwise merge first (step 6), then fix on main right after, in the same session.
   Keep the author's commits and credit; your fix commits follow the normal message and trailer rules.
5. Build (full `Core.sln`, VS 2022 MSBuild) and boot-test where it touches start-up, finders or shared code
   (`headless-server-testing.md`; regression boots as in the definition of done).
6. Merge with the repo's usual method, a merge commit: `gh pr merge <n> -R PongooDev/CoreV2 --merge`.
7. Comment on the PR: what you changed and why (each Epic-accuracy rework with its evidence), what was verified live
   and what only statically.
8. Deploy for the owner (`deploy_main_build`), remove the worktree.

**Reject** a PR only when it adds something the project has ruled out: player-bot features beyond the Phoebe scope,
CoreClient changes without the owner's OK, client-side fixes for the build's own behaviour. Comment why and close it
(`gh pr close <n> -R PongooDev/CoreV2 --comment "<why>"`). Everything else gets fixed and merged.

## Core-ServerAssets submissions

Pak submissions have their own checks (`verify`, `filter`, rebuilt sig, manifest entry): see Core-ServerAssets'
AGENTS.md.
