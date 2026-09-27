#!/usr/bin/env python3
"""Opt-in hardware runner for the real HardwareOne P4+C6 <-> S3 protocol.

This module does nothing on import. It never flashes, resets, erases, creates a
user, or supplies Wi-Fi credentials. Provision the two application firmwares
first and close other serial clients. Then run using the private tools Python:

  python test_mesh.py                         # preserve existing configuration
  python test_mesh.py --configure --pair discovery  # provisioned, unpaired pair
  python test_mesh.py --rekey --reopen         # include recovery checks

Credentials default to private/credentials.json with string keys username,
password, mesh_passphrase, mesh_label. No secret is accepted on the command line.
--configure explicitly sets names/channel/mesh/passphrase. --pair discovery
requires both peers unpaired; it will never unpair an existing peer implicitly.
--pair secure explicitly refreshes both secure peer records. Default 'preserve'
only inspects existing identities and establishes an ephemeral session if needed.

New logs and sanitized results go under private/test-runs/<UTC timestamp>-<id>/.
File tests leave uniquely named 8 KiB fixtures on each device for inspection.
They verify receiver completion and full readback, not just sender success.
Long TEXT is reconstructed from real history reqId/piece/of records; this is
not a three-node relay test or a claim about generic CMD_RESP reassembly.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import re
import secrets
import sys
import threading
import time
from typing import Callable

from console import ANSI, ConsoleTimeout, MeshConsole, extract_json_objects


HERE = Path(__file__).resolve().parent
BOUNDARIES = (1, 201, 202, 203, 400, 401, 1024)
DEFAULT_BOARDS = {
    "p4": {"port": "/dev/cu.usbmodem2101", "mac": "fc:01:2c:e0:b9:a8", "name": "HW1_P4"},
    "s3": {"port": "/dev/cu.usbmodem1101", "mac": "68:ee:8f:50:e9:d0", "name": "HW1_S3"},
}
FATAL = re.compile(r"Guru Meditation|panic'ed|assert failed|abort\(\) was called|"
                   r"ESP_ERROR_CHECK failed|Brownout detector|Task watchdog got triggered")
WIZARD = re.compile(r"Basic Setup|Enter admin username|Enter admin password|What will you use this device for")


class CheckFailed(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailed(message)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def normalize_mac(value: str) -> str:
    require(isinstance(value, str) and re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value) is not None,
            "Invalid MAC address")
    return value.lower()


def command_token(value: str) -> str:
    # CommandArgs supports quoted tokens but does not unescape embedded quotes.
    require(isinstance(value, str) and value and all(ord(c) >= 32 for c in value)
            and '"' not in value and "\x7f" not in value,
            "Credential or token is incompatible with the firmware command parser")
    return '"' + value + '"'


def find_object(output: str, key: str, *, schema: bool = True) -> dict:
    matches = [obj for obj in extract_json_objects(output) if key in obj]
    require(len(matches) == 1, f"Expected exactly one JSON response containing {key}")
    obj = matches[0]
    if schema:
        require(obj.get("schema") == 1, "Unexpected JSON schema version")
    return obj


def select_session(document: dict, peer_mac: str, mesh_id: int | None = None) -> dict | None:
    require(document.get("schema") == 1 and isinstance(document.get("sessions"), list),
            "Invalid session envelope")
    matches = [row for row in document["sessions"]
               if isinstance(row, dict) and str(row.get("mac", "")).lower() == normalize_mac(peer_mac)
               and row.get("state") == "ACTIVE"]
    require(len(matches) <= 1, "Multiple ACTIVE sessions for the same peer")
    if not matches:
        return None
    session = matches[0]
    for key in ("sessionId", "meshId", "ageMs", "txSeq", "rxHwm"):
        require(type(session.get(key)) is int and session[key] >= 0, f"Invalid session {key}")
    require(session["sessionId"] > 0 and session.get("dir") in ("A", "B"), "Invalid active session identity")
    if mesh_id is not None:
        require(session["meshId"] == mesh_id, "Session belongs to another local mesh slot")
    return session


def check_session_pair(p4: dict, s3: dict) -> None:
    require(p4["sessionId"] == s3["sessionId"], "Peer session IDs do not match")
    require({p4["dir"], s3["dir"]} == {"A", "B"}, "Peer session directions are not opposite")


def select_registered_peer(document: dict, peer_mac: str, mesh_id: int | None = None) -> dict | None:
    """espnowlist reads the actual send resolver's gEspNow->devices registry."""
    require(document.get("schema") == 1 and isinstance(document.get("devices"), list), "Invalid paired-device registry envelope")
    require(document.get("count") == len(document["devices"]), "Paired-device registry count mismatch")
    matches = [row for row in document["devices"]
               if isinstance(row, dict) and str(row.get("mac", "")).lower() == normalize_mac(peer_mac)]
    require(len(matches) <= 1, "Duplicate local registry entry for peer")
    if not matches:
        return None
    require(matches[0].get("encrypted") is True, "Local registry peer is not securely paired")
    if mesh_id is not None:
        require(matches[0].get("meshId") == mesh_id, "Local registry peer belongs to another mesh slot")
    return matches[0]


def parse_debug_state(flags_output: str, level_output: str) -> dict:
    flags = re.search(r"Debug flags: 0x([0-9a-fA-F]{16}(?::[0-9a-fA-F]{16}){3})", ANSI.sub("", flags_output))
    level = re.search(r"Current log level: (error|warn|info|debug) \(([0-3])\)", ANSI.sub("", level_output))
    require(flags is not None and level is not None, "Could not read current debug flags and log level")
    mask = int(flags.group(1).replace(":", ""), 16)
    return {"core": bool(mask & (1 << 32)), "level": int(level.group(2)), "levelName": level.group(1)}


def history_rows(document: dict, since: int, peer_mac: str) -> tuple[list[dict], int]:
    require(document.get("schema") == 1 and isinstance(document.get("messages"), list),
            "Invalid message-history envelope")
    rows = document["messages"]
    require(len(rows) <= 8, "History page exceeds the production eight-record contract")
    cursor = since
    for row in rows:
        require(isinstance(row, dict) and type(row.get("seq")) is int and row["seq"] > cursor,
                "History sequence is duplicated, stale or out of order")
        require(str(row.get("mac", "")).lower() == normalize_mac(peer_mac), "History returned another peer")
        cursor = row["seq"]
    return rows, cursor


def reconstruct_text(rows: list[dict], peer_mac: str, msg_id: int, expected: str) -> dict | None:
    """Return evidence only once every encrypted RX piece is present and exact."""
    relevant = [row for row in rows if row.get("reqId") == msg_id
                and str(row.get("mac", "")).lower() == normalize_mac(peer_mac)
                and row.get("sent") is False and row.get("type") == 0]
    count = 1 if len(expected.encode()) <= 202 else math.ceil(len(expected.encode()) / 200)
    pieces: dict[int, str] = {}
    for row in relevant:
        require(row.get("enc") is True, "Received TEXT is not marked session-encrypted")
        require(type(row.get("piece")) is int and 1 <= row["piece"] <= count and row.get("of") == count,
                "Unexpected TEXT fragment envelope")
        require(isinstance(row.get("msg"), str), "TEXT fragment is not a string")
        require(row["piece"] not in pieces, "Duplicate TEXT fragment history record")
        pieces[row["piece"]] = row["msg"]
    if len(pieces) != count:
        return None
    actual = "".join(pieces[index] for index in range(1, count + 1))
    require(actual == expected, "Reconstructed TEXT payload differs from sent payload")
    return {"msgId": msg_id, "bytes": len(actual.encode()), "pieces": count, "encrypted": True,
            "sha256": hashlib.sha256(actual.encode()).hexdigest()}


def decode_file_chunk(document: dict, offset: int, total: int, path: str) -> bytes:
    require(document.get("success") is True, "File read was unsuccessful")
    require(document.get("path") == path and document.get("size") == total
            and document.get("offset") == offset and document.get("enc") == "b64",
            "File-read metadata differs from request")
    require(isinstance(document.get("data"), str), "Missing file-read data")
    try:
        data = base64.b64decode(document["data"], validate=True)
    except (ValueError, TypeError):
        raise CheckFailed("Invalid file-read base64") from None
    require(document.get("len") == len(data) and 0 < len(data) <= min(512, total - offset),
            "Invalid file-read chunk length")
    require(document.get("eof") is (offset + len(data) == total), "Invalid file-read EOF marker")
    return data


def deterministic_text(seed: str, length: int) -> str:
    return hashlib.shake_256(seed.encode()).hexdigest((length + 1) // 2)[:length]


class MeshRunner:
    def __init__(self, boards: dict[str, MeshConsole], config: dict, credentials: dict,
                 run_dir: Path, run_id: str, args):
        self.boards, self.config, self.credentials = boards, config, credentials
        self.run_dir, self.run_id, self.args = run_dir, run_id, args
        self.monitor_marks = {key: 0 for key in boards}
        self.monitor_tails = {key: "" for key in boards}
        self.mesh_slots = {}
        self.history_cursors = {(receiver, sender): 0 for receiver in boards for sender in boards if receiver != sender}
        self.results = {"schema": 1, "started": utc_now(), "runId": run_id, "status": "running",
                        "boards": config, "options": {"configure": args.configure, "pair": args.pair,
                                                       "initiator": args.initiator,
                                                       "rekey": args.rekey, "reopen": args.reopen},
                        "checks": [], "scope": "two-node production pairing/session/text/file transport"}
        self.save()

    def safe(self, text: str) -> str:
        for board in self.boards.values():
            text = board.redact(text)
        return text

    def save(self) -> None:
        # Sanitize before JSON escaping, so even non-ASCII secret strings cannot
        # survive as reconstructable \uXXXX escape sequences in the artifact.
        def sanitized(value):
            if isinstance(value, str):
                return self.safe(value)
            if isinstance(value, list):
                return [sanitized(item) for item in value]
            if isinstance(value, dict):
                return {self.safe(str(key)): sanitized(item) for key, item in value.items()}
            return value
        data = json.dumps(sanitized(self.results), indent=2, ensure_ascii=True) + "\n"
        path = self.run_dir / "results.json"
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(data)

    def step(self, name: str, work: Callable[[], dict | None]) -> dict | None:
        print(self.safe(f"RUN {name}"), flush=True)
        started = time.monotonic()
        try:
            evidence = work()
            self.check_health()
        except Exception as exc:
            self.results["checks"].append({"name": name, "status": "failed", "error": self.safe(str(exc)),
                                            "durationSec": round(time.monotonic() - started, 3)})
            self.save()
            raise
        self.results["checks"].append({"name": name, "status": "passed", "evidence": evidence,
                                        "durationSec": round(time.monotonic() - started, 3)})
        self.save()
        print(self.safe(f"PASS {name}"), flush=True)
        return evidence

    def check_health(self) -> None:
        for key, board in self.boards.items():
            mark = self.monitor_marks[key]
            end = board.marker()
            output = board.read_since(mark)
            self.monitor_marks[key] = end
            combined = self.monitor_tails[key] + output
            self.monitor_tails[key] = combined[-256:]
            require(FATAL.search(ANSI.sub("", combined)) is None, f"{key}: firmware panic, assert or watchdog observed")

    def command(self, key: str, command: str, *, timeout: float = 15, allow_error: bool = False) -> str:
        require(len(command.encode()) <= 2047, "Host command exceeds firmware input limit")
        output = self.boards[key].command(command, timeout=timeout)
        self.check_health()
        if not allow_error:
            # Do not include raw command/response in failures; the private console
            # logs already hold a redacted account of the exchange.
            require(re.search(r"(?:^|\n|\] )\s*(?:Error:|ERROR:|Unknown command|Permission denied|Unauthorized)",
                              ANSI.sub("", output), re.IGNORECASE) is None,
                    f"{key}: firmware rejected command {command.split()[0]}")
        return output

    def json_command(self, key: str, command: str, field: str, *, schema: bool = True,
                     timeout: float = 15, allow_error: bool = False) -> dict:
        return find_object(self.command(key, command, timeout=timeout, allow_error=allow_error), field, schema=schema)

    def login(self, key: str) -> dict:
        board = self.boards[key]
        # Attach to a silent already-running console; there need not be a boot
        # prompt. A visible first-time wizard is explicitly refused.
        try:
            board.read_until(r"\$ |Basic Setup|Enter admin |What will you use", since=0, timeout=0.5)
        except ConsoleTimeout:
            pass
        require(WIZARD.search(board.read_since(0)) is None, f"{key}: finish first-time onboarding before this runner")
        user = self.credentials["username"]
        password = self.credentials["password"]
        self.command(key, f"login {command_token(user)} {command_token(password)}", timeout=30)
        output = self.command(key, "whoami")
        require(f"You are {user} (admin)" in output, f"{key}: serial login did not establish an admin identity")
        return {"authenticatedAdmin": True}

    def configure(self, key: str) -> dict:
        spec, label = self.config[key], self.credentials["mesh_label"]
        for command in ("espnowenabled 1", f"espnowsetname {spec['name']}", "espnowchannel 6", "openespnow", "espnowmode mesh"):
            self.command(key, command, timeout=60)
        meshes = self.json_command(key, "espnowmeshes listjson", "meshes", schema=False)
        if not any(mesh.get("label") == label for mesh in meshes["meshes"]):
            self.command(key, f"espnowmeshes add {command_token(label)}")
        self.command(key, f"espnowmeshes setdefault {command_token(label)}")
        self.command(key, f"espnowsetpassphrase {command_token(label)} {command_token(self.credentials['mesh_passphrase'])}", timeout=60)
        return {"name": spec["name"], "channel": 6, "meshConfigured": True}

    def inspect(self, key: str) -> dict:
        status = self.json_command(key, "espnowstatus json", "schema")
        require(status.get("initialized") is True, f"{key}: ESP-NOW is not open; configure it or use --configure")
        require(normalize_mac(status.get("mac", "")) == self.config[key]["mac"], f"{key}: unexpected device MAC on port")
        require(status.get("channel") == 6 and status.get("channelPref") == 6, f"{key}: expected pinned channel 6")
        mode = self.json_command(key, "espnowmode json", "mode")
        require(mode.get("enabled") is True and str(mode.get("mode", "")).lower() == "mesh", f"{key}: mesh mode is not enabled")
        channel = self.command(key, "espnowchannel")
        require("OK" in channel and "not joined" in channel, f"{key}: channel checker failed or Wi-Fi is joined")
        meshes = self.json_command(key, "espnowmeshes listjson", "meshes", schema=False)
        matches = [mesh for mesh in meshes["meshes"] if mesh.get("label") == self.credentials["mesh_label"]]
        require(len(matches) == 1 and matches[0].get("enabled") is True and matches[0].get("hasPassphrase") is True,
                f"{key}: requested mesh lacks an enabled passphrase")
        # The request/accept commands pair into slot zero in the current source.
        if self.args.pair == "discovery":
            require(matches[0].get("slot") == 0, "Discovery pairing currently uses slot zero; use --pair secure for another mesh")
        self.mesh_slots[key] = matches[0]["slot"]
        return {"mac": status["mac"], "channel": 6, "wifiJoined": False, "meshSlot": matches[0]["slot"],
                "meshFingerprint": matches[0]["fingerprint"], "pairedDevices": status["pairedDevices"]}

    def known_peer(self, key: str, other: str) -> bool:
        document = self.json_command(key, "espnowsubs json", "peers")
        return any(str(row.get("mac", "")).lower() == self.config[other]["mac"] for row in document["peers"])

    def registries(self) -> dict:
        return {key: select_registered_peer(self.json_command(key, "espnowlist", "devices"), self.config[other]["mac"], self.mesh_slots.get(key))
                for key, other in (("p4", "s3"), ("s3", "p4"))}

    def pair_ready(self) -> dict:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            registries = self.registries()
            if all(registries.values()):
                sessions = self.sessions()
                probes = [self.text(sender, receiver, 64, phase="pair_ready")
                          for sender, receiver in (("p4", "s3"), ("s3", "p4"))]
                return {"localRegistries": registries, "sessions": sessions, "senderReadyProbes": probes}
            time.sleep(0.3)
        raise CheckFailed("Pairing did not populate both local send registries; identities/sessions alone are insufficient")

    def pair(self) -> dict:
        known = {key: self.known_peer(key, other) for key, other in (("p4", "s3"), ("s3", "p4"))}
        registered = self.registries()
        if self.args.pair == "preserve":
            require(all(known.values()), "Peer identities missing; use --pair discovery on unpaired boards or --pair secure")
            require(all(registered.values()), "Both peers must exist in their local send registries; identity-only pairing is incomplete")
            return {"mode": "preserve", "existingIdentities": known, "discoveryTested": False, **self.pair_ready()}
        if self.args.pair == "secure":
            for key, other in (("p4", "s3"), ("s3", "p4")):
                self.command(key, f"espnowpairsecure {self.config[other]['mac']} {self.config[other]['name']} "
                             f"{command_token(self.credentials['mesh_label'])}", timeout=30)
            return {"mode": "secure", "discoveryTested": False, **self.pair_ready()}
        require(not any(known.values()) and not any(registered.values()),
                "Discovery excludes paired peers; existing pairing was preserved, no implicit unpair performed")
        for key in self.boards:
            self.command(key, "espnowpairmode 120")
        found = set()
        deadline = time.monotonic() + 30
        while len(found) < 2 and time.monotonic() < deadline:
            for key, other in (("p4", "s3"), ("s3", "p4")):
                if self.config[other]["mac"] in self.command(key, "espnowdiscovered").lower():
                    found.add(key)
            if len(found) < 2:
                time.sleep(0.5)
        require(len(found) == 2, "Both peers did not appear in authenticated discovery")
        initiator = self.args.initiator
        accepter = "s3" if initiator == "p4" else "p4"
        marker = self.boards[accepter].marker()
        self.command(initiator, f"espnowpairrequest {self.config[accepter]['mac']}")
        self.boards[accepter].read_until(r"wants to pair", since=marker, timeout=15)
        output = self.command(accepter, f"espnowaccept {self.config[initiator]['mac']}", timeout=30)
        require("Accepted" in output, "Receiver did not accept the pairing request")
        evidence = self.pair_ready()
        for key in self.boards:
            self.command(key, "espnowpairmode off")
        return {"mode": "discovery", "initiator": initiator, "accepter": accepter,
                "discoveryTested": True, "explicitAccept": True, **evidence}

    def sessions(self, *, timeout: float = 30, kick: bool = True) -> dict[str, dict]:
        deadline = time.monotonic() + timeout
        kick_after = time.monotonic() + 3
        initiated = False
        while time.monotonic() < deadline:
            pair = {key: select_session(self.json_command(key, "espnowsessions json", "sessions"), self.config[other]["mac"], self.mesh_slots.get(key))
                    for key, other in (("p4", "s3"), ("s3", "p4"))}
            if all(pair.values()) and pair["p4"]["sessionId"] == pair["s3"]["sessionId"]:
                check_session_pair(pair["p4"], pair["s3"])
                return pair
            if kick and not initiated and time.monotonic() >= kick_after and self.known_peer("p4", "s3") and self.known_peer("s3", "p4"):
                self.command("p4", f"espnowsessionopen {self.config['s3']['mac']} {command_token(self.credentials['mesh_label'])}",
                             allow_error=True)
                initiated = True
            time.sleep(0.3)
        raise CheckFailed("A reciprocal ACTIVE session did not converge before timeout")

    def page(self, receiver: str, sender: str) -> list[dict]:
        key = (receiver, sender)
        document = self.json_command(receiver, f"espnowmessages json {self.history_cursors[key]} {self.config[sender]['mac']}", "messages")
        rows, cursor = history_rows(document, self.history_cursors[key], self.config[sender]["mac"])
        self.history_cursors[key] = cursor
        return rows

    def drain_history(self, receiver: str, sender: str) -> None:
        for _ in range(100):
            if not self.page(receiver, sender):
                return
        raise CheckFailed("History did not reach its current end")

    def text(self, sender: str, receiver: str, length: int, *, phase: str = "boundary") -> dict:
        self.drain_history(receiver, sender)
        payload = deterministic_text(f"{self.run_id}:{phase}:{sender}:{length}", length)
        reply = self.json_command(sender, f"espnowsend json {self.config[receiver]['mac']} {payload}", "ok", timeout=30)
        if length > 1024:
            require(reply.get("ok") is False and reply.get("error") == "too long", "Oversized text was not cleanly rejected")
            # Nothing else sends chat during this controlled test. Reject any
            # new received TEXT, including a leaked partial fragment.
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                rows = self.page(receiver, sender)
                require(not any(row.get("sent") is False and row.get("type") == 0 for row in rows),
                        "Rejected text or a partial fragment appeared at receiver")
                time.sleep(0.2)
            return {"bytes": length, "rejected": True, "error": "too long"}
        require(reply.get("ok") is True and type(reply.get("msgId")) is int and reply["msgId"] > 0,
                "Text send did not return an accepted message ID")
        rows = []
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            rows.extend(self.page(receiver, sender))
            evidence = reconstruct_text(rows, self.config[sender]["mac"], reply["msgId"], payload)
            if evidence is not None:
                return {"direction": f"{sender}->{receiver}", **evidence}
            time.sleep(0.2)
        raise CheckFailed("Encrypted TEXT history did not contain every expected piece")

    def file(self, sender: str, receiver: str) -> dict:
        data = hashlib.shake_256(f"{self.run_id}:file:{sender}".encode()).digest(8192)
        filename = f"mesh_{self.run_id}_{sender}.bin"
        source = "/" + filename
        for offset in range(0, len(data), 384):
            chunk = data[offset:offset + 384]
            final = offset + len(chunk) == len(data)
            command = f'filewrite "{source}" {offset} {base64.b64encode(chunk).decode()}' + (" final" if final else "")
            reply = self.json_command(sender, command, "success", schema=False, timeout=30)
            require(reply.get("success") is True and reply.get("size") == offset + len(chunk)
                    and reply.get("final") is final, "Staging file write was not committed sequentially")
        self.read_file(sender, source, data)  # establish exactly what was staged
        marker = self.boards[receiver].marker()
        before = self.sessions(kick=False)
        output = self.command(sender, f'espnowsendfile {self.config[receiver]["mac"]} "{source}"', timeout=90)
        require("File sent successfully" in output, "File sender did not complete")
        completion = rf"\[V4_FILE\] Complete(?: \(streamed\))?: {re.escape(filename)} \(8192 bytes\)"
        self.boards[receiver].read_until(completion, since=marker, timeout=30)
        destination = f"/espnow/received/{self.config[sender]['mac'].replace(':', '').upper()}/{filename}"
        evidence = self.read_file(receiver, destination, data)
        after = self.sessions(kick=False)
        require(after[sender]["sessionId"] == before[sender]["sessionId"]
                and after[sender]["txSeq"] > before[sender]["txSeq"]
                and after[receiver]["rxHwm"] > before[receiver]["rxHwm"],
                "Session counters did not advance across file transfer")
        return {"direction": f"{sender}->{receiver}", **evidence, "receiverCompletion": True,
                "sessionId": after[sender]["sessionId"], "rxPathRequiresSessionEncryption": True}

    def read_file(self, key: str, path: str, expected: bytes) -> dict:
        content = bytearray()
        while len(content) < len(expected):
            document = self.json_command(key, f'fileread "{path}" {len(content)} 512 b64', "success", schema=False)
            content.extend(decode_file_chunk(document, len(content), len(expected), path))
        require(content == expected, "File readback differs from deterministic source bytes")
        return {"path": path, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}

    @contextlib.contextmanager
    def rekey_logging(self):
        saved, changed = {}, []
        try:
            for key, board in self.boards.items():
                marker = board.marker()
                self.command(key, "debugflags")
                flags = board.read_until(r"Debug flags: 0x[0-9a-fA-F]{16}(?::[0-9a-fA-F]{16}){3}", since=marker, timeout=10)
                saved[key] = parse_debug_state(flags, self.command(key, "loglevel"))
            for key, state in saved.items():
                if not state["core"]:
                    changed.append((key, "core"))
                    self.command(key, "debugespnowcore 1 temp")
                if state["level"] < 2:
                    # loglevel has no temporary variant. Restore its previous
                    # persisted value in finally; never read a debug toggle
                    # without arguments (that handler would disable it).
                    changed.append((key, "level"))
                    self.command(key, "loglevel info")
            yield saved
        finally:
            failures = []
            for key, field in reversed(changed):
                try:
                    command = (f"loglevel {saved[key]['levelName']}" if field == "level"
                               else f"debugespnowcore {int(saved[key]['core'])} temp")
                    self.command(key, command)
                except Exception:
                    failures.append(f"{key}:{field}")
            if failures:
                self.results["debugRestoreFailures"] = failures
                self.save()
                raise CheckFailed("Failed to restore rekey logging settings: " + ", ".join(failures))

    def rekey(self) -> dict:
        with self.rekey_logging() as previous_debug:
            before = self.sessions(kick=False)
            marks = {key: board.marker() for key, board in self.boards.items()}
            output = self.command("p4", f"espnowrekey {self.config['s3']['mac']}")
            require("REKEY sent" in output, "Rekey was not initiated")
            for key, other in (("p4", "s3"), ("s3", "p4")):
                self.boards[key].read_until(re.compile(r"SESSION rekeyed with " + re.escape(self.config[other]["mac"]), re.I),
                                            since=marks[key], timeout=30)
            after = self.sessions(kick=False)
            require(after["p4"]["sessionId"] == before["p4"]["sessionId"], "Rekey unexpectedly replaced the session identity")
            require(all(after[key]["ageMs"] < before[key]["ageMs"] for key in self.boards), "Session age did not reset on rekey")
            # Production permits the previous RX key for 5000 ms. Wait from
            # observing BOTH installations, so subsequent delivery proves the
            # current keys work after that fallback has expired on both boards.
            grace_start = time.monotonic()
            while time.monotonic() - grace_start < 5.5:
                time.sleep(max(0, min(0.2, 5.5 - (time.monotonic() - grace_start))))
                self.check_health()
            waited_ms = int((time.monotonic() - grace_start) * 1000)
        return {"before": before, "after": after, "bothReportedNewKeys": True,
                "previousKeyGraceMs": 5000, "waitedAfterBothConfirmationsMs": waited_ms,
                "priorDebugSettings": previous_debug, "debugSettingsRestored": True}

    def stats_snapshot(self, label: str) -> dict:
        snapshots = {key: self.json_command(key, "espnowstats json", "rxRingDrops") for key in self.boards}
        for document in snapshots.values():
            require(type(document.get("rxRingDrops")) is int, "Missing RX ring-drop counter")
        self.results.setdefault("statistics", {})[label] = snapshots
        self.save()
        return snapshots

    def capture_failure_stats(self, *, timeout: float = 5.0) -> None:
        """Best effort while ports are open; each board has a total deadline.

        Bypass check_health: an already-detected panic must not discard usable
        counters from the other board. A daemon worker bounds even a blocked
        serial write; closing the owning console subsequently wakes its reader.
        Workers only publish to an in-memory queue, never mutate results late.
        """
        workers = {}
        for key, board in self.boards.items():
            mailbox = queue.SimpleQueue()
            def sample(board=board, mailbox=mailbox):
                try:
                    document = find_object(board.command("espnowstats json", timeout=timeout), "rxRingDrops")
                    require(type(document.get("rxRingDrops")) is int, "Missing RX ring-drop counter")
                    mailbox.put((document, None))
                except BaseException as exc:
                    mailbox.put((None, f"{type(exc).__name__}: {exc}"))
            worker = threading.Thread(target=sample, name=f"mesh-final-stats-{key}", daemon=True)
            deadline = time.monotonic() + timeout
            worker.start()
            workers[key] = (worker, mailbox, deadline)
        snapshots, errors = {}, {}
        for key, (worker, mailbox, deadline) in workers.items():
            worker.join(max(0, deadline - time.monotonic()))
            if worker.is_alive():
                errors[key] = f"Statistics capture exceeded {timeout:g}s deadline"
                continue
            document, error = mailbox.get_nowait()
            if error is not None:
                errors[key] = self.safe(error)
            else:
                snapshots[key] = document
        self.results.setdefault("statistics", {})["end"] = snapshots
        self.results["endStatisticsCapture"] = {"reason": "run_failure", "timeoutPerBoardSec": timeout,
                                                "errors": errors}
        self.save()

    def run_with_failure_capture(self, startup_delay: float) -> None:
        # Called inside ExitStack: retain live consoles until failure evidence
        # has been requested. Cleanup failures must never replace the test error.
        try:
            time.sleep(startup_delay)
            self.run()
        except BaseException:
            try:
                self.capture_failure_stats()
            except BaseException:
                self.results["endStatisticsCaptureError"] = "Best-effort statistics capture or persistence failed"
            raise

    def reopen(self) -> dict:
        # Reopen only the P4 radio. Persistent identities and sessions are left
        # to the production shutdown/restart code, not edited by this runner.
        self.command("p4", "closeespnow", timeout=45)
        status = self.json_command("p4", "espnowstatus json", "schema")
        require(status.get("initialized") is False, "Radio still initialized after closeespnow")
        self.command("p4", "openespnow", timeout=60)
        self.history_cursors = {key: 0 for key in self.history_cursors}
        self.inspect("p4")
        return {"reopened": "p4", "sessions": self.sessions()}

    def run(self) -> None:
        for key in self.boards:
            self.step(f"login_{key}", lambda key=key: self.login(key))
        if self.args.configure:
            for key in self.boards:
                self.step(f"configure_{key}", lambda key=key: self.configure(key))
        inspections = {key: self.step(f"inspect_{key}", lambda key=key: self.inspect(key)) for key in self.boards}
        require(inspections["p4"]["meshFingerprint"] == inspections["s3"]["meshFingerprint"], "Mesh fingerprints differ")
        self.step("statistics_start", lambda: self.stats_snapshot("start"))
        self.step("pairing", self.pair)
        baseline = self.step("active_session", self.sessions)
        for sender, receiver in (("p4", "s3"), ("s3", "p4")):
            for length in (*BOUNDARIES, 1025):
                self.step(f"text_{sender}_to_{receiver}_{length}",
                          lambda sender=sender, receiver=receiver, length=length: self.text(sender, receiver, length))
        current = self.step("session_after_text", lambda: self.sessions(kick=False))
        require(all(current[key]["sessionId"] == baseline[key]["sessionId"]
                    and current[key]["txSeq"] > baseline[key]["txSeq"]
                    and current[key]["rxHwm"] > baseline[key]["rxHwm"] for key in self.boards),
                "Session counters did not advance bidirectionally across the text matrix")
        for sender, receiver in (("p4", "s3"), ("s3", "p4")):
            self.step(f"file_{sender}_to_{receiver}_8192", lambda sender=sender, receiver=receiver: self.file(sender, receiver))
        if self.args.rekey:
            self.step("rekey", self.rekey)
            for sender, receiver in (("p4", "s3"), ("s3", "p4")):
                self.step(f"after_rekey_{sender}", lambda sender=sender, receiver=receiver: self.text(sender, receiver, 401, phase="rekey"))
        if self.args.reopen:
            self.step("radio_reopen", self.reopen)
            for sender, receiver in (("p4", "s3"), ("s3", "p4")):
                self.step(f"after_reopen_{sender}", lambda sender=sender, receiver=receiver: self.text(sender, receiver, 401, phase="reopen"))
        self.step("statistics_end", lambda: self.stats_snapshot("end"))
        self.results["status"] = "passed"
        self.results["finished"] = utc_now()
        self.save()


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--credentials", type=Path, default=HERE / "private" / "credentials.json")
    parser.add_argument("--run-root", type=Path, default=HERE / "private" / "test-runs")
    parser.add_argument("--p4-port", default=DEFAULT_BOARDS["p4"]["port"])
    parser.add_argument("--s3-port", default=DEFAULT_BOARDS["s3"]["port"])
    parser.add_argument("--p4-mac", default=DEFAULT_BOARDS["p4"]["mac"])
    parser.add_argument("--s3-mac", default=DEFAULT_BOARDS["s3"]["mac"])
    parser.add_argument("--configure", action="store_true", help="Explicitly write names, channel, mesh label and passphrase")
    parser.add_argument("--pair", choices=("preserve", "discovery", "secure"), default="preserve")
    parser.add_argument("--initiator", choices=("p4", "s3"), default="p4",
                        help="Device that sends the discovery pair request (default p4)")
    parser.add_argument("--rekey", action="store_true", help="Rotate session keys, then repeat bidirectional fragmented text")
    parser.add_argument("--reopen", action="store_true", help="Close/reopen P4 radio, then repeat bidirectional fragmented text")
    parser.add_argument("--startup-delay", type=float, default=8.0,
                        help="Seconds to allow boot after opening ports (default 8; USB reconnect may reboot these boards)")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    runner = None
    secret_values: list[str] = []
    def safe(text):
        for value in sorted(secret_values, key=len, reverse=True):
            text = text.replace(value, "[REDACTED]")
        return text
    try:
        credentials = json.loads(args.credentials.read_text())
        require(isinstance(credentials, dict), "Credentials file must contain a JSON object")
        for key in ("username", "password", "mesh_passphrase", "mesh_label"):
            require(isinstance(credentials.get(key), str) and credentials[key], f"Credentials missing required string {key}")
        secret_values = [credentials[key] for key in ("username", "password", "mesh_passphrase")]
        for value in credentials.values():
            if isinstance(value, str):
                command_token(value)
        require(8 <= len(credentials["mesh_passphrase"]) <= 128, "Mesh passphrase has invalid length")
        require(1 <= len(credentials["mesh_label"]) <= 16 and not re.search(r'[\s"/]', credentials["mesh_label"]),
                "Mesh label is incompatible with firmware")
        config = {key: {**spec, "port": getattr(args, key + "_port"), "mac": normalize_mac(getattr(args, key + "_mac"))}
                  for key, spec in DEFAULT_BOARDS.items()}
        require(config["p4"]["port"] != config["s3"]["port"] and config["p4"]["mac"] != config["s3"]["mac"],
                "Boards must have distinct ports and MAC addresses")
        require(0 <= args.startup_delay <= 60, "Startup delay must be between 0 and 60 seconds")
        run_id = secrets.token_hex(4)
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        args.run_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        run_dir = args.run_root / f"{stamp}-{run_id}"
        run_dir.mkdir(mode=0o700)
        with contextlib.ExitStack() as stack:
            boards = {key: stack.enter_context(MeshConsole(spec["port"], run_dir / f"{key}.log", secrets=secret_values,
                                                          completion="hardwareone"))
                      for key, spec in config.items()}
            runner = MeshRunner(boards, config, credentials, run_dir, run_id, args)
            runner.run_with_failure_capture(args.startup_delay)
        print(safe(f"PASS {len(runner.results['checks'])} checks; results: {run_dir / 'results.json'}"), flush=True)
        return 0
    except BaseException as exc:
        if runner is not None:
            runner.results.update(status="interrupted" if isinstance(exc, KeyboardInterrupt) else "failed",
                                  finished=utc_now(), error=safe(str(exc)))
            try:
                runner.save()
            except BaseException:
                # Preserve and report the original failure even if final result
                # persistence fails (for example, a full host filesystem).
                pass
        # No traceback or raw serial response; exceptions can include filenames
        # but registered credential strings are always removed before output.
        print(safe(f"FAIL {type(exc).__name__}: {exc}"), file=sys.stderr, flush=True)
        return 130 if isinstance(exc, KeyboardInterrupt) else 1


if __name__ == "__main__":
    raise SystemExit(main())
