// Production definitions are inserted by test_servo_command_outcomes.py.
// String storage, allocation, driver transfers and bus admission are boundaries.
#include <Arduino.h>
#include <cassert>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
#define EXT_RAM_BSS_ATTR
#define PCA9685_I2C_ADDRESS 0x40
#define MAX_SERVO_CHANNELS 16
static bool validateOnly;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validateOnly) return "VALID"; } while (0)

// INSERT_TYPES

struct { int servoBus = 0; } gSettings;
struct TwoWire {} wire;
static bool wireAvailable, allocate, beginResult, bufferAvailable, transactionHeld;
static int allocations, frees, getWireCalls, transactionCalls, denyTransaction;
static int beginCalls, frequencyCalls, pulseCalls, pwmCalls, driverResult;
static int lastChannel, lastPulse, lastPWM, lastFrequency;
static std::vector<std::string> calls, output;
static char debugBuffer[1024];
static bool ensureDebugBuffer() { assert(!transactionHeld); return bufferAvailable; }
static char* getDebugBuffer() { assert(!transactionHeld); return debugBuffer; }
static long map(long x, long inLow, long inHigh, long outLow, long outHigh) {
  return (x - inLow) * (outHigh - outLow) / (inHigh - inLow) + outLow;
}
static void broadcastOutput(const char* text) {
  assert(!transactionHeld && pulseCalls > 0);
  output.emplace_back(text);
}
#define BROADCAST_PRINTF(...) do { char text[256]; std::snprintf(text, sizeof(text), __VA_ARGS__); broadcastOutput(text); } while (0)
enum class AllocPref { PreferPSRAM };
static void* ps_alloc(size_t size, AllocPref pref, const char* tag) {
  assert(!transactionHeld && pref == AllocPref::PreferPSRAM && std::strcmp(tag, "servo.obj") == 0);
  if (!allocate) return nullptr;
  ++allocations;
  return std::malloc(size);
}
template<class T> static void ps_delete(T* value) {
  assert(!transactionHeld);
  if (value) { value->~T(); std::free(value); ++frees; }
}
struct I2CDeviceManager {
  static I2CDeviceManager* getInstance() { static I2CDeviceManager manager; return &manager; }
  TwoWire* getWire(uint8_t bus) { ++getWireCalls; assert(bus == gSettings.servoBus); return wireAvailable ? &wire : nullptr; }
};
template<class Operation>
static bool i2cDeviceTransaction(uint8_t bus, uint8_t address, uint32_t hz,
                                 uint32_t wait, Operation operation) {
  assert(!transactionHeld && bus == gSettings.servoBus && address == PCA9685_I2C_ADDRESS);
  assert(hz == 100000 && (wait == 500 || wait == 200));
  if (++transactionCalls == denyTransaction) return false;
  transactionHeld = true;
  calls.emplace_back("lock");
  const bool result = operation();
  calls.emplace_back("unlock");
  transactionHeld = false;
  return result;
}
struct Adafruit_PWMServoDriver {
  Adafruit_PWMServoDriver(uint8_t address, TwoWire& bus) {
    assert(!transactionHeld && address == PCA9685_I2C_ADDRESS && &bus == &wire);
  }
  bool begin() { assert(transactionHeld); ++beginCalls; calls.emplace_back("begin"); return beginResult; }
  void setPWMFreq(float frequency) {
    assert(transactionHeld); ++frequencyCalls; lastFrequency = frequency; calls.emplace_back("frequency");
  }
  void writeMicroseconds(uint8_t channel, uint16_t pulse) {
    assert(transactionHeld); ++pulseCalls; lastChannel = channel; lastPulse = pulse; calls.emplace_back("pulse");
  }
  uint8_t setPWM(uint8_t channel, uint16_t on, uint16_t off) {
    assert(transactionHeld && on == 0); ++pwmCalls; lastChannel = channel; lastPWM = off; calls.emplace_back("pwm");
    return driverResult;
  }
};

// INSERT_STATE
// INSERT_FUNCTIONS

static void reset() {
  ps_delete(gPwmDriver); gPwmDriver = nullptr; gPwmDriverConnected = false;
  std::memset(servoProfiles, 0, sizeof(servoProfiles));
  allocations = frees = getWireCalls = transactionCalls = 0;
  beginCalls = frequencyCalls = pulseCalls = pwmCalls = driverResult = 0;
  denyTransaction = -1; lastChannel = lastPulse = lastPWM = lastFrequency = -1;
  validateOnly = transactionHeld = false;
  wireAvailable = allocate = beginResult = bufferAvailable = true;
  calls.clear(); output.clear(); debugBuffer[0] = '\0'; gSettings.servoBus = 0;
}
static bool contains(const char* text, const char* needle) { return std::strstr(text, needle) != nullptr; }
static bool failed(const char* text) { return std::strncmp(text, "Error:", 6) == 0; }
static void noHardware() {
  assert(allocations == 0 && getWireCalls == 0 && transactionCalls == 0);
  assert(beginCalls == 0 && frequencyCalls == 0 && pulseCalls == 0 && pwmCalls == 0 && output.empty());
}
static void ready() { assert(servoInit()); calls.clear(); }
using Handler = const char* (*)(const String&);
static void invalid(Handler handler, const char* args) {
  reset(); assert(failed(handler(args))); noHardware();
  ready();
  const auto before = calls;
  const int transactions = transactionCalls;
  assert(failed(handler(args)));
  assert(calls == before && transactionCalls == transactions && pulseCalls == 0 && pwmCalls == 0 && output.empty());
}

int main() {
  for (const char* input : {"", "0", "0 90 extra", "16 90", "-1 90", "0 181", "0 -1",
       "a 90", "1junk 90", "0 90junk", "0 90.0", "0 1e2", "0 999999999999999999999999999999",
       "0 \"90", "\"0\" \"90", "\" 0\" 90", "0 \"90 \""}) invalid(cmd_servo, input);
  for (const char* input : {"", "0", "0 100 50 extra", "16 100", "0 4096", "0 -1",
       "0 100 23", "0 100 1527", "0 100 -24", "0 100 bad", "0 100 50Hz", "0 100 50.0",
       "0 100 999999999999999999999999999", "0 100 \"50", "0junk 100", "0 100junk"}) invalid(cmd_pwm, input);
  for (const char* input : {"", "0 extra", "junk", "1junk", "0.5", "16", "-1", "\"0", "9999999999999999999999999"}) invalid(cmd_servocalibrate, input);
  for (auto handler : {cmd_servo, cmd_pwm}) {
    reset(); assert(failed(handler(String(std::string("0 90\0junk", 9))))); noHardware();
  }
  for (const char* input : {"", "0 500 2500 1500", "0x 500 2500 1500 name", "0 500us 2500 1500 name",
       "0 500 2500x 1500 name", "0 500 2500 1500x name", "0 2500 500 1500 name",
       "0 500 2500 2501 name", "0 500 2500 1500 \"name", "0 500 2500 1500 a-name-longer-than-thirty-one-characters"}) {
    reset(); assert(failed(cmd_servoprofile(input))); noHardware(); assert(!servoProfiles[0].configured);
  }
  for (auto handler : {cmd_servo, cmd_pwm, cmd_servocalibrate, cmd_servoprofile}) {
    reset(); validateOnly = true; assert(std::strcmp(handler("invalid"), "VALID") == 0); noHardware();
  }

  reset(); bufferAvailable = false; assert(failed(cmd_servo("0 90"))); noHardware();
  reset(); wireAvailable = false;
  assert(failed(cmd_pwm("0 100")) && allocations == 0 && transactionCalls == 0);
  reset(); allocate = false;
  assert(failed(cmd_servo("0 90")) && allocations == 0 && transactionCalls == 0);
  reset(); beginResult = false;
  assert(failed(cmd_servo("0 90")) && !gPwmDriver && !gPwmDriverConnected);
  assert(allocations == 1 && frees == 1 && pulseCalls == 0 && frequencyCalls == 0);
  beginResult = true;
  assert(contains(cmd_servo("0 90"), "request issued") && lastPulse == 1500 && allocations == 2);

  // A stored calibration survives both failed initialization and the first move.
  reset();
  assert(!failed(cmd_servoprofile("3 700 2300 1500 Arm joint"))); noHardware();
  denyTransaction = 1;
  assert(failed(cmd_servo("3 0")) && !gPwmDriver && frees == 1 && pulseCalls == 0);
  assert(servoProfiles[3].configured);
  const char* result = cmd_servo("3 0");
  assert(contains(result, "Arm joint") && contains(result, "request issued") && lastPulse == 700);
  assert(contains(cmd_servo("3 180"), "2300") && lastPulse == 2300);

  for (int bus : {0, 1}) {
    reset(); gSettings.servoBus = bus;
    assert(contains(cmd_servo("15 180"), "request issued") && lastChannel == 15 && lastPulse == 2500);
    assert(contains(cmd_servo("0 0"), "uncalibrated") && lastChannel == 0 && lastPulse == 500);
    assert(contains(cmd_pwm("15 4095 1526"), "write accepted") && lastChannel == 15 && lastPWM == 4095 && lastFrequency == 1526);
    assert(contains(cmd_pwm("0 0 24"), "frequency request") && lastPWM == 0 && lastFrequency == 24);
    assert(contains(cmd_pwm("0 100"), "write accepted") && lastPWM == 100 && lastFrequency == 24);
  }

  // An unavailable command transaction must not claim a write or show calibration.
  for (auto handler : {cmd_servo, cmd_pwm, cmd_servocalibrate}) {
    reset(); ready(); denyTransaction = transactionCalls + 1;
    result = handler(handler == cmd_servocalibrate ? "0" : "0 90");
    assert(failed(result) && contains(result, "not sent"));
    assert(pulseCalls == 0 && pwmCalls == 0 && output.empty() && calls.empty());
  }
  // Initialization may succeed before a subsequent output admission fails.
  reset(); denyTransaction = 2;
  assert(contains(cmd_pwm("0 100 50"), "not sent") && pwmCalls == 0 && frequencyCalls == 1);
  assert(gPwmDriverConnected);

  reset(); ready(); driverResult = 1;
  assert(contains(cmd_pwm("0 100"), "PWM write failed") && pwmCalls == 1);
  calls.clear();
  result = cmd_pwm("0 100 60");
  assert(failed(result) && contains(result, "frequency may have changed"));
  assert((calls == std::vector<std::string>{"lock", "frequency", "pwm", "unlock"}));
  driverResult = 0; calls.clear();
  assert(contains(cmd_pwm("0 100 60"), "write accepted"));
  assert((calls == std::vector<std::string>{"lock", "frequency", "pwm", "unlock"}));

  reset(); assert(failed(cmd_servocalibrate("0"))); noHardware();
  ready();
  assert(contains(cmd_servocalibrate("0"), "center request issued"));
  assert(pulseCalls == 1 && lastPulse == 1500 && !output.empty());
  assert((calls == std::vector<std::string>{"lock", "pulse", "unlock"}));

  reset();
  std::puts("PCA9685 command outcomes passed (production parsing/commands; controlled I2C/driver boundaries)");
}
