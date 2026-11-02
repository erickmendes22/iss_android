"""Reject stale ABI inputs and corrupted recovered dependency bytes."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "bloodframe_bootstrap", ROOT / "tools/android/bionic/stage_bloodframe_bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class BloodframeBootstrapTests(unittest.TestCase):
    def test_current_api_and_host_sync_match_cached_binary(self):
        manifest = json.loads(bootstrap.MANIFEST.read_text())
        bootstrap.check_inputs(manifest, manifest["bloodframe_commit"])

    def test_changed_pin_or_api_rejects_cached_binary(self):
        manifest = json.loads(bootstrap.MANIFEST.read_text())
        with self.assertRaisesRegex(RuntimeError, "pin changed"):
            bootstrap.check_inputs(manifest, "different")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in manifest["compatibility_inputs"]:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("changed API or host synchronization")
            with self.assertRaisesRegex(RuntimeError, "SHA256 mismatch"):
                bootstrap.check_inputs(manifest, manifest["bloodframe_commit"], root)

    def test_corrupt_artifact_rejected_before_archive_parsing(self):
        manifest = json.loads(bootstrap.MANIFEST.read_text())
        with self.assertRaisesRegex(RuntimeError, "Actions artifact SHA256 mismatch"):
            bootstrap.payloads(b"corrupted or unrelated artifact", manifest)

    def test_signed_blob_redirect_drops_github_authorization(self):
        request = urllib.request.Request("https://api.github.com/artifact", headers={
            "Authorization": "Bearer test-token"})
        redirected = bootstrap.ArtifactRedirect().redirect_request(
            request, None, 302, "Found", {}, "https://blob.example/artifact?signature=test")
        self.assertFalse(redirected.has_header("Authorization"))


if __name__ == "__main__":
    unittest.main()
