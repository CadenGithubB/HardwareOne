"""The owned text forms of the firmware as source text for a host harness.

    device_cipher_text(component)  kDeviceCipherPrefix, its offsets and
                                   isDeviceCipherText() from System_Settings.h;
                                   needs <string.h> and a String type before it
    mic_preference(component)      the micSource words (System_Microphone.h)
                                   and micPreferredSource() as shipped
                                   (System_Microphone.cpp); needs gSettings,
                                   AudioSource and a String type before it
    oled_mode_slugs(oled_source)   the kOledModeSlugs table of OLED_Utils.cpp

Text a tree does not have is left out, so a test can be pointed at an older
tree.
"""
from __future__ import annotations

from pathlib import Path
import re


def _between(text: str, start: str, end: str) -> str:
    if start not in text:
        return ""
    begin = text.index(start)
    return text[begin:text.index(end, begin) + len(end)]


def _maybe_unused(text: str) -> str:
    """A generated unit is the main file, where an unused constant is an error."""
    return re.sub(r"(?m)^constexpr ", "[[maybe_unused]] constexpr ", text)


def device_cipher_text(component: Path) -> str:
    header = (Path(component) / "System_Settings.h").read_text()
    text = _between(header, "constexpr char kDeviceCipherPrefix[]",
                    "inline bool isDeviceCipherText(const String& text) { return isDeviceCipherText(text.c_str()); }")
    return _maybe_unused(text) + "\n"


def mic_preference(component: Path) -> str:
    header = (Path(component) / "System_Microphone.h").read_text()
    source = (Path(component) / "System_Microphone.cpp").read_text()
    words = re.findall(r'^constexpr char kMicSource\w+\[\] = "[^"\n]*";$', header, re.M)
    reader = _between(source, "AudioSource micPreferredSource() {", "\n}\n")
    return _maybe_unused("\n".join(words)) + "\n" + reader


def oled_mode_slugs(oled_source: str) -> str:
    return _between(oled_source, "static const struct { const char* slug; OledMode mode; } kOledModeSlugs[] = {",
                    "\n};\n")
