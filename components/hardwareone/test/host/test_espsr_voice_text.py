#!/usr/bin/env python3
"""Tasks other than the recognizer read the voice state as locked copies.

The speech recognizer (task sr_main) kept the detected category, subcategory
and the last command in three Strings and re-made them at every recognised
phrase and timeout. The OLED speech page, the status JSON (web server task,
cmd_exec) and the telemetry print read those Strings with no lock, through
accessors that copied them into further static Strings. A String that is
re-made while another task copies it can be read after its block was freed.

Now the recognizer publishes every change into fixed text under one lock
(voiceSetContext, voiceSetLastCommand in System_ESPSR.cpp) and a reader gets a
copy made under the same lock (getEspSrCurrentCategory, ...SubCategory,
...LastCommand into a caller buffer; voiceTextSnapshot for the status JSON).

espsr_voice_text_harness.cpp compiles that production code with the size
constants of System_ESPSR.h: values and partial updates, the esp-sr phrase
limit published whole, over-long text cut in the shared copy only, reader
buffers of 0, 1 and 16 bytes, every reader and writer waiting for the lock,
every copy made with the lock held, and readers that never see a mix of two
values while the recognizer rewrites all three.

Source contract (the rest of System_ESPSR.cpp is not compiled on the host):
the two recognizer Strings are assigned only in voiceSetContext and named only
by the recognizer's own functions; gLastCommand is gone; the shared text is
named only by its own functions and the three accessors; the accessors return
nothing and keep no static String; the OLED speech page passes buffers.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
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

BLOCK_START = "// The recognizer's own working values."
BLOCK_END = "static uint8_t gSrDebugLevel = 0;"
ACCESSORS = ("getEspSrCurrentCategory", "getEspSrCurrentSubCategory", "getEspSrLastCommand")
# The recognizer: the phrase callback, the task loop and the one writer.
RECOGNIZER = ("static void onVoiceCommandDetected(int commandId, const char* phrase)",
              "static void srTask(void*)",
              "static void voiceSetContext(const char* category, const char* subCategory)")
SHARED_TEXT_USERS = ("static void voiceSetContext(const char* category, const char* subCategory)",
                     "static void voiceSetLastCommand(const String& text)",
                     "static void voiceTextSnapshot(VoiceSharedText& out)",
                     "static void voiceCopyText(const char* sharedText, char* out, size_t outSize)")


def code_only(text: str) -> str:
    """The text without // comments (no line here holds a string with //)."""
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def definition(source: str, signature: str) -> str:
    """A function definition, skipping a forward declaration of the same signature."""
    position = source.index(signature)
    while source[position + len(signature):].lstrip().startswith(";"):
        position = source.index(signature, position + len(signature))
    return extract_block(source[position:], signature)


def production(component: Path) -> dict[str, str]:
    header = (component / "System_ESPSR.h").read_text()
    source = (component / "System_ESPSR.cpp").read_text()
    constants = re.findall(r"^constexpr size_t kEspSr\w+TextSize\b[^\n]*$", header, re.M)
    require(len(constants) == 2, "System_ESPSR.h holds the two text sizes")
    declarations = [f"void {name}(char* out, size_t outSize);" for name in ACCESSORS]
    for declaration in declarations:
        require(declaration in header, f"System_ESPSR.h declares `{declaration}`")
    require(source.count(BLOCK_START) == 1 and source.count(BLOCK_END) == 1, "one shared voice text block")
    block = source[source.index(BLOCK_START):source.index(BLOCK_END)]
    # The stage timeout declared between the two Strings and the shared text is not part of it.
    block = "\n".join(line for line in block.splitlines() if "gCategoryTimeoutMs" not in line)
    accessors = "\n\n".join(extract_block(source, f"void {name}(char* out, size_t outSize)") for name in ACCESSORS)
    return {"// INSERT_HEADER": "\n".join(constants + declarations),
            "// INSERT_PRODUCTION": block + "\n" + accessors}


def check_sources(component: Path) -> None:
    header = (component / "System_ESPSR.h").read_text()
    source = (component / "System_ESPSR.cpp").read_text()
    page = (component / "OLED_Mode_Speech.cpp").read_text()
    code = code_only(source)

    require("gLastCommand" not in code, "the String gLastCommand is gone; the last command is shared text only")
    for name in ACCESSORS:
        require(f"const char* {name}(" not in header and f"const char* {name}(" not in source,
                f"{name} no longer returns a pointer")
        body = extract_block(source, f"void {name}(char* out, size_t outSize)")
        require("static" not in body and "String" not in body, f"{name} keeps no static String")

    writer = definition(source, RECOGNIZER[2])
    for name in ("gCurrentCategory", "gCurrentSubCategory"):
        assigned = len(re.findall(rf"\b{name}\s*(?:\+=|=(?!=))", code))
        require(len(re.findall(rf"\b{name}\s*=(?!=)", code_only(writer))) == 1 and assigned == 2,
                f"{name} is assigned at its definition and in voiceSetContext only (found {assigned} assignments)")
    recognizer = "".join(code_only(definition(source, signature)) for signature in RECOGNIZER)
    for name in ("gCurrentCategory", "gCurrentSubCategory"):
        named = len(re.findall(rf"\b{name}\b", code))
        inside = len(re.findall(rf"\b{name}\b", recognizer))
        require(named == inside + 1,
                f"{name} is named by the recognizer only ({named - inside - 1} use(s) elsewhere); "
                "other tasks read the shared text")

    users = "".join(code_only(definition(source, signature)) for signature in SHARED_TEXT_USERS)
    users += "".join(code_only(extract_block(source, f"void {name}(char* out, size_t outSize)")) for name in ACCESSORS)
    named = len(re.findall(r"\bsVoiceText\b", code))
    inside = len(re.findall(r"\bsVoiceText\b", users))
    require(named == inside + 1, f"sVoiceText is named only by its own functions ({named - inside - 1} use(s) elsewhere)")
    for signature in SHARED_TEXT_USERS:
        body = code_only(definition(source, signature))
        require(body.count("portENTER_CRITICAL(&sVoiceTextMux);") == 1 and
                body.count("portEXIT_CRITICAL(&sVoiceTextMux);") == 1 and "return" not in
                body[body.index("portENTER_CRITICAL"):body.index("portEXIT_CRITICAL")],
                f"{signature.split('(')[0].split()[-1]} enters and leaves the lock once, with no return inside")

    status = extract_block(source, "void buildEspSrStatusJson(String& output)")
    require("voiceTextSnapshot(voiceText);" in status and 'doc["category"] = voiceText.category;' in status and
            'doc["subcategory"] = voiceText.subCategory;' in status and
            'doc["lastCommand"] = voiceText.lastCommand;' in status,
            "the status JSON takes its three texts from one snapshot")
    require(re.search(r"\n  VoiceSharedText voiceText;\n", status) is not None,
            "the snapshot is a plain (non-const) local: a const char array would be stored as a literal with its padding")

    for name in ACCESSORS:
        calls = re.findall(rf"{name}\((\w+), sizeof\((\w+)\)\);", code_only(page))
        require(len(re.findall(rf"\b{name}\(", page)) == 1 and len(calls) == 1 and calls[0][0] == calls[0][1],
                f"the OLED speech page calls {name} once with its own buffer and that buffer's size")
    require("const char* cat" not in page and "const char* sub" not in page and "const char* lastCmd" not in page,
            "the OLED speech page keeps no pointer to recognizer text")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    harness = (HERE / "espsr_voice_text_harness.cpp").read_text()
    for marker, body in production(component).items():
        require(harness.count(marker) == 1, f"one {marker} in the harness")
        harness = harness.replace(marker, body)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espsr-voice-text-") as directory:
        source = Path(directory) / "voice_text.cpp"
        binary = Path(directory) / "voice_text"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=120)
    check_sources(component)
    print("voice text reaches other tasks as locked copies", flush=True)


if __name__ == "__main__":
    main()
