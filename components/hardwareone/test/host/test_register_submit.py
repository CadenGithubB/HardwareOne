#!/usr/bin/env python3
"""Execute the production /register/submit handler against a recording owner.

Extracts, unchanged, handleRegisterSubmit and its fixed failure page from
WebServer_Server.cpp, urlDecode and extractFormField from System_Utils.cpp,
and the public username/password rules from System_User.cpp. The httpd
request, the lockout adapters and the account-request owner are doubles: the
owner double records exactly what it receives. It checks that a passphrase
with spaces, quotes or percent-escapes reaches the owner byte-exact, that a
request refused because it is already pending, because the queue is full or
because storage failed does not count toward the login lockout, and that
rejected form fields still do.

It also extracts CommandArgs (System_Command.cpp), authorizeCommand and its
session-control classifier (System_Utils.cpp) and the `userrequest` adapter
(cmd_user_request) with its field check. It checks that `userrequest` is
still refused for every external transport without an identity (registration
does not open it to anonymous callers), that validate-only mode returns VALID
without reaching the owner, and that a quoted passphrase reaches the owner
byte-exact. On a tree that still files the request as a `userrequest` command
line under an empty web identity, the production authorizeCommand refuses it
and the web cases fail on behaviour; where cmd_user_request does its own
pending-list I/O the command cases are reported as failed.

The same authorizeCommand is driven for the forced password change on
command transports: while userMustChangePassword (a double) holds for the
caller, every external transport refuses anything but userchangepassword,
bare login/logout and whoami, with the change-password reply, one audit entry
and one denial event; other accounts, internal callers and the
require-auth-off identity are not gated. On a tree without that gate the case
fails on behaviour.
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

# Used only on a tree whose System_User.h has no account-request owner, so
# the harness still compiles there and fails on behaviour.
FALLBACK_RESULT = """enum class AccountRequestResult : uint8_t {
  Submitted, InvalidUsername, PasswordTooShort, PasswordTooLong, AlreadyPending,
  QueueFull, NotReady, ReadFailed, WriteFailed,
};"""


def unique(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"{name}: marker is not unique: {marker}")
    return extract_block(source, marker)


CASES = (
    "web_spaced_passphrase",
    "web_repeated_words",
    "web_quotes_and_escapes",
    "web_owner_refusals",
    "web_rejected_fields",
    "anonymous_userrequest_refused",
    "must_change_password_command_gate",
    "command_validate_only",
    "command_quoted_passphrase",
)


def has_adapter(user: str) -> bool:
    """True when cmd_user_request is a parser over the account-request owner."""
    command = unique(user, "const char* cmd_user_request(", "System_User.cpp")
    return "submitAccountRequest(" in command and "static AccountRequestResult checkAccountRequestFields(" in user


def generate() -> tuple[str, bool]:
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    user = (COMPONENT / "System_User.cpp").read_text()
    header = (COMPONENT / "System_User.h").read_text()
    command_header = (COMPONENT / "System_Command.h").read_text()
    command_source = (COMPONENT / "System_Command.cpp").read_text()

    limits = re.findall(r"^static constexpr size_t kPublic\w+ = \d+;$", header, re.M)
    require(len(limits) == 3, "the three public account limits")
    reserved = re.findall(r"^inline constexpr const char\* kBondAdminUser = .*;$", header, re.M)
    require(len(reserved) == 1, "one reserved bond-admin name")
    has_owner = "enum class AccountRequestResult" in header
    result = (unique(header, "enum class AccountRequestResult", "System_User.h") + ";") if has_owner \
        else FALLBACK_RESULT
    types = ["\n".join(limits + reserved),
             unique(header, "enum CommandSource", "System_User.h") + ";",
             unique(header, "struct AuthContext", "System_User.h") + ";",
             result]

    pieces = [
        unique(command_header, "class CommandArgs", "System_Command.h") + ";",
        "const String CommandArgs::empty_;",
        unique(command_source, "CommandArgs::CommandArgs(", "System_Command.cpp"),
        unique(command_source, "void CommandArgs::parse(", "System_Command.cpp"),
        unique(command_source, "const String& CommandArgs::arg(", "System_Command.cpp"),
        unique(utils, "enum class SessionControlForm", "System_Utils.cpp") + ";",
        unique(utils, "static SessionControlForm classifySessionControlLine(", "System_Utils.cpp"),
    ]
    # The must-change allowlist; a tree without it has no command-side gate.
    if "static bool commandAllowedDuringPasswordChange(" in utils:
        pieces.append(unique(utils, "static bool commandAllowedDuringPasswordChange(", "System_Utils.cpp"))
    pieces.append(unique(utils, "static bool authorizeCommand(", "System_Utils.cpp"))
    if "bool isReservedUsername(" in user:
        pieces.append(unique(user, "bool isReservedUsername(", "System_User.cpp"))
    pieces += [
        unique(user, "bool isValidPublicUsername(", "System_User.cpp"),
        unique(user, "bool isValidPublicPassword(", "System_User.cpp"),
    ]
    adapter = has_owner and has_adapter(user)
    if adapter:
        pieces += [
            unique(user, "static AccountRequestResult checkAccountRequestFields(", "System_User.cpp"),
            unique(user, "const char* cmd_user_request(", "System_User.cpp"),
        ]
    else:
        pieces.append("#define HW1_NO_ACCOUNT_REQUEST_ADAPTER 1")
    pieces += [
        unique(utils, "String urlDecode(", "System_Utils.cpp"),
        unique(utils, "String extractFormField(", "System_Utils.cpp"),
        unique(server, "static esp_err_t sendRegisterFailPage(", "WebServer_Server.cpp"),
        unique(server, "esp_err_t handleRegisterSubmit(", "WebServer_Server.cpp"),
    ]
    harness = (HERE / "register_submit_harness.cpp").read_text()
    for marker, text in (("// INSERT_TYPES", types), ("// INSERT_PRODUCTION", pieces)):
        require(harness.count(marker) == 1, f"one insertion marker {marker}")
        harness = harness.replace(marker, "\n\n".join(text))
    return harness, has_owner


def source_contract() -> None:
    """The handler files through the owner, and the command stays authenticated."""
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    user = (COMPONENT / "System_User.cpp").read_text()
    handler = unique(server, "esp_err_t handleRegisterSubmit(", "WebServer_Server.cpp")
    require(handler.count("submitAccountRequest(username, password)") == 1,
            "handleRegisterSubmit must hand the decoded fields to the account owner")
    for forbidden in ("executeUnifiedWebCommand(", "userrequest", "cmdBuf"):
        require(forbidden not in handler, f"handleRegisterSubmit must not build a command line: {forbidden}")
    command = unique(user, "const char* cmd_user_request(", "System_User.cpp")
    require("submitAccountRequest(" in command, "userrequest must call the account owner")
    for forbidden in ("writeTextAtomic(", "PSRAM_JSON_DOC", "readText(", "FsLockGuard"):
        require(forbidden not in command, f"userrequest must stay a thin parser: {forbidden}")
    validate = command.index("RETURN_VALID_IF_VALIDATE_CSTR();")
    require("//RETURN_VALID_IF_VALIDATE_CSTR" not in command, "the validate-only guard is live")
    require(command.index("checkAccountRequestFields(") < validate < command.index("submitAccountRequest("),
            "userrequest validates its arguments, then stops before the owner in validate-only mode")
    owner = unique(user, "AccountRequestResult submitAccountRequest(", "System_User.cpp")
    require("FsLockGuard" in owner and "writePendingUsers(doc" in owner,
            "the owner writes the pending list under the lock through writePendingUsers")
    require("writeTextAtomic(" not in owner and not re.search(r"(?<!de)serializeJson\(", owner),
            "the owner reuses the pending-list writer instead of its own serializer")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    unit_text, has_owner = generate()
    with tempfile.TemporaryDirectory(prefix="hw1-register-submit-") as directory:
        unit = Path(directory) / "register_submit.cpp"
        binary = Path(directory) / "register_submit"
        unit.write_text(unit_text)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-parameter",
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        failures = []
        for case in CASES:
            run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=30)
            if run.returncode == 0:
                print(f"PASS {case}: {run.stdout.strip()}")
            else:
                print(f"FAIL {case}: {(run.stderr or run.stdout).strip()}")
                failures.append(case)
    try:
        require(has_owner, "System_User.h declares no account-request owner")
        source_contract()
        print("PASS source_contract")
    except AssertionError as error:
        print(f"FAIL source_contract: {error}")
        failures.append("source_contract")
    require(not failures, f"register submit cases failed: {', '.join(failures)}")
    print("register submit suite passed")


if __name__ == "__main__":
    main()
