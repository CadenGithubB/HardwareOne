#!/usr/bin/env python3
"""Run the OLED console history owner as it ships.

OLED_ConsoleCore.h compiles directly; OLEDConsoleBuffer's methods are spliced
out of OLED_ConsoleBuffer.cpp over doubles for the FreeRTOS mutex, allocator,
clock and settings (oled_console_buffer_harness.cpp). ASan/UBSan always on.
"""
from pathlib import Path
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

METHODS = (
    "OLEDConsoleBuffer::OLEDConsoleBuffer()",
    "void OLEDConsoleBuffer::init()",
    "bool OLEDConsoleBuffer::isReady()",
    "bool OLEDConsoleBuffer::lock(",
    "void OLEDConsoleBuffer::unlock()",
    "void OLEDConsoleBuffer::append(",
    "void OLEDConsoleBuffer::clear()",
    "uint32_t OLEDConsoleBuffer::generation()",
    "uint32_t OLEDConsoleBuffer::droppedCount()",
    "uint32_t OLEDConsoleBuffer::fencedCount()",
    "uint32_t OLEDConsoleBuffer::deliveryGeneration()",
    "bool OLEDConsoleBuffer::tryStatus(",
    "bool OLEDConsoleBuffer::tryCopyRecord(",
    "size_t OLEDConsoleBuffer::tryCopyWindow(",
)


def production_methods() -> str:
    source = (COMPONENT / "OLED_ConsoleBuffer.cpp").read_text()
    constants = source[source.index("namespace {"):source.index("}  // namespace") + len("}  // namespace")]
    return "\n\n".join([constants] + [extract_block(source, marker) for marker in METHODS])


def source_guards() -> None:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    require("gOledConsole.append(msg->text, (uint32_t)msg->timestamp, msg->oledGeneration)" in debug,
            "the output task must deliver OLED-routed envelopes through append() with their captured generation")
    require("msg->oledGeneration = gOledConsole.deliveryGeneration();" in debug,
            "enqueueChunk must capture the OLED delivery generation for every envelope")
    header = (COMPONENT / "System_Debug.h").read_text()
    require("uint32_t oledGeneration;" in header and "static_assert(sizeof(DebugMessage) == 304" in header,
            "DebugMessage carries the generation inside its padding")
    utils = (COMPONENT / "OLED_Utils.cpp").read_text()
    require("OLEDConsoleBuffer::" not in utils, "the buffer implementation lives in OLED_ConsoleBuffer.cpp")
    transients = extract_block(utils, "static void oledResetLocalDisplaySessionTransients()")
    require("gOledConsole.clear()" in transients, "identity transitions must clear the console")
    cmake = (COMPONENT / "CMakeLists.txt").read_text()
    require("OLED_ConsoleBuffer.cpp" in cmake, "OLED_ConsoleBuffer.cpp must be in the component source list")


def main() -> None:
    source_guards()
    harness = (HERE / "oled_console_buffer_harness.cpp").read_text()
    harness = harness.replace("// INSERT_PRODUCTION_METHODS", production_methods())
    with tempfile.TemporaryDirectory(prefix="hw1-oled-console-") as directory:
        unit = Path(directory) / "console.cpp"
        binary = Path(directory) / "console"
        unit.write_text(harness)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-fsanitize=address,undefined", "-g", "-pthread",
                        "-I", str(COMPONENT), str(unit), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
