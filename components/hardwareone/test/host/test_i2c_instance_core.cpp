// Several instances of one I2C device type: the slot math, ids, names and the
// instance argument rules (System_I2CInstanceCore.h, design sections 1.1, 3).
//
// Built twice: with the firmware's maxima (every type has one instance, so
// every slot is the type value) and with HW1_HOST_I2C_TEST_ENCODER_MAX=4, which
// gives the encoder four instances as the multi-instance phases will.

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
  // Phase 1a: no type has a second instance yet, so nothing grows.
  assert(kEncoderMaxInstances == 1);
  assert(kI2cExtraSlots == 0 && kI2cSlotCount == I2C_DEVICE_COUNT);
  assert(i2cSlot(I2C_DEVICE_ENCODER, 2) == kI2cNoSlot);
  const I2cInstanceArg arg = i2cParseInstanceArg(I2C_DEVICE_ENCODER, "2", I2cInstanceForm::OpenClose);
  assert(arg.error == I2cInstanceError::SingleInstance);
  assert(errorText(I2C_DEVICE_ENCODER, arg, "openencoder") == "Error: encoder has one instance.");
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

int main() {
  testInstanceOneIsTheType();
  testSlotsRoundTrip();
  testFirmwareMaxima();
#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
  testEncoderFour();
  testOpenClose();
  testStatus();
  testSetter();
  testSingleInstanceTypes();
  std::puts("I2C instance core tests passed (encoder with four instances: slots, names, G1-G7, errors)");
#else
  std::puts("I2C instance core tests passed (firmware maxima: instance 1 is the type)");
#endif
  return 0;
}
