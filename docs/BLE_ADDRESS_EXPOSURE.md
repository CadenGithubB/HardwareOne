# Device addresses in output, and the G2 temple repair

Status (2026-10-18): the always-on log lines and the ring status fields are
fixed, uncommitted. The rest of this page lists what still shows an address and
needs a decision. Line
numbers are approximate; the files are being edited by several sessions.

## Fixed: always-on logs no longer print an address

| Where | Was | Now |
|---|---|---|
| `G2_Glasses.cpp` `attemptMissingArmRecoverySync` | `Recovery: scanning for missing %s temple (MAC <saved address>, attempt n/7)` on every repair attempt | `(saved-address match, attempt n/7)` |
| `G2_HijackCmd.cpp` stuck-owner warning | `mac1='<saved address>'` | `saved target present (L=saved R=none)`; it now also fires for a right-only pairing |
| `G2_Ring.cpp` `ringPerformConnect` | `BLE connect FAILED after N ms (<saved ring address>)` | `(saved target)` or `(scan target)` |
| `G2_Ring.cpp` `g2RingScan` | `ringscan: found <name> @ <address>` (INFO, on by default) | `ringscan: found <name>` |
| `G2_Glasses.cpp` lens tap lines (4) | the tapped row's text, which on the ESP-NOW and Settings pages contains a MAC | the label goes through `maskMacTokens` |
| `G2_Page_ESPNow.cpp` forget peer | the peer's MAC | its friendly name, then its name, then "(peer)" |
| `System_Utils.cpp` `redactCmdForAudit` | `ringconnect <mac>`, `espnowsend <mac>`, `espnowunpair <mac>` and others printed in `command-audit.log` and the `[CMD]` echo | every MAC is masked as `<mac>` first. This also covers the lens command traces in `G2_HijackCmd.cpp` |

**Dropped by the command rework (your approval, 2026-10-18):**
- `ring status` text no longer prints `addr=`.
- `ring status json` and `/api/ble/status` `ring` have no `addr` key (no `addrKnown` either), and the web Bluetooth page's ring-MAC row is gone.
- The `ring-status` SSE/BLE event has no `a` field.
- The `ring connect <mac>` reply no longer echoes the address.
- The ring identity copy is name-only (`g2RingSetIdentity(name)`).
- Guard: `test_ring_info_status` checks for no address and pins the event and the connect reply.

**Guards:**
- **`test_ble_log_privacy`** (new) scans every always-on log call in the Bluetooth sources for an address argument and pins each fixed line.
- **`test_llm_http_settings`** runs the real `maskMacTokens` and `redactCmdForAudit`. Only a whole six-pair address is masked: not five pairs, not seven pairs, not mixed separators, not one glued to more hex.

## Still showing an address (your call)

Each item below changes behaviour or the UI, not just a log line.

| Where | Exposure | Proposed |
|---|---|---|
| `G2_Ring.cpp` RING_CONNECTED / RING_DISCONNECTED events | Detail = the ring's address. It reaches `events.log`, the `events` command, the G2 SysEvents page and automation matching. | The transition reason, or the ring profile. First check that no automation matches on the address. |
| `Bluetooth.cpp` SYSEVT_BLE_CONNECTED (GATT server mode) | Detail = the central's address | `slot N conn M` |
| `G2_Ring.cpp` ring connect line | `Connected to EVEN R1_<id> …` prints the ring's advertised name, whose suffix identifies the ring (seen in the 498ac7d6 boot log) | Print the name without its suffix, or the profile only |
| `Bluetooth.cpp` `buildBLEInfoJson` `clients[].mac` | Web Bluetooth page (server mode) | `conn #N` |
| `BLE_Peers.cpp` `blePeersWriteJson` `mac1`/`mac2` | `GET /api/settings`, the G2 Settings JSON view and the ESP-NOW settings mirror | Saved/not-saved flags in exports. Keep the stored values: backup and restore need them. |
| `g2info` (`G2_Glasses.cpp`) | Prints both temple addresses; its help text advertises "MAC" | `addr=known/--`. The command-rework session owns command output now. |
| `blestatus`, `blepeers`, `bleautoreconnect` (query) | Print saved or live addresses on request | The same, in the command rework |
| Lens rows (`G2_Page_ESPNow.cpp`, `G2_Page_Network.cpp`, `G2_Page_Settings.cpp`) | Full ESP-NOW, Wi-Fi and BSSID MACs on screen | The tail only (`..xx:yy:zz`), as some rows already do |
| `R1_HealthHistoryStore.cpp` D-line | The ring address in day files | Sealed by default (`capturecrypt=1`); a keyed pseudonym is optional |
| Bluedroid `BT_APPL` traces (IDF code, for example "unable to connect to remote bd_addr") | Serial only, when the stack refuses an open | `esp_log_level_set("BT_APPL", ESP_LOG_NONE)` after BT init. That also hides other BT_APPL errors. |
| Debug-gated (off by default) | `G2_Glasses.cpp` every scanned advert (`[G2-SCAN] advert … addr=`; bystanders' devices included), ring stash, `Connecting to … @ <address>`, auto-reconnect targets; `BLE_Peers.cpp` saved MAC lines; several `G2_Ring.cpp` lifecycle lines | Leave them, or drop the address there too. They only print with `debugg2` or `debugring` on. |

The R1 address can also come from a **bystander's ring**. The G2 scan callback stores the first R1 advert it hears when nothing is stored yet. Those status fields no longer show it (see above), but the stored address can still be a bystander's until your ring connects by its saved address.

## The G2 one-temple repair (fixed: option B)

Status (2026-10-18): implemented, uncommitted, host-tested; not yet tried on
hardware.

**The bug.** Since v0.99.9 (7e347401) the repair never gave up. At the cap,
`missingArmRecoveryComplete` set the next-attempt time to 0, which the tick read
as "start a new episode", so a 7-try cycle restarted every ~13.5 minutes
(about 3% scan duty) and the give-up message could never print.

**What it does now** (the schedule is `G2_RepairCore.h`):
- **Fast tries:** 7 tries at 5, 10, 30, 60, 120, 240 and 300 s, about 13 minutes.
- **Slow tail:** then one try every 10 minutes for as long as one temple stays
  up and you haven't disconnected (about 0.6% scan duty). It logs once:
  "half-connected repair: RIGHT temple still missing after 7 tries, retrying
  every 10 min".
- **Restart:** the surviving temple being put back on (the SYSEVT_G2_PLUGIN_ALIVE edge)
  sends a slow repair back to try 1. A lens gesture (tap, scroll, double tap,
  long press) does too, but at most once per 30 minutes in an episode, so
  steady lens use can't keep the radio scanning (without the limit, about 32
  tries an hour instead of 6). `g2recover` tries at once and restarts at try 1.
- **Ends:** both temples up, both down, or the client stopped, except while a
  try is running. The repair's own connect shows both temples up until the
  new temple's session is promoted (a second or more); if promotion fails,
  that counts as one failed try instead of restarting the schedule.
- **Held:** checked every heartbeat, so it shows within ~5 s. Held means you
  disconnected (`bleautoreconnect g2-glasses off`), the glasses aren't owned
  here, or the surviving temple has no saved address (the try matches the
  survivor against it before scanning). A held repair makes no tries and uses
  none up. A missing side with no saved address is fine: the try matches it
  by name.
- **The ladder:** with `bleautoreconnect g2-glasses on`, the BLE_Peers ladder
  now answers COALESCED while one temple is up, so it polls every 30 s without
  scanning, and takes over once both temples are down. `openg2 saved` and the
  lens "Reconnect G2" don't go through the ladder.

**What you see:**
- `g2status` ends with `repair=none`, `repair=3/7`, `repair=slow` or `repair=held`.
- The `g2-status` SSE has `rp` with the same value, and is pushed (reason
  `repair`) whenever that value changes.
- `/api/ble/status` has `g2.connection.repair`. The web Bluetooth page shows,
  for example, "state: connecting, repair 3/7" or "repair every 10 min".

**Guard:** the `g2_repair_schedule` host test runs the shipped tick, queue,
finish, nudges, status text, ladder admission and `g2recover` over a simulated
day on a fake clock, including a tick that lands while the repair's own
connect is promoting. An independent review found the promotion-window reset,
the gesture rate, the unreachable "no saved target" case and a late-`held`
race; all four are fixed and covered. 49 mutations of the final change were
all caught (`g2_repair_schedule`, `g2_connection_status`, `ring_phone_yield`).
