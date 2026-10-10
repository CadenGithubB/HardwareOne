#!/usr/bin/env python3
"""Execute production wizard completion and both exit adapters on the host.

The interactive dialog, storage owners, network and output are boundaries. The
helper, adapters, cancel dispatch and serial timeout implementation are shipping
code extracted unchanged. Wi-Fi-enabled and disabled variants run separately.
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
    setup = (COMPONENT / "System_SetupWizard.cpp").read_text()
    header = (COMPONENT / "System_SetupWizard.h").read_text()
    mode = (COMPONENT / "System_SetupWizardMode.cpp").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    wifi = (COMPONENT / "System_WiFiNetworks.h").read_text()
    helper = extract_block(setup, "void wizardSaveAndApplySettings(")
    wrapper = extract_block(setup, "SetupWizardResult runAndApplyFeatureWizard(")
    on_exit = extract_block(mode, "static void wizardMode_onExit(")
    require("void wizardSaveAndApplySettings(const SetupWizardResult& result);" in header,
            "shared setup completion declaration missing")
    for adapter in (wrapper, on_exit):
        require(adapter.count("wizardSaveAndApplySettings(") == 1, "adapter must call shared completion once")
        for obsolete in ("saveSetupWiFiNetwork(", "writeSettingsJson(", "writeDebugJson(",
                         "applySettings(", "setupNTP("):
            require(obsolete not in adapter, f"adapter duplicates completion operation: {obsolete}")
    require("wizardFinalize(" not in helper, "completion must not move or repeat finalization")
    require(extract_block(setup, "SetupWizardResult runSetupWizard(").count("wizardFinalize(result);") == 1,
            "blocking dialog finalization must remain in its existing owner")
    require(on_exit.index("wizardFinalize(sWizard.result)") < on_exit.index("wizardSaveAndApplySettings("),
            "CLI finalization must precede shared persistence")
    for dependency in ('"System_NTP.h"', '"System_WiFiNetworks.h"', '<WiFi.h>'):
        require(dependency not in mode, f"CLI adapter still depends on completion implementation: {dependency}")

    types = [extract_block(header, "struct SetupWizardResult") + ";",
             extract_block(mode, "enum class WizardSubMode") + ";",
             extract_block(mode, "static struct") + " sWizard;",
             extract_block((COMPONENT / "System_CLIMode.h").read_text(), "enum CLIModeInputResult") + ";",
             extract_block(wifi, "enum class WifiNetworkSaveResult") + ";"]
    state = [re.search(r"(?m)^static volatile[^\n]*\b" + name + r"\b[^\n]*;", utils)[0]
             for name in ("sSerialWaitTimeoutMs", "sSerialWaitLastActivityMs", "sWizardCancelRequested")]
    state.append(re.search(r"(?m)^volatile bool gWizardOwnsSerial[^\n]*;", setup)[0])
    functions = [extract_block(wifi, "inline const char* wifiNetworkSaveResultMessage("),
                 extract_block(utils, "void setSerialWaitTimeout("),
                 extract_block(utils, "bool isWizardCancelRequested("),
                 extract_block(utils, "String waitForSerialInputBlocking("),
                 extract_block(mode, "static void clearWizardWifiChoices("),
                 helper, wrapper, on_exit,
                 extract_block(mode, "static CLIModeInputResult wizardMode_onInput(")]
    harness = (HERE / "setup_completion_harness.cpp").read_text()
    for marker, text in {"// INSERT_PRODUCTION_TYPES": "\n\n".join(types),
                         "// INSERT_PRODUCTION_STATE": "\n".join(state),
                         "// INSERT_PRODUCTION_FUNCTIONS": "\n\n".join(functions)}.items():
        require(harness.count(marker) == 1, f"expected one insertion point: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-setup-completion-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(generate())
        for wifi in (0, 1):
            executable = Path(directory) / f"test-wifi-{wifi}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-function", "-Wno-unused-parameter", f"-DENABLE_WIFI={wifi}",
                       str(source), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
