# MAC Address Formatting — Codebase Inventory & Unification

Full sweep of every place a 6-byte MAC is formatted, parsed, stored, or
compared across `components/hardwareone/`. Captured to drive a single
canonical API and retire the hand-rolled duplicates.

## TL;DR — two string forms, both UPPER

Everything is UPPER-case. The only remaining distinction is the **separator**,
driven by what the destination medium needs.

| Form | Example | Case | Sep | Consumers |
|---|---|---|---|---|
| **DISPLAY** | `AA:BB:CC:DD:EE:FF` | UPPER | `:` | logs, OLED/web UI, `devices.json`, settings, CLI output, topology path nodes |
| **PATH TOKEN** | `AABBCCDDEEFF` | UPPER | none | filesystem paths (`/system/espnow/peers/<TOK>/`, `/espnow/received/<TOK>/`) **and** MQTT client-id / topics / HomeAssistant entity ids |

MQTT/HomeAssistant now uses the UPPER no-separator form too. There is no
backwards-compat / entity-continuity concern — devices re-discover. HA matches
MACs case-insensitively (`format_mac()` only normalizes the device-registry
connection tuple), and topics, client-id, and unique_id are arbitrary strings,
so UPPER is accepted everywhere.

## Canonical API (NEW — `System_Utils.h`)

Always available, independent of `ENABLE_ESPNOW`. Byte-for-byte identical to
the hand-rolled sites they replace, so no on-disk path / stored MAC / MQTT
topic changes.

```cpp
void   macToDisplay(const uint8_t* mac, char* buf, size_t bufSize); // "AA:BB:..:FF"
String macToDisplayStr(const uint8_t* mac);
void   macToPathToken(const uint8_t* mac, char* out13);             // "AABBCCDDEEFF"
String macToPathTokenStr(const uint8_t* mac);
bool   macEquals(const uint8_t* a, const uint8_t* b);               // memcmp(...,6)==0
bool   macParse(const char* s, uint8_t mac[6]);                     // lenient: ':' '-' ' ' or none
```

`formatMacAddr` / `formatMacAddrStr` retained as thin aliases of the DISPLAY
form for existing callers.

## Resolution applied (this commit)

The named formatter/parser duplicates were re-pointed to the canonical API
(behavior-preserving). The ~100 inline `snprintf("%02X:...")` *log* sites were
left as-is — they already emit the correct DISPLAY form and rewriting each to a
temp buffer is pure churn with no behavior benefit. They can adopt the helper
opportunistically.

| Site | Old | Now |
|---|---|---|
| `System_Utils.h` `formatMacAddr`/`Str` | hand-rolled | DISPLAY canonical (others alias) |
| `System_ESPNow.cpp` `formatMacAddressBuf` | hand-rolled | → `macToDisplay` |
| `System_ESPNow.cpp` `formatMacAddress` | hand-rolled | → `macToDisplayStr` |
| `System_ESPNow.cpp` `macToHexString` | hand-rolled | → `macToDisplayStr` |
| `System_ESPNow.cpp` `macFromHexString` | `sscanf` colon-lower | → `macParse` (zero-fill on fail) |
| `System_ESPNow.cpp:5076/5122` (settings cache path) | inline nosep-UPPER | → `macToPathToken` |
| `System_ESPNow.cpp:3549` (`/espnow/received/`) | `macToHexString` + `replace(":")` | → `macToPathToken` |
| `System_ESPNow.cpp:4543` (capture log) | inline **colon-lower** | → `macToDisplay` (now UPPER) |
| `System_ESPNow_Identity.cpp` `formatMacNoSep` | hand-rolled | → `macToPathToken` |
| `System_ESPNow_Identity.cpp:378` (`macColons`) | inline colon-UPPER | → `macToDisplay` |
| `OLED_ESPNow.cpp` `oledEspNowFormatMac` | hand-rolled | → `macToDisplayStr` (keeps null guard) |
| `OLED_RemoteSettings.cpp:149/286` | `replace(":")` + manual strtol | → `macParse` |
| `OLED_RemoteSettings.cpp:293` | inline nosep-UPPER | → `macToPathToken` |
| `Bluetooth.cpp` `macToStackBuf` | hand-rolled | → `macToDisplay` |
| `BLE_IDF.cpp:83` (BLE addr log) | inline **colon-lower** | → `macToDisplay` (now UPPER) |
| `System_MQTT.cpp` ×5 | inline **nosep-lower** | → `macToPathToken` (now UPPER) |

### Normalized to UPPER (case change)

- `System_ESPNow.cpp:4543` — base64 capture log. Log-only; now UPPER.
- `BLE_IDF.cpp:83` — BLE device address debug log. Log-only; now UPPER.
- `System_MQTT.cpp` ×5 — MQTT client-id, peer device ids, base topic. Now UPPER
  no-separator. No entity-continuity concern (devices re-discover); HA accepts
  any case for topics / client-id / unique_id.

## Left intact (intentional)

- `parseMacAddress(const String&, uint8_t[6])` — the lenient CLI parser accepts
  1- or 2-digit colon groups (e.g. `A:B:C:D:E:F`). `macParse` requires 12
  nibbles. Kept separate to preserve CLI input behavior exactly.
- `parseMacNoSep` (Identity) — strict 12-char contract for directory-name scan.
- Topology path nodes (`System_ESPNow.cpp:5915/9231/9263`) — colon DISPLAY form
  is the on-wire node identifier; unchanged.
- `MAC_STR(mac)` macro (`System_ESPNow.h:1050`) — static `char[18]` footgun
  (one MAC per statement). Left for a future targeted cleanup.
- ~100 inline `snprintf("%02X:...")` log sites — already DISPLAY form.

## Raw inventory

See `git log` for the full grep sweep that produced this. Section breakdown:
- **A** — formatter/parser function definitions (29 sites across 13 files).
- **B** — raw format-string patterns by variant: A1 colon-UPPER (~90), A2
  colon-lower (3 → 2 normalized + 1 sscanf rerouted), A3 nosep-UPPER (7), A4
  nosep-lower (5, all MQTT → now nosep-UPPER). Zero lowercase sites remain.
- **C** — `uint8_t mac[6]` field/param declarations (~160).
- **D** — String-typed MAC fields (settings: `meshMasterMAC`, `bondPeerMac`,
  `bondPeerMacMesh[]`; BLE peers `mac1`/`mac2`).
- **E** — comparisons: 52 raw `memcmp(...,6)` in `System_ESPNow.cpp` alone,
  plus `isSelfMac`/`equalsIgnoreCase`. Candidates for `macEquals`.

## Deferred follow-ups

- Migrate the 52 `memcmp(...,6)` sites in `System_ESPNow.cpp` to `macEquals`
  for readability (no behavior change).
- Retire `MAC_STR` macro in favor of explicit `macToDisplay` + local buffer.
- Opportunistically convert inline log snprintf sites to `macToDisplay`.
