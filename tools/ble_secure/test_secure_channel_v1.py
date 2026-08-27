#!/usr/bin/env python3
"""Offline interoperability and failure tests for Secure Channel v1."""

from __future__ import annotations

import asyncio
import json
import sys
import unittest
from argparse import Namespace
from pathlib import Path

from secure_channel_v1 import (
    AuthenticationError,
    ClientState,
    ConflictingFragmentError,
    CounterGapError,
    DIR_C2D,
    DIR_D2C,
    DuplicateFragmentError,
    FrameFormatError,
    IncompleteMessageError,
    REPLY_PAYLOAD_BYTES,
    REPLY_VERSION_BINARY,
    REPLY_VERSION_TEXT,
    ReplayError,
    ReplyReassembler,
    SecureChannelV1Client,
    SessionKeys,
    StaleSessionError,
    build_confirm,
    build_confirm_ack,
    build_data_frame,
    build_hello,
    build_hello_ack,
    build_nonce,
    build_reply_fragment,
    decrypt_data_frame,
    derive_psk,
    derive_session_keys,
    fragment_reply,
    hkdf_sha256,
    parse_reply_fragment,
    x25519_public_key,
    x25519_shared_secret,
)

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from ble_secure_event_catalog_client import (  # noqa: E402
    CatalogValidationError,
    CompanionError,
    FIXTURE_PATH as CATALOG_FIXTURE_PATH,
    TransportError,
    _run_connected_physical,
    _wait_for_catalog,
    _wait_for_login,
    build_login_command,
    classify_catalog_reply,
    classify_login_reply,
    validate_live_catalog,
)


FIXTURE_PATH = (
    Path(__file__).resolve().parent
    / "fixtures"
    / "secure_channel_v1_vectors.json"
)


def unhex(value: str) -> bytes:
    return bytes.fromhex(value)


class VectorFixture:
    @classmethod
    def setUpClass(cls) -> None:
        cls.vector = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
        cls.handshake = cls.vector["handshake"]
        cls.derived = cls.vector["derived"]
        cls.app_private = unhex(cls.handshake["app_private_key_hex"])
        cls.device_private = unhex(cls.handshake["device_private_key_hex"])
        cls.app_nonce = unhex(cls.handshake["app_nonce_hex"])
        cls.device_nonce = unhex(cls.handshake["device_nonce_hex"])
        cls.device_public = unhex(cls.handshake["device_public_key_hex"])
        cls.psk = unhex(cls.derived["psk_hex"])
        cls.keys = SessionKeys(
            unhex(cls.derived["k_c2d_hex"]), unhex(cls.derived["k_d2c_hex"])
        )

    def new_established_client(self, **kwargs) -> tuple[SecureChannelV1Client, int]:
        client = SecureChannelV1Client(
            self.handshake["passphrase_utf8"], **kwargs
        )
        generation, hello = client.begin_handshake(
            app_private_key=self.app_private, app_nonce=self.app_nonce
        )
        self.assertEqual(hello.hex(), self.handshake["hello_hex"])
        confirm = client.receive_hello_ack(
            unhex(self.handshake["hello_ack_hex"]), generation
        )
        self.assertEqual(confirm.hex(), self.vector["confirmation"]["confirm_hex"])
        client.receive_confirm_ack(
            unhex(self.vector["confirmation"]["confirm_ack_hex"]), generation
        )
        self.assertEqual(client.state, ClientState.ESTABLISHED)
        return client, generation


class VectorTests(VectorFixture, unittest.TestCase):
    def test_fixture_is_explicitly_synthetic(self) -> None:
        self.assertEqual(self.vector["schema"], 1)
        self.assertIn("Synthetic offline", self.vector["warning"])
        self.assertEqual(self.vector["protocol"]["label_ascii"], "HW1-SC-v1")
        self.assertEqual(self.vector["protocol"]["pbkdf2_iterations"], 100_000)
        self.assertEqual(self.vector["protocol"]["reply_payload_bytes"], 195)

    def test_pbkdf2_hkdf_and_x25519_vectors(self) -> None:
        self.assertEqual(
            derive_psk(self.handshake["passphrase_utf8"]), self.psk
        )
        self.assertEqual(
            x25519_public_key(self.app_private).hex(),
            self.handshake["app_public_key_hex"],
        )
        self.assertEqual(
            x25519_public_key(self.device_private), self.device_public
        )
        shared_app = x25519_shared_secret(self.app_private, self.device_public)
        shared_device = x25519_shared_secret(
            self.device_private, unhex(self.handshake["app_public_key_hex"])
        )
        self.assertEqual(shared_app, shared_device)
        self.assertEqual(shared_app.hex(), self.derived["shared_secret_hex"])
        self.assertEqual((shared_app + self.psk).hex(), self.derived["hkdf_ikm_hex"])
        self.assertEqual(
            (self.app_nonce + self.device_nonce).hex(),
            self.derived["hkdf_salt_hex"],
        )
        expanded = hkdf_sha256(
            shared_app + self.psk,
            self.app_nonce + self.device_nonce,
            b"HW1-SC-v1",
            64,
        )
        self.assertEqual(expanded, self.keys.c2d + self.keys.d2c)
        self.assertEqual(
            derive_session_keys(
                self.app_private,
                self.device_public,
                self.app_nonce,
                self.device_nonce,
                self.psk,
            ),
            self.keys,
        )

    def test_handshake_and_directional_nonce_vectors(self) -> None:
        self.assertEqual(
            build_hello(self.app_private, self.app_nonce).hex(),
            self.handshake["hello_hex"],
        )
        self.assertEqual(
            build_hello_ack(self.device_private, self.device_nonce).hex(),
            self.handshake["hello_ack_hex"],
        )
        self.assertEqual(
            build_confirm(self.keys).hex(), self.vector["confirmation"]["confirm_hex"]
        )
        self.assertEqual(
            build_confirm_ack(self.keys).hex(),
            self.vector["confirmation"]["confirm_ack_hex"],
        )
        self.assertEqual(build_nonce(DIR_C2D, 0), bytes(12))
        self.assertEqual(
            build_nonce(DIR_D2C, 0x0102030405060708).hex(),
            "000000010102030405060708",
        )

    def test_c2d_data_vector_and_firmware_monotonic_gap_compatibility(self) -> None:
        entry = self.vector["c2d_data"]
        plaintext = entry["plaintext_utf8"].encode("utf-8")
        frame = build_data_frame(self.keys.c2d, DIR_C2D, entry["counter"], plaintext)
        self.assertEqual(frame.hex(), entry["frame_hex"])
        self.assertEqual(
            decrypt_data_frame(self.keys.c2d, DIR_C2D, frame),
            (entry["counter"], plaintext),
        )

        # Firmware's inbound C2D rule is ctr > last, not exact adjacency.  The
        # stateless crypto layer therefore authenticates an arbitrary later
        # counter; the phone-side D2C client below is intentionally stricter so
        # a lost notification never reaches reply parsing.
        later = build_data_frame(self.keys.c2d, DIR_C2D, 9, b"status")
        self.assertEqual(
            decrypt_data_frame(self.keys.c2d, DIR_C2D, later), (9, b"status")
        )

    def test_vector_text_and_binary_reply_reassembly(self) -> None:
        client, generation = self.new_established_client()
        text = self.vector["d2c_text_reply"]
        result = None
        for index, encoded in enumerate(text["frames_hex"]):
            result = client.receive_data(unhex(encoded), generation)
            if index + 1 < len(text["frames_hex"]):
                self.assertIsNone(result)
        self.assertIsNotNone(result)
        assert result is not None
        self.assertTrue(result.is_text)
        self.assertFalse(result.is_binary)
        self.assertEqual(result.message_id, text["message_id"])
        self.assertEqual(result.fragment_count, 3)
        self.assertEqual((result.first_counter, result.last_counter), (1, 3))
        self.assertEqual(result.payload.hex(), text["payload_hex"])
        self.assertEqual(json.loads(result.payload), json.loads(text["payload_utf8"]))

        binary = self.vector["d2c_binary_reply"]
        binary_result = client.receive_data(unhex(binary["frame_hex"]), generation)
        self.assertIsNotNone(binary_result)
        assert binary_result is not None
        self.assertTrue(binary_result.is_binary)
        self.assertEqual(binary_result.message_id, binary["message_id"])
        self.assertEqual(binary_result.payload.hex(), binary["payload_hex"])
        client.assert_complete(generation)

    def test_client_command_counter_starts_at_one(self) -> None:
        client, generation = self.new_established_client()
        expected = self.vector["c2d_data"]
        self.assertEqual(
            client.encrypt_command(expected["plaintext_utf8"].encode(), generation).hex(),
            expected["frame_hex"],
        )
        second = client.encrypt_command(b"second", generation)
        self.assertEqual(int.from_bytes(second[1:9], "big"), 2)


class StrictFailureTests(VectorFixture, unittest.TestCase):
    def test_counter_gap_is_a_hard_phone_side_failure(self) -> None:
        client, generation = self.new_established_client()
        second = unhex(self.vector["d2c_text_reply"]["frames_hex"][1])
        with self.assertRaises(CounterGapError):
            client.receive_data(second, generation)
        self.assertEqual(client.state, ClientState.FAILED)
        self.assertEqual(client.pending_message_ids, ())

    def test_replayed_counter_is_rejected(self) -> None:
        client, generation = self.new_established_client()
        plaintext = build_reply_fragment(REPLY_VERSION_TEXT, 5, 0, 1, b"one")
        frame = build_data_frame(self.keys.d2c, DIR_D2C, 1, plaintext)
        self.assertEqual(client.receive_data(frame, generation).payload, b"one")
        with self.assertRaises(ReplayError):
            client.receive_data(frame, generation)
        self.assertEqual(client.state, ClientState.FAILED)

    def test_duplicate_and_changed_duplicate_fragments_are_rejected(self) -> None:
        reassembler = ReplyReassembler()
        first = build_reply_fragment(REPLY_VERSION_TEXT, 7, 0, 2, b"first")
        self.assertIsNone(reassembler.feed(first, 1, now=0))
        with self.assertRaises(DuplicateFragmentError):
            reassembler.feed(first, 2, now=0.1)

        reassembler.reset()
        self.assertIsNone(reassembler.feed(first, 1, now=0))
        changed = build_reply_fragment(REPLY_VERSION_TEXT, 7, 0, 2, b"changed")
        with self.assertRaises(ConflictingFragmentError):
            reassembler.feed(changed, 2, now=0.1)

    def test_conflicting_metadata_interleave_and_order_are_rejected(self) -> None:
        first = build_reply_fragment(REPLY_VERSION_TEXT, 9, 0, 2, b"first")

        metadata = ReplyReassembler()
        self.assertIsNone(metadata.feed(first, 1, now=0))
        changed_count = build_reply_fragment(REPLY_VERSION_TEXT, 9, 1, 3, b"second")
        with self.assertRaises(ConflictingFragmentError):
            metadata.feed(changed_count, 2, now=0.1)

        interleaved = ReplyReassembler()
        self.assertIsNone(interleaved.feed(first, 1, now=0))
        another = build_reply_fragment(REPLY_VERSION_TEXT, 10, 0, 1, b"other")
        with self.assertRaises(ConflictingFragmentError):
            interleaved.feed(another, 2, now=0.1)

        out_of_order = ReplyReassembler()
        fragment_one = build_reply_fragment(REPLY_VERSION_TEXT, 9, 1, 2, b"second")
        with self.assertRaises(ConflictingFragmentError):
            out_of_order.feed(fragment_one, 1, now=0)

    def test_timeout_and_explicit_incomplete_checks_never_deliver(self) -> None:
        clock = [0.0]
        client, generation = self.new_established_client(
            reassembly_timeout=1.0, clock=lambda: clock[0]
        )
        first = build_reply_fragment(REPLY_VERSION_TEXT, 22, 0, 2, b"partial")
        frame = build_data_frame(self.keys.d2c, DIR_D2C, 1, first)
        self.assertIsNone(client.receive_data(frame, generation))
        with self.assertRaises(IncompleteMessageError):
            client.assert_complete(generation)
        self.assertEqual(client.state, ClientState.FAILED)

        client.reset(generation)
        client, generation = self.new_established_client(
            reassembly_timeout=1.0, clock=lambda: clock[0]
        )
        self.assertIsNone(client.receive_data(frame, generation))
        clock[0] = 1.0
        with self.assertRaises(IncompleteMessageError):
            client.expire_reassembly(generation)
        self.assertEqual(client.state, ClientState.FAILED)

    def test_reset_reports_incomplete_and_fences_late_old_callback(self) -> None:
        client, old_generation = self.new_established_client()
        first = build_reply_fragment(REPLY_VERSION_TEXT, 0xFFFE, 0, 2, b"old")
        old_frame = build_data_frame(self.keys.d2c, DIR_D2C, 1, first)
        self.assertIsNone(client.receive_data(old_frame, old_generation))

        with self.assertRaises(IncompleteMessageError) as caught:
            client.reset(old_generation)
        self.assertEqual(caught.exception.message_ids, (0xFFFE,))
        self.assertEqual(client.state, ClientState.IDLE)
        self.assertEqual(client.pending_message_ids, ())

        new_generation, _ = client.begin_handshake(
            app_private_key=self.app_private, app_nonce=self.app_nonce
        )
        self.assertNotEqual(new_generation, old_generation)
        with self.assertRaises(StaleSessionError):
            client.receive_hello_ack(
                unhex(self.handshake["hello_ack_hex"]), old_generation
            )
        self.assertEqual(client.state, ClientState.AWAIT_HELLO_ACK)

        client.receive_hello_ack(
            unhex(self.handshake["hello_ack_hex"]), new_generation
        )
        client.receive_confirm_ack(
            unhex(self.vector["confirmation"]["confirm_ack_hex"]), new_generation
        )
        # Same-connection HELLO resets crypto counters but firmware preserves
        # txMsgId.  A fresh reassembly generation therefore accepts any first
        # msgId rather than incorrectly requiring zero.
        replacement_plaintext = build_reply_fragment(
            REPLY_VERSION_TEXT, 0xFFFF, 0, 1, b"replacement"
        )
        replacement_frame = build_data_frame(
            self.keys.d2c, DIR_D2C, 1, replacement_plaintext
        )
        replacement = client.receive_data(replacement_frame, new_generation)
        self.assertEqual(replacement.payload, b"replacement")

    def test_authenticated_tag_failure_poisoned_session(self) -> None:
        client, generation = self.new_established_client()
        frame = bytearray(unhex(self.vector["d2c_text_reply"]["frames_hex"][0]))
        frame[-1] ^= 1
        with self.assertRaises(AuthenticationError):
            client.receive_data(frame, generation)
        self.assertEqual(client.state, ClientState.FAILED)


class FramingBoundaryTests(unittest.TestCase):
    def test_little_endian_message_id_and_binary_marker(self) -> None:
        plaintext = build_reply_fragment(
            REPLY_VERSION_BINARY, 0x1234, 0, 1, b"\x00\xff"
        )
        self.assertEqual(plaintext[:5], bytes((2, 0x34, 0x12, 0, 1)))
        parsed = parse_reply_fragment(plaintext)
        self.assertEqual(parsed.message_id, 0x1234)
        self.assertEqual(parsed.payload, b"\x00\xff")

    def test_firmware_fragment_size_boundaries(self) -> None:
        for length, expected_count in (
            (0, 1),
            (1, 1),
            (REPLY_PAYLOAD_BYTES, 1),
            (REPLY_PAYLOAD_BYTES + 1, 2),
            (255 * REPLY_PAYLOAD_BYTES, 255),
        ):
            with self.subTest(length=length):
                fragments = fragment_reply(b"x" * length, 0x1234)
                self.assertEqual(len(fragments), expected_count)
                self.assertEqual(
                    b"".join(parse_reply_fragment(part).payload for part in fragments),
                    b"x" * length,
                )
        with self.assertRaises(ValueError):
            fragment_reply(b"x" * (255 * REPLY_PAYLOAD_BYTES + 1), 0)

    def test_malformed_five_byte_headers_fail_closed(self) -> None:
        bad_values = (
            b"",
            b"\x01\x00\x00\x00",
            b"\x03\x00\x00\x00\x01",
            b"\x01\x00\x00\x00\x00",
            b"\x01\x00\x00\x02\x02",
            b"\x01\x00\x00\x00\x01" + b"x" * (REPLY_PAYLOAD_BYTES + 1),
        )
        for plaintext in bad_values:
            with self.subTest(plaintext=plaintext.hex()):
                with self.assertRaises(FrameFormatError):
                    parse_reply_fragment(plaintext)

    def test_message_id_sequence_wraps_after_completed_message(self) -> None:
        reassembler = ReplyReassembler()
        last = build_reply_fragment(REPLY_VERSION_TEXT, 0xFFFF, 0, 1, b"last")
        first = build_reply_fragment(REPLY_VERSION_TEXT, 0x0000, 0, 1, b"first")
        self.assertEqual(reassembler.feed(last, 1, now=0).payload, b"last")
        self.assertEqual(reassembler.feed(first, 2, now=0.1).payload, b"first")


class CompanionBoundaryTests(unittest.TestCase):
    @staticmethod
    def compact_fixture() -> bytes:
        fixture = json.loads(CATALOG_FIXTURE_PATH.read_text(encoding="utf-8"))
        return json.dumps(
            fixture, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")

    def test_offline_catalog_matches_wire_contract(self) -> None:
        compact = self.compact_fixture()
        summary = validate_live_catalog(compact)
        self.assertEqual(
            (summary.family_count, summary.kind_count, summary.byte_count),
            (12, 152, 2877),
        )
        self.assertEqual(classify_catalog_reply(compact), summary)

    def test_catalog_reorder_duplicate_key_and_drift_fail_closed(self) -> None:
        fixture = json.loads(CATALOG_FIXTURE_PATH.read_text(encoding="utf-8"))
        fixture["families"][0], fixture["families"][1] = (
            fixture["families"][1],
            fixture["families"][0],
        )
        reordered = json.dumps(
            fixture, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        with self.assertRaises(CatalogValidationError):
            validate_live_catalog(reordered)
        with self.assertRaises(CatalogValidationError):
            classify_catalog_reply(b'{"families":[],"families":[]}')
        with self.assertRaises(CatalogValidationError):
            classify_catalog_reply(b'{"families":not-json}')

    def test_reserved_catalog_names_fail_closed(self) -> None:
        for reserved in ("boot", "none", "set", "patch", "all", "list"):
            with self.subTest(reserved=reserved):
                fixture = json.loads(
                    CATALOG_FIXTURE_PATH.read_text(encoding="utf-8")
                )
                fixture["families"][0]["k"][0] = reserved
                payload = json.dumps(
                    fixture, ensure_ascii=False, separators=(",", ":")
                ).encode("utf-8")
                with self.assertRaisesRegex(CatalogValidationError, "reserved"):
                    validate_live_catalog(payload)

    def test_unsolicited_messages_are_consumed_before_reply_matching(self) -> None:
        notification = b"#NOTIF synthetic-sensitive-body"
        self.assertIsNone(classify_login_reply(notification, "alice"))
        self.assertIsNone(classify_catalog_reply(notification))
        self.assertIsNone(classify_login_reply(b"unrelated complete text", "alice"))
        self.assertIsNone(classify_catalog_reply(b'{"schema":1,"other":true}'))
        self.assertTrue(
            classify_login_reply(
                b"[ble] Login successful. User: alice (admin)", "alice"
            )
        )
        with self.assertRaises(CompanionError):
            classify_login_reply(b"[ble] Authentication failed.", "alice")

    def test_login_command_is_bounded_and_unambiguous(self) -> None:
        self.assertEqual(
            build_login_command("alice smith", "synthetic pass"),
            bytearray(b'login "alice smith" "synthetic pass"'),
        )
        for username, password in (
            ('bad"name', "valid"),
            ("valid", 'bad"secret'),
            ("valid", "line\nbreak"),
            ("snowman-\u2603", "valid"),
            ("x" * 65, "valid"),
        ):
            with self.subTest(username=username, password_length=len(password)):
                with self.assertRaises(CompanionError):
                    build_login_command(username, password)


class CompanionCleanupTests(unittest.IsolatedAsyncioTestCase):
    async def test_prewrite_connection_failure_wipes_login_command(self) -> None:
        class FailingClient:
            def __init__(self, *_args, **_kwargs) -> None:
                self.is_connected = False

            async def connect(self) -> None:
                raise RuntimeError("synthetic offline connect failure")

        command = build_login_command("alice", "synthetic pass")
        channel = SecureChannelV1Client("synthetic secure-channel secret")
        args = Namespace(connect_timeout=1.0)
        with self.assertRaises(TransportError):
            await _run_connected_physical(
                args,
                object(),
                FailingClient,
                object(),
                "alice",
                channel,
                command,
            )
        self.assertEqual(command, bytearray(len(command)))
        self.assertEqual(channel.state, ClientState.IDLE)


class CompanionUnsolicitedFlowTests(VectorFixture, unittest.IsolatedAsyncioTestCase):
    class Pump:
        def __init__(self, generation: int, frames: list[bytes]) -> None:
            self.generation = generation
            self.frames = list(frames)

        async def get(self, _timeout: float) -> tuple[bytes, int]:
            if not self.frames:
                raise asyncio.TimeoutError
            return self.frames.pop(0), self.generation

    def device_frames(
        self,
        payloads: list[tuple[int, bytes]],
        first_counter: int,
    ) -> list[bytes]:
        frames: list[bytes] = []
        counter = first_counter
        for message_id, payload in payloads:
            for plaintext in fragment_reply(payload, message_id):
                frames.append(
                    build_data_frame(self.keys.d2c, DIR_D2C, counter, plaintext)
                )
                counter += 1
        return frames

    async def test_unsolicited_messages_advance_stream_without_becoming_replies(self) -> None:
        client, generation = self.new_established_client()
        login_frames = self.device_frames(
            [
                (90, b"#NOTIF " + b"s" * 220),
                (91, b"[ble] Login successful. User: alice"),
            ],
            1,
        )
        login_pump = self.Pump(generation, login_frames)
        await _wait_for_login(login_pump, client, generation, "alice", 1.0)
        self.assertEqual(login_pump.frames, [])

        fixture = json.loads(CATALOG_FIXTURE_PATH.read_text(encoding="utf-8"))
        compact = json.dumps(
            fixture, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        catalog_frames = self.device_frames(
            [
                (92, b"#NOTIF synthetic-between-command-and-reply"),
                (93, compact),
            ],
            4,
        )
        catalog_pump = self.Pump(generation, catalog_frames)
        summary = await _wait_for_catalog(
            catalog_pump, client, generation, 1.0
        )
        self.assertEqual(
            (summary.family_count, summary.kind_count, summary.byte_count),
            (12, 152, 2877),
        )
        self.assertEqual(catalog_pump.frames, [])
        client.assert_complete(generation)


if __name__ == "__main__":
    unittest.main()
