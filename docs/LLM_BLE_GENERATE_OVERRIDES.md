# LLM Per-Message Generation Overrides — App Integration Spec

**Audience:** the Android companion-app developer/AI.
**Firmware side:** implemented 2026-07-10 (this repo). The device now accepts
optional per-message sampling overrides on the `llmgenerate json` command. This
doc is the wire contract to implement the app side against.

## What this enables

Until now the app could only start a generation with the device's *persisted*
settings — no way to tune a single reply. Now the app can pass per-message
overrides (e.g. a short, deterministic answer for one prompt) without mutating
the saved defaults. Anything the app omits falls back to the device settings.

## The command

The app already starts generation by sending the CLI command:

```
llmgenerate json <prompt text>
```

That **still works unchanged** (raw text = no overrides). To pass overrides,
send a JSON **object** instead of raw text:

```
llmgenerate json {"prompt":"What type is Pikachu?","params":{"temperature":0.2,"hard_cap":40}}
```

Detection is by first non-space character after `json `: `{` → structured form,
anything else → raw prompt. Keep sending whichever you like per message.

**Constraint:** because a leading `{` selects the structured form, a raw prompt
that itself begins with `{` (rare in natural language) would be parsed as JSON
and rejected. If a prompt can start with `{`, always use the object form and
put the text in `"prompt"`. Simplest app rule: always use the object form.

### Response (unchanged)

Success returns immediately (generation runs async):

```json
{"schema":1,"ok":true,"session":42,"hint":"the reply streams in asynchronously - read it with 'llmresult json 0'"}
```

Validate the start by the `ok` field, then poll `llmresult json 0` exactly as
today. Error shape:

```json
{"schema":1,"ok":false,"error":"invalid JSON payload"}
```

Possible errors: `model not ready`, `empty prompt`, `invalid JSON payload`,
`busy or failed to start`.

## The `params` object

All keys optional. Omitted → device setting. Values are clamped on-device to the
ranges below (out-of-range is clamped, not rejected).

| Key | Type | Range (clamp) | Meaning | Device setting it overrides |
|---|---|---|---|---|
| `max_tokens` | int | 1–512 | Max tokens generated | `llmmaxtokens` |
| `temperature` | float | 0.0–2.0 | Sampling temperature (0 = greedy) | `llmtemperature` |
| `top_p` | float | 0.01–1.0 | Nucleus sampling threshold | `llmtopp` |
| `min_p` | float | 0.0–1.0 | Min-p floor (0 = off → use top_p) | `llmminp` |
| `rep_penalty` | float | 1.0–5.0 | Repetition penalty (1.0 = off) | `llmreppenalty` |
| `rep_window` | int | 0–32 | Rep-penalty look-back (0 = off) | `llmrepwindow` |
| `sentence_limit` | int | 0–20 | Stop after N sentences (0 = off) | `llmsentencelimit` |
| `hard_cap` | int | 0–512 | Hard token cap (0 = off) | `llmhardcap` |

**Not per-message overridable** (device-wide only, set via their `llm*`
commands): `kvPrecision` (load-time), `maxContext` (load-time), `noRepeatNgram`,
`confThreshold`, `contentBoost`. These read directly from device settings each
generation; if the app needs to change them it sends the corresponding setter
command (e.g. `llmconfthreshold 0`) — they persist until changed.

## Recommended app presets

These map to how the device is tuned; use them as buttons/modes:

- **Factual (default):** send nothing — device defaults (temp 0.5, top_p 0.8,
  sentence_limit 2) already target fact recall.
- **Terse/one-liner:** `{"sentence_limit":1,"hard_cap":40}` — first correct
  sentence only.
- **Deterministic:** `{"temperature":0.15}` — near-greedy, best for "what
  type is X" style lookups where you want the single most-likely answer.
- **Command (Do:) mode:** the app builds the prompt ending in the Do: marker
  and sends `{"hard_cap":8,"sentence_limit":0}` for a bare command with no
  explanation. (The device already stops Do: output early, but the caps are a
  belt-and-suspenders.)

## Contract stability

- The override key names are the **same** ones the web chat page uses (single
  shared parser in firmware: `chatParamOverrideFromJson`), so the two surfaces
  can't drift.
- `schema:1` in every response — bump signals a breaking response change.
- The device owns clamping; the app should still range-check for good UX but
  need not enforce.

## What changed on the firmware side (context, not app work)

- `ChatParamOverride` lost its mirostat/dynTemp fields (those features were
  removed) and gained `minP`.
- The `llmgenerate json` handler now detects the `{...}` object form and parses
  `prompt` + `params` via the shared helper. Raw-text form is untouched.
- Reference: `components/hardwareone/System_LLM.cpp` (cmd_llm_generate json
  path), `System_LLMChat.cpp` (`chatParamOverrideFromJson`).
