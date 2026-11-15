#!/usr/bin/env python3
"""A command's identity is moved where its source dies, and the serial mirror is not a copy.

A command's AuthContext (five Strings) was copied from the transport's local
context into the Command although the local died right after (the serial
console, the UART link and runUnifiedSystemCommand); it is moved now. And
deliverCommandResult copied the whole CommandContext only to clear the
serial route bit for the mirror after the console write; broadcastOutput's
body now takes the route mask as an argument (broadcastOutputRouted), and
broadcastOutput(s, ctx) passes ctx.outputMask.

The production broadcastOutput, broadcastOutputRouted and deliverCommandResult
(HardwareOne.cpp, with redactOutputForLog from System_Utils.cpp) run on the
production command types and the real Arduino String with its heap calls
counted, built with the device's inline capacity (13 characters). For every
origin (serial, web, Bluetooth, MQTT, system) x all 64 masks of the six route
bits x three session ids x three results (empty, ordinary, private STT) x
validate-only x the serial kill switch x five session states (stateless,
live, stale, revoked during the console write, no epoch) x a CLI mode owned
or not x the trace on or off, the transcript records the console bytes, the
drain wait, the queued copy with its route and prefix, the BLE reply, the
broadcast lines and the traces (with the mask they print); broadcastOutput(s,
ctx) itself is recorded for the live cases. The transcript's digest must
equal that of the tree before (TRANSCRIPT_SHA256); --previous <component dir>
builds the older tree with the same harness and compares line by line.

Blocks: a serial delivery whose user and address are past the inline capacity
makes EXPECTED_BLOCKS["serial delivery"] blocks (the tree before made two more,
the copy's user and address). The task identity slot (ExecIdentityGuard on
TaskIdentity, System_AuthIdentity.cpp, with a FreeRTOS task-local storage
double) is measured as it is, not changed: a web identity (a 32-character
session id and a 16-character path) installed and restored on one task makes
two blocks the first time and none after; the restore copies empty Strings
into the slot, which keep their blocks (String::copy frees only when the
source has no buffer, after a move or a failed allocation).

Source checks: the serial console (HardwareOne.cpp), the UART link
(System_UartLink.cpp) and runUnifiedSystemCommand (System_Utils.cpp) move
their local AuthContext into the Command and do not read it afterwards; the
mirror in deliverCommandResult makes no CommandContext copy.

Options: --cxx <compiler>, --sanitize, --component <dir>, --previous <dir>,
--print-counts.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from arduino_string_host import arduino_string_counted
from test_redact_audit import device_string
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# sha256 of the transcript printed by 49659fe9 (before this change).
TRANSCRIPT_SHA256 = "1ed5df387e7c7e809d781fbb3a185fc38bd2cdca81fd3723c9838fa1218e1dfb"

# String blocks per call, device inline capacity (the tree before: a serial
# delivery 5, the rest the same). The identity rows also pin the frees.
EXPECTED_BLOCKS = {
    "serial delivery": 3,
    "broadcast": 3,
    "identity install 1 (freed 0)": 2,
    "identity install 2 (freed 0)": 0,
    "identity install 3 (freed 0)": 0,
    "identity install 4 (freed 0)": 0,
    "identity install 5 (freed 0)": 0,
}

ROUTED = "static void broadcastOutputRouted(const String& s, const CommandContext& ctx,"


def generate(component: Path) -> str:
    main = (component / "HardwareOne.cpp").read_text()
    utils = (component / "System_Utils.cpp").read_text()
    identity = (component / "System_AuthIdentity.cpp").read_text()
    identity_header = (component / "System_AuthIdentity.h").read_text()
    pieces = [extract_block(utils, "String redactOutputForLog(const String& output)")]
    if ROUTED in main:
        pieces.append(extract_block(main, ROUTED))
    pieces += [extract_block(main, "void broadcastOutput(const String& s, const CommandContext& ctx) "),
               extract_block(main, "void deliverCommandResult(const String& result, const CommandContext& ctx)")]
    slot = re.search(r"(?m)^static constexpr BaseType_t kAuthTlsSlot = \d+;$", identity)
    require(slot is not None, "kAuthTlsSlot")
    types = "\n".join([extract_block(identity_header, "struct CaptureBufState") + ";",
                       extract_block(identity_header, "class ExecIdentityGuard") + ";",
                       slot[0],
                       extract_block(identity, "struct TaskIdentity") + ";",
                       extract_block(identity, "void deleteIdentity("),
                       extract_block(identity, "TaskIdentity* getOrCreateSlot()"),
                       extract_block(identity, "ExecIdentityGuard::ExecIdentityGuard(const AuthContext& install)"),
                       extract_block(identity, "ExecIdentityGuard::~ExecIdentityGuard()")])
    harness = (HERE / "command_identity_copies_harness.cpp").read_text()
    for marker, code in (("IDENTITY_TYPES", types), ("PRODUCTION", "\n\n".join(pieces))):
        tag = "// INSERT_" + marker + "\n"
        require(harness.count(tag) == 1, "one marker " + marker)
        harness = harness.replace(tag, code + "\n")
    return harness


def build(component: Path, directory: Path, cxx: str, sanitize: list[str]) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    includes, sources = arduino_string_counted(directory, sanitize)
    device_string(directory)
    unit, binary = directory / "test.cpp", directory / "test"
    unit.write_text(generate(component))
    subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-sign-compare", "-Wno-format-truncation", *sanitize, *includes,
                    "-I", str(component), str(unit), *sources, "-o", str(binary)], check=True, timeout=300)
    return binary


def transcript(binary: Path, directory: Path) -> bytes:
    path = directory / "transcript.txt"
    subprocess.run([str(binary), "transcript", str(path)], check=True, timeout=300)
    return path.read_bytes()


def counts(binary: Path) -> dict[str, int]:
    out = subprocess.run([str(binary), "counts"], check=True, capture_output=True, text=True,
                         timeout=60).stdout
    result = {}
    for line in out.splitlines():
        made, label = line.split(" ", 1)
        result[label] = int(made)
    return result


def source_check(component: Path) -> None:
    main = (component / "HardwareOne.cpp").read_text()
    uart = (component / "System_UartLink.cpp").read_text()
    utils = (component / "System_Utils.cpp").read_text()
    # The serial console: the block from its AuthContext to the delivery.
    start = main.index("          AuthContext actx;\n          actx.transport = SOURCE_SERIAL;")
    end = main.index("deliverCommandResult(out, uc.ctx);", start)
    serial = main[start:end]
    system = extract_block(utils, "void runUnifiedSystemCommand(const String& argsInput)")
    link = extract_block(uart, "static void uartProcessLine(String& cmd, uint32_t admittedTransportEpoch)")
    for name, text in (("serial console", serial), ("UART link", link), ("runUnifiedSystemCommand", system)):
        require(text.count("uc.ctx.auth = std::move(actx);") == 1, f"{name} moves its AuthContext")
        after = text[text.index("uc.ctx.auth = std::move(actx);") + 1:]
        require(re.search(r"\bactx\b", after.split("\n", 1)[1]) is None, f"{name} reads actx after the move")
    deliver = extract_block(main, "void deliverCommandResult(const String& result, const CommandContext& ctx)")
    require("CommandContext mirrorCtx" not in deliver and "CommandContext " not in deliver,
            "deliverCommandResult makes no CommandContext copy")
    require("broadcastOutputRouted(result, ctx, ctx.outputMask & ~(uint32_t)MSG_ROUTE_SERIAL)" in deliver,
            "the mirror is the context with the serial route cleared")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--print-counts", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-command-identity-") as tmp:
        d = Path(tmp)
        binary = build(args.component, d / "new", args.cxx, sanitize)
        text = transcript(binary, d / "new")
        blocks = counts(binary)
        if args.print_counts:
            for label, made in blocks.items():
                print(f"{made:4d} {label}")
        digest = hashlib.sha256(text).hexdigest()
        print(f"transcript: {text.count(b'case ')} cases, sha256 {digest}")
        if args.previous:
            old_binary = build(args.previous, d / "old", args.cxx, sanitize)
            old = transcript(old_binary, d / "old")
            new_lines, old_lines = text.splitlines(), old.splitlines()
            for index, (a, b) in enumerate(zip(new_lines, old_lines)):
                require(a == b, f"transcript line {index + 1}: {a!r}, before {b!r}")
            require(len(new_lines) == len(old_lines), "transcript length")
            print(f"transcript: {len(new_lines)} lines equal to the previous tree")
            if args.print_counts:
                for label, made in counts(old_binary).items():
                    print(f"before {made:4d} {label}")
        require(digest == TRANSCRIPT_SHA256, "the delivery transcript differs from the tree before")
        for label, made in EXPECTED_BLOCKS.items():
            require(blocks.get(label) == made, f"{label}: {blocks.get(label)} String blocks, expected {made}")
    source_check(args.component)
    print("command identity copies: routes and texts as before, blocks as expected, sources checked")


if __name__ == "__main__":
    main()
