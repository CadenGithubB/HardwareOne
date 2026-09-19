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
