# Overnight Audit — Summary

**Nothing in the firmware was changed.** This is a report only.

- **600 raw findings** from 60 specialist finder agents, deduplicated to **505 distinct issues**
- **482 adversarial verifier verdicts** ran against them: 398 upheld, 84 refuted
- **961 verified-clean observations** recorded (things checked and found correct)
- Severity of deduplicated set: CRITICAL: 19, HIGH: 139, MEDIUM: 194, LOW: 110, NOTE: 43

> **Verification caveat.** Findings were produced by independent finder agents and then attacked by
> adversarial verifier agents instructed to refute them. In aggregate the verifiers upheld 398 and
> refuted 84 — roughly a 17% refutation rate. The run was interrupted and resumed several times, and
> the per-finding link between a verdict and its finding was not preserved in the journal, so the
> refutations could not be subtracted from individual entries. **Treat each finding's own
> `Confidence` field as the primary signal, and expect roughly one in six of these to not survive
> closer scrutiny.** Duplicate independent reports (noted inline) are a stronger signal.

## Where the detail lives

| Doc | Contents |
|---|---|
| `docs/AUTH_SECURITY_REVIEW.md` | 263 security findings |
| `docs/ESP32_PITFALL_AUDIT.md` | 45-pitfall checklist, A8 cross-core write-up, 210 platform findings |
| `docs/CODE_HEALTH_SWEEP.md` | 32 general robustness findings |
| `docs/audit-recon/` | 5 recon maps: tasks, ISRs, command surfaces × auth gates, persistence, prior audits |

---

## Critical findings

All 19 are marked CONFIRMED by their finder.

- **`blePushEvent` writes and transmits far past a 256-byte stack buffer; reachable from the non-admin `bleevent` command on every command surface**  
  `components/hardwareone/Bluetooth.cpp:2226` — `cmd_bleevent` (Bluetooth.cpp:1809, registered at :2070 with requiresAdmin=false) takes the raw CLI argument tail as `message` with no length check. Any authenticated non-admin user on serial / web `/api/cli` / MQTT / ESP-NOW remote command / G2 / OLED runs `bleevent x <2000 chars>` while a BLE client is connected. With a 2035-char message pos becomes ~2053, so the second snprintf writes ~17 bytes
- **G2 lens is the weakest surface: lens-initiated menu executes CLI as the device owner (super-admin) with zero authentication of any kind**  
  `components/hardwareone/G2_HijackCmd.cpp:177` — Physical possession of the paired glasses == super-admin CLI + unrestricted filesystem. There is no per-session credential, no PIN, no confirmation, and no BLE link-layer security anywhere in first-party code (zero hits for BLESecurity/setSecurityAuth/ESP_GATT_PERM_*_ENC under components/hardwareone/), so the link is also not cryptographically bound to the real glasses: the pairing scan matches on
- **g2_img_probe fallback puts a FreeRTOS task stack in PSRAM, then the task does LittleFS reads — guaranteed Cache-disabled panic**  
  `components/hardwareone/G2_Page_TestSuite.cpp:1109` — Every SPI-flash operation on this build disables the cache: SPI_FLASH_CACHE_NO_DISABLE = (CONFIG_SPI_FLASH_AUTO_SUSPEND || (SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA) || APP_BUILD_TYPE_RAM) and all three are off (sdkconfig:2560, 1670, 1671, 402), so spi1_start() takes the cache_disable(NULL) branch (esp-idf/components/spi_flash/spi_flash_os_func_app.c:105-124) for READS as well as writes. That pa
- **imgProbeWorker frees its own task stack, TCB, and context block while still executing on them, then calls vTaskDelete(NULL) through the freed TCB**  
  `components/hardwareone/G2_Page_TestSuite.cpp:1052` — Concretely: heap_caps_free(ctx->stack) returns the 8 KB block to TLSF, which writes free-list pointers into the block payload and may coalesce it with neighbours. Interrupts are enabled and the other core is running, so any concurrent allocation of <=8 KB can be handed this exact block while g2_img_probe is still pushing frames onto it -> silent stack/heap cross-corruption. heap_caps_free(ctx->tcb
- **XIAO Sense: camera SCCB re-acquires I2C port 1 that Wire1 already owns; the failed acquire tears down the live primary bus's SDA/SCL GPIOs**  
  `components/hardwareone/System_Camera_DVP.cpp:438` — IDF 5.5.1 i2c_common.c:134-137 logs "I2C bus id(1) has already been acquired", sets the new master's ->base to the EXISTING live bus struct, bumps s_i2c_platform.count[1] to 2, and returns ESP_ERR_INVALID_STATE. i2c_master.c:976 then does `goto err`, and err: calls i2c_master_bus_destroy(), whose first action (i2c_master.c:817) is i2c_common_deinit_pins(i2c_master->base) -- on the LIVE Wire1 bus. 
- **mic_record busy-spins at priority 5 on Core 1 whenever the audio source stops producing, permanently starving every lower-priority task on that core**  
  `components/hardwareone/System_Microphone.cpp:266` — Concrete trigger on the shipping board: user runs `openmic` then `micrecstart` with the G2 glasses as the source, then the LEFT temple drops. g2MicOnLeftDisconnect() (G2_Glasses.cpp:1824-1827) sets gMicAfeFeedActive=false and calls audioCaptureStop(nullptr); neither touches micRecording or gMicRunning, so recordingTask's loop condition stays true while audioReadPcm now returns 0 with zero delay. t
- **annotateActivityDaily() writes up to ~36 bytes past a 256-byte stack buffer on the BLE notify task (guard reserves 48 bytes, record renders up to 55)**  
  `components/hardwareone/System_R1_Protocol.cpp:829` — An R1 ring notify frame with module=R1_MODULE_HEALTH, cmd=R1_CMD_ACTIVITY, subCmd=R1_SUB_DAILY and a payload whose 7-byte records are chosen so `off` reaches 202..207 before a maximal-length record. Example: several records with steps/kcal = 0 to step `off` in ~39-byte increments, then one record with slot=255, steps=-32768, kcal=-32768, r[3]=r[4]=0xFF. `off` becomes 257..262; `snprintf(abuf+262, 
- **`remote:` / `@` command wrapper is invisible to findCommand, so authorizeCommand's admin and super-admin gates never fire — every surface can run super-admin commands on the bonded peer**  
  `components/hardwareone/System_Utils.cpp:4505` — A plain `user`-tier web account POSTs `cmd=factoryreset` to /api/bond/exec. Locally: authorizeCommand sees "remote:factoryreset", findCommand -> nullptr, no admin/super requirement, allowed. Remotely: the peer runs it as kBondAdminUser = super-admin, deletes /system/users/users.json and reboots into the setup wizard. The same wrapper is reachable from serial with serialRequireAuth off (ctx.user = 
- **writeText() returns true unconditionally, so writeTextAtomic() can commit an empty/truncated file over a good one and report success — 11 of its 18 call sites are the credential store**  
  `components/hardwareone/System_Utils.cpp:813` — On any write failure (LittleFS ENOSPC on the 2796 KB data partition, LFS_ERR_CORRUPT, an ECC/IO error) writeText leaves a 0-byte or truncated .tmp and returns true; VFS::rename is a metadata-only commit that still succeeds, so the truncated file is renamed over the good one and writeTextAtomic returns true. updateUserLastSeen() (System_User.cpp:1125-1131) runs this on EVERY successful login agains
- **executeCommand() returns true for every dispatched command, including handler errors and unknown commands — the bool is a constant, not a result**  
  `components/hardwareone/System_Utils.cpp:4834` — Every consumer that keys on the bool reports success on failure. Concretely: `mosquitto_pub -t hw/command -m '{"user":"u","pass":"p","cmd":"nosuchcommand"}'` → System_MQTT.cpp:544 gets success=true → :548 publishes `{"ok":true,"result":"Unknown command: nosuchcommand..."}`. `POST /api/waypoints action=delete index=99` → cmd_waypoint returns "Error: Invalid waypoint index" (System_Maps.cpp:3396) → 
- **ESP-NOW fragment reassembly does not bind per-fragment authentication to the message: plaintext fragments are silently merged into an AEAD-protected message**  
  `components/hardwareone/System_ESPNow.cpp:5315` — Every ESP-NOW peer is added with `peerInfo.encrypt = false` (System_ESPNow.cpp:925, :9904; Handlers_Crypto.cpp:108), so the radio layer performs no authentication and an attacker in range can transmit frames with an arbitrary 802.11 source MAC. The V4 header — including `msgId`, `fragIndex`, `fragCount`, `sessionId`, `meshFingerprint` — travels in cleartext even for SESSION_FRAME frames, so an att
- **Remote-supplied ESP-NOW peer metadata is printed unescaped into hand-rolled JSON, so one double-quote permanently destroys the paired-device registry (MACs + per-device AES keys)**  
  `components/hardwareone/System_ESPNow.cpp:585` — Fully traced, first-party trigger, no attacker needed: on device A run `espnowfriendlyname My "Lab" Node`. metaGetSet (:11500) only strips SURROUNDING quotes (:11516) so an interior quote is accepted and stored. buildMetadataPayload (:7310-7315) copies gSettings.espnowFriendlyName verbatim into the METADATA payload; sendMetadata pushes it to peers. On device B, processMetadata (:7473-7492) assigns
- **ESP-NOW remote filesystem (FS_LIST/FS_STAT/FS_GET) is served under SYSTEM identity to any paired peer with a live session — no credential, no role, no enable-gate**  
  `components/hardwareone/System_ESPNow_FsList.cpp:644` — Any peer that is securely paired and holds an active session (the only gate — `V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC`, System_ESPNow.cpp:5007-5012; deliberately NOT bond-gated) can `espnowbrowse` to `/system/users/users.json` and pull the password hashes, and to `/system/certs/*.key` and pull the TLS private key — files that the web/CLI layers deny even to a local admin. The confine
- **Non-admin `sensorlog start <path>` performs arbitrary-path create/append/rename/delete as FsRole::SYSTEM — a plain User can destroy /system/users/users.json and force the device into first-time setup (new owner = superadmin)**  
  `components/hardwareone/System_SensorLogging.cpp:1109` — A holder of a plain `user` account (rank 1, non-guest, reachable over serial, /api/cli, /api/cli/batch, BLE, MQTT, ESP-NOW or the OLED) runs: `sensorlog rotations 0` / `sensorlog maxsize 10240` / `sensorlog start /system/users/users.json 100`. sensorLogTick() (HardwareOne.cpp:2333) then appends heartbeat/sensor rows to the live auth database every interval — immediately breaking every `deserialize
- **FM-radio poll nests two manager transactions on the same bus — the inner one always self-deadlocks on the non-recursive bus mutex, so RDS/RSSI/seek-completion never run and the bus is held ~80% of the time**  
  `components/hardwareone/i2csensor_rda5807.cpp:427` — With `fmradiostart` on any build that compiles the RDA5807 in: (1) `radio.checkRDS()` / `radio.getRadioInfo()` never run — `gFmRadioCache.rssi/snr/stereo/headphonesConnected` stay at their init values forever and `gFmRadioCache.lastUpdate`/`dataValid` are never stamped, so the shared sensor envelope reports `valid:false` with a frozen `ts` on web/OLED/G2/ESP-NOW/MQTT. (2) `gFmRadioCache.seekInProg
- **Permission-check path and I/O path canonicalize differently — a non-canonical path skips every PathRule and lands on the catch-all PERM_ALL grant**  
  `components/hardwareone/System_Filesystem.cpp:1458` — Two escape forms, both confirmed by reading the code end to end.

Variant A (no leading slash) — `POST /api/files/write` with body `name=system/users/users.json&content=<forged users.json>` as a plain `user`-role account: `tgRequireAuth` passes (System_User.cpp:161 accepts any session, no role check); `webGuestAccessAllowed` only blocks guests; `isAdminOnlyPath("system/users/users.json")` resolves
- **Pre-auth 1-byte stack overflow in decodeBasicAuth: out_buf[out_len]='\0' can write index 256 of a 256-byte array**  
  `components/hardwareone/WebServer_Server.cpp:1350` — An attacker sends `Authorization: Basic <344 base64 chars ending in ==>` — 344 chars decode to exactly (344/4)*3 - 2 = 256 bytes. Total header is ~350 bytes, well under CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024 (sdkconfig:1448). Reachability is unauthenticated and unthrottled: isAuthed() calls decodeBasicAuth whenever the request carries no session cookie (:552-556), on every URI that goes through tgRequi
- **Full live web session tokens are written to the /api/cli/logs mirror, letting any authenticated non-admin steal an admin's session**  
  `components/hardwareone/WebServer_Server.cpp:376` — A low-privilege account (role "user", e.g. one created through /register + admin approval) logs in and polls `GET /api/cli/logs` — the same endpoint the CLI page polls every 500 ms. When the admin or superadmin next signs in, the response contains `[auth] setSession user=admin, sid=<32 hex chars>`. `isAuthed` (WebServer_Server.cpp:538) resolves a request purely by `findSessionIndexBySID` with no I
- **Web session cookie (sid) is broadcast in cleartext to every output sink on every login — no debug flag required**  
  `components/hardwareone/WebServer_Server.cpp:376` — Alice has the plain 'user' role. Bob (admin) signs in at the web UI. Alice GETs /api/cli/logs — handleLogs (WebServer_Server.cpp:2890) only calls tgRequireAuth + webGuestAccessAllowed, and webGuestAccessAllowed returns true immediately for any non-guest (WebServer_Utils.cpp:403), so Alice is allowed. The mirror contains `[auth] setSession user=bob, sid=<full token>, exp(ms)=...`. Alice sets that c

---

## High severity by theme


**Core system / tasks** (42)

- Confirm-mode `yes` is dispatched after authorizeCommand and the pending slot is a single global with no owner — any User-tier identity on any surface can complete another — `components/hardwareone/System_CLIConfirm.cpp:18`
- Confirm-mode `yes` has no owner check and is evaluated after authorizeCommand, so a plain User can complete a Super-Admin's pending `factoryreset` (deletes users.json, re — `components/hardwareone/System_CLIConfirm.cpp:43`
- CLIMode `sActiveMode` is written by TWO tasks (main loop + cmd_exec) despite documenting a single-writer invariant — null-deref and double-onExit windows — `components/hardwareone/System_CLIMode.cpp:21`
- srSnipDeinit force-deletes the snippet writer mid-file-write with no shutdown handshake, orphaning the global FS mutex — `components/hardwareone/System_ESPSR.cpp:1367`
- EdgeImpulse frees model buffer, tensor arena and interpreter while the continuous inference task is running — `components/hardwareone/System_EdgeImpulse.cpp:500`
- Raw remote-command text — which routinely contains cleartext credentials — is teed verbatim into /system/sys_logs/events.log, on by default — `components/hardwareone/System_Events.cpp:508`
- events.log persists remote command text verbatim, bypassing the audit-log redaction that covers the identical command locally — `components/hardwareone/System_Events.cpp:508`
- The `AuthBypass` sentinel can become a real, promotable account — first-time setup and ESP-NOW USER_SYNC both skip isValidPublicUsername's reserved-name check — `components/hardwareone/System_FirstTimeSetup.cpp:891`
- First-time setup writes the owner account without isValidPublicUsername, so the reserved sentinel `AuthBypass` can be a real superadmin account — `components/hardwareone/System_FirstTimeSetup.cpp:891`
- G2 protobuf length-varint wraparound bypasses bounds check → OOB read / crash on a crafted BLE frame — `components/hardwareone/System_G2_Protocol.cpp:1513`
- Bus 1 -- the display bus on the primary board -- has effectively no wedge-recovery path: quorum floor blocks it, void transactions never report errors, and i2creset is ha — `components/hardwareone/System_I2C.cpp:2169`
- I2C SDA/SCL pin settings accept SPI-flash/PSRAM and strapping GPIOs; a persisted bad value is a permanent boot loop — `components/hardwareone/System_I2C.cpp:711`
- Runtime-settable I2C pins accept the SPI-flash/PSRAM MSPI pins; the value is persisted and re-applied at every boot — `components/hardwareone/System_I2C.cpp:718`
- i2cOledTransactionVoid hardcodes bus 0 while the OLED lives on bus 1 by default, taking the wrong mutex, reclocking the sensor bus, and creating a phantom registry entry  — `components/hardwareone/System_I2C.h:160`
- initBus() and performBusRecovery() discard TwoWire::begin()'s bool and mark the bus 'online' unconditionally — `components/hardwareone/System_I2C_Manager.cpp:293`
- I2C error logging is unbounded per-error and the [ERROR] dedupe window is defeated by the counters embedded in the message — an I2C error storm becomes a flash-write stor — `components/hardwareone/System_I2C_Manager.cpp:845`
- void-returning transactions unconditionally recordSuccess(), so the health/degrade/recovery machinery is blind to the highest-rate bus users — `components/hardwareone/System_I2C_Manager.h:384`
- `imagedelete` deletes any absolute path as FsRole::SYSTEM, giving an ordinary Admin a route to Super Admin by wiping users.json and re-running first-time setup — `components/hardwareone/System_ImageManager.cpp:436`
- Always-on system log caps overcommit the entire LittleFS partition on every 8 MB board — `components/hardwareone/System_Logging.h:54`
- GPSTrackManager::clearTrack() frees and nulls _points before zeroing _pointCount, while lock-free readers on other cores iterate _points[i] per loop iteration — `components/hardwareone/System_Maps.cpp:2903`
- Credential material is routed through explicitly-forced PSRAM (heap_caps_malloc(MALLOC_CAP_SPIRAM)) and freed without zeroing, at sites the prior audit did not cover — in — `components/hardwareone/System_MemUtil.h:327`
- The CPU-clock I2C drain guard covers only the power-save path; `power mode` and `cpufreq` still switch raw — and the in-tree justification for leaving them raw is factual — `components/hardwareone/System_Power.cpp:106`
- All nine sensor create helpers call eTaskGetState() on a dangling TaskHandle_t whose TCB the idle task has already free()d — `components/hardwareone/System_TaskUtils.cpp:180`
- The Arduino loop is NOT on APP_CORE — setup(), loop() and all OLED I2C run on Core 0, violating the header's own "hard rule" — `components/hardwareone/System_TaskUtils.h:134`
- hardwareone_loop() runs on core 0, contradicting the documented "Arduino loop and all I2C work run on APP_CORE(1)" policy — and it does I2C — `components/hardwareone/System_TaskUtils.h:134`
- espnowroomcmd / espnowtagcmd are missing from the audit redaction table, so the target-device password is written cleartext to command-audit.log and echoed to a guest-vis — `components/hardwareone/System_Utils.cpp:1114`
- Cleartext passwords are written into PSRAM on every credential path, violating the standing no-secrets-in-PSRAM rule — and the prior audit's claim that they stay internal — `components/hardwareone/System_Utils.cpp:4792`
- The `remote:` / `@` prefix runs the wrapped command on the bonded peer as kBondAdminUser (Super Admin) with the local caller's role deliberately not re-checked — reachabl — `components/hardwareone/System_Utils.cpp:4505`
- Interactive-confirm resolution: three requesters use three different identity models, and two never consult the confirmer's privileges — `components/hardwareone/System_Utils.cpp:2180`
- The remote:/@ command wrapper defeats every audit-redaction rule, leaking typed credentials into the shared web console ring that any authenticated non-guest can poll — `components/hardwareone/System_Utils.cpp:1185`
- Enabling debugcli or debugcommandflow dumps every command's raw arguments to all sinks, bypassing redactCmdForAudit entirely — `components/hardwareone/System_Utils.cpp:4690`
- `writeText()` discards `f.print()`'s return and always returns true; `writeTextAtomic()` then renames the truncated tmp over the good file — the users.json path ends in a — `components/hardwareone/System_Utils.cpp:820`
- No write-failure detection exists in any atomic-write helper; on a full LittleFS writeTextAtomic renames an empty tmp over users.json and the device boots into the blocki — `components/hardwareone/System_Utils.cpp:812`
- executeCommand() returns true for every handler-level failure — the bool means "dispatched", not "succeeded" — `components/hardwareone/System_Utils.cpp:4760`
- Success is inferred from an "Error" string prefix, so all JSON failures and many human failure strings audit as OK and get stamped "OK: " — `components/hardwareone/System_Utils.cpp:4728`
- Guest-allowed `logout g2` re-homes the G2 lens identity to the device owner (usually Super Admin) and persists it to flash — `components/hardwareone/System_Utils.cpp:4356`
- A newline inside any command line is written verbatim into command-audit.log and (with one trigger word) successful_login.log — an authenticated non-guest can forge arbit — `components/hardwareone/System_Utils.cpp:936`
- `lightsleep` enters light sleep with WiFi, Bluedroid BLE and ESP-NOW all running, which ESP-IDF documents as unsupported — `components/hardwareone/System_Utils.cpp:1455`
- `certgen` writes the HTTPS cert and key with unchecked `print()` and no staging, then reports success — a truncated key silently downgrades the admin web UI to plain HTTP — `components/hardwareone/System_WiFi.cpp:1540`
- HTTPS silently downgrades to plain HTTP when the cert/key pair is unreadable or mismatched, and that pair is written by two independent truncating writes with no return v — `components/hardwareone/System_WiFi.cpp:1535`
- cmd_certgen discards both PEM write returns and declares success, so a failed write silently downgrades the device to plain HTTP at the next boot — `components/hardwareone/System_WiFi.cpp:1540`
- certgen reports "certificate generated and saved" without checking either PEM write — `components/hardwareone/System_WiFi.cpp:1541`

**ESP-NOW / radio mesh** (27)

- reassembledSize uses the completing frame's payloadLen as the last fragment's length — OOB read past e->buffer and stale-byte disclosure on out-of-order completion — `components/hardwareone/System_ESPNow.cpp:5407`
- Every inbound ESP-NOW remote command is broadcast unredacted through BROADCAST_PRINTF to serial, the web mirror, the system log file and the guest-visible OLED console — `components/hardwareone/System_ESPNow.cpp:5838`
- Reassembly bounds-checks the fragment index against the attacker's claimed fragCount, not the slot's allocated fragCount — OOB read of have[], 1-byte OOB write, and compl — `components/hardwareone/System_ESPNow.cpp:5347`
- captureEspNowFrame reserves 976 B on the caller's stack for every ESP-NOW frame, including on the 3584-byte input task's auto-disable path — the capture-disabled early re — `components/hardwareone/System_ESPNow.cpp:6015`
- In the default "auto" channel mode ESP-NOW has no channel anchor at all — the chosen channel is defined as "wherever the radio currently is", and the built-in drift check — `components/hardwareone/System_ESPNow.cpp:953`
- ESP-NOW CMD_RESP carries a success byte that is effectively a constant 1 — remote command failures are reported to the originating device as successes — `components/hardwareone/System_ESPNow.cpp:5679`
- Arduino WiFi layer re-applies WIFI_PS_MIN_MODEM on every STA start, silently undoing both esp_wifi_set_ps(WIFI_PS_NONE) call sites — `components/hardwareone/System_ESPNow.cpp:9738`
- ESP-NOW devices.json is written as hand-rolled JSON with unescaped, remotely-supplied strings; a single quote or backslash makes the file unparseable and permanently dest — `components/hardwareone/System_ESPNow.cpp:585`
- ESP-NOW USER_SYNC appends users.json entries with no username validation — the reserved-name guarantee documented in System_User.h is false, and a planted "bond-admin" ac — `components/hardwareone/System_ESPNow.cpp:3658`
- SENSOR_BROADCAST (opcode 150) still dispatches the same cache ingest as the session-encrypted SENSOR_ENVELOPE but with opcode flags = 0, contradicting the design doc's ex — `components/hardwareone/System_ESPNow.cpp:4975`
- ESP-NOW FS_LIST/FS_STAT/FS_GET serve the whole filesystem under SYSTEM identity with no account and no per-user authorization — this is the real "filesystem auth bypass" — `components/hardwareone/System_ESPNow.cpp:5007`
- Mesh passphrase and its unsalted SHA-256 derivative are held in PSRAM for the whole boot (gEspNow) — `components/hardwareone/System_ESPNow.cpp:9572`
- BROADCAST_AUTH frames have no replay protection; a replayed master heartbeat suppresses backup-master failover indefinitely — `components/hardwareone/System_ESPNow.cpp:5175`
- removeEspNowDevice() compacts gEspNow->devices[] from core 1 (OLED unpair) while httpd and cmd_exec iterate the same array and its Arduino String names — `components/hardwareone/System_ESPNow.cpp:8234`
- saveEspNowDevices()/saveMeshPeers() truncate the live registry in place, discard ~20 write returns each, and return true when every write failed — `components/hardwareone/System_ESPNow.cpp:536`
- Mesh passphrase and PBKDF2 stretched key are the only ESP-NOW key material with no damaged-load preservation guard, and no gSettingsLoadedOk gate — `components/hardwareone/System_ESPNow.cpp:1376`
- ESP-NOW channel 12/13 is accepted everywhere but is physically unreachable under the default country code — the radio stays on another channel while every surface reports — `components/hardwareone/System_ESPNow.cpp:988`
- ESP-NOW "auto" channel is derived from the live radio channel while unassociated, so any transient excursion is latched permanently as the new mesh home — `components/hardwareone/System_ESPNow.cpp:968`
- espnow roomcmd/tagcmd count every peer as "Sent" — the send return value is discarded entirely, and the payload is silently truncated — `components/hardwareone/System_ESPNow.cpp:11876`
- Nothing reserves free space for state writes: an ESP-NOW peer may push a declared 4 MB file with no free-space check, and web upload's ceiling is every remaining byte — `components/hardwareone/System_ESPNow_Files.cpp:247`
- Remote peer-filesystem browse/pull is implemented twice; the OLED/web copy drops the per-user role check that the CLI copy enforces, letting a plain local user read any f — `components/hardwareone/System_ESPNow_FsList.cpp:644`
- A replayed SESSION_OPEN tears down the victim's live encrypted session — no freshness check anywhere on the responder path — `components/hardwareone/System_ESPNow_Handlers_Crypto.cpp:746`
- A replayed SESSION_REKEY drives both peers into a non-converging rekey ping-pong; the anti-replay field the wire spec relies on is never actually compared — `components/hardwareone/System_ESPNow_Handlers_Crypto.cpp:1229`
- `writeIdentityFile` accepts any non-zero serialize length — a truncated ESP-NOW identity is renamed over the good one, destroying the device's Ed25519 trust anchor — `components/hardwareone/System_ESPNow_Identity.cpp:113`
- The TX-driven session self-heal is dead for every path except a human typing `espnowsend` — `components/hardwareone/System_ESPNow_Sessions.cpp:672`
- AEAD session keys, the in-flight X25519 ephemeral private key, the bond super-admin token and decrypted command credentials all live in PSRAM — `components/hardwareone/System_ESPNow_Sessions.cpp:26`
- Post-REKEY prev-key window accepts replayed old-key frames and one replay can wedge the session — `components/hardwareone/System_ESPNow_Sessions.cpp:759`

**Web / HTTP** (18)

- Bond remote-filesystem web endpoints have no local role check, and the peer serves FS_LIST/STAT/GET under SYSTEM identity with no credential in the request — `components/hardwareone/WebPage_Bond.cpp:2066`
- /api/bond/exec and /api/bond/cli/batch give any authenticated non-guest Super-Admin command execution on the bonded peer — `components/hardwareone/WebPage_Bond.cpp:1345`
- /api/bond/fs/get|list|stat lets any authenticated non-guest read arbitrary files off the bonded peer with no admin gate — `components/hardwareone/WebPage_Bond.cpp:2066`
- Bond role-swap split-brain guard is structurally inert — `remote:` returns on enqueue, never on peer execution — `components/hardwareone/WebPage_Bond.cpp:1439`
- Four web status endpoints execute command handlers inline on the unpinned httpd task, racing cmd_exec_task on the single shared gDebugBuffer result buffer — `components/hardwareone/WebPage_ESPNow.cpp:563`
- Unauthenticated ESP-NOW metadata injection reaches the admin web UI as unescaped innerHTML -> admin-privileged command execution (radio-range, no credentials, no mesh key — `components/hardwareone/WebPage_ESPNow.h:739`
- Guest accounts can start the GPS radio and create files through /api/gps/tracks — a guest-allowlisted GET that reaches executeCommandThroughRegistry and a SYSTEM-imperson — `components/hardwareone/WebPage_Maps.cpp:128`
- handleMapFeaturesAPI walks the map name table with no MapCacheGuard while unloadMap() frees it — `components/hardwareone/WebPage_Maps.cpp:90`
- Guest GET on `/api/gps/tracks?live=…` starts the GPS sensor, reconfigures and starts sensor logging, and writes a track file to flash with SYSTEM filesystem authority — `components/hardwareone/WebPage_Maps.cpp:109`
- SSE hold loop spins forever after a write failure once uptime passes 24.86 days (signed cast of a zeroed sentinel timestamp) — `components/hardwareone/WebServer_Events.cpp:249`
- Migration restore writes arbitrary bundle-chosen absolute paths verbatim, including /system/users/users.json — the attacker-reachable way to plant an "AuthBypass" superad — `components/hardwareone/WebServer_MigrationTool.cpp:519`
- GET /api/automations and /api/automations/export bypass the VFS permission table; /api/automations is additionally on the guest allowlist — `components/hardwareone/WebServer_Server.cpp:3340`
- POST /api/cli allocates from attacker-controlled Content-Length with no cap and no null check → guaranteed NULL store → panic reboot — `components/hardwareone/WebServer_Server.cpp:3194`
- Cleartext passwords and PBKDF2 hashes are staged in PSRAM on every login, registration, password change and web CLI command, and freed without zeroing — `components/hardwareone/WebServer_Server.cpp:3492`
- Brute-force lockout is bypassable: the 8-slot login-attempt table evicts the oldest entry, and a locked entry is always the oldest — `components/hardwareone/WebServer_Server.cpp:787`
- Same-LAN attacker with no credentials: plaintext HTTP is the default, so a passive sniff yields the admin session cookie/password -> full admin — `components/hardwareone/WebServer_Server.cpp:5309`
- Turning off HTTP or WiFi from the web Settings page strands gDeferWrites=true, silently disabling settings persistence device-wide until a reboot — `components/hardwareone/WebServer_Server.cpp:5101`
- `WebMirrorBuf::append`/`appendDirect` write the NUL terminator one byte past the allocation on an exact fit — `components/hardwareone/WebServer_Utils.cpp:91`

**BLE / G2 / R1** (10)

- deinitG2Client()/`closeg2 full` delete per-temple writeMutex and rxBuf, and vTaskDelete the BLE connect worker, without quiescing the core-1 G2 workers or the BTC notify  — `components/hardwareone/G2_Glasses.cpp:8700`
- LC3 audio decode runs inline on the Bluedroid BLE notify task and consumes ~6 KB of BTC_TASK's 8192 B stack in one call chain — `components/hardwareone/G2_Glasses.cpp:1880`
- Half-connected G2 recovery runs the full BLE connect + GATT-discovery chain on the 2048–3072 B heartbeat worker, while the project's own connect worker is 5120 B and mark — `components/hardwareone/G2_Glasses.cpp:8063`
- G2 mic capture writes the filesystem synchronously on the Bluedroid BLE-notify task, and the SD-availability gate does not constrain the destination — `components/hardwareone/G2_Glasses.cpp:1909`
- G2 text-entry isSecret is defeated downstream — the submit path echoes the assembled credential line at four sites, three of which are on by default — `components/hardwareone/G2_HijackCmd.cpp:138`
- G2 text-entry isSecret is defeated one hop later: g2SubmitHijackCommand logs the composed command line, password included, on always-on WARN/ERROR paths — `components/hardwareone/G2_HijackCmd.cpp:144`
- G2 command-submit echoes log full credential command lines; two of the four sites are DEBUG_ALWAYS and fire regardless of the debug flag — `components/hardwareone/G2_HijackCmd.cpp:100`
- CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is pinned in shared defaults and a live code path puts an 8 KB task stack in PSRAM, then does LittleFS I/O on it — `components/hardwareone/G2_Page_TestSuite.cpp:1108`
- g2_img_probe's PSRAM-stack fallback calls heap_caps_free on the stack it is currently executing on, then keeps running on it — `components/hardwareone/G2_Page_TestSuite.cpp:1049`
- 8 KB task stack allocated in PSRAM and handed to xTaskCreateStaticPinnedToCore, for a worker that performs LittleFS reads and Bluedroid BLE writes — `components/hardwareone/G2_Page_TestSuite.cpp:1109`

**Storage / filesystem** (8)

- Every user's PBKDF2 password hash is readable by any plain Admin — and the hashes are not in users.json, contradicting the prior hardening audit — `components/hardwareone/System_Filesystem.cpp:1343`
- exemptSensitiveExt on /system/certs/ is a no-op for its stated beneficiaries and its only real effect is handing the HTTPS private key to any plain Admin — `components/hardwareone/System_Filesystem.cpp:1355`
- appendLineWithCap rotation copies up to ~578 KB synchronously while holding the global FS mutex, on whichever task ran the command — `components/hardwareone/System_Filesystem.cpp:1812`
- `appendLineWithCap()` rotation copies the tail with an unchecked `w.write()`, then `remove()`s the original before renaming — a nearly-full FS silently destroys command-a — `components/hardwareone/System_Filesystem.cpp:1889`
- Files pulled from an ESP-NOW peer land in /espnow/received/, a path the rule table grants PERM_READ to every authenticated non-admin — so a peer's users.json password-has — `components/hardwareone/System_Filesystem.cpp:1388`
- readTextLimited() reserves the FULL byte cap before reading, ignores the reserve failure, and degrades to a per-16-byte realloc treadmill exactly when DRAM is tight — `components/hardwareone/System_Filesystem.cpp:1767`
- appendLineWithCap() deletes the original log before verifying the rotated copy, and the copy loop ignores every write return — this is the login audit trail — `components/hardwareone/System_Filesystem.cpp:1889`
- appendLineWithCap() — the engine behind every always-on log — returns success on a dropped append and can commit a truncated rotation — `components/hardwareone/System_Filesystem.cpp:1828`

**Other** (7)

- The base (non-Sense) XIAO board block is unreachable — every ESP32-S3 build force-defines XIAO_ESP32S3_SENSE_ENABLED, so a plain XIAO gets the Sense pin map — `CMakeLists.txt:236`
- Arduino `File::flush()` and `File::close()` are `void` — every fflush/fsync/fclose error in the tree is structurally undetectable, so no writer can see a full filesystem — `components/arduino/libraries/FS/src/vfs_api.cpp:352`
- Arduino File buffers 4096 bytes and discards fflush()/fclose() errors, so every "short write means the flash is full" check in this firmware is structurally inert for sub — `components/arduino/libraries/FS/src/vfs_api.cpp:336`
- A failed LittleFS commit is undetectable by any writer: File::flush() returns void and File::close() discards fclose(), so ENOSPC never reaches the caller — `components/arduino/libraries/FS/src/vfs_api.cpp:304`
- Nothing in the system can recover a hung task: TWDT panic is off with zero firmware subscribers, and 14 boot-path FATAL handlers wedge forever — `components/hardwareone/HardwareOne.cpp:1312`
- The `@` / `remote:` command wrapper defeats every redaction rule, and the wrapped credential is written straight into the web-readable command feed — `components/hardwareone/HardwareOne.cpp:498`
- Sixteen boot-path FATAL paths park the device in `while (1) delay(1000)` forever, and no watchdog exists that can reset it — `components/hardwareone/HardwareOne.cpp:1339`

**Auth / users** (6)

- No brute-force lockout on any login path except web and the serial pre-auth gate — cmd_login, BLE, OLED, ESP-NOW and MQTT are unthrottled — `components/hardwareone/System_User.cpp:448`
- User-enumeration timing oracle: isValidUser short-circuits before PBKDF2 when the username does not exist — `components/hardwareone/System_User.cpp:1146`
- The OLED 4-direction unlock pattern is a full-strength credential on every remote transport, with a keyspace as small as 256 — `components/hardwareone/System_User.cpp:1160`
- `logout g2` is guest-allowed and non-admin, and immediately re-homes the lens identity to the device owner — a guest can escalate the entire G2 surface to super-admin — `components/hardwareone/System_User.cpp:515`
- resolvePendingUserCreationTimes() reads users.json into a fixed 8 KB buffer with no truncation check, then writes the truncated copy back over the live auth database — `components/hardwareone/System_User.cpp:3216`
- Brute-force lockout lives in two per-surface inline copies instead of the shared login path, so 4 of 6 credential surfaces accept unlimited password guesses — `components/hardwareone/System_User.cpp:439`

**Sensors / I2C / HAL** (6)

- `sensorlog autostart on` persists a low-privilege user's arbitrary path and re-executes it at boot via a direct `cmd_sensorlog()` call that bypasses executeCommand and au — `components/hardwareone/System_SensorLogging.cpp:2186`
- Sensor-log row writes are unchecked while the header write is checked, so a full filesystem silently rotates real data away while recording nothing — `components/hardwareone/System_SensorLogging.cpp:909`
- apdsColorPoll()'s unbounded data-ready spin hangs cmd_exec_task permanently, taking every command transport with it — `components/hardwareone/i2csensor_apds9960.cpp:359`
- DS3231 TZ save/restore aliases getenv("TZ") into setenv("TZ") and permanently clobbers the process timezone to UTC — `components/hardwareone/i2csensor_ds3231.cpp:233`
- thermal and FM-radio sensors can never be restarted after any stop — the start path's stale-handle guard permanently blocks the only code that clears the stale handle — `components/hardwareone/i2csensor_mlx90640.cpp:200`
- STHS34PF80 presence driver wraps its bus-aware register helpers in a bus-0 transaction — every register read self-deadlocks, so the sensor can never initialise — `components/hardwareone/i2csensor_sths34pf80.cpp:334`

**Settings / config** (5)

- Device-key epoch selection is blind to the credential database and can permanently orphan every password hash — `components/hardwareone/System_Settings.cpp:1287`
- handleSettingCommand() ignores writeSettingsJson()'s result and returns "[Settings] Configuration updated" — the entire settings surface reports success on a failed persi — `components/hardwareone/System_Settings.cpp:2645`
- Mesh passphrase and its PBKDF2-stretched key are decrypted BEFORE the device-key epoch is selected, and the mesh writer destroys the merge-read blob before putSecret can  — `components/hardwareone/System_Settings.cpp:1386`
- saveUserSettings() truncates the live per-user credential file in place as its rename fallback, then reports success on an unverifiable buffered write — `components/hardwareone/System_Settings.cpp:3131`
- CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y puts every TLS secret — session traffic keys and the parsed HTTPS server private key — in plaintext PSRAM, breaking the project's own  — `sdkconfig.defaults:142`

**Automation / autostart** (4)

- Automation `createdBy` is trusted verbatim from automations.json, and the value "system" is a total authorizeCommand bypass — `components/hardwareone/System_Automation.cpp:99`
- Automation scheduler cache freed from cmd_exec_task while loopTask reads and writes it — `components/hardwareone/System_Automation.cpp:535`
- automationsAnyDue() walks gAutomationsCache on core 1 after stopAutomationScheduler() freed it on core 0 — pointer retired before the count — `components/hardwareone/System_Automation.cpp:533`
- executeConditionalCommand() scans unbounded past the end of the command string when the command is exactly "IF " or "IF x" (size_t underflow in the loop bound) — `components/hardwareone/System_Automation.cpp:3376`

**MQTT** (3)

- MQTT bridge still skips authorizeCommand — the `target:` mesh-routing branch calls three admin-gated handlers directly and never reaches executeCommand — `components/hardwareone/System_MQTT.cpp:486`
- MQTT topic/payload are treated as NUL-terminated C strings — unbounded strlen over the esp-mqtt receive buffer — `components/hardwareone/System_MQTT.cpp:149`
- MQTT command bridge reports ok:true for mesh-routed commands unconditionally, and ok:true for any locally-dispatched failure — `components/hardwareone/System_MQTT.cpp:496`

**OLED UI** (3)

- Location-context bar overflows a 128-byte stack array when map road + area names are long — `components/hardwareone/OLED_Mode_Map.cpp:1230`
- OLED map context bar overflows a 128-byte stack buffer when road + area names are long (snprintf return accumulated with no clamp) — `components/hardwareone/OLED_Mode_Map.cpp:1223`
- Four OLED code paths use the bus-0-hardcoded transaction helper while the OLED lives on bus 1 by default on the primary board — wrong mutex, wrong bus clocked, phantom he — `components/hardwareone/OLED_Utils.cpp:4687`

---

## Suggested order of work

**Cheap and safe (do first).** Ordering fixes and missing guards that touch one function each:
the `clearTrack()` store order, the `decodeBasicAuth` off-by-one, the `writeText()` unconditional
`true`, the `executeCommand()` constant `true`, and stripping session ids out of the log mirror.

**Structural (needs design).** The `remote:`/`@` wrapper bypass, G2 lens authentication, the
ESP-NOW filesystem identity model, and the path-canonicalization mismatch each change a trust
boundary rather than a line of code.

**Invasive.** The G2/BLE teardown-vs-quiesce ordering and the PSRAM task-stack removal in
`g2_img_probe` touch lifecycle across several tasks.

---

## What was not covered

- The three completeness critics never ran, so there is no independent check on coverage gaps.
- Per-finding verdict pairing was lost across the interrupted resumes (see caveat above).
- Subsystems compiled out on FeatherS3 (`ENABLE_ESP_SR`, `ENABLE_EDGE_IMPULSE`,
  `ENABLE_CAMERA_SENSOR`, `ENABLE_ONDEVICE_LLM`) were audited by reading only.
