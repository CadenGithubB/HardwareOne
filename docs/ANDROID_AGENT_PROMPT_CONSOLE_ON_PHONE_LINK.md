# Prompt for the Android app agent: console on the phone link

Historical prompt for the legacy GATT console attach. It does not describe
the v2 console stream or the current Bluetooth lifecycle commands. For new
Android integration, use the [phone-link v2 peer guide](../tools/phone_link_peer/README.md),
its protocol fixtures and the current [command reference](COMMAND_REFERENCE.md).

Paste everything below the line into the agent that works on
`hardwareone-android-app`.

---

You are working on the HardwareOne Android app (`hardwareone-android-app`, Kotlin;
console in `app/src/main/java/com/hardwareone/console/ble/BleManager.kt` and
`ui/ConsoleViewModel.kt`, relay pipe in `.../pipe/`). The firmware side
(HardwareOne, ESP-IDF) has changed. Make the app work with it as described below.
Do not change the console protocol itself: UUIDs, Secure Channel v1, login and the
reply framing are the same as before.

## What happened

**Since 2.7.0 the app has two separate Bluetooth links to the device:**
- **The app console** (since the start): the phone is the BLE central and GATT client.
  It scans for the command service `12345678-1234-5678-1234-56789abcdef0`
  (request `…de01`, response notify `…de02`, status `…de03`), connects, sets MTU,
  subscribes, runs Secure Channel v1, logs in and sends CLI commands.
- **The relay pipe** (added in 2.7.0, `pipe/*`): the reverse. The device (the
  P4X-EYE board) is the BLE central and GATT client of a GATT server the app hosts
  (`b7e10000-6f7d-4b2e-a8c4-3d9f5021c6a0`).

**The firmware ran only one of these at a time:**
- The console existed only in the device's "server" mode (`openble`), which stops the
  glasses, the R1 ring and the pipe.
- In the normal glasses ("client") mode the device was linked to the phone only as
  the pipe. Android showed the device as connected, but the console never advertised
  and never offered its command service, so the app's scan found nothing.

The user wants the glasses, the ring, the pipe and the app console all working at the
same time.

## What the firmware does now

**In glasses mode, on builds with the pipe, the console runs on the pipe's link:**
- The device registers its command service when the glasses mode starts Bluetooth.
- Once the pipe is ready, the device also acts as a GATT server to the phone on that
  same link. The command service, the characteristics and the protocol are unchanged.
- There is still no advertising and no extra link. The device's 4 BLE links are taken
  by the two glasses arms, the ring and the phone.
- Only the phone's pipe link can use the console. The glasses' and ring's links are
  ignored.

**Rules on that link:**
- **Secure Channel is required.** Plaintext commands get the existing refusal notice,
  and only encrypted replies go out. The device must have `blesecret` set.
- **The MTU is 244** (glasses mode), not 517. The app already sizes encrypted writes
  from the MTU it is told: about 216 bytes of command per write.
- **The console session follows the pipe.** It ends when the pipe leaves ready or the
  link drops. On the next ready it starts as a fresh session, which needs a new
  Secure Channel handshake and `login`.
- **The app joins with its first write.** Until the app writes, the device shows the
  console as "waiting for the phone link" and counts no app client.
- **New Secure Channel REJECT reason `0x03` ("no channel").**
  - The device sends it once per console session when a DATA frame arrives on a
    session that has no channel, for example after the session was ended or restarted
    while the link stayed up.
  - On REJECT `0x03`, run the Secure Channel handshake again (HELLO …), then `login`,
    then resend.
  - The existing reasons stay as they are: `0x01` no passphrase, `0x02` auth failed.
  - The constants are in the app's `SecureChannel.kt` (`T_REJECT = 0x05`, the reason
    in `payload[0]`).
- **When the console is switched off on the device** (`bleconsole off`):
  - the status characteristic `…de03` reads `{"state":"unavailable"}`;
  - writes get no reply.

  Treat that as "console off on the device".
- **`…de01` is write-only** in both modes (it was readable before). The app never reads
  it.
- **`bledisconnect` on the device** ends the console session without closing the link
  (the pipe owns the link).
- **On the device, `bleconsole on|off|status`** turns this on or off (default on).
- **Server mode (`openble`) is unchanged:** advertising, scan by service UUID, MTU 517,
  and no glasses, ring or pipe. The app's existing scan-and-connect path must keep
  working for it, and for boards without the pipe.

## What to build in the app

1. **Remember which device the pipe is connected to.**
   - In `PipeGattServer` / `PhonePipeService`, publish the admitted `BluetoothDevice`
     when the pipe session goes ready, and clear it when the session ends.
   - Keep it in memory only. Never log it, persist it or show the address in the UI.

2. **Add `BleManager.attach(device)` for the console over the existing link.**
   - Call `connectGatt(context, false, callback, TRANSPORT_LE)` on that
     `BluetoothDevice`. Android reuses the open link.
   - No scan. No `requestConnectionPriority` and no `setPreferredPhy`: the device
     manages that link's parameters for the pipe and the glasses.
   - Then `discoverServices`, `requestMtu(517)` (expect 244 back), subscribe to
     `…de02`, run Secure Channel (required here), then `login` and commands.
   - **Timeouts:** about 3 s per setup step. If `onMtuChanged` never arrives, continue
     with the MTU the pipe's GATT server saw, rather than hanging (today the setup has
     no timeout).
   - Handle `onServiceChanged` by discovering again.

3. **Lifecycle.**
   - **Attach** after the pipe reaches ready. Add a setting "Open console when the pipe
     is up", default on.
   - **Detach** when the pipe leaves ready, before the pipe's GATT server retires.
   - **Closing:** close only the console's `BluetoothGatt` client, never the GATT
     server connection the pipe uses.
   - **Reconnect:** instead of the 3-try direct reconnect, re-attach on the next pipe
     ready.
   - **First write:** if the first write after attaching gets no reply, retry it once.

4. **UI.**
   - **Devices page:** show "HardwareOne (via phone link)" while the pipe is ready, as
     a one-tap console entry. Keep the SCAN path for server mode.
   - **Connection card:** in this mode, DISCONNECT becomes "Detach console".
   - **Widget:** a tap uses the attach path when the pipe is ready.
   - **Pipe page:** add a line "Console: attached / waiting for pipe / off".
   - **Errors:** on "Command service not found", tell the user to check that the
     device runs glasses mode with the pipe and that `bleconsole` is on, or to use
     `openble` for a direct connection.

5. **Reply strings to recognise.**
   - `[ble] Login locked out. Retry in N seconds.` (after repeated failed logins)
   - `[ble] Session expired for user '…'` (idle timeout): show it and offer to log in
     again.

## Renamed device commands (no aliases exist)

| Old | New |
|---|---|
| `healthstatus json` | `ring health json` |
| `healthstatus poll` | `ring poll` |
| `ringstatus json` | `ring status json` |
| `phonestatus json` | `phone status json` |
| `phonepipesecret …` | `phone secret …` |
| `phonepipe stats` | `phone stats` |
| `phonepipe start` | `phone start` |
| `phonepipe enroll` | `phone pair` |

- **R1 Health:** the tool is gated on the verb `healthstatus`
  (`ToolGate("R1 Health", "r1health", listOf("healthstatus"))`). Gate it on `ring`
  and send `ring health json` / `ring poll`. Otherwise the device replies
  "Unknown command" and the tool hides itself.
- **Update the user-facing pipe instructions:** `PhonePipeScreen.kt` (around lines
  169, 206, 355), `PipeEngine.kt` (~120), `PipeSecret.kt` (~13) and
  `docs/BLE_RELAY_PIPE.md` (setup section).
- **Pipe pairing** is a shared passphrase (`phone secret` on the device) plus the
  pipe's PSK channel, not an Android bond. The pipe protocol itself is unchanged.
  - If an older build left a bond for the device in Android's Bluetooth settings and
    connections fail, "Forget" it there.
- **`blemode phone`** (which meant server) no longer exists. Use
  `blemode server|client|phone-client`.

## Optional: richer status

`status json` → `connectivity.bluetooth` keeps `running`, `state`, `mode`, `server`,
`client` and `g2Connected`. It now also has:

| Key | Meaning |
|---|---|
| `role` / `roleText` | the role running now, e.g. `glasses-pipe` / "Glasses+pipe" |
| `saved` / `savedText`, `savedDiffers` | the saved mode, and whether it differs from what runs |
| `pipe`, `pipeText`, `pipeReason`, `pipePaired` | the relay pipe: e.g. ready, searching, "fail" + "not found", not paired |
| `console`, `consoleText` | the app console: e.g. "waiting for the phone link", "app signed in" |
| `consoleClients` | apps connected to the console: up to 4 in server mode, 1 on the phone link once the app has joined |
| `advertising` | server mode only (always false on the phone link) |
| `phoneConnected`, `phoneState` | the pipe is ready / the pipe's status line |

`phoneConnected` means the pipe, not the console. You can use these to show the two
links separately.

## Test on hardware first

Use the user's P4X-EYE in glasses mode with the pipe set up, and `blesecret` set.

1. **Attach:** start the pipe, then attach from the new entry.
   - The app finds the command service within about 3 s.
   - The MTU is reported (244).
   - Secure Channel and `login` succeed, and `help` returns its full output.
2. **Everything at once:**
   - The pipe's relay traffic keeps working.
   - The glasses keep showing images and the ring keeps reporting.
   - Run a file listing and a short OTA check while all of this is up.
3. **Teardown:**
   - End the pipe from the device: the console ends too, and re-attaches on the next
     ready.
   - Turn Bluetooth off and on on the phone: nothing hangs.
4. **Server mode:** run `openble` on the device. The old SCAN and connect path still
   works at MTU 517.

Never log or display device addresses, passphrases or keys.
