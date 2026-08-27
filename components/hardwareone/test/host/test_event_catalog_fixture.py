#!/usr/bin/env python3
"""Verify frozen v1 grouped and declaration-order System Event fixtures.

This test is deliberately read-only: changing production rows never rewrites
the reviewed fixture. An intentional catalog change must update the fixture in
the same reviewed patch.
"""

from __future__ import annotations

import difflib
import json
from pathlib import Path
import re
import unittest


HERE = Path(__file__).resolve().parent
REPOSITORY_ROOT = HERE.parents[3]
SOURCE_PATH = (
    REPOSITORY_ROOT / "components" / "hardwareone" / "System_EventCatalogRows.h"
)
FIXTURE_PATH = HERE / "fixtures" / "event_catalog_v1.json"
DECLARATION_ORDER_FIXTURE_PATH = (
    HERE / "fixtures" / "event_catalog_declaration_order_v1.txt"
)
EXPECTED_FAMILY_COUNT = 12
EXPECTED_KIND_COUNT = 152

if not __debug__:
    raise RuntimeError(
        "event catalog fixture checks require Python assertions; do not use -O"
    )

FAMILY_ROW_START_RE = re.compile(
    r"^\s*HW1_EVENT_CATALOG_FAMILY_ROW\(", re.MULTILINE
)
FAMILY_ROW_RE = re.compile(
    r'^\s*HW1_EVENT_CATALOG_FAMILY_ROW\(\s*'
    r'(SYSEVT_FAM_[A-Z0-9_]+)\s*,\s*"([^"]*)"\s*\)',
    re.MULTILINE,
)
KIND_ROW_START_RE = re.compile(
    r"^\s*HW1_EVENT_CATALOG_KIND_ROW\(", re.MULTILINE
)
KIND_ROW_RE = re.compile(
    r'^\s*HW1_EVENT_CATALOG_KIND_ROW\(\s*'
    r'(SYSEVT_[A-Z0-9_]+)\s*,\s*"([^"]*)"\s*,\s*'
    r'(SYSEVT_FAM_[A-Z0-9_]+)\s*\)',
    re.MULTILINE,
)
CANONICAL_NAME_RE = re.compile(r"^[a-z0-9_]+$")


def parse_catalog() -> tuple[dict[str, object], list[str]]:
    source = SOURCE_PATH.read_text(encoding="utf-8")

    family_row_count = len(FAMILY_ROW_START_RE.findall(source))
    family_rows = FAMILY_ROW_RE.findall(source)
    if len(family_rows) != family_row_count:
        raise AssertionError(
            "System Event family-row parser recognized "
            f"{len(family_rows)} of {family_row_count} rows; update the parser "
            "deliberately instead of silently skipping a new row shape"
        )

    kind_row_count = len(KIND_ROW_START_RE.findall(source))
    kind_rows = KIND_ROW_RE.findall(source)
    if len(kind_rows) != kind_row_count:
        raise AssertionError(
            "System Event kind-row parser recognized "
            f"{len(kind_rows)} of {kind_row_count} rows; update the parser "
            "deliberately instead of silently skipping a new row shape"
        )

    grouped: dict[str, dict[str, object]] = {}
    families: list[dict[str, object]] = []
    for symbol, label in family_rows:
        if symbol in grouped:
            raise AssertionError(f"duplicate family symbol: {symbol}")
        family = {"n": label, "k": []}
        grouped[symbol] = family
        families.append(family)

    declaration_names: list[str] = []
    for _symbol, name, family_symbol in kind_rows:
        family = grouped.get(family_symbol)
        if family is None:
            raise AssertionError(
                f"kind {name!r} references unknown family {family_symbol}"
            )
        kinds = family["k"]
        assert isinstance(kinds, list)
        kinds.append(name)
        declaration_names.append(name)

    return {"families": families}, declaration_names


def load_fixture() -> dict[str, object]:
    with FIXTURE_PATH.open("r", encoding="utf-8") as fixture_file:
        fixture = json.load(fixture_file)
    if not isinstance(fixture, dict):
        raise AssertionError("fixture root must be an object")
    return fixture


def load_declaration_order_fixture() -> list[str]:
    names = DECLARATION_ORDER_FIXTURE_PATH.read_text(
        encoding="utf-8"
    ).splitlines()
    if any(not name for name in names):
        raise AssertionError("declaration-order fixture contains a blank row")
    return names


class EventCatalogFixtureTest(unittest.TestCase):
    def test_current_grouped_catalog_matches_frozen_v1_order(self) -> None:
        fixture = load_fixture()
        self.assertEqual(set(fixture), {"families"})

        families = fixture.get("families")
        self.assertIsInstance(families, list)
        assert isinstance(families, list)
        self.assertEqual(len(families), EXPECTED_FAMILY_COUNT)

        labels: list[str] = []
        fixture_names: list[str] = []
        for family in families:
            self.assertIsInstance(family, dict)
            assert isinstance(family, dict)
            self.assertEqual(set(family), {"n", "k"})
            label = family.get("n")
            kinds = family.get("k")
            self.assertIsInstance(label, str)
            self.assertTrue(label)
            self.assertIsInstance(kinds, list)
            assert isinstance(label, str)
            assert isinstance(kinds, list)
            self.assertTrue(kinds)
            self.assertTrue(all(isinstance(name, str) for name in kinds))
            labels.append(label)
            fixture_names.extend(kinds)

        self.assertEqual(len(set(labels)), EXPECTED_FAMILY_COUNT)
        self.assertEqual(len(fixture_names), EXPECTED_KIND_COUNT)
        self.assertEqual(
            len({name.casefold() for name in fixture_names}), EXPECTED_KIND_COUNT
        )
        self.assertTrue(
            all(CANONICAL_NAME_RE.fullmatch(name) for name in fixture_names)
        )
        self.assertFalse(
            {"boot", "none", "set", "patch", "all", "list"}
            & set(fixture_names)
        )

        frozen_declaration_names = load_declaration_order_fixture()
        self.assertEqual(len(frozen_declaration_names), EXPECTED_KIND_COUNT)
        self.assertEqual(
            len({name.casefold() for name in frozen_declaration_names}),
            EXPECTED_KIND_COUNT,
        )
        self.assertTrue(
            all(
                CANONICAL_NAME_RE.fullmatch(name)
                for name in frozen_declaration_names
            )
        )

        actual, declaration_names = parse_catalog()
        actual_families = actual["families"]
        assert isinstance(actual_families, list)
        self.assertEqual(len(actual_families), EXPECTED_FAMILY_COUNT)
        self.assertEqual(len(declaration_names), EXPECTED_KIND_COUNT)
        self.assertEqual(len(set(declaration_names)), EXPECTED_KIND_COUNT)
        self.assertEqual(declaration_names, frozen_declaration_names)
        if actual != fixture:
            expected_lines = json.dumps(
                fixture, indent=2, ensure_ascii=False, sort_keys=False
            ).splitlines(keepends=True)
            actual_lines = json.dumps(
                actual, indent=2, ensure_ascii=False, sort_keys=False
            ).splitlines(keepends=True)
            diff = "".join(
                difflib.unified_diff(
                    expected_lines,
                    actual_lines,
                    fromfile=str(FIXTURE_PATH.relative_to(REPOSITORY_ROOT)),
                    tofile=str(SOURCE_PATH.relative_to(REPOSITORY_ROOT)),
                )
            )
            self.fail(f"System Event grouped catalog changed:\n{diff}")

        # Keep readable boundary diagnostics in addition to the exhaustive
        # frozen declaration-order comparison above.
        self.assertEqual(declaration_names[126], "ota_rolled_back")
        self.assertEqual(declaration_names[127], "ota_recovery_entered")
        self.assertEqual(declaration_names[151], "automation_action_dropped")


if __name__ == "__main__":
    unittest.main()
