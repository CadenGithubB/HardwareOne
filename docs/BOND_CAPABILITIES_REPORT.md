# HardwareOne ESP32: ESP-NOW Bond Capabilities

This document describes **ESP-NOW bond mode only** (`ENABLE_BONDED_MODE`): how it works, what a bonded peer can do, trade-offs, OLED integration, and how bonded abilities could feel more “system-wide.” It is based on `System_ESPNow.cpp`, `System_ESPNow.h`, `OLED_Mode_Remote.cpp`, `OLED_Mode_RemoteSettings.cpp`, `OLED_RemoteSettings.cpp`, `OLED_Mode_UnifiedMenu.cpp`, `System_Utils.cpp`, `WebPage_Bond.cpp`, and related ESP-NOW sensor code.

---

## 1. What “bond” means here

**Bond** = a **privileged ESP-NOW relationship** with exactly one selected peer: encrypted pairing context, v3 protocol exchanges (capabilities, manifest, settings, heartbeats, remote CLI), and optional sensor streaming. Entry points include **`bondconnect <mac_or_name>`** and the OLED bond picker (same command).

This document does **not** cover other transports.

---

## 2. How ESP-NOW bond mode works

### 2.1 Prerequisites

1. **ESP-NOW initialized** (`openespnow`) and the target device **paired** in the normal ESP-NOW device list (same channel / pairing rules as the rest of the stack).
2. **`bondconnect`** sets:
   - `bondModeEnabled`
   - `bondPeerMac`
   - **`bondRole`**: **higher STA MAC = master** (deterministic so both ends agree).

### 2.2 Roles

- **Master**: CLI describes this side as **display / gamepad**–oriented; requests worker **manifest** and **settings**, controls **sensor enable** and **stream** preferences toward the worker, and receives **sensor data** streams.
- **Worker**: responds with **capabilities** and **settings**, sends **sensor JSON** to the master when enabled; it does **not** mirror the master’s full pull of manifest/settings in the same way.

### 2.3 Liveness and sync

- **Bond heartbeats** (`ESPNOW_V3_TYPE_BOND_HEARTBEAT`): online/offline, RSSI, uptime, settings hash, boot counter (reboot detection).
- **`isBondSynced()`**:
  - **Master**: peer online, valid remote **`CapabilitySummary`**, received **manifest**, received **settings**.
  - **Worker**: has **sent** capabilities and settings to the master.
- **`BondPeerStatus`** (`BOND_STATUS_REQ` / `BOND_STATUS_RESP`): periodic snapshot (uptime, heap, sensor masks, connection/service flags). Local sensor transitions can **proactively** request pushing status to the peer when bonded (`HardwareOne.cpp`).

### 2.4 Session token (`remote:` commands)

Commands prefixed with **`remote:`**, **`remote `**, or **`@`** become **`@BOND:<token>:<command>`** v3 CMD frames. They only send if **`isBondSessionTokenValid()`** is true.

The token is computed **in RAM** from the **ESP-NOW passphrase** and **deterministically ordered MAC pair** (see `computeBondSessionToken` in `System_ESPNow.cpp`). **No passphrase ⇒ no token ⇒ `remote:` fails** (“bond not ready or passphrase mismatch”). That ties remote execution to **shared secret + chosen bond peer**, not merely “any paired MAC.”

### 2.5 Capability exchange

**`CapabilitySummary`** (compact binary) carries firmware hash, feature/service/sensor bit masks, flash/PSRAM, Wi-Fi channel, device name, uptime. OLED/Web use this to show **what the bonded peer supports**.

---

## 3. Bonded peer: functions and abilities

### 3.1 CLI (representative)

| Area | Examples |
|------|----------|
| Connection | `bondconnect`, `bonddisconnect`, `bondstatus`, `bondrole` |
| Discovery | `bondrequestcap`, `bondshowcap`, `bondrequestmanifest`, `bondshowmanifest`, `bondshowremotemanifest` |
| Streaming | `bondstream`, `bondstreamthermal`, `bondstreamtof`, … |
| Remote execution | **`remote:<cmd>`** on the bonded peer |
| Debug | `bondtestsensor`, local manifest dump |

### 3.2 Sensor streaming

Worker → master via **`ESPNOW_V3_TYPE_SENSOR_DATA`** with JSON in the payload (length limited by v3). The master maintains **remote sensor caches** (`System_ESPNow_Sensors.h`); helpers like **remote GPS** consume bonded (or mesh) workers.

### 3.3 Web UI

With **`ENABLE_WEB_BOND`** + **`ENABLE_BONDED_MODE`**: **`/bond`** — configure bond, view status, toggle remote sensors, run remote CLI; output often arrives via **polling** `/api/espnow/messages` (async).

### 3.4 On-disk caches

- **Remote settings**: `/system/espnow/peers/<MAC>/settings.json` — filled during sync for **OLED Remote Settings** (`loadSettingsFromCache`).
- **Remote manifest**: `/system/manifests/<fwHash>.json` — drives **Unified Menu** `[R]` entries when present.

---

## 4. OLED integration (ESP-NOW bond UI)

### 4.1 Bond mode (`OLED_Mode_Remote.cpp`)

- **Not bonded**: picker over **ESP-NOW paired devices** (excluding self); confirm runs **`bondconnect`**.
- **Bonded**: **Status** (peer, online, role, sync, RSSI, heartbeats, caps), **Sensors** (**master only** — `remote:open*` / `close*` plus local **`bondstream*`** prefs), **Swap roles** (`remote:bondrole` then local `bondrole`).
- Uses **`executeOLEDCommand`** so commands match serial/web.

Workers see sensor control deferred to the master; Status still applies.

### 4.2 Remote Settings (`OLED_Mode_RemoteSettings.cpp`, `OLED_RemoteSettings.cpp`)

Loads **cached** peer `settings.json` into the same **settings editor** pattern; applying a value sends **`remote:set …`**. Requires a successful prior sync (otherwise “No remote settings”). Gated by **`ENABLE_OLED_DISPLAY && ENABLE_ESPNOW && ENABLE_BONDED_MODE`**.

### 4.3 Unified Menu (`OLED_Mode_UnifiedMenu.cpp`)

**Local** items vs **`[R]`** remote items: remote entries use **`remote:`** from manifest (if cached) or **capability-based placeholders**. Good for **one-shot CLI**; continuous sensor UIs need separate wiring.

---

## 5. Upsides

1. **One routing primitive**: `remote:` in `System_Utils.cpp` for OLED, web, serial, voice.
2. **Session token** binds execution to passphrase + bond relationship.
3. **Sync + caches** support menus, web, and automation with a structured view of the peer.
4. **Deterministic master/worker** reduces negotiation bugs.
5. **OLED** can manage bond lifecycle without a separate host.

---

## 6. Downsides and shortcomings

1. **`remote:` is often fire-and-send**; full command output may require **message polling** (web). OLED may not show long remote output without similar plumbing.
2. **Passphrase required** for token — easy to end up with bond up but **`remote:`** failing.
3. **Sync latency / retries** → “Syncing…” and empty caches until complete.
4. **Stale files**: settings/manifest on disk can lag real peer state until re-sync.
5. **Worker/master asymmetry**: not a full two-way mirror of each side’s manifest/settings.
6. **Capability vs hardware**: menus may offer **[R]** actions from bits alone; **Bond → Sensors** uses live masks for stricter truth.
7. **ESP-NOW limits**: packet size, throughput, channel alignment — not arbitrary RPC bandwidth.

---

## 7. Toward more “system-wide” bonded behavior (ESP-NOW only)

1. **Uniform remote results**: correlate CMD sends with responses (v3 seq) so OLED/voice can show the same output path as the bond web page’s polling.
2. **Feed `BondPeerStatus` into Unified Menu**: hide or dim **[R]** items when the peer reports sensor not connected.
3. **Refresh cache after `remote:set`**: re-pull or patch `settings.json` so Remote Settings stays accurate.
4. **Streaming UI adapters**: small OLED views driven by **bond stream** + `bondStream*` prefs (not only CLI).
5. **Policy layer on the master**: single “effective sensor” API (e.g. GPS read prefers bonded worker when policy says so) to avoid duplicating branches everywhere.

---

## 8. Summary

ESP-NOW **bond mode** pairs **sync + session token + `remote:`** with optional **sensor streaming** and **file caches** for settings/manifest. The OLED adds **Bond** (lifecycle + master sensor matrix), **Remote Settings** (cached editor), and **Unified Menu** (**`[R]`** commands). The largest gaps for a “native” feel are **symmetric sync**, **fresh caches**, **blocking or polled remote output**, and **streaming UI** beyond one-shot CLI.

---

*Codebase review (May 2026). Scope: ESP-NOW bonding only.*
