"""Provision the optional speech runtime in the user's persistent cache.

Only the standard library is needed until the pinned dependencies are installed.
The parent owns a Windows job, so cancellation also terminates pip descendants.
"""
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.request
import zipfile


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".writing")
    temporary.write_text(json.dumps(value), encoding="utf-8")
    # Other Windows readers (including older Radioify versions) may briefly
    # deny replacement. Keep publication atomic and bound retries to one second.
    for attempt in range(50):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(.02)


class Progress:
    def __init__(self, workspace):
        self.path = workspace / "setup-progress.json"
        self.sequence = 0

    def __call__(self, phase):
        self.sequence += 1
        write_json(self.path, {"sequence": self.sequence, "phase": phase})


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def verified(path, spec):
    if not path.is_file() or path.stat().st_size != spec["bytes"]:
        return False
    receipt = path.with_suffix(path.suffix + ".verified.json")
    identity = {"sha256": spec["sha256"], "bytes": spec["bytes"],
                "mtime_ns": path.stat().st_mtime_ns}
    try:
        # Recheck small assets even when a fast rewrite retains the filesystem's
        # timestamp. Only multi-GB weights need the metadata receipt fast path.
        if spec["bytes"] > 16 * 1024 * 1024 and json.loads(receipt.read_text()) == identity:
            return True
    except (OSError, ValueError):
        pass
    if digest(path) != spec["sha256"]:
        return False
    write_json(receipt, identity)
    return True


def download(root, spec, progress):
    path = root / spec["path"]
    path.parent.mkdir(parents=True, exist_ok=True)
    progress("Checking " + path.name)
    if verified(path, spec):
        return path
    partial = path.with_suffix(path.suffix + ".download")
    offset = partial.stat().st_size if partial.exists() else 0
    if offset and offset == spec["bytes"] and digest(partial) == spec["sha256"]:
        os.replace(partial, path)
        verified(path, spec)
        return path
    if offset >= spec["bytes"]:
        partial.unlink()
        offset = 0
    headers = {"User-Agent": "Radioify-Speech-Setup/1"}
    if offset:
        headers["Range"] = f"bytes={offset}-"
    request = urllib.request.Request(spec["url"], headers=headers)
    progress("Downloading " + path.name)
    with urllib.request.urlopen(request, timeout=30) as response:
        if response.status == 206:
            if not response.headers.get("Content-Range", "").startswith(f"bytes {offset}-"):
                raise RuntimeError("Invalid resumed download range for " + path.name)
        elif response.status == 200:
            offset = 0  # Servers may ignore Range; replace, never append.
        else:
            raise RuntimeError(f"Unexpected HTTP status {response.status} for {path.name}")
        with partial.open("ab" if offset else "wb") as output:
            last_report = 0
            while block := response.read(1024 * 1024):
                offset += len(block)
                if offset > spec["bytes"]:
                    raise RuntimeError("Download exceeded the pinned size: " + path.name)
                output.write(block)
                if time.monotonic() - last_report >= .5:
                    progress(f"Downloading {path.name}: {offset / 1e9:.2f} / {spec['bytes'] / 1e9:.2f} GB")
                    last_report = time.monotonic()
    progress("Verifying " + path.name)
    if offset != spec["bytes"]:
        raise RuntimeError("Download interrupted; it will resume on the next attempt: " + path.name)
    if digest(partial) != spec["sha256"]:
        partial.unlink()
        raise RuntimeError("Download checksum mismatch: " + path.name)
    os.replace(partial, path)
    verified(path, spec)
    return path


def run_command(arguments, progress, label):
    progress(label)
    started = time.monotonic()
    # Output is inherited by the setup log, so neither pipe can fill up.
    with subprocess.Popen(arguments, stdout=sys.stdout, stderr=sys.stderr,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)) as child:
        while child.poll() is None:
            time.sleep(.25)
            if time.monotonic() - started >= 5:
                progress(label)
                started = time.monotonic()
        if child.returncode:
            raise RuntimeError(f"{label} failed (exit {child.returncode}); see the speech setup log.")


def versions_match(requirements):
    for line in requirements.read_text().splitlines():
        name, version = line.split("==", 1)
        try:
            if importlib.metadata.version(name) != version:
                return False
        except importlib.metadata.PackageNotFoundError:
            return False
    return True


def dependencies(root, scripts, manifest, progress):
    requirements = scripts / "requirements.lock"
    expected = digest(requirements)
    marker = root / "dependencies.json"
    if versions_match(requirements):
        try:
            if json.loads(marker.read_text())["requirements_sha256"] == expected:
                return
        except (OSError, ValueError, KeyError):
            pass
        # An interrupted pip install may have written every dist-info entry
        # without completing the actual files. Validate before adopting it.
        try:
            run_command([sys.executable, "-I", "-B", str(scripts / "verify_runtime.py"), str(requirements)],
                        progress, "Verifying speech dependencies")
            write_json(marker, {"requirements_sha256": expected})
            return
        except RuntimeError:
            progress("Repairing incomplete speech dependencies")
    pip_wheel = download(root, manifest["pip"], progress)
    packages = root / "Lib" / "site-packages"
    packages.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(pip_wheel) as archive:
        archive.extractall(packages)
    pip = [sys.executable, "-I", "-B", "-m", "pip", "--isolated", "install",
           "--disable-pip-version-check", "--no-warn-script-location", "--no-compile",
           "--no-deps", "--upgrade", "--target", str(packages)]
    run_command(pip + ["setuptools==78.1.0"], progress, "Preparing speech dependency installer")
    run_command(pip + ["--no-build-isolation", "--extra-index-url", "https://download.pytorch.org/whl/cu128",
                       "-r", str(requirements)], progress, "Downloading and installing speech dependencies (first use)")
    run_command([sys.executable, "-I", "-B", str(scripts / "verify_runtime.py"), str(requirements)],
                progress, "Verifying speech dependencies")
    write_json(marker, {"requirements_sha256": expected})


def setup(workspace, root, component):
    scripts = Path(__file__).resolve().parent
    manifest = json.loads((scripts / "runtime-manifest.json").read_text())
    progress = Progress(workspace)
    if component == "quality":
        dependencies(root, scripts, manifest, progress)
        for entry in manifest["qwen_files"]:
            download(root, dict(entry, path="models/Qwen3-ASR-1.7B-hf/" + entry["name"],
                                url=manifest["qwen_base_url"] + entry["name"]), progress)
        download(root, manifest["alignment"], progress)
    elif component == "whisper":
        download(root, manifest["native_whisper"], progress)
    else:
        raise ValueError("Unknown speech runtime component")
    write_json(workspace / "setup-result.json", {"version": 1, "status": "ok", "runtime": str(root)})
    progress("Speech runtime ready")


if __name__ == "__main__":
    workspace, root = (Path(arg).resolve(strict=True) for arg in sys.argv[1:3])
    with (workspace / "setup.log").open("a", encoding="utf-8", buffering=1) as log:
        sys.stdout = sys.stderr = log
        try:
            setup(workspace, root, sys.argv[3])
        except Exception as error:
            traceback.print_exc()
            write_json(workspace / "setup-result.json", {"version": 1, "status": "error", "error": str(error)})
            sys.exit(1)
