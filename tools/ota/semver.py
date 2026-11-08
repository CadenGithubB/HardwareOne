"""Strict version validation shared by OTA host tools.

The grammar is Semantic Versioning extended with an optional fourth core number
for point releases: major.minor.patch[.revision][-prerelease][+build]. A
missing revision counts as 0, so 0.99.94 < 0.99.94.1 < 0.99.95.

The recovery updater validates signed manifest versions with the protocol
implementation in ``components/hw1_ota_protocol/hw1_ota_protocol.c``.  Host
release tooling must reject the same invalid values before it signs or audits
an artifact; otherwise a locally green release can be refused only after it
reaches the device.
"""

from __future__ import annotations


MAX_CORE_NUMBER = 0xFFFFFFFF


def _identifier_char(character: str) -> bool:
    return (
        "0" <= character <= "9"
        or "A" <= character <= "Z"
        or "a" <= character <= "z"
        or character == "-"
    )


def _valid_identifiers(value: str, *, reject_numeric_leading_zero: bool) -> bool:
    if not value:
        return False
    for identifier in value.split("."):
        if not identifier or not all(_identifier_char(char) for char in identifier):
            return False
        numeric = all("0" <= char <= "9" for char in identifier)
        if (
            reject_numeric_leading_zero
            and numeric
            and len(identifier) > 1
            and identifier[0] == "0"
        ):
            return False
    return True


def is_valid(value: object) -> bool:
    """Match the firmware protocol's accepted SemVer grammar exactly."""

    if not isinstance(value, str) or not value:
        return False

    version, build_separator, build = value.partition("+")
    if build_separator:
        if "+" in build or not _valid_identifiers(
            build, reject_numeric_leading_zero=False
        ):
            return False

    core, prerelease_separator, prerelease = version.partition("-")
    if prerelease_separator and not _valid_identifiers(
        prerelease, reject_numeric_leading_zero=True
    ):
        return False

    numbers = core.split(".")
    if len(numbers) not in (3, 4):
        return False
    for number in numbers:
        if not number or not all("0" <= char <= "9" for char in number):
            return False
        if len(number) > 1 and number[0] == "0":
            return False
        if int(number) > MAX_CORE_NUMBER:
            return False
    return True


def require(value: object, field: str = "version") -> str:
    """Return a valid version or raise a release-tool-friendly error."""

    if not is_valid(value):
        raise ValueError(
            f"{field} {value!r} is not valid SemVer; expected "
            "major.minor.patch[.revision] with optional -prerelease and +build metadata"
        )
    return value


def _split(value: str) -> tuple[tuple[int, int, int, int], str]:
    core_and_pre = value.partition("+")[0]
    core, _, prerelease = core_and_pre.partition("-")
    numbers = [int(number) for number in core.split(".")]
    if len(numbers) == 3:
        numbers.append(0)
    return (numbers[0], numbers[1], numbers[2], numbers[3]), prerelease


def _compare_identifier(left: str, right: str) -> int:
    left_numeric = left.isdigit()
    right_numeric = right.isdigit()
    if left_numeric != right_numeric:
        return -1 if left_numeric else 1
    if left_numeric:
        return (int(left) > int(right)) - (int(left) < int(right))
    return (left > right) - (left < right)


def compare(left: str, right: str) -> int:
    """Order two versions as hw1_ota_semver_compare() does on the device.

    Returns -1, 0 or 1. A missing revision counts as 0 and +build metadata is
    ignored, so 1.1.0+fv2ho1 equals 1.1.0. Raises ValueError on invalid input.
    """

    require(left, "left version")
    require(right, "right version")
    left_core, left_pre = _split(left)
    right_core, right_pre = _split(right)
    if left_core != right_core:
        return -1 if left_core < right_core else 1
    if not left_pre or not right_pre:
        if left_pre == right_pre:
            return 0
        return 1 if not left_pre else -1
    left_ids = left_pre.split(".")
    right_ids = right_pre.split(".")
    for left_id, right_id in zip(left_ids, right_ids):
        compared = _compare_identifier(left_id, right_id)
        if compared:
            return compared
    return (len(left_ids) > len(right_ids)) - (len(left_ids) < len(right_ids))
