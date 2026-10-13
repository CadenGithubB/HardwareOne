#!/usr/bin/env python3
"""Execute the production account-authority code on the fake VFS, with and without HTTP.

The harness compiles the real admin/super predicates (including the bond-admin
branch), isValidUser, getUserIdByUsername, userAccountRank, the mutation rank
rule, the ban/unban/sessionrevoke handlers, the targeted-logout owner, the
userlist/pendinglist/sessionlist handlers and the roster snapshot from
System_User.cpp, plus hasAdminPrivilege/hasSuperAdminPrivilege from
HardwareOne.cpp, against the shared fake VFS (fake_vfs.h) with vendored
ArduinoJson. It builds twice, ENABLE_HTTP_SERVER 0 and 1. It checks that an
account named bond-admin is never loginable or privileged off the live ESP-NOW
bond, that unban, sessionrevoke user and targeted logout follow the rank rule,
that account bans work without the HTTP server, that list replies are either
NUL-terminated and parseable or an explicit error, and that the roster
snapshot keeps full names. A source contract then checks that the OLED and
lens user pages read the snapshot instead of users.json and that the lens page
offers Kick/Ban in every build.
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
from test_user_store_mutations import last_block, string_double, unique_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def between(source: str, start_marker: str, end_marker: str) -> str:
    require(source.count(start_marker) == 1, f"marker is not unique: {start_marker!r}")
    start = source.index(start_marker)
    return source[start:source.index(end_marker, start)]


def auth_types(header: str) -> str:
    pieces = [extract_block(header, "enum CommandSource") + ";",
              extract_block(header, "struct AuthContext") + ";"]
    epoch = re.findall(r"^using TransportSessionEpoch = uint32_t;$", header, re.M)
    require(len(epoch) == 1, "one TransportSessionEpoch alias")
    pieces.append(epoch[0])
    return "\n".join(pieces)


def header_constants(header: str) -> str:
    lines = re.findall(r"^static constexpr size_t kPublicUsernameMaxLen = \d+;$", header, re.M)
    require(len(lines) == 1, "the public username limit")
    lines += re.findall(r"^inline constexpr const char\* kBondAdminUser = .*;$", header, re.M)
    ranks = re.findall(r"^constexpr int kRoleRank\w+\s*=\s*-?\d+;$", header, re.M)
    require(len(ranks) == 5, "the role ranks and the no-grant sentinel")
    lines += ranks
    limits = (COMPONENT / "System_CommandLimits.h").read_text()
    result_max = re.findall(r"^static constexpr size_t CMD_RESULT_MAX = \d+;$", limits, re.M)
    require(len(result_max) == 1, "CMD_RESULT_MAX")
    lines += result_max
    if "struct UserRosterEntry {" in header:
        lines.append(extract_block(header, "struct UserRosterEntry") + ";")
    return "\n".join(lines)


def prototypes(user: str) -> str:
    lines = []
    if "bool bondIdentityLive(" in user:
        lines.append("bool bondIdentityLive(const AuthContext& ctx);")
    if "bool isReservedUsername(" in user:
        lines.append("bool isReservedUsername(const String& username);")
    if "bool readUserRoster(" in user:
        lines.append("bool readUserRoster(UserRosterEntry* out, size_t capacity, size_t& countOut,"
                     " size_t* totalOut = nullptr);")
    return "\n".join(lines)


def ban_writer_double(user: str) -> str:
    record = ('  banCalls.emplace_back(std::string(username.c_str()) + (ban ? ":ban" : ":unban"));\n')
    if "static bool setUserBanInternal(const String& username, bool ban, const String& reason, String& errorOut)" in user:
        return ("static bool setUserBanInternal(const String& username, bool ban, const String&, String&) {\n"
                + record + "  return true;\n}")
    require("static const char* setUserBanInternal(const String& username, bool ban, const String& reason)" in user,
            "a ban writer")
    return ("static const char* setUserBanInternal(const String& username, bool ban, const String&) {\n"
            + record + "  return nullptr;\n}")


def production(user: str) -> str:
    pieces = []
    for marker in ("bool isReservedUsername(", "bool bondIdentityLive("):
        if marker in user:
            pieces.append(unique_block(user, marker))
    pieces += [
        unique_block(user, "int userRoleRank("),
        unique_block(user, "bool isKnownUserRole("),
        # Forward declarations of the roster parsers through isSuperAdminUser.
        between(user, "static bool readRosterJson(const char* tag, String& out);", "bool isSuperAdminUser(")
        + extract_block(user[user.index("bool isSuperAdminUser("):], "bool isSuperAdminUser("),
        unique_block(user, "bool isValidUser("),
        unique_block(user, "bool getUserIdByUsername("),
        unique_block(user, "bool getUserRole("),
        unique_block(user, "int userAccountRank("),
        unique_block(user, "static bool userMutationAllowed("),
        last_block(user, "bool logoutTransportFromNamedSession("),
    ]
    if "enum class RosterReadResult" in user:
        start = user.index("enum class RosterReadResult")
        list_end = user.index("const char* cmd_user_list(", start)
        pieces.append(user[start:list_end] + extract_block(user[list_end:], "const char* cmd_user_list("))
    else:
        pieces.append(unique_block(user, "const char* cmd_user_list("))
    pieces.append(unique_block(user, "const char* cmd_pending_list("))
    # Session list, session revoke, IP bans and account bans, with their HTTP
    # guards, up to the account request handler.
    pieces.append(between(user, "#if ENABLE_HTTP_SERVER\nconst char* cmd_session_list(", "const char* cmd_user_request("))
    return "\n\n".join(pieces)


def privilege(hardwareone: str) -> str:
    return "\n\n".join(last_block(hardwareone, marker)
                       for marker in ("bool hasAdminPrivilege(", "bool hasSuperAdminPrivilege("))


def fs_guard() -> str:
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()",
        "void fsLock(const char* owner)",
        "void fsUnlock()"))
    return guard


def generate(user: str) -> str:
    header = (COMPONENT / "System_User.h").read_text()
    hardwareone = (COMPONENT / "HardwareOne.cpp").read_text()
    harness = (HERE / "user_roster_authority_harness.cpp").read_text()
    logout = last_block(user, "bool logoutTransportFromNamedSession(")
    if "denialOut" in logout:
        logout_macro = "#define LOGOUT_TARGET(t, s, u, e, d) logoutTransportFromNamedSession(t, s, u, e, d)"
    else:
        logout_macro = "#define LOGOUT_TARGET(t, s, u, e, d) ((void)(d), logoutTransportFromNamedSession(t, s, u, e))"
    snapshot = "1" if "bool readUserRoster(" in user else "0"
    replacements = {
        "// INSERT_AUTH_TYPES": auth_types(header),
        "// INSERT_FS_GUARD": fs_guard(),
        "// INSERT_PROTOTYPES": header_constants(header) + "\n" + prototypes(user) + "\n" + logout_macro
        + f"\n#define HW1_HAS_ROSTER_SNAPSHOT {snapshot}",
        "// INSERT_BAN_WRITER_DOUBLE": ban_writer_double(user),
        "// INSERT_PRODUCTION": production(user),
        "// INSERT_PRIVILEGE": privilege(hardwareone),
    }
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def source_contract() -> None:
    """The display adapters read the roster owner and act on full names."""
    oled = (COMPONENT / "OLED_Mode_UserManager.cpp").read_text()
    lens = (COMPONENT / "G2_Page_Users.cpp").read_text()
    for name, text in (("OLED_Mode_UserManager.cpp", oled), ("G2_Page_Users.cpp", lens)):
        require("readUserRoster(" in text, f"{name} must read the System_User roster snapshot")
        for forbidden in ("deserializeJson(", "USERS_JSON_FILE", "readText("):
            require(forbidden not in text, f"{name} still parses users.json itself: {forbidden}")
    require("char name[24]" not in oled and "gUmDelName[kPublicUsernameMaxLen + 1]" in oled,
            "the OLED keeps full usernames for dispatch")
    require("#if ENABLE_HTTP_SERVER" not in lens, "the lens page offers Kick/Ban in every build")
    user = (COMPONENT / "System_User.cpp").read_text()
    lister = unique_block(user, "const char* cmd_user_list(")
    require("visitUserRoster(" in lister and ".set(users)" not in lister,
            "userlist uses the roster snapshot and no longer copies whole records")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name (for per-case evidence)")
    parser.add_argument("--http", choices=("0", "1"), help="build only one HTTP variant")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    user = (COMPONENT / "System_User.cpp").read_text()
    unit_text = generate(user)
    with tempfile.TemporaryDirectory(prefix="hw1-user-roster-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_double())
        unit = directory / "user_roster_authority.cpp"
        unit.write_text(unit_text)
        for http in ([args.http] if args.http else ["0", "1"]):
            binary = directory / f"user_roster_authority_http{http}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-parameter", "-Wno-unused-function", f"-DHW1_TEST_HTTP={http}",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                       "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                       "-I", str(directory), "-I", str(HERE), "-I", str(COMPONENT), "-I", str(JSON_INCLUDE),
                       str(unit), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=240)
            subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)
    source_contract()


if __name__ == "__main__":
    main()
