#!/usr/bin/env python3
"""Clearing the OLED notification center takes back its banners.

An event's banner is a message in the shared feedback queue of OLED_UI.cpp,
fed by systemEventsNotifyTick() (System_Notifications.cpp) and shown one at a
time. Clearing the notification center (oledNotificationClear, OLED_Utils.cpp)
used to move only the list's watermark, so up to twelve messages already
queued, and the one on screen, still showed for notifications the user had
just cleared.

This test runs the shipped queue code (oledToastShow, oledEventBannerShow,
oledNotificationBannerShow, oledEventBannersDismissThrough, the expiry step and
the confirmation pause) in a harness and checks the source joins that make the
fix whole:

1. oledNotificationClear() dismisses the banners through its new watermark.
2. The notify tick skips the banner of an event at or below the watermark,
   for an event posted before the clear and drained after it.
3. The tick passes the event's seq with its banner.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(component: Path) -> str:
    header = (component / "OLED_UI.h").read_text()
    source = (component / "OLED_UI.cpp").read_text()
    types = "\n".join(extract_block(header, marker) + ";" for marker in (
        "enum class OledUiIcon", "struct OledToast"))
    start = source.index("static constexpr unsigned kToastQueueCapacity")
    end = source.index("void oledToastRender()")  # the queue and its timing, not the drawing
    queue = source[start:end]
    require("void oledEventBannersDismissThrough(uint32_t seq)" in queue,
            "the dismiss function sits with the feedback queue")
    functions = "\n\n".join(extract_block(source, marker) for marker in (
        "void oledEventBannerShow(uint32_t eventSeq",
        "void oledNotificationBannerShow(const char* message"))
    harness = (HERE / "oled_banner_clear_harness.cpp").read_text()
    for marker in ("// INSERT_TYPES", "// INSERT_PRODUCTION"):
        require(harness.count(marker) == 1, f"one {marker}")
    harness = harness.replace("// INSERT_TYPES", types)
    return harness.replace("// INSERT_PRODUCTION", queue + "\n" + functions)


def check_sources(component: Path) -> None:
    utils = (component / "OLED_Utils.cpp").read_text()
    clear = extract_block(utils, "void oledNotificationClear()")
    require(clear.index("sNotifClearedSeq = systemEventLatestSeq();") <
            clear.index("oledEventBannersDismissThrough(sNotifClearedSeq);"),
            "oledNotificationClear dismisses the banners through its new watermark")
    notifications = (component / "System_Notifications.cpp").read_text()
    tick = extract_block(notifications, "void systemEventsNotifyTick(")
    require("e.seq <= oledNotificationClearedSeq()" in tick, "the tick skips banners of cleared events")
    require(tick.index("e.seq <= oledNotificationClearedSeq()") <
            tick.index("oledEventBannerShow(e.seq, "), "the cleared check comes before the banner")
    require("oledNotificationBannerShow(" not in tick, "event banners carry their seq")
    ui = (component / "OLED_UI.cpp").read_text()
    show = extract_block(ui, "void oledToastShow(const char* message")
    require("toastEnqueue(message, durationMs, icon, 0);" in show,
            "local feedback is not an event's message")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()
    check_sources(component)
    with tempfile.TemporaryDirectory(prefix="hw1-oled-banner-clear-") as temp:
        unit = Path(temp) / "banner_clear.cpp"
        unit.write_text(generated(component))
        exe = Path(temp) / "banner_clear"
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-g", "-I", str(component), str(unit), "-o", str(exe)], check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
