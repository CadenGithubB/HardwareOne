// Network time synchronization. Shared clock state and post-step duties belong
// to System_Clock; this module owns SNTP setup, reply tracking, and waiting.
#include "System_NTP.h"

#if ENABLE_WIFI
#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>  // Do not also include lwip/apps/sntp.h in this translation unit.
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <time.h>

#include "System_Clock.h"
#include "System_Debug.h"
#include "System_Settings.h"
#include "OLED_Display.h"  // oledUpdate is safe when the display is disabled.
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"
#endif

extern void resolvePendingUserCreationTimes();
extern void writeBootAnchor();
extern void logTimeSyncedMarkerIfReady(const char* source);

// ---------------------------------------------------------------------------
// SNTP sync notification — the only honest "an NTP server actually replied"
// signal. lwIP's daemon steps the clock itself after a valid reply and can
// schedule later updates per CONFIG_LWIP_SNTP_UPDATE_DELAY. Before this
// callback existed those steps were completely invisible: no event, no log,
// and syncNTPAndResolve's old getLocalTime() "success" test couldn't tell a
// reply from a clock that was already set.
//
// The callback runs on lwIP's tcpip task (~3 KB stack): STORE-ONLY — no
// broadcast, no filesystem, no event post. ntpSyncDrainTick() (main loop,
// the ONLY caller) hands the stored facts to Clock::clockStepped(), which
// owns every post-step duty.
// ---------------------------------------------------------------------------
// Every field shared with lwIP's callback is covered by this mux. In
// particular, publishing the counter and drain flag together avoids losing a
// callback that lands while the main loop is clearing the previous one.
static portMUX_TYPE gSntpMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t gSntpSyncCount = 0;
static uint32_t gSntpForegroundTimeoutCount = 0;
static uint32_t gSntpLateReplyCount = 0;
static uint32_t gSntpLateNoticeReplyCount = 0;
static bool gSntpDrainPending = false;
static bool gSntpAwaitingLateReply = false;
static bool gSntpLateNoticePending = false;
static int64_t gSntpPrevProjUs = 0;
static int64_t gSntpLastReplyEpoch = 0;
static int64_t gSntpLastReplyMonotonicUs = 0;

static uint32_t beginSntpForegroundWait() {
  portENTER_CRITICAL(&gSntpMux);
  // A new explicit attempt supersedes an unanswered older wait, but never
  // erase a late-reply notice that a callback has already made pending.
  gSntpAwaitingLateReply = false;
  const uint32_t startCount = gSntpSyncCount;
  portEXIT_CRITICAL(&gSntpMux);
  return startCount;
}

static uint32_t sntpReplyCountSnapshot() {
  portENTER_CRITICAL(&gSntpMux);
  const uint32_t count = gSntpSyncCount;
  portEXIT_CRITICAL(&gSntpMux);
  return count;
}

// Atomically closes the deadline race. If the callback won the mux first,
// the caller observes a reply. If this transition wins, that callback is
// classified as late and the main-loop drain will announce it exactly once.
static bool markSntpForegroundTimeout(uint32_t startCount, bool clientArmed) {
  portENTER_CRITICAL(&gSntpMux);
  const bool noReply = (gSntpSyncCount == startCount);
  if (noReply) {
    ++gSntpForegroundTimeoutCount;
    gSntpAwaitingLateReply = clientArmed;
  }
  portEXIT_CRITICAL(&gSntpMux);
  return noReply;
}

static void onSntpTimeSync(struct timeval* tv) {
  // lwIP already stepped the real clock before invoking us; the projection
  // reference is the only remaining evidence of the PRE-step clock.
  const int64_t prev = Clock::projectedEpochUs();
  const int64_t replyEpoch = tv ? static_cast<int64_t>(tv->tv_sec) : 0;
  const int64_t replyMonotonicUs = esp_timer_get_time();
  Clock::refreshProjection();

  portENTER_CRITICAL(&gSntpMux);
  gSntpPrevProjUs = prev;
  gSntpLastReplyEpoch = replyEpoch;
  gSntpLastReplyMonotonicUs = replyMonotonicUs;
  ++gSntpSyncCount;
  gSntpDrainPending = true;
  if (gSntpAwaitingLateReply) {
    gSntpAwaitingLateReply = false;
    ++gSntpLateReplyCount;
    gSntpLateNoticeReplyCount = gSntpSyncCount;
    gSntpLateNoticePending = true;
  }
  portEXIT_CRITICAL(&gSntpMux);
}

NtpRuntimeStatus getNtpRuntimeStatus() {
  NtpRuntimeStatus status{};
  const int64_t nowUs = esp_timer_get_time();

  portENTER_CRITICAL(&gSntpMux);
  status.replyCount = gSntpSyncCount;
  status.foregroundTimeoutCount = gSntpForegroundTimeoutCount;
  status.lateReplyCount = gSntpLateReplyCount;
  status.lastReplyEpoch = gSntpLastReplyEpoch;
  status.awaitingLateReply = gSntpAwaitingLateReply;
  const int64_t lastReplyUs = gSntpLastReplyMonotonicUs;
  portEXIT_CRITICAL(&gSntpMux);

  status.clientArmed = esp_sntp_enabled();
  if (status.replyCount > 0 && lastReplyUs > 0 && nowUs >= lastReplyUs) {
    status.lastReplyAgeMs = static_cast<uint64_t>(nowUs - lastReplyUs) / 1000ULL;
  }
  return status;
}

void ntpSyncDrainTick() {
  bool drainPending = false;
  bool lateNoticePending = false;
  int64_t prevUs = 0;
  uint32_t lateReplyNumber = 0;

  portENTER_CRITICAL(&gSntpMux);
  if (gSntpDrainPending) {
    drainPending = true;
    prevUs = gSntpPrevProjUs;
    gSntpDrainPending = false;
  }
  if (gSntpLateNoticePending) {
    lateNoticePending = true;
    lateReplyNumber = gSntpLateNoticeReplyCount;
    gSntpLateNoticePending = false;
  }
  portEXIT_CRITICAL(&gSntpMux);

  if (!drainPending && !lateNoticePending) return;
  if (drainPending) {
    const time_t before = static_cast<time_t>(prevUs / 1000000LL);
    Clock::clockStepped(Clock::SYNC_NTP, before);
  }
  if (lateNoticePending) {
    char message[96];
    snprintf(message, sizeof(message),
             "[OK] NTP reply received after foreground timeout (reply #%lu)",
             static_cast<unsigned long>(lateReplyNumber));
    broadcastOutput(message);
  }
  // Duties (step log, edge event, marker, anchor, resolve, scheduler wake,
  // RTC write-back) drain via Clock::clockDutiesTick() on this same lap.
}

void setupNTP() {
  long gmtOffset = (long)Clock::tzOffsetMinutes() * 60;  // seconds
  DEBUG_NTP_SETUPF("[NTP Setup] Starting NTP configuration");
  DEBUG_NTP_SETUPF("[NTP Setup] Primary server: %s", gSettings.ntpServer.c_str());
  DEBUG_NTP_SETUPF("[NTP Setup] GMT offset: %ld seconds (%d minutes)", gmtOffset, Clock::tzOffsetMinutes());
  DEBUG_NTP_SETUPF("[NTP Setup] WiFi status: %s", WiFi.isConnected() ? "CONNECTED" : "DISCONNECTED");
  if (WiFi.isConnected()) {
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi IP: %s", WiFi.localIP().toString().c_str());
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi gateway: %s", WiFi.gatewayIP().toString().c_str());
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi DNS: %s", WiFi.dnsIP().toString().c_str());
    DEBUG_NTP_SETUPF("[NTP Setup] WiFi subnet: %s", WiFi.subnetMask().toString().c_str());
  }

  // Register the reply callback BEFORE (re)starting SNTP. Registration is a
  // bare static-pointer store inside lwIP that survives sntp_stop/init
  // cycles; re-registering the same function is harmless.
  sntp_set_time_sync_notification_cb(&onSntpTimeSync);

  // Seed the projection so the FIRST background reply on a warm boot (clock
  // carried across a soft reboot) doesn't reconstruct a garbage "before".
  Clock::refreshProjection();

  // lwIP stores server-name POINTERS (sntp_setservername does not copy), so
  // the primary must live in storage that never moves. gSettings.ntpServer
  // is an Arduino String whose buffer reallocates on assignment — hand lwIP
  // a stable copy instead. Refreshed on every setupNTP(), which every
  // ntpserver-change path already calls.
  static char sNtpServer[64];
  strlcpy(sNtpServer, gSettings.ntpServer.c_str(), sizeof(sNtpServer));

  // configTime forwards all three names. lwIP uses as many as the board's
  // CONFIG_LWIP_SNTP_MAX_SERVERS provides (the current XIAO build provides
  // all three); any extras remain harmless on a smaller-slot build.
  configTime(gmtOffset, 0,
             sNtpServer,             // Primary (usually pool.ntp.org)
             "time.google.com",      // Backup (needs MAX_SERVERS >= 2)
             "time.cloudflare.com"); // Backup (needs MAX_SERVERS >= 3)

  // configTime()'s trailing setTimeZone() rewrites TZ from its numeric
  // offset (a rule-less "UTC<h>DST<h>" string). Re-assert the canonical
  // settings-derived TZ so this call can never clobber it — this is also
  // the hook that keeps a future DST rule alive across NTP restarts.
  Clock::applyTimezone();

  DEBUG_NTP_SETUPF("[NTP Setup] configTime() done; primary=%s, server slots=%d",
                   sNtpServer, CONFIG_LWIP_SNTP_MAX_SERVERS);
}

bool syncNTPAndResolve(NtpSyncOutcome* outcomeOut) {
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Starting NTP sync process");
  if (outcomeOut) *outcomeOut = NtpSyncOutcome::Failed;

  if (!WiFi.isConnected()) {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] FAILED - WiFi not connected");
    broadcastOutput("NTP sync requires WiFi connection");
    return false;
  }

  DEBUG_NTP_SYNCF("[syncNTPAndResolve] WiFi connected, proceeding with NTP sync");
  // No DNS pre-flight: lwIP's SNTP client resolves names itself, and the
  // wait loop below detects failure. (A synchronous hostByName probe here
  // used to add 5-8 s of dead wait on healthy networks.)

  // Capture the pre-attempt state BEFORE setupNTP restarts the daemon: the
  // success test below is "did the reply counter advance", and a reply can
  // land between sntp_init and our first poll.
  const time_t preSyncTime = time(nullptr);
  const bool preSet = Clock::isValidEpoch(preSyncTime);
  const uint32_t startCount = beginSntpForegroundWait();

  broadcastOutput("Synchronizing time with NTP server...");
  setupNTP();
  broadcastOutput("  Contacting NTP server, please wait...");

  // Success = an actual SNTP reply (the notification callback fired), NOT
  // "the clock looks set" — getLocalTime() is a clock-is-set test, and with
  // RTC/ring/warm-reboot sources able to pre-set the clock it reported
  // "NTP synchronized" without a single packet coming back.
  //
  // Wait budget: a dark boot has nothing better to do than wait the full
  // window; a boot whose clock is already valid shouldn't stall the caller.
  // Reaching the deadline proves only "no reply during this wait". The live
  // client state is reported separately as armed/stopped below.
  bool ntpSynced = false;
  const int maxWaitSeconds = preSet ? 7 : 15;
  const int iterationsPerSecond = 10;  // 100ms per iteration
  const int maxIterations = maxWaitSeconds * iterationsPerSecond;
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Starting %d-second wait loop for NTP reply", maxWaitSeconds);

  for (int i = 0; i < maxIterations; i++) {
    delay(100);
    oledUpdate();  // Keep boot animation running during NTP wait

    if (i > 0 && i % iterationsPerSecond == 0) {
      char progressMsg[64];
      snprintf(progressMsg, sizeof(progressMsg), "  Looking for updates... %d/%d seconds", i / iterationsPerSecond, maxWaitSeconds);
      broadcastOutput(progressMsg);
    }

    const uint32_t replyCount = sntpReplyCountSnapshot();
    if (replyCount != startCount) {
      DEBUG_NTP_SYNCF("[syncNTPAndResolve] SNTP reply received (count %lu)",
                      static_cast<unsigned long>(replyCount));
      ntpSynced = true;
      break;
    }
  }

  // Close the final-check race under the same mux as the callback. A callback
  // that wins first turns this into foreground success; one that arrives
  // after this transition is counted and announced as a late reply.
  const bool clientArmedAtDeadline = esp_sntp_enabled();
  if (!ntpSynced) {
    ntpSynced = !markSntpForegroundTimeout(startCount, clientArmedAtDeadline);
  }

  if (ntpSynced) {
    DEBUG_NTP_SYNCF("[syncNTPAndResolve] NTP sync completed successfully");
    broadcastOutput("[OK] NTP time synchronized");
    // Post-step duties (edge event, marker, anchor, resolve, scheduler wake,
    // RTC write-back, step diagnostic) all arrive via ntpSyncDrainTick →
    // Clock::clockStepped on the next main-loop lap — do NOT duplicate here.
    //
    // One deliberate exception, the warm-boot baseline: when the clock was
    // ALREADY valid (carried across a soft reboot), the helper sees no
    // invalid→valid edge and a small correction pends nothing — but THIS
    // boot still needs its anchor (gNTPAnchorId bumps every boot, and
    // same-boot users can only resolve against a same-boot anchor). The
    // anchor write is an upsert, so this is idempotent with the drain.
    if (preSet) {
      writeBootAnchor();
      resolvePendingUserCreationTimes();
      logTimeSyncedMarkerIfReady("ntp");
    }
    if (outcomeOut) *outcomeOut = NtpSyncOutcome::Reply;
    return true;
  }

  DEBUG_NTP_SYNCF("[syncNTPAndResolve] no SNTP reply after %d seconds", maxWaitSeconds);
  DEBUG_NTP_SYNCF("[syncNTPAndResolve] Check: WiFi=%s, DNS=%s, Gateway=%s",
                  WiFi.isConnected() ? "OK" : "FAIL",
                  WiFi.dnsIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());

  if (clientArmedAtDeadline) {
    BROADCAST_PRINTF("[INFO] No NTP reply within %d seconds; SNTP client remains armed",
                     maxWaitSeconds);
  } else {
    BROADCAST_PRINTF("[WARN] No NTP reply within %d seconds; SNTP client is stopped",
                     maxWaitSeconds);
  }

  if (preSet) {
    // No reply yet, but a prior source already supplied a valid clock. Keep
    // that distinct from both an NTP success and the armed client state.
    broadcastOutput("[OK] Clock kept its existing time");
    writeBootAnchor();
    resolvePendingUserCreationTimes();
    // Marker names WHO supplied this boot's clock: the ledger when a real
    // source stepped it (rtc/ring/manual), "carryover" only for the true
    // soft-reboot case where no settimeofday ever happened.
    logTimeSyncedMarkerIfReady(Clock::syncSource() != Clock::SYNC_NONE
                                   ? Clock::syncSourceName(Clock::syncSource())
                                   : "carryover");
    if (outcomeOut) *outcomeOut = NtpSyncOutcome::KeptPrior;
    return true;
  }

  // Dark timeout — try the RTC as a fallback source. rtcSyncToSystem() runs
  // its duties through Clock::clockStepped itself now.
#if ENABLE_RTC_SENSOR
  if (gRtcRunning && gRtcConnected) {
    if (rtcSyncToSystem()) {
      broadcastOutput("[OK] System time set from RTC (no NTP reply in foreground window)");
      if (outcomeOut) *outcomeOut = NtpSyncOutcome::RtcFallback;
      return true;
    }
  }
#endif

  broadcastOutput("[ERROR] NTP sync timeout - no other time source");
  broadcastOutput("  Check internet access, DNS, and whether UDP port 123 is blocked");
  return false;
}

#endif // ENABLE_WIFI
