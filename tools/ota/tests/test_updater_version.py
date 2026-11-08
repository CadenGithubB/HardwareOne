"""The recovery updater carries its own version, so minUpdaterVersion can gate.

Every updater used to build as 1.0.0+<suffix>; a manifest's minimum updater
could never refuse one, and `otastatus` could not tell two updaters apart.
"""
from __future__ import annotations

import pathlib
import re
import unittest

from tools.ota import check_ota_builds, make_manifest, semver

REPO = pathlib.Path(__file__).resolve().parents[3]
UPDATER_CMAKE = REPO / "updater" / "CMakeLists.txt"
BUILD_DEPLOYMENT = REPO / "tools" / "build_deployment.sh"
PLAIN = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


def core(version: str) -> tuple[int, int, int]:
    match = PLAIN.match(version)
    if not match:
        raise AssertionError(f"{version!r} is not a plain X.Y.Z")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def updater_semver() -> str:
    found = re.findall(
        r'^set\(HW1_UPDATER_SEMVER "([^"]+)"\)$',
        UPDATER_CMAKE.read_text(),
        re.MULTILINE,
    )
    if len(found) != 1:
        raise AssertionError(f"expected one HW1_UPDATER_SEMVER, found {found!r}")
    return found[0]


class UpdaterVersionTests(unittest.TestCase):
    def test_updater_has_its_own_bumped_semver(self) -> None:
        version = updater_semver()
        self.assertTrue(semver.is_valid(version))
        # 1.0.0 is what every earlier updater reported; a gate needs a newer one.
        self.assertGreater(core(version), (1, 0, 0))

    def test_project_version_is_built_from_it(self) -> None:
        source = UPDATER_CMAKE.read_text()
        self.assertIn(
            'set(_project_version "${HW1_UPDATER_SEMVER}+${_version_suffix}")',
            source,
        )
        self.assertNotRegex(source, r'set\(_project_version "\d')
        self.assertIn('set(PROJECT_VER "${_project_version}")', source)

    def test_default_manifest_floor_admits_current_updater(self) -> None:
        args = make_manifest.parser().parse_args(
            ["create", "--board", "feathers3", "--image", "x", "--key", "k",
             "--output", "o"]
        )
        self.assertLessEqual(core(args.min_updater), core(updater_semver()))

    def test_release_build_can_raise_the_floor(self) -> None:
        script = BUILD_DEPLOYMENT.read_text()
        self.assertIn(
            '${HW1_MIN_UPDATER_VERSION:+--min-updater "$HW1_MIN_UPDATER_VERSION"}',
            script,
        )

    def test_release_build_audits_the_floor_before_signing(self) -> None:
        script = BUILD_DEPLOYMENT.read_text()
        audit = script.index('"$REPO/tools/ota/check_ota_builds.py"')
        sign = script.index('"$REPO/tools/ota/make_manifest.py" create')
        floor = '${HW1_MIN_UPDATER_VERSION:+--min-updater "$HW1_MIN_UPDATER_VERSION"}'
        self.assertIn(floor, script[audit:sign])


class MinimumUpdaterAuditTests(unittest.TestCase):
    def errors(self, updater: str, floor: str | None) -> list[str]:
        audit = check_ota_builds.Audit()
        check_ota_builds.check_min_updater(audit, updater, floor)
        return audit.errors

    def test_floor_above_the_paired_updater_fails(self) -> None:
        built = f"{updater_semver()}+fv2ho1"
        major, minor, patch = core(updater_semver())
        for floor in (f"{major}.{minor + 1}.0", f"{major}.{minor}.{patch}.1",
                      f"{major + 1}.0.0-rc.1"):
            with self.subTest(floor=floor):
                self.assertTrue(self.errors(built, floor))

    def test_floor_at_or_below_the_paired_updater_passes(self) -> None:
        built = f"{updater_semver()}+fv2ho1"
        for floor in (None, "1.0.0", updater_semver(), f"{updater_semver()}+fv2ho1",
                      f"{updater_semver()}-rc.1"):
            with self.subTest(floor=floor):
                self.assertEqual(self.errors(built, floor), [])

    def test_invalid_floor_fails(self) -> None:
        self.assertTrue(self.errors("1.1.0+fv2ho1", "1.1"))


class SemverCompareTests(unittest.TestCase):
    def test_matches_firmware_ordering(self) -> None:
        ordered = (
            "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
            "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.0.1",
            "1.0.1", "1.1.0", "1.10.0", "2.0.0",
        )
        for index, lower in enumerate(ordered):
            for higher in ordered[index + 1:]:
                with self.subTest(lower=lower, higher=higher):
                    self.assertEqual(semver.compare(lower, higher), -1)
                    self.assertEqual(semver.compare(higher, lower), 1)

    def test_build_metadata_and_zero_revision_are_equal(self) -> None:
        self.assertEqual(semver.compare("1.1.0+fv2ho1", "1.1.0"), 0)
        self.assertEqual(semver.compare("1.1.0.0", "1.1.0+x"), 0)

    def test_invalid_input_raises(self) -> None:
        with self.assertRaises(ValueError):
            semver.compare("1.1", "1.1.0")


if __name__ == "__main__":
    unittest.main()
