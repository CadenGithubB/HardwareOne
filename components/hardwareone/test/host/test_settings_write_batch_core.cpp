#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <new>

#include "../../System_SettingsWriteBatchCore.h"

namespace {

using hw1_settings_batch::ALL;
using hw1_settings_batch::BeginMode;
using hw1_settings_batch::Core;
using hw1_settings_batch::DEBUG;
using hw1_settings_batch::ExpiredResult;
using hw1_settings_batch::FinishResult;
using hw1_settings_batch::MAIN;
using hw1_settings_batch::NoteResult;
using hw1_settings_batch::Owner;

size_t gNewCalls = 0;

constexpr Owner kWebRequest{1, 100};
constexpr Owner kOtherWebRequest{1, 101};
constexpr Owner kSerialSession{2, 100};

void assertFinish(const FinishResult& result, bool found, bool final,
                  uint8_t dirtyMask) {
  assert(result.found == found);
  assert(result.final == final);
  assert(result.dirtyMask == dirtyMask);
}

void assertExpired(const ExpiredResult& result, bool found,
                   Owner owner = {}, uint8_t dirtyMask = 0) {
  assert(result.found == found);
  assert(result.owner == owner);
  assert(result.dirtyMask == dirtyMask);
}

void testUnownedAndForeignMutationsWriteImmediately() {
  Core<3> core;

  assert(core.note(kWebRequest, MAIN) == NoteResult::WriteNow);
  assertFinish(core.finish(kWebRequest), false, false, 0);
  assert(!core.abort(kWebRequest));

  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.note(kOtherWebRequest, MAIN) == NoteResult::WriteNow);
  assert(core.note(kSerialSession, DEBUG) == NoteResult::WriteNow);

  // Owner matching uses both value fields. Neither an equal token in another
  // domain nor an equal domain with another token can join this batch.
  assert(core.note(Owner{2, 100}, MAIN) == NoteResult::WriteNow);
  assert(core.note(Owner{1, 101}, MAIN) == NoteResult::WriteNow);
  assert(core.note(Owner{1, 100}, MAIN) == NoteResult::Deferred);
  assertFinish(core.finish(kWebRequest), true, true, MAIN);
}

void testDirtyFilesAreTrackedIndependently() {
  Core<1> core;
  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.note(kWebRequest, MAIN) == NoteResult::Deferred);
  assert(core.note(kWebRequest, DEBUG) == NoteResult::Deferred);

  // Duplicate notes coalesce, and unsupported bits never escape to a flush.
  assert(core.note(kWebRequest, static_cast<uint8_t>(MAIN | 0xf0u)) ==
         NoteResult::Deferred);
  assertFinish(core.finish(kWebRequest), true, true, ALL);

  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assertFinish(core.finish(kWebRequest), true, true, 0);
}

void testIdempotentAndNestedBeginSemantics() {
  Core<1> core;

  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.note(kWebRequest, MAIN) == NoteResult::Deferred);
  // One finish closes two duplicate public begins.
  assertFinish(core.finish(kWebRequest), true, true, MAIN);

  assert(core.begin(kWebRequest, BeginMode::Nested));
  assert(core.begin(kWebRequest, BeginMode::Nested));
  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.note(kWebRequest, DEBUG) == NoteResult::Deferred);
  assertFinish(core.finish(kWebRequest), true, false, 0);

  // The owner remains active between nested finishes and can accumulate more
  // dirtiness without exposing a partial flush.
  assert(core.note(kWebRequest, MAIN) == NoteResult::Deferred);
  assertFinish(core.finish(kWebRequest), true, true, ALL);
  assert(core.note(kWebRequest, MAIN) == NoteResult::WriteNow);
}

void testFixedCapacityAndAbortIsolation() {
  Core<2> core;
  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.begin(kSerialSession, BeginMode::Idempotent));
  assert(!core.begin(kOtherWebRequest, BeginMode::Idempotent));

  // Slot exhaustion must not turn a foreign write into a deferred/lost write.
  assert(core.note(kOtherWebRequest, MAIN) == NoteResult::WriteNow);
  assert(core.note(kSerialSession, DEBUG) == NoteResult::Deferred);
  assert(core.abort(kSerialSession));
  assert(!core.abort(kSerialSession));

  assert(core.begin(kOtherWebRequest, BeginMode::Idempotent));
  assert(core.note(kOtherWebRequest, MAIN) == NoteResult::Deferred);
  assertFinish(core.finish(kOtherWebRequest), true, true, MAIN);
  assertFinish(core.finish(kWebRequest), true, true, 0);

  // Abort always cancels the complete nested owner scope.
  assert(core.begin(kWebRequest, BeginMode::Nested));
  assert(core.begin(kWebRequest, BeginMode::Nested));
  assert(core.note(kWebRequest, ALL) == NoteResult::Deferred);
  assert(core.abort(kWebRequest));
  assertFinish(core.finish(kWebRequest), false, false, 0);
}

void testFailedFlushRestoreAndPartialSuccess() {
  Core<2> core;
  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.note(kWebRequest, ALL) == NoteResult::Deferred);
  const FinishResult first = core.finish(kWebRequest);
  assertFinish(first, true, true, ALL);

  // Model MAIN succeeding and DEBUG failing. Only the failed file is restored.
  assert(core.restore(kWebRequest, DEBUG));
  assert(core.note(kWebRequest, MAIN) == NoteResult::Deferred);
  assertFinish(core.finish(kWebRequest), true, true, ALL);

  // Restoring into an already-active nested owner merges bits without adding
  // depth; the original two finish boundaries remain sufficient.
  assert(core.begin(kWebRequest, BeginMode::Nested));
  assert(core.begin(kWebRequest, BeginMode::Nested));
  assert(core.restore(kWebRequest, DEBUG));
  assertFinish(core.finish(kWebRequest), true, false, 0);
  assertFinish(core.finish(kWebRequest), true, true, DEBUG);

  // A zero/unknown-only restore is a successful no-op and consumes no slot.
  assert(core.restore(kWebRequest, 0));
  assert(core.restore(kWebRequest, 0x80u));
  assertFinish(core.finish(kWebRequest), false, false, 0);

  // Restoration failure is explicit when all slots belong to foreign owners;
  // restoring an existing owner still succeeds under the same saturation.
  assert(core.begin(kWebRequest, BeginMode::Idempotent));
  assert(core.begin(kSerialSession, BeginMode::Idempotent));
  assert(!core.restore(kOtherWebRequest, MAIN));
  assert(core.restore(kWebRequest, DEBUG));
  assertFinish(core.finish(kWebRequest), true, true, DEBUG);
  assertFinish(core.finish(kSerialSession), true, true, 0);
}

void testExpiryBoundaryAndImmediateExpiry() {
  Core<1> core;
  assert(core.begin(kWebRequest, BeginMode::Idempotent, 100));
  assertExpired(core.takeExpired(1099, 1000), false);
  assertExpired(core.takeExpired(1100, 1000), true, kWebRequest, 0);
  assertFinish(core.finish(kWebRequest), false, false, 0);

  assert(core.begin(kWebRequest, BeginMode::Idempotent, 500));
  assert(core.note(kWebRequest, MAIN, 500) == NoteResult::Deferred);
  assertExpired(core.takeExpired(500, 0), true, kWebRequest, MAIN);
}

void testEveryOwnedActivityRefreshesExpiry() {
  Core<2> core;

  // Both begin modes refresh the same owner's lease without changing their
  // established idempotent/nested depth semantics.
  assert(core.begin(kWebRequest, BeginMode::Idempotent, 100));
  assert(core.begin(kWebRequest, BeginMode::Idempotent, 600));
  assertExpired(core.takeExpired(1099, 500), false);
  assertExpired(core.takeExpired(1100, 500), true, kWebRequest, 0);

  assert(core.begin(kWebRequest, BeginMode::Nested, 100));
  assert(core.begin(kWebRequest, BeginMode::Nested, 600));
  assertExpired(core.takeExpired(1099, 500), false);
  // Expiry cancels the complete nested scope and returns its aggregate mask.
  assert(core.note(kWebRequest, MAIN, 1100) == NoteResult::Deferred);
  assertExpired(core.takeExpired(1599, 500), false);
  assertExpired(core.takeExpired(1600, 500), true, kWebRequest, MAIN);
  assertFinish(core.finish(kWebRequest), false, false, 0);

  // A matching note refreshes only its exact owner. The foreign note remains
  // immediate and cannot extend another request's lifetime.
  assert(core.begin(kWebRequest, BeginMode::Idempotent, 100));
  assert(core.note(kWebRequest, DEBUG, 900) == NoteResult::Deferred);
  assert(core.note(kOtherWebRequest, MAIN, 1399) == NoteResult::WriteNow);
  assertExpired(core.takeExpired(1399, 500), false);
  assertExpired(core.takeExpired(1400, 500), true, kWebRequest, DEBUG);

  // Restore both creates a timestamped retry scope and refreshes an existing
  // one. Even a zero-mask restore is activity when that owner already exists.
  assert(core.restore(kWebRequest, MAIN, 1000));
  assert(core.restore(kWebRequest, DEBUG, 1200));
  assert(core.restore(kWebRequest, 0, 1400));
  assertExpired(core.takeExpired(1899, 500), false);
  assertExpired(core.takeExpired(1900, 500), true, kWebRequest, ALL);
}

void testExpiryAcrossMillisWrap() {
  Core<1> core;
  constexpr uint32_t kBeforeWrap = UINT32_MAX - 5u;
  constexpr uint32_t kTouchBeforeWrap = UINT32_MAX - 3u;

  assert(core.begin(kWebRequest, BeginMode::Idempotent, kBeforeWrap));
  assert(core.note(kWebRequest, DEBUG, kTouchBeforeWrap) ==
         NoteResult::Deferred);
  assertExpired(core.takeExpired(5, 10), false);  // unsigned age is 9 ms
  assertExpired(core.takeExpired(6, 10), true, kWebRequest, DEBUG);
}

void testExpiryDrainsOnlyOneSlotPerCall() {
  Core<3> core;
  assert(core.begin(kWebRequest, BeginMode::Idempotent, 10));
  assert(core.note(kWebRequest, MAIN, 10) == NoteResult::Deferred);
  assert(core.begin(kSerialSession, BeginMode::Idempotent, 20));
  assert(core.note(kSerialSession, DEBUG, 20) == NoteResult::Deferred);
  assert(core.begin(kOtherWebRequest, BeginMode::Idempotent, 900));

  // Both first slots are old at t=1020, but one call transfers only the first
  // slot. A second call is required to obtain the other owner's dirty mask.
  assertExpired(core.takeExpired(1020, 1000), true, kWebRequest, MAIN);
  assertExpired(core.takeExpired(1020, 1000), true, kSerialSession, DEBUG);
  assertExpired(core.takeExpired(1020, 1000), false);
  assertFinish(core.finish(kOtherWebRequest), true, true, 0);
}

void testAllOwnerValuesAndNoAllocation() {
  const size_t allocationsBefore = gNewCalls;
  Core<4> core;

  // Zero is a valid value token; slot occupancy is represented by depth, not
  // by a sentinel Owner value.
  const Owner zero{0, 0};
  assert(core.begin(zero, BeginMode::Idempotent));
  assert(core.note(zero, MAIN) == NoteResult::Deferred);
  assertFinish(core.finish(zero), true, true, MAIN);

  for (uint64_t i = 0; i < 100000; ++i) {
    const Owner owner{i & 3u, i + 1u};
    const uint32_t nowMs = static_cast<uint32_t>(i);
    assert(core.begin(owner, BeginMode::Idempotent, nowMs));
    assert(core.note(owner, (i & 1u) ? MAIN : DEBUG, nowMs) ==
           NoteResult::Deferred);
    assertExpired(core.takeExpired(nowMs, 1), false);
    const FinishResult result = core.finish(owner);
    assert(result.found && result.final);
  }

  assert(gNewCalls == allocationsBefore);
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
  testUnownedAndForeignMutationsWriteImmediately();
  testDirtyFilesAreTrackedIndependently();
  testIdempotentAndNestedBeginSemantics();
  testFixedCapacityAndAbortIsolation();
  testFailedFlushRestoreAndPartialSuccess();
  testExpiryBoundaryAndImmediateExpiry();
  testEveryOwnedActivityRefreshesExpiry();
  testExpiryAcrossMillisWrap();
  testExpiryDrainsOnlyOneSlotPerCall();
  testAllOwnerValuesAndNoAllocation();
  puts("settings write batch core tests passed");
  return 0;
}
