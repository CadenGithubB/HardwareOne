#!/usr/bin/env python3
"""Opt-in USB qualification of the real authenticated HardwareOne JPEG loader.

Importing this module never opens a device. Explicit execution logs into both
boards, stages only synthetic fixtures in a new /jpeg_qualification/run-* folder,
checks exact readback, and exercises jpegdiag. It never flashes, changes peers,
starts radios/cameras, erases files, or changes G2 tone settings. Runtime loglink
is restored in finally. Fixture files remain for inspection; existing files are
never overwritten. Root must own both ports exclusively before execution.

Credentials: ignored mode-0600 JSON with username/password (same account on both).
Port and *radio* MAC arguments are mandatory. Results/logs stay under private/.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import statistics
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE.parent / "p4_ble_roles"))
sys.path.insert(0, str(HERE.parent / "p4_mesh"))
from board_control import RolesConsole
from console import ANSI, extract_json_objects
from test_mesh import CheckFailed, FATAL, WIZARD, command_token, decode_file_chunk

FIXTURE_DIR = REPO / "components/hardwareone/test/host/jpeg_fixtures"
ERROR = re.compile(r"(?:^|\n|\] )\s*(?:Error:|ERROR:|Unknown command|Permission denied|Unauthorized)", re.I)
TONE = re.compile(r"G2 stream tone-map: ([0-3]) \((Linear|Balanced|Shadows|Legacy)\)")
HASH = re.compile(r"[0-9a-f]{8}")


def require(condition, message):
    if not condition:
        raise CheckFailed(message)


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def mac(value):
    require(bool(re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value)), "Invalid radio MAC")
    return value.lower()


def string_values(value):
    if isinstance(value, str):
        return [value] if value else []
    if isinstance(value, dict):
        return [item for child in value.values() for item in string_values(child)]
    if isinstance(value, list):
        return [item for child in value for item in string_values(child)]
    return []


def one_json(output, predicate, label):
    matches = [item for item in extract_json_objects(output) if predicate(item)]
    require(len(matches) == 1, f"Expected one {label} response")
    return matches[0]


def oversized_dimensions(data):
    """Alter only a baseline SOF width; preserve bytes so parser rejects size."""
    image = bytearray(data)
    pos = 2
    while pos < len(image):
        require(image[pos] == 0xff, "Malformed source fixture marker")
        while image[pos] == 0xff:
            pos += 1
        marker = image[pos]
        pos += 1
        length = int.from_bytes(image[pos:pos + 2], "big")
        require(length >= 2 and pos + length <= len(image), "Malformed source fixture segment")
        if marker == 0xc0:
            image[pos + 5:pos + 7] = (1601).to_bytes(2, "big")
            return bytes(image)
        require(marker != 0xda, "Missing baseline SOF in source fixture")
        pos += length
    raise CheckFailed("Missing SOF")


def load_cases(args, reference):
    paths = sorted(FIXTURE_DIR.glob("*.jpg"))
    selected = set(args.fixtures.split(",")) if args.fixtures else {p.stem for p in paths}
    selected.add("rgb420_320x240")  # bounded repeated-workload reference
    available = {p.stem for p in paths}
    require(selected <= available, "Unknown selected fixture")
    cases = {}
    for path in paths:
        if path.stem not in selected:
            continue
        data = path.read_bytes()
        require(0 < len(data) <= 128 * 1024, "Committed fixture outside loader input bounds")
        for board in ("p4", "s3"):
            for record in reference["boards"][board]["fixtures"].get(path.stem, {}).values():
                require(record["fixture_sha256"] == sha(data), "Fixture differs from qualified reference")
        cases[path.stem] = {"data": data, "source": str(path.relative_to(REPO)), "kind": "committed"}
    base = (FIXTURE_DIR / "rgb420_24x24.jpg").read_bytes()
    require(base.endswith(b"\xff\xd9"), "Reference fixture has unexpected trailer")
    malformed = {
        "invalid_soi": (b"not-a-jpeg-fixture" * 2, "not a JPEG (missing SOI marker)"),
        "too_small": (b"\xff\xd8" + bytes(13), "file too small for JPEG"),
        "truncated_eoi": (base[:-2], "truncated JPEG (missing EOI)"),
        "dimensions_over_limit": (oversized_dimensions(base), "JPEG dimensions exceed limit"),
    }
    if args.include_file_limit:
        malformed["file_over_limit"] = (base + bytes(128 * 1024 + 1 - len(base)), "JPEG too large (>128 KB)")
    for name, (data, error) in malformed.items():
        cases[name] = {"data": data, "source": "deterministic host-derived negative fixture",
                       "kind": "invalid", "expected_error": error}
    return cases


class JpegRunner:
    def __init__(self, args, credentials, reference, cases, run_dir, run_id):
        self.args, self.credentials, self.reference, self.cases = args, credentials, reference, cases
        self.targets = ("p4", "s3") if args.board == "both" else (args.board,)
        self.run_dir, self.run_id = run_dir, run_id
        self.device_dir = "/jpeg_qualification/run-" + run_id
        self.boards, self.marks, self.tails, self.loglink_original = {}, {}, {}, {}
        self.deadline = time.monotonic() + args.total_timeout
        values = string_values(credentials)
        self.secret_values = sorted(set(values + [base64.b64encode(v.encode()).decode() for v in values]), key=len, reverse=True)
        self.result = {"schema": 1, "scope": "full-app USB JPEG loader, VFS and BMP conversion; no camera or G2 delivery",
                       "started": now(), "run_id": run_id, "status": "running", "targets": list(self.targets), "device_directory": self.device_dir,
                       "expectations_sha256": sha(args.expectations.read_bytes()), "checks": [], "cleanup": [],
                       "fixture_manifest": {name: {"bytes": len(case["data"]), "sha256": sha(case["data"]),
                                                  "source": case["source"], "kind": case["kind"]}
                                            for name, case in cases.items()},
                       "limits": {"repeat_count": args.repeats, "total_timeout_seconds": args.total_timeout,
                                  "camera_tested": False, "g2_transport_tested": False,
                                  "memory_claim": "bounded snapshots and integrity, not an endurance/leak proof"}}
        self.save()

    def safe(self, value):
        if isinstance(value, str):
            for secret in self.secret_values:
                value = value.replace(secret, "[REDACTED]")
            return value
        if isinstance(value, list):
            return [self.safe(child) for child in value]
        if isinstance(value, dict):
            return {key: self.safe(child) for key, child in value.items()}
        return value

    def save(self):
        descriptor, temporary = tempfile.mkstemp(prefix=".results-", dir=self.run_dir)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                json.dump(self.safe(self.result), output, indent=2)
                output.write("\n")
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.run_dir / "results.json")
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def health(self):
        for board, console in self.boards.items():
            end = console.marker()
            output = self.tails.get(board, "") + console.read_since(self.marks.get(board, 0))
            self.marks[board] = end
            self.tails[board] = output[-256:]
            require(FATAL.search(ANSI.sub("", output)) is None, f"{board}: panic/assert/watchdog detected")

    def command(self, board, command, *, timeout=20, allow_error=False, cleanup=False):
        require(len(command.encode()) <= 2047, "Command exceeds firmware input limit")
        if not cleanup:
            remaining = self.deadline - time.monotonic()
            require(remaining > 0, "Qualification exceeded total deadline")
            timeout = min(timeout, max(0.1, remaining))
        response = self.boards[board].command(command, timeout=timeout)
        if not cleanup:
            self.health()
        if not allow_error:
            require(ERROR.search(ANSI.sub("", response)) is None, f"{board}: {command.split()[0]} rejected")
        return response

    def step(self, name, operation):
        started = time.monotonic()
        print(self.safe("RUN " + name), flush=True)
        try:
            evidence = operation()
            self.health()
        except BaseException as error:
            failure = {"name": name, "error": self.safe(str(error)), "type": type(error).__name__}
            self.result.setdefault("first_failure", failure)
            self.result["checks"].append({**failure, "status": "failed", "seconds": round(time.monotonic() - started, 3)})
            self.save()
            raise
        self.result["checks"].append({"name": name, "status": "passed", "seconds": round(time.monotonic() - started, 3), "evidence": evidence})
        self.save()
        print(self.safe("PASS " + name), flush=True)
        return evidence

    def login_inventory(self, board):
        console = self.boards[board]
        require(WIZARD.search(console.read_since(0)) is None, f"{board}: setup wizard needs completion")
        self.command(board, "login " + command_token(self.credentials["username"]) + " " + command_token(self.credentials["password"]), timeout=30)
        identity = self.command(board, "whoami")
        require(f"You are {self.credentials['username']} (admin)" in identity, f"{board}: named admin login not established")
        link = self.command(board, "loglink")
        states = re.findall(r"loglink: (ON|OFF)\b", ANSI.sub("", link))
        require(len(states) == 1, f"{board}: loglink state ambiguous")
        self.loglink_original[board] = states[0] == "ON"
        if not self.loglink_original[board]:
            require("loglink ON" in self.command(board, "loglink on"), "Log serialization not acknowledged")
        radio = one_json(self.command(board, "espnowstatus json"), lambda item: "mac" in item and "initialized" in item, "radio status")
        require(mac(radio["mac"]) == getattr(self.args, board + "_mac"), f"{board}: wrong radio identity on port")
        system = one_json(self.command(board, "status json"), lambda item: "schema" in item, "system status")
        tone = TONE.findall(ANSI.sub("", self.command(board, "g2streamtonemap")))
        require(len(tone) == 1, f"{board}: tone state ambiguous")
        storage = one_json(self.command(board, 'files stats json "/"'), lambda item: "success" in item, "storage status")
        require(storage.get("success") is True, f"{board}: filesystem unavailable")
        required = sum(len(case["data"]) for case in self.cases.values()) + 65536
        require(type(storage.get("free")) is int and storage["free"] >= required, f"{board}: insufficient free space for synthetic fixtures")
        return {"named_admin": True, "radio_identity_verified": True, "radio": radio, "system": system,
                "storage": storage, "live_stream_tone": {"value": int(tone[0][0]), "name": tone[0][1], "changed": False},
                "file_loader_tone": "Linear default argument; independent of live-stream setting"}

    def make_directory(self, board):
        self.command(board, 'mkdir "/jpeg_qualification"')
        reply = self.command(board, f'mkdir "{self.device_dir}"')
        require(f"Created folder: {self.device_dir}" in reply and "already exists" not in reply,
                f"{board}: run directory was not newly created; refusing overwrite")
        return {"path": self.device_dir, "new_directory": True, "existing_files_touched": False}

    def stage(self, board, name):
        data = self.cases[name]["data"]
        path = f"{self.device_dir}/{name}.jpg"
        for offset in range(0, len(data), 384):
            chunk = data[offset:offset + 384]
            final = offset + len(chunk) == len(data)
            command = f'filewrite "{path}" {offset} {base64.b64encode(chunk).decode()}' + (" final" if final else "")
            reply = one_json(self.command(board, command, timeout=30), lambda item: "success" in item, "file write")
            require(reply.get("success") is True and reply.get("size") == offset + len(chunk) and reply.get("final") is final,
                    f"{board}/{name}: sequential file staging failed")
        readback = bytearray()
        while len(readback) < len(data):
            offset = len(readback)
            reply = one_json(self.command(board, f'fileread "{path}" {offset} 512 b64'), lambda item: "success" in item, "file read")
            readback.extend(decode_file_chunk(reply, offset, len(data), path))
        require(bytes(readback) == data and sha(readback) == sha(data), f"{board}/{name}: readback mismatch")
        return {"path": path, "bytes": len(data), "sha256": sha(data), "exact_readback": True}

    def diagnostic(self, board, name, *, expected_error=None):
        path = f"{self.device_dir}/{name}.jpg"
        reply = one_json(self.command(board, f'jpegdiag "{path}"', timeout=30),
                         lambda item: item.get("diagnostic") == "full-app-jpeg", "JPEG diagnostic")
        # Save observations before assertions so a failed comparison retains evidence.
        self.result.setdefault("observations", []).append({"board": board, "fixture": name, "result": reply})
        require(reply.get("schema") == 1, "Unsupported jpegdiag schema")
        require(reply.get("integrity_before") is True and reply.get("integrity_after") is True, f"{board}/{name}: heap integrity failed")
        for field in ("internal_before", "internal_after", "largest_before", "largest_after", "decode_us", "bmp_us", "total_us"):
            require(type(reply.get(field)) is int and reply[field] >= 0, f"Invalid diagnostic {field}")
        case = self.cases.get(name, {})
        records = self.reference["boards"][board]["fixtures"].get(name, {})
        if expected_error is None:
            expected_error = case.get("expected_error")
        if not records and case.get("kind") == "committed":
            require(name == "progressive_17x19" or (board == "s3" and name == "gray_17x19"), "No qualified reference for fixture")
            expected_error = "software JPEG decode failed"
        if expected_error is not None:
            require(reply.get("ok") is False and reply.get("converted") is False and reply.get("error") == expected_error,
                    f"{board}/{name}: expected bounded decoder rejection")
            require(reply.get("backend") == "none" and reply.get("bmp_valid") is False and reply.get("bmp_bytes") == 0,
                    f"{board}/{name}: failed load exposed an output buffer")
            return {"expected_failure": True, "diagnostic": reply}
        backend = "hardware" if board == "p4" and "hardware" in records else "software"
        expected = records[backend]
        require(reply.get("ok") is True and reply.get("converted") is True and reply.get("error") == "", f"{board}/{name}: conversion failed")
        require(reply.get("backend") == backend and reply.get("rgb_hash") == expected["rgb_hash"], f"{board}/{name}: RGB/backend mismatch")
        dimensions = re.search(r"_(\d+)x(\d+)$", name)
        require(dimensions is not None, "Missing source geometry")
        require((reply.get("width"), reply.get("height")) == tuple(map(int, dimensions.groups())), f"{board}/{name}: source geometry mismatch")
        require(reply.get("bmp_valid") is True and reply.get("bmp_bytes") == 20854 and reply.get("bmp_width") == 288 and reply.get("bmp_height") == -144,
                f"{board}/{name}: BMP structure mismatch")
        require(isinstance(reply.get("bmp_hash"), str) and HASH.fullmatch(reply["bmp_hash"]) is not None, "Invalid BMP digest")
        if "bmp_hash" in expected:
            require(reply["bmp_hash"] == expected["bmp_hash"], f"{board}/{name}: independent BMP-reference mismatch")
        return {"expected_failure": False, "independent_bmp_reference": "bmp_hash" in expected, "diagnostic": reply}

    def repeated(self, board):
        name = "rgb420_320x240"
        warmup = self.diagnostic(board, name)["diagnostic"]
        records = []
        for _ in range(self.args.repeats):
            time.sleep(self.args.pace)
            record = self.diagnostic(board, name)["diagnostic"]
            require(record["bmp_hash"] == warmup["bmp_hash"], f"{board}: repeated BMP output changed")
            records.append(record)
        return {"fixture": name, "warmup_excluded_from_timing": True, "iterations": len(records),
                "median_decode_us": statistics.median(item["decode_us"] for item in records),
                "median_bmp_us": statistics.median(item["bmp_us"] for item in records),
                "median_total_us": statistics.median(item["total_us"] for item in records),
                "internal_start": records[0]["internal_before"], "internal_end": records[-1]["internal_after"],
                "internal_after_min": min(item["internal_after"] for item in records),
                "internal_after_max": max(item["internal_after"] for item in records),
                "largest_after_min": min(item["largest_after"] for item in records),
                "largest_after_max": max(item["largest_after"] for item in records),
                "all_integrity_checks_passed": True, "all_rgb_and_bmp_hashes_stable": True,
                "interpretation": "short repeated workload; concurrent application activity affects heap snapshots; not a leak/endurance verdict"}

    def restore(self):
        for board, originally_on in self.loglink_original.items():
            try:
                if not originally_on:
                    reply = self.command(board, "loglink off", cleanup=True)
                    require("loglink OFF" in reply, "Loglink restore not acknowledged")
                self.result["cleanup"].append({"board": board, "loglink_restored": True, "originally_on": originally_on})
            except BaseException as error:
                failure = {"board": board, "loglink_restored": False, "error": self.safe(str(error))}
                self.result["cleanup"].append(failure)
                self.result.setdefault("first_failure", {"name": "restore_loglink", **failure})
                self.result["status"] = "failed"
            self.save()

    def run(self):
        with contextlib.ExitStack() as stack:
            try:
                for board in self.targets:
                    self.boards[board] = stack.enter_context(RolesConsole(getattr(self.args, board + "_port"), self.run_dir / f"{board}.log",
                                                                         secrets=self.secret_values, completion="hardwareone"))
                time.sleep(self.args.startup_delay)
                for board in self.boards:
                    self.step(board + "_inventory", lambda board=board: self.login_inventory(board))
                for board in self.boards:
                    self.step(board + "_directory", lambda board=board: self.make_directory(board))
                    for name in self.cases:
                        self.step(board + "_stage_" + name, lambda board=board, name=name: self.stage(board, name))
                        self.step(board + "_decode_" + name, lambda board=board, name=name: self.diagnostic(board, name))
                    self.step(board + "_missing_file", lambda board=board: self.diagnostic(board, "absent_" + self.run_id, expected_error="file not found"))
                    self.step(board + "_repeated_qvga", lambda board=board: self.repeated(board))
                self.result["status"] = "passed"
            except BaseException as error:
                self.result.setdefault("first_failure", {"name": "coordinator", "error": self.safe(str(error)), "type": type(error).__name__})
                self.result["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
            finally:
                self.restore()
                self.result["finished"] = now()
                self.save()
        return self.result["status"] == "passed"


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--credentials", type=Path, required=True)
    parser.add_argument("--board", choices=("p4", "s3", "both"), default="both", help="Open only the selected board(s)")
    for board in ("p4", "s3"):
        parser.add_argument("--" + board + "-port")
        parser.add_argument("--" + board + "-mac", type=mac)
    parser.add_argument("--expectations", type=Path, default=HERE / "jpeg-expectations.json")
    parser.add_argument("--run-root", type=Path, default=HERE / "private/jpeg-integration-runs")
    parser.add_argument("--fixtures", help="Comma-separated committed fixture stems; default all. QVGA420 and derived invalid cases always included.")
    parser.add_argument("--include-file-limit", action="store_true", help="Also upload a 128 KiB+1 fixture to test the compressed-file cap")
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--pace", type=float, default=0.25)
    parser.add_argument("--startup-delay", type=float, default=8.0)
    parser.add_argument("--total-timeout", type=float, default=3600)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    require(1 <= args.repeats <= 100 and 0.05 <= args.pace <= 2, "Invalid repetition/pacing limit")
    require(0 <= args.startup_delay <= 30 and 60 <= args.total_timeout <= 7200, "Invalid startup/total deadline")
    targets = ("p4", "s3") if args.board == "both" else (args.board,)
    for board in targets:
        require(getattr(args, board + "_port") and getattr(args, board + "_mac"), f"{board} port and radio MAC are required")
    if args.board == "both":
        require(args.p4_port != args.s3_port and args.p4_mac != args.s3_mac, "Boards must have distinct ports and radio MACs")
    require(args.credentials.is_file() and not args.credentials.is_symlink(), "Credentials must be a regular private file")
    require(args.credentials.stat().st_mode & 0o077 == 0, "Credentials file must not be group/world accessible")
    require("private" in args.run_root.resolve().parts, "Run root must be in ignored private storage")
    credentials = json.loads(args.credentials.read_text())
    for field in ("username", "password"):
        command_token(credentials.get(field))
    reference = json.loads(args.expectations.read_text())
    require(reference.get("schema") == 1 and set(reference.get("boards", {})) == {"p4", "s3"}, "Invalid reference schema")
    cases = load_cases(args, reference)
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + secrets.token_hex(8)
    args.run_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    run_dir = args.run_root / run_id
    run_dir.mkdir(mode=0o700)
    runner = JpegRunner(args, credentials, reference, cases, run_dir, run_id)
    success = runner.run()
    print(runner.safe(f"{'PASS' if success else 'FAIL'} full-app JPEG qualification; results: {run_dir / 'results.json'}"), flush=True)
    return 0 if success else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckFailed, ValueError, OSError, KeyError, TypeError):
        # Never print a raw configuration exception: it can contain credentials.
        print("FAIL coordinator preflight; check private configuration and argument requirements", file=sys.stderr)
        raise SystemExit(2)
