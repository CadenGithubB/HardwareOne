from __future__ import annotations

import struct
import unittest
import zlib

from tools.ota import check_ota_builds, make_manifest, semver


def manifest_fields() -> dict[str, object]:
    return {
        "boardId": "feather_esp32_v2",
        "layoutId": "hw1-hl-fv2-ota-v1",
        "projectName": "hardwareone-idf",
        "version": "0.99.93+fv2ho1",
        "imageSize": 4096,
        "imageSha256": "11" * 32,
        "minUpdaterVersion": "1.0.0+fv2ho1",
        "dataSchema": 1,
    }


class SemverGrammarTests(unittest.TestCase):
    def test_matches_firmware_protocol_grammar(self) -> None:
        valid = (
            "0.0.0",
            "0.99.93+fv2ho1",
            "1.2.3-alpha.2+build.01",
            "4294967295.4294967295.4294967295",
            "0.99.94.1+fv2ho1",
            "1.2.3.4",
            "1.2.3.4-rc.1+build",
            "4294967295.4294967295.4294967295.4294967295",
        )
        invalid = (
            "1",
            "1.2",
            "1.2.3.4.5",
            "1.2.3.",
            "1.2.3.04",
            "1.2.3.4294967296",
            "01.2.3",
            "1.02.3",
            "1.2.03",
            "4294967296.0.0",
            "1.2.3-01",
            "1.2.3-alpha..2",
            "1.2.3+",
            "1.2.3+build+again",
            "1.2.3+not_valid",
            "1.2.3+β",
        )
        for version in valid:
            with self.subTest(version=version):
                self.assertTrue(semver.is_valid(version))
        for version in invalid:
            with self.subTest(version=version):
                self.assertFalse(semver.is_valid(version))


class ManifestSemverTests(unittest.TestCase):
    def test_encode_accepts_four_part_point_release(self) -> None:
        fields = manifest_fields()
        fields["version"] = "0.99.94.1+fv2ho1"
        fields["minUpdaterVersion"] = "1.0.0.1+fv2ho1"
        decoded = make_manifest.decode_payload(make_manifest.encode_payload(fields))
        self.assertEqual(decoded["version"], "0.99.94.1+fv2ho1")
        self.assertEqual(decoded["minUpdaterVersion"], "1.0.0.1+fv2ho1")

    def test_encode_rejects_five_part_versions_before_signing(self) -> None:
        fields = manifest_fields()
        fields["version"] = "0.99.94.1.1+fv2ho1"
        with self.assertRaisesRegex(ValueError, r"version .*not valid SemVer"):
            make_manifest.encode_payload(fields)

        fields = manifest_fields()
        fields["minUpdaterVersion"] = "1.0.0.1.1+fv2ho1"
        with self.assertRaisesRegex(
            ValueError, r"minUpdaterVersion .*not valid SemVer"
        ):
            make_manifest.encode_payload(fields)

    def test_decode_rejects_signed_payload_with_five_part_version(self) -> None:
        payload = bytearray(make_manifest.encode_payload(manifest_fields()))
        invalid = b"0.99.94.1.1+fv2ho1"
        payload[88:136] = invalid + bytes(48 - len(invalid))
        struct.pack_into(
            "<I", payload, 220, zlib.crc32(payload[:220]) & 0xFFFFFFFF
        )

        with self.assertRaisesRegex(ValueError, r"version .*not valid SemVer"):
            make_manifest.decode_payload(bytes(payload))


class BuildAuditSemverTests(unittest.TestCase):
    def test_audit_requires_both_semver_and_deployment_suffix(self) -> None:
        audit = check_ota_builds.Audit()
        check_ota_builds.check_version_contract(
            audit, "0.99.94.1.1+fv2ho1", "+fv2ho1", "main"
        )
        self.assertEqual(
            audit.errors,
            ["main version '0.99.94.1.1+fv2ho1' is not valid SemVer"],
        )

        audit = check_ota_builds.Audit()
        check_ota_builds.check_version_contract(
            audit, "0.99.94.1+fv2ho1", "+fv2ho1", "main"
        )
        self.assertEqual(audit.errors, [])

        audit = check_ota_builds.Audit()
        check_ota_builds.check_version_contract(
            audit, "0.99.93+fv2o1", "+fv2ho1", "main"
        )
        self.assertEqual(
            audit.errors,
            ["main version '0.99.93+fv2o1' lacks '+fv2ho1'"],
        )

        audit = check_ota_builds.Audit()
        check_ota_builds.check_version_contract(
            audit, "0.99.93+fv2ho1", "+fv2ho1", "main"
        )
        self.assertEqual(audit.errors, [])


if __name__ == "__main__":
    unittest.main()
