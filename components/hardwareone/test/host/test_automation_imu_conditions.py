#!/usr/bin/env python3
"""Run the automation conditions on the IMU (tilt, flip, tap) as the firmware does.

evaluateCondition (System_Automation.cpp) is too wide to build on the host, so
its parts that an IMU condition passes through are extracted unchanged on
every run and joined in the same order: the `<variable> <operator> <value>`
parse, the IMU_ branch and the comparisons. The values come from the shipping
rule (HAL_MotionCore.h, motionConditionValue), the detector state from the
shipping detector fed with samples. The IMU's flags, its reading and its
detector copy are the harness's.

Covers: IMU_PITCH / IMU_ROLL / IMU_TILT with every numeric operator and
negative values, IMU_POSE with = / != / CONTAINS in any case (flip is
IMU_POSE=FACE-DOWN), IMU_TAP_AGE, and that a closed IMU, a reading that is not
valid yet, a busy cache, no tap yet and an unknown IMU_ name (IMU_HEADING: not
offered yet) all make the condition false.

Source contracts: the branch sits in evaluateCondition under ENABLE_IMU_SENSOR
with a false answer for builds without the IMU, and the user-facing lists
(the web Automations page and the automation help) name the five variables.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
VARIABLES = ("imu_pitch", "imu_roll", "imu_tilt", "imu_pose", "imu_tap_age")


def generate(component: Path) -> str:
    source = (component / "System_Automation.cpp").read_text()
    source = source[source.index("bool evaluateCondition(const char* condition) {"):]
    evaluate = extract_block(source, "bool evaluateCondition(")
    parse = evaluate[evaluate.index("  // Parse: sensor operator value"):evaluate.index("  // Get current sensor value")]
    branch = extract_block(evaluate, 'if (strncmp(sensor, "IMU_", 4) == 0)')
    require("#if ENABLE_IMU_SENSOR" in branch and "#else\n    return false;" in branch,
            "the IMU_ branch answers false in a build without the IMU")
    require("motionConditionValue(sensor," in branch, "the IMU_ branch asks the motion layer's rule")
    comparisons = evaluate[evaluate.index("  // Evaluate condition"):]
    function = ("static bool imuCondition(char* condStart) {\n" + parse +
                "  float currentValue = 0; bool isNumeric = true; char currentStringValue[32] = \"\";\n  " +
                branch + " else { return false; }\n" + comparisons)
    return (HERE / "automation_imu_conditions_harness.cpp").read_text().replace("// INSERT_PRODUCTION", function)


def lists(component: Path) -> None:
    page = (component / "WebPage_Automations.h").read_text()
    registry = (component / "System_CommandRegistry.cpp").read_text()
    for name in VARIABLES:
        require(page.count(f'<option value="{name}">') == 2,
                f"the web Automations page offers {name} in both condition pickers")
        require(name in registry, f"the automation help names {name}")
    require("imu_heading" not in page and "imu_heading" not in registry.lower(),
            "no heading condition is offered yet")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    lists(COMPONENT)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-function",
             "-Wno-unused-variable", "-I", str(COMPONENT)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-automation-imu-") as directory:
        source = Path(directory) / "imu_conditions.cpp"
        binary = Path(directory) / "imu_conditions"
        source.write_text(generate(COMPONENT))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
