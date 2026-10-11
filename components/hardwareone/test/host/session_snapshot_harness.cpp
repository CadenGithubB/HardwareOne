#include <Arduino.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

void webBatchTestStringAppend() {}
// INSERT_TYPES

static unsigned checks, criticalDepth, criticalEntries, yields, notifications;
static void checkAt(bool good, int line) {
  ++checks;
  if (!good) std::fprintf(stderr, "session snapshot assertion line %d\n", line);
  assert(good);
}
#define CHECK(condition) checkAt((condition), __LINE__)
struct portMUX_TYPE { bool locked = false; };
#define portMUX_INITIALIZER_UNLOCKED {}
static std::function<void(portMUX_TYPE*)> criticalEntryHook;
static void portENTER_CRITICAL(portMUX_TYPE* mux) {
  if (criticalEntryHook) criticalEntryHook(mux);
  CHECK(!mux->locked && criticalDepth == 0);
  mux->locked = true; ++criticalDepth; ++criticalEntries;
}
static void portEXIT_CRITICAL(portMUX_TYPE* mux) {
  CHECK(mux->locked && criticalDepth == 1);
  mux->locked = false; --criticalDepth;
}
static size_t sessionStringCopy(char* destination, const char* source, size_t capacity) {
  const size_t length = std::strlen(source);
  if (capacity) { std::memcpy(destination, source, std::min(length, capacity - 1)); destination[std::min(length, capacity - 1)] = '\0'; }
  return length;
}
static std::function<void()> yieldHook;
static void taskYIELD() {
  CHECK(criticalDepth == 0 && yields == 0 && static_cast<bool>(yieldHook));
  ++yields;
  auto hook = std::move(yieldHook); yieldHook = {}; hook();
}
static struct { bool serialRequireAuth = true, localDisplayRequireAuth = true; } gSettings;
static bool gSerialAuthed;
static String gSerialUser;
static std::atomic<unsigned long> gSerialLastInteractionMs, gLocalDisplayLastInteractionMs;
static unsigned long sessionStampNow() { return 1234; }
static void oledNotifyLocalDisplayAuthChanged() { ++notifications; }
static TransportSessionEpoch uartLinkTransportSessionEpoch() { return 0; }
static bool bleSessionEpochIsLive(TransportSessionEpoch) { return false; }
TransportSessionEpoch transportSessionOpen(CommandSource source);

#define strlcpy sessionStringCopy
// INSERT_PRODUCTION
#undef strlcpy

static LegacySessionShadow& shadow(CommandSource source) {
  CHECK(source == SOURCE_SERIAL || source == SOURCE_LOCAL_DISPLAY);
  return source == SOURCE_SERIAL ? sSerialSession : sDisplaySession;
}
static std::atomic_flag& writer(CommandSource source) {
  return source == SOURCE_SERIAL ? sSerialSessionWriter : sDisplaySessionWriter;
}
static bool& policy(CommandSource source) {
  return source == SOURCE_SERIAL ? gSettings.serialRequireAuth : gSettings.localDisplayRequireAuth;
}
static CommandSource otherSource(CommandSource source) {
  return source == SOURCE_SERIAL ? SOURCE_LOCAL_DISPLAY : SOURCE_SERIAL;
}
struct ReadResult { TransportSessionEpoch epoch; String user; bool authed; };
static ReadResult read(CommandSource source) {
  ReadResult result{0, "stale-caller", true};
  result.epoch = source == SOURCE_SERIAL
      ? serialTransportSessionSnapshot(result.user, result.authed)
      : localDisplayTransportSessionSnapshot(result.user, result.authed);
  CHECK(criticalDepth == 0); return result;
}
static size_t occupied() {
  size_t count = 0;
  for (const auto& slot : sTransportSessions) if (slot.epoch) ++count;
  return count;
}
static bool writerIsHeld(CommandSource source) {
  const bool held = writer(source).test_and_set(std::memory_order_acquire);
  if (!held) writer(source).clear(std::memory_order_release);
  return held;
}
static void reset() {
  CHECK(criticalDepth == 0);
  for (auto& slot : sTransportSessions) slot = {};
  sSerialSession = {}; sDisplaySession = {}; sTransportSessionGeneration = 0;
  sSerialSessionWriter.clear(); sDisplaySessionWriter.clear();
  sTransportSessionMux = {}; sLegacySessionShadowMux = {};
  gSettings = {}; gSerialAuthed = true; gSerialUser = "mirror-sentinel";
  gSerialLastInteractionMs.store(371); gLocalDisplayLastInteractionMs.store(911);
  notifications = 7; yields = criticalEntries = 0; yieldHook = {};
  criticalEntryHook = {};
}
static void unchangedSideEffects() {
  CHECK(gSerialAuthed && gSerialUser == "mirror-sentinel");
  CHECK(gSerialLastInteractionMs.load() == 371 && gLocalDisplayLastInteractionMs.load() == 911);
  CHECK(notifications == 7 && criticalDepth == 0);
}
static void record(const char* label, CommandSource source, const ReadResult& result) {
  unchangedSideEffects();
  std::cout << label << '|' << static_cast<int>(source) << '|' << result.epoch << '|'
            << result.user.c_str() << '|' << result.authed << '|' << sTransportSessionGeneration << '|'
            << occupied() << '|' << yields << '|' << criticalEntries << '\n';
}
static void noIdentity(const ReadResult& result, TransportSessionEpoch epoch = 0) {
  CHECK(result.epoch == epoch && result.user.length() == 0 && !result.authed);
}
static void namedIdentity(const ReadResult& result, TransportSessionEpoch epoch, const char* name) {
  CHECK(result.epoch == epoch && result.user == name && result.authed);
}
static void requiredAndBypass(CommandSource source) {
  reset();
  CHECK(!writer(source).test_and_set()); // Fast path must leave an existing writer untouched.
  auto result = read(source); noIdentity(result);
  CHECK(writerIsHeld(source) && yields == 0 && occupied() == 0 && sTransportSessionGeneration == 0);
  writer(source).clear(); record("required-empty", source, result);

  reset(); policy(source) = false; policy(otherSource(source)) = true;
  CHECK(!writer(otherSource(source)).test_and_set()); // Prove use of this source's lock.
  result = read(source); noIdentity(result, 1);
  CHECK(occupied() == 1 && sTransportSessions[0].source == source);
  CHECK(!writerIsHeld(source) && writerIsHeld(otherSource(source)));
  CHECK(transportSessionEpochIsLive(source, result.epoch));
  CHECK(!transportSessionEpochIsLive(otherSource(source), result.epoch));
  auto again = read(source); noIdentity(again, result.epoch);
  CHECK(sTransportSessionGeneration == 1 && occupied() == 1);
  writer(otherSource(source)).clear(); record("bypass-once", source, result);

  // An existing bypass epoch remains visible even after policy changed. Its
  // authority is separately decided by liveness/policy-change owners.
  policy(source) = true;
  CHECK(!writer(source).test_and_set());
  result = read(source); noIdentity(result, 1);
  CHECK(writerIsHeld(source) && !transportSessionEpochIsLive(source, result.epoch));
  writer(source).clear(); record("bypass-now-requires-auth", source, result);
}
static void existingNamed(CommandSource source) {
  for (bool requireAuth : {false, true}) {
    reset(); policy(source) = requireAuth;
    const auto epoch = transportSessionOpen(source);
    publishLegacyShadow(shadow(source), epoch, "fixture-user", true);
    CHECK(!writer(source).test_and_set());
    const auto result = read(source); namedIdentity(result, epoch, "fixture-user");
    CHECK(writerIsHeld(source) && yields == 0 && occupied() == 1);
    CHECK(transportSessionEpochIsLive(source, epoch));
    writer(source).clear(); record(requireAuth ? "named-required" : "named-bypass", source, result);
  }
  reset(); policy(source) = false;
  // A malformed zero-epoch shadow must never publish authed=true. A new
  // anonymous generation must replace both stale identity outputs.
  publishLegacyShadow(shadow(source), 0, "stale-shadow", true);
  const auto result = read(source); noIdentity(result, 1);
  record("zero-epoch-stale-shadow", source, result);
}
static void fullRegistry(CommandSource source) {
  reset(); policy(source) = false;
  std::vector<TransportSessionEpoch> slots;
  for (size_t i = 0; i < kTransportSessionSlots; ++i) slots.push_back(transportSessionOpen(SOURCE_INTERNAL));
  const auto generation = sTransportSessionGeneration;
  auto result = read(source); noIdentity(result);
  CHECK(!writerIsHeld(source) && occupied() == kTransportSessionSlots && sTransportSessionGeneration == generation);
  record("registry-full", source, result);
  transportSessionClose(SOURCE_INTERNAL, slots[3]);
  result = read(source); noIdentity(result, generation + 1);
  CHECK(!writerIsHeld(source) && occupied() == kTransportSessionSlots);
  CHECK(transportSessionEpochIsLive(source, result.epoch));
  record("registry-retry", source, result);
}
static void changedWhileWaiting(CommandSource source) {
  reset(); policy(source) = false; CHECK(!writer(source).test_and_set());
  yieldHook = [source] { policy(source) = true; writer(source).clear(std::memory_order_release); };
  auto result = read(source); noIdentity(result);
  CHECK(yields == 1 && occupied() == 0 && sTransportSessionGeneration == 0 && !writerIsHeld(source));
  record("waiting-policy-change", source, result);

  for (bool named : {false, true}) {
    reset(); policy(source) = false; CHECK(!writer(source).test_and_set());
    TransportSessionEpoch successor = 0;
    yieldHook = [source, named, &successor] {
      successor = transportSessionOpen(source);
      publishLegacyShadow(shadow(source), successor, named ? "successor" : "", named);
      policy(source) = true;
      writer(source).clear(std::memory_order_release);
    };
    result = read(source);
    if (named) namedIdentity(result, successor, "successor");
    else noIdentity(result, successor);
    CHECK(yields == 1 && occupied() == 1 && sTransportSessionGeneration == successor && !writerIsHeld(source));
    CHECK(transportSessionEpochIsLive(source, result.epoch) == named);
    record(named ? "waiting-named-successor" : "waiting-bypass-successor", source, result);
  }
}
static void slowPathWriterFence(CommandSource source) {
  reset(); policy(source) = false;
  std::vector<portMUX_TYPE*> entries;
  std::vector<bool> held;
  criticalEntryHook = [source, &entries, &held](portMUX_TYPE* mux) {
    entries.push_back(mux); held.push_back(writerIsHeld(source));
  };
  const auto result = read(source);
  criticalEntryHook = {};
  noIdentity(result, 1);
  CHECK((entries == std::vector<portMUX_TYPE*>{&sLegacySessionShadowMux, &sLegacySessionShadowMux,
                                             &sTransportSessionMux, &sLegacySessionShadowMux}));
  // The first snapshot needs no lifecycle writer. The guarded re-read,
  // registry allocation and publication must all keep this source's writer.
  CHECK((held == std::vector<bool>{false, true, true, true}));
  CHECK(!writerIsHeld(source));
  record("slow-path-writer-fence", source, result);
}
static void independentSources() {
  reset(); gSettings.serialRequireAuth = gSettings.localDisplayRequireAuth = false;
  const auto serial = read(SOURCE_SERIAL), display = read(SOURCE_LOCAL_DISPLAY);
  noIdentity(serial, 1); noIdentity(display, 2); CHECK(occupied() == 2);
  CHECK(transportSessionEpochIsLive(SOURCE_SERIAL, serial.epoch));
  CHECK(transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, display.epoch));
  CHECK(!transportSessionEpochIsLive(SOURCE_SERIAL, display.epoch));
  CHECK(!transportSessionEpochIsLive(SOURCE_LOCAL_DISPLAY, serial.epoch));
  record("independent-serial", SOURCE_SERIAL, serial); record("independent-display", SOURCE_LOCAL_DISPLAY, display);
}
static void serialPreAuth() {
  reset(); CHECK(gSettings.serialRequireAuth);
  const auto epoch = serialTransportInputEpoch(); CHECK(epoch != 0);
  CHECK(serialTransportInputEpochIsCurrent(epoch) && !transportSessionEpochIsLive(SOURCE_SERIAL, epoch));
  const auto result = read(SOURCE_SERIAL); noIdentity(result, epoch);
  CHECK(serialTransportInputEpoch() == epoch && occupied() == 1 && !writerIsHeld(SOURCE_SERIAL));
  record("serial-preauth-input", SOURCE_SERIAL, result);
  reset(); CHECK(!sSerialSessionWriter.test_and_set());
  yieldHook = [] {
    const auto successor = transportSessionOpen(SOURCE_SERIAL);
    publishLegacyShadow(sSerialSession, successor, "preauth-successor", true);
    sSerialSessionWriter.clear(std::memory_order_release);
  };
  CHECK(serialTransportInputEpoch() == 1 && yields == 1 && occupied() == 1);
  namedIdentity(read(SOURCE_SERIAL), 1, "preauth-successor");
  record("serial-input-successor", SOURCE_SERIAL, read(SOURCE_SERIAL));
}
static void displayUiSync() {
  for (bool requireAuth : {true, false}) {
    reset(); gSettings.localDisplayRequireAuth = requireAuth;
    ReadResult result{0, "stale-caller", true};
    result.epoch = localDisplayTransportSessionBeginUiSync(result.user, result.authed);
    noIdentity(result, requireAuth ? 0 : 1);
    CHECK(writerIsHeld(SOURCE_LOCAL_DISPLAY));
    localDisplayTransportSessionEndUiSync(); CHECK(!writerIsHeld(SOURCE_LOCAL_DISPLAY));
    record(requireAuth ? "ui-sync-empty-held" : "ui-sync-bypass-held", SOURCE_LOCAL_DISPLAY, result);
  }
  reset(); gSettings.localDisplayRequireAuth = false;
  for (size_t i = 0; i < kTransportSessionSlots; ++i) CHECK(transportSessionOpen(SOURCE_INTERNAL) != 0);
  ReadResult result{0, "stale-caller", true};
  result.epoch = localDisplayTransportSessionBeginUiSync(result.user, result.authed);
  noIdentity(result); CHECK(writerIsHeld(SOURCE_LOCAL_DISPLAY));
  localDisplayTransportSessionEndUiSync(); CHECK(!writerIsHeld(SOURCE_LOCAL_DISPLAY));
  record("ui-sync-full-held", SOURCE_LOCAL_DISPLAY, result);
}
int main() {
  for (const auto source : {SOURCE_SERIAL, SOURCE_LOCAL_DISPLAY}) {
    requiredAndBypass(source); existingNamed(source); fullRegistry(source); changedWhileWaiting(source);
    slowPathWriterFence(source);
  }
  independentSources(); serialPreAuth(); displayUiSync();
  std::cerr << "checks=" << checks << '\n';
}
