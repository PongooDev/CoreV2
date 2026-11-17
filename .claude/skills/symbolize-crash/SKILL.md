---
name: symbolize-crash
description: Use when a CoreV2 crash log or issue has Core.dll+0x... frames (or CoreRedirect/CoreClient offsets) that need function names and source lines, by building the matching commit's PDB and running llvm-symbolizer.
---

# Symbolise Core.dll crash frames

Depth: `docs/agents/crash-symbolising.md`.

1. Read the crash report's register and stack object names first (since 962da735): they may already answer it.
2. Find the commit: the issue's Core commit, or `gh release view build-<N> -R PongooDev/CoreV2`. If unknown, date
   the log by lines a known commit added.
3. Preferred: the core MCP's `symbolize_crash` (DLL path, offsets) with a DLL built from that commit and the PDB
   built with that same DLL (it refuses a PDB whose GUID and age don't both match; rebuilds share a GUID and differ
   in age). Else:
4. Build that commit in a detached worktree:
   ```
   git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree add --detach H:\CoreWork\sym-<commit> <commit>
   git -C H:\CoreWork\sym-<commit> submodule update --init
   cd H:\CoreWork\sym-<commit>
   "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Core.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
   ```
5. Check `x64\Release\Core.dll`'s size equals the release's `Core.dll`. Different size: wrong commit, stop.
6. Symbolise (PowerShell; image base + offset, Core.dll's base is 0x180000000):
   ```
   $B = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64"
   & "$B\dumpbin.exe" /nologo /headers H:\CoreWork\sym-<commit>\x64\Release\Core.dll | Select-String "image base"
   & "$B\llvm-symbolizer.exe" --obj=H:\CoreWork\sym-<commit>\x64\Release\Core.dll --inlines --pretty-print 0x1800DE172
   ```
7. Check the frames form one call path and the faulting instruction matches the log
   (`dumpbin /disasm /range:<va>,<va+0x40>`).
8. Remove the worktree: `git -C $env:USERPROFILE\Documents\GitHub\CoreV2 worktree remove H:\CoreWork\sym-<commit>`.

Done when: every Core frame has a name and line on one consistent path. Then diagnose from the build's code; don't
call it a missing member without a `[Fatal]` line naming it.
