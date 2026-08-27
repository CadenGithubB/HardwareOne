"""Read and stamp the CAPS section (id 5) of an LLM1 model file.

WHY THIS EXISTS
    The firmware decides whether a model may have its answers offered as
    RUNNABLE device commands (the /llm page's Do: mode, which renders a RUN
    button) by reading a capability the MODEL declares about itself. A model
    that declares nothing gets nothing -- fail closed, because a model that has
    never seen this device's command set cannot know a real command and would
    invent a plausible-looking one.

    Without this tool the only way to add that declaration is to re-run the
    browser converter and re-emit the model. This stamps an existing .bin
    instead: the info block is self-framing and the tokenizer and weights are
    read sequentially after it, so they are copied through byte-for-byte.

FILE LAYOUT (from the converter's writer, verified against a real model)
    64-byte header:
        @0  u32BE magic 'LLM1' (0x4C4C4D31)      @4  u8 file_version
        @5  u8 quant_type                        @6  u16LE group_size
        @8  u16LE dim                            @10 u16LE hidden_dim
        @12 u8 n_layers  @13 u8 n_heads  @14 u8 n_kv_heads
        @15 u32LE vocab_size (unaligned)         @19 u16LE seq_len
        @21 u8 arch_type   @22..23 reserved
        @24 u32LE info_len  (0 = no info block)  @28..63 pad
    then the info block (info_len bytes), then tokenizer, then weights.

    Info block v2:
        u16LE sentinel 0x4932
        u8    section_count
        section_count x [u8 id][u32LE len][payload]
        ids: 1=DESC 2=ICON 3=DOMAIN 4=MENU 5=CAPS

    CAPS payload: u8 caps_version (1) + u16LE flags.

WHAT THIS REFUSES TO DO
    - Touch a file whose magic is not LLM1.
    - Touch a legacy v1 info block (a non-zero info_len that does not open with
      the v2 sentinel). The firmware skips such a block wholesale; rewriting it
      as v2 would be a format migration, not a capability stamp, and it is not
      this tool's job to guess the v1 layout.
    - Overwrite the input unless asked. Model files are large and not cheaply
      regenerated.

Run from the repository root:

    python3 tools/model/caps.py show <model.bin>
    python3 tools/model/caps.py set  <model.bin> --command-mode -o <out.bin>
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

MAGIC = 0x4C4C4D31          # 'LLM1', stored BIG-endian at offset 0
HEADER_LEN = 64
INFO_LEN_OFF = 24
V2_SENTINEL = 0x4932
CAPS_SECTION_ID = 5
CAPS_VERSION = 1

# Keep in step with System_LLM.h.
CAP_COMMAND_MODE = 0x0001

SECTION_NAMES = {1: "DESC", 2: "ICON", 3: "DOMAIN", 4: "MENU", 5: "CAPS"}


class ModelFormatError(Exception):
    """The file is not a model this tool is willing to modify."""


def _check_magic(blob: bytes) -> None:
    if len(blob) < HEADER_LEN:
        raise ModelFormatError(f"file is {len(blob)} bytes, shorter than the 64-byte header")
    (magic,) = struct.unpack_from(">I", blob, 0)
    if magic != MAGIC:
        raise ModelFormatError(
            f"magic is 0x{magic:08X}, expected 0x{MAGIC:08X} ('LLM1') -- not a model file"
        )


def parse_sections(blob: bytes) -> tuple[int, list[tuple[int, bytes]]]:
    """Return (info_len, [(section_id, payload), ...]).

    An info_len of 0 means there is no info block at all, which is a normal
    state -- the shipped help agent is exactly that.
    """
    _check_magic(blob)
    (info_len,) = struct.unpack_from("<I", blob, INFO_LEN_OFF)
    if info_len == 0:
        return 0, []
    if HEADER_LEN + info_len > len(blob):
        raise ModelFormatError(
            f"info_len {info_len} overruns the file ({len(blob)} bytes total)"
        )
    body = blob[HEADER_LEN:HEADER_LEN + info_len]
    if len(body) < 3:
        raise ModelFormatError(f"info block is {len(body)} bytes, too short for a v2 header")
    (sentinel,) = struct.unpack_from("<H", body, 0)
    if sentinel != V2_SENTINEL:
        raise ModelFormatError(
            f"info block opens with 0x{sentinel:04X}, not the v2 sentinel 0x{V2_SENTINEL:04X}. "
            "This is a legacy v1 block; refusing to rewrite it."
        )
    count = body[2]
    out: list[tuple[int, bytes]] = []
    off = 3
    for i in range(count):
        if off + 5 > len(body):
            raise ModelFormatError(f"section {i} header runs past the info block")
        sid = body[off]
        (slen,) = struct.unpack_from("<I", body, off + 1)
        off += 5
        if off + slen > len(body):
            raise ModelFormatError(f"section {i} (id {sid}) length {slen} runs past the info block")
        out.append((sid, body[off:off + slen]))
        off += slen
    return info_len, out


def read_caps(blob: bytes) -> int | None:
    """Flags the model declares, or None when it declares nothing.

    None and 0 are deliberately distinct: 'no CAPS section' and 'a CAPS section
    saying zero capabilities' are different statements, even though the firmware
    treats both as untrusted.
    """
    _info_len, sections = parse_sections(blob)
    for sid, payload in sections:
        if sid != CAPS_SECTION_ID:
            continue
        if len(payload) < 3:
            return None                       # malformed -> declares nothing
        if payload[0] != CAPS_VERSION:
            return None                       # unknown version -> declares nothing
        (flags,) = struct.unpack_from("<H", payload, 1)
        return flags
    return None


def build_info_block(sections: list[tuple[int, bytes]]) -> bytes:
    parts = [struct.pack("<HB", V2_SENTINEL, len(sections))]
    for sid, payload in sections:
        parts.append(struct.pack("<BI", sid, len(payload)))
        parts.append(payload)
    return b"".join(parts)


def set_caps(blob: bytes, flags: int) -> bytes:
    """Return a new file with the CAPS section set to `flags`.

    Everything after the info block -- tokenizer and weights -- is copied
    through untouched; the caller asserts that.
    """
    info_len, sections = parse_sections(blob)
    payload = struct.pack("<BH", CAPS_VERSION, flags)
    replaced = False
    new_sections = []
    for sid, old in sections:
        if sid == CAPS_SECTION_ID:
            new_sections.append((sid, payload))
            replaced = True
        else:
            new_sections.append((sid, old))
    if not replaced:
        new_sections.append((CAPS_SECTION_ID, payload))

    info = build_info_block(new_sections)
    header = bytearray(blob[:HEADER_LEN])
    struct.pack_into("<I", header, INFO_LEN_OFF, len(info))
    tail = blob[HEADER_LEN + info_len:]
    return bytes(header) + info + tail


def _describe(blob: bytes) -> str:
    info_len, sections = parse_sections(blob)
    lines = [f"info block: {info_len} bytes, {len(sections)} section(s)"]
    for sid, payload in sections:
        lines.append(f"  id={sid} ({SECTION_NAMES.get(sid, '?')}) len={len(payload)}")
    caps = read_caps(blob)
    if caps is None:
        lines.append("caps: NONE declared -- the firmware will not offer Do: mode for this model")
    else:
        names = []
        if caps & CAP_COMMAND_MODE:
            names.append("COMMAND_MODE")
        other = caps & ~CAP_COMMAND_MODE
        if other:
            names.append(f"unknown bits 0x{other:04X}")
        lines.append(f"caps: 0x{caps:04X}" + (" (" + ", ".join(names) + ")" if names else ""))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_show = sub.add_parser("show", help="print the info block and declared capabilities")
    p_show.add_argument("model", type=pathlib.Path)

    p_set = sub.add_parser("set", help="stamp the CAPS section")
    p_set.add_argument("model", type=pathlib.Path)
    p_set.add_argument("--command-mode", action="store_true",
                       help="declare LLM_CAP_COMMAND_MODE: this model's answers may be "
                            "offered as runnable device commands")
    p_set.add_argument("-o", "--out", type=pathlib.Path,
                       help="output path (default: refuse, to avoid clobbering the input)")
    p_set.add_argument("--in-place", action="store_true",
                       help="overwrite the input file")

    args = ap.parse_args(argv)
    blob = args.model.read_bytes()

    if args.cmd == "show":
        print(_describe(blob))
        return 0

    if not args.out and not args.in_place:
        ap.error("give -o OUT, or --in-place to overwrite the model")
    flags = CAP_COMMAND_MODE if args.command_mode else 0
    out_blob = set_caps(blob, flags)

    # The whole promise of stamping rather than re-emitting is that nothing but
    # the header word and the info block moves. Prove it before writing.
    old_info_len, _ = parse_sections(blob)
    new_info_len, _ = parse_sections(out_blob)
    if blob[HEADER_LEN + old_info_len:] != out_blob[HEADER_LEN + new_info_len:]:
        raise ModelFormatError("tokenizer/weights changed -- refusing to write")
    if read_caps(out_blob) != flags:
        raise ModelFormatError("stamped file does not read back the flags -- refusing to write")

    dest = args.out if args.out else args.model
    dest.write_bytes(out_blob)
    print(f"wrote {dest} ({len(out_blob)} bytes, was {len(blob)})")
    print(_describe(out_blob))
    return 0


if __name__ == "__main__":
    sys.exit(main())
