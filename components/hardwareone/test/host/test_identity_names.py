#!/usr/bin/env python3
"""Reserved identities and role names have one owner: System_User.h.

The rule (CONTRIBUTING.md, "Identity and role names"):

1. System_User.h defines the names the firmware stamps on a context in place of
   an account (`kSystemUser`, `kAuthBypassUser`, `kUartSessionUser`,
   `kBondAdminUser`), the path of the G2 EvenAI worker (`kG2EvenAiPath`) and the
   four role names (`kRoleGuest`, `kRoleUser`, `kRoleAdmin`, `kRoleSuperAdmin`).
   Each is defined once.
2. No other source file spells one of these values as a literal. A check on a
   context asks a predicate of System_User.h (`isAuthBypassUser`,
   `hasNamedUser`, `isSystemIdentity`, `isG2EvenAiIdentity`, `bondIdentityLive`)
   and a role is compared with its constant.
3. `kUserRoleNames` lists the role names in rank order, so a page that shows
   or sends a role indexes it with a `kRoleRank*` value.

"AuthBypass", "uart-session", "bond-admin", "/g2evenai" and "superadmin" have
no other meaning in the firmware, so every literal use fails. "system",
"guest", "user" and "admin" are also words of other vocabularies (a
sub-command, a notification level, a chat turn, a JSON key), so they fail
where the code around them names a user or a role.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from identity_names_source import identity_predicates
from test_shared_text_helpers import blank_comments, sources

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
OWNER = "System_User.h"

CONSTANTS: dict[str, str] = {
    "kSystemUser": "system",
    "kAuthBypassUser": "AuthBypass",
    "kUartSessionUser": "uart-session",
    "kBondAdminUser": "bond-admin",
    "kG2EvenAiPath": "/g2evenai",
    "kRoleGuest": "guest",
    "kRoleUser": "user",
    "kRoleAdmin": "admin",
    "kRoleSuperAdmin": "superadmin",
}
PREDICATES = ("isAuthBypassUser", "hasNamedUser", "isSystemIdentity", "isG2EvenAiIdentity")
PREDICATE_PARTS = {
    "isAuthBypassUser": ("user == kAuthBypassUser",),
    "hasNamedUser": ("ctx.user.length() > 0", "!isAuthBypassUser(ctx.user)"),
    "isSystemIdentity": ("ctx.transport == SOURCE_INTERNAL && ctx.user == kSystemUser",),
    "isG2EvenAiIdentity": ("ctx.transport == SOURCE_INTERNAL && ctx.user == kUartSessionUser",
                           "ctx.path == kG2EvenAiPath"),
}
RANKS = ("kRoleRankGuest", "kRoleRankUser", "kRoleRankAdmin", "kRoleRankSuperAdmin")
RANK_ORDER = ("kRoleGuest", "kRoleUser", "kRoleAdmin", "kRoleSuperAdmin")

UNIQUE = ("AuthBypass", "uart-session", "bond-admin", "/g2evenai", "superadmin")
UNIQUE_LITERAL = re.compile(r'"(%s)"' % "|".join(map(re.escape, UNIQUE)))
ROLE_WORD = r'"(guest|user|admin|superadmin)"'
CONTEXTUAL = (
    # role == "guest", account.role = "user", String role = "admin"
    (re.compile(r'\b\w*[Rr]ole\w*(?:\.c_str\(\))?\s*(?:==|!=|=)\s*(?:String\(\s*)?' + ROLE_WORD),
     "a role is compared or set with a literal; use the kRole* constant"),
    (re.compile(r'\bstr(?:case)?cmp\s*\(\s*\w*[Rr]ole\w*\s*,\s*' + ROLE_WORD),
     "a role is compared with a literal; use the kRole* constant"),
    # "guest" is an account role wherever it is compared
    (re.compile(r'(?:==|!=)\s*"guest"'),
     'compared with "guest"; use kRoleGuest'),
    # doc["role"] | "user", entry["role"] = "admin"
    (re.compile(r'\[\s*"role"\s*\]\s*(?:\||=)\s*' + ROLE_WORD),
     'a "role" field gets a literal role; use the kRole* constant'),
    # ctx.user == "system", actx.user = "system"
    (re.compile(r'\b\w*[Uu]ser\w*\s*(?:==|!=|=)\s*(?:String\(\s*)?"system"'),
     'a user name is compared or set with "system"; use kSystemUser or isSystemIdentity'),
)
# Files whose "role" field is the speaker of a chat turn, not an account role.
CHAT_ROLE_FILES = {"System_LLMHttp.cpp", "System_LLMCommands.cpp", "WebPage_LLM.cpp"}


def check_owner(header: str) -> list[str]:
    failures = []
    for name, value in CONSTANTS.items():
        found = re.findall(r'^inline constexpr const char\* %s = "([^"\n]*)";$' % name, header, re.M)
        if found != [value]:
            failures.append(f'{OWNER} must define {name} once as "{value}" '
                            f"(inline constexpr const char*), found {found}")
    for name in PREDICATES:
        if len(re.findall(r"^inline bool %s\(" % name, header, re.M)) != 1:
            failures.append(f"{OWNER} must define the predicate {name} once")
    # A reserved name counts only together with its transport (and path).
    bodies = identity_predicates(header)
    for name, parts in PREDICATE_PARTS.items():
        body = re.search(r"^inline bool %s\(.*?(?=^inline bool |\Z)" % name, bodies, re.M | re.S)
        text = " ".join(body.group(0).split()) if body else ""
        for part in parts:
            if part not in text:
                failures.append(f"{OWNER}: {name} must test `{part}`")
    ranks = [re.findall(r"^constexpr int %s\s*=\s*(-?\d+);" % name, header, re.M) for name in RANKS]
    if ranks != [["0"], ["1"], ["2"], ["3"]]:
        failures.append(f"{OWNER}: the ranks {', '.join(RANKS)} must be 0, 1, 2, 3; found {ranks}")
    table = re.search(r"kUserRoleNames\[4\]\s*=\s*\{([^}]*)\}", header)
    order = tuple(part.strip() for part in table.group(1).split(",")) if table else ()
    if order != RANK_ORDER:
        failures.append(f"{OWNER}: kUserRoleNames must list {', '.join(RANK_ORDER)} in rank order, "
                        f"found {order}")
    return failures


def measure(component: Path):
    failures: list[str] = []
    checked = 0
    header = None
    for relative, text in sources(component, (".h", ".cpp", ".c", ".inc")):
        base = relative.rsplit("/", 1)[-1]
        if base == OWNER and "/" not in relative:
            header = text
            continue
        checked += 1
        code = blank_comments(text, keep_strings=True)

        def report(match, why):
            number = code.count("\n", 0, match.start()) + 1
            failures.append(f"{relative}:{number} {why}")

        for match in UNIQUE_LITERAL.finditer(code):
            name = next(k for k, v in CONSTANTS.items() if v == match.group(1))
            report(match, f'spells "{match.group(1)}"; use {name} of {OWNER} or the predicate that asks for it')
        for pattern, why in CONTEXTUAL:
            for match in pattern.finditer(code):
                if base in CHAT_ROLE_FILES and '"role"' in match.group(0):
                    continue
                if match.group(0).endswith('"superadmin"'):
                    continue   # reported by the unique-literal check
                report(match, why)
    if header is None:
        failures.append(f"{OWNER} not found under {component}")
    else:
        failures.extend(check_owner(header))
    return checked, sorted(set(failures))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    checked, failures = measure(component)
    if not checked:
        raise SystemExit(f"identity names: no sources under {component}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"identity names: {len(failures)} violation(s)")
    print(f"identity names: {checked} source files, {len(CONSTANTS)} names and "
          f"{len(PREDICATES)} predicates owned by {OWNER}")


if __name__ == "__main__":
    main()
