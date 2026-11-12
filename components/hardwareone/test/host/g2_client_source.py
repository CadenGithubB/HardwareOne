"""Text of the G2 glasses client for tests that slice production code.

The client was one file, G2_Glasses.cpp. It is now the core file, the private
header G2_GlassesInternal.h and the files in SPLIT_FILES. A test that takes a
function out by its signature, or checks that a pattern is absent from the
client, does not depend on which of these files holds the code:
g2_client_text() returns them joined: the core first, then the split files in
the order below, then the private header. The header is last so that a search
for a signature finds the definition before the prototype.

A tree from before the split (a --baseline directory) has only the core. Files
that do not exist are skipped.

A file that is split out of G2_Glasses.cpp later is added to SPLIT_FILES.
"""
from __future__ import annotations

from pathlib import Path

PRIVATE_HEADER = "G2_GlassesInternal.h"
CORE_FILE = "G2_Glasses.cpp"
SPLIT_FILES = (
    "G2_Keyboard.cpp",
    "G2_ImageProbes.cpp",
    "G2_ImageViewers.cpp",
    "G2_Page_Launchers.cpp",
    "G2_Page_Microphone.cpp",
    "G2_Page_SystemEvents.cpp",
    "G2_Page_Logging.cpp",
    "G2_Page_FMRadio.cpp",
    "G2_Page_LLM.cpp",
    "G2_Page_Map.cpp",
    "G2_Page_Health.cpp",
)
CLIENT_FILES = (CORE_FILE,) + SPLIT_FILES + (PRIVATE_HEADER,)


def g2_client_files(component) -> list[tuple[str, str]]:
    """(file name, text) for each client file that exists, in CLIENT_FILES order."""
    component = Path(component)
    found = []
    for name in CLIENT_FILES:
        path = component / name
        if path.is_file():
            found.append((name, path.read_text(encoding="utf-8", errors="replace")))
    if not any(name == CORE_FILE for name, _ in found):
        raise FileNotFoundError(f"{component / CORE_FILE} does not exist")
    return found


def g2_client_text(component, header_first: bool = False) -> str:
    """The client files joined into one text.

    header_first puts the private header before the core. Use it where a test
    takes the last match of a signature (`rindex`) to skip a forward
    declaration: the definition is then the last match again.
    """
    files = g2_client_files(component)
    if header_first:
        files = [f for f in files if f[0] == PRIVATE_HEADER] + [f for f in files if f[0] != PRIVATE_HEADER]
    return "\n".join(text for _, text in files)


def g2_client_text_via(read) -> str:
    """As g2_client_text, for a test that has its own reader (an overlay directory, for example)."""
    parts = []
    for name in CLIENT_FILES:
        try:
            parts.append(read(name))
        except FileNotFoundError:
            if name == CORE_FILE:
                raise
    return "\n".join(parts)


def g2_internal_declaration(component, signature: str) -> str:
    """The declaration in the private header that starts with `signature`, with its `;`.

    A function that is shared between the client files has its default
    arguments in the private header only. A harness that compiles the
    definition puts this declaration before it.
    """
    header = (Path(component) / PRIVATE_HEADER).read_text(encoding="utf-8", errors="replace")
    start = header.index(signature)
    return header[start:header.index(";", start) + 1]
