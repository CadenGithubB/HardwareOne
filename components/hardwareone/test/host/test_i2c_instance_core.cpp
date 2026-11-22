// Several instances of one I2C device type: the slot math, ids, names, the
// instance argument rules, the device-table rows per instance (identify and
// claims) and address collisions (System_I2CInstanceCore.h, design sections
// 1.1, 1.3, 3).
//
// Built twice: with the firmware's maxima (the encoder four instances since
// phase 1c, every other type one) and with HW1_HOST_I2C_TEST_ENCODER_MAX=4,
// which runs the cases written for four encoder instances.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "System_I2CInstanceCore.h"

static std::string str(const char* text, size_t len) { return std::string(text, len); }

[[maybe_unused]] static std::string rest(const I2cInstanceArg& arg) { return str(arg.rest, arg.restLen); }

static std::string errorText(I2cDeviceType t, const I2cInstanceArg& arg, const char* usage) {
  char buf[160];
  return i2cInstanceErrorText(t, arg, usage, buf, sizeof(buf));
}

// Instance 1 is every type's own slot, under any maxima.
static void testInstanceOneIsTheType() {
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
    const auto t = static_cast<I2cDeviceType>(i);
    assert(i2cSlot(t, 1) == i);
    I2cDeviceType back = I2C_DEVICE_COUNT;
    uint8_t n = 0;
    assert(i2cSlotToRef(static_cast<uint8_t>(i), back, n));
    assert(back == t && n == 1);
    assert(i2cSlot(t, 0) == kI2cNoSlot);
    assert(i2cSlot(t, static_cast<uint8_t>(i2cMaxInstances(t) + 1)) == kI2cNoSlot);
    char id[16], name[24];
    assert(std::strcmp(i2cInstanceId(t, 1, id, sizeof(id)), kI2cTypes[i].id) == 0);
    assert(std::strcmp(i2cInstanceName(t, 1, name, sizeof(name)), kI2cTypes[i].displayName) == 0);
  }
  for (int invalid : {-1, int(I2C_DEVICE_COUNT), 99}) {
    const auto t = static_cast<I2cDeviceType>(invalid);
    assert(i2cSlot(t, 1) == kI2cNoSlot);
    assert(i2cMaxInstances(t) == 0);
    char id[16] = "x";
    assert(std::strcmp(i2cInstanceId(t, 1, id, sizeof(id)), "") == 0);
  }
  I2cDeviceType t;
  uint8_t n;
  assert(!i2cSlotToRef(kI2cSlotCount, t, n));
  assert(!i2cSlotToRef(kI2cNoSlot, t, n));
  // The command words the open/close commands and sensorautostart use.
  const char* ids[] = {"thermal", "tof", "imu", "input", "gps", "fmradio", "apds", "rtc",
                       "presence", "encoder"};
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) assert(std::strcmp(kI2cTypes[i].id, ids[i]) == 0);
}

// Every slot maps to exactly one (type, n) and back.
static void testSlotsRoundTrip() {
  int seen = 0;
  for (int slot = 0; slot < kI2cSlotCount; ++slot) {
    I2cDeviceType t;
    uint8_t n;
    assert(i2cSlotToRef(static_cast<uint8_t>(slot), t, n));
    assert(i2cSlot(t, n) == slot);
    ++seen;
  }
  int instances = 0;
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) instances += kI2cTypes[i].maxInstances;
  assert(seen == instances && instances == kI2cSlotCount);
  assert(kI2cExtraSlots == kI2cSlotCount - I2C_DEVICE_COUNT);
  assert(kI2cExtraSlotStorage >= 1 && kI2cExtraSlotStorage >= kI2cExtraSlots);
}

static void testFirmwareMaxima() {
#ifndef HW1_HOST_I2C_TEST_ENCODER_MAX
  // Phase 1c: the encoder has four instances (owner question Q1), every
  // other type one; instance 1's slots are still the type values.
  assert(kEncoderMaxInstances == 4 && i2cMaxInstances(I2C_DEVICE_ENCODER) == 4);
  assert(kI2cExtraSlots == 3 && kI2cSlotCount == I2C_DEVICE_COUNT + 3);
  assert(i2cSlot(I2C_DEVICE_ENCODER, 2) == I2C_DEVICE_COUNT);
  assert(i2cSlot(I2C_DEVICE_ENCODER, 5) == kI2cNoSlot);
  for (int t = 0; t < I2C_DEVICE_COUNT; ++t) {
    if (t != I2C_DEVICE_ENCODER) assert(i2cMaxInstances(static_cast<I2cDeviceType>(t)) == 1);
  }
  I2cInstanceArg arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "2", I2cInstanceForm::OpenClose);
  assert(arg.error == I2cInstanceError::None && arg.n == 2);
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "5", I2cInstanceForm::OpenClose);
  assert(errorText(I2C_DEVICE_ENCODER, arg, "openencoder [n]") ==
         "Error: encoder has instances 1-4. Usage: openencoder [n]");
  arg = i2cParseInstanceArg(I2C_DEVICE_IMU, "2", I2cInstanceForm::OpenClose);
  assert(errorText(I2C_DEVICE_IMU, arg, "openimu") == "Error: imu has one instance.");
#endif
}

#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
static void testEncoderFour() {
  static_assert(HW1_HOST_I2C_TEST_ENCODER_MAX == 4, "the cases below are written for four");
  assert(i2cMaxInstances(I2C_DEVICE_ENCODER) == 4);
  assert(kI2cExtraSlots == 3 && kI2cSlotCount == I2C_DEVICE_COUNT + 3);
  // Extra slots come after I2C_DEVICE_COUNT, in instance order.
  assert(i2cSlot(I2C_DEVICE_ENCODER, 2) == I2C_DEVICE_COUNT);
  assert(i2cSlot(I2C_DEVICE_ENCODER, 3) == I2C_DEVICE_COUNT + 1);
  assert(i2cSlot(I2C_DEVICE_ENCODER, 4) == I2C_DEVICE_COUNT + 2);
  assert(i2cSlot(I2C_DEVICE_ENCODER, 5) == kI2cNoSlot);
  assert(i2cSlot(I2C_DEVICE_IMU, 2) == kI2cNoSlot);
  I2cDeviceType t;
  uint8_t n;
  assert(i2cSlotToRef(I2C_DEVICE_COUNT + 1, t, n) && t == I2C_DEVICE_ENCODER && n == 3);

  char id[16], name[24];
  assert(std::strcmp(i2cInstanceId(I2C_DEVICE_ENCODER, 2, id, sizeof(id)), "encoder2") == 0);
  assert(std::strcmp(i2cInstanceName(I2C_DEVICE_ENCODER, 2, name, sizeof(name)), "Encoder 2") == 0);
  assert(std::strcmp(i2cInstanceName(I2C_DEVICE_ENCODER, 4, name, sizeof(name)), "Encoder 4") == 0);
  assert(std::strcmp(i2cInstanceId(I2C_DEVICE_ENCODER, 5, id, sizeof(id)), "") == 0);
  assert(std::strcmp(i2cInstanceName(I2C_DEVICE_IMU, 2, name, sizeof(name)), "") == 0);
  char tiny[4];
  assert(std::strcmp(i2cInstanceName(I2C_DEVICE_ENCODER, 2, tiny, sizeof(tiny)), "Enc") == 0);
}

// G1: open<x> [n], close<x> [n].
static void testOpenClose() {
  const auto form = I2cInstanceForm::OpenClose;
  I2cInstanceArg arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1);
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, nullptr, form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1);
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "  2 ", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 2);
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "1", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1);
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "4", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 4);
  for (const char* bad : {"5", "0", "300", "99999999999"}) {
    arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, bad, form);
    assert(arg.error == I2cInstanceError::OutOfRange);
    assert(errorText(I2C_DEVICE_ENCODER, arg, "openencoder [n]") ==
           "Error: encoder has instances 1-4. Usage: openencoder [n]");
  }
  // G6: decimal digits only.
  for (const char* bad : {"0x2", "+2", "-1", "two", "2a"}) {
    arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, bad, form);
    assert(arg.error == I2cInstanceError::NotAnInstance);
    assert(errorText(I2C_DEVICE_ENCODER, arg, "openencoder [n]") ==
           std::string("Error: '") + bad + "' is not an instance number. Usage: openencoder [n]");
  }
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "2 3", form);
  assert(arg.error == I2cInstanceError::TooMany && str(arg.token, arg.tokenLen) == "3");
  assert(errorText(I2C_DEVICE_ENCODER, arg, "openencoder [n]") ==
         "Error: '3' is one argument too many. Usage: openencoder [n]");
}

// G2: <x>status [n] [json], json anywhere.
static void testStatus() {
  const auto form = I2cInstanceForm::Status;
  I2cInstanceArg arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg).empty());
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "json", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg) == "json");
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "2 json", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 2 && rest(arg) == "json");
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "JSON 3", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 3 && rest(arg).empty());
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "9 json", form);
  assert(arg.error == I2cInstanceError::OutOfRange && arg.n == 9);
  // A word that is not a number is left to the command, as before.
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "brief json", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg) == "brief json");
}

// G3: setters. One token stays the value for instance 1 (parseBoolArg reads
// "1" as on), two tokens are instance then value.
static void testSetter() {
  const auto form = I2cInstanceForm::Setter;
  I2cInstanceArg arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg).empty());
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "1", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg) == "1");
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "0x37", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg) == "0x37");
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, " 2   0x37 ", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 2 && rest(arg) == "0x37");
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "x 0x37", form);
  assert(arg.error == I2cInstanceError::NotAnInstance);
  assert(errorText(I2C_DEVICE_ENCODER, arg, "encoderaddress [n] <0x36..0x3D|0x49..0x50>") ==
         "Error: 'x' is not an instance number. Usage: encoderaddress [n] <0x36..0x3D|0x49..0x50>");
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "7 on", form);
  assert(arg.error == I2cInstanceError::OutOfRange);
  // One token that is no valid value but names an instance gets the hint.
  assert(i2cInstanceTokenIsInstance(I2C_DEVICE_ENCODER, "2", 1));
  assert(!i2cInstanceTokenIsInstance(I2C_DEVICE_ENCODER, "1", 1));  // a value for instance 1
  assert(!i2cInstanceTokenIsInstance(I2C_DEVICE_ENCODER, "5", 1));
  assert(!i2cInstanceTokenIsInstance(I2C_DEVICE_IMU, "2", 1));
  char buf[160];
  assert(std::string(i2cInstanceNotValueText("2", 1, "on or off", "encoderautostart", "on", buf,
                                             sizeof(buf))) ==
         "Error: '2' is not on or off. To set instance 2: encoderautostart 2 on");
  assert(std::string(i2cInstanceNoAddressText(I2C_DEVICE_ENCODER, 2, "encoderaddress", buf,
                                              sizeof(buf))) ==
         "Error: encoder 2 has no address. Set one: encoderaddress 2 <addr>");
}

// G4/G7: sensorautostart takes n = 1 for every type; a single-instance type
// refuses any other number.
static void testSingleInstanceTypes() {
  const auto form = I2cInstanceForm::Setter;
  I2cInstanceArg arg = i2cParseInstanceArg(I2C_DEVICE_IMU, "1 on", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg) == "on");
  arg = i2cParseInstanceArg(I2C_DEVICE_IMU, "on", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 1 && rest(arg) == "on");
  for (const char* bad : {"2 on", "0 off"}) {
    arg = i2cParseInstanceArg(I2C_DEVICE_IMU, bad, form);
    assert(arg.error == I2cInstanceError::SingleInstance);
    assert(errorText(I2C_DEVICE_IMU, arg, "sensorautostart <sensor> [n] <on|off>") ==
           "Error: imu has one instance.");
  }
  arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "3 off", form);
  assert(arg.error == I2cInstanceError::None && arg.n == 3 && rest(arg) == "off");
  assert(errorText(I2C_DEVICE_ENCODER, arg, "x").empty());
}
#endif

// ---------------------------------------------------------------------------
// Rows of the device table (phase 1b): identify and claims per instance.
// A row with the fields the core reads, the way i2cSensors[] rows have them.
struct TestRow {
  uint8_t address;
  bool multiAddress;
  uint8_t altAddress;
  bool compiled;
  int (*busAt)(uint8_t n);
  uint8_t (*addressAt)(uint8_t n);
  uint8_t instances;
};

// The encoder instances' settings for the rows below (index n - 1); 0 = not set.
static uint8_t gEncAddr[4] = {0x36, 0x37, 0, 0};
static int gEncBus[4] = {0, 0, 0, 0};
static uint8_t encAddressAt(uint8_t n) { return n >= 1 && n <= 4 ? gEncAddr[n - 1] : 0; }
static int encBusAt(uint8_t n) { return n >= 1 && n <= 4 ? gEncBus[n - 1] : 0; }
static int imuBusAt(uint8_t) { return 0; }

static void testRowInstances() {
  const uint8_t encoderInstances = i2cMaxInstances(I2C_DEVICE_ENCODER);
  const TestRow rows[] = {
    {0x28, true, 0x29, true, imuBusAt, nullptr, 1},                       // IMU, alternate 0x29
    {0x3D, true, 0x3C, true, nullptr, nullptr, 1},                        // OLED, no bus setting
    {0x49, false, 0, true, encBusAt, encAddressAt, encoderInstances},     // encoder 5752 row
    {0x36, false, 0, true, encBusAt, encAddressAt, encoderInstances},     // encoder 4991 row
  };
  const size_t count = sizeof(rows) / sizeof(rows[0]);
  assert(kI2cRowInstanceCap == encoderInstances);

  // Instance 1 as before: fixed address, alternate, a row without a bus.
  assert(i2cRowAddressAt(rows[0], 1) == 0x28 && i2cRowAddressAt(rows[0], 2) == 0);
  assert(i2cRowInstanceAt(rows[0], 0x28, 0) == 1 && i2cRowInstanceAt(rows[0], 0x29, 0) == 1);
  assert(i2cRowInstanceAt(rows[0], 0x28, 1) == 0);  // not configured on bus 1: a label only
  assert(i2cRowInstanceAt(rows[1], 0x3C, 1) == 1);  // no bus setting: either bus
  assert(i2cRowIndexByAddress(rows, count, 0x29) == 0);
  assert(i2cRowIndexByAddress(rows, count, 0x00) == -1);

  // The encoder's instance 1 at 0x36 on bus 0.
  assert(i2cRowIndexByAddress(rows, count, 0x36) == 2);  // instance 1's address, table order
  assert(i2cRowInstanceAt(rows[2], 0x36, 0) == 1 && i2cRowInstanceAt(rows[3], 0x36, 0) == 1);
  assert(i2cRowClaims(rows[3], 0x36, 0) && !i2cRowClaims(rows[3], 0x36, 1));

  // An unconfigured address is no instance and no row.
  assert(i2cRowIndexByAddress(rows, count, 0x38) == -1);
  assert(i2cRowInstanceAt(rows[2], 0x38, 0) == 0 && !i2cRowClaims(rows[2], 0x38, 0));

  if (encoderInstances >= 2) {
    // Two configured encoder addresses: (row, 1) and (row, 2).
    assert(i2cRowAddressAt(rows[2], 2) == 0x37 && i2cRowAddressAt(rows[2], 3) == 0);
    assert(i2cRowIndexByAddress(rows, count, 0x37) == 2);
    assert(i2cRowInstanceAt(rows[3], 0x37, 0) == 2);
    assert(i2cRowClaims(rows[2], 0x37, 0) && i2cRowClaims(rows[3], 0x37, 0));
    // Instance 2 moved to bus 1: found on bus 0 it is a label, not instance 2.
    gEncBus[1] = 1;
    assert(i2cRowInstanceAt(rows[3], 0x37, 0) == 0 && i2cRowInstanceAt(rows[3], 0x37, 1) == 2);
    // Instance 3 at instance 2's address on the other bus: each bus its own.
    gEncAddr[2] = 0x37;
    gEncBus[2] = 0;
    assert(i2cRowInstanceAt(rows[3], 0x37, 0) == 3 && i2cRowInstanceAt(rows[3], 0x37, 1) == 2);
    gEncAddr[2] = 0;
    gEncBus[1] = 0;
  } else {
    // One instance: a second address never names the encoder.
    assert(i2cRowIndexByAddress(rows, count, 0x37) == -1);
    assert(i2cRowInstanceAt(rows[3], 0x37, 0) == 0);
  }
  // A row not in this build claims nothing.
  TestRow absent = rows[2];
  absent.compiled = false;
  assert(!i2cRowClaims(absent, 0x36, 0));
  // An instance count past the cap reads as the cap.
  TestRow wide = rows[2];
  wide.instances = 200;
  assert(i2cRowInstances(wide) == kI2cRowInstanceCap);
}

// Address collisions: a pure search over a fixed list of uses (a FeatherS3
// build: fuel gauge on bus 0, OLED, APDS9960, gamepad, two encoders).
static std::string clashText(const I2cAddressUse* use) {
  char buf[128];
  return use ? i2cAddressClashText(*use, buf, sizeof(buf)) : std::string();
}

static void testAddressCollisions() {
  static constexpr I2cAddressUse uses[] = {
    {0, 0x36, I2C_DEVICE_COUNT, 0, "MAX17048", "LiPo Fuel Gauge"},
    {0, 0x3D, I2C_DEVICE_COUNT, 0, "SSD1306", "OLED 128x64 Display"},
    {0, 0x3C, I2C_DEVICE_COUNT, 0, "SSD1306", "OLED 128x64 Display"},
    {0, 0x39, I2C_DEVICE_APDS, 1, "APDS9960", "RGB, Gesture & Proximity"},
    {0, 0x50, I2C_DEVICE_INPUT, 1, "Seesaw", "Mini I2C Gamepad"},
    {1, 0x49, I2C_DEVICE_ENCODER, 1, "Encoder", "Adafruit 5752 Quad Rotary Encoder, 4 RGB pixels"},
    {0, 0x37, I2C_DEVICE_ENCODER, 2, "Encoder", "Adafruit 4991 Rotary Encoder, 1 RGB pixel"},
    {0, 0x70, I2C_DEVICE_COUNT, 0, "HT16K33", nullptr},
  };
  const size_t count = sizeof(uses) / sizeof(uses[0]);
  const auto taken = [&](uint8_t bus, uint8_t address, I2cDeviceType t, uint8_t n) {
    return i2cAddressTakenBy(uses, count, bus, address, t, n);
  };

  // Fixed-address devices of the build, on their bus only.
  assert(clashText(taken(0, 0x36, I2C_DEVICE_ENCODER, 1)) ==
         "Error: 0x36 on bus 0 is in use by MAX17048 (LiPo Fuel Gauge).");
  assert(taken(1, 0x36, I2C_DEVICE_ENCODER, 1) == nullptr);
  assert(taken(0, 0x3C, I2C_DEVICE_ENCODER, 1) && taken(0, 0x3D, I2C_DEVICE_ENCODER, 1));
  assert(taken(0, 0x39, I2C_DEVICE_ENCODER, 1)->type == I2C_DEVICE_APDS);
  assert(clashText(taken(0, 0x50, I2C_DEVICE_ENCODER, 1)) ==
         "Error: 0x50 on bus 0 is in use by Seesaw (Mini I2C Gamepad).");
  // The gamepad's alternate address is not one its driver uses.
  assert(taken(0, 0x49, I2C_DEVICE_ENCODER, 1) == nullptr);
  assert(clashText(taken(0, 0x70, I2C_DEVICE_ENCODER, 1)) ==
         "Error: 0x70 on bus 0 is in use by HT16K33.");
  // A free address, and address 0 (never in use).
  assert(taken(0, 0x38, I2C_DEVICE_ENCODER, 1) == nullptr);
  assert(taken(0, 0x00, I2C_DEVICE_ENCODER, 1) == nullptr);

  // Another instance of the same type is refused; the asking instance is not.
  assert(taken(0, 0x37, I2C_DEVICE_ENCODER, 2) == nullptr);
  assert(taken(1, 0x49, I2C_DEVICE_ENCODER, 1) == nullptr);
  const I2cAddressUse* other = taken(0, 0x37, I2C_DEVICE_ENCODER, 1);
  assert(other && other->type == I2C_DEVICE_ENCODER && other->n == 2);
  if (i2cMaxInstances(I2C_DEVICE_ENCODER) > 1) {
    assert(clashText(other) == "Error: 0x37 on bus 0 is in use by Encoder (instance 2).");
    assert(clashText(taken(1, 0x49, I2C_DEVICE_ENCODER, 3)) ==
           "Error: 0x49 on bus 1 is in use by Encoder (instance 1).");
  } else {
    // A type with one instance is named by its device.
    assert(clashText(other) ==
           "Error: 0x37 on bus 0 is in use by Encoder (Adafruit 4991 Rotary Encoder, 1 RGB pixel).");
  }
  // Another type asking: the encoder's addresses are taken.
  assert(taken(1, 0x49, I2C_DEVICE_COUNT, 0)->type == I2C_DEVICE_ENCODER);
  assert(i2cAddressTakenBy(nullptr, 3, 0, 0x36, I2C_DEVICE_ENCODER, 1) == nullptr);
  char tiny[8];
  assert(std::strlen(i2cAddressClashText(uses[0], tiny, sizeof(tiny))) == sizeof(tiny) - 1);
}

int main() {
  testInstanceOneIsTheType();
  testSlotsRoundTrip();
  testFirmwareMaxima();
  testRowInstances();
  testAddressCollisions();
#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
  testEncoderFour();
  testOpenClose();
  testStatus();
  testSetter();
  testSingleInstanceTypes();
  std::puts("I2C instance core tests passed (encoder with four instances: slots, names, G1-G7, errors, "
            "identify, collisions)");
#else
  std::puts("I2C instance core tests passed (firmware maxima: instance 1 is the type, identify, "
            "collisions)");
#endif
  return 0;
}
