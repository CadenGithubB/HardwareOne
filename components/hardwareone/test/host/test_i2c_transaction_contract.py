#!/usr/bin/env python3
"""Structural guards for the I2C identity/execution-policy boundary."""

from __future__ import annotations

import re
import sys
from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[2]


def fail(message: str) -> None:
    print(f"I2C transaction contract failure: {message}", file=sys.stderr)
    raise SystemExit(1)


def block_after(source: str, marker: str) -> str:
    """Return the brace-balanced function/struct body following marker."""
    marker_at = source.find(marker)
    if marker_at < 0:
        fail(f"missing production marker: {marker!r}")
    brace_at = source.find("{", marker_at + len(marker))
    if brace_at < 0:
        fail(f"missing body after marker: {marker!r}")

    depth = 0
    for pos in range(brace_at, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace_at : pos + 1]
    fail(f"unterminated body after marker: {marker!r}")
    raise AssertionError("unreachable")


manager_h = (COMPONENT / "System_I2C_Manager.h").read_text(encoding="utf-8")
manager_cpp = (COMPONENT / "System_I2C_Manager.cpp").read_text(encoding="utf-8")
helpers_h = (COMPONENT / "System_I2C.h").read_text(encoding="utf-8")
system_cpp = (COMPONENT / "System_I2C.cpp").read_text(encoding="utf-8")
hardware_cpp = (COMPONENT / "HardwareOne.cpp").read_text(encoding="utf-8")

options = block_after(manager_h, "struct I2CTransactionOptions")
for declaration in ("uint32_t clockHz;", "uint32_t lockWaitMs;"):
    if declaration not in options:
        fail(f"I2CTransactionOptions lost {declaration}")

device_identity = manager_h[
    manager_h.index("class I2CDevice {") : manager_h.index("// Health tracking")
]
for stale_field in ("baseTimeoutMs", "adaptiveTimeoutMs"):
    if stale_field in device_identity:
        fail(f"registry identity again owns transaction policy: {stale_field}")

execute = block_after(manager_h, "auto I2CDeviceManager::executeTransaction")
if "options.lockWaitMs" not in execute:
    fail("executeTransaction does not take its mutex wait from this call")
if "options.clockHz" not in execute:
    fail("executeTransaction does not take its clock from this call")
for stale_read in (
    "device->clockHz",
    "device->baseTimeoutMs",
    "device->adaptiveTimeoutMs",
):
    if stale_read in execute:
        fail(f"executeTransaction reads stale registry policy: {stale_read}")

device_forwarder = block_after(manager_h, "auto I2CDevice::transaction")
if not re.search(
    r"executeTransaction\s*\(\s*this\s*,[\s\S]*?options\s*,\s*mode\s*\)",
    device_forwarder,
):
    fail("I2CDevice::transaction does not forward per-call options")

registration = block_after(
    manager_cpp, "I2CDevice* I2CDeviceManager::registerDevice"
)
if "devices[i].address == addr && devices[i].bus == busIdx" not in registration:
    fail("duplicate registration is no longer keyed by the physical (bus, address)")
if "return &devices[i];" not in registration:
    fail("duplicate registration no longer returns the existing identity record")
for stale_policy in ("clockHz", "timeoutMs", "adaptiveTimeoutMs"):
    if stale_policy in registration:
        fail(f"duplicate registration mutates transaction policy: {stale_policy}")
if "dev->init(addr, name, busIdx)" not in registration:
    fail("new registry entries are not initialized as identity-only records")

# Restrict this check to the transaction helper family; direct probe helpers
# below it intentionally acquire the bus themselves and have a separate API.
helper_family = helpers_h[
    helpers_h.index("// Legacy single-bus helper") : helpers_h.index(
        "// Ping/probe helpers"
    )
]
option_builds = helper_family.count(
    "const I2CTransactionOptions options{clockHz, lockWaitMs};"
)
if option_builds != 6:
    fail(f"expected six standard/NACK helper option builds, found {option_builds}")

transaction_calls = re.findall(r"dev->transaction\s*\([^;]+\);", helper_family)
if len(transaction_calls) != 6:
    fail(f"expected six device transaction dispatches, found {len(transaction_calls)}")
for call in transaction_calls:
    if "options" not in call:
        fail(f"helper dropped its per-call options: {call}")

registration_calls = re.findall(r"registerDevice\s*\([^;]+\);", helper_family)
if len(registration_calls) != 6:
    fail(f"expected six lazy-registration calls, found {len(registration_calls)}")
for call in registration_calls:
    if "clockHz" in call or "lockWaitMs" in call:
        fail(f"helper persisted execution policy during lazy registration: {call}")

if "configuredBusForSensor(sensor)" not in system_cpp:
    fail("boot pre-registration is not routed through the configured device bus")
if not re.search(
    r"registerDevice\s*\(\s*sensor\.address\s*,\s*sensor\.name\s*,\s*bus\s*\)",
    system_cpp,
):
    fail("boot pre-registration does not record identity on the configured bus")

discovery = block_after(system_cpp, "void discoverI2CDevices()")
if "if (!isSensorCompiled(i2cSensors[i])) continue;" not in discovery:
    fail("smart discovery no longer filters its scan list with isSensorCompiled()")

# An infrastructure-only I2C profile (such as FeatherS3[D] + MAX17048) must
# not allocate the optional-sensor queue task. Guard both the normal boot site
# and the late auto-start site with the same derived compile-time predicate.
queue_create_pattern = re.compile(
    r"#if\s+ENABLE_I2C_SENSOR_QUEUE\b"
    r"(?:(?!#endif).)*?"
    r"xTaskCreateLogged\s*\(\s*sensorQueueProcessorTask",
    re.DOTALL,
)
for label, source in (
    ("normal boot", hardware_cpp),
    ("late sensor auto-start", system_cpp),
):
    create_count = len(
        re.findall(
            r"xTaskCreateLogged\s*\(\s*sensorQueueProcessorTask", source
        )
    )
    if create_count != 1:
        fail(f"expected one {label} sensor-queue task creation site, found {create_count}")
    if not queue_create_pattern.search(source):
        fail(f"{label} sensor-queue task creation is not compile-time gated")

print("I2C transaction source-contract guards passed")
