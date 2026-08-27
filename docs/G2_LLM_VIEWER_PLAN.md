# G2 LLM Viewer — Design Plan

A read-only, streaming LLM view on the Even Realities G2 lens. No keyboard on the
glasses; prompts originate elsewhere (a tap-selected preset, or a web/CLI/voice
generation started on another surface). Layout: a control list on the left 1/3, a
live chat pane on the right 2/3, showing **both** the user's prompt and the model's
streaming reply — token-by-token, like the web and OLED surfaces.

Status: **plan + decision pending** (see "The one decision" below). Uncommitted.

---

## How the existing surfaces do it (shared core)

All LLM UIs are thin readers over one model: the `llm_gen` task (core 1) appends
tokens to a bounded buffer; consumers **offset-poll** it. Starting a generation is
always `chatBeginTurn(prompt, …)` — the same call from web, OLED, and CLI.

- **OLED** (`OLED_Mode_LLM.cpp`): re-reads the whole turn each frame via the chat
  layer, hard-wraps to 21 cols, keeps the newest 12 lines, forces scroll-to-bottom
  while generating (follow-the-tail) with a blinking cursor; `B`=stop. Its liveness
  is *incidental* — it only re-renders when something else marks the OLED dirty, so
  a G2 worker must drive its **own** fixed cadence.
- **Web** (`WebPage_LLM.cpp`): `POST /api/llm/generate` → `chatBeginTurn` → session
  id; then a self-rescheduling **150 ms** poll of `/api/llm/result?session&offset`
  appends the delta and advances `offset`; done = `!chatIsGenerating()`; an instant
  one-liner is recovered via `chatReadFinished`. Server keeps no per-client state,
  so multiple readers can follow the same generation.

## The read seams (pick per need)

| Seam | Call | Props |
|---|---|---|
| **Engine** (lock-free) | `llmGetResultChunk(off,buf,len)` + `llmGetResultLen()` + `llmIsGenerationDone()` + `llmGetSessionId()` (`System_LLM.cpp:2327`) | 8 KB buffer, publish-after-write → **no mutex** from core 0; tail persists after done. Raw assistant text only (no turn structure). `cmd_llm_result` (`System_LLM.cpp:2574`) is the reference loop. |
| **Chat** (mutexed) | `chatReadTurn(i,off,…)` / `chatGetTurnCount()` / `chatReadStream` / `chatReadFinished(session,…)` / `chatIsGenerating()` / `chatGetSessionId()` (`System_LLMChat.cpp`) | Per-turn (USER + ASSISTANT), ~2 KB/turn. Needed to show **the user's prompt**. `chatReadStream` returns 0 the instant a turn finalizes → must pair with `chatReadFinished` for the tail. |

**We want the chat seam** (the user wants to see their prompt turns), reading the
current turn incrementally for the live feel, with the `chatReadFinished` fallback.

## Architecture — new `G2_Page_LLM.cpp`

- Register `G2_HIJACK_PAGE_LLM` (enum has room to 16); add an "LLM" row to the Apps
  menu (`g2ShowAppsMenu` items[] + `g2AppsHandleTap` case), gated behind the LLM
  build flag (mirror `ENABLE_MAPS`); register a `G2PageModule` in `registerG2Pages`.
- **Compound container** (list + text) via `g2BuildCreateMixedListText` /
  `sendCreateMixedListTextAndWait` (`System_G2_Protocol.cpp:1169`): a LIST child on
  the left (control rows) + a TEXT child named e.g. `"chat"` on the right. The
  builder honors the passed geometry for both children (`writeTextChildSpec`).
- **Streaming = REBUILD only the text child** by name via `Cmd=7` REBUILD_PAGE
  (`g2BuildRebuildText(seq,magic,"chat",content,…)` + `armRebuildSlot`/
  `waitRebuildAck` + `sendEnvelope`), leaving the list focus intact. This exact
  child-only REBUILD is proven by `g2ProbeRebuildTextChild` (`G2_Glasses.cpp:9239`).
- **Chat pane content**: build `"You: <prompt>\nAI: <assistant so far>"` from the
  recent chat turns; show the **tail** that fits the pane (follow-the-tail, like the
  OLED's newest-lines) and re-REBUILD as the assistant turn grows. Ring scroll pages
  back; double-tap exits.
- **Control list (left 1/3)**: `<- Back`, `Re-run last`, and (Phase 2) a few preset
  prompts. Row tap → start generation, then the pane follows the new session.

## Prompt input (no keyboard)

`chatBeginTurn(prompt)` is the shared start; it **refuses** (returns 0) if a
generation is already running, so a preset tap can't kill someone else's gen.

1. **Read-only follow** — open the page while a web/CLI/voice gen is running; just
   poll and render. Zero new UX. (Phase 1)
2. **Preset list** — tap a canned row → `g2SubmitHijackCommand("llmgenerate json
   <canned>")` (runs on `cmd_exec_task` under the glasses user's auth) → follow.
   (Phase 2)
3. **Re-run last** — one row → `chatRetryLast`. (Phase 1/2)
4. **Voice** — a mapped voice phrase can fire a preset command; ESP-SR is
   fixed-vocabulary (not dictation), so voice == a canned preset. (Later)

## The one decision — how to render the right 2/3 chat pane

The proven side-by-side "big right pane" (map/camera) uses an **image** tile;
native **text** children are only proven at the **status-bar** placement. The
protocol *can* express an arbitrary text-child geometry, but the firmware's render
of a side-placed TEXT widget is **untested**.

- **Option A — native text child, side geom (recommended).** Best fit: crisp native
  text, cheap `Cmd=7` live-append, exact 1/3 + 2/3 layout. Risk: firmware may force
  text to status-bar/full-width. Validate on first flash; if it won't side-place,
  fall back to B (a one-file change) or accept a top-bar text layout.
- **Option B — image-rendered chat pane.** Render the chat text into a 4-bpp BMP and
  push it as an image tile on the right (exactly the map/camera path). Guarantees the
  side-by-side layout, but each update re-pushes a bitmap (~2–4 fps, heavier BLE) and
  needs a text→bitmap renderer. "Live append" becomes frame-paced, not native append.

## 60 s hijack-timeout policy (must be explicit)

The hijack watchdog force-exits after 60 s unless `gHijackStartedMs` is refreshed on
a **genuine tap**. Refreshing it from token pushes is the documented `ack != presence`
footgun. Policy: **refresh `gHijackStartedMs` from the authoritative engine-busy
signal** (`chatIsGenerating()`) *while generating* — device-side, bounded by the
hard-cap/sentence-limit, so it's safe — then **stop** the instant it's done and let
the normal tap-only rule govern passive reading. Plus a worker hard cap (~5 min, like
the camera stream's `kStreamSafetyCapMs`).

## Memory & task plan (applying the map-page lesson)

- Text-only worker does **no** map/JPEG work → stack **16 KB** (`4096` words, like
  `liveTextWorker`), NOT the map's 32 KB.
- Accumulation + wrapped buffers in **PSRAM** (`EXT_RAM_BSS_ATTR` / `ps_alloc`).
- Heap guard before `xTaskCreate` (mirror `g2ShowMapPage`: require stack + ~8 KB
  internal, decline gracefully).
- Never call `chatBeginTurn`/CREATE/REBUILD inline from the BLE notify callback
  (~3–4 KB `BTC_TASK` stack) → defer to `g2_tap_disp` (25 KB) via
  `tapDispatcherEnqueue`.

## Footguns
8 KB output ceiling (page within it) · single global gen → latch `llmGetSessionId()`,
reset offset on change, treat takeover as terminal · UTF-8 chunk boundaries →
append-then-render · duplicate L+R gestures → dedup · **text-in-text only** (REBUILD
text into a *list* container crashes the plugin) · `SYSTEM_EXIT`/foreground-overlay
must stop the worker (resurrection loop).

## Phases
1. **Read-only viewer + Apps entry**: compound list+text, follow current/last gen,
   live-append the chat pane, engine-busy timeout + hard cap, heap guard, teardown
   hooks. Control list = `<- Back` + `Re-run last`.
2. **Preset prompts**: add canned rows → `g2SubmitHijackCommand`.
3. **Later**: voice-phrase presets, R1-ring niceties, model picker.

## Reuse map
- `G2TextPager` + `g2TextPagerRender` (`G2_Page_Common.h`) — wrap/paginate.
- ESPNow chat page (`G2_Page_ESPNow.cpp` `chatBuildPages`/`chatRenderPage`) — buffer
  rebuild + auto-follow template.
- `g2BuildCreateMixedListText` / `sendCreateMixedListTextAndWait` — the compound.
- `g2ProbeRebuildTextChild` (`G2_Glasses.cpp:9239`) — the child-only REBUILD recipe.
- `g2MapPageWorker` (`G2_Glasses.cpp:14240`) + `g2ShowMapPage` guard — worker skeleton.
- `liveTextWorker` / `g2StartLiveTextPage` (`G2_Glasses.cpp:10130`) — live-append base.
- `cmd_llm_result` (`System_LLM.cpp:2574`) — the engine read loop.
