#!/usr/bin/env python3
"""Compare production catalog JSON with independent typed traversal output."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys


HERE = Path(__file__).resolve().parent
FIXTURE_PATH = HERE / "fixtures" / "event_catalog_v1.json"
EXPECTED_FAMILY_COUNT = 12
EXPECTED_KIND_COUNT = 152
RUN_TIMEOUT_SECONDS = 20

if not __debug__:
    raise RuntimeError(
        "event catalog JSON parse checks require Python assertions; do not use -O"
    )

FAMILY_RECORD_RE = re.compile(
    rb"F ([0-9]+) ([0-9]+) ([0-9]+) ([0-9a-f]+)"
)
KIND_RECORD_RE = re.compile(
    rb"K ([0-9]+) ([0-9]+) ([0-9]+) ([0-9a-f]+)"
)


def fail(message: str) -> None:
    raise AssertionError(message)


def run_dump(binary: Path, mode: str) -> bytes:
    result = subprocess.run(
        [str(binary), mode],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=RUN_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        fail(
            f"{binary.name} {mode} exited {result.returncode}: "
            f"{result.stderr.decode('utf-8', errors='replace')}"
        )
    if result.stderr:
        fail(
            f"{binary.name} {mode} wrote unexpected stderr: "
            f"{result.stderr.decode('utf-8', errors='replace')}"
        )
    if not result.stdout:
        fail(f"{binary.name} {mode} produced no output")
    return result.stdout


def load_fixture() -> dict[str, object]:
    with FIXTURE_PATH.open("r", encoding="utf-8") as fixture_file:
        fixture = json.load(fixture_file)
    if not isinstance(fixture, dict) or set(fixture) != {"families"}:
        fail("event catalog fixture must contain only a families array")
    return fixture


def decode_hex_field(
    encoded: bytes, declared_length: bytes, record_number: int
) -> str:
    try:
        expected_length = int(declared_length)
        decoded = bytes.fromhex(encoded.decode("ascii"))
    except (UnicodeDecodeError, ValueError) as error:
        fail(f"typed record {record_number} has invalid lowercase hex: {error}")
    if len(decoded) != expected_length:
        fail(
            f"typed record {record_number} declares {expected_length} bytes "
            f"but carries {len(decoded)}"
        )
    try:
        return decoded.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        fail(f"typed record {record_number} is not valid UTF-8: {error}")
    raise AssertionError("unreachable")


def parse_typed_dump(payload: bytes) -> dict[str, object]:
    if payload.strip() != payload.rstrip(b"\r\n"):
        fail("typed dump has leading or non-newline trailing whitespace")
    lines = payload.splitlines()
    if not lines or any(not line for line in lines):
        fail("typed dump must contain non-empty line records")

    families: list[dict[str, object]] = []
    position = 0
    total_kinds = 0
    while position < len(lines):
        family_match = FAMILY_RECORD_RE.fullmatch(lines[position])
        if family_match is None:
            fail(f"typed record {position + 1} is not a family record")

        family_ordinal = int(family_match.group(1))
        kind_count = int(family_match.group(2))
        if family_ordinal != len(families):
            fail(
                f"typed family ordinal {family_ordinal} is not contiguous at "
                f"record {position + 1}"
            )
        label = decode_hex_field(
            family_match.group(4), family_match.group(3), position + 1
        )
        if not label:
            fail(f"typed family {family_ordinal} has an empty label")

        position += 1
        kinds: list[str] = []
        for within_family_ordinal in range(kind_count):
            if position >= len(lines):
                fail(f"typed family {family_ordinal} ended before its kinds")
            kind_match = KIND_RECORD_RE.fullmatch(lines[position])
            if kind_match is None:
                fail(f"typed record {position + 1} is not a kind record")
            if int(kind_match.group(1)) != family_ordinal:
                fail(f"typed kind at record {position + 1} changed family")
            if int(kind_match.group(2)) != within_family_ordinal:
                fail(
                    f"typed kind ordinal at record {position + 1} is not "
                    "contiguous"
                )
            name = decode_hex_field(
                kind_match.group(4), kind_match.group(3), position + 1
            )
            if not re.fullmatch(r"[a-z0-9_]+", name):
                fail(f"typed kind at record {position + 1} is not canonical")
            kinds.append(name)
            total_kinds += 1
            position += 1

        families.append({"n": label, "k": kinds})

    if len(families) != EXPECTED_FAMILY_COUNT:
        fail(
            f"typed dump has {len(families)} families, expected "
            f"{EXPECTED_FAMILY_COUNT}"
        )
    if total_kinds != EXPECTED_KIND_COUNT:
        fail(
            f"typed dump has {total_kinds} kinds, expected {EXPECTED_KIND_COUNT}"
        )
    return {"families": families}


def parse_json_dump(payload: bytes) -> tuple[dict[str, object], bytes]:
    compact_payload = payload.strip()
    if not compact_payload:
        fail("JSON dump is empty")
    try:
        decoded_text = compact_payload.decode("utf-8", errors="strict")
        document = json.loads(decoded_text)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"production serializer output is not valid UTF-8 JSON: {error}")
    if not isinstance(document, dict) or set(document) != {"families"}:
        fail("production serializer output must contain only a families array")
    return document, compact_payload


def validate_catalog_shape(catalog: dict[str, object]) -> None:
    families = catalog.get("families")
    if not isinstance(families, list):
        fail("families must be an array")
    labels: list[str] = []
    names: list[str] = []
    for family in families:
        if not isinstance(family, dict) or set(family) != {"n", "k"}:
            fail("each family must contain only n and k")
        label = family.get("n")
        kinds = family.get("k")
        if not isinstance(label, str) or not label:
            fail("each family label must be a non-empty string")
        if not isinstance(kinds, list) or not kinds:
            fail("each family kind list must be non-empty")
        if not all(
            isinstance(name, str) and re.fullmatch(r"[a-z0-9_]+", name)
            for name in kinds
        ):
            fail("each kind must be a canonical snake_case string")
        labels.append(label)
        names.extend(kinds)

    if len(families) != EXPECTED_FAMILY_COUNT:
        fail("JSON family count changed")
    if len(names) != EXPECTED_KIND_COUNT:
        fail("JSON kind count changed")
    if len(set(labels)) != len(labels):
        fail("JSON family labels are not unique")
    if len({name.casefold() for name in names}) != len(names):
        fail("JSON kind names are not unique under ASCII case-folding")
    reserved = {"boot", "none", "set", "patch", "all", "list"}
    if reserved & set(names):
        fail("JSON enumeration exposed a reserved event-kind token")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(
            f"usage: {Path(argv[0]).name} /path/to/event_catalog_json_tests",
            file=sys.stderr,
        )
        return 2

    binary = Path(argv[1]).resolve()
    if not binary.is_file():
        fail(f"host test binary does not exist: {binary}")

    fixture = load_fixture()
    typed = parse_typed_dump(run_dump(binary, "--dump-typed"))
    serialized, serialized_bytes = parse_json_dump(
        run_dump(binary, "--dump-json")
    )
    validate_catalog_shape(typed)
    validate_catalog_shape(serialized)

    if typed != fixture:
        fail("typed provider traversal differs from the reviewed v1 fixture")
    if serialized != typed:
        fail("parsed production JSON differs from typed provider traversal")

    expected_compact = json.dumps(
        fixture, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    if serialized_bytes != expected_compact:
        fail("production JSON is not byte-identical to the compact v1 fixture")

    print("event catalog JSON parse parity passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
