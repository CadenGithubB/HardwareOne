# ESP-Hosted 2.12.13 patch: shared TX pool alignment and restart TX fence

`prepare_esp_hosted.py` stages the pinned registry package plus
`esp-hosted-2.12.13-hw1.patch` in
`.sdk-overrides/esp32p4/espressif__esp_hosted`. The existing `prepare_sdk.sh`
entry point prepares/verifies it alongside the other P4 SDK overrides.
The P4-only `override_path` in HardwareOne's component manifest redirects
Arduino's Hosted dependency as well. The shared SDK and managed package
remain unchanged; no C6 firmware change is needed.

The source is ESP-Hosted **2.12.13**, registry component hash
`f1792f36ef9a637adf2faa2662f3c4500b07f3d2ba65c25f3ed4824490ba59f9`,
upstream commit `dd0176e5fc959d79b306f6677a2f878123c58e9a` (also pinned by
`esp_now_hosted_companion.h`). All 665 package files matched the local registry
cache and the previously qualified P4 checkout byte-for-byte when this patch
was prepared, excluding the component manager's `.component_hash` marker.
The existing safe realloc, transport teardown and SPIRAM preference changes
are already in that package, not extra modifications introduced here.
The preparer records and verifies the complete source, patch and result hashes.
An existing override with unexpected changes is rejected, never overwritten.

## Fault and bounded correction

The shared Wi-Fi TX pool used 1600-byte blocks. Its allocation base was aligned
by the DMA allocator, but the pool advanced each block by 1600 bytes. With
P4's configured 128-byte external cache lines, alternate blocks were therefore
64 bytes off the required boundary. Wi-Fi passes these buffers zero-copy to
SDIO. ESP-IDF's `sdmmc_io_write_blocks` rejects their addresses with
`ESP_ERR_INVALID_ARG` (258) before attempting an SDIO transfer.

The patch asks ESP-IDF 5.5.5's **actual allocator adjustment hook**,
`esp_heap_adjust_alignment_to_hw`, for the rounded single-block size for
internal DMA memory and, when selected, DMA SPIRAM. The larger size becomes
the shared pool stride. This includes cache and encryption requirements without
hardcoding a P4 cache size. The existing allocator still provides the correctly
aligned base for whichever memory location succeeds, including internal RAM
fallback. An explicit `esp_mm` component requirement provides the pinned SDK's
private hook header.

For the current P4 configuration this changes 1600 to **1664 bytes**, costing
1600 extra bytes in the 25-block common TX pool. The separate 1536-byte SDIO
pool already divides evenly by the P4 cache alignment and remains unchanged.
The low-level pool's optional debug guard is not enabled in this package; if
enabled in a future SDK, its extra guard bytes must also be considered in the
stride. This patch does not claim to qualify that unsupported debug variant.

The observed boot failure also reproduced on the saved image from before the
G2 reader changes: Wi-Fi/NTP SDIO errors came before G2 client initialization.
Queued transport recovery subsequently interrupted BLE teardown, leaving the
existing safety latch at `STOP_CONTROLLER`. That latch is a secondary effect;
it remains intact and requires a full P4 reboot after the failed teardown.
The original log did not print the failing buffer pointer, so source/test
evidence and subsequent hardware qualification should be reported separately.

Turning off `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` is a valid temporary
diagnostic: 1600 divides evenly by the current internal 64-byte cache line.
It moves roughly 85.6 KiB of the two pools into internal RAM before other DMA
allocations, so the retained fix preserves the existing SPIRAM policy.

## Restart TX fence

Before an intentional P4 restart, HardwareOne latches the C6's EN pad low so
the companion stops sending while the SDMMC host drains
(`radioHalSilenceForRestart`, HAL_Radio.cpp). The radio users keep running
through that drain, and each packet they queued after EN went low became a
CMD53 the held C6 never answered: "sdmmc_send_cmd returned 0x107", "Failed to
send data", "Unrecoverable host sdio state" (bench 2026-11-08, 7 of 15
reboots). Before the hold, HardwareOne closes TX and lets the write task send
what was already queued while the C6 still answers.

The patch adds that gate to `transport_drv.c`. `esp_hosted_hw1_fence_tx()` sets
a one-way flag, and `esp_hosted_hw1_tx_open()` (the TX-ready state and no
fence) replaces `is_transport_tx_ready()` in the STA, AP and serial wrappers
and in the SDIO `esp_hosted_tx`. Every producer reaches the queue through
those: lwIP through the STA/AP wrappers, serial RPC, BLE VHCI and the
private-interface path through `esp_hosted_tx`. A refused packet is freed and
returns the error a packet on a down transport already gets. Only a CPU reset
clears the flag. The other transports' `esp_hosted_tx` (SPI, SPI-HD, UART)
are not changed; the P4 uses SDIO.

A refusal at the fence is not logged. Hosted's refusal logs (`tx fail ...
transport_up(0)` in `esp_hosted_tx`, `Tx failed` in the Bluedroid VHCI send,
`drop pkt` in the wrappers) run on the producer's own task. With G2 or R1
connected, Bluedroid's `hciT` task (a fixed 2.5 KB stack, about 1.95 KB usable
after the P4's coprocessor and TLS reservations) kept sending HCI packets after
the fence, and vfprintf for the `tx fail` line overflowed it: a stack
protection fault in 3 of 15 reboots (bench 2026-11-08, image f9d931bb).
`esp_hosted_hw1_tx_drop_if_fenced()` returns 1 once the fence is set and
counts the drop; each of those log sites now logs only when it returns 0, so a
refusal for any other reason (transport down, bad length, missing channel)
still logs as upstream. `hosted_hci_bluedroid_send` checks it before its
allocation and drops the packet on hciT without a heap call or a log, and its
`Tx failed` line is skipped when the fence closed during the call
(`esp_hosted_hw1_tx_fenced()`, one-way). HardwareOne prints the count,
`esp_hosted_hw1_tx_fence_drops()`, in its `SDMMC quiesced` line. The NimBLE
VHCI paths log nothing on a refused send and are unchanged (the P4 build uses
Bluedroid).

`transport_state` is deliberately not changed. Lowering it with Hosted's own
`set_transport_state(TRANSPORT_RX_ACTIVE)` would also refuse TX, but
`transport_drv_reconfigure()` treats any state below TX_ACTIVE as a slave to
bring up. A Wi-Fi init (`esp_wifi_remote_init`) or BLE open (`vhci_drv.c`)
during the flush or the drain would then pulse the C6's reset and run SDIO
card init on slot 1, the bus activity the restart quiesce keeps away. With the
fence the state stays TX_ACTIVE and those calls return "Transport is already
up", as before.

## Verification

After exporting ESP-IDF 5.5.5, run:

```sh
python3 -B tools/p4/test_esp_hosted_mempool.py --idf "$IDF_PATH" --sanitize
```

The test obtains a pristine package from `--source`, `managed_components`, or
the shared download cache, prepares one temporary override and removes it when
finished. It verifies source/result hashes, unchanged repeat preparation, and
refusal to overwrite a tampered destination.

The C harness compiles the actual common-pool constructor, Hosted allocation
functions, complete mempool implementation, IDF hardware allocation adjustment
hook, SDMMC alignment check, and SDMMC block-write function. Only heap, cache,
RTOS and actual I/O boundaries are replaced for the host. The test allocates
all 25 blocks, checks bounds and reuse, and exercises SDMMC rejection before
I/O. The original 128-byte SPIRAM case rejects 12 of 25 blocks; the patched
case accepts all 25. It covers 64/128/256-byte cache requirements, internal
fallback, SPIRAM preference on/off, uncached memory and an increased encryption
alignment. ASan/UBSan failures abort the test.

`tools/p4/test_esp_hosted_tx_fence.py` applies the patch to the pristine
package and compiles the fence, the STA/AP/serial wrappers, the SDIO
`esp_hosted_tx` and `hosted_hci_bluedroid_send` unchanged against fakes of the
OS abstraction, queue and log: with TX open every producer queues, a down
transport keeps Hosted's logs, and after the fence (also one that closes
inside a VHCI send) nothing logs, the VHCI send allocates nothing and every
buffer is freed. `--patch` runs it on another patch (f9d931bb's fails). It
exits 77 without a pristine package.

For device qualification, build the affected saved profile and its baseline,
check their resolved component paths reference the override, then capture a
full reboot with normal Wi-Fi/NTP and BLE auto-start enabled. Verify no 258
errors or C6 recovery and confirm glasses connection plus a full chat turn.
Keep the selected firmware and boot evidence; incremental build directories
remain owned by the active task. The override is one approximately 17 MiB
dependency copy needed for reproducible builds, not another SDK/build snapshot.
