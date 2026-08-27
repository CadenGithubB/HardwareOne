// System_EventCatalog.h - immutable, presentation-neutral System Event options.
//
// Canonical names are the stable persistence/wire identity. Numeric enum
// values and provider ordinals are process-local and must never be persisted.
#ifndef SYSTEM_EVENTCATALOG_H
#define SYSTEM_EVENTCATALOG_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

enum SystemEventFamily : uint8_t {
#define HW1_EVENT_CATALOG_FAMILY_ROW(symbol, label) symbol,
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_FAMILY_ROW
  SYSEVT_FAM_COUNT
};

enum SystemEventKind : uint8_t {
  SYSEVT_NONE = 0,
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family) symbol,
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
  SYSEVT_COUNT
};

// SYSEVT_COUNT is itself represented by the uint8_t enum. With NONE at zero,
// this permits at most 254 canonical kinds before the representation must be
// widened or the sentinel moved outside the enum.
static_assert(static_cast<unsigned>(SYSEVT_COUNT) <= UINT8_MAX,
              "System Event kind enum outgrew uint8_t");

namespace hw1_event_catalog_header_detail {

template <size_t First, size_t... Rest>
struct StaticMax {
  static constexpr size_t tail = StaticMax<Rest...>::value;
  static constexpr size_t value = First > tail ? First : tail;
};

template <size_t Last>
struct StaticMax<Last> {
  static constexpr size_t value = Last;
};

}  // namespace hw1_event_catalog_header_detail

// Longest canonical kind token including its trailing NUL. The value is
// generated from the same private rows as the enum; consumers never maintain
// a parallel token-width literal.
inline constexpr size_t SYSTEM_EVENT_KIND_TOKEN_CAP =
    hw1_event_catalog_header_detail::StaticMax<
#define HW1_EVENT_CATALOG_KIND_ROW(symbol, name, family) sizeof(name),
#include "System_EventCatalogRows.h"
#undef HW1_EVENT_CATALOG_KIND_ROW
        size_t{1}>::value;

struct SystemEventCatalogFamilyInfo {
  SystemEventFamily id;
  const char* label;
  size_t kindCount;
};

struct SystemEventCatalogKindInfo {
  SystemEventKind id;
  SystemEventFamily family;
  const char* name;
};

// Indexed traversal uses catalog declaration/display order. Returned string
// pointers refer to immutable firmware-image storage and remain valid for the
// life of the image. Null outputs and invalid indices return false without
// modifying caller storage.
size_t systemEventCatalogFamilyCount();
size_t systemEventCatalogKindCount();  // excludes SYSEVT_NONE

bool systemEventCatalogFamilyAt(
    size_t index, SystemEventCatalogFamilyInfo* out);
bool systemEventCatalogKindAt(
    size_t index, SystemEventCatalogKindInfo* out);
bool systemEventCatalogFamilyKindAt(
    SystemEventFamily family,
    size_t index,
    SystemEventCatalogKindInfo* out);

// ASCII case-insensitive. The legacy read alias "boot" resolves to the
// canonical boot_finished record but is never enumerated.
bool systemEventCatalogFindKind(
    const char* name, SystemEventCatalogKindInfo* out);

// Compatibility entry points retained for existing producers/consumers.
const char* systemEventFamilyName(uint8_t family);
uint8_t systemEventKindFamily(uint8_t kind);
const char* systemEventKindName(uint8_t kind);
int systemEventKindFromName(const char* name);

#endif  // SYSTEM_EVENTCATALOG_H
