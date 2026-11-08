#!/usr/bin/env python3
"""Untrusted file names and beacon SSIDs on the shipping web pages.

File names: any authenticated user can upload under '/', and a bonded peer supplies its
own FS_LIST reply, so file names and paths reaching the shared explorers
(WebServer_Utils.h getFileBrowserScript: window.FileBrowser, createFileExplorer,
window.BondFs.renderExplorer; used by the Files, CLI, Logging, Maps, ESP-NOW and
Bond pages) are untrusted. The ESP-NOW remote browser (web_assets/espnow-core.js
browseRemoteFiles) feeds a peer's listing to BondFs and navigates by the peer's
folder names, so its own crumb and error sinks are untrusted too. SSIDs: the admin Settings Wi-Fi scan shows beacon SSIDs
from anyone in radio range.

Static source contracts run first and need no engine: the renderers must escape
names, path segments, peer labels and SSIDs, and must not splice a path between
quotes inside an inline onclick. Then the two file-browser <script> blocks and the
Settings Part 4 script run unmodified on whatever engine tools/webui/js_engine.py
finds (node, deno, jsc, ...), see web_untrusted_names_test.js: names holding
<img src=x onerror=...>, quotes and backticks must render as literal text, every
onclick must still hand the original path to its function, and clean names render
unchanged. --contract-only runs just the contracts; --source-dir replays other
production sources (for example the pre-fix tree).
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

UTILS = 'WebServer_Utils.h'
SETTINGS = 'WebPage_Settings.h'
MAPS = 'WebPage_Maps.h'
ESPNOW_CORE = 'web_assets/espnow-core.js'
SCAN_MARKER = 'window.scanNetworks = function'
SKIP_RETURN_CODE = 77  # CTest SKIP_RETURN_CODE for the registered test


def region(text, start, end, what, failures):
    i = text.find(start)
    j = text.find(end, i + len(start)) if i >= 0 else -1
    if i < 0 or j < 0:
        failures.append(f'{what}: cannot locate {start!r} .. {end!r}')
        return ''
    return text[i:j]


def file_browser_blocks(utils):
    """The two <script> blocks of getFileBrowserScript()'s raw string."""
    start = utils.index('R"FBSCRIPT(') + len('R"FBSCRIPT(')
    body = utils[start:utils.index(')FBSCRIPT"', start)]
    blocks = re.findall(r'<script>(.*?)</script>', body, re.S)
    if len(blocks) != 2:
        raise AssertionError(f'getFileBrowserScript has {len(blocks)} <script> blocks, expected 2')
    return blocks


def scan_chunk(settings):
    """The Settings <script> block that defines scanNetworks."""
    at = settings.index(SCAN_MARKER)
    start = settings.rindex('<script>', 0, at) + len('<script>')
    chunk = settings[start:settings.index('</script>', at)]
    if ')SETPART' in chunk or 'R"SETPART' in chunk:
        raise AssertionError('scan chunk crosses a C++ raw-string boundary')
    return chunk


def contract_failures(utils, settings, maps, espnow):
    failures = []
    fb = region(utils, 'window.FileBrowser = {', 'window.createFileExplorer = function',
                'FileBrowser helpers', failures)
    for helper in ('escHtml: function', 'jsArg: function'):
        if fb and fb.count(helper) != 1:
            failures.append(f'window.FileBrowser does not define {helper!r} once')
    # '&' first: without it an entity in a name (&quot;) survives escHtml and the
    # browser decodes it inside onclick="..." into a quote that ends the JS string.
    if fb and """.replace(/[&<>"'`]/g,""" not in fb:
        failures.append("escHtml does not escape all of & < > \" ' `")
    row = region(fb, 'rowHtml: function', '\n    }', 'rowHtml', failures) if fb else ''
    if row and "'<span>' + o.name + '</span>" in row:
        failures.append('rowHtml inserts o.name raw')
    crumb = region(fb, 'breadcrumbHtml: function', 'rowHtml: function', 'breadcrumbHtml', failures) if fb else ''
    if crumb and ("'\">' + part + '</span>'" in crumb or "'[' + displayRoot + ']" in crumb):
        failures.append('breadcrumbHtml inserts path parts raw')
    explorer = region(utils, 'window.createFileExplorer = function', 'window.createFileExplorerWithInput',
                      'createFileExplorer', failures)
    if "\\'' + itemPath + '\\'" in explorer:
        failures.append("createFileExplorer splices itemPath between quotes in an onclick")
    if """Navigate('" + p + "')""" in explorer:
        failures.append('createFileExplorer breadcrumb splices the path between quotes')
    bond = region(utils, 'function renderExplorer(', '// Public surface.', 'BondFs.renderExplorer', failures)
    for raw in ("'+full+'", "esc(path)", "esc(parent)", "esc(p)", "'+peerLabel+'", "'+emptyMsg+'",
                "Path: '+path+'"):
        if raw in bond:
            failures.append(f'BondFs.renderExplorer still uses {raw!r}')
    if 'function esc(s){' in utils:
        failures.append('BondFs keeps the JS-string-only esc() helper')
    scan = region(settings, SCAN_MARKER, 'window.selectSsid = function', 'Wi-Fi scan renderer', failures)
    if scan and "font-weight:bold\">' + ssid + " in scan:
        failures.append('Wi-Fi scan concatenates the raw SSID into innerHTML')
    if scan and 'escapeUserHtml(ssid)' not in scan:
        failures.append('Wi-Fi scan does not escape the SSID')
    if '${currentMap.filename}' in maps:
        failures.append('Maps details panel inserts the map file name raw')
    remote = region(espnow, 'window.browseRemoteFiles = function', 'window.parseRemoteFileListing = function',
                    'ESP-NOW browseRemoteFiles', failures)
    for line in remote.splitlines():
        if 'innerHTML' in line and re.search(r"\+ (browsePath|text) \+", line):
            failures.append('browseRemoteFiles concatenates a raw path or CLI reply into innerHTML: '
                            + line.strip()[:120])
    return failures


def run_behavior(utils, settings):
    sys.path.insert(0, str(REPO_ROOT))
    from tools.webui.js_engine import JS_ENGINE, NoJsEngine, run_js
    if JS_ENGINE is None:
        return None
    print(f'JavaScript engine: {JS_ENGINE[0]} ({JS_ENGINE[1]})', flush=True)
    blocks = file_browser_blocks(utils)
    with tempfile.TemporaryDirectory(prefix='hw1-web-untrusted-names-') as tmp:
        paths = [Path(tmp) / 'file-browser.js', Path(tmp) / 'bondfs.js', Path(tmp) / 'settings-scan.js']
        for path, text in zip(paths, blocks + [scan_chunk(settings)]):
            path.write_text(text, encoding='utf-8')
        try:
            result = run_js(HERE / 'web_untrusted_names_test.js', [str(p) for p in paths], read_roots=[tmp])
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
    a = p.parse_args()

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text(encoding='utf-8')

    utils, settings, maps = read(UTILS), read(SETTINGS), read(MAPS)
    failures = contract_failures(utils, settings, maps, read(ESPNOW_CORE))
    for failure in failures:
        print('FAIL contract:', failure, flush=True)
    if not failures:
        print('PASS source contracts', flush=True)
    behavior = [] if a.contract_only else run_behavior(utils, settings)
    if failures or behavior:
        failures += behavior or []
        print(f'{len(failures)} failure(s): ' + '; '.join(failures), file=sys.stderr)
        sys.exit(1)
    if behavior is None:
        print('SKIP render run: no JavaScript engine found', flush=True)
        sys.exit(SKIP_RETURN_CODE)
    print('PASS web_untrusted_names')


if __name__ == '__main__':
    main()
