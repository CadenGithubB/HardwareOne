"""Offline preparation safety checks; synthetic files only, no firmware build."""
from pathlib import Path
import importlib.util
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("roles_prepare_idf_bt", ROOT / "prepare_idf_bt.py")
prepare_bt = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare_bt)


class PrepareBluetoothTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hw1-bt-prepare-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.sdk = self.root / "sdk"
        self.component = self.sdk / "components/bt"
        self.component.mkdir(parents=True)
        (self.component / "core.c").write_text("int answer = 0;\n")
        (self.component / "unchanged.h").write_text("#define VALUE 7\n")
        (self.component / "alias.h").symlink_to("unchanged.h")
        (self.component / ".git").write_text("private SDK metadata\n")
        version = self.sdk / "tools/cmake/version.cmake"
        version.parent.mkdir(parents=True)
        version.write_text("set(IDF_VERSION_MAJOR 5)\nset(IDF_VERSION_MINOR 5)\nset(IDF_VERSION_PATCH 5)\n")
        self.patch = self.root / "fix.patch"
        self.patch.write_text(
            "--- a/components/bt/core.c\n+++ b/components/bt/core.c\n"
            "@@ -1 +1 @@\n-int answer = 0;\n+int answer = 42;\n")
        links = {"alias.h": "unchanged.h"}
        files, executable = prepare_bt.scan_tree(self.component, links, True)
        self.manifest = {
            "schema": 1, "idf_version": "5.5.5",
            "version_file": {"path": "tools/cmake/version.cmake",
                             "sha256": prepare_bt.digest(version.read_bytes())},
            "baseline": {"files": files, "executable_files": executable,
                         "source_symlinks": links},
            "patched_files": {"core.c": prepare_bt.digest(b"int answer = 42;\n")},
            "patch": {"file": "fix.patch", "sha256": prepare_bt.digest(self.patch.read_bytes()), "strip": 3},
        }
        self.output = self.root / "private/idf-components/bt"

    def prepare(self):
        return prepare_bt.prepare(self.sdk, self.output, self.manifest, self.root)

    def test_clean_copy_materializes_link_and_omits_git(self):
        self.assertEqual(self.prepare(), (3, True))
        self.assertEqual((self.output / "core.c").read_text(), "int answer = 42;\n")
        self.assertEqual((self.component / "core.c").read_text(), "int answer = 0;\n")
        self.assertFalse((self.output / "alias.h").is_symlink())
        self.assertFalse((self.output / ".git").exists())
        self.assertEqual(prepare_bt.check_output(self.output, self.manifest), 3)

    def test_idempotent_rerun_does_not_rewrite_files(self):
        self.prepare()
        path = self.output / "core.c"
        before = (path.stat().st_ino, path.stat().st_mtime_ns)
        self.assertEqual(self.prepare(), (3, False))
        self.assertEqual((path.stat().st_ino, path.stat().st_mtime_ns), before)

    def test_sdk_unrelated_content_change_is_rejected(self):
        (self.component / "unchanged.h").write_text("tampered\n")
        with self.assertRaisesRegex(ValueError, "content mismatch"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_sdk_extra_file_is_rejected(self):
        (self.component / "diagnostic.c").write_text("diagnostics\n")
        with self.assertRaisesRegex(ValueError, "content mismatch"):
            self.prepare()

    def test_sdk_version_change_is_rejected(self):
        (self.sdk / "tools/cmake/version.cmake").write_text("set(IDF_VERSION_PATCH 6)\n")
        with self.assertRaisesRegex(ValueError, "version file"):
            self.prepare()

    def test_existing_unknown_directory_is_not_overwritten(self):
        self.output.mkdir(parents=True)
        (self.output / "keep.c").write_text("user data\n")
        with self.assertRaisesRegex(ValueError, "content mismatch"):
            self.prepare()
        self.assertEqual((self.output / "keep.c").read_text(), "user data\n")

    def test_existing_patched_file_tamper_is_not_repaired(self):
        self.prepare()
        (self.output / "core.c").write_text("user change\n")
        with self.assertRaisesRegex(ValueError, "content mismatch"):
            self.prepare()
        self.assertEqual((self.output / "core.c").read_text(), "user change\n")

    def test_existing_extra_file_is_rejected(self):
        self.prepare()
        (self.output / "extra.c").write_text("unrecorded\n")
        with self.assertRaisesRegex(ValueError, "content mismatch"):
            prepare_bt.check_output(self.output, self.manifest)

    def test_existing_file_symlink_is_rejected(self):
        self.prepare()
        target = self.output / "alias.h"
        target.unlink()
        target.symlink_to("unchanged.h")
        with self.assertRaisesRegex(ValueError, "symlink"):
            prepare_bt.check_output(self.output, self.manifest)

    def test_output_parent_symlink_is_rejected(self):
        elsewhere = self.root / "elsewhere"
        elsewhere.mkdir()
        self.output.parent.parent.mkdir(parents=True)
        self.output.parent.symlink_to(elsewhere, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink in destination"):
            self.prepare()
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_escaping_recorded_source_link_is_rejected(self):
        (self.component.parent / "outside.h").write_text("outside\n")
        link = self.component / "alias.h"
        link.unlink()
        link.symlink_to("../outside.h")
        self.manifest["baseline"]["source_symlinks"]["alias.h"] = "../outside.h"
        with self.assertRaisesRegex(ValueError, "escapes component"):
            self.prepare()

    def test_patch_tamper_cleans_stage_and_publishes_nothing(self):
        self.patch.write_text("unexpected patch\n")
        with self.assertRaisesRegex(ValueError, "patch changed"):
            self.prepare()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.output.parent.glob(".bt-prepare-*")), [])

    def test_patch_offset_is_rejected_even_with_correct_expected_content(self):
        (self.component / "core.c").write_text("// shifted\nint answer = 0;\n")
        self.manifest["baseline"]["files"]["core.c"] = prepare_bt.digest((self.component / "core.c").read_bytes())
        self.manifest["patched_files"]["core.c"] = prepare_bt.digest(b"// shifted\nint answer = 42;\n")
        with self.assertRaisesRegex(ValueError, "exactly at recorded positions"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_output_executable_mode_change_is_rejected(self):
        self.prepare()
        (self.output / "core.c").chmod(0o755)
        with self.assertRaisesRegex(ValueError, "executable file modes"):
            prepare_bt.check_output(self.output, self.manifest)


if __name__ == "__main__":
    unittest.main()
