#!/usr/bin/env python3
"""Prove production mesh identity/configuration survive a USB reopen reboot.

Run only after both boards are provisioned, securely paired and working:

  python experiments/p4_mesh/test_persistence.py

Uses private/credentials.json and the same default ports/MACs as test_mesh.py.
No flashing, onboarding, settings writes, pairing or repair is performed. The
runner closes/reopens both serial ports; these particular boards reset on USB
reopen. It requires fresh ROM/reset/startup logs AND an increased NVS boot count
to prove this happened. Missing reset evidence is a failure, not an assumption.

The two phases use the HardwareOne whoami FIFO barrier, wait nine seconds after
opening ports, verify saved peer registries before any manual session kick, and
exchange encrypted 401-byte text both ways. Private timestamped artifacts contain
public identities, configuration, session evidence and redacted serial logs only.
Importing this module never opens hardware.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time
from types import SimpleNamespace

from console import ANSI, MeshConsole
from test_mesh import (DEFAULT_BOARDS, HERE, CheckFailed, MeshRunner, command_token,
                       normalize_mac, require, utc_now)


def public_identity(document: dict, expected_mac: str) -> dict:
    require(document.get("schema") == 1 and document.get("valid") is True, "Public identity is invalid")
    require(normalize_mac(document.get("mac", "")) == normalize_mac(expected_mac), "Public identity MAC differs from board")
    require(isinstance(document.get("pub"), str) and re.fullmatch(r"[a-fA-F0-9]{64}", document["pub"]) is not None,
            "Public Ed25519 identity has an invalid encoding")
    require(all(type(document.get(key)) is int and document[key] >= 0 for key in ("createdAtSec", "regenCount")),
            "Public identity creation metadata is invalid")
    return {"mac": normalize_mac(document["mac"]), "pub": document["pub"].lower(),
            "createdAtSec": document["createdAtSec"], "regenCount": document["regenCount"]}


def autostart_value(output: str) -> bool:
    match = re.search(r"\bespnowAutoStart\s*=\s*(true|false)\b", ANSI.sub("", output))
    require(match is not None, "Could not read ESP-NOW autostart setting")
    return match.group(1) == "true"


def prove_reboot(before: dict, after: dict, raw_boot: str, chip: str) -> dict:
    require(before.get("schema") == 1 and after.get("schema") == 1, "Invalid boot-count schema")
    require(before.get("boot_count_store") == after.get("boot_count_store") == "nvs", "Boot counter is not NVS-backed")
    require(type(before.get("boot_count")) is int and type(after.get("boot_count")) is int
            and after["boot_count"] > before["boot_count"], "USB reopen did not advance the boot count")
    require(after.get("reset_reason_code") == 11 and str(after.get("reset_reason", "")).upper() == "USB",
            "Reopened board did not report a USB reset")
    require(after.get("crash_count") == before.get("crash_count"), "Crash count changed across the persistence reboot")
    output = ANSI.sub("", raw_boot)
    rom = re.search(r"(?m)^ESP-ROM:(" + re.escape(chip) + r"[^\r\n]*)", output)
    reset = re.search(r"(?m)^rst:0x[0-9a-fA-F]+ \(([^\r\n)]*USB[^\r\n)]*)\),boot:0x[0-9a-fA-F]+", output)
    boot_event = re.search(r"\[EVENT\]\[BOOT\] boot #(\d+) \| reset=usb\(11\)", output)
    complete = output.find("[Boot] Setup complete")
    require(rom is not None and reset is not None and boot_event is not None and complete >= 0,
            "Fresh ROM, USB-reset, application boot event and setup-complete logs are all required")
    require(rom.start() < reset.start() < boot_event.start() < complete, "Boot evidence is not in startup order")
    require(int(boot_event.group(1)) == after["boot_count"], "Startup log and queried boot counter disagree")
    return {"rom": rom.group(1), "reset": reset.group(1), "beforeBootCount": before["boot_count"],
            "afterBootCount": after["boot_count"], "bootCountDelta": after["boot_count"] - before["boot_count"],
            "applicationStartupComplete": True, "crashCount": after["crash_count"]}


def compare_saved_state(before: dict, after: dict) -> dict:
    for key in ("identities", "configuration", "registries"):
        require(before[key] == after[key], f"Saved {key} changed across USB reboot")
    return {"publicIdentitiesUnchanged": True, "configurationUnchanged": True,
            "reciprocalRegistriesUnchangedBeforeManualSessionKick": True}


def snapshot(runner: MeshRunner) -> dict:
    state = {"identities": {}, "configuration": {}, "registries": {}, "boots": {}}
    for key, spec in runner.config.items():
        inspection = runner.step(f"inspect_{key}", lambda key=key: runner.inspect(key))
        identity = runner.json_command(key, "espnowidentity json", "valid")
        state["identities"][key] = public_identity(identity, spec["mac"])
        boot = runner.json_command(key, "bootcount json", "boot_count")
        state["boots"][key] = boot
        autostart = autostart_value(runner.command(key, "espnowautostart"))
        require(autostart, f"{key}: ESP-NOW autostart must already be enabled")
        name_output = runner.command(key, "espnowsetname")
        name = re.search(r"Device name: ([A-Za-z0-9_-]+)", name_output)
        require(name is not None and name.group(1) == spec["name"], f"{key}: stored device name differs from expected")
        meshes = runner.json_command(key, "espnowmeshes listjson", "meshes", schema=False)
        mesh = next(row for row in meshes["meshes"] if row["label"] == runner.credentials["mesh_label"])
        state["configuration"][key] = {"name": name.group(1), "channel": inspection["channel"],
                                        "wifiJoined": False, "autoStart": autostart, "mode": "mesh", "mesh": mesh}
    # This reads the application's actual send resolver registry. No pairing,
    # KEY_EX, SESSION_OPEN or repair command has been issued in this phase.
    state["registries"] = runner.registries()
    require(all(state["registries"].values()), "A reciprocal stored peer registry is missing after startup")
    for key, other in (("p4", "s3"), ("s3", "p4")):
        require(runner.known_peer(key, other), f"{key}: peer identity missing after startup")
    return state


def delivery_phase(runner: MeshRunner, phase: str) -> dict:
    sessions = runner.step("active_session", runner.sessions)
    messages = [runner.step(f"encrypted_401_{sender}_to_{receiver}",
                            lambda sender=sender, receiver=receiver: runner.text(sender, receiver, 401, phase=phase))
                for sender, receiver in (("p4", "s3"), ("s3", "p4"))]
    return {"sessions": sessions, "messages": messages}


@contextlib.contextmanager
def phase_console(config: dict, credentials: dict, run_dir: Path, phase: str, run_id: str):
    phase_dir = run_dir / phase
    phase_dir.mkdir(mode=0o700)
    private_values = [credentials[key] for key in ("username", "password", "mesh_passphrase")]
    with contextlib.ExitStack() as stack:
        boards = {key: stack.enter_context(MeshConsole(spec["port"], phase_dir / f"{key}.log",
                                                      secrets=private_values, completion="hardwareone"))
                  for key, spec in config.items()}
        options = SimpleNamespace(configure=False, pair="preserve", initiator="p4", rekey=False, reopen=False)
        runner = MeshRunner(boards, config, credentials, phase_dir, run_id, options)
        runner.results["scope"] = f"reboot persistence: {phase}"
        try:
            time.sleep(9)
            for key in boards:
                runner.step(f"login_{key}", lambda key=key: runner.login(key))
            yield runner
            runner.results.update(status="passed", finished=utc_now())
            runner.save()
        except BaseException as exc:
            try:
                runner.capture_failure_stats()
            except BaseException:
                pass
            runner.results.update(status="failed", error=runner.safe(str(exc)), finished=utc_now())
            try:
                runner.save()
            except BaseException:
                pass
            raise


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--credentials", type=Path, default=HERE / "private" / "credentials.json")
    parser.add_argument("--run-root", type=Path, default=HERE / "private" / "persistence-runs")
    for key in DEFAULT_BOARDS:
        parser.add_argument(f"--{key}-port", default=DEFAULT_BOARDS[key]["port"])
        parser.add_argument(f"--{key}-mac", default=DEFAULT_BOARDS[key]["mac"])
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    private_values, report, report_path = [], {}, None
    def safe(text):
        for value in sorted(private_values, key=len, reverse=True):
            text = text.replace(value, "[REDACTED]")
        return text
    def persist():
        def sanitize(value):
            if isinstance(value, str): return safe(value)
            if isinstance(value, list): return [sanitize(item) for item in value]
            if isinstance(value, dict): return {safe(str(key)): sanitize(item) for key, item in value.items()}
            return value
        if report_path is not None:
            descriptor = os.open(report_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                json.dump(sanitize(report), output, indent=2)
                output.write("\n")
    try:
        credentials = json.loads(args.credentials.read_text())
        require(isinstance(credentials, dict), "Credentials must be a JSON object")
        for key in ("username", "password", "mesh_passphrase", "mesh_label"):
            require(isinstance(credentials.get(key), str) and credentials[key], f"Credentials missing {key}")
        private_values = [credentials[key] for key in ("username", "password", "mesh_passphrase")]
        for value in credentials.values():
            if isinstance(value, str): command_token(value)
        config = {key: {**spec, "port": getattr(args, key + "_port"), "mac": normalize_mac(getattr(args, key + "_mac"))}
                  for key, spec in DEFAULT_BOARDS.items()}
        require(config["p4"]["port"] != config["s3"]["port"] and config["p4"]["mac"] != config["s3"]["mac"],
                "Boards must have distinct ports and MAC addresses")
        run_id = secrets.token_hex(4)
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        run_dir = args.run_root / f"{stamp}-{run_id}"
        args.run_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        run_dir.mkdir(mode=0o700)
        report_path = run_dir / "persistence.json"
        report.update(schema=1, status="running", started=utc_now(), runId=run_id, boards=config,
                      rebootMethod="close and reopen USB serial; no reset command", startupWaitSec=9)
        persist()
        with phase_console(config, credentials, run_dir, "baseline", run_id) as runner:
            baseline = runner.step("stored_state_before", lambda: snapshot(runner))
            report["baseline"] = baseline
            report["baselineDelivery"] = delivery_phase(runner, "persistence_before")
            runner.step("statistics", lambda: runner.stats_snapshot("end"))
            persist()
        # The first context closes BOTH ports before either is opened again.
        # A short gap permits USB close handling; the nine-second startup wait
        # belongs to the newly opened phase, not to an assumed reset timestamp.
        time.sleep(0.5)
        with phase_console(config, credentials, run_dir, "reopened", run_id) as runner:
            after = runner.step("stored_state_after_before_session_kick", lambda: snapshot(runner))
            report["after"] = after
            report["rebootProof"] = runner.step("prove_both_reboots", lambda: {
                key: prove_reboot(baseline["boots"][key], after["boots"][key], runner.boards[key].read_since(0),
                                  "esp32p4" if key == "p4" else "esp32s3") for key in config})
            report["persistenceProof"] = runner.step("compare_saved_state", lambda: compare_saved_state(baseline, after))
            persist()
            report["afterDelivery"] = delivery_phase(runner, "persistence_after")
            require(report["afterDelivery"]["sessions"]["p4"]["sessionId"] !=
                    report["baselineDelivery"]["sessions"]["p4"]["sessionId"],
                    "Session ID did not change; a fresh session was not demonstrated")
            runner.step("statistics", lambda: runner.stats_snapshot("end"))
        report.update(status="passed", finished=utc_now(), freshSessionVerified=True)
        persist()
        print(safe(f"PASS reboot persistence, fresh session and bidirectional encrypted text; results: {report_path}"), flush=True)
        return 0
    except BaseException as exc:
        report.update(status="interrupted" if isinstance(exc, KeyboardInterrupt) else "failed", finished=utc_now(), error=safe(str(exc)))
        try:
            persist()
        except BaseException:
            pass
        print(safe(f"FAIL {type(exc).__name__}: {exc}"), file=sys.stderr, flush=True)
        return 130 if isinstance(exc, KeyboardInterrupt) else 1


if __name__ == "__main__":
    raise SystemExit(main())
