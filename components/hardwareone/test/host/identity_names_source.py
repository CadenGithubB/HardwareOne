"""The identity and role names of System_User.h as source text for a host harness.

A harness that compiles production code out of the firmware sources gets the
names from the real header, so the harness cannot drift from it:

    identity_constants(header)   the reserved names, kG2EvenAiPath, the role
                                 names and kUserRoleNames; no dependencies
    identity_predicates(header)  isAuthBypassUser, hasNamedUser,
                                 isSystemIdentity, isG2EvenAiIdentity; place it
                                 after the harness's String, CommandSource and
                                 AuthContext

kBondAdminUser is not included: the harnesses that need it already take its
line from the header. A name the header does not define is left out, so a test
can still be pointed at an older tree.
"""
from __future__ import annotations

import re

CONSTANTS = ("kSystemUser", "kAuthBypassUser", "kUartSessionUser", "kG2EvenAiPath",
             "kRoleGuest", "kRoleUser", "kRoleAdmin", "kRoleSuperAdmin")
PREDICATES = ("isAuthBypassUser", "hasNamedUser", "isSystemIdentity", "isG2EvenAiIdentity")


def identity_constants(header: str) -> str:
    lines = []
    for name in CONSTANTS:
        lines += re.findall(r'^inline constexpr const char\* %s = "[^"\n]*";$' % name, header, re.M)
    lines += re.findall(r"^inline constexpr const char\* const kUserRoleNames\[4\] = \{[^}]*\};$",
                        header, re.M)
    return "\n".join(lines)


def identity_predicates(header: str, names: tuple[str, ...] = PREDICATES) -> str:
    found = []
    for name in names:
        match = re.search(r"^inline bool %s\([^)]*\) \{(?:[^\n]*\}$|.*?^\})" % name, header, re.M | re.S)
        if match:
            found.append(match.group(0))
    return "\n".join(found)
