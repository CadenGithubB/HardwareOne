#!/usr/bin/env python3
"""Execute the forced-password-change owners with real ArduinoJson.

Extracts, unchanged, the per-user settings merge and its editable-key list from
System_Settings.cpp, userMustChangePassword with its cache and guest rule and
createAccount from System_User.cpp, and webPasswordChangeAllows from
WebServer_Utils.cpp. It checks that the merge refuses password,
gamepad_password and mustChangePassword whatever their value (JSON null
included) and any key that is not a preference, that the decision ignores the
flag for guests and is cached until a password write or identity change, that
the web gate answers APIs with 403 password_change_required and pages with a
redirect while admitting only the change form and logout, and that a guest
cannot be created with a forced change. The settings store, roster, httpd and
users.json writer are doubles. On a tree without one of these owners the
present ones still run (so an older merge fails on its behaviour) and the
harness then fails naming what is missing.
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
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def unique(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"{name}: marker is not unique: {marker}")
    return extract_block(source, marker)


def generate() -> str:
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    user = (COMPONENT / "System_User.cpp").read_text()
    user_header = (COMPONENT / "System_User.h").read_text()
    utils = (COMPONENT / "WebServer_Utils.cpp").read_text()

    merge_struct = unique(settings, "struct UserSettingsMergeContext", "System_Settings.cpp") + ";"
    pieces = ["namespace {\n" + merge_struct + "\n" +
              unique(settings, "bool mergeUserSettingsMutation(", "System_Settings.cpp") + "\n}  // namespace"]
    has_refused_key = "const char* userSettingsPatchRefusedKey(" in settings
    if has_refused_key:
        pieces.append(unique(settings, "const char* userSettingsPatchRefusedKey(", "System_Settings.cpp"))
    pieces.append(unique(settings, "bool mergeAndSaveUserSettings(", "System_Settings.cpp"))

    # The leading newline keeps the closing "}  // namespace ..." comment out.
    has_cache = "\nnamespace must_change_cache" in user
    if has_cache:
        pieces.append(unique(user, "\nnamespace must_change_cache", "System_User.cpp"))
        pieces.append(unique(user, "static bool accountIsGuestForPasswordChange(", "System_User.cpp"))
        pieces.append(unique(user, "static bool readMustChangePassword(", "System_User.cpp"))
    pieces.append(unique(user, "bool userMustChangePassword(", "System_User.cpp"))

    has_gate = "bool webPasswordChangeAllows(" in utils
    if has_gate:
        pieces.append(unique(utils, "bool webPasswordChangeAllows(", "WebServer_Utils.cpp"))

    has_create = "bool createAccount(" in user and "struct NewAccount {" in user_header
    if has_create:
        pieces.append(unique(user_header, "struct NewAccount", "System_User.h") + ";")
        pieces.append(unique(user, "struct CreateAccountContext", "System_User.cpp") + ";")
        pieces.append(unique(user, "bool createAccount(", "System_User.cpp"))

    harness = (HERE / "password_change_gate_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION"
    require(harness.count(marker) == 1, "one insertion marker")
    flags = "".join(f"#define {name} {int(value)}\n" for name, value in (
        ("HAS_REFUSED_KEY", has_refused_key), ("HAS_CACHE", has_cache),
        ("HAS_GATE", has_gate), ("HAS_CREATE", has_create)))
    return flags + harness.replace(marker, "\n\n".join(pieces))


def source_contract() -> None:
    """The adapters consult the owners instead of deciding themselves."""
    user = (COMPONENT / "System_User.cpp").read_text()
    server = (COMPONENT / "WebServer_Server.cpp").read_text()
    # tgRequireAuth admits web requests only; its whole body is the web path.
    auth = extract_block(user, "bool tgRequireAuth(AuthContext& ctx)")
    require("webPasswordChangeAllows(req, ctx.user)" in auth,
            "tgRequireAuth must run the forced-password-change gate for every web request")
    setter = unique(user, "bool setUserPassword(", "System_User.cpp")
    require("must_change_cache::sWrites.fetch_add(" in setter,
            "setUserPassword must invalidate cached must-change answers")
    handler = unique(server, "esp_err_t handleUserSettingsSet(", "WebServer_Server.cpp")
    require("userSettingsPatchRefusedKey(patch)" in handler and "mergeAndSaveUserSettings(" in handler,
            "/api/user/settings must take the owner's editable-key decision")
    reset = unique(user, "const char* cmd_user_resetpassword(", "System_User.cpp")
    require(reset.index("accountIsGuestForPasswordChange(") < reset.index("setUserPassword("),
            "userresetpassword must refuse a forced change for a guest before writing")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-password-change-gate-") as directory:
        unit = Path(directory) / "password_change_gate.cpp"
        binary = Path(directory) / "password_change_gate"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-pthread",
                   "-I", str(HERE / "web_batch_stubs"), "-I", str(COMPONENT),
                   "-I", str(JSON_INCLUDE), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=30)
    source_contract()
    print("password change gate source contract passed")


if __name__ == "__main__":
    main()
