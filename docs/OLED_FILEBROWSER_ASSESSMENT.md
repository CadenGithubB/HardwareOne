# ASSESSMENT — "the OLED file viewer should move to OLED utils"

## 1. Is the premise true?

**No — not as stated.** The premise conflates two things that live in the same file and have opposite usage profiles.

**The BROWSER/PICKER is genuinely shared. VERIFIED.** Five entry points, only one of which is ESP-NOW:

| Route | Call site | Mode |
|---|---|---|
| Apps → Files | `OLED_Utils.cpp:5112` | full browser |
| CLI `oledmode files` | `OLED_Utils.cpp:4653` | full browser |
| LLM model picker | `OLED_Mode_LLM.cpp:248` (`oledFilePickerPush`) | picker |
| Maps → Select Map | `OLED_Mode_Map.cpp:1382-1386` | browser |
| ESP-NOW send/receive | `OLED_ESPNow.cpp:1186,1189` | picker / PEER source |

**The VIEWER is single-use.** I traced the only producer of `FbLevel::VIEW` and four of the five routes structurally cannot reach it:

- `OLED_Mode_FileBrowser.cpp:797-804` — `if (sPickerActive) { firePickerCallback(...); return; }` returns *before* the action menu is built. Kills LLM and ESP-NOW-send.
- `:807-822` — `.hwmap` loads the map and `break;`s with the comment "don't also open the action menu". Kills Maps for its actual payload.
- PEER never reaches that switch at all — render `:948-1052` and input `:1299-1351` branch on `FsSource::PEER` first and `return`. Kills ESP-NOW-receive.
- Only `:826-834` sets `ACTION_MENU`, whose "View" row (`fbBuildActionRows` `:192-201`) is the sole path to `LOAD_VIEW` → `VIEW`.

So the viewer is reachable from exactly **Apps→Files and the CLI alias**. It is ~105 lines of 1483 (~7%): buffers `:159-165`, `fbViewVisibleLines` `:171`, `fbIsViewableExt` `:179`, load `:838-877`, render `:1075-1109`, input `:1399-1404`.

**And the shareable part of it was already extracted.** `System_TextPager.h` is the wrap/sanitize/page core; its header comment names OLED, G2 and serial CLI as its three consumer families, and `OLED_Mode_FileBrowser.cpp:17` already includes it, `:855` already calls `textWrapInto`. Six call sites across `G2_Page_Files.cpp:667`, `G2_Page_Settings.cpp:1019`, `G2_Page_ESPNow.cpp:759`, `G2_Glasses.cpp:6722`, `System_Filesystem.cpp:810` and this file. **There is no duplicated file-viewing logic left in the OLED tree to hoist** — I checked `OLED_Mode_Logging.cpp`, `OLED_Mode_CLI.cpp` (console ring buffer, not files), `OLED_SettingsEditor.cpp`, `OLED_Mode_Map.cpp`. None render file text.

Moving "the viewer" to OLED utils would relocate ~105 lines with one caller and leave the five-caller thing where it is. It solves nothing.

## 2. What is the real problem?

Three distinct things, only two of which cost anything.

**(a) The include bug — fixed, but it was a symptom, not the disease.** The fix at `:22-27` is correct: `ps_alloc`/`ps_delete` are used ungated at `:647` and `:1470`, and gated at `:460`/`:596`. What produced it is the *interleaving*: seven `#if ENABLE_ESPNOW` regions woven through a 1483-line file so that "is this line inside the gate?" is not answerable by looking at it. Confirmed boundaries: `:30-42, :360-631, :655-662, :696-706, :948-1052, :1299-1351, :1472-1477` = **468 lines, 31.6% of the file.**

**(b) Genuine mixed-concern coupling — this costs real money, and it is already costing it.** Three verified live defects:

1. **`FsSource::PEER` is ungated.** The enum member `:308` and `cycleSourceForward()` `:330-338` sit outside every `#if`, but the code that services PEER is inside. With `ENABLE_ESPNOW=0`, pressing X twice sets `sCurrentSource = PEER`, `sourceRootPath` returns `nullptr` `:347`, `if (root)` `:679` is false, the compensating `else if` `:697-705` has been preprocessed away, and prepare publishes the **previous directory's** listing `:917-941`. A phantom third source where X does nothing. Not a crash — a shipped defect in a config nobody has compiled.
2. **Maps "Select Map" is clobbered.** `requestOLEDMode(OLED_FILE_BROWSER, "map.browse.maps")` at `OLED_Mode_Map.cpp:1384` fires `onEnterFunc` **synchronously** (`OLED_Utils.cpp:3347-3352`) → `fileBrowserOnEnter(true)` `:1444` sets `oledFileBrowserNeedsInit = true`. *Then* Map calls `navigate("/maps")` `:1385`. Next prepare sees `needsInit` `:752`, runs `initFileBrowser`, and navigates to `sourceRootPath(LOCAL) == "/"` `:694`. **The user lands at root, not /maps.** This is what happens when an external file has to poke `gOledFileManager` through a hand-written extern because there is no API.
3. **`initFileBrowser` `:694` ignores `navigate()`'s return.** `FileManager::navigate` returns false without mutating state on a missing path (`System_FileManager.cpp:40-57`), so `SD_QUICK` with no card leaves a stale listing under a stale breadcrumb. The picker branch `:669-673` handles this correctly with a fallback to `/`; the viewer branch does not.

Two more structural issues in the same family: `oledFileBrowserResetSessionState()` `:218-236` clears every *local* static and **nothing** ESP-NOW — `sEspnowCtx`, `sEspnowCtxMac`, `sPeerPath`, `sCurrentSource` all survive an auth session boundary, so a target MAC chosen by user A is still armed for user B. And `sourceLabel()` `:318-325` is **dead in every configuration** (zero callers repo-wide) while the design comment at `:296` claims "Source indicator appears in the header" — the header builder at `OLED_Utils.cpp:344-368` renders picker title or `Files>path` and never a source letter. **The user has no indication which source is active, in any build.**

**(c) Cosmetic organisation — costs nothing today.** 1483 lines is large but not pathological next to `OLED_Mode_Map.cpp` (2280) or `OLED_ESPNow.cpp` (2543). "This file is big" is not by itself a reason to touch it.

**The one organisational problem that *is* real is the missing header.** The convention here has zero exceptions: `ls OLED_Mode_*.h` → no matches, and nothing anywhere `#include`s an `OLED_Mode_*` header. A leaf mode publishes nothing. But this leaf has an API, so its declarations are scattered across `OLED_Display.h:496-562` (with stale comments naming files that no longer exist — `:495` "defined in oled_display.cpp", `:499` "defined in oled_file_browser.cpp") **plus five hand-written function-local `extern`s**: `OLED_Utils.cpp:350`, `:356`, `:2790`, `:6896`, and `OLED_Mode_Map.cpp:1382-1383`. That last one is the one that produced defect (2).

## 3. Recommended shape

**Do not move the viewer. Do not move anything into `OLED_Utils.cpp`.**

`OLED_Utils.cpp` is 7214 lines, is the only unconditionally-compiled OLED TU (`CMakeLists.txt:228-233`), and contains **27** `// X moved to OLED_Mode_Y.cpp` tombstones — five of them about this exact feature, at `:6698-6718`. The file browser was already in `OLED_Utils.cpp` and was deliberately moved out. Moving it back reverses the only documented direction of travel in this subsystem.

### Tier 0 — do this regardless, ~30 lines, no files move

1. Move `FsSource::PEER` `:308` and its `cycleSourceForward` wrap `:334` inside `#if ENABLE_ESPNOW`. Fixes the phantom source.
2. Add `bool oledFileBrowserOpenAt(const char* path)` and convert `OLED_Mode_Map.cpp:1382-1386` to call it. Fixes the /maps clobber and removes the worst extern.
3. Have `initFileBrowser` `:694` fall back to `/` on `navigate()` failure, matching the picker branch `:671`.
4. Extend `oledFileBrowserResetSessionState()` `:218` to clear the ESP-NOW statics.
5. Either wire `sourceLabel()` into the header builder or delete it and the comment at `:296`.

This captures most of the actual value at ~2% of the risk of a split.

### Tier 1 — the split that is actually warranted (later, see §6)

Peel the ESP-NOW half out, not the viewer:

```
OLED_FileBrowser.h/.cpp       browse + picker + viewer  (~800, ENABLE_OLED_DISPLAY)
OLED_FileBrowserPeer.cpp      peer browse/transfer      (~460, + ENABLE_ESPNOW)
OLED_Mode_FileBrowser.cpp     registry shell + input    (~180, KEEP)
System_FileManager.h/.cpp     FS model                  (exists, unchanged)
System_TextPager.h            wrap core                 (exists, unchanged)
```

The name drops `Mode_` because it gains a header — that is the actual convention (Axis B, zero exceptions). The precedent is exact: `OLED_RemoteSettings.cpp/.h` (308 lines, `ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE`) + `OLED_Mode_RemoteSettings.cpp` (167 lines, includes the header at `:6`). The new header absorbs the five function-local externs and the `FilePickerRequest` block currently renting space at `OLED_Display.h:509-565`, and the two orphaned globals `gOledFileManager`/`oledFileBrowserNeedsInit` (`OLED_Utils.cpp:6695-6696`) come home next to their `ps_alloc` `:647` and `ps_delete` `:1470` — which is the exact split that put the `System_MemUtil.h` include on the wrong side of a gate.

Do **not** fold the peer half into `OLED_ESPNow.cpp` — at 2543 lines its scope per `:23-24` is mesh peer *status* rendering, not file transfer.

## 4. Coupling to resolve

Good news: the two halves are far more disjoint than the file's shape suggests. Verified by grep — the PEER regions never touch `gOledFileManager` or `fileBrowserRenderData`, and the local path never touches `sPeer*`/`sEspnowCtx*`/`sGetUi`. Seven real seams:

| # | Coupling | Handling |
|---|---|---|
| 1 | `FsSource` enum `:305` + `sCurrentSource` `:311` + `cycleSourceForward` `:330` + `sourceRootPath` `:343` — **primary coupling**; `PEER` returns `nullptr` `:347` purely so init falls into the gated branch | Keep the enum in the core TU with `PEER` gated; expose `fbSourceIsPeer()` accessor. Do not invent a general "source provider" interface for two implementations. |
| 2 | Three dispatcher prefixes: `initFileBrowser` `:696-706`, `displayFileBrowserRendered` `:950-1051` (`return;` `:1050`), `fileBrowserInputHandler` `:1302-1350` (`return true;` `:1349`) | Pure prefix-dispatch — replace with a `bool fbPeerHandleInit/Render/Input()` trio, weak-defaulted to `false` when ESPNOW is off. Cheapest part of the job. |
| 3 | `oledFileBrowserNeedsInit` written by both halves (`:336`, `:613`, `:709`, `:1446`) and defined in a third file (`OLED_Utils.cpp:6696`) | Move the definition into `OLED_FileBrowser.cpp`; both halves keep writing it via the header. |
| 4 | Picker ↔ source, two-way: `cycleSourceForward` reads `sPickerActive` `:331`; picker is used by LLM `:248` *and* by ESP-NOW send `:627` | Picker stays in the core TU. The peer TU calls `oledFilePickerPush` like any other consumer. |
| 5 | `ps_alloc`/`ps_delete` split across the gate — `:647`/`:1470` ungated vs `:460`/`:596` gated | Solved automatically by the split: each TU includes `System_MemUtil.h` unconditionally. |
| 6 | `oledToastShow` from local prepare (`:772`, `:871`, `:886`) and from **BTC_TASK** ESP-NOW callbacks (`:520-525`, `:585`) | Already thread-safe per `:6`. No change, but do not "simplify" it during the move. |
| 7 | `resetOLEDFileBrowser()` `:1466` clears both domains (`ps_delete` + `sEspnowCtx`/`sGetUi` `:1475-1476`) | Zero callers repo-wide. Delete it, along with `oledFilePickerIsActive` (`OLED_Display.h:562`) and `oledFileBrowserUp/Down/Select/Back` (`:500-503`, in-file callers only). The true external contract is **five symbols, not fifteen.** |

## 5. Cost and risk

**Size:** ~460 lines move to a new TU, ~180 stay in the shell, ~800 rename-in-place, one `list(APPEND)` at `CMakeLists.txt:~367`, one new header, five function-local externs deleted, ~10 declarations relocated out of `OLED_Display.h`. Realistically a half-day of careful work plus a hardware pass.

**What could break:** the `#if ENABLE_BONDED_MODE` at `:478-481` is a **partial-branch guard nested inside an `if/else if/else` chain** — the `} else if (BondedPeer::isPaired()...) {` text is itself the gated content. That is the single most fragile preprocessor construct in the file and the most likely thing to be silently mis-cut during a move.

**The invisibility problem, and it is severe:**

- `System_BuildConfig.h:153` is `#define DISPLAY_TYPE 0`. `CMakeLists.txt:149-152` therefore sets `HW_CFG_BUILD_OLED_MODES 0` and drops **every** `OLED_*.cpp` except `OLED_Utils.cpp` from the build. **A green default build proves literally nothing about this work.** Every verification pass must run against a display-enabled board config.
- There is no `HW_CFG_ENABLE_ESPNOW` — grep of `CMakeLists.txt` for `ESPNOW` returns one hit, a *comment* at `:148`. `ENABLE_ESPNOW` is derived from `NETWORK_FEATURE_LEVEL` inside the header (`System_BuildConfig.h:515-527`), invisible to CMake. So `OLED_ESPNow.cpp` `:353`, `OLED_RemoteSettings.cpp` `:376` and `OLED_Mode_RemoteSettings.cpp` `:372` are listed unconditionally and rely entirely on in-file `#if` to collapse to empty TUs. **The new peer TU would be the same.** The split makes the ESPNOW=0 failure mode "one whole TU compiles or doesn't" instead of "seven interleaved gates each of which can drift" — but it does *not* by itself make ESPNOW=0 build.
- The proving matrix is 4 configs: {OLED on, OLED off} × {ESPNOW on, ESPNOW off}. Three of the four are currently unproven.

## 6. Sequencing — **later, not now, and not next**

Plainly: **do not start this until the tree is clean.**

- `git status --porcelain` reports **230 entries** right now, spanning wizard debug flags, ring reassembly, build-gate fixes, the LittleFS fix, a partition change, and FTS fixes.
- You are mid-way through wizard-unification Step 1.
- This change is a cross-file move touching `OLED_Display.h`, `OLED_Utils.cpp`, `OLED_Mode_Map.cpp`, `CMakeLists.txt` and two new files, in a subsystem the default build does not compile, validated only on hardware.

A refactor whose only honest verification is "flash it and click through Files, Maps, LLM picker and ESP-NOW send/receive" cannot be bisected out of a 230-entry working tree if something regresses. And it competes directly for the same files the wizard work is in.

**Ordering:**

1. **Now:** nothing from this assessment except, at most, Tier 0 item 1 (gate `FsSource::PEER`) if you are actively chasing the `ENABLE_ESPNOW=0` compile chain — it is three lines and belongs with the include fix you just made.
2. **Next, after the current tree lands and is hardware-tested:** the rest of Tier 0. ~30 lines, three real user-visible bug fixes, no file moves, no CMake change, low review cost.
3. **Later, as its own branch with nothing else in it:** the Tier 1 peel, with the 4-config build matrix run before and after.

The owner's instinct that "this is doing too much" is correct. The instinct about *which* part to move is wrong — the viewer is the smallest, most single-purpose thing in the file, and its shareable core already shipped as `System_TextPager.h`. The 468-line ESP-NOW peer-source state machine is the foreign body.