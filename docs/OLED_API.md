# OLED API Reference

A catalog of the shared OLED functions and the patterns ("the umbrella") that
modes are expected to follow. Sources: `OLED_Utils.h`, `OLED_UI.h`,
`OLED_Display.h`.

---

## How a mode plugs in

Every screen is an `OLEDMode` enum value registered via a module table. The
render loop (`updateOLEDDisplay`) dispatches to the registered `displayFunc`;
input goes to the registered `inputFunc`.

```cpp
static const OLEDModeEntry sMyModes[] = {
  // mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
  { OLED_MY_MODE, "My", "icon", displayMyMode, nullptr, myInputHandler, true, 10, "A:Select B:Back" },
};
REGISTER_OLED_MODE_MODULE(sMyModes, sizeof(sMyModes)/sizeof(sMyModes[0]), "MyModule");
```

- `displayFunc` runs inside the I2C transaction — do **no** blocking work there.
  Gather data in a `prepareXData()` first (called outside the transaction).
- `inputFunc` returns `true` if it consumed the input. Return `false` on **B**
  so the global handler runs `oledMenuBack()` (pops the mode stack).
- Sub-screens are **pushed modes**: `requestOLEDMode(OLED_SUB, "reason")`. Do
  **not** fake sub-screens with `gShowingX` booleans.

---

## The umbrella patterns

1. **Lists/menus → `OLEDScrollState`.** Never hand-roll a raw `int selection` +
   array. Store items in an `OLEDScrollState`; navigate with
   `oledScrollHandleNav`; render with `oledScrollRenderSimple` (single-line) or
   `oledScrollRender` (two-line / split-pane / icon).
2. **Rebuild per frame with `oledScrollClearKeepSelection` + `oledScrollClampSelection`.**
   Plain `oledScrollClear` resets the cursor to 0 — using it on a list that
   rebuilds every frame causes the "snap to top" bug.
3. **Sub-screens are pushed modes**, popped by the global **B** handler.
4. **Text entry → the keyboard overlay** (`oledKeyboardInit` + poll
   `oledKeyboardIsCompleted/IsCancelled`). Draw it with `oledKeyboardDrawIfActive`.
5. **Yes/no → `oledConfirmRequest`** (callback-based overlay).

---

## Function catalog

### Lifecycle / display control
`initOLEDDisplay` · `stopOLEDDisplay` · `oledEarlyInit` · `oledApplySettings` ·
`oledDisplayOn` / `oledDisplayOff` · `oledPrepareForSleep` /
`oledResumeFromSleep` · `oledShowSleepScreen` · `oledSetBootProgress`

### Mode management & render loop
- `requestOLEDMode(mode, reason, pushStack=true)` — the front door for transitions
- `setOLEDMode(mode)` — internal; prefer `requestOLEDMode`
- `pushOLEDMode(mode)` / `popOLEDMode()` — mode stack
- `oledMenuBack()` — returns true if it handled a submenu/back
- `updateOLEDDisplay()` — the per-frame render pump
- `slugFromMode(mode)` / `modeFromSlug(slug)` — CLI slug ↔ enum

### Mode registration
- `struct OLEDModeEntry` — { mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints }
- `REGISTER_OLED_MODE_MODULE(modes, count, name)`

### Dirty / refresh tracking
`oledMarkDirty` · `oledMarkDirtyMode` · `oledMarkDirtyUntil` · `oledIsDirty` ·
`oledClearDirty` · `oledSetAlwaysDirty`

### Scroll list — the shared menu system (`OLEDScrollState`)
- `oledScrollInit(state, title=nullptr, visibleLines=4)`
- `oledScrollAddItem(state, line1, line2=nullptr, selectable=true, userData=nullptr)`
- `oledScrollClear(state)` — clears items **and resets the cursor to 0**
- `oledScrollClearKeepSelection(state)` — clears items, **keeps the cursor** (for per-frame rebuilds)
- `oledScrollClampSelection(state)` — clamp cursor into range after a rebuild
- `oledScrollUp/Down`, `oledScrollPageUp/Down`
- `oledScrollGetSelected(state)` / `oledScrollGetItem(state, i)`
- `oledScrollHandleNav(state, leftRightNav=false)` — consumes up/down (returns true if handled)
- `oledScrollRender(display, state, showScrollbar=true, showSelection=true, footerHints=nullptr)` — 2-line / split-pane / icon list
- `oledScrollRenderSimple(display, state, showSelection=true)` — compact 1-line list ("> " cursor)
- `oledScrollCalculateVisibleLines(height, textSize, hasTitle, hasFooter)` — **note: assumes 16px two-line items**
- `oledScrollSetSplitPane(state, listWidth, separatorX, iconSize=32)`

### Keyboard overlay
- `oledKeyboardInit(title=nullptr, initialText=nullptr, maxLength)`
- `oledKeyboardReset()`
- `oledKeyboardDisplay(display)` / `oledKeyboardDrawIfActive(display)` — the second draws + returns true if active (curtain)
- `oledKeyboardHandleInput(dx, dy, newlyPressed)` — handled centrally by the input pump
- `oledKeyboardIsActive` / `oledKeyboardIsCompleted` / `oledKeyboardIsCancelled`
- `oledKeyboardGetText()`
- `oledKeyboardSetAutocomplete(func, userData)` / `oledKeyboardTriggerAutocomplete` / …
- (internal: `oledKeyboardMoveUp/Down/Left/Right`, `oledKeyboardSelectChar`, `oledKeyboardBackspace`, `oledKeyboardComplete`, `oledKeyboardCancel`, `oledKeyboardToggleMode`, `oledKeyboardAdvance`)

### Confirm dialog
- `oledConfirmRequest(line1, line2, onYes, userData, defaultYes=true)` — **stores the string pointers, does not copy** (pass stable buffers)
- `oledConfirmIsActive()`

### Other UI overlays (`OLED_UI.h`)
- Toast: `oledToastShow/Clear/Active/Render`
- Dialog: `oledDialogOK` / `oledDialogYesNo` / `oledDialogClose/Active/HandleInput/Render`
- Progress: `oledProgressShow/Update/Label/Close/Active/Render`
- Pairing ribbon: `oledPairingRibbon*`
- Umbrella: `oledUIInit` / `oledUIHandleInput` / `oledUIRender` / `oledUIModalActive`

### Content area / text
`oledContentInit/Begin/End` · `oledContentPrint/PrintAt/SetCursor` ·
`oledContentScrollUp/Down/UpdateScroll`

### Header / footer / drawing primitives
`oledRenderHeader` · `oledRenderFooter` · `oledDrawBackArrowIcon` · `oledDrawBox`
· `oledDrawButton` · `oledDrawIcon` · `oledDrawLevelBars` · `oledDrawTextCentered`

### Notifications
`oledNotificationAdd/Count/UnreadCount/MarkAllRead/Clear/Get`

### Command execution (mode → CLI)
- `executeOLEDCommand(cmd)` — fire-and-forget
- `executeOLEDCommandWithResult(cmd, out, size)` — capture output

---

## List/menu fragmentation (state of the code)

There is now effectively **one** list/menu system: `OLEDScrollState`. The two
core menus that used to hand-roll their own model + navigation + render — the
main launcher (`OLED_MENU`) and the sensor menu (`OLED_SENSOR_MENU`) — were
migrated onto it, including their split-pane availability badge/status via the
`OLEDScrollState::rightPaneDraw` callback. When adding or touching a list, use
`OLEDScrollState`.

| Abstraction | Where | Use it? |
|---|---|---|
| `OLEDScrollState` | `OLED_Utils.h` | **Yes** — the standard. Main launcher, sensor menu, Power, Network, Speech, Bluetooth, Logging, Remote, ESP-NOW. |
| `FileBrowserRenderData` | `System_FileManager.h` | Special case — large/paginated directory lists only. |
| raw `int *MenuSelection` + array | UnifiedMenu, Map menu | **No** — remaining legacy hand-rolling; migrate to `OLEDScrollState` when touched. |
| ~~`OledList`~~ | ~~`OLED_UI.h`~~ | **Removed** — was a dormant modal-list overlay superseded by `OLEDScrollState`. |

Shared menu helpers (in `OLED_Mode_Menu.cpp`): `populateMenuScroll()` fills a
scroll state from an `OLEDMenuItem[]` (optionally filtering NOT_BUILT items and
sorting by availability), `menuItemRightPaneDraw()` draws the `D`/`X` badge +
`Ready`/`Off`/`No HW`/`N/A` status beside the icon, and `oledMenuExecuteItem()`
is the shared availability-aware "select" dispatch. The old `OLED_MENU` input
special-case in `processOLEDInput()` is gone — the launcher is now a normal
registered mode (`mainMenuInputHandler`) like every other.

### Mode entry hooks (`OLEDModeEntry::onEnterFunc`)

A mode's "reset on entry" logic lives in one place: a `void onEnterFunc(bool
isForward)` on its `OLEDModeEntry`, invoked centrally by `requestOLEDMode()` when
the mode actually becomes current (mirrors the CLI framework's `onEnter` in
`System_CLIMode.h`). `isForward` is `true` for forward navigation (menu select,
CLI, boot) and `false` for back-navigation (`popOLEDMode()` destinations), so a
mode can reset its view on a fresh visit while preserving state when the user
returns to it. This replaced the per-mode resets that used to be scattered and
duplicated across `cmd_oledmode` and the menu-select path. `nullptr` = no entry
side-effects (the default for most modes).
