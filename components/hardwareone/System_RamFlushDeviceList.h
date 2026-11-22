#pragma once

// The RAM flush's I2C device list (owner decision 2026-11-22, morning Q1).
//
// A `ramflush` writes, next to its feature bit masks, the list of I2C devices
// that were running: one 2-byte entry per device, up to the device registry's
// maximum, under the overlay's CRC (System_RamFlush.cpp). The boot after it
// reopens exactly the listed devices, each only when its saved bus and address
// still match its settings; every other I2C device stays closed for that boot,
// whatever its start-at-boot says. After boot the list only holds where it
// differed from start-at-boot; a later change to the setting applies
// (ramFlushDeviceVerdictOpens).
//
// An entry names (type, instance n, bus, 7-bit address):
//
//   byte 0   bits 7-4  type    I2cDeviceType, 0..15
//            bits 3-0  n       the instance, 1..15 (0 is never written)
//   byte 1   bit 7     bus     0 or 1
//            bits 6-0  address the address its start uses; 0 for a driver
//                              without one fixed address (the GT911 probes two)
//
// A type with several instances (the encoder today; PCA9685, HT16K33 later)
// needs no layout change: each running instance is one more entry.
//
// Pure and free of Arduino types: the host tests include it
// (test_ramflush_device_list.cpp) and run the same encoding, lookup, check
// and CRC as the firmware.

#include <stddef.h>
#include <stdint.h>

struct RamFlushDevice {
  uint8_t type = 0;
  uint8_t n = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
};

constexpr uint8_t kRamFlushDeviceTypeLimit = 16;      // type values 0..15
constexpr uint8_t kRamFlushDeviceInstanceLimit = 15;  // instances 1..15
constexpr uint8_t kRamFlushDeviceBusLimit = 2;        // buses 0 and 1
constexpr size_t kRamFlushDeviceEntryBytes = 2;

inline bool ramFlushDeviceEncodable(const RamFlushDevice& d) {
  return d.type < kRamFlushDeviceTypeLimit && d.n >= 1 && d.n <= kRamFlushDeviceInstanceLimit &&
         d.bus < kRamFlushDeviceBusLimit && d.address < 0x80;
}

// Writes the two bytes of `d`; false (nothing written) when it does not fit.
inline bool ramFlushDeviceEncode(const RamFlushDevice& d, uint8_t* out) {
  if (!out || !ramFlushDeviceEncodable(d)) return false;
  out[0] = static_cast<uint8_t>((d.type << 4) | d.n);
  out[1] = static_cast<uint8_t>((d.bus << 7) | d.address);
  return true;
}

// Reads one entry; false for bytes no capture writes (instance 0).
inline bool ramFlushDeviceDecode(const uint8_t* in, RamFlushDevice& d) {
  if (!in) return false;
  d.type = static_cast<uint8_t>(in[0] >> 4);
  d.n = static_cast<uint8_t>(in[0] & 0x0F);
  d.bus = static_cast<uint8_t>(in[1] >> 7);
  d.address = static_cast<uint8_t>(in[1] & 0x7F);
  return d.n >= 1;
}

// The index of instance n of `type` in the list, or -1.
inline int ramFlushDeviceListFind(const uint8_t* bytes, uint8_t count, uint8_t type, uint8_t n) {
  if (!bytes) return -1;
  for (uint8_t i = 0; i < count; ++i) {
    RamFlushDevice d;
    if (ramFlushDeviceDecode(bytes + i * kRamFlushDeviceEntryBytes, d) && d.type == type && d.n == n) {
      return i;
    }
  }
  return -1;
}

// Appends `d`. False when the list is full, `d` does not fit an entry, or
// the instance is listed already.
inline bool ramFlushDeviceListAdd(uint8_t* bytes, uint8_t& count, uint8_t cap, const RamFlushDevice& d) {
  if (!bytes || count >= cap || !ramFlushDeviceEncodable(d)) return false;
  if (ramFlushDeviceListFind(bytes, count, d.type, d.n) >= 0) return false;
  ramFlushDeviceEncode(d, bytes + count * kRamFlushDeviceEntryBytes);
  ++count;
  return true;
}

// A list read back from RTC: no more entries than the space, each one an
// entry a capture writes, no instance twice.
inline bool ramFlushDeviceListValid(const uint8_t* bytes, uint8_t count, uint8_t cap) {
  if (count > cap) return false;
  if (count > 0 && !bytes) return false;
  for (uint8_t i = 0; i < count; ++i) {
    RamFlushDevice d;
    if (!ramFlushDeviceDecode(bytes + i * kRamFlushDeviceEntryBytes, d)) return false;
    if (ramFlushDeviceListFind(bytes, i, d.type, d.n) >= 0) return false;
  }
  return true;
}

// Whether a capture lists an instance: it is running, or it failed to start
// while it was meant to be on this session (start-at-boot as the boot
// resolved it). A failed device is not the user's doing, so it gets its
// start-at-boot again after the flush instead of being left closed.
inline bool ramFlushDeviceShouldList(bool running, bool failedMark, bool meantOn) {
  return running || (failedMark && meantOn);
}

// What the boot after a flush does with one instance.
enum class RamFlushDeviceVerdict : uint8_t {
  NoList,          // no flush list this boot: start-at-boot decides
  Listed,          // listed, same bus and address: reopen it
  NotListed,       // a flush list is in force and this instance is not on it: stays closed
  BusChanged,      // listed, but its bus setting differs from the saved one: stays closed
  AddressChanged,  // listed, but its address differs from the saved one: stays closed
};

inline RamFlushDeviceVerdict ramFlushDeviceCheck(bool listActive, const uint8_t* bytes, uint8_t count,
                                                 uint8_t type, uint8_t n, uint8_t bus, uint8_t address,
                                                 RamFlushDevice* saved = nullptr) {
  if (!listActive) return RamFlushDeviceVerdict::NoList;
  const int at = ramFlushDeviceListFind(bytes, count, type, n);
  if (at < 0) return RamFlushDeviceVerdict::NotListed;
  RamFlushDevice d;
  ramFlushDeviceDecode(bytes + at * kRamFlushDeviceEntryBytes, d);
  if (saved) *saved = d;
  if (d.bus != bus) return RamFlushDeviceVerdict::BusChanged;
  if (d.address != address) return RamFlushDeviceVerdict::AddressChanged;
  return RamFlushDeviceVerdict::Listed;
}

// Open it? `intentAtBoot` is its start-at-boot when this boot consumed the
// list, `intentNow` the setting at this call. The list is a boot decision:
// it overrides only where it differs from the boot's start-at-boot (the
// instance diverged at the flush, as the bit masks work). Where it agrees,
// the setting decides, so a later `<x>autostart 0/1`, the setup wizard or a
// settings import still applies to the session-long callers (the automatic
// restart gate, the OLED input re-resolve, the encoder's reconnect).
inline bool ramFlushDeviceVerdictOpens(RamFlushDeviceVerdict v, bool intentAtBoot, bool intentNow) {
  if (v == RamFlushDeviceVerdict::NoList) return intentNow;
  const bool listOpens = (v == RamFlushDeviceVerdict::Listed);
  return listOpens == intentAtBoot ? intentNow : listOpens;
}

// The overlay's check value over its masks, layout version and device list.
// Not protection against an attacker: it rejects RTC that holds uninitialised
// garbage rather than something a capture wrote. FNV-1a, folded to 16 bits.
inline uint16_t ramFlushOverlayCrc(uint32_t diverge, uint32_t live, uint16_t layout, uint8_t count,
                                   const uint8_t* bytes) {
  uint32_t h = 0x811C9DC5u;
  auto mix = [&h](uint32_t part) {
    h ^= part;
    h *= 0x01000193u;
  };
  mix(diverge);
  mix(live);
  mix(layout);
  mix(count);
  for (size_t i = 0; bytes && i < static_cast<size_t>(count) * kRamFlushDeviceEntryBytes; ++i) mix(bytes[i]);
  return static_cast<uint16_t>((h >> 16) ^ (h & 0xFFFFu));
}
