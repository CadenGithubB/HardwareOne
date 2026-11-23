#!/usr/bin/env python3
"""The OLED header status bar (OLED_HeaderCore.h) on the host.

oled_header_harness.cpp compiles the shipping core unchanged over a GFX double
with the shipping glcdfont.c (ASan/UBSan): the layout rules exhaustively
(slots, gaps, drop order, nothing moves when busy comes or goes), the title
cut, the Power text from the shared battery backend (hidden with no battery
measurement), the staleness signature and the pixel regions. It then prints
the frames below, which must match byte for byte. M1-M3 and M5-M7 are the
design mockups (design/oled-header-2026-11-22, DESIGN.md) unchanged; M4 has
busy left of the context (owner, 2026-11-22: nothing moves when busy comes).

Also checks that the firmware draws the bar only through the core and that
the old content-area hourglass is gone; that the keyboard's top line is the
core's second profile (K1-K4); the capture dot, the keyboard poll, the
DEBUG_DISPLAY band check and the oleddump command are wired; and the band
guard: no in-frame painter draws at a literal y of 0-9 (literal coordinates
only; computed ones are the device check's job).
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# name: (what it shows, 10 rows of 128 px, '#' = lit)
GOLDEN = """
M1
................................................................................................................................
.###................#......................###....#...........#..................................#...#####.......#...###..##....
#...#...............#.....................#...#...#...........#.................................###......#......##..#...#.##..#.
#.....#...#..####.#####..###..##.#........#.....#####..##...#####.#...#..####...................###.....#......#.#......#....#..
.###..#...#.#.......#...#...#.#.#.#........###....#......#....#...#...#.#.......................###....##.....#..#...###....#...
....#..####..###....#...#####.#.#.#...........#...#....###....#...#...#..###...................#####.....#....#####.#......#....
#...#.....#.....#...#.#.#.....#.#.#.......#...#...#.#.#..#....#.#.#..##.....#........................#...#.......#..#.....#..##.
.###..#...#.####.....#...###..#.#.#........###.....#...####....#...##.#.####.....................#....###........#..#####....##.
.......###......................................................................................................................
################################################################################################################################
M2
................................................................................................................................
.###................#......................###....#...........#......................#######.....#...#####.......#...###..##....
#...#...............#.....................#...#...#...........#.......................#...#.....###......#......##..#...#.##..#.
#.....#...#..####.#####..###..##.#........#.....#####..##...#####.#...#..####..........#.#......###.....#......#.#......#....#..
.###..#...#.#.......#...#...#.#.#.#........###....#......#....#...#...#.#...............#.......###....##.....#..#...###....#...
....#..####..###....#...#####.#.#.#...........#...#....###....#...#...#..###...........###.....#####.....#....#####.#......#....
#...#.....#.....#...#.#.#.....#.#.#.......#...#...#.#.#..#....#.#.#..##.....#.........#####..........#...#.......#..#.....#..##.
.###..#...#.####.....#...###..#.#.#........###.....#...####....#...##.#.####.........#######.....#....###........#..#####....##.
.......###......................................................................................................................
################################################################################################################################
M3
................................................................................................................................
.###..........#....#..................#.....#......#.............#######.............#.............#......#....###...###..##....
#...#.........#.....#.................#...........#.#.............#...#.....###.....###....#......#.#.#..##...#...#.#...#.##..#.
#......###..#####....#..#.##...###..#####..##.....#................#.#.....#####....###....#.........#....#...#..##.#..##....#..
.###..#...#...#.......#.##..#.#...#...#.....#....###................#......#####....###..#####............#...#.#.#.#.#.#...#...
....#.#####...#......#..#...#.#...#...#.....#.....#................###.....#####...#####...#..............#...##..#.##..#..#....
#...#.#.......#.#...#...#...#.#...#...#.#...#.....#...............#####.....###............#..............#...#...#.#...#.#..##.
.###...###.....#...#....#...#..###.....#...###....#...#.#.#......#######.............#...................###...###...###.....##.
................................................................................................................................
################################################################################################################################
M4
................................................................................................................................
.###..........................#.....................#######....###........#####......#...#####.....#......#....###...###..##....
#...#.........................#......................#...#....#...#.....#.#.........###......#....#.#.#..##...#...#.#...#.##..#.
#.....#.##...###...###...###..#.##....................#.#.........#....#..####......###.....#........#....#...#..##.#..##....#..
.###..##..#.#...#.#...#.#...#.##..#....................#.......###....#.......#.....###....##.............#...#.#.#.#.#.#...#...
....#.##..#.#####.#####.#.....#...#...................###.....#......#........#....#####.....#............#...##..#.##..#..#....
#...#.#.##..#.....#.....#...#.#...#..................#####....#.....#.....#...#..........#...#............#...#...#.#...#.#..##.
.###..#......###...###...###..#...#.................#######...#####........###.......#....###............###...###...###.....##.
......#.........................................................................................................................
################################################################################################################################
M5
................................................................................................................................
..#...........#.......................#.....#......................................#####.........#....###............###..##....
.#.#..........#.......................#................................................#.....#..##...#...#..........#...#.##..#.
#...#.#...#.#####..###..##.#...##...#####..##....###..#.##...####.....................#.....#....#.......#..........#...#....#..
#...#.#...#...#...#...#.#.#.#....#....#.....#...#...#.##..#.#........................##....#.....#....###............####...#...
#####.#...#...#...#...#.#.#.#..###....#.....#...#...#.#...#..###.......................#..#......#...#..................#..#....
#...#.#..##...#.#.#...#.#.#.#.#..#....#.#...#...#...#.#...#.....#..................#...#.#.......#...#.................#..#..##.
#...#..##.#....#...###..#.#.#..####....#...###...###..#...#.####....................###.........###..#####..........###......##.
................................................................................................................................
################################################################################################################################
M6
................................................................................................................................
#####...#....##................#............................#...................#..........#.....#.......#.......#...###..##....
#.............#.................#.........................#.#...................#.........###...##......#.#.#...##..#...#.##..#.
#......##.....#....###...####....#.........###...####....#..#.##...###...###..#####.......###....#.........#...#.#......#....#..
####....#.....#...#...#.#.........#.......#..##.#.......#...##..#.#...#.#...#...#.........###....#............#..#...###....#...
#.......#.....#...#####..###.....#........#..##..###...#....#...#.#...#.#...#...#........#####...#............#####.#......#....
#.......#.....#...#.........#...#..........##.#.....#.#.....##..#.#...#.#...#...#.#..............#...............#..#.....#..##.
#......###...###...###..####...#....#.#.#.....#.####........#.##...###...###.....#.........#....###..............#..#####....##.
...........................................###..................................................................................
################################################################################################################################
M7
................................................................................................................................
#...#...#...........................#.......................................................#######...........#...#..###..####..
##.##...............................#........................................................#...#.....###....#...#.#...#.#...#.
#.#.#..##....###..#.##...###..#.##..#.##...###..#.##...###....................................#.#.....#####...#...#.#.....#...#.
#.#.#...#...#...#.##..#.#...#.##..#.##..#.#...#.##..#.#...#....................................#......#####...#...#..###..####..
#.#.#...#...#.....#.....#...#.##..#.#...#.#...#.#...#.#####...................................###.....#####...#...#.....#.#...#.
#...#...#...#...#.#.....#...#.#.##..#...#.#...#.#...#.#......................................#####.....###....#...#.#...#.#...#.
#...#..###...###..#......###..#.....#...#..###..#...#..###..................................#######............###...###..####..
..............................#.................................................................................................
################################################################################################################################
K1
#####.........#.....................####..........................................#.................................#...........
#.............#.....................#...#.........................................#.................................#...........
#.....#.##..#####..###..#.##........#...#..##....####..####.#...#..###..#.##...##.#...#........................##...#.##...###..
####..##..#...#...#...#.##..#.......####.....#..#.....#.....#...#.#...#.##..#.#..##..............................#..##..#.#...#.
#.....#...#...#...#####.#...........#......###...###...###..#.#.#.#...#.#.....#...#...#........................###..#...#.#.....
#.....#...#...#.#.#.....#...........#.....#..#......#.....#.#.#.#.#...#.#.....#..##...........................#..#..##..#.#...#.
#####.#...#....#...###..#...........#......####.####..####...#.#...###..#......##.#............................####.#.##...###..
................................................................................................................................
................................................................................................................................
................................................................................................................................
K2
#####.........#.....................####....................................................#######.................#...........
#.............#.....................#...#....................................................#...#.....###..........#...........
#.....#.##..#####..###..#.##........#...#..##....####..####.#...#..###..#.##..................#.#.....#####....##...#.##...###..
####..##..#...#...#...#.##..#.......####.....#..#.....#.....#...#.#...#.##..#..................#......#####......#..##..#.#...#.
#.....#...#...#...#####.#...........#......###...###...###..#.#.#.#...#.#.....................###.....#####....###..#...#.#.....
#.....#...#...#.#.#.....#...........#.....#..#......#.....#.#.#.#.#...#.#....................#####.....###....#..#..##..#.#...#.
#####.#...#....#...###..#...........#......####.####..####...#.#...###..#.....#.#.#.........#######............####.#.##...###..
................................................................................................................................
................................................................................................................................
................................................................................................................................
K3
#...#............................#......................#...#.......................................#######.....#...####...###..
#...#...........................#.#.....................#...#........................................#...#.....#.#..#...#.#...#.
##..#..##...##.#...###..........#....###..#.##........#####.#.##...###........#.##...###..............#.#.....#...#.#...#.#.....
#.#.#....#..#.#.#.#...#........###..#...#.##..#.........#...##..#.#...#.......##..#.#...#..............#......#...#.####..#.....
#..##..###..#.#.#.#####.........#...#...#.#.............#...#...#.#####.......#...#.#####.............###.....#####.#...#.#.....
#...#.#..#..#.#.#.#.............#...#...#.#.............#.#.#...#.#...........#...#.#................#####....#...#.#...#.#...#.
#...#..####.#.#.#..###..........#....###..#..............#..#...#..###........#...#..###..#.#.#.....#######...#...#.####...###..
................................................................................................................................
................................................................................................................................
................................................................................................................................
K4
.###..................................#.....#................................................##.....#..................#...###..
#...#.................................#.......................................................#.......................##..#...#.
#.....#...#..###...###...###...####.#####..##....###..#.##...####...#..................##.....#....##....###...###...#.#......#.
.###..#...#.#..##.#..##.#...#.#.......#.....#...#...#.##..#.#............................#....#.....#...#...#.#...#.#..#...###..
....#.#...#.#..##.#..##.#####..###....#.....#...#...#.#...#..###....#..................###....#.....#...#.....#####.#####.#.....
#...#.#..##..##.#..##.#.#.........#...#.#...#...#...#.#...#.....#.....................#..#....#.....#...#...#.#........#..#.....
.###...##.#.....#.....#..###..####.....#...###...###..#...#.####.......................####..###...###...###...###.....#..#####.
.............###...###..........................................................................................................
................................................................................................................................
................................................................................................................................
"""

NOTES = {'M1': 'Normal: System Status, 42%, 3 unread.', 'M2': 'The same while an OLED command runs: bell and 42% stay, the hourglass takes title room.', 'M3': 'Worst case: Set>notifications, ~100%, 10+ unread, capture, busy, context 12/34 (context drops).', 'M4': 'Short title keeps its context: Speech, ~100%, 3 unread, busy, context 2/5 (busy left of context).', 'M5': 'Automations page 3/12, battery 9% (18 px power slot).', 'M6': 'Path cut that keeps the current folder: Files>/system/logs/boot, ~42%, 1 unread.', 'M7': 'Capture while a command runs, on USB.', 'K1': 'Keyboard line: Enter Password:, page abc.', 'K2': 'Keyboard line while a command runs and a capture is live: the prompt is cut, abc stays at the edge.', 'K3': 'Keyboard line: a long prompt cut with the mark, ABC, while a command runs.', 'K4': 'Suggestions page: the typed text in the mode slot.'}


def goldens() -> dict[str, list[str]]:
    lines = GOLDEN.strip("\n").split("\n")
    frames = {}
    for i in range(0, len(lines), 11):
        frames[lines[i]] = lines[i + 1:i + 11]
    return frames


def static_failures() -> list[str]:
    failures = []
    utils = (COMPONENT / "OLED_Utils.cpp").read_text()
    if "oledCommandBusyRender" in utils:
        failures.append("OLED_Utils.cpp still draws the content-area busy box")
    if "OledHeaderInfo" in utils or "OledHeaderInfo" in (COMPONENT / "OLED_Utils.h").read_text():
        failures.append("OledHeaderInfo is back")
    start = utils.find("int oledRenderHeader(")
    if start < 0:
        failures.append("oledRenderHeader is missing")
    else:
        body = utils[start:utils.index("\n}\n", start)]
        for call in ("oled_header::compose(", "oled_header::draw(", "oledHeaderCollect(", "headerFunc"):
            if call not in body:
                failures.append(f"oledRenderHeader does not call {call}")
        if re.search(r"->(print|setCursor)\(", body):
            failures.append("oledRenderHeader draws by itself instead of through oled_header::draw")
    dirty = utils[utils.index("bool oledIsDirty()"):]
    dirty = dirty[:dirty.index("\n}\n")]
    if "oledHeaderSignatureNow()" not in dirty or "sHeaderDrawnSig" not in dirty:
        failures.append("oledIsDirty does not watch the header status")
    return failures


# The band guard (literal coordinates only: a computed y is caught on the
# device by oledHeaderBandCheck under DEBUG_DISPLAY). In-frame painters must not
# draw at a literal y of 0-9 while the bar is shown. A black fill is not ink.
DRAW_CALL = re.compile(
    r"\b(?:oledDisplay|gDisplay|display|d)->(setCursor|fillRect|drawRect|drawFastHLine|drawFastVLine|"
    r"drawPixel|drawLine|fillCircle|drawCircle|drawBitmap|drawChar|fillRoundRect|drawRoundRect|"
    r"fillTriangle|drawTriangle)\(\s*((?:[^,()]|\((?:[^()]|\([^()]*\))*\))+),\s*([0-9])\s*[,)]([^;]*);")
# Painters that never run while the bar is shown (header off: the logo, the
# animations, the boot and setup screens), with the reason.
BAND_ALLOWED_FILES = {
    "OLED_Mode_Animations.cpp": "animations and the boot progress own the whole display (header off)",
    "OLED_SetupWizard.cpp": "the setup wizard draws its own top line outside the frame",
    "OLED_FirstTimeSetup.cpp": "first-time setup draws outside the frame",
}
BAND_ALLOWED_LINES = {
    ("OLED_Utils.cpp", 'gDisplay->setCursor(0, 0);'): "the init splash, before the first frame",
}


def band_failures() -> list[str]:
    failures = []
    for sample, hit in (("display->setCursor(128 - (strlen(m) * 6), 0);", True),
                        ("oledDisplay->drawFastHLine(0, 9, 128, SSD1306_WHITE);", True),
                        ("oledDisplay->setCursor(0, 10);", False),
                        ("oledDisplay->setCursor(0, OLED_CONTENT_START_Y);", False)):
        if bool(DRAW_CALL.search(sample)) != hit:
            failures.append(f"band guard pattern self-check failed on: {sample}")
    sources = sorted(list(COMPONENT.glob("OLED_*.cpp")) + list(COMPONENT.glob("OLED_*.h")) +
                     list(COMPONENT.glob("*_oled.h")) + list(COMPONENT.glob("*_OLED.h")))
    for path in sources:
        if path.name in BAND_ALLOWED_FILES or path.name == "OLED_HeaderCore.h":
            continue
        for number, line in enumerate(path.read_text().splitlines(), 1):
            match = DRAW_CALL.search(line)
            if not match:
                continue
            if "BLACK" in match.group(4) and match.group(1) == "fillRect":
                continue
            if (path.name, line.strip()) in BAND_ALLOWED_LINES:
                continue
            failures.append(f"{path.name}:{number} draws at a literal y of {match.group(3)} "
                            f"(header band 0-9): {line.strip()}")
    return failures


def part2_failures() -> list[str]:
    failures = []
    utils = (COMPONENT / "OLED_Utils.cpp").read_text()
    collect = utils[utils.index("static void oledHeaderCollect("):]
    collect = collect[:collect.index("\n}\n")]
    if "status.rec = oledCaptureLive();" not in collect:
        failures.append("the header does not collect the capture dot")
    live = utils[utils.index("static bool oledCaptureLive()"):]
    live = live[:live.index("\n}\n")]
    for word in ("audioCaptureActive()", "cameraCaptureBusy()"):
        if word not in live:
            failures.append(f"the capture dot does not read {word}")
    keyboard = utils[utils.index("void oledKeyboardDisplay(Adafruit_SSD1306* display)"):]
    keyboard = keyboard[:keyboard.index("bool oledKeyboardHandleInput(")]
    if re.search(r"print\(gOledKeyboardState\.title\)", keyboard) or "128 - (" in keyboard:
        failures.append("the keyboard draws its top line by hand instead of through oled_header::kb")
    if keyboard.count("oledKeyboardDrawLine(") < 4:
        failures.append("every keyboard page (grid, pattern, mic, suggestions) draws its top line through the core")
    line = utils[utils.index("static void oledKeyboardDrawLine("):]
    line = line[:line.index("\n}\n")]
    for call in ("oled_header::kb::compose(", "oled_header::kb::draw(", "sKeyboardLineDrawnSig"):
        if call not in line:
            failures.append(f"oledKeyboardDrawLine does not use {call}")
    dirty = utils[utils.index("bool oledIsDirty()"):]
    dirty = dirty[:dirty.index("\n}\n")]
    if "oledKeyboardLineSignatureNow()" not in dirty:
        failures.append("oledIsDirty does not watch the keyboard line")
    frame = utils[utils.index("void updateOledDisplay()"):]
    if frame.find("oledHeaderBandCheck(gDisplay)") < 0 or \
            frame.find("oledHeaderBandCheck(gDisplay)") > frame.find("oledRenderHeader(gDisplay)"):
        failures.append("the frame does not run the DEBUG_DISPLAY band check before the bar draws")
    check = utils[utils.index("static void oledHeaderBandCheck("):]
    check = check[:check.index("\n}\n")]
    if "isDebugFlagSet(DEBUG_DISPLAY)" not in check:
        failures.append("the band check runs without DEBUG_DISPLAY")
    if not re.search(r'\{ "oleddump", "[^"]*", true, cmd_oleddump,', utils):
        failures.append("oleddump is not an admin command")
    commit = utils[utils.index("static void oledCommitFrame()"):]
    commit = commit[:commit.index("\n}\n")]
    if "oledDumpCapture();" not in commit:
        failures.append("the dump does not copy the committed frame")
    map_source = (COMPONENT / "OLED_Mode_Map.cpp").read_text()
    if "gpsMapHeader" not in map_source[map_source.index("static const OledModeEntry gpsMapOledModes[]"):]:
        failures.append("the map does not register its header hook (popup titles)")
    return failures


def main() -> None:
    harness = HERE / "oled_header_harness.cpp"
    with tempfile.TemporaryDirectory(prefix="hw1-oled-header-") as directory:
        binary = Path(directory) / "oled_header"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                        "-I", str(COMPONENT), "-I", str(HERE),
                        "-I", str(COMPONENT.parent / "hardwareone_libs/Adafruit_GFX_Library"),
                        str(harness), "-o", str(binary)], check=True)
        run = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    sys.stderr.write(run.stderr)
    lines = run.stdout.strip("\n").split("\n")
    got = {lines[i]: lines[i + 1:i + 11] for i in range(0, len(lines), 11)}
    failures = static_failures() + band_failures() + part2_failures()
    for name, rows in goldens().items():
        if got.get(name) != rows:
            failures.append(f"{name} ({NOTES[name]}) differs from the golden frame")
            for y, (want, have) in enumerate(zip(rows, got.get(name, [""] * 10))):
                if want != have:
                    print(f"  {name} row {y}\n    want {want}\n    have {have}", file=sys.stderr)
    if set(got) != set(goldens()):
        failures.append(f"frames printed {sorted(got)} != golden {sorted(goldens())}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)
    print(f"OLED header: layout, title cut, power, signature, pixel regions, keyboard line, "
          f"band guard and {len(got)} golden frames passed")


if __name__ == "__main__":
    main()
