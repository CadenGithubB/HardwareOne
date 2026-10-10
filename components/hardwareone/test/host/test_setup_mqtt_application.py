#!/usr/bin/env python3
"""Run the production MQTT setup helper and all three input handlers on a host.

Only String storage, settings storage, input, navigation and output are faked.
The settings application and serial/OLED/CLI control flow are extracted intact.
An optional pre-refactor source directory runs the same handler cases for a
one-time behavior comparison; normal CTest needs only the current checkout.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    current = (COMPONENT / "System_SetupWizard.cpp").read_text()
    helper = extract_block(current, "void wizardApplyMQTTSettings(")
    assert "void wizardApplyMQTTSettings(const SetupWizardResult& result);" in (
        COMPONENT / "System_SetupWizard.h").read_text()
    mode_header = (COMPONENT / "System_CLIMode.h").read_text()
    cli_result = extract_block(mode_header, "enum CLIModeInputResult") + ";"
    harness = (HERE / "setup_mqtt_application_harness.cpp").read_text()

    with tempfile.TemporaryDirectory(prefix="hw1-setup-mqtt-") as temp:
        for label, source_dir in [("current", COMPONENT)] + (
                [("baseline", args.baseline)] if args.baseline else []):
            setup = (source_dir / "System_SetupWizard.cpp").read_text()
            oled = (source_dir / "OLED_SetupWizard.cpp").read_text()
            mode = (source_dir / "System_SetupWizardMode.cpp").read_text()
            header = (source_dir / "System_SetupWizard.h").read_text()
            handlers = [extract_block(setup, "static void handleSerialMQTTPage("),
                        extract_block(oled, "void handleOLEDMQTTPage("),
                        extract_block(mode[mode.rindex("static CLIModeInputResult dispatchMQTT("):],
                                      "static CLIModeInputResult dispatchMQTT(")]
            if label == "current":
                for handler in handlers:
                    assert handler.count("wizardApplyMQTTSettings(") == 1
            definitions = "\n\n".join([
                extract_block(header, "enum SetupWizardPage") + ";",
                extract_block(header, "struct SetupWizardResult") + ";",
                extract_block(mode, "enum class WizardSubMode") + ";",
                cli_result,
                extract_block(mode, "static struct") + " sWizard;",
            ])
            functions = "\n\n".join([
                helper,
                extract_block(mode, "static WizardSubMode subModeForPage("),
                *handlers,
                extract_block(mode, "static CLIModeInputResult wizardMode_onInput("),
            ])
            unit = Path(temp) / f"{label}.cpp"
            unit.write_text(harness.replace("// INSERT_PRODUCTION_TYPES", definitions)
                           .replace("// INSERT_PRODUCTION_FUNCTIONS", functions))
            executable = Path(temp) / label
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-pedantic", "-Wno-unused-function", str(unit), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(executable)], check=True, timeout=60)
            print(f"MQTT setup application: {label} handler behavior passed")


if __name__ == "__main__":
    main()
