// =============================================================================
// G2 glasses — "Tests" hijack page implementation
// =============================================================================
// Multi-level navigation so the root never becomes a giant scroll-fest:
//
//   ROOT:
//     <- Back
//     BLE Tests >>
//     Transport Tests >>
//     AI Panel Tests >>
//     Character Tests >>
//     Display Tests >>
//     Image Tests >>
//
//   BLE (drill from ROOT):
//     <- Back
//     Hide AI Card
//     Toggle Mic Feed
//     G2 AutoConnect: ON|OFF (toggles boot auto-reconnect, persisted)
//     Heap Snapshot (log) / Lens State Dump (log)
//     (further BLE-recovery / diagnostic taps go here. R1 Ring connect
//      / disconnect rows moved to Networking → Bluetooth submenu —
//      see triggerRingReconnect / triggerRingDisconnect there.)
//
//   BRACKETS (drill from ROOT):
//     <- Back
//     Send 100 B
//     Send 250 B
//     Send 500 B
//     Send 1 KB
//     Send 2 KB
//     (renders a payload of the chosen size to validate the multi-fragment
//      CREATE path against firmware's reassembler. 4 KB+ removed — they
//      consumed the row-buffer headroom needed by other categories.)
//
//   AI PANEL (drill from ROOT):
//     <- Back
//     Full pipeline (CTRL+ASK+ANALYSE+REPLY)
//     No ASK (CTRL+ANALYSE+REPLY)
//     Direct (CTRL+REPLY only)
//     Custom heading
//     (each entry fires the named pipeline against a fixed sample text so
//      we can compare visual outcomes side-by-side without typing on the
//      lens)
//
//   PAYLOAD (drill from BRACKETS, after picking a size):
//     <- Back (returns to ROOT, not BRACKETS — see comment in tap dispatcher)
//     [synthetic content of the chosen byte size]
//
//   CHARS (drill from ROOT):
//     <- Back
//     ASCII >>
//     Unicode >>
//     (split keeps each sub-menu under ~10 entries so it fits one
//      screen, and lets us pile on Unicode variants — especially
//      shaded-fill alternatives — without burying the basics)
//
//   CHARS_ASCII (drill from CHARS):
//     <- Back (returns to CHARS picker)
//     Lowercase / Uppercase / Digits / Symbols / Whitespace
//     Progress (ASCII) / Spinner (ASCII) / Density (punct)
//
//   CHARS_UNICODE (drill from CHARS) — category picker:
//     <- Back (returns to CHARS picker)
//     Blocks & Shading >>
//     Arrows & Spinners >>
//     Symbols & Boxes >>
//     (split into categories so each list's CREATE-list pb stays in a
//      single fragment — empirically a ~22-item / 482 B / 3-fragment
//      list times out the firmware's hijack-container reassembler)
//
//   CHARS_UNICODE_LIST (drill from a Unicode category):
//     <- Back (returns to Unicode picker)
//     <items in selected category>
//
//   CHARS_*_PAYLOAD (drill from a list row):
//     <- Back (returns to that category's list)
//     <category label>
//     <sample chars>
//
// Levels are tracked in gTestLevel; each builder populates gRowPtrs and the
// dispatcher routes taps via the level it just rendered. Adding a new
// category is "add a row to ROOT + a builder + a level enum value + a
// dispatch case" — the same shape we use for Settings.

#include "G2_Page_TestSuite.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "BLE_Peers.h"              // gBlePeerData / autoConnect toggle
#include "System_Settings.h"        // setSetting() — persists autoConnect to NVS
#include "System_Debug.h"
#include "System_MemUtil.h"   // ps_alloc
#include "G2_Page_Common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Source text — repeated lorem ipsum, ample for 8 KB brackets.
// -----------------------------------------------------------------------------
static const char kSourceText[] =
  "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do "
  "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim "
  "ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
  "aliquip ex ea commodo consequat. Duis aute irure dolor in "
  "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
  "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in "
  "culpa qui officia deserunt mollit anim id est laborum. The quick "
  "brown fox jumps over the lazy dog. Sphinx of black quartz, judge my "
  "vow. Pack my box with five dozen liquor jugs. How vexingly quick "
  "daft zebras jump! Bright vixens jump; dozy fowl quack. Crazy Fredrick "
  "bought many very exquisite opal jewels. We promptly judged antique "
  "ivory buckles for the next prize. A wizard's job is to vex chumps "
  "quickly in fog. Watch \"Jeopardy!\", Alex Trebek's fun TV quiz game. "
  "Two driven jocks help fax my big quiz. Five quacking zephyrs jolt my "
  "wax bed. The five boxing wizards jump quickly. Heavy boxes perform "
  "quick waltzes and jigs. Pack my red box with five dozen quality jugs. "
  "Big fjords vex quick waltz nymph. The job requires extra pluck and "
  "zeal from every young wage earner. Sympathizing would fix Quaker "
  "objectives. Jaded zombies acted quaintly but kept driving their "
  "oxen forward. Crabby astronauts jealously vex quiet pelicans. Mr. "
  "Jock, TV quiz PhD, bags few lynx. Quick zephyrs blow, vexing daft "
  "Jim. Sex-charged fop blew my junk TV quiz. Few black taxis drive up "
  "major roads on quiet hazy nights. Just keep examining every low bid "
  "quoted for zinc etchings. My ex pub quiz crowd just flocked to "
  "Venezia. Both fickle dwarves jinx my pig quiz. Public junk dwarves "
  "quiz mighty fox. Quick wafting zephyrs vex bold Jim. Two hardy "
  "boxing kangaroos jet from Sydney to Zanzibar by quick motorway. "
  "Pack my box with five dozen liquor jugs again, just to fill space. "
  "The end of one paragraph leads into the next. ";

// -----------------------------------------------------------------------------
// Size brackets
// -----------------------------------------------------------------------------
// Each bracket is paired into list-widget (L) and text-widget (T)
// variants so the operator can compare what each transport actually
// tolerates. The list path uses g2ShowListPage (ListContainer with N
// items[]); the text path uses g2ShowTextPage (single TextObject with
// a multi-line content string). Empirical data has shown the two
// transports diverge well before 2 KB, but we surface both at every
// size so the ceiling can be measured rather than assumed.
enum TestBracketKind : uint8_t {
  TBK_LIST      = 0,
  TBK_TEXT      = 1,
  // Same wire shape as TBK_TEXT but the dispatcher pre-arms a 100 ms
  // inter-fragment delay (vs. the default 20 ms) for the next burst.
  // Used to test whether the firmware's per-widget reassembly ceiling
  // moves when fragments are spaced further apart — answers "is the
  // 1 KB text drop a buffer-fill or a parse-time race?"
  TBK_TEXT_SLOW = 2,
  // CREATE-text(small placeholder) → REBUILD-text(N B blob). Tests
  // whether the in-place patch path has the same firmware reassembly
  // ceiling as the initial CREATE — answers "can we stream long
  // content into an existing TextObject even though we can't CREATE
  // it directly?".
  TBK_TEXT_REBUILD = 3,
};
struct TestBracket {
  const char*     label;
  size_t          bytes;
  TestBracketKind kind;
};

static const TestBracket kBrackets[] = {
  // Single-fragment territory — should always pass for both transports.
  { "Send 100 B (L)",      100, TBK_LIST },
  { "Send 100 B (T)",      100, TBK_TEXT },
  { "Send 250 B (L)",      250, TBK_LIST },
  { "Send 250 B (T)",      250, TBK_TEXT },
  // Multi-fragment territory — exercises sendPbFragmented.
  { "Send 500 B (L)",      500, TBK_LIST },
  { "Send 500 B (T)",      500, TBK_TEXT },
  // 750 B (T) — bisects the gap between 3-frag (works) and 5-frag
  // (fails) text bursts so we can pin the actual ceiling.
  { "Send 750 B (T)",      750, TBK_TEXT },
  { "Send 1 KB (L)",      1024, TBK_LIST },
  { "Send 1 KB (T)",      1024, TBK_TEXT },
  // Same payload as Send 1 KB (T) but with a 100 ms inter-fragment
  // delay instead of 20 ms. If this passes where the regular variant
  // fails, the firmware's ceiling is timing/buffer-fill, not absolute
  // payload size.
  { "Send 1 KB (T-slow)", 1024, TBK_TEXT_SLOW },
  { "Send 2 KB (L)",      2048, TBK_LIST },
  { "Send 2 KB (T)",      2048, TBK_TEXT },
  // Text-only stretch tests — list path can't reach these sizes
  // (row buffer + firmware reassembly both push back), but text
  // is one TextObject with a single content field; worth poking.
  { "Send 4 KB (T)",      4096, TBK_TEXT },
  { "Send 8 KB (T)",      8192, TBK_TEXT },
  // REBUILD-text scaling probes. CREATE seeds a small placeholder so
  // the firmware always acks, then the bracket's payload goes through
  // REBUILD-text. If these pass at sizes where (T) fails, REBUILD has
  // a higher (or no) reassembly ceiling and we have a building block
  // for streaming long content into an existing TextObject.
  { "Send 1 KB (R)",      1024, TBK_TEXT_REBUILD },
  { "Send 2 KB (R)",      2048, TBK_TEXT_REBUILD },
  { "Send 4 KB (R)",      4096, TBK_TEXT_REBUILD },
  { "Send 8 KB (R)",      8192, TBK_TEXT_REBUILD },
};
static constexpr size_t kBracketCount =
  sizeof(kBrackets) / sizeof(kBrackets[0]);

// -----------------------------------------------------------------------------
// BLE / diagnostic actions
// -----------------------------------------------------------------------------
// Each row is a one-tap operation against the live BLE state. Skew safe:
// we deliberately omit anything that would tear down the lens hijack the
// user is currently inside (g2Disconnect, g2ClearDisplay) — those would
// kill this menu mid-tap. Use the web UI or CLI for those.
struct TestAction {
  const char* label;
  void (*handler)();
  // Optional state-aware label. When non-null, buildActionsRows calls this
  // and uses the result instead of `label`. Lets toggles render their
  // current state on the lens (e.g. "G2 AutoConnect: ON").
  const char* (*dynLabel)();
};

static void actionHideAICard();
static void actionToggleMicFeed();
static void actionToggleG2AutoReconnect();
static const char* labelG2AutoReconnect();
static void actionHeapSnapshot();
static void actionLensStateDump();

static const TestAction kActions[] = {
  // (Ring Reconnect / Disconnect rows moved to Networking → Bluetooth
  //  submenu — see triggerRingReconnect / triggerRingDisconnect in
  //  G2_Page_Network.cpp.)
  // Front-pane card cleanup. AI card normally auto-dismisses after ~10 s
  // but a stuck card (firmware-side) blocks subsequent CTRL ENTER calls
  // with errorCode=7. Force-EXIT clears it.
  { "Hide AI Card",          actionHideAICard,            nullptr },
  // Toggle the G2 → ESP-SR mic feed without going through the Settings
  // page. Logs new state + AFE ring depth + overrun count.
  { "Toggle Mic Feed",       actionToggleMicFeed,         nullptr },
  // Toggle the G2 auto-reconnect-at-boot flag (gBlePeerData[G2].autoConnect).
  // Dynamic label shows ON/OFF; tap flips it and re-renders the page.
  { "G2 AutoConnect",        actionToggleG2AutoReconnect, labelG2AutoReconnect },
  // Pure-log entries: snapshot diagnostic state to serial. Safe — no BLE
  // I/O — useful as canaries before/after a heavy probe.
  { "Heap Snapshot (log)",   actionHeapSnapshot,          nullptr },
  { "Lens State Dump (log)", actionLensStateDump,         nullptr },
};
static constexpr size_t kActionCount =
  sizeof(kActions) / sizeof(kActions[0]);

// -----------------------------------------------------------------------------
// AI panel variants
// -----------------------------------------------------------------------------
// Each entry fires a named pipeline against a fixed sample. The pipelines
// inside g2ShowEvenAIReply* do four sequential BLE sends with vTaskDelays
// between them — that cannot run in the BLE notify task that dispatches
// our tap handlers, otherwise the writes deadlock against the BT stack
// servicing the same task (verified on hardware 2026-04-26: only the
// first envelope leaves and the firmware tears down our hijack with
// "Connection lost" while the worker hangs). Mirror the page-swap-worker
// pattern: tap handler spawns a one-shot task, returns immediately, the
// worker runs the multi-step send and exits.
//
// Stack: 4 KB is plenty — the longest call path is sendEnvelope (~1 KB
// stack high water on bench) plus pb-builder buffers (~400 B) plus
// FreeRTOS overhead (~600 B); 4 KB leaves comfortable headroom.

static void aiFullPipeline();
static void aiNoAsk();
static void aiDirect();
static void aiCustomHeading();
static void aiLoadingMock();

struct AIVariant {
  const char* label;
  void (*handler)();
};

static const AIVariant kAIVariants[] = {
  { "Full pipeline",   aiFullPipeline   },
  { "No ASK",          aiNoAsk          },
  { "Direct (CTRL+REPLY)", aiDirect    },
  { "Custom heading",  aiCustomHeading  },
  // Loading-mock: simulates what a "Scanning networks..." or generic
  // progress-card flow would look like — open the front pane, hold for
  // 5 s, then explicitly dismiss via CTRL EXIT. Useful for iterating
  // on the front-pane UX before wiring it into a real flow. Empirical
  // 2026-04-26: CTRL ENTER from inside a hijack sub-page (Network)
  // returns errorCode=7 and the card never appears, while the same
  // call from this test menu works. Reproduce both paths to learn
  // what context matters.
  { "Loading mock (5s)", aiLoadingMock  },
};
static constexpr size_t kAIVariantCount =
  sizeof(kAIVariants) / sizeof(kAIVariants[0]);

static const char kAISampleBody[] =
  "Sample reply: front-pane card test from the on-glasses suite.";

// -----------------------------------------------------------------------------
// Character-rendering tests — split into ASCII and Unicode sub-suites
// -----------------------------------------------------------------------------
// Each entry renders a sample character set on the lens via the hijacked
// widget. The split keeps each sub-menu under ~10 entries so it fits one
// screen, and lets us add a lot of Unicode variants (which is where the
// font coverage gets interesting) without burying the basics.
//
// Empirical findings live in `docs/G2_PROTOCOL.md` under "Firmware font
// coverage". Update both that section and these tables when re-testing
// against newer firmware.
struct CharTest {
  const char* label;
  const char* sample;
};

// ASCII sub-suite: 0x20..0x7E and immediate ASCII derivatives.
static const CharTest kAsciiCharTests[] = {
  { "Lowercase",        "abcdefghijklmnopqrstuvwxyz" },
  { "Uppercase",        "ABCDEFGHIJKLMNOPQRSTUVWXYZ" },
  { "Digits",           "0123456789" },
  { "Symbols",          "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~" },
  { "Whitespace",       "tab[\there] space[ ] end" },
  { "Progress (ASCII)", "[####......] 40%" },
  { "Spinner (ASCII)",  "| / - \\ | / - \\" },
  // Density via punctuation — investigates whether ASCII glyphs at
  // increasing visual weight can stand in for shade-block fills.
  { "Density (punct)",  ". o O 0 # |  ascending visual weight" },
};
static constexpr size_t kAsciiCharTestCount =
  sizeof(kAsciiCharTests) / sizeof(kAsciiCharTests[0]);

// Unicode sub-suites. Split into three categories so each CREATE-list
// fits in a single fragment — empirically, a 22-item list (482 B pb,
// 3 fragments) times out the firmware's hijack-container reassembler
// (observed 2026-04-26: "CREATE-list timeout — container not primed").
// Each sub-list below is ≤ ~250 B pb, single fragment.
//
// Heavy on shaded-fill alternatives — that's where firmware coverage is
// least mapped. Naming: prefix labels with category for at-a-glance
// scanning in each sub-picker.

// Blocks & shading. Ordering convention: fully-working entries first,
// then "(partial)" entries that render some but not all of their
// glyphs, then "(broken)" entries pinned at the bottom for re-test
// against future firmware. Empirical results live in
// docs/G2_PROTOCOL.md "Firmware font coverage".
static const CharTest kUniBlocksTests[] = {
  // ── Working ─────────────────────────────────────────────────────────

  // Eighth-block series U+2581..U+2588 — all eight render.
  { "Eighth-blocks 1/8..8/8",     "\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88" },

  // Hatched squares U+25A4..U+25A9 — all six render distinctly. Best
  // single source of shading variety on the firmware.
  { "Hatched squares H/V/cross",  "\xe2\x96\xa4\xe2\x96\xa5\xe2\x96\xa6\xe2\x96\xa7\xe2\x96\xa8\xe2\x96\xa9" },

  // Geometric shapes U+25A0..U+25CF — squares (filled/empty, large/
  // small), circles (filled/empty), diamonds (filled/empty).
  { "Geometric sq/circ/diam",     "\xe2\x96\xa0\xe2\x96\xa1\xe2\x96\xaa\xe2\x96\xab\xe2\x97\x8f\xe2\x97\x8b\xe2\x97\x86\xe2\x97\x87" },

  // Spacing experiment: same shade glyphs as "Shades" but with single
  // ASCII spaces between — confirms whitespace renders fine.
  { "Spacing: shades + spaces",   "\xe2\x96\x88 \xe2\x96\x93 \xe2\x96\x92 \xe2\x96\x91" },

  // Spacing experiment: full block + half block alternation.
  { "Spacing: full + half mix",   "\xe2\x96\x88\xe2\x96\x8c\xe2\x96\x88\xe2\x96\x90\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88\xe2\x96\x80" },

  // ── Partial (some glyphs render) ────────────────────────────────────

  // Full + shade glyphs (U+2588, U+2593, U+2592, U+2591). Only the full
  // block + the dark shade render; the dark shade renders as diagonal
  // stripes rather than a pure dark fill. Medium and light shades miss.
  { "Shades (partial: █▓ only)",  "\xe2\x96\x88\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91" },

  // Half-blocks U+2580 (top), U+2584 (bottom), U+258C (left), U+2590
  // (right). Top + bottom render; left/right are unreliable
  // (one of L/R rendered on 2026-04-26, the other did not). Pattern is
  // the firmware fonts cover horizontal-cell halves but not vertical
  // halves with quarter-cell precision.
  { "Half-blocks (partial: ▀▄)",  "\xe2\x96\x80\xe2\x96\x84\xe2\x96\x8c\xe2\x96\x90" },

  // ── Broken (kept for re-test against future firmware) ───────────────

  // Quadrants U+2596..U+259F — none rendered on 2026-04-26. Quarter-
  // cell precision is absent from the firmware font.
  { "Quadrants 25/50/75% (broken)","\xe2\x96\x96\xe2\x96\x97\xe2\x96\x98\xe2\x96\x99\xe2\x96\x9a\xe2\x96\x9b\xe2\x96\x9c\xe2\x96\x9d\xe2\x96\x9e\xe2\x96\x9f" },
};
static constexpr size_t kUniBlocksTestCount =
  sizeof(kUniBlocksTests) / sizeof(kUniBlocksTests[0]);

// Arrows & spinners. Same convention: working → partial → broken.
static const CharTest kUniArrowsTests[] = {
  // ── Working ─────────────────────────────────────────────────────────

  // Single-line arrows including the bidirectional ones (U+2194,
  // U+2195). All six render.
  { "Arrows single + bidir",      "\xe2\x86\x90\xe2\x86\x92\xe2\x86\x91\xe2\x86\x93\xe2\x86\x94\xe2\x86\x95" },

  // ── Partial ─────────────────────────────────────────────────────────

  // Filled triangle pointers U+25B2/BC/C4/BA. Up/down (▲▼) render;
  // left/right (◄►) miss — the U+25C4/BA pair is from a newer block
  // not in the firmware font.
  { "Triangles (partial: ▲▼)",    "\xe2\x96\xb2\xe2\x96\xbc\xe2\x97\x84\xe2\x96\xba" },

  // Quarter-circle spinner U+25D0..U+25D3. Left/right halves (◐◑)
  // render; top/bottom halves (◓◒) miss — same horizontal-only pattern
  // as half-blocks. Yields a 2-frame loop in practice.
  { "Spinner: quarter (partial)", "\xe2\x97\x90\xe2\x97\x93\xe2\x97\x91\xe2\x97\x92" },

  // ── Broken (kept for re-test against future firmware) ───────────────

  // Double arrows U+21D0..U+21D3 — only ⇒ rendered on 2026-04-26.
  { "Arrows double (broken)",     "\xe2\x87\x90\xe2\x87\x92\xe2\x87\x91\xe2\x87\x93" },

  // Quadrant spinner using single quadrant glyphs — depends on
  // "Quadrants" rendering, which the firmware doesn't support.
  { "Spinner: quadrants (broken)","\xe2\x96\x98\xe2\x96\x9d\xe2\x96\x97\xe2\x96\x96" },

  // Braille pattern U+2800-block — confirmed broken on 2026-04-26.
  { "Spinner: Braille (broken)",  "\xe2\xa0\x8b\xe2\xa0\x99\xe2\xa0\xb9\xe2\xa0\xb8\xe2\xa0\xbc\xe2\xa0\xb4\xe2\xa0\xa6\xe2\xa0\xa7\xe2\xa0\x87\xe2\xa0\x8f" },
};
static constexpr size_t kUniArrowsTestCount =
  sizeof(kUniArrowsTests) / sizeof(kUniArrowsTests[0]);

// Symbols & boxes. Same convention: working → broken.
static const CharTest kUniSymbolsTests[] = {
  // ── Working ─────────────────────────────────────────────────────────
  { "Box light",                  "\xe2\x94\x80\xe2\x94\x82\xe2\x94\x8c\xe2\x94\x90\xe2\x94\x94\xe2\x94\x98\xe2\x94\x9c\xe2\x94\xa4\xe2\x94\xac\xe2\x94\xb4\xe2\x94\xbc" },
  { "Box double",                 "\xe2\x95\x90\xe2\x95\x91\xe2\x95\x94\xe2\x95\x97\xe2\x95\x9a\xe2\x95\x9d\xe2\x95\xa0\xe2\x95\xa3\xe2\x95\xa6\xe2\x95\xa9\xe2\x95\xac" },
  { "Progress (uni eighth)",      "[\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x83\xe2\x96\x81\xe2\x96\x81\xe2\x96\x81\xe2\x96\x81\xe2\x96\x81] 43%" },
  { "Math / units",               "\xc2\xb1\xc3\x97\xc3\xb7\xc2\xb0\xe2\x80\xb2\xe2\x80\xb3\xc2\xb5\xce\xa9\xe2\x88\x9e" },
  { "Bullets / stars",            "\xe2\x80\xa2\xe2\x97\xa6\xe2\x96\xaa\xe2\x96\xab\xe2\x98\x85\xe2\x98\x86" },
  { "Currency",                   "\xc2\xa2\xc2\xa3\xc2\xa5\xe2\x82\xac\xe2\x82\xb9" },

  // ── Broken (kept for re-test against future firmware) ───────────────

  // Dingbats U+2713/17/14/18 — entire block missing from firmware font.
  // Use ASCII `[x] / [ ]` for checks until firmware extends coverage.
  { "Checks / Xs (broken)",       "\xe2\x9c\x93\xe2\x9c\x97\xe2\x9c\x94\xe2\x9c\x98" },
};
static constexpr size_t kUniSymbolsTestCount =
  sizeof(kUniSymbolsTests) / sizeof(kUniSymbolsTests[0]);

// Unicode-category picker entries — order must match
// kUnicodeCategories[] indices used by the dispatcher.
struct UnicodeCategory {
  const char*     label;
  const CharTest* tests;
  size_t          count;
};

static const UnicodeCategory kUnicodeCategories[] = {
  { "Blocks & Shading",  kUniBlocksTests,  kUniBlocksTestCount  },
  { "Arrows & Spinners", kUniArrowsTests,  kUniArrowsTestCount  },
  { "Symbols & Boxes",   kUniSymbolsTests, kUniSymbolsTestCount },
};
static constexpr size_t kUnicodeCategoryCount =
  sizeof(kUnicodeCategories) / sizeof(kUnicodeCategories[0]);

// Selected category index — set when the user taps a row in the Unicode
// picker, read by the list/payload builders so we don't need a level
// per category.
static size_t gUnicodeCategoryIdx = 0;

enum AIWorkerKind : uint8_t {
  AIWK_FULL    = 0,
  AIWK_NOASK   = 1,
  AIWK_DIRECT  = 2,
  AIWK_LOADING = 3,   // open card, hold ~5 s, dismiss via CTRL EXIT
};

struct AIWorkerArgs {
  AIWorkerKind kind;
  char heading[64];
  char body[256];
};

static void aiWorker(void* arg) {
  AIWorkerArgs* a = (AIWorkerArgs*)arg;
  if (!a) { vTaskDelete(nullptr); return; }
  switch (a->kind) {
    case AIWK_FULL:    g2ShowEvenAIReply(a->heading, a->body); break;
    case AIWK_NOASK:   g2ShowEvenAIReplyNoAsk(a->body); break;
    case AIWK_DIRECT:  g2ShowEvenAIReplyDirect(a->body); break;
    case AIWK_LOADING:
      // Open the card, hold for the same ~5 s a real WiFi scan would
      // take, then explicitly dismiss. The hold mimics what the
      // network-scan worker would do — useful to A/B against the
      // production flow if/when CTRL ENTER errorCode=7 gets resolved.
      g2ShowEvenAIReplyNoAsk(a->body);
      vTaskDelay(pdMS_TO_TICKS(5000));
      g2HideEvenAICard();
      break;
  }
  free(a);
  vTaskDelete(nullptr);
}

// Spawn the worker so the BLE notify task that ran the tap handler can
// return immediately. Heap-allocates args; the worker frees them.
static void spawnAIWorker(AIWorkerKind kind, const char* heading, const char* body) {
  // Worker args (~340 B) — short-lived, freed by the worker. PSRAM is fine
  // here: the worker reads them once at task entry, no DMA / ISR.
  AIWorkerArgs* a = (AIWorkerArgs*)ps_alloc(sizeof(AIWorkerArgs),
                                            AllocPref::PreferPSRAM, "g2.test.aiArgs");
  if (!a) {
    DEBUG_G2F("[G2] AI test: ps_alloc failed (kind=%u)", (unsigned)kind);
    return;
  }
  a->kind = kind;
  if (heading) {
    strncpy(a->heading, heading, sizeof(a->heading) - 1);
    a->heading[sizeof(a->heading) - 1] = '\0';
  } else {
    a->heading[0] = '\0';
  }
  if (body) {
    strncpy(a->body, body, sizeof(a->body) - 1);
    a->body[sizeof(a->body) - 1] = '\0';
  } else {
    a->body[0] = '\0';
  }
  if (xTaskCreate(aiWorker, "g2_ai_test", 4096, a, 5, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] AI test: xTaskCreate failed (kind=%u)", (unsigned)kind);
    free(a);
  }
}

// imgProbeWorker / spawnImgProbeWorker are defined further down, after
// buildImageRows() and gRowPtrs[] (the worker calls into both to redraw
// the picker when a probe burst finishes). Forward-decl so the
// dispatcher branch can reference spawnImgProbeWorker.
typedef const char* (*ImgProbeFn)();
static bool spawnImgProbeWorker(ImgProbeFn fn);

static void aiFullPipeline()  { spawnAIWorker(AIWK_FULL,    "(host)", kAISampleBody); }
static void aiLoadingMock()   { spawnAIWorker(AIWK_LOADING, nullptr,  "Scanning networks..."); }
static void aiNoAsk()         { spawnAIWorker(AIWK_NOASK,  nullptr,  kAISampleBody); }
static void aiDirect()        { spawnAIWorker(AIWK_DIRECT, nullptr,  kAISampleBody); }
static void aiCustomHeading() { spawnAIWorker(AIWK_FULL,   "Weather", "72 °F sunny — heading test"); }

// -----------------------------------------------------------------------------
// Display container tests — exercise the geometry presets visually
// -----------------------------------------------------------------------------
// Each entry renders the same sample content via a different
// G2ContainerGeom preset so we can compare on-lens position and size
// side-by-side. Useful when designing new pages that want to coexist
// with other surfaces (front-pane card, dashboard) or fit a smaller
// region than the default LARGE.
//
// All entries use a TEXT widget with a multi-line sample so the user
// can see how content wraps in each rect. A LIST variant is included
// at the bottom to demonstrate the per-row selection chrome difference
// between widget types.

static const char kDisplaySampleText[] =
  "Display geometry test\n"
  "Line 2: lorem ipsum dolor sit amet\n"
  "Line 3: 0123456789\n"
  "Line 4: ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
  "Line 5: abcdefghijklmnopqrstuvwxyz\n"
  "Line 6: short last line";

struct DisplayVariant {
  const char*           label;
  const G2ContainerGeom geom;
};

// Geometry variants — same set used by both the TEXT and LIST submenus
// so a tester can A/B the two widget types at every preset without
// scrolling between two visually different lists. Order: full → halves
// → quadrants → strips → stress shapes. Production presets first, then
// stress shapes pinned to the bottom.
static const DisplayVariant kGeomVariants[] = {
  { "Full (576x288)",         G2_GEOM_FULL         },
  { "Large (560x272)",        G2_GEOM_LARGE        },
  { "Medium (480x240)",       G2_GEOM_MEDIUM       },
  { "Small (280x130 ctr)",    G2_GEOM_SMALL        },
  { "Top half (560x130)",     G2_GEOM_TOP_HALF     },
  { "Bottom half (560x130)",  G2_GEOM_BOTTOM_HALF  },
  { "Left half (280x272)",    G2_GEOM_LEFT_HALF    },
  { "Right half (280x272)",   G2_GEOM_RIGHT_HALF   },
  { "Quad TL (280x136)",      G2_GEOM_QUAD_TL      },
  { "Quad TR (280x136)",      G2_GEOM_QUAD_TR      },
  { "Quad BL (280x136)",      G2_GEOM_QUAD_BL      },
  { "Quad BR (280x136)",      G2_GEOM_QUAD_BR      },
  { "Status bar (576x40)",    G2_GEOM_STATUS_BAR   },
  { "Footer (576x40)",        G2_GEOM_FOOTER       },
  { "Tall narrow (96x272)",   G2_GEOM_TALL_NARROW  },
  { "Center dot (128x80)",    G2_GEOM_CENTER_DOT   },
};
static constexpr size_t kGeomVariantCount =
  sizeof(kGeomVariants) / sizeof(kGeomVariants[0]);

// Combo (text + image) tests live in their own sub-list and route to the
// existing "mixed" image probes (Q16/Q17/Q18). These probes exercise a
// single CreateStartUpPageContainer carrying both ListObject and
// ImageObject children at varying spatial positions — exactly the
// "combo display at various positions" shape this submenu surfaces.
//
// Kept here (not moved out of Image Tests/Streaming) because the same
// probe is interesting from two angles: spatial layout (this menu) and
// streaming-lifecycle (the Image Tests menu). Two paths, one probe.
struct ComboTest {
  const char* label;
  ImgProbeFn  probe;
};

static const ComboTest kComboTests[] = {
  { "List + Image side-by-side", g2ProbeImageQ16MixedSideBySide },
  { "List + Image overlap",      g2ProbeImageQ17MixedOverlap    },
  { "List + Icon (80x80 TR)",    g2ProbeImageQ18MixedIcon       },
};
static constexpr size_t kComboTestCount =
  sizeof(kComboTests) / sizeof(kComboTests[0]);

// Track whether the lens is currently rendering a display test
// (LIST or TEXT) versus the Display Tests picker. Used by the
// dispatcher to decide whether tap idx=0 means "back to picker"
// (we're inside a test) or "back to ROOT" (we're on the picker).
static bool gInDisplayTest = false;

// displayTestExitToPicker() and runDisplayVariant() are defined
// further down, after buildDisplayRows() and the gRowPtrs[] buffer
// declaration — they need both at call time. Forward-decl of the
// dispatch entry-point so the action table (if any) can reference
// it; runDisplayVariant is only called from the dispatcher below
// the buffer, so no forward decl needed.

// -----------------------------------------------------------------------------
// Navigation state
// -----------------------------------------------------------------------------

enum TestLevel : uint8_t {
  TEST_LEVEL_ROOT                  = 0,  // category list
  TEST_LEVEL_ACTIONS               = 1,  // BLE Tests sub-menu
  TEST_LEVEL_BRACKETS              = 2,  // size-bracket sub-menu
  TEST_LEVEL_PAYLOAD               = 3,  // synthetic payload after a bracket tap
  TEST_LEVEL_AI                    = 4,  // AI panel variant sub-menu
  TEST_LEVEL_CHARS                 = 5,  // ASCII / Unicode picker
  TEST_LEVEL_CHARS_ASCII           = 6,  // ASCII char-test list
  TEST_LEVEL_CHARS_ASCII_PAYLOAD   = 7,  // showing one ASCII set
  TEST_LEVEL_CHARS_UNICODE         = 8,  // Unicode category picker
  TEST_LEVEL_CHARS_UNICODE_LIST    = 9,  // tests within selected Unicode category
  TEST_LEVEL_CHARS_UNICODE_PAYLOAD = 10, // showing one Unicode set
  TEST_LEVEL_DISPLAY               = 11, // Display Tests parent (TEXT/LIST/Combo picker)
  TEST_LEVEL_DISPLAY_TEXT          = 12, // Display Tests / TEXT widget geom variants
  TEST_LEVEL_DISPLAY_LIST          = 13, // Display Tests / LIST widget geom variants
  TEST_LEVEL_DISPLAY_COMBO         = 14, // Display Tests / Combo (text + image)
  TEST_LEVEL_IMAGE                 = 15, // Image Tests router (3 sub-levels below)
  TEST_LEVEL_IMAGE_CONFIRMED       = 16, // Doc + Q4 canary + Q6 + Q6b
  TEST_LEVEL_IMAGE_STATIC          = 17, // Q9 frame builder
  TEST_LEVEL_IMAGE_STREAMING       = 18, // Q11 then Q10
  TEST_LEVEL_DISPLAY_SELECTION         = 19, // Selection Patterns parent (3 buckets)
  TEST_LEVEL_DISPLAY_SELECT_LISTS      = 20, // Bucket 1 — native list widgets
  TEST_LEVEL_DISPLAY_SELECT_MIXED      = 21, // Bucket 2 — compound TextObject layouts
  TEST_LEVEL_DISPLAY_SELECT_EDGES      = 22, // Bucket 3 — edge-anchored geoms + canaries
};
static TestLevel gTestLevel = TEST_LEVEL_ROOT;

// -----------------------------------------------------------------------------
// Row buffer.
// -----------------------------------------------------------------------------
// 96 rows × 80 chars handles the 8 KB bracket payload comfortably
// (8192 / 80 ≈ 103 chars-per-row, but with overhead each row's pb-
// encoded length adds ~3 bytes, so ~80 effective chars/row → ~96 rows
// to reach 8 KB pb).
#define TEST_MAX_ROWS  96
#define TEST_ROW_LEN   80

// Per-page item cap for paginated lists (currently the char-test
// sub-menus). 10 matches Settings; raise if a category's CREATE-list
// stays comfortably in a single fragment, lower if any future addition
// pushes it past ~250 B pb.
#define TEST_VISIBLE_LIST_ROWS  10

// Row buffers live in PSRAM via EXT_RAM_BSS_ATTR — they're 7.6 KB +
// 0.4 KB total and only touched from regular task context (page
// builders + BLE notify task), no DMA, no ISR. Frees ~8 KB of DRAM
// for BLE/WiFi/HTTP runtime allocations.
EXT_RAM_BSS_ATTR static char        gRows[TEST_MAX_ROWS][TEST_ROW_LEN];
EXT_RAM_BSS_ATTR static const char* gRowPtrs[TEST_MAX_ROWS];

// Pagination state for char-test lists. One page var per paginated
// level — they're independent (you can be on page 2 of ASCII and page
// 0 of Unicode/Blocks at the same time, though only one is visible at
// once). The rest are scratch values populated by the most recent
// builder so the dispatcher can recognise prev/next taps without
// re-deriving the layout.
static size_t gAsciiPage         = 0;
static size_t gUnicodeListPage   = 0;
static int    gTsPagePrevRow     = -1;
static int    gTsPageNextRow     = -1;
static size_t gTsPageStartIdx    = 0;

// -----------------------------------------------------------------------------
// Row helpers
// -----------------------------------------------------------------------------

// Each level passes its own label so the back row tells the user where
// the tap actually returns to (e.g. "<- Main Menu" at the testsuite root,
// "<- Tests" at level-1 submenus, "<- Image" inside Image/Static).
static void writeBackRow(const char* label) {
  const char* lbl = (label && label[0]) ? label : "<- Back";
  strncpy(gRows[0], lbl, TEST_ROW_LEN);
  gRows[0][TEST_ROW_LEN - 1] = '\0';
  gRowPtrs[0] = gRows[0];
}

// -----------------------------------------------------------------------------
// Level builders
// -----------------------------------------------------------------------------

// ROOT: category list
static size_t buildRootRows() {
  writeBackRow("<- Main Menu");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "BLE Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Transport Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "AI Panel Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Character Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Display Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Image Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// IMAGE: parent router. Each entry drills into a sub-level grouped by
// what the probes are for: Confirmed/Diagnostic for known-good things
// to re-run as canaries, Static for single-frame draw API tests,
// Streaming for multi-frame swap tests. Keeps each sub-list short
// enough to fit one screen on the lens.
static size_t buildImageRows() {
  writeBackRow("<- Tests");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "Confirmed / Diagnostic >>"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Static Tests >>");           gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Streaming Tests >>");        gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// IMAGE / Confirmed — known-good probes used as session-health canaries
// or baseline references. Tap Q4 first each session to verify the
// pipeline is alive; Q6 is the proven full-tile renderer; Q6b is the
// tap-to-dismiss variant for testing the dismiss mechanism.
static size_t buildImageConfirmedRows() {
  writeBackRow("<- Image");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "Doc: dump verified schema");        gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q4: CREATE-image canary");          gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q6: BMP 288x144 (3 s hold)");       gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q6b: BMP 288x144 (2tap-dismiss)"); gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// IMAGE / Static — single-frame composition tests. Q9 exercises the
// rect-primitive draw API that'll back the future pushTile() public
// surface for feature code. QGlizzy is a hardcoded-path SD-load canary.
// Q12 is the full-display 4-tile probe (first multi-tile CREATE).
static size_t buildImageStaticRows() {
  writeBackRow("<- Image");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "Q9: frame builder (3-band)");       gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "QGlizzy: SD /PICTURES/test.bmp");   gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q12: full-screen 576x288 (4 tiles)"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q19: solo 96x96 (small-dim test)"); gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// IMAGE / Streaming — multi-frame swap tests, no re-CREATE between
// frames. Q11 first (simplest); Q10 only if Q11 leaves visible tearing.
// Q13/Q14 are live-update pipelines paced by `g2liverate` (CLI).
static size_t buildImageStreamingRows() {
  writeBackRow("<- Image");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "Q11: simple swap (A->B)");          gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q10: clear-then-push (A->blk->B)"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q13: live image tile @ rate");      gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q14: live TEXT (rebuild) @ rate");  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q15: LEFT-arm image push test");    gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q16: mixed list+image side-by-side"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q17: mixed list+image overlap");    gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q18: mixed list+icon (80x80)");     gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// Forward-decl: imgProbeWorker (below) rebuilds whichever sub-list the
// probe was launched from, including DISPLAY_COMBO whose builder is
// defined further down with the rest of the Display section.
static size_t buildDisplayComboRows();
// Selection Edges bucket — needed by imgProbeWorker so it can rebuild
// the right picker after the dual-pane CREATE probe (test S) returns.
// Defined further down with the rest of the Selection Patterns
// builders.
static size_t buildSelectionEdgesRows();

// -----------------------------------------------------------------------------
// Image-probe worker (defined here so it can call buildImageRows + gRowPtrs)
// -----------------------------------------------------------------------------
// Image probes do BLE TX + 500 ms-scale settles + multi-second candidate
// loops. Running them on the BLE notify thread (where tap dispatchers
// live) blocks the notify pipeline so the firmware's response acks can't
// be processed — which manifests on lens as "Connection lost" because
// the firmware's plugin task gives up waiting for our ack flow. Same
// reason the existing AI tests use spawnAIWorker. Mirror that pattern:
// the worker runs the probe AND rebuilds the picker afterward, so the
// dispatcher returns immediately and the notify thread stays live.
static void imgProbeWorker(void* arg) {
  ImgProbeFn fn = (ImgProbeFn)arg;
  if (fn) {
    const char* result = fn();
    DEBUG_G2F("[G2] Image probe done → %s", result ? result : "(null)");
  }
  // Settle window before rebuilding the picker. The probe's own
  // teardown (typically a final Shutdown) often triggers firmware
  // SYSTEM_EXIT + DISPLAY_OFF echoes that arrive ~300-500 ms later.
  // Those echoes can race our picker-rebuild CREATE for the BLE
  // write mutex, ending in a "sendPbFragmented: mutex timeout" and
  // a wedged hijack state (observed 2026-04-30 with Selection-S
  // dual-pane + REBUILD-text-child probes). 500 ms is enough for the
  // echoes to land and our DISPLAY_OFF→sendHijackShutdown handler
  // to complete before we contend for the wire.
  vTaskDelay(pdMS_TO_TICKS(500));

  // Sanity check: if the firmware tore down the hijack while we were
  // probing (DISPLAY_OFF / SYSTEM_EXIT cleared lens.hijackActive), we
  // shouldn't push more content — there's no widget to host it. The
  // user has effectively exited; rebuilding the picker would just
  // re-establish a fresh hijack they didn't ask for, and on the
  // failure path it produces the noisy mutex-timeout sequence.
  if (!g2LensGetState().hijackActive) {
    DEBUG_G2F("[G2] Image probe: hijack ended during probe — "
              "skipping picker rebuild (user exited or firmware "
              "tore down)");
    vTaskDelete(nullptr);
    return;
  }

  // Rebuild whichever sub-level the probe was launched from so the user
  // can chain probes without backing out to the parent. gTestLevel is
  // still set to the source sub-level since the dispatcher leaves it
  // alone when invoking a probe (only Back changes the level).
  size_t n;
  switch (gTestLevel) {
    case TEST_LEVEL_IMAGE_CONFIRMED:     n = buildImageConfirmedRows();  break;
    case TEST_LEVEL_IMAGE_STATIC:        n = buildImageStaticRows();     break;
    case TEST_LEVEL_IMAGE_STREAMING:     n = buildImageStreamingRows();  break;
    case TEST_LEVEL_DISPLAY_COMBO:       n = buildDisplayComboRows();    break;
    case TEST_LEVEL_DISPLAY_SELECT_EDGES: n = buildSelectionEdgesRows(); break;
    default:                             n = buildImageRows();           break;
  }
  g2ShowListPage(gRowPtrs, n);
  vTaskDelete(nullptr);
}

static bool spawnImgProbeWorker(ImgProbeFn fn) {
  // 8 KB stack — image probes that build a small BMP (Q5/Q7) keep the
  // pixel buffer (~1 KB) and pb body (~1.1 KB) on the stack, plus
  // sendPbFragmented's per-fragment frame buffer (242 B), plus the
  // DEBUG_G2F vsnprintf path. 4 KB was tight enough to overflow on Q5
  // (observed 2026-04-26: stack overflow on g2_img_probe). 8 KB gives
  // ~3 KB of headroom over the worst-case probe footprint.
  if (xTaskCreate(imgProbeWorker, "g2_img_probe", 8192, (void*)fn, 5, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] Image probe: xTaskCreate failed");
    return false;
  }
  return true;
}

// DISPLAY: parent picker — TEXT / LIST / Combo / Selection patterns.
static size_t buildDisplayRows() {
  writeBackRow("<- Tests");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "TEXT widget >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "LIST widget >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Combo (Text + Image) >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Selection patterns >>");
  gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// DISPLAY/SELECTION: picker for "how should a 2-choice prompt look
// on the lens" experiments. Empirical findings (firmware 2.2.0.24,
// 2026-04-30):
//
//   * Native list widgets stack rows vertically at their geom; in
//     containers shorter than ~40 px only the top row is visible.
//   * Compound CreateStartUpPage with up to 4 TextObject children
//     (repeated wrapper field 3) all render and ack — but content
//     within each TextObject draws at the top-left of its box, so
//     "bottom" content needs a geom with y near 240+, not just a
//     box that occupies the lower half.
//   * Compound TextObject CHILDREN ARE NOT NATIVELY TAPPABLE — the
//     test bench's eventCapture canary (Q below) and the list+text
//     mix (R below) explore the two viable selection paths.
//
// Three families:
//
//   Native list-widget at non-standard geom (cheap, no pb work):
//     A.  Footer list (Yes/No) [BROKEN: rows clip at 40px]
//     A3. List @ LEFT_HALF (narrow vertical list)
//     A5. List @ SMALL (centered modal-style)
//     C.  Vertical Yes/No (default geom — known good)
//   (A1/A2/A4 dropped — overlapped existing text area or were
//   visually identical to A3/C.)
//
//   Compound TextObject children — display only unless eventCapture
//   is set on each child. Verified working at 1, 2, 3, 4 children.
//     B. Bottom corners (Yes / No)              — 2 children @ QUAD_BL/BR
//     D. Top corners (Yes / No)                 — 2 children @ QUAD_TL/TR
//     E. Halves (Yes / No)                      — 2 children @ LEFT/RIGHT_HALF
//     F. Triple bottom (Yes / Maybe / No)       — 3 children across bottom
//     G. Title + corners (Q / Yes / No)         — 3 children: title + 2 buttons
//     H. Quad 4 buttons (Yes/No/Maybe/Cancel)   — 4 children @ all 4 quadrants
//     I. Header + halves (title + Yes/No)       — 3 children: top strip + halves
//     J. Three stacked strips                   — 3 children: top/mid/bot horizontal
//     K. Centered modal (CENTER_DOT)            — 1 child @ centered 128x80
//
//   New edge-anchored geoms + selectability canaries:
//     L. Right column (text)                    — 1 TextObject @ rightmost 130px strip
//     M. Right column (list)                    — list at same right strip
//     N. Bottom-edge text strip                 — 1 TextObject @ {8,248,560,32}
//     O. Bottom-edge buttons (mix)              — 2 TextObjects @ bottom edge
//     P. Top notification bar                   — 1 TextObject @ STATUS_BAR
//     Q. eventCapture=1 corners (canary)        — same as B with evcap=1
//     R. Title + 2-row list (list+text mix)     — title TextObject + Yes/No ListObject
// Top-level Selection Patterns picker: 3 sub-buckets. Originally this
// was a flat 20-row menu, but the firmware on G2 (2.2.0.24) cannot
// reassemble 3-fragment CREATE-list payloads — once the row count
// pushed the protobuf past two MTU fragments the container never
// primed. Splitting into three sub-pickers keeps every CREATE inside
// the safe 1–2 fragment envelope. The buckets are organised by what
// they exercise rather than evenly by count:
//   1. Lists      — native ListObject geom variants (4 entries)
//   2. Compound   — multi-TextObject CREATEs, display-only (9 entries)
//   3. Edges      — edge-anchored geoms + selectability canaries (7)
static size_t buildSelectionRows() {
  writeBackRow("<- Display");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "Lists >>");            gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Compound text >>");    gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Edges + canaries >>"); gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// Bucket 1 — native ListObject geom variants. Each row tap spawns a
// real list at a named geom; tap inside the list returns to picker.
static size_t buildSelectionListsRows() {
  writeBackRow("<- Selection");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "A. Footer list (BROKEN)");  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "A3. List @ LEFT_HALF");     gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "A5. List @ SMALL (modal)"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "C. Vertical Yes/No");       gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// Bucket 2 — compound TextObject CREATEs. Each row spawns a multi-
// TextObject page (display-only; DOUBLE_CLICK exits via worker). These
// confirmed empirically that the firmware accepts up to 4 children in
// a single CREATE on 2.2.0.24.
static size_t buildSelectionMixedRows() {
  writeBackRow("<- Selection");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "B. Bottom corners (mix)");  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "D. Top corners (mix)");     gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "E. Halves L/R (mix)");      gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "F. Triple bottom (mix)");   gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "G. Title + corners (mix)"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "H. Quad 4 buttons (mix)");  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "I. Header + halves (mix)"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "J. Stacked strips (mix)");  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "K. Centered modal (mix)");  gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// Bucket 3 — edge-anchored geoms + selectability canaries. Probes the
// firmware's geom honoring on extreme rects (right column, bottom
// strip, top notification bar) and the eventCapture=1 flag (Q). R is
// the recommended title+list interactive pattern. S is the dual-pane
// CREATE probe — does the firmware accept two simultaneously CREATEd
// containers with distinct ContainerNames?
static size_t buildSelectionEdgesRows() {
  writeBackRow("<- Selection");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "L. Right column (text)");    gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "M. Right column (list)");    gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "N. Bottom-edge text");       gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "O. Bottom-edge buttons");    gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "P. Top notif bar (text)");   gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q. EvCap=1 (REJECT)");       gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "R. Title + Y/N list (mix)"); gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "S. REBUILD-text child probe"); gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// Forward decl: defined further down (after the geom-variant tables)
// but called from runSelectionPattern's compound-TextObject branches
// for the gTextViewExitFn slot in g2ShowMultiTextPage.
static void displayTestExitToPicker();

// Run a Selection pattern on the lens. List-widget variants stay in
// gInDisplayTest=true mode (any tap returns to picker via the
// dispatcher). Compound-TextObject variants fire g2ShowMultiTextPage
// with displayTestExitToPicker as the exit handler, so DOUBLE_CLICK
// dismisses to the picker.
static void runSelectionPattern(size_t idx) {
  static const char* rows[3];
  rows[0] = "<- Back";
  rows[1] = "Yes";
  rows[2] = "No";

  // Geoms used by multiple cases below — aliases of the validated
  // edge-anchored presets in System_G2_Protocol.h. Originally these
  // were inline magic numbers in this file; the tests confirmed them
  // empirically (2026-04-30) and they've been promoted to named
  // presets so production UI code can pull from the same vetted
  // library.
  static constexpr G2ContainerGeom kRightCol      = G2_GEOM_RIGHT_COL;
  static constexpr G2ContainerGeom kBottomEdge    = G2_GEOM_BOTTOM_BAR;
  static constexpr G2ContainerGeom kBottomEdgeL   = G2_GEOM_BOTTOM_BAR_L;
  static constexpr G2ContainerGeom kBottomEdgeR   = G2_GEOM_BOTTOM_BAR_R;
  // Title strip + list region for R (title + 2-row list)
  static constexpr G2ContainerGeom kTitleStrip    = G2_GEOM_STATUS_BAR;       // 576×40 at top
  static constexpr G2ContainerGeom kListBelowTitle = {   8,  56, 560, 224 };  // remaining area

  switch (idx) {
    case 0:
      // A. Footer — kept as a known-broken canary (40 px clips rows).
      gInDisplayTest = true;
      g2ShowListPage(rows, 3, G2_GEOM_FOOTER);
      DEBUG_G2F("[G2] Selection pattern: A footer list [BROKEN: clips]");
      return;
    case 1:
      gInDisplayTest = true;
      g2ShowListPage(rows, 3, G2_GEOM_LEFT_HALF);
      DEBUG_G2F("[G2] Selection pattern: A3 list @ LEFT_HALF (280x272)");
      return;
    case 2:
      gInDisplayTest = true;
      g2ShowListPage(rows, 3, G2_GEOM_SMALL);
      DEBUG_G2F("[G2] Selection pattern: A5 list @ SMALL (280x130 ctr)");
      return;
    case 3:
      gInDisplayTest = true;
      g2ShowListPage(rows, 3, G2_GEOM_LARGE);
      DEBUG_G2F("[G2] Selection pattern: C vertical list (3 rows @ LARGE)");
      return;

    case 4: {
      // B. Bottom-corner buttons.
      static const G2TextChildSpec children[] = {
        { "btnYes", "Yes", 1, G2_GEOM_QUAD_BL, false },
        { "btnNo",  "No",  2, G2_GEOM_QUAD_BR, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 2, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: B failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: B bottom corners (2 children @ QUAD_BL/BR)");
      }
      return;
    }
    case 5: {
      static const G2TextChildSpec children[] = {
        { "btnYes", "Yes", 1, G2_GEOM_QUAD_TL, false },
        { "btnNo",  "No",  2, G2_GEOM_QUAD_TR, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 2, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: D failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: D top corners (2 children @ QUAD_TL/TR)");
      }
      return;
    }
    case 6: {
      static const G2TextChildSpec children[] = {
        { "btnYes", "Yes", 1, G2_GEOM_LEFT_HALF,  false },
        { "btnNo",  "No",  2, G2_GEOM_RIGHT_HALF, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 2, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: E failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: E halves L/R (2 children @ LEFT/RIGHT_HALF)");
      }
      return;
    }
    case 7: {
      static constexpr G2ContainerGeom kThirdL  = {   0, 144, 190, 136 };
      static constexpr G2ContainerGeom kThirdM  = { 193, 144, 190, 136 };
      static constexpr G2ContainerGeom kThirdR  = { 386, 144, 190, 136 };
      static const G2TextChildSpec children[] = {
        { "btnYes", "Yes",   1, kThirdL, false },
        { "btnMay", "Maybe", 2, kThirdM, false },
        { "btnNo",  "No",    3, kThirdR, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 3, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: F failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: F triple bottom (3 children @ thirds)");
      }
      return;
    }
    case 8: {
      static const G2TextChildSpec children[] = {
        { "title",  "Save changes?", 1, G2_GEOM_TOP_HALF, false },
        { "btnYes", "Yes",           2, G2_GEOM_QUAD_BL,  false },
        { "btnNo",  "No",            3, G2_GEOM_QUAD_BR,  false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 3, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: G failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: G title + corners (3 children)");
      }
      return;
    }
    case 9: {
      static const G2TextChildSpec children[] = {
        { "btnYes", "Yes",    1, G2_GEOM_QUAD_TL, false },
        { "btnNo",  "No",     2, G2_GEOM_QUAD_TR, false },
        { "btnMay", "Maybe",  3, G2_GEOM_QUAD_BL, false },
        { "btnCan", "Cancel", 4, G2_GEOM_QUAD_BR, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 4, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: H failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: H quad 4 buttons (4 children @ all quads)");
      }
      return;
    }
    case 10: {
      static constexpr G2ContainerGeom kBtnL = {   8,  56, 280, 224 };
      static constexpr G2ContainerGeom kBtnR = { 288,  56, 280, 224 };
      static const G2TextChildSpec children[] = {
        { "title",  "Confirm action", 1, G2_GEOM_STATUS_BAR, false },
        { "btnYes", "Yes",            2, kBtnL,              false },
        { "btnNo",  "No",             3, kBtnR,              false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 3, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: I failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: I header + halves (3 children)");
      }
      return;
    }
    case 11: {
      static constexpr G2ContainerGeom kTop = {   8,   8, 560, 80 };
      static constexpr G2ContainerGeom kMid = {   8,  96, 560, 96 };
      static constexpr G2ContainerGeom kBot = {   8, 200, 560, 80 };
      static const G2TextChildSpec children[] = {
        { "topTxt", "Top strip",     1, kTop, false },
        { "midTxt", "Middle strip",  2, kMid, false },
        { "botTxt", "Bottom strip",  3, kBot, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 3, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: J failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: J stacked strips (3 children)");
      }
      return;
    }
    case 12: {
      static const G2TextChildSpec children[] = {
        { "modal", "OK", 1, G2_GEOM_CENTER_DOT, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 1, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: K failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: K centered modal (1 child @ CENTER_DOT)");
      }
      return;
    }

    case 13: {
      // L. Right column — single TextObject at the rightmost 130-px
      // strip. Lets us confirm whether the firmware honors a high-x
      // geom or maps everything to the lens center as the user has
      // observed for QUAD_BL/BR.
      static const G2TextChildSpec children[] = {
        { "rightCol", "Right side\nstatus", 1, kRightCol, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 1, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: L failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: L right column text (130x272 @ x=438)");
      }
      return;
    }
    case 14:
      gInDisplayTest = true;
      g2ShowListPage(rows, 3, kRightCol);
      DEBUG_G2F("[G2] Selection pattern: M list @ right column (130x272 @ x=438)");
      return;
    case 15: {
      // N. Bottom-edge text strip at y=248 (last 32 px of canvas).
      // If text-anchor-top-left is the firmware behaviour, content
      // will appear at the actual bottom of the lens here.
      static const G2TextChildSpec children[] = {
        { "btmEdge", "Connected — battery 67%", 1, kBottomEdge, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 1, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: N failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: N bottom-edge text (560x32 @ y=248)");
      }
      return;
    }
    case 16: {
      // O. Bottom-edge buttons at y=248. Two TextObjects, ~280×32 each.
      static const G2TextChildSpec children[] = {
        { "btnYes", "Yes", 1, kBottomEdgeL, false },
        { "btnNo",  "No",  2, kBottomEdgeR, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 2, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: O failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: O bottom-edge buttons (2 children @ y=248)");
      }
      return;
    }
    case 17: {
      // P. Top notification bar — single TextObject at STATUS_BAR
      // (576×40 @ y=8). A "thin status strip across the top of vision"
      // canary — the dual of N at the top.
      static const G2TextChildSpec children[] = {
        { "topBar", "WiFi up • 192.168.0.36 • 2 peers", 1, G2_GEOM_STATUS_BAR, false },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 1, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: P failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: P top notification bar (576x40 @ y=8)");
      }
      return;
    }
    case 18: {
      // Q. eventCapture=1 corners — same geometry as B but with
      // eventCapture=true on each TextObject. Verified 2026-04-30:
      // firmware rejects this CREATE with res=1
      // (CreateInvalidContainer); auto-recovery returns to root
      // hijack menu. Kept as a regression canary so we notice if
      // a future firmware drop ever starts accepting it.
      static const G2TextChildSpec children[] = {
        { "btnYes", "Yes", 1, G2_GEOM_QUAD_BL, true },
        { "btnNo",  "No",  2, G2_GEOM_QUAD_BR, true },
      };
      gInDisplayTest = true;
      if (!g2ShowMultiTextPage(children, 2, displayTestExitToPicker)) {
        DEBUG_G2F("[G2] Selection pattern: Q failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: Q eventCapture=1 corners — tap each button and watch log");
      }
      return;
    }
    case 19: {
      // R. Title + 2-row list — TextObject header above ListObject.
      // The list manages focus + scroll + CLICK natively, so this
      // page is FULLY interactive: scroll up/down to change focused
      // row, single tap to select. On tap the ListEvent CLICK reaches
      // the dispatcher with the row index — we just log "Hijack tap:
      // item N" here since we don't act on the choice. Use this
      // pattern for real prompts.
      static const char* listItems[] = { "<- Back", "Yes", "No" };
      static const G2TextChildSpec title = {
        "title", "Save changes?", 99, kTitleStrip, false
      };
      gInDisplayTest = true;
      if (!g2ShowMixedListText(listItems, 3, kListBelowTitle, title)) {
        DEBUG_G2F("[G2] Selection pattern: R failed to spawn worker");
        gInDisplayTest = false;
      } else {
        DEBUG_G2F("[G2] Selection pattern: R title + 2-row list (TextObject + ListObject)");
      }
      return;
    }

    case 20: {
      // S. REBUILD-text child probe — replaces the retired dual-pane
      // CREATE experiment (closed 2026-04-30, see G2_PROTOCOL.md:
      // ContainerName is not a multiplexer, second CREATE silently
      // dropped + SYSTEM_EXIT). Now answers the next question: in
      // a compound list+text container, does REBUILD-text targeting
      // the child name only update that child? Probe runs on a
      // worker task (synchronous ack-waits would deadlock the notify
      // task) — same pattern as the image probes.
      DEBUG_G2F("[G2] Selection pattern: S REBUILD-text child probe — spawning worker");
      if (!spawnImgProbeWorker(g2ProbeRebuildTextChild)) {
        DEBUG_G2F("[G2] Selection pattern: S failed to spawn worker");
      }
      // Don't set gInDisplayTest — the probe owns the lens for ~6 s
      // then hands control back via the worker's picker rebuild.
      return;
    }

    default:
      DEBUG_G2F("[G2] Selection pattern: idx=%u out of range", (unsigned)idx);
      return;
  }
}

// DISPLAY/TEXT and DISPLAY/LIST share the same kGeomVariants table;
// the dispatcher picks which g2Show* path to invoke based on level.
// Both builders just emit the geom labels — same content, different
// run target.
static size_t buildGeomVariantRows() {
  writeBackRow("<- Display");
  size_t row = 1;
  for (size_t i = 0; i < kGeomVariantCount && row < TEST_MAX_ROWS; i++) {
    strncpy(gRows[row], kGeomVariants[i].label, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// DISPLAY/COMBO: text + image layout tests, routed to existing Q-probes.
static size_t buildDisplayComboRows() {
  writeBackRow("<- Display");
  size_t row = 1;
  for (size_t i = 0; i < kComboTestCount && row < TEST_MAX_ROWS; i++) {
    strncpy(gRows[row], kComboTests[i].label, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// Exit handler for TEXT-widget display tests. Wired as the exitFn arg
// to g2ShowTextPage so any tap (USER_ACTIVITY post-grace, SysEvent
// CLICK / SCROLL / DOUBLE_CLICK, TextEvent CLICK) returns the user to
// the appropriate sub-picker. Without this, the TEXT widget had no
// exit route and left the firmware to timeout-tear-down at ~8 s with
// "Connection lost" on the lens.
//
// Routes back to whichever sub-level was active when the test
// launched: DISPLAY_TEXT for TEXT-widget tests, DISPLAY_LIST is
// unreachable here (LIST tests use the dispatcher path). Falls back
// to DISPLAY parent if state is somehow stale.
static void displayTestExitToPicker() {
  gInDisplayTest = false;
  size_t n;
  switch (gTestLevel) {
    case TEST_LEVEL_DISPLAY_TEXT:
    case TEST_LEVEL_DISPLAY_LIST:
      n = buildGeomVariantRows();
      break;
    case TEST_LEVEL_DISPLAY_SELECTION:
      n = buildSelectionRows();
      break;
    case TEST_LEVEL_DISPLAY_SELECT_LISTS:
      n = buildSelectionListsRows();
      break;
    case TEST_LEVEL_DISPLAY_SELECT_MIXED:
      n = buildSelectionMixedRows();
      break;
    case TEST_LEVEL_DISPLAY_SELECT_EDGES:
      n = buildSelectionEdgesRows();
      break;
    default:
      gTestLevel = TEST_LEVEL_DISPLAY;
      n = buildDisplayRows();
      break;
  }
  g2ShowListPage(gRowPtrs, n);
  DEBUG_G2F("[G2] Display test: exit → picker (level=%u rows=%u)",
            (unsigned)gTestLevel, (unsigned)n);
}

// Run a TEXT-widget test at the geom indexed by `idx` in kGeomVariants.
static void runDisplayTextVariant(size_t idx) {
  if (idx >= kGeomVariantCount) return;
  const DisplayVariant& v = kGeomVariants[idx];
  gInDisplayTest = true;
  // TEXT widget — wire the exit handler so tap/scroll dismisses to
  // the picker. tapFn=nullptr keeps the legacy "any tap exits" UX.
  g2ShowTextPage(kDisplaySampleText, v.geom, displayTestExitToPicker);
  DEBUG_G2F("[G2] Display test: TEXT '%s' (%ux%u @ %u,%u)",
            v.label, (unsigned)v.geom.w, (unsigned)v.geom.h,
            (unsigned)v.geom.x, (unsigned)v.geom.y);
}

// Run a LIST-widget test at the geom indexed by `idx` in kGeomVariants.
static void runDisplayListVariant(size_t idx) {
  if (idx >= kGeomVariantCount) return;
  const DisplayVariant& v = kGeomVariants[idx];
  gInDisplayTest = true;
  // Split kDisplaySampleText on newlines into rows so the LIST
  // widget has one row per line. First row is "<- Back" — tapping
  // it routes through the dispatcher's TEST_LEVEL_DISPLAY_LIST case
  // which checks gInDisplayTest and returns to the picker.
  static const char* rows[7];
  rows[0] = "<- Back";
  rows[1] = "Display geometry test";
  rows[2] = "Line 2: lorem ipsum";
  rows[3] = "Line 3: 0123456789";
  rows[4] = "Line 4: ABCDEFGHIJKLM";
  rows[5] = "Line 5: abcdefghijklm";
  rows[6] = "Line 6: short last";
  g2ShowListPage(rows, 7, v.geom);
  DEBUG_G2F("[G2] Display test: LIST '%s' (%ux%u @ %u,%u)",
            v.label, (unsigned)v.geom.w, (unsigned)v.geom.h,
            (unsigned)v.geom.x, (unsigned)v.geom.y);
}

// ACTIONS: list of one-shot diagnostic operations
static size_t buildActionsRows() {
  writeBackRow("<- Tests");
  size_t row = 1;
  for (size_t i = 0; i < kActionCount && row < TEST_MAX_ROWS; i++) {
    const char* lbl = kActions[i].dynLabel ? kActions[i].dynLabel()
                                           : kActions[i].label;
    if (!lbl) lbl = kActions[i].label;
    strncpy(gRows[row], lbl, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// CHARS: ASCII / Unicode picker.
static size_t buildCharsRows() {
  writeBackRow("<- Tests");
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "ASCII >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Unicode >>");
  gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// Helper: copy a CharTest array into rows (skipping the back row),
// paginated so a category that grows past TEST_VISIBLE_LIST_ROWS gets
// "<< Prev / Next >>" chrome instead of overflowing the single-fragment
// pb budget. Sub-menu organisation (ASCII vs Unicode/Blocks/...) is
// orthogonal — pagination kicks in within a category as a fallback.
static size_t buildCharTestList(const CharTest* tests, size_t count,
                                size_t& page, const char* backLabel) {
  writeBackRow(backLabel);
  G2Paginator p = g2PaginatorPrepare(count, TEST_VISIBLE_LIST_ROWS, page);
  gTsPageStartIdx = p.startIdx;

  size_t row = 1;
  for (size_t i = p.startIdx; i < p.endIdx && row < TEST_MAX_ROWS; i++) {
    strncpy(gRows[row], tests[i].label, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }

  row = g2PaginatorWriteChrome(p, page, row, TEST_MAX_ROWS,
                                &gRows[0][0], TEST_ROW_LEN, gRowPtrs);
  gTsPagePrevRow = p.prevRow;
  gTsPageNextRow = p.nextRow;
  return row;
}

static size_t buildCharsAsciiRows() {
  return buildCharTestList(kAsciiCharTests, kAsciiCharTestCount, gAsciiPage,
                           "<- Chars");
}

// Unicode picker — three category rows.
static size_t buildCharsUnicodeRows() {
  writeBackRow("<- Chars");
  size_t row = 1;
  for (size_t i = 0; i < kUnicodeCategoryCount && row < TEST_MAX_ROWS; i++) {
    snprintf(gRows[row], TEST_ROW_LEN, "%s >>", kUnicodeCategories[i].label);
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// Unicode list for the currently selected category.
static size_t buildCharsUnicodeListRows() {
  if (gUnicodeCategoryIdx >= kUnicodeCategoryCount) {
    writeBackRow("<- Unicode");
    return 1;
  }
  const UnicodeCategory& c = kUnicodeCategories[gUnicodeCategoryIdx];
  return buildCharTestList(c.tests, c.count, gUnicodeListPage, "<- Unicode");
}

// Render one character-set test on the lens. Two rows of content under
// the back row: a label header and the sample bytes themselves. Sample
// is truncated at TEST_ROW_LEN-1; UTF-8 is byte-safe for strncpy as long
// as we don't truncate mid-codepoint, which we avoid by keeping samples
// shorter than TEST_ROW_LEN.
static size_t buildCharsPayloadRowsFrom(const CharTest* tests, size_t count,
                                        size_t idx, const char* backLabel) {
  writeBackRow(backLabel);
  size_t row = 1;
  if (idx >= count) return row;
  snprintf(gRows[row], TEST_ROW_LEN, "[%s]", tests[idx].label);
  gRowPtrs[row] = gRows[row]; row++;
  strncpy(gRows[row], tests[idx].sample, TEST_ROW_LEN);
  gRows[row][TEST_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row]; row++;
  return row;
}

static size_t buildCharsAsciiPayloadRows(size_t idx) {
  return buildCharsPayloadRowsFrom(kAsciiCharTests, kAsciiCharTestCount, idx,
                                   "<- ASCII");
}

static size_t buildCharsUnicodePayloadRows(size_t idx) {
  if (gUnicodeCategoryIdx >= kUnicodeCategoryCount) {
    writeBackRow("<- Unicode");
    return 1;
  }
  const UnicodeCategory& c = kUnicodeCategories[gUnicodeCategoryIdx];
  return buildCharsPayloadRowsFrom(c.tests, c.count, idx, "<- Unicode");
}

// AI: list of front-pane AI panel pipeline variants
static size_t buildAIRows() {
  writeBackRow("<- Tests");
  size_t row = 1;
  for (size_t i = 0; i < kAIVariantCount && row < TEST_MAX_ROWS; i++) {
    strncpy(gRows[row], kAIVariants[i].label, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// BRACKETS: list of size-bracket payload tests
static size_t buildBracketsRows() {
  writeBackRow("<- Tests");
  size_t row = 1;
  for (size_t i = 0; i < kBracketCount && row < TEST_MAX_ROWS; i++) {
    strncpy(gRows[row], kBrackets[i].label, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// PAYLOAD: synthesized list whose pb body approximates `targetBytes`.
//
// Per-row pb cost: tag (1) + length varint (1, content < 128 B) + content.
// We aim per-row at (TEST_ROW_LEN - 1) for content; the rest of the
// encoded message (envelope wrapper, container metadata, item-count,
// etc.) adds ~30 bytes the loop accounts for via a fixed initial
// budget.
static size_t buildPayloadRows(size_t targetBytes) {
  writeBackRow("<- Transport");

  const size_t fixedOverhead  = 40;
  const size_t perRowOverhead = 3;
  const size_t maxRowContent  = TEST_ROW_LEN - 1;

  size_t remaining = (targetBytes > fixedOverhead)
                       ? targetBytes - fixedOverhead : 1;

  const size_t srcLen = sizeof(kSourceText) - 1;
  size_t srcOff = 0;
  size_t row = 1;

  while (remaining > 0 && row < TEST_MAX_ROWS) {
    size_t take = (remaining < maxRowContent + perRowOverhead)
                    ? remaining - perRowOverhead
                    : maxRowContent;
    if (take == 0 || take > maxRowContent) take = maxRowContent;
    if (take == 0) break;

    for (size_t i = 0; i < take; i++) {
      gRows[row][i] = kSourceText[(srcOff + i) % srcLen];
    }
    gRows[row][take] = '\0';
    gRowPtrs[row] = gRows[row];

    srcOff   += take;
    remaining = (remaining > take + perRowOverhead)
                  ? remaining - take - perRowOverhead : 0;
    row++;
  }

  // Final-row sentinel so on-glasses observers can confirm the tail
  // rendered. Firmware that reassembled correctly will show this last
  // entry; truncation hides it.
  if (row < TEST_MAX_ROWS) {
    snprintf(gRows[row], TEST_ROW_LEN, "[end %u B]", (unsigned)targetBytes);
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// Text-blob payload buffer — sized to the largest bracket. Lives in
// PSRAM so the 8 KB cap doesn't eat DRAM. g2ShowTextPage heap-copies
// the content, so this buffer is reused for every text bracket tap.
EXT_RAM_BSS_ATTR static char gTextBlob[8200];

// Build a multi-line text blob whose total length approximates
// `targetBytes` (the rendered string, before pb encoding). Lines are
// 64 chars wide so the firmware's TextObject auto-wrap behaviour is
// less in play — we want to exercise transport size, not wrapping.
// Returns the actual length written (including a leading "[N B test]"
// banner and trailing "[end N B]" sentinel so the operator can confirm
// the head and tail both made it through reassembly).
static size_t buildPayloadTextBlob(size_t targetBytes) {
  const size_t cap = sizeof(gTextBlob);
  if (cap == 0) return 0;
  gTextBlob[0] = '\0';

  size_t pos = 0;
  pos += (size_t)snprintf(gTextBlob + pos, cap - pos,
                          "[%u B test]\n", (unsigned)targetBytes);

  const size_t srcLen   = sizeof(kSourceText) - 1;
  const size_t lineLen  = 64;
  const char* sentinelFmt = "\n[end %u B]";
  // Reserve room for sentinel + NUL.
  const size_t sentinelMax = 32;
  const size_t hardCap     = (cap > sentinelMax) ? cap - sentinelMax : cap;

  size_t srcOff = 0;
  while (pos + lineLen + 1 < hardCap) {
    const size_t budgetLeft = (targetBytes > pos) ? (targetBytes - pos) : 0;
    if (budgetLeft <= 1) break;
    const size_t take = (budgetLeft < lineLen) ? budgetLeft : lineLen;
    for (size_t i = 0; i < take; i++) {
      gTextBlob[pos + i] = kSourceText[(srcOff + i) % srcLen];
    }
    pos += take;
    if (pos < hardCap) gTextBlob[pos++] = '\n';
    srcOff += take;
  }

  pos += (size_t)snprintf(gTextBlob + pos, cap - pos,
                          sentinelFmt, (unsigned)targetBytes);
  if (pos >= cap) pos = cap - 1;
  gTextBlob[pos] = '\0';
  return pos;
}

// Forward decl — defined in the dispatcher block below. The text-mode
// payload's exit handler returns the operator to the brackets list.
static void payloadTextExitToBrackets();

// -----------------------------------------------------------------------------
// Public — text-mode info dump (CLI direct invocation path)
// -----------------------------------------------------------------------------
void g2BuildTestSuiteInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;

  String s;
  s.reserve(256);
  s += "Test Suite (root)\n";
  s += "Tap a group, then a specific test.\n";
  s += "\n";
  s += "BLE Tests:\n";
  for (size_t i = 0; i < kActionCount; i++) {
    const char* lbl = kActions[i].dynLabel ? kActions[i].dynLabel()
                                           : kActions[i].label;
    if (!lbl) lbl = kActions[i].label;
    char line[48];
    snprintf(line, sizeof(line), " %s\n", lbl);
    s += line;
    if (s.length() > cap - 32) break;
  }
  s += "Transport Tests:\n";
  for (size_t i = 0; i < kBracketCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), " %s\n", kBrackets[i].label);
    s += line;
    if (s.length() > cap - 32) break;
  }
  s += "AI Panel Tests:\n";
  for (size_t i = 0; i < kAIVariantCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), " %s\n", kAIVariants[i].label);
    s += line;
    if (s.length() > cap - 32) break;
  }
  s += "Character Tests (ASCII):\n";
  for (size_t i = 0; i < kAsciiCharTestCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), " %s\n", kAsciiCharTests[i].label);
    s += line;
    if (s.length() > cap - 32) break;
  }
  for (size_t cat = 0; cat < kUnicodeCategoryCount; cat++) {
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Character Tests (Unicode/%s):\n",
             kUnicodeCategories[cat].label);
    s += hdr;
    const UnicodeCategory& c = kUnicodeCategories[cat];
    for (size_t i = 0; i < c.count; i++) {
      char line[48];
      snprintf(line, sizeof(line), " %s\n", c.tests[i].label);
      s += line;
      if (s.length() > cap - 32) break;
    }
    if (s.length() > cap - 32) break;
  }
  s += "Display Tests (TEXT widget / LIST widget — same geom variants):\n";
  for (size_t i = 0; i < kGeomVariantCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), " %s\n", kGeomVariants[i].label);
    s += line;
    if (s.length() > cap - 32) break;
  }
  s += "Display Tests (Combo: text + image):\n";
  for (size_t i = 0; i < kComboTestCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), " %s\n", kComboTests[i].label);
    s += line;
    if (s.length() > cap - 32) break;
  }
  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

// -----------------------------------------------------------------------------
// Public — render the root menu on the lens
// -----------------------------------------------------------------------------
void g2ShowTestSuiteMenu() {
  // Always land on root when entering from MAIN.
  gTestLevel = TEST_LEVEL_ROOT;
  size_t n = buildRootRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_TESTS);
    DEBUG_G2F("[G2] Test suite root shown (rows=%u)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Test suite menu: g2ShowListPage failed");
  }
}

// Text-widget exit handler for the size-bracket payload tests. Wired
// as g2ShowTextPage's exitFn so the user's dismiss gesture (double-
// click on the lens, or DOUBLE_CLICK SysEvent from the ring) drops
// straight back to the brackets list rather than the main hijack menu.
static void payloadTextExitToBrackets() {
  gTestLevel = TEST_LEVEL_BRACKETS;
  size_t n = buildBracketsRows();
  g2ShowListPage(gRowPtrs, n);
}

// -----------------------------------------------------------------------------
// Public — tap dispatcher (level-aware)
// -----------------------------------------------------------------------------
void g2TestSuiteHandleTap(uint32_t idx) {
  switch (gTestLevel) {

    case TEST_LEVEL_ROOT: {
      if (idx == 0) {
        // <- Back: out of Test Suite, back to root hijack menu.
        g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
        extern void g2RedrawHijackMainMenu();
        g2RedrawHijackMainMenu();
        return;
      }
      if (idx == 1) {
        gTestLevel = TEST_LEVEL_ACTIONS;
        size_t n = buildActionsRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: BLE Tests sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      if (idx == 2) {
        gTestLevel = TEST_LEVEL_BRACKETS;
        size_t n = buildBracketsRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: Transport Tests sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      if (idx == 3) {
        gTestLevel = TEST_LEVEL_AI;
        size_t n = buildAIRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: AI Panel Tests sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      if (idx == 4) {
        gTestLevel = TEST_LEVEL_CHARS;
        size_t n = buildCharsRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: Character Tests sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      if (idx == 5) {
        gTestLevel = TEST_LEVEL_DISPLAY;
        gInDisplayTest = false;   // entering picker, not a test
        size_t n = buildDisplayRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: Display Tests sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      if (idx == 6) {
        gTestLevel = TEST_LEVEL_IMAGE;
        size_t n = buildImageRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: Image Tests sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      DEBUG_G2F("[G2] Test suite root: tap idx=%u out of range", (unsigned)idx);
      return;
    }

    case TEST_LEVEL_DISPLAY: {
      // Parent picker: TEXT widget / LIST widget / Combo. No tests fire
      // from here — each row drills into a sub-list whose dispatcher
      // case actually runs the tests.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_ROOT;
        size_t n = buildRootRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      size_t n = 0;
      switch (idx) {
        case 1:
          gTestLevel = TEST_LEVEL_DISPLAY_TEXT;
          gInDisplayTest = false;
          n = buildGeomVariantRows();
          break;
        case 2:
          gTestLevel = TEST_LEVEL_DISPLAY_LIST;
          gInDisplayTest = false;
          n = buildGeomVariantRows();
          break;
        case 3:
          gTestLevel = TEST_LEVEL_DISPLAY_COMBO;
          n = buildDisplayComboRows();
          break;
        case 4:
          gTestLevel = TEST_LEVEL_DISPLAY_SELECTION;
          gInDisplayTest = false;
          n = buildSelectionRows();
          break;
        default:
          DEBUG_G2F("[G2] Test suite DISPLAY: tap idx=%u out of range",
                    (unsigned)idx);
          return;
      }
      g2ShowListPage(gRowPtrs, n);
      return;
    }

    case TEST_LEVEL_DISPLAY_TEXT: {
      // Two sub-states under this level:
      //   gInDisplayTest=false → user is on the geom-variant picker.
      //                          idx=0 → back to DISPLAY parent.
      //                          idx 1..N → drill into a TEXT test.
      //   gInDisplayTest=true  → not reachable here; TEXT tests exit
      //                          via displayTestExitToPicker() which
      //                          rebuilds the picker and clears the
      //                          flag before any tap arrives.
      if (idx == 0) {
        if (gInDisplayTest) {
          displayTestExitToPicker();
        } else {
          gTestLevel = TEST_LEVEL_DISPLAY;
          size_t n = buildDisplayRows();
          g2ShowListPage(gRowPtrs, n);
        }
        return;
      }
      if (gInDisplayTest) return;
      const size_t pos = idx - 1;
      if (pos >= kGeomVariantCount) {
        DEBUG_G2F("[G2] Test suite DISPLAY/TEXT: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      runDisplayTextVariant(pos);
      return;
    }

    case TEST_LEVEL_DISPLAY_LIST: {
      // Two sub-states under this level (mirrors DISPLAY_TEXT):
      //   gInDisplayTest=false → user is on the geom-variant picker.
      //                          idx=0 → back to DISPLAY parent.
      //                          idx 1..N → drill into a LIST test.
      //   gInDisplayTest=true  → a LIST display test is on screen.
      //                          idx=0 → back to picker.
      //                          other idx → no-op (the rows are
      //                                       sample content, not
      //                                       actions).
      if (idx == 0) {
        if (gInDisplayTest) {
          displayTestExitToPicker();
        } else {
          gTestLevel = TEST_LEVEL_DISPLAY;
          size_t n = buildDisplayRows();
          g2ShowListPage(gRowPtrs, n);
        }
        return;
      }
      if (gInDisplayTest) {
        // Tap on a sample-content row inside a LIST test — no-op.
        return;
      }
      const size_t pos = idx - 1;
      if (pos >= kGeomVariantCount) {
        DEBUG_G2F("[G2] Test suite DISPLAY/LIST: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      runDisplayListVariant(pos);
      return;
    }

    case TEST_LEVEL_DISPLAY_COMBO: {
      // Combo (Text + Image): each row routes to a Q-probe that
      // exercises a mixed CreateStartUpPageContainer (list + image)
      // at a specific spatial layout. Probes are async — spawn the
      // worker; the user keeps the menu visible until results land.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_DISPLAY;
        size_t n = buildDisplayRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      const size_t pos = idx - 1;
      if (pos >= kComboTestCount) {
        DEBUG_G2F("[G2] Test suite DISPLAY/COMBO: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      DEBUG_G2F("[G2] Display combo: '%s' → spawning probe worker",
                kComboTests[pos].label);
      if (!spawnImgProbeWorker(kComboTests[pos].probe)) {
        size_t n = buildDisplayComboRows();
        g2ShowListPage(gRowPtrs, n);
      }
      return;
    }

    case TEST_LEVEL_DISPLAY_SELECTION: {
      // Top picker — 3 sub-buckets (Lists / Compound / Edges). No
      // patterns fire from here; each row drills into a sub-list whose
      // dispatcher case actually runs the tests.
      //
      // Splitting up was forced by firmware 2.2.0.24's inability to
      // reassemble 3-fragment CREATE-list payloads — at 21+ rows the
      // protobuf overflowed two MTU fragments and the container never
      // primed.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_DISPLAY;
        size_t n = buildDisplayRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      size_t n = 0;
      switch (idx) {
        case 1:
          gTestLevel = TEST_LEVEL_DISPLAY_SELECT_LISTS;
          gInDisplayTest = false;
          n = buildSelectionListsRows();
          break;
        case 2:
          gTestLevel = TEST_LEVEL_DISPLAY_SELECT_MIXED;
          gInDisplayTest = false;
          n = buildSelectionMixedRows();
          break;
        case 3:
          gTestLevel = TEST_LEVEL_DISPLAY_SELECT_EDGES;
          gInDisplayTest = false;
          n = buildSelectionEdgesRows();
          break;
        default:
          DEBUG_G2F("[G2] Test suite SELECTION: tap idx=%u out of range",
                    (unsigned)idx);
          return;
      }
      g2ShowListPage(gRowPtrs, n);
      return;
    }

    case TEST_LEVEL_DISPLAY_SELECT_LISTS: {
      // Bucket 1 — native ListObject geom variants (4 entries).
      // Local idx 1..4 maps directly to flat runSelectionPattern
      // indices 0..3 (offset = -1).
      if (idx == 0) {
        if (gInDisplayTest) {
          displayTestExitToPicker();
        } else {
          gTestLevel = TEST_LEVEL_DISPLAY_SELECTION;
          size_t n = buildSelectionRows();
          g2ShowListPage(gRowPtrs, n);
        }
        return;
      }
      if (gInDisplayTest) {
        // Tap on Yes/No while pattern is shown — exit demo.
        displayTestExitToPicker();
        return;
      }
      const size_t local = idx - 1;
      if (local >= 4) {
        DEBUG_G2F("[G2] Test suite SELECT/LISTS: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      runSelectionPattern(local);  // flat 0..3
      return;
    }

    case TEST_LEVEL_DISPLAY_SELECT_MIXED: {
      // Bucket 2 — compound TextObject CREATEs (9 entries).
      // Local idx 1..9 maps to flat runSelectionPattern indices
      // 4..12 (offset = +3).
      if (idx == 0) {
        if (gInDisplayTest) {
          displayTestExitToPicker();
        } else {
          gTestLevel = TEST_LEVEL_DISPLAY_SELECTION;
          size_t n = buildSelectionRows();
          g2ShowListPage(gRowPtrs, n);
        }
        return;
      }
      if (gInDisplayTest) {
        displayTestExitToPicker();
        return;
      }
      const size_t local = idx - 1;
      if (local >= 9) {
        DEBUG_G2F("[G2] Test suite SELECT/MIXED: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      runSelectionPattern(local + 4);  // flat 4..12
      return;
    }

    case TEST_LEVEL_DISPLAY_SELECT_EDGES: {
      // Bucket 3 — edge-anchored geoms + selectability canaries
      // (8 entries: L–S). Local idx 1..8 maps to flat
      // runSelectionPattern indices 13..20 (offset = +12).
      if (idx == 0) {
        if (gInDisplayTest) {
          displayTestExitToPicker();
        } else {
          gTestLevel = TEST_LEVEL_DISPLAY_SELECTION;
          size_t n = buildSelectionRows();
          g2ShowListPage(gRowPtrs, n);
        }
        return;
      }
      if (gInDisplayTest) {
        displayTestExitToPicker();
        return;
      }
      const size_t local = idx - 1;
      if (local >= 8) {
        DEBUG_G2F("[G2] Test suite SELECT/EDGES: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      runSelectionPattern(local + 13);  // flat 13..20
      return;
    }

    case TEST_LEVEL_IMAGE: {
      // Parent router: 3 sub-levels (Confirmed, Static, Streaming).
      // Tapping anything else is a misuse — we don't fire probes from
      // here.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_ROOT;
        size_t n = buildRootRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      size_t n = 0;
      switch (idx) {
        case 1: gTestLevel = TEST_LEVEL_IMAGE_CONFIRMED; n = buildImageConfirmedRows(); break;
        case 2: gTestLevel = TEST_LEVEL_IMAGE_STATIC;    n = buildImageStaticRows();    break;
        case 3: gTestLevel = TEST_LEVEL_IMAGE_STREAMING; n = buildImageStreamingRows(); break;
        default:
          DEBUG_G2F("[G2] Test suite IMAGE: tap idx=%u out of range",
                    (unsigned)idx);
          return;
      }
      g2ShowListPage(gRowPtrs, n);
      return;
    }

    case TEST_LEVEL_IMAGE_CONFIRMED: {
      // Confirmed/Diagnostic: Doc, Q4 canary, Q6, Q6b.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_IMAGE;
        size_t n = buildImageRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      ImgProbeFn fn = nullptr;
      switch (idx) {
        case 1: fn = g2ProbeImageDocSummary;         break;
        case 2: fn = g2ProbeImageQ4Lifecycle;        break;
        case 3: fn = g2ProbeImageQ6BmpMultiFragment; break;
        case 4: fn = g2ProbeImageQ6bBmpTapDismiss;   break;
        default:
          DEBUG_G2F("[G2] Test suite IMAGE/Confirmed: tap idx=%u out of range",
                    (unsigned)idx);
          return;
      }
      DEBUG_G2F("[G2] Image probe (Confirmed) idx=%u → spawning worker", (unsigned)idx);
      if (!spawnImgProbeWorker(fn)) {
        size_t n = buildImageConfirmedRows();
        g2ShowListPage(gRowPtrs, n);
      }
      return;
    }

    case TEST_LEVEL_IMAGE_STATIC: {
      // Static: Q9, QGlizzy, Q12 (full-display 4-tile).
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_IMAGE;
        size_t n = buildImageRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      ImgProbeFn fn = nullptr;
      switch (idx) {
        case 1: fn = g2ProbeImageQ9FrameBuilder; break;
        case 2: fn = g2ProbeImageQGlizzy;        break;
        case 3: fn = g2ProbeImageQ12FullScreen;  break;
        case 4: fn = g2ProbeImageQ19SmallSolo;   break;
        default:
          DEBUG_G2F("[G2] Test suite IMAGE/Static: tap idx=%u out of range",
                    (unsigned)idx);
          return;
      }
      DEBUG_G2F("[G2] Image probe (Static) idx=%u → spawning worker", (unsigned)idx);
      if (!spawnImgProbeWorker(fn)) {
        size_t n = buildImageStaticRows();
        g2ShowListPage(gRowPtrs, n);
      }
      return;
    }

    case TEST_LEVEL_IMAGE_STREAMING: {
      // Streaming: Q11 first, Q10 second. Run order matters per the
      // sub-list comment in buildImageStreamingRows.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_IMAGE;
        size_t n = buildImageRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      ImgProbeFn fn = nullptr;
      switch (idx) {
        case 1: fn = g2ProbeImageQ11SimpleSwap;        break;
        case 2: fn = g2ProbeImageQ10ClearThenPush;     break;
        case 3: fn = g2ProbeImageQ13LiveTile;          break;
        case 4: fn = g2ProbeImageQ14LiveText;          break;
        case 5: fn = g2ProbeImageQ15LeftArm;           break;
        case 6: fn = g2ProbeImageQ16MixedSideBySide;   break;
        case 7: fn = g2ProbeImageQ17MixedOverlap;      break;
        case 8: fn = g2ProbeImageQ18MixedIcon;         break;
        default:
          DEBUG_G2F("[G2] Test suite IMAGE/Streaming: tap idx=%u out of range",
                    (unsigned)idx);
          return;
      }
      DEBUG_G2F("[G2] Image probe (Streaming) idx=%u → spawning worker", (unsigned)idx);
      if (!spawnImgProbeWorker(fn)) {
        size_t n = buildImageStreamingRows();
        g2ShowListPage(gRowPtrs, n);
      }
      return;
    }

    case TEST_LEVEL_CHARS: {
      // ASCII / Unicode picker.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_ROOT;
        size_t n = buildRootRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      if (idx == 1) {
        gTestLevel  = TEST_LEVEL_CHARS_ASCII;
        gAsciiPage  = 0;   // entering a list always lands on page 0
        size_t n = buildCharsAsciiRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: Char Tests (ASCII) sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      if (idx == 2) {
        gTestLevel = TEST_LEVEL_CHARS_UNICODE;
        size_t n = buildCharsUnicodeRows();
        g2ShowListPage(gRowPtrs, n);
        DEBUG_G2F("[G2] Test suite: Char Tests (Unicode) sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      DEBUG_G2F("[G2] Test suite CHARS: tap idx=%u out of range",
                (unsigned)idx);
      return;
    }

    case TEST_LEVEL_CHARS_ASCII: {
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_CHARS;
        size_t n = buildCharsRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      // Pagination chrome — same row-index detection as Settings.
      if ((int)idx == gTsPagePrevRow) {
        if (gAsciiPage > 0) gAsciiPage--;
        DEBUG_G2F("[G2] Test suite: ASCII prev page → %u", (unsigned)gAsciiPage);
        size_t n = buildCharsAsciiRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      if ((int)idx == gTsPageNextRow) {
        gAsciiPage++;
        DEBUG_G2F("[G2] Test suite: ASCII next page → %u", (unsigned)gAsciiPage);
        size_t n = buildCharsAsciiRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      // Item taps: convert page-local position into the full-array index.
      const size_t pos = idx - 1;
      const size_t actualIdx = gTsPageStartIdx + pos;
      if (actualIdx >= kAsciiCharTestCount) {
        DEBUG_G2F("[G2] Test suite CHARS_ASCII: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      DEBUG_G2F("[G2] Test suite: ASCII char test '%s'",
                kAsciiCharTests[actualIdx].label);
      gTestLevel = TEST_LEVEL_CHARS_ASCII_PAYLOAD;
      size_t n = buildCharsAsciiPayloadRows(actualIdx);
      if (!g2ShowListPage(gRowPtrs, n)) {
        DEBUG_G2F("[G2] Test suite: g2ShowListPage failed for ASCII '%s'",
                  kAsciiCharTests[actualIdx].label);
        gTestLevel = TEST_LEVEL_CHARS_ASCII;
        size_t cn = buildCharsAsciiRows();
        g2ShowListPage(gRowPtrs, cn);
      }
      return;
    }

    case TEST_LEVEL_CHARS_ASCII_PAYLOAD: {
      if (idx == 0) {
        // Back returns to ASCII list, not the picker.
        gTestLevel = TEST_LEVEL_CHARS_ASCII;
        size_t n = buildCharsAsciiRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      return;
    }

    case TEST_LEVEL_CHARS_UNICODE: {
      // Unicode category picker.
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_CHARS;
        size_t n = buildCharsRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      const size_t pos = idx - 1;
      if (pos >= kUnicodeCategoryCount) {
        DEBUG_G2F("[G2] Test suite CHARS_UNICODE: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      gUnicodeCategoryIdx = pos;
      gUnicodeListPage    = 0;   // category change → land on page 0
      gTestLevel = TEST_LEVEL_CHARS_UNICODE_LIST;
      size_t n = buildCharsUnicodeListRows();
      DEBUG_G2F("[G2] Test suite: Unicode category '%s' (rows=%u)",
                kUnicodeCategories[pos].label, (unsigned)n);
      if (!g2ShowListPage(gRowPtrs, n)) {
        DEBUG_G2F("[G2] Test suite: g2ShowListPage failed for Unicode category '%s'",
                  kUnicodeCategories[pos].label);
        gTestLevel = TEST_LEVEL_CHARS_UNICODE;
        size_t cn = buildCharsUnicodeRows();
        g2ShowListPage(gRowPtrs, cn);
      }
      return;
    }

    case TEST_LEVEL_CHARS_UNICODE_LIST: {
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_CHARS_UNICODE;
        size_t n = buildCharsUnicodeRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      if (gUnicodeCategoryIdx >= kUnicodeCategoryCount) {
        gTestLevel = TEST_LEVEL_CHARS_UNICODE;
        size_t n = buildCharsUnicodeRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      // Pagination chrome.
      if ((int)idx == gTsPagePrevRow) {
        if (gUnicodeListPage > 0) gUnicodeListPage--;
        DEBUG_G2F("[G2] Test suite: Unicode prev page → %u",
                  (unsigned)gUnicodeListPage);
        size_t n = buildCharsUnicodeListRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      if ((int)idx == gTsPageNextRow) {
        gUnicodeListPage++;
        DEBUG_G2F("[G2] Test suite: Unicode next page → %u",
                  (unsigned)gUnicodeListPage);
        size_t n = buildCharsUnicodeListRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      const UnicodeCategory& c = kUnicodeCategories[gUnicodeCategoryIdx];
      const size_t pos       = idx - 1;
      const size_t actualIdx = gTsPageStartIdx + pos;
      if (actualIdx >= c.count) {
        DEBUG_G2F("[G2] Test suite CHARS_UNICODE_LIST: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      DEBUG_G2F("[G2] Test suite: Unicode test '%s/%s'",
                c.label, c.tests[actualIdx].label);
      gTestLevel = TEST_LEVEL_CHARS_UNICODE_PAYLOAD;
      size_t n = buildCharsUnicodePayloadRows(actualIdx);
      if (!g2ShowListPage(gRowPtrs, n)) {
        DEBUG_G2F("[G2] Test suite: g2ShowListPage failed for Unicode '%s'",
                  c.tests[actualIdx].label);
        gTestLevel = TEST_LEVEL_CHARS_UNICODE_LIST;
        size_t cn = buildCharsUnicodeListRows();
        g2ShowListPage(gRowPtrs, cn);
      }
      return;
    }

    case TEST_LEVEL_CHARS_UNICODE_PAYLOAD: {
      if (idx == 0) {
        // Back returns to the category's list, not the picker.
        gTestLevel = TEST_LEVEL_CHARS_UNICODE_LIST;
        size_t n = buildCharsUnicodeListRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      return;
    }

    case TEST_LEVEL_AI: {
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_ROOT;
        size_t n = buildRootRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      const size_t pos = idx - 1;
      if (pos >= kAIVariantCount) {
        DEBUG_G2F("[G2] Test suite AI: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      DEBUG_G2F("[G2] Test suite: AI variant '%s'", kAIVariants[pos].label);
      if (kAIVariants[pos].handler) kAIVariants[pos].handler();
      return;
    }

    case TEST_LEVEL_ACTIONS: {
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_ROOT;
        size_t n = buildRootRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      const size_t pos = idx - 1;
      if (pos >= kActionCount) {
        DEBUG_G2F("[G2] Test suite ACTIONS: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      DEBUG_G2F("[G2] Test suite: action '%s'", kActions[pos].label);
      if (kActions[pos].handler) kActions[pos].handler();
      // Re-render the page so state-aware entries (dynLabel) reflect the
      // new value immediately without making the tester back out.
      if (kActions[pos].dynLabel) {
        size_t n = buildActionsRows();
        g2ShowListPage(gRowPtrs, n);
      }
      return;
    }

    case TEST_LEVEL_BRACKETS: {
      if (idx == 0) {
        gTestLevel = TEST_LEVEL_ROOT;
        size_t n = buildRootRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      const size_t pos = idx - 1;
      if (pos >= kBracketCount) {
        DEBUG_G2F("[G2] Test suite BRACKETS: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      const TestBracket& b = kBrackets[pos];
      if (b.kind == TBK_TEXT_REBUILD) {
        // CREATE-text with a 1-fragment placeholder, then REBUILD-text
        // the actual blob. Logs report whether the REBUILD acks at
        // sizes where the equivalent (T) bracket's CREATE-text fails.
        const size_t blobLen = buildPayloadTextBlob(b.bytes);
        DEBUG_G2F("[G2] Test suite: rendering %s (text-rebuild probe, "
                  "blob=%u B, target=%u B)",
                  b.label, (unsigned)blobLen, (unsigned)b.bytes);
        if (!g2ShowTextPageRebuildProbe("Loading...", gTextBlob,
                                        G2_GEOM_LARGE,
                                        &payloadTextExitToBrackets)) {
          DEBUG_G2F("[G2] Test suite: g2ShowTextPageRebuildProbe failed for %s",
                    b.label);
          gTestLevel = TEST_LEVEL_BRACKETS;
          size_t bn = buildBracketsRows();
          g2ShowListPage(gRowPtrs, bn);
        }
        return;
      }
      if (b.kind == TBK_TEXT || b.kind == TBK_TEXT_SLOW) {
        // Text-widget bracket: build a multi-line content string and
        // ship it via g2ShowTextPage (single TextObject, content = the
        // blob). g2ShowTextPage heap-copies the string so gTextBlob can
        // be reused on the next tap. The exit handler returns to the
        // brackets list — no TEST_LEVEL_PAYLOAD transition needed
        // because text-widget exit is gesture-based, not row-tap.
        //
        // T-slow variant: arm a 100 ms inter-fragment delay for the
        // next sendPbFragmented call (which will be the CREATE-text
        // for this page). The override is one-shot and consumed by
        // the burst that immediately follows g2ShowTextPage's worker
        // SHUTDOWN, so it doesn't bleed into post-dismiss page-swap.
        const size_t blobLen = buildPayloadTextBlob(b.bytes);
        if (b.kind == TBK_TEXT_SLOW) {
          g2DebugSetNextBurstFragDelay(100);
        }
        DEBUG_G2F("[G2] Test suite: rendering %s (text-widget%s, "
                  "blob=%u B, target=%u B)",
                  b.label,
                  b.kind == TBK_TEXT_SLOW ? ", 100 ms inter-frag" : "",
                  (unsigned)blobLen, (unsigned)b.bytes);
        if (!g2ShowTextPage(gTextBlob, G2_GEOM_LARGE,
                            &payloadTextExitToBrackets, nullptr)) {
          DEBUG_G2F("[G2] Test suite: g2ShowTextPage failed for %s",
                    b.label);
          gTestLevel = TEST_LEVEL_BRACKETS;
          size_t bn = buildBracketsRows();
          g2ShowListPage(gRowPtrs, bn);
        }
        return;
      }
      // Default: list-widget bracket (legacy path).
      size_t n = buildPayloadRows(b.bytes);
      DEBUG_G2F("[G2] Test suite: rendering %s (list-widget, "
                "rows=%u, target=%u B)",
                b.label, (unsigned)n, (unsigned)b.bytes);
      gTestLevel = TEST_LEVEL_PAYLOAD;
      if (!g2ShowListPage(gRowPtrs, n)) {
        DEBUG_G2F("[G2] Test suite: g2ShowListPage failed for %s", b.label);
        // Fall back to brackets menu so the user has something to tap.
        gTestLevel = TEST_LEVEL_BRACKETS;
        size_t bn = buildBracketsRows();
        g2ShowListPage(gRowPtrs, bn);
      }
      return;
    }

    case TEST_LEVEL_PAYLOAD: {
      if (idx == 0) {
        // <- Back from a synthetic payload returns to BRACKETS menu so
        // the user can pick another size without bouncing all the way
        // to root. (Two-tap return to root.)
        gTestLevel = TEST_LEVEL_BRACKETS;
        size_t n = buildBracketsRows();
        g2ShowListPage(gRowPtrs, n);
        return;
      }
      // Other taps on the payload page are no-ops — the rows are pure
      // content with no actions.
      DEBUG_G2F("[G2] Test suite PAYLOAD: tap idx=%u (read-only payload)",
                (unsigned)idx);
      return;
    }
  }
}

// -----------------------------------------------------------------------------
// Action handlers
// -----------------------------------------------------------------------------

// Force-dismiss the front-pane EvenAI card. Card normally auto-
// dismisses after ~10 s but a stuck card (firmware-side) blocks
// subsequent CTRL ENTER calls with errorCode=7. Sends CTRL EXIT,
// which is a no-op if no card is active. Single envelope — runs on
// the BLE notify task without a worker.
static void actionHideAICard() {
  const bool ok = g2HideEvenAICard();
  DEBUG_G2F("[G2] Hide AI Card: %s", ok ? "sent CTRL EXIT" : "send failed");
}

// Toggle the G2 → ESP-SR mic feed. Logs the new state plus AFE ring
// depth + cumulative overrun count so a tester can correlate "I just
// turned this on" with "samples are arriving" without leaving the
// glasses. Doesn't redraw the menu — log-only.
static void actionToggleMicFeed() {
  const bool wasOn = g2MicAfeFeedIsActive();
  const bool turnOn = !wasOn;
  const bool ok = g2MicSetAfeFeedActive(turnOn);
  DEBUG_G2F("[G2] Toggle Mic Feed: %s → %s (%s); ring=%u overruns=%u",
            wasOn  ? "ON"  : "OFF",
            turnOn ? "ON"  : "OFF",
            ok     ? "ok"  : "FAILED",
            (unsigned)g2MicAfeRingDepth(),
            (unsigned)g2MicAfeOverrunCount());
}

// Toggle the G2 auto-reconnect-at-boot flag. Backed by the same
// gBlePeerData[BLE_PEER_G2_GLASSES].autoConnect that `bleautoconnect
// g2-glasses [on|off]` writes — setSetting() persists it to NVS, so
// the choice survives reboot. Lets a tester flip auto-reconnect from
// the lens (e.g. before flashing, or to stop the boot-time scan when
// debugging another peer) without a host CLI.
static void actionToggleG2AutoReconnect() {
  BlePeerData& d = gBlePeerData[BLE_PEER_G2_GLASSES];
  const bool wasOn = d.autoConnect;
  setSetting(d.autoConnect, !wasOn);
  DEBUG_G2F("[G2] AutoConnect: %s -> %s",
            wasOn ? "ON" : "OFF",
            d.autoConnect ? "ON" : "OFF");
}

// Buffer is static so the returned pointer stays valid until the next
// buildActionsRows() copies it into gRows. buildActionsRows is the only
// caller, so a single shared buffer is fine.
static const char* labelG2AutoReconnect() {
  static char buf[TEST_ROW_LEN];
  const bool on = gBlePeerData[BLE_PEER_G2_GLASSES].autoConnect;
  snprintf(buf, sizeof(buf), "G2 AutoConnect: %s", on ? "ON" : "OFF");
  return buf;
}

// Snapshot heap state to serial. Pure log — no BLE I/O. Useful as a
// canary before/after a heavy probe to bracket leaks. Includes free
// + min-ever-free so a single tap shows both current pressure and
// the worst point this session has hit.
static void actionHeapSnapshot() {
  const uint32_t freeNow = ESP.getFreeHeap();
  const uint32_t minEver = ESP.getMinFreeHeap();
  DEBUG_G2F("[G2] Heap: free=%u B  min-ever=%u B  used-since-min=%u B",
            (unsigned)freeNow, (unsigned)minEver,
            (unsigned)(freeNow > minEver ? freeNow - minEver : 0));
}

// Dump the consolidated lens-state struct to serial. Useful when
// debugging hijack lifecycle or container-state desync without
// running a CLI command from the host. Pure log — no side effects.
static void actionLensStateDump() {
  const G2LensState s = g2LensGetState();
  DEBUG_G2F("[G2] Lens: hijack=%d page=%u containerReady=%d isList=%d "
            "widgetId=%u overlay=%u",
            (int)s.hijackActive, (unsigned)s.hijackPage,
            (int)s.containerReady, (int)s.containerIsList,
            (unsigned)s.containerWidgetId, (unsigned)s.overlayKind);
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
