"""Generic download/cache regressions; no model downloads or GPU required."""
import hashlib
import http.server
import json
from pathlib import Path
import tempfile
import threading
import unittest
import zipfile
from unittest.mock import patch

import setup_runtime as setup


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="radioify-download-test-")
        self.root = Path(self.temporary.name)
        self.payload = b"verified generic payload\n" * 4096
        self.requests = []
        self.ignore_range = False
        self.interrupt = False
        self.wrong_range = False
        test = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requested = self.headers.get("Range")
                test.requests.append(requested)
                offset = int(requested.removeprefix("bytes=").removesuffix("-")) if requested else 0
                resumed = bool(offset) and not test.ignore_range
                self.send_response(206 if resumed else 200)
                if resumed:
                    start = offset + 1 if test.wrong_range else offset
                    self.send_header("Content-Range", f"bytes {start}-{len(test.payload) - 1}/{len(test.payload)}")
                else:
                    offset = 0
                self.end_headers()
                data = test.payload[offset:]
                self.wfile.write(data[:1024] if test.interrupt else data)

            def log_message(self, *_):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.spec = {"path": "models/fixture.bin", "bytes": len(self.payload),
                     "sha256": hashlib.sha256(self.payload).hexdigest(),
                     "url": f"http://127.0.0.1:{self.server.server_port}/fixture.bin"}
        self.phases = []

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temporary.cleanup()

    def test_verified_cache_is_reused_without_network(self):
        path = setup.download(self.root, self.spec, self.phases.append)
        self.assertEqual(path.read_bytes(), self.payload)
        with patch.object(setup.urllib.request, "urlopen", side_effect=AssertionError("network used")):
            self.assertEqual(setup.download(self.root, self.spec, self.phases.append), path)
        self.assertIn("Verifying fixture.bin", self.phases)

    def test_progress_publication_survives_temporary_windows_file_lock(self):
        path = self.root / "progress.json"
        path.write_text('{"sequence": 1}', encoding="utf-8")
        replace = setup.os.replace
        calls = 0

        def temporarily_locked(source, destination):
            nonlocal calls
            calls += 1
            if calls <= 2:
                self.assertEqual(json.loads(path.read_text()), {"sequence": 1})
                raise PermissionError("Temporary Windows sharing violation")
            return replace(source, destination)

        with patch.object(setup.os, "replace", side_effect=temporarily_locked):
            setup.write_json(path, {"sequence": 2})
        self.assertEqual(json.loads(path.read_text()), {"sequence": 2})
        self.assertFalse(path.with_suffix(".json.writing").exists())

    def test_interrupted_transfer_resumes_and_verifies(self):
        self.interrupt = True
        with self.assertRaisesRegex(RuntimeError, "resume"):
            setup.download(self.root, self.spec, self.phases.append)
        partial = self.root / "models/fixture.bin.download"
        self.assertEqual(partial.stat().st_size, 1024)
        self.assertFalse((self.root / self.spec["path"]).exists())
        self.interrupt = False
        path = setup.download(self.root, self.spec, self.phases.append)
        self.assertEqual(self.requests, [None, "bytes=1024-"])
        self.assertEqual(path.read_bytes(), self.payload)

    def test_server_ignoring_range_restarts_instead_of_appending(self):
        partial = self.root / "models/fixture.bin.download"
        partial.parent.mkdir()
        partial.write_bytes(self.payload[:1024])
        self.ignore_range = True
        path = setup.download(self.root, self.spec, self.phases.append)
        self.assertEqual(path.read_bytes(), self.payload)

    def test_bad_resumed_range_does_not_corrupt_partial(self):
        partial = self.root / "models/fixture.bin.download"
        partial.parent.mkdir()
        partial.write_bytes(self.payload[:1024])
        self.wrong_range = True
        with self.assertRaisesRegex(RuntimeError, "range"):
            setup.download(self.root, self.spec, self.phases.append)
        self.assertEqual(partial.read_bytes(), self.payload[:1024])

    def test_bad_checksum_preserves_existing_file(self):
        path = self.root / self.spec["path"]
        path.parent.mkdir()
        path.write_bytes(b"previous file")
        self.spec["sha256"] = "0" * 64
        with self.assertRaisesRegex(RuntimeError, "checksum"):
            setup.download(self.root, self.spec, self.phases.append)
        self.assertEqual(path.read_bytes(), b"previous file")
        self.assertFalse(path.with_suffix(".bin.download").exists())

    def test_changed_cached_file_is_repaired(self):
        path = setup.download(self.root, self.spec, self.phases.append)
        path.write_bytes(b"x" * len(self.payload))
        setup.download(self.root, self.spec, self.phases.append)
        self.assertEqual(path.read_bytes(), self.payload)
        self.assertEqual(len(self.requests), 2)

    def test_completed_partial_is_published_without_redownloading(self):
        partial = self.root / "models/fixture.bin.download"
        partial.parent.mkdir()
        partial.write_bytes(self.payload)
        with patch.object(setup.urllib.request, "urlopen", side_effect=AssertionError("network used")):
            setup.download(self.root, self.spec, self.phases.append)
        self.assertFalse(partial.exists())

    def test_native_model_setup_does_not_install_python_dependencies(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        (scripts / "runtime-manifest.json").write_text(json.dumps({"native_whisper": self.spec}))
        with patch.object(setup, "__file__", str(scripts / "setup_runtime.py")), \
             patch.object(setup, "dependencies", side_effect=AssertionError("unneeded dependencies")):
            setup.setup(self.root, self.root, "whisper")
        result = json.loads((self.root / "setup-result.json").read_text())
        self.assertEqual(result["status"], "ok")
        self.assertEqual(Path(result["runtime"]), self.root)

    def test_incomplete_dependencies_are_repaired_before_being_marked_ready(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        (scripts / "requirements.lock").write_text("fixture==1.0\n")
        wheel = self.root / "pip.whl"
        with zipfile.ZipFile(wheel, "w"):
            pass
        with patch.object(setup, "versions_match", return_value=True), \
             patch.object(setup, "download", return_value=wheel), \
             patch.object(setup, "run_command", side_effect=[RuntimeError("interrupted install"), None, None, None]):
            setup.dependencies(self.root, scripts, {"pip": {}}, self.phases.append)
        self.assertIn("Repairing incomplete speech dependencies", self.phases)
        self.assertTrue((self.root / "dependencies.json").is_file())


if __name__ == "__main__":
    unittest.main()
