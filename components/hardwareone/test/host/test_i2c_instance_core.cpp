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

// Scan labels by address and bus (i2cscan on the S3D: a 4991 encoder at 0x36
// on bus 1 while the fuel gauge is wired to bus 0).
static int gaugeBusAt(uint8_t) { return 0; }

static void testScanLabelRows() {
  const uint8_t encoderInstances = i2cMaxInstances(I2C_DEVICE_ENCODER);
  gEncAddr[0] = 0x49;  // encoder board 1 at the 5752's default, bus 0
  gEncAddr[1] = 0;
  gEncBus[0] = 0;
  const TestRow rows[] = {
    {0x36, false, 0, true, gaugeBusAt, nullptr, 1},                        // fuel gauge, bus 0
    {0x3D, true, 0x3C, true, nullptr, nullptr, 1},                         // OLED, no bus setting
    {0x33, false, 0, false, imuBusAt, nullptr, 1},                         // a row not in this build
    {0x49, false, 0, true, encBusAt, encAddressAt, encoderInstances},      // encoder 5752 row
    {0x36, false, 0, true, encBusAt, nullptr, 1},                          // encoder 4991 row (default)
  };
  const size_t count = sizeof(rows) / sizeof(rows[0]);
  const auto all = [](size_t) { return true; };
  int out[4] = {};

  // The bus the fuel gauge is wired to names it alone.
  assert(i2cAddressLabelRows(rows, count, 0x36, 0, all, out, 4) == 1 && out[0] == 0);
  // Another bus: every row with 0x36, the gauge and the 4991, in table order.
  assert(i2cAddressLabelRows(rows, count, 0x36, 1, all, out, 4) == 2 && out[0] == 0 && out[1] == 4);
  // Leaving the gauge out (a board that reported a product no row names is
  // labelled among the plain rows; here the other way round) keeps the rest.
  assert(i2cAddressLabelRows(rows, count, 0x36, 1, [](size_t i) { return i != 0; }, out, 4) == 1 &&
         out[0] == 4);
  // The configured encoder on its bus; on the other bus, by address alone.
  assert(i2cAddressLabelRows(rows, count, 0x49, 0, all, out, 4) == 1 && out[0] == 3);
  assert(i2cAddressLabelRows(rows, count, 0x49, 1, all, out, 4) == 1 && out[0] == 3);
  // A row without a bus setting claims either bus, at its alternate too.
  assert(i2cAddressLabelRows(rows, count, 0x3C, 1, all, out, 4) == 1 && out[0] == 1);
  // A row not in this build still labels its address; an unknown address is none.
  assert(i2cAddressLabelRows(rows, count, 0x33, 0, all, out, 4) == 1 && out[0] == 2);
  assert(i2cAddressLabelRows(rows, count, 0x38, 0, all, out, 4) == 0);
  assert(i2cAddressLabelRows(rows, count, 0x00, 0, all, out, 4) == 0);
  // The cap bounds the list.
  assert(i2cAddressLabelRows(rows, count, 0x36, 1, all, out, 1) == 1 && out[0] == 0);
  assert(i2cRowHasAddress(rows[1], 0x3C) && !i2cRowHasAddress(rows[0], 0x37));
  // A row's own address before an earlier row's alternate: the ToF at 0x29
  // is not a two-address chip at 0x28 (alternate 0x29), both on bus 0; with
  // the ToF not in this build the alternate claims it. (The BNO055 row no
  // longer has an alternate: its address is the imuAddress setting.)
  const TestRow imuTof[] = {
    {0x28, true, 0x29, true, imuBusAt, nullptr, 1},   // a chip with alternate 0x29
    {0x39, false, 0, true, imuBusAt, nullptr, 1},     // APDS9960
    {0x29, false, 0, true, imuBusAt, nullptr, 1},     // VL53L4CX
  };
  assert(i2cAddressLabelRows(imuTof, 3, 0x29, 0, all, out, 4) == 1 && out[0] == 2);
  assert(i2cAddressLabelRows(imuTof, 3, 0x28, 0, all, out, 4) == 1 && out[0] == 0);
  TestRow imuOnly[3] = {imuTof[0], imuTof[1], imuTof[2]};
  imuOnly[2].compiled = false;
  assert(i2cAddressLabelRows(imuOnly, 3, 0x29, 0, all, out, 4) == 1 && out[0] == 0);
  gEncAddr[0] = 0x36;
  gEncAddr[1] = 0x37;
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


// Owners: an instance of a queue type, or one row of a device without one.
// Rows without a queue type share (I2C_DEVICE_COUNT, 0); only the row tells
// them apart, so the OLED asking is not exempt from the fuel gauge.
static void testOwners() {
  static const I2cAddressUse uses[] = {
    {0, 0x36, I2C_DEVICE_COUNT, 0, "MAX17048", "LiPo Fuel Gauge", 10},
    {1, 0x3D, I2C_DEVICE_COUNT, 0, "SSD1306", "OLED 128x64 Display", 11},
    {1, 0x3C, I2C_DEVICE_COUNT, 0, "SSD1306", "OLED 128x64 Display", 11},
    {1, 0x36, I2C_DEVICE_ENCODER, 1, "Encoder", "Adafruit 4991 Rotary Encoder, 1 RGB pixel", 14},
    {1, 0x49, I2C_DEVICE_INPUT, 1, "Seesaw", "ANO Rotary Encoder (default)", 3},
  };
  const size_t count = sizeof(uses) / sizeof(uses[0]);
  assert(i2cAddressUseOwnedBy(uses[1], i2cRowOwner(11)) && i2cAddressUseOwnedBy(uses[2], i2cRowOwner(11)));
  assert(!i2cAddressUseOwnedBy(uses[0], i2cRowOwner(11)));
  assert(!i2cAddressUseOwnedBy(uses[0], i2cRowOwner(-1)));
  assert(i2cAddressUseOwnedBy(uses[3], i2cInstanceOwner(I2C_DEVICE_ENCODER, 1)));
  assert(!i2cAddressUseOwnedBy(uses[3], i2cInstanceOwner(I2C_DEVICE_ENCODER, 2)));
  // The OLED's own addresses are free for it; the gauge's are not.
  assert(i2cAddressTakenBy(uses, count, 1, 0x3D, i2cRowOwner(11)) == nullptr);
  assert(i2cAddressTakenBy(uses, count, 0, 0x36, i2cRowOwner(11)) == &uses[0]);
  // A queue type asking with (I2C_DEVICE_COUNT, n) is no row: every device
  // without a queue type clashes.
  assert(i2cAddressTakenBy(uses, count, 0, 0x36, I2C_DEVICE_COUNT, 0) == &uses[0]);
  assert(i2cAddressTakenBy(uses, count, 1, 0x3D, I2C_DEVICE_COUNT, 0) == &uses[1]);
}

// A bus setter: every address the owner holds is checked on the new bus.
static void testBusMove() {
  // The status sign on a FeatherS3[D]: bus 0 has the fuel gauge (0x36) and an
  // RTC; bus 1 the OLED, encoder 1 (0x36), the ANO (0x49) and a two-chip IMU
  // (accelerometer 0x6A, magnetometer 0x1C).
  static const I2cAddressUse uses[] = {
    {0, 0x36, I2C_DEVICE_COUNT, 0, "MAX17048", "LiPo Fuel Gauge", 10},
    {1, 0x3D, I2C_DEVICE_COUNT, 0, "SSD1306", "OLED 128x64 Display", 11},
    {1, 0x3C, I2C_DEVICE_COUNT, 0, "SSD1306", "OLED 128x64 Display", 11},
    {0, 0x68, I2C_DEVICE_RTC, 1, "DS3231", "Precision RTC", 9},
    {1, 0x36, I2C_DEVICE_ENCODER, 1, "Encoder", "Adafruit 4991 Rotary Encoder, 1 RGB pixel", 14},
    {1, 0x49, I2C_DEVICE_INPUT, 1, "Seesaw", "ANO Rotary Encoder (default)", 3},
    {1, 0x6A, I2C_DEVICE_IMU, 1, "LSM6DSOX", "Accelerometer + gyroscope", 0},
    {1, 0x1C, I2C_DEVICE_IMU, 1, "LIS3MDL", "Magnetometer", 1},
  };
  const size_t count = sizeof(uses) / sizeof(uses[0]);
  // The fuel gauge to bus 1: 0x36 is encoder 1's there.
  const I2cAddressUse* clash = i2cOwnerBusClash(uses, count, i2cRowOwner(10), 1);
  assert(clash == &uses[4]);
  assert(clashText(clash) == (i2cMaxInstances(I2C_DEVICE_ENCODER) > 1
                                  ? "Error: 0x36 on bus 1 is in use by Encoder (instance 1)."
                                  : "Error: 0x36 on bus 1 is in use by Encoder (Adafruit 4991 Rotary Encoder, 1 RGB pixel)."));
  // Encoder 1 to bus 0: the gauge's 0x36.
  assert(i2cOwnerBusClash(uses, count, i2cInstanceOwner(I2C_DEVICE_ENCODER, 1), 0) == &uses[0]);
  // The ANO to bus 0: 0x49 is free there; on its own bus nothing clashes.
  assert(i2cOwnerBusClash(uses, count, i2cInstanceOwner(I2C_DEVICE_INPUT, 1), 0) == nullptr);
  assert(i2cOwnerBusClash(uses, count, i2cInstanceOwner(I2C_DEVICE_INPUT, 1), 1) == nullptr);
  // The OLED (both addresses) to bus 0: free; the OLED row is not exempt from
  // the gauge, but holds neither 0x36 nor 0x68.
  assert(i2cOwnerBusClash(uses, count, i2cRowOwner(11), 0) == nullptr);
  // The IMU to bus 0 is free; with a magnetometer-address device on bus 0 the
  // second chip is refused although the first chip's address is free.
  assert(i2cOwnerBusClash(uses, count, i2cInstanceOwner(I2C_DEVICE_IMU, 1), 0) == nullptr);
  I2cAddressUse more[count + 1];
  for (size_t i = 0; i < count; ++i) more[i] = uses[i];
  more[count] = I2cAddressUse{0, 0x1C, I2C_DEVICE_COUNT, 0, "Other", nullptr, 12};
  assert(i2cOwnerBusClash(more, count + 1, i2cInstanceOwner(I2C_DEVICE_IMU, 1), 0) == &more[count]);
  // The visitor form agrees with the array form.
  const auto forEach = [&](auto&& visit) {
    for (size_t i = 0; i < count + 1; ++i) {
      if (visit(more[i])) return;
    }
  };
  I2cAddressUse found{};
  assert(i2cOwnerBusClash(forEach, i2cInstanceOwner(I2C_DEVICE_IMU, 1), 0, found));
  assert(found.address == 0x1C && found.row == 12);
  assert(!i2cOwnerBusClash(forEach, i2cInstanceOwner(I2C_DEVICE_INPUT, 1), 0, found));
  assert(i2cOwnerBusClash(forEach, i2cRowOwner(10), 1, found) && found.type == I2C_DEVICE_ENCODER);
  // An owner with no uses (a row not in the build) never clashes.
  assert(i2cOwnerBusClash(uses, count, i2cRowOwner(-1), 1) == nullptr);
}

// Instance 1's extra rows: a row of the type with its own address setting
// claims its configured address; a fixed or alternate-only row claims nothing.
struct TypedRow {
  uint8_t address;
  bool compiled;
  int deviceType;
  uint8_t (*addressAt)(uint8_t n);
};
static uint8_t gMagAddress = 0x1C;
static uint8_t accelAt(uint8_t) { return 0x6A; }
static uint8_t magAt(uint8_t) { return gMagAddress; }
static void testExtraRowAddresses() {
  const TypedRow rows[] = {
    {0x6A, true, I2C_DEVICE_IMU, accelAt},     // 0: the IMU's first chip (the driver's address)
    {0x1C, true, I2C_DEVICE_IMU, magAt},       // 1: the second chip
    {0x49, true, I2C_DEVICE_INPUT, nullptr},   // 2: the ANO default row
    {0x50, true, I2C_DEVICE_INPUT, nullptr},   // 3: the ANO alternate row: no setting
    {0x1E, false, I2C_DEVICE_IMU, magAt},      // 4: not in this build
    {0x1C, true, I2C_DEVICE_IMU, magAt},       // 5: the same address again: once
  };
  const size_t count = sizeof(rows) / sizeof(rows[0]);
  size_t seen[8];
  uint8_t addresses[8];
  size_t found = 0;
  auto collect = [&](size_t row, uint8_t address) {
    seen[found] = row;
    addresses[found++] = address;
    return false;
  };
  i2cForEachExtraRowAddress(rows, count, I2C_DEVICE_IMU, 0x6A, collect);
  assert(found == 1 && seen[0] == 1 && addresses[0] == 0x1C);
  found = 0;
  i2cForEachExtraRowAddress(rows, count, I2C_DEVICE_INPUT, 0x49, collect);
  assert(found == 0);
  // A magnetometer set to the accelerometer's address claims nothing more.
  gMagAddress = 0x6A;
  found = 0;
  i2cForEachExtraRowAddress(rows, count, I2C_DEVICE_IMU, 0x6A, collect);
  assert(found == 0);
  gMagAddress = 0x1C;
  // The visitor can stop the walk.
  found = 0;
  i2cForEachExtraRowAddress(rows, count, I2C_DEVICE_IMU, 0x00, [&](size_t row, uint8_t address) {
    collect(row, address);
    return true;
  });
  assert(found == 1 && seen[0] == 0);
  i2cForEachExtraRowAddress(static_cast<const TypedRow*>(nullptr), 0, I2C_DEVICE_IMU, 0x6A, collect);
}

int main() {
  testInstanceOneIsTheType();
  testSlotsRoundTrip();
  testFirmwareMaxima();
  testRowInstances();
  testScanLabelRows();
  testAddressCollisions();
  testOwners();
  testBusMove();
  testExtraRowAddresses();
#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
  testEncoderFour();
  testOpenClose();
  testStatus();
  testSetter();
  testSingleInstanceTypes();
  std::puts("I2C instance core tests passed (encoder with four instances: slots, names, G1-G7, errors, "
            "identify, scan labels, collisions)");
#else
  std::puts("I2C instance core tests passed (firmware maxima: instance 1 is the type, identify, "
            "scan labels, collisions)");
#endif
  return 0;
}
