# Building CoreV2

## The command

Always the Visual Studio 2022 MSBuild, by full path, Release x64 (toolset v143):

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Core.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
```

- While iterating: add `-t:Server` (only `Core.dll`). Before the final push: the full `Core.sln` once.
- Output: `x64\Release\Core.dll`, `CoreClient.dll`, `CoreRedirect.dll` (+ PDBs) in the worktree.
- A fresh worktree needs the MinHook submodule: `git submodule update --init`.
- The core MCP's `build_corev2` runs this for you. It needs `worktree` (the worktree's path). A build outlasts one
  call: the call waits up to about 100 s, then returns a `job_id`; call `build_corev2` again with just `job_id` until
  it reports the result. Builds run one at a time: `build_corev2` and `deploy_main_build` share one slot, so a call
  may wait for another agent's build first.

## Pitfalls seen here

| Symptom | Cause | Fix |
| --- | --- | --- |
| Every file fails with C1853 "precompiled header file is from a different version of the compiler" | Visual Studio 18's MSBuild (`vswhere -latest` returns it) against PCHs from 2022 | Use the 2022 path above. Only if C1853 still appears with it, delete `x64\Release\Intermediate\**\*.pch` and rebuild |
| LNK1201 writing a PDB, and the DLLs are gone afterwards | a running game server holds the PDB open, or C: is nearly full; a failed link deletes the DLLs | Never build into a folder a running server loads from. Delete `x64\Release\Core*.pdb`, rebuild at once, check all three DLLs exist |
| LNK2001 unresolved external with an old mangled name after a pull | an incremental build kept an object compiled against the old header | Touch the calling `.cpp` (set its LastWriteTime to now) and build again |
| A change you reverted is still in the DLL | the file was restored with `Copy-Item`, which keeps the old timestamp, so MSBuild skipped it | Set the restored file's LastWriteTime to now and rebuild; check the DLL's timestamp |

After any failed build, check that the three DLLs in `x64\Release` still exist.

## Adding files

All three projects compile the shared tree. Add a new `.cpp`/`.h` to `Server/Server.vcxproj`,
`Client/Client.vcxproj`, `Redirect/Redirect.vcxproj` and their `.vcxproj.filters` **by hand**. Don't regenerate them with
`sync_vcxproj.py`: it is out of date and drops the full link-time code generation setting and some source files.

Review the diff it makes; commit only the project files your change needs.

## Getting a fix to the owner (deploying main's build)

The owner's launcher loads `Core.dll` and `CoreRedirect.dll` from the main checkout's `x64\Release`
(`%USERPROFILE%\Documents\GitHub\CoreV2`), so a pushed fix reaches the owner only when main is pulled and rebuilt
there. A running game holds the PDBs there open, and a failed link deletes the DLLs, so never build directly into it.

Preferred: the core MCP's `deploy_main_build` (it returns a `job_id` to poll like `build_corev2`, and shares its build
slot). Fallback, only after your commit is on `origin/main`:

1. `git -C $env:USERPROFILE\Documents\GitHub\CoreV2 pull --ff-only`. If it fails (local changes, diverged), stop and
   write it in `D:\CoreWork\HANDOFF.md`.
2. Build the main checkout into a separate folder:
   ```
   cd $env:USERPROFILE\Documents\GitHub\CoreV2
   "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Core.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo -nodeReuse:false -p:OutDir=D:\CoreWork\mainbuild\
   ```
3. Copy `Core.dll`, `CoreClient.dll` and `CoreRedirect.dll` from `D:\CoreWork\mainbuild\` into
   `%USERPROFILE%\Documents\GitHub\CoreV2\x64\Release\`.
4. Copy the PDBs only when no `FortniteClient*` process is running; otherwise note in `HANDOFF.md` that the PDBs
   are stale.
