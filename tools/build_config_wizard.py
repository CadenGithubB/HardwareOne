"""Terminal presentation for the shared saved-configuration API."""

import json
import re


class Cancelled(Exception):
    pass


def ask(prompt):
    answer = input(prompt).strip()
    if answer.lower() in ("q", "cancel"):
        raise Cancelled
    return answer


def choose(title, items, describe=lambda name: name, *, back=False):
    """Select a discovered name; a /substring narrows long feature lists."""
    names = list(items)
    if not names:
        raise ValueError(f"No choices available for {title.lower()}")
    visible = names
    while True:
        print(f"\n{title}")
        for index, name in enumerate(visible, 1):
            print(f"  {index}. {describe(name)}")
        hint = "; Enter to go back" if back else ""
        answer = ask(f"Number or name (=name for literal, /text to filter{hint}; q to cancel): ")
        if not answer and back:
            return None
        if answer.startswith("/"):
            matches = [name for name in names if answer[1:].lower() in name.lower()]
            if matches:
                visible = matches
            else:
                print("No matching choices. Use / to show all.")
            continue
        literal = answer.startswith("=")
        if literal:
            answer = answer[1:]
        if not literal and answer.isdecimal() and 1 <= int(answer) <= len(visible):
            return visible[int(answer) - 1]
        match = next((name for name in names if name.lower() == answer.lower()), None)
        if match:
            return match
        print("Choose one of the listed names or numbers.")


def source_help(core, namespace, key):
    """Show existing source comments without maintaining another feature catalog."""
    if namespace == "features":
        path = core.REPO / "components/hardwareone/System_BuildConfig.h"
        pattern = rf"^\s*#define\s+{re.escape(key)}\s"
        prefix = "//"
    else:
        path = core.REPO / "presets/sdkconfig.defaults"
        pattern = rf"^{re.escape(key)}="
        prefix = "#"
    lines = path.read_text().splitlines()
    index = next((i for i, line in enumerate(lines) if re.match(pattern, line)), None)
    if index is None:
        return ""
    if namespace == "features" and "//" in lines[index]:
        return lines[index].split("//", 1)[1].strip()
    comments = []
    for line in reversed(lines[max(0, index - 32):index]):
        line = line.strip()
        if line.startswith(prefix):
            comments.append(line[len(prefix):].strip())
        elif not line or (namespace == "features" and
                          re.match(r"#\s*(if|ifdef|ifndef|else|elif|endif)\b", line)):
            continue
        else:
            break
    return "\n".join(reversed(comments)).strip()


def choice_state(config, namespace, key):
    choices = config.get(namespace, {})
    if key not in choices:
        return "inherit"
    value = choices[key]
    if namespace == "sdkconfig":
        return "on" if value else "off"
    return str(value)


def change_choice(core, config, namespace):
    schema = core.choice_schema()[namespace]
    key = choose("Feature choices" if namespace == "features" else "SDK choices",
                 schema, lambda name: f"{name} [{choice_state(config, namespace, name)}]",
                 back=True)
    if key is None:
        return config
    help_text = source_help(core, namespace, key)
    if help_text:
        print(f"\n{help_text}")
    spec = schema[key]
    boolean = spec["type"] == "boolean" or spec.get("maximum") == 1
    values = "on/off" if boolean else f"{spec['minimum']}..{spec['maximum']}"
    print(f"{key}: {choice_state(config, namespace, key)}")
    while True:
        answer = ask(f"Value ({values}, inherit; Enter to keep): ").lower()
        if not answer:
            return config
        if answer == "inherit":
            change = {"feature_unsets" if namespace == "features" else "sdk_unsets": [key]}
        else:
            if boolean:
                aliases = {"on": "1", "off": "0", "y": "1", "n": "0",
                           "true": "1", "false": "0", "1": "1", "0": "0"}
                if answer not in aliases:
                    print("Use on, off, or inherit.")
                    continue
                answer = aliases[answer]
            if namespace == "sdkconfig":
                change = {"sdk_sets": [f"{key}={'y' if answer == '1' else 'n'}"]}
            else:
                change = {"feature_sets": [f"{key}={answer}"]}
        try:
            return core.apply_changes(config, **change)
        except ValueError as error:
            print(error)


def draft(core, name, board, base, editing):
    if not (board or base or editing):
        origin = choose("Start from", ("copy", "board", "edit"),
                        {"copy": "Copy a saved configuration", "board": "Board defaults",
                         "edit": "Edit a saved configuration"}.__getitem__)
        editing = origin == "edit"
        if origin == "copy":
            base = choose("Saved configurations", [row["name"] for row in core.list_choices("configurations")])
        elif origin == "board":
            boards = core.board_choices()
            board = choose("Boards", boards, lambda item: f"{item} ({boards[item]})")
    original = None
    if editing:
        if not name:
            name = choose("Configuration to edit", [row["name"] for row in core.list_choices("configurations")])
        original = core.config_path(name).read_text()
        config = core.validate(json.loads(original))
    else:
        config = core.load(base) if base else core.validate({"board": board, "features": {}})
        if name and core.config_path(name).exists():
            raise ValueError(f"{name} already exists; use configure {name} --edit")
        while not name:
            candidate_name = ask("New configuration name (=name for a literal name): ")
            if candidate_name.startswith("="):
                candidate_name = candidate_name[1:]
            try:
                path = core.config_path(candidate_name)
                if path.exists():
                    raise ValueError("That name already exists; choose a new name or cancel and use --edit.")
                core.check_build_board(candidate_name, config["board"])
                name = candidate_name
            except ValueError as error:
                print(error)
    core.check_build_board(name, config["board"])
    while True:
        print(f"\nConfiguration: {name} | Board: {config['board']}")
        print("  1. Feature choices\n  2. SDK choices\n  3. Review and save\n  q. Cancel")
        action = ask("Choose: ").lower()
        if action in ("1", "features"):
            config = change_choice(core, config, "features")
        elif action in ("2", "sdk"):
            config = change_choice(core, config, "sdkconfig")
        elif action in ("3", "review"):
            print(f"\nReview: configurations/{name}.json")
            print(core.encode(config), end="")
            print("Unlisted choices inherit defaults at the source revision being built.")
            print("The build checks dependencies and image size; these inputs have not been built.")
            while True:
                result = ask("s: Save | b: Save and build | e: Continue editing | q: Cancel: ").lower()
                if result in ("s", "save", "b", "build"):
                    return name, config, original, result in ("b", "build")
                if result in ("e", "edit", ""):
                    break
                print("Choose save, build, edit, or cancel.")
        else:
            print("Choose features, SDK, review, or cancel.")


def run(core, name=None, *, board=None, base=None, editing=False):
    """Use the caller's configuration API, including its repository and save rules."""
    print("Hardware One configuration setup")
    print("Changes stay in memory until you review and save. Use q to cancel.")
    try:
        name, config, original, build_now = draft(core, name, board, base, editing)
    except (Cancelled, EOFError, KeyboardInterrupt):
        print("\nCancelled. No configuration changes saved.")
        return 0
    try:
        changed = core.save_configuration(name, config, original=original)
    except KeyboardInterrupt:
        print("\nSave interrupted. Inspect the configuration before retrying.")
        return 130
    print(f"{'Saved' if changed else 'Unchanged'} configurations/{name}.json")
    if not build_now:
        print(f"Build when ready: python3 tools/build_config.py build {name}")
        return 0
    try:
        status = core.build(name)
    except KeyboardInterrupt:
        print("\nBuild interrupted. The configuration remains saved.")
        return 130
    if status:
        print("Build failed. The configuration remains saved; revise its choices or retry the build.")
    return status
