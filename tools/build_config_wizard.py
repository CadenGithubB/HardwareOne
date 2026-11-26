"""Terminal presentation for the shared saved-configuration API.

The guided flow is board first: pick a board, pick a saved configuration of that
board to start from (or its defaults), then switch things on and off by group,
in plain words. build_config_catalog.py supplies the names and asks the
configuration header what each change does. Every switch stays reachable by its
own name, and SDK choices keep their list.
"""

import json
import re

from build_config_catalog import Refused, Setup


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
        path = core.REPO / "config/sdkconfig.features.defaults"
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


def change_raw(core, config, namespace):
    schema = core.choice_schema()[namespace]
    key = choose("Every switch, by name" if namespace == "features" else "SDK choices",
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


def pick(title, rows, extras=(), *, back=False):
    """Select one (key, label) row by number, key or label; extras are (word, label) commands."""
    rows, extras = list(rows), list(extras)
    while True:
        print(f"\n{title}")
        for index, (_, label) in enumerate(rows, 1):
            print(f"  {index:>2}. {label}")
        for word, label in extras:
            print(f"   {word}: {label}")
        hint = "; Enter to go back" if back else ""
        answer = ask(f"Number or name (=name for a literal name{hint}; q to cancel): ")
        if not answer and back:
            return None
        literal = answer.startswith("=")
        if literal:
            answer = answer[1:]
        if not literal and answer.isdecimal() and 1 <= int(answer) <= len(rows):
            return rows[int(answer) - 1][0]
        wanted = answer.lower()
        for key, label in rows + ([] if literal else extras):
            if wanted in (key.lower(), label.lower(), label.split(": ")[0].lower()):
                return key
        print("Choose one of the listed numbers or names.")


def described(title, text):
    return f"{title}: {text}" if text else title


def pick_board(core, setup):
    """Step 1. Returns a board name, or "edit" to change a saved configuration instead."""
    boards = core.board_choices()
    ordered = [name for name in setup.catalog["boards"] if name in boards]
    ordered += [name for name in boards if name not in ordered]
    rows = [(name, described(setup.board(name)["title"], setup.board(name).get("hardware", "")))
            for name in ordered]
    extras = [("edit", "change a saved configuration instead")] if core.list_choices("configurations") else []
    board = pick("1. Board", rows, extras)
    if board != "edit" and setup.board(board).get("caution"):
        print(f"\nCaution: {setup.board(board)['caution']}")
    return board


def pick_start(core, setup, board):
    """Step 2. Returns a saved configuration of this board to start from, or None for its defaults."""
    saved = core.list_choices("configurations", board)
    title = setup.board(board)["title"]
    if not saved:
        print(f"\n2. What should it do?  ({title})\n  No saved configuration for this board yet; starting from its defaults.")
        return None
    rows = [(row["name"], described(row.get("title", row["name"]), row.get("summary", "")) +
             (f"  [{row['name']}]" if "title" in row else "")) for row in saved]
    rows.append(("defaults", "Board defaults only"))
    start = pick(f"2. What should it do?  ({title})", rows)
    return None if start == "defaults" else start


def find_row(rows, answer):
    if answer.isdecimal() and 1 <= int(answer) <= len(rows):
        return rows[int(answer) - 1]
    wanted = answer.lower()
    return next((entry for entry in rows
                 if wanted in (entry[1]["title"].lower(), entry[1].get("switch", entry[1].get("id", "")).lower())), None)


def choice_screen(setup, config, choice):
    while True:
        values, errors = setup.evaluate(config)
        if values is None:
            print(f"Not available: {errors[0]}")
            return config
        now = setup.current(values, choice)
        others = {index: (lambda index=index: setup.change_choice(config, choice, index))
                  for index in range(len(choice["values"])) if index != now}
        blocked = setup.blocked(config, others)
        # A value the header rejects on every board of this kind only clutters the list.
        shown = [index for index in range(len(choice["values"])) if index == now or index not in blocked]
        hidden = len(choice["values"]) - len(shown)
        print(f"\n{choice['title']}: {choice['summary']}")
        for position, index in enumerate(shown, 1):
            print(f"  {position:>2}. {choice['values'][index]['title']}{'  (now)' if index == now else ''}")
        if hidden:
            print(f"  {hidden} more cannot be selected as things stand; type 'why' to list them.")
        answer = ask("Number or name; Enter to go back: ")
        if not answer:
            return config
        if answer.lower() == "why":
            for index, reason in sorted(blocked.items()):
                print(f"  {choice['values'][index]['title']}: {setup.named(reason)}")
            continue
        wanted = answer.lower()
        index = (shown[int(answer) - 1] if answer.isdecimal() and 1 <= int(answer) <= len(shown) else
                 next((i for i, value in enumerate(choice["values"]) if value["title"].lower() == wanted), None))
        if index is None:
            print("Choose one of the listed numbers or names.")
            continue
        try:
            config, notes = setup.change_choice(config, choice, index)
        except Refused as reason:
            print(f"Not changed: {setup.named(reason)}")
            continue
        print(f"{choice['title']}: {choice['values'][index]['title']}")
        for note in notes:
            print(f"  {note}")
        return config


def group_screen(setup, config, group):
    while True:
        values, errors = setup.evaluate(config)
        if values is None:
            print(f"Not available: {errors[0]}")
            return config
        rows = setup.group_rows(config, group["id"])
        off = {row["switch"]: (lambda switch=row["switch"]: setup.change_option(config, switch, True))
               for kind, row in rows if kind == "option" and not setup.is_on(values, row["switch"])}
        blocked = setup.blocked(config, off)
        print(f"\n{group['title']}   (x on, blank off, - cannot be turned on as things stand)")
        for index, (kind, row) in enumerate(rows, 1):
            if kind == "choice":
                now = setup.current(values, row)
                print(f"  {index:>2}. {row['title']}: {row['values'][now]['title'] if now is not None else 'other'}")
            else:
                mark = "x" if setup.is_on(values, row["switch"]) else "-" if row["switch"] in blocked else " "
                print(f"  {index:>2}. [{mark}] {described(row['title'], row['summary'])}")
        answer = ask("Number or name to change it; Enter to go back: ")
        if not answer:
            return config
        entry = find_row(rows, answer)
        if entry is None:
            print("Choose one of the listed numbers or names.")
            continue
        kind, row = entry
        if kind == "choice":
            config = choice_screen(setup, config, row)
            continue
        on = not setup.is_on(values, row["switch"])
        try:
            config, notes = setup.change_option(config, row["switch"], on)
        except Refused as reason:
            print(f"Not changed: {setup.named(reason)}")
            continue
        print(f"{row['title']}: {'on' if on else 'off'}")
        for note in notes:
            print(f"  {note}")


def show_groups(setup, config, numbered=True):
    groups = [group for group in setup.catalog["groups"] if setup.group_rows(config, group["id"])]
    width = max(len(group["title"]) for group in groups) if groups else 0
    for index, group in enumerate(groups, 1):
        number = f"{index:>2}. " if numbered else "  "
        print(f"  {number}{group['title']:<{width}}  {setup.summary(config, group['id'])}")
    for message in setup.evaluate(config)[1]:
        print(f"  Not buildable yet: {setup.named(message)}")
    return groups


def suggest_name(core, setup, board):
    stem = setup.board(board).get("short", board.replace("_", "-")) + "-custom"
    name, count = stem, 1
    while core.config_path(name).exists():
        count += 1
        name = f"{stem}-{count}"
    return name


def ask_name(core, setup, board):
    suggestion = suggest_name(core, setup, board)
    while True:
        answer = ask(f"Name for this configuration (Enter for {suggestion}; =name for a literal name): ")
        candidate = answer[1:] if answer.startswith("=") else answer or suggestion
        try:
            if core.config_path(candidate).exists():
                raise ValueError("That name already exists; choose a new name or cancel and use --edit.")
            core.check_build_board(candidate, board)
            return candidate
        except ValueError as error:
            print(error)


def ask_summary(core, config):
    while True:
        answer = ask("One line on what it is for (Enter to skip): ")
        if not answer:
            return config
        try:
            return core.validate(dict(config, summary=answer))
        except ValueError as error:
            print(error)


def draft(core, name, board, base, editing):
    setup = Setup(core)
    if not (board or base or editing):
        board = pick_board(core, setup)
        if board == "edit":
            board, editing = None, True
        else:
            base = pick_start(core, setup, board)
    original = None
    if editing:
        if not name:
            name = choose("Configuration to change", [row["name"] for row in core.list_choices("configurations")])
        original = core.config_path(name).read_text()
        config = core.validate(json.loads(original))
    else:
        # A copy is a new selection: it does not take the original's title or summary.
        config = core.build_inputs(core.load(base)) if base else core.validate({"board": board, "features": {}})
        if name and core.config_path(name).exists():
            raise ValueError(f"{name} already exists; use configure {name} --edit")
    if name:
        core.check_build_board(name, config["board"])
    while True:
        print(f"\n{name or 'New configuration'} on {setup.board(config['board'])['title']}"
              f"{'  (started from ' + base + ')' if base else ''}")
        groups = show_groups(setup, config)
        print("   names: every switch by name | sdk: SDK choices | review: review and save | q: cancel")
        action = ask("Group number or name, or names / sdk / review: ").lower()
        group = (groups[int(action) - 1] if action.isdecimal() and 1 <= int(action) <= len(groups) else
                 next((entry for entry in groups if action in (entry["title"].lower(), entry["id"])), None))
        if group:
            config = group_screen(setup, config, group)
        elif action in ("names", "features"):
            config = change_raw(core, config, "features")
        elif action == "sdk":
            config = change_raw(core, config, "sdkconfig")
        elif action in ("review", "r"):
            if not name:
                name = ask_name(core, setup, config["board"])
                config = ask_summary(core, config)
            print(f"\nReview: configurations/{name}.json on {setup.board(config['board'])['title']}")
            show_groups(setup, config, numbered=False)
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
            print("Choose a group, names, sdk, review, or q to cancel.")


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
