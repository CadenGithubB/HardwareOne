#!/usr/bin/env python3
"""`espnowtimestatus` reports the time since the last applied mesh TIME_SYNC.

The reply's "Last sync: N seconds ago" used to subtract a stamp nothing ever
wrote, so it showed the uptime. The production time-sync state, the TIME_SYNC
handler v4h_time_sync and cmd_espnow_timestatus (System_ESPNow.cpp) are
compiled with the production V4PayloadTimeSync and V4RxCtx against a settable
32-bit millis(): before a sync the reply names no sync time; after one it
counts from the moment the sync was applied, not from boot; a later sync
restarts the count; a refused correction and a short payload leave the stamp;
the count is right across the millis() wrap. Source contract: the stamp is
stored before gTimeIsSynced is set, so a reader that sees "synced" has it.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

STATE_START = "// Time synchronization state"
STATE_END = "// Metadata transmission tracking"
HANDLER = "static void v4h_time_sync(const V4RxCtx& ctx)"
COMMAND = "const char* cmd_espnow_timestatus(const String& argsInput)"


def production(component: Path) -> str:
    espnow = (component / "System_ESPNow.cpp").read_text()
    wire = (component / "System_ESPNow_Wire.h").read_text()
    rx = (component / "System_ESPNow_RxCtx.h").read_text()
    for marker in (STATE_START, HANDLER, COMMAND):
        require(espnow.count(marker) == 1, f"one {marker}")
    start = espnow.index(STATE_START)
    state = espnow[start:espnow.index(STATE_END, start)]
    require(state.count("\n") <= 12, "the time sync state is the short block under its comment")
    # The broadcast interval constant beside the state is not part of it.
    state = "\n".join(line for line in state.splitlines() if "TIME_SYNC_INTERVAL" not in line)
    return "\n\n".join([
        extract_block(wire, "struct __attribute__((packed)) V4PayloadTimeSync") + ";",
        extract_block(rx, "struct V4RxCtx") + ";",
        state,
        extract_block(espnow, HANDLER),
        extract_block(espnow, COMMAND),
    ])


def check_sources(component: Path) -> None:
    espnow = (component / "System_ESPNow.cpp").read_text()
    require("gLastTimeSyncMs" not in espnow, "the never-written gLastTimeSyncMs is gone")
    handler = extract_block(espnow, HANDLER)
    require(handler.count("sLastTimeSyncMs.store(") == 1, "the handler stores the stamp once")
    require(handler.index("sLastTimeSyncMs.store(") < handler.index("gTimeIsSynced = true;"),
            "the stamp is stored before gTimeIsSynced is set")
    writers = espnow.count("gTimeIsSynced = true;")
    require(writers == 1, f"one place sets gTimeIsSynced (found {writers}); each needs the stamp")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    harness = (HERE / "espnow_time_status_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-time-status-") as directory:
        source = Path(directory) / "time_status.cpp"
        binary = Path(directory) / "time_status"
        source.write_text(harness.replace("// INSERT_PRODUCTION", production(args.component)))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)
    check_sources(args.component)
    print("espnowtimestatus counts from the last applied TIME_SYNC", flush=True)


if __name__ == "__main__":
    main()
