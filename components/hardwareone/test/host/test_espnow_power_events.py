#!/usr/bin/env python3
"""Battery state and system events over the mesh, as production code.

POWER_STATUS (152) and EVENT_PUSH (132) were reserved numbers with no payload:
a master could not see a worker's battery or its alerts.

The production code of System_ESPNow_PowerEvents.cpp between
POWER_EVENTS_HOST_BEGIN and POWER_EVENTS_HOST_END (the sender step on
espnow_task, the two receive paths, the per-sender records, the event lists,
`espnowbattery` and `espnowevents` with their json forms) is compiled with the
production rules header System_ESPNow_PowerEventCore.h against a host device
(battery, event ring, sessions, radio queue with its acknowledgement watch):

  * a device that is not the acting master sends its battery state to each
    master named on it, the same frame to both: at once, every interval, on a
    change of state but not within 5 s, never with the interval 0, also with
    no valid sample; a session under 2 s old is not used; every frame's
    acknowledgement is watched;
  * a worker whose master lost the session has a new one within three battery
    intervals (two unacknowledged frames clear it, the next look starts one);
  * events: the floor for each setting and tier, the kind as its name, long
    texts cut with their flags, eight at once then one every 2 s with what was
    left out in `skipped` (stops at 65535), the cursor waiting without a
    session and following the head when nothing is sent, a full queue;
  * a worker refuses both frames, counted and said once a minute; a master
    and a backup master keep them, stamped with their own UTC clock (0 when
    not set) and uptime;
  * the counter rule for both: lost, late, repeated, a restart;
  * what the sender did not know is null in JSON; 24 and 128 bytes of 0xFF
    give valid JSON with "unknown" names and cleaned text; quotes and control
    bytes are escaped;
  * the stores: sixteen senders, the seventeenth refused and counted; 24
    events per mesh, the 25th replaces the first, `since` and `overwritten`,
    a `since` from before a restart, mesh 1 does not push out mesh 0, the
    receiver's rate limit per sender, a busy sender does not push out a quiet
    one; both JSON replies page (`next`, `more`) and are whole JSON within
    the reply buffer;
  * nothing that may block, print or allocate runs under the lock, and a
    received event is never posted into the receiver's own event ring;
  * the event counts of other devices (the status line words and the json of
    one device for espnowsensorstatus) are for an admin caller only; the
    battery state is for every user.

Source rules: rows 132 and 152 of the receive table ask for a paired sender
and a session-encrypted frame and nothing else (no bond gate); both may travel
through a relay and neither is flooded; the battery frame asks for an
acknowledgement and has it watched; the task step runs once in the espnow_task
loop, outside every bonded-mode block; the file needs no bond and reaches the
device only through its pe* functions; the two settings and the four commands
exist.

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
from test_espnow_sensor_wire import definition, depth_in, strip_comments  # noqa: F401

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

BEGIN = "// POWER_EVENTS_HOST_BEGIN"
END = "// POWER_EVENTS_HOST_END"


def production(component: Path) -> str:
    source = (component / "System_ESPNow_PowerEvents.cpp").read_text()
    require(source.count(BEGIN) == 1 and source.count(END) == 1,
            "one host-tested block in System_ESPNow_PowerEvents.cpp")
    return source[source.index(BEGIN):source.index(END)]


def check_sources(component: Path) -> None:
    espnow = (component / "System_ESPNow.cpp").read_text()
    table = strip_comments(extract_block(espnow, "static const V4OpcodeEntry kV4HandlerTable[]"))
    rows = {name: ({flag.strip() for flag in flags.split("|")}, handler)
            for name, flags, handler in re.findall(r"\{\s*ESPNOW_V4_TYPE_(\w+)\s*,([^,]*),\s*ESPNOW_V4_MINLEN_\w+\s*,\s*(\w+)\s*\}", table)}
    for name, handler in (("POWER_STATUS", "v4hPowerStatus"), ("EVENT_PUSH", "v4hEventPush")):
        require(name in rows, f"kV4HandlerTable has a {name} row")
        require(rows[name][0] == {"V4_OPC_FLAG_REQ_PAIRED", "V4_OPC_FLAG_REQ_SESSION_ENC"},
                f"the {name} row asks for a paired sender and a session-encrypted frame, nothing else (no bond gate): "
                f"{sorted(rows[name][0])}")
        require(rows[name][1] == handler, f"the {name} row names {handler}")
    flood = strip_comments(extract_block(espnow, "static inline bool v4_flood_type_allowed(uint8_t type)"))
    routed = strip_comments(definition(espnow, "static bool v4_routed_type_allowed(uint8_t type)"))
    for name in ("POWER_STATUS", "EVENT_PUSH"):
        require(f"case ESPNOW_V4_TYPE_{name}:" in routed, f"{name} may travel inside a relay envelope")
        require(name not in flood, f"{name} is not flooded")
    require(espnow.count("  espnowPowerEventsTick(now);") == 1, "the espnow_task loop runs the sender step once")
    require(depth_in(espnow, "  espnowPowerEventsTick(now);", "ENABLE_BONDED_MODE") == 0,
            "the sender step is outside every ENABLE_BONDED_MODE block")
    require(espnow.index("  sensorLinkTick(now);") < espnow.index("  espnowPowerEventsTick(now);"),
            "the sender step runs after sensorLinkTick, which looks up the named masters")

    source = (component / "System_ESPNow_PowerEvents.cpp").read_text()
    code = strip_comments(source)
    # A device that is not a bond worker needs no bond; a bond worker's
    # battery state and events go to its bond master only (180/182). The bond
    # is reached through the pe* functions, never from the host-tested block.
    hosted_code = strip_comments(production(component))
    for word in ("ENABLE_BONDED_MODE", "isBond", "bondPeer", "REQ_BOND", "gSettings.bond", "v4_send_bond",
                 "bondForward", "espnowForward"):
        require(word not in hosted_code, f"the host-tested block reaches the bond and forwarding only through pe* ({word})")
    for word in ("bondModeEnabledMesh", "bondRoleMesh", "bondPeerMacMesh"):
        require(word not in code, f"only the single-bond settings are read ({word})")
    require(re.search(r"\bsystemEventPost\b", code) is None,
            "a received event is never posted into this device's own event ring")
    hosted = strip_comments(production(component))
    for direct in ("Clock::", "gSettings", "millis()", "peerIdentity", "espnowtx::", "systemEvent", "getBatterySnapshot",
                   "ps_alloc", "broadcastOutput", "sendStatusRegister", "espnowSession", "notifKindTier"):
        require(direct not in hosted, f"the host-tested block reaches the device only through its pe* functions ({direct})")
    power_tx = strip_comments(extract_block(source, "static bool peSendPower(const uint8_t mac[6], const uint8_t* payload, uint16_t len)"))
    require("espnowtx::sendAead(mac, ESPNOW_V4_TYPE_POWER_STATUS, ESPNOW_V4_FLAG_ACK_REQ, msgId, payload, len, 1)" in power_tx and
            "sendStatusRegister(msgId, mac);" in power_tx,
            "the battery frame is a session unicast that asks for an acknowledgement and has it watched")
    event_tx = strip_comments(extract_block(source, "static bool peSendEvent(const uint8_t mac[6], const uint8_t* payload, uint16_t len)"))
    require("espnowtx::sendAead(mac, ESPNOW_V4_TYPE_EVENT_PUSH, 0," in event_tx, "the event frame is a session unicast")
    for handler, ingest, bond in (("V4Verdict v4hPowerStatus(const V4RxCtx& ctx)", "powerEventsIngestPower", "false"),
                                  ("V4Verdict v4hEventPush(const V4RxCtx& ctx)", "powerEventsIngestEvent", "false"),
                                  ("V4Verdict v4hBondPowerStatus(const V4RxCtx& ctx)", "powerEventsIngestPower", "true"),
                                  ("V4Verdict v4hBondEventPush(const V4RxCtx& ctx)", "powerEventsIngestEvent", "true")):
        require(f"{ingest}(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen, /*bondLink=*/{bond});" in
                extract_block(source, handler), f"{handler.split('(')[0].split()[-1]} hands the frame to {ingest}")
    require(depth_in(source, "V4Verdict v4hBondPowerStatus(const V4RxCtx& ctx) {", "ENABLE_BONDED_MODE") == 1,
            "the bond handlers exist in a bond build only")
    for name, handler in (("BOND_POWER_STATUS", "v4hBondPowerStatus"), ("BOND_EVENT_PUSH", "v4hBondEventPush")):
        require(name in rows and rows[name][0] == {"V4_OPC_FLAG_REQ_PAIRED", "V4_OPC_FLAG_REQ_BOND_PEER",
                                                   "V4_OPC_FLAG_REQ_SESSION_ENC"} and rows[name][1] == handler,
                f"the {name} row takes the frame from the bond partner over its session only, to {handler}")
    for ingest in ("static void powerEventsIngestPower(", "static void powerEventsIngestEvent("):
        body = strip_comments(extract_block(source[source.index(ingest):], ingest.rstrip("(")))
        require("peOriginOf(sender, senderName, bondLink," in body and "peAccept(origin, origin.name, nowMs)" in body and
                body.index("peOriginOf(") < body.index("peAccept("),
                f"{ingest.split()[2].rstrip('(')} asks whose the frame is, then the role rule and the record")
        require("if (bondLink && kept) peForward" in body, f"{ingest.split()[2].rstrip('(')} hands what it kept of the partner on")
    bond_tx = strip_comments(extract_block(espnow, "bool v4_send_bond_power(const uint8_t* dstMac, const uint8_t* payload, uint16_t len)"))
    require("bondSendEncryptedAsync(dstMac, ESPNOW_V4_TYPE_BOND_POWER_STATUS, 0," in bond_tx,
            "the bond battery frame goes through the bond send rules")
    bond_tx = strip_comments(extract_block(espnow, "bool v4_send_bond_event(const uint8_t* dstMac, const uint8_t* payload, uint16_t len)"))
    require("bondSendEncryptedAsync(dstMac, ESPNOW_V4_TYPE_BOND_EVENT_PUSH, 0," in bond_tx,
            "the bond event frame goes through the bond send rules")
    # Every hold of sPeMux (a spinlock: interrupts are masked while it is held)
    # touches a small piece of PSRAM: a full list is not searched under it,
    # and a walk of the events takes one list per hold.
    spans = re.findall(r"PE_LOCK\(\);(.*?)PE_UNLOCK\(\);", hosted, re.S)
    require(len(spans) >= 10, f"the lock holds of the host-tested block were found ({len(spans)})")
    for span in spans:
        require("eventListSlot(" not in span, "the entry a new event takes is worked out without the lock")
        require("kEventMeshes" not in span, "a walk of the events holds the lock for one list at a time")
    require("espnowsensor::eventOldestHeld(systemEventLatestSeq(), SYSEVT_RING_SIZE)" in
            strip_comments(extract_block(source, "static uint32_t peEventOldestSeq()")),
            "the oldest event the ring holds comes from the tested rule")
    require("espnowsensor::elapsedSinceMs(nowMs, state.lastReadMs)" in
            strip_comments(extract_block(source, "static void peBatteryInputs(espnowsensor::PowerInputs* in, uint32_t nowMs)")),
            "the battery sample's age is 0, not 49 days, when the sample is newer than the pass")
    accept = strip_comments(extract_block(source, "static PeCollectPeer* peAccept(const ForwardOrigin& origin, const char* senderName, uint32_t nowMs)"))
    require(accept.index("espnowsensor::roleReceives(peRole())") < accept.index("peAlloc("),
            "the role rule comes before anything is allocated")

    settings = (component / "System_Settings.h").read_text()
    require("uint16_t powerStatusInterval;" in settings and "uint8_t eventPush;" in settings, "the two settings exist")
    require('{ "powerStatusInterval", SETTING_U16, &gSettings.powerStatusInterval, 60, 0, nullptr, 0, 3600,' in espnow,
            "powerStatusInterval: default 60 s, 0 to 3600")
    require('{ "eventPush", SETTING_U8, &gSettings.eventPush, 1, 0, nullptr, 0, 3,' in espnow,
            "eventPush: default alert (1), 0 to 3")
    interval_cmd = strip_comments(extract_block(espnow, "const char* cmd_espnow_powerinterval(const String& a)"))
    require("espnowsensor::powerIntervalValid(seconds)" in interval_cmd, "espnowpowerinterval refuses 1 to 9 and over 3600")
    push_cmd = strip_comments(extract_block(espnow, "const char* cmd_espnow_eventpush(const String& a)"))
    require("espnowsensor::eventPushFromName(" in push_cmd, "espnoweventpush takes the words off, alert, standard, verbose")
    for name, handler in (("espnowbattery", "cmd_espnow_battery"), ("espnowevents", "cmd_espnow_events"),
                          ("espnowpowerinterval", "cmd_espnow_powerinterval"), ("espnoweventpush", "cmd_espnow_eventpush")):
        require(re.search(rf'\{{ "{name}", "[^\n]*, {handler}, "Usage: {name}', espnow) is not None,
                f"the command {name} is registered with its usage")
    # Who may read them: the events other devices sent (logins, failures) are
    # for an admin; the battery state is for any user. The local 'events'
    # command is a separate row and keeps its own flag.
    for name, handler, admin in (("espnowevents", "cmd_espnow_events", "true"),
                                 ("espnowbattery", "cmd_espnow_battery", "false")):
        require(re.search(rf'\{{ "{name}", "[^\n]*", {admin}, {handler}, "Usage: {name}', espnow) is not None,
                f"{name} is registered with admin = {admin}")
    sensors = (component / "System_ESPNow_Sensors.cpp").read_text()
    require('"  Battery state and events it received: espnowbattery, espnowevents (admin only)."' in sensors,
            "espnowsensorstatus points to espnowevents as admin only")
    # The event counts of other devices in espnowsensorstatus: an admin only,
    # as the caller's identity says; anyone else is told once.
    require("currentExecIsAdmin()" in strip_comments(extract_block(source, "static bool peCallerIsAdmin()")),
            "the event counts ask the identity the command runs under")
    one_json = strip_comments(extract_block(sensors, "static const char* sensorStatusOneDeviceJson(const String& whoArgs)"))
    require('if (!espnowEventCountsVisible()) doc["eventCounts"] = "admin only";' in one_json,
            "espnowsensorstatus json <device>: no events key for a user who is not an admin, and eventCounts says why")
    status_cmd = strip_comments(extract_block(sensors, "const char* cmd_espnow_sensorstatus(const String& argsInput)"))
    require(status_cmd.count('broadcastOutput("  Event counts: admin only.");') == 1 and
            "if (senderListed && !espnowEventCountsVisible())" in status_cmd,
            "espnowsensorstatus says once that event counts are for an admin")
    notif_h = (component / "System_Notifications.h").read_text()
    notif = (component / "System_Notifications.cpp").read_text()
    require("uint8_t notifKindTier(uint8_t kind);" in notif_h and "static uint8_t notifKindTier" not in notif,
            "the event tier is a declared function: the sender's floor uses the notification tiers")
    cmake = (component / "CMakeLists.txt").read_text()
    require("System_ESPNow_PowerEvents.cpp" in cmake, "the new source file is in the component's build list")
    wire = (component / "System_ESPNow_Wire.h").read_text()
    require("ESPNOW_V4_TYPE_EVENT_PUSH      = 132," in wire and "ESPNOW_V4_TYPE_POWER_STATUS    = 152," in wire,
            "the two opcodes have their numbers")
    require("ESPNOW_V4_TYPE_BOND_POWER_STATUS = 180," in wire and "ESPNOW_V4_TYPE_BOND_EVENT_PUSH = 182," in wire,
            "a bond worker's battery state and events are the bond frames 180 and 182")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    harness = (HERE / "espnow_power_events_harness.cpp").read_text()
    inserts = {
        "// INSERT_TARGETS_STRUCT": extract_block((component / "System_ESPNow_Sensors.h").read_text(),
                                                  "struct SensorReceiverTargets") + ";",
        "// INSERT_VIEW_STRUCT": extract_block((component / "System_ESPNow_PowerEvents.h").read_text(),
                                               "struct PowerEventsSendView") + ";",
        "// INSERT_PRODUCTION": production(component),
    }
    for marker, text in inserts.items():
        require(harness.count(marker) == 1, f"one {marker}")
        harness = harness.replace(marker, text)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function", f"-I{component}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-power-events-") as directory:
        source = Path(directory) / "power_events.cpp"
        binary = Path(directory) / "power_events"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=120)
    check_sources(component)
    print("battery state and events: sent to the named masters, kept by the master, never trusted as text",
          flush=True)


if __name__ == "__main__":
    main()
