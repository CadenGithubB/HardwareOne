# Task inventory and consolidation assessment

Date: 2026-10-30. Source of the live figures: `taskstats json` on the P4X-EYE bench device (boot #29, glasses + ring + phone connected, idle). Stack figures are bytes. Creation sites are in `components/hardwareone/` unless noted.

Note (2026-11-08): the figures below are from before three stacks were raised. `cmd_exec` is now 10240, `log_out` 6144 and `g2_owner` 8192. The free figures here are the old ones.

## 1. The 33 live tasks

### Ours (14 resident)

| task | prio | core | stack (free/size) | what it is | wakes on |
|---|---|---|---|---|---|
| `main` | 1 | 0 | 10660 / IDF | the Arduino loop: display, input, command intake, heartbeats | continuous, ~60 laps/s |
| `cmd_exec` | 1 | 0 | 1484 / 8192 | runs every command (serial, web-deferred, BLE, glasses, voice) and sends the reply (HardwareOne.cpp:1954) | command queue |
| `log_out` | 1 | 0 | 1100 / 4096 | drains the 256 B log line queue to the sinks (System_Debug.cpp:718) | log queue |
| `input_encoder` | 1 | any | 3108 / 3584 | input poller: samples click + board buttons every 5 ms, wheel edges by interrupt (Input_GPIOEncoder.cpp:157) | 5 ms timer |
| `g2_owner` | 5 | 1 | 1408 / 6144 | **the G2 owner**: heartbeats, control-plane writes, link lifecycle (G2_Glasses.cpp:18023, `heartbeatWorkerTask`) | timers + control queue |
| `g2_fsm` | 5 | 1 | 3724 / 4096 | hijack page state machine; lifecycle transitions (G2_HijackFsm.cpp:329) | FSM event queue |
| `g2_pages` | 2 | 1 | 6032 / 8192 | renders and swaps lens pages; the only page-navigation worker (G2_Glasses.cpp:24146) | page-swap queue |
| `g2_taps` | 2 | 1 | 6252 / 10240 | runs tap/gesture handlers deferred off the BT host task (G2_Glasses.cpp:24738) | tap queue |
| `g2_session` | 2 | 1 | 9876 / 10240 | long glasses sessions (EvenAI turns, session jobs) (G2_Glasses.cpp:26663) | session job queue |
| `g2_notes` | 1 | 0 | 3268 / 8192 | native-notes recording worker; exists only while notes are active (G2_NativeNotes.cpp:356) | request queue |
| `r1_owner` | 4 | 0 | 3028 / 6144 | the ring owner: transactions, vitals polls (G2_Ring.cpp:5367) | wake semaphore |
| `phone_pipe` | 1 | any | 4152 / 8192 | phone relay pipe runtime; self-retires when idle (BLE_PhonePipe.cpp:926) | 10 ms tick |
| `ble_setup` | 5 | 1 | 1740 / 5120 | the one serialized BLE connect/setup worker for glasses, ring and phone (BLE_SetupWorker.cpp:120) | wake semaphore |
| `c6_console` | 3 | any | **740 / 3072** | mirrors the C6's UART console (HAL_Radio.cpp:294) | UART bytes |

Transient (seen in creation sites, not resident at idle): `g2_net_scan`, `g2_wifi_pending`, `g2_ring_pending`, `g2_ai_test`, `g2_img_probe`, `boot_net`, `mapRender`, `cam_power`, `espnow_tx`, `mic_viz`, `stt`, `stt_capture`, `stt_fe`, `stt_warm`, `sr_*`, `llm_gen`, `llm_http`, `sensor_queue`, `i2c0_begin`, the I2C sensor pollers.

### Not ours (19)

| group | tasks | owner |
|---|---|---|
| Bluedroid host | `BTC_TASK` (19), `BTU_TASK` (20), `hciT` (22) | ESP-IDF |
| ESP-Hosted transport to the C6 | `sdio_read`, `sdio_write`, `sdio_rx_buf`, `sdio_process_rx`, `rpc_rx`, `rpc_tx` (all 23) | esp_hosted |
| ESP-NOW over hosted | `now_callbacks` (5) | components/esp_now_hosted |
| esp-dl model runners | `dl_mc0`, `dl_mc1` (2, one per core) | esp-dl (speech model) |
| kernel | `IDLE0`, `IDLE1`, `ipc0`, `ipc1`, `esp_timer`, `Tmr Svc`, `sys_evt` | ESP-IDF |

So 19 of the 33 rows are not ours, and five of those exist because ESP-Hosted runs one task per SDIO direction and stage. Nothing there is ours to merge.

## 2. Naming

Applied 2026-10-30, `<family>_<role>` with no `_task` suffix (FreeRTOS cap is 16 characters):

| old | new |
|---|---|
| `cmd_exec_task` | `cmd_exec` |
| `encoder_task` / `ano_task` / `gamepad_task` | `input_encoder` / `input_ano` / `input_gamepad` |
| `sensor_queue_task` | `sensor_queue` |
| `thermal_task`, `tof_task`, `apds_task`, `gps_task`, `imu_task`, `presence_task`, `rtc_task`, `fmradio_task` | `sensor_thermal`, `sensor_tof`, `sensor_apds`, `sensor_gps`, `sensor_imu`, `sensor_presence`, `sensor_rtc`, `sensor_fmradio` |
| `g2-fsm`, `g2_ctrl_owner`, `g2_page_swap_w`, `g2_tap_disp`, `g2_session_w` | `g2_fsm`, `g2_owner`, `g2_pages`, `g2_taps`, `g2_session` |
| `c6console`, `debug_out`, `mapRender`, `cam_pwr`, `sr_task` | `c6_console`, `log_out`, `map_render`, `cam_power`, `sr_main` |

Unchanged: `g2_notes`, `r1_owner`, `ble_setup`, `phone_pipe`, `boot_net`, `espnow_tx`, `mic_viz`, `stt*`, `sr_feed`, `sr_snip_wr`, `llm_gen`, `llm_http`, `i2c0_begin`, the transient `g2_*` workers. The §1 table below reads with the new names. `c6_console` stack raised 3072 → 4096 the same day.

Before the sweep, three styles coexisted:

Three styles coexist: `snake_case` (most), a hyphen (`g2_fsm`), camelCase (`mapRender`), and a `_task` suffix on some (`cmd_exec`, `input_encoder`, `sensor_queue`, `sensor_thermal`) but not others (`phone_pipe`, `r1_owner`). The FreeRTOS name cap is 16 characters, which is why `g2_pages` and `g2_session` are abbreviated. A consistent scheme that fits: `<family>_<role>` with no suffix, e.g. `g2_owner`, `g2_fsm`, `g2_pages`, `g2_taps`, `g2_session`, `g2_notes`, `r1_owner`, `ble_setup`, `phone_pipe`, `cmd_exec`, `log_out`, `input_poll`, `c6_console`, `map_render`. Names appear in `System_TaskUtils` / `System_MemoryMonitor` / `System_Utils` known-task tables and in the showcase trail, so a rename is a mechanical sweep with host tests to re-pin.

## 3. Why the G2 family is six tasks

The split is by **blocking behaviour and priority**, not by feature:

- `g2_owner` (prio 5) must keep heartbeats and control writes flowing no matter what the UI is doing; if a render blocked it, the glasses would time the link out.
- `g2_fsm` (prio 5) drains lifecycle transitions promptly and preempts the UI workers.
- `g2_taps` (prio 2) exists because taps arrive on the Bluedroid `BTC_TASK`, whose stack cannot carry a handler (the "BTC_TASK stack footgun"); handlers are deferred here.
- `g2_pages` (prio 2) serializes renders; a redraw chain measured 3.7 KB deep.
- `g2_session` (prio 2) carries long jobs (an EvenAI turn can run for tens of seconds). It is separate from taps and pages precisely so a long session job never delays a tap.
- `g2_notes` (prio 1, core 0) is a recording worker that only exists while a note is being taken.

Resident G2 stack total: 6144 + 4096 + 8192 + 10240 + 10240 + 8192 = **47 KB**, of which about 30 KB is unused headroom at idle.

## 4. What could be combined

| candidate | saves | cost / risk | verdict |
|---|---|---|---|
| **`g2_taps` + `g2_pages` → one `g2_ui` worker** with a single job queue | 1 task, ~8 KB stack (one 10 KB stack instead of 10 + 8) | Both are prio 2, core 1, short jobs, and both end in BLE writes to the same link, so they already serialize on the TX path. Needs a unified job type and an ordering rule (a tap that lands mid-redraw should run after the redraw completes, which is today's behaviour too). The three init-state machines and `gUiWorkerInitters` bookkeeping collapse to one. | **Worth doing**, medium effort |
| `g2_session` into `g2_ui` | 1 task, ~10 KB | A session job can run for tens of seconds; on a shared queue it would delay taps and renders, which is the exact regression the split prevents. Could be made safe only with a two-lane queue (short lane drained first), which is a new scheduler inside a worker. | **No**, keep separate |
| `g2_fsm` into `g2_owner` | 1 task, ~2 KB net (owner needs +4 KB) | FSM transitions wait on control writes the owner itself makes; folding them in creates re-entrancy on the owner loop. Owner is at 1.4 KB free today. | **No** |
| `g2_notes` into anything | 0 at idle (it is transient) | It already leaves when idle. | nothing to do |
| `r1_owner` + `g2_owner` → one "wearables owner" | 1 task, ~6 KB | Different links, different cadences, different priorities (4 vs 5), and the ring's vitals transactions block for hundreds of ms; sharing a loop couples ring stalls to glasses heartbeats. | **No** |
| `phone_pipe` + `ble_setup` | 0 | Already designed as one setup worker plus a self-retiring pipe runtime. | nothing to do |
| `c6_console` → UART event queue, or absorb into `log_out` | 1 task, 3 KB | It is a blocking UART reader; an event-driven version is small. `log_out` is on core 0 at prio 1 and already drains a queue, so adding a UART source is natural. | **Worth doing**, small |
| `input_encoder` event-driven buttons | 0 tasks, ~1% CPU at idle | GPIO interrupts on the button lines, 5 ms sampling only while a press is in flight. | nice-to-have |

Realistic total: **two tasks and ~11 KB** of resident stack, for one medium change (the glasses UI merge) and one small one (the C6 console). Everything else in the G2 family is split for a reason that still holds.

## 5. Things to fix regardless of merging

- `c6_console` has **740 B free of 3072** (24%): the tightest stack in the system and ours. Raise to 4096 now; it costs 1 KB.
- `ipc0` at 980 B and `BTU_TASK` at 1444 B are IDF-owned; leave them but keep watching.
- The naming sweep in §2 is cheap and makes the trail and the stack reports read as one system.

## 6. Order of work, if pursued

1. `c6_console` stack to 4096 (one constant).
2. Naming sweep with the known-task tables and host tests re-pinned.
3. `g2_ui` worker: merge tap dispatch and page swap under one queue; keep `g2_session`.
4. `c6_console` as a UART-event source drained by `log_out`.
5. Event-driven input buttons.
