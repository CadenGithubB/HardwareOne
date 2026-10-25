# Mac test phone for phone link v2 (`hw1peer`)

This tool lets the Mac stand in for the Android phone on the HardwareOne phone link v2, so the
P4 firmware's v2 side can be tested on hardware before the Android app speaks v2.

The roles are the same as with the app:

- **The P4** is the BLE central and GATT client.
- **The Mac** is the peripheral and GATT server. It advertises the pipe service and answers the
  P4's CCCD write with the secure handshake.

The tool speaks every layer the app speaks:

| Layer | Module | What it does |
|---|---|---|
| Outer channel | `channel.py` | HW1-PIPE-v1 |
| Inner protocol | `inner.py` | Version 2 |
| Console | `records.py` | Records on stream slot 2 |
| Relay | `relay.py` | Optional real TCP relay for the P4's relay streams |
| Pairing | `paircode.py` | Pairing code (or passphrase) and the PSK |
| Radio | `gatt_macos.py` | CoreBluetooth, via pyobjc |
| App | `app.py` | The engine, REPL, `cmd.in` and log |

**Status.** Every layer is tested offline (226 tests):

- the firmware's fixtures are replayed byte for byte;
- the full app stack is run in a loopback against a Python port of the firmware's P4 pipe core,
  including its relay and display pressure inputs.

The tool has **not yet run against a real P4**. The hardware script is below.

## Before you flash v2: warnings

- **Flashing the v2 image cuts today's Android app off the phone link.**
  - A v2 P4 never reaches READY with a v1 app (2.7.x). Neither the app's relay nor its console works.
  - After three failed attempts in a row the P4 stops reconnecting and shows "phone app too old".
  - Direct mode (`openble`) still works.
- **Keep the Pixel out of the way during the test.** Turn its Bluetooth off, or force-stop the app.
  - The P4 connects to the first advertiser of the pipe service it hears. That could be the Pixel
    instead of the Mac.
  - If it picks the Pixel, it burns version strikes.
- **`phone pair new` replaces the P4's phone secret and its enrollment.**
  - After you roll back to the v1 image, the Pixel will not connect with its old passphrase on its own.
  - You must re-pair it yourself: on the P4's serial console as super admin, run
    `phone secret <the old passphrase>`, then `phone pair`, then **Pair** in the app.
  - This tool never knows, asks for or stores that passphrase.
- **Rollback reference: `tools/p4/companion/private/flash-2026-10-19a/`.**
  - `image-3fix.bin` is the v1 image (phone link v1 plus design A) that was on the P4 before v2.
    Its SHA-256 is `53c2a9b2…bd40af8`, recorded in `flashed-image.sha256`.
  - `README.txt` records how it was flashed: `write_flash 0x10000` with the hash verified.
  - `rollback/` holds the older `cfc67e42` app image.
  - The firmware session does the flashing, with your approval. This tool never touches serial
    ports or flash.

## Install

The `.venv` already exists. To rebuild it:

```sh
cd tools/phone_link_peer
python3.14 -m venv .venv
.venv/bin/pip install pynacl pyobjc-framework-CoreBluetooth   # PyNaCl 1.6, pyobjc 12.2
```

No other packages are needed. The tests run on the standard library alone.

## First run: the Bluetooth permission

macOS only lets an app that holds the Bluetooth permission use CoreBluetooth. Run the tool from
**Terminal.app**. Claude's shell and editors cannot use it: macOS kills the process.

- **First run:** macOS asks whether Terminal may use Bluetooth. Allow it.
- **If the tool says `Bluetooth permission denied`:** open System Settings > Privacy & Security >
  Bluetooth, switch **Terminal** on, and start the tool again.

## Run

Use one of these:

- Double-click `run.command` in Finder.
- Run `open -a Terminal tools/phone_link_peer/run.command`.
- In Terminal:

  ```sh
  cd tools/phone_link_peer
  .venv/bin/python -m hw1peer             # options: --relay  --no-console  --name NAME  --log FILE  --cmd-file FILE  --no-repl
  ```

  `--no-repl` reads no keyboard commands (for a run without a terminal on stdin): drive it with `cmd.in`
  and stop it with `quit` there, Ctrl-C or SIGTERM. Without it, end of input on stdin (Ctrl-D) quits.

Rules for running it:

- **One instance at a time.** Two instances would publish the service twice, and the P4 refuses that.
  The tool holds a lock (`.hw1peer.lock`, next to it); a second start says `another hw1peer is running`
  and exits.
- **Keep the Mac awake, open and on power.** While it runs, the tool holds a macOS activity that stops
  idle sleep and timer coalescing (`pmset -g assertions` lists it as `PreventUserIdleSystemSleep`,
  "HardwareOne phone link test"). Closing the lid still sleeps the Mac, which stops the radio: the P4
  then loses the link, and that would look like a firmware failure. This matters most in H9 step 3.
- **Nothing is persisted.** After every start, type `code` again.
- **Advertising starts only once a secret is held.** The tool advertises the 128-bit service UUID in
  the primary advertising data, which is where the P4's passive scan looks. It also sends a short
  name, `HW1Peer` (a name is sent only if it fits: 8 bytes at most).
- **Advertising stops while a P4 is connected.**

### Commands

| Command | What it does |
|---|---|
| `status` | Bluetooth, advertising, link, session, P4 console offer, our flags, streams, console INFO, relay |
| `stats` | Counters: values in and out, stack busy retries, frames, LINK_STATE, PING, console, relay |
| `code` | Type the pairing code (or a passphrase) at the hidden prompt (echo **off**). Only the derived PSK is kept, in RAM. Anything typed after `code` on the same line is ignored and never shown |
| `login [user]` | Sign in on the P4 console. The password is read with echo off; the line is never printed or logged. `login <user> <password>` on one line is refused (the password would be on screen) |
| `cmd <line>` | Send one CLI line to the P4 console. Replies print as `P4> …` |
| `relay on\|off` | RELAY_ON (default off). Off refuses relay OPENs with 16 and ends live relay streams with 16 |
| `console on\|off` | CONSOLE_WANTED (default on). A passphrase secret never takes the console (§6.2 #7) |
| `adv on\|off` | Advertise the service (default on) |
| `mismatch on\|off` | Answer each new session's HELLO with the version signal `01 25 0000 00000000 0200 03 00 0200` |
| `ping` | Send a PING |
| `end [code]` | End the session with a stream-0 RESET (default 18 paused) |
| `forget` | Wipe the secret, end the link (RESET 18 if READY; at once if not, so it never reaches READY with the old key), stop advertising |
| `quit` (or Ctrl-C) | Send RESET 18 if a session is up, stop advertising, remove the service, exit. A second Ctrl-C exits at once. SIGTERM, and closing the Terminal window (SIGHUP), quit the same way |

At a hidden prompt (`code`, `login`), **Ctrl-D cancels the prompt**. Ctrl-C there only prints
`Ctrl-D cancels the prompt; press Ctrl-C again to quit the tool`; a second Ctrl-C quits.
A line whose first word is not one of these commands is never echoed or logged (it could be a secret
typed in the wrong place): the tool only says `unknown command (not shown; type help)`.

### Output

- **Console records** print with a prefix:
  - `P4 INFO>` (the stream's first record)
  - `P4>` (TEXT, one line per output line)
  - `P4 CARD>`
  - `P4 MIRROR>`
  - `P4 BLOB>` (size and header only)
  - `P4 ABORT>`
- **Link events** print as plain lines: `pipe READY …`, `sent LINK_STATE …`,
  `refused an OPEN from the P4 with 16 …`, `link ended: …`, `relay: stream N …`.
- **When our side ends a link the P4 still holds** and no frame can tell it (a P4 value that fails
  authentication, the 5 s stuck-notification or 75 s silence watchdog, `forget` before READY), the
  tool removes and re-adds the pipe service, so the P4 sees Service Changed and stops at once. It
  prints `re-added the pipe service so the P4 sees Service Changed and stops now …`. A Mac peripheral
  cannot disconnect a central, so if the P4 misses that, it notices only through its own idle PING:
  up to about 65 s (60 s idle, then 5 s for the PONG). This kick has not yet been seen on hardware.

### `cmd.in` and `peer.log` (for Claude)

- **`cmd.in`:** the tool tails this file, next to it, after it starts. Each appended line is one
  command, for example `echo status >> tools/phone_link_peer/cmd.in`.
  - Old lines are never replayed.
  - The tool echoes each command as `[cmd.in] > …`, redacted (`code` and what follows as `code ***`).
  - A line whose first word is not a command is not echoed: `unknown command from cmd.in (not shown; …)`.
- **`peer.log`:** every printed line is appended here, with a timestamp. It is created readable by you
  only (mode 0600; an older log is tightened when the tool opens it).
- **Secrets never go through the file.** `code`, `login` and `cmd login …` from `cmd.in` are refused.

Masking (best effort):

- **On screen and in the log:** `login <user> <password>` and anything that parses as a pairing code
  are masked.
- **Logged command lines** also get the firmware's own redaction rules: `phone secret`, `wifiadd`,
  `llmhttpurl`, and so on.
- **The log file** additionally masks IP and MAC addresses and SSID values: `ssid:`/`ssid=`/`SSID:`
  fields, and the single-quoted names in the firmware's Wi-Fi messages (`wifiscan` rows with `RSSI=` and
  `BSSID=`, `Connecting to '…'`, saved-network rows `1. [2] '…'`, and the like). A dotted version
  after `fw`, `v` or `version` (the P4's `"fw":"0.99.98.4"`) is kept. The screen shows SSIDs as they are.
- **Relay logs** name streams and address types only, never a host or port.

## Hardware test script (P4 running the v2 image)

**Before you start:**

- The P4 runs the v2 image. firmware-009 §3 names it: `p4-phone-pipe-camera-off-test`, SHA-256
  `98946d40…ee3b9`.
- The Pixel's Bluetooth is off.
- The tool is running in Terminal. "P4 serial" means the P4's serial console, signed in as super admin.

### H1: pairing by code

1. **Tool:** after the start-up lines, it says `No secret yet`.
2. **P4 serial:** `phone pair new`. It prints `Pairing code (shown once): XXXX-XXXX-XXXX-XXXX` and
   opens a 3-minute window.
3. **Tool:** `code`, then type the code at the hidden prompt. Dashes, case and spaces do not matter.
   - Expect `Pairing code accepted …` and `advertising the pipe service: waiting for the P4`.
4. **Tool:** within the window, expect these lines in order:
   - `P4 subscribed (ATT MTU …)`
   - `secure channel up`
   - `pipe READY: frame limit …; the P4 offers the console; our flags: relay off, console wanted`
   - `console: open (stream N)`
   - `P4 INFO> {"v":1,"fw":…}`
5. **P4 serial:** `phone status` shows the link ready, the console offered and open, and relay off.
   Also run `phone stats`.
6. **Tool:** `status` and `stats`. Note the ATT MTU and the frame limit.
7. **Optional, H1b: a link that offers no console.**
   - **P4 serial:** `phone secret <a passphrase>`, then `phone pair`, which opens a 60 s window.
   - **Tool:** `forget`, then `code` with the same passphrase.
   - **Expect:** `pipe READY …; the P4 does not offer the console; our flags: relay off, console not wanted`.
   - **Afterwards:** repeat steps 2 to 4 to go back to a device code.

### H2: console login and help with glasses and ring up

1. **P4:** in glasses mode (`blemode glasses`), with the G2 and the R1 connected (`blestatus`).
2. **Tool:** `login`, then type the user and the password. The password is hidden, and the tool
   never prints or logs it. The P4's reply prints as `P4> …`.
3. **Tool:** `cmd help`.
   - **Expect** several `P4>` batches of whole lines, then `P4> OK`, not a bare OK.
   - **Then:** `cmd help all`.
4. **Tool:** `cmd blestatus`. The glasses and the ring are still connected.
5. **Tool:** repeat steps 3 and 4 a few times. Watch the lens for responsiveness meanwhile.
6. **Check:** `peer.log` contains `> login (hidden)` and never the password.

### H4: `bledisconnect`, `bleconsole off/on`, idle logout

1. **P4 serial: `bledisconnect`.**
   - **Serial:** answers "App console session ended; the phone link stays and the console starts a
     new signed-out session".
   - **Tool:** `console: closed: the P4 closed it (CLOSE 2 renewed: Session renewed)`, then at once
     `console: open (stream N+1)` and a new `P4 INFO>`.
   - **Tool:** `cmd whoami` shows the console signed out. `login` again.
2. **P4 serial: `bleconsole off`.**
   - **Tool:** `P4 LINK_STATE: it no longer offers the console` and
     `console: closed: … (CLOSE 3 console_off …)`, in either order.
   - **Tool:** `status` still shows the session ready. The link stays up.
3. **P4 serial: `bleconsole on`.**
   - **Tool:** `P4 LINK_STATE: it offers the console`, then `console: open`.
4. **Tool:** `console off`.
   - **Tool:** `sent LINK_STATE: relay off, console not wanted`, then the P4's `CLOSE 4 not_wanted`.
   - **Tool:** `console on` reopens the console.
5. **Idle logout.**
   - **P4 serial:** `sessionidleble 1`.
   - **Tool:** `login`, then do nothing for a little over a minute.
   - **Expect:** `P4> [ble] Session expired …`. The stream stays open, and `status` still shows the
     session ready.
   - **Restore:** `sessionidleble 15`.

### H9: phone-client mode, and the relay

1. **P4 serial: `blemode phonelink`.** Follow the reply's hint: `phone start`, or a reboot.
2. **Tool:** pipe READY and the console open, as in H1 step 4. `login`, then `cmd help`.
3. **Relay off with the link up.**
   - **Tool:** `relay off` (the default).
   - **P4 serial:** `relay status` shows `relay=off`.
   - **Leave it idle for 3 minutes or more**, with the Mac open and on power (see Rules). The link
     stays READY: `stats` shows `PING in` growing, and there is no `link ended`.
4. **Relay on.**
   - **Tool:** `relay on`. Expect `sent LINK_STATE: relay on, console wanted`.
   - **P4 serial:** `relay status` shows `relay=on`.
5. **Optional: a real TCP target.**
   - **Needs:** the P4's HTTP agent source configured (`llmhttpenabled 1` and an `https://` `llmhttpurl`
     that the Mac can reach).
   - **P4 serial:** `wifidisconnect`, so the phone path is used. Then make one Chat request.
   - **Tool:** expect these lines, with no host or address:
     - `relay: stream N OPEN to a DNS name target`
     - `relay: stream N connected`
     - `relay: stream N ended (… B to TCP, … B to the P4)`
   - **P4 serial:** `relay status` shows the request went through the phone. `openwifi` afterwards.
6. **Tool:** `relay off` while a relay stream is open ends it with 16. The link and the console stay up.

### H10: version mismatch

1. **Tool:** `mismatch on`, then `end`. This ends the current session; the P4 reconnects.
   - **Expect**, three times: `answered the P4's HELLO with the version signal (mismatch mode)` and
     `link ended: we sent the version signal …`.
   - **Tool:** `status` shows `version signals sent 3`.
2. **P4 serial:** `phone status` reads "phone app too old". The P4 stops reconnecting, so the tool
   stays advertising with no new subscription.
3. **Tool:** `mismatch off`.
4. **P4 serial:** `phone connect` lifts the latch. `phone start`, `phone pair` or a reboot also
   would. **Tool:** pipe READY again.
5. **Optional, the other direction, only after a rollback to v1 and before you re-pair the Pixel:**
   the tool answers a v1 HELLO with the version signal by itself (§6.2 #8).

**Afterwards:** quit the tool (`quit`), turn the Pixel's Bluetooth back on, and decide on the
rollback (see the warnings).

## Troubleshooting

- **The P4 never subscribes.**
  - **P4:** check `phone status` and `phone stats`; the scan records show whether the advertisement
    was heard.
  - **Tool:** `status` must show `advertising yes`. Advertising needs a secret, Bluetooth on, and no
    current link.
  - **The Pixel:** check it is not advertising the pipe.
- **`the P4 refused the handshake (REJECT 2)`:** the code or passphrase differs from the P4's.
  - The P4 then refuses new attempts for 30 s.
  - Type `code` again.
- **The terminal stops echoing after a cut-short prompt:** type `reset`. The tool restores the
  terminal's settings on exit.
- **The P4 logs Service Changed:** the tool was restarted or quit while connected, or it kicked a
  link its side had ended (see Output). This is harmless; the P4 reconnects.
- **`phone stats` on the P4 shows no matching advertisement:** restart the tool with `--name ''`, so the
  advertisement carries the service UUID alone (the P4's passive scan never reads the name).
- **`the pipe service was refused`:** the tool retries every 5 s, five times. If it gives up, it says
  so: toggle Bluetooth off and on, or restart the tool.
- **`another hw1peer is running`:** quit the other one (its Terminal window), then start again.

## Offline tests

```sh
cd tools/phone_link_peer
.venv/bin/python -m hw1peer.tests              # everything (about 15 s)
.venv/bin/python -m hw1peer.tests -v test_app  # one module, verbose
```

| Test module | What it covers |
|---|---|
| `test_paircode` | Pairing code and PSK |
| `test_channel` | Outer channel: golden handshakes, X1, X2 |
| `test_inner`, `test_x1`, `test_x2` | Inner v2 rules and fixture replay |
| `test_records` | X4 console records |
| `test_app` | The full app over the fake GATT, against the P4 model: handshake, console, relay refusal and real localhost TCP relay, LINK_STATE, PING, mismatch, back-pressure, the P4's display and relay pressure, reconnect, watchdogs and the Service Changed kick, service retries, `forget`, masking, `cmd.in`, prompts and Ctrl-C, and `main` (lock, signals, shutdown, `--no-repl`). It also replays X1's first nine frames through the app byte for byte at all four MTUs |
| `test_relay` | TcpRelay against a bare session and localhost sockets |
| `test_gatt` | GATT rules, an import-only check of the pyobjc layer's selectors, `reset_service` against a fake manager, and the keep-awake assertion (via `pmset`) |

The test support lives in `tests/fake_gatt.py` (the fake peripheral and the bench) and
`tests/p4_model.py` (the P4 model).
