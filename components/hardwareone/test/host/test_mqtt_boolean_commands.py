#!/usr/bin/env python3
"""Execute MQTT toggle handlers, shared bool parser and setting mutator on a host.

The production functions and publish macro are extracted intact on every run.
Only String storage, persistence/notification and MQTT stop are host boundaries.
--baseline replays the same assertions against a saved component directory;
the pre-fix source deliberately fails the alias/invalid-input regressions.
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


def extract_macro(source, name):
    lines = source[source.index(f"#define {name}("):].splitlines()
    result = []
    for line in lines:
        result.append(line)
        if not line.endswith("\\"):
            return "\n".join(result)
    raise AssertionError(f"unterminated macro {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    source_dir = args.baseline or COMPONENT
    mqtt = (source_dir / "System_MQTT.cpp").read_text()
    utils = (source_dir / "System_Utils.h").read_text()
    settings = (source_dir / "System_Settings.h").read_text()
    publish = mqtt[mqtt.index("#define MQTT_PUBLISH_CMD("):
                   mqtt.index("// Command table - names must match setting keys")]
    definitions = "\n\n".join([
        extract_macro(utils, "RETURN_VALID_IF_VALIDATE_CSTR"),
        extract_block(utils, "inline int parseBoolArg("),
        extract_block(settings, "template<typename T>\ninline void setSetting("),
        *[extract_block(mqtt, f"const char* cmd_{name}(") for name in
          ("mqttclientenabled", "mqttautostart", "mqttsubscribe")],
        publish,
    ])
    harness = (HERE / "mqtt_boolean_commands_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION_DEFINITIONS"
    assert harness.count(marker) == 1
    results = []
    with tempfile.TemporaryDirectory(prefix="hw1-mqtt-booleans-") as temp:
        unit = Path(temp) / "commands.cpp"
        unit.write_text(harness.replace(marker, definitions))
        for encoder in (0, 1):
            executable = Path(temp) / f"commands-{encoder}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-pedantic", f"-DENABLE_ANO_ENCODER={encoder}",
                       str(unit), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            results.append(subprocess.run([str(executable)], timeout=60).returncode)
    raise SystemExit(1 if any(results) else 0)


if __name__ == "__main__":
    main()
