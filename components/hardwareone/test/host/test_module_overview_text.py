#!/usr/bin/env python3
"""The Bluetooth, OLED and R1 ring module overviews match their command rows.

docs/COMMAND_REFERENCE.md is regenerated from the module descriptions in
System_CommandRegistry.cpp, so a stale description reverts a corrected
reference. The registry is parsed with tools/command_registry.py (the same
parser the reference uses) and each overview is checked against the rows:

  bluetooth  blesecret, blesecure and blerequireauth are super admin rows, and
             the overview says so instead of "admin-only";
  oled       every ble/oled/ring/display command it names is registered (the
             removed oledrequireauth is not), and the display login switch is
             named as displayrequireauth with its tier and owning module;
  ring, phone, relay
             each overview names every one of its module's verb rows, `ring`
             names `ring control` with the row's own syntax and tier and no
             longer claims bridge commands that "exist in the code", and no
             registered row or overview uses a retired name (ringstatus,
             healthstatus, phonepipe, phonestatus, phonepipesecret, ...).

--source-dir points the suite at another copy of the component; its repository's
tools/command_registry.py is used (the pre-fix tree fails all three).
"""
import argparse
import importlib.util
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
COMMAND_TOKEN = re.compile(r'(?<![\w/*-])((?:ble|oled|ring|display)[a-z0-9]+)\b')


def load_registry_tool(component):
    path = component.parents[1] / 'tools' / 'command_registry.py'
    spec = importlib.util.spec_from_file_location('hw1_command_registry', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.COMPONENTS = component  # read this tree's sources
    return module


def sentence_with(text, word):
    for sentence in re.split(r'(?<=\.)\s+', text):
        if re.search(r'\b' + re.escape(word) + r'\b', sentence):
            return sentence
    return ''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', help='accepted for the shared ctest arguments; unused')
    parser.add_argument('--sanitize', action='store_true', help='accepted; unused')
    parser.add_argument('--source-dir', '--component', dest='source_dir', type=Path, default=COMPONENT)
    args = parser.parse_args()
    tool = load_registry_tool(args.source_dir)
    modules = {m['module']: m for m in tool.parse_modules()}
    by_array = tool.parse_commands()
    rows = {c['name'].lower(): c for v in by_array.values() for c in v}
    failures = []

    def check(ok, message):
        if not ok:
            failures.append(message)

    def unregistered(text):
        return sorted({t for t in COMMAND_TOKEN.findall(text) if t not in rows})

    # Bluetooth: the three posture commands are super admin.
    bt = modules['bluetooth']['summary']
    for name in ('blesecret', 'blesecure', 'blerequireauth'):
        check(rows[name]['superadmin'], f'test premise: `{name}` is no longer a super admin row')
        sentence = sentence_with(bt, name)
        check('super admin' in sentence and 'admin-only' not in sentence,
              f'bluetooth overview does not give `{name}` its super admin tier: "{sentence}"')
    check(not unregistered(bt), f'bluetooth overview names unregistered commands: {unregistered(bt)}')

    # OLED: displayrequireauth (super admin, Settings module), never oledrequireauth.
    oled = modules['oled']['summary']
    check(not unregistered(oled), f'oled overview names unregistered commands: {unregistered(oled)}')
    display = rows.get('displayrequireauth')
    check(display is not None and display['superadmin'], 'test premise: displayrequireauth is not a super admin row')
    owner = next((m['module'] for m in modules.values() if display and m['array'] == display['array']), '')
    sentence = sentence_with(oled, 'displayrequireauth')
    check(bool(sentence), 'oled overview does not name displayrequireauth for the display login')
    check('super admin' in sentence and owner.lower() == 'settings' and 'Settings module' in sentence,
          f'oled overview does not give displayrequireauth its tier and module ({owner}): "{sentence}"')

    # R1 ring: ring control with its syntax and tier; no bridge commands in the code.
    ring = modules['ring']['summary']
    control = rows.get('ring control')
    check(control is not None and control['admin'] and not control['superadmin'],
          'test premise: ring control is not an admin row')
    syntax = 'ring control <collection|lowpower> <preserve|on|off>'
    check(control is not None and syntax in control['usage'], 'test premise: ring control usage changed')
    check(syntax + ' (admin)' in ring, f'ring overview does not name `{syntax}` (admin)')
    check(not unregistered(ring), f'ring overview names unregistered commands: {unregistered(ring)}')
    check('exist in the code' not in ring and 'left unregistered' not in ring,
          'ring overview still claims unregistered bridge commands exist in the code')

    # Each device module's overview names every one of its verb rows (the
    # root row, e.g. `ring`, only answers with usage).
    for key in ('ring', 'phone', 'relay'):
        module = modules.get(key)
        check(module is not None, f'no `{key}` module')
        if not module:
            continue
        verbs = [c['name'] for c in by_array.get(module['array'], []) if ' ' in c['name']]
        check(bool(verbs), f'`{key}` module has no verb rows')
        missing = [v for v in verbs if v not in module['summary']]
        check(not missing, f'{key} overview does not name {missing}')
    retired = ('ringstatus', 'ringscan', 'ringconnect', 'ringdisconnect', 'ringquery', 'ringcontrol',
               'healthstatus', 'phonepipe', 'phonestatus', 'phonepipesecret')
    check(not any(r in rows for r in retired), f'retired names are registered: {[r for r in retired if r in rows]}')
    for m in modules.values():
        words = set(re.findall(r'[a-z0-9]+', m['summary']))
        check(not words & set(retired), f"{m['module']} overview names retired commands: {sorted(words & set(retired))}")

    for message in failures:
        print('FAIL', message, file=sys.stderr)
    if failures:
        sys.exit(1)
    print('module overviews: bluetooth posture commands super admin; oled names displayrequireauth '
          '(super admin, Settings); ring names ring control (admin) and no bridge commands; ring, '
          'phone and relay name all their verbs; no retired ring/phone names')


if __name__ == '__main__':
    main()
