# Web JS / CSS Shared-Asset Audit

**Scope:** every browser-facing JavaScript and CSS byte under
`components/hardwareone/`, excluding `WebPage_Games.h` and `WebPage_DarkRoom.h`.
There are no `.js` files in this project and no bundler — every byte of browser
JavaScript lives inside a C++ string literal (`R"JS( … )JS"` raw strings and
ordinary concatenated `"…"` literals) that is streamed to the browser by
`httpd_resp_send_chunk`. Duplicated JS therefore costs **flash on the device**,
and no page's JS is ever parsed by the compiler.

**Method:** 12 independent page audits produced 35 clustered proposals. Every
proposal was then re-verified against the real C++ source by an adversarial
pass that re-grepped each cited line, re-counted each site list, and re-measured
each byte figure. **Every status label below traces to one of those verdicts.**
Where a verifier corrected a number, the corrected number is the one used —
this report never quotes a figure a verifier lowered.

**Verdict tally: 19 CONFIRMED, 16 PARTIAL, 0 fully refuted.** No proposal
collapsed entirely, but 16 had a cited line, a site count, a byte figure, or a
supporting sub-claim that did not survive. Those corrections are recorded in
§5, which is load-bearing: it exists to stop a future pass from re-filing work
that has already been checked and rejected.

---

## 1. Bottom line

**A shared runtime already exists and is almost entirely unadopted.**
`WebServer_Utils.cpp:773` streams a `window.hw` namespace — DOM helpers, an
HTTP layer with centralised 401 handling, a poll manager, a toast, a themed
confirm, a CLI-confirm that already implements the project's `OK:`/`Error:`
contract — into every authenticated page. Measured adoption: **1,005 raw
`document.getElementById` against 40 `hw._ge`; 368 raw `el.style.display=`
against 3 `hw.show/hide/toggle`; 200 `alert()` against 7 `hw.notify`; 35 raw
`setInterval` against 3 `hw.pollJSON`.** The pages that adopted anything
adopted only the HTTP layer.

The defensible flash saving is **roughly 300 KB**, and it splits sharply.
About **194 KB is comments and indentation** shipped verbatim inside raw string
literals — recoverable by a CMake pre-pass that changes not one line of JS
logic (P1, verifier-corrected *upward* from the original 60 KB estimate).
Another **~38 KB is verified-dead or pure-scaffolding code** (P2, P3). The
remaining **~78 KB is real deduplication** across 30 helper proposals. Two
large figures are deliberately **excluded** from that total because they are
not flash: **49,630 B of heap per request** on five pages (P5) and **35,897 B
of wire per CLI page load** (P17).

Six findings are live defects that only unification prevents recurring, and
they should ship regardless of whether any refactor does: `var(--warning)` is
used 15 times and declared **zero** times; `WebPage_Settings.h:2647` emits
`bluetoothrequireauth`, a verb that exists nowhere in the repository, so a
security toggle has never worked; SSE listeners are never re-attached after a
reconnect, so the Dashboard silently freezes on stale data;
`WebPage_Bluetooth.h:533` is an unbounded request loop; peer-supplied names
reach `innerHTML` unescaped on the bond channel; and `WebPage_Dashboard.h:111`
misspells a status flag so every board with an FM radio shows a false
"not compiled" banner.

---

## 2. The measured baseline

Re-counted directly from source at audit time (`grep -o … | wc -l` over
`components/hardwareone/*.h *.cpp`, games and darkroom excluded unless noted):

| Concept | Hand-rolled | Shared helper in use | Helper that already exists |
|---|---:|---:|---|
| Element lookup by id | **1,005** `document.getElementById` | **40** `hw._ge` | `hw._ge(idOrEl)` |
| Show / hide | **368** `el.style.display=` | **3** `hw.show/hide/toggle` | `hw.show/hide/toggle` |
| User notification | **200** `alert()` | **7** `hw.notify` | `hw.notify(level,msg,ms)` |
| CLI transport | **112** `hw.postForm*('/api/cli'…)` | **0** shared wrapper | *(none — P6)* |
| Polling | **35** raw `setInterval` | **3** `hw.pollJSON` | `hw.pollJSON(url,ms,cb)` |
| Promise chains | **362** `.then(` | — | — |
| Raw `fetch(` | **17** | — | `hw.fetchJSON/postJSON/fetchText/postForm` |
| HTML escaping | **7** private implementations | `hw._esc` (weakest of the 8) | `hw._esc(s)` |
| Reading `.value` | **105** `getElementById(x).value` | **0** | *(none — no `hw.val`)* |
| `hw.setText` | — | **27** | `hw.setText(x,t)` |

Notes on drift from the original brief's figures: the brief cited 963/360/192/
127/344/15. Re-measurement gives 1,005/368/200/112/362/17 on the same excluded
scope; the P4 verifier independently measured 1,088/377 counting all files.
The differences are counting-scope artifacts, not disagreements — every
ratio holds and the conclusion is unchanged.

Two structural counts worth stating separately:

- **~194 KB** of the JS corpus is comment-only lines, leading indentation and
  blank lines living inside `R"…( … )…"` literals, i.e. shipped to the browser
  and burned into flash (P1, measured in-source; a dump-side cross-check over
  the extracted `.js` files puts it between 204 KB and 232 KB depending on
  whether concatenated-literal indentation is counted).
- **814** `console.*` statements ship, of which ~195 are pure load-order
  sentinels. `WebServer_Utils.cpp:715-722` already stubs `console.log/warn/debug`
  to no-ops unless `gSettings.webConsoleDebug` is set — so the majority are paid
  for in flash and then discarded at runtime (P3).

---

## 3. What `hw.*` already gives every page

Streamed by `WebServer_Utils.cpp` into every authenticated page. **Read this
before writing a helper.** Line numbers are the streaming call sites.

| Group | API | Where |
|---|---|---|
| DOM | `hw.qs(sel,ctx)` `hw.qsa(sel,ctx)` `hw.on(el,evt,fn)` `hw._ge(idOrEl)` | `WebServer_Utils.cpp:773` |
| DOM | `hw.setText(x,t)` `hw.setHTML(x,h)` `hw.show(x)` `hw.hide(x)` `hw.toggle(x,shown)` | `WebServer_Utils.cpp:773` |
| HTTP | `hw.fetchJSON(url,opts)` `hw.postJSON(url,body,opts)` `hw.fetchText(url,opts)` | `WebServer_Utils.cpp:773` |
| HTTP | `hw.postForm(url,formObj,opts)` `hw.postFormText(url,formObj,opts)` | `WebServer_Utils.cpp:773` |
| HTTP | `hw._auth401(resp)` — centralised 401 → `/login` redirect | `WebServer_Utils.cpp:773` |
| Polling | `hw.pollJSON(url,intervalMs,cb)` → returns `stop()`; clears itself on `auth_required` | `WebServer_Utils.cpp:774` |
| CLI | `hw.cliConfirm(cliCmd,userPrompt,opts)` → `{cancelled,ok,pending,result}` | `WebServer_Utils.cpp:773` |
| Toast | `hw.notify(level,msg,ms)` · `hw._esc(s)` | `WebServer_Utils.cpp:812`, `:825` |
| Theme | `hw.applyTheme/loadThemePref/saveThemePref/initTheme/cycleTheme/updateThemeIcon` | `WebServer_Utils.cpp:762` |
| Auth | `hw.isGuest()` `hw.isAdmin()` `hw.applyGuestViewOnly()` | `WebServer_Utils.cpp:773` |
| Dialogs | `window.hwConfirm(msg)` → `Promise<bool>` · `window.hwPrompt(…)` | `WebServer_Utils.h:1543` |
| SSE | `sseNotify()` owns `window.__es`, auto-connects, retries after 10 s | `WebServer_Utils.cpp:828` |
| Streaming | `streamChunkC(req, const char*)` — zero-allocation chunk send | `WebServer_Utils.h:1226` |
| Bonded FS | `window.BondFs.{checkAvailable,exec,list,stat,pull,renderExplorer}` | `WebServer_Utils.h:979-1218` |
| File UI | `window.FileBrowser.*`, `createFileExplorer`, `createFileManager`, `hwUploadFile` | `WebServer_Utils.h:187-970` |

**Three availability traps that shape every proposal below:**

1. `hw.notify`, `hw._esc`, `hw.cliConfirm` and `streamCommonDialogs` are all
   defined **inside** the `if (!isPublic)` block that opens at
   `WebServer_Utils.cpp:784` / `:779`. They do **not** exist on `/login` or
   `/register`. Only the `hw.qs`/`hw.pollJSON` bundle at
   `WebServer_Utils.cpp:773-774` is unconditional.
2. `window.BondFs` and `window.FileBrowser` ship only inside
   `getFileBrowserScript()` (`WebServer_Utils.h:186`), which five pages stream
   and everyone else cannot reach. This is why Settings hand-rolls the
   `/api/bond/status` probe at `WebPage_Settings.h:3688`.
3. The standalone file-viewer shell (`WebServer_Server.cpp:4406-4414`) states in
   its own comment that it carries **none** of `streamBeginHtml()`'s output.
   Any `hw.*` referenced from a code path reachable there is `undefined`.

---

## 4. Ranked opportunities

Ranked by `(defensible bytes × sites) / risk`, with correctness weighted up.
**Bytes column is the verifier-corrected figure**, never the proposal's own
estimate where the two disagree. `†` marks a figure that is *not* flash.

| ID | Proposed API / action | Cat. | Sites | Bytes | Effort | Risk | Verdict |
|---|---|---|---:|---:|---|---|---|
| **P1** | CMake pre-pass strips `//` comments, indent, blank lines from JS literals | NEW SHARED | 42 regions | **194,000** | M | low | **CONFIRMED** |
| **P5** | `getFileBrowserScript()`: `inline String` → `const char*` + `streamChunkC` | REDUNDANT | 5 callers | **49,630 †heap/req** | S | low | **CONFIRMED** |
| **P17** | Split `getBondFsScript()` out of `getFileBrowserScript()` | NEW SHARED | 7 | **35,897 †wire/load** | M | med | **CONFIRMED** |
| **P2** | Delete dead and unreachable JavaScript | REDUNDANT | 22 | **33,000** | M | low | **PARTIAL** |
| **P3** | `hw.dbg(tag)` gated logger; delete console scaffolding | NEW SHARED | 814 | **15,000** | M | low | **PARTIAL** |
| **P4** | Adopt `hw` DOM layer; add `hw.val` and `hw.flip` | REDUNDANT | 1,570 | **13,500** | L | low | **CONFIRMED** |
| **P13** | `SchemaPanel` as the ONE schema-driven settings renderer | PROMOTE | 10 | **12,000** | L | high | **PARTIAL** |
| **P8** | `hw.peerExec()` — one peer command collector | PROMOTE | 4 impls | **6,000** | L | med | **PARTIAL** |
| **P7** | CSS token + class consolidation; declare `--warning`/`--panel-border` | REDUNDANT | 60 | **4,000** | M | med | **CONFIRMED** |
| **P10** | `hw.poll` — one poll manager with all the guards | PROMOTE | 35 | **4,000** | M | med | **CONFIRMED** |
| **P12** | `hw.setDot` / `hw.badge` — collapse 2 CSS families + 80 sites | NEW SHARED | 80 | **4,000** | M | low | **CONFIRMED** |
| **P14** | `hw.kvRow` / `hw.renderKV` — label/value row | PROMOTE | 44 | **4,000** | M | low | **PARTIAL** |
| **P9** | `hw.tabs` / `hw.showOnly` / `hw.togglePane` | NEW SHARED | 24 | **3,800** | M | med | **PARTIAL** |
| **P11** | `hw.status` / `hw.banner` — level-coloured status line | NEW SHARED | 70 | **3,000** | M | low | **CONFIRMED** |
| **P15** | `hw.renderList` / `hw.renderTable` / `hw.emptyState` | NEW SHARED | 22 | **3,000** | M | med | **CONFIRMED** |
| **P6** | `hw.cli()` — CLI transport that preserves the body on non-2xx | NEW SHARED | 112 | **2,800** | M | med | **CONFIRMED** |
| **P21** | `hw.download` / `downloadJSON` / `pickJSONFile` / `upload` / `fetchBuffer` | NEW SHARED | 20 | **2,500** | M | low | **CONFIRMED** |
| **P22** | `hw.sse` — EventSource manager whose listeners survive reconnects | NEW SHARED | 6 | **2,500** | M | med | **CONFIRMED** |
| **P26** | `hw.bindCliInput` + `hw.debounce` | NEW SHARED | 16 | **2,400** | M | low | **PARTIAL** |
| **P20** | `hw.fmt` — bytes / duration / timestamp / missing-value / pad | NEW SHARED | 30 | **2,000** | S | med | **CONFIRMED** |
| **P33** | Serialise `hw.cli` against the `/api/cli` rate limit | PROMOTE | 112 | **1,600** | M | med | **PARTIAL** |
| **P18** | `hw.esc()` — one quote-safe HTML escaper | REDUNDANT | 10 | **1,500** | S | low | **CONFIRMED** |
| **P25** | `hw.modal` — one shell with backdrop + Escape | NEW SHARED | 12 | **1,500** | M | low | **CONFIRMED** |
| **P32** | `hw.sensorReader` — one envelope for the sensor plugin registry | NEW SHARED | 10 | **1,400** | S | low | **PARTIAL** |
| **P23** | `hw.busy(btn,label) -> restore()` | NEW SHARED | 14 | **1,200** | S | low | **CONFIRMED** |
| **P30** | `hw.fillSelect` — idempotent, selection-preserving | NEW SHARED | 13 | **1,200** | S | low | **CONFIRMED** |
| **P28** | `hw.cliFailed` / `hw.cliPending` — one failure predicate | PROMOTE | 33 | **800** | S | med | **PARTIAL** |
| **P29** | Retire the 10 native `confirm()` dialogs | REDUNDANT | 25 | **800** | M | med | **CONFIRMED** |
| **P16** | `hw.delegate` — event delegation instead of interpolated `onclick` | PROMOTE | 295 attrs | **600** | M | med | **PARTIAL** |
| **P27** | `hw.bindCliToggle` — reflect the DEVICE's state, not the click | NEW SHARED | 14 | **600** | M | med | **PARTIAL** |
| **P35** | `hw.bondTarget` — the This-Device / Bonded-Device switcher | NEW SHARED | 8 | **500** | M | med | **PARTIAL** |
| **P31** | `hw.cachedJSON` / `hw.settings.load` / `hw.bondStatus` | NEW SHARED | 25 | **400** | M | med | **PARTIAL** |
| **P34** | `hw.basename` / `pathJoin` / `parentPath` / `macToken` / `quoteArg` | PROMOTE | 35 | **150** | S | med | **PARTIAL** |
| **P19** | Escape untrusted strings at the ~25 unguarded `innerHTML` sinks | NEW SHARED | 25 | **0** | M | low | **CONFIRMED** |
| **P24** | Retire the 200 blocking `alert()` calls | REDUNDANT | 200 | **−2,200** | M | med | **PARTIAL** |

### Honest arithmetic

```
Tier A — build-time strip and deletion, no logic change
  P1  comments + indent + blank inside JS literals        194,000
  P2  dead code, less ~30% already counted in P1           23,100
  P3  console scaffolding (sentinels deleted, rest gated)  15,000
                                                   Tier A  232,100

Tier B — real deduplication (P4, P6–P23, P25–P35)
  gross                                                    81,750
  less P4 overlap with P11/P12/P14/P20                     -4,000
                                                   Tier B   77,750

                                       DEFENSIBLE FLASH   ~309,850  (~300 KB)

NOT flash — do not add to the total
  P5   49,630 B of heap, per request, on 5 pages
  P17  35,897 B of wire, per CLI page load
  P24  ADDS ~2,200 B; its value is user experience, not bytes
  P19  0 B; ranked on security, not size
```

---

### P1 — Strip comments and indentation from JS/CSS literals at build time — **CONFIRMED**

*The single largest number in this audit, and it changes not one line of JS.*

Every byte inside `R"JS( … )JS"` is streamed verbatim to the browser **and**
stored in `.rodata`. Per-file measurements, all re-verified in-source:

| File | Literal opens at | Comment+indent+blank | % of that file's JS |
|---|---|---:|---:|
| `WebPage_LLM.h` | `:231` | 14,287 B | 35.8% |
| `WebPage_Settings.h` | `:7` | 42,823 B | 24.8% |
| `WebPage_ESPNow.h` | `:302` | 39,100 B | 22.5% |
| `WebPage_Bluetooth.h` | `:432` | 12,299 B | 38.0% |
| `WebPage_Bond.cpp` | `:143` | 11,824 B | 28.3% |
| `WebPage_Automations.h` | `:278` | 10,589 B | 12.7% |
| `WebPage_Files.h` | `:61` | 3,777 B | 32.9% |
| `WebPage_AviPlayer.h` | `:83` | **766 B indent, 0 comment** | — |

`WebPage_AviPlayer.h` is the proof of concept: it already keeps its explanatory
comments in C++ **between** concatenated literals, so it ships 6.5 KB of JS with
zero comment bytes. The comments elsewhere are genuinely good — several record
bugs already fixed — so deletion is wrong. A build pre-pass keeps them in source
and off the device.

**Choose the in-place variant, not `websrc/*.js`.** Preprocessor gating and
runtime interpolation are interleaved *between* JS chunks —
`WebPage_Sensors.h` has 117 `#if`/`#else`/`#endif` lines, `WebPage_Bond.cpp` 46,
`WebPage_Settings.h` 26, plus 8–28 `snprintf`/`String(` sites per file. A pass
that rewrites only the **body** of each `R"TAG( … )TAG"` sidesteps all of it,
because preprocessor directives cannot live inside a raw literal.

**Blockers (verifier-found, all must be handled):**
- Needs a real JS tokenizer, not a line regex — **63 string literals** in the
  component contain `//` (e.g. `G2_Page_Files.cpp:875` `"// Pretty parse failed: "`).
- **442 B of indentation sits inside template literals** (390 B of it in
  `WebPage_Maps.h`) and is part of the emitted string. Preserve it.
- No ASI hazard: only comments, leading indentation and blank lines are removed;
  line joins never are.
- Concat-literal files already pay zero for comments — `WebPage_Dashboard.h`
  (556 quoted lines, 0 comment / 530 B indent), `WebPage_AviPlayer.h`,
  `WebServer_Utils.h`, `WebPage_Sensors.h`, `WebServer_Utils.cpp`. Rewriting
  those into `websrc/` is a large diff for indentation-only gain.
- `tools/webui/tests/test_llm_page.py` and `test_embedded_js_syntax.py` extract
  JS from these literals. The pass must run on generated output, not on the
  source those tests read.
- The standalone file-viewer shell (`WebServer_Server.cpp:4407-4413`) carries its
  own JS and none of `streamBeginHtml`'s output — include it by path.

Feeds the existing **Web asset strip plan** (`websrc/` + CMake pre-pass) rather
than being new infrastructure. **Do this first** — it makes every later diff smaller.

---

### P5 — `getFileBrowserScript()`: `inline String` → `const char*` — **CONFIRMED**

*Highest value per line changed in the entire report.*

`WebServer_Utils.h:186` is declared `inline String` and returns a raw literal
measured at **49,630 bytes** (the proposal said 49,627 — off by 3). Every call
therefore constructs an Arduino String: one ~49.7 KB `malloc` from internal
DRAM plus a memcpy, purely to hand a `const char*` to `httpd_resp_send_chunk`.

`streamChunkC` already exists at `WebServer_Utils.h:1226` (impl
`WebServer_Utils.cpp:577` — `strlen()` then `httpd_resp_send_chunk`) and every
other shared block in `WebServer_Utils.cpp` uses it.

```cpp
// before — WebServer_Utils.h:186, and at each of the 5 callers
inline String getFileBrowserScript() { return R"FBSCRIPT( … )FBSCRIPT"; }

String fbScript = getFileBrowserScript();                  // ~49.7 KB malloc
httpd_resp_send_chunk(req, fbScript.c_str(), fbScript.length());

// after
inline const char* getFileBrowserScript() { return R"FBSCRIPT( … )FBSCRIPT"; }

streamChunkC(req, getFileBrowserScript());                 // zero allocation
```

**Exactly 5 call sites**, all of which use only `.c_str()`, so nothing depends
on the `String` type:
`WebPage_Files.h:10` · `WebPage_Logging.h:10` · `WebPage_CLI.h:95` ·
`WebPage_Maps.h:21` · `WebPage_ESPNow.h:3771`.

`WebPage_ESPNow.h:3771` already passes `HTTPD_RESP_USE_STRLEN`, proving the
length is discarded and that a 49 KB single-chunk send works. No embedded NULs
in a raw literal, so `strlen()` == `String::length()`. **No blockers.**

**Unit correction:** 49,630 is **heap per request**, not flash. The flash delta
is roughly **−150 B** (five String ctor/dtor sequences); the literal is
COMDAT/mergeable either way. The argument stands on its own: under heap pressure
this single contiguous internal-DRAM request is large enough to fail where the
streamed blocks would have succeeded, turning a low-memory condition into a
blank page on exactly the pages most likely to be open during a long session.

---

### P17 — Split `getBondFsScript()` out of `getFileBrowserScript()` — **CONFIRMED**

`WebPage_CLI.h:95` streams all **49,650 B** solely to obtain `window.BondFs`
(**13,753 B**, `WebServer_Utils.h:972-1217`) for the bonded-CLI toggle. Grep
proves the CLI page touches only `BondFs.checkAvailable` (`:243`) and
`BondFs.exec` (`:280`) — zero references to `FileBrowser`, `createFileExplorer`,
`createFileManager` or `hwUploadFile`. **35,897 B of wire per CLI page load**
is file-manager rendering the page never uses, on a server whose own comment
(`WebPage_CLI.h:169-172`) warns about socket contention.

Conversely Settings hand-rolls `BondFs.checkAvailable`'s `/api/bond/status`
probe **twice** (`WebPage_Settings.h:3676` and `:3688`) because it cannot reach
`BondFs`, and `WebPage_Bond.cpp:739` re-implements `BondFs.exec` for the same
reason. `WebPage_Logging.h:1544` has already adopted `BondFs`.

**Blocker — the split must be three-way, not two.** `BondFs.renderExplorer`
hard-depends on `window.FileBrowser`, calling `FileBrowser.breadcrumbHtml`,
`.rowHtml`, `.formatSize`, `.iconName` and `.iconFallback` at
`WebServer_Utils.h:1147, 1186, 1192, 1193, 1194`. Emitting only lines 972-1217
would break `renderExplorer` for `WebPage_Files.h:225/228/235`,
`WebPage_Logging.h:1587/1590/1593` and `WebPage_ESPNow.h:1623`. Split into
**(a)** FileBrowser render core, **(b)** BondFs transport core
(`WebServer_Utils.h:972-1116`, 8,575 B), **(c)** explorer/manager/upload markup.
CLI needs only (b) — which would save **41,075 B**, but the conservative 35,897
figure is the one quoted here.

**Two more blockers:** `hw.bondStatus()` is not a drop-in —
`BondFs.checkAvailable` (`WebServer_Utils.h:989-994`) tests
`d.bonded===true && d.role===1 && d.peerMac`, while `WebPage_Settings.h:3688-3691`
adds a fourth guard `if (d.peerMac === '00:00:00:00:00:00') return;`. Fold the
zero-MAC guard in. And `WebServer_Utils.h:984`'s `esc()` is a **JS-string**
escaper used to build inline `onclick` attributes in `renderExplorer` — keep it
private to that bundle and do not merge it toward `hw._esc` (see P18).

Prerequisite for P8 and P35.

---

### P2 — Delete dead and unreachable JavaScript — **PARTIAL** (33,000 B, down from 48,000)

**Nine of fourteen cited families hold up. Three were filed as "can never
execute" and in fact execute.** Two more cited lines contain live code with no
dead-code case at all.

**Confirmed dead — delete:**

| Site | What | Evidence |
|---|---|---|
| `WebPage_ESPNow.h:2157`, `:2920-2995`, `:3060-3121` | legacy message/remote/file UI | all 18 bound DOM ids resolve to nothing under a repo-wide scan including the C++-escaped `id=\"x\"` form |
| `WebPage_Maps.cpp:312` | `handleWaypointsPage` | declared `WebPage_Maps.h:2952`, defined `:312`, **never** passed to `httpd_register_uri_handler`; only `/api/waypoints` is registered (`:516-517`) |
| `i2csensor_bno055_web.h:85` | `DeviceRotationViz` | constructed only inside `initDeviceVisualization` (`:207-210`), which has zero callers |
| `i2csensor_sths34pf80_web.h:38` | `getPresenceWebCard()` | zero callers (1.0 KB, not the claimed 1.9 KB) |
| `WebPage_LoginSuccess.h:14` | `streamLoginSuccessContent` | zero callers; only the `#include` at `HardwareOne.cpp:51` |
| `WebPage_Settings.h:2704` | SETPART3 IIFE | defines `getInt`/`getStr`/`getBool` and uses none |
| `WebPage_Settings.h:2439` | first `window.onload` | clobbered by the second at `:2542` in the same IIFE |
| `WebPage_Login.h:66`, `WebPage_LoginRequired.h:40` | `revokeMsg` handlers | sessionStorage key `revokeMsg` is read at both, **written nowhere** in the repo (0.9 KB, not 1.4 KB) |

The Settings finding is **better than advertised**: the clobbered `onload` was
`fetchBuildConfig`'s only caller, so `window.__buildConfig` (declared `:2562`,
set `:2568`) has **zero readers** and the debug-option hiding it exists for has
never run. Record that as a known gap rather than letting the deletion bury it.

**REFUTED — these execute; do not delete:**

- `WebPage_Sensors.h:491` `updateStatusIndicators` **is** called, at `:399` inside
  `attachPageSSEListeners`, wired at `:686`. Worse: `applySensorStatus` (`:402`)
  touches **zero** elements whose id ends in `-status-indicator` — grepped, 0
  matches. `updateStatusIndicators` is the **sole writer** of every status dot's
  className. Deleting it silently freezes all 13 indicators with no error.
- `WebPage_Sensors.h:493` `checkAlreadyActiveSensors` **is** called at `:686`
  from `DOMContentLoaded` and is the only initial
  `Promise.all([/api/devices, /api/sensors/status])` on the page.
- `WebPage_Dashboard.h:371` `Dash.updateSensorStatus` **is** called at `:449`.
  Redundant with `createSensorCards`, but reachable and running.
- `WebPage_ESPNow.h:2880-2915` was counted inside the 12.2 KB but is **live**:
  `btn-set-friendly`, `friendly-name` and `btn-set-room` all have real HTML.
- `WebPage_CLI.h:228` and `i2csensor_mlx90640_web.h:45` — cited as dead sites;
  both contain live code (the `/api/cli` fetch handler, and the thermal
  colour-map generators). No dead-code case exists at either line.

**Three items are the ONLY implementation of a shipped feature — decide
deliberately:** `viewFiles()` on the dead `/waypoints` page is the sole UI for
the waypoint file attachments `/api/waypoints` still publishes
(`WebPage_Maps.cpp:573`, `:445`); the Edge Impulse overlay is one wrong id away
from working; `DeviceRotationViz` is one call away from working **but** its only
caller passes `yaw,pitch,roll` into a `(pitch,roll,yaw)` signature, so wiring it
up ships a visibly broken cube. `updateDeviceOrientation` (`i2csensor_bno055_web.h:211`)
**is** called from `:57-58` and must survive the deletion.

**One nuance the proposal missed:** `updateDeviceVisibility` is reachable but a
permanent no-op — its only caller `fetchDeviceRegistry` (`WebPage_Dashboard.h:449`)
invokes it *before* the `.then()` that runs `createSensorCards`, so every
`getElementById` returns null; later ticks skip `fetchDeviceRegistry` entirely.
Deletable (~1.2 KB) on effective-no-op grounds, not unreachability.

Also `System_EdgeImpulse_Web.h`: `_eiDrawBoxes` **is** called at `:324`, `:332`,
`:335`. Deleting the definitions requires deleting those three call sites too, or
the page throws `ReferenceError` on every inference tick.

---

### P3 — `hw.dbg(tag)` gated logger — **PARTIAL** (15,000 B, down from 24,000)

**814** `console.*` statements ship (656 in the audited scope, 639 inside the
extracted JS — the "803" figure included `WebPage_Games.h`).
`WebServer_Utils.cpp:715-722` already replaces `console.log/warn/debug` with
no-ops unless `gSettings.webConsoleDebug` is set, handing the originals back only
via `window.__restoreConsole`. So most of these are paid for in flash and
discarded at runtime.

The gated-logger pattern has now been written **four** times and exposed zero:
`WebServer_Utils.cpp:762` (`dbg`/`log` behind `localStorage.hwDebugTheme`),
`WebServer_Utils.h:239-241` (`dbgIcons`/`logIcons`, declared *inside* `renderIcon`
so two closures are built per file row), `WebPage_Maps.h:2036-2038`
(`const WP_DEBUG = false` + `wpLog`/`wpWarn`), and `WebPage_Sensors.h:290`
(`debugLog`, permanently gated off).

**Confirmed sentinel blocks:** `WebPage_ESPNow.h:564-831` carries **53** chunk
markers totalling **exactly 3,378 B** (the proposal's figure, exact).
`WebPage_Dashboard.h:368` — 34 sentinels, 2,210 B. `WebPage_Logging.h:400` — 39
sentinels, 2,506 B. `WebPage_Sensors.h:192`, `WebPage_Automations.h:418`,
`WebPage_CLI.h:112` all confirmed at the cited line.

**Corrected figures:** ESPNow 7,149 B (claimed 7,961). Automations 974 B
(claimed 1,709 — 75% high). Logging 5,113 B (claimed ~4,000 — *understated*).
`WebPage_ESPNow.h:604` was mis-cited: that line is
`window.__meshUnpaired = window.__meshUnpaired || [];`; the markers are at
`:564`, `:599`, `:601`, `:783`, `:785`, `:804`, `:805`, `:831`.

**Why 15,000 and not 24,000:** the proposal's own plan is *delete the sentinels,
convert the rest*. Measured: 195 literal-string sentinel calls = **11,222 B**
deletable outright; converting the remaining ~444 to a 2-char `d(` saves ~4,400 B
gross, ~4,000 B net after the definition and per-page tag. The 30,918 B figure is
only reachable if the messages are deleted rather than converted, which is not
what is proposed.

**Design-breaking blocker.** Because `WebServer_Utils.cpp:715-722` no-ops
`console.log`, an `hw.dbg` implemented as `if (tagMatches) console.log(…)` is
**silently dead in exactly the configuration its localStorage escape hatch is
meant to serve**. The stub must export the saved `L`/`W`/`D` to `hw`.

**Placement blocker.** `hw.dbg` must go in the unconditional block
(`WebServer_Utils.cpp:773/774`), not the `!isPublic` theme block at `:762` —
`WebPage_LoginSuccess.h` has 10 console calls and `WebPage_LoginRequired.h` 7.

**Do not simply delete the sentinels.** They exist to bisect which chunk failed
to parse. `tools/webui/tests/test_embedded_js_syntax.py`'s own docstring states
it covers 0% of JS assembled from ordinary concatenated C++ literals — which is
precisely where the 34 Dashboard and 33 Sensors sentinels live. **Convert those,
delete the rest.**

Retiring `debugLog` must also delete its two orphan call sites
(`i2csensor_vl53l4cx_web.h:41`, `i2csensor_mlx90640_web.h:57`), which cannot see
its definition and would throw `ReferenceError` on any other page.

---

### P4 — Adopt the existing `hw` DOM layer — **CONFIRMED**

The most honest arithmetic in the proposal set; every count re-derived on the
nose. Twelve pages independently re-declared the same one-liners:

| Local helper | Site | Re-implements |
|---|---|---|
| `Dash.setText` | `WebPage_Dashboard.h:369` | `hw.setText` verbatim |
| `Dash.showHideCard` | `WebPage_Dashboard.h:448` | `hw.toggle` (69 `Dash.*` call sites) |
| `setVis` | `WebPage_Sensors.h:194` | `hw.toggle` — and already calls `hw._ge` internally |
| `setStatus` | `WebPage_Speech.h:273` | `hw.setText` |
| `getOutputEl`/`setOutputText` | `WebPage_Bond.cpp:753` | `hw._ge`/`hw.setText` |
| `window.$` | `WebPage_Settings.h:2430` | `hw._ge` — 0 call sites before line 2430, 50 after |
| `el`/`setText` | `WebPage_Bluetooth.h:477` | `hw._ge`/`hw.setText` (39 sites) |
| `_on(id,evt,fn)` | `WebPage_ESPNow.h:2757` | *(a missing verb — see blocker)* |
| inline `s()`/`el()` | `System_Camera_DVP_Web.h:213`, `i2csensor_mlx90640_web.h:68`, `i2csensor_rda5807_web.h:48` | re-allocated per poll tick |

**The best evidence is a page that worked around the gap.** `WebPage_Battery.cpp`
adopted `hw._ge`, `hw.setText` and `hw.toggle` everywhere — and hand-writes
`live.style.display='flex'` at `:81`, because `#bat-live` is declared at `:48`
with an **inline** `style='display:flex;…'` and has no CSS rule anywhere.
`hw.show` sets `display:''`, which would clear the inline style and collapse the
flex row. Same at `WebPage_MQTT.cpp:346`/`:352` (`inline-block`).

**Blockers:**
1. `hw.show` **must take an explicit display argument** before Battery, MQTT or
   Speech can adopt it.
2. **Adoption is byte-negative at 89 sites.** `el(` (39 sites, Bluetooth) and
   `$(` (50 sites, Settings) are *shorter* than `hw._ge` — converting adds ~4 B
   each, ~356 B. Have those aliases delegate to `hw._ge` instead, or leave them.
3. `hw.on(el,evt,fn)` takes an **element**; ESPNow's `_on` takes an **id**. So
   `_on` is a missing verb, not a redundancy. Widen `hw.on` to accept `idOrEl`
   (safe — `hw._ge` passes non-strings through).
4. `hw.setText(x, undefined)` renders the string `"undefined"`; Bluetooth's
   `setText` does `v||''`. Add the guard or audit all 39 sites.
5. **`.vis-gone` is `display:none!important`** (`WebServer_Utils.h:1438`), so
   `hw.show`/`hw.toggle` silently no-op on anything hidden by that class — which
   is every group on the Automations page. See P9.

`hw.flip` (toggle from current state) is the verb behind 6 hand-rolled
`display==='none'?'block':'none'` ternaries. `hw.val(x[,v])` covers 105 sites
with no helper at all.

---

### P13 — `SchemaPanel` as the ONE settings renderer — **PARTIAL** (12,000 B, down from 24,000)

**Every behavioural claim is true, and the headline finding is the strongest
single item in the audit.**

`WebPage_Settings.h:2647` emits `bluetoothrequireauth`. A repo-wide grep returns
**exactly one hit — that line.** The registered command is `blerequireauth`
(`Bluetooth.cpp:2661`); `bluetoothRequireAuth` at `Bluetooth.cpp:2693` is a
settings-registry `jsonKey` whose `cmdKey` field points back at `"blerequireauth"`.

The failure is then swallowed at `WebPage_Settings.h:2649`:

```js
// WebPage_Settings.h:2647-2650  — the reply is never inspected
cmds.push('bluetoothrequireauth ' + (ble.checked ? '1' : '0'));   // no such verb
…
Promise.all(cmds.map(function(cmd) { return postSettingsCli(cmd); }))
  .then(function(){ refreshSettings(); })   // discards every result string
```

**The Bluetooth "Require Authentication" security toggle has never worked, and
the "Unknown command" reply is discarded.** A schema-driven panel reads `cmdKey`
from the firmware, so this typo is not expressible.

Three complete `SettingEntry` → HTML renderers exist, and they have drifted:

| Capability | `SchemaPanel` `:331/:368/:427` | `renderNetworkInput` `:725` | sensors `renderInput` `:1026` |
|---|---|---|---|
| `entry.readOnly` | **yes** (`:376`) | no | no |
| `bitmask:` options dialect | **yes** (`:383`) | **no** (`:734`) | yes (`:1031`) |
| Theme border | `var(--border)` | hardcoded `#ddd` ×4 | hardcoded `#ddd` ×5 |

`readOnly` appears **exactly once** in the whole of `WebPage_Settings.h` — that
one line. The firmware really does emit it (`System_Settings.cpp:3324`, `:3420`).
A `readOnly` entry added to any of the ~20 modules the other two cover becomes
editable and then emits its dotted `jsonKey` as a bogus CLI verb, via the
fallback at `WebPage_Settings.h:953`. A bitmask setting added to
`mqtt`/`http`/`bluetooth`/`espnow` renders as a dropdown whose options are the
literal tokens `0x1|Thermal` — even though `System_Settings.h:1365-1372`
documents the dialect as universal.

**Corrected bytes.** Directly deletable: `renderNetworkInput` 2,989 B +
`saveNetworkSettings` 1,456 B + sensors `renderInput` 6,623 B +
`saveDynamicSettings` 2,215 B = **13,283 B**. But `renderNetworkModule`
(`:771-929`, 8,951 B) is mostly card chrome that migrates into *options* rather
than vanishing, and `SchemaPanel` must **grow** ~2–3 KB. Net **~12,000 B**.

**Blockers:**
- `SchemaPanel` renders no **password** fields. Both copies emit
  `<input type="password">` with a blank-means-unchanged contract enforced in the
  savers (`:942`). Absorb both halves or WiFi/MQTT credentials become unsettable.
- `SchemaPanel` has no **`disabled`** concept. Both copies emit `disAttr` +
  `grayStyle`; `renderNetworkModule` also greys a whole card on `isDisconnected`.
- **Id prefixes are load-bearing and different**: `net-` vs `dyn-` vs
  caller-supplied. `saveNetworkSettings` selects `[id^="net-"]:not([disabled])`
  and reconstructs the dotted key by string surgery; `window._isChanged` and
  `window._snapshotContainer` key off the same ids. Change a prefix and
  change-detection silently breaks, so saves start sending unchanged values.
- **Behaviour-changing:** fixing the typo turns a dead toggle into a live,
  **superadmin-gated** one (`Bluetooth.cpp:2661`, `requiresSuperAdmin=true`).
  Non-superadmins who saw a no-op will start getting a refusal.
- Bring `sendSequential` (`:117`) along — it is the ONLY implementation of the
  `beginwrite`/…/`savesettings` batching protocol (`System_Settings.cpp:266-267`;
  the proposal's `:261` cite was 5 lines off), and `saveAuthSettings` already
  broke it by firing three unbatched parallel writes.

**Stage this module by module. Risk high is correct.**

---

### P6 — `hw.cli()` — a CLI transport that preserves the body — **CONFIRMED**

```js
// WebServer_Utils.cpp:773 — the body is thrown away on ANY non-2xx
hw.postFormText = function(u,form,o){
  return hw.postForm(u,form,o).then(function(r){
    if(!r.ok) throw new Error('HTTP '+r.status);   // <-- the real message is in r.text()
    return r.text();
  });
};
```

`handleCliCommand` (`WebServer_Server.cpp:3646-3656`) deliberately sets **401**
when the session died, **403** when the output starts with
`Error: Admin access required`, and **400** when the command failed or the output
starts with `Empty command`/`Unknown command` — then sends the real message as
the body via `httpd_resp_send(redactedOut)`.

So at `WebPage_CLI.h:223-225`, typing an unknown command into the web terminal
prints **`Error: HTTP 400`** instead of `Unknown command: foo`. Verified end to
end. Guests see `Error: HTTP 403` instead of `Error: Admin access required`.

**112** hand-rolled `/api/cli` sites (ESPNow **53**, Logging **13** — both exact).
Three sub-contracts each invented by one page and reachable by no other:

- `capture:'1'` — only `WebPage_CLI.h:223` and `WebPage_LLM.h:823`. It maps to
  `ctx.captureOutput` (`System_CommandTypes.h:108`), which captures
  `broadcastOutput` into the HTTP response. `WebPage_Speech.h:269` omits it and
  therefore **silently loses all broadcast text**.
- `interactive:'1'` — CLI only.
- `validate:'1'` — `WebPage_Automations.h:1087`, the only occurrence in the tree.
  Automations invented the dry-run and nobody else can reach it.

```js
// proposed
hw.cli = function(cmd, opts) {           // opts: {capture=true, interactive=false,
                                         //        validate=false, target='local'|'bonded'}
  // → Promise<{ok, status, text}>       // body preserved on 400/403; 429 handled (P33)
};
```

Absorb `postSettingsCli` (`WebPage_Settings.h:60`, the only target-aware client,
collapsing `/api/cli` and `/api/bond/exec` into one plain-text contract) as the
`target` option, and **`bind()`/`sendCmd()` from `WebPage_Sensors.h:227`/`:283`,
which 13 module files call as if they were platform API** while they are defined
only on the Sensors page — those modules currently cannot be included anywhere else.

**Blocker:** `/api/cli` is globally rate-limited at
`WebServer_Server.cpp:3498-3507` — a **process-wide** `static unsigned long
lastCmdTime` with a 50 ms floor returning **429 with a JSON body**, where every
caller expects text. Making the call site cheaper will surface more 429s.
`hw.cli` must absorb Bluetooth's queue and special-case 429 (see P33) or the fix
trades one confusing error for another.

**Behaviour-changing on ~112 paths — that is the point, but review the 403 path
against the guest-view-only surface.** Do not change `postFormText`'s own
semantics; other endpoints use it too.

---

### P22 — `hw.sse` — listeners that survive a reconnect — **CONFIRMED**

*Fixes a confirmed live bug, not just duplication.*

Three lifecycles fight over `window.__es`:

- `sseNotify` (`WebServer_Utils.cpp:828-843`) auto-connects on every
  authenticated page, owns `__es`, and on error **closes, nulls, and 10 s later
  builds a brand-new EventSource carrying only its own `notification` listener.**
- `WebPage_Dashboard.h:451` `createSSEIfNeeded` is a second owner that also nulls
  `__es` and **never retries**.
- `WebPage_Dashboard.h:452` and `WebPage_Sensors.h:399` attach
  `sensor-status`/`system` listeners once, at `DOMContentLoaded`, to whatever
  `__es` existed then. **Nothing re-attaches.**

Verified consequence: `WebPage_Dashboard.h` has exactly two `setInterval` calls —
a 500 ms DOM-patch retry (`:134`) and a 15 s signed-in-users refresh (`:812`).
There is **no polling fallback** for `sensor-status` or `system`. After the first
SSE hiccup the Dashboard is permanently frozen on stale data with no recovery
short of reload, which reads as a firmware hang. The Sensors page survives only
because `WebPage_Sensors.h:281`'s 1 Hz `hw.pollJSON` masks the identical defect.

`WebPage_Dashboard.h:453` `setupSensorSSE` — grepped the whole tree: **1
occurrence, its own definition.** Confirmed dead.

**Blockers:**
- A **fourth owner** the proposal missed: `WebPage_Bond.cpp:943` closes
  `window.__es` and nulls it on `beforeunload`. Migrate it in the same pass.
- A **duplicate-listener bug in the code being replaced**: in `sseNotify`, the
  `es.addEventListener('notification',…)` call sits **outside** the
  `if(!es||es.readyState===2)` block, so it re-registers against an existing
  socket. Latent today; guard registration by identity.
- **Ordering is load-bearing by accident:** `sseNotify` registers its
  `DOMContentLoaded` handler from the HEAD (`WebServer_Utils.cpp:842`), which is
  the only reason `WebPage_Sensors.h:686`'s `if(window.__es)` guard ever passes.
  Lazy-connect on first `hw.sse()` call removes the dependency.
- The reconnect handler should also **re-fetch the snapshot** —
  `WebPage_Dashboard.h:812` bootstraps once, so a reconnected page would come
  back live but showing whatever it had when the socket died.

Measured: `Dashboard:451` 1,160 B + `:452` 1,381 B + `:453` 709 B +
`Sensors:399` 684 B = 3,934 B, against a ~700–900 B manager. **2,500 B is fair
to conservative.**

---

### P7 — CSS token and class consolidation — **CONFIRMED** (4,000 B, down from 10,000)

**Four defects, one of which is a one-line fix that makes 15 currently-colourless
UI states visible.**

**(1) `var(--warning)` is used 15 times and declared nowhere.** Confirmed by
grep: 15 uses, 0 `--warning:` declarations. `streamCommonCSS` declares only
`--warning-bg/-fg/-border/-accent`. Distribution corrected: **ESPNow ×10**
(`:654, :655, :667, :725, :726, :729, :730, :759` + 2), Speech ×3 (`:24, :638, :646`),
**Bluetooth ×1** (`:23`), MQTT ×1 (`:122`) — not "half Bluetooth".

Wording correction: `color: var(--warning)` with an undefined custom property is
invalid at computed-value time, so `color` **inherits** rather than rendering
"with no colour at all". The text is visible, just wrong — which is why nobody
reported it.

**`--panel-border` has the same problem and the proposal missed it.**
`WebPage_Bond.cpp` uses `border-top:1px solid var(--panel-border)` at **10** sites
and `--panel-border` is declared **nowhere**. Unlike `color`, an invalid
`border-top` shorthand falls back to the initial value, so **all 10 Bond-page
dividers are currently invisible.** Fix both in the same one-line pass.

**(2) The `:root` / `html[data-theme=light]` duplication — THE FIX DIRECTION IN
THE PROPOSAL IS BACKWARDS AND WOULD BREAK EVERY PAGE.** They are not twins:
`html[data-theme=light]` (`WebServer_Utils.h:1299-1333`) declares **32** tokens;
`:root` (`:1272-1298`) declares only **24**. The light block *uniquely* owns
`--warning-bg/-fg/-border/-accent` and `--info-bg/-fg/-border/-accent`.
Deleting `:root` as the signature says would leave 24 tokens defined only under an
explicit `data-theme`. **Correct edit: keep `:root`, move the 8 warning/info
tokens into it, delete the `html[data-theme=light]` selector** — safe only
because `WebServer_Utils.cpp:667-674` always emits `data-theme="light"|"dark"`
server-side. ~673 B of the light block is byte-identical to `:root`.

**(3) Dead classes — understated.** `.panel-light`, `.table-striped`,
`.alert-info`, `.text-accent`, `.text-primary` each have exactly one occurrence:
their own definition. **Eight** of twelve `.space-*` utilities are unreferenced
(not seven). `.btn-primary,.btn-secondary{ }` at `WebServer_Utils.h:1497` is
verbatim empty despite 11 markup sites (6 + 5) expecting styling, including the
public login page (`WebPage_Login.h:60`, `WebServer_Server.cpp:4076/4227`).

`.btn` hard-bakes `min-height:40px` (`:1489`) and `.btn-small` (`:1498`) does not
reset it — so no button in the app can be shorter than 40px. The consequence is
visible: `WebPage_Automations.h` works around it **6 times** with a 122-char
`height:32px;line-height:32px;…` override (`:65`, `:562` + 4).

**(4) Duplicated blocks.** `.bt-header*` (`WebPage_Bluetooth.h:46-52`) and
`.en-*` (`WebPage_ESPNow.h:21-27`) are byte-identical across all seven rules
except one `flex-wrap`. `.sensor-card` (`WebPage_Sensors.h:98`) and `.sr-card`
(`WebPage_Speech.h:52`) are the same rule twice. Plus repeated inline literals:
Settings' 79-char auto-fit grid ×12 (~640 B), Automations' button override ×6
(~640 B), Bond's divider ×10 (~280 B).

**Refuted:** `.bt-card` is **not** "90% of `.panel`" — it is
`{background,border,radius:10px,overflow:hidden}` with **no padding and no
shadow**; folding it into `.panel` adds 1.25rem of padding to every Bluetooth
card. `.remote-card` **does not exist anywhere in the tree.**

**Blocker:** consolidating `.sr-info` (`WebPage_Speech.h:65`) into `.alert-info`
is a deliberate visual change — `.sr-info` is cornflower `rgba(100,149,237,…)`,
`.alert-info` is `#d1ecf1`/`#0c5460`.

Defensible tally: light-block dedup 673 + dead classes 586 + bt/en header ~800 +
card dedup ~150 + Settings grid ~640 + Automations button ~640 + Bond divider
~280 ≈ **3,770**, rounded to 4,000. The claimed 10,000 is not supportable.

---

### P19 — Escape at the ~25 unguarded `innerHTML` sinks — **CONFIRMED** (0 bytes; ranked on security)

Escaping is applied inconsistently *within single functions*, and the places
where the string is not firmware-controlled are precisely the places with no
escaper in scope.

**The clearest example, `WebPage_Settings.h:2801`** — one line, one variable,
one protected copy and one not:

```js
// the machine-readable copy IS escaped …
'… data-ssid="' + esc + '" …'                    // esc = encodeURIComponent(ssid)
// … the copy the human sees is NOT
+ '<div style="font-weight:bold">' + ssid + ' ' + badge + '</div>'
```

The SSID arrives over the air from any nearby AP.

**Two sinks are worse than the proposal claims:**

- `WebPage_Sensors.h:573`/`:577` — `fmt()` **escapes the value** (via the 3-char
  `esc` at `:562`) but the **object key** `ik`/`sk` is concatenated completely
  raw, then the whole string lands in `el.innerHTML` at `:580`. The code
  demonstrably knows escaping is needed and never applies it to the key. Keys come
  from a remote ESP-NOW peer's sensor JSON, polled into the DOM **once a second**.
- `WebServer_Utils.h:343` — `FileBrowser.rowHtml` interpolates `o.name` and
  `o.sizeInfo` unescaped, **and at `:342` interpolates `o.clickExpr` directly into
  an `onclick` attribute.** No HTML-entity escaper can fix that one.

**Ship the Bond fixes first and on their own.** `WebPage_Bond.cpp:218` and `:350`
interpolate `peerName` and `capabilities.features` — received over ESP-NOW from
the bonded device — straight into `innerHTML`, while the same file's
`refreshBondDevices` uses the safe `createElement`+`textContent` path.
**The bond channel is the auth/RCE channel.** Reviewable and revertable
independently.

Other confirmed sinks: `WebPage_ESPNow.h:687/739/762` and `:1817-1842` (device
names, `/api/espnow/metadata` friendlyName/room/zone/tags),
`WebPage_Automations.h:1050` (names arriving from an arbitrary URL via
`downloadFromGitHub`), `WebPage_Speech.h:551` (raw filenames off removable SD),
`WebPage_Maps.h:2666` (`loadMapFeatures` — neither `cleanName` nor `escapeHtml`),
`WebPage_Battery.cpp:109`.

**One sink the proposal missed:** `System_Microphone_Web.h:173-175` puts a raw SD
filename into `data-name="…"` **and** into a `src="…"` URL with neither escaping
nor `encodeURIComponent`, while its camera twin (`System_Camera_DVP_Web.h:178`)
does encode.

**Correction to the model-page claim:** `WebPage_Bluetooth.h` is clean because it
calls `esc()` on every field (`:729-730`), **not** because it uses `textContent`.
That distinction changes the prescription other pages should copy.

**Blockers:** P18 is a hard prerequisite, and the ordering matters — several sinks
are in **attribute** position, so fixing them with the quote-unsafe `hw._esc`
leaves them exploitable while appearing fixed. `WebServer_Utils.h:332-343` is
reachable from the standalone file-viewer shell where `hw.esc` is `undefined`, so
FileBrowser must carry its own. And `WebPage_Sensors.h:573/577` is a 1 Hz hot
path — escape once at the `addRow` boundary, not inside the map callbacks.

---

### P18 — `hw.esc()` — one quote-safe escaper — **CONFIRMED**

Eight functions, four names, three semantics — all read directly:

| Site | Name | Escapes | Mechanism |
|---|---|---|---|
| `WebServer_Utils.cpp:825` | `hw._esc` | `& < >` | `textContent` round-trip |
| `WebPage_Bluetooth.h:698` | `esc` | `& < > " '` | regex |
| `WebPage_Maps.h:2041` | `escapeHtml` | `& < > " '` | regex, null-guarded |
| `WebPage_Maps.cpp:338` | `escapeHtml` | `& < > " '` | verbatim clone |
| `WebPage_Settings.h:3037` | `escapeUserHtml` | `& < > " '` | regex |
| `WebPage_Settings.h:1946` | `esc` | `& " < >` | regex + `(s\|\|'')` bug |
| `WebPage_ESPNow.h:3226` | `escHtml` | `& < > "` | chained replaces |
| `WebPage_Logging.h:1469` | `escapeHtml` | `& < >` | `textContent` |
| `WebPage_Sensors.h:562` | `esc` | `< > &` | regex |
| `WebServer_Utils.h:984` | `esc` | `\` and `'` | **JS-string escaper, not HTML** |

**The crux, verified verbatim at `WebPage_ESPNow.h:1354` and `:1897`:**

```js
var esc = (typeof hw !== 'undefined' && hw._esc) ? hw._esc
        : function(s){ /* stricter 4-char regex */ };
```

The page **prefers the weakest escaper available** and only falls back to its own
stricter one when `hw` is absent. Nobody reading either site alone would notice.

`hw._esc` is defined at `WebServer_Utils.cpp:825`, comfortably inside the
`!isPublic` guard that opens at `:784` — so the codebase's only shared escaper is
**missing on exactly the pages that handle untrusted credentials.**

`WebPage_Settings.h:1946`'s `(s||'')` guard turns the number `0` and `false` into
`''` — visible in the debug-flag tooltips that function feeds.

**Promote the REGEX form, never `hw._esc`.** A blanket "use `hw._esc` everywhere"
sweep would **regress** `WebPage_Maps.h:2779`, which correctly interpolates
`escapeHtml(p)` into `title="…"`, and `WebPage_Bluetooth.h:729-730`.

**Blockers:** move the definition out of the `!isPublic` block — but note
`hw.notify` (`WebServer_Utils.cpp:816`) *calls* `hw._esc`, so the relocation must
keep it defined first. Keeping `hw._esc` as an alias for a stricter `hw.esc`
silently changes behaviour at every existing caller (escaping more is safe for
rendering, but not for anything that compares or stores the result) — audit the
3 non-notify callers. And **rename `WebServer_Utils.h:984` to `escJs()`** in the
same commit; folding it into `hw.esc` would emit `&#39;` inside generated
JavaScript string literals and break the generated code.

---

### P28 — `hw.cliFailed` / `hw.cliPending` — **PARTIAL** (800 B, down from 1,200)

**33 live predicate sites in at least nine mutually incompatible shapes**, while
the correct anchored one is trapped inside `hw.cliConfirm`:

```js
// WebServer_Utils.cpp:773 — correct, and reachable from exactly 2 call sites
var failed = !result || /^(?:Error|Failed|Cancelled)(?:\s|:|$)/i.test(result);
```

Those two sites are `WebServer_Utils.h:592` and `WebPage_Settings.h:3381`.

Live shapes: `t.indexOf('Error')>=0` unguarded (`WebPage_Settings.h:3272`, also
`:3297/:3322/:3354`); guarded `t && t.indexOf('Error')>=0`;
`indexOf('Error')!==0` (`WebPage_ESPNow.h:1215`); `startsWith('Error')`
(`WebServer_Utils.h:627`); `indexOf('Error')||indexOf('Failed')`
(`WebServer_Utils.h:579`); lowercase substring scan
(`WebPage_Bond.cpp:910` — exact); `toLowerCase().indexOf('error:')`
(`WebPage_Automations.h:1443/1485/1495/1677` — **×4, not ×3**); `sendSequential`'s
four-token scan (`WebPage_Settings.h:135-137`).

**ESPNow is worse than claimed — four incompatible readings in one file:**
`:1115` and `:1439` use `lower.indexOf('failed')||lower.indexOf('error')`; `:1215`
uses anchored-`!==0` plus a success sniff; `:1370` adds `'not initialized'`;
`:3022` uses `text.indexOf('Error')>=0`.

**Three cited sites do not survive:**
- `WebServer_Utils.h:393` is inside `renderBreadcrumb()` — **no predicate at all**.
  The real site is `WebServer_Utils.h:579`.
- `WebPage_Bluetooth.h:491` is `function updateToggle(btnId, enabled)` — a label
  setter. Bluetooth's two checks are at `:917` and `:926`.
- `WebPage_Logging.h:651` is **misclassified**: it is an *inverted success sniff*
  (`if (text.includes('SUCCESS') || text.includes('started'))`, else prefix
  `'Error: '`). A ninth and worse shape — any successful reply lacking those two
  words is reported as an error — but not the shape it was filed under.

**The sharpest correctness claim is REFUTED.** "The unguarded forms throw a
TypeError on an empty body, reported as `Error: Cannot read properties of
undefined`" **does not happen.** Every producer yields a string or throws:
`hw.postFormText` resolves `r.text()` (empty body → `''`), and `postSettingsCli`
(`WebPage_Settings.h:60`) returns `j.result || ''` in bonded mode. `''.indexOf(…)`
is `-1`. The guarded and unguarded forms are **behaviourally identical today** —
cosmetic inconsistency, not a live defect. That claim was doing most of the
persuasive work.

The `Error.log` and `0 errors` false-positive scenarios are **plausible but
unverified** — no command whose success output contains the substring was found.
Present the argument as incompatibility of nine shapes, not a demonstrated bug.

**Blockers:** the change flips **both** directions. `WebPage_Logging.h:651` and
`WebPage_ESPNow.h:1215` require an affirmative success token, so replacing them
with `!hw.cliFailed(t)` flips their default from assume-failure to assume-success.
Adding `Unknown command` to the alternation (which the live `cliConfirm` regex
does **not** contain) changes the two existing `cliConfirm` call sites — both on
destructive paths. And `hw.cliFailed('')` must be **failed** to match the existing
helper, while several sites treat an empty reply as success. **Per-site audit,
not a regex sweep.**

---

### P12 — `hw.setDot` / `hw.badge` — **CONFIRMED**

*Structurally the best-verified proposal in the set.* `siteCount: 80` matched an
independent count of `'status-indicator status-'` literals **exactly**.

**Two CSS families for one concept**, plus a third redundancy:
`.status-dot`/`.status-active`/`.status-inactive` (`WebServer_Utils.h:1473-1475`)
and `.status-indicator`/`-enabled`/`-disabled`/`-recording`/`-running`/`-wake`
(`:1480-1485`). `@keyframes pulse-fast` (`:1477`) and `@keyframes blink` (`:1478`)
have **byte-identical bodies** (`0%{opacity:1}50%{opacity:.3}100%{opacity:1}`).

Four JS implementations: `Dash.setIndicator` (`WebPage_Dashboard.h:371`, 16 call
sites, clobbers `className`), `setClass` (`WebPage_Sensors.h:226`, **with** the
change-guard), `setDot` (`WebPage_Sensors.h:587`, also guarded), and 12 fully
hand-inlined copies in `updateStatusIndicators` (`WebPage_Sensors.h:491`) without
the guard. Plus `System_Microphone_Web.h` ×6, `WebPage_MQTT.cpp` ×3,
`System_EdgeImpulse_Web.h` ×1.

`WebPage_ESPNow.h` separately inlines the same 110-byte dot span **exactly 11
times** (1,336 B) across four renderers, with **exactly two** `#28a745` drifts at
`:1948` and `:2551`.

**Keep the change-guard in the shared version** (`if(el.className!==c)`). It is
load-bearing beyond CPU: `WebPage_Sensors.h:491` rewrites `className` on 12
elements on every SSE `sensor-status` event while the page decodes thermal/JPEG
frames. It also unblocks any future CSS transition on the dot.

**Blockers:** the two families are **not interchangeable** — `.status-dot` is
12×12 with no margin and no animation; `.status-indicator` is 12×12 with
`margin-right:8px`, `flex:0 0 12px`, `box-sizing:content-box`,
`vertical-align:middle`, and animating state classes. Collapsing them moves every
`.status-dot` by 8px unless the shared class keeps both geometries.
`.status-recording` hardcodes `#e74c3c` and `.status-wake` uses
`var(--warning-accent,#ffc107)` — neither is a plain token; do not silently
retheme them. `hw.setDot`'s optional `title` is not decoration —
`System_Microphone_Web.h` sets `statusInd.title` alongside the className.

`renderSignalBars` (`WebPage_Bond.cpp:170`) is genuinely the **only** graphical
RSSI renderer (`'signal-bar'` appears in no other file) and belongs in this
family — but promoting it means promoting its CSS to the common sheet, which
costs bytes on every page.

**Corrected in-text figures:** the literal `'status-indicator status-'` appears
**80** times, not 49 (which is what `siteCount` already said);
`updateStatusIndicators` has **12** inlined copies, not 14; and
`WebPage_Dashboard.h` contains **zero** occurrences of `dBm` (ESPNow has 3,
Bluetooth 1).

---

### P10 — `hw.poll` — one poll manager — **CONFIRMED** (4,000 B, down from 5,900)

**Every "solved exactly once" guard is real**, each verified by tree-wide grep:

| Guard | Sole implementation |
|---|---|
| pause on `document.hidden` | `WebPage_LLM.h:519` / `:523` — **`visibilitychange` has exactly 1 hit repo-wide** |
| exponential backoff + give-up budget | `WebPage_LLM.h:629` (150/300/600/1200, `LOST_GIVEUP_MS`) |
| adaptive two-speed cadence | `WebPage_Bluetooth.h:1140` `pickNextDelay` |
| coalescing in-flight + pending | `WebPage_Automations.h:930` |
| both hidden **and** in-flight guards | `WebPage_ESPNow.h:2215-2217` |
| stop on 401 | `hw.pollJSON` (`WebServer_Utils.cpp:774`) — **3 call sites** |

**The codebase documents the cost in its own voice**, quoted accurately at
`WebPage_CLI.h:168-173`: the device HTTP server has only 5 sockets with LRU purge
on, so an always-on `/api/cli/logs` poll competes with `BondFs.exec` for the last
socket and gets an in-flight bonded request force-closed.

Real consequences: `WebPage_ESPNow.h:2546` fires `espnowmeshtopo` every 30 s with
no hidden guard, no in-flight guard and no 401 stop — a backgrounded tab floods
the mesh forever, including after logout. `WebPage_CLI.h:183`'s 500 ms poller
swallows every non-401 failure (`.catch(function(_){})` at `:182`) and hammers a
rebooted device at 2 req/s. `WebPage_MQTT.cpp:446` is a top-level `setInterval`
with no `clearInterval` anywhere in the file.

**THE decisive blocker: `hw.pollJSON` as it exists today is WEAKER than four of
the pollers it would replace.** No hidden check, no in-flight guard, no backoff,
JSON-only. "Adopt `hw.pollJSON`" applied to `WebPage_ESPNow.h:2215` deletes both
guards that file uniquely has; applied to `WebPage_Bluetooth.h:1140` it deletes
`pickNextDelay`. **Upgrade the manager to the proposed signature first.** Any
adoption before that is a straight regression. This is why the proposal is
correctly filed PROMOTE, not REDUNDANT.

**More blockers:** `hw.pollJSON`'s failure path routes through
`hw._auth401` → `location.href='/login'`, but `WebPage_CLI.h:180` deliberately
stops **quietly** on 401 — converting it would navigate away from a terminal that
may hold unsent input, and CLI polls **text**, so an `hw.pollText` sibling is
required first. `pauseWhenHidden=true` as a default would silently change the
three pages that already adopted `hw.pollJSON`
(`WebPage_Sensors.h:281`, `WebPage_Battery.cpp:94`, `WebPage_R1_Health.cpp:346`) —
default it **false** and opt in. And `WebPage_ESPNow.h:2546` is a **write**, so a
`maxErrors` give-up silently disables a feature whose button still says ON;
reconcile the button state.

**Corrections:** the "54 raw setInterval" figure only holds if you include
`WebPage_DarkRoom.h` (11), `WebPage_Games.h` (5) and the non-web `G2_Glasses.cpp`
(2) / `G2_Ring.cpp` (1). The real web figure is **35**. `WebPage_CLI.h:180` **has**
its own 401 stop (a second independent solution, not an instance of the problem),
and `WebPage_Bond.cpp:940` clears its interval on `beforeunload`.

---

### P8 — `hw.peerExec()` — one peer command collector — **PARTIAL** (6,000 B, down from 8,800)

**Four independent implementations, not ten sites.** The mature one is
`BondFs.exec` (`WebServer_Utils.h:1008`), and its own comment at `:1000-1007`
names the race a **shared** sequence watermark caused and states that a per-call
seq baseline is *"the only correct design"*.

`WebPage_Bond.cpp:724` then resurrects exactly that removed design:

```js
// WebPage_Bond.cpp:724 — page-global, seeded once by the IIFE at :726-737
var bondMsgSeq = 0;
…
// :788-789
'/api/espnow/messages?since=' + bondMsgSeq
```

The Bond deletion range `:723-847` measures **4,823 B** — the proposal's "~4.8 KB"
is exact, and it is free flash, because `BondFs` already ships and Bond need only
stream it.

`browseRemoteFiles` (`WebPage_ESPNow.h:1500`) re-derives the fix in a comment at
`:1541-1555` and owns the `' entries'` vs `'Total:'` doneMarker collision fix,
**which exists nowhere else in the tree** — so any future `BondFs.exec` caller
passing `'Total:'` re-introduces it. `fetchRemoteFile` (`:1700`) is a third
variant taking its baseline from `existing[existing.length-1].seq` (`:1726`) with
**no doneMarker at all**.

**Three cited sites are not this pattern:** `WebPage_ESPNow.h:1859`
`syncMetadata` polls `/api/espnow/metadata`, not the message stream, and has no
watermark. `WebPage_ESPNow.h:2698` `discoverTopology` re-issues a CLI command
every 2 s and string-matches. `WebServer_Utils.h:1077` `BondFs.pull` polls the
**local** filesystem. `WebPage_CLI.h:274` is a **consumer**, not an implementation.

**Blockers:**
- **Three incompatible message shapes.** `BondFs.exec` reads `m.seq`/`m.msg` and
  filters server-side via `&mac=`. `browseRemoteFiles` reads `m.seq||m.seqNum`,
  `m.message||m.msg||m.text`, `m.mac||m.from` and filters **client**-side on an
  uppercased MAC. Picking one changes which messages each caller sees.
- **Two transports with different auth models.** `BondFs.exec` POSTs
  `/api/bond/exec` over the bond session token with no credentials;
  `browseRemoteFiles`/`fetchRemoteFile` POST `/api/cli` with
  `espnowremote <mac> <user> <pass> …`, reading per-peer credentials out of DOM
  inputs. `hw.peerExec(cmd,{mac,…})` has nowhere to carry credentials.
- `fetchRemoteFile` has **no doneMarker by design** — it terminates on a content
  predicate over both success and failure strings and paints three status colours.
  The signature needs an extra predicate option.
- Bond's `execRemoteCmd` carries page-specific side effects that must become
  callbacks: it clears and restores the 5 s `refreshBond` interval
  (`:767-771`) because the page re-renders and detaches the output node, and it
  flashes the output border for 5 s.
- The Bond race is **reachable but narrower** than the two-pane Files case — the
  page has only one command input, so it takes deliberate double-firing.
- `browseRemoteFiles` fires `/api/cli`, globally rate-limited at 50 ms. Serialise
  (P33).

Defensible: the 4,823 B Bond deletion plus roughly 1,500 B of the
`browseRemoteFiles` envelope. The rest of that function is page-specific parsing.

---

### P11, P15, P20, P21, P23, P25, P29, P30 — confirmed, smaller

| ID | Verdict | What survives, in one line |
|---|---|---|
| **P11** `hw.status`/`hw.banner` | CONFIRMED 3,000 B | ESPNow hardcodes the Bootstrap triples at **13** sites and Bond at **5**, so those banners render dark-text-on-pale in dark theme; three named `setStatus` implementations disagree on whether error recolours (`WebServer_Utils.h:814` does, `WebPage_Speech.h:273` and `WebPage_Bluetooth.h:741` do not). **Blocker:** the triples also appear on the **public** Login/Register pages, so `hw.status` must live at `WebServer_Utils.cpp:773`, outside the `isPublic` guard; and the server-rendered sites in `WebServer_Server.cpp` need a **CSS class, not a JS helper**. `WebPage_Files.h:110` is **refuted** — it drives a progress bar, wrong concept. The "auto-hide exists in exactly two of seventy" claim is **false** (at least four: `WebPage_Bond.cpp:896` 3 s, `WebPage_Maps.h:582` 3 s). Depends on P7. |
| **P15** `hw.renderTable`/`renderList`/`emptyState` | CONFIRMED 3,000 B | `WebPage_Automations.h:948-955` re-inlines per-`th` padding and `border-collapse` that `streamCommonCSS` **already ships to the same page** at `WebServer_Utils.h:1418-1420`. `WebPage_Bluetooth.h:734` emits `onclick="btDisconnect()"` with **no argument** inside a per-client loop — with `maxConnections` up to 4, every Kick button does the same thing. The camera and mic recordings lists are the same feature written twice, diverging on escaping, URL encoding **and** confirm dialog. **Blockers:** `esc:true` as default would double-escape `WebPage_Maps.h:2779` and `WebPage_Bluetooth.h:729-730`, which are already correct; `WebPage_Battery.cpp:107-108`'s sticky header needs `border-collapse:separate`, which the shared rule overrides. Battery's per-cell style is **already hoisted into a var**, so it is runtime cost, not flash. |
| **P20** `hw.fmt` | CONFIRMED 2,000 B | Five byte formatters with three precisions and three top rungs; only `WebPage_LLM.h:461` scales to GB, and its source comment records that a flat KB figure printed `3590144KB`. `WebPage_Maps.h:2699` renders a 3 MB track as `3072.0 KB`. **The ms-vs-s collision is real:** `WebPage_R1_Health.cpp:270` `when()` takes **seconds**, `WebPage_Settings.h:2902` `formatMillisTimestamp` takes **milliseconds**. **Blockers:** `WebPage_Battery.cpp:74` `etaStr` takes **MINUTES** — a third unit the signature does not cover; `WebServer_Utils.h:278` `formatSize` **sniffs its input** and passes non-numeric remote strings through untouched, so a drop-in renders bonded directory sizes as NaN. **Refuted:** `unitToMs`/`msToUnit` **do not exist** — grep returns zero hits; the logic is inline at `WebPage_Automations.h:1241-1244` and `:1202-1216`. The `autoEdit` bug is **real but conditional**: both unit selects default to `ms` (`:112-127`), so the ×86.4M multiply only fires if the user changed a dropdown earlier in the session, because `createAutomation`'s reset (`:1455-1456`) clears values but not units. |
| **P21** file in/out family | CONFIRMED 2,500 B | **`WebPage_Speech.h:602` POSTs to `/api/upload`, which is registered nowhere** — only `/api/files/upload` exists (`WebServer_Server.cpp:5843`). The Upload Model button has never worked. **`WebPage_Logging.h:1621` does `window.location = res.localUrl`** — exactly what `WebPage_Files.h:262-266` documents in prose as wrong for text/JSON, in the same `BondFs.pull` callback shape 400 lines away. `hwUploadFile` (`WebServer_Utils.h:687`) `JSON.parse`s `xhr.responseText` with **no status check** (`:706-708`). Both ArrayBuffer sites (`WebPage_Maps.h:614`, `WebPage_AviPlayer.h:154`) are raw fetches with no 401 path. **Blocker, already correctly carved out:** `WebPage_Automations.h:1731` `downloadFromGitHub` is cross-origin and every `hw.fetch*` forces `credentials:'include'` — **keep it a bare fetch**. `hw.upload` cannot simply wrap `hwUploadFile`: that one base64s via `readAsDataURL` and posts urlencoded, while Speech builds multipart. Dance count is **6**, not 7; the "bad magic" symptom is **not reachable** because `WebPage_Maps.h:615` checks `resp.ok` first. |
| **P23** `hw.busy(btn,label)->restore()` | CONFIRMED 1,200 B | **`WebPage_LLM.h:813-838` verified line by line:** entry sets six properties (`:813-819`), the `.then` at `:823-825` restores exactly one (the label), and the full six-property restore sits entirely inside the `.catch` at `:828-837` behind a comment saying it is the error path. **After every successful Do: run the Run button stays disabled at opacity 0.6 and the command input stays disabled, until reload.** ESPNow invented the good version (`:336`/`:347`, `btn.dataset.defaultLabel`) then hand-rolled it twice more in the same file. A seventh variant found at `WebPage_Bond.cpp:885` exactly where predicted. **Refuted:** the green status span appears **3** times in `System_EdgeImpulse_Web.h`, not 7. **Blockers:** ESPNow's version is keyed by MAC and coupled to `window.__autoFetchState[mac]` — only the label/disabled half should move; `System_Camera_DVP_Web.h:513` and `WebPage_ESPNow.h:2020` deliberately hold a label for 2–3 s via `setTimeout`; `WebPage_Bond.cpp:883` never restores on success **by design** (it reloads). Returning a `restore()` closure is what makes the LLM bug unwritable — worth more than the bytes. |
| **P25** `hw.modal` | CONFIRMED 1,500 B | **Two modals on the same rendered page disagree**: `WebPage_Files.h:33` streams `streamAviPlayerModal` and `:35` emits `#editor-modal`. `#editor-modal` has **no** overlay `onclick` — the Close button is the only exit, and it holds unsaved textarea edits. `#avi-modal` closes on backdrop click (`WebPage_AviPlayer.h:180`). `WebPage_Dashboard.h:458` and `:481` are character-identical apart from the id. The shared `.modal-overlay` (`WebServer_Utils.h:1499`) has exactly **two** consumers, both on Settings. **`WebPage_Maps.h:1859-1866` vs `:2433-2443` implement dismiss-on-outside twice in one file** with four differences: `click` vs `pointerdown`, bubble vs capture, 100 ms vs 0 ms arming, explicit `removeEventListener` vs `{once:true}` self-re-arm. **Refuted:** `hw-dlg` does **not** have Escape — `WebServer_Utils.h:1540` attaches keydown to the **input**, so only `hwPrompt` closes on Escape. Escape dismissal is effectively absent app-wide, so a shared shell **adds** behaviour. **Blocker:** that new Escape would fire inside the Files editor over unsaved edits. |
| **P29** retire native `confirm()` | CONFIRMED 800 B | **The count is exactly right — 10 sites**, and more complete than the supplied ground truth (which omitted `WebServer_Utils.h:576`). **The availability check passes:** all ten are on `isPublic=false` pages; only `/login` and `/register` are public and neither has a `confirm()`. The standout divergence is real: `WebServer_Utils.h:576` uses native `confirm()` in the **folder**-delete branch while `:592` uses `hw.cliConfirm` in the **file**-delete branch — 16 lines apart, same handler, same file. `hw.cliConfirm` hardcodes `interactive:true` and appends a `'yes'` second command (`WebServer_Utils.cpp:773`), which fully explains the ~15 open-codings. **Blockers:** every one of the ten enclosing functions must become async (`WebServer_Utils.h:576` is the awkward one — three other synchronous `return` branches); two sites live in escaped `"…\n"` C-string chains (`System_Camera_DVP_Web.h:191`, `WebPage_Maps.cpp:418`) where a hand-escaping slip is never caught by the compiler; **add the `interactive:false` branch first** or the stray `'yes'` executes as a second command. **Refuted:** `WebPage_CLI.h` has **zero** `confirm()` calls — the stream-tick-stall hazard belongs to `System_Camera_DVP_Web.h:191`, whose confirm shares a script with the `setTimeout` loop at `:281-289`. |
| **P30** `hw.fillSelect` | CONFIRMED 1,200 B | **13** `createElement('option')` loops across 8 files (not 17 across 9). Only two preserve selection across a poll-driven rebuild (`WebPage_LLM.h:449`/`:476`, which had to add a signature diff) plus Bond on manual refresh (`:854`/`:874`). **The strongest finding is fully confirmed:** `WebPage_Settings.h:3452` `refreshSyncPeers` and `:3502` `refreshSyncPeersFor` are the same 25-line function twice, differing only in a target id and a cosmetically-different character class — both scrape human-readable output with a two-leading-spaces heuristic, and **neither has a `.catch`**, so a failed request leaves `Loading...` forever. The structured alternative exists: `System_ESPNow.cpp:17228` registers `espnowdevices [json]`. **Blocker:** `keepSelection` as default does **not** reproduce what LLM does — `WebPage_LLM.h:448-476` short-circuits with `if (sig === modelSig) return;` and never rebuilds; restoring `.value` after an unconditional rebuild still closes an open dropdown. **Carry the signature short-circuit, not just `keepSelection`.** Also `LLM:458` sets `o.disabled` for unavailable models — a naive helper lets users select them. `WebPage_Automations.h:503` is **refuted** as a victim: it early-returns on `sel.options.length`. |

### P9, P14, P16, P26, P27, P31, P32, P33, P34, P35 — partial

| ID | Verdict | What survives / what was corrected |
|---|---|---|
| **P9** pane visibility | PARTIAL 3,800 B | `togglePane` is **byte-identical in three files** down to the same `console.warn` — `WebPage_ESPNow.h:303`, `WebPage_Settings.h:9`, `WebPage_Logging.h:771` (the last a bare declaration, not `window.`-assigned). ESPNow uses **three** highlight mechanisms in one file (`:1272` class, `:1483` `var(--link)`, `:2296` `var(--crumb-bg)`). Camera uses read-back-the-DOM state, EI uses a closure boolean. **THE `.vis-gone` TRAP IS CONFIRMED:** `WebServer_Utils.h:1438` is `display:none!important`, and `hw.show` sets `style.display=''`, which cannot beat it — so `hw.show`/`hw.toggle` **silently no-op on every Automations group**. Pick the class mechanism. **Blocker:** `togglePane` has **~29 inline `onclick` call sites** baked into C++ literals (Settings 20, Logging 10, ESPNow 2). Ship a `window.togglePane = hw.togglePane;` alias (~35 B) or this is M-effort with real regression surface, not S. Bytes cut because `toggleMessageType` (5,913 B) is mostly per-MAC wiring and side effects no generic helper removes. |
| **P14** `hw.kvRow` | PARTIAL 4,000 B | Bond emits **exactly 24** `stat-row` lines totalling **exactly 3,909 B** — matched to the byte. ESPNow's six-block at `:1815-1841` is 2,069 B, every value raw. The centred status panel is **exactly 12** copies across **exactly** `i2csensor_ds3231_web.h` and `i2csensor_pa1010d_web.h`. `hwRenderGenericSensor` (`WebPage_Sensors.h:557`) is `hw`-namespaced yet trapped in one page. **`WebPage_R1_Health.cpp:274` is REFUTED and the citation inverts the argument** — that line is `function apply(s){`, and the file contains **zero** `stat-row`/`innerHTML` hits. R1_Health builds no rows at all; it is the page **already doing the right thing** and should be cited as the model, not a duplication site. Defensible count is **44**, not 50. **Blocker:** Bond passes trusted markup into value slots (`renderSignalBars` at `:427`, `style=` overrides at `:350/:376/:549`), so an unconditionally-escaping `kvRow` renders the signal bars as literal text — needs a `valueHtml` opt-in. |
| **P16** `hw.delegate` | PARTIAL **600 B** | `attachMeshDelegation` (`WebPage_ESPNow.h:3454`) is the only correct implementation — one listener, dispatch by class, dataset arguments, guarded by `card.__meshHandlersAttached`. Three breakages confirmed: `WebServer_Utils.h:467-471` concatenates `itemPath` into `onclick` with **no escaping** while its sibling `BondFs.renderExplorer` escapes; `WebPage_Maps.h:2481-2484` escapes only apostrophes inside a **double**-quoted attribute while `:2389`, 92 lines above, does it correctly with `data-` + `addEventListener`; `WebPage_Automations.h:1465` locates a button by `button[onclick="createAutomation()"]`. **`doSensorBroadcast` is referenced at `WebPage_ESPNow.h:960`/`:961` and defined nowhere** — both Broadcast buttons throw `ReferenceError`, invisible at build time. **The byte claim is REFUTED and points the wrong way:** `class="btn-x" data-a="…"` is as long as `onclick="fn('a','b')"`, and each dispatcher replaces window globals ~1:1. The only reliable win is deleting ~20 per-site `.replace(/'/g,"\\'")` escapes. There are **295** `onclick=` attributes, not 40. **Rank this on correctness, not size.** |
| **P26** `hw.bindCliInput` | PARTIAL 2,400 B | `System_Camera_DVP_Web.h:254-268` holds the **only** keyed-debounce in the tree (`__cameraDebounceTimers` + `__cameraCancelDebounce`); grep found no other. The **five** ESPNow metadata setters (`:2887, :2892, :2897, :2902, :2907`) all interpolate raw input inside double quotes and **none has a `.catch`**, so a failed write leaves the typed value looking persisted. **The headline rationale is REFUTED:** Mic (`System_Microphone_Web.h:226-231`) and EI (`System_EdgeImpulse_Web.h:109-112`) bind the label to `input` and the CLI POST to **`change`**, which on a range input fires once on release. They are not missing a debounce; they do not need one. Only the camera uses `input`, which is why it is the only place a debounce exists. **Blockers:** the shared helper must replicate cancel-on-commit or it *introduces* a stale-value resend; `applyCameraAdjustment` has side effects beyond the CLI call (`:245-253`) so the helper needs an `onApply` hook. `WebPage_Settings.h:2612` (`wifiSetFlag`, a button reading its own label), `WebPage_Maps.h:256` (press-and-hold) and `WebPage_ESPNow.h:2641` (validated text submit) are **refuted** as sites. |
| **P27** `hw.bindCliToggle` | PARTIAL **600 B** | **The guest defect is fully confirmed on both halves.** `Bluetooth.cpp:2673` registers `bleautoreconnect` with `requiresAdmin=true` (field position checked against `System_Utils.h:60` and the adjacent `blepeers` row carrying `false`), and neither checkbox at `WebPage_Bluetooth.h:284`/`:322` carries `data-guest-hide`, while `bindAutoToggle` (`:1030-1039`) has no `.then`, no result check, no revert and no `.catch`. **A guest ticks the box, the device refuses, the box stays ticked.** **Two citations do not survive:** `:883` is mis-cited by ~25 lines (it is the blestream/config binds; the fixed toggle with the "displaying the OPPOSITE" comment is at `:906-930`), and **`:1049-1071` is refuted on substance** — `btn-g2-nav` and `btn-g2-verbose` mirror optimistically and then **correct from the device reply**, with a comment at `:1051-1055` explaining why a bare `g2nav` is a read. Their only defect is a missing `.catch`. So the file demonstrates **three** patterns, not a clean fixed/unfixed pair, and the binder must accommodate the optimistic-then-corrected middle case or those buttons regress to a visible round-trip lag. **Fix `data-guest-hide` in the same commit** — the binder alone only makes the refusal visible. |
| **P31** memoized fetches | PARTIAL **400 B** | **The most valuable claim is fully verified:** `WebPage_Bluetooth.h:499` `applyMode` calls `refresh()` at `:533`, and `refresh()` at `:763` calls `applyMode(d.mode)` at `:766`, with **no early-out**; `WebPage_Bluetooth.cpp:100` sets `doc["mode"]` unconditionally, so `if(d.mode)` is always true. **An unbounded self-perpetuating request loop** against `/api/ble/status` at round-trip cadence, each iteration running `bleinfo json` server-side, bypassing the page's own 80 ms queue. It also retroactively explains the symptom the file's comment at `:1116` recorded (~800 audit lines in 4 min) and mis-attributed to poll cadence. Exact counts: `/api/settings` **6×** in Settings, `/api/user/settings` **4×** in Dashboard, `/api/devices` **2×** in Sensors. **But the byte figure is not defensible** — the helper costs nearly what it removes; book this as latency and radio contention. **Blockers:** memoizing `/api/settings` is **wrong by default** (the page writes then re-reads — needs `invalidate()` wired into `sendSequential`); `/api/ble/status` is live status, not a document, so **the Bluetooth fix is a guard, not a cache** and must not be bundled into a memoization commit. Unconfirmed: schema ×4, Dashboard `/api/devices` ×2 (1 source site). |
| **P32** `hw.sensorReader` | PARTIAL 1,400 B | Eight `_sensorReaders` registrations confirmed at their exact lines. GPS (`i2csensor_pa1010d_web.h:36`) and RTC (`i2csensor_ds3231_web.h:82`) gate on `/api/sensors/status` **first** — two round-trips per second on a page also streaming camera frames. **`WebPage_Dashboard.h:111` reads `'fmRadioCompiled'` while `System_I2C.cpp:2534/2536` emits `"fmradioCompiled"`** and `WebPage_Sensors.h:201`/`:511` spell it correctly — **every Dashboard load on a board with an RDA5807 falsely shows "Detected but not compiled".** Four disagreeing device tables confirmed. **Two claims refuted:** *no* reader drops its promise — all eight `return hw.fetchJSON(…)`; and the gamepad/ANO validation divergence lives one level down in `hwRenderGamepadState` (`i2csensor_seesaw_web.h:48`) vs `hwRenderAnoState` (`i2csensor_ano_encoder_web.h:68`), so the proposed envelope **would not fix it**. **Blockers:** the GPS/RTC gate produces three distinct UI states and drives `stopTick()` — keep it as a first-class option; the registry already carries per-sensor cadence (`_sensorPollingIntervals`), so the envelope must read it; **ship the one-character `fmRadioCompiled` fix independently**; and extending `__dashSensorDefs` rows **adds** bytes — the table collapse buys correctness. |
| **P33** rate-limit serialisation | PARTIAL 1,600 B | Infrastructure fully verified: `WebServer_Server.cpp:3499-3507` is a **process-wide** `static unsigned long lastCmdTime` with a 50 ms floor returning **429 with a JSON body**; `/api/cli/batch` really is registered at **exactly** `WebServer_Server.cpp:5849`; `hw.postFormText` turns 429 into an opaque `HTTP 429`; Bluetooth's queue measures 1,576 B against a claimed 1,563. **But the danger map is substantially wrong.** `WebPage_Logging.h:721`, `WebPage_ESPNow.h:2107` and `:2013` are all **strictly sequential recursive chains** (`next()` called from inside the `.then`), so they await each response and are at **low** 429 risk — not the fan-outs described; and `WebPage_Automations.h:1083` is a two-line wrapper. **The genuine parallel fan-outs are elsewhere and were not cited:** `WebPage_Settings.h:2649` (`Promise.all` over three CLI writes), `WebPage_Automations.h:1433` and `:1441` (two consecutive `Promise.all`s over N per-time commands — a 429 on any one rejects the whole thing and silently drops that schedule), `System_Camera_DVP_Web.h:246`. **Blockers:** a global 80 ms FIFO **serialises the whole page** (six status commands on ESPNow become ~480 ms of enforced spacing) — needs a per-call opt-out; the limiter is process-global **across tabs**, so a client queue reduces but cannot eliminate 429s — **retry-on-429 is the load-bearing half, not the spacing**; scope the retry to 429 **only** (the check at `:3501` precedes execution, so it is safe today, but never widen it to 5xx). |
| **P34** path/arg helpers | PARTIAL **150 B** | `BondFs.token`/`join` (`WebServer_Utils.h:982`/`:983`) verified verbatim; the weaker inline join appears at **exactly four** other lines (`:458, :853, :869, :906`) and does mishandle a trailing-slash base. `basename` via `split('/').pop()` is **exactly 15** occurrences plus 2 `lastIndexOf` variants. **The best-evidenced item is the Automations divergence, confirmed to the character:** `:1348` emits `'name='+name` unquoted while `:1616` emits it quoted **and** backslash-escaped — creating and importing the same record stores two different names. The `g2bmp` bug is real and fixable: `G2_Glasses.cpp:26224`/`28908-28919` read positionally via `CommandArgs::arg(0..3)` and `System_Command.h:76` confirms quoted tokens are honoured verbatim. **But three of four Bluetooth sites are REFUTED as deliberately raw** free-text-to-end-of-line commands: `:951` `g2show` (`cmd_g2show` passes `argsInput` straight through), `:970` `g2notify` (**the source comment at `:968-969` says so explicitly**), `:903` `blename` (`Bluetooth.cpp:2408-2418` takes the whole remainder). The **parent-path divergence is refuted as stated** — both forms agree on every realistic path and diverge only on multi-trailing-slash input. **THE BYTE CASE COLLAPSES:** `hw.basename(x)` is 4 B *shorter* than `x.split('/').pop()`, gross ~425 B against a ~300 B bundle, and `hw.quoteArg` **adds** bytes. **Sell this on correctness or not at all.** `hw.macToken` cannot be one function — of seven rebuilds, two uppercase, two lowercase as a matched pair, three do no case change and two of those feed **DOM element ids**. |
| **P35** `hw.bondTarget` | PARTIAL **500 B** | All eight sites exact, including the self-incriminating comment at `WebPage_Settings.h:3610`: `// Toggle helpers — mirror the Files/CLI page pattern.` Three different state variables confirmed: `bondCurPath` (Files, no target var at all), `window._settingsTarget`, `cliBondMode`. The read/write asymmetry is real — `sendSequential` (`:117-121`) routes by target, `loadNotifPolicy` (`:1631`) POSTs `/api/cli` directly. **But the bug scenario is REFUTED:** the Expand button (`:1600`), pane (`:1602`) and Save (`:1618`) all sit inside `settings-local-container` (`:589-3584`), which `showBondedSettings` hides — so neither half is reachable in bonded mode. **Latent inconsistency, not a live read-local/write-remote defect.** **Blockers:** the three switchers are not interchangeable — `cliSetTarget` (`WebPage_CLI.h:253-257`) stops the local log poller specifically to free an HTTP socket and clears `cliOutput` + localStorage; `showBondedSettings` stops a dirty-poll and re-hides fields; `showBondedFs` deliberately does **not** call `updateBondedStorageStats`, with a comment recording a three-way race. All need `onSwitch` hooks. Persisting the target across pages is a **safety-relevant behaviour change** — a user who chose "Bonded" on Files lands on Settings pointed at the worker. |

---

## 5. Refuted / do-not-unify

**This section is load-bearing.** Every item here was checked and rejected. Do
not re-file them.

### 5.1 — Do NOT unify: the local version is correct

| Site | Why it must stay |
|---|---|
| `WebPage_Maps.h:2041` `escapeHtml` | Interpolates into `title="…"` at `:2779`. `hw._esc` is `textContent`-based and does **not** escape quotes — adopting it would introduce attribute injection on a path that is currently safe. Same for `WebPage_Bluetooth.h:729-730`. |
| `WebPage_Automations.h:1731` `downloadFromGitHub` | Cross-origin to `raw.githubusercontent.com`. Every `hw.fetch*` forces `credentials:'include'` (`WebServer_Utils.cpp:773`), and GitHub sends no matching CORS credentials headers, so the request would fail outright. **Keep it a bare `fetch`.** Give it a timeout and an `https:` scheme check instead. |
| `WebServer_Utils.h:984` `esc()` | A **JS-string-literal** escaper (backslash + apostrophe) used to build inline `onclick` in `renderExplorer`. Folding it into an HTML escaper emits `&#39;` inside generated JavaScript and breaks the code. **Rename to `escJs()`**, do not merge. |
| `WebPage_Bluetooth.h:951` `g2show`, `:970` `g2notify`, `:903` `blename` | All three take the **raw remainder** of the line. `cmd_g2show` (`G2_Glasses.cpp:23899-23902`) and `cmd_blename` (`Bluetooth.cpp:2408-2418`) do no tokenisation, and `WebPage_Bluetooth.h:968-969` says so in a comment. `hw.quoteArg` would display literal quote characters on the glasses / in the BLE name. |
| `WebPage_Maps.h:2055` `cleanName` | Strips nulls, C0 controls, bidi overrides, BOM, soft hyphen and PUA codepoints from OSM/Overpass strings. **Data sanitisation for one source, not HTML escaping.** Must not be merged with `hw.esc`. |
| `WebServer_Utils.h:278` `formatSize` | Sniffs its input (`if (s.indexOf('bytes') < 0 && !/^\d+$/.test(s)) return s;`) and passes pre-formatted **remote** strings through untouched. A drop-in `hw.fmtBytes(n)` renders bonded directory sizes as `NaN`. |
| `WebServer_Server.cpp:4423` `streamViewerHead` | Deliberately avoids `streamBeginHtml` so it does not drag the nav, the toast system and 49 KB of file-browser script onto a page whose whole job is to show bytes. Its `--v-*` dark mode is real; the extracted CSS dump shows it empty only because the extractor does not expand `FILE_VIEWER_DARK_VARS`. |
| `WebPage_Files.h:221-256` `bondBrowse` | The double-render and the deliberate serialisation of the listing before the storage-stat fetch encode a diagnosed three-way race. That ordering must stay local. |
| `WebPage_Sensors.h:388` `readSensor` + `_sensorReaders` registry | **The best extensibility pattern in the web layer.** Copy it, do not refactor it away. |
| `WebPage_R1_Health.cpp` | Zero `getElementById`, zero `style.display`, zero `alert`, all writes through `hw.setText`/`hw.toggle`. **The reference implementation.** Cite it as the model, never as a duplication site. |

### 5.2 — False leads: checked and dropped

- **Dynamically-injected `[data-guest-hide]` is NOT a guest leak.**
  `WebServer_Utils.cpp:759-761` streams a
  `.guest-view [data-guest-hide]{display:none!important}` rule that covers nodes
  added after the `DOMContentLoaded` sweep. The real gap is elements that carry
  **no** `data-guest-hide` at all (`WebPage_Bluetooth.h:284`, `:322`).
- **`/enabled/i.test()` does NOT false-positive on `"disabled"`** — `"disabled"`
  does not contain the substring `"enabled"`. `WebPage_Bluetooth.h:1037`'s seed is
  correct for today's strings; it is a fragility, not a bug.
- **The `TypeError` on an empty CLI body does not happen** (P28). Every producer
  returns a string or throws. Guarded and unguarded `indexOf` are identical today.
- **`unitToMs`/`msToUnit` do not exist** (P20). Grep returns zero hits.
- **`WebPage_Maps.h:614`'s expired session does not produce "bad magic"** (P21).
  `:615` checks `resp.ok` first.
- **`.remote-card` does not exist anywhere** (P7).
- **`WebPage_Automations.h:503` does not reset the user's selection** (P30) — it
  early-returns on `sel.options.length`.
- **No `_sensorReaders` registration drops its promise** (P32) — all eight
  `return hw.fetchJSON(…)`.
- **Empty CSS/JS literals in the dumps are extractor artifacts.** Anything that
  looks empty or truncated in `/private/tmp/.../js|css` must be re-checked against
  the C++ before it is reported — macros are not expanded, and C++ String
  concatenation shows up as `{guest:,user:,admin:,}`.

### 5.3 — Unit blockers: do not add these to the flash total

| Item | Correct unit |
|---|---|
| **P5** 49,630 B | **heap, per request**, on 5 pages. Flash delta is ≈ **−150 B**. |
| **P17** 35,897 B | **wire, per CLI page load.** The literal exists once in the image either way. |
| **P24** ~2,200 B | **ADDED, not saved.** `alert(x)` is 9 chars; `hw.notify('info',x)` is 20. The dialog shell cannot be deleted while `hwConfirm`/`hwPrompt` remain in use. Rank on UX. |
| **P19** 0 B | Security, not size. |
| **P16** | Delegation is byte-neutral at best — `class=` + `data-` is as long as `onclick=`. |
| **P34** | `hw.basename(x)` is *shorter* than the code it replaces; `hw.quoteArg` adds bytes. |
| **P32** table collapse | **Adds** bytes; buys correctness. |
| **P31** | Helper costs nearly what it removes; the payoff is latency and radio contention. |

### 5.4 — Adoption blockers that must be cleared first

1. **`hw.show` sets `display:''`**, which resolves to the stylesheet value.
   `WebPage_Battery.cpp:48` (`#bat-live`, inline `display:flex`, no CSS rule),
   `WebPage_MQTT.cpp:346`/`:352` (`inline-block`) and Speech cannot adopt it until
   it takes an explicit display argument.
2. **`.vis-gone` is `display:none!important`** (`WebServer_Utils.h:1438`), so
   `hw.show`/`hw.toggle` silently no-op on it. Pick the class mechanism.
3. **`hw.pollJSON` is weaker than four of the pollers it would replace.** Upgrade
   before adopting.
4. **`hw.notify`, `hw._esc`, `hw.cliConfirm` and `hwConfirm` do not exist on
   public pages** (`WebServer_Utils.cpp:779`/`:784`).
5. **The standalone file-viewer shell has no `hw.*` at all**
   (`WebServer_Server.cpp:4406-4414`).
6. **`console.log` is stubbed to a no-op** by `WebServer_Utils.cpp:715-722`, so
   `hw.dbg` must use the saved `L`/`W`/`D`.
7. **`hw.cliConfirm` hardcodes `interactive:true`** and appends a `'yes'` command.
8. **`/api/cli` is rate-limited process-wide at 50 ms** with a **JSON** 429 body
   (`WebServer_Server.cpp:3499-3507`), while every caller expects text.
9. **`BondFs.renderExplorer` hard-depends on `FileBrowser`** — the P17 split must
   be three-way.
10. **`getFileBrowserScript` is emitted TWICE on the ESPNow page**
    (`WebPage_ESPNow.h:3771` plus its own explorer at `:788`) — check load order
    before changing what each bundle defines.

### 5.5 — Dead code that is the ONLY implementation of a shipped feature

Decide keep-or-delete deliberately; do not let a cleanup pass bury these.

- **`WebPage_Maps.cpp:445` `viewFiles()`** on the unregistered `/waypoints` page
  is the sole UI for waypoint file attachments that `/api/waypoints` still
  publishes (`WebPage_Maps.cpp:573`).
- **`System_EdgeImpulse_Web.h:263`** bounding-box overlay is one wrong element id
  (`camera-stream-img`) away from working — and `_eiDrawBoxes` **is** called at
  `:324`, `:332`, `:335`, so deleting the definitions requires deleting those too.
- **`i2csensor_bno055_web.h:85` `DeviceRotationViz`** is one call away from
  working, **but its only caller passes `yaw,pitch,roll` into a `(pitch,roll,yaw)`
  signature** — wiring it up ships a visibly broken cube. Prefer deletion.
  `updateDeviceOrientation` (`:211`) **is** called from `:57-58` and must survive.
- **`WebPage_Settings.h:2439` `fetchBuildConfig`** — deleting the clobbered
  `onload` is correct, but it makes permanent a latent regression:
  `window.__buildConfig` stays null, so build-unavailable debug options are never
  hidden. Record it as a known gap.

---

## 6. Divergences worth fixing as bugs

### Tier 1 — a shipped feature does not work

| # | Bug | Site | Fix |
|---|---|---|---|
| 1 | **`bluetoothrequireauth` is not a command.** Exactly 1 occurrence repo-wide; the real verb is `blerequireauth` (`Bluetooth.cpp:2661`). The "Unknown command" reply is discarded by the `Promise.all` at `:2649`. The Bluetooth Require-Authentication toggle **has never worked**. | `WebPage_Settings.h:2647` | One-word fix; then honour the superadmin refusal (P13, P27) |
| 2 | **`var(--warning)` used 15×, declared 0×**, and **`--panel-border` used 10× in `WebPage_Bond.cpp`, declared 0×** — all 10 Bond dividers are invisible. | `WebServer_Utils.h:1272` | Two lines (P7) |
| 3 | **SSE listeners are never re-attached after reconnect.** `sseNotify` builds a new EventSource carrying only its own listener; Dashboard has **no polling fallback**, so it freezes on stale data and reads as a firmware hang. | `WebServer_Utils.cpp:828`, `WebPage_Dashboard.h:451-453` | P22 |
| 4 | **`fmRadioCompiled` vs `fmradioCompiled`.** Every Dashboard load on a board with an RDA5807 shows a false "Detected but not compiled" banner telling the user to enable a flag that is already on. | `WebPage_Dashboard.h:111` vs `System_I2C.cpp:2534` | One character |
| 5 | **`/api/upload` is registered nowhere** — only `/api/files/upload` (`WebServer_Server.cpp:5843`). Speech's Upload Model button has never worked, and its elaborate error-parse guard at `:508-522` can never execute because the failure arrives as a rejected promise. | `WebPage_Speech.h:602` | P21 |
| 6 | **`doSensorBroadcast` is referenced by two buttons and defined nowhere.** Both throw `ReferenceError`; invisible at build time because the JS lives in a C++ literal. | `WebPage_ESPNow.h:960`, `:961` | P16 |
| 7 | **The LLM Run button stays permanently disabled after a *successful* run.** Six properties set on entry, one restored in `.then`, all six restored only in `.catch`. | `WebPage_LLM.h:813-838` | P23 |
| 8 | **Bonded log Download navigates instead of downloading** — exactly the bug `WebPage_Files.h:262-266` documents in prose 400 lines away, in the same `BondFs.pull` callback shape. | `WebPage_Logging.h:1621` | P21 |
| 9 | **`WebPage_Bluetooth.h:533` is an unbounded request loop.** `applyMode` → `refresh` → `applyMode`, with `doc["mode"]` always set (`WebPage_Bluetooth.cpp:100`) so the condition never goes false. Bypasses the page's own 80 ms queue. Retroactively explains the ~800-audit-lines-in-4-min symptom recorded at `:1116` and mis-attributed to poll cadence. | `WebPage_Bluetooth.h:499`/`:533`/`:763`/`:766` | A guard, **not** a cache (P31) |
| 10 | **Sensors page: `WebPage_Sensors.h:604`/`:609` call `hwBuildAnoInner`/`hwRenderAnoState` unguarded** while the gamepad branch one line below at `:610` **is** guarded. On a build with `ENABLE_ANO_ENCODER` off, an ANO-shaped peer payload throws into a `.catch` that renders a bare `Error` once a second with nothing in the log. | `WebPage_Sensors.h:604`, `:609` | `typeof` guard |

### Tier 2 — the UI lies about device state

| # | Divergence | Sites |
|---|---|---|
| 11 | **Guest can tick a checkbox the device refuses.** `bleautoreconnect` is `requiresAdmin=true` (`Bluetooth.cpp:2673`); neither checkbox carries `data-guest-hide`; `bindAutoToggle` discards the refusal. | `WebPage_Bluetooth.h:284`, `:322`, `:1030-1039` |
| 12 | **Peer-supplied names reach `innerHTML` unescaped on the bond channel** — the auth/RCE channel — while the same file's `refreshBondDevices` uses the safe `createElement`+`textContent` path. | `WebPage_Bond.cpp:218`, `:350` vs `:873-877` |
| 13 | **Remote peer JSON *keys* are concatenated raw into `innerHTML` once a second**, while the sibling `fmt()` escapes the value. | `WebPage_Sensors.h:573`, `:577` → `:580` |
| 14 | **Over-the-air SSID: the machine-readable copy is escaped, the visible one is not — on the same line.** | `WebPage_Settings.h:2801` |
| 15 | **`FileBrowser.rowHtml` interpolates `o.clickExpr` into an `onclick` attribute** with no escaping — no HTML-entity escaper can fix it. | `WebServer_Utils.h:342-344` |
| 16 | **A local file named `it's.txt` makes every button on that row inert**, while the bonded pane renders the same name correctly. | `WebServer_Utils.h:467-471` vs `:1117` |
| 17 | **Every "Kick" button sends the same argument-less command**, with `maxConnections` up to 4. | `WebPage_Bluetooth.h:734` |
| 18 | **The Bond page resurrects a shared-watermark race** that `BondFs.exec`'s own comment says was removed as incorrect. | `WebPage_Bond.cpp:724` vs `WebServer_Utils.h:1000-1008` |
| 19 | **`readOnly` is honoured by 1 of 3 settings renderers**, so a readOnly entry becomes editable and then emits its dotted `jsonKey` as a bogus CLI verb. `readOnly` has exactly one occurrence in `WebPage_Settings.h`. | `:376` vs `:725`, `:1026` |
| 20 | **The `bitmask:` dialect is documented as universal** (`System_Settings.h:1365-1372`) **and implemented by 2 of 3 renderers.** In `mqtt`/`http`/`bluetooth`/`espnow` it renders as a dropdown of literal `0x1|Thermal` tokens. | `:383`, `:1031` vs `:734` |

### Tier 3 — divergent contracts, latent

| # | Divergence | Sites |
|---|---|---|
| 21 | **Nine incompatible readings of one `OK:`/`Error:` contract**, 33 live sites; the correct anchored regex is trapped in `hw.cliConfirm` with 2 callers. `WebPage_ESPNow.h` alone has **four**. | P28 |
| 22 | **`when()` takes seconds, `formatMillisTimestamp` takes milliseconds**, and `etaStr` takes **minutes**. Four missing-value sentinels: `--`, `Unknown`, em-dash, `?`. | `WebPage_R1_Health.cpp:270`, `WebPage_Settings.h:2902`, `WebPage_Battery.cpp:74` |
| 23 | **`createAutomation` stores `name=` unquoted; `importAutomationFromJson` stores it quoted and escaped** — the same record gets two different names depending on how it was created. | `WebPage_Automations.h:1348` vs `:1616` |
| 24 | **Two modals on the same rendered page dismiss differently**; `#editor-modal` can only be closed by its Close button and holds unsaved edits. | `WebPage_Files.h:35` vs `WebPage_AviPlayer.h:180` |
| 25 | **Dismiss-on-outside implemented twice in one file** with four differences (event, phase, arming delay, teardown); `_searchOutside` is never detached on other close routes. | `WebPage_Maps.h:1859-1866` vs `:2433-2443` |
| 26 | **`refreshSyncPeers` and `refreshSyncPeersFor` are the same 25 lines twice**, both scraping human-readable output with a column heuristic, **neither with a `.catch`** — `Loading...` forever on failure. The command has supported `json` all along. | `WebPage_Settings.h:3452`, `:3502` vs `System_ESPNow.cpp:17228` |
| 27 | **Folder-delete uses native `confirm()`; file-delete 16 lines later uses `hw.cliConfirm`** — same handler, same file. | `WebServer_Utils.h:576` vs `:592` |
| 28 | **Camera and mic recordings lists are the same feature written twice**, diverging on escaping, `encodeURIComponent`, redundant-rebuild gating **and** confirm dialog (async vs sync). | `System_Camera_DVP_Web.h:167` vs `System_Microphone_Web.h:154` |
| 29 | **The Sensors page has two parallel status appliers with non-overlapping coverage** (poll path `:276`, SSE path `:400`+`:491`). Whichever transport dies takes a different half of the UI with it. | `WebPage_Sensors.h:276`, `:400`, `:491` |
| 30 | **`EVENT_KINDS` is hand-typed and has drifted**: `SYSEVT_KIND_LIST` declares 152 kinds, the web picker lists 140. Twelve events the firmware emits and the OLED offers cannot be chosen as a web automation trigger. | `WebPage_Automations.h:433` vs `System_Events.h:129` |
| 31 | **`window.alert` is monkey-patched to the async `hwAlert`** (`WebServer_Utils.h:1545`) which discards the promise, and `hwAlert` keeps a **single module-level resolve slot** (`:1526`) so overlapping alerts strand each other. On public pages the same source line is a native blocking dialog. | P24 |
| 32 | **`WebPage_Sensors.h:229` `bind(id,cmd)` takes a CLI string; `WebPage_Bluetooth.h:856` `bind(id,fn)` takes a handler.** Same name, same arity, opposite meaning. | — |
| 33 | **`controlSensor` re-fetches and rethrows on Sensors; the Games copy swallows every error and resolves `''`** — so gameplay starts with a dead IMU and no diagnostic. | `WebPage_Sensors.h:371` vs `WebPage_Games.h:9708` |

---

## 7. Page-by-page notes

**`WebServer_Utils.cpp` / `.h`** — defines the entire `hw.*` runtime and
`getFileBrowserScript`. **The shared layer does not use itself:**
`getFileBrowserScript` calls `document.getElementById` 24 times, assigns
`el.style.display` 14 times, calls `alert()` 4 times, and calls
`hw.qs`/`hw.show`/`hw.hide`/`hw._ge` **zero** times. Its one raw `fetch()`
(`:406`) is the only raw fetch in the shared layer and is exactly where the 401
redirect goes missing. Genuinely page-specific: `FileBrowser.iconName` (`:205`),
`renderActionIcon` (`:271`), `BondFs.token`/`checkAvailable`/`list`/`stat`/`pull`,
`createFileManager`'s `free * 0.9` precheck (`:882`), the console-suppression IIFE
(`:717`).

**`WebServer_Server.cpp`** — overwhelmingly C++ handlers; only three small JS
regions (`:1868`, `:4027`, `:5081`). `:1868-1880` re-emits a snapshot
`streamBeginHtml` already sent six lines earlier — free deletion. The
file-viewer shell (`:4423`) is deliberately chrome-free; leave it.
**`WebServer_Handle.h` contains no JavaScript at all.**

**`WebPage_ESPNow.h`** (174 KB of JS, the heaviest page) — 53 of the 112
hand-rolled `/api/cli` sites, 228 `getElementById`, 93 `style.display`, 31
`alert()`, one `hw.notify`. Owns the only correct delegation implementation
(`attachMeshDelegation`, `:3454`) and the `' entries'` doneMarker fix. Genuinely
page-specific: chunked-message reassembly (`:3613-3674`), delivery-state
reconciliation via `data-msg-id` (`:3714`), the `espnowtoporesults` parser and
hop-tree renderer (`:2313`), the ASCII connection graph (`:2467`), the
sensor-streaming pill state machine (`:2047-2106`), `pairMeshArg`, the
frames-dropped badge.

**`WebPage_Settings.h`** (173 KB) — three settings renderers, three
snapshot/diff engines keyed three different ways (element id, `data-cmd`,
`data-kind`), 81 `getElementById` + 79 `$()` with a hard invisible boundary at
line 2430, 75 `alert()`, six fetches of `/api/settings` and four of the schema
per load. Genuinely page-specific: `refreshUsers` composition (14.7 KB — its
escaping, ranking and badges are shareable, the assembly is not), the WiFi
tri-state widgets, `scanNetworks`/`selectSsid`, the 14 KB `HELP`/`GL` debug
tooltip tables (note `GL` declares `storage:'Storage'` **twice**), the HTTPS
cert-present interlock, `renderBondedModules`' policy, `checkDirty`.

**`WebPage_Maps.h` / `.cpp`** — 107 KB of JS plus a 7.3 KB **unregistered**
second page. The projection math is written **six** times (`:318, :988, :1722,
:1764, :2134, :2521`), three byte-identical. `layerEnabled` (`:1023-1046`)
duplicates `WEB_LAYER_DEFS` (`:381`) and does a `getElementById` **per feature per
render pass** across nine passes — almost certainly the page's dominant cost.
Genuinely page-specific: `parseHWMap` (HWMAP v6 binary reader), `renderMap` (30 KB
canvas renderer), the label-placement pass, the segment-break haversine
heuristic, `drawTrackLabel`, `cleanName`, the `STITCH_MAX=9` model (mirrors the
CLI token cap), the drag-vs-click discrimination, the press-and-hold trio.
`streamMapsPageLodZoomConstants` (`WebPage_Maps.cpp:23`) generates JS constants
from `System_Maps.h` so they cannot drift — **the technique the hand-mirrored
`FT_*`/`ST_*` block at `WebPage_Maps.h:359-378` should adopt.** Note
`LOD_RAILWAY` is emitted and never used; `renderMap` gates railways on
`LOD_WATER`.

**`WebPage_Automations.h`** (85 KB, largest single JS page) — 148
`getElementById`, 56 `style.display`, 10 `alert()`, zero `hw._esc`. Uses the HTTP
layer, none of the DOM layer. Genuinely page-specific: `normalizeAutomation`
(v2 `triggers[]` shim), the condition mini-DSL builder and parser (170 lines
apart), `updateValuePlaceholder`'s 33-case map, the 4-trigger caps, the Armed
badge + 1 Hz countdown, `buildParts`' multi-time fan-out, the GitHub blob→raw
rewrite.

**`WebPage_Logging.h`** (50 KB) — **the dominant finding is the sensor/system
twin**: eight function pairs hand-copied with a command name and element ids
swapped (~13.5 KB collapsing to ~7 KB), and **nine of the page's divergences sit
inside that seam**. Zero raw `fetch` — the HTTP layer is fully adopted; the
problem is that the copies did not stay copies. Genuinely page-specific:
`parseLogFile` (four log dialects), `getCategoryColor`'s four-stage tag policy,
`populateFlagsPane`, `displayLogLines`, the `&dec=1` unseal parameter, the
`/logging_captures` ↔ `/system/sys_logs` toggle, `bondLogView`/`bondLogCtl`.
Perf risk not measured on hardware: `displayLogLines` (`:1359`) rebuilds the whole
filtered set as one `innerHTML` string with ~200 B of inline style per line, no
cap, no virtualisation, wired to `oninput` with **no debounce**.

**`WebPage_Bluetooth.h`** — 41.8% of its shipped JS is comments and indentation.
Owns the **only** client-side rate-limit queue (`:431`, 80 ms) and the **only**
adaptive two-speed poller (`:1140`). Contains both the fixed (`:906-930`), the
optimistic-then-corrected (`:1049-1071`) and the unfixed (`:1030-1039`) toggle
patterns. Genuinely page-specific: `parseG2Status`/`g2OverallState` (two-temple
topology), `parseRingStatus`/`renderRing` (compensating for 5–9 s ring connect
latency), `applyMode`/`applyState`'s server-vs-G2 dual personality,
`snapshotConnState`. `WebPage_Bluetooth.cpp` emits no JS.

**`WebPage_Dashboard.h`** — effectively already minified (556 quoted lines, 0
comment bytes, 530 B indent) but carries 5,703 B of `console.*` (15.6% of its JS).
`createSensorCards` does `grid.innerHTML=''` on every status update (`:447`),
destroying the order/hidden state `Dash.loadSavedLayouts` just applied at `:797`
— **the "Sensor Status" layout editor is a shipped feature that does nothing.**
The never-cleared `setInterval(tryPatch,500)` at `:134` calls a no-op twice a
second forever. Genuinely page-specific: `Dash.updateSystem`'s field mapping,
`getPanelLabel`, `applyOrder`/`applyHidden`/`getCurrentOrder`, `saveLayout`'s
deliberate CLI audit dual-write.

**`WebPage_Sensors.h`** — the `_sensorReaders` / `_sensorPollingIntervals`
registry is the best extensibility pattern in the web layer. Three mechanisms
decide one sensor's poll rate (registry, hardcoded ladder in the poller,
settings-loaded global). Defines `bind`, `sendCmd` and `debugLog` that **13 other
files call as platform API**, unguarded. Genuinely page-specific:
`loadSensorSettings`' thermal/ToF paths, `readSensor`'s registry,
`updateRemoteSensor`'s ANO-vs-gamepad widget swap, `applySensorStatus`'s queue
banners. **`WebPage_Sensors.cpp` contains no JavaScript.**

**`WebPage_LLM.h`** — 35.8% comments. Owns the **only** `visibilitychange`
handler, the **only** exponential backoff, and the **only** GB-scaling byte
formatter, all trapped. Genuinely page-specific: `fetchStatus`'s edge-triggered
narration, `pollResult`'s byte-cursor streaming (deliberately adopts the device's
next offset because the device passes malformed UTF-8 through untouched),
`menuFetchAll` pagination, the guided template/entity compose, `finishGen`'s
`Do:`-suggestion regex, `qaAsk`'s `do:` parsing. `WebPage_LLM.cpp` emits no JS.

**`WebPage_CLI.h`** — streams 49,650 B to use 13,753 B of it. Has its own 401
stop (a second independent solution). Writes localStorage `cliOutputHistory` at
**7 sites and reads it at zero** (~518 B); `clearHistory()` (`:228-234`, 190 B)
has zero callers. Genuinely page-specific: `__stripAnsi`/`__applyClear`, the
bond-mode poller suppression and its socket rationale, `executeCommand`'s
help-mode backup/restore.

**`WebPage_Speech.h`** — the strongest single argument for a shared transport:
**four things it invokes do not exist** — `ls` (the real command is `files`),
`sr loadwake`, `sr loadcmds` (neither appears anywhere in the firmware), and
`/api/upload`. Six `postCli` chains have no `.catch` while four on the same page
do. Builds three panes as concatenated HTML with no escaping at all. Genuinely
page-specific: `parseStatus`'s dB meter and the MultiNet voice-state ladder.
`WebPage_Speech.cpp` emits no JS.

**`WebPage_Bond.cpp`** — 29% of its JS is comments and indentation. Resurrects
the removed shared-watermark design (`:724`). Has three sensor-capability tables
that disagree (`:393` has 8 rows and omits FM Radio; `:460` and `:568` have 9).
Its `.sensor-row .st-name.disconnected` CSS rule (`:94`) is **dead** — the
renderer emits class `off` (`:512`), so an off sensor looks identical to an on
one. Applies `.alert-warning` and then overrides every property with hardcoded
light-mode hex (`:259`). Genuinely page-specific: `renderBondProgress`,
`getHealthClass`, `isBondSynced`, the sensor Enable/Stream toggle table,
`setOutputBorder`, `renderSignalBars`.

**`WebPage_MQTT.cpp` / `WebPage_Battery.cpp` / `WebPage_R1_Health.cpp`** —
Battery and R1_Health together have **0** `getElementById`, **0** `alert()`,
**0** `confirm()`, 2 `style.display`, and use `hw.setText`/`hw.toggle`/`hw._ge`/
`hw.pollJSON` throughout. **They are the reference implementations.** R1_Health's
`action()` (`:338`) chains the re-fetch onto the action's promise instead of
guessing a settle delay — the pattern Bond's five different `setTimeout(refresh,
N)` values (400/1500/1000/600/300 ms) should adopt. R1_Health declares `#rh-live`
(`:173`) and never references it, so a disconnected ring shows "not connected"
above a still-visible stale metrics panel. MQTT's `mqttRefresh` (`:421`) repeats a
pill update that its sibling at `:366` guards with `if (dot && txt)` — it throws
every 5 s once the service is disabled and the pill is hidden.

**`WebPage_Files.h` / `WebPage_AviPlayer.h`** — 52 raw `getElementById` between
them against a streamed `hw._ge`. The `/files` page composes three
independently-authored script blocks using three handler conventions
(`window.onload` + inline `onclick`; clobbering `el.onclick`; `addEventListener`
in an IIFE) — both assignment forms are last-writer-wins, and
`WebPage_Settings.h` already demonstrates the failure with two `window.onload`
assignments. `bondView` (`:283`) is a hand-copy of the shared `ViewFile` whose
comment cites a line number now 23 lines stale. Genuinely page-specific:
`WebPage_AviPlayer.h:44-138` (RIFF/AVI parser + canvas playback, including the
truncated-file recovery path), `bondBrowse`'s deliberate ordering.

**`WebPage_Login.h` / `LoginRequired.h` / `LoginSuccess.h` / `Register.h`** —
the only pages with **no** `hw.notify`/`hw._esc`/`hwConfirm`. `WebPage_Login.h`
escapes via `streamHtmlEscaped`; `WebPage_Register.h:46/:53` and
`WebPage_LoginRequired.h:23` stream server-supplied text **unescaped** — two
adjacent public pages disagreeing on whether escaping is needed.
`streamLoginSuccessContent` has no caller; if it is ever wired up, note that its
cookie poll can never succeed (the session cookie is `HttpOnly`,
`WebServer_Server.cpp:476`) and the page works only because the `meta refresh`
fires 1 s before the JS gives up.

**Sensor modules (`i2csensor_*_web.h`, `System_*_Web.h`)** — the plugin registry
is good; the envelopes around it have drifted (GPS/RTC double round-trip;
gamepad validates `j.valid`, ANO ignores it). `System_Camera_DVP_Web.h` holds the
**only** keyed debounce in the tree. `i2csensor_pca9685_web.h` emits no JS at all.
Genuinely page-specific: the MJPEG-over-`<img>` pull loop and the
hmirror/vflip/rotate tri-state model, the five 256-entry colormap generators +
EWMA blit, `updateToFObjects`' stability hysteresis (noise debouncing, not UI
debouncing), the DS3231 local 1 Hz tick, the BNO055 rotate/project/shade
pipeline, the EI detection markup. `hwRenderAnoState`/`hwRenderGamepadState` are
**already correctly promoted** and reused by the remote path.

---

## 8. Suggested sequencing

### Stage 0 — one-line fixes, land immediately, independent of everything

These need no shared-layer design and each fixes a user-visible defect.

1. `WebPage_Settings.h:2647` — `bluetoothrequireauth` → `blerequireauth`.
   **Then verify the superadmin refusal is surfaced**, because the toggle goes
   from dead to live and gated.
2. `WebServer_Utils.h:1272` — declare `--warning` and `--panel-border` on `:root`.
3. `WebPage_Dashboard.h:111` — `fmRadioCompiled` → `fmradioCompiled`.
4. `WebPage_LLM.h:823` — restore all six properties on the success path
   (or move the restore to a `.finally`).
5. `WebPage_Logging.h:1621` — replace `window.location` with the `<a download>`
   dance already written at `WebPage_Files.h:269`.
6. `WebPage_Sensors.h:604`/`:609` — add the `typeof` guard the adjacent gamepad
   branch already has.
7. `WebPage_Bluetooth.h:533` — early-out `applyMode` on an unchanged mode (or stop
   `refresh()` calling it). **A guard, not a cache.**
8. `WebPage_Bluetooth.h:284`/`:322` — add `data-guest-hide`.

### Stage 1 — bytes with no logic change

9. **P5** — `inline String` → `const char*` + `streamChunkC`. One signature, five
   call sites, ~49.7 KB of heap per request on five pages. **Highest value per
   line changed in the report.**
10. **P1** — the CMake pre-pass. **~194 KB**, zero JS logic changed. Do it before
    anything else because it shrinks every later diff. Honour all six blockers
    (JS tokenizer not a line regex; preserve template-literal indentation; operate
    on raw-literal bodies in place; skip concat-literal files; run after the JS
    extraction the webui tests read; include the file-viewer shell by path).
11. **P2** — delete the **nine confirmed-dead** families only. Do **not** touch
    `updateStatusIndicators`, `checkAlreadyActiveSensors`, `Dash.updateSensorStatus`,
    the live ESPNow metadata block, `WebPage_CLI.h:228` or
    `i2csensor_mlx90640_web.h:45`. Make the three keep-or-delete product decisions
    explicitly (§5.5).
12. **P3** — delete the ~195 sentinels **except** the Dashboard and Sensors ones
    (`test_embedded_js_syntax.py` covers 0% of concat-literal JS), then convert the
    rest to `hw.dbg` — with `hw.dbg` reading the saved `L`/`W`/`D` and living in
    the unconditional block.

### Stage 2 — small helpers with wide blast radius

13. **P18** `hw.esc` — promote the **regex** form, move it out of `!isPublic`,
    rename `WebServer_Utils.h:984` to `escJs()`. **Prerequisite for P19.**
14. **P19** — escape the ~25 sinks. **Ship the Bond ones alone and first.**
15. **P28** `hw.cliFailed` — small helper, wide blast radius, land early so P6,
    P26 and P27 build on it. **Per-site audit, not a regex sweep** — it flips
    classification in both directions.
16. **P17** — the **three-way** split, plus `hw.bondStatus()` (with the zero-MAC
    guard) in the always-streamed layer. **Prerequisite for P8 and P35.**
17. **P9** — ship `hw.togglePane` alone (three byte-identical copies) **with a
    `window.togglePane` alias** so the ~29 inline `onclick` sites need no edit.
    Pick the class mechanism because of `.vis-gone`.
18. **P23** `hw.busy` — S effort, low risk, makes the LLM bug unwritable.
19. **P30** `hw.fillSelect` — carry the **signature short-circuit**, not just
    `keepSelection`.
20. **P32** — ship the `fmRadioCompiled` fix separately (already Stage 0), then the
    envelope; keep the GPS/RTC gate as an option.

### Stage 3 — the transport and the DOM

21. **P33** first, then **P6**. The queue and retry-on-429 must exist before the
    CLI call site gets cheaper, or the fix trades `HTTP 400` for `HTTP 429`. Note
    the real fan-outs are `WebPage_Settings.h:2649`, `WebPage_Automations.h:1433`
    and `:1441`, `System_Camera_DVP_Web.h:246` — **not** the sequential chains the
    proposal cited.
22. **P10** — **upgrade `hw.pollJSON` to the full signature first**, add
    `hw.pollText`, default `pauseWhenHidden` to **false**, then adopt per page.
23. **P4** — add the explicit display argument to `hw.show`, add `hw.val` and
    `hw.flip`, widen `hw.on` to `idOrEl`, add the `||''` guard to `hw.setText`.
    Then convert **per file**, leaving `el(`/`$(` as delegating aliases.
24. **P22** — the SSE manager, with the Bond owner migrated and snapshot re-fetch
    on reconnect.
25. **P8** — delete `WebPage_Bond.cpp:723-847` and stream `BondFs`; that alone is
    4,823 B of free flash. The full `hw.peerExec` needs the message-shape and
    credential decisions made first.

### Stage 4 — presentation

26. **P7** (keep `:root`, delete the `data-theme=light` selector), then **P11**
    (which depends on P7's tokens and must live outside the `isPublic` guard),
    **P12**, **P14** (with a `valueHtml` opt-in), **P15** (with `esc` opt-out),
    **P20**, **P21**, **P25**, **P26**, **P29**, **P31**, **P34**, **P35**.
27. **P16** and **P24** last, and rank them on correctness / UX — both are
    byte-neutral or byte-negative. **Sequence P2 before P24**: the two public-page
    `alert()` sites that have no `hw.notify` are inside the dead `revokeMsg`
    handlers P2 deletes, so the blocker disappears for free.

### Stage 5 — the settings surface

28. **P13**, **module by module**, absorbing password rendering, the `disabled`
    concept, the status badge and `sendSequential`, and **keeping the `net-`/`dyn-`
    id prefixes** until change-detection is migrated with them. Risk high; this is
    the settings surface.

### What must be HW-tested regardless of which stages land

This is firmware. The user flashes and tests on a board. **Nothing below can be
established by static analysis.**

| # | Test | Why |
|---|---|---|
| 1 | Every page renders after **P1** | A tokenizer bug inside a raw literal is invisible to the compiler and to `test_embedded_js_syntax.py` for concat-literal files. Load all ~26 pages. |
| 2 | Files / Logging / CLI / Maps / ESPNow under heap pressure after **P5** | The change is about a 49.7 KB internal-DRAM allocation that no longer happens. Measure free internal DRAM during a page load before and after. |
| 3 | Bonded flows after **P17** | Files, Logging, CLI and ESPNow all consume `renderExplorer`, which hard-depends on `FileBrowser`. A wrong split breaks them at runtime only. |
| 4 | The three Speech "does not exist" findings | `ls`, `sr loadwake`, `sr loadcmds`, `/api/upload` are static-analysis conclusions. **60 seconds on hardware confirms or kills them** before anyone rewrites that card. |
| 5 | Dashboard live tiles across a forced SSE drop, after **P22** | Kill WiFi briefly and confirm `sensor-status` and `system` resume without a reload. |
| 6 | `/api/cli` under load after **P33** + **P6** | The 429 path returns **JSON** where callers expect text, and the limiter is process-global across tabs. Test two browser tabs plus the serial CLI simultaneously. |
| 7 | Bluetooth page request rate after the Stage-0 `applyMode` guard | Watch the audit log line rate; the pre-fix symptom was ~3.3 lines/s. |
| 8 | Guest and non-superadmin sessions after **P13** / **P27** | Toggles that previously lied will start surfacing refusals. Confirm the refusal text is readable and the control reverts. |
| 9 | Bond page with a real peer after **P8** | Two remote commands issued in quick succession must not lose or cross their output. |
| 10 | Thermal / camera pages while the Sensors page polls, after **P4** / **P12** | The change-guard removal risk and the `hw.show` display-value risk both show up only as visual glitches under load. |
| 11 | Every board variant | Per the board-gating note in project memory: a green build proves only the **current** board. `#if ENABLE_*` gates hide breaks — build and load pages on each configured board. |

---

*Audit produced from 12 independent page audits, clustered into 35 proposals,
then adversarially re-verified against source. Every status label traces to a
verdict object. All file:line citations are real source lines in
`/Users/morgan/esp/hardwareone-idf/components/hardwareone/`, re-confirmed by grep
at audit time.*
