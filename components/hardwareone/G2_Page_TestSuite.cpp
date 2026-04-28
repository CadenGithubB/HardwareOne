// =============================================================================
// G2 glasses — "Tests" hijack page implementation
// =============================================================================
// Multi-level navigation so the root never becomes a giant scroll-fest:
//
//   ROOT:
//     <- Back
//     BLE Actions >>
//     Transport Tests >>
//     AI Panel Tests >>
//     Character Tests >>
//
//   ACTIONS (drill from ROOT):
//     <- Back
//     Reconnect Ring
//     (room for future BLE-recovery actions)
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

#include "Optional_EvenG2.h"
#include "Optional_EvenG2_Ring.h"   // g2RingDisconnect/Connect
#include "System_Debug.h"
#include "G2_Page_Common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
struct TestBracket {
  const char* label;
  size_t      bytes;
};

static const TestBracket kBrackets[] = {
  // Single-fragment territory — should always pass.
  { "Send 100 B",   100 },
  { "Send 250 B",   250 },
  // Multi-fragment territory — exercises sendPbFragmented.
  { "Send 500 B",   500 },
  { "Send 1 KB",   1024 },
  { "Send 2 KB",   2048 },
  // 4 KB / 6 KB / 8 KB removed — they overflowed the row buffer needed
  // by the AI panel sub-menu and the heaviest brackets weren't yielding
  // new information vs. 2 KB. Restore if we re-enter that territory.
};
static constexpr size_t kBracketCount =
  sizeof(kBrackets) / sizeof(kBrackets[0]);

// -----------------------------------------------------------------------------
// BLE / diagnostic actions
// -----------------------------------------------------------------------------
struct TestAction {
  const char* label;
  void (*handler)();
};

static void actionReconnectRing();

static const TestAction kActions[] = {
  // Force-disconnect then reconnect the R1 ring. Useful when the ring
  // BLE link enters a wedged state.
  { "Reconnect Ring", actionReconnectRing },
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
  AIWorkerArgs* a = (AIWorkerArgs*)malloc(sizeof(AIWorkerArgs));
  if (!a) {
    DEBUG_G2F("[G2] AI test: malloc failed (kind=%u)", (unsigned)kind);
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
  bool                  isList;  // false = TEXT widget, true = LIST widget
};

static const DisplayVariant kDisplayVariants[] = {
  // TEXT widget at varying sizes. Same content; compare visual extents.
  { "Full (576x288)",        G2_GEOM_FULL,        false },
  { "Large (560x272)",       G2_GEOM_LARGE,       false },
  { "Medium (480x240)",      G2_GEOM_MEDIUM,      false },
  { "Small (280x130 ctr)",   G2_GEOM_SMALL,       false },
  { "Top half (560x130)",    G2_GEOM_TOP_HALF,    false },
  { "Bottom half (560x130)", G2_GEOM_BOTTOM_HALF, false },

  // LIST widget for the same sizes that fit a list. Compares the
  // selection-rectangle chrome against the flowing-text view above.
  { "List: Large",           G2_GEOM_LARGE,       true  },
  { "List: Small",           G2_GEOM_SMALL,       true  },
};
static constexpr size_t kDisplayVariantCount =
  sizeof(kDisplayVariants) / sizeof(kDisplayVariants[0]);

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
  TEST_LEVEL_ACTIONS               = 1,  // BLE-action sub-menu
  TEST_LEVEL_BRACKETS              = 2,  // size-bracket sub-menu
  TEST_LEVEL_PAYLOAD               = 3,  // synthetic payload after a bracket tap
  TEST_LEVEL_AI                    = 4,  // AI panel variant sub-menu
  TEST_LEVEL_CHARS                 = 5,  // ASCII / Unicode picker
  TEST_LEVEL_CHARS_ASCII           = 6,  // ASCII char-test list
  TEST_LEVEL_CHARS_ASCII_PAYLOAD   = 7,  // showing one ASCII set
  TEST_LEVEL_CHARS_UNICODE         = 8,  // Unicode category picker
  TEST_LEVEL_CHARS_UNICODE_LIST    = 9,  // tests within selected Unicode category
  TEST_LEVEL_CHARS_UNICODE_PAYLOAD = 10, // showing one Unicode set
  TEST_LEVEL_DISPLAY               = 11, // Display geometry / container variants
  TEST_LEVEL_IMAGE                 = 12, // Image-probes router (3 sub-levels below)
  TEST_LEVEL_IMAGE_CONFIRMED       = 13, // Doc + Q4 canary + Q6 + Q6b
  TEST_LEVEL_IMAGE_STATIC          = 14, // Q9 frame builder
  TEST_LEVEL_IMAGE_STREAMING       = 15, // Q11 then Q10
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

static char        gRows[TEST_MAX_ROWS][TEST_ROW_LEN];
static const char* gRowPtrs[TEST_MAX_ROWS];

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

static void writeBackRow() {
  strncpy(gRows[0], "<- Back", TEST_ROW_LEN);
  gRows[0][TEST_ROW_LEN - 1] = '\0';
  gRowPtrs[0] = gRows[0];
}

// -----------------------------------------------------------------------------
// Level builders
// -----------------------------------------------------------------------------

// ROOT: category list
static size_t buildRootRows() {
  writeBackRow();
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "BLE Actions >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Transport Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "AI Panel Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Character Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Display Tests >>");
  gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Image Probes >>");
  gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// IMAGE: parent router. Each entry drills into a sub-level grouped by
// what the probes are for: Confirmed/Diagnostic for known-good things
// to re-run as canaries, Static for single-frame draw API tests,
// Streaming for multi-frame swap tests. Keeps each sub-list short
// enough to fit one screen on the lens.
static size_t buildImageRows() {
  writeBackRow();
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
  writeBackRow();
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
  writeBackRow();
  size_t row = 1;
  snprintf(gRows[row], TEST_ROW_LEN, "Q9: frame builder (3-band)");       gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "QGlizzy: SD /PICTURES/test.bmp");   gRowPtrs[row] = gRows[row]; row++;
  snprintf(gRows[row], TEST_ROW_LEN, "Q12: full-screen 576x288 (4 tiles)"); gRowPtrs[row] = gRows[row]; row++;
  return row;
}

// IMAGE / Streaming — multi-frame swap tests, no re-CREATE between
// frames. Q11 first (simplest); Q10 only if Q11 leaves visible tearing.
// Q13/Q14 are live-update pipelines paced by `g2liverate` (CLI).
static size_t buildImageStreamingRows() {
  writeBackRow();
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
  // Rebuild whichever sub-level the probe was launched from so the user
  // can chain probes without backing out to the parent. gTestLevel is
  // still set to the source sub-level since the dispatcher leaves it
  // alone when invoking a probe (only Back changes the level).
  vTaskDelay(pdMS_TO_TICKS(200));
  size_t n;
  switch (gTestLevel) {
    case TEST_LEVEL_IMAGE_CONFIRMED:  n = buildImageConfirmedRows();  break;
    case TEST_LEVEL_IMAGE_STATIC:     n = buildImageStaticRows();     break;
    case TEST_LEVEL_IMAGE_STREAMING:  n = buildImageStreamingRows();  break;
    default:                          n = buildImageRows();           break;
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

// DISPLAY: list of geometry / widget-type variants.
static size_t buildDisplayRows() {
  writeBackRow();
  size_t row = 1;
  for (size_t i = 0; i < kDisplayVariantCount && row < TEST_MAX_ROWS; i++) {
    strncpy(gRows[row], kDisplayVariants[i].label, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// Exit handler for TEXT-widget display tests. Wired as the exitFn arg
// to g2ShowTextPage so any tap (USER_ACTIVITY post-grace, SysEvent
// CLICK / SCROLL / DOUBLE_CLICK, TextEvent CLICK) returns the user to
// the Display Tests picker. Without this, the TEXT widget had no exit
// route and left the firmware to timeout-tear-down at ~8 s with
// "Connection lost" on the lens.
static void displayTestExitToPicker() {
  gInDisplayTest = false;
  size_t n = buildDisplayRows();
  g2ShowListPage(gRowPtrs, n);
  DEBUG_G2F("[G2] Display test: exit → picker (%u rows)", (unsigned)n);
}

static void runDisplayVariant(size_t idx) {
  if (idx >= kDisplayVariantCount) return;
  const DisplayVariant& v = kDisplayVariants[idx];
  gInDisplayTest = true;
  if (v.isList) {
    // Split kDisplaySampleText on newlines into rows so the LIST
    // widget has one row per line. First row is "<- Back" — tapping
    // it routes through the dispatcher's TEST_LEVEL_DISPLAY case
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
  } else {
    // TEXT widget — wire the exit handler so tap/scroll dismisses to
    // the picker. tapFn=nullptr keeps the legacy "any tap exits" UX.
    g2ShowTextPage(kDisplaySampleText, v.geom, displayTestExitToPicker);
  }
  DEBUG_G2F("[G2] Display test: '%s' (%s, %ux%u @ %u,%u)",
            v.label, v.isList ? "LIST" : "TEXT",
            (unsigned)v.geom.w, (unsigned)v.geom.h,
            (unsigned)v.geom.x, (unsigned)v.geom.y);
}

// ACTIONS: list of one-shot diagnostic operations
static size_t buildActionsRows() {
  writeBackRow();
  size_t row = 1;
  for (size_t i = 0; i < kActionCount && row < TEST_MAX_ROWS; i++) {
    strncpy(gRows[row], kActions[i].label, TEST_ROW_LEN);
    gRows[row][TEST_ROW_LEN - 1] = '\0';
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// CHARS: ASCII / Unicode picker.
static size_t buildCharsRows() {
  writeBackRow();
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
                                size_t& page) {
  writeBackRow();
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
  return buildCharTestList(kAsciiCharTests, kAsciiCharTestCount, gAsciiPage);
}

// Unicode picker — three category rows.
static size_t buildCharsUnicodeRows() {
  writeBackRow();
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
    writeBackRow();
    return 1;
  }
  const UnicodeCategory& c = kUnicodeCategories[gUnicodeCategoryIdx];
  return buildCharTestList(c.tests, c.count, gUnicodeListPage);
}

// Render one character-set test on the lens. Two rows of content under
// the back row: a label header and the sample bytes themselves. Sample
// is truncated at TEST_ROW_LEN-1; UTF-8 is byte-safe for strncpy as long
// as we don't truncate mid-codepoint, which we avoid by keeping samples
// shorter than TEST_ROW_LEN.
static size_t buildCharsPayloadRowsFrom(const CharTest* tests, size_t count, size_t idx) {
  writeBackRow();
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
  return buildCharsPayloadRowsFrom(kAsciiCharTests, kAsciiCharTestCount, idx);
}

static size_t buildCharsUnicodePayloadRows(size_t idx) {
  if (gUnicodeCategoryIdx >= kUnicodeCategoryCount) {
    writeBackRow();
    return 1;
  }
  const UnicodeCategory& c = kUnicodeCategories[gUnicodeCategoryIdx];
  return buildCharsPayloadRowsFrom(c.tests, c.count, idx);
}

// AI: list of front-pane AI panel pipeline variants
static size_t buildAIRows() {
  writeBackRow();
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
  writeBackRow();
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
  writeBackRow();

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
  s += "BLE Actions:\n";
  for (size_t i = 0; i < kActionCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), " %s\n", kActions[i].label);
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
  s += "Display Tests:\n";
  for (size_t i = 0; i < kDisplayVariantCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), " %s\n", kDisplayVariants[i].label);
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
        DEBUG_G2F("[G2] Test suite: BLE Actions sub-menu (rows=%u)",
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
        DEBUG_G2F("[G2] Test suite: Image Probes sub-menu (rows=%u)",
                  (unsigned)n);
        return;
      }
      DEBUG_G2F("[G2] Test suite root: tap idx=%u out of range", (unsigned)idx);
      return;
    }

    case TEST_LEVEL_DISPLAY: {
      // Two sub-states under this level:
      //   gInDisplayTest=false → user is on the picker (9-entry list).
      //                          idx=0 → back to ROOT.
      //                          idx 1..N → drill into a test.
      //   gInDisplayTest=true  → a LIST display test is on screen.
      //                          idx=0 → back to picker.
      //                          other idx → no-op (the rows are
      //                                       sample content, not
      //                                       actions).
      // TEXT display tests don't dispatch through here — they have
      // their own exitFn (displayTestExitToPicker) that handles the
      // gesture-to-exit routing via the gTextView* hooks.
      if (idx == 0) {
        if (gInDisplayTest) {
          displayTestExitToPicker();
        } else {
          gTestLevel = TEST_LEVEL_ROOT;
          size_t n = buildRootRows();
          g2ShowListPage(gRowPtrs, n);
        }
        return;
      }
      if (gInDisplayTest) {
        // Tap on a sample-content row inside a LIST test — no-op.
        return;
      }
      const size_t pos = idx - 1;
      if (pos >= kDisplayVariantCount) {
        DEBUG_G2F("[G2] Test suite DISPLAY: tap idx=%u out of range",
                  (unsigned)idx);
        return;
      }
      runDisplayVariant(pos);
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
      size_t n = buildPayloadRows(b.bytes);
      DEBUG_G2F("[G2] Test suite: rendering %s (rows=%u, target=%u B)",
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

static void actionReconnectRing() {
  // Heap-low guard. Arduino BLE leaks ~30 KB per server↔client
  // reconnect cycle, so a long session that's already cycled the BLE
  // stack a few times can run DRAM down to single-digit KB. If we
  // spawn the ring connect task AND the page-swap-worker
  // back-to-back when the budget is tight, the second xTaskCreate
  // fails and a few seconds later the BLE stack asserts-and-reboots
  // when its own internal queue-send hits a half-allocated structure.
  // Observed 2026-04-25: heap=2591 right before the cascade.
  //
  // 16 KB is "enough for both tasks plus pb buffer plus a margin."
  // If we're below that, abort with a clear log and skip the action;
  // user can hit Re-open Hijack from web UI, which doesn't allocate
  // as much.
  const uint32_t freeNow = ESP.getFreeHeap();
  if (freeNow < 16 * 1024) {
    DEBUG_G2F("[G2] Reconnect Ring: ABORTED — DRAM free %u B < 16 KB safety "
              "threshold. Reboot or disconnect/reconnect via web UI to "
              "recover heap (Arduino BLE leak per reconnect cycle is the "
              "usual cause).",
              (unsigned)freeNow);
    return;
  }

  // Force-disconnect first to give the BLE stack a clean slate.
  const bool wasUp = g2RingIsConnected();
  if (wasUp) {
    DEBUG_G2F("[G2] Reconnect Ring: dropping current connection first "
              "(heap=%u)", (unsigned)freeNow);
    g2RingDisconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
  } else {
    DEBUG_G2F("[G2] Reconnect Ring: ring already disconnected, "
              "skipping disconnect step (heap=%u)", (unsigned)freeNow);
  }

  if (g2RingConnect()) {
    DEBUG_G2F("[G2] Reconnect Ring: connect task started — "
              "watch ringstatus for completion");
  } else {
    DEBUG_G2F("[G2] Reconnect Ring: g2RingConnect() returned false — "
              "run a Temples Scan first to populate ring advertisement");
  }

  // Skip the menu redraw on purpose. Re-rendering the actions menu
  // here means a SHUTDOWN+CREATE-list page swap (4 KB worker stack,
  // 8 KB pb buffer) RIGHT after we already spent some heap on the
  // ring connect task. That's exactly the back-to-back allocation
  // pattern that triggered the heap-exhaustion crash before this
  // guard was added. The user just tapped a row — the lens still
  // shows the actions menu (we never tore it down), so there's
  // nothing to redraw. The action's outcome surfaces in serial
  // and via the web UI's ring-status panel.
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
