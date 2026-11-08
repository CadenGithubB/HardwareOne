# Local ESP-IDF patches

Patches applied to the ESP-IDF install this project builds with (`~/esp/esp-idf`, v5.5.5). Re-apply after a fresh
install; drop each one when ESP-IDF is upgraded to a version that contains it.

| Patch | Upstream | Contained in | Why |
|---|---|---|---|
| `0001-freertos-fix-xesppie-registers-save-restore.patch` | [b25cb29](https://github.com/espressif/esp-idf/commit/b25cb2906c) | v6.0+ (not in any v5.5.x or v5.4.x) | On a context switch FreeRTOS did not save/restore PIE register q3 and mis-packed XACC. Any two tasks using PIE on the same core corrupt each other's vector state: the w8a16 speaker-embedding kernels keep per-channel shifts in q3, so a background speaker task interleaved with the STT worker produced wrong voiceprints. |

Apply (from the IDF root):

```bash
git -C ~/esp/esp-idf apply "$PWD"/tools/p4/idf-patches/0001-freertos-fix-xesppie-registers-save-restore.patch
```

Check: `tools/p4/prepare_sdk.sh --check` (or `python3 -B tools/p4/check_idf_pie.py --idf ~/esp/esp-idf`) prints
`present`, `MISSING` or `PARTIAL`. It recognises the instructions the fix adds to `portasm.S` (q3 save and restore,
XACC stored without advancing the frame), so an upstream 6.0+ tree passes too.
