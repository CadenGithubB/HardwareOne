#!/usr/bin/env python3
"""An intentional restart idles the P4's SDMMC host first; a companion retry waits for the slave.

Bench 2026-11-08 (P4X-EYE): after `reboot` the C6 link never came back. The
ESP32-P4's software restart is a CPU reset that pulses the SDMMC reset without
idling its DMA, and it was taken with the C6 link streaming (and the wifi_remote
esp_wifi_stop shutdown handler sending an RPC over SDIO). After it, commands
worked and every data transfer timed out, on the C6's slot and the SD card's,
until a chip reset. Separately, every later companion retry gave up in the same
millisecond because Hosted 2.12.13 never resets its slave-ready retry counter
after a failure.

The harness compiles the shipping definitions against recording fakes:
rebootDevice, quiesceSdmmcForRestart and factoryreset_doRestart
(System_Utils.cpp), fsLockForRestart (System_Mutex.cpp), hostedEventHandler,
startOnce, radioHalPrepare, radioHalStopTransport, radioHalSilenceForRestart
and radioHalHold with their helpers (HAL_Radio.cpp), and submitOp,
radioCompanionBeforeRestart, radioCompanionTick and radioCompanionBlocksRadio
(System_RadioCompanion.cpp), with the real System_RestartQuiesceCore.h and
Radio_CompanionCore.h. A restart must refuse new companion work before it
silences the C6 (unregister the RPC handler, latch EN low with gpio_hold_en),
take the filesystem lock for at most its timeout and drain for the SDMMC
timeout, after the flush delay and before ESP.restart(); an op already
running (Hosted's reset pulse, a hold release, also one racing the silence)
must not raise EN again before the restart; nothing (the tick, a feature
start, even before boot init) restarts the companion. A start whose Hosted
wait gave up must still wait for TRANSPORT_UP inside Hosted's budget, and
boot attempt 2 must reset the C6 again. A source check covers the one gate
the harness does not compile (startTransport). --source-dir points the suite
at another copy of the component (the pre-fix tree fails).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def one(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"expected one definition of {marker!r} in {name}")
    return extract_block(source, marker)


def constant(source: str, name: str) -> str:
    match = re.search(r"constexpr (?:uint32_t|int) " + name + r" = [^;]+;", source)
    require(match is not None, f"HAL_Radio.cpp defines {name}")
    return match.group(0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-dir", "--component", dest="source_dir", type=Path, default=COMPONENT)
    args = parser.parse_args()
    src = args.source_dir

    utils = (src / "System_Utils.cpp").read_text()
    hal = (src / "HAL_Radio.cpp").read_text()
    companion = (src / "System_RadioCompanion.cpp").read_text()
    mutex = (src / "System_Mutex.cpp").read_text()

    blocks = {
        "/*@MUTEX@*/": "\n\n".join([
            one(mutex, "static bool isHeldByCurrentTask(SemaphoreHandle_t mutex)", "System_Mutex.cpp"),
            one(mutex, "bool fsLockForRestart(uint32_t timeoutMs)", "System_Mutex.cpp"),
        ]),
        "/*@UTILS@*/": "\n\n".join([
            one(utils, "static void quiesceSdmmcForRestart()", "System_Utils.cpp"),
            one(utils, "void rebootDevice(const char* reason, const char* auditDetail, uint32_t flushDelayMs)",
                "System_Utils.cpp"),
            one(utils, "static void factoryreset_doRestart(void* /*arg*/)", "System_Utils.cpp"),
        ]),
        "/*@EVENTS@*/": "\n\n".join([
            one(hal, "void deliver(RadioHalEvent type, uint32_t value, uint32_t value2 = 0)", "HAL_Radio.cpp"),
            one(hal, "void hostedEventHandler(void*, esp_event_base_t base, int32_t id, void* data)",
                "HAL_Radio.cpp"),
            one(hal, "esp_err_t registerEvents()", "HAL_Radio.cpp"),
        ]),
        "/*@START@*/": "\n\n".join([
            constant(hal, "kBootAttempts"),
            constant(hal, "kSlaveReadyBudgetMs"),
            constant(hal, "kSlaveReadyPollMs"),
            one(hal, "bool hostedLinkUp()", "HAL_Radio.cpp"),
            one(hal, "esp_err_t startOnce(bool allowLegacyUpdate)", "HAL_Radio.cpp"),
        ]),
        "/*@SILENCE@*/": "\n\n".join([
            one(hal, "static bool restartLatched()", "HAL_Radio.cpp"),
            one(hal, "static void latchC6EnableLow()", "HAL_Radio.cpp"),
            one(hal, "esp_err_t radioHalHold(bool hold)", "HAL_Radio.cpp"),
            one(hal, "void radioHalSilenceForRestart()", "HAL_Radio.cpp"),
        ]),
        "/*@PREPARE@*/": "\n\n".join([
            one(hal, "void radioHalStopTransport()", "HAL_Radio.cpp"),
            one(hal, "esp_err_t radioHalPrepare()", "HAL_Radio.cpp"),
        ]),
        "/*@COMPANION@*/": "\n\n".join([
            one(companion, "bool submitOp(Op op, ExecReq::DeferredFn fn, void* arg)", "System_RadioCompanion.cpp"),
            one(companion, "void radioCompanionBeforeRestart()", "System_RadioCompanion.cpp"),
            one(companion, "void radioCompanionTick()", "System_RadioCompanion.cpp"),
            one(companion, "bool radioCompanionBlocksRadio(const char** reason)", "System_RadioCompanion.cpp"),
        ]),
    }

    # The gate the harness does not compile: once the silence latched, the HAL
    # starts no transport (an op already running in the companion worker may
    # still call it).
    hal_plain = strip_comments(hal)
    body = extract_block(hal_plain, "static esp_err_t startTransport(bool allowLegacyUpdate)")
    gate = body.find("if (restartLatched()) return")
    require(0 <= gate < body.find("prepareBoard()"), "startTransport refuses once the restart latched the C6")
    # The restart path does not tear Hosted down under live users.
    silence = strip_comments(blocks["/*@SILENCE@*/"])
    for forbidden in ("hostedDeinit", "esp_hosted_deinit", "radioHalStopTransport"):
        require(forbidden not in silence, f"radioHalSilenceForRestart must not call {forbidden}")

    harness = (HERE / "restart_sdmmc_quiesce_harness.cpp").read_text()
    for token, code in blocks.items():
        require(harness.count(token) == 1, token)
        harness = harness.replace(token, code)

    with tempfile.TemporaryDirectory(prefix="hw1-restart-sdmmc-quiesce-") as tmp:
        unit = Path(tmp) / "test" / "host" / "harness.cpp"
        unit.parent.mkdir(parents=True)
        # The harness includes the core headers as ../../X.h from test/host.
        for header in ("System_RestartQuiesceCore.h", "Radio_CompanionCore.h"):
            shutil.copy(src / header, Path(tmp) / header)
        exe = Path(tmp) / "test_bin"
        unit.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-Wno-unused-parameter", "-Wno-unused-const-variable", str(unit), "-o", str(exe)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        run = subprocess.run([str(exe)])
    sys.exit(run.returncode)


if __name__ == "__main__":
    main()
