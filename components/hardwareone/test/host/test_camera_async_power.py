#!/usr/bin/env python3
"""The camera owner's gated asynchronous start/stop, and the lens using it.

`opencamera async` / `closecamera async` (System_Camera_DVP.cpp) are the form
UI adapters submit through the command executor, so the registry authorizes
the change, the live-session fence applies and the [CMD] audit records it. A
start honours the cameraEnabled master switch exactly as the synchronous
`opencamera` does; both forms only report whether cam_pwr admitted the request
and never wait for the camera. The production handlers are compiled unchanged
against power-request doubles; the synchronous forms must behave as before.

A source contract checks that the G2 Sensors page requests no camera power
itself (cameraPowerRequest*): its CAM toggle submits the command, and
g2_sensors_redraw runs that flow. --component replays against another tree.
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

HARNESS = r'''
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

class String {
  std::string s_;
 public:
  String(const char* value = "") : s_(value ? value : "") {}
  void trim() {
    const auto first = s_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { s_.clear(); return; }
    s_ = s_.substr(first, s_.find_last_not_of(" \t\r\n") - first + 1);
  }
  void toLowerCase() { for (auto& c : s_) c = char(std::tolower(static_cast<unsigned char>(c))); }
  bool operator==(const char* other) const { return s_ == other; }
};
static bool gCLIValidateOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (gCLIValidateOnly) return "VALID"; } while (0)
static struct { bool cameraEnabled = true; } gSettings;

// cam_pwr admission doubles: what reached the owner's power requests.
static bool queueAccepts = true, syncResult = true;
static int startAsync = 0, stopAsync = 0, startSync = 0, stopSync = 0;
static unsigned lastWaitMs = 0;
// (A tree without the asynchronous form never calls these two.)
[[maybe_unused]] static bool cameraPowerRequestStartAsync() { ++startAsync; return queueAccepts; }
[[maybe_unused]] static bool cameraPowerRequestStopAsync() { ++stopAsync; return queueAccepts; }
static bool cameraPowerRequestStartSync(unsigned waitMs) { ++startSync; lastWaitMs = waitMs; return syncResult; }
static bool cameraPowerRequestStopSync(unsigned waitMs) { ++stopSync; lastWaitMs = waitMs; return syncResult; }

// INSERT_PRODUCTION

static void reset() {
  gCLIValidateOnly = false;
  gSettings.cameraEnabled = true;
  queueAccepts = syncResult = true;
  startAsync = stopAsync = startSync = stopSync = 0;
  lastWaitMs = 0;
}
static bool startsWith(const char* text, const char* prefix) {
  return text && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}
static int requests() { return startAsync + stopAsync + startSync + stopSync; }

int main() {
  // Asynchronous start: admitted when cam_pwr takes it, never waited for.
  reset();
  const char* reply = cmd_camerastart("async");
  CHECK(startAsync == 1 && requests() == 1);
  CHECK(!startsWith(reply, "Error") && !startsWith(reply, "ERROR"));
  reset();
  reply = cmd_camerastart("  ASYNC ");
  CHECK(startAsync == 1 && requests() == 1);
  // The master switch gates the asynchronous start like the synchronous one.
  reset();
  gSettings.cameraEnabled = false;
  reply = cmd_camerastart("async");
  CHECK(requests() == 0 && startsWith(reply, "ERROR: Camera is disabled"));
  // cam_pwr could not take it (worker or queue unavailable): an error reply.
  reset();
  queueAccepts = false;
  reply = cmd_camerastart("async");
  CHECK(startAsync == 1 && startsWith(reply, "Error"));
  // Asynchronous stop: allowed while disabled, refused only by admission.
  reset();
  gSettings.cameraEnabled = false;
  reply = cmd_camerastop("async");
  CHECK(stopAsync == 1 && requests() == 1 && !startsWith(reply, "Error"));
  reset();
  queueAccepts = false;
  reply = cmd_camerastop("async");
  CHECK(stopAsync == 1 && startsWith(reply, "Error"));
  // The validate pass requests nothing.
  reset();
  gCLIValidateOnly = true;
  CHECK(std::strcmp(cmd_camerastart("async"), "VALID") == 0 &&
        std::strcmp(cmd_camerastop("async"), "VALID") == 0 && requests() == 0);
  // The synchronous forms are unchanged.
  reset();
  CHECK(std::strcmp(cmd_camerastart(""), "Camera started successfully") == 0);
  CHECK(startSync == 1 && lastWaitMs == 60000 && requests() == 1);
  reset();
  gSettings.cameraEnabled = false;
  CHECK(startsWith(cmd_camerastart(""), "ERROR: Camera is disabled") && requests() == 0);
  reset();
  syncResult = false;
  CHECK(std::strcmp(cmd_camerastart(""), "Error: Camera initialization failed") == 0);
  reset();
  CHECK(std::strcmp(cmd_camerastop(""), "Camera stopped") == 0);
  CHECK(stopSync == 1 && lastWaitMs == 30000 && requests() == 1);
  std::printf("camera async power: %d checks passed\n", checks);
  return 0;
}
'''

OPTIONAL_HELPERS = ("static bool cameraAsyncForm(", "static const char* cameraPowerAsyncCommand(")


def production(camera: str) -> str:
    pieces = []
    constant = re.search(r"(?m)^static const char\* const kCameraDisabledError =[^;]*;", camera)
    if constant:
        pieces.append(constant[0])
    pieces += [extract_block(camera, marker) for marker in OPTIONAL_HELPERS if marker in camera]
    for marker in ("const char* cmd_camerastart(", "const char* cmd_camerastop("):
        require(camera.count(marker) == 1, f"System_Camera_DVP.cpp: one {marker}")
        pieces.append(extract_block(camera, marker))
    return "\n\n".join(pieces)


def lens_contract(page: str) -> None:
    code = re.sub(r"/\*.*?\*/|//[^\n]*", "", page, flags=re.S)
    require(re.search(r"\bcameraPowerRequest\w*\s*\(", code) is None,
            "G2_Page_Sensors.cpp requests camera power itself instead of submitting "
            "the camera owner's command")
    require('"opencamera async"' in code and '"closecamera async"' in code,
            "G2_Page_Sensors.cpp submits the owner's asynchronous camera command")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read production sources from")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    unit_text = HARNESS.replace("// INSERT_PRODUCTION",
                                production((args.component / "System_Camera_DVP.cpp").read_text()))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-camera-async-power-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(unit_text)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)
    lens_contract((args.component / "G2_Page_Sensors.cpp").read_text())
    print("camera async power: owner and lens contract passed")


if __name__ == "__main__":
    main()
