#!/usr/bin/env python3
"""The portable P4X-EYE can bring its web server up on demand.

The owner wants the handheld's web server usable when asked for (decision of
2026-11-19: "If I want to use the web server on the handheld I should be able
to"). This suite pins what that needs from the build and from the defaults, so
a later change of a default or of the saved configuration cannot take it away
unnoticed:

- `configurations/p4-portable.json`, resolved through System_BuildConfig.h as
  the build resolves it, compiles Wi-Fi and the HTTP server in (and HTTPS out,
  which is the board profile's choice, not a block: the server then speaks
  plain HTTP);
- `openwifi`, `openhttp` and their counterparts are registered commands, the
  two web-server rows inside the HTTP server's own compile gate only;
- `openhttp` refuses for two reasons only: Wi-Fi is not joined, or the server
  already runs (no saved setting refuses it since 2026-11-21);
- the settings those depend on default to on in both places a default lives
  (the struct initialiser and the settings row, which is what a boot applies);
- nothing this batch's defaults changed (the C6 console mirror, the phrase
  leveler, the companion fault counters, HTTPS/bonded/assistant build
  defaults) is read by those two handlers.

The command sequence with `httpautostart 0` saved, written down from
HardwareOne.cpp (boot starts the server only with httpAutoStart on) and
System_WiFi.cpp (cmd_httpstart):

    wifiadd <ssid> <pass>      once, when no network is saved
    openwifi                   then `wifistatus` until it reads connected
    openhttp                   "HTTP server started"
    closehttp                  stops it; it stays off across later joins until openhttp

Source text and the host preprocessor only; nothing is built or run.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import sys

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = HERE.parents[3]


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def check_build() -> str:
    """Resolve the saved configuration; returns a note for the summary line."""
    config = json.loads((REPO / "configurations/p4-portable.json").read_text())
    require(config["board"] == "p4x_eye", "p4-portable is a P4X-EYE configuration")
    if not (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")):
        return "build resolution skipped (no host C++ compiler)"
    sys.path.insert(0, str(REPO / "tools"))
    import build_config_resolve
    names = ("ENABLE_WIFI", "ENABLE_HTTP_SERVER", "ENABLE_HTTPS", "ENABLE_BLUETOOTH", "ENABLE_G2_GLASSES",
             "ENABLE_SD_CARD", "ENABLE_LOCAL_STT", "ENABLE_BLE_PHONE_PIPE", "ENABLE_ESPNOW")
    values, messages = build_config_resolve.resolve(REPO, "p4x_eye", "esp32p4", config["features"],
                                                    config.get("sdkconfig"), names)
    require(not messages, f"System_BuildConfig.h accepts p4-portable: {messages}")
    require(values is not None, "p4-portable resolves to values")
    require(values.get("ENABLE_WIFI") == 1, "p4-portable compiles Wi-Fi in")
    require(values.get("ENABLE_HTTP_SERVER") == 1, "p4-portable compiles the HTTP server in")
    # Not a block: without HTTPS in the image the server starts as plain HTTP.
    require(values.get("ENABLE_HTTPS") == 0, "p4-portable has HTTPS out of the image (board profile)")
    # The rest of what the portable is, so this test notices a different file under the same name.
    for name in ("ENABLE_BLUETOOTH", "ENABLE_G2_GLASSES", "ENABLE_SD_CARD", "ENABLE_LOCAL_STT",
                 "ENABLE_BLE_PHONE_PIPE", "ENABLE_ESPNOW"):
        require(values.get(name) == 1, f"p4-portable has {name}")
    return "p4-portable resolves with Wi-Fi and the HTTP server"


def check_commands() -> None:
    wifi = (COMPONENT / "System_WiFi.cpp").read_text()
    table = extract_block(wifi, "const CommandEntry wifiCommands[] =")
    rows = {name: (admin == "true", handler) for name, admin, handler in
            re.findall(r'\{ "(\w+)", "(?:[^"\\]|\\.)*", (true|false), (\w+)', table)}
    for name, handler in (("openwifi", "cmd_wificonnect"), ("closewifi", "cmd_closewifi"),
                          ("openhttp", "cmd_httpstart"), ("closehttp", "cmd_httpstop")):
        require(name in rows, f"{name} is a registered Wi-Fi command")
        require(rows[name] == (True, handler), f"{name} is an admin command handled by {handler}")
    require("wifiadd" in rows, "wifiadd is registered")
    # The web-server rows sit inside the HTTP server's gate and no other.
    start = table.index('{ "openhttp",')
    opened = [m for m in re.finditer(r"^#(if|ifdef|ifndef|endif)\b(.*)$", table[:start], re.M)]
    stack = []
    for match in opened:
        if match.group(1) == "endif":
            stack.pop()
        else:
            stack.append(match.group(2).strip())
    require(stack == ["ENABLE_HTTP_SERVER"], f"openhttp is compiled with the HTTP server only (gates: {stack})")
    before_wifi = [m.group(1) for m in re.finditer(r"^#(if|ifdef|ifndef|endif)\b", table[:table.index('{ "openwifi",')],
                                                    re.M)]
    require(before_wifi.count("endif") == len(before_wifi) - before_wifi.count("endif"),
            "openwifi is in no conditional block of the table")

    # openhttp: two refusals, in this order, then the start.
    markers = [m.start() for m in re.finditer(r"^const char\* cmd_httpstart\(const String& argsInput\) \{$", wifi, re.M)]
    require(len(markers) >= 1, "cmd_httpstart is defined")
    body = strip_comments(extract_block(wifi[markers[0]:], "const char* cmd_httpstart(const String& argsInput)"))
    refusals = re.findall(r'if \(([^{}]+)\) \{', body)
    require([r.strip() for r in refusals[:2]] == ["!WiFi.isConnected()", "server != NULL"],
            f"openhttp refuses only for: Wi-Fi not joined, already running ({refusals})")
    require(body.index("server != NULL") < body.index("startHttpServer();"), "then it starts the server")
    require("'openwifi'" in body and "'closehttp'" in body, "each refusal says what to run")
    # Every command a refusal or hint names exists under that name (the sequence in this file's
    # header uses the same ones).
    named = set(re.findall(r"'(\w+)(?: \d)?'", body))
    require({"openwifi", "closehttp"} <= named, f"the refusals name their commands ({sorted(named)})")
    editor = strip_comments((COMPONENT / "System_Settings.cpp").read_text())
    for name in sorted(named):
        require(name in rows or re.search(r'\{ "' + name + r'",', editor) is not None,
                f"{name}, named by openhttp's replies, is a registered command")
    settings_read = set(re.findall(r"gSettings\.(\w+)", body))
    require(not settings_read, f"openhttp reads no saved setting ({sorted(settings_read)})")
    connect = strip_comments(extract_block(wifi, "const char* cmd_wificonnect(const String&"))
    # Nothing this batch gave a new default is consulted by either command.
    for changed in ("c6ConsoleMirror", "sttPhraseLevel", "c6OutageCount", "c6RebootCount", "c6LastFaultEpochSec",
                    "ENABLE_HTTPS", "ENABLE_BONDED_MODE", "ENABLE_LLM_"):
        require(changed not in body and changed not in connect,
                f"{changed} does not decide whether openwifi or openhttp run")

    # Boot starts the server only with httpautostart on; with it off the commands are the way.
    main = strip_comments((COMPONENT / "HardwareOne.cpp").read_text())
    require(re.search(r"sBootNetServices\.http = "
                      r"ramFlushResolve\(RF_HTTP, gSettings\.httpAutoStart\);", main) is not None,
            "boot auto-starts the server only with httpAutoStart")


def check_defaults() -> None:
    settings_h = (COMPONENT / "System_Settings.h").read_text()
    wifi = strip_comments((COMPONENT / "System_WiFi.cpp").read_text())
    for field in ("wifiAutoStart", "httpAutoStart"):
        require(re.search(r"\b" + field + r"\(true\)", settings_h) is not None,
                f"the struct default of {field} is on")
        row = re.search(r"\{ [^{}\n]*SETTING_BOOL,\s*&gSettings\." + field + r",\s*(\w+),", wifi)
        require(row is not None, f"{field} has a settings row in System_WiFi.cpp")
        require(row.group(1) in ("true", "1"), f"the row default of {field} is on (it is what a boot applies)")
    for key, command in (("wifiAutoStart", "wifiautostart"), ("httpAutoStart", "httpautostart")):
        require(re.search(r"&gSettings\." + key + r",[^\n]*\"" + command + r"\" \}", wifi) is not None,
                f"{key} is switched with {command}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", help="accepted for the shared ctest arguments; the host c++ is used")
    parser.add_argument("--sanitize", action="store_true", help="accepted; unused")
    parser.parse_args()
    note = check_build()
    check_commands()
    check_defaults()
    print(f"portable web on demand: {note}; openwifi/openhttp registered, two refusals only, "
          "Wi-Fi and web server defaults on")


if __name__ == "__main__":
    main()
