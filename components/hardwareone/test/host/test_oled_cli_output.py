#!/usr/bin/env python3
"""Run the shipping CLI Output page over the shipping console owner.

The page body is spliced out of OLED_Mode_CLI.cpp and the owner's methods out
of OLED_ConsoleBuffer.cpp into oled_cli_output_harness.cpp, which supplies a
bounds-checking framebuffer, input/mode doubles and FreeRTOS/allocator/clock
doubles. It runs for the 128x64 OLED, the 128x88 Waveshare canvas, the
P4X-EYE's 128x128 monochrome canvas (10/10 chrome) and the 12/16 color chrome
geometry. ASan/UBSan always on. `--preview-dir <path>` writes PGM renders.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_oled_console_buffer import production_methods
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

GEOMETRIES = (
    ("oled-64", 64, 10, 10),
    ("waveshare-88", 88, 10, 10),
    ("p4x-eye-128", 128, 10, 10),
    ("color-chrome-128", 128, 12, 16),
)


def source_guards() -> None:
    page = (COMPONENT / "OLED_Mode_CLI.cpp").read_text()
    require("DEBUG_" not in page and "System_Debug.h" not in page,
            "the viewer keeps no instrumentation that could enter the stream it shows")
    require("xSemaphoreTake" not in page and "gOledConsole.mutex" not in page,
            "the viewer never takes the producer lock itself")
    utils = (COMPONENT / "OLED_Utils.cpp").read_text()
    require("oledCLIViewerNeedsFrame()" in extract_block(utils, "bool oledIsDirty()"),
            "new console output must invalidate the display through the dirty poll")
    transients = extract_block(utils, "static void oledResetLocalDisplaySessionTransients()")
    require("oledCLIViewerResetSessionState()" in transients and "gOledConsole.clear()" in transients,
            "identity transitions must erase the viewer's owned copies and the ring")
    require("hints = oledCLIViewerFooterHint();" in utils, "the footer shows the page's own state hint")
    cli_input = (COMPONENT / "OLED_Mode_CLIInput.cpp").read_text()
    require("getLineCount" not in cli_input and "sPreSubmitCount" not in cli_input,
            "CLI Input no longer guesses from record counts")
    require("broadcastOutputCore_Routed(text, strlen(text), MSG_ROUTE_OLED)" in cli_input,
            "CLI Input hands echo and reply to the OLED sink through the output queue")
    require("gOledConsole" not in cli_input, "CLI Input does not append to the ring directly")


def generate() -> str:
    page = (COMPONENT / "OLED_Mode_CLI.cpp").read_text()
    body = page[page.index("enum class CliView"):page.index("static const OLEDModeEntry")]
    harness = (HERE / "oled_cli_output_harness.cpp").read_text()
    return harness.replace("// INSERT_CONSOLE_METHODS", production_methods()).replace("// INSERT_PAGE", body)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    source_guards()
    if args.preview_dir:
        args.preview_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hw1-oled-cli-output-") as directory:
        unit = Path(directory) / "cli_output.cpp"
        unit.write_text(generate())
        for name, height, header, footer in GEOMETRIES:
            binary = Path(directory) / name
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            "-fsanitize=address,undefined", "-g", "-pthread",
                            f"-DHW1_CANVAS_HEIGHT={height}", f"-DHW1_HEADER={header}", f"-DHW1_FOOTER={footer}",
                            "-I", str(COMPONENT), "-I", str(COMPONENT.parent / "hardwareone_libs/Adafruit_GFX_Library"),
                            str(unit), "-o", str(binary)], check=True)
            command = [str(binary)]
            if args.preview_dir:
                out = args.preview_dir / name
                out.mkdir(exist_ok=True)
                command.append(str(out))
            subprocess.run(command, check=True)
    print("CLI Output: Live/Paused/Reader, stable IDs, arrivals, expiry, pinned Reader, "
          "sanitized text, busy ring retry, lock-free drawing, session reset and render bounds passed")


if __name__ == "__main__":
    main()
