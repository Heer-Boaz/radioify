#!/usr/bin/env python3
"""Generate simple test tones and run radioify extract-sheet on them."""

from __future__ import annotations

import argparse
import math
import os
import subprocess
import wave
import struct
from pathlib import Path


def write_wav(path: Path, segments, sample_rate: int, amplitude: float = 16000.0) -> None:
    """Write a mono PCM16 wav with tone/silence segments.

    `segments` is a list of (frequency_hz, duration_seconds), where frequency 0 means silence.
    """
    sample_rate = int(sample_rate)
    path = path.with_suffix(".wav")

    frames = []
    for freq_hz, duration in segments:
        if duration <= 0:
            continue
        frame_count = int(sample_rate * float(duration))
        freq = float(freq_hz)
        for i in range(frame_count):
            t = i / sample_rate
            if freq <= 0.0:
                value = 0.0
            else:
                value = math.sin(2.0 * math.pi * freq * t)
            sample = int(max(-1.0, min(1.0, value)) * amplitude)
            frames.append(struct.pack("<h", sample))

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(b"".join(frames))


def run_extract(binary: Path, input_wav: Path, output_melody: Path) -> None:
    cmd = [str(binary), "extract-sheet", str(input_wav), "--out", str(output_melody)]
    subprocess.run(cmd, check=True)


def read_midi_runs(path: Path, max_items: int = 12) -> list[tuple[int, int]]:
    runs: list[tuple[int, int]] = []
    current = None
    count = 0
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = line.strip().split()
        if len(parts) != 4:
            continue
        try:
            frame = int(parts[0])
        except ValueError:
            continue
        if frame < 0:
            continue
        try:
            midi = int(float(parts[3]))
        except ValueError:
            continue
        if midi < 0:
            continue
        if current is None:
            current = midi
            count = 1
            continue
        if midi == current:
            count += 1
        else:
            runs.append((current, count))
            current = midi
            count = 1
    if current is not None:
        runs.append((current, count))
    return runs[:max_items]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        default="dist/radioify.exe",
        help="Path to radioify executable (default: dist/radioify.exe)",
    )
    parser.add_argument(
        "--out-dir",
        default="test_waves",
        help="Directory to store generated wav/melody files",
    )
    parser.add_argument("--sr", type=int, default=22050, help="Sample rate for generated tones")
    parser.add_argument("--amplitude", type=float, default=16000.0, help="Wave amplitude")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only generate wavs, do not call extract-sheet",
    )
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    binary = Path(args.binary)
    if not binary.exists():
        raise FileNotFoundError(f"radioify binary not found: {binary}")

    samples = {
        "melody_simple": [
            (440.0, 1.0),
            (0.0, 0.2),
            (523.25, 1.0),
            (0.0, 0.2),
            (659.25, 1.0),
            (0.0, 0.2),
            (783.99, 1.0),
        ],
        "melody_step": [
            (440.0, 1.0),
            (523.25, 1.0),
            (659.25, 1.0),
            (783.99, 1.0),
        ],
        "tone_440": [(440.0, 3.0)],
        "tone_drop": [
            (880.0, 0.6),
            (440.0, 0.7),
            (220.0, 0.8),
        ],
    }

    created: list[Path] = []
    for name, segments in samples.items():
        wav_path = out_dir / f"{name}.wav"
        melody_path = out_dir / f"{name}.melody"
        print(f"generate {wav_path}")
        write_wav(wav_path, segments, sample_rate=args.sr, amplitude=args.amplitude)
        created.append(wav_path)
        if args.dry_run:
            continue
        print(f"extract {wav_path} -> {melody_path}")
        run_extract(binary, wav_path, melody_path)
        if melody_path.exists():
            runs = read_midi_runs(melody_path)
            print(f"runs: {runs}")
        else:
            print(f"missing output: {melody_path}")

    print(f"created {len(created)} wav files in {out_dir}")
    if not args.dry_run:
        print(f"melody outputs in {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
