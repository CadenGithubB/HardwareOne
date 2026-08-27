# tools/webui — host tests for the device's web-UI JavaScript

The device serves its web interface from C++ raw string literals compiled into
the firmware. That has one consequence worth stating plainly: **the firmware
build never parses any of it.** To the compiler, `R"JS( … )JS"` is an opaque
array of bytes. A missing brace, a typo'd identifier, a control-flow mistake —
all of it compiles clean, links clean, flashes clean, and then fails on the
board in front of whoever is holding it.

About 1.7 MiB of first-party JavaScript ships that way. This package is what
checks it before the board does.

What is here, in four parts:

| file | what it does |
|---|---|
| `tests/test_embedded_js_syntax.py` | parses **every** raw-string JS region in `components/hardwareone/` (78 regions across 36 files, ~1.7 MiB) |
| `tests/test_llm_page.py` | runs the `/llm` chat page's real JS against a stub DOM and a fake device, 32 behavioral checks |
| `tests/test_harness_detects_regressions.py` | breaks the page eight ways on purpose and asserts the harness notices each |
| `tests/test_js_engine.py` | asserts the engine-portability layer gives byte-identical output on every engine present |

### The syntax gate

Cheap, broad, and it reaches somewhere the build cannot: JavaScript inside a
block the current board never even preprocesses. The whole of `WebPage_Maps.h`
sits under `#if ENABLE_MAPS`, which is `0` in the current build config — the
preprocessor discards the file and no compiler on this machine has ever looked
at that JavaScript. The gate reads the *source*, so it does not care which board
you built. Not hypothetical: breaking a line in that discarded file is one of
the three mutations the suite asserts are caught.

### The behavioral harness

The `/llm` chat page, driven through the scenario that motivated it: a CM5
co-processor whose model catalog arrives asynchronously over UART, and whose
model "load" is a host-side llama-server restart finishing long after the HTTP
request that asked for it already returned. It asserts poll cadence,
announcement de-duplication, catalog diffing, selection survival, and that the
page stands down while the device is generating.

### The meta-test, and why it is not paranoia

Almost everything the harness asserts is a negative — *no* duplicate
announcement, *no* catalog rebuild, *no* polling during generation — and a
negative passes just as happily when the scenario never reached the state that
would violate it. So eight specific breakages are applied to a copy of the page
and the harness must notice each one, by name.

That caught two live faults while this was being written. One draft advanced its
clock by 400 ms to prove the page stops polling during generation; no heartbeat
was due inside a 400 ms window, so deleting the guard entirely still produced a
green run. Another reported "ALL CHECKS PASSED" while the page threw a
`TypeError` on every generation.

All of it takes input from the **real shipping source**. Nothing here holds a
copy of the page; a copy would only ever prove the copy works. That is the same
rule the C host suites state at `updater/test/host/CMakeLists.txt:9-10`.

## Running them

From the repository root:

```sh
python3 -m unittest discover -s tools/webui/tests -t .
```

Fifteen tests, about 1.2 s. They are also picked up by the repo-wide sweep
alongside the OTA suite, which goes from 29 tests to 44:

```sh
python3 -m unittest discover -s . -t .
```

To debug a failure, dump what the extractor actually produced:

```sh
python3 tools/webui/extract_js.py components/hardwareone/WebPage_LLM.h
```

To run a harness by hand on whatever engine this machine has:

```sh
python3 -m tools.webui.js_engine <harness.js> [args...]
```

Each harness takes its own arguments — `llm_page_harness.js` needs an extracted
page and an element-id list, which `tests/test_llm_page.py` builds in
`setUpClass`. Read that method for the real invocation rather than guessing;
run with no arguments and you get a stack trace and no verdict line.

## Requirements, and what happens without them

A JavaScript engine — `node`, `deno`, `bun`, `qjs`, macOS's `jsc`, or
`osascript`. `js_engine.py` finds whichever is present and normalises the
differences between them, which are not small: they disagree on how arguments
arrive, on whether a file is a module or a sloppy script, on where a stack trace
is printed, and on whether a script can set an exit code at all. That last one
is why every harness here reports its verdict as a sentinel line on stdout and
nothing ever trusts a return code.

With no engine at all, the suite **skips** rather than fails — the same way
`tools/ota/tests/test_bundle.py` skips when `openssl` is absent. On macOS
`osascript` always exists, so in practice the skip path only appears on Linux.

No third-party packages. No `pytest` (it is not installed in this repo and must
not become required). Stdlib `unittest` only.

## What a green run does not mean

Worth being blunt about, because a passing test is easy to over-read:

- **The gate checks syntax, not meaning.** It cannot tell that
  `WebPage_Maps.h:453` uses `LOD_MAJOR_ROAD`, a constant that only exists after
  `streamMapsPageLodZoomConstants()` (`WebPage_Maps.cpp:23`) injects it. Delete
  that C++ function and the gate stays green while the map breaks.
- **Only raw-string JS is covered.** JavaScript assembled from ordinary C++
  string literals gets nothing: `WebPage_Dashboard.h`, the shared `window.hw`
  client library in `WebServer_Utils.cpp`, and the i2c sensor web headers.
  Reaching those needs a C++ literal concatenator and an escape decoder, and
  feeding raw C++ text to a JS parser is a reliable way to manufacture false
  failures.
- **`WebPage_DarkRoom.h` is excluded** by path. It is vendored MPL-2.0 code
  generated by `tools/build_darkroom_header.py` and never hand-edited; gating it
  would produce a wall of noise every time the pinned upstream moves.
- **The harness is not a browser.** Its promise stub resolves synchronously, so
  it asserts an ordering a real browser never produces. It catches logic
  regressions, not rendering, layout, or event-loop bugs.
- **Nothing runs this automatically.** There is no CI in this repo and no git
  hook wired to it. It is discoverable and it is fast; it is not enforced.
