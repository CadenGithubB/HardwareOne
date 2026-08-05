#!/usr/bin/env python3
"""File-level backup/restore for the one-time HardwareOne OTA migration.

The partition-layout migration relocates LittleFS.  This tool copies files
through the firmware's authenticated VFS endpoints so the backup is independent
of LittleFS's old partition size.  NVS is intentionally not touched by the
migration flash procedure.
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import json
import os
import pathlib
import re

try:
    from .device_http import (
        DeviceClient,
        FILE_RESPONSE_CHARSET,
        FILE_RESPONSE_CONTENT_TYPE,
        normalized_login_username,
    )
except ImportError:  # Direct `python tools/ota/device_backup.py` execution.
    from device_http import (  # type: ignore[no-redef]
        DeviceClient,
        FILE_RESPONSE_CHARSET,
        FILE_RESPONSE_CONTENT_TYPE,
        normalized_login_username,
    )


MANIFEST_NAME = "hw1-file-backup.json"
FORMAT = "hardwareone-file-backup"
FORMAT_VERSION = 1
USERS_JSON_PATH = "/system/users/users.json"
FILE_READ_ERROR_BODIES = frozenset(
    {
        b"Filesystem not initialized",
        b"No filename specified",
        b"Invalid filename",
        b"File not found",
    }
)


def safe_relative(device_path: str) -> pathlib.PurePosixPath:
    if not device_path.startswith("/") or "\0" in device_path:
        raise ValueError(f"invalid device path: {device_path!r}")
    relative = pathlib.PurePosixPath(device_path.lstrip("/"))
    if not relative.parts or ".." in relative.parts:
        raise ValueError(f"unsafe device path: {device_path!r}")
    if "/" + relative.as_posix() != device_path:
        raise ValueError(f"noncanonical device path: {device_path!r}")
    return relative


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def listed_file_size(entry: dict[str, object], path: str) -> int:
    """Return the byte count emitted by the current directory-listing API."""
    raw_size = entry.get("size")
    if isinstance(raw_size, int) and not isinstance(raw_size, bool):
        if raw_size >= 0:
            return raw_size
    elif isinstance(raw_size, str):
        match = re.fullmatch(r"([0-9]+) bytes", raw_size)
        if match:
            return int(match.group(1))
    raise RuntimeError(f"malformed file size for {path}: {raw_size!r}")


def validate_file_read(path: str, content: bytes, expected_size: int) -> None:
    """Reject legacy HTTP-200 errors and files changed after their listing."""
    if content in FILE_READ_ERROR_BODIES:
        raise RuntimeError(
            f"device returned an error body while reading {path}: "
            f"{content.decode('ascii')!r}"
        )
    if len(content) != expected_size:
        raise RuntimeError(
            f"downloaded size mismatch for {path}: "
            f"listing={expected_size}, response={len(content)}"
        )


def restore_username_is_superadmin(users_json: bytes, username: str) -> bool:
    """Mirror the firmware's explicit-super/legacy-owner role resolution."""
    try:
        document = json.loads(users_json.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"backup {USERS_JSON_PATH} is malformed: {exc}") from exc

    users = document.get("users") if isinstance(document, dict) else None
    if not isinstance(users, list) or not users:
        raise RuntimeError(f"backup {USERS_JSON_PATH} has no non-empty users array")

    parsed_users: list[tuple[str, bool, object]] = []
    seen_usernames: set[str] = set()
    for item in users:
        if not isinstance(item, dict) or not isinstance(item.get("username"), str):
            raise RuntimeError(f"backup {USERS_JSON_PATH} has a malformed user entry")
        item_username = item["username"]
        if not item_username or item_username in seen_usernames:
            raise RuntimeError(
                f"backup {USERS_JSON_PATH} has an empty or duplicate username"
            )
        seen_usernames.add(item_username)

        role_present = "role" in item
        role = item.get("role")
        if role_present:
            keys = list(item)
            if keys.index("role") < keys.index("username"):
                raise RuntimeError(
                    f"backup {USERS_JSON_PATH} puts role before username; "
                    "the current firmware's role parser would not authorize it"
                )
            if role not in ("guest", "user", "admin", "superadmin"):
                raise RuntimeError(
                    f"backup {USERS_JSON_PATH} has an invalid explicit role "
                    f"for {item_username!r}"
                )
        parsed_users.append((item_username, role_present, role))

    explicit_supers = {
        item_username
        for item_username, role_present, role in parsed_users
        if role_present and role == "superadmin"
    }
    if explicit_supers:
        return username in explicit_supers

    # With no explicit superadmin, current firmware treats the first user as the
    # legacy superadmin fallback. The file endpoints also call isAdminUser(), so
    # that owner must have role=admin or omit the pre-role-era field entirely.
    owner_username, owner_role_present, owner_role = parsed_users[0]
    return username == owner_username and (
        not owner_role_present or owner_role == "admin"
    )


def join_device_path(parent: str, child: str) -> str:
    if "/" in child or child in ("", ".", ".."):
        raise ValueError(f"unsafe directory entry: {child!r}")
    return "/" + child if parent == "/" else parent.rstrip("/") + "/" + child


def backup(args: argparse.Namespace, client: DeviceClient) -> None:
    destination = args.directory.resolve()
    if destination.exists() and any(destination.iterdir()):
        raise SystemExit(f"backup directory is not empty: {destination}")
    destination.mkdir(parents=True, exist_ok=True)

    queue = ["/"]
    directories: list[str] = []
    records: list[dict[str, object]] = []
    seen_paths: set[str] = set()
    while queue:
        directory = queue.pop()
        for entry in client.list_dir(directory):
            name = entry.get("name")
            kind = entry.get("type")
            if not isinstance(name, str) or kind not in ("file", "folder"):
                raise RuntimeError(f"malformed entry below {directory}: {entry!r}")
            path = join_device_path(directory, name)
            if path == "/sd" or path.startswith("/sd/"):
                continue
            if path == "/system/ota" or path.startswith("/system/ota/"):
                continue
            if path in seen_paths:
                raise RuntimeError(f"duplicate path in device listing: {path}")
            seen_paths.add(path)
            if kind == "folder":
                directories.append(path)
                queue.append(path)
                continue

            expected_size = listed_file_size(entry, path)
            relative = safe_relative(path)
            content = client.read_file(path)
            validate_file_read(path, content, expected_size)
            local = destination.joinpath(*relative.parts)
            local.parent.mkdir(parents=True, exist_ok=True)
            local.write_bytes(content)
            stored = local.read_bytes()
            content_digest = digest(content)
            if len(stored) != expected_size or digest(stored) != content_digest:
                raise RuntimeError(f"local backup verification failed for {path}")
            record = {
                "path": path,
                "sha256": content_digest,
                "size": expected_size,
            }
            records.append(record)
            print(f"saved {path} ({expected_size} bytes)")

    records.sort(key=lambda item: str(item["path"]))
    directories.sort()
    users_records = [record for record in records if record["path"] == USERS_JSON_PATH]
    if len(users_records) != 1:
        raise RuntimeError(
            f"backup requires exactly one canonical {USERS_JSON_PATH}; "
            f"found {len(users_records)}"
        )
    users_relative = safe_relative(USERS_JSON_PATH)
    users_content = destination.joinpath(*users_relative.parts).read_bytes()
    if not restore_username_is_superadmin(users_content, client.username):
        raise RuntimeError(
            f"backup login user {client.username!r} would not remain a "
            "superadmin after restoring the archived roster"
        )
    manifest = {
        "directories": directories,
        "files": records,
        "format": FORMAT,
        "formatVersion": FORMAT_VERSION,
    }
    (destination / MANIFEST_NAME).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"backup complete: {len(records)} files in {destination}")


def restore(args: argparse.Namespace, client: DeviceClient) -> None:
    source = args.directory.resolve()
    manifest_path = source / MANIFEST_NAME
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("format") != FORMAT or manifest.get("formatVersion") != FORMAT_VERSION:
        raise SystemExit("unsupported or malformed backup manifest")
    records = manifest.get("files")
    if not isinstance(records, list):
        raise SystemExit("backup manifest has no files array")
    directories = manifest.get("directories", [])
    if not isinstance(directories, list) or not all(
        isinstance(path, str) for path in directories
    ):
        raise SystemExit("backup manifest has a malformed directories array")

    verified: list[tuple[str, bytes]] = []
    seen_paths: set[str] = set()
    for record in records:
        if not isinstance(record, dict) or not isinstance(record.get("path"), str):
            raise SystemExit("malformed file record in backup manifest")
        path = str(record["path"])
        if path in seen_paths:
            raise SystemExit(f"duplicate file record in backup manifest: {path}")
        seen_paths.add(path)
        relative = safe_relative(path)
        content = source.joinpath(*relative.parts).read_bytes()
        if len(content) != record.get("size") or digest(content) != record.get("sha256"):
            raise SystemExit(f"backup file failed size/hash verification: {path}")
        verified.append((path, content))

    validated_directories: list[str] = []
    seen_directories: set[str] = set()
    for path in directories:
        safe_relative(path)
        if path in seen_directories:
            raise SystemExit(f"duplicate directory in backup manifest: {path}")
        if path in seen_paths:
            raise SystemExit(f"file/directory collision in backup manifest: {path}")
        seen_directories.add(path)
        validated_directories.append(path)

    users_record = next(
        (content for path, content in verified if path == USERS_JSON_PATH),
        None,
    )
    if users_record is None:
        raise RuntimeError(
            f"backup has no canonical {USERS_JSON_PATH}; refusing a partial "
            "identity restore"
        )
    if not restore_username_is_superadmin(users_record, client.username):
        raise RuntimeError(
            f"restore would replace {USERS_JSON_PATH} and revoke active "
            f"user {client.username!r}; rerun first-time setup with a username "
            "that is a superadmin in the backup (the first/owner user for a "
            "legacy roster)"
        )

    # Keep the authentication database until last. All other files are then
    # durably uploaded and read-back verified before the active role roster is
    # replaced. The preflight above guarantees this session's username remains
    # a superadmin for the final users.json readback.
    verified.sort(key=lambda item: item[0] == USERS_JSON_PATH)

    # Parents first; this also preserves directories which contained no files.
    for path in sorted(
        validated_directories, key=lambda value: (value.count("/"), value)
    ):
        client.create_dir(path)
        print(f"created {path}")

    for path, content in verified:
        client.upload_file(path, content)
        restored = client.read_file(path)
        validate_file_read(path, restored, len(content))
        if digest(restored) != digest(content):
            raise RuntimeError(f"post-restore verification failed: {path}")
        print(f"restored {path} ({len(content)} bytes)")
    print(f"restore complete: {len(verified)} files")


def password_from_args(args: argparse.Namespace) -> str:
    if args.password_env:
        value = os.environ.get(args.password_env)
        if value is None:
            raise SystemExit(f"environment variable {args.password_env} is not set")
        return value
    return getpass.getpass("HardwareOne password: ")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("--url", required=True, help="device base URL, e.g. http://192.168.1.20")
    root.add_argument("--username", required=True, help="super-admin account")
    root.add_argument(
        "--password-env",
        metavar="NAME",
        help="read password from environment variable NAME instead of prompting",
    )
    root.add_argument("--timeout", type=float, default=120.0)
    root.add_argument(
        "--insecure",
        action="store_true",
        help="allow a self-signed HTTPS certificate (local migration only)",
    )
    sub = root.add_subparsers(dest="command", required=True)
    save = sub.add_parser("backup")
    save.add_argument("directory", type=pathlib.Path)
    save.set_defaults(func=backup)
    load = sub.add_parser("restore")
    load.add_argument("directory", type=pathlib.Path)
    load.set_defaults(func=restore)
    return root


def main() -> None:
    args = parser().parse_args()
    client = DeviceClient(
        args.url,
        args.username,
        password_from_args(args),
        args.timeout,
        args.insecure,
    )
    args.func(args, client)


if __name__ == "__main__":
    main()
