#!/usr/bin/env python3
"""Save feature choices and build them through the ordinary board build path.

These small JSON files override board defaults. Dependency resolution stays in
System_BuildConfig.h; the real build compiler verifies the requested choices.
"""

import argparse
import errno
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

import gen_build_info

REPO = Path(__file__).resolve().parents[1]

# os.link errors that mean "this filesystem has no hard links" (exFAT, some
# SMB/NFS mounts) rather than "the destination exists".
NO_HARD_LINK_ERRNOS = {errno.EPERM, errno.ENOTSUP, errno.EOPNOTSUPP, errno.EXDEV, errno.EMLINK}
NAME = re.compile(r"[a-z0-9][a-z0-9_-]*\Z")


def checked_name(name):
    if not NAME.fullmatch(name):
        raise ValueError("names must contain only lowercase letters, digits, '-' or '_'")
    return name


def config_path(name):
    return REPO / "configurations" / (checked_name(name) + ".json")


def build_dir(name):
    return REPO / ("build-config-" + checked_name(name))


def editable_choices():
    # Discover input names from the existing editable section, not a second
    # feature registry. Derived flags, pin maps and SDK settings are excluded.
    header = (REPO / "components/hardwareone/System_BuildConfig.h").read_text()
    marker = "// CHECKED-IN DEPLOYMENT PROFILE OVERRIDE"
    if header.count(marker) != 1:
        raise ValueError("cannot locate the editable feature section in System_BuildConfig.h")
    editable = header.split(marker, 1)[0]
    names = re.findall(r"^\s*#define\s+(\w+)\s+\d+\b", editable, re.MULTILINE)
    return {name for name in names if name.startswith(("ENABLE_", "CUSTOM_ENABLE_"))
            or name.endswith("_FEATURE_LEVEL")
            or name in ("DISPLAY_TYPE", "INPUT_DEVICE_TYPE")}


def sdk_choices():
    # This file is also the build's default policy; hardware/security settings
    # stay in board definitions and cannot be overridden by a feature selection.
    policy = (REPO / "presets/sdkconfig.defaults").read_text()
    return set(re.findall(r"^(CONFIG_[A-Z0-9_]+)=[yn]$", policy, re.MULTILINE))


def choice_schema():
    """Describe exactly the choices accepted by validation and the CLI."""
    features = {}
    for name in sorted(editable_choices()):
        maximum = 4 if name.endswith("_FEATURE_LEVEL") or name == "DISPLAY_TYPE" else (
            3 if name == "INPUT_DEVICE_TYPE" else 1)
        features[name] = {"type": "integer", "minimum": 0, "maximum": maximum}
    return {"features": features,
            "sdkconfig": {name: {"type": "boolean"} for name in sorted(sdk_choices())}}


def board_choices():
    boards = {}
    for path in sorted((REPO / "boards").glob("*.defaults")):
        target = re.search(r"^#[ \t]*HW_TARGET:[ \t]*([A-Za-z0-9]+)[ \t]*$", path.read_text(), re.M)
        if target and NAME.fullmatch(path.stem):
            boards[path.stem] = target.group(1)
    return boards


def validate(config):
    if (not isinstance(config, dict) or not {"board", "features"} <= set(config)
            or set(config) - {"board", "features", "sdkconfig"}):
        raise ValueError("configuration needs 'board', 'features', and optionally 'sdkconfig'")
    board = config["board"]
    if not isinstance(board, str):
        raise ValueError("board must be a name")
    checked_name(board)
    if board not in board_choices():
        raise ValueError(f"unknown board: {board}")
    features = config["features"]
    if not isinstance(features, dict):
        raise ValueError("features must be an object of integer choices")
    schema = choice_schema()
    for key, value in features.items():
        if key not in schema["features"]:
            raise ValueError(f"{key}: unknown or derived feature; use an editable input")
        specification = schema["features"][key]
        minimum, maximum = specification["minimum"], specification["maximum"]
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(f"{key}: expected an integer from {minimum} to {maximum}")
    sdk = config.get("sdkconfig", {})
    if not isinstance(sdk, dict):
        raise ValueError("sdkconfig must be an object of boolean feature choices")
    for key, value in sdk.items():
        if key not in schema["sdkconfig"]:
            raise ValueError(f"{key}: not a selectable SDK feature; see presets/sdkconfig.defaults")
        if type(value) is not bool:
            raise ValueError(f"{key}: SDK feature choices must be true or false")
    return config


def load(name):
    return validate(json.loads(config_path(name).read_text()))


def encode(config):
    return json.dumps(config, indent=2, sort_keys=True) + "\n"


def apply_changes(config, feature_sets=(), feature_unsets=(), sdk_sets=(), sdk_unsets=()):
    """Apply explicit choices to a copy; omitted choices continue to inherit."""
    validate(config)
    candidate = {"board": config["board"], "features": dict(config["features"])}
    if "sdkconfig" in config:
        candidate["sdkconfig"] = dict(config["sdkconfig"])
    feature_updates, sdk_updates = {}, {}
    for setting in feature_sets:
        key, separator, value = setting.partition("=")
        if not separator:
            raise ValueError("--set expects FEATURE=INTEGER")
        try:
            feature_updates[key] = int(value)
        except ValueError:
            raise ValueError(f"{key}: --set expects FEATURE=INTEGER") from None
    for setting in sdk_sets:
        key, separator, value = setting.partition("=")
        if not separator or value not in ("y", "n"):
            raise ValueError("--sdk expects CONFIG_FEATURE=y or CONFIG_FEATURE=n")
        sdk_updates[key] = value == "y"
    schema = choice_schema()
    for namespace, updates, removals in (
            ("features", feature_updates, set(feature_unsets)),
            ("sdkconfig", sdk_updates, set(sdk_unsets))):
        conflicts = updates.keys() & removals
        if conflicts:
            raise ValueError("cannot set and unset the same choice: " + ", ".join(sorted(conflicts)))
        for key in sorted(removals):
            if key not in schema[namespace]:
                raise ValueError(f"{key}: unknown {namespace} choice to unset")
        if updates:
            candidate.setdefault(namespace, {}).update(updates)
        for key in removals:
            candidate.get(namespace, {}).pop(key, None)
    return validate(candidate)


def check_build_board(name, board):
    snapshot = build_dir(name) / "configuration.json"
    if snapshot.exists() and json.loads(snapshot.read_text())["board"] != board:
        raise ValueError("saved configuration changed board; use a new configuration name")


def save_configuration(name, config, *, original=None):
    """Save validated choices, returning whether a file was created or changed.

    Pass the exact previously read text as original when editing. Stale text
    is rejected, including for a semantic no-op. With original=None this is a
    new configuration: publication fails if another file already has the name.
    """
    validate(config)
    path = config_path(name)
    check_build_board(name, config["board"])
    if original is not None:
        if not isinstance(original, str):
            raise ValueError("original must be the saved configuration's exact text")
        previous = validate(json.loads(original))
        if config["board"] != previous["board"]:
            raise ValueError("saved configuration changed board; use a new configuration name")
        if path.read_text() != original:
            raise ValueError("configuration changed while editing; retry with the current file")
        if config == previous:
            return False
    elif path.exists() or path.is_symlink():
        raise FileExistsError(f"configuration already exists: {path}")

    # Publish only after a complete write. Editing preserves the original
    # permissions; new files use the temporary file's private permissions.
    path.parent.mkdir(exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            if original is not None:
                os.fchmod(output.fileno(), stat.S_IMODE(path.stat().st_mode))
            output.write(encode(config))
            output.flush()
            os.fsync(output.fileno())
        if original is not None and path.read_text() != original:
            raise ValueError("configuration changed while editing; retry with the current file")
        check_build_board(name, config["board"])
        if original is None:
            # Linking a completed same-directory file publishes atomically and
            # refuses a destination created by another caller while we wrote.
            try:
                os.link(temporary, path)
            except FileExistsError:
                raise
            except OSError as error:
                if error.errno not in NO_HARD_LINK_ERRNOS:
                    raise
                # No hard links here: reserve the name exclusively (a concurrent
                # creator still wins and is kept), then atomically replace the
                # empty reservation with the completed file.
                os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600))
                os.replace(temporary, path)
        else:
            os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return True


def edit(name, feature_sets=(), feature_unsets=(), sdk_sets=(), sdk_unsets=(), *, dry_run=False):
    path = config_path(name)
    original = path.read_text()
    config = validate(json.loads(original))
    candidate = apply_changes(config, feature_sets, feature_unsets, sdk_sets, sdk_unsets)
    if dry_run:
        check_build_board(name, candidate["board"])
        print(encode(candidate), end="")
        return candidate
    changed = save_configuration(name, candidate, original=original)
    print(f"{'Updated' if changed else 'Unchanged'} {path.relative_to(REPO)}")
    return candidate


def list_choices(category):
    if category == "boards":
        return [{"name": name, "target": target} for name, target in board_choices().items()]
    if category == "configurations":
        return [{"name": path.stem, "board": load(path.stem)["board"]}
                for path in sorted((REPO / "configurations").glob("*.json"))]
    namespace = "features" if category == "features" else "sdkconfig"
    return [{"name": name, **spec} for name, spec in choice_schema()[namespace].items()]


def show(name, *, as_json=False):
    config = load(name)
    if as_json:
        print(encode(config), end="")
        return
    print(f"Configuration: {name}\nBoard: {config['board']}")
    for namespace, label in (("features", "Feature overrides"), ("sdkconfig", "SDK overrides")):
        print(f"{label}:")
        choices = config.get(namespace, {})
        if not choices:
            print("  (none)")
        for key, value in sorted(choices.items()):
            shown = ("y" if value else "n") if namespace == "sdkconfig" else value
            print(f"  {key}={shown}")
    print("Unlisted choices inherit shared and board defaults at the source revision being built.")
    print("These are saved inputs; effective values require compiler verification with 'check'.")


def write_changed(path, text):
    # Keep mtimes stable on an incremental build; the manifest's freshness check
    # must not mistake an identical regenerated overlay for changed source.
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def prepare(name, board):
    config = load(name)
    if config["board"] != board:
        raise ValueError(f"{name} selects {config['board']}, not {board}")
    directory = build_dir(name)
    snapshot = directory / "configuration.json"
    check_build_board(name, board)
    directory.mkdir(parents=True, exist_ok=True)
    lines = ["// Generated by tools/build_config.py; edit configurations/" + name + ".json.",
             "#pragma once"]
    for key, value in sorted(config["features"].items()):
        lines.extend([f"#undef {key}", f"#define {key} {value}"])
    write_changed(directory / "features.h", "\n".join(lines) + "\n")
    sdk_lines = ["# Generated from configurations/" + name + ".json"]
    sdk_lines.extend(f"{key}={'y' if value else 'n'}"
                     for key, value in sorted(config.get("sdkconfig", {}).items()))
    write_changed(directory / "sdkconfig.features", "\n".join(sdk_lines) + "\n")
    write_changed(snapshot, encode(config))
    return config


def check(name):
    directory = build_dir(name)
    config = load(name)
    if (directory / "configuration.json").read_text() != encode(config):
        raise ValueError("configuration changed; prepare and reconfigure the build first")
    sdk, sdk_path = gen_build_info.read_sdkconfig(str(directory))
    if sdk_path != str(directory / "sdkconfig"):
        raise ValueError("build-local sdkconfig is missing; reconfigure this saved configuration")
    failures = []
    for key, requested in config.get("sdkconfig", {}).items():
        actual = sdk.get(key) == "y"
        if not sdk_path or requested != actual:
            failures.append(f"{key}: selected {'on' if requested else 'off'}, "
                            f"Kconfig resolved {'on' if actual else 'off'}")
    if failures:
        raise ValueError("SDK choices could not be applied:\n  " + "\n  ".join(failures))
    macros, error = gen_build_info.resolve_macros(str(directory))
    if error:
        raise ValueError(error)
    expected = directory / "features.h"
    # The wrapper may pass a logical (symlinked) checkout path while REPO is
    # resolved, so compare physical paths and name both when they differ.
    compiled = macros.get("configuration_header") or ""
    if not compiled or os.path.realpath(compiled) != os.path.realpath(expected):
        raise ValueError("compiler is not using this saved configuration; reconfigure the build\n"
                         f"  compiler uses: {compiled or '(no configuration header)'}\n"
                         f"  expected:      {expected}")
    failures = []
    for key, requested in config["features"].items():
        actual = macros["value"].get(key)
        if actual != str(requested):
            failures.append(f"{key}: selected {requested}, compiler resolved {actual or 'undefined'}")
    if failures:
        raise ValueError("choices were changed by board/dependency rules:\n  " +
                         "\n  ".join(failures) +
                         "\nCheck the corresponding rule in System_BuildConfig.h.")
    print(f"{name}: verified {len(config['features'])} feature input(s) and "
          f"{len(config.get('sdkconfig', {}))} SDK choice(s)")
    return macros


def build(name, idf_args=()):
    """Run the existing board wrapper; no extra actions are added by default."""
    config = load(name)
    command = ["bash", str(REPO / "tools/build_board.sh"), config["board"],
               "--config", name] + list(idf_args)
    return subprocess.call(command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create", help="save choices based on board defaults or another configuration")
    create.add_argument("name")
    origin = create.add_mutually_exclusive_group(required=True)
    origin.add_argument("--board")
    origin.add_argument("--from", dest="base")
    create.add_argument("--set", action="append", default=[], metavar="FEATURE=INTEGER")
    create.add_argument("--sdk", action="append", default=[], metavar="CONFIG_FEATURE=y|n")
    listing = commands.add_parser("list", help="discover boards, configurations, or selectable inputs")
    listing.add_argument("category", choices=("boards", "configurations", "features", "sdk"))
    listing.add_argument("--json", action="store_true")
    display = commands.add_parser("show", help="inspect saved choices and inheritance")
    display.add_argument("name")
    display.add_argument("--json", action="store_true")
    update = commands.add_parser("edit", help="change choices or remove overrides to restore inheritance")
    update.add_argument("name")
    update.add_argument("--set", action="append", default=[], metavar="FEATURE=INTEGER")
    update.add_argument("--unset", action="append", default=[], metavar="FEATURE")
    update.add_argument("--sdk", action="append", default=[], metavar="CONFIG_FEATURE=y|n")
    update.add_argument("--unset-sdk", action="append", default=[], metavar="CONFIG_FEATURE")
    update.add_argument("--dry-run", action="store_true",
                        help="validate input syntax and preview JSON without saving; "
                             "dependency and resource checks require a build")
    configure = commands.add_parser("configure", help="guided configuration creation or editing")
    configure.add_argument("name", nargs="?")
    source = configure.add_mutually_exclusive_group()
    source.add_argument("--board")
    source.add_argument("--from", dest="base")
    source.add_argument("--edit", action="store_true", help="edit an existing saved configuration")
    build_command = commands.add_parser("build", help="build a saved configuration")
    build_command.add_argument("name")
    build_command.add_argument("idf_args", nargs=argparse.REMAINDER)
    prep = commands.add_parser("prepare", help=argparse.SUPPRESS)
    prep.add_argument("name")
    prep.add_argument("board")
    verify = commands.add_parser("check", help="verify explicit choices with the configured build's compiler")
    verify.add_argument("name")
    args = parser.parse_args()
    try:
        if args.command == "create":
            config = load(args.base) if args.base else {"board": args.board, "features": {}}
            config = apply_changes(config, feature_sets=args.set, sdk_sets=args.sdk)
            save_configuration(args.name, config)
            path = config_path(args.name)
            print(f"Saved {path.relative_to(REPO)}")
        elif args.command == "list":
            rows = list_choices(args.category)
            if args.json:
                print(encode(rows), end="")
            else:
                for row in rows:
                    if args.category in ("boards", "configurations"):
                        print(f"{row['name']}\t{row.get('target', row.get('board'))}")
                    elif args.category == "features":
                        print(f"{row['name']}\tinteger {row['minimum']}..{row['maximum']}")
                    else:
                        print(f"{row['name']}\tboolean (y|n)")
                if args.category in ("features", "sdk"):
                    print("Listed types and ranges describe accepted inputs; "
                          "backend and dependency validation runs during build.")
        elif args.command == "show":
            show(args.name, as_json=args.json)
        elif args.command == "edit":
            edit(args.name, args.set, args.unset, args.sdk, args.unset_sdk, dry_run=args.dry_run)
        elif args.command == "configure":
            from build_config_wizard import run
            return run(sys.modules[__name__], args.name, board=args.board,
                       base=args.base, editing=args.edit)
        elif args.command == "prepare":
            prepare(args.name, args.board)
        elif args.command == "check":
            check(args.name)
        else:
            return build(args.name, args.idf_args)
    except (ValueError, OSError) as error:
        print(f"build_config: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
