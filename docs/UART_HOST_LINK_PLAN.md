# UART Host Link Plan — CM5 ↔ hardwareone command channel

**Goal:** a Linux host (Raspberry Pi CM5) drives this firmware over UART0
(GPIO43/44 = XIAO D6/D7 = FeatherS3 TX/RX header pins). UART-only by carrier
design (no USB between the boards). The USB-CDC bench console must keep
working untouched.

**Status: IMPLEMENTED 2026-08-07, uncommitted, awaiting hardware test.**
S3 (FeatherS3) build green; every touched file also compiles for classic
ESP32. Reviewed by a 5-agent adversarial pass over the diff; all CRITICAL/
HIGH/MEDIUM findings fixed (see §6). **OTA-layout build NOT verified** — it
requires the RSA signing key (HW1_OTA_SIGNING_KEY); sdkconfig.ota was edited
and normalized by hand, so run one OTA build before any OTA flash.

**Original status: PLAN ONLY.** Investigated 2026-08-07 by a
4-agent workflow (console mechanics / output routing / UART transport /
settings+auth); all file:line refs verified against the working tree, which
carries the uncommitted settings-integrity changes (+271 lines in
System_Settings.cpp — line numbers here reflect that state).

**Adversarially re-verified 2026-08-07** by a second, 5-skeptic workflow
(per-board impact / console move / architecture / auth completeness /
budget+policy). Sections below are amended in place with its corrections;
§5 records the verdicts, the new hazards it surfaced, and the accepted
residual risks. The single biggest correction: **this project supports SIX
board configurations, three of them classic ESP32** (QT Py ESP32 Pico,
Feather ESP32 V2, Generic ESP32 — see boards/*.defaults and
System_BuildConfig.h:774-998), and on classic ESP32 `Serial` IS `Serial0`,
so the unguarded feature would have destroyed the console there. Resolution
(user decision): the feature supports ALL SIX boards via per-board
UART/pin parameterization — classic boards bind UART1/UART2, never the
console's UART0. See Phase 3.

---

## 1. The headline findings

1. **UART0 is free at the app level but dirty at the system level.** No
   firmware code touches UART0/Serial0 anywhere (grep-clean), and no board
   block claims GPIO43/44. But sdkconfig routes the **IDF primary console** to
   UART0 @115200 (sdkconfig.esp32s3:1546-1557): ROM boot banner, bootloader
   logs, app ESP_LOG, and panic dumps all transmit on GPIO43 today.
2. **The USB console and a UART link coexist by construction.** `Serial` is
   HWCDC (USB-Serial-JTAG) via CONFIG_ARDUINO_USB_CDC_ON_BOOT=y; `Serial0`
   (UART0) is a permanently-instantiated separate object
   (HardwareSerial.cpp:43-44) — opening it is one `begin()` call. Nothing
   about the bench console changes.
3. **The clean-channel architecture already exists in-repo: MQTT.** The MQTT
   bridge (System_MQTT.cpp:521-560) is the proven "transport, not sink"
   shape: build a Command, `submitAndExecuteSync`, and the transport itself
   writes the result — `outputMask=MSG_ROUTE_FILE` means **no broadcast/debug
   line can ever reach the channel**. This needs zero routing-system changes
   and doesn't spend the last free routing bit (0x80 is the only one left in
   the uint8_t `DebugMessage::routing`).
4. **Identity/authorization comes free.** The serial drain does no identity
   work — `executeCommand` installs per-task TLS identity itself
   (CommandIdentityScope, System_Utils.cpp:4597). A UART drain that builds an
   AuthContext and submits through `submitAndExecuteSync` inherits the whole
   auth stack.
5. **ORIGIN_SERIAL is load-bearing security** — it is proof of physical
   presence for cmdOtaPin/cmdOtaResetJournal (System_CommandTypes.h:53-66).
   The UART channel MUST get its own `ORIGIN_UART` + `SOURCE_UART` and its
   own session globals; reusing serial's would hand a compromised CM5 the
   physical-console OTA surface and fuse the two login sessions.
6. **Bench testing needs no carrier:** the FeatherS3 exposes GPIO43/44 as its
   TX/RX header pins (variants/um_feathers3/pins_arduino.h:13-14) — the whole
   link is testable today with a 3.3V USB-UART dongle on the bench board.

## 2. Decisions to make before implementation

**D1 — Console fate (the big one).**
- **Option A (recommended): move the IDF console to USB-Serial-JTAG.**
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in sdkconfig.defaults.esp32s3;
  UART0 becomes app-owned and clean. Cost: panic dumps and bootloader logs
  leave the CM5 link — a crash looks like a link reset. Mitigation: the
  crash-history subsystem (crashRecordEmitEarly, HardwareOne.cpp:1341)
  records panics; the CM5 daemon queries it after every reconnect.
- Option B: keep console on UART0 at link baud (ESP_CONSOLE_UART_CUSTOM,
  921600). CM5 sees panics live, but console bytes interleave into protocol
  frames (the documented "PSRAM→PS7391" failure, System_Debug.cpp:1053-1057);
  requires CRC framing and a boot-armed `loglink`. Not recommended.
- Either way the **ROM first-stage burst at 115200 on every reset is
  unremovable** (eFuse-gated) — the CM5 parser needs resync-after-garbage
  regardless.

**D2 — Auth mode.** Recommended: `uartRequireAuth=1` + a dedicated real
account (e.g. `useradd cm5 <pass> 0 user`). The AuthBypass path
(uartRequireAuth=0) is non-admin but NOT read-only (can run every
non-admin command, answer pending confirm prompts — docs/AUTH_SECURITY_REVIEW.md:773),
and two CONFIRMED-open backlog holes allow a *real* account named AuthBypass
to be created (FTS even as superadmin). Session login beats MQTT-style
per-message credentials for a persistent daemon (PBKDF2 is ~12s per login).

**D3 — Account tier.** `user` tier covers the expected CM5 workload:
imuread/gpsread/tofread/thermalread, sensorlog, healthstatus, ringquery/
ringstatus, espnowstatus/list/send/messages, status, time — all
requiresAdmin=false. **`files`/`fileread` are admin-gated**
(System_Filesystem.cpp:1268,1277). If the CM5 must pull files (health CSVs,
recordings), either grant admin (weakens the isolation story) or add a
scoped read capability later. Decide by actual need; start with `user`.

**D4 — Streamed output.** Request/response only (Option A) for v1. The
MSG_ROUTE_UART sink bit (Option B in the routing findings) is only needed if
the CM5 wants live debug/event streaming; it burns the last routing bit and
inherits the 255-byte clamp. Skip unless a concrete need appears;
`captureOutput=true` (HardwareOne.cpp:742-782) already folds a command's
streamed lines into its 4KB reply for human-ish commands.

**D5 — Baud.** 921600 (divider-exact from the 40MHz XTAL source the HAL
forces, immune to power-save CPU scaling — esp32-hal-uart.c:914-916).
2,000,000 is also exact; 3M has 0.16% error and exceeds nothing on the CM5
side (PL011 caps at 3M). Start at 921600, make it a setting.

## 3. Implementation plan (Option A shape)

### Phase 1 — sdkconfig console move (recipe CORRECTED by verification round)
- The original recipe was WRONG: `idf.py fullclean` never regenerates
  sdkconfig, and defaults cannot override values already present in an
  existing sdkconfig (acknowledged in-repo at CMakeLists.txt:294-295); the
  board-key auto-recovery pass carries no ESP_CONSOLE keys. Following the old
  recipe would have shipped both builds still spraying the console on UART0.
- Correct procedure: **hand-edit the LIVE build configs** — `sdkconfig` and
  `sdkconfig.ota` — replacing the CONFIG_ESP_CONSOLE_UART block with
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (+ `# CONFIG_ESP_CONSOLE_UART_DEFAULT
  is not set`, UART_NUM=-1, SECONDARY_NONE; the updater configs show the
  resolved shape, updater/sdkconfig.feathers3:1312-1320), then build and
  **verify the console block in the generated config** for both layouts.
- ALSO add the line to sdkconfig.defaults.esp32s3 so future fresh
  regenerations agree, and re-export the audit snapshots
  (sdkconfig.esp32s3, *.latest) so later file:line audits don't read stale
  console state.
- Scope: the symbol only exists for S3 targets — classic-ESP32 boards keep
  their UART0-through-USB-bridge console untouched (it's their only console).
- Bench impact is smaller than originally framed: panics/logs ALREADY mirror
  to the USB console today via CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y
  (sdkconfig.esp32s3:1552) — the only observable S3 delta is UART0 going
  quiet, plus 2nd-stage bootloader logs relocating to USB at boot.
- **Phase-ordering is load-bearing:** flashing Phase 3 code onto a device
  whose config still has the UART0 console re-baud-rates the live console
  mid-output and interleaves panic dumps into the CM5 link. Phase 1 and
  Phase 3 ship in the SAME flash, never separately.

### Phase 2 — transport plumbing (the fiddly, security-relevant part)
- `ORIGIN_UART` appended at END of CommandOrigin (System_CommandTypes.h:19-44
  — zero-value = physical-presence hazard documented at :53-66).
- `SOURCE_UART = 9` in CommandSource (System_User.h:29-40), then every
  transport switch gains a case (all currently fail safe-but-dead):
  - sessionIdleWindowMs → gSettings.sessionIdleUart (System_User.cpp:84-98)
  - tgRequireAuth → mirror the SOURCE_SERIAL branch against gUartAuthed
    (System_User.cpp:175-223) — must NOT fall into the trusting `else`
  - loginTransport/logoutTransport (System_User.cpp:438-520) + add "uart"
    to the credentialTransport audit set
  - transportToNotifSource (System_AuthIdentity.cpp:142-155)
  - origin→string switch for the web feed (HardwareOne.cpp:831-843)
- **Session revocation (corrected by verification round):** only TWO sites
  actually clear serial state — the revokeUserSessions sweep branch
  (System_User.cpp:588-594) and the hand-rolled ban path (:1059-1061); the
  other three previously-cited sites are user-facing notices only, and
  delete/demote/password-change all funnel through revokeUserSessions.
  Extend both clearing sites + isTransportAuthenticated (:633) + the
  session-list JSON synthetic entries (:2453-2479, so admins can SEE the CM5
  session) for the UART globals.
- **Sites the first investigation missed (found by exhaustive grep):**
  - authSuccessUnified (WebServer_Server.cpp:485-518): without a SOURCE_UART
    branch it sets NO session state and logs the login as transport=internal.
  - recordLoginAttempt's path map (WebServer_Server.cpp:1591-1602): add a
    uart mapping so audit lines read correctly.
  - **cmd_login/cmd_logout (System_Utils.cpp:5367-5437) — security hole:**
    the registry `login` command defaults its transport to SOURCE_SERIAL, so
    an authed (or AuthBypass) CM5 could mint a live USB-console session by
    submitting `login u p`. Fix: derive the default transport from the
    calling context, and refuse cross-transport session minting from
    SOURCE_UART.
  - tgRequireAuth STUB build (System_User.cpp:227-256): the ENABLE_HTTP_SERVER=0
    stub returns true for unknown transports — add the SOURCE_UART case in
    BOTH builds (in the stub it's an auth bypass, not a dead feature).
  - bleResolveStampUsername (BLE_Peers.cpp:158-166) falls back to gSerialUser
    as G2 pair-owner: explicitly EXCLUDE gUartUser (a machine account must
    never become the lens's pair-time identity).
  - powerSaveNoteActivity (System_Utils.cpp:4584-4586): exclude SOURCE_UART
    like SOURCE_INTERNAL, or a polling CM5 daemon permanently defeats
    power-save idle.
- Result delivery: NO deliverCommandResult branch needed — verified that its
  single call site is the serial drain; the UART drain writes its own reply
  (MQTT pattern, exactly-once confirmed). Do NOT clone deliverCommandResult's
  serial gates: `outSerial off` and CLI help-mode must not mute the CM5 link.

### Phase 3 — the channel itself (new System_UartLink.cpp/.h)
- **Per-board parameterization, ALL SIX boards supported (user decision
  2026-08-07, supersedes the earlier S3-only gate):** the feature exists on
  every board via board-block defines in System_BuildConfig.h — exactly the
  existing pattern for I2C/SD/camera pins:
    UART_LINK_PORT / UART_LINK_TX_PIN / UART_LINK_RX_PIN
  - FeatherS3, FeatherS3-FE:  Serial0 (UART0), TX=43, RX=44
  - XIAO Sense:               Serial0 (UART0), TX=43, RX=44
  - QT Py ESP32 Pico:         Serial1 (UART1), TX=32, RX=7  (its TX/RX header)
  - Feather ESP32 V2:         Serial1 (UART1), TX=8,  RX=7  (its TX/RX header)
  - Generic ESP32:            Serial2 (UART2), TX=17, RX=16 (console owns 1/3)
  The classic-ESP32 console-killer hazard is solved structurally: on classic
  chips the console stays on UART0 through the bridge chip and the link binds
  a DIFFERENT UART. Hard rule enforced at build time (static_assert or #error):
  **the link may never bind the UART instance that carries the console** —
  UART0 is only bindable where the console has moved to USB-Serial-JTAG (S3).
  Bonus: on classic boards the link pins carry NO ROM boot burst (that goes
  out the console pins), so the channel is clean at reset there.
  Per-chip baud clamp: classic ESP32 keeps baud-vs-CPU-scaling immunity only
  on the REF_TICK clock source, ceiling ~230400 baud (~23KB/s) — clamp the
  uartLinkBaud setting range per target (esp32-hal-uart.c:918-928 picks
  REF_TICK automatically ≤250k). S3 keeps the 921600 default (XTAL source).
  No backward-compat handling anywhere: pre-1.0, user erase-flashes all
  devices (standing convention).
- Init beside Serial.begin (HardwareOne.cpp:1348), order-critical:
  `Serial0.setRxBufferSize(4096); Serial0.setTxBufferSize(4096);` (both MUST
  precede begin — HardwareSerial.cpp:662-696), then
  `Serial0.begin(baud, SERIAL_8N1, 44, 43)`, then **`gpio_pullup_en(GPIO 44)`**
  — verified the Arduino HAL enables NO pull on RX (esp32-hal-uart.c:352-453;
  the only rx pullup call is #if 0 dead code), so an empty header or a
  CM5 held in reset otherwise feeds break/garbage into the drain.
  (RX was 8192 in the draft; corrected — request/response traffic peaks at a
  2047B command, and no realistic buffer survives a documented 200ms+ loop
  stall under streaming anyway. Driver total ≈ 9-10KB internal DRAM,
  allocated ONLY at begin() — lazy when uartLinkEnabled=0, verified.)
- Drain: clone the serial pattern (HardwareOne.cpp:2642-2771) polled from
  loop() beside the existing drain — not onReceive (spawns an event task;
  violates the no-per-action-tasks / core-pinning policy). Own state:
  gUartAuthed / gUartUser / gUartLastInteractionMs / gUartCLI. One command
  per lap. **Explicit accumulator cap** (~2100B) + garbage hygiene: an
  unauthenticated line that is not a well-formed `login` gets a rate-limited
  single error reply (not one per line) so break-noise can't flood the OLED
  via broadcastOutput or spam the audit log.
- Login gate cloned from HardwareOne.cpp:2662-2728 with lockout key
  **"uart"** (isolation from serial's "local" verified — per-key counters,
  no bulk reset). Inline logout/whoami intrinsics. **Accepted cost:** the
  gate runs PBKDF2 (~12s) inline on the loop task per login, freezing UI
  ticks — same as the serial console today; acceptable because the daemon
  logs in once per boot (sessionIdleUart=0 default). Documented, not fixed,
  in v1.
- Command ctx: origin=ORIGIN_UART, transport=SOURCE_UART, ip="uart",
  path="uart", **outputMask=MSG_ROUTE_FILE**, and **captureOutput=false** —
  the verification round found capture PREPENDS streamed lines to the reply
  (breaking OK:/Error: framing) and silently drops the result at ≥4095B
  captured; this is exactly why MQTT sets false. The CM5 uses `json`-token
  commands for structured replies.
- TX policy (corrected): plain `Serial0.write(blob, len)` + newline. The
  draft's stall-protection machinery guarded an impossible failure: HW flow
  control is hard-disabled (esp32-hal-uart.c:888) and no CTS is wired, so
  the TX ring ALWAYS drains at wire rate — worst case ~45ms for a 4KB reply
  at 921600, and a drop policy couldn't even detect drops (write() returns
  the requested size unconditionally, HardwareSerial.cpp:585-588).
- **FTS/wizard gating:** boot-time FTS blocks before loop() runs (verified),
  so the drain is inherently safe pre-provisioning; park the drain while
  gWizardOwnsSerial for the post-boot wizard case — the real reason is the
  60s submitAndExecuteSync loop-task stall against a wizard-occupied
  cmd_exec, not a byte race (the wizard never reads Serial0).
- Baud note: 2,000,000 is the divider-exact rate from the 40MHz XTAL;
  921600 carries ~0.06% error (fine). XTAL-source immunity to power-save CPU
  scaling confirmed for S3.

### Phase 4 — settings (per project conventions, no auto-registration)
- Settings struct: `uartLinkEnabled` (default **false**, opt-in),
  `uartLinkBaud` (921600), `uartRequireAuth` (true), `sessionIdleUart`
  (**0** = never — machine sessions shouldn't idle out; the CM5 client must
  still re-login on any "Authentication required").
- Rows go in the **existing output module** ("auth"/"channels" groups,
  System_Settings.cpp:2105-2112) — a new module would consume the LAST
  MAX_SETTINGS_MODULES slot after the uncommitted 32→36 bump, and overflow
  is silent.
- Real commands: cmd_uartrequireauth (clone :2123-2126 wrapper, admin+super
  like serialrequireauth), SETTING_EDITOR_CMD for sessionidleuart (:2988
  pattern). Do NOT replicate the existing serialrequireauth dual-registration
  (System_Settings.cpp:244 vs System_User.cpp:3421 — pre-existing duplicate,
  flagged for separate cleanup).

### Phase 5 — HW validation (before the carrier exists)
- FeatherS3 bench: USB-UART dongle on the TX/RX header pins @ 921600.
- Test matrix: login/lockout ("uart" key isolation from "local"), idle
  logout with a SOURCE_UART case present (it's silently dead without it),
  user-delete revokes the UART session, 4KB result integrity, ROM-burst
  resync at reset, wizard parking, `outSerial off` does NOT silence the UART
  reply path, USB console fully independent (parallel sessions, different
  users).

### Phase 6 — CM5-side client (separate project, sketch)
- Python daemon on /dev/ttyAMA2 (CM5 uart2, GPIO4/5 — the carrier crosses
  TX/RX to the module's GPIO44/43). pyserial; discard garbage until first
  clean prompt/reply; `login cm5 <pass>`; prefer the `json` token per
  command (whole-document replies, stampOkStatus-exempt); OK:/Error: framing
  otherwise; client-side caps (cmd ≤2047B, reply ≤4095B); 65s command
  timeout (submitAndExecuteSync waits up to 60s — PBKDF2 login is ~12s);
  re-login on "Authentication required"; query crash history after any
  unexplained link silence; expect the updater to be UART-silent (recovery
  is physical by design).

## 3b. Verification round — per-board matrix and residual risks (2026-08-07)

Per-board verdicts from the 5-skeptic adversarial pass (all six
System_BuildConfig.h blocks + boards/*.defaults enumerated):

| Board | 43/44 | Verdict |
|---|---|---|
| FeatherS3 (primary) | free (TX/RX header) | SAFE with RX pullup |
| FeatherS3-FE (flash-enc) | free; USB CDC survives JTAG-off eFuse | SAFE |
| XIAO S3 Sense (`xiao_s3`) | free; **variant SS=44 latent** — inert while nothing calls setHwCs (verified), documented constraint | SAFE |
| XIAO S3 base block | — | unreachable dead code (Sense force-defined for every S3 build, CMakeLists.txt:770-772) |
| QT Py ESP32 Pico (classic) | 43/44 don't exist; Serial==Serial0 on UART0 | SUPPORTED via UART1 on TX=32/RX=7 (never UART0); baud ≤230400 |
| Feather ESP32 V2 (classic) | same | SUPPORTED via UART1 on TX=8/RX=7; baud ≤230400 |
| Generic ESP32 (classic) | TX/RX header pins ARE the console pins | SUPPORTED via UART2 on 17/16; baud ≤230400 |

Residual risks ACCEPTED (documented, not fixed in v1):
- OLED/G2 forced-route: broadcastOutputCore force-adds OLED|G2 to every
  command's streamed lines (System_Debug.cpp:967-968), so CM5 command noise
  can scroll the OLED console — identical to MQTT today; revisit only if it
  annoys in practice.
- Login-attempt table is 8 shared slots with oldest-eviction
  (WebServer_Server.h:121): a web attacker rotating ≥8 IPs can evict the
  "uart"/"local" tiers. Pre-existing weakness for serial too — separate
  backlog item (pin non-IP keys).
- OTA probation: a chatty CM5 immediately after an OTA reboot can gap the
  loop heartbeat (>5s commands) and postpone image validation — the CM5
  daemon should stay idle during probation (client-side rule, Phase 6).
- factoryreset leaves sessions live for the ~1s pre-reboot window (all
  transports; pre-existing).
- Crash-history mitigation for lost UART panics is partial: brownout/WDT
  resets and pre-hook crashes leave phase-only records without backtraces
  (System_CrashRecord.cpp:330-331); the recorder itself (RTC_NOINIT panic
  hook, crashRecordInstallPanicHook) is console-independent — verified.

## 4. Out of scope / explicitly deferred
- MSG_ROUTE_UART streaming sink (D4) — only on demonstrated need.
- Bulk/file transfer framing — CLI envelope caps at 4KB/reply; a framed
  transfer protocol is a separate design if fileread-over-UART is approved.
- Flow control (CTS/RTS) — carrier reserves CM5 GPIO6/7 + XIAO D0-D3 for a
  future rev.
- Recovery-updater UART console — recovery stays USB-only per the isolation
  model (the updater's authenticated SoftAP is the only CM5-adjacent
  recovery avenue; out of scope here).

## 5. Adversarial verification summary (2026-08-07)

Second workflow, five skeptics instructed to refute. Score against the
original draft: the ARCHITECTURE survived (transport-not-sink, separate
session state, S3 coexistence of Serial0+HWCDC, lockout-key isolation, FTS
boot gating, settings plumbing fit — all CONFIRMED or confirmed-with-nuance),
but FOUR draft instructions were flat wrong and are corrected above:

1. No board gating → console-killer on 3 classic-ESP32 boards (§Phase 3).
2. Phase 1 propagation recipe was inert — fullclean regenerates nothing
   (§Phase 1).
3. TX stall-protection guarded an impossible failure and couldn't work
   (§Phase 3).
4. "Five revocation sites" was miscounted — two clear state, and the missed
   list (authSuccessUnified, cmd_login transport default, stub tgRequireAuth,
   BLE pair-owner fallback, powerSaveNoteActivity) mattered more (§Phase 2).

Also corrected: captureOutput defaults false (framing break), RX buffer
4096 not 8192, 2M is the exact baud not 921600, crash-mitigation function
misnamed (recorder is crashRecordInstallPanicHook, not crashRecordEmitEarly),
panic mirroring to USB already exists today (D1 cost smaller than framed).

## 6. Implementation record (2026-08-07)

Files: NEW System_UartLink.{h,cpp}; modified System_BuildConfig.h (six board
blocks), System_CommandTypes.h, System_User.{h,cpp}, System_AuthIdentity.cpp,
System_Utils.cpp, System_Settings.{h,cpp}, WebServer_Server.cpp,
System_ESPSR.cpp, BLE_Peers.cpp, HardwareOne.cpp, CMakeLists.txt;
sdkconfig + sdkconfig.ota + sdkconfig.defaults.esp32s3.

Defects found by the diff review and FIXED:
- **HIGH — lifecycle race.** Command handlers run on cmd_exec_task, which
  ticks concurrently with the loop task for web/MQTT/BLE/automation-issued
  commands, so `uartlink off` / `uartlinkbaud` could free the port and the
  line accumulator underneath an in-flight drain (concurrent String mutation
  = heap corruption). Fixed: handlers call uartLinkRequestStart/Stop/Restart,
  which set a flag the drain consumes on the loop task. The plan's
  "serialization makes this unreachable" reasoning was wrong.
- **MEDIUM — auth-off login was impossible.** The in-band login lived inside
  the `uartRequireAuth` gate, so with auth off it was skipped while cmd_login
  refused SOURCE_UART callers — the host was pinned to AuthBypass and looped
  on an error telling it to use the login it just used. Login is now an
  intrinsic in both modes.
- **MEDIUM — over-long lines executed their tail.** The cap cleared the
  accumulator but kept consuming the same line, so the residue ran as a
  command. Now discards through the newline; cap lowered to 2047 so nothing
  is silently strncpy-truncated into a shorter, still-valid command.
- **MEDIUM — Generic-ESP32 pins were fatal.** GPIO16/17 are the in-module
  PSRAM CLK//CE on WROVER, and that target requires PSRAM; enabling the link
  would fault the cache and crash-loop (setting persists). Moved to 32/33.
- **MEDIUM — G2 pair-owner exclusion** (planned, initially omitted): a
  `bleautoreconnect` over the link would stamp the machine account as the
  lens's persistent owner. Now excluded in bleResolveStampUsername.
- **LOW** — SOURCE_UART added to logCommandExecution (was auditing as
  `user@unknown`), getTransportUser, transportToStableString; 9600 baud
  floor; begin() failure detected via operator bool(); AuthContext::opaque
  nulled; stale loginTransport comment corrected.

Deviations from the plan, deliberate: session state is file-static
(sUartCLI/sLastInteractionMs) rather than globals — nothing outside the
channel needs them; uartLinkBaud persists 0 = "board default" instead of a
literal 921600, so one settings.json works across boards.

### Remaining before merge
1. **Hardware test** (§Phase 5 matrix) — nothing here is HW-validated.
2. **OTA-layout build** with the signing key exported, to confirm the
   hand-edited sdkconfig.ota resolves the console the same way the main
   build does (verified there: USB_SERIAL_JTAG=1, UART_NUM=-1).
3. Classic-ESP32 full-image build needs the manual feature-level edits in
   System_BuildConfig.h per docs/BOARD_SWITCHING.md; only per-file
   compilation of the touched files was verified for that target.
