#!/usr/bin/env python3
"""Exercise the production ESP-NOW setup staging and its three interfaces.

The serial/OLED/CLI handlers are extracted intact. Draft-owner calls are recording
boundaries; test_setup_draft.py separately exercises the real owner.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    setup = (COMPONENT / "System_SetupWizard.cpp").read_text()
    header = (COMPONENT / "System_SetupWizard.h").read_text()
    oled = (COMPONENT / "OLED_SetupWizard.cpp").read_text()
    mode = (COMPONENT / "System_SetupWizardMode.cpp").read_text()
    assert "bool wizardStageESPNowSettings(const SetupWizardResult& result);" in header
    handlers = [extract_block(setup, "static void handleSerialESPNowPage("),
                extract_block(oled, "void handleOLEDESPNowPage("),
                extract_block(mode[mode.rindex("static CLIModeInputResult dispatchESPNow("):],
                              "static CLIModeInputResult dispatchESPNow(")]
    for handler in handlers:
        assert handler.count("wizardStageESPNowSettings(") == 1
        for field in ("bleDeviceName", "espnowDeviceName", "espnowFriendlyName", "espnowRoom", "espnowZone", "espnowStationary"):
            assert f"gSettings.{field} =" not in handler, f"duplicate {field} application"
    definitions = "\n\n".join([
        extract_block(header, "enum SetupWizardPage") + ";",
        extract_block(header, "struct SetupWizardResult") + ";",
        extract_block(mode, "enum class WizardSubMode") + ";",
        extract_block((COMPONENT / "System_CLIMode.h").read_text(), "enum CLIModeInputResult") + ";",
        extract_block(mode, "static struct") + " sWizard;",
        extract_block(mode, "struct WizardRender") + ";",
    ])
    functions = "\n\n".join([
        extract_block(setup, "bool wizardHandleCancelInput("),
        extract_block(setup, "static String waitForWizardInput("),
        extract_block(mode, "static WizardSubMode subModeForPage("),
        *handlers, extract_block(mode, "static CLIModeInputResult wizardMode_onInput("),
    ])
    harness = (HERE / "setup_espnow_application_harness.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="hw1-setup-espnow-") as temp:
        source = Path(temp) / "test.cpp"
        source.write_text(harness.replace("// INSERT_PRODUCTION_TYPES", definitions)
                          .replace("// INSERT_PRODUCTION_FUNCTIONS", functions))
        executable = Path(temp) / "test"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(source), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
