# G2 Keyboard Improvement — Design Study

Status: **historical design study.** Produced 2026-07-27 from a multi-agent
exploration (5 code/doc deep-dives, 6 competing designs, 6 adversarial
critiques) plus direct verification against the tree as it existed then. The
original analysis is retained below as design history; its unmarked
descriptions of the "current" keyboard are not an as-built spec.

## Implementation update — 2026-08-21

One load-bearing input assumption in the study was resolved differently on
hardware than an earlier update claimed. On the captured production keyboard,
an arrow activation is an indexed `List_ItemEvent CLICK(0)`, but a ring
double-tap arrives separately as rowless `SysEvent DOUBLE_CLICK(3) src=2`.
There is no indexed `List_ItemEvent DOUBLE_CLICK` in that capture. The SysEvent
contains neither a List container nor `CurrentSelectItemIndex`, so firmware
cannot tell the host which control row was focused.

The production arrow-pad keyboard now has seven navigation rows:
`X Cancel`, `Done`, `Up`, `Left`, `Right`, `Down`, and `Mic / Keys`. The
dedicated `Select` row was removed. A single tap on an arrow moves the QWERTY
cursor. While the active pad owns the current presentation, a rowless ring
double-tap selects the currently highlighted grid key on the tap-dispatch
worker. The control owner scopes that gesture with an atomically published
presentation epoch and consumes it before generic back/exit handling; the
worker revalidates the live pad. Double-tap is a no-op on the Mic status page.
Because the event is rowless, the host cannot strictly prove that focus was on
one of the four arrows rather than `Cancel`, `Done`, or `Mic / Keys`.

This implemented mapping supersedes the study's proposed double-tap-as-Shift
and double-tap-as-Done shortcuts below; those paragraphs remain to preserve the
alternatives that were considered.

---

## 1. TL;DR

The keyboard is clunky for one mechanical reason and one layout reason:
**every keystroke REBUILDs the whole list, which resets the lens's native
highlight to row 0** (so the user re-scrolls from the top for *every*
character), and **the group selector is forward-only** (reaching N-Z from a-m
costs 3 group taps, each itself paying the reset).

The single biggest lever: **stop rebuilding the char list.** Convert the
keyboard to the HW-proven mic-detail pattern — a compound of one static,
event-capturing List child + one display-only Text child holding
`<prompt>: <buf>_` — and update **only the buffer** via fire-and-forget
`UPDATE_TEXT` (Cmd=5) on each keystroke. Every primitive already ships
(`sendCreateMixedListMultiTextAndWait`, `sendUpdateTextNamed`); one ~1-hour
HW probe decides how much it pays off (see §6 Q1).

Independent of that bet, a set of **feasible-today, zero-HW-risk wins** should
ship first: per-field charset modes (a digits-only pad for numeric settings is
the highest-ROI single change), pick-lists instead of typing for enumerable
fields (usernames — SSIDs already work this way), bidirectional group
navigation, and three latent-bug fixes the study surfaced (§7).

Voice dictation is **dead** — not deferred, dead (§5).

---

## 2. Why it's clunky today — the mechanics

Historical layout when this study was written (the source has since changed):

```
row 0  X Cancel          (auto-prepended back row)
row 1  <prompt>: <buf>_  (tap = no-op)
row 2  Space
row 3  Backspace
row 4  Done
row 5  Group: <name>     (FORWARD-ONLY cycle through 6 groups)
row 6..18  13 chars of the current group
```

Three compounding problems:

1. **Highlight reset on every keystroke.** The keyboard runs on
   `g2StartLiveListPage`; every tap kicks a full `sendRebuildListAndWait`
   (Cmd=7 REBUILD-list). G2_PROTOCOL.md is explicit: *"Cursor still resets to
   row 0; there is no schema field to preserve selection."* So each character
   starts navigation from the top.
2. **Forward-only group cycle.** 6 groups (a-m, n-z, A-M, N-Z, 0-9._-, sym),
   advance-only with wrap. Worst-case group reach = 5 taps of row 5, each one
   also a REBUILD → reset.
3. **One-size-fits-all charset.** All 10 call sites get all 6 groups, even
   pure-numeric settings fields.

**Worked example — typing capital `W`** (group 0 active): 3 taps of the Group
row to reach N-Z (each tap: scroll 0→5, click, reset), then scroll 0→15 to
`W`'s row. ≈ **30 native scroll steps + 4 clicks for one letter.** An 8-char
mixed password (`Xk9$mQp2`) costs ≈ **213 scroll + 31 clicks** — which is why
long PSKs punt to the web UI.

*(All scroll counts here and below assume 1 ring detent / touchpad step = 1
row and no list wrap; both are unverified — see §6 Q3. Relative comparisons
hold regardless.)*

---

## 3. What the hardware actually allows

The bounded input/render model (all verified in code + G2_PROTOCOL.md +
community RE proto):

**Proven facts:**

- Inside our hijacked List widget, a normal selection produces
  `List_ItemEvent CLICK` with the committed row index
  (`CurrentSelectItemIndex`) and, when populated, the row label
  (`CurrentSelectItemName`). On the production pad, a CLICK on `Up`, `Left`,
  `Right`, or `Down` moves the grid cursor. List events still provide no swipe
  direction, timing stream, or coordinates; the lens firmware owns
  scroll/highlight navigation.
- On the captured production keyboard, ring double-tap is **not** an indexed
  List event. It is `SysEvent DOUBLE_CLICK(3) src=2`, with no row or container.
  The active pad claims it through an atomic presentation epoch and selects the
  current grid key on the tap-dispatch worker; the Mic page makes it a no-op.
  This supports the intended "double-tap while focused on an arrow" workflow,
  but the wire event cannot strictly prove arrow focus or distinguish another
  control row. Do not extrapolate either channel's behavior to every List/Text
  compound without direct evidence.
- There is **no native text-input/IME widget**. The entire widget vocabulary
  is List / Text / Image (EvenHub.proto). Text and Sys events carry **no
  cell index**, so direct 2D hit-testing is impossible. A rendered grid can
  still be controlled indirectly, as the production arrow pad now demonstrates.
- Lists cap at **20 items** (community RE `itemCount` 1–20; our 19-row
  keyboard sits at the edge). Item labels ≤64 chars. A flat ~90-char list is
  impossible.
- **REBUILD-list always resets the cursor to row 0** (no preserve-selection
  field exists).
- **HW-verified (2026-04-30, fw 2.2.0.24): a compound's text child can be
  patched while the List child keeps rows/focus/scroll/event-capture.**
  Two mechanisms: single-child REBUILD-text (Cmd=7, f1=1, acked) and
  `UPDATE_TEXT` (Cmd=5, fire-and-forget, single-fragment, explicitly does NOT
  blank siblings). The shipping mic-detail page uses UPDATE_TEXT every tick;
  its comment: *"List-row selection stays put across ticks because
  UPDATE_TEXT doesn't touch the list child at all."*
- Safe compound shape is exactly **1 List + 1 Text**. Multi-child REBUILD has
  *replace* semantics (unmentioned children go dark). Never UPDATE_TEXT the
  List container itself (2026-04-24 plugin-kill incident). `eventCapture=1`
  on a TextObject is firmware-rejected → the buffer preview can never be
  tappable.
- Latency: REBUILD-list ≈ 70–80 ms + ack wait; SHUTDOWN+CREATE ≈ 600 ms;
  UPDATE_TEXT is fire-and-forget (no ack RTT) — the fastest per-keystroke
  path available.

**The one open question that gates the big win (§6 Q1):** after the user
CLICKs a row on a *static, never-rebuilt* list, does the native highlight
*stay on that row* (making the next nearby char cheap)? Mic-detail proves the
list isn't disturbed by UPDATE_TEXT on a 1 Hz timer; nobody has probed
highlight position *immediately after a CLICK* during rapid typing.

---

## 4. Recommended layered plan

### Phase 0 — correctness fixes (S, independent, no redesign needed)

| Fix | Detail |
|---|---|
| **Secret leak to debug logs** | `finishCancel`/`finishCommit` log the full buffer (`DEBUG_G2F(... buf='%s')`, [G2_Page_TextEntry.cpp:196,205](../components/hardwareone/G2_Page_TextEntry.cpp)) — passwords in plaintext whenever the G2 debug flag is on. Add `bool isSecret` to `TextEntryConfig`; suppress buffer contents in logs when set. WiFi PSK / OLED login password / add-user password sites set it. |
| **`espnowsetname` space-split** | Submit is unquoted string concat ([G2_Page_Network.cpp](../components/hardwareone/G2_Page_Network.cpp)) — a typed Space silently splits the arg and mangles the name. Quote the arg or set `allowSpace=false` for that field. |
| **Struct-init landmine (prerequisite)** | [G2_Page_Settings.cpp](../components/hardwareone/G2_Page_Settings.cpp) declares bare `TextEntryConfig cfg;` (no `= {}`). Before adding ANY new config field, give every `TextEntryConfig` member a C++ default initializer, or that caller reads garbage. This is the gateway task for everything below. |

### Phase 1 — per-field modes + pick-lists (feasible-today, zero unproven HW deps)

1. **`TextEntryMode` + `startGroup` on `TextEntryConfig`** (defaults preserve
   today's behavior; all 10 callers compile unchanged):
   - `NUMERIC` / `UINT`: single-group digit pad (`0123456789.-` / `0-9`), **no
     Group row at all**. Wired from `settingsLaunchKeyboard`, which already
     knows `e->type` (INT/FLOAT → NUMERIC, U8/U16/U32 → UINT). **Highest-ROI
     single change in the whole study**: `42` drops from 42 scroll + 7 clicks
     to 16 scroll + 3 clicks (~60% less), with zero protocol risk.
   - `IDENT` (`a-zA-Z0-9._-`, symbols group gone): ESPNow Name, OLED
     Username, New Username — matches `isValidPublicUsername` exactly, so
     illegal chars become unenterable. Files-rename gets the same set.
   - `FULL` stays for the 3 password fields + freeform messages +
     STRING settings.
2. **`g2BeginPickList(prompt, options[], n, onPick, onCancel)`** — a plain
   static list, tap-to-commit, no keyboard. Generalizes the already-shipping
   SSID scan-pick (`gScanCache` pattern in G2_Page_Network). First consumer:
   OLED Username picks from `userlist` (≈3 scroll + 1 click instead of ~90+
   scroll typed), with keyboard fallback for unlisted users. This is the
   G2-side realization of the OLED autocomplete-provider idea — and for the
   small enumerable sets this device actually has, a direct pick-list beats
   prefix-completion outright (fewer taps, no new modules).
3. **Bidirectional group nav**: replace the single forward-only `Group:` row
   with adjacent `Grp <-` / `Grp ->` rows (chars shift down one row; 20-row
   budget still holds since NUMERIC/UINT modes shed rows elsewhere). Worst
   group reach drops 5 taps → ≤3. Cheap, layout-local, composes with
   everything later.

### Phase 2 — the substrate bet: static list + UPDATE_TEXT buffer (M/L, gated on §6 Q1)

**Run the 1-hour probe first** (§6 Q1). Then:

- Convert the keyboard render from `g2StartLiveListPage` to the mic-detail
  compound: static List child (controls + current group's chars — buffer row
  moves out of the list, freeing a row) + Text child for `<prompt>: <buf>_`.
- Char/Space/Backspace taps → mutate buffer → **UPDATE_TEXT only**. The list
  is untouched → highlight persists (if Q1 confirms) → consecutive/nearby
  chars cost `|Δrow|` instead of scroll-from-0. `hello`: 73→46 scroll;
  same-group runs (`ll`, digit strings) become ~free.
- **Group switch is the only remaining rebuild** (SHUTDOWN+CREATE, ~600 ms).
  This is A's honest weak spot: mixed-case/symbol-hopping input (passwords)
  switches groups almost every char, so Phase 2 alone is a wash there — and
  slower per group-switch than today's 80 ms REBUILD. Mitigations, in order:
  (a) Phase 1 modes mean most fields have fewer/no groups; (b) ring
  double-tap as **sticky Shift** (historical proposal, not the implemented
  mapping — production uses an active-pad-owned rowless double-tap as Select);
  (c) keep the REBUILD-list fast path for the group-switch swap when the gate
  passes
  (pure list→list swap machinery already exists).
- Even if Q1 **fails** (highlight resets after CLICK anyway): keep the
  compound. The per-keystroke cost drops from REBUILD+ack (~80 ms) to
  fire-and-forget UPDATE_TEXT, and the 19-row re-render disappears — a
  strictly snappier feel at parity scroll cost. Low-regret either way.
- **Fold in editing ops here, not before** (from Approach F): caret `<-`/`->`
  rows + Clear, caret rendered inline in the buffer text
  (`Read|Me.txt`) — pure UPDATE_TEXT, cheap only under the static list.
  Opt-in via config flag so simple fields don't pay the extra-rows tax.
  First real mid-string editing on any surface (OLED included).
- **Ring double-tap = Done** was another historical proposal. It was not the
  mapping selected for production: the removed row is `Select`, and a rowless
  ring double-tap on the active key page now selects the highlighted key.

### Phase 3 — optional garnish (only if still wanted after 1+2)

- Canned-phrase/MRU rows for the two ESP-NOW message fields (they already
  have Hi/OK/Here/Help; MRU adds recall of recent messages).
- Bigram next-char prediction (Approach C) **only** for the two prose-ish
  message fields, off by default, never for identifiers or secrets. The
  critique is right that identifiers/SSIDs miss too often and 6 shifting
  prediction rows add a read-tax on a monocular lens; the persistent static
  list (Phase 2) beats it for the real field population.

---

## 5. Explicitly rejected

| Idea | Why |
|---|---|
| **Voice dictation ("Speak to type")** | Dead, not deferred. The only speech engine in the tree (ESP-SR) is a closed-vocabulary MultiNet **command matcher** — it structurally cannot emit arbitrary text. It's compiled off (`ENABLE_ESP_SR 0`) and the 3 MB model partition isn't flashed on FeatherS3 (reclaimed for LittleFS). The G2 `TRANSCRIBE` channel's RESULT opcode is logged-only in our firmware, and community RE says the Even app does STT phone/cloud-side. Passwords could never go through voice regardless (undictatable + plaintext credential exposure). Salvaged: the `isSecret` flag (Phase 0). |
| **True 2D grid keyboard** | Impossible. Text/Image widgets return no cell index; only List returns an index and it's strictly 1D. A *rendered* grid backed by a hidden list buys nothing over an ordered list and adds proportional-font alignment fragility. |
| **Flat all-chars list (~90 rows)** | Lists cap at 20 items ([display.md](../docs/even-g2-notes-main/docs/display.md)). |
| **Prefix-completion infrastructure (System_TextSuggest + MRU modules)** | The device's enumerable value sets are all tiny; a direct pick-list is strictly fewer taps than type-prefix→Suggest→pick, with zero new modules. Most proposed providers had no real caller. |
| **Two-level group→char menus as a standalone layout (Approach B)** | Real wins on mixed-case (-44% scroll on passwords) but regresses Done/Backspace to deep rows (+12 scroll tax on every field and every correction) and is a wash on the common short-lowercase case. Its direct-group-pick idea survives as Phase 1's bidirectional nav + Phase 2's group handling; the full L1/L2 apparatus doesn't pay for itself. |
| **Per-keystroke live prediction as the default keyboard** | See Phase 3 — optional, prose fields only. |

Bounds that stay regardless of design: maxLen 32 (real WPA2 PSKs go to 63 —
long secrets keep punting to web), ASCII-only (non-ASCII like Cyrillic is
unenterable on-lens), no double-quote (CommandArgs has no escaping).

---

## 6. HW validation checklist (ordered by how much rides on each)

1. **Highlight persistence after CLICK on a static list** *(gates Phase 2's
   core value)*: CREATE a static list+text compound, click a mid-list row,
   send an UPDATE_TEXT to the text child, then scroll one step. Does the
   highlight move from the clicked row, or from row 0? ~1 hour with existing
   test-suite plumbing (`g2ProbeRebuildTextChild` is the template).
2. **Resolved for the production keyboard (2026-08-21):** its arrows emit
   indexed `List_ItemEvent CLICK(0)` when activated, while ring double-tap emits
   rowless `SysEvent DOUBLE_CLICK(3) src=2`. The active-pad presentation epoch
   scopes that SysEvent and selection runs on the tap-dispatch worker; Mic is a
   no-op. Because no row/container is present, the capture does not establish
   strict directional-row identity or identical behavior for every compound.
3. **Scroll wrap at list ends** *(calibrates all cost math)*: does scrolling
   up from row 0 wrap to the bottom? If yes, deep rows cost
   `min(d, N-d)` and every design's numbers improve.
4. **Tap-synchronous UPDATE_TEXT race**: mic-detail updates on a timer;
   confirm an UPDATE_TEXT fired in the same ~50 ms window as a CLICK doesn't
   disturb the list or race the firmware's post-CLICK repaint. (Fallback:
   debounce one tick.)
5. **20-item boundary**: confirm 20 rows renders/scrolls acceptably (we ship
   19 today) and the compound CREATE with single-char labels stays under the
   ~2 KB encode budget.
6. *(curiosity only — expected NO)* Does `TRANSCRIBE` cmd=2 RESULT ever carry
   usable text without the phone app? Only worth checking if someone insists
   on revisiting voice.

---

## 7. Latent bugs surfaced by this study (fix regardless of redesign)

1. **Password plaintext in debug logs** — [G2_Page_TextEntry.cpp:196,205](../components/hardwareone/G2_Page_TextEntry.cpp)
   log the full buffer on cancel/done; three callers type passwords.
2. **`espnowsetname` unquoted submit** — [G2_Page_Network.cpp](../components/hardwareone/G2_Page_Network.cpp);
   a Space in the name splits CLI args silently.
3. **`TextEntryConfig` has no default member initializers** while
   [G2_Page_Settings.cpp](../components/hardwareone/G2_Page_Settings.cpp)
   stack-allocates it bare — any future field addition without defaults is a
   garbage-read. (Currently benign because all five fields happen to be
   assigned; it's a trap, not a live bug.)

---

## 8. Gesture-cost before/after (representative inputs)

Counts = native scroll steps + clicks, current layout vs Phase 1 vs Phase 1+2
(Q1 confirmed). "—" = unchanged by that phase.

| Input | Today | After Phase 1 | After Phase 1+2 |
|---|---|---|---|
| `42` (U8 setting) | 42s + 7c | **16s + 3c** (UINT pad) | 14s + 3c (persist) |
| `morgan` (login username) | ~107s + 14c | **~3s + 1c** (pick-list) | — |
| `hello` (rename/freeform) | 73s + 7c | ~73s + 7c | **~46s + 7c** |
| `WiFi` (mixed-case SSID typed) | 118s + 17c | ~100s + 15c (bidir groups) | **~62s + ~9c** (historical double-tap Shift proposal) |
| `Xk9$mQp2` (WiFi PSK) | ~213s + 31c | ~190s + 28c (bidir groups) | **~120s + ~20c** (Shift + persist; still punts >32 chars to web) |
| `HomeNet2024` (SSID) | already a scan pick | — | — |

Passwords remain the hardest field on any lens keyboard — the honest framing
is "less painful," not "solved." The fields users touch most (settings values,
usernames, names) get the dramatic wins.
