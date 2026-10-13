#!/usr/bin/env python3
"""Repository tools stay pinned to what the firmware actually has.

tools/command_registry.py `audit` checks that command strings the firmware runs
from code resolve to registered commands. The OLED screens run theirs through
oledRunCommand(), so CALL_RE must see that helper: every oledRunCommand("...")
literal in the component is scanned and resolves to a registered command.

tools/ble_secure_event_catalog_client.py validates a live `events kinds json`
reply against the checked-in compact fixture and pins its summary. Its
EXPECTED_FAMILY_COUNT / EXPECTED_KIND_COUNT / EXPECTED_CATALOG_BYTES must equal
the fixture's real family count, kind count and compact byte length (the
fixture itself is held to System_EventCatalogRows.h by event_catalog_fixture),
and so must the summary tools/ble_secure/test_secure_channel_v1.py asserts.
The constants are read with ast and the test's pins as text, so the client's
BLE/crypto dependencies are not needed.

--source-dir points the suite at another copy of the component; that
repository's tools are used (the pre-fix tree fails both).
"""
import argparse
import ast
import importlib.util
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
OLED_CALL = re.compile(r'\boledRunCommand\s*\(\s*"([^"]{2,60})"')


def load_registry_tool(component):
    path = component.parents[1] / 'tools' / 'command_registry.py'
    spec = importlib.util.spec_from_file_location('hw1_command_registry_pins', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.COMPONENTS = component
    return module


def client_constants(path):
    values = {}
    for node in ast.parse(path.read_text()).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            name = node.targets[0].id
            if name.startswith('EXPECTED_') and isinstance(node.value, ast.Constant):
                values[name] = node.value.value
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', help='accepted for the shared ctest arguments; unused')
    parser.add_argument('--sanitize', action='store_true', help='accepted; unused')
    parser.add_argument('--source-dir', '--component', dest='source_dir', type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.source_dir
    failures = []

    # F10: the audit scans oledRunCommand literals.
    tool = load_registry_tool(component)
    names = {c['name'].lower() for v in tool.parse_commands().values() for c in v}
    modules = {m['module'].lower() for m in tool.parse_modules()}
    literals = []
    for path in tool.sources():
        for lineno, line in enumerate(tool.read(path).splitlines(), 1):
            for match in OLED_CALL.finditer(line):
                literals.append((f'{path.name}:{lineno}', match.group(1).strip()))
    if not literals:
        failures.append('no oledRunCommand("...") literal found in the component; wrong --source-dir?')
    unseen = [where for where, text in literals
              if not any(m.group(2).strip() == text for m in tool.CALL_RE.finditer(f'oledRunCommand("{text}")'))]
    if unseen:
        failures.append(f'command_registry.py CALL_RE does not scan oledRunCommand: {len(unseen)} of '
                        f'{len(literals)} OLED command literals escape the audit (e.g. {unseen[0]})')
    for where, text in literals:
        low = text.lower()
        if not any(low == n or low.startswith(n + ' ') for n in names) and low.split()[0] not in modules:
            failures.append(f'{where}: oledRunCommand("{text}") resolves to no registered command')

    # H10 tools: the catalog client's pins equal the fixture's real summary.
    fixture_path = component / 'test' / 'host' / 'fixtures' / 'event_catalog_v1.json'
    fixture = json.loads(fixture_path.read_text())
    compact = json.dumps(fixture, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    actual = {
        'EXPECTED_FAMILY_COUNT': len(fixture['families']),
        'EXPECTED_KIND_COUNT': sum(len(f['k']) for f in fixture['families']),
        'EXPECTED_CATALOG_BYTES': len(compact),
    }
    pins = client_constants(component.parents[1] / 'tools' / 'ble_secure_event_catalog_client.py')
    for name, value in actual.items():
        if pins.get(name) != value:
            failures.append(f'ble_secure_event_catalog_client.py {name} = {pins.get(name)}, '
                            f'but the compact fixture has {value}')
    # The client's protocol test (run by tools/test_ble_secure_event_catalog_client.sh)
    # asserts the same summary from validate_live_catalog.
    summary = (actual['EXPECTED_FAMILY_COUNT'], actual['EXPECTED_KIND_COUNT'],
               actual['EXPECTED_CATALOG_BYTES'])
    protocol_test = component.parents[1] / 'tools' / 'ble_secure' / 'test_secure_channel_v1.py'
    pinned = re.findall(r'\(\s*summary\.family_count,\s*summary\.kind_count,\s*summary\.byte_count\s*\),'
                        r'\s*\(\s*(\d+),\s*(\d+),\s*(\d+)\s*\)', protocol_test.read_text())
    if not pinned:
        failures.append('tools/ble_secure/test_secure_channel_v1.py no longer pins the catalog summary; '
                        'update this check')
    for triple in pinned:
        if tuple(int(v) for v in triple) != summary:
            failures.append(f'tools/ble_secure/test_secure_channel_v1.py pins the catalog summary '
                            f'{tuple(int(v) for v in triple)}, but the compact fixture has {summary}')

    for message in failures:
        print('FAIL', message, file=sys.stderr)
    if failures:
        sys.exit(1)
    print(f'tool pins: command_registry audit scans {len(literals)} oledRunCommand literals, all registered; '
          f'catalog client pins {actual["EXPECTED_FAMILY_COUNT"]} families / {actual["EXPECTED_KIND_COUNT"]} '
          f'kinds / {actual["EXPECTED_CATALOG_BYTES"]} bytes, matching the fixture, and so does its '
          f'protocol test ({len(pinned)} pins)')


if __name__ == '__main__':
    main()
