// =============================================================================
// G2 hijack command bridge — implementation.
// See G2_HijackCmd.h for the public contract and docs/G2_REFACTOR_PROPOSAL.md
// §5 for architectural context.
// =============================================================================

#include "G2_HijackCmd.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include <new>          // std::nothrow
#include "System_CommandTypes.h"
#include "System_User.h"   // AuthContext, CommandSource
#include "System_Debug.h"  // WARN_COMMANDF, ERROR_MEMORYF
#include "BLE_Peers.h"     // gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser

// Provided by System_Utils.cpp — same function the BLE server uses.
extern bool submitCommandAsync(const Command& cmd,
                               ExecAsyncCallback callback,
                               void* userData);

// =============================================================================
// Auth identity for hijack-submitted commands.
//
// Tap commands run "on behalf of" whoever paired the glasses. That identity
// is captured at pairing time and stored in gBlePeerData[].pairedByUser
// (see BLE_Peers.cpp::bleStampPairedByIfBlank). Persisting the username
// rather than synthesizing a fake "g2_hijack" account means glasses
// inherit exactly the privileges of their owner — admin pairs → admin
// taps, regular user pairs → regular taps. No synthetic auth bypass,
// no separate enum, no special case in tgRequireAuth.
//
// If pairedByUser is blank, the tap command will be submitted with an
// empty user string. Downstream tgRequireAuth will reject it and the
// command fails gracefully with an auth error. To recover, re-run
// `bleautoconnect g2-glasses on` (or any pair gesture that triggers
// bleSavePeerMac) — that stamps pairedByUser from gExecAuthContext.user.
// =============================================================================

// =============================================================================
// Generation + sequence counters.
// Single-writer for gCmdSeq (only g2SubmitHijackCommand mutates it).
// Multi-writer for gMenuGen (any navigation site bumps). volatile is
// sufficient for single-core monotonic increments here; if we ever need
// strict atomicity we'll switch to std::atomic, but for staleness checks
// the worst-case "torn read" is benign — a cookie that captured a partial
// value compares unequal to the live value, which just drops the job.
// =============================================================================
static volatile uint32_t gMenuGen = 0;
static volatile uint64_t gCmdSeq  = 0;

void g2BumpMenuGen() {
  gMenuGen++;
  // Intentionally not logged — bump sites fire on every page transition
  // and would dominate the log. Wire to TRACE if a specific bug needs it.
}

uint32_t g2CurrentMenuGen() { return gMenuGen; }
uint64_t g2CurrentCmdSeq()  { return gCmdSeq; }

namespace {

// Bridge between ExecAsyncCallback (raw) and G2HijackCmdCallback (cookie-aware).
// The wrapper owns the cookie copy + user callback identity for the lifetime
// of the in-flight command, and is freed by g2HijackInternalCallback.
struct HijackCallContext {
  G2CmdCookie         cookie;
  G2HijackCmdCallback userCallback;
  void*               userData;
};

void g2HijackInternalCallback(bool ok, const char* result, void* userData) {
  HijackCallContext* ctx = static_cast<HijackCallContext*>(userData);
  if (!ctx) return;

  if (ctx->userCallback) {
    ctx->userCallback(ok, result ? result : "", ctx->cookie, ctx->userData);
  }
  delete ctx;
}

} // namespace

bool g2SubmitHijackCommand(const char* line,
                           const G2CmdCookie& cookie,
                           G2HijackCmdCallback callback,
                           void* userData) {
  if (!line || !*line) {
    WARN_COMMANDF("g2.hijack.cmd: reject empty line");
    return false;
  }

  HijackCallContext* ctx = new (std::nothrow) HijackCallContext{};
  if (!ctx) {
    ERROR_MEMORYF("g2.hijack.cmd: alloc failed line='%s'", line);
    return false;
  }
  // Helper-owned fields: seq/menuGen are stamped here, overwriting whatever
  // the caller passed in. Caller-owned fields (targetPage/targetNetSub) are
  // copied through unchanged.
  ctx->cookie         = cookie;
  ctx->cookie.seq     = ++gCmdSeq;
  ctx->cookie.menuGen = gMenuGen;
  ctx->userCallback   = callback;
  ctx->userData       = userData;

  Command cmd;
  cmd.line = line;
  cmd.ctx.origin       = ORIGIN_G2_HIJACK;
  // SOURCE_LOCAL_DISPLAY: the lens IS a local display (just BLE-attached
  // rather than wired). Matches g2HijackAuthContext() so command-dispatched
  // and direct-FS hijack work resolve to the same identity. Was previously
  // SOURCE_INTERNAL with a "revisit" TODO — SOURCE_INTERNAL only matters
  // for ANON/SYSTEM resolution when user=="system", which pairedByUser
  // never is, so the change is purely cosmetic but removes a misleading
  // transport label from audit lines.
  cmd.ctx.auth.transport = SOURCE_LOCAL_DISPLAY;
  cmd.ctx.auth.path    = "/g2/hijack";
  cmd.ctx.auth.ip      = "g2.local";
  // Identity = the user who paired these glasses. If pairedByUser is
  // blank, downstream tgRequireAuth rejects and the tap fails with an
  // auth error in the log — re-pair to stamp the field.
  cmd.ctx.auth.user    = gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser;
  cmd.ctx.auth.sid     = "";
  cmd.ctx.auth.opaque  = nullptr;
  cmd.ctx.id           = (uint32_t)millis();
  cmd.ctx.timestampMs  = (uint32_t)millis();
  cmd.ctx.outputMask   = CMD_OUT_LOG;
  cmd.ctx.validateOnly = false;
  cmd.ctx.captureOutput = false;
  cmd.ctx.replyHandle  = nullptr;
  cmd.ctx.httpReq      = nullptr;

  if (!submitCommandAsync(cmd, g2HijackInternalCallback, ctx)) {
    WARN_COMMANDF("g2.hijack.cmd: queue full line='%s'", line);
    delete ctx;
    return false;
  }
  return true;
}

// =============================================================================
// In-callback AuthContext helper + RAII guard. See G2_HijackCmd.h for the
// rationale (file-UI hijack handlers run synchronously in BLE callbacks
// without going through cmd_exec, so gExecAuthContext stays empty and
// every guarded VFS call fails ANON).
//
// Identity matches g2SubmitHijackCommand exactly: pairedByUser as user,
// transport = SOURCE_LOCAL_DISPLAY (the lens IS a local display, even
// though it's BLE-attached). Path/ip strings are documentary — they only
// ever surface in [PERM] DENY logs and audit output.
// =============================================================================
extern AuthContext gExecAuthContext;

AuthContext g2HijackAuthContext() {
  AuthContext ctx;
  ctx.transport = SOURCE_LOCAL_DISPLAY;
  ctx.user      = gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser;
  ctx.path      = "/g2/hijack";
  ctx.ip        = "g2.local";
  ctx.sid       = "";
  ctx.opaque    = nullptr;
  return ctx;
}

G2HijackCtxGuard::G2HijackCtxGuard()
    : saved_(gExecAuthContext) {
  gExecAuthContext = g2HijackAuthContext();
}

G2HijackCtxGuard::~G2HijackCtxGuard() {
  gExecAuthContext = saved_;
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
