# RECON — Persistence Map (HardwareOne / ESP32-S3, IDF 5.5.1)

Read-only recon. Every entry below was opened and read; grep-only leads were dropped.
Line numbers are at working-tree HEAD (dirty tree, `main`).

---

## 0. Platform substrate

| Fact | Value | Evidence |
|---|---|---|
| Internal FS | **LittleFS** (joltwallet `esp_littlefs`, lfs **v2.11** `LFS_VERSION 0x0002000b`) | `managed_components/joltwallet__littlefs/src/littlefs/lfs.h:24` |
| Mount | `LittleFS.begin(false, "/littlefs", 10, "littlefs")` — formatOnFail **false**, maxOpenFiles **10**, label `littlefs` | `System_Filesystem.cpp:59` |
| Second tier | SD (SPI, Arduino `SD` lib, FAT). `CONFIG_FATFS_SECTOR_4096`, `FATFS_VOLUME_COUNT=2`, `FATFS_IMMEDIATE_FSYNC` **not set** | `sdkconfig:1911-1954`, `System_VFS.cpp:137` |
| Third tier (SR builds only) | `model` partition, subtype **spiffs**, holds `srmodels.bin`. Read-only at runtime | `partitions_sr_*.csv` |
| NVS | default `nvs` partition, **0x6000 = 24 KB (6 pages)**, shared with WiFi/PHY. `CONFIG_NVS_ENCRYPTION` **off** | `partitions*.csv`, `sdkconfig:2488` |
| Flash encryption / secure boot | **both OFF** (`CONFIG_SECURE_FLASH_ENC_ENABLED` not set, `CONFIG_SECURE_BOOT` not set) | `sdkconfig:490-494` |
| ArduinoJson | **7.4.2** (elastic docs; `overflowed()` == allocation failure, not a byte budget) | `components/hardwareone_libs/ArduinoJson/src/ArduinoJson/version.hpp:7` |
| `File::flush()` | `fflush()` **+ `fsync()`** → real `lfs_file_sync` | `components/arduino/libraries/FS/src/vfs_api.cpp:352-358` |

### LittleFS power-loss semantics (load-bearing — read before judging any writer)
`LFS_O_TRUNC` at open only sets `LFS_F_DIRTY` in RAM; the directory-entry commit happens at
`sync`/`close` (`lfs.c:3145-3148`). Therefore **on LittleFS a power cut mid-`"w"`-rewrite leaves the
OLD file fully intact** — you never get a zero-length or half file (the prior
`SETTINGS_LIFECYCLE_AUDIT.md` §2 reached the same conclusion and called the "doomsday chain"
refuted). What truncate-in-place *is* still exposed to:
1. **Short write / ENOSPC** — the code closes anyway and commits a truncated file.
2. **Multi-file invariants** — a crash between two related files (cert+key, users.json+boot_anchors).
3. **`remove()` then `rename()`** sequences (a real gap; see appendLineWithCap).
4. **SD/FAT paths**, where there is no COW and truncate-in-place *is* genuinely power-unsafe.

---

## 1. Partition tables

`CMakeLists.txt:119-157` generates `partitions.csv` by copying `partitions_{sr|no_sr}_{8mb|16mb}.csv`
based on `ENABLE_ESP_SR` × flash size from the board file. `sdkconfig` points at the generated
`partitions.csv`; offset 0x8000, MD5 on.

| Layout | nvs | phy | factory | model | littlefs |
|---|---|---|---|---|---|
| `no_sr_16mb` **(= current `partitions.csv`; FeatherS3 primary board)** | 0x9000 / 0x6000 | 0xf000 / 0x1000 | 0x10000 / 0x595000 (5716K) | — | 0x5A5000 / **0xA5B000 = 10604K** |
| `sr_16mb` | same | same | 0x10000 / 0x4E0000 | 0x4F0000 / 0x2F0000 | 0x7E0000 / **0x820000 = 8320K** |
| `no_sr_8mb` | same | same | 0x10000 / 0x535000 | — | 0x545000 / **0x2BB000 = 2796K** |
| `sr_8mb` | same | same | 0x10000 / 0x4E0000 | 0x4F0000 / 0x2F0000 | 0x7E0000 / **0x20000 = 128K** |

**Board-gated hazard (`sr_8mb` only):** LittleFS total is 128 KB while
`LOG_OVERFLOW_DEFAULT_RESERVE` is **100 KB** (`System_VFS.cpp:533`). The overflow latch therefore
fires almost immediately on that layout; with no SD card mounted `resolveOverflowPath` falls back to
the primary path and accepts silent loss (`System_VFS.cpp:628-631`). Applies to 8 MB + ESP-SR builds
only — not to the primary FeatherS3 16 MB layout.

No OTA partitions anywhere; single `factory` app. Moving a littlefs offset between layouts
reformats user data (documented in the CSV headers; irrelevant per the no-backwards-compat policy).

---

## 2. The write chokepoints

### 2.1 `VFS` dispatcher — `System_VFS.cpp`
Routes `/sd/...` → `SD`, everything else → `LittleFS` (`fsForPath` :378). All 59 write-mode opens in
the tree go through `VFS::open` / `VFS::openGuarded`; **no writer bypasses VFS** (only
`totalBytes()/usedBytes()` stat calls are direct — `System_SensorLogging.cpp:1177`,
`System_ImageManager.cpp:199`, `System_FileManager.cpp:395`, `System_Utils.cpp:2006`).

- `VFS::rename` (:458) rejects cross-tier renames, then `LittleFS.rename` (atomic, overwrites dest)
  or `SD.rename` (FAT: **fails if dest exists**).
- `VFS::remove` (:436) and `rename` invalidate the free-space cache.
- Guarded variants (`:786-900`) run `normalizeFsPath` (`..` reject) → `canX()` role table → dispatch.

### 2.2 FS mutex — `System_Mutex.cpp`
`FsLockGuard` (:83) is reentrancy-safe: if the current task already holds `gFsMutex` it sets
`held=false` and does nothing. The **manual** pair is asymmetric: `fsLock()` (:104) skips the take
when already held, but `fsUnlock()` (:110) **gives unconditionally if held by this task**. So a
manual `fsLock/fsUnlock` nested inside an outer `FsLockGuard` scope releases the *outer* lock, and
the outer destructor then gives a mutex it no longer owns. PLAUSIBLE hazard — `writeSettingsJson`
uses manual pairs (:1019/:1033, :1060/:1075, :1089/:1091) and is reachable from `setSetting()` at
arbitrary call sites; I did not trace a concrete caller that holds an outer guard, so it stays
PLAUSIBLE.

### 2.3 Generic text helpers — `System_Utils.cpp`
- `readText` :794, `readTextLimited` `System_Filesystem.cpp:1758` (byte-at-a-time `out += buf[i]`).
- **`writeText` :812** — `"w"` truncate; `f.print(in)` **return value discarded**; `flush()+close()`;
  returns `true` unconditionally after the open succeeds.
- **`writeTextAtomic` :827** — `writeText(path + ".tmp")` then `VFS::rename`. On rename failure it
  deliberately **refuses** the truncate-in-place fallback, removes the tmp, and returns false
  (:841-847). Good pattern — **except** it inherits `writeText`'s unchecked `print()`, so a short
  write produces a *complete-looking* tmp that then gets renamed over the good file.

### 2.4 Free-space machinery — `System_VFS.cpp:533-645`
`refreshLittleFsFreeCached` caches free bytes for 2 s, force-refreshed after 32 KB of
`noteLittleFsBytesWritten` hints. `resolveOverflowPath` latches (once per boot, never unlatches) to
the `/sd` mirror when free < max(100 KB, caller reserve). **Opt-in only** — state files
(settings/users/automations) deliberately never overflow.

Explicit pre-write free-space checks exist in exactly **two** places:
- `System_ImageManager.cpp:180-208` (`len + MIN_FREE_SPACE_BYTES`, both tiers).
- `System_SensorLogging.cpp:1170-1199` (`sensorlog start` gate: needs `gSensorLogMaxSize` free on
  LittleFS **or** SD).
Every other writer relies on the overflow latch or on nothing at all.

---

## 3. Boot-time persistence lifecycle — `System_Filesystem.cpp:54-237`

Order matters; this is the whole recovery story:

1. `LittleFS.begin(formatOnFail=false)` (:59). On failure → `logSystemEvent` + **`LittleFS.format()`
   — the entire data partition is erased on a single failed mount** (:60-69), then remount.
   If format or remount fails, `filesystemReady=false` and `initFilesystem()` returns false →
   `HardwareOne.cpp:1310-1313` prints `FATAL` and **hangs forever in `while(1) delay(1000)`**.
   *(KNOWN — `docs/SETTINGS_LIFECYCLE_AUDIT.md` §4.2, status unchanged.)*
2. `VFS::init()` → `tryMountSD()` (:201). SD mount failure is non-fatal; `gSdMounted/gSdWritable`
   both false. Writability is proven by a write+read+delete probe of `/HWPROBE.TMP`
   (`System_VFS.cpp:219-274`) with lazy re-probe on `isSDWritable()`.
3. **`cleanupLogOrphan()` × 6** (:94-99) — recovers `<log>.tmp` orphans from a crashed
   `appendLineWithCap` rotation. **Runs before** the generic `.tmp` sweep, which is the correct
   order.
4. `mkdir` tree (:108-134) — `/logging_captures{,/sensors,/system,/tracks}`, `/system`,
   `/system/sys_logs`, `/system/users{,/user_settings}`, `/system/certs`, `/system/llm`,
   `/espnow`, `/system/espnow{,/peers,/this_device}`, `/maps`.
5. **Orphan `.tmp` sweep** (:136-170) over exactly `{"/", "/system", "/system/users",
   "/system/users/user_settings", "/maps"}` (+`/system/llm`). Safe by construction: every
   `writeTextAtomic`-style writer leaves the *original* intact, so deleting the orphan loses nothing.
   **Not swept:** `/system/espnow` (`identity.tmp`, `System_ESPNow_Identity.cpp:25`),
   `/system/espnow/peers`, `/system/sys_logs`, `/logging_captures/sensors` (`.anchors.tmp`,
   `System_TimeAnchors.cpp:101`). Those orphans persist across boots (cosmetic; the next write
   overwrites them).
6. **Boot JSON "corrupt" check** (:172-189) over `{"/settings.json", "/system/debug.json",
   "/system/automations.json", "/system/users/users.json"}`. Test is `content[0] != '{' && != '['`
   — a first-character heuristic, not a parse — and the action is **delete**, not quarantine.
   `/settings.json` is a **stale root path**; the live file is `/system/settings.json`
   (`HardwareOne.cpp:239`). *(KNOWN — `SETTINGS_LIFECYCLE_AUDIT.md` §4.3, still unfixed.)*
7. `loadAndIncrementBootSeq()` (:194 → `System_User.cpp:3434`) — NVS boot counter bump + read-only
   users.json integrity pass (quarantine on corrupt, no write-back on the healthy path).
8. Automations dedup pass (:202-234) — reads `automations.json`, rewrites atomically only if
   duplicate IDs were found.
9. `fileSlotsBootCleanup()` (`HardwareOne.cpp:2068` → `System_ESPNow_Files.cpp:193`) — removes up to
   **8** orphan `/espnow/received/.part-*` staging files per boot.

**Settings load (`HardwareOne.cpp:1344-1368`):** parse failure now **leaves the file on disk** and
logs a `[EVENT]`; `gSettingsLoadedOk` stays false so `buildSettingsJsonDoc` skips the WiFi-array
rebuild (`System_Settings.cpp:960`), and crash counters are plain assignments, **not** `setSetting`
(`HardwareOne.cpp:1374-1381`). *(KNOWN — `SETTINGS_LIFECYCLE_AUDIT.md` §4.1 — **status changed:
FIXED**. The doc's "corrupt file silently replaced with defaults at boot" no longer applies.)*

---

## 4. File-write inventory

Legend — **Pattern**: `TMP+RENAME` / `TRUNC` (`"w"` in place) / `APPEND` (`"a"`).
**RV**: return values checked? **Freq**: how often.

### 4.1 Core state files (LittleFS, never overflow to SD)

| Path | Writer | Pattern | RV | Freq | Power-loss residue |
|---|---|---|---|---|---|
| `/system/settings.json` | `writeSettingsJson` `System_Settings.cpp:1003` | `/settings.tmp` + rename (:1058, :1090); **fallback = TRUNC in place on rename failure** (:1096-1110) | open ✔, `bytesWritten==0` ✔, **`bytesWritten < measureJson` ✘**, fallback serialize RV ✘ | per changed `setSetting()` unless `gDeferWrites`; **plus every successful WiFi connect** (`System_WiFi.cpp:1099` `lastConnected=millis()` → `saveWiFiNetworks()` :850) | tmp orphan at root, swept; old file intact |
| `/system/debug.json` | `writeDebugJson` :1140 | `/debug.tmp` + rename; fallback TRUNC (:1183) | same as above | per `setDebugSetting()` change | tmp orphan, swept |
| `/system/users/users.json` | `writeTextAtomic` from `System_User.cpp:916,1050,1130,1361,1429,1675,1799`, `System_FirstTimeSetup.cpp:275,908`, `System_ESPNow.cpp:3694` | TMP+RENAME | bool ✔ at all sites; **inner `print()` RV ✘** | account CRUD + `lastSeen` update only (boot counter moved to NVS — see :3435) | `users.json.tmp` orphan, swept; original intact |
| `/system/users/pending_users.json` | `writeTextAtomic` :2764 **and TRUNC** at :1320 (approve) / :1527 (deny) | mixed | `written==0` ✔ only | registration / approve / deny | TRUNC sites: old file survives (LittleFS COW) |
| `/system/boot_anchors.json` | `writeTextAtomic` :3193 (prune), :3356 (`writeBootAnchor`) | TMP+RENAME | **RV discarded at both sites** | once per NTP sync + prune | fine |
| `/system/automations.json` | `writeAutomationsJsonAtomic` `System_Automation.cpp:613` (11 call sites) | TMP+RENAME | mostly ✔ | **per automation fire** (`rescheduleAfterFire` :2434-2473 rewrites the whole file), per `recomputeAllNextAt` **per automation** (:2084), per CRUD | fine |
| `/system/notifications.json` | `notifPolicySave` `System_Notifications.cpp:183` | TMP+RENAME | ✔ | on policy change | fine |
| `/system/pet.json` | `G2_Pet.cpp:303` | TMP+RENAME | ✔ (logs) | on Pet page state change | fine |
| `/system/users/user_settings/<id>.json` | `saveUserSettings` `System_Settings.cpp:3113` | `<path>.tmp` + rename; fallback TRUNC (:3134) | `written==0` ✔ | per per-user pref change | tmp orphan, swept |
| `/system/users/ip_bans.json` | `saveIpBans` `WebServer_Server.cpp:961` | **TRUNC** | `written>0` ✔ | on ban/unban | old file survives |
| `/system/espnow/mesh_peers.json` | `saveMeshPeers` `System_ESPNow.cpp:470` | **TRUNC**, hand-rolled `println` JSON | **no write RV at all**; returns `skipped==0` | topology change / mode change / `espnowtoposave` — not periodic | old file survives on LittleFS |
| `/system/espnow/devices.json` | `saveEspNowDevices` :536 | **TRUNC**, hand-rolled | same | pairing changes | same |
| `/system/espnow/identity.json` | `writeIdentityFile` `System_ESPNow_Identity.cpp:70` | `identity.tmp` + rename | open ✔, `written==0` ✔, rename ✔, tmp cleaned on failure | identity create/regen only | **tmp orphan NOT swept** (dir not in cleanup list) |
| `/system/espnow/peers/<mac>/identity` | :397 | TMP+RENAME | ✔ | per peer identity learn | same |
| `/system/certs/https_server.crt` + `.key` | `System_WiFi.cpp:1535` + `:1543` | **TRUNC ×2, no staging, no rollback** | **neither `print()` RV checked** | `certgen` command only | **crash between the two writes leaves a cert/key mismatch** → HTTPS won't start next boot. The only genuinely two-file-invariant writer in the tree. |
| `/system/.pending_credential_setup` | `writeText` `WebServer_MigrationTool.cpp:586` | TRUNC | ✘ | migration restore only | — |

### 4.2 Capped/rotating logs

`appendLineWithCap` (`System_Filesystem.cpp:1812`) is the shared engine:
open `"a"` → append → `noteLittleFsBytesWritten` → if `size > cap`, stream the tail (85 % of cap)
into `<dest>.tmp` in 1 KB PSRAM-buffered chunks → **`VFS::remove(dest)` then `VFS::rename(tmp,dest)`**
(:1899-1908). The remove-before-rename is a genuine window (the code says LittleFS requires it — it
does **not**; only FAT does). `cleanupLogOrphan` (:1923) recovers it at boot for the six registered
logs. Neither the append nor the rotate copy checks a write return value.

| Path | Cap | Writer | Freq | Orphan recovered at boot? |
|---|---|---|---|---|
| `/system/sys_logs/errors.log` | 256 KB | `System_Debug.cpp:308` (debug_out task, 2 s dedupe window) | per distinct `[ERROR]` line | ✔ |
| `/system/sys_logs/system-events.log` | 256 KB | :322 | per `[EVENT]` | ✔ |
| `/system/sys_logs/events.log` | 256 KB | :332 | per `[EVLOG]` (opt-out via `eventlog 0`) | ✔ |
| `/system/sys_logs/successful_login.log` | ~680 KB | :2333, :2399 | login + boot marker | ✔ |
| `/system/sys_logs/failed_login.log` | ~680 KB | :2334, :2400 | failed login + boot marker | ✔ |
| `/system/sys_logs/i2c_errors.log` | 64 KB | :2377, :2415 | per I2C error/recovery | ✔ |
| **`/system/sys_logs/command-audit.log`** | **500 KB** | `System_Utils.cpp:939` `logCommandExecution` | **every non-quiet CLI/web/BLE/ESP-NOW/MQTT/voice command** | **✘ — not in the `cleanupLogOrphan` list (`System_Filesystem.cpp:94-99`) and `/system/sys_logs` is not in the `.tmp` sweep dirs. A power cut inside its rotation window loses the whole audit log and strands a permanent `.tmp`.** CONFIRMED gap. |

Other log-ish writers:
- **Live system log** (`log start`) — `System_Debug.cpp:254-283`. Holds `gSystemLogFile` **open**
  across writes; flushes only every **20 messages or 5 s** (`:96-97`). No flush/close on the reboot
  path (`System_Utils.cpp:2105 ESP.restart()` after a bare `delay(flushDelayMs)`), so up to 20 lines
  / 5 s are lost on a commanded reboot as well as on power loss.
- **Automation log** — `appendAutoLogEntry` `System_Utils.cpp:972`, `"a"`, overflow-aware, `print()`
  RV checked (`written > 0`), writes under the **captured owner's** AuthContext.
- **`/battery.csv`** — `batteryLogAppend` `System_Battery.cpp:645`, `"a"`, 64 KB cap, 2 rotations,
  **`gSettings.batteryLogEnabled` default on, `batteryLogIntervalMs` default 60 s**. Writes to the
  LittleFS **root**, no `resolveOverflowPath`, no free-space check, no write RV check.
- **Sensor log** — `System_SensorLogging.cpp:894`, `"a"`, overflow-aware, header-on-create checked
  (`writeHeaderChecked`), per-tick at `gSensorLogIntervalMs` (**default 5000 ms**, min 100 ms per the
  registry row :2119). Rotation is remove-oldest → shift `.i`→`.i+1` → base→`.1`, unchecked renames.
  Day-rollover re-points the path in the same locked section.
- **Time anchors** — `/logging_captures/sensors/.anchors.csv`. `writeAnchorLine`
  (`System_TimeAnchors.cpp:132`) appends once per boot; `registryPersist` (:99) writes
  `.anchors.tmp` + rename but **discards the rename RV and leaks the tmp on failure**, and that dir
  is not in the boot `.tmp` sweep.

### 4.3 Bulk / opt-in data

| Path | Writer | Pattern | Notes |
|---|---|---|---|
| `/espnow/received/<peer>/<file>` | `System_ESPNow.cpp:4437` (RAM path) | staging `.part-<tok>-<hash>-<msgid>` → verify `wn == recvBytes` → rename; tmp removed on any failure | **best-behaved writer in the tree** — full length verified before the rename, sender NACKed on failure |
| same (streaming path) | `System_ESPNow_Files.cpp:600` `streamDrainPending` | `.part` held open for the whole transfer, `wn == len` checked per buffer, `streamFailed` latch | orphan `.part-*` swept at boot (cap 8) |
| `/system/manifests/<fwhash>.json`, `/system/espnow/peers/<mac>/{settings,schema}.json` | `System_ESPNow.cpp:6523, 6614, 7114` | TRUNC | `written != len` **is** checked — but after the truncating open, so a short write leaves a truncated cache file that is only detected, not repaired |
| `/system/_settings_out.json`, `_schema_out.json`, manifest staging | :6754, :6883, :6980 | TRUNC | staging files for outbound transfers; `.json` suffix means the boot `.tmp` sweep never removes them — they persist as junk in `/system` |
| `/sd/espnow/capture-*.log` | :6063 | `"a"`, open+close **per frame**, 16 MB part rollover | SD only, off by default (`espnow.captureToSd`) |
| `/logging_captures/photos/*`, `/sd/...` images | `System_ImageManager.cpp:226/240/261/276` | TRUNC | the only writer with a real pre-write free-space gate (:180-208); **`f.write()` RV not checked — `success = true` is set unconditionally** |
| `/sd/VIDEOS/VID_*.AVI` | `System_Camera_Video.cpp:384` | long-held handle, periodic `flush()` (:286) | SD; `noteSDWriteFailure()` on open failure |
| `<recdir>/rec_*.wav` | `System_Microphone.cpp:383` | long-held handle; placeholder header rewritten at stop | header patch at close is the crash-exposed step |
| `/sd/g2_mic-*.lc3` / `.wav` | `G2_Glasses.cpp:14577`, `:14706` | long-held handle | SD |
| `/maps/waypoints_<base>.json` | `System_Maps.cpp:2421` | TRUNC, `serializeJson` RV **discarded** | writes under `currentAuthContext()`, not systemAuth |
| `/logging_captures/tracks/track-*.csv` | `:2955`, merge `:3307` | TRUNC, new filename each time | write RVs unchecked |
| `/sd/ESPSR/commands.txt` | `System_ESPSR.cpp:1610` | TRUNC | SD-only → **genuinely power-unsafe** (FAT, no COW) |
| web file write / upload | `WebServer_Server.cpp:1852`, `:2102` | TRUNC | :1852 checks every chunk (`written != chunk` → error); upload path flushes periodically (:2321) |
| `System_FileManager.cpp:286`, `:370` | TRUNC | user-driven file create/write |

---

## 5. NVS

**Exactly one application NVS user in the entire tree.**

`System_BootState.cpp` — namespace `"bootstate"`, key `"bootcount"` (u32).
- `bootStateInit()` :13 — `nvs_flash_init()`, and on `NO_FREE_PAGES`/`NEW_VERSION_FOUND`
  **`nvs_flash_erase()` + re-init** (:15-23). Failure is non-fatal: `sNvsReady` stays false and every
  accessor degrades to 0 / no-op.
- `writeCount()` :44 — `nvs_open(RW)` → `nvs_set_u32` → `nvs_commit` → `nvs_close`. Open RV checked
  and logged; `set` RV gates the commit; **`nvs_commit` RV discarded**.
- **Frequency: exactly once per boot** (`bootStateIncrementBootCount` from `loadAndIncrementBootSeq`,
  `System_Filesystem.cpp:194`), plus `bootStateResetBootCount()` on demand. P36 (NVS wear) is a
  non-issue here.
- Power loss mid-write: NVS is log-structured with per-entry CRC — an uncommitted entry is ignored,
  the previous boot count survives. Worst case the counter repeats one value.

Other NVS consumers are **framework-owned, same partition** (P37 applies but is unavoidable at
24 KB / 6 pages): `CONFIG_ESP_WIFI_NVS_ENABLED=y` (`sdkconfig:1852`) → WiFi driver config;
`CONFIG_BT_BLE_SMP_ENABLE=y` (:806) → BLE bonding keys; PHY calibration.

`Preferences prefs;` is declared at `HardwareOne.cpp:381` and `<Preferences.h>` is included there and
in `System_ESPNow.cpp:19` — **`prefs` has zero uses**; dead globals, not a persistence path.

RTC-backed (survives soft reset, **not** power-off) — not flash, listed for completeness:
`HardwareOne.cpp:1231-1240` (`rtcCrashCount`, `rtcLastResetReason`, `rtcMagic`, reboot-reason stash)
and `System_RamFlush.cpp:89-93` (resume overlay, magic `'RAMF'` + FNV CRC + layout version).

---

## 6. Ranked open questions / candidate findings for the follow-up pass

1. **CONFIRMED — `command-audit.log` rotation has no boot recovery.** `System_Utils.cpp:939` uses
   `appendLineWithCap`, whose rotation does `remove(dest)` then `rename(tmp,dest)`
   (`System_Filesystem.cpp:1899-1905`), but the path is absent from the `cleanupLogOrphan` list
   (:94-99) and `/system/sys_logs` is absent from the `.tmp` sweep dirs (:139-144). A reset inside
   that window destroys the always-on audit trail and strands a permanent orphan.
2. **CONFIRMED — `serializeJson`-to-`File` short writes are undetected.** ArduinoJson 7's
   `CountingDecorator` (`Serialization/CountingDecorator.hpp`) accumulates whatever `Print::write`
   returns and never aborts. Every writer that checks only `bytesWritten == 0`
   (`System_Settings.cpp:1077`, `:1169`, `:3126`; `System_ESPNow_Identity.cpp:113`;
   `System_User.cpp:1327`; `WebServer_Server.cpp:963`) will happily rename a truncated file over a
   good one when flash is full. Same class: `writeText` (`System_Utils.cpp:812`) discards
   `f.print()` entirely, so `writeTextAtomic` inherits it for `users.json`, `automations.json`,
   `pending_users.json`, `boot_anchors.json`, `notifications.json`, `pet.json`.
3. **CONFIRMED — HTTPS cert/key pair is written non-atomically across two files.**
   `System_WiFi.cpp:1535` and `:1543`, both TRUNC, neither RV checked, no staging or rollback. A
   crash between them leaves a cert that doesn't match the key.
4. **CONFIRMED — the `remove()` before `rename()` in `appendLineWithCap` is unnecessary on
   LittleFS.** `lfs_rename` replaces an existing destination atomically; only FAT/SD needs the
   remove. Gating the remove on `getStorageType() == SDCARD` would close the window entirely and
   make `cleanupLogOrphan` case (c) unreachable on flash.
5. **CONFIRMED — automations.json is rewritten in full on every automation fire**
   (`System_Automation.cpp:2434-2473`) and once **per automation** during `recomputeAllNextAt`
   (:2084). `intervalMs` has no enforced floor (:1111-1115) and `nextFire` uses
   `intervalMs / 1000` (:2318), so a sub-second interval trigger rewrites the file every scheduler
   tick — a read + `serializeJsonPretty` + tmp write + rename each time.
6. **CONFIRMED (board-gated) — `sr_8mb` layout gives 128 KB LittleFS against a 100 KB overflow
   reserve.** Overflow latches essentially at boot; with no SD present the fallback is documented
   silent loss (`System_VFS.cpp:628-631`). Does not affect the primary FeatherS3 16 MB layout.
7. **KNOWN, unchanged — boot "corrupt JSON" check deletes on a first-character heuristic and still
   lists the dead root path `/settings.json`** (`System_Filesystem.cpp:175-186`;
   `docs/SETTINGS_LIFECYCLE_AUDIT.md` §4.3).
8. **KNOWN, unchanged — a single failed LittleFS mount formats the whole data partition**
   (`System_Filesystem.cpp:60-69`; audit §4.2), and a failed format hangs boot forever
   (`HardwareOne.cpp:1310-1313`).
9. **KNOWN, unchanged — every successful WiFi connect rewrites settings.json** to store a
   boot-relative `millis()` (`System_WiFi.cpp:1099` → `saveWiFiNetworks` :850; audit §4.8).
10. **KNOWN — status CHANGED to FIXED:** audit §4.1 ("corrupt settings.json silently replaced with
    defaults next boot"). `HardwareOne.cpp:1374-1381` now uses plain assignment for the crash
    counters and `gSettingsLoadedOk` (`System_Settings.cpp:572, 960`) protects the WiFi array. Do
    not re-file.
11. **PLAUSIBLE — asymmetric manual `fsLock`/`fsUnlock` can release an outer `FsLockGuard`.**
    `System_Mutex.cpp:104-114` vs `:83-101`. `writeSettingsJson` uses three manual pairs. I found no
    concrete caller holding an outer guard, so this needs a call-graph pass before it is filed.
12. **Minor — unswept `.tmp` dirs**: `/system/espnow` (`identity.tmp`),
    `/system/espnow/peers`, `/logging_captures/sensors` (`.anchors.tmp`); plus `/system/_*_out.json`
    staging files that the `.tmp`-suffix sweep can never match.
13. **Minor — `/battery.csv` writes to the LittleFS root every 60 s by default** with no overflow
    routing and no free-space check (`System_Battery.cpp:637-705`).
