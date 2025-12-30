# Radioify

Console media browser/player with a vintage radio filter toggle.

## Requirements
- Windows 10+ (uses console APIs and Media Foundation)
- CMake 3.16+
- MSVC (Visual Studio Build Tools)

## Build
```powershell
./build.ps1
```

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

## Controls
- Mouse: select; click to play/open
- Enter: open folder / play file
- Backspace: up
- Arrows: move selection
- PgUp/PgDn: page
- Space: pause/resume
- Ctrl+Left/Right: seek
- R: toggle 1938 radio filter
- Q or Ctrl+C: quit

## Supported files
- Audio: .wav, .mp3, .flac
- Audio (media containers): .m4a, .webm, .mp4 (audio stream only)
- Video (ASCII preview): .mp4, .webm (audio + video)
- Images (ASCII art preview): .jpg, .jpeg, .png, .bmp
