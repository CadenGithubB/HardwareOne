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
assert 'gSystemLogPath.startsWith("/sd/")' in handle and "gSystemLogFile.close();" in handle
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
print("SDMMC lifecycle, failure cleanup, removal, format, card watch and disabled-build tests passed")
