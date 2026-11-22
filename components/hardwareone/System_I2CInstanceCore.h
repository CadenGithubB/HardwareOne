#pragma once

// Several instances of one I2C device type (multi-instance design, 2026-11-22).
//
// A running device is (type, n): n is 1-based and 1 is the default
// everywhere. Instance 1 keeps every id, key, index, command and event subject
// it had before instances existed; instance n >= 2 is `<id><n>` in machine ids
// ("encoder2") and "<Display> <n>" in text ("Encoder 2").
//
// Every table that was indexed by I2cDeviceType is indexed by slot instead:
// instance 1's slot is the type value itself, so every existing index is
// unchanged, and the extra slots (n >= 2) follow I2C_DEVICE_COUNT.
//
// Pure constexpr and inline, free of Arduino types: the host tests include it
// (test_i2c_instance_core.cpp) and run the same slot math, names and argument
// rules as the firmware.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "i2csensor_encoder_light.h"  // kEncoderMaxInstances

// The queue-managed I2C device types (System_I2C_Manager.h includes this).
enum I2cDeviceType : int {
  I2C_DEVICE_THERMAL = 0,
  I2C_DEVICE_TOF = 1,
  I2C_DEVICE_IMU = 2,
  I2C_DEVICE_INPUT = 3,
  I2C_DEVICE_GPS = 4,
  I2C_DEVICE_FMRADIO = 5,
  I2C_DEVICE_APDS = 6,
  I2C_DEVICE_RTC = 7,
  I2C_DEVICE_PRESENCE = 8,
  I2C_DEVICE_ENCODER = 9,  // Adafruit 5752/4991 encoder board pixels: no task, ticked by the main loop
  I2C_DEVICE_COUNT
};

// Host tests that need a second instance before any firmware type has one
// compile with HW1_HOST_I2C_TEST_ENCODER_MAX; no firmware build defines it.
#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
constexpr uint8_t kI2cEncoderMaxInstances = HW1_HOST_I2C_TEST_ENCODER_MAX;
#else
constexpr uint8_t kI2cEncoderMaxInstances = kEncoderMaxInstances;
#endif

// One row per queue type, in enum order. `id` is the word the commands use
// (open<id>, sensorautostart <id>); `displayName` is the text name of an
// instance n >= 2 ("Encoder 2"). Instance 1's display name stays
// deviceTypeDisplayName() (System_I2C.cpp), which knows the build's input
// device. The maximum is a code constant (owner question Q1), not a build
// switch.
struct I2cTypeInfo {
  I2cDeviceType type;
  const char* id;
  const char* displayName;
  uint8_t maxInstances;
};

constexpr I2cTypeInfo kI2cTypes[I2C_DEVICE_COUNT] = {
  { I2C_DEVICE_THERMAL,  "thermal",  "Thermal",  1 },
  { I2C_DEVICE_TOF,      "tof",      "ToF",      1 },
  { I2C_DEVICE_IMU,      "imu",      "IMU",      1 },
  { I2C_DEVICE_INPUT,    "input",    "Input",    1 },
  { I2C_DEVICE_GPS,      "gps",      "GPS",      1 },
  { I2C_DEVICE_FMRADIO,  "fmradio",  "FM Radio", 1 },
  { I2C_DEVICE_APDS,     "apds",     "APDS",     1 },
  { I2C_DEVICE_RTC,      "rtc",      "RTC",      1 },
  { I2C_DEVICE_PRESENCE, "presence", "Presence", 1 },
  { I2C_DEVICE_ENCODER,  "encoder",  "Encoder",  kI2cEncoderMaxInstances },
};

constexpr bool i2cTypesInEnumOrder() {
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
    if (kI2cTypes[i].type != static_cast<I2cDeviceType>(i)) return false;
    if (kI2cTypes[i].maxInstances < 1 || kI2cTypes[i].maxInstances > 9) return false;
    if (!kI2cTypes[i].id || !kI2cTypes[i].id[0]) return false;
    if (!kI2cTypes[i].displayName || !kI2cTypes[i].displayName[0]) return false;
  }
  return true;
}
static_assert(i2cTypesInEnumOrder(),
              "kI2cTypes: one row per I2cDeviceType in enum order, 1..9 instances, named");

constexpr bool i2cTypeValid(I2cDeviceType t) { return t >= 0 && t < I2C_DEVICE_COUNT; }

// 0 for a value outside the enum.
constexpr uint8_t i2cMaxInstances(I2cDeviceType t) {
  return i2cTypeValid(t) ? kI2cTypes[t].maxInstances : 0;
}

// First extra slot of a type, counted from I2C_DEVICE_COUNT.
constexpr uint8_t i2cExtraBase(I2cDeviceType t) {
  uint8_t base = 0;
  for (int i = 0; i < I2C_DEVICE_COUNT && i < t; ++i) base += kI2cTypes[i].maxInstances - 1;
  return base;
}

constexpr uint8_t i2cExtraSlotTotal() {
  uint8_t total = 0;
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) total += kI2cTypes[i].maxInstances - 1;
  return total;
}

constexpr uint8_t kI2cExtraSlots = i2cExtraSlotTotal();
constexpr uint8_t kI2cSlotCount = I2C_DEVICE_COUNT + kI2cExtraSlots;
// Length of a companion array for the extra slots (C++ has no empty arrays).
constexpr uint8_t kI2cExtraSlotStorage = kI2cExtraSlots ? kI2cExtraSlots : 1;
constexpr uint8_t kI2cNoSlot = 0xFF;

// Budgets: the RAM-flush failed mask for the extra slots is one 32-bit word
// (System_RamFlush.cpp). The health registry and connected-device list check
// their own budgets where their sizes are declared (System_I2C_Manager.h,
// System_I2C.h).
static_assert(kI2cExtraSlots <= 32, "extra instance slots must fit the RAM-flush failed word");
static_assert(kI2cSlotCount < kI2cNoSlot, "slot numbers must fit a byte");

// The slot of instance n of type t, or kI2cNoSlot when there is no such
// instance (a value outside the enum, n == 0, n above the type's maximum).
constexpr uint8_t i2cSlot(I2cDeviceType t, uint8_t n) {
  if (!i2cTypeValid(t) || n < 1 || n > kI2cTypes[t].maxInstances) return kI2cNoSlot;
  if (n == 1) return static_cast<uint8_t>(t);
  return static_cast<uint8_t>(I2C_DEVICE_COUNT + i2cExtraBase(t) + (n - 2));
}

// The (type, n) a slot belongs to. False for a slot outside 0..kI2cSlotCount-1.
inline bool i2cSlotToRef(uint8_t slot, I2cDeviceType& t, uint8_t& n) {
  if (slot < I2C_DEVICE_COUNT) {
    t = static_cast<I2cDeviceType>(slot);
    n = 1;
    return true;
  }
  if (slot >= kI2cSlotCount) return false;
  const uint8_t extra = static_cast<uint8_t>(slot - I2C_DEVICE_COUNT);
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
    const I2cDeviceType type = static_cast<I2cDeviceType>(i);
    const uint8_t base = i2cExtraBase(type);
    const uint8_t extras = static_cast<uint8_t>(kI2cTypes[i].maxInstances - 1);
    if (extra >= base && extra < base + extras) {
      t = type;
      n = static_cast<uint8_t>(extra - base + 2);
      return true;
    }
  }
  return false;
}

// Machine id: "encoder" for instance 1, "encoder2" for instance 2. Writes ""
// for an instance that does not exist. Returns buf.
inline const char* i2cInstanceId(I2cDeviceType t, uint8_t n, char* buf, size_t len) {
  if (!buf || len == 0) return "";
  buf[0] = '\0';
  if (i2cSlot(t, n) == kI2cNoSlot) return buf;
  if (n == 1) snprintf(buf, len, "%s", kI2cTypes[t].id);
  else snprintf(buf, len, "%s%u", kI2cTypes[t].id, (unsigned)n);
  return buf;
}

// Text name: "Encoder" for instance 1, "Encoder 2" for instance 2. Writes ""
// for an instance that does not exist. Returns buf.
inline const char* i2cInstanceName(I2cDeviceType t, uint8_t n, char* buf, size_t len) {
  if (!buf || len == 0) return "";
  buf[0] = '\0';
  if (i2cSlot(t, n) == kI2cNoSlot) return buf;
  if (n == 1) snprintf(buf, len, "%s", kI2cTypes[t].displayName);
  else snprintf(buf, len, "%s %u", kI2cTypes[t].displayName, (unsigned)n);
  return buf;
}

// ---------------------------------------------------------------------------
// The instance argument of a command (design section 3).
//
//   G1 open<x> [n], close<x> [n]      OpenClose: an integer token is the
//                                     instance; no token means 1.
//   G2 <x>status [n] [json]           Status: a leading integer token is the
//                                     instance; `json` may appear anywhere.
//   G3 <x><field> [n] <value>         Setter: 0 tokens shows instance 1, 1
//                                     token is the value for instance 1 (as
//                                     before), 2 tokens are instance then value.
//   G4 sensorautostart <sensor> [n] <on|off>
//                                     Setter after the sensor word; n = 1 is
//                                     accepted for every type.
//   G6 an instance token is decimal digits only (no 0x, no sign), 1..max.
//
// A lone integer is never read as an instance by a setter: parseBoolArg takes
// "1" and "0" (System_Utils.h), so `encoderautostart 1` keeps meaning "on".
// ---------------------------------------------------------------------------

enum class I2cInstanceForm : uint8_t { OpenClose, Status, Setter };

enum class I2cInstanceError : uint8_t {
  None,
  OutOfRange,      // an instance number outside 1..max of a multi-instance type
  SingleInstance,  // an instance number other than 1 for a type with one instance
  NotAnInstance,   // a token where an instance number belongs is not one
  TooMany,         // more tokens than the form takes
};

struct I2cInstanceArg {
  uint8_t n = 1;
  I2cInstanceError error = I2cInstanceError::None;
  // The rest of the arguments, trimmed, pointing into the parsed text: a
  // setter's value; for a status line the text after the instance token, or
  // the whole line when it names none.
  const char* rest = "";
  size_t restLen = 0;
  // The offending token for an error (points into the parsed text).
  const char* token = "";
  size_t tokenLen = 0;
};

inline bool i2cInstanceSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// One whitespace-separated token starting at or after *p; false at the end.
inline bool i2cInstanceNextToken(const char*& p, const char*& start, size_t& len) {
  while (*p && i2cInstanceSpace(*p)) ++p;
  if (!*p) return false;
  start = p;
  while (*p && !i2cInstanceSpace(*p)) ++p;
  len = static_cast<size_t>(p - start);
  return true;
}

// Decimal digits only. Values above 255 read as 255, which every range check
// refuses. False for anything that is not all digits.
inline bool i2cInstanceNumber(const char* token, size_t len, uint8_t& out) {
  if (!token || len == 0) return false;
  unsigned value = 0;
  for (size_t i = 0; i < len; ++i) {
    if (token[i] < '0' || token[i] > '9') return false;
    if (value < 1000) value = value * 10 + static_cast<unsigned>(token[i] - '0');
  }
  out = static_cast<uint8_t>(value > 255 ? 255 : value);
  return true;
}

inline bool i2cInstanceIsJsonWord(const char* token, size_t len) {
  return len == 4 && (token[0] == 'j' || token[0] == 'J') && (token[1] == 's' || token[1] == 'S') &&
         (token[2] == 'o' || token[2] == 'O') && (token[3] == 'n' || token[3] == 'N');
}

// Checks n against the type's instances and fills the error.
inline bool i2cInstanceInRange(I2cDeviceType t, uint8_t n, I2cInstanceArg& out) {
  out.n = n;
  if (i2cSlot(t, n) != kI2cNoSlot) return true;
  out.error = i2cMaxInstances(t) > 1 ? I2cInstanceError::OutOfRange : I2cInstanceError::SingleInstance;
  return false;
}

inline I2cInstanceArg i2cParseInstanceArg(I2cDeviceType t, const char* args, I2cInstanceForm form) {
  I2cInstanceArg out;
  if (!args) args = "";
  const char* p = args;
  const char* first = nullptr;
  size_t firstLen = 0;

  auto setRest = [&](const char* from) {
    while (*from && i2cInstanceSpace(*from)) ++from;
    size_t len = strlen(from);
    while (len > 0 && i2cInstanceSpace(from[len - 1])) --len;
    out.rest = from;
    out.restLen = len;
  };
  auto fail = [&](I2cInstanceError error, const char* token, size_t len) {
    out.error = error;
    out.token = token;
    out.tokenLen = len;
    return out;
  };

  switch (form) {
    case I2cInstanceForm::OpenClose: {
      if (!i2cInstanceNextToken(p, first, firstLen)) return out;  // no token: instance 1
      uint8_t n = 0;
      if (!i2cInstanceNumber(first, firstLen, n)) {
        return fail(I2cInstanceError::NotAnInstance, first, firstLen);
      }
      const char* extra = nullptr;
      size_t extraLen = 0;
      const char* after = p;
      if (i2cInstanceNextToken(after, extra, extraLen)) {
        return fail(I2cInstanceError::TooMany, extra, extraLen);
      }
      if (!i2cInstanceInRange(t, n, out)) {
        out.token = first;
        out.tokenLen = firstLen;
      }
      return out;
    }
    case I2cInstanceForm::Status: {
      // The first word that is not `json` is the instance when it is a number;
      // anything else stays in the rest for the command.
      const char* scan = p;
      while (i2cInstanceNextToken(scan, first, firstLen)) {
        if (i2cInstanceIsJsonWord(first, firstLen)) continue;
        uint8_t n = 0;
        if (i2cInstanceNumber(first, firstLen, n)) {
          if (!i2cInstanceInRange(t, n, out)) {
            out.token = first;
            out.tokenLen = firstLen;
            return out;
          }
          // The rest is the text after the instance; the command reads `json`
          // from the whole line (argWantsJson), wherever it stands.
          setRest(scan);
          out.token = first;
          out.tokenLen = firstLen;
          return out;
        }
        break;
      }
      setRest(args);
      return out;
    }
    case I2cInstanceForm::Setter: {
      if (!i2cInstanceNextToken(p, first, firstLen)) return out;  // show instance 1
      const char* second = nullptr;
      size_t secondLen = 0;
      const char* after = p;
      if (!i2cInstanceNextToken(after, second, secondLen)) {
        setRest(first);  // one token: the value for instance 1
        return out;
      }
      uint8_t n = 0;
      if (!i2cInstanceNumber(first, firstLen, n)) {
        return fail(I2cInstanceError::NotAnInstance, first, firstLen);
      }
      if (!i2cInstanceInRange(t, n, out)) {
        out.token = first;
        out.tokenLen = firstLen;
        return out;
      }
      setRest(p);
      return out;
    }
  }
  return out;
}

// True when the one token a setter got as its value would name an instance
// (for the "To set instance n" hint when the value is not valid).
inline bool i2cInstanceTokenIsInstance(I2cDeviceType t, const char* token, size_t len) {
  uint8_t n = 0;
  return i2cInstanceNumber(token, len, n) && n >= 2 && i2cSlot(t, n) != kI2cNoSlot;
}

// One-line error replies (design section 3.2). `usage` is the command's usage
// ("openencoder [n]"). Returns buf; "" for a parse without an error.
inline const char* i2cInstanceErrorText(I2cDeviceType t, const I2cInstanceArg& arg, const char* usage,
                                        char* buf, size_t len) {
  if (!buf || len == 0) return "";
  buf[0] = '\0';
  if (!i2cTypeValid(t)) return buf;
  const char* id = kI2cTypes[t].id;
  if (!usage) usage = "";
  const int tokenLen = static_cast<int>(arg.tokenLen > 32 ? 32 : arg.tokenLen);
  switch (arg.error) {
    case I2cInstanceError::None:
      break;
    case I2cInstanceError::OutOfRange:
      snprintf(buf, len, "Error: %s has instances 1-%u. Usage: %s", id,
               (unsigned)kI2cTypes[t].maxInstances, usage);
      break;
    case I2cInstanceError::SingleInstance:
      snprintf(buf, len, "Error: %s has one instance.", id);
      break;
    case I2cInstanceError::NotAnInstance:
      snprintf(buf, len, "Error: '%.*s' is not an instance number. Usage: %s", tokenLen,
               arg.token ? arg.token : "", usage);
      break;
    case I2cInstanceError::TooMany:
      snprintf(buf, len, "Error: '%.*s' is one argument too many. Usage: %s", tokenLen,
               arg.token ? arg.token : "", usage);
      break;
  }
  return buf;
}

// A setter's single token was not a valid value but names an instance:
// "Error: '2' is not on or off. To set instance 2: encoderautostart 2 on".
// `expected` says what the value should be ("on or off"), `example` is a valid
// value for the hint ("on").
inline const char* i2cInstanceNotValueText(const char* token, size_t tokenLen, const char* expected,
                                           const char* command, const char* example,
                                           char* buf, size_t len) {
  if (!buf || len == 0) return "";
  const int shown = static_cast<int>(tokenLen > 32 ? 32 : tokenLen);
  snprintf(buf, len, "Error: '%.*s' is not %s. To set instance %.*s: %s %.*s %s", shown,
           token ? token : "", expected ? expected : "a valid value", shown, token ? token : "",
           command ? command : "", shown, token ? token : "", example ? example : "");
  return buf;
}

// An open of an instance with no address:
// "Error: encoder 2 has no address. Set one: encoderaddress 2 <addr>".
inline const char* i2cInstanceNoAddressText(I2cDeviceType t, uint8_t n, const char* addressCommand,
                                            char* buf, size_t len) {
  if (!buf || len == 0) return "";
  buf[0] = '\0';
  if (!i2cTypeValid(t)) return buf;
  snprintf(buf, len, "Error: %s %u has no address. Set one: %s %u <addr>", kI2cTypes[t].id,
           (unsigned)n, addressCommand ? addressCommand : "", (unsigned)n);
  return buf;
}

// ---------------------------------------------------------------------------
// Rows of the device table (design section 1.3).
//
// An i2cSensors[] row (System_I2C.h) names each of its instances' bus and
// address: busAt(n) and addressAt(n), with `instances` the number it can be
// configured with (1 except on a multi-instance row). The functions below
// read any row with the fields they use (address, multiAddress, altAddress,
// compiled, busAt, addressAt, instances), so the host tests run them on rows
// of their own.
// ---------------------------------------------------------------------------

// The most instances any type can have: the size of a per-row instance table.
constexpr uint8_t i2cMaxInstancesOfAnyType() {
  uint8_t most = 1;
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
    if (kI2cTypes[i].maxInstances > most) most = kI2cTypes[i].maxInstances;
  }
  return most;
}
constexpr uint8_t kI2cRowInstanceCap = i2cMaxInstancesOfAnyType();

// The instances a row has, capped at kI2cRowInstanceCap (a row cannot have
// more than its type).
template <typename Row>
inline uint8_t i2cRowInstances(const Row& row) {
  if (row.instances < 1) return 1;
  return row.instances > kI2cRowInstanceCap ? kI2cRowInstanceCap : row.instances;
}

// Instance n's address: its setting when the row has one (0 = not
// configured), else the row's fixed address for instance 1. 0 for an n the
// row does not have.
template <typename Row>
inline uint8_t i2cRowAddressAt(const Row& row, uint8_t n) {
  if (n < 1 || n > i2cRowInstances(row)) return 0;
  if (row.addressAt) return row.addressAt(n);
  return n == 1 ? row.address : 0;
}

// Instance n's bus, or -1 for a row without a bus setting (it claims either
// bus).
template <typename Row>
inline int i2cRowBusAt(const Row& row, uint8_t n) {
  return row.busAt ? row.busAt(n) : -1;
}

// Which of the row's instances is configured at (address, bus): 1..instances,
// or 0 when none is. Instance 1 also answers at the row's alternate address.
template <typename Row>
inline uint8_t i2cRowInstanceAt(const Row& row, uint8_t address, uint8_t bus) {
  if (address == 0) return 0;
  const uint8_t instances = i2cRowInstances(row);
  for (uint8_t n = 1; n <= instances; ++n) {
    const bool at = i2cRowAddressAt(row, n) == address ||
                    (n == 1 && row.multiAddress && row.altAddress == address);
    if (!at) continue;
    const int rowBus = i2cRowBusAt(row, n);
    if (rowBus < 0 || rowBus == static_cast<int>(bus)) return n;
  }
  return 0;
}

// A compiled row claims (address, bus) when one of its instances is
// configured there.
template <typename Row>
inline bool i2cRowClaims(const Row& row, uint8_t address, uint8_t bus) {
  return row.compiled && i2cRowInstanceAt(row, address, bus) != 0;
}

// A compiled row claims (address, bus) at a configured instance's own
// address, not instance 1's alternate: the claim a scan label prefers.
template <typename Row>
inline bool i2cRowClaimsOwnAddress(const Row& row, uint8_t address, uint8_t bus) {
  if (!row.compiled || address == 0) return false;
  const uint8_t instances = i2cRowInstances(row);
  for (uint8_t n = 1; n <= instances; ++n) {
    if (i2cRowAddressAt(row, n) != address) continue;
    const int rowBus = i2cRowBusAt(row, n);
    if (rowBus < 0 || rowBus == static_cast<int>(bus)) return true;
  }
  return false;
}

// Whether `address` is one of the row's addresses, on any bus: a configured
// instance's address or instance 1's alternate address.
template <typename Row>
inline bool i2cRowHasAddress(const Row& row, uint8_t address) {
  if (address == 0) return false;
  const uint8_t instances = i2cRowInstances(row);
  for (uint8_t n = 1; n <= instances; ++n) {
    if (i2cRowAddressAt(row, n) == address) return true;
  }
  return row.multiAddress && row.altAddress == address;
}

// The rows a scan label names for what answered at (address, bus) when no
// product id told it apart, in table order (up to cap indices in out; returns
// how many): the first compiled row configured at that address on that bus,
// alone, a row's own address before another row's alternate (the ToF at 0x29
// is not the IMU at its alternate 0x29); otherwise every row with that
// address on any bus, since the address cannot tell them apart (a device
// wired to one bus, such as the fuel gauge, names only that bus: at 0x36 on
// another bus it is one of several). 0 is an unknown device. include(i)
// false leaves row i out.
template <typename Row, typename Include>
inline size_t i2cAddressLabelRows(const Row* rows, size_t count, uint8_t address, uint8_t bus,
                                  Include include, int* out, size_t cap) {
  if (!rows || !out || cap == 0 || address == 0) return 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < count; ++i) {
      if (!include(i)) continue;
      if (pass == 0 ? i2cRowClaimsOwnAddress(rows[i], address, bus)
                    : i2cRowClaims(rows[i], address, bus)) {
        out[0] = static_cast<int>(i);
        return 1;
      }
    }
  }
  size_t found = 0;
  for (size_t i = 0; i < count && found < cap; ++i) {
    if (include(i) && i2cRowHasAddress(rows[i], address)) out[found++] = static_cast<int>(i);
  }
  return found;
}

// The row an address is labelled with by address alone (a label is a hint;
// drivers check identity): instance 1's address first in table order, then a
// configured instance n >= 2, then an alternate address. -1 when no row has
// the address.
template <typename Row>
inline int i2cRowIndexByAddress(const Row* rows, size_t count, uint8_t address) {
  if (!rows || address == 0) return -1;
  for (size_t i = 0; i < count; ++i) {
    if (i2cRowAddressAt(rows[i], 1) == address) return static_cast<int>(i);
  }
  for (size_t i = 0; i < count; ++i) {
    const uint8_t instances = i2cRowInstances(rows[i]);
    for (uint8_t n = 2; n <= instances; ++n) {
      if (i2cRowAddressAt(rows[i], n) == address) return static_cast<int>(i);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (rows[i].multiAddress && rows[i].altAddress == address) return static_cast<int>(i);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Address collisions (design section 1.3).
//
// One address in use on a bus: a configured instance of a queue type at the
// address its driver uses, or a device without a queue type (fuel gauge,
// OLED, PWM driver, ...) compiled into the build. A setter and an open refuse
// an address another use already has on that bus (System_I2C.cpp lists the
// uses, i2cAddressTakenBy()).
// ---------------------------------------------------------------------------

struct I2cAddressUse {
  uint8_t bus;
  uint8_t address;
  I2cDeviceType type;  // the queue type of an instance; I2C_DEVICE_COUNT for a device without one
  uint8_t n;           // the instance (1..max); 0 for a device without a queue type
  const char* name;    // the device's name ("MAX17048")
  const char* detail;  // what it is ("LiPo Fuel Gauge"), or nullptr
};

// Whether `use` holds (bus, address) for someone other than instance
// (exceptType, exceptN), the one asking.
inline bool i2cAddressUseClashes(const I2cAddressUse& use, uint8_t bus, uint8_t address,
                                 I2cDeviceType exceptType, uint8_t exceptN) {
  if (address == 0 || use.bus != bus || use.address != address) return false;
  return !(use.type == exceptType && use.n == exceptN);
}

// The first use in `uses` that holds (bus, address) for someone else, or
// nullptr when the address is free there.
inline const I2cAddressUse* i2cAddressTakenBy(const I2cAddressUse* uses, size_t count, uint8_t bus,
                                              uint8_t address, I2cDeviceType exceptType,
                                              uint8_t exceptN) {
  if (!uses) return nullptr;
  for (size_t i = 0; i < count; ++i) {
    if (i2cAddressUseClashes(uses[i], bus, address, exceptType, exceptN)) return &uses[i];
  }
  return nullptr;
}

// "Error: 0x37 on bus 0 is in use by Encoder (instance 1)." for an instance of
// a type that can have several; "Error: 0x36 on bus 0 is in use by MAX17048
// (LiPo Fuel Gauge)." for any other device. Returns buf.
inline const char* i2cAddressClashText(const I2cAddressUse& use, char* buf, size_t len) {
  if (!buf || len == 0) return "";
  if (i2cTypeValid(use.type) && kI2cTypes[use.type].maxInstances > 1) {
    snprintf(buf, len, "Error: 0x%02X on bus %u is in use by %s (instance %u).",
             (unsigned)use.address, (unsigned)use.bus, kI2cTypes[use.type].displayName,
             (unsigned)use.n);
  } else if (use.detail && use.detail[0]) {
    snprintf(buf, len, "Error: 0x%02X on bus %u is in use by %s (%s).", (unsigned)use.address,
             (unsigned)use.bus, use.name ? use.name : "another device", use.detail);
  } else {
    snprintf(buf, len, "Error: 0x%02X on bus %u is in use by %s.", (unsigned)use.address,
             (unsigned)use.bus, use.name ? use.name : "another device");
  }
  return buf;
}
