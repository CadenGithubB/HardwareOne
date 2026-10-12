#!/usr/bin/env python3
"""Exercise wizard rows against the production registry and build configuration.

The board headers, saved choices, derived rules, registry predicates/table and
wizard owner all run unchanged. Storage/platform boundaries reuse setup_draft;
expected feature coverage is enumerated from the actual compiled registry.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_setup_draft import generate as draft_generate
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = COMPONENT.parents[1]


def generate() -> str:
    unit = draft_generate()
    # Retain only the shared boundary and reset/check helpers; the separate
    # draft suite owns persistence failure/touched-only coverage.
    unit = unit[:unit.index("static void toggleFeature(")]
    source = (COMPONENT / "System_FeatureRegistry.cpp").read_text()
    begin = source.index("static bool isWiFiCompiled()")
    end = source.index(";", source.index("static const size_t featureRegistryCount", begin)) + 1
    registry = source[begin:end]
    fixture_begin = unit.index("static bool compiledWifi()")
    fixture_end = unit.index("size_t getFeatureCount(", fixture_begin)
    unit = unit[:fixture_begin] + registry + "\n" + unit[fixture_end:]
    # Settings binding fields are boundary storage only; the pointer targets
    # and their compiled availability still come from the shipping registry.
    fields = sorted(set(re.findall(r"&gSettings\.(\w+)", registry)))
    settings_begin = unit.index("static struct SettingsBoundary")
    settings_end = unit.index("} gSettings;", settings_begin)
    existing = unit[settings_begin:settings_end]
    missing = [field for field in fields if not re.search(r"\b" + field + r"\b", existing)]
    unit = unit[:settings_end] + "  bool " + ", ".join(missing) + ";\n" + unit[settings_end:]
    mode = (COMPONENT / "System_SetupWizardMode.cpp").read_text()
    cli_header = (COMPONENT / "System_CLIMode.h").read_text()
    setup = (COMPONENT / "System_SetupWizard.cpp").read_text()
    adapter_types = "\n".join([
        extract_block(mode, "enum class WizardSubMode") + ";",
        extract_block(mode, "static struct") + " sWizard;",
        "volatile bool gInHelpRender = false;",
        extract_block(mode, "struct WizardRender") + ";",
        extract_block(cli_header, "enum CLIModeInputResult") + ";",
    ])
    adapter_boundaries = """
static void appendPromptTo(char*, size_t) {}
static void clearWizardWifiChoices() {}
static CLIModeInputResult unexpectedDispatch(const String&, char*, size_t) {
  assert(false && "unexpected optional page dispatch"); return CLI_MODE_HANDLED;
}
static auto dispatchESPNow = unexpectedDispatch;
static auto dispatchMQTT = unexpectedDispatch;
static auto dispatchWiFi = unexpectedDispatch;
"""
    adapter_code = "\n\n".join([
        extract_block(setup, "void printSerialPageStatus("),
        extract_block(mode, "static WizardSubMode subModeForPage("),
        extract_block(mode[mode.rindex("static CLIModeInputResult dispatchTopLevelPage("):],
                      "static CLIModeInputResult dispatchTopLevelPage("),
        extract_block(mode[mode.rindex("static CLIModeInputResult dispatchModePage("):],
                      "static CLIModeInputResult dispatchModePage("),
        extract_block(mode, "static CLIModeInputResult wizardMode_onInput("),
    ])
    return ('#include "System_BuildConfig.h"\n' + unit + adapter_types + adapter_boundaries + adapter_code +
            (HERE / "setup_menu_layout_harness.cpp").read_text())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    profiles = [
        ("qtpy-maps", "qtpy-maps-test", None, "ESP32", "ARDUINO_ADAFRUIT_QTPY_ESP32_DEV", {}),
        ("xiao-default", "xiao-default", "xiao_s3", "ESP32S3", "ARDUINO_XIAO_ESP32S3_DEV", {}),
        ("p4-default", None, "p4x_eye", "ESP32P4", None, {}),
        # Explicitly qualify the SENSOR-category enablement exception and
        # MQTT parent/startup adjacency even when ordinary profiles omit them.
        ("xiao-ml-mqtt", "xiao-default", "xiao_s3", "ESP32S3", "ARDUINO_XIAO_ESP32S3_DEV",
         {"ENABLE_EDGE_IMPULSE": 1, "ENABLE_MQTT": 1}),
        ("minimal", None, None, "ESP32", "ARDUINO_ADAFRUIT_QTPY_ESP32_DEV", {
            "NETWORK_FEATURE_LEVEL": 0, "WEB_FEATURE_LEVEL": 0, "I2C_FEATURE_LEVEL": 0,
            "DISPLAY_TYPE": 0, "INPUT_DEVICE_TYPE": 0, "ENABLE_BLUETOOTH": 0,
            "ENABLE_MQTT": 0, "ENABLE_NEOPIXEL": 0, "ENABLE_AUTOMATION": 0,
            "ENABLE_ESP_SR": 0, "ENABLE_EDGE_IMPULSE": 0, "ENABLE_LLM_BACKEND": 0,
            "ENABLE_LLM_SOURCE_ONBOARD": 0, "ENABLE_LLM_SOURCE_CM5": 0,
            "ENABLE_CAMERA_SENSOR": 0, "ENABLE_MICROPHONE_SENSOR": 0,
            "ENABLE_LOCAL_STT": 0, "ENABLE_QUICKNOTE": 0}),
    ]
    with tempfile.TemporaryDirectory(prefix="hw1-setup-menu-") as temporary:
        directory = Path(temporary)
        unit = directory / "menu.cpp"
        unit.write_text(generate())
        for name, saved, board, target, variant, extra in profiles:
            features = json.loads((REPO / "configurations" / f"{saved}.json").read_text())["features"] if saved else {}
            features.update(extra)
            override = directory / f"{name}.h"
            override.write_text("\n".join(f"#undef {key}\n#define {key} {value}" for key, value in features.items()))
            flags = [f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{override}"', f"-DCONFIG_IDF_TARGET_{target}=1",
                     f'-DTEST_PROFILE="{name}"', f"-DTEST_MINIMAL={int(name == 'minimal')}"]
            if board:
                flags.append(f'-DHW1_BOARD_CONFIG_HEADER="{REPO / "boards" / (board + ".features.h")}"')
            if variant:
                flags.append(f"-D{variant}=1")
            binary = directory / name
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
                       "-Wno-unused-but-set-variable", *flags, "-I", str(COMPONENT),
                       str(unit), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
