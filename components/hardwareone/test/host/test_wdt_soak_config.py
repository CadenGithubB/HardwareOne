#!/usr/bin/env python3
"""The task-watchdog soak stays opt-in.

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
import gen_build_info  # noqa: E402

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
        elif re.match(r"^(?:else|elseif)\s*\(", code):
            # Lines after else()/elseif() run when the if() was false.
            stack[-1] = "!" + stack[-1]
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
            # The header always defines HW1_WDT_SOAK (0 when off), so only a
            # value test (#if) selects soak builds; #ifdef would be always true.
            kind, cond = m.group(1), m.group(2).strip()
            stack.append(cond if kind == "if" else f"{kind} {cond}")
    return lines, out


class CmakeWiring(unittest.TestCase):
    def setUp(self):
        self.lines, self.conds = if_blocks((REPO / "CMakeLists.txt").read_text())

    def test_switch_defaults_off(self):
        self.assertIn('option(HW1_WDT_SOAK "', "\n".join(self.lines))
        self.assertRegex("\n".join(self.lines), r'option\(HW1_WDT_SOAK "[^"]*" OFF\)')

    def test_overlay_and_definitions_only_under_the_switch(self):
        uses = []
        for line, conds in zip(self.lines, self.conds):
            code = line.split("#", 1)[0]
            if (("HW1_WDT_SOAK_DEFAULTS" in code and "APPEND" in code) or "HW1_WDT_SOAK=1" in code
                    or "hw1_wdt_soak_check_" in code):
                uses.append(code.strip())
                self.assertIn("HW1_WDT_SOAK", conds, line)
                self.assertNotIn("!HW1_WDT_SOAK", conds, line)
        # defaults, reconcile, the two guards, hardwareone and main.
        self.assertEqual(len(uses), 6, uses)

    def test_both_components_get_the_definition(self):
        text = "\n".join(l.split("#", 1)[0] for l in self.lines)
        self.assertEqual(text.count(
            "idf_component_get_property(_hw1_app_component hardwareone COMPONENT_LIB)"), 1)
        self.assertEqual(text.count(
            "idf_component_get_property(_hw1_main_component main COMPONENT_LIB)"), 1)
        for target in ("${_hw1_app_component}", "${_hw1_main_component}"):
            self.assertEqual(text.count(
                f"target_compile_definitions({target} PRIVATE HW1_WDT_SOAK=1)"), 1, target)

    def test_guards_are_called_with_the_real_inputs(self):
        text = "\n".join(self.lines)
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HW1WdtSoak.cmake")', text)
        self.assertIn('hw1_wdt_soak_check_build_dir("${CMAKE_BINARY_DIR}" "${_sdkconfig}" '
                      '"${HW1_WDT_SOAK_DEFAULTS}")', text)
        self.assertRegex(text, r'hw1_wdt_soak_check_resolved\("\$\{HW1_WDT_SOAK_DEFAULTS\}"\s+'
                         r'"\$\{CONFIG_ESP_TASK_WDT_PANIC\}" "\$\{CONFIG_ESP_TASK_WDT_TIMEOUT_S\}"\)')
        # The build-directory guard runs before any defaults are layered, and the
        # resolved check after project() and before the app gets HW1_WDT_SOAK=1.
        dir_guard = text.index("hw1_wdt_soak_check_build_dir(")
        resolved = text.index("hw1_wdt_soak_check_resolved(")
        self.assertLess(dir_guard, text.index("set(SDKCONFIG_DEFAULTS"))
        self.assertLess(text.index("include($ENV{IDF_PATH}/tools/cmake/project.cmake)"), resolved)
        self.assertLess(resolved, text.index("PRIVATE HW1_WDT_SOAK=1)"))

    def test_overlay_is_layered_last(self):
        code = [l.split("#", 1)[0].strip() for l in self.lines]
        touches = [i for i, l in enumerate(code)
                   if re.match(r"^(?:set\(|list\((?:APPEND|PREPEND|INSERT|REMOVE_\w+) )SDKCONFIG_DEFAULTS\b", l)]
        soak = [i for i in touches if "HW1_WDT_SOAK_DEFAULTS" in code[i]]
        self.assertEqual(len(soak), 1)
        self.assertGreater(len(touches), 3)
        self.assertEqual(touches[-1], soak[0], "nothing may layer after the soak overlay")
        recon = [i for i, l in enumerate(code) if "_sdk_reconcile_files" in l]
        call = [i for i in recon if code[i].startswith("hw1_reconcile_sdkconfig(")]
        self.assertEqual(len(call), 1)
        before = [i for i in recon if i < call[0] and "APPEND" in code[i]]
        self.assertIn("HW1_WDT_SOAK_DEFAULTS", code[before[-1]])


@unittest.skipUnless(shutil.which("cmake"), "cmake is required")
class CmakeGuards(unittest.TestCase):
    """Run the real guards in cmake/HW1WdtSoak.cmake with cmake -P."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.soak_dir = self.root / "build-feathers3-wdt-soak"
        self.soak_dir.mkdir()

    def run_cmake(self, call):
        script = self.root / "check.cmake"
        script.write_text('cmake_minimum_required(VERSION 3.16)\n'
                          f'include("{REPO / "cmake/HW1WdtSoak.cmake"}")\n{call}\n')
        result = subprocess.run([shutil.which("cmake"), "-P", str(script)],
                                capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def check_dir(self, binary_dir, sdkconfig, defaults=SOAK):
        return self.run_cmake(f'hw1_wdt_soak_check_build_dir("{binary_dir}" "{sdkconfig}" "{defaults}")')

    def check_resolved(self, panic, timeout, defaults=SOAK):
        return self.run_cmake(f'hw1_wdt_soak_check_resolved("{defaults}" "{panic}" "{timeout}")')

    def test_soak_directory_with_its_own_sdkconfig_passes(self):
        code, out = self.check_dir(self.soak_dir, self.soak_dir / "sdkconfig")
        self.assertEqual(code, 0, out)

    def test_ordinary_directory_is_refused(self):
        plain = self.root / "build-feathers3"
        plain.mkdir()
        code, out = self.check_dir(plain, plain / "sdkconfig")
        self.assertNotEqual(code, 0)
        self.assertIn("name ending in -wdt-soak", out)
        code, out = self.check_dir(self.root / "build-wdt-soak-feathers3",
                                   self.root / "build-wdt-soak-feathers3/sdkconfig")
        self.assertNotEqual(code, 0, out)

    def test_shared_sdkconfig_is_refused(self):
        code, out = self.check_dir(self.soak_dir, REPO / "config/sdkconfig")
        self.assertNotEqual(code, 0)
        self.assertIn("SDKCONFIG inside its build directory", out)
        nested = self.soak_dir / "config"
        nested.mkdir()
        code, out = self.check_dir(self.soak_dir, nested / "sdkconfig")
        self.assertNotEqual(code, 0, out)

    def test_missing_overlay_is_refused(self):
        code, out = self.check_dir(self.soak_dir, self.soak_dir / "sdkconfig",
                                   defaults=self.root / "missing.defaults")
        self.assertNotEqual(code, 0)
        self.assertIn("Missing watchdog soak overlay", out)

    def test_resolved_values_must_match_the_overlay(self):
        timeout = wdt_settings(SOAK)["CONFIG_ESP_TASK_WDT_TIMEOUT_S"]
        code, out = self.check_resolved("y", timeout)
        self.assertEqual(code, 0, out)
        for panic, value in (("", timeout), ("n", timeout), ("y", "5"), ("y", "")):
            with self.subTest(panic=panic, timeout=value):
                code, out = self.check_resolved(panic, value)
                self.assertNotEqual(code, 0, out)
                self.assertIn("Watchdog soak build resolved", out)

    def test_overlay_without_a_timeout_is_refused(self):
        bare = self.root / "bare.defaults"
        bare.write_text("CONFIG_ESP_TASK_WDT_PANIC=y\n")
        code, out = self.check_resolved("y", "20", defaults=bare)
        self.assertNotEqual(code, 0)
        self.assertIn("sets no timeout", out)


class AppWiring(unittest.TestCase):
    def test_main_subscribes_only_in_soak_builds(self):
        lines, conds = cpp_blocks((REPO / "main/hardwareone-idf.cpp").read_text())
        found = 0
        for line, stack in zip(lines, conds):
            if "taskWdtSubscribeCurrent()" in line or "taskWdtFeed()" in line:
                found += 1
                self.assertIn("HW1_WDT_SOAK", stack, line)
                self.assertNotIn("!HW1_WDT_SOAK", stack, line)
        self.assertEqual(found, 2)

    def test_main_subscribes_after_setup_and_feeds_each_pass(self):
        lines = [l.split("//", 1)[0].strip()
                 for l in (REPO / "main/hardwareone-idf.cpp").read_text().splitlines()]
        start = [i for i, l in enumerate(lines) if l.startswith('extern "C" void app_main(')]
        self.assertEqual(len(start), 1)
        def index(text):
            hits = [i for i, l in enumerate(lines) if i > start[0] and text in l and
                    (l == text or not text.endswith(";"))]
            self.assertEqual(len(hits), 1, text)
            return hits[0]
        setup, subscribe = index("setup();"), index("taskWdtSubscribeCurrent()")
        loop_start, call, feed = index("while (true)"), index("loop();"), index("taskWdtFeed()")
        self.assertLess(setup, subscribe, "setup() must not run subscribed")
        self.assertLess(subscribe, loop_start)
        self.assertLess(loop_start, call)
        self.assertLess(call, feed)

    def test_command_wait_feeds_a_subscribed_caller(self):
        # Serial, UART-link and OLED commands all wait here on the main loop.
        text = (REPO / "components/hardwareone/System_Utils.cpp").read_text()
        body = text[text.index("bool submitAndExecuteSync("):]
        body = body[:body.index("\n}\n")]
        self.assertIn('#include "System_TaskWdt.h"', text)
        self.assertEqual(body.count("taskWdtAwareSemaphoreTake(r->done, 60000)"), 1)
        self.assertNotIn("xSemaphoreTake(r->done", body)

    def test_header_defaults_off(self):
        text = (REPO / "components/hardwareone/System_TaskWdt.h").read_text()
        self.assertIn("#ifndef HW1_WDT_SOAK\n#define HW1_WDT_SOAK 0\n#endif", text)


class ReservedSuffix(unittest.TestCase):
    def test_configuration_names_may_not_end_in_the_soak_suffix(self):
        build_config.checked_name("foo-wdt")
        with self.assertRaises(ValueError):
            build_config.checked_name("foo-wdt-soak")


class ManifestNaming(unittest.TestCase):
    """BUILD_INFO.md names a soak build by its real configuration and command."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def manifest(self, directory, board, configuration_header=None):
        directory.mkdir()
        (directory / "sdkconfig").write_text('CONFIG_IDF_TARGET="esp32s3"\n'
                                             "CONFIG_ESP_TASK_WDT_PANIC=y\n"
                                             "CONFIG_ESP_TASK_WDT_TIMEOUT_S=20\n")
        macros = None
        if configuration_header:
            (directory / "configuration.json").write_text('{"board": "%s", "features": {}}' % board)
            macros = {"configuration_header": str(directory / "features.h"), "value": {},
                      "bool": {}, "names": []}
        with patch.object(sys, "argv", ["gen_build_info.py", str(directory), board]), \
                patch.object(gen_build_info, "resolve_macros",
                             return_value=(macros, None if macros else "not configured")), \
                patch("builtins.print"):
            gen_build_info.main()
        return (directory / "BUILD_INFO.md").read_text()

    def test_board_soak_reproduces_with_the_switch(self):
        text = self.manifest(self.root / "build-feathers3-wdt-soak", "feathers3")
        self.assertIn("tools/build_board.sh feathers3 --wdt-soak\n", text)
        self.assertIn("Watchdog soak build", text)

    def test_configuration_soak_names_the_real_configuration(self):
        text = self.manifest(self.root / "build-config-xiao-no-bluetooth-wdt-soak", "xiao_s3",
                             configuration_header=True)
        self.assertIn("# Build manifest — `xiao-no-bluetooth`", text)
        self.assertIn("python3 tools/build_config.py build xiao-no-bluetooth --wdt-soak\n", text)
        self.assertIn("configurations/xiao-no-bluetooth.json", text)
        self.assertNotIn("build xiao-no-bluetooth-wdt-soak", text)
        self.assertNotIn("configurations/xiao-no-bluetooth-wdt-soak", text)

    def test_ordinary_build_manifest_has_no_soak_marks(self):
        text = self.manifest(self.root / "build-feathers3", "feathers3")
        self.assertIn("tools/build_board.sh feathers3\n", text)
        # The Source line quotes the commit subject, which may name a soak branch.
        text = "\n".join(l for l in text.splitlines() if not l.startswith("**Source:**"))
        self.assertNotIn("wdt-soak", text)
        self.assertNotIn("Watchdog soak build", text)


class WrapperWiring(unittest.TestCase):
    """Run the real tools/build_board.sh against a fake idf.py."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.repo = root / "repo"
        (self.repo / "tools").mkdir(parents=True)
        shutil.copy(REPO / "tools/build_board.sh", self.repo / "tools/build_board.sh")
        # build_board.sh sources the checkout build lock helper.
        shutil.copy(REPO / "tools/checkout_build_lock.sh", self.repo / "tools/checkout_build_lock.sh")
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
        # The checkout build lock has its own test (managed_components_stamp); mark it held
        # so the stand-in python3 here is not asked to take it.
        env["HW1_CHECKOUT_BUILD_LOCK"] = str(self.repo / ".hw1-build/checkout.lock")
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


    def test_reserved_configuration_name_is_refused(self):
        env = dict(os.environ, PATH=f"{self.bin}:{os.environ.get('PATH', '')}")
        result = subprocess.run(["bash", str(self.repo / "tools/build_board.sh"), "xiao_s3",
                                 "--config", "xiao-no-bluetooth-wdt-soak"],
                                capture_output=True, text=True, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("reserved", result.stderr)
        self.assertFalse(self.log.exists() and self.log.read_text().strip())


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
