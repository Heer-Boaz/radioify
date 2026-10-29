"""Validate a seeded/downloaded runtime without requiring GPU availability."""
import importlib.metadata
from pathlib import Path
import sys

for line in Path(sys.argv[1]).read_text().splitlines():
    name, version = line.split("==", 1)
    actual = importlib.metadata.version(name)
    if actual != version:
        raise RuntimeError(f"{name}: expected {version}, found {actual}")

import torch
import torchaudio
import stable_whisper
from transformers import AutoModelForMultimodalLM, AutoProcessor

if torch.version.cuda != "12.8":
    raise RuntimeError(f"Expected CUDA 12.8 runtime, got {torch.version.cuda}")
print("Pinned transcript dependencies verified; CUDA", torch.version.cuda)
