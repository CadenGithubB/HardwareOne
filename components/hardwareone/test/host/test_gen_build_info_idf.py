#!/usr/bin/env python3
"""BUILD_INFO.md records the ESP-IDF tree an image was built with.

Builds fake build directories and fake ESP-IDF trees in a temporary directory,
runs tools/gen_build_info.py on them and checks the IDF commit, the locally
modified paths and, for esp32p4, the PIE-fix state line. No compiler, SDK or
firmware build is used.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[4]
GEN = REPO / "tools/gen_build_info.py"
PORTASM = "components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S"
# The four instructions check_idf_pie.py recognises as upstream b25cb29.
PATCHED_PORTASM = """\
    esp.vst.128.ip  q3, \\frame, 16
    esp.vld.128.ip  q3, \\frame, 16
    esp.st.u.xacc.ip \\frame, 0
    esp.ld.xacc.ip \\frame, 0
"""
GIT_ENV = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
           "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}


def git(cwd: Path, *args: str) -> str:
    env = dict(os.environ, **GIT_ENV)
    return subprocess.run(("git",) + args, cwd=cwd, env=env, check=True,
                          capture_output=True, text=True).stdout.strip()


def make_idf_checkout(path: Path) -> str:
    """A git checkout with one commit (plumbing only, so no commit hooks run); returns HEAD."""
    (path / PORTASM).parent.mkdir(parents=True)
    (path / PORTASM).write_text("    esp.vst.128.ip  q2, \\frame, 16\n")
    (path / "README.md").write_text("fake ESP-IDF\n")
    git(path, "init", "-q")
    git(path, "add", "-A")
    tree = git(path, "write-tree")
    commit = git(path, "commit-tree", tree, "-m", "v5.5.5")
    git(path, "update-ref", "HEAD", commit)
    git(path, "tag", "v5.5.5", commit)
    return commit


class BuildInfoIdf(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name).resolve()

    def tearDown(self):
        self.tmp.cleanup()

    def build_info(self, idf: Path, target: str, home: str = "") -> str:
        build = self.base / "build-fake"
        build.mkdir(exist_ok=True)
        (build / "sdkconfig").write_text('CONFIG_IDF_TARGET="%s"\n' % target)
        (build / "project_description.json").write_text(json.dumps(
            {"project_name": "hardwareone-idf", "idf_path": str(idf)}))
        env = dict(os.environ, IDF_PATH="", PYTHONDONTWRITEBYTECODE="1")
        if home:
            env["HOME"] = home
        result = subprocess.run([sys.executable, str(GEN), str(build), "fake"], env=env,
                                capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (build / "BUILD_INFO.md").read_text()

    def test_locally_patched_p4_idf_shows_commit_modified_paths_and_pie_state(self):
        idf = self.base / "esp-idf"
        commit = make_idf_checkout(idf)
        (idf / PORTASM).write_text(PATCHED_PORTASM)
        text = self.build_info(idf, "esp32p4")
        self.assertIn("**ESP-IDF:** `v5.5.5` (`%s`) + 1 locally modified path(s)" % commit, text)
        self.assertIn("- `%s`" % PORTASM, text)
        self.assertIn("PIE q3 context-switch fix (upstream b25cb29): present", text)

    def test_idf_under_home_is_shown_relative_to_it(self):
        idf = self.base / "esp-idf"
        make_idf_checkout(idf)
        text = self.build_info(idf, "esp32p4", home=str(self.base))
        self.assertIn("— `~/esp-idf`", text)
        self.assertIn("b25cb29): MISSING from ~/esp-idf", text)
        idf_lines = [line for line in text.splitlines() if "ESP-IDF" in line]
        self.assertEqual(len(idf_lines), 2, text)
        for line in idf_lines:
            self.assertNotIn(str(self.base), line)

    def test_clean_idf_on_a_non_p4_target_has_no_pie_line(self):
        idf = self.base / "esp-idf"
        commit = make_idf_checkout(idf)
        text = self.build_info(idf, "esp32s3")
        self.assertIn("**ESP-IDF:** `v5.5.5` (`%s`) (clean tree)" % commit, text)
        self.assertNotIn("PIE q3", text)

    def test_idf_that_is_not_a_git_checkout_is_reported_as_unknown(self):
        # It sits inside another repository: that repository's HEAD must not be reported.
        outer = self.base / "outer"
        outer.mkdir()
        outer_commit = make_idf_checkout(outer)
        idf = outer / "unpacked-esp-idf"
        (idf / PORTASM).parent.mkdir(parents=True)
        (idf / PORTASM).write_text("    esp.vst.128.ip  q2, \\frame, 16\n")
        text = self.build_info(idf, "esp32p4")
        self.assertIn("**ESP-IDF:** not a git checkout; commit and local edits unknown — `%s`" % idf, text)
        self.assertNotIn(outer_commit, text)
        self.assertIn("PIE q3 context-switch fix (upstream b25cb29): MISSING", text)


if __name__ == "__main__":
    unittest.main()
