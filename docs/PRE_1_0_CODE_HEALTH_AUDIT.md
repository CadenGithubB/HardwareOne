# Pre-1.0 Code Health Audit

_Generated 2026-07-14, first-party firmware only (`components/hardwareone/`, `main/`), vendored libraries, the event system (`System_Events`/`System_Notifications`, under active refactor), and the generated game blobs were out of scope._

## What this is

A full-codebase sweep for the three things flagged before the 1.0 release: **(1)** hot-path allocation / string churn / memory fragmentation, **(2)** dead / unused code, and **(3)** logic that should be a shared function but is duplicated or only half-adopted. It was produced by 28 parallel subsystem auditors (12 hot-path, 14 dead-code, 2 cross-cutting) over ~275k lines.

**275 findings** survived de-duplication: **92** hot-path memory, **142** dead-code/unused, **40** duplication/shared-helper.

### How to read confidence + verification

Each finding carries a confidence badge from its auditor: `H` (high) / `M` (medium). Findings I **personally re-read the code for this session** are marked **verified** - treat those as actionable now. Everything else is auditor-reported and should get the standard reachability check before you act, **especially dead-code**: this codebase dispatches CLI commands by string name, token-pastes identifiers via X-macros, references web handlers from HTML blobs, and gates code by board config - all of which make a symbol look dead when it is live. During spot-checks I already found grep counts inflated by log strings, comments, and declarations; confirm each deletion the same way.

---

## The short list - fix these first

These are the highest-leverage items (frequency x impact x confidence), all verified by direct code reading:

1. **Kill the per-command waste in `executeCommand`** - the single hottest path (every command from web/serial/BLE/ESP-NOW/OLED). It builds a **dead `String lc` copy + `toLowerCase()` that is never read** ([System_Utils.cpp:4272](components/hardwareone/System_Utils.cpp#L4272)), **unconditionally copies the full command line** for the rare `remote:`/`@` case ([:4180](components/hardwareone/System_Utils.cpp#L4180)), and **rebuilds a `normalizedCmd` String every call solely to feed two default-off debug lines** ([:4303](components/hardwareone/System_Utils.cpp#L4303)). Three heap allocations per command, all avoidable.
2. **Fix the thermal/ToF task busy-spin** ([i2csensor_mlx90640.cpp:1488](components/hardwareone/i2csensor_mlx90640.cpp#L1488), [i2csensor_vl53l4cx.cpp:732](components/hardwareone/i2csensor_vl53l4cx.cpp#L732)) - the loop's only `vTaskDelay` sits *inside* the `enabled && connected && !pollPaused` gate, so when the sensor is enabled but disconnected or polling is paused (every BLE init pauses polling), the task spins a core at 100%. One-line fix; this is a real bug, not just inefficiency.
3. **Stop the BLE notify path from copying the payload ~4x per fragment** ([Bluetooth.cpp:1191](components/hardwareone/Bluetooth.cpp#L1191)) - `setValue()` + three by-value `getValue()` calls inside the Arduino wrapper churn ~30 KB/s of internal DRAM during a secure-channel file transfer. Inbound writes have the mirror issue ([:436](components/hardwareone/Bluetooth.cpp#L436)). Use the zero-copy `getData()/getLength()` accessors / `esp_ble_gatts_send_indicate()` directly.
4. **Delete `BLE_IDF.cpp`/`.h`** ([BLE_IDF.cpp](components/hardwareone/BLE_IDF.cpp)) - 1,155 lines of a second, half-stubbed BLE stack behind `ENABLE_BLE_IDF_EXPERIMENTAL`, which is `#define`d to 0 and set nowhere. Confirmed zero build configs enable it. Biggest single dead-code win.
5. **Guard the logging fast-path** - `isDebugFlagSet()` ([System_Debug.h:502](components/hardwareone/System_Debug.h#L502)) makes two out-of-line `DEBUG_MANAGER` calls on every `DEBUGF` site regardless of flag state, and the debug layer heap-copies each line into a `String` for the ESP-NOW/broadcast fan-out ([System_Debug.cpp:278/834](components/hardwareone/System_Debug.cpp#L278)). This tax is paid across every subsystem.

---

## 1. Hot-path memory - allocation, string churn, fragmentation

_92 findings. Ordered by how often the code runs. The steady-state tier (per-tick / per-frame / per-packet / per-command / per-event) is where fragmentation actually accrues; init-only allocs are noise and are omitted from the top tier._

### 1a. Steady-state hot paths (highest priority)

- **[i2csensor_mlx90640.cpp:1488](components/hardwareone/i2csensor_mlx90640.cpp#L1488)** `H` verified, _per-tick_ - thermalTask busy-spins at 100% CPU when polling is gated off
  - The only vTaskDelay(10) in thermalTask's while(true) loop is inside the 'enabled && connected && !pollPaused(0)' block with no else-branch delay, so whenever polling is paused (every sensor-init batch, i2c scans, bus recovery) or the sensor is enabled-but-disconnected, the loop spins with zero yield. At priority 1 this starves the idle task on its core for the whole pause window (imuTask/gpsTask have the else vTaskDelay; thermal and tof do not).
  - _Evidence:_ `if (gThermalEnabled && gThermalConnected && gMLX90640 != nullptr && !pollPaused(0)) { ... vTaskDelay(pdMS_TO_TICKS(10)); } // no else delay`
  - _Fix:_ Move vTaskDelay(pdMS_TO_TICKS(10)) after the if-block (or add an else vTaskDelay(50) like imuTask).
- **[i2csensor_vl53l4cx.cpp:732](components/hardwareone/i2csensor_vl53l4cx.cpp#L732)** `H` verified, _per-tick_ - tofTask busy-spins at 100% CPU when polling is gated off
  - Same defect as thermalTask: vTaskDelay(10) at line 764 sits inside the 'gTofEnabled && gTofConnected && !pollPaused(0)' block and there is no else-branch delay, so any pollPause window (sensor init batches run pollPause() for 0.3-1.5s per queued device) makes tof_task spin without yielding, starving the idle task and burning CPU.
  - _Evidence:_ `if (gTofEnabled && gTofConnected && gVL53L4CX != nullptr && !pollPaused(0)) { ... vTaskDelay(pdMS_TO_TICKS(10)); } // closes with no else`
  - _Fix:_ Move the vTaskDelay outside the conditional or add an else vTaskDelay(50) branch.
- **[Bluetooth.cpp:436](components/hardwareone/Bluetooth.cpp#L436)** `H` verified, _per-packet_ - Every inbound GATT write heap-copied into a String on BTC_TASK via getValue()
  - CmdRequestCallbacks::onWrite does 'String value = pCharacteristic->getValue();' - getValue() returns the characteristic's String by value, so every write (every command and every secure-channel frame, up to ~514 B with the 517 MTU) costs an internal-heap alloc+free on BTC_TASK before the data is immediately passed on as c_str()/length(). During phone-to-device uploads this fires per packet.
  - _Evidence:_ `String value = pCharacteristic->getValue(); ... processIncomingBLECommand(param->write.conn_id, value.c_str(), value.length());`
  - _Fix:_ Use pCharacteristic->getData()/getLength() (zero-copy accessors that already exist) instead of the by-value getValue().
- **[Bluetooth.cpp:815](components/hardwareone/Bluetooth.cpp#L815)** `H` verified, _per-packet_ - Two heap allocations (~6.8 KB total) created and freed per inbound secure-channel frame
  - Each SC frame allocates a 522 B BleScDeferred, and submitDeferredToCmdExec (System_Utils.cpp:4578) then allocates a full ~6.3 KB ExecReq whose line[2048]/out[4096] members are ignored in deferred mode; both are freed after the frame is handled. PSRAM-preferred so internal DRAM is mostly safe, but it is a same-size alloc/free pair per received packet during sustained transfers (with fallback to internal heap if PSRAM is exhausted).
  - _Evidence:_ `BleScDeferred* d = (BleScDeferred*)ps_alloc(sizeof(BleScDeferred), AllocPref::PreferPSRAM, "ble.sc.rx"); + ps_alloc(sizeof(ExecReq)) in submitDeferredToCmdExec`
  - _Fix:_ Add a slim deferred-work envelope (fn+arg only) or a small static ring of BleScDeferred slots instead of allocating a full ExecReq per frame.
- **[Bluetooth.cpp:1191](components/hardwareone/Bluetooth.cpp#L1191)** `H` verified, _per-packet_ - bleRawNotify pays ~4 heap String copies of the payload per notification fragment
  - bleRawNotify routes every outbound fragment through the Arduino wrapper: setValue() builds a temporary heap String (BLEValue::setValue does m_value = String(pData,len)) and notify() then calls m_value.getValue() three times, each returning a by-value String copy of the full payload - roughly 4 internal-heap alloc/free cycles of up to 514 B per fragment, and the setValue copy is redone on every congestion retry. A paced secure-channel file read (~33 frames/s at 225 B) churns ~30 KB/s of internal DRAM through this path.
  - _Evidence:_ `pCmdResponseChar->setValue((uint8_t*)data, len); pCmdResponseChar->notify(); // wrapper: m_value=String(...); 3x m_value.getValue() inside notify()`
  - _Fix:_ Call esp_ble_gatts_send_indicate() directly with the caller's buffer (pattern already exists in BLE_IDF.cpp bleIdfServerSendResponse), or at minimum hoist setValue() out of the retry loop.
- **[Bluetooth.cpp:1213](components/hardwareone/Bluetooth.cpp#L1213)** `H` verified, _per-packet_ - OLED message history stamped per raw notify fragment, including binary ciphertext frames
  - bleRawNotify snprintf's a 'TX:...' entry into the 4-slot OLED history on every successful or failed notify - for secure-channel traffic that is every ~225 B encrypted fragment, so a multi-fragment message overwrites the whole 4-entry history with unreadable ciphertext prefixes (the %.*s stops at the first NUL byte of frame data) many times per second, whether or not the OLED Bluetooth screen is showing.
  - _Evidence:_ `snprintf(tagged, sizeof(tagged), "TX:%.*s", (int)(BLE_MSG_MAX_LEN - 4), data ? data : ""); bleAddMessageToHistory(tagged);`
  - _Fix:_ Move the TX-history tagging up to sendBLEResponse/sendBLEResponseToConn (once per plaintext message) instead of per wire fragment in bleRawNotify.
- **[HardwareOne.cpp:753](components/hardwareone/HardwareOne.cpp#L753)** `H` verified, _per-command_ - 4KB capture buffer malloc/freed every captured command
  - commandExecTask mallocs a 4096-byte capture buffer from internal DRAM and frees it for every command with ctx.captureOutput (all web CLI capture requests). Repeated same-size alloc/free on the tight internal heap is a fragmentation driver, and cmd_exec is a single task so one persistent buffer suffices.
  - _Evidence:_ `captureBuf = (char*)malloc(CAPTURE_BUF_SIZE); ... free(captureBuf);`
  - _Fix:_ Allocate one static/lazy-init 4KB buffer owned by cmd_exec_task (or ps_alloc once) and reuse it across commands.
- **[System_Utils.cpp:4158](components/hardwareone/System_Utils.cpp#L4158)** `H` verified, _per-command_ - Full registry scan runs twice per command (authorize + dispatch)
  - executeCommand calls authorizeCommand -> commandRequiresAdmin (System_Utils.cpp:3109) which does a complete findCommand scan, then line 4295 calls findCommand again on the same line to dispatch. Each scan carries the per-entry String churn above, so the whole cost is doubled on every command.
  - _Evidence:_ `4158: authorizeCommand(ctx, command, ...) -> commandRequiresAdmin -> findCommand; 4295: found = findCommand(command);`
  - _Fix:_ Resolve the CommandEntry once at the top of executeCommand and pass it to the admin check (entry->requiresAdmin) instead of re-scanning.
- **[System_Utils.cpp:4180](components/hardwareone/System_Utils.cpp#L4180)** `H` verified, _per-command_ - executeCommand makes two full String copies of every command line
  - `String actualCommand = command;` runs unconditionally (a full heap copy, up to 2KB) and `command = actualCommand;` at line 4269 copies it back, even though actualCommand only differs on the rare remote:/@ path. Two extra heap allocations per command on the hottest command path, despite the comment about avoiding String temporaries.
  - _Evidence:_ `String actualCommand = command;  ...  command = actualCommand;  // both run for every non-remote command`
  - _Fix:_ Only build actualCommand inside the isRemoteCommand branch (or strip the prefix in place) so the common path makes zero extra copies.
- **[System_Utils.cpp:4303](components/hardwareone/System_Utils.cpp#L4303)** `H` verified, _per-command_ - normalizedCmd String rebuilt per command solely for runtime-gated debug logs
  - executeCommand unconditionally builds normalizedCmd (String ctor + substring + trim + two concats) but its only consumers are DEBUG_CMD_FLOWF/DEBUGF at 4358-4359, which are runtime-gated (getLogLevel() >= DEBUG) - at production log levels the allocation and copies are pure waste on every command.
  - _Evidence:_ `String normalizedCmd = String(found->name); ... used only in DEBUG_CMD_FLOWF/DEBUGF at 4358-4359`
  - _Fix:_ Drop normalizedCmd and log found->name plus args directly inside the debug macro, or guard the build behind the same log-level check.
- **[System_Utils.cpp:4441](components/hardwareone/System_Utils.cpp#L4441)** `H` verified, _per-command_ - ExecReq (~6.3KB) + binary semaphore created/destroyed per command
  - submitAndExecuteSync ps_allocs a ~6.3KB ExecReq (line[2048]+out[4096]+ctx) and creates/deletes a binary semaphore (internal-DRAM FreeRTOS alloc) for every synchronous command from serial/OLED/web. The queue is only 6 deep, so a small fixed pool of ExecReq slots with persistent semaphores would eliminate all per-command create/destroy churn.
  - _Evidence:_ `ExecReq* r = (ExecReq*)ps_alloc(sizeof(ExecReq), ..."cmd.exec.req"); ... r->done = xSemaphoreCreateBinary();`
  - _Fix:_ Use a static pool of 6 ExecReq slots with pre-created semaphores instead of per-command ps_alloc + xSemaphoreCreateBinary/vSemaphoreDelete.
- **[System_Debug.h:502](components/hardwareone/System_Debug.h#L502)** `H` verified, _per-event_ - Flag-off macro guard pays 2-4 out-of-line singleton calls plus a 32-byte struct copy
  - The macros correctly guard before evaluating arguments (no String temporaries when off), but the guard itself calls getLogLevel() and isDebugFlagSet(), which route through DebugManager::getInstance()/getLogLevel()/getDebugFlags() - all defined out-of-line in System_Debug.cpp, with getDebugFlags() returning the 32-byte DebugFlagMask by value. Across ~3,770 call sites (including per-poll sensor loops, per-packet ESP-NOW, per-step LLM forward), every disabled log line pays cross-TU call overhead for what could be two inline global loads (gLogLevel and gDebugFlags are already extern in this same header).
  - _Evidence:_ `inline bool isDebugFlagSet(DebugFlagMask flag) { return gDebugVerbose || ((getDebugFlags() & flag) != (DebugFlagMask)0); } // getDebugFlags -> DEBUG_MANAGER call`
  - _Fix:_ Make getDebugFlags()/getLogLevel() read the extern globals directly inline instead of going through the DebugManager singleton.
- **[System_ESPNow.cpp:7771](components/hardwareone/System_ESPNow.cpp#L7771)** `H`, _per-tick_ - Seven timeout sweeps plus a rekey table walk run every 10 ms for multi-second deadlines
  - processMeshHeartbeats runs pendingFrameTimeoutSweep, sessionEstablishingTimeoutSweep, sendStatusSweep, fileSlotsTimeoutSweep, fsListTick, sessionRekeyPrevKeysSweep, keyExRetrySweep and the rekey-threshold session walk (7812-7843) on every 100 Hz tick, though the shortest deadline any of them enforces is ~3-5 s. Each is a full static-table walk, so the pack burns CPU on core 0 every tick for work that would be identical at 4 Hz.
  - _Evidence:_ `pendingFrameTimeoutSweep((uint32_t)millis()); sessionEstablishingTimeoutSweep(...); sendStatusSweep(...); ... keyExRetrySweep(...); (all unconditional per tick)`
  - _Fix:_ Wrap the sweep block in the shared everyMs() helper (System_Utils.h:184) at ~250 ms, keeping only the RX-ring drain and deferred-flag handling per-tick.
- **[System_ESPNow.cpp:8163](components/hardwareone/System_ESPNow.cpp#L8163)** `H`, _per-tick_ - bondPeerMac String re-parsed to bytes at 100 Hz and per sensor frame
  - The master bond-sync tick parses gSettings.bondPeerMac into a 6-byte MAC on every 10 ms super-loop tick while the peer is online (plus again at 8293 until the post-sync kick fires), and sendBondedSensorData (12396) re-parses it for every worker sensor frame at stream rate. The MAC only changes when the setting changes, so this is constant re-derivation on two hot paths.
  - _Evidence:_ `bool macOk = (gSettings.bondPeerMac.length() > 0 && parseMacAddress(gSettings.bondPeerMac, peerMac)); (per tick; again per frame at 12396)`
  - _Fix:_ Cache the parsed bond peer MAC (bytes + valid flag) beside the setting and invalidate on settings change.
- **[i2csensor_pa1010d.cpp:383](components/hardwareone/i2csensor_pa1010d.cpp#L383)** `H`, _per-tick_ - GPS reads one NMEA byte per full I2C transaction, 100x/sec
  - gpsReadChar() wraps a single Adafruit_GPS::read() (which pops one char from the library's 32-byte buffer) in a complete i2cTransactionNACKTolerant - bus mutex take/give, clock-stack push/pop, setBusClock, metrics update - once per 10ms tick. That is ~100 full transactions/sec of pure overhead per byte, and 100 B/s consumption is below the ~130-160 B/s a 1Hz RMC+GGA stream produces, so the parser also falls behind the device.
  - _Evidence:_ `i2cTransactionNACKTolerant((uint8_t)gSettings.gpsBus, I2C_ADDR_GPS, 100000, 100, [&]() { c = gPA1010D->read(); });`
  - _Fix:_ Read a burst (e.g. up to 32 read()/newNMEAreceived() iterations) inside ONE transaction per tick instead of one transaction per byte.
- **[G2_Glasses.cpp:1846](components/hardwareone/G2_Glasses.cpp#L1846)** `H`, _per-packet_ - Per-notify hex dump built even when G2 debug is off
  - The BLE notify handler formats up to 32 bytes into hex[128] via a snprintf loop on every incoming notification before the gated DEBUG_G2F call; DEBUGF_QUEUE_DEBUG's lazy-arg design is defeated because the string is prebuilt unconditionally. This runs on the Bluedroid notify task for every RX packet, including every per-fragment image ack during streaming.
  - _Evidence:_ `for (size_t i = 0; i < shown && hp + 3 < sizeof(hex); i++) { hp += snprintf(hex + hp, ..., "%02X ", data[i]); }`
  - _Fix:_ Wrap the hex-formatting loop in the same gate DEBUG_G2F uses (getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_G2)) so it costs nothing in normal operation.
- **[G2_Glasses.cpp:15042](components/hardwareone/G2_Glasses.cpp#L15042)** `H`, _per-frame_ - Camera stream re-allocates rgb + bmp buffers every frame
  - g2CameraStreamWorker's frame loop does ps_alloc/free of rgbBuf (srcW*srcH*3, ~230 KB at QVGA) and bmpBuf (kBmpCap ~21 KB) on every frame, even though both sizes are constant for the whole session (kBmpCap is a compile-time-ish constant and cameraWidth/Height persist across captures). That is a ~250 KB alloc/free cycle per frame at ~0.4-1.4 fps for the life of the stream.
  - _Evidence:_ `uint8_t* rgbBuf = (uint8_t*)ps_alloc(rgbLen, ..."g2.camstr.rgb"); ... uint8_t* bmpBuf = (uint8_t*)ps_alloc(kBmpCap, ..."g2.camstr.bmp");`
  - _Fix:_ Allocate rgbBuf and bmpBuf once before the while(true) loop (sizes are session-constant) and free them after the loop exits.
- **[G2_Ring.cpp:422](components/hardwareone/G2_Ring.cpp#L422)** `H`, _per-packet_ - Ring notify decode builds hex dump + annotation with debug off
  - ringDumpFrame formats up to 64 payload bytes into pbuf via snprintf and calls r1AnnotatePayload into abuf on every ring notification, before the gated DEBUG_G2F consumes them - all of it wasted work when the G2 debug flag or debug log level is off. The pattern repeats for the gRingDumpVerbose-independent portion; only the second dump at line 449 is properly gated.
  - _Evidence:_ `off += snprintf(pbuf + off, ..., "%02X ", d.payload[i]); ... size_t alen = r1AnnotatePayload(d, abuf, sizeof(abuf));`
  - _Fix:_ Gate the pbuf formatting and r1AnnotatePayload call behind the same debug-level/flag check DEBUG_G2F performs.
- **[OLED_Mode_Network.cpp:89](components/hardwareone/OLED_Mode_Network.cpp#L89)** `H`, _per-frame_ - prepareNetworkData allocates 3 Strings per frame on Network pages
  - Runs for both OLED_NETWORK_INFO and OLED_NETWORK_STATUS on every render: WiFi.SSID() String by value, a substring(0,15) temp, and WiFi.localIP().toString(), each freed within the same call after strncpy into char[16] fields. Identical pattern to prepareSystemStatusData - same fix applies to both.
  - _Evidence:_ `String ssid = WiFi.SSID(); ... ssid = ssid.substring(0, 15); ... String ip = WiFi.localIP().toString();`
  - _Fix:_ Copy straight into the fixed buffers with strncpy/snprintf (no substring needed) or cache and refresh only when WiFi state changes.
- **[OLED_Mode_Network.cpp:821](components/hardwareone/OLED_Mode_Network.cpp#L821)** `H`, _per-frame_ - prepareMeshStatusData builds up to ~8 String temporaries per frame
  - Every render of OLED_MESH_STATUS calls getEspNowDeviceName() (String by value) for self and master, with fallback chains macToHexString(myMac).substring(8) and repeated substring(0,11) reassignments - each a heap alloc - before the results land in fixed char[12] fields that rarely change.
  - _Evidence:_ `String myName = getEspNowDeviceName(myMac); ... myName = macToHexString(myMac).substring(8); ... myName = myName.substring(0, 11);`
  - _Fix:_ Resolve the names once on mode entry (or when peer table generation changes) into the char[12] fields instead of re-deriving them every frame.
- **[OLED_Mode_System.cpp:223](components/hardwareone/OLED_Mode_System.cpp#L223)** `H`, _per-frame_ - displayUnavailable allocates a substring per displayed line per frame
  - The unavailable-page renderer splits unavailableOLEDReason on newlines with unavailableOLEDReason.substring(start[, nl]) - up to 3 heap String temporaries per frame for the ~5 s (or indefinite, for 'Press X' pages) the page is shown, re-splitting text that never changes while displayed.
  - _Evidence:_ `oledDisplay->println(unavailableOLEDReason.substring(start, nl));`
  - _Fix:_ Print line ranges directly from the underlying buffer (display->write(c_str()+start, nl-start)) or pre-split into static char lines in enterUnavailablePage().
- **[OLED_Mode_System.cpp:400](components/hardwareone/OLED_Mode_System.cpp#L400)** `H`, _per-frame_ - Per-frame WiFi String temporaries on the default System Status page
  - prepareSystemStatusData() runs on every render of OLED_SYSTEM_STATUS (the default screen) and allocates up to 3 heap Strings per frame: WiFi.SSID() by value, ssid.substring(0,15), and WiFi.localIP().toString() - all immediately copied into fixed char[16] buffers and discarded.
  - _Evidence:_ `String ssid = WiFi.SSID(); if (ssid.length() > 15) ssid = ssid.substring(0, 15); ... String ip = WiFi.localIP().toString();`
  - _Fix:_ Read SSID via esp_wifi_sta_get_ap_info into a stack buffer (or truncate during strncpy - substring is redundant) and format the IP with snprintf; optionally gate with everyMs() since SSID/IP change rarely.
- **[OLED_Mode_UnifiedMenu.cpp:473](components/hardwareone/OLED_Mode_UnifiedMenu.cpp#L473)** `H`, _per-frame_ - peerName String + substring allocated every unified-menu frame
  - displayUnifiedMenu() calls BondedPeer::peerName() (returns String by value) and then peerName.substring(0, 10) on every render frame - two heap allocations per frame for a value that only changes on re-bond.
  - _Evidence:_ `String peerName = BondedPeer::peerName(); oledDisplay->println(peerName.substring(0, 10));`
  - _Fix:_ Cache the truncated peer name in a static char[11] refreshed on mode entry or bond change, and print that.
- **[System_Camera_DVP.cpp:754](components/hardwareone/System_Camera_DVP.cpp#L754)** `H`, _per-frame_ - captureFrame allocs+copies whole JPEG every frame
  - Every capture does ps_alloc(fb->len) + memcpy of the full JPEG (~30-100KB at VGA), and every caller frees it: the video recorder loop (System_Camera_Video.cpp:310/330) at up to 20 fps, EdgeImpulse continuous inference (System_EdgeImpulse.cpp:1054/1072) each interval, and web/G2 snapshot paths. Variable-size alloc/free at 10-20 Hz is a PSRAM fragmentation driver plus a redundant full-frame copy.
  - _Evidence:_ `uint8_t* buf = (uint8_t*)ps_alloc(fb->len, AllocPref::PreferPSRAM, "camera.frame"); memcpy(buf, fb->buf, fb->len); ... free(jpeg) per frame`
  - _Fix:_ Keep one persistent max-frame-size buffer (or add a borrow/return API that hands out the camera fb directly) instead of alloc+memcpy+free per frame.
- **[System_ESPNow.cpp:2138](components/hardwareone/System_ESPNow.cpp#L2138)** `H`, _per-packet_ - MAC formatted into stack buffer per packet even when debug logging is disabled
  - v4_send_ack pre-formats the destination MAC with formatMacAddressBuf (snprintf, six %02X) before a DEBUGF whose macro gates argument evaluation - so the snprintf runs on every ACK TX with debug off. The same unconditional pre-format sits on other per-packet paths: v4_send_frag_ack (2160), ACK RX (4818), fragment RX (4695), v4_send_text (2486), v4_send_user_sync (2470), v4_send_command_response (2503).
  - _Evidence:_ `char dstMac[18]; formatMacAddressBuf(dst, dstMac, sizeof(dstMac)); DEBUGF(DEBUG_ESPNOW_CORE, "[V4_ACK_TX] Sending ACK to %s ...", dstMac, ...);`
  - _Fix:_ Move the formatMacAddressBuf call inside the debug gate (or use the lazy MAC_STR macro as a DEBUGF argument) at all seven sites.
- **[System_ESPNow.cpp:8110](components/hardwareone/System_ESPNow.cpp#L8110)** `H`, _per-packet_ - Stream-queue drain builds heap Strings per drained chunk on espnow_task
  - Each drained STREAM entry constructs String(entry.deviceName) (heap alloc for names >~10 chars) and falls back to formatMacAddress() which returns a 17-char heap String - this runs per remote-command output chunk, up to streamDrainMax entries per 10 ms tick during streaming. The same String-or-formatMacAddress pattern repeats in the CMD_RESP drain (line 8133), metadata drain (8547), and text drain (8559).
  - _Evidence:_ `String devName = String(entry.deviceName); if (devName.length() == 0) devName = formatMacAddress(entry.srcMac);`
  - _Fix:_ Use a stack char[32] with strlcpy and the existing formatMacAddressBuf() instead of String in all four drain sites.
- **[System_ESPNow_Crypto.cpp:119](components/hardwareone/System_ESPNow_Crypto.cpp#L119)** `H`, _per-packet_ - mbedtls context alloc/free on every BROADCAST_AUTH packet
  - espnowCryptoHmacSha256 runs mbedtls_md_init/setup/free per call; setup with is_hmac=1 does two internal-DRAM callocs (SHA-256 ctx + 128 B ipad/opad pair) that are freed at the end of the same call. v4_broadcast_category ORs BROADCAST_AUTH into every sensor/status/heartbeat broadcast (System_ESPNow.cpp:1855), so TX at up to 10 Hz per peer during input streaming plus RX verification of every received authenticated broadcast each churn small same-size internal-heap blocks - a classic fragmentation driver on the tight internal heap.
  - _Evidence:_ `mbedtls_md_init(&ctx); if (mbedtls_md_setup(&ctx, mdInfo, 1) != 0) ... mbedtls_md_free(&ctx);  (called per BROADCAST_AUTH frame, TX and RX)`
  - _Fix:_ Hand-roll HMAC-SHA256 over stack mbedtls_sha256_context objects (no heap) or cache a lazily-initialized keyed md context per task instead of setup/free per call.
- **[System_ESPNow_Tx.cpp:250](components/hardwareone/System_ESPNow_Tx.cpp#L250)** `H`, _per-packet_ - TX clerk allocates and frees a payload copy for every outbound frame
  - sendAead/sendAeadSync ps_alloc a fresh payload buffer per job and the dispatcher frees it after send, so every clerk-routed frame (sensor streams at ~10 Hz/sensor, command-output STREAM chunks, bond traffic via the identical copy in bondSendEncryptedAsync at System_ESPNow.cpp:2285) does a same-size alloc/free round trip. PreferPSRAM softens it, but it falls back to internal DRAM under PSRAM pressure and is exactly the per-message-alloc pattern the 15800-byte static buffer fix removed elsewhere.
  - _Evidence:_ `psBuf = (uint8_t*)ps_alloc(payloadLen, AllocPref::PreferPSRAM, "espnow.tx.aead"); ... memcpy(psBuf, payload, payloadLen); (freed in runJob)`
  - _Fix:_ Replace per-job ps_alloc with a fixed slab pool sized to the 32-slot queue (e.g. 32 x 256 B inline in Job plus a couple of large slots for >250 B payloads).
- **[System_MQTT.cpp:106](components/hardwareone/System_MQTT.cpp#L106)** `H`, _per-packet_ - updateExternalSensor double-allocates and overreads topic/data
  - String(topic) runs strlen on esp-mqtt's non-NUL-terminated event buffer (reads past topic_len, potentially past the rx buffer), heap-copies everything found, then .substring() allocates a second copy - twice, for topic and data, on every subscribed message. Two allocations per field per packet in internal DRAM.
  - _Evidence:_ `String topicStr = String(topic).substring(0, topicLen); String dataStr = String(data).substring(0, dataLen);`
  - _Fix:_ Build each String with a length-bounded copy (String s; s.concat(topic, topicLen);) to remove the overread and halve the allocations.
- **[System_MQTT.cpp:335](components/hardwareone/System_MQTT.cpp#L335)** `H`, _per-packet_ - Command/response topics rebuilt on every MQTT message
  - handleMQTTCommand is called unconditionally from MQTT_EVENT_DATA (line 848), so every received message on any subscribed topic - including all external sensor traffic - heap-builds two String concats (baseTopic+"/command" and +"/response") just to reject non-command topics. The topics are constant while connected.
  - _Evidence:_ `String commandTopic = gSettings.mqttBaseTopic + "/command"; String responseTopic = gSettings.mqttBaseTopic + "/response"; (before the topic-match early-return)`
  - _Fix:_ Cache both topic strings once at connect/subscribe time and compare with strncmp against the cached c_str().
- **[HardwareOne.cpp:506](components/hardwareone/HardwareOne.cpp#L506)** `H`, _per-command_ - appendCommandToFeed concatenates temporary Strings per command
  - After formatting the prefix into a stack buffer, the line is assembled as `String(prefix) + redactCmdForAudit(cmd)` - a String temporary, a by-value String return, and a concat alloc per command - only to immediately pass .c_str() to gWebMirror.appendDirect. Runs for every serial (and feed-audited) command.
  - _Evidence:_ `String line = String(prefix) + redactCmdForAudit(cmd); gWebMirror.appendDirect(line.c_str(), line.length(), true);`
  - _Fix:_ appendDirect the prefix and the redacted command as two consecutive writes (or snprintf both into one stack buffer) to avoid the String concat chain.
- **[HardwareOne.cpp:851](components/hardwareone/HardwareOne.cpp#L851)** `H`, _per-command_ - broadcastOutput(ctx) builds prefix String by value then grows without reserve
  - The context-aware broadcastOutput runs after every command: originPrefix() snprintfs into a stack buf but returns a heap String by value, then `prefixed += s` reallocs to append the (possibly multi-KB) result. Two to three heap allocs plus a full copy per command result, when the whole prefix+message could be assembled in one stack/preallocated buffer before broadcastOutputCore_Routed.
  - _Evidence:_ `String prefixed = originPrefix(source, ctx.auth.user, ctx.auth.ip); prefixed += s;`
  - _Fix:_ reserve(prefixLen + s.length()) before appending, or snprintf the prefix into a stack buffer and pass two-part output to a core that accepts (prefix, body).
- **[System_Automation.cpp:1235](components/hardwareone/System_Automation.cpp#L1235)** `H`, _per-command_ - Automation JSON built via dozens of += concats without reserve()
  - cmd_automation_add assembles the new automation object through ~30 `+=` concat chains with String(number) temporaries and jsonEscape returns (lines 1212-1360), plus temporary wrapper objects for computeInitialNextAt (1267) - an unreserved growth pattern causing repeated reallocs of a KB-scale String.
  - _Evidence:_ `b += ",\n      \"weekInterval\": " + String(weekInterval.toInt()); ... String tmp = "{\"triggers\":[{\n" + body + "\n    }]}";`
  - _Fix:_ reserve() the builder Strings up front or assemble with snprintf into a single buffer; per-command so low urgency, but it is the largest single concat cluster in the file.
- **[System_Command.cpp:103](components/hardwareone/System_Command.cpp#L103)** `H`, _per-command_ - findCommand allocates a lowercased String per registry entry per lookup
  - The lookup loop constructs String(entryName) + toLowerCase() for every one of the ~500+ registered commands (MAX_COMMANDS=1024) on each call; names over ~10 chars heap-allocate in internal DRAM. Since findCommand runs at least twice per command (auth check + dispatch), this is on the order of 1000 String constructions and hundreds of malloc/free cycles per command executed.
  - _Evidence:_ `String lcEntry = String(entryName); lcEntry.toLowerCase(); if (lc.startsWith(lcEntry)) ...`
  - _Fix:_ Compare with strncasecmp(lc.c_str(), entryName, entryLen) (plus the word-boundary check) so the loop does zero allocations, or precompute lowercase names once at registration.
- **[System_ESPNow.cpp:5047](components/hardwareone/System_ESPNow.cpp#L5047)** `H`, _per-command_ - Per-command malloc/free of up to 6 KB internal DRAM for CMD_RESP payload
  - v4CmdResultCallback mallocs 1+textLen+1 (capped 6145 B) on every remote-command completion and frees it right after the synchronous send; plain malloc of this size lands in internal DRAM, making repeated remote commands a fragmentation driver. v4_send_command_response (line 2513) has the same malloc-per-response pattern, while the RX side already uses a one-time ps_alloc'd 6144 B buffer (gEspNow->deferredCmdRespResult) as precedent.
  - _Evidence:_ `uint8_t* respPayload = (uint8_t*)malloc(payloadLen); ... espnowtx::sendAeadSync(...); free(respPayload);`
  - _Fix:_ Use a one-time ps_alloc'd (or static PSRAM) 6.2 KB response buffer; cmd_exec serializes these callbacks so a single shared buffer is race-free.
- **[System_ESPNow_Sensors.cpp:950](components/hardwareone/System_ESPNow_Sensors.cpp#L950)** `H`, _per-command_ - JSON serialize-parse-serialize round trip in espnow sensorstatus
  - The master JSON path of cmd_espnow_sensorstatus calls getRemoteDevicesListJSON() (builds a JsonDocument, serializes it into a heap String), then deserializes that String into a second document just to graft it under doc["devices"], then serializes everything again into jbuf - three JSON documents plus an intermediate String per command.
  - _Evidence:_ `if (deserializeJson(tmp, getRemoteDevicesListJSON()) == DeserializationError::Ok && tmp["devices"].is<JsonArray>())`
  - _Fix:_ Refactor the devices-list builder to populate a caller-supplied JsonArray/JsonDocument so the status command fills doc["devices"] directly with no serialize/parse round trip.
- **[System_I2C.cpp:1801](components/hardwareone/System_I2C.cpp#L1801)** `H`, _per-command_ - cmd_sensors filter allocates 3 heap Strings per database row
  - When a filter is given, the loop constructs String(sensor.name/description/manufacturer) and lowercases them for every entry in the sensor database (~3 allocs + copies per row, dozens of rows) on each 'sensors <filter>' command. A case-insensitive substring check on the const char* fields (strcasestr-style) would do zero allocations.
  - _Evidence:_ `String sensorName = String(sensor.name); String sensorDesc = String(sensor.description); String sensorMfg = String(sensor.manufacturer); ...toLowerCase();`
  - _Fix:_ Replace the per-row String temporaries with an alloc-free case-insensitive substring helper on the raw C strings.
- **[System_Utils.cpp:4408](components/hardwareone/System_Utils.cpp#L4408)** `H`, _per-command_ - redactCmdForAudit computed twice per command, each with multiple full-line String copies
  - Every command redacts the line once inside logCommandExecution (System_Utils.cpp:921) and again for the logAuthAttempt audit line (4408, and 4344 on the help-exit path); each call copies the whole line twice (String c, String cl lowercased) and may build substring pieces. The char auditBuf is then converted to yet another String temporary because logAuthAttempt takes const String& extra.
  - _Evidence:_ `4408: redactCmdForAudit(command).c_str() ... 921: String redactedCmd = redactCmdForAudit(cmd); (both run per command)`
  - _Fix:_ Redact once per executeCommand invocation and pass the result to both audit sinks.
- **[OLED_Mode_Menu.cpp:309](components/hardwareone/OLED_Mode_Menu.cpp#L309)** `M`, _per-frame_ - Sensor menu rebuilt and bubble-sorted with O(n^2) availability probes every frame
  - sensorMenuPopulate() runs from both displaySensorMenu() (per frame) and the input handler (per event) with sortByAvailability=true; the bubble sort in populateMenuScroll calls getMenuAvailability() twice per comparison (~100+ calls for a dozen items), each of which can loop the connectedDevices[] registry, even though ordering only changes when sensor status (gSensorStatusSeq) changes.
  - _Evidence:_ `populateMenuScroll(&sSensorScroll, oledSensorMenuItems, oledSensorMenuItemCount, /*sort=*/true, /*dropNotBuilt=*/true);  // bubble sort re-calls getMenuAvailability per compare`
  - _Fix:_ Compute each item's availability rank once into a local array before sorting, and skip the rebuild entirely unless gSensorStatusSeq changed since the last populate.
- **[System_ESPNow.cpp:1501](components/hardwareone/System_ESPNow.cpp#L1501)** `M`, _per-packet_ - Own STA MAC re-fetched from the WiFi driver on every TX frame
  - v4_send_frame calls esp_wifi_get_mac(WIFI_IF_STA,...) per frame to stamp the origin field, and the same call repeats in v4_send_session_wrapped (1603), v4_send_frag_ack (2168), v4_send_encrypted_chunked (1956) and inside inline isSelfMac (System_ESPNow.h:1088) which runs per-peer in every broadcast fan-out and heartbeat sweep. The STA MAC is constant after WiFi init, so this is a driver API round trip per packet for an invariant value.
  - _Evidence:_ `uint8_t myMac[6]; esp_wifi_get_mac(WIFI_IF_STA, myMac); memcpy(h.origin, myMac, 6);`
  - _Fix:_ Cache the self MAC once at ESP-NOW init (refresh on WiFi restart) and use it in v4_send_* and isSelfMac.
- **[System_ESPNow.cpp:3246](components/hardwareone/System_ESPNow.cpp#L3246)** `M`, _per-packet_ - Topology stream grows a String per TOPO_PEER frame and rescans it for dedup
  - v4h_topo_peer appends a ~60-100 B formatted line to stream->accumulatedData with += (repeated realloc, no reserve) and dedups by accumulatedData.indexOf(peerMacStr), an O(total) scan per received frame - O(n^2) over the collection window, all on espnow_task. Impact is bounded by peer count and the operation is user-triggered, but the reallocs churn heap during an RX burst.
  - _Evidence:_ `if (stream->accumulatedData.indexOf(peerMacStr) != -1) ... stream->accumulatedData += peerInfoBuf;`
  - _Fix:_ reserve() the buffer at TOPO_START (totalPeers x ~100 B) and dedup via a small seen-MAC array instead of indexOf on the text.
- **[System_ESPNow.cpp:5417](components/hardwareone/System_ESPNow.cpp#L5417)** `M`, _per-packet_ - SD capture opens and closes the log file for every captured frame
  - When espnowCaptureToSd is enabled, captureEspNowFrame does a guarded VFS open + write + close per RX and per TX frame, plus a heap String peerName from getEspNowDeviceName (5393) per frame. At normal mesh traffic rates that is hundreds of open/close cycles and String allocs per minute on the RX-drain task; off by default, but the feature exists precisely for high-traffic debugging.
  - _Evidence:_ `String peerName = peerMac ? getEspNowDeviceName(peerMac) : String(); ... File f = VFS::openGuarded(gEspNowCapturePath, "a", ...); f.write(...); f.close();`
  - _Fix:_ Keep the capture File handle open with a periodic flush/rotate, and resolve the peer name into a stack buffer.
- **[System_ESPNow_Sensors.cpp:706](components/hardwareone/System_ESPNow_Sensors.cpp#L706)** `M`, _per-frame_ - formatRemoteSensorReadable re-parses unchanged JSON on every redraw
  - Each call allocates a fresh PSRAM JsonDocument and fully deserializes entry->jsonData; callers (OLED remote-sensors page at OLED_Mode_Network.cpp:1240, G2 detail at G2_Page_ESPNow.cpp:629) invoke it on every screen redraw even when lastUpdate hasn't changed, and the OLED caller has already parsed the exact same JSON into its own doc at line 1060 - a double parse per frame.
  - _Evidence:_ `PSRAM_JSON_DOC(doc); if (deserializeJson(doc, json) != DeserializationError::Ok)  - per redraw, data usually unchanged`
  - _Fix:_ Add an overload taking an already-parsed JsonObjectConst (reusing the caller's doc), or cache the formatted lines keyed on entry->lastUpdate so unchanged data skips the parse.
- **[G2_Glasses.cpp:14287](components/hardwareone/G2_Glasses.cpp#L14287)** `H`, _per-event_ - Map page re-allocates 21 KB bmp + 41 KB shade buffer per render
  - Each dirty tick of g2MapPageWorker ps_allocs a ~21 KB bmp buffer, and g2RenderCurrentMapBmp (line 14151) allocates a 288*144 = 41 KB shade buffer inside it; both are freed immediately and both sizes are constants, so every zoom/pan tap costs a ~62 KB alloc/free cycle.
  - _Evidence:_ `uint8_t* bmp = (uint8_t*)ps_alloc(bmpCap, ..."g2.map.bmp"); / uint8_t* shades = (uint8_t*)ps_alloc((size_t)kLensW * kLensH, ..."g2.map.shade");`
  - _Fix:_ Allocate both buffers once at map-page-worker entry (before the poll loop) and free them at exit, since dimensions never change within a session.
- **[System_Automation.cpp:2384](components/hardwareone/System_Automation.cpp#L2384)** `H`, _per-event_ - Full file read/parse/pretty-serialize/rewrite per fired automation
  - rescheduleAfterFire does readText + PSRAM deserialize + serializeJsonPretty into a new String + atomic file rewrite for each automation that fires; several fires in one tick repeat the entire cycle per automation.
  - _Evidence:_ `if (!readText(AUTOMATIONS_JSON_FILE, json)) return; ... serializeJsonPretty(doc, json); writeAutomationsJsonAtomic(json);`
  - _Fix:_ Accumulate nextAt updates during the tick and do one read-modify-write after the scan loop.
- **[System_Automation.cpp:2621](components/hardwareone/System_Automation.cpp#L2621)** `H`, _per-event_ - Dead unguarded copy of TOF objects before the mutex-guarded copy
  - The DISTANCE branch copies all four gTofCache.tofObjects without holding the mutex, then immediately re-copies them under SensorCacheGuard; if the guard fails, tofTotal stays 0 so the unguarded values are never used - the first copy is dead work and an unsynchronized read.
  - _Evidence:_ `for (int i = 0; i < 4; i++) objs[i] = gTofCache.tofObjects[i]; // then repeated inside SensorCacheGuard at 2626`
  - _Fix:_ Delete the unguarded pre-copy at line 2621.
- **[System_Automation.cpp:3465](components/hardwareone/System_Automation.cpp#L3465)** `H`, _per-event_ - appendAutoLogEntry(const String&) forces a heap temp per log line
  - All ~8 hot call sites (3723, 3856, 3875, 3900, 828, 1636...) snprintf into a stack char buffer and then pass it to a const String& parameter, constructing a 128-256 byte temporary heap String per autolog line whenever logging is active.
  - _Evidence:_ `extern bool appendAutoLogEntry(const char* type, const String& message); // called with char startBuf[256]`
  - _Fix:_ Change the message parameter to const char* (definition at System_Utils.cpp:970); no call site needs String semantics.
- **[System_Automation.cpp:3699](components/hardwareone/System_Automation.cpp#L3699)** `H`, _per-event_ - Second JSON parse of the same object on event ticks
  - When drained events exist, automationEventTriggersMatch calls triggersFromJson (line 3563) on the identical object that computeNextRunTime just parsed one line above, doubling the JsonDocument alloc + deserialize per automation on event ticks (rate-limited to 4 Hz).
  - _Evidence:_ `bool eventDue = automationEventTriggersMatch(obj.c_str(), sEventBuf, evCount, &evKindName);`
  - _Fix:_ Parse the triggers array once per automation and feed both the clock-due computation and the event matcher from the same Trigger array.
- **[System_Automation.cpp:3848](components/hardwareone/System_Automation.cpp#L3848)** `H`, _per-event_ - IF/THEN wrapper built via String concat chain per condition-gated fire
  - The global condition gate builds the wrapped expression with a two-realloc String concat chain, while the same file already uses the snprintf-into-stack-buffer pattern for the identical job (fullCondBuf at line 3354). Same pattern repeats per-command at line 995.
  - _Evidence:_ `String wrapped = "IF " + condition + " THEN _";`
  - _Fix:_ snprintf into a stack buffer ("IF %s THEN _") as executeConditionalCommand already does.
- **[System_Debug.cpp:278](components/hardwareone/System_Debug.cpp#L278)** `H`, _per-event_ - debugOutputTask builds a heap String per [ERROR]/[EVENT]/[EVLOG] line
  - The single sink-fanout task does `String line = buildTimestampPrefix(); line += msg->text;` at three sites (278, 293, 303) - buildTimestampPrefix formats into a stack char[48] then returns a heap String by value, and the += reallocs to append up to 256 bytes. Two heap allocs per logged line on the task every broadcast funnels through, when a ~304-byte stack snprintf would be zero-alloc.
  - _Evidence:_ `String line = buildTimestampPrefix(); line += msg->text; appendLineWithCap(LOG_ERROR_FILE, line, LOG_ERROR_CAP);`
  - _Fix:_ Give appendLineWithCap a (const char*) overload and snprintf "%s%s" of prefix+text into a stack buffer at the three call sites.
- **[System_Debug.cpp:662](components/hardwareone/System_Debug.cpp#L662)** `H`, _per-event_ - debugQueuePrintf formats into a 256 B stack buffer then memcpys into the pooled slot
  - Every ON-path debug line is vsnprintf'd into `char line[DEBUG_MSG_SIZE]` and then enqueueChunk memcpys the same bytes into the pooled DebugMessage. Grabbing the free-list slot first and formatting directly into msg->text would drop 256 B of stack use and a 256 B copy per message.
  - _Evidence:_ `char line[DEBUG_MSG_SIZE]; ... vsnprintf(line, sizeof(line), fmt, args); ... enqueueChunk(line, llen, MSG_ROUTE_ALL, flag, ...);`
  - _Fix:_ Add an enqueue variant that formats va_list directly into the pooled slot (returning it on failure).
- **[System_Debug.cpp:834](components/hardwareone/System_Debug.cpp#L834)** `H`, _per-event_ - broadcastOutputCore heap-copies every line into a String for ESP-NOW streaming
  - When gCurrentStreamCmdId is active, every single broadcast line is wrapped in `String(text)` (heap alloc + copy) just to call sendEspNowStreamMessage, and the send runs synchronously on whatever task broadcast the line. A chatty streamed command allocates once per output line on top of the queue copy already made in enqueueChunk.
  - _Evidence:_ `if (gCurrentStreamCmdId != 0) { sendEspNowStreamMessage(String(text)); }`
  - _Fix:_ Add a (const char*, size_t) overload of sendEspNowStreamMessage so the stream path reuses the existing buffer with zero allocation.
- **[System_Debug.cpp:838](components/hardwareone/System_Debug.cpp#L838)** `H`, _per-event_ - broadcastOutputCore allocates String(text) per line whenever an ESP-NOW stream is active
  - While gCurrentStreamCmdId != 0, EVERY broadcastOutput() from ANY task constructs a heap String copy (up to 256 B) of the line to call sendEspNowStreamMessage(String(text)) - yet the callee immediately discards calls not on cmd_exec_task, so most of these temporaries are pure waste. This runs per output line of every streamed remote command plus all concurrent tasks' broadcasts during that window.
  - _Evidence:_ `if (gCurrentStreamCmdId != 0) { sendEspNowStreamMessage(String(text)); }`
  - _Fix:_ Change sendEspNowStreamMessage to take (const char*, size_t) and hoist the cmd_exec-task gate before any copy.
- **[System_Debug.cpp:2157](components/hardwareone/System_Debug.cpp#L2157)** `H`, _per-event_ - getDebugCategoryName does a ~115-branch linear scan of 256-bit mask tests per file-logged line
  - When file logging is active with category tags (the default), every queued message walks up to ~115 sequential `if (flag & DEBUG_X)` tests, each a 4x64-bit AND plus OR-reduce, to find one name. A bit-index lookup (count-trailing-zeros into a name table) would be O(1).
  - _Evidence:_ `if (flag & DEBUG_AUTH) return "AUTH"; if (flag & DEBUG_HTTP) return "HTTP"; ... (~115 sequential DebugFlagMask tests)`
  - _Fix:_ Index a static name table by lowest-set-bit position instead of the if-chain.
- **[System_Debug.cpp:3332](components/hardwareone/System_Debug.cpp#L3332)** `H`, _per-event_ - logI2CError/logI2CRecovery build lines via String concat chains with String(number) temporaries
  - Each call does ~8 `+=` appends including String(address, HEX), String(consecutiveErrors), String(totalErrors) temporaries onto a heap String started by buildTimestampPrefix() - several allocs/reallocs per call. logI2CError fires on every BUS_ERROR occurrence and on degradation events (System_I2C_Manager.cpp recordError), so a flapping bus churns internal heap at poll cadence.
  - _Evidence:_ `String line = buildTimestampPrefix(); line += "I2C ERROR | addr=0x"; ... line += String(consecutiveErrors); line += String(totalErrors);`
  - _Fix:_ snprintf the whole bounded line into a stack char[160] and append that.
- **[System_LLM.cpp:2536](components/hardwareone/System_LLM.cpp#L2536)** `H`, _per-event_ - llm generate CLI output String under-reserved for its own cap
  - cmd_llm_generate reserves 1024 bytes but the callback appends tokens until output.length() reaches 2000, so once past 1KB every generated token's `output += token` triggers a WString realloc+copy (arduino WString grows to exact need, 16B-rounded). This is the only per-token heap churn in the generation path; the engine itself streams into a preallocated PSRAM buffer.
  - _Evidence:_ `output.reserve(1024); ... [&output](const char* token) { output += token; return (output.length() < 2000); }`
  - _Fix:_ reserve(2048) to cover the 2000-byte cap so no per-token reallocation occurs.
- **[WebServer_Events.cpp:119](components/hardwareone/WebServer_Events.cpp#L119)** `H`, _per-event_ - SSE event dequeue round-trips fixed char queues through heap Strings
  - sseDequeueEvent copies the already-NUL-terminated queue slots (16B name / 128B data) into two fresh Strings per event, the hold loop constructs them anew each iteration, and oversize data takes an extra .substring(0,160) alloc before being snprintf'd back into a stack buffer. sseSendNotice similarly does `String safe = note` + replace per notice.
  - _Evidence:_ `outEventName = String(s.eventNameQ[s.eqHead]); outData = String(s.eventDataQ[s.eqHead]); ... evData = evData.substring(0, 160);`
  - _Fix:_ Add a dequeue variant that copies straight into caller char buffers (or formats the SSE frame directly from the queue slot) so the loop is allocation-free.
- **[Bluetooth.cpp:761](components/hardwareone/Bluetooth.cpp#L761)** `M`, _per-command_ - Command line round-trips stack buffer -> heap String -> ExecReq char array per BLE command
  - processBleCommandLine builds the command in a stack cmdBuf[512], then 'ucmd.line = cmdStart' heap-allocates a String copy (Command.line is String) which submitCommandAsync immediately strncpy's into ExecReq.line[2048] and discards. That is one avoidable heap alloc/free of up to ~512 B per BLE command, on BTC_TASK when the plaintext path is used; the login branch does the same via loginCmd[256].
  - _Evidence:_ `Command ucmd; ucmd.line = cmdStart;  // String alloc, then strncpy(r->line, cmd.line.c_str(), ...) in submitCommandAsync`
  - _Fix:_ Add a const char* overload of submitCommandAsync (or make Command.line a pointer+len for this path) so the stack buffer is copied straight into ExecReq.line.
- **[System_AuthIdentity.cpp:105](components/hardwareone/System_AuthIdentity.cpp#L105)** `M`, _per-command_ - ExecIdentityGuard deep-copies AuthContext three times per command
  - Every executeCommand constructs a CommandIdentityScope whose ExecIdentityGuard copy-assigns the slot's AuthContext into savedCtx_ (4 String members: user/path/ip/sid), copy-assigns the installed ctx in, and copies the saved one back in the destructor - roughly 12 String assignments per command, several heap-allocating for web paths/IPs longer than SSO.
  - _Evidence:_ `savedCtx_ = slot->ctx; savedUser_ = slot->user; ... slot->ctx = install; (dtor: slot->ctx = savedCtx_;)`
  - _Fix:_ Swap/move the saved AuthContext (std::swap or move-assign) instead of copy-assigning both directions.
- **[System_Command.cpp:452](components/hardwareone/System_Command.cpp#L452)** `M`, _per-command_ - CommandArgs::value()/hasKey() build a needle String per call and grow output char-by-char
  - Each of the ~38 value()/hasKey() call sites allocates String needle = key + "=" per invocation, and the quoted-value path grows out += c one character at a time with no reserve(), causing repeated reallocation for long values (e.g. secondarytriggers JSON); handlers that call hasKey then value for the same key pay it all twice.
  - _Evidence:_ `String needle = key + "="; ... String out; while (...) { out += c; }`
  - _Fix:_ Take const char* keys and search with strstr on raw_.c_str(), and reserve() the remaining-length before the unescape loop.
- **[G2_Glasses.cpp:14039](components/hardwareone/G2_Glasses.cpp#L14039)** `M`, _per-event_ - Viewer entry points spawn a transient worker task per UI action
  - g2ShowBmpFile (14039), g2ShowCameraViewer (14720), g2ShowCameraStream (15162), g2ShowMapPage (14334), g2ShowJpgFile (15760), and g2ShowJpgFileFullScreen (15883) each xTaskCreate a 6-8 KB(+) internal-DRAM stack per user action and vTaskDelete on completion - the exact per-action task-churn pattern the file's own page-swap-worker comment (line 8367) identifies as the root cause of a past contiguous-DRAM exhaustion. Repeated open/close of images from the Files page cycles these stacks through the internal heap.
  - _Evidence:_ `if (xTaskCreate(g2BmpViewerWorker, "g2_bmp_view", 6144, a, tskIDLE_PRIORITY + 2, nullptr) != pdPASS)`
  - _Fix:_ Route these one-shot viewer bodies through the existing persistent lens-applier queue (LensJobKind::Custom) or a single shared viewer worker instead of a fresh task per action.
- **[System_Automation.cpp:3750](components/hardwareone/System_Automation.cpp#L3750)** `M`, _per-event_ - 16-String array plus heap substring per command on every fire
  - Each firing automation constructs a String cmdsList[16], a heap `body` substring (3779), and one substring alloc per command before queueing; the Strings live only for the duration of the fire, so it is repeated same-size alloc/free churn.
  - _Evidence:_ `String cmdsList[MAX_AUTO_CMDS]; ... String body = obj.substring(arrStart + 1, arrEnd); ... cmdsList[cmdsCount++] = one;`
  - _Fix:_ Evaluate the condition gate first, then extract and queue each command in a single pass using a stack char buffer instead of retaining a String array.

### 1b. Periodic & per-request paths

- **[System_Command.cpp:209](components/hardwareone/System_Command.cpp#L209)** `H`, _per-request_ - executeCommandThroughRegistry rescans the registry with String temporaries and double-copies the result
  - After resolveRegistryCommandKey already ran a full findCommand scan, the function loops the whole registry again building String(commandRegistry[i]->name) per entry just to re-find the same entry, and on success constructs String(result) at 244 for the error check then again at 259 for the return - two full copies of a result that can be several KB.
  - _Evidence:_ `if (resolvedKey == String(commandRegistry[i]->name)) ... String resultStr = String(result); ... return String(result);`
  - _Fix:_ Have findCommand return the entry directly (skip the re-scan), compare with ==(const char*), and reuse one result String for both the check and the return.
- **[System_Debug.cpp:996](components/hardwareone/System_Debug.cpp#L996)** `H`, _per-request_ - gStreamTag global String reassigned per streamed HTTP response
  - streamDebugReset() is called once per chunked page serve (WebServer_Server.cpp:1167) and does `gStreamTag = tag ? String(tag) : String("")` - a temporary plus copy-assign that heap-allocates for page names over ~10 chars. A fixed char[32] would make it allocation-free.
  - _Evidence:_ `gStreamTag = tag ? String(tag) : String("");`
  - _Fix:_ Replace the String global with a static char[32] filled by strlcpy.
- **[System_ESPNow_FsList.cpp:703](components/hardwareone/System_ESPNow_FsList.cpp#L703)** `H`, _per-request_ - Loop-invariant String prefix rebuilt per directory entry in LIST reply
  - Inside the while(file) scan of processListDeferred, expectedPrefix is constructed from fsDirPath and grown with += "/" on every entry even though it never changes, alongside a fresh String(file.name()), a .substring() copy, and a formatPath() String per entry. A directory listing walks every entry (even past the 32-entry emit cap, to count total), so one LIST request can do 4+ heap String allocs per file.
  - _Evidence:_ `while (file) { String fileName = String(file.name()); ... String expectedPrefix = fsDirPath; if (!expectedPrefix.endsWith("/")) expectedPrefix += "/";`
  - _Fix:_ Hoist the prefix build (and its length) out of the loop and do the prefix-strip/hidden checks with strncmp on file.name() directly, avoiding per-entry String construction.
- **[System_ESPNow_Sensors.cpp:773](components/hardwareone/System_ESPNow_Sensors.cpp#L773)** `H`, _per-request_ - Heap String MAC per cache entry in getRemoteDevicesListJSON
  - The 64-slot cache loop creates a by-value String from macToHexString() for every connected entry (17 chars, exceeds SSO so always heap), copies it into the JsonDocument, and does an O(n^2) strcmp scan of the devices array per entry. The web sensors page polls this endpoint periodically while open, so the churn repeats on every poll.
  - _Evidence:_ `String macStr = macToHexString(e.deviceMac); ... for (JsonObject dev : devices) { if (strcmp(dev["mac"], macStr.c_str()) == 0)`
  - _Fix:_ Format the MAC into a stack char[18] with the existing macToDisplayStr-style buffer helper and remember the last-matched device object (cache entries for one device are typically adjacent).
- **[WebServer_Server.cpp:1459](components/hardwareone/WebServer_Server.cpp#L1459)** `H`, _per-request_ - logAuthAttempt heap-allocates and double-scans the URI on every authenticated request
  - tgRequireAuth calls logAuthAttempt on every successful web request (including 56ms encoder polls and 500ms log polls); it always builds `String cleanPath` plus two .replace() passes before the isSecurityAuditEvent check rejects ~all of them. That's a per-request heap alloc + three string scans purely to decide 'not a login event'.
  - _Evidence:_ `String cleanPath = String(path ? path : ""); cleanPath.replace("%2F", "/"); cleanPath.replace("%20", " ");`
  - _Fix:_ Pre-filter with cheap C checks on the raw path/reason (strstr for "login", empty reason) and only allocate/normalize when the event actually qualifies.
- **[WebServer_Server.cpp:4800](components/hardwareone/WebServer_Server.cpp#L4800)** `H`, _per-request_ - handleCliBatch copies every command output 4-5 times
  - Each command's output is copied via redactOutputForLog (String by value), copied again into `results.push_back(out)`, copied a third time into respDoc via arr.add(r), then serialized into yet another String - peak memory is ~4-5x total output size per batch. The CLI page routes passive background polls through this endpoint, so it runs steadily, not just on user actions.
  - _Evidence:_ `String redacted = redactOutputForLog(out); broadcastOutput(redacted, uc.ctx); results.push_back(out); ... arr.add(r); ... serializeJson(respDoc, respStr);`
  - _Fix:_ std::move(out) into the vector, redact in place (or only when a log route is enabled), and stream the JSON response with httpd_resp_send_chunk instead of building respStr.
- **[WebServer_Utils.cpp:356](components/hardwareone/WebServer_Utils.cpp#L356)** `H`, _per-request_ - generateNavigation grows a ~2KB String through ~80 += with no reserve()
  - The nav bar is rebuilt on every authenticated page load via dozens of small += appends (plus a `"<div...>" + username + "</div>"` temporary chain), causing repeated internal-heap reallocs for output that is immediately streamed and discarded. generatePublicNavigation has the same pattern in miniature.
  - _Evidence:_ `nav += "<a href=\""; nav += href; nav += "\" class=\"menu-item"; ... nav += "<div class=\"username\">" + username + "</div>";`
  - _Fix:_ Stream the links directly with streamChunkC (the caller already streams), or at minimum nav.reserve(2048) up front.
- **[WebServer_Utils.h:173](components/hardwareone/WebServer_Utils.h#L173)** `H`, _per-request_ - getFileBrowserScript() returns ~48KB flash literal as String by value
  - The file-browser JS is a ~48KB raw string literal in rodata, but the helper wraps it in a String, forcing a ~48KB heap allocation plus full copy on every Files/CLI/Logging/Maps/ESPNow page load. The callers immediately stream it and discard the String.
  - _Evidence:_ `inline String getFileBrowserScript() { return R"FBSCRIPT( ... )FBSCRIPT"; }  // literal spans lines 174-1198, ~48KB`
  - _Fix:_ Return `const char*` to the literal (or a streamFileBrowserScript(req) that chunks it) so no heap copy is ever made.
- **[G2_Glasses.cpp:3301](components/hardwareone/G2_Glasses.cpp#L3301)** `H`, _periodic_ - Status body builder uses a heap String per tick
  - buildG2StatusSnapshot constructs a temporary String (reserve(256) - internal-heap alloc/free per call, since WString uses plain malloc) plus a WiFi.localIP().toString() temporary, then strncpy's into the caller's already-provided char buffer. It runs on every live-status tick from livePageWorker/renderStatusCompound.
  - _Evidence:_ `String s; s.reserve(256); ... String ip = WiFi.localIP().toString(); ... strncpy(out, s.c_str(), cap - 1);`
  - _Fix:_ Build directly into the caller's out buffer with a snprintf cursor (the function already receives out/cap), eliminating both String temporaries.
- **[G2_Glasses.cpp:7266](components/hardwareone/G2_Glasses.cpp#L7266)** `H`, _periodic_ - sendRebuildListAndWait allocates 8 KB for an envelope capped at 240 B
  - The helper ps_allocs kPbCap=8192 but then rejects any built envelope larger than kSingleFragmentCap=240 bytes, so 97% of the buffer can never be used on a successful send. It runs on every live-list refresh tick and on every list-rebuild fast-path attempt.
  - _Evidence:_ `constexpr size_t kPbCap = 8192; ... constexpr size_t kSingleFragmentCap = 240; if (envLen > kSingleFragmentCap) { free(pb); return false; }`
  - _Fix:_ Right-size the buffer to the worst-case build (~2-3 KB) or reuse a persistent scratch buffer instead of an 8 KB heap round-trip per tick.
- **[G2_Glasses.cpp:7448](components/hardwareone/G2_Glasses.cpp#L7448)** `H`, _periodic_ - Every REBUILD/CREATE send allocates and frees a fresh 8 KB pb buffer
  - sendRebuildTextAndWait (the per-tick live-text path) and its ~8 siblings (create-list 7220, rebuild-list 7266, rebuild-multitext 7585, rebuild-list+multitext 7683, create-list+multitext 7742, etc.) each ps_alloc an 8 KB buffer and free it at the end of every call. On a live page this is a same-size alloc/free cycle every refresh tick; ps_alloc falls back to internal heap when PSRAM is tight, making it a fragmentation driver exactly when memory is scarce.
  - _Evidence:_ `constexpr size_t kPbCap = 8192; uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, ..."g2.pb.rebuild-text"); ... free(pb);`
  - _Fix:_ Hoist one persistent 8 KB pb scratch buffer (allocated at G2 init, serialized by the existing armCreateSlot/armRebuildSlot single-flight discipline) and reuse it across all these send helpers.
- **[G2_Glasses.cpp:9508](components/hardwareone/G2_Glasses.cpp#L9508)** `H`, _periodic_ - Live-page/live-text ticks re-send REBUILD when content is unchanged
  - livePageWorker calls buildFn and ships a full REBUILD-list every tick with no diff against the previous textBuf; liveTextWorker's buildFn path (lines 10164-10165, g2ShowText) does the same. renderStatusCompound (line 9934) already proves the fix - its strcmp cache skips quiet ticks because each redundant REBUILD costs an 8 KB pb alloc, a multi-fragment BLE burst, and a visible firmware repaint flicker.
  - _Evidence:_ `if (sendRebuildListAndWait(*arm, ptrs, n, G2_GEOM_LARGE)) { ... }  // no change-check; runs every tick`
  - _Fix:_ Keep a copy of the previous buildFn output in the worker and skip the REBUILD (continue) when strcmp says nothing changed, mirroring the gStatusLast* cache pattern.
- **[G2_Glasses.cpp:9836](components/hardwareone/G2_Glasses.cpp#L9836)** `H`, _periodic_ - Status compound render heap-allocates 2 KB bodyBuf every tick
  - renderStatusCompound does ps_alloc(2048)/free of bodyBuf on every live-status tick (including the fully-skipped no-change ticks), with free() duplicated across six exit paths. An adjacent static already exists for the same content size (EXT_RAM_BSS gStatusLastBodyStr[2048]), showing the cheaper pattern.
  - _Evidence:_ `char* bodyBuf = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "g2.statusCompound.body"); ... free(bodyBuf); (6 sites)`
  - _Fix:_ Make bodyBuf a static EXT_RAM_BSS buffer like gStatusLastBodyStr, removing the per-tick alloc and all six free() paths.
- **[HardwareOne.cpp:2135](components/hardwareone/HardwareOne.cpp#L2135)** `H`, _periodic_ - Automation 60s safety tick re-reads and re-parses automations.json when idle
  - The `(nowAuto - lastAutoCheck >= 60000)` safety term forces schedulerTickMinute at least once a minute, which does readText(AUTOMATIONS_JSON_FILE) into a transient heap String plus a full PSRAM JSON parse (System_Automation.cpp:3621) even when nothing is due, no events are pending, and the file hasn't changed. That's a flash read + file-sized String alloc + parse every minute for the lifetime of the device.
  - _Evidence:_ `needFullTick = gAutosDirty || automationsAnyDue(nowT) || ... || (nowAuto - lastAutoCheck >= 60000);`
  - _Fix:_ Drive the safety tick off gAutosDirty/cache-invalid instead of a blind 60s reparse, or have schedulerTickMinute skip the file load when the in-RAM cache is valid and nothing is due.
- **[OLED_Mode_UnifiedMenu.cpp:464](components/hardwareone/OLED_Mode_UnifiedMenu.cpp#L464)** `H`, _periodic_ - Menu rebuild with file I/O + JSON parse inside render path
  - displayUnifiedMenu() (called inside the frame render) invokes buildUnifiedMenu() every 30s while the Actions mode is shown: free() + ps_calloc of a ~3KB item array, plus VFS::openGuarded of the cached manifest and deserializeJson into a PSRAM_JSON_DOC. This blocks the 10ms main-loop tick with flash I/O and repeats a same-size alloc/free cycle, violating the file's own prepare-outside-render pattern.
  - _Evidence:_ `if (!gUnifiedMenuItems || (millis() - gUnifiedMenuLastBuild > 30000)) { buildUnifiedMenu(); }  // -> free() + ps_calloc + deserializeJson(doc, f)`
  - _Fix:_ Build once on mode entry (onEnterFunc) into a static array and rebuild only on manifest/bond change events, never from the display function.
- **[System_Automation.cpp:3621](components/hardwareone/System_Automation.cpp#L3621)** `H`, _periodic_ - Whole automations.json heap-read every scheduler tick
  - schedulerTickMinute runs at least every 60s, up to 4 Hz on subscribed bus events, and at every interval-trigger cadence; each run heap-allocates the entire file into a String even when nothing fires. Counting rebuildAutoCache's re-read (finding 2), that is two file-sized internal-DRAM allocations per tick.
  - _Evidence:_ `String json; if (!readText(AUTOMATIONS_JSON_FILE, json)) return;`
  - _Fix:_ Keep a parsed in-RAM copy invalidated by writeAutomationsJsonAtomic (the gAutoCacheValid seam already exists) so idle ticks do zero file I/O.
- **[System_Automation.cpp:3652](components/hardwareone/System_Automation.cpp#L3652)** `H`, _periodic_ - Per-automation heap substring copy of each object every tick
  - The tick scan materializes every automation object as a fresh heap String each pass; with N automations that is N alloc/free cycles per tick (objects with 16 commands can be 1-2 KB each), a classic internal-DRAM fragmentation driver.
  - _Evidence:_ `String obj = json.substring(objStart, objEnd + 1);`
  - _Fix:_ Operate on offsets/lengths into the json buffer (deserializeJson and the char* helpers accept ptr+len) instead of copying each object.
- **[System_Automation.cpp:3691](components/hardwareone/System_Automation.cpp#L3691)** `H`, _periodic_ - Triggers JSON re-deserialized per automation per tick
  - computeNextRunTime -> triggersFromJson allocates a fresh PSRAM JsonDocument and deserializes the object for every enabled automation on every tick (lines 2226-2227), even though trigger schedules only change on edits and fires. Repeated same-size doc create/destroy is pure churn.
  - _Evidence:_ `time_t nextAt = timeValid ? computeNextRunTime(obj.c_str(), now) : 0; // PSRAM_JSON_DOC + deserializeJson per automation`
  - _Fix:_ Cache parsed Trigger structs alongside gAutoCache (invalidated via the existing gAutoCacheValid) instead of re-parsing JSON each tick.
- **[System_Automation.cpp:3941](components/hardwareone/System_Automation.cpp#L3941)** `H`, _periodic_ - rebuildAutoCache re-reads and re-parses the file every tick, even unchanged
  - rebuildAutoCache runs unconditionally at the end of every full tick, doing a second readText plus a full ArduinoJson deserialize (lines 509-515). On the common no-fire tick the file is byte-identical to the String already read at line 3621, so both the I/O and the parse are redundant.
  - _Evidence:_ `rebuildAutoCache(); // -> readText(AUTOMATIONS_JSON_FILE, json) + deserializeJson(doc, json) at 509-515`
  - _Fix:_ Skip the rebuild when executed==0, no sanitize ran, and the cache is valid - or at minimum pass in the String already read at line 3621.
- **[System_MQTT.cpp:1008](components/hardwareone/System_MQTT.cpp#L1008)** `H`, _periodic_ - MQTT publish allocates and frees a 16KB buffer plus topic String every interval
  - publishMQTTSensorData ps_allocs 16KB, builds JSON, publishes, and frees it on every publish interval; it also rebuilds `String stateTopic = gSettings.mqttBaseTopic + "/state"` (line 1124, same pattern at 264) and creates WiFi.SSID()/localIP().toString() String temporaries each time. PSRAM-preferred so less critical than DRAM, but it's the same-size alloc/free repeated forever and falls back to internal heap under PSRAM pressure.
  - _Evidence:_ `char* jsonBuf = (char*)ps_alloc(16384, ..., "mqtt.json"); ... String stateTopic = gSettings.mqttBaseTopic + "/state"; ... free(jsonBuf);`
  - _Fix:_ Allocate the 16KB JSON buffer once at MQTT start (freed in stopMQTT) and cache the state topic string, refreshing it only when mqttBaseTopic changes.
- **[WebServer_Server.cpp:2951](components/hardwareone/WebServer_Server.cpp#L2951)** `H`, _periodic_ - handleLogs allocates/frees an 8KB buffer on every 500ms CLI-page poll
  - The CLI page polls /api/cli/logs every 500ms (WebPage_CLI.h:182) and each hit does ps_alloc(gWebMirrorCap)+free plus an unconditional 8KB copy/send, even when the mirror hasn't changed. gWebMirrorSeq is fetched for the debug log but never used to skip unchanged content.
  - _Evidence:_ `char* responseBuf = (char*)ps_alloc(gWebMirrorCap, AllocPref::PreferPSRAM, "handleLogs.resp"); ... free(responseBuf);`
  - _Fix:_ Use a one-time static buffer (httpd handlers are single-task) and gate the copy/send on gWebMirrorSeq (e.g. ?seq= param returning 304/empty when unchanged).
- **[System_Camera_Video.cpp:514](components/hardwareone/System_Camera_Video.cpp#L514)** `M`, _per-request_ - Video list endpoint builds a String list then re-parses it, walking the dir twice
  - handleVideoRecordingsList calls getVideoRecordingsList() (heap String grown with += per file, no reserve) and getVideoRecordingCount() - two full SD directory walks per request - then re-splits the "name:size\n" String with substring() (3 more String allocs per file) to build the JSON String, also grown without reserve.
  - _Evidence:_ `String raw = getVideoRecordingsList(); int count = getVideoRecordingCount(); ... String item = raw.substring(start, nl); json += item.substring(0, colon);`
  - _Fix:_ Walk the directory once, emitting JSON entries directly into a reserved String (or snprintf into a reused buffer), deriving the count from the same pass.
- **[System_Camera_Video.cpp:598](components/hardwareone/System_Camera_Video.cpp#L598)** `M`, _per-request_ - 8KB stream buffer malloc'd per video download
  - handleVideoRecordingFile mallocs the same fixed 8192-byte chunk buffer on every download request and frees it at the end; plain malloc lands in internal DRAM first, so long downloads hold (and repeated downloads cycle) an 8KB internal-heap block on the tight internal heap.
  - _Evidence:_ `const size_t CHUNK = 8192; uint8_t* buf = (uint8_t*)malloc(CHUNK); ... free(buf);`
  - _Fix:_ Use a one-time static buffer or ps_alloc(PreferPSRAM) so the repeated fixed-size alloc stays off the internal heap.
- **[System_ESPNow_FsList.cpp:609](components/hardwareone/System_ESPNow_FsList.cpp#L609)** `M`, _per-request_ - Fixed-size 2.5 KB reply buffer allocated and freed per LIST request
  - processListDeferred ps_alloc's the same kReplyBufSize (~2572 B) buffer and frees it at the end of every LIST request. The single sDeferred slot already serializes all deferred FS ops on cmd_exec, so a one-time persistent buffer is race-free; the current pattern churns PSRAM per request and hits internal DRAM whenever the PSRAM-preferred alloc falls back.
  - _Evidence:_ `uint8_t* replyBuf = (uint8_t*)ps_alloc(kReplyBufSize, AllocPref::PreferPSRAM, "fslist.reply"); ... free(replyBuf);`
  - _Fix:_ Allocate the reply buffer once (lazy static) and reuse it - the single-slot sDeferred serialization already guarantees exclusive access.
- **[WebPage_Sensors.cpp:633](components/hardwareone/WebPage_Sensors.cpp#L633)** `M`, _per-request_ - handleRemoteSensors makes 3 full copies of the devices-list JSON
  - The device-list branch copies getRemoteDevicesListJSON() into `resp`, then rebuilds it via a String temporary chain (`String("{\"enabled\":") + ... + resp.substring(1)`), so the whole list JSON exists in ~3 heap copies simultaneously on each sensors-page remote poll.
  - _Evidence:_ `String resp = devicesList; ... resp = String("{\"enabled\":") + (espnowActive ? "true" : "false") + "," + resp.substring(1);`
  - _Fix:_ Send the prefix and devicesList.c_str()+1 as two chunks (httpd_resp_send_chunk) instead of splicing Strings.
- **[WebPage_Sensors.cpp:971](components/hardwareone/WebPage_Sensors.cpp#L971)** `M`, _per-request_ - Mic recording served via whole-file heap buffer with internal-DRAM fallback
  - handleMicRecordingFile mallocs the entire WAV (up to ~1.9MB) and, if the PSRAM alloc fails, falls back to plain malloc which can carve a large block out of the tight internal DRAM at the worst possible moment. WebServer_Server.cpp:4154 (audio file view, up to 5MB) has the identical fallback pattern.
  - _Evidence:_ `char* buf = (char*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); if (!buf) { buf = (char*)malloc(fileSize); }`
  - _Fix:_ Drop the internal-malloc fallback and stream the file in fixed-size chunks (Content-Length is already known, so chunked reads into a small buffer keep seek support).
- **[WebServer_Server.cpp:2970](components/hardwareone/WebServer_Server.cpp#L2970)** `M`, _per-request_ - Session cookie parsed twice per request in polled handlers
  - isAuthed (inside WEB_AUTH_OR_RETURN) already runs getCookieSID + findSessionIndexBySID, then handleSensorsStatusWithUpdates, handleNotice (2909), handleCLICommand (3080), handleCliBatch (4767) and handleCameraStream re-parse the Cookie header and re-scan the session table for the same request - an extra header copy, String alloc, and linear search on every poll.
  - _Evidence:_ `int sessIdx = findSessionIndexBySID(getCookieSID(req));  // isAuthed already resolved this SID for the same req`
  - _Fix:_ Have isAuthed/tgRequireAuth stash the resolved session index (e.g. in AuthContext) so handlers reuse it instead of re-parsing the cookie.
- **[WebServer_Utils.cpp:465](components/hardwareone/WebServer_Utils.cpp#L465)** `M`, _per-request_ - streamBeginHtml re-reads user settings file on every page load just for theme
  - Every authenticated page render does getUserIdByUsername + loadUserSettings (filesystem read + PSRAM JSON parse) solely to pick 'light'/'dark', and the injected hw.initTheme JS then fetches /api/user/settings again, loading the same file a second time per page view.
  - _Evidence:_ `PSRAM_JSON_DOC(settings); if (loadUserSettings(uid, settings)) { const char* t = settings["theme"] | "light"; ...`
  - _Fix:_ Cache the theme (e.g. a char in SessionEntry, refreshed when /api/user/settings POST changes it) instead of hitting the filesystem per page render.
- **[System_MQTT.cpp:718](components/hardwareone/System_MQTT.cpp#L718)** `M`, _periodic_ - Mesh peer publish re-parses unchanged cached JSON every tick
  - publishMeshPeerSensorData runs every publish interval and, per active peer, heap-builds two topic Strings by 4-part concat (lines 718, 774), deserializes every matching gRemoteSensorCache JSON blob into a fresh PSRAM JsonDocument, and re-serializes to a String - even when no cache entry's lastUpdate changed since the previous publish.
  - _Evidence:_ `String peerStateTopic = gSettings.mqttBaseTopic + "/devices/hardwareone_" + macCompact + "/state"; deserializeJson(sensorDoc, gRemoteSensorCache[s].jsonData, ...)`
  - _Fix:_ Skip peers whose cache entries are unchanged since the last publish and build topics with snprintf into a stack buffer.
- **[System_SensorLogging.cpp:504](components/hardwareone/System_SensorLogging.cpp#L504)** `M`, _periodic_ - Sensor-log flush opens/closes the log file on every write
  - Each log interval does VFS::openGuarded(String(activePath), "a") ... f.close(): the String temporary plus the per-open LittleFS file object/cache buffers are allocated and freed on internal heap every cycle. At gpslog rates (default 1s, min 100ms) this is a steady same-size alloc/free churn - exactly the fragmentation pattern the project is trying to avoid - plus full open/append/close filesystem overhead per line.
  - _Evidence:_ `File f = VFS::openGuarded(String(activePath), "a", VFS::systemAuth("senlog.append"), true); ... f.close();`
  - _Fix:_ Keep the file handle open across ticks (reopen only on rotation/path change) or batch several lines per open, and pass activePath without the String temporary.

## 2. Dead & unused code

_142 findings across 84 files. Grouped by file, files ordered by cleanup leverage. **Reachability-check each before deleting** (string dispatch / X-macros / board gates / HTML blobs). The whole-file and whole-subsystem candidates below are the safe, high-value bulk removals._

### `System_Utils.cpp` (6) - _includes verified_

- **[System_Utils.cpp:4272](components/hardwareone/System_Utils.cpp#L4272)** `H` verified - Dead lowercased copy of the command line in executeCommand
  - String lc = command; lc.toLowerCase(); is computed on every command but never read anywhere in the rest of the function (findCommand does its own lowercasing) - a wasted full-line heap copy plus lowercase pass per command.
  - _Evidence:_ `4272-4273: String lc = command; lc.toLowerCase(); - zero later uses of lc through line 4413`
  - _Fix:_ Delete the two lines.
- **[System_Utils.cpp:97](components/hardwareone/System_Utils.cpp#L97)** `H` - Task Execution Metrics subsystem is entirely dead
  - gTaskMetrics plus taskOperationStart()/taskOperationComplete()/resetTaskMetrics() (declared in System_Utils.h:18-31) have zero callers anywhere; the global is only ever written by these three uncalled functions and never read. The whole TaskExecutionMetrics struct + EWMA/timeout logic is unreachable.
  - _Evidence:_ `gTaskMetrics/taskOperationStart/Complete/resetTaskMetrics: only def+decl, 0 call sites repo-wide`
  - _Fix:_ Delete the TaskExecutionMetrics struct, gTaskMetrics, and the three task-metric functions from System_Utils.h/.cpp.
- **[System_Utils.cpp:386](components/hardwareone/System_Utils.cpp#L386)** `H` - Unused JSON scalar-parse helpers
  - parseJsonBool (386), parseJsonFloat (435), parseJsonU16 (468), and extractObjectByKey (487) have no callers; only parseJsonInt/parseJsonString/extractArrayByKey/extractArrayItem from the same family are actually used.
  - _Evidence:_ `parseJsonBool/parseJsonFloat/parseJsonU16/extractObjectByKey: only def+decl repo-wide`
  - _Fix:_ Remove the four unused JSON parse helpers and their declarations.
- **[System_Utils.cpp:563](components/hardwareone/System_Utils.cpp#L563)** `H` - Several public String/serial/time utils have no callers
  - urlEncode (563), serializeJsonArrayWithRepair (651), formatDateTime (670), waitForSerialInput non-blocking variant (681), and nowEpoch (2379) are all declared in System_Utils.h and defined but never called (urlDecode and waitForSerialInputBlocking are the live counterparts).
  - _Evidence:_ `urlEncode/serializeJsonArrayWithRepair/formatDateTime/waitForSerialInput/nowEpoch: only def+decl, 0 callers`
  - _Fix:_ Delete these five unused helpers and their System_Utils.h declarations.
- **[System_Utils.cpp:4652](components/hardwareone/System_Utils.cpp#L4652)** `H` - Unused icon-system entry points
  - initIconSystem (4652), getIconPath (4657), and iconExists (4663) have no callers; only drawIcon/drawIconScaled/getIconNameForExtension (and internal loadIconData) from the icon API are actually used.
  - _Evidence:_ `initIconSystem/getIconPath/iconExists: only def+decl, 0 callers`
  - _Fix:_ Delete the three unused icon helpers and their declarations.
- **[System_Utils.cpp:2218](components/hardwareone/System_Utils.cpp#L2218)** `M` - #if 0 DNS pre-check block in syncNTPAndResolve
  - A ~26-line disabled DNS-resolution pre-flight block (2218-2244) is retained under #if 0 with a 'to re-enable change #if 0 to #if 1' note; it is compiled out and, given the no-backward-compat policy, is dead source.
  - _Evidence:_ `#if 0 ... WiFi.hostByName('time.google.com') ... #endif  (System_Utils.cpp:2218-2244)`
  - _Fix:_ Delete the #if 0 DNS block (keep the surviving explanatory comment if useful).

### `BLE_IDF.cpp` (1) - _includes verified_

- **[BLE_IDF.cpp:3](components/hardwareone/BLE_IDF.cpp#L3)** `H` verified - Entire 1155-line experimental BLE_IDF implementation is compiled out and half-stubbed
  - BLE_IDF.cpp is gated on ENABLE_BLE_IDF_EXPERIMENTAL, which defaults to 0 in BLE_IDF.h and is set nowhere else in the repo, so this parallel Bluedroid server/client implementation never compiles; its GATTC client half is TODO stubs (bleIdfClientWrite/Scan/Connect 'not yet implemented') and bleIdfUpdateStreams only stamps timestamps without sending. It duplicates Bluetooth.cpp's server and hardcodes the device name, so it will rot further.
  - _Evidence:_ `#if ENABLE_BLUETOOTH && ENABLE_BLE_IDF_EXPERIMENTAL  (BLE_IDF.h: #define ENABLE_BLE_IDF_EXPERIMENTAL 0; no build config overrides it)`
  - _Fix:_ Delete BLE_IDF.cpp/.h before 1.0 (or move to an attic folder) rather than shipping a stale second BLE stack behind a never-set flag.

### `G2_Glasses.cpp` (9) - _includes verified_

- **[G2_Glasses.cpp:10550](components/hardwareone/G2_Glasses.cpp#L10550)** `H` verified - g2Tick() is never called, so overlay auto-dismiss never runs
  - The header says 'Call from main loop to process events' but no caller exists anywhere (hardwareone_loop never calls it; nm shows zero undefined refs across all first-party objects). Its only job is g2LensTickOverlay(), so G2_OVERLAY_* deadlines and the Files page's overlay-expired callback (g2LensSetOverlayExpiredCb) can never fire - a latent wiring bug, not just dead code.
  - _Evidence:_ `rg 'g2Tick' across components/hardwareone+main: only the def, header decl, and disabled stub; no call site`
  - _Fix:_ Either wire g2Tick() into hardwareone_loop or delete it plus g2LensTickOverlay/gOverlayExpiredCb and the overlay-deadline fields it services.
- **[G2_Glasses.cpp:1796](components/hardwareone/G2_Glasses.cpp#L1796)** `H` - enumerateDiagService survives only in commented-out call sites
  - The ~30-line BLE service walker was 'DISABLED 2026-05-04' - its three call sites (6338-6340) are comments - leaving the function unreferenced (symbol absent from the compiled object). The DIAG_SVC_7450 and DIAG_SVC_1001 constants (111-112) are stranded with it (DIAG_SVC_6450 stays live via the audio subscribe path).
  - _Evidence:_ `6331: '// DISABLED 2026-05-04: each enumerateDiagService call invokes...' with calls commented out`
  - _Fix:_ Delete enumerateDiagService and the two now-unused DIAG_SVC_* constants.
- **[G2_Glasses.cpp:3606](components/hardwareone/G2_Glasses.cpp#L3606)** `H` - Registry iterators g2RegisteredPageCount/g2RegisteredPageAt (and g2GetLeftTempleMac) are unused
  - The page-registry iteration pair (3606/3608) is exported in the header but nothing iterates the registry externally - internal consumers use gPageRegistry/populateHijackMenuItems directly - and g2GetLeftTempleMac (6974) has no caller either (the ring only needs the right temple). nm confirms zero undefined references to all three.
  - _Evidence:_ `rg + nm: no references outside their own definitions/header decls for all three symbols`
  - _Fix:_ Drop the two iterators and g2GetLeftTempleMac from the public API (fillTempleMac stays for the right-temple path).
- **[G2_Glasses.cpp:7334](components/hardwareone/G2_Glasses.cpp#L7334)** `H` - sendRebuildListNamedAndWait (~40 lines) has zero callers
  - Written for compound list children (comment cites camera-stream lstCam), but no call site exists under any config; the compound paths use sendRebuildMixedListMultiTextAndWait or UPDATE_TEXT instead. Symbol is absent from the compiled object.
  - _Evidence:_ `rg 'sendRebuildListNamedAndWait' -> definition only; nm on G2_Glasses.cpp.obj: not present`
  - _Fix:_ Delete it, or keep only if the planned per-child list REBUILD lands before 1.0.
- **[G2_Glasses.cpp:7575](components/hardwareone/G2_Glasses.cpp#L7575)** `H` - sendRebuildMultiTextAndWait (~95 lines) has zero callers
  - Defined at 7575 but never called; the Status compound path it was written for now uses sendRebuildMixedListMultiTextAndWait (9964) instead. The symbol is absent from the compiled G2_Glasses.cpp.obj, confirming no references under the current config, and no textual reference exists under any config gate.
  - _Evidence:_ `rg 'sendRebuildMultiTextAndWait' -> definition only; nm on G2_Glasses.cpp.obj: symbol eliminated`
  - _Fix:_ Delete the function (its g2BuildRebuildMultiTextPb builder in System_G2_Protocol has other uses, keep that).
- **[G2_Glasses.cpp:7830](components/hardwareone/G2_Glasses.cpp#L7830)** `H` - sendShutdownAndSettle and allocMagic kept as attribute((unused)) shelfware
  - Both are marked __attribute__((unused)) with 'retained for future use' comments and have zero callers; allocMagic also strands the gNextMagic counter (line 445), and image code now uses stable G2_MAGIC_* constants instead. Per project policy there is no need to keep speculative compat code.
  - _Evidence:_ `1002: static uint8_t __attribute__((unused)) allocMagic(); 7829: static bool __attribute__((unused)) sendShutdownAndSettle`
  - _Fix:_ Delete both functions plus gNextMagic; git history preserves them if the future use materializes.
- **[G2_Glasses.cpp:7941](components/hardwareone/G2_Glasses.cpp#L7941)** `H` - g2ShowMultiLine has no callers anywhere
  - Public API declared at G2_Glasses.h:817 with a disabled-branch stub, but no CLI command, page module, web blob, or other module ever calls it; nm confirms zero undefined references across all first-party objects.
  - _Evidence:_ `rg 'g2ShowMultiLine' -> def (7941), header decl (817), stub (1071) only`
  - _Fix:_ Delete the function, header declaration, and stub.
- **[G2_Glasses.cpp:10645](components/hardwareone/G2_Glasses.cpp#L10645)** `H` - Legacy packet API trio (g2SendPacket/g2CalcCRC16/g2EncodeVarint) is dead
  - g2SendPacket is an explicit 'legacy API, ignored' stub that always returns false, and g2CalcCRC16/g2EncodeVarint are thin wrappers over g2CrcCcittFalse/g2PbWriteVarint; none of the three has any caller repo-wide (nm confirms zero undefined refs). This project explicitly keeps no backwards-compat shims.
  - _Evidence:_ `10650: DEBUG_G2F("[G2] g2SendPacket() called - legacy API, ignored"); return false;`
  - _Fix:_ Delete all three functions and their header declarations (G2_Glasses.h:828-830).
- **[G2_Glasses.cpp:12737](components/hardwareone/G2_Glasses.cpp#L12737)** `M` - ~2,500 lines of image-discovery probes are test-suite-only
  - All 27 g2ProbeImage* / g2ProbeRebuildTextChild probes (lines ~12737-13637 and 15901-17834) plus private helpers used only by them (probeBanner/probeFooter, fillStripePattern, buildBmp2bpp, bmpDrawRect4bpp, runMixedListImageProbe, probeLiveScrollingBarSolo) are called solely from G2_Page_TestSuite.cpp; cmd_g2imgprobe does not dispatch to them. The public test hooks g2ShowTextPageRebuildProbe, g2ShowMultiTextPage, g2DebugSetNextBurstFragDelay and g2ProbeImageQ25SetPackPath are likewise test-suite-only, so this whole discovery layer ships in the 1.0 image.
  - _Evidence:_ `rg: every g2ProbeImageQ* call site is G2_Page_TestSuite.cpp; Q28 vs Q28L are ~90% copy-paste (~175 lines each)`
  - _Fix:_ Gate the probe block and the test-suite page behind an ENABLE_G2_TESTS build flag (or delete superseded probes) before 1.0.

### `System_Debug.h` (2) - _includes verified_

- **[System_Debug.h:911](components/hardwareone/System_Debug.h#L911)** `H` verified - DEBUG_SECURITYF macro has zero call sites and writes the shared gDebugBuffer unsynchronized
  - No first-party code invokes DEBUG_SECURITYF (only a redundant #ifndef fallback re-definition in HardwareOne.cpp:273), so the macro is dead; if it were ever used it is also ungated (runs regardless of flags/level) and snprintfs into the shared 1 KB gDebugBuffer from any task without a lock, racing other users of that buffer.
  - _Evidence:_ `#define DEBUG_SECURITYF(fmt, ...) do { if (ensureDebugBuffer()) { snprintf(getDebugBuffer(), 1024, "[SECURITY] " fmt, ...); ...`
  - _Fix:_ Delete both definitions, or reimplement it on debugQueuePrintf with a stack buffer if security logging is wanted.
- **[System_Debug.h:117](components/hardwareone/System_Debug.h#L117)** `H` - DEBUG_SECURITY flag never tested; DEBUG_SECURITYF duplicated and uninvoked
  - DEBUG_SECURITY (bit 1) is set in the boot-default mask but no isDebugFlagSet(DEBUG_SECURITY) exists and it has no name-table entry; its only macro DEBUG_SECURITYF is defined twice (System_Debug.h:911 and HardwareOne.cpp:274), unconditionally broadcasts without testing the flag, and is never invoked.
  - _Evidence:_ `DEBUG_SECURITY only at cpp:38 default mask; DEBUG_SECURITYF defined 2x, 0 call sites`
  - _Fix:_ Remove the DEBUG_SECURITY bit and the duplicated DEBUG_SECURITYF macro (or wire real usage through it).

### `System_FileManager.cpp` (3) - _includes verified_

- **[System_FileManager.cpp:244](components/hardwareone/System_FileManager.cpp#L244)** `H` verified - FileManager write/content/stats API is entirely unused
  - createFolder(244), createFile(259), deleteItem(275), renameItem(295), readFile(315), writeFile(341) and getStorageStats(360) have no caller anywhere; the only FileManager consumers (OLED_Mode_FileBrowser, G2_Page_Files) use it as a read-only browser, and all real mutations go through VFS::*Guarded / CLI handlers. nm confirms these symbols are defined (T) but referenced (U) by no other object in the current build.
  - _Evidence:_ `repo-wide grep: createFolder/createFile/deleteItem/renameItem/readFile/writeFile/getStorageStats external refs = 0`
  - _Fix:_ Delete the unused FileManager mutation/content/stats methods (they duplicate the VFS-guarded ops used elsewhere).
- **[System_FileManager.cpp:21](components/hardwareone/System_FileManager.cpp#L21)** `H` - gFileManager global is defined but never used
  - gFileManager is declared extern and defined = nullptr but never assigned or read; the real FileManager instances are gOledFileManager (OLED_Utils.cpp) and gFilesFm (G2_Page_Files.cpp). It is a leftover 'optional global instance' fossil.
  - _Evidence:_ `rg gFileManager -> only the extern decl (line 146 .h) and definition (line 21 .cpp); zero uses`
  - _Fix:_ Delete the gFileManager global and its extern declaration.
- **[System_FileManager.cpp:144](components/hardwareone/System_FileManager.cpp#L144)** `H` - FileManager nav/state accessors have no callers
  - moveToTop (144), moveToBottom (149), clearDirty (.h:87), needsRefresh (.h:86) and the private isProtectedPath (538) are never called; the browsers use moveUp/moveDown/refresh/getItem only. needsRefresh's 16 grep hits are all unrelated struct members.
  - _Evidence:_ `external refs = 0 for moveToTop, moveToBottom, clearDirty, isProtectedPath; needsRefresh() (FileManager) uncalled`
  - _Fix:_ Remove the unused accessors/predicate from FileManager.

### `WebPage_Maps.cpp` (1) - _includes verified_

- **[WebPage_Maps.cpp:293](components/hardwareone/WebPage_Maps.cpp#L293)** `H` verified - handleWaypointsPage (~170 lines) is unregistered and calls two functions that do not exist
  - handleWaypointsPage is never registered as a URI handler and never called; it references streamPageHeader/streamPageFooter (declared at lines 48-49) which have no definition anywhere - nm shows them as undefined symbols, so the function only links because gc-sections discards it. Related stale decls: handleMapSelectAPI/handleMapUnloadAPI (WebPage_Maps.h:2923-2924) have no definition or registration either.
  - _Evidence:_ `nm WebPage_Maps.cpp.obj: `U streamPageHeader(httpd_req*, char const*)`; no definition repo-wide; no httpd_register for handleWaypointsPage`
  - _Fix:_ Delete handleWaypointsPage, the streamPageHeader/Footer declarations, and the handleMapSelectAPI/handleMapUnloadAPI/handleWaypointsPage declarations in WebPage_Maps.h.

### `WebPage_Register.h` (1) - _includes verified_

- **[WebPage_Register.h:1](components/hardwareone/WebPage_Register.h#L1)** `H` verified - WebPage_Register.h is included by nothing - entire file is dead
  - No file in components/hardwareone or main includes WebPage_Register.h; the /register page and /register/submit result pages are streamed independently inside WebServer_Server.cpp (handleRegisterPage/handleRegisterSubmit, ~L3498-3628). The header also defines two non-inline, non-static functions, which would be an ODR hazard if it were ever included twice.
  - _Evidence:_ `grep -rn 'WebPage_Register' components/hardwareone main -> no matches outside the file itself`
  - _Fix:_ Delete WebPage_Register.h.

### `WebPage_Sensors.cpp` (2) - _includes verified_

- **[WebPage_Sensors.cpp:547](components/hardwareone/WebPage_Sensors.cpp#L547)** `H` verified - handleSensorsStatus is a superseded duplicate of handleSensorsStatusWithUpdates
  - The /api/sensors/status route registers handleSensorsStatusWithUpdates (WebPage_Sensors.cpp:1005, defined in WebServer_Server.cpp:2966); handleSensorsStatus is never registered or called, yet keeps stale declarations alive in WebServer_Server.h:280 and HardwareOne.cpp:339.
  - _Evidence:_ `httpd_uri_t sensorsStatus = { .uri="/api/sensors/status", .handler=handleSensorsStatusWithUpdates } - handleSensorsStatus has 0 callers/registrations`
  - _Fix:_ Delete handleSensorsStatus and its two declarations.
- **[WebPage_Sensors.cpp:169](components/hardwareone/WebPage_Sensors.cpp#L169)** `H` - Shadowed `String json` makes thermal fallback serialize into a discarded local
  - Inside the lockThermalCache block a second `String json;` shadows the outer one declared at line 131; serializeJson fills the inner String which is destroyed at the block end, so the whole ~768-element doc serialization is wasted and the handler sends the outer empty String (empty 200 body).
  - _Evidence:_ `String json; serializeJson(doc, json); } ... httpd_resp_send(req, json.c_str(), json.length());  // outer json still empty`
  - _Fix:_ Delete the inner declaration at line 169 so serializeJson writes to the outer `json`.

### `WebServer_MigrationTool.cpp` (1) - _includes verified_

- **[WebServer_MigrationTool.cpp:624](components/hardwareone/WebServer_MigrationTool.cpp#L624)** `H` verified - registerMigrationRestoreHandler/unregisterMigrationRestoreHandler are never called
  - Both are declared in WebServer_MigrationTool.h:30-33 and defined here, but the restore endpoints are registered inline by startRestoreOnlyHttpServer (L773-790) and torn down by stopping that server; nothing calls the register/unregister pair (the 'Gate 1/Gate 3' comments describe a flow that no longer exists).
  - _Evidence:_ `rg 'registerMigrationRestoreHandler|unregisterMigrationRestoreHandler' -> header decls + definitions only, zero call sites`
  - _Fix:_ Delete both functions and their header declarations; keep the inline registration in startRestoreOnlyHttpServer.

### `System_Debug.cpp` (3)

- **[System_Debug.cpp:598](components/hardwareone/System_Debug.cpp#L598)** `H` - drainDebugRing() is a documented no-op still called from sensor polling loops
  - The function body is empty ("No-op: Debug output task handles all output automatically") yet it is still declared in System_I2C.h and invoked from at least 5 sensor task loops (apds9960, seesaw, pa1010d, sths34pf80, ano_encoder) - a pointless cross-TU call per poll iteration and dead API surface going into 1.0.
  - _Evidence:_ `void drainDebugRing() { // No-op: Debug output task handles all output automatically }`
  - _Fix:_ Delete drainDebugRing() and its declaration/call sites.
- **[System_Debug.cpp:3274](components/hardwareone/System_Debug.cpp#L3274)** `H` - logToFile() is defined but never called
  - The generic logToFile(path,line,capBytes) wrapper (System_Debug.cpp:3274, declared System_Logging.h:76) has no callers; every log site calls appendLineWithCap or the specific logI2C*/logSystemEvent helpers directly.
  - _Evidence:_ `logToFile: only def (System_Debug.cpp:3274) + decl (System_Logging.h:76)`
  - _Fix:_ Remove logToFile and its declaration.
- **[System_Debug.cpp:3210](components/hardwareone/System_Debug.cpp#L3210)** `M` - DebugManager::queueDebugMessage and the GET_DEBUG_FLAGS/GET_LOG_LEVEL compat macros are unused
  - queueDebugMessage() and the System_Debug_Manager.h "backward compatibility during transition" macros GET_DEBUG_FLAGS()/GET_LOG_LEVEL() have no callers outside the debug files themselves - leftover transition scaffolding that also motivates keeping the costly singleton indirection (see the flag-off guard finding).
  - _Evidence:_ `void DebugManager::queueDebugMessage(DebugFlagMask flag, const char* message) { ... DEBUGF_QUEUE(flag, "%s", message); }`
  - _Fix:_ Remove queueDebugMessage and the unused compat macros as part of collapsing the DebugManager wrapper.

### `System_SensorLogging.cpp` (1)

- **[System_SensorLogging.cpp:351](components/hardwareone/System_SensorLogging.cpp#L351)** `H` - SensorCacheSnapshot fields written every tick but never read
  - snap.tof[i].detected (351) and snap.gApdsColorEnabled/gApdsProximityEnabled/gApdsGestureEnabled (400-402) are populated under mutex on every logging tick but no builder or hasSelectedData check ever reads them. The snapshot struct is used only in this file.
  - _Evidence:_ ``.detected`/`gApds*Enabled` assigned in snapshot but zero reads in buildFromSnap/buildCSVFromSnap/buildTrackFromSnap`
  - _Fix:_ Drop the unread fields from SensorCacheSnapshot and stop populating them.

### `G2_Ring.cpp` (3)

- **[G2_Ring.cpp:1655](components/hardwareone/G2_Ring.cpp#L1655)** `H` - g2RingNoteBridgePoll feeds a mirror only dead code reads
  - g2RingNoteBridgePoll is called live from parseSid80Rx (G2_Glasses.cpp:4783) on every RING_CONNECT_INFO poll and writes gBridgeProgress (1649), but the only reader of gBridgeProgress is the unregistered cmd_ringbridge status branch - so the hook does per-packet work into state nothing reachable ever displays.
  - _Evidence:_ `static R1BridgeProgress gBridgeProgress; - only read inside __attribute__((unused)) cmd_ringbridge`
  - _Fix:_ Remove gBridgeProgress, g2RingNoteBridgePoll, its header declaration/stub, and the call site in parseSid80Rx together with the ring-bridge cluster.
- **[G2_Ring.cpp:1524](components/hardwareone/G2_Ring.cpp#L1524)** `H` - ~500-line unreachable ring spoof/bridge cluster kept compiled
  - cmd_ringtoglasses (1525) and cmd_ringbridge (1696) are deliberately unregistered from g2RingCommands and marked __attribute__((unused)); their entire support graph is unreachable: ringSpoofSendOnce (1027), ringSpoofTaskBody (1093), ringSpoofStart/Stop (1151/1169), ringBridgeHeartbeatBody/Start/Stop (1590-1633), plus g2BuildRingRawDataPush + G2RingPushFields in System_G2_Protocol.cpp:1750 whose only caller is this dead path. The registry comment documents both as empirically-verified dead ends, and the project's no-backwards-compat policy says such reference code should be deleted (it also pins two EXT_RAM 300 B static buffers and keeps the gR1Cache read side alive).
  - _Evidence:_ `// DEPRECATED: unregistered from g2RingCommands ... __attribute__((unused)) static const char* cmd_ringtoglasses(...)`
  - _Fix:_ Delete the two commands, both worker tasks, ringSpoofSendOnce, g2BuildRingRawDataPush/G2RingPushFields, and move the empirical findings into R1_RING_PROTOCOL.md which already documents them.
- **[G2_Ring.cpp:85](components/hardwareone/G2_Ring.cpp#L85)** `H` - gRing.writeMutex created but never taken
  - g2RingInit() creates the semaphore (897) but no code ever takes or gives it - every gRing.writeChar->writeValue happens without it, from at least three different task contexts (connect flow, spoof task, cmd_exec ringquery). The intended serialization never materialized, so today it is just a leaked-at-boot semaphore plus a false sense of locking.
  - _Evidence:_ `SemaphoreHandle_t writeMutex = nullptr; ... gRing.writeMutex = xSemaphoreCreateMutex(); - zero xSemaphoreTake/Give references`
  - _Fix:_ Either wrap the ring writeValue sites in the mutex or delete the field and its creation.

### `OLED_UI.cpp` (2)

- **[OLED_UI.cpp:270](components/hardwareone/OLED_UI.cpp#L270)** `H` - Modal Dialog + Progress components and unified UI entry points are dead (~300 lines)
  - oledDialogOK/YesNo/Custom and oledProgressShow/Update/Label are never called anywhere, so gOledDialog/gOledProgress can never become active; the only input dispatcher oledUIHandleInput plus oledUIInit/oledUIModalActive also have zero callers, and the live modal confirm is the separate oledConfirmRequest overlay in OLED_Utils.cpp (17 call-sites) that this duplicates. oledUIRender still calls oledProgressRender+oledDialogRender as no-ops every render tick.
  - _Evidence:_ `gOledDialog/gOledProgress referenced only in OLED_UI.{h,cpp}; oledDialogOK/oledProgressShow/oledUIHandleInput/oledUIInit: decl+def only, zero call sites`
  - _Fix:_ Remove the Dialog and Progress components, gOledDialog/gOledProgress, oledUIInit/oledUIHandleInput/oledUIModalActive, and their transitively-dead helpers (oledDrawButton, oledUIButtonLabel, OledUIButton enum).
- **[OLED_UI.cpp:532](components/hardwareone/OLED_UI.cpp#L532)** `H` - Pairing-ribbon MINIMIZED state machine and several overlay APIs are vestigial
  - oledPairingRibbonMinimize/oledPairingRibbonHide/oledToastClear/oledNotificationBannerUpdate/oledDrawBox have zero callers, and PairingRibbonState::MINIMIZED is never entered (Update() immediately converts it to HIDDEN, comment says 'No longer used'), making the MINIMIZED render branch (~lines 734-744) and RIBBON_MIN_SIZE unreachable. Note oledNotificationBannerUpdate could plausibly be wanted by the in-flight notifications refactor - confirm before deleting that one.
  - _Evidence:_ `oledPairingRibbonMinimize/Hide, oledToastClear, oledNotificationBannerUpdate, oledDrawBox: decl+def only; case MINIMIZED: state = HIDDEN; // 'No longer used'`
  - _Fix:_ Remove the MINIMIZED enum value, its render branch, and the uncalled overlay functions (checking with the notifications-refactor owner about oledNotificationBannerUpdate).

### `System_ESPSR.cpp` (4)

- **[System_ESPSR.cpp:2017](components/hardwareone/System_ESPSR.cpp#L2017)** `H` - SR feed loop error branch unreachable; error counter never increments
  - err is initialized to ESP_OK and never reassigned - both the G2 and audioReadPcm read paths fold errors into 0 samples - so the `if (err != ESP_OK)` branch (lines 2049-2057) is dead and the gSrI2SReadErr telemetry counter reported by srstatus can never increment, masking real I2S failures as zero-byte reads. The check runs every audio-chunk iteration of the SR task.
  - _Evidence:_ `esp_err_t err = ESP_OK; ... if (err != ESP_OK) { gSrI2SReadErr++; ... } // no path assigns err`
  - _Fix:_ Delete err and the dead branch, or plumb a real error signal out of audioReadPcm/g2MicReadPcmSamples so gSrI2SReadErr is meaningful.
- **[System_ESPSR.cpp:598](components/hardwareone/System_ESPSR.cpp#L598)** `H` - Superseded voice-command lookup helpers are dead
  - phraseMatches(590), findCommandForCategoryTarget(598) and findCommandForCategorySubCategoryTarget(779) are never called - they are the obsolete siblings of the live findCommandForSingleStageCategory / loadTargetsForCategorySubCategory path. Dead under all configs.
  - _Evidence:_ `grep -rn returns only each function's definition line`
  - _Fix:_ Remove the three dead lookup helpers.
- **[System_ESPSR.cpp:1332](components/hardwareone/System_ESPSR.cpp#L1332)** `H` - srSnipDeinit() teardown is never called
  - srSnipDeinit() (~18 lines) deletes the snippet writer task, drains and deletes the job queue, and frees the ring buffer, but nothing ever calls it - so the SR snippet-capture resources are never released once initialized.
  - _Evidence:_ `grep -rn srSnipDeinit -> only the definition at line 1332`
  - _Fix:_ Call srSnipDeinit() from stopESPSR()/shutdown, or delete it if snippet capture never needs teardown.
- **[System_ESPSR.cpp:2686](components/hardwareone/System_ESPSR.cpp#L2686)** `H` - Wake-word callback registration path is dead
  - setESPSRWakeCallback(2686) is never called, so gWakeWordCallback (348) stays null and the guarded invocation at lines 2239-2240 can never fire. The wake-word callback plumbing is inert (only the command callback path is actually wired).
  - _Evidence:_ `grep -rn setESPSRWakeCallback -> definition only; gWakeWordCallback only set inside that dead setter`
  - _Fix:_ Remove setESPSRWakeCallback + gWakeWordCallback and the dead invocation, or register a real wake callback.

### `System_Microphone.cpp` (2)

- **[System_Microphone.cpp:227](components/hardwareone/System_Microphone.cpp#L227)** `H` - Recording loop's err is constant; error log branch dead
  - Same pattern as the SR loop: err is set to ESP_OK once and never modified (audioReadPcm errors fold into 0 samples), so the `else if (err != ESP_OK)` branch at line 260 can never execute; the condition is evaluated every ~128ms audio chunk for the life of a recording.
  - _Evidence:_ `esp_err_t err = ESP_OK; { size_t got = audioReadPcm(...); bytesRead = got * sizeof(int16_t); } ... else if (err != ESP_OK) { DEBUG_MIC_POLLINGF(...) }`
  - _Fix:_ Remove err and the unreachable branch (or surface a real error from audioReadPcm).
- **[System_Microphone.cpp:575](components/hardwareone/System_Microphone.cpp#L575)** `H` - captureAudioSamples() (~55 lines) is never called
  - captureAudioSamples() is declared in System_Microphone.h, stubbed in System_SensorStubs.h, and defined here (allocates a PSRAM buffer and pulls PCM via HAL_Audio), but no caller exists in any config. It is dead even under the XIAO S3 (mic-enabled) build.
  - _Evidence:_ `grep -rn captureAudioSamples -> only header decl, stub, and this definition; no call site`
  - _Fix:_ Remove captureAudioSamples() and its header declaration and stub.

### `i2csensor_mlx90640.cpp` (2)

- **[i2csensor_mlx90640.cpp:712](components/hardwareone/i2csensor_mlx90640.cpp#L712)** `H` - thermalPoll dead else-branch behind always-true static flag
  - useSpatialDownsampling is a function-static initialized true and never written again, so the full-resolution min/max else-branch (lines 737-749) can never execute. It is dead code carried through every frame's build and misleads readers into thinking the full-res path is selectable.
  - _Evidence:_ `static bool useSpatialDownsampling = true; ... if (useSpatialDownsampling) { ... } else { /* unreachable 768-px loop */ }`
  - _Fix:_ Delete the flag and the dead else-branch (or make it a real setting if full-res stats are wanted).
- **[i2csensor_mlx90640.cpp:716](components/hardwareone/i2csensor_mlx90640.cpp#L716)** `H` - hottestX/hottestY and frameCount computed every frame but never read
  - thermalPoll tracks hottestX/hottestY in three separate passes (downsample scan, filtered min/max pass, outlier pass) and increments a static frameCount, but nothing ever reads any of them - they are not stored in gThermalCache or logged. Pure wasted per-frame work at up to 8 FPS.
  - _Evidence:_ `int hottestX = 0, hottestY = 0; ... hottestX = i % 32; hottestY = i / 32; (assigned at 730, 745, 799 - zero reads) / static uint32_t frameCount ... frameCount++;`
  - _Fix:_ Remove hottestX/hottestY tracking and frameCount, or actually publish the hotspot coordinates in the cache/JSON.

### `System_CommandTypes.h` (1)

- **[System_CommandTypes.h:49](components/hardwareone/System_CommandTypes.h#L49)** `H` - CommandContext.id / timestampMs / replyHandle are write-only dead fields
  - Every command-submission site (11 files: HardwareOne.cpp, System_Utils.cpp, WebServer_Server.cpp, Bluetooth.cpp, System_ESPNow.cpp, System_Automation.cpp, OLED_Utils.cpp, G2_HijackCmd.cpp) sets ctx.id=millis(), ctx.timestampMs=millis(), and ctx.replyHandle=nullptr, but no code anywhere reads these three fields (executeCommand only receives ctx.auth). They are pure per-command boilerplate feeding dead struct members.
  - _Evidence:_ `uc.ctx.id=(uint32_t)millis(); uc.ctx.timestampMs=(uint32_t)millis(); uc.ctx.replyHandle=nullptr; - 11 sites each, 0 reads`
  - _Fix:_ Delete id, timestampMs, and replyHandle from CommandContext and remove the per-submission writes.

### `System_ESPNow.cpp` (10)

- **[System_ESPNow.cpp:314](components/hardwareone/System_ESPNow.cpp#L314)** `H` - gLastTimeSyncMs never written, so espnowtimestatus reports uptime as 'last sync'; TIME_SYNC_INTERVAL is dead
  - gLastTimeSyncMs stays 0 forever (v4h_time_sync sets gTimeOffset/gTimeIsSynced but not this), so cmd_espnow_timestatus (cpp:11234) prints 'Last sync: N seconds ago' where N is just seconds-since-boot. TIME_SYNC_INTERVAL (cpp:315, 'broadcast every 10 minutes, master only') has zero references - the periodic master time-sync it documents was never wired up; sync only happens via manual espnowtimesync.
  - _Evidence:_ `rg -w gLastTimeSyncMs -> only cpp:314 (def=0) and cpp:11234 (read); extern const unsigned long TIME_SYNC_INTERVAL = 600000 has no other reference`
  - _Fix:_ Either stamp gLastTimeSyncMs in v4h_time_sync and use TIME_SYNC_INTERVAL in the heartbeat task, or delete both and the misleading timestatus line.
- **[System_ESPNow.cpp:846](components/hardwareone/System_ESPNow.cpp#L846)** `H` - Post-LMK vestigial encryption-key plumbing: per-device key[16] stored/persisted but never used for crypto
  - addEspNowPeerWithEncryption explicitly ignores its useEncryption/encryptionKey parameters and always sets peerInfo.encrypt=false (LMK removed; AEAD sessions handle confidentiality), and there is no esp_now_set_pmk anywhere. Yet EspNowDevice.key[16] (System_ESPNow.h:106) is still filled from gEspNow->derivedKey on secure pair (cpp:12801,12813), hex-encoded, at-rest-encrypted, and persisted to devices.json (cpp:508-513) then loaded back (cpp:7262-7279) - 16 bytes of derived secret written to flash that nothing consumes.
  - _Evidence:_ `static bool addEspNowPeerWithEncryption(const uint8_t* mac, bool /*useEncryption*/, const uint8_t* /*encryptionKey*/) { ... peerInfo.encrypt = false; }`
  - _Fix:_ Drop EspNowDevice.key, the keyHex persistence/load code, and the ignored parameters (project policy allows clean breaking changes).
- **[System_ESPNow.cpp:301](components/hardwareone/System_ESPNow.cpp#L301)** `H` - 'Exported for .ino access' topology fossils: gMeshTopology vector, gExpectedWorkerCount, gLastTopoRequest, TopologyStream.path
  - std::vector<MeshTopoNode> gMeshTopology (cpp:301) has zero readers/writers in any config (the stub pointer in System_SensorStubs.cpp:221 is equally unreferenced), taking MeshTopoNode/MeshTopoPeer (System_ESPNow.h:156-170) with it; gExpectedWorkerCount (cpp:306) is never touched again; gLastTopoRequest (cpp:308, extern at h:1114 'for auto-discovery check in loop') is written once at cpp:7627 and never read. TopologyStream's String path field (h:134, 'path from master' - mesh is single-hop) is likewise never referenced.
  - _Evidence:_ `std::vector<MeshTopoNode> gMeshTopology; // Exported for .ino access - no reads; gExpectedWorkerCount/gLastTopoRequest write-only`
  - _Fix:_ Delete these globals, the MeshTopoNode/MeshTopoPeer structs, the stale extern, and the TopologyStream.path field.
- **[System_ESPNow.cpp:330](components/hardwareone/System_ESPNow.cpp#L330)** `H` - Dead metadata-change tracking: four static Strings never referenced plus write-only gMetadataChanged flag
  - gLastSentFriendlyName/gLastSentRoom/gLastSentZone/gLastSentTags (cpp:330-333) have zero references beyond their definitions (four global String objects constructed at static-init in DRAM), and gMetadataChanged (cpp:329) is set true at cpp:10452/10466 but never read - the debounced 'send metadata only if changed' mechanism they belonged to no longer exists (sendMetadata uses a plain time debounce).
  - _Evidence:_ `static String gLastSentFriendlyName = ""; ... 1 occurrence each; gMetadataChanged: 2 writes, 0 reads`
  - _Fix:_ Delete the four Strings and the gMetadataChanged flag with its two assignments.
- **[System_ESPNow.cpp:356](components/hardwareone/System_ESPNow.cpp#L356)** `H` - gMeshSeen/gMeshSeenIndex V3 dedup table superseded by gV4Dedup and now unreferenced
  - MeshSeenEntry gMeshSeen[MESH_DEDUP_SIZE] and gMeshSeenIndex ('Exported for .ino access') have zero references beyond their definitions; actual RX dedup is the V4DedupEntry gV4Dedup mechanism (cpp:1069, v4_dedup_seen_and_insert). The header struct MeshSeenEntry plus MESH_DEDUP_SIZE/MESH_DEDUP_WINDOW (System_ESPNow.h:268-274) are dead with it (~288 B static DRAM).
  - _Evidence:_ `MeshSeenEntry gMeshSeen[MESH_DEDUP_SIZE]; int gMeshSeenIndex = 0; - no other occurrence repo-wide; dedup path uses gV4Dedup`
  - _Fix:_ Delete gMeshSeen/gMeshSeenIndex and the MeshSeenEntry/MESH_DEDUP_* block in the header.
- **[System_ESPNow.cpp:368](components/hardwareone/System_ESPNow.cpp#L368)** `H` - Mesh retry-queue subsystem is a fossil: nothing ever enqueues
  - gMeshRetryQueue is only memset() at init/stop and read by a stats loop that can never see an active entry; no code sets .active or fills a slot. The whole apparatus is dead: MeshRetryEntry/MESH_RETRY_QUEUE_SIZE/MESH_ACK_TIMEOUT_MS/MESH_MAX_RETRIES (System_ESPNow.h:252-263), the dedicated gMeshRetryMutex + MeshRetryGuard (System_Mutex), the boot message 'Retry queue initialized (8 slots, 3s timeout, 2 retries)' (cpp:9015), and the always-empty retryQueue JSON in espnow debug output (cpp:11579-11606).
  - _Evidence:_ `static MeshRetryEntry gMeshRetryQueue[MESH_RETRY_QUEUE_SIZE]; - only writes are two memset()s; no assignment to .active anywhere`
  - _Fix:_ Delete the queue, its header structs/constants, gMeshRetryMutex/MeshRetryGuard, the boot log line, and the retryQueue stats section (real retries live in the V4 frag-ACK/sendStatus path).
- **[System_ESPNow.cpp:388](components/hardwareone/System_ESPNow.cpp#L388)** `H` - gPeerBuffer out-of-order topology buffer never read or written
  - static BufferedPeerMessage gPeerBuffer[MAX_BUFFERED_PEERS] has no reference anywhere except a stale System_Mutex.h comment claiming the topo mutex protects it. The BufferedPeerMessage struct and MAX_BUFFERED_PEERS (System_ESPNow.h:146-153) die with it - 10 slots each holding a String member (static-init cost plus DRAM).
  - _Evidence:_ `static BufferedPeerMessage gPeerBuffer[MAX_BUFFERED_PEERS]; - only other hit is the comment in System_Mutex.h:244`
  - _Fix:_ Delete gPeerBuffer, the BufferedPeerMessage struct, MAX_BUFFERED_PEERS, and fix the System_Mutex.h comment.
- **[System_ESPNow.cpp:870](components/hardwareone/System_ESPNow.cpp#L870)** `H` - sendChunkedResponse and shouldChunk are uncalled, with a mismatched stale header declaration
  - sendChunkedResponse (cpp:870, 5 params with a default) has zero callers; the header declares a different 4-param overload (System_ESPNow.h:1138) that is never defined - a leftover of the pre-clerk chunked-response path now handled by v4_send_command_response/v4_send_payload_smart. shouldChunk (cpp:6867, declared h:1027) is equally uncalled.
  - _Evidence:_ `header: sendChunkedResponse(mac,bool,String,String) vs cpp:870 sendChunkedResponse(...,uint32_t msgId = 0); rg -w finds no call site for either function`
  - _Fix:_ Delete both functions and their header declarations.
- **[System_ESPNow.cpp:2483](components/hardwareone/System_ESPNow.cpp#L2483)** `H` - Dead TX helpers: v4_send_text and the v4_broadcast_topo_request/v4_send_topo_request chain
  - v4_send_text (decl cpp:265, def cpp:2483) has no callers anywhere - TEXT sends go through the cmd_espnow_send chunk path directly. v4_broadcast_topo_request (fwd-decl cpp:263, def cpp:2228) is likewise never called, and v4_send_topo_request (cpp:2213) is only called from it; requestTopologyDiscovery (cpp:7620) builds and sends TOPO_REQ per-peer itself, duplicating that helper's body.
  - _Evidence:_ `rg -w v4_send_text / v4_broadcast_topo_request -> only decl+def(+comment); requestTopologyDiscovery sends TOPO_REQ via its own v4_send_frame loop (cpp:7632)`
  - _Fix:_ Delete all three helpers (or route requestTopologyDiscovery through v4_send_topo_request and delete the other two).
- **[System_ESPNow.cpp:7595](components/hardwareone/System_ESPNow.cpp#L7595)** `H` - meshSendEnvelopeToPeers has no callers; stale extern in System_ESPNow_Sensors.cpp
  - The generic envelope wrapper meshSendEnvelopeToPeers (def cpp:7595, decl h:1110) is never called; only the BOOT-typed sibling meshSendBootToPeers is used. System_ESPNow_Sensors.cpp:50 still carries a hand-written extern for it that nothing in that file uses either.
  - _Evidence:_ `callers of meshSendEnvelopeToPeers: none (def cpp:7595, decl h:1110, stale extern System_ESPNow_Sensors.cpp:50)`
  - _Fix:_ Delete meshSendEnvelopeToPeers, its header declaration, and the stale extern; inline meshBroadcastEnvelopeTyped into meshSendBootToPeers if desired.

### `System_ESPNow_Identity.h` (1)

- **[System_ESPNow_Identity.h:110](components/hardwareone/System_ESPNow_Identity.h#L110)** `M` - 5 of 7 event-subscription category bits are never consulted
  - peerIdentityWantsEvent is only ever called with ESPNOW_EVT_HEARTBEAT and ESPNOW_EVT_SENSOR (System_ESPNow.cpp:7909, 2417, 2448); the TOPOLOGY, BOND_HEARTBEAT, WORKER_STATUS (retired), METADATA_PUSH, and TIME_SYNC bits gate nothing, so a peer narrowing its subscription for those categories still receives everything. Caveat: the bitmask is a wire contract (V4PayloadSubscribe), so this is either dead enum surface or an unimplemented Phase-5 gap - decide which before 1.0.
  - _Evidence:_ `rg 'ESPNOW_EVT_' -> only ESPNOW_EVT_ALL, ESPNOW_EVT_HEARTBEAT, ESPNOW_EVT_SENSOR used outside the enum definition`
  - _Fix:_ Either add wantsEvent gates to the topology/bond-heartbeat/metadata/time-sync broadcast paths or trim the enum to the two honored bits.

### `System_ESPNow_Wire.h` (2)

- **[System_ESPNow_Wire.h:254](components/hardwareone/System_ESPNow_Wire.h#L254)** `H` - V4PayloadTimeSync.senderUptime never assigned or read - sent as uninitialized stack bytes
  - The only sender, v4_broadcast_time_sync (System_ESPNow.cpp:2203), declares the payload on the stack and fills only epochTime/timeOffset, so senderUptime goes on-air as 4 uninitialized bytes; the RX handler v4h_time_sync never reads it (it also ignores epochTime except for logging). Dead wire field plus a minor uninitialized-memory-disclosure hygiene issue.
  - _Evidence:_ `V4PayloadTimeSync payload; payload.epochTime = ...; payload.timeOffset = ...; // senderUptime never set (cpp:2204-2206); RX reads only timeOffset`
  - _Fix:_ Remove senderUptime from the struct (no-compat policy) or at least zero the payload before send.
- **[System_ESPNow_Wire.h:123](components/hardwareone/System_ESPNow_Wire.h#L123)** `H` - ESPNOW_V4_TYPE_METADATA_PUSH is RX-plumbed but unreachable: sendMetadata's isPush parameter is always false
  - The single sendMetadata call site passes isPush=false (System_ESPNow.cpp:8522), so opcode 55 is never transmitted; the RX table row (cpp:4382) and the PUSH/RESP branching inside v4h_metadata_resp_push and sendMetadata (cpp:3756, 6725-6748) are dead weight the Wire.h comment itself acknowledges. Under the no-backwards-compat policy there is no old-firmware sender to receive from either.
  - _Evidence:_ `sole caller: sendMetadata(gEspNow->metadataPendingResponseMac, false, true) (cpp:8522); Wire.h:123 comment: 'never TX'd today'`
  - _Fix:_ Either add the intended push-on-change caller or drop the opcode, the isPush parameter, and the PUSH branches.

### `WebPage_Speech.h` (1)

- **[WebPage_Speech.h:592](components/hardwareone/WebPage_Speech.h#L592)** `H` - Speech page model-upload button POSTs to nonexistent /api/upload
  - uploadModelFile() fetches '/api/upload', but no handler registers that URI anywhere - the real upload endpoint is /api/files/upload. The 'Upload Model' button on /speech therefore always fails with a 404.
  - _Evidence:_ `fetch('/api/upload', {method:'POST',...}) - only occurrence of 'api/upload' repo-wide; registered endpoint is /api/files/upload`
  - _Fix:_ Point uploadModelFile() at /api/files/upload (matching its multipart contract) or remove the upload UI from the speech page.

### `BLE_Peers.cpp` (1)

- **[BLE_Peers.cpp:123](components/hardwareone/BLE_Peers.cpp#L123)** `H` - Peer-registry query/iteration accessors never called
  - bleRegisteredPeerCount() (123), bleRegisteredPeerAt() (125), and bleIsPeerRegistered() (104) are declared in BLE_Peers.h and defined here but have zero callers anywhere in components/hardwareone or main; cmd_blepeers/blePeersWriteJson iterate the registry via other internals. They are an unused public iteration API.
  - _Evidence:_ `repo-wide search: only the definitions of bleRegisteredPeerCount/bleRegisteredPeerAt/bleIsPeerRegistered exist, no call sites`
  - _Fix:_ Remove the three accessors and their declarations from BLE_Peers.h.

### `Bluetooth.cpp` (3)

- **[Bluetooth.cpp:1145](components/hardwareone/Bluetooth.cpp#L1145)** `H` - getBLEConnectionDuration() is never called
  - getBLEConnectionDuration() is declared in Bluetooth.h (with a disabled-build stub) and defined here but has zero callers across components/hardwareone and main. Unused public accessor.
  - _Evidence:_ `repo-wide search: only the definition at Bluetooth.cpp:1145 plus header decl/stub, no call sites`
  - _Fix:_ Remove getBLEConnectionDuration and its declaration/stub from Bluetooth.h.
- **[Bluetooth.cpp:1358](components/hardwareone/Bluetooth.cpp#L1358)** `H` - getBLEStatus(char*,size_t) is never called
  - getBLEStatus() builds a multi-line text status into a caller buffer and is declared in Bluetooth.h (with a disabled-build stub), but has no callers anywhere in components/hardwareone or main; status is surfaced instead via cmd_blestatus/cmd_bleinfo. It is a dead alternate status builder.
  - _Evidence:_ `repo-wide search: only Bluetooth.cpp:1358 definition and the two header declarations, no call sites`
  - _Fix:_ Remove getBLEStatus and its declaration/stub from Bluetooth.h.
- **[Bluetooth.cpp:1382](components/hardwareone/Bluetooth.cpp#L1382)** `H` - bleApplySettings() is never called
  - bleApplySettings() (refreshes the BLE MAC cache) is declared in Bluetooth.h with a disabled-build stub and defined here, but nothing calls it - the bluetoothSettingsModule's 5th field is the isBLERunning status callback, not this apply hook. The MAC cache is only ever refreshed lazily inside bleIdentifyDeviceByMAC.
  - _Evidence:_ `only refs are Bluetooth.cpp:1382 (def), Bluetooth.h:188 (decl), Bluetooth.h:231 (stub); no call sites`
  - _Fix:_ Delete bleApplySettings, or wire it into the settings-apply path if the eager cache refresh is wanted.

### `G2_Glasses.h` (2)

- **[G2_Glasses.h:74](components/hardwareone/G2_Glasses.h#L74)** `H` - 15 legacy protocol #defines in the header are unreferenced
  - G2_UUID_BASE, G2_SERVICE_UUID, G2_CHAR_*_UUID, G2_PACKET_MAGIC/TYPE_CMD/TYPE_RSP, G2_MTU_TARGET, G2_AUTH_PACKET_COUNT and the ten G2_SVC_* hi/lo bytes (lines 74-98) have zero uses anywhere; the .cpp owns its own authoritative SERVICE_UUID/CHAR_* constants (lines 91-118). The header comment itself calls them 'Legacy #defines'. _(Update 2026-07-28: G2_MTU_TARGET removed during the per-link MTU pass; the rest remain.)_
  - _Evidence:_ `rg for each macro name: only the #define lines in G2_Glasses.h match`
  - _Fix:_ Delete the macro block (lines 74-98).
- **[G2_Glasses.h:103](components/hardwareone/G2_Glasses.h#L103)** `H` - 11 of 14 G2ClientState fields are never referenced
  - Only state, initialized and connectedSince are ever read/written; deviceName, deviceAddress, mtu, targetEye, seqNumber, msgId, packetsSent, packetsReceived, authAttempts, eventCallback, deferredGesturePending and deferredGestureEvent have zero accesses. The two unused String members are the sole reason initG2Client (G2_Glasses.cpp:6429-6440) needs the ps_calloc + placement-new + comment-heavy destructor dance.
  - _Evidence:_ `rg 'gG2State->' shows only ->state, ->initialized, ->connectedSince accesses in the whole repo`
  - _Fix:_ Shrink G2ClientState to the three live fields (making it trivially constructible and removing the placement-new ceremony).

### `G2_HijackCmd.h` (1)

- **[G2_HijackCmd.h:68](components/hardwareone/G2_HijackCmd.h#L68)** `H` - Unused lens-applier scaffolding: g2CurrentCmdSeq, Toast kind, G2_LENS_GEN_GUARD
  - g2CurrentCmdSeq() (defined G2_HijackCmd.cpp:61) has zero callers; ToastSpec is forward-declared and given a union slot + LensJobKind::Toast enum value but nothing ever constructs a Toast job (the applier switch just logs 'not implemented - dropping'); the G2_LENS_GEN_GUARD macro (default 0) is referenced only in comments. All are leftovers of refactor steps that were completed differently.
  - _Evidence:_ `uint64_t g2CurrentCmdSeq(); / struct ToastSpec; / #define G2_LENS_GEN_GUARD 0 - no non-comment references`
  - _Fix:_ Delete g2CurrentCmdSeq, the ToastSpec forward-decl + union member + enum value + applier case, and the G2_LENS_GEN_GUARD macro.

### `G2_Page_Network.cpp` (1)

- **[G2_Page_Network.cpp:162](components/hardwareone/G2_Page_Network.cpp#L162)** `H` - Four g2Show*Page/List wrappers have no callers
  - g2ShowNetworkPage (G2_Page_Network.cpp:162), g2ShowFilesPage (G2_Page_Files.cpp:599), g2ShowSettingsPage (G2_Page_Settings.cpp:579) and g2ShowSensorList (G2_Page_Sensors.cpp:462) are declared in their headers (with stub inlines) but nothing calls them - the CLI g2network/g2files/g2settingspage/g2sensors commands all route through cmd_g2page_run + the page registry's buildText hook (G2_Glasses.cpp:11810), which duplicates exactly what these wrappers do. Verified by repo-wide search including command tables and WebPage_*.h blobs.
  - _Evidence:_ `bool g2ShowNetworkPage() { char buf[400]; g2BuildNetworkInfo(buf, sizeof(buf)); ... return g2ShowText(buf); }`
  - _Fix:_ Delete all four wrappers plus their header declarations and disabled-build stubs; cmd_g2page_run is the single live path.

### `G2_Ring.h` (1)

- **[G2_Ring.h:42](components/hardwareone/G2_Ring.h#L42)** `H` - 19 stale G2RING_CMD_*/G2RING_FLAG_* macros superseded by R1_* constants
  - The entire G2RING_CMD_* / G2RING_FLAG_* block (lines 42-62) has zero uses anywhere - the live wire code uses the verified R1_MODULE_*/R1_CMD_*/R1_SUB_* constants from System_R1_Protocol.h. The macros come from the older ring.ts community RE with different names for overlapping opcode values (e.g. G2RING_CMD_FIRMWARE 0x02 vs R1_CMD_SPO2 0x02), so keeping both tables invites picking the wrong one.
  - _Evidence:_ `#define G2RING_CMD_HEARTRATE 0x01 ... #define G2RING_FLAG_RESPONSE 0x0003 - each symbol appears exactly once (its definition)`
  - _Fix:_ Delete the macro block and its reference comment; System_R1_Protocol.h is the protocol charter.

### `HAL_Audio.cpp` (1)

- **[HAL_Audio.cpp:110](components/hardwareone/HAL_Audio.cpp#L110)** `H` - HAL_Audio source-switch + capture-status API is unused (Phase-2 scaffold)
  - audioGetSource(110), audioSetSource(112), audioCaptureActive(121) and audioCaptureOwner(122) are never called anywhere. Since audioSetSource is never invoked, gAudioSource is permanently AUDIO_SRC_LOCAL_PDM, making the AUDIO_SRC_G2_LEFT branches at lines 144, 161 and 168-169 unreachable dead code too.
  - _Evidence:_ `grep -rn: audioGetSource/audioSetSource/audioCaptureActive/audioCaptureOwner appear only at their definitions`
  - _Fix:_ Drop the unused accessors and either remove the G2-source branches or land the Phase-2 caller that registers the source.

### `HAL_Display.cpp` (1)

- **[HAL_Display.cpp:205](components/hardwareone/HAL_Display.cpp#L205)** `H` - displaySetBrightness and displayDim have no callers; brightness logic re-rolled in applyOLEDBrightness
  - Neither displayDim() nor displaySetBrightness() is called anywhere under any config; OLED_Utils.cpp:6253 applyOLEDBrightness() instead re-implements the SSD1306 contrast sequence inline (correctly wrapped in i2cDeviceTransactionVoid, which the HAL version dangerously lacks). The HAL functions are both dead code and an unsafe-to-adopt duplicate.
  - _Evidence:_ `rg '\bdisplayDim\b|\bdisplaySetBrightness\b' -> only HAL_Display.{h,cpp}; applyOLEDBrightness: ssd1306_command(SSD1306_SETCONTRAST) inside i2cDeviceTransactionVoid`
  - _Fix:_ Delete displayDim/displaySetBrightness, or make displaySetBrightness the bus-transaction-wrapped implementation and have applyOLEDBrightness call it.

### `HAL_Input.cpp` (1)

- **[HAL_Input.cpp:139](components/hardwareone/HAL_Input.cpp#L139)** `H` - Runtime controller-switching layer in HAL_Input is unreachable
  - inputSetControllerType/inputGetControllerType/inputSetCustomButtonMapping/inputGetCustomButtonMapping have zero callers, so gCurrentControllerType can only ever be its compile-time value (SEESAW or ANO - INPUT_TYPE is derived only from INPUT_DEVICE_TYPE==2, so INPUT_TYPE_CLICK_WHEEL/INPUT_TYPE_CUSTOM are unselectable under every config). gClickWheelMapping and the writable gCustomMapping (24B DRAM) plus the CLICK_WHEEL/CUSTOM switch arms and inputAbstractionInit's redundant re-assignment are dead.
  - _Evidence:_ `inputSetControllerType/inputSetCustomButtonMapping: only decl (HAL_Input.h) + def (HAL_Input.cpp); INPUT_TYPE derives only to ANO_ENCODER or SEESAW_GAMEPAD`
  - _Fix:_ Collapse to the two real controller types: drop the get/set-controller and custom-mapping APIs, gClickWheelMapping/gCustomMapping, and the CLICK_WHEEL/CUSTOM enum branches.

### `HardwareOne.cpp` (2)

- **[HardwareOne.cpp:459](components/hardwareone/HardwareOne.cpp#L459)** `H` - stripANSICSI/printToSerial are dead (and un-linkable externally)
  - static stripANSICSI (459) is only called by static printToSerial (491), which has zero callers in this translation unit; the sole other reference is an extern declaration in System_ESPNow.cpp:71 that cannot bind to a static definition and is itself never called. Dead code carrying a per-call String-copy pattern that invites future misuse.
  - _Evidence:_ `static String stripANSICSI(const String& in) ... static inline void printToSerial(const String& s) { broadcastOutput(stripANSICSI(s)); }`
  - _Fix:_ Delete both functions and the stale extern declaration in System_ESPNow.cpp.
- **[HardwareOne.cpp:398](components/hardwareone/HardwareOne.cpp#L398)** `M` - gWebMirrorSeq is initialized to 0 and never incremented
  - volatile unsigned long gWebMirrorSeq (HardwareOne.cpp:398) is only ever assigned its initial 0 and is read exactly once in a debug log (WebServer_Server.cpp:2958); nothing ever advances it, so the 'sequence' is permanently 0. It is a vestigial counter left from a removed web-mirror mechanism.
  - _Evidence:_ `gWebMirrorSeq: def '=0' only, no '++'/reassignment; single read in DEBUG_HTTPF`
  - _Fix:_ Delete gWebMirrorSeq and the stale debug read, or restore the increment if the sequence is still wanted.

### `OLED_Display.h` (1)

- **[OLED_Display.h:423](components/hardwareone/OLED_Display.h#L423)** `H` - Stale declarations: displayMenu() has no definition; displayConnectedSensors() declared twice
  - void displayMenu(); refers to a function that no longer exists anywhere (replaced by displayMenuListStyle in OLED_Mode_Menu.cpp - OLED_Utils.cpp:5224 only has a 'moved' comment), and displayConnectedSensors is declared at both line 396 and 449 of the same header.
  - _Evidence:_ `rg '\bdisplayMenu\b' -> only OLED_Display.h:423 decl + OLED_Utils.cpp:5224 comment; displayConnectedSensors declared at OLED_Display.h:396 and :449`
  - _Fix:_ Remove the displayMenu declaration and the duplicate displayConnectedSensors declaration.

### `OLED_ESPNow.cpp` (2)

- **[OLED_ESPNow.cpp:653](components/hardwareone/OLED_ESPNow.cpp#L653)** `H` - Four public OLED ESP-NOW functions defined but never called
  - oledEspNowGetMainMenuItemCount (653), oledEspNowFormatMac (1574, a one-line wrapper around macToDisplayStr), oledEspNowValidateDevicePtr (1630), and oledEspNowOpenSettings (1878) have zero callers; each appears only at its definition and header declaration. The settings view is entered inline at line 685 instead of via oledEspNowOpenSettings, and device-detail rendering validates messages via oledEspNowValidateMessagePtr only.
  - _Evidence:_ `rg word-boundary search across components/hardwareone + main: definition + OLED_ESPNow.h declaration are the only hits for all four`
  - _Fix:_ Delete the four functions and their OLED_ESPNow.h declarations (lines 146, 158, 171, 181).
- **[OLED_ESPNow.cpp:2330](components/hardwareone/OLED_ESPNow.cpp#L2330)** `H` - Entire legacy remote-file-browse subsystem is dead
  - The 'Remote File Browsing' block (lines ~2313-2466: RemoteFileBrowseState, gRemoteFileBrowse PSRAM static ~800B, oledEspNowSendBrowseRequest which is itself a 'not yet implemented' placeholder, oledEspNowDisplayRemoteFiles, oledEspNowHandleRemoteFilesInput, storeRemoteFileBrowseResult) has zero callers repo-wide. It was superseded by the FsList-based shared file browser (oledFileBrowserStartEspnowReceive/Send); the companion MSG_TYPE_FILE_BROWSE macro in System_ESPNow.h:63 is also referenced nowhere.
  - _Evidence:_ `broadcastOutput("[ESP-NOW] Remote file browse requires stored credentials (not yet implemented)"); + EXT_RAM_BSS_ATTR static RemoteFileBrowseState gRemoteFileBrowse;`
  - _Fix:_ Delete the whole block, its four header declarations in OLED_ESPNow.h (125-186 region and 193), and MSG_TYPE_FILE_BROWSE.

### `OLED_ESPNow.h` (1)

- **[OLED_ESPNow.h:99](components/hardwareone/OLED_ESPNow.h#L99)** `H` - OLEDEspNowState.showingStatusDetail is write-only
  - The field is initialized to false in oledEspNowInit (OLED_ESPNow.cpp:133) and never read or set anywhere else; the status view is driven purely by currentView == ESPNOW_VIEW_STATUS.
  - _Evidence:_ `rg 'showingStatusDetail' -> only the struct declaration and the single init write`
  - _Fix:_ Delete the field and its init line.

### `OLED_FirstTimeSetup.cpp` (1)

- **[OLED_FirstTimeSetup.cpp:206](components/hardwareone/OLED_FirstTimeSetup.cpp#L206)** `H` - getOLEDYesNoPrompt() is a large unused public function
  - This ~135-line function (lines 206-341) is declared in OLED_FirstTimeSetup.h but has zero callers anywhere in components/hardwareone or main; the setup flows use the serial-fallback yes/no path instead. Repo-wide search on the full name found only the definition and the header prototype.
  - _Evidence:_ `bool getOLEDYesNoPrompt(const char* prompt, bool defaultYes) {  // no callers in first-party tree`
  - _Fix:_ Delete getOLEDYesNoPrompt() and its prototype in OLED_FirstTimeSetup.h.

### `OLED_Mode_System.cpp` (1)

- **[OLED_Mode_System.cpp:37](components/hardwareone/OLED_Mode_System.cpp#L37)** `H` - Legacy non-'Rendered' display functions are dead duplicates
  - displaySystemStatus() (line 37), displayMemoryStats() (line 77), displayWebStats() (line 134), and displayConnectedSensors() (OLED_Mode_Sensors.cpp line 309) have no callers - mode registration uses the *Rendered two-phase versions - yet they duplicate the live rendering logic (displaySystemStatus even prints WiFi.SSID() inline, the pattern the Rendered versions were written to avoid). ~350 lines of flash and a maintenance trap of two divergent copies.
  - _Evidence:_ `rg shows only declarations in OLED_Display.h and the definitions; sSystemModes/sSensorModes register displaySystemStatusRendered / displayConnectedSensorsRendered instead`
  - _Fix:_ Delete the four legacy functions and their declarations in OLED_Display.h (lines 441-443, 396, 449).

### `OLED_RemoteSettings.cpp` (1)

- **[OLED_RemoteSettings.cpp:228](components/hardwareone/OLED_RemoteSettings.cpp#L228)** `H` - getRemoteSettingsModules() is defined but never called
  - This exported accessor has no callers; the remote-settings mode's displaySettingsEditor() actually renders via getSettingsModules() (the LOCAL module set), so the remote flat-array builder's output is never consumed through this API. Confirms both dead code and a likely latent bug where remote settings shows local data.
  - _Evidence:_ `const SettingsModule** getRemoteSettingsModules(size_t& count) { ... } // no callers; editor uses getSettingsModules()`
  - _Fix:_ Delete getRemoteSettingsModules() (and its header prototype), or wire the editor to actually call it.

### `OLED_SettingsEditor.cpp` (1)

- **[OLED_SettingsEditor.cpp:78](components/hardwareone/OLED_SettingsEditor.cpp#L78)** `H` - resetSettingsEditor() is an uncalled one-line alias of initSettingsEditor()
  - The wrapper just forwards to initSettingsEditor() and has zero callers repo-wide despite being exported in OLED_SettingsEditor.h - an obsolete shim in a project that explicitly keeps no compatibility layers.
  - _Evidence:_ `void resetSettingsEditor() { initSettingsEditor(); } - decl+def are the only references`
  - _Fix:_ Delete the function and its header declaration.

### `OLED_SetupWizard.cpp` (1)

- **[OLED_SetupWizard.cpp:1309](components/hardwareone/OLED_SetupWizard.cpp#L1309)** `H` - runOLEDSetupWizard() is a dead backwards-compat shim
  - The function just forwards to runSetupWizard() and its own header comment calls it "kept for any existing call sites", but no call sites exist in the first-party tree. This project explicitly needs no backwards compatibility.
  - _Evidence:_ `SetupWizardResult runOLEDSetupWizard() { return runSetupWizard(); } // zero callers`
  - _Fix:_ Remove runOLEDSetupWizard() and its declaration in OLED_SetupWizard.h.

### `OLED_Utils.cpp` (6)

- **[OLED_Utils.cpp:106](components/hardwareone/OLED_Utils.cpp#L106)** `H` - 'Standardized Footer System' (FOOTER_* presets + oledRenderFooter) never fires; duplicated by drawOLEDFooter
  - All five FOOTER_* preset globals are referenced nowhere outside their definitions, and oledRenderFooter's only call site is guarded by oledScrollRender's footerHints parameter, which every caller passes as nullptr (explicitly or by default). The live footer is the independent hand-rolled drawOLEDFooter() central switch - two parallel footer systems, one of them stillborn.
  - _Evidence:_ `oledScrollRender(...footerHints=nullptr) at every call site (Menu.cpp:234/316 pass nullptr; others omit); FOOTER_BACK_ONLY etc. defined at 106-110, never used`
  - _Fix:_ Delete oledRenderFooter, the five FOOTER_* presets, and the OLEDFooterHints plumbing through oledScrollRender (or actually route drawOLEDFooter through it - pick one system).
- **[OLED_Utils.cpp:1004](components/hardwareone/OLED_Utils.cpp#L1004)** `H` - Uncalled list/content/keyboard helpers: oledScrollPageUp/Down, oledScrollGetItem, oledScrollCalculateVisibleLines, oledContentPrintAt, oledContentSetCursor, oledKeyboardShowingSuggestions
  - Seven exported helper functions in the scroll/content/keyboard toolkits have zero call sites repo-wide (including sensor *_oled.h consumers, WebPage blobs, and command tables); the rest of each toolkit is heavily used, so these are leftover surface, not gated features.
  - _Evidence:_ `each symbol: 1 decl in OLED_Utils.h + 1 def in OLED_Utils.cpp, no other hits across components/hardwareone + main`
  - _Fix:_ Delete the seven functions and their header declarations.
- **[OLED_Utils.cpp:2295](components/hardwareone/OLED_Utils.cpp#L2295)** `H` - static getBluetoothActionText() has no callers
  - The file-local helper computing the Bluetooth footer action label (Start/Stop Adv/Advertise/Disconnect) is never invoked - the footer hints for the Bluetooth modes now come from the mode registry's hints strings - leaving ~15 lines of BLE-state logic to rot.
  - _Evidence:_ `rg 'getBluetoothActionText' -> single hit: its definition at OLED_Utils.cpp:2295`
  - _Fix:_ Delete the function (as a static it may also be generating an unused-function warning).
- **[OLED_Utils.cpp:2704](components/hardwareone/OLED_Utils.cpp#L2704)** `H` - Zero-caller 'compatibility' accessors: oledMarkDirtyMode, oledSetAlwaysDirty, getPreviousOLEDMode, getRegisteredOLEDModes, getOLEDModeByIndex, getRegisteredOLEDModeCount
  - Six public functions declared in OLED_Display.h have definition+declaration as their only repo-wide references (two are even commented 'for compatibility'); this project explicitly keeps no backwards compatibility. oledSetAlwaysDirty being dead also means its always-dirty flag is permanently false.
  - _Evidence:_ `oledMarkDirtyMode(2704), oledSetAlwaysDirty(2734), getRegisteredOLEDModes(2929), getRegisteredOLEDModeCount(2934), getOLEDModeByIndex(2939), getPreviousOLEDMode(5288)`
  - _Fix:_ Delete all six functions, their OLED_Display.h declarations, and whatever static state only they touched.
- **[OLED_Utils.cpp:5221](components/hardwareone/OLED_Utils.cpp#L5221)** `H` - batteryIconState global + BATTERY_ICON_UPDATE_INTERVAL + struct BatteryIconState entirely unused
  - The global is defined and extern-exported but never read or written anywhere (header rendering calls getBatteryPercentage()/getBatteryIcon() directly); the 2-minute interval constant and the struct type are equally orphaned, with OLED_Mode_Menu.cpp carrying only a stale comment about it.
  - _Evidence:_ `BatteryIconState batteryIconState = {0}; extern const unsigned long BATTERY_ICON_UPDATE_INTERVAL = 120000; - no other read/write in repo`
  - _Fix:_ Delete the global, the constant, and the BatteryIconState struct/extern block in OLED_Utils.h.
- **[OLED_Utils.cpp:4531](components/hardwareone/OLED_Utils.cpp#L4531)** `H` - Entire dynamic/remote-menu subsystem unreachable (~440 lines, ~7.7KB PSRAM)
  - buildDynamicMenu() is only called by getFilteredMenuItemCount() which itself has zero callers, and buildRemoteSubmenu/exitRemoteSubmenu/getRemoteSubmenu*/setRemoteSubmenuSelection/isRemoteCommandInputActive/completeRemoteCommandInput/startRemoteCommandInput ([[maybe_unused]]) have no callers at all; OLED_Mode_Menu.cpp:188 even documents 'that subsystem is currently unreachable'. Two EXT_RAM_BSS arrays (gDynamicMenuItems, gRemoteSubmenuItems, 32x~120B each) hold ~7.7KB PSRAM forever, plus loadRemoteMenuItems/loadCachedManifest/addSubmenuHeader are transitively dead (lines ~4527-4967).
  - _Evidence:_ `EXT_RAM_BSS_ATTR OLEDMenuItemEx gDynamicMenuItems[32]; ... gRemoteSubmenuItems[32]; buildDynamicMenu() only caller = uncalled getFilteredMenuItemCount()`
  - _Fix:_ Delete the block from the Dynamic Menu System banner (~4527) through getFilteredMenuItemCount (~4967) plus the decls in OLED_Display.h and the extern stanzas in OLED_Mode_Menu.cpp/System_ESPNow.cpp menu-source hook.

### `OLED_Utils.h` (1)

- **[OLED_Utils.h:149](components/hardwareone/OLED_Utils.h#L149)** `H` - OLEDScrollItem.icon and .validationKey (and OLEDScrollState.footer) are written but never read
  - icon is only ever zeroed, validationKey is only ever assigned (from refreshCounter, whose sole purpose is feeding it), and state->footer is set to nullptr once and never consulted - no code reads any of them. That's ~5 wasted bytes x 32 items in every static OLEDScrollState (several live in PSRAM, some in DRAM), plus OLEDHeaderInfo.notificationCount which no renderer reads.
  - _Evidence:_ `items[idx].validationKey = state->refreshCounter (write-only); items[i].icon = 0 (write-only); state->footer = nullptr (write-only)`
  - _Fix:_ Drop the icon/validationKey/footer fields (and refreshCounter if nothing else needs it) and the notificationCount header field to shrink every scroll-state instance.

### `System_Automation.cpp` (3)

- **[System_Automation.cpp:926](components/hardwareone/System_Automation.cpp#L926)** `H` - Four automation functions are defined but never called
  - runAutomationCommandUnified(926) and the suspendAutomationSystem(914)/resumeAutomationSystem(920) lifecycle pair are declared in System_Automation.h but have no callers anywhere; findAutomationsArrayBounds(364) is a locally-declared helper that is also never called.
  - _Evidence:_ `runAutomationCommandUnified/suspendAutomationSystem/resumeAutomationSystem/findAutomationsArrayBounds: only definition sites found repo-wide`
  - _Fix:_ Delete these four dead functions and their header declarations.
- **[System_Automation.cpp:893](components/hardwareone/System_Automation.cpp#L893)** `H` - gAutoMemoId/gAutoMemoNextAt PSRAM buffers allocated at init but never used
  - initAutomationSystem ps_allocs gAutoMemoId (128*8B) and gAutoMemoNextAt (128*8B) and zeroes gAutoMemoCount, but these three globals (declared extern in the header) are never read or written anywhere else in the repo. ~2KB of PSRAM plus three dead externs and the kAutoMemoCap macro.
  - _Evidence:_ `gAutoMemoId/gAutoMemoNextAt appear only at decl+alloc (lines 892-901); gAutoMemoCount only set to 0; zero external readers`
  - _Fix:_ Remove the two ps_alloc calls, the three gAutoMemo* globals, and kAutoMemoCap.
- **[System_Automation.cpp:191](components/hardwareone/System_Automation.cpp#L191)** `M` - Stale validateConditionalCommand declaration and stale computeNextRunTime extern
  - validateConditionalCommand is forward-declared at line 191 with no definition and no caller anywhere. Separately, System_Filesystem.cpp:32 carries an extern for computeNextRunTime that is never called in that file.
  - _Evidence:_ `validateConditionalCommand: only line 191 exists; System_Filesystem.cpp:32 extern computeNextRunTime unused`
  - _Fix:_ Delete the validateConditionalCommand declaration and the unused extern in System_Filesystem.cpp.

### `System_BondedPeer.cpp` (1)

- **[System_BondedPeer.cpp:70](components/hardwareone/System_BondedPeer.cpp#L70)** `H` - 5 of 13 BondedPeer accessors never called
  - BondedPeer::isMaster (line 70), isOnline (72), peerMacString (74), peerSettingsHash (97), and isSettingsDirty (105) have zero callers across components/hardwareone and main (callers use isPaired/peerMacBytes/peerName/sync/lastError only). isSettingsDirty's documented 'settings changed since last sync' UI hint was evidently never wired into WebPage_Bond.
  - _Evidence:_ `rg 'BondedPeer::(isMaster|isOnline|peerMacString|peerSettingsHash|isSettingsDirty)' -> no matches outside System_BondedPeer.*`
  - _Fix:_ Delete the five accessors and their header docs, or wire isSettingsDirty into the bond web UI it was written for before 1.0.

### `System_CLI.cpp` (1)

- **[System_CLI.cpp:163](components/hardwareone/System_CLI.cpp#L163)** `H` - Five 'backward compatibility' help-render wrappers have zero callers
  - renderHelpSystem, renderHelpSettings, renderHelpAutomations, renderHelpEspnow, and renderHelpWifi (System_CLI.cpp:163-186, declared System_CLI.h:53-57) are thin renderHelpModuleByName wrappers explicitly annotated 'kept for backward compatibility' but are never called; the six legacy CLIState aliases CLI_HELP_SYSTEM/WIFI/SENSORS/SETTINGS/AUTOMATIONS/ESPNOW (System_CLI.h:20-25, 'kept for ABI compatibility') are also referenced nowhere. This project needs no backwards compatibility.
  - _Evidence:_ `renderHelpSystem/Settings/Automations/Espnow/Wifi: 0 callers; CLI_HELP_SYSTEM..ESPNOW: 0 refs`
  - _Fix:_ Delete the five wrappers, their header declarations, and the six legacy CLI_HELP_* enum aliases.

### `System_Camera_DVP.cpp` (1)

- **[System_Camera_DVP.cpp:774](components/hardwareone/System_Camera_DVP.cpp#L774)** `H` - setCameraResolution() is dead, superseded by setting+restart
  - setCameraResolution(framesize_t) (~30 lines) is declared in the header but has no caller; cmd_camerares/cmd_cameraframesize instead persist gSettings.cameraFramesize and restart the camera so init re-applies it. Dead under all configs.
  - _Evidence:_ `grep -rn setCameraResolution -> only header decl + this definition; cmd_camerares uses setSetting(cameraFramesize)+cameraPowerRequestRestartSync`
  - _Fix:_ Delete setCameraResolution() and its header declaration.

### `System_Clock.cpp` (1)

- **[System_Clock.cpp:44](components/hardwareone/System_Clock.cpp#L44)** `H` - Half the Clock namespace helpers are unused
  - Clock::formatISO8601Local (44), formatFilenameLocal (56), formatHHMMLocal (66), and the inline ageSeconds/ageMs (System_Clock.h:65,71) have zero call sites; call sites still hand-roll 'millis()-x)/1000' (e.g. G2_Ring.cpp:1715, System_Debug.cpp:2336), so the consolidation these were meant to enable never happened.
  - _Evidence:_ `Clock::formatISO8601Local/formatFilenameLocal/formatHHMMLocal/ageSeconds/ageMs = 0 refs`
  - _Fix:_ Either migrate the hand-rolled age/format sites onto these helpers or delete the five unused ones.

### `System_Debug_Manager.h` (1)

- **[System_Debug_Manager.h:30](components/hardwareone/System_Debug_Manager.h#L30)** `H` - DebugManager compatibility wrapper has dead members
  - The 'compatibility wrapper' singleton's setSystemLogEnabled/isSystemLogEnabled (30-31), setLogCategoryTags/getLogCategoryTags (33-34), initialize (37, comment says never call it), and shutdown (51) methods have zero callers, as do the GET_LOG_LEVEL()/GET_DEBUG_FLAGS() macros (56-57).
  - _Evidence:_ `no DEBUG_MANAGER.setSystemLogEnabled/.../shutdown() calls; GET_LOG_LEVEL()/GET_DEBUG_FLAGS() never used`
  - _Fix:_ Drop the unused DebugManager accessors/macros (or retire the shim, keeping only the ~8 live methods).

### `System_ESPNow_Files.h` (1)

- **[System_ESPNow_Files.h:60](components/hardwareone/System_ESPNow_Files.h#L60)** `H` - FILE_SLOT_FAILED enum state never assigned or tested
  - The FileSlotState enum exposes FILE_SLOT_FAILED = 3 'for diagnostics CLI', but no code ever assigns or compares it - failures are tracked via the internal streamFailed bool and slots go straight to COMPLETING/FREE, so the espnowfiles CLI can never display a FAILED state.
  - _Evidence:_ `rg 'FILE_SLOT_FAILED' -> single hit: the enum definition`
  - _Fix:_ Delete the enum value or actually set it in fileSlotsStreamFailLocked so diagnostics can distinguish failing slots.

### `System_ESPNow_FsList.cpp` (1)

- **[System_ESPNow_FsList.cpp:35](components/hardwareone/System_ESPNow_FsList.cpp#L35)** `H` - Stale extern for v4_send_payload_smart left over from Step 3c migration
  - The 10-line forward declaration + rationale comment for v4_send_payload_smart (lines 29-38) survives although every send in this file now goes through espnowtx::sendAead/sendAeadSync; the symbol is never referenced in the TU. The comment actively misleads by claiming this module still calls it.
  - _Evidence:_ `extern bool v4_send_payload_smart(...); - only other mention in the file is a comment at line 755`
  - _Fix:_ Delete the extern declaration and its comment block.

### `System_ESPNow_Sensors.h` (1)

- **[System_ESPNow_Sensors.h:32](components/hardwareone/System_ESPNow_Sensors.h#L32)** `H` - RemoteSensorStatus struct never instantiated; stale extern in the .cpp
  - struct RemoteSensorStatus (header lines 32-38) is defined but never used anywhere in the repo - status updates flow through updateRemoteSensorStatus's plain parameters instead. In the same module, System_ESPNow_Sensors.cpp:50 declares 'extern void meshSendEnvelopeToPeers(...)' which is never called in that translation unit (and is already declared in System_ESPNow.h:1110).
  - _Evidence:_ `rg 'RemoteSensorStatus' -> only the struct definition; meshSendEnvelopeToPeers never appears in Sensors.cpp after line 50`
  - _Fix:_ Delete the struct and the stale extern declaration.

### `System_ESPNow_Tx.cpp` (1)

- **[System_ESPNow_Tx.cpp:230](components/hardwareone/System_ESPNow_Tx.cpp#L230)** `H` - espnowtx::getStats has no callers and JOB_RAW is never submitted
  - getStats (Tx.cpp:230) is advertised for CLI use ('CLI / other code can also read them via getStats()', Tx.h:177) but nothing calls it - stats are only visible via the task's own 10s debug log. JOB_RAW (Tx.h:82) is handled in runJob (Tx.cpp:84) but no producer ever creates a JOB_RAW job; every submit site uses JOB_AEAD_SMART.
  - _Evidence:_ `rg 'espnowtx::getStats|JOB_RAW' outside System_ESPNow_Tx.* -> zero matches`
  - _Fix:_ Either wire getStats into an espnowtx CLI/diag command or delete it; drop JOB_RAW until a raw-send producer actually migrates.

### `System_EdgeImpulse.cpp` (1)

- **[System_EdgeImpulse.cpp:333](components/hardwareone/System_EdgeImpulse.cpp#L333)** `H` - EdgeImpulse object state-tracking read API is entirely dead
  - setStateChangeCallback(333), getTrackedObjectCount(346), getTrackedObject(350), buildStateChangeJson(358), getLastDetectionResults(1456), isModelLoaded(745) and getLoadedModelPath(749) are defined and header-declared but never called anywhere (verified repo-wide incl. Web.h). Because setStateChangeCallback is never invoked, gStateChangeCallback stays null and the whole tracked-object output path produces nothing consumable.
  - _Evidence:_ `grep -rn shows only the definitions; setStateTrackingEnabled is wired to a setting but no reader of gTrackedObjects exists`
  - _Fix:_ Delete the unused state-tracking accessors/callback and the getLastDetectionResults/isModelLoaded/getLoadedModelPath getters, or wire them to the web/SSE layer.

### `System_FeatureRegistry.cpp` (2)

- **[System_FeatureRegistry.cpp:366](components/hardwareone/System_FeatureRegistry.cpp#L366)** `H` - initFeatureRegistry() (no-op) and getFeaturesByCategory() (undefined) are dead API
  - initFeatureRegistry() (cpp:366) is an empty no-op ('Nothing to init currently') that is never called from anywhere including boot. getFeaturesByCategory() is declared (System_FeatureRegistry.h:49) but has no definition and no caller - a fully stale declaration.
  - _Evidence:_ `initFeatureRegistry: 0 callers, empty body; getFeaturesByCategory: declared h:49, never defined, 0 refs`
  - _Fix:_ Delete initFeatureRegistry() plus the getFeaturesByCategory declaration.
- **[System_FeatureRegistry.cpp:437](components/hardwareone/System_FeatureRegistry.cpp#L437)** `H` - getCategoryHeapEstimate() is defined but never called
  - uint32_t getCategoryHeapEstimate(FeatureCategory) (System_FeatureRegistry.cpp:437, declared .h:54) iterates the registry summing per-category heap but is called from nowhere; the 'features' command uses getEnabledFeaturesHeapEstimate and getTotalPossibleHeapCost instead.
  - _Evidence:_ `getCategoryHeapEstimate: only decl (h:54) + def (cpp:437), 0 callers`
  - _Fix:_ Delete getCategoryHeapEstimate() and its header declaration.

### `System_G2_Protocol.cpp` (1)

- **[System_G2_Protocol.cpp:553](components/hardwareone/System_G2_Protocol.cpp#L553)** `H` - Legacy single-fragment builders and g2statsReset have no callers
  - g2BuildCreateListPage (553) and g2BuildCreateMultiText (1021) are single-fragment envelope wrappers superseded by the *Pb variants + the fragmenting transport in G2_Glasses.cpp; neither is called anywhere (each also carries a 512 B / 1 KB stack buffer). g2statsReset (1300) is likewise defined and declared but never invoked - the g2 stats CLI path only reads via g2statsCount/g2statsAt.
  - _Evidence:_ `size_t g2BuildCreateListPage(...) { uint8_t payload[512]; ... } / void g2statsReset() - zero call sites outside definitions`
  - _Fix:_ Delete the two wrapper functions and g2statsReset along with their header declarations.

### `System_I2C.cpp` (1)

- **[System_I2C.cpp:1212](components/hardwareone/System_I2C.cpp#L1212)** `H` - Dead full-bus scanner scanBusForDevices() (superseded by Smart variant)
  - scanBusForDevices() is a ~55-line bus re-init + full 1..126 address scan with no caller anywhere; the codebase uses scanBusForDevicesSmart() (line 1264) exclusively. The tiny helper wireForBus() (line 1207) it was paired with is likewise only named in a comment (line 1386), never called.
  - _Evidence:_ `static void scanBusForDevices(uint8_t busNumber){...} - only ref is comment; scanBusForDevicesSmart() used at 1387/1396`
  - _Fix:_ Delete scanBusForDevices() and wireForBus(); keep only scanBusForDevicesSmart().

### `System_I2C.h` (1)

- **[System_I2C.h:344](components/hardwareone/System_I2C.h#L344)** `H` - Unused I2C health inline helpers i2cDeviceSuccess/i2cDeviceError/i2cBusRecovery
  - These three inline helpers (344/351/376) have zero callers anywhere; sibling helpers i2cShouldAutoDisable/i2cGetConsecutiveErrors/i2cDeviceIsDegraded in the same block are used, these three never are.
  - _Evidence:_ `inline void i2cDeviceSuccess(u8)/i2cDeviceError(u8); inline bool i2cBusRecovery() - no references outside the header`
  - _Fix:_ Delete the three unused inline helpers.

### `System_I2C_Manager.cpp` (1)

- **[System_I2C_Manager.cpp:150](components/hardwareone/System_I2C_Manager.cpp#L150)** `H` - Unused I2CDeviceManager API: getDeviceByName, classifyI2CError, getActiveDeviceCount
  - getDeviceByName() (cpp:150 / h:217), classifyI2CError() (cpp:645 / h:33) and inline getActiveDeviceCount() (h:219) are declared and defined but called nowhere; getActiveDeviceCount is only named in a disparaging comment at System_I2C.cpp:1455.
  - _Evidence:_ `I2CDevice* getDeviceByName(const char*); I2CErrorType classifyI2CError(uint8_t); int getActiveDeviceCount() const - no callers`
  - _Fix:_ Remove the three unused manager members/functions.

### `System_ImageManager.cpp` (1)

- **[System_ImageManager.cpp:373](components/hardwareone/System_ImageManager.cpp#L373)** `H` - ImageManager::getImage and getImageInfo are unused public API
  - getImage (373, mallocs the whole file) and getImageInfo (410) have no caller in any transport, web handler, or CLI command; gImageManager only ever receives init/listImages/captureAndSave/getStorageStats/deleteImage. isLittleFSAvailable()/isSDAvailable() getters are likewise uncalled.
  - _Evidence:_ `gImageManager.<method> call sites: listImages, captureAndSave, init, getStorageStats, deleteImage only`
  - _Fix:_ Remove getImage/getImageInfo (and the unused isLittleFSAvailable/isSDAvailable getters).

### `System_LLM.cpp` (1)

- **[System_LLM.cpp:293](components/hardwareone/System_LLM.cpp#L293)** `H` - LLMProfile.embed bucket is never written or read
  - The 'embed' field of struct LLMProfile is declared but has no PROF_ADD(embed, ...) writer and is omitted from the profiler dump at lines 2159-2181, unlike every other bucket. It is a dead field on the profiling scaffold.
  - _Evidence:_ `struct has 'int64_t embed, norm, qkv, ...'; grep gProf.embed / PROF_ADD(embed -> zero hits`
  - _Fix:_ Remove the embed field (or add the missing PROF_ADD around the token-embedding copy and print it).

### `System_Maps.cpp` (2)

- **[System_Maps.cpp:201](components/hardwareone/System_Maps.cpp#L201)** `H` - Six public map layer/subtype/highlight API functions have no callers
  - mapLayerIsVisible(201), mapSubtypeIsVisible(179), mapLayersSetVisible(171), mapSubtypeSetMask(196), mapHighlightByName(80), and mapHighlightByType(90) are declared in System_Maps.h but referenced nowhere; layer/subtype visibility now flows through the MapRenderParams snapshot and the highlight system uses only mapHighlightByNameAndType/mapHighlightClear.
  - _Evidence:_ `each symbol appears only at its .cpp definition and .h declaration across all of components/hardwareone + main`
  - _Fix:_ Delete these six superseded accessors from System_Maps.cpp and System_Maps.h.
- **[System_Maps.cpp:1781](components/hardwareone/System_Maps.cpp#L1781)** `H` - OLEDMapRenderer feature-drawing methods are unreachable after OffscreenMapRenderer migration
  - The OLED map now renders features through OffscreenMapRenderer (OLED_Mode_Map.cpp:982-987) and gOledMapRenderer is used only for drawOverlayText/drawContextBar, so MapCore::renderMap is never passed an OLEDMapRenderer. drawLine, drawDashedLine, drawDottedLine, drawPositionMarker, clipToContent, getFeatureStyle, shouldRenderFeature, clear, setViewport, and flush (~lines 1723-1876, 1929-1990) are dead.
  - _Evidence:_ `renderMap only ever receives OffscreenMapRenderer; gOledMapRenderer-> calls are only drawOverlayText/drawContextBar/getHeight`
  - _Fix:_ Remove the unused OLEDMapRenderer draw methods (keep only the text/context-bar overlay methods that are still called).

### `System_MemUtil.h` (1)

- **[System_MemUtil.h:63](components/hardwareone/System_MemUtil.h#L63)** `H` - Unused allocation helpers superseded by ps_alloc family
  - ps_try_malloc/ps_try_calloc/ps_try_realloc (63-101), ps_new/ps_delete (239-251), and the whole getPsramBuffer template + getPsramBuffer1K/2K/4K wrappers (337-363) have no references anywhere; ps_alloc/ps_calloc/ps_realloc and PSRAM_STATIC_BUF are what callers actually use.
  - _Evidence:_ `rg ps_try_*/ps_new/ps_delete/getPsramBuffer across components+main = 0 hits outside System_MemUtil.h`
  - _Fix:_ Remove the ps_try_* trio, ps_new/ps_delete, and the getPsramBuffer template+wrappers.

### `System_MemoryMonitor.cpp` (1)

- **[System_MemoryMonitor.cpp:139](components/hardwareone/System_MemoryMonitor.cpp#L139)** `H` - getAllMemoryRequirements() has no callers
  - The 'for diagnostics' accessor getAllMemoryRequirements(outCount) (139) is never called; checkMemoryAvailable and its internal getMemoryRequirement are the only live parts of the registry.
  - _Evidence:_ `getAllMemoryRequirements: only def (cpp:139) + decl (h:28), 0 callers`
  - _Fix:_ Remove getAllMemoryRequirements and its declaration.

### `System_MeshPeers.cpp` (1)

- **[System_MeshPeers.cpp:14](components/hardwareone/System_MeshPeers.cpp#L14)** `H` - MeshPeers::isReachable/isKnown/countByRoom unused; countByRoom strands a whole call chain
  - Only isHealthy, displayName, countHealthy, and countActive have callers (OLED_ESPNow, System_MQTT); isReachable (14), isKnown (19), and countByRoom (56) are referenced nowhere. countByRoom is explicitly a 'backward compatibility' forwarder (contrary to project no-compat policy) and is the ONLY caller of countMeshPeerMetaByRoom in System_ESPNow.cpp:7388, so removing it kills that function too.
  - _Evidence:_ `System_MeshPeers.h:81 comment: 'Forwards to countMeshPeerMetaByRoom for backward compatibility with existing callers' - yet zero callers exist`
  - _Fix:_ Delete the three functions plus countMeshPeerMetaByRoom (System_ESPNow.cpp:7388 and its System_ESPNow.h:247 declaration).

### `System_NeoPixel.cpp` (1)

- **[System_NeoPixel.cpp:190](components/hardwareone/System_NeoPixel.cpp#L190)** `H` - Three never-called NeoPixel color utilities
  - blendColors(190), adjustBrightness(200) and getClosestColorName(233) are declared in System_NeoPixel.h but never called; getClosestColorName appears only in a commented-out line in i2csensor_apds9960.cpp. rainbowColor is the only util actually used.
  - _Evidence:_ `grep -rn blendColors/adjustBrightness -> definitions only; getClosestColorName -> only '// String colorName = getClosestColorName(...)'`
  - _Fix:_ Delete blendColors, adjustBrightness and getClosestColorName and their header declarations.

### `System_Power.cpp` (2)

- **[System_Power.cpp:43](components/hardwareone/System_Power.cpp#L43)** `H` - Three never-called functions in System_Power
  - getPowerModeCpuFreq(43) and getPowerModeDisplayBrightness(50) are never called (applyPowerMode reads gPowerModes[mode] fields directly), and checkAutoPowerMode(111) is a never-invoked placeholder whose body is entirely commented out - so the whole 'auto power mode' setting is non-functional.
  - _Evidence:_ `grep -rn: getPowerModeCpuFreq / getPowerModeDisplayBrightness / checkAutoPowerMode each match only their definition`
  - _Fix:_ Remove the two unused getters and checkAutoPowerMode (or implement the auto-mode it stubs).
- **[System_Power.cpp:359](components/hardwareone/System_Power.cpp#L359)** `M` - powerDisplayDimLevel setting is write-only (no consumer)
  - gSettings.powerDisplayDimLevel is settable via the 'powerdim' command (cmd_set_powerdim) and persisted, but its value is never read anywhere in the codebase, so the 'Display Dim Level (%)' setting has no runtime effect.
  - _Evidence:_ `grep -rn powerDisplayDimLevel -> only the SettingEntry registration at System_Power.cpp:359 (plus the settings struct def)`
  - _Fix:_ Wire powerDisplayDimLevel into the power-save dimming path or remove the setting.

### `System_R1_Protocol.cpp` (1)

- **[System_R1_Protocol.cpp:209](components/hardwareone/System_R1_Protocol.cpp#L209)** `H` - R1Encoder::buildHeartbeat and buildDeviceInfoQuery never called
  - Both convenience builders (209 and 215) have no callers: the spoof poller uses buildGenericQuery for deviceStatus, and the ringquery CLI reaches deviceInfo only via its raw escape hatch; the header itself notes heartbeat is exposed 'for future use'. Everything else in the R1 encoder/decoder surface is live via G2_Ring.cpp.
  - _Evidence:_ `R1Frame R1Encoder::buildHeartbeat() / buildDeviceInfoQuery() - referenced only in System_R1_Protocol.{h,cpp}`
  - _Fix:_ Delete both builders (buildGenericQuery already covers these opcodes) or wire them as ringquery subjects.

### `System_SetupWizardMode.cpp` (1)

- **[System_SetupWizardMode.cpp:960](components/hardwareone/System_SetupWizardMode.cpp#L960)** `H` - setupWizardMode_isActive() has no callers
  - This public diagnostic helper is declared in the header and forward-declared internally but is never called anywhere; its own header comment notes the framework itself uses cliCurrentMode() for the check. Only the definition, forward decl, and prototype appear in a repo-wide search.
  - _Evidence:_ `bool setupWizardMode_isActive() { return cliCurrentMode() == &kWizardMode; } // no callers`
  - _Fix:_ Remove setupWizardMode_isActive() and its declaration in System_SetupWizardMode.h.

### `System_User.cpp` (2)

- **[System_User.cpp:561](components/hardwareone/System_User.cpp#L561)** `H` - isTransportAdmin() is defined but never called
  - bool isTransportAdmin(CommandSource) (System_User.cpp:561, declared System_User.h:89) has no caller anywhere in the tree - only its declaration and definition exist. Its sibling accessors getTransportUser/isTransportAuthenticated are heavily used, so this one was simply left behind.
  - _Evidence:_ `isTransportAdmin: only decl (System_User.h:89) + def (System_User.cpp:561), 0 callers`
  - _Fix:_ Delete isTransportAdmin() and its header declaration.
- **[System_User.cpp:2594](components/hardwareone/System_User.cpp#L2594)** `H` - usernameExistsInUsersJson() is defined but never called
  - bool usernameExistsInUsersJson(const String&, const String&) (System_User.cpp:2594, declared System_User.h:151) has no callers - a substring-scan helper that nothing invokes. Username-existence checks elsewhere go through the parsed-JSON path instead.
  - _Evidence:_ `usernameExistsInUsersJson: only decl (h:151) + def (cpp:2594), 0 callers`
  - _Fix:_ Delete usernameExistsInUsersJson() and its header declaration.

### `System_Utils.h` (1)

- **[System_Utils.h:433](components/hardwareone/System_Utils.h#L433)** `H` - Unused MAC-format helpers and backward-compat aliases
  - macToDisplayStr (433), macToPathTokenStr (446), macEquals (453), and the alias formatMacAddrStr (506) have no callers; formatMacAddr (503) is an explicitly 'backward-compatible' alias with a single caller (WebPage_Bond.cpp:981) despite the project needing no backward compat.
  - _Evidence:_ `macToDisplayStr/macToPathTokenStr/macEquals/formatMacAddrStr = 0 refs; formatMacAddr = 1 alias call`
  - _Fix:_ Delete the unused MAC helpers and both aliases, pointing the one formatMacAddr caller at macToDisplay.

### `System_WiFi.cpp` (1)

- **[System_WiFi.cpp:456](components/hardwareone/System_WiFi.cpp#L456)** `H` - cmd_wifigettxpower defined but never wired (getter orphaned)
  - cmd_wifigettxpower() reads TX power and is declared in System_WiFi.h, but the wifiCommands table binds the "wifigettxpower" command string to cmd_wifitxpower (the setter) instead, so this getter has zero callers repo-wide. It is dead and its name even mismatches the behavior actually registered.
  - _Evidence:_ `table: {"wifigettxpower", ... cmd_wifitxpower ...}; cmd_wifigettxpower has only its decl+def, no references`
  - _Fix:_ Delete cmd_wifigettxpower and its header declaration (or wire it if a real getter command is intended).

### `WebPage_LoginRequired.h` (1)

- **[WebPage_LoginRequired.h:11](components/hardwareone/WebPage_LoginRequired.h#L11)** `H` - WebPage_LoginRequired.h and WebPage_LoginSuccess.h streamers are never called
  - streamAuthRequiredInner (this header's only content) and streamLoginSuccessContent (WebPage_LoginSuccess.h:14, only content) have zero call sites; sendAuthRequiredResponse and handleLoginSetSession build their own responses inline in WebServer_Server.cpp. Both headers (~170 lines) are fossils of the old login flow, still #included by HardwareOne.cpp and WebServer_Server.cpp.
  - _Evidence:_ `rg 'streamAuthRequired|streamLoginSuccess' -> only the two definitions; no callers`
  - _Fix:_ Delete both headers and their #include lines in HardwareOne.cpp:49-50 and WebServer_Server.cpp:47.

### `WebServer_Server.cpp` (5)

- **[WebServer_Server.cpp:125](components/hardwareone/WebServer_Server.cpp#L125)** `H` - Dead SSE-era helpers: broadcastNoticeToAllSessions, sendSSEBurstToSession, sseSendLogs, sseDebug, sseHeartbeat
  - broadcastNoticeToAllSessions (L125), sendSSEBurstToSession (L167) and sseSendLogs (L1262) are defined here with zero callers; sseDebug and sseHeartbeat (WebServer_Events.cpp:15/61) are likewise uncalled but still declared in WebServer_Server.h:356-357. All are leftovers from the pre-/api/events notification plumbing.
  - _Evidence:_ `rg '\bbroadcastNoticeToAllSessions\b|\bsendSSEBurstToSession\b|\bsseSendLogs\b|sseDebug\(|sseHeartbeat\(' -> definitions/declarations only`
  - _Fix:_ Delete the five functions and their declarations in WebServer_Server.h.
- **[WebServer_Server.cpp:1262](components/hardwareone/WebServer_Server.cpp#L1262)** `H` - sseSendLogs and several web utils have no callers
  - sseSendLogs (with its per-line substring churn) is never called anywhere in the tree; likewise streamNav (WebServer_Utils.cpp:614, which also passes generateNavigation's args swapped), getHeaderValue/getCookieValue (WebServer_Utils.cpp:232/246, which BROADCAST-log cookie values), and WebMirrorBuf::snapshot() are declaration-only leftovers.
  - _Evidence:_ `grep -rn sseSendLogs -> only the definition (WebServer_Server.cpp:1262) and declaration (WebServer_Server.h:354); same for streamNav/getHeaderValue/getCookieValue/snapshot()`
  - _Fix:_ Delete these functions (and their header declarations) before 1.0 to shrink the TU and remove the swapped-arg streamNav trap.
- **[WebServer_Server.cpp:1355](components/hardwareone/WebServer_Server.cpp#L1355)** `H` - sendAuthRequired and redirectToLogin are never called
  - Both functions (L1355 and L1363) have zero call sites - the 401/redirect flow goes through sendAuthRequiredResponse (L3421) instead. Confirmed present-but-unreferenced in the built object.
  - _Evidence:_ `rg '\bsendAuthRequired\b|\bredirectToLogin\b' -> only the two definitions (plus redirectToLogin's own debug log)`
  - _Fix:_ Delete both functions.
- **[WebServer_Server.cpp:3452](components/hardwareone/WebServer_Server.cpp#L3452)** `H` - /login/setsession route is a dead two-step-login fossil
  - handleLoginSetSession gates on gSessUser being non-empty, but gSessUser is never assigned a non-empty value anywhere (only defined at line 180 and cleared at 3465), so the registered /login/setsession route can only ever redirect back to /login. The current login flow calls setSession()/authSuccessUnified() directly from handleLogin.
  - _Evidence:_ `if (gSessUser.length() == 0) { ...redirect /login... } - only writes are `String gSessUser;` (L180) and `gSessUser = ""` (L3465)`
  - _Fix:_ Delete handleLoginSetSession, the gSessUser global, and the /login/setsession registration (line 5035) plus its WEB_API_INVENTORY.md entry.
- **[WebServer_Server.cpp:4413](components/hardwareone/WebServer_Server.cpp#L4413)** `M` - Six registered API endpoints have zero consumers (admin trio, notice, output x2, files/create)
  - /api/admin/pending, /api/admin/approve, /api/admin/reject, /api/notice, /api/output, /api/output/temp, and /api/files/create are registered but referenced by no HTML/JS anywhere; the settings UI does these actions via CLI commands over /api/cli, and /api/notice was superseded by the /api/events SSE. The project's own docs/WEB_PAGE_API_MAP.md already lists all of them as 'remove candidate' - ~350 lines of dead handler code and needless pre-1.0 attack surface.
  - _Evidence:_ `rg 'admin/pending|admin/approve|admin/reject|api/output|api/notice' hits only the httpd_uri_t registrations; WEB_PAGE_API_MAP.md: 'remove candidate'`
  - _Fix:_ Delete these handlers and registrations (or intentionally keep and document an external-client contract) before 1.0.

### `WebServer_Utils.cpp` (2)

- **[WebServer_Utils.cpp:52](components/hardwareone/WebServer_Utils.cpp#L52)** `H` - Four unused WebMirrorBuf methods, including a wholesale copy of appendDirect's trim loop
  - All live traffic uses gWebMirror.appendDirect/init/clear/snapshotTo; append(const String&,bool) (L52), append(const char*,bool) (L96), snapshot() (L164) and assignFrom() (L145) have zero call sites. append(const String&) duplicates appendDirect's ~40-line overflow/drop-oldest logic line-for-line, so any ring-buffer fix must currently be made twice.
  - _Evidence:_ `rg 'gWebMirror\.' -> only .buf/.init/.clear/.appendDirect/.snapshotTo; append/snapshot/assignFrom unreferenced`
  - _Fix:_ Delete the four methods (and their !ENABLE_HTTP_SERVER stubs at WebServer_Utils.h:1523-1528).
- **[WebServer_Utils.cpp:614](components/hardwareone/WebServer_Utils.cpp#L614)** `H` - Five dead public functions in WebServer_Utils.cpp (one with a latent swapped-args bug)
  - streamNav (L614), streamContentGeneric (L622), streamChunkBuf (L439), getHeaderValue (L232) and getCookieValue (L246) have zero callers repo-wide (each declared in both WebServer_Utils.h and WebServer_Server.h). streamNav additionally calls generateNavigation(username, activePage) with the arguments in the wrong order - a bug waiting for whoever revives it - and getHeaderValue/getCookieValue log header/cookie values via BROADCAST_PRINTF.
  - _Evidence:_ `streamNav: generateNavigation(username, activePage) vs signature (activePage, username); rg finds no call sites for any of the five`
  - _Fix:_ Delete all five functions and their duplicate declarations in both headers.

### `WebServer_Utils.h` (1)

- **[WebServer_Utils.h:121](components/hardwareone/WebServer_Utils.h#L121)** `H` - renderTwoFieldForm is an unused 50-line inline HTML builder
  - renderTwoFieldForm is defined inline in this widely-included header but has zero call sites anywhere in components/hardwareone or main; the login/register/password forms all stream their own markup.
  - _Evidence:_ `rg '\brenderTwoFieldForm\b' -> single occurrence (its definition)`
  - _Fix:_ Delete renderTwoFieldForm.

### `i2csensor_pa1010d.cpp` (1)

- **[i2csensor_pa1010d.cpp:574](components/hardwareone/i2csensor_pa1010d.cpp#L574)** `H` - Unused GPS accessor functions (hasGPSFix + 5 getGPS*)
  - hasGPSFix() (574), getGPSLatitude/Longitude/Altitude/Speed/Satellites (579-603) are defined but not declared in the header and called nowhere; OLED_Mode_Map.cpp only has a local var also named hasGPSFix. Data is consumed via gpsCacheSnapshot()/gpsBuildDataJSON() instead.
  - _Evidence:_ `float getGPSLatitude(){...} etc. - no callers; map uses local `bool hasGPSFix` not the function`
  - _Fix:_ Delete the six unused GPS accessor functions.

### `i2csensor_seesaw.cpp` (1)

- **[i2csensor_seesaw.cpp:698](components/hardwareone/i2csensor_seesaw.cpp#L698)** `H` - Obsolete gamepadGetX/Y/Buttons() replaced by HAL_Input
  - gamepadGetX (698), gamepadGetY (703), gamepadGetButtons (708) plus their decls in i2csensor_seesaw.h (95-97) have no callers; HAL_Input.cpp explicitly documents them as replaced by inputGetX/Y/Buttons (HAL_Input.cpp:199 comment). The only mentions are those comments.
  - _Evidence:_ `int gamepadGetX()/gamepadGetY(); uint32_t gamepadGetButtons(); - 'previous gamepadGetX/Y/Buttons functions ... are' (HAL comment)`
  - _Fix:_ Delete the three gamepadGet* functions and their header declarations; HAL_Input is the live path.

### `i2csensor_sths34pf80_web.h` (1)

- **[i2csensor_sths34pf80_web.h:15](components/hardwareone/i2csensor_sths34pf80_web.h#L15)** `H` - Orphaned old-pattern presence web functions (getPresenceDataJson/WebCard/WebScript)
  - These three inline functions (getPresenceDataJson:15, getPresenceWebCard:38 with a full HTML blob, getPresenceWebScript:71 with a full JS blob) are never called; the live page uses the streamSTHS34PF80Presence* functions in the same header. They are a leftover of the pre-stream web pattern and are the only sensor web header still carrying it.
  - _Evidence:_ `inline const char* getPresenceWebCard()/getPresenceWebScript()/getPresenceDataJson() - zero references repo-wide`
  - _Fix:_ Delete the getPresence* trio; only the streamSTHS34PF80Presence* functions are used.

### `icons_embedded.cpp` (1)

- **[icons_embedded.cpp:1](components/hardwareone/icons_embedded.cpp#L1)** `H` - Entire icons_embedded.cpp is an orphaned stale duplicate of System_Icons.cpp
  - This 6313-line auto-generated file is not listed in CMakeLists.txt (only System_Icons.cpp.obj is built), includes a nonexistent header icons_embedded.h, and holds an older 87-icon table vs System_Icons.cpp's 105 (missing camera, mic, mqtt, web, etc.). It is a fully dead superseded copy that cannot even compile.
  - _Evidence:_ `#include "icons_embedded.h" (no such file); EMBEDDED_ICONS_COUNT = 87; not in CMakeLists hardwareone_srcs`
  - _Fix:_ Delete icons_embedded.cpp and reconcile the two generator scripts to emit only System_Icons.cpp.

### `System_ESPNow.h` (1)

- **[System_ESPNow.h:53](components/hardwareone/System_ESPNow.h#L53)** `H` - 16 of 17 V2-era MSG_TYPE_*/PAYLOAD_* string macros are unreferenced
  - Of the MSG_TYPE_* (h:53-63) and PAYLOAD_* (h:66-71) JSON message-type macros, only MSG_TYPE_BOOT has a single use (buildBootNotification, cpp:7610); the other 16 (HB/ACK/MESH_SYS/RESPONSE/STREAM/FILE/CMD/TEXT/USER_SYNC/FILE_BROWSE, cmd/topoReq/topoResp/query/status/timeSync) are fossils of the removed V2 JSON envelope protocol.
  - _Evidence:_ `each macro occurs exactly once (its own #define); only MSG_TYPE_BOOT has a second hit at System_ESPNow.cpp:7610`
  - _Fix:_ Delete the 16 unused macros (and consider inlining "BOOT" at the one v2_init_envelope call).

### `System_ESPNow_Identity.cpp` (1)

- **[System_ESPNow_Identity.cpp:428](components/hardwareone/System_ESPNow_Identity.cpp#L428)** `H` - v1 peer-identity file compat shim contradicts no-backwards-compat policy
  - readPeerIdentityFile accepts schema version 1 files ('v1 files are upgraded on load', line 297-300) and defaults their subscribedEvents to ALL, but this project explicitly never needs migration code - devices are fully erased before flashing, so v1 files cannot exist. Same for ESPNOW_EVT_WORKER_STATUS in System_ESPNow_Identity.h:115, a bit kept only 'for bitmap stability' of a retired opcode.
  - _Evidence:_ `if (ver != 1 && ver != kPeerFileVersion) return false;  // plus header comment 'v1 files are upgraded on load'`
  - _Fix:_ Accept only kPeerFileVersion and drop the v1 default/upgrade comments; reclaim or delete the retired WORKER_STATUS bit.

### `System_SelfDevice.cpp` (1)

- **[System_SelfDevice.cpp:67](components/hardwareone/System_SelfDevice.cpp#L67)** `M` - SelfDevice facade accessors deviceName/uptimeMs/dramFreeBytes have no callers
  - SelfDevice::deviceName() (cpp:67), SelfDevice::uptimeMs() (inline h:40), and SelfDevice::dramFreeBytes() (cpp:28) have zero callers across the tree, even though the namespace is billed as the single source of truth for these reads. They are facade methods that no consumer ever adopted.
  - _Evidence:_ `SelfDevice::deviceName / uptimeMs / dramFreeBytes: 0 external refs each`
  - _Fix:_ Remove the three unused accessors, or wire the existing open-coded call sites to them if the facade consolidation is still intended.

### `System_SensorRegistry.cpp` (1)

- **[System_SensorRegistry.cpp:97](components/hardwareone/System_SensorRegistry.cpp#L97)** `H` - Unused registry API findNonI2CSensor() and initSensorRegistry()
  - findNonI2CSensor() (97) and initSensorRegistry() (107, body only comments 'Future: could validate') are declared in the header but invoked nowhere; the nonI2CSensors[] table is iterated directly in System_I2C.cpp, and no code calls either function.
  - _Evidence:_ `const NonI2CSensorEntry* findNonI2CSensor(const char*); void initSensorRegistry(); - no callers repo-wide`
  - _Fix:_ Delete both functions (and their declarations) or wire initSensorRegistry into setup if intended.

### `System_AuthIdentity.h` (1)

- **[System_AuthIdentity.h:6](components/hardwareone/System_AuthIdentity.h#L6)** `M` - Stale header comment claims removed legacy identity globals still exist
  - The file-top comment states 'During migration the legacy globals still exist and ExecIdentityGuard keeps them in sync', but gExecAuthContext/gExecUser/gExecIsAdmin are fully removed (only referenced in comments) and ExecIdentityGuard syncs nothing. The comment describes a finished migration as in-progress and misleads readers about a sync that no longer happens.
  - _Evidence:_ `'the legacy globals still exist and ExecIdentityGuard keeps them in sync' - globals gone, no sync code`
  - _Fix:_ Delete the migration-in-progress paragraph so the header describes only the current TLS-only design.

### `System_Filesystem.h` (1)

- **[System_Filesystem.h:60](components/hardwareone/System_Filesystem.h#L60)** `M` - loadAndIncrementBootSeq declared in two headers
  - loadAndIncrementBootSeq() is declared in System_Filesystem.h (60) and again in System_User.h (264) and defined in System_User.cpp; the Filesystem.h declaration is a stray extern for a symbol this module doesn't own.
  - _Evidence:_ `decl in System_Filesystem.h:60 and System_User.h:264; definition only in System_User.cpp:3188`
  - _Fix:_ Remove the duplicate declaration from System_Filesystem.h and include System_User.h where needed.

## 3. Duplication & shared-function opportunities

_40 findings - the "things that could be shared functions, or are only used by part of the system" bucket. `duplication` = the same logic re-implemented in 2+ places; `helper-bypass` = a shared helper exists but some call sites hand-roll it instead._

### 3a. Duplicated logic (extract to one shared home)

- **[OLED_Mode_System.cpp:64](components/hardwareone/OLED_Mode_System.cpp#L64)** `H`, _per-frame_ - Uptime h/m/s decomposition duplicated across surfaces
  - The seconds->hours/minutes(/seconds) decomposition and "%luh %lum %lus" style are re-derived independently on OLED, G2, and CLI/JSON with no shared formatter. Neither Clock:: nor System_Utils exposes a formatUptime helper, so each surface rolls its own.
  - _Evidence:_ `hours=s/3600; min=(s%3600)/60 at OLED_Mode_System.cpp:64,152,415; OLED_Mode_Network.cpp:784; G2_Glasses.cpp:3311; System_Utils.cpp:1538,1766`
  - _Fix:_ Add one shared formatUptime(seconds, buf) (Clock:: or System_Utils) and call it from every surface.
- **[i2csensor_ds3231.cpp:38](components/hardwareone/i2csensor_ds3231.cpp#L38)** `H`, _per-packet_ - Six identical per-sensor *ResolveBus() helpers
  - rtcResolveBus, presenceResolveBus, gamepadResolveBus, fmRadioResolveBus, anoResolveBus and fuelGaugeResolveBus are byte-for-byte identical except the gSettings.<x>Bus field read. Each does i2c()->getWire(bus) null-checks with the same body.
  - _Evidence:_ `static bool rtcResolveBus(u8*,TwoWire**){ bus=gSettings.rtcBus; w=i2c()?i2c()->getWire(bus):null; ... } x6`
  - _Fix:_ Add one shared resolveSensorBus(uint8_t busSetting, uint8_t* outBus, TwoWire** outWire) and call it from each driver.
- **[i2csensor_ds3231.cpp:895](components/hardwareone/i2csensor_ds3231.cpp#L895)** `H`, _per-command_ - Eight near-identical cmd_*autostart command handlers with drift
  - cmd_rtcautostart/gpsautostart/imuautostart/thermalautostart/tofautostart/fmradioautostart/apdsautostart/presenceautostart all repeat the same on/off/query on a gSettings.<x>AutoStart bool. They have already drifted: rtc uses normalizeCliArg() while gps uses arg.trim()+toLowerCase().
  - _Evidence:_ `cmd_rtcautostart: setSetting(gSettings.rtcAutoStart,...) vs cmd_gpsautostart: setSetting(gSettings.gpsAutoStart,...) - same body, different normalize`
  - _Fix:_ Extract cmdBoolAutoStart(argsInput, &settingRef, "[RTC]") shared helper and delegate all eight to it.
- **[WebPage_ESPNow.h:748](components/hardwareone/WebPage_ESPNow.h#L748)** `M`, _per-frame_ - RSSI-to-signal-quality mapping re-invented per surface
  - The dBm-to-quality/color/bars derivation is re-implemented on each surface with inconsistent thresholds (web ESP-NOW -60/-75, OLED bars -50/-70, bond page (rssi+90)/15), and the web copy itself appears twice. No shared rssiToQuality helper exists, so the surfaces disagree on what counts as a good signal.
  - _Evidence:_ `RSSI->quality diverges: WebPage_ESPNow.h:748,2407 (-60/-75); OLED_Mode_Network.cpp:584 (-50/-70); WebPage_Bond.cpp:171 ((rssi+90)/15)`
  - _Fix:_ Add a shared rssiToQuality()/rssiBars() helper so all surfaces classify signal strength with identical thresholds.
- **[G2_Glasses.cpp:7441](components/hardwareone/G2_Glasses.cpp#L7441)** `H`, _per-event_ - Unnamed rebuild helpers duplicate their Named variants verbatim
  - sendRebuildTextAndWait (7441) is byte-identical to sendRebuildTextNamedAndWait (7481) except CONTAINER_NAME is hardcoded, and sendRebuildListAndWait (7260) repeats the same alloc/build/cap-check/send/ack flow as its named twin. Each unnamed variant could be a one-line delegation, removing ~120 lines.
  - _Evidence:_ `Both call g2BuildRebuildTextPb with identical args except CONTAINER_NAME vs containerName param`
  - _Fix:_ Make sendRebuildTextAndWait/sendRebuildListAndWait one-line wrappers that pass CONTAINER_NAME to the named variants.
- **[G2_Glasses.cpp:15688](components/hardwareone/G2_Glasses.cpp#L15688)** `H`, _per-event_ - BMP and JPG viewer workers plus 4 spawners are copy-paste clones
  - g2JpgViewerWorker (15688) is line-for-line g2BmpViewerWorker (13951) with only the loader call (loadJpgAsBmp288x144 vs readBmpFromVfs) and log tag changed, and the four spawn functions g2ShowBmpFile/g2ShowJpgFile/g2ShowBmpFileFullScreen/g2ShowJpgFileFullScreen repeat identical heap-guard + strdup + xTaskCreate + cleanup boilerplate. One worker parameterized by a loader function pointer (and a fullscreen flag) would replace ~250 duplicated lines.
  - _Evidence:_ `g2JpgViewerWorker body == g2BmpViewerWorker body modulo load fn; comment at 15770: 'mirror of g2BmpFullViewerWorker'`
  - _Fix:_ Factor a single viewer worker taking a load-to-288x144-BMP callback and share one spawn helper.
- **[G2_Page_ESPNow.cpp:211](components/hardwareone/G2_Page_ESPNow.cpp#L211)** `H`, _per-event_ - RedrawSpec+LensUiJob enqueue boilerplate copied three times
  - enqueueRedrawFromCallback (G2_Page_ESPNow.cpp:211) is a verbatim copy of enqueueWifiRedrawFromCallback (G2_Page_Network.cpp:938) - its own comment says "mirrors Network's helper" - and g2ESPNowAppOnRxText (G2_Page_ESPNow.cpp:1279) inlines the same ~25-line alloc/stamp/enqueue/cleanup sequence a third time. Any change to LensUiJob stamping or error cleanup must now be made in three places.
  - _Evidence:_ `// Lens redraw from cmd_exec_task - mirrors Network's helper ... RedrawSpec* spec = new (std::nothrow) RedrawSpec{};`
  - _Fix:_ Hoist one shared g2EnqueueRedraw(cookie, renderFn, tag) helper next to g2EnqueueLensJob (G2_HijackCmd or G2_Glasses) and call it from all three sites.
- **[G2_Ring.cpp:606](components/hardwareone/G2_Ring.cpp#L606)** `H`, _per-event_ - Ring advert-name classifier duplicated from G2_Glasses.cpp
  - RingScanCallbacks::onResult inlines the EVEN R1_XXXXXX name matcher (lines 606-622) and its own comment admits it is an "Inline mirror of classifyRingName() in G2_Glasses.cpp" (static at G2_Glasses.cpp:1043). If the accepted advert format ever changes, the two parsers can silently diverge and the dedicated ring scan would disagree with the shared G2 scan.
  - _Evidence:_ `// Inline mirror of classifyRingName() in G2_Glasses.cpp: /^EVEN\s+R1_[0-9A-F]{6}$/i`
  - _Fix:_ Export classifyRingName from G2_Glasses.h (drop the static) and call it from RingScanCallbacks::onResult.
- **[HardwareOne.cpp:1908](components/hardwareone/HardwareOne.cpp#L1908)** `H`, _per-event_ - Boot notification broadcast twice on every boot
  - initEspNow() already builds and sends the boot notice at its tail (System_ESPNow.cpp:9155-9157), and HardwareOne.cpp's setup repeats the identical buildBootNotification+meshSendBootToPeers block right after init returns; both fire with different msgIds, so RX dedup does not collapse them and peers record two MSG_SYSTEM_EVENT boot notices per reboot. The HardwareOne copy is the redundant one (manual `openespnow` re-init only goes through initEspNow).
  - _Evidence:_ `HardwareOne.cpp:1901-1910 buildBootNotification+meshSendBootToPeers duplicates System_ESPNow.cpp:9155-9156 which just ran inside cmd_espnow_init`
  - _Fix:_ Delete the HardwareOne.cpp block (lines ~1899-1911) and keep the single send inside initEspNow().
- **[System_ESPNow_Handlers_Crypto.cpp:1130](components/hardwareone/System_ESPNow_Handlers_Crypto.cpp#L1130)** `H`, _per-event_ - Rekey key derivation duplicates sessionDeriveAeadKeys' KDF core
  - deriveRekeyedKeysFromShared re-implements the exact subkey-id/context/direction logic of sessionDeriveAeadKeys (System_ESPNow_Sessions.cpp:108) - KDF ids 1/2, contexts "esp-AtoB"/"esp-BtoA", myDirection swap - as its own comment admits ('Mirrors sessionDeriveAeadKeys but writes into caller-provided buffers'). This is a security-critical invariant maintained in two files; a change to one silently breaks interop with the other.
  - _Evidence:_ `espnowCryptoKdfSubkey(kAtoB, shared, 1, "esp-AtoB") ... same constants + direction branch as Sessions.cpp:111-125`
  - _Fix:_ Add a buffer-output sessionDeriveDirectionalKeys(shared, myDirection, outTx, outRx) in Sessions and have both call it.
- **[System_FileManager.cpp:168](components/hardwareone/System_FileManager.cpp#L168)** `H`, _per-event_ - getItem scan-fallback re-implements loadDirectory's per-entry loop
  - The >64-entry fallback in getItem (168-233) repeats the exact effective-path prefix strip, nested-path skip, and hidden-file skip logic already in loadDirectory (433-480). The two copies must be kept in sync by hand.
  - _Evidence:_ `stripSdPrefix + snprintf(prefix) + startsWith(prefix) + indexOf('/')!=-1 + startsWith(".") blocks in both getItem and loadDirectory`
  - _Fix:_ Extract the entry-name normalization + filter into one shared helper both methods call.
- **[System_Filesystem.cpp:310](components/hardwareone/System_Filesystem.cpp#L310)** `H`, _per-request_ - Directory-entry name normalization copied across 3 in-scope sites
  - The 'strip effective-dir prefix, skip nested paths, skip hidden' entry loop in buildFilesListing (309-324) is a fourth copy of the same logic in FileManager::loadDirectory and FileManager::getItem (and again in System_ESPNow_FsList.cpp). Behavior drift between listing surfaces is a live risk.
  - _Evidence:_ `'Skip nested paths' + prefix-strip appears at Filesystem.cpp:313-324, FileManager.cpp:184-207 & 441-461`
  - _Fix:_ Centralize directory-entry name extraction/filtering into one VFS helper used by every browser/listing path.
- **[G2_Page_Network.cpp:1122](components/hardwareone/G2_Page_Network.cpp#L1122)** `M`, _per-event_ - WiFi pending-watchdog spawn block duplicated at two tap sites
  - The guarded gWifiPendingTaskActive + xTaskCreate(wifiPendingWatchdogTask, "g2_wifi_pending", ...) block appears verbatim in the WiFi toggle handler (1120-1131) and the saved-network connect handler (1281-1288), with slightly different failure cleanup (the second copy silently drops the showWiFiMenu fallback). It is also a per-UI-action task spawn, which project guidance says to avoid where a persistent mechanism exists.
  - _Evidence:_ `if (!gWifiPendingTaskActive) { gWifiPendingTaskActive = true; if (xTaskCreate(wifiPendingWatchdogTask, "g2_wifi_pending", ... ) != pdPASS) {...} }`
  - _Fix:_ Extract a single armWifiPendingWatchdog() helper (one failure-cleanup policy), or replace the watchdog task with a WiFi event callback as the code comment already proposes.
- **[System_ESPNow_FsList.cpp:180](components/hardwareone/System_ESPNow_FsList.cpp#L180)** `M`, _per-request_ - fsList/fsStat/fsGet send-request functions are ~90% identical
  - fsListSendRequest (180), fsStatSendRequest (214), and fsGetSendRequest (245) repeat the same ~30-line body - null-checks, fsListInit, lock, allocSlotAndReqIdLocked, fill req struct, sendAeadSync, roll-back-slot-on-failure, log - differing only in op tag, payload struct, and opcode. A fourth peer-FS op would mean a fourth copy of the rollback/logging logic.
  - _Evidence:_ `Three near-identical blocks each ending: if (!sent) { sPending[slot].reqId = 0; WARN_ESPNOWF("[FSLIST] ... send failed ..."); return 0; }`
  - _Fix:_ Factor a sendFsRequest(op, opcode, reqBytes, reqLen, cbUnion) helper the three thin wrappers call.
- **[System_Filesystem.cpp:340](components/hardwareone/System_Filesystem.cpp#L340)** `M`, _per-request_ - buildFilesListing counts subdirectory children in three near-identical loops
  - The openNextFile child-count loop is written three times: for virtual mount entries (283-288), the JSON folder branch (346-354) and the text folder branch (400-408). All three do the same open/iterate/close to produce an item count.
  - _Evidence:_ `three copies of 'File child = subDir.openNextFile(); while(child){count++; child=...}' in one function`
  - _Fix:_ Add a small countDirEntries(path, ctx) helper and call it from all three sites.
- **[System_Filesystem.cpp:1362](components/hardwareone/System_Filesystem.cpp#L1362)** `M`, _per-request_ - normalizeFsPath and VFS::normalize overlap; guarded paths get normalized twice
  - normalizeFsPath (1362) collapses // and strips trailing slash, then VFS::*Guarded dispatches to VFS::open which calls VFS::normalize (System_VFS.cpp:318) doing the same collapse/strip again. The two normalizers have subtly different rules (leading-slash, /sd, '..' handling), which is exactly the drift risk of parallel helpers.
  - _Evidence:_ `guarded call chain: normalizeFsPath(in,out) -> VFS::open -> VFS::normalize(path) - double normalization`
  - _Fix:_ Converge on one canonicalizer so guarded ops normalize once.
- **[System_VFS.cpp:381](components/hardwareone/System_VFS.cpp#L381)** `M`, _per-request_ - '..' traversal rejection duplicated in six VFS ops
  - exists(381), open(396), mkdir(411), remove(435), rename(458) and rmdir(482) each inline `if (p.indexOf("..") >= 0) return ...` after calling normalize(); the same check also runs earlier in normalizeFsPath for guarded callers. It is a copy-paste guard that should live inside normalize().
  - _Evidence:_ `6x `if (p.indexOf("..") >= 0) return` across VFS::exists/open/mkdir/remove/rename/rmdir`
  - _Fix:_ Fold the traversal rejection into VFS::normalize (or a single validated-normalize helper) so each op doesn't re-check.
- **[WebServer_Server.cpp:3823](components/hardwareone/WebServer_Server.cpp#L3823)** `M`, _per-request_ - handleFileView (~430 lines) parallels handleFileRead as a second file-content endpoint
  - /api/files/read (handleFileRead, L1607) and /api/files/view (handleFileView, L3823) both serve file contents with duplicated auth/pollPause/query-parse/stream scaffolding, and the UI uses them inconsistently (Files editor uses read, open-in-tab and Logging/Battery use view); docs/WEB_PAGE_API_MAP.md flags this dual-endpoint pair as the main structural duplication. handleFileView also hand-rolls percent-decoding in a char loop (L3854-3862) instead of the shared urlDecode() used ~10 other places in this file.
  - _Evidence:_ `Both handlers open with identical pollPause/tgRequireAuth/httpd_req_get_url_query_str/name-param prologue; L3857 `if (path[i] == '%' ...)` vs shared urlDecode()`
  - _Fix:_ Fold the two into one endpoint (view's mode=raw covers read's use case) or extract the shared prologue/streaming into one helper, and use urlDecode().
- **[System_TaskUtils.cpp:516](components/hardwareone/System_TaskUtils.cpp#L516)** `H` - Known-task inventory duplicated across two files
  - reportAllTaskStacks() (System_TaskUtils.cpp:516) and sampleMemoryState() (System_MemoryMonitor.cpp:264) each hand-maintain a near-identical {name, handle, stackWords} table plus a parallel taskAlive[] built from the same gXxxEnabled flags; the two lists must be kept in sync by hand.
  - _Evidence:_ `KnownTask knownTasks[] vs TaskEntry tasks[] - same espnow/cmd_exec/sensor task rows + taskAlive[]`
  - _Fix:_ Extract one shared task-inventory table (name/handle-getter/stackWords/alive) and consume it from both reporters.
- **[WebServer_Server.h:367](components/hardwareone/WebServer_Server.h#L367)** `H` - Stale and triplicated declarations of the web streaming/cookie helpers
  - WebServer_Server.h:367 declares a 3-param streamBeginHtml(String title) that matches no definition (the real one is the 5-param const-char* version in WebServer_Utils.h:1218), and re-declares ~10 other WebServer_Utils functions (getCookieSID/getHeaderValue/getCookieValue/makeSessToken/getClientIP/streamChunk*/streamNav/streamContentGeneric) alongside two declarations with no definition at all (handleFileDelete L324, handleMicRecordingDelete L342). Six more files bypass both headers with ad-hoc `extern void streamBeginHtml(...)` copies (WebPage_Bond.cpp:33, WebPage_Sensors.cpp:59 + WebPage_Sensors.h:18/80 twice, WebPage_MQTT.cpp:19, WebPage_LLM.cpp:34, WebServer_Server.cpp:1114).
  - _Evidence:_ `WebServer_Server.h:367 `streamBeginHtml(httpd_req_t*, const String&, const String& = "")` - no such definition exists; 6 files carry private externs of the 5-param version`
  - _Fix:_ Make WebServer_Utils.h the single home for these declarations, delete the stale/duplicate ones from WebServer_Server.h, and replace the ad-hoc externs with the include.
- **[OLED_Mode_UnifiedMenu.cpp:173](components/hardwareone/OLED_Mode_UnifiedMenu.cpp#L173)** `M`, _periodic_ - fwHash-to-hex-to-manifest-path block duplicated
  - The 16-byte fwHash hex-encode loop plus /system/manifests/<hex>.json path build is copy-pasted at lines 172-178 and 285-291 within the same file (and again in WebPage_ESPNow.cpp). No shared hex/manifest-path helper exists.
  - _Evidence:_ `for (int i=0;i<16;i++) snprintf(fwHashHex+(i*2),3,"%02x",...fwHash[i]); ... "/system/manifests/%s.json"`
  - _Fix:_ Extract a helper like remoteManifestPath(cap, char* out, size_t) and call it from both sites.

### 3b. Shared-helper bypasses (adopt the existing helper)

- **[System_ESPNow_Sensors.cpp:582](components/hardwareone/System_ESPNow_Sensors.cpp#L582)** `H`, _per-tick_ - Broadcaster task hand-rolls interval gates instead of everyMs()
  - sensorBroadcasterTask re-implements the static-timestamp 5 s interval pattern twice (lastHwmLog at 582, lastPresenceAnnounce at 596) instead of the shared everyMs() helper in System_Utils.h:184, and hand-rolls the stack-HWM observation that observeHwm() (line 195) standardizes. No memory cost - pure duplication of the shared primitives.
  - _Evidence:_ `static unsigned long lastHwmLog = 0; if (now - lastHwmLog >= 5000) { lastHwmLog = now; ... }  (x2 in task loop)`
  - _Fix:_ Replace both hand-rolled gates with everyMs(&last, 5000) and route the HWM diagnostic through observeHwm().
- **[OLED_Mode_Auth.cpp:50](components/hardwareone/OLED_Mode_Auth.cpp#L50)** `H`, _per-frame_ - Keyboard-curtain idiom hand-rolled instead of oledKeyboardDrawIfActive()
  - OLED_Utils added oledKeyboardDrawIfActive() specifically to replace the `if (oledKeyboardIsActive()) { oledKeyboardDisplay(oledDisplay); return; }` idiom, but Auth (line 50), ChangePassword (54), and SetPattern (88) still open-code it verbatim (LLM/CLIInput too, slightly varied). Only OLED_Mode_Network adopted the helper.
  - _Evidence:_ `if (oledKeyboardIsActive()) { oledKeyboardDisplay(oledDisplay); return; }  // == oledKeyboardDrawIfActive(oledDisplay)`
  - _Fix:_ Replace the three-line idiom in each display func with `if (oledKeyboardDrawIfActive(oledDisplay)) return;`.
- **[OLED_Mode_Remote.cpp:260](components/hardwareone/OLED_Mode_Remote.cpp#L260)** `H`, _per-frame_ - MAC formatting hand-rolled instead of macToDisplay()
  - formatDeviceLabel() open-codes the exact `%02X:%02X:...` MAC display format that System_Utils.h::macToDisplay()/macToDisplayStr() already provide; the same file bypasses it again at line 398 building a bondconnect command. Duplicates the canonical formatter this project standardized on.
  - _Evidence:_ `snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X", dev.mac[0]..dev.mac[5]);`
  - _Fix:_ Call macToDisplay(dev.mac, buf, bufLen) (and macToDisplayStr for the command string at line 398).
- **[System_SensorLogging.cpp:78](components/hardwareone/System_SensorLogging.cpp#L78)** `M`, _per-tick_ - sensorLogTick hand-rolls millis() intervals instead of everyMs()
  - The tick gate (76-79), the idle heartbeat (467-483) and the periodic summary (582-588) each re-implement the `now - last >= interval` pattern that System_Utils.h::everyMs(uint32_t*, uint32_t) already provides. The project note explicitly says not to re-roll everyMs.
  - _Evidence:_ `static lastTickMs/lastHeartbeatMs/lastSummaryMs + `(long)(nowMs-last) < (long)interval` vs everyMs()`
  - _Fix:_ Replace the three hand-rolled interval checks with everyMs().
- **[G2_Page_Files.cpp:533](components/hardwareone/G2_Page_Files.cpp#L533)** `M`, _per-frame_ - File-size formatting re-implemented on the G2 file browser
  - System_FileManager.cpp exposes formatFileSize() (B/KB/MB), which the OLED file browser uses, but the G2 file page re-derives the same B/KB/MB thresholds inline in two spots with slightly different precision ("%.1f KB" vs the shared "%.2f KB"). Re-derivation of the same size-formatting logic, not just layout.
  - _Evidence:_ `G2 re-rolls B/K/M at G2_Page_Files.cpp:208 & 533-534; shared formatFileSize() at System_FileManager.cpp:545 (used by OLED_Mode_FileBrowser.cpp:927)`
  - _Fix:_ Reuse formatFileSize() (add a compact variant for the narrow list column) instead of inline snprintf in G2_Page_Files.
- **[G2_Glasses.cpp:14039](components/hardwareone/G2_Glasses.cpp#L14039)** `H`, _per-event_ - 14 raw xTaskCreate calls bypass xTaskCreateLogged
  - G2_Glasses.cpp spawns all its workers (g2_bmp_view, g2_jpg_view/full, g2_cam_view/stream, g2_map_page, g2_live_page, g2_live_text, g2_hb_worker, g2_ble_connect, g2_page_swap_w, g2_tap_disp) with bare xTaskCreate, while the project standard xTaskCreateLogged (System_TaskUtils.h:112) durably logs spawn failures to the system event log. Several of these are per-user-action spawns under tight internal DRAM - exactly the case where a silent pdFAIL makes a feature vanish with no trace beyond a debug-flag line.
  - _Evidence:_ `rg -c 'xTaskCreate\(' G2_Glasses.cpp = 14; System_TaskUtils.cpp:105 logs 'failed to create ... feature unavailable this boot'`
  - _Fix:_ Swap the 14 call sites to xTaskCreateLogged with appropriate tags (also gains the DEBUG_MEMORY per-task telemetry).
- **[System_ESPNow.cpp:3969](components/hardwareone/System_ESPNow.cpp#L3969)** `H`, _per-event_ - File-transfer event paths build heap String MACs despite existing buf helper
  - v4h_file_end (3969), fileWriterFinalizeJob (3912) and the streaming-append failure path (3874) each materialize String senderMacStr = formatMacAddress(...) - a 17-char heap alloc on espnow_task - only to pass .c_str() to logFileTransferEvent, and logFileTransferEvent itself (15143) builds another String deviceName per event. formatMacAddressBuf() already provides the zero-alloc form used elsewhere on this path.
  - _Evidence:_ `String senderMacStr = formatMacAddress(sndMac); ... logFileTransferEvent((uint8_t*)sndMac, senderMacStr.c_str(), filename, ...);`
  - _Fix:_ Use char mac[18] + formatMacAddressBuf at all four sites.
- **[System_ESPNow_Identity.cpp:309](components/hardwareone/System_ESPNow_Identity.cpp#L309)** `H`, _per-event_ - Hand-rolled MAC parsing instead of canonical macParse
  - parseMacNoSep (309, with its own nyb lambda) and the manual colon-strip loop in readPeerIdentityFile (432-439) re-implement System_Utils.h's macParse, which is documented as 'the canonical inbound parser' accepting separators or none - and the file's own #include comment (line 18) even names macParse. hexToBytes (41) duplicates the same nyb lambda a third time within this file.
  - _Evidence:_ `Line 18: '#include "System_Utils.h" // macToPathToken / macToDisplay / macParse' - yet parseMacNoSep + colon-strip loop are hand-rolled below`
  - _Fix:_ Replace parseMacNoSep calls and the colon-strip block with macParse(); share one nybble helper for hexToBytes.
- **[System_ESPNow_Sensors.cpp:650](components/hardwareone/System_ESPNow_Sensors.cpp#L650)** `H`, _per-event_ - Sensor broadcaster task created with raw xTaskCreatePinnedToCore
  - startSensorBroadcaster bypasses xTaskCreateLogged (System_TaskUtils.h:112), the house task-creation helper that logs heap/PSRAM deltas and accepts a coreId parameter - the sibling espnow_tx task in System_ESPNow_Tx.cpp:153 uses it correctly. This task is created/destroyed at runtime on streaming toggles, exactly when allocation logging is most useful.
  - _Evidence:_ `BaseType_t ret = xTaskCreatePinnedToCore(sensorBroadcasterTask, "sensor_bcast", SENSOR_BCAST_STACK_WORDS, nullptr, TASK_PRIORITY_HIGH, &gSensorBroadcasterTask, 1);`
  - _Fix:_ Use xTaskCreateLogged(..., "espnow.sensor_bcast", 1) like the espnow_tx task does.
- **[WebPage_ESPNow.cpp:252](components/hardwareone/WebPage_ESPNow.cpp#L252)** `H`, _per-request_ - MAC-address display formatting hand-rolled instead of shared helper
  - System_Utils.h already defines macToDisplay()/formatMacAddr() (and System_ESPNow.h formatMacAddress) as the canonical colon-hex formatter, but the web ESP-NOW JSON builder, the OLED ESP-NOW screens, and SelfDevice each hand-roll the identical snprintf("%02X:..."). Same format re-implemented on three surfaces.
  - _Evidence:_ `snprintf("%02X:..%02X",mac) at WebPage_ESPNow.cpp:252,322,370; OLED_ESPNow.cpp:1789,1833,2182,2254; System_SelfDevice.cpp:62`
  - _Fix:_ Replace the hand-rolled snprintf sites with macToDisplay()/formatMacAddr() from System_Utils.h.
- **[System_MemoryMonitor.cpp:421](components/hardwareone/System_MemoryMonitor.cpp#L421)** `H`, _periodic_ - periodicMemorySample hand-rolls millis() interval gates
  - The Tier-1 pressure check hand-rolls 'if (last==0 || (now-last)>=intervalMs)' twice (421 for the 1s poll, 427 for the 10s warn), which is exactly the everyMs(&stamp,interval) primitive already provided in System_Utils.h and used elsewhere.
  - _Evidence:_ `if (lastPressureCheckMs == 0 || (nowMs - lastPressureCheckMs) >= 1000UL) ...  (and lastPressureWarnMs 10000UL)`
  - _Fix:_ Replace the two hand-rolled interval checks with everyMs(&lastPressureCheckMs,1000) / everyMs(&lastPressureWarnMs,10000).
- **[System_ESPNow.cpp:150](components/hardwareone/System_ESPNow.cpp#L150)** `M`, _per-event_ - 41 hand-rolled "%02X:%02X:..." MAC format sites despite dedicated MAC_STR/formatMacAddressBuf helpers
  - System_ESPNow.cpp contains 41 log/snprintf sites that spell out the six-octet format string and six array-index arguments by hand, while the same file/header provide MAC_STR() (zero-alloc, built exactly for DEBUGF call sites), formatMacAddressBuf, and macToHexString. Not a heap issue, but sustained duplication that invites octet-order/typo bugs and bloats format strings.
  - _Evidence:_ `rg '%02X:%02X:%02X:%02X:%02X:%02X' System_ESPNow.cpp -> 41 hits, e.g. cpp:150, 1699, 4466, 8054 - alongside MAC_STR (System_ESPNow.h:1098)`
  - _Fix:_ Sweep the printf-style sites to MAC_STR(mac) (one caveat: its static buffer allows only one use per format call).
- **[System_TaskUtils.h:112](components/hardwareone/System_TaskUtils.h#L112)** `H` - Raw xTaskCreate/PinnedToCore bypasses xTaskCreateLogged
  - ~30 task-creation sites call raw xTaskCreate / xTaskCreatePinnedToCore instead of xTaskCreateLogged, so those tasks never get the heap/PSRAM-delta accounting and tag logging the helper provides. This is the single largest consistency gap for the memory-tight goal, since untracked task stacks are the biggest DRAM lever.
  - _Evidence:_ `G2_Glasses.cpp:6054, System_LLM.cpp:2293, System_EdgeImpulse.cpp:1879, System_Microphone.cpp:365, System_Camera_Video.cpp:421, OLED_Mode_Map.cpp:928, System_ESPNow.cpp:8613, System`
  - _Fix:_ Route all task creation through xTaskCreateLogged (pass coreId where pinning is needed) so every task is tracked.
- **[System_Utils.cpp:1526](components/hardwareone/System_Utils.cpp#L1526)** `H` - Timestamp formatting bypasses the Clock:: namespace
  - A dedicated Clock:: namespace (System_Clock.h) provides formatISO8601Local/formatFilenameLocal/formatHHMMLocal to unify time formatting, yet those helpers have zero callers and every surface hand-rolls localtime_r + strftime, even inventing three different filename-timestamp formats. Same fact/format re-derived across CLI, web, logging, and capture paths.
  - _Evidence:_ `strftime/localtime re-rolled; also System_User.cpp:2684, System_WiFi.cpp:948, WebServer_Server.cpp:4636, System_SensorLogging.cpp:1128, System_Maps.cpp:2857`
  - _Fix:_ Route all timestamp/filename formatting through the existing (currently unused) Clock::formatISO8601Local / formatFilenameLocal helpers.
- **[System_Utils.h:427](components/hardwareone/System_Utils.h#L427)** `H` - Standalone MAC snprintf bypasses macToDisplay/macToDisplayStr
  - Many sites open-code snprintf(buf,size,"%02X:%02X:%02X:%02X:%02X:%02X", mac[0..5]) into a dedicated buffer instead of calling macToDisplay()/macToDisplayStr(), duplicating the canonical DISPLAY-form formatter. (Inline-in-larger-format-string log lines are excluded.)
  - _Evidence:_ `System_ESPNow.cpp:7466, System_ESPNow.cpp:13496, System_ESPNow.cpp:9326, OLED_ESPNow.cpp:1789, WebPage_ESPNow.cpp:252, Bluetooth.cpp:364, System_SelfDevice.cpp:62, OLED_Mode_Remote`
  - _Fix:_ Replace standalone MAC snprintf blocks with macToDisplay(mac,buf,size) (or macToDisplayStr for String results).
- **[System_Mutex.h:187](components/hardwareone/System_Mutex.h#L187)** `M` - Direct xSemaphoreTake on guarded mutexes bypasses RAII guards
  - Sensor OLED/web render helpers take gXxxCache.mutex directly with xSemaphoreTake instead of SensorCacheGuard, and one JSON-buffer site bypasses JsonBufferGuard. Manual take/give risks a missed give on an early-return path, exactly what the reentrant-safe guards prevent.
  - _Evidence:_ `i2csensor_bno055_oled.h:31, i2csensor_vl53l4cx_oled.h:24, i2csensor_pa1010d_oled.h:38, i2csensor_apds9960_oled.h:29, i2csensor_seesaw_oled.h:33, i2csensor_ds3231_oled.h:36, i2csens`
  - _Fix:_ Wrap these critical sections in SensorCacheGuard (cache mutexes) / JsonBufferGuard (gJsonResponseMutex).
- **[System_Utils.h:184](components/hardwareone/System_Utils.h#L184)** `M` - Hand-rolled millis()-gate and HWM-max bypass everyMs/observeHwm
  - Periodic reporters and UI tick-gates open-code `millis() - lastX >= interval; lastX = millis();` instead of everyMs(&lastX,interval); the two ESP-NOW stack reporters also hand-roll the running-max that observeHwm() provides. The diagnostics-primitive header explicitly intends these to migrate.
  - _Evidence:_ `HardwareOne.cpp:2081, OLED_Mode_Map.cpp:1299, OLED_Mode_UnifiedMenu.cpp:464, System_ESPNow_Tx.cpp:120 & :68(observeHwm), System_ESPNow_Sensors.cpp:582, System_Microphone_OLED.h:39,`
  - _Fix:_ Adopt everyMs() for periodic gates and observeHwm()/saturationLabel() for the queue/stack reporters.
- **[System_Utils.h:236](components/hardwareone/System_Utils.h#L236)** `M` - Hand-rolled on/true/1/enable chains bypass parseBoolArg
  - Numerous CLI handlers re-roll `arg == "on" || arg == "1" || arg == "true" || arg == "enable"` instead of calling parseBoolArg(), which already encodes the full on/off/true/false/1/0/enable/disable contract with a tri-state return. The hand-rolled variants also diverge (some omit "enable", some omit the off side).
  - _Evidence:_ `System_MQTT.cpp:1495, System_ESPNow_Sensors.cpp:1009, System_ESPNow.cpp:11152, System_ESPNow.cpp:14850, i2csensor_ds3231.cpp:902, i2csensor_bno055.cpp:1017, HAL_Input.cpp:289, Syst`
  - _Fix:_ Use parseBoolArg(arg) (or the settingBoolToggle/BOOL_CMD helpers) for on/off argument parsing.
- **[System_Utils.h:401](components/hardwareone/System_Utils.h#L401)** `M` - arg.trim();arg.toLowerCase() pairs bypass normalizeCliArg
  - Command handlers repeat the two-line `arg.trim(); arg.toLowerCase();` normalization that normalizeCliArg(String&) was created to collapse. These are exactly the pattern the helper's own comment calls out.
  - _Evidence:_ `System_Hardware.cpp:40, G2_Glasses.cpp:10701, OLED_Utils.cpp:3620, i2csensor_ano_encoder.cpp:355, i2csensor_ano_encoder.cpp:375, Bluetooth.cpp:1551, Bluetooth.cpp:1600, OLED_SetupW`
  - _Fix:_ Call normalizeCliArg(arg) instead of the manual trim()+toLowerCase() pair.

## 4. Other

- **[Bluetooth.cpp:612](components/hardwareone/Bluetooth.cpp#L612)** `M`, _per-command_ - Plaintext command path runs ~1 KB of locals on BTC_TASK, with contradictory in-file stack claims
  - For plaintext (non secure-channel) writes, processIncomingBLECommand calls processBleCommandLine directly on BTC_TASK: cmdBuf[512] plus the login branch's u[64]+p[128]+loginCmd[256]+Command (with String members) is ~1 KB in one frame beneath Bluedroid's own callback depth. sdkconfig currently sets CONFIG_BT_BTC_TASK_STACK_SIZE=8192 so it fits today, but the file's own comments disagree (line 430 says ~3 KB, line 788 says 8 KB) and the board default sdkconfig.esp32s3 is 4096 - a config regression would make this an overflow.
  - _Evidence:_ `char cmdBuf[512]; ... char u[64]; char p[128]; char loginCmd[256]; (all live under BTC_TASK when plaintext commands arrive)`
  - _Fix:_ Defer plaintext command parsing through the same BleScDeferred/cmd_exec hop the secure path uses (or fix the stale ~3KB comment and add a static_assert-style note tying it to CONFIG_BT_BTC_TASK_STACK_SIZE).

## Recommended sequencing

**Wave 1 - verified, low-risk, high-frequency (do now):** the `executeCommand` cleanups (sec.  short-list 1), the thermal/ToF busy-spin (2), and the BLE notify/write copies (3). Small diffs, hot paths, all verified.

**Wave 2 - bulk dead-code deletion:** start with the whole-file/whole-subsystem candidates (`BLE_IDF.*`, `WebPage_Register.h`, the `WebServer_MigrationTool` restore handler, the legacy OLED remote-file-browse subsystem, the ESP-NOW mesh-retry / V3-dedup / topology fossils). These are large and self-contained. Reachability-check each, then delete in one commit per subsystem so a mistake is easy to bisect. No backwards-compat shims are needed (single-owner devices, full-erase reflash).

**Wave 3 - string-churn sweep on the recurring surfaces:** the debug/logging layer, the ESP-NOW packet path, the automation evaluator, and the OLED per-frame renderers. These share a pattern (`String` concat / by-value returns on a loop) and benefit from the same fixes: `reserve()`, `snprintf` into stack buffers, and passing `const char*` instead of `String` by value.

**Wave 4 - consolidation:** the duplication/helper-bypass items (sec. 3). Lower urgency but directly serves the 1.0 goal of one shared implementation per feature. The MAC-formatting bypass (41 hand-rolled `%02X:...` sites) and the raw `xTaskCreate` sites (bypassing `xTaskCreateLogged`) are the widest-reaching.

## Caveats

- **Verification status:** 21 findings were re-read and confirmed by me this session (marked verified). The remaining 254 are auditor-reported at the stated confidence and were **not** individually re-verified - the adversarial verification pass was dropped to stay within the spend limit. Confirm reachability before deleting any un-verified dead-code item.
- **Board config:** the primary board is FeatherS3; some camera / XIAO-octal paths are config-gated and will read as "dead" under the current build but are live under other `HW_BOARD` values. Auditors were told to apply this rule, but double-check any camera/board-specific deletion.
- **Scope gaps:** the event system was excluded by request (another agent is refactoring it). Vendored libraries and the generated game blobs (`WebPage_Games.h`, `WebPage_DarkRoom.h`) were not audited.

