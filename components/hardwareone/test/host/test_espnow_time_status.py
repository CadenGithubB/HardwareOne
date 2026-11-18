#!/usr/bin/env python3
"""Mesh time (TIME_SYNC): the master sends its clock, a device without a better source takes it.

It used to be broken: `espnowtimesync` sent (epoch, the sender's millis()) in a
signed broadcast, the receiver stored the sender's millis() as an "offset", set
no clock and `espnowtimestatus` said "Synced: Yes".

The production code of System_ESPNow_MeshTime.cpp between MESH_TIME_HOST_BEGIN
and MESH_TIME_HOST_END (what the handler keeps, the main-loop apply tick, the
sender step on espnow_task, `espnowtimesync` and `espnowtimestatus` with its
json form) is compiled with the production rules header
System_ESPNow_TimeCore.h and the real Clock::isSynced / Clock::isValidEpoch of
System_Clock.cpp against a settable clock:

  * an applied frame sets the system clock to the frame's time plus its wait
    and goes through the clock chokepoint once, with the mesh source; an
    ignored, refused or held frame sets nothing;
  * a device whose clock comes from rtc, ntp, timeset or cm5 ignores mesh
    time; ring time and a clock carried over a restart are replaced;
  * a clock set by another source between the decision and the step is not
    overwritten;
  * only the acting master sends, only from a set clock with a real source
    (not 55 hours after a boot without one, not ring, not mesh, not carried
    over a restart, not an unconfirmed NTP jump); a round at once, every 10
    minutes, after a clock step and 15 s after that, and by hand; a device
    whose session came up gets a frame once the session is 2 s old; one
    device per half second; a full queue is tried again;
  * the master starts a session with a paired device that has none;
  * the status text and JSON say the true source and age in each state;
  * a paired device that sends TIME_SYNC all the time cannot keep the named
    master's frame (or, with no master named, another sender's) from being
    judged; mesh mode off takes no time; with no master named a set clock is
    changed at most once in 10 minutes; a hostile time is refused with no
    difference computed; the "held" console line comes once a minute at most;
  * the frame names the master's own source and the minutes since it was set;
  * which sources may be written into the RTC chip (Clock::sourceWritesRtc).

Source rules: the old state and sender are gone from the component; row 35 of
the receive table asks for a paired sender and a session-encrypted frame;
TIME_SYNC is not flooded and may travel through a relay; the clock module
names the mesh source, does not write mesh time into the RTC chip and does not
take it as a reference for the confirmed clock; the main loop applies mesh
time after the SNTP report and before the clock duties.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
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

BEGIN = "// MESH_TIME_HOST_BEGIN"
END = "// MESH_TIME_HOST_END"
GONE = ("gTimeOffset", "gTimeIsSynced", "sLastTimeSyncMs", "TIME_SYNC_INTERVAL", "PAYLOAD_TIME_SYNC",
        "v4_broadcast_time_sync", "v4h_time_sync")


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def production(component: Path) -> str:
    source = (component / "System_ESPNow_MeshTime.cpp").read_text()
    require(source.count(BEGIN) == 1 and source.count(END) == 1, "one host-tested block in System_ESPNow_MeshTime.cpp")
    return source[source.index(BEGIN):source.index(END)]


def clock_questions(component: Path) -> str:
    clock = (component / "System_Clock.cpp").read_text()
    threshold = re.search(r"^static constexpr int kSyncedTmYearThreshold = \d+;", clock, re.M)
    require(threshold is not None, "System_Clock.cpp holds the year threshold")
    header = (component / "System_Clock.h").read_text()
    sources = re.search(r"^enum SyncSource : uint8_t \{[^}]*\};", header, re.M)
    writes = re.search(r"^constexpr bool sourceWritesRtc\(SyncSource s\) \{[^\n]*\}$", header, re.M)
    require(sources is not None and writes is not None, "System_Clock.h holds SyncSource and sourceWritesRtc")
    return "\n".join([threshold.group(0), sources.group(0), writes.group(0), extract_block(clock, "bool isSynced()"),
                      extract_block(clock, "bool isValidEpoch(time_t t)")])


def check_sources(component: Path) -> None:
    for path in sorted(list(component.glob("*.cpp")) + list(component.glob("*.h"))):
        code = strip_comments(path.read_text())
        for name in GONE:
            require(re.search(rf"\b{name}\b", code) is None, f"{path.name}: {name} is gone")
    espnow = (component / "System_ESPNow.cpp").read_text()
    table = strip_comments(extract_block(espnow, "static const V4OpcodeEntry kV4HandlerTable[]"))
    row = re.search(r"\{\s*ESPNOW_V4_TYPE_TIME_SYNC\s*,([^,]*),\s*(\w+)\s*\}", table)
    require(row is not None, "kV4HandlerTable has a TIME_SYNC row")
    flags = {flag.strip() for flag in row.group(1).split("|")}
    require(flags == {"V4_OPC_FLAG_REQ_PAIRED", "V4_OPC_FLAG_REQ_SESSION_ENC"},
            f"the TIME_SYNC row asks for a paired sender and a session-encrypted frame, nothing else: {sorted(flags)}")
    require(row.group(2) == "v4hTimeSync", "the TIME_SYNC row names v4hTimeSync")
    flood = strip_comments(extract_block(espnow, "static inline bool v4_flood_type_allowed(uint8_t type)"))
    require("TIME_SYNC" not in flood, "TIME_SYNC is not flooded")
    # (a forward declaration of the same name comes first in the file)
    routed_at = espnow.index("static bool v4_routed_type_allowed(uint8_t type) {")
    routed = strip_comments(extract_block(espnow[routed_at:], "static bool v4_routed_type_allowed(uint8_t type)"))
    require("case ESPNOW_V4_TYPE_TIME_SYNC:" in routed, "TIME_SYNC may travel inside a relay envelope")
    require(espnow.count("espnowMeshTimeSendTick(now);") == 1, "the espnow_task loop runs the sender step once")

    mesh = (component / "System_ESPNow_MeshTime.cpp").read_text()
    code = strip_comments(mesh)
    require(code.count("settimeofday(") == 1 and code.count("Clock::clockStepped(Clock::SYNC_MESH, before)") == 1,
            "one place sets the clock from mesh time, through the chokepoint with the mesh source")
    setter = extract_block(mesh, "static void mtSetClock(int64_t epochMs)")
    require(setter.index("settimeofday(") < setter.index("Clock::clockStepped("),
            "the chokepoint is called after settimeofday")
    require("ESPNOW_V4_TYPE_TIME_SYNC, 0," in extract_block(mesh, "static bool mtSend("),
            "the frame goes out as a session unicast with no flags")
    handler = extract_block(mesh, "void v4hTimeSync(const V4RxCtx& ctx)")
    require("meshTimeKeepFrame(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);" in handler and
            "settimeofday" not in handler, "the handler only keeps the frame for the main loop")
    hosted = strip_comments(production(component))
    for direct in ("Clock::", "gSettings", "millis()", "esp_timer", "peerIdentity", "espnowtx::", "time(nullptr)"):
        require(direct not in hosted, f"the host-tested block reaches the device only through its mt* functions ({direct})")

    clock = (component / "System_Clock.cpp").read_text()
    require('case SYNC_MESH:   return "mesh";' in clock, "syncSourceName names the mesh source")
    stepped = strip_comments(extract_block(clock, "void clockStepped(SyncSource src, time_t before)"))
    require("sourceWritesRtc(src) && nowValid" in stepped, "mesh time is not written into the RTC chip")
    rtc = (component / "i2csensor_ds3231.cpp").read_text()
    task = strip_comments(extract_block(rtc, "void rtcTask(void* pvParameters)"))
    require(task.count("rtcSyncFromSystem()") == 1 and
            "Clock::sourceWritesRtc(Clock::syncSource()) && rtcSyncFromSystem()" in task,
            "the RTC task calibrates an uncalibrated chip from the system clock only when its source may be written "
            "(not mesh time, not a clock carried over a restart)")
    require("src != SYNC_RING && src != SYNC_MESH && src != SYNC_NONE" in stepped,
            "mesh time is no reference for the confirmed clock")
    require("sStepSerial++;" in stepped, "every step is counted (Clock::stepSerial)")
    header = (component / "System_Clock.h").read_text()
    require("SYNC_CM5, SYNC_MESH };" in header, "SYNC_MESH is the last clock source")

    loop = (component / "HardwareOne.cpp").read_text()
    ntp = loop.index("  ntpSyncDrainTick();")
    apply_at = loop.index("  espnowMeshTimeApplyTick();")
    duties = loop.index("  Clock::clockDutiesTick();")
    require(ntp < apply_at < duties,
            "the main loop applies mesh time after the SNTP report and before the clock duties")
    require(loop.count("espnowMeshTimeApplyTick();") == 1, "one call of the apply tick")

    wire = (component / "System_ESPNow_Wire.h").read_text()
    struct = strip_comments(extract_block(wire, "struct __attribute__((packed)) V4PayloadTimeSync"))
    fields = re.findall(r"^\s*(\w+)\s+(\w+)(\[\d+\])?;", struct, re.M)
    require([(kind, name, size) for kind, name, size in fields] ==
            [("int64_t", "epochMs", ""), ("uint8_t", "source", ""), ("uint8_t", "flags", ""),
             ("uint16_t", "sinceSetMin", ""), ("uint8_t", "reserved", "[4]")],
            f"V4PayloadTimeSync is epochMs, source, flags, sinceSetMin, reserved[4]: {fields}")
    require("static_assert(sizeof(V4PayloadTimeSync) == 16" in wire, "its size is asserted")

    settings = (component / "System_Settings.h").read_text()
    require("bool meshTime;" in settings, "the setting meshTime exists")
    require('{ "meshTime", SETTING_BOOL, &gSettings.meshTime, true,' in espnow, "meshTime is on by default")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    harness = (HERE / "espnow_time_status_harness.cpp").read_text()
    for marker in ("// INSERT_PRODUCTION", "// INSERT_CLOCK"):
        require(harness.count(marker) == 1, f"one {marker}")
    harness = harness.replace("// INSERT_CLOCK", clock_questions(component))
    harness = harness.replace("// INSERT_PRODUCTION", production(component))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", f"-I{component}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-time-status-") as directory:
        source = Path(directory) / "time_status.cpp"
        binary = Path(directory) / "time_status"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)
    check_sources(component)
    print("mesh time: the master's clock reaches a device without a better source, and the status tells the truth",
          flush=True)


if __name__ == "__main__":
    main()
