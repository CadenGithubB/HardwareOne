// Decoder, reset and FIFO read methods are extracted from the shipping source.
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// INSERT_CONSTANTS

static void check(bool condition, const char* description) {
  if (!condition) throw std::runtime_error(description);
}
using Frame = std::array<uint8_t, 4>;
using Batch = std::vector<Frame>;
static const Frame neutral{50, 50, 50, 50};
static const Frame upPositive{90, 10, 50, 50};
static const Frame upNegative{10, 90, 50, 50};
static const Frame leftPositive{50, 50, 90, 10};
static const Frame leftNegative{50, 50, 10, 90};
struct Scenario {
  unsigned long now = 0, elapsed = 0, ioMs = 0;
  Batch batch;
  unsigned reads = 0, validityChecks = 0, levelReads = 0;
  int levelOverride = -1;
  bool valid = true, transferOK = true, overflow = false;
} scenario;

static void advance(unsigned long ms) {
  scenario.now += ms;
  scenario.elapsed += ms;
}
static unsigned long millis() { return scenario.now; }
[[maybe_unused]] static void delay(unsigned long) {
  throw std::runtime_error("incremental gesture reader must not sleep with the shared bus held");
}

struct I2CDevice {
  bool write_then_read(const uint8_t* prefix, size_t prefixLength,
                       uint8_t* buffer, size_t count) {
    check(prefixLength == 1 && *prefix == APDS9960_GFIFO_U, "read helper selects gesture FIFO");
    check(count == scenario.batch.size() * 4 && count > 0 && count <= 128,
          "FIFO dataset count is converted into complete four-byte records");
    check(++scenario.reads == 1, "one call performs at most one FIFO transfer");
    size_t offset = 0;
    for (const auto& frame : scenario.batch) {
      for (uint8_t value : frame) {
        if (scenario.transferOK || offset < 2) buffer[offset] = value;
        ++offset;
      }
    }
    advance(scenario.ioMs);
    // A failed transfer can leave two plausible direction bytes behind. They
    // must not be decoded, nor paired with a previous call's direction state.
    return scenario.transferOK;
  }
} i2cDevice;

class Adafruit_APDS9960 {
 public:
  // INSERT_COUNTERS
  I2CDevice* i2c_dev = &i2cDevice;
  struct GestureStatus { uint8_t GVALID = 0, GFOV = 0; } _gstatus;
  uint8_t readGesture();
  uint8_t read(uint8_t reg, uint8_t* buffer, uint8_t count);
  void resetCounts();
  bool gestureValid() {
    check(++scenario.validityChecks == 1, "one call checks gesture status at most once");
    advance(scenario.ioMs);
    _gstatus.GVALID = scenario.valid;
    _gstatus.GFOV = scenario.overflow;  // the same GSTATUS read reports overflow
    return scenario.valid;
  }
  uint8_t read8(uint8_t reg) {
    check(reg == APDS9960_GFLVL, "decoder requests FIFO level");
    check(++scenario.levelReads == 1, "one call checks FIFO level at most once");
    advance(scenario.ioMs);
    return scenario.levelOverride >= 0 ? static_cast<uint8_t>(scenario.levelOverride)
                                     : static_cast<uint8_t>(scenario.batch.size());
  }
};

// INSERT_METHODS

static void feed(Batch batch, unsigned long atMs = 5000, unsigned long ioMs = 0) {
  scenario = {};
  scenario.batch = std::move(batch);
  scenario.now = atMs;
  scenario.ioMs = ioMs;
}
static uint8_t readOnce(Adafruit_APDS9960& driver) {
  const uint8_t result = driver.readGesture();
  check(scenario.reads <= 1 && scenario.levelReads <= 1 && scenario.validityChecks <= 1,
        "one call performs at most three I/O operations");
  check(scenario.elapsed <= 3 * scenario.ioMs,
        "elapsed time is only the supplied I/O latency, with no polling wait");
  return result;
}
static void countsReset(const Adafruit_APDS9960& driver) {
  check(driver.gestCnt == 0 && driver.UCount == 0 && driver.DCount == 0 &&
        driver.LCount == 0 && driver.RCount == 0, "direction state is reset");
}

static void tests() {
  Adafruit_APDS9960 driver{};
  // Held motion over many calls cannot monopolize the bus. Each call samples
  // only the FIFO batch already available; time between calls belongs to caller.
  for (const auto& frame : {upPositive, upNegative, leftPositive, leftNegative}) {
    for (unsigned long ioMs : {0UL, 2UL, 17UL}) {
      for (unsigned long start : {0UL, 5000UL, ULONG_MAX - 100}) {
        driver.resetCounts();
        unsigned long atMs = start;
        for (int call = 0; call < 100; ++call) {
          feed(Batch(32, frame), atMs, ioMs);
          check(readOnce(driver) == 0, "held same-sign input is not a completed gesture");
          check(scenario.reads == 1, "ready input consumes one batch");
          atMs = scenario.now + 200;
        }
      }
    }
  }

  driver.resetCounts();
  feed({}, ULONG_MAX - 1, 2);
  scenario.valid = false;
  check(readOnce(driver) == 0, "no gesture data reports none");
  check(scenario.reads == 0 && scenario.levelReads == 0, "no-data exit does not read FIFO");
  countsReset(driver);
  for (unsigned long start : {0UL, 5000UL, ULONG_MAX - 100}) {
    driver.resetCounts();
    feed({neutral}, start, 2);
    check(readOnce(driver) == 0, "neutral sample is not a gesture");
    countsReset(driver);
  }
  for (int level : {0, 33, 255}) {
    driver.resetCounts();
    feed({}, ULONG_MAX - 100, 2);
    scenario.levelOverride = level;
    check(readOnce(driver) == 0, "empty/invalid FIFO is not a gesture");
    check(scenario.reads == 0, "empty/invalid FIFO does not perform data transfer");
    countsReset(driver);
  }

  struct Direction { Frame first, second; uint8_t expected; };
  const Direction directions[] = {
      {upPositive, upNegative, APDS9960_UP},
      {upNegative, upPositive, APDS9960_DOWN},
      {leftPositive, leftNegative, APDS9960_LEFT},
      {leftNegative, leftPositive, APDS9960_RIGHT}};
  for (const auto& direction : directions) {
    for (unsigned long start : {0UL, 5000UL, ULONG_MAX - 100}) {
      driver.resetCounts();
      feed({direction.first}, start, 2);
      check(readOnce(driver) == 0, "first directional sample is retained without waiting");
      feed({direction.second}, scenario.now + 200, 2);
      check(readOnce(driver) == direction.expected, "200ms-later reversal completes the gesture");
      countsReset(driver);

      // A no-data, empty FIFO or neutral sample must preserve recent partial
      // motion, while none of those paths should refresh its inactivity clock.
      for (int gapKind = 0; gapKind < 3; ++gapKind) {
        driver.resetCounts();
        feed({direction.first}, start, 2);
        check(readOnce(driver) == 0, "partial gesture starts");
        const unsigned long partialAt = scenario.now;
        feed(gapKind == 2 ? Batch{neutral} : Batch{}, partialAt + 100, 2);
        if (gapKind == 0) scenario.valid = false;
        check(readOnce(driver) == 0, "brief gap produces no gesture");
        feed({direction.second}, partialAt + 200, 2);
        check(readOnce(driver) == direction.expected, "recent partial gesture survives a short gap");
        countsReset(driver);

        feed({direction.first}, scenario.now + 10, 2);
        check(readOnce(driver) == 0, "new partial gesture starts");
        const unsigned long lastMotion = scenario.now;
        feed(gapKind == 2 ? Batch{neutral} : Batch{}, lastMotion + 200, 2);
        if (gapKind == 0) scenario.valid = false;
        check(readOnce(driver) == 0, "inactive gap produces no gesture");
        feed({direction.second}, lastMotion + 301, 2);
        check(readOnce(driver) == 0, "expired motion cannot pair with a later opposite sample");
      }

      driver.resetCounts();
      feed({direction.first}, start, 2);
      check(readOnce(driver) == 0, "partial gesture begins before inactivity");
      feed({}, scenario.now + 301, 2);
      scenario.valid = false;
      check(readOnce(driver) == 0, "expired no-data call returns immediately");
      countsReset(driver);
    }

    for (size_t datasets : {2U, 3U, 32U}) {
      driver.resetCounts();
      Batch batch(datasets, neutral);
      batch.front() = direction.first;
      batch.back() = direction.second;
      feed(batch, ULONG_MAX - 10, 2);
      check(readOnce(driver) == direction.expected, "reversal within a FIFO batch decodes direction");
      check(scenario.reads == 1, "complete in-batch gesture consumes one transfer");
      countsReset(driver);
    }

    for (size_t extraSameSign : {0U, 31U}) {
      driver.resetCounts();
      unsigned long atMs = ULONG_MAX - 200;
      for (int batch = 0; batch < 8; ++batch) {
        feed(Batch(32, direction.first), atMs);
        check(readOnce(driver) == 0, "full same-sign batches retain partial direction");
        atMs += 200;
      }
      Batch tail(extraSameSign, direction.first);
      tail.push_back(direction.second);
      feed(tail, atMs);
      check(readOnce(driver) == direction.expected,
            "direction marker survives 256+ samples across calls before reversal");
      countsReset(driver);
    }

    // Failed transfer or invalid level clears previous direction state; partial
    // transfer bytes cannot manufacture a gesture or contaminate the next read.
    for (int failure = 0; failure < 2; ++failure) {
      driver.resetCounts();
      feed({direction.first});
      check(readOnce(driver) == 0, "partial direction exists before failed read");
      feed({direction.second}, scenario.now + 100, 2);
      if (failure == 0) scenario.transferOK = false;
      else scenario.levelOverride = 33;
      check(readOnce(driver) == 0, "failed sample produces no gesture");
      countsReset(driver);
      feed({direction.second}, scenario.now + 100, 2);
      check(readOnce(driver) == 0, "failed read did not retain the preceding direction");
    }
    driver.resetCounts();
    feed({direction.first, direction.second}, 5000, 2);
    scenario.transferOK = false;
    check(readOnce(driver) == 0, "failed batch containing a reversal produces no gesture");
    countsReset(driver);

    // GFOV: the FIFO filled and later datasets were lost. The overflowed batch
    // still follows the previous call, so it can complete a swipe begun there,
    // but its own partial direction must not pair with samples after the gap.
    driver.resetCounts();
    feed({direction.first}, 5000, 2);
    scenario.overflow = true;
    check(readOnce(driver) == 0, "overflowed partial produces no gesture");
    countsReset(driver);
    feed({direction.second}, scenario.now + 100, 2);
    check(readOnce(driver) == 0, "a sample after an overflow gap does not pair with the lost half");
    driver.resetCounts();
    feed({direction.first}, 5000, 2);
    check(readOnce(driver) == 0, "partial before an overflowed batch");
    feed({direction.second}, scenario.now + 100, 2);
    scenario.overflow = true;
    check(readOnce(driver) == direction.expected, "an overflowed batch completes a swipe begun before it");
    countsReset(driver);
    driver.resetCounts();
    feed({direction.first}, 5000, 2);
    check(readOnce(driver) == 0, "partial before an empty overflowed read");
    feed({}, scenario.now + 50, 2);
    scenario.overflow = true;
    check(readOnce(driver) == 0, "an empty overflowed read produces no gesture");
    countsReset(driver);
  }
}

int main() {
  try {
    tests();
    std::cout << "APDS incremental gesture driver: single-batch bound, FIFO/errors, retained directions, inactivity and rollover passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "APDS gesture driver failure: " << error.what() << '\n';
    return 1;
  }
}
