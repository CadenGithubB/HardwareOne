from __future__ import annotations

import json
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile

from tools.ota import make_bundle, make_manifest


@unittest.skipUnless(shutil.which("openssl"), "openssl is required")
class OtaBundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = pathlib.Path(cls.temporary.name)
        cls.key = cls.root / "lab-key.pem"
        generated = subprocess.run(
            [
                "openssl",
                "genpkey",
                "-algorithm",
                "RSA",
                "-pkeyopt",
                "rsa_keygen_bits:3072",
                "-out",
                str(cls.key),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if generated.returncode != 0:
            raise RuntimeError(generated.stderr)
        cls.public_key = cls.root / "lab-public-key.pem"
        make_manifest.extract_public_key(cls.key, cls.public_key)
        cls.image = cls.root / "firmware.bin"
        image = bytearray(4096)
        descriptor = make_manifest.APP_DESC_OFFSET
        struct.pack_into("<I", image, descriptor, make_manifest.APP_DESC_MAGIC)
        version = b"9.9.9+f3o1"
        project = b"hardwareone-idf"
        image[descriptor + 16 : descriptor + 16 + len(version)] = version
        image[descriptor + 48 : descriptor + 48 + len(project)] = project
        for index in range(descriptor + 256, len(image)):
            image[index] = index % 251
        cls.image.write_bytes(image)
        cls.manifest = cls.root / "manifest.json"
        make_manifest.write_envelope(
            {
                "boardId": "feathers3",
                "layoutId": "hw1-f3-ota-v1",
                "projectName": "hardwareone-idf",
                "version": "9.9.9+f3o1",
                "imageSize": len(image),
                "imageSha256": make_manifest.sha256_file(cls.image),
                "minUpdaterVersion": "1.0.0",
                "dataSchema": 1,
            },
            cls.key,
            cls.manifest,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_bundle_is_deterministic_strict_and_self_describing(self) -> None:
        first = self.root / "first.hw1ota"
        second = self.root / "second.hw1ota"
        one = make_bundle.create_bundle(
            self.image, self.manifest, self.public_key, first
        )
        two = make_bundle.create_bundle(
            self.image, self.manifest, self.public_key, second
        )
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(one["bundleSha256"], two["bundleSha256"])
        inspected = make_bundle.inspect_bundle(first)
        self.assertEqual(inspected["version"], "9.9.9+f3o1")
        self.assertEqual(inspected["board"], "feathers3")
        with zipfile.ZipFile(first) as archive:
            self.assertEqual(
                tuple(info.filename for info in archive.infolist()),
                make_bundle.BUNDLE_MEMBERS,
            )
            metadata = json.loads(archive.read(make_bundle.BUNDLE_METADATA))
            self.assertEqual(metadata["format"], make_bundle.BUNDLE_FORMAT)
            self.assertTrue(
                all(info.compress_type == zipfile.ZIP_STORED for info in archive.infolist())
            )

    def test_bundle_rejects_mismatch_and_unexpected_members(self) -> None:
        damaged = self.root / "damaged.bin"
        value = bytearray(self.image.read_bytes())
        value[-1] ^= 1
        damaged.write_bytes(value)
        with self.assertRaisesRegex(ValueError, "mismatch"):
            make_bundle.create_bundle(
                damaged,
                self.manifest,
                self.public_key,
                self.root / "damaged.hw1ota",
            )

        extra = self.root / "extra.hw1ota"
        make_bundle.create_bundle(
            self.image, self.manifest, self.public_key, extra
        )
        with zipfile.ZipFile(extra, "a", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("release-url.txt", "https://example.invalid")
        with self.assertRaisesRegex(ValueError, "exactly"):
            make_bundle.inspect_bundle(extra)

    def test_cli_inspect_emits_machine_readable_identity(self) -> None:
        bundle = self.root / "cli.hw1ota"
        make_bundle.create_bundle(
            self.image, self.manifest, self.public_key, bundle
        )
        result = subprocess.run(
            [
                "python3",
                "tools/ota/make_bundle.py",
                "inspect",
                str(bundle),
            ],
            cwd=pathlib.Path(__file__).resolve().parents[3],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["imageSize"], 4096)


if __name__ == "__main__":
    unittest.main()
