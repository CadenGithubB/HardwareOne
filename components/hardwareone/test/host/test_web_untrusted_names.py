#!/usr/bin/env python3
"""Untrusted file names and beacon SSIDs on the shipping web pages (readiness G-50/G-51).

G-50: any authenticated user can upload under '/', and a bonded peer supplies its
own FS_LIST reply, so file names and paths reaching the shared explorers
(WebServer_Utils.h getFileBrowserScript: window.FileBrowser, createFileExplorer,
window.BondFs.renderExplorer; used by the Files, CLI, Logging, Maps, ESP-NOW and
Bond pages) are untrusted. The ESP-NOW remote browser (web_assets/espnow-core.js
browseRemoteFiles) feeds a peer's listing to BondFs and navigates by the peer's
folder names, so its own crumb and error sinks are untrusted too. G-51: the admin Settings Wi-Fi scan shows beacon SSIDs
from anyone in radio range.

WEB-4 / D-29: recording names on the camera and microphone panels come off the
SD card or LittleFS, a bonded peer's name may be the free-text pairing-registry
name, and a remote sensor's JSON keys come from the remote device. The lists are
built from nodes (no inline onclick, encoded file URLs, delete commands that
quote the name), the bond page escapes the peer name and /api/bond/status
JSON-escapes it, and the generic remote-sensor rows escape nested keys. WEB-1:
hw.fetchJSON/fetchText reject with the reply's reason, not 'HTTP <n>'.

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
RUNTIME = 'WebServer_Utils.cpp'
CAMERA = 'System_Camera_DVP_Web.h'
MIC = 'System_Microphone_Web.h'
SENSORS = 'WebPage_Sensors.h'
BOND = 'WebPage_Bond.cpp'
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


def concat_script(path, marker):
    """The decoded <script> region of a concatenated-literal page holding marker."""
    sys.path.insert(0, str(REPO_ROOT))
    from tools.webui import extract_js
    for region in extract_js.concat_regions(path):
        if marker in region.text:
            return region.text
    raise AssertionError(f'{Path(path).name}: no <script> region holds {marker!r}')


def js_slice(text, start, last):
    """From `start` through the end of the column-0 function `last` (its '\n};')."""
    i = text.index(start)
    j = text.index('\n};', text.index(last, i)) + len('\n};')
    return text[i:j]


def js_function(text, marker):
    """A JS function declaration by brace matching (strings and comments aware)."""
    from test_web_batch_handlers import extract_block
    return extract_block(text, marker)


def bond_script(bond):
    start = bond.index('R"JS(') + len('R"JS(')
    return bond[start:bond.index(')JS"', start)]


def page_sources(read):
    """The decoded page scripts the WEB-4/D-29/WEB-1 checks look at; a missing
    marker is reported as a failure, not an exception."""
    out, failures = {}, []
    specs = {
        'runtime': lambda: concat_script(read.path(RUNTIME), 'hw.postFormText=function'),
        'camera': lambda: js_slice(concat_script(read.path(CAMERA), 'window.loadCameraRecordings = function'),
                                   'window.loadCameraRecordings = function', 'window.deleteCameraRecording = function'),
        'mic': lambda: js_slice(concat_script(read.path(MIC), 'window.loadMicRecordings = function'),
                                'window.__lastRecCount = -1;', 'window.deleteMicRecording = '),
        'sensor': lambda: js_slice(concat_script(read.path(SENSORS), 'window.hwRenderGenericSensor=function'),
                                   'window.hwRenderGenericSensor=function', 'window.hwRenderGenericSensor=function'),
        'bond': lambda: bond_script(read(BOND)),
    }
    for key, make in specs.items():
        try:
            out[key] = make()
        except (AssertionError, ValueError) as error:
            failures.append(f'{key} page script: {error}')
            out[key] = ''
    try:
        out['bond_progress'] = js_function(out['bond'], 'function renderBondProgress(') if out['bond'] else ''
    except (AssertionError, ValueError) as error:
        failures.append(f'bond progress renderer: {error}')
        out['bond_progress'] = ''
    return out, failures


def page_contract_failures(pages, bond_cpp):
    failures = []
    runtime = pages['runtime']
    if runtime:
        if not re.search(r"hw\.esc=function\(s\)\{return String\(s==null\?'':s\)\.replace\(/\[&<>\"'\]/g", runtime):
            failures.append("hw runtime does not define hw.esc escaping & < > \" '")
        for fn in ('hw.fetchJSON=function', 'hw.fetchText=function'):
            at = runtime.find(fn)
            body = runtime[at:runtime.find('};hw.', at)] if at >= 0 else ''
            if not body:
                failures.append(f'hw runtime does not define {fn[:-9]}')
            if "throw new Error('HTTP '+r.status)" in body:
                failures.append(f'{fn[:-9]} still throws a bare HTTP status instead of the reply reason')
    for key, verb in (('camera', 'cameravideodelete'), ('mic', 'micdelete')):
        text = pages[key]
        if not text:
            continue
        if 'onclick' in text or 'innerHTML = html' in text or "+ f.name +" in text or 'data-name' in text:
            failures.append(f'{key} recording list builds rows from HTML with the name or an inline handler')
        if 'encodeURIComponent(name)' not in text:
            failures.append(f'{key} recording file URL does not encode the name')
        # The positional quoted token has no escapes (CommandArgs::parse ends it
        # at the first "): the name goes between quotes as it is, and a name
        # holding " is refused on the page.
        if ".replace(/\\\\/g, '\\\\\\\\')" in text:
            failures.append(f'{key} {verb} doubles backslashes, which the positional quoted token does not unescape')
        if "indexOf('\"') >= 0" not in text:
            failures.append(f'{key} {verb} does not refuse a name holding a quote')
    sensor = pages['sensor']
    if sensor and ("return ik+':'" in sensor or "return sk+':'" in sensor):
        failures.append('hwRenderGenericSensor inserts nested sensor keys raw')
    bond = pages['bond']
    for line in bond.splitlines():
        if ('html +=' in line or 'peerLabel =' in line) and 'data.peerName' in line and 'hw.esc(' not in line:
            failures.append('bond page puts data.peerName into HTML unescaped: ' + line.strip()[:120])
    if '"\\"peerName\\":\\"%s\\","' in bond_cpp:
        failures.append('/api/bond/status prints peerName with %s and no JSON escaping')
    if 'jsonQuote(peerNameStr)' not in bond_cpp:
        failures.append('/api/bond/status does not JSON-quote peerName')
    return failures


def run_behavior(utils, settings, pages):
    sys.path.insert(0, str(REPO_ROOT))
    from tools.webui.js_engine import JS_ENGINE, NoJsEngine, run_js
    if JS_ENGINE is None:
        return None
    print(f'JavaScript engine: {JS_ENGINE[0]} ({JS_ENGINE[1]})', flush=True)
    blocks = file_browser_blocks(utils)
    with tempfile.TemporaryDirectory(prefix='hw1-web-untrusted-names-') as tmp:
        paths = [Path(tmp) / name for name in (
            'file-browser.js', 'bondfs.js', 'settings-scan.js', 'hw-runtime.js', 'camera-recordings.js',
            'mic-recordings.js', 'generic-sensor.js', 'bond-progress.js')]
        texts = blocks + [scan_chunk(settings)] + [
            pages[key] or "throw new Error('script not found');"
            for key in ('runtime', 'camera', 'mic', 'sensor', 'bond_progress')]
        for path, text in zip(paths, texts):
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

    def path_of(name):
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    def read(name):
        return path_of(name).read_text(encoding='utf-8')
    read.path = path_of

    utils, settings, maps = read(UTILS), read(SETTINGS), read(MAPS)
    failures = contract_failures(utils, settings, maps, read(ESPNOW_CORE))
    pages, missing = page_sources(read)
    failures += missing + page_contract_failures(pages, read(BOND))
    for failure in failures:
        print('FAIL contract:', failure, flush=True)
    if not failures:
        print('PASS source contracts', flush=True)
    behavior = [] if a.contract_only else run_behavior(utils, settings, pages)
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
