# Video editing review

## Scope and current status

The goal is to reduce long gameplay archives while retaining meaningful events,
not merely select spectacular highlights. Keep / Shorten / Review are suggestions
over source-time intervals. Preview, selection, cuts, undo and export remain
editor-owned operations. Analysis never deletes, exports or replaces a source.

The workflow and staged backend are implemented, but **semantic acceptance has
not passed**. Bounded NTE tests now preserve a previously misclassified cinematic
shot and distinguish ordinary traversal from a collapsing route. Event grouping
still over-segments encounters, descriptions can contain unsupported details,
and some proposed cuts are too small to be useful. Inspect footage before cutting.
Automatic playback analysis has been retired; see the 2026-09-12 validation below.

## 2026-09-12: one explicit analysis route

Playback no longer creates an AI service, schedules GPU inference, downloads
chapter models or exposes chapter controls/commands. The exclusive Chapter-Llama
pipeline and its smaller VLM are removed from the source and packaging paths.
Shared inference, model installation, speech evidence and worker ownership now
live under `playback/video/analysis`. The private executable is
`radioify_analysis_worker.exe`. Existing media, transcripts, downloaded models
and historical chapter caches are not deleted or migrated.

Editing review first snapshots a complete English subtitle track or reads the
existing generated English SRT. Otherwise it translates source audio with
Whisper in the isolated worker. All three Qwen stages receive overlapping
original speech cues with their source timestamps. A name spoken in dialogue
does not establish that the character is on screen. Speech remains fallible
evidence, not ground truth or instructions. The exact text and both cue timestamps
participate in the review identity; there is no transcript provenance sidecar or
schema migration.

The reported speech-worker crash was diagnosed from Windows Error Reporting and
the retained minidump `radioify_chapter_worker.exe.6628.dmp`. The crashed executable
belonged to the installed 8 September package
`Radioify.Win11Explorer_2026.908.2220.54_x64__j386kq5ankcap`. Its fail-fast exit
`0xC0000409` came from Whisper's DTW median-filter assertion
`filter_width < a->ne[2]`, not an established stack-buffer overwrite. See the
[pinned Whisper implementation](https://github.com/ggml-org/whisper.cpp/blob/v1.8.3/src/whisper.cpp).
DTW was enabled when the context was created, including translation contexts;
disabling per-call token timestamps did not turn it off. The task is now fixed
at engine initialization, and translation does not enable word-level DTW.
Same-language transcription retains its alignment path.

Functional validation on the shared RTX 5070 Ti (driver VRAM checked before each
inference run; about 13.3 GiB initially free):

- The complete 15:08 FFVII video `FINAL FANTASY VII REBIRTH_20240914013158.webm`
  passed the speech route and published 183 English cues without a worker crash.
  FFmpeg still logged Opus packet warnings; this was not an audio-integrity audit.
- A 200 ms audio excerpt returned no usable dialogue without crashing.
- A newly encoded 32-second excerpt from 04:00 passed speech preparation, both
  visual windows, seven observations, event grouping and both final assessments.
  It decoded 73 images including context overlap and returned two Keep proposals.
  The first run took about 85 seconds; a warm run reused the result in 0.16 seconds.
  These are functional run observations, not an isolated GPU benchmark or a
  real-time guarantee. The proposed names, boundaries and reasons have not been
  independently accepted as correct.
- `build.ps1 -Static -Tests` passed 87 tests, including the isolated runtime-layout
  check, timed-speech prompt/identity tests, worker protocol and editor state tests.
- The production MSIX layout function was also exercised in an isolated staging
  directory. MakeAppx successfully packed all 13 payload files; archive inspection
  confirmed the new worker and absence of Chapter-Llama/old-worker artifacts.
  This test package was not signed or installed. Its isolated copies remain under
  `.tmp/editor-retirement-20260912/msix-runtime`: terminal policy rejected cleanup.

The new app is built into `dist`; building does not replace an already installed
MSIX application. No package installation or GUI automation was performed.

The following design description includes historical quality checks. Passing
the technical route does not supersede their unresolved semantic limitations.

The design uses bounded video windows, timestamped observations and subsequent
event aggregation, also described in
[NVIDIA VSS](https://docs.nvidia.com/vss/latest/long-video-summarization.html).
It is not a port of VSS or a claim of equivalent accuracy. Native temporal input
follows [Qwen3-VL's video reference](https://github.com/QwenLM/Qwen3-VL/blob/main/cookbooks/video_understanding.ipynb).
There are no game names, boss names, HUD templates, timestamp exceptions or
title-based event merges in the task policy.

## Editor workflow

`Edit suggestions` is a stable toolbar/context-menu entry. Opening it shows
scope, local-model requirements and limitations; it does not start or restart
analysis. `Start analysis` is a separate action. Closing the panel leaves an
existing job running, and completion does not reopen it.

Pause requests stop the private worker; Resume becomes available only after
that stop is acknowledged. Completed work survives closing and reopening a video.
Retry continues valid progress. Analyse again requires confirmation and leaves
committed edits unchanged.

The scrollable overview shows source ranges, category durations and complete
reasons. Clicking a row or Previous/Next changes focus only. Preview plays the
retained portion on the current edited timeline without modifying marks or cuts.
It requests synchronized A/V pause at the first playback update reaching the end
or EOF; this is a transport audition, not sample-accurate export clipping. A newer
user seek disarms the automatic stop.

Select range sets marks and seeks while paused. Remove and Keep only are separate
deliberate editing actions. Hiding a suggestion has its own undo. One presentation
owner supplies labels/actions to the toolbar, context menu and panel; console and
native video/PiP share layout and hit geometry.

## Analysis ownership

The same local Qwen3-VL 8B Instruct backbone runs three sequential stages:

1. **Observe video.** Each window produces factual, timestamped activities and an
   uncertainty flag. It makes no retention decision.
2. **Group events.** Each new observation receives one continuity decision:
   does it begin a different event? Current-event context and the latest original
   observation are separate inputs. The application maps the boolean to the
   existing evidence boundary. The model supplies brief rolling context, not
   event indices or an editing decision.
3. **Recognize activity.** Each fixed event is classified from all its original
   observations, never a generated grouping summary. Directly preceding/following
   observations are separate context, not part of the target interval.
   Application policy maps the recognized activity to an archive suggestion.

Recognition labels, prompt definitions, grammar alternatives and retention
mapping share one table:

| Recognized activity | Archive policy |
| --- | --- |
| Story scene, including quiet connecting shots | Keep |
| Major encounter, including introduction, pauses and phases | Keep |
| Notable gameplay, discoveries, achievements or environmental hazards | Keep |
| Routine traversal or repetitive minor combat | Shorten |
| Ordinary menus or idling | Shorten |
| Unclear activity | Review |

Classification returns one factual reason and one activity category. The model
does not choose a different retention preference, invent sub-event boundaries,
or return a parallel list of decisions. This policy is a generic application
preference, not a trained personal preference model.

Large events use actual-tokenizer-admitted pages without dropping observations.
Keep dominates Review, which dominates Shorten across pages, so every page must
permit shortening before the event can be shortened. The displayed reason comes
from the winning assessment. Interruption repeats the current event assessment;
it does not publish an incomplete reduction.

Closed events remain stable during grouping; the last event stays open for the
next observation. This is bounded rolling context, not unlimited attention over
the complete recording. Classification cannot split attacks out of an already
grouped encounter, but the grouping model can still incorrectly separate them.

## Temporal and inference contract

- Source duration must be positive and at most 24 hours. Every 30-second core is
  scheduled with two seconds of neighboring visual context, clipped at source ends.
- One frame is requested every 500 ms, sequentially decoded within each window.
  Actual decoded presentation timestamps are retained. Variable-frame-rate input
  can legitimately repeat a held frame.
- Native temporal frame pairs and timestamps are supplied to the vision model.
  Activity starts select an ordered subset of their timecodes; the application
  derives ends from the next start and retains exact fractional EOF.
  Boundary resolution is approximately one second, not frame-accurate.
- Overlapping observations are clipped to their core once. Native grammars
  constrain output structure; parsers independently validate timestamps, ordering,
  fields and text. Normal JSON whitespace is accepted around punctuation.
- Keep receives two-second editing handles. Keep wins over Review, which wins over
  Shorten where handles overlap. Shorten with uncertain target observations becomes
  Review. Unprocessed intervals remain Review. Distinct events remain separately
  inspectable even when their retention decisions match.
- The deployment uses the official 8B Instruct Q4_K_M weights, F16 projector,
  256 maximum visual tokens per image and a 16,384-token language context.
  Over-budget visual input fails explicitly instead of thinning evidence.
- Vision uses the published
  [Qwen3-VL profile](https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct#generation-hyperparameters):
  temperature 0.7, top-p 0.8, top-k 20 and presence penalty 1.5.
  The text stages instead use task-specific deterministic decoding: top-k 1,
  temperature 0 and no presence penalty. This is not the model card's text default.
  Both use repetition penalty 1, no frequency penalty and a local seed of 42.
- Observation/grouping/classification output budgets are 2,048 / 4,096 / 512
  tokens. These and the context size are local deployment limits, not model maxima.
  Text is not cut off by a fixed character-count grammar; incomplete generation
  fails explicitly.

Editor review now consumes time-aligned English speech as well as video. It reuses
English dialogue subtitles or the saved English transcript; otherwise Whisper
translates the audio. The observation, grouping and assessment stages receive
original overlapping speech cues with source timestamps. Speech is separate from
visual evidence: a mentioned name does not establish a visible character. Music
and non-speech audio are not classified.
Tiny HUD text, brief moments between samples, lost details in observations and
subjective significance remain limitations. Scheduling every interval does not
mean inspecting every source frame or detecting every memorable event.

## Resources and persistence

Radioify inference processes share an interprocess GPU lease. Editing review
checks driver-wide free memory after acquiring it and before loading weights.
It waits for 11 GiB free and yields its own worker below a 2 GiB running reserve.
These are admission margins, not a measured minimum or protection against
instantaneous allocations by another application. Other workloads are never
terminated or unloaded.

The memory reader uses four public
[NVML APIs](https://docs.nvidia.com/deploy/nvml-api/group__nvmlDeviceQueries.html),
matching the inference device by PCI identity. No CUDA toolkit or redistributed
driver DLL is required. Vulkan's WDDM allocation budget was unsuitable in testing:
it reported nearly 15 GiB available when the driver reported about 2.6 GiB free.
Unavailable driver-wide telemetry produces an explicit error, not a process-local
fallback.

A private Windows Job Object releases worker memory on pause, application exit
or GPU pressure. Observation windows, continuity steps and completed event
assessments are atomically checkpointed in one application-cache document under
`%LOCALAPPDATA%\Radioify\cache\video-edit-review`. Nothing is written beside the
source video except one generated English SRT if speech needs transcription.

The cache identity includes source file instance, path, size, modification time,
duration, stream, exact model/projector hashes, runtime revision, prompts,
grammars, inference parameters and the exact speech text and start/end times. A changed source cannot publish stale proposals.
There are no migrations or compatibility branches. Resume after visual completion
skips vision initialization and continues the unfinished text stage.

Transcript publication is independent: it leaves one SRT document, without a
manifest, schema or producer sidecar. The running transcription's source identity
is transient; changes to transcript text invalidate downstream editing analysis.

## Validation on 2026-09-06

The final Windows `build.ps1 -Static -Tests` run passed **88/88** tests
(25.18 seconds of test execution). `dist/radioify.exe` and its private worker
are updated. The vocabulary-only native check reports
`native_review_grammar_ok=1`; it loads no model weights or GPU context.
`git diff --check` passes.

Regressions cover decoded-frame ownership, fractional EOF, ordered partitions,
uncertainty and handles, activity-to-policy mapping, original-versus-context
observations, conservative assessment reduction, independently resumable stages,
stable closed events and atomic rejection of corrupt/non-advancing checkpoints.
Editor tests cover shared actions, pause acknowledgement, preview/seek/EOF
ownership, filters, hide/undo, wrapped reasons and hit geometry. Chapter-hover
tests preserve title styling through wrapping and separate title from ordinal
metadata. Transcript tests reject companion control files.

### Latest bounded model results

The three full-pipeline runs below preceded the final neighboring-context and
activity-policy change. Their recorded visual observations were then reused by
the final text-stage checks. The two kinds of validation are deliberately separate.

| Full-pipeline excerpt | Windows / frames / core observations | Elapsed | Result before final classification policy |
| --- | ---: | ---: | --- |
| NTE source 02:00–04:00, 119.966667 s | 4 / 264 / 22 | 182.39 s | 48 s Shorten; collapse/escape retained, some ordinary traversal overvalued |
| NTE source 42:30–EOF, 109.7 s | 4 / 244 / 27 | 206.98 s | Distinct cinematic and final room recognized; wrong 3 s Shorten inside the cinematic |
| Existing 30 s NTE encounter excerpt | 1 / 60 / 10 | 77.89 s | All Keep, but introduction/fight divided into three events |

The final text-stage checks execute the same inference and assembly code on those
recorded observations, ignore previous grouping/assessments, and modify neither
fixtures nor production caches. They report `text-stage-replay` and zero frames:

- **Ending, 93.39 s:** eight events, all Keep over the complete 109.7 seconds.
  The earlier three-second cinematic cut is gone. The red-sky sequence and final
  conceptual/grid room remain identifiable. The preceding dialogue and cinematic
  are still divided too finely; some reasons add unsupported interpretation.
- **Traversal, 78.61 s:** nine events; 64.966667 seconds proposed Shorten:
  relative 00:00–00:21, 00:28–01:11 and 01:59–01:59.966667.
  The earthquake cue and collapse/escape remain Keep with context handles.
  The final sub-second proposal illustrates remaining over-fragmentation;
  these are suggestions, not independently approved cuts.

The pre-policy traversal warm-cache rerun returned identical proposals in
0.002 s without loading a model. This does not claim a current-policy warm-cache
semantic validation.

The traversal and ending excerpts were CPU-encoded H.264 CRF 20, 8-bit, without
audio, at original dimensions. They are bounded excerpt tests, not full-source
or HDR-equivalence tests. Frame counts include repeated neighboring context,
not every source frame. Original videos, transcripts and chapter documents were
not modified. No GUI was launched and no video uploaded.

These elapsed times are functional observations on a shared GPU, not throughput
benchmarks. Actual pressure tests exercised yielding and preserved checkpoints;
one FFVII text replay stopped when the reserve became unavailable. Earlier
packaged-backend pause/resume runs released the private worker and later resumed
text stages without repeating completed visual windows. The current activity
policy's real pause/resume behavior is covered structurally, not by a new full
source stress run. Asynchronous execution is supported; realtime throughput is
not promised. No current whole-source FFVII or 44-minute NTE semantic pass is claimed.

### What the failed variants established

The earlier per-window decision route shortened quiet parts of encounter setup
and cinematics. A longer protective prompt did not fix this. Batch event/decision
arrays produced shifted reasons or model-owned numbering. Bulk boundary generation
could split 82 FFVII observations into 78 events; other batch continuity output
merged a distinct ending into one event.

Assessing original observations avoids treating a generated summary as factual
evidence, but did not by itself fix retention. Neighboring context let the model
recognize a cinematic shot while still preferring to shorten it. That motivated
the current separation between activity recognition and application-owned policy,
not a timestamp exception, keyword filter or longer protective handles.

These failures are why structural validity and successful inference are not
described as semantic acceptance. The remaining acceptance work includes complete
encounter continuity, factuality, useful proposal granularity and missed-event
rates over representative recordings.

### Background chapters: unresolved regression

NTE's stored chapter plan ends with the chapter beginning at 38:22. Its existing
visual observations already describe the red-sky cinematic from about 42:57 and
the illuminated grid room near 44:13, but the planner places them under the earlier
conversation. This is a planning/segmentation failure, not missing visual samples.

The disappearing/returning enemy also receives multiple chapter entries for one
encounter. This continuation did not change that planner or overwrite its chapter
document. Seeing the final scene in editing review does not repair navigation
chapters, and retaining the battle excerpt does not prove correct grouping of the
complete encounter.

## Diagnostic commands

Use the production backend without a GUI:

```powershell
.\build-ninja-clangcl-static\analysis_inference_probe.exe --gpu-memory
.\build-ninja-clangcl-static\analysis_inference_probe.exe --review-video '<video>' --pause-after 300
```

Omit the deadline for a complete run. Repeating resumes valid progress;
`--fresh` explicitly replaces that source's current review checkpoint.

For an explicit text-only replay:

```powershell
.\build-ninja-clangcl-static\analysis_inference_probe.exe --review-evidence '<video>' '<recorded-observations.json>' --pause-after 240
```

The replay validates observation timestamps against the supplied duration and
shares the production GPU lease/admission/reserve. It does not assert that an
old fixture was produced by the current pipeline.

Recorded observation fixtures for the latest checks are in the ordinary
application cache, with basenames:

- Traversal: `8cdbc0fee28e59dc63c6c7d27542d848a5381826a6386737a51c422e386f9389.json`
- Ending: `0ad9975ac9af8e0312dceb1c529b644add0d26469fe30aa1dbed81707d23ce58.json`
- Battle: `d33331e7ad820fce697dd804c8d739cd39fd30bc5bdae183ddbc23f98ab003b0.json`

The two recreated traversal/ending MP4s were removed after validation
(189,785,661 bytes). They can be regenerated from the unchanged originals.
The existing battle excerpt and earlier retained inspection sheets were preserved.
