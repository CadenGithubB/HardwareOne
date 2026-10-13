#!/usr/bin/env python3
"""Run the BLE login job and loginTransport against the real credential attempt table.

bleLoginDeferred, bleLoginJobFree and BleLoginAsyncJob (Bluetooth.cpp), and
loginTransport plus the credential_attempts table (System_User.cpp, with its
System_User.h declarations) are compiled unchanged into
ble_display_login_lockout_harness.cpp. The host replaces the clock, the
critical section, the filesystem lock, the password check, the BLE session
binder and sender, the console session publishers, the audit line and the
event register.

Bluetooth (C8/D3): five wrong passwords lock BLE login for every connection
and a sixth attempt, even with the right password, is refused before the
password is checked, with the lockout text sent only to the requesting
session; the job's password is wiped and the job freed on every path; an
unavailable credential store and a connection lost before the bind (right
password) are not guesses; a bound login clears its account's failures and
posts SYSEVT_LOGIN_OK, a wrong one SYSEVT_LOGIN_FAIL; a stale session epoch
touches nothing.

Display (C8/D3, set 2's F1 login owner): loginTransport throttles the OLED
login screen and a bare `login` typed on the display under the reserved
"display" key: the sixth attempt is refused without checking the password,
reports the time left and writes a "Locked out" audit line; a login whose
session does not publish is not a guess; a published one clears.

--component DIR reads Bluetooth.cpp and System_User.cpp/.h from another tree
(the pre-fix tree fails the lockout cases behaviourally).
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

TABLE_FUNCTIONS = (
    "bool credentialAttemptAllowed(CommandSource transport",
    "void credentialAttemptRecord(CommandSource transport",
    "uint8_t credentialAttemptFailures(CommandSource transport",
    "void credentialAttemptForget(CommandSource transport",
    "const char* credentialAttemptConsolePeer(CommandSource transport",
    "uint32_t getTotalFailedLoginCount(",
)


def unique_block(source: str, marker: str, name: str) -> str:
    require(source.count(marker) == 1, f"{name}: expected one {marker!r}")
    return extract_block(source, marker)


def generate(component: Path) -> str:
    header = (component / "System_User.h").read_text()
    user = (component / "System_User.cpp").read_text()
    ble = (component / "Bluetooth.cpp").read_text()

    types = extract_block(header, "enum CommandSource") + ";\n"
    limits = re.findall(r"^static constexpr size_t kPublic(?:Username|Password)MaxLen = \d+;$",
                        header, re.M)
    require(len(limits) == 2, "System_User.h: the public name and password limits")
    types += "\n".join(limits) + "\n"
    begin_marker = "// Credential attempt table: the brute-force lockout"
    end_marker = "uint32_t getTotalFailedLoginCount();"
    require(begin_marker in header and end_marker in header,
            "System_User.h must declare the credential attempt table")
    declarations = header[header.index(begin_marker):header.index(end_marker) + len(end_marker)]

    table = extract_block(user, "\nnamespace credential_attempts") + "\n"
    for marker in TABLE_FUNCTIONS:
        table += unique_block(user, marker, "System_User.cpp") + "\n"
    login = unique_block(user, "bool loginTransport(CommandSource transport", "System_User.cpp")
    has_lockout_out = "lockoutRetryMsOut" in login.split("{", 1)[0]

    job = unique_block(ble, "struct BleLoginAsyncJob", "Bluetooth.cpp") + ";\n"
    job_free = unique_block(ble, "static void bleLoginJobFree(", "Bluetooth.cpp")
    deferred = unique_block(ble, "static void bleLoginDeferred(void* arg)", "Bluetooth.cpp")

    harness = (HERE / "ble_display_login_lockout_harness.cpp").read_text()
    for marker, value in (
        ("// INSERT_TYPES", types),
        ("// INSERT_DECLARATIONS", declarations),
        ("// INSERT_TABLE", table),
        ("// INSERT_LOGIN_TRANSPORT", login),
        ("// INSERT_BLE_JOB", job),
        ("// INSERT_BLE_LOGIN", job_free + "\n\n" + deferred),
    ):
        require(harness.count(marker) == 1, f"missing insertion point {marker}")
        harness = harness.replace(marker, value)
    return f"#define HAS_LOCKOUT_OUT {int(has_lockout_out)}\n" + harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    unit_text = generate(args.component.resolve())
    with tempfile.TemporaryDirectory(prefix="hw1-ble-display-lockout-") as temporary:
        directory = Path(temporary)
        unit, binary = directory / "harness.cpp", directory / "harness"
        unit.write_text(unit_text)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-parameter", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        print(result.stdout, end="")
        require(result.returncode == 0, f"BLE/display login lockout failed:\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed assertion count")
        print(f"ble_display_login_lockout: {count[1]} assertions passed", flush=True)


if __name__ == "__main__":
    main()
