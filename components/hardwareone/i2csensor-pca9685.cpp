/**
 * PCA9685 Servo Controller Module
 * 
 * 16-channel I2C PWM/Servo controller
 */

#include "System_BuildConfig.h"

#if ENABLE_SERVO

#include "i2csensor-pca9685.h"
#include "System_Utils.h"
#include "System_Debug.h"  // For BROADCAST_PRINTF macro
#include "System_Command.h"  // For CommandModuleRegistrar
#include <Wire.h>

// Validation macro
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

// Note: BROADCAST_PRINTF is a macro defined in debug_system.h (included via system_utils.h)

// Global PCA9685 driver instance
Adafruit_PWMServoDriver* pwmDriver = nullptr;
bool pwmDriverConnected = false;

// Servo profiles (16 channels)
ServoProfile servoProfiles[MAX_SERVO_CHANNELS];

// ============================================================================
// PCA9685 Initialization
// ============================================================================

bool initPCA9685() {
  if (pwmDriverConnected) return true;
  
  if (!pwmDriver) {
    pwmDriver = new Adafruit_PWMServoDriver(PCA9685_I2C_ADDRESS, Wire1);
    if (!pwmDriver) return false;
  }
  
  if (!pwmDriver->begin()) {
    delete pwmDriver;
    pwmDriver = nullptr;
    return false;
  }
  
  pwmDriver->setPWMFreq(50);  // 50Hz for standard servos
  pwmDriverConnected = true;
  
  // Initialize servo profiles
  for (int i = 0; i < MAX_SERVO_CHANNELS; i++) {
    servoProfiles[i].configured = false;
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

const char* cmd_servo(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "[Servo] Error: Debug buffer unavailable";
  
  // Initialize PWM driver if not already done
  if (!pwmDriverConnected) {
    if (!initPCA9685()) {
      return "[Servo] Error: PCA9685 not found at 0x40 - check wiring";
    }
  }
  
  // Parse: <channel> <angle>
  String rest = args;
  rest.trim();
  int sp1 = rest.indexOf(' ');
  if (sp1 < 0) return "Usage: servo <channel> <angle>";
  
  int channel = rest.substring(0, sp1).toInt();
  int angle = rest.substring(sp1 + 1).toInt();
  
  if (channel < 0 || channel > 15) return "[Servo] Error: Channel must be 0-15";
  if (angle < 0 || angle > 180) return "[Servo] Error: Angle must be 0-180";
  
  // Use profile if configured, otherwise use default range
  int pulseWidth;
  if (servoProfiles[channel].configured) {
    // Map angle using calibrated profile
    pulseWidth = map(angle, 0, 180, 
                     servoProfiles[channel].minPulse, 
                     servoProfiles[channel].maxPulse);
    snprintf(getDebugBuffer(), 1024, "Servo '%s' (ch %d) set to %d° (%dµs)", 
             servoProfiles[channel].name, channel, angle, pulseWidth);
  } else {
    // Use default range (500-2500µs)
    pulseWidth = map(angle, 0, 180, 500, 2500);
    snprintf(getDebugBuffer(), 1024, "WARNING: Servo channel %d set to %d° (%dµs) [uncalibrated]", 
             channel, angle, pulseWidth);
  }
  
  pwmDriver->writeMicroseconds(channel, pulseWidth);
  return getDebugBuffer();
}

const char* cmd_servoprofile(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "[Servo] Error: Debug buffer unavailable";
  
  // Parse: <channel> <minPulse> <maxPulse> <centerPulse> <name>
  String rest = args;
  rest.trim();
  if (rest.length() == 0) return "Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>";
  
  int sp2 = rest.indexOf(' ');
  if (sp2 < 0) return "Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>";
  
  int channel = rest.substring(0, sp2).toInt();
  rest = rest.substring(sp2 + 1);
  rest.trim();
  
  int sp3 = rest.indexOf(' ');
  if (sp3 < 0) return "Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>";
  
  int minPulse = rest.substring(0, sp3).toInt();
  rest = rest.substring(sp3 + 1);
  rest.trim();
  
  int sp4 = rest.indexOf(' ');
  if (sp4 < 0) return "Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>";
  
  int maxPulse = rest.substring(0, sp4).toInt();
  rest = rest.substring(sp4 + 1);
  rest.trim();
  
  int sp5 = rest.indexOf(' ');
  if (sp5 < 0) return "Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>";
  
  int centerPulse = rest.substring(0, sp5).toInt();
  String name = rest.substring(sp5 + 1);
  name.trim();
  
  if (channel < 0 || channel > 15) return "[Servo] Error: Channel must be 0-15";
  if (minPulse < 500 || minPulse > 2500) return "[Servo] Error: Min pulse must be 500-2500µs";
  if (maxPulse < 500 || maxPulse > 2500) return "[Servo] Error: Max pulse must be 500-2500µs";
  if (centerPulse < minPulse || centerPulse > maxPulse) return "[Servo] Error: Center pulse must be between min and max";
  if (name.length() == 0 || name.length() > 31) return "[Servo] Error: Name must be 1-31 characters";
  
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

const char* cmd_servolist(const String& command) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
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

const char* cmd_servocalibrate(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "[Servo] Error: Debug buffer unavailable";
  
  // Parse: <channel>
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: servocalibrate <channel>";
  
  int channel = valStr.toInt();
  if (channel < 0 || channel > 15) return "[Servo] Error: Channel must be 0-15";
  
  if (!pwmDriverConnected) {
    return "[Servo] Error: PCA9685 not initialized - run 'servo' command first";
  }
  
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
  
  // Set to safe center
  pwmDriver->writeMicroseconds(channel, 1500);
  
  snprintf(getDebugBuffer(), 1024, "Channel %d set to center (1500µs). Begin testing.", channel);
  return getDebugBuffer();
}

const char* cmd_pwm(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "[Servo] Error: Debug buffer unavailable";
  
  // Initialize PWM driver if not already done
  if (!pwmDriverConnected) {
    if (!initPCA9685()) {
      return "[Servo] Error: PCA9685 not found at 0x40 - check wiring";
    }
  }
  
  // Parse: <channel> <value> [freq]
  String rest = args;
  rest.trim();
  int sp1 = rest.indexOf(' ');
  if (sp1 < 0) return "Usage: pwm <channel> <value> [freq]";
  
  int channel = rest.substring(0, sp1).toInt();
  rest = rest.substring(sp1 + 1);
  rest.trim();
  int sp2 = rest.indexOf(' ');
  int value = (sp2 >= 0) ? rest.substring(0, sp2).toInt() : rest.toInt();
  
  if (channel < 0 || channel > 15) return "[Servo] Error: Channel must be 0-15";
  if (value < 0 || value > 4095) return "[Servo] Error: Value must be 0-4095";
  
  // Optional frequency parameter
  if (sp2 >= 0) {
    int freq = rest.substring(sp2 + 1).toInt();
    if (freq >= 24 && freq <= 1526) {
      pwmDriver->setPWMFreq(freq);
      snprintf(getDebugBuffer(), 1024, "PWM channel %d set to %d (freq: %dHz)", channel, value, freq);
    } else {
      snprintf(getDebugBuffer(), 1024, "[Servo] Error: Frequency must be 24-1526Hz");
    }
  } else {
    snprintf(getDebugBuffer(), 1024, "PWM channel %d set to %d", channel, value);
  }
  
  pwmDriver->setPWM(channel, 0, value);
  return getDebugBuffer();
}

// ============================================================================
// PCA9685/Servo Command Registry
// ============================================================================

const CommandEntry servoCommands[] = {
  { "servo", "Control servo motor: servo <channel> <angle>.", false, cmd_servo, "Usage: servo <channel> <angle>" },
  { "pwm", "Set PWM output: pwm <channel> <value> [freq].", false, cmd_pwm, "Usage: pwm <channel> <value> [freq]" },
  { "servoprofile", "Configure servo profile: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>.", false, cmd_servoprofile, "Usage: servoprofile <ch> <minPulse> <maxPulse> <centerPulse> <name>" },
  { "servolist", "List configured servo profiles.", false, cmd_servolist },
  { "servocalibrate", "Enter calibration mode: servocalibrate <channel>.", false, cmd_servocalibrate, "Usage: servocalibrate <channel>" },
};

const size_t servoCommandsCount = sizeof(servoCommands) / sizeof(servoCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _servo_cmd_registrar(servoCommands, servoCommandsCount, "servo");

#endif // ENABLE_SERVO