#!/usr/bin/env python3
"""Execute the production users.json and pending_users.json writers on the fake VFS.

The harness compiles the real roster mutators from System_User.cpp (ban/unban,
promote/demote, delete, lastSeen, creation-time resolution, useradd, approve,
deny, request), the real FsLockGuard, text writers and settings path, with
vendored ArduinoJson against the shared fake VFS (fake_vfs.h). Hashing, sessions,
events and the per-user settings writer are doubles. It checks that free-text
fields holding '}' or ']' never break the file, that a roster larger than 8 KB
is resolved whole, that account creation stores the credential before the roster
entry and publishes nothing on a refused write, that a roster without nextId
gets a unique id, that a recreated or restored roster never reissues an id at
or below the persisted account-id mark (a variable double of the NVS key), and
that approve holds the filesystem lock across its
read-modify-write. On a tree that still edits users.json as text the same
calls run, so the cases fail behaviourally rather than at compile time; the
source-level contract checks run after the binary.
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
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def string_double() -> str:
    """The shared host String plus the members the roster code calls."""
    stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
    anchor = "  bool concat(const char* text) {"
    require(stub.count(anchor) == 1, "one concat(const char*) in the String double")
    extra = """  bool concat(const String& other) { return concat(other.c_str()); }
  bool reserve(size_t capacity) { text_.reserve(capacity); return true; }
  long toInt() const { return std::atol(text_.c_str()); }
  bool equalsIgnoreCase(const String& other) const { return equalsIgnoreCase(other.c_str()); }
  int indexOf(const String& value, size_t from = 0) const { return indexOf(value.c_str(), from); }
"""
    stub = stub.replace(anchor, extra + anchor)
    return stub.replace("#include <string>", "#include <cstdlib>\n#include <string>")


def region(source: str, start_markers: tuple[str, ...], end_marker: str) -> str:
    """Source text from the first marker present up to the end of end_marker's block."""
    starts = [m for m in start_markers if m in source]
    require(bool(starts), f"none of {start_markers} found")
    start = source.index(starts[0])
    require(source.count(starts[0]) == 1, f"marker is not unique: {starts[0]}")
    end_start = source.index(end_marker, start)
    block = extract_block(source[end_start:], end_marker)
    return source[start:end_start + len(block)]


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"marker is not unique: {marker}")
    return extract_block(source, marker)


def last_block(source: str, marker: str) -> str:
    """The definition that follows an earlier declaration of the same name."""
    require(marker in source, f"missing definition: {marker}")
    return extract_block(source[source.rindex(marker):], marker)


def production(user: str, header: str, settings: str, mutex: str, mutex_header: str, utils: str) -> dict:
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()",
        "void fsLock(const char* owner)",
        "void fsUnlock()"))
    text_io = "\n\n".join(extract_block(utils, marker) for marker in ("bool readText(", "bool writeText(", "bool writeTextAtomic("))
    text_io += "\n\n" + unique_block(settings, "String getUserSettingsPath(")

    types = "\n".join(re.findall(r"^static constexpr size_t kPublic\w+ = \d+;$", header, re.M))
    require(types.count("\n") == 2, "the three public account limits")
    reserved = re.findall(r"^inline constexpr const char\* kBondAdminUser = .*;$", header, re.M)
    require(len(reserved) == 1, "one reserved bond-admin name")
    types += "\n" + reserved[0]
    declaration = re.findall(r"^bool adminCreateUser\([^;]*\);", header, re.M | re.S)
    require(len(declaration) == 1, "one useradd declaration (its default role)")
    types += "\n" + declaration[0]
    if "struct NewAccount {" in header:
        types += "\n" + extract_block(header, "struct NewAccount") + ";"
    # On a tree with the self-registration owner, cmd_user_request is a thin
    # parser over submitAccountRequest and its field check.
    has_request_owner = "enum class AccountRequestResult" in header
    if has_request_owner:
        types += "\n" + extract_block(header, "enum class AccountRequestResult") + ";"

    for marker in ("bool isValidPublicUsername(", "bool adminCreateUser("):
        require(user.count(marker) == 1, f"marker is not unique: {marker}")
    pieces = [
        unique_block(user, "bool isKnownUserRole("),
        # The reserved-name rule the validators and the id lookup share.
        unique_block(user, "bool isReservedUsername(") if "bool isReservedUsername(" in user else "",
        unique_block(user, "bool getUserIdByUsername("),
        # Validators, and on a tree with the owner: mutateUsersJson, the pending-list
        # writer and createAccount, which sit between the validators and useradd.
        user[user.index("bool isValidPublicUsername("):user.index("bool adminCreateUser(")],
        unique_block(user, "bool adminCreateUser("),
        region(user, ("struct BanMutationContext {", "static const char* setUserBanInternal("), "void updateUserLastSeen("),
        region(user, ("bool approvePendingUserInternal(",), "bool denyPendingUserInternal("),
        region(user, ("struct RoleMutationContext {", "static bool setUserRoleInternal("), "static bool deleteUserInternal("),
        unique_block(user, "static AccountRequestResult checkAccountRequestFields(") if has_request_owner else "",
        unique_block(user, "AccountRequestResult submitAccountRequest(") if has_request_owner else "",
        unique_block(user, "const char* cmd_user_request("),
        region(user, ("struct BootAnchor {",), "void resolvePendingUserCreationTimes()"),
    ]
    new_signature = "static bool setUserBanInternal(const String& username, bool ban, const String& reason, String& errorOut)"
    if new_signature in user:
        adapter = ("static bool banUser(const String& u, bool ban, const String& reason, String& err) {\n"
                   "  return setUserBanInternal(u, ban, reason, err);\n}")
    else:
        require("static const char* setUserBanInternal(" in user, "a ban writer")
        adapter = ("static bool banUser(const String& u, bool ban, const String& reason, String& err) {\n"
                   "  const char* failure = setUserBanInternal(u, ban, reason);\n"
                   "  if (failure) { err = failure; return false; }\n  return true;\n}")
    return {"// INSERT_FS_GUARD": guard, "// INSERT_TEXT_IO": text_io, "// INSERT_TYPES": types,
            "// INSERT_PRODUCTION": "\n\n".join(pieces), "// INSERT_BAN_ADAPTER": adapter}


def generate(user: str) -> tuple[str, bool]:
    header = (COMPONENT / "System_User.h").read_text()
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    harness = (HERE / "user_store_mutations_harness.cpp").read_text()
    for marker, replacement in production(user, header, settings, mutex, mutex_header, utils).items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    # The factory-reset id record sits between createAccount and useradd, so
    # the validator region above already carries it when the tree has one.
    harness = f"#define HAS_ACCOUNT_ID_RECORD {int('bool recordAccountIdHighWater(' in user)}\n" + harness
    legacy_text_editing = "USERS_JSON_BUF_SIZE" in user or "replaceJsonField(" in user
    return harness, legacy_text_editing


def source_contract(user: str) -> None:
    """One users.json owner, one account owner, locked pending-list writers."""
    for marker in ("static bool setUserBanInternal(", "void updateUserLastSeen(", "static bool setUserRoleInternal(",
                   "static bool deleteUserInternal(", "void resolvePendingUserCreationTimes()"):
        body = last_block(user, marker)  # the resolver is also declared extern earlier in the file
        require("mutateUsersJson(" in body, f"{marker} must go through the users.json owner")
        for forbidden in ("indexOf(", "writeTextAtomic(", "serializeJson(", "readText("):
            require(forbidden not in body, f"{marker} still edits or writes users.json itself: {forbidden}")
    owner = unique_block(user, "static bool mutateUsersJson(")
    for required in ("FsLockGuard", "deserializeJson(", "overflowed()", "measureJson(", "writeTextAtomic("):
        require(required in owner, f"mutateUsersJson must contain {required}")
    require("USERS_JSON_BUF_SIZE" not in user, "the fixed resolver buffer is gone")
    for marker in ("bool adminCreateUser(", "bool approvePendingUserInternal("):
        body = unique_block(user, marker)
        require(body.count("createAccount(") == 1, f"{marker} must create the account through createAccount")
        require("saveUserSettings(" not in body and "users.add<JsonObject>()" not in body,
                f"{marker} duplicates the account owner")
    create = unique_block(user, "bool createAccount(")
    require("isValidPublicUsername(" in create and "FsLockGuard" in create, "createAccount validates and locks")
    mutation = unique_block(user, "static UsersJsonMutation createAccountMutation(")
    require(mutation.index("saveUserSettings(") < mutation.index("users.add<JsonObject>()"),
            "the credential is stored before the roster entry is added")
    # The request intake's read-modify-write lives in submitAccountRequest once the
    # self-registration owner exists; cmd_user_request is then only its parser.
    request_writer = ("AccountRequestResult submitAccountRequest(" if "AccountRequestResult submitAccountRequest(" in user
                      else "const char* cmd_user_request(")
    for marker in ("bool approvePendingUserInternal(", "bool denyPendingUserInternal(", request_writer):
        body = unique_block(user, marker)
        require("FsLockGuard" in body, f"{marker} must hold the filesystem lock across its read-modify-write")
        require('"w"' not in body, f"{marker} must not rewrite the pending list in place")
    approve = unique_block(user, "bool approvePendingUserInternal(")
    require(approve.index("createAccount(") < approve.index("writePendingUsers("),
            "approve retires the request only after the account is published")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name (for per-case evidence)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    user = (COMPONENT / "System_User.cpp").read_text()
    unit_text, legacy_text_editing = generate(user)
    with tempfile.TemporaryDirectory(prefix="hw1-user-store-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_double())
        unit = directory / "user_store_mutations.cpp"
        unit.write_text(unit_text)
        binary = directory / "user_store_mutations"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(HERE), "-I", str(COMPONENT), "-I", str(JSON_INCLUDE),
                   str(unit), "-o", str(binary)]
        if legacy_text_editing:
            # The text-editing mutators compare int offsets with String::length();
            # let them build so the comparison fails on behaviour, not on a warning.
            at = command.index("-Wno-unused-function") + 1
            command[at:at] = ["-Wno-sign-compare", "-Wno-unused-variable"]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)
    source_contract(user)


if __name__ == "__main__":
    main()
