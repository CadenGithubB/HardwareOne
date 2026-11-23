#!/usr/bin/env python3
"""The bond file routes start a request and are polled; no handler waits for the peer.

/api/bond/fs/list, /stat and /get used to send an ESP-NOW request and wait up
to 6 s on a semaphore inside the handler, on the one esp_http_server task.
This test compiles the shipping handlers, reply callbacks and bridges of
WebPage_Bond.cpp (from the bridge structs to the end of handleBondFsGet) with
the real WebBondFsPollCore.h, System_ArgParse.h, System_ESPNow_AnswerCore.h
and the packed FS structs of System_ESPNow_Wire.h, and runs them against
doubles of esp_http_server, the FreeRTOS mutex and the protocol module:

  - a start returns {success:true,pending:true,id} while the peer has not
    answered; the answer (the same JSON as before) comes with a poll, once;
  - the bridge mutex is the only lock, no take waits longer than 100 ms, and
    it is never held across the ESP-NOW send, fsListCancel or a socket write;
  - a reply that arrives before the send call returns is kept; a late reply
    of the previous request is not taken for the new one's answer;
  - busy, send failed, not bonded, the 7 s timeout with one fsListCancel, the
    protocol's own io_error, an answer nobody collects (dropped by the next
    start after 2 s), a malformed or foreign id (never a start);
  - the admin gate holds on the start and on every poll of /list and /get,
    and an id of one route is unknown to the others;
  - the state machine on its own, including millis() and id wrap.

Source contracts: the region creates no binary semaphore and has no
unbounded or multi-second wait; each send call stays in its handler after
the admin gate (test_espnow_fs_transcript_privacy.py reads the same text);
the three bridges are PSRAM.

Not covered: the real httpd task, the real ESP-NOW send (which can itself
block up to about 4 s when the transmit queue is congested), task timing
between the callback and the handler, or the page script
(tools/webui/tests/test_bond_fs_poll_page.py).
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

REGION_START = "#if ENABLE_BONDED_MODE  // the bridge and the three handlers"
REGION_END = "#endif // ENABLE_BONDED_MODE\n\n// =============================================================================\n// Register Handlers"


def production_region(bond: str) -> str:
    require(bond.count(REGION_START) == 1 and bond.count(REGION_END) == 1,
            "WebPage_Bond.cpp: the bridge/handler region markers")
    start = bond.index(REGION_START) + len(REGION_START)
    start = bond.index("\n", start) + 1
    return bond[start:bond.index(REGION_END)]


def strip_comments(code: str) -> str:
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


def source_contracts(bond: str, region: str) -> None:
    code = strip_comments(region)
    for forbidden in ("xSemaphoreCreateBinary", "portMAX_DELAY", "pdMS_TO_TICKS(6000)",
                      "vTaskDelay", "delay("):
        require(forbidden not in code, f"the bond fs handlers must not use {forbidden}")
    waits = set(re.findall(r"pdMS_TO_TICKS\((\d+)\)", code))
    require(waits == {"100", "20"}, f"mutex waits are 100 ms (handler) and 20 ms (callback): {waits}")
    require(code.count("xSemaphoreTake(") == 1, "one take site: the scoped lock")
    require(code.count("xSemaphoreCreateMutex()") == 1, "one mutex for the three bridges")

    # The sends stay in their handlers, after the admin gate (all three are admin-only).
    for handler, send, gated in (("handleBondFsList", "fsListSendRequest(", True),
                                 ("handleBondFsStat", "fsStatSendRequest(", True),
                                 ("handleBondFsGet", "fsGetSendRequest(", True)):
        body = extract_block(bond, f"static esp_err_t {handler}(httpd_req_t* req)")
        require(code.count(send) == 1 and body.count(send) == 1, f"{send} is called once, in {handler}")
        require(body.count("bond_fs_poll::poll(") == 1 and body.count("bond_fs_poll::begin(") == 1,
                f"{handler}: one poll branch and one start")
        if gated:
            gate = body.index("hasAdminPrivilege(ctx)")
            require(gate < body.index("bond_fs_poll::poll(") and gate < body.index(send),
                    f"{handler}: the admin gate comes before the poll and the start")
        # The handler holds the lock in scoped blocks only; the send is outside them.
        before_send = body[:body.index(send)]
        require(before_send.count("{\n    WebFsLock lock(") >= 1 and
                before_send.rstrip().endswith("uint32_t reqId ="),
                f"{handler}: the send is a statement of its own after the locked block")

    # All three bridges live in PSRAM; the mutex pointer is the only internal static.
    for name in ("WebFsBridge     sWebFsBridge", "WebFsStatBridge sWebFsStatBridge",
                 "WebFsGetBridge  sWebFsGetBridge"):
        require(f"EXT_RAM_BSS_ATTR static {name} = {{}};" in region, f"{name} must be PSRAM")
    statics = re.findall(r"(?m)^static (?!const |esp_err_t |void |bool |WebFsQueryId )[^\n(]*;", code)
    require(statics == ["static SemaphoreHandle_t sWebFsMutex = nullptr;"],
            f"internal statics of the bridge: {statics}")

    # The routes are registered as before.
    for route in ("/api/bond/fs/list", "/api/bond/fs/stat", "/api/bond/fs/get"):
        require(len(re.findall(rf'\.uri = "{route}",\s+\.method = HTTP_GET', bond)) == 1,
                f"route {route}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    bond = (COMPONENT / "WebPage_Bond.cpp").read_text()
    region = production_region(bond)
    source_contracts(bond, region)

    wire = (COMPONENT / "System_ESPNow_Wire.h").read_text()
    # The real packed request/reply structs with their static assertions.
    wire_slice = wire[wire.index("struct __attribute__((packed)) V4PayloadFsListReq"):
                      wire.index("struct __attribute__((packed)) V4PayloadSessionUnknown")]
    wire_slice = wire_slice[:wire_slice.rindex("\n// ----")]
    opcode = re.search(r"ESPNOW_V4_TYPE_FS_LIST_RESP\s*=\s*(\d+)", wire)
    require(opcode is not None, "FS_LIST_RESP opcode")
    max_path = re.search(r"^#define FILE_MANAGER_MAX_PATH (\d+)$",
                         (COMPONENT / "System_FileManager.h").read_text(), re.M)
    require(max_path is not None, "FILE_MANAGER_MAX_PATH")

    harness = (HERE / "bond_fs_poll_harness.cpp").read_text()
    for marker, replacement in (
            ("// INSERT_OPCODE", f"constexpr uint8_t ESPNOW_V4_TYPE_FS_LIST_RESP = {opcode.group(1)};"),
            ("// INSERT_WIRE", wire_slice),
            ("INSERT_MAX_PATH", max_path.group(1)),
            ("// INSERT_PRODUCTION", region)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)

    with tempfile.TemporaryDirectory(prefix="hw1-bond-fs-poll-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-I", str(COMPONENT), str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        subprocess.run(command, check=True, timeout=300)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
