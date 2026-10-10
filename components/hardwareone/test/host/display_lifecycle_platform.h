#pragma once

// SDK/driver boundaries for the real HAL_Display.cpp host build. Hardware
// methods assert that the production HAL admitted them through its bus owner.
#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

constexpr int OUTPUT = 1, LOW = 0, HIGH = 1;
constexpr uint8_t SSD1306_BLACK = 0, SSD1306_WHITE = 1, SSD1306_INVERSE = 2;
constexpr uint8_t SSD1306_SWITCHCAPVCC = 2, SSD1306_DISPLAYOFF = 0xae;
constexpr uint8_t SSD1306_DISPLAYON = 0xaf, SSD1306_CHARGEPUMP = 0x8d;
constexpr uint8_t SSD1306_SETCONTRAST = 0x81;

struct TwoWire { uint8_t bus; };
struct DisplayTestPlatform {
  bool managerAvailable = true;
  bool busAvailable[2] = {true, true};
  bool paused[2] = {false, false};
  bool lockAvailable = true;
  bool lockUnavailableAfterBegin = false;
  bool locked = false;
  uint8_t transactionBus = 0, transactionAddress = 0;
  uint32_t transactionClock = 0;
  uint8_t detectedAddress[2] = {0x3d, 0x3d};
  TwoWire wires[2] = {{0}, {1}};
  std::deque<bool> beginResults;
  std::vector<std::pair<uint8_t, uint8_t>> probes;
  std::vector<std::pair<uint8_t, uint8_t>> registrations;
  std::vector<std::string> events;
  int constructors = 0, destructors = 0, beginCalls = 0, updates = 0;
  int commands = 0, dims = 0, powerCalls = 0, ends = 0;
};
inline DisplayTestPlatform platform;
struct Settings { int oledBus = 0; uint8_t oledBrightness = 128; bool oledFlipped = false; };
inline Settings gSettings;

inline void pinMode(int pin, int) { platform.events.push_back("pin:" + std::to_string(pin)); }
inline void digitalWrite(int pin, int value) {
  platform.events.push_back("rail:" + std::to_string(pin) + ":" + std::to_string(value));
}
inline void delay(unsigned value) { platform.events.push_back("delay:" + std::to_string(value)); }
inline void analogWrite(int, int) {}

class I2CDeviceManager {
 public:
  static I2CDeviceManager* getInstance() {
    static I2CDeviceManager manager;
    return platform.managerAvailable ? &manager : nullptr;
  }
  TwoWire* getWire(uint8_t bus) {
    return bus < 2 && platform.busAvailable[bus] ? &platform.wires[bus] : nullptr;
  }
  void registerDevice(uint8_t address, const char*, uint8_t bus) {
    platform.registrations.emplace_back(bus, address);
  }
  void performBusRecovery(uint8_t bus) {
    platform.events.push_back("recover:" + std::to_string(bus));
  }
};
inline I2CDeviceManager* i2c() { return I2CDeviceManager::getInstance(); }
inline bool pollPaused(uint8_t bus) { assert(bus < 2); return platform.paused[bus]; }
inline uint8_t i2cProbeAddress(uint8_t address, uint32_t, uint32_t, uint8_t bus) {
  platform.probes.emplace_back(bus, address);
  assert(bus < 2);
  return platform.detectedAddress[bus] == address ? 0 : 2;
}
inline bool i2cPingAddress(uint8_t address, uint32_t clock, uint32_t wait, uint8_t bus) {
  return i2cProbeAddress(address, clock, wait, bus) == 0;
}
template<typename Func>
auto i2cDeviceTransaction(uint8_t bus, uint8_t address, uint32_t clock, uint32_t, Func&& operation)
    -> decltype(operation()) {
  if (!platform.managerAvailable || bus > 1 || !platform.busAvailable[bus] || !platform.lockAvailable)
    return decltype(operation())();
  assert(!platform.locked);
  platform.locked = true;
  platform.transactionBus = bus;
  platform.transactionAddress = address;
  platform.transactionClock = clock;
  platform.events.push_back("lock");
  auto result = operation();
  platform.events.push_back("unlock");
  platform.locked = false;
  return result;
}
template<typename Func>
void i2cDeviceTransactionVoid(uint8_t bus, uint8_t address, uint32_t clock, uint32_t wait, Func&& operation) {
  i2cDeviceTransaction(bus, address, clock, wait, [&] { operation(); return true; });
}

class Adafruit_SSD1306 {
 public:
  Adafruit_SSD1306(int, int, TwoWire* wire, int, uint32_t during = 400000, uint32_t after = 100000)
      : wire_(wire) {
    assert(during == 400000 && after == 400000);
    ++platform.constructors;
  }
  ~Adafruit_SSD1306() { ++platform.destructors; }
  bool begin(uint8_t, uint8_t address, bool = true, bool periphBegin = true) {
    hardware();
    assert(!periphBegin); // Wire initialization belongs to the shared manager.
    assert(address == platform.transactionAddress);
    ++platform.beginCalls;
    platform.events.push_back("begin");
    if (platform.lockUnavailableAfterBegin) platform.lockAvailable = false;
    if (platform.beginResults.empty()) return true;
    const bool result = platform.beginResults.front();
    platform.beginResults.pop_front();
    return result;
  }
  void clearDisplay() { platform.events.push_back("clear"); }
  void display() { hardware(); ++platform.updates; platform.events.push_back("display"); }
  void dim(bool) { hardware(); ++platform.dims; }
  void ssd1306_command(uint8_t value) {
    hardware(); ++platform.commands;
    platform.events.push_back("cmd:" + std::to_string(value));
  }
  void setRotation(uint8_t) {}
 private:
  void hardware() const {
    assert(platform.locked && wire_->bus == platform.transactionBus);
    assert(platform.transactionClock == 400000);
  }
  TwoWire* wire_;
};

class P4EyeDisplay {
 public:
  P4EyeDisplay() { ++platform.constructors; }
  ~P4EyeDisplay() { ++platform.destructors; }
  bool begin() {
    ++platform.beginCalls;
    if (platform.beginResults.empty()) return true;
    const bool result = platform.beginResults.front();
    platform.beginResults.pop_front();
    return result;
  }
  void end() { ++platform.ends; }
  void clearDisplay() {}
  void display() { ++platform.updates; }
  void dim(bool) { ++platform.dims; }
  void setBrightness(uint8_t) { ++platform.commands; }
  void setPower(bool) { ++platform.powerCalls; }
  uint8_t* getBuffer() { return canvas_; }
 private:
  uint8_t canvas_[1024]{};
};
