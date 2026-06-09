# ESP32-P4 port — assessment & future plan

**Status:** 🟡 Assessment only — **NOT scheduled. No code changes.** This is a large future change; this doc captures the research and design thinking so it can be picked up later (by us or a future session) without re-deriving it.
**Date:** 2026-06-07
**Context:** We currently target ESP32 + ESP32-S3 (both native-radio). This evaluates whether/how to add ESP32-P4 (specifically the P4X boards on order, which ship with an onboard ESP32-C6 for connectivity).
**Related:** [HEAP_OPTIMIZATION_FINDINGS.md](HEAP_OPTIMIZATION_FINDINGS.md) (the memory pressure that partly motivates looking at P4).

---

## 0. TL;DR / decision

- **Not now.** This is a multi-week port with one genuine blocker to clear first (ESP-NOW). Revisit when there's appetite for a large change.
- **"P4X" is not a faster/better CPU** — it's the P4 silicon **revision 3.0/3.1** (a power-rail/stability fix), same 400 MHz cores. Don't wait for it expecting a capability jump; your older P4s are capability-identical, just rev-1.x.
- **The dual-CPU (P4 host + C6 radio) architecture does NOT block ESP-NOW.** ESP-NOW works natively on the C6; what's missing is only the *host-side proxy* in stock esp-hosted. It's a software gap, not an architectural wall.
- **The right enabling move is a thin `EspNowLink` abstraction** in our code (≈8 primitives, already ~half-built) — and it's **not net-new**: it's the existing `HAL_*` idiom (`HAL_Input`/`HAL_Display`/`HAL_Audio`) applied as a 4th HAL, *and* it's the completion of the codebase's already-planned "Seam 3: one send entry point" (see §6a). Good hygiene on S3 *today*; lets us defer the P4 implementation choice indefinitely.
- **Big upside if we ever do it:** P4 has 768 KB SRAM + up to 32 MB PSRAM, so the internal-RAM ceiling fight (see heap doc) largely *evaporates*, and the on-device LLM gets much faster.

---

## 1. P4 vs "P4X" — clearing the confusion

| | Reality |
|---|---|
| Is P4X a new/faster chip? | **No.** Same dual RISC-V @ 400 MHz, 768 KB SRAM, same peripherals. |
| What changed? | Silicon **rev 3.0**: pin 54 NC → power rail (`VDD_HP_1`) for HP-domain stability + a few extra passives. Rev 3.0 = `XFXX`, rev 3.1 = `XGXX` in the date code. |
| Firmware impact | **Firmware must be compiled per-revision** — a rev-1.x build won't run on rev-3.x and vice versa. |
| Old P4s (rev 1.0/1.1/1.3) | Still work; Espressif just flags them "not recommended for *new designs*." Fine as bench units. |
| Practical takeaway | If we ever port, target the **P4X (rev 3.1)** boards; treat the older P4s as throwaway test boards. Sold under confusingly-overlapping SKUs — verify revision on arrival. |

Sources: [Hackaday — "When is a P4 a P4…"](https://hackaday.com/2026/03/21/esp32-when-is-a-p4-a-p4-but-not-the-p4-you-thought-it-was/), [CNX Software — rev 3.0 power rail](https://www.cnx-software.com/2026/03/23/esp32-p4-revision-3-0-gains-new-power-rail-requires-new-pcb-design-and-firmware/).

---

## 2. The dual-CPU architecture (what it actually is)

Two distinct "dual" notions — don't conflate:

1. **The P4's own 2× HP RISC-V cores (+1 LP core).** Ordinary FreeRTOS SMP — same model as the S3 we already run on (core pinning, etc.). **Not the hard part.**
2. **P4 (host) + C6 (companion) split.** This is the new thing, and it's **host + radio co-processor**, *not* symmetric multiprocessing:

```
┌──────────────── ESP32-P4 (HOST) ────────────────┐        ┌──── ESP32-C6 (SLAVE) ────┐
│ our app + lwIP TCP/IP + Bluedroid BLE *host*     │  SDIO  │ esp-hosted-mcu slave fw   │
│ esp_wifi_remote shim (calls esp_wifi_*/esp_ble_*)├──4bit──┤ = WiFi MAC/PHY + BLE ctlr │
└──────────────────────────────────────────────────┘ ~40MHz └───────────────────────────┘
```

- The **C6 runs Espressif's slave firmware, not our code** — it's just the radio. We flash **two firmwares** (our app on P4, the slave on C6 once).
- Transport: **SDIO 4-bit** (the EV/P4X boards wire this) or SPI/UART.
- Software: `esp-hosted` (transport + RPC) + `esp_wifi_remote` (host-side shim that re-exposes the standard `esp_wifi_*` API and RPCs it to the C6).

Sources: [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu), [esp-wifi-remote](https://github.com/espressif/esp-wifi-remote), [Wireless solutions for ESP32-P4](https://developer.espressif.com/blog/wireless-connectivity-solutions-for-esp32-p4/).

---

## 3. Connectivity feasibility per subsystem

| Subsystem | Over P4+C6 (esp-hosted) | Verdict |
|---|---|---|
| **WiFi / HTTP / HTTPS / mDNS** | Transparent via `esp_wifi_remote` — 65+ WiFi RPCs implemented; lwIP/web server sit on top unchanged. | ✅ Easy–Med |
| **BLE central (G2 temples, R1 ring)** | C6 exposes **virtual HCI** over the link; P4 runs full Bluedroid host → GAP central + GATT client work. (BLE rides a raw-HCI channel, *not* the RPC list — that's why it's not in `implemented_rpcs.md`.) | ⚠️ Med–Hard; latency over SDIO is the risk for the G2 hijack/tap + notify path |
| **ESP-NOW mesh (V4, bond/RCE, sensors, file xfer, clerk)** | **No ESP-NOW RPCs exist** in stock esp-hosted. Not a transparent drop-in. | ❌ The blocker — see §4 |
| Camera (DVP→MIPI-CSI), mic, OLED, sensors | Re-port (different peripherals/IDF APIs; P4 uses MIPI-CSI not DVP). | Med, grind |
| Memory tuning | Redo, but with *vastly* more headroom (see §7). | A payoff, not a fight |
| Arduino layer | arduino-esp32 P4 support is **beta** (2026). | Med, ongoing |

**Verified:** esp-hosted's [`implemented_rpcs.md`](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/implemented_rpcs.md) lists 65+ WiFi RPCs and **zero** ESP-NOW RPCs (no `esp_now_init/send/add_peer/...`). No public roadmap item to add it.

---

## 4. The ESP-NOW gap — why, and how to solve it

**Why it's missing (not "abandoned"):** ESP-NOW's design assumption is *the app runs on the WiFi chip*. The hosted model inverts that (app on P4, radio on C6), and Espressif simply hasn't built the remoting glue. It's also latency-sensitive by design, and its peer/key state lives in the slave's MAC — so proxying the *whole* state machine + callbacks is more than forwarding a frame. **The C6 itself does ESP-NOW natively, fully.** So this is a software gap, not impossibility.

**Three routes to get ESP-NOW on P4:**

| Route | What | Effort | Notes |
|---|---|---|---|
| **A. Thin proxy** ⭐ (our lean) | Add `esp_now_*` opcodes/events to the slave + host; our app's `esp_now_*` calls "just work". | High (but mechanical) | App code unchanged; upstreamable to Espressif |
| **B. Fat co-processor** | Run the whole ESP-NOW V4 stack *on the C6*; expose coarse app-level messages to the P4. | High | Fewer SDIO round-trips (lower latency); splits codebase across 2 chips/firmwares |
| **C. Don't run ESP-NOW on P4** | Keep ESP-NOW nodes on S3/C-chips; P4 hub bridges over WiFi/MQTT. | Low | Changes topology |

### Slave firmware is open and extensible
- **License Apache-2.0**; slave source is in the repo's **`slave/`** dir, builds for C6.
- RPC protocol is **protobuf-c**; framework is explicitly extensible (Espressif: *"the RPC … can be extended to provide any function required by the Host, as long as the co-processor can support it"*).

### "Adding ESP-NOW = new opcodes" — the precise shape
1. **Control opcodes** (request/response): new protobuf messages + slave handler + host stub for `esp_now_init`, `esp_now_add_peer`, `esp_now_set_pmk`, **`esp_now_send`** (≤250 B payload fits in the message).
2. **Async events** (slave→host, on the existing event channel): the two ESP-NOW callbacks — **`recv_cb`** (incoming frame + src MAC) and **`send_cb`** (TX status).
3. **Data path:** usually **none needed** — frames are tiny, so they ride the control/event messages; skip the WiFi netif data-path plumbing.

Slave handlers are easy (they just call the C6's real `esp_now_*`). Template to copy: an existing WiFi RPC triple (e.g. `WifiScanStart` + its scan-done event). For Route A, opcodes mirror `esp_now_*` 1:1; for Route B, opcodes are our V4 app messages.

**Gotchas (both routes):** peer/key (PMK/LMK) state lives on the C6 now; ESP-NOW shares the one radio's channel with the STA connection (same constraint as S3 today); send/recv correlation + flow control across the async boundary.

---

## 5. Latency — what's trustworthy

**There is NO published ESP-NOW-over-hosted latency number** (feature doesn't exist). Any figure is an estimate. What *is* measured:

| Measured | Value | How to read it |
|---|---|---|
| P4+C6 SDIO ping RTT (no BLE) | ~43 ms (134 ms outlier) | ⚠️ Mostly WiFi air-time + remote host, **not** the SDIO hop — upper-bound proxy only |
| Same, BLE active | 49–316 ms, then stalls | The real warning: shared SDIO link → jitter; open backpressure bugs ([#184](https://github.com/espressif/esp-hosted-mcu/issues/184)) |
| Throughput | ~36–80 Mbps SDIO 4-bit | Bandwidth is a non-issue for tiny ESP-NOW frames |

**Estimated incremental cost of the hosted hop** (TX = 1 crossing down, RX = 1 event up):
- SDIO bus transfer of a ≤250 B frame: ~10–15 µs (negligible).
- Software/RPC overhead (protobuf + task scheduling + IRQ/DMA + context switch): **~0.5–2 ms per crossing**.
- ⇒ thin-proxy ESP-NOW ≈ native (~1–4 ms) **+ ~1–2 ms each way**; median likely **low-single-digit ms**.

**The tail, not the median, is the risk** — the SDIO link is shared by WiFi + BLE + (would-be) ESP-NOW, so under load expect jitter/outliers. **Practical verdict:** irrelevant for our ESP-NOW workloads (sensor relay, bond, file transfer, chat). The only paths to scrutinize are **tight-timeout/timing-sensitive** ones: bond handshake, KEY_EX retry windows, time-sync. **Only a PoC measurement on real hardware (p50 *and* p99, with WiFi+BLE busy) is trustworthy.**

Sources: [esp-hosted-mcu #184](https://github.com/espressif/esp-hosted-mcu/issues/184), [r4d10n throughput test](https://github.com/r4d10n/esp32p4-c6-wifi-test), [Electric UI latency benchmarks](https://electricui.com/blog/latency-comparison).

---

## 6. The `EspNowLink` abstraction (the enabling refactor)

We discovered the raw `esp_now_*` surface is **tiny and already concentrated** (grep, 2026-06-07):
- `esp_now_send` — 18 calls, **almost all in `System_ESPNow.cpp`** (+ a couple in `System_ESPNow_Tx.h`, `_Sessions.cpp`)
- `register_recv_cb` / `register_send_cb` — 1 each
- `add_peer` / `del_peer` / `get_peer` / `mod_peer`, `init` / `deinit` / `set_pmk`

≈ **8 primitives**, and there's already scaffolding around them: `EspNowTxGuard` (TX mutex), an `espnowTxQueueSize` **TX queue**, and high-level senders (`EspNowSendTextMessage`, `EspNowSendRemoteCommand`, `EspNowSendBrowseRequest`). V4/bond/sensors/clerk all sit *above* this.

**Design:** a backend interface with two compile-time implementations selected by `HW_BOARD`/target.

```
        app logic (V4, bond, sensors, clerk)   ← unchanged
                    │  EspNowSend* / dispatch
        ┌───────────▼───────────┐
        │   EspNowLink (iface)   │   ~8 methods
        └───────────┬───────────┘
          ┌─────────┴─────────┐
   EspNowLinkNative      EspNowLinkHosted
   (S3/ESP32: real        (P4: proxied esp_now_* over SDIO opcodes [Route A]
    esp_now_*)             — or fat-C6 message API [Route B])
```

**Why this is the key move:**
- It makes **Route A vs Route B a swappable backend decision we can defer** — ship thin-proxy, measure, swap to fat-co-processor behind the *same* interface without touching app logic.
- Our **recv path is already deferred/async** (we never do heavy work in callbacks — same rule as the BTC_TASK/tap-dispatcher), which exactly matches hosted async-event delivery. No impedance mismatch.
- **Side benefit on S3 *today*:** introducing `EspNowLink` with only the native backend is pure hygiene — collapses the 18+ scattered `esp_now_send` calls behind one tested chokepoint. This is the one piece worth doing independently of P4, *if/when* we touch that area.

**Gotchas to bake into the interface:** peer/key state location (C6 in hosted mode), channel coupling with the STA connection.

---

## 6a. Existing abstraction patterns to model on (codebase recon, 2026-06-07)

We don't need to invent the seam — the codebase already ships this idiom **three times over**. `EspNowLink` should be the **4th `HAL_*`**, built to match.

### House idiom: `HAL_*` = free-function API + compile-time backend selection
Existing HALs: **`HAL_Input`**, **`HAL_Display`**, **`HAL_Audio`** (`components/hardwareone/HAL_*.{h,cpp}`). `HAL_Input.h`'s own doc states the goal verbatim: *"Provides compile-time and runtime input controller selection … the same UI code works with different input hardware by changing INPUT_TYPE."* Copy this shape exactly:

1. **Stable free-function API** in a `HAL_*.h` header (e.g. `inputGetButtonMask()`, `inputGetX()`) consumed everywhere above. **No C++ virtual classes / vtables** — matches house style (the whole codebase has exactly *one* pure-virtual interface) and avoids vtable/RAM overhead (relevant given the heap work).
2. **User-facing flag** in `System_BuildConfig.h` (`INPUT_DEVICE_TYPE`), **regex-parsed by CMakeLists**, which `list(APPEND …)`s the chosen backend `.cpp` (`i2csensor_seesaw.cpp` *xor* `i2csensor_ano_encoder.cpp`).
3. **Derived internal constant** (`INPUT_TYPE`) so HAL code switches on one symbol regardless of the user flag.
4. Per-file `#if` guards as the fine-grained backstop.

→ `EspNowLink` = `HAL_EspNow.{h,cpp}` (or `System_ESPNow_Link.*` to stay in the ESP-NOW family), backends `…_Native.cpp` / `…_Hosted.cpp`, selected by **`CONFIG_IDF_TARGET`/board** (P4 → hosted, S3/ESP32 → native) rather than a hand-set flag. `HAL_Display` (SSD1306/ST7789/ILI9341) proves the pattern scales past 2 backends — handy if thin-proxy vs fat-co-processor ever coexist.

### Bigger synergy: this *is* the already-planned "Seam 3"
The ESP-NOW subsystem is already cleanly layered (`System_ESPNow_Wire` / `_Tx` / `_Crypto` / `_Sessions` / `_Files` / `_Sensors` / …), and the codebase's own [ESPNOW_SEAM_UNIFICATION.md](ESPNOW_SEAM_UNIFICATION.md) has an **open goal — "Seam 3: one send entry point"** — to make `v4_send_payload_smart` the single application send path, with `v4_send_frame` / `v4_send_raw_*` as documented low-level exceptions.

That consolidation **is the prerequisite the HAL needs**, and they want it anyway for simplicity. Once every send funnels through `v4_send_payload_smart` → the `_Tx` layer → a single `esp_now_send` call site, the native-vs-hosted swap collapses to **one place at the bottom of the existing stack**. Everything above the radio primitives (wire format, crypto, sessions, smart-send, the recv dispatch table) is already platform-agnostic.

- **So `EspNowLink` is a *thin HAL at the bottom of the existing Tx layer* — the radio primitives only — not a parallel layer.**
- **Sequencing:** (1) finish Seam 3 (single app send entry; wanted regardless) → (2) funnel `_Tx` to one set of raw primitives → (3) extract those ~8 primitives + recv-cb into `HAL_EspNow` with a native backend = **no-op refactor on S3, shippable today** → (4) add the hosted backend if/when P4 happens.

---

## 6b. Send/recv internals — recon findings (2026-06-07)

Reading the as-built ([ESPNOW_ARCHITECTURE.md](ESPNOW_ARCHITECTURE.md) §3/§4/§6) + the TX clerk header confirms the seam is **unusually clean** — cleaner than §6's first estimate.

- **TX already funnels to one base.** The 6-layer send stack (typed wrappers → `v4_send_payload_smart` → encrypted/chunked variants) bottoms out at **`v4_send_frame`** ([System_ESPNow.cpp:1436]) — *"build header + CRC + single `esp_now_send`."* Above it, a **single-sender TX clerk** (`espnowtx::` in `System_ESPNow_Tx.h`) already routes ~95% of sends through one dispatcher task (JOB_AEAD_SMART → `v4_send_payload_smart`, JOB_RAW → `v4_send_frame`); producers never touch `esp_now_send`. So the radio TX primitive is effectively *one call site already*; "Seam 3" just mops up the few direct `v4_send_frame`/handshake stragglers.
- **RX already one entry + declarative dispatch.** `esp_now_register_recv_cb(onEspNowDataReceived)` (one site, [:8752]) → `v4_try_handle_incoming` → `v4_dispatch_table_try` → `kV4HandlerTable` (one row per opcode). Heavy handlers already defer to cmd_exec. The code **already synthesizes a `recv_info` from a non-radio source** ([:7674]) — proof the RX path can be fed by something other than the native callback (exactly what the hosted backend does).
- **The big simplifier: NO link-layer keys cross the seam.** ESP-NOW native encryption is *not used* — peers are added `encrypt=false` (LMK removed 2026-05-21); all confidentiality is **app-layer AEAD** (chacha20poly1305 SESSION_FRAME) computed in `System_ESPNow_Crypto.cpp` *above* `v4_send_frame`. Consequences:
  - The whole crypto/session/identity stack (KEY_EX, SESSION_*, Ed25519/X25519, replay window) is **platform-agnostic** — identical native vs hosted; **P4 needs no crypto re-validation**.
  - What crosses the radio seam is only **opaque, already-sealed frame bytes + MAC addresses** — no PMK/LMK, no session state. The earlier "keys must cross to the C6" gotcha is **moot**.
  - For Route A, the C6 therefore only handles opaque frames + MACs (send-to-MAC, deliver-from-MAC, add-peer=MAC+channel) — a *minimal* opcode set, and all secrets stay on the P4.

---

## 6c. Draft `HAL_EspNow` surface (≈8 functions)

Grounded in §6b — the literal seam (native backend = thin `esp_now_*` wrappers; hosted backend = esp-hosted opcodes/events to the C6):

```c
// Carries opaque (already header+CRC+AEAD-sealed) frames + MACs ONLY. No keys.
bool      halNowInit();                                  // esp_now_init + install recv trampoline
void      halNowDeinit();
esp_err_t halNowSend(const uint8_t dst[6],              // FF*6 = broadcast
                     const uint8_t* frame, size_t len);  // ← replaces the ~6 esp_now_send sites (all in System_ESPNow.cpp)
typedef void (*HalNowRecvFn)(const uint8_t src[6], int8_t rssi,   // dst unused; rssi from rx_ctrl
                             const uint8_t* frame, int len);
void      halNowSetRecvHandler(HalNowRecvFn fn);         // native: trampoline from esp_now recv cb
esp_err_t halNowAddPeer(const uint8_t mac[6], uint8_t channel);  // encrypt=false always
esp_err_t halNowDelPeer(const uint8_t mac[6]);
esp_err_t halNowSetChannel(uint8_t ch);                  // ESP-NOW shares the STA channel
```

- **Native backend** (`HAL_EspNow_Native.cpp`, S3/ESP32): each fn is a 1–3 line wrapper over `esp_now_*`; the recv trampoline adapts `esp_now_recv_info*` → `HalNowRecvFn`. Extraction is a **no-op refactor** — identical behavior, shippable on S3 today.
- **Hosted backend** (`HAL_EspNow_Hosted.cpp`, P4): `halNowSend` → a `send` opcode (frame+dstMAC) over esp-hosted; the C6 delivers inbound frames as an async event → `HalNowRecvFn`; `addPeer`/`setChannel` → config opcodes. (Requires the C6 slave-fw opcodes from §4 Route A.)
- Selection: CMakeLists picks the `.cpp` by `CONFIG_IDF_TARGET` (esp32p4 → Hosted, else Native) — mirrors the `INPUT_DEVICE_TYPE` pattern (§6a).
- **RX fields confirmed:** the pipeline consumes only `recv_info->src_addr` (sender MAC) + `recv_info->rx_ctrl` (RSSI/channel, diagnostics) — **not** `des_addr` (broadcast/unicast comes from the V4 header flags). So `HalNowRecvFn` needs just `src + rssi + frame`; the hosted event carries those.

---

## 7. Why bother at all — the upside

- **Kills the internal-RAM ceiling problem.** P4 = 768 KB SRAM + up to 32 MB PSRAM. The whole heap saga in [HEAP_OPTIMIZATION_FINDINGS.md](HEAP_OPTIMIZATION_FINDINGS.md) (241 KB ceiling, EXT_RAM_BSS_ATTR diversions, stack-to-PSRAM, BLE-from-PSRAM) largely becomes moot.
- **On-device LLM gets much faster** (dual 400 MHz RISC-V + more PSRAM bandwidth).
- **Future-proofs vision/HMI** (MIPI-CSI camera, MIPI-DSI display, H.264 encode, AI accel) — *if* the project ever heads that way.

The catch: none of that is what the project is bottlenecked on today, and you pay for it with the C6 companion + the ESP-NOW work + a broad peripheral re-port.

---

## 8. Effort tiers (if/when we do it)

| Tier | Work | Difficulty |
|---|---|---|
| Build plumbing | `idf.py set-target esp32p4`, `sdkconfig.defaults.esp32p4`, `boards/p4x.defaults`, partition table, add esp_hosted + esp_wifi_remote (already optional deps), flash C6 slave | Medium |
| WiFi/HTTP/TLS/mDNS | transparent via esp_wifi_remote | Easy–Med |
| BLE G2/R1 central | works in principle; SDIO latency on the hijack/notify path is the risk | Med–Hard |
| **ESP-NOW** | Route A (opcodes) or B (fat C6) — see §4 | **High / blocker** |
| Camera/mic/OLED/sensors | re-port (MIPI-CSI, pins, IDF APIs) | Med, grind |
| Memory tuning | redo, but with huge headroom | Med (payoff) |
| Arduino layer | beta P4 support | Med, ongoing |
| Maintenance | two-firmware flash flow; P4X-rev firmware ≠ old-P4 firmware | ongoing |

---

## 9. PoC plan & go/no-go gates (do this BEFORE committing to a full port)

When the P4X boards arrive, run a ~1–2 day spike that answers the unknowns first:

1. **ESP-NOW path (go/no-go):** prove Route A (add a minimal `esp_now_send`/recv opcode+event to the slave) *or* Route B (mesh on C6) works P4↔C6. ← decides viability for *this* project.
2. **BLE G2 central over SDIO:** connect to a temple, push a notify, observe under WiFi load.
3. **Latency (p50 *and* p99)** for the ESP-NOW path with WiFi+BLE busy — compare against the tight-timeout paths (bond handshake, KEY_EX, time-sync).

Decision rule: if (1) passes and (3) shows acceptable tail for the timeout-sensitive paths, the rest is mostly mechanical. If (1) fails/too costly, fall back to Route C or stay S3.

---

## 9a. Remaining exploratory work (analysis only, no code)

Progress 2026-06-07 (see §6b/§6c):
- ✅ **(1) Send-path map** — `v4_send_frame` is the single TX base; the TX clerk already funnels sends.
- ✅ **(2) Radio-primitive inventory → interface** — drafted in §6c (`HAL_EspNow`, ≈8 fns).
- ✅ **(4) Recv decoupling** — one cb → `v4_dispatch_table_try`/`kV4HandlerTable`; synthetic `recv_info` already exists.
- ✅ **(6) Peer/key ownership** — resolved *and simplified*: no link-layer keys cross the seam (`encrypt=false`; app-layer AEAD).

Also resolved this round (see §6c):
- ✅ **(5)** ~6 `esp_now_send` sites, all in `System_ESPNow.cpp` (base frame, AEAD-single, 2 fragmentation paths, frag-ACK, relay-forward) — the contained funnel into `halNowSend`.
- ✅ **(2b)** RX consumes only `src_addr` + `rx_ctrl` (RSSI); `des_addr` unused → `HalNowRecvFn(src, rssi, frame, len)`.
- ✅ **Channel** — `gEspNow->channel` derives from the STA channel (`conf.sta.channel`/`WiFi.channel()`); peers add with it. On hosted, `esp_wifi_remote` already proxies the STA channel from the C6 → essentially free, no fixed-channel assumption in the code.

Still open (small; S3-tree analysis, no P4 hardware):
- ◻️ **(3)** Study `HAL_Audio` init/teardown lifecycle as a second template. (`HAL_Display` reviewed — macro/typedef-based; `HAL_Input` is the function-API model to follow.)
- ◻️ Periodically re-check esp-hosted `implemented_rpcs.md` for upstream ESP-NOW RPCs (would make Route A free).

---

## 10. Open questions

- Has esp-hosted added ESP-NOW RPCs since 2026-06-07? (Re-check `implemented_rpcs.md` — would make Route A free.)
- Exact SDIO control-RPC round-trip latency on the P4X+C6 boards (only our own measurement is trustworthy).
- arduino-esp32 P4 maturity at port time (beta → stable?).
- Does any current ESP-NOW path have a timeout tighter than the hosted tail latency?

---

## 11. Revision history

| Date | Notes |
|---|---|
| 2026-06-07 | Initial assessment. Verified: P4X = rev 3.x (not faster); P4 needs C6 companion; WiFi transparent, BLE via HCI works, **ESP-NOW has zero hosted RPCs** (the blocker); slave fw is Apache-2.0/extensible; `EspNowLink` seam is ~8 primitives already concentrated in `System_ESPNow.cpp`. Decision: not now; large future change. |
| 2026-06-07 | Added §6a (existing `HAL_*` idiom — `HAL_Input`/`HAL_Display`/`HAL_Audio` — as the template; the HAL is the bottom of the existing Tx layer and *is* the already-planned "Seam 3" send-path consolidation) and §9a (remaining analysis-only exploration before implementation). |
| 2026-06-07 | Deep-dive: added §6b (send/recv internals — `v4_send_frame` is the single TX base, single RX cb → `v4_dispatch_table_try`/`kV4HandlerTable`, and crucially **no link-layer keys cross the seam** — confidentiality is app-layer AEAD) and §6c (draft `HAL_EspNow` ≈8-fn surface carrying opaque frames+MACs only). Closed §9a items 1/2/2b/4/5/6 + channel; only the `HAL_Audio` lifecycle study and an upstream-RPC watch remain. Net: the host-side abstraction is small, well-scoped, and shippable on S3 today as a no-op refactor. |

**Sources:** [esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu) · [implemented_rpcs.md](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/implemented_rpcs.md) · [esp-wifi-remote](https://github.com/espressif/esp-wifi-remote) · [esp-hosted-mcu #184 (latency/stalls)](https://github.com/espressif/esp-hosted-mcu/issues/184) · [r4d10n P4+C6 throughput](https://github.com/r4d10n/esp32p4-c6-wifi-test) · [Wireless solutions for ESP32-P4](https://developer.espressif.com/blog/wireless-connectivity-solutions-for-esp32-p4/) · [Hackaday P4/P4X](https://hackaday.com/2026/03/21/esp32-when-is-a-p4-a-p4-but-not-the-p4-you-thought-it-was/) · [CNX P4 rev 3.0](https://www.cnx-software.com/2026/03/23/esp32-p4-revision-3-0-gains-new-power-rail-requires-new-pcb-design-and-firmware/) · [Electric UI latency](https://electricui.com/blog/latency-comparison)
