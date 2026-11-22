#!/usr/bin/env python3
"""heap_low and fs_low_space reach the notification centre and the web toast.

Compiles, from the shipping System_Notifications.cpp on every run, the default
rule switch, the importance tiers, the message formatter and the cooldown
check of systemEventsNotifyTick, with the real event catalog, and checks:

  - both kinds have the queue and toast sinks, level 2 (warning), no banner;
  - the cooldown is 10 minutes and the rule field holds it (a 16-bit field
    would store 10176 ms);
  - through the production cooldown check, a second post 61 s after the first
    is not rendered again and one 10 minutes later is;
  - the text of each fits the 64-byte message buffer the tick uses;
  - both kinds stay at the STANDARD tier (what the mesh event push carries);
  - companion_low_memory and the QuickNote rows are unchanged;
  - EVT-2 (owner: "toast, rate-limited"): sd_write_failed, settings_save_failed,
    sensor_fault, wifi_connect_failed and g2_disconnected have the queue and
    toast sinks, no banner, a cooldown, and their own sentence.

Not covered: the tick's viewer resolution, the glasses and app sinks, the
post sites (System_MemoryMonitor.cpp, System_VFS.cpp) or anything on a device.
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
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

class String;
#include "System_Notifications.h"

void systemEventPost(uint8_t, const char*, const char*, uint8_t, const char*) {}

static uint32_t sKindLastShownMs[SYSEVT_COUNT];
static struct { unsigned cooldownSupp; } gNotifPipe;

// INSERT_PRODUCTION_DEFINITIONS

// The production cooldown check, with the tick's own variable names. Returns
// true when the event goes on to the interrupt sinks.
static bool rendered(const SystemEvent& e, uint32_t nowMs) {
  const NotifRule r = notifDefaultRuleFor(e.kind);
  for (int once = 0; once < 1; ++once) {
    // INSERT_COOLDOWN_BLOCK
    return true;
  }
  return false;
}

static void expectRule(uint8_t kind, const char* name, const char* subject,
                       const char* text) {
  assert(std::strcmp(systemEventKindName(kind), name) == 0);
  const NotifRule rule = notifDefaultRuleFor(kind);
  assert(rule.sinks == (NSINK_QUEUE | NSINK_TOAST));
  assert((rule.sinks & NSINK_BANNER) == 0);
  assert(rule.level == 2);
  assert(rule.durMs == 3000);
  assert(rule.cooldownMs == 600000u);
  assert(notifKindTier(kind) == NTIER_STANDARD);

  SystemEvent e{};
  e.kind = kind;
  std::snprintf(e.subject, sizeof(e.subject), "%s", subject);
  char msg[64];
  notifFormatEvent(e, msg, sizeof(msg));
  assert(std::strcmp(msg, text) == 0);
  // Largest subject either post site can write: a 32-bit byte count.
  std::snprintf(e.subject, sizeof(e.subject), "%s", "4294967295");
  notifFormatEvent(e, msg, sizeof(msg));
  assert(std::strlen(msg) < 48);
  // An empty subject still reads as a sentence.
  e.subject[0] = '\0';
  notifFormatEvent(e, msg, sizeof(msg));
  assert(std::strstr(msg, "low: ? B free") != nullptr);

  // First post renders; the post site's own 60 s cooldown lets a second
  // through to the tick, which holds it back until 10 minutes have passed.
  std::memset(sKindLastShownMs, 0, sizeof(sKindLastShownMs));
  gNotifPipe.cooldownSupp = 0;
  e.kind = kind;
  assert(rendered(e, 5000));
  assert(!rendered(e, 5000 + 61000));
  assert(!rendered(e, 5000 + 599999));
  assert(gNotifPipe.cooldownSupp == 2);
  assert(rendered(e, 5000 + 600000));
  assert(!rendered(e, 5000 + 600000 + 70000));
  // millis() wrap: the gap is computed in unsigned 32-bit arithmetic.
  sKindLastShownMs[kind] = 0xFFFFFF00u;
  assert(!rendered(e, 0x00000100u));
  assert(rendered(e, 0xFFFFFF00u + 600000u));
}

int main() {
  // The field holds the value; 600000 in 16 bits would be 10176.
  static_assert(sizeof(NotifRule::cooldownMs) >= 4, "cooldown needs 32 bits");
  static_assert(sizeof(NotifRule) == 8, "rule stays two words, passed by value");

  expectRule(SYSEVT_HEAP_LOW, "heap_low", "23456", "Memory low: 23456 B free");
  expectRule(SYSEVT_FS_LOW_SPACE, "fs_low_space", "65536", "Storage low: 65536 B free");

  // Neighbours keep their rows.
  const NotifRule companion = notifDefaultRuleFor(SYSEVT_COMPANION_LOW_MEMORY);
  assert(companion.sinks == (NSINK_QUEUE | NSINK_TOAST));
  assert(companion.cooldownMs == 60000u);
  assert(notifDefaultRuleFor(SYSEVT_NOTE_SAVED).sinks == NSINK_QUEUE);
  assert(notifDefaultRuleFor(SYSEVT_BATTERY_LOW).cooldownMs == 30000u);
  assert(notifDefaultRuleFor(SYSEVT_BATTERY_CRITICAL).durMs == 4000);

  // EVT-2 faults: queue + toast, no banner, rate-limited, own sentence.
  struct Fault { uint8_t kind; const char* subject; const char* text; uint32_t cooldownMs; };
  const Fault faults[] = {
      {SYSEVT_SD_WRITE_FAILED, "log.append", "SD card write failed (log.append)", 60000u},
      {SYSEVT_SETTINGS_SAVE_FAILED, "snapshot", "Settings not saved (snapshot)", 60000u},
      {SYSEVT_SENSOR_FAULT, "Gamepad", "Gamepad fault: switched off", 60000u},
      {SYSEVT_WIFI_CONNECT_FAILED, "home", "WiFi join failed: home", 600000u},
      {SYSEVT_G2_DISCONNECTED, "LEFT", "Glasses off (LEFT)", 30000u},
  };
  for (const Fault& f : faults) {
    const NotifRule rule = notifDefaultRuleFor(f.kind);
    assert(rule.sinks == (NSINK_QUEUE | NSINK_TOAST));
    assert(rule.cooldownMs == f.cooldownMs && rule.durMs > 0);
    SystemEvent e{};
    e.kind = f.kind;
    std::snprintf(e.subject, sizeof(e.subject), "%s", f.subject);
    char msg[64];
    notifFormatEvent(e, msg, sizeof(msg));
    assert(std::strcmp(msg, f.text) == 0);
    std::memset(sKindLastShownMs, 0, sizeof(sKindLastShownMs));
    assert(rendered(e, 1000));
    assert(!rendered(e, 1000 + f.cooldownMs - 1));
    assert(rendered(e, 1000 + f.cooldownMs));
  }

  std::puts("heap_low and fs_low_space: queue + toast, warning level, one "
            "interrupt per 10 minutes; text fits; tier unchanged; EVT-2 faults "
            "toast, rate-limited");
  return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    notifications = (COMPONENT / "System_Notifications.cpp").read_text()

    rules = extract_block(notifications, "static NotifRule notifDefaultRuleFor(")
    for kind in ("SYSEVT_HEAP_LOW", "SYSEVT_FS_LOW_SPACE", "SYSEVT_SD_WRITE_FAILED",
                 "SYSEVT_SETTINGS_SAVE_FAILED", "SYSEVT_SENSOR_FAULT", "SYSEVT_WIFI_CONNECT_FAILED",
                 "SYSEVT_G2_DISCONNECTED"):
        require(rules.count(f"case {kind}:") == 1, f"one rule row for {kind}")
    tier = extract_block(notifications, "uint8_t notifKindTier(")
    require("SYSEVT_HEAP_LOW" not in tier and "SYSEVT_FS_LOW_SPACE" not in tier,
            "the two kinds stay at the default tier (mesh event push unchanged)")
    require(notifications.count("if (r.cooldownMs)") == 1, "one cooldown check in the tick")
    cooldown = extract_block(notifications, "if (r.cooldownMs)")
    require("sKindLastShownMs[e.kind]" in cooldown and "continue;" in cooldown,
            "cooldown check no longer has the shape this test drives")

    definitions = "\n\n".join([rules, tier,
                               extract_block(notifications, "void notifFormatEvent(")])
    harness = HARNESS.replace("// INSERT_PRODUCTION_DEFINITIONS", definitions)
    harness = harness.replace("// INSERT_COOLDOWN_BLOCK", cooldown)
    with tempfile.TemporaryDirectory(prefix="hw1-notif-low-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-keyword-macro", "-I", str(HERE / "i2c_stubs"),
                   "-I", str(COMPONENT), str(source),
                   str(COMPONENT / "System_EventCatalog.cpp"), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
