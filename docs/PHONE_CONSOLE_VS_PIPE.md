# Phone: relay pipe and app console

Historical design A notes. The legacy GATT console attach remains supported,
but phone link v2 also carries its own console stream using a device-made
pairing code. The current boot roles are `client|server`; `openble` starts the
shared client stack and `openconsole` starts the direct app-console server.
Use the [phone-link peer guide](../tools/phone_link_peer/README.md) and
[command reference](COMMAND_REFERENCE.md) for the current integration.

Status (2026-10-18): design A is implemented in the working tree (uncommitted,
host-tested, not yet on hardware). In G2 client mode the glasses (two links),
the R1 ring and the relay pipe run together, and the app console rides the
pipe's own link. Server mode (`openble`) is unchanged. Line numbers are left
out on purpose; several sessions edit these files.

Where things live (all in `components/hardwareone/`):

| What | File |
|---|---|
| Status words every surface shows | `BLE_LinkStatusCore.h` (pure), one snapshot in `BLE_LinkStatus.h` |
| Which link may hold a console slot, and when | `BLE_ConsoleAttachCore.h` (pure) |
| The pipe's link record | `phonePipeLinkRecord()` in `BLE_PhonePipe.h` |
| Registration, admission, Secure Channel, TX gate, commands | `Bluetooth.cpp` |
| Teardown hooks | `bluetoothHalDeinit()` in `HAL_Bluetooth.cpp` |

## Two roles, one phone link

| | Relay pipe | App console |
|---|---|---|
| Who is central | the P4 | the phone (HardwareOne app) |
| Who hosts the GATT server | the Android app (`b7e10000-…`) | the P4: command service `12345678-…def0` (`…de01` request write, `…de02` response notify, `…de03` status read) |
| Used for | relay, LLM HTTP fallback, Terminal | CLI commands, `#NOTIF` device cards, the console mirror |
| Runs in | phone-client mode, and beside the glasses in G2 mode | server mode (`openble`), and in G2 mode on the pipe's own link |

The console never runs in phone-client mode (`phonePipeStart(true)`): that host
never registers it.

## How the console rides the pipe's link (design A)

**Registration, once per client host.**
- `initG2Client` starts a fresh host, sets up the client MTU (244) and scan, and
  then calls `bleConsoleRegisterOnClientHost()`, before any scan or connect and
  before the glasses runtime, ring, setup worker and pipe start.
- It registers the command service only: no device-info or data service, no
  advertising, no MTU raise, no TX power change. With no link open, starting the
  service sends no Service Changed.
- **Never create or start another GATT service later in client mode.** Every
  open link (temples, ring, phone) would be sent Service Changed, and a peer
  that never confirms trips Bluedroid's 40 s timeout handler, which misreads its
  own data.
- A failed registration only logs; the glasses, ring and pipe are unaffected.
- `bluetoothHalDeinit()` is the one teardown point. Before the host stops it
  closes admission, ends every console session and drops the GATT pointers;
  after a terminal stop it frees the console state (a failed stop keeps it,
  closed).

**Admission: only the ready pipe's link.**
- Bluedroid tells the server about every link. In client mode a connect admits
  nothing, and a disconnect ends only a slot on that exact connection id. Events
  for any other id (temples, ring) return before taking the lifecycle lock.
- `bleConsoleReconcile()` (from `bleUpdateStreams`, `bleconsole` and every
  write on the request characteristic) reads `phonePipeLinkRecord()`:
  `{generation, connId}`, valid only while the pipe is authenticated and READY.
  It makes one slot (typed Phone, a fresh session, no address) on that
  connection id, or ends it. A write reconciles before it is admitted, so a slot
  whose pipe left READY takes no command meanwhile.
- The slot is a session waiting for the app until the app joins it with its
  first admitted write: only then is it a connected app (connection state,
  counts, the BLE connect event, the `connected` word). A slot no app joined
  ends without an event, so a pipe going READY and dropping is no console
  notification.
- The slot is found by the pipe's live connection id (server and client share
  one per link), never by the phone's rotating address. A pipe generation whose
  link has gone is never attached again, so a reused connection id never
  inherits a session.
- The phone link's audit identity is `ble:pipe-link`.

**Security and link ownership.**
- Secure Channel only: no slot without `blesecret`, plaintext commands are
  refused with the fixed refusal notice, and only encrypted output goes out. The
  P4's encrypted frames (at most 225 bytes) fit the 244 MTU; commands are limited
  to about 216 bytes per write.
- A session renewed under the live link (`bledisconnect`, `bleconsole` off then
  on, `blesecret` cleared and set again) holds no channel while the app still
  holds its old one: the app's next DATA frame gets `SC_REJECT` reason `0x03`
  ("no channel"), once per session, and the app runs the handshake again
  (`docs/APP_JSON_CONTRACT.md` sec. 11.8).
- The request characteristic `…de01` is write-only (Arduino keeps the last
  write as its value, and Bluedroid checks the permission, not the properties,
  on a read). The status read `…de03` answers the slot's link only; every other
  link reads `{"state":"unavailable"}`.
- The console never calls `esp_ble_gatts_open`, so it never holds the link up.
  The pipe owns the link and can drop it; the console session ends with it, and
  a new session attaches on the next READY.

**Airtime.** Every console frame takes one try of the shared central TX gate
(`BLE_CentralTx.h`) and never waits for it: replies skip the glasses' and ring's
soft priority, best-effort frames (mirror, notification cards) yield. Console
writes and sent replies count as pipe activity (mirror and card frames do not),
so the link uses its active parameters and returns to idle 30 s after the last
console traffic.

**No lifecycle lock across a post.** `esp_ble_gatts_send_indicate` waits forever
for room on BTC_TASK's queue, which on this host carries every link, and
BTC_TASK takes the lifecycle lock for the phone's writes and link events. So on
the client host a console frame's session is checked under the lock and posted
after it is released, counted in flight; the host teardown waits for posts in
flight before the host stops. (A frame that lands on a reused id is
ciphertext, as one queued behind that id's link events could be anyway.)
Server mode keeps posting under the lock.

**App ids.** The console server registers an id from the reserved 15-bit
allocator the clients use (`BLEDevice::allocateAppId`), so neither a long
uptime of client connects nor a client reusing its id can stop it registering.

**Commands.**

| Command | Server mode | G2 client mode |
|---|---|---|
| `openble` | starts the server | switches to server mode for a direct app connection (stops glasses and pipe) |
| `closeble` | stops the server | stops the relay pipe; the console on its link ends with it, the glasses keep running |
| `bledisconnect [connId]` | disconnects app clients | ends a joined console session only; the pipe keeps the link and a fresh session waits for the app |
| `bleadv`, `blestream`, `bleevent` | as before | refused (no advertising, no data service on that host) |
| `blesend` | to the connected app | to the attached console |
| `bleconsole on\|off\|status` | (used in glasses mode) | persisted switch `bleConsoleOnPipe`, default on; applies at once |

`blemode client` is the glasses, the relay pipe and the app console via the
phone link; `blemode server` is the direct app connection.

## Status words

Every surface takes them from `BLE_LinkStatusCore.h`; JSON ids in
`docs/APP_JSON_CONTRACT.md` sec. 11.

| Console state | Short (OLED, lens) | Long (CLI, web, JSON `consoleText`) | Id |
|---|---|---|---|
| Not available | `n/a here` | `not available in this mode` | `not-here` |
| Off | `off` | `off` | `off` |
| Server, advertising, no app | `waiting` | `waiting for the app` | `waiting` |
| Server up, advertising off | `hidden` | `not visible (advertising off)` | `hidden` |
| Phone link, the app not joined | `waiting` | `waiting for the phone link` | `waiting-phone` |
| Phone link, no `blesecret` | `no secret` | `set blesecret first` | `no-secret` |
| An app connected, nobody signed in | `signed out` | `app connected, signed out` | `signed-out` |
| An app signed in | `signed in` | `app signed in` | `signed-in` |

In G2 mode: `off` when `bleconsole off`; `not available` only when the console
is not registered on the host (a build without the pipe, or a failed
registration). A slot counts as connected only once the app has joined it and
while the pipe is ready, so a session still waiting for the app, or a slot the
old link left, reads `waiting` and is not counted.

| Surface | G2 mode example |
|---|---|
| OLED Bluetooth status | `Role: Glasses+pipe` / `Glasses: connected` / `Ring: --` / `Pipe: ready` / `Console: signed in` |
| OLED Bluetooth menu | `End console` (runs `bledisconnect`) while attached; no Advertising row. The App console row confirms `Switch to server?` / `Stops glasses/pipe` |
| OLED Phone screen | `PIPE [OK] paired` / `ready` / `Console: signed in` |
| Lens Phone >> page | `Pipe: ready` / `Paired: yes` / `Console: signed in` |
| Lens status corner | `PH: ready` (the pipe only) |
| Web Bluetooth page | `App console: app signed in`, without the server's clients/advertising meta; "Use app console" confirms "Switch to server mode for a direct app connection?" |
| Dashboard card | `Console: waiting for the phone link` / `app signed in` |
| `blestatus` | `Role: Glasses+pipe` / `App console: app signed in` / `Relay pipe: ready (see 'phone status')`; no `Clients:` line; a hint only while not attached |
| `phone status` | `pipe=ready role=with-glasses paired=yes auto=on console=signed-in state='ready'` |
| `status json` | `connectivity.bluetooth.console` = `signed-in`, `consoleClients` = 1, `advertising` = false |

`blestatus` hints outside the console server:

| Role | Console | Hint |
|---|---|---|
| Glasses / Glasses+pipe | `waiting-phone`, pipe not ready | the app console attaches to the phone link once the relay pipe is ready; see 'phone status' |
| | `waiting-phone`, pipe ready | the phone link is ready; the app console attaches when the app opens it |
| | `no-secret` | the app console on the phone link is Secure Channel only; set 'blesecret' first |
| | `off` | 'bleconsole on' attaches the app console to the phone link |
| | `not-here` | 'openble' switches to server mode for a direct app connection (stops the glasses [and pipe]) |
| | `connected`, `signed-in` | none |
| Phone pipe | | 'openble' switches to server mode for a direct app connection (stops the pipe) |
| Off | | to start the app console, run 'openble' |

## Still open

Needs hardware:
- Android `connectGatt` joins the existing link, finds the command service and
  reports MTU 244 on the target phone; the de03 read succeeds; relay traffic
  keeps flowing.
- Glasses image pushes and ring readings are unaffected; the connection
  interval stays as before; ending the pipe from the P4 drops the link with the
  console attached.
- The registration log line, no Service Changed, and the GATT app slot count
  during reconnect churn.

Firmware:
- `bleinfo` and the `blestatus` client table list server-mode clients only; the
  phone-link session shows in the link words.
- Phone-client mode has no console.
- The first `initG2Client` of a boot with `blesecret` set pays the PBKDF2
  derivation; the `ble_setup` task's stack headroom with registration added is
  unmeasured.

App (Android): attach on pipe "ready" (no scan, no priority or PHY requests,
about 3 s per setup step), detach when the pipe leaves "ready", show "Console
via phone link", and on `SC_REJECT` reason `0x03` run the handshake and
`login` again (sec. 11.8 of the app contract).

## History (short)

- The 2026-10-18 audit, written against HEAD c0384687, found the two links
  exclusive: the console server only existed in server mode, which resets the
  host and advertises, and G2 plus the pipe already use all four C6 links. Its
  status-word proposal (option O1) shipped first, with "n/a here" for the
  console beside the glasses.
- Its smaller findings are fixed in the tree: the quick-settings stop runs
  `phone stop`, the OLED and lens have phone rows, `phone status` says
  `pipe=off`, `blestatus` no longer points at `openble` from the glasses,
  `populateBtMenu` reads the link snapshot, pipe failures use short reasons,
  advertising is re-armed while another app fits, and both service UUIDs are
  advertised.
- The feasibility review then showed Bluedroid serves a GATT server on a link
  where the P4 is central, server and client share the connection id, and
  registering at host start sends no Service Changed. That made design A (the
  console on the pipe's own link: no fifth link, no advertising, no host reset)
  the choice over design B (the console carried inside the pipe, a protocol v2
  on both sides).
- A three-way review of the built design then fixed: a console post held the
  lifecycle lock across a BTC queue post (a stall of every link), a READY with
  no app counted as a connected app, a renewed session left the app on a dead
  channel, a slot took commands after its pipe left READY, `…de01` was
  readable and `…de03` answered every link, the server's GATT app id was not
  reserved, and builds without the pipe named it in `closeble` and help.
