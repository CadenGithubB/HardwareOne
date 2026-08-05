from __future__ import annotations

import argparse
import email.message
import hashlib
import json
import pathlib
import tempfile
import unittest

from tools.ota import device_backup


class BackupClient:
    def __init__(
        self,
        entries: (
            list[dict[str, object]]
            | dict[str, list[dict[str, object]]]
        ),
        contents: dict[str, bytes],
    ):
        self.username = "owner"
        self.entries = {"/": entries} if isinstance(entries, list) else entries
        self.contents = contents
        self.read_calls: list[str] = []

    def list_dir(self, path: str) -> list[dict[str, object]]:
        try:
            return self.entries[path]
        except KeyError as exc:
            raise AssertionError(f"unexpected directory: {path}") from exc

    def read_file(self, path: str) -> bytes:
        self.read_calls.append(path)
        return self.contents[path]


class RestoreClient:
    def __init__(self, username: str):
        self.username = username
        self.created: list[str] = []
        self.uploaded: list[str] = []
        self.read_calls: list[str] = []
        self.contents: dict[str, bytes] = {}

    def create_dir(self, path: str) -> None:
        self.created.append(path)

    def upload_file(self, path: str, content: bytes) -> None:
        self.uploaded.append(path)
        self.contents[path] = content

    def read_file(self, path: str) -> bytes:
        self.read_calls.append(path)
        if device_backup.USERS_JSON_PATH in self.contents:
            if not device_backup.restore_username_is_superadmin(
                self.contents[device_backup.USERS_JSON_PATH], self.username
            ):
                raise RuntimeError("active restore session lost authorization")
        return self.contents[path]


class InterruptingRestoreClient(RestoreClient):
    def __init__(
        self,
        username: str,
        *,
        fail_upload: str | None = None,
        fail_read: str | None = None,
    ):
        super().__init__(username)
        self.fail_upload = fail_upload
        self.fail_read = fail_read

    def upload_file(self, path: str, content: bytes) -> None:
        if path == self.fail_upload:
            raise RuntimeError(f"forced upload failure for {path}")
        super().upload_file(path, content)

    def read_file(self, path: str) -> bytes:
        if path == self.fail_read:
            self.read_calls.append(path)
            raise RuntimeError(f"forced readback failure for {path}")
        return super().read_file(path)


class FakeResponse:
    def __init__(self, content_type: str, body: bytes):
        self.headers = email.message.Message()
        self.headers["Content-Type"] = content_type
        self.body = body

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def read(self) -> bytes:
        return self.body


class FakeOpener:
    def __init__(self, response: FakeResponse):
        self.response = response

    def open(self, _request: object, timeout: float) -> FakeResponse:
        if timeout <= 0:
            raise AssertionError("timeout must be positive")
        return self.response


def args_for(directory: pathlib.Path) -> argparse.Namespace:
    return argparse.Namespace(directory=directory)


def write_restore_fixture(
    root: pathlib.Path,
    files: list[tuple[str, bytes]],
    directories: list[str] | None = None,
) -> None:
    root.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, object]] = []
    for path, content in files:
        relative = device_backup.safe_relative(path)
        local = root.joinpath(*relative.parts)
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(content)
        records.append(
            {
                "path": path,
                "sha256": hashlib.sha256(content).hexdigest(),
                "size": len(content),
            }
        )
    manifest = {
        "directories": directories or [],
        "files": records,
        "format": device_backup.FORMAT,
        "formatVersion": device_backup.FORMAT_VERSION,
    }
    (root / device_backup.MANIFEST_NAME).write_text(
        json.dumps(manifest), encoding="utf-8"
    )


def users_json(users: list[dict[str, object]]) -> bytes:
    return json.dumps({"users": users, "nextId": len(users) + 1}).encode("utf-8")


class BackupTests(unittest.TestCase):
    def test_each_legacy_http_200_error_body_is_rejected_on_size_collision(self) -> None:
        for body in device_backup.FILE_READ_ERROR_BODIES:
            with self.subTest(body=body), tempfile.TemporaryDirectory() as temporary:
                destination = pathlib.Path(temporary) / "backup"
                client = BackupClient(
                    [{"name": "record.txt", "type": "file", "size": f"{len(body)} bytes"}],
                    {"/record.txt": body},
                )
                with self.assertRaisesRegex(RuntimeError, "error body"):
                    device_backup.backup(args_for(destination), client)
                self.assertFalse((destination / "record.txt").exists())
                self.assertFalse((destination / device_backup.MANIFEST_NAME).exists())

    def test_size_mismatch_and_malformed_sizes_fail_before_archive(self) -> None:
        cases = [
            ("2 bytes", b"a", True),
            ("2 bytes", b"abc", True),
            ("unknown", b"abc", False),
            (-1, b"abc", False),
            (True, b"abc", False),
        ]
        for listed_size, body, should_read in cases:
            with self.subTest(size=listed_size, body=body):
                with tempfile.TemporaryDirectory() as temporary:
                    destination = pathlib.Path(temporary) / "backup"
                    client = BackupClient(
                        [{"name": "record", "type": "file", "size": listed_size}],
                        {"/record": body},
                    )
                    with self.assertRaises(RuntimeError):
                        device_backup.backup(args_for(destination), client)
                    self.assertEqual(bool(client.read_calls), should_read)
                    self.assertFalse((destination / "record").exists())

    def test_zero_and_binary_files_are_hashed_after_exact_size_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = pathlib.Path(temporary) / "backup"
            binary = b"\x00\xff\x01binary\x00"
            roster = users_json([{"username": "owner", "role": "superadmin"}])
            client = BackupClient(
                {
                    "/": [
                        {"name": "empty", "type": "file", "size": "0 bytes"},
                        {
                            "name": "binary",
                            "type": "file",
                            "size": f"{len(binary)} bytes",
                        },
                        {"name": "system", "type": "folder", "size": "1 item"},
                    ],
                    "/system": [
                        {"name": "users", "type": "folder", "size": "1 item"}
                    ],
                    "/system/users": [
                        {
                            "name": "users.json",
                            "type": "file",
                            "size": f"{len(roster)} bytes",
                        }
                    ],
                },
                {
                    "/empty": b"",
                    "/binary": binary,
                    device_backup.USERS_JSON_PATH: roster,
                },
            )
            device_backup.backup(args_for(destination), client)
            manifest = json.loads(
                (destination / device_backup.MANIFEST_NAME).read_text(encoding="utf-8")
            )
            by_path = {record["path"]: record for record in manifest["files"]}
            self.assertEqual(by_path["/empty"]["size"], 0)
            self.assertEqual(by_path["/binary"]["size"], len(binary))
            self.assertEqual(
                by_path["/binary"]["sha256"], hashlib.sha256(binary).hexdigest()
            )
            self.assertEqual((destination / "binary").read_bytes(), binary)

    def test_backup_requires_a_valid_restorable_identity_before_manifest(self) -> None:
        rosters = [
            None,
            b"not json",
            users_json([{"username": "owner", "role": None}]),
            users_json([{"username": "different", "role": "superadmin"}]),
        ]
        for roster in rosters:
            with self.subTest(roster=roster):
                with tempfile.TemporaryDirectory() as temporary:
                    destination = pathlib.Path(temporary) / "backup"
                    listings: dict[str, list[dict[str, object]]] = {
                        "/": [
                            {"name": "settings.json", "type": "file", "size": "2 bytes"}
                        ]
                    }
                    contents = {"/settings.json": b"{}"}
                    if roster is not None:
                        listings["/"].append(
                            {"name": "system", "type": "folder", "size": "1 item"}
                        )
                        listings["/system"] = [
                            {"name": "users", "type": "folder", "size": "1 item"}
                        ]
                        listings["/system/users"] = [
                            {
                                "name": "users.json",
                                "type": "file",
                                "size": f"{len(roster)} bytes",
                            }
                        ]
                        contents[device_backup.USERS_JSON_PATH] = roster
                    client = BackupClient(listings, contents)
                    with self.assertRaises(RuntimeError):
                        device_backup.backup(args_for(destination), client)
                    self.assertFalse(
                        (destination / device_backup.MANIFEST_NAME).exists()
                    )

    def test_read_file_requires_the_server_success_content_type_marker(self) -> None:
        client = object.__new__(device_backup.DeviceClient)
        client.base_url = "http://device.invalid"
        client.timeout = 1.0
        client.opener = FakeOpener(FakeResponse("text/plain", b"File not found"))
        with self.assertRaisesRegex(RuntimeError, "unexpected response type"):
            client.read_file("/missing")

        content = b"\x00\xffraw file"
        client.opener = FakeOpener(
            FakeResponse("text/plain; charset=utf-8", content)
        )
        self.assertEqual(client.read_file("/binary"), content)

    def test_login_username_is_trimmed_like_the_device_handler(self) -> None:
        self.assertEqual(device_backup.normalized_login_username(" owner \r\n"), "owner")
        with self.assertRaises(ValueError):
            device_backup.normalized_login_username(" \t\r\n")


class RestoreTests(unittest.TestCase):
    def test_users_database_is_restored_last_and_read_back(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "backup"
            roster = users_json([{"username": "owner", "role": "superadmin"}])
            # Intentionally put users.json first in the manifest.
            write_restore_fixture(
                source,
                [
                    (device_backup.USERS_JSON_PATH, roster),
                    ("/zzz.txt", b"restored last alphabetically, but before users"),
                ],
                ["/system", "/system/users"],
            )
            client = RestoreClient("owner")
            device_backup.restore(args_for(source), client)
            self.assertEqual(client.uploaded[-1], device_backup.USERS_JSON_PATH)
            self.assertEqual(client.read_calls[-1], device_backup.USERS_JSON_PATH)
            self.assertEqual(client.contents[device_backup.USERS_JSON_PATH], roster)

    def test_different_bootstrap_username_fails_before_device_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "backup"
            write_restore_fixture(
                source,
                [
                    (
                        device_backup.USERS_JSON_PATH,
                        users_json([{"username": "owner", "role": "superadmin"}]),
                    ),
                    ("/settings.json", b"{}"),
                ],
                ["/system"],
            )
            client = RestoreClient("temporary-owner")
            with self.assertRaisesRegex(RuntimeError, "revoke active user"):
                device_backup.restore(args_for(source), client)
            self.assertEqual(client.created, [])
            self.assertEqual(client.uploaded, [])

    def test_role_resolution_matches_safe_explicit_and_legacy_cases(self) -> None:
        explicit = users_json(
            [
                {"username": "admin", "role": "admin"},
                {"username": "owner", "role": "superadmin"},
            ]
        )
        self.assertTrue(device_backup.restore_username_is_superadmin(explicit, "owner"))
        self.assertFalse(device_backup.restore_username_is_superadmin(explicit, "admin"))

        legacy_no_role = users_json([{"username": "owner"}])
        legacy_admin = users_json([{"username": "owner", "role": "admin"}])
        unsafe_legacy_user = users_json([{"username": "owner", "role": "user"}])
        self.assertTrue(
            device_backup.restore_username_is_superadmin(legacy_no_role, "owner")
        )
        self.assertTrue(device_backup.restore_username_is_superadmin(legacy_admin, "owner"))
        self.assertFalse(
            device_backup.restore_username_is_superadmin(unsafe_legacy_user, "owner")
        )
        with self.assertRaisesRegex(RuntimeError, "invalid explicit role"):
            device_backup.restore_username_is_superadmin(
                users_json([{"username": "owner", "role": None}]), "owner"
            )
        with self.assertRaisesRegex(RuntimeError, "role before username"):
            device_backup.restore_username_is_superadmin(
                b'{"users":[{"role":"superadmin","username":"owner"}]}',
                "owner",
            )

    def test_missing_or_ambiguous_identity_backup_fails_before_mutation(self) -> None:
        fixtures = [
            [("/settings.json", b"{}")],
            [(device_backup.USERS_JSON_PATH, b"not json")],
            [
                (
                    device_backup.USERS_JSON_PATH,
                    users_json(
                        [
                            {"username": "owner", "role": "superadmin"},
                            {"username": "owner", "role": "superadmin"},
                        ]
                    ),
                )
            ],
        ]
        for files in fixtures:
            with self.subTest(files=[path for path, _ in files]):
                with tempfile.TemporaryDirectory() as temporary:
                    source = pathlib.Path(temporary) / "backup"
                    write_restore_fixture(source, files, ["/system"])
                    client = RestoreClient("owner")
                    with self.assertRaises(RuntimeError):
                        device_backup.restore(args_for(source), client)
                    self.assertEqual(client.created, [])
                    self.assertEqual(client.uploaded, [])

    def test_duplicate_manifest_path_fails_before_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "duplicate"
            write_restore_fixture(
                source,
                [
                    (
                        device_backup.USERS_JSON_PATH,
                        users_json([{"username": "owner", "role": "superadmin"}]),
                    )
                ],
            )
            manifest_path = source / device_backup.MANIFEST_NAME
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["files"].append(dict(manifest["files"][0]))
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            client = RestoreClient("owner")
            with self.assertRaises(SystemExit):
                device_backup.restore(args_for(source), client)
            self.assertEqual(client.created, [])
            self.assertEqual(client.uploaded, [])

    def test_all_directories_are_validated_before_device_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "backup"
            write_restore_fixture(
                source,
                [
                    (
                        device_backup.USERS_JSON_PATH,
                        users_json([{"username": "owner", "role": "superadmin"}]),
                    )
                ],
                ["/valid", "//invalid"],
            )
            client = RestoreClient("owner")
            with self.assertRaisesRegex(ValueError, "noncanonical"):
                device_backup.restore(args_for(source), client)
            self.assertEqual(client.created, [])
            self.assertEqual(client.uploaded, [])

    def test_upload_and_final_users_readback_interruptions_propagate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "backup"
            roster = users_json([{"username": "owner", "role": "superadmin"}])
            write_restore_fixture(
                source,
                [
                    ("/settings.json", b"{}"),
                    (device_backup.USERS_JSON_PATH, roster),
                ],
            )

            upload_failure = InterruptingRestoreClient(
                "owner", fail_upload="/settings.json"
            )
            with self.assertRaisesRegex(RuntimeError, "forced upload failure"):
                device_backup.restore(args_for(source), upload_failure)
            self.assertNotIn(device_backup.USERS_JSON_PATH, upload_failure.uploaded)

            read_failure = InterruptingRestoreClient(
                "owner", fail_read=device_backup.USERS_JSON_PATH
            )
            with self.assertRaisesRegex(RuntimeError, "forced readback failure"):
                device_backup.restore(args_for(source), read_failure)
            self.assertEqual(read_failure.uploaded[-1], device_backup.USERS_JSON_PATH)
            self.assertEqual(read_failure.read_calls[-1], device_backup.USERS_JSON_PATH)

    def test_noncanonical_manifest_path_fails_before_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "alias"
            source.mkdir(parents=True)
            content = users_json([{"username": "owner", "role": "superadmin"}])
            manifest = {
                "directories": [],
                "files": [
                    {
                        "path": "//system/users/users.json",
                        "sha256": hashlib.sha256(content).hexdigest(),
                        "size": len(content),
                    }
                ],
                "format": device_backup.FORMAT,
                "formatVersion": device_backup.FORMAT_VERSION,
            }
            (source / device_backup.MANIFEST_NAME).write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            client = RestoreClient("owner")
            with self.assertRaisesRegex(ValueError, "noncanonical"):
                device_backup.restore(args_for(source), client)
            self.assertEqual(client.created, [])
            self.assertEqual(client.uploaded, [])


if __name__ == "__main__":
    unittest.main()
