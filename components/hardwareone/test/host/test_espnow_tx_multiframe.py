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
(and v4_single_frame_cap, streamCmdIdForCurrentTask and espnowStreamOutputWanted
when the source has them) from System_ESPNow.cpp and checks that STREAM lines
are clamped to one frame for their target, on both the legacy and the
command-session path. A9: the session is chosen from the command context
cmd_exec is running (origin ORIGIN_ESPNOW, ctx.id == the request msgId), so with
two requests queued each requester gets only its own output, and a web command
or a deferred job on cmd_exec streams nothing to a queued requester; the
broadcast gate stays open for the legacy startstream sink while a remote command
is in flight. A source check pins the order around it: the per-task validate
drop before the gate, STREAM_BEGIN before submit, STREAM_END before CMD_RESP
before destroyStreamSession, and the executor clearing the context before the
async callback. The pre-A9 source (one global id set at queue time) fails S7-S10.

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


PICK = "static uint32_t streamCmdIdForCurrentTask()"
GATE = "bool espnowStreamOutputWanted()"


def stream_unit(source: str) -> str:
    pieces = []
    if "gCurrentStreamCmdId" in source:
        # Before A9 the sender read one global id, set when a request was queued.
        print("gCurrentStreamCmdId present (pre-A9 source)")
        pieces.append("uint32_t gCurrentStreamCmdId = 0;\n#define HW1_STREAM_GLOBAL_ID 1")
    else:
        require(source.count(PICK + " {") == 1, f"expected one {PICK} definition")
        require(source.count(GATE + " {") == 1, f"expected one {GATE} definition")
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
    if PICK + " {" in source:
        pieces.append(extract_block(source[source.index(PICK + " {"):], PICK))
    sender = "void sendEspNowStreamMessage(const String& message)"
    require(source.count(sender) == 1, "expected one sendEspNowStreamMessage definition")
    pieces.append(extract_block(source, sender))
    if GATE + " {" in source:
        pieces.append(extract_block(source[source.index(GATE + " {"):], GATE))
        pieces.append("#define HW1_STREAM_GATE 1")
    return "\n\n".join(pieces)


def stream_target_contract(read) -> list[str]:
    """A9 (source side): the stream target comes from the running command's
    context, and the surrounding order the session path relies on holds."""
    failures = []
    espnow, debug = read("System_ESPNow.cpp"), read("System_Debug.cpp")
    for name, text in (("System_ESPNow.cpp", espnow), ("System_Debug.cpp", debug)):
        if "gCurrentStreamCmdId" in text:
            failures.append(f"{name} still uses the global gCurrentStreamCmdId")
    core = extract_block(debug, "static void broadcastOutputCore(")
    validate, gate = core.find("currentCommandValidateOnly()"), core.find("espnowStreamOutputWanted()")
    if not 0 <= validate < gate or core.find("sendEspNowStreamMessage(", gate) < 0:
        failures.append("broadcastOutputCore: the per-task validate drop must precede the "
                        "espnowStreamOutputWanted() gate that guards sendEspNowStreamMessage")
    types = read("System_CommandTypes.h")
    context = extract_block(types, "struct CommandContext")
    if not re.search(r"\bCommandOrigin origin\b", context) or not re.search(r"\buint32_t id\b", context) \
            or not re.search(r"\bORIGIN_ESPNOW\b", types):
        failures.append("System_CommandTypes.h: CommandContext.origin / .id or ORIGIN_ESPNOW changed "
                        "(the harness mirrors them)")
    handle = extract_block(espnow, "static void v4_handle_cmd(const uint8_t* srcMac, const char* deviceName, "
                                   "uint32_t msgId, const char* cmdPayload")
    begin, submit = handle.find("ESPNOW_V4_FLAG_STREAM_BEGIN"), handle.find("submitCommandAsync(")
    if not ("cmd.ctx.origin = ORIGIN_ESPNOW;" in handle and "cmd.ctx.id = msgId;" in handle):
        failures.append("v4_handle_cmd no longer queues origin ORIGIN_ESPNOW with ctx.id = msgId")
    if not 0 <= begin < submit:
        failures.append("v4_handle_cmd: STREAM_BEGIN is no longer sent before the command is submitted")
    callback = extract_block(espnow, "static void v4CmdResultCallback(")
    end, resp, destroy = (callback.find("ESPNOW_V4_FLAG_STREAM_END"), callback.find("ESPNOW_V4_TYPE_CMD_RESP"),
                          callback.find("destroyStreamSession("))
    if not 0 <= end < resp < destroy:
        failures.append("v4CmdResultCallback: expected STREAM_END, then CMD_RESP, then destroyStreamSession")
    main_source = read("HardwareOne.cpp")
    task = "static void commandExecTask(void* pv)"  # also forward-declared; take the definition
    require(main_source.count(task + " {") == 1, "expected one commandExecTask definition")
    executor = extract_block(main_source[main_source.index(task + " {"):], task)
    cleared, called = executor.find("clearCurrentCommandContext();"), executor.find("r->asyncCallback(")
    if not 0 <= cleared < called:
        failures.append("commandExecTask must clear the command context before the async callback, "
                        "or lines printed after STREAM_END would still stream")
    return failures


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
    for failure in stream_target_contract(lambda name: path(name).read_text()):
        print(f"FAIL [A9 source] {failure}", file=sys.stderr)
        failures.append(failure)
    if failures:
        print(f"{len(failures)} failure(s): " + "; ".join(failures), file=sys.stderr)
        sys.exit(1)
    print("espnow_tx_multiframe: all units passed")


if __name__ == "__main__":
    main()
