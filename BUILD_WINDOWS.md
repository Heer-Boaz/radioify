# Windows Build And Run

This is the repo-specific build flow for the Windows version of Radioify.

Use this file instead of guessing from memory.

## Known-Good Defaults

- Build script: `.\build.ps1`
- Build directory: `.\build-ninja` when Ninja is auto-detected, otherwise `.\build`
- Output directory: `.\dist`
- Main binary: `.\dist\radioify.exe`
- Preferred config: `Release`
- Preferred build command: `.\build.ps1 -Static`
- Optional Ninja build command: `.\build.ps1 -Static -Ninja`

## Requirements

- Windows 10 or newer
- Visual Studio 2022 with `Desktop development with C++`
- CMake
- PowerShell
- `vcpkg` if you need to install dependencies from scratch

`build.ps1` now tries to auto-detect the Visual Studio-bundled `vcpkg` via
`vswhere`, so in the normal case you should not need to set `VCPKG_ROOT`
manually.

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

Normal build with Ninja progress output:

```powershell
.\build.ps1 -Static -Ninja
```

Clean only:

```powershell
.\build.ps1 -Clean
```

Optional variants:

```powershell
.\build.ps1 -Static
.\build.ps1 -InstallDeps -Static
.\build.ps1 -Static -Ninja
.\build.ps1 -InstallDeps -Static -Ninja
.\build.ps1 -Static -MelodyAnalysis
.\build.ps1 -InstallDeps -Static -MelodyAnalysis
```

## From WSL

This is supported and has been tested from this repo. Use `powershell.exe`,
not `pwsh`.

If the repo is open in WSL and you still want to trigger the Windows build:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PWD")\\build.ps1" -Static
```

Equivalent wrapper:

```bash
./build_windows.sh
```

Only use a clean rebuild when the cache is genuinely broken:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PWD")\\build.ps1" -Rebuild
```

After any WSL-triggered build, verify:

```powershell
Get-Item .\dist\radioify.exe
```

If that file is missing, the build did not really finish successfully.

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

If `build.ps1` says `vcpkg` could not be found, first resolve the Visual Studio
copy dynamically instead of hardcoding a specific VS edition:

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$env:VCPKG_ROOT = Join-Path (& $vswhere -latest -products * -property installationPath) "VC\vcpkg"
.\build.ps1 -InstallDeps -Static
```

Or pass that resolved path explicitly:

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
.\build.ps1 -InstallDeps -Static -VcpkgRoot (Join-Path (& $vswhere -latest -products * -property installationPath) "VC\vcpkg")
```

If you use a custom standalone `vcpkg`, point `-VcpkgRoot` at that directory
instead.

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
- `build.ps1 -Ninja` writes to `.\build-ninja` and `.\dist`.
- Non-static builds may copy FFmpeg runtime DLLs into `.\dist`.
- Static builds do not copy FFmpeg DLLs.
- Use `.\build.ps1 -Static` as the normal Windows build command.
- Reserve `-Rebuild` for stale-cache/path breakage, not for routine builds.
