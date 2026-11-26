# ESP32-P4 patches

## Staged by `prepare_sdk.sh`

`tools/p4/prepare_sdk.sh` copies these components into `.sdk-overrides/esp32p4/` and patches the copy;
the exported SDK and the component cache are only read. Each patch is pinned by SHA-256, so a changed
byte is refused.

| Patch | Applied to | By |
|---|---|---|
| `idf-tinycrypt-9fd7cb7.patch`, `idf-bt-gap-deinit.patch`, `idf-bt-failed-conn-handle.patch`, `idf-bt-unknown-handle-acl.patch` | ESP-IDF 5.5.5 `bt` component; pins in `idf-bt-manifest.json` | `prepare_idf_bt.py` |
| `idf-jpeg.patch` | ESP-IDF 5.5.5 `esp_driver_jpeg` (optional) | `prepare_idf_jpeg.py` |
| `esp-dl-3.3.12-hw1.patch` | `espressif/esp-dl` 3.3.12 | `prepare_esp_dl.py` |
| `esp-hosted-2.12.13-hw1.patch` | `espressif/esp_hosted` 2.12.13 ([notes](esp-hosted-2.12.13-hw1.md)) | `prepare_esp_hosted.py` |

## Applied to the ESP-IDF install by hand

Patches applied to the ESP-IDF install this project builds with (`~/esp/esp-idf`, v5.5.5). Re-apply after a fresh
install; drop each one when ESP-IDF is upgraded to a version that contains it.

| Patch | Upstream | Contained in | Why |
|---|---|---|---|
| `0001-freertos-fix-xesppie-registers-save-restore.patch` | [b25cb29](https://github.com/espressif/esp-idf/commit/b25cb2906c) | v6.0+ (not in any v5.5.x or v5.4.x) | On a context switch FreeRTOS did not save/restore PIE register q3 and mis-packed XACC. Any two tasks using PIE on the same core corrupt each other's vector state: the w8a16 speaker-embedding kernels keep per-channel shifts in q3, so a background speaker task interleaved with the STT worker produced wrong voiceprints. |

Apply (from the IDF root):

```bash
git -C ~/esp/esp-idf apply "$PWD"/tools/p4/patches/0001-freertos-fix-xesppie-registers-save-restore.patch
```

Check: `tools/p4/prepare_sdk.sh --check` (or `python3 -B tools/p4/check_idf_pie.py --idf ~/esp/esp-idf`) prints
`present`, `MISSING` or `PARTIAL`. It recognises the instructions the fix adds to `portasm.S` (q3 save and restore,
XACC stored without advancing the frame), so an upstream 6.0+ tree passes too.
