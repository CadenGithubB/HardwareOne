# Internal-DRAM Sweep — 2026-08-11

Triggered by boot #66/#67 on the XIAO S3 (fw v0.99.89, `build-xiao_s3`, ELF 092be66a5): free internal
DRAM falls to ~14 KB once G2 + R1 are connected and a hijack menu is shown, then
`tap-dispatch: worker xTaskCreate FAILED (rc=-1)` drops every glasses tap. PSRAM is fine (6.9 MB free).

12-agent sweep (8 inventory + 4 adversarial verify), 94 findings, all file:line-verified against the
working tree and `build-xiao_s3` map/objects. Every recommendation below survived a refutation pass;
refuted proposals are listed at the bottom so they don't get re-proposed.

## Where the 294 KB internal heap goes (measured, boot #67)

| Consumer | ~Cost | Notes |
|---|---|---|
| Early boot (FreeRTOS, newlib, Arduino, LittleFS, USB-CDC, cmd_exec+debug tasks) | ~81 KB | 294→213 KB free |
| BLE controller + Bluedroid init | ~82 KB | Host env is already PSRAM-first; the internal cost is the controller env (~35–45 KB, `BLE_MAX_ACT=5` is already minimal for 3 links+adv+scan), ~19 KB BT task stacks, 17.2 KB controller IRAM |
| WiFi driver + softAP + mic DMA + ESP-NOW internal | ~80 KB | softAP exists only to park the ESP-NOW channel (see below) |
| G2-R + R1 BLE connects | ~15 KB | Arduino BLE lib remote-GATT mirror, plain `new` → internal via `ALWAYSINTERNAL=16384`; never freed on disconnect |
| Hijack menu entry | ~13 KB | Lazy spawn of lens-applier worker (8192 B stack + TCB + queue) at the worst moment |

Static (outside heap): 42.1 KB `.dram0.data` + 57.6 KB `.dram0.bss`. Top app blocks: G2_Ring 9.2 KB,
gEventRing 7.9 KB, camera stack ~15 KB (12 unused sensor drivers + to_bmp + sccb).

---

## Tier 0 — fixes the actual symptom (no bytes saved, do first)

**Eager-create the lazy G2 workers at `initG2Client()`** — VERIFIED SAFE.
- `g2_tap_disp` 10240 B: today created on the FIRST TAP via `tapDispatcherEnqueue` (G2_Glasses.cpp:14946-14948, :14988-14990); the 10.6 KB contiguous ask fails at ~14 KB free → rc=-1 (:14926). The no-inline-on-BTC fallback discipline (:14936-14940) is correct — keep it.
- `g2_page_swap_w` 8192 B: created on first hijack display (:14480/:14630) — the 27→14 KB cliff.
- `g2_ble_connect` 5120 B: created at first reconnect (:12174).
- `g2_session_w` is ALREADY eager (:11785, "paid early while DRAM is still contiguous") — same pattern, same file. `r1_owner` is also already boot-resident via `g2RingInit` (:11769). Leave `ring_spoof`/`ring_bridge_hb` lazy (user-toggled features).
- Net: ~23.5 KB becomes boot-resident, allocated when 120 KB+ is free and unfragmented. Failure mode eliminated deterministically.

## Tier 1 — safe wins (verified, low risk): ~25–30 KB DRAM + 36 KB flash

| Change | Bytes | Where | Notes |
|---|---|---|---|
| Disable 12 unused camera sensor drivers | 6,397 DRAM + 37,103 flash | `boards/xiao_s3.defaults` + regenerate/edit `build-xiao_s3/sdkconfig` | HW is OV2640 (Sense), code recognizes OV2640/3660/5640 PIDs only (System_Camera_DVP.cpp:484-497, OV3660 bench workarounds present — keep all 3 OV). Kill: OV7670, OV7725, NT99141, GC2145, GC032A, GC0308, BF3005, BF20A6, SC030IOT, HM1055, HM0360, MEGA_CCM (`CONFIG_*_SUPPORT=n`) |
| Defuse the recovery-path WiFi bomb | prevents ~44–51 KB demand | sdkconfig: `STATIC_RX_BUFFER_NUM 16→4`, TX→dynamic (type 1, NUM 32), `CACHE_TX 32→4` | System_WiFi.cpp:1038/:1147 call raw `WIFI_INIT_CONFIG_DEFAULT()`; normal boot goes through the Arduino override (WiFiGeneric.cpp:273-280) which already runs 4/32/dyn/4 — so this change has ZERO boot delta, it just makes the recovery re-init match the HW-validated numbers instead of ESP_ERR_NO_MEM-ing and leaving WiFi dead. Optionally also apply the overrides at both call sites via a shared helper |
| G2_Ring statics → `EXT_RAM_BSS_ATTR` | ~7,800 | G2_Ring.cpp:219-244 | sPacketAckQueue 2368, sRxQueueBytes 2240 (PSRAM-backed `xQueueCreateStatic` storage; keep the 92 B control block internal), sRawPayloadSlots 1028, sTransactionHistory 672, sActivePacketAck 584, sPendingRawSet 356, sSetupOwner 308, sRxFingerprints 256. All task-context (BTC enqueue at :1926 is task ctx, sole consumer r1_owner); leave the .data pair (sIntentQueue/sActiveTransaction) alone for now |
| Camera_DVP CLI reply-buffer sweep | 3,239 | System_Camera_DVP.cpp | ~40 function-local statics |
| captureBuf → ps_alloc | 4,096 | cmd capture path | per-exec transient |
| OLED remote-settings strdup registry → PSRAM | ~2,500 | | |
| sccb-ng `devices[254]` → 4 entries | ~2,000 | managed component patch | at most 2 live entries ever |
| sReply pair (LiveAudio 1024 + RaspberryPi 640) | 1,664 | | |
| SSE >512 B fallback malloc → ps_alloc | ~1,500 | WebServer_Events.cpp:140 | |
| ESP-NOW residual .bss | 1,448 | System_ESPNow.cpp | gPeerBuffer, gTopoStreams, gMeshRetryQueue, frag-ack |
| gMicRecordingControl / gSidStats / gAnchors | 608+576+512 | | all task-context POD |

Pattern to follow: `EXT_RAM_BSS_ATTR` as in System_MQTT.cpp:341 (332 existing uses across 73 files),
POD-only; `ps_alloc`/`PSRAM_JSON_DOC` for heap. Per project rule, verify on ALL board configs
(board-gated code hides compile breaks), then HW-test before commit.

## Tier 2 — needs-care (real wins, each needs a decision and/or HW test)

1. **`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` → +17,228 B internal** (biggest single knob). Controller
   IRAM (libbtdm_app 16,939 + libbtbb 289) moves to flash; S3 SRAM is unified so heap grows 1:1.
   HIGH risk until HW-tested: REQUIRES `CONFIG_SPI_FLASH_AUTO_SUSPEND=y` (currently unset) + flash-chip
   suspend support, because this firmware writes LittleFS while streaming G2 BLE audio. Test exactly
   that concurrency. Do not stack with other BLE changes in one test.
2. **Task-stack plan (−8.7 to −12.3 KB)**. Conservative variant: eager-create tap worker (Tier 0) +
   trim page-swap 8192→6144, cmd_exec 8192→6656, r1_owner 6144→4608, g2_ctrl_owner 6144→5120.
   Aggressive variant: merge tap+page-swap into ONE 10240 B worker via `xQueueSelectFromSet` over both
   existing queues (keeps gTapQueue fixed-size/alloc-free for the BTC producer — do NOT merge tap
   entries into heap LensUiJob*, that puts `operator new` on the BTC path) → extra −8,192 B. Verified
   tradeoff: merged worker head-of-line-blocks taps behind a page-swap CREATE-ack wait (up to 1.5 s)
   — decide if acceptable. Surviving shrinks land at 28–30% headroom, just above the project's 25%
   CRITICAL threshold, so the soak session (boot → G2+R1 → hijack walk → lens WiFi join → camera op,
   watching self-reported stack peaks) is load-bearing, not optional.
3. **SoftAP → STA-only ESP-NOW (~6–12 KB)**. The comment "ESP-NOW requires AP mode"
   (System_ESPNow.cpp:10544-10547) is FALSE — verified: all peers register `ifidx=WIFI_IF_STA` (:964),
   mesh identity is STA MAC at all 10 sites, no `WiFi.softAP()` call exists anywhere, and
   `espnowApplyChannel` already uses `esp_wifi_set_channel()` directly (:1028) which works in STA.
   Change :10528 to `WIFI_STA`, delete the ap_config block, fix System_WiFi.cpp:298/:1727.
   Needs mesh HW test (channel-follow when STA later associates; no re-pairing — identity unchanged).
4. **Arduino BLE lib PSRAM patch (~6–9 KB across 3 links, combined)**. Remote-GATT mirror is built
   with plain `new` inside BTC-task event handlers (BLEClient.cpp:721-726, :760 NULL-filter search),
   never freed on disconnect (clearServices() commented out, :441). Tier 1 of the patch: class-level
   `operator new` → PSRAM for Service/Characteristic/Descriptor + a `pruneServices(keep[])` after
   connect; kernel semaphore blocks stay internal. MUST be captured in docs/arduino-local-patches
   (established workflow, sits alongside the MTU patches). Do NOT switch getService to per-UUID
   filtered search — getServices() clears the whole map on entry and the app later does
   getService(6450) for audio after caching raw pointers; a clearing re-search would delete live objects.
   HW-verify notify/write timing across both temples + ring.
5. **`RX_BA_WIN 16→8` (~1.5 KB)** — the one WiFi sdkconfig knob that actually reaches boot runtime.
6. **BLE_50/DTM compile-out (~2.5 KB + flash)** — needs a 3-line gating patch to the vendored Arduino
   lib (only dead BLE_IDF.cpp uses ext-adv APIs).
7. **ESP-NOW heartbeat/tx stack trims (~2 KB)** — only after fresh HWM soak; see refuted item below.
8. **Finish PSRAM_JSON_DOC sweep** — 8 remaining plain JsonDocument sites (~4 KB transient).

## Contested — prior project decisions say no; this sweep's verifier says technically safe. USER CALL.

- **gEventRing → PSRAM (7,872 B)**: previously REJECTED in prior docs (spinlock/hot-path class).
  This sweep verified it is task-context-only, portMUX critical sections keep cache enabled, no
  DMA/secrets. Residual concern is real: `systemEventFetchSince` copies up to 48×164 B inside the
  critical section, and PSRAM lengthens that interrupt-masked window. Default: leave per prior
  decision unless the user overrides.
- **`SPIRAM_MALLOC_ALWAYSINTERNAL` 16384→4096 (~8–16 KB + contiguity relief)**: previously REJECTED.
  New facts from this sweep: no DMA/ISR hazards found in the 4–16 KB plain-malloc band (all audio/DMA
  paths already use explicit heap_caps); FreeRTOS stacks provably unaffected (pvPortMalloc hard-codes
  MALLOC_CAP_INTERNAL); and the mbedTLS objection is moot — `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` is
  ALREADY set, TLS state already goes to PSRAM today. Do NOT go to 2048 (BTC-notify-band allocs would
  migrate). Default: stands rejected until the user re-decides with these facts.

## Refuted — do NOT do (verifier kills, with reasons)

- **WiFi static-RX trim as a steady-state saving**: the sdkconfig 16/16/32 values are DORMANT — the
  Arduino override (WiFiGeneric.cpp:273-280) already runs 4/dyn/4 at boot. Value is ONLY the
  recovery-path fix (Tier 1). Claimed "25.6 KB boot pool" does not exist.
- **espnow_task 6656→4096**: System_TaskUtils.h:43-56 records the corrected (÷4) history — real peak
  ~4.37 KB under bond traffic, above 4096. The 1536 HWM in the log is idle traffic. Floor is ~5632.
- **BTC_TASK 8192→6144**: G2 mic audio decodes LC3 INLINE on the BLE notify (BTC) context — two
  1600 B pcm locals + 5× lc3_decode + LittleFS write can run in the SAME notification. Keep 8192.
- **sWizard → PSRAM**: holds `String wifiPassword`/`mqttPassword` — Arduino String SSO stores short
  secrets INLINE in the struct. Secrets never in PSRAM. (Same reason gSettings stays internal.)
- **gLoopPerf → PSRAM**: the perf meter would measure its own PSRAM jitter.

## Overlapping open work (from prior docs, still valid)

- STACK_TO_PSRAM Part 1: 72 stack-buffer candidates, 33,141 B — verified still unimplemented.
- LAZY audit residue: sensor_queue task, MLX scratch, static String reply holders, `esp_wifi_deinit`
  (~32 KB when WiFi fully off), dead gAutoMemo.
- Build-flag audit's ~36 KB is FLASH, not DRAM (only ~0.7 KB DRAM there).
- MQTT thermalJson[2048] size bug (correctness, not RAM) — still open.

## Realistic outcome

Tier 0 alone should stop the tap drops. Tier 0 + Tier 1 raises the steady-state floor by ~25–30 KB.
Adding the Tier 2 items that survive HW testing (RUN_IN_FLASH_ONLY + STA-only + BLE lib patch +
stack plan) gets to ~55–70 KB total — turning the 14 KB floor into ~70–85 KB.

Testing protocol per project rules: all changes verified across every board config; no commits until
the user HW-tests (boot → G2+R1 connect → hijack menu walk → taps → lens WiFi join → camera op →
live STT/mic stream + LittleFS write concurrency for the BLE flash change → mesh send/receive for
the STA-only change).
