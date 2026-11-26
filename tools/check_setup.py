#!/usr/bin/env python3
"""Check that this computer is ready to build and flash HardwareOne.

    python3 tools/check_setup.py                  # quick check, a few seconds
    python3 tools/check_setup.py --p4             # also the ESP32-P4 SDK staging
    python3 tools/check_setup.py --build xiao_s3  # also build one board (minutes)

It checks the setup steps in docs/QUICKSTART.md, not the firmware itself: the
host tools, ESP-IDF 5.5.5, the vendored Arduino component and its patches, the
compiler the guided setup uses, disk space and connected boards. Serial ports
are only listed, never opened, so a plugged-in board is not reset.

Every line is OK, WARN (works, but read the hint) or FAIL (a build will not
work until it is fixed). The exit status is 1 when anything failed.
"""
from __future__ import annotations

import argparse
import glob
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
IDF_VERSION = (5, 5, 5)
MIN_PYTHON = (3, 9)
MIN_FREE_GB = 8

failed = 0
warned = 0


def report(state, what, hint=""):
    global failed, warned
    if state == "FAIL":
        failed += 1
    elif state == "WARN":
        warned += 1
    print(f"  {state:<4}  {what}")
    if hint:
        for line in hint.splitlines():
            print(f"        {line}")


def run(argv, **kwargs):
    try:
        return subprocess.run(argv, capture_output=True, text=True, timeout=kwargs.pop("timeout", 120),
                              cwd=REPO, **kwargs)
    except (OSError, subprocess.TimeoutExpired) as error:
        return subprocess.CompletedProcess(argv, 127, "", str(error))


def last_line(result):
    text = (result.stdout + result.stderr).strip().splitlines()
    return text[-1] if text else ""


def check_checkout():
    print("Repository")
    needed = ["components/hardwareone", "boards", "config/sdkconfig.defaults",
              "tools/build_board.sh", "tools/vendor_arduino.sh"]
    missing = [name for name in needed if not (REPO / name).exists()]
    if missing:
        report("FAIL", "checkout is incomplete: missing " + ", ".join(missing),
               "Clone the whole repository again (git clone ...), not a single folder.")
    else:
        report("OK", f"checkout at {REPO}")
    if (REPO / "config/sdkconfig").exists():
        report("WARN", "config/sdkconfig exists (left by a bare idf.py run)",
               "No supported build reads it; delete it. Build with tools/build_board.sh <board>.")


def check_host_tools():
    print("Host tools")
    if sys.version_info[:2] >= MIN_PYTHON:
        report("OK", f"Python {platform.python_version()}")
    else:
        report("FAIL", f"Python {platform.python_version()} is too old",
               f"Install Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]} or newer.")
    for tool in ("git", "bash"):
        if shutil.which(tool):
            report("OK", tool)
        else:
            report("FAIL", f"{tool} not found", f"Install {tool}; the build scripts need it.")
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        hint = "Install Xcode Command Line Tools (xcode-select --install)." if sys.platform == "darwin" \
            else "Install a C++ compiler (for example g++)."
        report("FAIL", "no host C++ compiler (the guided setup in tools/build_config.py needs one)", hint)
        return
    with tempfile.TemporaryDirectory() as scratch:
        probe = Path(scratch) / "probe.cpp"
        probe.write_text("#include <cstdio>\nint main() { return 0; }\n")
        result = run([compiler, "-std=c++17", "-o", str(Path(scratch) / "probe"), str(probe)], timeout=60)
    if result.returncode == 0:
        report("OK", f"host C++ compiler ({Path(compiler).name})")
    else:
        report("FAIL", f"host C++ compiler {compiler} cannot build a test program", last_line(result))
    result = run([sys.executable, "tools/build_config.py", "list", "configurations"], timeout=60)
    if result.returncode == 0 and result.stdout.strip():
        count = len(result.stdout.strip().splitlines())
        report("OK", f"guided setup runs ({count} saved configurations)")
    else:
        report("FAIL", "python3 tools/build_config.py list configurations failed", last_line(result))


def idf_version(idf):
    header = idf / "components/esp_common/include/esp_idf_version.h"
    if not header.exists():
        return None
    text = header.read_text()
    parts = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"#define\s+ESP_IDF_VERSION_{name}\s+(\d+)", text)
        if not match:
            return None
        parts.append(int(match[1]))
    return tuple(parts)


def find_idf():
    env = os.environ.get("IDF_PATH")
    if env:
        return Path(env), "IDF_PATH"
    fallback = Path.home() / "esp" / "esp-idf"
    if fallback.exists():
        return fallback, "~/esp/esp-idf"
    return None, ""


def check_idf():
    print("ESP-IDF")
    idf, source = find_idf()
    wanted = ".".join(map(str, IDF_VERSION))
    guide = f"https://docs.espressif.com/projects/esp-idf/en/v{wanted}/esp32/get-started/index.html"
    if idf is None:
        report("FAIL", "ESP-IDF not found (IDF_PATH is not set and ~/esp/esp-idf does not exist)",
               f"Install ESP-IDF v{wanted}: {guide}")
        return None
    version = idf_version(idf)
    if version is None:
        report("FAIL", f"{idf} ({source}) is not an ESP-IDF checkout", f"Install ESP-IDF v{wanted}: {guide}")
        return None
    shown = ".".join(map(str, version))
    if version == IDF_VERSION:
        report("OK", f"ESP-IDF {shown} at {idf}")
    else:
        report("FAIL", f"ESP-IDF {shown} at {idf}; HardwareOne needs exactly {wanted}",
               f"In {idf}: git checkout v{wanted} && git submodule update --init --recursive && ./install.sh")
    if shutil.which("idf.py") and os.environ.get("IDF_PATH"):
        report("OK", "ESP-IDF environment is active in this shell")
    else:
        report("WARN", "ESP-IDF environment is not active in this shell",
               f'Run  . "{idf}/export.sh"  in each new terminal before building.')
    return idf


def check_arduino():
    print("Arduino component")
    if not (REPO / "components/arduino").exists():
        report("FAIL", "components/arduino is missing (a fresh clone has none)",
               "Run tools/vendor_arduino.sh once (needs network access to GitHub).")
        return
    result = run(["bash", "tools/vendor_arduino.sh", "--check"], timeout=120)
    if result.returncode == 0:
        report("OK", "arduino-esp32 vendored with every local patch applied")
    else:
        report("FAIL", "vendored arduino-esp32 is missing a local patch", last_line(result) +
               "\nRe-vendor it: tools/vendor_arduino.sh --force")


def check_p4(idf):
    print("ESP32-P4 SDK staging")
    if idf is None:
        report("FAIL", "needs ESP-IDF (above)")
        return
    env = dict(os.environ, IDF_PATH=str(idf))
    result = run(["bash", "tools/p4/prepare_sdk.sh", "--check"], env=env, timeout=300)
    if result.returncode == 0:
        report("OK", "P4 components staged (tools/p4/prepare_sdk.sh --check)")
    else:
        report("FAIL", "P4 components are not staged", last_line(result) +
               "\nRun tools/p4/prepare_sdk.sh once per ESP-IDF install (needs network access).")
    pie = run([sys.executable, "tools/p4/check_idf_pie.py", "--idf", str(idf)], timeout=60)
    line = last_line(pie)
    if pie.returncode == 0 and ": present in " in line:
        report("OK", "ESP-IDF carries the PIE fix P4X-EYE speaker labels need")
    else:
        report("WARN", "ESP-IDF lacks the PIE fix P4X-EYE speaker labels need",
               line + "\nSee tools/p4/patches/README.md.")


def check_disk():
    print("Disk")
    free_gb = shutil.disk_usage(REPO).free / 1e9
    if free_gb >= MIN_FREE_GB:
        report("OK", f"{free_gb:.0f} GB free")
    else:
        report("WARN", f"only {free_gb:.1f} GB free; each board's build folder takes a few GB",
               "Free some space, or delete build-*/ folders you no longer need.")


def check_ports():
    print("Connected boards (listed only, not opened)")
    patterns = ["/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.SLAB*", "/dev/cu.wchusbserial*",
                "/dev/ttyACM*", "/dev/ttyUSB*"]
    ports = sorted({port for pattern in patterns for port in glob.glob(pattern)})
    if ports:
        for port in ports:
            report("OK", port)
        print("        Pass one as -p PORT to tools/build_board.sh <board> ... flash monitor.")
    else:
        report("WARN", "no USB serial port found",
               "Plug the board in with a data cable (not charge-only). Not needed to build.")


def check_build(board, idf):
    print(f"Build {board}")
    if not (REPO / "boards" / f"{board}.defaults").exists() or ".ota" in board:
        names = sorted(p.stem for p in (REPO / "boards").glob("*.defaults") if ".ota" not in p.stem)
        report("FAIL", f"unknown board '{board}'", "Boards: " + ", ".join(names))
        return
    if idf is None:
        report("FAIL", "needs ESP-IDF (above)")
        return
    print(f"        building into build-{board}/ (the first build takes several minutes) ...", flush=True)
    script = f'. "{idf}/export.sh" >/dev/null 2>&1 && tools/build_board.sh {board}'
    result = run(["bash", "-c", script], timeout=3600)
    if result.returncode == 0:
        image = [line for line in result.stdout.splitlines() if line.startswith("Image:")]
        report("OK", f"build-{board}/ built", image[-1] if image else "")
    else:
        output = result.stdout + result.stderr
        if "app partition is too small" in output:
            # Every feature on can outgrow a board's app partition; that is a
            # feature choice, not a broken setup.
            report("WARN", f"build-{board}/ compiled, but the image with every feature on is larger "
                           "than the board's app partition",
                   "The setup works. Pick fewer features: python3 tools/build_config.py new\n"
                   "or build a saved configuration: python3 tools/build_config.py list configurations")
            return
        tail = "\n".join(output.strip().splitlines()[-8:])
        report("FAIL", f"tools/build_board.sh {board} failed", tail)


def main():
    parser = argparse.ArgumentParser(description="Check that this computer is ready to build HardwareOne.")
    parser.add_argument("--p4", action="store_true", help="also check the ESP32-P4 SDK staging")
    parser.add_argument("--build", metavar="BOARD", help="also build one board, e.g. xiao_s3 (takes minutes)")
    args = parser.parse_args()

    print("HardwareOne setup check\n")
    check_checkout()
    check_host_tools()
    idf = check_idf()
    check_arduino()
    if args.p4:
        check_p4(idf)
    check_disk()
    check_ports()
    if args.build:
        check_build(args.build, idf)

    print()
    if failed:
        print(f"{failed} problem(s) to fix before building ({warned} warning(s)). See docs/QUICKSTART.md.")
        return 1
    print("Ready to build" + (f", with {warned} warning(s) above." if warned else "."))
    if not args.build:
        print("Next: tools/build_board.sh <board>   or   python3 tools/build_config.py new")
    return 0


if __name__ == "__main__":
    sys.exit(main())
