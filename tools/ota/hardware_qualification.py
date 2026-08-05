#!/usr/bin/env python3
"""Plan and preflight HardwareOne OTA hardware qualification runs.

This first implementation is intentionally non-destructive: it validates exact
artifacts/builds and creates restartable evidence/checkpoints, but does not
upload, stage, acknowledge, reboot, or flash a device. Review a generated plan
before adding an executor for any scenario.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from tools.ota import make_manifest
from tools.ota.qualification.artifacts import (
    ArtifactIdentity,
    load_verified_artifacts,
    public_key_fingerprint,
    run_pair_audit,
)
from tools.ota.qualification.evidence import EvidenceRecorder, default_output_root
from tools.ota.qualification.model import Checkpoint
from tools.ota.qualification.scenarios import SCENARIOS, get_scenario
from tools.ota.qualification.serial_observer import available_ports
from tools.ota.qualification.transports import AdbTransport


def emit(value: object, as_json: bool) -> None:
    if as_json:
        print(json.dumps(value, indent=2, sort_keys=True))
        return
    if isinstance(value, str):
        print(value)
        return
    print(json.dumps(value, indent=2, sort_keys=True))


def command_cases(args: argparse.Namespace) -> int:
    values = [scenario.as_dict() for scenario in SCENARIOS]
    if args.json:
        emit(values, True)
    else:
        for scenario in SCENARIOS:
            marker = "DESTRUCTIVE" if scenario.destructive else "pre-write"
            print(f"{scenario.case_id}  {marker:11s}  {scenario.title}")
    return 0


def command_plan(args: argparse.Namespace) -> int:
    try:
        scenario = get_scenario(args.case)
    except KeyError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    emit(scenario.as_dict(), args.json)
    return 0


def artifact_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--board", choices=sorted(make_manifest.BOARD_LAYOUTS), required=True
    )
    parser.add_argument("--image", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--public-key", type=pathlib.Path, required=True)
    parser.add_argument("--main-build", type=pathlib.Path)
    parser.add_argument("--updater-build", type=pathlib.Path)


def preflight(args: argparse.Namespace) -> tuple[dict[str, object], ArtifactIdentity]:
    identity = load_verified_artifacts(
        args.image,
        args.manifest,
        args.public_key,
        expected_board=args.board,
    )
    checks: dict[str, object] = {
        "artifacts": {"status": "PASS", **identity.as_dict()},
        "publicKeyFingerprint": public_key_fingerprint(args.public_key.resolve()),
    }
    pair_requested = args.main_build is not None or args.updater_build is not None
    if pair_requested and (args.main_build is None or args.updater_build is None):
        raise ValueError("--main-build and --updater-build must be supplied together")
    pair_passed = False
    if pair_requested:
        audit = run_pair_audit(args.board, args.main_build, args.updater_build)
        checks["pairAudit"] = {"status": "PASS" if audit.passed else "FAIL", **audit.as_dict()}
        pair_passed = audit.passed
    else:
        checks["pairAudit"] = {
            "status": "SKIP",
            "reason": "main/updater build directories were not supplied",
        }

    serial_passed = False
    if getattr(args, "serial_port", None):
        ports = available_ports()
        matches = [item for item in ports if item["device"] == args.serial_port]
        serial_passed = len(matches) == 1
        checks["serial"] = {
            "status": "PASS" if serial_passed else "FAIL",
            "expected": args.serial_port,
            "matches": matches,
        }
    else:
        checks["serial"] = {
            "status": "SKIP",
            "reason": "no serial port requested; destructive readiness requires one",
        }

    adb_passed = True
    if getattr(args, "adb_serial", None):
        adb = AdbTransport(args.adb_serial, args.adb)
        probe = adb.probe()
        adb_passed = probe["returncode"] == "0" and probe["state"] == "device"
        checks["adb"] = {"status": "PASS" if adb_passed else "FAIL", **probe}
    else:
        checks["adb"] = {"status": "SKIP", "reason": "no ADB device requested"}

    report = {
        "board": args.board,
        "layout": make_manifest.BOARD_LAYOUTS[args.board],
        "checks": checks,
        "artifactReady": True,
        "readyForDestructiveRun": pair_passed and serial_passed and adb_passed,
        "note": "This command performs no device mutation.",
    }
    return report, identity


def command_preflight(args: argparse.Namespace) -> int:
    try:
        report, _identity = preflight(args)
    except (OSError, RuntimeError, SystemExit, ValueError) as exc:
        print(f"preflight failed: {exc}", file=sys.stderr)
        return 2
    emit(report, args.json)
    pair = report["checks"]["pairAudit"]
    requested_failure = pair["status"] == "FAIL"
    for name in ("serial", "adb"):
        if report["checks"][name]["status"] == "FAIL":
            requested_failure = True
    return 1 if requested_failure else 0


def command_init_run(args: argparse.Namespace) -> int:
    if not args.disposable_device:
        print("init-run requires --disposable-device", file=sys.stderr)
        return 2
    try:
        scenario = get_scenario(args.case)
        report, identity = preflight(args)
        recorder = EvidenceRecorder(args.output)
        run_directory = recorder.create()
        checkpoint = Checkpoint(
            run_id=recorder.run_id,
            case_id=scenario.case_id,
            board=identity.board,
            layout=identity.layout,
            image_sha256=identity.image_sha256,
            manifest_sha256=identity.manifest_sha256,
            expected_transition="review plan; no executor has run",
        )
        recorder.write_json("artifacts.json", identity.as_dict())
        recorder.write_json("preflight.json", report)
        recorder.write_json(f"cases/{scenario.case_id}.json", scenario.as_dict())
        recorder.write_json(
            "summary.json",
            {
                "runId": recorder.run_id,
                "caseId": scenario.case_id,
                "outcome": "",
                "dryRunOnly": True,
                "destructiveExecutorPresent": False,
            },
        )
        recorder.write_checkpoint(checkpoint)
        recorder.append_event(
            "run.initialized",
            {
                "caseId": scenario.case_id,
                "dryRunOnly": True,
                "readyForDestructiveRun": report["readyForDestructiveRun"],
            },
        )
    except (KeyError, OSError, RuntimeError, SystemExit, ValueError) as exc:
        print(f"could not initialize qualification run: {exc}", file=sys.stderr)
        return 2
    emit(
        {
            "runDirectory": str(run_directory),
            "runId": recorder.run_id,
            "caseId": scenario.case_id,
            "dryRunOnly": True,
        },
        args.json,
    )
    return 0


def command_resume(args: argparse.Namespace) -> int:
    try:
        checkpoint = EvidenceRecorder.load_checkpoint(args.run)
        scenario = get_scenario(checkpoint.case_id)
    except (KeyError, OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"cannot resume qualification run: {exc}", file=sys.stderr)
        return 2
    emit(
        {
            "checkpoint": checkpoint.as_dict(),
            "scenario": scenario.as_dict(),
            "requiredRediscovery": [
                "reopen the exact serial device",
                "identify the running main or factory component",
                "query authenticated durable status",
                "compare candidate and result sequences with this checkpoint",
            ],
            "executorPresent": False,
        },
        args.json,
    )
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)

    cases = sub.add_parser("cases", help="list the staged qualification contracts")
    cases.add_argument("--json", action="store_true")
    cases.set_defaults(func=command_cases)

    plan = sub.add_parser("plan", help="show one complete staged case contract")
    plan.add_argument("--case", required=True)
    plan.add_argument("--json", action="store_true")
    plan.set_defaults(func=command_plan)

    check = sub.add_parser("preflight", help="validate artifacts and optional lab interfaces")
    artifact_args(check)
    check.add_argument("--serial-port")
    check.add_argument("--adb-serial")
    check.add_argument("--adb", type=pathlib.Path)
    check.add_argument("--json", action="store_true")
    check.set_defaults(func=command_preflight)

    initialize = sub.add_parser("init-run", help="create a dry-run evidence/checkpoint bundle")
    artifact_args(initialize)
    initialize.add_argument("--case", required=True)
    initialize.add_argument("--serial-port")
    initialize.add_argument("--adb-serial")
    initialize.add_argument("--adb", type=pathlib.Path)
    initialize.add_argument("--output", type=pathlib.Path, default=default_output_root())
    initialize.add_argument("--disposable-device", action="store_true")
    initialize.add_argument("--json", action="store_true")
    initialize.set_defaults(func=command_init_run)

    resume = sub.add_parser("resume", help="validate and display a saved checkpoint")
    resume.add_argument("--run", type=pathlib.Path, required=True)
    resume.add_argument("--json", action="store_true")
    resume.set_defaults(func=command_resume)
    return root


def main() -> int:
    args = parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
