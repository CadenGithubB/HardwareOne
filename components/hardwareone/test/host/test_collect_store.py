#!/usr/bin/env python3
"""Execute the collector's SD store (file catch-up, package 5) on the shared fake VFS.

The harness compiles System_CollectStore.cpp between its COLLECT_STORE_HOST
markers unchanged, with the pure cores it uses (System_CollectCore.h,
System_ESPNow_CatchupCore.h), the real FsLockGuard and vendored ArduinoJson,
against fake_vfs.h. StagedWrite is a host double with the production
contract; SHA-256 is CommonCrypto.

It checks:
  * index.json round trip: schema 1, the MAC, the ledger id, UTC times,
    segments named from ids, gaps, elsewhere, the digest; written through the
    PSRAM document and a staged file;
  * a window is flushed to its segment before the file records it (the next
    READ may then claim storedBefore), .raw bytes and .sealed records byte by
    byte;
  * recovery: a .raw length beats the index, a .sealed torn tail is cut, a
    missing segment is refetched, a shorter segment is closed;
  * names only from ids: a hostile device name ("../../system/users.json",
    a backslash, control bytes) or a tampered index never reaches a path;
  * the quota: retired segments oldest first, then the oldest closed file
    (kept in the index as evicted), never the file being fetched;
  * a new pairing moves files/ to files.prev-<UTC stamp>; a bad index is kept
    as index.json.bad;
  * index-archive.jsonl: one JSON line per retired file or rolled segment;
  * write failures: a short write keeps what landed and closes the segment, a
    failed flush records nothing, a stale file is never appended to;
  * the digest of a held range, with the card let go between 16 KB steps;
  * every card operation under the filesystem lock, no handle left open.
It also checks the source: no heap (new/malloc/std::vector/String members),
JSON only through PSRAM_JSON_DOC, no System_ESPNow dependency in the store;
in System_Collect.cpp the receive handlers and the espnow_task tick never
touch the card, the main-loop job never sends, every table is PSRAM (only the
job spinlock is internal) and the lock is a PSRAM mutex.
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
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"
BEGIN = "// COLLECT_STORE_HOST_BEGIN"
END = "// COLLECT_STORE_HOST_END"


def store_body(source: str) -> str:
    require(source.count(BEGIN) == 1 and source.count(END) == 1, "one COLLECT_STORE_HOST section")
    return source[source.index(BEGIN):source.index(END)]


def fs_guard() -> str:
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    guard = extract_block((COMPONENT / "System_Mutex.h").read_text(), "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    return guard


def source_contract(source: str, header: str, core: str) -> None:
    body = store_body(source)
    code = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', re.sub(r"//[^\n]*", "", body))
    for banned, why in ((r"\bnew\s+[A-Za-z_]", "plain new"), (r"\bmalloc\s*\(", "malloc"),
                        (r"std::vector", "std::vector"), (r"\bcalloc\s*\(", "calloc"),
                        (r"\bString\s+[a-z_]\w*\s*;", "a String member or local")):
        require(not re.search(banned, code), f"System_CollectStore.cpp: no {why}")
    require(not re.search(r"\bJsonDocument\s+\w+\s*[;(]", code),
            "System_CollectStore.cpp: JSON documents only through PSRAM_JSON_DOC")
    require("PSRAM_JSON_DOC(doc)" in code, "System_CollectStore.cpp uses PSRAM_JSON_DOC")
    require("EXT_RAM_BSS_ATTR uint8_t sIo" in code, "the I/O buffer is a PSRAM static")
    require("System_ESPNow.h" not in source and "espnowtx" not in code,
            "the store has no ESP-NOW dependency (it runs on the main loop)")
    require("VFS::Scopes::COLLECT_SD" in code, "every card access is confined to Scopes::COLLECT_SD")
    # The device's file name is only ever a JSON value.
    for call in re.findall(r"(?:joinPath|segmentPath|snprintf)\([^;]*;", code):
        require(".name" not in call or "sCand" in call, f"a device name never builds a path: {call.strip()}")
    require("Arduino.h" not in header and "System_VFS.h" not in header,
            "System_CollectStore.h stays free of Arduino and the VFS")
    includes = re.findall(r'#include\s+[<"]([^>"]+)[>"]', core)
    require(sorted(includes) == ["System_ESPNow_CatchupCore.h", "stddef.h", "stdint.h", "string.h"],
            f"System_CollectCore.h is pure: {includes}")


def glue_contract(glue: str) -> None:
    """System_Collect.cpp: the radio side never touches the card, the card side never sends,
    every table is PSRAM, and nothing allocates."""
    code = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', re.sub(r"//[^\n]*", "", glue))
    for banned, why in ((r"\bnew\s+[A-Za-z_]", "plain new"), (r"\bmalloc\s*\(", "malloc"),
                        (r"std::vector", "std::vector"), (r"\bJsonDocument\b", "a JSON document"),
                        (r"xTaskCreate", "a new task")):
        require(not re.search(banned, code), f"System_Collect.cpp: no {why}")
    radio = ("V4Verdict v4hCatchupListResp(const V4RxCtx& ctx)", "V4Verdict v4hCatchupReadResp(const V4RxCtx& ctx)",
             "void tickLocked(uint32_t now)", "void stepListing(uint32_t now)", "void stepFetching(uint32_t now)",
             "void finishWindow(uint32_t now)", "void handleReadStatus(uint32_t now)", "void startNext(uint32_t now)",
             "void refreshPeers(uint32_t now)", "void jobFinished(uint32_t now)", "bool sendList(uint32_t now)",
             "bool sendRead(uint32_t now)", "void collectTick(uint32_t nowMs)")
    for marker in radio:
        body = extract_block(glue, marker)
        require("collectstore::" not in body and "openGuarded" not in body and "FsLockGuard" not in body,
                f"{marker}: espnow_task never touches the card")
    for marker in ("V4Verdict v4hCatchupListResp(const V4RxCtx& ctx)", "V4Verdict v4hCatchupReadResp(const V4RxCtx& ctx)"):
        body = extract_block(glue, marker)
        require("xSemaphoreTake" not in body and "sendAead" not in body, f"{marker}: copies only, no lock, no send")
    for marker in ("void runJob()", "void collectWriteTick()"):
        body = extract_block(glue, marker)
        require("sendAead" not in body, f"{marker}: the main loop never sends")
    statics = re.findall(r"^(?!EXT_RAM_BSS_ATTR)(?:static\s+)?[\w:<>]+\s+(s[A-Z]\w*)\s*(?:\[[^\]]*\])*\s*(?:=|;)",
                         code, re.M)
    require(statics == ["sJobMux"], f"System_Collect.cpp: only the job spinlock is internal RAM: {statics}")
    require("xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM" in code, "the collector's lock is a PSRAM mutex")
    # Q8 claims only for bytes the saved index can recover (robustness review
    # 2026-11-19): a Write that opened a segment saves index.json, and the
    # next READ may carry storedBefore only when nothing new is unsaved.
    run = extract_block(glue, "void runJob()")
    require("if (r.opened) sUnsavedSegment = 1;" in run and "sJob.claimSafe = !sUnsavedSegment && !r.ioError;" in run,
            "runJob: a new segment is named in a saved index before a claim")
    finished = extract_block(glue, "void jobFinished(uint32_t now)")
    require("sClient.flushed = sJob.claimSafe &&" in finished, "jobFinished: storedBefore waits for a safe index")
    require("sJob.planFull = true" in extract_block(glue, "bool ensureFilesLoaded(uint32_t epoch)"),
            "a fresh or unreadable index plans as full: this hub's own claims are not taken as the other master's")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (COMPONENT / "System_CollectStore.cpp").read_text()
    harness = (HERE / "collect_store_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_FS_GUARD", fs_guard()), ("// INSERT_STORE", store_body(source))):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-collect-store-") as temporary:
        directory = Path(temporary)
        stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
        (directory / "Arduino.h").write_text(stub)
        unit = directory / "collect_store.cpp"
        unit.write_text(harness)
        binary = directory / "collect_store"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-deprecated-declarations",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(HERE), "-I", str(COMPONENT), "-I", str(JSON_INCLUDE),
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=240)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)
    source_contract(source, (COMPONENT / "System_CollectStore.h").read_text(),
                    (COMPONENT / "System_CollectCore.h").read_text())
    glue_contract((COMPONENT / "System_Collect.cpp").read_text())
    print("collect store: source contracts hold (store, core, collector glue)")


if __name__ == "__main__":
    main()
