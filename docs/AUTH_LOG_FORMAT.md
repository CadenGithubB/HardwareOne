# HardwareOne — Auth / Security Log Format

Audience: anyone reading the on-device security logs or wiring a UI over them.
This documents the two login audit files, their line format, and every transport
that writes to them. It reflects the firmware after the pending flash (it rides the
next version bump on commit).

> Not to be confused with `errors.log` — see [§5](#5-relationship-to-errorslog).

---

## 1. The files

Both live under `/system/sys_logs/` and are append-with-cap ring files (oldest
lines drop when the cap is hit).

| File | Path | Cap | Written when |
|---|---|---|---|
| Successful logins | `/system/sys_logs/successful_login.log` | ~680 KB | auth event, `success=true` |
| Failed logins | `/system/sys_logs/failed_login.log` | ~680 KB | auth event, `success=false` |

The split is driven purely by the `success` bool inside `logAuthAttempt()`
(`success ? LOG_OK_FILE : LOG_FAIL_FILE`). The same event type can land in either
file depending on outcome.

Each file also receives a per-boot anchor line on NTP time-sync:
`… | Device Powered On | Time Synced via NTP`.

---

## 2. Line format

```
<timestamp> | <STATUS> | user=<who> | ip=<ip> | <path> [| reason=<reason>]
```

- `STATUS` — `SUCCESS` or `FAILED`.
- `user` — username for credential logins; peer name (or MAC fallback) for
  device-to-device events (`g2/pair`, `espnow/bond`).
- `ip` — real client IP for web; a synthetic tag otherwise (`web`, `local`, `ble`,
  `espnow`).
- `path` — the canonical transport tag (see §3). The audit filter keys off this.
- `reason` — optional human string (see §4).

Example lines:

```
2026-06-19 14:02:11 | SUCCESS | user=morgan | ip=192.168.1.42 | web/login | reason=Login successful
2026-06-19 14:03:50 | FAILED  | user=morgan | ip=ble | bluetooth/login | reason=Invalid credentials
2026-06-19 14:05:09 | SUCCESS | user=dev2   | ip=espnow | espnow/bond | reason=Bond session active (role=master)
2026-06-19 14:06:31 | FAILED  | user=dev2   | ip=espnow | espnow/bond | reason=Invalid bond session token
```

---

## 3. Transports (paths)

All credential logins flow through one front-door (`recordLoginAttempt`) that maps
the transport to a canonical path; device-grant events call `logAuthAttempt`
directly with their own path.

| Path | Source | Success line | Failure line |
|---|---|---|---|
| `web/login` | web cookie login | ✓ | ✓ |
| `serial/login` | serial console login | ✓ | ✓ |
| `bluetooth/login` | BLE credential login | ✓ | ✓ |
| `display/login` | OLED/local display login | ✓ | ✓ |
| `g2/pair` | G2 glasses pairing | ✓ | — (success-only by design) |
| `espnow/bond` | ESP-NOW bond (RCE channel) | ✓ | ✓ |

`g2/pair` is success-only: pairing is not a credential check that can "fail" in the
audit sense — a rejected pair simply doesn't stamp identity.

---

## 4. Reasons

### Credential logins (`*/login`)
- `Login successful` (SUCCESS)
- `Invalid credentials` (FAILED)
- `Locked out` (FAILED — too many recent failures)
- Credential rotation: `Password changed`, `Current password incorrect`,
  `Password storage failed`

### Device grants
- `g2/pair` — `G2 glasses paired` (SUCCESS)
- `espnow/bond` — **SUCCESS:** `Bond session active (role=master)` /
  `Bond session active (role=worker)` — the encrypted, token-capable command
  channel with the configured bond peer went live. Emitted once per session on the
  offline→online transition (not per heartbeat), on both master and worker.
- `espnow/bond` — **FAILED:**
  - `Invalid bond session token` — a peer (paired) presented a bad, malformed, or
    wrong-length bond token at the command-auth chokepoint. The direct mirror of a
    wrong password.
  - `Bond traffic from unpaired peer` — a peer attempted to drive the bond channel
    without being paired.

---

## 5. Operational notes (espnow/bond specifics)

- **Throttling.** Failed-bond lines are rate-limited to **one per 5 s**. Unlike a
  human typing a password, a hostile or looping peer can spam bond traffic; the
  throttle protects the 680 KB-capped file. Expect bursts to collapse to a single
  line.
- **Deferral.** Bond auth is detected on `espnow_task` (the RX drain / super-loop),
  where synchronous filesystem writes are forbidden. The log write is deferred to
  `cmd_exec_task`. There can be a small delay between the event and the line landing
  on disk.
- **Web-server gating.** `logAuthAttempt` lives in the web-server module, so all of
  the above is gated `#if ENABLE_HTTP_SERVER`. On a web-off build, no login/bond
  audit lines are written (same posture as every other auth-log caller).

---

## 6. Relationship to `errors.log`

`/system/sys_logs/errors.log` (256 KB cap) is a **separate** sink and is *not* an
auth log. It is fed by the debug pipeline: any message whose text starts with
`[ERROR]` (the `ERROR_*` macros) is appended, with a 2 s dedup window to prevent
rotation storms. Bond rejections that reach `failed_login.log` are *not* duplicated
into `errors.log` (they are not `[ERROR]` macro output). Keep the two distinct:
`*_login.log` = who tried to authenticate and whether it worked; `errors.log` =
application-level error spew.
