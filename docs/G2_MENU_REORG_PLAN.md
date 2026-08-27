# G2 Lens Menu Reorganization Plan

**Status:** ✅ IMPLEMENTED (uncommitted, awaiting HW flash+test). Built green on
`feathers3` (esp32s3); app partition 4% free. Adversarial multi-lens review run
post-impl — one finding (Status back-nav) fixed (see § 6.7 †). Not committed
(per the "user HW-tests, then commits" workflow). Cross-board verification
(all-features FM+camera, XIAO no-OLED) is the remaining check — see § 8.
Decisions locked in § 7.
**Goal:** Reshape the G2 (Even Realities lens) menu so it mirrors the OLED's
6-category layout and sort order. **Preserve every existing item — reorganize
only, remove nothing.**

---

## 1. Why

The G2 root menu has grown into a flat mix of leaves and submenus
(`Status, Sensors, Network, Apps, Settings, Power, Tests`), and the **Apps**
launcher has become an 11-row grab-bag (ESP-NOW, Files, Maps, LLM, Automations,
Ring, Pet, System Events, Logging, Users, OLED Login). The OLED went through the
same cleanup earlier and settled on **6 categories**; it even adopted the G2's
"Apps" terminology. This plan brings the G2 into parity — same grouping, order,
and category names — which also gives the G2 named homes for future parity
screens. The win is **logical grouping + a decluttered Apps list + room to
grow**, not fewer taps (root goes 7→6).

---

## 2. Final shape (locked)

```
Root (6, OLED order):   System · Config · Connect · Hardware · Apps · Power

System    →  Status · System Events · Logging · Tests
Config    →  Settings · Users (admin) · OLED Login
Connect   →  (the Network page, relabeled)   WiFi · Bluetooth …
Hardware  →  (the Sensors page, relabeled)   IMU · TOF · … · MIC · LED · CAM
Apps      →  ESP-NOW · Files · Maps · LLM · Automations · Ring · Pet
Power     →  (unchanged)   CPU preset · Restart · RAM Flush · Power Off
```

OLED reference (source of truth, `OLED_Utils.cpp:4777`): System, Config, Connect,
Hardware, Apps, Power.

---

## 3. The mapping — every current G2 item → its new home

Nothing is deleted. **(move)** changes parent; **(relabel)** keeps the whole
submenu and only the top row's label changes; **(stay)** is untouched.

### → System *(new launcher page, `hijackLabel="System"`)*
| G2 item | Today | Mirrors OLED |
|---|---|---|
| Status **(move)** | root leaf | System → Status |
| System Events **(move)** | Apps | System → Notifs (event ring) |
| Logging **(move)** | Apps | System → Logging |
| Tests **(move)** | root submenu | *(no OLED analog — diagnostics/dev; D2)* |

### → Config *(new launcher page, `hijackLabel="Config"`)*
| G2 item | Today | Mirrors OLED |
|---|---|---|
| Settings **(move)** | root submenu | Config → Settings |
| Users **(move, admin)** | Apps | Config → Users |
| OLED Login **(move)** | Apps | Config → Login |

### → Connect *(relabel: the existing Network page, submenu intact)*
| Content | Note |
|---|---|
| WiFi (+ HTTP(S) + ESP-NOW settings), Bluetooth (G2 / R1) | OLED splits these as flat siblings; G2 keeps them grouped by radio (D3). "Connect" reads well over {WiFi, Bluetooth}. |

### → Hardware *(relabel: the existing Sensors page, list intact)*
| Content | Note |
|---|---|
| IMU, TOF, APDS, GAMEP, ANO, RTC, GPS, FM, CAM, MIC, LED + drill-ins | Mic already lives inside; Speech / I2C-Scan have no G2 page (§9). "Hardware" fits the mixed peripheral list (LED/CAM aren't sensors). |

### → Apps *(existing launcher, trimmed)*
| Content | Note |
|---|---|
| ESP-NOW, Files, Maps, LLM, Automations | exact OLED Apps order, unchanged |
| Ring, Pet | G2-only apps; kept in Apps |
| ~~System Events, Logging~~ → System · ~~Users, OLED Login~~ → Config | moved out |

### → Power *(stay — already matches OLED)*
Power submenu unchanged.

---

## 4. How the two menu engines differ (context)

- **OLED** menu = static data model: `oledMenuCategories[]` + per-category
  `oledMenuCategory0..5[]` arrays + a `getCategoryItems()` switch; `targetMode`
  joins into a runtime screen registry.
- **G2** menu = a **page registry** (`registerG2Pages()` order + each
  `G2PageModule.hijackLabel`; `nullptr` = hidden) **plus one hand-built
  launcher** (`Apps`). The Apps launcher is the template we clone: a
  `{label,id}` builder + an enum + a tap-dispatch switch, all in `G2_Glasses.cpp`
  (`g2AppsBuildRows` @3938 / `enum G2AppRow` @3920 / `g2AppsHandleTap` @~4073).
  Streaming pages (Maps, Pet, LLM viewer, Camera stream) are launched with a
  callback and are **not** in the registry.
- **Back-nav is a fixed `backLabel` model — there is NO dynamic page back-stack**
  (verified: no `backStack`/`returnPage`/`navStack` in `G2_Glasses.*`). A page
  therefore has exactly one back-parent; this is why Status can't be cleanly
  dual-listed (D1).

---

## 5. Implementation approach & blast radius

**Approach: clone the Apps-launcher pattern.** Create two launcher pages
(**System**, **Config**) as structural copies of the shipped Apps launcher; their
rows forward to *existing* entry points. Connect/Hardware are `hijackLabel`
relabels; Apps is a trim; Power is untouched. Low risk — reuses an HW-validated
pattern.

**Files touched:**
- `G2_Glasses.cpp` — bulk: 2 new launchers, Apps trim, registration edit, cap
  bump, back-nav for Status/Settings/Tests/System-Events/Logging/OLED-Login, and
  the FM/MIC/LED `"<- Sensors"` label sweep.
- `G2_Glasses.h` — 2 new `G2HijackPage` enum ids; extern decls for the two new
  `g2Show*Menu()`.
- `G2_Page_Common.h` (or `G2_Glasses.h`) — declare `g2ShowSystemMenu()` /
  `g2ShowConfigMenu()` non-static (cross-TU, like `g2ShowAppsMenu`).
- `G2_Page_Users.cpp` — back-nav `"<- Apps"` → `"<- Config"` + return call.
- `G2_Page_Network.cpp` — 5× `"<- Network"` → `"<- Connect"`.
- `G2_Page_Sensors.cpp` — 2× `"<- Sensors"` → `"<- Hardware"`.
- `G2_Page_LED.cpp` — 1× `"<- Sensors"` → `"<- Hardware"`.

*(Rejected alternative: porting the OLED's static category-array model — a large
rewrite of a working BLE render/tap engine for no user-visible gain.)*

---

## 6. Step-by-step

1. **Bump the registry cap — BLOCKER, board-gating.**
   `#define G2_PAGE_REGISTRY_MAX 20` → **`22`** (`G2_Glasses.cpp:3653`).
   An all-features board (FM + camera + LLM) already registers **19** pages (only
   1 free); adding 2 category pages overflows *on that board* even though a
   feathers3 build stays green. Precedent: bumped 16→20 before. **Verify on the
   all-on profile, not just feathers3.**

2. **Add two enum ids** in `G2HijackPage` (`G2_Glasses.h`; max is `USERS=18`,
   19/20 are free — `LLM_MENU` aliases 14, not a real slot):
   `G2_HIJACK_PAGE_SYSTEM = 19`, `G2_HIJACK_PAGE_CONFIG = 20`.

3. **Create `kSystemPage`** (clone Apps): `enum G2SystemRow`, `g2SystemBuildRows`,
   `g2ShowSystemMenu` (**non-static**, header-declared), `g2SystemHandleTap`, a
   `G2PageModule` literal (`name="system"` — verify no CLI collision, fallback
   `sysmenu`), `hijackLabel="System"`, plus `g2RegisterPage(kSystemPage)`.
   Row order (mirrors OLED): **Status · System Events · Logging · Tests** →
   forward to the Status live-start · `g2ShowSysEventsPage` · `g2ShowLoggingMenu`
   · `g2ShowTestSuiteMenu`.
   *(Status has no standalone `g2Show*` today — it's started via
   `invokePageFromMain`; extract a tiny `g2ShowStatusPage()` helper or reuse that
   path.)*

4. **Create `kConfigPage`** (same shape): `enum G2ConfigRow`, builder, dispatch,
   `g2ShowConfigMenu` (**non-static**, header-declared), `name="config"`
   (fallback `configmenu`), `hijackLabel="Config"`, `g2RegisterPage`.
   Row order: **Settings · Users · OLED Login** → `g2ShowSettingsMenu` ·
   `g2ShowUsersMenu` (keep the `isAdminUser(g2HijackAuthContext().user)` gate so
   the row hides for non-admins) · `beginOledLogin` (keep `#if
   ENABLE_OLED_DISPLAY`).

5. **Relabel + reorder registration** (`registerG2Pages` @5928 = menu order):
   ```
   kSystemPage    ("System")   // new
   kConfigPage    ("Config")   // new
   kNetworkPage   hijackLabel "Network" → "Connect"
   kSensorsPage   hijackLabel "Sensors" → "Hardware"
   kAppsPage      ("Apps")
   kPowerPage     ("Power")
   // now hidden (hijackLabel=nullptr), reached via a category launcher:
   kStatusPage, kSettingsPage, kTestSuitePage, + the existing hidden pages
   ```
   Set `hijackLabel=nullptr` on `kStatusPage`, `kSettingsPage`, `kTestSuitePage`.

6. **Trim the Apps launcher** — keep the 3 spots in sync: remove
   `APP_ROW_SYSEVENTS`, `APP_ROW_LOGGING`, `APP_ROW_USERS`, `APP_ROW_OLED_LOGIN`
   from `enum G2AppRow`, the `add(...)` calls in `g2AppsBuildRows`, and the
   `g2AppsHandleTap` switch. Result: `ESP-NOW, Files, Maps, LLM, Automations,
   Ring, Pet`. `g2BuildAppsInfo` (CLI text) tracks the builder automatically.

7. **Repoint back-navigation** — fixed-`backLabel` model means each moved page's
   label **and** its Back-row handler must change together:

   | Page | now | → label | → handler |
   |---|---|---|---|
   | Status (`kStatusPage`) † | `<- Main Menu` | `<- System` | `g2StatusHandleTap` → `g2ShowSystemMenu()` |
   | Settings (`kSettingsPage`) | `<- Main Menu` | `<- Config` | `g2ShowConfigMenu()` |
   | Tests (`kTestSuitePage`) | `<- Main Menu` | `<- System` | `g2ShowSystemMenu()` |
   | System Events (`kSysEventsPage` @4758) | `<- Apps` | `<- System` | `g2ShowSystemMenu()` |
   | Logging (`kLoggingPage` @4913, row @4811) | `<- Apps` | `<- System` | `g2ShowSystemMenu()` |
   | Users (`G2_Page_Users.cpp:141`) | `<- Apps` | `<- Config` | `g2ShowConfigMenu()` |
   | OLED Login (inline) | `<- Apps` | `<- Config` | `g2ShowConfigMenu()` |

   † **Status mechanism correction (found in review, now implemented):** the
   original note here assumed Status "has no back row (double-click exits)".
   That was wrong — Status is a *compound* live-text page (`renderStatusCompound`)
   that DOES render a tappable back row, and it shared the generic
   `G2_HIJACK_PAGE_TEXT_VIEW` id, so its back-tap fell through to MAIN. Simply
   relabeling wasn't enough. The implemented fix gives Status its **own** id
   `G2_HIJACK_PAGE_STATUS = 21` plus a `g2StatusHandleTap` (idx 0 →
   `g2ShowSystemMenu`), and relabels both `kBackItems` in `renderStatusCompound`
   to `<- System`. This is exactly the SysEvents/Ring pattern: the tappable back
   row returns to the parent launcher, while **double-click still exits to MAIN**
   (platform-wide live-text behavior, shared by all live-text pages). Generic
   `TEXT_VIEW` views are unaffected — no registered page owns that id now, so
   they still fall back to MAIN.

   *Ring, Pet, ESP-NOW App, Automations, LLM menu keep `<- Apps` — they stay in
   Apps.* Ring/SysEvents/Logging render a **static** first row (build-time
   constant) — relabeling the constant is safe; never make it a live-updating
   row.

8. **Relabel sweep for Connect/Hardware** (cosmetic; D4) — the internal
   "back to parent" rows follow the rename:
   - `"<- Network"` → `"<- Connect"`: `G2_Page_Network.cpp` lines 263, 665, 676,
     692, 702 (5).
   - `"<- Sensors"` → `"<- Hardware"`: `G2_Page_LED.cpp:78`;
     `G2_Page_Sensors.cpp:624,1168`; `G2_Glasses.cpp:3859,4257,4331,4939,5107`
     (LED / MIC detail / FM tuner back rows, 8). Also grep each page for any
     title/hint string still reading "Network"/"Sensors".

9. **Cross-TU visibility.** Declare `g2ShowSystemMenu()` / `g2ShowConfigMenu()`
   in a shared header exactly as `g2ShowAppsMenu()` was made non-static so
   `G2_Page_Users.cpp` can call `g2ShowConfigMenu()` on Back.

10. **Gating parity.** Preserve existing gates only — Users stays admin-gated in
    the Config builder; don't newly restrict Status/Settings. Mirror whatever the
    Apps builder does for any guest concept.

11. **Info/CLI text.** Add `g2BuildSystemInfo`/`g2BuildConfigInfo` off the same
    builders (Apps precedent: `g2BuildAppsInfo` @3978) so CLI text can't drift.

12. **Build both board profiles** — feathers3 **and** an all-features build
    (FM + camera + LLM) to prove the cap fix and no board-gated breakage. A green
    feathers3 build proves nothing about the FM/camera paths.

---

## 7. Resolved decisions

- **D1 — Status placement → move into System (first row), only there.**
  Mirroring OLED costs one extra tap (`Menu → System → Status`). Dual-listing
  (root + System) was considered and **rejected**: the G2 has no page back-stack
  (§4), so a page reachable from two parents has an ambiguous Back. The +1 tap
  inside an already-intentional menu session is an acceptable parity cost. *(A
  separate "home/dashboard" landing shown before the menu is a possible future
  enhancement, not part of this reorg.)*

- **D2 — Tests → into System (last row).** No OLED analog exists (Tests is
  G2-only, dev/QA-facing). Folding it under System keeps a clean 6 and matches
  OLED's "System & Diagnostics" spirit; a dev bench belongs one level down, not
  competing with user categories at root.

- **D3 — Connect/Hardware → relabel only (v1); do not hoist.** The Network page's
  radio-grouped submenu and the Sensors list are well-suited to the G2's tap
  model and carry real regression risk if restructured. The OLED siblings that
  would justify hoisting (Speech, I2C-Scan) don't even exist on G2 yet. Top-level
  parity (6 categories, same names/order) is achieved without touching the
  internals. *(Future option: hoist Bluetooth/Microphone to siblings once those
  extra siblings exist.)*

- **D4 — Adopt the OLED names `Connect` / `Hardware`.** Cross-surface consistency
  is a stated project value (parity push), the labels are arguably *more* accurate
  ("Connect" over {WiFi, Bluetooth}; "Hardware" over a mixed peripheral list incl.
  LED/CAM), and they future-proof for the §9 siblings. Cost is the 13 cosmetic
  back-label edits in §6.8. *(Keeping `Network`/`Sensors` is a trivial per-page
  fallback if specificity is later preferred.)*

---

## 8. Risks & gotchas

- **Registry cap / board-gating** (§6.1) — the one true blocker; verify on the
  all-on board.
- **3-spots-in-sync** — every launcher (Apps, System, Config) must keep builder +
  enum + dispatch aligned or rows mis-route.
- **Back-nav breakage** — any missed `backLabel`/handler on a moved page strands
  the user (the §6.7 table is the checklist); the §6.8 sweep is label-only.
- **Static live-text rows** — Ring/SysEvents/Logging first row is a build-time
  constant; safe to relabel, never make it live-updating.
- **Status entry path** ✅ RESOLVED — the System row opens Status via
  `invokePageFromMain(kStatusPage)` (same path a MAIN tap took). Status also got
  its own `G2_HIJACK_PAGE_STATUS` id + `g2StatusHandleTap` so its tappable back
  row returns to System instead of falling through to MAIN (see § 6.7 †).
- **No backwards-compat needed** — user erases before flashing; a clean breaking
  reorg is fine (no migration/aliases).

---

## 9. Explicitly out of scope (not "reorganizing existing items")

These OLED items have **no G2 page today**; the reorg creates named homes but
does **not** fabricate screens (that would be *adding* features). The new
categories now have room for them:
- System: **Memory, Perf, Notifs, CLI Output, CLI Input**
- Config: **Logout, Change PW, Gamepad PW**
- Connect (as flat siblings): **Bond, Web** (exist nested under Network today)
- Hardware (as flat siblings): **Speech, I2C Scan**

---

## 10. Effort

**Moderate, low-risk.** Two new launchers are mechanical clones of the shipped
Apps launcher; the rest is relabel/reorder/trim + back-nav bookkeeping. Primary
work in `G2_Glasses.cpp` + the enum in `G2_Glasses.h`; small back-nav/label edits
in `G2_Page_Users.cpp`, `G2_Page_Network.cpp`, `G2_Page_Sensors.cpp`,
`G2_Page_LED.cpp`. No new rendering primitives, no new commands, no removed items.
```
