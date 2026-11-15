#!/usr/bin/env python3
"""A flag or counter that more than one task changes changes in one atomic step.

`x |= bit`, `x &= ~bit` and `++x` on a plain or volatile integer are a load, a
change and a store. Two tasks that do this to the same variable lose one of the
two changes when one is scheduled between the other's load and store. The
variables below were changed that way from several tasks; each is now a
std::atomic and each change is one atomic operation.

Output lanes (gOutputFlags, System_Debug.h): command handlers on cmd_exec open
and close lanes (`log start`, outble, outg2, outserial, the HTTP server start)
while the main loop closes the BLE and WEB lanes at a session end or server
stop, and applySettings rewrites the SERIAL lane from both tasks.

Glasses tap masks (gCamStreamPendingTap, gHealthPagePendingTap): the event
dispatch on g2_owner records a tapped row with |= while the page worker on
g2_session drains the mask. The drain was a read followed by a store of 0; a
tap recorded between the two was erased. It is one exchange(0) now.

G2 hijack bridge (gMenuGen, gCmdSeq in G2_HijackCmd.cpp): g2BumpMenuGen and
g2SubmitHijackCommand run on the tap, session, page and command tasks on both
cores. A lost menu bump lets a stale completion pass the menuGen check; a
64-bit ++ gave two submissions one number. Both are std::atomic<uint32_t>.

Generation counters (gNotifPrefsGen in System_Notifications.cpp, gHealthPageGen
in G2_Glasses.cpp): bumped with ++ from the main loop, cmd_exec and the G2 tap
dispatcher. Two Health page opens could get one generation, so the older
worker's deferred onDone was not recognised as stale.

Not covered here on purpose: sRxQueueDropped (G2_Ring.cpp) and gDebugDropped
(System_Debug.h) are diagnostic counters that are only printed;
System_Utils.h (observeHwm) records that a lost count is accepted for them.

The production definition of each variable and the production statements that
change it are compiled into shared_flag_atomics_harness.cpp and run against
each other on real threads; the harness states the invariant each run checks.
Source contract: one definition and one declaration per variable, both atomic,
and no statement that stores a value computed from an earlier read.

Options: --cxx <compiler>, --sanitize (accepted; the threads run without a
sanitizer because the pre-fix data race is the thing under test), --component
<dir> to run the same cases against another components/hardwareone directory.
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

LANES = ("SERIAL", "WEB", "FILE", "BLE", "G2")


def firmware_sources(component: Path) -> list[Path]:
    found = []
    for pattern in ("*.cpp", "*.h"):
        for path in component.rglob(pattern):
            parts = path.relative_to(component).parts
            if "test" in parts or path.name.startswith("._"):
                continue
            found.append(path)
    return sorted(found)


def code_lines(text: str):
    """Source lines with their // comment removed (none of these lines holds a string with //)."""
    for number, line in enumerate(text.splitlines(), 1):
        yield number, line.split("//", 1)[0]


def definition(text: str, name: str) -> str:
    """The file-scope definition of an integer variable, whatever its type says."""
    return one_line(text, r"^(?:static\s+)?(?:volatile\s+)?(?:uint32_t|uint64_t|std::atomic<\w+>)\s+"
                    + name + r"\b[^;\n]*;", f"the definition of {name}")


def lines_naming(component: Path, sources: dict[Path, str], name: str):
    """(file:line, code) of every firmware line that names the variable."""
    for path, text in sources.items():
        if name not in text:
            continue
        for number, line in code_lines(text):
            if re.search(rf"\b{name}\b", line):
                yield f"{path.relative_to(component)}:{number}", line


def one_line(text: str, pattern: str, what: str) -> str:
    found = re.findall(pattern, text, re.M)
    require(len(found) == 1, f"{what}: expected one, found {len(found)}")
    return found[0]


def between(text: str, start: str, end: str, what: str) -> str:
    require(text.count(start) == 1, f"{what}: one start marker")
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


# ---- Output lanes -----------------------------------------------------------------

def output_lane_parts(component: Path, sources: dict[Path, str]) -> dict[str, str]:
    debug_header = sources[component / "System_Debug.h"]
    route_bits = "\n".join(re.findall(r"^#define MSG_ROUTE_\w+\s+0x[0-9A-Fa-f]+", debug_header, re.M))
    require(route_bits.count("#define") == 8, "the eight MSG_ROUTE_* values of System_Debug.h")
    flags_definition = definition(sources[component / "HardwareOne.cpp"], "gOutputFlags")
    apply_settings = extract_block(sources[component / "System_Settings.cpp"], "void applySettings(")
    lane = between(apply_settings, "// Apply the one persisted output lane (SERIAL)",
                   "// Apply debug settings to runtime flags", "the output lane step of applySettings")
    steps: dict[str, str] = {}
    for path, text in sources.items():
        for _, line in code_lines(text):
            for statement, operator, lane_name in re.findall(
                    r"(gOutputFlags\s*(\|=|&=)\s*~?MSG_ROUTE_(\w+)\s*;)", line):
                opens = operator == "|="
                require(opens != ("~" in statement), f"{path.name}: unexpected lane change `{statement}`")
                steps.setdefault(("laneOpen_" if opens else "laneClose_") + lane_name, statement)
    for name in LANES:
        for prefix in ("laneOpen_", "laneClose_"):
            require(prefix + name in steps, f"a production statement for {prefix}{name}")
    functions = "\n".join(f"static void {name}() {{ {statement} }}" for name, statement in sorted(steps.items()))
    return {"// INSERT_ROUTE_BITS": route_bits, "// INSERT_OUTPUT_FLAGS_DEFINITION": flags_definition,
            "// INSERT_APPLY_SETTINGS_LANE": lane, "// INSERT_LANE_STEPS": functions}


def check_output_lane_sources(component: Path, sources: dict[Path, str]) -> None:
    declarations = []
    for where, line in lines_naming(component, sources, "gOutputFlags"):
        if re.search(r"\b(?:uint32_t|std::atomic<[^>]*>)\s+gOutputFlags\b", line):
            declarations.append((where, line.strip()))
            continue
        require(not re.search(r"\bgOutputFlags\s*=(?!=)", line),
                f"{where}: gOutputFlags is stored from an earlier read; use |=, &= or a compare-exchange")
        require(not re.search(r"gOutputFlags\s*(\^=|\+=|-=|\+\+|--)", line),
                f"{where}: unexpected change of gOutputFlags")
    require(sorted(text for _, text in declarations) ==
            ["extern std::atomic<uint32_t> gOutputFlags;", "std::atomic<uint32_t> gOutputFlags{MSG_ROUTE_SERIAL};"],
            f"gOutputFlags has one atomic definition and one atomic declaration, found {declarations}")
    apply_settings = extract_block(sources[component / "System_Settings.cpp"], "void applySettings(")
    require(apply_settings.count("gOutputFlags.compare_exchange_weak(") == 1,
            "applySettings rewrites the SERIAL lane with one compare-exchange loop")


# ---- Glasses tap masks ------------------------------------------------------------

TAP_MASKS = (("gCamStreamPendingTap", "G2_ImageViewers.cpp", "CAM"),
             ("gHealthPagePendingTap", "G2_Page_Health.cpp", "HEALTH"))


def tap_drain(text: str, name: str) -> str:
    """The worker's statements from the read of the mask up to its first use of `taps`."""
    lines = text.splitlines()
    starts = [i for i, line in enumerate(lines) if re.match(rf"\s*const uint32_t taps = {name}\b", line)]
    require(len(starts) == 1, f"one drain of {name}, found {len(starts)}")
    end = next(i for i in range(starts[0] + 1, len(lines)) if re.match(r"\s*if \(taps &", lines[i]))
    return "\n".join(lines[starts[0]:end])


def tap_mask_parts(component: Path, sources: dict[Path, str]) -> dict[str, str]:
    glasses = sources[component / "G2_Glasses.cpp"]
    parts = {"// INSERT_TAP_MASK_DEFINITIONS":
             "\n".join(definition(glasses, name) for name, _, _ in TAP_MASKS)}
    for name, worker_file, tag in TAP_MASKS:
        parts[f"// INSERT_{tag}_TAP_RECORD"] = one_line(
            glasses, rf"^\s*({name}\s*\|=[^;\n]*;)", f"the statement that records a tap in {name}")
        parts[f"// INSERT_{tag}_TAPS_DRAIN"] = tap_drain(sources[component / worker_file], name)
    return parts


def check_tap_mask_sources(component: Path, sources: dict[Path, str]) -> None:
    internal = sources[component / "G2_GlassesInternal.h"]
    glasses = sources[component / "G2_Glasses.cpp"]
    for name, worker_file, _ in TAP_MASKS:
        require(f"extern std::atomic<uint32_t> {name};" in internal,
                f"G2_GlassesInternal.h declares {name} as std::atomic<uint32_t>")
        require(definition(glasses, name) == f"std::atomic<uint32_t> {name}{{0}};",
                f"G2_Glasses.cpp defines {name} as std::atomic<uint32_t>")
        drain = "\n".join(line for _, line in code_lines(tap_drain(sources[component / worker_file], name)))
        require(drain.strip() == f"const uint32_t taps = {name}.exchange(0);",
                f"{worker_file}: the worker takes and clears {name} in one exchange(0), found `{drain.strip()}`")
        for where, line in lines_naming(component, sources, name):
            # A reset to 0 (page start, page end, teardown) is one store; anything
            # else that assigns the mask would be built from an earlier read.
            require(not re.search(rf"\b{name}\s*=(?!=)(?!\s*0\s*;)", line),
                    f"{where}: {name} is assigned something other than 0")


# ---- G2 hijack bridge counters ----------------------------------------------------

def hijack_counter_parts(component: Path, sources: dict[Path, str]) -> dict[str, str]:
    bridge = sources[component / "G2_HijackCmd.cpp"]
    return {
        "// INSERT_HIJACK_COUNTER_DEFINITIONS":
            definition(bridge, "gMenuGen") + "\n" + definition(bridge, "gCmdSeq"),
        "// INSERT_MENU_GEN_FUNCTIONS":
            extract_block(bridge, "void g2BumpMenuGen()") + "\n" + extract_block(bridge, "uint32_t g2CurrentMenuGen()"),
        "// INSERT_COMMAND_SEQ_STAMP":
            one_line(bridge, r"^\s*(ctx->cookie\.seq\s*=[^;\n]*;)", "the statement that stamps a command's sequence number"),
    }


def check_hijack_counter_sources(component: Path, sources: dict[Path, str]) -> None:
    bridge = sources[component / "G2_HijackCmd.cpp"]
    for name in ("gMenuGen", "gCmdSeq"):
        require(definition(bridge, name) == f"static std::atomic<uint32_t> {name}{{0}};",
                f"G2_HijackCmd.cpp defines {name} as a file-private std::atomic<uint32_t>")
        for where, line in lines_naming(component, sources, name):
            require(not re.search(rf"\b{name}\s*(?:=(?!=)|\+=|-=)", line),
                    f"{where}: {name} is stored from an earlier read")
    require(re.search(r"ctx->cookie\.seq\s*=\s*\+\+gCmdSeq;", bridge) is not None,
            "g2SubmitHijackCommand stamps the sequence number with one ++gCmdSeq")


# ---- Generation counters ----------------------------------------------------------

def generation_parts(component: Path, sources: dict[Path, str]) -> dict[str, str]:
    notifications = sources[component / "System_Notifications.cpp"]
    bumps = set(re.findall(r"^\s*(gNotifPrefsGen\s*\+\+\s*;|\+\+\s*gNotifPrefsGen\s*;)", notifications, re.M))
    require(len(bumps) == 1, f"the notification prefs generation is bumped with one statement form, found {bumps}")
    return {
        "// INSERT_GENERATION_DEFINITIONS":
            definition(notifications, "gNotifPrefsGen") + "\n"
            + definition(sources[component / "G2_Glasses.cpp"], "gHealthPageGen"),
        "// INSERT_NOTIF_PREFS_GENERATION": extract_block(notifications, "uint32_t notifPrefsGeneration()"),
        "// INSERT_NOTIF_PREFS_BUMP": bumps.pop(),
        "// INSERT_HEALTH_PAGE_GEN_STAMP": one_line(
            sources[component / "G2_Page_Health.cpp"], r"^\s*(a->gen\s*=[^;\n]*;)",
            "the statement that stamps a Health page open's generation"),
    }


def check_generation_sources(component: Path, sources: dict[Path, str]) -> None:
    notifications = sources[component / "System_Notifications.cpp"]
    require(definition(notifications, "gNotifPrefsGen") == "static std::atomic<uint32_t> gNotifPrefsGen{1};",
            "System_Notifications.cpp defines gNotifPrefsGen as a file-private std::atomic<uint32_t>")
    require('#include <atomic>' in notifications, "System_Notifications.cpp includes <atomic> itself")
    require(len(re.findall(r"^\s*gNotifPrefsGen\+\+;", notifications, re.M)) == 3,
            "the three notification prefs bumps (policy load, user prefs invalidate, notifydevicekind)")
    require(definition(sources[component / "G2_Glasses.cpp"], "gHealthPageGen") ==
            "std::atomic<uint32_t> gHealthPageGen{0};", "G2_Glasses.cpp defines gHealthPageGen as std::atomic<uint32_t>")
    require("extern std::atomic<uint32_t> gHealthPageGen;" in sources[component / "G2_GlassesInternal.h"],
            "G2_GlassesInternal.h declares gHealthPageGen as std::atomic<uint32_t>")
    require(re.search(r"a->gen\s*=\s*\+\+gHealthPageGen;", sources[component / "G2_Page_Health.cpp"]) is not None,
            "g2ShowHealthPage stamps the generation with one ++gHealthPageGen")
    for name in ("gNotifPrefsGen", "gHealthPageGen"):
        for where, line in lines_naming(component, sources, name):
            require(not re.search(rf"\b{name}\s*(?:=(?!=)|\+=|-=)", line),
                    f"{where}: {name} is stored from an earlier read")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    sources = {path: path.read_text(encoding="utf-8", errors="replace") for path in firmware_sources(component)}

    harness = (HERE / "shared_flag_atomics_harness.cpp").read_text()
    parts = {}
    parts.update(output_lane_parts(component, sources))
    parts.update(tap_mask_parts(component, sources))
    parts.update(hijack_counter_parts(component, sources))
    parts.update(generation_parts(component, sources))
    for marker, body in parts.items():
        require(harness.count(marker) == 1, f"one {marker} in the harness")
        harness = harness.replace(marker, body)
    require("// INSERT_" not in harness, "every insertion marker of the harness is filled")

    flags = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-shared-flag-atomics-") as directory:
        source = Path(directory) / "shared_flag_atomics.cpp"
        binary = Path(directory) / "shared_flag_atomics"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        if subprocess.run([str(binary)], timeout=300).returncode != 0:
            failures.append("the harness reported lost updates or a non-atomic type")
    for check in (check_output_lane_sources, check_tap_mask_sources, check_hijack_counter_sources,
                  check_generation_sources):
        try:
            check(component, sources)
        except AssertionError as error:
            failures.append(str(error))
    require(not failures, "\n".join(failures))
    print("shared flags and counters change in one atomic step each", flush=True)


if __name__ == "__main__":
    main()
