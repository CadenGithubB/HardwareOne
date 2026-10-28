#!/usr/bin/env python3
"""Run the vendored BLE library's service discovery against the R1 wedge.

Splices the Bluedroid BLEClient::getServices() and FreeRTOS::Semaphore (class
and its take/timedWait/give definitions) out of
components/arduino/libraries/BLE/src into ble_service_search_harness.cpp, which
supplies a fake kernel semaphore and a scripted search request.

Static check: the R1 ring connect's "service not found" branch in G2_Ring.cpp
re-runs discovery (getServices) only behind its debug-listing gate, as the
glasses' branch does; on 2026-10-25 the ungated call was the third search on a
dead link and wedged the shared BLE-connect worker until reboot.
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
BLE = COMPONENT.parent / "arduino" / "libraries" / "BLE" / "src"
DEBUG_GATE = "if (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_RING | DEBUG_RING_LIFECYCLE))"


def static_checks() -> None:
    ring = (COMPONENT / "G2_Ring.cpp").read_text()
    marker = 'ERROR_RINGF("Connect FAILED — ring service not found");'
    require(ring.count(marker) == 1, "G2_Ring.cpp: service-not-found branch missing or not unique")
    start = ring.index(marker)
    branch = re.sub(r"//[^\n]*", "", ring[start:ring.index("return false;", start)])
    calls = [m.start() for m in re.finditer(r"getServices\s*\(", branch)]
    gate = branch.find(DEBUG_GATE)
    for call in calls:
        require(gate != -1 and gate < call,
                "G2_Ring.cpp: the service-not-found branch re-runs getServices() outside its debug gate")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    rtos = (BLE / "RTOS.h").read_text()
    freertos = (BLE / "FreeRTOS.cpp").read_text()
    client = (BLE / "BLEClient.cpp").read_text()
    semaphore_class = extract_block(rtos, "class Semaphore") + ";"
    make_mux = re.search(r"^namespace \{\n.*?^\}\n", freertos, re.M | re.S)
    require(make_mux is not None, "FreeRTOS.cpp: semaphore publication mutex missing")
    definitions = [make_mux.group()] + [extract_block(freertos, marker) for marker in (
        "SemaphoreHandle_t FreeRTOS::Semaphore::handle()",
        "bool FreeRTOS::Semaphore::timedWait(",
        "FreeRTOS::Semaphore::~Semaphore()",
        "void FreeRTOS::Semaphore::give()",
        "void FreeRTOS::Semaphore::give(uint32_t value)",
        "bool FreeRTOS::Semaphore::take(const char* owner)",
        "bool FreeRTOS::Semaphore::take(uint32_t timeoutMs, const char* owner)",
    )]
    # The first definition is the Bluedroid one (the NimBLE one follows it).
    get_services = extract_block(
        client, "std::map<std::string, BLERemoteService *> *BLEClient::getServices()")

    harness = (HERE / "ble_service_search_harness.cpp").read_text()
    for marker, body in (("// INSERT_SEMAPHORE_CLASS", semaphore_class),
                         ("// INSERT_SEMAPHORE_DEFINITIONS", "\n\n".join(definitions)),
                         ("// INSERT_GETSERVICES", get_services)):
        require(harness.count(marker) == 1, f"harness marker missing/duplicated: {marker}")
        harness = harness.replace(marker, body)

    with tempfile.TemporaryDirectory(prefix="hw1-ble-search-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text("#include <atomic>\n#include <cstddef>\n" + harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    static_checks()
    print("ble_service_search: discovery never waits forever; the ring listing is gated")


if __name__ == "__main__":
    main()
