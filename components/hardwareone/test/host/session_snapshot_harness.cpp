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

static unsigned checks, criticalDepth, criticalEntries, waits, notifications;
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
// FreeRTOS mutex stand-in for the lifecycle writers. A take on a held writer
// models the blocking wait: the registered hook plays the holder and must
// release before the take returns. The harness is one task, so a take on a
// writer this task already holds is the self-deadlock it would be on the
// device, and a give must match an outstanding take.
struct StaticSemaphore_t { bool held = false; };
using SemaphoreHandle_t = StaticSemaphore_t*;
using TickType_t = uint32_t;
static constexpr TickType_t portMAX_DELAY = 0xffffffffu;
static constexpr int pdTRUE = 1;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  *storage = {}; return storage;
}
static std::function<void()> waitHook;
static int xSemaphoreTake(SemaphoreHandle_t writer, TickType_t wait) {
  CHECK(wait == portMAX_DELAY && criticalDepth == 0);
  if (writer->held) {
    CHECK(waits == 0 && static_cast<bool>(waitHook));
    ++waits;
    auto hook = std::move(waitHook); waitHook = {}; hook();
    CHECK(!writer->held);
  }
  writer->held = true;
  return pdTRUE;
}
static int xSemaphoreGive(SemaphoreHandle_t writer) {
  CHECK(writer->held && criticalDepth == 0);
  writer->held = false;
  return pdTRUE;
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
static SemaphoreHandle_t writer(CommandSource source) {
  return source == SOURCE_SERIAL ? sSerialSessionWriter : sDisplaySessionWriter;
}
// Another task holding or releasing a writer, outside the production code.
static void hold(CommandSource source) { CHECK(!writer(source)->held); writer(source)->held = true; }
static void release(CommandSource source) { CHECK(writer(source)->held); writer(source)->held = false; }
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
static bool writerIsHeld(CommandSource source) { return writer(source)->held; }
static void reset() {
  CHECK(criticalDepth == 0);
  for (auto& slot : sTransportSessions) slot = {};
  sSerialSession = {}; sDisplaySession = {}; sTransportSessionGeneration = 0;
  sSerialSessionWriter->held = sDisplaySessionWriter->held = false;
  sTransportSessionMux = {}; sLegacySessionShadowMux = {};
  gSettings = {}; gSerialAuthed = true; gSerialUser = "mirror-sentinel";
  gSerialLastInteractionMs.store(371); gLocalDisplayLastInteractionMs.store(911);
  notifications = 7; waits = criticalEntries = 0; waitHook = {};
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
            << occupied() << '|' << waits << '|' << criticalEntries << '\n';
}
static void noIdentity(const ReadResult& result, TransportSessionEpoch epoch = 0) {
  CHECK(result.epoch == epoch && result.user.length() == 0 && !result.authed);
}
static void namedIdentity(const ReadResult& result, TransportSessionEpoch epoch, const char* name) {
  CHECK(result.epoch == epoch && result.user == name && result.authed);
}
static void requiredAndBypass(CommandSource source) {
  reset();
  hold(source); // Fast path must leave an existing writer untouched.
  auto result = read(source); noIdentity(result);
  CHECK(writerIsHeld(source) && waits == 0 && occupied() == 0 && sTransportSessionGeneration == 0);
  release(source); record("required-empty", source, result);

  reset(); policy(source) = false; policy(otherSource(source)) = true;
  hold(otherSource(source)); // Prove use of this source's lock.
  result = read(source); noIdentity(result, 1);
  CHECK(occupied() == 1 && sTransportSessions[0].source == source);
  CHECK(!writerIsHeld(source) && writerIsHeld(otherSource(source)));
  CHECK(transportSessionEpochIsLive(source, result.epoch));
  CHECK(!transportSessionEpochIsLive(otherSource(source), result.epoch));
  auto again = read(source); noIdentity(again, result.epoch);
  CHECK(sTransportSessionGeneration == 1 && occupied() == 1);
  release(otherSource(source)); record("bypass-once", source, result);

  // An existing bypass epoch remains visible even after policy changed. Its
  // authority is separately decided by liveness/policy-change owners.
  policy(source) = true;
  hold(source);
  result = read(source); noIdentity(result, 1);
  CHECK(writerIsHeld(source) && !transportSessionEpochIsLive(source, result.epoch));
  release(source); record("bypass-now-requires-auth", source, result);
}
static void existingNamed(CommandSource source) {
  for (bool requireAuth : {false, true}) {
    reset(); policy(source) = requireAuth;
    const auto epoch = transportSessionOpen(source);
    publishLegacyShadow(shadow(source), epoch, "fixture-user", true);
    hold(source);
    const auto result = read(source); namedIdentity(result, epoch, "fixture-user");
    CHECK(writerIsHeld(source) && waits == 0 && occupied() == 1);
    CHECK(transportSessionEpochIsLive(source, epoch));
    release(source); record(requireAuth ? "named-required" : "named-bypass", source, result);
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
  reset(); policy(source) = false; hold(source);
  waitHook = [source] { policy(source) = true; release(source); };
  auto result = read(source); noIdentity(result);
  CHECK(waits == 1 && occupied() == 0 && sTransportSessionGeneration == 0 && !writerIsHeld(source));
  record("waiting-policy-change", source, result);

  for (bool named : {false, true}) {
    reset(); policy(source) = false; hold(source);
    TransportSessionEpoch successor = 0;
    waitHook = [source, named, &successor] {
      successor = transportSessionOpen(source);
      publishLegacyShadow(shadow(source), successor, named ? "successor" : "", named);
      policy(source) = true;
      release(source);
    };
    result = read(source);
    if (named) namedIdentity(result, successor, "successor");
    else noIdentity(result, successor);
    CHECK(waits == 1 && occupied() == 1 && sTransportSessionGeneration == successor && !writerIsHeld(source));
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
  { String shadowUser; bool shadowAuthed = true;
    CHECK(snapshotLegacyShadow(sSerialSession, shadowUser, shadowAuthed) == epoch && !shadowAuthed);
    CHECK(!transportSessionEpochIsLive(SOURCE_SERIAL, epoch)); }
  const auto result = read(SOURCE_SERIAL); noIdentity(result, epoch);
  CHECK(serialTransportInputEpoch() == epoch && occupied() == 1 && !writerIsHeld(SOURCE_SERIAL));
  record("serial-preauth-input", SOURCE_SERIAL, result);
  reset(); hold(SOURCE_SERIAL);
  waitHook = [] {
    const auto successor = transportSessionOpen(SOURCE_SERIAL);
    publishLegacyShadow(sSerialSession, successor, "preauth-successor", true);
    release(SOURCE_SERIAL);
  };
  CHECK(serialTransportInputEpoch() == 1 && waits == 1 && occupied() == 1);
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
static void deliveryWindows(CommandSource source) {
  // Begin returns with the writer held only for the live epoch; End releases
  // it. A stale or zero epoch leaves the writer free, and a Begin against a
  // writer another task holds waits for that holder instead of spinning.
  reset(); policy(source) = false;
  const auto epoch = transportSessionOpen(source);
  publishLegacyShadow(shadow(source), epoch, "window-user", true);
  auto begin = [source](TransportSessionEpoch candidate) {
    return source == SOURCE_SERIAL ? serialTransportSessionBeginDelivery(candidate)
                                   : localDisplayTransportSessionBeginDelivery(candidate);
  };
  auto end = [source] {
    if (source == SOURCE_SERIAL) serialTransportSessionEndDelivery();
    else localDisplayTransportSessionEndDelivery();
  };
  CHECK(begin(epoch) && writerIsHeld(source) && !writerIsHeld(otherSource(source)));
  end(); CHECK(!writerIsHeld(source));
  CHECK(!begin(epoch + 1) && !writerIsHeld(source));
  CHECK(!begin(0) && !writerIsHeld(source) && waits == 0);
  hold(source); waitHook = [source] { release(source); };
  CHECK(begin(epoch) && waits == 1 && writerIsHeld(source));
  end(); CHECK(!writerIsHeld(source));
  record("delivery-window", source, read(source));
}
static void serialClearIfEpoch() {
  // The serial logout helper clears only the expected incarnation and returns
  // with the writer released; the caller prints its acknowledgement afterwards.
  reset(); CHECK(gSettings.serialRequireAuth);
  const auto epoch = transportSessionOpen(SOURCE_SERIAL);
  publishLegacyShadow(sSerialSession, epoch, "clear-user", true);
  CHECK(!serialTransportSessionClearIfEpoch(epoch + 1) && !writerIsHeld(SOURCE_SERIAL));
  CHECK(!serialTransportSessionClearIfEpoch(0) && !writerIsHeld(SOURCE_SERIAL));
  namedIdentity(read(SOURCE_SERIAL), epoch, "clear-user");
  CHECK(occupied() == 1 && transportSessionEpochIsLive(SOURCE_SERIAL, epoch));
  unchangedSideEffects();
  CHECK(serialTransportSessionClearIfEpoch(epoch));
  CHECK(!writerIsHeld(SOURCE_SERIAL) && occupied() == 0 && waits == 0);
  CHECK(!transportSessionEpochIsLive(SOURCE_SERIAL, epoch));
  CHECK(!gSerialAuthed && gSerialUser.length() == 0 && gSerialLastInteractionMs.load() == 0);
  noIdentity(read(SOURCE_SERIAL));
  CHECK(!serialTransportSessionClearIfEpoch(epoch) && !writerIsHeld(SOURCE_SERIAL));
  std::cout << "serial-clear-if-epoch|" << static_cast<int>(SOURCE_SERIAL) << '|' << epoch << '|'
            << occupied() << '|' << waits << '|' << criticalEntries << '\n';
}
int main() {
  for (const auto source : {SOURCE_SERIAL, SOURCE_LOCAL_DISPLAY}) {
    requiredAndBypass(source); existingNamed(source); fullRegistry(source); changedWhileWaiting(source);
    slowPathWriterFence(source); deliveryWindows(source);
  }
  independentSources(); serialPreAuth(); displayUiSync(); serialClearIfEpoch();
  std::cerr << "checks=" << checks << '\n';
}
