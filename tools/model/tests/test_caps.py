"""Tests for the CAPS stamper.

This tool edits multi-megabyte model files that are expensive to regenerate, and
its whole promise is that everything except the header word and the info block
is copied through untouched. So the tail-preservation property is asserted
directly on every mutation path, not assumed.

Run from the repository root:

    python3 -m unittest discover -s tools/model/tests -t .
"""

from __future__ import annotations

import struct
import unittest

from tools.model import caps


def make_model(sections: list[tuple[int, bytes]] | None, tail: bytes = b"TOKENIZER+WEIGHTS") -> bytes:
    """Build a synthetic LLM1 file. `sections=None` means no info block at all."""
    header = bytearray(64)
    struct.pack_into(">I", header, 0, caps.MAGIC)
    header[4] = 2          # file_version
    header[5] = 1          # quant_type
    info = caps.build_info_block(sections) if sections is not None else b""
    struct.pack_into("<I", header, caps.INFO_LEN_OFF, len(info))
    return bytes(header) + info + tail


class ParseTests(unittest.TestCase):
    def test_no_info_block_is_normal(self) -> None:
        """The shipped help agent is exactly this shape: info_len 0."""
        blob = make_model(None)
        self.assertEqual((0, []), caps.parse_sections(blob))
        self.assertIsNone(caps.read_caps(blob))

    def test_rejects_a_foreign_file(self) -> None:
        bad = bytearray(make_model(None))
        bad[0] = 0x00
        with self.assertRaises(caps.ModelFormatError) as e:
            caps.parse_sections(bytes(bad))
        self.assertIn("magic", str(e.exception))

    def test_refuses_a_legacy_v1_info_block(self) -> None:
        """A non-zero info_len that lacks the v2 sentinel is v1 positional data.

        The firmware skips such a block wholesale. Rewriting it would be a format
        migration disguised as a capability stamp, so the tool refuses.
        """
        header = bytearray(64)
        struct.pack_into(">I", header, 0, caps.MAGIC)
        v1 = b"\x05\x00hello"          # desc_len=5 -- high byte 0, so not the sentinel
        struct.pack_into("<I", header, caps.INFO_LEN_OFF, len(v1))
        with self.assertRaises(caps.ModelFormatError) as e:
            caps.parse_sections(bytes(header) + v1 + b"TAIL")
        self.assertIn("v1", str(e.exception))

    def test_rejects_a_length_that_overruns(self) -> None:
        blob = bytearray(make_model([(1, b"desc")]))
        struct.pack_into("<I", blob, caps.INFO_LEN_OFF, 10_000)
        with self.assertRaises(caps.ModelFormatError):
            caps.parse_sections(bytes(blob))

    def test_unknown_caps_version_declares_nothing(self) -> None:
        """Fail closed: a version we cannot read is not a grant of trust."""
        blob = make_model([(caps.CAPS_SECTION_ID, struct.pack("<BH", 99, 0xFFFF))])
        self.assertIsNone(caps.read_caps(blob))

    def test_truncated_caps_payload_declares_nothing(self) -> None:
        blob = make_model([(caps.CAPS_SECTION_ID, b"\x01")])
        self.assertIsNone(caps.read_caps(blob))


class StampTests(unittest.TestCase):
    TAIL = b"TOKENIZER+WEIGHTS" * 100

    def _assert_tail_survived(self, before: bytes, after: bytes) -> None:
        old_len, _ = caps.parse_sections(before)
        new_len, _ = caps.parse_sections(after)
        self.assertEqual(
            before[caps.HEADER_LEN + old_len:],
            after[caps.HEADER_LEN + new_len:],
            "tokenizer/weights must be copied through byte-for-byte",
        )

    def test_stamps_a_model_with_no_info_block(self) -> None:
        blob = make_model(None, self.TAIL)
        out = caps.set_caps(blob, caps.CAP_COMMAND_MODE)
        self.assertEqual(caps.CAP_COMMAND_MODE, caps.read_caps(out))
        self._assert_tail_survived(blob, out)
        # 2 sentinel + 1 count + (1 id + 4 len + 3 payload)
        self.assertEqual(len(blob) + 11, len(out))

    def test_preserves_existing_sections(self) -> None:
        blob = make_model([(1, b"a description"), (4, b"menu-ish")], self.TAIL)
        out = caps.set_caps(blob, caps.CAP_COMMAND_MODE)
        _len, sections = caps.parse_sections(out)
        self.assertEqual([1, 4, caps.CAPS_SECTION_ID], [sid for sid, _ in sections])
        self.assertEqual(b"a description", sections[0][1])
        self.assertEqual(b"menu-ish", sections[1][1])
        self._assert_tail_survived(blob, out)

    def test_restamping_replaces_rather_than_duplicates(self) -> None:
        blob = make_model(None, self.TAIL)
        once = caps.set_caps(blob, caps.CAP_COMMAND_MODE)
        twice = caps.set_caps(once, 0)
        _len, sections = caps.parse_sections(twice)
        self.assertEqual(1, sum(1 for sid, _ in sections if sid == caps.CAPS_SECTION_ID),
                         "a second stamp must replace the CAPS section, not append another")
        self.assertEqual(0, caps.read_caps(twice))
        self.assertEqual(len(once), len(twice))

    def test_clearing_the_bit_is_expressible(self) -> None:
        """Revoking is as important as granting: a model can be un-trusted."""
        blob = caps.set_caps(make_model(None, self.TAIL), caps.CAP_COMMAND_MODE)
        cleared = caps.set_caps(blob, 0)
        self.assertEqual(0, caps.read_caps(cleared))
        self.assertNotEqual(None, caps.read_caps(cleared),
                            "an explicit zero is a declaration, distinct from no section")


if __name__ == "__main__":
    unittest.main()
