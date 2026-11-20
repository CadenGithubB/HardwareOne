#!/usr/bin/env python3
"""Run the file catch-up server (System_ESPNow_Catchup.cpp) end to end on the host.

Compiles the server's CATCHUP_SERVE_COMMON and CATCHUP_SERVE_HOST regions with
the real ledger store (System_CatchupLedger.cpp, CATCHUP_LEDGER_HOST), the real
pure core, the real ArduinoJson and the shared fake VFS, with doubles for
ESP-NOW sessions, the TX queue, the battery, the capture key's sealing, the
clock and SHA-256 (espnow_catchup_serve_harness.cpp). Checks: the reader rule
(refused once per peer session, then silent; a backup served as the primary;
setting off, link, collect caps); busy answers (not settled, battery floor only
where measured, slot taken, full LIST within 60 s, deep TX queue, cmd_exec
queue full); LIST pages (withheld unsealed health, sealed flags, nextFileId
chaining at a 64-byte maxFrame, kind mask); READ windows (182/126 per frame,
20 ms spacing, the TX-depth wait, contiguous offsets, last/atEnd flags, the
empty at-end frame, growth after the LIST, past_end, gone, another ledger,
trimmed with the new start, the Q8 delivery claim raising only); sealed on
send (154 per frame, the AAD per frame, no plaintext on the air, digest
unsupported, seal failure and no key fail closed, a rotation between LIST and
READ followed by id); the digest (SHA-256 of the range, once per file per
session, the FS lock released between blocks); a send that keeps failing
abandons the window, a lost session frees the slot, one requester at a time.

Source rules: the handlers and the job never touch VFS (the ledger owns every
file access); only the receive path and the tick send; the job never sends or
sleeps for pacing (the digest yields between blocks); the server's buffers are
EXT_RAM_BSS statics with one spinlock; no heap, no String member, no task;
the build without ENABLE_ESPNOW_SEND answers unsupported.

Not covered: the real ESP-NOW stack, relayed delivery (ttl 1 to a routed
master), FreeRTOS timing, and the commands' text (bench).
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
from test_catchup_ledger import code_only, header_text, store_text

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def region(text: str, name: str) -> str:
    begin, end = f"\n// {name}_BEGIN", f"\n// {name}_END\n"
    require(text.count(begin) == 1 and text.count(end) == 1, f"one {name} region")
    start = text.index(begin)
    return text[text.index("\n", start + 1):text.index(end)]


def source_rules(server: str) -> list[str]:
    failures = []
    code = code_only(server)
    for banned in ("VFS::", "FsLockGuard", "File ", "openGuarded"):
        if banned in code:
            failures.append(f"the server uses {banned}: file access belongs to System_CatchupLedger")
    for banned in (r"\bmalloc\(", r"\bnew\s+[A-Za-z_]", r"std::vector", r"\bps_alloc\(", r"xTaskCreate"):
        if re.search(banned, code):
            failures.append(f"the server uses {banned}")
    host = code_only(region(server, "CATCHUP_SERVE_HOST"))
    for fn in ("uint8_t buildList(", "uint8_t buildRead(", "void catchupServeJob(void*) {", "Fetch fetch(",
               "uint8_t buildDigest("):
        body = extract_block(host, fn)
        if "csSend(" in body or "sendAead" in body:
            failures.append(f"{fn} sends: only the receive path and the tick send")
        if "vTaskDelay" in body and fn != "uint8_t buildDigest(":
            failures.append(f"{fn} sleeps: pacing belongs to the tick")
    digest = extract_block(host, "uint8_t buildDigest(")
    if digest.count("vTaskDelay(1)") != 1:
        failures.append("the digest yields once per block")
    sends = [m.start() for m in re.finditer(r"csSend\(", host)]
    allowed = [extract_block(host, "void intake("), extract_block(host, "void catchupServeTick(")]
    for at in sends:
        line = host[host.rfind("\n", 0, at) + 1:host.find("\n", at)]
        if not any(line in body for body in allowed):
            failures.append(f"csSend outside the intake and the tick: {line.strip()}")
    for name in ("sSlot", "sOut", "sRead", "sEntries", "sCur", "sPacer", "sStats"):
        if not re.search(rf"EXT_RAM_BSS_ATTR [^;\n]*\b{name}\b", host):
            failures.append(f"{name} is not an EXT_RAM_BSS static")
    if len(re.findall(r"portMUX_TYPE \w+ = portMUX_INITIALIZER_UNLOCKED", host)) != 1:
        failures.append("the server keeps exactly one spinlock")
    if re.search(r"^\s+String \w+;", host, re.M):
        failures.append("a String member in the server")
    read = extract_block(host, "uint8_t buildRead(")
    seal_at = read.find("captureCryptoSealChunk(")
    if seal_at < 0 or "kStNotReady" not in read[seal_at:read.find("memcpy(", seal_at)]:
        failures.append("a failed seal must answer not_ready before any plaintext copy")
    unsupported = server[server.index("#if !ENABLE_ESPNOW_SEND"):server.index("#else  // ENABLE_ESPNOW_SEND")]
    if "kStUnsupported" not in unsupported or "sNotBuiltDropped" not in unsupported:
        failures.append("the build without ENABLE_ESPNOW_SEND answers unsupported once, then counts and drops")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    server = (COMPONENT / "System_ESPNow_Catchup.cpp").read_text()
    failures = source_rules(server)
    serve = region(server, "CATCHUP_SERVE_COMMON") + "\n" + region(server, "CATCHUP_SERVE_HOST")
    harness = (HERE / "espnow_catchup_serve_harness.cpp").read_text()
    for marker in ("// INSERT_LEDGER_HEADER", "// INSERT_LEDGER", "// INSERT_SERVE"):
        require(harness.count(marker + "\n") == 1, f"one insertion marker: {marker}")
    unit_text = (harness.replace("// INSERT_LEDGER_HEADER\n", header_text() + "\n")
                 .replace("// INSERT_LEDGER\n", store_text() + "\n")
                 .replace("// INSERT_SERVE\n", serve + "\n"))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
             "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
             "-I", str(HERE / "web_batch_stubs"), "-I", str(HERE), "-I", str(COMPONENT),
             "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-catchup-serve-") as temp:
        work = Path(temp)
        unit, binary = work / "catchup_serve.cpp", work / "catchup_serve"
        unit.write_text(unit_text)
        if subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], timeout=240).returncode != 0:
            failures.append("the server harness does not compile")
        else:
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            print((result.stdout + result.stderr).strip())
            if result.returncode != 0:
                failures.append("the server harness failed")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
