#!/usr/bin/env python3
"""A refusal at HardwareOne's restart TX fence is silent on the producer's task.

Bench 2026-11-08 (P4X-EYE, G2 and R1 connected): 3 of 15 reboots panicked in
Bluedroid's hciT task (stack protection fault) after the restart closed
Hosted TX at the fence. Every HCI packet hciT sent was refused in
esp_hosted_tx, whose ESP_LOGE ran vfprintf on hciT's 2.5 KB stack.

The runner applies the Hosted patch (default: the tracked
esp-hosted-2.12.13-hw1.patch; --patch selects another, e.g. an older one that
must fail) to the pristine package's files, then compiles the patched fence
and helpers, the STA/AP/serial wrappers, esp_hosted_tx (SDIO) and
hosted_hci_bluedroid_send unchanged against esp_hosted_tx_fence_harness.c.
Exit 77 (skipped) when no pristine Hosted 2.12.13 package is available.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import prepare_esp_hosted as prepare
from test_esp_hosted_mempool import function

HERE = Path(__file__).resolve().parent
TRANSPORT = "host/drivers/transport/transport_drv.c"
HEADER = "host/drivers/transport/transport_drv.h"
SDIO = "host/drivers/transport/sdio/sdio_drv.c"
VHCI = "host/drivers/bt/vhci_drv.c"


def block(source: str, pattern: str, what: str) -> str:
    match = re.search(pattern, source, re.S | re.M)
    if not match:
        raise SystemExit(f"FAIL: {what} not found")
    return match.group(0) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="pristine Hosted 2.12.13 package")
    parser.add_argument("--patch", type=Path, default=prepare.PATCH)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("clang") or shutil.which("cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    try:
        source = prepare.find_source(args.source)
    except ValueError as exc:
        print(f"SKIP: {exc}")
        return 77
    patch_text = args.patch.read_text()
    touched = re.findall(r"^\+\+\+ b/(\S+)", patch_text, re.M)
    with tempfile.TemporaryDirectory(prefix="hw1-hosted-fence-") as tmp:
        tree = Path(tmp) / "tree"
        for name in sorted(set(touched) | {TRANSPORT, HEADER, SDIO, VHCI}):
            (tree / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(source / name, tree / name)
        applied = subprocess.run(["patch", "-f", "-F", "0", "-p", "1"], input=patch_text.encode(),
                                 cwd=tree, capture_output=True)
        if applied.returncode:
            raise SystemExit("FAIL: patch: " + applied.stdout.decode() + applied.stderr.decode())

        transport = (tree / TRANSPORT).read_text()
        header = (tree / HEADER).read_text()
        types = block(header, r"^#define H_FREE_PTR_WITH_FUNC\(.*?\} while \(0\);$", "H_FREE_PTR_WITH_FUNC")
        types += block(header, r"^typedef struct \{\n\tunion \{\n\t\tvoid \*priv_buffer_handle;.*?"
                               r"\} interface_buffer_handle_t;$", "interface_buffer_handle_t")
        fence = "#define TAG \"transport\"\n"
        fence += "".join(re.findall(r"^static [^\n(]*\bhw1_tx_\w+ = 0;\n", transport, re.M))
        names = re.findall(r"^\w[\w ]*?\b(esp_hosted_hw1_\w+)\(void\)\n\{", transport, re.M)
        if "esp_hosted_hw1_fence_tx" not in names or "esp_hosted_hw1_tx_open" not in names:
            raise SystemExit("FAIL: the patch defines no restart TX fence")
        fence += "".join(function(transport, name) for name in names)
        for name in ("transport_drv_sta_tx", "transport_drv_ap_tx", "transport_drv_serial_tx"):
            fence += function(transport, name)
        fence += "#undef TAG\n#define TAG \"H_SDIO_DRV\"\n"
        fence += function((tree / SDIO).read_text(), "esp_hosted_tx")
        fence += "#undef TAG\n#define TAG \"vhci_drv\"\n"
        fence += function((tree / VHCI).read_text(), "hosted_hci_bluedroid_send")
        fence += "#undef TAG\n"

        harness = (HERE / "esp_hosted_tx_fence_harness.c").read_text()
        for marker, code in (("/*@TYPES@*/", types), ("/*@PRODUCTION@*/", fence)):
            assert harness.count(marker) == 1, marker
            harness = harness.replace(marker, code)
        unit = Path(tmp) / "fence.c"
        unit.write_text(harness)
        exe = Path(tmp) / "fence"
        command = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-gnu-zero-variadic-macro-arguments",
                   str(unit), "-o", str(exe)]
        if "esp_hosted_hw1_tx_fence_drops" in names:
            command.insert(1, "-DHAVE_FENCE_DROPS=1")
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        run = subprocess.run([str(exe)])
        if run.returncode:
            return run.returncode
        # The drop count is what the restart reports in place of the logs.
        if "esp_hosted_hw1_tx_fence_drops" not in names:
            raise SystemExit("FAIL: the patch keeps no count of the packets dropped at the fence")
    print("PASS: the restart TX fence drops silently on every producer; other refusals still log")
    return 0


if __name__ == "__main__":
    sys.exit(main())
