// =============================================================================
// G2 hijack command bridge — implementation.
// See G2_HijackCmd.h for the public contract and docs/G2_REFACTOR_PROPOSAL.md
// §5 for architectural context.
// =============================================================================

#include "G2_HijackCmd.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>
#include <atomic>
#include <new>          // std::nothrow
#include "System_CommandTypes.h"
#include "System_User.h"   // AuthContext, CommandSource
#include "System_Debug.h"  // WARN_COMMANDF, ERROR_MEMORYF
#include "System_Utils.h"  // redactCmdForAudit
#include "BLE_Peers.h"     // synchronized G2 paired-owner authority

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
// `bleautoreconnect g2-glasses on` from an authenticated CLI — that stamps
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
  BlePeerOwnerSession owner;
};

void g2HijackInternalCallback(bool ok, const char* result, void* userData) {
  HijackCallContext* ctx = static_cast<HijackCallContext*>(userData);
  if (!ctx) return;

  // The executor admission fence prevents a queued command from running after
  // an owner replacement. Fence the UI callback too: a result produced for a
  // prior owner must not redraw or disclose data to the replacement session.
  const bool ownerStillCurrent = blePeerOwnerSessionIsCurrent(
      BLE_PEER_G2_GLASSES, ctx->owner);
  if (ctx->userCallback && ownerStillCurrent) {
    ctx->userCallback(ok, result ? result : "", ctx->cookie, ctx->userData);
  } else if (!ownerStillCurrent) {
    DEBUG_G2F("[G2-Hijack] drop stale completion seq=%u ownerGen=%lu epoch=%lu",
              (unsigned)ctx->cookie.seq,
              (unsigned long)ctx->owner.generation,
              (unsigned long)ctx->owner.transportEpoch);
  }
  delete ctx;
}

AuthContext g2HijackAuthContextForOwner(BlePeerOwnerSession* ownerOut) {
  AuthContext ctx;
  ctx.transport = SOURCE_G2_GLASSES;
  ctx.path      = "/g2/hijack";
  ctx.ip        = "g2.local";
  ctx.sid       = "";
  ctx.opaque    = nullptr;

  BlePeerOwnerSession owner;
  (void)blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner);
  BlePeerSavedTargetSnapshot target;
  (void)blePeerSavedTargetSnapshot(BLE_PEER_G2_GLASSES, target);

  // Stuck/unowned-state detector. Automatic reconnect must never manufacture
  // authority: only an explicit authenticated pairing/reconnect-enable action
  // may assign the lens owner. Fail closed and explain the recovery gesture.
  static std::atomic<bool> warnedStuck{false};
  if (!owner.live() && target.target.mac1[0]) {
    if (!owner.live() && !warnedStuck.exchange(true)) {
      WARN_BLUETOOTHF("g2-glasses peer STUCK: mac1='%s' but owner authority is unavailable (no device owner/session slot?).",
                      target.target.mac1);
      WARN_BLUETOOTHF("  Recovery: finish first-time setup / create owner, then `bleautoreconnect g2-glasses on`.");
    }
  }

  ctx.user = owner.live() ? owner.user : String();
  if (ownerOut) *ownerOut = owner;
  return ctx;
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
  const String safeLineForTrace = redactCmdForAudit(String(line));

  // Fail closed before queueing — same identity source as cmd.ctx.auth below.
  // Blank pairedByUser is the stuck-stamp state; do not let anonymous taps
  // reach cmd_exec. Callers must treat false as a hard no-op (no inline
  // WiFi/settings mutate on the tap/BLE task — wrong stack + no auth).
  BlePeerOwnerSession owner;
  AuthContext hijackAuth = g2HijackAuthContextForOwner(&owner);
  if (hijackAuth.user.length() == 0 || !owner.live()) {
    WARN_COMMANDF("g2.hijack.cmd: reject blank pairedByUser line='%s'", safeLineForTrace.c_str());
    return false;
  }

  HijackCallContext* ctx = new (std::nothrow) HijackCallContext{};
  if (!ctx) {
    ERROR_MEMORYF("g2.hijack.cmd: alloc failed line='%s'", safeLineForTrace.c_str());
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
  ctx->owner          = owner;

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
  cmd.ctx.transportSessionEpoch = owner.transportEpoch;
  cmd.ctx.behaviorFlags |= COMMAND_CONTEXT_MODE_INDEPENDENT |
                           COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
  cmd.ctx.outputMask   = MSG_ROUTE_FILE;
  cmd.ctx.validateOnly = false;
  cmd.ctx.captureOutput = false;
  cmd.ctx.replyHandle  = nullptr;
  cmd.ctx.httpReq      = nullptr;

  DEBUG_G2F("[G2-Hijack] submit '%.40s' user='%s' transport=%d seq=%u menuGen=%u ownerGen=%lu epoch=%lu",
            safeLineForTrace.c_str(), cmd.ctx.auth.user.c_str(),
            (int)cmd.ctx.auth.transport,
            (unsigned)ctx->cookie.seq,
            (unsigned)ctx->cookie.menuGen,
            (unsigned long)owner.generation,
            (unsigned long)owner.transportEpoch);
  if (!submitCommandAsync(cmd, g2HijackInternalCallback, ctx)) {
    WARN_COMMANDF("g2.hijack.cmd: queue full line='%s'", safeLineForTrace.c_str());
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
  return g2HijackAuthContextForOwner(nullptr);
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
    : owner_{}, scope_(g2HijackAuthContextForOwner(&owner_)) {}

bool G2HijackCtxGuard::stillCurrent() const {
  return blePeerOwnerSessionIsCurrent(BLE_PEER_G2_GLASSES, owner_);
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
