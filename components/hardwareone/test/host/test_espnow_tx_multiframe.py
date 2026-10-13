#!/usr/bin/env python3
"""Run the production espnow_tx dispatcher and STREAM line sender on the host.

Unit 1 slices the whole `espnowtx` namespace from System_ESPNow_Tx.cpp (queue,
completion pool, submit/submitSync, sendAead/sendAeadSync, the dispatcher task
body and stop()) and compiles it against single-threaded FreeRTOS doubles and a
stubbed radio whose multi-frame path models the per-fragment ACK wait. It checks
that a synchronous payload from cmd_exec over its destination's single-frame cap
(202 B direct, 130 B routed) runs on cmd_exec while fire-and-forget producers
keep draining (no queue drops), that another task's synchronous multi-frame job
(httpd's 144 B FS request to a routed peer) stays dispatcher work, and that
single-frame jobs, the self-deadlock guard, lifecycle refusal, stop() and
failure accounting are unchanged.

Unit 2 extracts sendEspNowStreamMessage, its session helper and session lookup
(and v4_single_frame_cap when the source has it) from System_ESPNow.cpp and
checks that STREAM lines are clamped to one frame for their target, on both the
legacy and the command-session path.

--component / --source-dir point the suite at another copy of the production
sources (e.g. the pre-fix tree); the harness and this script are always read
from this directory. Radio timing, real ACK loss and task stack depth are not
modelled.
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

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402

HARNESS = HERE / "espnow_tx_multiframe_harness.cpp"


def dispatcher_unit(tx_source: str) -> str:
    start_marker, end_marker = "namespace espnowtx {", "}  // namespace espnowtx"
    require(tx_source.count(start_marker) == 1, f"expected one '{start_marker}' in System_ESPNow_Tx.cpp")
    require(tx_source.count(end_marker) == 1, f"expected one '{end_marker}' in System_ESPNow_Tx.cpp")
    start = tx_source.index(start_marker)
    end = tx_source.index(end_marker) + 1  # keep the closing brace
    return tx_source[start:end]


def stream_unit(source: str) -> str:
    pieces = []
    # The single-frame cap owner exists only after the fix; the pre-fix sender
    # does not reference it, so its absence still compiles.
    cap = "uint16_t v4_single_frame_cap(const uint8_t* dst)"
    if cap + " {" in source:
        body = source[source.index(cap + " {"):]
        pieces.append(extract_block(body, cap))
    else:
        print("v4_single_frame_cap absent (pre-fix source)")
    interval = re.search(r"static const unsigned long STREAM_MIN_INTERVAL_MS = \d+;", source)
    require(interval is not None, "STREAM_MIN_INTERVAL_MS definition not found")
    pieces.append(interval.group(0))
    table_start = source.index("struct StreamSession {")
    table_end_marker = "static StreamSession gStreamSessions[MAX_STREAM_SESSIONS];"
    table_end = source.index(table_end_marker, table_start) + len(table_end_marker)
    pieces.append(source[table_start:table_end])
    pieces.append(extract_block(source, "static StreamSession* findStreamSession(uint32_t cmdMsgId)"))
    # The bare signature also matches the forward declaration; take the definition.
    helper = "static bool trySendToStreamSession(uint32_t cmdMsgId, const char* data, size_t len)"
    require(source.count(helper + " {") == 1, "expected one trySendToStreamSession definition")
    pieces.append(extract_block(source[source.index(helper + " {"):], helper))
    sender = "void sendEspNowStreamMessage(const String& message)"
    require(source.count(sender) == 1, "expected one sendEspNowStreamMessage definition")
    pieces.append(extract_block(source, sender))
    return "\n\n".join(pieces)


def wire_contract(wire: str) -> None:
    """The stream unit mirrors these constants; fail if the header drifts."""
    require(re.search(r"#define ESPNOW_V4_MAX_PAYLOAD\s+\(250 - 32\)", wire) is not None,
            "ESPNOW_V4_MAX_PAYLOAD is no longer (250 - 32)")
    require(re.search(r"#define ESPNOW_V4_AEAD_TAG_LEN\s+16\b", wire) is not None,
            "ESPNOW_V4_AEAD_TAG_LEN is no longer 16")
    require(re.search(r"#define ESPNOW_V4_MAX_PLAINTEXT\s+\(ESPNOW_V4_MAX_PAYLOAD - ESPNOW_V4_AEAD_TAG_LEN\)",
                      wire) is not None, "ESPNOW_V4_MAX_PLAINTEXT is no longer MAX_PAYLOAD - AEAD_TAG_LEN")
    require("static_assert(ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT == 130" in wire,
            "ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT is no longer pinned to 130")
    require(re.search(r"ESPNOW_V4_TYPE_STREAM\s*=\s*90,", wire) is not None,
            "ESPNOW_V4_TYPE_STREAM is no longer 90")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    p.add_argument("--sanitize", action="store_true")
    p.add_argument("--component", type=Path, default=HERE.parents[1])
    p.add_argument("--source-dir", type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error("a host C++17 compiler is required")

    def path(name: str) -> Path:
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    wire_contract(path("System_ESPNow_Wire.h").read_text())
    harness = HARNESS.read_text()
    for tag in ("// INSERT_DISPATCHER", "// INSERT_STREAM"):
        require(harness.count(tag) == 1, f"harness must contain {tag} once")
    units = (
        ("dispatcher", "-DHW1_UNIT_DISPATCHER",
         harness.replace("// INSERT_DISPATCHER", dispatcher_unit(path("System_ESPNow_Tx.cpp").read_text()))
                .replace("// INSERT_STREAM", "")),
        ("stream", "-DHW1_UNIT_STREAM",
         harness.replace("// INSERT_DISPATCHER", "")
                .replace("// INSERT_STREAM", stream_unit(path("System_ESPNow.cpp").read_text()))),
    )
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-tx-multiframe-") as tmp:
        work = Path(tmp)
        (work / "freertos").mkdir()
        for header in ("FreeRTOS.h", "task.h"):
            (work / "freertos" / header).write_text("#pragma once\n")
        shutil.copyfile(path("System_ESPNow_Tx.h"), work / "System_ESPNow_Tx.h")
        for label, define, text in units:
            unit, exe = work / f"{label}.cpp", work / label
            unit.write_text(text)
            cmd = [a.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", define,
                   "-I", str(work), str(unit), "-o", str(exe)]
            if a.sanitize:
                cmd[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(cmd, check=True, timeout=180)
            if subprocess.run([str(exe)], timeout=60).returncode:
                failures.append(f"{label} unit reported failures")
    if failures:
        print(f"{len(failures)} failure(s): " + "; ".join(failures), file=sys.stderr)
        sys.exit(1)
    print("espnow_tx_multiframe: all units passed")


if __name__ == "__main__":
    main()
