/**
 * System_LLM_Menu.cpp - Guided-input menu: shared, thread-safe accessors over the
 * MENU section parsed from a model's info block, plus the llmmenu/llmask commands.
 *
 * The menu (curated corpus-exact question templates + entity rosters) is parsed at
 * model load into ONE PSRAM blob (gLLM.menuBlob) with per-group descriptors
 * (gLLM.menuGroups). Surfaces pick integer indices; this module composes the final
 * question once, on-device, and hands it to the existing chat pipeline.
 *
 * Locking (mandatory — verified race). Menu readers run on the OLED loop task,
 * httpd, g2_tap_disp and cmd_exec, while llmUnload can free the blob from ANOTHER
 * task. So all menu state is guarded by a single static mutex (sMenuLock). Every
 * accessor takes it, checks state, COPIES the requested item out, and releases
 * (µs hold). llmMenuPublish / llmMenuClear take it around the free/publish +
 * generation bump. No caller ever receives a pointer into the blob, and a
 * generation check without the lock (a TOCTOU) is forbidden. See
 * LLM_GUIDED_MENU_SPEC §5.
 */
#include "System_BuildConfig.h"
#if ENABLE_ONDEVICE_LLM

#include "System_LLM.h"
#include "System_LLM_Internal.h"   // gLLM, LLMMenuGroupDesc, lifecycle hooks, llmPsramFree
#include "System_LLMChat.h"        // ChatParamOverride, chatBeginTurn, chatGetSessionId
#include "System_Command.h"        // CommandArgs
#include "System_Utils.h"          // argLeadingTokenIsJson
#include "System_Debug.h"          // cliHint
#include "System_MemUtil.h"        // PSRAM_JSON_DOC, ps_alloc, AllocPref

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_attr.h>
#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The slot marker byte inside a template ("{}" in display form). Single byte so a
// template is a plain length-prefixed string with 0 or 1 occurrence.
static constexpr uint8_t MENU_SLOT_BYTE = 0x1F;

// ============================================================================
// Lock + last-guided-ask state
// ============================================================================

static SemaphoreHandle_t sMenuLock = nullptr;

// The last guided question submitted (for the guided-retry fix — re-asking PLAIN
// rather than via chatRetryLast, which would suppress the memorized-correct
// answer). Protected by sMenuLock. Cleared on every load/unload (llmMenuClear).
EXT_RAM_BSS_ATTR static char sLastComposed[192];    // composed question WITHOUT the "do: " lead-in
static int  sLastAskSession = 0;   // chat session id of the last guided ask (0 = none)
static bool sLastAskIsDo    = false;

struct MenuLock {
  bool held;
  MenuLock() : held(false) {
    if (sMenuLock) held = (xSemaphoreTake(sMenuLock, portMAX_DELAY) == pdTRUE);
  }
  ~MenuLock() { if (held) xSemaphoreGive(sMenuLock); }
};

static void menuEnsureLock() {
  if (!sMenuLock) sMenuLock = xSemaphoreCreateMutex();
}

// ============================================================================
// Lifecycle (called from the model loader / llmInit / llmUnload)
// ============================================================================

void llmMenuInit(void) { menuEnsureLock(); }

void llmMenuPublish(uint8_t* blob, size_t bytes,
                    const LLMMenuGroupDesc* groups, uint8_t count) {
  menuEnsureLock();
  MenuLock lk;
  if (gLLM.menuBlob) llmPsramFree((void**)&gLLM.menuBlob);   // nulls the pointer
  const bool have = (blob != nullptr && count > 0 && groups != nullptr);
  gLLM.menuBlob       = have ? blob : nullptr;
  gLLM.menuBytes      = have ? bytes : 0;
  gLLM.menuGroupCount = have ? count : 0;
  if (have) memcpy(gLLM.menuGroups, groups, (size_t)count * sizeof(LLMMenuGroupDesc));
  else      memset(gLLM.menuGroups, 0, sizeof(gLLM.menuGroups));
  gLLM.menuGeneration++;
}

void llmMenuClear(void) {
  menuEnsureLock();
  MenuLock lk;
  if (gLLM.menuBlob) llmPsramFree((void**)&gLLM.menuBlob);
  gLLM.menuBytes      = 0;
  gLLM.menuGroupCount = 0;
  memset(gLLM.menuGroups, 0, sizeof(gLLM.menuGroups));
  sLastComposed[0] = '\0';
  sLastAskSession  = 0;
  sLastAskIsDo     = false;
  gLLM.menuGeneration++;
}

// ============================================================================
// Internal walkers (assume sMenuLock is held)
// ============================================================================

// Walk to the idx-th length-prefixed item ([u8 len][bytes]) starting at byte
// offset `startOff` in the blob. Returns the item length and sets *outPtr to the
// item's bytes — an INTERIOR pointer valid only while the lock is held; the
// caller copies out before releasing. Returns -1 if idx runs off the blob.
static int menuItemAt(uint32_t startOff, uint16_t idx, const uint8_t** outPtr) {
  const uint8_t* b = gLLM.menuBlob;
  const size_t   n = gLLM.menuBytes;
  if (!b) return -1;
  size_t pos = startOff;
  for (uint16_t i = 0; ; i++) {
    if (pos + 1 > n) return -1;
    uint8_t len = b[pos];
    if (pos + 1 + (size_t)len > n) return -1;
    if (i == idx) { *outPtr = b + pos + 1; return (int)len; }
    pos += 1 + (size_t)len;
  }
}

// Compose (group g, template t, entity e) into buf. e = -1 for a slotless
// template. isDoOut (if non-null) reports the group's Do-mode flag. Returns the
// composed length, or <0 on error (-2 bad index, -3 no menu). Assumes lock held.
static int composeLocked(uint8_t g, uint16_t t, int e, char* buf, size_t cap, bool* isDoOut) {
  if (!buf || cap == 0) return -2;
  if (gLLM.menuGroupCount == 0) return -3;
  if (g >= gLLM.menuGroupCount) return -2;
  const LLMMenuGroupDesc& d = gLLM.menuGroups[g];
  if (t >= d.tplCount) return -2;
  const uint8_t* tpl = nullptr;
  int tlen = menuItemAt(d.tplOff, t, &tpl);
  if (tlen < 0) return -2;
  if (isDoOut) *isDoOut = (d.flags & 0x01) != 0;

  // Locate the (optional) single slot marker.
  int slotIdx = -1;
  for (int k = 0; k < tlen; k++) if (tpl[k] == MENU_SLOT_BYTE) { slotIdx = k; break; }

  const uint8_t* ent = nullptr;
  int elen = 0;
  if (slotIdx >= 0) {
    if (e < 0 || e >= (int)d.entCount) return -2;   // a slot needs a valid entity
    elen = menuItemAt(d.entOff, (uint16_t)e, &ent);
    if (elen < 0) return -2;
  }

  size_t o = 0;
  for (int k = 0; k < tlen; k++) {
    if (k == slotIdx) {
      for (int j = 0; j < elen; j++) if (o + 1 < cap) buf[o++] = (char)ent[j];
    } else {
      if (o + 1 < cap) buf[o++] = (char)tpl[k];
    }
  }
  buf[o] = '\0';
  return (int)o;
}

// ============================================================================
// Public accessors (all copy out under the lock)
// ============================================================================

uint16_t llmMenuGeneration(void) {
  MenuLock lk;
  return gLLM.menuGeneration;
}

uint8_t llmMenuGroupCount(void) {
  MenuLock lk;
  return gLLM.menuGroupCount;
}

bool llmMenuGroupInfo(uint8_t g, LLMMenuGroupInfo* out) {
  if (!out) return false;
  MenuLock lk;
  if (gLLM.menuGroupCount == 0 || g >= gLLM.menuGroupCount) return false;
  const LLMMenuGroupDesc& d = gLLM.menuGroups[g];
  uint8_t nl = d.nameLen; if (nl > 32) nl = 32;
  if (nl > 0 && gLLM.menuBlob && (size_t)d.nameOff + nl <= gLLM.menuBytes)
    memcpy(out->name, gLLM.menuBlob + d.nameOff, nl);
  else
    nl = 0;
  out->name[nl]  = '\0';
  out->flags     = d.flags;
  out->tplCount  = d.tplCount;
  out->entCount  = d.entCount;
  return true;
}

int llmMenuTemplate(uint8_t g, uint16_t t, char* buf, size_t cap, bool* hasSlot) {
  if (!buf || cap == 0) return -1;
  buf[0] = '\0';
  MenuLock lk;
  if (gLLM.menuGroupCount == 0 || g >= gLLM.menuGroupCount) return -1;
  const LLMMenuGroupDesc& d = gLLM.menuGroups[g];
  if (t >= d.tplCount) return -1;
  const uint8_t* tpl = nullptr;
  int tlen = menuItemAt(d.tplOff, t, &tpl);
  if (tlen < 0) return -1;
  bool slot = false;
  size_t o = 0;
  for (int k = 0; k < tlen; k++) {
    if (tpl[k] == MENU_SLOT_BYTE) {         // display form: slot -> "{}"
      slot = true;
      if (o + 2 < cap) { buf[o++] = '{'; buf[o++] = '}'; }
    } else if (o + 1 < cap) {
      buf[o++] = (char)tpl[k];
    }
  }
  buf[o] = '\0';
  if (hasSlot) *hasSlot = slot;
  return (int)o;
}

int llmMenuEntity(uint8_t g, uint16_t e, char* buf, size_t cap) {
  if (!buf || cap == 0) return -1;
  buf[0] = '\0';
  MenuLock lk;
  if (gLLM.menuGroupCount == 0 || g >= gLLM.menuGroupCount) return -1;
  const LLMMenuGroupDesc& d = gLLM.menuGroups[g];
  if (e >= d.entCount) return -1;
  const uint8_t* ent = nullptr;
  int elen = menuItemAt(d.entOff, e, &ent);
  if (elen < 0) return -1;
  size_t o = 0;
  for (int k = 0; k < elen; k++) if (o + 1 < cap) buf[o++] = (char)ent[k];
  buf[o] = '\0';
  return (int)o;
}

int llmMenuCompose(uint8_t g, uint16_t t, int e, char* buf, size_t cap) {
  if (!buf || cap == 0) return -2;
  MenuLock lk;
  int r = composeLocked(g, t, e, buf, cap, nullptr);
  if (r < 0 && cap) buf[0] = '\0';
  return r;
}

int llmMenuAsk(uint16_t gen, uint8_t g, uint16_t t, int e, const ChatParamOverride* ov) {
  // Compose under the lock WITH the generation check (a gen check without the
  // lock would be a TOCTOU). composed is a private copy, so once the lock is
  // released nothing here touches the blob again.
  char composed[192];
  bool isDo = false;
  {
    MenuLock lk;
    if (gLLM.menuGroupCount == 0) return -3;
    if (gen != gLLM.menuGeneration) return -1;     // stale — model swapped under the caller
    int r = composeLocked(g, t, e, composed, sizeof(composed), &isDo);
    if (r < 0) return (r == -3) ? -3 : -2;
  }

  // Build the prompt + override outside the lock (chatBeginTurn takes its own
  // lock and must not nest under sMenuLock — µs-hold discipline).
  ChatParamOverride eff = ov ? *ov : ChatParamOverride{};
  char prompt[208];
  if (isDo) {
    // Do-mode: the "do: " lead-in makes llmFramePrompt emit the Do: scaffold;
    // force a short answer (hardCap=4, sentenceLimit=0), mirroring the web Do:
    // path. Menu-fired Do: results are SUGGESTIONS only — never auto-executed.
    snprintf(prompt, sizeof(prompt), "do: %s", composed);
    eff.hardCap = 4;
    eff.sentenceLimit = 0;
  } else {
    strlcpy(prompt, composed, sizeof(prompt));
  }

  int sid = chatBeginTurn(prompt, &eff);
  if (sid <= 0) return 0;   // busy (generation in flight) or model not ready

  // Record for the guided-retry path.
  {
    MenuLock lk;
    strlcpy(sLastComposed, composed, sizeof(sLastComposed));
    sLastAskSession = sid;
    sLastAskIsDo    = isDo;
  }
  return sid;
}

int llmMenuRepeatLast(void) {
  char composed[192];
  bool isDo = false;
  {
    MenuLock lk;
    if (sLastAskSession == 0 || sLastComposed[0] == '\0') return -2;   // nothing to repeat
    strlcpy(composed, sLastComposed, sizeof(composed));
    isDo = sLastAskIsDo;
  }
  ChatParamOverride eff{};
  char prompt[208];
  if (isDo) {
    snprintf(prompt, sizeof(prompt), "do: %s", composed);
    eff.hardCap = 4;
    eff.sentenceLimit = 0;
  } else {
    strlcpy(prompt, composed, sizeof(prompt));
  }
  int sid = chatBeginTurn(prompt, &eff);   // PLAIN re-ask: no suppress list
  if (sid <= 0) return 0;
  { MenuLock lk; sLastAskSession = sid; }
  return sid;
}

bool llmMenuLastAskInfo(int* sessionOut) {
  MenuLock lk;
  if (sLastAskSession == 0) return false;
  if (sessionOut) *sessionOut = sLastAskSession;
  return true;
}

// ============================================================================
// Commands (registered NON-admin in llmCommands[], System_LLM.cpp)
// ============================================================================

// Shared human-text scratch (reply is stamped OK: by executeCommand). PSRAM to
// keep it off the tight internal DRAM, like llmCmdBuf.
EXT_RAM_BSS_ATTR static char menuCmdBuf[1024];

// llmmenu — guided-menu status / paged listing.
//   llmmenu                            → human status (groups + counts)
//   llmmenu json                       → {"schema":1,"gen":G,"groups":[...]}
//   llmmenu json tpl <g> <off>         → {"schema":1,"gen":G,"g":,"off":,"total":,"items":[...]}
//   llmmenu json ent <g> <off>         → same shape with entity strings
const char* cmd_llm_menu(const String& args) {
  String a = args; a.trim();

  if (argLeadingTokenIsJson(a)) {
    CommandArgs ca(a);                       // arg(0)=="json"
    String sub = ca.arg(1);
    uint16_t gen = llmMenuGeneration();
    uint8_t  gc  = llmMenuGroupCount();

    if (sub == "tpl" || sub == "ent") {
      const bool wantTpl = (sub == "tpl");
      int g   = ca.argInt(2, -1);
      int off = ca.argInt(3, 0);
      if (g < 0 || off < 0)
        return "Error: invalid arguments — Usage: llmmenu json tpl|ent <group> <offset>";
      LLMMenuGroupInfo gi;
      if (gc == 0 || g >= (int)gc || !llmMenuGroupInfo((uint8_t)g, &gi))
        return "Error: no such group (llmmenu json)";
      const int total = wantTpl ? (int)gi.tplCount : (int)gi.entCount;
      // Page window keeps every reply <= ~3 KB (worst-case 120 B templates /
      // 48 B entities, JSON-escaped). Caller advances off by items.length.
      const int PAGE = wantTpl ? 16 : 24;

      PSRAM_JSON_DOC(doc);
      doc["schema"] = 1;
      doc["gen"]    = gen;
      doc["g"]      = g;
      doc["off"]    = off;
      doc["total"]  = total;
      JsonArray items = doc["items"].to<JsonArray>();
      char item[136];
      for (int i = off; i < total && i < off + PAGE; i++) {
        int n = wantTpl
                  ? llmMenuTemplate((uint8_t)g, (uint16_t)i, item, sizeof(item), nullptr)
                  : llmMenuEntity((uint8_t)g, (uint16_t)i, item, sizeof(item));
        if (n < 0) break;
        items.add(item);                     // char[] → copied into the doc pool
      }
      static char* jbuf = nullptr;
      if (!jbuf) jbuf = (char*)ps_alloc(3200, AllocPref::PreferPSRAM, "llmmenu_list.json");
      if (!jbuf) return "{\"error\":\"oom\"}";
      serializeJson(doc, jbuf, 3200);
      return jbuf;
    }

    // Default json form: the group list.
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["gen"]    = gen;
    JsonArray groups = doc["groups"].to<JsonArray>();
    for (uint8_t i = 0; i < gc; i++) {
      LLMMenuGroupInfo gi;
      if (!llmMenuGroupInfo(i, &gi)) continue;
      JsonObject o = groups.add<JsonObject>();
      o["i"]         = i;
      o["name"]      = gi.name;              // char[33] → copied
      o["mode"]      = (gi.flags & 0x01) ? "do" : "ask";
      o["templates"] = gi.tplCount;
      o["entities"]  = gi.entCount;
    }
    static char* gbuf = nullptr;
    if (!gbuf) gbuf = (char*)ps_alloc(1536, AllocPref::PreferPSRAM, "llmmenu_groups.json");
    if (!gbuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, gbuf, 1536);
    return gbuf;
  }

  // Human status. No menu is a first-class state, not an error.
  uint16_t gen = llmMenuGeneration();
  uint8_t  gc  = llmMenuGroupCount();
  if (gc == 0)
    return "No guided menu (this model ships no question templates). Ask freely with llmgenerate.";

  size_t o = 0;
  int w = snprintf(menuCmdBuf, sizeof(menuCmdBuf),
                   "Guided menu: %u group(s), gen %u\n", (unsigned)gc, (unsigned)gen);
  if (w > 0) o = (size_t)w;
  if (o >= sizeof(menuCmdBuf)) o = sizeof(menuCmdBuf) - 1;
  for (uint8_t i = 0; i < gc && o < sizeof(menuCmdBuf) - 1; i++) {
    LLMMenuGroupInfo gi;
    if (!llmMenuGroupInfo(i, &gi)) continue;
    w = snprintf(menuCmdBuf + o, sizeof(menuCmdBuf) - o,
                 "  [%u] %s  (%s, %u templates, %u entities)\n",
                 (unsigned)i, gi.name, (gi.flags & 0x01) ? "do" : "ask",
                 (unsigned)gi.tplCount, (unsigned)gi.entCount);
    if (w > 0) o += (size_t)w;
    if (o >= sizeof(menuCmdBuf)) { o = sizeof(menuCmdBuf) - 1; break; }
  }
  cliHint("ask a guided question with 'llmask <group> <template> [entity]' "
          "(indices from 'llmmenu json')");
  return menuCmdBuf;
}

// Map an llmMenuAsk/RepeatLast return <= 0 to the contract reply text.
static const char* menuAskErr(int rc, bool json) {
  switch (rc) {
    case 0:  return json ? "{\"schema\":1,\"ok\":false,\"error\":\"busy - answer in progress\"}"
                         : "Error: busy - answer in progress";
    case -1: return json ? "{\"schema\":1,\"ok\":false,\"error\":\"menu changed - refetch (llmmenu json)\"}"
                         : "Error: menu changed - refetch (llmmenu json)";
    case -2: return json ? "{\"schema\":1,\"ok\":false,\"error\":\"bad index\"}"
                         : "Error: bad index (see llmmenu)";
    case -3: return json ? "{\"schema\":1,\"ok\":false,\"error\":\"no guided menu\"}"
                         : "Error: no guided menu for this model";
    default: return json ? "{\"schema\":1,\"ok\":false,\"error\":\"ask failed\"}"
                         : "Error: ask failed";
  }
}

// llmask — submit a guided question (composition happens on-device).
//   llmask <g> <t> [e]                 (human; reads current gen)
//   llmask json <gen> <g> <t> [e]      → {"schema":1,"ok":true,"session":N,"q":"..."}
//   llmask repeat / llmask json repeat → re-ask the last guided question PLAIN
const char* cmd_llm_ask(const String& args) {
  String a = args; a.trim();
  const bool json = argLeadingTokenIsJson(a);
  CommandArgs ca(a);
  const int base = json ? 1 : 0;             // skip the leading "json" token
  String first = ca.arg(base);

  if (!llmIsReady())
    return json ? "{\"schema\":1,\"ok\":false,\"error\":\"model not ready\"}"
                : "Error: no model loaded";

  // Re-ask the last guided question (PLAIN — no suppress list).
  if (first == "repeat") {
    int sid = llmMenuRepeatLast();
    if (sid <= 0) return menuAskErr(sid, json);
    if (json) {
      snprintf(menuCmdBuf, sizeof(menuCmdBuf),
               "{\"schema\":1,\"ok\":true,\"session\":%d,"
               "\"hint\":\"the reply streams in asynchronously - read it with 'llmresult json 0'\"}",
               sid);
      return menuCmdBuf;
    }
    snprintf(menuCmdBuf, sizeof(menuCmdBuf), "re-asking last guided question (session %d)", sid);
    cliHint("read the answer with 'llmresult json 0'");
    return menuCmdBuf;
  }

  // Parse indices.
  uint16_t gen = 0;
  int g, t, e;
  if (json) {
    // json <gen> <g> <t> [e] — gen is REQUIRED on the json form.
    if (!ca.has(1) || !ca.has(2) || !ca.has(3))
      return "{\"schema\":1,\"ok\":false,\"error\":\"usage: llmask json <gen> <g> <t> [e]\"}";
    gen = (uint16_t)ca.argInt(1, 0);
    g   = ca.argInt(2, -1);
    t   = ca.argInt(3, -1);
    e   = ca.has(4) ? ca.argInt(4, -1) : -1;
  } else {
    // <g> <t> [e] — human form reads the CURRENT gen; llmMenuAsk still errors
    // cleanly (-1) if a model swap raced between this read and the compose.
    if (!ca.has(0) || !ca.has(1))
      return "Error: invalid arguments — Usage: llmask <group> <template> [entity]";
    gen = llmMenuGeneration();
    g   = ca.argInt(0, -1);
    t   = ca.argInt(1, -1);
    e   = ca.has(2) ? ca.argInt(2, -1) : -1;
  }
  if (g < 0 || t < 0)
    return json ? "{\"schema\":1,\"ok\":false,\"error\":\"bad index\"}"
                : "Error: bad index (see llmmenu)";

  // Compose the display question up-front so success can echo it. The ask
  // re-validates gen under the lock, so a raced swap still fails cleanly there.
  char q[192];
  int qn = llmMenuCompose((uint8_t)g, (uint16_t)t, e, q, sizeof(q));

  int sid = llmMenuAsk(gen, (uint8_t)g, (uint16_t)t, e, nullptr);
  if (sid <= 0) return menuAskErr(sid, json);

  if (json) {
    PSRAM_JSON_DOC(doc);
    doc["schema"]  = 1;
    doc["ok"]      = true;
    doc["session"] = sid;
    doc["q"]       = (qn >= 0) ? q : "";      // char[] → copied; JSON-escaped
    doc["hint"]    = "the reply streams in asynchronously - read it with 'llmresult json 0'";
    static char* abuf = nullptr;
    if (!abuf) abuf = (char*)ps_alloc(768, AllocPref::PreferPSRAM, "llmask.json");
    if (!abuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, abuf, 768);
    return abuf;
  }

  snprintf(menuCmdBuf, sizeof(menuCmdBuf), "asking \"%s\" (session %d)",
           (qn >= 0) ? q : "?", sid);
  cliHint("read the answer with 'llmresult json 0'");
  return menuCmdBuf;
}

#endif // ENABLE_ONDEVICE_LLM
