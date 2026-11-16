#!/usr/bin/env python3
"""Run the shipping bounded text field at each canvas geometry.

OLED_TextFieldCore.h compiles directly; OLED_TextField.cpp's implementation is
spliced into oled_text_field_harness.cpp over the shared GFX double
(oled_text_field_gfx_double.h) and a pixel recorder. ASan/UBSan always on.
"""
from pathlib import Path
import subprocess
import tempfile

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

GEOMETRIES = (
    ("oled-64", 64, 10, 10),
    ("waveshare-88", 88, 10, 10),
    ("p4x-eye-128", 128, 10, 10),
)


def text_field_implementation() -> str:
    source = (COMPONENT / "OLED_TextField.cpp").read_text()
    begin = "// BEGIN_TEXT_FIELD_IMPL"
    end = "// END_TEXT_FIELD_IMPL"
    require(begin in source and end in source, "OLED_TextField.cpp must keep its splice markers")
    body = source[source.index(begin):source.index(end)]
    require("print(" not in body and "println(" not in body,
            "the text field draws glyphs through the clipping adapter, never print()")
    return '#include "OLED_TextFieldCore.h"\n' + body


def main() -> None:
    harness = (HERE / "oled_text_field_harness.cpp").read_text()
    harness = harness.replace("// INSERT_PRODUCTION", text_field_implementation())
    with tempfile.TemporaryDirectory(prefix="hw1-oled-text-field-") as directory:
        unit = Path(directory) / "text_field.cpp"
        unit.write_text(harness)
        for name, height, header, footer in GEOMETRIES:
            binary = Path(directory) / name
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            "-fsanitize=address,undefined", "-g",
                            f"-DHW1_CANVAS_HEIGHT={height}", f"-DHW1_HEADER={header}", f"-DHW1_FOOTER={footer}",
                            "-I", str(COMPONENT), "-I", str(HERE),
                            "-I", str(COMPONENT.parent / "hardwareone_libs/Adafruit_GFX_Library"),
                            str(unit), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print("Text field: bounds at every geometry, ellipsis degradation, 1000 ms / 20 px/s / 1500 ms marquee, "
          "rebuild continuity, selection/width/owner resets, view and modal pauses, rollover passed")


if __name__ == "__main__":
    main()
