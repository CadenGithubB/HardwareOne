#!/usr/bin/env python3
"""The IMU's holders and per-sample hook as HAL_Motion.cpp wires them.

HAL_MotionHookCore.h's rules have their own test (motion_hook_core_tests).
Here the firmware's use of them:

- Run on the host, extracted unchanged: imuHold / imuRelease /
  imuHolderCount and `closeimu` (cmd_imustop). While a consumer holds the
  IMU, `closeimu` answers "Error: The IMU is in use by <who> ..." and asks
  the stop queue for nothing; released, it goes to the queue as before; a
  bad argument is still refused first. The holder list is touched only
  inside its critical section.
- Source contracts for the IMU task's side (imuStoreSample is too wide to
  run here): the hook is called once per stored sample, after the action
  detector, outside the cache lock's scope, with the backend's tilt (before
  the orientation correction) and the time the sample was read; unregister
  does not wait for its own call when it runs on the IMU task.
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


def contracts(owner: str) -> None:
    store = extract_block(owner, "static bool imuStoreSample(")
    require(store.count("motionHookCall(sImuHook,") == 1, "imuStoreSample calls the hook once")
    detector = store.index("imuRunActions(actions, /*postEvents=*/true)")
    call = store.index("motionHookCall(sImuHook,")
    require(detector < call, "the hook is called after the action detector has seen the sample")
    # The cache lock lives in the `if (publish) { ... }` block, which ends before the detector.
    publish = extract_block(store, "if (publish)")
    require("SensorCacheGuard" in publish and store.index(publish) + len(publish) < detector,
            "the cache lock's scope ends before the detector and the hook")
    require("SensorCacheGuard" not in store[detector:], "no lock is taken around the hook call")
    sample = store[store.index("motionHookSample("):call]
    require(re.search(r"motionHookSample\(\(uint32_t\)readMs, sample\.pitch, sample\.roll, sample\.accel,", sample)
            is not None, "the hook gets the read time and the backend's tilt, before the orientation correction")
    require("sample.hasGravity ? sample.gravity : nullptr" in sample and "sample.hasHwTap, sample.hwTap, shaking" in sample,
            "the hook gets gravity only when the backend has it, the chip's raw tap flag and the detector's shake state")
    task = extract_block(owner, "void imuTask(void* parameter)")
    require("imuStoreSample(sample, publish, sampleMs, nowMs)" in task and task.count("imuStoreSample(") == 1,
            "the task stores each ready sample once, with the time it was read")
    require(re.search(r"if \(result && sample\.ready\) \{[^}]*imuStoreSample\(", task, re.S) is not None,
            "only a sample with a reading reaches the hook")
    unregister = extract_block(owner, "bool imuSampleHookUnregister(")
    require("xTaskGetCurrentTaskHandle() == imuTaskNow" in unregister and
            "motionHookUnregister(sImuHook, fn, ctx, fromHook," in unregister and "vTaskDelay(1)" in unregister,
            "unregister waits a tick at a time, and not for its own call on the IMU task")
    require("return motionHookRegister(sImuHook, fn, ctx);" in extract_block(owner, "bool imuSampleHookRegister("),
            "register takes the one slot")
    # A build without the IMU has the same API, answering no.
    header = (COMPONENT / "HAL_Motion.h").read_text()
    disabled = header[header.index("\n#else\n"):]
    for name in ("imuSampleHookRegister", "imuSampleHookUnregister", "imuHold", "imuRelease", "imuHolderCount"):
        require(re.search(r"inline \w+ " + name + r"\(", disabled) is not None,
                f"HAL_Motion.h has no {name} for a build without the IMU")


def generate(owner: str) -> str:
    functions = [extract_block(owner, marker) for marker in (
        "bool imuHold(const char* who)", "bool imuRelease(const char* who)",
        "static MotionHolders imuHoldersCopy()", "uint8_t imuHolderCount()",
        "const char* cmd_imustop(const String& argsInput)")]
    return (HERE / "motion_hook_wiring_harness.cpp").read_text().replace("// INSERT_PRODUCTION", "\n\n".join(functions))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    owner = (COMPONENT / "HAL_Motion.cpp").read_text()
    contracts(owner)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-function",
             "-I", str(COMPONENT)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-motion-hook-wiring-") as directory:
        source = Path(directory) / "wiring.cpp"
        binary = Path(directory) / "wiring"
        source.write_text(generate(owner))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
