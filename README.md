# Radioify

Console media browser/player with a vintage radio filter toggle.

## Requirements
- Windows 10+ (uses console APIs and Media Foundation)
- CMake 3.16+
- MSVC (Visual Studio Build Tools)
- vcpkg (for `-InstallDeps`)

## Build
Install deps (FFmpeg via vcpkg):
```powershell
./build.ps1 -InstallDeps
```
Static deps (FFmpeg via vcpkg):
```powershell
./build.ps1 -InstallDeps -Static
```
If `vcpkg` complains about an invalid Visual Studio instance, install the C++ build tools ("Desktop development with C++") and retry.

Build:
```powershell
./build.ps1
```

Clean build:
```powershell
./build.ps1 -Clean
```
This removes `build` and `dist`.

Rebuild (clean + build):
```powershell
./build.ps1 -Rebuild
```

Static build (no FFmpeg DLLs copied to `dist`):
```powershell
./build.ps1 -Static
```
By default this uses `x64-windows-static` and will override a non-static
`VCPKG_TARGET_TRIPLET`. Set `VCPKG_TARGET_TRIPLET` to
`arm64-windows-static` or `x86-windows-static` if needed.

Or:
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
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
- Q or Ctrl+C: quit

## Supported files
- Audio: .wav, .mp3, .flac, .kss, .nsf, .psf, .minipsf, .psf2, .minipsf2
- Audio (media containers): .m4a, .webm, .mp4 (audio stream only)
- Video (ASCII preview): .mp4, .webm (audio + video)
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
