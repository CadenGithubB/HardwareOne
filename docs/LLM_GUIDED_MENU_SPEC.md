# LLM Guided-Input Menu — Implementation Spec (v1)

Authoritative spec for the menu-driven LLM input system. Both repos implement THIS document;
where code and spec disagree, the spec wins. Derived from the 2026-07-18 design study
(11-agent recon + design + adversarial critique).

## 0. Rules of engagement

- Never `git commit` or `git push`. Leave everything in the working tree.
- Firmware repo: `/Users/morgan/esp/hardwareone-idf`. Converter/training repo: `/Users/morgan/esp/llm-converter`.
- The firmware working tree has UNCOMMITTED changes (G2 LLM viewer phase 1, G2 apps, notification work). Make targeted edits only; never revert or reformat existing code.
- All new firmware code sits inside `#if ENABLE_ONDEVICE_LLM`; web parts additionally inside `ENABLE_HTTP_SERVER`.
- No backwards compatibility or migration code anywhere. Old-format info blocks: metadata is skipped (model still loads), no shims.
- No new FreeRTOS tasks. No task stacks in PSRAM. Menu data lives in PSRAM (not secret). BLE work stays off BTC_TASK (existing deferrals only).
- New commands follow the uniform `OK:`/`Error:` return contract and the cliHint convention.
- Build board: `feathers3` (fallback `xiao_s3` if feathers3 hits the known pre-existing BuildConfig `#error`). Parallel-build gotcha: if `x509_crt_bundle.S` corrupts, regen with `ninja -j1 x509_crt_bundle.S`.

## 1. Data flow

Generator (owns ground-truth templates + entity rosters) → `menu_manifest.json` →
converter (`index.html`) auto-loads it like `*domain_vocab.txt` → MENU section inside a
restructured TLV info block v2 in the `.bin` → firmware parses at model load into one PSRAM
blob → shared `System_LLM_Menu` API → `llmmenu`/`llmask` commands + thin per-surface adapters.
Surfaces submit integer indices; composition happens once, on-device; the composed question
feeds the existing `chatBeginTurn` → `llmresult` streaming pipeline unchanged.

## 2. `menu_manifest.json` (interchange, emitted by generators)

```json
{
  "menu_version": 1,
  "groups": [
    { "name": "Pokemon",
      "templates": [ { "q": "What type is {}?", "label": "Type of {}" }, ... ],
      "entities": [ "Bulbasaur", "Ivysaur", ... ] },
    { "name": "General",
      "templates": [ { "q": "How do I catch a Pokemon?" } ],
      "entities": [] }
  ]
}
```

- `{}` marks the single entity slot; **0 or 1 per template**. A slotless template is "canned"
  (entity pick skipped, even if the group has entities).
- `label` optional short display form (target ≤20 chars for OLED); absent → UIs truncate `q`.
- `q` strings are **corpus-exact** (casing, punctuation). This is the entire point.
- Curation rule: ONE canonical phrasing per question archetype = the FIRST entry of each
  `*_Q` list in the generator. 5–15 templates per group. Do NOT ship all trained paraphrases.
- Irregular slotless corpus questions → a "General" group.
- Caps (converter enforces, firmware re-validates): ≤8 groups, ≤64 templates/group,
  ≤1024 entities/group, group name ≤32 B, `q` ≤120 B, entity ≤48 B, encoded MENU section ≤32768 B.
- Do-mode groups (`"mode": "do"`) are SUPPORTED by the format/firmware (flags bit0) but
  generators do NOT emit them in v1 — Do: stays free-text.

## 3. Info block v2 (TLV) — binary layout

The 64-byte `.bin` header is UNTOUCHED (`info_len` u32LE at offset 24, file_version stays 2).
Only the info-block CONTENT changes. Rationale: the v1 positional layout reads the domain
vocab as `infoEnd - pos` (System_LLM_Model.cpp:946), so nothing can be appended after it.

```
offset 0: u16LE sentinel = 0x4932      ; legacy v1 starts with desc_len ≤255 (high byte 0),
                                       ; so any value ≥0x0100 is unambiguous
offset 2: u8    section_count
then section_count × section:
  u8    section_id                     ; 1=DESC 2=ICON 3=DOMAIN 4=MENU; unknown id → skip len bytes
  u32LE section_len
  u8    payload[section_len]
```

Section payloads (inner encodings preserved from v1 so field parsers are reused):
- **DESC (1)**: raw UTF-8 (≤255 B by converter).
- **ICON (2)**: u8 fmt (1 = 1bpp MSB-first row-major) + u8 w + u8 h + u16LE icon_len + bitmap
  (exactly 128 B for 32×32).
- **DOMAIN (3)**: u16LE refusal_len + refusal + u16LE vocab_count + vocab_count × [u8 len][word].
- **MENU (4)**: see §4.

Firmware parse (`loadInfoBlockFromFile`, System_LLM_Model.cpp:887-963 rewritten as a TLV
walker): read sentinel — if ≠ 0x4932, wipe metadata fields and `f.seek(infoStart+infoLen)`
(model still loads; best-effort ethos preserved; the final seek at :961-962 stays in ALL paths).
Per section: bounds-check `section_len` against infoEnd; malformed section → skip it, keep walking.

## 4. MENU section payload

```
u8    menu_ver = 1                     ; ≠1 → firmware skips the section
u8    group_count (1..8)
u16LE reserved = 0
group_count × group:
  u8    flags                          ; bit0 = Do-mode; bits1-7 reserved 0
  u8    name_len + name bytes (≤32)
  u16LE tpl_count (≤64)
  u16LE ent_count (≤1024)
  tpl_count × [u8 len][UTF-8 ≤120 B]   ; slot encoded as single byte 0x1F, 0 or 1 occurrence
  ent_count × [u8 len][UTF-8 ≤48 B]
```

Firmware validation: one strict walk over the whole payload — every length-prefixed item must
land inside the section, counts within caps, ≤1 slot byte per template. Any violation → free
the blob, `menuGroupCount = 0`, one DEBUG_LLM log line. Absence of a menu is a first-class
state (all surfaces hide guided UI), never an error.

## 5. Firmware state, locking, API

### State (System_LLM_Internal.h, next to domainVocab)
```c
typedef struct { uint32_t nameOff, tplOff, entOff;
                 uint16_t tplCount, entCount; uint8_t flags, nameLen; } LLMMenuGroupDesc;
uint8_t*         menuBlob;        // PSRAM via llmPsramAlloc("llm.menu"); NULL if none
size_t           menuBytes;
uint8_t          menuGroupCount;  // 0 = no guided input
LLMMenuGroupDesc menuGroups[8];
uint16_t         menuGeneration;  // bumped on EVERY llmLoadModel AND llmUnload
```

### Locking (MANDATORY — verified race)
domainVocab is only free-safe today because its sole reader runs inside the quiesced
llmGenerate. Menu readers run on the OLED loop task, httpd, g2_tap_disp, and cmd_exec while
`llmUnload` can free the blob from another task. Therefore: a static FreeRTOS mutex
(`sMenuLock`) inside System_LLM_Menu.cpp. Every accessor takes it, checks state, COPIES the
requested item out, releases (µs hold). llmUnload/llmLoadModel take it around free/publish +
generation bump. **No caller ever receives a pointer into the blob.** A generation check
without the lock is a TOCTOU and is forbidden.

### API (new `System_LLM_Menu.cpp` + decls in System_LLM.h, ENABLE_ONDEVICE_LLM)
```c
uint16_t llmMenuGeneration(void);
uint8_t  llmMenuGroupCount(void);
typedef struct { char name[33]; uint8_t flags; uint16_t tplCount; uint16_t entCount; } LLMMenuGroupInfo;
bool llmMenuGroupInfo(uint8_t g, LLMMenuGroupInfo* out);
int  llmMenuTemplate(uint8_t g, uint16_t t, char* buf, size_t cap, bool* hasSlot); // display form: 0x1F → "{}"
int  llmMenuEntity  (uint8_t g, uint16_t e, char* buf, size_t cap);
int  llmMenuCompose (uint8_t g, uint16_t t, int e, char* buf, size_t cap); // e = -1 for slotless
int  llmMenuAsk(uint16_t gen, uint8_t g, uint16_t t, int e, const ChatParamOverride* ov);
     // >0 = chat session id; 0 = busy (generation in flight); -1 = stale generation;
     // -2 = bad index; -3 = no menu
int  llmMenuRepeatLast(void);          // re-ask last guided question PLAIN (no suppress); same returns
bool llmMenuLastAskInfo(int* sessionOut); // true if a guided last-ask exists; its chat session id
```
- `llmMenuAsk`: gen check → compose (≤ 4+120+48 B, well under the 1024 B prompt cap) →
  `chatBeginTurn(composed, ov)`. Do-mode groups (flags bit0): prefix `"do: "` so
  `llmFramePrompt` emits the Do: scaffold, and force `hardCap=4, sentenceLimit=0` in the
  override (mirrors the web Do: path). Menu-fired Do: results are SUGGESTIONS only — never
  auto-executed anywhere.
- `llmMenuRepeatLast`: the retry fix. `chatRetryLast` suppresses the previous answer's tokens —
  correct for free-text rambling, WRONG for guided asks (it bans the memorized-correct answer).
  The module stores the last composed question (protected by sMenuLock) + returned session id.
  Surfaces offering retry MUST branch: last turn was guided (`llmMenuLastAskInfo` session
  matches the latest) → `llmMenuRepeatLast()`; else → `chatRetryLast()`.
- Lifecycle: parse/publish in `llmLoadModel` path after weights load; free+zero+bump in
  `llmUnload` next to the domainVocab free (System_LLM.cpp:1053-1062).

## 6. Commands (append to llmCommands[], System_LLM.cpp:2956-2985)

All NON-admin (llmgenerate-family precedent). All lead with `OK:`/`Error:`. JSON replies use
the `{"schema":1,...}` envelope. Page sizes chosen so every reply ≤ ~3 KB (under the 4096 B
ExecReq.out cap and the 4095 B /api/cli cap). Use `argLeadingTokenIsJson`, not `argWantsJson`.

- `llmmenu` — human status: menu present/absent, gen, group list with counts.
- `llmmenu json` → `{"schema":1,"gen":G,"groups":[{"i":0,"name":"Pokemon","mode":"ask","templates":12,"entities":151},...]}`
- `llmmenu json tpl <g> <off>` → `{"schema":1,"gen":G,"g":0,"off":0,"total":12,"items":["What type is {}?",...]}`
- `llmmenu json ent <g> <off>` → same shape with entity strings.
- `llmask <g> <t> [e]` (human) / `llmask json <gen> <g> <t> [e]` →
  `{"schema":1,"ok":true,"session":N,"q":"What type is Pikachu?"}`.
  Stale gen → `Error: menu changed - refetch (llmmenu json)`. Busy → `Error: busy - answer in progress`.
  cliHint on success → `llmresult json 0`.
- `llmask repeat` → re-ask last guided question via llmMenuRepeatLast.
- `llmstatus json` gains `"menu":{"groups":N,"gen":G}`.

BLE contract note (docs only, no firmware change): menu paging replies exceed the 514 B
plaintext single-notify limit — guided-menu commands are Secure-Channel-only; document next to
docs/LLM_BLE_GENERATE_OVERRIDES.md.

## 7. Web (WebPage_LLM.cpp / WebPage_LLM.h)

- New `GET /api/llm/menu?kind=groups|tpl|ent&g=<g>&off=<off>` in registerLLMHandlers
  (WEB_AUTH_OR_RETURN; same JSON shapes as `llmmenu json`; dedicated handler avoids the
  /api/cli 4095 B cap; /api/llm/models is the precedent).
- UI: a "Guided ask" strip above the existing input — selects `#qa-group` → `#qa-tpl` →
  `#qa-ent` (entity select hidden for slotless), filled via `hw.fetchJSON` with the
  `#qa-model` option-fill pattern (WebPage_LLM.h:309-329). Picking writes the COMPOSED question
  into the existing free-text input (still editable); the untouched `qaAsk()` submits. Strip
  hides when `/api/llm/status` reports no menu; refetch on model load and on gen change.
- Web retry on a guided ask: the page knows it composed the prompt — Retry re-asks the same
  composed question WITHOUT the suppress[] list (client-side branch in the existing retry JS).

## 8. OLED (OLED_Mode_LLM.cpp)

- New sub-states `PICK_GROUP` → `PICK_TEMPLATE` → `PICK_ENTITY` in the existing state machine
  (:58-63), each one `OLEDScrollState` (per-mode pattern; the generic modal picker was
  deliberately removed — do not recreate it).
- Entry: **X** in READY when `llmMenuGroupCount()>0`. X currently ALIASES A at
  OLED_Mode_LLM.cpp:435 and :469 — remove the X alias at those two sites. A = keyboard
  (unchanged), Y = retry, B = back one level (ENT→TPL→GROUP→READY).
- Lists: groups ≤8 and templates ≤15 fit directly; entities (≤1024, typically 151) use a
  windowed page of ≤29 rows + `< Prev` / `Next >` edge rows refilled via the C API.
  Rows truncate to 21 chars (display only — selection is by index); slot renders as `_`.
- Submit: `llmMenuAsk(gen,...)` → GENERATING view (existing). Return 0 (busy) → status line
  "Busy: answer in progress"; -1 (stale) → pop to READY.
- Y retry branch: guided-last → `llmMenuRepeatLast()`, else `chatRetryLast()` (§5).

## 9. G2 glasses (G2_Glasses.cpp)

- Entry: Apps → LLM currently opens the viewer directly (APP_ROW_LLM). Change to a small
  submenu list page: `[<- Back, Open chat, Ask (guided), Re-run last, Select Model]`;
  "Ask (guided)" hidden when no menu. Select Model label becomes `Model: <basename>` when
  weights are loaded. Picker pages run as ORDINARY hijack list pages BEFORE the viewer
  worker spawns (never from inside the running viewer).
- Flow: groups page → templates page (G2Paginator chrome if >10 rows) → entities: if
  ent_count > 40, insert an ALPHA-BUCKET level ("A-D", "E-H", … ≤8 rows) so any of 151
  entities is reachable in 2 taps → entity page → terminal tap runs
  `g2SubmitHijackCommand("llmask json <gen> <g> <t> <e>", cookie, cb)` under the paired user's
  auth, then `g2ShowLlmPage()` streams the answer (existing viewer, unchanged).
- Select Model: list page `[<- Back, Unload model, <*.bin>…]` (paginated; scans `/system/llm`
  + `/sd/llm`). Unload → `llmunload`; model tap → `llmload <full-path>`. Both via
  `g2SubmitHijackCommand` (admin-gated like CLI/web); never call `llmLoadModel` on the tap task.
  Wait page "Loading model…" / "Unloading…"; success returns to the submenu; admin denial →
  "Admin required".
- Every tap path stamps `gHijackStartedMs` (framework already does on the 'app' container
  path — verify each new page uses it). NEVER feed the watchdog from acks/pushes.
- Watchdog vs deliberation: if the 60 s timeout fires mid-pick, picker state (group/template/
  bucket indices, cached gen) lives in file statics so re-opening Apps→LLM→Ask RESUMES at the
  last level. Keep pages small so each decision is quick.
- 'Re-run last' row: guided-last → `llmMenuRepeatLast()`, else `chatRetryLast()` (§5).
- Busy/stale from llmask: brief list page "Busy - try again" / "Menu changed - reopen", then
  back to the submenu. All tap work stays on g2_tap_disp / cmd_exec — nothing inline on BTC_TASK.

## 10. Converter (`index.html` + tokenizer.js untouched)

- Auto-load `*menu_manifest.json` from the dropped model folder (clone of the domain_vocab
  auto-load at :1539-1553, including clear-on-new-drop hygiene at :1451-1454); manual file
  picker override; UI status line "Menu: 3 groups / 34 templates / 256 entities / 6.2 KB".
- `buildOutputBin` rewritten to emit info block v2 (§3): always TLV when ANY metadata exists
  (desc, icon, domain, menu). `{}` → 0x1F. Enforce all §2 caps with clear errors.
- Domain-vocab merge: after loading the manifest, tokenize all entity strings and template
  content words (same rule as the trainer: lowercase, split `[a-z0-9]+`, drop all-digit and
  len<3) and UNION them into vocabWords before writing DOMAIN — menu-visible entities must
  never refuse when typed free-text. One vocabulary, not two drifting ones.
- Lints at convert time: template with >1 slot → error; any string over caps → error; sample
  composition (template + longest entity) >100 chars → warn (seq_len headroom); entity not
  found in tokenizer vocab as whole-word special token → warn (quality signal, not fatal).

## 11. Generators (`/Users/morgan/esp/llm-converter/Training/training_scripts/` + kit)

- `corpus_lib.py`: menu bookkeeping (`menu_group(name)`, template/entity registration,
  `write_menu(out_path)`), reusable by all generators and TEMPLATE.py.
- `generate_pokemon_data.py` / `generate_elements_data.py` / `generate_pop_culture_data.py`:
  emit `menu_manifest.json` next to their corpus output. Canonical phrasing = FIRST entry of
  each `*_Q` list. Suggested groups — pokemon: Pokemon / By number / Types / Items / General;
  elements: Elements / By symbol / By number / Families / General; pop-culture: People /
  Categories / Places / General. Manifest strings byte-exact vs corpus lines.
- **Corpus text output must remain byte-identical** — checksum the existing corpus files
  before running, verify after (generators are seed-deterministic). If a generator can't run
  without rewriting, add a manifest-only path.
- `build_your_own_model/TEMPLATE.py` + `corpus_lib.py` kit: emit the manifest from
  ATTRIBUTE_QUESTIONS/ENTITIES so every future kit model ships a menu for free;
  note in BUILD_YOUR_OWN_MODEL.md.
- hardwareone corpus: NO menu (not template-shaped; Do: stays free-text).
- `train_tiny_model.py` (+ GPU variant if trivially parallel): ~10-line pass-through — if a
  `menu_manifest.json` sits next to the corpus file (or `--menu <path>` is given), copy it
  verbatim into out_dir next to `domain_vocab.txt`, so the converter's drop-one-folder
  auto-load finds it. No other trainer changes.

## 12. Critic checklist (verify before declaring done)

1. sMenuLock copy-out discipline everywhere; no interior pointers escape. (use-after-free)
2. `llmask` REQUIRES gen on the json form; human form reads current gen but still errors
   cleanly if a swap raced. (wrong-model composition)
3. Guided retry never goes through chatRetryLast. (suppresses the correct answer)
4. Composed questions survive the prompt normalizer (title-case + vocab-aware recasing,
   System_LLM.cpp:1342-1402) — templates start capitalized, entities corpus-cased; add a code
   comment at FILLER_PREFIXES tying it to the menu contract (re-enabling stripping would break
   menu templates like "Tell me about …").
5. Manifest words merged into domain vocab at convert time. (menu-visible ≠ gate-refused)
6. Busy (chatBeginTurn → 0) has explicit UX on every surface.
7. OLED X-alias removed at :435/:469; A/Y/B bindings unchanged.
8. G2: pickers before viewer worker; watchdog stamped on taps only; picker state survives
   force-exit; nothing heavy on BTC_TASK; UPDATE_TEXT paths untouched.
9. Do-mode: format+firmware support with hardCap=4/sentenceLimit=0, never auto-run; no
   generator emits it in v1.
10. Commands non-admin, OK:/Error: contract, cliHint chain (llmmenu → llmask → llmresult json 0).
11. All firmware code gated ENABLE_ONDEVICE_LLM (+ ENABLE_HTTP_SERVER for web).
12. llmSettingEntries is APPEND-ONLY by index — this feature adds NO settings; do not touch
    that table.

## 13. Out of scope (v1)

Statistical extractor for generator-less corpora; Do-mode menu content; multi-slot templates
(format reserves nothing extra — TLV + menu_ver covers future revs); validity bitmasks
(density ~100%); sticky-entity follow-up rows (v1.5 candidate); companion-app native pickers
(app-side work).
