#!/usr/bin/env python3
"""Exercise the production G2 Status compound's child set and corner rows.

Compiles G2_Glasses.cpp's Status geometry, kStatusTextChildCount,
buildStatusTextChildren and buildPhoneStatusRow for every
ENABLE_BLE_PHONE_PIPE x BATTERY_MONITOR_AVAILABLE combination. The phone row
reads the phone owner's phoneStatusSnapshot(), which runs BLE_LinkStatus.cpp's
bleLinkStatus() and BLE_LinkStatusCore.h as shipped over fake pipe, peer and
console inputs. Checks that CREATE/REBUILD ship exactly kStatusTextChildCount
children with distinct container ids (the back-row list holds id 1), that no
two rectangles overlap and all fit the 576x288 lens, that the phone row exists
only with the pipe and sits directly under the last device row, and that every
pipe phase the lens can see reads "PH: <pipeBrief>" within the corner's
nine-character budget (the 110 px width "ESP:~100%" already fills). The lens
firmware's acceptance of one more text child is not qualified here.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "G2_RenderTypes.h"

static unsigned checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// INSERT_LINK_TYPES
// The rest of bleLinkStatus()'s inputs: no console server, the glasses up.
static bool bleServerUp() { return false; }
static bool isG2ClientInitialized() { return true; }
struct { int bleMode = 1; } gSettings;
// INSERT_LINK_STATUS

// INSERT_PRODUCTION

static bool overlap(const G2ContainerGeom& a, const G2ContainerGeom& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int main() {
  G2TextChildSpec children[16];
  const size_t n = buildStatusTextChildren(children, "body", "G2: 80%", "R1: 70%",
                                           "ESP: 60%", "PH: ready", "meter");
  CHECK(n == kStatusTextChildCount);
  bool sawPhone = false;
  for (size_t i = 0; i < n; ++i) {
    CHECK(children[i].containerId != 1);  // the back-row list's id
    CHECK(children[i].geom.x + children[i].geom.w <= 576 &&
          children[i].geom.y + children[i].geom.h <= 288);
    CHECK(!overlap(children[i].geom, kStatusBackGeom));
    for (size_t j = i + 1; j < n; ++j) {
      CHECK(children[i].containerId != children[j].containerId);
      CHECK(!overlap(children[i].geom, children[j].geom));
    }
    if (std::strcmp(children[i].containerName, "phone") == 0) {
      sawPhone = true;
      CHECK(std::strcmp(children[i].content, "PH: ready") == 0);
      // Directly under the last device row: ESP if measured, else R1.
      const G2ContainerGeom above = BATTERY_MONITOR_AVAILABLE ? kStatusEspGeom : kStatusR1Geom;
      CHECK(children[i].geom.x == above.x && children[i].geom.w == above.w);
      CHECK(children[i].geom.y > above.y && children[i].geom.y - (above.y + above.h) <= 4);
    }
  }
  CHECK(sawPhone == (ENABLE_BLE_PHONE_PIPE != 0));
#if ENABLE_BLE_PHONE_PIPE
  // Every phase a built pipe can show, from the pipe's own inputs.
  using hw1link::PipeStep;
  struct Phase { const char* name; FakeLinkInputs in; const char* row; };
  auto running = [](PipeStep step, const char* label, bool paired) {
    FakeLinkInputs in;
    in.pipeActive = true; in.step = step; in.label = label; in.paired = paired;
    return in;
  };
  FakeLinkInputs stopped, noSecret, ready, pairing;
  noSecret.label = "configure phone secret first";
  ready = running(PipeStep::Link, "ready", true); ready.pipeReady = true;
  pairing = running(PipeStep::Scanning, "waiting to enroll", false); pairing.enrolling = true;
  const Phase phases[] = {
    {"off", stopped, "PH: off"},
    {"no secret", noSecret, "PH: nokey"},
    {"not paired", running(PipeStep::Idle, "stopped", false), "PH:unpair"},
    {"pairing", pairing, "PH: pair"},
    {"waiting", running(PipeStep::Waiting, "waiting for radio", true), "PH: wait"},
    {"searching", running(PipeStep::Scanning, "scanning", true), "PH: scan"},
    {"linking", running(PipeStep::Link, "authenticating pipe", true), "PH: link"},
    {"ready", ready, "PH: ready"},
    {"closing", running(PipeStep::Retiring, "phone close requested", true), "PH: close"},
    {"idle", running(PipeStep::Idle, "disconnected", true), "PH: idle"},
    {"failed", running(PipeStep::Idle, "phone not found", true), "PH: fail"},
    {"fault", running(PipeStep::Fault, "phone cleanup incomplete", true), "PH: fault"},
  };
  char row[32];
  for (const Phase& phase : phases) {
    linkIn = phase.in;
    buildPhoneStatusRow(row, sizeof(row));
    if (std::strcmp(row, phase.row) != 0)
      std::fprintf(stderr, "%s: got '%s', want '%s'\n", phase.name, row, phase.row);
    CHECK(std::strcmp(row, phase.row) == 0);
    CHECK(std::strlen(row) <= std::strlen("ESP:~100%"));
  }
  // The snapshot the row reads is the pipe part of bleLinkStatus().
  linkIn = ready;
  CHECK(phoneStatusSnapshot().pipe.phase == bleLinkStatus().pipe.phase);
  CHECK(phoneStatusSnapshot().ready && phoneStatusSnapshot().enrolled);
#endif
  std::printf("%u\n", checks);
  return 0;
}
'''


# bleLinkStatus()'s pipe, peer and console inputs, driven through `linkIn`. A
# harness defines bleServerUp(), isG2ClientInitialized() and
# gSettings.bleMode itself, before the definition.
LINK_STATUS_FAKES = r'''
struct FakeLinkInputs {
  bool pipeActive = false, pipeStandalone = false, pipeReady = false, enrolling = false;
  hw1link::PipeStep step = hw1link::PipeStep::Idle;
  const char* label = "stopped";               // phonePipeStatus()
  bool paired = false, autoReconnect = false;  // the phone's peer record
  BleConsoleView console;
  hw1link::PhoneLinkInputs phone;              // the phone link block (phone link v2)
};
static FakeLinkInputs linkIn;
static bool phonePipeActive() { return linkIn.pipeActive; }
static bool phonePipeReady() { return linkIn.pipeReady; }
static bool phonePipeEnrolling() { return linkIn.enrolling; }
static uint8_t phonePipeStep() { return uint8_t(linkIn.step); }
static const char* phonePipeStatus() { return linkIn.label; }
static bool blePeerReconnectSnapshot(BlePeerKind kind, BlePeerReconnectSnapshot& out) {
  out = BlePeerReconnectSnapshot{};
  if (kind != BLE_PEER_PHONE) return false;
  out.hasSavedTarget = linkIn.paired;
  out.autoReconnect = linkIn.autoReconnect;
  return true;
}
static BleConsoleView bleConsoleView() { return linkIn.console; }
static hw1link::PhoneLinkInputs bleLinkPhoneInputs() { return linkIn.phone; }
'''


def link_status_unit(component: Path) -> tuple[str, str]:
    """BLE_LinkStatus as shipped, for a harness that shows its words.

    Returns (types, definition). The types (BLE_LinkStatusCore.h, BleConsoleView,
    BlePeerKind, BlePeerReconnectSnapshot, BleLinkStatus) and LINK_STATUS_FAKES
    go before the harness's own role inputs and gSettings; bleLinkStatus() from
    BLE_LinkStatus.cpp goes after them. ENABLE_BLE_PHONE_PIPE must be defined.
    """
    core = (component / "BLE_LinkStatusCore.h").read_text().replace("#pragma once\n", "", 1)
    bluetooth = (component / "Bluetooth.h").read_text()
    peers = (component / "BLE_Peers.h").read_text()
    header = (component / "BLE_LinkStatus.h").read_text()
    source = (component / "BLE_LinkStatus.cpp").read_text()
    types = "\n\n".join([
        core,
        extract_block(bluetooth, "struct BleConsoleView") + ";",
        extract_block(peers, "enum BlePeerKind") + ";",
        extract_block(peers, "struct BlePeerReconnectSnapshot") + ";",
        extract_block(header, "struct BleLinkStatus") + ";",
        LINK_STATUS_FAKES,
    ])
    return types, extract_block(source, "BleLinkStatus bleLinkStatus()")


def region(source: str, begin: str, end: str) -> str:
    start = source.index(begin)
    return source[start:source.index(end, start) + len(end)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
                if args.sanitize else [])
    glasses = (args.component / "G2_Glasses.cpp").read_text()
    phone_header = (args.component / "BLE_PhoneStatus.h").read_text()
    phone_source = (args.component / "BLE_PhoneStatus.cpp").read_text()
    link_types, link_status = link_status_unit(args.component)
    production = "\n\n".join([
        region(glasses, "static constexpr G2ContainerGeom kStatusBackGeom",
               "static constexpr G2ContainerGeom kStatusMeterGeom = { 318, 224, 250,  56 };"),
        region(glasses, "#if BATTERY_MONITOR_AVAILABLE\nstatic constexpr size_t kStatusTextChildCount",
               "#endif\n"),
        extract_block(glasses, "static void buildPhoneStatusRow("),
        extract_block(glasses, "static size_t buildStatusTextChildren("),
    ])
    # The phone owner's snapshot (the row's source) runs as shipped too.
    unit_text = HARNESS.replace("// INSERT_LINK_TYPES", "\n\n".join([
        link_types, extract_block(phone_header, "struct PhoneStatusSnapshot") + ";"]))
    unit_text = unit_text.replace("// INSERT_LINK_STATUS", "\n\n".join([
        link_status,
        # The pipe-alone reading (active with the glasses runtime closed) the
        # snapshot's `standalone` now means.
        extract_block(phone_source, "static bool phonePipeAlone()"),
        extract_block(phone_source, "PhoneStatusSnapshot phoneStatusSnapshot()")]))
    unit_text = unit_text.replace("// INSERT_PRODUCTION", production)
    total = 0
    with tempfile.TemporaryDirectory(prefix="hw1-g2-status-corner-") as directory:
        work = Path(directory)
        shutil.copyfile(args.component / "G2_RenderTypes.h", work / "G2_RenderTypes.h")
        unit = work / "corner.cpp"
        unit.write_text(unit_text)
        for pipe in (0, 1):
            for battery in (0, 1):
                binary = work / f"corner-{pipe}{battery}"
                subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                "-Wno-unused-function", *sanitize, "-I", str(work),
                                f"-DENABLE_BLE_PHONE_PIPE={pipe}",
                                f"-DBATTERY_MONITOR_AVAILABLE={battery}",
                                str(unit), "-o", str(binary)], check=True, timeout=120)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                require(result.returncode == 0,
                        f"pipe={pipe} battery={battery}: {result.stderr.strip()}")
                total += int(result.stdout.strip())
    print(f"G2 Status corner: child set, ids, layout and phone row words passed in 4 builds "
          f"({total} checks)")


if __name__ == "__main__":
    main()
