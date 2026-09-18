# Radioify

Console media browser/player with selectable period-radio receiver models.

## Requirements
- Windows 10+ (uses console APIs and Media Foundation)
- CMake 3.16+
- MSVC (Visual Studio Build Tools)
- vcpkg (for `-InstallDeps`)
- A Vulkan-capable GPU and current graphics driver (generated transcripts and
  on-demand editing analysis)

## Build
For the repo-specific Windows build/run flow and common failure recovery, see
[`BUILD_WINDOWS.md`](BUILD_WINDOWS.md).

Quick start:

```powershell
.\build.ps1 -Static
.\build.ps1 -Static -Ninja
.\build.ps1 -Static -Tests
.\dist\radioify.exe
```

The binary is written to `dist/radioify.exe`.
`-Tests` builds every executable registered with CTest before running the
suite. It
does not install or launch the MSIX package.

The default build enables whisper.cpp's Vulkan backend and downloads the
SHA-256-verified multilingual Whisper base model used for offline,
GPU-accelerated transcript generation. Set
`RADIOIFY_WHISPER_MODEL` to use another compatible whisper.cpp model at
runtime. Automatic English evidence rejects English-only checkpoints because
Whisper's translation task requires a multilingual model. Custom models use
ordinary token timestamps unless the matching
official alignment-head preset is explicitly selected with
`RADIOIFY_WHISPER_DTW_PRESET` (for example `base.en`, `small`, or
`large.v3.turbo`; use `none` to disable DTW). Invalid explicit configuration
is reported instead of silently falling back. Automatic language detection is
kept stable after the first chunk with recognized speech; set
`RADIOIFY_WHISPER_LANGUAGE` to `auto` or an explicit two-letter source code
such as `en`, `nl`, `de`, or `fr` when the opening speech is not representative.
Transcript creation fails with
a clear error if the selected Vulkan device cannot initialize; it never
silently falls back to CPU. The normal static build keeps the static MSVC
runtime; it does not require a Visual C++ redistributable install on another
PC.

## Editing review

In video edit mode, open **Edit suggestions** in the editor controls or context
menu. The panel explains the scope, download and limitations before you choose
**Start analysis**. This is a
manual archive-editing job over the original source, independent of playback
position. It proposes **Keep**,
**Shorten**, or **Review**, with visual reasons. Filter and preview proposals;
select one to set the editor's range, then use the existing editing commands.
Analysis never changes the edit decision list or deletes the source.

The local backbone is the official Qwen3-VL 8B Instruct GGUF, with native
temporal video input in llama.cpp. Visual observations are grouped across windows
into events; a separate call classifies each fixed event from its original
observations. The application owns the retention policy, ordering and source
boundaries.
Each 30-second interval receives two seconds
of neighbouring context on either side and samples at 2 fps. There is no fixed
image limit for the entire recording. All intervals are scheduled, but this is
**not every-frame inspection or a guarantee that every event is found**.
This first editing-review route is visual-only: audio and speech are not model
inputs. Tiny text, brief events and long-range narrative dependencies can be
missed. Keep includes context handles; uncertainty and missing assessments
take priority over Shorten. Model judgments remain suggestions to inspect.
Real-video validation found over-fragmented encounters, incorrect names and
over-retention of ordinary movement. Earlier variants also proposed shortening
parts of story scenes and encounter setup. The route is experimental and has **not passed
semantic acceptance**; inspect proposed cuts before using them. See
[editing-review validation](docs/video-editing-review.md#validation-on-2026-09-06).

The first run installs the pinned, checksum-verified
[Qwen3-VL 8B model and projector](https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct-GGUF)
(about 6.19 GB combined) in the per-user model directory. No video is uploaded.
The job currently requires NVIDIA driver-wide memory telemetry (NVML), waits
for at least 11 GiB free before loading and yields below 2 GiB free while
running. These are conservative admission limits, not measured peak usage.
Other Radioify model jobs share a device lease; another application's models
are never unloaded. Unsupported telemetry is reported explicitly.

**Pause analysis** stops its private worker and releases its model
memory. **Resume analysis** reuses completed windows, including after
reopening the video. Atomic checkpoints live under
`%LOCALAPPDATA%\Radioify\cache\video-edit-review`, bound to the source file
instance, timestamps, stream, model hashes, runtime revision and task contract.
Changing these starts a new assessment; old results are not migrated.
Failed/incomplete jobs do not publish a finished review.

See [implementation and validation](docs/video-editing-review.md) for the
reference approach, limitations and tests still pending. This is an offline job
with asynchronous progress, not a promised real-time analyzer.

## Video analysis

AI runs only when requested in the video editor. Ordinary playback does not
start analysis, download models, or generate AI chapters. The former playback
chapter pipeline was retired because its output was not reliable enough.
Existing transcripts, cached chapter files and downloaded models are left intact.

Editor review combines video at 2 fps with time-aligned English speech. Existing
English subtitles or a saved transcript are reused. If needed, Whisper creates
one English SRT beside the source video; no companion manifest is created.
Translation retains source segment times and does not run word alignment.
Names mentioned in dialogue are not evidence that those characters are visible.

See [editing review](docs/video-editing-review.md) for limitations and validation.

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
dist/radioify.exe --radio <file-or-folder>
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

When video uses a native window (windowed, fullscreen, or picture-in-picture),
the media browser remains live in the terminal and marks the current video.
Entering picture-in-picture returns keyboard focus to that browser surface.
Browser navigation keeps its normal ownership there: Backspace goes up and
browser Back follows browser history. Escape stops the active video and returns
to the browser.

- Mouse: select; click to play/open
- Right-click a video in the browser or the active video and choose
  `Generate transcript...` to create its managed `video.transcript.srt`
  sidecar. The active player renders that indexed transcript as a subtitle
  overlay as soon as generation completes. `Regenerate transcript...`
  atomically replaces the managed transcript instead of accumulating
  competing tracks. While generation is running, use the task panel's
  `Cancel` button or choose `Cancel transcript generation` from the video's
  context menu; Radioify asks for confirmation before stopping the task.
- Long-running media jobs stay asynchronous and modeless: browsing and
  playback remain available while the task panel shows progress. `Tab` can
  focus its `Cancel` and `Hide` buttons; hiding the panel leaves a footer
  indicator that can restore it. Task failures open a separate detailed dialog
  with `Retry` when that operation supports retrying.
- Enter: open folder / play file
- Backspace: up
- Arrows: move selection
- PgUp/PgDn: page
- Space or Media Play/Pause: pause/resume
- Media Previous/Next: previous/next track
- Media Stop: stop playback
- Left/Right or [ ]: seek +/-5s
- , / .: previous/next video frame
- F1: open command menu / command palette
- Shift+S: copy the current rendered video frame (including subtitles) to the clipboard
- E: enter the non-destructive video editor; while editing, E requests leaving it
- In the video editor: I/O set the inclusive frame selection; Alt+I/Alt+O clear a boundary; Alt+X clears both
- Esc follows the edit back-stack: clear the current range, request leaving edit mode, or cancel that confirmation
- Delete ripple-removes an explicit In/Out range; T trims at either mark and
  keeps the unmarked side at the current sequence edge
- Drag the visible I/O handles on the program timeline to adjust either boundary; handles clamp instead of crossing
- In the video editor: Ctrl+Z/Ctrl+Y undo/redo; Ctrl+R resets all edits
- The editor bar keeps playback, In/Out, Remove, Keep only, and Done visible.
  `Edit suggestions` opens a review panel without starting or pausing a job.
  Start explicitly, then review Keep / Shorten / Review ranges and full reasons.
  Previous/Next (or Up/Down) changes focus without seeking. Preview plays the
  range and pauses at its end; Select range sets In/Out and seeks to its start.
  Hide suggestion is reversible with Undo hide. Only Remove or Keep only changes
  the edit. Closing the panel keeps analysis running and preserves review focus.
  Analyse again asks for confirmation before replacing suggestions (R confirms;
  Enter or Escape preserves the existing results).
  Right-click remains available for secondary history, export, and discard
  actions.
- Done only leaves edit mode; it never starts or cancels an export. Committed
  edits remain in the live program preview, while an unapplied In/Out selection
  is temporary and is cleared without changing the edit
- Committed edits become the program playback immediately and remain active
  after leaving edit mode; reset or discard restores the unedited source
- Ctrl+E: export edits to a new sibling `- edited.mp4` file; cancellation is
  an explicit context-menu action and the source is never overwritten
- A failed export remains visible for that exact edit revision and can be
  retried from the context menu; changing the edit never inherits stale job state
- The playback context menu can resume, export, or discard an edit session;
  discarding always requires explicit confirmation
- Leaving playback distinguishes exporting the current revision, waiting for
  it, and cancelling a running export. A job must finish or be explicitly
  cancelled before playback can close; no edit is silently lost
- Ctrl+W: switch between ASCII and framebuffer presentation; base playback
  switches between the terminal and a normal native window
- Alt+Enter: toggle fullscreen; from picture-in-picture, enter fullscreen
- Ctrl+P: enter picture-in-picture or return to its exact previous surface
- T: cycle the browser view (grid, list, preview)
- R: cycle dry -> typical 1930s radio -> Philco 37-116 -> dry
- H: toggle 50Hz mode
- O: options (KSS/NSF only)
- V: toggle window vsync
- Shift+Up/Down: volume +/-10%
- Ctrl+Up/Down: radio makeup gain (RG)
- Ctrl+Q or Ctrl+C: quit

## Supported files
- Audio: .wav, .mp3, .flac, .ogg, .wma, .aac, .ac3, .eac3, .aif, .aiff, .aifc, .opus, .oga, .mka, .wv, .tta, .caf, .au, .mp2, .ape, .tak, .amr, .ra, .dts, .dsf, .qcp, .spx, .mpc, .xwma, .w64, .voc, .awb, .gsm, .oma, .aa, .aax, .mlp, .truehd, .ac4, .loas, .latm, .kss, .nsf, .mid, .midi, .vgm, .vgz, .psf, .minipsf, .psf2, .minipsf2
- GSF (GPL, enabled by default; disable with `-DRADIOIFY_DISABLE_GSF_GPL=ON`): .gsf, .minigsf
- Audio (media containers): .m4a, .m4b, .m4r, .m4p, .webm, .mp4, .mov, .mkv, .ogg (audio stream only)
- Video (ASCII preview): .mp4, .m4v, .webm, .mov, .qt, .mkv, .avi, .wmv, .asf, .flv, .mpg, .mpeg, .mpe, .mpv, .m2v, .ts, .m2ts, .mts, .3gp, .3g2, .ogv, .vob, .mxf, .f4v, .dv, .ogm, .ivf, .nut, .rm, .rmvb, .bik, .smk, .wtv, .nsv, .pmp, .divx, .mjpg, .mjpeg, .mj2, .y4m, .roq, .mod, .tod (audio + video)
- Subtitles: .srt, .vtt, .ass, .ssa, .sbv, .sub, .txt, .smi, and .sami sidecars with the exact video basename or a recognized language/role qualifier (including generated `video.transcript.srt` files); unrelated subtitle files in the same directory are never loaded automatically
- Images (ASCII art preview): .jpg, .jpeg, .jpe, .jfif, .png, .bmp, .gif, .tif, .tiff, .webp, .heic, .heif, .avif, .ico

PSF2 playback needs `hebios.bin`. Set `RADIOIFY_PSF_BIOS` to the file path or
place `hebios.bin` next to the PSF2 file, next to `radioify.exe`, or in the
directory from which Radioify was launched.

## Options

Radio filter:

- Playback starts unfiltered. Press `R` to select the representative
  mass-market receiver, press it again for the high-end Philco, and press it a
  third time to return to the original audio.
- `--radio` starts playback with a radio model enabled. `--no-radio` explicitly
  selects the normal unfiltered default.
- `--radio-model typical-1930s` (default model): representative mass-market
  1938 table set, anchored to the five-tube Philco 38-12C with a single-ended
  41 output pentode, approximately 2 W output, and a five-inch speaker.
- `--radio-model philco-37-116`: Philco's high-end 1937/1938 console with a
  push-pull output stage, approximately 15 W output, a 14-inch Type-W speaker,
  and three acoustic clarifiers.

The receiver model and reception environment are independent. The former
selects the radio, amplifier, speaker, and cabinet; the latter selects what
arrives at its antenna. A custom `--radio-settings` / `--radio-preset` overlay
is applied after the selected physical model.

Radio reception:

- `--radio-reception everyday-1938` (default): stable groundwave plus a weak,
  slowly fading skywave path and a rare weak co-channel RF carrier.
- `--radio-reception strong-local`: preserve the former clean, ideal AM
  reception for a strong local station.

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
