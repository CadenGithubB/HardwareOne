/**
 * PCA9685 Servo Controller Module
 * 
 * 16-channel I2C PWM/Servo controller
 */

#include "System_BuildConfig.h"

#if ENABLE_SERVO

#include "i2csensor_pca9685.h"
#include "System_Utils.h"
#include "System_Debug.h"  // For BROADCAST_PRINTF macro
#include "System_Command.h"
#include "System_MemUtil.h"  // PSRAM_JSON_DOC, ps_alloc, ps_delete
#include <ArduinoJson.h>
#include <Adafruit_PWMServoDriver.h>
#include "System_I2C.h"
#include "System_Settings.h"
#include <Wire.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <new>

// Note: BROADCAST_PRINTF is a macro defined in debug_system.h (included via system_utils.h)

// Driver and profiles are owned by the servo operations below.
static Adafruit_PWMServoDriver* gPwmDriver = nullptr;
bool gPwmDriverConnected = false;

// Servo profiles (16 channels)
// PSRAM .bss: plain calibration config, read on the I2C task; never DMA'd.
EXT_RAM_BSS_ATTR static ServoProfile servoProfiles[MAX_SERVO_CHANNELS];

// ============================================================================
// PCA9685 Initialization
// ============================================================================

bool servoInit() {
  if (gPwmDriverConnected) return true;

  const uint8_t servoBus = (uint8_t)gSettings.servoBus;
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  TwoWire* servoWire = mgr ? mgr->getWire(servoBus) : nullptr;
  if (!servoWire) return false;

  if (!gPwmDriver) {
    // Only the task-context wrapper moves; the library owns its I2C transport.
    void* servoObjBuf = ps_alloc(sizeof(Adafruit_PWMServoDriver), AllocPref::PreferPSRAM, "servo.obj");
    if (!servoObjBuf) return false;
    gPwmDriver = new (servoObjBuf) Adafruit_PWMServoDriver(PCA9685_I2C_ADDRESS, *servoWire);
  }

  bool success = i2cDeviceTransaction(servoBus, PCA9685_I2C_ADDRESS, 100000, 500, [&]() -> bool {
    if (!gPwmDriver->begin()) return false;
    gPwmDriver->setPWMFreq(50);  // 50Hz for standard servos
    return true;
  });
  if (!success) {
    ps_delete(gPwmDriver);
    gPwmDriver = nullptr;
    return false;
  }
  gPwmDriverConnected = true;
  
  // Keep profiles entered before the first hardware command (or an init retry).
  for (int i = 0; i < MAX_SERVO_CHANNELS; i++) {
    if (servoProfiles[i].configured) continue;
    servoProfiles[i].minPulse = 500;
    servoProfiles[i].maxPulse = 2500;
    servoProfiles[i].centerPulse = 1500;
    servoProfiles[i].name[0] = '\0';
  }
  
  return true;
}

// ============================================================================
// PCA9685/Servo Command Handlers
// ============================================================================

// CommandArgs owns tokenization; reject partial numbers and overflow before any
// driver initialization or output. Its legacy argInt() accepts numeric prefixes.
static bool parseServoNumber(const String& text, int low, int high, int& value) {
  const char* start = text.c_str();
  if (!*start || isspace(static_cast<unsigned char>(*start))) return false;
  errno = 0;
  char* end = nullptr;
  const long parsed = strtol(start, &end, 10);
  if (errno || end == start || end != start + text.length() || parsed < low || parsed > high) return false;
  value = static_cast<int>(parsed);
  return true;
}

const char* cmd_servo(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: [Servo] Debug buffer unavailable";
  
  // Parse: <channel> <angle>
  CommandArgs a(argsInput);
  if (a.count() != 2 || a.unterminatedQuote()) return "Error: invalid arguments — Usage: servo <channel> <angle>";

  int channel, angle;
  if (!parseServoNumber(a.arg(0), 0, 15, channel)) return "Error: [Servo] Channel must be an integer from 0-15";
  if (!parseServoNumber(a.arg(1), 0, 180, angle)) return "Error: [Servo] Angle must be an integer from 0-180";
  if (!servoInit()) return "Error: [Servo] PCA9685 initialization failed - check bus and wiring";
  
  // Use profile if configured, otherwise use default range
  int pulseWidth;
  if (servoProfiles[channel].configured) {
    // Map angle using calibrated profile
    pulseWidth = map(angle, 0, 180, 
                     servoProfiles[channel].minPulse, 
                     servoProfiles[channel].maxPulse);
  } else {
    // Use default range (500-2500µs)
    pulseWidth = map(angle, 0, 180, 500, 2500);
  }
  
  const bool issued = i2cDeviceTransaction((uint8_t)gSettings.servoBus, PCA9685_I2C_ADDRESS,
                           100000, 200, [&]() -> bool {
    gPwmDriver->writeMicroseconds(channel, pulseWidth);
    return true;  // The library's microsecond API exposes no write result.
  });
  if (!issued) return "Error: [Servo] I2C bus unavailable - command not sent";
  if (servoProfiles[channel].configured) {
    snprintf(getDebugBuffer(), 1024, "Servo '%s' (ch %d) request issued: %d° (%dµs)",
             servoProfiles[channel].name, channel, angle, pulseWidth);
  } else {
    snprintf(getDebugBuffer(), 1024, "WARNING: Servo channel %d request issued: %d° (%dµs) [uncalibrated]",
             channel, angle, pulseWidth);
  }
  return getDebugBuffer();
}

const char* cmd_servoprofile(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: [Servo] Debug buffer unavailable";
  
  // Parse: <channel> <minPulse> <maxPulse> <centerPulse> <name>
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(5) || a.unterminatedQuote()) return "Error: invalid arguments — Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>";

  int channel, minPulse, maxPulse, centerPulse;
  String name = a.remaining(3);
  
  if (!parseServoNumber(a.arg(0), 0, 15, channel)) return "Error: [Servo] Channel must be an integer from 0-15";
  if (!parseServoNumber(a.arg(1), 500, 2500, minPulse)) return "Error: [Servo] Min pulse must be an integer from 500-2500µs";
  if (!parseServoNumber(a.arg(2), 500, 2500, maxPulse)) return "Error: [Servo] Max pulse must be an integer from 500-2500µs";
  if (!parseServoNumber(a.arg(3), minPulse, maxPulse, centerPulse)) return "Error: [Servo] Center pulse must be an integer between min and max";
  if (name.length() == 0 || name.length() > 31) return "Error: [Servo] Name must be 1-31 characters";
  
  servoProfiles[channel].configured = true;
  servoProfiles[channel].minPulse = minPulse;
  servoProfiles[channel].maxPulse = maxPulse;
  servoProfiles[channel].centerPulse = centerPulse;
  strncpy(servoProfiles[channel].name, name.c_str(), sizeof(servoProfiles[channel].name) - 1);
  servoProfiles[channel].name[sizeof(servoProfiles[channel].name) - 1] = '\0';
  
  snprintf(getDebugBuffer(), 1024, "Servo profile saved: ch%d '%s' [%d-%dµs, center:%dµs]", 
           channel, servoProfiles[channel].name, minPulse, maxPulse, centerPulse);
  return getDebugBuffer();
}

const char* cmd_servolist(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["servos"].to<JsonArray>();
    for (int i = 0; i < MAX_SERVO_CHANNELS; i++) {
      if (!servoProfiles[i].configured) continue;
      JsonObject o = arr.add<JsonObject>();
      o["channel"]     = i;
      o["name"]        = servoProfiles[i].name;
      o["minPulse"]    = servoProfiles[i].minPulse;
      o["maxPulse"]    = servoProfiles[i].maxPulse;
      o["centerPulse"] = servoProfiles[i].centerPulse;
    }
    doc["count"] = (int)arr.size();
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  broadcastOutput("Configured Servos:");
  broadcastOutput("Ch  Name            Min    Max    Center  Status");
  broadcastOutput("--  --------------  -----  -----  ------  --------");
  
  bool anyConfigured = false;
  for (int i = 0; i < MAX_SERVO_CHANNELS; i++) {
    if (servoProfiles[i].configured) {
      anyConfigured = true;
      char line[128];
      snprintf(line, sizeof(line), "%2d  %-14.14s  %5d  %5d  %6d  Active",
               i, servoProfiles[i].name, 
               servoProfiles[i].minPulse, servoProfiles[i].maxPulse,
               servoProfiles[i].centerPulse);
      broadcastOutput(line);
    }
  }
  
  if (!anyConfigured) {
    broadcastOutput("No servos configured. Use 'servoprofile' to add.");
  }
  
  return "[Servo] Profile list displayed";
}

const char* cmd_servocalibrate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: [Servo] Debug buffer unavailable";
  
  // Parse: <channel>
  CommandArgs a(argsInput);
  if (a.count() != 1 || a.unterminatedQuote()) return "Error: invalid arguments — Usage: servocalibrate <channel>";
  int channel;
  if (!parseServoNumber(a.arg(0), 0, 15, channel)) return "Error: [Servo] Channel must be an integer from 0-15";
  
  if (!gPwmDriverConnected) {
    return "Error: [Servo] PCA9685 not initialized - run 'servo' command first";
  }

  // Only present calibration instructions after the center command was issued.
  const bool issued = i2cDeviceTransaction((uint8_t)gSettings.servoBus, PCA9685_I2C_ADDRESS,
                           100000, 200, [&]() -> bool {
    gPwmDriver->writeMicroseconds(channel, 1500);
    return true;  // The library's microsecond API exposes no write result.
  });
  if (!issued) return "Error: [Servo] I2C bus unavailable - command not sent";
  
  broadcastOutput("SERVO CALIBRATION MODE");
  broadcastOutput("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  BROADCAST_PRINTF("Calibrating channel %d", channel);
  broadcastOutput("");
  broadcastOutput("Steps:");
  broadcastOutput("1. Use 'servo <ch> <angle>' to test positions manually");
  broadcastOutput("2. Find min/max angles where servo moves WITHOUT strain");
  broadcastOutput("3. Note the pulse widths from command output");
  broadcastOutput("4. Save with: servoprofile <ch> <min> <max> <center> <name>");
  broadcastOutput("");
  broadcastOutput("Safe starting points:");
  broadcastOutput("  Standard servo:    500-2500µs (0-180°)");
  broadcastOutput("  Limited servo:     1000-2000µs (0-180°)");
  broadcastOutput("  Wide-angle servo:  500-2500µs (0-270°)");
  broadcastOutput("");
  
  snprintf(getDebugBuffer(), 1024, "Channel %d center request issued (1500µs). Check position before testing.", channel);
  return getDebugBuffer();
}

const char* cmd_pwm(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: [Servo] Debug buffer unavailable";
  
  // Parse: <channel> <value> [freq]
  CommandArgs a(argsInput);
  if (a.count() < 2 || a.count() > 3 || a.unterminatedQuote()) return "Error: invalid arguments — Usage: pwm <channel> <value> [freq]";

  int channel, value, freq = 0;
  if (!parseServoNumber(a.arg(0), 0, 15, channel)) return "Error: [Servo] Channel must be an integer from 0-15";
  if (!parseServoNumber(a.arg(1), 0, 4095, value)) return "Error: [Servo] Value must be an integer from 0-4095";
  if (a.has(2) && !parseServoNumber(a.arg(2), 24, 1526, freq)) return "Error: [Servo] Frequency must be an integer from 24-1526Hz";
  if (!servoInit()) return "Error: [Servo] PCA9685 initialization failed - check bus and wiring";

  // Frequency is chip-wide. Keep it and this channel's update under one lock.
  // setPWM reports its transfer result; setPWMFreq does not expose one.
  bool attempted = false;
  const bool success = i2cDeviceTransaction((uint8_t)gSettings.servoBus, PCA9685_I2C_ADDRESS,
                           100000, 200, [&]() -> bool {
    attempted = true;
    if (a.has(2)) gPwmDriver->setPWMFreq(freq);
    return gPwmDriver->setPWM(channel, 0, value) == 0;
  });
  if (!attempted) return "Error: [Servo] I2C bus unavailable - command not sent";
  if (!success) return a.has(2)
      ? "Error: [Servo] PWM write failed after frequency request; frequency may have changed"
      : "Error: [Servo] PWM write failed";

  if (a.has(2)) {
    snprintf(getDebugBuffer(), 1024, "PWM channel %d write accepted: %d (chip frequency request: %dHz)", channel, value, freq);
  } else {
    snprintf(getDebugBuffer(), 1024, "PWM channel %d write accepted: %d", channel, value);
  }
  return getDebugBuffer();
}

// ============================================================================
// PCA9685/Servo Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
// Bus routing for this device. Lives here rather than in System_I2C.cpp so it
// compiles out with the driver — see i2cSetDeviceBusAndReport in System_I2C.h.
static const char* cmd_servobus(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.servoBus, a, "servoBus");
}

const CommandEntry servoCommands[] = {
  { "servo", "Control servo motor: servo <channel> <angle>.", false, cmd_servo, "Usage: servo <channel> <angle>" },
  { "pwm", "Set PWM output: pwm <channel> <value> [freq].", false, cmd_pwm, "Usage: pwm <channel> <value> [freq]" },
  { "servoprofile", "Configure servo profile: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>.", false, cmd_servoprofile, "Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>" },
  { "servolist", "List configured servo profiles. (add 'json' for JSON output)", false, cmd_servolist },
  { "servocalibrate", "Enter calibration mode: servocalibrate <channel>.", false, cmd_servocalibrate, "Usage: servocalibrate <channel>" },
  { "servobus",    "Route PCA9685 servo to bus: <0|1> (reboot required)", true, cmd_servobus,  "Usage: servoBus <0|1>" },
};

const size_t servoCommandsCount = sizeof(servoCommands) / sizeof(servoCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

#endif // ENABLE_SERVO
