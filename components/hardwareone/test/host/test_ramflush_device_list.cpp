// The RAM flush's I2C device list (System_RamFlushDeviceList.h): the 2-byte
// entry, the list a capture builds, the check the boot after it makes per
// instance, and the CRC that covers the list. The firmware
// (System_RamFlush.cpp) runs this same code.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "System_I2CInstanceCore.h"
#include "System_RamFlushDeviceList.h"

static RamFlushDevice dev(uint8_t type, uint8_t n, uint8_t bus, uint8_t address) {
  RamFlushDevice d;
  d.type = type;
  d.n = n;
  d.bus = bus;
  d.address = address;
  return d;
}

// Every field survives the two bytes, at both ends of its range.
static void testEncodeRoundTrip() {
  const RamFlushDevice cases[] = {
    dev(0, 1, 0, 0x33), dev(15, 15, 1, 0x7F), dev(I2C_DEVICE_ENCODER, 2, 1, 0x38),
    dev(I2C_DEVICE_INPUT, 1, 0, 0x00),  // the GT911 has no one fixed address
  };
  for (const RamFlushDevice& d : cases) {
    uint8_t bytes[2] = {0xAA, 0xAA};
    assert(ramFlushDeviceEncode(d, bytes));
    RamFlushDevice back;
    assert(ramFlushDeviceDecode(bytes, back));
    assert(back.type == d.type && back.n == d.n && back.bus == d.bus && back.address == d.address);
  }
  // The documented bit layout.
  uint8_t bytes[2];
  assert(ramFlushDeviceEncode(dev(9, 2, 1, 0x38), bytes));
  assert(bytes[0] == 0x92 && bytes[1] == 0xB8);
}

// A value outside an entry's fields is refused, nothing written.
static void testEncodeRefuses() {
  const RamFlushDevice bad[] = {
    dev(16, 1, 0, 0x10), dev(0, 0, 0, 0x10), dev(0, 16, 0, 0x10), dev(0, 1, 2, 0x10), dev(0, 1, 0, 0x80),
  };
  for (const RamFlushDevice& d : bad) {
    uint8_t bytes[2] = {0x5A, 0x5A};
    assert(!ramFlushDeviceEncode(d, bytes));
    assert(bytes[0] == 0x5A && bytes[1] == 0x5A);
  }
  assert(!ramFlushDeviceEncode(dev(0, 1, 0, 0x10), nullptr));
  // Instance 0 is never written, so it never reads back as an entry.
  const uint8_t zero[2] = {0, 0};
  RamFlushDevice d;
  assert(!ramFlushDeviceDecode(zero, d));
}

// The list: append, find, no instance twice, a full list refuses.
static void testListAddFind() {
  uint8_t bytes[4 * kRamFlushDeviceEntryBytes] = {};
  uint8_t count = 0;
  assert(ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_THERMAL, 1, 0, 0x33)));
  assert(ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_ENCODER, 1, 0, 0x49)));
  assert(ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_ENCODER, 3, 1, 0x38)));
  assert(count == 3);
  assert(!ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_ENCODER, 3, 0, 0x39)));  // listed already
  assert(!ramFlushDeviceListAdd(bytes, count, 4, dev(0, 1, 2, 0x10)));                   // does not fit
  assert(count == 3);
  assert(ramFlushDeviceListFind(bytes, count, I2C_DEVICE_THERMAL, 1) == 0);
  assert(ramFlushDeviceListFind(bytes, count, I2C_DEVICE_ENCODER, 3) == 2);
  assert(ramFlushDeviceListFind(bytes, count, I2C_DEVICE_ENCODER, 2) == -1);
  assert(ramFlushDeviceListFind(bytes, count, I2C_DEVICE_TOF, 1) == -1);
  assert(ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_GPS, 1, 0, 0x10)));
  assert(!ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_RTC, 1, 0, 0x68)));  // full
  assert(count == 4);
  assert(ramFlushDeviceListValid(bytes, count, 4));
}

// Several instances of one type are separate entries: a type that gains
// instances later (PCA9685, HT16K33) needs no layout change.
static void testManyInstancesOneType() {
  uint8_t bytes[24 * kRamFlushDeviceEntryBytes] = {};
  uint8_t count = 0;
  for (uint8_t n = 1; n <= 8; ++n) {
    assert(ramFlushDeviceListAdd(bytes, count, 24, dev(11, n, n & 1, static_cast<uint8_t>(0x70 + n - 1))));
  }
  for (uint8_t n = 1; n <= 8; ++n) assert(ramFlushDeviceListFind(bytes, count, 11, n) == n - 1);
  assert(ramFlushDeviceListValid(bytes, count, 24));
  // The device list holds every instance slot of this build.
  assert(kI2cSlotCount <= 24);
  assert(I2C_DEVICE_COUNT <= kRamFlushDeviceTypeLimit);
}

// Read back from RTC: more entries than the space, an entry no capture
// writes, or an instance twice is not a list.
static void testListValid() {
  uint8_t bytes[3 * kRamFlushDeviceEntryBytes] = {};
  assert(ramFlushDeviceListValid(bytes, 0, 3));
  assert(!ramFlushDeviceListValid(bytes, 4, 3));
  assert(!ramFlushDeviceListValid(bytes, 1, 3));  // zero bytes: instance 0
  uint8_t count = 0;
  assert(ramFlushDeviceListAdd(bytes, count, 3, dev(1, 1, 0, 0x29)));
  assert(ramFlushDeviceListAdd(bytes, count, 3, dev(2, 1, 0, 0x28)));
  assert(ramFlushDeviceListValid(bytes, count, 3));
  std::memcpy(bytes + 2 * kRamFlushDeviceEntryBytes, bytes, kRamFlushDeviceEntryBytes);
  assert(!ramFlushDeviceListValid(bytes, 3, 3));  // instance listed twice
}

// What the boot after a flush does with each instance.
static void testCheck() {
  uint8_t bytes[4 * kRamFlushDeviceEntryBytes] = {};
  uint8_t count = 0;
  assert(ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_THERMAL, 1, 0, 0x33)));
  assert(ramFlushDeviceListAdd(bytes, count, 4, dev(I2C_DEVICE_ENCODER, 2, 1, 0x38)));

  // No flush this boot: start-at-boot decides, the list is not looked at.
  auto v = ramFlushDeviceCheck(false, bytes, count, I2C_DEVICE_TOF, 1, 0, 0x29);
  assert(v == RamFlushDeviceVerdict::NoList);
  assert(ramFlushDeviceVerdictOpens(v, true) && !ramFlushDeviceVerdictOpens(v, false));

  // Listed, same bus and address: reopened, whatever its start-at-boot says.
  RamFlushDevice saved;
  v = ramFlushDeviceCheck(true, bytes, count, I2C_DEVICE_ENCODER, 2, 1, 0x38, &saved);
  assert(v == RamFlushDeviceVerdict::Listed && saved.bus == 1 && saved.address == 0x38);
  assert(ramFlushDeviceVerdictOpens(v, false) && ramFlushDeviceVerdictOpens(v, true));

  // Not listed: stays closed even with start-at-boot on (board 1 here).
  v = ramFlushDeviceCheck(true, bytes, count, I2C_DEVICE_ENCODER, 1, 0, 0x49);
  assert(v == RamFlushDeviceVerdict::NotListed && !ramFlushDeviceVerdictOpens(v, true));

  // Listed, but its bus or address setting changed: stays closed.
  v = ramFlushDeviceCheck(true, bytes, count, I2C_DEVICE_ENCODER, 2, 0, 0x38, &saved);
  assert(v == RamFlushDeviceVerdict::BusChanged && saved.bus == 1 && !ramFlushDeviceVerdictOpens(v, true));
  v = ramFlushDeviceCheck(true, bytes, count, I2C_DEVICE_ENCODER, 2, 1, 0x39, &saved);
  assert(v == RamFlushDeviceVerdict::AddressChanged && saved.address == 0x38);
  assert(!ramFlushDeviceVerdictOpens(v, true));

  // An empty list (nothing ran) keeps every I2C device closed.
  v = ramFlushDeviceCheck(true, bytes, 0, I2C_DEVICE_THERMAL, 1, 0, 0x33);
  assert(v == RamFlushDeviceVerdict::NotListed && !ramFlushDeviceVerdictOpens(v, true));
}

// Who a capture lists: what runs, and what failed while meant to be on.
static void testShouldList() {
  assert(ramFlushDeviceShouldList(true, false, false));
  assert(ramFlushDeviceShouldList(true, true, false));
  assert(ramFlushDeviceShouldList(false, true, true));    // failed start: tried again after the flush
  assert(!ramFlushDeviceShouldList(false, true, false));  // failed, but not meant to be on
  assert(!ramFlushDeviceShouldList(false, false, true));  // closed by hand: stays closed
}

// The CRC covers the masks, the layout version and every list byte.
static void testCrc() {
  uint8_t bytes[2 * kRamFlushDeviceEntryBytes] = {};
  uint8_t count = 0;
  assert(ramFlushDeviceListAdd(bytes, count, 2, dev(I2C_DEVICE_ENCODER, 2, 1, 0x38)));
  const uint16_t base = ramFlushOverlayCrc(0x5, 0x1, 3, count, bytes);
  assert(base == ramFlushOverlayCrc(0x5, 0x1, 3, count, bytes));
  assert(base != ramFlushOverlayCrc(0x4, 0x1, 3, count, bytes));
  assert(base != ramFlushOverlayCrc(0x5, 0x0, 3, count, bytes));
  assert(base != ramFlushOverlayCrc(0x5, 0x1, 2, count, bytes));  // an older layout
  assert(base != ramFlushOverlayCrc(0x5, 0x1, 3, 0, bytes));
  for (size_t i = 0; i < kRamFlushDeviceEntryBytes; ++i) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
      bytes[i] ^= static_cast<uint8_t>(1u << bit);
      assert(base != ramFlushOverlayCrc(0x5, 0x1, 3, count, bytes));
      bytes[i] ^= static_cast<uint8_t>(1u << bit);
    }
  }
}

int main() {
  testEncodeRoundTrip();
  testEncodeRefuses();
  testListAddFind();
  testManyInstancesOneType();
  testListValid();
  testCheck();
  testShouldList();
  testCrc();
  std::puts("ramflush_device_list_tests: all passed");
  return 0;
}
