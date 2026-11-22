// The firmware's walk over every I2C address in use (System_I2C.cpp
// forEachI2cAddressUse), the connected check built on it (isSensorConnected)
// and the row-owner lookup (i2cRowOwnerByName), run on a fixture table.
// test_i2c_address_walk.py inserts the production functions below; the
// table and the settings are the fixture's.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>

#define ENABLE_GT911_TOUCH 0
#define ENABLE_ANO_ENCODER 1
#include "System_I2CInstanceCore.h"
#include "i2csensor_ano_encoder_core.h"

#define WARN_I2CF(...) do {} while (0)
struct I2cDeviceManager { static constexpr int NUM_BUSES = 2; };

// INSERT_TYPES

static struct {
  int thermalBus = 0, tofBus = 0, imuBus = 1, inputBus = 1, gpsBus = 0;
  int fmRadioBus = 0, apdsBus = 0, rtcBus = 0, presenceBus = 0;
  int fuelGaugeBus = 0, oledBus = 1;
  int anoEncoderI2cAddr = 0x4A;  // a non-default ANO address (jumper A0)
  int magAddress = 0x1C;         // the second chip's address setting
} gSettings;
static uint8_t encoderInstanceAddress(uint8_t) { return 0; }
static int encoderInstanceBus(uint8_t) { return 0; }

static int fuelBus(uint8_t) { return gSettings.fuelGaugeBus; }
static int oledBus(uint8_t) { return gSettings.oledBus; }
static int imuBus(uint8_t) { return gSettings.imuBus; }
static int inputBus(uint8_t) { return gSettings.inputBus; }
static int presenceBus(uint8_t) { return gSettings.presenceBus; }
static int tofBus(uint8_t) { return gSettings.tofBus; }
static uint8_t magAddress(uint8_t n) { return n == 1 ? static_cast<uint8_t>(gSettings.magAddress) : 0; }

// address, name, description, manufacturer, multiAddress, altAddress, heap,
// library, guard, moduleName, clock, timeout, compiled, busAt, addressAt, type
static const I2cSensorEntry i2cSensors[] = {
  {0x36, "MAX17048", "LiPo Fuel Gauge", "", false, 0, 0, "", "", "fuelgauge", 100000, 50,
   true, fuelBus, nullptr, I2C_DEVICE_COUNT},
  {0x3D, "SSD1306", "OLED", "", true, 0x3C, 0, "", "", "oled", 400000, 50,
   true, oledBus, nullptr, I2C_DEVICE_COUNT},
  {0x28, "LSM6DSOX", "Accel/Gyro", "", false, 0, 0, "", "", "imu", 400000, 50,
   true, imuBus, nullptr, I2C_DEVICE_IMU},
  {0x1C, "LIS3MDL", "Magnetometer", "", false, 0, 0, "", "", "imu", 400000, 50,
   true, imuBus, magAddress, I2C_DEVICE_IMU},
  {0x49, "ANO", "Rotary Encoder", "", true, 0x50, 0, "", "", "input", 400000, 50,
   true, inputBus, nullptr, I2C_DEVICE_INPUT},
  {0x5A, "MLX90632", "Presence", "", false, 0, 0, "", "", "presence", 100000, 50,
   true, presenceBus, nullptr, I2C_DEVICE_PRESENCE},
  {0x29, "VL53L4CX", "ToF", "", false, 0, 0, "", "", "tof", 400000, 50,
   false, tofBus, nullptr, I2C_DEVICE_TOF},
};
static const size_t i2cSensorsCount = sizeof(i2cSensors) / sizeof(i2cSensors[0]);

static ConnectedDevice connectedDevices[8];
static int connectedDeviceCount = 0;
static void scanned(std::initializer_list<std::pair<uint8_t, uint8_t>> found) {
  connectedDeviceCount = 0;
  for (const auto& f : found) {
    ConnectedDevice d{};
    d.bus = f.first;
    d.address = f.second;
    d.isConnected = true;
    connectedDevices[connectedDeviceCount++] = d;
  }
}

// INSERT_PRODUCTION

using Use = std::tuple<int, int, int, int, int>;  // bus, address, type, n, row

static std::set<Use> walk() {
  std::set<Use> uses;
  forEachI2cAddressUse([&](const I2cAddressUse& use) {
    assert(uses.insert(Use{use.bus, use.address, use.type, use.n, use.row}).second);
    return false;
  });
  return uses;
}

int main() {
  // Every use, each with its row: the OLED at both addresses, the IMU at
  // both chips (the second from its row's address setting), the ANO at its
  // setting (named by the type's first row), presence at its row address.
  // The ToF is not in this build.
  const std::set<Use> expected = {
    Use{0, 0x36, I2C_DEVICE_COUNT, 0, 0},
    Use{1, 0x3D, I2C_DEVICE_COUNT, 0, 1}, Use{1, 0x3C, I2C_DEVICE_COUNT, 0, 1},
    Use{1, 0x28, I2C_DEVICE_IMU, 1, 2}, Use{1, 0x1C, I2C_DEVICE_IMU, 1, 3},
    Use{1, 0x4A, I2C_DEVICE_INPUT, 1, 4},
    Use{0, 0x5A, I2C_DEVICE_PRESENCE, 1, 5},
  };
  assert(walk() == expected);
  // The walk stops when the visitor says so.
  unsigned visited = 0;
  forEachI2cAddressUse([&](const I2cAddressUse&) { return ++visited == 3; });
  assert(visited == 3);

  // isSensorConnected: by (bus, address) of the build's uses.
  scanned({});
  assert(!isSensorConnected("imu") && !isSensorConnected("") && !isSensorConnected(nullptr));
  scanned({{0, 0x28}});  // the right module on the wrong bus
  assert(!isSensorConnected("imu"));
  scanned({{1, 0x28}});
  assert(isSensorConnected("imu"));
  scanned({{1, 0x1C}});  // the second chip alone answers for the module
  assert(isSensorConnected("imu"));
  scanned({{1, 0x49}});  // the ANO's default address, but this build uses 0x4A
  assert(!isSensorConnected("input"));
  scanned({{1, 0x4A}});
  assert(isSensorConnected("input"));
  scanned({{0, 0x61}});  // presence is its row address, not 0x61
  assert(!isSensorConnected("presence"));
  scanned({{0, 0x5A}});
  assert(isSensorConnected("presence") && !isSensorConnected("imu"));
  scanned({{1, 0x3C}});  // the OLED at its alternate address
  assert(isSensorConnected("oled"));
  scanned({{0, 0x29}});  // a module not in this build
  assert(!isSensorConnected("tof"));
  scanned({{1, 0x28}});
  connectedDevices[0].isConnected = false;  // gone since the scan
  assert(!isSensorConnected("imu"));

  // i2cRowOwnerByName: rows of devices without a queue type only.
  const I2cAddressOwner oled = i2cRowOwnerByName("SSD1306");
  assert(oled.type == I2C_DEVICE_COUNT && oled.row == 1);
  assert(i2cRowOwnerByName("MAX17048").row == 0);
  assert(i2cRowOwnerByName("LSM6DSOX").row == -1 && i2cRowOwnerByName("nope").row == -1);
  assert(i2cRowOwnerByName(nullptr).row == -1);

  // A bus move checks every address the owner holds, through the walk: the
  // IMU's second chip on 0x36 clashes with the fuel gauge on bus 0.
  I2cAddressUse clash{};
  auto forEach = [](auto&& visit) { forEachI2cAddressUse(visit); };
  assert(!i2cOwnerBusClash(forEach, i2cInstanceOwner(I2C_DEVICE_IMU, 1), 0, clash));
  gSettings.magAddress = 0x36;
  assert(i2cOwnerBusClash(forEach, i2cInstanceOwner(I2C_DEVICE_IMU, 1), 0, clash));
  assert(clash.row == 0 && clash.address == 0x36 && clash.bus == 0);
  gSettings.magAddress = 0x1C;
  // The OLED moving to bus 0 meets nothing; the fuel gauge moving to bus 1
  // meets nothing either (no 0x36 there), and the OLED's own row is exempt.
  assert(!i2cOwnerBusClash(forEach, oled, 0, clash));
  assert(!i2cOwnerBusClash(forEach, oled, 1, clash));
  assert(!i2cOwnerBusClash(forEach, i2cRowOwnerByName("MAX17048"), 1, clash));

  std::puts("I2C address walk passed (uses with rows, extra-row chips, connected by bus and address, "
            "row owners, bus-move clashes)");
  return 0;
}
