#!/usr/bin/env python3
"""Execute the production log tier resolution (B11, G-39) and the bulk-write admission (FILE-3).

The free-space cache, resolveLogPath and resolveOverflowPath come unchanged
from System_VFS.cpp; the clock, the LittleFS size report, the SD mount and
write-probe answers, the card's free figure and the device key are
host-controlled. B11: a log resolves to its card twin while a card takes
writes, has room above its reserve and the key is there (card present, absent,
full with its hysteresis and once-per-episode warning, in back-off after a
failed append, no key), with one [EVENT] line per switch. G-39: with no such
card a latched resolution is Refused at the flash floor (the reserve is kept),
a mounted but unwritable card is not used, and the latch re-arms when space
returns.

FILE-3: VFS::admitBulkWrite and VFS::peekLittleFsFree come from the same
region and run over the same fake volume: with reserve + N bytes free, N bytes
are admitted and N + 1 refused; a peek never walks the volume, a refresh does
when the figure is stale; a card passes; an invalidation keeps the last
reading. The microphone recorder's size helpers (System_Microphone.cpp) run
too. The source contract holds how the recorder and ESP-NOW receive ask
(the receive side is executed by test_espnow_file_transfer.py).
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


def generate() -> str:
    vfs = (COMPONENT / "System_VFS.cpp").read_text()
    start = vfs.index("static constexpr size_t LOG_OVERFLOW_DEFAULT_RESERVE")
    block = extract_block(vfs, "bool resolveOverflowPath(const char* primaryPath")
    end = vfs.index(block) + len(block)
    production = vfs[start:end]
    require("LogTier resolveLogPath(" in production, "resolveLogPath lives in the overflow region")
    # FILE-3: the admission rule is part of this region, and the microphone
    # recorder's size helpers are pure.
    for symbol in ("size_t peekLittleFsFree()", "bool admitBulkWrite(StorageType tier", "uint32_t bulkWriteRefusals()"):
        require(symbol in production, f"{symbol} lives in the overflow region")
    mic = (COMPONENT / "System_Microphone.cpp").read_text()
    chunk = re.search(r"(?m)^#define RECORDING_CHUNK_SIZE\s+(\d+)$", mic)
    require(chunk is not None, "RECORDING_CHUNK_SIZE is a plain number")
    mic_start = mic.index("static constexpr uint32_t kRecMinAdmitSec")
    mic_end = mic.index("// The limit of the recording being started or running")
    mic_code = f"#define RECORDING_CHUNK_SIZE {chunk.group(1)}\n" + mic[mic_start:mic_end]
    harness = (HERE / "log_overflow_latch_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one production insertion marker")
    require(harness.count("// INSERT_MIC_ADMISSION") == 1, "one microphone insertion marker")
    # B11: the card-first resolution is part of the region, and the card
    # reserve it asks is the production System_LogCapsCore.h.
    for symbol in ("static LogCardUse logCardUse()", "static bool logCardHasRoom()", "static bool logCardKeyReady()",
                   "LogCardStatus logCardStatus()", "void noteLogCardFileFailure(const char* path, const char* hint)",
                   "static bool logCardFileHeld(const char* path)"):
        require(symbol in production, f"{symbol} lives in the overflow region")
    require('#include "System_LogCapsCore.h"' in harness, "the harness compiles the production caps header")
    return harness.replace("// INSERT_PRODUCTION", production).replace("// INSERT_MIC_ADMISSION", mic_code)


def code_of(text: str) -> str:
    return re.sub(r"//[^\n]*", "", text)


def source_contract() -> None:
    """FILE-3: how the two bulk writers ask, and what the admission may touch."""
    vfs = (COMPONENT / "System_VFS.cpp").read_text()
    peek = code_of(extract_block(vfs, "size_t peekLittleFsFree()"))
    for banned in ("refreshLittleFsFreeCached", "FsLockGuard", "esp_littlefs_info", "getStats"):
        require(banned not in peek, f"peekLittleFsFree must not use {banned}")
    admit = code_of(extract_block(vfs, "bool admitBulkWrite(StorageType tier"))
    # A Peek admission (espnow_task) never walks the volume: the refresh is
    # for Refresh only, and the forced reading is behind "not Peek".
    require(admit.count("refreshLittleFsFreeCached") == 1 and
            "if (figure == BulkFigure::Refresh) (void)refreshLittleFsFreeCached();" in admit,
            "only a Refresh admission refreshes up front")
    require(admit.count("bulkFigureResample();") == 1 and
            "if (room < want && figure != BulkFigure::Peek && bulkFigureIsEstimate()) {" in admit and
            admit.index("figure != BulkFigure::Peek && bulkFigureIsEstimate()") < admit.index("bulkFigureResample();"),
            "the volume is read for a caller that may wait, and only on an estimate that is short")
    require("submitDeferredToCmdExec(bulkFigureResampleJob, nullptr)" in admit and
            "figure == BulkFigure::Peek && bulkFigureIsEstimate() && !gBulkWrite.resampleQueued" in admit,
            "a Peek refusal on an estimate asks cmd_exec for the reading, once")
    require("FsLockGuard" not in admit and "noteLittleFsBytesWritten" not in admit, "the admission takes no lock of its own")
    for name in ("void noteBulkWriteTaken(size_t bytes)", "void noteBulkWriteReturned(size_t bytes)",
                 "static bool bulkRoomStored(size_t& room)", "static bool bulkFigureIsEstimate()"):
        body = code_of(extract_block(vfs, name))
        for banned in ("refreshLittleFsFreeCached", "FsLockGuard", "esp_littlefs_info", "getStats"):
            require(banned not in body, f"{name} must not use {banned}")
    # S-2/S-3 are not decided: the refresh and its cadence are as they were.
    # The one addition drops the bulk writers' count when a new reading is taken.
    refresh = extract_block(vfs, "static size_t refreshLittleFsFreeCached()")
    require(refresh.count("gBulkWrite") == 1 and "gBulkWrite.takenBytes = 0;" in refresh and
            "peekLittleFsFree" not in refresh, "the free-space refresh only drops the taken count")
    require(refresh.index("gLogFreeBytesWrittenSinceRefresh = 0;") < refresh.index("gBulkWrite.takenBytes = 0;") <
            refresh.index("publishCapacitySnapshot("), "the count is dropped where the new reading is stored")
    require("size_t getCachedLittleFsFree() { return refreshLittleFsFreeCached(); }" in vfs,
            "getCachedLittleFsFree is unchanged")
    # ESP-NOW receive: accepted on espnow_task with the stored figure only,
    # before anything is torn down; asked again on cmd_exec at the .part open.
    files = (COMPONENT / "System_ESPNow_Files.cpp").read_text()
    allocate = code_of(extract_block(files, "FileTransferSlot* fileSlotsAllocate("))
    require(allocate.count("VFS::admitBulkWrite(") == 1 and
            'VFS::admitBulkWrite(VFS::INTERNAL, ask, "ESP-NOW file receive", VFS::BulkFigure::Peek)' in allocate,
            "the slot allocation asks once, with the stored figure")
    for banned in ("BulkFigure::Refresh", "getCachedLittleFsFree", "FsLockGuard", "VFS::getStats", "esp_littlefs_info"):
        require(banned not in allocate, f"fileSlotsAllocate (espnow_task) must not use {banned}")
    require(allocate.index("VFS::admitBulkWrite(") < allocate.index("releaseLocked(old);") and
            allocate.index("VFS::admitBulkWrite(") < allocate.index("fileSlotsStreamFailLocked(old);"),
            "admission is decided before a transfer is superseded")
    require(allocate.index("setErr(kFileSlotErrStorage);") > allocate.index("VFS::admitBulkWrite("),
            "a refusal reports the storage reason")
    # What a transfer put on flash is counted against the stored figure once
    # it is no longer in flight: each streamed write, and a RAM-path file when
    # its slot is released with every chunk in.
    release = code_of(extract_block(files, "void releaseSlot(FileTransferSlotImpl& s)"))
    require("VFS::noteBulkWriteTaken(s.totalSize);" in release and
            release.index("s.receivedChunks == s.totalChunks && s.receivedBytes == s.totalSize") <
            release.index("VFS::noteBulkWriteTaken(s.totalSize);") < release.index("releaseLocked(s);"),
            "a complete RAM-path transfer is counted when its slot is released")
    require(allocate.count("noteBulkWriteTaken") == 0, "a transfer in flight is counted by its slot, not twice")
    drain = code_of(extract_block(files, "void streamDrainPending("))
    require("else VFS::noteBulkWriteTaken(len);" in drain, "each streamed write is counted")
    require('VFS::admitBulkWrite(VFS::INTERNAL, total, "ESP-NOW file receive",' in drain and
            "VFS::BulkFigure::Refresh" in drain and
            drain.index("VFS::admitBulkWrite(") < drain.index('VFS::openGuarded(partPathCopy, "w"'),
            "the .part open asks again with a fresh figure")
    job = code_of(extract_block(files, "void fileSlotsStreamDrainJob("))
    require("if (s.streamPendCount == 0 || s.streamFailed) { s.streamDrainQueued = false; return; }" in job,
            "the drain job ends on a failed stream instead of spinning")
    header = (COMPONENT / "System_ESPNow_Files.h").read_text()
    require('constexpr const char* kFileSlotErrStorage  = "NO_SPACE";' in header, "the storage reason has a name")
    # The caller answers every allocation error with the existing rejected
    # cancel; no new frame or reason goes on the wire.
    espnow = (COMPONENT / "System_ESPNow.cpp").read_text()
    start = extract_block(espnow, "static V4Verdict v4h_file_start(") if "static V4Verdict v4h_file_start(" in espnow else espnow
    require("FILE_CANCEL_REJECTED" in start, "FILE_START answers an allocation error with FILE_CANCEL_REJECTED")
    # Microphone: asked once before the file exists, with the stored figure
    # (no walk while the speaker waits), and the recording is limited to what
    # is above the reserve instead of being refused for a full 60 s.
    mic = (COMPONENT / "System_Microphone.cpp").read_text()
    begin = code_of(mic[mic.index("static bool startRecordingInternal("):mic.index("taskStackRecord(\"mic_record\"")])
    require(begin.count("VFS::admitBulkWrite(") == 1 and
            "VFS::admitBulkWrite(VFS::getStorageType(recDir), micRecordingBytes(gRecSampleRate, kRecMinAdmitSec)," in begin and
            '"microphone recording", VFS::BulkFigure::RefreshIfShort, &roomBytes, fullBytes)' in begin and
            "const size_t fullBytes = micRecordingBytes(gRecSampleRate, MAX_RECORDING_SEC);" in begin,
            "the recorder asks for five seconds, wants a whole recording, and reads the volume only on a short estimate")
    require("gRecStorageTakenBytes = roomBytes < fullBytes ? roomBytes : fullBytes;" in begin and
            "VFS::noteBulkWriteTaken(gRecStorageTakenBytes);" in begin and
            begin.index("if (roomBytes != SIZE_MAX) {") < begin.index("VFS::noteBulkWriteTaken(gRecStorageTakenBytes);"),
            "the recorder claims the room it may write (nothing for a card or an unknown figure)")
    require("micRecordingSettleStorage(0);" in begin, "a failed start gives the claim back")
    require(begin.index("VFS::admitBulkWrite(") < begin.index('VFS::openGuarded(String(currentRecordingPath), "w"') and
            begin.index("VFS::admitBulkWrite(") < begin.index("VFS::mkdirGuarded(recDir"),
            "the recorder asks before it creates anything")
    require('return failStart("not enough free flash space", kRecStartNoSpace);' in begin and
            "gRecStorageMaxSamples = micStorageSampleLimit(roomBytes);" in begin, "refused through failStart, or limited")
    require(len("not enough free flash space") <= 47, "the failure text fits MicRecordingResult.failure")
    reply = re.search(r'static constexpr char kRecStartNoSpace\[\] =\s*"([^"]+)";', mic)
    require(reply is not None and reply.group(1).startswith("Error: "), "the refusal reply leads with Error:")
    task = code_of(mic)
    require("if (gRecStorageMaxSamples != 0 && gRecStorageMaxSamples < maxSamples) {" in task and
            "maxSamples = gRecStorageMaxSamples;" in task, "the recording task takes the smaller limit")
    require("EXT_RAM_BSS_ATTR static uint32_t gRecStorageMaxSamples;" in mic, "the limit is not in internal RAM")
    require("EXT_RAM_BSS_ATTR static size_t gRecStorageTakenBytes;" in mic, "the claim is not in internal RAM")
    settle = code_of(extract_block(mic, "static void micRecordingSettleStorage(size_t writtenBytes)"))
    require("gRecStorageTakenBytes = 0;" in settle and "VFS::noteBulkWriteReturned(taken - writtenBytes);" in settle,
            "the unused part of the claim is given back once")
    rec_task = code_of(extract_block(mic, "static void recordingTask(void* param)"))
    require(rec_task.count("micRecordingSettleStorage(") == 1 and
            rec_task.index("micRecordingSettleStorage(") < rec_task.index("micRecordingPublishIdle("),
            "the recording task settles its claim before it publishes IDLE")
    require("noteLittleFsBytesWritten" not in mic and "noteLittleFsBytesWritten" not in files,
            "the bulk writers do not drive the free-space refresh (S-2/S-3 undecided)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-log-overflow-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-parameter", "-I", str(COMPONENT),
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f"log overflow latch failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "harness reports its check count")
        print(f"log overflow latch: {count.group(1)} checks passed")
    source_contract()
    print("bulk-write admission: source contract passed")


if __name__ == "__main__":
    main()
