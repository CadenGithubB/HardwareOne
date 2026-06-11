# HardwareOne — Device Status Page integration (CLI `status json`)

This is the contract for building a **Device Status / Device Info page** in the
Android companion app. It uses the firmware's existing command channel — **no
new endpoint, no web/HTTP dependency.**

---

## TL;DR for the app

1. You already have a working command channel (you send a command string, you
   get a response back — the same path the "status" button and `login` use).
2. To populate a status page, send the literal command:

   ```
   status json
   ```

3. The response is **one JSON object** (schema below). Reassemble the response
   chunks exactly as you already do for any command reply, then `JSON.parse`
   it once. **Do not** parse the plain-text `status` output.
4. Re-request on a timer (e.g. every 2–5 s) while the page is open. There is no
   push/subscribe for this — it's request/response.

That's the whole integration. Everything below is detail.

---

## Why `status json` and not the plain text

- `status`      → human-readable lines, meant for a console view. Format is not
  stable and must not be screen-scraped.
- `status json` → a single machine-readable JSON document with **stable keys**.
  This is the same schema the web dashboard's `/api/system` returns, so it
  won't silently drift.

The `json` token is detected at a word boundary, so `status json` works; case
matters (lowercase `json`).

## Transport notes

- Works over **any** transport with identical bytes: BLE, the web CLI, and the
  USB serial console. You can prototype against serial first, then wire up BLE.
- Over BLE the reply comes back on the command **response (notify)**
  characteristic, encrypted + chunked by the Secure Channel exactly like every
  other command result. **Accumulate notification fragments and attempt
  `JSON.parse` on the assembled buffer**; when it parses, you have the full
  document. (This is the same reassembly you already do for long replies.)
- The command requires an **authenticated session** if the device has BLE auth
  enabled — i.e. do your normal `login` first (you already do). `status` is not
  admin-only.
- The CLI/BLE reply is the **compact** form (~800 bytes) — it omits the
  unbounded I²C device list. It fits comfortably in all result buffers.

## Versioning

Top-level `"v"` is the schema version (currently `1`). Check it; if it's a
number you don't recognize, degrade gracefully rather than assuming fields.

## Error shape

If the device is momentarily out of memory it returns:

```json
{"error":"oom"}
```

Treat any object containing `"error"` as a soft failure (show "unavailable",
retry on the next tick).

---

## Schema (`status json`, compact form)

All `*_kb` / `*_mb` values are integers. Booleans are real JSON `true`/`false`.
Strings may be empty (`""`) — empty is a meaningful sentinel where noted.

```json
{
  "v": 1,
  "fw": "1.2.3",                 // firmware version string
  "board": "feathers3",          // board name
  "reset_reason": "Power-on",    // human label for the last reset
  "reset_reason_code": 1,        // numeric esp_reset_reason_t
  "crash_count": 0,

  "system_time": "2026-06-11 14:03:22",  // "" if clock not yet synced
  "uptime_hms": "3h 12m 40s",

  "net": {
    "ssid": "MyWiFi",            // "" when disconnected
    "ip": "192.168.1.42",        // "" when disconnected
    "rssi": -58,                 // 0 when disconnected
    "channel": 6,                // 0 when disconnected
    "mac": "AA:BB:CC:DD:EE:FF"   // always present
  },

  "mem": {
    "heap_free_kb": 142,
    "heap_total_kb": 320,
    "psram_total_kb": 8192,
    "psram_free_kb": 6100
  },

  "storage": {
    "total_kb": 1024,
    "used_kb": 312,
    "free_kb": 712,
    "sd": {                      // PRESENT ONLY when an SD card is mounted
      "total_mb": 30436,
      "used_mb": 1200,
      "free_mb": 29236
    }
  },

  "connectivity": {
    // Every sub-object below is OPTIONAL — present only when that feature is
    // compiled into the running firmware. Use null-safe access and render a
    // section only if its object exists.

    "espnow": {
      "enabled": true,
      "running": true,
      "mesh": false,
      "deviceName": "node-1",
      "encrypted": true,
      "passphraseSet": true
    },
    "bond": {                    // only if ESP-NOW + bonded mode are compiled
      "enabled": false,
      "role": 0,
      "online": false,
      "synced": false,
      "peer": "AA:BB:CC:11:22:33"
    },
    "mqtt": {
      "enabled": false,
      "connected": false,
      "host": "broker.local"
    },
    "bluetooth": {
      "running": true,
      "state": "advertising",
      "mode": "server",          // "server" | "client"
      "server": true,
      "client": false,
      "g2Connected": false
    },
    "webserver": {               // only if the HTTP server is compiled
      "running": true,
      "https": false,
      "port": 80,
      "sessions": 1,
      "maxSessions": 2
    },
    "i2c": {
      "compiled": true,
      "enabled": true,
      "devices": 3,              // discovered device count
      "activeDevices": 2,        // devices with a polling driver enabled
      "sdaPin": 8,
      "sclPin": 9
      // NOTE: the per-device list is intentionally NOT in `status json` (it is
      // the only unbounded array — keeping it out is what makes the status poll
      // ~800 B). `i2c.devices`/`i2c.activeDevices` give you the counts for the
      // card header; fetch the actual list on demand with `devices json` (see
      // the next section).
    },
    "llm": {                     // only if on-device LLM is compiled
      "state": "READY",          // UNLOADED | LOADING | READY | GENERATING | ERROR
      "model": "model.gguf",
      "psramKB": 4096,
      "tokPerSec": 12.4
    }
  }
}
```

### Field semantics worth handling explicitly

- **`system_time == ""`** → clock not synced yet; show "—" / "syncing".
- **`net.rssi == 0` with `net.ip == ""`** → not connected; don't render a bar.
- **`connectivity.*` missing** → that subsystem isn't in this build; hide the
  card, don't show "off".
- **`bluetooth.mode`** tells you whether the radio is acting as a phone-facing
  server or a G2-glasses client.

---

## Fetching the I²C device list (`devices json`)

The connected-sensor list — the same one the web dashboard shows — is a
**separate, on-demand command** so the status poll stays small. Call it when
the user opens/expands the I²C card; you don't need it on every status tick.

Send:

```
devices json
```

Returns:

```json
{
  "v": 1,
  "count": 3,
  "devices": [
    { "name": "DS3231",  "addr": 104, "bus": 0 },
    { "name": "BNO055",  "addr": 40,  "bus": 1 },
    { "name": "VL53L4CX","addr": 41,  "bus": 1 }
  ]
}
```

- `count` — number of entries in `devices` (only currently-connected devices).
- `addr` — the **decimal** I²C address (e.g. `104` = `0x68`). Format as hex in
  the UI if you like: `0x%02X`.
- `bus` — which I²C bus the device is on (0 / 1).
- Same data and field names the web dashboard renders, so it won't drift.

Tie it to `status json`'s `connectivity.i2c.devices` count: show "3 devices"
on the card from the status poll, and expand to the full `devices json` list
when tapped.

---

## Recommended page layout (suggestion, not required)

- **Header:** `fw`, `board`, `uptime_hms`, `system_time`.
- **Health:** `mem.*`, `storage.*`, `reset_reason` + `crash_count`.
- **Network:** `net.*`.
- **Connectivity cards:** one per present `connectivity.*` object.

## Other structured commands (same `json` convention)

All take a trailing `json` token and return one verbatim JSON object. Use these
for detail views; `status json` already carries the summaries.

**`features json`** — what this firmware build supports + runtime state. Use it
to gate UI (don't show a thermal-camera page if `compiled:false`). Can exceed
2 KB — read it on demand, not on a poll.
```json
{"v":1,"features":[
  {"id":"thermal","name":"Thermal Camera","category":"Sensor",
   "heapKB":32,"compiled":true,"enabled":false,"toggleable":true}, ...]}
```
- `compiled` — present in this build at all.
- `enabled` — currently active.
- `toggleable` — can be turned on/off at runtime (vs. needs reboot / compile-time).

**`bleinfo json`** — BLE configuration + live state (no secrets):
```json
{"v":1,"deviceName":"HardwareOne","txPower":7,"autoStart":true,
 "requireAuth":true,"secureChannelRequired":true,"initialized":true,
 "state":"advertising","connections":1,"maxConnections":3}
```
- `secureChannelRequired` — if true, the app MUST establish the Secure Channel
  before plaintext commands are accepted.

**`uptime json`**:
```json
{"v":1,"uptime_s":11560,"uptime_ms":11560123,"uptime_hms":"3h 12m 40s"}
```

**`time json`** — active time source (RTC primary, NTP fallback):
```json
{"v":1,"synced":true,"source":"rtc","time":"2026-06-11T14:03:22",
 "uptime_ms":11560123,"rtc_temp_c":27.5}
```
- `source` — `"rtc"` | `"ntp"` | `"none"`. `rtc_temp_c` present only with an RTC.
- `synced:false` → show "time not set".

**`battery json`** — battery telemetry for a battery widget:
```json
{"v":1,"voltage":3.97,"percentage":82,"status":"Discharging","charging":false,
 "usbPresent":true,"vbusSense":true,"lastReadMsAgo":1200,
 "backend":"adc","rawADC":2450}
```
- `backend` — `"adc"` | `"fuelgauge"` | `"usb-only"`. With `"adc"` you get
  `rawADC`; with `"fuelgauge"` you get `cratePctPerHr` (charge rate %/hr);
  `"usb-only"` means no battery hardware (treat percentage as nominal).
- `vbusSense:false` → `usbPresent`/`charging` are inferred from voltage, not a
  real USB-detect pin (slightly less reliable).

## Sensors — already covered (no separate command)

There is intentionally no `sensors json`. For a sensor section, combine what
you already have:
- **`features json`** → every sensor's `compiled` (present in this build) and
  `enabled` (currently on) state — thermal, tof, imu, gps, fmradio, apds,
  presence are all `category:"Sensor"`.
- **`devices json`** → the I²C sensors actually detected on the bus right now.

(The `sensors` CLI command is a ~50-entry static parts catalog, not this
device's live state — it's deliberately not exposed as JSON.)

## Do / Don't

- ✅ Send `status json`; parse the JSON; key off stable field names.
- ✅ Poll on a timer while the page is visible; stop when it's not.
- ✅ Treat `connectivity.*` sub-objects as optional.
- ❌ Don't parse the plain `status` text.
- ❌ Don't assume a field exists because another build had it — guard on `v`
  and on presence.
