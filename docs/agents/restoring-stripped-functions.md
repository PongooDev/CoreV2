# Restoring stripped functions

Fortnite's PC builds are client builds: Epic compiled the server code out (`WITH_SERVER_CODE=0`). What is left:

- a stripped **virtual** keeps its vtable slot, which now points at the shared empty function (a bare `ret`,
  `Finder::MostReferencedFunction()`, `Natives::SharedEmptyFunction`) or a shared return-zero stub;
- a stripped **UFunction** (RPC `_Implementation`, exec) has a native that does nothing or calls the stub;
- a stripped **call** inside intact code is a call into the stub, or the code around it is gone;
- the intact game code around these still runs and still calls them at the right moment.

Core's job is to put the missing body back **at that moment, in Epic's class, with Epic's name**, so the game's own
flow drives it. Different builds strip different things (5.30 strips `AFortGameModeAthena::OnGivenMatchAssignment`,
5.21 keeps its own), so every restoration is gated on the stub really being there.

## 1. Prove it is stripped, in this build

Before restoring anything, in the IDB of each build you target (read-only, see `reverse-engineering.md`):

1. Find the function's slot or exec (21.40 PDB for the name, the SDK for the UFunction).
2. Check the slot's target in **each class that overrides it**: is it the shared empty function or a return-zero
   stub? The base class may keep a body while a subclass folds into the stub, or the other way round.
3. Check older and newer builds too: stripped in 6.31 doesn't mean stripped in 6.22.
4. `python -I $env:USERPROFILE\Documents\Core-Docs\Tools\stripdiff.py --old <port> --new <port> --out <file>` lists the slots
   and execs that became stubs between two builds.

If the function has a real body in the build, it is **not** stripped: don't restore it. Find out why the game's own
code doesn't do what you expect (a missing call into it, missing data, a different trigger). #461's first fix
"restored" an intact `ServerStartFire_Implementation` and had to be reverted.

**Careful with the shared empty function.** It is also what every deliberately empty override compiles to. A slot
holding it in a subclass may be Epic's own empty override, not a strip. Keep base-class gates; never swap every class
whose slot holds the stub without checking the base class keeps a body that the subclass should inherit.

## 2. Pick the mechanism (in this order)

| Situation | Mechanism | Example |
| --- | --- | --- |
| A stripped virtual the game calls | `Hooks::SwapVTable` in each class whose slot holds the stub | `AFortGameModeAthena::HookMatchAssignmentVTables` |
| A call into the stub inside intact code | `Hooks::RedirectCall` (Core's stripped-call redirect) at that call site | `UFortAbilitySystemComponent::PlaySharedAnim` call in `FortAbilitySystemComponent.cpp` |
| A stripped function bound into a delegate or timer (`lea` of its address) | `Hooks::RedirectAddress` | timer and event delegates |
| A stripped UFunction native | replace its native (`UFunction::SetNativeFunc`, `UClass::AddNativeFunction`) | RPC `_Implementation`s |
| The game broadcasts an event at that moment | bind it: `AddDynamic` / `AddNativeFunction` for Blueprint delegates; `AddStatic`, `AddLambda`, `AddUObject` for native ones | `GamePhaseChanged`, `OnDied`, `UWorld::OnActorSpawned` |
| Something must happen after a delay or repeatedly, as Epic does it | the engine's `FTimerManager` (`Engine/Public/TimerManager.h`) | Save the World timers |
| An async load or request completes | its async callback | soft object loads |

`Hooks::Detour` (MinHook) is a last resort for stripped game code only. On packed builds from 3.5, code writes
before the first menu tick trip the packer, so code patches go through `Hooks::HoldCode` / `ReleaseCode`; vtable
swaps are data writes and are safe.

## 3. Never

- **No polling loops.** No `GameThread::Enqueue` task that re-queues itself to watch state each frame or second.
  The 2026-11-11 audit converted all of them. If the build truly cut the call site out, a fallback is allowed only
  with a comment saying what Epic calls there and why it can't be used.
- **No hooks on intact engine code.** If the engine has an API (a UFunction, a settings object, a console
  variable), call it. Example: the tick rate uses `UGameUserSettings::SetFrameRateLimit`, not a hook on
  `UEngine::GetMaxTickRate`. The one named exception is 2.4's `AFortGameSession::OnVerifyAuthComplete` vtable
  swap, which the owner OK'd; don't add others without the owner's OK.
- **Don't write state the game maintains.** If the game's own tick writes a replicated value (Team Rumble's
  `TeamScore`), restore the stripped input to that code instead of writing the value from Core.
- **No client fixes** (`Client/`, CoreClient.dll) without the owner's OK for that change.

## 4. Write the body

1. Name, class, file and signature from the 21.40 PDB (`symbols_21.40.txt`, `types_21.40_layout64.h`). Put it in
   that class's `.cpp` under `FortniteGame/Source/FortniteGame/Private/<Area>/` or the engine module's folder.
2. The **behaviour** comes from the build you target: read the intact code around the call, the callers, the data
   the build ships (FModel), and what the client reads (the HUD's Blueprint). 21.40 is the naming reference, not
   a body to paste: features differ by years.
3. Read every parameter the build passes (`finders.md`, "Signature and stack-argument changes").
4. Where Epic's code is unknown and Core has to choose, mark it `// Core's: ...` with the reason, and say so in
   the commit message, so it is easy to change later.
5. Gate everything on existence (class, member, UFunction, finder result). A build without it must run as before.
6. Log once at install (`Finder: X is stripped, restored in N classes`), and log the first use when the path can
   only be seen in a match, so a live log shows it ran.

## 5. Checklist

- [ ] Stub proven in this build's IDB, per class; older builds checked.
- [ ] Mechanism from the table, in order; no polling, no intact-engine hook, no client change.
- [ ] Epic's name, class, file and signature; behaviour from this build.
- [ ] Every parameter read; every member and delegate exists in the SDKs it runs on.
- [ ] Gated; the install line and the finder line read in a boot log.
- [ ] Report says what a live match still has to confirm.
