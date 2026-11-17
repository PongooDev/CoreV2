# Reverse-engineering workflow

Every fix and restoration needs evidence from the build itself. These are the sources, in the order to use them.
**All of them are read-only.** Never write to an IDB, an SDK folder, an FModel setup or a build folder.

## 1. Names and signatures: the 21.40 PDB exports

`D:\ResortWork\IDA\21.40` holds exports from a Fortnite 21.40 PDB. The old builds have no symbols, so this is where
Epic's function, class, struct and member names, signatures and virtual order come from.

| File | Size | Use |
| --- | --- | --- |
| `symbols_21.40.txt` | 1.2 MB | function names (`Class::Function`): grep it |
| `types_21.40_index.tsv`, `types_21.40_index64.tsv` | ~78 MB | type lookup index: grep it |
| `types_21.40.txt` | 154 MB | type records: grep, never open whole |
| `types_21.40_layout.h`, `types_21.40_layout64.h` | ~3 GB each | class layouts and vtables: **only** grep with a narrow pattern, never read or open whole |
| `AFortPawn_21.40.h`, `AFortPawn_21.40_64.h` | 45 KB | one class, a worked example |

Prefer the core MCP's `pdb2140_find` (query) tool if available. Fallback (PowerShell; `rg` isn't installed, and
Git Bash's `grep -n -m 5` works too):

```
Select-String -Path D:\ResortWork\IDA\21.40\symbols_21.40.txt -Pattern "AFortGameModeAthena::OnGivenMatchAssignment" -SimpleMatch
Select-String -Path D:\ResortWork\IDA\21.40\types_21.40_layout64.h -Pattern "^class AFortAthenaMutator_TDM\b" | Select-Object -First 5
```

21.40 is years later than the builds Core supports. Use it for **names and placement**, then read the target
build's own code for what the function does there.

## 2. The build's code: IDA (read-only)

The owner keeps IDA instances open with the IDA MCP plugin, one build per port, on `13337` to `13342`. **Which port
holds which build changes** when a new build starts: run the core MCP's `project_status` first, or check the port:

```
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\ida.py --port=13339 health
```

Agents with MCP access use the `ida-<port>` servers directly. Allowed: decompile, disasm, xrefs, find, lookups,
and `py_eval` **only for reading**. **Never** patch bytes, rename, define functions or data, set types or comments,
or save (`idb_save`): the IDBs are shared with the owner and other agents.

`ida.py` (fallback client) commands:

```
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\ida.py --port=<port> decompile 0x12F4400
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\ida.py --port=<port> py <read-only-snippet.py>
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\ida.py --port=<port> call <tool> "<json arguments>"
```

Keep snippets bounded (each call has a time limit) and write them in your own scratch folder, not in `Core-Docs\Tools`.

**When no instance has the build:** older builds have IDB copies in `H:\CoreWork\idb\<build>\` (about 1.2 GB
each). Serve one headless on a free port (13350 and up) and stop it when you are done; keep the copy:

```
cd H:\CoreWork
python -I $env:USERPROFILE\Documents\Core-Docs\Tools\idaserve.py H:\CoreWork\idb\6.21\<file>.i64 13350
```

Run it from outside `Core-Docs\Tools` (a `dis.py` there breaks IDA's Python). Run at most two IDA instances of
your own at a time; the PC has 16 GB of RAM. The owner's own IDB folders (`H:\IDA Dumps\<version>`) are never
opened for writing; serve a copy instead.

## 3. Types and members: Dumper-7 SDKs

`C:\Dumper-7\<UE version>-<CL>+++Fortnite+Release-<version>-FortniteGame\` per build (e.g.
`4.22.0-4727874+++Fortnite+Release-7.20-FortniteGame`): `CppSDK\SDK\*_classes.hpp`, `*_structs.hpp`,
`*_parameters.hpp`, `*_functions.cpp`, plus `Dumpspace`, `Mappings`, `IDAMappings`. Member lines carry their offset
and size; each class has a `static_assert` of its size.

Prefer `sdk_find` (query, build or CL) on the core MCP. Fallback:

```
Select-String -Path "C:\Dumper-7\4.21.0-4573279+++Fortnite+Release-6.31-FortniteGame\CppSDK\SDK\*_classes.hpp" -Pattern "^class AFortAthenaVehicle\b"
```

Check **several builds' SDKs** before declaring a member: when it appeared, which class it was on, whether its type
changed. Some builds have no SDK (8.30, a reference only); their layouts come from IDA's reflection registration code.

## 4. Assets and Blueprints: FModel

The `fmodel` MCP reads each build's paks (one instance per build): asset JSON, Blueprint decompiles, data tables.
Use it for what a HUD widget reads, what a Blueprint calls, playlist and loot data. FModel holds the builds' AES keys:
never print, copy or commit them.

## 5. Logs

- Core's log: `Server_log_<pid>.txt` next to the exe during a run; the owner's launcher moves it to
  `%LOCALAPPDATA%\Core-Launcher\Logs\Core\` afterwards. `run_server_dev.py` also copies the last one to
  `%USERPROFILE%\Documents\Core-Docs\Tools\run_server_dev.py.last.txt`.
- The game's own engine log: `%LOCALAPPDATA%\FortniteGame\Saved\Logs\FortniteGame.log` (client), and
  `%LOCALAPPDATA%\FortniteGame\Saved_<suffix>\Logs\` for servers started with `-saveddirsuffix=<suffix>`. It is not
  in the build folder. Diff a broken run's log against a working build's to find where they diverge.

## 6. Engine versions (layouts differ)

| Builds | Unreal Engine |
| --- | --- |
| 1.1 to 1.9.1 | 4.16 |
| 1.10 to 2.4.2 | 4.19 |
| 2.5 to 4.5 | 4.20 |
| 5.0 to 6.31 | 4.21 |
| 7.00 on | 4.22 |

Builds from 1.9.1 are packed (encrypted `.text`, moved code, hidden imports). Core waits for the packer's stub
(`Core/Private/Packed.cpp`); don't try to decrypt, bypass or disable the packer.

## 7. Method

1. Name it with the PDB. 2. Find it in the build's IDB (string reference, vtable slot from a known neighbour, xrefs
from a reflected exec). 3. Compare the same function in one older and one newer build. 4. Check the members in the
SDKs. 5. Write the addresses down with the build's CL (`6.31 0xC81D20`) in the commit message, so a reviewer can check
them. 6. Stop when the cause is proven.
