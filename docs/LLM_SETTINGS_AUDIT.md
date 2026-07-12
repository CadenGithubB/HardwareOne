# LLM Settings & Control-Surface Audit — 2026-07-10

**Status: AUDIT ONLY — nothing implemented.** 5-auditor sweep (table-vs-reality,
hardcoded knobs, command surface, cross-surface drift, param plumbing) +
verification synthesis. Every finding re-checked against the code; file:line
throughout. 37 findings: 16 QUICK / 13 REAL / 2 STRUCTURAL / 6 ASK-USER.

**The headline:** the reason the system "feels outdated" is that on the two
main surfaces, the settings mostly *don't do anything*:
- The **web chat page force-sends 5 hardcoded params on every message**
  (temperature, top_p:0.8, sentence_limit, rep_penalty, rep_window:32 literals
  in WebPage_LLM.h:344-355), so `llmtemperature`/`llmtopp`/`llmsentencelimit`/
  `llmreppenalty`/`llmrepwindow` are silently ignored by the primary surface.
- The **bare CLI `llmgenerate` calls the engine with compile-time defaults**
  (System_LLM.cpp:2328-2331: hardcoded 128 tokens + LLM_DEFAULT_TEMPERATURE,
  12 params left on positional defaults) — tuning a setting then testing over
  serial shows no effect. (Perversely the only settings it DOES honor are the
  two newest, which read gSettings directly.)
- **`llmmaxcontext` is consumed by nothing a user can reach** — its one
  consumer (WebPage_LLM.cpp:127) is overridden by a hardcoded `max_ctx:64` in
  the web Load button (WebPage_LLM.h:618), so **every web-loaded model runs at
  64-token context, half the model's 128**, while CLI/OLED loads get full ctx.
- **Bare `llmload` ignores `llmdefaultmodel`** and hardcodes model.bin
  (System_LLM.cpp:2218); the setting only affects boot autostart.
- **OLED sends raw unframed prompts** (OLED_Mode_LLM.cpp:368) while web wraps
  in the trained `Q: …\nA:` framing — OLED questions are systematically
  off-distribution.

---

## QUICK (16) — truth fixes, no behavior change

| # | Fix | Where |
|---|---|---|
| Q1 | INT8-KV comment says "not yet implemented" — feature fully ships; rewrite (FP32/FP16-default/INT8+scales). Also stale "capped 1-256" repBuf comment | System_LLM_Internal.h:99-102, :233 |
| Q2 | llmmaxcontext truth package: label "0=model seq_len; reduce-only; reload to apply", honest usage bound, delete dead LLM_MAX_CONTEXT_LEN macro (2 defs, 0 consumers), fix three "0=compile-time default" comments, align web clamp | System_LLM.cpp:2499/:2568, System_LLM.h:35/:122, System_BuildConfig.h:230, WebPage_LLM.cpp:127-129, System_Settings.h:968 |
| Q3 | temperature & mirostatEta declare min=max=0 → NO validation; "llmtemperature 99" persists+displays 99, silently runs 2.0. Set real bounds {0,2}/{0,1} | System_LLM.cpp:2492, :2502 |
| Q4 | Same knob, different legal ranges per door: repPenalty 1-3(table)/1-5(resolver)/5(web); mirostatTau 1-10 vs 0.5-20; sentenceLimit 0-20 vs web max 10; repWindow table min 1 vs engine 0=off (lower table min to 0). Align to table + cross-ref comments | System_LLMChat.cpp:265-272, System_LLM.cpp:2495-2501, WebPage_LLM.h:180-181 |
| Q5 | llmnorepeatngram usage claims "default 3" — shipped default 0; value 1 is silent no-op. Usage → "<0|2-8> (0=off default; 3 typical)" | System_LLM.cpp:2576 |
| Q6 | Generation-loop comment sweep: boost comment says "first 10" (window is 16, late boost now permanent); hoist taper literals 0.8/0.75 to named constants; note top-p taper INERT when minP>0; document CONF_GATE_TOKENS=6 ≈ 4s first-byte; coupling contracts (DO_STOP_WORDS↔training, tok<10↔ID layout, dynTemp pivot 6.0↔checkpoint, YIELD_INTERVAL WDT) | System_LLM.cpp:1679-1684, :1742, :1775, :1403 |
| Q7 | Four JSON comments document `"v":1`; code emits `"schema":1` | System_LLM.cpp:2170/:2255/:2348/:2433 |
| Q8 | Module help blurb: bare llmgenerate is NOT async (only json form is); missing norepeatngram/confthreshold from setter list; maxcontext claim wrong until R2 | System_Utils.cpp:3048-3057 |
| Q9 | WebPage_LLM.cpp header: "chunked" claim wrong, lists 6 of 10 endpoints | WebPage_LLM.cpp:4-11 |
| Q10 | OLED help says "/llmload" — no slash commands exist; quote per hint convention | OLED_Mode_LLM.cpp:258 |
| Q11 | llmstatus embeds "Error: <stale msg>" inside an OK:-stamped reply; errorMsg never cleared on successful load → READY status carries stale failures. Rename "LastError:" or gate on state==ERROR | System_LLM.cpp:2209-2210, :904 |
| Q12 | llmGenerate header doc describes dynTemp scaling as always-on; it defaults off | System_LLM.h:141 |
| Q13 | docs/LLM_FIRMWARE_ARCHITECTURE.md: confidence no longer "pure observability" (gate ships armed); "tunable in three places" recipe stale; refresh §5-6 | docs/LLM_FIRMWARE_ARCHITECTURE.md:157-162 |
| Q14 | Single-source ring size LLM_REP_WINDOW_MAX(=32) across allocator/table/clamp; delete dead `>256` branch | System_LLM_Model.cpp:762-766, System_LLM.cpp:2498/:1333 |
| Q15 | Bool rows: autoStart declares 0/1, mirostat2+dynTemp declare 0/0 (both no-ops) — pick one style, comment it | System_LLM.cpp:2500/:2503/:2511 |
| Q16 | Web Do:-strip regex drifted from engine DO_STOP_WORDS ("this" missing); comment both as mirrored copies | WebPage_LLM.h:433-434, System_LLM.cpp:1850-1854 |

## REAL (13) — behavior fixes

| # | Fix | Where |
|---|---|---|
| R1 | **Sync CLI llmgenerate ignores every saved setting** (hardcoded 128 + compile defaults; turn also invisible to llmturns/llmretry). Route through chat layer (chatBeginTurn + sync poll) or minimally chatResolveParams | System_LLM.cpp:2328-2331 |
| R2 | **Honor llmMaxContext in llmLoadModel chokepoint** (maxCtx==0 → gSettings) — fixes CLI/boot/OLED; delete web `max_ctx:64` literal (every web-loaded model runs half context today) | System_LLM.cpp:777, WebPage_LLM.h:618, OLED_Mode_LLM.cpp:152 |
| R3 | **Bare llmload → gSettings.llmDefaultModel**, not hardcoded model.bin | System_LLM.cpp:2218 |
| R4 | llmretry success JSON missing `"ok":true` — app's start-validation key; shape-match llmgenerate json | System_LLM.cpp:2417-2420 |
| R5 | Failed sync generation returns "Generation error" without Error: prefix → audit-logged success + "OK: Generation error" stamp. Fix to "Error: generation failed: %s" | System_LLM.cpp:2334 |
| R6 | Busy engine reports "no model loaded" (llmIsReady is state==READY only) — branch on state; add cliHints (busy→llmresult/llmstop; unloaded→llmmodels/llmload; kvprec set→reload) | System_LLM.cpp:2318/:2298/:2412 |
| R7 | **Web chat force-overrides 5 saved settings** with hardcoded values every message — send only user-edited keys; seed Advanced inputs from device settings | WebPage_LLM.h:344-355, :179-181 |
| R8 | Web Retry uses legacy client path (server ignores its suppress field → identical retries, duplicate USER turn); registered /chat/retry endpoint has zero JS refs. Point button at it; delete client suppress plumbing + server shim per no-backwards-compat | WebPage_LLM.h:357/:468-507, WebPage_LLM.cpp:230-236 |
| R9 | **Frame prompts once in chatBeginTurn** (`Q: …\nA:` when unmarked); delete web JS wrapping; fixes OLED unframed prompts | System_LLMChat.cpp:285-312, WebPage_LLM.h:594, OLED_Mode_LLM.cpp:368 |
| R10 | Setting parse hardening (all modules): atoi/strtof accept garbage → "llmtopp abc" persists 0.0; "llmautostart on" saves FALSE. strtol/strtof+endptr, bool accepts on/off/yes/no, else Error: | System_Settings.cpp:2413/:2437/:2452 |
| R11 | Status can't answer "what's actually running": add in-effect kvPrecision to LLMStatus+JSON (configured vs effective diverge until reload), config sub-object (dim/layers/vocab/seq), meanLogprob to web status, fix INT4_MIXED rendered as FP32 by two-way ternaries | System_LLM.h:93-107, System_LLM.cpp:2177-2193, WebPage_LLM.cpp:77-88 |
| R12 | **New `llmparams` command** (+json): dump all 18 persisted values + chatResolveParams-resolved effective values. Reading config today = 18 no-arg calls; effective values visible nowhere; solves discoverability of new knobs | System_LLM.cpp:2548 |
| R13 | Web UI never fetches /chat/turns on load (comment claims it does) and only BLE can clear the conversation — add history fetch + New Chat button; OLED clear gesture | WebPage_LLM.cpp:306-310, WebPage_LLM.h |

## STRUCTURAL (2)

- **S1 — Replace the 16-positional llmGenerate with LLMGenParams struct.** The
  defaulted positional tail is what *caused* R1 (call with 2 args → silently
  compile-time behavior for 12 more), and the two newest knobs bypassed the
  struct because extending 16 positionals is painful. Fold noRepeatNgram/
  confThreshold/minP-override into LLMGenParams + ChatParamOverride + resolver
  clamps. (System_LLM.h:148-187, System_LLM.cpp:176-181/:890-896,
  System_LLMChat.cpp:246)
- **S2 — Naming harmonization** (five names for top-p: topP/llmTopP/topp/
  top_p/llmtopp; mirostat2 vs use_mirostat2; llmkvprec the only abbreviated
  cmdKey). One breaking sweep, mechanical derivation per layer. Low urgency —
  bundle with S1 or skip.

## ASK-USER (6) — owner decisions

1. **Mirostat trio (idx 8-10 + 3 commands + web keys + sampler path):** remove
   end-to-end (requires renumbering LLM_SETTING_CMD 11-17 in one verified
   commit — the one sanctioned append-only violation) or deprecate-in-place?
   Rejected feature, presented neutrally, and enabling it silently disarms the
   armed-by-default confidence gate.
2. **Promote CONTENT_LOGIT_BOOST/_LATE to a runtime setting?** Strongest
   new-knob case: two recompile-retunes already ("(was 0.5)"/"(was 10)"),
   model-dependent, co-tunes with repPenalty/noRepeatNgram which ARE settings,
   and it's the proven verbatim-loop stabilizer. Window (16) stays constant.
3. **Flip shipped defaults to operating knowledge?** sentenceLimit 2→1 (the
   known daily-driver rec; three places in one commit) and the topP 0.8 +
   minP 0 posture if min-p becomes the recommendation.
4. **Delete the HardwareOneHelpAgent filename sniff** (strstr → forced
   maxCtx=45, clamps even explicit user values; belongs in llmmaxcontext once
   R2 lands)? (System_LLM.cpp:779-781)
5. **BLE per-message overrides:** web accepts 11 override keys, BLE zero. A
   future app-side Do: mode needs hard_cap=4+sentence_limit=0 and can't say it.
   Extend llmgenerate json, or document BLE as settings-only? (Free-ish after S1.)
6. **dynTemp:** verified fully plumbed and reachable (NOT a zombie), default
   off, documented as worse for recall models, pivot calibrated to this
   checkpoint. Keep as dormant experiment or delete alongside mirostat call?
