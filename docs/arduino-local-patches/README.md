# Local patches to the vendored arduino component

`components/arduino/` is excluded by the main repo's `.gitignore` and is its own
nested git checkout (arduino-esp32 core 3.3.5, nested HEAD `11bc7ac1a`). Every
local fix to it exists **only as uncommitted working-tree changes** in that
nested repo. A `git clean -dfx`, a fresh clone, or a re-vendor of the component
silently reverts all of them — while the firmware code that depends on them
survives and keeps compiling. The failure mode is invisible until hardware
misbehaves.

## What the patches are

All marked with `[hardwareone local patch YYYY-MM-DD]` comments:

| File | Markers | What |
|---|---|---|
| `libraries/BLE/src/BLECharacteristic.cpp` / `.h` | 2 / 1 | Per-characteristic prepared-write opt-out. HardwareOne disables ATT long writes on the executable command characteristic so the library's shared, allocation-fallible accumulator cannot commit a truncated prefix, replay an old value, or mix connections. Ordinary bounded writes remain unchanged. |
| `libraries/BLE/src/BLEClient.cpp` | 13 | Raise-only local-MTU guard in `setMTU` (the per-link MTU fix — without it, ring-first glasses discovery fails with `pkt size: 102, PDU size: 64`); `m_conn_id`/`m_mtu` reset on disconnect and failed open; REG_EVT app_id filter (stops a parked client capturing another client's registration); bounded REG/OPEN/SEARCH waits; semaphore-leak fixes; stale-rc-on-timeout fix; structured connect-stage telemetry; acknowledged GATTC unregister that retains routing until completion; **`~BLEClient()` clears the static `BLEDevice::m_pClient` alias if it points at this client — stops the double-free that rebooted the device on the `openble`↔G2-client role flip (multi-client alias never nulled by the library)** |
| `libraries/BLE/src/BLEDevice.cpp` | 2 | Sync `m_localMTU` cache to Bluedroid's real boot value (517) at init; checked, phase-ordered host/controller teardown that reports incomplete retirement and quarantines an unsafe client instead of deleting through callbacks |
| `libraries/BLE/src/BLERemoteCharacteristic.cpp` | 7 | Semaphore-leak + bounded-wait + failure-delivery fixes, including a bounded write overload and a 30-second bound for the legacy write API; **`retrieveDescriptors()` stack-smash fix — upstream passed `count = 10` as the in/out capacity for a SINGLE-element stack `esp_gattc_descr_elem_t result`, letting `esp_ble_gattc_get_all_descr()` write ~180 B past it on any characteristic with more than one descriptor. The loop consumes one element per call and does `offset++`, so the correct capacity is 1. Reached on EVERY G2/R1 connect via the constructor.** |
| `libraries/BLE/src/BLERemoteDescriptor.cpp` | 4 | Same family |
| `libraries/WiFi/src/STA.cpp` | 1 | Treat 802.11 `MISSING_ACKS` disconnects as transient so the existing Arduino auto-reconnect owner retries them; protocol-reason based, with no board/RSSI/antenna special case. |
| `cores/esp32/esp32-hal-i2c-ng.c` | 1 | I2C driver bookkeeping array `bus[]` (1,064 B) moved to PSRAM via `EXT_RAM_BSS_ATTR` — all mutating access is mutex-guarded task context, no DMA retention; verified per-symbol 2026-08-19 (docs/DRAM_LONGTAIL_VERDICTS_2026-08-19.md) |
| `cores/esp32/esp32-hal-periman.c` | 2 | Peripheral-manager `pins[]` + `deinit_functions[]` (856 B) moved to PSRAM — task-context attach/detach only; this firmware registers no GPIO ISRs (latent caveat documented in the marker for any future ISR user) |
| `libraries/Network/src/NetworkEvents.cpp` | 0 (unmarked) | arduino_events task stack fix (see wifirm/arduino_events memory) |
| `CMakeLists.txt`, `Kconfig.projbuild`, `idf_component.yml` | — | Build-integration tweaks |
| ESP32-P4 / ESP-Hosted (2026-09-29, unmarked) | - | Required by `HW_BOARD=p4x_eye`, harmless elsewhere. `esp32-hal-bt.c/.h`: Bluedroid over the Hosted C6 controller (VHCI), with acknowledged controller lifecycle state (`btHostedControllerStatus`). `esp32-hal-hosted.c/.h`: `hostedSetPins`, BLE claim/release. BLE library: every `SOC_BLE_SUPPORTED` gate also accepts Hosted Bluedroid/NimBLE; owned CCCD descriptors (`addDescriptor(d, true)`) and GATT server trees are reclaimed only after checked terminal host teardown. `esp32-hal-uart.c`: P4 LP-UART RC_FAST clock. `idf_component.yml`: pins `esp_hosted` 2.12.13, `esp_wifi_remote` 1.3.1, `mdns` 1.10.1, `littlefs` 1.20.4 and drops Arduino's own `esp-sr` rule (HardwareOne pins ESP-SR itself). Qualified on hardware in `experiments/p4_connectivity`, `p4_ble_roles` and `p4_mesh`. |
| URL-pull OTA removed (2026-08-30, unmarked) | - | Deletes `libraries/ESP_HostedOTA/`, `libraries/HTTPUpdate/` and `libraries/Update/src/HttpsOTAUpdate.cpp/.h`, drops them from `CMakeLists.txt`, and removes `hostedGetUpdateURL()` and its `Update URL:` log line from `cores/esp32/esp32-hal-hosted.c/.h`. This is a policy decision, not a size saving: no firmware path may download an image from a URL compiled into the binary. `ESP_HostedOTA` fetched co-processor firmware from `https://espressif.github.io/arduino-esp32/hosted/<target>-vX.Y.Z.bin`. Nothing called any of it and the linker already dropped it, so the image does not change. HardwareOne's own update paths (staged BLE OTA, the recovery updater, `c6update`) never used it. `ArduinoOTA` (push, not URL-pull) and `HTTPUpdateServer` (not in the build list) were deliberately left. Deletions carry no markers; `verify_patches.sh` checks they stay absent. Recorded in `arduino-local-patches-2026-08-30-url-ota-removal.patch`. |
| Companion transport lost (2026-10-02) | 8 | `cores/esp32/esp32-hal-hosted.c/.h`: `hostedSetTransportLost`/`hostedTransportLost`; a successful `hostedInit` clears the flag and `hostedDeinitBLE` skips the controller RPCs while it is set. `cores/esp32/esp32-hal-bt.c`: controller disable/deinit report the controller as lost (`ESP_ERR_INVALID_STATE`) instead of pretending it acknowledged, so the BLE lifecycle records an incomplete retirement and refuses to reopen BLE until a reboot. `libraries/WiFi/src/WiFiGeneric.cpp`: `wifiLowLevelDeinit` and `espWiFiStop` forgive remote failures while lost. Lets `System_RadioCompanion` tear Wi-Fi down on a dead C6 without waiting out RPC timeouts (a live C6 is always stopped cleanly); recorded in `arduino-local-patches-2026-10-02-companion.patch`. |
| Panic path without flash (2026-10-15) | 2 | `cores/esp32/esp32-hal-misc.c`: with `CONFIG_ESP_PANIC_HANDLER_IRAM` (set for every ESP32-P4 in `config/sdkconfig.defaults.esp32p4`), `__wrap_esp_panic_handler` and `handle_custom_backtrace` are placed in IRAM, and the custom panic handler runs only on the first entry. A fault while flash is unreadable (mid erase/program, or a cache error) previously re-faulted on every fetch of the flash-resident wrap, so the IDF handler never printed or restarted and only the HP watchdog recovered the P4 about ten minutes later. Recorded in `arduino-local-patches-2026-10-15-panic-iram.patch`. |

| Bluetooth library heap (2026-10-22, memory audit tranche C) | 5 + 1 | `libraries/BLE/src/RTOS.h`, `FreeRTOS.cpp`: `FreeRTOS::Semaphore` is 8 bytes (atomic handle + value), down from 48; the debug name/owner Strings and the dead pthread branch are gone, the API takes `const char*` literals, and the kernel object is made on the first take/wait (a give before any take is a no-op, exactly a give on the constructor's available binary semaphore). `BLERemoteService.h`, `BLEAdvertising.h`: the never-waited `m_semaphoreGetCharEvt` and `m_semaphoreSetAdv` removed. `BLEScan.cpp`: `take("start")` literal. `BLEDevice.cpp` (the phone pipe's seam, so it lives in `tools/ble-phone-pipe/arduino.patch`): the 4 KB app-id bitmap is a 64-entry table. Recorded in `arduino-local-patches-2026-10-22-ble-memory.patch`; applies after the three dated patches below. |
| Service discovery never waits forever (2026-10-25) | 1 | `libraries/BLE/src/BLEClient.cpp` `getServices()`: returns at once for a disconnected client, and claims the search-complete semaphore without waiting, before the request, instead of an unbounded `take()` after it. A search that timed out leaves that semaphore taken, and a link that is already gone never gives it back, so the next discovery on that client (here the R1 ring's debug listing after "ring service not found") blocked forever and held the shared BLE-connect worker until reboot. Taking it first also stops a fast `SEARCH_CMPL_EVT` being consumed by the take. Host suite `ble_service_search` runs the shipped function. Recorded in `arduino-local-patches-2026-10-25-ble-search-wait.patch`; applies after the 2026-10-22 patch. |

`arduino-local-patches.patch` is the full `git -C components/arduino diff`
snapshot of all of the above.

**Not covered:** the untracked `components/arduino/variants/XIAO_ESP32S3_SENSE/`
directory (board variant) — `git diff` cannot capture untracked files.

## Verify (run any time, especially after anything touched components/arduino)

`tools/build_board.sh` runs this check before every build and refuses to build
when a patch is missing, so a dropped patch fails at the build step instead of
on hardware.

```bash
bash docs/arduino-local-patches/verify_patches.sh
```

## Re-apply after a revert

```bash
git -C components/arduino apply --check ../../docs/arduino-local-patches/arduino-local-patches.patch
git -C components/arduino apply ../../docs/arduino-local-patches/arduino-local-patches.patch
git -C components/arduino apply ../../docs/arduino-local-patches/arduino-local-patches-2026-08-30-url-ota-removal.patch
git -C components/arduino apply ../../docs/arduino-local-patches/arduino-local-patches-2026-10-02-companion.patch
git -C components/arduino apply ../../docs/arduino-local-patches/arduino-local-patches-2026-10-15-panic-iram.patch
git -C components/arduino apply ../../docs/arduino-local-patches/arduino-local-patches-2026-10-22-ble-memory.patch
git -C components/arduino apply ../../docs/arduino-local-patches/arduino-local-patches-2026-10-25-ble-search-wait.patch
git -C components/arduino apply ../../tools/ble-phone-pipe/arduino.patch
```

Patches after the snapshot are separate files applied on top of it, in date
order; `arduino-local-patches-2026-10-02-companion.patch` was produced by
diffing the upstream 3.3.5 files plus the snapshot's hunks against the
working copy (a checkout without the nested git repository cannot use `git
diff` for this). `arduino-local-patches-2026-08-30-url-ota-removal.patch` was
recorded on 2026-10-15, after a worktree build showed the snapshot did not
carry it. It is a `git diff` against a clean clone with only the snapshot
applied, and it also applies after the companion patch. Clean clone plus all
three patches reproduces the main checkout's tree on 2026-10-15. The only
exceptions were the untracked variant below and in-progress edits to
`BLEDevice` and `BLESecurity` from another branch.

(The `../../` matters: `git -C` resolves relative paths against
`components/arduino`, not your shell's cwd.)

(`--check` first; if it fails, the component was re-vendored to a different
base — re-apply hunks by hand using the marker comments as the guide.)

## Refresh after adding a new patch

```bash
git -C components/arduino diff > docs/arduino-local-patches/arduino-local-patches.patch
```

and bump the marker counts in `verify_patches.sh`.

## Honest limitation

This `docs/` directory is itself untracked (project convention), so it also
does not survive `git clean -dfx`. For real durability, either commit the
patches inside the nested `components/arduino` repo, commit this directory in
the main repo, or keep a copy outside the tree. That decision is deliberately
left to the repo owner.
