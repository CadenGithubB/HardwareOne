#!/usr/bin/env python3
"""Run the shipping ESP-NOW page renderers on a host JavaScript engine.

Executes web_assets/espnow-core.js (device list, peer metadata panel) and the
WebPage_ESPNow.h script chunk that defines the graph view, unmodified, against a
fake hw/DOM on whatever engine tools/webui/js_engine.py finds (node, deno, jsc,
...). Peer- and registry-supplied names and metadata must reach innerHTML as
literal text, clean text must render unchanged, and the Pair button must pass a
discovered name containing quotes intact. Static source contracts on the same
renderers run first and need no engine (--contract-only runs just those).
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
GRAPH_MARKER = 'window.refreshGraphView = function'


def region(text, start, end, what, failures):
    i = text.find(start)
    j = text.find(end, i + len(start)) if i >= 0 else -1
    if i < 0 or j < 0:
        failures.append(f'{what}: cannot locate {start!r} .. {end!r}')
        return ''
    return text[i:j]


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


def graph_chunk(page):
    """The <script> block of the page that defines the graph view."""
    at = page.index(GRAPH_MARKER)
    start = page.rindex('<script>', 0, at) + len('<script>')
    end = page.index('</script>', at)
    chunk = page[start:end]
    if ')JS"' in chunk or 'R"JS(' in chunk:
        raise AssertionError('graph view chunk crosses a C++ raw-string boundary')
    return chunk


SKIP_RETURN_CODE = 77  # CTest SKIP_RETURN_CODE for the registered test


def run_behavior(core, page):
    sys.path.insert(0, str(REPO_ROOT))
    from tools.webui.js_engine import JS_ENGINE, NoJsEngine, run_js
    if JS_ENGINE is None:
        return None
    print(f'JavaScript engine: {JS_ENGINE[0]} ({JS_ENGINE[1]})', flush=True)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-page-render-') as tmp:
        core_path, chunk_path = Path(tmp) / 'espnow-core.js', Path(tmp) / 'espnow-graph-chunk.js'
        core_path.write_text(core, encoding='utf-8')
        chunk_path.write_text(graph_chunk(page), encoding='utf-8')
        try:
            result = run_js(HERE / 'espnow_page_render_test.js', [str(core_path), str(chunk_path)],
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
    failures = contract_failures(core, page)
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
