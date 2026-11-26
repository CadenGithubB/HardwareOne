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
the harness does not compile (startTransport).

Bench 2026-11-08, second run: 7 of 15 reboots still logged "sdmmc_send_cmd
returned 0x107" and "Unrecoverable host sdio state" in the drain, packets the
radio users queued after EN went low; and after `c6restart` the web server
never came back (Wi-Fi and BLE did). The silence must close Hosted's TX at
HardwareOne's one-way fence (esp_hosted_hw1_fence_tx, tools/p4's Hosted patch)
and let the write task flush a full queue before it latches EN, so no CMD53
meets the held C6. It must leave Hosted's transport state up: below TX_ACTIVE,
a Wi-Fi init or BLE open in the window would reset the C6 and rerun SDIO card
init (review 2026-11-08). An op inside the flush is refused like one in the
drain. Source checks cover the Hosted patch (every producer path goes through
the fence, transport_drv_reconfigure still keys on the state). The real snapshotUsers, stopUsers,
restoreUsers, recoverProc, holdProc and releaseProc and the HTTP restore run
over a fake network whose `radiopower off` stops the server and whose
`radiopower on` does not start it: after a c6restart or c6hold off the server
must run again, also after a stop deferred past a web request or a late Wi-Fi
link (main-loop tick, 60 s window).

Bench 2026-11-18 (c6update): the P4 reset the C6 before the new image was
confirmed, so the rollback bootloader returned to the old one, and Wi-Fi did
not come back. The fake network now has the host's WiFi.isConnected() flag
next to the companion's association (esp_wifi_sta_get_ap_info): restoreUsers
must join again when the device was joined before and the companion is not,
whatever the host flag says, and log both radiopower replies. The real
updateProc runs over a fake OTA and running slot: Wi-Fi and the web server
stop before the transfer, the link is rebuilt only after a wait that covers
the bridge's SELF_CONFIRM_MS (read from tools/p4/companion/bridge), a return
to the same slot is reported as a rollback, a link lost mid-transfer is
restarted, and every path that brings the companion back restores Wi-Fi and
the web server. --source-dir points the suite at another copy of the
component (the pre-fix tree fails).
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


def constant(source: str, name: str, where: str = "HAL_Radio.cpp") -> str:
    match = re.search(r"constexpr (?:uint32_t|uint8_t|int|size_t) " + name + r" = [^;]+;", source)
    require(match is not None, f"{where} defines {name}")
    return match.group(0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-dir", "--component", dest="source_dir", type=Path, default=COMPONENT)
    parser.add_argument("--hosted-patch", type=Path, help="default: tools/p4/patches/esp-hosted-2.12.13-hw1.patch "
                        "two levels above --source-dir")
    parser.add_argument("--bridge-source", type=Path, help="default: tools/p4/companion/bridge/"
                        "esp_now_hosted_slave.c two levels above --source-dir")
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
            constant(hal, "kHostedTxFlushMs"),
            one(hal, "static bool restartLatched()", "HAL_Radio.cpp"),
            one(hal, "static void latchC6EnableLow()", "HAL_Radio.cpp"),
            one(hal, "esp_err_t radioHalHold(bool hold)", "HAL_Radio.cpp"),
            one(hal, "void radioHalSilenceForRestart()", "HAL_Radio.cpp"),
        ]),
        "/*@PREPARE@*/": "\n\n".join([
            one(hal, "void radioHalStopTransport()", "HAL_Radio.cpp"),
            one(hal, "esp_err_t radioHalPrepare()", "HAL_Radio.cpp"),
        ]),
        "/*@USERS_TYPE@*/": one(companion, "struct RadioUsers ", "System_RadioCompanion.cpp") + ";",
        "/*@USERS@*/": "\n\n".join(
            [constant(companion, "kHttpRestoreWindowMs", "System_RadioCompanion.cpp")]
            + [one(companion, marker, "System_RadioCompanion.cpp") for marker in (
                "RadioUsers snapshotUsers()",
                "void stopUsers(RadioUsers& users)",
                "void armHttpRestore()",
                "void restartHttpServer()",
                "void httpRestoreProc(void*)",
                "void httpRestoreTick(uint32_t now)",
                "void restoreUsers(const RadioUsers& users)",
                "void recoverProc(void*)",
                "void holdProc(void*)",
                "void releaseProc(void*)",
            )]),
        "/*@UPDATE@*/": "\n\n".join([
            constant(companion, "kUpdateChunk", "System_RadioCompanion.cpp"),
            constant(companion, "kUpdateRebootWaitMs", "System_RadioCompanion.cpp"),
            one(companion, "struct UpdateJob ", "System_RadioCompanion.cpp") + ";",
            one(companion, "void updateProc(void* arg)", "System_RadioCompanion.cpp"),
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
    for forbidden in ("hostedDeinit", "esp_hosted_deinit", "radioHalStopTransport", "set_transport_state"):
        require(forbidden not in silence, f"radioHalSilenceForRestart must not call {forbidden}")
    # The C6 bridge confirms a pending image itself after SELF_CONFIRM_MS; the
    # harness checks updateProc's wait against it.
    bridge = args.bridge_source or src.parent.parent / "tools" / "p4" / "companion" / "bridge" / "esp_now_hosted_slave.c"
    require(bridge.is_file(), f"C6 bridge source at {bridge} (pass --bridge-source)")
    confirm = re.findall(r"^#define SELF_CONFIRM_MS (\d+)\b", bridge.read_text(), re.M)
    require(len(confirm) == 1, "the C6 bridge defines SELF_CONFIRM_MS once")
    blocks["/*@BRIDGE@*/"] = f"constexpr uint32_t kBridgeSelfConfirmMs = {confirm[0]};"
    # The Hosted patch (tools/p4/patches/esp-hosted-2.12.13-hw1.patch, the tree the P4
    # build uses): the fence gates every producer and leaves the state alone.
    patch = args.hosted_patch or src.parent.parent / "tools" / "p4" / "patches" / "esp-hosted-2.12.13-hw1.patch"
    require(patch.is_file(), f"Hosted patch at {patch} (pass --hosted-patch)")
    added = [l[1:].strip() for l in patch.read_text().splitlines() if l.startswith("+") and not l.startswith("+++")]
    removed = [l[1:].strip() for l in patch.read_text().splitlines() if l.startswith("-") and not l.startswith("---")]
    require("void esp_hosted_hw1_fence_tx(void)" in added and "hw1_tx_fenced = 1;" in added,
            "the patch defines the one-way fence")
    require("return is_transport_tx_ready() && !hw1_tx_fenced;" in added, "esp_hosted_hw1_tx_open adds the fence")
    require("uint8_t transport_up = esp_hosted_hw1_tx_open();" in added
            and "uint8_t transport_up = is_transport_tx_ready();" in removed,
            "SDIO esp_hosted_tx (VHCI, serial RPC, private path) checks the fence")
    for iface in ("STA", "AP", "SERIAL"):
        require(f"if (!esp_hosted_hw1_tx_open() || !chan_arr[ESP_{iface}_IF]) {{" in added,
                f"the {iface} wrapper checks the fence")
    fence_lines = " ".join(added)
    require("transport_state =" not in fence_lines and "set_transport_state" not in fence_lines,
            "the patch does not change Hosted's transport state")
    others = [l for l in removed if "is_transport_tx_ready()" in l and "transport_up" not in l and "chan_arr" not in l]
    require(not others, "transport_drv_reconfigure still keys its slave bring-up on the transport state")

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
