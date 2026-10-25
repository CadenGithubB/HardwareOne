#!/usr/bin/env python3
"""Exercise the production ring daily packetAck decision against the Health history model.

ringProcessRxFrame, ringExtractTelemetryCache, ringReasmFinalize and the RX
duplicate/ACK-lane helpers (G2_Ring.cpp) run with the production pending-session
model (G2_Health.cpp) and the production profile capabilities and packetAck
descriptor factories (System_R1_Protocol.cpp). The frame decoder, the typed
daily parsers, RTOS locks, logging and the setup/transaction/control handlers
are doubles. No BLE link, ring firmware, owner-loop scheduling or store commit
is simulated.
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


def optional(source: str, marker: str) -> list[str]:
    """A definition the fixed sources add; absent on 0.99.97.4."""
    return [extract_block(source, marker)] if marker in source else []


def definition(source: str, signature: str) -> str:
    """Extract the definition of `signature`, skipping forward declarations."""
    start = 0
    while True:
        index = source.index(signature, start)
        close = source.index(")", index)
        if source[close + 1:].lstrip().startswith("{"):
            return extract_block(source[index:], signature)
        start = index + len(signature)


def optional_line(source: str, pattern: str) -> list[str]:
    """A declaration the fixed sources add; absent on older trees."""
    match = re.search(pattern, source, re.M)
    return [match[0]] if match else []


def line(source: str, pattern: str) -> str:
    match = re.search(pattern, source, re.M)
    require(match is not None, f"production declaration not found: {pattern}")
    return match[0]


def generated(component: Path, source_dir: Path | None) -> tuple[str, dict[str, str]]:
    def read(name: str) -> str:
        overlay = source_dir / name if source_dir is not None else None
        return (overlay if overlay is not None and overlay.exists() else component / name).read_text()

    ring = read("G2_Ring.cpp")
    ring_h = read("G2_Ring.h")
    health = read("G2_Health.cpp")
    protocol = read("System_R1_Protocol.cpp")
    store = read("R1_HealthHistoryStore.cpp")
    headers = {name: read(name) for name in (
        "G2_Health.h", "R1_HealthHistoryStore.h", "System_R1_Protocol.h")}

    types = [*(extract_block(ring_h, f"enum {name}") + ";" for name in (
                 "G2RingTransactionState", "G2RingTransactionError", "G2RingObservedState")),
             line(ring, r"^static constexpr uint8_t\s+RING_PACKET_ACK_DEPTH = \d+;"),
             line(ring, r"^static constexpr uint8_t\s+RING_RX_DUPLICATE_DEPTH = \d+;"),
             *(extract_block(ring, marker) + ";" for marker in (
                 "enum RingControlTarget", "struct RingPacketAckPending", "struct RingActivePacketAck",
                 "struct RingRxFingerprint", "struct RingReassembly", "struct R1TelemetryCache",
                 "enum RingTelemetrySource")),
             extract_block(health, "struct HistoryPeerCustody") + ";",
             extract_block(health, "struct PendingHistorySession") + ";",
             *(line(health, pattern) for pattern in (
                 r"^static std::atomic<bool> sHistoryDirty\{false\};",
                 r"^static std::atomic<uint8_t> sTypedTrendSuppressMask\{0\};",
                 r"^static std::atomic<bool> sTypedHistoryFetchActive\{false\};",
                 r"^static HistoryPeerCustody sHistoryPeerCustody;",
                 r"^static bool sDropCurrentHistoryFetch = false;",
                 r"^static R1HealthHistoryFetchState sHistoryUiFetchState[^\n]*",
                 r"^static uint8_t sHistoryUiFetchError[^\n]*",
                 r"^EXT_RAM_BSS_ATTR static PendingHistorySession sPendingHistory;")),
             *optional_line(health, r"^static std::atomic<bool> sHistoryBacklogPending\{false\};"),
             *optional_line(ring, r"^static constexpr uint32_t RING_HISTORY_SETTLE_QUIET_MS = \d+;"),
             *optional_line(ring, r"^static constexpr uint32_t RING_HISTORY_SETTLE_MAX_MS = \d+;"),
             *optional_line(ring, r"^static uint32_t sLastHealthPageRxMs = 0;"),
             *optional_line(ring, r"^static constexpr uint32_t RING_DEVICE_STATUS_POLL_MS = [^;]+;"),
             *optional_line(ring, r"^static constexpr uint32_t RING_DEVICE_STATUS_RETRY_MS = [^;]+;"),
             *[block + ";" for block in optional(ring, "struct RingDeviceStatusPollState")],
             *optional(health, "static bool latestInDay(")]
    if "struct G2RingTelemetry {" in ring_h and "struct RingTelemetrySnapshot {" in ring:
        types += [extract_block(ring_h, "struct G2RingTelemetry") + ";",
                  extract_block(ring, "struct RingTelemetrySnapshot") + ";"]

    first_capability = next(protocol.index(name) for name in (
        "bool r1DailyHistoryDataCmd(", "bool r1Captured229DailyDataCmd(") if name in protocol)
    capabilities = protocol[protocol.rindex("namespace {", 0, first_capability):
                            protocol.index("const char* r1ParseErrorName(")]
    from_decoded = extract_block(protocol, "bool r1PacketAckDescriptorFromDecoded(R1ProtocolProfile profile,")
    ack_factory = protocol[protocol.index("R1ParseError r1ValidateDecoded(const R1Decoded& decoded)"):
                           protocol.index(from_decoded) + len(from_decoded)]
    protocol_parts = [capabilities, ack_factory,
                      extract_block(protocol, "bool r1MakeDailyPacketAckDescriptor(")]

    production = [extract_block(health, marker) for marker in (
        "static bool historyPeerIdValid(", "static bool validateCommonDaily(",
        "static bool validateHrvDaily(", "static bool validateActivityDaily(",
        "static R1HealthHistoryLayout historyLayoutForProfile(", "static bool pendingKeyAcceptLocked(",
        "template <typename Result>\nstatic void mergePendingHourlyResult(",
        "static void mergePendingActivityResult(", "bool g2HealthApplyCommonDaily(",
        "bool g2HealthApplyHrvDaily(", "bool g2HealthApplyActivityDaily(",
        "void g2HealthHistoryFetchStarted(", "void g2HealthHistoryFetchFinished(")]
    production += [*optional(health, "static void mergeSleepRecord("),
                   *optional(health, "static uint32_t sleepRecordDayStart("),
                   *optional(health, "bool g2HealthApplySleepDaily("),
                   *optional(health, "bool g2HealthHistoryBacklogPending("),
                   *optional(store, "bool r1HealthSleepRecordValid(")]
    production += [extract_block(ring, "static bool ringFingerprintEqual("),
                   extract_block(ring, "static bool ringRememberRx("),
                   extract_block(ring, "static void ringQueuePacketAck("),
                   *optional(ring, "static bool ringDailyAckWaitsForHistory("),
                   definition(ring, "static bool ringDeviceStatusPayloadValid("),
                   extract_block(ring, "static bool ringDailyPayloadValid("),
                   extract_block(ring, "static void ringCacheDailyLatest(const R1CommonDailyResult& daily)"),
                   extract_block(ring, "static void ringCacheDailyLatest(const R1HrvDailyResult& daily)"),
                   *optional(ring, "static uint32_t ringDailyPageEndTs("),
                   *optional(ring, "static bool ringPageHoldsNewest("),
                   *optional(ring, "static bool ringTelemetryDirectIdentityCurrent("),
                   *optional(ring, "static int32_t ringSampleAgeSec("),
                   *optional(ring, "static void ringPopulateTelemetryOutput("),
                   *optional(ring, "static bool ringHistorySweepSettled("),
                   *optional(ring, "const char* g2RingWearName("),
                   *optional(ring, "static bool ringDeviceStatusPollDue("),
                   *optional(health, "static const char* wearLabel("),
                   definition(ring, "static void ringExtractTelemetryCache("),
                   extract_block(ring, "static bool ringReasmFinalizeCurrentLocked("),
                   definition(ring, "static void ringReasmFinalize("),
                   extract_block(ring, "static void ringProcessRxFrame(")]

    harness = (HERE / "ring_daily_ack_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", types), ("// INSERT_PROTOCOL", protocol_parts),
                         ("// INSERT_PRODUCTION", production)):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, "\n\n".join(body))
    return harness, headers


def check_wear_wording(component: Path, source_dir: Path | None) -> None:
    """Every wear display reads g2RingWearName (the web page its JSON wearName)."""
    def read(name: str) -> str:
        overlay = source_dir / name if source_dir is not None else None
        return (overlay if overlay is not None and overlay.exists() else component / name).read_text()

    for name, shared in (("G2_Health.cpp", "g2RingWearName("),
                         ("OLED_Mode_R1_Health.cpp", "g2RingWearName("),
                         ("System_SensorLogging.cpp", "g2RingWearName("),
                         ("WebPage_R1_Health.cpp", "s.wearName")):
        text = read(name)
        require(shared in text, f"{name} does not use the shared wear wording")
        require(re.search(r"\bwear\s*={2,3}\s*\d|r1Wear\s*==\s*\d", text) is None,
                f"{name} maps wear codes itself")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    parser.add_argument("--source-dir", type=Path,
                        help="directory whose files overlay --component file by file")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    check_wear_wording(args.component, args.source_dir)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-parameter",
             # Static helpers the fixed or base path no longer calls, and locals
             # read only by the compiled-out debug macros.
             "-Wno-unused-function", "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-ring-daily-ack-") as directory:
        work = Path(directory)
        unit, headers = generated(args.component, args.source_dir)
        for name, text in headers.items():
            (work / name).write_text(text)
        (work / "System_BuildConfig.h").write_text(
            "#pragma once\n#define ENABLE_BLUETOOTH 1\n#define ENABLE_G2_GLASSES 1\n#define ENABLE_R1_HEALTH 1\n")
        (work / "Arduino.h").write_text("#pragma once\n#include <cstddef>\n#include <cstdint>\n")
        (work / "test.cpp").write_text(unit)
        subprocess.run([args.cxx, *flags, "-I", str(work), str(work / "test.cpp"),
                        "-o", str(work / "test")], check=True, timeout=180)
        subprocess.run([str(work / "test")], check=True, timeout=60)


if __name__ == "__main__":
    main()
