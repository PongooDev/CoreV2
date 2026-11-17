# Finders and declared members

A **finder** locates something in the running build's exe that reflection can't give Core: a function address, a
vtable slot, a call site, an unreflected member offset. Finders live in `Core/Private/Resolve.cpp` (grouped in
`Finder::Resolve*` functions declared in `Core/Public/Finder.h`) and store their results in `Natives::` (`Core/Public/Natives.h`).
Reflected things (UClasses, UProperties, UFunctions, enums) are **declared**, not found: see the second half.

One DLL runs on every supported build, so a finder that is right for the newest build and wrong for 1.1 is a bug.

## Rules

1. **Additive.** Keep the existing pattern first and add the new build's shape as an alternative after it. Never
   replace or loosen an old pattern so the new build matches; old builds then pick the wrong thing.
2. **Gate on existence.** Run a finder only where its class or anchor exists (`StaticClass()` non-null, the member
   `IsAvailable()`, the string present). A build without the feature must skip it quietly, not log an error or crash.
3. **Old builds first.** When you design a pattern, check it against the oldest builds that have the function, then
   the newest. Use the IDBs (read-only) and the SDKs of several builds.
4. **One log line per result.** Log what was found (`Finder: X at 0x...`, `... slot 0x...`) or that it wasn't. A
   required finder counts in its group's `MissingCount`; an optional one logs "none". Never hide a miss.
5. **Read the boot line.** After adding a finder, boot the build (and an old one) and read that line. Static checks
   against the IDB are not enough: see the pitfalls below.
6. **Prove it on the live server when it runs late.** A finder that only runs in a match can't be seen headless.
   Say so in the report, and ask for the next `Server_log_<pid>.txt` from
   `%LOCALAPPDATA%\Core-Launcher\Logs\Core` before calling it done.

## Primitives (Core/Public/Finder.h)

| Primitive | Use |
| --- | --- |
| `StringRefs(text)`, `FunctionStart`, `FunctionEnd`, `FunctionChunks` | a function from a log string it references; its bounds (all pieces) |
| `DirectCalls`, `DecodedDirectCalls`, `CallsTo`, `NextCall` | call sites; `DecodedDirectCalls` decodes instructions, so it skips false E8 bytes |
| `VTableIndex`, `VTableContaining`, `ReadOnlySlotsHolding` | vtable slots; prefer `FirstVTableIndex` / `FirstVTableIndexWhere` in `Resolve.cpp` |
| `MostReferencedFunction()` | the shared empty function every stripped void virtual points at |
| `BodyIs`, `TraceFind`, `RunsInto` | read code in run order, following jumps (moved code on packed exes) |
| `Unthunk`, `HasMovedCode`, `ExecutableSections` | packed 1.9.1+ exes, whose moved functions start with jumps |
| `AddressRefs` | `lea r64, [rip+X]` sites: functions bound into delegates instead of called |
| `MemberThisCalls`, `ReadsMemberAt`, `LastBroadcastDelegateOffset` | unreflected member offsets from code |
| `RescanStringRefs` | packed exes: re-read string refs once the engine is up |

## Pitfalls that passed static checks and failed live

- **No RTTI, vtables back to back.** `VTableIndex` with a large `MaxSlots` runs into the next class's vtable and
  returns -1. Use `FirstVTableIndex` / `FirstVTableIndexWhere`.
- **Core's own hooks are in the vtables.** A vtable walk that runs after `Hooks::SwapVTable` sees Core functions;
  step over them (`IsCoreFunction`).
- **Bitfield masks.** `GetFieldMask()` is 0 until `GetOffsetChecked()` has run, and MSVC evaluates call arguments
  right to left. Resolve the offset in its own statement first.
- **One string, two functions.** A log string can be referenced by two functions. Pick by a structural property,
  not "the last reference".
- **Tail jumps.** `DirectCalls` sees `call rel32` (E8) only; a tail `jmp` to the target is invisible to it.
- **Leaf functions have no `.pdata`.** `FunctionStart` returns 0 for a frameless leaf, so a
  `FunctionStart(x) == x` check rejects it (7.00 death rule, f6e39c62, fixed in 247c7710).
- **Arxan-dispersed functions** (some engine and online functions on later builds) have no readable body and no
  unwind data. Don't anchor a finder inside one.
- **Packed exes** (1.9.1 and later): the code is decrypted by the packer's stub on worker threads. Finders that
  write code run after `Packed::WaitForCodeCheck`; string refs may need `RescanStringRefs`.

## Signature and stack-argument changes

Builds change the parameters of functions Core calls or replaces. Each miss was a crash or silent wrong behaviour:
7.00's `CanPlaceBuildableClassInStructuralGrid` gained `MarkerOptionalAdjustment` (#479), 7.10's
`CanCraftSchematic` gained a tier before the count (`[rsp+20h]`/`[rsp+28h]`), 6.22's `TossPickup` gained
`InPickupSourceTypeFlags`, 6.21's `RefocusQuickBars` took a bool, 7.10's cloud save dropped three parameters.

- A function Core **replaces** must read every parameter the build passes. A function Core **calls** must be given
  every parameter the build's version takes.
- Detect the shape from the code, not the build number: the prologue (`movzx r13d, dl` vs `movzx edi, r9b`), the
  highest `[rsp+N]` slot the body reads, the callers' stores to `[rsp+20h..]` before the call. Where the change has a
  reflected trace (an RPC that gained `RequestedTier`), gate on that.
- Log which shape was detected (`... takes bCheckin only`), so a boot shows it.

## Declared members (Core/Public/Reflection.h)

| Macro / call | Use |
| --- | --- |
| `DefineUnrealClass(Name, Package)` | a reflected class |
| `DefineRenamedUnrealClass(Name, FormerName, Package)` | a class a later build renamed: falls back to the old name. **Never** rename a declaration plainly |
| `DefineMovedUnrealClass`, `DefineMovedRenamedUnrealClass` | a class that moved package |
| `DefineUnrealStruct`, `DefineRenamedUnrealStruct` | structs; arrays of them use the reflected size (`TStructArray`) |
| `DefineUProperty(Type, Name)`, `DefineUStructProperty`, `DefineUStructArrayProperty` | members, read by reflected offset |
| `DefineBitfieldUProperty(Name)` | bools that are really bitfields (6.01's `bThankedBusDriver`) |
| `DefineUFunction(Name)` | gives `StaticFunction_Name()` and `FindStaticFunction_Name()` |
| `Member_X.IsAvailable()` | check before touching a member a build may lack |

- `StaticFunction_X()` and a member's checked offset call `Log::Fatal` ("... is not available") when missing. Use
  them only for things every supported build has.
- For anything that exists only from some build on, or that a build dropped, use `FindStaticFunction_X()` (null
  when missing) and `IsAvailable()`. A dropped member is fatal only when the code runs, so a boot doesn't show it
  (2.2's `DefaultHeroes` killed the server at the first join).
- Declare a member on the class the SDKs show it on, and check older SDKs for where it was before.

## SDK diff tools (every new build, and any member change)

Args are always: the CoreV2 worktree, the new SDK folder, the previous SDK folder (folders in `C:\Dumper-7`).

```
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\sdk_member_check.py <repo> <new SDK> <old SDK>
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\sdk_member_class_check.py <repo> <new SDK> <old SDK>
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\sdk_func_check.py <repo> <new SDK> <old SDK>
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\sdk_type_check.py <repo> <new SDK> <old SDK>
```

The name-only checks miss a member that keeps its name but changes type or size (5.41's `CharacterParts`, 6.01's
`CurrentPlaylistData` becoming `CurrentPlaylistInfo`); `sdk_type_check.py` catches those, plus UFunction parameter
lists, enum renumbers and struct sizes. Guard every dropped or changed item before the first boot.
