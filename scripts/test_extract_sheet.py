#!/usr/bin/env python3
"""Generate test tones, run extract-sheet, and evaluate roundtrip quality."""

from __future__ import annotations

import argparse
import difflib
import math
import re
import shutil
import subprocess
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

    import wave

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(b"".join(frames))


def run_extract(binary: Path, input_file: Path, output_melody: Path) -> None:
    cmd = [str(binary), "extract-sheet", str(input_file), "--out", str(output_melody)]
    subprocess.run(cmd, check=True)


def run_midi_to_wav(ffmpeg: str, midi_path: Path, output_wav: Path, sample_rate: int | None) -> None:
    cmd = [ffmpeg, "-y", "-i", str(midi_path), "-ac", "1"]
    if sample_rate is not None:
        cmd.extend(["-ar", str(sample_rate)])
    cmd.extend(["-c:a", "pcm_s16le", "-f", "wav", str(output_wav), "-loglevel", "error"])
    subprocess.run(cmd, check=True)


def query_sample_rate(path: Path, ffprobe: str | None) -> int | None:
    if ffprobe is None:
        return None
    result = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=sample_rate",
            "-of",
            "default=nw=1:nk=1",
            str(path),
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    value = result.stdout.strip().splitlines()[-1:] if result.stdout else []
    if not value:
        return None
    try:
        return int(value[0])
    except ValueError:
        return None


def _parse_vlq(data: bytes, offset: int) -> tuple[int, int] | None:
    value = 0
    for _ in range(4):
        if offset >= len(data):
            return None
        byte = data[offset]
        offset += 1
        value = (value << 7) | (byte & 0x7F)
        if (byte & 0x80) == 0:
            return value, offset
    return None


def _channel_data_len(status: int) -> int:
    high = status & 0xF0
    if high in (0xC0, 0xD0):
        return 1
    return 2


def validate_midi_smf(path: Path) -> tuple[bool, str]:
    if not path.exists():
        return False, "missing file"

    try:
        data = path.read_bytes()
    except OSError as exc:
        return False, f"read failed: {exc}"

    if len(data) < 14 or data[:4] != b"MThd":
        return False, "missing MThd header"
    if int.from_bytes(data[4:8], "big") != 6:
        return False, "invalid MThd length"

    format_type = int.from_bytes(data[8:10], "big")
    track_count = int.from_bytes(data[10:12], "big")
    division = int.from_bytes(data[12:14], "big")
    if format_type > 2:
        return False, f"unsupported MIDI format {format_type}"
    if track_count <= 0:
        return False, "track_count is zero"
    if division == 0:
        return False, "invalid division"

    offset = 14
    parsed_tracks = 0
    note_events = 0
    while offset < len(data):
        if offset + 8 > len(data):
            return False, f"truncated chunk header at {offset}"
        chunk_id = data[offset : offset + 4]
        chunk_len = int.from_bytes(data[offset + 4 : offset + 8], "big")
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_len
        if chunk_end > len(data):
            return False, f"chunk {chunk_id!r} out of bounds"

        if chunk_id == b"MTrk":
            parsed_tracks += 1
            idx = chunk_start
            running_status = None
            while idx < chunk_end:
                vlq = _parse_vlq(data, idx)
                if vlq is None:
                    return False, f"invalid delta-time at track {parsed_tracks}"
                _, idx = vlq
                if idx >= chunk_end:
                    return False, f"truncated event at track {parsed_tracks}"

                status_or_data = data[idx]
                explicit_status = (status_or_data & 0x80) != 0
                if explicit_status:
                    idx += 1
                    status = status_or_data
                else:
                    if running_status is None:
                        return False, f"running status without prior status at track {parsed_tracks}"
                    status = running_status

                if status == 0xFF:
                    if idx >= chunk_end:
                        return False, f"truncated meta event at track {parsed_tracks}"
                    idx += 1  # meta type
                    vlq = _parse_vlq(data, idx)
                    if vlq is None:
                        return False, f"invalid meta length at track {parsed_tracks}"
                    meta_len, idx = vlq
                    idx += meta_len
                    if idx > chunk_end:
                        return False, f"meta event overruns track {parsed_tracks}"
                    running_status = None
                    continue

                if status in (0xF0, 0xF7):
                    vlq = _parse_vlq(data, idx)
                    if vlq is None:
                        return False, f"invalid sysex length at track {parsed_tracks}"
                    sysex_len, idx = vlq
                    idx += sysex_len
                    if idx > chunk_end:
                        return False, f"sysex overruns track {parsed_tracks}"
                    running_status = None
                    continue

                if not (0x80 <= status <= 0xEF):
                    return False, f"invalid channel status 0x{status:02X} at track {parsed_tracks}"

                data_len = _channel_data_len(status)
                if explicit_status:
                    idx += data_len
                else:
                    idx += data_len
                if idx > chunk_end:
                    return False, f"channel message overruns track {parsed_tracks}"

                running_status = status
                if (status & 0xF0) in (0x90, 0x80):
                    note_events += 1
        offset = chunk_end

    if parsed_tracks != track_count:
        return False, f"declared {track_count} track(s), parsed {parsed_tracks}"

    return True, f"format={format_type} tracks={track_count} note_events={note_events}"


def ffmpeg_supports_midi_input(ffmpeg: str | None) -> bool:
    if not ffmpeg:
        return False
    result = subprocess.run([ffmpeg, "-demuxers"], capture_output=True, text=True)
    if result.returncode != 0:
        return False
    return re.search(r"^\s*D\s+midi(\s|$)", result.stdout, flags=re.MULTILINE) is not None


def validate_mid_file(path: Path, ffmpeg: str | None) -> tuple[bool, str]:
    ok, info = validate_midi_smf(path)
    if not ok:
        return False, info

    if ffmpeg and ffmpeg_supports_midi_input(ffmpeg):
        result = subprocess.run(
            [ffmpeg, "-v", "error", "-i", str(path), "-f", "null", "-"],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            return False, "SMF valid, ffmpeg decode failed"
        return True, f"{info}; ffmpeg decode ok"

    return True, f"{info}; ffmpeg midi demuxer unavailable"


def read_melody_midi_sequence(path: Path) -> list[int]:
    sequence: list[int] = []
    if not path.exists():
        return sequence
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = line.strip().split()
        if len(parts) < 4:
            continue
        # Data rows are: frame frequency_hz confidence midi
        if not parts[0].lstrip("-").isdigit():
            continue
        try:
            midi = int(float(parts[-1]))
        except ValueError:
            continue
        if midi < 0:
            continue
        sequence.append(midi)
    return sequence


def run_length_encode(sequence: list[int], max_items: int | None = None) -> list[tuple[int, int]]:
    runs: list[tuple[int, int]] = []
    if not sequence:
        return runs

    current = sequence[0]
    count = 1
    for midi in sequence[1:]:
        if midi == current:
            count += 1
            continue
        runs.append((current, count))
        current = midi
        count = 1
    runs.append((current, count))

    if max_items is not None:
        return runs[:max_items]
    return runs


def compare_melody_sequences(
    source_melody: Path,
    rendered_melody: Path,
) -> tuple[float, float, float, int, int]:
    source = read_melody_midi_sequence(source_melody)
    rendered = read_melody_midi_sequence(rendered_melody)

    if not source and not rendered:
        return 1.0, 1.0, 1.0, 0, 0

    min_len = min(len(source), len(rendered))
    frame_match = (
        sum(1 for a, b in zip(source[:min_len], rendered[:min_len]) if a == b) / min_len
        if min_len > 0
        else 0.0
    )
    frame_overlap = min_len / max(len(source), len(rendered)) if max(len(source), len(rendered)) > 0 else 0.0
    seq_similarity = difflib.SequenceMatcher(None, source, rendered).ratio()
    return seq_similarity, frame_overlap, frame_match, len(source), len(rendered)


def analyze_file(
    binary: Path,
    input_path: Path,
    out_dir: Path,
    ffmpeg: str | None,
    ffmpeg_midi_ok: bool,
    ffprobe: str | None,
    dry_run: bool,
) -> None:
    base = out_dir / input_path.stem
    melody_path = base.with_suffix(".melody")
    midi_path = base.with_suffix(".mid")

    print(f"extract: {input_path} -> {melody_path.name}")
    run_extract(binary, input_path, melody_path)
    if not melody_path.exists():
        print(f"  extract failed: missing melody output {melody_path}")
        return
    if not midi_path.exists():
        print(f"  extract failed: missing midi output {midi_path}")
        return

    source_seq = read_melody_midi_sequence(melody_path)
    print(f"  source runs (up to 12): {run_length_encode(source_seq)[:12]} (frames={len(source_seq)})")

    mid_ok, mid_info = validate_mid_file(midi_path, ffmpeg=ffmpeg)
    print(f"  mid valid: {mid_ok} ({mid_info})")
    if dry_run or not midi_path.exists() or not mid_ok:
        return
    if not ffmpeg:
        print("  roundtrip skipped: ffmpeg not found")
        return
    if not ffmpeg_midi_ok:
        print("  roundtrip skipped: ffmpeg build has no MIDI input demuxer")
        return

    sample_rate = query_sample_rate(input_path, ffprobe)
    rendered_wav = base.with_name(base.name + ".roundtrip.wav")
    try:
        run_midi_to_wav(ffmpeg, midi_path, rendered_wav, sample_rate=sample_rate)
    except subprocess.CalledProcessError as exc:
        print(f"  roundtrip skipped: midi->wav render failed ({exc.returncode})")
        return

    rendered_melody = base.with_name(base.name + ".roundtrip.melody")
    print(f"  re-extract: {rendered_wav} -> {rendered_melody.name}")
    run_extract(binary, rendered_wav, rendered_melody)

    seq_similarity, frame_overlap, frame_match, source_len, rendered_len = compare_melody_sequences(
        melody_path, rendered_melody
    )
    print(
        "  roundtrip melody match: "
        f"sequence={seq_similarity:.3f} overlap={frame_overlap:.3f} frame={frame_match:.3f} "
        f"source={source_len} rendered={rendered_len}"
    )
    rendered_seq = read_melody_midi_sequence(rendered_melody)
    print(f"  roundtrip runs (up to 12): {run_length_encode(rendered_seq)[:12]}")


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
        help="Generate inputs but do not call extract-sheet",
    )
    parser.add_argument(
        "--input",
        nargs="+",
        type=Path,
        help="Analyze one or more existing audio files for roundtrip comparison",
    )

    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    binary = Path(args.binary)
    if not binary.exists():
        raise FileNotFoundError(f"radioify binary not found: {binary}")

    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    ffmpeg_midi_ok = ffmpeg_supports_midi_input(ffmpeg)
    if ffmpeg is None and not args.dry_run:
        print("ffmpeg not found; roundtrip melody comparison will be skipped.")
    elif ffmpeg and not ffmpeg_midi_ok and not args.dry_run:
        print("ffmpeg found but has no MIDI input demuxer; roundtrip melody comparison will be skipped.")

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

    inputs: list[Path] = [Path(p) for p in args.input] if args.input else []
    created: list[Path] = []

    if not inputs:
        for name, segments in samples.items():
            wav_path = out_dir / f"{name}"
            write_wav(wav_path, segments, sample_rate=args.sr, amplitude=args.amplitude)
            created.append(wav_path.with_suffix(".wav"))
            if args.dry_run:
                continue
            analyze_file(
                binary=binary,
                input_path=wav_path.with_suffix(".wav"),
                out_dir=out_dir,
                ffmpeg=ffmpeg,
                ffmpeg_midi_ok=ffmpeg_midi_ok,
                ffprobe=ffprobe,
                dry_run=args.dry_run,
            )
    else:
        for input_path in inputs:
            if not input_path.exists():
                raise FileNotFoundError(f"input file not found: {input_path}")
            created.append(input_path)
            if args.dry_run:
                continue
            if input_path.suffix.lower() not in {".wav", ".mp3", ".flac", ".ogg", ".m4a"}:
                print(f"  warning: untested extension for input: {input_path.suffix}")
            analyze_file(
                binary=binary,
                input_path=input_path,
                out_dir=out_dir,
                ffmpeg=ffmpeg,
                ffmpeg_midi_ok=ffmpeg_midi_ok,
                ffprobe=ffprobe,
                dry_run=args.dry_run,
            )

    print(f"created {len(created)} input files in {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
