// System_LLMCm5.cpp — CM5 co-processor as an LLM answer source.
// Protocol and escaping contract are documented in System_LLMCm5.h.

#include "System_LLMCm5.h"

#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_CM5

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "System_Cm5Presence.h"
#include "System_Debug.h"
#include "System_UartLink.h"

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// The intrinsic runs on the UART drain task while surfaces poll from the OLED /
// web / BLE tasks, so every field below is written or read under sMux. The
// result buffer is PSRAM (2 KB of prose is not worth internal DRAM and is not
// secret); everything else is small enough to stay in DRAM where the critical
// sections are cheap.

portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;

struct Cm5Model {
  char     name[LLM_MODEL_NAME_LEN];
  uint32_t sizeKB;
};

Cm5Model sCatalog[CM5_LLM_MAX_MODELS];
uint8_t  sCatalogCount = 0;
uint32_t sCatalogGen   = 0;      // host-supplied; bumps when the host's set changes

char sActiveName[LLM_MODEL_NAME_LEN] = {0};
bool sSelected  = false;          // user picked a model
bool sHostReady = false;          // host confirmed it is serving sActiveName
uint32_t sSelectStartedMs = 0;    // 0 = no select pending; armed by select, disarmed by `ready` or the watchdog in cm5LlmTick
// Last measured load percentage the host reported (0..100), ratcheted upward.
// Display only — nothing reads it to make a decision.
uint8_t  sLoadPct = 0;

EXT_RAM_BSS_ATTR char sResult[2048];
int      sResultLen  = 0;
int      sSessionId  = 0;         // 0 = never generated
bool     sDone       = true;      // idle counts as done
uint32_t sLastEventMs = 0;        // last push/ask — drives the stall timeout
uint16_t sExpectSeq  = 0;
int      sLastTokens = 0;
float    sLastTps    = 0.0f;
uint32_t sGenEpoch   = 0;         // UART named-session epoch owning the live generation
char     sLastError[64] = {0};

int  sNextSession = 1;

// ---------------------------------------------------------------------------
// Escaping
// ---------------------------------------------------------------------------

// Decode in place-safe fashion into `out`. Returns bytes written (NUL-terminated).
size_t cm5LlmUnescape(const char* in, size_t inLen, char* out, size_t outCap) {
  if (!in || !out || outCap == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; i < inLen && o + 1 < outCap; i++) {
    if (in[i] != '\\' || i + 1 >= inLen) { out[o++] = in[i]; continue; }
    const char c = in[++i];
    switch (c) {
      case 'n':  out[o++] = '\n'; break;
      case 'r':  out[o++] = '\r'; break;
      case 't':  out[o++] = '\t'; break;
      case 's':  out[o++] = ' ';  break;
      case '\\': out[o++] = '\\'; break;
      // Unknown escape: emit the character itself rather than dropping it, so a
      // newer host adding an escape degrades to readable text instead of a hole.
      default:   out[o++] = c;    break;
    }
  }
  out[o] = '\0';
  return o;
}

// Encode for the outbound EVT. Escapes every whitespace byte so the payload can
// never be damaged by trimming or re-tokenizing on the far side either.
size_t cm5LlmEscape(const char* in, char* out, size_t outCap) {
  if (!in || !out || outCap == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < outCap; i++) {
    const unsigned char c = (unsigned char)in[i];
    char esc = 0;
    switch (c) {
      case '\\': esc = '\\'; break;
      case '\n': esc = 'n';  break;
      case '\r': esc = 'r';  break;
      case '\t': esc = 't';  break;
      case ' ':  esc = 's';  break;
      default: break;
    }
    if (esc) { out[o++] = '\\'; out[o++] = esc; }
    else     { out[o++] = (char)c; }
  }
  out[o] = '\0';
  return o;
}

// ---------------------------------------------------------------------------
// Raw-line token helpers
// ---------------------------------------------------------------------------
// Deliberately NOT CommandArgs: the tail of a push line is payload, and handing
// it to a tokenizer is how the escaping contract would get quietly undone.

// Advance past one whitespace-delimited token. Returns the start of the next
// token, or nullptr at end of line.
const char* nextTok(const char* p) {
  if (!p) return nullptr;
  while (*p && !isspace((unsigned char)*p)) p++;
  while (*p && isspace((unsigned char)*p))  p++;
  return *p ? p : nullptr;
}

bool tokIs(const char* p, const char* lit) {
  if (!p) return false;
  const size_t n = strlen(lit);
  if (strncasecmp(p, lit, n) != 0) return false;
  return p[n] == '\0' || isspace((unsigned char)p[n]);
}

bool tokU32(const char* p, uint32_t* out) {
  if (!p || !isdigit((unsigned char)*p)) return false;
  uint32_t v = 0;
  while (*p && isdigit((unsigned char)*p)) {
    const uint32_t d = (uint32_t)(*p++ - '0');
    if (v > (UINT32_MAX - d) / 10u) return false;
    v = v * 10u + d;
  }
  if (*p && !isspace((unsigned char)*p)) return false;
  *out = v;
  return true;
}

// Copy one token into a fixed buffer.
void tokCopy(const char* p, char* out, size_t cap) {
  size_t i = 0;
  while (p && p[i] && !isspace((unsigned char)p[i]) && i + 1 < cap) { out[i] = p[i]; i++; }
  out[i] = '\0';
}

void resetGenerationLocked(const char* why) {
  sResultLen = 0;
  sDone      = true;
  sExpectSeq = 0;
  sGenEpoch  = 0;
  if (why) strlcpy(sLastError, why, sizeof(sLastError));
}

}  // namespace

// ---------------------------------------------------------------------------
// Inbound intrinsic
// ---------------------------------------------------------------------------

bool cm5LlmIsCallbackLine(const char* line) {
  if (!line) return false;
  while (*line && isspace((unsigned char)*line)) line++;
  if (strncasecmp(line, "cm5", 3) != 0) return false;
  const char* t1 = nextTok(line);
  return tokIs(t1, "llm");
}

Cm5LlmCallbackResult cm5LlmHandleCallbackIntrinsic(
    const char* line, uint32_t namedSessionEpoch, bool sessionMayControl,
    char* reply, size_t replySize) {
  if (!cm5LlmIsCallbackLine(line)) return Cm5LlmCallbackResult::NotCallback;
  if (!reply || replySize == 0)    return Cm5LlmCallbackResult::NotCallback;

  // From here the line is OURS on every path, including refusals — a host retry
  // must never fall through to cmd_exec and land in the durable command audit.
  auto fail = [&](const char* msg) {
    snprintf(reply, replySize, "ERROR %s", msg);
    return Cm5LlmCallbackResult::Handled;
  };

  if (namedSessionEpoch == 0) return fail("no authenticated uart session");
  if (!sessionMayControl)     return fail("session may not control this device");

  const char* verb = nextTok(nextTok(line));   // skip "cm5" "llm"
  if (!verb) return fail("usage: cm5 llm <models|loading|ready|push|end> ...");

  // ---- cm5 llm models <gen> <idx> <count> <sizeMB> <name> ----------------
  if (tokIs(verb, "models")) {
    const char* pGen = nextTok(verb);
    const char* pIdx = nextTok(pGen);
    const char* pCnt = nextTok(pIdx);
    const char* pMB  = nextTok(pCnt);
    const char* pNm  = nextTok(pMB);
    uint32_t gen = 0, idx = 0, cnt = 0, mb = 0;
    if (!tokU32(pGen, &gen) || !tokU32(pIdx, &idx) ||
        !tokU32(pCnt, &cnt) || !tokU32(pMB, &mb) || !pNm) {
      return fail("usage: cm5 llm models <gen> <idx> <count> <sizeMB> <name>");
    }
    if (cnt > CM5_LLM_MAX_MODELS) cnt = CM5_LLM_MAX_MODELS;
    if (idx >= CM5_LLM_MAX_MODELS) {
      DEBUG_UART_CONTROLF("[LLM-CM5] catalog row %lu dropped (cap %d)",
                          (unsigned long)idx, CM5_LLM_MAX_MODELS);
      snprintf(reply, replySize, "OK dropped");
      return Cm5LlmCallbackResult::Handled;
    }
    char nameBuf[LLM_MODEL_NAME_LEN];
    tokCopy(pNm, nameBuf, sizeof(nameBuf));

    portENTER_CRITICAL(&sMux);
    // A new generation restarts the table so a shrinking catalog cannot leave
    // stale rows behind.
    if (gen != sCatalogGen) { sCatalogGen = gen; sCatalogCount = 0; }
    strlcpy(sCatalog[idx].name, nameBuf, sizeof(sCatalog[idx].name));
    sCatalog[idx].sizeKB = mb * 1024u;
    if (idx + 1 > sCatalogCount) sCatalogCount = (uint8_t)(idx + 1);
    if (sCatalogCount > cnt && cnt > 0) sCatalogCount = (uint8_t)cnt;
    portEXIT_CRITICAL(&sMux);

    snprintf(reply, replySize, "OK %lu", (unsigned long)idx);
    return Cm5LlmCallbackResult::Handled;
  }

  // ---- cm5 llm loading <gen> <pct> --------------------------------------
  // Measured load progress, pushed ~1 Hz while the host populates a model.
  // Cosmetic BY CONSTRUCTION: it touches only sLoadPct, so a hostile or
  // malformed value cannot affect selection, readiness, freshness or any guard
  // — the worst it can do is draw a wrong bar. That is why it needs no fencing
  // beyond the range clamp, unlike `ready`/`end` which mutate real state.
  if (tokIs(verb, "loading")) {
    const char* pGen = nextTok(verb);
    const char* pPct = nextTok(pGen);
    uint32_t gen = 0, pct = 0;
    if (!tokU32(pGen, &gen) || !tokU32(pPct, &pct)) {
      return fail("usage: cm5 llm loading <gen> <pct>");
    }
    if (pct > 100) pct = 100;
    portENTER_CRITICAL(&sMux);
    // Only while a select is actually pending. A late line from a superseded
    // load must not paint over a model that has already finished loading.
    if (sSelectStartedMs != 0 && !sHostReady) {
      // Ratchet: residency can dip on the host, and a bar that walks backwards
      // reads as a fault rather than as noise.
      if ((uint8_t)pct > sLoadPct) sLoadPct = (uint8_t)pct;
    }
    portEXIT_CRITICAL(&sMux);
    snprintf(reply, replySize, "OK %lu", (unsigned long)pct);
    return Cm5LlmCallbackResult::Handled;
  }

  // ---- cm5 llm ready <gen> <name> ---------------------------------------
  if (tokIs(verb, "ready")) {
    const char* pGen = nextTok(verb);
    const char* pNm  = nextTok(pGen);
    uint32_t gen = 0;
    if (!tokU32(pGen, &gen) || !pNm) return fail("usage: cm5 llm ready <gen> <name>");
    char nameBuf[LLM_MODEL_NAME_LEN];
    tokCopy(pNm, nameBuf, sizeof(nameBuf));

    portENTER_CRITICAL(&sMux);
    sCatalogGen = gen;
    // The host is authoritative about what it is actually serving. Adopt its
    // name so a switch that resolved to a different file (or a host restart
    // under a different config) is reflected rather than silently disagreeing.
    strlcpy(sActiveName, nameBuf, sizeof(sActiveName));
    sSelected  = true;
    sHostReady = true;
    // Disarm the select watchdog at the mutation site as well as in the tick.
    // This is also the recovery path: a `ready` that arrives AFTER the watchdog
    // gave up re-adopts the model cleanly, because every field it needs is set
    // here unconditionally.
    sSelectStartedMs = 0;
    sLastError[0] = '\0';
    portEXIT_CRITICAL(&sMux);

    snprintf(reply, replySize, "OK %s", nameBuf);
    return Cm5LlmCallbackResult::Handled;
  }

  // ---- cm5 llm push <session> <seq> <escaped-text> -----------------------
  if (tokIs(verb, "push")) {
    const char* pSess = nextTok(verb);
    const char* pSeq  = nextTok(pSess);
    const char* pTxt  = nextTok(pSeq);
    uint32_t sess = 0, seq = 0;
    if (!tokU32(pSess, &sess) || !tokU32(pSeq, &seq)) {
      return fail("usage: cm5 llm push <session> <seq> <text>");
    }
    // An empty tail is legal (a chunk that escaped to nothing); treat as no-op.
    const size_t rawLen = pTxt ? strlen(pTxt) : 0;

    char decoded[256];
    const size_t dlen = rawLen ? cm5LlmUnescape(pTxt, rawLen, decoded, sizeof(decoded)) : 0;

    portENTER_CRITICAL(&sMux);
    const bool sessionOk = ((int)sess == sSessionId) && sSessionId != 0;
    const bool epochOk   = (sGenEpoch != 0 && sGenEpoch == namedSessionEpoch);
    const bool live      = !sDone;
    bool seqOk = false;
    if (sessionOk && epochOk && live) {
      // Idempotent by seq: a daemon-side retry that replays a chunk must not
      // duplicate answer text. Anything older than expected is a replay and is
      // accepted-but-ignored; anything newer means we lost a chunk.
      if (seq == sExpectSeq) {
        seqOk = true;
        sExpectSeq++;
        const int room = (int)sizeof(sResult) - 1 - sResultLen;
        if (room > 0) {
          const int n = (int)dlen < room ? (int)dlen : room;
          memcpy(sResult + sResultLen, decoded, (size_t)n);
          sResultLen += n;
          sResult[sResultLen] = '\0';
        }
        sLastEventMs = millis();
      } else if (seq < sExpectSeq) {
        seqOk = true;                       // replay — already applied
      }
    }
    const bool gap = sessionOk && epochOk && live && !seqOk;
    if (gap) resetGenerationLocked("lost a chunk from the host");
    portEXIT_CRITICAL(&sMux);

    if (!sessionOk) return fail("stale session");
    if (!epochOk)   return fail("session epoch mismatch");
    if (!live)      return fail("no generation in flight");
    if (gap)        return fail("sequence gap");

    snprintf(reply, replySize, "OK %lu", (unsigned long)seq);
    return Cm5LlmCallbackResult::Handled;
  }

  // ---- cm5 llm end <session> <status> [tokens] [tokPerSecX10] -----------
  if (tokIs(verb, "end")) {
    const char* pSess = nextTok(verb);
    const char* pSt   = nextTok(pSess);
    const char* pTok  = nextTok(pSt);
    const char* pTps  = nextTok(pTok);
    uint32_t sess = 0, toks = 0, tpsX10 = 0;
    if (!tokU32(pSess, &sess) || !pSt) {
      return fail("usage: cm5 llm end <session> <ok|error|stopped> [tokens] [tokPerSecX10]");
    }
    (void)tokU32(pTok, &toks);
    (void)tokU32(pTps, &tpsX10);
    const bool errored = tokIs(pSt, "error");

    portENTER_CRITICAL(&sMux);
    const bool sessionOk = ((int)sess == sSessionId) && sSessionId != 0;
    const bool epochOk   = (sGenEpoch != 0 && sGenEpoch == namedSessionEpoch);
    if (sessionOk && epochOk) {
      sDone       = true;
      sGenEpoch   = 0;
      sLastTokens = (int)toks;
      sLastTps    = (float)tpsX10 / 10.0f;
      if (errored) strlcpy(sLastError, "host reported a generation error", sizeof(sLastError));
    }
    portEXIT_CRITICAL(&sMux);

    if (!sessionOk) return fail("stale session");
    if (!epochOk)   return fail("session epoch mismatch");
    snprintf(reply, replySize, "OK");
    return Cm5LlmCallbackResult::Handled;
  }

  return fail("unknown verb — expected models|loading|ready|push|end");
}

// ---------------------------------------------------------------------------
// Source implementation
// ---------------------------------------------------------------------------

size_t cm5LlmEnumerate(LlmModelDesc* out, size_t cap) {
  if (!out || cap == 0) return 0;

  const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
  // Listed-but-unavailable rather than hidden: a model that vanishes from the
  // picker the moment the lease lapses reads as "the device forgot", which is
  // the wrong story and hides the actual fault.
  const bool usable = pres.seenForSession && pres.fresh &&
                      (pres.mode == Cm5PresenceMode::Ready ||
                       pres.mode == Cm5PresenceMode::Busy);

  Cm5Model snap[CM5_LLM_MAX_MODELS];
  uint8_t n;
  portENTER_CRITICAL(&sMux);
  n = sCatalogCount;
  if (n > CM5_LLM_MAX_MODELS) n = CM5_LLM_MAX_MODELS;
  memcpy(snap, sCatalog, sizeof(Cm5Model) * n);
  portEXIT_CRITICAL(&sMux);

  size_t w = 0;
  for (uint8_t i = 0; i < n && w < cap; i++) {
    if (!snap[i].name[0]) continue;
    LlmModelDesc& d = out[w];
    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "cm5:%s", snap[i].name);
    strlcpy(d.name, snap[i].name, sizeof(d.name));
    d.backend   = LlmBackendKind::Cm5;
    d.path[0]   = '\0';
    d.sizeKB    = snap[i].sizeKB;
    d.storage   = LLM_STORAGE_REMOTE;
    d.available = usable;
    w++;
  }
  return w;
}

bool cm5LlmFindByName(const char* name, LlmModelDesc* out) {
  if (!name || !*name || !out) return false;

  char found[LLM_MODEL_NAME_LEN] = {0};
  uint32_t sizeKB = 0;
  portENTER_CRITICAL(&sMux);
  for (uint8_t i = 0; i < sCatalogCount && i < CM5_LLM_MAX_MODELS; i++) {
    if (sCatalog[i].name[0] && strcasecmp(sCatalog[i].name, name) == 0) {
      strlcpy(found, sCatalog[i].name, sizeof(found));
      sizeKB = sCatalog[i].sizeKB;
      break;
    }
  }
  portEXIT_CRITICAL(&sMux);
  if (!found[0]) return false;

  const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
  memset(out, 0, sizeof(*out));
  snprintf(out->id, sizeof(out->id), "cm5:%s", found);
  strlcpy(out->name, found, sizeof(out->name));
  out->backend   = LlmBackendKind::Cm5;
  out->path[0]   = '\0';
  out->sizeKB    = sizeKB;
  out->storage   = LLM_STORAGE_REMOTE;
  out->available = pres.seenForSession && pres.fresh &&
                   (pres.mode == Cm5PresenceMode::Ready ||
                    pres.mode == Cm5PresenceMode::Busy);
  return true;
}

bool cm5LlmSelectByName(const char* name, char* errOut, size_t errCap) {
  auto bail = [&](const char* m) {
    if (errOut && errCap) strlcpy(errOut, m, errCap);
    return false;
  };
  if (!name || !*name) return bail("no model name");

  const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
  if (!pres.seenForSession) return bail("CM5 service has not checked in");
  if (!pres.fresh)          return bail("CM5 presence lease is stale");

  char evt[96];
  char esc[LLM_MODEL_NAME_LEN * 2 + 2];
  cm5LlmEscape(name, esc, sizeof(esc));
  snprintf(evt, sizeof(evt), "llm_select %s", esc);
  if (!uartLinkPushEventForSession(pres.sessionEpoch, evt)) {
    return bail("could not reach the CM5 over the UART link");
  }

  portENTER_CRITICAL(&sMux);
  strlcpy(sActiveName, name, sizeof(sActiveName));
  sSelected  = true;
  // Not ready until the host says so: switching models restarts its server,
  // which takes seconds. `cm5 llm ready` flips this.
  sHostReady = false;
  // Arm the select watchdog. The protocol has NO verb for "that select failed"
  // — models/loading/ready/push/end is the whole inbound vocabulary, and `end` is fenced
  // on a generation session a select never mints — so a host that accepts
  // llm_select and then fails leaves this in LOADING with no way to say so. The
  // generation stall timer does not cover it either: that one is gated on
  // !sDone, and a select leaves sDone true. Without a timer here, LOADING is
  // permanent until a human runs llmunload.
  sSelectStartedMs = millis();
  if (sSelectStartedMs == 0) sSelectStartedMs = 1;   // 0 means "not pending"
  sLoadPct = 0;                    // a new load starts an empty bar, not the last one's
  strlcpy(sLastError, "waiting for the CM5 to load this model", sizeof(sLastError));
  portEXIT_CRITICAL(&sMux);
  return true;
}

uint8_t cm5LlmLoadPercent() {
  uint8_t pct;
  portENTER_CRITICAL(&sMux);
  pct = sLoadPct;
  portEXIT_CRITICAL(&sMux);
  return pct;
}

uint32_t cm5LlmSelectPendingMs() {
  uint32_t started;
  portENTER_CRITICAL(&sMux);
  started = sSelectStartedMs;
  portEXIT_CRITICAL(&sMux);
  if (started == 0) return 0;
  const uint32_t elapsed = (uint32_t)(millis() - started);
  // Never report 0 while a select is genuinely pending — the caller uses 0 as
  // "nothing in flight", and the first millisecond would otherwise read as done.
  return elapsed ? elapsed : 1;
}

void cm5LlmUnload() {
  portENTER_CRITICAL(&sMux);
  sSelected  = false;
  sHostReady = false;
  sSelectStartedMs = 0;           // an unload cancels any pending select
  sLoadPct = 0;
  sActiveName[0] = '\0';
  resetGenerationLocked(nullptr);
  portEXIT_CRITICAL(&sMux);
}

bool cm5LlmIsReady() {
  bool sel, ready;
  portENTER_CRITICAL(&sMux);
  sel = sSelected; ready = sHostReady;
  portEXIT_CRITICAL(&sMux);
  if (!sel || !ready) return false;
  const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
  return pres.seenForSession && pres.fresh &&
         (pres.mode == Cm5PresenceMode::Ready || pres.mode == Cm5PresenceMode::Busy);
}

LLMStatus cm5LlmStatus() {
  LLMStatus st;
  memset(&st, 0, sizeof(st));
  bool sel, ready, done;
  portENTER_CRITICAL(&sMux);
  sel = sSelected; ready = sHostReady; done = sDone;
  snprintf(st.modelPath, sizeof(st.modelPath), "cm5:%s", sActiveName);
  st.lastTokenCount   = sLastTokens;
  st.lastTokensPerSec = sLastTps;
  strlcpy(st.errorMsg, sLastError, sizeof(st.errorMsg));
  portEXIT_CRITICAL(&sMux);

  if (!sel)                st.state = LLMState::UNLOADED;
  else if (!ready)         st.state = LLMState::LOADING;   // host is switching
  else if (!done)          st.state = LLMState::GENERATING;
  else                     st.state = LLMState::READY;

  // The presence lease is part of the truth. cm5LlmIsReady() already consults
  // it and cm5LlmStartAsync() already refuses without it, but status() did not
  // — so every surface would keep rendering READY on a model whose host had
  // been gone for minutes, which is the sharpest form of "the screen shows a
  // model that cannot answer". Every surface renders from status, so one check
  // here makes all of them honest at once.
  if (sel) {
    const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
    if (!pres.fresh || !pres.seenForSession) {
      st.state = LLMState::ERROR;
      strlcpy(st.errorMsg, "the CM5 is not reachable", sizeof(st.errorMsg));
    }
  }
  // config/PSRAM fields stay zeroed — they describe a local checkpoint and mean
  // nothing here. Surfaces branch on the backend kind rather than render them.
  return st;
}

int cm5LlmStartAsync(const char* prompt, const LLMGenParams& params) {
  if (!prompt || !*prompt) return 0;
  if (!cm5LlmIsReady())    return 0;

  const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
  if (!pres.fresh || pres.sessionEpoch == 0) return 0;

  // Reject on the RAW length first: escaping only ever grows a string, so a raw
  // prompt already over budget cannot possibly fit escaped, and this keeps the
  // scratch buffer from having to absorb an arbitrarily long input.
  if (strlen(prompt) > CM5_LLM_MAX_PROMPT_ESCAPED) return 0;

  // Then escape and check what ACTUALLY goes on the wire. Sized for the
  // worst case (every byte escapes to two) so the escape never truncates
  // silently — a truncated prompt that passed the check would be worse than a
  // rejected one.
  static EXT_RAM_BSS_ATTR char escaped[CM5_LLM_MAX_PROMPT_ESCAPED * 2 + 4];
  cm5LlmEscape(prompt, escaped, sizeof(escaped));
  if (strlen(escaped) > CM5_LLM_MAX_PROMPT_ESCAPED) return 0;

  int session;
  portENTER_CRITICAL(&sMux);
  if (!sDone) { portEXIT_CRITICAL(&sMux); return 0; }   // one generation at a time
  session      = sNextSession++;
  if (sNextSession <= 0) sNextSession = 1;
  sSessionId   = session;
  sResultLen   = 0;
  sResult[0]   = '\0';
  sDone        = false;
  sExpectSeq   = 0;
  sLastTokens  = 0;
  sLastTps     = 0.0f;
  sGenEpoch    = pres.sessionEpoch;
  sLastEventMs = millis();
  sLastError[0] = '\0';
  portEXIT_CRITICAL(&sMux);

  // Params the host can actually honour. suppressTokens are deliberately absent:
  // they are token ids from THIS device's tokenizer and mean nothing remotely.
  static EXT_RAM_BSS_ATTR char evt[CM5_LLM_MAX_PROMPT_ESCAPED + 64];
  snprintf(evt, sizeof(evt), "llm_ask %d %d %d %d %s",
           session, params.maxTokens,
           (int)(params.temperature * 100.0f), (int)(params.topp * 100.0f),
           escaped);

  if (!uartLinkPushEventForSession(pres.sessionEpoch, evt)) {
    portENTER_CRITICAL(&sMux);
    resetGenerationLocked("could not reach the CM5 over the UART link");
    portEXIT_CRITICAL(&sMux);
    return 0;
  }
  DEBUG_UART_CONTROLF("[LLM-CM5] ask session=%d maxTok=%d", session, params.maxTokens);
  return session;
}

void cm5LlmStop() {
  int session; uint32_t epoch;
  portENTER_CRITICAL(&sMux);
  session = sSessionId; epoch = sGenEpoch;
  if (!sDone) {
    sDone     = true;         // stop locally regardless of whether the host hears us
    sGenEpoch = 0;
  }
  portEXIT_CRITICAL(&sMux);
  if (session && epoch) {
    char evt[48];
    snprintf(evt, sizeof(evt), "llm_cancel %d", session);
    (void)uartLinkTryPushEventForSession(epoch, evt);
  }
}

int cm5LlmSessionId() {
  portENTER_CRITICAL(&sMux);
  const int s = sSessionId;
  portEXIT_CRITICAL(&sMux);
  return s;
}

int cm5LlmResultLen() {
  portENTER_CRITICAL(&sMux);
  const int n = sResultLen;
  portEXIT_CRITICAL(&sMux);
  return n;
}

int cm5LlmResultChunk(int offset, char* buf, int maxLen) {
  if (!buf || maxLen <= 1 || offset < 0) return 0;
  portENTER_CRITICAL(&sMux);
  int n = sResultLen - offset;
  if (n < 0) n = 0;
  if (n > maxLen - 1) n = maxLen - 1;
  if (n > 0) memcpy(buf, sResult + offset, (size_t)n);
  portEXIT_CRITICAL(&sMux);
  buf[n > 0 ? n : 0] = '\0';
  return n > 0 ? n : 0;
}

bool cm5LlmIsDone() {
  portENTER_CRITICAL(&sMux);
  const bool d = sDone;
  portEXIT_CRITICAL(&sMux);
  return d;
}

void cm5LlmTick() {
  bool wedged = false;
  portENTER_CRITICAL(&sMux);
  if (!sDone && (millis() - sLastEventMs) > CM5_LLM_STALL_MS) {
    resetGenerationLocked("the CM5 stopped responding mid-answer");
    wedged = true;
  }
  portEXIT_CRITICAL(&sMux);

  if (wedged) {
    // Without this the chat layer's streaming slot never clears and EVERY
    // surface is stuck on a turn that will never finish.
    DEBUG_UART_CONTROLF("[LLM-CM5] generation abandoned after %lums with no host activity",
                        (unsigned long)CM5_LLM_STALL_MS);
    return;
  }

  // A select that the host never confirms. Deliberately generous: a cold GGUF
  // on a spinning-up llama-server is tens of seconds, so this must sit well
  // above CM5_LLM_STALL_MS — it is bounding "never", not "slow". Presence loss
  // fails a pending select immediately rather than waiting the budget out,
  // because a host that is gone is not going to confirm anything.
  bool selectFailed = false;
  const Cm5PresenceSnapshot selPres = cm5PresenceSnapshot();
  portENTER_CRITICAL(&sMux);
  if (sSelectStartedMs != 0 && sSelected && !sHostReady) {
    const bool timedOut =
        (millis() - sSelectStartedMs) > CM5_LLM_SELECT_TIMEOUT_MS;
    const bool hostGone = !selPres.fresh || !selPres.seenForSession;
    if (timedOut || hostGone) {
      sSelectStartedMs = 0;
      sLoadPct = 0;               // abandoned: a stale bar would outlive its load
      sSelected = false;          // stop claiming a model the host never took
      sHostReady = false;
      strlcpy(sLastError,
              hostGone ? "the CM5 went away before it loaded the model"
                       : "the CM5 never confirmed the model",
              sizeof(sLastError));
      selectFailed = true;
    }
  } else if (sSelectStartedMs != 0 && sHostReady) {
    sSelectStartedMs = 0;         // confirmed — disarm
    sLoadPct = 0;                 // the bar's job is over; READY renders instead
  }
  portEXIT_CRITICAL(&sMux);
  if (selectFailed) {
    DEBUG_UART_CONTROLF("[LLM-CM5] model select abandoned — host never sent `ready`");
  }

  // Losing the presence lease mid-answer is the same wedge by another route.
  bool live;
  portENTER_CRITICAL(&sMux);
  live = !sDone;
  portEXIT_CRITICAL(&sMux);
  if (!live) return;
  const Cm5PresenceSnapshot pres = cm5PresenceSnapshot();
  if (!pres.fresh || !pres.seenForSession) {
    portENTER_CRITICAL(&sMux);
    resetGenerationLocked("the CM5 went away mid-answer");
    portEXIT_CRITICAL(&sMux);
    DEBUG_UART_CONTROLF("[LLM-CM5] generation abandoned — presence lease lost");
  }
}

#endif // ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_CM5
