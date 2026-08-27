# R1 Ring debug-flag family — migration plan

**Scope:** give the R1 ring its own `RING` bank + sub-flags, so ring logging is independent of `DEBUG_G2`.
**Status of this report:** every line number and count below was re-verified against the working tree on 2026-08-16. Where the two upstream audits disagreed, I say so and pick one.

---

## 1. Current state

All R1 ring logging rides on `DEBUG_G2F` — the glasses macro (`System_Debug.h:484`, `DEBUGF_QUEUE_DEBUG(DEBUG_G2, …)`), so `debugg2 1` is the only way to see a ring log and it also turns on the ~897-site glasses firehose. Verified counts: **`G2_Ring.cpp` 104 `DEBUG_G2F` + 20 `BROADCAST_PRINTF`**, **`System_R1_Protocol.cpp` 5** (all `[R1-selftest]`), **`G2_Health.cpp` 8** (all tagged `[HEALTH]`, but only `:350` and `:405` are ring data — the other six are lens navigation). That is **117 `DEBUG_G2F` sites in ring-owned files**, of which **111 are unambiguously ring**. A further ~23 ring-adjacent sites live in other files (8 in `G2_Glasses.cpp`, 5 in `G2_Page_Network.cpp`, 4 in `System_SensorLogging.cpp` already on `DEBUG_LOGGERF`, 6 in `G2_Health.cpp`). Two side-issues compound it: `G2_Ring.cpp:1084 static bool gRingDumpVerbose = true;` is a hand-rolled second toggle that **defaults ON**, so every ring notify frame already emits a 64-byte hex line whenever `debugg2` is on (gate at `:2499`, command `ringverbose` at `:4572`, registered `:5274`); and `R1_HealthHistoryStore.cpp` (937 lines), `OLED_Mode_R1_Health.cpp` and `WebPage_R1_Health.cpp` contain **zero** log sites — a "history wrote nothing" bug is currently invisible.

---

## 2. Proposed flag family

**Bank.** Bits 224–247 are genuinely free — confirmed by reading `DBG_BANK_LIST` (`System_DebugFlags.h:43-68`): last low bank is `F(UART, 216, 8, "UART")` at `:67`, next is `F(CONTROL, 248, 8, "Control")` at `:68`. The prose at `:39-40` and the row comment at `:349` both say so.

**The audits disagree here and I'm resolving it toward INVENTORY.** MECHANICS proposed an 8-bit bank with 6 rows (`LIFECYCLE / PROTOCOL / TRANSACTION / HISTORY / HEALTH`) built from the structure of the flag system; INVENTORY proposed 8 rows (adding `SETUP`, `BRIDGE`, `DUMP`, folding `HISTORY` into `HEALTH`) built from an actual 124-site partition of `G2_Ring.cpp`. The site census is the better evidence: the spoof-bridge (15 sites) and the setup/auth ritual (16 sites) are real, separable subsystems, and the hex-dump group has to exist anyway to absorb `ringverbose`. Eight rows fit an 8-bit bank with **zero** spare bits, so take a **16-bit bank** — the same precedent as `G2 (72,16)` and `ESPNOW (32,16)` — which still leaves 240–247 as one whole spare bank.

Add to `DBG_BANK_LIST` (`System_DebugFlags.h:67`, after UART):

```c
  F(RING,        224, 16, "R1 Ring")     \
```

Add to `DBG_FLAG_LIST`, inserted after the UART block (`System_DebugFlags.h:347`) and before the `/* Bits 224-247: spare */` comment at `:349`:

```c
  /* Bits 224-239: Even Realities R1 ring. Explicit master parent (NOT in     */
  /* DBG_AGG_FAMILY_LIST); subs gate on parent|sub like the G2/sensor banks.  */
  X(RING,           224, RING, 255, "RING",        debugRing,           debugring,           "ring", "enabled",   "All R1 Ring")   /* parent */ \
  X(RING_LIFECYCLE, 225, RING, 224, "RING_LIFE",   debugRingLifecycle,  debugringlifecycle,  "ring", "lifecycle", "Lifecycle")     \
  X(RING_SETUP,     226, RING, 224, "RING_SETUP",  debugRingSetup,      debugringsetup,      "ring", "setup",     "Setup ritual")  \
  X(RING_PROTOCOL,  227, RING, 224, "RING_PROTO",  debugRingProtocol,   debugringprotocol,   "ring", "protocol",  "Protocol")      \
  X(RING_TXN,       228, RING, 224, "RING_TXN",    debugRingTxn,        debugringtxn,        "ring", "txn",       "Transactions")  \
  X(RING_HEALTH,    229, RING, 224, "RING_HEALTH", debugRingHealth,     debugringhealth,     "ring", "health",    "Health data")   \
  X(RING_BRIDGE,    230, RING, 224, "RING_BRIDGE", debugRingBridge,     debugringbridge,     "ring", "bridge",    "Spoof bridge")  \
  X(RING_DUMP,      231, RING, 224, "RING_DUMP",   debugRingDump,       debugringdump,       "ring", "dump",      "Hex dumps")     \
  /* Bits 232-239: spare (RING) */                                                                                                 \
  /* Bits 240-247: spare (one whole bank)                                     */                                                   \
```

Verified free: `grep '"RING'` in `System_DebugFlags.h` returns nothing (no tag collision), and `DEBUG_RING` / `debugRing` / `debugring` appear nowhere in the component (no SYM, settings-field, or `cmd_*` collision). `RING` is not an Arduino core macro (unlike `INPUT`, the known casualty).

Per-sub rationale, with the verified site counts from the `G2_Ring.cpp` partition (124 sites, no double-assignment):

| Flag | Sites | Why it's its own flag | Loudness |
|---|---|---|---|
| `RING` | 0 | Parent/master gate only — `debugring 1` turns the family on | — |
| `RING_LIFECYCLE` | 67 (49 debug + 18 broadcast) | BLE link: scan, connect admission, GATT discovery, notify subscribe, disconnect/teardown deferrals. The "ring won't connect" flag. | quiet, bursty |
| `RING_SETUP` | 16 | Setup/auth ritual + clock custody + the 5 `[R1-selftest]` vectors. Runs once per link, not per frame. | quiet |
| `RING_PROTOCOL` | 11 | Per-frame envelope decode, rejects, dup-serial, fragment reassembly control lines. | **loud** (≥1 line/notify) |
| `RING_TXN` | 5 | Intent-queued + TX-write layer. Separate from PROTOCOL so "my `ringquery` never completed" doesn't cost one RX line per frame. | **loud** (~1.4 lines/s — `g2RingPollVitalForLogging` throttles at 700 ms, `G2_Ring.cpp:4253`) |
| `RING_HEALTH` | 9 | Ring *data*: telemetry cache, history-sweep coordinator, plus `G2_Health.cpp:350/:405` model ingest. | **loud** |
| `RING_BRIDGE` | 15 | sid=0x90 spoof push + spoof task + 30 s bridge-heartbeat. Exactly what you want OFF while debugging the link. | moderate (30 s cadence) |
| `RING_DUMP` | 8 | Raw hex only. **Absorbs `ringverbose`** and flips its default to OFF. | **loud** |

---

## 3. Migration mechanics (ordered)

| # | File / anchor | Edit |
|---|---|---|
| 1 | `System_DebugFlags.h:43-68` | Add `F(RING, 224, 16, "R1 Ring")` to `DBG_BANK_LIST`. Nothing else — the enum, `kDbgBankBase/Width/Label`, `DBG_BANK_COUNT` are all generated (`:70-87`). |
| 2 | `System_DebugFlags.h:347` | Add the 8 `X(...)` rows above. Fix the now-false prose at `:39-40` and `:349`. |
| 3 | `System_DebugFlags.h:621` | `DBG_FLAG_COUNT == 120` → **`== 128`**. Do **not** touch `:622` (`DBG_SUBBOOL_COUNT == 40`). |
| 4 | `System_Settings.h` | Add 8 `bool debugRing…;` members (next to `debugG2Dump`, ~`:575`) **and** 8 matching `debugRing…(false),` ctor initializers (~`:125`), in the same order. |
| 5 | `System_Settings.cpp:751` | `kDebugMappingCount == 120` → **`== 128`**; update the accounting comment at `:746-747`. |
| 6 | `System_Settings.cpp:1986-1993` area | Add a `// --- ring group ---` block of 8 `DBG_ROW(RING…)` picks next to the g2 block. Order here = web card order. |
| 7 | `System_Debug.h:496` | Add 8 producer macros (hand-written; nothing generates them) — see §4. |
| 8 | `System_Debug.cpp:2217-2226` | Add 8 `debugCommands[]` rows **inside** the existing `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` block (ring code is under that same gate, `G2_Ring.cpp:10`). The `cmd_debugring*` thunks are generated at `:1649-1659` — do not write them, do not declare them. |
| 9 | `System_Debug.cpp:2260` | `- 7` → **`- 15`** (7 `debugg2*` + 8 `debugring*`); update the comment at `:2247-2248`. Also decide `kBootDefaultDebugFlags` (`:49-58`) — `DEBUG_G2` is in it today, so migrating without adding `DEBUG_RING` silently kills early-boot ring output. |
| 10 | `G2_Ring.cpp` (104), `System_R1_Protocol.cpp` (5), `G2_Health.cpp` (2 of 8) | Re-point the call sites. Also delete `gRingDumpVerbose` + `cmd_ringverbose` (`:1084`, `:2499`, `:4572-4579`, `:5274`). |
| 11 | `WebPage_Settings.h:1760`/`:1919-1925`, `WebPage_Logging.h:~1012` | Optional cosmetics: `ring:'R1 Ring'` group label, `debugring*` tooltips, `gBankColor` hue. UART shipped without these and fell back fine. |
| 12 | `tools/build_board.sh` | Build **two** boards — one with `ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` on (FeatherS3) and one off. Step 9's arithmetic is only exercised by the off-board build. |

**Drift guards that turn a mistake into a build error** (all verified present):

- `System_DebugFlags.h:621` `DBG_FLAG_COUNT == 120` — fires on any added row (the deliberate speed bump).
- `:623-627` — `dbgBitsUnique()`, `dbgBitsInDeclaredBank()`, `dbgBanksDisjoint()`, `dbgTagsUniqueNonEmpty()`, `dbgParentsWellFormed()` (parent must be a root row **in the same bank**).
- `System_Settings.cpp:751` map count, `:763` `dbgMapFieldsDistinct()`, `:2026` registry row count (auto-derives from `DBG_FLAG_COUNT`, so it fails until the picks land), `:2051` `dbgRegRowsPickedOnce()` (forgotten *or* duplicated pick), `:2076` columns present, `:2096` `(group, jsonKey)` unique.
- `System_Debug.cpp:2268` `debugCommands` row-count tripwire — the only guard tying the hand-written table to the generated thunks, and its `#if` subtractions are hand arithmetic.
- Implicit: `offsetof(Settings, field)` won't compile for a non-member; `cmd_##cmdIdent` collides at link time on a duplicate.

**Do NOT:** add `RING` to `DBG_AGG_FAMILY_LIST` (`:485-499`) — it's pinned at exactly 14 families (`return n == 14;`, `:618`), and membership means the parent bit gets *rederived* by `dbgRecomputeParent()`, which would clobber `debugring 1`. `G2`, `MQTT`, `CAMERA`, `I2C` and every sensor stay out for exactly this reason. Also do **not** put the children in `DBG_SUBBOOL_LIST` — that list is for the 40 *bitless* subs; RING children own real bits.

---

## 4. Call-site migration

New macros, mirroring the G2 block verbatim (`System_Debug.h:484-496`) — parent-OR-sub so `debugring` stays a true master switch and `getDebugCategoryName()` (`System_Debug.cpp:1694-1712`) resolves to the sub tag:

```c
#define DEBUG_RINGF(fmt, ...)            DEBUGF_QUEUE_DEBUG(DEBUG_RING, fmt, ##__VA_ARGS__)
#define DEBUG_RING_LIFECYCLEF(fmt, ...)  DEBUGF_QUEUE_DEBUG(DEBUG_RING | DEBUG_RING_LIFECYCLE, fmt, ##__VA_ARGS__)
#define DEBUG_RING_SETUPF(fmt, ...)      DEBUGF_QUEUE_DEBUG(DEBUG_RING | DEBUG_RING_SETUP,     fmt, ##__VA_ARGS__)
#define DEBUG_RING_PROTOCOLF(fmt, ...)   DEBUGF_QUEUE_DEBUG(DEBUG_RING | DEBUG_RING_PROTOCOL,  fmt, ##__VA_ARGS__)
#define DEBUG_RING_TXNF(fmt, ...)        DEBUGF_QUEUE_DEBUG(DEBUG_RING | DEBUG_RING_TXN,       fmt, ##__VA_ARGS__)
#define DEBUG_RING_HEALTHF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_RING | DEBUG_RING_HEALTH,    fmt, ##__VA_ARGS__)
#define DEBUG_RING_BRIDGEF(fmt, ...)     DEBUGF_QUEUE_DEBUG(DEBUG_RING | DEBUG_RING_BRIDGE,    fmt, ##__VA_ARGS__)
#define DEBUG_RING_DUMPF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_RING | DEBUG_RING_DUMP,      fmt, ##__VA_ARGS__)
```

Keep the per-flag named macro form (not a generic `DEBUG_FLAGF(DBG_RING_PROTOCOL, …)`) — every other family in this codebase uses named macros and the parent-OR is baked into the macro, which is what keeps `parentBit=224` honest.

Reassignment:

- **111 mechanical conversions**: 104 in `G2_Ring.cpp` (per the §2 group table), 5 `[R1-selftest]` in `System_R1_Protocol.cpp` → `DEBUG_RING_SETUPF`, 2 in `G2_Health.cpp` (`:350`, `:405`) → `DEBUG_RING_HEALTHF`.
- **20 `BROADCAST_PRINTF` in `G2_Ring.cpp` stay unconditional.** They are counted in the group totals but they are user-facing status ("Connect FAILED — ring service not found", "WATCHDOG: central job stuck for %lus"). Converting them would silence connect failures for normal users.
- **The file boundary is not the flag boundary — a per-file `sed` gets this wrong in both directions.** `G2_Health.cpp` splits 2/6: `:1064`, `:1109`, `:1155`, `:1191` are lens navigation wearing a `[HEALTH]` tag → send them to `DEBUG_G2_PAGESF`. Conversely `G2_Glasses.cpp:13746` is a `[RING]`-tagged auto-reconnect line hiding in the glasses file → `DEBUG_RING_LIFECYCLEF`.
- **Ambiguous, my recommendations** (owner may overrule): `G2_Health.cpp:1130` "Trends Refresh requested" and `:1187` "Poll Now requested" straddle — they are lens taps that arm a ring sweep whose companion log (`G2_Ring.cpp:3087`) is `RING_HEALTH`; put them on `RING_HEALTH` to preserve the causal chain, accepting the tag/file mismatch. `G2_Glasses.cpp:2251` (ring advert stashed by the *shared* G2 scan) → `RING_LIFECYCLE`, so it pairs with the dedicated-scan twin at `G2_Ring.cpp:3384`. `G2_Glasses.cpp:9446` (temple forwards RingRawData) → `RING_BRIDGE`. `G2_Glasses.cpp:13657` (preempting ring scan for G2 repair) → leave on G2/BT; it's a radio-arbiter decision. `G2_Glasses.cpp:13157`/`:13171` are `BROADCAST_PRINTF` BLE-stack faults — leave unconditional. All five `G2_Page_Network.cpp` sites and `G2_Glasses.cpp:25434`/`:25494` are page concerns — **leave as G2**.
- **`System_SensorLogging.cpp:1805/1860/1878/1905`** are already on `DEBUG_LOGGERF`, so they are not blocked by the G2 coupling. `:1860` ("mine due but ring down — requested BLE reseek") is the one genuine `RING_LIFECYCLE` candidate. Leaving all four alone is defensible; dual-gating is not supported by the macro shape.
- **Nothing enforces step 10.** The family compiles green with zero producers. This is the one part with no drift guard — it needs review, per-site, with an eye on the surrounding function.
- **Redaction must survive.** `sensitivePayload` (`G2_Ring.cpp:2426-2434`, `:2500`) suppresses SN / algo-key / nvRecover / userInfo bytes. `RING_DUMP` must not become a way to print device identifiers.

---

## 5. User-facing result

```
debugring 1                      -> 'debugRing enabled (persistent)'   (all ring logs, /system/debug.json rewritten)
debugring 0
debugring 1 temp                 -> live mask only, lost on reboot
debugringlifecycle 1             -> scan/connect/GATT/disconnect only, glasses silent
debugringsetup 1 runtime         -> auth ritual, protocol profile, clock adoption, selftest vectors
debugringprotocol 1              -> per-frame RX summary, rejects, reassembly (loud)
debugringtxn 1                   -> intent queued + TX writes (loud while health logging runs)
debugringhealth 1                -> telemetry cache + history sweep + model ingest
debugringbridge 1                -> sid=0x90 spoof push + 30s bridge-heartbeat
debugringdump 1                  -> raw hex per frame/fragment (replaces `ringverbose`, now default OFF)
debugring 2                      -> 'Error: invalid arguments — Usage: debugring <0|1> [temp|runtime]'
debugflags                       -> mask + '  RING RING_PROTO' etc.
log start flags=0x<mask>         -> file capture limited to the ring bits
```

All eight are admin-gated, all follow the uniform `<0|1> [temp|runtime]` contract, and all appear automatically in: the web Settings debug card (new "ring" group), the web `systemLogFlags` checkbox grid under a new "R1 Ring" bank header, the OLED settings editor, the G2 lens settings pages, and `help debug`. No structural work on any of those surfaces.

---

## 6. Effort & risk

**Size:** 8 files must change, 2 more optional. Roughly **~70 added lines** of table/plumbing (1 bank row, 8 flag rows, 16 Settings lines, 8 registry picks, 8 macros, 8 command rows, 3 assert bumps, 3 comment fixes), **~117 edited call-site lines**, and **~12 deleted lines** (`gRingDumpVerbose` + `cmd_ringverbose` + its registration). Call it 200 touched lines. Plus two board builds.

**Mechanical (guarded by the compiler):** steps 1–9 and 12. If you get a bit, bank, tag, parent, settings field, pick, or count wrong, the build fails with a message naming the invariant. This is the safest part.

**Judgement (guarded by nothing):** step 10, all 117 sites. Getting a site on the wrong sub-flag is invisible — it just means the flag you turned on doesn't show the line you needed. The `G2_Health.cpp` 2/6 split and the `G2_Glasses.cpp:13746` stray are the two places a naive sweep is *known* to be wrong.

**What could break:**
1. **Early-boot verbosity changes silently.** `DEBUG_G2` is in `kBootDefaultDebugFlags` (`System_Debug.cpp:49-58`); `DEBUG_RING` won't be unless you add it. Ring logs that appear today before `debug.json` loads will vanish.
2. **`ringverbose` fight.** If you add `RING_DUMP` and leave `gRingDumpVerbose`, two independent toggles gate the same lines and the ad-hoc one wins by defaulting ON.
3. **Off-board build.** The `- 7` → `- 15` subtraction at `System_Debug.cpp:2260` is only proven by a build with G2 disabled. Per the repo's board-gating rule, "built green" on FeatherS3 proves nothing here.
4. **`-Wreorder`** if the `System_Settings.h` members and ctor initializers go in different orders.
5. **Behavioural noise change on HW.** `RING_DUMP` going default-OFF means anyone used to seeing hex frames under `debugg2` will see them stop. That's the intent, but worth knowing before the first HW test.

Nothing here changes runtime behaviour of the ring itself, and bit positions are internal-only (`System_DebugFlags.h:95-97` — persistence is by name), so inserting at 224 invalidates nobody's saved settings, only a hand-written `log start flags=0x…` mask.

---

## 7. Open questions for the owner

1. **Bank width — 8 or 16?** The audits split. 8 bits fits these exact 8 rows with **zero** headroom; 16 bits leaves 232–239 free inside RING and still leaves 240–247 as a whole spare bank. I recommend 16. Obvious future splits (`RING_SCAN` out of LIFECYCLE at ~14 sites; a `RING_STORE` for the silent history layer) have nowhere to go otherwise.
2. **Sub-flag set — 8 rows or 6?** MECHANICS' 6-row shape drops `SETUP`/`BRIDGE`/`DUMP` and adds a separate `HISTORY`. I recommend the 8-row census-derived set, but `RING_SETUP` folding into `RING_LIFECYCLE` is a defensible simplification if you'd rather have fewer switches.
3. **Does `DEBUG_RING` join `kBootDefaultDebugFlags`?** `DEBUG_G2` is there today. Include it and early-boot ring output survives; exclude it and boot gets quieter. Needs to be a decision, not an oversight.
4. **Which flags default ON in Settings?** INVENTORY recommends `RING_LIFECYCLE` + `RING_SETUP` on (bursty per connect, silent in steady state), everything else off. Note the settings registry defaults every debug bool to 0 — defaulting these on means an explicit default in `applyRegisteredDefaults`, which is a deviation from every other family. Simplest honest answer: default everything off and let `kBootDefaultDebugFlags` carry `DEBUG_RING | DEBUG_RING_LIFECYCLE` if you want the connect story on by default.
5. **Do the 6 `G2_Health.cpp` lens-page logs move?** My recommendation: `:1064/:1109/:1155/:1191` → `DEBUG_G2_PAGESF`; `:1130/:1187` → `RING_HEALTH` to keep the tap→sweep chain on one flag. Either way the `[HEALTH]` text tags should probably be re-spelled to match wherever they land.
6. **Fold `ringverbose` into `RING_DUMP`, or keep it?** I recommend deleting it (command + bool + gate). Keeping it means two toggles for one set of lines, and the surviving one defaults ON. Per the repo's no-backwards-compat rule there's no reason for a shim — but it *is* an existing user-facing command name disappearing.
7. **Add logging to the silent files?** `R1_HealthHistoryStore.cpp` (937 lines, zero sites), `OLED_Mode_R1_Health.cpp`, `WebPage_R1_Health.cpp` have nothing. This is a real observability gap, not an audit oversight — but adding sites is new work, not migration, and should be a separate decision (and possibly a separate `RING_STORE` bit, which is another argument for the 16-bit bank).
8. **Do the 4 `System_SensorLogging.cpp` `DEBUG_LOGGERF` sites move?** They're not blocked by the coupling, so "leave them" is fine; `:1860` is the one that arguably belongs to `RING_LIFECYCLE`.
---

## 8. Owner decisions (2026-08-16)

Recorded from the owner; these override the recommendations above where they differ.

### D1 — Delete `ringverbose`. **APPROVED.**
Remove `gRingDumpVerbose` (`G2_Ring.cpp:1084`), its gate (`:2499`), `cmd_ringverbose`
(`:4572-4579`) and its registration (`:5274`). Those lines move to `RING_DUMP`, which
defaults OFF. Per the repo's no-backwards-compat rule, no shim.

### D2 — `DEBUG_RING` joins `kBootDefaultDebugFlags`. **APPROVED.**
Add `DEBUG_RING` to the boot-default mask (`System_Debug.cpp:49-58`) alongside `DEBUG_G2`,
so early-boot ring output survives the migration. Note this makes the *parent* on at boot;
loud children (`RING_PROTOCOL`, `RING_TXN`, `RING_HEALTH`, `RING_DUMP`) stay off unless
separately enabled, so boot verbosity does not increase.

### D3 — The 20 `BROADCAST_PRINTF` become severity macros. **APPROVED — supersedes §4.**
§4 said these "stay unconditional". That outcome is right but the mechanism was wrong:
`BROADCAST_PRINTF` (`System_Debug.h:742`) bypasses the debug queue entirely, which the
header itself calls out as the thing the queue exists to prevent ("All debug output now
uses queue - no direct broadcast ... ensures thread-safe, ordered output from all sources",
`:450-451`). It is also uncategorised, so the log-file writer and log viewer cannot tag it.

The codebase already has the correct three-tier pattern (~68 `INFO_*F`, 18 `WARN_*F`, and a
matching `ERROR_*F` set). Add the ring trio next to them:

```c
#define ERROR_RINGF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_RING, "[ERROR][RING] " fmt, ##__VA_ARGS__)
#define WARN_RINGF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_RING, "[WARN][RING] " fmt, ##__VA_ARGS__); } while (0)
#define INFO_RINGF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_RING, "[INFO][RING] " fmt, ##__VA_ARGS__); } while (0)
```

`DEBUG_ALWAYS` is bit 255, the tagless CONTROL row (`System_DebugFlags.h:354`); OR-ing it in
makes `DEBUGF_QUEUE` emit regardless of flag state (`:445`), so these still print with every
ring flag off. `ERROR_*F` carries no level gate at all — always visible, by design.

**Behaviour is preserved by default:** `gLogLevel = LOG_LEVEL_DEBUG` at boot
(`System_Debug.cpp:88`), so INFO and WARN both emit today. The migration *adds* the ability
to quiet them by lowering the log level, and makes all 20 filterable in file captures
(`log start flags=<ring mask>`) because the mask now carries `DEBUG_RING`.

Proposed triage of the 20 (owner may re-tier any line):

| Tier | Count | Lines (`G2_Ring.cpp`) |
|---|---|---|
| `ERROR_RINGF` — hard failures, always print | 10 | 3464 scan submit failed · 3706 connect failed · 3729 service not found · 3749 notify char not found · 3768 capability missing · 3779 notify registration · 3809 setup failed · 3963/3989/4059 connect submit failed |
| `WARN_RINGF` — degraded/anomalous | 5 | 1460 watchdog stuck · 1471 connect skipped (in flight) · 1529 dropped BLE link · 3557 queued connect expired · 4129 link invalidation deferred |
| `INFO_RINGF` — normal status worth seeing | 5 | 1262 adopted ring clock · 3440 ringscan found · 3551 connect queued (G2 audio) · 3566 audio idle, starting connect · 3844 connected + profile |

This raises the migration total to **~137 edited call sites** (117 `DEBUG_G2F` + 20
`BROADCAST_PRINTF`) and adds 3 macros to §4's 8.

**Note:** `:3440` (`ringscan: found ...`) is *command output* for the user-invoked `ringscan`,
not really a log line. `INFO_RINGF` preserves today's behaviour; returning it through the
command's result string would be more correct but is a separate change.

### Still open
- **D4 — bank width 8 or 16?** Recommendation stands: **16** (bits 224-239), leaving 232-239
  spare inside RING and 240-247 as a whole spare bank. 8 bits fits the 8 rows with zero headroom.
- **D5 — do the 6 lens-page `[HEALTH]` logs move?** Recommendation stands: `G2_Health.cpp`
  `:1064/:1109/:1155/:1191` → `DEBUG_G2_PAGESF`; `:1130/:1187` → `RING_HEALTH`.

### D4 — Bank width. **16 bits (APPROVED).**
`F(RING, 224, 16, "R1 Ring")`. Bits 224-231 used, **232-239 spare inside RING**,
240-247 left as one whole spare bank.

### D5 — Lens-page logs. **MOVE (APPROVED).**
`G2_Health.cpp:1064/:1109/:1155/:1191` → `DEBUG_G2_PAGESF`;
`:1130/:1187` → `DEBUG_RING_HEALTHF` (keeps the tap→sweep chain on one flag).

---

## 9. Implementation record — 2026-08-16

**Status: IMPLEMENTED, built green on xiao_s3, UNCOMMITTED, HW test pending.**

Shipped exactly as planned in §2-§4 with all five decisions applied. Final census:

| Flag | Sites |
|---|---|
| `DEBUG_RING_LIFECYCLEF` | 50 |
| `DEBUG_RING_SETUPF` | 14 |
| `DEBUG_RING_PROTOCOLF` | 13 |
| `DEBUG_RING_TXNF` | 5 |
| `DEBUG_RING_HEALTHF` | 11 |
| `DEBUG_RING_BRIDGEF` | 15 |
| `DEBUG_RING_DUMPF` | 6 |
| **subtotal `DEBUG_RING_*F`** | **114** |
| `ERROR_RINGF` / `WARN_RINGF` / `INFO_RINGF` | 10 / 5 / 5 |
| **total migrated** | **134** |

Plus 4 lens-nav sites re-pointed to `DEBUG_G2_PAGESF`. **Zero `DEBUG_G2F` and zero
`BROADCAST_PRINTF` remain in ring-owned files.** `ringverbose` + `gRingDumpVerbose`
deleted; `RING_DUMP` owns those lines and they are now silenceable.

**G2-disabled path verified.** `ENABLE_BLUETOOTH` was temporarily set to 0, the board
rebuilt, and the header restored byte-identically (md5-checked). Every `static_assert`
passed — including the `debugCommands` row-count tripwire — which proves the
`- 7` → `- 15` subtraction. That configuration still fails at **link** with a single
undefined reference, `g2ESPNowAppOnRxText(unsigned char const*)`. **This is pre-existing
and unrelated to this migration** — a genuine board-gating gap in the BLE-off build worth
fixing separately.

**Flash cost: ~6.3 KB** (app partition free 0x1dae0 → 0x1c1d0; still 2% free). Eight flags
cost ~6.3 KB in settings-registry rows, generated command thunks, and strings — budget for
that before adding another family to a partition this full.

**Deliberately not done:** the four lines that moved to `DEBUG_G2_PAGESF` keep their
`[HEALTH]` text tag. Re-spelling them would change user-visible log text that was not in
scope; it remains an optional cosmetic follow-up.

### §9.1 Build-configuration verification matrix (2026-08-16)

All four relevant configurations were built (`tools/build_board.sh xiao_s3 build`), flipping
flags in `System_BuildConfig.h` and restoring the header byte-identically (md5-checked) after
each. The ring sits under `ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`; the health UI layer
(`G2_Health.cpp`, `R1_HealthHistoryStore.cpp`) sits under the separate `ENABLE_R1_HEALTH`,
which the DERIVED rules force to 0 when BT or G2 is off.

| # | Configuration | `-15` applies | Result |
|---|---|---|---|
| 1 | BT=1, G2=1, R1_HEALTH=1 (**shipping**) | no | **GREEN** |
| 2 | BT=1, G2=1, **R1_HEALTH=0** | no | **GREEN** — ring transport compiles against the health inline stubs |
| 3 | BT=1, **G2=0** | yes | asserts **PASS** (`System_Debug.cpp.obj` built clean); fails compiling `Bluetooth.cpp` — **pre-existing** |
| 4 | **BT=0** | yes | asserts **PASS** (reached link); fails link on `g2ESPNowAppOnRxText` — **pre-existing** |

**The `- 7` → `- 15` subtraction is verified twice** (configs 3 and 4, the only two where it
applies): `System_Debug.cpp` compiled with zero errors in both, so the `debugCommands`
row-count tripwire accepted the new arithmetic.

**Two pre-existing board-gating breaks were found** — neither caused by this migration, and
neither reachable from any shipping configuration (both gates are hardcoded `1`):

1. **BT=0, link:** `undefined reference to g2ESPNowAppOnRxText(unsigned char const*)`.
2. **G2=0, compile:** `Bluetooth.cpp:1241/:1288/:1493` call
   `bleCentralClientsTerminalTeardownAcknowledged()`, declared at `G2_Glasses.h:266` inside a
   G2-gated region. `Bluetooth.cpp` carries substantial uncommitted local work (743 insertions
   vs `d3ad5354`), so these calls are likely recent.

Both are tracked as separate work — they block ever shipping a BT-less or G2-less board.
