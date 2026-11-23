#!/usr/bin/env python3
"""The IMU's holders and per-sample hook as HAL_Motion.cpp wires them.

HAL_MotionHookCore.h's rules have their own test (motion_hook_core_tests).
Here the firmware's use of them:

- Run on the host, extracted unchanged: imuHold / imuRelease /
  imuHolderCount and `closeimu` (cmd_imustop). While a consumer holds the
  IMU, `closeimu` answers "Error: The IMU is in use by <who> ..." and asks
  the stop queue for nothing; released, it goes to the queue as before; a
  bad argument is still refused first; a held IMU that already stopped is
  not refused; a hold is refused while a close is on its way. The holder list is touched only
  inside its critical section.
- Source contracts for the IMU task's side (imuStoreSample is too wide to
  run here): the hook is called once per stored sample, after the action
  detector, outside the cache lock's scope, with the backend's tilt (before
  the orientation correction) and the time the sample was read; unregister
  does not wait for its own call when it runs on the IMU task and reports a
  long wait once; the hook's last call (stopped) is made once in the task's
  exit branch; the queue worker drops a stop of a held IMU.
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
    require("sample.hasGravity ? sample.gravity : nullptr" in sample and
            "sample.hasHwTap, sample.hwTap, afterGap," in sample and "shaking, kMotionTapAccel" in sample,
            "the hook gets gravity only when the backend has it, the chip's tap flag with the gap rule and the "
            "detector's shake state")
    # The gap rule sees every stored sample, hooked or not, before the hook's sample is built.
    gap = store.index("motionHookAfterGap(sImuHookFeed, (uint32_t)readMs, (uint32_t)sampleMs)")
    require(detector < gap < store.index("if (motionHookRegistered(sImuHook))"),
            "the gap rule runs on every stored sample")
    task = extract_block(owner, "void imuTask(void* parameter)")
    require(task.count("imuStoreSample(") == 1 and "imuStoreSample(sample, publish, sampleMs, readMs)" in task,
            "the task stores each ready sample once, with the time it was read")
    read = task.index("motionBackendRead(imuBus, address, second, sample);")
    stamp = task.index("const unsigned long readMs = millis();")
    require(read < stamp and task[read:stamp].count(";") == 1,
            "the sample's time is taken right after the read returns, not before it")
    require(re.search(r"if \(result && sample\.ready\) \{[^}]*imuStoreSample\(", task, re.S) is not None,
            "only a sample with a reading reaches the hook")
    # Every way the IMU closes goes through the task's exit branch: the hook's
    # last call (stopped) is made there, once, after the backend was released.
    exit_branch = extract_block(task, "if (!gImuRunning)")
    require(task.count("imuHookStopped();") == 1 and exit_branch.count("imuHookStopped();") == 1,
            "the stopped call is made once, in the task's exit branch")
    require(exit_branch.index("motionBackendStop();") < exit_branch.index("imuHookStopped();") <
            exit_branch.index("SENSOR_TASK_EXIT(IMU, gImuTaskHandle);"),
            "the stopped call comes after the backend is released and before the task ends")
    require(exit_branch.index("continue;") < exit_branch.index("imuHookStopped();"),
            "a retried exit (the cache was busy) does not repeat the stopped call")
    stopped = extract_block(owner, "static void imuHookStopped()")
    require("motionHookCall(sImuHook, motionHookStoppedSample((uint32_t)millis()))" in stopped and
            "sImuHookFeed = MotionHookFeed{};" in stopped, "the stopped call, and the next run starts after a gap")
    require("sImuHookFeed = MotionHookFeed{};" in extract_block(owner, "static void imuResetActions()"),
            "a run's first sample follows a gap")
    unregister = extract_block(owner, "bool imuSampleHookUnregister(")
    require("xTaskGetCurrentTaskHandle() == imuTaskNow" in unregister and
            "motionHookUnregister(sImuHook, fn, ctx, fromHook," in unregister and "vTaskDelay(1)" in unregister,
            "unregister waits a tick at a time, and not for its own call on the IMU task")
    require("motionHookWaitTooLong(" in unregister and "WARN_IMUF(" in unregister and "warned = true;" in unregister,
            "a long unregister wait is reported once")
    require("return motionHookRegister(sImuHook, fn, ctx," in extract_block(owner, "bool imuSampleHookRegister("),
            "register takes the one slot")
    # A hold that overtakes a queued closeimu is honoured by the queue worker.
    controller = (COMPONENT / "System_I2C.cpp").read_text()
    stop = extract_block(controller, "static void applySensorStop(")
    held = stop.index("if (sensor == I2C_DEVICE_IMU && imuHolderCount() > 0)")
    require(held < stop.index("sensorStartOutstanding(slot).exchange(false)"),
            "a held IMU's queued stop is dropped before anything is changed")
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
