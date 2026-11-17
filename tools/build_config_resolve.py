"""Evaluate System_BuildConfig.h on this computer for one board and one set of choices.

The guided setup uses this to show what a selection really turns on and to
report the header's own dependency messages before a build. There is no second
copy of the rules: the board feature header, the saved choices and the derived
section are read by a C++ preprocessor exactly as the firmware build reads them.

The SDK settings come from the same layered defaults files the build passes to
Kconfig, not from a resolved sdkconfig, so a setting Kconfig would add by
itself is absent here. The build stays the final check.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HEADER = "components/hardwareone/System_BuildConfig.h"
SETTING = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
NOT_SET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")


def sdk_layers(repo, board, target):
    """The defaults files of one board, in the order the root CMakeLists.txt layers them."""
    files = [repo / "config/sdkconfig.defaults", repo / f"config/sdkconfig.defaults.{target}",
             repo / f"boards/{board}.defaults", repo / "presets/sdkconfig.defaults",
             repo / f"presets/{board}.sdkconfig.defaults"]
    return [path for path in files if path.is_file()]


def sdk_defaults(repo, board, target, choices=None):
    """Last assignment wins, as in Kconfig defaults; `choices` are a saved configuration's SDK choices."""
    settings = {}
    for path in sdk_layers(repo, board, target):
        for line in path.read_text().splitlines():
            line = line.strip()
            match = SETTING.match(line)
            if match:
                settings[match.group(1)] = match.group(2)
            else:
                match = NOT_SET.match(line)
                if match:
                    settings.pop(match.group(1), None)
    for key, value in (choices or {}).items():
        settings[key] = "y" if value else "n"
    settings = {key: value for key, value in settings.items() if value != "n"}
    # What Kconfig settles by itself, for the settings System_BuildConfig.h reads (compared with the
    # sdkconfig.h of real builds on 2026-11-16): the Bluetooth options exist only with the stack, whose
    # default host is Bluedroid, and the camera driver's SCCB bus defaults to I2C controller 1.
    if "CONFIG_BT_ENABLED" not in settings:
        settings = {key: value for key, value in settings.items() if not key.startswith("CONFIG_BT_")}
    elif not {"CONFIG_BT_NIMBLE_ENABLED", "CONFIG_BT_CONTROLLER_ONLY"} & set(settings):
        settings.setdefault("CONFIG_BT_BLUEDROID_ENABLED", "y")
    if "CONFIG_SCCB_HARDWARE_I2C_PORT0" not in settings:
        settings.setdefault("CONFIG_SCCB_HARDWARE_I2C_PORT1", "y")
    return settings


def sdkconfig_header(repo, board, target, choices=None):
    settings = sdk_defaults(repo, board, target, choices)
    lines = [f'#define CONFIG_IDF_TARGET "{target}"', f"#define CONFIG_IDF_TARGET_{target.upper()} 1"]
    for key, value in sorted(settings.items()):
        if key.startswith("CONFIG_IDF_TARGET"):
            continue
        if value == "y":
            value = "1"
        elif not (value.startswith('"') or re.fullmatch(r"-?(0x[0-9A-Fa-f]+|\d+)", value)):
            continue
        lines.append(f"#define {key} {value}")
    return "\n".join(lines) + "\n", settings


def derived_names(repo):
    """CUSTOM_ENABLE_X -> the ENABLE_Y the rest of the firmware reads, from the header's own lines."""
    text = (repo / HEADER).read_text()
    names = {custom: derived for derived, custom in
             re.findall(r"^\s*#define\s+(ENABLE_\w+)\s+(CUSTOM_ENABLE_\w+)\s*$", text, re.M)}
    # A toggle that is one term of a condition: "#if !CUSTOM_ENABLE_NET_HTTP || ..." / "#define ENABLE_HTTP_SERVER 0".
    for custom, derived in re.findall(r"^\s*#if\s+!(CUSTOM_ENABLE_\w+)\b.*\n\s*#define\s+(ENABLE_\w+)\s+0\s*$", text, re.M):
        names.setdefault(custom, derived)
    return names


def level_groups(repo):
    """CUSTOM_ENABLE_X -> the *_FEATURE_LEVEL whose value 4 makes the firmware read it."""
    groups = {}
    text = (repo / HEADER).read_text().split("// CHECKED-IN DEPLOYMENT PROFILE OVERRIDE", 1)[0]
    for level, body in re.findall(r"^#if (\w+_FEATURE_LEVEL) == 4\n(.*?)^#endif", text, re.M | re.S):
        for name in re.findall(r"^\s*#define\s+(CUSTOM_ENABLE_\w+)\s+\d+\b", body, re.M):
            groups[name] = level
    return groups


def resolve(repo, board, target, features, sdk_choices=None, names=()):
    """Return ({name: int}, ()). When the header rejects the selection: (values or None, its messages)."""
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        return None, ("no C++ compiler on this computer; choices are checked by the build only",)
    sdk_text, settings = sdkconfig_header(repo, board, target, sdk_choices)
    header = repo / HEADER
    variant = settings.get("CONFIG_ARDUINO_VARIANT", '""').strip('"')
    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        (scratch / "sdkconfig.h").write_text(sdk_text)
        lines = ["#pragma once"]
        for key, value in sorted(features.items()):
            lines.extend([f"#undef {key}", f"#define {key} {value}"])
        (scratch / "features.h").write_text("\n".join(lines) + "\n")
        probe = [f'#include "{header}"']
        for name in sorted(set(names)):
            probe.append(f'#if defined({name})\n#if {name}\nHW1BOOL "{name}" 1\n#else\n'
                         f'HW1BOOL "{name}" 0\n#endif\nHW1VAL "{name}" {name}\n#endif')
        (scratch / "probe.cpp").write_text("\n".join(probe) + "\n")
        argv = [compiler, "-E", "-P", "-x", "c++", "-I", str(scratch), "-I", str(header.parent),
                f'-DHW1_FEATURE_CONFIG_HEADER="{scratch / "features.h"}"']
        if variant:
            argv.append("-DARDUINO_" + re.sub(r"\W", "_", variant).upper() + "_DEV=1")
        board_header = repo / f"boards/{board}.features.h"
        if board_header.is_file():
            argv.append(f'-DHW1_BOARD_CONFIG_HEADER="{board_header}"')
        try:
            done = subprocess.run(argv + [str(scratch / "probe.cpp")], capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as error:
            return None, (f"could not run {compiler}: {error}",)
    values = {}
    for line in done.stdout.splitlines():
        match = re.match(r'\s*HW1(BOOL|VAL)\s+"(\w+)"\s+(.*)$', line)
        if not match:
            continue
        kind, name, text = match.groups()
        text = text.strip()
        if kind == "BOOL":
            values.setdefault(name, int(text))
        elif re.fullmatch(r"-?\d+", text):
            values[name] = int(text)
    if done.returncode:
        # A preprocessor that carries on after #error still reports the values, so a rejected
        # selection can be shown and repaired; one that stops there reports none.
        messages = list(dict.fromkeys(re.findall(r'error: (?:#error )?"?(.*?)"?\s*$', done.stderr, re.M)))
        return values or None, tuple(messages or [done.stderr.strip().splitlines()[-1] if done.stderr.strip()
                                                  else "the configuration header did not evaluate"])
    return values, ()
