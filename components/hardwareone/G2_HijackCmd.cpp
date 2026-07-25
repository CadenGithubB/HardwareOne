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
// If pairedByUser is blank, g2SubmitHijackCommand refuses to queue (and
// authorizeCommand would also deny empty non-INTERNAL identities). The
// stuck-stamp warning below still fires once per boot. To recover, re-run
// `bleautoconnect g2-glasses on` from an authenticated CLI — that stamps
// pairedByUser from the caller's currentAuthContext().user.
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

  // Fail closed before queueing — same identity source as cmd.ctx.auth below.
  // Blank pairedByUser is the stuck-stamp state; do not let anonymous taps
  // reach cmd_exec. Callers must treat false as a hard no-op (no inline
  // WiFi/settings mutate on the tap/BLE task — wrong stack + no auth).
  AuthContext hijackAuth = g2HijackAuthContext();
  if (hijackAuth.user.length() == 0) {
    WARN_COMMANDF("g2.hijack.cmd: reject blank pairedByUser line='%s'", line);
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
  // Delegate AuthContext build to g2HijackAuthContext() — the single source
  // of truth for "what a G2-lens tap's identity looks like." Previously
  // this site hand-built its own AuthContext alongside g2HijackAuthContext;
  // the two drifted (one was migrated to SOURCE_G2_GLASSES while the other
  // stayed at SOURCE_LOCAL_DISPLAY, causing async tap commands to log as
  // "alice@display" while sync direct-FS work logged as "alice@g2"). Using
  // the helper for both paths means future identity changes (path tweak,
  // ip rename, new field) land in one place.
  cmd.ctx.auth         = hijackAuth;
  cmd.ctx.id           = (uint32_t)millis();
  cmd.ctx.timestampMs  = (uint32_t)millis();
  cmd.ctx.outputMask   = MSG_ROUTE_FILE;
  cmd.ctx.validateOnly = false;
  cmd.ctx.captureOutput = false;
  cmd.ctx.replyHandle  = nullptr;
  cmd.ctx.httpReq      = nullptr;

  DEBUG_G2F("[G2-Hijack] submit '%.40s' user='%s' transport=%d seq=%u menuGen=%u",
            line, cmd.ctx.auth.user.c_str(),
            (int)cmd.ctx.auth.transport,
            (unsigned)ctx->cookie.seq,
            (unsigned)ctx->cookie.menuGen);
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
// without going through cmd_exec, so the task's TLS slot stays at the
// default ANON identity and every guarded VFS call fails).
//
// Identity matches g2SubmitHijackCommand exactly: pairedByUser as user,
// transport = SOURCE_G2_GLASSES.
//
// Previously this used SOURCE_LOCAL_DISPLAY because "the lens IS a local
// display, even though it's BLE-attached." That framing folded G2 into the
// OLED's transport enum value despite the two having completely different
// identity models — OLED is per-session login (gLocalDisplayUser), G2 is
// pair-time stamp (pairedByUser). The split into a dedicated transport:
//   - lets audit logs distinguish "[CMD] alice@display: ..." from
//     "[CMD] alice@g2: ..." so it's clear which surface ran a command
//   - makes the System_User helpers self-documenting (separate switch
//     branches in isTransportAuthenticated / getTransportUser /
//     applyTransportAuth, each with its own identity-source logic)
//   - removes the need for a "transport X but with user from a different
//     source" override in any future TransportIdentityScope helper
//
// Path/ip strings are documentary — they only ever surface in [PERM] DENY
// logs and audit output. "g2.local" reads cleanly next to "local" (OLED).
// =============================================================================

AuthContext g2HijackAuthContext() {
  AuthContext ctx;
  ctx.transport = SOURCE_G2_GLASSES;
  ctx.user      = gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser;
  ctx.path      = "/g2/hijack";
  ctx.ip        = "g2.local";
  ctx.sid       = "";
  ctx.opaque    = nullptr;

  // Stuck-state detector — should be rare now (boot/load heal + founder
  // fallback). If it still fires, attempt one heal then warn.
  static bool warnedStuck = false;
  if (!warnedStuck
      && ctx.user.length() == 0
      && gBlePeerData[BLE_PEER_G2_GLASSES].mac1.length() > 0) {
    bleStampPairedByIfBlank(BLE_PEER_G2_GLASSES);
    ctx.user = gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser;
    if (ctx.user.length() == 0) {
      warnedStuck = true;
      WARN_BLUETOOTHF("g2-glasses peer STUCK: mac1='%s' but pairedByUser blank (no device owner yet?).",
                      gBlePeerData[BLE_PEER_G2_GLASSES].mac1.c_str());
      WARN_BLUETOOTHF("  Recovery: finish first-time setup / create owner, then `bleautoconnect g2-glasses on`.");
    }
  }

  return ctx;
}

// Sugar over CommandIdentityScope: installs the G2 identity + G2 notification
// context (auto-derived from SOURCE_G2_GLASSES) into the calling task's TLS
// slots for the scope of this object's lifetime, and restores prior values
// on destruction.
//
// The AuthContext temporary returned by g2HijackAuthContext() lives for the
// full-expression of the initializer, which exceeds the lifetime of the
// CommandIdentityScope constructor body — so passing it by const& is safe.
// CommandIdentityScope's inner guards copy what they need from the ctx at
// construction; neither retains a reference back.
G2HijackCtxGuard::G2HijackCtxGuard()
    : scope_(g2HijackAuthContext()) {}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
