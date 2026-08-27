# WiFi Radio Ownership vs ESP-NOW — Plan

Problem (user): turned WiFi off from the OLED quick-settings toggle. HTTP stopped, but
the toggle still showed **ON** and the driver kept logging `Haven't to connect to a
suitable AP now!`. Cause: ESP-NOW owns the radio, and the "off" path is half-implemented.

## 1. What actually happened

- `closewifi` (`cmd_wifidisconnect`, System_WiFi.cpp:384-404) stops the HTTP server, clears
  the web output lane, then calls `WiFi.disconnect()` — **association drop only**. Mode,
  driver, and power-save are untouched. That's the `connection lost: 'WooFoo' reason=8`
  (reason 8 = station left on purpose) in the log.
- **`WiFi.setAutoReconnect(false)` is called nowhere** in the component. So Arduino's default
  auto-reconnect stays armed and the driver keeps hunting the lost AP forever →
  `Haven't to connect to a suitable AP now!`. That reconnect **scan hops channels** on a
  single radio, so it can knock a pinned ESP-NOW channel (espnowApplyChannel,
  System_ESPNow.cpp:953-965) off the air intermittently. This is the real defect, not cosmetic.
- The quick-toggle reads the wrong state: `getQuickWiFiState()` returns
  `WiFi.getMode() != WIFI_MODE_NULL` (OLED_SettingsEditor.cpp:995-999). ESP-NOW holds the
  radio in `WIFI_AP_STA` (initEspNow, System_ESPNow.cpp:9400) — mode is never NULL — so the
  toggle is stuck **ON** and `toggleQuickWiFi` can only ever issue `closewifi`
  (the `openwifi --best` branch is unreachable).

## 2. Root causes (defects, file:line)

- D1 — reconnect hunt never stopped. closewifi lacks `WiFi.setAutoReconnect(false)`
  (System_WiFi.cpp:397). Channel-hop hazard for ESP-NOW.
- D2 — status surfaces read radio *mode*, not *association*. getQuickWiFiState
  (OLED_SettingsEditor.cpp:996). Same class likely on other surfaces (wifistatus, web
  indicator, OLED header icon, G2 Network page) — audit needed.
- D3 — `openwifi` while ESP-NOW is up downgrades `WIFI_AP_STA → WIFI_STA` and stops the
  driver per attempt (connectToBestWiFiNetwork System_WiFi.cpp:286; connectWiFiIndex :708-724),
  blacking out ESP-NOW TX/RX and never restoring AP_STA. (Not hit in the user's log, but the
  inverse hazard of the same entanglement.)
- D4 — nuclear reinit path `esp_wifi_deinit` without `esp_now_deinit` first
  (System_WiFi.cpp:820-836) wedges an initialized mesh against a destroyed driver.
- D5 — no persisted "user turned WiFi off" intent; `gSettings.wifiEnabled` exists
  (System_Settings.h:412, default true) but closewifi doesn't set it, so behavior across a
  reboot is inconsistent with the runtime toggle.

## 3. Target state model

Single source-of-truth accessor for WiFi radio state, tri-state:

- `RADIO_OFF` — driver mode NULL/off; nothing using the radio.
- `RADIO_UP_FOR_ESPNOW` — driver up (AP_STA), STA **unassociated**; ESP-NOW holds it.
- `STA_CONNECTED` — associated to an AP.

Add `wifiRadioState()` (System_WiFi.cpp) returning this enum from `esp_wifi_get_mode` +
`WiFi.isConnected()` + `isEspNowInitialized()`. Every surface reads it. The seed enum already
exists read-only in G2_Page_ESPNow.cpp:97-112 (RadioOff/Off/On) — generalize.

User-facing rule: "WiFi ON/OFF" means **associated or not** (D2). When off-but-radio-held,
show a secondary hint "(radio up for ESP-NOW)".

## 4. Staged fix

### Two separate settings — DO NOT conflate (user requirement)
- RUNTIME auto-reconnect = `WiFi.setAutoReconnect(bool)` — driver retrying the AP now. NOT
  persisted; a reboot resets it to the default (on). closewifi turns this OFF; openwifi ON.
- BOOT autostart = `gSettings.wifiAutoReconnect` (System_Settings.h:413), consulted only at
  boot (HardwareOne.cpp:1564). **closewifi must NEVER touch this.** Turning WiFi off at runtime
  must not disable WiFi-on-boot. Controlling boot autostart stays a separate explicit setting.

### Stage 1 — make "WiFi off" honest and stop the hunt  (S, low risk) — RECOMMENDED FIRST
- closewifi: add `WiFi.setAutoReconnect(false)` (RUNTIME only — does NOT persist / does NOT
  affect boot autostart) before `WiFi.disconnect()`. Then branch on `isEspNowInitialized()`:
  - ESP-NOW up → keep radio; message `OK: WiFi disconnected (radio held for ESP-NOW).` The
    existing WIFI_STA_DISCONNECTED→espnow_task channel re-pin already fires.
  - ESP-NOW down → full power down (`WiFi.disconnect(true)` / `WiFi.mode(WIFI_OFF)`); message
    `OK: WiFi off.`
  - Keep the HTTP-stop + MSG_ROUTE_WEB clear exactly as is. **Do NOT** persist `outWeb`
    (regression guard — comment at System_WiFi.cpp:391-393).
- getQuickWiFiState → `WiFi.isConnected()`; toggleQuickWiFi label off `isConnected()`.
- openwifi re-arms auto-reconnect (`WiFi.setAutoReconnect(true)`) so Stage-1 doesn't strand it.
- Build + HW test: toggle reflects connection; no reconnect spam after off; ESP-NOW keeps
  ACKing 100%.

### Stage 2 — WiFi status truth everywhere  (M) — RECOMMENDED
- Add `wifiRadioState()` accessor; route wifistatus, web dashboard indicator, OLED header
  icon, G2 Network line through it (share logic — no per-surface reimpl).
- Status is derived from live radio/association state only — NO new persisted "off" intent
  from runtime toggles (see the two-settings rule above). Boot autostart remains the
  independent `gSettings.wifiAutoReconnect`.
- Build + HW test each surface.

### Stage 3 — connect path coexists with ESP-NOW  (M/L) — the deeper fix
- Make connectWiFiIndex preserve `WIFI_AP_STA` when ESP-NOW is up (don't force STA-only /
  full stop); restore AP_STA + re-pin channel after connect. Fence the nuclear reinit
  (D4): `esp_now_deinit` first (or refuse while ESP-NOW is up).
- Wrap mode changes in I2C `pausePolling` like initEspNow does (System_ESPNow.cpp:9395).

### Stage 4 — (optional, large) System_RfMode radio arbiter
- The full single-owner retrofit already designed in docs/DEFLOCK_CAR_DETECTOR_PLAN.md §9.
  Only if a comprehensive cleanup is wanted; Stages 1-3 solve the reported problem without it.

## 5. Risks & guards
- Regression: do not re-persist `outWeb` in closewifi (already-fixed clobber).
- ESP-NOW channel re-pin timing: rely on the existing disconnect-event resync; verify with
  `espnowstatus` channel after off.
- BLE coex: esp_now_init maps radio-busy to a clear message (System_ESPNow.cpp:9452) — no new risk.
- Heap: the WiFi driver stays allocated when radio is held for ESP-NOW (expected; it's the point).
- Boot ordering: RF_WIFI/RF_HTTP/RF_MQTT/RF_ESPNOW apply-sites HardwareOne.cpp:1564/1767/1795/1926.

## Recommendation
Do **Stage 1 + Stage 2** now (solves the user's problem end-to-end: honest toggle, no hunt,
correct status everywhere). Stage 3 only if `openwifi`-while-ESP-NOW-up needs to work
cleanly. Stage 4 (arbiter) deferred to the DeFlock doc.
