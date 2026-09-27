#!/usr/bin/env python3
"""Exercise the isolated P4/C6 and S3 ESP-NOW probes; never flash or reset USB.

Requires pyserial. Each run writes timestamped logs, events.jsonl and summary.json
under private/test-runs/. Only these credential-free experiment firmwares are
supported. An unexpected result aborts the matrix and produces a failed summary.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import json
import math
import queue
import re
import sys
import threading
import time
import traceback
from pathlib import Path
from typing import Callable


ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
EVENT = re.compile(r"\b(HW1_HOST|HW1_PROBE):\s+([A-Z][A-Z_]+)(?:\s|$)")
FIELD = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)")
FATAL = re.compile(
    r"Guru Meditation|panic'ed|assert failed|abort\(\) was called|"
    r"ESP_ERROR_CHECK failed|Brownout detector|Task watchdog got triggered"
)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def parse_line(line: str) -> tuple[str, dict]:
    clean = ANSI.sub("", line)
    match = EVENT.search(clean)
    fields = {}
    for key, value in FIELD.findall(clean):
        fields[key] = int(value) if re.fullmatch(r"-?\d+", value) else value
    if match:
        fields["log_tag"] = match.group(1)
    return match.group(2) if match else "LOG", fields


@dataclasses.dataclass(frozen=True)
class Event:
    seq: int
    wall_time: str
    monotonic: float
    board: str
    direction: str
    kind: str
    fields: dict
    line: str


class CheckFailed(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailed(message)


def assess_ping(events: list[Event], count: int, size: int, expect_timeout: bool) -> dict:
    """Reconcile individual packet evidence with the firmware run summary."""
    relevant = [event for event in events if event.board == "p4" and event.direction == "rx"]
    starts = [event for event in relevant if event.kind == "PING_START"]
    done = [event for event in relevant if event.kind == "PING_DONE"]
    require(len(starts) == 1 and len(done) == 1, "Expected exactly one PING_START and PING_DONE")
    start, end = starts[0], done[0]
    require(start.seq < end.seq, "PING_DONE arrived before PING_START")
    require(start.fields.get("count") == count and start.fields.get("size") == size,
            f"PING_START does not match requested count={count}, size={size}")
    fields = end.fields
    for key, value in {"requested": count, "size": size, "attempted": count,
                       "replies": 0 if expect_timeout else count,
                       "timeouts": count if expect_timeout else 0, "rejected": 0}.items():
        require(fields.get(key) == value, f"PING_DONE {key}={fields.get(key)!r}, expected {value}")
    packets = [event for event in relevant
               if start.seq < event.seq < end.seq and event.kind in ("PING_REPLY", "PING_TIMEOUT")]
    ids = [event.fields.get("id") for event in packets]
    require(len(packets) == count and len(set(ids)) == count and None not in ids,
            f"Packet evidence incomplete or duplicated: {len(packets)} events, {len(set(ids))} IDs")
    expected_kind = "PING_TIMEOUT" if expect_timeout else "PING_REPLY"
    for event in packets:
        require(event.kind == expected_kind and event.fields.get("len") == size,
                f"Unexpected packet event: {event.line}")
        if not expect_timeout:
            require(event.fields.get("pattern") == "ok", f"Payload validation missing: {event.line}")
            rtt = event.fields.get("rtt_us")
            require(isinstance(rtt, int) and 0 <= rtt <= 200000, f"Invalid RTT: {event.line}")
    errors = [event.line for event in relevant if event.kind in
              ("PING_REJECT", "SEND_REJECT", "RX_INVALID", "ECHO_UNMATCHED", "ECHO_LATE")]
    require(not errors, f"Unexpected packet errors: {errors[:4]}")
    result = {"destination": start.fields.get("dst"), "firmware": fields,
              "packet_events": len(packets), "first_packet_id": min(ids), "last_packet_id": max(ids),
              "expected_timeout": expect_timeout}
    if not expect_timeout:
        rtts = sorted(event.fields["rtt_us"] for event in packets)
        require(fields.get("rtt_min_us") == min(rtts) and fields.get("rtt_max_us") == max(rtts)
                and fields.get("rtt_avg_us") == sum(rtts) // count,
                "Firmware RTT aggregate differs from individual replies")
        result["rtt_us"] = {"min": min(rtts), "mean": sum(rtts) / count, "max": max(rtts),
                            **{f"p{p}": rtts[max(0, math.ceil(count * p / 100) - 1)]
                               for p in (50, 95, 99)}}
    return result


class SerialHub:
    def __init__(self, directory: Path, ports: dict[str, str], baud: int):
        self.directory = directory
        self.ports = ports
        self.baud = baud
        self.events: queue.Queue[Event] = queue.Queue()
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.sequence = 0
        self.connections = {}
        self.threads = []
        self.logs = {name: (directory / f"{name}.log").open("w", encoding="utf-8", buffering=1)
                     for name in ports}
        self.json_log = (directory / "events.jsonl").open("w", encoding="utf-8", buffering=1)

    def record(self, board: str, direction: str, line: str) -> Event:
        kind, fields = parse_line(line) if direction == "rx" else (direction.upper(), {})
        with self.lock:
            self.sequence += 1
            event = Event(self.sequence, utc_now(), time.monotonic(), board, direction, kind, fields, line)
            self.logs[board].write(f"{event.wall_time} [{event.monotonic:.6f}] {direction.upper()} {line}\n")
            self.json_log.write(json.dumps(dataclasses.asdict(event), sort_keys=True) + "\n")
            self.events.put(event)
        return event

    def open(self) -> None:
        import serial  # Optional until actually running against hardware.

        for name, port in self.ports.items():
            connection = serial.Serial(port=None, baudrate=self.baud, timeout=0.2,
                                       write_timeout=2.0, exclusive=True)
            # Set before opening. Do not toggle modem lines, send boot sequences,
            # clear serial buffers, or invoke esptool.
            connection.dtr = False
            connection.rts = False
            connection.port = port
            connection.open()
            self.connections[name] = connection
            self.record(name, "open", port)
            thread = threading.Thread(target=self.read_loop, args=(name, connection), daemon=True,
                                      name=f"read-{name}")
            self.threads.append(thread)
            thread.start()

    def read_loop(self, name: str, connection) -> None:
        pending = bytearray()
        try:
            while not self.stop.is_set():
                chunk = connection.read(max(1, min(connection.in_waiting, 4096)))
                if not chunk:
                    continue
                pending.extend(chunk)
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending[:] = remainder
                    self.record(name, "rx", line.rstrip(b"\r").decode("utf-8", errors="replace"))
                if len(pending) > 65536:
                    self.record(name, "io_error", "Serial line exceeded 65536 bytes")
                    return
        except Exception as error:
            if not self.stop.is_set():
                self.record(name, "io_error", repr(error))
        finally:
            if pending:
                self.record(name, "partial", pending.decode("utf-8", errors="replace"))

    def send(self, board: str, command: str) -> int:
        require("\n" not in command and "\r" not in command, "Command must fit one line")
        marker = self.record(board, "command", command)
        connection = self.connections[board]
        encoded = (command + "\n").encode("ascii")
        written = connection.write(encoded)
        require(written == len(encoded), f"Short serial write to {board}")
        connection.flush()
        return marker.seq

    def close(self) -> None:
        self.stop.set()
        for connection in self.connections.values():
            connection.close()
        for thread in self.threads:
            thread.join(timeout=2.0)
        for logfile in self.logs.values():
            logfile.close()
        self.json_log.close()


class Runner:
    def __init__(self, hub: SerialHub, summary: dict, summary_path: Path):
        self.hub = hub
        self.summary = summary
        self.summary_path = summary_path
        self.history: list[Event] = []
        self.current_step = None

    def save(self) -> None:
        temporary = self.summary_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        temporary.replace(self.summary_path)

    def wait(self, board: str, after: int, predicate: Callable[[Event], bool], timeout: float,
             description: str, *, allow_fatal: bool = False) -> Event:
        deadline = time.monotonic() + timeout
        next_progress = time.monotonic() + 10
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CheckFailed(f"Timed out after {timeout:.1f}s waiting for {board}: {description}")
            try:
                event = self.hub.events.get(timeout=min(remaining, 0.5))
            except queue.Empty:
                continue
            self.history.append(event)
            if not allow_fatal:
                if event.direction == "io_error" or (event.direction == "rx" and FATAL.search(event.line)):
                    raise CheckFailed(f"{event.board} fatal event: {event.line}")
            if event.board == board and event.seq > after and event.direction == "rx" and predicate(event):
                return event
            if time.monotonic() >= next_progress:
                print(f"Waiting: {board} {description}", flush=True)
                next_progress = time.monotonic() + 10

    def command(self, board: str, command: str, kind: str, expected: dict | None = None,
                timeout: float = 8, *, allow_fatal: bool = False) -> Event:
        marker = self.hub.send(board, command)
        event = self.wait(board, marker, lambda item: item.kind == kind, timeout,
                          f"{command!r} -> {kind}", allow_fatal=allow_fatal)
        for key, value in (expected or {}).items():
            require(event.fields.get(key) == value,
                    f"{board} {command!r}: {key}={event.fields.get(key)!r}, expected {value}")
        return event

    def stats(self, board: str, timeout: float = 8) -> dict:
        event = self.command(board, "stats", "STATS", timeout=timeout)
        return self.stats_fields(board, event)

    def stats_fields(self, board: str, event: Event) -> dict:
        expected_tag = "HW1_HOST" if board == "p4" else "HW1_PROBE"
        require(event.fields.get("log_tag") == expected_tag, f"Wrong firmware on {board}: {event.line}")
        require(event.fields.get("peers_status") == "ESP_OK" and
                event.fields.get("channel_status") == "ESP_OK", f"Stats RPC failed: {event.line}")
        fields = dict(event.fields)
        if board == "p4":
            bridge = self.wait("p4", event.seq, lambda item: item.kind == "BRIDGE_STATS",
                               5, "BRIDGE_STATS after STATS")
            fields["bridge"] = bridge.fields
        return fields

    def discover(self, timeout: float = 20) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.stats("p4", timeout=min(8, max(0.1, deadline - time.monotonic())))
            if state.get("peer_known") == 1 and state.get("peers", 0) >= 2:
                return state
            time.sleep(0.5)
        raise CheckFailed("P4 did not discover and register the S3 peer")

    def step(self, name: str, action: Callable[[], dict]) -> dict:
        print(f"TEST {name}", flush=True)
        self.current_step = {"name": name, "status": "running", "started_at": utc_now()}
        self.summary["tests"].append(self.current_step)
        started = time.monotonic()
        self.save()
        try:
            result = action()
            self.current_step.update(status="passed", result=result)
            print(f"PASS {name}", flush=True)
            return result
        except BaseException as error:
            self.current_step.update(status="failed", error=f"{type(error).__name__}: {error}")
            print(f"FAIL {name}: {error}", flush=True)
            raise
        finally:
            self.current_step.update(finished_at=utc_now(), duration_seconds=time.monotonic() - started)
            self.save()

    def ping(self, count: int, size: int, expect_timeout: bool = False) -> dict:
        marker = self.hub.send("p4", f"ping {count} {size}")
        start = self.wait("p4", marker, lambda item: item.kind in ("PING_START", "PING_REJECT"),
                          10, "PING_START")
        require(start.kind == "PING_START", f"Ping command rejected: {start.line}")
        done = self.wait("p4", start.seq, lambda item: item.kind == "PING_DONE",
                         count * 0.25 + 30, f"PING_DONE count={count} size={size}")
        # Include the completed firmware summary even when packet reconciliation fails.
        if self.current_step is not None:
            self.current_step["observed_ping_done"] = done.fields
        observations = [event for event in self.history if marker < event.seq <= done.seq]
        return assess_ping(observations, count, size, expect_timeout)

    def set_channel(self, board: str, channel: int) -> dict:
        ack = self.command(board, f"channel {channel}", "CHANNEL", {"requested": channel, "result": "ESP_OK"})
        # Both firmwares emit stats automatically after CHANNEL. Drain that
        # complete response (including BRIDGE_STATS) before the next command.
        event = self.wait(board, ack.seq, lambda item: item.kind == "STATS", 8, "channel STATS")
        state = self.stats_fields(board, event)
        require(state.get("channel") == channel, f"{board} did not switch to channel {channel}")
        return state

    def peer_test(self) -> dict:
        marker = self.hub.send("p4", "peer_test")
        done = self.wait("p4", marker, lambda item: item.kind == "PEER_TEST_DONE", 30, "peer semantics test")
        require(done.fields.get("failures") == 0, f"Peer test failed: {done.line}")
        checks = [event for event in self.history if event.board == "p4" and marker < event.seq <= done.seq
                  and event.kind == "PEER_TEST"]
        require(len(checks) >= 15 and all(event.fields.get("pass") == 1 for event in checks),
                "Peer test did not provide complete passing operation evidence")
        require(done.fields.get("capacity") == 20, "Unexpected tested peer table capacity")
        return {"summary": done.fields, "checks": [event.fields for event in checks]}

    def restart(self, board: str) -> dict:
        before = self.stats(board)
        marker = self.hub.send(board, "restart_radio")
        ready = self.wait(board, marker, lambda item: item.kind == "READY", 60, "radio restart READY")
        require(ready.fields.get("role") == ("hosted_p4" if board == "p4" else "native_s3"),
                "Unexpected role after radio restart")
        if board == "p4":
            self.wait(board, ready.seq,
                      lambda item: item.kind == "RADIO_RESTART" and " complete" in item.line,
                      10, "RADIO_RESTART complete")
        after = self.stats(board)
        require(after.get("restarts") == before.get("restarts", 0) + 1,
                f"{board} restart counter failed to increment; possible full host reboot")
        discovered = self.discover()
        return {"before": before, "ready": ready.fields, "after": after, "discovery": discovered}

    def initial(self) -> dict:
        # Explicitly establish known state, without assuming fresh firmware boots.
        # USB can enumerate before app_main creates the console reader. Reader
        # threads already retain boot logs throughout this short settling time.
        print("Waiting 5s for firmware console startup", flush=True)
        time.sleep(5)
        p4_before = self.initial_stats("p4")
        s3_before = self.initial_stats("s3")
        require(p4_before.get("ping_active") == 0, "Another P4 ping run is active")
        self.command("s3", "silence 0", "SILENCE", {"enabled": 0})
        self.set_channel("s3", 6)
        self.set_channel("p4", 6)
        discovery = self.discover()
        self.check_bridge(discovery["bridge"])
        return {"before": {"p4": p4_before, "s3": s3_before},
                "baseline": {"p4": discovery, "s3": self.stats("s3")}}

    def initial_stats(self, board: str) -> dict:
        deadline = time.monotonic() + 45
        attempts = 0
        while time.monotonic() < deadline:
            attempts += 1
            try:
                state = self.stats(board, timeout=min(3, max(0.1, deadline - time.monotonic())))
                self.summary.setdefault("startup_stats_attempts", {})[board] = attempts
                return state
            except CheckFailed as error:
                # Only an unanswered startup command is retryable. Malformed
                # responses, failed RPC status and crash reports abort at once.
                if not str(error).startswith("Timed out after"):
                    raise
                print(f"Startup console retry: {board} attempt={attempts}", flush=True)
        raise CheckFailed(f"{board} console did not respond to stats within 45s ({attempts} attempts)")

    @staticmethod
    def check_bridge(bridge: dict) -> None:
        for field in ("malformed", "event_drops", "request_timeouts", "transport_errors"):
            require(bridge.get(field) == 0, f"Unexpected bridge {field}={bridge.get(field)!r}")

    def final_stats(self, baseline: dict) -> dict:
        final = {board: self.stats(board) for board in ("p4", "s3")}
        for board, state in final.items():
            require(state.get("channel") == 6, f"{board} final channel is not 6")
            for field in ("invalid", "callback_drops", "peer_errors", "rejected"):
                require(state.get(field, 0) == baseline[board].get(field, 0),
                        f"{board} accumulated unexpected {field}: {baseline[board].get(field)} -> {state.get(field)}")
        require(final["s3"].get("silence") == 0, "S3 was left silent")
        require(final["p4"].get("peer_test_failures", 0) == baseline["p4"].get("peer_test_failures", 0),
                "Peer API failures accumulated")
        self.check_bridge(final["p4"]["bridge"])
        return final

    def restore_after_failure(self) -> list[dict]:
        results = []
        for board, command, kind, expected in (
            ("s3", "silence 0", "SILENCE", {"enabled": 0}),
            ("s3", "channel 6", "CHANNEL", {"requested": 6, "result": "ESP_OK"}),
            ("p4", "channel 6", "CHANNEL", {"requested": 6, "result": "ESP_OK"}),
        ):
            if board not in self.hub.connections:
                continue
            try:
                event = self.command(board, command, kind, expected, timeout=3, allow_fatal=True)
                results.append({"board": board, "command": command, "status": "restored", "event": event.fields})
            except Exception as error:
                results.append({"board": board, "command": command, "status": "failed", "error": str(error)})
        return results

    def run_matrix(self) -> None:
        baseline = self.step("initial_discovery_and_stats", self.initial)["baseline"]
        for size in (32, 64, 128, 250):
            self.step(f"ping_100_size_{size}", lambda size=size: self.ping(100, size))
        self.step("ping_1000_size_250", lambda: self.ping(1000, 250))
        self.step("peer_capacity_and_error_semantics", self.peer_test)
        self.step("post_peer_test_ping", lambda: self.ping(20, 250))
        self.step("s3_silence_enabled", lambda: self.command("s3", "silence 1", "SILENCE", {"enabled": 1}).fields)
        self.step("silent_peer_timeouts", lambda: self.ping(10, 64, expect_timeout=True))
        self.step("s3_silence_disabled", lambda: self.command("s3", "silence 0", "SILENCE", {"enabled": 0}).fields)
        self.step("silence_recovery_ping", lambda: self.ping(20, 250))
        self.step("s3_channel_11", lambda: self.set_channel("s3", 11))
        self.step("channel_mismatch_timeouts", lambda: self.ping(10, 64, expect_timeout=True))
        self.step("p4_channel_11", lambda: self.set_channel("p4", 11))
        self.step("channel_11_ping", lambda: self.ping(20, 250))
        self.step("restore_s3_channel_6", lambda: self.set_channel("s3", 6))
        self.step("restore_p4_channel_6", lambda: self.set_channel("p4", 6))
        self.step("restored_channel_6_ping", lambda: self.ping(20, 250))
        self.step("restart_s3_radio", lambda: self.restart("s3"))
        self.step("s3_restart_recovery_ping", lambda: self.ping(20, 250))
        for attempt in (1, 2):
            self.step(f"restart_p4_radio_{attempt}", lambda: self.restart("p4"))
            self.step(f"p4_restart_{attempt}_recovery_ping", lambda: self.ping(20, 250))
        self.step("final_stats", lambda: self.final_stats(baseline))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--p4-port", default="/dev/cu.usbmodem2101")
    parser.add_argument("--s3-port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output-root", type=Path,
                        default=Path(__file__).resolve().parent / "private" / "test-runs")
    args = parser.parse_args(argv)
    if args.p4_port == args.s3_port:
        parser.error("P4 and S3 ports must be different")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory = args.output_root.resolve() / stamp
    directory.mkdir(parents=True, exist_ok=False)
    summary = {"schema_version": 1, "status": "running", "started_at": utc_now(),
               "ports": {"p4": args.p4_port, "s3": args.s3_port}, "baud": args.baud,
               "test_scope": "Unencrypted standalone HardwareOne V4 packet transport probe; not full application pairing",
               "usb_reset": False, "directory": str(directory), "tests": []}
    hub = SerialHub(directory, summary["ports"], args.baud)
    runner = Runner(hub, summary, directory / "summary.json")
    runner.save()
    print(f"Run logs: {directory}", flush=True)
    code = 1
    try:
        hub.open()
        runner.run_matrix()
        summary["status"] = "passed"
        code = 0
    except BaseException as error:
        summary["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
        summary["error"] = f"{type(error).__name__}: {error}"
        summary["traceback"] = traceback.format_exc()
        print(f"ABORT {summary['error']}", file=sys.stderr, flush=True)
        summary["cleanup"] = runner.restore_after_failure()
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        hub.close()
        summary["finished_at"] = utc_now()
        summary["passed_tests"] = sum(test["status"] == "passed" for test in summary["tests"])
        runner.save()
        print(f"RESULT {summary['status']}: {directory / 'summary.json'}", flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
