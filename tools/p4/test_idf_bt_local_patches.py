#!/usr/bin/env python3
"""The local Bluetooth patches apply to the pinned ESP-IDF and say what they fix.

tools/p4/prepare_idf_bt.py stages ESP-IDF 5.5.5's `bt` component for P4 builds
with the upstream TinyCrypt patch and then each `local_patches` entry of
idf-bt-manifest.json. This test checks every local patch against the pinned
ESP-IDF install (IDF_PATH, else ~/esp/esp-idf):

1. The manifest loads (the loader checks each patch's hash, strip level, files
   and upstream status).
2. The patch applies to the pinned files at its recorded positions, with no
   fuzz and no offset, and the result has the hash pinned in patched_files.

and, for idf-bt-gap-deinit.patch, that every GATT callback GAP registers
checks gap_ble_cb_alive() before it reads the GAP control block. Without that
check, l2c_free() during esp_bluedroid_deinit() calls GAP's connect callback
after GAP_Deinit() freed gap_cb, which faulted the P4 on 2026-11-13.

Exit 77 (skipped) when the pinned ESP-IDF install is not on this machine.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import prepare_idf_bt  # noqa: E402


def fail(message: str) -> None:
    print(f"FAIL {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    manifest = prepare_idf_bt.load_manifest()
    local = manifest.get("local_patches", [])
    if not local:
        fail("idf-bt-manifest.json has no local_patches; the GAP deinit fix is missing")
    idf = Path(os.environ.get("IDF_PATH") or Path.home() / "esp/esp-idf")
    component = idf / "components/bt"
    if not component.is_dir():
        print(f"idf bt local patches: no ESP-IDF at {idf}; skipped")
        raise SystemExit(77)

    for entry in local:
        with tempfile.TemporaryDirectory(prefix="hw1-bt-patch-") as temp:
            stage = Path(temp)
            for relative in entry["files"]:
                source = component / relative
                pinned = manifest["baseline"]["files"][relative]
                if hashlib.sha256(source.read_bytes()).hexdigest() != pinned:
                    print(f"idf bt local patches: {source} is not the pinned ESP-IDF 5.5.5 file; skipped")
                    raise SystemExit(77)
                target = stage / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            result = subprocess.run(
                ["patch", "--verbose", "-f", "-F", "0", "-p", "3", "-i", str(HERE / entry["file"])],
                cwd=stage, capture_output=True, text=True, timeout=30, env={**os.environ, "LC_ALL": "C"})
            report = result.stdout + result.stderr
            if result.returncode or re.search(r"\b(?:fuzz|offset)\b", report, re.IGNORECASE):
                fail(f"{entry['file']} does not apply exactly to ESP-IDF 5.5.5:\n{report}")
            for relative in entry["files"]:
                got = hashlib.sha256((stage / relative).read_bytes()).hexdigest()
                if got != manifest["patched_files"][relative]:
                    fail(f"{entry['file']}: {relative} does not match its patched_files hash")
            if entry["file"] == "idf-bt-gap-deinit.patch":
                check_gap(stage / "host/bluedroid/stack/gap/gap_ble.c")
    print(f"idf bt local patches: {len(local)} local patch(es) apply exactly to ESP-IDF 5.5.5")


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if depth == 0:
            return source[open_brace:index + 1]
    fail(f"unbalanced body: {signature}")


def check_gap(path: Path) -> None:
    source = path.read_text()
    table = body(source, "static const tGATT_CBACK gap_cback =")
    callbacks = [name for name in re.findall(r"\b(gap_\w+)\b", table)]
    if len(callbacks) != 3:
        fail(f"expected GAP's three GATT callbacks in gap_cback, found {callbacks}")
    for name in callbacks:
        text = None
        for match in re.finditer(r"^(?:static\s+)?void\s+%s\s*\(" % name, source, re.M):
            close = source.index(")", match.end())
            if source[close + 1:].lstrip().startswith("{"):   # a definition, not a prototype
                text = body(source[match.start():], match.group(0))
                break
        if text is None:
            fail(f"no definition of {name}")
        guard = text.find("if (!gap_ble_cb_alive())")
        reads = [text.find(word) for word in ("gap_find_clcb_by_bd_addr", "gap_ble_find_clcb_by_conn_id",
                                               "gap_cb.") if word in text]
        if guard < 0:
            fail(f"{name} does not check gap_ble_cb_alive()")
        if reads and min(reads) < guard:
            fail(f"{name} reads the GAP control block before checking gap_ble_cb_alive()")


if __name__ == "__main__":
    main()
