#!/usr/bin/env python3
"""The software knock detector's shared feed as the backends and the IMU input use it.

The pure parts (knock::Feed, PendingTaps, SampleClock, the detector at 416 and
100 Hz) have their own test (motion_knock_core_tests); the decimator its own
(input_imu_core_tests). Here the firmware's wiring, as source contracts:

- One feed for both backends (HAL_Motion_KnockFeed.cpp): the tuning atomics,
  the PSRAM trace and its critical section and the knock API bodies live
  there only; the 4517 backend keeps none of its own.
- The 4517 feeds it from its FIFO at 416 Hz; the BNO055 (with IMU input) at
  100 Hz: one short transaction of ACC_DATA and GRV_DATA per sample, the full
  block every 100 ms, the tap from the pending queue, gravity, and taps
  reported as found in software. Without IMU input the BNO055 reads as before.
- The IMU input hands the engine its samples through the decimator.
"""
from __future__ import annotations

from pathlib import Path
import re

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def read(name: str) -> str:
    return (COMPONENT / name).read_text()


def main() -> None:
    feed = read("HAL_Motion_KnockFeed.cpp")
    lsm = read("HAL_Motion_LSM6DSOX.cpp")
    bno = read("HAL_Motion_BNO055.cpp")
    owner = read("HAL_Motion.cpp")
    hook = read("Input_IMU.cpp")
    config = read("System_BuildConfig.h")

    # One home for the shared state and the API bodies.
    for name in ("motionBackendSetKnockTuning(long thresholdMg", "motionBackendSetKnockSide(long axis",
                 "motionBackendKnockTrace(knock::Trace& out", "motionBackendKnockTraceClear() {"):
        require(name in feed, f"HAL_Motion_KnockFeed.cpp defines {name}")
        require(name not in lsm, f"{name} is defined once, in the shared feed")
    stubs = bno[bno.index("#if !ENABLE_IMU_KNOCK_DETECTOR"):]
    stubs = stubs[:stubs.index("#endif")]
    require("motionBackendKnockTraceClear() {}" in stubs and "motionBackendKnockTraceClear" not in
            bno.replace(stubs, ""), "the BNO055 has empty knock bodies only in a build without the detector")
    require("EXT_RAM_BSS_ATTR knock::Trace" in feed and "EXT_RAM_BSS_ATTR knock::Feed" in feed,
            "the trace and the feed are in PSRAM")
    require("portENTER_CRITICAL(&sKnockMux)" in feed, "the trace is copied under its critical section")
    for gone in ("sKnockTuning", "sKnockTrace", "sKnockMux", "knock::Detector", "pendingSide"):
        require(gone not in lsm, f"the 4517 backend keeps no {gone} of its own")
    require("ps_alloc" not in feed and "ps_alloc" not in owner, "static PSRAM, no allocation")
    require("#if ENABLE_IMU_KNOCK_DETECTOR" in feed, "the feed compiles only with the detector")

    # The 4517: FIFO at 416 Hz, its taps from the queue.
    require("knockFeedStart(true, kFifoHz)" in lsm and "constexpr uint16_t kFifoHz = 416;" in lsm,
            "the 4517 feeds the detector at 416 Hz once its FIFO streams")
    require("out.hwTap = knockFeedTakeTap(out.hwTapKind);" in lsm, "the 4517 hands out one pending tap per sample")

    # The BNO055 fast path.
    require("#if ENABLE_IMU_KNOCK_DETECTOR" in bno, "the BNO055 fast path is built only with the detector")
    fast = bno[bno.index("#if ENABLE_IMU_KNOCK_DETECTOR\nbool motionBackendRead("):]
    fast = extract_block(fast, "bool motionBackendRead(")
    require(fast.count("i2cTaskWithTimeout(") == 1, "one transaction per sample")
    require("kBno055RegAccelData" in fast and "kBno055RegGravityData" in fast,
            "each sample reads ACC_DATA and GRV_DATA")
    require(re.search(r"full \? kBno055DataBytes : kBno055VectorBytes", fast) is not None and
            "bno055FullDue(" in fast, "the full block only when due")
    require("feed.clock.at(readUs)" in fast and "esp_timer_get_time()" in fast,
            "samples are stamped from their read time on the forward-only clock")
    require("out.hwTap = knockFeedTakeTap(out.hwTapKind);" in fast and "out.hasHwTap = true;" in fast and
            "out.hasGravity = true;" in fast, "taps from the queue, gravity, hasHwTap")
    require("kInfo{kMotionBackendBno055, \"BNO055\", kClockHz, kBno055FastPeriodMs, nullptr}" in bno and
            "kInfo{kMotionBackendBno055, \"BNO055\", kClockHz, 0, nullptr}" in bno,
            "10 ms with the detector, every imuDevicePollMs without")
    status = extract_block(bno, "void motionBackendStatus(")
    require("out.tapsInSoftware = true;" in status and "out.knockHz = kBno055FastHz;" in status,
            "imustatus says the firmware finds the taps")

    # The build: IMU input with either backend; the detector's commands follow it.
    require("#if ENABLE_IMU_INPUT && !ENABLE_IMU_LSM6DSOX" not in config, "IMU input is not refused for the BNO055")
    table = owner[owner.index("const CommandEntry imuCommands[] = {"):]
    trace = table.index('{ "imutaptrace"')
    require(table.rindex("#if ENABLE_IMU_KNOCK_DETECTOR", 0, trace) > table.rindex("#if ENABLE_IMU_LSM6DSOX", 0, trace),
            "imutaptrace is registered with the detector, for either backend")
    shock = table.index('{ "imutapshock"')
    require(table.rindex("#if ENABLE_IMU_LSM6DSOX", 0, shock) > table.rindex("#if ENABLE_IMU_KNOCK_DETECTOR", 0, shock),
            "imutapshock stays LSM6-only")

    # The input: the decimator in front of the engine.
    body = extract_block(hook, "static void imuInputHook(")
    require(body.index("sDecimator.add(") < body.index("sEngine.update("), "the engine takes decimated samples")
    require("motionBackendInfo().samplePeriodMs" in body, "the factor follows the backend's sample period")
    print("motion_knock_feed_wiring: ok")


if __name__ == "__main__":
    main()
