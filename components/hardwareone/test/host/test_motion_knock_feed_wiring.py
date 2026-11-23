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
- The IMU input hands the engine its samples through the decimator, each tap
  with its peak jump (backend -> hook sample -> engine sample).
- The shake configuration (Input_IMUCore.h: centre, 5 side, 5 top knocks):
  the hook only collects (the engine's knock stages) and hands the two
  finished steps over; the main loop (imuInputTick) saves them through the
  same save as imutapteach (imuKnockSaveTaught). No mount teach offer after it.
- The taught surfaces (imutapteach): the IMU task adds a teach step's knocks
  and drops their taps, never touches settings; the main loop (imuKnockTick)
  saves a finished step and hands the saved surfaces to the detector.
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
    require("out.hwTap = knockFeedTakeTap(out.hwTapKind, out.hwTapVec);" in lsm, "the 4517 hands out one pending tap per sample")

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
    require("out.hwTap = knockFeedTakeTap(out.hwTapKind, out.hwTapVec);" in fast and "out.hasHwTap = true;" in fast and
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

    # Taught surfaces (imutapteach): the IMU task collects the knocks, the
    # main loop saves them and hands the saved surfaces to the detector.
    require("knock::Teacher sTeacher;" in feed and "sTeacher.add(events[i])" in feed,
            "the feed's record step (IMU task) adds the teach step's knocks")
    require("sFeed.pending.clear()" in extract_block(feed, "void knockFeedRecord("),
            "a teach step's knocks press no button")
    require("c.surfaces = sSurfaces;" in extract_block(feed, "knock::Config knockFeedConfig()"),
            "each sample's config carries the taught surfaces")
    for gone in ("setSetting", "gSettings"):
        require(gone not in feed, f"the feed (IMU task) never touches settings ({gone})")
    tick = extract_block(owner, "void imuKnockTick()")
    require("knockFeedSetSurfaces(" in tick and "knockFeedTeachPoll(now, true)" in tick,
            "the main loop applies the saved surfaces and takes a finished step")
    require("imuKnockSaveTaught(" in extract_block(owner, "static void teachFinished("), "a finished step is saved there")
    save = extract_block(owner, "bool imuKnockSaveTaught(")
    require("knock::judgeTaught(savedSurfaces(), side, face)" in save and save.count("setSetting(") == 2,
            "one save for one surface or both, judged by the shared core")
    teach_cmd = extract_block(owner, "const char* cmd_imutapteach(")
    require("imuInputConfiguring()" in teach_cmd, "imutapteach waits while a shake configuration runs")
    require("imuKnockTick();" in read("HardwareOne.cpp"), "the main loop calls imuKnockTick")
    teach = table.index('{ "imutapteach"')
    require(table.rindex("#if ENABLE_IMU_KNOCK_DETECTOR", 0, teach) > table.rindex("#endif", 0, teach),
            "imutapteach is registered with the detector")

    # The input: the decimator in front of the engine.
    body = extract_block(hook, "static void imuInputHook(")
    require(body.index("sDecimator.add(") < body.index("sEngine.update("), "the engine takes decimated samples")
    require("motionBackendInfo().samplePeriodMs" in body, "the factor follows the backend's sample period")
    # Each tap's peak jump travels with it to the engine.
    require("out.tapVec[i] = hwTapVec[i]" in read("HAL_MotionHookCore.h"), "the hook sample carries the peak jump")
    require("sample.hwTapKind, sample.hwTapVec)" in owner, "the motion task hands the backend's peak jump to the hook")
    require("raw.tapVec[i] = sample.tapVec[i]" in body, "the hook hands it to the engine's sample")

    # The shake configuration: collected on the IMU task, saved on the main loop.
    for gone in ("setSetting(", "imuKnockSaveTaught("):
        require(gone not in body, f"the hook (IMU task) never saves ({gone})")
    require("EXT_RAM_BSS_ATTR static ConfigTaught sConfigTaught;" in hook, "the hand-over is in PSRAM")
    require("sConfigTaughtReady.store(true)" in body and "knockFeedTeachCancel()" in body,
            "the hook hands the finished steps over and stops an imutapteach step at the side stage")
    tick = extract_block(hook, "void imuInputTick()")
    require("imuKnockSaveTaught(&taught.side, &taught.face" in tick and "sConfigTaughtReady.store(false)" in tick,
            "the main loop saves both surfaces")
    require("cfg.teachKnocks = knockFeedActive();" in hook, "the knock stages run where the detector feeds the taps")
    for gone in ("learnAfterCalibration", "kLearnOffered", "Tap the face to teach"):
        require(gone not in hook and gone not in read("Input_IMUCore.h"), f"no mount teach offer after it ({gone})")
    print("motion_knock_feed_wiring: ok")


if __name__ == "__main__":
    main()
