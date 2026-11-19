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

and, per patch:

- idf-bt-gap-deinit.patch: every GATT callback GAP registers checks
  gap_ble_cb_alive() before it reads the GAP control block. Without that
  check, l2c_free() during esp_bluedroid_deinit() calls GAP's connect callback
  after GAP_Deinit() freed gap_cb, which faulted the P4 on 2026-11-13.
- idf-bt-failed-conn-handle.patch: btm_ble_conn_complete's failure branch,
  compiled unchanged into idf_bt_failed_conn_harness.c, no longer passes a
  handle that names a live link to l2c_link_hci_disc_comp/btm_sec_disconnected
  (the pristine branch must still show the bug, so the harness is live).
  Bench case: the R1 ring's direct connect hit Bluedroid's 30 s timeout, the
  C6 answered the cancel with status 0x02 on handle 0x0000, and the G2 LEFT
  arm on handle 0 was released in the host only.
- idf-bt-unknown-handle-acl.patch: the unknown-handle branch of
  l2c_rcv_acl_data keeps the INFO/CONN hold path first and logs through the
  rate limit; the helpers, compiled unchanged into
  idf_bt_unknown_handle_harness.c, send HCI_Disconnect for a handle that keeps
  sending (3 packets in 1 s, or seen again 2 s or more after its first packet,
  as the bench ghost's one parameter request every ~39 s; unknown to BTM) at
  most once per 5 s, with no link-setup gate that a hung connect could hold
  shut, and log the first and every 100th packet.

The harnesses need a host C compiler (CC, else clang or cc; on macOS export
SDKROOT=$(xcrun --show-sdk-path)). Exit 77 (skipped) when the pinned ESP-IDF
install is not on this machine.
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
BTM_BLE = "host/bluedroid/stack/btm/btm_ble.c"
L2C_MAIN = "host/bluedroid/stack/l2cap/l2c_main.c"
sys.path.insert(0, str(HERE))
import prepare_idf_bt  # noqa: E402


def fail(message: str) -> None:
    print(f"FAIL {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    manifest = prepare_idf_bt.load_manifest()
    local = manifest.get("local_patches", [])
    expected = {"idf-bt-gap-deinit.patch", "idf-bt-failed-conn-handle.patch",
                "idf-bt-unknown-handle-acl.patch"}
    missing = expected - {entry["file"] for entry in local}
    if missing:
        fail(f"idf-bt-manifest.json local_patches lacks {sorted(missing)}")
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
            elif entry["file"] == "idf-bt-failed-conn-handle.patch":
                check_failed_conn(component / BTM_BLE, stage / BTM_BLE)
            elif entry["file"] == "idf-bt-unknown-handle-acl.patch":
                check_unknown_handle(stage / L2C_MAIN)
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


def compiler() -> str:
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        fail("no host C compiler (set CC) for the Bluetooth patch harnesses")
    return cc


def run_harness(harness: str, production: str, what: str) -> str:
    template = (HERE / harness).read_text()
    if template.count("/* PRODUCTION */") != 1:
        fail(f"{harness} needs exactly one PRODUCTION marker")
    with tempfile.TemporaryDirectory(prefix="hw1-bt-harness-") as temp:
        source = Path(temp) / "harness.c"
        binary = Path(temp) / "harness"
        source.write_text(template.replace("/* PRODUCTION */", production))
        built = subprocess.run([compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                                "-Wno-unused-function", "-o", str(binary), str(source)],
                               capture_output=True, text=True, timeout=120)
        if built.returncode:
            fail(f"{what}: harness does not compile:\n{built.stdout}{built.stderr}")
        ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        if ran.returncode:
            fail(f"{what}: harness failed:\n{ran.stdout}{ran.stderr}")
        return ran.stdout


def failure_branch(path: Path) -> str:
    # The function's #if/#else arms open two braces for one close, so cut at
    # the function's closing brace in column 0 instead of counting braces.
    source = path.read_text()
    function = source.find("void btm_ble_conn_complete(UINT8 *p, UINT16 evt_len, BOOLEAN enhanced)\n{")
    start = source.find("    BOOLEAN bg_con = btm_ble_update_mode_operation(role, bda, status);", function)
    end = source.find("\n}\n", start)
    if function < 0 or start < 0 or end < 0 or source.find("\n}\n", function) != end:
        fail(f"{path.name}: btm_ble_conn_complete does not end with the bg_con failure branch")
    return source[start:end + 1]


def branch_results(output: str) -> dict[str, tuple[int, int, int]]:
    results = {}
    for line in output.splitlines():
        match = re.fullmatch(r"(\w+) disc=(-?\d+) sec=(-?\d+) warn=(\d+)", line)
        if match:
            results[match.group(1)] = tuple(int(value) for value in match.groups()[1:])
    return results


def check_failed_conn(pristine: Path, patched: Path) -> None:
    before = branch_results(run_harness("idf_bt_failed_conn_harness.c", failure_branch(pristine),
                                        "pristine btm_ble_conn_complete"))
    if before.get("live_handle0") != (0, 0, 0):
        fail(f"harness no longer reproduces the stock bug (live handle released): {before}")
    after = branch_results(run_harness("idf_bt_failed_conn_harness.c", failure_branch(patched),
                                       "patched btm_ble_conn_complete"))
    expected = {
        "live_handle0": (0xFFFE, 0xFFFE, 1),   # the live LEFT arm is left alone, one warning
        "sec_only": (0xFFFE, 0xFFFE, 1),       # a security record alone also counts as live
        "no_live": (0, 0, 0),                  # stock behaviour when no link holds the handle
        "invalid_handle": (0xFFFF, 0xFFFF, 0), # a connecting LCB is still found as before
        "bg_con": (-1, -1, 0),
        "success": (-1, -1, 0),
    }
    if after != expected:
        fail(f"btm_ble_conn_complete failure branch: expected {expected}, got {after}")


def check_unknown_handle(path: Path) -> None:
    source = path.read_text()
    receive = body(source, "void l2c_rcv_acl_data (BT_HDR *p_msg)")
    hold = receive.find("L2CAP - holding ACL for unknown handle")
    track = receive.find("UINT32 dropped = hw1_l2c_unknown_handle_acl(handle);")
    gate = receive.find("if (dropped) {", track)
    log = receive.find("L2CAP - rcvd ACL for unknown handle")
    if min(hold, track, gate, log) < 0 or not hold < track < gate < log:
        fail("l2c_rcv_acl_data: unknown-handle data must pass the hold path, then "
             "hw1_l2c_unknown_handle_acl, then the rate-limited log")
    if "memset (hw1_ghost_handles, 0, sizeof (hw1_ghost_handles));" not in body(source, "void l2c_init (void)"):
        fail("l2c_init does not clear the unknown-handle slots")
    start = source.find("/* HardwareOne local patch (tools/p4/idf-bt-unknown-handle-acl.patch).")
    end_sig = "static UINT32 hw1_l2c_unknown_handle_acl(UINT16 handle)"
    if start < 0 or end_sig not in source:
        fail("l2c_main.c lacks the unknown-handle safety net")
    helper = body(source, end_sig)
    production = source[start:source.index(end_sig)] + end_sig + "\n" + helper + "\n"
    for constant, value in (("HW1_GHOST_BURST", "3"), ("HW1_GHOST_WINDOW_MS", "1000"),
                            ("HW1_GHOST_PERSIST_MS", "2000"), ("HW1_GHOST_DISC_GAP_MS", "5000"),
                            ("HW1_GHOST_LOG_EVERY", "100")):
        if not re.search(r"^#define %s\s+%s\b" % (constant, value), production, re.M):
            fail(f"{constant} is not {value}")
    if "is_ble_connecting" in production or "link_setup_pending" in production:
        fail("the safety net must not wait for link setup: a hung connect would hold it shut")
    if "btsnd_hcic_disconnect(handle, HCI_ERR_PEER_USER);" not in helper:
        fail("the safety net does not send HCI_Disconnect with reason 0x13")
    output = run_harness("idf_bt_unknown_handle_harness.c", production, "unknown-handle safety net")
    if output.strip() != "ok":
        fail(f"unknown-handle safety net: {output}")


if __name__ == "__main__":
    main()
