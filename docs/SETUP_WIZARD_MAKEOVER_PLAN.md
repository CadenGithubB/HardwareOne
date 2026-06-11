# First-Time-Setup Wizard Makeover — Plan & Status

**Goal:** make the boot first-time-setup (FTS) more *guided* instead of a flat dump of
toggles — driven by deployment archetypes, hardware auto-detect, and a consistent
OLED/serial UI — then converge the legacy blocking FTS engine onto the CLIMode state
machine so there's one wizard, not two.

Branch: `setup-autodetect` (off `main`).

Two wizard engines exist today and the makeover spans both:
- **Legacy blocking FTS** — `System_FirstTimeSetup.cpp` + `OLED_FirstTimeSetup.cpp`
  (`runSetupWizard()` / `firstTimeSetupIfNeeded()`), runs at boot. **Phase 1/2 work lives here.**
- **CLIMode wizard** — `System_SetupWizardMode.cpp` (`featuresetup` command), the Phase-5
  state-machine engine. **Phase 3 converges the boot FTS onto this.**

Shared core (used by BOTH engines): `System_SetupWizard.cpp/.h` — archetype table,
`kModeMenus[]` mode-picker table, `getHeapBarData()`, page model.

---

## Phase 1 — Hardware-detect capability + `detect` command  ✅ committed (`37a5d2c`)

- `detectHardware(DetectionResult&)` in `System_I2C.cpp`: pauses both I2C buses
  (`PollPauseGuard`), probes the known-address sensor DB at a fixed **100 kHz** (the
  MAX17048 fuel gauge is marginal at 400 kHz), classifies each address
  PRESENT_ENABLED / PRESENT_DISABLED / MISSING / PRESENT_INFRA. NACK log from
  `i2c.master` is suppressed during the scan.
- `cmd_detect()` — read-only report; `detect apply` (admin-gated via
  `currentExecIsAdmin()`) calls `applyDetectedHardware()`.

## Phase 2 — Guided Basic wizard  ◻ built green, pending HW-validation + commit

All in the **blocking FTS** engine + shared core. Built green on `feathers3`
(uncommitted batch). Per the project rule, no incremental commits during the refactor —
user HW-tests the whole batch, then it commits only if it passes.

**Detect apply-core** (`System_I2C.cpp/.h`, uncommitted, beyond the Phase-1 report):
- `applyDetectedHardware(const DetectionResult&) -> ApplyResult{enabled, offered, rebootNeeded}`:
  auto-enables cheap present-but-disabled features; *offers* heavy modules
  (`detectIsHeavyModule`: camera, microphone, espsr, input) rather than silently
  enabling; persists via `writeSettingsJson()`.

**Archetype model** (`System_SetupWizard.cpp/.h`):
- `struct SetupArchetype { id, name, blurb, seedFeatures[], requiredFeatures[] }`.
- Table: `handheld` "Standard Handheld", `headless` "Headless / relay",
  `glasses` "G2 Companion", `mesh` "Meshed Node".
- `setupArchetypeAvailable()` — **compile-gates** an archetype out when a required
  feature isn't compiled: handheld needs `oled`+`input` (a non-serial input — gamepad
  or ANO encoder + OLED), glasses needs `bluetooth`, mesh needs `espnow`.
- `applyArchetypeSeed()` — enables each seed feature that is compiled + has a setting,
  persists.

**Two-level menu** (`OLED_SetupWizard.cpp` + `System_FirstTimeSetup.cpp`):
- Level 1 = **Basic / Advanced / Import** (flat list — all visible at once on the OLED).
- Level 2 (reached via Basic) = archetype chooser: serial = full list + blurbs;
  OLED = one card per archetype via `drawSetupInfoPage()`, joystick up/down to flip.
- Back/escape: `'b'` (serial) and B-button (OLED) back out of every prompt, incl.
  username/password (via `getOLEDTextInput(... wasCancelled ...)`).

**Shared info-page renderer** `drawSetupInfoPage(title, body, footer, pageNum, pageCount)`:
- Header rule + wrapped body + footer rule, consistent with the wizard's other screens.
- **Page indicator** (`n/N`) is drawn **top-right** on the title line; the title
  auto-clips to the space before it so a long name never overlaps. Used by BOTH the
  restore-mode pages and the archetype cards (DRY).

**Heap bar is mode-aware** (`System_SetupWizard.cpp`):
- The header heap bar (`getHeapBarData` → `getEnabledFeaturesHeapEstimate`) was
  feature-granular, so HTTP-vs-HTTPS and Server-vs-G2 (sub-modes of one feature)
  looked identical. Each `WizardModeOption` now carries `extraHeapKB`
  (HTTPS **+20 KB**, G2 **+10 KB**; baseline modes 0), summed by
  `wizardEnabledModeExtraHeapKB()` and folded into `getHeapBarData()`.
- On the OLED mode pages the highlighted mode applies on joystick scroll, so the bar
  updates **live** as you flip options.

**Mode-page navigation fix** (`OLED_SetupWizard.cpp::handleModePage`):
- `nav.right` now advances (apply + next), mirroring A/START — previously right did
  nothing on the WEBMODE/BTMODE pages while every other page advanced on it. Footer:
  `A/>:Next  B/<:Back`.

**WiFi / restore robustness** (the import-crash fixes):
- `upsertWiFiNetwork()` (`System_WiFi.cpp`) is now `bool` and **blocks blank SSIDs** —
  the root-cause fix for the import reboot loop (saving a hidden/blank network → connect
  to "" → debug_out stack overflow → reboot loop).
- OLED + serial WiFi pickers map a typed number to the Nth **named** network (skip
  hidden/empty); serial restore picker rewritten as a loop, never returns empty SSID,
  groups hidden networks with a "type the exact SSID" affordance.
- Restore-mode OLED is now a **paged** screen (Restore / Device IP / Get the Tool) via
  `drawSetupInfoPage`, joystick up/down to flip, B to back.

### Phase 2 — remaining increment (not yet started)
- Wire `detectHardware()` + the heavy-device **offer/confirm screens** INTO the wizard
  flow. Today an archetype seeds only its fixed feature bundle; it does not yet
  auto-detect attached sensors during setup.

## Phase 3 — Converge boot-FTS onto the CLIMode state machine  ◻ pending

Collapse the two engines into one (`System_SetupWizardMode.cpp`). The shared core
(`System_SetupWizard.cpp`) already backs both, so the mode tables / page model /
heap bar behave identically — Phase 3 retires the blocking duplicate.

---

## Open follow-ups / notes
- README "Configurations" table is stale (old build-time framing: Barebones / Sensor
  Appliance / Standard Handheld / Bonded). Runtime archetypes here supersede it; the
  README should be reconciled to the four runtime archetypes.
- The `FlutterApp-main` names are community RE — firmware empirical observations win.
