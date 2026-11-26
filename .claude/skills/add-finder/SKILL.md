---
name: add-finder
description: Use when adding or changing a CoreV2 finder in Core/Private/Resolve.cpp (a function address, vtable slot, call site or unreflected offset) or a declared reflected member/UFunction/class, especially for a new build. Keeps old builds working with additive patterns and gates.
---

# Add or change a finder

Depth: `docs/agents/finders.md`. Rules: AGENTS.md sections 1 and 2.

1. Name the target with the 21.40 PDB (`pdb2140_find`, else grep `D:\ResortWork\IDA\21.40\symbols_21.40.txt`).
2. Locate it read-only in the IDB of: the new build, the previous build, and the oldest build that has it
   (`project_status` gives the IDA ports; older builds: serve the original `H:\IDA Dumps\<build>` IDB with `idaserve.py`; never copy IDBs).
3. Write down the anchor that holds on all of them (a log string, a neighbouring vtable slot, a reflected exec's
   call). Note what differs.
4. Code it in the existing `Finder::Resolve*` group:
   - keep the old pattern first; add the new shape as an alternative after it (never loosen the old one);
   - use `FirstVTableIndex` / `FirstVTableIndexWhere`, not `VTableIndex` with a large slot count;
   - skip Core's own functions in vtable walks (`IsCoreFunction`);
   - `DirectCalls` doesn't see tail jumps; leaf functions have no `.pdata` (`FunctionStart` returns 0);
   - gate on the class or member existing; a build without it skips quietly;
   - log one line with the result (address, slot) or "none"; count required ones in `MissingCount`.
5. Reflected members: `DefineUProperty` / `DefineUFunction` on the class the SDKs show; renames via
   `DefineRenamedUnrealClass`; build-specific ones read through `IsAvailable()` and `FindStaticFunction_X()` only.
6. If a function Core replaces or calls changed its parameters, read or pass every parameter; detect the shape from
   the code and log it.
7. Build (`build_corev2`, else MSBuild `-t:Server`), then boot the new build and an old one (`boot-test-server`).
8. Read your finder's own boot line on each. A result you only checked in IDA isn't done.

Done when: the line shows the right address or slot on every booted build, all groups say `0 missing`, and the
report lists builds checked live vs statically.
