#!/usr/bin/env python3
"""Run the shipping ESP-NOW page renderers on a host JavaScript engine.

Executes web_assets/espnow-core.js (device list, peer metadata panel) and the
WebPage_ESPNow.h script chunks that define the graph view, the status apply, the
mesh role card, the local metadata / channel card and the button handlers,
unmodified, against a fake hw/DOM on whatever engine tools/webui/js_engine.py finds (node, deno, jsc,
...). Peer- and registry-supplied names and metadata must reach innerHTML as
literal text, clean text must render unchanged, and the Pair button must pass a
discovered name containing quotes intact. The page must decide from the json
replies of /api/espnow/status only: a text reply decides nothing, a slot that is
not json is tolerated, and settings strings are shape-checked and escaped. The
Initialize button opens the name setup from the json name fields and the mode
toggle reads 'espnowmode json'.
Static source contracts on the same renderers run first and need no engine
(--contract-only runs just those). They also pin the firmware side the page and
the Bond page rely on, which no host test compiles (System_ESPNow.cpp): the
'espnowstatus json' size guard, 'nameValid', the internal-RAM heap figure in
heartbeats and bond status, and the metadata setter limits taken from the wire
struct.
--source-dir replays other production sources (for example the pre-fix tree).
"""
import argparse
from pathlib import Path
import re
import sys
import tempfile

HERE = Path(__file__).resolve().parent
# The JS engine tooling lives with this suite's checkout, independent of the
# production overlay being tested.
REPO_ROOT = HERE.parents[3]

CORE = 'web_assets/espnow-core.js'
PAGE = 'WebPage_ESPNow.h'
ENDPOINT = 'WebPage_ESPNow.cpp'
FIRMWARE = 'System_ESPNow.cpp'
GRAPH_MARKER = 'window.refreshGraphView = function'
# The page <script> chunks the harness runs, in its argument order after espnow-core.js.
CHUNK_MARKERS = (
    GRAPH_MARKER,
    'window.espnowApplyStatus = function',
    'window.refreshMeshRole = function',
    'window.loadLocalDeviceMetadata = function',
    'window.setupButtonHandlers = function',
)
# /api/espnow/status reply slots. The page reads them by index
# (espnowApplyStatusSlots); slots 0-7 are json, slot 8 is display text.
STATUS_SLOTS = (
    'espnowstatus json', 'espnowmode json', 'bondstatus json', 'espnowlist', 'espnowmeshes listjson',
    'espnowdeviceinfo json', 'espnowmeshrole json', 'espnowmeshstatus', 'espnowstatus',
)
# Wording of the text replies the page used to match on. The page decides from
# json fields only, so none of these may appear in the page or in espnow-core.js.
TEXT_REPLY_WORDING = (
    'Initialized:', 'Last start error:', 'Peer MAC:', 'Friendly Name:', 'Mesh role:', '/Master MAC:',
    'Backup enabled:', '/Backup MAC:', 'channel preference:', 'Radio channel now:', 'Radio channel (actual):',
    '(not set)', "=== '(none)'", "indexOf('mesh')", '/MAC:', '/Room:', '/Zone:', '/Tags:', '/Stationary:', '/Channel:',
)
# Status commands the page may only request in their json form.
JSON_ONLY_COMMANDS = ('espnowmode', 'bondstatus', 'espnowdeviceinfo', 'espnowmeshrole', 'espnowchannel', 'devicename')


def region(text, start, end, what, failures):
    i = text.find(start)
    j = text.find(end, i + len(start)) if i >= 0 else -1
    if i < 0 or j < 0:
        failures.append(f'{what}: cannot locate {start!r} .. {end!r}')
        return ''
    return text[i:j]


def status_contract_failures(core, page, endpoint):
    """The page reads json replies: no text wording, json command forms, slot order."""
    failures = []
    for name, text in (('WebPage_ESPNow.h', page), ('espnow-core.js', core)):
        for wording in TEXT_REPLY_WORDING:
            if wording in text:
                failures.append(f'{name} still carries text-reply wording {wording!r}')
        for command in JSON_ONLY_COMMANDS:
            if re.search(r"cmd:\s*'" + command + r"'", text):
                failures.append(f"{name} requests the text form of '{command}'")
    table = re.search(r'static const char\* CMDS\[\] = \{(.*?)\};', endpoint, re.DOTALL)
    slots = tuple(re.findall(r'"([^"]+)"', table[1])) if table else ()
    if slots != STATUS_SLOTS:
        failures.append(f'/api/espnow/status slots are {slots}, expected {STATUS_SLOTS}')
    apply = region(page, 'window.espnowApplyStatusSlots = function', 'window.espnowRefreshStatusFromCli = function',
                   'status slot apply', failures)
    for index in range(len(STATUS_SLOTS)):
        if apply and f'results[{index}]' not in apply:
            failures.append(f'espnowApplyStatusSlots does not read results[{index}]')
    for raw in ("+ masterMAC;", "+ backupMAC;", "' (' + deviceMac + ')'", "+ role.charAt(0)"):
        if raw in page:
            failures.append(f'page renders a settings string raw: {raw}')
    if core.count('window.espnowParseReply = function') != 1:
        failures.append('espnow-core.js must define espnowParseReply exactly once')
    return failures


def function_body(text, signature, failures):
    """A top-level C++ function's text, from its signature to its closing brace."""
    i = text.find(signature + ' {')
    j = text.find('\n}\n', i) if i >= 0 else -1
    if i < 0 or j < 0:
        failures.append(f'System_ESPNow.cpp: cannot locate {signature!r}')
        return ''
    return text[i:j]


def firmware_contract_failures(espnow):
    """What the pages read from System_ESPNow.cpp. No host test compiles these lines."""
    failures = []
    internal_free = r'\(uint32_t\)hw1InternalFreeBytes\(\);'
    internal_min = r'\(uint32_t\)hw1InternalMinFreeBytes\(\);'

    def needs(body, pattern, what):
        if body and not re.search(pattern, body):
            failures.append(what)

    def forbids(body, needle, what):
        if needle in body:
            failures.append(what)

    # Heap figures sent to peers are internal byte-addressable RAM, the figure the
    # Bond page shows for this device (the wire fields keep their type and size).
    heartbeat = function_body(espnow, 'static void v4FillHeartbeat(V4PayloadHeartbeat& hb)', failures)
    needs(heartbeat, r'hb\.freeHeap\s*=\s*' + internal_free, 'v4FillHeartbeat: hb.freeHeap is not hw1InternalFreeBytes()')
    forbids(heartbeat, 'ESP.getFreeHeap', 'v4FillHeartbeat reads ESP.getFreeHeap()')
    bond = function_body(espnow, 'void buildLocalBondStatus(BondPeerStatus& status)', failures)
    needs(bond, r'status\.freeHeap\s*=\s*' + internal_free, 'buildLocalBondStatus: status.freeHeap is not hw1InternalFreeBytes()')
    needs(bond, r'status\.minFreeHeap\s*=\s*' + internal_min,
          'buildLocalBondStatus: status.minFreeHeap is not hw1InternalMinFreeBytes()')
    for reader in ('ESP.getFreeHeap', 'ESP.getMinFreeHeap'):
        forbids(bond, reader, f'buildLocalBondStatus reads {reader}()')
    bond_heartbeats = re.findall(r'^.*\bphb\.freeHeap\s*=.*$', espnow, re.MULTILINE)
    if not bond_heartbeats:
        failures.append('System_ESPNow.cpp: cannot locate the bond heartbeat phb.freeHeap line')
    for line in bond_heartbeats:
        if not re.search(r'phb\.freeHeap\s*=\s*' + internal_free, line):
            failures.append(f'bond heartbeat: phb.freeHeap is not hw1InternalFreeBytes(): {line.strip()}')

    # 'espnowstatus json': a document that does not fit is refused before it is written.
    status = function_body(espnow, 'const char* cmd_espnow_status(const String& argsInput)', failures)
    guard = status.find('measureJson(doc) + 1 > 1024')
    write = status.find('serializeJson(doc, getDebugBuffer(), 1024)')
    if status and (guard < 0 or write < 0 or guard > write):
        failures.append("cmd_espnow_status: no 'measureJson(doc) + 1 > 1024' check before serializeJson")
    if status and '"error\\":\\"response too large\\"' not in status[max(guard, 0):max(write, 0)]:
        failures.append('cmd_espnow_status: the size guard does not return the "response too large" error object')

    # 'espnowdeviceinfo json': the page's name-not-set check reads nameValid.
    info = function_body(espnow, 'const char* cmd_espnow_deviceinfo(const String& argsInput)', failures)
    needs(info, r'doc\["nameValid"\]\s*=\s*devicename::valid\(gSettings\.espnowDeviceName\.c_str\(\)\);',
          'cmd_espnow_deviceinfo: "nameValid" is not devicename::valid(the saved name)')

    # Setter limits come from the wire struct (V4PayloadMetadata), not literals.
    for setting, field in (('espnowRoom', 'room'), ('espnowZone', 'zone'), ('espnowTags', 'tags'),
                           ('espnowFriendlyName', 'friendlyName')):
        call = re.search(r'metaGetSet\(argsInput, gSettings\.' + setting + r',[^;]*;', espnow)
        if not call:
            failures.append(f'System_ESPNow.cpp: cannot locate the {setting} setter')
        elif f'sizeof(V4PayloadMetadata::{field}) - 1);' not in call[0]:
            failures.append(f'{setting} setter limit is not sizeof(V4PayloadMetadata::{field}) - 1: {call[0]}')
    return failures


def contract_failures(core, page):
    failures = []
    metadata = region(core, 'window.loadPeerMetadata = function', 'window.syncMetadata = function',
                      'metadata panel', failures)
    for field in ('deviceName', 'friendlyName', 'room', 'zone', 'tags'):
        if f'+ data.{field} +' in metadata:
            failures.append(f'metadata panel concatenates raw data.{field} into innerHTML')
        if metadata and f'esc(data.{field})' not in metadata:
            failures.append(f'metadata panel does not escape data.{field}')
    devices = region(core, 'window.renderUnifiedDeviceList = function', 'Per-device panel rendering and actions',
                     'device list', failures)
    for raw in ("'<strong>' + (deviceName || mac) + '</strong>'", "'<strong>' + mname + '</strong>'",
                "<strong>' + uname + '</strong>'", "uname.replace(/'/g"):
        if raw in devices:
            failures.append(f'device list renders peer text raw: {raw}')
    for escaped in ('esc(deviceName || mac)', 'esc(mname)', 'esc(uname)', 'esc(JSON.stringify(uname))'):
        if devices and escaped not in devices:
            failures.append(f'device list is missing {escaped}')
    helper = 'window.espnowEscapeHtml = function'
    if core.count(helper) != 1:
        failures.append(f'espnow-core.js defines {helper!r} {core.count(helper)} times, expected 1')
    elif core.index(helper) > core.find('Chunk 3A: listDevices function start'):
        failures.append('espnowEscapeHtml is defined after the first renderer')
    graph = region(page, GRAPH_MARKER, '// Auto-topology toggle', 'graph view', failures)
    for raw in ('statusDot + peer.name', 'selfDot + deviceName;'):
        if raw in graph:
            failures.append(f'graph view renders peer text raw: {raw}')
    if graph and 'topologyEscapeHtml(peer.name)' not in graph:
        failures.append('graph view does not escape peer.name')
    return failures


def script_chunk(page, marker):
    """The <script> block of the page that defines `marker`."""
    at = page.index(marker)
    start = page.rindex('<script>', 0, at) + len('<script>')
    end = page.index('</script>', at)
    chunk = page[start:end]
    if ')JS"' in chunk or 'R"JS(' in chunk:
        raise AssertionError(f'the chunk defining {marker!r} crosses a C++ raw-string boundary')
    return chunk


SKIP_RETURN_CODE = 77  # CTest SKIP_RETURN_CODE for the registered test


def run_behavior(core, page):
    sys.path.insert(0, str(REPO_ROOT))
    from tools.webui.js_engine import JS_ENGINE, NoJsEngine, run_js
    if JS_ENGINE is None:
        return None
    print(f'JavaScript engine: {JS_ENGINE[0]} ({JS_ENGINE[1]})', flush=True)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-page-render-') as tmp:
        core_path = Path(tmp) / 'espnow-core.js'
        core_path.write_text(core, encoding='utf-8')
        chunk_paths = []
        for index, marker in enumerate(CHUNK_MARKERS):
            if marker not in page:
                return [f'page chunk not found: {marker!r}']
            chunk_path = Path(tmp) / f'espnow-page-chunk-{index}.js'
            chunk_path.write_text(script_chunk(page, marker), encoding='utf-8')
            chunk_paths.append(str(chunk_path))
        try:
            result = run_js(HERE / 'espnow_page_render_test.js', [str(core_path)] + chunk_paths,
                            read_roots=[tmp])
        except NoJsEngine as error:
            return [str(error)]
    sys.stdout.write(result.stdout)
    sys.stdout.flush()
    sentinel = re.search(r'^HARNESS_RESULT passed=(\d+) failed=(\d+)$', result.stdout, re.MULTILINE)
    if not sentinel:
        if result.stderr:
            sys.stderr.write(result.stderr)
        return [f'render harness did not finish (no HARNESS_RESULT line; engine returncode {result.returncode})']
    passed, failed = int(sentinel[1]), int(sentinel[2])
    if failed or not passed:
        return [f'render harness: {failed} failed, {passed} passed']
    return []


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    p.add_argument('--contract-only', action='store_true', help='skip the JavaScript engine run')
    # Accepted for the shared test registration arguments; there is nothing to compile.
    p.add_argument('--cxx')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text(encoding='utf-8')

    core, page = read(CORE), read(PAGE)
    failures = (contract_failures(core, page) + status_contract_failures(core, page, read(ENDPOINT)) +
                firmware_contract_failures(read(FIRMWARE)))
    for failure in failures:
        print('FAIL contract:', failure, flush=True)
    if not failures:
        print('PASS source contracts', flush=True)
    behavior = [] if a.contract_only else run_behavior(core, page)
    if failures or behavior:
        failures += behavior or []
        print(f'{len(failures)} failure(s): ' + '; '.join(failures), file=sys.stderr)
        sys.exit(1)
    if behavior is None:
        # Source contracts passed but no engine exists (tools/webui/js_engine.py looked for
        # node, deno, bun, qjs, jsc and osascript). CTest reports this code as skipped,
        # like the Node-only browser suites without Node.
        print('SKIP render run: no JavaScript engine found', flush=True)
        sys.exit(SKIP_RETURN_CODE)
    print('PASS espnow_page_render')


if __name__ == '__main__':
    main()
