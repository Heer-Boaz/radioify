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

From the repo root on your own machine:

```powershell
.\install_radioify.ps1
.\uninstall_radioify.ps1
```

`.\install_radioify.ps1` refreshes the default bundle in
`dist\packages\Radioify-Windows-x64` before installing. Use `-SkipPackage` to
reuse the current bundle as-is.

The packaged install:
- installs the full Radioify MSIX package
- lets the MSIX package own `radioify.exe`, file associations, and Explorer integration
- uses Windows package servicing for install, update, and uninstall

The packaged install triggers a UAC prompt when the development certificate
needs to be trusted in `Cert:\LocalMachine\TrustedPeople`.

The distributable bundle contains the signed `.msix` and its certificate at the
bundle root. The manifest inputs stay under `win11-explorer-integration\` for
package metadata only.

For temporary legacy portable registry integration from a repo checkout instead
of installing the standard MSIX-owned shell package, run the legacy tools:

```powershell
.\legacy\windows_media_app\register_windows_media_app.ps1 -ExecutablePath .\dist\radioify.exe
.\legacy\windows_media_app\unregister_windows_media_app.ps1 -ExecutablePath .\dist\radioify.exe
```

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
dist/radioify.exe --single-instance <file>
dist/radioify.exe --new-instance <file>
```

Windows shell opens default to `same-instance`: opening media from Explorer
forwards the file to the running Radioify instance when one is available. The
incoming file uses the same open route as drag/drop, so active playback is
replaced by the new target. To make Explorer opens always launch separately,
create
`%LOCALAPPDATA%\Radioify\radioify.ini` with:

```ini
shell_open_mode = new-instance
```

Accepted values are `same-instance` and `new-instance`. The environment
variable `RADIOIFY_SHELL_OPEN_MODE` uses the same values, and command-line
`--shell-open-mode`, `--single-instance`, and `--new-instance` override the
configured default for that launch.

## Windows 11 Explorer Integration
Current intent:
- keep `radioify.exe` as the core executable
- keep Windows shell ownership in the MSIX package manifest
- register file context-menu commands declaratively through file associations
- use a small packaged `IExplorerCommand` adapter only for folders
- do not register Desktop/background null-selection menus

The standalone maintenance commands are:

```powershell
.\install_win11_explorer_integration.ps1
.\uninstall_win11_explorer_integration.ps1
```

Run the install command from an elevated PowerShell window. The development
MSIX package is self-signed, so its certificate is trusted in
`Cert:\LocalMachine\TrustedPeople` during install.

The normal packaged installer uses this same lane automatically. Keep these
commands around for maintenance when you want to rebuild or re-register only
the shell package without reinstalling the full app.

Advanced / internal flow:

```powershell
.\build.ps1 -Static -Win11ExplorerIntegration
powershell -ExecutionPolicy Bypass -File .\scripts\windows\install_radioify_msix_package.ps1
```

Dry-run the package actions without changing package or certificate state:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\install_radioify_msix_package.ps1 -WhatIf
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
- Left/Right or [ ]: seek +/-5s
- Ctrl+Left/Right: previous/next track
- , / .: previous/next video frame
- F1: open command menu / command palette
- Ctrl+W: toggle video Window mode (framebuffer-mode)
- Ctrl+P: toggle picture-in-picture in Window mode
- T: toggle picture-in-picture TUI in Window mode
- R: toggle 1938 radio filter
- H: toggle 50Hz mode
- O: options (KSS/NSF only)
- V: toggle window vsync
- Shift+Up/Down: volume +/-10%
- Ctrl+Up/Down: radio makeup gain (RG)
- Q or Ctrl+C: quit

## Supported files
- Audio: .wav, .mp3, .flac, .ogg, .wma, .aac, .ac3, .eac3, .aif, .aiff, .aifc, .opus, .oga, .mka, .wv, .tta, .caf, .au, .mp2, .ape, .tak, .amr, .ra, .dts, .dsf, .qcp, .spx, .mpc, .xwma, .w64, .voc, .awb, .gsm, .oma, .aa, .aax, .mlp, .truehd, .ac4, .loas, .latm, .kss, .nsf, .mid, .midi, .vgm, .vgz, .psf, .minipsf, .psf2, .minipsf2
- GSF (GPL, enabled by default; disable with `-DRADIOIFY_DISABLE_GSF_GPL=ON`): .gsf, .minigsf
- Audio (media containers): .m4a, .m4b, .m4r, .m4p, .webm, .mp4, .mov, .mkv, .ogg (audio stream only)
- Video (ASCII preview): .mp4, .m4v, .webm, .mov, .qt, .mkv, .avi, .wmv, .asf, .flv, .mpg, .mpeg, .mpe, .mpv, .m2v, .ts, .m2ts, .mts, .3gp, .3g2, .ogv, .vob, .mxf, .f4v, .dv, .ogm, .ivf, .nut, .rm, .rmvb, .bik, .smk, .wtv, .nsv, .pmp, .divx, .mjpg, .mjpeg, .mj2, .y4m, .roq, .mod, .tod (audio + video)
- Subtitles: .srt and .vtt sidecar files (same basename, e.g. `video.mkv` + `video.srt`)
- Images (ASCII art preview): .jpg, .jpeg, .jpe, .jfif, .png, .bmp, .gif, .tif, .tiff, .webp, .heic, .heif, .avif, .ico

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
