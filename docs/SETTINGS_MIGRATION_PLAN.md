# Settings across firmware updates

What happens to `/settings.json` when a new build boots against a file written
by an older one, which of the proposed fixes survive contact with the source,
and what each one actually touches.

Written 2026-08-06 against v0.99.82. Every claim below was re-derived from
source; where the first-pass analysis was wrong, the correction is stated
rather than quietly dropped.

## Why this matters now

Until OTA, the answer was "erase before flashing" and none of this applied. A
signed OTA update replaces `ota_0` only - `littlefs` and `nvs` survive - so from
1.0 onward a new build routinely boots against an older build's settings file.

---

## Part 1: what happens today

### A key the new build knows, absent from the old file

It gets the compiled-in `SettingEntry` default, silently. The order is what
makes this safe:

| Step | Where |
|---|---|
| `settingsDefaults()` -> `applyRegisteredDefaults()` stamps every registered field with its row default | `HardwareOne.cpp:1392` |
| `readSettingsJson()` overlays the file | `HardwareOne.cpp:1401` |
| Missing key hits `if (val.isNull()) continue;` and the default survives | `System_Settings.cpp:2350` |

A new setting therefore cannot produce garbage - only whatever you declared as
its default. Two coarser skips sit above it: a missing `jsonSection` drops a
whole module to defaults (`:2339`), and a missing `group` drops that group
(`:2347`), each one `continue` earlier and equally silently.

The `val | e->intDefault` idiom on the following line is dead code for this
case - `isNull()` already skipped the row. It is live only for a
present-but-wrong-type value, which is Part 2.

### It does not self-heal

`writeSettingsJson()` runs at boot only when the file is **absent**
(`HardwareOne.cpp:1408-1411`). After an OTA the file exists, so nothing writes
back. New keys stay missing from disk until some unrelated save happens.

Consequence: `firmwareVersion` is only re-stamped inside `buildSettingsJsonDoc`
(`System_Settings.cpp:876`), so the `SYSEVT_FIRMWARE_CHANGED` "settings carried
over from prior firmware" event **re-fires on every boot** until a save occurs,
rather than once per update.

### A key the new build no longer knows

Saving merge-reads the existing file before rebuilding
(`System_Settings.cpp:1023-1041`), so unknown keys survive on disk. Good for
downgrades. But a *renamed* key both loses its value and leaves an orphan
forever, because nothing prunes.

`debug.json` and `notifications.json` behave the opposite way - both are full
rewrites, so an older build that saves either one silently deletes rows for
kinds it does not know. `settings.json` is downgrade-safe; those two are not.

### A key whose type changed between builds

The stored value is discarded and the compiled default installed. ArduinoJson's
`operator|` is literally:

```cpp
// ArduinoJson/Variant/VariantOperators.hpp:35
if (variant.template is<T>()) return variant.template as<T>();
else                          return defaultValue;
```

This has already happened once in this codebase: `g2StreamToneMap` changed
`SETTING_BOOL` -> `SETTING_INT`, so a stored `false` silently becomes the
compiled default.

### Not affected: event-kind renumbering

v0.99.82 inserted ten kinds into the middle of `SYSEVT_KIND_LIST`. This breaks
nothing. Notification masks and automation triggers persist kind **names**, not
bit indices, resolved through `systemEventKindFromName()` at load
(`System_Notifications.cpp:185`, `System_Automation.cpp:2295`). Unknown names
are skipped deliberately; automations parse an unknown name as kind 0 rather
than dropping the trigger and corrupting sibling positional writes.

---

## Part 2: the proposed fixes, validated

### Correction to two earlier claims

Two things stated in the initial analysis were wrong, and both change the
prescription:

**1. "Range violations warn loudly; type mismatches are silent."** Both are
silent at boot. `WARN_STORAGEF` reaches `debugQueuePrintf`, which returns
immediately when the debug queue is null (`System_Debug.cpp:793`), and
`initDebugSystem()` does not run until `HardwareOne.cpp:1573` - **172 lines
after** the settings load at `:1401`. The existing clamp warnings at
`System_Settings.cpp:2299/2304` have never appeared on a boot load. The
asymmetry is in the code, not in anything an operator can see.

**2. "Write settings back on a detected version change."** As phrased, this
factory-resets the device. See Fix 2.

### Fix 1 - report type mismatches. RECOMMENDED, with a corrected sink

The idea is right; the mechanism named for it does not work, and the
"legitimate cross-type" intuitions are backwards for this library.

**Sink.** Must be `logSystemEvent()`, which has a pre-init Serial plus 12-slot
ring fallback (`System_Debug.cpp:851-860`). Not `WARN_STORAGEF`. The two
existing clamp helpers should be switched at the same time, since they are
currently mute for the same reason.

**Which pairs are genuinely wrong.** Verified against the library's own
predicates - `isBoolean()` is an exact tag test (`VariantData.hpp:365`),
`isFloat()` is a `NumberBit` mask (`:373`):

| Declared | Accepts | Silently discarded today |
|---|---|---|
| `INT`/`U8`/`U16`/`U32` | integer tags only | **whole-number floats like `5.0`**, bools, strings |
| `FLOAT` | every numeric tag, integers included | bools, strings |
| `BOOL` | `true`/`false` only | **`0`/`1` integers**, `"true"` strings |
| `STRING` | string tags | numbers, bools |

Two of these are the opposite of the natural assumption. `5.0` into an int
field is a real mismatch. `0`/`1` into a bool field is a real mismatch. But an
integer into a float field is *normal and is the firmware's own output* - the
serializer strips trailing zeros, so `2.0f` round-trips as `2`
(`FloatParts.hpp:86-90`). A naive `is<T>()` check on floats would warn on
almost every float setting on every boot.

**One true false-positive class to exclude:** a valid integer too large for
`int` fails `is<int>()` via `canConvertNumber` (`convertNumber.hpp:24-30`).
That is a range fault, not a type fault, and must be reported separately.

**Must `continue` after warning**, not fall through - otherwise the clamp
helpers receive the compiled default and can emit a bogus "below min" line, and
`count++` over-reports "Applied N settings".

**Rate-limit it.** The risk is not CPU (~450 entry iterations per boot, one
extra tag test each) but the 12-slot pre-init event ring
(`System_Debug.cpp:827-831`), which keeps the *first* 12 entries. An unbounded
flood would evict the boot record. Name the first few, then aggregate.

**Downstream surface:** `readRegisteredSettings` (`System_Settings.cpp:2326`)
has exactly two callers - `readDebugJson` (`:1237`) and `readSettingsJson`
(`:1401`). Sibling dispatchers that read the same `SettingEntry.type` and must
stay consistent if any repair logic is added:
`applyRegisteredDefaults` (`:2249`), `writeRegisteredSettings` (`:2432`),
`System_SettingsEditorCore.cpp:69`, `G2_Page_Settings.cpp:214`.

**Worth doing in the same edit:** `SETTING_U32` uses `val | e->intDefault`
(`:2378`), silently capping U32 loads at `INT_MAX`.

### Fix 2 - write back on version change. REJECTED as specified

This would factory-reset the device on the first boot after any OTA that
changes the version string.

The version check is at `System_Settings.cpp:1370-1378`. `readRegisteredSettings()`
- the call that actually copies file values into RAM - runs at **`:1401`**,
thirty lines later. A `writeSettingsJson()` inside the version-change branch
serialises a RAM image still holding nothing but `applyRegisteredDefaults()`
values, then renames it over the user's real settings.

`blePeersReadJson` at `:1386` is also after the check, and ESP-NOW mesh and
bond arrays are deserialised later still.

**What to do instead**, in increasing order of ambition:

1. **Re-stamp only `firmwareVersion`.** Stops the event re-firing every boot.
   Does not materialise new keys, but new keys already resolve to their
   defaults, so nothing is broken by leaving them absent.
2. **Mark dirty and let the existing save path run**, after boot completes and
   every subsystem has populated its state. No new write path, no new failure
   mode.

Do not write inside `readSettingsJson()` under any condition.

### Fix 3 - add `_schemaVersion`. NOT YET

Safe but not useful, and subtly wrong in shape.

An unrecognised top-level key breaks no consumer: preserved by merge-on-save,
preserved verbatim by `.hwbackup` export/restore, opaque to
`tools/ota/device_backup.py`. So it *could* be added harmlessly.

But `settings.json` is a **union document** - many independent modules'
sections in one file. A single top-level version would falsely assert that they
all move together, which is exactly the claim that makes migrations go wrong
later. And nothing would read it: a version field with no consumer is dead
weight that acquires false authority over time.

Revisit when there is a first real migration to hang off it, and prefer
per-module versioning if it happens at all.

### Fix 4 - raise `MAX_SETTINGS_MODULES`. DONE (32 -> 36)

There are exactly 35 `registerSettingsModule()` call sites in
`registerAllSettingsModules()`, most `#if`-guarded, so the true worst case for
a maximal build is 35. 36 leaves exactly one spare.

No shipping board reaches 32 today - the OTA build has sensors off and is well
clear - so this is headroom, not a live bug fix.

**Downstream:** two arrays key off the macro, and they must move together:
`gSettingsModules[]` (`System_Settings.cpp:2031`, PSRAM BSS) and the
`modulePointers[]` snapshot in `OLED_RemoteSettings.cpp:239`, which is the
destination bound for the bonded-peer module list. That source array is
heap-allocated from the peer's own count (`ps_calloc`, `:182`), so only the
destination needed raising. Cost is 4 pointers in each - 32 bytes total.

**Left alone deliberately:** the overflow path at `System_Settings.cpp:2037`
logs via `ERROR_SYSTEMF` and returns. That message is subject to the same
pre-init muteness as Fix 1's - registration runs inside `settingsDefaults()`,
long before `initDebugSystem()`. A dropped module neither loads nor persists
any of its settings and runs on compiled defaults forever, so if the cap is
ever approached this deserves a `logSystemEvent()` too. Not urgent at 35/36.

---

## Recommended order

1. **Fix 1** - type-mismatch reporting via `logSystemEvent`, plus fixing the two
   clamp helpers' dead sink and the `U32` default cast. This is the only one
   that closes actual silent data loss.
2. **Fix 2, variant 1** - re-stamp `firmwareVersion` so the update event fires
   once. Small and self-contained.
3. **Fix 4** - done.
4. **Fix 3** - not yet.

## Separately: `meshRelay`

Defaults to `true` and shipped in v0.99.8, the OTA release itself
(`System_Settings.h:219`, `System_ESPNow.cpp:16981`). Any pre-0.99.8 node that
updates over the air starts forwarding other nodes' mesh traffic without
opting in. Group-key auth still applies, so this is an unconsented role change
rather than an auth hole - but it is the exact class of upgrade-unsafe default
worth auditing for before 1.0.

The pattern to copy for anything new is the G2 device module's `0 = Preserve`
encoding (`G2_Glasses.cpp:17891`) - the only default in the tree that is
correct on both a fresh flash and an upgrade.

---

# Part 3: implementation plan

Two changes. Fix 1 closes real silent data loss; Fix 2 is small and stops a
recurring false alarm. Neither touches the boot order, and neither serialises
RAM to disk - that constraint is what killed the original Fix 2 and it governs
both designs here.

## Change A - report and repair type mismatches

### A1. Give the settings loader a sink that works before `initDebugSystem()`

The blocker first, because everything else in A is invisible without it.

`settingsLoadClampInt` / `settingsLoadClampFloat`
(`System_Settings.cpp:2296-2324`) use `WARN_STORAGEF`, which is inert this
early. Switch both to `logSystemEvent()`. This is a two-line change that also
retroactively fixes range-clamp reporting, which has never worked on a boot
load.

Keep `WARN_STORAGEF` nowhere in `readRegisteredSettings` or its helpers.

### A2. Add a type gate before the switch

New file-scope helper next to the clamp helpers, and a counter beside the
existing `gSecretLoadFailures`:

```c
// Why not just let ArduinoJson's operator| handle it: that idiom is
//   is<T>() ? as<T>() : defaultValue
// so a wrong-typed value is indistinguishable from an absent one. Both
// silently install the compiled default. On a cable-flashed device that was
// fine; under OTA a type change between builds is a real upgrade path, and it
// must not discard a user's value without saying so.
static uint32_t gSettingsTypeMismatches = 0;

static bool settingsTypeAccepted(JsonVariantConst val, const SettingEntry* e,
                                 const char** why) {
  switch (e->type) {
    case SETTING_INT: case SETTING_U8: case SETTING_U16: case SETTING_U32:
      if (val.is<int>()) return true;
      // is<float>() is a NumberBit mask, true for EVERY numeric tag. So a
      // numeric value that failed is<int>() is fractional or out of int
      // range - a range fault, not a type fault. Keep them distinct or the
      // message sends the reader hunting for the wrong bug.
      *why = val.is<float>() ? "number is fractional or out of int range"
                             : "value is not a number";
      return false;
    case SETTING_FLOAT:
      // Integers are accepted deliberately: the serializer strips trailing
      // zeros, so this firmware's own 2.0f is re-read as the integer 2.
      if (val.is<float>()) return true;
      *why = "value is not a number";
      return false;
    case SETTING_BOOL:
      // isBoolean() is an exact tag test - a stored 0/1 does NOT pass.
      if (val.is<bool>()) return true;
      *why = "value is not true/false";
      return false;
    case SETTING_STRING:
      if (val.is<const char*>()) return true;
      *why = "value is not a string";
      return false;
  }
  return true;
}
```

Call site, immediately after the existing `isNull()` guard at
`System_Settings.cpp:2350` and before the `switch`:

```c
      const char* why = "";
      if (!settingsTypeAccepted(val, e, &why)) {
        gSettingsTypeMismatches++;
        if (gSettingsTypeMismatches <= 4) {
          logSystemEvent("SETTINGS", "%s: %s - keeping compiled default",
                         e->jsonKey, why);
        }
        continue;
      }
```

`continue` is mandatory, not stylistic. Falling through would hand the compiled
default to the clamp helpers, which can emit a bogus "below min" line if any
entry's default sits outside its own declared range, and would inflate the
"Applied N settings" count.

The `<= 4` cap protects the 12-slot pre-init event ring
(`System_Debug.cpp:827-831`), which keeps the *first* twelve entries - an
unbounded flood would evict the boot record. Emit one aggregate line after the
module loop when the counter exceeds 4.

### A3. Repair the two mismatches an OTA actually produces

Logging alone still loses the value. Two cases are unambiguous and worth
accepting rather than discarding:

- **Whole-number float into an int field** (`5.0` -> `5`): accept when
  `d == floor(d)` and it fits the target width.
- **`0`/`1` into a bool field**: accept as false/true.

Anything else keeps the default and logs. Do not attempt string-to-number or
number-to-string coercion - those are genuine schema changes and should be
loud.

### A4. Fix the U32 width bug found on the way

`System_Settings.cpp:2378` reads `(uint32_t)(val | e->intDefault)`. The `|`
resolves as `int`, so any stored value above `INT_MAX` fails `is<int>()` and
silently becomes the default. Change to `val | (uint32_t)e->intDefault`.

This is independent of migration - it is wrong today on a fresh flash too.

### Downstream touched by Change A

| Thing | Where | Why it matters |
|---|---|---|
| `readRegisteredSettings` | `System_Settings.cpp:2326` | the edited function |
| `readDebugJson` | `:1237` | caller 1 of 2 |
| `readSettingsJson` | `:1401` | caller 2 of 2 |
| `settingsLoadClampInt/Float` | `:2296`, `:2311` | sink swap in A1 |
| `gSecretLoadFailures` | `:2403` | new counter sits beside it |
| `applyRegisteredDefaults` | `:2249` | reads the same `.type`; must stay consistent |
| `writeRegisteredSettings` | `:2432` | ditto |
| `System_SettingsEditorCore.cpp:69`, `G2_Page_Settings.cpp:214` | | ditto, UI dispatchers |

No new SYSEVT kind. `logSystemEvent` is the untyped path and needs no X-macro
change - deliberately, since this is diagnostic detail rather than an event
anyone should automate on.

### Verifying Change A

Hardware, in this order:

1. Boot once unmodified, confirm nothing new appears in the event log.
2. Hand-edit `/settings.json` over the web file editor: set an int-typed key to
   `5.0`, a bool-typed key to `1`, and a string-typed key to a number. Reboot.
3. Expect three named lines in the boot event log, the first two values
   *preserved* (A3 repair), the third reverted to default with a message.
4. Set six keys wrong; expect four named lines plus one aggregate.
5. Set an int key below its declared min; expect the clamp line - which is the
   proof A1 worked, since that message has never appeared before.

## Change B - stop the update event firing on every boot

### The constraint

`writeSettingsJson()` builds the document from RAM
(`buildSettingsJsonDoc`, `System_Settings.cpp:874`). Anything that calls it
during boot risks persisting defaults over real values, and
`putSecretPreserving` (`:581`) writes `String("")` for a secret whose plaintext
is empty on a healthy load - so a mistimed save can also blank a stored secret.
That is why the original Fix 2 was rejected.

### The shape that avoids all of it

Do not rebuild the document. Read the file, change one key, write it back:

```c
// Re-stamp ONLY firmwareVersion, from the parsed file rather than from RAM.
//
// buildSettingsJsonDoc() serialises live settings state, so calling it at boot
// can persist defaults over values that have not been loaded yet, and can
// blank a secret through putSecretPreserving's empty-plaintext branch. This
// path touches exactly one key of the on-disk document and preserves every
// other byte, including keys this build does not know.
static bool settingsRestampFirmwareVersion();
```

Implementation: `readText(SETTINGS_JSON_FILE)` -> `deserializeJson` ->
`doc["firmwareVersion"] = SelfDevice::firmwareVersion()` -> serialize to
`/settings.tmp` -> `renameGuarded`, reusing the atomic pattern already at
`System_Settings.cpp:1064-1095`.

### Where to call it

From `HardwareOne.cpp`, immediately after `readSettingsJson()` returns true,
guarded by a "version differed" flag that `readSettingsJson` sets. Not inside
`readSettingsJson` - keeping the write out of the loader is the whole point.

There is no boot-order dependency because RAM is never consulted.

### One honest caveat to document in the code

If the running image is an OTA trial that later fails probation and rolls back,
the file will briefly claim it was written by a version that is no longer
running. Harmless: the restored firmware detects the mismatch on its next boot
and re-stamps itself. Worth a comment so the next reader does not mistake it
for a bug.

An alternative - defer the re-stamp until `otaSystemOnImageMarkedValid()` - is
strictly more correct and strictly more complex, and does not exist on non-OTA
builds. Not worth it for an informational stamp.

### Verifying Change B

1. Note the current `firmwareVersion` in `/settings.json`.
2. Flash a build with a different `PROJECT_VER` without erasing.
3. First boot: expect exactly one `SYSEVT_FIRMWARE_CHANGED` and the on-disk
   stamp updated.
4. Reboot twice more: expect no further firmware-changed events.
5. Confirm by diff that no other key in `/settings.json` changed - especially
   any `AES:`-prefixed secret and any key the build does not know.

## Not doing

- **`_schemaVersion`** - `settings.json` is a union document and nothing would
  read the field. Revisit at the first real migration.
- **A general migration engine** - there is no migration to run. Defaults plus
  honest reporting cover every case that exists today.
- **Louder module-cap overflow** - real, but 35 of 36 used and unreachable on
  any shipping board. Fold into A1's sink change if it ever gets close.
