# Health Data At-Rest Encryption — Plan

Status: implemented (increments 1–3), HW test pending
Scope decision: runtime protection. Flash-dump protection is explicitly out of
scope — the user will enable ESP-IDF flash encryption at a later date, which
closes the physical-dump gap (remember to enable CONFIG_NVS_ENCRYPTION at the
same time so the key partition is covered too).

## Goal

R1 ring health captures (heart rate, HRV, SpO2, skin temp, wear state) are
encrypted at rest on LittleFS and the SD overflow mirror, and every surface
that presents them can honestly mark them as encrypted.

## Threat model (what this defends against, and what it doesn't)

Captures are already ACL-restricted (admin+ read only; users/guests get
nothing — System_Filesystem.cpp:1371). Given that, at-rest sealing adds:

1. **SD card removal** — the overflow mirror (/sd/logging_captures) is FAT on
   removable media; no ACL survives a card reader. Sealed rows do.
2. **ESP-NOW mesh pulls** — FS_GET / espnowfetch / OLED send-file all run
   under SYSTEM identity and byte-pump file contents to peers. Peers now
   receive ciphertext (per-device key).
3. **Sub-admin roles and authz bugs** — defense in depth against path/auth
   bypasses (e.g. the open FTS AuthBypass in the security backlog): a bypass
   yields ciphertext, not vitals.
4. **Byte-level web/BLE surfaces** — /api/files/read and `fileread` ship raw
   bytes; those bytes are now sealed.

NOT defended (accepted): physical flash dump (key sits in NVS until flash
encryption lands); a compromised admin session (admins see decrypted views by
design); RAM contents (plaintext rows transit PSRAM exactly as today).

## Key custody

- Random 256-bit key, generated on first seal via libsodium `randombytes_buf`.
- Stored as an NVS blob (namespace `hw1cap`, key `k1`) — NVS is deliberately
  NOT reachable through any file-serving surface, unlike a /system key file
  which the web file manager could download. Precedent: System_BootState.cpp.
- Held in a static buffer in internal DRAM only (never PSRAM — probeable).
- No key rotation in v1: rotation orphans every existing sealed file. Erasing
  flash + NVS mints a fresh key (old files are gone anyway — no-backcompat).
- Loss behavior: NVS erased but files kept ⇒ rows render as
  `[undecryptable row]`. Honest, non-fatal.

## File format v1 (per-line sealed records)

```
#HW1ENC v1 k1\n                      <- plaintext magic (the at-rest "mark")
timestamp_ms,r1_connected,...\n      <- plaintext column header (schema, not data)
ENC1:<base64(nonce12 || ct || tag16)>\n   <- one per data row
```

- AEAD: ChaCha20-Poly1305-IETF (libsodium, already linked unconditionally —
  components/hardwareone/CMakeLists.txt:373). Fresh random 12-byte nonce per
  row; constant AAD `"HW1ENC1"`.
- **Filenames do not change.** An `.enc` rename would ripple through
  shapeSessionPath/stripSessionShaping, the boot-file promote parser
  (parseBootFile), G2 extension routing, and the persisted base path.
  Detection is by magic first line only.
- Plaintext column header keeps resolveSessionTarget's append-compat compare
  cheap, and keeps the two 15-line track-sniffers (WebPage_Maps.cpp:246,
  OLED_Mode_Map.cpp:604) classifying files exactly as they do today
  (comma-detection sees the header line).
- TEXT sessions: magic line only, then sealed prose lines. TRACK sessions
  (only sealed under mode=all): magic, then the plaintext structural comment
  header, then sealed rows.
- Why per-line: append-only writes keep working; a torn tail line loses ONE
  sample (tag verify fails, viewer marks it); rotation's byte accounting is
  untouched; and byte-concatenation (healthlogmerge) of sealed files is still
  a valid sealed stream — every line stays independently decryptable, and the
  decoder tolerates interleaved magic/header lines mid-file.
- Overhead: ~2x row size (28 B AEAD + base64). At the 45 s Health Track
  cadence ≈ 250 KB/day vs ~130 KB. Rotation caps absorb it.

## The one rule for consumers

**Presentation surfaces decrypt after their existing auth gate; byte surfaces
never decrypt.** No new role logic anywhere: if a surface's existing guarded
read succeeds AND the surface renders for a human, it reveals; anything that
ships bytes (download, fileread, ESP-NOW file streaming, backups) ships
ciphertext. SYSTEM-identity byte pumps (FS_GET, OLED espnow-send) stay pumps.

Escape hatch for legitimate off-device use: `capturecrypt export "<in>"
"<out>"` (admin) writes a decrypted copy, which can then be downloaded/sent
deliberately.

## Writer changes (all in System_SensorLogging.cpp)

Every session — healthtrack, autostart, manual CLI — funnels through
`cmd_sensorlog start`, and every byte on disk passes through three sites:

| Site | Change |
|---|---|
| `writeHeaderChecked` (:182) | When session is sealed: write magic line first, then the normal header (CSV/TRACK). TEXT gains the magic line (it is otherwise headerless). |
| Row append (:916) | Seal `line` into a PSRAM scratch before `f.write` when session is sealed. |
| `resolveSessionTarget` (:243) | Teach the probe the format: candidate missing/empty → adopt with current mode. First line magic → sealed file; require session sealing on AND second line == expected header, else next variant. First line not magic → plaintext file; require sealing off, else next variant. TEXT/TRACK (today early-return) get the same sealed-state match check when a non-empty candidate exists. A sealed/plain mismatch forks a `-2..-9` variant exactly like a mask mismatch — never refuses to capture, never mixes modes in one file. |

Session sealed flag (`gSensorLogSealed`) is derived at session start from
`captureEncryptMode` + mask, re-derived on rollover (rollover re-runs
resolveSessionTarget). Mid-session mode changes take effect at the next
session/rollover — a single file is always all-sealed or all-plain.

Content-agnostic machinery verified untouched: rotation (`path + ".N"`),
SD overflow resolution, the time-anchor sweep (filename parsing + rename
only — System_TimeAnchors.cpp:196), size seeding (f.size() only), and
`.anchors.csv` itself (written by TimeAnchors' own tmp+rename writer, never
sealed — its boot-time systemAuth reader keeps working).

## Setting + command

- `gSettings.captureEncryptMode` int 0..2 — 0=Off, 1=Health (default: seal
  sessions whose mask includes LOG_R1), 2=All (every capture session).
  Registered in `sensorLogSettingEntries` (jsonKey `captureEncryptMode`,
  cmdKey `capturecrypt`) so web/OLED/G2 settings editors pick it up
  automatically.
- `capturecrypt` command (admin): bare/`status` (mode, key present, active
  session sealed?), `off|health|all`, `export "<in>" "<out>"`.
- Marking: `healthstatus` text + `buildHealthStatusJson` gain
  `atRestEncryption` (off/health/all) — this is the product-facing "health
  data is marked as encrypted" bit, served on CLI/BLE/`/api/health/status`.

## Consumer matrix (from the full read-surface inventory)

| Consumer | Path | Behavior with sealed files | Change |
|---|---|---|---|
| Web view `/api/files/view` (themed + `mode=raw`) | WebServer_Server.cpp:4106 | Themed: decrypts per line after existing auth, banner on top. Raw: unchanged ciphertext | dec branch in text renderer |
| Web read `/api/files/read` (= download) | WebServer_Server.cpp:1635 | Ciphertext (byte surface) | none |
| Logging page viewer | WebPage_Logging.h:1101 | JS detects magic in raw fetch → refetches `/api/files/view?...&mode=raw&dec=1`; server decrypts only for authorized ctx | small JS + dec param |
| CLI `fileview` | System_Filesystem.cpp:700 | Reveals in place after readTextLimited (admin-gated cmd), banner line | reveal hook |
| CLI `fileread` (BLE app download) | System_Filesystem.cpp:866 | Ciphertext (byte surface, windowed reads can split lines) | none (documented) |
| G2 Files text viewer | G2_Page_Files.cpp:613 | Reveals in place after readTextLimited (existing canRead gate), info overlay shows Encrypted | reveal hook + info |
| OLED file viewer | OLED_Mode_FileBrowser.cpp:822 | Reveals after FileManager::readFile (guarded by OLED identity) | reveal hook |
| `healthlogmerge` | System_SensorLogging.cpp:1995 | Sealed+sealed concat = valid sealed stream. Sealed+plain refused (mode-mix guard) | first-byte sniff guard |
| `gpstrackmerge` | System_Maps.cpp:3260 | Tracks sealed only under mode=all; same caveat documented | none in v1 |
| ESP-NOW FS_GET / `espnowsendfile` / OLED send | System_ESPNow.cpp:13269 | Byte pump ships ciphertext; peer cannot decrypt (per-device key) — use `capturecrypt export` for deliberate sharing | none (by design) |
| Remote CLI (`espnowbrowse`/`espnowfetch` credentialed exec) | System_ESPNow.cpp:~5904 | Remote `fileview` reveals only under that user's role (same gate as local); `fileread`/fetch ship ciphertext | covered by fileview hook |
| Bond bridge `fs/get` → local view of pulled file | WebPage_Bond.cpp:2066 | Foreign-key file: every line renders `[undecryptable row]` + banner — graceful, honest | decoder tolerance |
| Track sniffers (web + OLED pickers) | WebPage_Maps.cpp:246, OLED_Mode_Map.cpp:604 | Unchanged classification under mode=health (header line still has commas / no gps: lines). mode=all hides sealed TRACKs from pickers — documented caveat | none in v1 |
| `GPSTrackManager::loadTrack` | System_Maps.cpp:2838 | Sealed track (mode=all) loads zero points — same failure as any non-track file | none in v1 |
| Time anchors registry + sweep | System_TimeAnchors.cpp:53/:196 | .anchors.csv never sealed; sweep is filename-only | none (verified) |
| Compat probe / size seed | System_SensorLogging.cpp:243/:220 | Probe taught the format (above); size seed reads f.size() only | probe change |
| Listings (web/G2/OLED/espnow) | System_Filesystem.cpp:243 etc. | No per-file badge in v1 — would cost one open+sniff per entry in 256-file dirs. Marking lives in viewers, G2 file-info, healthstatus. Insertion points recorded below if wanted later | deferred |
| Backup `/api/backup` | WebServer_MigrationTool.cpp:225 | Does not walk captures (verified) | none |

Deferred listing-badge insertion points: buildFilesListing per-file JSON
(System_Filesystem.cpp:389), FileEntry struct (System_FileManager.h:49) +
loadDirectory (:420), G2 buildRows (G2_Page_Files.cpp:237), espnow
V4PayloadFsEntry (wire-format change — System_ESPNow_FsList.cpp:686).

## Decoder tolerance rules (shared helper)

Line-oriented reveal, applied in place (plaintext is always shorter than its
sealed line):

- `#HW1ENC` line → kept (it is the mark).
- `ENC1:` line → AEAD open; on success replace with plaintext; on failure
  (torn tail, foreign key) replace with `[undecryptable row]` and continue.
- Anything else (headers, merged-in headers, plaintext files) → pass through.

Scratch buffers are lazily ps_alloc'd (PSRAM — plaintext rows already transit
PSRAM via buildCSVFromSnap) behind a mutex; the key alone stays internal.

## New module

`System_CaptureCrypto.{h,cpp}`: key ensure/load (NVS), sealLine, openLine,
magic/prefix predicates, `captureCryptoRevealText(String&)` and
`captureCryptoRevealBuffer(char*, len)` in-place helpers, file first-line
sniff, export core. Uses libsodium directly (not the ESP-NOW wrappers) so it
exists independent of ENABLE_ESPNOW; calls `sodium_init` itself (idempotent).

## Increments

1. Core module + writer integration + setting/command + healthstatus mark.
2. Reveal on fileview + web themed view + logging-page JS + dec param.
3. Reveal on G2/OLED viewers + G2 file-info + merge mix-guard + export.
4. (Optional, later) listing badges; decrypt-on-send policy switch; TRACK
   picker awareness for mode=all.

## HW test plan

- `healthtrack on` → day file starts with magic; rows are ENC1; `fileview`
  (admin) shows decrypted rows + banner; `/api/files/read` shows ciphertext.
- Logging page renders the sealed day file via dec path.
- G2 Files and OLED viewer show decrypted rows; G2 file info says Encrypted.
- Reboot mid-day → session re-adopts the sealed day file (no variant fork).
- `capturecrypt off` → next session forks a `-2` plaintext variant beside the
  sealed day file; both viewable.
- Kill power mid-write → tail row shows `[undecryptable row]`, rest intact.
- healthlogmerge two sealed days → viewable; sealed+plain → refused.
- espnowfetch of a sealed file → peer-side view shows undecryptable rows +
  banner (expected); `capturecrypt export` then fetch → plaintext.
- Overnight soak: heap steady, no TLSF events, cadence unchanged.

## Caveats / follow-ups

- BLE app `fileread` downloads of sealed files are ciphertext (by design);
  app-side viewing needs export or a future dec-aware pull.
- mode=all hides sealed TRACK files from both track pickers until taught.
- Vitals do NOT leak into events.log / notifications today (verified — paths
  and statuses only); re-check if automation vars ever gain R1 metrics.
- When flash encryption lands: enable NVS encryption, and this feature's key
  moves under hardware protection with zero code change.
