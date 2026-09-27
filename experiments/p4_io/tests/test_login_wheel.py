"""Run the actual login input handler with fake keyboard/authentication boundaries.

The private app must first be reconstructed; override its location with
HW1_P4_IO_APP. No board or firmware build is used. The negative control restores
only the old no-input guard and must fail both wheel-navigation scenarios.

Run: python3 -B experiments/p4_io/tests/test_login_wheel.py -v
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest


APP = Path(os.environ.get(
    "HW1_P4_IO_APP", Path(__file__).resolve().parents[1] / "private/app"))


def function(source, signature):
    """Extract a top-level function, retaining its original body unchanged."""
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


PLATFORM = r'''
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
using String = std::string;
constexpr int JOYSTICK_DEADZONE = 10;
constexpr int SOURCE_LOCAL_DISPLAY = 1;
constexpr int SYSEVT_LOGIN_OK = 2, SYSEVT_LOGIN_FAIL = 3;
enum class OLEDKeyboardDictationPolicy { ALLOW_PLAINTEXT, DENY };
struct { bool up = false, down = false; } gNavEvents;
struct { bool localDisplayRequireAuth = true; } gSettings;
bool authenticated = false, loginAllowed = false;
bool keyboardCompleted = false, keyboardCancelled = false;
int keyboardStarts = 0, keyboardResets = 0, loginAttempts = 0, event = 0;
unsigned long dirtyUntil = 0;
String keyboardText, keyboardTitle, keyboardInitial;
OLEDKeyboardDictationPolicy keyboardPolicy = OLEDKeyboardDictationPolicy::DENY;
bool inputIsButtonPressed(uint32_t state, InputButton button) {
  return (state & (1u << button)) != 0;
}
#define INPUT_CHECK(state, button) inputIsButtonPressed(state, button)
unsigned long millis() { return 10000; }
void secureClearString(String& text) { text.clear(); }
bool oledKeyboardIsCompleted() { return keyboardCompleted; }
bool oledKeyboardIsCancelled() { return keyboardCancelled; }
const char* oledKeyboardGetText() { return keyboardText.c_str(); }
void oledKeyboardReset() { ++keyboardResets; }
void oledKeyboardInit(const char* title, const char* initial, int,
                      OLEDKeyboardDictationPolicy policy) {
  ++keyboardStarts; keyboardTitle = title; keyboardInitial = initial;
  keyboardPolicy = policy;
}
bool isTransportAuthenticated(int) { return authenticated; }
bool loginTransport(int, const String&, const String&) {
  ++loginAttempts; authenticated = loginAllowed; return loginAllowed;
}
void systemEventPost(int value, const char*, const char*) { event = value; }
void oledMarkDirtyUntil(unsigned long until) { dirtyUntil = until; }
#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "CHECK failed: " #condition << '\n'; return 1; } } while (0)
'''


CHECKS = r'''
int main(int argc, char** argv) {
  CHECK(argc == 2);
  const String scenario = argv[1];
  if (scenario == "wheel-down" || scenario == "wheel-up") {
    const bool down = scenario == "wheel-down";
    gNavEvents.down = down; gNavEvents.up = !down;
    for (int step = 1; step <= 3; ++step) {
      CHECK(handleLoginModeInput(0, 0, 0));
      CHECK(currentField == (down ? step % 3 : (step * 2) % 3));
    }
    CHECK(!authenticated && loginAttempts == 0 && keyboardStarts == 0);
  } else if (scenario == "neutral") {
    currentField = FIELD_PASSWORD;
    CHECK(!handleLoginModeInput(0, 0, 0));
    CHECK(!handleLoginModeInput(1, -1, 0));
    CHECK(currentField == FIELD_PASSWORD && loginAttempts == 0);
  } else if (scenario == "select") {
    usernameBuffer = "fixture-user";
    CHECK(handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_A));
    CHECK(loginKeyboardActive && keyboardStarts == 1);
    CHECK(keyboardTitle == "Enter Username:" && keyboardInitial == "fixture-user");
    CHECK(keyboardPolicy == OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT);
    resetLoginSessionState(); currentField = FIELD_PASSWORD;
    CHECK(handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_A));
    CHECK(loginKeyboardActive && keyboardStarts == 2);
    CHECK(keyboardTitle == "Enter Password:");
    CHECK(keyboardPolicy == OLEDKeyboardDictationPolicy::DENY);
    CHECK(!authenticated && loginAttempts == 0);
  } else if (scenario == "back-auth") {
    currentField = FIELD_PASSWORD;
    usernameBuffer = "fixture-user"; passwordBuffer = "fixture-password";
    CHECK(handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_B));
    CHECK(currentField == FIELD_PASSWORD && errorMessage == "Login required");
    CHECK(!authenticated && loginAttempts == 0 && dirtyUntil == 15000);
    authenticated = true;
    CHECK(!handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_B));
    CHECK(currentField == FIELD_USERNAME && usernameBuffer.empty() && passwordBuffer.empty());
    authenticated = false; gSettings.localDisplayRequireAuth = false;
    currentField = FIELD_PASSWORD; passwordBuffer = "fixture-password";
    CHECK(!handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_B));
    CHECK(currentField == FIELD_USERNAME && passwordBuffer.empty());
  } else if (scenario == "keyboard") {
    loginKeyboardActive = true; gNavEvents.down = true;
    CHECK(!handleLoginModeInput(0, 0, 0));
    CHECK(currentField == FIELD_USERNAME && loginKeyboardActive);
    keyboardCompleted = true; keyboardText = "fixture-user";
    CHECK(handleLoginModeInput(0, 0, 0));
    CHECK(usernameBuffer == "fixture-user" && !loginKeyboardActive && keyboardResets == 1);
    keyboardCompleted = false; keyboardCancelled = true;
    loginKeyboardActive = true; currentField = FIELD_PASSWORD;
    CHECK(handleLoginModeInput(0, 0, 0));
    CHECK(currentField == FIELD_USERNAME && !loginKeyboardActive && keyboardResets == 2);
  } else if (scenario == "credentials") {
    currentField = FIELD_LOGIN_BUTTON;
    CHECK(handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_A));
    CHECK(loginAttempts == 0 && !authenticated && errorMessage == "Enter user/pass");
    usernameBuffer = "fixture-user"; passwordBuffer = "fixture-password";
    CHECK(handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_A));
    CHECK(loginAttempts == 1 && !authenticated && event == SYSEVT_LOGIN_FAIL);
    CHECK(errorMessage == "Invalid credentials");
    loginAllowed = true;
    CHECK(handleLoginModeInput(0, 0, 1u << INPUT_BUTTON_A));
    CHECK(loginAttempts == 2 && authenticated && event == SYSEVT_LOGIN_OK);
    CHECK(currentField == FIELD_USERNAME && usernameBuffer.empty() && passwordBuffer.empty());
    CHECK(errorMessage.empty() && errorDisplayUntil == 0 && !loginKeyboardActive);
  } else { CHECK(false); }
  return 0;
}
'''


class LoginWheelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        production = APP / "components/hardwareone"
        if not compiler or not (production / "OLED_Mode_Auth.cpp").is_file():
            raise unittest.SkipTest("Prepared p4_io app and C++ compiler are required")
        source = (production / "OLED_Mode_Auth.cpp").read_text()
        inputs = (production / "HAL_Input.h").read_text()
        buttons = re.search(r"enum InputButton \{.*?\n\};", inputs, re.S).group()
        state = source[source.index("enum LoginField {"):source.index("// Identity-boundary teardown.")]
        reset = function(source, "static void resetLoginSessionState()")
        handler = function(source, "static bool handleLoginModeInput(")
        start = handler.index("  if (newlyPressed == 0")
        end = handler.index("  bool handled = false;", start)
        old_guard = '''  // Early return if no meaningful input
  if (newlyPressed == 0 && abs(deltaX) < JOYSTICK_DEADZONE && abs(deltaY) < JOYSTICK_DEADZONE) {
    return false;
  }

'''
        old_handler = handler[:start] + old_guard + handler[end:]
        cls.work = tempfile.TemporaryDirectory(prefix="hw1-login-wheel-")
        cls.addClassCleanup(cls.work.cleanup)
        cls.binaries = {}
        for name, body in (("current", handler), ("old-guard", old_handler)):
            cpp = Path(cls.work.name) / (name + ".cpp")
            cpp.write_text(buttons + "\n" + PLATFORM + state + reset + body + CHECKS)
            binary = cpp.with_suffix("")
            result = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", str(cpp), "-o", str(binary),
            ], text=True, capture_output=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[name] = binary

    def run_scenario(self, binary, scenario):
        return subprocess.run([str(self.binaries[binary]), scenario],
                              text=True, capture_output=True)

    def test_wheel_only_changes_fields_and_wraps(self):
        for scenario in ("wheel-up", "wheel-down"):
            with self.subTest(scenario=scenario):
                result = self.run_scenario("current", scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_neutral_buttons_keyboard_and_authentication(self):
        for scenario in ("neutral", "select", "back-auth", "keyboard", "credentials"):
            with self.subTest(scenario=scenario):
                result = self.run_scenario("current", scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pre_fix_guard_fails_both_wheel_directions(self):
        for scenario in ("wheel-up", "wheel-down"):
            with self.subTest(scenario=scenario):
                result = self.run_scenario("old-guard", scenario)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("CHECK failed: handleLoginModeInput(0, 0, 0)", result.stderr)


if __name__ == "__main__":
    unittest.main()
