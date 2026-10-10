#!/usr/bin/env python3
"""Execute the production wizard draft, navigation and completion owner.

The complete noninteractive wizard core is compiled, not a second implementation
of its choices or draft bookkeeping. The feature/settings registries, platform
allocation, storage, clock and service application are controlled boundaries.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate() -> str:
    source = (COMPONENT / "System_SetupWizard.cpp").read_text()
    header = (COMPONENT / "System_SetupWizard.h").read_text()
    feature_header = (COMPONENT / "System_FeatureRegistry.h").read_text()
    feature_source = (COMPONENT / "System_FeatureRegistry.cpp").read_text()
    wifi_header = (COMPONENT / "network/wifi/System_WiFiNetworks.h").read_text()
    settings_header = (COMPONENT / "System_Settings.h").read_text()
    # Keep all owner-private helpers/constants and their actual call graph.
    start = source.index("static const char* const SEED_HANDHELD")
    end = source.index("// Serial Console Wizard", start)
    types = re.sub(r"(?m)^#include[^\n]*\n", "", header)
    types += "\n" + "\n".join(extract_block(feature_header, name) + ";" for name in
                                  ("enum FeatureCategory", "enum FeatureFlags", "struct FeatureEntry"))
    types += "\n" + extract_block(wifi_header, "enum class WifiNetworkSaveResult") + ";"
    registry = "\n\n".join(extract_block(feature_source, name) for name in
        ("size_t getFeatureCount(", "const FeatureEntry* getFeatureByIndex(",
         "const FeatureEntry* getFeatureById(", "bool isFeatureCompiled(",
         "bool isFeatureEnabled(", "uint32_t getEnabledFeaturesHeapEstimate("))
    functions = source[start:end] + "\n" + extract_block(wifi_header, "inline const char* wifiNetworkSaveResultMessage(")
    functions += "\n" + extract_block(source, "bool wizardSaveAndApplySettings(")
    mutators = "\n\n".join(extract_block(settings_header, marker) for marker in
                           ("template<typename T>\ninline void setSetting(",
                            "inline void setSetting(String& field, const String&",
                            "inline void setSetting(String& field, const char*",
                            "template<typename T>\ninline void setDebugSetting("))
    harness = (HERE / "setup_draft_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_TYPES", types), ("// INSERT_REGISTRY", registry),
                                ("// INSERT_MUTATORS", mutators),
                                ("// INSERT_PRODUCTION", functions)):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-setup-draft-") as temporary:
        directory = Path(temporary)
        unit = directory / "draft.cpp"
        unit.write_text(generate())
        for full in (0, 1):
            output = directory / f"draft-{full}"
            flags = [f"-D{feature}={full}" for feature in
                     ("ENABLE_WIFI", "ENABLE_ESPNOW", "ENABLE_MQTT", "ENABLE_NEOPIXEL",
                      "ENABLE_HTTP_SERVER", "ENABLE_HTTPS", "ENABLE_BLUETOOTH",
                      "ENABLE_G2_GLASSES", "ENABLE_OLED_DISPLAY")]
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-function", "-Wno-unused-parameter", *flags,
                       "-Wno-zero-length-array", "-Wno-unused-but-set-variable",
                       "-I", str(COMPONENT), str(unit), "-o", str(output)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(output)], check=True, timeout=60)


if __name__ == "__main__":
    main()
