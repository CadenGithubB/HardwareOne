#!/usr/bin/env python3
"""Run the MQTT port parser and all three setup input handlers on a host.

Draft staging is a recording boundary; setup_draft executes the actual owner.
The serial/OLED/CLI input flow and shared port parser are shipping code.
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
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    current = (COMPONENT / "System_SetupWizard.cpp").read_text()
    helper = extract_block(current, "bool wizardParseMQTTPort(")
    assert "bool wizardStageMQTTSettings(const SetupWizardResult& result);" in (
        COMPONENT / "System_SetupWizard.h").read_text()
    mode_header = (COMPONENT / "System_CLIMode.h").read_text()
    cli_result = extract_block(mode_header, "enum CLIModeInputResult") + ";"
    harness = (HERE / "setup_mqtt_application_harness.cpp").read_text()

    with tempfile.TemporaryDirectory(prefix="hw1-setup-mqtt-") as temp:
        for label, source_dir in [("current", COMPONENT)]:
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
                    assert handler.count("wizardStageMQTTSettings(") == 1
                    assert "gSettings.mqtt" not in handler
            definitions = "\n\n".join([
                extract_block(header, "enum SetupWizardPage") + ";",
                extract_block(header, "struct SetupWizardResult") + ";",
                extract_block(mode, "enum class WizardSubMode") + ";",
                cli_result,
                extract_block(mode, "static struct") + " sWizard;",
            ])
            functions = "\n\n".join([
                extract_block(setup, "bool wizardHandleCancelInput("),
                extract_block(setup, "static String waitForWizardInput("),
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
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(executable)], check=True, timeout=60)
            print(f"MQTT setup application: {label} handler behavior passed")


if __name__ == "__main__":
    main()
