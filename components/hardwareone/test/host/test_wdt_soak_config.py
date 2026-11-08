#!/usr/bin/env python3
"""The task-watchdog soak stays opt-in (readiness P1-7).

Ordinary build configurations must not gain a watchdog panic or a changed
timeout; the soak overlay must set the panic, the idle checks and a timeout of
at least MIN_SOAK_TIMEOUT_S; and only the soak switch may layer the overlay,
define HW1_WDT_SOAK for the app, or pick a -wdt-soak build directory.
"""

import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import build_config  # noqa: E402

SOAK = REPO / "config/sdkconfig.wdt_soak.defaults"
OTA = REPO / "config/sdkconfig.ota.defaults"
# The longest bench stall is 10.8 s (P4X-EYE main loop in LittleFS, IDLE0
# starved); the soak timeout must clear it with margin.
MIN_SOAK_TIMEOUT_S = 20
STABLE_ROWS = [("feather_esp32_v2", None), ("qtpy_esp32", None), ("feathers3", None),
               ("xiao_s3", "xiao-no-bluetooth"), ("p4x_eye", "p4-openclaw")]
WDT_KEY = re.compile(r"^(?:#\s*)?(CONFIG_(?:ESP_)?TASK_WDT\w*)(?:=(.*)| is not set)\s*$", re.M)


def wdt_settings(path):
    return {m.group(1): m.group(2) for m in WDT_KEY.finditer(path.read_text(errors="replace"))}


def default_chain_files():
    """Every tracked defaults input an ordinary build can layer."""
    files = sorted((REPO / "config").glob("sdkconfig.defaults*"))
    files += sorted((REPO / "boards").glob("*.defaults"))
    files += sorted((REPO / "presets").glob("*defaults*"))
    files += sorted((REPO / "deployments").rglob("sdkconfig*"))
    files += sorted((REPO / "deployments").rglob("*.defaults"))
    return files


class DefaultsStayUnchanged(unittest.TestCase):
    def test_no_default_input_sets_task_watchdog_keys(self):
        files = default_chain_files()
        self.assertTrue(any(f.name == "sdkconfig.defaults" for f in files))
        self.assertTrue(any(f.name == "p4x_eye.defaults" for f in files))
        for path in files:
            with self.subTest(path=str(path.relative_to(REPO))):
                self.assertEqual(wdt_settings(path), {},
                                 "task watchdog settings belong only in the OTA or soak overlay")

    def test_ota_overlay_is_still_the_only_existing_panic(self):
        # The OTA layout's 5 s panic predates the soak; it must not grow.
        self.assertEqual(wdt_settings(OTA), {"CONFIG_ESP_TASK_WDT_EN": "y",
                                             "CONFIG_ESP_TASK_WDT_INIT": "y",
                                             "CONFIG_ESP_TASK_WDT_PANIC": "y"})

    def test_tracked_sdkconfig_does_not_panic(self):
        settings = wdt_settings(REPO / "config/sdkconfig")
        self.assertIn("CONFIG_ESP_TASK_WDT_PANIC", settings)
        self.assertIsNone(settings["CONFIG_ESP_TASK_WDT_PANIC"], "panic must stay unset")
        self.assertEqual(settings.get("CONFIG_ESP_TASK_WDT_TIMEOUT_S"), "5")

    def test_saved_configurations_do_not_select_watchdog_keys(self):
        for path in sorted((REPO / "configurations").glob("*.json")):
            sdk = json.loads(path.read_text()).get("sdkconfig", {})
            self.assertFalse([k for k in sdk if "TASK_WDT" in k], path.name)
        self.assertFalse([k for k in build_config.sdk_choices() if "TASK_WDT" in k],
                         "the watchdog is not a saved-configuration choice")

    def test_no_overlay_outside_config_mentions_the_soak_file(self):
        hits = [p for p in default_chain_files() if "wdt_soak" in p.read_text(errors="replace")]
        self.assertEqual(hits, [])


class SoakOverlay(unittest.TestCase):
    def test_soak_sets_panic_idle_checks_and_timeout(self):
        s = wdt_settings(SOAK)
        for key in ("CONFIG_ESP_TASK_WDT_EN", "CONFIG_ESP_TASK_WDT_INIT",
                    "CONFIG_ESP_TASK_WDT_PANIC", "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0",
                    "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1"):
            self.assertEqual(s.get(key), "y", key)
        self.assertGreaterEqual(int(s["CONFIG_ESP_TASK_WDT_TIMEOUT_S"]), MIN_SOAK_TIMEOUT_S)
        self.assertLessEqual(int(s["CONFIG_ESP_TASK_WDT_TIMEOUT_S"]), 60)  # Kconfig range

    def test_overlay_holds_only_watchdog_settings(self):
        lines = [l for l in SOAK.read_text().splitlines() if l and not l.startswith("#")]
        self.assertTrue(lines)
        self.assertTrue(all(l.startswith("CONFIG_ESP_TASK_WDT_") for l in lines), lines)


def if_blocks(text):
    """Pair each CMake line with the stack of enclosing if() conditions."""
    stack, out = [], []
    lines = text.splitlines()
    for line in lines:
        code = line.strip()
        if code.startswith("#"):
            out.append(list(stack))
            continue
        if re.match(r"^endif\s*\(", code):
            stack.pop()
        out.append(list(stack))
        m = re.match(r"^if\s*\((.*)", code)
        if m:
            stack.append(m.group(1).rstrip(") ").strip())
    return lines, out


def cpp_blocks(text):
    """Pair each C/C++ line with the stack of enclosing #if conditions."""
    stack, out = [], []
    lines = text.splitlines()
    for line in lines:
        code = line.strip()
        if code.startswith("#endif"):
            stack.pop()
        elif code.startswith(("#else", "#elif")):
            stack[-1] = "!" + stack[-1]
        out.append(list(stack))
        m = re.match(r"^#\s*(if|ifdef|ifndef)\s+(.*)", code)
        if m:
            stack.append(m.group(2).strip())
    return lines, out


class CmakeWiring(unittest.TestCase):
    def setUp(self):
        self.lines, self.conds = if_blocks((REPO / "CMakeLists.txt").read_text())

    def test_switch_defaults_off(self):
        self.assertIn('option(HW1_WDT_SOAK "', "\n".join(self.lines))
        self.assertRegex("\n".join(self.lines), r'option\(HW1_WDT_SOAK "[^"]*" OFF\)')

    def test_overlay_and_definitions_only_under_the_switch(self):
        uses = 0
        for line, conds in zip(self.lines, self.conds):
            code = line.split("#", 1)[0]
            if ("HW1_WDT_SOAK_DEFAULTS" in code and "APPEND" in code) or "HW1_WDT_SOAK=1" in code:
                uses += 1
                self.assertIn("HW1_WDT_SOAK", conds, line)
        self.assertEqual(uses, 4)  # defaults, reconcile, hardwareone, main

    def test_soak_requires_its_own_build_directory(self):
        text = "\n".join(self.lines)
        self.assertIn('MATCHES "-wdt-soak$"', text)


class AppWiring(unittest.TestCase):
    def test_main_subscribes_only_in_soak_builds(self):
        lines, conds = cpp_blocks((REPO / "main/hardwareone-idf.cpp").read_text())
        found = 0
        for line, stack in zip(lines, conds):
            if "taskWdtSubscribeCurrent()" in line or "taskWdtFeed()" in line:
                found += 1
                self.assertIn("HW1_WDT_SOAK", stack, line)
        self.assertEqual(found, 2)

    def test_header_defaults_off(self):
        text = (REPO / "components/hardwareone/System_TaskWdt.h").read_text()
        self.assertIn("#ifndef HW1_WDT_SOAK\n#define HW1_WDT_SOAK 0\n#endif", text)


class WrapperWiring(unittest.TestCase):
    """Run the real tools/build_board.sh against a fake idf.py."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.repo = root / "repo"
        (self.repo / "tools").mkdir(parents=True)
        shutil.copy(REPO / "tools/build_board.sh", self.repo / "tools/build_board.sh")
        shutil.copytree(REPO / "boards", self.repo / "boards")
        (self.repo / "components/arduino").mkdir(parents=True)
        patches = self.repo / "docs/arduino-local-patches"
        patches.mkdir(parents=True)
        (patches / "verify_patches.sh").write_text("exit 0\n")
        self.log = root / "calls.log"
        # Stand-ins record their arguments and the soak switch they saw.
        stub = ('#!/bin/sh\nprintf "%s HW1_WDT_SOAK=%s" "$(basename "$0")" "$HW1_WDT_SOAK" '
                f'>> "{self.log}"\nfor a in "$@"; do printf " %s" "$a" >> "{self.log}"; done\n'
                f'echo >> "{self.log}"\n')
        self.bin = root / "bin"
        self.bin.mkdir()
        for path in (self.bin / "idf.py", self.repo / "tools/build_config.py"):
            path.write_text(stub)
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        (self.bin / "python3").write_text('#!/bin/sh\nexec "$@"\n')
        (self.bin / "python3").chmod(0o755)

    def run_wrapper(self, *args):
        env = dict(os.environ, PATH=f"{self.bin}:{os.environ.get('PATH', '')}")
        env.pop("HW1_WDT_SOAK", None)
        result = subprocess.run(["bash", str(self.repo / "tools/build_board.sh"), *args],
                                capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.log.read_text().splitlines()

    def test_ordinary_board_build_is_unchanged(self):
        for board, _ in STABLE_ROWS:
            with self.subTest(board=board):
                self.log.write_text("")
                calls = self.run_wrapper(board)
                self.assertEqual(len(calls), 1)
                self.assertIn(f"-B {self.repo}/build-{board} ", calls[0])
                self.assertNotIn("WDT_SOAK=ON", calls[0])
                self.assertNotIn("wdt-soak", calls[0])

    def test_soak_on_every_stable_row_uses_its_own_directory(self):
        for board, config in STABLE_ROWS:
            with self.subTest(board=board, config=config):
                self.log.write_text("")
                args = [board] + (["--config", config] if config else []) + ["--wdt-soak"]
                calls = self.run_wrapper(*args)
                name = f"build-config-{config}" if config else f"build-{board}"
                idf = [c for c in calls if c.startswith("idf.py")]
                self.assertTrue(idf)
                for call in idf:
                    self.assertIn(f"-B {self.repo}/{name}-wdt-soak ", call)
                    self.assertIn("-DHW1_WDT_SOAK=ON", call)
                if config:
                    prepare = [c for c in calls if " prepare " in c]
                    self.assertEqual(prepare, [f"build_config.py HW1_WDT_SOAK=1 prepare {config} {board}"])
                    self.assertIn(f"-DHW1_FEATURE_CONFIG_FILE={self.repo}/{name}-wdt-soak/features.h",
                                  idf[0])

    def test_option_order_and_pass_through_actions(self):
        calls = self.run_wrapper("xiao_s3", "--wdt-soak", "--config", "xiao-no-bluetooth", "size")
        self.assertTrue(calls[-1].endswith("-DHW1_WDT_SOAK=ON size"), calls[-1])
        self.log.write_text("")
        calls = self.run_wrapper("xiao_s3", "--config", "xiao-no-bluetooth", "size")
        self.assertTrue(all("HW1_WDT_SOAK=0" in c for c in calls), calls)
        self.assertNotIn("wdt-soak", "\n".join(calls))


class BuildConfigDirectory(unittest.TestCase):
    def test_soak_directory_only_with_the_switch(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop("HW1_WDT_SOAK", None)
            self.assertEqual(build_config.build_dir("p4-openclaw").name, "build-config-p4-openclaw")
            os.environ["HW1_WDT_SOAK"] = "0"
            self.assertEqual(build_config.build_dir("p4-openclaw").name, "build-config-p4-openclaw")
            os.environ["HW1_WDT_SOAK"] = "1"
            self.assertEqual(build_config.build_dir("p4-openclaw").name,
                             "build-config-p4-openclaw-wdt-soak")

    def test_build_command_passes_the_switch_to_the_wrapper(self):
        with patch.object(sys, "argv", ["build_config.py", "build", "p4-openclaw", "--wdt-soak"]), \
                patch.object(build_config.subprocess, "call", return_value=0) as wrapper:
            self.assertEqual(build_config.main(), 0)
        wrapper.assert_called_once_with(["bash", str(REPO / "tools/build_board.sh"), "p4x_eye",
                                         "--config", "p4-openclaw", "--wdt-soak"])


if __name__ == "__main__":
    unittest.main()
