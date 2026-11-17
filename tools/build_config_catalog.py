"""What the guided setup offers on one board, and changes to a draft configuration.

Names, groups and descriptions come from build_config_catalog.json. Everything
else is asked of System_BuildConfig.h (build_config_resolve.py): whether an item
is on for this board and selection, and whether a change is allowed. A change
the header rejects is refused with the header's own message.
"""

from concurrent.futures import ThreadPoolExecutor
import json
import re

import build_config_resolve as resolver


class Refused(Exception):
    """The configuration header does not accept the change; str() says why."""


class Setup:
    def __init__(self, core):
        self.core = core
        self.repo = core.REPO
        self.catalog = core.catalog()
        self.schema = core.choice_schema()
        self.editable = set(self.schema["features"])
        self.derived = resolver.derived_names(self.repo)
        self.levels = resolver.level_groups(self.repo)
        self.options = {row["switch"]: row for row in self.catalog["options"] if row["switch"] in self.editable}
        self.choices = [row for row in self.catalog["choices"]
                        if all(key in self.editable for value in row["values"] for key in value["set"])]
        self.names = sorted(self.editable | set(self.derived.values()))
        self._evaluated = {}

    # --- what the catalogue covers ---------------------------------------------------------

    def owned(self):
        """Switches a multi-value choice sets, and the level switches the setup manages."""
        owned = {key for row in self.choices for value in row["values"] for key in value["set"]}
        return owned | (set(self.catalog["managed"]) & self.editable)

    def uncatalogued(self):
        """Switches the header offers that the catalogue has no name for yet; the setup lists them raw."""
        return sorted(self.editable - set(self.options) - self.owned())

    def title(self, switch):
        return self.options[switch]["title"] if switch in self.options else switch

    def named(self, message):
        """A header message with each switch it names followed by the name the setup shows for it."""
        def name(match):
            switch = match.group(0)
            return f'{switch} ("{self.options[switch]["title"]}")' if switch in self.options else switch
        return re.sub(r"\b(?:CUSTOM_)?ENABLE_\w+|\bXIAO_ESP32S3_SENSE_ENABLED\b", name, str(message))

    def board(self, name):
        return self.catalog["boards"].get(name, {"title": name, "hardware": ""})

    def offered(self, config, row):
        """False when the catalogue says the board has no such hardware."""
        board = config["board"]
        if "boards" in row and board not in row["boards"]:
            return False
        return "targets" not in row or self.core.board_choices().get(board) in row["targets"]

    # --- state -----------------------------------------------------------------------------

    def evaluate(self, config):
        """({name: value}, ()) for this board and selection. When the header rejects it: (values, its
        messages), or (None, messages) where the values could not be read either."""
        key = json.dumps(self.core.build_inputs(config), sort_keys=True)
        if key not in self._evaluated:
            target = self.core.board_choices()[config["board"]]
            self._evaluated[key] = resolver.resolve(self.repo, config["board"], target, config["features"],
                                                    config.get("sdkconfig"), self.names)
        return self._evaluated[key]

    def state(self, values, switch):
        """The value the firmware ends up with: a custom toggle is read through the switch it feeds."""
        return values.get(self.derived.get(switch, switch), 0)

    def is_on(self, values, switch):
        return bool(self.state(values, switch))

    def current(self, values, choice):
        """Index of the value a multi-value choice has now, or None when no listed value matches."""
        for index, value in enumerate(choice["values"]):
            if all(self.state(values, key) == wanted for key, wanted in value["set"].items()):
                return index
        return None

    def sdk_on(self, config, key):
        target = self.core.board_choices()[config["board"]]
        return key in resolver.sdk_defaults(self.repo, config["board"], target, config.get("sdkconfig"))

    # --- changes ---------------------------------------------------------------------------

    def _features(self, values, targets):
        """Overrides that give each switch its value and leave the rest of its level group as it is."""
        updates = {}
        for switch in targets:
            level = self.levels.get(switch)
            if level and values.get(level) != 4 and level not in updates:
                for member, owner in self.levels.items():
                    if owner == level and member in self.editable:
                        updates[member] = int(self.is_on(values, member))
                updates[level] = 4
        updates.update(targets)
        return updates

    def _apply(self, config, values, targets, sdk=None):
        features = self._features(values, targets)
        return self.core.apply_changes(
            config, feature_sets=[f"{key}={value}" for key, value in sorted(features.items())],
            sdk_sets=[f"{key}={'y' if value else 'n'}" for key, value in sorted((sdk or {}).items())])

    def _side_effects(self, before, after, changed):
        """Other catalogued items whose state the change moved, in plain words."""
        notes = []
        for word, now in (("on", True), ("off", False)):
            moved = [row["title"] for switch, row in self.options.items()
                     if switch not in changed and self.is_on(before, switch) != now
                     and self.is_on(after, switch) == now]
            if moved:
                notes.append(f"now {word} as well: " + ", ".join(moved))
        return notes

    def _accepted(self, candidate, standing):
        """The candidate's values. A selection the header already rejected may keep its messages while
        it is being repaired; a message it did not have refuses the change."""
        after, errors = self.evaluate(candidate)
        new = [message for message in errors if message not in standing]
        if after is None or new:
            raise Refused((new or errors)[0])
        return after

    def change_option(self, config, switch, on):
        """Return (new configuration, notes). Raises Refused with the reason."""
        before, standing = self.evaluate(config)
        if before is None:
            raise Refused(standing[0])
        row = self.options.get(switch, {})
        targets, sdk, notes = {}, {}, []
        if not on:
            dropped = [other for other in row.get("off_with", ()) if other in self.editable and self.is_on(before, other)]
            targets.update({other: 0 for other in dropped})
            if dropped:
                notes.append("turned off with it: " + ", ".join(self.title(other) for other in dropped))
        if on:
            needed = [need for need in row.get("needs", ()) if need in self.editable and not self.is_on(before, need)]
            for need in needed:
                targets[need] = 1
                for key in self.options.get(need, {}).get("sdk", ()):
                    if key in self.schema["sdkconfig"] and not self.sdk_on(config, key):
                        sdk[key] = True
            if needed:
                notes.append("turned on with it: " + ", ".join(self.title(need) for need in needed))
            for key in row.get("sdk_on", ()):
                if key in self.schema["sdkconfig"] and not self.sdk_on(config, key):
                    sdk[key] = True
                    notes.append(f"SDK choice {key} turned on with it")
        targets[switch] = int(on)
        for key in row.get("sdk", ()):
            if key in self.schema["sdkconfig"] and self.sdk_on(config, key) != on:
                sdk[key] = on
                notes.append(f"SDK choice {key} turned {'on' if on else 'off'} with it")
        candidate = self._apply(config, before, targets, sdk)
        try:
            after = self._accepted(candidate, standing)
        except Refused as reason:
            # Something that needs this item may still be on, and the header will not drop it by itself.
            dependants = {other: 0 for other, entry in self.options.items()
                          if switch in entry.get("needs", ()) and self.is_on(before, other)}
            if on or not dependants:
                raise Refused(self._hinted(row, reason)) from None
            targets = {**dependants, **targets}
            candidate = self._apply(config, before, targets, sdk)
            after = self._accepted(candidate, standing)
            notes.append("turned off with it: " + ", ".join(self.title(other) for other in dependants))
        if self.is_on(after, switch) != on:
            raise Refused(f"the firmware's rules keep {self.title(switch)} {'off' if on else 'on'} "
                          "for this board with this selection")
        return candidate, notes + self._side_effects(before, after, set(targets))

    @staticmethod
    def _hinted(row, reason):
        return f"{reason} {row['hint']}" if row.get("hint") else str(reason)

    def change_choice(self, config, choice, index):
        before, standing = self.evaluate(config)
        if before is None:
            raise Refused(standing[0])
        wanted = choice["values"][index]["set"]
        targets = {key: value for key, value in wanted.items() if self.state(before, key) != value}
        if not targets:
            return config, []
        candidate = self._apply(config, before, targets)
        after = self._accepted(candidate, standing)
        for key, value in wanted.items():
            if self.state(after, key) != value:
                raise Refused(f"the firmware's rules do not allow {choice['values'][index]['title']} "
                              "for this board with this selection")
        return candidate, self._side_effects(before, after, set(targets))

    def blocked(self, config, attempts):
        """Try each change (a callable returning (configuration, notes)); {key: reason} for the refused ones."""
        def attempt(item):
            key, change = item
            try:
                change()
            except Refused as reason:
                return key, str(reason)
            return key, None
        with ThreadPoolExecutor(max_workers=8) as pool:
            return {key: reason for key, reason in pool.map(attempt, attempts.items()) if reason}

    # --- views -----------------------------------------------------------------------------

    def group_rows(self, config, group):
        """The options and choices of one group that this board can have, in catalogue order."""
        rows = []
        for row in self.catalog["choices"]:
            if row in self.choices and row["group"] == group:
                rows.append(("choice", row))
        for switch, row in self.options.items():
            if row["group"] == group and self.offered(config, row):
                rows.append(("option", row))
        return rows

    def summary(self, config, group):
        """What is on in a group, for the overview."""
        values, _ = self.evaluate(config)
        if values is None:
            return "?"
        parts = []
        for kind, row in self.group_rows(config, group):
            if kind == "choice":
                index = self.current(values, row)
                parts.append(f"{row['title']}: {row['values'][index]['title'] if index is not None else 'other'}")
            elif self.is_on(values, row["switch"]):
                parts.append(row["title"])
        return ", ".join(parts) if parts else "nothing on"
