/**
 * User System Implementation
 * 
 * User management, session management, and authentication commands
 * Moved from main .ino to reduce file size and improve organization
 */

#include "System_User.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_BuildConfig.h"  // ENABLE_HTTP_SERVER flag
#if ENABLE_HTTP_SERVER
  #include "WebServer_Server.h"
#endif
#include "System_SensorStubs.h" // Network stubs when disabled
#include "System_Utils.h"  // For CommandEntry
#include "System_Command.h"
#include "System_Clock.h"  // Clock::isValidEpoch / isSynced — replaces tm_year>=120 magic
#include "System_BootState.h"  // bootStateIncrementBootCount — boot counter now lives in NVS
#include "System_Mutex.h"  // For FsLockGuard
#include "System_Debug.h"  // For DEBUG_AUTHF, DEBUG_USERF
#include "System_Logging.h" // For log file paths and constants
#include "System_Filesystem.h"    // For writeText, readText
#include "System_VFS.h"    // For VFS::*Guarded + VFS::systemAuth
#include "System_AuthIdentity.h"   // currentAuthContext (createdBy stamp, admin sync)
#include "System_CLIConfirm.h"     // cliRequestConfirm — yes/no gate for destructive userdelete
#include "System_Settings.h"
#include "System_UartLink.h"
#include "Bluetooth.h"
#include "BLE_Peers.h"            // synchronized G2 paired-owner authority
#include <esp_timer.h>            // AuthPerfScope — instrumentation, see below
#include "System_PollPause.h"   // PollPauseGuard — readRosterJson, mirrors readText

// ============================================================================
// TEMPORARY: auth-latency instrumentation
// ============================================================================
// Added to decide whether memoising the roster lookups is worth touching
// authorization code. Every named-account permission check costs TWO full
// users.json reads (getUserAuthorizationRole parses it, then isSuperAdminUser
// reads and scans it again), and login adds getUserIdByUsername plus a
// 10,000-iteration PBKDF2. Which of those dominates was never measured, and
// the LOOPHEALTH stall counters cannot see it — they attribute the whole cost
// to the loop section that happened to be running.
//
// Enable with `debug auth on`. When the flag is off, t0 stays 0 and neither
// esp_timer_get_time() nor the log call runs, so this is genuinely free.
// REMOVE once the memoisation decision is made.
namespace {
struct AuthPerfScope {
  const char* name;
  int64_t     t0;
  explicit AuthPerfScope(const char* n)
      : name(n), t0(isDebugFlagSet(DEBUG_AUTH) ? esp_timer_get_time() : 0) {}
  ~AuthPerfScope() {
    if (t0 != 0) {
      DEBUG_AUTHF("[AUTHPERF] %-22s %8lld us", name,
                  (long long)(esp_timer_get_time() - t0));
    }
  }
};
}  // namespace
                                  // (declarations are #if ENABLE_BLUETOOTH-gated inside)

// ----------------------------------------------------------------------------
// G2 identity accessors — use the coherent owner+generation+epoch snapshot
// behind the BT compile gate. On BT-off the lens cannot connect, so readers
// return empty/false and the mutator is a no-op.
// ----------------------------------------------------------------------------
static inline String g2PairedUserGet() {
#if ENABLE_BLUETOOTH
  BlePeerOwnerSession owner;
  return blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner) &&
                 owner.live()
             ? owner.user
             : String();
#else
  return String();
#endif
}
static inline void g2PairedUserClear() {
#if ENABLE_BLUETOOTH
  (void)blePeerOwnerSessionClear(BLE_PEER_G2_GLASSES);
#endif
}
static inline bool g2PairedUserClearIfMatches(const String& username) {
#if ENABLE_BLUETOOTH
  BlePeerOwnerSession owner;
  if (!blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner) ||
      !owner.live() || !owner.user.equalsIgnoreCase(username)) {
    return false;
  }
  return blePeerOwnerSessionClearIfCurrent(BLE_PEER_G2_GLASSES, owner);
#else
  (void)username;
  return false;
#endif
}
#include "OLED_Display.h"
#include <atomic>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "System_UserSettings.h"
#include "System_MemUtil.h"
#include <Arduino.h>
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

static String uartSessionUserSnapshot() {
  char user[kPublicUsernameMaxLen + 1] = {};
  if (!uartLinkSessionSnapshot(user, sizeof(user))) return String();
  return String(user);
}

static bool localDisplayBootAuthBypassActive() {
#if ENABLE_OLED_DISPLAY
  return oledBootModeActive;
#else
  return false;
#endif
}

// ============================================================================
// External Dependencies from .ino
// ============================================================================

// filesystemReady is provided by System_Filesystem.h (included above).

// Session management now in web_server.h (included above)

// ----------------------------------------------------------------------------
// Shared session idle-timeout policy (Step 0). See System_User.h for the
// contract. Pure read of gSettings + millis(); safe to call from any task.
// ----------------------------------------------------------------------------
unsigned long sessionStampNow() {
  unsigned long t = millis();
  return (t == 0) ? 1UL : t;                 // reserve 0 to mean "never stamped"
}

unsigned long sessionIdleWindowMs(CommandSource transport) {
  uint32_t mins = 0;                                    // unknown/unwired → disabled
  switch (transport) {
    case SOURCE_WEB:       mins = gSettings.sessionIdleWeb;    break;
    case SOURCE_SERIAL:    mins = gSettings.sessionIdleSerial; break;
    case SOURCE_BLUETOOTH: mins = gSettings.sessionIdleBle;     break;
    case SOURCE_LOCAL_DISPLAY: mins = gSettings.sessionIdleDisplay; break;
    case SOURCE_UART:      mins = gSettings.sessionIdleUart;   break;  // default 0: a machine session doesn't wander off; client re-logins on rejection
    // SOURCE_ESPNOW (bond) is intentionally excluded: it's a device-to-device
    // trust channel, not a human session, so idle-logout doesn't apply. Any
    // other transport reads as disabled.
    default:               mins = 0;                            break;
  }
  return (mins == 0) ? 0UL : (unsigned long)mins * 60000UL;   // 0 = disabled
}

bool sessionIdleExpired(unsigned long lastInteractionMs, unsigned long nowMs, unsigned long windowMs) {
  if (windowMs == 0)          return false;  // feature off → never expire
  if (lastInteractionMs == 0) return false;  // never stamped → treat as fresh
  // Wrap-safe: the window (<= 24h) is far below the ~49.7-day millis() rollover,
  // and an idle session would have expired long before wrap could matter.
  return (unsigned long)(nowMs - lastInteractionMs) > windowMs;
}

bool sessionIdleExpired(CommandSource transport, unsigned long lastInteractionMs, unsigned long nowMs) {
  return sessionIdleExpired(lastInteractionMs, nowMs, sessionIdleWindowMs(transport));
}

// File paths
#define PENDING_USERS_FILE "/system/users/pending_users.json"

// External utility dependencies
extern String getDeviceEncryptionKey();
extern void fsLock(const char* reason);
extern void fsUnlock();
extern void resolvePendingUserCreationTimes();

// Boot tracking globals
extern uint32_t gNTPAnchorId;
extern uint32_t gBootCounter;

// Serial authentication globals
extern bool gSerialAuthed;
extern String gSerialUser;
extern std::atomic<unsigned long> gSerialLastInteractionMs;

// ============================================================================
// Boot-local transport session generations
// ============================================================================
//
// These are incarnation counters, not timestamps. They intentionally ignore
// RTC/NTP/wall-clock state, so a device may gain or lose time synchronization
// without changing command authority or an interactive prompt's owner.
namespace {
constexpr size_t kTransportSessionSlots = 12;  // web(2)+BLE(4)+serial+OLED+G2+headroom
struct TransportSessionSlot {
  CommandSource source = SOURCE_INTERNAL;
  TransportSessionEpoch epoch = kNoTransportSessionEpoch;
};
TransportSessionSlot sTransportSessions[kTransportSessionSlots];
TransportSessionEpoch sTransportSessionGeneration = 0;
portMUX_TYPE sTransportSessionMux = portMUX_INITIALIZER_UNLOCKED;

struct LegacySessionShadow {
  TransportSessionEpoch epoch = kNoTransportSessionEpoch;
  bool authed = false;
  char user[kPublicUsernameMaxLen + 1] = {};
};
LegacySessionShadow sSerialSession;
LegacySessionShadow sDisplaySession;
portMUX_TYPE sLegacySessionShadowMux = portMUX_INITIALIZER_UNLOCKED;

// Lifecycle writers, one per legacy transport. FreeRTOS mutexes rather than
// spin flags: a higher-priority waiter (the httpd task revoking a session)
// lends its priority to the holder instead of starving it on the same core.
// Static storage created at load, like the other owner mutexes in this
// component; nothing calls into the session API before that.
// Rules for every holder: take a writer only around the owner-side check or
// publication itself, never across a console write, display push, log append
// or users.json access; never take the second writer while holding the first;
// release on the task that took it, as a mutex requires.
StaticSemaphore_t sSerialSessionWriterStorage;
StaticSemaphore_t sDisplaySessionWriterStorage;
SemaphoreHandle_t sSerialSessionWriter =
    xSemaphoreCreateMutexStatic(&sSerialSessionWriterStorage);
SemaphoreHandle_t sDisplaySessionWriter =
    xSemaphoreCreateMutexStatic(&sDisplaySessionWriterStorage);

void sessionWriterTake(SemaphoreHandle_t writer) {
  (void)xSemaphoreTake(writer, portMAX_DELAY);
}

void sessionWriterGive(SemaphoreHandle_t writer) {
  (void)xSemaphoreGive(writer);
}

class SessionWriterGuard {
 public:
  explicit SessionWriterGuard(SemaphoreHandle_t writer) : writer_(writer) {
    sessionWriterTake(writer_);
  }
  ~SessionWriterGuard() { sessionWriterGive(writer_); }
 private:
  SemaphoreHandle_t writer_;
};

TransportSessionEpoch nextTransportSessionEpochLocked() {
  for (;;) {
    do {
      ++sTransportSessionGeneration;
    } while (sTransportSessionGeneration == kNoTransportSessionEpoch);
    bool inUse = false;
    for (const TransportSessionSlot& slot : sTransportSessions) {
      if (slot.epoch == sTransportSessionGeneration) {
        inUse = true;
        break;
      }
    }
    if (!inUse) return sTransportSessionGeneration;
  }
}

void clearLegacyShadow(LegacySessionShadow& shadow,
                       TransportSessionEpoch& oldEpoch) {
  portENTER_CRITICAL(&sLegacySessionShadowMux);
  oldEpoch = shadow.epoch;
  shadow.epoch = kNoTransportSessionEpoch;
  shadow.authed = false;
  shadow.user[0] = '\0';
  portEXIT_CRITICAL(&sLegacySessionShadowMux);
}

void publishLegacyShadow(LegacySessionShadow& shadow,
                         TransportSessionEpoch epoch,
                         const String& user,
                         bool authed = true) {
  portENTER_CRITICAL(&sLegacySessionShadowMux);
  shadow.authed = authed;
  strlcpy(shadow.user, user.c_str(), sizeof(shadow.user));
  shadow.epoch = epoch;
  portEXIT_CRITICAL(&sLegacySessionShadowMux);
}

TransportSessionEpoch snapshotLegacyShadow(const LegacySessionShadow& shadow,
                                            String& userOut,
                                            bool& authedOut) {
  char user[kPublicUsernameMaxLen + 1] = {};
  TransportSessionEpoch epoch = 0;
  bool authed = false;
  portENTER_CRITICAL(&sLegacySessionShadowMux);
  epoch = shadow.epoch;
  authed = shadow.authed;
  strlcpy(user, shadow.user, sizeof(user));
  portEXIT_CRITICAL(&sLegacySessionShadowMux);
  userOut = user;
  authedOut = authed && epoch != 0;
  return epoch;
}

// Serial and local display share lazy anonymous-session creation. Keep the
// policy by reference: another writer may change it while we wait for the
// lifecycle lock, so both the session and policy must be read again afterward.
TransportSessionEpoch snapshotLegacySessionWithAuthPolicy(
    LegacySessionShadow& shadow, SemaphoreHandle_t writer,
    CommandSource source, const bool& requireAuth,
    String& userOut, bool& authedOut) {
  TransportSessionEpoch epoch = snapshotLegacyShadow(shadow, userOut, authedOut);
  if (epoch != 0 || requireAuth) return epoch;
  SessionWriterGuard guard(writer);
  epoch = snapshotLegacyShadow(shadow, userOut, authedOut);
  if (epoch == 0 && !requireAuth) {
    epoch = transportSessionOpen(source);
    if (epoch != 0) {
      publishLegacyShadow(shadow, epoch, String(), false);
      userOut = String();
      authedOut = false;
    }
  }
  return epoch;
}
}  // namespace

TransportSessionEpoch transportSessionOpen(CommandSource source) {
  TransportSessionEpoch epoch = 0;
  portENTER_CRITICAL(&sTransportSessionMux);
  for (size_t i = 0; i < kTransportSessionSlots; ++i) {
    if (sTransportSessions[i].epoch != 0) continue;
    epoch = nextTransportSessionEpochLocked();
    sTransportSessions[i].source = source;
    sTransportSessions[i].epoch = epoch;
    break;
  }
  portEXIT_CRITICAL(&sTransportSessionMux);
  return epoch;
}

void transportSessionClose(CommandSource source, TransportSessionEpoch epoch) {
  if (epoch == 0) return;
  portENTER_CRITICAL(&sTransportSessionMux);
  for (size_t i = 0; i < kTransportSessionSlots; ++i) {
    if (sTransportSessions[i].epoch != epoch ||
        sTransportSessions[i].source != source) continue;
    sTransportSessions[i].epoch = 0;
    sTransportSessions[i].source = SOURCE_INTERNAL;
    break;
  }
  portEXIT_CRITICAL(&sTransportSessionMux);
}

bool transportSessionEpochIsLive(CommandSource source,
                                 TransportSessionEpoch epoch) {
  if (epoch == 0) return false;
  if (source == SOURCE_UART) {
    return uartLinkTransportSessionEpoch() == epoch;
  }
  if (source == SOURCE_WEB) {
#if ENABLE_HTTP_SERVER
    // The fixed sidecar is cleared before the String-heavy SessionEntry is
    // mutated. It is a prerequisite, while the central registry remains the
    // second half of the publication contract. Requiring both makes the
    // open-then-publish and unpublish-then-close windows fail closed.
    if (!webSessionEpochIsLive(epoch)) return false;
#else
    return false;
#endif
  }
  if (source == SOURCE_BLUETOOTH) {
    if (!bleSessionEpochIsLive(epoch)) return false;
  }
  if (source == SOURCE_SERIAL || source == SOURCE_LOCAL_DISPLAY) {
    const LegacySessionShadow& shadow =
        source == SOURCE_SERIAL ? sSerialSession : sDisplaySession;
    bool policyLive = false;
    portENTER_CRITICAL(&sLegacySessionShadowMux);
    policyLive = shadow.epoch == epoch &&
                 (shadow.authed ||
                  (source == SOURCE_SERIAL
                       ? !gSettings.serialRequireAuth
                       : !gSettings.localDisplayRequireAuth));
    portEXIT_CRITICAL(&sLegacySessionShadowMux);
    if (!policyLive) return false;
  }
  bool live = false;
  portENTER_CRITICAL(&sTransportSessionMux);
  for (size_t i = 0; i < kTransportSessionSlots; ++i) {
    if (sTransportSessions[i].epoch == epoch &&
        sTransportSessions[i].source == source) {
      live = true;
      break;
    }
  }
  portEXIT_CRITICAL(&sTransportSessionMux);
  return live;
}

void serialTransportSessionCleared() {
  SessionWriterGuard guard(sSerialSessionWriter);
  TransportSessionEpoch oldEpoch = 0;
  clearLegacyShadow(sSerialSession, oldEpoch);
  transportSessionClose(SOURCE_SERIAL, oldEpoch);
  gSerialAuthed = false;
  gSerialUser = String();
  gSerialLastInteractionMs.store(0, std::memory_order_release);
}

void serialTransportAuthPolicyChanged() {
  SessionWriterGuard guard(sSerialSessionWriter);
  TransportSessionEpoch oldEpoch = 0;
  portENTER_CRITICAL(&sLegacySessionShadowMux);
  if (!sSerialSession.authed) {
    oldEpoch = sSerialSession.epoch;
    sSerialSession.epoch = kNoTransportSessionEpoch;
    sSerialSession.user[0] = '\0';
  }
  portEXIT_CRITICAL(&sLegacySessionShadowMux);
  transportSessionClose(SOURCE_SERIAL, oldEpoch);
}

bool serialTransportSessionBeginDelivery(TransportSessionEpoch epoch) {
  if (epoch == 0) return false;
  sessionWriterTake(sSerialSessionWriter);
  String liveUser;
  bool liveAuthed = false;
  if (snapshotLegacyShadow(sSerialSession, liveUser, liveAuthed) != epoch) {
    sessionWriterGive(sSerialSessionWriter);
    return false;
  }
  return true;
}

void serialTransportSessionEndDelivery() {
  sessionWriterGive(sSerialSessionWriter);
}

TransportSessionEpoch serialTransportInputEpoch() {
  String user;
  bool authed = false;
  TransportSessionEpoch epoch =
      snapshotLegacyShadow(sSerialSession, user, authed);
  if (epoch != 0) return epoch;
  SessionWriterGuard guard(sSerialSessionWriter);
  epoch = snapshotLegacyShadow(sSerialSession, user, authed);
  if (epoch == 0) {
    epoch = transportSessionOpen(SOURCE_SERIAL);
    if (epoch != 0) publishLegacyShadow(sSerialSession, epoch, String(), false);
  }
  return epoch;
}

bool serialTransportInputEpochIsCurrent(TransportSessionEpoch epoch) {
  if (epoch == 0) return false;
  String user;
  bool authed = false;
  return snapshotLegacyShadow(sSerialSession, user, authed) == epoch;
}

bool localDisplayTransportSessionBeginDelivery(TransportSessionEpoch epoch) {
  if (epoch == 0) return false;
  sessionWriterTake(sDisplaySessionWriter);
  if (!transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, epoch)) {
    sessionWriterGive(sDisplaySessionWriter);
    return false;
  }
  return true;
}

void localDisplayTransportSessionEndDelivery() {
  sessionWriterGive(sDisplaySessionWriter);
}

TransportSessionEpoch localDisplayTransportSessionBeginUiSync(
    String& userOut, bool& authedOut) {
  sessionWriterTake(sDisplaySessionWriter);

  // Unlike BeginDelivery(), logout is a real UI boundary too. Keep the writer
  // held and return epoch zero instead of failing so the OLED owner can erase
  // the former identity and all of its cached UI state.
  TransportSessionEpoch epoch =
      snapshotLegacyShadow(sDisplaySession, userOut, authedOut);
  if (epoch == 0 && !gSettings.localDisplayRequireAuth) {
    epoch = transportSessionOpen(SOURCE_LOCAL_DISPLAY);
    if (epoch != 0) {
      publishLegacyShadow(sDisplaySession, epoch, String(), false);
      userOut = String();
      authedOut = false;
    }
  }
  return epoch;
}

void localDisplayTransportSessionEndUiSync() {
  sessionWriterGive(sDisplaySessionWriter);
}

static TransportSessionEpoch serialTransportSessionAuthenticatedLocked(
    const String& user) {
  TransportSessionEpoch oldEpoch = 0;
  clearLegacyShadow(sSerialSession, oldEpoch);
  transportSessionClose(SOURCE_SERIAL, oldEpoch);
  gSerialAuthed = true;
  gSerialUser = user.length() ? user : String("serial");
  TransportSessionEpoch epoch = transportSessionOpen(SOURCE_SERIAL);
  if (epoch == 0) {
    gSerialAuthed = false;
    gSerialUser = String();
    gSerialLastInteractionMs.store(0, std::memory_order_release);
    return 0;
  }
  publishLegacyShadow(sSerialSession, epoch, gSerialUser);
  gSerialLastInteractionMs.store(sessionStampNow(),
                                  std::memory_order_release);
  return epoch;
}

TransportSessionEpoch serialTransportSessionAuthenticated(const String& user) {
  SessionWriterGuard guard(sSerialSessionWriter);
  return serialTransportSessionAuthenticatedLocked(user);
}

TransportSessionEpoch serialTransportSessionAuthenticatedIfEpoch(
    TransportSessionEpoch expectedEpoch, const String& user) {
  SessionWriterGuard guard(sSerialSessionWriter);
  String currentUser;
  bool currentAuthed = false;
  const TransportSessionEpoch currentEpoch =
      snapshotLegacyShadow(sSerialSession, currentUser, currentAuthed);
  if (currentEpoch != expectedEpoch) return 0;
  return serialTransportSessionAuthenticatedLocked(user);
}

bool serialTransportSessionClearIfEpoch(TransportSessionEpoch expectedEpoch) {
  SessionWriterGuard guard(sSerialSessionWriter);
  String currentUser;
  bool currentAuthed = false;
  const TransportSessionEpoch currentEpoch =
      snapshotLegacyShadow(sSerialSession, currentUser, currentAuthed);
  if (currentEpoch != expectedEpoch) return false;
  TransportSessionEpoch oldEpoch = 0;
  clearLegacyShadow(sSerialSession, oldEpoch);
  transportSessionClose(SOURCE_SERIAL, oldEpoch);
  gSerialAuthed = false;
  gSerialUser = String();
  gSerialLastInteractionMs.store(0, std::memory_order_release);
  // The writer is released here; the caller prints its fixed ACK afterwards.
  // A successor login publishes through this same writer and never writes to
  // the console itself, so the ACK cannot land after a replacement's output.
  return true;
}

TransportSessionEpoch serialTransportSessionSnapshot(String& userOut,
                                                      bool& authedOut) {
  return snapshotLegacySessionWithAuthPolicy(
      sSerialSession, sSerialSessionWriter, SOURCE_SERIAL,
      gSettings.serialRequireAuth, userOut, authedOut);
}

void localDisplayTransportSessionCleared() {
  {
    SessionWriterGuard guard(sDisplaySessionWriter);
    TransportSessionEpoch oldEpoch = 0;
    clearLegacyShadow(sDisplaySession, oldEpoch);
    transportSessionClose(SOURCE_LOCAL_DISPLAY, oldEpoch);
    gLocalDisplayLastInteractionMs.store(0, std::memory_order_release);
    // Publish the atomic UI generation before releasing the lifecycle writer.
    // The OLED commit fence takes this same writer, so it cannot observe new
    // authority without also observing a pending owner-side boundary.
    oledNotifyLocalDisplayAuthChanged();
  }
}

void localDisplayTransportAuthPolicyChanged() {
  {
    SessionWriterGuard guard(sDisplaySessionWriter);
    TransportSessionEpoch oldEpoch = 0;
    portENTER_CRITICAL(&sLegacySessionShadowMux);
    if (!sDisplaySession.authed) {
      oldEpoch = sDisplaySession.epoch;
      sDisplaySession.epoch = kNoTransportSessionEpoch;
      sDisplaySession.user[0] = '\0';
    }
    portEXIT_CRITICAL(&sLegacySessionShadowMux);
    transportSessionClose(SOURCE_LOCAL_DISPLAY, oldEpoch);
    if (oldEpoch != 0) {
      gLocalDisplayLastInteractionMs.store(0, std::memory_order_release);
    }
    oledNotifyLocalDisplayAuthChanged();
  }
}

void localDisplayTransportSessionAuthenticated(const String& user) {
  {
    SessionWriterGuard guard(sDisplaySessionWriter);
    TransportSessionEpoch oldEpoch = 0;
    clearLegacyShadow(sDisplaySession, oldEpoch);
    transportSessionClose(SOURCE_LOCAL_DISPLAY, oldEpoch);
    const String canonicalUser = user.length() ? user : String("display");
    TransportSessionEpoch epoch = transportSessionOpen(SOURCE_LOCAL_DISPLAY);
    if (epoch == 0) {
      gLocalDisplayLastInteractionMs.store(0, std::memory_order_release);
    } else {
      publishLegacyShadow(sDisplaySession, epoch, canonicalUser);
      gLocalDisplayLastInteractionMs.store(sessionStampNow(),
                                            std::memory_order_release);
    }
    oledNotifyLocalDisplayAuthChanged();
  }
}

TransportSessionEpoch localDisplayTransportSessionSnapshot(String& userOut,
                                                            bool& authedOut) {
  return snapshotLegacySessionWithAuthPolicy(
      sDisplaySession, sDisplaySessionWriter, SOURCE_LOCAL_DISPLAY,
      gSettings.localDisplayRequireAuth, userOut, authedOut);
}

TransportSessionEpoch captureTransportSessionEpoch(const AuthContext& ctx) {
  switch (ctx.transport) {
    case SOURCE_WEB:
#if ENABLE_HTTP_SERVER
      return ctx.sid.length() ? webSessionEpochForSID(ctx.sid) : 0;
#else
      return 0;
#endif
    case SOURCE_SERIAL: {
      String user;
      bool authed = false;
      TransportSessionEpoch epoch = serialTransportSessionSnapshot(user, authed);
      if (authed) return user.equalsIgnoreCase(ctx.user) ? epoch : 0;
      return !gSettings.serialRequireAuth && ctx.user == "AuthBypass"
                 ? epoch : 0;
    }
    case SOURCE_LOCAL_DISPLAY: {
      String user;
      bool authed = false;
      TransportSessionEpoch epoch =
          localDisplayTransportSessionSnapshot(user, authed);
      if (authed) return user.equalsIgnoreCase(ctx.user) ? epoch : 0;
      return !gSettings.localDisplayRequireAuth && ctx.user == "AuthBypass"
                 ? epoch : 0;
    }
    case SOURCE_BLUETOOTH: {
      if (ctx.sid.length() == 0) return 0;
      char* end = nullptr;
      unsigned long conn = strtoul(ctx.sid.c_str(), &end, 10);
      if (!end || *end != '\0' || conn > UINT16_MAX) return 0;
      return bleSessionEpochForConnection(static_cast<uint16_t>(conn));
    }
    case SOURCE_UART:
      return uartLinkTransportSessionEpoch();
    case SOURCE_G2_GLASSES: {
#if ENABLE_BLUETOOTH
      BlePeerOwnerSession owner;
      if (!blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner) ||
          !owner.live() || owner.user != ctx.user) {
        return kNoTransportSessionEpoch;
      }
      return owner.transportEpoch;
#else
      return kNoTransportSessionEpoch;
#endif
    }
    default:
      return 0;
  }
}

// Web authentication functions (from web_server.h)
extern bool isAuthed(httpd_req_t* req, String& outUser);
extern esp_err_t sendAuthRequiredResponse(httpd_req_t* req);
extern void getClientIP(httpd_req_t* req, String& ip);
extern bool webPasswordChangeAllows(httpd_req_t* req, const String& user);

// Debug macros now defined in debug_system.h with performance optimizations
// DEBUG_USERS, DEBUG_USERSF, DEBUG_CMD_FLOW, DEBUG_CMD_FLOWF, DEBUG_SYSTEM, DEBUG_SYSTEMF

// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations

// RETURN_VALID_IF_VALIDATE_CSTR is defined centrally in system_utils.h;
// use that shared definition to keep behavior consistent across modules.

// ============================================================================
// Helper Functions
// ============================================================================

// Logout reason tracking moved to web_server.h
// Session helpers now in web_server.h (included above)

// ============================================================================
// Transport-Generic Authentication Functions
// ============================================================================

#if ENABLE_HTTP_SERVER
// Require auth across transports. Returns true if authenticated; otherwise emits the
// appropriate denial (401/console note) and returns false.
bool tgRequireAuth(AuthContext& ctx) {
  if (ctx.transport == SOURCE_WEB) {
    httpd_req_t* req = reinterpret_cast<httpd_req_t*>(ctx.opaque);
    if (!req) return false;
    // Prefer cached auth for high-frequency endpoints
    String userTmp;
    bool ok = isAuthed(req, userTmp);
    if (!ok) {
      sendAuthRequiredResponse(req);
      return false;
    }
    ctx.user = userTmp;
    // Keep the bearer locator separate from the numeric command-generation
    // fence. Basic Auth has no cookie and therefore remains intentionally
    // stateless (empty sid / epoch zero).
    ctx.sid = getCookieSID(req);
    if (ctx.ip.length() == 0) { getClientIP(req, ctx.ip); }
    logAuthAttempt(true, ctx.path.c_str(), ctx.user, ctx.ip, "");
    // A forced password change admits only the change form and logout, for
    // pages and APIs alike (userMustChangePassword is the one decision).
    if (!webPasswordChangeAllows(req, ctx.user)) return false;
    return true;
  } else if (ctx.transport == SOURCE_SERIAL) {
    // Serial console auth state. Use the synchronized fixed-storage shadow;
    // targeted session administration can replace the legacy Arduino String
    // from cmd_exec while another task is resolving a request.
    String serialUser;
    bool serialAuthed = false;
    (void)serialTransportSessionSnapshot(serialUser, serialAuthed);
    if (gSettings.serialRequireAuth && !serialAuthed) {
      broadcastOutput("ERROR: auth required");
      return false;
    }
    ctx.user = serialAuthed ? serialUser : String();
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_UART) {
    // UART host-link auth state — mirrors the serial branch against the
    // link's own session pair. Must NOT fall into the trusting else-branch
    // below (that branch assumes upstream validation the UART drain doesn't
    // claim to do).
    const String uartUser = uartSessionUserSnapshot();
    if (gSettings.uartRequireAuth && uartUser.length() == 0) {
      broadcastOutput("ERROR: auth required (uart)");
      return false;
    }
    ctx.user = uartUser;
    if (ctx.ip.length() == 0) ctx.ip = "uart";
    return true;
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    // Resolve from the synchronized authority, not the OLED-loop's legacy UI
    // mirror. A remote login/logout updates this shadow synchronously and only
    // then asks the main loop to redraw.
    String displayUser;
    bool displayAuthed = false;
    (void)localDisplayTransportSessionSnapshot(displayUser, displayAuthed);
    // Allow commands during boot phase (before auth is enforced).
    if (gSettings.localDisplayRequireAuth && !displayAuthed &&
        !localDisplayBootAuthBypassActive()) {
      broadcastOutput("ERROR: auth required (display)");
      return false;
    }
    ctx.user = displayAuthed ? displayUser : String();
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_G2_GLASSES) {
    // G2 lens — auth IS pairing. pairedByUser is the captured identity from
    // pair-time (set by bleStampPairedByIfBlank when an authenticated CLI
    // ran `bleautoreconnect g2-glasses on`). Blank means "paired-but-stamp-lost"
    // — refuse the command and tell the user how to recover (re-stamp via
    // bleautoreconnect from an authenticated CLI session).
    String g2User = g2PairedUserGet();
    if (g2User.length() == 0) {
      broadcastOutput("ERROR: G2 pairedByUser blank — run 'bleautoreconnect g2-glasses on' from authenticated CLI to re-stamp");
      return false;
    }
    ctx.user = g2User;
    if (ctx.ip.length() == 0) ctx.ip = "g2.local";
    return true;
  } else {
    // SOURCE_BLUETOOTH and SOURCE_ESPNOW are expected to validate auth in
    // their command-receive handlers BEFORE submitting the command to
    // executeCommand:
    //   * BLE  — per-conn session token check in Bluetooth.cpp before submit
    //   * ESP-NOW — v3 protocol signed msgId verification before submit
    //   * MQTT — handleMQTTCommand checks the credential attempt table and
    //     then the user/pass carried in every command envelope before submit
    // SOURCE_INTERNAL is firmware-generated and trusted by design (used by
    // automation rules, boot sequences, scheduled tasks).
    return true;
  }
}

#else
// Stub implementations when HTTP server is disabled
bool tgRequireAuth(AuthContext& ctx) {
  // Serial auth only when HTTP server disabled
  if (ctx.transport == SOURCE_SERIAL) {
    String serialUser;
    bool serialAuthed = false;
    (void)serialTransportSessionSnapshot(serialUser, serialAuthed);
    if (gSettings.serialRequireAuth && !serialAuthed) {
      broadcastOutput("ERROR: auth required");
      return false;
    }
    ctx.user = serialAuthed ? serialUser : String();
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_UART) {
    // UART host link. This stub's fall-through returns true, so a missing
    // case here would be an auth BYPASS, not a dead feature — keep this
    // branch in lockstep with the full-build one above.
    const String uartUser = uartSessionUserSnapshot();
    if (gSettings.uartRequireAuth && uartUser.length() == 0) {
      broadcastOutput("ERROR: auth required (uart)");
      return false;
    }
    ctx.user = uartUser;
    if (ctx.ip.length() == 0) ctx.ip = "uart";
    return true;
  } else if (ctx.transport == SOURCE_LOCAL_DISPLAY) {
    String displayUser;
    bool displayAuthed = false;
    (void)localDisplayTransportSessionSnapshot(displayUser, displayAuthed);
    if (gSettings.localDisplayRequireAuth && !displayAuthed &&
        !localDisplayBootAuthBypassActive()) {
      broadcastOutput("ERROR: auth required (display)");
      return false;
    }
    ctx.user = displayAuthed ? displayUser : String();
    if (ctx.ip.length() == 0) ctx.ip = "local";
    return true;
  } else if (ctx.transport == SOURCE_G2_GLASSES) {
    String g2User = g2PairedUserGet();
    if (g2User.length() == 0) {
      broadcastOutput("ERROR: G2 pairedByUser blank — run 'bleautoreconnect g2-glasses on' from authenticated CLI to re-stamp");
      return false;
    }
    ctx.user = g2User;
    if (ctx.ip.length() == 0) ctx.ip = "g2.local";
    return true;
  }
  return true; // Internal commands pass through
}
#endif // ENABLE_HTTP_SERVER

String getDeviceOwnerUsername() {
  if (!filesystemReady) return String();
  FsLockGuard _g("user.deviceOwner");
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.deviceOwner"))) {
    return String();
  }
  String json;
  if (!readText(USERS_JSON_FILE, json)) return String();
  int usersIdx = json.indexOf("\"users\"");
  if (usersIdx < 0) return String();
  int firstUKey = json.indexOf("\"username\"", usersIdx);
  if (firstUKey < 0) return String();
  int uq1 = json.indexOf('"', json.indexOf(':', firstUKey) + 1);
  int uq2 = json.indexOf('"', uq1 + 1);
  if (uq1 <= 0 || uq2 <= uq1) return String();
  return json.substring(uq1 + 1, uq2);
}

// Determine if the given username is admin (any user with role == admin)
// Roster helpers — defined below, next to the other two parsers. Declared here
// because isAdminUser sits above them in this file.
static bool readRosterJson(const char* tag, String& out);
static bool adminFromRosterJson(const String& json, const String& who);
static bool superFromRosterJson(const String& json, const String& who);
static bool roleFromRosterJson(const String& json, const String& username, String& roleOut);
static bool legacyRoleFromRosterJson(const String& json, const String& username, String& outRole);

bool isAdminUser(const String& who) {
  // An empty identity is nobody — never admin. Checked before anything else so the
  // invariant holds regardless of how the roster parses below: a users.json with no
  // "username" key leaves firstUser empty, and "" == "" would otherwise satisfy the
  // first-user fallback. resolveRole() and isGuestUser() already guard the same way.
  if (who.length() == 0) return false;
  // A bonded master that presented a valid bond session token runs commands under
  // kBondAdminUser. The bare name grants admin only while the calling task runs
  // that live ESP-NOW bond identity: an account or a session on another
  // transport carrying the same name is never elevated (and never the roster's
  // answer either). Checked before the filesystem/JSON path.
  if (who == kBondAdminUser) {
    return bondIdentityLive(currentAuthContext());
  }
  if (!filesystemReady) return false;
  // Serialize with role writes (reentrant per task) so this hot-path privilege
  // check can't tear-read a users.json mid-write.
  FsLockGuard _g("user.isAdmin");
  // Prefer JSON. One guarded open instead of existsGuarded-then-readText:
  // that pair resolved this 3-level path twice (~15 ms of the measured 36.5 ms
  // per read) AND finished with an unguarded readText, so the exists probe was
  // carrying the only permission check on the credential database.
  String json;
  if (!readRosterJson("user.isAdmin", json)) return false;
  return adminFromRosterJson(json, who);
}

// Verbatim body of the old isAdminUser, from the point the file contents were
// in hand — a THIRD parser over the same bytes, kept separate for the same
// reason as the other two. Its first-user fallback is STRICTER than
// superFromRosterJson's: it fires only when the first account has no role field
// at all (firstUserHasRole), whereas the super fallback ignores the first
// user's own role entirely. Those two rules genuinely disagree; do not merge
// them, only the read is shared.
static bool adminFromRosterJson(const String& json, const String& who) {
  AuthPerfScope _perf("roster.scanAdmin");
  {
    int usersIdx = json.indexOf("\"users\"");
    if (usersIdx < 0) return false;
    int firstUKey = json.indexOf("\"username\"", usersIdx);
    // No account in the roster at all — nobody is admin. Bail rather than fall
    // through with an empty firstUser, matching getDeviceOwnerUsername()'s
    // discipline directly above.
    if (firstUKey < 0) return false;
    String firstUser = "";
    {
      int uq1 = json.indexOf('"', json.indexOf(':', firstUKey) + 1);
      int uq2 = json.indexOf('"', uq1 + 1);
      if (uq1 > 0 && uq2 > uq1) firstUser = json.substring(uq1 + 1, uq2);
    }
    if (firstUser.length() == 0) return false;  // unparseable owner — fail closed
    // Search for target user and role. An explicit role always wins — never
    // elevate via the first-user fallback when role is user/guest/etc.
    bool firstUserHasRole = false;
    int pos = usersIdx;
    while (true) {
      int uKey = json.indexOf("\"username\"", pos);
      if (uKey < 0) break;
      int uq1 = json.indexOf('"', json.indexOf(':', uKey) + 1);
      int uq2 = json.indexOf('"', uq1 + 1);
      if (uq1 < 0 || uq2 <= uq1) break;
      String uname = json.substring(uq1 + 1, uq2);
      int rKey = json.indexOf("\"role\"", uKey);
      int nextU = json.indexOf("\"username\"", uKey + 1);
      if (rKey > 0 && (nextU < 0 || rKey < nextU)) {
        if (uname == firstUser) firstUserHasRole = true;
        int rq1 = json.indexOf('"', json.indexOf(':', rKey) + 1);
        int rq2 = json.indexOf('"', rq1 + 1);
        String role = (rq1 > 0 && rq2 > rq1) ? json.substring(rq1 + 1, rq2) : String("");
        if (uname == who) {
          // superadmin is a strict superset of admin — it satisfies every
          // admin-gated command too.
          return (role == "admin" || role == "superadmin");
        }
      }
      pos = uq2 + 1;
    }
    // Fallback: first user with no role field is admin (pre-role backups).
    // Matches isSuperAdminUser's "only elevate when the roster lacks the
    // explicit tier" spirit — never override an explicit non-admin role.
    return (who == firstUser && !firstUserHasRole);
  }
  return false;
}

// Determine if the given username is a super-admin (role == "superadmin").
// This is the top tier: it gates identity/crypto/destructive/auth-posture
// commands that an ordinary admin must not run (see commandRequiresSuperAdmin).
// The bonded ESP-NOW master (kBondAdminUser) is treated as super for the life
// of a valid bond session — the bond is the token-authenticated 1:1 trust
// channel and is intentionally the *only* over-the-air path to super (a
// regular mesh/pair account gets only its stored role, never elevated here).
// ============================================================================
// Roster access: ONE read of users.json, two independent interpretations
// ============================================================================
// resolveRole() asks two questions about the same file back to back — "what is
// this account's role?" and "is this account superadmin?" — and each used to
// open and read /system/users/users.json separately. Measured on hardware at
// ~36.5 ms per read, so every permission check paid ~73 ms, once per directory
// entry in a listing.
//
// The READ is now shared. The two PARSERS are deliberately NOT unified: they
// disagree in ways that decide authorization.
//   - roleFromRosterJson uses ArduinoJson: key-order independent, lowercases
//     the role before matching, and honours "banned".
//   - superFromRosterJson is a raw-text scan: case-SENSITIVE on "superadmin",
//     requires "role" to appear before the next "username" in the byte stream,
//     and ignores "banned" entirely.
// A roster with role "SuperAdmin" is superadmin to the first and not to the
// second. Unifying the parsing would silently change who is authorized, so
// only the file read is shared.

// One guarded open, one read. Replaces the old existsGuarded-then-open and
// existsGuarded-then-readText pairs: those resolved this 3-level path TWICE,
// and the readText half finished with an UNGUARDED VFS::open (System_Utils.cpp)
// so the exists probe was carrying the only permission check on that read.
static bool readRosterJson(const char* tag, String& out) {
  AuthPerfScope _perf("roster.read(open+slurp)");
  // isAdminUser and isSuperAdminUser previously read via readText(), which holds
  // a PollPauseGuard to keep sensor polling off the I2C bus during file I/O
  // (System_Utils.cpp:823). Sharing the read must not silently drop that.
  PollPauseGuard pollGuard;
  out = "";
  File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth(tag));
  if (!f) return false;
  out = f.readString();
  f.close();
  return true;
}

// Verbatim body of the old isSuperAdminUser, from the point the file contents
// were in hand. Do not "clean up" the string scanning — see the note above.
static bool superFromRosterJson(const String& json, const String& who) {
  AuthPerfScope _perf("roster.scanSuper");
  int usersIdx = json.indexOf("\"users\"");
  if (usersIdx < 0) return false;
  // First user (device owner) — for the no-explicit-super fallback below.
  String firstUser = "";
  {
    int fk = json.indexOf("\"username\"", usersIdx);
    // No account in the roster at all — nobody is super. Bail rather than fall
    // through with an empty firstUser (see isAdminUser()).
    if (fk < 0) return false;
    int q1 = json.indexOf('"', json.indexOf(':', fk) + 1);
    int q2 = json.indexOf('"', q1 + 1);
    if (q1 > 0 && q2 > q1) firstUser = json.substring(q1 + 1, q2);
  }
  if (firstUser.length() == 0) return false;  // unparseable owner — fail closed
  bool anyExplicitSuper = false;
  int pos = usersIdx;
  while (true) {
    int uKey = json.indexOf("\"username\"", pos);
    if (uKey < 0) break;
    int uq1 = json.indexOf('"', json.indexOf(':', uKey) + 1);
    int uq2 = json.indexOf('"', uq1 + 1);
    if (uq1 < 0 || uq2 <= uq1) break;
    String uname = json.substring(uq1 + 1, uq2);
    int rKey = json.indexOf("\"role\"", uKey);
    int nextU = json.indexOf("\"username\"", uKey + 1);
    if (rKey > 0 && (nextU < 0 || rKey < nextU)) {
      int rq1 = json.indexOf('"', json.indexOf(':', rKey) + 1);
      int rq2 = json.indexOf('"', rq1 + 1);
      String role = (rq1 > 0 && rq2 > rq1) ? json.substring(rq1 + 1, rq2) : String("");
      if (role == "superadmin") {
        anyExplicitSuper = true;
        if (uname == who) return true;
      }
    }
    pos = uq2 + 1;
  }
  // Fallback: ONLY when no account is explicitly superadmin, treat the owner
  // (first user) as super so super-only recovery (e.g. factoryreset) can never
  // be permanently locked out — e.g. on a users.json restored from before this
  // tier existed. On a normally-created device the owner is explicitly
  // superadmin and returns above. Guarded by anyExplicitSuper so a real super
  // roster is never silently widened, and it grants super to the first user
  // only — never anyone else.
  return (!anyExplicitSuper && who == firstUser);
}

// Verbatim body of getUserRole, from the point the file contents were in hand.
// A FOURTH parser, and deliberately so: unlike roleFromRosterJson it does NOT
// honour "banned", does NOT lowercase, and does NOT validate against
// isKnownUserRole — it returns the stored bytes as-is and defaults a missing
// role to "user". userAccountRank's guest branch has always used exactly this.
// Substituting roleFromRosterJson here silently RAISED a banned guest's rank by
// a tier (roleFromRosterJson returns false on banned, so the guest branch was
// skipped) and lowered a "Guest"-cased account's. Share the read, not the parser.
static bool legacyRoleFromRosterJson(const String& json, const String& username,
                                     String& outRole) {
  AuthPerfScope _perf("roster.parseRoleLegacy");
  outRole = "";
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return false;
  JsonArray users = doc["users"].as<JsonArray>();
  if (!users) return false;
  for (JsonObject uObj : users) {
    const char* uname = uObj["username"] | "";
    if (username == uname) {
      outRole = String(uObj["role"] | "user");
      return true;
    }
  }
  return false;
}

// Verbatim body of the old getUserAuthorizationRole, from the point the file
// contents were in hand.
static bool roleFromRosterJson(const String& json, const String& username,
                               String& roleOut) {
  AuthPerfScope _perf("roster.parseRole");
  roleOut = String();
  PSRAM_JSON_DOC(doc);
  const DeserializationError err = deserializeJson(doc, json);
  if (err) return false;
  const JsonArray users = doc["users"].as<JsonArray>();
  if (!users) return false;
  bool firstAccount = true;
  for (JsonObject user : users) {
    const char* storedName = user["username"] | "";
    // Usernames are currently exact/case-sensitive in credential and role
    // lookup. Do not let a case-colliding ordinary account authorize a Guest
    // whose name differs only by case.
    if (username != storedName) {
      firstAccount = false;
      continue;
    }
    if (user["banned"] | false) return false;
    const JsonVariantConst roleField = user["role"];
    String role;
    if (roleField.isNull()) {
      // Compatibility with pre-role rosters is deliberate elsewhere in this
      // module: the first account remains the recovery owner, while another
      // account with no legacy role is an ordinary user. Missing is distinct
      // from a present-but-malformed/unknown role, which stays fail-closed.
      role = firstAccount ? "superadmin" : "user";
    } else if (roleField.is<const char*>()) {
      role = String(roleField.as<const char*>());
    } else {
      return false;
    }
    role.toLowerCase();
    if (!isKnownUserRole(role)) return false;
    roleOut = role;
    return true;
  }
  return false;
}

bool isSuperAdminUser(const String& who) {
  AuthPerfScope _perf("isSuperAdminUser");
  // An empty identity is nobody — never super. See isAdminUser() for why this
  // guards the first-user fallback below.
  if (who.length() == 0) return false;
  // Same rule as isAdminUser: the bare bond name is super only on the task
  // running the live ESP-NOW bond identity.
  if (who == kBondAdminUser) {
    return bondIdentityLive(currentAuthContext());
  }
  if (!filesystemReady) return false;
  // Serialize with role writes — an unlocked read can tear mid-write and
  // misjudge privilege. Reentrant per task, so nested FS ops are fine.
  FsLockGuard _g("user.isSuperAdmin");
  String json;
  if (!readRosterJson("user.isSuperAdmin", json)) return false;
  return superFromRosterJson(json, who);
}

bool bondIdentityLive(const AuthContext& ctx) {
#if ENABLE_BONDED_MODE
  // The bonded-command path (v4_handle_cmd) is the only producer of this
  // tuple, and only after validating the bond session token.
  if (ctx.transport != SOURCE_ESPNOW || ctx.user != kBondAdminUser) return false;
  extern bool isBondSessionTokenValid();
  return isBondSessionTokenValid();
#else
  (void)ctx;
  return false;
#endif
}

// Combined lookup for resolveRole(): both answers from ONE read of the roster.
// Semantically identical to calling getUserAuthorizationRole() then
// isSuperAdminUser(), because it runs the same two parsers over the same bytes
// — it just does not read the file twice.
//
// NOTE: this deliberately does NOT replicate isSuperAdminUser()'s bond-admin
// branch. resolveRole() returns for kBondAdminUser before it ever gets here
// (System_Filesystem.cpp), so that path cannot reach this function. If you add
// a second caller, handle the bond identity yourself or call the two public
// functions instead.
bool getUserRoleAndSuper(const String& who, String& roleOut, bool& isSuperOut) {
  AuthPerfScope _perf("getUserRoleAndSuper");
  roleOut = String();
  isSuperOut = false;
  if (!filesystemReady || who.length() == 0) return false;
  FsLockGuard guard("users.roleAndSuper");
  if (!guard.held && !isFsLockedByCurrentTask()) return false;
  String json;
  if (!readRosterJson("user.roleAndSuper", json)) return false;
  const bool haveRole = roleFromRosterJson(json, who, roleOut);
  // Evaluated even when the role lookup fails, to match the old call order in
  // resolveRole — which bailed to ANON on a failed role lookup and therefore
  // never consulted super. Callers must preserve that precedence themselves.
  isSuperOut = superFromRosterJson(json, who);
  return haveRole;
}


// Privilege rank for a role *name*. See kRoleRank* in System_User.h.
// A caller may not demote/ban/delete a target of higher rank, nor grant a
// role above its own (enforced in the user-mutation handlers).
int userRoleRank(const String& role) {
  if (role == "superadmin") return kRoleRankSuperAdmin;
  if (role == "admin")      return kRoleRankAdmin;
  if (role == "guest")      return kRoleRankGuest;
  return kRoleRankUser;  // "user" and unrecognised names
}

bool isKnownUserRole(const String& role) {
  return role == "guest" || role == "user" || role == "admin" || role == "superadmin";
}

// Authoritative role lookup at the access boundary: an unreadable, missing or
// malformed account must never default to the ordinary-user role, so this
// fails closed rather than falling back the way the older
// isAdminUser()/isGuestUser() chain does.
//
// THIS IS A HOT PATH, despite reading and parsing users.json on every call.
// It is reached from:
//   - resolveRole (System_Filesystem.cpp) -> checkPerm, so once per *guarded
//     VFS operation*, and once per *directory entry* from the listing walks
//     in FileManager::loadDirectory and buildFilesListing;
//   - authorizeCommand (System_Utils.cpp), so once per command from a named
//     account on every transport;
//   - WebServer_Utils.cpp, so once per authenticated HTTP request;
//   - BLE_Peers.cpp and OLED_Utils.cpp.
// resolveRole then calls isSuperAdminUser() as well, which reads the same file
// a second time, so a single directory entry costs two roster reads.
//
// Do NOT add a naive cache here. Every caller depends on this reading LIVE
// state so that a ban, demotion or delete takes effect immediately; a cache
// that outlives a mutation is an authorization bypass, not a slow render. Any
// memoisation must be keyed on gIdentityGeneration (see System_AuthIdentity.h)
// AND must also cover out-of-band writes to users.json through generic file
// operations, which a superadmin can perform and which bump nothing today.
bool getUserAuthorizationRole(const String& username, String& roleOut) {
  AuthPerfScope _perf("getUserAuthorizationRole");
  roleOut = String();
  if (!filesystemReady || username.length() == 0) return false;
  FsLockGuard guard("users.authorization_role");
  if (!guard.held && !isFsLockedByCurrentTask()) return false;
  String json;
  if (!readRosterJson("user.authorizationRole", json)) return false;
  return roleFromRosterJson(json, username, roleOut);
}

bool userMayControlOtherSessions(const String& username) {
  String role;
  return getUserAuthorizationRole(username, role) && role != "guest";
}

static bool userMayPublishCm5Presence(const String& username) {
  return userMayControlOtherSessions(username);
}

// CM5 readiness is stronger than ordinary guest login: it can admit native
// device work, so an unreadable/missing/malformed account must never default
// to the ordinary-user role. That is why the permission is resolved here, once
// at UART login, and then latched into the session — it is deliberately NOT
// re-evaluated at the five-second heartbeat cadence, so the roster read below
// happens once per login rather than once per heartbeat.
uint32_t publishUartAccountSession(const String& username,
                                   uint32_t* transportEpochOut) {
  if (transportEpochOut) *transportEpochOut = 0;
  // Role/delete mutations bump this generation after the durable write and
  // before their session-revocation sweep. Ban/unban follows the same order.
  // If any mutation overlaps this tiny publication window, retain the named
  // login but conservatively disable CM5 presence until the next login.
  const uint32_t generationBefore =
      gIdentityGeneration.load(std::memory_order_acquire);
  const bool allowCm5Presence = userMayPublishCm5Presence(username);
  const uint32_t sessionEpoch =
      uartLinkSessionAuthenticated(username, allowCm5Presence,
                                   transportEpochOut);
  if (sessionEpoch != 0 &&
      gIdentityGeneration.load(std::memory_order_acquire) !=
          generationBefore) {
    (void)uartLinkSessionRestrictCm5PresenceIfUser(username, sessionEpoch);
  }
  return sessionEpoch;
}

// ============================================================================
// Credential attempt table (contract in System_User.h)
// ============================================================================

namespace credential_attempts {

constexpr uint8_t kMixedUsers = 0x01;   // failures named more than one account
constexpr uint8_t kLockApplied = 0x02;  // lockedUntil holds the last lock's end

struct Entry {
  char peer[48];                         // "" = unused
  char user[kPublicUsernameMaxLen + 1];  // the account its failures named
  CommandSource transport;
  uint8_t failCount;
  uint8_t flags;
  // Milliseconds on the 64-bit esp_timer clock, which does not wrap; a
  // 32-bit millis() end time would read as running again after ~24.8 days.
  uint64_t windowStart;  // first failure
  uint64_t lastFailMs;   // latest failure (eviction order)
  uint64_t lockedUntil;  // valid while kLockApplied is set
};

uint64_t nowMs() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

// Slots 0..3 belong to serial, UART, the display and MQTT (one key each: a
// console's fixed peer; every MQTT client shares its broker's key); the rest
// are the pool shared by network peers (the web today).
constexpr size_t kReservedSlots = 4;
constexpr size_t kSlots = kReservedSlots + CREDENTIAL_ATTEMPT_POOL_SLOTS;
// PSRAM: peers, public usernames and counters, no secrets.
EXT_RAM_BSS_ATTR Entry sEntries[kSlots];
portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t sTotalFailures = 0;

int reservedSlot(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL: return 0;
    case SOURCE_UART: return 1;
    case SOURCE_LOCAL_DISPLAY: return 2;
    case SOURCE_MQTT: return 3;
    default: return -1;
  }
}

const char* peerOrUnknown(const char* peer) {
  return (peer && peer[0]) ? peer : "unknown";
}

unsigned long lockoutMs(uint8_t count) {
  if (count >= LOGIN_LOCKOUT_TIER3_COUNT) return LOGIN_LOCKOUT_TIER3_MS;
  if (count >= LOGIN_LOCKOUT_TIER2_COUNT) return LOGIN_LOCKOUT_TIER2_MS;
  if (count >= LOGIN_LOCKOUT_TIER1_COUNT) return LOGIN_LOCKOUT_TIER1_MS;
  return 0;
}

bool lockRunning(const Entry& e, uint64_t now) {
  return (e.flags & kLockApplied) != 0 && now < e.lockedUntil;
}

// Unused, or unlocked and quiet for a whole window: counted from the first
// failure, or from the end of the last lock once one was applied.
bool expired(const Entry& e, uint64_t now) {
  if (e.peer[0] == '\0') return true;
  if (lockRunning(e, now)) return false;
  const uint64_t anchor =
      (e.flags & kLockApplied) ? e.lockedUntil : e.windowStart;
  return now > anchor && (now - anchor) > LOGIN_ATTEMPT_WINDOW_MS;
}

// The live entry for this key, or nullptr; an expired entry for the key is
// cleared on the way. Caller holds sMux.
Entry* find(CommandSource transport, const char* peer, uint64_t now) {
  const int reserved = reservedSlot(transport);
  Entry* match = nullptr;
  if (reserved >= 0) {
    match = &sEntries[reserved];
  } else {
    for (size_t i = kReservedSlots; i < kSlots && !match; ++i) {
      Entry& e = sEntries[i];
      if (e.peer[0] != '\0' && e.transport == transport &&
          strncmp(e.peer, peer, sizeof(e.peer) - 1) == 0) {
        match = &e;
      }
    }
  }
  if (match && expired(*match, now)) {
    memset(match, 0, sizeof(*match));
    match = nullptr;
  }
  return match;
}

// The entry a failure for this key counts in: its live entry, else a fresh
// one in an unused or expired slot, else the unlocked pool entry with the
// oldest failure. nullptr when every pool entry holds a running lock.
// Caller holds sMux.
Entry* claim(CommandSource transport, const char* peer, uint64_t now) {
  Entry* e = find(transport, peer, now);
  if (e) return e;
  const int reserved = reservedSlot(transport);
  if (reserved >= 0) {
    e = &sEntries[reserved];
  } else {
    Entry* oldest = nullptr;
    for (size_t i = kReservedSlots; i < kSlots && !e; ++i) {
      Entry& candidate = sEntries[i];
      if (expired(candidate, now)) {
        e = &candidate;
      } else if (!lockRunning(candidate, now) &&
                 (!oldest || candidate.lastFailMs < oldest->lastFailMs)) {
        oldest = &candidate;
      }
    }
    if (!e) e = oldest;
    if (!e) return nullptr;
  }
  memset(e, 0, sizeof(*e));
  strlcpy(e->peer, peer, sizeof(e->peer));
  e->transport = transport;
  e->windowStart = now;
  return e;
}

}  // namespace credential_attempts

bool credentialAttemptAllowed(CommandSource transport, const char* peer,
                              unsigned long* retryMsOut) {
  using namespace credential_attempts;
  peer = peerOrUnknown(peer);
  const uint64_t now = nowMs();
  unsigned long retryMs = 0;
  bool allowed = true;
  portENTER_CRITICAL(&sMux);
  const Entry* e = find(transport, peer, now);
  if (e) {
    if (lockRunning(*e, now)) {
      allowed = false;
      retryMs = static_cast<unsigned long>(e->lockedUntil - now);
    }
  } else if (reservedSlot(transport) < 0) {
    // A new network peer needs a pool slot to count its failures. While every
    // slot holds a running lock it is refused until the earliest lock ends.
    allowed = false;
    for (size_t i = kReservedSlots; i < kSlots; ++i) {
      if (!lockRunning(sEntries[i], now)) {
        allowed = true;
        break;
      }
      const unsigned long left =
          static_cast<unsigned long>(sEntries[i].lockedUntil - now);
      if (retryMs == 0 || left < retryMs) retryMs = left;
    }
    if (allowed) retryMs = 0;
  }
  portEXIT_CRITICAL(&sMux);
  if (retryMsOut) *retryMsOut = retryMs;
  return allowed;
}

void credentialAttemptRecord(CommandSource transport, const char* peer,
                             const char* username, bool success) {
  using namespace credential_attempts;
  peer = peerOrUnknown(peer);
  if (!username) username = "";
  const uint64_t now = nowMs();
  unsigned long lockMs = 0;
  int failures = -1;  // -1: no slot was free to count this failure
  portENTER_CRITICAL(&sMux);
  if (success) {
    // Only the account that succeeded is cleared: an entry whose failures
    // named another account (or several) keeps counting.
    Entry* e = find(transport, peer, now);
    if (e && !(e->flags & kMixedUsers) && strcmp(e->user, username) == 0) {
      memset(e, 0, sizeof(*e));
    }
  } else {
    ++sTotalFailures;
    Entry* e = claim(transport, peer, now);
    if (e) {
      const bool fits = strlen(username) <= kPublicUsernameMaxLen;
      if (e->failCount == 0 && fits) {
        strlcpy(e->user, username, sizeof(e->user));
      } else if (!fits || strcmp(e->user, username) != 0) {
        e->flags |= kMixedUsers;
      }
      if (e->failCount < UINT8_MAX) ++e->failCount;
      e->lastFailMs = now;
      lockMs = lockoutMs(e->failCount);
      if (lockMs > 0) {
        e->lockedUntil = now + lockMs;
        e->flags |= kLockApplied;
      }
      failures = e->failCount;
    }
  }
  portEXIT_CRITICAL(&sMux);
  if (success) return;
  // Diagnostics and the event run after the lock is released.
  if (lockMs > 0) {
    DEBUG_AUTHF("[brute] peer=%s failures=%d locked=%lus", peer, failures,
                lockMs / 1000UL);
    BROADCAST_PRINTF("[auth] login locked peer=%s failures=%d duration=%lus",
                     peer, failures, lockMs / 1000UL);
    char lockSec[16];
    snprintf(lockSec, sizeof(lockSec), "%lus", lockMs / 1000UL);
    systemEventPost(SYSEVT_LOGIN_LOCKED, peer, lockSec);
  } else {
    DEBUG_AUTHF("[brute] peer=%s failures=%d (below lockout threshold)", peer,
                failures);
  }
  (void)failures;
}

uint8_t credentialAttemptFailures(CommandSource transport, const char* peer) {
  using namespace credential_attempts;
  peer = peerOrUnknown(peer);
  const uint64_t now = nowMs();
  portENTER_CRITICAL(&sMux);
  const Entry* e = find(transport, peer, now);
  const uint8_t failures = e ? e->failCount : 0;
  portEXIT_CRITICAL(&sMux);
  return failures;
}

void credentialAttemptForget(CommandSource transport, const char* peer) {
  using namespace credential_attempts;
  peer = peerOrUnknown(peer);
  const uint64_t now = nowMs();
  portENTER_CRITICAL(&sMux);
  Entry* e = find(transport, peer, now);
  if (e) memset(e, 0, sizeof(*e));
  portEXIT_CRITICAL(&sMux);
}

const char* credentialAttemptConsolePeer(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL: return "local";
    case SOURCE_UART: return "uart";
    case SOURCE_LOCAL_DISPLAY: return "display";
    default: return nullptr;
  }
}

uint32_t getTotalFailedLoginCount() {
  return credential_attempts::sTotalFailures;
}

// ============================================================================
// Centralized Transport Authentication Management
// ============================================================================

bool validateTransportCredentials(CommandSource transport,
                                  const String& username,
                                  const String& password) {
  if (!isFsLockedByCurrentTask()) return false;
  (void)transport;
  return isValidUser(username, password);
}

void recordTransportLoginResult(CommandSource transport,
                                const String& username,
                                bool success,
                                const char* reason) {
  recordLoginAttempt(transport, username, String(), success,
                     reason ? reason : (success ? "Login successful"
                                                : "Authentication failed"));
  if (success) updateUserLastSeen(username);
}

bool loginTransport(CommandSource transport, const String& username, const String& password) {
  // serial / display / bluetooth are the credential-login transports that funnel
  // through here (web uses cookie sessions; G2 uses pair-time identity). Audit
  // each attempt once via the shared recordLoginAttempt() front-door.
  const bool credentialTransport = (transport == SOURCE_SERIAL ||
                                    transport == SOURCE_LOCAL_DISPLAY ||
                                    transport == SOURCE_BLUETOOTH ||
                                    transport == SOURCE_UART);

  // Serialize credential validation through publication for every native
  // transport. Account writers use this recursive filesystem lock and revoke
  // only after releasing it, so an old password can never publish after a
  // reset/delete/ban sweep observed an empty session slot.
  FsLockGuard authGuard("users.login_transport");
  if (!authGuard.held && !isFsLockedByCurrentTask()) {
    if (credentialTransport)
      recordLoginAttempt(transport, username, String(), false,
                         "Authentication store unavailable");
    return false;
  }
  if (!isValidUser(username, password)) {
    if (credentialTransport)
      recordLoginAttempt(transport, username, String(), false, "Invalid credentials");
    return false;
  }

  // Set auth state based on transport
  bool published = false;
  switch (transport) {
    case SOURCE_SERIAL:
      serialTransportSessionAuthenticated(username);
      {
        String liveUser; bool authed = false;
        (void)serialTransportSessionSnapshot(liveUser, authed);
        published = authed && liveUser == username;
      }
      break;

    case SOURCE_UART:
      if (!uartLinkIsRunning()) break;
      publishUartAccountSession(username);
      {
        char liveUser[kPublicUsernameMaxLen + 1] = {};
        published = uartLinkSessionSnapshot(liveUser, sizeof(liveUser)) &&
                    username == liveUser;
      }
      break;

    case SOURCE_LOCAL_DISPLAY:
      localDisplayTransportSessionAuthenticated(username);
      {
        String liveUser; bool authed = false;
        (void)localDisplayTransportSessionSnapshot(liveUser, authed);
        published = authed && liveUser == username;
      }
      break;

    case SOURCE_BLUETOOTH:
      // Validation-only here. The BLE owner binds the exact connection before
      // releasing this auth transaction via its dedicated deferred login path.
      published = true;
      break;
      
    case SOURCE_WEB:
      // Web auth is handled separately via session cookies
      // This function doesn't apply to web transport
      break;

    case SOURCE_G2_GLASSES:
      // G2 doesn't use credential login. Identity is captured at pair time
      // by bleStampPairedByIfBlank, called from `bleautoreconnect g2-glasses on`
      // executed under an already-authenticated CLI session. No code path
      // should be calling loginTransport(SOURCE_G2_GLASSES) — return false
      // so it surfaces clearly if one ever does.
      break;

    default:
      break;
  }

  if (credentialTransport)
    recordLoginAttempt(transport, username, String(), published,
                       published ? "Login successful" : "Session publication failed");
  if (published) updateUserLastSeen(username);
  return published;
}

namespace {
// Holds the source transport's lifecycle writer while confirming that the
// exact epoch still belongs to the named user. Scope it to the check alone:
// the writer must be released before any other transport's writer is taken
// and before any log append, roster access or console output.
class NamedSourceUseGuard {
 public:
  NamedSourceUseGuard(CommandSource source, const String& user,
                      TransportSessionEpoch epoch)
      : source_(source) {
    if (source == SOURCE_SERIAL) {
      locked_ = serialTransportSessionBeginDelivery(epoch);
      if (locked_) {
        String liveUser; bool authed = false;
        const TransportSessionEpoch liveEpoch =
            serialTransportSessionSnapshot(liveUser, authed);
        locked_ = authed && liveEpoch == epoch && liveUser == user;
        if (!locked_) serialTransportSessionEndDelivery();
      }
    } else if (source == SOURCE_LOCAL_DISPLAY) {
      locked_ = localDisplayTransportSessionBeginDelivery(epoch);
      if (locked_) {
        String liveUser; bool authed = false;
        const TransportSessionEpoch liveEpoch =
            localDisplayTransportSessionSnapshot(liveUser, authed);
        locked_ = authed && liveEpoch == epoch && liveUser == user;
        if (!locked_) localDisplayTransportSessionEndDelivery();
      }
    } else if (source == SOURCE_UART) {
      locked_ = uartLinkNamedSessionBeginUse(epoch, user);
    }
  }
  ~NamedSourceUseGuard() {
    if (!locked_) return;
    if (source_ == SOURCE_SERIAL) serialTransportSessionEndDelivery();
    else if (source_ == SOURCE_LOCAL_DISPLAY)
      localDisplayTransportSessionEndDelivery();
    else if (source_ == SOURCE_UART) uartLinkNamedSessionEndUse();
  }
  explicit operator bool() const { return locked_; }
 private:
  CommandSource source_;
  bool locked_ = false;
};
}  // namespace

bool loginTransportFromNamedSession(CommandSource target,
                                    const String& username,
                                    const String& password,
                                    CommandSource source,
                                    const String& sourceUser,
                                    TransportSessionEpoch sourceEpoch) {
  if (target == source ||
      (target != SOURCE_SERIAL && target != SOURCE_UART &&
       target != SOURCE_LOCAL_DISPLAY)) return false;

  // Credential writers use this same recursive lock and revoke only after
  // releasing it. Keeping it through exact source revalidation and target
  // publication makes the operation linearizable with reset/delete/ban.
  FsLockGuard authGuard("users.cross_transport_login");
  if (!authGuard.held && !isFsLockedByCurrentTask()) return false;
  if (!isValidUser(username, password)) {
    recordLoginAttempt(target, username, String(), false,
                       "Invalid credentials");
    return false;
  }
  if (!userMayControlOtherSessions(sourceUser)) return false;

  // Validate the source under its own writer, then release it before the
  // target publishes under its writer. The two lifecycle writers (and the
  // UART link's lifecycle mutex) are therefore never nested, so no lock order
  // exists between them, and the audit append and roster update below run
  // with no session writer held. The filesystem lock above still serializes
  // this whole operation with credential writers.
  {
    NamedSourceUseGuard sourceGuard(source, sourceUser, sourceEpoch);
    if (!sourceGuard) return false;
  }

  bool published = false;
  if (target == SOURCE_SERIAL) {
    serialTransportSessionAuthenticated(username);
    String liveUser; bool authed = false;
    (void)serialTransportSessionSnapshot(liveUser, authed);
    published = authed && liveUser == username;
  } else if (target == SOURCE_LOCAL_DISPLAY) {
    localDisplayTransportSessionAuthenticated(username);
    String liveUser; bool authed = false;
    (void)localDisplayTransportSessionSnapshot(liveUser, authed);
    published = authed && liveUser == username;
  } else {
    if (!uartLinkIsRunning()) return false;
    publishUartAccountSession(username);
    char liveUser[kPublicUsernameMaxLen + 1] = {};
    published = uartLinkSessionSnapshot(liveUser, sizeof(liveUser)) &&
                username == liveUser;
  }
  recordLoginAttempt(target, username, String(), published,
                     published ? "Login successful" : "Session publication failed");
  if (published) updateUserLastSeen(username);
  return published;
}

bool logoutTransportFromNamedSession(CommandSource target,
                                     CommandSource source,
                                     const String& sourceUser,
                                     TransportSessionEpoch sourceEpoch,
                                     const char** denialOut) {
  if (target == source ||
      (target != SOURCE_SERIAL && target != SOURCE_UART &&
       target != SOURCE_LOCAL_DISPLAY)) return false;
  FsLockGuard authGuard("users.cross_transport_logout");
  if (!authGuard.held && !isFsLockedByCurrentTask()) return false;
  if (!userMayControlOtherSessions(sourceUser)) return false;
  // The rank rule of the user mutations (ban, unban, sessionrevoke): an
  // account may not sign out a session of a higher-privileged account.
  {
    const String targetUser = getTransportUser(target);
    if (targetUser.length() &&
        userAccountRank(targetUser) > userAccountRank(sourceUser)) {
      if (denialOut) *denialOut = "Error: Cannot sign out a higher-privileged account";
      return false;
    }
  }
  // Same shape as the login above: the source check releases its writer
  // before the target's writer is taken.
  {
    NamedSourceUseGuard sourceGuard(source, sourceUser, sourceEpoch);
    if (!sourceGuard) return false;
  }
  logoutTransport(target);
  return true;
}

void logoutTransport(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL:
      serialTransportSessionCleared();
      break;

    case SOURCE_UART:
      uartLinkSessionCleared(UartLinkSessionClearReason::TransportLogout);
      break;
      
    case SOURCE_LOCAL_DISPLAY:
      localDisplayTransportSessionCleared();
      break;

    case SOURCE_G2_GLASSES:
      // Logout revokes lens authority. MAC/reconnect data may remain, but a
      // later automatic reconnect must not silently assign a replacement
      // owner; explicit authenticated pairing is required.
      g2PairedUserClear();
      break;

    case SOURCE_BLUETOOTH:
      bleRevokeAllSessions();
      break;

    case SOURCE_WEB:
      // Web logout is handled separately via session management
      break;
      
    default:
      break;
  }
}

// ============================================================================
// revokeUserSessions — force-logout helper used by user-mutation paths
// ============================================================================
//
// Walks every transport's session state and clears any session belonging to
// `username`. Optionally skips a single web SID (for self-password-change
// where the user just authenticated with the new password and would be
// annoyed to be kicked out of the session they're typing into) and/or skips
// one transport entirely (same reason for serial/oled/bluetooth — those
// transports use a single session-per-transport model, no SID).
//
// Callers:
//   * cmd_user_delete  → revoke everywhere (account is gone)
//   * cmd_user_demote  → revoke everywhere (was admin, now isn't —
//                        existing session was running with admin perms)
//   * cmd_user_changepassword → revoke everywhere EXCEPT the calling
//                        session (self-service: credential rotated,
//                        kick other devices but keep current alive)
//   * cmd_user_resetpassword  → revoke everywhere (admin reset target's
//                        password; admin's own session is a different
//                        user, so no exception needed)
//
// This is intentionally NOT wired to gIdentityGeneration. The clock is for
// permission-topology changes that invalidate auth-derived caches like
// FileManager. Session revocation is a separate concern with its own
// trigger points — calling it from mutators directly is cleaner than
// bumping the clock and having a generic listener trawl sessions on
// every cache-relevant event.
int revokeUserSessions(const String& username,
                       const String& reason,
                       const String& exceptSid,
                       CommandSource exceptTransport) {
  if (username.length() == 0) return 0;
  int revoked = 0;

  // The web helper owns the String session table lock and rechecks the exact
  // bearer SID before revocation, so a concurrently reused slot cannot revoke
  // the replacement browser.
#if ENABLE_HTTP_SERVER
  revoked += webRevokeSessionsForUser(username, reason, exceptSid);
#endif

  // Serial transport: single per-device session, skipped if this is the
  // calling transport (self-modify case).
  if (exceptTransport != SOURCE_SERIAL) {
    String serialUser;
    bool serialAuthed = false;
    (void)serialTransportSessionSnapshot(serialUser, serialAuthed);
    if (serialAuthed && serialUser.equalsIgnoreCase(username)) {
      serialTransportSessionCleared();
      revoked++;
    }
  }

  // UART host link: single per-port session, same shape as serial.
  if (exceptTransport != SOURCE_UART &&
      uartLinkSessionClearIfUser(username)) {
    revoked++;
  }

  // Local display (OLED).
  if (exceptTransport != SOURCE_LOCAL_DISPLAY) {
    String displayUser;
    bool displayAuthed = false;
    (void)localDisplayTransportSessionSnapshot(displayUser, displayAuthed);
    if (displayAuthed && displayUser.equalsIgnoreCase(username)) {
      localDisplayTransportSessionCleared();
      revoked++;
    }
  }

  // G2 glasses (BLE-attached lens; pair-time identity).
  // The pair stays valid (MAC + autoReconnect kept) but the lens stops being
  // able to act as this user until re-stamp via `bleautoreconnect g2-glasses on`.
  if (exceptTransport != SOURCE_G2_GLASSES &&
      g2PairedUserClearIfMatches(username)) {
    revoked++;
  }

  // Bluetooth (its own session table inside the BT module).
  if (exceptTransport != SOURCE_BLUETOOTH) {
    revoked += bleRevokeUserSessions(username);
  }

  if (revoked > 0) {
    WARN_USERF("[SESSION-REVOKE] user='%s' count=%d reason='%s'",
               username.c_str(), revoked, reason.c_str());
  }
  return revoked;
}

bool isTransportAuthenticated(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL: {
      String user; bool authed = false;
      (void)serialTransportSessionSnapshot(user, authed);
      return authed;
    }

    case SOURCE_UART:
      return uartLinkSessionEpoch() != 0;

    case SOURCE_LOCAL_DISPLAY: {
      String user; bool authed = false;
      (void)localDisplayTransportSessionSnapshot(user, authed);
      return authed;
    }

    case SOURCE_G2_GLASSES:
      // G2 is "authenticated" iff the lens is paired AND pairedByUser is
      // a non-empty captured username. Pairing without pairedByUser counts
      // as not-authenticated (stuck-stamp state — see G2_HijackCmd.cpp).
      return g2PairedUserGet().length() > 0;

    case SOURCE_BLUETOOTH:
      return bleHasAuthenticatedSession();

    case SOURCE_WEB:
      // Web auth requires request context, can't check here
      return false;

    default:
      return false;
  }
}

String getTransportUser(CommandSource transport) {
  switch (transport) {
    case SOURCE_SERIAL: {
      String user; bool authed = false;
      (void)serialTransportSessionSnapshot(user, authed);
      return authed ? user : String();
    }

    case SOURCE_UART:
      return uartSessionUserSnapshot();


    case SOURCE_LOCAL_DISPLAY: {
      String user; bool authed = false;
      (void)localDisplayTransportSessionSnapshot(user, authed);
      return authed ? user : String();
    }

    case SOURCE_G2_GLASSES:
      // Pair-time stamp. Empty string when not yet stamped / cleared by
      // revoke. Callers (G2 page handlers etc.) get the username string
      // identical to what gLocalDisplayUser would have returned before
      // the OLED/G2 split.
      return g2PairedUserGet();

    case SOURCE_BLUETOOTH:
      {
        uint16_t connId = 0;
        String user;
        if (bleGetAuthenticatedSessionInfo(0, connId, user)) {
          return user;
        }
        return String();
      }

    case SOURCE_WEB:
      // Web user requires request context, can't get here
      return String();

    default:
      return String();
  }
}

bool isTransportAdmin(CommandSource transport) {
  String user = getTransportUser(transport);
  if (user.length() == 0) {
    return false;
  }
  return isAdminUser(user);
}

// ============================================================================
// Password Hashing
// ============================================================================

// Password hashing
String hashUserPassword(const String& password) {
  AuthPerfScope _perf("hashUserPassword(PBKDF2)");
  if (password.length() == 0) return "";

  // Use PBKDF2-HMAC-SHA256 for strong password hashing
  String salt = getDeviceEncryptionKey();
  const int iterations = 10000;  // NIST minimum recommendation
  uint8_t hash[32];  // 256-bit output

  int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
    MBEDTLS_MD_SHA256,
    (const uint8_t*)password.c_str(), password.length(),
    (const uint8_t*)salt.c_str(), salt.length(),
    iterations,
    32,  // Output 32 bytes (256 bits)
    hash
  );

  secureClearString(salt);

  if (ret != 0) {
    DEBUG_AUTHF("[PBKDF2] Hash generation failed: %d", ret);
    return "";
  }

  // Encode as hex
  char hashStr[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hashStr + (i * 2), 3, "%02x", hash[i]);
  }
  hashStr[64] = '\0';

  // Format: PBKDF2:iterations:hex
  char hashFmt[80];
  snprintf(hashFmt, sizeof(hashFmt), "PBKDF2:%d:%s", iterations, hashStr);
  return String(hashFmt);
}

bool verifyUserPassword(const String& inputPassword, const String& storedHash) {
  if (inputPassword.length() == 0 || storedHash.length() == 0) return false;

  // Verify PBKDF2 format
  if (!storedHash.startsWith("PBKDF2:")) {
    DEBUG_AUTHF("[PBKDF2] Invalid hash format detected");
    return false;
  }

  // Hash the input password and compare
  String inputHash = hashUserPassword(inputPassword);
  return (inputHash == storedHash);
}

namespace {

struct UserPasswordMutationContext {
  const String* hashed;
  bool requireChangeOnNextLogin;
};

bool applyUserPasswordMutation(JsonDocument& settings, void* opaque) {
  const auto* context = static_cast<const UserPasswordMutationContext*>(opaque);
  if (!context || !context->hashed || context->hashed->length() == 0) return false;
  if (!settings.is<JsonObject>()) return false;
  settings["password"] = *context->hashed;
  settings["mustChangePassword"] = context->requireChangeOnNextLogin;
  return !settings.overflowed();
}

struct UserGamepadPasswordMutationContext {
  const String* hashed;
};

bool applyUserGamepadPasswordMutation(JsonDocument& settings, void* opaque) {
  const auto* context = static_cast<const UserGamepadPasswordMutationContext*>(opaque);
  if (!context || !context->hashed || context->hashed->length() == 0) return false;
  if (!settings.is<JsonObject>()) return false;
  settings["gamepad_password"] = *context->hashed;
  return !settings.overflowed();
}

}  // namespace

// ----------------------------------------------------------------------------
// Forced password change: one decision for every caller
// ----------------------------------------------------------------------------
// Every authenticated web request asks userMustChangePassword(), so answers
// are cached per account. An entry is used only while both counters it was
// filled under are current: gIdentityGeneration (accounts added, deleted or
// re-roled) and sWrites, bumped after every setUserPassword (the only writer
// of the flag on an existing account; per-user settings merges refuse it). A
// write that lands while an entry is being filled bumps sWrites after the fill
// read it, so that entry is refilled on its next use. Failed lookups are not
// cached.
namespace must_change_cache {

constexpr size_t kSlots = 4;
struct Entry {
  char user[kPublicUsernameMaxLen + 1];  // "" = unused
  uint32_t identityGeneration;
  uint32_t writes;
  bool mustChange;
};
// PSRAM: public usernames and one flag each, no secrets.
EXT_RAM_BSS_ATTR Entry sEntries[kSlots];
size_t sNext = 0;
portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> sWrites{0};

}  // namespace must_change_cache

// A guest cannot change its own password on any interface, so it can never
// satisfy a forced change. Same reading as the web guest gate: the legacy
// recovery owner is not treated as a guest.
static bool accountIsGuestForPasswordChange(const String& username) {
  String role;
  return getUserAuthorizationRole(username, role) && role == "guest" &&
         !isSuperAdminUser(username);
}

// Reads the flag from the account's settings file. False when the account or
// its file cannot be read (the caller does not cache that answer).
static bool readMustChangePassword(const String& username, bool& mustChangeOut) {
  mustChangeOut = false;
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  PSRAM_JSON_DOC(settings);
  if (!loadUserSettings(userId, settings)) return false;
  mustChangeOut = settings["mustChangePassword"] == true &&
                  !accountIsGuestForPasswordChange(username);
  return true;
}

bool userMustChangePassword(const String& username) {
  using namespace must_change_cache;
  if (!filesystemReady || username.length() == 0) return false;
  const bool cacheable = username.length() <= kPublicUsernameMaxLen;
  const uint32_t identity = gIdentityGeneration.load(std::memory_order_acquire);
  const uint32_t writes = sWrites.load(std::memory_order_acquire);
  if (cacheable) {
    bool hit = false;
    bool cached = false;
    portENTER_CRITICAL(&sMux);
    for (const Entry& entry : sEntries) {
      if (entry.identityGeneration == identity && entry.writes == writes &&
          strcmp(entry.user, username.c_str()) == 0) {
        hit = true;
        cached = entry.mustChange;
        break;
      }
    }
    portEXIT_CRITICAL(&sMux);
    if (hit) return cached;
  }

  bool mustChange = false;
  if (!readMustChangePassword(username, mustChange)) return false;
  if (cacheable) {
    portENTER_CRITICAL(&sMux);
    size_t slot = sNext;
    for (size_t i = 0; i < kSlots; ++i) {
      if (strcmp(sEntries[i].user, username.c_str()) == 0) {
        slot = i;
        break;
      }
    }
    if (slot == sNext) sNext = (sNext + 1) % kSlots;
    Entry& entry = sEntries[slot];
    memcpy(entry.user, username.c_str(), username.length() + 1);
    entry.identityGeneration = identity;
    entry.writes = writes;
    entry.mustChange = mustChange;
    portEXIT_CRITICAL(&sMux);
  }
  return mustChange;
}

// Update a user's text password in per-user settings file
bool setUserPassword(const String& username, const String& newPasswordRaw, bool requireChangeOnNextLogin) {
  if (!filesystemReady || username.length() == 0 || newPasswordRaw.length() == 0) return false;
  
  // Get user ID from username
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  
  // Hash the password
  String hashed = hashUserPassword(newPasswordRaw);
  if (hashed.length() == 0) return false;
  
  // Mutate existing user settings in one guarded load→patch→save transaction.
  //
  // The return value is LOAD-BEARING and used to be dropped here. loadUserSettings
  // returns true with an empty object when the file is legitimately ABSENT (the
  // bootstrap case the old "// OK if doesn't exist yet" comment was thinking of),
  // but on a PARSE FAILURE it returns false with the document CLEARED. Continuing
  // from a cleared doc writes a file containing only the one key set below,
  // silently destroying every other per-user setting — including, on the gamepad
  // path, the account's own login credential.
  UserPasswordMutationContext mutation{&hashed, requireChangeOnNextLogin};
  const UserSettingsTxnStatus status =
      runUserSettingsTransaction(userId, applyUserPasswordMutation, &mutation);
  must_change_cache::sWrites.fetch_add(1, std::memory_order_acq_rel);
  if (status == UserSettingsTxnStatus::LoadFailed) {
    ERROR_STORAGEF("[user] password change REFUSED: user_settings/%lu.json is unreadable - "
                   "refusing to overwrite it",
                   (unsigned long)userId);
    systemEventPost(SYSEVT_CONFIG_FILE_CORRUPT, "user_settings",
                    "unreadable at password change");
    return false;
  }
  return status == UserSettingsTxnStatus::Committed;
}

bool isReservedUsername(const String& username) {
  // Audit/dispatch sentinels, compared case-insensitively and ignoring the
  // surrounding whitespace the public intake trims.
  static const char* const kReserved[] = { "system", "AuthBypass", kBondAdminUser };
  String u = username;
  u.trim();
  for (const char* r : kReserved) {
    if (strcasecmp(u.c_str(), r) == 0) return true;
  }
  return false;
}

bool isValidPublicUsername(const String& username, String* errorOut) {
  String u = username;
  u.trim();
  if (u.length() == 0 || u.length() > kPublicUsernameMaxLen) {
    if (errorOut) *errorOut = "Invalid username";
    return false;
  }
  for (size_t i = 0; i < u.length(); ++i) {
    const unsigned char c = (unsigned char)u[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) {
      if (errorOut) *errorOut = "Invalid username";
      return false;
    }
  }
  if (isReservedUsername(u)) {
    if (errorOut) *errorOut = "Invalid username";
    return false;
  }
  return true;
}

bool isValidPublicPassword(const String& password, String* errorOut) {
  if (password.length() < kPublicPasswordMinLen) {
    if (errorOut) *errorOut = "Password must be at least 6 characters";
    return false;
  }
  if (password.length() > kPublicPasswordMaxLen) {
    if (errorOut) *errorOut = "Password is too long";
    return false;
  }
  return true;
}

// ============================================================================
// users.json mutation owner
// ============================================================================
// Every users.json read-modify-write goes through mutateUsersJson. It holds the
// filesystem lock from the guarded open through parse, mutation, serialization
// and writeTextAtomic, so a writer on another task is neither overwritten by a
// stale copy nor interleaved with the temp-file promotion, and a document that
// overflowed or serialized short is never published over the live roster. The
// mutator edits the parsed document and reports whether it changed anything;
// on Failed it has set errorOut and the file is left as it was.
enum class UsersJsonMutation : uint8_t { Failed, Unchanged, Changed };
typedef UsersJsonMutation (*UsersJsonMutator)(JsonDocument& doc, JsonArray users,
                                              void* context, String& errorOut);

static bool mutateUsersJson(const char* tag, UsersJsonMutator mutator, void* context,
                            String& errorOut) {
  if (!filesystemReady) {
    errorOut = "Filesystem not ready";
    return false;
  }
  FsLockGuard guard(tag);
  if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth(tag))) {
    errorOut = "users.json not found";
    return false;
  }
  File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth(tag));
  if (!f) {
    errorOut = "Failed to read users.json";
    return false;
  }
  PSRAM_JSON_DOC(doc);
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    errorOut = "Malformed users.json";
    return false;
  }
  JsonArray users = doc["users"];
  if (!users) {
    errorOut = "Malformed users.json - missing users array";
    return false;
  }
  const UsersJsonMutation outcome = mutator(doc, users, context, errorOut);
  if (outcome == UsersJsonMutation::Failed) return false;
  if (outcome == UsersJsonMutation::Unchanged) return true;
  // A truncated document must never become the roster: refuse an overflowed
  // document and check the serialization against its measured size.
  String json;
  const size_t expected = doc.overflowed() ? 0 : measureJson(doc);
  if (expected == 0 || !json.reserve(expected) || serializeJson(doc, json) != expected ||
      json.length() != expected) {
    errorOut = "Failed to prepare users.json";
    return false;
  }
  if (!writeTextAtomic(USERS_JSON_FILE, json)) {
    logSystemEvent("USERS", "users.json REWRITE FAILED during %s - auth database may be inconsistent", tag);
    errorOut = "Failed to write users.json";
    return false;
  }
  return true;
}

// The roster entry for a username, or a null object when there is none.
// indexOut (optional) receives its position in the users array.
static JsonObject findRosterUser(JsonArray users, const String& username, size_t* indexOut = nullptr) {
  size_t index = 0;
  for (JsonObject user : users) {
    const char* name = user["username"] | "";
    if (username == name) {
      if (indexOut) *indexOut = index;
      return user;
    }
    ++index;
  }
  return JsonObject();
}

// Publishes pending_users.json from its parsed array under the caller's lock:
// an empty list removes the file, anything else goes through writeTextAtomic.
static bool writePendingUsers(JsonDocument& doc, const char* tag, String& errorOut) {
  JsonArray list = doc.as<JsonArray>();
  if (!list || list.size() == 0) {
    if (VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth(tag)) &&
        !VFS::removeGuarded(PENDING_USERS_FILE, VFS::systemAuth(tag))) {
      errorOut = "Could not update pending list";
      return false;
    }
    return true;
  }
  String json;
  const size_t expected = doc.overflowed() ? 0 : measureJson(doc);
  if (expected == 0 || !json.reserve(expected) || serializeJson(doc, json) != expected ||
      json.length() != expected || !writeTextAtomic(PENDING_USERS_FILE, json)) {
    errorOut = "Could not update pending list";
    return false;
  }
  return true;
}

// ============================================================================
// Account creation owner
// ============================================================================
// See createAccount in System_User.h. The credential is stored before the
// roster entry is added because users.json is the commit token: a power cut
// between the two leaves a settings file nobody references, never an account
// nobody can log into.
struct CreateAccountContext {
  const NewAccount* account;
  String username;
  uint32_t id;
};

// The highest id this roster has issued: its largest account id, or the one
// before its nextId counter when that is larger.
static uint32_t rosterIssuedIdHighWater(JsonDocument& doc, JsonArray users) {
  uint32_t highest = 0;
  for (JsonObject user : users) {
    const uint32_t id = (uint32_t)(user["id"] | 0);
    if (id > highest) highest = id;
  }
  const uint32_t nextId = (uint32_t)(doc["nextId"] | 0);
  if (nextId > 0 && nextId - 1 > highest) highest = nextId - 1;
  return highest;
}

static UsersJsonMutation createAccountMutation(JsonDocument& doc, JsonArray users, void* opaque,
                                               String& errorOut) {
  CreateAccountContext* ctx = static_cast<CreateAccountContext*>(opaque);
  uint32_t highestId = 0;
  for (JsonObject user : users) {
    const char* existing = user["username"] | "";
    if (ctx->username == existing) {
      errorOut = "Username already exists";
      return UsersJsonMutation::Failed;
    }
    const uint32_t id = (uint32_t)(user["id"] | 0);
    if (id > highestId) highestId = id;
  }
  // nextId is advisory: a roster without it, or one restored from a backup
  // whose counter lags, must still never reuse an id (the settings file of the
  // account holding that id would be overwritten with the new credential).
  uint32_t newId = (uint32_t)(doc["nextId"] | 2);
  if (newId == 0 || newId <= highestId) newId = highestId + 1;
  // Ids are never reissued, not even after a factory reset or a restore has
  // removed every account that held them: per-id data (transcripts under
  // /stt/u<id>, user_settings/<id>.json) can outlive the roster. The mark is
  // raised before anything is written, so a failed creation only skips an id.
  uint32_t highWater = 0;
  if (!bootStateGetAccountIdHighWater(highWater)) {
    errorOut = "Could not read the account id record";
    return UsersJsonMutation::Failed;
  }
  if (newId <= highWater) newId = highWater + 1;
  if (newId == 0 || !bootStateRaiseAccountIdHighWater(newId)) {
    errorOut = "Could not reserve an account id";
    return UsersJsonMutation::Failed;
  }

  {
    PSRAM_JSON_DOC(defaults);
    defaults["theme"] = "light";
    defaults["password"] = ctx->account->passwordHash;
    defaults["mustChangePassword"] = ctx->account->mustChangePassword;
    if (defaults.overflowed() || !saveUserSettings(newId, defaults)) {
      errorOut = "Failed to write user settings";
      return UsersJsonMutation::Failed;
    }
  }
  ctx->id = newId;

  JsonObject newUser = users.add<JsonObject>();
  newUser["id"] = newId;
  newUser["username"] = ctx->username;
  // Unrecognised values collapse to "user" rather than being written through:
  // an unknown string would otherwise look like a real tier in users.json
  // while rating as kRoleRankUser everywhere else.
  newUser["role"] = isKnownUserRole(ctx->account->role) ? ctx->account->role.c_str() : "user";
  newUser["createdAt"] = (const char*)nullptr;  // resolved lazily via boot anchor
  newUser["createdBy"] = ctx->account->createdBy.length() ? ctx->account->createdBy.c_str() : "admin";
  newUser["createdAtSource"] = "pending";
  newUser["createdMs"] = millis();
  newUser["ntpAnchorId"] = gNTPAnchorId;
  newUser["bootCount"] = gBootCounter;
  doc["nextId"] = newId + 1;
  return UsersJsonMutation::Changed;
}

bool createAccount(const NewAccount& account, uint32_t& idOut, String& errorOut) {
  idOut = 0;
  errorOut = "";
  if (!filesystemReady) {
    errorOut = "LittleFS not ready";
    return false;
  }
  CreateAccountContext ctx{&account, account.username, 0};
  ctx.username.trim();
  if (!isValidPublicUsername(ctx.username, &errorOut)) return false;
  if (account.passwordHash.length() == 0) {
    errorOut = "Invalid password";
    return false;
  }
  // Guests cannot change their own password on any interface, so a guest
  // created with a forced change could never get past it.
  if (account.mustChangePassword && account.role == "guest") {
    errorOut = "A guest account cannot be required to change its password";
    return false;
  }
  // One lock from the roster read to the publication, and over the removal of
  // a credential whose publication failed.
  FsLockGuard guard("user.create");
  if (!mutateUsersJson("user.create", createAccountMutation, &ctx, errorOut)) {
    if (ctx.id != 0) {
      (void)VFS::removeGuarded(getUserSettingsPath(ctx.id).c_str(), VFS::systemAuth("user.create"));
    }
    return false;
  }
  idOut = ctx.id;
  DEBUG_USERSF("[users] created account '%s' id=%u role=%s", ctx.username.c_str(), (unsigned)ctx.id,
               account.role.c_str());
  return true;
}

static UsersJsonMutation recordIssuedIdsMutation(JsonDocument& doc, JsonArray users, void* opaque,
                                                 String& errorOut) {
  (void)opaque;
  if (!bootStateRaiseAccountIdHighWater(rosterIssuedIdHighWater(doc, users))) {
    errorOut = "Could not record the account id mark";
    return UsersJsonMutation::Failed;
  }
  return UsersJsonMutation::Unchanged;
}

bool recordAccountIdHighWater(String& errorOut) {
  errorOut = "";
  return mutateUsersJson("user.idmark", recordIssuedIdsMutation, nullptr, errorOut);
}

bool adminCreateUser(const String& username, const String& plainPassword, bool mustChangeOnLogin,
                     const String& createdBy, String& errorOut, const String& role) {
  errorOut = "";
  if (!filesystemReady) {
    errorOut = "LittleFS not ready";
    return false;
  }
  String u = username;
  u.trim();
  if (!isValidPublicUsername(u, &errorOut)) {
    return false;
  }
  if (!isValidPublicPassword(plainPassword, &errorOut)) {
    return false;
  }

  // Reject if already pending registration
  if (VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.admin.create"))) {
    File pf = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.admin.create"));
    if (pf) {
      PSRAM_JSON_DOC(pdoc);
      if (!deserializeJson(pdoc, pf)) {
        JsonArray parr = pdoc.as<JsonArray>();
        if (parr) {
          for (JsonObject pu : parr) {
            const char* pun = pu["username"] | "";
            if (pun && u == pun) {
              pf.close();
              errorOut = "Username already pending approval";
              return false;
            }
          }
        }
      }
      pf.close();
    }
  }

  NewAccount account;
  account.username = u;
  account.passwordHash = hashUserPassword(plainPassword);
  account.role = role;
  account.createdBy = createdBy.length() ? createdBy : String("admin");
  account.mustChangePassword = mustChangeOnLogin;
  uint32_t createdUserId = 0;
  if (!createAccount(account, createdUserId, errorOut)) return false;

  // Identity topology changed (a new account is now usable). Invalidate
  // any auth-dependent caches — see System_AuthIdentity.h for the protocol.
  bumpIdentityGeneration("user.add");
  systemEventPost(SYSEVT_USER_ADDED, u.c_str(), createdBy.length() ? createdBy.c_str() : nullptr);
  DEBUG_USERSF("[users] admin created user '%s' id=%u mustChange=%d", u.c_str(), (unsigned)createdUserId,
               mustChangeOnLogin ? 1 : 0);
  return true;
}

// Update a user's gamepad pattern password in per-user settings file
bool setUserGamepadPassword(const String& username, const String& newPatternRaw) {
  if (!filesystemReady || username.length() == 0 || newPatternRaw.length() == 0) return false;
  
  // Get user ID from username
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  
  // Hash the pattern (same as text password)
  String hashed = hashUserPassword(newPatternRaw);
  if (hashed.length() == 0) return false;
  
  // Mutate existing user settings transactionally. See the note in
  // setUserPassword: a dropped
  // return value here destroys the account's "password" key, because a failed
  // parse leaves the document cleared and the save below writes only this key.
  UserGamepadPasswordMutationContext mutation{&hashed};
  const UserSettingsTxnStatus status =
      runUserSettingsTransaction(userId, applyUserGamepadPasswordMutation, &mutation);
  if (status == UserSettingsTxnStatus::LoadFailed) {
    ERROR_STORAGEF("[user] gamepad-pattern change REFUSED: user_settings/%lu.json is "
                   "unreadable - refusing to overwrite it (the stored login hash "
                   "would be lost)",
                   (unsigned long)userId);
    systemEventPost(SYSEVT_CONFIG_FILE_CORRUPT, "user_settings",
                    "unreadable at gamepad-pattern change");
    return false;
  }
  return status == UserSettingsTxnStatus::Committed;
}

// Check if a user has a gamepad password set (in per-user settings file)
bool hasUserGamepadPassword(const String& username) {
  if (!filesystemReady || username.length() == 0) return false;
  
  // Get user ID from username
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId) || userId == 0) return false;
  
  // Load user settings
  PSRAM_JSON_DOC(settings);
  if (!loadUserSettings(userId, settings)) return false;
  
  // Check if gamepad_password field exists and is non-empty
  const char* gamepadPass = settings["gamepad_password"];
  return (gamepadPass && strlen(gamepadPass) > 0);
}

// ============================================================================
// User Account Ban
// ============================================================================

// Returns true if the given username has "banned": true in users.json.
bool isUserBanned(const String& username) {
  if (!filesystemReady || username.length() == 0) return false;
  FsLockGuard guard("users.is_banned");
  // See getUserRole: the exists probe was a duplicate path resolution only.
  File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.isBanned"));
  if (!f) return false;
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  for (JsonObject u : doc["users"].as<JsonArray>()) {
    const char* uname = u["username"] | "";
    if (username == uname) return u["banned"] | false;
  }
  return false;
}

struct BanMutationContext {
  const String* username;
  bool ban;
  const String* reason;
};

static UsersJsonMutation banMutation(JsonDocument& doc, JsonArray users, void* opaque, String& errorOut) {
  (void)doc;
  const BanMutationContext* ctx = static_cast<const BanMutationContext*>(opaque);
  JsonObject u = findRosterUser(users, *ctx->username);
  if (u.isNull()) {
    errorOut = "User not found";
    return UsersJsonMutation::Failed;
  }
  if ((uint32_t)(u["id"] | 0) == 1) {
    errorOut = "Cannot ban the primary admin account";
    return UsersJsonMutation::Failed;
  }
  if (ctx->ban) {
    u["banned"] = true;
    if (ctx->reason->length()) u["banReason"] = *ctx->reason;
    else                       u.remove("banReason");
  } else {
    u.remove("banned");
    u.remove("banReason");
  }
  return UsersJsonMutation::Changed;
}

// Sets (or clears) the "banned" flag on a user entry in users.json. Also stores
// the optional reason, which may contain any byte: the roster is edited as a
// parsed document, never as text. Kicks active sessions when banning. Returns
// false with errorOut set on failure.
static bool setUserBanInternal(const String& username, bool ban, const String& reason, String& errorOut) {
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  if (!filesystemReady) {
    errorOut = "Filesystem not ready";
    return false;
  }
  BanMutationContext ctx{&username, ban, &reason};
  if (!mutateUsersJson(ban ? "user.ban" : "user.unban", banMutation, &ctx, errorOut)) return false;

  // Account suspension changes whether an identity may publish device
  // readiness. Bump before revoking sessions so a concurrently completing
  // UART login either sees the new account state or disables CM5 presence.
  bumpIdentityGeneration(ban ? "user.ban" : "user.unban");

  if (ban) {
    // Revoke serial transport session if active for this user
    String serialUser;
    bool serialAuthed = false;
    (void)serialTransportSessionSnapshot(serialUser, serialAuthed);
    if (serialAuthed && serialUser.equalsIgnoreCase(username)) {
      serialTransportSessionCleared();
    }
    // Revoke UART host-link session if active for this user
    (void)uartLinkSessionClearIfUser(username);
    // Revoke OLED/local display authority from its synchronized shadow. The
    // legacy globals are updated later by the OLED owner and may intentionally
    // still show the prior frame here.
    String displayUser;
    bool displayAuthed = false;
    (void)localDisplayTransportSessionSnapshot(displayUser, displayAuthed);
    if (displayAuthed && displayUser.equalsIgnoreCase(username)) {
      localDisplayTransportSessionCleared();
    }
    // Clear G2 pair-time authority. Reconnect may restore transport only;
    // authority remains absent until a new authenticated pairing gesture.
    (void)g2PairedUserClearIfMatches(username);
    // Revoke Bluetooth sessions for this user
    (void)bleRevokeUserSessions(username);
#if ENABLE_HTTP_SERVER
    (void)webRevokeSessionsForUser(
        username,
        "Your account has been suspended by an administrator.");
#endif
    systemEventPost(SYSEVT_USER_BANNED, username.c_str());
  }

  DEBUG_USERSF("[users] %s user ban for '%s'", ban ? "set" : "cleared", username.c_str());
  return true;
}

struct LastSeenMutationContext {
  const String* username;
  const char* isoTimestamp;
};

static UsersJsonMutation lastSeenMutation(JsonDocument& doc, JsonArray users, void* opaque, String& errorOut) {
  (void)doc;
  (void)errorOut;
  const LastSeenMutationContext* ctx = static_cast<const LastSeenMutationContext*>(opaque);
  JsonObject u = findRosterUser(users, *ctx->username);
  if (u.isNull()) return UsersJsonMutation::Unchanged;
  u.remove("lastSeenSec");  // Remove legacy field if present
  u["lastSeen"] = ctx->isoTimestamp;
  return UsersJsonMutation::Changed;
}

// Updates the "lastSeen" field in users.json for the given username.
// Stores an ISO-8601 timestamp (same format as createdAt).
// Only writes if the system clock appears valid (epoch > Jan 1, 2021).
void updateUserLastSeen(const String& username) {
  if (username.length() == 0 || !filesystemReady) return;
  time_t now = Clock::epochSeconds();
  if (!Clock::isValidEpoch(now)) return;  // Clock not set yet — skip write

  char isoTimestamp[25];
  struct tm tminfo;
  if (!gmtime_r(&now, &tminfo)) return;
  strftime(isoTimestamp, sizeof(isoTimestamp), "%Y-%m-%dT%H:%M:%SZ", &tminfo);

  // This runs on every login: the owner's atomic write keeps a power cut here
  // from shredding the auth database. A missing account changes nothing.
  LastSeenMutationContext ctx{&username, isoTimestamp};
  String error;
  if (!mutateUsersJson("user.lastSeen", lastSeenMutation, &ctx, error)) return;
  DEBUG_USERSF("[users] lastSeen updated for '%s' -> %s", username.c_str(), isoTimestamp);
}

// Validate a username/password against per-user settings file
// Checks both 'password' (text) and 'gamepad_password' (pattern) fields
bool isValidUser(const String& u, const String& p) {
  AuthPerfScope _perf("isValidUser(TOTAL)");
  if (!filesystemReady) return false;
  if (u.length() == 0 || p.length() == 0) return false;
  // A reserved name is never a loginable account, whatever users.json holds
  // (an entry could arrive from a sync receiver or a hand edit).
  if (isReservedUsername(u)) return false;

  // Reject banned accounts before touching credentials
  if (isUserBanned(u)) return false;

  // Get user ID from username (verifies user exists in users.json)
  uint32_t userId = 0;
  if (!getUserIdByUsername(u, userId) || userId == 0) return false;
  
  // Load user settings containing passwords
  PSRAM_JSON_DOC(settings);
  if (!loadUserSettings(userId, settings)) return false;
  
  // Derive the input hash ONCE and compare it against both stored credentials.
  //
  // hashUserPassword() is a pure function of the password: the salt is the
  // device key and the iteration count is a hardcoded 10000, so hashing the
  // same input twice produces the same string. Calling verifyUserPassword()
  // per stored credential therefore ran a second 10,000-iteration PBKDF2 for
  // an answer it already had — paid on every login where the text password
  // was not the match, i.e. every wrong password and every gamepad-pattern
  // login. Same comparisons, same values, half the derivations.
  //
  // Side benefit: a text-password miss no longer costs measurably more than a
  // hit, which removes a timing signal for "the text password was wrong".
  const char* textPass    = settings["password"];
  const char* gamepadPass = settings["gamepad_password"];

  // Preserve the old short-circuit: verifyUserPassword() rejected a
  // non-PBKDF2 stored hash *before* hashing, so a roster with no PBKDF2
  // credential must still cost zero derivations.
  const bool textIsPbkdf2    = textPass    && strncmp(textPass,    "PBKDF2:", 7) == 0;
  const bool gamepadIsPbkdf2 = gamepadPass && strncmp(gamepadPass, "PBKDF2:", 7) == 0;
  if (!textIsPbkdf2 && !gamepadIsPbkdf2) return false;

  const String inputHash = hashUserPassword(p);
  if (inputHash.length() == 0) return false;  // derivation failed — fail closed

  if (textIsPbkdf2 && inputHash == textPass) {
    return true;
  }

  if (gamepadIsPbkdf2 && inputHash == gamepadPass) {
    return true;
  }
  
  return false;
}

bool getUserIdByUsername(const String& username, uint32_t& outUserId) {
  AuthPerfScope _perf("getUserIdByUsername");
  outUserId = 0;
  if (!filesystemReady) return false;
  if (username.length() == 0) return false;
  // Reserved names never resolve to an account, so nothing keyed by account
  // id (credentials, per-user settings, private data) can be reached by one.
  if (isReservedUsername(username)) return false;

  {
    FsLockGuard guard("users.get_id");
    // No existsGuarded probe: openGuarded already fails closed on a missing
    // file and is still the permission check. The probe only bought a second
    // resolution of the same 3-level path.
    File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.getId"));
    if (!f) return false;

    PSRAM_JSON_DOC(doc);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      // Credential DB exists but is unparseable — auth cannot resolve anyone.
      systemEventPost(SYSEVT_AUTH_DB_FAULT, "users.json parse failed", err.c_str());
      return false;
    }

    JsonArray users = doc["users"].as<JsonArray>();
    if (!users) return false;

    for (JsonObject uObj : users) {
      const char* uname = uObj["username"] | "";
      if (username == uname) {
        outUserId = (uint32_t)(uObj["id"] | 0);
        return outUserId > 0;
      }
    }
  }

  return false;
}


// Get user role from users.json
bool getUserRole(const String& username, String& outRole) {
  outRole = "";
  if (!filesystemReady) return false;
  if (username.length() == 0) return false;

  FsLockGuard guard("users.get_role");
  // openGuarded fails closed on a missing file and is the permission check;
  // the old existsGuarded probe just resolved this 3-level path a second time.
  File f = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.getRole"));
  if (!f) return false;

  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  JsonArray users = doc["users"].as<JsonArray>();
  if (!users) return false;

  for (JsonObject uObj : users) {
    const char* uname = uObj["username"] | "";
    if (username == uname) {
      outRole = String(uObj["role"] | "user");
      return true;
    }
  }

  return false;
}

// Effective privilege rank for a username. Bond/owner fallbacks live in
// isSuperAdminUser/isAdminUser — use those first so a restored-backup owner
// rates the same whether you ask about the caller or the target. Guest is
// only reachable when those predicates are false and users.json says so.
int userAccountRank(const String& username) {
  AuthPerfScope _perf("userAccountRank");
  // ONE read of users.json, three parsers over the same bytes.
  //
  // This used to call isSuperAdminUser, then isAdminUser, then getUserRole —
  // three independent opens of the same file to produce one integer, ~20 ms
  // each (measured). streamBeginHtml calls this on every authenticated page
  // load, and separately calls isAdminUser again beside it.
  //
  // The three predicates are NOT collapsed into one rule: each keeps its own
  // parser and its own first-user fallback, which genuinely disagree (see the
  // note above adminFromRosterJson). The guest branch uses
  // legacyRoleFromRosterJson — getUserRole's parser, NOT roleFromRosterJson;
  // they differ on "banned" and on case, and swapping them changes an account's
  // rank. Evaluation order and short-circuiting match the original call
  // sequence; only the file read is shared.
  if (username.length() == 0) return kRoleRankUser;
#if ENABLE_BONDED_MODE
  // The bond identity is answered by bondIdentityLive() inside isSuperAdminUser /
  // isAdminUser (the calling task must be the live ESP-NOW bond) without
  // touching the filesystem, so it cannot use the shared read below. Run the
  // ORIGINAL three-call sequence verbatim for it — including
  // the getUserRole guest branch, which the old code reached and which an early
  // `return kRoleRankUser` here would have skipped. One extra read, only ever
  // for this one reserved name.
  if (username == kBondAdminUser) {
    if (isSuperAdminUser(username)) return kRoleRankSuperAdmin;
    if (isAdminUser(username))      return kRoleRankAdmin;
    String bondRole;
    if (getUserRole(username, bondRole) && bondRole == "guest") return kRoleRankGuest;
    return kRoleRankUser;
  }
#endif
  if (!filesystemReady) return kRoleRankUser;
  FsLockGuard guard("users.accountRank");
  String json;
  if (!readRosterJson("user.accountRank", json)) return kRoleRankUser;

  if (superFromRosterJson(json, username)) return kRoleRankSuperAdmin;
  if (adminFromRosterJson(json, username)) return kRoleRankAdmin;
  String role;
  if (legacyRoleFromRosterJson(json, username, role) && role == "guest") return kRoleRankGuest;
  return kRoleRankUser;
}

bool isGuestUser(const String& who) {
  return who.length() > 0 && userAccountRank(who) == kRoleRankGuest;
}

// findSessionIndexBySID moved to web_server.cpp

// ============================================================================
// User Management Internal Functions
// ============================================================================

bool approvePendingUserInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] approve internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  // One lock from the pending-list read through the account publication to
  // the pending-list update: a ban or role change committing on another task
  // meanwhile is neither lost nor interleaved, and the request that holds the
  // only copy of the credential hash is removed only once the account exists.
  FsLockGuard guard("user.approve");

  if (!VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.approve"))) {
    errorOut = "User not found in pending list";
    return false;
  }
  File file = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.approve"));
  if (!file) {
    errorOut = "Could not read pending list";
    return false;
  }
  PSRAM_JSON_DOC(pending);
  const DeserializationError error = deserializeJson(pending, file);
  file.close();
  if (error) {
    errorOut = "Malformed pending_users.json";
    return false;
  }

  JsonArray pendingArray = pending.as<JsonArray>();
  size_t approvedIndex = 0;
  JsonObject request = findRosterUser(pendingArray, username, &approvedIndex);
  if (request.isNull()) {
    errorOut = "User not found in pending list";
    return false;
  }

  // Record WHO approved this pending request (provenance), distinct from a
  // first-setup owner admin. Falls back to "admin" if there is no auth context.
  char approvedByBuf[48];
  { String ap = currentAuthContext().user;
    snprintf(approvedByBuf, sizeof(approvedByBuf), "approved:%s", ap.length() ? ap.c_str() : "admin"); }

  NewAccount account;
  account.username = username;
  account.passwordHash = String(request["password"] | "");  // stored hashed at request time
  account.role = "user";
  account.createdBy = approvedByBuf;
  uint32_t createdUserId = 0;
  if (!createAccount(account, createdUserId, errorOut)) {
    // Nothing was published and the request is still pending; the admin sees
    // the reason and can retry or deny it.
    return false;
  }

  // Identity topology changed (pending account is now real and usable).
  // Note: userrequest/userdeny do NOT bump — pending accounts can't
  // authenticate, so their existence/removal doesn't change "who can read
  // what." Approval is when the account first matters. See
  // System_AuthIdentity.h for the full bump-site list.
  bumpIdentityGeneration("user.approve");
  // [EVENT] Covers BOTH the CLI and the web approve path (the web POST
  // bypasses the command audit, so this is the single source of truth).
  logSystemEvent("USERS", "user '%s' approved (id=%lu)", username.c_str(), (unsigned long)createdUserId);
  systemEventPost(SYSEVT_USER_APPROVED, username.c_str());
  BROADCAST_PRINTF("[admin] Approved user: %s", username.c_str());

  // The account is live; retire its request. A failure here is reported as
  // such rather than hidden: the entry stays until a later approve (refused as
  // a duplicate) or deny clears it.
  pendingArray.remove(approvedIndex);
  if (!writePendingUsers(pending, "user.approve", errorOut)) {
    errorOut = "User approved, but the pending list could not be updated";
    return false;
  }

  // If NTP already synced, resolve the creation timestamp immediately
  if (time(nullptr) > 0) {
    resolvePendingUserCreationTimes();
  }

  return true;
}

bool denyPendingUserInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] deny internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  FsLockGuard guard("user.deny");
  if (!VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.deny"))) {
    errorOut = "User not found in pending list";
    return false;
  }
  File file = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.deny"));
  if (!file) {
    errorOut = "Could not read pending list";
    return false;
  }
  PSRAM_JSON_DOC(pending);
  const DeserializationError error = deserializeJson(pending, file);
  file.close();
  if (error) {
    errorOut = "Malformed pending_users.json";
    return false;
  }

  JsonArray pendingArray = pending.as<JsonArray>();
  size_t deniedIndex = 0;
  if (findRosterUser(pendingArray, username, &deniedIndex).isNull()) {
    errorOut = "User not found in pending list";
    return false;
  }
  pendingArray.remove(deniedIndex);
  if (!writePendingUsers(pending, "user.deny", errorOut)) return false;

  systemEventPost(SYSEVT_USER_REJECTED, username.c_str());
  return true;
}

// The public intake rules for a self-registration request, without side
// effects: Submitted when both fields pass.
static AccountRequestResult checkAccountRequestFields(const String& username, const String& password) {
  if (!isValidPublicUsername(username, nullptr)) return AccountRequestResult::InvalidUsername;
  if (!isValidPublicPassword(password, nullptr)) {
    return (password.length() < kPublicPasswordMinLen) ? AccountRequestResult::PasswordTooShort
                                                       : AccountRequestResult::PasswordTooLong;
  }
  return AccountRequestResult::Submitted;
}

// See System_User.h.
AccountRequestResult submitAccountRequest(const String& usernameInput, const String& password) {
  String username = usernameInput;
  username.trim();
  const AccountRequestResult fields = checkAccountRequestFields(username, password);
  if (fields != AccountRequestResult::Submitted) return fields;
  if (!filesystemReady) return AccountRequestResult::NotReady;

  DEBUG_CMD_FLOWF("[users] Adding user to pending_users.json, filesystemReady=%d", filesystemReady ? 1 : 0);

  // Held across the read-modify-write so an approve or deny committing on
  // another task meanwhile is not overwritten by this stale copy.
  FsLockGuard guard("user.request");
  PSRAM_JSON_DOC(doc);
  if (VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.request"))) {
    String json;
    if (!readText(PENDING_USERS_FILE, json)) {
      DEBUG_CMD_FLOWF("[users] ERROR: Failed to read existing /system/pending_users.json");
      return AccountRequestResult::ReadFailed;
    }
    if (json.length() >= 2 && json.startsWith("[")) {
      if (deserializeJson(doc, json)) {
        doc.clear();
        doc.to<JsonArray>();
      }
    } else {
      doc.to<JsonArray>();
    }
  } else {
    doc.to<JsonArray>();
  }

  JsonArray arr = doc.as<JsonArray>();
  if (!arr) {
    doc.clear();
    arr = doc.to<JsonArray>();
  }
  // Cap spam even if rate-limit is bypassed (CLI / other transports).
  if (arr.size() >= 32) {
    return AccountRequestResult::QueueFull;
  }
  for (JsonObject pu : arr) {
    const char* existing = pu["username"] | "";
    if (existing[0] && strcasecmp(existing, username.c_str()) == 0) {
      return AccountRequestResult::AlreadyPending;
    }
  }

  JsonObject entry = arr.add<JsonObject>();
  entry["username"] = username;
  entry["password"] = hashUserPassword(password);
  entry["timestamp"] = (unsigned long)millis();

  String writeError;
  if (!writePendingUsers(doc, "user.request", writeError)) {
    ERROR_STORAGEF("writeText failed when writing pending_users.json");
    broadcastOutput("[users] ERROR: writeText failed for /system/pending_users.json");
    return AccountRequestResult::WriteFailed;
  }

  DEBUG_CMD_FLOWF("[users] request username=%s", username.c_str());
  BROADCAST_PRINTF("[register] New user request: %s", username.c_str());
  systemEventPost(SYSEVT_USER_REQUEST, username.c_str());
  return AccountRequestResult::Submitted;
}

// Shared authorization for user-mutation commands. Enforces the two rank rules:
//   (1) a caller may not act on a target of HIGHER privilege than itself
//       (a regular admin cannot demote/ban/delete a super-admin);
//   (2) a caller may not grant a role above its own rank
//       (only a super-admin can create or raise another super-admin).
// newRoleRank is the rank being granted, or kRoleRankNoGrant for delete/ban.
// Caller identity is the per-task exec identity (currentExecUser()); a live
// bond session resolves to kBondAdminUser which isSuperAdminUser() rates super.
// Both caller AND target rank come from userAccountRank() so the owner/bond
// fallbacks and guest detection apply uniformly — otherwise a restored-backup
// owner can read as super for the caller but only admin for the target. The
// founder (id==1) also has hard protection inside the mutators as a second layer.
static bool userMutationAllowed(const String& targetUser, int newRoleRank, String& errOut) {
  const String caller = currentExecUser();
  const int callerRank = userAccountRank(caller);
  const int targetRank = userAccountRank(targetUser);
  if (targetRank > callerRank) {
    errOut = "Cannot modify a higher-privileged account";
    return false;
  }
  if (newRoleRank != kRoleRankNoGrant && newRoleRank > callerRank) {
    errOut = "Cannot grant a role above your own";
    return false;
  }
  return true;
}

// Set a user's role to an arbitrary value ("guest"/"user"/"admin"/"superadmin")
// in users.json. Pure data mutation: founder (id==1) protection, role replace
// or insert, and an identity-generation bump. Direction-specific behavior
// (which event to post, session revocation on downgrade, user-facing messaging)
// lives in the promote/demote handlers. Authorization is the caller's
// responsibility via userMutationAllowed() BEFORE invoking this.
struct RoleMutationContext {
  const String* username;
  const char* role;
};

static UsersJsonMutation roleMutation(JsonDocument& doc, JsonArray users, void* opaque, String& errorOut) {
  (void)doc;
  const RoleMutationContext* ctx = static_cast<const RoleMutationContext*>(opaque);
  JsonObject u = findRosterUser(users, *ctx->username);
  if (u.isNull()) {
    errorOut = "User not found";
    return UsersJsonMutation::Failed;
  }
  if ((uint32_t)(u["id"] | 0) == 1) {
    errorOut = "Cannot modify the first admin account";
    return UsersJsonMutation::Failed;
  }
  u["role"] = ctx->role;  // replaces the field, or adds it to a role-less legacy entry
  return UsersJsonMutation::Changed;
}

static bool setUserRoleInternal(const String& username, const char* newRole, String& errorOut) {
  DEBUG_USERSF("[users] setrole internal username=%s role=%s", username.c_str(), newRole);
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  RoleMutationContext ctx{&username, newRole};
  if (!mutateUsersJson("user.setrole", roleMutation, &ctx, errorOut)) return false;
  // Identity topology changed (role differs). Invalidate auth-dependent caches
  // — see System_AuthIdentity.h. Event posting, session revocation and messaging
  // are the handlers' job (they differ between promote and demote).
  bumpIdentityGeneration("user.setrole");
  return true;
}

struct DeleteMutationContext {
  const String* username;
  uint32_t id;  // the removed entry's id, for its settings file
};

static UsersJsonMutation deleteMutation(JsonDocument& doc, JsonArray users, void* opaque, String& errorOut) {
  (void)doc;
  DeleteMutationContext* ctx = static_cast<DeleteMutationContext*>(opaque);
  size_t index = 0;
  JsonObject u = findRosterUser(users, *ctx->username, &index);
  if (u.isNull()) {
    errorOut = "User not found";
    return UsersJsonMutation::Failed;
  }
  ctx->id = (uint32_t)(u["id"] | 0);
  if (ctx->id == 1) {
    errorOut = "Cannot delete the first admin account";
    return UsersJsonMutation::Failed;
  }
  users.remove(index);
  return UsersJsonMutation::Changed;
}

// Delete an existing user from users.json and their settings file
static bool deleteUserInternal(const String& username, String& errorOut) {
  DEBUG_USERSF("[users] delete internal username=%s", username.c_str());
  if (username.length() == 0) {
    errorOut = "Username required";
    return false;
  }
  DeleteMutationContext ctx{&username, 0};
  if (!mutateUsersJson("user.delete", deleteMutation, &ctx, errorOut)) return false;
  const uint32_t userId = ctx.id;
  // Identity topology changed (user removed — their permission set
  // evaporates). Invalidate any auth-dependent caches — see
  // System_AuthIdentity.h for the protocol.
  bumpIdentityGeneration("user.delete");
  systemEventPost(SYSEVT_USER_DELETED, username.c_str());
  // Forcibly log out every active session for the deleted user across
  // all transports (web/serial/oled/bluetooth). No exception filter:
  // the account is gone, so even if the calling admin is themselves
  // the user being deleted, kick them too. revokeUserSessions returns
  // the count and emits [SESSION-REVOKE] to the audit log; we just
  // need a per-transport user-facing notice on serial in addition.
  String deletedSerialUser;
  bool deletedSerialAuthed = false;
  (void)serialTransportSessionSnapshot(deletedSerialUser,
                                       deletedSerialAuthed);
  if (deletedSerialAuthed &&
      deletedSerialUser.equalsIgnoreCase(username)) {
    broadcastOutput("[serial] Your account has been deleted. You have been logged out.");
  }
  int revokedSessions =
      revokeUserSessions(username, "Account deleted by administrator");

  // Delete user settings file (contains password and preferences)
  if (userId > 0) {
    String settingsPath = getUserSettingsPath(userId);
    if (VFS::existsGuarded(settingsPath.c_str(), VFS::systemAuth("user.settings.remove"))) {
      VFS::removeGuarded(settingsPath.c_str(), VFS::systemAuth("user.settings.remove"));
      DEBUG_USERSF("[users] Deleted settings file for userId=%u", (unsigned)userId);
    }
  }

  if (revokedSessions > 0) {
    BROADCAST_PRINTF("[admin] Deleted user: %s (%d active session(s) terminated)", username.c_str(), revokedSessions);
  } else {
    BROADCAST_PRINTF("[admin] Deleted user: %s", username.c_str());
  }
  return true;
}

// ============================================================================
// User Command Handlers
// ============================================================================

const char* cmd_user_approve(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
  username.trim();
  DEBUG_USERSF("[users] CLI approve username=%s", username.c_str());
  String err;
  if (!approvePendingUserInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Approved";
  snprintf(getDebugBuffer(), 1024, "Approved user '%s'", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_deny(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
  username.trim();
  DEBUG_USERSF("[users] CLI deny username=%s", username.c_str());
  String err;
  if (!denyPendingUserInternal(username, err)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Error: Denied";
  snprintf(getDebugBuffer(), 1024, "Denied user '%s'", username.c_str());
  return getDebugBuffer();
}

const char* cmd_user_promote(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: userpromote <username> [user|admin|superadmin]";
  String username = a.arg(0);
  String role = a.has(1) ? a.arg(1) : String("admin");
  role.toLowerCase();
  // Promote steps up: guest→user, user→admin, admin→superadmin. "guest" is
  // demote-only (use userdemote).
  if (role != "user" && role != "admin" && role != "superadmin")
    return "Error: role must be 'user', 'admin', or 'superadmin'";
  DEBUG_USERSF("[users] CLI promote username=%s role=%s", username.c_str(), role.c_str());
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  String err;
  // Rank check first: a non-super can't grant super, and no one can act on a
  // target above their own rank.
  if (!userMutationAllowed(username, userRoleRank(role), err)) {
    snprintf(buf, 1024, "Error: %s", err.c_str());
    return buf;
  }
  if (!setUserRoleInternal(username, role.c_str(), err)) {
    snprintf(buf, 1024, "Error: %s", err.c_str());
    return buf;
  }
  systemEventPost(SYSEVT_USER_PROMOTED, username.c_str());
  String promotedSerialUser;
  bool promotedSerialAuthed = false;
  (void)serialTransportSessionSnapshot(promotedSerialUser,
                                       promotedSerialAuthed);
  if (promotedSerialAuthed && promotedSerialUser == username)
    broadcastOutput("[serial] Your privileges have been updated");
  BROADCAST_PRINTF("[admin] Set user '%s' role to %s", username.c_str(), role.c_str());
  snprintf(buf, 1024, "Promoted user '%s' to %s", username.c_str(), role.c_str());
  return buf;
}

const char* cmd_user_demote(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: userdemote <username> [admin|user|guest]";
  String username = a.arg(0);
  String role = a.has(1) ? a.arg(1) : String("user");
  role.toLowerCase();
  if (role != "admin" && role != "user" && role != "guest")
    return "Error: role must be 'admin', 'user', or 'guest'";
  DEBUG_USERSF("[users] CLI demote username=%s role=%s", username.c_str(), role.c_str());
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  String err;
  // Rank check: a regular admin cannot step a super-admin down; only a super
  // can act on a super. Founder (id==1) is additionally protected in the writer.
  if (!userMutationAllowed(username, userRoleRank(role), err)) {
    snprintf(buf, 1024, "Error: %s", err.c_str());
    return buf;
  }
  if (!setUserRoleInternal(username, role.c_str(), err)) {
    snprintf(buf, 1024, "Error: %s", err.c_str());
    return buf;
  }
  systemEventPost(SYSEVT_USER_DEMOTED, username.c_str());
  // Force re-auth so a session that just lost privilege can't keep using it.
  String demotedSerialUser;
  bool demotedSerialAuthed = false;
  (void)serialTransportSessionSnapshot(demotedSerialUser,
                                       demotedSerialAuthed);
  if (demotedSerialAuthed && demotedSerialUser == username)
    broadcastOutput("[serial] Your privileges have been changed");
  revokeUserSessions(username, "Your privileges have changed. Please log in again.");
  BROADCAST_PRINTF("[admin] Set user '%s' role to %s", username.c_str(), role.c_str());
  snprintf(buf, 1024, "Set user '%s' to %s", username.c_str(), role.c_str());
  return buf;
}

// userdelete is two-step like filedelete: prompt + yes/no confirm via the
// CLIMode framework. Static state survives the gap between the prompting
// command and the user's yes/no reply (cmd_exec runs both, sequentially,
// with the framework holding sActiveMode in between).
static String s_pendingUserDeleteName;

static const char* doUserDelete(const String& username) {
  EXT_RAM_BSS_ATTR static char respBuf[160];
  if (!filesystemReady) return "Error: LittleFS not ready";

  DEBUG_USERSF("[users] CLI delete (confirmed) username=%s",
               username.c_str());
  // Re-check rank at confirm time (not just at prompt): close the window where
  // the target could have been elevated between the prompt and the 'yes'.
  {
    String merr;
    if (!userMutationAllowed(username, kRoleRankNoGrant, merr)) {
      snprintf(respBuf, sizeof(respBuf), "Error: %s", merr.c_str());
      return respBuf;
    }
  }
  String err;
  if (!deleteUserInternal(username, err)) {
    snprintf(respBuf, sizeof(respBuf), "Error: %s", err.c_str());
    return respBuf;
  }
  snprintf(respBuf, sizeof(respBuf), "Deleted user '%s'",
           username.c_str());
  return respBuf;
}

static const char* user_delete_confirmed(void* /*userData*/) {
  return doUserDelete(s_pendingUserDeleteName);
}

static const char* user_delete_cancelled(void* /*userData*/) {
  EXT_RAM_BSS_ATTR static char respBuf[120];
  snprintf(respBuf, sizeof(respBuf), "Cancelled. User '%s' not deleted.",
           s_pendingUserDeleteName.c_str());
  return respBuf;
}

static void user_delete_confirm_accepted(void* opaque) {
  const String* username = static_cast<const String*>(opaque);
  if (username) s_pendingUserDeleteName = *username;
}

const char* cmd_user_delete(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  String username = argsInput;
  username.trim();
  if (username.length() == 0) return "Error: invalid arguments — Usage: user delete <username> [confirm]";

  // One-shot bypass token: `userdelete <username> confirm` (or yes/--yes/-y)
  // deletes immediately, skipping the interactive prompt. This is the form the
  // tap UIs (OLED/G2 user manager) dispatch AFTER their own on-screen confirm —
  // the interactive cliRequestConfirm mode can't be answered from a tap.
  // Mirrors filedelete's one-shot token. The trailing token is the delimiter,
  // so it's stripped before the name is resolved.
  bool oneShot = false;
  {
    int sp = username.lastIndexOf(' ');
    if (sp > 0) {
      String tail = username.substring(sp + 1);
      tail.trim();
      if (tail.equalsIgnoreCase("confirm") || tail.equalsIgnoreCase("yes") ||
          tail == "--yes" || tail == "-y") {
        oneShot = true;
        username = username.substring(0, sp);
        username.trim();
      }
    }
  }
  if (username.length() == 0) return "Error: invalid arguments — Usage: user delete <username> [confirm]";

  // The tap UIs (OLED/G2 user manager) wrap the name in quotes so names with
  // spaces survive dispatch: `userdelete "b" confirm`. We hand-parse here
  // instead of routing through CommandArgs, so strip the surrounding quotes
  // ourselves — otherwise the literal quote chars leak into the name and no
  // stored user ever matches (useradd de-quotes via CommandArgs, so it stored
  // `b`, not `"b"`).
  if (username.length() >= 2 && username[0] == '"' && username[username.length() - 1] == '"') {
    username = username.substring(1, username.length() - 1);
    username.trim();
    if (username.length() == 0) return "Error: invalid arguments — Usage: user delete <username> [confirm]";
  }
  DEBUG_USERSF("[users] CLI delete (%s) username=%s", oneShot ? "one-shot" : "prompt", username.c_str());

  // Rank check up front (fail fast, before prompting): a regular admin cannot
  // delete a super-admin. Founder (id==1) is additionally protected in the
  // deleter itself.
  {
    String merr;
    if (!userMutationAllowed(username, kRoleRankNoGrant, merr)) {
      if (!ensureDebugBuffer()) return "Error: denied";
      snprintf(getDebugBuffer(), 1024, "Error: %s", merr.c_str());
      return getDebugBuffer();
    }
  }

  // One-shot: run the confirmed path now (it re-checks rank + deletes).
  if (oneShot) {
    return doUserDelete(username);
  }

  String prompt = "Confirm delete of user '" + username +
                  "'? All sessions for this user will be revoked.";
  // Originating command line stored for the resolution audit -- shows up
  // in [CMD] log as "userdelete bob (confirm: yes) -> Deleted user 'bob'"
  // (or "(confirm: no) -> Cancelled. User 'bob' not deleted." on cancel).
  String origCmd = "userdelete " + username;
  if (!cliRequestConfirm(prompt, origCmd, user_delete_confirmed,
                         user_delete_cancelled, nullptr,
                         user_delete_confirm_accepted, &username)) {
    return "Error: cannot request confirm (another interactive mode is active)";
  }
  return cliConfirmPromptResponse();
}

// Shared password-change implementation. Both the CLI command wrapper below
// and the web POST handler call this. Resolves the caller via the per-task
// TLS identity (ExecIdentityGuard installed upstream by executeCommand for
// command-pipeline callers, or by the web handler before invocation).
//
// Previously the body of cmd_user_changepassword hard-coded
// getTransportUser(SOURCE_LOCAL_DISPLAY) for user resolution, which only
// worked from the OLED. Every non-OLED transport got the OLED user's name
// back regardless of who actually called. Now currentExecUser() returns the
// right user for every transport because each dispatch path installs its own
// AuthContext into the TLS slot.
//
// The message goes into the caller's buffer, never the shared debug buffer:
// the web handler calls this on httpd while cmd_exec may be formatting
// another command's result there.
bool userChangePasswordCore(const String& currentPassword,
                            const String& newPassword,
                            const String& confirmPassword,
                            char* message, size_t messageSize) {
  char scratch[1];
  if (!message || messageSize == 0) {
    message = scratch;
    messageSize = sizeof(scratch);
  }
  auto fail = [&](const char* text) {
    snprintf(message, messageSize, "%s", text);
    return false;
  };

  if (!filesystemReady) return fail("Error: LittleFS not ready");

  if (newPassword != confirmPassword) {
    return fail("Error: New passwords do not match");
  }

  if (newPassword.length() < 6) {
    return fail("Error: Password must be at least 6 characters");
  }

  if (newPassword == currentPassword) {
    return fail("Error: New password must differ from current password");
  }

  // Resolve calling user from per-task TLS identity. Works for every
  // transport (web / CLI / OLED / BLE / ESP-NOW / serial) because each
  // dispatch path installs its own AuthContext via ExecIdentityGuard.
  String username = currentExecUser();
  if (username.length() == 0) return fail("Error: Not authenticated");
  const AuthContext& caller = currentAuthContext();

  // Verify current password
  if (!isValidUser(username, currentPassword)) {
    // Security event — log alongside login attempts so the auth-events
    // file shows both authentication AND credential-rotation activity.
    logAuthAttempt(false, caller.path.c_str(), username, caller.ip, "Current password incorrect");
    return fail("Error: Current password incorrect");
  }

  if (!setUserPassword(username, newPassword)) {
    logAuthAttempt(false, caller.path.c_str(), username, caller.ip, "Password storage failed");
    snprintf(message, messageSize, "Error: Failed to change password for user '%s'", username.c_str());
    return false;
  }

  // Force-logout the user's OTHER sessions across all transports —
  // their credentials just rotated. Keep the calling session alive
  // (they just authenticated with the new password; kicking them
  // immediately would be bad UX). No clock bump: password change
  // does not change permission topology, so auth-derived caches
  // (FileManager etc.) don't need to invalidate. See
  // System_AuthIdentity.h "SISTER PROTOCOL: SESSION REVOCATION"
  // for the bump-vs-revoke decision matrix.
  revokeUserSessions(username,
                     "Your password was changed. Please log in again.",
                     caller.sid,         // skip current web SID (if any)
                     caller.transport);  // skip current transport's session

  // Audit-log the successful rotation. The reason string "Password changed"
  // is what logAuthAttempt's filter matches on to write this to the
  // security-events file (rather than the verbose command-audit log).
  logAuthAttempt(true, caller.path.c_str(), username, caller.ip, "Password changed");
  systemEventPost(SYSEVT_PASSWORD_CHANGED, username.c_str(), "self");

  snprintf(message, messageSize, "Password changed successfully for user '%s'", username.c_str());
  return true;
}

const char* cmd_user_changepassword(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Parse: "currentPassword newPassword confirmPassword"
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: user changepassword <currentPassword> <newPassword> <confirmPassword>";

  // Handlers run on the executor, so the reply may live in the debug buffer.
  char message[USER_CHANGE_PASSWORD_MESSAGE_MAX];
  const bool ok = userChangePasswordCore(a.arg(0), a.arg(1), a.arg(2),
                                         message, sizeof(message));
  if (!ensureDebugBuffer()) {
    return ok ? "Password changed successfully" : "Error: Failed to change password";
  }
  snprintf(getDebugBuffer(), 1024, "%s", message);
  return getDebugBuffer();
}

const char* cmd_user_resetpassword(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";
  
  // Parse: "<username> <newPassword> [0|1]" — optional 1 = require new password on next login
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: user resetpassword <username> <newPassword> [0|1]\nOptional: 1 = require password change on next login, 0 = omit";
  }

  String username = a.arg(0);
  String rest = a.arg(1);
  bool mustChange = a.argBool(2, false);

  // A password reset is account takeover, so gate it like the other user
  // mutations (promote/demote/delete/ban): a caller may not reset the password
  // of a higher-privileged account (a plain admin cannot seize a super-admin),
  // and the owner (id==1) changes its own password via changepassword. Without
  // this an admin could reset a super's password and log in as them — which
  // defeats the entire tier.
  {
    uint32_t targetId = 0;
    if (getUserIdByUsername(username, targetId) && targetId == 1)
      return "Error: the owner account manages its own password (use 'changepassword')";
    String merr;
    if (!userMutationAllowed(username, kRoleRankNoGrant, merr)) {
      if (!ensureDebugBuffer()) return "Error: denied";
      snprintf(getDebugBuffer(), 1024, "Error: %s", merr.c_str());
      return getDebugBuffer();
    }
  }

  if (rest.length() < 6) {
    return "Error: Password must be at least 6 characters";
  }

  if (mustChange && accountIsGuestForPasswordChange(username)) {
    return "Error: A guest account cannot be required to change its password";
  }

  if (!setUserPassword(username, rest, mustChange)) {
    if (!ensureDebugBuffer()) return "Error: Failed to reset password";
    snprintf(getDebugBuffer(), 1024, "Error: Failed to reset password for user '%s'", username.c_str());
    return getDebugBuffer();
  }

  // Force-logout target user's sessions everywhere. Admin's own session
  // is a different user, so no exception filter — the target gets
  // kicked from every transport and must log in with the new password.
  // No clock bump: password reset does not change permission topology.
  revokeUserSessions(username,
                     "Your password was reset by an administrator. Please log in again.");
  systemEventPost(SYSEVT_PASSWORD_CHANGED, username.c_str(), "admin-reset");

  if (!ensureDebugBuffer()) return "Password reset successfully";
  snprintf(getDebugBuffer(), 1024, "Password reset successfully for user '%s'%s", username.c_str(),
           mustChange ? " (must change password on next login)" : "");
  return getDebugBuffer();
}

// Create user immediately (admin).
// Args: "<username> <password> [0|1] [guest|user|admin|superadmin]"
// The two optional tokens are order-independent: a bare 0/1 is the
// must-change-password flag, a role name is the role. They can't be confused
// with each other (no role is named "0"), so `useradd bob secret admin` does
// what it looks like instead of silently creating a plain user — which is what
// a strictly-positional third argument would have done.
const char* cmd_user_add(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: useradd <username> <password> [0|1] [guest|user|admin|superadmin]\n"
           "  0|1: 1 = require a new password on next login\n"
           "  role: defaults to user; you cannot grant a role above your own";
  }

  String username = a.arg(0);
  String rest = a.arg(1);

  bool mustChange = false;
  String role = "user";
  for (int i = 2; i <= 3; i++) {
    String t = a.arg(i);
    if (t.length() == 0) continue;
    t.toLowerCase();
    if (t == "0" || t == "1")        mustChange = (t == "1");
    else if (isKnownUserRole(t))     role = t;
    else {
      if (!ensureDebugBuffer()) return "Error: unrecognised argument";
      snprintf(getDebugBuffer(), 1024,
               "Error: unrecognised argument '%s' - expected 0|1 or guest|user|admin|superadmin",
               t.c_str());
      return getDebugBuffer();
    }
  }

  if (rest.length() < 6) {
    return "Error: Password must be at least 6 characters";
  }

  // Same gate promote/demote use: you cannot create an account ranked above
  // yourself. Checked BEFORE the account exists, so a rejected request leaves
  // nothing behind — the alternative (create-then-promote) would strand a
  // half-made user at the wrong tier if the second step failed.
  String rankErr;
  if (!userMutationAllowed(username, userRoleRank(role), rankErr)) {
    if (!ensureDebugBuffer()) return "Error: Cannot grant that role";
    snprintf(getDebugBuffer(), 1024, "Error: %s", rankErr.c_str());
    return getDebugBuffer();
  }

  String createdBy = currentAuthContext().user;
  if (createdBy.length() == 0) {
    createdBy = "cli";
  }

  String err;
  if (!adminCreateUser(username, rest, mustChange, createdBy, err, role)) {
    if (!ensureDebugBuffer()) return "Error: Failed to create user";
    snprintf(getDebugBuffer(), 1024, "Error: %s", err.c_str());
    return getDebugBuffer();
  }

  if (!ensureDebugBuffer()) return "User created";
  snprintf(getDebugBuffer(), 1024, "Created user '%s' as %s%s", username.c_str(), role.c_str(),
           mustChange ? " (must change password on next login)" : "");
  return getDebugBuffer();
}

// ============================================================================
// Read-only roster snapshot (display pages, userlist)
// ============================================================================
enum class RosterReadResult : uint8_t { Ok, Missing, Unreadable, Malformed };
// createdAt is the stored creation time, empty while unresolved; only the
// userlist reply carries it, so it is not part of the public row.
typedef void (*UserRosterVisitor)(const UserRosterEntry& entry, const char* createdAt,
                                  void* context);

// One locked read of users.json, then calls visit once per account in file
// order with a normalized row (see UserRosterEntry in System_User.h). The
// filesystem lock covers only the read, so a visitor may print.
static RosterReadResult visitUserRoster(UserRosterVisitor visit, void* context) {
  if (!filesystemReady) return RosterReadResult::Unreadable;
  String json;
  {
    FsLockGuard guard("users.roster");
    if (!VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.roster"))) {
      return RosterReadResult::Missing;
    }
    if (!readRosterJson("user.roster", json)) return RosterReadResult::Unreadable;
  }
  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json)) return RosterReadResult::Malformed;
  JsonArrayConst users = doc["users"].as<JsonArrayConst>();
  bool firstAccount = true;
  for (JsonObjectConst user : users) {
    const bool wasFirst = firstAccount;
    firstAccount = false;
    const char* name = user["username"] | "";
    const size_t nameLen = strlen(name);
    if (nameLen == 0 || nameLen > kPublicUsernameMaxLen) continue;
    UserRosterEntry entry = {};
    entry.id = user["id"] | 0u;
    memcpy(entry.username, name, nameLen + 1);
    // Same normalization as the authorization lookup (roleFromRosterJson),
    // except that an unknown or malformed role reads as "user" for display.
    const JsonVariantConst roleField = user["role"];
    String role;
    if (roleField.isNull()) {
      role = wasFirst ? "superadmin" : "user";
    } else if (roleField.is<const char*>()) {
      role = roleField.as<const char*>();
      role.toLowerCase();
      if (!isKnownUserRole(role)) role = "user";
    } else {
      role = "user";
    }
    snprintf(entry.role, sizeof(entry.role), "%s", role.c_str());
    entry.banned = user["banned"] | false;
    const char* createdAt =
        user["createdAt"].is<const char*>() ? user["createdAt"].as<const char*>() : "";
    visit(entry, createdAt, context);
  }
  return RosterReadResult::Ok;
}

struct RosterArrayFill {
  UserRosterEntry* out;
  size_t capacity;
  size_t count;
  size_t total;
};

static void rosterArrayVisit(const UserRosterEntry& entry, const char*, void* context) {
  RosterArrayFill* fill = static_cast<RosterArrayFill*>(context);
  if (fill->count < fill->capacity) fill->out[fill->count++] = entry;
  fill->total++;
}

bool readUserRoster(UserRosterEntry* out, size_t capacity, size_t& countOut,
                    size_t* totalOut) {
  RosterArrayFill fill = { out, out ? capacity : 0, 0, 0 };
  const bool ok = visitUserRoster(rosterArrayVisit, &fill) == RosterReadResult::Ok;
  countOut = ok ? fill.count : 0;
  if (totalOut) *totalOut = ok ? fill.total : 0;
  return ok;
}

// Serializes a list reply into a lazily allocated PSRAM buffer of one command
// result. serializeJson writes no NUL when the output fills the buffer, so a
// document that does not fit becomes an explicit error, never a cut or
// unterminated reply.
static const char* listJsonReply(const JsonDocument& doc, char*& buffer,
                                 const char* tag, const char* tooLarge) {
  if (!buffer) {
    buffer = (char*)ps_alloc(CMD_RESULT_MAX, AllocPref::PreferPSRAM, tag);
    if (!buffer) return "Error: out of memory for the list reply";
  }
  if (doc.overflowed()) return tooLarge;
  const size_t len = serializeJson(doc, buffer, CMD_RESULT_MAX);
  if (len == 0 || len >= CMD_RESULT_MAX - 1) {
    buffer[0] = '\0';
    ERROR_MEMORYF("%s: reply does not fit (%zu >= %zu)", tag, len, (size_t)CMD_RESULT_MAX - 1);
    return tooLarge;
  }
  return buffer;
}

static void userListJsonVisit(const UserRosterEntry& entry, const char* createdAt,
                              void* context) {
  JsonArray& users = *static_cast<JsonArray*>(context);
  // Only the fields the Settings Users panel and the user-sync picker read.
  // The char arrays are passed as const char* so ArduinoJson copies them: a
  // const char[N] would be kept by pointer as if it were a string literal, and
  // the entry is reused for the next row.
  JsonObject row = users.add<JsonObject>();
  row["id"] = entry.id;
  row["username"] = static_cast<const char*>(entry.username);
  row["role"] = static_cast<const char*>(entry.role);
  row["banned"] = entry.banned;
  if (createdAt && createdAt[0]) row["createdAt"] = createdAt;
}

static void userListTextVisit(const UserRosterEntry& entry, const char*, void* context) {
  int& count = *static_cast<int*>(context);
  if (count++ == 0) broadcastOutput("Users:");
  BROADCAST_PRINTF("  %s (%s)%s", entry.username, entry.role, entry.banned ? " [banned]" : "");
}

const char* cmd_user_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  // Check if JSON output is requested (word-boundary "json" token via argWantsJson)
  bool jsonOutput = argWantsJson(argsInput);

  DEBUG_USERSF("[USER_LIST_DEBUG] Called with args='%s', jsonOutput=%d", argsInput.c_str(), jsonOutput);

  if (jsonOutput) {
    // Wrap under {"schema":1,"users":[...]} for the object-only JSON contract.
    PSRAM_JSON_DOC(out);
    out["schema"] = 1;
    JsonArray users = out["users"].to<JsonArray>();
    const RosterReadResult read = visitUserRoster(userListJsonVisit, &users);
    if (read != RosterReadResult::Ok) {
      if (read != RosterReadResult::Missing) ERROR_SESSIONF("userlist: users file unreadable or malformed");
      return "{\"schema\":1,\"users\":[]}";
    }
    static char* jsonBuf = nullptr;
    return listJsonReply(out, jsonBuf, "user.list.json",
                         "Error: user list does not fit in one reply");
  }

  // Stream human-readable format (the visitor prints the "Users:" header
  // before the first row).
  int userCount = 0;
  const RosterReadResult read = visitUserRoster(userListTextVisit, &userCount);
  if (read == RosterReadResult::Missing) return "No users found";
  if (read != RosterReadResult::Ok) {
    broadcastOutput(read == RosterReadResult::Malformed ? "Error: Malformed users file"
                                                        : "Error: Failed to read users file");
    return "ERROR";
  }
  if (userCount == 0) {
    broadcastOutput("Users:");
    broadcastOutput("No users found");
  }
  return "OK";
}

const char* cmd_pending_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!filesystemReady) return "Error: LittleFS not ready";

  // Check if JSON output is requested
  bool jsonOutput = argWantsJson(argsInput);

  if (!VFS::existsGuarded(PENDING_USERS_FILE, VFS::systemAuth("user.pending.list"))) {
    if (jsonOutput) return "{\"schema\":1,\"pending\":[]}";
    broadcastOutput("No pending users");
    return "OK";
  }

  // Open and parse pending users file with ArduinoJson
  File file = VFS::openGuarded(PENDING_USERS_FILE, "r", VFS::systemAuth("user.pending.list"));
  if (!file) {
    if (jsonOutput) return "{\"schema\":1,\"pending\":[]}";
    ERROR_SESSIONF("Failed to read pending users file");
    broadcastOutput("Error: Failed to read pending users file");
    return "ERROR";
  }

  // Parse JSON array (1KB should be enough for pending users)
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    if (jsonOutput) return "{\"schema\":1,\"pending\":[]}";
    ERROR_SESSIONF("Malformed pending users file");
    broadcastOutput("Error: Malformed pending users file");
    return "ERROR";
  }

  JsonArray pending = doc.as<JsonArray>();
  if (!pending) {
    return jsonOutput ? "{\"schema\":1,\"pending\":[]}" : "No pending users";
  }

  if (jsonOutput) {
    // Build sanitized {"schema":1,"pending":[...]} object without password hashes.
    PSRAM_JSON_DOC(sanitized);
    sanitized["schema"] = 1;
    JsonArray sanitizedArray = sanitized["pending"].to<JsonArray>();
    for (JsonObject user : pending) {
      JsonObject sanitizedUser = sanitizedArray.add<JsonObject>();
      sanitizedUser["username"] = user["username"];
      sanitizedUser["timestamp"] = user["timestamp"];
      // Explicitly exclude password field for security
    }
    static char* jsonBuf = nullptr;
    return listJsonReply(sanitized, jsonBuf, "pending.list.json",
                         "Error: pending list does not fit in one reply");
  } else {
    // Stream human-readable format
    broadcastOutput("Pending Users:");

    int userCount = 0;
    for (JsonObject user : pending) {
      const char* username = user["username"];
      if (username) {
        BROADCAST_PRINTF("  %s (pending approval)", username);
        userCount++;
      }
    }

    if (userCount == 0) {
      broadcastOutput("No pending users");
    }
    return "OK";
  }
}

#if ENABLE_HTTP_SERVER
const char* cmd_session_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Check if JSON output is requested
  bool jsonOutput = argWantsJson(argsInput);

  if (jsonOutput) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray sessions = doc["sessions"].to<JsonArray>();
    // This is the single all-transport aggregator: web plus Serial, UART,
    // OLED, G2, and every authenticated BLE connection.
    buildAllSessionsJson("", sessions);
    static char* jsonBuf = nullptr;
    return listJsonReply(doc, jsonBuf, "session.list.json",
                         "Error: session list does not fit in one reply");
  } else {
    // Stream human-readable format
    broadcastOutput("Active Sessions:");

    const int sessionCount = webBroadcastSessionsHuman();

    if (sessionCount == 0) {
      broadcastOutput("No active sessions");
    }
    return "OK";
  }
}

#else
const char* cmd_session_list(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(originalCmd)) return "{\"schema\":1,\"available\":false,\"reason\":\"http server disabled\"}";
  return "Session management requires HTTP server to be enabled";
}
#endif // ENABLE_HTTP_SERVER

// cmd_login and cmd_logout moved to System_Utils.cpp (critical system functions)

// Account sessions live on every transport, so `sessionrevoke user` exists in
// every build; only revocation by web session id needs the HTTP server.
const char* cmd_session_revoke(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  String subcmd = a.arg(0);
  subcmd.toLowerCase();

  auto defaultReason = String("Your session has been signed out by an administrator.");

  int revoked = 0;

  if (subcmd == "sid") {
#if !ENABLE_HTTP_SERVER
    return "Error: revoking by session id requires the HTTP server";
#else
    String sid = a.arg(1);
    String reason = a.has(2) ? a.remaining(1) : String();
    if (!reason.length()) reason = defaultReason;
    String who;
    if (!webRevokeSessionBySid(sid, reason, &who)) {
      return "Error: Session not found for given SID.";
    }
    // Admin audit broadcast
    {
      if (!who.length()) who = "(unknown)";
      if (ensureDebugBuffer()) {
        snprintf(getDebugBuffer(), 1024, "Admin audit: revoked session by SID for user '%s' reason='%s'", who.c_str(), reason.c_str());
        broadcastOutput(getDebugBuffer());
      }
    }
    if (!ensureDebugBuffer()) return "Revoked 1 session";
    snprintf(getDebugBuffer(), 1024, "Revoked 1 session (sid=%s)", sid.c_str());
    return getDebugBuffer();
#endif
  }

  if (subcmd == "user") {
    String username = a.arg(1);
    String reason = a.has(2) ? a.remaining(1) : String();
    if (!reason.length()) reason = defaultReason;

    // Same rank rule as ban/unban: an account may not sign out a
    // higher-privileged one.
    {
      String merr;
      if (!userMutationAllowed(username, kRoleRankNoGrant, merr)) {
        EXT_RAM_BSS_ATTR static char ebuf[160];
        snprintf(ebuf, sizeof(ebuf), "Error: %s", merr.c_str());
        return ebuf;
      }
    }

    // Delegate fan-out to revokeUserSessions — single source of truth for
    // "kick user X out of every transport's session table." The previous
    // inline implementation duplicated this loop and drifted: it was
    // missing the oledNotifyLocalDisplayAuthChanged() call that
    // revokeUserSessions emits when clearing the OLED session, so the
    // OLED UI didn't refresh after an admin-revoke. Routing through the
    // shared helper picks that up automatically, and any future revoke-
    // protocol additions land in one place. We do NOT pass exceptSid /
    // exceptTransport because this command is admin-driven and revokes
    // someone else's sessions (not a self-modify).
    revoked = revokeUserSessions(username, reason);

    if (revoked > 0) {
      // Admin audit broadcast — emitted at the command layer (not inside
      // revokeUserSessions, which only WARN_USERFs internally) so the
      // operator running the CLI sees confirmation in their terminal.
      if (ensureDebugBuffer()) {
        snprintf(getDebugBuffer(), 1024, "Admin audit: revoked %d session(s) for user '%s' reason='%s'", revoked, username.c_str(), reason.c_str());
        broadcastOutput(getDebugBuffer());
      }
    }
    if (revoked == 0) {
      if (!ensureDebugBuffer()) return "No active sessions found";
      snprintf(getDebugBuffer(), 1024, "No active sessions found for user '%s'.", username.c_str());
      return getDebugBuffer();
    }
    if (!ensureDebugBuffer()) return "Revoked";
    snprintf(getDebugBuffer(), 1024, "Revoked %d session(s) for user '%s'.", revoked, username.c_str());
    return getDebugBuffer();
  }

  return "Error: invalid arguments — Usage:\n"
         "  sessionrevoke sid <sid> [reason]\n"
         "  sessionrevoke user <username> [reason]";
}

// The IP ban list lives in the web server (sIpBans).
#if ENABLE_HTTP_SERVER
const char* cmd_ban(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: ban <ip> [reason]";

  String ip = a.arg(0);
  String reason = a.has(1) ? a.remaining(0) : String();
  // Basic format check — must contain a dot (IPv4) or colon (IPv6)
  if (ip.indexOf('.') < 0 && ip.indexOf(':') < 0) {
    return "Error: invalid IP address format (expected e.g. 192.168.1.100)";
  }

  if (banIp(ip.c_str(), reason.length() ? reason.c_str() : nullptr)) {
    EXT_RAM_BSS_ATTR static char buf[140];
    if (reason.length()) snprintf(buf, sizeof(buf), "Banned %s — %s", ip.c_str(), reason.c_str());
    else                 snprintf(buf, sizeof(buf), "Banned %s", ip.c_str());
    return buf;
  }
  // banIp keeps a ban in force when only the save failed.
  if (isIpBanned(ip.c_str())) {
    EXT_RAM_BSS_ATTR static char notSaved[120];
    snprintf(notSaved, sizeof(notSaved), "Error: banned %s, not saved (ends at reboot)", ip.c_str());
    return notSaved;
  }
  return "Error: could not ban IP (ban list full)";
}

const char* cmd_unban(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String ip = argsInput;
  ip.trim();
  if (ip.length() == 0) return "Error: invalid arguments — Usage: unban <ip>";

  if (unbanIp(ip.c_str())) {
    EXT_RAM_BSS_ATTR static char buf[80];
    snprintf(buf, sizeof(buf), "Unbanned %s", ip.c_str());
    return buf;
  }
  // unbanIp restores the ban when the save failed.
  if (isIpBanned(ip.c_str())) {
    EXT_RAM_BSS_ATTR static char notSaved[120];
    snprintf(notSaved, sizeof(notSaved), "Error: unban of %s not saved; the ban stays", ip.c_str());
    return notSaved;
  }
  return "Error: IP not found in ban list";
}

const char* cmd_banlist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) return banListJson();  // built in WebServer_Server.cpp where sIpBans lives
  broadcastBanList();
  return "";
}
#else
const char* cmd_ban(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Ban management requires HTTP server to be enabled";
}
const char* cmd_unban(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Ban management requires HTTP server to be enabled";
}
const char* cmd_banlist(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(originalCmd)) return "{\"schema\":1,\"available\":false,\"reason\":\"http server disabled\"}";
  return "Ban management requires HTTP server to be enabled";
}
#endif // ENABLE_HTTP_SERVER

// Account bans live in users.json, so banuser/unbanuser exist in every build.
const char* cmd_banuser(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: banuser <username> [reason]";

  String username = a.arg(0);
  String reason = a.has(1) ? a.remaining(0) : String();

  // Rank check: a regular admin cannot ban a super-admin (founder also hard-
  // protected in setUserBanInternal).
  EXT_RAM_BSS_ATTR static char ebuf[160];
  {
    String merr;
    if (!userMutationAllowed(username, kRoleRankNoGrant, merr)) {
      snprintf(ebuf, sizeof(ebuf), "Error: %s", merr.c_str());
      return ebuf;
    }
  }

  String err;
  if (!setUserBanInternal(username, true, reason, err)) {
    snprintf(ebuf, sizeof(ebuf), "Error: %s", err.c_str());
    return ebuf;
  }

  EXT_RAM_BSS_ATTR static char buf[160];
  if (reason.length()) snprintf(buf, sizeof(buf), "Banned user '%s' — %s", username.c_str(), reason.c_str());
  else                 snprintf(buf, sizeof(buf), "Banned user '%s'", username.c_str());
  return buf;
}

const char* cmd_unbanuser(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String username = argsInput;
  username.trim();
  if (username.length() == 0) return "Error: invalid arguments — Usage: unbanuser <username>";

  // Same rank rule as banuser: a regular admin cannot lift a super-admin's ban.
  EXT_RAM_BSS_ATTR static char ebuf[160];
  {
    String merr;
    if (!userMutationAllowed(username, kRoleRankNoGrant, merr)) {
      snprintf(ebuf, sizeof(ebuf), "Error: %s", merr.c_str());
      return ebuf;
    }
  }

  String err;
  if (!setUserBanInternal(username, false, "", err)) {
    snprintf(ebuf, sizeof(ebuf), "Error: %s", err.c_str());
    return ebuf;
  }

  EXT_RAM_BSS_ATTR static char buf[80];
  snprintf(buf, sizeof(buf), "Unbanned user '%s'", username.c_str());
  return buf;
}

// Command adapter: parses the arguments and reports the owner's result
// (submitAccountRequest, beside the other pending-list owners).
const char* cmd_user_request(const String& argsInput) {
  // Syntax: args = "<username> <password> [confirmPassword]"
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: user request <username> <password> [confirmPassword]";

  String username = a.arg(0);
  username.trim();
  String password = a.arg(1);
  String confirm = a.arg(2);  // empty if not provided
  if (confirm.length() && confirm != password) return "Error: passwords do not match";

  AccountRequestResult result = checkAccountRequestFields(username, password);
  if (result == AccountRequestResult::Submitted) {
    // Validation stops here: no pending-list read or write, no event.
    RETURN_VALID_IF_VALIDATE_CSTR();
    result = submitAccountRequest(username, password);
  }

  switch (result) {
    case AccountRequestResult::Submitted: break;
    case AccountRequestResult::InvalidUsername: return "Error: Invalid username";
    case AccountRequestResult::PasswordTooShort: return "Error: Password must be at least 6 characters";
    case AccountRequestResult::PasswordTooLong: return "Error: Password is too long";
    case AccountRequestResult::AlreadyPending: return "Error: Username already pending approval";
    case AccountRequestResult::QueueFull: return "Error: Too many pending requests";
    case AccountRequestResult::NotReady: return "Error: LittleFS not ready";
    case AccountRequestResult::ReadFailed: return "Error: could not read pending list";
    case AccountRequestResult::WriteFailed: return "Error: could not write pending list";
  }

  if (!ensureDebugBuffer()) return "Request submitted (buffer unavailable)";
  snprintf(getDebugBuffer(), 1024, "Request submitted for '%s' (JSON)", username.c_str());
  return getDebugBuffer();
}

// ============================================================================
// Command Registry
// ============================================================================

// CommandEntry struct is defined in system_utils.h (included via user_system.h)
// Note: userCommands array removed - use userSystemCommands instead

// ============================================================================
// User Filesystem Operations - MIGRATED from main .ino
// ============================================================================

// File path (exported for use by other modules)
const char* USERS_JSON_FILE = "/system/users/users.json";

// Boot anchors (NTP time-sync scaffolding) live in their OWN file, decoupled
// from the credential database. They are written once per boot on NTP sync;
// keeping them out of users.json means users.json only changes on real account
// edits, and an anchor write can never touch the auth DB. Structure:
//   {"bootAnchors":[{"ntpAnchorId":N,"epochAtSync":E,"millisAtSync":M}, ...]}
const char* BOOT_ANCHORS_FILE = "/system/boot_anchors.json";

// External dependencies for timestamp resolution
extern uint32_t gNTPAnchorId;
extern uint32_t gBootCounter;

// Boot anchor structure - represents an NTP sync point
struct BootAnchor {
  uint32_t ntpAnchorId;
  time_t epochAtSync;
  unsigned long millisAtSync;

  BootAnchor()
    : ntpAnchorId(0), epochAtSync(0), millisAtSync(0) {}
  BootAnchor(uint32_t seq, time_t epoch, unsigned long ms)
    : ntpAnchorId(seq), epochAtSync(epoch), millisAtSync(ms) {}
};

// Helper: Check if username exists in users.json content
bool usernameExistsInUsersJson(const String& json, const String& username) {
  char needleBuf[80];
  snprintf(needleBuf, sizeof(needleBuf), "\"username\": \"%s\"", username.c_str());
  String needle = needleBuf;
  return json.indexOf(needle) >= 0;
}

// (Removed: loadUsersFromFile — read a `passwordHash` field that no longer
// exists in users.json since the User struct moved to PBKDF2 with per-device
// salt. The function returned false every time, so its only caller in
// HardwareOne.cpp boot left the legacy gAuthUser/gAuthPass globals at their
// "admin"/"admin" defaults. Both the function and its callers + the Basic-
// Auth fast path that consumed those globals were deleted together. See
// AUTH_ASSESSMENT_REPORT.md §4 #6.)

// Forward declarations for helper functions
static int parseBootAnchors(const String& usersJson, BootAnchor* anchors, int maxCount);
static BootAnchor* findMatchingAnchor(BootAnchor* anchors, int count, uint32_t ntpAnchorId);
static void buildOrdinal(uint32_t n, char* buf, size_t bufSize);
static bool formatEpochAsISO8601(time_t epoch, char* buf, size_t bufSize);

// Build ordinal string (1st, 2nd, 3rd, etc.)
static void buildOrdinal(uint32_t n, char* buf, size_t bufSize) {
  if (!buf || bufSize < 6) return;

  const char* suffix;
  uint32_t mod100 = n % 100;
  if (mod100 >= 11 && mod100 <= 13) {
    suffix = "th";
  } else {
    switch (n % 10) {
      case 1: suffix = "st"; break;
      case 2: suffix = "nd"; break;
      case 3: suffix = "rd"; break;
      default: suffix = "th"; break;
    }
  }
  snprintf(buf, bufSize, "%lu%s", (unsigned long)n, suffix);
}

// Format epoch as ISO 8601 string
static bool formatEpochAsISO8601(time_t epoch, char* buf, size_t bufSize) {
  if (!buf || bufSize < 21) return false;
  if (epoch <= 0) {
    strncpy(buf, "null", bufSize);
    return false;
  }
  if (!Clock::isValidEpoch(epoch)) {
    strncpy(buf, "null", bufSize);
    return false;
  }
  struct tm tminfo;
  if (!gmtime_r(&epoch, &tminfo)) {
    strncpy(buf, "null", bufSize);
    return false;
  }
  strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%SZ", &tminfo);
  return true;
}

// Parse boot anchors from users.json
static int parseBootAnchors(const String& usersJson, BootAnchor* anchors, int maxCount) {
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, usersJson);
  if (error) return 0;

  JsonArray bootAnchorsArray = doc["bootAnchors"];
  if (!bootAnchorsArray) return 0;

  int count = 0;
  for (JsonObject anchor : bootAnchorsArray) {
    if (count >= maxCount) break;
    
    int ntpAnchorId = anchor["ntpAnchorId"] | 0;
    int epochAtSync = anchor["epochAtSync"] | 0;
    int millisAtSync = anchor["millisAtSync"] | 0;
    
    if (ntpAnchorId > 0 && epochAtSync > 0) {
      anchors[count++] = BootAnchor(ntpAnchorId, epochAtSync, millisAtSync);
    }
  }

  return count;
}

// Read boot anchors from their dedicated file. Returns count (0 if the file is
// absent, empty, or corrupt — all benign: anchors are regenerated on NTP sync).
static int readBootAnchors(BootAnchor* anchors, int maxCount) {
  if (!filesystemReady) return 0;
  String json;
  if (!readText(BOOT_ANCHORS_FILE, json) || json.length() == 0) return 0;
  return parseBootAnchors(json, anchors, maxCount);
}

// Highest ntpAnchorId currently persisted (0 if none). Seeds gNTPAnchorId at boot.
static uint32_t highestBootAnchorId() {
  BootAnchor anchors[16];
  int n = readBootAnchors(anchors, 16);
  uint32_t maxId = 0;
  for (int i = 0; i < n; i++) {
    if (anchors[i].ntpAnchorId > maxId) maxId = anchors[i].ntpAnchorId;
  }
  return maxId;
}

// A roster entry still waits for its creation time when createdAt is:
//   1. JSON null (NTP wasn't synced at creation time)
//   2. Missing entirely (ArduinoJson may drop null values on re-serialize)
//   3. An approximate sentinel like "1st Power Cycle" written by approximateUserTimestamp()
//      — happens when gBootCounter advanced past bootCount before an anchor was available;
//      now that NTP has synced we can replace the approximation with the real timestamp.
// The approximate sentinel lives in createdAtSource; the legacy createdBy
// location is also accepted so pre-split users are still re-resolved.
static bool userNeedsCreationTime(JsonObjectConst user) {
  if (user["createdAt"].isNull()) return true;
  const char* source = user["createdAtSource"] | "";
  const char* createdBy = user["createdBy"] | "";
  return strcmp(source, "approx_power_cycle") == 0 || strcmp(createdBy, "approx_power_cycle") == 0;
}

// Find matching anchor for boot sequence
static BootAnchor* findMatchingAnchor(BootAnchor* anchors, int count, uint32_t ntpAnchorId) {
  for (int i = 0; i < count; i++) {
    if (anchors[i].ntpAnchorId == ntpAnchorId) {
      return &anchors[i];
    }
  }
  return nullptr;
}

// Resolve user timestamp using NTP anchor
static bool resolveUserTimestamp(JsonObject user, unsigned long createdMs,
                                 const BootAnchor& anchor) {
  long delta = (long)anchor.millisAtSync - (long)createdMs;

  time_t createdAtUtc;
  bool crossBoot = false;

  if (delta >= 0) {
    // Same-boot: user was created before NTP synced in the same boot cycle.
    // Exact calculation: walk back from the sync epoch by the elapsed ms difference.
    createdAtUtc = anchor.epochAtSync - (delta / 1000);
  } else {
    // Cross-boot: user was created in a previous boot (createdMs > millisAtSync).
    // We can't calculate the exact time without knowing when the prior boot started,
    // so use epochAtSync as an upper bound (user was created no later than this).
    createdAtUtc = anchor.epochAtSync;
    crossBoot = true;
  }

  if (!Clock::isValidEpoch(createdAtUtc)) return false;  // sanity: before 2020
  time_t now = time(nullptr);
  if (now > 0 && createdAtUtc > now + 60) return false;  // sanity: not in the future

  char isoTimestamp[24];
  if (!formatEpochAsISO8601(createdAtUtc, isoTimestamp, sizeof(isoTimestamp))) {
    return false;
  }

  user["createdAt"] = isoTimestamp;
  // Mark whether this was an exact resolution or a cross-boot approximation.
  // Record HOW createdAt was derived in createdAtSource; leave createdBy
  // (provenance: who created the account) untouched.
  user["createdAtSource"] = crossBoot ? "approx_ntp" : "ntp_resolved";
  return true;
}

// Approximate user timestamp using power cycle label
static bool approximateUserTimestamp(JsonObject user, uint32_t ordinalNumber) {
  char ordinal[16];
  buildOrdinal(ordinalNumber, ordinal, sizeof(ordinal));

  char approxLabel[48];
  snprintf(approxLabel, sizeof(approxLabel), "%s Power Cycle", ordinal);

  // Idempotence: the resolve pass re-runs on every clock step (duty drain),
  // and an already-approximated user would otherwise report "modified" every
  // time — a pointless users.json flash rewrite per step.
  const char* current = user["createdAt"] | "";
  if (strcmp(current, approxLabel) == 0) return false;  // unchanged

  user["createdAt"] = approxLabel;
  user["createdAtSource"] = "approx_power_cycle";
  return true;
}

// Prune boot anchors down to the single most-recent one — but only once NO user
// is still awaiting timestamp resolution (a pending user needs its anchor kept).
// Anchors now live in their own file (BOOT_ANCHORS_FILE); the pending-user guard
// still reads users.json, optionally reusing a users doc the caller already has.
// The usersDocPtr param is a JsonDocument* of parsed users.json, or nullptr.
void cleanupOldBootAnchors(void* usersDocPtr) {
  if (!filesystemReady) return;

  // 1) Don't prune if any user still has an unresolved (null) createdAt — that
  //    user's anchor must survive until it can be resolved.
  bool anyPending = false;
  {
    PSRAM_JSON_DOC(localUsers);
    JsonDocument* usersDoc = static_cast<JsonDocument*>(usersDocPtr);
    if (!usersDoc) {
      String usersJson;
      if (readText(USERS_JSON_FILE, usersJson) && usersJson.length() > 0) {
        DeserializationError err = deserializeJson(localUsers, usersJson);
        if (!err) usersDoc = &localUsers;
      }
    }
    if (!usersDoc) {
      // Fail CLOSED. A read failure of users.json is not evidence that no user is
      // pending — and pruning on that assumption destroys the anchor those users
      // need to resolve their createdAt. This is a read failure of file A
      // triggering a destructive write of file B.
      WARN_SESSIONF("[anchor] users.json unreadable - skipping anchor prune this pass");
      return;
    }
    {
      JsonArray usersArray = (*usersDoc)["users"];
      if (usersArray) {
        for (JsonObject user : usersArray) {
          if (user["createdAt"].isNull()) { anyPending = true; break; }
        }
      }
    }
  }
  if (anyPending) return;

  // 2) Prune the anchors file to just the most recent anchor.
  BootAnchor anchors[16];
  int n = readBootAnchors(anchors, 16);
  if (n <= 1) return;  // nothing to prune

  int maxIdx = 0;
  for (int i = 1; i < n; i++) {
    if (anchors[i].ntpAnchorId > anchors[maxIdx].ntpAnchorId) maxIdx = i;
  }

  PSRAM_JSON_DOC(doc);
  JsonArray arr = doc["bootAnchors"].to<JsonArray>();
  JsonObject a = arr.add<JsonObject>();
  a["ntpAnchorId"]  = (uint32_t)anchors[maxIdx].ntpAnchorId;
  a["epochAtSync"]  = (uint32_t)anchors[maxIdx].epochAtSync;
  a["millisAtSync"] = (uint32_t)anchors[maxIdx].millisAtSync;

  String out;
  serializeJson(doc, out);
  writeTextAtomic(BOOT_ANCHORS_FILE, out);
}

struct ResolveCreationTimesContext {
  BootAnchor* anchors;
  int anchorCount;
  bool modified;
};

static UsersJsonMutation resolveCreationTimesMutation(JsonDocument& doc, JsonArray users, void* opaque,
                                                      String& errorOut) {
  (void)doc;
  (void)errorOut;
  ResolveCreationTimesContext* ctx = static_cast<ResolveCreationTimesContext*>(opaque);
  bool& modified = ctx->modified;
  for (JsonObject user : users) {
    if (!userNeedsCreationTime(user)) continue;
    JsonVariant anchorField = user["ntpAnchorId"];
    JsonVariant createdField = user["createdMs"];
    if (!anchorField.is<uint32_t>() || !createdField.is<unsigned long>()) {
      DEBUG_NTP_RESOLVEF("[resolve] User lacks ntpAnchorId/createdMs - cannot resolve");
      continue;
    }
    const uint32_t ntpAnchorId = anchorField.as<uint32_t>();
    const unsigned long createdMs = createdField.as<unsigned long>();
    const int bootCount = user["bootCount"] | -1;
    DEBUG_NTP_RESOLVEF("[resolve] User needs resolution: ntpAnchorId=%lu createdMs=%lu bootCount=%d",
                       (unsigned long)ntpAnchorId, createdMs, bootCount);

    BootAnchor* anchor = findMatchingAnchor(ctx->anchors, ctx->anchorCount, ntpAnchorId);
    if (anchor) {
      DEBUG_NTP_RESOLVEF("[resolve] Found matching anchor for ntpAnchorId=%lu", (unsigned long)ntpAnchorId);
      if (resolveUserTimestamp(user, createdMs, *anchor)) {
        INFO_SESSIONF("Successfully resolved timestamp");
        modified = true;
      } else {
        WARN_SESSIONF("Failed to resolve timestamp");
      }
      continue;
    }

    DEBUG_NTP_RESOLVEF("[resolve] No matching anchor for ntpAnchorId=%lu", (unsigned long)ntpAnchorId);
    bool shouldApprox = false;
    uint32_t ordinalN = ntpAnchorId;
    if (bootCount > 0 && gBootCounter > 0) {
      if ((uint32_t)bootCount < gBootCounter) {
        shouldApprox = true;
        ordinalN = (uint32_t)bootCount;
      }
    } else if (ntpAnchorId < gNTPAnchorId) {
      shouldApprox = true;
    }
    if (shouldApprox) {
      DEBUG_NTP_RESOLVEF("[resolve] Approximating timestamp with ordinal %lu", (unsigned long)ordinalN);
      if (approximateUserTimestamp(user, ordinalN)) modified = true;
    }
  }
  return modified ? UsersJsonMutation::Changed : UsersJsonMutation::Unchanged;
}

// Resolve pending user creation timestamps. The whole roster is parsed and
// rewritten through the users.json owner, so its size does not matter and the
// filesystem lock covers the read-modify-write: this runs from the main loop
// (Clock duties) concurrently with cmd_exec account edits.
void resolvePendingUserCreationTimes() {
  DEBUG_NTP_RESOLVEF("[resolve] Starting timestamp resolution");
  if (!filesystemReady) {
    DEBUG_NTP_RESOLVEF("[resolve] Skipping - FS not ready");
    return;
  }
  FsLockGuard fsGuard("user.resolveCreated");

  const int MAX_ANCHORS = 16;
  BootAnchor anchors[MAX_ANCHORS];
  int anchorCount = readBootAnchors(anchors, MAX_ANCHORS);  // from BOOT_ANCHORS_FILE
  DEBUG_NTP_RESOLVEF("[resolve] Found %d boot anchors", anchorCount);
  for (int i = 0; i < anchorCount; i++) {
    DEBUG_NTP_RESOLVEF("[resolve] Anchor %d: ntpAnchorId=%lu epochAtSync=%u millisAtSync=%lu",
                 i, (unsigned long)anchors[i].ntpAnchorId, (unsigned)anchors[i].epochAtSync, (unsigned long)anchors[i].millisAtSync);
  }

  ResolveCreationTimesContext ctx{anchors, anchorCount, false};
  String error;
  if (!mutateUsersJson("user.resolveCreated", resolveCreationTimesMutation, &ctx, error)) {
    DEBUG_NTP_RESOLVEF("[resolve] Skipping - %s", error.c_str());
    return;
  }
  if (ctx.modified) {
    cleanupOldBootAnchors();  // prunes BOOT_ANCHORS_FILE, guarded by pending-user check
  } else {
    DEBUG_NTP_RESOLVEF("[resolve] No modifications needed");
  }
}

// Append a boot anchor (NTP just synced) to the dedicated anchors file. Written
// atomically via writeTextAtomic; users.json is NOT touched.
void writeBootAnchor() {
  time_t now = time(nullptr);
  if (now <= 0 || gNTPAnchorId == 0) return;
  if (!filesystemReady) return;

  unsigned long currentMillis = millis();

  // Whole-RMW lock, same reasoning as resolvePendingUserCreationTimes: this
  // runs from the main-loop duty drain and must not interleave with another
  // task's write of the same file.
  FsLockGuard fsGuard("user.bootAnchor");

  // Load existing anchors. ABSENT is fine — start empty and create the file.
  // CORRUPT is not: "start empty" collapses up to 16 anchors to one and
  // permanently breaks timestamp resolution for every user still pending.
  PSRAM_JSON_DOC(doc);
  String existing;
  if (readText(BOOT_ANCHORS_FILE, existing) && existing.length() > 0) {
    DeserializationError error = deserializeJson(doc, existing);
    if (error) {
      ERROR_STORAGEF("[anchor] %s unreadable (%s) - anchor NOT written; file preserved",
                     BOOT_ANCHORS_FILE, error.c_str());
      systemEventPost(SYSEVT_CONFIG_FILE_CORRUPT, BOOT_ANCHORS_FILE, error.c_str());
      return;
    }
  }

  JsonArray bootAnchorsArray = doc["bootAnchors"];
  if (bootAnchorsArray.isNull()) bootAnchorsArray = doc["bootAnchors"].to<JsonArray>();

  // UPSERT, not append: this boot can anchor more than once (ring adoption
  // first, a real NTP sync minutes later), and findMatchingAnchor() returns
  // the FIRST row with this id — an appended second row would lose to the
  // stale ring-derived one forever. Overwriting in place keeps exactly one
  // row per anchor id, always the freshest (= most accurate) fix.
  for (JsonObject a : bootAnchorsArray) {
    if (a["ntpAnchorId"].as<uint32_t>() == (uint32_t)gNTPAnchorId) {
      a["epochAtSync"] = (uint32_t)now;
      a["millisAtSync"] = (uint32_t)currentMillis;
      String updated;
      serializeJson(doc, updated);
      writeTextAtomic(BOOT_ANCHORS_FILE, updated);
      return;
    }
  }

  if ((int)bootAnchorsArray.size() >= 16) {
    bootAnchorsArray.remove(0);
  }

  JsonObject newAnchor = bootAnchorsArray.add<JsonObject>();
  newAnchor["ntpAnchorId"] = (uint32_t)gNTPAnchorId;
  newAnchor["epochAtSync"] = (uint32_t)now;
  newAnchor["millisAtSync"] = (uint32_t)currentMillis;

  String out;
  serializeJson(doc, out);
  writeTextAtomic(BOOT_ANCHORS_FILE, out);
}

// ============================================================================
// User System Command Registry (System-Specific)
// ============================================================================
// serialrequireauth is registered by the settings module (System_Settings.cpp).

static const char* cmd_whoami(const String& argsInput) {
  CommandArgs args(argsInput);
  if (args.unterminatedQuote() || args.count() != 0) {
    return "Error: invalid arguments — Usage: whoami";
  }
  RETURN_VALID_IF_VALIDATE_CSTR();
  const AuthContext& auth = currentAuthContext();
  const char* transport = "unknown";
  switch (auth.transport) {
    case SOURCE_SERIAL: transport = "serial"; break;
    case SOURCE_UART: transport = "uart"; break;
    case SOURCE_LOCAL_DISPLAY: transport = "display"; break;
    case SOURCE_WEB: transport = "web"; break;
    case SOURCE_BLUETOOTH: transport = "bluetooth"; break;
    default: break;
  }
  EXT_RAM_BSS_ATTR static char buf[128];
  const String& user = auth.user.length() ? auth.user : String("(unknown)");
  snprintf(buf, sizeof(buf), "You are %s%s on %s", user.c_str(),
           isAdminUser(user) ? " (admin)" : "", transport);
  return buf;
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry userSystemCommands[] = {
  // Authentication commands
  { "login", "Login this interface, or target another session after signing in: <user> <pass> [serial|uart|display]", false, cmd_login,
    "Usage: login <username> <password> [serial|uart|display]\nBare login always targets the submitting interface. An explicit target requires a live named non-Guest Serial/UART/display session." },
  { "logout", "Logout this interface, or target another session after signing in: [serial|uart|display]", false, cmd_logout,
    "Usage: logout [serial|uart|display]\nBare logout always targets the submitting interface. An explicit target requires a live named non-Guest Serial/UART/display session." },
  { "whoami", "Show the identity of the submitting interface.", false, cmd_whoami, "Usage: whoami" },

  // User management commands
  { "userapprove", "Approve pending request: <username>", true, cmd_user_approve, "Usage: userapprove <username>" },
  { "userdeny", "Deny pending request: <username>", true, cmd_user_deny, "Usage: userdeny <username>" },
  { "userpromote", "Promote a user: <username> [user|admin|superadmin]", true, cmd_user_promote, "Usage: userpromote <username> [user|admin|superadmin]  (default admin; granting superadmin requires a super-admin caller)" },
  { "userdemote", "Lower a user's role: <username> [admin|user|guest]", true, cmd_user_demote, "Usage: userdemote <username> [admin|user|guest]  (default user; demoting a super-admin requires a super-admin caller)" },
  { "userdelete", "Delete user: <username>", true, cmd_user_delete, "Usage: userdelete <username>" },
  { "userchangepassword", "Change own password: <currentPass> <newPass> <confirmPass>", false, cmd_user_changepassword, "Usage: userchangepassword <currentPassword> <newPassword> <confirmPassword>" },
  { "userresetpassword", "Reset user password: <username> <newPassword> [0|1]", true, cmd_user_resetpassword,
    "Usage: userresetpassword <username> <newPassword> [0|1]\nOptional: 1 = require password change on next login" },
  { "useradd", "Create user: <username> <password> [0|1] [role]", true, cmd_user_add,
    "Usage: useradd <username> <password> [0|1] [guest|user|admin|superadmin]\n"
    "  0|1:  1 = require a new password on next login (default 0)\n"
    "  role: defaults to user; you cannot grant a role above your own\n"
    "  The two optional tokens may appear in either order" },
  { "userlist", "List all users. (add 'json' for JSON output)", true, cmd_user_list },
  { "userrequest", "Request account: <user> <pass> [confirm]", false, cmd_user_request, "Usage: userrequest <username> <password> [confirmPassword]" },
  { "usersync", "Sync a user to another device over ESP-NOW. (async; result only on the target device - check its userlist)", true, cmd_user_sync,
    "Usage: usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>\n\n       Returns OK on delivery; the user is created on the TARGET device (no confirmation returns here) - verify on that device's userlist."
    "  targetAdminUser/targetAdminPass = an admin account on the RECEIVING device (validated there).\n"
    "  yourAdminPass = your admin password on THIS device; userPass = the synced user's password." },

  // Session management commands
  { "pendinglist", "List pending user requests. (add 'json' for JSON output)", true, cmd_pending_list },
  { "sessionlist", "List active sessions. (add 'json' for JSON output)", true, cmd_session_list },
  { "sessionrevoke", "Revoke session: <sid|user> [reason]", true, cmd_session_revoke, "Usage:\n  sessionrevoke sid <sid> [reason]\n  sessionrevoke user <username> [reason]" },

  // IP ban management
  { "ban",      "Permanently ban an IP: <ip> [reason]",      true, cmd_ban,      "Usage: ban <ip> [reason]\nBlocks all access from the IP until manually unbanned." },
  { "unban",    "Remove an IP ban: <ip>",                    true, cmd_unban,    "Usage: unban <ip>" },
  { "banlist",  "List all banned IPs. (add 'json' for JSON output)",                      true, cmd_banlist },
  { "banuser",  "Permanently ban a user account: <username> [reason]", true, cmd_banuser,  "Usage: banuser <username> [reason]\nPrevents the account from logging in until manually unbanned." },
  { "unbanuser","Remove a user account ban: <username>",     true, cmd_unbanuser,"Usage: unbanuser <username>" }
};

const size_t userSystemCommandsCount = sizeof(userSystemCommands) / sizeof(userSystemCommands[0]);

// ============================================================================
// Command Registration (System-Specific)
// ============================================================================
// Direct static registration to avoid macro issues
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Boot Sequence Management
// ============================================================================

// Increment NTP anchor ID counter (memory-only, resets on power cycle)
void loadAndIncrementBootSeq() {
  // Boot counter now lives in NVS (its own flash partition), NOT users.json.
  // This was the ONLY reason users.json used to be rewritten every boot; moving
  // it here makes users.json a write-rarely file (real account edits only), so a
  // power cut can no longer destroy the auth database via the counter bump.
  gBootCounter = bootStateIncrementBootCount();

  // NTP anchor id is derived (read-only) from the highest id in the dedicated
  // anchors file — also decoupled from users.json.
  gNTPAnchorId = highestBootAnchorId();

  // Temporarily enable DEBUG_SYSTEM for boot sequence initialization (runs before settings loaded)
  DebugFlagMask _dbgSaved = getDebugFlags();
  setDebugFlag(DEBUG_SYSTEM);
  DEBUG_SYSTEMF("BootSeqInit: filesystemReady=%d, users.json exists=%d, bootCounter=%lu, maxAnchorId=%lu",
                (int)filesystemReady,
                (int)(filesystemReady && VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.bootSeq"))),
                (unsigned long)gBootCounter, (unsigned long)gNTPAnchorId);

  // Read-only integrity pass over users.json: if it's corrupt, quarantine it so
  // first-time setup re-arms cleanly (otherwise every login fails silently with
  // no recovery path, since serial itself requires auth). No write-back on the
  // healthy path — the file is never modified here.
  if (filesystemReady && VFS::existsGuarded(USERS_JSON_FILE, VFS::systemAuth("user.bootSeq"))) {
    File file = VFS::openGuarded(USERS_JSON_FILE, "r", VFS::systemAuth("user.bootSeq"));
    if (!file) {
      ERROR_SYSTEMF("BootSeqInit: Failed to open users.json");
    } else {
      PSRAM_JSON_DOC(doc);
      DeserializationError error = deserializeJson(doc, file);
      file.close();

      if (error) {
        ERROR_SYSTEMF("BootSeqInit: Failed to parse users.json");
        String bad = String(USERS_JSON_FILE) + ".bad";
        VFS::removeGuarded(bad.c_str(), VFS::systemAuth("user.bootSeq"));
        bool q = VFS::renameGuarded(USERS_JSON_FILE, bad.c_str(), VFS::systemAuth("user.bootSeq"));
        logSystemEvent("USERS", "users.json corrupt at boot (%s) — %s; first-time setup will re-arm",
                       error.c_str(), q ? "quarantined to users.json.bad" : "quarantine rename FAILED");
      } else {
        DEBUG_NTP_ANCHORF("BootSeqInit: users.json parsed OK");
        // Carry the roster's issued ids into the account-id mark, so a roster
        // written before the mark existed is covered by the next reset or
        // restore. Only a lower stored mark is rewritten.
        JsonArray users = doc["users"];
        if (users && !bootStateRaiseAccountIdHighWater(rosterIssuedIdHighWater(doc, users))) {
          ERROR_SYSTEMF("BootSeqInit: could not record the account id mark");
        }
      }
    }
  }

  gNTPAnchorId++;
  // Restore debug flags
  setDebugFlags(_dbgSaved);
  // Use DEBUG macro - now safe since debug system is initialized
  DEBUG_NTP_ANCHORF("[BOOT] NTP anchor ID: %lu (from %s) | Boot counter: %lu (NVS)",
                    (unsigned long)gNTPAnchorId, BOOT_ANCHORS_FILE, (unsigned long)gBootCounter);
}

// ============================================================================
// User Sync Command (merged from System_User_Sync.cpp)
// ============================================================================
// ESP-NOW user credential propagation - allows admin to sync users across devices

#if ENABLE_ESPNOW

#include "System_ESPNow.h"

extern uint32_t generateMessageId();
extern String getEspNowDeviceName(const uint8_t* mac);

// Helper to parse MAC address or device name
static bool userSyncParseMacAddress(const String& s, uint8_t out[6]) {
  if (s.length() == 17 && s[2] == ':' && s[5] == ':') {
    // MAC format: XX:XX:XX:XX:XX:XX
    for (int i = 0; i < 6; i++) {
      char hex[3] = {s[i*3], s[i*3+1], '\0'};
      out[i] = strtol(hex, NULL, 16);
    }
    return true;
  }
  return false;
}

static bool userSyncResolveDeviceNameOrMac(const String& deviceStr, uint8_t outMac[6]) {
  // Try parsing as MAC first
  if (userSyncParseMacAddress(deviceStr, outMac)) {
    return true;
  }
  
  // Try resolving as device name
  if (!gEspNow) return false;
  
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (gEspNow->devices[i].name.equalsIgnoreCase(deviceStr)) {
      memcpy(outMac, gEspNow->devices[i].mac, 6);
      return true;
    }
  }
  
  return false;
}

/**
 * User sync command: user sync <username> <device> <password>
 * 
 * Sends user credentials to a paired ESP-NOW device.
 * Requires admin privileges and user sync to be enabled.
 */
const char* cmd_user_sync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  // Check if ESP-NOW is enabled
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized";
  }
  
  // Check if user sync is enabled
  if (!gSettings.espnowUserSyncEnabled) {
    return "Error: User sync disabled - enable with 'espnowusersync on'";
  }
  
  // Parse command args
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(6)) {
    return "Error: invalid arguments — Usage: usersync <username> <userPass> <device> <targetAdminUser> <targetAdminPass> <yourAdminPass>";
  }

  // Auth mirrors remote command execution (espnow remote): the RECEIVING device
  // authenticates an admin from ITS OWN user store, so we transmit the TARGET
  // device's admin username+password. Fields are grouped logically — the synced
  // account + its password, then the device, then the target-admin + its
  // password, then your re-auth; the audit redactor (redactUserSyncCmd) masks
  // the three password tokens by position.
  String username      = a.arg(0);  // the user being synced
  String userPass      = a.arg(1);  // the synced user's password
  String deviceStr     = a.arg(2);  // target device (name or MAC)
  String recvAdminUser = a.arg(3);  // admin that exists on the TARGET device
  String recvAdminPass = a.arg(4);  // the TARGET admin's password
  String myAdminPass   = a.arg(5);  // this device's admin password (local re-auth)
  
  // Verify user exists locally
  uint32_t userId = 0;
  if (!getUserIdByUsername(username, userId)) {
    snprintf(getDebugBuffer(), 1024, "Error: User '%s' not found", username.c_str());
    return getDebugBuffer();
  }

  // Verify provided password matches the selected user locally
  if (!isValidUser(username, userPass)) {
    snprintf(getDebugBuffer(), 1024, "Error: Password incorrect for user '%s'", username.c_str());
    return getDebugBuffer();
  }
  
  // Get user role
  String role;
  if (!getUserRole(username, role)) {
    role = "user";  // Default if not found
  }
  
  // Resolve device MAC
  uint8_t targetMac[6];
  if (!userSyncResolveDeviceNameOrMac(deviceStr, targetMac)) {
    snprintf(getDebugBuffer(), 1024, "Error: Device '%s' not found in paired devices", deviceStr.c_str());
    return getDebugBuffer();
  }
  
  String deviceName = getEspNowDeviceName(targetMac);
  if (deviceName.length() == 0) {
    deviceName = deviceStr;
  }
  
  // Get admin credentials from current auth context
  String adminUser = currentAuthContext().user;
  
  if (adminUser.length() == 0) {
    return "Error: Not authenticated - admin login required";
  }
  
  // Verify admin privileges
  if (!isAdminUser(adminUser)) {
    return "Error: Admin privileges required for user sync";
  }
  // Re-confirm the admin password was re-entered (sensitive cross-device push of
  // a credential), and that the target device's admin credentials were provided.
  if (!isValidUser(adminUser, myAdminPass)) {
    return "Error: Your admin password is incorrect";
  }
  if (recvAdminUser.length() == 0 || recvAdminPass.length() == 0) {
    return "Error: Target device admin username and password required";
  }
  
  INFO_USERF("[USER_SYNC] Syncing user '%s' (role=%s) to device '%s'", 
             username.c_str(), role.c_str(), deviceName.c_str());
  
  // Build USER_SYNC message
  PSRAM_JSON_DOC(doc);
  uint32_t msgId = generateMessageId();
  
  // Get device name for source
  String myDeviceName = gSettings.espnowDeviceName;
  if (myDeviceName.length() == 0) {
    myDeviceName = "unknown";
  }
  
  // Build JSON payload (no V2 envelope, just the sync data)
  JsonObject payload = doc.to<JsonObject>();
  payload["recv_admin_user"] = recvAdminUser;
  payload["recv_admin_pass"] = recvAdminPass;
  payload["target_user"] = username;
  payload["target_pass"] = userPass;
  payload["role"] = role;
  payload["src_device"] = myDeviceName;
  payload["dst_device"] = deviceName;
  
  // Serialize to string
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  // Send via V4 binary protocol
  extern bool v4_send_user_sync(const uint8_t* dst, const char* jsonPayload, uint16_t jsonLen);
  if (!v4_send_user_sync(targetMac, jsonStr.c_str(), jsonStr.length())) {
    ERROR_USERF("[USER_SYNC] Failed to send sync message to %s", deviceName.c_str());
    snprintf(getDebugBuffer(), 1024, "Error: Failed to send user sync to '%s'", deviceName.c_str());
    return getDebugBuffer();
  }
  
  INFO_USERF("[USER_SYNC] ✓ Sent user '%s' to device '%s' (msgId=%lu)", 
             username.c_str(), deviceName.c_str(), (unsigned long)msgId);
  
  snprintf(getDebugBuffer(), 1024, "User sync sent: '%s' → '%s' (role=%s)", 
           username.c_str(), deviceName.c_str(), role.c_str());
  return getDebugBuffer();
}

#else
// Stub when ESP-NOW is disabled
const char* cmd_user_sync(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "Error: ESP-NOW not enabled";
}
#endif // ENABLE_ESPNOW
