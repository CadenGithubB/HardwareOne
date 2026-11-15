// =============================================================================
// G2 glasses: the LLM input menu and the reply reader
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"
#include "System_CommandReply.h"  // the reply words and how to read them

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_HoldGesture.h"
#include "BLE_Server.h"
#include "G2_HijackCmd.h"

#include "G2_Page_Common.h"
#if ENABLE_LLM_BACKEND
#include "System_LLMChat.h"
#include "G2_LlmReader.h"
#include "System_PsramBuffer.h"
#include "System_Deadline.h"  // deadlinePending: the busy notice
#endif
#include "G2_GlassesInternal.h"

#if ENABLE_LLM_BACKEND
// ─────────────────────────────────────────────────────────────────────
// LLM input menu — free-text/speech entry plus the guided picker for the
// tiny on-device model
// (LLM_GUIDED_MENU_SPEC §9). Apps → LLM opens a small submenu
// (Open chat / Ask (Mic / Keys) / Ask (guided) / Re-run last). Free input
// uses the shared G2 text-entry surface; "Ask (guided)" drills through
// group → template → (alpha bucket) → entity as ORDINARY hijack list pages
// and composes the question ON-DEVICE via `llmask json <gen> <g> <t> <e>`,
// then hands the streaming answer to the existing read-only viewer
// (g2ShowLlmPage, unchanged) below.
//
// Why ordinary list pages: they ride the "app" container tap path
// (handleDevEvent → tapDispatcherEnqueue → g2_taps → handleHijackMenuTap
// → this page's handleTap), so every genuine tap already stamps
// gHijackStartedMs (the watchdog refresh) — nothing new is fed from acks.
// Picker state lives in file statics so a 60 s watchdog force-exit resumes
// at the last level on re-entry (Apps → LLM → Ask). All picker/compose work
// stays off BTC_TASK: the terminal `llmask` runs on cmd_exec via
// g2SubmitHijackCommand, and its completion opens the viewer (or a
// busy/stale message) on the lens-applier context.
//
// This page's hijack-page identity is G2_HIJACK_PAGE_LLM_MENU, declared with
// the rest of the enum in G2_Glasses.h. It used to be minted here as a local
// `(G2HijackPage)14` cast; the menu reorg then grew the enum past 13, so 14
// became G2_HIJACK_PAGE_LED and this page's taps were resolved to the LED
// module instead (g2FindPageByHijackPage returns the first id match, and LED
// registers first). Keep the id in the header.
// ─────────────────────────────────────────────────────────────────────

// Picker levels. HOME is the submenu; GROUP..ENTITY are the guided drill-down
// list pages; MODELS is Select Model / Unload; MSG is a transient
// busy/stale/error/loading page that any tap dismisses back to the submenu.
enum LlmMenuLevel : uint8_t {
  LLM_MENU_HOME = 0,
  LLM_MENU_GROUP,
  LLM_MENU_TEMPLATE,
  LLM_MENU_BUCKET,
  LLM_MENU_ENTITY,
  LLM_MENU_MODELS,
  LLM_MENU_MSG,
};

// Submenu rows come from the shared home menu (System_LLMHomeMenuCore.h, also
// used by the local display); the row count varies, so taps map through a
// per-row action table, mirroring g2AppsBuildRows.

#define LLM_MENU_ROW_LEN         52   // wide enough for a ≤48 B entity + NUL
#define LLM_MENU_VIS_ROWS        10   // picker items per page (>10 → paginator chrome)
#define LLM_MENU_MAX_ROWS        (1 + LLM_MENU_VIS_ROWS + 2)  // back + items + prev + next
#define LLM_MENU_BUCKET_THRESH   40   // ent_count over this inserts the alpha-bucket level
#define LLM_MENU_MAX_BUCKETS     8    // alpha buckets are reachable in one screen (no chrome)
#define LLM_MENU_MAX_BUCKET_ENTS 1024 // == the §2 per-group entity cap (a bucket never exceeds it)
// Model-table sizing now comes from the shared registry (System_LLMBackend.h).
// G2 used to define its own LLM_MODEL_NAME_LEN/PATH_LEN, which both duplicated
// the registry's constants and collided with them by name once this file needed
// the registry header.
#define G2_LLM_MODEL_MAX  LLM_REGISTRY_MAX_MODELS

// Row buffers shared by every level (one page is on screen at a time;
// g2ShowListPage deep-copies immediately, so reuse between renders is safe).
EXT_RAM_BSS_ATTR static char        sLlmRows[LLM_MENU_MAX_ROWS][LLM_MENU_ROW_LEN];
EXT_RAM_BSS_ATTR static const char* sLlmRowPtrs[LLM_MENU_MAX_ROWS];

// Picker state — persisted across a watchdog force-exit so Ask can resume.
static LlmMenuLevel sLlmLevel   = LLM_MENU_HOME;   // level currently on screen (drives handleTap)
static LlmMenuLevel sLlmResume  = LLM_MENU_HOME;   // level to resume on next Ask (HOME = fresh)
static uint16_t     sLlmGen     = 0;               // menuGeneration cached when the pick started
static uint8_t      sLlmGroup   = 0;               // selected group index
static uint16_t     sLlmTpl     = 0;               // selected template index
static uint8_t      sLlmBucket  = 0;               // selected alpha-bucket index
static uint8_t      sLlmBucketCount = 0;           // active buckets (0 = direct entity list)
static size_t       sLlmPage    = 0;               // pagination page of the current list

// Per-page layout captured by the renderer so the tap handler maps rows
// without re-deriving the layout (mirrors G2_Page_Settings' gPage* hooks).
static int          sLlmPrevRow      = -1;
static int          sLlmNextRow      = -1;
static size_t       sLlmFirstItemRow = 1;
static size_t       sLlmItemsOnPage  = 0;
static size_t       sLlmPageStartIdx = 0;

// Alpha-bucket boundaries (letter-class ranges) + the entity indices that
// fall in the selected bucket. Class 0 = '#'/non-alpha, 1..26 = A..Z.
static uint8_t      sLlmBucketLo[LLM_MENU_MAX_BUCKETS];
static uint8_t      sLlmBucketHi[LLM_MENU_MAX_BUCKETS];
EXT_RAM_BSS_ATTR static uint16_t sLlmEntIdx[LLM_MENU_MAX_BUCKET_ENTS];
static uint16_t     sLlmEntIdxCount = 0;

// Submenu row→action map + transient message text.
static LlmHomeAction sLlmHomeAct[LLM_MENU_MAX_ROWS];
static uint8_t      sLlmHomeActCount = 0;
static char         sLlmMsg[40] = {0};

// Retry provenance. Once the G2 accepts free input, the old "a guided ask
// exists" test is no longer sufficient: it would re-run an older guided
// question after a newer keyboard/mic turn. Mirror the OLED's rule — track
// whether this surface's last submission was guided and, for guided turns,
// require the menu module to still hold the same session.
static bool         sLlmLastTurnGuided = false;
static int          sLlmGuidedAskSession = 0;

enum class LlmPromptReturn : uint8_t {
  MENU = 0,
  VIEWER,
};

// Select Model cache — filled once when the page opens (tap task), then
// paginated. Paths are full VFS paths so llmload is unambiguous if the same
// basename exists on both internal and SD.
EXT_RAM_BSS_ATTR static LlmModelDesc sLlmModels[G2_LLM_MODEL_MAX];
static uint8_t sLlmModelCount = 0;

// Show functions used before their definitions (forward decls keep the
// natural top-down reading order below).
static void llmMenuShowGroups();
static void llmMenuShowTemplates();
static void llmMenuShowBuckets();
static void llmMenuShowEntities();
static void llmMenuShowModels();

// --- alpha-bucket helpers -------------------------------------------------

// Map an entity's first byte to a sortable letter class: 0 = '#'/non-alpha,
// 1..26 = A..Z (case-folded).
static uint8_t llmMenuLetterClass(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 1);
  return 0;
}

// Render a bucket label ("A", "#", or "A-D") for a class range.
static void llmMenuClassLabel(uint8_t lo, uint8_t hi, char* out, size_t cap) {
  auto ch = [](uint8_t cl) -> char { return cl == 0 ? '#' : (char)('A' + cl - 1); };
  if (lo == hi) snprintf(out, cap, "%c", ch(lo));
  else          snprintf(out, cap, "%c-%c", ch(lo), ch(hi));
}

// Partition the group's entities into ≤8 alpha buckets balanced by count,
// computed from the ACTUAL first letters present. Returns the bucket count
// (fills sLlmBucketLo/Hi). Reads each entity's first byte via the copy-out
// API (µs lock per call; runs once on the g2_taps tap, not a hot loop).
static uint8_t llmMenuComputeBuckets(uint8_t g, uint16_t entCount) {
  uint16_t cnt[27] = {0};
  char eb[8];
  for (uint16_t e = 0; e < entCount; e++) {
    if (llmMenuEntity(g, e, eb, sizeof(eb)) < 0) continue;
    cnt[llmMenuLetterClass(eb[0])]++;
  }
  uint32_t total = 0;
  int lastNon = -1;
  for (int c = 0; c < 27; c++) { total += cnt[c]; if (cnt[c]) lastNon = c; }
  if (total == 0) return 0;

  const uint32_t target = (total + LLM_MENU_MAX_BUCKETS - 1) / LLM_MENU_MAX_BUCKETS;  // ceil
  uint8_t  nb = 0;
  int      curLo = -1;
  uint32_t curCount = 0;
  for (int c = 0; c <= lastNon; c++) {
    if (!cnt[c]) continue;
    if (curLo < 0) curLo = c;
    curCount += cnt[c];
    // Close a bucket when it reaches the target — but reserve the LAST slot
    // for whatever remains so we never exceed LLM_MENU_MAX_BUCKETS buckets.
    const bool isLast = (c == lastNon);
    if ((curCount >= target && nb < LLM_MENU_MAX_BUCKETS - 1) || isLast) {
      sLlmBucketLo[nb] = (uint8_t)curLo;
      sLlmBucketHi[nb] = (uint8_t)c;
      nb++;
      curLo = -1;
      curCount = 0;
    }
  }
  return nb;
}

// Build the selected bucket's entity-index list into sLlmEntIdx (original
// menu indices, so the composed `llmask` sees the right entity).
static void llmMenuBuildFilter(uint8_t g, uint16_t entCount) {
  sLlmEntIdxCount = 0;
  if (sLlmBucketCount == 0 || sLlmBucket >= sLlmBucketCount) return;
  const uint8_t lo = sLlmBucketLo[sLlmBucket];
  const uint8_t hi = sLlmBucketHi[sLlmBucket];
  char eb[8];
  for (uint16_t e = 0; e < entCount && sLlmEntIdxCount < LLM_MENU_MAX_BUCKET_ENTS; e++) {
    if (llmMenuEntity(g, e, eb, sizeof(eb)) < 0) continue;
    const uint8_t cl = llmMenuLetterClass(eb[0]);
    if (cl >= lo && cl <= hi) sLlmEntIdx[sLlmEntIdxCount++] = e;
  }
}

// --- generic list renderer ------------------------------------------------

// Fill one display row for source item `globalIdx` at level `lvl`.
static void llmMenuFillItem(LlmMenuLevel lvl, size_t globalIdx, char* out, size_t cap) {
  out[0] = '\0';
  switch (lvl) {
    case LLM_MENU_GROUP: {
      LlmMenuGroupInfo gi;
      if (llmMenuGroupInfo((uint8_t)globalIdx, &gi)) snprintf(out, cap, "%s", gi.name);
      break;
    }
    case LLM_MENU_TEMPLATE:
      llmMenuTemplate(sLlmGroup, (uint16_t)globalIdx, out, cap, nullptr);  // 0x1F → "{}"
      break;
    case LLM_MENU_BUCKET:
      if (globalIdx < sLlmBucketCount)
        llmMenuClassLabel(sLlmBucketLo[globalIdx], sLlmBucketHi[globalIdx], out, cap);
      break;
    case LLM_MENU_ENTITY: {
      const uint16_t e = (sLlmBucketCount > 0) ? sLlmEntIdx[globalIdx] : (uint16_t)globalIdx;
      llmMenuEntity(sLlmGroup, e, out, cap);
      break;
    }
    case LLM_MENU_MODELS: {
      // Item 0 is always Unload; 1..N are cached model basenames. Mark the
      // currently loaded path with a leading '*' so the user can see which
      // row is active without a separate status page.
      if (globalIdx == 0) {
        snprintf(out, cap, "Unload model");
        break;
      }
      const size_t mi = globalIdx - 1;
      if (mi >= sLlmModelCount) break;
      // Compare IDs, not paths: a remote model has no path, and the host may
      // have adopted a different name than the one that was asked for.
      LlmModelDesc act;
      const bool cur = llmBackendIsReady() && llmBackendActiveModel(&act) &&
                       strcasecmp(act.id, sLlmModels[mi].id) == 0;
      snprintf(out, cap, "%s%s%s", cur ? "* " : "", sLlmModels[mi].name,
               sLlmModels[mi].available ? "" : " (offline)");
      break;
    }
    default: break;
  }
  if (out[0] == '\0') snprintf(out, cap, "?");   // defensive: never a blank row
}

// Render a paginated picker list: back row + items on sLlmPage + prev/next
// chrome (only when the source outgrows one page). Captures the layout for
// the tap handler and flips gHijackPage's menu-gen on a level change so a
// stale in-flight `llmask` redraw is dropped if the user navigates away.
static void llmMenuRenderList(LlmMenuLevel lvl, size_t itemCount, const char* backLabel) {
  G2Paginator p = g2PaginatorPrepare(itemCount, LLM_MENU_VIS_ROWS, sLlmPage);

  snprintf(sLlmRows[0], LLM_MENU_ROW_LEN, "%s", backLabel ? backLabel : "<- Back");
  sLlmRowPtrs[0] = sLlmRows[0];

  size_t row = 1;
  for (size_t i = p.startIdx; i < p.endIdx && row < LLM_MENU_MAX_ROWS - 2; i++) {
    llmMenuFillItem(lvl, i, sLlmRows[row], LLM_MENU_ROW_LEN);
    sLlmRowPtrs[row] = sLlmRows[row];
    row++;
  }
  sLlmFirstItemRow = 1;
  sLlmItemsOnPage  = row - 1;
  sLlmPageStartIdx = p.startIdx;

  row = g2PaginatorWriteChrome(p, sLlmPage, row, LLM_MENU_MAX_ROWS,
                               &sLlmRows[0][0], LLM_MENU_ROW_LEN, sLlmRowPtrs);
  sLlmPrevRow = p.prevRow;
  sLlmNextRow = p.nextRow;

  if (sLlmLevel != lvl) { sLlmLevel = lvl; g2BumpMenuGen(); }
  g2ShowListPage(sLlmRowPtrs, row);
}

// --- per-level show helpers (render at the current sLlmPage) --------------

static void llmMenuShowGroups() {
  llmMenuRenderList(LLM_MENU_GROUP, llmMenuGroupCount(), "<- Back");
}

static void llmMenuShowTemplates() {
  LlmMenuGroupInfo gi;
  const size_t n = llmMenuGroupInfo(sLlmGroup, &gi) ? gi.tplCount : 0;
  llmMenuRenderList(LLM_MENU_TEMPLATE, n, "<- Groups");
}

static void llmMenuShowBuckets() {
  llmMenuRenderList(LLM_MENU_BUCKET, sLlmBucketCount, "<- Templates");
}

static void llmMenuShowEntities() {
  size_t      count;
  const char* back;
  if (sLlmBucketCount > 0) {           // bucketed: filtered index list
    count = sLlmEntIdxCount;
    back  = "<- Buckets";
  } else {                             // direct: entity indices 0..entCount-1
    LlmMenuGroupInfo gi;
    count = llmMenuGroupInfo(sLlmGroup, &gi) ? gi.entCount : 0;
    back  = "<- Templates";
  }
  llmMenuRenderList(LLM_MENU_ENTITY, count, back);
}

// Scan /system/llm (+ /sd/llm when mounted) for *.bin into the Select Model
// cache. Same locations as llmListModels(), but fixed buffers — no Arduino
// String / JSON on the tap path. Called once when the page opens.
static void llmMenuScanModels() {
  // Was a third private copy of the .bin scan (web and OLED had the other two).
  // The registry lists every source, so a CM5 model — which has no file on this
  // device and therefore could never appear in a filesystem walk — shows up here
  // for free.
  sLlmModelCount = (uint8_t)llmEnumerateModels(sLlmModels, G2_LLM_MODEL_MAX);
}

static void llmMenuShowModels() {
  llmMenuRenderList(LLM_MENU_MODELS, 1u + (size_t)sLlmModelCount, "<- Back");
}

static void llmMenuOpenModels() {
  llmMenuScanModels();
  sLlmPage = 0;
  llmMenuShowModels();
}

static void llmMenuRerenderCurrent() {
  switch (sLlmLevel) {
    case LLM_MENU_GROUP:    llmMenuShowGroups();    break;
    case LLM_MENU_TEMPLATE: llmMenuShowTemplates(); break;
    case LLM_MENU_BUCKET:   llmMenuShowBuckets();   break;
    case LLM_MENU_ENTITY:   llmMenuShowEntities();  break;
    case LLM_MENU_MODELS:   llmMenuShowModels();    break;
    default: break;
  }
}

// True (and re-renders) when idx is a prev/next chrome row.
static bool llmMenuHandlePaging(uint32_t idx) {
  if (sLlmPrevRow >= 0 && (int)idx == sLlmPrevRow) {
    if (sLlmPage > 0) sLlmPage--;
    llmMenuRerenderCurrent();
    return true;
  }
  if (sLlmNextRow >= 0 && (int)idx == sLlmNextRow) {
    sLlmPage++;                          // g2PaginatorPrepare clamps on render
    llmMenuRerenderCurrent();
    return true;
  }
  return false;
}

// Map a tapped row to its source item index, or -1 for a non-item row.
static int llmMenuItemIdx(uint32_t idx) {
  if (idx < sLlmFirstItemRow) return -1;
  const size_t pos = idx - sLlmFirstItemRow;
  if (pos >= sLlmItemsOnPage) return -1;
  return (int)(sLlmPageStartIdx + pos);
}

// --- transient message page ----------------------------------------------

static void llmMenuMsgRender() {
  const char* items[2] = { "<- Back", sLlmMsg };
  sLlmLevel = LLM_MENU_MSG;
  if (g2ShowListPage(items, 2)) g2SetHijackPage(G2_HIJACK_PAGE_LLM_MENU);
}

static void llmMenuShowMsg(const char* line) {
  snprintf(sLlmMsg, sizeof(sLlmMsg), "%s", line ? line : "");
  llmMenuMsgRender();
}

static void llmMenuMarkGuidedTurn(int session) {
  if (session <= 0) return;
  __atomic_store_n(&sLlmGuidedAskSession, session, __ATOMIC_RELAXED);
  __atomic_store_n(&sLlmLastTurnGuided, true, __ATOMIC_RELEASE);
}

static void llmMenuMarkFreeTurn() {
  __atomic_store_n(&sLlmLastTurnGuided, false, __ATOMIC_RELEASE);
}

static bool llmMenuClearChat() {
  if (!chatClear()) return false;
  llmMenuMarkFreeTurn();
  __atomic_store_n(&sLlmGuidedAskSession, 0, __ATOMIC_RELAXED);
  return true;
}

// Repeat according to the actual kind of turn submitted by this G2 surface.
// A guided repeat must be plain (no suppress-token list); a free prompt uses
// the normal chat retry. Returns the new session id, or <=0 on refusal.
static int llmMenuRetryLastForG2() {
  if (!llmBackendSupportsRetry()) return 0;
  const bool guided =
      __atomic_load_n(&sLlmLastTurnGuided, __ATOMIC_ACQUIRE);
  const int trackedSession =
      __atomic_load_n(&sLlmGuidedAskSession, __ATOMIC_RELAXED);
  int menuSession = 0;
  if (guided && llmMenuLastAskInfo(&menuSession) &&
      menuSession == trackedSession) {
    const int sid = llmMenuRepeatLast();
    if (sid > 0) llmMenuMarkGuidedTurn(sid);
    return sid;
  }
  const int sid = chatRetryLast(nullptr);
  if (sid > 0) llmMenuMarkFreeTurn();
  return sid;
}

static void llmPromptReturnToOrigin(LlmPromptReturn returnTo) {
  if (returnTo == LlmPromptReturn::VIEWER) {
    if (!g2ShowLlmPage(g2ShowLlmMenu)) g2ShowLlmMenu();
  } else {
    g2ShowLlmMenu();
  }
}

static void llmPromptCommit(const char* text, LlmPromptReturn returnTo) {
  if (!text || !text[0]) {
    llmPromptReturnToOrigin(returnTo);
    return;
  }
  const int sid = chatBeginTurn(text, nullptr);
  if (sid <= 0) {
    llmMenuShowMsg(llmBackendIsReady() ? "Busy - try again" : "No model loaded");
    return;
  }
  llmMenuMarkFreeTurn();
  if (!g2ShowLlmPage(g2ShowLlmMenu)) {
    DEBUG_G2F("[G2-LLM] prompt accepted but viewer spawn declined");
    g2ShowLlmMenu();
  }
}

static bool llmMenuStartPromptEntry(LlmPromptReturn returnTo, bool startWithMic = false,
                                   uint32_t expectedLifecycleEpoch = 0) {
  TextEntryConfig cfg = {};
  cfg.prompt   = "Ask LLM";
  cfg.initial  = "";
  cfg.maxLen   = G2_TEXT_ENTRY_MAX_LEN;
  // The return target belongs to this entry's callbacks. A stale startup must
  // not change the destination of another currently active prompt.
  if (returnTo == LlmPromptReturn::VIEWER) {
    cfg.onCommit = [](const char* text) { llmPromptCommit(text, LlmPromptReturn::VIEWER); };
    cfg.onCancel = [] { llmPromptReturnToOrigin(LlmPromptReturn::VIEWER); };
  } else {
    cfg.onCommit = [](const char* text) { llmPromptCommit(text, LlmPromptReturn::MENU); };
    cfg.onCancel = [] { llmPromptReturnToOrigin(LlmPromptReturn::MENU); };
  }
  cfg.startWithMic = startWithMic;
  cfg.expectedLifecycleEpoch = expectedLifecycleEpoch;
  if (g2BeginTextEntry(cfg)) return true;
  DEBUG_G2F("[G2-LLM] prompt text-entry failed to open");
  return false;
}

// --- terminal ask (submit → viewer, or busy/stale message) ----------------

// Runs on the lens-applier context (see g2EnqueueRedraw): spawn the
// interactive viewer to stream the answer. Back returns to the submenu.
static void llmMenuOpenViewerRender() {
  if (!g2ShowLlmPage(g2ShowLlmMenu))
    DEBUG_G2F("[G2-LLM] guided ask: viewer spawn declined (low RAM)");
}

// cmd_exec completion for `llmask json ...`. executeCommand returns true even
// for a busy/stale reply (the JSON payload is exempt from OK:/Error: stamping),
// so classify off the reply's own {"ok":...} field.
static void llmMenuAskDone(bool ok, const char* result, const G2CmdCookie& c, void* /*ud*/) {
  if (ok && commandReplyJsonOk(result)) {
    int guidedSession = 0;
    if (llmMenuLastAskInfo(&guidedSession))
      llmMenuMarkGuidedTurn(guidedSession);
    (void)g2EnqueueRedraw(c, llmMenuOpenViewerRender, "LLM ask-ok");     // stream the answer
    return;
  }
  const bool stale = result && strstr(result, "menu changed");
  snprintf(sLlmMsg, sizeof(sLlmMsg), "%s", stale ? "Menu changed - reopen" : "Busy - try again");
  (void)g2EnqueueRedraw(c, llmMenuMsgRender, "LLM ask-msg");
}

// Compose + submit on cmd_exec under the paired user's auth. entityIdx < 0
// for a slotless ("canned") template. The gen is the one cached when the pick
// started; a raced model swap fails cleanly (llmask → stale → message).
static void llmMenuSubmit(int entityIdx) {
  char line[64];
  if (entityIdx >= 0)
    snprintf(line, sizeof(line), "llmask json %u %u %u %d",
             (unsigned)sLlmGen, (unsigned)sLlmGroup, (unsigned)sLlmTpl, entityIdx);
  else
    snprintf(line, sizeof(line), "llmask json %u %u %u",
             (unsigned)sLlmGen, (unsigned)sLlmGroup, (unsigned)sLlmTpl);
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)sLlmLevel;
  DEBUG_G2F("[G2-LLM] submit '%s'", line);
  if (!g2SubmitHijackCommand(line, cookie, llmMenuAskDone, nullptr)) {
    DEBUG_G2F("[G2-LLM] ask submit failed (queue full)");
    llmMenuShowMsg("Busy - try again");
  }
}

// cmd_exec completion for llmload / llmunload. Same JSON {"ok":...} shape as
// the web / CLI; admin denial is an Error: line (executeCommand returns false).
static void llmMenuModelCmdDone(bool ok, const char* result, const G2CmdCookie& c, void* /*ud*/) {
  if (ok && commandReplyJsonOk(result)) {
    sLlmResume = LLM_MENU_HOME;   // guided resume is stale after a model swap
    (void)g2EnqueueRedraw(c, g2ShowLlmMenu, "LLM model-ok");
    return;
  }
  if (commandReplyIsAdminRefusal(result))
    snprintf(sLlmMsg, sizeof(sLlmMsg), "Admin required");
  else if (result && strstr(result, "clear chat before"))
    snprintf(sLlmMsg, sizeof(sLlmMsg), "New chat before changing agent source");
  else if (commandReplyJsonFailed(result))
    snprintf(sLlmMsg, sizeof(sLlmMsg), "Load failed");
  else
    snprintf(sLlmMsg, sizeof(sLlmMsg), "Command failed");
  (void)g2EnqueueRedraw(c, llmMenuMsgRender, "LLM model-err");
}

// Show a wait message, then run llmload/llmunload on cmd_exec (never call
// llmLoadModel inline — it blocks for seconds on the tap task).
static void llmMenuSubmitModelCmd(const char* line, const char* waitMsg) {
  llmMenuShowMsg(waitMsg ? waitMsg : "Working...");
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)LLM_MENU_MODELS;
  DEBUG_G2F("[G2-LLM] model cmd '%s'", line);
  if (!g2SubmitHijackCommand(line, cookie, llmMenuModelCmdDone, nullptr)) {
    DEBUG_G2F("[G2-LLM] model submit failed (queue full)");
    llmMenuShowMsg("Busy - try again");
  }
}

static void llmMenuModelTap(uint32_t idx) {
  if (idx == 0) { g2ShowLlmMenu(); return; }          // <- Back to submenu
  if (llmMenuHandlePaging(idx)) return;
  const int item = llmMenuItemIdx(idx);
  if (item < 0) return;
  if (item == 0) {
    llmMenuSubmitModelCmd("llmunload", "Unloading...");
    return;
  }
  const size_t mi = (size_t)item - 1;
  if (mi >= sLlmModelCount) return;
  char line[112];
  snprintf(line, sizeof(line), "llmload %s", sLlmModels[mi].id);
  llmMenuSubmitModelCmd(line, "Loading model...");
}

// --- per-level tap handlers -----------------------------------------------

static void llmMenuGroupTap(uint32_t idx) {
  if (idx == 0) { g2ShowLlmMenu(); return; }          // <- Back to submenu
  if (llmMenuHandlePaging(idx)) return;
  const int g = llmMenuItemIdx(idx);
  if (g < 0) return;
  sLlmGroup = (uint8_t)g;
  sLlmPage  = 0;
  llmMenuShowTemplates();
}

static void llmMenuTemplateTap(uint32_t idx) {
  if (idx == 0) { sLlmPage = 0; llmMenuShowGroups(); return; }   // <- Groups
  if (llmMenuHandlePaging(idx)) return;
  const int t = llmMenuItemIdx(idx);
  if (t < 0) return;

  bool slot = false;
  char tb[LLM_MENU_ROW_LEN];
  llmMenuTemplate(sLlmGroup, (uint16_t)t, tb, sizeof(tb), &slot);
  sLlmTpl = (uint16_t)t;

  if (!slot) { llmMenuSubmit(-1); return; }           // slotless / canned → ask now

  LlmMenuGroupInfo gi;
  if (!llmMenuGroupInfo(sLlmGroup, &gi) || gi.entCount == 0) {
    llmMenuShowMsg("No entities for this group");      // slotted but empty (shouldn't happen)
    return;
  }
  // Alpha-bucket level only pays off past the threshold AND when it actually
  // splits into ≥2 reachable buckets; otherwise go straight to the entity list.
  if (gi.entCount > LLM_MENU_BUCKET_THRESH) {
    const uint8_t nb = llmMenuComputeBuckets(sLlmGroup, gi.entCount);
    if (nb >= 2) {
      sLlmBucketCount = nb;
      sLlmBucket      = 0;
      sLlmPage        = 0;
      llmMenuShowBuckets();
      return;
    }
  }
  sLlmBucketCount = 0;
  sLlmEntIdxCount = 0;
  sLlmPage        = 0;
  llmMenuShowEntities();
}

static void llmMenuBucketTap(uint32_t idx) {
  if (idx == 0) { sLlmPage = 0; llmMenuShowTemplates(); return; }  // <- Templates
  if (llmMenuHandlePaging(idx)) return;                            // ≤8 buckets → no chrome
  const int b = llmMenuItemIdx(idx);
  if (b < 0 || b >= (int)sLlmBucketCount) return;
  sLlmBucket = (uint8_t)b;
  sLlmPage   = 0;
  LlmMenuGroupInfo gi;
  if (!llmMenuGroupInfo(sLlmGroup, &gi)) { llmMenuShowTemplates(); return; }
  llmMenuBuildFilter(sLlmGroup, gi.entCount);
  llmMenuShowEntities();
}

static void llmMenuEntityTap(uint32_t idx) {
  if (idx == 0) {                                     // <- back one level
    sLlmPage = 0;
    if (sLlmBucketCount > 0) llmMenuShowBuckets();
    else                     llmMenuShowTemplates();
    return;
  }
  if (llmMenuHandlePaging(idx)) return;
  const int ei = llmMenuItemIdx(idx);
  if (ei < 0) return;
  const uint16_t e = (sLlmBucketCount > 0) ? sLlmEntIdx[ei] : (uint16_t)ei;
  llmMenuSubmit((int)e);
}

// Start (or resume) the guided pick. A watchdog force-exit leaves the picker
// statics intact, so if the menu hasn't changed since, resume at the level
// the user abandoned; otherwise reset to a fresh group pick.
static void llmMenuStartAsk() {
  const uint16_t curGen = llmMenuGeneration();
  const uint8_t  gc     = llmMenuGroupCount();
  if (gc == 0) { g2ShowLlmMenu(); return; }           // defensive — Ask row is hidden when 0

  if (sLlmResume >= LLM_MENU_GROUP && sLlmResume <= LLM_MENU_ENTITY &&
      sLlmGen == curGen && sLlmGroup < gc) {
    DEBUG_G2F("[G2-LLM] Ask: resuming at level %u", (unsigned)sLlmResume);
    switch (sLlmResume) {
      case LLM_MENU_TEMPLATE: llmMenuShowTemplates(); return;
      case LLM_MENU_BUCKET:   llmMenuShowBuckets();   return;
      case LLM_MENU_ENTITY:   llmMenuShowEntities();  return;
      default:                llmMenuShowGroups();    return;
    }
  }

  sLlmGen         = curGen;
  sLlmGroup       = 0;
  sLlmTpl         = 0;
  sLlmBucket      = 0;
  sLlmBucketCount = 0;
  sLlmEntIdxCount = 0;
  sLlmPage        = 0;
  llmMenuShowGroups();
}

// Re-run the last question. Guided turns are re-asked plain (no suppress list),
// while keyboard/mic turns use the ordinary retry path. The local provenance
// recorded at submit time remains available after chatGetSessionId() returns 0.
static void llmMenuRerunLast() {
  (void)llmMenuRetryLastForG2();
  g2ShowLlmPage(g2ShowLlmMenu);
}

static void llmMenuHomeTap(uint32_t idx) {
  if (idx >= sLlmHomeActCount) return;
  switch (sLlmHomeAct[idx]) {
    case LlmHomeAction::Back:   g2ShowAppsMenu();            return;
    case LlmHomeAction::Open:   g2ShowLlmPage(g2ShowLlmMenu); return;
    case LlmHomeAction::Terminal: {
      const char* result = g2NativeTerminalRequestOpen();
      if (commandReplyIsFailure(result)) llmMenuShowMsg(result);
      return;
    }
    case LlmHomeAction::Ask:
      if (!llmMenuStartPromptEntry(LlmPromptReturn::MENU))
        llmMenuShowMsg("Keyboard unavailable");
      return;
    case LlmHomeAction::Guided: llmMenuStartAsk();           return;
    case LlmHomeAction::NewChat:
      llmMenuShowMsg(llmMenuClearChat() ? "New chat ready" : "Stop or wait, then New chat");
      return;
    case LlmHomeAction::Rerun:  llmMenuRerunLast();          return;
    case LlmHomeAction::Model:  llmMenuOpenModels();         return;
  }
}

static void g2LlmMenuHandleTap(uint32_t idx) {
  switch (sLlmLevel) {
    case LLM_MENU_HOME:     llmMenuHomeTap(idx);     return;
    case LLM_MENU_GROUP:    llmMenuGroupTap(idx);    return;
    case LLM_MENU_TEMPLATE: llmMenuTemplateTap(idx); return;
    case LLM_MENU_BUCKET:   llmMenuBucketTap(idx);   return;
    case LLM_MENU_ENTITY:   llmMenuEntityTap(idx);   return;
    case LLM_MENU_MODELS:   llmMenuModelTap(idx);    return;
    case LLM_MENU_MSG:      g2ShowLlmMenu();         return;   // any tap dismisses to submenu
    default: return;
  }
}

// Stub CLI text view (required by g2RegisterPage; the page is hidden and
// its real surface is on the lens).
static void g2BuildLlmMenuInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap,
           "LLM (chat / mic or keyboard ask / guided ask / re-run / model on lens)");
}

const G2PageModule kLlmMenuPage = {
  // Hidden — hijackLabel=nullptr keeps it out of the main menu; the only
  // entry is g2ShowLlmMenu() from the Apps launcher (Apps → LLM).
  "llmapp", nullptr,
  "LLM input picker (chat / mic or keyboard / guided ask / re-run / model on lens)",
  /*buildText=*/  g2BuildLlmMenuInfo,
  /*showMenu=*/   nullptr,
  /*handleTap=*/  g2LlmMenuHandleTap,
  G2_HIJACK_PAGE_LLM_MENU,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Apps",
  /*prefersTextWidget=*/ false,   // own list layout — flag is irrelevant
  /*liveRender=*/    nullptr,
};

// Entry point — the LLM submenu (Apps → LLM): "<- Back", then the shared home
// rows (System_LLMHomeMenuCore.h): Open chat / Open Terminal / Ask (Mic / Keys) /
// Ask (guided) / New chat / Re-run last / Model, plus the low-context warning,
// which opens the model picker since reloading is one fix (a restart the other).
// Re-showing it after a picker (Back, or the viewer's onDone) captures the
// abandoned guided level so the next Ask can resume there.
void g2ShowLlmMenu() {
  if (sLlmLevel >= LLM_MENU_GROUP && sLlmLevel <= LLM_MENU_ENTITY)
    sLlmResume = sLlmLevel;
  else
    sLlmResume = LLM_MENU_HOME;

  const bool haveMenu = (llmMenuGroupCount() > 0);
  size_t n = 0;
  auto add = [&](const char* label, LlmHomeAction act) {
    if (n < LLM_MENU_MAX_ROWS) {
      snprintf(sLlmRows[n], LLM_MENU_ROW_LEN, "%s", label);
      sLlmRowPtrs[n]  = sLlmRows[n];
      sLlmHomeAct[n]  = act;
      n++;
    }
  };
  add("<- Back", LlmHomeAction::Back);
  // Static, like sLlmRows: ~600 B is too much for the lens tasks' stacks.
  EXT_RAM_BSS_ATTR static llmhome::Row homeRows[LLM_MENU_MAX_ROWS - 1];
  const size_t homeCount = llmHomeMenuRowsNow(ENABLE_LOCAL_STT, LLM_MENU_ROW_LEN - 1,
                                              homeRows, LLM_MENU_MAX_ROWS - 1);
  for (size_t i = 0; i < homeCount; i++) add(homeRows[i].label, homeRows[i].action);
  sLlmHomeActCount = (uint8_t)n;

  sLlmLevel = LLM_MENU_HOME;
  g2BumpMenuGen();                 // navigation invalidates any in-flight ask redraw
  if (g2ShowListPage(sLlmRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_LLM_MENU);
    DEBUG_G2F("[G2-LLM] submenu shown (%u rows, menu=%s)",
              (unsigned)n, haveMenu ? "yes" : "no");
  } else {
    DEBUG_G2F("[G2-LLM] submenu show FAILED");
  }
}
#endif // ENABLE_LLM_BACKEND

#if ENABLE_LLM_BACKEND
// ─────────────────────────────────────────────────────────────────────
// Full-width LLM reply reader. The session worker owns text, pagination and
// wire updates; the existing fenced text/hold dispatchers only post intent.
// Menus keep Ask/New chat/Model actions. A hold opens the same draft/review
// flow with microphone startup, without clearing or resubmitting a chat turn.
// ─────────────────────────────────────────────────────────────────────
struct LlmPageArgs {
  void (*onDone)();
  uint32_t lifecycleEpoch;
};

static constexpr const char* kLlmChatChildName = "llmChat";

static bool g2LlmReaderCurrent() {
  return gLlmPageActive.load(std::memory_order_acquire) &&
      g2GetHijackPage() == G2_HIJACK_PAGE_LLM_READER;
}

static void g2LlmReaderExit() {
  if (g2LlmReaderCurrent()) gLlmPageExit.store(true, std::memory_order_release);
}

static void g2LlmReaderNav(G2TapKind kind) {
  if (!g2LlmReaderCurrent()) return;
  // Bounded intent count; no pager state is touched on the dispatch task.
  int pending = gLlmPageNav.load(std::memory_order_relaxed);
  const int step = kind == G2_TAP_PAGE_PREV ? -1 : 1;
  while (pending + step >= -8 && pending + step <= 8 &&
         !gLlmPageNav.compare_exchange_weak(pending, pending + step)) {}
}

bool g2LlmReaderHold(uint8_t phase, uint8_t source) {
  if (phase != (uint8_t)G2HoldPhase::Press) return true;
  if (!g2LlmReaderCurrent() || !g2HoldSourceAdmitted(source)) return false;
  // Consume a busy hold on this page, so it cannot fall through to QuickNote.
  // Starting another prompt does not implicitly cancel/retry an agent action.
  if (chatIsGenerating()) gLlmPageBusyNotice.store(true, std::memory_order_release);
  else gLlmPageAsk.store(true, std::memory_order_release);
  return true;
}

static void g2LlmPageWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;
  auto* a = static_cast<LlmPageArgs*>(arg);
  const uint32_t life = a->lifecycleEpoch;
  bool openPrompt = false, returnToMenu = false;
  G2Temple* arm = nullptr;
  uint32_t generation = 0, presentation = 0;
  const size_t rawCap = LLM_CHAT_TURN_MAX_BYTES;
  const size_t wrapCap = G2LlmReader::wrappedCapacity(rawCap);
  const size_t offsetCount = G2LlmReader::pageOffsetsCapacity(rawCap);
  PsramBuffer raw(rawCap, "g2.llm.raw");
  PsramBuffer wrapped(wrapCap, "g2.llm.wrapped");
  PsramBuffer offsets(offsetCount * sizeof(uint16_t), "g2.llm.offsets");

  do {
    if (!raw.reserve(rawCap) || !wrapped.reserve(wrapCap) ||
        !offsets.reserve(offsetCount * sizeof(uint16_t))) {
      DEBUG_G2F("[G2] LLM reader: allocation failed");
      break;
    }
    arm = pickEvenAiArm("llmReader");
    if (!arm) break;
    generation = arm->connectionGeneration;
    if (!g2LensTxFenceCurrent(*arm, generation, life) ||
        !tearDownActiveContainerAtGeneration(*arm, generation, false, life)) break;

    gLlmPageExit.store(false);
    gLlmPageAsk.store(false);
    gLlmPageBusyNotice.store(false);
    gLlmPageNav.store(0);
    if (!sendCreateTextAndWait(*arm, "Opening reply...", BLOCKS_WIDGET_ID,
                               G2_GEOM_LARGE, true, kLlmChatChildName) ||
        !g2NoteCreateSuccess(*arm, false, BLOCKS_WIDGET_ID, life, &presentation)) break;
    {
      // Publish callbacks and page ownership in the same completion gate as
      // terminal cleanup/CREATE, so a stale worker cannot relabel a new page.
      G2CompletionGuard completion;
      if (!completion ||
          !g2TextViewPublishForLifecycle(g2LlmReaderExit, g2LlmReaderNav,
                                         life, presentation, true)) break;
      g2SetHijackPage(G2_HIJACK_PAGE_LLM_READER);
      gLlmPageActive.store(true, std::memory_order_release);
    }
    TextPager pager = {wrapped.data(), reinterpret_cast<uint16_t*>(offsets.data()),
                       (int)offsetCount - 1, G2LlmReader::kBodyBytes,
                       0, 0, false};
    char frame[G2LlmReader::kFrameCapacity] = {};
    char lastFrame[G2LlmReader::kFrameCapacity] = {};
    ChatRunToken run = 0;
    int lastLen = -1;
    bool lastDone = false;
    uint32_t noticeUntil = 0;
    unsigned sendFailures = 0;
    while (g2WidgetLifecycleOpen(life) && g2FsmHijackActive() &&
           !gImgProbeAbort && g2LlmReaderCurrent() &&
           g2PresentationEpochCurrent() == presentation &&
           g2LensTxFenceCurrent(*arm, generation, life)) {
      // Defer both rendering and a pending handoff while native UI owns focus.
      if (firmwareOverlayActive()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
      if (gLlmPageExit.exchange(false)) { returnToMenu = true; break; }
      if (gLlmPageAsk.exchange(false)) {
        if (!chatIsGenerating()) { openPrompt = true; break; }
        gLlmPageBusyNotice.store(true);
      }
      const ChatRunToken currentRun = chatGetRunToken();
      if (currentRun != run) {
        run = currentRun;
        pager.curPage = 0;
        lastLen = -1;
      }
      bool done = true;
      const int len = run
          ? chatReadRun(run, 0, raw.data(), (int)rawCap, &done) : 0;
      // A replaced/cleared turn cannot be combined with the prior snapshot.
      if (len < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
      if (!run) raw.data()[0] = '\0';
      if (len != lastLen || done != lastDone) {
        G2LlmReader::rebuild(raw.data(), (size_t)len, done, pager,
                             wrapped.data(), wrapCap);
        lastLen = len;
        lastDone = done;
      }
      int moves = gLlmPageNav.exchange(0);
      while (moves > 0) { G2LlmReader::navigate(pager, true); --moves; }
      while (moves < 0) { G2LlmReader::navigate(pager, false); ++moves; }
      if (gLlmPageBusyNotice.exchange(false)) noticeUntil = millis() + 2500;
      if (deadlinePending(noticeUntil, millis())) {  // 0: no notice asked for
        strlcpy(frame, "Reply in progress.\nHold to ask after it finishes.\n2x-tap: back", sizeof(frame));
      } else if (!run) {
        strlcpy(frame, "No reply yet.\nHold to ask a question.\n2x-tap: back", sizeof(frame));
      } else {
        G2LlmReader::compose(pager, done, frame, sizeof(frame));
      }
      // Retain the last successful wire frame, not a failed attempt. Failed
      // updates are retried locally; agent requests are never replayed.
      if (strcmp(frame, lastFrame) != 0) {
        // Fill all six body lines. REBUILD's existing fragmentation keeps each
        // ATT packet bounded while replacing this same input-capturing child.
        if (sendRebuildTextNamedAndWait(*arm, kLlmChatChildName, frame,
                                        G2_GEOM_LARGE, true, generation, life,
                                        presentation)) {
          strlcpy(lastFrame, frame, sizeof(lastFrame));
          sendFailures = 0;
        } else if (++sendFailures >= 3) {
          DEBUG_G2F("[G2] LLM reader: text updates failed");
          returnToMenu = true;
          break;
        }
      }
      if (!done && g2FsmHijackActive()) gHijackStartedMs = millis();
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    gLlmPageActive.store(false, std::memory_order_release);
  } while (false);

  gLlmPageActive.store(false, std::memory_order_release);
  gLlmPageAsk.store(false);
  gLlmPageNav.store(0);
  g2TextViewDisarmIfOwned(g2LlmReaderExit, life);
  const bool ownsSurface = arm && presentation &&
      g2WidgetLifecycleOpen(life) && g2PresentationEpochCurrent() == presentation &&
      g2GetHijackPage() == G2_HIJACK_PAGE_LLM_READER && !gImgProbeAbort;
  if (ownsSurface) {
    // Finish the reader's own widget before opening microphone input. A stale
    // worker never tears down a replacement page or opens its return menu.
    const bool closed = tearDownActiveContainerAtGeneration(*arm, generation, false, life);
    const uint32_t afterTeardown = presentation == UINT32_MAX ? 1 : presentation + 1;
    const auto canReturn = [&]() {
      return g2LensTxFenceCurrent(*arm, generation, life) &&
          g2PresentationEpochCurrent() == afterTeardown &&
          !gImgProbeAbort && g2GetHijackPage() == G2_HIJACK_PAGE_LLM_READER;
    };
    if (closed && canReturn() && openPrompt) {
      if (!llmMenuStartPromptEntry(LlmPromptReturn::VIEWER, true, life) &&
          canReturn() && a->onDone)
        a->onDone();
    } else if (closed && canReturn() && returnToMenu && a->onDone) {
      a->onDone();
    }
  }
  delete a;
}

static void g2LlmPageDrop(void* p) { delete static_cast<LlmPageArgs*>(p); }

bool g2ShowLlmPage(void (*onDone)()) {
  const uint32_t life = __atomic_load_n(&gWidgetLifecycleEpoch, __ATOMIC_ACQUIRE);
  auto* a = new (std::nothrow) LlmPageArgs{onDone, life};
  if (!a) return false;
  if (!g2SessionSubmit(g2LlmPageWorker, a, g2LlmPageDrop, "llm_reader", life)) {
    g2LlmPageDrop(a);
    return false;
  }
  return true;
}
#endif // ENABLE_LLM_BACKEND

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
