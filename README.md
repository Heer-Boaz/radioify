# Radioify

Console media browser/player with selectable period-radio receiver models.

## Requirements
- Windows 10+ (uses console APIs and Media Foundation)
- CMake 3.16+
- MSVC (Visual Studio Build Tools)
- vcpkg (for `-InstallDeps`)
- A Vulkan-capable GPU and current graphics driver (generated transcripts and
  automatic video chapters)

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
suite, and validates chapter resources from an isolated staged directory. It
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

For each active video, Radioify starts asynchronous chapter analysis after
subtitle discovery completes. This feature is deliberately GPU-only: source
frames must decode through D3D11VA, the pinned MiniCPM-V 2.0 visual model and
projector must run through Vulkan, and the Meta Llama 3.1 8B planning model must
fit completely on a Vulkan device. The llama.cpp adapter verifies the
projector backend after initialization, so a library-level CPU fallback is
rejected rather than merely requested away. Playback remains the foreground
GPU owner. Model inference runs in a private Windows Job Object: complete stage
boundaries are atomically checkpointed, and Radioify terminates the worker to
reclaim all Vulkan allocations whenever playback buffers, seeks, or starves.
Pass `--no-automatic-chapters` to disable automatic chapters for a launch.

**Built with Llama.** The packaged `models/chapter_analysis` directory includes
the Llama 3.1 Community License and Meta's required attribution notice. Use of
the planner is also subject to the incorporated
[Llama 3.1 Acceptable Use Policy](https://llama.meta.com/llama3_1/use-policy).

Open `Chapters` in the playback controls to install the fixed,
SHA-256-verified model artifacts once. Radioify downloads the MiniCPM-V model
and projector plus the Chapter-Llama base model (about 7.75 GB total) into the
per-user data directory. Releases include the two small, pinned Chapter-Llama
adapters and their attribution notice; the base models are not bundled.
Chapter analysis requires English timecoded text, independently of the
subtitle track selected for presentation. Radioify uses that complete
transcript with Chapter-Llama's published ASR adapter to predict candidate
boundaries, then samples exactly one frame at each candidate. MiniCPM-V captions
those frames and the published captions-plus-ASR adapter jointly produces the
final chapter boundaries and navigation titles. Existing English subtitles or
a persisted English transcript are reused. When neither exists, the same
asynchronous analysis request runs Whisper's translation task and atomically
publishes a source-bound `video.ext.radioify.transcript.en.srt` plus provenance
record before planning. The private name prevents background work from ever
overwriting a user-authored subtitle or transcript. Foreground playback can
preempt each Whisper chunk; Radioify releases the Vulkan model allocation,
retains the exact decoded PCM transaction, and resumes without an approximate
media seek. Sources without decodable speech fail explicitly. There is no
periodic visual-only fallback.

Automatic runtime analysis currently accepts videos from 30 seconds through
60 minutes. This is a Radioify admission contract for the single-pass runtime
implementation, not a claimed limit of the research model. A bounded preflight
rejects evidence that cannot fit the planner context before either inference
model is loaded; longer media will require Chapter-Llama's separately published
iterative procedure rather than silently thinning the timeline.

The inference backend links the vcpkg-baseline-pinned `llama` and `libmtmd`
libraries directly. Their native objects live behind one RAII-owned Radioify
adapter; libmtmd's published helper API owns multimodal batching, M-RoPE
positions, and `llama_decode` orchestration instead of duplicating that vendor
logic in Radioify. Radioify packages both official Chapter-Llama adapters.
MiniCPM-V 2.0 receives the reference single-image question at each
ASR-predicted candidate and returns ordinary caption text. Radioify then
chronologically interleaves `Caption HH:MM:SS` and `ASR HH:MM:SS` records and
passes that evidence to the captions-plus-ASR adapter with the published task
prompt. No Radioify-authored semantic prompt or title-repair stage participates
in chapter planning.

The final adapter's second-resolution boundary times and titles are accepted
only when they begin at zero, strictly increase, remain inside the source, and
form a complete partition. Invalid or over-budget output is rejected rather
than sorted, snapped, deduplicated, summarized, or repaired. Only that complete
validated chapter domain object is published. The private helper
uses a bounded, schema-versioned file protocol and atomically published
checkpoints; no shell command, temporary PNG, or diagnostic-log parsing
participates in inference.

Because llama.cpp marks `libmtmd` experimental, its version is pinned at the
build boundary rather than allowed to drift at runtime. The visual model and
projector come from OpenBMB's published
[MiniCPM-V 2.0 GGUF repository](https://huggingface.co/openbmb/MiniCPM-V-2-gguf)
at a fixed revision and are accepted only at their compiled-in sizes and
SHA-256 hashes. Both planner adapters are MIT-licensed
[Chapter-Llama artifacts](https://huggingface.co/lucas-ventura/chapter-llama)
at a fixed revision, converted reproducibly for llama.cpp and paired with a
pinned Meta Llama 3.1 8B Instruct GGUF subject to its community license.

The architecture follows the complete published
[Chapter-Llama inference pipeline](https://github.com/lucas-ventura/chapter-llama).
Its persistent chapter artifact follows the same start-time-and-title product
shape used by [Mux AI](https://github.com/muxinc/ai/blob/main/src/workflows/chapters.ts).

A completed result is atomically cached under
`%LOCALAPPDATA%\Radioify\cache\video-chapters`. Its identity includes the
source file, selected stream, every model/adapter hash, and English text
evidence, so reopening unchanged media publishes the result immediately while changed
inputs are analyzed again. Private in-progress inference checkpoints use the
same identity, so a foreground GPU yield resumes at a completed model-stage
boundary instead of relabeling partial output. A transient cache-write failure keeps the completed
in-memory result available for the active session and reports a non-fatal OSD
warning. In-progress or unsupported analysis never opens an
empty chapter or timeline-metadata panel; failures remain visible through the
normal playback status/retry surface.

For a presentation-free production-path diagnostic, run:

```powershell
.\dist\radioify.exe analyze-chapters "C:\path\to\video.mkv"
```

This uses the same metadata probe, English-evidence preparation, chapter
service, GPU backend, validation, and durable cache as playback. Progress goes
to stderr; the final JSON document on stdout reports the cache key, cache path,
and whether the completed result is persisted. It never opens a playback
window and never installs models without explicit user interaction.

The explicit hardware/model end-to-end test uses a fresh isolated cache, runs
that production path twice, and requires a cold first run followed by discovery
of its durable result:

```powershell
.\scripts\test\Invoke-RadioifyChapterE2E.ps1 `
  -ApplicationPath .\dist\radioify.exe `
  -VideoPath "C:\path\to\video.mkv" `
  -RequireGeneratedTranscript
```

This test is intentionally separate from CTest because it requires supported
GPU hardware, locally installed model artifacts, and representative media. It
uses a private scratch media fixture so cache and sidecar discovery both start
cold (a hard link where supported, otherwise an isolated copy), uses hidden
child processes, and does not require an installed MSIX package. Pass
`-ScratchRoot` when the system temp volume cannot hold the representative
video.

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
- `Chapters` opens a responsive chapter list. Once analysis is
  ready, chapter boundaries appear on the shared timeline in both ASCII and
  framebuffer presentation. Hovering anywhere on that timeline keeps the
  existing preview frame and adds the chapter title and time range;
  metadata wraps to the responsive panel width. Until usable chapter content
  exists, the chapter list stays closed and the hover preview remains frame-only
  instead of reserving an empty status panel.
- Right-click the active video to start chapter analysis manually, inspect a
  failure, approve model installation, or cancel/retry the current request.
  The same typed actions are exposed in terminal and framebuffer playback.
  While analysis runs, its context-menu action reports the current phase or
  progress without taking permanent toolbar space. `--no-automatic-chapters`
  disables only the automatic trigger; manual analysis remains available.
- Enter: open folder / play file
- Backspace: up
- Arrows: move selection
- PgUp/PgDn: page
- Space or Media Play/Pause: pause/resume
- Media Previous/Next: previous/next track
- Ctrl+Left/Right: previous/next video chapter (when chapters are ready)
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
  `Suggestions` detects useful segments and opens a persistent review workflow
  with filters for cutscenes, dialogue, gameplay, and menu/loading ranges.
  Previous/Next seeks between suggestions, Select range only sets In/Out, Hide
  is reversible with Undo hide, and no suggestion changes the edit by itself.
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
