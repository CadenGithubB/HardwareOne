# Bond Mode Settings Architecture

## Core Principle

Bonded devices are **different hardware with different feature sets**. There is no
"mirror everything" relationship. Each device owns its own settings.

## Rules

1. **Settings changes on a peer** → use a **remote command** (`remote:<cmd>` / `@<cmd>`)
2. **Never push** a settings file to the peer on local changes
3. The initial sync fetches peer settings **read-only** for display (OLED, web) — never applied/mounted
4. The settings hash in the heartbeat detects peer changes → triggers re-fetch of the display cache

## How to Change a Peer's Settings

Settings commands use the setting name directly (e.g. `wifitxpower 20`, not `set wifitxpower 20`).

**CLI:**
```bash
remote:wifitxpower 20
@espnowenabled 1
```

**Web (bond page → /api/bond/exec):**
```javascript
fetch('/api/bond/exec', {
  method: 'POST',
  body: 'cmd=' + encodeURIComponent('wifitxpower 20')
});
```

**Programmatic:**
```cpp
executeCommand(ctx, "remote:wifitxpower 20", out, outSize);
```

## What Gets Transferred

| Item | When | Direction | Purpose |
|---|---|---|---|
| Settings file (read-only) | Initial sync + hash mismatch re-fetch | Master pulls from worker | Cached for OLED/web display |
| Settings hash | Every heartbeat (5s) | Both directions | Detect peer changes, trigger re-fetch |
| Remote commands | On demand | Either direction | Mutate peer settings |

## Code Locations

- **Remote command routing**: `System_Utils.cpp` `executeCommand()` — `remote:` / `@` prefix
- **Bond exec API**: `WebPage_Bond.cpp` `/api/bond/exec`
- **Settings fetch (sync only)**: `System_ESPNow.cpp` `requestBondSettings()` / `sendBondSettings()`
- **Settings cache (read-only)**: `System_ESPNow.cpp` `processBondSettings()` / `cacheSettingsToLittleFS()`
- **Settings hash**: `System_ESPNow.cpp` `computeBondLocalSettingsHash()` — recomputed on every `writeSettingsJson()`

## Security

- Remote commands require `isBondSynced()` + valid session token
- Session token derived from shared passphrase + MAC addresses
- Validated on every remote command via `isBondSessionTokenValid()`

---
**Last Updated:** 2026-02-27
