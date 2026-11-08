#!/usr/bin/env python3
"""Firmware code keeps no thread_local storage, and the camera options cache that replaced one.

On the ESP32-P4 (FreeRTOS RISC-V port, pxPortInitialiseStack) the whole TLS
image is reserved at the top of every task's stack, IDF's fixed-size stacks
included. Two camera buffers (System_Camera_DVP.cpp, 576 bytes) left
Bluedroid's 2.5 KB hciT about 1.95 KB, and it overflowed by 4 bytes printing a
log line (bench 2026-11-08, stack protection fault in 3 of 15 reboots).

1. Source contract: no `thread_local` / `_Thread_local` / `__thread` in the
   HardwareOne-owned firmware sources (components/hardwareone,
   hw1_ota_protocol, esp_now_hosted, main), comments and string literals
   ignored.
2. cameraResolutionOptions is compiled unchanged against HAL_Camera.cpp and
   fakes for the capability snapshot, ps_alloc/ps_free and portMUX: the text
   is right, one resolution set gives one stable pointer, a new set gets its
   own buffer and the older one is left intact, an empty set, a failed
   allocation and a full table give "" and free what they allocated.
--component replays against another components/hardwareone tree.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
# HardwareOne-owned code; vendored libraries (hardwareone_libs, arduino) are
# covered by the ELF TLS size check in the device procedure.
FIRMWARE_DIRS = ("hardwareone", "hw1_ota_protocol", "esp_now_hosted")
TLS = re.compile(r"\b(thread_local|_Thread_local|__thread)\b")

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "HAL_Camera.h"

static int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static int lockDepth = 0;
#define portENTER_CRITICAL(m) do { (void)(m); CHECK(lockDepth == 0); ++lockDepth; } while (0)
#define portEXIT_CRITICAL(m) do { (void)(m); CHECK(lockDepth == 1); --lockDepth; } while (0)

enum class AllocPref : uint8_t { PreferPSRAM = 0 };
static int live = 0, allocs = 0;
static bool failAlloc = false;
static void* ps_alloc(size_t size, AllocPref, const char*) {
  CHECK(lockDepth == 0);  // never allocate inside the critical section
  ++allocs;
  if (failAlloc) return nullptr;
  ++live;
  void* p = std::malloc(size);
  std::memset(p, 0x5a, size);
  return p;
}
static void ps_free(void* p) { if (p) { --live; std::free(p); } }

static CameraCapabilities caps{"fake", 0, 0, false, false, 0, 0};
static CameraCapabilities getCameraCapabilities() { CHECK(lockDepth == 0); return caps; }

// INSERT_PRODUCTION

static uint32_t bits(std::initializer_list<CameraFrameSize> sizes) {
  uint32_t mask = 0;
  for (auto s : sizes) mask |= cameraResolutionBit(s);
  return mask;
}

int main() {
  caps.resolutions = 0;
  CHECK(std::strcmp(cameraResolutionOptions(), "") == 0 && allocs == 0);

  caps.resolutions = bits({CameraFrameSize::QVGA, CameraFrameSize::VGA, CameraFrameSize::HD});
  const char* first = cameraResolutionOptions();
  CHECK(std::strcmp(first, "0|320x240 (qvga),1|640x480 (vga),11|1280x720 (hd)") == 0);
  CHECK(allocs == 1 && live == 1);
  // G2/OLED/web readers call it in loops: no rebuild, the same pointer.
  for (int i = 0; i < 20; ++i) CHECK(cameraResolutionOptions() == first);
  CHECK(allocs == 1);

  const std::string kept = first;
  caps.resolutions = bits({CameraFrameSize::QVGA});
  const char* second = cameraResolutionOptions();
  CHECK(second != first && std::strcmp(second, "0|320x240 (qvga)") == 0);
  CHECK(kept == first);  // a reader still holding the first text is unaffected
  caps.resolutions = bits({CameraFrameSize::QVGA, CameraFrameSize::VGA, CameraFrameSize::HD});
  CHECK(cameraResolutionOptions() == first && allocs == 2);

  failAlloc = true;
  caps.resolutions = bits({CameraFrameSize::SVGA});
  CHECK(std::strcmp(cameraResolutionOptions(), "") == 0 && live == 2);
  failAlloc = false;

  // Every frame size: the text fits the buffer.
  caps.resolutions = (1u << unsigned(CameraFrameSize::Count)) - 1;
  const char* all = cameraResolutionOptions();
  CHECK(std::strstr(all, "12|400x296 (cif)") != nullptr && live == 3);
  caps.resolutions = bits({CameraFrameSize::XGA});
  CHECK(std::strcmp(cameraResolutionOptions(), "3|1024x768 (xga)") == 0 && live == 4);

  // Table full: a fifth set gets no list and leaks nothing.
  caps.resolutions = bits({CameraFrameSize::UXGA});
  CHECK(std::strcmp(cameraResolutionOptions(), "") == 0 && live == 4);
  CHECK(cameraResolutionOptions() != nullptr && live == 4);
  caps.resolutions = bits({CameraFrameSize::QVGA});
  CHECK(cameraResolutionOptions() == second);
  CHECK(lockDepth == 0);
  std::printf("camera options cache: %d checks passed\n", checks);
  return 0;
}
'''


def strip_code(text: str) -> str:
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
    return pattern.sub(lambda m: " " if m.group(0)[0] == "/" else '""', text)


def source_contract(components: Path, main_dir: Path) -> None:
    roots = [components / name for name in FIRMWARE_DIRS] + [main_dir]
    offenders = []
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp", ".inc"):
                continue
            if "test" in path.relative_to(root).parts:
                continue
            code = strip_code(path.read_text(errors="replace"))
            for number, line in enumerate(code.splitlines(), 1):
                if TLS.search(line):
                    offenders.append(f"{path.relative_to(components.parent)}:{number}")
    require(not offenders, "thread-local storage in firmware code (reserved on every P4 task stack): "
            + ", ".join(offenders))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    source_contract(component.parent, component.parent.parent / "main")

    camera = (component / "System_Camera_DVP.cpp").read_text()
    marker = "static const char* cameraResolutionOptions()"
    require(camera.count(marker) == 1, "System_Camera_DVP.cpp: one cameraResolutionOptions")
    unit_text = HARNESS.replace("// INSERT_PRODUCTION", extract_block(camera, marker))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-DHW_CAMERA_HAL_HOST_TEST=1", "-I", str(component)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-no-task-tls-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(unit_text)
        subprocess.run([args.cxx, *flags, str(unit), str(component / "HAL_Camera.cpp"), "-o", str(binary)],
                       check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)
    print("no task TLS: source contract and camera options cache passed")


if __name__ == "__main__":
    main()
