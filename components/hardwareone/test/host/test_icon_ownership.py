#!/usr/bin/env python3
"""Compare production icon lookup/rendering with captured pre-move assets/pixels.

The generated asset source and handwritten lookup source compile unchanged.
Only the external display pixel sink and an optional malformed lookup result are
controlled. Rendering helpers and the named adapter are extracted unchanged.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
FIXTURE = HERE / "fixtures/icon_ownership_expected.json"

ARDUINO = r"""#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#define PROGMEM
using PGM_P = const char*;
#define pgm_read_byte(p) (*(p))
#define pgm_read_ptr(p) (*(p))
#define memcpy_P std::memcpy
#define strcpy_P std::strcpy
"""
DISPLAY = r"""#pragma once
#include <cstdint>
#include <map>
#include <utility>
class PixelRecorder {
 public:
  std::map<std::pair<int, int>, uint16_t> pixels;
  void drawPixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) pixels[{x,y}] = color;
  }
};
class Adafruit_GFX : public PixelRecorder {
 public:
  virtual ~Adafruit_GFX() = default;
  virtual void drawPixel(int x, int y, uint16_t color) = 0;
};
class Adafruit_SSD1306 : public Adafruit_GFX {
 public:
  void drawPixel(int x, int y, uint16_t color) override { PixelRecorder::drawPixel(x, y, color); }
};
class Adafruit_ST7789 : public Adafruit_GFX {
 public:
  void drawPixel(int x, int y, uint16_t color) override { PixelRecorder::drawPixel(x, y, color); }
};
#define SSD1306_BLACK 0
#define SSD1306_WHITE 1
#define SSD1306_INVERSE 2
#define ST77XX_BLACK 0
#define ST77XX_WHITE 65535
#define ST77XX_RED 63488
#define ST77XX_GREEN 2016
#define ST77XX_BLUE 31
#define ST77XX_CYAN 2047
#define ST77XX_MAGENTA 63519
#define ST77XX_YELLOW 65504
#define ST77XX_ORANGE 64800
"""


def production_code(source: Path, baseline: bool) -> str:
    if baseline:
        utils = (source / "System_Utils.cpp").read_text()
        lookup = extract_block(utils, "bool iconExists(") + "\n"
        lookup += utils[utils.index("static const struct {", utils.index("bool iconExists(")):
                        utils.index("bool loadIconData(")]
        renderer = "\n".join(extract_block(utils, marker) for marker in (
            "bool loadIconData(", "bool drawIcon(", "static inline bool getBitmapBit(",
            "bool drawIconScaled("))
        return lookup + "\n#if ENABLE_OLED_DISPLAY\n" + renderer + "\n#endif\n"
    ui = (source / "OLED_UI.cpp").read_text()
    utils = (source / "System_Utils.cpp").read_text()
    header = (source / "System_Utils.h").read_text()
    for obsolete in ("drawIconScaled(", "bool drawIcon(", "loadIconData(", "getIconNameForExtension(",
                     "iconExists(", "getIconPath(", "initIconSystem("):
        require(obsolete not in utils + header, f"old utility icon API remains: {obsolete}")
    renderer = "\n".join(extract_block(ui, marker) for marker in (
        "static inline bool getBitmapBit(", "bool oledDrawIcon(Adafruit_GFX*",
        "void oledDrawIcon(int x, int y, const char*"))
    require("Adafruit_SSD1306" not in renderer, "renderer still names an SSD1306 backend")
    adapter = extract_block(ui, "void oledDrawIcon(int x, int y, const char*")
    require(adapter.count("oledDrawIcon(") == 2, "named adapter must delegate once")
    return "#if ENABLE_OLED_DISPLAY\n" + renderer + "\n#endif\n"


def compile_and_run(args: argparse.Namespace, source: Path, baseline: bool) -> dict:
    generated = (source / "System_Icons.cpp").read_text()
    fixture = {"generated_source_sha256": hashlib.sha256(generated.encode()).hexdigest(), "variants": {}}
    with tempfile.TemporaryDirectory(prefix="hw1-icons-") as directory:
        work = Path(directory)
        for name in ("System_Icons.h", "HAL_Display.h", "OLED_Layout.h", "OLED_UI.h"):
            shutil.copy2(source / name, work / name)
        (work / "System_Icons.cpp").write_text(generated)
        (work / "Arduino.h").write_text(ARDUINO)
        flags = sorted(set(re.findall(r"!defined\((ENABLE_\w+)\)", generated)))
        config = "#pragma once\n" + "\n".join(f"#ifndef {flag}\n#define {flag} 1\n#endif" for flag in flags)
        config += "\n#define DISPLAY_TYPE_NONE 0\n#define DISPLAY_TYPE_SSD1306 1\n#define DISPLAY_TYPE_ST7789 2\n"
        config += "#define DISPLAY_TYPE_ILI9341 3\n#define DISPLAY_TYPE_ST7789_P4_EYE 4\n"
        (work / "System_BuildConfig.h").write_text(config)
        body = (HERE / "icon_ownership_harness.cpp").read_text().replace(
            "// INSERT_PRODUCTION", production_code(source, baseline))
        (work / "test.cpp").write_text(body)
        if not baseline:
            shutil.copy2(source / "System_Icons_Lookup.cpp", work / "System_Icons_Lookup.cpp")
        variants = (("oled-web", 1, 1), ("oled-only", 1, 0), ("web-only", 0, 1), ("headless", 0, 0))
        for name, oled, web in variants:
            graphics_headers = ("Adafruit_GFX.h", "Adafruit_SSD1306.h", "Adafruit_ST7789.h")
            if oled:
                (work / "pixel_recorder.h").write_text(DISPLAY)
                for graphics_header in graphics_headers:
                    (work / graphics_header).write_text('#include "pixel_recorder.h"\n')
            else:
                # Web/headless lookup must compile without any display library.
                for graphics_header in (*graphics_headers, "pixel_recorder.h"):
                    (work / graphics_header).unlink(missing_ok=True)
            for display in ((1,) if baseline or not oled else (1, 2)):
                compiler = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            "-Wno-unused-parameter", "-I" + str(work), f"-DENABLE_OLED_DISPLAY={oled}",
                            f"-DENABLE_HTTP_SERVER={web}", f"-DDISPLAY_TYPE={display}",
                            f"-DBASELINE_CAPTURE={int(baseline)}"]
                if args.sanitize:
                    compiler += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
                asset_object = work / "assets.o"
                # Rename only the lookup symbol so malformed metadata can be
                # injected without modifying generated read-only asset tables.
                subprocess.run([*compiler, "-DfindEmbeddedIcon=findProductionEmbeddedIcon", "-c",
                                str(work / "System_Icons.cpp"), "-o", str(asset_object)], check=True, timeout=60)
                binary = work / "test"
                extra = [] if baseline else [str(work / "System_Icons_Lookup.cpp")]
                subprocess.run([*compiler, str(work / "test.cpp"), str(asset_object), *extra,
                                "-o", str(binary)], check=True, timeout=60)
                output = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=60).stdout
                records = {}
                pixel_groups = {}
                for line in output.splitlines():
                    key, payload = line.split("\t", 1)
                    require(key not in records, f"duplicate icon record: {key}")
                    if key.startswith(("render:", "clipped:")):
                        # One reviewed digest per icon covers every scale and
                        # clipped placement, avoiding repeated fixture tables.
                        pixel_groups.setdefault(key.split(":")[1], []).append(line)
                    else:
                        records[key] = hashlib.sha256(payload.encode()).hexdigest()
                for name_key, lines in pixel_groups.items():
                    records["pixels:" + name_key] = hashlib.sha256("\n".join(lines).encode()).hexdigest()
                if name in fixture["variants"]:
                    require(records == fixture["variants"][name], f"non-SSD rendering differs: {name}")
                fixture["variants"][name] = records
    return fixture


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    updates = parser.add_mutually_exclusive_group()
    updates.add_argument("--capture-baseline", type=Path,
                         help="Explicit pre-move source snapshot; writes the historical fixture")
    updates.add_argument("--update-fixture", action="store_true",
                         help="Deliberately replace the fixture with current production output; review the diff")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    actual = compile_and_run(args, args.capture_baseline or COMPONENT, bool(args.capture_baseline))
    if args.capture_baseline or args.update_fixture:
        FIXTURE.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n")
        operation = "Captured historical icon baseline" if args.capture_baseline else "Updated icon fixture from current production"
        print(f"{operation}: {FIXTURE}")
        return
    expected = json.loads(FIXTURE.read_text())
    require(actual["generated_source_sha256"] == expected["generated_source_sha256"], "generated assets changed")
    for variant, records in actual["variants"].items():
        require(records.keys() == expected["variants"][variant].keys(), f"icon records changed: {variant}")
        for key, digest in records.items():
            require(digest == expected["variants"][variant][key], f"icon output changed: {variant}/{key}")
    print("PASS icon ownership: four feature combinations, asset/pixel fixtures, SSD1306 + ST7789 aliases")


if __name__ == "__main__":
    main()
