"""Offline reproduction checks using disposable, small source fixtures."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("connectivity_prepare", Path(__file__).with_name("prepare.py"))
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class ReproductionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.artifacts = self.root / "artifacts"
        self.baseline = self.root / "baseline"
        self.destination = self.root / "output"
        self.artifacts.mkdir()
        self.baseline.mkdir()
        self.original_root = prepare.ROOT
        prepare.ROOT = self.artifacts
        self.addCleanup(setattr, prepare, "ROOT", self.original_root)
        self.before = {"main.cpp": b"before\n", "unchanged.h": b"untouched\n", "removed.h": b"old\n"}
        self.after = {"main.cpp": b"after without newline", "unchanged.h": b"untouched\n", "new.h": b"added\n"}
        for name, data in self.before.items():
            (self.baseline / name).write_bytes(data)
        changes = {name: prepare.digest(data) for name, data in self.after.items() if self.before.get(name) != data}
        changes["removed.h"] = None
        app_patch = "".join(prepare.unified_patch(self.before.get(name), self.after.get(name), name)
                            for name in sorted(changes)).encode()
        patches = {"app-overlay.patch": app_patch, "arduino-overlay.patch": b""}
        for filename, data in patches.items():
            (self.artifacts / filename).write_bytes(data)
        self.manifest = {"schema": 1, "baseline": {name: prepare.digest(data) for name, data in self.before.items()},
                         "changes": changes, "patches": {name: prepare.digest(data) for name, data in patches.items()}}
        (self.artifacts / "source-manifest.json").write_text(json.dumps(self.manifest))

    def test_exact_roundtrip_add_remove_and_no_final_newline(self):
        count = prepare.prepare(self.baseline, self.destination, prepare.load_manifest())
        self.assertEqual(count, len(self.after))
        self.assertEqual({p.name: p.read_bytes() for p in self.destination.iterdir()}, self.after)
        self.assertEqual({p.name: p.read_bytes() for p in self.baseline.iterdir()}, self.before)

    def test_existing_destination_is_never_modified(self):
        self.destination.mkdir()
        marker = self.destination / "user-work"
        marker.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "already exists"):
            prepare.prepare(self.baseline, self.destination, self.manifest)
        self.assertEqual(marker.read_bytes(), b"keep")

    def test_modified_baseline_rejected_before_copy(self):
        (self.baseline / "main.cpp").write_bytes(b"other work")
        with self.assertRaisesRegex(ValueError, "baseline mismatch"):
            prepare.prepare(self.baseline, self.destination, self.manifest)
        self.assertFalse(self.destination.exists())

    def test_patch_tampering_rejected(self):
        (self.artifacts / "app-overlay.patch").write_text("changed patch")
        with self.assertRaisesRegex(ValueError, "patch hash mismatch"):
            prepare.load_manifest()

    def test_check_detects_drift_and_preserves_generated_files(self):
        prepare.prepare(self.baseline, self.destination, self.manifest)
        marker = self.destination / "generated-cache"
        marker.write_bytes(b"keep cache")
        prepare.check_destination(self.destination, self.manifest)
        (self.destination / "main.cpp").write_bytes(b"local edit")
        with self.assertRaisesRegex(ValueError, "prepared source mismatch"):
            prepare.check_destination(self.destination, self.manifest)
        self.assertEqual(marker.read_bytes(), b"keep cache")

    def test_paths_cannot_escape_source_tree(self):
        for relative in ("../outside", "/absolute", "foo/../../outside"):
            with self.subTest(relative=relative), self.assertRaises(ValueError):
                prepare.safe_path(self.baseline, relative)


if __name__ == "__main__":
    unittest.main()
