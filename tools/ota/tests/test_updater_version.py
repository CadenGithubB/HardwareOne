"""The recovery updater carries its own version, so minUpdaterVersion can gate.

Every updater used to build as 1.0.0+<suffix>; a manifest's minimum updater
could never refuse one, and `otastatus` could not tell two updaters apart.
"""
from __future__ import annotations

import pathlib
import re
import unittest

from tools.ota import make_manifest, semver

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


if __name__ == "__main__":
    unittest.main()
