# OLED Menu Unification — Plan

Goal: collapse the **three** OLED list/menu implementations into one shared
primitive (`OLEDScrollState`) so the menu system is consistent to use and cheap
to extend. This is the capstone of the broader "umbrella" effort (Power,
Network, Speech, Bluetooth, etc. already use `OLEDScrollState`).

---

## 1. Why there are three systems today

| System | Where | What's special |
|---|---|---|
| **Main app menu** (`OLED_MENU`) | render `displayMenuListStyle()` (OLED_Mode_Menu.cpp:135); nav `oledMenuUp/Down/Select/Back` (OLED_Utils.cpp:5153/5180/5247/5209); data `oledMenuCategories[]`+`oledMenuCategory0..5[]` (OLED_Utils.cpp:4299+); dynamic remote `gRemoteSubmenuItems` (OLED_Utils.cpp:4456) | Hierarchical (categories → items → remote submenu), dynamic remote-peer aggregation, availability badges, **dispatched by a special-case in `processOLEDInput` (~OLED_Utils.cpp:5991), not a registered mode** |
| **Sensor menu** (`OLED_SENSOR_MENU`) | render `displaySensorMenu()` (OLED_Mode_Menu.cpp:346); nav `sensorMenuInputHandler()` (OLED_Mode_Menu.cpp:456, registered); data `oledSensorMenuItems[]` (OLED_Utils.cpp:4402) | Filter+sort by availability (`sortSensorMenu`, OLED_Mode_Menu.cpp:63), availability badges |
| **`OLEDScrollState`** | OLED_Utils.h:151 + `oledScroll*` (OLED_Utils.cpp:845+) | The modern shared flat-list; used by every converted mode |

**Root cause:** `OLEDScrollState` came later. The two core menus predate it and
have richer needs that were never migrated. Crucially, the *render layout is
nearly identical* — both core menus are split-pane (text list + preview icon),
which is exactly what `OLEDScrollState`'s split-pane mode produces. The real
divergence is in **navigation, dispatch wiring, and the right-pane badge/status**
— not the fundamental layout. So this is tractable.

---

## 2. Target architecture

One menu = one registered OLED mode that owns an `OLEDScrollState`. Specifically:

- **One model:** `OLEDScrollState` (items, selection, scroll, clamp).
- **One renderer:** `oledScrollRender()` split-pane (extended to draw the badge/status).
- **One nav:** `oledScrollHandleNav()` (clamps — already the default after the no-wrap change).
- **One dispatch wiring:** every menu registered via `REGISTER_OLED_MODE_MODULE` with an `inputFunc`. **Delete the `OLED_MENU` special-case** in `processOLEDInput`.
- **One action model:** a shared `oledMenuExecuteItem(const OLEDMenuItem*)` that does availability-check → `requestOLEDMode` / `executeOLEDCommand` / enter-submenu / unavailable-page. (Today each menu hand-rolls this.)

The bridge already exists: `OLEDScrollItem` has `line1` (name), `iconName`
(icon), and `userData` (opaque). Point `userData` at the existing
`OLEDMenuItem`/`OLEDMenuItemEx` descriptor — no new item struct needed.

---

## 3. Gap analysis — what `OLEDScrollState` must gain

`OLEDScrollState`'s split-pane render currently draws **only the icon** in the
right pane (OLED_Utils.cpp ~1114). The core menus draw more. To render them:

- **G1 — Right-pane status/badge.** Availability badge (`D`/`X`/`R`) + status text (`Ready`/`Off`/`No HW`/`N/A`/`Remote`) (OLED_Mode_Menu.cpp:257-326). **Add** an optional per-item `statusText`/`badge`, **or** a `void(*rightPaneDraw)(Adafruit_SSD1306*, const OLEDScrollItem*, int areaX)` callback on `OLEDScrollState`. Callback is the cleaner choice (keeps availability logic out of the primitive).
- **G2 — Item decorations.** `" >"` suffix for categories, `"R "` prefix for remote items (OLED_Mode_Menu.cpp:229-241). Bake into `line1` at populate time (simplest) — no primitive change needed.
- **G3 — Shared action dispatch.** `oledMenuExecuteItem(const OLEDMenuItem*)`: availability via `getMenuAvailability`, then `requestOLEDMode` / `executeOLEDCommand("remote:…")` / submenu / `enterUnavailablePage`. Replaces the bespoke select logic in `oledMenuSelect` (OLED_Utils.cpp:5247) and `sensorMenuInputHandler` (OLED_Mode_Menu.cpp:475).
- **G4 — Shared availability-aware populate.** `populateMenuScroll(OLEDScrollState*, const OLEDMenuItem* items, int count, bool sortByAvailability, bool dropNotBuilt)` — encapsulates the filter+sort (OLED_Mode_Menu.cpp:63) and the `line1`/`iconName`/`userData` wiring.
- **G5 — Hierarchy.** The main menu is multi-level. Two options:
  - **(a) Level-state + repopulate** *(recommended)*: keep `OLED_MENU` as one mode with a small `level`/`categoryIndex` state; repopulate the shared `OLEDScrollState` when the level changes. Lowest risk; removes the duplicate scroll model + nav + 3 wrap implementations without restructuring into many modes.
  - **(b) Pushed sub-modes per level**: more "umbrella", but categories are data-driven (6 of them) so it'd need a parameterized category mode. More invasive. Not recommended for the first pass.
- **G6 — Item cap.** `OLED_SCROLL_MAX_ITEMS = 32` vs `MAX_DYNAMIC_MENU_ITEMS = 32`. The remote submenu can hit exactly 32 — **verify or raise** `OLED_SCROLL_MAX_ITEMS` before converting the remote submenu.

Already covered by `OLEDScrollState` (no work): split-pane icons, two-line
items, `userData`, scrollbar, clamp/keep-selection, wrap toggle.

---

## 4. Behaviors that MUST survive the conversion

These live in the `OLED_MENU` special-case and are easy to drop accidentally:
- **START cycles the data source** (`oledCycleDataSource`, OLED_Utils.cpp ~6063) — LOCAL/REMOTE/BOTH.
- **Remote command keyboard** (`gRemoteCommandInputActive`, ~5992-6021) — text entry for remote commands that need input (`needsInput`).
- **Dynamic remote aggregation** (`buildRemoteSubmenu`) — querying the bonded peer; only the scroll *model* changes, the build logic stays.
- **SELECT → Quick Settings** global shortcut.

---

## 5. Phased plan (build + HW-test each phase — these menus ARE compiled in)

**Phase 0 — Extend the primitive (additive, low risk).**
- Add the right-pane status callback (G1) to `OLEDScrollState` + `oledScrollRender`.
- Add `oledMenuExecuteItem()` (G3) and `populateMenuScroll()` (G4) as shared helpers (OLED_Utils).
- Verify `OLED_SCROLL_MAX_ITEMS` (G6).
- Build; confirm existing `OLEDScrollState` users are unaffected.

**Phase 1 — Sensor menu (the proof-of-concept, low risk).**
- Replace `sensorMenuSortedIndices` + `displaySensorMenu` + the hand-rolled nav with: `populateMenuScroll` (sort/filter) → `oledScrollRender` split-pane (with status callback) → `oledScrollHandleNav` → `oledMenuExecuteItem`.
- It's already a registered mode, so no dispatch change.
- Build + **HW-test the sensor menu** (scroll, icons, badges, select, back).

**Phase 2 — Main menu (the heavy lift).**
- Move `displayMenu` + a new `mainMenuInputHandler` into `OLED_Mode_Menu.cpp`; **register `OLED_MENU`** and **delete the `processOLEDInput` special-case** (~5991-6075), porting START/remote-keyboard/SELECT behaviors into the handler.
- Replace `oledMenuSelectedIndex` / `oledMenuCategorySelected` / `oledMenuCategoryItemIndex` with one `OLEDScrollState` + a `level`/`categoryIndex` state (G5a); repopulate on level change.
- Dispatch via `oledMenuExecuteItem`.
- Build + **HW-test the whole launcher** (every category, back navigation, availability, the START toggle). Highest-risk phase.

**Phase 3 — Remote submenu.**
- Populate the shared `OLEDScrollState` from `gRemoteSubmenuItems` (keep `buildRemoteSubmenu`); confirm the 32-item cap (G6).
- Build + **HW-test with a bonded peer** (or at least confirm it compiles + renders empty/local).

**Phase 4 — Cleanup.**
- Delete `oledMenuUp/Down/Select/Back`, the now-dead nav/wrap/modulo code, the special-case dispatch, and the orphaned state vars.
- Update `docs/OLED_API.md` (the list-fragmentation table → "one system").

---

## 6. Risks & mitigations

- **The launcher is *the* front door.** Breaking it breaks the whole device UI. → Phase it; HW-test each phase; keep the old code in git history for fast revert; do Phase 2 only after Phase 0/1 are proven.
- **Untestable remote paths.** The dynamic remote submenu needs a bonded peer. → At minimum compile-validate + verify the local/empty path; defer full remote testing.
- **Badge/status parity.** The new render callback must reproduce the exact `D/X/R` + `Ready/Off/No HW` look. → Port the logic verbatim from OLED_Mode_Menu.cpp:257-326 into the callback.
- **Pointer lifetime.** `OLEDScrollState` stores label pointers (no copy). Static arrays + the persistent `gRemoteSubmenuItems` buffer are fine; never point at stack temporaries.
- **Item cap.** Confirm 32 is enough for the largest level (remote submenu).

---

## 7. Effort & recommendation

- **Phase 0 + 1** (primitive + sensor menu): moderate, low-risk, high-value — this is the proof that the model fits. Do this first.
- **Phase 2** (main menu): the real lift; do it as its own focused, HW-tested pass.
- **Phase 3-4**: incremental cleanup.

Do **not** do it all at once. Each phase is independently shippable and HW-testable
(unlike the off-by-default Speech/Bluetooth work). The end state: one menu
primitive, one render, one nav, one dispatch, one action model — a menu system
you extend by populating an `OLEDScrollState`, nothing more.

---

## Appendix — key file/symbol map

- `OLEDScrollState` / `OLEDScrollItem` — OLED_Utils.h:151 / :140
- `oledScrollRender` (split-pane icon at ~1114) / `oledScrollRenderSimple` — OLED_Utils.cpp:1035 / :1199
- `oledScrollHandleNav` / `oledScrollSetSplitPane` / `oledScrollClearKeepSelection` / `oledScrollClampSelection` — OLED_Utils.cpp
- `OLEDMenuItem` / `OLEDMenuItemEx` — OLED_Display.h:148 / :155
- `MenuAvailability` / `getMenuAvailability` — OLED_Display.h ~293 / OLED_Mode_Menu.cpp
- Main menu: `displayMenuListStyle` (OLED_Mode_Menu.cpp:135), `oledMenuUp/Down/Select/Back` (OLED_Utils.cpp:5153/5180/5247/5209), category data (OLED_Utils.cpp:4299+), state vars (OLED_Utils.cpp:4069-4074, 4454-4458), special-case dispatch (~5991)
- Sensor menu: `displaySensorMenu` (OLED_Mode_Menu.cpp:346), `sensorMenuInputHandler` (:456), `sortSensorMenu` (:63), `oledSensorMenuItems[]` (OLED_Utils.cpp:4402)
- Caps: `OLED_SCROLL_MAX_ITEMS` (OLED_Utils.h:138) vs `MAX_DYNAMIC_MENU_ITEMS` (OLED_Display.h:158)
