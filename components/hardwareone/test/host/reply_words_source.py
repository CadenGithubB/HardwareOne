"""The reply words of the firmware as source text for a host harness.

A harness that compiles production code out of the firmware sources reads
replies through the real System_CommandReply.h and compares the real module
constants, so it cannot drift from them:

    reply_header_include(component)    an #include of System_CommandReply.h by
                                       its absolute path (a pure header; its
                                       #pragma once makes a second include of
                                       the same file harmless)
    module_reply_constants(component)  the replies a page compares, from the
                                       headers of the modules that return them
    reply_prelude(component)           both, for the top of a generated unit

A constant its header does not define is left out, so a test can still be
pointed at an older tree.
"""
from __future__ import annotations

from pathlib import Path
import re

MODULE_REPLIES = (
    ("System_Maps.h", "kGpsTrackReplyCleared"),
    ("System_Filesystem.h", "kFileReplyMissingOrDenied"),
    ("System_User.h", "kPasswordReplyWrongCurrent"),
    ("System_User.h", "kPasswordReplyMismatch"),
    ("System_Settings.h", "kSettingsReplySaved"),
    ("System_Settings.h", "kSettingsReplySavedNoChange"),
)


def reply_header_include(component: Path) -> str:
    return f'#include "{(Path(component) / "System_CommandReply.h").resolve()}"\n'


def module_reply_constants(component: Path) -> str:
    lines = []
    texts: dict[str, str] = {}
    for header, name in MODULE_REPLIES:
        if header not in texts:
            path = Path(component) / header
            texts[header] = path.read_text() if path.exists() else ""
        lines += ["[[maybe_unused]] " + line for line in
                  re.findall(r'^constexpr char %s\[\] = "[^"\n]*";$' % name, texts[header], re.M)]
    return "\n".join(lines) + ("\n" if lines else "")


def reply_prelude(component: Path) -> str:
    return reply_header_include(component) + module_reply_constants(component)
