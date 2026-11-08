#!/usr/bin/env python3
"""Host test for the P4 SDK bootstrap: fetch on a fresh machine, IDF PIE-fix report.

Runs the scripts from a copy of tools/p4 in a temporary fake checkout, with a
fake ESP-IDF tree and stand-in registry packages; nothing is downloaded, no
firmware is configured or built, and the real IDF and component cache are never
read.
"""
from __future__ import annotations

import importlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
PATCH = HERE / "idf-patches/0001-freertos-fix-xesppie-registers-save-restore.patch"
PORTASM = "components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S"
MODULES = ("prepare_esp_dl", "prepare_esp_hosted", "fetch_p4_packages", "check_idf_pie")


def portasm_versions() -> tuple[str, str]:
    """(before, after) portasm.S hunk text taken from the tracked upstream patch."""
    before, after, inside = [], [], False
    for line in PATCH.read_text().splitlines():
        if line.startswith("diff --git"):
            inside = line.endswith("portasm.S")
            continue
        if not inside or line.startswith(("---", "+++", "index ", "\\")):
            continue
        if line.startswith("@@"):
            before.append("")
            after.append("")
        elif line.startswith("-"):
            before.append(line[1:])
        elif line.startswith("+"):
            after.append(line[1:])
        else:
            before.append(line[1:])
            after.append(line[1:])
    return "\n".join(before) + "\n", "\n".join(after) + "\n"


class Checkout:
    """A fake repository holding a copy of tools/p4, plus a fake ESP-IDF tree."""

    def __init__(self, base: Path):
        self.repo = base / "repo"
        self.p4 = self.repo / "tools/p4"
        shutil.copytree(HERE, self.p4, ignore=shutil.ignore_patterns("companion", "__pycache__", "*.pyc"))
        self.idf = base / "idf"
        (self.idf / "tools").mkdir(parents=True)
        (self.idf / "tools/idf.py").write_text("raise SystemExit('the fake idf.py must not run')\n")

    def write_portasm(self, text: str) -> None:
        path = self.idf / PORTASM
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def load(self):
        """Import the copied scripts so their REPO is the fake checkout; isolate the cache."""
        for name in MODULES:
            sys.modules.pop(name, None)
        sys.path.insert(0, str(self.p4))
        try:
            modules = {name: importlib.import_module(name) for name in MODULES}
        finally:
            sys.path.remove(str(self.p4))
        for name in ("prepare_esp_dl", "prepare_esp_hosted"):
            modules[name].cache_roots = lambda: []
        return modules


def fake_package(path: Path, name: str) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    (path / "idf_component.yml").write_text(f"description: stand-in for {name}\n")
    (path / "CMakeLists.txt").write_text("idf_component_register()\n")
    (path / ".component_hash").write_text("ignored by the tree digest\n")
    return path


class Result:
    def __init__(self, returncode: int):
        self.returncode = returncode


class FreshMachineFetch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.co = Checkout(Path(self.tmp.name).resolve())
        self.m = self.co.load()
        self.fetch = self.m["fetch_p4_packages"]
        self.packages = (self.m["prepare_esp_dl"], self.m["prepare_esp_hosted"])
        # The pinned digest of each stand-in package, as the registry's would be.
        sample = Path(self.tmp.name) / "sample"
        for package in self.packages:
            package.SOURCE_TREE_SHA256 = package.tree_digest(fake_package(sample / package.NAME, package.NAME))
        self.calls = []

    def tearDown(self):
        for name in MODULES:
            sys.modules.pop(name, None)
        self.tmp.cleanup()

    def registry(self, returncode=0, corrupt=False, downloaded=None):
        def run(command):
            self.calls.append(command)
            work = Path(command[command.index("-C") + 1])
            if (returncode == 0) if downloaded is None else downloaded:
                for package in self.packages:
                    tree = fake_package(work / "managed_components" / package.NAME, package.NAME)
                    if corrupt:
                        (tree / "extra.c").write_text("not in the pinned package\n")
            return Result(returncode)
        return run

    def test_fresh_machine_needs_both_and_finds_none(self):
        self.assertEqual([p.NAME for p in self.fetch.needed()],
                         ["espressif__esp-dl", "espressif__esp_hosted"])
        with self.assertRaisesRegex(ValueError, "prepare_sdk.sh fetches it from the ESP Component Registry"):
            self.packages[0].find_source(None)

    def test_fetch_runs_update_dependencies_on_exact_pins_and_prepare_finds_the_result(self):
        sources = self.fetch.fetch(self.fetch.needed(), self.co.idf, run=self.registry())
        self.assertEqual(len(self.calls), 1)
        command = self.calls[0]
        work = self.co.repo / ".sdk-overrides/registry-fetch"
        self.assertEqual(command[1:], [str(self.co.idf / "tools/idf.py"), "-C", str(work), "-B",
                                       str(work / "build"), "-DIDF_TARGET=esp32p4", "update-dependencies"])
        manifest = (work / "main/idf_component.yml").read_text()
        self.assertIn('espressif/esp-dl:\n    version: "==3.3.12"', manifest)
        self.assertIn('espressif/esp_hosted:\n    version: "==2.12.13"', manifest)
        self.assertIn("set(COMPONENTS main)", (work / "CMakeLists.txt").read_text())
        # prepare_esp_dl/prepare_esp_hosted pick the fetched trees up without --source.
        for package, source in zip(self.packages, sources):
            self.assertEqual(package.find_source(None), source)
        self.assertEqual(self.fetch.needed(), [])

    def test_nothing_fetched_when_staged_or_available_locally(self):
        dl, hosted = self.packages
        fake_package(self.co.repo / "managed_components" / dl.NAME, dl.NAME)
        (self.co.repo / ".sdk-overrides/esp32p4" / hosted.NAME).mkdir(parents=True)
        self.assertEqual(self.fetch.needed(), [])
        self.assertEqual(self.fetch.fetch(self.fetch.needed(), self.co.idf, run=self.registry()), [])
        self.assertEqual(self.calls, [])

    def test_only_the_missing_package_is_fetched(self):
        dl, hosted = self.packages
        fake_package(self.co.repo / "managed_components" / dl.NAME, dl.NAME)
        self.assertEqual(self.fetch.needed(), [hosted])
        self.fetch.fetch(self.fetch.needed(), self.co.idf, run=self.registry())
        manifest = (self.co.repo / ".sdk-overrides/registry-fetch/main/idf_component.yml").read_text()
        self.assertNotIn("esp-dl", manifest)
        self.assertIn("espressif/esp_hosted", manifest)

    def test_registry_failure_names_the_offline_remedy(self):
        with self.assertRaises(ValueError) as caught:
            self.fetch.fetch(self.fetch.needed(), self.co.idf, run=self.registry(returncode=2))
        message = str(caught.exception)
        for expected in ("exited 2", "components.espressif.com", "rerun tools/p4/prepare_sdk.sh",
                         "tools/p4/prepare_esp_dl.py --source", "tools/p4/prepare_esp_hosted.py --source"):
            self.assertIn(expected, message)

    def test_configure_failure_after_a_verified_download_is_not_a_registry_failure(self):
        # update-dependencies downloads, then reconfigures the throwaway project;
        # if only that configure fails, the verified packages are still used.
        sources = self.fetch.fetch(self.fetch.needed(), self.co.idf,
                                   run=self.registry(returncode=1, downloaded=True))
        self.assertEqual([source.name for source in sources], [p.NAME for p in self.packages])
        self.assertEqual(self.fetch.needed(), [])

    def test_configure_failure_with_a_bad_download_still_fails(self):
        with self.assertRaisesRegex(ValueError, "exited 1"):
            self.fetch.fetch(self.fetch.needed(), self.co.idf,
                             run=self.registry(returncode=1, downloaded=True, corrupt=True))

    def test_fetched_tree_must_match_the_pinned_digest(self):
        with self.assertRaisesRegex(ValueError, "does not match the pinned source digest"):
            self.fetch.fetch(self.fetch.needed(), self.co.idf, run=self.registry(corrupt=True))

    def test_unexported_idf_is_reported_before_anything_runs(self):
        shutil.rmtree(self.co.idf / "tools")
        with self.assertRaisesRegex(ValueError, "export ESP-IDF 5.5.5"):
            self.fetch.fetch(self.fetch.needed(), self.co.idf, run=self.registry())
        self.assertEqual(self.calls, [])


class IdfPieFix(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.co = Checkout(Path(self.tmp.name).resolve())
        self.check = self.co.load()["check_idf_pie"]
        self.before, self.after = portasm_versions()

    def tearDown(self):
        for name in MODULES:
            sys.modules.pop(name, None)
        self.tmp.cleanup()

    def test_patched_tree_is_present(self):
        self.co.write_portasm(self.after)
        state, line = self.check.pie_state(self.co.idf)
        self.assertEqual(state, "present", line)

    def test_stock_5_5_5_tree_is_missing_with_the_apply_command(self):
        self.co.write_portasm(self.before)
        state, line = self.check.pie_state(self.co.idf)
        self.assertEqual(state, "missing", line)
        self.assertIn(f"git -C {self.co.idf} apply", line)
        self.assertIn("idf-patches/0001-freertos-fix-xesppie-registers-save-restore.patch", line)
        self.assertIn("ENABLE_STT_SPEAKER_LABELS", line)

    def test_q3_save_alone_is_partial(self):
        # The CMake gate only looks for the q3 save; a half-applied fix must not pass here.
        restore = [line for line in self.after.splitlines() if "esp.vld.128.ip  q3" in line]
        self.assertEqual(len(restore), 1)
        self.co.write_portasm(self.after.replace(restore[0] + "\n", ""))
        state, line = self.check.pie_state(self.co.idf)
        self.assertEqual(state, "partial", line)
        self.assertIn("q3 restore", line)
        self.assertIn("checkout --", line)

    def test_tree_without_the_riscv_port_is_unknown(self):
        state, line = self.check.pie_state(self.co.idf)
        self.assertEqual(state, "unknown", line)
        self.assertIn("cannot check", line)


class PrepareSdkCheck(unittest.TestCase):
    """prepare_sdk.sh --check runs every check and always reports the PIE state."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.co = Checkout(Path(self.tmp.name).resolve())
        self.before, self.after = portasm_versions()

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self):
        env = dict(os.environ, IDF_PATH=str(self.co.idf), PYTHONDONTWRITEBYTECODE="1")
        python_dir = str(Path(sys.executable).parent)
        env["PATH"] = python_dir + os.pathsep + env.get("PATH", "")
        return subprocess.run(["bash", str(self.co.p4 / "prepare_sdk.sh"), "--check"], env=env,
                              capture_output=True, text=True, timeout=120)

    def test_unprepared_checkout_reports_all_four_and_the_missing_fix(self):
        self.co.write_portasm(self.before)
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for expected in ("prepare_idf_bt:", "JPEG override:", "ESP-DL override:", "ESP-Hosted override:",
                         "overrides missing or changed"):
            self.assertIn(expected, result.stderr)
        self.assertIn("PIE q3 context-switch fix (upstream b25cb29): MISSING", result.stdout)
        self.assertIn("apply", result.stdout)

    def test_patched_idf_is_reported_present(self):
        self.co.write_portasm(self.after)
        result = self.run_check()
        self.assertIn("PIE q3 context-switch fix (upstream b25cb29): present", result.stdout,
                      result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
