#!/usr/bin/env python3
"""
build_darkroom_header.py — bundle "A Dark Room" into the HardwareOne firmware.

A Dark Room (https://github.com/doublespeakgames/adarkroom) is a multi-file web
game (MPL-2.0). This tool flattens it into a single self-contained HTML document
and emits components/hardwareone/WebPage_DarkRoom.h, where the document is stored
as C++ raw-string chunks and streamed at the /darkroom route (mirrors how the
Tilt Maze game is embedded in WebPage_Games.h).

What it does to make the game self-contained and offline-safe:
  - Inlines every local <script> and <link rel=stylesheet> in load order.
  - Inlines jQuery locally and drops the googleapis CDN <script> + its fallback.
  - Ships only EN/ES/FR/ZH_CN: trims langs.js, inlines each non-English
    strings.js into an in-memory dictionary map, inlines each per-language
    main.css, and rewires the dynamic (document.write) language loader to pick
    from those embedded maps based on ?lang= / localStorage.lang.
  - Removes Google Analytics (gtag loader) and injects no-op ga()/gtag() shims
    so the in-code ga('send', ...) call doesn't throw.
  - Strips audio: replaces audio.js with a no-op AudioEngine stub (same public
    surface) so the many in-game audio calls run without fetching audio files.
    audioLibrary.js is kept (it only defines name constants).
  - Inlines dark.css (the default theme) and rewires the lights toggle to inject
    it as a <style> instead of fetching css/dark.css.
  - Drops favicon / og:image / image_src <link>/<meta> (no asset fetches; the
    game itself is imageless).
  - Keeps normal saving (localStorage) and the export/import save string. Dropbox
    cloud-save is not loaded by index.html, so nothing to strip there.

Saves live in the player's browser (localStorage), not on the device.

Usage:
    python3 tools/build_darkroom_header.py            # clone if needed, build .h
    python3 tools/build_darkroom_header.py --html out.html   # also write standalone

Run from the repo root.
"""

import argparse
import os
import re
import subprocess
import sys

PINNED_COMMIT = "1fada4620b6c66bd07bf15a3f1eb8223df8bc1d7"
REPO_URL = "https://github.com/doublespeakgames/adarkroom.git"
NON_EN_LANGS = ["es", "fr", "zh_cn"]  # English is the base text (no strings file)

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_SRC = os.path.join(REPO_ROOT, "third_party", "adarkroom")
DEFAULT_HEADER = os.path.join(
    REPO_ROOT, "components", "hardwareone", "WebPage_DarkRoom.h"
)

# C++ raw-string delimiter. The literal ends at the first ")<DELIM>\"" so the
# bundle must never contain that exact sequence; we assert this below.
DELIM = "ADR"
CHUNK_BYTES = 24000


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def fail(msg):
    sys.stderr.write("ERROR: " + msg + "\n")
    sys.exit(1)


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def ensure_src(src):
    """Clone the pinned upstream into `src` if it isn't already present."""
    if os.path.isfile(os.path.join(src, "index.html")):
        return
    print("A Dark Room source not found at %s — cloning %s" % (src, REPO_URL))
    os.makedirs(os.path.dirname(src), exist_ok=True)
    subprocess.check_call(["git", "clone", REPO_URL, src])
    subprocess.check_call(["git", "-C", src, "checkout", PINNED_COMMIT])


def js_string(text):
    """Escape arbitrary text for embedding inside a double-quoted JS string that
    itself lives inside an inline <script> block."""
    text = text.replace("\\", "\\\\").replace('"', '\\"')
    text = text.replace("\r", "").replace("\n", "\\n")
    text = text.replace("</", "<\\/")  # never let CSS close the <script>
    return text


def script_block(js_code):
    return "<script>\n" + js_code + "\n</script>"


# --------------------------------------------------------------------------- #
# per-file transforms
# --------------------------------------------------------------------------- #
AUDIO_STUB = """// Audio removed from the HardwareOne build. No-op AudioEngine keeps the
// game's audio calls (playSound / playBackgroundMusic / ...) working without
// fetching any audio files. Public surface matches script/audio.js.
var AudioEngine = {
  init: function(){},
  isAudioContextRunning: function(){ return false; },
  tryResumingAudioContext: function(){},
  playBackgroundMusic: function(){},
  playEventMusic: function(){},
  stopEventMusic: function(){},
  playSound: function(){},
  loadAudioFile: function(){ return Promise.resolve(null); },
  setBackgroundMusicVolume: function(){},
  setMasterVolume: function(){}
};"""


def trimmed_langs_js(src):
    """langs.js defines `var langs = {...}` (the language-picker list). Keep only
    the languages we ship."""
    full = read(os.path.join(src, "lang", "langs.js"))
    keep = {"en": None}
    for code in NON_EN_LANGS:
        keep[code] = None
    pairs = dict(re.findall(r"'([^']+)'\s*:\s*'([^']*)'", full))
    out = ["var langs = {"]
    items = []
    for code in ["en"] + NON_EN_LANGS:
        if code not in pairs:
            fail("language '%s' not found in langs.js" % code)
        items.append("\t'%s':'%s'" % (code, pairs[code]))
    out.append(",\n".join(items))
    out.append("};")
    return "\n".join(out)


def patched_engine_js(src):
    """Rewire the dark-theme toggle to inject inlined CSS instead of fetching
    css/dark.css (dark mode is on by default, so this matters on first load)."""
    code = read(os.path.join(src, "script", "engine.js"))
    needle = (
        "$('head').append('<link rel=\"stylesheet\" href=\"css/dark.css\" "
        "type=\"text/css\" title=\"darkenLights\" />');"
    )
    replacement = (
        "$('head').append('<style id=\"darkenLights\" title=\"darkenLights\" "
        "type=\"text/css\">' + (window.__ADR_DARK_CSS || '') + '</style>');"
    )
    if needle not in code:
        fail("dark.css <link> append not found in engine.js (upstream drift?)")
    return code.replace(needle, replacement)


def inline_for_src(src_dir, rel):
    """Return the JS body to inline for a given <script src> path."""
    if rel == "lang/langs.js":
        return trimmed_langs_js(src_dir)
    if rel == "script/audio.js":
        return AUDIO_STUB
    if rel == "script/engine.js":
        return patched_engine_js(src_dir)
    path = os.path.join(src_dir, rel)
    if not os.path.isfile(path):
        fail("referenced script not found: %s" % rel)
    return read(path)


def lang_injection_block(src_dir):
    """The replacement for the original document.write() language loader:
    analytics shims + embedded dark/lang CSS + captured per-language
    dictionaries + a selector that applies the active language at load."""
    parts = []

    # 1) analytics no-op shims (engine.js calls ga(); index.html defined gtag()).
    dark_css = read(os.path.join(src_dir, "css", "dark.css"))
    lang_css = {}
    for code in NON_EN_LANGS:
        lang_css[code] = read(os.path.join(src_dir, "lang", code, "main.css"))

    shims = []
    shims.append("// HardwareOne bundle shims (analytics removed, lang loader rewired)")
    shims.append("window.dataLayer = window.dataLayer || [];")
    shims.append("window.gtag = window.gtag || function(){};")
    shims.append("window.ga = window.ga || function(){};")
    shims.append('window.__ADR_DARK_CSS = "%s";' % js_string(dark_css))
    css_entries = ", ".join(
        '"%s": "%s"' % (code, js_string(lang_css[code])) for code in NON_EN_LANGS
    )
    shims.append("window.__ADR_LANG_CSS = {%s};" % css_entries)
    shims.append("window.__ADR_LANG_DICT = {};")
    parts.append(script_block("\n".join(shims)))

    # 2) capture each non-English dictionary without letting the last one win.
    for code in NON_EN_LANGS:
        strings_js = read(os.path.join(src_dir, "lang", code, "strings.js"))
        if "</script" in strings_js.lower():
            fail("strings.js for %s contains </script (would break inlining)" % code)
        cap = (
            "(function(){ var captured = null, real = _.setTranslation;\n"
            "  _.setTranslation = function(t){ captured = t; };\n"
            "/* === lang/%s/strings.js === */\n%s\n"
            "  _.setTranslation = real; window.__ADR_LANG_DICT[%r] = captured; })();"
            % (code, strings_js, code)
        )
        parts.append(script_block(cap))

    # 3) selector: mirror upstream's ?lang= / localStorage.lang precedence.
    selector = """(function(){
  var l = null;
  try { l = decodeURIComponent((new RegExp('[?|&]lang=' + '([^&;]+?)(&|#|;|$)').exec(location.search)||[,""])[1].replace(/\\+/g, '%20')) || null; } catch(e) {}
  if (!l) { try { l = localStorage.lang; } catch(e) {} }
  if (l && l !== 'en' && window.__ADR_LANG_DICT[l]) {
    _.setTranslation(window.__ADR_LANG_DICT[l]);
    var css = window.__ADR_LANG_CSS[l];
    if (css) { var s = document.createElement('style'); s.type = 'text/css'; s.appendChild(document.createTextNode(css)); document.head.appendChild(s); }
  }
  window.__ADR_LANG = l || 'en';
})();"""
    parts.append(script_block(selector))
    return "\n".join(parts)


# --------------------------------------------------------------------------- #
# document assembly
# --------------------------------------------------------------------------- #
def build_html(src_dir):
    html = read(os.path.join(src_dir, "index.html"))
    SENTINEL = "__ADR_LANG_BLOCK__"

    def expect(pattern, text, what, flags=0):
        new, n = re.subn(pattern, "", text, flags=flags)
        if n == 0:
            fail("expected to strip %s but found none (upstream drift?)" % what)
        return new

    # Drop asset <meta>/<link> the game doesn't need (avoids 404 fetches).
    html = expect(r'\s*<meta[^>]*property="og:image"[^>]*>', html, "og:image meta")
    html = expect(r'\s*<link[^>]*rel="shortcut icon"[^>]*>', html, "favicon link")
    html = expect(r'\s*<link[^>]*rel="image_src"[^>]*>', html, "image_src link")

    # Replace the googleapis CDN jQuery with an inlined local copy...
    jq = read(os.path.join(src_dir, "lib", "jquery.min.js"))
    html, n = re.subn(
        r'<script[^>]*src="https://ajax\.googleapis\.com/[^"]*jquery[^"]*"[^>]*></script>',
        lambda m: script_block(jq),
        html,
    )
    if n != 1:
        fail("CDN jQuery <script> not found exactly once")
    # ...and drop the `if(!window.jQuery) document.write(...)` fallback block.
    html = expect(
        r"<script>\s*if\(!window\.jQuery\).*?</script>",
        html,
        "jQuery fallback block",
        flags=re.DOTALL,
    )

    # Remove Google Analytics: comment, async loader, and the inline config.
    html = expect(r"\s*<!--\s*Google tag \(gtag\.js\)\s*-->", html, "gtag comment")
    html = expect(
        r'\s*<script[^>]*src="https://www\.googletagmanager\.com/[^"]*"[^>]*></script>',
        html,
        "gtag loader",
    )
    html = expect(
        r"\s*<script>\s*window\.dataLayer.*?gtag\('config'.*?</script>",
        html,
        "gtag inline config",
        flags=re.DOTALL,
    )

    # Replace the dynamic language loader with our sentinel.
    html, n = re.subn(
        r'<script>\s*//\s*try to read "lang".*?</script>',
        SENTINEL,
        html,
        flags=re.DOTALL,
    )
    if n != 1:
        fail("dynamic language loader block not found exactly once")

    # Inline every remaining local <script src="...">.
    def repl_script(m):
        rel = m.group(1)
        if rel.startswith("http"):
            return ""  # any stray external script: drop
        return script_block(inline_for_src(src_dir, rel))

    html = re.sub(r'<script\s+src="([^"]+)"\s*></script>', repl_script, html)

    # Inline every <link rel="stylesheet" href="css/...">.
    def repl_css(m):
        rel = m.group(1)
        path = os.path.join(src_dir, rel)
        if not os.path.isfile(path):
            fail("referenced stylesheet not found: %s" % rel)
        return "<style>\n" + read(path) + "\n</style>"

    html = re.sub(
        r'<link\s+rel="stylesheet"\s+type="text/css"\s+href="(css/[^"]+)"\s*/?>',
        repl_css,
        html,
    )

    # Drop the inlined poedit-keyword line in body that re-uses _() at parse
    # time is fine (translate.js is already inlined above). Swap the sentinel.
    html = html.replace(SENTINEL, lang_injection_block(src_dir))

    # ---- sanity assertions: nothing external or unresolved remains ----
    leftovers = re.findall(r'<script\s+src=', html) + re.findall(r'<link[^>]*\.css"', html)
    if leftovers:
        fail("unresolved external references remain: %r" % leftovers[:5])
    for bad in ("ajax.googleapis.com", "googletagmanager.com"):
        if bad in html:
            fail("external reference %s still present after stripping" % bad)
    return html


# --------------------------------------------------------------------------- #
# header emission
# --------------------------------------------------------------------------- #
def chunkify(html):
    if (")" + DELIM + '"') in html:
        fail("bundle contains the raw-string close sequence; pick another DELIM")
    chunks = []
    cur = []
    cur_len = 0
    for line in html.splitlines(keepends=True):
        cur.append(line)
        cur_len += len(line.encode("utf-8"))
        if cur_len >= CHUNK_BYTES:
            chunks.append("".join(cur))
            cur = []
            cur_len = 0
    if cur:
        chunks.append("".join(cur))
    return chunks


def write_header(path, html):
    chunks = chunkify(html)
    out = []
    out.append("// AUTO-GENERATED by tools/build_darkroom_header.py — DO NOT EDIT.")
    out.append("// Regenerate with: python3 tools/build_darkroom_header.py")
    out.append("//")
    out.append("// A Dark Room (https://github.com/doublespeakgames/adarkroom)")
    out.append("//   pinned commit %s" % PINNED_COMMIT)
    out.append("//   License: Mozilla Public License 2.0 (see third_party/adarkroom/LICENSE.md)")
    out.append("// Languages: en, es, fr, zh_cn. Audio / Dropbox / analytics removed.")
    out.append("// Saves use the browser's localStorage (plus in-game export/import).")
    out.append("")
    out.append("#ifndef WEBPAGE_DARKROOM_H")
    out.append("#define WEBPAGE_DARKROOM_H")
    out.append("")
    out.append('#include <Arduino.h>')
    out.append('#include "System_BuildConfig.h"')
    out.append("#if ENABLE_HTTP_SERVER")
    out.append('#include <esp_http_server.h>')
    out.append("#endif")
    out.append("")
    out.append("#if ENABLE_WEB_GAME_DARKROOM")
    out.append("")
    out.append("// Streams the complete self-contained A Dark Room document. The caller")
    out.append("// is responsible for Content-Type, any caching headers, and the final")
    out.append("// httpd_resp_send_chunk(req, NULL, 0) terminator.")
    out.append("inline void streamDarkRoomDoc(httpd_req_t* req) {")
    for c in chunks:
        out.append('  httpd_resp_send_chunk(req, R"%s(%s)%s", HTTPD_RESP_USE_STRLEN);'
                   % (DELIM, c, DELIM))
    out.append("}")
    out.append("")
    out.append("void registerDarkRoomHandlers(httpd_handle_t server);")
    out.append("")
    out.append("#endif // ENABLE_WEB_GAME_DARKROOM")
    out.append("#endif // WEBPAGE_DARKROOM_H")
    out.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    return chunks


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description="Bundle A Dark Room into WebPage_DarkRoom.h")
    ap.add_argument("--src", default=DEFAULT_SRC, help="adarkroom checkout (auto-clone if absent)")
    ap.add_argument("--header", default=DEFAULT_HEADER, help="output .h path")
    ap.add_argument("--html", default=None, help="also write the standalone HTML here (for testing)")
    args = ap.parse_args()

    ensure_src(args.src)
    html = build_html(args.src)

    if args.html:
        with open(args.html, "w", encoding="utf-8") as f:
            f.write(html)
        print("wrote standalone HTML: %s (%d bytes)" % (args.html, len(html.encode("utf-8"))))

    chunks = write_header(args.header, html)
    size = len(html.encode("utf-8"))
    print("wrote %s" % args.header)
    print("  bundle: %d bytes (%.1f KB) in %d chunks" % (size, size / 1024.0, len(chunks)))


if __name__ == "__main__":
    main()
