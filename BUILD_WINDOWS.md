# Windows Build And Run

This is the repo-specific build flow for the Windows version of Radioify.

Use this file instead of guessing from memory.

## Known-Good Defaults

- Build script: `.\build.ps1`
- Build directory: `.\build`
- Output directory: `.\dist`
- Main binary: `.\dist\radioify.exe`
- Preferred config: `Release`
- Preferred build command: `.\build.ps1 -Static`

## Requirements

- Windows 10 or newer
- Visual Studio 2022 with `Desktop development with C++`
- CMake
- PowerShell
- `vcpkg` if you need to install dependencies from scratch

## Preferred Build Flow

Open a Windows PowerShell prompt in the repo root.

First-time dependency install:

```powershell
.\build.ps1 -InstallDeps -Static
```

Normal build:

```powershell
.\build.ps1 -Static
```

Clean only:

```powershell
.\build.ps1 -Clean
```

Optional variants:

```powershell
.\build.ps1 -Static
.\build.ps1 -InstallDeps -Static
.\build.ps1 -Static -MelodyAnalysis
.\build.ps1 -InstallDeps -Static -MelodyAnalysis
```

## From WSL

This can work, but on this machine it has also produced false-positive runs
where PowerShell/CMake returned success without leaving a finished
`dist\radioify.exe`.

Use native Windows PowerShell when possible.

If the repo is open in WSL and you still want to trigger the Windows build:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PWD")\\build.ps1" -Static
```

Only use a clean rebuild when the cache is genuinely broken:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PWD")\\build.ps1" -Rebuild
```

After any WSL-triggered build, always verify:

```powershell
Get-Item .\dist\radioify.exe
```

If that file is missing, rerun the build from a native Windows PowerShell prompt
in the repo root.

## Run

Run the app from the repo root:

```powershell
.\dist\radioify.exe
.\dist\radioify.exe "C:\path\to\file-or-folder"
```

Examples:

```powershell
.\dist\radioify.exe "C:\Users\boazp\Music\PS2\FFXII"
.\dist\radioify.exe --no-ascii "C:\Users\boazp\Music"
.\dist\radioify.exe --no-audio "C:\Users\boazp\Music"
```

## Verify The Output

After a build, confirm the binary exists and has a fresh timestamp:

```powershell
Get-Item .\dist\radioify.exe
```

If the app is already running, restart it after rebuilding.

## Common Failure Modes

### 1. Stale CMake Cache Or Path Mismatch

This happens a lot after switching between WSL paths like `/mnt/b/radioify` and
Windows paths like `B:\radioify`.

Use:

```powershell
.\build.ps1 -Static
```

If needed:

```powershell
Remove-Item -Recurse -Force .\build
.\build.ps1 -Static
```

### 2. `vcpkg` Not Found

If `build.ps1` says `vcpkg` could not be found, point it at the Windows vcpkg
install explicitly:

```powershell
.\build.ps1 -InstallDeps -Static -VcpkgRoot "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg"
```

Or set it once for the session:

```powershell
$env:VCPKG_ROOT="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg"
```

If your VS edition is not `Community`, change that path accordingly.

### 3. FFmpeg Not Found

`build.ps1` expects FFmpeg either through `vcpkg_installed` or via
`FFMPEG_DIR`/`FFMPEG_ROOT`.

The usual fix is:

```powershell
.\build.ps1 -InstallDeps -Static
```

### 4. CMake Build Is Flaky

If `cmake --build` behaves oddly, or you want a more direct fallback, build the
generated Visual Studio project with MSBuild:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" .\build\radioify.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
```

If you use `BuildTools`, `Professional`, or `Enterprise`, adjust the MSBuild
path.

If you launched the build from WSL and it claims success but no binary appears,
move to a native Windows PowerShell prompt before retrying this fallback.

### 5. Files In `dist` Are Locked

If rebuilds fail because the executable is still running:

```powershell
Get-Process radioify -ErrorAction SilentlyContinue | Stop-Process -Force
```

Then rebuild again.

### 6. PSF2 Plays Nothing Because BIOS Is Missing

PSF2 needs a BIOS available to the app.

Known-good options:

- Put `hebios.bin` next to `.\dist\radioify.exe`
- Set `RADIOIFY_PSF_BIOS` to the BIOS file path

Check:

```powershell
Get-ChildItem .\dist\hebios.bin
```

## Notes

- `build.ps1` writes to `.\build` and `.\dist`.
- Non-static builds may copy FFmpeg runtime DLLs into `.\dist`.
- Static builds do not copy FFmpeg DLLs.
- Use `.\build.ps1 -Static` as the normal Windows build command.
- Reserve `-Rebuild` for stale-cache/path breakage, not for routine builds.
