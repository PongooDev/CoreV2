---
name: restore-stripped-function
description: Use when CoreV2 game behaviour doesn't happen because Epic compiled the server code out of the build (a stripped virtual, UFunction native or call), to put the body back in Epic's class and run it from the game's own trigger (vtable swap, stripped-call redirect, native replacement, delegate, FTimerManager). Also use to decide that something is NOT stripped.
---

# Restore a stripped function

Depth: `docs/agents/restoring-stripped-functions.md`. Rules: AGENTS.md sections 1 and 2.

1. Name it: `pdb2140_find` (21.40 PDB) for the class, function, signature and file.
2. Prove it is stripped **in this build**, read-only in the IDB: the slot (in every class that overrides it) holds
   the shared empty function or a return-zero stub, or the exec/call goes into the stub. Check an older and a newer
   build too, or run `stripdiff.py --old <port> --new <port>`.
3. If it has a real body: it is NOT stripped. Don't restore it. Find why the game's code doesn't do what you expect.
4. Pick the mechanism, in this order:
   1. `Hooks::SwapVTable` in the classes whose slot holds the stub (keep base-class gates);
   2. `Hooks::RedirectCall` at the stripped call site inside intact code;
   3. `Hooks::RedirectAddress` for a stripped function bound into a delegate or timer;
   4. native replacement for a stripped UFunction;
   5. bind the event the game broadcasts (`AddDynamic`, `AddNativeFunction`, `AddStatic`, `AddLambda`, `AddUObject`);
   6. `FTimerManager` for Epic's own delays; async callbacks for loads.
   Never: a self-re-queuing `GameThread::Enqueue` loop, a hook on intact engine code, a `Client/` change.
5. Write the body in Epic's class and file (`FortniteGame/Source/FortniteGame/Private/<Area>/<Class>.cpp`), with
   Epic's name and signature, and **this build's** behaviour (not 21.40's body pasted). Read every parameter.
6. Don't write state the game maintains itself (replicated scores the game's tick writes): restore the input instead.
7. Mark Core's own choices `// Core's: <why>`. Gate on the class, member, UFunction and finder existing.
8. Log once at install (`Finder: X is stripped, restored in N classes`) and on first use if only a match reaches it.
9. Build, boot the build and a regression build (`boot-test-server`), read the install line.

Done when: stub proven per class, mechanism from the list, gated, install line seen in a boot, and the report says
what a live match still has to confirm.
