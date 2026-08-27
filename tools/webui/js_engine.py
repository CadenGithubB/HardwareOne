#!/usr/bin/env python3
"""Run a JavaScript harness on whatever JS engine this machine happens to have.

The web-UI tests execute the JS that ships inside the firmware's C++ raw string
literals. That needs a JS engine, and the developer machines involved do not
agree on which one exists: this Mac has no node at all (deno, Apple's jsc, and
osascript are what it offers), while a Linux CI box will usually have node and
nothing else. This module hides that, so a test can say "run this file, give me
its stdout lines" and get the same answer on all of them.

Covered here:
  - engine discovery, in a fixed preference order (find_js_engine)
  - the per-engine command line, which is where the real differences live
  - concatenating harness/_bootstrap.js in front of the script, so the harness
    gets __out / slurp / __argv / __engine no matter what is running it

NOT covered here: reading the firmware sources (tools/webui/extract_js.py) and
deciding whether a run passed. Deliberately not the latter, because:

    EXIT CODES DISAGREE ACROSS ENGINES AND CANNOT BE USED AS PASS/FAIL.

Measured on this machine, same one-line `throw new Error("BOOM")` script:

    engine   exit   where the trace lands
    ------   ----   ---------------------
    deno       1    stderr
    jxa        1    stderr
    jsc        3    STDOUT -- it pollutes the very stream you are checking

and jsc's own `quit(3)` exits 0, so an "I failed on purpose" signal from inside
the script does not survive either. A caller therefore decides pass/fail from a
SENTINEL LINE IN STDOUT and never from returncode. The returncode is still worth
printing in a failure report -- it just cannot be the verdict.

Usage as a library:

    from tools.webui.js_engine import JS_ENGINE, NoJsEngine, run_js

    if JS_ENGINE is None:
        raise unittest.SkipTest("no JavaScript engine available")
    result = run_js("tools/webui/harness/check_foo.js", ["--verbose"])
    lines = result.stdout.splitlines()

Usage by hand, to run a harness on whatever is present and see everything:

    python3 -m tools.webui.js_engine tools/webui/harness/check_foo.js [args...]
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BOOTSTRAP = Path(__file__).resolve().parent / "harness" / "_bootstrap.js"

# jsc ships inside the JavaScriptCore framework and is NOT on PATH, so it needs
# an existence check rather than shutil.which. Note "Helpers", not "Resources":
# the Resources path is in a lot of blog posts and does not exist on this OS.
JSC_HELPER = (
    "/System/Library/Frameworks/JavaScriptCore.framework"
    "/Versions/A/Helpers/jsc"
)

# Preference order. node first because it is the least surprising thing to find
# on a CI box; deno next because it is the best behaved of the rest (real
# stdout, real argv, sandboxed reads); the Apple engines last because they are
# the most hostile targets and only exist on macOS. d8 is skipped on purpose --
# it is never installed on its own, only as part of a v8 checkout.
_PATH_ENGINES = (
    ("node", "node"),
    ("deno", "deno"),
    ("bun", "bun"),
    ("qjs", "qjs"),
    ("qjs", "quickjs"),
)


class NoJsEngine(Exception):
    """No usable JavaScript engine, or one that this module cannot drive."""


def find_js_engine() -> tuple[str, str] | None:
    """Return (engine_name, executable_path), or None if nothing is installed.

    The name is the same token _bootstrap.js resolves as __engine, so a caller
    and a harness can compare notes in one vocabulary. That is why osascript
    reports as "jxa": the binary is a script host, the engine it hosts is
    JavaScript for Automation, and the harness only ever sees the latter.
    """
    for name, binary in _PATH_ENGINES:
        found = shutil.which(binary)
        if found:
            return (name, found)
    if os.path.exists(JSC_HELPER):
        return ("jsc", JSC_HELPER)
    found = shutil.which("osascript")
    if found:
        return ("jxa", found)
    return None


# Resolved once, at import, so a test module can decorate with
# @unittest.skipUnless(JS_ENGINE, ...) at class definition time.
JS_ENGINE = find_js_engine()


def _read_roots(extra: Sequence[str | Path]) -> list[str]:
    """Directories a harness is allowed to read from, for engines that sandbox.

    The repo root is obvious -- harnesses read the page sources. The system temp
    directory is not, and leaving it out is a trap: callers hand a harness its
    inputs through tempfile.TemporaryDirectory() precisely so nothing is written
    into the source tree, and those files then live outside the repo. Deno
    refuses them with NotCapable, exits 1, and prints NOTHING to stdout -- which
    is indistinguishable from a harness that ran and reported nothing.

    Both the raw and the fully-resolved temp path are granted: on macOS
    gettempdir() is /var/folders/... which is a symlink into /private/var/...,
    and only one of the two will match depending on how the caller built its
    path.
    """
    roots: list[str] = [str(REPO_ROOT)]
    tmp = Path(tempfile.gettempdir())
    roots += [str(tmp), str(tmp.resolve())]
    roots += [str(Path(e).resolve()) for e in extra]
    seen: set[str] = set()
    return [r for r in roots if not (r in seen or seen.add(r))]


def _argv_for(name: str, executable: str, script: str,
              args: list[str], read_roots: list[str]) -> list[str]:
    """Build the command line. Every entry below is a measured requirement."""
    if name in ("node", "bun"):
        return [executable, script, *args]

    if name == "deno":
        # --allow-read is MANDATORY: without it Deno.readTextFileSync throws
        # NotCapable and the run exits 1 with empty stdout, which reads exactly
        # like a harness that printed nothing. The entry-point script itself is
        # exempt from the sandbox, but its DATA files are not -- see
        # _read_roots. Grant real paths, never a wildcard and never a guess.
        # The --no-* flags stop deno from picking up a deno.json, a lockfile or
        # the network just because the cwd is a project directory.
        # One flag PER ROOT. Deno accumulates repeated --allow-read, and a
        # comma-joined list would be split by deno on the same comma a path is
        # allowed to contain -- a checkout under /some,dir/repo would silently
        # grant two nonexistent paths and fail with NotCapable and empty stdout,
        # which reads exactly like a harness that ran and printed nothing.
        # Repeated flags remove the separator collision; a comma INSIDE one root
        # is still ambiguous to deno, so refuse loudly rather than guess.
        bad = [r for r in read_roots if "," in r]
        if bad:
            raise NoJsEngine(
                "deno cannot be granted read access to a path containing a "
                "comma (it is deno's own list separator): " + "; ".join(bad)
            )
        grants = ["--allow-read=" + r for r in read_roots]
        return [
            executable, "run", "--quiet", "--no-remote", "--no-config",
            "--no-lock", *grants, script, *args,
        ]

    if name in ("qjs", "quickjs"):
        return [executable, script, *args]

    if name == "jsc":
        # The "--" is MANDATORY. `jsc script.js ALPHA` treats ALPHA as a second
        # script FILE: it prints "Could not open file: ALPHA" and exits 3, and
        # top-level `arguments` is not even defined. With the separator the
        # arguments land in `arguments` as expected.
        return [executable, script, "--", *args]

    if name in ("jxa", "osascript"):
        return [executable, "-l", "JavaScript", script, *args]

    raise NoJsEngine(f"unknown engine name: {name!r}")


def run_js(script_path, args=(), timeout: int = 120,
           engine: tuple[str, str] | None = None,
           read_roots: Sequence[str | Path] = ()) -> subprocess.CompletedProcess:
    """Run a harness file and return the completed subprocess.

    harness/_bootstrap.js is prepended to the script and the combined source is
    written to one temp file, which is what actually runs. See the long comment
    at the top of _bootstrap.js for why concatenation is the only mechanism that
    works on both an ES-module engine (deno) and a classic-script engine (jsc,
    JXA) -- they disagree in OPPOSITE directions about top-level scope.

    `read_roots` adds directories a sandboxing engine may read, on top of the
    repo root and the system temp directory. Only deno sandboxes today; the
    argument is accepted and ignored elsewhere so callers need not branch.

    `engine` overrides discovery, as (name, executable_path). Tests use it to
    prove the same harness produces byte-identical stdout on every engine that
    is present; leave it None for normal use.

    Raises NoJsEngine if there is no engine to run on. Remember that a non-zero
    returncode is not a verdict -- see the module docstring.
    """
    resolved = engine if engine is not None else JS_ENGINE
    if resolved is None:
        raise NoJsEngine(
            "no JavaScript engine found (looked for node, deno, bun, qjs, "
            f"quickjs, {JSC_HELPER}, osascript)"
        )
    name, executable = resolved

    script = Path(script_path)
    if not script.is_absolute():
        script = REPO_ROOT / script
    combined = (
        BOOTSTRAP.read_text(encoding="utf-8")
        + "\n"
        + script.read_text(encoding="utf-8")
    )

    # delete=False plus an explicit unlink, because the child process opens the
    # file by name while this process still holds the handle.
    handle = tempfile.NamedTemporaryFile(
        "w", suffix=".js", prefix="hw1_webui_", delete=False, encoding="utf-8")
    try:
        handle.write(combined)
        handle.close()
        # NO_COLOR because deno writes raw ANSI escapes into its error output
        # even when stdout is a pipe, which makes a failure report unreadable
        # and breaks any test that compares stderr text.
        env = dict(os.environ, NO_COLOR="1")
        return subprocess.run(
            _argv_for(name, executable, handle.name, [str(a) for a in args],
                      _read_roots(read_roots)),
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=REPO_ROOT,
            env=env,
        )
    finally:
        handle.close()
        os.unlink(handle.name)


def _main(argv: list[str]) -> int:
    if not argv:
        print("usage: python3 -m tools.webui.js_engine <harness.js> [args...]",
              file=sys.stderr)
        return 2
    if JS_ENGINE is None:
        print("no JavaScript engine found; install node or deno",
              file=sys.stderr)
        return 2

    name, executable = JS_ENGINE
    result = run_js(argv[0], argv[1:])
    print(f"engine: {name} ({executable})")
    print("--- stdout ---")
    sys.stdout.write(result.stdout)
    print("--- stderr ---")
    sys.stdout.write(result.stderr)
    print(f"--- returncode: {result.returncode} "
          f"(not a verdict -- engines disagree; check a stdout sentinel) ---")
    return result.returncode


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
