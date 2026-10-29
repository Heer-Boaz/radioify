"""Offline Qwen3-ASR recognition followed by Whisper large-v3 alignment.

Invoked only by Radioify with a private workspace containing 16 kHz mono PCM.
The parent owns cancellation, source identity checks and atomic SRT publication.
"""
from __future__ import annotations

from collections import Counter
from difflib import SequenceMatcher
import gc
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import traceback

RATE = 16000
CORE_SECONDS = 10.0
CONTEXT_SECONDS = 5.0
TOKEN_LIMIT = 512
LANGUAGES = {
    "zh": "Chinese", "en": "English", "yue": "Cantonese", "ar": "Arabic",
    "de": "German", "fr": "French", "es": "Spanish", "pt": "Portuguese",
    "id": "Indonesian", "it": "Italian", "ko": "Korean", "ru": "Russian",
    "th": "Thai", "vi": "Vietnamese", "ja": "Japanese", "tr": "Turkish",
    "hi": "Hindi", "ms": "Malay", "nl": "Dutch", "sv": "Swedish",
    "da": "Danish", "fi": "Finnish", "pl": "Polish", "cs": "Czech",
    "fil": "Filipino", "fa": "Persian", "el": "Greek", "hu": "Hungarian",
    "mk": "Macedonian", "ro": "Romanian",
}


def language_code(value):
    key = (value or "").strip().lower()
    if key in ("", "auto", "none"):
        return None
    if key in LANGUAGES:
        return key
    for code, name in LANGUAGES.items():
        if key == name.lower():
            return code
    raise ValueError(f"Unsupported Qwen3-ASR language: {value}")


def write_json(path, value):
    pending = path.with_suffix(".pending")
    pending.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
    # Readers open for short bounded reads. Windows can briefly hold the file
    # during replacement, so retry sharing violations rather than lose the job.
    import time
    for attempt in range(50):
        try:
            pending.replace(path)
            return
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(0.02)


def normalize(text):
    return "".join(c.casefold() for c in text if c.isalnum())


def phrases(words, source):
    """Keep whole phrases when reconciling overlapping recognition windows."""
    pending = []
    groups = []
    for word in words:
        pending.append(word)
        if re.search(r"[.!?。！？][\"'’”]*$", word["text"].strip()):
            groups.append(pending)
            pending = []
    if pending:
        groups.append(pending)
    for group in groups:
        start, end = group[0]["start"], group[-1]["end"]
        confidence = sum(math.log(max(w["probability"], 1e-8)) for w in group) / len(group)
        gaps = sum(max(0, b["start"] - a["end"] - .8) for a, b in zip(group, group[1:]))
        yield {"start": start, "end": end, "source": source,
               "words": group, "probability": math.exp(confidence),
               "compact": all(w["end"] - w["start"] <= 2 for w in group) and gaps == 0,
               "text": "".join(w["text"] for w in group).strip(),
               "score": confidence - .08 * (end - start) / len(group) - .15 * gaps}


def same_occurrence(a, b):
    # Repeated cries within one window, or at different times, are legitimate.
    # Compare whole phrases before subtitle splitting, so an extended final
    # word cannot survive as an unrelated fragment from the weaker window.
    if (a["source"] == b["source"] or
            min(a["end"], b["end"]) <= max(a["start"], b["start"]) or
            abs(a["start"] - b["start"]) > 2.5):
        return False
    if normalize(a["text"]) == normalize(b["text"]):
        return True
    wa = [normalize(word) for word in a["text"].split()]
    wb = [normalize(word) for word in b["text"].split()]
    return (min(len(wa), len(wb)) >= 3 and wa[:3] == wb[:3]) or (
        min(len(wa), len(wb)) >= 3 and SequenceMatcher(None, wa, wb, autojunk=False).ratio() >= .65)


def build_cues(candidates, duration):
    selected = []
    buckets = {}
    for candidate in sorted(candidates, key=lambda c: c["score"], reverse=True):
        bucket = math.floor(candidate["start"] / 3)
        nearby = (kept for key in range(bucket - 1, bucket + 2) for kept in buckets.get(key, []))
        if not any(same_occurrence(candidate, kept) for kept in nearby):
            selected.append(candidate)
            buckets.setdefault(bucket, []).append(candidate)
    cues = []
    for phrase in selected:
        groups, pending = [], []
        for word in phrase.get("words", [phrase]):
            if pending and (word["start"] - pending[-1]["end"] > .8 or
                            word["end"] - pending[0]["start"] > 7 or
                            len("".join(w["text"] for w in pending + [word])) > 80):
                groups.append(pending)
                pending = []
            pending.append(word)
        if pending:
            groups.append(pending)
        for group in groups:
            start = max(0., group[0]["start"] - .08)
            end = min(duration, max(start + .7, group[-1]["end"] + .18))
            text = "".join(w["text"] for w in group).strip()
            if end > start and text:
                cues.append({"start_us": round(start * 1e6), "end_us": round(end * 1e6), "text": text})
    # Do not clamp to the next cue: speakers and combat barks can overlap.
    return sorted(cues, key=lambda cue: (cue["start_us"], cue["end_us"]))


def recognize_window(infer, audio_duration, core_start, core_end, language, depth=0,
                     context_limit=CONTEXT_SECONDS):
    context = min(context_limit, (core_end - core_start) / 2)
    start, end = max(0., core_start - context), min(audio_duration, core_end + context)
    # Shift the window inward at file boundaries instead of losing context.
    # A 20-second clip should receive the same full-window alignment as a
    # 20-second excerpt in the middle of a longer recording.
    width = min(audio_duration, core_end - core_start + 2 * context)
    start = max(0., min(start, audio_duration - width))
    end = start + width
    result = infer(start, end, language)
    if result["limited"] and language is not None:
        result = infer(start, end, None)
    if result["limited"]:
        if depth >= 3 or core_end - core_start <= 1.25:
            # Never publish a truncated loop as a forty-second subtitle.
            raise RuntimeError(f"Qwen3-ASR still reached its output limit near {core_start:.1f}s "
                               "after retrying shorter windows; transcript was not replaced.")
        middle = (core_start + core_end) / 2
        return (recognize_window(infer, audio_duration, core_start, middle, language, depth + 1, context_limit) +
                recognize_window(infer, audio_duration, middle, core_end, language, depth + 1, context_limit))
    return [{**result, "start": start, "end": end,
             "core_start": core_start, "core_end": core_end}]


def run(workspace, runtime):
    os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1",
                      HF_HUB_DISABLE_TELEMETRY="1", TOKENIZERS_PARALLELISM="false",
                      NUMBA_CACHE_DIR=str(workspace / "numba-cache"))
    request = json.loads((workspace / "request.json").read_text(encoding="utf-8"))
    if request.get("version") != 1:
        raise ValueError("Unsupported transcript request version")
    requested_language = language_code(request.get("language"))
    sequence = 0
    device_name = ""

    def progress(fraction, phase):
        nonlocal sequence
        sequence += 1
        write_json(workspace / "progress.json", {"sequence": sequence, "fraction": fraction,
                                                "phase": phase, "device": device_name})

    progress(0., "Loading Qwen3-ASR 1.7B")
    import numpy as np
    import soundfile as sf
    import torch
    from transformers import AutoModelForMultimodalLM, AutoProcessor

    if not torch.cuda.is_available():
        raise RuntimeError("Qwen3-ASR transcription requires an NVIDIA CUDA GPU and a current driver.")
    if not torch.cuda.is_bf16_supported():
        raise RuntimeError("Qwen3-ASR transcription requires a GPU supporting bfloat16 (NVIDIA Ampere or newer).")
    torch.set_num_threads(min(8, os.cpu_count() or 1))
    audio = np.memmap(workspace / "audio.f32", dtype="<f4", mode="r")
    if not len(audio):
        raise ValueError("No audio samples")
    duration = len(audio) / RATE
    model_path = runtime / "models" / "Qwen3-ASR-1.7B-hf"
    processor = AutoProcessor.from_pretrained(model_path, local_files_only=True)
    model = AutoModelForMultimodalLM.from_pretrained(
        model_path, local_files_only=True, dtype=torch.bfloat16,
        device_map="cuda:0", attn_implementation="sdpa").eval()
    device_name = torch.cuda.get_device_name(0)
    progress(.02, "Qwen3-ASR 1.7B ready on CUDA: " + device_name)

    def infer(start, end, language):
        waveform = np.array(audio[round(start * RATE):round(end * RATE)])
        if not np.isfinite(waveform).all():
            raise ValueError("Non-finite audio samples")
        sf.write(workspace / "window.wav", waveform, RATE, subtype="FLOAT")
        inputs = processor.apply_transcription_request(
            audio=str(workspace / "window.wav"), language=LANGUAGES.get(language)
        ).to(model.device, model.dtype)
        with torch.inference_mode():
            output = model.generate(**inputs, max_new_tokens=TOKEN_LIMIT, do_sample=False)
        generated = output[:, inputs["input_ids"].shape[1]:]
        # Check raw output length BEFORE the processor's repetition cleanup.
        limited = int(generated.shape[1]) >= TOKEN_LIMIT
        parsed = processor.decode(generated, return_format="parsed")[0]
        detected = language_code(parsed.get("language")) or language
        return {"text": parsed["transcription"].strip(), "language": detected, "limited": limited}

    # Longer context recovers complete sentences in ordinary conversation.
    # Independent short windows provide competing timings for noisy/overlapping
    # speech. Neither stage carries generated text into the next prompt.
    records = []
    detected_languages = Counter()
    for index in range(math.ceil(duration / 60)):
        start, end = index * 60, min(duration, (index + 1) * 60)
        progress(.02 + .15 * start / duration,
                 f"Qwen3-ASR 1.7B: recognizing conversation ({start:.0f}/{duration:.0f}s)")
        for record in recognize_window(infer, duration, start, end, requested_language, context_limit=4):
            records.append({**record, "kind": "long"})
            if record["language"]:
                detected_languages[record["language"]] += len(record["text"])
    short_language = requested_language
    if short_language is None and detected_languages:
        short_language = detected_languages.most_common(1)[0][0]
    for index in range(math.ceil(duration / CORE_SECONDS)):
        start = index * CORE_SECONDS
        end = min(duration, start + CORE_SECONDS)
        progress(.17 + .35 * start / duration,
                 f"Qwen3-ASR 1.7B: recognizing speech ({start:.0f}/{duration:.0f}s)")
        records.extend({**record, "kind": "short"} for record in
                       recognize_window(infer, duration, start, end, short_language))
    del infer, processor, model
    gc.collect()
    torch.cuda.empty_cache()
    # The alignment model never supplies replacement dialogue. It receives
    # precisely Qwen's recognized words, without game-specific prompts.
    progress(.54, "Loading Whisper large-v3 for word timing")
    import stable_whisper
    checkpoint = runtime / "models" / "large-v3.pt"
    with checkpoint.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != "e5b1a55b89c1367dacf97e3e19bfd829a01529dbfdeefa8caeb59b3f1b81dadb":
        raise RuntimeError("Whisper large-v3 checkpoint checksum mismatch; reinstall the transcript runtime.")
    # Loading by filename omits Whisper's model-specific alignment-head mask!
    # Use the official model name with the verified local download directory.
    # The precheck also ensures Whisper cannot download a replacement at run time.
    aligner = stable_whisper.load_model("large-v3", device="cuda", download_root=str(checkpoint.parent))
    candidates = []
    languages = Counter()
    for index, record in enumerate(records):
        progress(.56 + .43 * index / len(records),
                 f"Whisper large-v3: aligning words ({index + 1}/{len(records)})")
        if not record["text"]:
            continue
        language = record["language"] or requested_language
        if not language:
            raise RuntimeError("Qwen3-ASR returned speech without a detectable language.")
        languages[language] += len(record["text"])
        # Whisper uses 'tl' for Filipino and has no separate Cantonese token.
        alignment_language = {"fil": "tl", "yue": "zh"}.get(language, language)
        waveform = np.array(audio[round(record["start"] * RATE):round(record["end"] * RATE)])
        aligned = aligner.align(waveform, record["text"], language=alignment_language,
                                verbose=None, suppress_silence=False, nonspeech_skip=None,
                                remove_instant_words=False, regroup=False)
        if aligned is None:
            raise RuntimeError(f"Whisper could not align speech near {record['start']:.1f}s")
        words = [{"text": word.word, "start": record["start"] + float(word.start),
                  "end": min(duration, record["start"] + float(word.end)),
                  "probability": float(word.probability)}
                 for segment in aligned.segments for word in segment.words]
        if normalize("".join(w["text"] for w in words)) != normalize(record["text"]):
            raise RuntimeError("Whisper alignment changed Qwen's recognized words")
        # Reconcile overlapping phrase candidates after alignment rather than
        # discarding words at a midpoint seam. Long-window timings must have
        # compact, acoustically supported words; uncertain short-window speech
        # remains reviewable rather than being removed by a confidence filter.
        for phrase in phrases(words, index):
            if record["kind"] == "short" or (phrase["compact"] and phrase["probability"] >= .5):
                candidates.append(phrase)
    cues = build_cues(candidates, duration)
    write_json(workspace / "result.json", {
        "version": 1, "status": "ok", "recognition": "Qwen3-ASR-1.7B",
        "alignment": "Whisper large-v3", "language": languages.most_common(1)[0][0] if languages else "",
        "cues": cues,
    })
    progress(1., "Qwen3-ASR recognition and Whisper timing complete")


if __name__ == "__main__":
    workspace = Path(sys.argv[1]).resolve(strict=True)
    with (workspace / "worker.log").open("w", encoding="utf-8", buffering=1) as log:
        sys.stdout = sys.stderr = log
        try:
            runtime = Path(sys.argv[2]).resolve(strict=True) if len(sys.argv) > 2 else Path(__file__).resolve().parent
            run(workspace, runtime)
        except Exception as error:
            traceback.print_exc()
            write_json(workspace / "result.json", {"version": 1, "status": "error", "error": str(error)})
            sys.exit(1)
