// System_EventCatalog.cpp - immutable System Event metadata and typed provider.
#include "System_EventCatalog.h"

#include "System_EventCatalogCore.h"

namespace {

namespace catalog_core = hw1_event_catalog_core;

// These are the moved legacy runtime tables. Keep them parallel and compact:
// do not materialize a second FamilyInfo/KindInfo descriptor table.
static constexpr const char* kFamilyLabels[] = {
#define HW1_EVENT_CATALOG_FAMILY_ROW(symbol, label) label,
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_FAMILY_ROW
};

static constexpr const char* kKindNames[] = {
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family) name,
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
};

static constexpr uint8_t kKindFamilies[] = {
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family) \
  static_cast<uint8_t>(family),
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
};

static constexpr size_t kFamilyCount =
    sizeof(kFamilyLabels) / sizeof(kFamilyLabels[0]);
static constexpr size_t kKindCount =
    sizeof(kKindNames) / sizeof(kKindNames[0]);

static_assert(kFamilyCount == static_cast<size_t>(SYSEVT_FAM_COUNT),
              "family enum/table mismatch");
static_assert(kKindCount + 1u == static_cast<size_t>(SYSEVT_COUNT),
              "kind enum/table mismatch");
static_assert(sizeof(kKindFamilies) / sizeof(kKindFamilies[0]) == kKindCount,
              "kind name/family table mismatch");

// Descriptor structs exist only as automatic constexpr inputs to constant
// evaluation. They never become runtime tables or linked symbols.
static constexpr catalog_core::ValidationResult validateProductionCatalog() {
  constexpr catalog_core::FamilyDescriptor families[] = {
#define HW1_EVENT_CATALOG_FAMILY_ROW(symbol, label) \
    {static_cast<uint8_t>(symbol), catalog_core::literalView(label)},
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_FAMILY_ROW
  };
  constexpr catalog_core::KindDescriptor kinds[] = {
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family)               \
    {static_cast<uint8_t>(symbol), static_cast<uint8_t>(family),       \
     catalog_core::literalView(name)},
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
  };
  return catalog_core::validate(families, kinds);
}

static_assert(validateProductionCatalog().ok(),
              "invalid System Event catalog rows");

// One kind id per canonical kind plus one offset/count pair per family. The
// explicit grouped index preserves declaration order even when one family's
// rows are interleaved with later additions.
static constexpr auto buildProductionFamilyIndex() {
  constexpr catalog_core::FamilyDescriptor families[] = {
#define HW1_EVENT_CATALOG_FAMILY_ROW(symbol, label) \
    {static_cast<uint8_t>(symbol), catalog_core::literalView(label)},
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_FAMILY_ROW
  };
  constexpr catalog_core::KindDescriptor kinds[] = {
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family)               \
    {static_cast<uint8_t>(symbol), static_cast<uint8_t>(family),       \
     catalog_core::literalView(name)},
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
  };
  return catalog_core::buildFamilyIndex(families, kinds);
}

static constexpr auto kFamilyIndex = buildProductionFamilyIndex();

static constexpr catalog_core::ValidationResult
validateProductionFamilyIndex() {
  constexpr catalog_core::FamilyDescriptor families[] = {
#define HW1_EVENT_CATALOG_FAMILY_ROW(symbol, label) \
    {static_cast<uint8_t>(symbol), catalog_core::literalView(label)},
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_FAMILY_ROW
  };
  constexpr catalog_core::KindDescriptor kinds[] = {
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family)               \
    {static_cast<uint8_t>(symbol), static_cast<uint8_t>(family),       \
     catalog_core::literalView(name)},
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
  };
  return catalog_core::validateFamilyIndex(families, kinds, kFamilyIndex);
}

static_assert(validateProductionFamilyIndex().ok(),
              "invalid System Event family index");

static constexpr size_t productionLongestKindTokenCapacity() {
  constexpr catalog_core::KindDescriptor kinds[] = {
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family)               \
    {static_cast<uint8_t>(symbol), static_cast<uint8_t>(family),       \
     catalog_core::literalView(name)},
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
  };
  return catalog_core::longestKindTokenCapacity(kinds);
}

static_assert(
    SYSTEM_EVENT_KIND_TOKEN_CAP == productionLongestKindTokenCapacity(),
    "SYSTEM_EVENT_KIND_TOKEN_CAP must be longest canonical token plus NUL");

static void fillKindInfo(size_t index,
                         SystemEventCatalogKindInfo& out) {
  out.id = static_cast<SystemEventKind>(index + 1u);
  out.family = static_cast<SystemEventFamily>(kKindFamilies[index]);
  out.name = kKindNames[index];
}

static char asciiLower(char value) {
  return value >= 'A' && value <= 'Z'
             ? static_cast<char>(value + ('a' - 'A'))
             : value;
}

static bool equalsAsciiFolded(const char* left, const char* right) {
  if (!left || !right) return false;
  while (*left != '\0' && *right != '\0') {
    if (asciiLower(*left) != asciiLower(*right)) return false;
    ++left;
    ++right;
  }
  return *left == *right;
}

}  // namespace

size_t systemEventCatalogFamilyCount() {
  return kFamilyCount;
}

size_t systemEventCatalogKindCount() {
  return kKindCount;
}

bool systemEventCatalogFamilyAt(
    size_t index, SystemEventCatalogFamilyInfo* out) {
  if (!out || index >= kFamilyCount) return false;
  SystemEventCatalogFamilyInfo resolved{};
  resolved.id = static_cast<SystemEventFamily>(index);
  resolved.label = kFamilyLabels[index];
  resolved.kindCount = kFamilyIndex.counts[index];
  *out = resolved;
  return true;
}

bool systemEventCatalogKindAt(
    size_t index, SystemEventCatalogKindInfo* out) {
  if (!out || index >= kKindCount) return false;
  SystemEventCatalogKindInfo resolved{};
  fillKindInfo(index, resolved);
  *out = resolved;
  return true;
}

bool systemEventCatalogFamilyKindAt(
    SystemEventFamily family,
    size_t index,
    SystemEventCatalogKindInfo* out) {
  if (!out) return false;
  const size_t familyIndex = static_cast<uint8_t>(family);
  if (familyIndex >= kFamilyCount ||
      index >= kFamilyIndex.counts[familyIndex]) {
    return false;
  }

  const size_t groupedIndex =
      static_cast<size_t>(kFamilyIndex.offsets[familyIndex]) + index;
  const uint8_t kindId = kFamilyIndex.kindIds[groupedIndex];
  // Dense live ids are compile-time validated as descriptor ordinal + 1.
  const size_t kindIndex = static_cast<size_t>(kindId - 1u);
  if (kindIndex >= kKindCount) return false;

  SystemEventCatalogKindInfo resolved{};
  fillKindInfo(kindIndex, resolved);
  *out = resolved;
  return true;
}

bool systemEventCatalogFindKind(
    const char* name, SystemEventCatalogKindInfo* out) {
  if (!out || !name || name[0] == '\0') return false;
  const char* canonicalInput =
      equalsAsciiFolded(name, "boot") ? "boot_finished" : name;
  size_t index = 0;
  for (; index < kKindCount; ++index) {
    if (equalsAsciiFolded(canonicalInput, kKindNames[index])) break;
  }
  if (index == kKindCount) return false;
  SystemEventCatalogKindInfo resolved{};
  fillKindInfo(index, resolved);
  *out = resolved;
  return true;
}

const char* systemEventFamilyName(uint8_t family) {
  return family < kFamilyCount ? kFamilyLabels[family] : "?";
}

uint8_t systemEventKindFamily(uint8_t kind) {
  if (kind == static_cast<uint8_t>(SYSEVT_NONE) || kind >= SYSEVT_COUNT) {
    return static_cast<uint8_t>(SYSEVT_FAM_SYSTEM);
  }
  return kKindFamilies[kind - 1u];
}

const char* systemEventKindName(uint8_t kind) {
  if (kind == static_cast<uint8_t>(SYSEVT_NONE)) return "none";
  if (kind >= SYSEVT_COUNT) return "?";
  return kKindNames[kind - 1u];
}

int systemEventKindFromName(const char* name) {
  SystemEventCatalogKindInfo resolved{};
  return systemEventCatalogFindKind(name, &resolved)
             ? static_cast<int>(resolved.id)
             : -1;
}
