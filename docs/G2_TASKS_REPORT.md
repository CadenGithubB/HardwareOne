# G2-related FreeRTOS tasks — inventory and assessment

**Scope:** Tasks created via `xTaskCreate` / `xTaskCreateStatic` that implement G2 glasses transport, hijack UI, ring bridge, on-glasses Network helpers, or Test Suite workers that talk to the lens.  
**Sources:** `G2_Glasses.cpp`, `G2_HijackFsm.cpp`, `G2_Ring.cpp`, `G2_Page_Network.cpp`, `G2_Page_TestSuite.cpp` (grep `xTaskCreate`, May 2026).  
**Note:** ESP-IDF and Arduino BLE allocate additional tasks (e.g. BTC, `nimble_host`) that are *not* listed here; they still compete for internal DRAM with the stacks below.

---

## Executive summary

The G2 surface has **grown many distinct task entry points** over time: long-lived workers (page swap, tap dispatch, FSM), medium-lived BLE work (`g2_connect` / `g2_reconnect`), short-lived hijack and notification workers, media pipelines (BMP/camera), and adjunct Network/Test tasks. That is a **real “balloon” in the number of named patterns**, but there was also a **deliberate consolidation**: page swaps moved from **per-navigation `xTaskCreate`** to a **single `g2_page_swap_w` queue worker**, and hijack taps moved to **`g2_tap_disp`** to avoid BLE spinlock/deadlock issues.

The largest **single** internal stack commitment tied to G2 init is **`g2_tap_disp` at 20 KB**, sized so `handleHijackMenuTap` can call `setSetting` → `writeSettingsJson` and similar deep stacks off the BLE notify task. Together with **`g2_page_swap_w` (4 KB)** and **`g2-fsm` (3 KB)**, baseline G2 client infrastructure is on the order of **~27–30 KB internal stack** before heartbeat (2–3 KB variable) and any connect/ring/media tasks.

---

## 1. Long-lived tasks (created at `initG2Client` / first use, not torn down on `deinitG2Client`)

| Task name      | Stack   | Priority | Role |
|----------------|---------|----------|------|
| `g2_page_swap_w` | 4096 B | `tskIDLE_PRIORITY + 2` | Drains `gPageSwapQueue`; runs all SHUTDOWN/CREATE/REBUILD page swaps. Replaced per-tap task spawns. |
| `g2_tap_disp`    | **20480 B** | `tskIDLE_PRIORITY + 2` | Drains hijack tap queue; runs `handleHijackMenuTap` off BLE callback stack. |
| `g2-fsm`         | 3072 B | `5` | `G2_HijackFsm.cpp` — hijack lifecycle FSM worker; queue-backed. |

`deinitG2Client()` stops temples and heartbeat but **does not delete** these three workers (by design in current code comments: cheap when idle).

**Heartbeat** (`g2_hb_worker`): created when the heartbeat timer starts, **not** at `initG2Client` alone. Stack is **dynamic 2048–3072 B** based on `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`; priority **5**. Skipped entirely if internal free heap is below a computed threshold (~stack + ~1200 B TCB/overhead).

---

## 2. BLE / session tasks (G2 client + ring)

| Task name        | Stack   | Priority | Mutex / overlap |
|------------------|---------|----------|-----------------|
| `g2_connect`     | 6144 B  | `5` | `gConnectTaskActive` — only one connect-style task at a time. |
| `g2_reconnect`   | 6144 B  | `5` | Same handle slot as `g2_connect` (`gConnectTaskHandle`). |
| `ring_connect`   | 5120 B  | `5` | `gRingConnectTaskActive` — one ring connect task. |
| `ring_reconnect` | 5120 B  | `5` | Same. |
| `ring_connect_mac` | 5120 B | `5` | Same. |
| `ring_spoof`     | 4096 B  | `4` | Optional telemetry spoof loop. |
| `ring_bridge_hb` | 3072 B  | `3` | Optional bridge heartbeat. |

Ring tasks live in `G2_Ring.cpp` and are part of the same product story as G2 (shared central stack). They are **not** prefixed `g2_` but should be counted when reasoning about **worst-case concurrent stacks** with glasses + ring + WiFi.

---

## 3. Short-lived / on-demand tasks (`G2_Glasses.cpp`)

| Task name          | Stack   | Priority | When spawned |
|--------------------|---------|----------|----------------|
| `g2_hijack`        | 4096 B  | `tskIDLE_PRIORITY + 2` | Hijack MenuStartup worker; avoids blocking BLE notify task on CREATE ack. |
| `g2_live_page`     | 4096 B  | `5` | Live list refresh (`g2StartLiveListPage`). |
| `g2_live_text`     | 4096 B  | `5` | Live text refresh (`g2StartLiveTextPage`). |
| `g2_notify_clear`  | 3072 B  | `tskIDLE_PRIORITY + 1` | Auto-dismiss lens notification after delay. |
| `g2_bmp_view`      | 6144 B  | `tskIDLE_PRIORITY + 2` | Single-tile BMP push from VFS. |
| `g2_cam_view`      | 6144 B  | `tskIDLE_PRIORITY + 2` | One-shot camera → BMP pipeline (`ENABLE_CAMERA_SENSOR`). |
| `g2_cam_stream`    | 6144 B  | `tskIDLE_PRIORITY + 2` | Streaming variant of camera path. |
| `g2_bmp_full`      | **8192 B** | `tskIDLE_PRIORITY + 2` | Multi-tile BMP viewer; larger stack per comments. |

These generally **self-delete** after work. Overlap risk: user could theoretically have **live page + live text** design-wise; in practice hijack pages usually serialize. **Notification clear** can overlap with anything briefly.

---

## 4. Network page (`G2_Page_Network.cpp`) — `g2_*` workers

| Task name         | Stack | Priority | When spawned |
|-------------------|-------|----------|--------------|
| `g2_net_scan`     | 4096 B | `5` | WiFi scan from glasses (one-at-a-time mutex in file). |
| `g2_wifi_pending` | 4096 B | `5` | WiFi “Pending…” watchdog after connect-from-glasses. |

These exist so **WiFi.scan / polling** does not run on the BLE notify task. They are G2-adjacent (hijack Network submenu) rather than core protocol.

---

## 5. Test Suite (`G2_Page_TestSuite.cpp`)

| Task name       | Stack | Priority | Notes |
|-----------------|-------|----------|--------|
| `g2_ai_test`    | 4096 B | `5` | AI pipeline worker (multi-step BLE sends). |
| `g2_img_probe`  | **8192 B** (or PSRAM static stack + internal TCB fallback) | `5` | Image probe; internal `xTaskCreate` first, then `xTaskCreateStatic` with SPIRAM stack if internal heap is tight. |

---

## 6. Ballooning assessment

### Evidence the task count *did* grow

- **Many orthogonal features** each got a **dedicated worker**: hijack bootstrap, connect, reconnect, live list, live text, notification timer, BMP/camera/stream/full BMP, FSM, page swap, tap dispatch, ring connect variants, ring spoof/HB, network scan, WiFi pending, AI test, image probe.
- **Stack sizes are conservative** (often 4–8 KB, camera/BMP 6–8 KB, tap dispatcher **20 KB**), which is appropriate for embedded BLE + JSON + protobuf paths but **adds up** if several overlap.

### Evidence of intentional *de*-ballooning

- **Page swap:** Comments in `G2_Glasses.cpp` document moving from **per UI action** `xTaskCreate` to **one** `g2_page_swap_w` + queue — directly addressing internal heap fragmentation and `xTaskCreate` failures after camera viewer.
- **Tap dispatch:** **One** `g2_tap_disp` replaces running `handleHijackMenuTap` on the BLE stack; avoids spinlock asserts and stack overflow on BTC.
- **Image probe:** Fallback to **PSRAM-backed static stack** when internal `xTaskCreate` fails — acknowledges dual-client + WiFi internal DRAM pressure.

### Largest concern for “feels ballooned”

1. **`g2_tap_disp` at 20 KB** is the dominant fixed cost; it is **justified in-code** by `writeSettingsJson` / sensor paths, but any future work that grows hijack handlers **further** will hit this first.
2. **Concurrent overlap** of `g2_connect` (6144) + **`ring_connect` (5120)** + persistent workers + **optional** `g2_hijack` (4096) is a realistic worst case during “connect everything” flows — aligns with field logs of **BLE_INIT malloc failed** / **wifi:m f null** when internal DRAM is stressed.

---

## 7. Suggested follow-ups (analysis only)

| Direction | Rationale |
|-----------|-----------|
| **Task list in one header** | Single table (name, stack, owner TU) updated when adding workers — avoids drift from this doc. |
| **Cap concurrent ephemeral spawns** | e.g. global “heavy worker” semaphore for BMP/camera/img_probe if overlap becomes possible from UI. |
| **Re-audit `g2_tap_disp` stack** | If JSON/settings paths shrink or move to PSRAM buffers, **20 KB** might be reducible — high leverage for internal DRAM. |
| **Ring vs G2 connect serialization** | Already partially gated; document expected ordering for operators. |

---

## 8. Quick reference — stack bytes (internal unless noted)

```
Persistent:   page_swap 4096 + tap_disp 20480 + fsm 3072  ≈ 27.5 KB (+ hb 2–3 KB when running)
Connect:      g2_connect / g2_reconnect 6144 (one active)
Ring:         ring_* 5120 (one active) + optional spoof 4096 + bridge_hb 3072
Short-lived:  hijack 4096; live_* 4096×2; notify_clear 3072; bmp_view/cam_* 6144×3; bmp_full 8192
Network:      net_scan 4096; wifi_pending 4096
Tests:        ai_test 4096; img_probe 8192 (PSRAM stack possible)
```

---

*End of report.*
