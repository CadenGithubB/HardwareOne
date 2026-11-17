// ============================================================================
// OLED Console Buffer - history owner implementation
// ============================================================================
// Moved out of OLED_Utils.cpp. Allocation, the producer mutex and the delivery
// generation live here; the ring arithmetic and sanitizer are OLED_ConsoleCore.h so
// host tests run them as they ship.

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "OLED_ConsoleBuffer.h"
#include "System_Debug.h"  // DEBUG_SYSTEMF / ERROR_SYSTEMF
#include "System_MemUtil.h"
#include "System_Settings.h"

namespace {
// Producers and screens wait this long for the ring. Holds are a few
// microseconds (one sanitize-and-copy, one snapshot copy), so a timeout means
// a wedged task, not ordinary contention.
constexpr uint32_t kProducerWaitMs = 10;
constexpr uint32_t kSnapshotWaitMs = 10;
}  // namespace

OledConsoleBuffer gOledConsole;

// Static init runs long before gSettings is loaded; init() owns the allocation.
OledConsoleBuffer::OledConsoleBuffer()
    : records_(nullptr), arena_(nullptr), mutex_(nullptr), generation_(0),
      dropped_(0), fenced_(0), delivery_(0) {}

void OledConsoleBuffer::init() {
  // Idempotent, and deliberately never resets a ring it did not just allocate:
  // debugOutputTask is already running by the time initDebugSystem calls this,
  // so touching a live ring here would race its append(). The allocation fixes
  // the depth, hence "requires reboot" on the setting.
  if (!records_) {
    int slots = gSettings.oledCliHistorySize;
    if (slots < oled_console::kMinSlots) slots = oled_console::kMinSlots;
    if (slots > oled_console::kMaxSlots) slots = oled_console::kMaxSlots;
    const size_t arenaBytes = oled_console::arenaBytesFor((uint16_t)slots);

    // RequireInternal, not PreferInternal: the ring holds identity-private
    // command history and PreferInternal still spills to PSRAM once the
    // internal heap is exhausted. calloc zeroes both halves.
    oled_console::Record* records = (oled_console::Record*)ps_calloc(
        (size_t)slots, sizeof(oled_console::Record), AllocPref::RequireInternal,
        "oled.console.records");
    char* arena = (char*)ps_calloc(arenaBytes, 1, AllocPref::RequireInternal,
                                   "oled.console.text");
    if (records && arena && ring_.attach(records, (uint16_t)slots, arena, arenaBytes)) {
      // Publish last: isReady() gates every accessor on these pointers.
      arena_ = arena;
      records_ = records;
    } else {
      heap_caps_free(records);
      heap_caps_free(arena);
      ERROR_SYSTEMF("Failed to allocate OLED console buffer (%d slots, %u B) - history disabled",
                    slots, (unsigned)(arenaBytes + (size_t)slots * sizeof(oled_console::Record)));
    }
  }

  if (!mutex_) {
    mutex_ = (void*)xSemaphoreCreateMutex();
    if (!mutex_) ERROR_SYSTEMF("Failed to create OLED console buffer mutex");
  }

  if (records_ && mutex_) {
    const oled_console::Status st = ring_.status();
    DEBUG_SYSTEMF("OLED console buffer initialized (%u slots of %u max, %u B text, %u B total)",
                  (unsigned)st.capacity, (unsigned)oled_console::kMaxSlots,
                  (unsigned)oled_console::arenaBytesFor(st.capacity),
                  (unsigned)(oled_console::arenaBytesFor(st.capacity) +
                             (size_t)st.capacity * sizeof(oled_console::Record)));
  }
}

bool OledConsoleBuffer::isReady() const { return records_ && mutex_; }

bool OledConsoleBuffer::lock(uint32_t waitMs) {
  if (!mutex_) return false;
  return xSemaphoreTake((SemaphoreHandle_t)mutex_, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void OledConsoleBuffer::unlock() { xSemaphoreGive((SemaphoreHandle_t)mutex_); }

void OledConsoleBuffer::append(const char* text, uint32_t timestampMs,
                               uint32_t deliveryGeneration) {
  if (!text || !isReady()) return;
  if (!lock(kProducerWaitMs)) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // An envelope queued before the last identity boundary belongs to the
  // identity that was cleared; refuse it rather than refill their history.
  // Compared under the mutex, so it is ordered with clear()'s advance.
  if (deliveryGeneration != delivery_.load(std::memory_order_relaxed)) {
    unlock();
    fenced_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ring_.append(text, timestampMs);
  generation_.store(ring_.status().generation, std::memory_order_release);
  unlock();
}

void OledConsoleBuffer::clear() {
  if (!isReady()) return;
  if (!lock(portMAX_DELAY)) return;
  ring_.clear();
  delivery_.fetch_add(1, std::memory_order_release);
  generation_.store(ring_.status().generation, std::memory_order_release);
  unlock();
}

uint32_t OledConsoleBuffer::deliveryGeneration() const {
  return delivery_.load(std::memory_order_relaxed);
}

uint32_t OledConsoleBuffer::generation() const {
  return generation_.load(std::memory_order_acquire);
}

uint32_t OledConsoleBuffer::droppedCount() const {
  return dropped_.load(std::memory_order_relaxed);
}

uint32_t OledConsoleBuffer::fencedCount() const {
  return fenced_.load(std::memory_order_relaxed);
}

bool OledConsoleBuffer::tryStatus(oled_console::Status& out) {
  if (!isReady() || !lock(kSnapshotWaitMs)) return false;
  out = ring_.status();
  unlock();
  return true;
}

bool OledConsoleBuffer::tryCopyRecord(uint32_t id, oled_console::RecordCopy& out, bool* busy) {
  if (busy) *busy = false;
  if (!isReady()) return false;
  if (!lock(kSnapshotWaitMs)) {
    if (busy) *busy = true;
    return false;
  }
  const bool found = ring_.copyRecord(id, out);
  unlock();
  return found;
}

size_t OledConsoleBuffer::tryCopyWindow(uint32_t firstId, size_t rows, size_t cols,
                                        oled_console::Preview* out, bool* busy) {
  if (busy) *busy = false;
  if (!isReady()) return 0;
  if (!lock(kSnapshotWaitMs)) {
    if (busy) *busy = true;
    return 0;
  }
  const size_t copied = ring_.copyWindow(firstId, rows, cols, out);
  unlock();
  return copied;
}

#endif  // ENABLE_OLED_DISPLAY
