#!/usr/bin/env python3
"""The mesh master's side of forwarding for a bond partner, as production code.

A bond master may forward its bond partner's readings, battery state and
events to the masters named on it (System_ESPNow_ForwardCore.h). The
production code of System_ESPNow_ForwardRx.cpp between FORWARD_RX_HOST_BEGIN
and FORWARD_RX_HOST_END (the link table, the FORWARD_LINK handler,
espnowForwardResolve and the readers, where a sensor request for a forwarded
partner goes, and espnowforwardlinks) is compiled with the production rules
header against a host device (role, paired devices with keys and names, a
recording radio):

  * a worker answers not_receiver; a master accepts any paired device that
    announces (there is no list of devices that may forward), also one
    paired again under another key;
  * every announce check: the sender or this device as partner, no key, a
    name taken by a paired device or this device, a paired partner under
    another key (key_mismatch) or the same (this device's own name shown);
  * a new partner replaces the old link and the old frames are refused; a
    late announcement of the old partner, or of an ended link, is stale;
  * whose a frame is: own (no lookup), over the bond link, forwarded (the
    partner via the sender, the link's name, the resume once per stream), the
    battery frame's byte alone, the sensor status's byte alone (never a
    resume, malformed on the bond link), malformed marks, unannounced and old link
    numbers refused with a query at most every 5 s (none from a worker), a
    stream not announced refused without a query;
  * withdraw and the withdrawal of everything; a worker's links end, a
    master's tick ends none;
  * a sensor request for a partner: '<partner> via <device>', the partner's
    name alone, addresses; an unknown via, a device that forwards nothing for
    it, no link, an ended link; the current link number;
  * sixteen entries, a seventeenth refused while every holder is paired, an
    unpaired one's entry reused; no memory: full;
  * espnowforwardlinks in text and JSON, paged, "not confirmed" after 30
    minutes; a query or answer goes to the bond master's side;
  * nothing that may block, print, send or allocate runs under the lock.

Source rules: the row of 155 and its tick are outside every bonded-mode
block, the handler hands the frame to the host-tested ingest, the block
reaches the device only through its fwd* functions, espnowforwardlinks
exists, the allow list (espnowforwardfrom, its setting, its key binding) is
gone, and espnowsensorreq sends a request for a forwarded partner to the
device that forwards for it with that link's number.

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

BEGIN = "// FORWARD_RX_HOST_BEGIN"
END = "// FORWARD_RX_HOST_END"


def production(component: Path) -> str:
    source = (component / "System_ESPNow_ForwardRx.cpp").read_text()
    require(source.count(BEGIN) == 1 and source.count(END) == 1, "one host-tested block in System_ESPNow_ForwardRx.cpp")
    return source[source.index(BEGIN):source.index(END)]


def check_sources(component: Path) -> None:
    source = (component / "System_ESPNow_ForwardRx.cpp").read_text()
    hosted = strip_comments(production(component))
    for direct in ("gSettings", "millis()", "peerIdentity", "espnowtx::", "broadcastOutput", "ps_alloc",
                   "getEspNowDeviceName", "resolveDeviceNameOrMac", "bondForward", "ENABLE_BONDED_MODE"):
        require(direct not in hosted, f"the host-tested block reaches the device only through its fwd* functions ({direct})")
    spans = re.findall(r"FWD_LOCK\(\);(.*?)FWD_UNLOCK\(\);", hosted, re.S)
    require(len(spans) >= 10, f"the lock holds of the host-tested block were found ({len(spans)})")
    for span in spans:
        for call in ("fwdSend(", "fwdConsole(", "fwdAlloc(", "fwdIsPaired(", "fwdPairedKey(", "fwdNameTaken(",
                     "fwdPeerName(", "fwdNameOf(", "fwdQueryIfDue(", "fwdEntryFor("):
            require(call not in span, f"nothing that may send, print, allocate or look up pairing runs under the lock ({call})")
    require("forwardLinkIngest(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen);" in
            extract_block(source, "void v4hForwardLink(const V4RxCtx& ctx)"), "v4hForwardLink hands the frame to the ingest")
    send = strip_comments(extract_block(source, "static bool fwdSend(const uint8_t mac[6], const uint8_t* payload, uint16_t len)"))
    require("espnowtx::sendAead(mac, ESPNOW_V4_TYPE_FORWARD_LINK, 0, generateMessageId(), payload, len, 1)" in send,
            "a forward-link frame is a session unicast")
    bond_side = strip_comments(extract_block(source, "static bool fwdToBondSide(const uint8_t src[6], const espnowfwd::LinkFrame& f)"))
    require("bondForwardOnLinkFrame(src, f)" in bond_side, "a query or answer goes to the bond master's side in a bond build")
    require(strip_comments(source).count("ENABLE_BONDED_MODE") == 2, "only the include and the bond side are bond-mode code")
    espnow = (component / "System_ESPNow.cpp").read_text()
    require(depth_in(espnow, "  { ESPNOW_V4_TYPE_FORWARD_LINK,", "ENABLE_BONDED_MODE") == 0,
            "the FORWARD_LINK row is in every build")
    require(re.search(r'\{ "espnowforwardlinks", "[^\n]*", false, cmd_espnow_forwardlinks, "Usage: espnowforwardlinks',
                      espnow) is not None, "espnowforwardlinks is registered (not admin) with its usage")
    # Owner decision 2026-11-18: no allow list on the mesh master.
    bonded = (component / "System_BondedPeer.cpp").read_text()
    settings = (component / "System_Settings.h").read_text()
    core = (component / "System_ESPNow_ForwardCore.h").read_text()
    for name, text in (("System_ESPNow.cpp", espnow), ("System_BondedPeer.cpp", bonded), ("System_Settings.h", settings),
                       ("System_ESPNow_ForwardRx.cpp", source), ("System_ESPNow_ForwardCore.h", strip_comments(core))):
        for word in ("forwardFrom", "ForwardFrom", "espnowforwardfrom", "AllowEntry", "allowList", "kResultNotAllowed"):
            require(word not in strip_comments(text), f"the allow list is gone ({word} in {name})")
    sensorreq = strip_comments(extract_block(espnow, "const char* cmd_espnow_sensorreq(const String& argsInput)"))
    require('a.arg(1) == "via"' in sensorreq and
            "espnowForwardRequestTarget(a.arg(0).c_str(), a.arg(2).c_str(), mac, origin, &linkGen)" in sensorreq and
            "espnowForwardRequestTarget(a.arg(0).c_str(), nullptr, mac, origin, &linkGen)" in sensorreq and
            "rq.fwdLink    = linkGen;" in sensorreq,
            "espnowsensorreq names a forwarded partner ('via', or its name alone) and sends the link number")
    require(sensorreq.index("resolveDeviceNameOrMac(a.arg(0), mac)") <
            sensorreq.index("espnowForwardRequestTarget(a.arg(0).c_str(), nullptr"),
            "a device paired here is asked directly; a forwarded partner only when no paired device has that name")
    # Bench 2026-11-18: the sensors and the mode in words too, by the rules of
    # the core (parseSensorList, parseRequestMode); every bad token is named.
    require("espnowsensor::parseSensorList(sensorsArg.c_str(), sensorMask, bad, sizeof(bad))" in sensorreq and
            "espnowsensor::parseRequestMode(modeArg.c_str(), mode)" in sensorreq and
            "rq.sensorMask = sensorMask;" in sensorreq and "strtoul(" not in sensorreq and
            "kCommandReplyNotANumber" not in sensorreq,
            "espnowsensorreq takes sensor names, 'all' or a mask, and the mode as a word or a number")
    for text in ("is not a sensor (", "is not a sensor mask (", "an empty sensor name in", "is not a mode (subscribe|sub|0",
                 "Error: intervalMs '%s' is not a number", "Error: leaseMs '%s' is not a number"):
        require(text in sensorreq, f"espnowsensorreq names the bad token ({text})")
    usage = "Usage: espnowsensorreq <MAC|name> [via <MAC|name>] <sensors> <mode> <intervalMs> <leaseMs>"
    require(espnow.count(usage) == 2 and "mode: subscribe|sub|0, unsubscribe|unsub|1, oneshot|once|2." in sensorreq,
            "the usage (command and registry row) names the words")
    cmake = (component / "CMakeLists.txt").read_text()
    require("System_ESPNow_ForwardRx.cpp" in cmake and "System_ESPNow_BondForward.cpp" in cmake,
            "the new sources are in the component's build list")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    harness = (HERE / "espnow_forward_rx_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one // INSERT_PRODUCTION")
    harness = harness.replace("// INSERT_PRODUCTION", production(component))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function", f"-I{component}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-forward-rx-") as directory:
        source = Path(directory) / "forward_rx.cpp"
        binary = Path(directory) / "forward_rx"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=120)
    check_sources(component)
    print("forwarding, mesh master side: links announced, accepted, refused and ended; whose a frame is", flush=True)


if __name__ == "__main__":
    main()
