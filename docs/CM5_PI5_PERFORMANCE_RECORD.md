# CM5 / Pi 5 voice pipeline — performance record

Long-term record of every measurement taken on the G2 → XIAO → UART → Pi 5 voice
assistant path. Session of **2026-08-08**.

> **Current interpretation (2026-08-09):** this is a historical measurement
> record, not the current pipeline baseline. The source has since added leading
> and trailing WAV trim, background deletion, ASK/LLM overlap, streamed REPLY,
> background `replyend`, power profiles, and a wake-only EVT grace. Its stage
> timers also do not isolate SD/wire/STT/LLM/native-render work. The latest
> six-exchange host mean is 10.75 s, while five long answers continued to G2
> `STREAM_COMPLETE` for another 3.75 s on average. Use
> [`../cm5/LIVE_STT_G2_ASSESSMENT_2026-08-09.md`](../cm5/LIVE_STT_G2_ASSESSMENT_2026-08-09.md)
> for the corrected current assessment and
> [`../cm5/CM5_DEPLOYMENT_PATHS.md`](../cm5/CM5_DEPLOYMENT_PATHS.md) for the
> canonical `/home/caden/hw1-ai-service` deployment path. “Authoritative” below
> means authoritative for the named historical run only.

Every number is tagged **MEASURED** (read off hardware), **COMPUTED** (derived from
measured inputs by arithmetic), or **ESTIMATED** (modelled, not confirmed). Numbers
taken while the Pi was under-volted are kept and marked **INVALID** — see §2. They are
retained deliberately: they are what a browned-out Pi 5 looks like, and recognising that
signature again is worth more than the disk space.

## 1. Rig

| | |
|---|---|
| Glasses | Even Realities G2 |
| MCU | XIAO ESP32-S3 Sense, `HW_BOARD=xiao_s3`, 8 MB flash |
| Link | UART `/dev/ttyAMA2`, 8N1, **2 000 000 baud**, no CTS/RTS wired |
| Host | Raspberry Pi 5, 4 GB, Debian trixie, kernel 6.18.39+rpt-rpi-2712 |
| Python | CPython 3.13.5 |
| Service | `/home/caden/ai-service`, venv `~/hw1ai` |
| llama.cpp | build `3653e6d6d` (10326) |

Recordings live on **SD** (`/sd/recordings`) — confirmed at runtime, not assumed.
`micPrimaryRecordingsFolder()` returns `/sd/recordings` whenever `VFS::isSDWritable()`
(`System_Microphone.cpp:55-60`), and the firmware log shows
`voicefetch "/sd/recordings/rec_3965100.wav"`.

## 2. The under-voltage incident — read this before trusting any old number

Two hard crashes (SSH drop, red LED, power-cycle required) during back-to-back
`llama-bench` sweeps. **Cause: two devices sharing power across a dock plus a second
PSU.** Not thermal, not OOM.

Monitor evidence (MEASURED, logged from the Mac over SSH so it survived the crash):

```
14:58:40 temp=44.4'C throttled=0x0
14:58:42 temp=48.3'C throttled=0x50000     <- bit 16 under-voltage OCCURRED, bit 18 throttling OCCURRED
14:58:44 temp=49.4'C throttled=0x50005     <- + bits 0,2 = under-voltage NOW, throttled NOW
...
14:59:12 temp=56.0'C throttled=0x50000
```

After fixing the power split: `throttled=0x0` for a full four-model sweep, peak
**60.9 °C**. Temperature was never the issue — it throttled at 48 °C.

### The signature worth memorising

Under-voltage throttling drops the **CPU clock** but not the **memory clock**:

| | throttled | clean | delta |
|---|---|---|---|
| `pp128` (prefill, compute-bound) | 117.7 | 162.57 | **+38%** |
| `tg64` (decode, bandwidth-bound) | 27.6 | 28.64 | **+4%** |

*(Qwen3-0.6B-Q4_K_M, MEASURED both ways.)*

**A ~40% prefill drop with near-zero decode change is the fingerprint.** If prefill looks
bad and decode looks fine, check `vcgencmd get_throttled` before anything else.

Diagnostic that produced this — run it from the **Mac**, not the Pi, so the log survives
a hard power loss:

```bash
ssh caden@<pi> 'while :; do printf "%s %s %s %s\n" "$(date +%T)" "$(vcgencmd measure_temp)" "$(vcgencmd get_throttled)" "$(vcgencmd measure_volts)"; sleep 2; done' | tee ~/pi-monitor.log
```

## 3. LLM benchmarks

`llama-bench -p 128 -n 64 -t 4`. All MEASURED.

### 3a. Clean (power-fixed) — authoritative

| model | file | params | pp128 | tg64 |
|---|---|---|---|---|
| Qwen3-0.6B Q4_K_M | 372.65 MiB | 596.05 M | **162.57 ± 0.78** | **28.64 ± 0.38** |
| Qwen3-1.7B Q4_0 | 1.14 GiB | 2.03 B | **91.62 ± 0.50** | **10.87 ± 0.02** |
| Qwen3-1.7B IQ4_NL | 1.14 GiB | 2.03 B | **70.79 ± 0.20** | **10.89 ± 0.03** |
| Qwen3-1.7B Q4_K_M | 1.19 GiB | 2.03 B | **56.49 ± 0.58** | **9.18 ± 0.01** |
| Qwen3-1.7B Q5_K_M | 1.37 GiB | 2.03 B | **47.37 ± 0.21** | **7.86 ± 0.03** |

Note "Qwen3-1.7B" reports **2.03 B** params — embeddings push it well past the name.

**Effective memory bandwidth ≈ 10.4 GB/s** (COMPUTED: 28.64 tok/s × 0.364 GB). Decode
speed tracks `1/filesize` closely; this constant predicts any new model's `tg` to within
about 10%, with quant type the main source of error.

**Q4_0 is the pick.** Decode ties IQ4_NL, prefill wins by 29%. llama.cpp repacks Q4_0
onto the ARM i8mm/dotprod GEMM kernels, and that lands on prefill because prefill is
compute-bound. Decode (GEMV) is bandwidth-bound and gets nothing from it.

**Do not pick a quant by file size alone.** `IQ4_XS` is smaller than `Q4_0` and slower
here — the importance-matrix dequant costs more than the bandwidth it saves on a
Cortex-A76. Bench, don't extrapolate.

**Do not go below Q4 on a sub-3B model.** Small models take quantization damage
disproportionately; Q3_K_M on the 1.7B would likely lose to Q4_K_M on a 1.2B at
similar speed.

### 3b. Under-volted — INVALID, retained as the throttling fingerprint

| model | pp128 | tg64 | vs clean |
|---|---|---|---|
| Qwen3-0.6B Q4_K_M | 117.19 / 118.14 | 27.08 / 28.12 | pp −28% |
| Qwen3-1.7B IQ4_NL | 50.58 | 9.04 | pp −29%, tg −17% |
| Qwen3-1.7B Q4_0 | 65.01 | 7.65 | pp −29%, **tg −30%** |
| Qwen3-1.7B Q4_K_M | 40.17 | 8.24 | pp −29%, tg −10% |
| Qwen3-1.7B Q5_K_M | 33.74 | *(crashed mid-run)* | |

Q4_0 was hit hardest on decode and looked like the *worst* option. It is the best.
**Any ranking derived from a throttled run is untrustworthy, not merely scaled.**

### 3c. Sub-2B MoE — surveyed, none viable

Checked for a small mixture-of-experts model to break the size/speed tradeoff. Under 2 B
total, the field is effectively empty: IBM `granite-3.1-1b-a400m` (1.3 B / 400 M active)
is the only instruct-tuned option from a major lab, and by the `√(active × total)`
heuristic it lands at ~0.63 B-equivalent — no better than Qwen3-0.6B. Qwen's smallest is
14.3 B total, Microsoft's 42 B, Meta's 109 B; Gemma has no open MoE at all.

Reason: MoE decouples capacity from compute, which only pays when there is capacity to
decouple. Below ~3 B total the experts are too small to specialise. The smallest useful
MoE anywhere is `granite-3.1-3b-a800m` (3.3 B / 800 M active), and its GGUF repo could
not be located under the expected name.

## 4. UART voicefetch transfer

Reference transfer: **151,596 B file, 150 frames, 153,237 B on the wire** after COBS.

### 4a. The additive model

`total = wire + C`, where C is baud-independent. Validated by solving independently at
two bauds (MEASURED totals):

| baud | total | wire (COMPUTED) | C |
|---|---|---|---|
| 921 600 | 3.089 s | 1.663 s | **1.4263 s** |
| 2 000 000 | 2.205 s | 0.766 s | **1.4388 s** |

Two equations, one unknown, agreeing to **0.88%**. A constant that survives a baud change
is the signature of a term that is not on the wire.

Raising 921600 → 2 Mbaud was MEASURED at **28.6% off transfer** against a 28.8% prediction
— it bought the wire term and nothing else, because C dominated.

### 4b. Root cause #1 — pure-Python bit-serial CRC (FIXED)

`crc16_ccitt` in `link/protocol.py` was a bit-by-bit loop with no table, and the RX path
ran it over **4.01× the file's bytes** every transfer: three frame parses (all funnelling
through `parse_frame_body` — `transport.py:227`, `session.py:316`, `fetch.py:218`) plus
one whole-file check (`fetch.py:246`).

Pi timings (MEASURED):

| | bit-serial | `binascii.crc_hqx` | ratio |
|---|---|---|---|
| 1029 B frame | 1447.0 µs | 3.22 µs | 449× |
| 151,596 B whole file | 188.9 ms | 0.47 ms | 402× |
| **total per transfer** | **0.840 s** | **0.002 s** | |

`crc_hqx` verified bit-identical over **12,000 random vectors across four init values,
zero mismatches**; check value `0x29B1` both ways; chaining equivalence holds.
`_crc16_ccitt_bitwise` retained as the executable spec with a drift test.

### 4c. Root cause #2 — the SD card (NOT fixed)

Post-fix, MEASURED by regressing firmware `LOOPHEALTH` stall duration against frame count
across five transfers spanning 130–490 frames:

```
stall = 7.98 ms/frame + 432 ms fixed     (residuals ±120 ms)
```

Decomposition (COMPUTED against MEASURED slope):

| term | ms/frame |
|---|---|
| wire @ 2 Mbaud | 5.14 |
| **SD read** | **2.47** ← implies ~410 kB/s |
| firmware per-frame `uartCrc16` | 0.37 |
| **predicted total** | **7.98** (MEASURED 7.98) |

`cmd_voicefetch` reads the **entire file into PSRAM before emitting the first byte**
(`System_UartLink.cpp:561`), then runs a whole-file `uartCrc16` (`:571`). The card mounts
at **4 MHz SPI** — `System_VFS.cpp:145` hard-codes `uint32_t frequencies[] = {4000000, 400000}`
— giving ~500 kB/s raw, ~410 kB/s through FatFs. That matches an independent estimate of
330–460 kB/s derived from bus arithmetic and disassembly.

Projected if the SD clock were raised (COMPUTED, **untested**):

| SD clock | slope | 158-frame transfer |
|---|---|---|
| 4 MHz *(was)* | 8.04 | 1.27 s |
| **10 MHz** *(now)* | **6.50** | **1.03 s (+19%)** |
| 20 MHz | 6.02 | 0.95 s (+25%) |

**Changed to `{10000000, 4000000, 400000}`** — 10 MHz **added as a rung above** the old
4 MHz, not replacing it. A card or harness that cannot sustain 10 MHz therefore falls
back to the previously known-good 4 MHz instead of the 400 kHz limp rung, which would be
10× slower than before the change. 10 MHz is divisor-exact (ESP32-S3 SPI derives from the
80 MHz APB, so only 80/N is achievable; 80/8 = 10 MHz, while asking for 12 would quietly
give 80/7 = 11.43 MHz).

**A successful mount is necessary but not sufficient.** The mount handshake is short and
low-rate; marginal signal integrity can pass it and still corrupt data under sustained
reads. The real test is sustained voicefetch traffic — and that path already carries an
end-to-end integrity check, since the firmware sends a whole-file `uartCrc16` that
`fetch.py` verifies against the reassembled bytes. **A card returning corrupt data at
10 MHz surfaces as `FetchError: whole-file CRC mismatch`, not as silent corruption.**
Watch for that line, and for `[SD] Mount SUCCESS at ... Hz` naming which rung won.

### 4d. Ruled out — do not re-investigate

- **Firmware per-frame pacing.** `grep -cE "vTaskDelay|taskYIELD|uart_wait_tx_done|delay\(|\.flush\(\)"`
  over `System_UartLink.cpp` returns **0**. The 4096 B TX ring is 3.98 frames deep, so
  firmware compute overlaps the wire.
- **Host RX path (post-CRC-fix).** MEASURED at 36–44 ms *total* per transfer. For it to
  own the residue the demux loop would need to run 51× slower than measured.
- **Syscalls and asyncio.** 2.2 µs and 6.8 µs per frame respectively.
- **Larger frame payload.** MEASURED: payload 2048 with `transport.py` `_MAX_FRAME_WIRE`
  left at 1100 bricks voicefetch outright. Above 4093 B of protected data, CRC-16-CCITT
  drops from HD=4 to HD=2.

### 4e. Net result

| | before | after |
|---|---|---|
| per frame | 14.70 ms | **7.98 ms** |
| improvement | | **46%** (predicted 38%) |

Fix is Pi-only, no reflash. It beat prediction because the 189 ms whole-file CRC was
blocking the asyncio loop, so removing it bought more than its own runtime.

## 5. Device VAD

From a 7-capture session (MEASURED).

| | value |
|---|---|
| mean leading silence before latch | **0.97 s** (worst observed **2.43 s**) |
| trailing window armed | 1800 ms |
| trailing silence actually recorded | **1920 ms** — `gRecSilenceMs` steps in 128 ms units and first crosses 1800 on the 15th chunk |
| mean capture | 7.24 s |
| **silence fraction** | **38%** |

### The 768 ms rule

**Longest mid-sentence pause observed: 768 ms.** Six consecutive sub-threshold chunks
(avg dipped to 300 against a cut of 342 — missed by 42), then speech resumed at 379:

```
c31 avg=240 SIL sil=128ms ... c36 avg=300 SIL sil=768ms   c37 avg=379 snd
```

**Do not shorten `vad_silence_ms` below ~1200 ms.** A 900 ms window would have truncated
that utterance mid-sentence, and 768 ms is the worst case from only seven samples — the
true tail is longer.

The correct fix is to trim the *recorded file* without shortening the *detection window*.
Projected saving (COMPUTED): 2.13 s removed, ~67 frames, ~0.53 s transfer + ~0.78 s STT
≈ **1.31 s/exchange** on the wake path (`vad 1800`), **0.91 s** on the ask path
(`vad 1200`, `config.py:37`). Design exists; not implemented as of this record.

Note `RECORDING_CHUNK_SIZE` is **4096 bytes** = 2048 samples = 128 ms, so a 3-chunk
pre-roll is 384 ms, not 300 ms.

## 6. End-to-end exchange timing

### 6a. Baseline session — UNDER-VOLTED, treat as upper bounds

7 exchanges, Qwen3-0.6B:

| stage | mean | share |
|---|---|---|
| fetch | 10.77 s | 63.3% |
| llm+reply | 3.61 s | 21.2% |
| stt | 2.64 s | 15.5% |
| **total** | **17.03 s** | |

`stt` and the prefill half of `llm` are inflated ~38% by throttling. `fetch` is not —
it is bounded by the recording window and the firmware/SD path, both immune to Pi power.

### 6b. After the model upgrade (clean power, Qwen3-1.7B Q4_0)

5 exchanges, mean total **10.20 s** — but utterances averaged 127 frames / 4.0 s of audio
versus 242 frames / 7.65 s before, so **the totals are not comparable**.

What is comparable:

| | old (throttled 0.6B) | new (clean 1.7B) |
|---|---|---|
| llm+reply | 3.61 s | **2.84 s (−21%)** |
| stt / second of audio | 0.346 s/s | **0.285 s/s (−18%)** |
| transfer | 9.1–13.5 ms/frame | 11.3–14.1 ms/frame *(unchanged, as predicted)* |

**A 3.4× larger model runs 21% faster than the small one did**, because the old
measurement was throttled. The transfer rate not moving is an independent confirmation
that it is firmware/SD-bound, not Pi-bound.

Estimated healthy 0.6B baseline for future comparison: **~16.0 s** (ESTIMATED —
fetch 10.77 + stt ~1.92 + llm ~3.30).

### 6b-2. Full loop breakdown — 2026-08-08 16:35, the authoritative run

6 wake exchanges. Qwen3-1.7B Q4_0, trailing trim ON, SD at 10 MHz, power clean.
All MEASURED. **Mean exchange 12.68 s.**

| stage | sec | % of total |
|---|---|---|
| **FETCH** | **6.67** | **52.6%** |
| — recording (wait) | 4.50 | 35.5% |
| — UART transfer | 1.21 | 9.6% |
| — `micdelete` | 0.54 | 4.2% |
| — `micrecord stop` | 0.42 | 3.3% |
| **LLM + REPLY** | **4.92** | **38.8%** |
| — decode | 3.07 | 24.2% |
| — BLE delivery to lens | 1.06 | 8.3% |
| — prefill (ttft) | 0.79 | 6.2% |
| **STT** | **1.09** | **8.6%** |

Rates: transfer **5.11 ms/frame + ~640 ms fixed**; STT **0.311 s per s of audio**;
decode **10.3 tok/s**; mean audio kept 3.52 s after 1.54 s of trailing silence trimmed.

**`micdelete` is on the critical path** — 0.54 s/exchange spent deleting the recording
before `fetch_wake_utterance` returns, so the wearer waits on it before STT starts. It
runs in `_cleanup`'s `finally`. Confirmed firmware-side: the `voicefetch → micdelete` gap
is 0.50–0.60 s on all six exchanges. Deferring it past the reply recovers ~all of it.
Larger than prefill, half the transfer. **Not yet fixed.**

Of the 4.50 s recording, **1.92 s is the mandatory VAD trailing window** (see §5) and
~0.97 s is leading silence (recoverable only via the leading trim, which was not built).

### 6c. Cost model

Clean 0.6B LLM time = 1.75 s (prefill 0.74 + decode 1.01) at a 120-token prompt and a
29-token reply. Roughly **1.55 s of the measured `llm+reply` is BLE delivery to the
lens**, which does not scale with model size — this is why naive size-ratio scaling badly
overstates upgrade cost.

| model | LLM time | vs 0.6B | with the VAD trim, on 17.0 s |
|---|---|---|---|
| Qwen3-1.7B Q4_0 | 3.98 s | +2.23 s | **+0.92 s (+5%)** |
| Qwen3-1.7B IQ4_NL | 4.36 s | +2.61 s | +1.30 s (+8%) |
| Qwen3-1.7B Q4_K_M | 5.28 s | +3.53 s | +2.22 s (+13%) |
| Qwen3-1.7B Q5_K_M | 6.22 s | +4.47 s | +3.16 s (+19%) |

### 6d. The floor

After Q4_0 and the trim, an exchange lands near **16.6 s**, of which **~7.2 s is the
recording window** — the user speaking plus the 1.8 s the VAD needs to be confident they
stopped. That is 43% of the exchange and cannot be reduced without paying the truncation
cost documented in §5.

### 6e. 2026-08-11 — first live-STT path (Gate E)

The live-STT path (streaming Moonshine fed by the XIAO recorder-shadow) replaces the
batch capture: **`stt` recognition overlaps the wearer's speech and the voicefetch file
transfer is gone entirely** — the WAV never crosses the wire. `stt` therefore reports
**0.0 s**, and `fetch` here is the live-capture/finalize wait, a **different anchor** than
the recording-plus-transfer `fetch` of §6a–6d, so compare **totals, not stages**.

Two exchanges survived the journal window (`n=2` — data points, not a session mean):

| exchange | fetch | stt | llm+reply | first_paint | **total** |
|---|---|---|---|---|---|
| "Bright swamp arresting" (22 B, short answer) | 2.8 | 0.0 | 4.7 *(ttft 2.83)* | 3.6 | **7.5 s** |
| "Brainstorm a recipe" (19 B, long answer) | 3.8 | 0.0 | 17.0 | 7.3 | **20.8 s** |

The recipe's 17.0 s `llm+reply` is a `max_tokens`-bound *generation* (it hit the 120-token
cap mid-word), not a pipeline cost. A normal exchange lands near **7.5 s** — ≈40% under
the 12.68 s authoritative batch run (§6b-2), with `stt` (1.09 s) and the whole UART
transfer + `micdelete` (§6b-2) removed. Config: Qwen3-1.7B Q4_0, thinking off,
`g2_stream_speed 40`, `max_tokens` 120 → 250 (raised same day, uncommitted).

Firmware-cross-checked milestones (XIAO serial, same two exchanges): record (ENTER→ask)
4.9 / 5.5 s; ask→first-reply 2.8 / 6.8 s (matches host `ask_gap`); reply stream 1.1 / 9.7 s.

Caveats: `n=2`; the first wake of a session pays a cold llama-server prefill (the opener
showed ~7.5 s ttft, not tabled); `ask_gap`/ttft **climbs with accumulated history** (2.83 s
→ 6.72 s across the session as the 8-turn context filled).

**Open regression:** the G2 mic lane ran at ~14–16 fps with `lost` climbing to 691 over the
session — the half-rate / frame-loss anomaly (see
[`../cm5/LIVE_STT_TRIAGE_2026-08-10.md`](../cm5/LIVE_STT_TRIAGE_2026-08-10.md)), under
separate investigation. Degraded mic delivery is a live-STT **accuracy** risk, not a
latency one.

## 7. RAM

| | |
|---|---|
| Pi 5 total / available | 4.0 GB / 3.7 GB |
| Qwen3-0.6B Q4_K_M | service estimate 0.6 GB |
| Qwen3-1.7B Q4_0 | service estimate **1.6 GB** (file is 1.14 GiB; the preflight pads) |
| moonshine `small` (en) | 0.6 GB |
| **both engines, 1.7B** | **~2.5 GB of 3.7 GB** |

`llama-server` startup grew 3.1 s → 18.4 s loading the larger model. One-time, matters
only on daemon restarts.

## 8. Thermal

Peak **60.9 °C** through a full four-model bench sweep with `throttled=0x0`. The Pi 5
throttle point is 80–85 °C. **No active cooling needed for this workload.**

## 9. Errata — corrections made during this session

Recorded because each was believed and acted on before being caught.

| claim | correction |
|---|---|
| Pi/Mac CPython factor K = 3.05 | **MEASURED 1.91.** The first "zero residue" conclusion was wrong; a real 0.550 s residue existed and turned out to be the SD read. |
| CRC fix worth 61% | **38% predicted from clean Pi numbers; 46% measured.** The 61% rested on the unmeasured K. |
| Mean exchange 15.6 s | **17.03 s.** Plain arithmetic error; it was the denominator for several percentage claims. |
| LLM generation ~9.4 tok/s | That conflated decode with BLE reply delivery. Real decode is **27.6–28.6 tok/s**; ~1.55 s of `llm+reply` is delivery. |
| Q4_0 would improve decode | It *dropped* 19% under throttle and merely **ties** IQ4_NL when clean. The i8mm repack helps prefill (GEMM), not decode (GEMV). |
| IQ4_NL is the best quant | True only on throttled data. **Q4_0 wins clean** on prefill by 29%. |
| `mic_autostop` EVT saves ~125 ms | **Saved 0 ms in the then-current poll-first host.** The 250 ms poll could see `micRecording=false` inside the chunk loop before the EVT, which is deliberately emitted after `close()`, and immediately fall back to `micrecord stop`. Its value there was 87% fewer status polls. See the later grace-window correction below. |
| A generic ~0.8 s EVT grace fixes 4/8 misses and saves ~0.20 s/exchange | **Overstated.** Only 3/4 poll wins were VAD/EVT races; the fourth was a session EXIT, which intentionally emits no `mic_autostop`. Both paths still wait for WAV finalization, so only the redundant path-recovery command can disappear. The implemented mitigation is wake-only, 250 ms, and capped by the freshly recomputed 15 s host wait deadline; it preserves immediate manual/ask behavior and explicit-stop fallback. The optimistic ceiling from this run is <0.144 s/exchange before subtracting grace wait, so the gain must be remeasured. |
| Trailing silence is 1800 ms | **1920 ms** recorded, from 128 ms chunk quantization. |
| 3-chunk pre-roll is 300 ms | **384 ms** — `RECORDING_CHUNK_SIZE` is 4096 *bytes*. |

## 10. Code findings

Fixed this session:

- **`crc16_ccitt` → `binascii.crc_hqx`** (`link/protocol.py`), bitwise version kept as
  `_crc16_ccitt_bitwise` with a drift test in `tests/test_p2_frames.py`.
- **Auth audit was coupled to the web server.** `logAuthAttempt` / `recordLoginAttempt`
  lived in `WebServer_Server.cpp`, whose entire contents sit inside
  `#if ENABLE_HTTP_SERVER` — so a headless build silently lost login auditing on *every*
  transport. Moved to `System_Debug.cpp` (declared in `System_User.h`); guards removed at
  all call sites including `loginTransport` and the ESP-NOW bond auth-failure log.
- **`SYSEVT_LOGIN_OK` / `SYSEVT_LOGIN_FAIL` now posted for UART.** The host link handles
  `login` in-band before the registry (`System_UartLink.cpp`, "login lines never fall
  through to the registry"), so `cmd_login` never ran and no event ever fired. The
  notification side was already complete — policy `{ALL, tier 1, 2000 ms}`, rendered as
  `"Login: <user>"`.
- **Stub with transposed parameters deleted.** The `logAuthAttempt` stub in
  `System_SensorStubs.h` had `ip` and `user` swapped relative to the real signature.
  Same types, so it compiled either way. Four files re-declaring the symbol as a local
  `extern` is why it was never caught; all now use the header.

Known-open:

- `SYSEVT_LOGOUT` is posted only by the web session manager. UART logout, serial logout,
  and UART idle-expiry all pass silently.
- The lockout family (`isLoginLocked` / `recordFailedLogin` / `clearLoginAttempts`) is
  string-keyed and therefore transport-neutral by design, but still lives in
  `WebServer_Server.h` with its `LoginAttemptEntry` state.
- `authSuccessUnified` is genuinely mixed — it sets transport auth globals *and* creates
  web sessions. Splitting it is real work, not a move.
- The VAD trim is designed but unimplemented.
- The ~432 ms fixed intercept in §4c is only partly accounted (whole-file `uartCrc16`
  ~55 ms, FS open, PSRAM alloc, command round-trip). Roughly 300 ms has no owner.

## 11. Reproducing these measurements

```bash
# LLM — always confirm throttled=0x0 first
/home/caden/llama.cpp/build/bin/llama-bench -m <model.gguf> -p 128 -n 64 -t 4
```

```bash
# Transfer — regress firmware LOOPHEALTH stall duration against frame count.
# Frames come from the Pi's "voicefetch OK: N bytes, M frames" line; the stall is the
# firmware line logged immediately after the voicefetch command completes.
# NOTE the stall warning is rate-limited to 1 per 5 s (HardwareOne.cpp), so it cannot
# establish per-poll causation — only per-transfer totals.
```

Model choice is recorded in `cm5/ai-service/config.example.yaml` alongside the bench
table that justified it.
