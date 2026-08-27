# EvenAI Ask-Display Debugging Plan

**Status: ADVERSARIALLY VERIFIED 2026-08-11 — 28 claims: 21 confirmed, 7
adjusted, 0 refuted.** Corrections are folded into the text below and listed in
the verification table at the bottom. One verifier cluster (fix-mechanics)
died mid-run and was re-executed; its addenda are marked ⟲ where applicable.

Symptom: after a "Hey Even" wake, the transcribed **question** (`g2evenai
askid` text) has trouble displaying on the lens before the **answer** replaces
it — sometimes it barely flashes, sometimes cut short, sometimes seemingly
absent. This plan produces a *deciding measurement* for each candidate cause.

## Ground truth from 2026-08-11 serial captures (arithmetic re-verified)

| Wake | Path | wake→ask | ask→ANALYSE | first reply after ANALYSE |
|---|---|---|---|---|
| `…0003` (13:25) | live | 11.84 s | **2.64 s** | ~0.1 s (single `replyid`) |
| `…0002` (13:22) | batch | 21.74 s (restart window) | **5.40 s** | ~0.4 s |
| 13:14 (empty) | live | — | **no askid ever sent** | "Sorry, I didn't catch that." |

- The glasses ACK every ask (`EvenAI ASK magic=…`), 0.04 s *before* the XIAO's
  UART OK reaches the Pi. Receipt is proven; **render is not** — no protocol
  event exists for "ask painted", and the daemon awaits nothing past the XIAO
  status OK ([pipeline.py:900](../cm5/ai-service/hw1_ai_service/pipeline.py)).
- The *slower* batch path gave the question twice the screen time of the fast
  live path — every latency win shrinks the question's on-lens life.

## The verified display pipeline

Wake → firmware CTRL{ENTER} + 3 s HEARTBEATs hold the native listening card
(~41 s bench lifetime, comment at G2_Glasses.cpp:10674-10678) → capture → STT
→ daemon clips transcript at 1900 B (`_CLIP_BYTES`, pipeline.py:37) and sends
`askid` **concurrently with LLM generation** (deliberate overlap; ~0.3 s UART
RTT hides under 0.5–0.9 s ttft) → firmware askid handler
(G2_Glasses.cpp:17790) builds **one** sid-0x07 ASK frame — text strnlen-capped
at 250 B, single-fragment envelope capped at 253 pb bytes with **17 B fixed
overhead ⇒ ask text ≤ 236 B builds; 237–250 B fails the build and
terminalizes the whole exchange** (EXIT/`send_failed`, :17802-17804; verified
arithmetic, magic is always a 2-byte varint per the 200–250 clamp at :10326) →
synchronous BLE write; glasses echo ASK (decoded in the generic else-branch at
:8860, *not* the EVENT raw dump) → glasses paint the question progressively
(**rate unknown**; sole calibration was one 104-char/1.84 s observation ⇒
~44 cps, contradicted by later ASK98 trials at <23 cps effective) → daemon
`_hold_for_ask_render` (pipeline.py:780-819) sleeps until `ack + len/44cps` —
verified: **no minimum-dwell floor anywhere** (`remain = ask_render_until −
now`, `remain<=0` falls straight through), and the :790-794 comment makes the
no-op deliberate whenever LLM latency exceeds the budget → first reply write
triggers firmware ANALYSE (exits LISTEN), `vTaskDelay(150)`, then REPLY
(G2_Glasses.cpp:10385-10402) → **the answer replaces the question**.

## Central finding (verified — this is the nugget)

The protection that should keep the question on screen exists but is **doubly
broken, by the project's own hardware record**:

1. `g2_ask_render_cps = 44.0` (config.py:140, consumed pipeline.py:760) came
   from a single calibration point. The hardware record —
   **`cm5/G2_EVENAI_RENDER_TEST_RECORD.md`** (path corrected; content verified
   at matching lines) — later ran five wearer-rated 98-char ASK trials, ALL
   rated *Cut* despite a 4.24 s window (:74-99), and **formally rejected the
   44 cps barrier** (:381: "Rejected — 98 chars remained cut with 4.24 s of
   native opportunity"). `config.example.yaml:96` itself labels the value
   "provisional… not calibrated". The record's own ordered next step — the
   **long-ASK gate at streamSpeed=40** (:390-399) — was never run.
2. Even a perfectly calibrated hold models only draw *completion*, never
   reading time: no floor term exists, so a short, fully-drawn question is
   replaced the instant the answer exists.

Corollary now precisely quantified: the daemon clips the shown ask at
**1900 B** while the firmware ceiling is **236 B** — any real transcript of
237+ UTF-8 bytes kills the *entire exchange* (no question, no answer).

## Ranked hypotheses — all five SURVIVED verification

| # | Hypothesis | Status | Deciding measurement |
|---|---|---|---|
| 1 | **Lens draws slower than the 44 cps budget** → long questions mid-paint when the answer lands (cut) | Stands | Run the record's owed **long-ASK gate at streamSpeed=40**: 98-char ask, stepped delays, wearer-rated + display stamps. Tracks ~20–25 cps ⇒ confirmed; completes in ~2.2 s ⇒ killed |
| 2 | **Zero minimum dwell** → short questions fully drawn but instantly replaced ("barely flashes") | Stands | Per-exchange `ask_ack → first-reply-write` gap + `hold_remain` — the `question` tap window already measures exactly this and is merely gated off |
| 3 | **ASK echoed but never painted** — glasses FSM advanced during long wake→ask gap; heartbeat failures are silently swallowed (stamp-before-send, no counter, :10752-10759) | Stands | Stepped askid delay after real wakes + heartbeat-suppress knob + failure counter. Echo-without-paint at a threshold ⇒ confirmed |
| 4 | **Anchor skew** — budget clock starts at UART OK; paint-start latency invisible | Stands (adjusted: stamp the echo at the :8860 else-branch, not the EVENT dump) | Distribution of `(ASK-echo − UART OK)` across exchanges under BLE load |
| 5 | **Oversize-ask terminalization** | Stands, **strengthened**: threshold is exactly **236 B / 237+ kills** | Daemon warning at 236; one deliberate 237-byte bench ask |

## Instrumentation plan (verified mechanics; corrections folded in)

| Item | Side | Effort | Reflash | Verified notes |
|---|---|---|---|---|
| **1. Firmware display stamps** — `ask_tx_ms / ask_echo_ms / analyse_tx_ms / first_reply_tx_ms` + heartbeat-fail count | fw | hours | ✅ | **Adjusted ×3:** append fields to the *existing* `evenai_timing` EVT (a NEW event name is dropped by `parse_event`'s allowlist, evenai_protocol.py:85-87) or extend the allowlist; stamp `ask_echo_ms` at the **:8860 else-branch** (ASK echoes bypass the EVENT dump); add a consumer — `jobs.py:284-288` currently discards parsed telemetry after an INFO log |
| **2. Daemon delivery stamps** → `ask_gap=`, `hold_remain=` on the `evenai timings:` line | daemon | minutes | — | Buildable; `accepted_at` is currently a local in `_send_physical_part` (:838) — add dataclass fields, and capture `hold_remain` **before** :815 nulls `ask_render_until` |
| **3. Ungate the `question` tap window** | daemon | minutes | — | Cheapest item; window already anchored ask-ACK→pre-first-reply; gated only by `cancel_marker_interval_s = 0.0` default (pipeline.py:166, __main__.py:63). Run with the flag today or decouple the gate |
| **4. Delivery timing in corpus JSON** | daemon | minutes | — | **Scope caveat:** corpus capture is live-path-only (:391-394) — batch exchanges write nothing; extend to batch or the 236 B question stays half-answerable |
| **5. Long-ASK gate at streamSpeed=40** — *the deciding experiment* | both | hours | — | Ordered by the record itself (:390-399); streamSpeed=40 submission path verified working |
| **6. ANALYSE-vs-REPLY separation probe** | fw | hours | ✅ | Buildable; note :10395 re-checks bound-arm after the 150 ms delay — a wearer dismiss during a long inserted hold yields ANALYSE-without-REPLY; the probe must log that branch, not misread it as "ANALYSE clears the question" |
| **7. Cheap rule-outs** — EVENT raw-hex watch + oversize warning | both | minutes | — | Dump at :8820-8826 correctly EVENT-scoped for the unmapped-event watch; set the oversize warning at **exactly 236 B** and clip the shown ask to 236 (not 1900) |

**Recommended order: 2+3+4 (+236 B clip) as one minutes-level daemon deploy →
5 (the deciding wearer experiment) → 1 (reflash bundle, ride the pending XIAO
flash) → 6+7 follow-ups.**

## Other issues surfaced (verified)

- **Empty-transcript UX**: skipping `askid` is by design but on-lens
  indistinguishable from a display failure; a synthetic ask ("(no speech
  detected)") would separate them.
- **The 1900 B vs 236 B clip mismatch** (see corollary above) — fix rides
  item 7's clip.
- **`route_link_event` discards parsed telemetry fields** after an INFO log —
  must gain a consumer before item 1's stamps mean anything host-side.
- **Heartbeat failures are invisible** — stamp-before-send, result ignored, no
  counter; one Busy send silently yields a ≥6 s gap (feeds H3).
- **Speed vs readability tension**: no dwell floor arbitrates; today's live
  win halved question screen time.
- Doc conflict: `G2_PROTOCOL.md` ENTER-path "stacked on the same plane" vs the
  measured REPLACE on the native wake path (`G2_PROTOCOL.md:1360-1365` also
  proves state-dependent rendering: ENTER+REPLY acks cleanly, renders nothing).

## Open unknowns (measurable only, not readable)

Actual ASK render cps + paint-start latency at streamSpeed=40; whether CONFIG
field 2 affects ASK rendering at all; whether ANALYSE or first REPLY clears
the question; replace vs clear-then-redraw; any protocol event marking ask
render completion; whether a late ASK is silently dropped with a clean echo;
heartbeat-gap tolerance of the listening card; streamSpeed persistence across
a glasses power cycle; ask re-send/update behavior (G2_NATIVE_EVENAI_SESSION
.md:255-257); whether 237+ B transcripts occur in practice (item 4 answers).

## Verification table (adversarial pass, 2026-08-11)

28 claims: **21 CONFIRMED · 7 ADJUSTED · 0 REFUTED**. Central finding survives
intact. Corrections applied above:

1. Render record path: `cm5/G2_EVENAI_RENDER_TEST_RECORD.md`, not `docs/`
   (all content citations confirmed at matching lines).
2. Oversize threshold exact: **236 B passes, 237+ terminalizes** (17 B fixed
   pb overhead; magic always 2-byte varint per the 200–250 clamp).
3. "No visibility past XIAO OK" is a mechanical fact (`_display_command`
   awaits only the UART status line), not a code comment as first written.
4. `_EvenAiDelivery` holds only `ask_render_until` (nulled at :815);
   `accepted_at` is a local — item 2 adjusted accordingly.
5. `config.example.yaml:96` says "provisional… not calibrated" — the
   pending-hardware-gate framing is this plan's, not the config's.
6. Host parser tolerance is scoped to `evenai_timing` only; new event names
   need an allowlist entry — item 1 adjusted.
7. ASK echoes decode at the :8860 else-branch, not the EVENT raw dump —
   item 1's echo stamp point corrected.

⟲ fix-mechanics cluster: the workflow verifier for this cluster failed twice
(API error, then stalled on all retries); its three unique claims were closed
by direct manual inspection instead (2026-08-11):
- `g2_evenai_probe.py` already has every knob item 5 needs: `trial` action
  with fixed identical ask text, `parse_delays` stepped-ms delays (:214-222),
  askid send (:443), reply delay anchored to the final ask OK (:453-454).
- `g2EvenAiSendAsk` is a true single chokepoint: defined once (:10370), one
  caller (:17793) — no second ask path can dodge the display stamps.
- The EVENT raw-hex dump is gated by `DEBUG_G2F` → runtime-settable debug
  flag; rule-out 7a needs no reflash.
