#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "../../System_HelpPagerCore.h"

namespace {

using hw1_help_pager::Cost;
using hw1_help_pager::Limits;
using hw1_help_pager::PageTokenStatus;
using hw1_help_pager::Pager;
using hw1_help_pager::Placement;

size_t gNewCalls = 0;

void assertPageToken(const char* text, PageTokenStatus status,
                     size_t expectedPage = 0) {
  const size_t length = text ? strlen(text) : 0;
  const hw1_help_pager::PageToken parsed =
      hw1_help_pager::parsePageToken(text, length);
  assert(parsed.status == status);
  assert(parsed.page == expectedPage);
}

void testStrictPageTokenParsing() {
  assertPageToken(nullptr, PageTokenStatus::NotPageToken);
  assertPageToken("", PageTokenStatus::NotPageToken);
  assertPageToken("2", PageTokenStatus::NotPageToken);
  assertPageToken("page2", PageTokenStatus::Invalid);

  assertPageToken("p1", PageTokenStatus::Valid, 1);
  assertPageToken("P2", PageTokenStatus::Valid, 2);
  assertPageToken("p987654", PageTokenStatus::Valid, 987654);

  const char* const invalid[] = {
      "p", "P", "p0", "P0", "p01", "P001", "p-1", "p+1",
      "p 1", "p1 ", "p1x", "pp1", "p1.0",
  };
  for (const char* token : invalid) {
    assertPageToken(token, PageTokenStatus::Invalid);
  }
  assertPageToken(" p1", PageTokenStatus::NotPageToken);

  char overflow[3 + sizeof(size_t) * 3]{};
  overflow[0] = 'p';
  for (size_t i = 1; i + 1 < sizeof(overflow); ++i) overflow[i] = '9';
  assertPageToken(overflow, PageTokenStatus::Invalid);

  // Parsing honors the supplied span and never requires a trailing NUL.
  const char bounded[] = {'P', '4', '2', 'x'};
  const hw1_help_pager::PageToken parsed =
      hw1_help_pager::parsePageToken(bounded, 3);
  assert(parsed.status == PageTokenStatus::Valid);
  assert(parsed.page == 42);
}

void assertPlacement(const Placement& placement, size_t ordinal, size_t page,
                     bool oversized = false) {
  assert(placement.itemOrdinal == ordinal);
  assert(placement.page == page);
  assert(placement.oversizedItem == oversized);
}

void testIndependentByteAndFrameBudgets() {
  Pager bytes(Limits{10, 100});
  assert(bytes.valid());
  assert(bytes.pageCount() == 0);
  assertPlacement(bytes.addItem(Cost{4, 1}), 0, 1);
  assertPlacement(bytes.addItem(Cost{6, 1}), 1, 1);
  assertPlacement(bytes.addItem(Cost{1, 1}), 2, 2);
  assert(bytes.pageCount() == 2);
  assert(bytes.currentCost().captureBytes == 1);
  assert(bytes.currentCost().logicalFrames == 1);

  Pager frames(Limits{100, 2});
  assertPlacement(frames.addItem(Cost{1, 1}), 0, 1);
  assertPlacement(frames.addItem(Cost{1, 1}), 1, 1);
  assertPlacement(frames.addItem(Cost{1, 1}), 2, 2);
  assert(frames.pageCount() == 2);

  Pager combined(Limits{10, 3});
  assertPlacement(combined.addItem(Cost{5, 2}), 0, 1);
  assertPlacement(combined.addItem(Cost{5, 1}), 1, 1);
  assertPlacement(combined.addItem(Cost{1, 1}), 2, 2);
}

void testFittingGroupsRemainAtomic() {
  Pager pager(Limits{10, 4});

  assertPlacement(pager.addItem(Cost{4, 1}), 0, 1);

  // The two-item group fits an empty page but not page 1's remainder. The
  // declaration therefore moves both items to page 2 before either is added.
  pager.beginGroup(Cost{7, 3});
  assertPlacement(pager.addItem(Cost{3, 1}), 1, 2);
  assertPlacement(pager.addItem(Cost{4, 2}), 2, 2);

  // Exact-fit group remains on the current page.
  pager.beginGroup(Cost{3, 1});
  assertPlacement(pager.addItem(Cost{3, 1}), 3, 2);
  assert(pager.pageCount() == 2);
  assert(pager.currentCost().captureBytes == 10);
  assert(pager.currentCost().logicalFrames == 4);
}

void testOversizedGroupsAndItemsMakeForwardProgress() {
  Pager pager(Limits{10, 3});
  assertPlacement(pager.addItem(Cost{2, 1}), 0, 1);

  // This group cannot be atomic. It starts on a clean page and splits at item
  // boundaries while preserving every item.
  pager.beginGroup(Cost{16, 4});
  assertPlacement(pager.addItem(Cost{6, 1}), 1, 2);
  assertPlacement(pager.addItem(Cost{4, 1}), 2, 2);
  assertPlacement(pager.addItem(Cost{6, 2}), 3, 3);

  // One indivisible item can itself exceed the limit. It appears exactly once
  // on a dedicated sealed page, and the following item advances rather than
  // retrying forever.
  pager.beginGroup(Cost{12, 1});
  assertPlacement(pager.addItem(Cost{12, 1}), 4, 4, true);
  assertPlacement(pager.addItem(Cost{1, 1}), 5, 5);
  assert(pager.pageCount() == 5);
  assert(pager.itemCount() == 6);
  assert(pager.oversizedItemCount() == 1);
}

void testOrderAndExactlyOnceAcrossMixedGroups() {
  static constexpr Cost kItems[] = {
      {3, 1}, {2, 1}, {4, 1}, {4, 1}, {4, 1},
      {9, 2}, {3, 1}, {20, 1}, {1, 1}, {2, 1},
  };
  static constexpr size_t kGroupStarts[] = {0, 2, 5, 7, 8};
  static constexpr size_t kGroupEnds[] = {2, 5, 7, 8, 10};
  static constexpr size_t kItemCount = sizeof(kItems) / sizeof(kItems[0]);

  Pager pager(Limits{10, 3});
  size_t assignedPages[kItemCount]{};
  size_t priorPage = 0;

  for (size_t group = 0;
       group < sizeof(kGroupStarts) / sizeof(kGroupStarts[0]); ++group) {
    Cost total{};
    for (size_t i = kGroupStarts[group]; i < kGroupEnds[group]; ++i) {
      total.captureBytes += kItems[i].captureBytes;
      total.logicalFrames += kItems[i].logicalFrames;
    }
    pager.beginGroup(total);
    for (size_t i = kGroupStarts[group]; i < kGroupEnds[group]; ++i) {
      const Placement placement = pager.addItem(kItems[i]);
      assert(placement.page >= priorPage);
      priorPage = placement.page;
      assert(placement.itemOrdinal == i);
      assignedPages[i] = placement.page;
    }
  }

  assert(pager.itemCount() == kItemCount);
  assert(pager.oversizedItemCount() == 1);

  // Model the adapter's allocation-free count pass followed by one replay per
  // selected page. Filtering placements must reproduce every original item
  // exactly once and in declaration order.
  bool seen[kItemCount]{};
  size_t emitted[kItemCount]{};
  size_t emittedCount = 0;
  for (size_t selectedPage = 1; selectedPage <= pager.pageCount();
       ++selectedPage) {
    Pager replay(Limits{10, 3});
    for (size_t group = 0;
         group < sizeof(kGroupStarts) / sizeof(kGroupStarts[0]); ++group) {
      Cost total{};
      for (size_t i = kGroupStarts[group]; i < kGroupEnds[group]; ++i) {
        total.captureBytes += kItems[i].captureBytes;
        total.logicalFrames += kItems[i].logicalFrames;
      }
      replay.beginGroup(total);
      for (size_t i = kGroupStarts[group]; i < kGroupEnds[group]; ++i) {
        const Placement placement = replay.addItem(kItems[i]);
        assert(placement.page == assignedPages[i]);
        if (placement.page != selectedPage) continue;
        assert(!seen[placement.itemOrdinal]);
        seen[placement.itemOrdinal] = true;
        emitted[emittedCount++] = placement.itemOrdinal;
      }
    }
    assert(replay.pageCount() == pager.pageCount());
  }

  assert(emittedCount == kItemCount);
  for (size_t i = 0; i < kItemCount; ++i) {
    assert(seen[i]);
    assert(emitted[i] == i);
  }
}

void testInvalidLimitsAndSaturatingOversizeAccounting() {
  Pager noBytes(Limits{0, 1});
  Pager noFrames(Limits{1, 0});
  assert(!noBytes.valid());
  assert(!noFrames.valid());
  assert(noBytes.addItem(Cost{1, 1}).page == 0);
  assert(noBytes.itemCount() == 0);
  assert(noBytes.pageCount() == 0);

  const size_t maxValue = static_cast<size_t>(-1);
  Pager pager(Limits{8, 2});
  const Placement huge = pager.addItem(Cost{maxValue, maxValue});
  assertPlacement(huge, 0, 1, true);
  assert(pager.currentCost().captureBytes == maxValue);
  assert(pager.currentCost().logicalFrames == maxValue);
  assertPlacement(pager.addItem(Cost{1, 1}), 1, 2);
}

void testNoAllocation() {
  const size_t before = gNewCalls;
  for (size_t run = 0; run < 10000; ++run) {
    Pager pager(Limits{31, 5});
    for (size_t group = 0; group < 8; ++group) {
      const Cost itemA{1 + ((run + group) % 17), 1};
      const Cost itemB{1 + ((run * 3 + group) % 19), 1};
      pager.beginGroup(Cost{itemA.captureBytes + itemB.captureBytes, 2});
      assert(pager.addItem(itemA).page != 0);
      assert(pager.addItem(itemB).page != 0);
    }
    assert(pager.itemCount() == 16);
  }
  assert(gNewCalls == before);
}

}  // namespace

void* operator new(size_t size) {
  ++gNewCalls;
  if (void* allocation = malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}

void* operator new[](size_t size) {
  ++gNewCalls;
  if (void* allocation = malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept { free(allocation); }
void operator delete[](void* allocation) noexcept { free(allocation); }
void operator delete(void* allocation, size_t) noexcept { free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { free(allocation); }

int main() {
  testStrictPageTokenParsing();
  testIndependentByteAndFrameBudgets();
  testFittingGroupsRemainAtomic();
  testOversizedGroupsAndItemsMakeForwardProgress();
  testOrderAndExactlyOnceAcrossMixedGroups();
  testInvalidLimitsAndSaturatingOversizeAccounting();
  testNoAllocation();
  puts("help pager core tests passed");
  return 0;
}
