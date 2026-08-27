#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <new>
#include <thread>

#ifdef NDEBUG
#error "event_catalog_tests require active runtime assertions"
#endif

#include "../../System_EventCatalog.h"
#include "../../System_EventCatalogCore.h"

namespace catalog_core = hw1_event_catalog_core;

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

template <typename T, size_t N>
constexpr size_t arrayCount(const T (&)[N]) {
  return N;
}

static constexpr catalog_core::FamilyDescriptor kProductionFamilies[] = {
#define HW1_EVENT_CATALOG_FAMILY_ROW(symbol, label) \
  {static_cast<uint8_t>(symbol), catalog_core::literalView(label)},
#include "../../System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_FAMILY_ROW
};

static constexpr catalog_core::KindDescriptor kProductionKinds[] = {
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family)                 \
  {static_cast<uint8_t>(symbol), static_cast<uint8_t>(family),          \
   catalog_core::literalView(name)},
#include "../../System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
};

static constexpr auto kProductionFamilyIndex =
    catalog_core::buildFamilyIndex(kProductionFamilies, kProductionKinds);

static_assert(arrayCount(kProductionFamilies) == 12,
              "reviewed fixture family count changed");
static_assert(arrayCount(kProductionKinds) == 152,
              "reviewed fixture kind count changed");
static_assert(catalog_core::validate(kProductionFamilies, kProductionKinds)
                  .ok(),
              "production descriptors must satisfy the real core");
static_assert(
    catalog_core::validateFamilyIndex(kProductionFamilies, kProductionKinds,
                                      kProductionFamilyIndex)
        .ok(),
    "production family index must cover every kind exactly once");
static_assert(SYSTEM_EVENT_KIND_TOKEN_CAP == 26,
              "longest reviewed kind token plus NUL changed");
static_assert(
    catalog_core::longestKindTokenCapacity(kProductionKinds) ==
        SYSTEM_EVENT_KIND_TOKEN_CAP,
    "public token capacity must be generated from production rows");

// Family zero intentionally owns more than 32 non-contiguous kinds. Family
// one interrupts it near both ends, so a start/count assumption cannot pass.
static constexpr catalog_core::FamilyDescriptor kLargeFamilies[] = {
    {0, catalog_core::literalView("Large")},
    {1, catalog_core::literalView("Other")},
};

static constexpr catalog_core::KindDescriptor kLargeKinds[] = {
    {1, 0, catalog_core::literalView("k01")},
    {2, 1, catalog_core::literalView("k02")},
    {3, 0, catalog_core::literalView("k03")},
    {4, 0, catalog_core::literalView("k04")},
    {5, 0, catalog_core::literalView("k05")},
    {6, 0, catalog_core::literalView("k06")},
    {7, 0, catalog_core::literalView("k07")},
    {8, 0, catalog_core::literalView("k08")},
    {9, 0, catalog_core::literalView("k09")},
    {10, 0, catalog_core::literalView("k10")},
    {11, 0, catalog_core::literalView("k11")},
    {12, 0, catalog_core::literalView("k12")},
    {13, 0, catalog_core::literalView("k13")},
    {14, 0, catalog_core::literalView("k14")},
    {15, 0, catalog_core::literalView("k15")},
    {16, 0, catalog_core::literalView("k16")},
    {17, 0, catalog_core::literalView("k17")},
    {18, 0, catalog_core::literalView("k18")},
    {19, 0, catalog_core::literalView("k19")},
    {20, 0, catalog_core::literalView("k20")},
    {21, 0, catalog_core::literalView("k21")},
    {22, 0, catalog_core::literalView("k22")},
    {23, 0, catalog_core::literalView("k23")},
    {24, 0, catalog_core::literalView("k24")},
    {25, 0, catalog_core::literalView("k25")},
    {26, 0, catalog_core::literalView("k26")},
    {27, 0, catalog_core::literalView("k27")},
    {28, 0, catalog_core::literalView("k28")},
    {29, 0, catalog_core::literalView("k29")},
    {30, 0, catalog_core::literalView("k30")},
    {31, 0, catalog_core::literalView("k31")},
    {32, 0, catalog_core::literalView("k32")},
    {33, 0, catalog_core::literalView("k33")},
    {34, 0, catalog_core::literalView("k34")},
    {35, 0, catalog_core::literalView("k35")},
    {36, 1, catalog_core::literalView("k36")},
    {37, 0, catalog_core::literalView("k37")},
};

static constexpr auto kLargeFamilyIndex =
    catalog_core::buildFamilyIndex(kLargeFamilies, kLargeKinds);
static_assert(catalog_core::validate(kLargeFamilies, kLargeKinds).ok(),
              "large interleaved fixture must be valid");
static_assert(
    catalog_core::validateFamilyIndex(kLargeFamilies, kLargeKinds,
                                      kLargeFamilyIndex)
        .ok(),
    "large interleaved fixture index must be valid");
static_assert(kLargeFamilyIndex.counts[0] == 35,
              "large family must cross the former 32-row boundary");
static_assert(kLargeFamilyIndex.counts[1] == 2,
              "interleaved family count changed");
static_assert(kLargeFamilyIndex.kindIds[0] == 1 &&
                  kLargeFamilyIndex.kindIds[1] == 3 &&
                  kLargeFamilyIndex.kindIds[34] == 37 &&
                  kLargeFamilyIndex.kindIds[35] == 2 &&
                  kLargeFamilyIndex.kindIds[36] == 36,
              "family index must preserve within-family declaration order");

static constexpr catalog_core::FamilyDescriptor kEmptyFamilyFixture[] = {
    {0, catalog_core::literalView("Used")},
    {1, catalog_core::literalView("Empty")},
};
static constexpr catalog_core::KindDescriptor kEmptyFamilyKinds[] = {
    {1, 0, catalog_core::literalView("only")},
};
static_assert(
    catalog_core::validate(kEmptyFamilyFixture, kEmptyFamilyKinds)
        .has(catalog_core::EmptyFamily),
    "an empty declared family must be rejected");

static constexpr catalog_core::FamilyDescriptor kOneFamily[] = {
    {0, catalog_core::literalView("Only")},
};
static constexpr catalog_core::KindDescriptor kInvalidFamilyKinds[] = {
    {1, 1, catalog_core::literalView("wrong_family")},
};
static_assert(
    catalog_core::validate(kOneFamily, kInvalidFamilyKinds)
        .has(catalog_core::InvalidKindFamily),
    "an invalid kind family must be rejected");

static constexpr catalog_core::KindDescriptor kDuplicateKinds[] = {
    {1, 0, catalog_core::literalView("same")},
    {2, 0, catalog_core::literalView("SAME")},
};
static_assert(catalog_core::validate(kOneFamily, kDuplicateKinds)
                  .has(catalog_core::DuplicateKindName),
              "case-folded duplicate names must be rejected");

template <size_t N>
constexpr bool rejectsReservedKind(const char (&name)[N]) {
  const catalog_core::KindDescriptor kinds[] = {
      {1, 0, catalog_core::literalView(name)},
  };
  return catalog_core::validate(kOneFamily, kinds)
      .has(catalog_core::ReservedKindName);
}

static_assert(rejectsReservedKind("boot"),
              "boot must remain a read-only alias");
static_assert(rejectsReservedKind("none"),
              "none must remain the zero-kind sentinel token");
static_assert(rejectsReservedKind("set"),
              "set must remain a notification-list operator");
static_assert(rejectsReservedKind("patch"),
              "patch must remain a notification-list operator");
static_assert(rejectsReservedKind("all"),
              "all must remain a notification-list operator");
static_assert(rejectsReservedKind("list"),
              "list must remain the device-policy listing operator");

static constexpr catalog_core::KindDescriptor kReservedNearNeighbors[] = {
    {1, 0, catalog_core::literalView("boot_finished")},
    {2, 0, catalog_core::literalView("nonevent")},
    {3, 0, catalog_core::literalView("setting_changed")},
    {4, 0, catalog_core::literalView("patch_applied")},
    {5, 0, catalog_core::literalView("all_clear")},
    {6, 0, catalog_core::literalView("listing")},
};
static_assert(
    catalog_core::validate(kOneFamily, kReservedNearNeighbors).ok(),
    "reserved-name rejection must match exact tokens, not substrings");

static constexpr char kEmbeddedNul[] = {'a', '\0', 'b', '\0'};
static constexpr catalog_core::KindDescriptor kEmbeddedNulKind[] = {
    {1, 0, {kEmbeddedNul, 3}},
};
static_assert(catalog_core::validate(kOneFamily, kEmbeddedNulKind)
                  .has(catalog_core::KindNameEmbeddedNul),
              "embedded NUL kind names must be rejected");

static constexpr char kInvalidUtf8[] = {static_cast<char>(0xC0), '\0'};
static constexpr catalog_core::FamilyDescriptor kInvalidUtf8Family[] = {
    {0, {kInvalidUtf8, 1}},
};
static constexpr catalog_core::KindDescriptor kValidSingleKind[] = {
    {1, 0, catalog_core::literalView("valid")},
};
static_assert(catalog_core::validate(kInvalidUtf8Family, kValidSingleKind)
                  .has(catalog_core::InvalidFamilyLabelUtf8),
              "malformed UTF-8 family labels must be rejected");

static bool sameKind(const SystemEventCatalogKindInfo& actual,
                     const catalog_core::KindDescriptor& expected) {
  return static_cast<uint8_t>(actual.id) == expected.id &&
         static_cast<uint8_t>(actual.family) == expected.family &&
         strcmp(actual.name, expected.name.data) == 0;
}

// Resolve one row through the same family index shape used by the production
// provider. This fixture deliberately has 35 non-contiguous rows in family 0,
// crossing both of the former OLED cache limits (24 and 32).
static const catalog_core::KindDescriptor* largeFamilyKindAt(
    size_t familyIndex, size_t withinFamily) {
  if (familyIndex >= arrayCount(kLargeFamilies) ||
      withinFamily >= kLargeFamilyIndex.counts[familyIndex]) {
    return nullptr;
  }
  const size_t groupedIndex =
      static_cast<size_t>(kLargeFamilyIndex.offsets[familyIndex]) +
      withinFamily;
  const size_t descriptorIndex = catalog_core::descriptorIndexForId(
      kLargeKinds, kLargeFamilyIndex.kindIds[groupedIndex]);
  return descriptorIndex == catalog_core::kNotFound
             ? nullptr
             : &kLargeKinds[descriptorIndex];
}

static void testProductionTraversal() {
  assert(systemEventCatalogFamilyCount() == arrayCount(kProductionFamilies));
  assert(systemEventCatalogKindCount() == arrayCount(kProductionKinds));
  assert(static_cast<size_t>(SYSEVT_FAM_COUNT) ==
         systemEventCatalogFamilyCount());
  assert(static_cast<size_t>(SYSEVT_COUNT) ==
         systemEventCatalogKindCount() + 1u);

  bool globallySeen[SYSEVT_COUNT]{};
  for (size_t kindIndex = 0; kindIndex < systemEventCatalogKindCount();
       ++kindIndex) {
    SystemEventCatalogKindInfo kind{};
    assert(systemEventCatalogKindAt(kindIndex, &kind));
    assert(sameKind(kind, kProductionKinds[kindIndex]));
    assert(static_cast<size_t>(kind.id) == kindIndex + 1u);
    assert(!globallySeen[static_cast<size_t>(kind.id)]);
    globallySeen[static_cast<size_t>(kind.id)] = true;

    SystemEventCatalogKindInfo resolved{};
    assert(systemEventCatalogFindKind(kind.name, &resolved));
    assert(resolved.id == kind.id);
    assert(resolved.family == kind.family);
    assert(resolved.name == kind.name);

    char upper[SYSTEM_EVENT_KIND_TOKEN_CAP]{};
    const size_t length = strlen(kind.name);
    assert(length + 1u <= sizeof(upper));
    for (size_t index = 0; index < length; ++index) {
      const char value = kind.name[index];
      upper[index] = value >= 'a' && value <= 'z'
                         ? static_cast<char>(value - ('a' - 'A'))
                         : value;
    }
    assert(systemEventCatalogFindKind(upper, &resolved));
    assert(resolved.id == kind.id);
  }

  size_t groupedCount = 0;
  for (size_t familyIndex = 0;
       familyIndex < systemEventCatalogFamilyCount(); ++familyIndex) {
    SystemEventCatalogFamilyInfo family{};
    assert(systemEventCatalogFamilyAt(familyIndex, &family));
    assert(static_cast<uint8_t>(family.id) ==
           kProductionFamilies[familyIndex].id);
    assert(strcmp(family.label,
                  kProductionFamilies[familyIndex].label.data) == 0);
    assert(family.kindCount == kProductionFamilyIndex.counts[familyIndex]);
    assert(family.kindCount > 0);

    for (size_t within = 0; within < family.kindCount; ++within) {
      SystemEventCatalogKindInfo kind{};
      assert(systemEventCatalogFamilyKindAt(family.id, within, &kind));
      const size_t groupedIndex =
          static_cast<size_t>(kProductionFamilyIndex.offsets[familyIndex]) +
          within;
      const size_t expectedIndex =
          static_cast<size_t>(kProductionFamilyIndex.kindIds[groupedIndex]) -
          1u;
      assert(sameKind(kind, kProductionKinds[expectedIndex]));
      assert(kind.family == family.id);
      ++groupedCount;
    }

    // UI consumers navigate within one family. Exercise the exact final row
    // and the immediately-out-of-range ordinal for every production family.
    SystemEventCatalogKindInfo last{};
    assert(systemEventCatalogFamilyKindAt(
        family.id, family.kindCount - 1u, &last));
    const size_t groupedLast =
        static_cast<size_t>(kProductionFamilyIndex.offsets[familyIndex]) +
        family.kindCount - 1u;
    const size_t expectedLast =
        static_cast<size_t>(kProductionFamilyIndex.kindIds[groupedLast]) - 1u;
    assert(sameKind(last, kProductionKinds[expectedLast]));

    const SystemEventCatalogKindInfo sentinel = last;
    assert(!systemEventCatalogFamilyKindAt(
        family.id, family.kindCount, &last));
    assert(last.id == sentinel.id);
    assert(last.family == sentinel.family);
    assert(last.name == sentinel.name);
  }
  assert(groupedCount == systemEventCatalogKindCount());
  for (size_t id = 1; id < static_cast<size_t>(SYSEVT_COUNT); ++id) {
    assert(globallySeen[id]);
  }
}

static void testLargeSyntheticFamilyTraversal() {
  assert(kLargeFamilyIndex.counts[0] == 35);
  for (size_t within = 0; within < 35; ++within) {
    const catalog_core::KindDescriptor* kind =
        largeFamilyKindAt(0, within);
    assert(kind != nullptr);
    assert(kind->family == 0);
  }
  assert(largeFamilyKindAt(0, 0)->id == 1);
  assert(largeFamilyKindAt(0, 1)->id == 3);
  assert(largeFamilyKindAt(0, 31)->id == 33);
  assert(largeFamilyKindAt(0, 32)->id == 34);
  assert(largeFamilyKindAt(0, 34)->id == 37);
  assert(largeFamilyKindAt(0, 35) == nullptr);

  assert(kLargeFamilyIndex.counts[1] == 2);
  assert(largeFamilyKindAt(1, 0)->id == 2);
  assert(largeFamilyKindAt(1, 1)->id == 36);
  assert(largeFamilyKindAt(1, 2) == nullptr);
  assert(largeFamilyKindAt(2, 0) == nullptr);
}

static void testBoundsAndCompatibility() {
  const SystemEventCatalogFamilyInfo familySentinel{
      SYSEVT_FAM_OTA, "unchanged-family", 999u};
  SystemEventCatalogFamilyInfo family = familySentinel;
  assert(!systemEventCatalogFamilyAt(systemEventCatalogFamilyCount(),
                                     &family));
  assert(family.id == familySentinel.id);
  assert(family.label == familySentinel.label);
  assert(family.kindCount == familySentinel.kindCount);
  assert(!systemEventCatalogFamilyAt(0, nullptr));

  const SystemEventCatalogKindInfo kindSentinel{
      SYSEVT_OTA_ROLLED_BACK, SYSEVT_FAM_OTA, "unchanged-kind"};
  SystemEventCatalogKindInfo kind = kindSentinel;
  assert(!systemEventCatalogKindAt(systemEventCatalogKindCount(), &kind));
  assert(kind.id == kindSentinel.id);
  assert(kind.family == kindSentinel.family);
  assert(kind.name == kindSentinel.name);
  assert(!systemEventCatalogKindAt(0, nullptr));

  assert(!systemEventCatalogFamilyKindAt(
      static_cast<SystemEventFamily>(SYSEVT_FAM_COUNT), 0, &kind));
  assert(kind.name == kindSentinel.name);
  assert(!systemEventCatalogFamilyKindAt(SYSEVT_FAM_MESH, SIZE_MAX, &kind));
  assert(kind.name == kindSentinel.name);
  assert(!systemEventCatalogFamilyKindAt(SYSEVT_FAM_MESH, 0, nullptr));

  assert(!systemEventCatalogFindKind(nullptr, &kind));
  assert(!systemEventCatalogFindKind("", &kind));
  assert(!systemEventCatalogFindKind("not_a_real_kind", &kind));
  assert(!systemEventCatalogFindKind("peer_online", nullptr));
  assert(kind.id == kindSentinel.id);
  assert(kind.family == kindSentinel.family);
  assert(kind.name == kindSentinel.name);

  assert(systemEventCatalogFindKind("BoOt", &kind));
  assert(kind.id == SYSEVT_BOOT_FINISHED);
  assert(strcmp(kind.name, "boot_finished") == 0);

  assert(strcmp(systemEventKindName(SYSEVT_NONE), "none") == 0);
  assert(strcmp(systemEventKindName(UINT8_MAX), "?") == 0);
  assert(systemEventKindFamily(SYSEVT_NONE) == SYSEVT_FAM_SYSTEM);
  assert(systemEventKindFamily(UINT8_MAX) == SYSEVT_FAM_SYSTEM);
  assert(strcmp(systemEventFamilyName(UINT8_MAX), "?") == 0);
  assert(systemEventKindFromName(nullptr) == -1);
  assert(systemEventKindFromName("") == -1);
  assert(systemEventKindFromName("boot") == SYSEVT_BOOT_FINISHED);
  assert(systemEventKindFromName("none") == -1);
  assert(systemEventKindFromName("set") == -1);
  assert(systemEventKindFromName("patch") == -1);
  assert(systemEventKindFromName("all") == -1);
  assert(systemEventKindFromName("list") == -1);
  assert(systemEventKindFromName("PEER_ONLINE") == SYSEVT_PEER_ONLINE);
}

static void testCoreFailuresAndIndexBounds() {
  auto badIndex = kLargeFamilyIndex;
  badIndex.kindIds[1] = badIndex.kindIds[0];
  const auto duplicateResult =
      catalog_core::validateFamilyIndex(kLargeFamilies, kLargeKinds, badIndex);
  assert(duplicateResult.has(catalog_core::IndexDuplicateKind));
  assert(duplicateResult.has(catalog_core::IndexCoverageMismatch));

  badIndex = kLargeFamilyIndex;
  badIndex.offsets[1] = 0;
  const auto offsetResult =
      catalog_core::validateFamilyIndex(kLargeFamilies, kLargeKinds, badIndex);
  assert(offsetResult.has(catalog_core::IndexOffsetMismatch));

  assert(catalog_core::findKindOrdinal(
             kProductionKinds, catalog_core::literalView("boot")) ==
         static_cast<size_t>(SYSEVT_BOOT_FINISHED) - 1u);
  assert(catalog_core::findKindOrdinal(
             kProductionKinds, catalog_core::literalView("PeEr_OnLiNe")) ==
         static_cast<size_t>(SYSEVT_PEER_ONLINE) - 1u);
  assert(catalog_core::findKindOrdinal(
             kProductionKinds, catalog_core::literalView("missing")) ==
         catalog_core::kNotFound);
}

static void testProductionReadsAllocateNothing() {
  const size_t before = gNewCalls.load(std::memory_order_relaxed);
  for (size_t iteration = 0; iteration < 1000; ++iteration) {
    const size_t kindIndex = iteration % systemEventCatalogKindCount();
    const size_t familyIndex = iteration % systemEventCatalogFamilyCount();
    SystemEventCatalogKindInfo kind{};
    SystemEventCatalogFamilyInfo family{};
    assert(systemEventCatalogKindAt(kindIndex, &kind));
    assert(systemEventCatalogFamilyAt(familyIndex, &family));
    assert(systemEventCatalogFamilyKindAt(
        family.id, iteration % family.kindCount, &kind));
    assert(systemEventCatalogFindKind(kind.name, &kind));
  }
  assert(gNewCalls.load(std::memory_order_relaxed) == before);
}

static void testConcurrentReadsAreIndependent() {
  constexpr size_t kWorkerCount = 4;
  constexpr size_t kIterations = 20000;
  std::atomic<bool> consistent{true};
  std::thread workers[kWorkerCount];

  for (size_t worker = 0; worker < kWorkerCount; ++worker) {
    workers[worker] = std::thread([worker, &consistent]() {
      for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const size_t index =
            (iteration * 17u + worker) % systemEventCatalogKindCount();
        SystemEventCatalogKindInfo kind{};
        SystemEventCatalogKindInfo resolved{};
        if (!systemEventCatalogKindAt(index, &kind) ||
            !systemEventCatalogFindKind(kind.name, &resolved) ||
            kind.id != resolved.id || kind.family != resolved.family ||
            kind.name != resolved.name ||
            strcmp(systemEventKindName(static_cast<uint8_t>(kind.id)),
                   kind.name) != 0 ||
            systemEventKindFamily(static_cast<uint8_t>(kind.id)) !=
                static_cast<uint8_t>(kind.family)) {
          consistent.store(false, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  assert(consistent.load(std::memory_order_relaxed));
}

int main() {
  testProductionTraversal();
  testLargeSyntheticFamilyTraversal();
  testBoundsAndCompatibility();
  testCoreFailuresAndIndexBounds();
  testProductionReadsAllocateNothing();
  testConcurrentReadsAreIndependent();
  puts("event catalog tests passed");
  return 0;
}
