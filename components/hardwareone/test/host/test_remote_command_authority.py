#!/usr/bin/env python3
"""remote:/@ commands cannot borrow the bonded peer's authority.

The bonded peer runs every remote:/@ command as bond-admin, which is super for
the live bond session. executeCommand's remote routing can check the unwrapped
line only against this build's registry flags, so a handler that decides
access at run time from the caller (file path rules, the owner of notes and
transcripts, sessions, voice arming), or a command this build does not
compile, would be decided for super on the peer. The routing therefore needs a
local administrator, except for the peer sensor start/stop lines the bond page
and the OLED bond screen send for any signed-in user.

The harness compiles, unchanged, the remote routing of executeCommand (from
the "REMOTE COMMAND ROUTING" marker through its `if (isRemoteCommand)` block),
authorizeCommand with its guest, password-change and session classifiers, the
remote gate and its allowlist when the tree has them, the remote reply owner
(formatRemoteCommandSent / parseRemoteCommandSentReqId) from System_Utils.cpp,
CommandArgs from System_Command.cpp and the registry name matcher
(System_CommandLookupCore.h). Accounts, registry rows, redaction, the audit
and event sinks and the bond transport are doubles. It checks that a signed-in
user (any transport, an automation, the require-auth-off identity, an
anonymous pre-auth line) cannot send the file verbs, sensorlog, opensr,
whoami, a setter, or a command this build lacks; that the sensor toggles the
two senders use stay open to them; that administrators, the superadmin and the
trusted internal identity keep the remote surface with the same CMD frame,
"Remote command sent: <redacted> (reqId <msgId>)" reply and broadcast; that the
guest gate, the registry's own admin/super refusals, the nested-wrapper
refusal and the bond-readiness errors are unchanged; and that the refusal
audit carries the redacted line.

Source checks: the sensor ids the bond page (WebPage_Bond.cpp) and the OLED
bond screen (OLED_Mode_Remote.cpp) toggle are read from those files and run
through the cases, and executeCommand's routing stays the only caller of
buildBondedCommandPayload (no second sender bypasses the gate).

--component / --source-dir point the suite at another copy of the production
sources (the pre-fix tree fails the user, unknown-command, guest-whoami and
redaction cases); the harness and this script are always read from here.
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

HARNESS = HERE / "remote_command_authority_harness.cpp"
ROUTING_MARKER = "// ===== REMOTE COMMAND ROUTING ====="
CASES = (
    "user_cannot_use_bond_authority",
    "unknown_here_refused",
    "sensor_toggles_stay_open",
    "administrators_keep_remote",
    "guest_and_unknown_account",
    "nested_refusal_first",
    "redaction",
    "local_lines_untouched",
    "bond_not_ready",
)


def unique(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"{name}: marker is not unique: {marker}")
    return extract_block(source, marker)


def routing_block(utils: str) -> str:
    require(utils.count(ROUTING_MARKER) == 1, "one remote routing marker in executeCommand")
    start = utils.index(ROUTING_MARKER)
    head_end = utils.index("if (isRemoteCommand)", start)
    block = extract_block(utils[head_end:], "if (isRemoteCommand)")
    return utils[start:head_end] + block


def sender_sensor_ids(bond_page: str, oled: str) -> list[str]:
    table = re.search(r"const sensors = \[(.*?)\];", bond_page, re.S)
    require(table is not None, "WebPage_Bond.cpp: the remote sensor table")
    web = re.findall(r"\{\s*id:\s*'(\w+)'", table.group(1))
    defs = re.search(r"static const BondSensorDef bondSensorDefs\[\] = \{(.*?)\n\};", oled, re.S)
    require(defs is not None, "OLED_Mode_Remote.cpp: bondSensorDefs")
    screen = re.findall(r'\{\s*"[^"]*",\s*"(\w+)"', defs.group(1))
    require(len(web) >= 8 and len(screen) >= 7, f"sensor ids: web={web} oled={screen}")
    # Both senders build "open<id>" / "close<id>" from these ids.
    require("(enable ? 'open' : 'close') + sensorId" in bond_page,
            "the bond page no longer sends open<id>/close<id>")
    require('"remote:%s%s", currentlyOn ? "close" : "open", sd.id' in oled,
            "the OLED bond screen no longer sends remote:open<id>/close<id>")
    ids: list[str] = []
    for sensor in web + screen:
        if sensor not in ids:
            ids.append(sensor)
    return ids


def generate(read) -> str:
    utils = read("System_Utils.cpp")
    command_header = read("System_Command.h")
    command_source = read("System_Command.cpp")
    user_header = read("System_User.h")
    wire = read("System_ESPNow_Wire.h")
    lookup = read("System_CommandLookupCore.h")

    reserved = re.findall(r"^inline constexpr const char\* kBondAdminUser = .*;$", user_header, re.M)
    require(len(reserved) == 1, "one reserved bond-admin name")
    cmd_type = re.search(r"\bESPNOW_V4_TYPE_CMD\s*=\s*(\w+)", wire)
    ack_flag = re.search(r"\bESPNOW_V4_FLAG_ACK_REQ\s*=\s*(\w+)", wire)
    require(cmd_type is not None and ack_flag is not None, "the CMD opcode and ACK_REQ flag")
    types = [
        reserved[0],
        unique(user_header, "enum CommandSource", "System_User.h") + ";",
        unique(user_header, "struct AuthContext", "System_User.h") + ";",
        f"static constexpr uint8_t ESPNOW_V4_TYPE_CMD = {cmd_type.group(1)};\n"
        f"static constexpr uint16_t ESPNOW_V4_FLAG_ACK_REQ = {ack_flag.group(1)};",
    ]

    pieces = [
        unique(command_header, "class CommandArgs", "System_Command.h") + ";",
        "const String CommandArgs::empty_;",
        unique(command_source, "CommandArgs::CommandArgs(", "System_Command.cpp"),
        unique(command_source, "void CommandArgs::parse(", "System_Command.cpp"),
        unique(command_source, "const String& CommandArgs::arg(", "System_Command.cpp"),
        unique(utils, "static bool commandAllowedForGuest(", "System_Utils.cpp"),
    ]
    if "static bool commandAllowedDuringPasswordChange(" in utils:
        pieces.append(unique(utils, "static bool commandAllowedDuringPasswordChange(", "System_Utils.cpp"))
    # The remote gate and its allowlist; the pre-fix tree has neither.
    if "static bool remoteCommandAllowedWithoutAdmin(" in utils:
        pieces.append(unique(utils, "static bool remoteCommandAllowedWithoutAdmin(", "System_Utils.cpp"))
    pieces += [
        unique(utils, "enum class SessionControlForm", "System_Utils.cpp") + ";",
        unique(utils, "static SessionControlForm classifySessionControlLine(", "System_Utils.cpp"),
        unique(utils, "static bool authorizeCommand(", "System_Utils.cpp"),
    ]
    if "static bool authorizeRemoteCommand(" in utils:
        pieces.append(unique(utils, "static bool authorizeRemoteCommand(", "System_Utils.cpp"))
    constants = re.findall(r"^static constexpr char kRemoteSent\w+\[\] = .*;$", utils, re.M)
    require(len(constants) == 2, "the remote reply owner's two constants")
    pieces += constants
    pieces += [
        unique(utils, "void formatRemoteCommandSent(", "System_Utils.cpp"),
        unique(utils, "bool parseRemoteCommandSentReqId(", "System_Utils.cpp"),
    ]

    ids = sender_sensor_ids(read("WebPage_Bond.cpp"), read("OLED_Mode_Remote.cpp"))
    senders = ("static const char* const kSenderSensorIds[] = {"
               + ", ".join(f'"{sensor}"' for sensor in ids) + "};")

    harness = HARNESS.read_text()
    replacements = (
        ("// INSERT_TYPES", "\n\n".join(types)),
        ("// INSERT_LOOKUP", lookup),
        ("// INSERT_PRODUCTION", "\n\n".join(pieces)),
        ("// INSERT_REMOTE_ROUTING", "#define ENABLE_ESPNOW 1\n#define ENABLE_BONDED_MODE 1\n"
                                     + routing_block(utils)),
        ("// INSERT_SENDER_SENSORS", senders),
    )
    for marker, text in replacements:
        require(harness.count(marker) == 1, f"one insertion marker {marker}")
        harness = harness.replace(marker, text)
    return harness


def single_bond_sender(component_files: dict[str, str]) -> list[str]:
    """executeCommand's routing is the only place a bonded command is built."""
    failures = []
    for name, text in component_files.items():
        code = re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", text, flags=re.S))
        calls = re.findall(r"(?<![\w:])buildBondedCommandPayload\s*\(", code)
        definitions = re.findall(r"\bString\s+buildBondedCommandPayload\s*\(", code)
        extra = len(calls) - len(definitions)
        if name == "System_Utils.cpp":
            # The routing's block-scope declaration and its one call.
            if extra != 1 or len(definitions) != 1:
                failures.append(f"System_Utils.cpp builds bonded commands {extra} times "
                                f"({len(definitions)} declarations); expected one call in the routing")
        elif extra > 0:
            failures.append(f"{name} builds a bonded command outside executeCommand's routing")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def path_of(name: str) -> Path:
        overlay = args.source_dir / name if args.source_dir else None
        return overlay if overlay and overlay.exists() else args.component / name

    def read(name: str) -> str:
        return path_of(name).read_text()

    failures: list[str] = []
    sources = {p.name: p.read_text(errors="ignore")
               for p in sorted(args.component.glob("*.cpp")) + sorted(args.component.glob("*.h"))}
    if args.source_dir:
        for p in sorted(args.source_dir.glob("*.cpp")) + sorted(args.source_dir.glob("*.h")):
            sources[p.name] = p.read_text(errors="ignore")
    failures += single_bond_sender(sources)
    print(("FAIL" if failures else "PASS") + " single_bond_sender")

    unit_text = generate(read)
    with tempfile.TemporaryDirectory(prefix="hw1-remote-command-authority-") as directory:
        unit = Path(directory) / "remote_command_authority.cpp"
        binary = Path(directory) / "remote_command_authority"
        unit.write_text(unit_text)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-parameter",
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=240)
        if compiled.returncode != 0:
            print(compiled.stderr[-4000:], file=sys.stderr)
            raise SystemExit("remote_command_authority: compile failed")
        for case in CASES:
            run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
            if run.returncode == 0:
                print(f"PASS {case}: {run.stdout.strip()}")
            else:
                print(f"FAIL {case}: {(run.stderr or run.stdout).strip()}")
                failures.append(case)
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        raise SystemExit("remote_command_authority: FAILED")
    print("remote_command_authority: remote:/@ needs a local administrator except the peer sensor "
          "toggles; admin replies, nested refusal and redaction unchanged")


if __name__ == "__main__":
    main()
