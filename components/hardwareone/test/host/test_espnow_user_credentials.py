#!/usr/bin/env python3
"""Run ESP-NOW's password checks and the USER_SYNC receiver against their owners.

Compiled unchanged into espnow_user_credentials_harness.cpp: the credential
attempt table and checkCredentialAttempt (System_User.cpp, where the tree has
it), the account owner region of System_User.cpp (name and password rules,
mutateUsersJson, createAccount and the account-id high-water mark), the roster
lookup and reserved-name rule, the real FsLockGuard, text writers and settings
path, USER_SYNC's DeferredUserSyncWork and doUserSyncWork, and v4_handle_cmd
up to its authentication decision (the command it would queue is recorded
instead). Storage is the shared fake VFS (fake_vfs.h) with vendored
ArduinoJson; the password check, admin rule, hashing, the per-user settings
writer, the NVS id mark, the bond-token validators, the radio senders, the
audit and the event register are doubles.

C8/D3 (ESP-NOW part): password remote commands and USER_SYNC's admin check
count under ESP-NOW's one attempt-table key, for every peer; the sixth attempt
is refused before the password is checked with a CMD_RESP that echoes the
msgId; a plaintext command is refused before any check; the bond-token path
neither consults nor feeds the table and still works while it is locked.

A11 with C10: USER_SYNC creates accounts through createAccount: reserved and
malformed names and short passwords are refused with nothing written; the id
comes from the high-water mark, never nextId; a failed credential write
publishes nothing and answers ok=false; the role is forced to "user", the
provenance is "espnow:<device>", createdAt is stamped from a valid clock,
SYSEVT_USER_ADDED is posted and the identity generation bumped; an existing
name is still "User already exists (skipped)" with ok=true; the
session-encryption and opt-in refusals still answer with a bare ACK.

--component DIR reads the production files from another tree (the pre-fix
tree fails the lockout and account-owner cases behaviourally).
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
from test_user_store_mutations import string_double

HERE = Path(__file__).resolve().parent
JSON_INCLUDE = HERE.parents[2] / "hardwareone_libs" / "ArduinoJson" / "src"

TABLE_FUNCTIONS = (
    "bool credentialAttemptAllowed(CommandSource transport",
    "void credentialAttemptRecord(CommandSource transport",
    "uint8_t credentialAttemptFailures(CommandSource transport",
    "void credentialAttemptForget(CommandSource transport",
    "const char* credentialAttemptConsolePeer(CommandSource transport",
    "uint32_t getTotalFailedLoginCount(",
)
AUTH_DECISION = "if (!authOk) return;"


def string_with_length() -> str:
    """The shared String double plus the (pointer, length) constructor that
    doUserSyncWork uses for the frame payload."""
    stub = string_double()
    anchor = "  String(const std::string& text) : text_(text) {}"
    require(stub.count(anchor) == 1, "one std::string constructor in the String double")
    extra = "  String(const char* text, size_t length) : text_(text ? std::string(text, length) : std::string()) {}\n"
    return stub.replace(anchor, anchor + "\n" + extra)


def unique_block(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"{name}: expected one {marker!r}")
    return extract_block(source, marker)


def generate(component: Path) -> str:
    header = (component / "System_User.h").read_text()
    user = (component / "System_User.cpp").read_text()
    espnow = (component / "System_ESPNow.cpp").read_text()
    mutex = (component / "System_Mutex.cpp").read_text()
    mutex_header = (component / "System_Mutex.h").read_text()
    utils = (component / "System_Utils.cpp").read_text()
    settings = (component / "System_Settings.cpp").read_text()

    # Types and contracts from System_User.h.
    types = extract_block(header, "enum CommandSource") + ";\n"
    limits = re.findall(r"^static constexpr size_t kPublic\w+ = \d+;$", header, re.M)
    require(len(limits) == 3, "System_User.h: the three public account limits")
    types += "\n".join(limits) + "\n"
    reserved = re.findall(r"^inline constexpr const char\* kBondAdminUser = .*;$", header, re.M)
    require(len(reserved) == 1, "System_User.h: one reserved bond-admin name")
    types += reserved[0] + "\n"
    types += extract_block(header, "struct NewAccount") + ";\n"
    begin_marker = "// Credential attempt table: the brute-force lockout"
    end_marker = "uint32_t getTotalFailedLoginCount();"
    require(begin_marker in header and end_marker in header,
            "System_User.h must declare the credential attempt table")
    types += header[header.index(begin_marker):header.index(end_marker) + len(end_marker)] + "\n"

    table = extract_block(user, "\nnamespace credential_attempts") + "\n"
    for marker in TABLE_FUNCTIONS:
        table += unique_block(user, marker, "System_User.cpp") + "\n"
    check_marker = "CredentialCheck checkCredentialAttempt(CommandSource transport"
    if "enum class CredentialCheck" in header and user.count(check_marker) == 1:
        begin = header.index("enum class CredentialCheck")
        types += header[begin:header.index(");", header.index(check_marker, begin)) + 2] + "\n"
        table += extract_block(user, check_marker) + "\n"

    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()",
        "void fsLock(const char* owner)",
        "void fsUnlock()"))
    text_io = "\n\n".join(extract_block(utils, marker)
                          for marker in ("bool readText(", "bool writeText(", "bool writeTextAtomic("))
    text_io += "\n\n" + unique_block(settings, "String getUserSettingsPath(", "System_Settings.cpp")

    for marker in ("bool isValidPublicUsername(", "bool adminCreateUser("):
        require(user.count(marker) == 1, f"System_User.cpp: expected one {marker!r}")
    accounts = "\n\n".join((
        unique_block(user, "bool isKnownUserRole(", "System_User.cpp"),
        unique_block(user, "bool isReservedUsername(", "System_User.cpp"),
        unique_block(user, "bool getUserIdByUsername(", "System_User.cpp"),
        # Validators, mutateUsersJson, the pending-list writer, createAccount
        # and the id record: everything between the validators and useradd.
        user[user.index("bool isValidPublicUsername("):user.index("bool adminCreateUser(")],
    ))

    sync = unique_block(espnow, "struct DeferredUserSyncWork", "System_ESPNow.cpp") + ";\n\n"
    sync += unique_block(espnow, "static void doUserSyncWork(const DeferredUserSyncWork* w)",
                         "System_ESPNow.cpp")
    remote = unique_block(espnow, "static void v4_handle_cmd(const uint8_t* srcMac, const char* "
                                  "deviceName, uint32_t msgId, const char* cmdPayload",
                          "System_ESPNow.cpp")
    require(remote.count(AUTH_DECISION) == 1, f"v4_handle_cmd: one {AUTH_DECISION!r}")
    # Everything up to the authentication decision; the queued command that
    # follows is recorded by the harness instead of built.
    remote = (remote[:remote.index(AUTH_DECISION) + len(AUTH_DECISION)]
              + "\n  harnessCommandAccepted(srcMac, msgId, username, actualCmd);\n}\n")

    harness = (HERE / "espnow_user_credentials_harness.cpp").read_text()
    for marker, value in (
        ("// INSERT_TYPES", types),
        ("// INSERT_FS_GUARD", guard),
        ("// INSERT_TEXT_IO", text_io),
        ("// INSERT_TABLE", table),
        ("// INSERT_ACCOUNTS", accounts),
        ("// INSERT_USER_SYNC", sync),
        ("// INSERT_REMOTE_COMMAND", remote),
    ):
        require(harness.count(marker) == 1, f"missing insertion point {marker}")
        harness = harness.replace(marker, value)
    has_check = int("enum class CredentialCheck" in types)
    return f"#define HAS_CHECK_ATTEMPT {has_check}\n" + harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    unit_text = generate(component)
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-user-credentials-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_with_length())
        unit, binary = directory / "harness.cpp", directory / "harness"
        unit.write_text(unit_text)
        # No -pedantic: DeferredUserSyncWork ends in a flexible array member.
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(HERE), "-I", str(component),
                   "-I", str(JSON_INCLUDE), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=240)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        print(result.stdout, end="")
        require(result.returncode == 0, f"ESP-NOW user credentials failed:\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed assertion count")
        print(f"espnow_user_credentials: {count[1]} assertions passed", flush=True)


if __name__ == "__main__":
    main()
