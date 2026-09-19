#!/usr/bin/env python3
"""Generate BUILD_INFO.md for a build directory.

    tools/gen_build_info.py build-feathers3 [board-name] [deployment-selector]

Documents what a given firmware image actually contains: which feature flags
were on, which board/chip it targets, how big it is, and what tree it came
from. Written into the build dir so an image shipped or archived on its own
still explains itself.

Accuracy note: feature-flag values are resolved BY THE REAL COMPILER, using
the exact include paths and -D flags recorded in the build's
compile_commands.json. That means derived and expression-valued flags
(ENABLE_NEOPIXEL, ENABLE_MICROPHONE, ENABLE_HTTP_SERVER, per-sensor flags
that come from a level) report the value the build truly used -- not a
best-effort regex of the header.
"""

import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Macro families worth reporting, in display order. (prefix/exact, section)
SECTIONS = [
    ("Feature flags", lambda n: n.startswith("ENABLE_")),
    ("Levels & selection", lambda n: n.endswith("_FEATURE_LEVEL") or n in (
        "DISPLAY_TYPE", "INPUT_DEVICE_TYPE", "HW1_OTA_LAYOUT")),
    ("Board hardware", lambda n: n.startswith((
        "BOARD_", "NEOPIXEL_", "BATTERY_", "I2C_", "I2C2_", "MIC_", "SD_",
        "CAMERA_", "UART_LINK_", "USER_LED_")) or n in ("XIAO_ESP32S3_SENSE_ENABLED",)),
]

# sdkconfig keys that describe the chip/build environment.
SDKCONFIG_KEYS = [
    ("CONFIG_IDF_TARGET", "Chip target"),
    ("CONFIG_ARDUINO_VARIANT", "Arduino variant"),
    ("CONFIG_ESPTOOLPY_FLASHSIZE", "Flash size"),
    ("CONFIG_ESPTOOLPY_FLASHMODE_QIO", "Flash mode QIO"),
    ("CONFIG_SPIRAM", "PSRAM enabled"),
    ("CONFIG_SPIRAM_MODE_QUAD", "PSRAM quad mode"),
    ("CONFIG_SPIRAM_MODE_OCT", "PSRAM octal mode"),
    ("CONFIG_SPIRAM_TYPE_AUTO", "PSRAM type auto-detect"),
    ("CONFIG_BT_ENABLED", "Bluedroid BT stack"),
    ("CONFIG_ESP32S3_DATA_CACHE_LINE_64B", "64B data cache line"),
    ("CONFIG_COMPILER_OPTIMIZATION_PERF", "Optimize for performance"),
    ("CONFIG_SECURE_FLASH_ENC_ENABLED", "Flash encryption"),
    ("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME", "Partition table"),
]


def die(msg):
    print("gen_build_info: %s" % msg, file=sys.stderr)
    sys.exit(1)


def compiler_cmd_for_buildconfig(build_dir):
    """Return (argv_prefix, cwd) able to preprocess a stub in hardwareone's context."""
    ccj = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(ccj):
        return None, None
    try:
        entries = json.load(open(ccj))
    except Exception:
        return None, None
    hw = [e for e in entries
          if os.sep + "hardwareone" + os.sep in e.get("file", "")
          and e.get("file", "").endswith((".cpp", ".c"))]
    if not hw:
        return None, None
    entry = hw[0]
    argv = shlex.split(entry.get("command", ""))
    out = []
    skip_next = False
    for i, tok in enumerate(argv):
        if skip_next:
            skip_next = False
            continue
        if tok in ("-o", "-MT", "-MF"):
            skip_next = True
            continue
        if tok in ("-c", "-MD"):
            continue
        if tok == entry["file"] or tok.endswith(entry["file"]):
            continue
        out.append(tok)
    return out, entry.get("directory", build_dir)


def deployment_header_from_argv(argv):
    """Return the deployment feature overlay named by the compile command."""
    prefix = "-DHW1_DEPLOYMENT_CONFIG_HEADER="
    for token in argv:
        if token.startswith(prefix):
            value = token[len(prefix):].strip()
            # CMake emits a C string literal. shlex removes shell quoting but
            # deliberately leaves the quotes that form part of the macro.
            value = value.strip('"')
            return os.path.abspath(value) if value else None
    return None


def resolve_macros(build_dir, artifact_path=None):
    """Ask the real compiler for the resolved value of every interesting macro.

    Refuses to report anything if System_BuildConfig.h has been touched since
    the image was linked: flags are resolved from the header's CURRENT text, so
    a later run would confidently print values this image was never built with.
    Wrong values are worse than absent ones.
    """
    argv, cwd = compiler_cmd_for_buildconfig(build_dir)
    if not argv:
        return None, "compile_commands.json unavailable (build not configured?)"
    hdr = os.path.join(REPO, "components", "hardwareone", "System_BuildConfig.h")
    deployment_hdr = deployment_header_from_argv(argv)
    source_headers = [hdr]
    if deployment_hdr:
        if not os.path.isfile(deployment_hdr):
            return None, ("deployment feature overlay recorded by this build is missing: "
                          "%s" % deployment_hdr)
        source_headers.append(deployment_hdr)
    if artifact_path and os.path.exists(artifact_path):
        changed = [path for path in source_headers
                   if os.path.getmtime(path) > os.path.getmtime(artifact_path)]
        if changed:
            names = ", ".join(os.path.relpath(path, REPO) for path in changed)
            return None, ("STALE — %s changed after this image was linked, so its "
                          "feature flags can no longer be recovered from current source. "
                          "Rebuild this board to regenerate an accurate manifest." % names)

    with tempfile.TemporaryDirectory() as td:
        # Pass 1: what is defined at all?
        stub1 = os.path.join(td, "dump1.cpp")
        with open(stub1, "w") as f:
            f.write('#include "%s"\n' % hdr)
        try:
            p1 = subprocess.run(argv + ["-dM", "-E", "-x", "c++", stub1],
                                cwd=cwd, capture_output=True, text=True, timeout=180)
        except Exception as e:
            return None, "preprocessor invocation failed: %s" % e
        if p1.returncode != 0:
            # Most likely cause: a build-configuration header has been edited
            # since this image was linked. Surface the compiler's own words.
            err = ""
            for line in p1.stderr.splitlines():
                clean = re.sub(r"\x1b\[[0-9;]*[A-Za-z]|\[\d+m\[K|\[K", "", line)
                m = re.search(r"(#error.*|error:.*)", clean)
                if m:
                    err = m.group(1).strip()
                    break
            return None, ("System_BuildConfig.h no longer preprocesses for this board "
                          "— it has been edited since this build, so flags cannot be "
                          "resolved retroactively. Rebuild to refresh. Compiler said: %s"
                          % (err or "(no detail)"))

        names = set()
        for line in p1.stdout.splitlines():
            m = re.match(r"#define\s+([A-Za-z_][A-Za-z0-9_]*)", line)
            if m:
                n = m.group(1)
                if any(pred(n) for _, pred in SECTIONS):
                    names.add(n)
        if not names:
            return None, "no HardwareOne macros found in preprocessor output"

        # Pass 2: resolve each to a concrete value. Booleans go through #if so
        # expression-valued flags (ENABLE_NEOPIXEL, ENABLE_MICROPHONE, ...)
        # report what the build really used; others expand in place.
        stub2 = os.path.join(td, "dump2.cpp")
        ordered = sorted(names)
        # NOTE: the macro NAME must be a string literal in the emitted marker.
        # Written bare it is itself macro-expanded, and the output line no
        # longer says which macro it describes.
        with open(stub2, "w") as f:
            f.write('#include "%s"\n' % hdr)
            for n in ordered:
                f.write("#if defined(%s)\n" % n)
                f.write('#if %s\nHW1BOOL "%s" 1\n#else\nHW1BOOL "%s" 0\n#endif\n' % (n, n, n))
                f.write('HW1VAL "%s" %s\n' % (n, n))
                f.write("#endif\n")
        try:
            p2 = subprocess.run(argv + ["-E", "-P", "-x", "c++", stub2],
                                cwd=cwd, capture_output=True, text=True, timeout=180)
        except Exception as e:
            return None, "value pass failed: %s" % e

        boolean, value = {}, {}
        for line in p2.stdout.splitlines():
            line = line.strip()
            mb = re.match(r'HW1BOOL\s+"(\w+)"\s+([01])$', line)
            if mb:
                boolean[mb.group(1)] = mb.group(2)
                continue
            mv = re.match(r'HW1VAL\s+"(\w+)"\s+(.*)$', line)
            if mv:
                value[mv.group(1)] = mv.group(2).strip()
        return {
            "bool": boolean,
            "value": value,
            "names": ordered,
            "deployment_header": deployment_hdr,
        }, None


def read_sdkconfig(build_dir):
    cfg = {}
    path = os.path.join(build_dir, "sdkconfig")
    if not os.path.exists(path):
        path = os.path.join(REPO, "sdkconfig")
    if not os.path.exists(path):
        return cfg, None
    for line in open(path, errors="replace"):
        line = line.strip()
        m = re.match(r'^(CONFIG_[A-Z0-9_]+)=(.*)$', line)
        if m:
            cfg[m.group(1)] = m.group(2).strip('"')
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            cfg[line.split()[1]] = None
    return cfg, path


def git_provenance():
    def run(*a):
        try:
            return subprocess.run(a, cwd=REPO, capture_output=True, text=True,
                                  timeout=30).stdout.strip()
        except Exception:
            return ""
    commit = run("git", "rev-parse", "--short", "HEAD")
    desc = run("git", "log", "-1", "--pretty=%s")
    dirty = run("git", "status", "--porcelain")
    n_dirty = len([l for l in dirty.splitlines() if l.strip()])
    return commit, desc, n_dirty


def artifact_info(build_dir):
    out = {}
    proj = None
    pj = os.path.join(build_dir, "project_description.json")
    if os.path.exists(pj):
        try:
            proj = json.load(open(pj))
        except Exception:
            proj = None
    name = (proj or {}).get("project_name", "hardwareone-idf")
    out["project"] = name
    out["version"] = (proj or {}).get("project_version", "")
    binp = os.path.join(build_dir, name + ".bin")
    if os.path.exists(binp):
        out["bin_path"] = os.path.relpath(binp, REPO)
        out["bin_bytes"] = os.path.getsize(binp)
    # App partition size — read THIS build's own partition table image.
    ptbin = os.path.join(build_dir, "partition_table", "partition-table.bin")
    csv_text = None
    if os.path.exists(ptbin):
        idf_path = (proj or {}).get("idf_path") or os.environ.get("IDF_PATH", "")
        gen = os.path.join(idf_path, "components",
                           "partition_table", "gen_esp32part.py")
        if os.path.exists(gen):
            try:
                p = subprocess.run([sys.executable, gen, ptbin],
                                   capture_output=True, text=True, timeout=60)
                if p.returncode == 0:
                    csv_text = p.stdout
                    out["part_source"] = "this build's partition-table.bin"
            except Exception:
                pass
    if csv_text is None:
        # The selected path is also recorded in this build's sdkconfig. This
        # remains authoritative when the IDF partition decoder is unavailable.
        sdkconfig = os.path.join(build_dir, "sdkconfig")
        if os.path.isfile(sdkconfig):
            for line in open(sdkconfig, errors="replace"):
                match = re.match(
                    r'^CONFIG_PARTITION_TABLE_FILENAME="([^"]+)"$',
                    line.strip(),
                )
                if not match:
                    continue
                configured = match.group(1)
                partition_path = (
                    configured
                    if os.path.isabs(configured)
                    else os.path.join(REPO, configured)
                )
                if os.path.isfile(partition_path):
                    csv_text = open(partition_path, errors="replace").read()
                    out["part_source"] = os.path.relpath(partition_path, REPO)
                break
    app_partitions = []
    for line in (csv_text or "").splitlines():
        if line.strip().startswith("#") or "," not in line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) >= 5 and parts[1] == "app":
            try:
                raw_size = parts[4]
                multiplier = 1
                if raw_size.upper().endswith("K"):
                    multiplier = 1024
                    raw_size = raw_size[:-1]
                elif raw_size.upper().endswith("M"):
                    multiplier = 1024 * 1024
                    raw_size = raw_size[:-1]
                app_partitions.append(
                    (parts[0], int(raw_size, 0) * multiplier)
                )
            except Exception:
                pass
    if app_partitions:
        # HardwareOne recovery layouts boot the golden updater from factory and
        # place the main firmware in ota_0. Ordinary layouts have only factory.
        selected = next(
            (part for part in app_partitions if part[0] == "ota_0"),
            app_partitions[0],
        )
        out["app_part_name"], out["app_part_bytes"] = selected
    return out


def fmt_kb(n):
    return "%s bytes (%.1f KB)" % ("{:,}".format(n), n / 1024.0)


def main():
    if len(sys.argv) < 2:
        die("usage: gen_build_info.py <build-dir> [board] [deployment-selector]")
    build_dir = os.path.abspath(sys.argv[1])
    if not os.path.isdir(build_dir):
        die("no such build dir: %s" % build_dir)
    board = sys.argv[2] if len(sys.argv) > 2 else (
        os.path.basename(build_dir).replace("build-", "") or "(default)")
    deployment = sys.argv[3] if len(sys.argv) > 3 else ""

    art = artifact_info(build_dir)
    art_abs = os.path.join(REPO, art["bin_path"]) if art.get("bin_path") else None
    macros, macro_err = resolve_macros(build_dir, art_abs)
    cfg, cfg_path = read_sdkconfig(build_dir)
    commit, desc, n_dirty = git_provenance()
    now = datetime.now(timezone.utc).astimezone()

    L = []
    A = L.append
    identity = deployment or board
    A("# Build manifest — `%s`" % identity)
    A("")
    A("_Generated by `tools/gen_build_info.py` at build time. Regenerated on every")
    A("build; do not hand-edit._")
    A("")
    board_name = (macros or {}).get("value", {}).get("BOARD_NAME", "").strip('"') if macros else ""
    if board_name:
        A("**Board:** %s (`HW_BOARD=%s`)  " % (board_name, board))
    else:
        A("**Board:** `HW_BOARD=%s`  " % board)
    if deployment:
        A("**Deployment:** `%s`  " % deployment)
    A("**Chip:** %s  " % cfg.get("CONFIG_IDF_TARGET", "?"))
    A("**Built:** %s  " % now.strftime("%Y-%m-%d %H:%M:%S %Z"))
    if commit:
        dirty_note = " + %d uncommitted file(s)" % n_dirty if n_dirty else " (clean tree)"
        A("**Source:** `%s`%s — %s" % (commit, dirty_note, desc))
    A("")

    if art.get("bin_bytes"):
        A("## Artifact")
        A("")
        A("| | |")
        A("|---|---|")
        A("| Image | `%s` |" % art.get("bin_path", ""))
        A("| Size | %s |" % fmt_kb(art["bin_bytes"]))
        if art.get("app_part_bytes"):
            free = art["app_part_bytes"] - art["bin_bytes"]
            pct = 100.0 * free / art["app_part_bytes"]
            A("| App partition (`%s`) | %s |" % (art.get("app_part_name", "app"),
                                                 fmt_kb(art["app_part_bytes"])))
            A("| Free headroom | %s (%.1f%%) |" % (fmt_kb(free), pct))
        if art.get("version"):
            A("| Project version | `%s` |" % art["version"])
        A("")

    A("## Chip / SDK configuration")
    A("")
    A("| Setting | Value |")
    A("|---|---|")
    for key, label in SDKCONFIG_KEYS:
        if key in cfg:
            v = cfg[key]
            v = "off" if v is None else ("on" if v == "y" else v)
            A("| %s | `%s` |" % (label, v))
    A("")
    if cfg_path:
        A("_Source: `%s`_" % os.path.relpath(cfg_path, REPO))
        A("")

    if macros:
        for title, pred in SECTIONS:
            names = [n for n in macros["names"] if pred(n)]
            if not names:
                continue
            A("## %s" % title)
            A("")
            if title == "Feature flags":
                on = [n for n in names if macros["bool"].get(n) == "1"]
                off = [n for n in names if macros["bool"].get(n) == "0"]
                A("**Enabled (%d):**" % len(on))
                A("")
                for n in on:
                    A("- `%s`" % n)
                A("")
                A("**Disabled (%d):**" % len(off))
                A("")
                A(", ".join("`%s`" % n for n in off) if off else "_(none)_")
                A("")
            else:
                A("| Macro | Value |")
                A("|---|---|")
                for n in names:
                    val = macros["value"].get(n, "")
                    if val == n or val == "":
                        val = macros["bool"].get(n, "?")
                    A("| `%s` | `%s` |" % (n, val))
                A("")
    else:
        A("## Feature flags")
        A("")
        A("> **Not recorded.** %s" % macro_err)
        A("")
        A("Flags are deliberately omitted rather than guessed — this manifest reports")
        A("only what can be proven about the image above.")
        A("")

    A("## Reproduce this build")
    A("")
    A("```bash")
    if deployment:
        family, deployment_board = deployment.split("/", 1)
        A("HW1_OTA_SIGNING_KEY=/absolute/path/to/key.pem \\")
        A("  tools/build_deployment.sh %s %s" % (family, deployment_board))
    else:
        A("tools/build_board.sh %s" % board)
    A("```")
    A("")
    if deployment and macros and macros.get("deployment_header"):
        overlay = os.path.relpath(macros["deployment_header"], REPO)
        A("Feature defaults come from `components/hardwareone/System_BuildConfig.h` and")
        A("are overridden by the checked-in deployment policy `%s`;" % overlay)
    else:
        A("Feature flags come from `components/hardwareone/System_BuildConfig.h`")
        A("(edit the user-config section at the top);")
    A("board/chip settings come from")
    A("`boards/%s.defaults`. Values above were resolved by the compiler for this" % board)
    A("exact build, so they include derived flags, not just the literals in the header.")
    A("")

    out_path = os.path.join(build_dir, "BUILD_INFO.md")
    with open(out_path, "w") as f:
        f.write("\n".join(L) + "\n")
    print("gen_build_info: wrote %s" % os.path.relpath(out_path, REPO))
    if macro_err:
        print("gen_build_info: warning: %s" % macro_err, file=sys.stderr)


if __name__ == "__main__":
    main()
