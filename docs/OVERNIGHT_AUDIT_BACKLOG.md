# Overnight Audit Backlog

**Started:** 2026-07-27 (evening)
**Mode:** READ-ONLY. Investigate, analyze, and report. **Never edit source files.**
The only files this run may write are the three output docs listed below and this backlog file.

> ## ⚠ RUN HALTED — monthly spend limit, 2026-07-27 ~23:30
>
> A 33-finder parallel run was launched and killed partway through. **6 of 53 agents completed**
> (2.97M tokens, 64 min): the 5 recon maps, and the A8 cross-core finder. Everything else — 32
> finders, all adversarial verifiers, all critics, all synthesis writers — died on
> "You've hit your monthly spend limit".
>
> **This is a MONTHLY limit, not a 5-hour rate limit. It does NOT reset overnight.**
> Raise it at claude.ai/settings/usage, or wait for the billing cycle, before resuming.
>
> **Completed:** A0 (checklist), A8 (cross-core — findings unverified, see the doc's run-status header).
> **Recon maps survived and are reusable** — they cost ~5 agents and are the expensive part of any
> restart. They live in the session scratchpad; copy them somewhere durable before that is cleaned up:
> `/private/tmp/claude-501/-Users-morgan-esp-hardwareone-idf/d9ef9d49-21ee-449a-a8a4-ba5a19f1830f/scratchpad/recon_*.md`
>
> **On resume, do these first** (highest expected value, based on what A8 surfaced):
> 1. **B0 + B4** — the trust-model map and the two known-open holes (FTS AuthBypass, MQTT skipping
>    `authorizeCommand`). Zero security work was completed. This was the user's explicit second ask.
> 2. **A8 follow-up** — sweep the ~40 `WebPage_*` handlers for the same lock-free-reader pattern
>    against the `tskNO_AFFINITY` httpd task. A8 traced only `WebPage_Maps.cpp` and found a HIGH there.
> 3. **B10/B11** — A8 incidentally established that the OLED console ring "routinely holds echoed
>    `login <user> <pass>` plaintext". That credentials reach a display buffer at all is a security
>    finding nobody audited; chase where else that echo lands (event history, log files).

---

## Resume protocol (read this first, every wake-up)

1. Read this file top to bottom.
2. Find the first item whose status is `TODO` (scan tracks in order A → B → C, but any
   `IN-PROGRESS` item always takes priority — finish it before starting anything new).
3. Mark it `IN-PROGRESS` with a timestamp, do the work, append findings to the matching
   output doc, then mark it `DONE` with a one-line result summary right here.
4. Do **one item per wake-up** unless items are trivially small, in which case do 2–3.
   Depth beats coverage — a shallow pass on 40 items is worth less than a real pass on 12.
5. Schedule the next wake-up and stop.

If a wake-up starts and every item is `DONE`, write the synthesis pass (bottom of this file)
and then stop the loop.

**Never** leave an item `IN-PROGRESS` at the end of a wake-up. If you run out of room,
downgrade it back to `TODO` and note what was already covered in its notes line.

---

## Output docs

| Doc | Contents |
|---|---|
| `docs/ESP32_PITFALL_AUDIT.md` | Track A — ESP32/ESP-IDF common-mistake checklist + this codebase measured against it |
| `docs/AUTH_SECURITY_REVIEW.md` | Track B — auth, users, roles, sessions, secrets, command authorization |
| `docs/CODE_HEALTH_SWEEP.md` | Track C — general robustness findings that aren't ESP32-specific or auth-specific |

### Finding format (use this everywhere)

```
### [SEV] Short title
- **Where:** path/to/File.cpp:123 (and any other sites)
- **What:** what the code actually does
- **Why it matters:** the concrete failure — inputs/state → wrong behavior, crash, or exposure
- **Confidence:** CONFIRMED (read the code, traced it) | PLAUSIBLE (pattern match, not traced)
- **Suggested direction:** one or two sentences. DO NOT IMPLEMENT.
```

Severity: `CRITICAL` / `HIGH` / `MEDIUM` / `LOW` / `NOTE`.

**Rules that matter more than volume:**
- Do not report a finding you have not opened the file and read. No speculative findings.
- Board-gated code (`#if BOARD_*`) hides compile breaks — note which board a finding applies to.
- This project has an extensive prior-audit history in `docs/*_AUDIT.md`. Before filing,
  grep those docs; if a finding is already known, either skip it or mark it
  `KNOWN — see docs/X.md` and only report if the status has changed.
- No backwards-compat concerns exist in this project (user erases before flashing).
- Prefer 5 confirmed findings over 30 plausible ones.

---

## Track A — ESP32 / ESP-IDF common mistakes

**A0 is the gate: do it first, it produces the checklist everything else in Track A uses.**

| # | Status | Item |
|---|---|---|
| A0 | **DONE** | Web research pass. → 45 pitfalls (P1–P45) in 6 groups written to `docs/ESP32_PITFALL_AUDIT.md` §1, with a coverage map assigning every pitfall to an A-item. Platform pinned: **ESP32-S3, IDF v5.5.1**, dual-core Xtensa, Arduino APIs in use, secondary ESP32 target. **A1–A21 must work their assigned pitfalls from that coverage map** — do not re-derive the list. |
| A1 | TODO | Task stacks: every `xTaskCreate*` call — size, overflow headroom, `uxTaskGetStackHighWaterMark` coverage. NOTE: in this repo `*_STACK_WORDS` constants are actually BYTE counts. |
| A2 | TODO | ISR safety: every ISR/callback — `IRAM_ATTR`, no blocking calls, `...FromISR` variants used, no float, no logging, no heap alloc. |
| A3 | TODO | Flash/cache: any flash write (NVS, LittleFS, OTA) that can run while an ISR needs cached code; `CONFIG_SPI_FLASH_AUTO_SUSPEND` posture. |
| A4 | TODO | PSRAM caveats: DMA-incapability, ISR access, cache-miss latency in hot paths, allocation-failure fallbacks. Cross-check against `docs/HEAP_OFFLOAD_SWEEP_2026-07-24.md`. |
| A5 | TODO | Internal DRAM exhaustion + heap fragmentation: worst-case allocation paths, `heap_caps_get_largest_free_block` posture. |
| A6 | TODO | Watchdogs: TWDT/IWDT config, every long-running loop, who feeds what, panic-on-timeout posture. |
| A7 | TODO | FreeRTOS misuse: priority inversion, unbounded queues, `vTaskDelay(0)`, busy-waits, core-pinning policy adherence (`System_TaskUtils.h`). |
| A8 | **DONE** | Cross-core/thread safety. → 11 findings (2 HIGH, 4 MEDIUM, 4 LOW, 1 NOTE) + 13 verified-clean items in `docs/ESP32_PITFALL_AUDIT.md`. **UNVERIFIED** — the adversarial verifiers were killed by the spend limit; 3 of 11 spot-checked by hand and confirmed. Left uncovered: ~40 web handlers vs the `tskNO_AFFINITY` httpd task (largest remaining surface), `gSettings`, automation/mesh tables, and all `ENABLE_*=0` subsystems. |
| A9 | TODO | `esp_err_t` return values ignored — enumerate unchecked calls, rank by consequence. |
| A10 | TODO | I2C robustness: bus-hang recovery, timeouts, clock stretching, shared-bus arbitration, device-absent handling. |
| A11 | TODO | WiFi/BLE/ESP-NOW coexistence: channel conflicts, radio ownership, ADC2-vs-WiFi, init/deinit ordering. Cross-check `docs/WIFI_RADIO_OWNERSHIP_PLAN.md`. |
| A12 | TODO | NVS + flash endurance: write frequency on hot settings, wear-leveling posture, partition sizing, full-NVS handling. |
| A13 | TODO | Filesystem (LittleFS) power-loss safety: partial writes, no-atomic-rename patterns, corruption recovery, free-space exhaustion. |
| A14 | TODO | Buffer/bounds: `strcpy`/`strcat`/`sprintf`/fixed arrays/`memcpy` with attacker- or sensor-influenced lengths. |
| A15 | TODO | Arduino `String` heap churn in hot paths and in low-stack tasks. Cross-check `docs/HOT_PATH_HEAP_AUDIT.md` (that audit is PAUSED — note what it never reached). |
| A16 | TODO | Time: `millis()`/`micros()` rollover, `int` vs `uint32_t` deltas, monotonic-vs-wallclock mixing, RTC sync assumptions. |
| A17 | TODO | Boot path: failure resilience — what happens if each init step fails? Any init that can wedge or bootloop. |
| A18 | TODO | Power: brownout detector config, CPU-freq changes vs in-flight peripheral I/O (see the known `setCpuFrequencyMhz` vs `gps_task` issue), sleep-mode re-init. |
| A19 | TODO | Memory leaks: allocation paths with early returns, missing frees on error branches, task-deletion cleanup. |
| A20 | TODO | GPIO: strapping-pin usage, pull configuration, ADC2 conflicts, pin reuse across board variants. |
| A21 | TODO | `sdkconfig` reality-check: verify the config actually matches what code and docs assume (PSRAM mode, flash freq, task WDT, log level, coredump, BT alloc). |
| A22 | TODO | Long-tail sweep: any A0 checklist item not covered by A1–A21 — work through the remainder. Repeat this item as many times as needed; it is the overflow valve. |

## Track B — Auth / user / security review

| # | Status | Item |
|---|---|---|
| B0 | TODO | **Map the trust model.** Enumerate every command-entry surface (serial, web, `/api/cli`, MQTT, ESP-NOW, BLE/G2, OLED, automations, autostart, scheduled) and every role (Guest → User → Admin → Super Admin). Produce a surface × role matrix in `docs/AUTH_SECURITY_REVIEW.md` showing where `authorizeCommand` is actually called. This is the map the rest of Track B works against. |
| B1 | TODO | `System_AuthIdentity.{h,cpp}`: per-task TLS identity — can identity leak across tasks, survive a task reuse, or default to elevated? Audit every `ExecIdentityGuard` scope for early-return leaks. |
| B2 | TODO | `System_User.{h,cpp}`: password storage — hashing algorithm, salt, iteration count, constant-time compare, what lands in flash and in RAM. |
| B3 | TODO | Session/token handling on web: generation entropy, expiry, revocation on password change/user delete, fixation, cookie flags (`HttpOnly`/`SameSite`/`Secure`). |
| B4 | TODO | Verify the two open items in the security backlog memory: **FTS AuthBypass** and **MQTT bridge skips `authorizeCommand`**. Confirm or refute against current code; if real, write the full finding. |
| B5 | TODO | Privilege escalation: every path that can change a user's role or grant Super Admin. Can a lower tier reach any of them (directly, via automation, via autostart, via a command that runs as someone else)? |
| B6 | TODO | Guest role re-audit: enumerate every action reachable as Guest across all surfaces and confirm each is genuinely read-only. Cross-check `project_guest_role_audit` findings. |
| B7 | TODO | Command authorization coverage: for every registered command, confirm its declared privilege matches what it can actually do. Look for under-declared commands (a "read" command that writes). |
| B8 | TODO | ESP-NOW security: HMAC coverage, replay protection, nonce/counter handling, key storage, what bond mode exposes, whether the RCE channel is reachable outside bond mode. |
| B9 | TODO | Secrets at rest and in flight: flash encryption is OFF and PSRAM is probeable — trace every secret (user passwords, WiFi creds, ESP-NOW keys, API tokens) and confirm none lands in PSRAM or in a world-readable file. |
| B10 | TODO | Secret leakage through output: logs, CLI echo, web responses, BLE/G2 notifications, event history, notification sinks. Includes the known open **G2 text-entry submit-echo leak**. |
| B11 | TODO | Login hardening: rate limiting, lockout, timing-attack surface on username and password compare, user enumeration via differing error messages or response timing. |
| B12 | TODO | First-boot / provisioning: default credentials, what an un-provisioned device accepts, recovery/reset paths and who can trigger them. |
| B13 | TODO | Injection: command strings built from user input (automations, autostart, web params, ESP-NOW payloads, MQTT topics) — can a value break out into a second command? |
| B14 | TODO | Web endpoint sweep: every handler in `WebPage_*` / API routes — auth check present, correct role, CSRF posture on state-changing endpoints. |
| B15 | TODO | Untrusted-input parsers: ESP-NOW frame parsing, JSON parsing, CLI tokenizer, file-upload paths — malformed/hostile input handling. This is the highest-value memory-safety surface. |
| B16 | TODO | Long-tail: anything B0's matrix flagged as unexplained, plus a final adversarial pass — "if I had a radio and no credentials, what would I try first?" Repeat as needed. |

## Track C — General code health (overflow track; only start once A and B are exhausted, or if blocked)

| # | Status | Item |
|---|---|---|
| C1 | TODO | Error-path audit: functions that report success on partial failure; the uniform `OK:`/`Error:` contract vs actual behavior. |
| C2 | TODO | Reboot/persistence integrity: settings write → reboot → read round-trip for every persisted file; what a truncated file does. |
| C3 | TODO | Dead code and unreachable branches — list only, and re-verify before believing (prior audits over-reported here). |
| C4 | TODO | Comment/doc accuracy against current behavior in the auth and ESP-NOW subsystems. |
| C5 | TODO | Duplicated logic that should call a shared helper (web handlers vs OLED vs G2 vs CLI paths). |
| C6 | TODO | Test-coverage gaps: what in the test suite would not catch the findings from Tracks A and B. |

---

## Synthesis pass (run only when every item above is DONE)

Write `docs/OVERNIGHT_AUDIT_SUMMARY.md`:
1. Top 10 findings across all tracks, ranked by (severity × confidence × blast radius).
2. Anything CRITICAL or HIGH, called out separately with the one-line reason it's urgent.
3. What was checked and came back **clean** — this is as valuable as the findings.
4. What was NOT covered and why.
5. A suggested fix order, with the cheap/safe ones separated from the invasive ones.

---

## Wake-up log

| Time | Item | Result |
|---|---|---|
| 2026-07-27 22:35 | A0 | Checklist built: 45 pitfalls, ESP32-S3 / IDF v5.5.1. Coverage map assigns all 45 to A1–A21. Next tick starts A1 (task stacks). |
