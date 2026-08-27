# Verified audit — filesystem write locking (`FsLockGuard`)

**Date:** 2026-08-02  
**Scope:** `components/hardwareone/` write paths that touch LittleFS or SD via `VFS` / `File`  
**Trigger:** Concurrent video + mic recording produced an unplayable AVI (`VID_257816.AVI`: header never finalized, `movi` size stayed 0, event log claimed 398 frames while only ~157 complete frames landed on disk)

This document records **verified** lock status only. No fixes are prescribed here.

---

## 1. Lock semantics (confirmed)

| API | What the lock covers |
|---|---|
| `VFS::open` / `VFS::openGuarded` | `FsLockGuard` only for the **open** syscall (`System_VFS.cpp` ~397–409). Returns a live `File`. |
| Later `File::write` / `seek` / `flush` / `close` / `print` | **Unlocked** unless the caller holds `FsLockGuard` or `fsLock()`/`fsUnlock()` (same `gFsMutex`). |
| `VFS::rename` / `remove` / `mkdir` / `rmdir` | Lock held for the **full** operation. |
| Direct `SD.open` outside VFS | App code: **none**. Only inside `System_VFS.cpp` (writable probe under `FsLockGuard` via `VFS.sdProbeLazy`, plus the locked `VFS::open` SD dispatch). |
| `fopen` / `fwrite` | **None** found under `components/hardwareone/`. |

**Implication:** A long-lived `File` opened through `openGuarded`, then written without an outer FS lock, can race any other SD/LittleFS writer that *does* take `FsLockGuard` (mic WAV, sensorlog append, uploads, etc.). Private mutexes (e.g. G2 mic rec) do **not** serialize against `gFsMutex`.

---

## 2. Incident that motivated this audit

| Observation | Evidence |
|---|---|
| Player error `No frames found in AVI` | Web player (`WebPage_AviPlayer.h`) walks `LIST movi` using the LIST **size** field. Size `0` → zero frames. |
| On-disk AVI | `VID_257816.AVI` ~291 KB; RIFF size / `movi` size / `avih` frame count all still **0**; dims *were* patched (176×144); **157** complete `00dc` frames + truncated tail; no `idx1`. |
| Event log | `video_saved … 398 frames` — in-RAM `s_frameCount` ahead of bytes on disk. |
| Concurrent activity | Video recording overlapped mic start/stop/source switch and sensor logging. |

**Root cause (video path):** Per-frame AVI writes ignored write return values and (before the partial fix) did not hold `FsLockGuard`, while mic recording holds `FsLockGuard` on every chunk.

**Partial fix already landed (not fully verified as complete):** frame write, dims patch, and finalize take `FsLockGuard`; failed frame writes stop recording. **Still unlocked:** AVI skeleton header write+flush at `startVideoRecording` (`System_Camera_Video.cpp` ~467–468). Avi player also gained a `movi size==0` EOF scan fallback.

---

## 3. Scoreboard

| Class | Count |
|---|---|
| Streaming / hot-path writers **without** outer FS lock | 6 |
| High-rate optional SD append without FS lock | 1 |
| One-shot / rare writers without outer FS lock | 7 |
| User-asked areas verified **locked** or non-writers | see §5 |
| Heuristic false positives corrected by re-read | 4 |

---

## 4. Verified unlocked writers

### 4.1 Streaming / concurrent risk (same class as the AVI bug)

| # | Location | What | Lock actually held | Notes |
|---|---|---|---|---|
| 1 | `G2_Glasses.cpp` ~2044–2054 | G2 raw LC3 → SD (`g2micrec`) | Private `gMicRecMutex` only | Open at ~15877; append on BLE notify path. |
| 2 | `G2_Glasses.cpp` ~1985–1999, ~1744–1752, ~16014 | G2 LC3→WAV → SD (`g2micwav`) | Private `gMicWavMutex` only | Seek+patch on close; header written at start under private mutex. |
| 3 | `System_Camera_Video.cpp` ~440–468 | AVI open + skeleton header + flush | `s_recCtrl` only (recorder start/stop mutex), **not** FS lock | Frame/finalize/dims paths now use `FsLockGuard`. |
| 4 | `System_ESPSR.cpp` ~1189–1198 | Voice-snippet WAV writer task | None | Can write under `/sd/ESP-SR Models/snips` (or internal). |
| 5 | `System_ImageManager.cpp` ~242–245, ~263–266 | JPEG save to SD | None on SD path | LittleFS save **is** under `FsLockGuard` (~227, ~277). BOTH-mode SD write sits *after* the LFS guard scope ends. |
| 6 | `G2_Glasses.cpp` ~20916–20920 | G2 camera-stream snapshot → `/sd/PICTURES/cam_*.jpg` | None | Comment mentions VFS/`FsLockGuard` for open routing only. |

### 4.2 High-rate if enabled

| # | Location | What | Lock | Notes |
|---|---|---|---|---|
| 7 | `System_ESPNow.cpp` ~6064–6067 | Per-frame ESP-NOW capture append | None | Path `/sd/espnow/capture-<ts>.log`; gated by `espnowCaptureToSd` (off by default). |

### 4.3 One-shot / rare (same pattern, lower concurrent risk)

| # | Location | What | Lock | Notes |
|---|---|---|---|---|
| 8 | `System_SensorLogging.cpp` ~1242–1255 | Session create + CSV/header write | None | Steady-state **append** path *is* locked (~912). |
| 9 | `System_Utils.cpp` ~996–1000 | Automation log append | None | May overflow to `/sd` via `resolveOverflowPath`. |
| 10 | `System_ESPSR.cpp` ~1610–1624 | Save MN commands → `/sd/ESPSR/commands.txt` | `lockMN()` only | Function name `saveCommandsFileLocked` refers to MultiNet lock, **not** FS lock. |
| 11 | `System_User.cpp` ~1320–1326, ~1527–1533 | Rewrite `pending_users` on approve/deny | None on rewrite | Approve empty-file **remove** branch takes `fsLock` (~1315); rewrite branch does not. |
| 12 | `System_WiFi.cpp` ~1537–1551 | HTTPS cert + key PEM write | None | Rare setup. |
| 13 | `System_Filesystem.cpp` ~689–694 | `filecreate` empty create+close | None | CLI one-shot. |
| 14 | `System_Microphone.cpp` ~421 | `recordingFile.close()` if task create fails | None | After open/header guards have been released; rare failure path. |

---

## 5. Verified locked (or non-writer) — answers to “what about …?”

| Area | Verdict | Evidence |
|---|---|---|
| Ring health / `G2_Health` / `G2_Ring` | No direct `File` / write-mode `openGuarded` | Grep over those files: no matches. Health CSV goes through sensor logging. |
| Sensorlog append / merge / capturecrypt export | Locked | `FsLockGuard` at `System_SensorLogging.cpp` ~912, ~2130, ~2289. |
| Web upload / `handleFileWrite` | Locked | `WebServer_Server.cpp` ~1896, ~2150 (`fsLock` held across stream writes). |
| Map organize | Locked per rename; outer guard on CLI scan | `VFS::rename` holds `FsLockGuard`; `cmd_maporganize` ~3696. |
| GPS track save / merge | Locked | `fsLock("gpstrack.save")` ~2940; `FsLockGuard("gpstrackmerge")` ~3295. |
| Waypoint save | Locked | `FsLockGuard("WaypointManager.saveWaypoints")` ~2378 spans write ~2422. |
| PDM mic record chunks + finalize | Locked | `System_Microphone.cpp` ~292, ~326 (also open/header ~382, ~395). |
| ESP-NOW streaming `.part` receive | Locked | `System_ESPNow_Files.cpp` ~596, ~753. |
| ESP-NOW peers/devices/caches/temp sends | Locked | e.g. `System_ESPNow.cpp` ~478, ~542, ~4432+, ~6523+. |
| Settings / debug JSON | Locked | `fsLock("settings.write")` etc. |
| Battery log | Locked | `System_Battery.cpp` ~671–704. |
| Debug system log append | Locked | `System_Debug.cpp` ~272–298. |
| Time anchors | Locked | `System_TimeAnchors.cpp`. |
| FileManager create/write | Locked | `System_FileManager.cpp`. |
| `writeText` / `appendLineWithCap` | Locked | `System_Utils.cpp` ~803; `System_Filesystem.cpp` ~1823. |

---

## 6. Heuristic false positives (corrected on re-read)

A naive “open write site without `FsLockGuard` in the previous 25 lines” scan mis-labeled several sites:

| Site | First pass | After reading scopes |
|---|---|---|
| `System_ImageManager.cpp` ~242 (SD in BOTH mode) | Looked locked (LFS guard ~227 nearby) | **Unlocked** — LFS guard scope already ended. |
| `System_User.cpp` ~1320 (approve rewrite) | Looked locked (`fsLock` ~1315) | **Unlocked** — lock is only on the empty-file remove branch. |
| `System_Maps.cpp` ~2422 / ~3323 | Looked unlocked (guard >25 lines above) | **Locked** — outer `FsLockGuard` spans the write. |
| `System_SensorLogging.cpp` ~2156 (merge out) | Looked unlocked | **Locked** — `FsLockGuard("healthlogmerge")` ~2130. |

Lesson: lock status must be judged by **RAII scope / branch**, not proximity.

---

## 7. Long-lived `File` handles (inventory)

Handles kept open across tasks/packets. Whether each write is FS-locked matters more than open alone.

| Handle | Owner | FS-locked on write? |
|---|---|---|
| `s_file` (AVI) | `System_Camera_Video` | Yes on frame/finalize/dims; **no** on start header |
| `recordingFile` (PDM WAV) | `System_Microphone` | Yes on chunk/finalize |
| `gMicRecFile` / `gMicWavFile` | `G2_Glasses` | **No** (private mutex only) |
| `gStreamFile[]` | `System_ESPNow_Files` | Yes |
| `gSystemLogFile` | `System_Debug` | Yes (`fsLock` around append/flush) |

---

## 8. Method

1. Enumerate write-mode opens (`openGuarded` / `VFS::open` with `"w"` / `"a"` / `FILE_WRITE`) and `.write`/`.print`/`.flush` on `File` under `components/hardwareone/`.
2. For each site, read surrounding function scope for `FsLockGuard` / `fsLock` (not private mutexes, not auth-only guards).
3. Confirm `VFS::open` / `rename` / `remove` / `mkdir` lock behavior in `System_VFS.cpp`.
4. Confirm absence of app-level `SD.open` / `fopen` writers outside VFS.
5. Re-check false positives where a lock sat nearby but in another branch or closed scope.

Line numbers are approximate (± a few) and may drift; citations above were re-read on 2026-08-02 against the tree that already contained the video frame/finalize lock fix.

---

## 9. Out of scope / not claimed

- Whether SDMMC/SPI driver itself is re-entrant under the hood.
- Performance cost of holding `FsLockGuard` on every video/G2-mic chunk.
- Fix order or patches (intentionally omitted until requested).
- Components outside `components/hardwareone/` (Arduino libs, ESP-IDF FatFs internals).
