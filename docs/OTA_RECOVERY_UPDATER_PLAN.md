# OTA via Recovery Updater — Design & Implementation Plan

**Date:** 2026-07-30
**Status:** PLAN ONLY — nothing implemented, nothing committed. Facts verified against the working tree and ESP-IDF v5.5.1 source on 2026-07-30, then the whole plan adversarially reviewed by three independent verification passes (repo claims, IDF mechanics, design completeness); all 30 findings are folded in below. Repo citations are `file:line`; IDF citations are relative to `/Users/morgan/esp/esp-idf`.
**Decision owner:** user. Open questions in §14 must be answered before S1.

---

## 1. Executive summary

Add over-the-air firmware updating using the **recovery-loader ("golden image") pattern**: a small, change-controlled **updater app** lives in the `factory` partition (1336 K slot), and the real firmware lives in a single full-size `ota_0` slot. Updates flow: main app stages a signed image (usually via the existing web file upload) → `otaupdate confirm` reboots into the updater → updater verifies and writes `ota_0` → reboots back. A crashing or corrupt main image automatically falls back to the updater (never a brick); a bad staged image costs at most a re-upload. Image authenticity comes from **RSA-3072 signed app images without secure boot** (no eFuses burned, reversible). This composes cleanly with the flash-encryption trial: writes encrypt transparently, and OTA becomes the update path that survives an eventual FE Release mode.

Scope for the first delivery: **FeatherS3 (16 MB) boards only** — plain and FE variants. XIAO (8 MB), classic-ESP32 boards, ESP-NOW transport, GitHub-pull, and menu surfaces are explicitly deferred (§13, S7).

Why not classic A/B: two ~5.7 MB slots cost ~5.9 MB of LittleFS on 16 MB and are impossible on 8 MB (current binary ~5.14 MB and growing). Recovery-loader costs ~1.4 MB total. Full decision record in §12.

---

## 2. Goals and non-goals

### Goals
- G1. Update firmware over the network with no USB cable, on 16 MB boards.
- G2. **Unbrickable invariant (update path):** every failure mode *of staging, applying, or verifying an update* ends in either the running app or the recovery updater, both reachable without a cable. (A crash-loop that begins only *after* an image is committed is a distinct case — §6.7 "crash-loop escape" extends the invariant to it, pending §14 Q12.)
- G3. Only images signed with the project key ever gain execution via the update path. (§7 states the exact bound honestly, including the `updaterflash` caveats and their mitigations.)
- G4. NVS + LittleFS survive updates (settings, users, health data, captures). Corollary: **neither the updater nor the main app may ever bulk-erase NVS or format LittleFS as error handling** — NVS now holds the `hw1cap` health-sealing key, whose loss makes sealed health files permanently unreadable (`System_CaptureCrypto.cpp:20-21`).
- G5. Work identically under flash encryption (Development mode now; compatible with Release mode later).
- G6. Failed updates are loud: the updater records every terminal outcome in `hw1up/result`; the main app surfaces it at next boot as SYSEVT + notification + `otastatus`. Never silent.
- G7. Keep the updater dumb, small, and **change-controlled**: it changes only through the re-qualification gate in §11 (S2 bench matrix + soak before any `updaterflash` to the fleet). It carries its own version, shown in `otastatus`.

### Non-goals (deliberate)
- N1. Zero-downtime/background updates. The device is in recovery for the minutes of an update.
- N2. Instant revert to previous version. The old image is overwritten; "revert" = stage an older release. (GH history keeps all sources; local build archives keep bins — §9.)
- N3. Secure Boot v2 eFuse burn. Signature checking is software-only (§7). Physical-access attacks remain out of scope, as today.
- N4. **Anti-rollback.** Not just skipped — *structurally impossible*: IDF refuses to build anti-rollback with a factory partition present (`esptool_py/CMakeLists.txt:30-34`, `Kconfig.app_rollback:39`). Recovery-loader and anti-rollback are mutually exclusive; we choose recovery-loader.
- N5. OTA of the bootloader or partition table. (IDF 5.5 technically *can* stage a bootloader via `esp_ota_ops` — `esp_ota_ops.c:182-187` — we choose never to use it: maximal-risk component, and the layout must be final before fleet rollout regardless.)
- N6. Updating 8 MB / classic-ESP32 boards in the first delivery (deferred, §13).
- N7. Auto-update / scheduled checks. Updates are deliberate, user-initiated events. Enforcement, not just intent: `otaupdate` refuses execution from automation context (§6.7), since a stored automation could otherwise fire `otaupdate confirm` under its creator's superadmin identity.

---

## 3. Current state (verified 2026-07-30)

Facts the design stands on, from codebase recon (citations re-verified by the review pass):

- **No OTA anything today.** Zero `esp_ota_*`/`esp_https_ota`/`Update.h` call sites in project code; only `esp_app_get_description()` for version display (`System_SelfDevice.cpp:76-80`). No `ota_0`/`ota_1`/`otadata` in any of the four partition CSVs.
- **Partition CSV selection:** CMake copies `partitions_${sr}_${flashsize}.csv` → `partitions.csv` based on `ENABLE_ESP_SR` (`System_BuildConfig.h:293`, currently 0) and the board's flash size (`CMakeLists.txt:119-158`). Boards: `feathers3`/`feathers3_fe` (16 MB, S3), `xiao_s3` (8 MB, S3), `qtpy_esp32`/`feather_esp32_v2` (8 MB, classic ESP32).
- **Current layouts** (all: nvs @0xA000 16K, nvs_key @0xE000 4K, phy @0xF000 4K — the uncommitted FE-prep move):
  - no_sr_16mb: factory 0x10000/0x595000 (5716K), littlefs 0x5A5000/0xA5B000 (10604K)
  - sr_16mb: factory 4992K, model 3008K, littlefs 8320K
  - no_sr_8mb: factory 5332K, littlefs 2796K
  - sr_8mb: factory 4992K, model 3008K, littlefs 128K
- **Binary:** `build/hardwareone-idf.bin` ≈ 5.14 MB (5,262,544 B at last check — drifts every build); FE bootloader 33,056 B (needs table @0x9000; plain boards use 0x8000).
- **IDF v5.5.1** at `/Users/morgan/esp/esp-idf` (build/project_description.json).
- **Version:** `set(PROJECT_VER "0.99.5")` (`CMakeLists.txt:238`) → `esp_app_desc_t.version`; `project_name` = "hardwareone-idf". A settings-load version diff already posts `SYSEVT_FIRMWARE_CHANGED` (`System_Settings.cpp:1370-1377`).
- **Releases are source-only by policy** — RELEASING.md:38-40 explicitly forbids attaching board bins ("board-specific footgun"). This shapes transports (§6.5): default flow is *local build → push to device*, not device-pulls-from-GitHub.
- **WiFi creds, two copies:** authoritative = `/system/settings.json` (`network.wifi.networks[]`, passwords AES-128-CBC under the device key with 3 key epochs — `System_Settings.cpp:268-395,1268-1313`); **plus the esp_wifi driver's own NVS copy** — the project never sets `WIFI_STORAGE_RAM` and never calls `WiFi.persistent(false)`, so `esp_wifi_set_config(WIFI_IF_STA,…)` (`System_WiFi.cpp:953`, retry path `:1049`) persists to driver NVS; no call site ever passes `eraseap=true`. Caveats that matter for the updater (from review): the driver stores the **last-attempted** config (written per connection attempt, so it can name a dead AP), and on FE boards this NVS is XTS-encrypted (`sdkconfig:2511,2521`). **Conflict flag:** the hardening audit's #1 HIGH fix (`WIFI_STORAGE_RAM`, docs/PRE_1_0_HARDENING_AUDIT.md) would delete this copy entirely — decision in §14 Q11.
- **Web upload exists:** `POST /api/files/upload` streams url-encoded/base64 to any path *the caller's role may import to* (admin: effectively any path — `canImport()` + admin-only-path gates), with a ≤90 %-of-current-free-space guard computed **before** any truncation (`WebServer_Server.cpp:1944-2381`; guard `:2005`, path gates `:2125-2135`) — a working image stager with zero new code, but the guard's compute-before-truncate behavior forces the staged-file lifecycle rules in §6.5. `filewrite` base64 chunks work over serial/BLE (`System_Filesystem.cpp:1005-1083`).
- **ESP-NOW file streaming caps at 4 MB** (`kFileSlotMaxStreamSize`, `System_ESPNow_Files.h:93`) — a 5.14 MB image does **not** fit today; ESP-NOW transport is deferred (§13).
- **No HTTP client use anywhere** (HTTPClient included but zero call sites); cert bundle compiled but unused. GH-pull would be all-new code — deferred.
- **Boot health machinery to reuse:** crash records in RTC RAM with boot-phase tracking and magic/CRC guards (`System_CrashRecord.*`), `crashRecordSetPhase(CRASH_PHASE_RUNNING)` at `HardwareOne.cpp:2186`, `crashRecordMarkBootHealthy()` at `millis()>60000` (`HardwareOne.cpp:2386`), commanded-reboot chokepoint `recordRebootIntent()` (`System_Utils.cpp:2115-2134`). Verified: every intentional *restart* routes through the chokepoint — but **deep sleep does not** (`cmd_deepsleep` → `esp_deep_sleep_start`, `System_Utils.cpp:1493`; G2 Power page likewise), so wake-from-deep-sleep is a cold boot with no intent record (§8 row).
- **Battery:** `gBatteryState` (percentage, `usbPresent`) on a 10 s tick (`System_Battery.h:64-93`); there is **no** existing low-battery refusal logic (`checkAutoPowerMode` is a TODO placeholder, `System_Power.cpp:143-162`) — the OTA battery gate in §6.7 is new, and must define behavior when the gauge hasn't ticked yet (treat unknown as "require `force`").
- **Auth:** `requiresSuperAdmin` on `CommandEntry` enforced at `authorizeCommand()` (`System_Utils.cpp:4443-4454`); `confirm`-token convention per `filedelete`/`userdelete`; per-task TLS identity. Passwords are `PBKDF2:10000:<hex>` in `/system/users/user_settings/<id>.json` — but **salted with the device key** (`System_User.cpp:707` → `getDeviceEncryptionKey()`), i.e. the eFuse-MAC+flash-UID derivation with its 3-epoch ambiguity. The recovery PIN therefore must NOT reuse this scheme verbatim (§6.8).
- **NVS is nearly empty but not disposable:** project namespaces are `bootstate` (boot counter) and `hw1cap` (capture sealing key — irreplaceable, see G4). Note `bootStateInit()` currently does erase-and-retry on NVS init failure with a stale "only WiFi calibration and boot state" comment (`System_BootState.cpp:14-22`) — S3 adds a task to bound that (§11).
- **LittleFS mount:** label `littlefs`, and the Arduino wrapper **formats on mount failure** (`System_Filesystem.cpp:58-74`) — the updater must bypass the wrapper and mount read-only via `esp_vfs_littlefs_register` (`read_only=1, format_if_mount_failed=0`; fields at `managed_components/joltwallet__littlefs/include/esp_littlefs.h:45-46`).

## 4. Verified IDF v5.5.1 mechanics (the load-bearing behaviors)

Each verified by reading IDF source, then independently re-attacked by the review pass. M13/M14 were *discovered by* the review and are design-shaping:

| # | Behavior | Citation |
|---|---|---|
| M1 | `esp_ota_set_boot_partition(factory)` **image-validates the factory image first** (incl. signature — M4), then erases the whole otadata partition (boots factory via "otadata empty" default). → "enter recovery" fails cleanly if the updater slot is corrupt/empty. | `esp_ota_ops.c:605,609-617` |
| M2 | Rollback state machine: writer sets `NEW`; first boot bootloader sets `PENDING_VERIFY`; if the app doesn't `esp_ota_mark_app_valid_cancel_rollback()`, the **next** boot sets `ABORTED` and selects another slot; with only factory remaining, **falls back to factory**. Load loop also walks back to factory if the selected image fails SHA — **without touching otadata** (`set_actual_ota_seq` is a no-op when a factory partition exists). | `bootloader_utility.c:396-414,447-451,491-506,594-606`; `bootloader_common_loader.c:71-74` |
| M3 | `esp_ota_begin` refuses factory as a write target *when the staging partition is APP-type* (OTA_0..15 only); `esp_ota_get_next_update_partition` never returns factory. 5.5's `esp_ota_set_final_partition(final=factory, finalize_with_copy)` **does** support a fully-verified updater refresh via a DATA-type staging partition (verify-then-copy: `esp_ota_ops.c:450-513`) — rejected here only because it costs a ~1.4 MB staging partition of LittleFS (§6.7, §12). | `esp_ota_ops.c:65-71,153-173,260-293,450-513,758-781` |
| M4 | Signed-apps-without-secure-boot on S3 = **RSA-3072 only** (no ECDSA-V2 on S3). Verification runs inside **both** `esp_ota_end` and `esp_ota_set_boot_partition`. The **trusted public key is the signature block appended to the RUNNING app** (only 1 block honored). **The bootloader does NOT verify signatures at boot in this mode on S3** (on-boot check requires the ESP32-only ECDSA-V1 scheme) — load-time gate is SHA-256 only. | `Kconfig.projbuild:497-604`; `bootloader_support/src/secure_boot_v2/secure_boot_signatures_app.c:77,164-169,244-248`; `esp_image_format.c:34-45,191-205` |
| M5 | Build auto-signs when `CONFIG_SECURE_BOOT_SIGNING_KEY` is set (`SECURE_BOOT_BUILD_SIGNED_BINARIES` default y). Out-of-band: `espsecure.py generate_signing_key --version 2 --scheme rsa3072 k.pem` / `espsecure.py sign_data --version 2 --keyfile k.pem -o out.bin in.bin`. | `Kconfig.projbuild:681-698`; `esptool_py/project_include.cmake:155-171` |
| M6 | **`idf.py flash`/`app-flash` write the app at the FACTORY offset when a factory partition exists** (search order `['factory','ota_0',…]`). No config can retarget the stock targets; supported alternatives are project-defined flash targets via `esptool_py_flash_to_partition()` or `otatool.py write_ota_partition`. The two stock targets fail *differently* (§8): `flash` also rewrites otadata to blank (new build actually boots, from the wrong slot); `app-flash` leaves otadata → ota_0 (old app keeps booting, "my change had no effect"). | `esptool_py/CMakeLists.txt:19-23`; `parttool.py:142-146`; `project_include.cmake:314-342`; `otatool.py:195`; `app_update/CMakeLists.txt:73` |
| M7 | `ota_data_initial.bin` = all 0xFF, generated when otadata exists, and written by `idf.py flash`. All-0xFF otadata → bootloader defaults to factory. | `app_update/CMakeLists.txt:12-31,73`; `gen_empty_partition.py:20-27` |
| M8 | Bootloader factory-reset GPIO erases only DATA partitions (named list, default "nvs"; otadata only with `BOOTLOADER_OTA_DATA_ERASE`). GPIO0 unusable on S3 (ROM download strap). | `bootloader/Kconfig.projbuild:148-265`; `bootloader_common.c:110-131` |
| M9 | Under FE, **always encrypted**: bootloader, partition table, otadata, every app partition, nvs_keys; `esp_partition_write`/esp_ota encrypt transparently. | `flash_encrypt.c:267-443`; `esp_partition/partition.c:75-102` |
| M10 | App partition offset must be 64 K-aligned; app **size** only 4 K-aligned (no SB / SB v2); otadata exactly 0x2000. | `gen_esp32part.py:106-130,340-343,567-573` |
| M11 | `esp_app_desc_t.project_name[32]` + `version[32]`; read any slot's descriptor via `esp_ota_get_partition_description()`. **Descriptor-readable ≠ bootable**: the descriptor sits in the first sectors and typically survives a half-written or corrupt image — validity decisions must use full validation, never the descriptor (§6.3). | `esp_app_desc.h:30-31`; `esp_ota_ops.h:313`; `esp_ota_ops.c:811-831` |
| M12 | Updater size estimate (±30%): ~250-350 K without radio; **~750-950 K with WiFi STA+SoftAP + esp_http_server** → 1336 K slot has margin. | estimate, flagged as such |
| M13 | **Single-OTA-slot otadata hazard (review-discovered):** with `ota_app_count==1`, a new `esp_ota_set_boot_partition(ota_0)` writes an entry whose seq **equals** the existing active entry's, and the bootloader's equal-seq tie-break always picks entry 0 — a stale `VALID` entry-0 can shadow the new `NEW` entry, so the fresh image boots already-VALID with **no verify window armed**, and a crashing image then loops forever with no fallback. → **Updater invariant: erase the entire otadata partition immediately before every `set_boot_partition(ota_0)`** ("canonicalization"), guaranteeing a single entry-0 in state NEW. | `esp_ota_ops.c:581-590`; `bootloader_common_loader.c:144-157` |
| M14 | **State-read ambiguity (review-discovered):** `esp_ota_get_state_partition` maps *every* CRC-valid otadata entry to slot 0 when one OTA slot exists and returns the first match — its answer can disagree with what the bootloader will act on whenever two entries are populated. Canonicalization (M13) removes the case; `otastatus` additionally dumps **both raw otadata entries** so any violation is visible in the field. | `esp_ota_ops.c:1023-1032` |

**Consequence of M4 that shapes everything:** the *first cable flash after repartition must already be signed* — an unsigned running app has no signature block, so it could never accept a signed OTA image. Signing is therefore part of bring-up (S2), not a later hardening layer. Key rotation or key loss = one-time cable reflash of both slots (no eFuse involved, fully recoverable).

**Consequence of M4+M2 that shapes `updaterflash`:** since the bootloader's only load-time gate is SHA-256, *any* well-formed image left in the factory slot will run on the next fallback. Raw writes to factory must therefore never leave unverified bytes behind (§6.7).

---

## 5. Architecture overview

```
flash (16 MB)
┌────────────┬──────────┬───────────────┬──────────────┬─────────┬─────────────────────┐
│ bootloader │ pt @9000 │ nvs/nvskey/phy│ factory:     │ otadata │ ota_0: main app     │ …
│ @0x0       │          │ @A000..FFFF   │ hw1-updater  │         │ (hardwareone-idf)   │
└────────────┴──────────┴───────────────┴──────────────┴─────────┴─────────────────────┘
                                                                   + model (SR) + littlefs

Normal boot:      otadata → ota_0 → main app runs
Update:           main app stages signed .bin on littlefs (existing upload paths)
                  → `otastage` validates + records SHA-256 → `otaupdate confirm` (superadmin, battery-gated)
                  → set hw1up: req=1, esp_ota_set_boot_partition(factory) [M1 validates updater], reboot
                  → updater: pre-checks staged file (incl. staged_sha) → erase otadata? no — first:
                    esp_ota_begin/write(ota_0) → esp_ota_end [M4 verifies signature]
                    → canonicalize otadata (M13) → esp_ota_set_boot_partition(ota_0) → write result, clear req → reboot
                  → main app boots PENDING_VERIFY → marks valid when healthy (§6.7) → deletes staged file
Crash fallback:   new image crashes → next boot ABORTED → bootloader boots factory → updater per §6.3 [M2]
Fresh flash:      otadata 0xFF → updater boots → no req + ota_0 passes full validation → auto-return (§6.3)
```

Components: (a) partition layouts §6.1-6.2; (b) updater app §6.3-6.4; (c) transports + staged-file lifecycle §6.5; (d) signing §6.6/§7; (e) main-app integration §6.7; (f) recovery auth §6.8; (g) build/flash/release tooling §9.

---

## 6. Design

### 6.1 Partition layouts (proposed, paste-ready)

Shared head (unchanged from FE prep): `nvs 0xA000/0x4000`, `nvs_key 0xE000/0x1000 encrypted`, `phy_init 0xF000/0x1000`.

**partitions_no_sr_16mb.csv** (primary):

```
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0xA000,   0x4000,
nvs_key,  data, nvs_keys, 0xE000,   0x1000,   encrypted
phy_init, data, phy,      0xF000,   0x1000,
factory,  app,  factory,  0x10000,  0x14E000,
otadata,  data, ota,      0x15E000, 0x2000,
ota_0,    app,  ota_0,    0x160000, 0x5A0000,
littlefs, data, littlefs, 0x700000, 0x900000, encrypted
```

- Updater slot **0x14E000 = 1336 K**, gap-free: factory ends 0x15E000, otadata ends exactly at ota_0's 64 K-aligned 0x160000 (app *size* only needs 4 K alignment — M10 — so the alignment slack lives inside the updater slot instead of dead space). Size-gate discipline: S2 fails if the built updater exceeds **1.15 MB** (≈13 % slot margin); if it lands well under 1.0 MB in practice, §14 Q10 revisits shrinking the slot in XIAO's favor later — for 16 MB the littlefs difference is noise.
- ota_0 = 5760 K, ≥ today's 5716 K factory; current binary leaves ~620 K headroom.
- littlefs 9216 K (**−1388 K** vs today's 10604 K). A staged image occupies ~5.14 MB *transiently* — the lifecycle rules in §6.5 guarantee it is deleted after a successful update, so steady-state loss is only the 1388 K.
- Offsets chain exactly: 0x10000+0x14E000=0x15E000; +0x2000=0x160000; +0x5A0000=0x700000; +0x900000=0x1000000.

**partitions_sr_16mb.csv**: identical through ota_0, then `model data/spiffs 0x700000/0x2F0000 encrypted`, `littlefs 0x9F0000/0x610000 encrypted` (6208 K, −2112 K vs today). Note: littlefs 6208 K still fits a 5.14 MB transient staged image, barely — SR boards should prefer staging when the FS is light, or use the updater push path.

**partitions_no_sr_8mb.csv / partitions_sr_8mb.csv: UNCHANGED in this delivery.** 8 MB OTA math (deferred): updater 1 MB + ota_0 5376 K → littlefs 1664 K and *no littlefs staging possible* (image > FS) — XIAO would rely purely on updater-network push. sr_8mb (littlefs 128 K) can never host OTA under any scheme.

**Migration cost:** littlefs offset moves → **one full data wipe** on first flash of the new layout (fine per current conventions; sequencing per §10).

### 6.2 Partition-table offset: 0x9000 for the boards being repartitioned

`CONFIG_PARTITION_TABLE_OFFSET` is compiled into **every app** (it's how esp_partition finds the table), so one updater build cannot serve mixed offsets. **Proposal: move `feathers3` to 0x9000** (matching `feathers3_fe`) as part of this repartition — 4 K of already-reserved slack, and the wipe/reflash is happening anyway. **`xiao_s3` keeps 0x8000 until its own S7 repartition** (changing it now would force a pointless wipe on a board this delivery doesn't serve — review finding). Classic-ESP32 boards remain a second offset axis whenever S7 reaches them. (§14 Q5)

### 6.3 Updater boot-decision logic

NVS namespace `hw1up`:

| Key | Writer | Meaning |
|---|---|---|
| `req` (u8) | main app sets; **updater clears only on terminal success or explicit user cancel** | 1 = update requested — persists across power loss so interrupted applies resume (review fix: clearing at entry made mid-write power-pulls unresumable) |
| `staged` (str) | main app (`otastage`) | littlefs path of validated staged image (canonical: `/ota/staged/<board>.bin`) |
| `staged_sha` (str) | main app (`otastage`) | SHA-256 of the staged file, computed by `otastage` — the **mandatory** pre-erase integrity check (replaces the earlier "optional sidecar file", which had no producer) |
| `apply` (u8) | updater | apply-attempt counter, cap 3 — a crashing apply path can't loop forever |
| `rearm` (u8) | updater | consecutive auto-re-arm count, cap 2 |
| `result` (u32+str) | updater, at **every** terminal outcome incl. success | outcome code + short text + monotonic counter; read/surfaced/cleared by main app at boot (G6) |
| `pin` (str) | main app (`otapin`) | `PBKDF2:<iter>:<salthex>:<hashhex>` — **explicit random salt** (§6.8) |
| `board` (str) | main app (boot) | board tag, e.g. `feathers3` |

**Validity oracle (used everywhere below):** "ota_0 is valid" means `esp_ota_set_boot_partition(ota_0)` (full image validation incl. signature) *succeeds* — never "descriptor readable" (M11 category error, review fix). Set_boot failure leaves otadata untouched, so probing is safe.

Decision tree at updater boot:

1. **`req==1`** → recovery mode. If `staged`+`staged_sha` present and pre-checks pass and `apply<3`: increment `apply`, run the apply sequence (§5 diagram; canonicalize otadata per M13 before the final set_boot). On success: write `result`=OK, clear `req`/`apply`, reboot. On pre-check/verify failure: write `result`, and if ota_0 is still valid, **auto-return immediately** (don't strand a healthy device on a 15-min timer); else hold in recovery. `apply` cap reached → hold, `result`=apply-loop.
2. **No req, otadata selects nothing, ota_0 valid** (oracle) → auto-return: the probe *was* the set_boot; reboot. Covers fresh provisioning (all-0xFF otadata — M7) and post-`updaterflash` restores.
3. **No req, ota_0 state `ABORTED`** (crash fallback — M2): `rearm<2` → increment, canonicalize + re-arm ota_0, reboot (heals "power pulled or deep-slept during verify window"); else hold with SOS LED, `result`=crash-loop.
4. **No req, ota_0 invalid** (oracle fails — covers empty slot, half-written image, *and* the review-found "bootloader walked back to factory while otadata still says VALID" state, which branch 2/3 tests miss) → hold in recovery, `result`=ota0-invalid. Canonicalize otadata on entering any hold state so the *next* update deterministically arms the verify window (M13).
5. **Idle timeout (hardcoded 15 min)** with valid ota_0 → auto-return; invalid → keep holding.

`rearm` and `apply` are cleared by the main app's mark-valid hook (§6.7). The updater **never writes the factory partition** (its own slot — G7) and **never erases NVS or formats littlefs** (G4): if `nvs_flash_init` fails, degrade to no-NVS mode (SoftAP open with banner, network push only, no staged path, no PIN) — the IDF-idiomatic erase-and-retry is *forbidden* in the updater (review: it would destroy `hw1cap` and WiFi creds on the exact device that's already in trouble).

### 6.4 The updater app (`updater/` project)

**Layout:** standalone IDF project in-repo (`updater/CMakeLists.txt`, `updater/main/…`, `updater/sdkconfig.defaults`, `updater/boards.h`). It shares **nothing** with `components/hardwareone` except the signing key at build time (G7). Built per board variant via the same `HW_BOARD` defaults-layering (board file first, updater overlay last so its own keys win) — this inherits each variant's FE / NVS-encryption / table-offset config automatically. **Artifacts are board-suffixed: `hw1-updater-<board>.bin`** (review: plain vs FE builds are otherwise indistinguishable and cross-flashing them breaks NVS access on the FE board), and the updater's `PROJECT_VER` carries the same `+<board>` metadata as the main app so `updaterflash` can check it.

**sdkconfig posture:** no PSRAM (quad/octal split disappears), no BT, no TLS/cert bundle, console = USB-Serial/JTAG, log INFO, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, `CONFIG_ESP_WIFI_NVS_ENABLED=y` (explicitly — trimming it silently kills STA rescue; review), NVS-encryption keys matching the board variant, signing config identical to main (§9 drift guard). `project(hw1-updater)` → `project_name="hw1-updater"` is the cross-flash discriminator. **No `RTC_NOINIT_ATTR` variables at all** — the updater's linker layout differs from the main app's, and writing RTC noinit would corrupt the main app's crash records / reboot stash / ramflush overlay across the update round-trip (they are magic/CRC-guarded on read, but S3 verifies that assumption — §11).

**Function set (deliberately complete — anything not here is out):**
1. NVS (`hw1up`, driver WiFi config; XTS via nvs_key on FE boards); LittleFS **read-only** via `esp_vfs_littlefs_register` (`read_only=1, format_if_mount_failed=0`). Never format, never erase (G4).
2. Staged-file update path per §6.3 branch 1. Pre-checks before erasing ota_0: image magic 0xE9, `chip_id`==ESP32-S3, size ≤ ota_0, `project_name=="hardwareone-idf"`, board tag matches `hw1up/board` (**refuse** on mismatch; absent tag ⇒ accept + result-warn), **`staged_sha` match** (mandatory — this is what stands between a truncated upload and a lost app). Residual accepted risk: a corrupt-*signed* file passing SHA is caught only by `esp_ota_end` after erase → app lost until re-stage; recovery holds (G2).
3. WiFi **STA** join from the driver's NVS config — explicit sequence `esp_wifi_init` → `set_mode(WIFI_STA)` → `start` → `connect` (config auto-loads at init; the persisted mode may be NULL/AP so mode must be forced). 60 s fail → **SoftAP** `HW1-Recovery-<mac4>`. *This STA feature is conditional on §14 Q11* — if the hardening audit's `WIFI_STORAGE_RAM` fix lands, the updater is SoftAP-only by design.
4. Minimal HTTP server (port 80): status page (slot versions, board tag, last `result`), `POST /stage` (raw octet-stream → littlefs staging when it fits, else direct-to-ota_0 streaming with the same header pre-checks — network drop mid-direct-stream = app lost until re-push, §8), `POST /apply`, `GET /status` JSON. PIN-gated with backoff: 3 failures → 30 s lockout, exponential (§6.8).
5. LED codes (`updater/boards.h`: NeoPixel FeatherS3 / plain LED XIAO): breathing=idle-recovery, fast=writing, double-blink=waiting-for-image, SOS=held (crash-loop or ota0-invalid).
6. Serial console: log stream + 3-command CLI (`status`, `apply`, `reboot`). No binary-over-CDC in v1 (S7; any future updater change goes through the G7 re-qualification gate).
7. TWDT enabled and **subscribed** in every updater task (unlike the main app today); wedge → reset → decision tree re-runs (idempotent by design: `req`/`apply` semantics).

**Explicitly absent:** TLS, BLE, ESP-NOW, SD access, settings.json parsing, device-key/AES/epoch code, RTC-noinit state, any NVS/FS erase-or-format path.

### 6.5 Transports and the staged-file lifecycle

| Tier | Path | Status |
|---|---|---|
| **A (day one)** | Build locally → existing web upload or `filewrite` base64 → `/ota/staged/<board>.bin` (littlefs **only** — the updater can't read `/sd/`) → `otastage` → `otaupdate confirm` | Reuses existing transport code; matches source-only release policy |
| **B (day one)** | Updater's own HTTP push (SoftAP; STA pending Q11) — the rescue path | New code, in updater |
| C (deferred) | ESP-NOW staging from a peer (mesh update of remote nodes) — needs `kFileSlotMaxStreamSize` 4 MB→6 MB+ and the integrity-plan items | docs/ESPNOW_FILE_TRANSFER_INTEGRITY_PLAN.md |
| D (deferred, policy) | Device pulls from GitHub release asset — requires reversing source-only policy + new HTTPS client code | §14 Q6 |
| E (deferred) | Binary-safe USB-CDC in the updater (cable path surviving FE Release mode) | S7 + G7 gate |

**Staged-file lifecycle (review: without this, the second update is impossible** — a leftover 5.14 MB staged file plus the upload guard's compute-before-truncate math refuses the next stage):
- One canonical path per board: `/ota/staged/<board>.bin`. `otastage` **deletes any previously staged file first**, then validates, then records `staged`+`staged_sha`.
- The mark-valid hook (§6.7) **deletes the staged file and clears `staged`/`staged_sha`** after a successful update.
- `otastatus` flags a leftover staged file (present but no `req` pending) as stale, with a hint to delete or re-stage.

### 6.6 Signing pipeline (summary; threat model in §7)

- **Key:** RSA-3072 via `espsecure.py generate_signing_key --version 2 --scheme rsa3072` (M5). Lives **outside the repo**, never on any device, never in git; backed up. Loss = one-time cable reflash with a new key.
- **Key path plumbing (review):** the `CONFIG_SECURE_BOOT_SIGNING_KEY` line lives **only in a gitignored overlay** (e.g. `boards/local_signing.defaults`, pulled into `SDKCONFIG_DEFAULTS` by the existing layering for both projects) — an absolute home path must never land in tracked defaults or sdkconfig. Missing key ⇒ build fails with a clear message. The overlay filename joins the `.gitignore` tripwire alongside `*.pem`.
- **Config (both projects, identical):** `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y`, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y`, key per overlay. Build signs automatically (M5). Bootloader untouched — no on-boot signature verification on S3 (M4), so cable flashing stays free.
- **Who verifies what:** updater verifies main-app images at `esp_ota_end`/`set_boot` (M4); the main app verifies replacement updater images via the `updaterflash` protocol (§6.7). Both apps carry signature blocks from the first post-repartition flash (M4 consequence). Trust root = whatever key signed the currently-running image; sign both apps with the same key.

### 6.7 Main-app changes

**New commands** (existing conventions: `OK:`/`Error:` funnel, confirm tokens, `cliHint`):

| Command | Gate | Behavior |
|---|---|---|
| `otastatus` | none (read-only) | Running partition; factory descriptor (project_name/version — "updater missing/mis-flashed/legacy" verdicts); ota_0 state **plus both raw otadata entries** (seq/state/CRC — M14); staged file + verdict + stale flag; `rearm`/`apply`; last `result`. **Legacy-layout behavior specified:** every partition lookup null-guarded; on a pre-repartition table prints "legacy layout — OTA unavailable" (review: S1 ships this before the updater exists, and mixed-fleet boards must not error). `json` variant. |
| `otastage "<path>"` | admin | Refuses non-littlefs paths; deletes previous staged file; §6.4 pre-checks incl. board tag (**refuse** on mismatch, `force` token to override — review: warn-then-refuse-in-updater was the worst UX ordering); computes + stores `staged_sha`; prints app-desc. Hint: `'otaupdate confirm' to install.` |
| `otaupdate [confirm]` | **superadmin** + confirm (§14 Q4); **refuses automation-context callers** (N7) | Gates: staged file validated; battery ≥ 30 % or `usbPresent`, *unknown battery state treated as failing* (`force` to override); warns if BLE/G2 session active. Then: `req=1`, `esp_ota_set_boot_partition(factory)` — fails cleanly if the updater slot is invalid (M1) — `rebootDevice("otaupdate", …, 1000)`. |
| `updaterflash "<path>" [confirm]` | superadmin + confirm | Refresh the updater. Protocol (rewritten after the review's HIGH finding — see below): pre-checks (`project_name=="hw1-updater"`, board tag vs `hw1up/board`, size ≤ factory, `otastage`-style SHA), **pre-write signature verification of the staged file** against the running app's trust root, raw `esp_partition` erase+write of factory (legal — M3), then `esp_ota_set_boot_partition(factory)` as the post-write integrity check; **on validation failure the factory partition is immediately erased** before reporting `Error:` (never leave unverified bytes bootable — M4 consequence); on success immediately canonicalize + `set_boot(ota_0)`. Honest residuals, documented in output: power loss between write and validate leaves an unvalidated image in factory until the command re-runs; power loss between the two set_boot calls lands in the updater, which auto-returns (branch 2); the post-success `set_boot(ota_0)` writes state NEW, so the *next* reboot runs a verify window (visible in `otastatus`). Battery-gated like `otaupdate`. |
| `otapin <pin>` / `otapin clear` | superadmin | §6.8. |

**`updaterflash` design note:** the review demonstrated that the original "raw-write then validate" order let a superadmin park an *unsigned* image in factory (set_boot fails but the bytes persist; the bootloader's SHA-only load gate then happily boots it on the next fallback — M4). The rewritten protocol closes this with pre-write signature verification + erase-on-failure. The fully-API-verified alternative — a dedicated DATA staging partition + `esp_ota_set_final_partition` (M3) — is acknowledged and rejected for its ~1.4 MB littlefs cost; revisit only if the manual verification proves fragile in S3 testing.

**Post-update verification (mark-valid) hooks:**
1. `recordRebootIntent()` chokepoint: if running state == `PENDING_VERIFY` **and** `crashRecordGetPhase()==CRASH_PHASE_RUNNING` → mark valid first. The phase condition (review) stops an automation-driven reboot seconds into boot from committing an image that never reached RUNNING.
2. The 60 s healthy marker (`HardwareOne.cpp:2386`): mark valid + clear `rearm`/`apply` + **delete staged file, clear `staged`/`staged_sha`** (§6.5) + post "firmware vX committed".
3. Boot-time `hw1up/result` consumption: post SYSEVT + notification for the last updater outcome (success or failure — G6), show in `otastatus`, clear.
4. Deep sleep during the verify window is *not* rescued by hook 1 (deep-sleep entry bypasses `recordRebootIntent` — §3); the wake bounces through recovery and `rearm` auto-heals. Optionally add mark-valid to the deep-sleep entry points (cheap; decide during S3).

**Crash-loop escape (extends G2 to post-commit failures — §14 Q12):** a committed image that crash-loops *after* the verify window (first health flush, 3 a.m. automation…) would otherwise reboot into itself forever — the most realistic bad-update mode bench testing misses. Proposal: in the early-boot crash-record consume path, if consecutive crashes ≥ 3 with every crashed boot's phase short of `RUNNING` → `esp_ota_set_boot_partition(factory)` + `result`=crashloop-escape, so the device parks in recovery instead of looping. (This also finally gives the crash-record consecutive counter a consumer — the hardening audit noted it's tracked but never read.)

**Tripwires:**
- Running from factory + `project_name!="hw1-updater"` + **table contains ota_0** → "main app flashed over the updater" (M6 misuse), loud banner + notification; fix = rebuild updater + `updaterflash`, then rebuild app + `flash-ota0` (your just-built app was in the wrong slot — §9). Without ota_0 in the table → "legacy layout" info line only (review: pre-migration boards must not false-positive).
- Factory descriptor unreadable/wrong on an OTA-layout table → "no updater installed; otaupdate disabled" warning.
- Write `hw1up/board` every boot (new short compile-time tag; `HW_BOARD` is currently CMake-only and `BOARD_NAME` strings are too long/pretty for matching).

**Version/board tagging:** `set(PROJECT_VER "0.99.6+feathers3")` derived from `HW_BOARD` in CMake, both projects (M11 fits). Docs keep the bare version; only descriptors carry the suffix. (§14 Q8)

**Surfaces:** G2/OLED/web UI entries deferred; they are thin wrappers over these commands per `G2_Page_Power.cpp:238-257` / `OLED_Mode_Power.cpp:148-152` patterns.

### 6.8 Recovery-page auth

Signatures bound *what* can be installed; the PIN bounds *who* can trigger it. Review reshaped both halves:

- **PIN storage:** `PBKDF2:<iter>:<salthex>:<hashhex>` with an **explicit random salt** in the NVS record. (The main-app password scheme salts with `getDeviceEncryptionKey()` — the eFuse+flash-UID derivation with its 3-epoch ambiguity; reusing it verbatim would drag exactly the device-key code §6.4 excludes into the updater, with a silent-never-verifies failure mode if epochs disagree. Same PBKDF2-HMAC-SHA256 *parameters*, self-contained salt.)
- **PIN policy:** ≥ 8 printable chars, used **verbatim** as the SoftAP WPA2 passphrase (review: any PSK "derived" from a short PIN is both untypeable and offline-crackable from one captured handshake at the PIN's entropy — so the PIN must *be* a real passphrase, and the user types the same string they set). PBKDF2 hash gates `/stage`/`/apply` on both STA and SoftAP paths, with 3-failure → 30 s exponential backoff.
- **Honest limits (§7):** on the STA path the PIN travels plaintext-HTTP on the LAN — capture yields *trigger* ability, not code execution (signatures). Unprovisioned PIN ⇒ updater still functions (else a wiped-NVS device with a broken app is stranded — G2 wins) but SoftAP runs open with a red "no PIN set" banner.
- Rejected: parsing `users.json` in the updater (G7); no auth at all.

---

## 7. Security & threat model

| Threat | Answer |
|---|---|
| MITM / hostile LAN client supplies image | RSA-3072 signature verified on-device before the boot pointer moves (M4); unsigned images cannot gain execution via the update path (G3). Transport TLS deliberately absent; trust is end-to-end in the image. |
| Compromised **admin** web session | Can stage files (existing upload, own path perms) but `otaupdate`/`updaterflash`/`otapin` are **superadmin** — relevant given the open FTS AuthBypass finding (docs/PRE_1_0_HARDENING_AUDIT.md). |
| Compromised **superadmin** | Can install any *project-signed* image, downgrades included (accepted). **Cannot** install arbitrary unsigned code *provided* the `updaterflash` protocol's pre-write verification + erase-on-failure are implemented exactly (§6.7) — the review showed the naive write-then-validate order breaks this bound (unverified bytes + SHA-only bootloader gate — M4). Residual: the power-loss window between raw write and validation briefly leaves unverified bytes in factory; a *deliberate* power-cut by the attacker in that window is inside the superadmin-compromise threat and noted as accepted. |
| Downgrade attack | Accepted by design (N4 — anti-rollback impossible with a factory partition). Personal fleet; downgrades are a feature. |
| Physical access | Out of scope, unchanged (no secure boot; FE dev-mode per its own trial). Signature checks don't bind cable flashing — that's the recovery story working as intended. |
| Key compromise | Attacker still needs a device-side trigger (superadmin or PIN). Rotate = re-sign + cable reflash both slots once. |
| PIN capture (LAN sniff) / SoftAP handshake capture | Yields update-trigger ability only; images still signature-bound. PIN-as-passphrase policy (§6.8) keeps SoftAP crack cost = real passphrase strength, with online brute force rate-limited. |
| Recovery squatting (stuck in updater, open SoftAP) | Idle auto-return; PIN-passphrase WPA2 when provisioned; SOS hold only when ota_0 is genuinely dead. |
| Automation abuse (3 a.m. `otaupdate confirm`) | Refused at the command by execution-context check (N7); reboot automations can't commit a bad image thanks to the phase-gated mark-valid (§6.7 hook 1). |
| Updater as attack surface | Function set frozen and enumerated (§6.4); no TLS/BLE/espnow; read-only FS; refuses self-writes; never erases NVS; change-controlled (G7). |

Secrets discipline: no secrets in the updater binary; PIN hash + board tag in NVS (XTS-encrypted on FE boards); signing key never touches a device; nothing new in PSRAM (updater has none).

---

## 8. Failure-mode matrix

| Failure | Outcome | Recovery |
|---|---|---|
| Power loss while staging upload | Partial file | `otastage` refuses (`staged_sha` never recorded); re-upload |
| Power loss after `otaupdate` before apply | otadata erased, `req=1` persists | Updater resumes the apply on next boot (branch 1 — req survives until success) |
| Power loss during ota_0 erase/write | Half-written ota_0; `req=1` | Bootloader can't boot ota_0 → updater → branch 1 re-applies (apply-counter capped) |
| Power loss during `esp_ota_end`/otadata flip | Dual-sector otadata tolerates torn writes; worst case boots updater | Branch 1 re-applies / branch 2 auto-returns |
| New image crashes in verify window | 2nd boot ABORTED → updater (M2); rearm ≤ 2 then SOS hold, `result`=crash-loop | Stage a fixed build via recovery page |
| Power pulled **or deep-slept** during verify window (good image) | Same ABORTED path — deep sleep bypasses the reboot chokepoint (§3) | `rearm` auto-heals; optional S3 fix adds mark-valid to deep-sleep entry |
| Commanded reboot during verify window | Hook 1 marks valid first (only if boot reached RUNNING) | none needed |
| Automation reboots seconds into pending boot | Phase gate stops premature commit → bounces through updater, rearm heals | none needed |
| **Committed image crash-loops later (post-verify)** | Without the escape: loops forever, cable-only — G2's worst gap | Crash-loop escape parks it in recovery (§6.7, Q12) |
| Staged file truncated/corrupt (transfer) | Caught **pre-erase** by mandatory `staged_sha` | Re-stage; app untouched |
| Staged file corrupt but SHA-matching (bad build) | Caught at `esp_ota_end` after erase → app lost, recovery holds, `result` recorded | Re-stage good image (accepted residual) |
| Unsigned / wrong-key image | Rejected at `esp_ota_end`/`set_boot` (M4) | none needed |
| Wrong-chip image | `chip_id` reject, pre-erase | none needed |
| Wrong-board same-chip image | Board-tag **refusal** in `otastage` (force-overridable) and updater | Stage correct build |
| Updater bin staged as app / app as updater | `project_name` discriminator refuses both directions | none needed |
| Wrong-*variant* updater (plain vs FE) via `updaterflash` | Board-suffixed artifacts + board-tag pre-check refuse | Use matching `hw1-updater-<board>.bin` |
| `idf.py flash` (muscle memory) | App lands in factory **and otadata blanked** → new build boots from wrong slot; tripwire #1 fires loudly | `updaterflash` restores updater; rebuild + `flash-ota0`; §9 deprecates stock targets |
| `idf.py app-flash` | App lands in factory, otadata still → ota_0 → **old app keeps running** ("my change had no effect"); tripwire #2 (factory-descriptor mismatch) catches | Same restore; note your new build was in factory — rebuild + `flash-ota0` |
| Network drop during tier-B **direct-to-ota_0** stream | ota_0 already erased → app lost until re-push; recovery holds, `result` recorded | Re-push (prefer staged path when littlefs has room) |
| littlefs unmountable in updater | No staging path (updater must NOT format — G4); `result`=fs-unavailable | Network push only; main app repairs FS later |
| Updater slot corrupt/empty | `otaupdate` refuses cleanly (M1); app keeps running | `updaterflash` |
| Updater itself buggy/wedged | TWDT reset → idempotent decision tree; still broken → cable (`encrypted-flash` works in dev-mode FE) | G7 change-control keeps this rare |
| NVS wiped | WiFi creds + PIN + **`hw1cap` health-sealing key** gone — sealed health files permanently unreadable (G4 blast radius, stated honestly) | SoftAP-open rescue still works; re-provision `otapin`; health loss is why *nothing* may auto-erase NVS |
| Battery state unknown at `otaupdate` | Gate treats unknown as failing | `force` token |
| littlefs full | Upload guard refuses staging; lifecycle rules (§6.5) prevent the self-inflicted version | Free space / delete stale staged file |
| Signing key lost | OTA refuses differently-signed images | One-time cable reflash both slots with new key |
| **FE Release mode someday + updater AND app both dead** | The one true brick (ROM DL restricted) | Policy: never burn Release before S7's CDC path + long soak; dev-mode FE recommended indefinitely for this fleet |

---

## 9. Build, flash & release tooling

- **`tools/flash_all.sh <board> <port>`** — the only documented full-flash path post-repartition: bootloader @0x0 (**built by the main project** — bootloader-resident behavior comes from it), table, `ota_data_initial.bin`, updater @0x10000 (tolerates a missing updater bin during S1 bring-up), app @0x160000, then explicit otadata write selecting ota_0 (`otatool.py`) so first boot never depends on fallbacks. Wraps `encrypted-flash` for the FE board.
- **Custom targets** in the main project via `esptool_py_flash_to_partition` (M6's sanctioned hook): `flash-ota0` and `app-flash-ota0`. **Both stock targets (`flash`, `app-flash`) are deprecated in docs** — they fail in two different confusing ways (M6/§8).
- **Daily dev loop after repartition (review — this is a conventions change):**
  - Main-app iteration: `idf.py app-flash-ota0 monitor`. **No `erase-flash`** — the erase-before-flashing habit ends here: NVS now carries `hw1cap` (health-sealing key) and OTA state; a casual erase destroys sealed health data (G4). Full-erase + `flash_all.sh` is reserved for layout migrations.
  - Updater iteration (S2-S5): `idf.py -C updater app-flash` **only**. `-C updater flash` is forbidden — it would overwrite the main project's bootloader/table/otadata with the updater project's copies, silently changing rollback semantics.
  - After any mis-flash recovery: your just-built image was in the wrong slot; rebuild and `flash-ota0`.
- **Updater build:** `HW_BOARD=<board> idf.py -C updater build` → `hw1-updater-<board>.bin`, auto-signed (M5).
- **Config-drift guard `tools/check_ota_configs.py`** — asserts across main + updater generated sdkconfigs: `PARTITION_TABLE_OFFSET`, `BOOTLOADER_APP_ROLLBACK_ENABLE` (=y **both** — note the main project's sdkconfig does NOT have it today, `sdkconfig:424`; enabling it is an explicit S1 task and takes effect via the main project's flashed bootloader), FE trio, `NVS_ENCRYPTION` **and** `NVS_SEC_KEY_PROTECT_USING_FLASH_ENC` (scheme must match, not just the flag), signing scheme + resolved key path identity, flash size, `ESP_WIFI_NVS_ENABLED=y` (updater), and `BOOTLOADER_SKIP_VALIDATE_{IN_DEEP_SLEEP,ON_POWER_ON,ALWAYS}` **all =n in both** (any future perf tweak enabling them would kill the SHA-walk-back safety leg §8 relies on — review). Run at stage exits and in RELEASING.
- **RELEASING.md deltas:** drift guard step; verify signature blocks on artifacts; keep a local per-board bin archive (the N2 "revert" store); updater rebuild only through the G7 gate; PROJECT_VER `+board` note. Releases stay source-only unless Q6 flips.
- **Docs at S5:** README flash section, QUICKSTART, USERGUIDE update chapter, COMMAND_REFERENCE (5 new commands), this plan → living doc.

## 10. Sequencing vs the flash-encryption trial — RESOLVED 2026-07-30

**Board topology (user has three FeatherS3s):**
| Board | Role | Build |
|---|---|---|
| Daily driver | Untouched until S6 fleet rollout | `feathers3` (current layout) |
| FE-trial board (sacrificial) | Flash-encryption trial, unchanged from its own plan | `feathers3_fe` |
| **Spare (new)** | **OTA testbed — all S1-S4 bring-up** | `feathers3` (plain, no FE variables in the debug loop) |

**Git + track sequencing:**
1. **Commit the inert FE-prep baseline to main** (the currently-uncommitted CSV edits, `boards/feathers3_fe.defaults`, sdkconfig — deliberately no-op on plain boards), so both tracks share a real ancestor and the OTA branch doesn't carry tangled FE working-tree state. *Pending user approval per the no-autonomous-commits rule.*
2. Cut branch **`ota-updater`** from that baseline; per-stage commits land on the branch, never main.
3. S1-S4 develop + HW-test on the OTA testbed (`HW_BOARD=feathers3`). The FE trial proceeds independently on its own board, in parallel, on the main baseline — the two tracks share no hardware and no uncommitted files.
4. FE leg of OTA: build `HW_BOARD=feathers3_fe` **from the same branch** (merging is not what enables FE — it's per-board build config) and run the FE-specific matrix on the FE board: encrypted otadata writes, NVS-XTS in the updater, `encrypted-flash` path in `flash_all.sh`. Sequenced after the FE trial has validated FE basics on that board; dev-mode fuses don't lock the layout, so the board takes the OTA repartition as one more `encrypted-flash` wipe.
5. Merge to main when the full matrix (plain + FE) is green; S6 fleet rollout follows.

The OTA repartition + feathers3 0x9000 move still roll out to each board as **one** migration event — the last flash-erase-required migration of its kind: after it, updates preserve data (G4), the erase-before-flash habit ends (§9), and data-format changes start needing a migration thought (§14 Q7).

## 11. Implementation stages

Per repo conventions: no incremental commits; each stage bench-verified → user HW-tests → commit on approval.

- **S0 — Decisions + key + baseline.** Remaining §14 questions answered; RSA-3072 key generated + backed up; gitignore tripwires (`*.pem`, signing overlay); FE-prep baseline committed to main (user-approved) and `ota-updater` branch cut (§10).
- **S1 — Repartition + tooling (no updater yet).** New 16 MB CSVs; feathers3 → 0x9000; **main-project sdkconfig gains `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + signing configs**; `flash_all.sh` (missing-updater tolerant, explicit otadata→ota_0); `flash-ota0`/`app-flash-ota0` targets; boot tripwires (legacy-aware) + `otastatus` (incl. legacy-layout output). *Exit:* FeatherS3 boots **signed** app from ota_0; `otastatus` sane on both new and legacy boards; features regression-pass. *HW test:* extended use on the OTA testbed board.
- **S2 — Updater skeleton.** `updater/` project (drift guard now has two configs — green becomes an exit criterion **here**, not S1); decision tree with req/apply/result semantics; staged-file path incl. `staged_sha`; canonicalization (M13); LED codes; TWDT; auto-return; read-only littlefs; **fault-injection hooks** (env-gated deliberate abort/delay points: mid-erase, mid-write, post-`esp_ota_end`-pre-set_boot) so power-loss legs are scripted, not hand-timed (review: the "pre-otadata-flip" window is milliseconds — untestable by hand). *Exit:* full staged cycle; scripted interruption at all 3 points lands in recovery and **resumes to success**; unsigned + SHA-mismatch + wrong-board images rejected at the specified layer; updater bin ≤ 1.15 MB; drift guard green.
- **S3 — Main-app integration.** Commands (§6.7), battery gate, mark-valid hooks 1-3 (+ optional deep-sleep hook), staged-file lifecycle, crash-loop escape (per Q12), `hw1up/result` consumption, tripwires. Verification tasks: confirm RTC crash-record/reboot-stash/ramflush structs reject garbage after an updater round-trip (magic/CRC assumption — §6.4); bound `bootStateInit`'s NVS erase-and-retry (its "nothing important in NVS" comment is now false — §3). *Exit:* end-to-end web-upload→update→auto-verify; deliberate-crash build ends held in recovery with correct `result`; commanded reboot in window doesn't bounce; automation-context `otaupdate` refused.
- **S4 — Updater network rescue.** SoftAP (+ STA if Q11 keeps it), push page, PIN + backoff, immediate-return-on-refusal, idle auto-return. *Exit:* update a deliberately-bricked-ota_0 device over SoftAP from a phone, PIN enforced, `result` surfaced after return.
- **S5 — Soak + docs + freeze.** Docs sweep (§9); **updater-specific soak legs** (review): one timed 15-min idle auto-return, one ≥24 h SOS hold on battery, one forced TWDT trip; then updater tagged/frozen under the G7 gate. 1-week soak on the OTA testbed under daily-driver-like use, with ≥3 real updates.
- **S6 — Fleet rollout.** Per-board exit (review): `flash_all.sh`, one full OTA cycle, `otastatus` clean, `result`=success. FE board per §10.
- **S7 — Deferred tier** (each its own mini-plan): XIAO 8 MB (incl. its 0x9000 move), ESP-NOW transport, GH-pull, USB-CDC loader (G7 re-qualification), G2/OLED surfaces, factory-reset GPIO (needs a spare strap-safe pin — M8), FE Release posture.

## 12. Alternatives considered

| Alternative | Verdict |
|---|---|
| Classic A/B (ota_0+ota_1) | Rejected: −5.9 MB littlefs on 16 MB, impossible on 8 MB; buys only zero-downtime + instant-revert (N1/N2 accepted) |
| A/B on 16 MB only | Rejected: same littlefs cost; 8 MB boards get nothing, ever. Recovery-loader eventually gives no_sr-8 MB a network-push path (sr_8mb is out of reach for *any* scheme) |
| Custom 2nd-stage bootloader fetches images | Rejected: no netstack, maximal risk; we choose never to modify or field-update it (N5) |
| `esp_ota_set_final_partition` staging for updater refresh | API-supported and fully verified (M3 — the review corrected the earlier "impossible" claim), rejected for the ~1.4 MB staging-partition cost; revisit if S3 shows the manual `updaterflash` verification is fragile |
| Compressed/delta staging (esp_delta_ota) | Orthogonal: shrinks transfer, not slots; revisit for ESP-NOW tier where 9 KB/s hurts |
| Updater parses settings.json for WiFi | Rejected: drags AES/device-key/epoch code into the trust anchor (G7); driver-NVS copy (Q11 permitting) + SoftAP cover it |
| TLS in updater | Rejected: cert timebombs in a change-controlled image; signatures are the trust root |
| Anti-rollback | Impossible with a factory partition (N4) |
| "Updater = stripped main app" single project | Rejected: G7; 5 MB can't fit; shared bugs would take down both slots |

## 13. Deferred-item parking lot

XIAO/8 MB layout + 0x9000 move; sr_8mb permanently OTA-less; classic-ESP32 (esp32-target updater, 0x8000 axis); ESP-NOW `kFileSlotMaxStreamSize` bump + sender CRC-verdict + resume; GH-pull + release-policy reversal; USB-CDC loader; G2/OLED surfaces; factory-reset GPIO; FE Release posture; slot-size revisit (Q10).

## 14. Open questions (answers close S0)

1. **Sequencing:** ~~Option B (FE trial first) or A?~~ **RESOLVED 2026-07-30:** third FeatherS3 becomes a dedicated plain-build OTA testbed; FE trial and OTA bring-up run in parallel on separate boards off a committed FE-prep baseline, with the OTA work on an `ota-updater` branch and the FE leg built from that branch via `HW_BOARD=feathers3_fe`. — §10
2. **Scope:** confirm 16 MB-only first delivery? — §6.1
3. **Recovery auth:** PIN-as-passphrase (≥8 chars) + open-with-banner when unprovisioned — acceptable? — §6.8
4. **Gate tier:** `otaupdate` superadmin (recommended) or admin? — §6.7
5. **feathers3 → 0x9000** (xiao deferred): approve? — §6.2
6. **Release policy:** stay source-only, or per-board signed bins to enable future GH-pull? — §6.5
7. **Data-compat policy post-OTA:** adopt "migration thought required" for format changes, or keep wipe-on-major-change? — §10
8. **PROJECT_VER `+<board>` suffix:** approve? — §6.7
9. **Verify window:** ride the 60 s healthy marker (recommended) or a dedicated shorter timer? — §6.7
10. **Updater slot 1336 K / gate 1.15 MB:** approve littlefs 10604K→9216K trade (with post-S2 shrink revisit)? — §6.1
11. **STA rescue vs hardening-audit fix (review-found conflict):** the updater's STA rescue depends on the plaintext-PSK driver-NVS copy that the audit's #1 fix (`WIFI_STORAGE_RAM`) would delete. (a) SoftAP-only rescue → audit fix can land fleet-wide (recommended for a personal, radio-range fleet); (b) keep STA rescue → record accepted-risk in PRE_1_0_HARDENING_AUDIT.md pointing here. Both docs must agree. — §6.4/§3
12. **Crash-loop escape** (auto `set_boot(factory)` after 3 consecutive pre-RUNNING crashes): enable, or scope G2 to the update path only? — §6.7

## 15. Cross-references

RELEASING.md (release steps, source-only policy) · docs/PRE_1_0_HARDENING_AUDIT.md (FTS AuthBypass; plaintext-PSK finding now coupled to Q11; crash-counter-never-read finding closed by the crash-loop escape) · docs/ESPNOW_FILE_TRANSFER_INTEGRITY_PLAN.md (tier-C prerequisite) · boards/feathers3_fe.defaults (FE trial config) · docs/CRASH_HISTORY_DESIGN.md (crashlog the verify hooks ride on) · memory: project_flash_encryption_trial, feedback_no_backwards_compat (ends at §10), feedback_verify_fixes_not_just_findings (the §4/§6.7 review trail is this principle applied).
