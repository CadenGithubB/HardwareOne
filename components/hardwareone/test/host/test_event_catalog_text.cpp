#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "event_catalog_text_tests require active runtime assertions"
#endif

#include "../../System_EventCatalog.h"
#include "../../System_EventCatalogTextCore.h"

namespace text_core = hw1_event_catalog_text_core;

struct FixtureKind {
  const char* name;
  size_t length;
};

struct FixtureFamily {
  size_t key;
  const char* label;
  size_t labelLength;
  const FixtureKind* kinds;
  size_t kindCount;
};

struct FixtureProvider {
  const FixtureFamily* families;
  size_t familyCount;
};

static size_t fixtureFamilyCount(void* context) {
  return static_cast<FixtureProvider*>(context)->familyCount;
}

static bool fixtureFamilyAt(void* context,
                            size_t index,
                            text_core::FamilyView* out) {
  FixtureProvider& provider = *static_cast<FixtureProvider*>(context);
  if (!out || index >= provider.familyCount) return false;
  const FixtureFamily& family = provider.families[index];
  out->key = family.key;
  out->label = {family.label, family.labelLength};
  out->kindCount = family.kindCount;
  return true;
}

static bool fixtureFamilyKindAt(void* context,
                                size_t familyKey,
                                size_t index,
                                text_core::KindView* out) {
  FixtureProvider& provider = *static_cast<FixtureProvider*>(context);
  if (!out) return false;
  for (size_t familyIndex = 0; familyIndex < provider.familyCount;
       ++familyIndex) {
    const FixtureFamily& family = provider.families[familyIndex];
    if (family.key != familyKey) continue;
    if (index >= family.kindCount) return false;
    out->name = {family.kinds[index].name, family.kinds[index].length};
    return true;
  }
  return false;
}

static text_core::ProviderView providerView(FixtureProvider& provider) {
  return {&provider, fixtureFamilyCount, fixtureFamilyAt,
          fixtureFamilyKindAt};
}

struct Capture {
  static constexpr size_t kMaxLines = 64;
  static constexpr size_t kMaxPayload = 511;

  size_t calls = 0;
  size_t accepted = 0;
  size_t failCall = static_cast<size_t>(-1);
  size_t lengths[kMaxLines]{};
  char lines[kMaxLines][kMaxPayload + 1]{};
};

static size_t productionFamilyCount(void*) {
  return systemEventCatalogFamilyCount();
}

static bool productionFamilyAt(void*,
                               size_t index,
                               text_core::FamilyView* out) {
  if (!out) return false;
  SystemEventCatalogFamilyInfo family{};
  if (!systemEventCatalogFamilyAt(index, &family)) return false;
  out->key = static_cast<size_t>(family.id);
  out->label = {family.label, strlen(family.label)};
  out->kindCount = family.kindCount;
  return true;
}

static bool productionFamilyKindAt(void*,
                                   size_t familyKey,
                                   size_t index,
                                   text_core::KindView* out) {
  if (!out || familyKey > UINT8_MAX) return false;
  SystemEventCatalogKindInfo kind{};
  if (!systemEventCatalogFamilyKindAt(
          static_cast<SystemEventFamily>(familyKey), index, &kind)) {
    return false;
  }
  out->name = {kind.name, strlen(kind.name)};
  return true;
}

static text_core::ProviderView productionProviderView() {
  return {nullptr, productionFamilyCount, productionFamilyAt,
          productionFamilyKindAt};
}

static bool captureLine(void* context, const char* line, size_t length) {
  Capture& capture = *static_cast<Capture*>(context);
  const size_t call = capture.calls++;
  assert(line != nullptr);
  assert(line[length] == '\0');
  if (call == capture.failCall) return false;
  assert(capture.accepted < Capture::kMaxLines);
  assert(length <= Capture::kMaxPayload);
  capture.lengths[capture.accepted] = length;
  memcpy(capture.lines[capture.accepted], line, length + 1u);
  ++capture.accepted;
  return true;
}

static FixtureProvider basicProvider() {
  static const FixtureKind alphaKinds[] = {
      {"one", 3},
      {"two", 3},
  };
  static const FixtureKind betaKinds[] = {
      {"three", 5},
  };
  static const FixtureFamily families[] = {
      {17, "Alpha", 5, alphaKinds, 2},
      {41, "Beta", 4, betaKinds, 1},
  };
  return {families, 2};
}

static void expectLine(const Capture& capture,
                       size_t index,
                       const char* expected) {
  assert(index < capture.accepted);
  assert(capture.lengths[index] == strlen(expected));
  assert(strcmp(capture.lines[index], expected) == 0);
}

static bool captureContainsToken(const Capture& capture, const char* wanted) {
  const size_t wantedLength = strlen(wanted);
  for (size_t lineIndex = 0; lineIndex < capture.accepted; ++lineIndex) {
    const char* line = capture.lines[lineIndex];
    size_t cursor = 0;
    while (cursor < capture.lengths[lineIndex]) {
      while (cursor < capture.lengths[lineIndex] && line[cursor] == ' ') {
        ++cursor;
      }
      const size_t begin = cursor;
      while (cursor < capture.lengths[lineIndex] && line[cursor] != ' ') {
        ++cursor;
      }
      if (cursor - begin == wantedLength &&
          memcmp(line + begin, wanted, wantedLength) == 0) {
        return true;
      }
    }
  }
  return false;
}

static void testProductionCatalogOutput() {
  Capture capture{};
  char line[256]{};
  size_t written = 0;
  const text_core::Status status = text_core::writeCatalog(
      productionProviderView(), captureLine, &capture, line, sizeof(line),
      sizeof(line) - 1u, &written);

  assert(status == text_core::Status::Ok);
  assert(written == capture.accepted);
  assert(capture.calls == capture.accepted);
  assert(capture.accepted > systemEventCatalogFamilyCount());
  expectLine(capture, 0, "OK: 152 event kinds in 12 families:");

  size_t visited = 0;
  for (size_t familyIndex = 0;
       familyIndex < systemEventCatalogFamilyCount(); ++familyIndex) {
    SystemEventCatalogFamilyInfo family{};
    assert(systemEventCatalogFamilyAt(familyIndex, &family));
    for (size_t kindIndex = 0; kindIndex < family.kindCount; ++kindIndex) {
      SystemEventCatalogKindInfo kind{};
      assert(systemEventCatalogFamilyKindAt(family.id, kindIndex, &kind));
      assert(captureContainsToken(capture, kind.name));
      ++visited;
    }
  }
  assert(visited == systemEventCatalogKindCount());
}

static void testBasicOutput() {
  FixtureProvider fixture = basicProvider();
  Capture capture{};
  char line[65]{};
  size_t written = 999;
  const text_core::Status status = text_core::writeCatalog(
      providerView(fixture), captureLine, &capture, line, sizeof(line), 64,
      &written);

  assert(status == text_core::Status::Ok);
  assert(written == 5);
  assert(capture.calls == 5);
  assert(capture.accepted == 5);
  expectLine(capture, 0, "OK: 3 event kinds in 2 families:");
  expectLine(capture, 1, "  Alpha (2):");
  expectLine(capture, 2, "    one two");
  expectLine(capture, 3, "  Beta (1):");
  expectLine(capture, 4, "    three");
}

static void testExactFitPacking() {
  static const FixtureKind kinds[] = {
      {"aaaaaaaaaa", 10},
      {"bbbbbbbbbbbbbbbbbbbbbbbbb", 25},
  };
  static const FixtureFamily families[] = {
      {3, "Exact", 5, kinds, 2},
  };
  FixtureProvider fixture{families, 1};
  Capture capture{};
  char line[41]{};
  const text_core::Status status = text_core::writeCatalog(
      providerView(fixture), captureLine, &capture, line, sizeof(line), 40);

  assert(status == text_core::Status::Ok);
  assert(capture.accepted == 3);
  assert(capture.lengths[2] == 40);
  expectLine(capture, 2,
             "    aaaaaaaaaa bbbbbbbbbbbbbbbbbbbbbbbbb");
}

static void fillToken(char* out, size_t length, char value) {
  for (size_t index = 0; index < length; ++index) out[index] = value;
  out[length] = '\0';
}

static void testLongTokenFlushAndRetry() {
  static char first[126];
  static char second[131];
  fillToken(first, 125, 'a');
  fillToken(second, 130, 'b');
  assert(strlen(second) > 120);

  const FixtureKind kinds[] = {
      {first, 125},
      {second, 130},
  };
  const FixtureFamily families[] = {
      {9, "Long", 4, kinds, 2},
  };
  FixtureProvider fixture{families, 1};
  Capture capture{};
  char line[256]{};
  const text_core::Status status = text_core::writeCatalog(
      providerView(fixture), captureLine, &capture, line, sizeof(line), 255);

  assert(status == text_core::Status::Ok);
  assert(capture.accepted == 4);
  assert(capture.lengths[2] == 4u + 125u);
  assert(capture.lengths[3] == 4u + 130u);
  assert(memcmp(capture.lines[2], "    ", 4) == 0);
  assert(memcmp(capture.lines[2] + 4, first, 125) == 0);
  assert(memcmp(capture.lines[3], "    ", 4) == 0);
  assert(memcmp(capture.lines[3] + 4, second, 130) == 0);
}

static void testOverLimitPreflightHasNoCallbacks() {
  static char oversized[253];
  fillToken(oversized, 252, 'z');
  const FixtureKind kinds[] = {{oversized, 252}};
  const FixtureFamily families[] = {
      {1, "Too long", 8, kinds, 1},
  };
  FixtureProvider fixture{families, 1};
  Capture capture{};
  char line[256];
  memset(line, 'x', sizeof(line));
  size_t written = 999;
  const text_core::Status status = text_core::writeCatalog(
      providerView(fixture), captureLine, &capture, line, sizeof(line), 255,
      &written);

  assert(status == text_core::Status::LineTooLong);
  assert(capture.calls == 0);
  assert(capture.accepted == 0);
  assert(written == 0);
  assert(line[0] == '\0');
}

static void testLateInvalidProviderPreflightHasNoCallbacks() {
  static const FixtureKind kinds[] = {
      {"valid", 5},
      {nullptr, 4},
  };
  static const FixtureFamily families[] = {
      {5, "Broken", 6, kinds, 2},
  };
  FixtureProvider fixture{families, 1};
  Capture capture{};
  char line[256]{};
  const text_core::Status status = text_core::writeCatalog(
      providerView(fixture), captureLine, &capture, line, sizeof(line), 255);

  assert(status == text_core::Status::ProviderFailed);
  assert(capture.calls == 0);
  assert(capture.accepted == 0);
}

static void testSinkFailureStopsCallbacks() {
  FixtureProvider fixture = basicProvider();
  Capture capture{};
  capture.failCall = 2;
  char line[65]{};
  size_t written = 999;
  const text_core::Status status = text_core::writeCatalog(
      providerView(fixture), captureLine, &capture, line, sizeof(line), 64,
      &written);

  assert(status == text_core::Status::SinkFailed);
  assert(capture.calls == 3);
  assert(capture.accepted == 2);
  assert(written == 2);
  expectLine(capture, 0, "OK: 3 event kinds in 2 families:");
  expectLine(capture, 1, "  Alpha (2):");
}

static void testInvalidArgumentsHaveNoCallbacks() {
  FixtureProvider fixture = basicProvider();
  Capture capture{};
  char line[64]{};
  size_t written = 999;
  assert(text_core::writeCatalog(providerView(fixture), captureLine, &capture,
                                 line, sizeof(line), sizeof(line), &written) ==
         text_core::Status::InvalidArgument);
  assert(written == 0);
  assert(capture.calls == 0);

  text_core::ProviderView missing = providerView(fixture);
  missing.familyKindAt = nullptr;
  assert(text_core::writeCatalog(missing, captureLine, &capture, line,
                                 sizeof(line), sizeof(line) - 1u) ==
         text_core::Status::InvalidArgument);
  assert(capture.calls == 0);
}

int main() {
  testProductionCatalogOutput();
  testBasicOutput();
  testExactFitPacking();
  testLongTokenFlushAndRetry();
  testOverLimitPreflightHasNoCallbacks();
  testLateInvalidProviderPreflightHasNoCallbacks();
  testSinkFailureStopsCallbacks();
  testInvalidArgumentsHaveNoCallbacks();
  puts("event catalog text tests passed");
  return 0;
}
