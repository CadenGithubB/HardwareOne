#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <new>

#ifdef NDEBUG
#error "event_catalog_json_tests require active runtime assertions"
#endif

#include "../../System_CommandLimits.h"
#include "../../System_EventCatalog.h"
#include "../../System_EventCatalogJson.h"
#include "../../System_EventCatalogJsonCore.h"

namespace json_core = hw1_event_catalog_json_core;

static std::atomic<size_t> gNewCalls{0};

void* operator new(size_t size) {
  gNewCalls.fetch_add(1, std::memory_order_relaxed);
  if (void* storage = malloc(size == 0 ? 1 : size)) return storage;
  throw std::bad_alloc();
}

void* operator new[](size_t size) {
  gNewCalls.fetch_add(1, std::memory_order_relaxed);
  if (void* storage = malloc(size == 0 ? 1 : size)) return storage;
  throw std::bad_alloc();
}

void operator delete(void* storage) noexcept { free(storage); }
void operator delete[](void* storage) noexcept { free(storage); }

#if defined(__cpp_sized_deallocation)
void operator delete(void* storage, size_t) noexcept { free(storage); }
void operator delete[](void* storage, size_t) noexcept { free(storage); }
#endif

namespace {

struct FixtureFamily {
  size_t key;
  json_core::TextView label;
  const json_core::TextView* kinds;
  size_t kindCount;
};

struct FixtureProvider {
  const FixtureFamily* families;
  size_t familyCount;
  bool failFamilyAt;
  bool failKindAt;
};

size_t fixtureFamilyCount(void* opaque) {
  return static_cast<FixtureProvider*>(opaque)->familyCount;
}

bool fixtureFamilyAt(void* opaque,
                     size_t index,
                     json_core::FamilyView* out) {
  FixtureProvider& provider = *static_cast<FixtureProvider*>(opaque);
  if (!out || provider.failFamilyAt || index >= provider.familyCount) {
    return false;
  }
  const FixtureFamily& family = provider.families[index];
  json_core::FamilyView resolved{};
  resolved.key = family.key;
  resolved.label = family.label;
  resolved.kindCount = family.kindCount;
  *out = resolved;
  return true;
}

bool fixtureFamilyKindAt(void* opaque,
                         size_t familyKey,
                         size_t index,
                         json_core::KindView* out) {
  FixtureProvider& provider = *static_cast<FixtureProvider*>(opaque);
  if (!out || provider.failKindAt) return false;
  for (size_t familyIndex = 0; familyIndex < provider.familyCount;
       ++familyIndex) {
    const FixtureFamily& family = provider.families[familyIndex];
    if (family.key != familyKey) continue;
    if (!family.kinds || index >= family.kindCount) return false;
    json_core::KindView resolved{};
    resolved.name = family.kinds[index];
    *out = resolved;
    return true;
  }
  return false;
}

json_core::ProviderView fixtureView(FixtureProvider& provider) {
  return json_core::ProviderView{&provider, fixtureFamilyCount,
                                 fixtureFamilyAt, fixtureFamilyKindAt};
}

struct FixedSink {
  char data[8192]{};
  size_t used = 0;
};

bool fixedSink(void* opaque, const char* data, size_t length) {
  FixedSink& sink = *static_cast<FixedSink*>(opaque);
  if (!data || length > sizeof(sink.data) - sink.used) return false;
  memcpy(sink.data + sink.used, data, length);
  sink.used += length;
  return true;
}

struct ProbeSink {
  size_t calls = 0;
  size_t accepted = 0;
  size_t failOnCall = 0;
};

bool probeSink(void* opaque, const char*, size_t length) {
  ProbeSink& sink = *static_cast<ProbeSink*>(opaque);
  ++sink.calls;
  if (sink.failOnCall != 0 && sink.calls == sink.failOnCall) return false;
  sink.accepted += length;
  return true;
}

json_core::EmitResult serializeFixture(json_core::ProviderView view,
                                       FixedSink& sink) {
  const json_core::EmitResult measured = json_core::measureJson(view);
  assert(measured.status == SystemEventCatalogJsonStatus::Ok);
  const json_core::EmitResult written =
      json_core::writeJson(view, fixedSink, &sink);
  assert(written.status == SystemEventCatalogJsonStatus::Ok);
  assert(written.bytesWritten == measured.bytesWritten);
  assert(sink.used == measured.bytesWritten);
  return written;
}

struct AppendBuffer {
  char* data;
  size_t capacity;
  size_t used;
};

void appendBytes(AppendBuffer& out, const char* data, size_t length) {
  assert(data != nullptr || length == 0);
  assert(length <= out.capacity - out.used);
  memcpy(out.data + out.used, data, length);
  out.used += length;
}

template <size_t N>
void appendLiteral(AppendBuffer& out, const char (&literal)[N]) {
  appendBytes(out, literal, N - 1u);
}

void appendCurrentCatalogText(AppendBuffer& out, const char* text) {
  assert(text != nullptr);
  const size_t length = strlen(text);
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = static_cast<uint8_t>(text[index]);
    assert(value >= UINT8_C(0x20));
    assert(value != static_cast<uint8_t>('"'));
    assert(value != static_cast<uint8_t>('\\'));
  }
  appendBytes(out, text, length);
}

size_t buildExpectedProductionJson(char* out, size_t capacity) {
  AppendBuffer expected{out, capacity, 0};
  appendLiteral(expected, "{\"families\":[");
  for (size_t familyIndex = 0;
       familyIndex < systemEventCatalogFamilyCount(); ++familyIndex) {
    SystemEventCatalogFamilyInfo family{};
    assert(systemEventCatalogFamilyAt(familyIndex, &family));
    if (familyIndex != 0) appendLiteral(expected, ",");
    appendLiteral(expected, "{\"n\":\"");
    appendCurrentCatalogText(expected, family.label);
    appendLiteral(expected, "\",\"k\":[");
    for (size_t kindIndex = 0; kindIndex < family.kindCount; ++kindIndex) {
      SystemEventCatalogKindInfo kind{};
      assert(systemEventCatalogFamilyKindAt(family.id, kindIndex, &kind));
      if (kindIndex != 0) appendLiteral(expected, ",");
      appendLiteral(expected, "\"");
      appendCurrentCatalogText(expected, kind.name);
      appendLiteral(expected, "\"");
    }
    appendLiteral(expected, "]}");
  }
  appendLiteral(expected, "]}");
  return expected.used;
}

void testProductionBytesAndSizing() {
  const size_t allocationsBefore =
      gNewCalls.load(std::memory_order_relaxed);
  const size_t jsonSize = systemEventCatalogJsonSize();
  assert(jsonSize > json_core::kStageCapacity);
  assert(jsonSize + 1u < CMD_RESULT_MAX);

  char serialized[CMD_RESULT_MAX]{};
  size_t bytesWritten = static_cast<size_t>(-1);
  size_t requiredCapacity = static_cast<size_t>(-1);
  assert(systemEventCatalogJsonToBuffer(serialized, jsonSize + 1u,
                                        &bytesWritten, &requiredCapacity) ==
         SystemEventCatalogJsonStatus::Ok);
  assert(bytesWritten == jsonSize);
  assert(requiredCapacity == jsonSize + 1u);
  assert(serialized[jsonSize] == '\0');

  char expected[CMD_RESULT_MAX]{};
  const size_t expectedSize =
      buildExpectedProductionJson(expected, sizeof(expected));
  assert(expectedSize == jsonSize);
  assert(memcmp(serialized, expected, jsonSize) == 0);

  FixedSink streamed{};
  bytesWritten = static_cast<size_t>(-1);
  assert(systemEventCatalogWriteJson(fixedSink, &streamed, &bytesWritten) ==
         SystemEventCatalogJsonStatus::Ok);
  assert(bytesWritten == jsonSize);
  assert(streamed.used == jsonSize);
  assert(memcmp(streamed.data, serialized, jsonSize) == 0);
  assert(gNewCalls.load(std::memory_order_relaxed) == allocationsBefore);
}

void testPublicOutputContracts() {
  const size_t jsonSize = systemEventCatalogJsonSize();
  assert(jsonSize != 0 && jsonSize < CMD_RESULT_MAX);
  char buffer[CMD_RESULT_MAX];

  memset(buffer, 'x', sizeof(buffer));
  size_t bytesWritten = 89;
  size_t requiredCapacity = 90;
  assert(systemEventCatalogJsonToBuffer(buffer, jsonSize + 1u,
                                        &bytesWritten, &requiredCapacity) ==
         SystemEventCatalogJsonStatus::Ok);
  assert(bytesWritten == jsonSize);
  assert(requiredCapacity == jsonSize + 1u);
  assert(buffer[jsonSize] == '\0');

  memset(buffer, 'x', sizeof(buffer));
  bytesWritten = 91;
  requiredCapacity = 92;
  assert(systemEventCatalogJsonToBuffer(buffer, jsonSize, &bytesWritten,
                                        &requiredCapacity) ==
         SystemEventCatalogJsonStatus::BufferTooSmall);
  assert(buffer[0] == '\0');
  assert(bytesWritten == 0);
  assert(requiredCapacity == jsonSize + 1u);

  char zeroCapacitySentinel = 'z';
  bytesWritten = 93;
  requiredCapacity = 94;
  assert(systemEventCatalogJsonToBuffer(&zeroCapacitySentinel, 0,
                                        &bytesWritten,
                                        &requiredCapacity) ==
         SystemEventCatalogJsonStatus::BufferTooSmall);
  assert(zeroCapacitySentinel == 'z');
  assert(bytesWritten == 0);
  assert(requiredCapacity == jsonSize + 1u);

  bytesWritten = 95;
  requiredCapacity = 96;
  assert(systemEventCatalogJsonToBuffer(nullptr, 0, &bytesWritten,
                                        &requiredCapacity) ==
         SystemEventCatalogJsonStatus::InvalidArgument);
  assert(bytesWritten == 0);
  assert(requiredCapacity == 0);

  bytesWritten = 97;
  requiredCapacity = 98;
  assert(systemEventCatalogJsonToBuffer(nullptr, jsonSize + 1u,
                                        &bytesWritten,
                                        &requiredCapacity) ==
         SystemEventCatalogJsonStatus::InvalidArgument);
  assert(bytesWritten == 0);
  assert(requiredCapacity == 0);

  bytesWritten = 99;
  assert(systemEventCatalogWriteJson(nullptr, nullptr, &bytesWritten) ==
         SystemEventCatalogJsonStatus::InvalidArgument);
  assert(bytesWritten == 0);
}

void testSinkFailureAccounting() {
  size_t bytesWritten = static_cast<size_t>(-1);
  ProbeSink firstFailure{};
  firstFailure.failOnCall = 1;
  assert(systemEventCatalogWriteJson(probeSink, &firstFailure,
                                     &bytesWritten) ==
         SystemEventCatalogJsonStatus::SinkFailed);
  assert(firstFailure.calls == 1);
  assert(firstFailure.accepted == 0);
  assert(bytesWritten == 0);

  ProbeSink laterFailure{};
  laterFailure.failOnCall = 2;
  bytesWritten = static_cast<size_t>(-1);
  assert(systemEventCatalogWriteJson(probeSink, &laterFailure,
                                     &bytesWritten) ==
         SystemEventCatalogJsonStatus::SinkFailed);
  assert(laterFailure.calls == 2);
  assert(laterFailure.accepted > 0);
  assert(bytesWritten == laterFailure.accepted);
  assert(bytesWritten < systemEventCatalogJsonSize());
}

void testQuoteBackslashSlashAndUtf8Escaping() {
  static const char labelBytes[] = {'A', '"', '\\', '/',
                                    static_cast<char>(0xC2),
                                    static_cast<char>(0xA2)};
  static const char kindBytes[] = {'q', '"', '\\', '/',
                                   static_cast<char>(0xC2),
                                   static_cast<char>(0xA2)};
  const json_core::TextView kinds[] = {
      json_core::TextView{kindBytes, sizeof(kindBytes)}};
  const FixtureFamily families[] = {
      FixtureFamily{7, json_core::TextView{labelBytes, sizeof(labelBytes)},
                    kinds, 1}};
  FixtureProvider provider{families, 1, false, false};
  FixedSink serialized{};
  serializeFixture(fixtureView(provider), serialized);

  static const char expected[] =
      "{\"families\":[{\"n\":\"A\\\"\\\\/\xC2\xA2\","
      "\"k\":[\"q\\\"\\\\/\xC2\xA2\"]}]}";
  assert(serialized.used == sizeof(expected) - 1u);
  assert(memcmp(serialized.data, expected, sizeof(expected) - 1u) == 0);
}

void testEveryC0Escape() {
  static const char* const expectedControls[32] = {
      nullptr,   "\\u0001", "\\u0002", "\\u0003", "\\u0004",
      "\\u0005", "\\u0006", "\\u0007", "\\b",     "\\t",
      "\\n",     "\\u000b", "\\f",     "\\r",     "\\u000e",
      "\\u000f", "\\u0010", "\\u0011", "\\u0012", "\\u0013",
      "\\u0014", "\\u0015", "\\u0016", "\\u0017", "\\u0018",
      "\\u0019", "\\u001a", "\\u001b", "\\u001c", "\\u001d",
      "\\u001e", "\\u001f",
  };

  for (uint8_t value = UINT8_C(0x01); value <= UINT8_C(0x1F); ++value) {
    const char byte = static_cast<char>(value);
    const json_core::TextView kinds[] = {
        json_core::TextView{&byte, 1}};
    const FixtureFamily families[] = {
        FixtureFamily{3, json_core::literalView("F"), kinds, 1}};
    FixtureProvider provider{families, 1, false, false};
    FixedSink serialized{};
    serializeFixture(fixtureView(provider), serialized);

    char expected[96]{};
    const int length = snprintf(
        expected, sizeof(expected),
        "{\"families\":[{\"n\":\"F\",\"k\":[\"%s\"]}]}",
        expectedControls[value]);
    assert(length > 0 && static_cast<size_t>(length) < sizeof(expected));
    assert(serialized.used == static_cast<size_t>(length));
    assert(memcmp(serialized.data, expected,
                  static_cast<size_t>(length)) == 0);
  }
}

void expectInvalidBeforeSink(json_core::ProviderView view) {
  ProbeSink probe{};
  const json_core::EmitResult written =
      json_core::writeJson(view, probeSink, &probe);
  assert(written.status == SystemEventCatalogJsonStatus::InvalidArgument);
  assert(written.bytesWritten == 0);
  assert(probe.calls == 0);

  const json_core::EmitResult measured = json_core::measureJson(view);
  assert(measured.status == SystemEventCatalogJsonStatus::InvalidArgument);
  assert(measured.bytesWritten == 0);
}

void testMalformedViewsFailBeforeOutput() {
  json_core::ProviderView missingCallbacks{};
  expectInvalidBeforeSink(missingCallbacks);

  FixtureProvider emptyProvider{nullptr, 0, false, false};
  expectInvalidBeforeSink(fixtureView(emptyProvider));

  const json_core::TextView goodKinds[] = {json_core::literalView("kind")};

  FixtureFamily goodFamily{1, json_core::literalView("Family"), goodKinds, 1};
  FixtureProvider failedFamily{&goodFamily, 1, true, false};
  expectInvalidBeforeSink(fixtureView(failedFamily));

  FixtureProvider failedKind{&goodFamily, 1, false, true};
  expectInvalidBeforeSink(fixtureView(failedKind));

  static const char embeddedNul[] = {'b', 'a', 'd', '\0', 'x'};
  const json_core::TextView nulKinds[] = {
      json_core::TextView{embeddedNul, sizeof(embeddedNul)}};
  const FixtureFamily nulFamily{
      2, json_core::literalView("Family"), nulKinds, 1};
  FixtureProvider nulProvider{&nulFamily, 1, false, false};
  expectInvalidBeforeSink(fixtureView(nulProvider));

  static const char invalidUtf8[] = {static_cast<char>(0xC0),
                                     static_cast<char>(0xAF)};
  const json_core::TextView invalidKinds[] = {
      json_core::TextView{invalidUtf8, sizeof(invalidUtf8)}};
  const FixtureFamily invalidFamily{
      3, json_core::literalView("Family"), invalidKinds, 1};
  FixtureProvider invalidProvider{&invalidFamily, 1, false, false};
  expectInvalidBeforeSink(fixtureView(invalidProvider));

  // A streaming validator would already have emitted the first kind here.
  // The real core must validate the complete view before its first callback.
  const json_core::TextView lateInvalidKinds[] = {
      json_core::literalView("good"),
      json_core::TextView{invalidUtf8, sizeof(invalidUtf8)}};
  const FixtureFamily lateInvalidFamily{
      31, json_core::literalView("Family"), lateInvalidKinds, 2};
  FixtureProvider lateInvalidProvider{
      &lateInvalidFamily, 1, false, false};
  expectInvalidBeforeSink(fixtureView(lateInvalidProvider));

  const json_core::TextView emptyKinds[] = {
      json_core::TextView{"", 0}};
  const FixtureFamily emptyKindFamily{
      4, json_core::literalView("Family"), emptyKinds, 1};
  FixtureProvider emptyKindProvider{&emptyKindFamily, 1, false, false};
  expectInvalidBeforeSink(fixtureView(emptyKindProvider));

  const FixtureFamily emptyLabelFamily{
      5, json_core::TextView{"", 0}, goodKinds, 1};
  FixtureProvider emptyLabelProvider{&emptyLabelFamily, 1, false, false};
  expectInvalidBeforeSink(fixtureView(emptyLabelProvider));

  const FixtureFamily nullStorageFamily{
      6, json_core::TextView{nullptr, 1}, goodKinds, 1};
  FixtureProvider nullStorageProvider{&nullStorageFamily, 1, false, false};
  expectInvalidBeforeSink(fixtureView(nullStorageProvider));
}

void runTests() {
  testProductionBytesAndSizing();
  testPublicOutputContracts();
  testSinkFailureAccounting();
  testQuoteBackslashSlashAndUtf8Escaping();
  testEveryC0Escape();
  testMalformedViewsFailBeforeOutput();
}

bool emitHex(const char* data, size_t length) {
  static constexpr char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = static_cast<uint8_t>(data[index]);
    if (fputc(hex[value >> 4u], stdout) == EOF ||
        fputc(hex[value & UINT8_C(0x0F)], stdout) == EOF) {
      return false;
    }
  }
  return true;
}

int dumpTypedCatalog() {
  for (size_t familyIndex = 0;
       familyIndex < systemEventCatalogFamilyCount(); ++familyIndex) {
    SystemEventCatalogFamilyInfo family{};
    if (!systemEventCatalogFamilyAt(familyIndex, &family) || !family.label) {
      return 1;
    }
    const size_t labelLength = strlen(family.label);
    if (printf("F %zu %zu %zu ", familyIndex, family.kindCount,
               labelLength) < 0 ||
        !emitHex(family.label, labelLength) || fputc('\n', stdout) == EOF) {
      return 1;
    }
    for (size_t kindIndex = 0; kindIndex < family.kindCount; ++kindIndex) {
      SystemEventCatalogKindInfo kind{};
      if (!systemEventCatalogFamilyKindAt(family.id, kindIndex, &kind) ||
          !kind.name) {
        return 1;
      }
      const size_t nameLength = strlen(kind.name);
      if (printf("K %zu %zu %zu ", familyIndex, kindIndex, nameLength) < 0 ||
          !emitHex(kind.name, nameLength) || fputc('\n', stdout) == EOF) {
        return 1;
      }
    }
  }
  return ferror(stdout) ? 1 : 0;
}

int dumpProductionJson() {
  char buffer[CMD_RESULT_MAX]{};
  size_t bytesWritten = 0;
  if (systemEventCatalogJsonToBuffer(buffer, sizeof(buffer), &bytesWritten,
                                     nullptr) !=
      SystemEventCatalogJsonStatus::Ok) {
    return 1;
  }
  return fwrite(buffer, 1, bytesWritten, stdout) == bytesWritten &&
                 !ferror(stdout)
             ? 0
             : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--dump-json") == 0) {
    return dumpProductionJson();
  }
  if (argc == 2 && strcmp(argv[1], "--dump-typed") == 0) {
    return dumpTypedCatalog();
  }
  if (argc != 1) {
    fprintf(stderr, "usage: %s [--dump-json|--dump-typed]\n", argv[0]);
    return 2;
  }
  runTests();
  puts("event catalog JSON tests passed");
  return 0;
}
