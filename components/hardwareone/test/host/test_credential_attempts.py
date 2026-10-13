#!/usr/bin/env python3
"""Execute the production credential attempt table and pin who consults it.

The table section of System_User.cpp (the credential_attempts namespace and
the credentialAttempt* functions) and its System_User.h contract are compiled
unchanged into credential_attempts_harness.cpp. The host replaces only the
clock, the critical section, diagnostics and the event register. Scenarios:
tiers 5/10/20, per-key isolation, a success for another account does not
clear, window expiry during a lock stays locked, no eviction of a locked
entry, a full pool of locks refuses new peers while the consoles keep their
slots, MQTT keeps its own slot, lock events and totals, a lock across the
32-bit millis() wrap, and ended locks staying ended through weeks of idle.
Bluetooth and ESP-NOW each count under one reserved key ("ble", "espnow")
that a full web pool cannot refuse and a changed peer label cannot split, and
checkCredentialAttempt refuses a locked key without validating, then records
the outcome and wipes its copy of the password (host isValidUser and
secureClearString doubles count the validations and the wipes).

Source contracts: the table is compiled regardless of ENABLE_HTTP_SERVER; the
web (/login and Basic Auth), serial, UART, MQTT and targeted-login paths
consult it before validating a password; so do the BLE login job (after its
session check, recording success only after the bind, never inside the
command tail), loginTransport (the display login screen and a bare `login`),
the display's set-pattern re-authentication, and ESP-NOW's password remote
commands and USER_SYNC admin check (not the bond-token path); Basic Auth
failures are audited and evented like /login; MQTT failures are audited; the
old web table and the targeted-login throttle are gone.

--baseline-web PATH runs the same scenarios against the old per-IP lockout
functions extracted from a WebServer_Server.cpp (with its WebServer_Server.h
beside it) through a thin adapter; that run is expected to fail.
--component DIR reads the production files from another tree.
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

TABLE_FUNCTIONS = (
    "bool credentialAttemptAllowed(CommandSource transport",
    "void credentialAttemptRecord(CommandSource transport",
    "uint8_t credentialAttemptFailures(CommandSource transport",
    "void credentialAttemptForget(CommandSource transport",
    "const char* credentialAttemptConsolePeer(CommandSource transport",
    "uint32_t getTotalFailedLoginCount(",
)


def header_types() -> str:
    header = (COMPONENT / "System_User.h").read_text()
    types = extract_block(header, "enum CommandSource") + ";\n"
    marker = "static constexpr size_t kPublicUsernameMaxLen ="
    begin = header.index(marker)
    return types + header[begin:header.index(";", begin) + 1] + "\n"


def current_units() -> tuple[str, str, str]:
    header = (COMPONENT / "System_User.h").read_text()
    begin_marker = "// Credential attempt table: the brute-force lockout"
    end_marker = "uint32_t getTotalFailedLoginCount();"
    require(begin_marker in header and end_marker in header,
            "System_User.h must declare the credential attempt table")
    declarations = header[header.index(begin_marker):
                          header.index(end_marker) + len(end_marker)]
    source = (COMPONENT / "System_User.cpp").read_text()
    require("\nnamespace credential_attempts" in source,
            "System_User.cpp must own the credential attempt table "
            "(namespace credential_attempts)")
    production = extract_block(source, "\nnamespace credential_attempts") + "\n"
    for marker in TABLE_FUNCTIONS:
        require(source.count(marker) == 1, f"expected one definition: {marker}")
        production += extract_block(source, marker) + "\n"
    # The throttled check-validate-record helper, where the tree has it; a tree
    # without it still runs every other scenario and fails the helper's.
    check_decl = "enum class CredentialCheck"
    check_marker = "CredentialCheck checkCredentialAttempt(CommandSource transport"
    has_check = check_decl in header and source.count(check_marker) == 1
    if has_check:
        begin = header.index(check_decl)
        declarations += "\n" + header[begin:header.index(");", header.index(check_marker, begin)) + 2]
        production += extract_block(source, check_marker) + "\n"
    declarations = (f"#define HARNESS_CURRENT_TABLE 1\n#define HAS_CHECK_ATTEMPT {int(has_check)}\n"
                    + declarations)
    adapter = """
static void harnessReset() {
  std::memset(credential_attempts::sEntries, 0,
              sizeof(credential_attempts::sEntries));
  credential_attempts::sTotalFailures = 0;
}
static constexpr size_t kHarnessPoolSlots = CREDENTIAL_ATTEMPT_POOL_SLOTS;
"""
    return declarations, production, adapter


def baseline_web_units(web_source: Path) -> tuple[str, str, str]:
    """The old per-IP lockout, driven through the new API's names."""
    header = (web_source.parent / "WebServer_Server.h").read_text()
    begin = header.index("#define MAX_LOGIN_ATTEMPT_ENTRIES")
    end_marker = "uint32_t getTotalFailedLoginCount();"
    declarations = ("#define HARNESS_CURRENT_TABLE 0\n#define HAS_CHECK_ATTEMPT 0\n"
                    + header[begin:header.index(end_marker) + len(end_marker)])
    source = web_source.read_text()
    begin = source.index("EXT_RAM_BSS_ATTR static LoginAttemptEntry sLoginAttempts")
    marker = "uint32_t getTotalFailedLoginCount() "
    end = source.index(marker) + len(extract_block(source, marker))
    production = source[begin:end] + "\n"
    adapter = """
bool credentialAttemptAllowed(CommandSource, const char* peer,
                              unsigned long* retryMsOut = nullptr) {
  return !isLoginLocked(peer, retryMsOut);
}
void credentialAttemptRecord(CommandSource, const char* peer, const char*,
                             bool success) {
  if (success) clearLoginAttempts(peer);
  else recordFailedLogin(peer);
}
uint8_t credentialAttemptFailures(CommandSource, const char* peer) {
  LoginAttemptEntry* e = findLoginEntry(peer);
  return e ? e->failCount : 0;
}
void credentialAttemptForget(CommandSource, const char* peer) {
  clearLoginAttempts(peer);
}
static void harnessReset() {
  std::memset(sLoginAttempts, 0, sizeof(sLoginAttempts));
  sTotalFailedLogins = 0;
}
static constexpr size_t kHarnessPoolSlots = MAX_LOGIN_ATTEMPT_ENTRIES;
"""
    return declarations, production, adapter


def conditional_stack(text: str, offset: int) -> list[str]:
    """Preprocessor conditions (#if/#ifdef/#ifndef lines) open at offset."""
    stack: list[str] = []
    for line in text[:offset].splitlines():
        stripped = line.strip()
        if re.match(r"#\s*if", stripped):
            stack.append(stripped)
        elif re.match(r"#\s*(elif|else)", stripped) and stack:
            stack[-1] = stack[-1] + " / " + stripped
        elif re.match(r"#\s*endif", stripped) and stack:
            stack.pop()
    return stack


def require_not_http_gated(name: str, text: str, needle: str) -> None:
    require(needle in text, f"{name}: missing {needle}")
    for occurrence in re.finditer(re.escape(needle), text):
        stack = conditional_stack(text, occurrence.start())
        gated = [c for c in stack if "ENABLE_HTTP_SERVER" in c]
        require(not gated, f"{name}: {needle} sits under {gated}")


def require_before(name: str, body: str, first: str, second: str) -> None:
    require(first in body, f"{name}: missing {first}")
    require(second in body, f"{name}: missing {second}")
    require(body.index(first) < body.index(second),
            f"{name}: {first} must come before {second}")


def check_contracts() -> None:
    user_source = (COMPONENT / "System_User.cpp").read_text()
    for marker in ("\nnamespace credential_attempts",) + TABLE_FUNCTIONS:
        require_not_http_gated("System_User.cpp", user_source, marker)
    require("portENTER_CRITICAL(&sMux)" in user_source,
            "the table must take its own lock")
    _, table, _ = current_units()
    code = re.sub(r"//[^\n]*", "", table)
    require("millis()" not in code and "esp_timer_get_time()" in code,
            "the table keeps time on the 64-bit esp_timer clock, not 32-bit millis()")
    require("static_cast<long>" not in code,
            "no signed-difference time checks in the table")
    require("SOURCE_MQTT currently has NO such upstream check" not in user_source,
            "stale comment: MQTT does validate before submit")

    web = (COMPONENT / "WebServer_Server.cpp").read_text()
    web_header = (COMPONENT / "WebServer_Server.h").read_text()
    require("LoginAttemptEntry" not in web and "LoginAttemptEntry" not in web_header,
            "the old web-owned lockout table must be gone")
    require("single-threaded" not in web_header,
            "WebServer_Server.h must not claim a single-threaded writer")
    authed = extract_block(web, "bool isAuthed(httpd_req_t* req, String& outUser)")
    basic = authed[authed.index("decodeBasicAuth("):authed.index("no session cookie")]
    require_before("Basic Auth", basic, "credentialAttemptAllowed(SOURCE_WEB",
                   "isValidUser(baUser, baPass)")
    failure = basic[basic.index("credentialAttemptRecord(SOURCE_WEB, ipBuf, baUser.c_str(), false)"):]
    for token in ("recordLoginAttempt(SOURCE_WEB", "SYSEVT_LOGIN_FAIL"):
        require(token in failure, f"Basic Auth failures must call {token} like /login")
    require("clearLoginAttempts(" not in basic, "Basic success clears only its account")
    login = extract_block(web, "esp_err_t handleLogin(httpd_req_t* req)")
    require_before("/login", login, "credentialAttemptAllowed(SOURCE_WEB",
                   "isValidUser(u, p)")
    require("clearLoginAttempts(" not in login, "/login success clears only its account")
    require("credentialAttemptRecord(SOURCE_WEB, ip.c_str(), u.c_str(), true)" in login,
            "/login records the account that succeeded")

    main = (COMPONENT / "HardwareOne.cpp").read_text()
    serial = main[main.index("auto handleSerialLocalLogin = [&]()"):]
    serial = serial[:serial.index("// Serial auth gate")]
    require_before("serial login", serial, "credentialAttemptAllowed(SOURCE_SERIAL",
                   "isValidUser(u, p)")
    require_not_http_gated("HardwareOne.cpp", main, "credentialAttemptAllowed(SOURCE_SERIAL")
    require("isLoginLocked(" not in serial and "ENABLE_HTTP_SERVER" not in serial,
            "serial lockout must not depend on the web server")

    uart = (COMPONENT / "System_UartLink.cpp").read_text()
    require_before("UART login", uart, "credentialAttemptAllowed(SOURCE_UART",
                   "isValidUser(u, p)")
    require("isLoginLocked(" not in uart and "recordFailedLogin(" not in uart,
            "the UART link must use the System_User table")
    require_not_http_gated("System_UartLink.cpp", uart, "credentialAttemptAllowed(SOURCE_UART")

    mqtt = (COMPONENT / "System_MQTT.cpp").read_text()
    command = extract_block(mqtt, "static void handleMQTTCommand(")
    require_before("MQTT command", command, "credentialAttemptAllowed(SOURCE_MQTT",
                   "isValidUser(")
    rejected = command[command.index("isValidUser("):command.index("Authentication failed")]
    for token in ("credentialAttemptRecord(SOURCE_MQTT", "recordLoginAttempt(SOURCE_MQTT"):
        require(token in rejected, f"MQTT failures must call {token}")
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    audit = extract_block(debug, "void recordLoginAttempt(CommandSource transport")
    require("case SOURCE_MQTT:" in audit and '"mqtt/login"' in audit,
            "recordLoginAttempt maps MQTT to mqtt/login")

    utils = (COMPONENT / "System_Utils.cpp").read_text()
    require("TargetLoginThrottle" not in utils, "the targeted-login throttle folds into the table")
    login_cmd = extract_block(utils, "const char* cmd_login(const String& originalCmd)")
    require_before("targeted login", login_cmd, "credentialAttemptAllowed(caller.transport",
                   "loginTransportFromNamedSession(")
    require("credentialAttemptRecord(caller.transport" in login_cmd,
            "targeted login records its outcome in the table")
    require("loginTransport(transport, username, password, &lockoutRetryMs)" in login_cmd,
            "a bare login reports loginTransport's lockout")

    # Bluetooth: one reserved key, checked after the session check and before
    # the password, a success recorded only once the session is bound. The
    # command tail (spliced whole by ble_sc_policy) stays free of the table.
    ble = (COMPONENT / "Bluetooth.cpp").read_text()
    ble_login = extract_block(ble, "static void bleLoginDeferred(void* arg)")
    require_before("BLE login", ble_login, "bleSessionEpochMatches(",
                   "credentialAttemptAllowed(SOURCE_BLUETOOTH")
    require_before("BLE login", ble_login, "credentialAttemptAllowed(SOURCE_BLUETOOTH",
                   "validateTransportCredentials(")
    require_before("BLE login", ble_login, "bleBindSession(",
                   "credentialAttemptRecord(SOURCE_BLUETOOTH, attemptPeer, job->user, true)")
    require("credentialAttemptConsolePeer(SOURCE_BLUETOOTH)" in ble_login,
            "BLE logins count under Bluetooth's reserved key, never a connId")
    require("credentialAttempt" not in extract_block(ble, "static void processBleCommandLine("),
            "the BLE command tail must not consult the table itself")

    # The display: loginTransport throttles the login screen and a bare
    # `login`; the set-pattern re-authentication uses the same key.
    transport_login = extract_block(user_source, "bool loginTransport(CommandSource transport")
    require("credentialAttemptConsolePeer(transport)" in transport_login,
            "loginTransport keys consoles by their fixed peer")
    require_before("loginTransport", transport_login, "credentialAttemptAllowed(transport, attemptPeer",
                   "isValidUser(username, password)")
    oled_login = extract_block((COMPONENT / "OLED_Mode_Auth.cpp").read_text(),
                               "static bool handleLoginModeInput(")
    require("&lockoutRetryMs)" in oled_login,
            "the display login screen shows loginTransport's lockout")
    pattern = extract_block((COMPONENT / "OLED_Mode_SetPattern.cpp").read_text(),
                            "static bool handleSetPatternInput(")
    require("checkCredentialAttempt(SOURCE_LOCAL_DISPLAY" in pattern and "isValidUser(" not in pattern,
            "set-pattern re-authentication counts under the display's key")

    # ESP-NOW: password remote commands and USER_SYNC's admin check share one
    # reserved key; the bond-token path is not a password and stays outside.
    espnow = (COMPONENT / "System_ESPNow.cpp").read_text()
    remote = extract_block(espnow, "static void v4_handle_cmd(const uint8_t* srcMac, "
                                   "const char* deviceName, uint32_t msgId, const char* cmdPayload")
    password = remote[remote.index("Traditional username:password auth"):]
    require(re.search(r"checkCredentialAttempt\(\s*SOURCE_ESPNOW", password) is not None
            and "isValidUser(" not in password,
            "ESP-NOW password remote commands consult the table")
    bond = remote[:remote.index("Traditional username:password auth")]
    require("credentialAttempt" not in bond and "logBondAuthFailure(" in bond,
            "the bond-token path keeps logBondAuthFailure and stays out of the table")
    sync = extract_block(espnow, "static void doUserSyncWork(const DeferredUserSyncWork* w)")
    require(re.search(r"checkCredentialAttempt\(\s*SOURCE_ESPNOW", sync) is not None
            and "isValidUser(" not in sync,
            "USER_SYNC's admin check consults the table")


def main() -> None:
    global COMPONENT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline-web", type=Path,
                        help="run the scenarios against an old WebServer_Server.cpp lockout")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="read the production files from this components/hardwareone")
    args = parser.parse_args()
    COMPONENT = args.component.resolve()
    require(bool(args.cxx), "a host C++17 compiler is required")
    if args.baseline_web:
        label = "baseline-web"
        declarations, production, adapter = baseline_web_units(args.baseline_web)
    else:
        label = "current"
        declarations, production, adapter = current_units()
    harness = (HERE / "credential_attempts_harness.cpp").read_text()
    for marker, value in (("// INSERT_TYPES", header_types()),
                          ("// INSERT_DECLARATIONS", declarations),
                          ("// INSERT_PRODUCTION", production),
                          ("// INSERT_ADAPTER", adapter)):
        require(harness.count(marker) == 1, f"missing insertion point {marker}")
        harness = harness.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix="hw1-credential-attempts-") as temporary:
        directory = Path(temporary)
        unit, binary = directory / f"{label}.cpp", directory / label
        unit.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        require(result.returncode == 0, f"{label} scenarios failed:\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, f"{label}: missing completed assertion count")
        print(f"credential attempts {label}: {count[1]} assertions passed", flush=True)
    if not args.baseline_web:
        check_contracts()
        print("credential attempts: web, serial, UART, MQTT, targeted login, BLE, the "
              "display and ESP-NOW password logins consult the System_User table before "
              "validating; it compiles without the web server", flush=True)


if __name__ == "__main__":
    main()
