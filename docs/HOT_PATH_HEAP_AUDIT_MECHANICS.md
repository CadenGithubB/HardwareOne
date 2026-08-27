# Hot-Path Heap Audit — Systemic Fragmentation Mechanics

_Companion to `HOT_PATH_HEAP_AUDIT.md`. Produced 2026-07-16 by the fragmentation-mechanics analyst of the
27-lane audit. **This analysis never reached the first synthesis pass** (the result payload was truncated),
which is why the main report over-indexes on String site-patching. Recovered from the run journal and
preserved verbatim below — it is the structural half of the audit and it partly CONTRADICTS the main
report's framing._

**Headline: this is a CONTIGUITY problem, not a byte problem.** String churn is the stirrer, not the killer.

> Caveats stated by the analyst: sdkconfig values and `.map` section sizes are read directly and are firm.
> The 65%/70-77% fragmentation figures are the developer's own recorded HW measurements quoted from
> `G2_Glasses.cpp` comments, not fresh measurements, and predate the 32.7 KB `.bss` reduction (so likely
> pessimistic now). The ALWAYSINTERNAL claim is a **mechanism argument, testable in one build — not tested.**
> No build or HW profile was run.

---

SYSTEMIC INTERNAL-DRAM FRAGMENTATION PICTURE — ESP32-S3 / hardwareone-idf

Headline: this codebase does NOT have a String-churn problem in its logging lane, and it does not have an "add reserve() everywhere" problem. It has a CONTIGUITY problem. The scarce resource is not free internal DRAM bytes, it is a contiguous internal-DRAM block big enough for a task stack. The code already knows this — two G2 call sites gate on largest-free-block and the comments record measured fragmentation of 65%+. Everything below follows from that.

=== 1. THE ACTUAL BUDGET ===

Verified from build/hardwareone-idf.map (2026-07-16 build):
  .dram0.data  0x6cf8  = 27,896 B (27.2 KB)
  .dram0.bss   0x12280 = 74,368 B (72.6 KB)
  → ~99.9 KB of internal DRAM is gone to statics before a single malloc runs.
(Consistent with docs/LAZY_ALLOCATION_AUDIT.md, which reports .dram0.bss cut 106,329 → 73,589 B — that 32.7 KB win is already banked and reflected in this map.)

Task stacks — the numbers below are BYTES (xTaskCreateLogged → xTaskCreatePinnedToCore; IDF portSTACK_TYPE=uint8_t). The `_WORDS` suffixes, the `~NNKB` comments, and `HardwareOne.cpp:693` (`constexpr uint32_t stackBytes = CMD_EXEC_STACK_WORDS * 4;`) are all 4x inflated. Per instructions I am not flagging this as a finding, but it MUST be understood when reading the budget, because the true stack cost is 1/4 of what System_TaskUtils.h advertises:
  App always-on: cmd_exec 8192 + sensor_queue 4096 + espnow_hb 6656 + espnow_tx 5120 + debug_out 4096 + sensor_bcast 4096 = 31.5 KB
  System:        arduino_loop 8192 + main 8192 + tcpip 3072 + sys_event 2304 + esp_timer 3584 + BTU 4352 + BTC 8192 = 37.0 KB
  → ~68.5 KB combined, all internal DRAM, all permanent.
Plus on-demand sensor pollers (thermal 6144, imu 4096, fmradio 4608, rtc 4096, tof/apds/gps/presence 3072 each) and the G2 client workers the audit measured at ~21.9 KB (g2-fsm 11,520 + g2_tap_disp 6,752 + g2_page_swap_w 3,616), which survive Bluetooth-off.

Mitigations already in place and correctly configured:
  CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y  (Bluedroid heap → PSRAM)
  CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y
  CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y    (WiFi/lwIP dynamic bufs → PSRAM)
  CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (the EXT_RAM_BSS_ATTR lever the audit leans on)
WiFi still pins static buffers internal by config: STATIC_RX 16, STATIC_TX 16, RX_MGMT 5, MGMT_SBUF 32, RX_BA_WIN 16. Those are DMA-capable and cannot move.

Net: the app is operating on roughly the 250–300 KB the ps_alloc header itself calls "the ~300 KB internal heap," minus ~100 KB statics minus ~70 KB permanent stacks. The empirical steady-state numbers the code records are the real story:
  G2_Glasses.cpp:14615-14624 — "largest ~9 KB with a model loaded even when total free is ~29-39 KB"  → ~70-77% fragmented
  G2_Glasses.cpp:14840-14850 — "largest ~31 KB, frag ~65%"
Note that 29-39 KB free is BELOW System_MemoryMonitor.cpp's own HEAP_WARNING_THRESHOLD of 40,960 B. The device's normal LLM-loaded state is already permanently in its own "pressured" band.

=== 2. THE INTERLEAVING MECHANISM (the part that actually matters) ===

Fragmentation here is not caused by allocation COUNT. It is caused by long-lived allocations being made lazily, at runtime, in the middle of short-lived churn. Four families, in order of damage:

(a) FIRST-USE RATCHETS ARE THE PRIMARY MECHANISM. docs/LAZY_ALLOCATION_AUDIT.md §4 catalogues these and they are exactly the wrong lifetime shape: allocated late (after the heap has been stirred by command/packet/log traffic), never freed. Each one places a permanent block at whatever arbitrary address was free at that moment, permanently bisecting the free list. The audit's own list — mapRender task (8.2 KB DRAM), cam_pwr worker, the ESP-NOW working set, gLLMResultBuf — plus §3's toggle-off leaks (g2-fsm/g2_tap_disp/g2_page_swap_w ~21.9 KB surviving BT-off, espnow_tx ~6.5 KB surviving closeespnow, HTTPS PEM Strings ~2.5 KB surviving httpstop). A lazily-allocated permanent block is strictly worse for fragmentation than an eager boot-time static, because the static lands in .bss at a known low address and never touches the heap at all. Laziness saves bytes and costs contiguity. That tension is not acknowledged in the audit and it is the single most important thing to say about it: several of its recommendations (e.g. #2 sEventBuf 7,872 B, #5 sensor_queue_task, #17 gAutoCache) convert a .bss static into a first-use heap ratchet. That trades 100% of a byte win for a permanent mid-heap divot. For anything that is allocated-on-first-use and then NEVER FREED, the correct move is EXT_RAM_BSS_ATTR (→ PSRAM, zero DRAM, zero fragmentation) or leaving it in .bss — not lazy DRAM heap.

(b) THE BIG-CONTIGUOUS CONSUMER IS THE PER-UI-ACTION TASK STACK. G2_Glasses.cpp spawns transient workers per user action: g2_bmp_view 6144, g2_cam_view 6144, g2_cam_stream 6144, g2_jpg_view 6144, g2_bmp_full 8192, g2_jpg_full 8192, g2_map_page (MAP_RENDER 8192), g2_llm_page (LLM_VIEW 6144), g2_net_scan 4096, g2_ai_test 4096, g2_live_page 4096, g2_live_text 4096. Every one needs a SINGLE CONTIGUOUS internal-DRAM block (FreeRTOS stacks are MALLOC_CAP_INTERNAL regardless of CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y, unless created via *WithCaps). These are the allocations that fail first, and they are the ones the largest-free-block guards exist to protect. This is also why the LLM_VIEW_STACK_WORDS comment says "Kept under the map's 32KB because this DRAM-fragmented config often has no 32KB block" — the developer sized a stack around the fragmentation rather than fixing it. This matches the standing guidance to avoid spawning a worker task per UI action; the fragmentation angle is the strongest argument for that rule, because it is not the 6 KB that hurts, it is the demand for 6 KB *unbroken*.

(c) STRING CHURN IS THE STIRRER, NOT THE KILLER. The 16-byte-granular exact-fit growth in changeBuffer means every `s += x` loop walks a block across the heap via realloc, freeing a trail of 16-byte-larger holes behind it. On its own that is self-healing — the holes coalesce. It becomes permanent damage only when a long-lived allocation from family (a) or (b) lands in the middle of the trail before it coalesces. So String churn's real role is to WIDEN THE WINDOW in which a ratchet allocation can land badly. This is why fixing Strings site-by-site has poor ROI here and why I am not returning a site list: the churn is the weather, the ratchets are the damage.

(d) THE LOGGING LANE IS ALREADY SOLVED — DO NOT "FIX" IT. Worth stating explicitly so effort is not wasted re-deriving it. System_Debug.cpp:488-520 pre-allocates a fixed pool of `gDebugQueueSize` DebugMessage structs (128 slots with PSRAM) via ps_alloc into PSRAM, and recycles them through a free-queue/output-queue pointer pair. debugQueuePrintf (:646-670) formats into a stack `char line[DEBUG_MSG_SIZE]` and hands off — zero heap. BROADCAST_PRINTF (System_Debug.h:889) uses a stack `char _bpBuf[256]`. The drain loop's serial/web/OLED sinks all use fixed char buffers. gBLEOutputBuffer is a String but it is `.reserve(BLE_OUTPUT_BUFFER_MAX)`-ed once at init (:550) and cleared with `= ""`, which under Arduino String cannot shrink capacity (reserve() only grows), so it holds one stable 1,040 B block for the life of the device. This is textbook. The highest-frequency output path in the firmware is already a fixed-slot pool with a String-free formatting lane and correct reserve discipline. The residue: appendLineWithCap() paths at :280/:295/:305 build a `String line = buildTimestampPrefix(); line += msg->text;` per [ERROR]/[EVENT]/[EVLOG] line, on the debug_out task. That is a genuine >14-char two-step String on a repeated path, but it is deliberately low-volume (2s dedupe window on errors; [EVENT]/[EVLOG] are discrete lifecycle records, not polling), and debug_out's own stack comment records that this file-I/O path is what drives its 16 KB HWM. Low priority.

=== 3. HEAP TRACING / POISONING / WATERMARKS ===

Tracing and poisoning: ALL OFF.
  CONFIG_HEAP_POISONING_DISABLED=y  (LIGHT and COMPREHENSIVE both unset)
  CONFIG_HEAP_TRACING_OFF=y         (STANDALONE and TOHOST both unset)
  CONFIG_HEAP_USE_HOOKS unset
  CONFIG_HEAP_TASK_TRACKING unset
  CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS unset
So there is no allocation attribution available at the IDF level. Note CONFIG_HEAP_TASK_TRACKING is the single highest-value flag NOT enabled — it would answer "which task owns the blocks bisecting my heap," which is precisely the open question, at the cost of a few bytes per allocation. Worth a diagnostic build.

Watermark reporting, by contrast, is GOOD and already fragmentation-aware — this is a real strength:
  - System_MemoryMonitor.cpp:150-215 sampleMemoryState() correctly separates internal DRAM from the combined heap and from PSRAM, tracks heap_caps_get_minimum_free_size and heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT), and — crucially — computes and prints an explicit fragmentation percentage (`100 - largest*100/free`, warns >30%). The device measures its own fragmentation.
  - HardwareOne.cpp:1107-1127 heapLogSummary() reports dram_free/dram_largest/dram_maxalloc/dram_min plus PSRAM largest. Its comment records a real bug already fixed: MALLOC_CAP_8BIT alone was matching PSRAM and reporting ~1.9 MB as the DRAM largest block.
  - System_MemUtil.h has gPsAllocFallbacks + __psAllocReportFallback() — a counter+log for every ps_alloc that wanted PSRAM and silently got internal DRAM. This is the single best-designed diagnostic in the memory subsystem, because ps_alloc falling back to internal is BOTH a byte hit AND a fragmentation hit (a >16 KB block, sized for PSRAM, landing in a ~250 KB DRAM heap). If gPsAllocFallbacks is ever nonzero in the field, that alone explains a fragmented heap. RECOMMENDATION: surface gPsAllocFallbacks in the periodic [MEMSAMPLE] line — it is currently only observable via an ESP_LOGW and a global nobody prints.
  - Note one gap: the tagged ps_realloc overload (System_MemUtil.h, last of the four) is missing the `if (p2 && wantPS) __psAllocReportFallback(...)` call that its three siblings have. Tagged ps_realloc fallbacks are therefore invisible to the counter. Minor, but it is a hole in the one diagnostic that matters most.

=== 4. IS CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 WELL-TUNED? ===

No. 16384 is the IDF default and it is the wrong default for this device. It is arguably the single highest-leverage line in sdkconfig.

What it means here: every malloc/new/String allocation under 16 KB — which is essentially ALL of them, since the only things over 16 KB are already explicitly ps_alloc'd — is served from internal DRAM. The threshold is set so high that it is effectively inert: it is not routing anything to PSRAM. Plain malloc on this device is, in practice, "internal DRAM allocator."

Companion setting: CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768. Only 32 KB of internal DRAM is held back for allocations that MUST be internal (DMA, and the internal-only fallback pool). Given that WiFi static RX/TX buffers and every task stack are internal-mandatory, 32 KB is thin.

What changes if lowered — and this is the key asymmetry: lowering ALWAYSINTERNAL does NOT primarily save bytes, it PRESERVES CONTIGUITY. Pushing the mid-size (1–16 KB) transients — JSON docs, command output buffers, String bodies for HTML/JSON/paths, reassembly buffers — out to PSRAM means they stop landing in and stirring the DRAM free list. The DRAM heap then holds mostly stacks and DMA buffers, which are long-lived and similar-sized, i.e. the shape that does not fragment. That is a direct attack on the "largest block 9 KB / free 39 KB" state.
  - Lower to ~4096: mid-size transients relocate to PSRAM. Expect the largest-free-block figure to recover substantially. Cost: those allocations get PSRAM latency, and the device is already documented as PSRAM-bus-bound on quad — so anything on the LLM inference inner loop must NOT be caught by this. It won't be (weights/KV are explicitly ps_alloc'd already, and inference scratch is either stack or >16 KB), but this needs measuring, not assuming.
  - Lower to ~2048: more aggressive; starts catching String bodies for log lines/paths. Higher risk of a latency surprise on I2C/packet paths.
  - CAUTION, non-obvious: lowering this makes plain `malloc` return PSRAM pointers. Any code that assumes malloc'd memory is DMA-capable will break at runtime, not compile time. Before touching this, audit for malloc'd buffers handed to DMA peripherals (I2S mic, SPI display, camera). This is the real risk of the change and the reason it hasn't been done.
  - SECRETS INTERACTION — a hard constraint, not a preference: flash encryption is OFF, so PSRAM is plaintext on an externally probeable chip. Lowering ALWAYSINTERNAL silently relocates SHORT allocations to PSRAM, and short allocations are exactly what key material, tokens, and typed passwords look like (an X25519 secret is 32 B; a session key is 16-32 B; a password String is well under 16 KB). Today those are protected by accident — they're small, so ALWAYSINTERNAL keeps them internal. Lowering the threshold removes that accidental protection wholesale and with no diagnostic. Anything holding secrets must be moved to explicit heap_caps_malloc(MALLOC_CAP_INTERNAL) BEFORE this knob is touched. The audit already flags gSc, gMeshDerivedKeys/gIdentity, the HTTPS private key, and gOledEspNowState's password members, and notes gSessions (ESP-NOW AEAD session keys) is ALREADY on the PSRAM heap in violation. Fix that first regardless.

Recommended sequencing: (1) fix gSessions + audit DMA-fed mallocs; (2) drop to 4096 in a diagnostic build; (3) compare [MEMSAMPLE] fragmentation % and dram_largest before/after; (4) watch gPsAllocFallbacks and LLM tok/s for the latency regression.

=== 5. HIGHEST-LEVERAGE STRUCTURAL FIXES (ranked) ===

1. LOWER CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16384 → 4096), gated on the secrets + DMA audit above. One line; systemically removes mid-size transients from the DRAM free list. Nothing else has this leverage-to-effort ratio. Everything below is a subset of what this does automatically.

2. PREFER EXT_RAM_BSS_ATTR OVER LAZY DRAM HEAP for anything never-freed. The audit's #3 (gEventRing 7,872), #6 (commandRegistry 4,096), #11 (gAllocTracker 2,560), #20 (gFrameRing 1,152), #22 (gPeerIdentities 896), and the #9/#13 G2 static-buffer sweeps (~4.8 KB) are the right shape: they remove DRAM entirely without creating a heap ratchet. Do these BEFORE the byte-motivated lazy-heap conversions (#2 sEventBuf, #17 gAutoCache), which trade contiguity for bytes and are net-negative for fragmentation. Explicitly reconsider those two.
   Free win, no fragmentation tradeoff: audit #15 — add `const` to the ~115 static httpd_uri_t structs → 1,696 B moves to flash rodata (httpd deep-copies them). Correctly called "the cleanest single win."

3. REPLACE PER-UI-ACTION G2 WORKER TASKS WITH ONE LONG-LIVED G2 WORKER + A JOB QUEUE. There are ~12 spawn sites (G2_Glasses.cpp:14310/14634/14860/15238/15680/15994/16278/16401, plus net_scan/ai_test/live_page/live_text) each demanding a fresh 4–8 KB CONTIGUOUS internal block at an arbitrary moment. Collapsing them onto one worker sized for the deepest job (the 8192 full-viewers) allocates that block ONCE, early, and reuses it forever. This converts the firmware's largest contiguity demand into a boot-time allocation, and it deletes the need for the largest-free-block preflight guards at :14624 and :14850 — those guards are a symptom, and this fix removes the disease. It also directly serves the existing "avoid spawning a worker task per UI action" rule and subsumes the audit's §3 G2 teardown work (~21.9 KB), whose §6.3 blockers exist precisely because these workers have ad-hoc lifecycles.

4. GIVE THE VIEWERS A SHARED SCRATCH BUFFER IN PSRAM. The image/map/JPG viewers each need a decode/render buffer. One ps_alloc'd PSRAM scratch, owned by the single worker from #3 and reused per job, removes the last mid-size DRAM transients from the UI path. Pairs naturally with #3 — same owner, same lifetime.

5. RESERVE() DISCIPLINE ONLY WHERE A LOOP DEMONSTRABLY BUILDS >14 CHARS, and only after #1. Currently 33 reserve() call sites against 119 `+=` occurrences in WebServer_*.cpp alone. But note the ordering logic: if ALWAYSINTERNAL drops to 4096, most of those String bodies are in PSRAM and their realloc churn stops mattering for DRAM contiguity. Doing reserve() first is a lot of diff for a fraction of the benefit. Do #1, re-measure, then reserve() only what still shows up.

6. INSTRUMENT BEFORE OPTIMIZING FURTHER: enable CONFIG_HEAP_TASK_TRACKING in a diagnostic build, and print gPsAllocFallbacks in the periodic [MEMSAMPLE] line. The fragmentation % is already computed (System_MemoryMonitor.cpp:205-213) — the missing half is WHO. Also close the tagged-ps_realloc fallback-reporting hole noted in §3.

COVERAGE / CAVEATS: All sdkconfig values and the .map section sizes are read directly and are firm. Stack sizes are the literal byte arguments to xTaskCreate. The fragmentation percentages (65%, ~70-77%) and the "largest ~9 KB / free ~29-39 KB" figures are the developer's own recorded HW measurements quoted from G2_Glasses.cpp comments, not my measurements — they should be re-confirmed on current firmware, since both predate the audit's 32.7 KB .bss reduction and are therefore likely pessimistic now. I did not run a build or profile on hardware. The claim that lowering ALWAYSINTERNAL improves contiguity is a mechanism argument from the allocator's routing rule; it is testable in one build and should be tested rather than trusted. I did not exhaustively audit for DMA-fed plain-malloc buffers — that audit is a prerequisite for recommendation #1 and is the main thing standing between this analysis and a safe change.


---

## Completeness critic — coverage gaps

_Swept first-party files that produced ZERO findings, on the theory that absence is suspicious. NOT independently verified._


### `components/hardwareone/G2_Glasses.cpp:3328` — buildG2StatusSnapshot

- **Pattern:** oversized-transient · **Tier:** per-tick · **Arena:** internal-dram · **Confidence:** H
- **Summary:** The Status-page snapshot builder allocates a 256-byte internal-DRAM String purely as an intermediate, then strncpy's it into the caller's buffer and frees it — a same-size alloc/free pair every 5 s while the Status page is open, in a function that already receives (out, cap) and needs no String at all.
- **Why it's hot:** Traced the full driver chain: kStatusPage (G2_Glasses.cpp:3692) registers buildG2StatusSnapshot as its builder with /*liveIntervalMs=*/ 5000 and /*liveRender=*/ renderStatusCompound (G2_Glasses.cpp:3704, 3707). liveTextWorker's tick loop calls gLiveTextRenderFn() (G2_Glasses.cpp:10425) => renderStatusCompound() (G2_Glasses.cpp:10072) => buildG2StatusSnapshot(bodyBuf, 2048) (G2_Glasses.cpp:10107). So this runs every 5 s for as long as the G2 Status page is on the lens — a genuine steady-state tick, not a one-shot.
- **Cost:** None / None allocs
- **Evidence:** `  String s;
  s.reserve(256);
...
  // Truncate cleanly into caller's buffer. snprintf-style guarantee.
  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';`
- **Fix:** Delete the String entirely and append straight into the caller's buffer with a running snprintf cursor (the pattern this same codebase already uses in G2_Page_Sensors.cpp:820-836 and System_R1_Protocol.cpp:523-533). The body is 4 short lines; `size_t off = 0; off += snprintf(out + off, cap - off, ...)` per section removes the 256 B alloc/free pair from the tick completely and drops the strncpy copy. Note the reserve(256) is also the only thing preventing 16-byte-granular realloc churn here, so removing the String removes both risks at once.

### `components/hardwareone/OLED_Mode_Remote.cpp:110` — displayBondStatus

- **Pattern:** string-return-by-value · **Tier:** per-event · **Arena:** internal-dram · **Confidence:** M
- **Summary:** Each Remote-page redraw calls BondedPeer::peerName(), which returns a String by value that is either a 17-char MAC ('AA:BB:CC:DD:EE:FF') or a device name — both past the 14-char SSO ceiling, so every redraw allocates and frees an internal-DRAM block for a value that changes only on re-pair.
- **Why it's hot:** displayBondStatus/displayRemoteMode are OLED mode draw functions reached from the updateOLEDDisplay dispatch, which is throttled to gSettings.oledUpdateInterval (default 125 ms => 8 Hz, OLED_Utils.cpp:3319). Renders are gated by oledIsDirty() (OLED_Utils.cpp:2751), which returns true on gInputCache.seq changes — bumped per gamepad/encoder detent (i2csensor_seesaw.cpp:647, i2csensor_ano_encoder.cpp:644). So while the user is navigating the Remote page this redraws at up to 8 Hz, each redraw re-resolving the peer name from scratch. Honest tier is per-event (per input / per state change), NOT free-running per-frame — the dirty gate does suppress idle redraws.
- **Cost:** None / None allocs
- **Evidence:** `  String displayName = BondedPeer::peerName();`
- **Fix:** peerName() is a pure function of gSettings.bondPeerMac plus the ESP-NOW name registry — both of which change only on pair/unpair/identity-arrival. Resolve it once into a static char[33] cache invalidated on those events, and have the draw path read the cache. Alternatively give BondedPeer a `bool peerName(char* out, size_t cap)` overload so the draw path never materializes a String; the value is displayed via display->println() and never needs String semantics.

### `components/hardwareone/OLED_ESPNow.cpp:376` — oledEspNowDisplayStatus

- **Pattern:** redundant-copy · **Tier:** per-event · **Arena:** internal-dram · **Confidence:** M
- **Summary:** The draw path deep-copies gSettings.espnowDeviceName into a local String on every redraw, then on the >15-char branch allocates a second String via substring() — two internal-DRAM alloc/free pairs per redraw to render a device name that is effectively immutable at runtime.
- **Why it's hot:** oledEspNowDisplayStatus (OLED_ESPNow.cpp:338) is the ESP-NOW OLED mode's draw function, invoked from the updateOLEDDisplay 125 ms dispatch (OLED_Utils.cpp:3319) whenever oledIsDirty() (OLED_Utils.cpp:2751) is true — i.e. up to 8 Hz while the user is on this page and input/sensor state is moving. Same dirty-gate caveat as the Remote page: idle is suppressed, active navigation is not.
- **Cost:** None / None allocs
- **Evidence:** `  String name = gSettings.espnowDeviceName.length() > 0 ? gSettings.espnowDeviceName : "(none)";
  if (name.length() > 15) { name = name.substring(0, 14); name += '~'; }`
- **Fix:** No copy is needed — the value is only measured and printed. Use a stack buffer: `char name[17]; snprintf(name, sizeof(name), "%s", gSettings.espnowDeviceName.length() ? gSettings.espnowDeviceName.c_str() : "(none)");` then overwrite name[14]='~', name[15]='\0' when strlen exceeds 15. That renders identically with zero heap traffic. (Note: the substring(0,14) result itself is 14 chars and lands in SSO — it is the initial full-name copy that is the real allocation.)

### `components/hardwareone/OLED_Mode_FileBrowser.cpp:885` — displayFileBrowserRendered

- **Pattern:** redundant-copy · **Tier:** per-event · **Arena:** internal-dram · **Confidence:** M
- **Summary:** Every visible directory entry is copied into a String on every redraw solely to measure length and truncate for display; filenames past 14 chars allocate internal DRAM, multiplied by the number of rows drawn and repeated on each scroll step.
- **Why it's hot:** displayFileBrowserRendered (OLED_Mode_FileBrowser.cpp:724) is the file-browser mode draw function on the updateOLEDDisplay 125 ms path (OLED_Utils.cpp:3319), gated by oledIsDirty(). Scrolling the browser bumps gInputCache.seq per encoder detent (i2csensor_ano_encoder.cpp:644), marking dirty and redrawing at up to 8 Hz. The String construction sits inside the visible-entry loop, so the cost is (allocs x entries-on-screen) per redraw rather than one per redraw.
- **Cost:** None / None allocs
- **Evidence:** `      String name = String(entry.name);
      if (name.length() > 13) {
        name = name.substring(0, 10); name += "...";
      }`
- **Fix:** entry.name is already a C string — the copy buys nothing. Replace with a stack buffer inside the loop: `char name[14]; if (strlen(entry.name) > 13) { memcpy(name, entry.name, 10); memcpy(name + 10, "...", 4); } else { snprintf(name, sizeof(name), "%s", entry.name); }` and pass name to the print call. Removes all per-row heap traffic from the scroll path.


**Analyst notes:**

COVERAGE METHOD: enumerated all 319 first-party files in components/hardwareone/ + main/, diffed against the 27 files the sweep produced findings for, then ranked the zero-finding remainder by size and by density of alloc-shaped tokens (+=, String(, malloc, new, snprintf). Chased the top ~20 candidates to their actual driver (task loop / dispatch site / callback), not just by eyeballing the function.

THE BIG GAP: G2_Glasses.cpp — 18,352 lines, the largest first-party file in the repo, and the sweep produced ZERO findings for it. That absence was not legitimate; finding #1 came out of it. Worth noting the sweep DID cover several G2_Page_*.cpp satellites but skipped the hub.

LEGITIMATE ABSENCES (verified clean — do NOT chase these again):
- G2_Glasses.cpp live workers (livePageWorker:9710, liveTextWorker) — textbook discipline: rows/ptrs/textBuf are ps_alloc'd ONCE in the worker prologue, reused across every tick, freed at exit. Nothing per-tick. renderStatusCompound's per-tick ps_alloc(2048) is PreferPSRAM, so per ground truth #1 it is much less concerning; I did not report it, though it could trivially be made static.
- System_Battery.cpp — looked like a strong hit (String(kBatteryLogPath) built twice per append plus more on the rotation path) but kBatteryLogPath = "/battery.csv" is 12 chars => SSO, never allocates. batteryLogTick is also interval-floored at 5 s. Genuinely free. Good example of ground truth #2 killing a plausible-looking finding.
- OLED_Utils.cpp updateOLEDDisplay — the render body (3340-3500) has no String use at all, and the path is double-gated (125 ms throttle + oledIsDirty()). Clean.
- System_Maps.cpp — the hot map render/decode path (1305-1379, 3771-3787) is raw pointer walks + snprintf into stack buffers. Every String in the file is behind a cmd_* CLI entry point (per-command, human-driven). Clean where it counts.
- G2_Ring.cpp / System_R1_Protocol.cpp — the per-packet BLE notify path (ringNotifyThunk:465 -> ringDumpFrame) is entirely stack buffers + snprintf cursors, including the hex dumps. R1Encoder returns fixed R1Frame structs by value. Clean.
- System_LLM_Model.cpp — the three mallocs (120, 128, 770) are all model-load/dequant, i.e. init-only. Correctly noise.
- System_I2C.cpp sensorQueueProcessorTask:2618 — despite the `while(true)` this only drains sensor-START requests; it is an init/enable batch, not steady state.
- System_TaskUtils.cpp:127-154 — the worst raw String-append chain I found anywhere (~14 `+=` plus String(millis()) temporaries, so ~9 reallocs under the 16-byte growth policy), but it lives in xTaskCreateLogged. Task creation is rare/boot-time, and per the project's own "avoid per-action tasks" rule it stays rare. Correctly excluded — flagging it would be exactly the init-only padding the brief warns against.
- OLED_SettingsEditor.cpp:120-126 — `String maxStr = String(maxVal)` on a draw path looked promising, but these are ints ("100" = 3 chars) => SSO. Free.
- System_ESPSR.cpp — normalizePhrase() returns String by value and is called inside registry-iteration loops (756-757, 790-792), which is a genuinely bad shape. Not reported because (a) the driver is per-utterance (human speech), not steady state, and (b) the compared fields (voiceCategory/voiceTarget: "sensor", "thermal") are mostly under 14 chars => SSO. Real code smell, no real fragmentation.
- System_MeshPeers.cpp displayName() returns String by value, but its only caller is rebuildRoomDeviceList (OLED_ESPNow.cpp:440), which is event-driven on topology change, not a draw path.

IMPORTANT CAVEAT ON THE THREE OLED FINDINGS (#2-#4): I want to be explicit rather than oversell these. My first read was that they were per-frame at 8 Hz, which would have made them severe. That was wrong. updateOLEDDisplay gates on oledIsDirty(), and I traced both dirty sources: gSensorStatusSeq is bumped only by sensorStatusBumpWith on sensor start/stop (System_I2C.cpp:2366), NOT per sensor reading; gInputCache.seq bumps only on gamepad/encoder input change. So an idle OLED does not redraw and these cost nothing at rest. They are real internal-DRAM churn only while a user is actively navigating that page (~8 Hz burst). I tiered them per-event and set confidence M accordingly. They are correct findings and the fixes are cheap, but they should rank below anything on a true per-packet/per-token path.

UNRESOLVED: I did not chase OLED_Mode_UnifiedMenu.cpp:473 (String peerName = BondedPeer::peerName() in displayUnifiedMenu) — same shape and same fix as finding #2, same dirty-gate caveat. It is the main menu so it is seen more often than the Remote page; worth folding into the #2 fix rather than treating separately. Also did not deep-dive System_ESPNow_Handlers_Crypto.cpp / _Sessions / _FsList / _Files — all are on genuine per-packet paths, but each showed only 4-8 alloc-shaped tokens on grep, so the prior for a miss is low; they would be the next place I'd look if you want another pass.


---

## Pattern critic — patterns outside the String-concat categories

_Hunted fragmentation drivers the sweep's categories would have missed entirely. NOT independently verified._


### `components/hardwareone/System_Utils.cpp:804` — readText(const char* path, String& out)

- **Pattern:** string-concat-in-loop · **Tier:** per-tick · **Arena:** internal-dram · **Confidence:** H
- **Summary:** readText fills the output String via the vendored Stream::readString(), which appends ONE CHAR AT A TIME with no reserve; combined with String's 16-byte-granular exact-fit growth this reallocs every 16 bytes of file — the whole automations.json is rebuilt through ~128 realloc/memcpy steps on every automation tick.
- **Why it's hot:** HardwareOne.cpp:2212-2217 main loop calls schedulerTickMinute() when needFullTick, which includes `automationEventsPending() && (nowAuto - lastAutoCheck >= 250)` => up to 4 Hz whenever any subscribed event is firing, and unconditionally every 60000ms. schedulerTickMinute (System_Automation.cpp:3699) calls readText(AUTOMATIONS_JSON_FILE, json) on every full tick.
- **Cost:** sizeof(automations.json) — the entire file, grown incrementally (final block e.g. ~2KB) / fileSize/16 reallocs (~128 for a 2KB file), each a realloc+memcpy of a growing block allocs
- **Evidence:** `bool readText(const char* path, String& out) {
  out = "";
  ...
  File f = VFS::open(String(path), "r");
  if (!f) {
    return false;
  }
  out = f.readString();`
- **Fix:** The append loop is invisible at this call site — it lives in components/arduino/cores/esp32/Stream.cpp:229 (`String ret; int c = timedRead(); while (c >= 0) { ret += (char)c; ... }`), and File (components/arduino/libraries/FS/src/FS.h:46) does NOT override readString(). Do not call readString(). This repo already has the correct shape in readTextLimited (System_Filesystem.cpp:1635-1647): `out.reserve(maxBytes)` + 512-byte chunked f.read() into a static scratch buffer. Rewrite readText to `out.reserve(f.size()); ` then chunk-read, which collapses ~128 reallocs to 1. This is the single highest-leverage fix in the sweep.

### `components/hardwareone/System_Automation.cpp:520` — rebuildAutoCache()

- **Pattern:** string-concat-in-loop · **Tier:** per-tick · **Arena:** internal-dram · **Confidence:** H
- **Summary:** rebuildAutoCache re-reads automations.json through the same readText/readString char-by-char treadmill, so every automation tick pays the ~128-realloc cost TWICE (once at :3699 for the scan, once at :520 for the cache refill).
- **Why it's hot:** rebuildAutoCache() is the LAST statement of schedulerTickMinute() (System_Automation.cpp:4028), which is driven by HardwareOne.cpp:2217 at up to 4 Hz. Verified sole call site: grep 'rebuildAutoCache()' returns only the definition at :518 and the call at :4028.
- **Cost:** sizeof(automations.json) again — a SECOND full copy per tick / fileSize/16 reallocs (~128 for a 2KB file) allocs
- **Evidence:** `static void rebuildAutoCache() {
  String json;
  if (!readText(AUTOMATIONS_JSON_FILE, json)) {
    gAutoCacheCount = 0;
    gAutoCacheValid = true;
    return;
  }
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) {`
- **Fix:** Fixing readText (finding #1) halves automatically. Beyond that: schedulerTickMinute already has the file text in its local `json` String at :3699 — pass it into rebuildAutoCache(const String& json) instead of re-reading from flash, eliminating a whole redundant file read + parse per tick. The comment at :516 justifies the re-read as capturing 'post-fire nextAt updates', which only matters on ticks that actually fired and rewrote the file — gate the re-read on that.

### `components/hardwareone/System_Automation.cpp:3730` — schedulerTickMinute()

- **Pattern:** redundant-copy · **Tier:** per-tick · **Arena:** internal-dram · **Confidence:** H
- **Summary:** The tick loop deep-copies each automation object out of the already-in-RAM json String via substring(), purely to run indexOf() searches against it — the offsets objStart/objEnd are already known, so the copy buys nothing but an internal-DRAM allocation per automation per tick.
- **Why it's hot:** Inside the `while (true)` per-automation scan loop of schedulerTickMinute (System_Automation.cpp:3706+), driven by HardwareOne.cpp:2217 at up to 4 Hz. Executes once per automation object in the file, per tick.
- **Cost:** full text of one automation object (~200-800 B) per automation, per tick / 1 substring alloc per automation for `obj`, plus 1 for `idStr`, plus a `condition` substring — so ~3N allocs/tick for N automations allocs
- **Evidence:** `String idStr = json.substring(colon + 1, idValEnd);
    idStr.trim();
    long id = idStr.toInt();

    String obj = json.substring(objStart, objEnd + 1);`
- **Fix:** Operate on offsets into `json` rather than materializing `obj`: the subsequent lookups are all `obj.indexOf("\"key\"")` which can be expressed as `json.indexOf("\"key\"", objStart)` bounded by objEnd. `idStr`/`toInt()` can be replaced by strtol(json.c_str() + colon + 1, ...) with no allocation at all. This removes ~3N internal-DRAM allocs per tick.

### `components/hardwareone/System_Automation.cpp:3927` — schedulerTickMinute()

- **Pattern:** string-temporary-chain · **Tier:** per-tick · **Arena:** internal-dram · **Confidence:** H
- **Summary:** Builds the condition wrapper with a String temporary chain, materializing intermediates and allocating from internal DRAM — while line 820 of the SAME FILE already does the identical job allocation-free with a stack buffer + snprintf.
- **Why it's hot:** Inside schedulerTickMinute's per-automation scan loop (enclosing function confirmed at System_Automation.cpp:3630), driven by HardwareOne.cpp:2217 at up to 4 Hz. Runs for every automation carrying a non-empty `condition`.
- **Cost:** ~20-100 B across 2-3 temporaries per conditioned automation, per tick / 2-3 (temp for "IF " + condition, temp for + " THEN _", final assignment) allocs
- **Evidence:** `String wrapped = "IF " + condition + " THEN _";
          bool conditionMet = evaluateCondition(wrapped.c_str());`
- **Fix:** Copy the established pattern from System_Automation.cpp:828-830 verbatim: `char wrapped[384]; snprintf(wrapped, sizeof(wrapped), "IF %s THEN _", condition.c_str()); evaluateCondition(wrapped);`. evaluateCondition already takes const char* (declared :198, 'const char* input, stack-based parsing' per the file's own header comment at :18), so no signature change. The same fix applies to the sibling site at System_Automation.cpp:1762 (cmd_automation_run, per-command tier — lower priority).

### `components/hardwareone/System_VFS.cpp:397` — VFS::open(const String& path, const char* mode, bool create)

- **Pattern:** string-param-by-value · **Tier:** per-tick · **Arena:** internal-dram · **Confidence:** H
- **Summary:** VFS::open takes const String&, forcing every const char* caller to materialize a heap String temporary; normalize() then immediately copies it AGAIN — so ~2 internal-DRAM allocations are paid before a single byte is read, on every file open including the per-tick automation reads.
- **Why it's hot:** Every file operation in the firmware routes through here. On the hot path specifically: readText (System_Utils.cpp:800) calls `VFS::open(String(path), "r")`, and readText itself runs twice per automation tick (System_Automation.cpp:3699 and :520) at up to 4 Hz via HardwareOne.cpp:2217.
- **Cost:** ~2x path length (e.g. "/automations.json" = 17 chars, over SSO's 14-char limit, so both copies allocate) / 2+ per open — one for the String(path) temporary at the call site, one for `String p = path` inside normalize() allocs
- **Evidence:** `File open(const String& path, const char* mode, bool create) {
  String p = normalize(path);
  if (p.indexOf("..") >= 0) return File();  // reject traversal`
- **Fix:** Two independent wins. (1) At the call site: readText's `VFS::open(String(path), "r")` wraps a const char* that is already a string literal — an overload taking const char* would let normalize() build exactly one String instead of two. (2) normalize() (System_VFS.cpp:321) unconditionally does `String p = path; p.trim();` even when the path is already canonical — which is the overwhelmingly common case for the compile-time literals (AUTOMATIONS_JSON_FILE, SETTINGS_FILE, USERS_JSON_FILE) that drive the hot paths. Add an early-out that returns the input untouched when it already starts with '/', has no "//" and no trailing slash, skipping the copy entirely.

### `components/hardwareone/System_ESPNow.cpp:5253` — v4 CMD RX handler (allocates V3CmdAsyncCtx, freed in v4CmdResultCallback)

- **Pattern:** same-size-alloc-free-churn · **Tier:** per-command · **Arena:** internal-dram · **Confidence:** M
- **Summary:** Every inbound remote command mallocs a fixed 76-byte context from internal DRAM and frees it on a different task later, producing a small same-size alloc/free pair whose alloc and free are separated in time — the classic shape for wedging small holes between longer-lived blocks.
- **Why it's hot:** Sits in the ESP-NOW V4 CMD receive handler, immediately after the stream-session slot acquisition at :5240; runs once per remote command frame received from a bonded/mesh peer. Freed asynchronously in v4CmdResultCallback (:5040) once cmd_exec completes.
- **Cost:** 76 B (uint8_t[6] + char[32] + char[32] + uint32_t, struct at :5027) / 1 malloc + 1 deferred free per received command allocs
- **Evidence:** `V3CmdAsyncCtx* asyncCtx = (V3CmdAsyncCtx*)malloc(sizeof(V3CmdAsyncCtx));
  if (!asyncCtx) {
    BROADCAST_PRINTF("[ESP-NOW] CMD handler: alloc failed");
    destroyStreamSession(msgId);
    return;
  }`
- **Fix:** The size is fixed and the concurrency is already bounded by the stream-session slot pool acquired just above (:5240 bails with 'no session slots'). Replace the malloc with a static array of V3CmdAsyncCtx indexed off the session slot — the lifetime is already exactly the session's, so the pool needs no new bookkeeping and the free in v4CmdResultCallback becomes a flag clear. Lower severity than the automation findings: 76 B and only as fast as commands arrive, but it is free to eliminate given the pool already exists.


**Analyst notes:**

HEADLINE: The #1 finding is structurally invisible to any first-party `s +=` grep. readText (System_Utils.cpp:804) says `out = f.readString()` — one innocuous line. The char-by-char append loop is in VENDORED code (components/arduino/cores/esp32/Stream.cpp:229), and File (FS.h:46) does NOT override readString(), so it inherits Stream's `while (c >= 0) { ret += (char)c; }` with no reserve. Against this repo's 16-byte-granular growth that is fileSize/16 reallocs per call, twice per automation tick at up to 4 Hz. Recommend the sweep re-run with vendored-API-shape awareness: any first-party call to readString/readStringUntil is a hidden concat loop. Note readTextLimited (System_Filesystem.cpp:1635) already implements the correct reserve+chunk pattern — the fix is to copy an existing in-repo idiom, not invent one.

PATTERNS EXPLICITLY HUNTED AND FOUND CLEAN (negative results, reported so they are not re-swept):
- std::function heap capture: exactly ONE std::function in the entire first-party tree (LLMTokenCallback, System_LLM.h:119). Both call sites are safe — System_LLM.cpp:207 is a captureless lambda (no alloc), :2687 captures a single reference (8 B, fits libstdc++'s 16 B SBO). It is passed by value into llmGenerate but that is per-generation, not per-token. NOT a finding.
- STL container growth: std::vector/std::map are barely used (11 hits total, all init-only or per-command listings). gMeshTopology is the only persistent vector and is never rebuilt on a periodic path. std::string and std::to_string: ZERO occurrences. NOT a finding.
- ESP-NOW RX per-packet path: onEspNowDataReceived (System_ESPNow.cpp:972) is exemplary — bounds-checks into a preallocated ring, zero allocation in the callback. Reassembly buffers are ps_alloc'd PSRAM (gV4Reasm, :135). NOT a finding.
- Per-event path: systemEventPost (System_Events.cpp:183) uses a fixed-size struct + strncpy into a static ring, no heap. The events.log sink (:443) snprintfs into a stack buffer. NOT a finding.
- Sensor logging per-sample: buildCSVFromSnap (System_SensorLogging.cpp:196) uses a static ps_alloc'd PSRAM buffer + snprintf. Textbook. NOT a finding.
- c_str()-on-temporary (e.g. `formatMacAddress(mac).c_str()`, a guaranteed-allocating 17-char String): ~19 sites, but ALL the per-packet ones are inside DEBUGF, which expands to DEBUGF_QUEUE_DEBUG -> `if (isDebugFlagSet(flag))` (System_Debug.h:595-600, :736) and therefore SHORT-CIRCUITS argument evaluation when the flag is off. Not paid in steady state. CAVEAT for other lanes: BROADCAST_PRINTF (System_Debug.h:889) gates only on `gOutputFlags & (SERIAL|WEB|FILE|BLE)`, which is effectively always true — String temporaries in BROADCAST_PRINTF args ARE always paid. I checked its ~19 call sites with String temporaries and all are on error/rare/transition-edge paths (peer offline, send rejected), so none qualify as steady-state. Worth a targeted re-check if BROADCAST_PRINTF ever lands in a loop.

COVERAGE / LIMITS: Frequency for every finding is traced to a concrete driver, not inferred. The automation chain is HardwareOne.cpp:2212-2217 -> schedulerTickMinute (System_Automation.cpp:3630) -> readText x2. I did NOT resolve the real-world size of automations.json (no cap found in System_Automation.cpp), so realloc counts are given as fileSize/16 with a 2KB worked example — the ratio holds regardless of size, and the cost scales linearly with it. I did not exhaustively trace all 33 readText call sites; I confirmed the ones in System_ESPNow.cpp (:576 loadMeshPeers, :7235 loadEspNowDevices) are init-only and the System_BondedPeer/cmd_* ones are per-command, so they are correctly excluded as noise. Overall assessment: this firmware is already heavily and deliberately optimized (PSRAM pools, static scratch buffers, ring buffers, arg-gated debug macros) — the finding count is low because the codebase is genuinely good, not because coverage was thin. The automation tick is the one place where the discipline visibly lapses, and it lapses hard.
