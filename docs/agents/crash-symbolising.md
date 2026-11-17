# Symbolising crash logs

A crash report gives frames as `Core.dll+0x...`. CI's `build-N` releases ship only the three DLLs, no PDB, so the
frames have to be named against a PDB built from the **same commit**. A local build with the VS 2022 toolchain
matches CI's byte for byte in size, so its PDB names CI's frames.

Since 962da735 (build-669) a server crash report also names the objects in the registers and on the stack (class,
name, pending kill). Read that first: it often answers the question without symbolising.

## 1. Find the commit

- The issue's "Core commit" field, or the release (`build-N` is CI run N; `gh release view build-N -R PongooDev/CoreV2`
  shows the commit).
- The owner's own launcher loads DLLs from the main checkout's `x64\Release`, so a crash from the owner usually
  matches main at that time, and its PDB sits next to the DLL.
- If unknown, narrow the range from the log itself: lines a dated commit added or changed. A missing
  "other registers / objects" section dates a log to before 962da735.

## 2. Build that commit's PDB (in a worktree, never in the main checkout)

```
git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree add --detach H:\CoreWork\sym-<commit> <commit>
cd H:\CoreWork\sym-<commit>
git submodule update --init
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Core.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
```

Check the DLL size matches the release's `Core.dll` (`gh release download build-N -R PongooDev/CoreV2 -p Core.dll -D <scratch>`).
A different size means a different commit or toolchain: the names would be wrong.

## 3. Name the frames

Prefer the core MCP's `symbolize_crash` (DLL path, offsets). It refuses a PDB whose GUID and age don't both match
the DLL. Rebuilds of the same project share a GUID and differ only in age, so always symbolise against the PDB that
was built together with that exact DLL (the one next to it in the same output folder), never a PDB from another
build of the same commit. Give it your local build's `Core.dll` (whose size matched the release's), not the
downloaded release DLL, which has no PDB of its own. Fallback: the tools in the VS 2022 MSVC bin folder.

```
$B = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64"
& "$B\dumpbin.exe" /nologo /headers H:\CoreWork\sym-<commit>\x64\Release\Core.dll | Select-String "image base"
& "$B\llvm-symbolizer.exe" --obj=H:\CoreWork\sym-<commit>\x64\Release\Core.dll --inlines --pretty-print 0x1800DE172 0x180012345
```

- `llvm-symbolizer` takes **virtual addresses**: image base (`0x180000000` for Core.dll; check with dumpbin) plus the
  log's offset. `Core.dll+0xDE172` is `0x1800DE172`. It finds the PDB next to the DLL.
- Run `dumpbin` from PowerShell or cmd. Git Bash rewrites `/headers` into a path and dumpbin prints nothing.
- If the MSVC version folder changed, list `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\`.
- To see the faulting instruction: `& "$B\dumpbin.exe" /nologo /disasm /range:0x1800DE150,0x1800DE190 <dll>`.

## 4. Check the match

The names are right when every frame lands on one consistent call path and the faulting instruction is the read or
write the log describes (e.g. `mov r9d, [rcx+r9*4]` with the bad pointer in `rcx`). If frames jump between
unrelated functions, the commit or build is wrong.

## 5. Then diagnose

A symbolised crash says where, not why. Read the faulting code against the build's IDB and SDK: a layout that moved
(#457: `FNetworkObjectInfo` in 6.30), a parameter the build added, a member a build dropped. Don't call it a missing
member unless the log has a `[Fatal]` line naming it.

Frames in the game's exe (`FortniteClient-Win64-Shipping.exe+0x...`) are read in that build's IDB at
image base + offset (the IDB's base is shown by `project_status` or IDA).

Remove the worktree when done: `git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree remove H:\CoreWork\sym-<commit>`.
