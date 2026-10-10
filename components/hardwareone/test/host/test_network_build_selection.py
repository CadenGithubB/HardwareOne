#!/usr/bin/env python3
"""Check real CMake source selection against compiler-resolved web guards.

Run the component CMake through idf_component_register in both requirement
discovery (no CONFIG_* values) and configured phases. Compile/preprocess the
actual BuildConfig header and web translation units' outer guards for the same
inputs. Disabled sources/dependencies may deliberately remain in the list;
an enabled implementation or its SDK dependency must never be omitted.

This does not compile the web implementation or qualify network hardware.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

COMPONENT = Path(__file__).resolve().parents[2]
REPO = COMPONENT.parents[1]
CXX = shutil.which("c++")
CMAKE = shutil.which("cmake")


def cmake_path(path):
    return "[==[" + str(path) + "]==]"


def write_override(path, values):
    path.write_text("\n".join(f"#undef {key}\n#define {key} {value}"
                              for key, value in values.items()) + "\n")


def web_guards():
    """Read representative production guards; app-specific gates are separate."""
    files = [COMPONENT / name for name in (
        "WebServer_Server.cpp", "WebServer_Events.cpp", "WebServer_Utils.cpp",
        "WebServer_MigrationTool.cpp", "WebAssets.cpp", "WebPage_Sensors.cpp",
        "WebPage_MQTT.cpp", "WebPage_Bond.cpp")]
    result = {}
    for path in files:
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
        guard = re.search(r"(?m)^\s*#if\s+([^\n]+)", source)
        if not guard or not guard[1].strip().startswith("ENABLE_"):
            raise AssertionError(f"{path.name}: review changed outer feature guard")
        result[path.name] = guard[1].strip()
    return result


class NetworkBuildSelection(unittest.TestCase):
    def test_supported_inputs_reach_both_build_phases(self):
        self.assertIsNotNone(CXX, "a host C++17 compiler is required")
        self.assertIsNotNone(CMAKE, "cmake is required")
        guards = web_guards()
        # These expectations describe requested outcomes at public entry points.
        # No test-side mapping derives pages or dependencies from feature levels.
        cases = [
            ("shared-default", {}, {}, {}, False, (1, 1, 1, 1)),
            ("manual-network-disabled", {"NETWORK_FEATURE_LEVEL": 0}, {}, {}, False, (0, 0, 0, 0)),
            ("manual-wifi-only", {"NETWORK_FEATURE_LEVEL": 1}, {}, {}, False, (1, 0, 0, 0)),
            ("manual-custom-http-off", {"CUSTOM_ENABLE_NET_HTTP": 0}, {}, {}, False, (1, 0, 0, 0)),
            ("manual-custom-wifi-off", {"CUSTOM_ENABLE_NET_WIFI": 0}, {}, {}, False, (0, 0, 0, 0)),
            ("manual-web-disabled", {"WEB_FEATURE_LEVEL": 0}, {}, {}, False, (1, 0, 0, 0)),
            ("manual-http-without-tls", {"NETWORK_FEATURE_LEVEL": 2, "WEB_FEATURE_LEVEL": 1,
                                          "ENABLE_HTTPS": 0}, {}, {}, False, (1, 1, 0, 1)),
            ("manual-network-expressions", {"NETWORK_FEATURE_LEVEL": "(1 + 1)",
                                             "WEB_FEATURE_LEVEL": "(1 + 2)",
                                             "ENABLE_HTTPS": "(1)"}, {}, {}, False, (1, 1, 1, 1)),
            ("xiao-board-default", {}, {}, {}, True, (1, 1, 1, 1)),
            ("named-http-off", {}, {"WEB_FEATURE_LEVEL": 0}, {}, True, (1, 0, 0, 0)),
            ("named-custom-wifi-off", {}, {"CUSTOM_ENABLE_NET_WIFI": 0,
                                           "ENABLE_MQTT": 0}, {}, True, (0, 0, 0, 0)),
            ("named-overrides-manual", {"WEB_FEATURE_LEVEL": 0},
             {"NETWORK_FEATURE_LEVEL": 2, "WEB_FEATURE_LEVEL": 3}, {}, True, (1, 1, 1, 1)),
            ("deployment-overrides-manual", {"WEB_FEATURE_LEVEL": 0}, {},
             {"NETWORK_FEATURE_LEVEL": 2, "WEB_FEATURE_LEVEL": 3}, True, (1, 1, 1, 1)),
            ("deployment-restore-only", {}, {},
             {"NETWORK_FEATURE_LEVEL": 1, "WEB_FEATURE_LEVEL": 0,
              "ENABLE_MIGRATION_TOOL": 1}, True, (1, 0, 0, 1)),
        ]
        for name, manual, named, deployment, board, expected in cases:
            with tempfile.TemporaryDirectory(prefix="hw1-network-selection-") as tmp:
                root = Path(tmp)
                source_dir = root / "component"
                source_dir.mkdir()
                header = (COMPONENT / "System_BuildConfig.h").read_text()
                for key, value in manual.items():
                    pattern = r"(?m)^(\s*#define\s+" + key + r"\s+)[^\n/]+"
                    header, changed = re.subn(pattern, lambda m: m[1] + str(value) + " ",
                                              header, count=1)
                    self.assertEqual(changed, 1, key)
                (source_dir / "System_BuildConfig.h").write_text(header)
                shutil.copyfile(COMPONENT / "CMakeLists.txt", source_dir / "CMakeLists.txt")
                overlays = {}
                if board:
                    overlays["BOARD"] = REPO / "boards/xiao_s3.features.h"
                for kind, values in (("FEATURE", named), ("DEPLOYMENT", deployment)):
                    if values:
                        overlays[kind] = root / f"{kind.lower()}.h"
                        write_override(overlays[kind], values)
                for configured in (False, True):
                    with self.subTest(configuration=name, phase="configured" if configured else "early"):
                        self.check_phase(root, source_dir, overlays, expected, guards, configured)

    def check_phase(self, root, source_dir, overlays, expected, guards, configured):
        sdk_dir = root / ("configured" if configured else "early")
        sdk_dir.mkdir()
        if configured:
            (sdk_dir / "sdkconfig.h").write_text(
                '#define CONFIG_IDF_TARGET "esp32s3"\n'
                '#define CONFIG_IDF_TARGET_ESP32S3 1\n'
                '#define CONFIG_BT_ENABLED 1\n'
                '#define CONFIG_BT_BLUEDROID_ENABLED 1\n'
                '#define CONFIG_SCCB_HARDWARE_I2C_PORT1 1\n')
        compiler = [CXX, "-x", "c++", "-std=c++17", "-I", str(source_dir),
                    "-I", str(sdk_dir), "-DCONFIG_IDF_TARGET_ESP32S3=1"]
        compiler.append("-DARDUINO_XIAO_ESP32S3_DEV=1" if "BOARD" in overlays else
                        "-DARDUINO_UM_FEATHERS3_DEV=1")
        for kind, path in overlays.items():
            compiler.append(f'-DHW1_{kind}_CONFIG_HEADER="{path}"')

        flags = ("ENABLE_WIFI", "ENABLE_HTTP_SERVER", "ENABLE_HTTPS", "ENABLE_MIGRATION_TOOL")
        unit = '#include "System_BuildConfig.h"\n' + "\n".join(
            f'static_assert({flag} == {value}, "{flag}");' for flag, value in zip(flags, expected))
        compiled = subprocess.run(compiler + ["-fsyntax-only", "-"], input=unit,
                                  text=True, capture_output=True, timeout=30)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

        unit = '#include "System_BuildConfig.h"\n'
        for index, predicate in enumerate(guards.values()):
            unit += f"#if {predicate}\nHW1_ENABLED_SOURCE_{index}\n#endif\n"
        for flag in flags:
            unit += f"#if {flag}\nHW1_FLAG_{flag}\n#endif\n"
        resolved = subprocess.run(compiler + ["-E", "-P", "-"], input=unit,
                                  text=True, capture_output=True, timeout=30)
        self.assertEqual(resolved.returncode, 0, resolved.stderr)
        enabled = {name for index, name in enumerate(guards)
                   if f"HW1_ENABLED_SOURCE_{index}" in resolved.stdout.splitlines()}

        result = sdk_dir / "registration.txt"
        wrapper = ["cmake_minimum_required(VERSION 3.16)", 'set(IDF_TARGET "esp32s3")']
        if configured:
            wrapper += ["set(CONFIG_IDF_TARGET_ESP32S3 1)", "set(CONFIG_BT_ENABLED 1)",
                        "set(CONFIG_BT_BLUEDROID_ENABLED 1)"]
        for kind, path in overlays.items():
            variable = {"BOARD": "HW1_BOARD_FEATURE_FILE", "FEATURE": "HW1_FEATURE_CONFIG_FILE",
                        "DEPLOYMENT": "HW1_DEPLOYMENT_FEATURE_FILE"}[kind]
            wrapper.append(f"set({variable} {cmake_path(path)})")
        wrapper += [
            "function(idf_build_get_property output property)",
            '  set(${output} "${${property}}" PARENT_SCOPE)',
            "endfunction()",
            "macro(idf_component_register)",
            '  cmake_parse_arguments(CAPTURE "" "" "SRCS;INCLUDE_DIRS;REQUIRES;LDFRAGMENTS" ${ARGN})',
            f'  file(WRITE {cmake_path(result)} "")',
            "  foreach(source IN LISTS CAPTURE_SRCS)",
            f'    file(APPEND {cmake_path(result)} "source:${{source}}\n")',
            "  endforeach()",
            "  foreach(dependency IN LISTS CAPTURE_REQUIRES)",
            f'    file(APPEND {cmake_path(result)} "dependency:${{dependency}}\n")',
            "  endforeach()",
            "  return()",
            "endmacro()",
            f"include({cmake_path(source_dir / 'CMakeLists.txt')})",
        ]
        script = sdk_dir / "capture.cmake"
        script.write_text("\n".join(wrapper) + "\n")
        evaluated = subprocess.run([CMAKE, "-P", str(script)], text=True,
                                   capture_output=True, timeout=30)
        self.assertEqual(evaluated.returncode, 0, evaluated.stdout + evaluated.stderr)
        self.assertTrue(result.exists(), "actual idf_component_register was not reached")
        entries = result.read_text().splitlines()
        source_list = [line.removeprefix("source:") for line in entries if line.startswith("source:")]
        sources = set(source_list)
        # The owner remains registered in both phases with Wi-Fi enabled or
        # disabled; its own feature guard supplies the disabled implementation.
        # Retain duplicates here: a set alone would hide double registration.
        wifi_owner = "network/wifi/System_WiFiNetworks.cpp"
        self.assertTrue((COMPONENT / wifi_owner).is_file())
        self.assertEqual(source_list.count(wifi_owner), 1)
        self.assertNotIn("System_WiFiNetworks.cpp", sources)
        dependencies = {line.removeprefix("dependency:") for line in entries
                        if line.startswith("dependency:")}
        self.assertFalse(enabled - sources, f"enabled source omitted: {sorted(enabled - sources)}")
        if "HW1_FLAG_ENABLE_HTTP_SERVER" in resolved.stdout or "HW1_FLAG_ENABLE_MIGRATION_TOOL" in resolved.stdout:
            self.assertIn("esp_http_server", dependencies)
        if "HW1_FLAG_ENABLE_HTTPS" in resolved.stdout:
            self.assertIn("esp_https_server", dependencies)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=CXX)
    parser.add_argument("--cmake", default=CMAKE)
    args, remaining = parser.parse_known_args()
    CXX, CMAKE = args.cxx, args.cmake
    unittest.main(argv=[__file__, *remaining])
