# Follow-up: decouple bond/automation processing from the file-transfer layer

> Status: **proposed — not implemented.** Separate, deliberate cleanup. Do this
> *after* Phase A streaming is HW-validated, and ideally with **two bonded devices
> in hand** to re-test settings sync, because it moves working sync code.

## The wart (and what it is NOT)

`v4h_file_end` (the file-transfer FINALIZE handler) special-cases incoming files
**by filename** and processes them inline, out of the in-RAM buffer:

```c
if      (strcmp(filename, "_manifest_out.json") == 0) processBondModeManifestResp(...);
else if (strcmp(filename, "_settings_out.json") == 0) processBondSettings(...);
else if (strcmp(filename, "_schema_out.json")   == 0) processBondSchema(...);
else { /* normal write; automations.json also parsed here just to log a summary */ }
```

**Important correction to an earlier mis-description:** this does **not** make the
device "copy the peer's settings and apply them to itself." What each actually does:

| File | Handler | What it really does |
|---|---|---|
| `_settings_out.json` | `processBondSettings` | `cacheSettingsToLittleFS()` — **stores** the peer's settings to a flash cache + snapshots a CRC32 for heartbeat drift-detection. Never touches `gSettings`. |
| `_schema_out.json` | `processBondSchema` | caches the peer's schema for the bond system's reference. |
| `_manifest_out.json` | `processBondModeManifestResp` | updates the bond view of the **peer's** capabilities. |
| `automations.json` | (normal path) | saved to the inbox like any file, then parsed **only to print a human summary** to the log. Not applied. |

So nothing clobbers the device's own config — the model is "mirror the partner's
config for reference + drift detection." The real problem is purely **architectural
coupling**: the transfer layer knows bond/automation filenames and runs their logic
from RAM. That coupling is also what makes streaming awkward (those processors read
from the RAM buffer, which streaming mode doesn't have).

## The target shape — dumb pipe + consumer hook

1. The file-transfer layer **only** delivers bytes to a path on flash. No `strcmp`,
   no bond/automation knowledge.
2. On completion it fires **one** generic hook:
   `espnowOnFileReceived(senderMac, filename, finalPath)`.
3. The **bond module** owns that hook: it recognizes its filenames and processes
   them **by reading the file back from flash** (small config files — a cheap read),
   instead of from a transfer-layer RAM buffer.

Result: the transfer layer is reusable and streaming-friendly; bond logic lives in
the bond module; "process a received settings file" is decoupled from "a file
arrived over ESP-NOW."

## Why this is a SEPARATE change from Phase A (don't bundle)

- **No synergy.** With Phase A's hybrid routing, the special files are all < 128 KB
  → they take the **RAM path**, which streaming never touches. So streaming is
  already clean *without* this. Decoupling buys architecture, not streaming.
- **Different risk class.** Phase A is *additive* (cannot regress anything that
  works). Decoupling **moves working, two-device-only-testable settings-sync code**
  → real regression surface. Bundling them forfeits Phase A's "can't regress
  anything" guarantee and tangles a streaming bug with a sync bug — if HW breaks you
  can't tell which change did it.

## Design questions to resolve before coding

1. **Where do special files land?** Today they're processed-and-discarded (never
   written to `/espnow/received`). "Deliver to flash, then hook" means they'd now be
   **saved** — clutter — unless the hook deletes them after processing. Decide:
   dedicated path (e.g. `/system/bond/inbox/`) vs. inbox + cleanup.
2. **Redundancy.** `processBondSettings` already calls `cacheSettingsToLittleFS`,
   which writes its own cache copy. If the transfer also writes the file, that's a
   **double write** — make the hook either (a) rename the delivered file into the
   cache location, or (b) keep `cacheSettingsToLittleFS` and delete the delivered
   copy. Pick one; don't keep both.
3. **Timing/state.** Bond sync is a state machine (`bondSyncInFlight`, retries,
   drift CRC). The hook must feed it the same "settings arrived" event the inline
   call does today — verify no ordering assumptions break.

## Concrete change sketch

- `System_ESPNow.cpp / v4h_file_end`: delete the `strcmp` chain; after a successful
  publish call `espnowOnFileReceived(sndMac, filename, finalPath)`. Both RAM and
  streaming paths converge on "write to flash → fire hook."
- New `espnowOnFileReceived()` (bond module / `#if ENABLE_BONDED_MODE`): the
  `strcmp` chain moves here; each branch reads its file from `finalPath` and calls
  the existing `processBond*` (signatures unchanged — they still take a `String`,
  now sourced from a flash read instead of the RAM buffer).
- `automations.json` summary: read back from flash to log, or drop the cosmetic
  summary.

## Test plan (why two devices)

Settings/schema/manifest sync only exercises with **two bonded devices**:
`bondconnect`, force a settings change, confirm the peer caches + the drift CRC
matches, exercise peer-reboot / HB-timeout / role-swap re-sync paths. Without that,
this change is unverifiable — which is exactly why it waits behind Phase A.
