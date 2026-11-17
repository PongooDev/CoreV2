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

## Pull requests

- Review the diff for correctness against the rules (Epic's place and names, no polling, no intact-engine hooks, no
  client changes, gated for old builds) and for security (downloads, paths, process launches, secrets).
- Test it merged onto main in a detached worktree: build, and boot where it touches start-up code.
- Merging is the owner's decision (or the session the owner runs). Post your review as a comment; merge only when
  told to. The standing practice when merging a PR that isn't perfect: merge it with the author credited and fix the
  problems in follow-up commits, instead of requesting changes and waiting.

## Core-ServerAssets submissions

Pak submissions have their own checks (`verify`, `filter`, rebuilt sig, manifest entry): see Core-ServerAssets'
AGENTS.md.
