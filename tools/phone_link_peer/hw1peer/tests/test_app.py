"""app: the full app stack (channel + inner + records + relay) over the fake GATT.

The P4 side is ``p4_model`` (a port of the firmware's pipe core, GATT client and a reduced
console driver), so every frame the app sends is checked by the firmware's own rules. X1's
first frames are also replayed through the app byte for byte, from the CCCD write to the
console OPEN_OK, at all four MTUs.
"""

import contextlib
import io
import signal
import socket
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
from pathlib import Path

from hw1peer import inner, records
from hw1peer.app import (CODE_INLINE, HANDSHAKE_DEADLINE_MS, LINK_SILENCE_MS, LOGIN_INLINE, NOTIFY_STUCK_MS,
                         PROMPT_HINT, SERVICE_RETRY_LIMIT, SERVICE_RETRY_MS, CommandFileTail, Credentials, Inbox,
                         Interrupts, Output, Peer, Phase, PromptGuard, Request, RequestKind, Settings,
                         acquire_instance_lock, credentials_from_entry, main, mask, read_code, read_login,
                         redact_command, repl, split_verb)
from hw1peer.channel import DIR_D2P, DIR_P2D, open_data
from hw1peer.gatt import RadioState
from hw1peer.inner import Error, Op
from hw1peer.relay import TcpRelay
from hw1peer.tests import load_fixture, unhex
from hw1peer.tests.fake_gatt import CODE, OTHER_CODE, Bench, FakePeripheral
from hw1peer.tests.p4_model import GattPhase, HelloEnd, StState

X1 = "phone_pipe_v2_sessions.json"
IPV4_DOC = bytes([192, 0, 2, 10])


def x1_random(x1: dict):
    """The X1 phone's draws: private key (32), handshake nonce (16), pipe nonce (8)."""
    key, nonce = unhex(x1["phone_private_key_hex"]), unhex(x1["phone_nonce_hex"])
    pipe_nonce = unhex(x1["pipe_nonces"]["phone_hex"])

    def draw(size: int) -> bytes:
        return {32: key, 16: nonce, 8: pipe_nonce}.get(size) or bytes(range(1, size + 1))
    return draw


# ---- X1 through the whole app ------------------------------------------------------------------

def test_x1_prefix_through_the_app_byte_for_byte():
    """CCCD write -> HELLO, HELLO_ACK -> CONFIRM, CONFIRM_ACK, pipe HELLO -> HELLO_ACK, console
    OPEN -> OPEN_OK: every phone value equals X1's, at MTU 64/244/247/517."""
    x1 = load_fixture(X1)
    for session in x1["sessions"]:
        frames = session["frames"]
        assert [f.get("op") or f["kind"] for f in frames[:9]] == [
            "cccd_enable", "secure_hello", "secure_hello_ack", "secure_confirm", "secure_confirm_ack",
            "HELLO", "HELLO_ACK", "OPEN", "OPEN_OK"]
        now = [0]
        fake = FakePeripheral()
        peer = Peer(fake, Output(None, stream=io.StringIO()), settings=Settings(relay_on=True, console_wanted=True),
                    clock=lambda: now[0], random=x1_random(x1))
        peer.start()
        peer.creds = Credentials(bytearray(unhex(x1["psk_hex"])), is_code=True)  # X1 offers the console
        peer.tick()
        assert fake.advertising
        received: list[bytes] = []
        fake.connect("c1", session["att_mtu"], received.append)
        fake.subscribe("c1")
        for f in frames[1:9]:
            while now[0] < f["t_ms"]:
                now[0] += 1
                peer.tick()
            if f["dir"] == "p4_to_phone":
                fake.write("c1", unhex(f["value_hex"]))
            else:
                deadline = now[0] + 100
                while len(received) < sum(1 for g in frames[1:f["i"] + 1] if g["dir"] == "phone_to_p4"):
                    assert now[0] < deadline, f"MTU {session['att_mtu']}: frame {f['i']} never sent"
                    now[0] += 1
                    peer.tick()
                assert received[-1] == unhex(f["value_hex"]), f"MTU {session['att_mtu']} frame {f['i']}"
        assert peer.console.stream == 1
        assert not fake.advertising


# ---- loopback scenarios ---------------------------------------------------------------------------

def test_handshake_and_pipe_hello():
    bench = Bench()
    assert bench.fake.advertising, "advertises once a secret is held"
    bench.up()
    session = bench.peer.session()
    assert session is not None and session.ready
    assert bench.p4.pipe.peer_flags() == inner.CONSOLE_WANTED  # relay off by default
    assert bench.p4.pipe.value == session.value == inner.frame_limit(247)
    assert not bench.fake.advertising, "advertising stops for the admitted link"
    assert "pipe READY: frame limit 219; the P4 offers the console" in bench.text()


def test_console_info_command_and_multi_record_text():
    bench = Bench()
    bench.console_up()
    assert '"model":"P4X-EYE"' in bench.text()
    bench.command("cmd status")
    bench.until(lambda: "P4> OK: phone link up" in bench.text(), what="status reply")
    bench.command("cmd help")  # about 1.9 KB: a TEXT chain of two records
    bench.until(lambda: "P4> OK" in bench.lines(), what="help reply")
    printed = [line for line in bench.lines() if line.startswith("P4> line ")]
    assert printed == [f"P4> line {i}: help text for command {i}" for i in range(60)]
    assert bench.p4.console.commands == ["status", "help"]
    assert bench.peer.console.endpoint.stats.records >= 4  # INFO, TEXT, TEXT+MORE, TEXT
    assert bench.p4.ends == [] and bench.p4.ready


def test_relay_open_refused_16_while_relay_off_and_link_stays_up():
    bench = Bench()
    bench.console_up()
    stream = bench.p4.open_relay(1, IPV4_DOC, 443, bench.now)
    assert stream
    bench.until(lambda: bench.p4.pipe.find(stream).state is StState.ENDED, what="relay refusal")
    s = bench.p4.pipe.find(stream)
    assert s.error == Error.SERVICE_OFF and s.remote_end
    assert "refused an OPEN from the P4 with 16 service_off" in bench.text()
    bench.step(20_000)
    assert "sent LINK_STATE: relay off, console wanted" in bench.text(), "§6.2 #3: LINK_STATE after a 16"
    assert bench.p4.ready and bench.p4.ends == []
    bench.command("cmd status")
    bench.until(lambda: bench.text().count("P4> OK: phone link up") == 1, what="console still works")


def test_link_state_follows_relay_and_console_switches():
    bench = Bench()
    bench.console_up()
    first = bench.peer.console.stream
    bench.command("relay on")
    bench.until(lambda: bench.p4.pipe.peer_flags() == inner.RELAY_ON | inner.CONSOLE_WANTED, what="RELAY_ON")
    bench.command("console off")
    bench.until(lambda: bench.p4.pipe.peer_flags() == inner.RELAY_ON, 2_000, "CONSOLE_WANTED 0")
    bench.until(lambda: bench.peer.console.stream == 0, what="the P4's CLOSE 4")
    assert "console: closed: the P4 closed it (CLOSE 4 not_wanted)" in bench.text()
    assert bench.p4.console.closes == [4]
    bench.command("console on")
    bench.until(lambda: bench.peer.console.stream > first, 3_000, "reopen on the wanted rise")
    bench.until(lambda: bench.peer.console.info is not None, what="INFO again")
    # A flap inside the 1 s spacing: the unsent LINK_STATE is overwritten, the P4 ends where we are.
    sent = bench.peer.session().stats.link_states_out
    bench.command("relay off")
    bench.command("relay on")
    bench.step(2_000)
    assert bench.p4.pipe.peer_flags() == inner.RELAY_ON | inner.CONSOLE_WANTED
    assert bench.peer.session().stats.link_states_out - sent <= 1


def test_ping_pong_both_ways_and_a_quiet_link_stays_up():
    bench = Bench()
    bench.up()
    bench.command("ping")
    bench.until(lambda: bench.peer.session().stats.pings_out == 1 and not bench.peer.session().ping_pending,
                what="PONG")
    # Three idle minutes: the P4's link PING (60 s idle) keeps both watchdogs quiet.
    bench.step(180_000)
    assert bench.p4.ready and bench.p4.ends == []
    assert bench.p4.pipe.pongs_received >= 2
    assert bench.peer.session().stats.pings_in >= 2
    assert "link ended" not in bench.text()


def test_version_mismatch_mode_strikes_then_recovers():
    bench = Bench()
    bench.command("mismatch on")
    for strike in range(1, 4):
        bench.until(lambda: bench.fake.advertising, what="advertising")
        bench.connect()
        bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="the P4 ends on the version signal")
        end = bench.p4.ends[-1]
        assert end.reason == Error.UNSUPPORTED_VERSION and end.hello_end is HelloEnd.VERSION_SIGNAL and end.peer_reset
        assert bench.peer.strikes_sent == strike
    assert bench.text().count("answered the P4's HELLO with the version signal") == 3
    bench.command("mismatch off")
    bench.until(lambda: bench.fake.advertising, what="advertising")
    bench.up()
    assert bench.p4.ready_sessions == 1


def test_version_signal_bytes_on_the_wire():
    """The phone's first inner frame is exactly 01 25 0000 00000000 0200 03 00 0200 (§5.4)."""
    bench = Bench(settings=Settings(mismatch=True))
    bench.connect()
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="strike")
    assert bench.p4.frames_in == [bytes.fromhex("0125000000000000020003000200")]
    assert bench.p4.frames_in[0] == inner.VERSION_RESET


def test_notification_back_pressure_keeps_order_and_counters():
    bench = Bench()
    bench.console_up()
    sent_before = len(bench.fake.notifications)
    bench.fake.busy_budget = 4
    bench.command("cmd help")
    for _ in range(4):
        bench.step(40)
        bench.fake.ready()
    bench.until(lambda: "P4> OK" in bench.lines(), what="help reply under back-pressure")
    assert bench.fake.busy_answers == 4
    assert bench.peer.link.stats.busy == 4
    # Every DATA value after the handshake carries the next outer counter: nothing was re-sealed or lost.
    counters = [int.from_bytes(v[1:9], "big") for _, v in bench.fake.notifications if v[0] == 0x10]
    assert counters == list(range(1, len(counters) + 1))
    assert len(bench.fake.notifications) > sent_before
    assert bench.p4.ends == [] and bench.p4.ready


def test_a_notification_never_taken_ends_the_link():
    bench = Bench()
    bench.console_up()
    bench.step(500)  # let the console CREDIT go out first
    bench.fake.busy_budget = 1_000_000
    bench.command("cmd status")
    bench.step(NOTIFY_STUCK_MS + 100)
    assert "link ended: the stack took no notification for 5 s" in bench.text()
    assert bench.peer.link is None or bench.peer.link.phase.value == "ended"


def test_disconnect_and_reconnect_with_fresh_keys():
    bench = Bench()
    bench.console_up()
    first_hello = bench.fake.notifications[0][1]
    bench.p4.stop(bench.now, Error.CONNECT_FAILED)  # the P4 drops the link
    assert "link ended: the P4 unsubscribed or disconnected" in bench.text()
    assert "console: closed: the pipe session ended" in bench.text()
    assert bench.peer.link is None
    bench.until(lambda: bench.fake.advertising, 100, "advertising again")
    hellos_before = len(bench.fake.notifications)
    bench.console_up()
    assert bench.fake.notifications[hellos_before][1] != first_hello, "a new HELLO: new key and nonce"
    bench.command("cmd status")
    bench.until(lambda: "P4> OK: phone link up" in bench.text(), what="console after reconnect")
    assert bench.p4.ready_sessions == 2


def test_wrong_code_is_rejected_by_the_p4():
    bench = Bench(code=CODE, p4_code=OTHER_CODE)
    bench.connect()
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="REJECT")
    assert bench.p4.ends[-1].reason == Error.NOT_AUTHORIZED
    assert "the P4 refused the handshake (REJECT 2)" in bench.text()
    bench.until(lambda: bench.fake.advertising, 100, "advertising again")


def test_no_secret_means_no_advertising():
    bench = Bench(code=None)
    assert not bench.fake.advertising and not bench.p4.connect(bench.now)
    assert "secret: none" in (bench.peer.status_text())
    bench.give_secret(CODE.lower())  # any spelling of the code
    assert bench.fake.advertising
    bench.up()


def test_a_passphrase_secret_never_takes_the_console():
    phrase = "SyntheticPipe!123"
    bench = Bench(code=phrase, p4_code=phrase)
    bench.up()
    assert bench.p4.pipe.peer_flags() == 0, "CONSOLE_WANTED stays 0 for a passphrase (§6.2 #7)"
    bench.step(3_000)
    assert bench.p4.console.opens == 0
    bench.command("cmd status")
    assert "the console stream is not open (we do not want it" in bench.text()
    bench.command("console on")
    assert "console OPENs are still refused with 16" in bench.text()


def test_p4_console_close_2_reopens_at_once():
    bench = Bench()
    bench.console_up()
    first = bench.peer.console.stream
    bench.p4.console.close(bench.p4.pipe, bench.now, 2)  # bledisconnect
    bench.until(lambda: bench.peer.console.stream > first, 2_000, "a fresh console stream")
    assert "CLOSE 2 renewed: Session renewed" in bench.text()


def test_malformed_record_answered_with_stream_reset_1():
    bench = Bench()
    bench.console_up()
    stream = bench.peer.console.stream
    bad = records.encode(records.RecordType.INFO, 0, b"{}")  # a second INFO is malformed
    assert bench.p4.pipe.write(stream, bad) == len(bad)
    bench.until(lambda: bench.p4.pipe.find(stream) is None or bench.p4.pipe.find(stream).state is StState.ENDED
                or bench.p4.console.id != stream, what="stream RESET 1")
    assert (stream, Error.BAD_PROTOCOL, True) in bench.p4.console.ends, "the phone's stream RESET 1"
    assert any(f[1] == Op.RESET and f[2] == stream and f[12:] == b"\x01\x00" for f in bench.p4.frames_in)
    assert "malformed record from the P4" in bench.text()
    bench.until(lambda: bench.peer.console.stream > stream, 4_000, "reopen after the 2 s backoff")


def test_a_p4_violation_fails_the_session_with_a_reset_frame():
    """An unsolicited PONG is BAD_PROTOCOL: the app sends the stream-0 RESET 1 (failure_frame)
    before giving up, so the P4 ends as a peer reset with that code."""
    bench = Bench()
    bench.console_up()
    assert bench.p4.pipe._queue(Op.PONG, 0, b"\x34\x12\x00\x00")
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="the P4 sees our RESET")
    assert bench.p4.frames_in[-1][1:4] == bytes((Op.RESET, 0, 0)) and bench.p4.frames_in[-1][12:] == b"\x01\x00"
    end = bench.p4.ends[-1]
    assert end.peer_reset and end.reason == Error.BAD_PROTOCOL
    assert "link ended: failed: we refused something from the P4 (1 bad_protocol)" in bench.text()
    bench.until(lambda: bench.fake.advertising, 100, "advertising again")


def test_end_and_quit():
    bench = Bench()
    bench.up()
    bench.command("end")
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="RESET 18")
    assert bench.p4.ends[-1].reason == Error.PAUSED and bench.p4.ends[-1].peer_reset
    bench.until(lambda: bench.fake.advertising, 100, "advertising again")
    bench.up()
    bench.command("quit")
    bench.until(lambda: bench.peer.finished, 2_000, "quit")
    assert bench.p4.ends[-1].reason == Error.PAUSED
    assert bench.fake.closed and not bench.fake.advertising
    assert bench.peer.creds is None


def test_handshake_deadline_without_a_p4_answer():
    bench = Bench()
    bench.fake.connect("c9", 185, lambda value: None)  # a central that never answers HELLO
    bench.fake.subscribe("c9")
    bench.step(HANDSHAKE_DEADLINE_MS + 20)
    assert "link ended: the secure handshake timed out" in bench.text()
    bench.until(lambda: bench.fake.advertising, 7_000, "advertising after the grace period")


def test_silent_link_watchdog():
    bench = Bench()
    bench.up()
    for _ in range(LINK_SILENCE_MS // 10 + 10):  # the P4 is gone without an unsubscribe
        bench.now += 10
        bench.peer.tick()
    assert "nothing from the P4 for 75 s" in bench.text()


def test_radio_off_and_on():
    bench = Bench()
    bench.up()
    bench.fake.radio(RadioState.POWERED_OFF)
    assert bench.peer.link is None and "Bluetooth is off." in bench.text()
    bench.fake.radio(RadioState.POWERED_ON)
    bench.step(20)
    assert bench.fake.advertising


def test_second_central_is_ignored_while_busy():
    bench = Bench()
    bench.up()
    bench.fake.connect("c7", 247, lambda value: None)
    bench.fake.subscribe("c7")
    assert "another central subscribed while the pipe is busy: ignored" in bench.text()
    bench.step(100)
    assert bench.p4.ready


# ---- relay over real localhost TCP ----------------------------------------------------------------

class EchoServer:
    """A localhost TCP server that echoes until the client's EOF, then closes."""

    def __init__(self) -> None:
        self.server = socket.create_server(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        self.received = bytearray()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        conn, _ = self.server.accept()
        with conn:
            while data := conn.recv(4096):
                self.received += data
                conn.sendall(data)

    def close(self) -> None:
        self.server.close()


def realtime() -> None:
    time.sleep(0.0002)


def test_relay_on_carries_tcp_both_ways():
    echo = EchoServer()
    bench: Bench | None = None
    try:
        bench = Bench(settings=Settings(relay_on=True), realtime=realtime)
        bench.up()
        assert bench.p4.pipe.peer_flags() == inner.RELAY_ON | inner.CONSOLE_WANTED
        stream = bench.p4.open_relay(1, bytes([127, 0, 0, 1]), echo.port, bench.now)
        bench.until(lambda: bench.p4.pipe.find(stream).state is StState.OPEN, 3_000, "OPEN_OK")
        payload = bytes(range(256)) * 12  # 3072 B: more than the relay's 1024 B first window
        written = 0
        echoed = bytearray()
        while len(echoed) < len(payload):
            if written < len(payload):
                written += bench.p4.relay_write(stream, payload[written:])
            bench.step()
            echoed += bench.p4.relay_read(stream)
            assert bench.now < 60_000, "echo incomplete"
        assert bytes(echoed) == payload and bytes(echo.received) == payload
        assert f"relay: stream {stream} connected" in bench.text()
        bench.p4.relay_close(stream, bench.now)
        bench.until(lambda: bench.p4.pipe.find(stream).state is StState.ENDED, 3_000, "CLOSE_ACK")
        bench.until(lambda: not bench.relay.streams(), 1_000, "socket closed")
        assert "127.0.0.1" not in bench.text() and str(echo.port) not in bench.text()
    finally:
        echo.close()
        if bench is not None:
            bench.close()


def test_relay_off_ends_live_relay_streams_with_16():
    echo = EchoServer()
    bench: Bench | None = None
    try:
        bench = Bench(settings=Settings(relay_on=True), realtime=realtime)
        bench.up()
        stream = bench.p4.open_relay(1, bytes([127, 0, 0, 1]), echo.port, bench.now)
        bench.until(lambda: bench.p4.pipe.find(stream).state is StState.OPEN, 3_000, "OPEN_OK")
        bench.command("relay off")
        bench.until(lambda: bench.p4.pipe.find(stream).state is StState.ENDED, 2_000, "RESET 16")
        s = bench.p4.pipe.find(stream)
        assert s.error == Error.SERVICE_OFF and s.remote_end
        bench.until(lambda: bench.p4.pipe.peer_flags() == inner.CONSOLE_WANTED, 2_000, "RELAY_ON 0")
        assert bench.p4.ready, "the link stays up with the relay off"
    finally:
        echo.close()
        if bench is not None:
            bench.close()


def test_relay_dns_target_through_the_resolver():
    echo = EchoServer()
    bench: Bench | None = None
    try:
        names: list[str] = []

        def resolver(host: str, port: int):
            names.append(host)
            return [(socket.AF_INET, ("127.0.0.1", port))]
        bench = Bench(settings=Settings(relay_on=True), realtime=realtime,
                      relay=TcpRelay(resolver=resolver))
        bench.up()
        stream = bench.p4.open_relay(3, b"relay.example.test", echo.port, bench.now)
        bench.until(lambda: bench.p4.pipe.find(stream).state is StState.OPEN, 3_000, "OPEN_OK")
        assert names == ["relay.example.test"]
    finally:
        echo.close()
        if bench is not None:
            bench.close()


# ---- secrets, masking and command sources --------------------------------------------------------

def test_secrets_never_reach_the_screen_or_the_log():
    with tempfile.TemporaryDirectory() as folder:
        log = Path(folder) / "peer.log"
        bench = Bench(code=None)
        bench.out = Output(log, stream=bench.screen)
        bench.peer.out = bench.out
        bench.give_secret(paircode_display(CODE))
        bench.console_up()
        bench.peer.inbox.put(_login("morgan", "hunter2-Secret"))
        bench.until(lambda: "P4> OK: logged in" in bench.text(), what="login reply")
        bench.command(f"cmd phone secret {CODE}")
        bench.command("cmd echo login morgan hunter2-Secret")
        bench.step(500)
        bench.out.close()
        logged = log.read_text(encoding="utf-8")
        for text in (bench.text(), logged):
            for secret in ("hunter2-Secret", CODE, paircode_display(CODE), CODE.lower()):
                assert secret not in text, f"{secret!r} leaked"
        assert bench.p4.console.commands[0] == "login morgan hunter2-Secret", "the P4 still gets the real line"
        assert "> login (hidden)" in logged


def _login(user: str, password: str):
    from hw1peer.app import Request
    return Request(RequestKind.LOGIN, text=f"login {user} {password}")


def paircode_display(code: str) -> str:
    from hw1peer import paircode
    return paircode.display(code)


def test_command_file_cannot_carry_secrets():
    bench = Bench()
    bench.console_up()
    bench.command("code", source="cmd.in")
    bench.command("cmd login morgan pw", source="cmd.in")
    bench.command("login", source="cmd.in")
    bench.step(200)
    assert bench.p4.console.commands == []
    assert bench.text().count("only from the keyboard") == 3
    bench.command("cmd status", source="cmd.in")
    bench.until(lambda: "P4> OK: phone link up" in bench.text(), what="cmd.in command")
    assert "[cmd.in] > cmd status" in bench.text()


def test_mask_and_redact():
    assert mask("login morgan hunter2") == "login *** ***"
    assert mask("> cmd LOGIN bob pw now") == "> cmd LOGIN *** *** now"
    assert mask("the code is 0123-4567-89AB-CDEF") == "the code is 0123-4567-89AB-CDEF"  # not a valid code
    assert mask("relay: stream 2 connected") == "relay: stream 2 connected"  # 16 symbols, valid check, prose
    assert mask("help text for command 37") == "help text for command 37"
    for spelling in (CODE, CODE.lower(), paircode_display(CODE), " ".join(paircode_display(CODE).split("-"))):
        assert mask(f"x {spelling} y") == "x ****-****-****-**** y", spelling
    assert mask("ip 192.168.1.20 mac aa:bb:cc:dd:ee:ff v6 fe80::1 at 12:30:45", for_file=True) == \
        "ip *** mac *** v6 *** at 12:30:45"
    assert mask('{"ssid":"HomeNet","rssi":-40}', for_file=True) == '{"ssid":***,"rssi":-40}'
    assert mask("login [user]  sign in", login=False) == "login [user]  sign in"
    assert redact_command("phone secret Abc!12345678") == "phone secret ***"
    assert redact_command("wifiadd HomeNet pass1234") == "wifiadd ***"
    assert redact_command("useradd bob pw 0 user") == "useradd bob *** 0 user"
    assert redact_command("llmhttpurl https://10.0.0.2:8443/v1") == "llmhttpurl ***"
    assert redact_command("status") == "status"
    assert redact_command("cmd phone secret Abc!12345678 more") == "cmd phone secret ***"
    assert redact_command("cmd login morgan pw") == "cmd login ***"
    assert redact_command("code Synthetic!Pass123") == "code ***"
    assert redact_command("CODE 0123 4567") == "CODE ***"


def test_credentials_from_entry():
    creds = credentials_from_entry(paircode_display(CODE).lower())
    assert isinstance(creds, Credentials) and creds.is_code
    from hw1peer import paircode
    assert bytes(creds.psk) == paircode.psk(CODE.encode())
    phrase = credentials_from_entry("SyntheticPipe!123")
    assert isinstance(phrase, Credentials) and not phrase.is_code
    refused = credentials_from_entry("short")
    assert isinstance(refused, str) and "short" not in refused.lower().replace("fewer", "")
    creds.wipe()
    assert not any(creds.psk)


def test_repl_code_and_login_use_hidden_reads():
    inbox, screen = Inbox(), io.StringIO()
    out = Output(None, stream=screen)
    prompts: list[str] = []

    def secret(prompt: str) -> str:
        prompts.append(prompt)
        return CODE if "code" in prompt.lower() else "pw-hidden"
    lines = iter(["code", "login morgan", "status"])

    def line(prompt: str) -> str:
        try:
            return next(lines)
        except StopIteration:
            raise EOFError from None
    repl(inbox, out, read_line=line, read_secret=secret)
    got = inbox.drain()
    assert [r.kind for r in got] == [RequestKind.SECRET, RequestKind.LOGIN, RequestKind.LINE, RequestKind.QUIT]
    assert got[0].creds is not None and got[0].creds.is_code
    assert got[1].text == "login morgan pw-hidden"
    assert len(prompts) == 2 and "hidden" in prompts[0]
    assert CODE not in screen.getvalue() and "pw-hidden" not in screen.getvalue()
    # Refusals never echo the entry.
    read_code(inbox, out, lambda _p: "tiny")
    assert "tiny" not in screen.getvalue()
    read_login("", inbox, out, lambda _p: "two words", lambda _p: "pw")
    assert "Not sent: the user and the password must be non-empty" in screen.getvalue()


def test_command_file_tail():
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "cmd.in"
        path.write_text("old line\n", encoding="utf-8")
        tail = CommandFileTail(path)
        assert tail.poll() == []  # old content is never replayed
        with path.open("a", encoding="utf-8") as handle:
            handle.write("status\nrelay o")
        assert tail.poll() == ["status"]
        with path.open("a", encoding="utf-8") as handle:
            handle.write("n\r\n")
        assert tail.poll() == ["relay on"]
        path.write_text("stats\n", encoding="utf-8")  # truncated and rewritten
        assert tail.poll() == ["stats"]


def test_status_and_stats_carry_no_identifiers():
    bench = Bench(settings=Settings(relay_on=True))
    bench.console_up()
    status, stats = bench.peer.status_text(), bench.peer.stats_text()
    for text in (status, stats):
        assert "c1" not in text.split() and CODE not in text
    assert "secret: pairing code (console allowed)" in status
    assert "console: open (stream 1); fw 0.99.98.4-model, model P4X-EYE" in status
    assert "session: ready" in status


def test_open_data_sanity_for_the_fixture_keys():
    """The fixture keys open X1's first P4 DATA (guards the replay helper above)."""
    x1 = load_fixture(X1)
    frame = x1["sessions"][0]["frames"][5]
    opened = open_data(unhex(x1["p4_to_phone_key_hex"]), DIR_D2P, unhex(frame["value_hex"]))
    assert opened is not None and opened[1] == unhex(frame["inner_hex"])
    assert DIR_P2D == 0 and Op.HELLO == 1


MAIN_DRIVER = textwrap.dedent("""
    import os, sys, time, types
    from hw1peer.tests.fake_gatt import FakePeripheral
    FAULT = os.environ.get("HW1_FAULT", "")

    class MacPeripheral(FakePeripheral):
        def __init__(self, *, name=None, on_error=print):
            super().__init__()

        def start(self, events):
            if FAULT == "start":
                raise RuntimeError("start boom")
            super().start(events)

        def close(self):
            super().close()
            if FAULT == "close":
                raise RuntimeError("close boom")

    def run_main_loop(tick, should_stop, period_s=0.010):
        print("main loop running", flush=True)
        started = time.monotonic()
        while not should_stop():
            tick()
            if FAULT == "close" and time.monotonic() - started > 0.5:
                raise RuntimeError("loop boom")
            time.sleep(period_s)

    def hold_awake(reason):
        print(f"awake begin: {reason}", flush=True)
        return lambda: print("awake end", flush=True)

    fake = types.ModuleType("hw1peer.gatt_macos")
    fake.MacPeripheral, fake.run_main_loop, fake.hold_awake = MacPeripheral, run_main_loop, hold_awake
    sys.modules["hw1peer.gatt_macos"] = fake
    from hw1peer.app import main
    sys.exit(main(sys.argv[1:]))
""")


def start_main(folder: str, *extra: str, stdin: int = subprocess.PIPE, env: dict | None = None) -> subprocess.Popen:
    """main() in a subprocess with a fake radio, its files in ``folder``."""
    paths = {name: str(Path(folder) / name) for name in ("peer.log", "cmd.in", "hw1peer.lock")}
    return subprocess.Popen([sys.executable, "-c", MAIN_DRIVER, "--log", paths["peer.log"], "--cmd-file",
                             paths["cmd.in"], "--lock-file", paths["hw1peer.lock"], *extra],
                            stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            env={**__import__("os").environ, **(env or {})})


def wait_for_file(path: Path, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while not path.exists():
        assert time.monotonic() < deadline, f"{path.name} was never created"
        time.sleep(0.05)


def test_main_wiring_with_a_fake_radio():
    """main(): the REPL on stdin, the cmd.in tail, peer.log and a clean quit (CoreBluetooth replaced)."""
    with tempfile.TemporaryDirectory() as folder:
        log, cmd = Path(folder) / "peer.log", Path(folder) / "cmd.in"
        proc = start_main(folder)
        try:
            assert proc.stdin is not None
            proc.stdin.write("status\n")
            proc.stdin.flush()
            wait_for_file(cmd)
            time.sleep(0.3)
            with cmd.open("a", encoding="utf-8") as handle:
                handle.write("stats\ncode\n")
            time.sleep(0.8)
            proc.stdin.write("quit\n")
            proc.stdin.flush()
            out, err = proc.communicate(timeout=20)
        finally:
            if proc.poll() is None:
                proc.kill()
        assert proc.returncode == 0, err[-2000:]
        logged = log.read_text(encoding="utf-8")
        assert "secret: none: type code" in out and "> status" in logged, "the REPL's command ran (echo logged only)"
        assert "[cmd.in] > stats" in out and "links 0;" in out, "the cmd.in command ran"
        assert "code is accepted only from the keyboard" in out
        assert "stopped advertising and removed the pipe service" in out
        assert all(line[:4].isdigit() for line in logged.splitlines() if line), "every log line is timestamped"
        assert "awake begin: HardwareOne phone link test" in out and out.index("awake end") > out.index(
            "stopped advertising"), "the keep-awake activity spans the run"
        assert log.stat().st_mode & 0o777 == 0o600


# ---- review fixes: P4 pressure (the model's pressure budget, BLE_PhonePipeCore.h) ---------------------

def _console_info(bench: Bench) -> inner.StreamInfo:
    return next(s for s in bench.peer.session().streams() if s.stream == bench.peer.console.stream)


def test_console_display_busy_holds_the_reply_with_no_time_cap():
    """consoleDisplayBusy defers the P4's console DATA and CREDIT for as long as it lasts (no 2 s cap,
    unlike a relay): the reply to `cmd help` waits 12 s in the P4, the link stays up, then all of it
    arrives in order."""
    bench = Bench()
    bench.console_up()
    bench.step(300)
    before = _console_info(bench)
    bench.p4.console_display_busy = True
    bench.command("cmd help")
    bench.step(12_000)
    held = _console_info(bench)
    assert bench.p4.console.commands == ["help"], "our COMMAND still reaches the P4"
    assert held.received == before.received and "P4> line 0" not in bench.text(), "console DATA held"
    assert held.peer_limit == before.peer_limit, "the P4's CREDIT for our COMMAND is held too"
    assert bench.p4.pipe.find(bench.peer.console.stream).tx, "the reply waits in the P4's buffer"
    assert bench.p4.ready and bench.p4.ends == [] and "link ended" not in bench.text()
    bench.p4.console_display_busy = False
    bench.until(lambda: "P4> OK" in bench.lines(), 5_000, "the reply once the display is free")
    printed = [line for line in bench.lines() if line.startswith("P4> line ")]
    assert printed == [f"P4> line {i}: help text for command {i}" for i in range(60)]
    bench.until(lambda: _console_info(bench).peer_limit > before.peer_limit, 1_000, "the held CREDIT")


def test_relay_pressure_defers_only_within_its_2s_budget():
    """relayPressure defers a relay stream's DATA within one cumulative 2 s budget, then it flows
    although the pressure lasts; the phone's own DATA to the P4 is never held."""
    echo = EchoServer()
    bench: Bench | None = None
    try:
        bench = Bench(settings=Settings(relay_on=True), realtime=realtime)
        bench.up()
        stream = bench.p4.open_relay(1, bytes([127, 0, 0, 1]), echo.port, bench.now)
        bench.until(lambda: bench.p4.pipe.find(stream).state is StState.OPEN, 3_000, "OPEN_OK")
        bench.p4.relay_pressure = True
        start = bench.now
        payload = bytes(range(200)) * 3
        assert bench.p4.relay_write(stream, payload) == len(payload)
        bench.step(1_500)
        assert bytes(echo.received) == b"", "relay DATA held within the budget"
        bench.until(lambda: len(echo.received) == len(payload), 3_000, "relay DATA after the 2 s budget")
        assert 2_000 <= bench.now - start <= 2_200 and bench.p4.relay_pressure
        echoed = bytearray()
        bench.until(lambda: (echoed.extend(bench.p4.relay_read(stream)), len(echoed) == len(payload))[1],
                    3_000, "the echo back to the P4")
        assert bytes(echoed) == payload and bench.p4.ready
    finally:
        echo.close()
        if bench is not None:
            bench.close()


# ---- review fixes: a local end kicks the P4 (Service Changed) ----------------------------------------

def test_a_local_end_kicks_the_p4_with_service_changed():
    """A P4 value that fails authentication ends our link with nothing able to tell the P4: the
    app removes and re-adds the service, the P4 sees Service Changed and stops at once (not ~65 s
    later on its idle PING), and the next connection works."""
    bench = Bench()
    bench.console_up()
    bench.fake.write("c1", bytes([0x10]) + (10_000).to_bytes(8, "big") + bytes(32))  # a bad tag
    assert "link ended: secure channel failed" in bench.text()
    took = bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, 100, "the P4 stops on Service Changed")
    assert took <= 20 and bench.p4.service_changes == 1 and bench.fake.service_resets == 1
    assert bench.p4.ends[-1].reason == Error.BAD_PROTOCOL
    assert "re-added the pipe service so the P4 sees Service Changed" in bench.text()
    bench.until(lambda: bench.fake.advertising, 100, "advertising again")
    bench.console_up()
    assert bench.p4.ready_sessions == 2


def test_normal_ends_never_touch_the_service():
    """Ends the P4 hears about (its RESET, our RESET 18, its disconnect, a version strike) never kick."""
    bench = Bench()
    bench.up()
    bench.command("end")
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="RESET 18")
    bench.until(lambda: bench.fake.advertising, 7_000, "advertising again")
    bench.up()
    bench.p4.stop(bench.now, Error.CONNECT_FAILED)
    bench.until(lambda: bench.fake.advertising, 100, "advertising again")
    bench.up()
    assert bench.p4.pipe._queue(Op.RESET, 0, b"\x12\x00")  # the P4 ends the session (RESET 18)
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED or bench.peer.link is None
                or bench.peer.link.phase is Phase.ENDED, what="the P4's RESET")
    bench.step(7_000)
    assert bench.fake.service_resets == 0 and bench.peer.kicks == 0


def test_handshake_ends_do_not_kick():
    """A REJECT (the P4 is already retiring) and our handshake deadline (after the P4's own 5 s cap)
    leave the service alone."""
    bench = Bench(code=CODE, p4_code=OTHER_CODE)
    bench.connect()
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="REJECT")
    bench.fake.connect("c9", 185, lambda value: None)  # a central that never answers HELLO
    bench.until(lambda: bench.fake.advertising, 7_000, "advertising again")
    bench.fake.subscribe("c9")
    bench.step(HANDSHAKE_DEADLINE_MS + 20)
    assert "link ended: the secure handshake timed out" in bench.text()
    assert bench.fake.service_resets == 0 and bench.peer.kicks == 0


# ---- review fixes: forget, service retries -------------------------------------------------------------

def test_forget_during_the_handshake_never_reaches_ready():
    bench = Bench()
    bench.connect()
    bench.step(5)  # HELLO out, the P4's HELLO_ACK not yet answered
    assert bench.peer.link is not None and bench.peer.link.phase is Phase.HANDSHAKE
    bench.command("forget")
    assert "link ended: secret forgotten" in bench.text()
    bench.step(7_000)
    assert bench.p4.ready_sessions == 0 and not bench.p4.ready
    assert bench.peer.creds is None and not bench.fake.advertising


def test_forget_while_awaiting_the_pipe_hello_ends_and_kicks():
    bench = Bench()
    bench.connect()
    bench.until(lambda: bench.peer.link.phase is Phase.PIPE, what="secure channel up")
    assert not bench.peer.link.session.ready
    bench.command("forget")
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, 100, "the P4 stops")
    assert bench.p4.ready_sessions == 0 and bench.p4.service_changes == 1
    assert bench.peer.session() is None and bench.peer.creds is None


def test_forget_with_a_ready_session_sends_reset_18():
    bench = Bench()
    bench.up()
    bench.command("forget")
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="RESET 18")
    assert bench.p4.ends[-1].reason == Error.PAUSED and bench.p4.ends[-1].peer_reset
    assert bench.fake.service_resets == 0


def test_a_refused_service_add_is_retried_then_advertises():
    bench = Bench(add_failures=2)
    assert not bench.peer.service_ready and not bench.fake.advertising
    assert "the pipe service was refused: Unknown error. (retrying in 5 s)" in bench.text()
    assert "service refused (Unknown error.)" in bench.peer.status_text()
    bench.step(SERVICE_RETRY_MS + 20)
    assert bench.fake.service_adds == 2 and "retry 1 of 5" in bench.text() and not bench.fake.advertising
    bench.step(SERVICE_RETRY_MS + 20)
    assert bench.fake.service_adds == 3 and bench.peer.service_ready and bench.fake.advertising
    bench.up()


def test_service_retries_stop_at_the_cap_and_say_what_to_do():
    bench = Bench(add_failures=1_000)
    bench.step((SERVICE_RETRY_LIMIT + 2) * (SERVICE_RETRY_MS + 20))
    assert bench.fake.service_adds == 1 + SERVICE_RETRY_LIMIT
    assert "toggle Bluetooth off and on, or restart the tool" in bench.text()
    bench.fake.add_failures = 0
    bench.fake.radio(RadioState.POWERED_OFF)
    bench.fake.radio(RadioState.POWERED_ON)
    bench.step(20)
    assert bench.peer.service_ready and bench.fake.advertising


# ---- review fixes: secrets typed in the wrong place, tool text, the log file ---------------------------

PHRASE = "Synthetic!Pass123"


def test_a_secret_typed_in_the_wrong_place_is_never_shown_or_logged():
    with tempfile.TemporaryDirectory() as folder:
        log = Path(folder) / "peer.log"
        bench = Bench(log=log)
        bench.command(f"code {PHRASE}", source="cmd.in")      # refused, echoed masked
        bench.command(PHRASE, source="cmd.in")                # a bare secret: unknown verb
        bench.command(PHRASE)                                 # the same at the keyboard
        bench.command(f"{PHRASE} more words")
        bench.out.close()
        logged = log.read_text(encoding="utf-8")
        for text in (bench.text(), logged):
            assert PHRASE not in text and PHRASE.lower() not in text
        assert "[cmd.in] > code ***" in bench.text()
        assert bench.text().count("unknown command") == 3 and "unknown command from cmd.in (not shown" in bench.text()


def test_split_verb_and_tab_separated_commands():
    assert split_verb("  Relay\ton ") == ("relay", "on") and split_verb("") == ("", "") and split_verb("ping") == ("ping", "")
    bench = Bench()
    bench.command("relay\ton")
    assert "relay on (from the next session)" in bench.text() and bench.peer.settings.relay_on


def test_tool_text_is_not_mangled_by_the_login_mask():
    bench = Bench()
    bench.command("login", source="cmd.in")
    bench.command("login bob pw", source="cmd.in")
    bench.command("cmd status", source="cmd.in")
    text = bench.text()
    assert "login is accepted only from the keyboard: secrets never go through files" in text
    assert "[cmd.in] > login ***" in text and "*** only from" not in text and "***   ***" not in text
    assert "[cmd.in] > cmd status" in text and "pw" not in text.split()


WIFI_ROWS = ("3 networks found:\n  1) 'Home Net 5G'  RSSI=-40  BSSID=aa:bb:cc:dd:ee:ff\n"
             "  2) 'CafeGuest'  RSSI=-71  BSSID=11:22:33:44:55:66\n"
             "Connecting to 'Home Net 5G' (priority 1) ...\nSaved network 'CafeGuest' with priority 2\n"
             "  1. [2] 'CafeGuest'\n  SSID: Home Net 5G\nIP 192.168.1.20 gateway 10.0.0.1\nOK")


def test_the_log_masks_wifi_ssids_keeps_versions_and_is_private():
    with tempfile.TemporaryDirectory() as folder:
        log = Path(folder) / "peer.log"
        log.write_text("", encoding="utf-8")
        log.chmod(0o644)  # an older log: tightened on open
        bench = Bench(log=log, responder=lambda line: WIFI_ROWS if line == "wifiscan" else "OK")
        assert log.stat().st_mode & 0o777 == 0o600
        bench.console_up()
        bench.command("cmd wifiscan")
        bench.until(lambda: "P4> OK" in bench.lines(), what="wifiscan reply")
        bench.command("status")
        bench.out.close()
        logged = log.read_text(encoding="utf-8")
        for secret in ("Home Net 5G", "CafeGuest", "aa:bb:cc:dd:ee:ff", "192.168.1.20", "10.0.0.1"):
            assert secret not in logged, secret
        assert "RSSI=-40" in logged and "1) '***'" in logged
        assert '"fw":"0.99.98.4-model"' in logged, "the P4's INFO keeps its firmware version"
        assert "fw 0.99.98.4-model, model P4X-EYE" in logged
        assert "Home Net 5G" in bench.text(), "the screen is not file-masked"
    fresh = Path(tempfile.mkdtemp()) / "new.log"
    Output(fresh).close()
    assert fresh.stat().st_mode & 0o777 == 0o600
    fresh.unlink()
    fresh.parent.rmdir()


# ---- review fixes: prompts, Ctrl-C, inline secrets --------------------------------------------------------

def test_ctrl_c_at_a_hidden_prompt_only_warns_then_quits():
    inbox, prompts = Inbox(), PromptGuard()
    quitting = [False]
    clock = [100.0]
    handler = Interrupts(inbox, lambda: quitting[0], prompts, clock=lambda: clock[0])
    handler.handle(signal.SIGINT)
    assert [r.kind for r in inbox.drain()] == [RequestKind.QUIT], "no prompt: Ctrl-C quits"
    with prompts.prompt():
        handler.handle(signal.SIGINT)
        got = inbox.drain()
        assert [r.kind for r in got] == [RequestKind.NOTICE] and got[0].text == PROMPT_HINT
        handler.handle(signal.SIGINT)
        assert [r.kind for r in inbox.drain()] == [RequestKind.QUIT], "a second Ctrl-C quits"
    with prompts.prompt():  # a new prompt warns again
        handler.handle(signal.SIGINT)
        assert [r.kind for r in inbox.drain()] == [RequestKind.NOTICE]
    handler.handle(signal.SIGTERM)
    assert [(r.kind, r.source) for r in inbox.drain()] == [(RequestKind.QUIT, "SIGTERM")]
    assert not handler.should_stop()
    handler.handle(signal.SIGHUP)
    assert [r.kind for r in inbox.drain()] == [RequestKind.QUIT] and not handler.should_stop()
    clock[0] += 3.1
    assert handler.should_stop(), "a hang-up stops at once if the clean quit takes too long"
    quitting[0] = True
    handler2 = Interrupts(inbox, lambda: True, prompts)
    handler2.handle(signal.SIGINT)
    assert handler2.force.is_set() and handler2.should_stop() and inbox.drain() == []


def test_the_notice_prints_on_the_engine_tick():
    bench = Bench()
    bench.peer.inbox.put(Request(RequestKind.NOTICE, text=PROMPT_HINT))
    bench.step(10)
    assert PROMPT_HINT in bench.text()


def test_repl_marks_the_prompt_open_and_ctrl_d_cancels_it():
    inbox, screen, prompts = Inbox(), io.StringIO(), PromptGuard()
    out = Output(None, stream=screen)
    seen: list[bool] = []
    answers = iter([EOFError, CODE])

    def secret(_prompt: str) -> str:
        seen.append(prompts.is_open)
        answer = next(answers)
        if answer is EOFError:
            raise EOFError
        return answer
    lines = iter(["code", "code", "status"])

    def line(_prompt: str) -> str:
        try:
            return next(lines)
        except StopIteration:
            raise EOFError from None
    repl(inbox, out, read_line=line, read_secret=secret, prompts=prompts)
    assert seen == [True, True] and not prompts.is_open
    assert "cancelled" in screen.getvalue()
    assert [r.kind for r in inbox.drain()] == [RequestKind.SECRET, RequestKind.LINE, RequestKind.QUIT]


def test_inline_login_and_code_secrets_are_refused_and_never_shown():
    inbox, screen = Inbox(), io.StringIO()
    out = Output(None, stream=screen)
    lines = iter(["login bob pw-inline", "code ZZZ-inline-secret", "LOGIN bob"])
    secrets = iter([CODE, "pw-hidden"])

    def line(_prompt: str) -> str:
        try:
            return next(lines)
        except StopIteration:
            raise EOFError from None
    repl(inbox, out, read_line=line, read_secret=lambda _p: next(secrets))
    got = inbox.drain()
    assert [r.kind for r in got] == [RequestKind.SECRET, RequestKind.LOGIN, RequestKind.QUIT]
    assert got[1].text == "login bob pw-hidden", "only the hidden password is sent"
    text = screen.getvalue()
    assert LOGIN_INLINE in text and CODE_INLINE in text
    assert "pw-inline" not in text and "ZZZ-inline-secret" not in text


# ---- review fixes: main (one instance, signals, shutdown, --no-repl) ------------------------------------

def test_one_instance_at_a_time():
    with tempfile.TemporaryDirectory() as folder:
        lock, log = Path(folder) / "hw1peer.lock", Path(folder) / "peer.log"
        held = acquire_instance_lock(lock)
        assert held is not None and acquire_instance_lock(lock) is None
        screen = io.StringIO()
        with contextlib.redirect_stdout(screen):
            code = main(["--lock-file", str(lock), "--log", str(log), "--cmd-file", str(Path(folder) / "cmd.in")])
        assert code == 2 and "another hw1peer is running" in screen.getvalue()
        assert "another hw1peer is running" in log.read_text(encoding="utf-8")
        held.close()
        again = acquire_instance_lock(lock)
        assert again is not None
        again.close()


def _run_until_loop(proc: subprocess.Popen) -> None:
    assert proc.stdout is not None
    deadline = time.monotonic() + 10
    while "main loop running" not in proc.stdout.readline():
        assert time.monotonic() < deadline and proc.poll() is None, "main never reached its loop"


def test_sigterm_and_sighup_quit_cleanly():
    for signum in (signal.SIGTERM, signal.SIGHUP):
        with tempfile.TemporaryDirectory() as folder:
            proc = start_main(folder)
            try:
                _run_until_loop(proc)
                time.sleep(0.2)
                proc.send_signal(signum)
                out, err = proc.communicate(timeout=20)
            finally:
                if proc.poll() is None:
                    proc.kill()
            assert proc.returncode == 0, (signum, proc.returncode, err[-2000:])
            assert "quitting" in out and "stopped advertising and removed the pipe service" in out, signum


def test_shutdown_steps_survive_a_failing_close_and_keep_the_original_error():
    with tempfile.TemporaryDirectory() as folder:
        proc = start_main(folder, "--no-repl", stdin=subprocess.DEVNULL, env={"HW1_FAULT": "close"})
        out, err = proc.communicate(timeout=30)
        logged = (Path(folder) / "peer.log").read_text(encoding="utf-8")
        assert proc.returncode != 0 and "RuntimeError: loop boom" in err, err[-2000:]
        assert "shutdown: closing the peer failed" in logged and "close boom" in logged
        assert "awake end" in out, "later shutdown steps still ran"


def test_a_failing_start_still_shuts_down():
    with tempfile.TemporaryDirectory() as folder:
        proc = start_main(folder, env={"HW1_FAULT": "start"})
        out, err = proc.communicate(timeout=30)
        assert proc.returncode != 0 and "RuntimeError: start boom" in err, err[-2000:]
        assert "stopped advertising and removed the pipe service" in out and "awake end" in out


def test_no_repl_runs_from_the_command_file_alone():
    with tempfile.TemporaryDirectory() as folder:
        cmd = Path(folder) / "cmd.in"
        proc = start_main(folder, "--no-repl", stdin=subprocess.DEVNULL)
        try:
            wait_for_file(cmd)
            time.sleep(1.0)
            assert proc.poll() is None, "stdin at EOF must not quit with --no-repl"
            with cmd.open("a", encoding="utf-8") as handle:
                handle.write("status\nquit\n")
            out, err = proc.communicate(timeout=20)
        finally:
            if proc.poll() is None:
                proc.kill()
        assert proc.returncode == 0, err[-2000:]
        assert "[cmd.in] > status" in out and "secret: none" in out and "stopped advertising" in out
