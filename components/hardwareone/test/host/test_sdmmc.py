#!/usr/bin/env python3
"""Compile/run the real SDMMC lifecycle with fault-injected SDK calls."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
host = Path(__file__).resolve().parent
component = host.parent.parent
with tempfile.TemporaryDirectory(prefix="hw1-sdmmc-") as tmp:
    exe = Path(tmp) / "lifecycle"
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for board_flags in ([], ["-DTEST_WAVESHARE=1"]):
        subprocess.run([args.cxx, *flags, *board_flags, "-I" + str(host / "sdmmc_stubs"),
                        str(host / "test_sdmmc_lifecycle.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    # A disabled card must compile to an empty backend even if the board
    # requests SDMMC. No Arduino/SDK include path is supplied on purpose.
    subprocess.run([args.cxx, *flags, "-DSYSTEM_BUILDCONFIG_H",
                    "-DENABLE_SDMMC_CARD=1", "-DENABLE_SD_CARD=0", "-c",
                    str(component / "HAL_SDCard.cpp"),
                    "-o", str(Path(tmp) / "disabled.o")], check=True)
# UP-06: the card watch's glue in System_VFS.cpp (the policy and the driver
# calls run in test_sdmmc_lifecycle.cpp above; the lifecycle notes run in
# sd_in_use_harness.cpp through test_video_completion.py).
import re
import sys
sys.path.insert(0, str(host))
from test_web_batch_handlers import extract_block  # noqa: E402


def code_of(text):
    return re.sub(r"//[^\n]*", "", text)


vfs = (component / "System_VFS.cpp").read_text()
assert ("#if ENABLE_SD_CARD && ENABLE_SDMMC_CARD && defined(SD_MMC_DETECT_PIN) && (SD_MMC_DETECT_PIN >= 0)\n"
        "#define HW1_SD_CARD_WATCH 1\n#else\n#define HW1_SD_CARD_WATCH 0\n#endif") in vfs, \
    "the watch is compiled only for an SDMMC slot whose board defines a detect pin"
pins = {path.name: re.search(r"(?m)^#define SD_MMC_DETECT_PIN\s+(-?\d+)", path.read_text())
        for path in component.glob("System_Board_*.h")}
with_switch = sorted(name for name, match in pins.items() if match and int(match.group(1)) >= 0)
assert with_switch == ["System_Board_P4X_EYE.h"], with_switch  # a new board with a switch gets the watch: check it on the bench
tick = code_of(extract_block(vfs, "void sdWatchTick()"))
for banned in ("FsLockGuard", "isSdAvailable(", "isSdWritable(", "remountSd(", "tryMountSd(", "SD.begin(", "SD.end(",
               "sdInUseOwner(", "delay("):
    assert banned not in tick, f"the loop tick must not use {banned}"
assert tick.index("SD.cardPresent()") < tick.index("sdwatch::sampleDue(gSdWatch, now)"), "the switch is read on every pass"
assert "submitDeferredToCmdExec(sdWatchMountJob, nullptr)" in tick, "the mount runs on cmd_exec"
assert tick.index("SD.noteRemoved();") < tick.index("systemEventPost(SYSEVT_SD_UNMOUNTED);"), "a removal is latched, then reported"
assert tick.count("systemEventPost(") == 1
job = code_of(extract_block(vfs, "static void sdWatchMountJob("))
for guard in ("sdInUseOwner() != nullptr", "micRecordingBusy()", "sttAnySessionActive()", "cappedLogsRestartAnnounced()",
              "sdwatch::mountStillWanted(gSdWatch)"):
    assert guard in job and job.index(guard) < job.index("remountSd()"), f"the mount job checks {guard} before mounting"
assert "sdwatch::mountSkipped(gSdWatch, millis());" in job and "sdwatch::mountResult(gSdWatch, ok, millis());" in job
for signature in ("bool unmountSd()", "bool remountSd()", "bool formatSd()"):
    body = code_of(extract_block(vfs, signature))
    assert "sdReleaseHandlesBeforeUnmount();" in body, signature
    first_driver = min(i for i in (body.find("SD.end()"), body.find("SD.format()"), body.find("spiTeardown();")) if i >= 0)
    assert body.index("sdReleaseHandlesBeforeUnmount();") < first_driver, f"{signature}: handles close before the driver"
unmount = code_of(extract_block(vfs, "bool unmountSd()"))
assert "if (!removalReported) systemEventPost(SYSEVT_SD_UNMOUNTED);" in unmount and unmount.count("systemEventPost(") == 1
release = extract_block(vfs, "static void sdReleaseHandlesBeforeUnmount()")
assert "systemLogReleaseSdHandle();" in release
debug = (component / "System_Debug.cpp").read_text()
handle = code_of(extract_block(debug, "void systemLogReleaseSdHandle()"))
# B11: the file the handle is on (gSystemLogActivePath) is the card twin while
# a card takes log lines, whatever path the session was started with.
assert 'gSystemLogActivePath.startsWith("/sd/")' in handle and "systemLogCloseHandleLocked();" in handle
# The close verifies a card file first (its flushed size) and then closes it.
closing = code_of(extract_block(debug, "static void systemLogCloseHandleLocked()"))
assert "systemLogCardSettleLocked()" in closing and "gSystemLogFile.close();" in closing
assert closing.index("systemLogCardSettleLocked()") < closing.index("gSystemLogFile.close();")
# Its callers (unmountSd, remountSd, formatSd) hold the filesystem lock. The
# guard is reentrant and releases only what it took; fsUnlock() gives the
# mutex whenever the task holds it, so the pair would drop the caller's lock
# before SD.end() / SD.format().
assert 'FsLockGuard guard("debug.log.sd_release");' in handle, "the handle release uses the reentrant guard"
assert "fsLock(" not in handle and "fsUnlock(" not in handle, "the handle release must not give the caller's lock away"
loop = (component / "HardwareOne.cpp").read_text()
assert loop.count("VFS::sdWatchTick();") == 1, "one tick line in the main loop"
assert "void sdWatchTick();" in (component / "System_VFS.h").read_text()
hal = (component / "HAL_SDCard.h").read_text()
assert "bool removalLatched() const { return card_ != nullptr && removed_; }" in hal
assert "void noteRemoved() { if (card_ != nullptr) removed_ = true; }" in hal
print("SD card watch: tick, mount job and lifecycle notes source contract passed")

# SPI slots without a detect pin (XIAO): an empty slot used to run 3 attempts x
# 4 rungs of SD.begin() under the FS lock (~13 s). The first rung still runs as
# before; only after it fails does a raw CMD0 decide whether the rest runs.
spi_guard = "#if ENABLE_SD_CARD && !ENABLE_SDMMC_CARD && defined(SD_CS_PIN)\n"
helpers_at = vfs.index(spi_guard + "// Raw CMD0 (GO_IDLE_STATE)")
assert helpers_at < vfs.index("static bool tryMountSd()"), "the CMD0 helpers are SPI-only and precede tryMountSd"
cmd0 = code_of(extract_block(vfs, "static uint8_t sdSendCmd0("))
assert "{0x40, 0x00, 0x00, 0x00, 0x00, 0x95}" in cmd0
assert cmd0.rindex("digitalWrite(cs, HIGH);") < cmd0.index("SPI.endTransaction();"), "CS is released before the transaction ends"
presence = code_of(extract_block(vfs, "static bool sdCardAnswersCmd0()"))
assert "check < 2" in presence and "if (check) delay(250);" in presence, "one re-check, 250 ms later"
assert "sdSendCmd0(SD_CS_PIN, 400000)" in presence and "(r1 & 0x80) == 0" in presence
for body in (cmd0, presence):
    for banned in ("fsLock(", "fsUnlock(", "FsLockGuard", "malloc(", "new ", "ps_", "heap_caps_"):
        assert banned not in body, f"the CMD0 helpers must not use {banned}"
diag = code_of(extract_block(vfs, "static uint8_t testSdPins("))
assert "VFS::sdSendCmd0(cs, 250000, &atByte)" in diag and "0x40" not in diag, "sddiag shares the CMD0 helper"

mount = code_of(extract_block(vfs, "static bool tryMountSd()"))
spi_at = mount.index("#elif ENABLE_SD_CARD && defined(SD_CS_PIN)")
assert mount.index('FsLockGuard guard("VFS.sdMount");') < mount.index("invalidateStatsSnapshot(SDCARD);") < spi_at
assert "fsUnlock(" not in mount and "fsLock(" not in mount, "the FS lock is held for the whole mount"
assert mount.count("sdCardAnswersCmd0()") == 1 and mount.index("sdCardAnswersCmd0()") > spi_at, "presence check is SPI-only"
check_at = mount.index("if (attempt == 0 && freq == frequencies[0] && !sdCardAnswersCmd0()) {")
assert mount.index("SD.begin(SD_CS_PIN, SPI, freq, \"/sd\")") < mount.index("SD.end();") < check_at, \
    "the first rung's SD.begin runs (and is ended) before the presence check"
assert mount.index("sdPowerUpClocks();") < check_at
early = mount[check_at:]
assert early.index("noCardAnswered = true;") < early.index("break;") < early.index("}"), "silence leaves the ladder"
assert "attempt < FULL_ATTEMPTS && !noCardAnswered" in mount, "silence skips the remaining attempts"
assert 'WARN_STORAGEF("[SD] no card answered; skipping mount retries");' in mount
assert mount.count("return false;") == 1 and mount.count("return true;") == 2
tail = mount[mount.index("return false;") - 120:]
assert "gSdMounted = false;" in tail and "gSdWritable = false;" in tail, "silence returns through the shared failure path"
print("SD SPI no-card early exit source contract passed")

# sddiag's raw CMD0 resets a mounted card (writes then fail with "Check status
# failed"), so the SPI branch unmounts through the sdunmount path, runs the pin
# test and mounts again only if the card was mounted, all under one FS lock.
sddiag = code_of(extract_block(vfs, "static const char* cmd_sddiag("))
spi_diag = sddiag[sddiag.index("#elif !ENABLE_SD_CARD"):]
spi_diag = spi_diag[spi_diag.index("#else"):]
assert "fsUnlock(" not in sddiag and "fsLock(" not in sddiag, "sddiag never gives the FS lock away"
order = [spi_diag.index(s) for s in (
    'FsLockGuard guard("VFS.sdDiag");',
    "wasMounted = VFS::gSdMounted;",
    "if (const char* owner = VFS::sdInUseOwner()) {",
    "if (!VFS::unmountSd()) {",
    "testSdPins(SD_CS_PIN,",
    "if (wasMounted) remounted = VFS::remountSd();",
    "broadcastMultilineReport(buf);",
)]
assert order == sorted(order), "lock, mounted check, in-use refusal, shared unmount, pin test, remount, then broadcast"
assert spi_diag.count("testSdPins(") == 3 and spi_diag.count("VFS::remountSd()") == 1
assert "tryMountSd(" not in spi_diag and "SD.end(" not in spi_diag and "SD.begin(" not in spi_diag, \
    "sddiag uses the shared unmount/mount paths"
refusal = spi_diag[spi_diag.index("if (wasMounted) {"):spi_diag.index("testSdPins(SD_CS_PIN,")]
assert refusal.count("return buf;") == 2 and refusal.count("sddiag was not run") == 2, \
    "an in-use card or a refused unmount returns before the pin test"
# The lock scope closes before the broadcast so other tasks are not held up by output.
assert spi_diag.rindex("}", 0, spi_diag.index("broadcastMultilineReport(buf);")) > spi_diag.index("if (r1 == 0xFF) {")
assert '"Remounted /sd: OK"' in spi_diag and '"Remount failed: run \'sdmount\'' in spi_diag
assert "if (!wasMounted) return \"sddiag complete (full report sent to this session's output)\";" in spi_diag
print("sddiag mounted-card unmount/remount source contract passed")
print("SDMMC lifecycle, failure cleanup, removal, format, card watch and disabled-build tests passed")
