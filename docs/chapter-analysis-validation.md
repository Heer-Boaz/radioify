# Chapter analysis validation — updated 11 September 2026

Historical report. The playback AI chapter pipeline was retired on 12 September
2026. Its commands and models below describe the removed implementation, not the
current application. See [editing review](video-editing-review.md) for the retained
on-demand workflow.

## Status

The visual-coverage and runtime changes are implemented. Semantic chapter
quality is still experimental, and the NTE recording does **not** pass a strict
chapter-quality acceptance test. Structural success (`state: ready`) means a
complete, valid navigation document, not that every title or boundary is correct.

## Semantic acceptance: NTE 58:27

The two-chapter `Eibon` / `Back Home` result is **rejected**, not a successful
repair of chapter quality. It identifies broad story arcs but hides the distinct
scenes and activities needed for navigation. A structurally valid model response
and a passing CTest run are separate from this acceptance criterion.

The saved 370 visual observations and 743 speech cues already describe the
missing events. Selected source frames were also inspected around 19:50, 25:00,
32:25, 40:06, 42:40 and 50:20. The observations are model-derived evidence, not
an independently annotated ground truth for every boundary. These are useful
manual-review anchors for the reported recording:

| Approximate source interval | Distinction to preserve |
| --- | --- |
| 00:00–14:47 | Several visits, conversations and the confrontation at the fiery portal, not one generic Eibon scene |
| 14:47–19:42 | Pursuit through a different setting, farewell beneath the tree, then the arena transition |
| 19:42–24:53 | The arena confrontation and boss introduction |
| 24:53–39:59 | The continuing Madam T encounter, including changed forms and intervening cinematics |
| 39:59–42:17 | The aftermath and escape, distinct from active combat |
| 42:17–49:53 | Awakening/reunion, followed by the separate Edgar conversation |
| 49:53–57:08 | Separate courtyard and library conversations, not one undifferentiated epilogue |
| 57:08–58:27 | The final menus/travel setup must remain visible in the analysis |

These anchors are validation evidence, **not production timing rules**. Camera
cuts, individual attacks and changed boss names alone do not prove a new
encounter. Conversely, shared characters or an overarching quest do not erase
different activities. There is no target chapter count.

### Rejected replacement trial

[Scene-VLM](https://arxiv.org/html/2512.21778v2) separates boundary decisions from
chapter naming and evaluates central focus shots with context on both sides.
Crucially, its reported results use a scene-segmentation-fine-tuned multimodal
model. Its zero-shot ablation is substantially worse. Copying the context-focus
pattern into an unfine-tuned caption-only planner does not reproduce that model.

A read-only trial used the already-installed Qwen3-VL 8B text backbone on the
original observations and ASR, with ten focus observations and five surrounding
observations on either side. Every focus transition received a Boolean decision;
neither generated titles nor summaries were fed back as evidence. Nonetheless,
the completed prefix proposed 194 scene starts among 341 observations, including
many false boundaries inside continuous activities. This is a quality failure,
not something to fix by truncating, deduplicating or merging titles afterward.

The trial was interrupted when free driver-reported VRAM fell below the 2 GiB
foreground reserve, and it released its GPU allocations. Later checks found
roughly 1.5–2 GiB free, below the 12 GiB admission requirement. No second model
trial was started. The partial output is retained under
`.tmp/chapter-fix-20260911/nte-scene-plan.txt` and `.log`; it is **not** an app
cache. The rejected trial implementation was removed from the source tree.
Production chapters, source media and completed visual observations were not
replaced. Scene-level chapter quality remains unresolved.

## What changed

The reviewed baseline selected images at speech-predicted chapter starts. An
event missing from the speech plan could therefore be absent from visual
evidence; failure of that preliminary planner could prevent visual analysis
entirely. More elaborate wording cannot recover images the model never saw.

Production now scans the entire visual timeline at half-second intervals,
keeps both sides of detected transitions, and fills gaps to at most ten seconds.
The schedule is independent of speech and chapter count. A 4096-frame safety
limit causes an explicit refusal, not a reduction to ten thumbnails.

The isolated worker decodes frames on demand at preserved aspect ratio, up to
1280 pixels on the longest side. Every selected frame belongs to a chronological
observation window of at most four frames and twenty seconds. The pinned
[Qwen3-VL 4B model](https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct-GGUF)
produces one short activity description per window. It is not asked to announce
encounter beginnings or endings from each isolated window.

The [Chapter-Llama captions-plus-ASR planner](https://github.com/lucas-ventura/chapter-llama)
receives the complete interleaved visual observations and speech in one turn.
Its [published inference profile](https://github.com/lucas-ventura/chapter-llama/blob/main/src/models/llama_inference.py)
admits at most 35,000 input tokens and generates at most 1,024 tokens with greedy
decoding. Radioify counts the actual formatted prompt and sizes the native
context for that input plus output. Over-capacity inputs are explicitly refused,
not truncated or replaced with previous model output. Vision remains bounded to
8192 tokens; vision and planner allocations do not overlap. The final parser
validates timestamps and titles; there is no name-based merging or output repair.

ASR is optional. No-audio and no-speech sources retain visual analysis.
Checkpoints preserve completed observations across worker
termination. Final results retain the actual timestamps and observations for
inspection and reject a source that changed while analysis was running.

The full-context chapter worker requires 12 GiB free driver-reported VRAM before
loading and yields if the foreground reserve drops below 2 GiB. Admission is
serialized with other Radioify inference jobs. Unavailable memory is rechecked
at a bounded rate without requiring a playback-permission toggle.
Device-wide telemetry currently uses the NVIDIA driver's NVML interface;
unsupported telemetry and insufficient total memory are reported explicitly,
not treated as temporary busy states that could wait forever.

The private worker schema is an exact-match format guard. The result-cache
identity also includes pipeline policy. Older entries are rejected, not migrated:
there are no old-format readers or backwards-compatibility branches.

## 11 September: repeated-plan regression

The production failure on NTE `2026.08.25 - 01.34.17.16.mp4` (58:27) retained
370 completed visual observations and 743 speech cues. Its rejected planner
output contained 143 entries, all titled `Eibon`. The 100-chapter validation limit
exposed this failure; it was not evidence that the recording needed 143 chapters.

The removed implementation split evidence into 8K-context pages, fed generated
chapter titles back as `Caption` records, and prefilled the next answer with the
previous draft. Those records were not visual observations. The checkpoint no
longer contains partial planner output; retry planning consumes the preserved
observations afresh. The chapter-count guard was not increased, and repeated
titles were not deduplicated after generation.

A read-only text-stage replay of this exact request used 28,355 prompt tokens
and completed with two chapters: `Eibon` (00:00–42:17) and `Back Home`
(42:17–58:27). This resolves the runaway repeated plan, but **is still a coarse
semantic result**, not evidence of scene-accurate chapter segmentation. Vision
was not rerun in this diagnostic. The original failed fixture is preserved
under `.tmp/chapter-fix-20260911/nte-failed`; replay it with:

```powershell
.\build-ninja-clangcl-static\chapter_inference_probe.exe --chapter-evidence .tmp/chapter-fix-20260911/nte-failed
```

The replay uses current planner artifacts with recorded observations, honors
the shared GPU lease, checks available VRAM, and writes neither source files nor
application caches. It has a ten-minute cancellation deadline.

Shared transient-message layout now wraps error messages on UTF-8 display-cell
word boundaries for both console and GPU text-grid rendering, including explicit
newlines. Only a genuinely insufficient viewport height clips the message, with
the existing `~` overflow indicator. Regression tests cover the reported error,
wide characters, tiny viewports, padding/alignment, complete long-input admission,
and GPU-memory deferral without a busy retry loop.

The normal `radioify.exe analyze-chapters` route also completed on the reported
source, resumed all 370 observations without loading the visual model, and
persisted the same two chapters under the existing source key. The obsolete
partial planner draft did not enter the new request. Source size and modification
time were unchanged. The failed private workspace was retired by the normal
successful-job cleanup; the diagnostic copy above preserves its evidence.
`build.ps1 -Static -Tests -Win11ExplorerIntegration` passed 89/89 tests and built
the executable and signed MSIX; no package installation was performed.

## Earlier gameplay regressions and remaining failures

| Recording | Duration | Frames sent to VLM | Observation windows |
| --- | ---: | ---: | ---: |
| FFVII Rebirth `20240830001312.webm` | 9:32 | 238 | 60 |
| NTE `2026.08.25 - 00.15.51.15.mp4` | 44:20 | 1238 | 314 |

Human inspection confirmed the Materia Guardian encounter in FFVII and the
Fogden-Wright encounter around 18:00 in NTE. NTE observations now include that
encounter; it is no longer invisible because of sparse speech-selected images.
However, seeing an encounter and assigning it a useful chapter are different
quality requirements.

In the final product run on the complete NTE observations, `Fogden`
started at 16:31 and included the battle around 18:00. The same plan also
invented an `Outro` at 28:00 with over sixteen minutes remaining. Other titles
were vague. FFVII's broad battle/aftermath boundary was around 7:46, but names
and finer event interpretation still varied. Visual captions can misidentify
characters, opponents or summons; speech recognition and combat barks add
further noise. These are known failures, not fixed by serialization validation.

Follow-up inspection on 6 September located the user's missed final NTE scene:
the rainy conversation changes into a distinct red-sky cinematic around 42:57,
followed by the illuminated grid room near 44:13. The current stored visual
observations describe both, but the displayed chapter plan keeps them under
the conversation chapter starting at 38:22. This is a planning/segmentation
failure, not missing visual evidence. The user's disappearing/reappearing enemy
also remains an encounter-continuity regression. Neither case was patched with
fixed timestamps or name-based merges. See the separate
[editing-review validation](video-editing-review.md#first-real-model-runs) for
actual native-video tests; its results are not chapter-quality improvements.

Earlier experiments included lengthy negative-instruction prompts, a single
large-context plan, unconstrained general-model chapter generation, a Qwen3-VL
8B text-planner comparison, and rolling continue/new decisions. They variously
over-segmented attacks, invented or repeated titles, or merged distinct
activities. The experimental prompt and decision paths are not in production.
The 11 September fix restores complete-input planning for the repeated-plan
regression; it does not turn the earlier semantic failures into passing results.
The 8B comparison model was not selected; existing downloaded models were not
deleted. Neither source video was modified.

## Runtime and verification

Hardware: RTX 5070 Ti, 16 GB. Runs are headless and reuse existing English
transcripts; they are not concurrent-playback smoothness benchmarks. GPU-memory
samples measure total device usage, including other processes, every five seconds.

| Product run | Analysis time | Time / video duration | Sampled total VRAM |
| --- | ---: | ---: | ---: |
| FFVII, cold chapter cache | 2:11 | 0.229 | 10,215 MiB |
| NTE, cold chapter cache | 6:03 | 0.137 | 10,976 MiB |
| No-audio excerpt, cold chapter cache | 11.8 s | 0.389 | 8,978 MiB |

Warm product runs returned identical chapters from the default per-user cache:
FFVII in 0.067 s and NTE in 0.155 s. Both reported `cache_hit: true`. The silent
excerpt completed with `english_text_evidence: false`; its duplicate encounter
titles remain a semantic failure despite successful visual-only execution.

Full product rerun results are recorded locally under
`.tmp/chapter-quality-20260905/final-e2e`. Cold means no completed chapter cache;
it does not mean a fresh speech-transcription pass or an empty filesystem cache.

`build.ps1 -Static -Tests` passed 87/87 tests. One earlier run had a transient
failure in the unrelated native keyboard-input test; its standalone retry and
the complete rerun passed without changing that code. Added coverage includes
silent visual evidence, a short visual event without an ASR boundary, complete
window coverage, source-backed requests with zero staged RGB bytes, ordered
boundary grammar, invalid observations and exact rejection of older/newer
worker schemas.

A no-audio 30-second excerpt also completed the isolated-worker yield test:
the worker was terminated after one observation, then resumed at the next
unfinished interval. This verifies release/resume mechanics, not a frame-time
or input-latency guarantee during gameplay.

Background analysis is technically realistic on this GPU. Live frame-by-frame
recognition with a reliable immediate chapter decision is not demonstrated.
The current product publishes chapters only after the complete analysis.
Coverage, throughput, memory use and semantic accuracy must remain separate
acceptance criteria. Further semantic work needs a broader labelled evaluation
set, including encounter onset/aftermath and non-gameplay videos, before another
model or planner is promoted to production.
