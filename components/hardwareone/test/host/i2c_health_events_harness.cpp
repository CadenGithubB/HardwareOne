// Drives the production I2C device-health edges (System_I2C_Manager.cpp) into
// the production notification rules and message formatter
// (System_Notifications.cpp) and the real event catalog.
//
// Cases:
//   never_answered  a pre-registered sensor that is not plugged in degrades
//                   silently; only a device that answered this boot is lost.
//   outage          the third failure in a row announces it once; the 30 s
//                   retries while it stays unplugged are quiet; the next
//                   success announces it back once.
//   manual_recover  i2crecover clears degraded without a success: no second
//                   announcement, and the next success still reports it back.
//   naming          a placeholder-named device is announced by address; bus
//                   faults alone (which never degrade a device) announce
//                   nothing.
//   two_devices     sensors dropping together each get their own notice.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define DEBUG_SYSTEM_H
#define SYSTEM_BUILDCONFIG_H
// Log lines are unevaluated but still type-checked against their arguments.
#define DEBUG_I2CF(...) do { } while (0)
#define INFO_I2CF(...) do { (void)sizeof(std::printf(__VA_ARGS__)); } while (0)
#define WARN_I2CF(...) do { (void)sizeof(std::printf(__VA_ARGS__)); } while (0)
#define ERROR_I2CF(...) do { (void)sizeof(std::printf(__VA_ARGS__)); } while (0)
#define private public
#include "System_I2C_Manager.h"
#undef private

class String;
#include "System_Notifications.h"

TwoWire Wire;
TwoWire Wire1;
static uint32_t gNow = 0;
uint32_t millis() { return gNow; }
uint32_t micros() { return gNow * 1000u; }

static unsigned logErrors = 0;
static unsigned logRecoveries = 0;
static unsigned recoveryChecks = 0;
void logI2CError(uint8_t, const char*, int, int, bool) { ++logErrors; }
void logI2CRecovery(uint8_t, const char*, int) { ++logRecoveries; }

I2CDeviceManager* I2CDeviceManager::instance = nullptr;
I2CDeviceManager* I2CDeviceManager::getInstance() { return instance; }
void I2CDeviceManager::checkBusRecoveryNeeded() { ++recoveryChecks; }
bool I2CDeviceManager::busRecoveryThrottled(uint8_t) const { return true; }
void I2CDeviceManager::performBusRecovery(uint8_t) { assert(!"throttled in this harness"); }

struct Posted {
  uint8_t kind;
  std::string subject;
  std::string detail;
};
static std::vector<Posted> posted;
void systemEventPost(uint8_t kind, const char* subject, const char* detail,
                     uint8_t, const char*) {
  posted.push_back({kind, subject ? subject : "", detail ? detail : ""});
}

// INSERT_PRODUCTION_DEFINITIONS

static void reset() {
  posted.clear();
  logErrors = logRecoveries = recoveryChecks = 0;
  gNow = 1000;
}

// The posted event as every surface shows it: canonical name, banner text and
// the compiled default rule.
static void expectShown(const Posted& p, uint8_t kind, const char* name,
                        const char* subject, const char* detail,
                        const char* text, uint8_t level) {
  assert(p.kind == kind);
  assert(std::strcmp(systemEventKindName(kind), name) == 0);
  assert(systemEventKindFamily(kind) == SYSEVT_FAM_SENSORS);
  assert(p.subject == subject);
  assert(p.detail == detail);

  SystemEvent e{};
  e.kind = p.kind;
  std::snprintf(e.subject, sizeof(e.subject), "%s", p.subject.c_str());
  std::snprintf(e.detail, sizeof(e.detail), "%s", p.detail.c_str());
  char msg[64];
  notifFormatEvent(e, msg, sizeof(msg));
  assert(std::strcmp(msg, text) == 0);

  const NotifRule rule = notifDefaultRuleFor(kind);
  assert(rule.sinks == (NSINK_BANNER | NSINK_QUEUE | NSINK_TOAST));
  assert(rule.level == level);
  assert(rule.cooldownMs == 0);
}

static void failTimes(I2CDevice& d, I2CErrorType type, int times) {
  for (int i = 0; i < times; ++i) d.recordError(type, type == I2CErrorType::TIMEOUT ? 3 : 2);
}

int main() {
  static I2CDeviceManager manager;
  I2CDeviceManager::instance = &manager;

  {  // never_answered
    reset();
    I2CDevice d;
    d.init(0x28, "BNO055", 0);
    failTimes(d, I2CErrorType::NACK, 6);
    assert(d.health.degraded && !d.health.reportedLost);
    assert(posted.empty());
    assert(logErrors == 4 && recoveryChecks == 4);  // existing log/recovery path unchanged
  }

  {  // outage
    reset();
    I2CDevice d;
    d.init(0x28, "BNO055", 0);
    d.recordSuccess();
    failTimes(d, I2CErrorType::NACK, 2);
    assert(posted.empty() && !d.health.degraded);
    failTimes(d, I2CErrorType::NACK, 1);
    assert(d.health.degraded && posted.size() == 1);
    expectShown(posted[0], SYSEVT_SENSOR_UNRESPONSIVE, "sensor_unresponsive",
                "BNO055", "0x28 bus 0 nack", "BNO055 not responding", 2);
    for (int i = 0; i < 5; ++i) {
      gNow += 31000;
      assert(!d.isDegraded());  // the transaction path lets one retry through
      failTimes(d, I2CErrorType::NACK, 1);
    }
    assert(posted.size() == 1);
    assert(logErrors == 6 && recoveryChecks == 6);
    gNow += 31000;
    d.recordSuccess();
    assert(posted.size() == 2 && !d.health.degraded && logRecoveries == 1);
    expectShown(posted[1], SYSEVT_SENSOR_RECOVERED, "sensor_recovered",
                "BNO055", "0x28 bus 0", "BNO055 reconnected", 1);
    d.recordSuccess();
    assert(posted.size() == 2);
    failTimes(d, I2CErrorType::NACK, 3);  // a later outage is announced again
    assert(posted.size() == 3 && posted[2].kind == SYSEVT_SENSOR_UNRESPONSIVE);
  }

  {  // manual_recover
    reset();
    I2CDevice d;
    d.init(0x5A, "STHS34PF80", 1);
    d.recordSuccess();
    failTimes(d, I2CErrorType::TIMEOUT, 3);
    assert(posted.size() == 1);
    expectShown(posted[0], SYSEVT_SENSOR_UNRESPONSIVE, "sensor_unresponsive",
                "STHS34PF80", "0x5A bus 1 timeout", "STHS34PF80 not responding", 2);
    d.attemptRecovery();
    assert(!d.health.degraded && d.health.reportedLost);
    failTimes(d, I2CErrorType::TIMEOUT, 3);
    assert(d.health.degraded && posted.size() == 1);
    d.recordSuccess();
    assert(posted.size() == 2);
    expectShown(posted[1], SYSEVT_SENSOR_RECOVERED, "sensor_recovered",
                "STHS34PF80", "0x5A bus 1", "STHS34PF80 reconnected", 1);
  }

  {  // naming
    reset();
    I2CDevice d;
    d.init(0x50, "Auto", 1);
    d.recordSuccess();
    failTimes(d, I2CErrorType::BUS_ERROR, 4);
    failTimes(d, I2CErrorType::BUFFER_OVERFLOW, 4);
    assert(!d.health.degraded && posted.empty());
    d.recordSuccess();
    assert(posted.empty());
    failTimes(d, I2CErrorType::NACK, 3);
    assert(posted.size() == 1);
    expectShown(posted[0], SYSEVT_SENSOR_UNRESPONSIVE, "sensor_unresponsive",
                "I2C 0x50", "0x50 bus 1 nack", "I2C 0x50 not responding", 2);
  }

  {  // two_devices
    reset();
    I2CDevice imu;
    I2CDevice tof;
    imu.init(0x28, "BNO055", 0);
    tof.init(0x29, "VL53L4CX", 0);
    imu.recordSuccess();
    tof.recordSuccess();
    for (int i = 0; i < 3; ++i) {
      failTimes(imu, I2CErrorType::NACK, 1);
      failTimes(tof, I2CErrorType::NACK, 1);
    }
    assert(posted.size() == 2);
    assert(posted[0].subject == "BNO055" && posted[1].subject == "VL53L4CX");
    tof.recordSuccess();
    imu.recordSuccess();
    assert(posted.size() == 4);
    assert(posted[2].kind == SYSEVT_SENSOR_RECOVERED && posted[2].subject == "VL53L4CX");
    assert(posted[3].kind == SYSEVT_SENSOR_RECOVERED && posted[3].subject == "BNO055");
  }

  std::puts("I2C health event checks passed");
  return 0;
}
