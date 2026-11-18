#!/usr/bin/env python3
"""The bond master's side of forwarding its partner's data, as production code.

With 'bondforward on' a bond master announces to the masters named on it that
it forwards its bond partner's readings, battery state and events
(FORWARD_LINK, 155), and forwards them once a master accepted
(System_ESPNow_ForwardCore.h). The production code of
System_ESPNow_BondForward.cpp between BOND_FORWARD_HOST_BEGIN and
BOND_FORWARD_HOST_END (the tick, the answers and queries, the three forward
calls, 'bondforward', the status line and the JSON object) is compiled with
the production rules headers against a host bond, settings, named masters,
sessions with session ids and a recording radio:

  * off: once per session each named master hears "I forward for nobody";
    every reason in order (worker, partner not paired, never agreed, mesh
    off, the acting master, no master, none paired) with its reply;
    'bondforward sensors' by name, all, default, refused names; usage;
  * on: the primary hears readings, battery and events with the sensors and
    the partner's key and name, the backup master battery and events; the
    back-off; a wrong answer ignored; acceptance said once; nothing forwarded
    before it;
  * readings with X's header plus the bit and the link number, to the
    primary only, cut for the master's path (four routed parts of 472
    bytes); input left out; battery state to both, acknowledged, the byte;
    events to both, bit and byte; a frame from another source or after a
    partner change began is not filed under the link; off stops at once; a
    full queue; what could not be kept; the JSON and the status line; one
    console line a minute;
  * a rekey keeps forwarding; a new session announces again and holds the
    data until answered; a query; the refresh; a partner out of range keeps
    the link; a decline withdraws (three tries, an answered one not
    repeated), agreed again takes a new number; a partner change withdraws
    the old link and announces the new; stale; refused and its retry; a new
    named master; a handshake; promoted and demoted; the partner named as
    master is never announced to and gets no data;
  * review cases: the tick waits while a partner change runs (and drops a
    read a change began in), so a partner that never agreed is never
    announced; a master that became the partner hears the old link's
    withdrawal; another hold epoch with the same partner makes a new link;
    a sensor change while an announcement is out is announced at once and
    the older acceptance ignored; the longest bondstatus lines fit one
    console message each and the longest JSON object fits its cap;
  * nothing that may block, print or send runs under the lock.

Source rules: the tick runs in the espnow_task loop of a bond build, the
block reaches the device only through its bf* functions, the battery frame
is acknowledged and watched, the setting rows are in the "bond" group
(not remotely editable), the commands exist, and bondstatus and
/api/bond/status carry the forward object.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir>.
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
from test_espnow_sensor_wire import depth_in, strip_comments

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

BEGIN = "// BOND_FORWARD_HOST_BEGIN"
END = "// BOND_FORWARD_HOST_END"


def production(component: Path) -> str:
    source = (component / "System_ESPNow_BondForward.cpp").read_text()
    require(source.count(BEGIN) == 1 and source.count(END) == 1, "one host-tested block in System_ESPNow_BondForward.cpp")
    return source[source.index(BEGIN):source.index(END)]


def check_sources(component: Path) -> None:
    source = (component / "System_ESPNow_BondForward.cpp").read_text()
    hosted = strip_comments(production(component))
    for direct in ("gSettings", "millis()", "peerIdentity", "espnowtx::", "broadcastOutput", "ps_alloc", "malloc",
                   "getEspNowDeviceName", "bondAgreementView", "bondHoldEpochNow", "bondHeldSince", "espnowSession",
                   "sendStatusRegister"):
        require(direct not in hosted, f"the host-tested block reaches the device only through its bf* functions ({direct})")
    spans = re.findall(r"BF_LOCK\(\);(.*?)BF_UNLOCK\(\);", hosted, re.S)
    require(len(spans) >= 5, f"the lock holds were found ({len(spans)})")
    for span in spans:
        for call in ("bfSend", "bfConsole(", "bfName(", "bfHandshake(", "bfBond("):
            require(call not in span, f"nothing that may send, print or look up runs under the lock ({call})")
    code = strip_comments(source)
    for word in ("bondModeEnabledMesh", "bondRoleMesh", "bondPeerMacMesh"):
        require(word not in code, f"a bond is 1:1: only the single-bond settings are read ({word})")
    power = strip_comments(extract_block(source, "static bool bfSendPower(const uint8_t mac[6], const uint8_t* payload, uint16_t len)"))
    require("ESPNOW_V4_TYPE_POWER_STATUS, ESPNOW_V4_FLAG_ACK_REQ" in power and "sendStatusRegister(msgId, mac);" in power,
            "a forwarded battery frame is acknowledged and watched, as the device's own")
    sid = strip_comments(extract_block(source, "static uint16_t bfSessionId(const uint8_t mac[6])"))
    require("espnowSessionIdOf(mac)" in sid, "a new session is told apart from a rekey by its sessionId")
    epoch = strip_comments(extract_block(source, "static uint32_t bfEpoch()"))
    require("bondHoldEpochNow()" in epoch, "a partner change is seen by the bond hold epoch")
    held = strip_comments(extract_block(source, "static bool bfHeld(uint32_t epoch)"))
    require("bondHeldSince(epoch)" in held, "the tick waits for a partner change as every bond step does (bondTickHeld)")
    tick = strip_comments(extract_block(source, "void bondForwardTick(uint32_t nowMs)"))
    require(tick.index("if (bfHeld(epoch)) return;") < tick.index("bfBond(&b);") <
            tick.index("if (bfHeld(epoch) || b.epoch != epoch) return;") < tick.index("agreementAllows("),
            "the tick reads the bond only outside a partner change, and drops what it read when one began meanwhile")
    espnow = (component / "System_ESPNow.cpp").read_text()
    require(depth_in(espnow, "  bondForwardTick(now);", "ENABLE_BONDED_MODE") == 1, "the tick runs in a bond build")
    sid_def = strip_comments(extract_block(espnow, "uint16_t espnowSessionIdOf(const uint8_t* mac)"))
    require("s->state != SESSION_ACTIVE" in sid_def and "s->sessionId" in sid_def, "espnowSessionIdOf: the ACTIVE session's id")
    for key, kind, default in (("bondForwardToMesh", "SETTING_BOOL", "false"), ("bondForwardSensors", "SETTING_U16", "0x07EF")):
        row = re.search(rf'\{{ "{key}", {kind}, &gSettings\.{key}, {default}, [^\n]*"bond", "(\w+)" \}}', espnow)
        require(row is not None, f"the setting {key} is in the 'bond' group with default {default}")
    for name, handler in (("bondforward", "cmd_bond_forward"), ("bondforwardsensors", "cmd_bond_forwardsensors")):
        require(re.search(rf'\{{ "{name}",\s*"[^\n]*", true, {handler}, "Usage: {name}', espnow) is not None,
                f"{name} is an admin command with its usage")
    status = strip_comments(extract_block(espnow, "const char* cmd_bond_status(const String& argsInput)"))
    require("bondForwardStatusJson(forward, sizeof(forward))" in status and
            "bondForwardStatusLines(forwardLines, sizeof(forwardLines));" in status,
            "bondstatus and bondstatus json carry forwarding")
    require("EXT_RAM_BSS_ATTR static char forward[espnowfwd::kStatusJsonCap];" in status and
            "EXT_RAM_BSS_ATTR static char forwardLines[espnowfwd::kStatusLinesCap];" in status,
            "bondstatus keeps the forward object and lines in PSRAM, sized for the longest")
    require("serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);" in status,
            "bondstatus json is written into the whole reply buffer (the forward object alone may pass 2 KB)")
    lines_at = status.index("bondForwardStatusLines(forwardLines")
    require("broadcastOutput(line);" in status[lines_at:] and "strchr(line, '\\n')" in status[lines_at:],
            "each bondstatus forwarding line is one console message (at most 255 bytes)")
    require('doc["sensorRequestsRefused"]' in status and "gEspNow->sensorReqRefusedBond" in status,
            "the sensor requests a bond worker refused are shown by bondstatus and bondstatus json")
    web = (component / "WebPage_Bond.cpp").read_text()
    require("bondForwardStatusJson(forward, sizeof(forward))" in web, "GET /api/bond/status carries the forward object")
    require("EXT_RAM_BSS_ATTR static char forward[espnowfwd::kStatusJsonCap];" in web,
            "GET /api/bond/status keeps the forward object in PSRAM, sized for the longest")
    bonded = (component / "System_BondedPeer.cpp").read_text()
    require('strcmp(moduleName, "bond") == 0' in bonded, "the 'bond' group is not remotely editable")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    source = (component / "System_ESPNow_BondForward.cpp").read_text()
    harness = (HERE / "espnow_bond_forward_harness.cpp").read_text()
    inserts = {
        "// INSERT_TARGETS_STRUCT": extract_block((component / "System_ESPNow_Sensors.h").read_text(),
                                                  "struct SensorReceiverTargets") + ";",
        "// INSERT_BOND_STRUCT": extract_block(source, "struct BfBond") + ";",
        "// INSERT_PRODUCTION": production(component),
    }
    for marker, text in inserts.items():
        require(harness.count(marker) == 1, f"one {marker}")
        harness = harness.replace(marker, text)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function", f"-I{component}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-bond-forward-") as directory:
        src = Path(directory) / "bond_forward.cpp"
        binary = Path(directory) / "bond_forward"
        src.write_text(harness)
        subprocess.run([args.cxx, *flags, str(src), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=120)
    check_sources(component)
    print("forwarding, bond master side: announced, accepted, forwarded with the partner's numbers, withdrawn", flush=True)


if __name__ == "__main__":
    main()
