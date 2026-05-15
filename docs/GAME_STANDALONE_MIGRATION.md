# Standalone Game → WebPage_Games.h Migration Plan

## Goal

Replace the inner CSS/HTML/JS payload of [`components/hardwareone/WebPage_Games.h`](../components/hardwareone/WebPage_Games.h) with the contents of [`game_standalone.html`](../game_standalone.html) so the firmware's `/games` route serves the newer, better version of Tilt Maze. Keep all firmware plumbing (auth, nav shell, build flags, URI registration) untouched.

## Current state

**File A — `components/hardwareone/WebPage_Games.h`** (11,214 lines)
- Exposes `streamGamesInner(httpd_req_t* req)` consumed by [`WebPage_Games.cpp`](../components/hardwareone/WebPage_Games.cpp).
- Inner content is split into **3 raw-string chunks** sent via `httpd_resp_send_chunk`:
  | Chunk | Delimiter | Lines | Content |
  |---|---|---|---|
  | CSS | `R"CSS(...)CSS"` | 21–29 | `.games-wrap`, `.row`, `.col`, `.card-light`, `.hud`, `canvas#maze` |
  | HTML | `R"HTML(...)HTML"` | 32–117 | Controls panel: start/stop, terrain `<select>`, view-mode buttons, debug checkboxes |
  | JS  | `R"JS(...)JS"`    | 118–11199 | Game logic, rendering, IMU polling, gamepad |
- After the JS chunk a final `</div>` is sent (line ~11202).
- Wrapper [`WebPage_Games.cpp:13-17`](../components/hardwareone/WebPage_Games.cpp) calls `streamBeginHtml(... "games")` → `streamGamesInner` → `streamEndHtml`, which provides the shared nav, user banner, and global CSS (see `streamCommonCSS()` in [`WebServer_Utils.h:979-1050`](../components/hardwareone/WebServer_Utils.h)).

**File B — `game_standalone.html`** (22,517 lines)
- Self-contained: `<!DOCTYPE>`, `<head><style>`, `<body>`, inline `<script>`.
- Sections:
  | Section | Lines | Notes |
  |---|---|---|
  | `<style>` | 7–25  | First 12 rules (lines 8–19) **duplicate firmware-shell CSS** (`:root`, `.btn`, `.input-tall`, `.btn-row`, `.text-sm`, `.space-*`, `body`). Last 5 rules (lines 20–24) are the game-specific styles that match File A's CSS chunk. |
  | `<body>` | 27–207 | Same `<div class='games-wrap'>` skeleton, **plus** new `cavetest` terrain option and a `#caveTestOptions` panel with 8 toggles (lines 44, 53–79). |
  | `<script>` | 208–22515 | Roughly 2× the JS of File A. Adds cave-test panel wiring, extra debug toggles, expanded sprite builders. |
- No external `<script src>` / `<link>` — fully self-contained.

## What needs to change

**Migration is mostly mechanical chunk-swapping**, with one real care point: stripping the standalone's CSS overrides so it doesn't fight the firmware's global theme.

### Required edits in `WebPage_Games.h`

1. **Replace CSS chunk** (lines 21–29).
   Copy **only lines 20–24** of `game_standalone.html` (`.games-wrap`, `.row`, `.col` + `.card-light`, `.hud`, `canvas#maze`) into the `R"CSS(...)CSS"` body.
   **Drop** lines 8–19 of the standalone (`:root`, `body`, `.btn`, `.btn-small`, `.btn-row`, `.input-tall`, `.text-sm`, `.text-muted`, `.space-*`). These are already provided by `streamCommonCSS()` and using the standalone versions would override the user's theme, break dark/light mode, and visually break other pages if any rule leaks.

2. **Replace HTML chunk** (lines 32–117).
   Copy the `<div class='games-wrap'>...</div>` block from `game_standalone.html` lines 28–206 (everything inside `<body>` *before* the `<script>` opens at line 207). Wrap unchanged in `R"HTML(...)HTML"`. The new `#caveTestOptions` panel and `cavetest` terrain `<option>` come along for free.

3. **Replace JS chunk** (lines 118–11199).
   Copy the contents of `game_standalone.html` between `<script>` (line 208) and `</script>` (~line 22515) — **the script tags themselves stay out**; only the JS body goes inside `R"JS(...)JS"`. Trailing `</div>` chunk after the JS stays as-is.

### No changes needed in

- `WebPage_Games.cpp` — wrapper, auth, URI handler, `ENABLE_WEB_GAMES` gate are all correct.
- `WebServer_Utils.h` / shared shell.
- Build config / `Kconfig` / `sdkconfig` — the page is gated by `ENABLE_WEB_GAMES` and `ENABLE_GAMES` which already exist.
- API endpoints — the standalone calls only endpoints the firmware already serves: `/api/sensors?sensor=imu`, `/api/sensors?sensor=gamepad`, `/api/sensors/status`, `/api/system`, `/api/cli`, `/api/cli/logs`. No new server work.

## Step-by-step procedure

1. **Snapshot the current file** (`git status` already shows `main` is clean here aside from untracked docs/sdkconfig artifacts — current `WebPage_Games.h` is committed).
2. Open `game_standalone.html` and identify the three section boundaries:
   - CSS body start/end: between `<style>` (line 7) and `</style>` (line 25).
   - HTML body: between `</head>`/`<body>` (line 27) and the `<script>` open (line 207).
   - JS body: between `<script>` (line 208) and `</script>` (~line 22515).
3. Edit `WebPage_Games.h`:
   - Inside the `R"CSS(...)CSS"` chunk, paste only the game-specific rules (`.games-wrap`/`.row`/`.col`/`.card-light`/`.hud`/`canvas#maze`).
   - Inside the `R"HTML(...)HTML"` chunk, paste the new body markup.
   - Inside the `R"JS(...)JS"` chunk, paste the new JS.
4. Run `idf.py build` for both `esp32` and `esp32s3` targets to confirm:
   - No raw-string delimiter collisions (none expected — see Footguns).
   - No compiler RAM/flash overflow from the JS roughly doubling.
5. Flash to a device, visit `/games`, run through the smoke checklist below.
6. Once verified, delete (or move out of repo root) `game_standalone.html` — it has served its purpose and shouldn't live next to `main.cpp`.

## Footguns

- **Raw-string delimiter collisions.** The standalone content was grepped for `)CSS"`, `)HTML"`, `)JS"`, `)RAW"` — none found. If a future edit introduces one, change that specific chunk's delimiter (e.g. `R"JS2(...)JS2"`).
- **CSS shadowing.** Don't paste the standalone's `:root`/`.btn`/`.input-tall`/`body` rules. They will visually break the nav bar and any other page that happens to render after the games CSS is cached. The firmware shell already supplies these via `streamCommonCSS()`.
- **Flash budget.** JS roughly doubles (~11k → ~22k lines). The chunk is sent via `httpd_resp_send_chunk` so there's no in-RAM streaming concern, but the raw string literal lives in flash. Verify both `.elf` sizes fit after build — particularly on `esp32` (smaller flash partition than `esp32s3`). If tight, the JS chunk is the obvious minify target.
- **HMR / browser cache.** After flashing, hard-reload the page (`/games?nocache=1` or Cmd-Shift-R). Captive-portal / browser caches often hold the previous page indefinitely.
- **Endpoints differ between targets.** The standalone references `/api/sensors?sensor=imu` and `/api/sensors?sensor=gamepad`. On an `esp32` build without the gamepad seesaw wired up, the gamepad poll will 404/return empty — confirm the standalone's fallback handles that gracefully (it should — File A handles it today).
- **Auth.** `/games` is already gated by `tgRequireAuth(ctx)` in `WebPage_Games.cpp`. The standalone HTML running in a browser bypasses auth because it has no shell — that's fine, only matters when served from the device.

## Verification checklist after migration

- [ ] `idf.py build` succeeds on both `esp32` and `esp32s3`.
- [ ] `.elf` flash size delta noted; no partition overflow.
- [ ] `/games` loads with shared nav and user banner intact (= firmware shell is still wrapping the page).
- [ ] Page background/buttons/inputs follow the user's theme (= cosmetic CSS overrides successfully stripped).
- [ ] Terrain selector includes `Cave Test`; selecting it reveals the 8-checkbox panel.
- [ ] Start → IMU polling begins; HUD updates.
- [ ] 2D view, 3D view, Overview, Endless Mode, Toggle Textures all work.
- [ ] Gamepad checkbox works on a build that has the seesaw gamepad; gracefully degrades on builds that don't.
- [ ] Browser console shows no 404s for endpoints or assets.

## Rollback

Single-commit migration → `git revert <sha>` returns the previous embedded game. Keep the migration as one commit so revert is trivial.
