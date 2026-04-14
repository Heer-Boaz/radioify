# Windows Build And Run

This is the repo-specific build flow for the Windows version of Radioify.

Use this file instead of guessing from memory.

## Known-Good Defaults

- Build script: `.\build.ps1`
- Build directory: depends on the selected preset/toolchain, for example `.\build-ninja-clangcl-static` for the default static clang-cl path
- Published output directory: `.\dist`
- Native link output: under the active build dir, usually `.\build-...\bin`
- Main binary: usually published to `.\dist\radioify.exe`
- Preferred config: `Release`
- Preferred build command: `.\build.ps1 -Static`

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

```powershell
.\build.ps1 -InstallDeps -Static
.\build.ps1 -Static
.\build.ps1 -Static -MelodyAnalysis
.\build.ps1 -Clean
.\package_windows.ps1
.\install_radioify.ps1
.\uninstall_radioify.ps1
```

By default `.\install_radioify.ps1` repackages the current repo state before
installing. Use `-SkipPackage` only when you intentionally want to reuse the
existing bundle under `dist\packages\Radioify-Windows-x64`. The install now
also installs the Win11 Explorer context menu integration, so expect a UAC
prompt during the install/uninstall flow.

Equivalent packaging entrypoints from the repo root:

```bash
./package_windows.sh
```

```bat
package_windows.cmd
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

Only use a rebuild when the cache is genuinely broken:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PWD")\\build.ps1" -Rebuild
```

After any WSL-triggered build, verify the published binary when `dist` is not locked:

```powershell
Get-Item .\dist\radioify.exe
```

If `dist` is locked by a running process, the build script now leaves the fresh executable in the active build dir and prints that path explicitly.

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

## Common Failure Modes

### 1. Stale CMake Cache Or Path Mismatch

This happens a lot after switching between WSL paths like `/mnt/b/radioify` and
Windows paths like `B:\radioify`.

Usually just rerun:

```powershell
.\build.ps1 -Static
```

If the cache is genuinely broken, use:

```powershell
.\build.ps1 -Rebuild -Static
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

### 4. Files In `dist` Are Locked

If the published executable is still running:

```powershell
Get-Process radioify -ErrorAction SilentlyContinue | Stop-Process -Force
```

Then rebuild again. If you do not stop it, the build can still succeed, but the fresh executable will remain under the active build dir instead of being copied into `.\dist`.

### 5. PSF2 Plays Nothing Because BIOS Is Missing

PSF2 needs a BIOS available to the app.

Known-good options:

- Put `hebios.bin` next to `.\dist\radioify.exe`
- Set `RADIOIFY_PSF_BIOS` to the BIOS file path

Check:

```powershell
Get-ChildItem .\dist\hebios.bin
```

## Notes

- `build.ps1` writes to a preset-specific build dir such as `.\build`, `.\build-static`, `.\build-ninja`, or `.\build-ninja-clangcl-static`.
- CMake links executables into the active build dir first, then the script publishes `radioify.exe` into `.\dist` when that path is not locked.
- Non-static builds may copy FFmpeg runtime DLLs into `.\dist`.
- Static builds do not copy FFmpeg DLLs.
- Reserve `-Rebuild` for stale-cache/path breakage, not for routine builds.
