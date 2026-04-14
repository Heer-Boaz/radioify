# Radioify

Console media browser/player with a vintage radio filter toggle.

## Requirements
- Windows 10+ (uses console APIs and Media Foundation)
- CMake 3.16+
- MSVC (Visual Studio Build Tools)
- vcpkg (for `-InstallDeps`)

## Build
For the repo-specific Windows build/run flow and common failure recovery, see
[`BUILD_WINDOWS.md`](BUILD_WINDOWS.md).

Quick start:

```powershell
.\build.ps1 -Static
.\build.ps1 -Static -Ninja
.\dist\radioify.exe
```

The binary is written to `dist/radioify.exe`.

## Windows Package
Build a distributable Windows x64 bundle and zip:

```powershell
.\package_windows.ps1
```

From WSL/Linux:

```bash
./package_windows.sh
```

From `cmd.exe`:

```bat
package_windows.cmd
```

That writes:
- `dist\packages\Radioify-Windows-x64\`
- `dist\packages\Radioify-Windows-x64.zip`

On another Windows PC:
- extract the zip
- run `install_radioify.cmd` for a per-user install
- or run `radioify.exe` directly for portable use

The packaged install:
- copies Radioify to `%LOCALAPPDATA%\Programs\Radioify`
- registers it as a Windows media app
- adds a per-user uninstall entry in Apps & Features

The experimental Windows 11 Explorer integration is intentionally not part of
the normal package lane.

## CI Packaging
A GitHub Actions workflow at [windows-package.yml](/mnt/b/radioify/.github/workflows/windows-package.yml)
builds the same Windows package and uploads `Radioify-Windows-x64.zip` as an artifact.

## Run
```sh
dist/radioify.exe
dist/radioify.exe <file-or-folder>
```

Optional flags:
```sh
dist/radioify.exe --no-ascii <file-or-folder>
dist/radioify.exe --no-audio <file-or-folder>
dist/radioify.exe --no-radio <file-or-folder>
```

## Windows 11 Explorer Integration
This lane is currently experimental bring-up work, not a finished or preferred
integration path.

Current intent:
- keep `radioify.exe` as the core executable
- keep Explorer integration in a separate `IExplorerCommand` shell extension
- use a sparse-package lane only for testing modern Win11 integration

The normal front-door commands are:

```powershell
.\install_win11_explorer_integration.ps1
.\uninstall_win11_explorer_integration.ps1
```

Run the install command from an elevated PowerShell window. The development
MSIX package is self-signed, so its certificate is trusted in
`Cert:\LocalMachine\TrustedPeople` during install.

Current reality:
- the build/install lane exists
- end-to-end Explorer activation is still being proven
- this should be treated as a debug path, not stable product UX

Advanced / internal flow:

```powershell
.\build.ps1 -Static -Win11ExplorerIntegration
powershell -ExecutionPolicy Bypass -File .\scripts\windows\install_radioify_win11_context_menu.ps1
```

Dry-run the package actions without changing package or certificate state:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\install_radioify_win11_context_menu.ps1 -WhatIf
```

You can skip the rebuild during repeated install testing:

```powershell
.\install_win11_explorer_integration.ps1 -SkipBuild
```

The concrete implementation plan lives in
[`WINDOWS11_EXPLORER_INTEGRATION.md`](WINDOWS11_EXPLORER_INTEGRATION.md).

## Controls
- Mouse: select; click to play/open
- Enter: open folder / play file
- Backspace: up
- Arrows: move selection
- PgUp/PgDn: page
- Space or Media Play/Pause: pause/resume
- Media Previous/Next: previous/next track
- Media Stop: stop playback
- Ctrl+Left/Right: seek
- R: toggle 1938 radio filter
- H: toggle 50Hz mode
- O: options (KSS/NSF only)
- V: toggle window vsync
- Shift+Up/Down: volume +/-10%
- Ctrl+Up/Down: radio makeup gain (RG)
- Q or Ctrl+C: quit

## Supported files
- Audio: .wav, .mp3, .flac, .ogg, .kss, .nsf, .psf, .minipsf, .psf2, .minipsf2
- GSF (GPL, enabled by default; disable with `-DRADIOIFY_DISABLE_GSF_GPL=ON`): .gsf, .minigsf
- Audio (media containers): .m4a, .webm, .mp4, .mov, .mkv, .ogg (audio stream only)
- Video (ASCII preview): .mp4, .webm, .mov, .mkv (audio + video)
- Subtitles: .srt and .vtt sidecar files (same basename, e.g. `video.mkv` + `video.srt`)
- Images (ASCII art preview): .jpg, .jpeg, .png, .bmp

PSF2 playback needs `hebios.bin`. Set `RADIOIFY_PSF_BIOS` to the file path or
place `hebios.bin` next to the PSF2 file, next to `radioify.exe`, or in the
directory from which Radioify was launched.

## Options
KSS options:
- 50Hz (auto/forced)
- SCC type (auto/standard/enhanced)
- PSG quality (auto/low/high)
- SCC quality (auto/low/high)
- OPLL stereo (on/off)
- PSG mute (on/off)
- SCC mute (on/off)
- OPLL mute (on/off)

NSF options:
- EQ preset (NES/Famicom)
- Stereo depth (off/50%/100%)
- Ignore silence (on/off)
