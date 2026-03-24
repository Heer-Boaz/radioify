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
.\dist\radioify.exe
```

The binary is written to `dist/radioify.exe`.

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

## Controls
- Mouse: select; click to play/open
- Enter: open folder / play file
- Backspace: up
- Arrows: move selection
- PgUp/PgDn: page
- Space: pause/resume
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
place `hebios.bin` next to the PSF2 file, in the executable directory, or in
the current working directory.

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
