"""channel: the phone side of HW1-PIPE-v1 against the firmware's fixtures.

* the handshake golden (v2 file, and the frozen v1 file for the handshake and
  the outer DATA framing) replayed byte for byte from the phone side;
* every X1 v2 session's outer layer: the phone's HELLO/CONFIRM/DATA equal the
  recorded values, every P4 value opens to the recorded inner bytes;
* X2 v2: the phone's sealed DATA under the vectors' counters (gaps, 2^63,
  2^64-1), the wrong-passphrase and wrong-domain CONFIRMs, and the P4-to-phone
  outer failures (tampered CONFIRM_ACK, REJECT, zero key, replayed DATA);
* the failure, counter and exhaustion rules, and random sessions against the
  Python mirror of the P4 responder.
"""

import os
import random

from hw1peer import channel, paircode
from hw1peer.channel import (U64_MAX, Data, DeviceChannel, Established, Failure, Invalid, PhoneChannel, Rejected,
                             Reply, State)
from hw1peer.tests import load_fixture, unhex

GOLDEN_V2 = "phone_secure_channel_v1_pipe_v2_vectors.json"
GOLDEN_V1 = "phone_secure_channel_v1_vectors.json"
X1 = "phone_pipe_v2_sessions.json"
X2 = "phone_pipe_v2_negative.json"
# test_phone_secure_channel.py: the outer channel's fields, equal in both goldens.
HANDSHAKE_FIELDS = ("phone_private_key_hex", "phone_public_key_hex", "device_private_key_hex",
                    "device_public_key_hex", "phone_nonce_hex", "device_nonce_hex", "psk_hex", "shared_secret_hex",
                    "phone_to_p4_key_hex", "p4_to_phone_key_hex", "hello_hex", "hello_ack_hex", "confirm_hex",
                    "confirm_ack_hex")
# Curve25519 points of small order (libsodium's blocklist): every one must fail.
LOW_ORDER_POINTS = (
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0100000000000000000000000000000000000000000000000000000000000000",
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
    "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
    "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
)


def phone_for(fixture: dict, psk: bytes | None = None, label: str = channel.LABEL) -> PhoneChannel:
    """A deterministic phone channel with a fixture's synthetic key and nonce."""
    return PhoneChannel(psk if psk is not None else unhex(fixture["psk_hex"]),
                        private_key=unhex(fixture["phone_private_key_hex"]),
                        nonce=unhex(fixture["phone_nonce_hex"]), label=label)


def established(fixture: dict) -> PhoneChannel:
    """A phone channel taken through the golden handshake."""
    phone = phone_for(fixture)
    assert phone.hello() == unhex(fixture["hello_hex"])
    assert phone.receive(unhex(fixture["hello_ack_hex"])) == Reply(unhex(fixture["confirm_hex"]))
    assert phone.receive(unhex(fixture["confirm_ack_hex"])) == Established()
    return phone


def failed_auth(phone: PhoneChannel) -> bool:
    """FAILED with AUTH and every key buffer zeroed."""
    buffers = (phone._psk, phone._secret, phone._phone_nonce, phone._p2d, phone._d2p)
    return phone.state is State.FAILED and phone.failure is Failure.AUTH and not any(any(b) for b in buffers)


# ---- primitives and the goldens -------------------------------------------------------

def test_golden_primitives():
    for name in (GOLDEN_V2, GOLDEN_V1):
        g = load_fixture(name)
        assert g["label_ascii"] == channel.LABEL
        assert channel.public_key(unhex(g["phone_private_key_hex"])) == unhex(g["phone_public_key_hex"])
        assert channel.public_key(unhex(g["device_private_key_hex"])) == unhex(g["device_public_key_hex"])
        shared = channel.x25519(unhex(g["phone_private_key_hex"]), unhex(g["device_public_key_hex"]))
        assert shared == unhex(g["shared_secret_hex"])
        assert channel.x25519(unhex(g["device_private_key_hex"]), unhex(g["phone_public_key_hex"])) == shared
        p2d, d2p = channel.derive_keys(shared, unhex(g["psk_hex"]), unhex(g["phone_nonce_hex"]),
                                       unhex(g["device_nonce_hex"]))
        assert p2d == unhex(g["phone_to_p4_key_hex"]) and d2p == unhex(g["p4_to_phone_key_hex"])


def test_hkdf_rfc5869_case_1():
    okm = channel.hkdf_sha256(bytes([0x0B] * 22), bytes(range(13)), bytes(range(0xF0, 0xFA)), 42)
    assert okm.hex() == ("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                         "34007208d5b887185865")


def test_aead_nonce_layout():
    assert channel.aead_nonce(1, 0x0102030405060708).hex() == "000000010102030405060708"
    assert channel.aead_nonce(0, 0) == bytes(12)


def test_handshake_fields_unchanged_in_v2():
    v1, v2 = load_fixture(GOLDEN_V1), load_fixture(GOLDEN_V2)
    for key in HANDSHAKE_FIELDS:
        assert v1[key] == v2[key], key


def _replay_golden(name: str) -> None:
    g = load_fixture(name)
    phone = phone_for(g)
    assert phone.state is State.NEW
    assert phone.hello() == unhex(g["hello_hex"])
    assert phone.state is State.AWAIT_HELLO_ACK
    assert phone.receive(unhex(g["hello_ack_hex"])) == Reply(unhex(g["confirm_hex"]))
    assert phone.state is State.AWAIT_CONFIRM_ACK
    assert phone.receive(unhex(g["confirm_ack_hex"])) == Established()
    assert phone.established and g["data_counter_each_direction"] == 1
    assert phone.receive(unhex(g["p4_to_phone_data_hex"])) == Data(unhex(g["p4_to_phone_plaintext_hello_hex"]), 1)
    assert phone.seal(unhex(g["phone_to_p4_plaintext_hello_ack_hex"])) == unhex(g["phone_to_p4_data_hex"])
    assert phone.tx_counter == 2 and phone.rx_counter == 1


def test_golden_v2_phone_side():
    _replay_golden(GOLDEN_V2)


def test_golden_v1_frozen_phone_side():
    _replay_golden(GOLDEN_V1)


def test_golden_psk_from_passphrase():
    g = load_fixture(GOLDEN_V2)
    assert paircode.psk(g["passphrase_utf8"].encode("utf-8")) == unhex(g["psk_hex"])


# ---- X1 v2: every session's outer layer -------------------------------------------------

def test_x1_sessions_outer_replay():
    x1, golden = load_fixture(X1), load_fixture(GOLDEN_V2)
    for key in HANDSHAKE_FIELDS[:10]:  # the keys, nonces, PSK and derived keys (X1 has no frame fields)
        assert x1[key] == golden[key], key
    assert [s["att_mtu"] for s in x1["sessions"]] == [64, 244, 247, 517]
    for session in x1["sessions"]:
        frames = session["frames"]
        assert [f["kind"] for f in frames[:5]] == ["cccd_enable", "secure_hello", "secure_hello_ack",
                                                   "secure_confirm", "secure_confirm_ack"]
        phone = phone_for(x1)
        assert phone.hello() == unhex(frames[1]["value_hex"])
        assert phone.receive(unhex(frames[2]["value_hex"])) == Reply(unhex(frames[3]["value_hex"]))
        assert phone.receive(unhex(frames[4]["value_hex"])) == Established()
        opened = sealed = 0
        for frame in frames[5:]:
            assert frame["kind"] == "data", (session["att_mtu"], frame["i"])
            value, inner = unhex(frame["value_hex"]), unhex(frame["inner_hex"])
            assert len(value) <= session["value_limit"]
            if frame["dir"] == "p4_to_phone":
                assert phone.receive(value) == Data(inner, frame["counter"]), (session["att_mtu"], frame["i"])
                opened += 1
            else:
                assert phone.tx_counter == frame["counter"], (session["att_mtu"], frame["i"])
                assert phone.seal(inner) == value, (session["att_mtu"], frame["i"])
                sealed += 1
        assert phone.established and opened and sealed
        assert phone.rx_counter == opened and phone.tx_counter == sealed + 1


# ---- X2 v2: outer-layer vectors --------------------------------------------------------

def _session_247() -> list[dict]:
    return next(s for s in load_fixture(X1)["sessions"] if s["att_mtu"] == 247)["frames"]


def _phone_at(stage: str, direction: str) -> PhoneChannel:
    """The phone channel at an X2 vector's base point (X2 ``base``): frames 0-4
    for a P4 value replacing the pipe HELLO, 0-5 after it, 0-6 for ``pipe``."""
    last = {"pipe": 6, "pipe_hello": 5 if direction == "phone_to_p4" else 4}[stage]
    frames = _session_247()
    phone = established(load_fixture(GOLDEN_V2))  # X1 and X2 use the golden's keys and nonces
    for frame in frames[5:last + 1]:
        value, inner = unhex(frame["value_hex"]), unhex(frame["inner_hex"])
        if frame["dir"] == "p4_to_phone":
            assert phone.receive(value) == Data(inner, frame["counter"])
        else:
            assert phone.seal(inner) == value
    return phone


def test_x2_pipe_vectors_outer_layer():
    x1 = load_fixture(X1)
    p2d = unhex(x1["phone_to_p4_key_hex"])
    checked = sealed_at_max = 0
    for vector in load_fixture(X2)["vectors"]:
        if vector["stage"] not in ("pipe", "pipe_hello"):
            continue
        phone = _phone_at(vector["stage"], vector["direction"])
        for frame in vector["frames"]:
            if frame["kind"] != "data":
                continue  # a secure HELLO after READY, plaintext inner bytes: not the phone's DATA
            value = unhex(frame["value_hex"])
            where = (vector["id"], frame["i"])
            if frame["dir"] == "p4_to_phone":
                if "inner_hex" in frame:
                    assert phone.receive(value) == Data(unhex(frame["inner_hex"]), frame["counter"]), where
                else:  # replayed_p4_data: a counter not above the last accepted
                    assert isinstance(phone.receive(value), Invalid) and failed_auth(phone), where
                checked += 1
            elif "inner_hex" in frame:
                inner, counter = unhex(frame["inner_hex"]), frame["counter"]
                if counter == U64_MAX:  # the P4 accepts it once; the phone itself never sends it
                    assert channel.seal_data(p2d, channel.DIR_P2D, counter, inner) == value, where
                    sealed_at_max += 1
                else:
                    phone.set_counters_for_test(counter, phone.rx_counter)
                    assert phone.seal(inner) == value, where
                checked += 1
    assert checked >= 150 and sealed_at_max == 2


def test_x2_handshake_confirms_from_other_keys():
    x1 = load_fixture(X1)
    vectors = {v["id"]: v for v in load_fixture(X2)["vectors"]}
    wrong = phone_for(x1, psk=paircode.psk(b"WrongPipe!456"))
    domain = phone_for(x1, psk=paircode.pbkdf2_sha256(x1["passphrase_utf8"].encode("utf-8"), b"HW1-SC-v1"),
                       label="HW1-SC-v1")
    for vector_id, phone in (("wrong_passphrase", wrong), ("console_domain", domain)):
        frames = vectors[vector_id]["frames"]
        assert phone.hello() == unhex(frames[1]["value_hex"])
        assert phone.receive(unhex(frames[2]["value_hex"])) == Reply(unhex(frames[3]["value_hex"])), vector_id
        assert frames[4]["kind"] == "secure_reject"
        assert phone.receive(unhex(frames[4]["value_hex"])) == Rejected(2) and failed_auth(phone), vector_id
    # The tampered CONFIRM is the real one with its last tag bit flipped.
    tampered = unhex(vectors["tampered_confirm"]["frames"][3]["value_hex"])
    real = unhex(x1["sessions"][0]["frames"][3]["value_hex"])
    assert tampered[:-1] == real[:-1] and tampered[-1] == real[-1] ^ 1


def test_x2_p4_to_phone_handshake_failures():
    x1 = load_fixture(X1)
    hello_sent = 0
    for vector in load_fixture(X2)["vectors"]:
        if vector["stage"] != "handshake" or vector["direction"] != "p4_to_phone":
            continue
        assert vector["android_auth_cooldown"] is True
        for frame in vector["frames"]:
            value = unhex(frame["value_hex"])
            phone = phone_for(x1)
            phone.hello()
            hello_sent += 1
            if frame["kind"] == "secure_confirm_ack":
                assert isinstance(phone.receive(unhex(_session_247()[2]["value_hex"])), Reply)
            result = phone.receive(value)
            if frame["kind"] == "secure_reject":
                assert result == Rejected(value[1]), vector["id"]
            else:
                assert isinstance(result, Invalid), (vector["id"], result)
            assert failed_auth(phone), vector["id"]
    assert hello_sent == 3


def test_x2_p4_reject_after_hello_and_after_confirm():
    x1 = load_fixture(X1)
    reject = bytes([channel.MsgType.REJECT, 2])
    phone = phone_for(x1)
    phone.hello()
    assert phone.receive(unhex(_session_247()[2]["value_hex"])) == Reply(unhex(_session_247()[3]["value_hex"]))
    assert phone.receive(reject) == Rejected(2) and failed_auth(phone)
    # Once established a REJECT is just an invalid value.
    phone = established(load_fixture(GOLDEN_V2))
    assert isinstance(phone.receive(reject), Invalid) and failed_auth(phone)


# ---- rules -------------------------------------------------------------------------------

def test_low_order_device_keys_fail():
    g = load_fixture(GOLDEN_V2)
    for point in LOW_ORDER_POINTS:
        assert channel.x25519(unhex(g["phone_private_key_hex"]), unhex(point)) is None, point
        phone = phone_for(g)
        phone.hello()
        ack = bytes([channel.MsgType.HELLO_ACK]) + unhex(point) + unhex(g["device_nonce_hex"])
        assert isinstance(phone.receive(ack), Invalid) and failed_auth(phone), point


def test_bad_handshake_values_fail():
    g = load_fixture(GOLDEN_V2)
    ack, confirm_ack = unhex(g["hello_ack_hex"]), unhex(g["confirm_ack_hex"])
    for bad in (ack[:-1], ack + b"\0", bytes([0x04]) + ack[1:], b"", bytes([5]), bytes([5, 2, 0])):
        phone = phone_for(g)
        phone.hello()
        assert isinstance(phone.receive(bad), Invalid) and failed_auth(phone), bad.hex()
    for bad in (confirm_ack[:-1], confirm_ack[:-1] + bytes([confirm_ack[-1] ^ 1]), bytes([2]) + confirm_ack[1:]):
        phone = phone_for(g)
        phone.hello()
        phone.receive(ack)
        assert isinstance(phone.receive(bad), Invalid) and failed_auth(phone), bad.hex()
    # A value before hello() fails too.
    phone = phone_for(g)
    assert isinstance(phone.receive(ack), Invalid) and failed_auth(phone)


def test_hello_only_once_and_seal_needs_established():
    g = load_fixture(GOLDEN_V2)
    phone = phone_for(g)
    assert phone.seal(b"x") is None
    phone.hello()
    try:
        phone.hello()
    except RuntimeError:
        pass
    else:
        raise AssertionError("second hello() accepted")
    assert phone.seal(b"x") is None and phone.state is State.AWAIT_HELLO_ACK


def test_rx_counter_rules():
    g = load_fixture(GOLDEN_V2)
    d2p = unhex(g["p4_to_phone_key_hex"])

    def p4(counter: int, plain: bytes = b"inner") -> bytes:
        return channel.seal_data(d2p, channel.DIR_D2P, counter, plain)

    phone = established(g)
    assert phone.receive(p4(1)) == Data(b"inner", 1)
    assert phone.receive(p4(5, b"")) == Data(b"", 5)  # gaps allowed; empty payload = 25 bytes
    assert phone.receive(p4(1 << 63)) == Data(b"inner", 1 << 63)
    tampered = bytearray(p4((1 << 63) + 1))
    tampered[-1] ^= 1
    assert isinstance(phone.receive(bytes(tampered)), Invalid) and failed_auth(phone)
    assert phone.rx_counter == 1 << 63, "a counter is committed only after its tag verifies"
    assert isinstance(phone.receive(p4((1 << 63) + 2)), Invalid), "FAILED is terminal"
    assert phone.seal(b"x") is None

    for second in (5, 4):  # replay, regression
        phone = established(g)
        assert phone.receive(p4(5)) == Data(b"inner", 5)
        assert isinstance(phone.receive(p4(second)), Invalid) and failed_auth(phone), second

    phone = established(g)
    assert phone.receive(p4(U64_MAX)) == Data(b"inner", U64_MAX)
    assert isinstance(phone.receive(p4(U64_MAX)), Invalid) and failed_auth(phone)

    for bad in (p4(1)[:24], bytes([0x11]) + p4(1)[1:], p4(1, b"payload")[:-1]):
        phone = established(g)
        assert isinstance(phone.receive(bad), Invalid) and failed_auth(phone), bad.hex()


def test_tx_counter_exhaustion():
    g = load_fixture(GOLDEN_V2)
    p2d = unhex(g["phone_to_p4_key_hex"])
    phone = established(g)
    phone.set_counters_for_test(U64_MAX - 1, 0)
    assert phone.seal(b"last") == channel.seal_data(p2d, channel.DIR_P2D, U64_MAX - 1, b"last")
    assert phone.tx_counter == U64_MAX
    assert phone.seal(b"never") is None
    assert phone.state is State.FAILED and phone.failure is Failure.EXHAUSTED
    # The first cause sticks.
    assert isinstance(phone.receive(b"\x10" + bytes(24)), Invalid) and phone.failure is Failure.EXHAUSTED


def test_seal_layout():
    g = load_fixture(GOLDEN_V2)
    phone = established(g)
    for size in (0, 1, 219, 600):
        value = phone.seal(bytes(size))
        assert value is not None and len(value) == size + channel.OVERHEAD and value[0] == 0x10
    assert int.from_bytes(value[1:9], "big") == 4


def test_wipe_ends_the_channel():
    phone = established(load_fixture(GOLDEN_V2))
    phone.wipe()
    assert phone.state is State.FAILED and phone.failure is None
    assert phone.seal(b"x") is None


# ---- random sessions against the responder mirror ------------------------------------

def test_random_sessions_with_device_mirror():
    rng = random.Random(99)
    for _ in range(20):
        psk = os.urandom(32)
        phone, device = PhoneChannel(psk), DeviceChannel()
        kind, ack = device.receive(phone.hello(), psk)
        assert kind == "reply" and device.reply_submitted()
        reply = phone.receive(ack)
        assert isinstance(reply, Reply)
        kind, confirm_ack = device.receive(reply.value, psk)
        assert kind == "reply" and device.reply_submitted() and device.ready
        assert phone.receive(confirm_ack) == Established()
        for counter in range(1, 30):
            payload = rng.randbytes(rng.randrange(0, 300))
            assert device.receive(phone.seal(payload), psk) == ("data", payload)
            assert phone.receive(device.encrypt(payload)) == Data(payload, counter)


def test_wrong_psk_fails_at_the_device():
    phone, device = PhoneChannel(os.urandom(32)), DeviceChannel()
    other = os.urandom(32)
    kind, ack = device.receive(phone.hello(), other)
    assert kind == "reply" and device.reply_submitted()
    reply = phone.receive(ack)
    assert isinstance(reply, Reply)
    assert device.receive(reply.value, other) == ("error", b"") and not device.ready


def test_device_mirror_matches_golden():
    g = load_fixture(GOLDEN_V2)
    psk = unhex(g["psk_hex"])
    device = DeviceChannel(private_key=unhex(g["device_private_key_hex"]), nonce=unhex(g["device_nonce_hex"]))
    assert device.receive(unhex(g["hello_hex"]), psk) == ("reply", unhex(g["hello_ack_hex"]))
    assert device.reply_submitted()
    assert device.receive(unhex(g["confirm_hex"]), psk) == ("reply", unhex(g["confirm_ack_hex"]))
    assert device.reply_submitted() and device.ready
    assert device.encrypt(unhex(g["p4_to_phone_plaintext_hello_hex"])) == unhex(g["p4_to_phone_data_hex"])
    assert device.receive(unhex(g["phone_to_p4_data_hex"]), psk) == \
        ("data", unhex(g["phone_to_p4_plaintext_hello_ack_hex"]))
