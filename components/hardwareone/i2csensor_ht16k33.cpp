#include "i2csensor_ht16k33.h"

#if ENABLE_LED_MATRIX
#include <Adafruit_LEDBackpack.h>
#include <errno.h>
#include <stdlib.h>
#include "System_Command.h"
#include "System_I2C.h"
#include "System_Settings.h"
#include "System_AuthIdentity.h"
#include "HT16K33_Geometry.h"

namespace {
// Use Adafruit's tested pixel mapping/font renderer, but own the transport:
// its begin/writeDisplay/brightness methods discard I2C write failures.
class MatrixCanvas : public Adafruit_8x16matrix {
 public:
  bool square = false;
  int panel = 0;
  void configure(int rotation, bool useSquare, int selectedPanel) {
    setRotation(rotation);
    square = useSquare;
    panel = selectedPanel;
    if (square) { _width = 8; _height = 8; }
  }
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    int column, row;
    if (!ht16k33MapPixel(x, y, getRotation(), square, panel, column, row)) return;
    if (color) displaybuffer[row] |= uint16_t(1U << column);
    else displaybuffer[row] &= ~uint16_t(1U << column);
  }
};
MatrixCanvas matrix;
bool connected = false;
bool displayOn = false;
uint8_t blink = 0;

bool isConnected() { return connected; }

const SettingEntry settings[] = {
  { "matrixBus", SETTING_INT, &gSettings.matrixBus, 0, 0, nullptr, 0, 1,
    "I2C bus (reboot required)", "0|I2C1,1|I2C2", false, nullptr, "matrixbus" },
  { "matrixAddress", SETTING_INT, &gSettings.matrixAddress, 112, 0, nullptr, 112, 119,
    "I2C address (reboot required)", "112|0x70,113|0x71,114|0x72,115|0x73,116|0x74,117|0x75,118|0x76,119|0x77", false, nullptr, "matrixaddress" },
  { "matrixBrightness", SETTING_INT, &gSettings.matrixBrightness, 4, 0, nullptr, 0, 15,
    "Brightness (applied on next matrix command)", nullptr, false, nullptr, "matrixbrightness" },
  { "matrixRotation", SETTING_INT, &gSettings.matrixRotation, 1, 0, nullptr, 0, 3,
    "Rotation (applied on next matrix command)", "0|8x16,1|16x8,2|8x16 inverted,3|16x8 inverted", false, nullptr, "matrixrotation" },
  { "matrixSquare", SETTING_BOOL, &gSettings.matrixSquare, 0, 0, nullptr, 0, 1,
    "Use one 8x8 square (next matrix command)", nullptr, false, nullptr, "matrixsquare" },
  { "matrixPanel", SETTING_INT, &gSettings.matrixPanel, 0, 0, nullptr, 0, 1,
    "8x8 square (next matrix command)", "0|First square,1|Second square", false, nullptr, "matrixpanel" },
};

bool parseNumber(const String& text, int low, int high, int& value) {
  if (text.isEmpty()) return false;
  errno = 0;
  char* end = nullptr;
  const long n = strtol(text.c_str(), &end, text.startsWith("0x") ? 16 : 10);
  if (errno || !end || *end || n < low || n > high) return false;
  value = static_cast<int>(n);
  return true;
}

// Caller owns the bus mutex. Never nest manager transactions here.
bool writeBytes(TwoWire& wire, const uint8_t* bytes, size_t count) {
  wire.beginTransmission(matrixDeviceAddress());
  const size_t written = wire.write(bytes, count);
  const uint8_t error = wire.endTransmission();
  return written == count && error == 0;
}

bool writeCommand(TwoWire& wire, uint8_t command) {
  return writeBytes(wire, &command, 1);
}

bool flush(TwoWire& wire, bool on, int brightness, uint8_t blinkRate) {
  // Reassert configuration on each update so a reconnected/reset backpack
  // recovers without a task or an extra initialization allocation.
  if (!writeCommand(wire, 0x21)) return false; // oscillator on
  uint8_t bytes[17] = {};
  for (size_t row = 0; row < 8; ++row) {
    bytes[1 + row * 2] = matrix.displaybuffer[row] & 0xff;
    bytes[2 + row * 2] = matrix.displaybuffer[row] >> 8;
  }
  if (!writeBytes(wire, bytes, sizeof(bytes)) ||
      !writeCommand(wire, 0xe0 | (brightness & 15)) ||
      !writeCommand(wire, 0x80 | (on ? 1 : 0) | (blinkRate << 1))) return false;
  displayOn = on;
  blink = blinkRate;
  return true;
}

const char* cmdMatrix(const String& input) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs args(input);
  String op = args.has(0) ? args.arg(0) : "status";
  op.toLowerCase();
  int x = 0, y = 0, value = 0;
  const int rotation = gSettings.matrixRotation;
  const int width = gSettings.matrixSquare ? 8 : ((rotation & 1) ? 16 : 8);
  const int height = gSettings.matrixSquare ? 8 : ((rotation & 1) ? 8 : 16);
  const bool status = op == "status";
  const bool simple = status || op == "clear" || op == "fill" ||
                      op == "on" || op == "off" || op == "test";
  if (args.unterminatedQuote()) return "Error: unterminated quote";
  if (simple) {
    if (args.count() > 1) return "Error: unexpected matrix arguments";
  } else if (op == "pixel") {
    if (args.count() != 4 || !parseNumber(args.arg(1), 0, width - 1, x) ||
        !parseNumber(args.arg(2), 0, height - 1, y))
      return "Error: matrix pixel <x> <y> <on|off>; coordinates must fit the selected rotation";
    if (args.arg(3) == "on" || args.arg(3) == "1") value = 1;
    else if (args.arg(3) != "off" && args.arg(3) != "0")
      return "Error: pixel state must be on or off";
  } else if (op == "size") {
    if (args.count() != 2 || (args.arg(1) != "8x8" && args.arg(1) != "16x8"))
      return "Usage: matrix size <8x8|16x8>";
    value = args.arg(1) == "8x8";
  } else if (op == "brightness" || op == "rotation" || op == "blink" || op == "panel") {
    const int maxValue = op == "brightness" ? 15 : (op == "panel" ? 1 : 3);
    if (args.count() != 2 || !parseNumber(args.arg(1), 0, maxValue, value))
      return "Error: brightness 0..15; rotation/blink 0..3; panel 0..1";
  } else if (op == "text") {
    if (args.count() != 2 || args.arg(1).length() > 64)
      return "Error: matrix text <text up to 64 bytes>; quote text containing spaces";
  } else {
    return "Usage: matrix <status|clear|fill|on|off|test|pixel x y on/off|text text|brightness 0..15|rotation 0..3|blink 0..3|size 8x8/16x8|panel 0/1>";
  }

  const uint8_t bus = matrixDeviceBus();
  const uint8_t address = matrixDeviceAddress();
  if ((op == "brightness" || op == "rotation" || op == "size" || op == "panel") &&
      !currentExecIsAdmin()) return "Error: admin required to change persistent matrix settings";
  if (!i2c() || !i2c()->getWire(bus)) return "Error: matrix I2C bus is unavailable";
  if (status) {
    connected = i2cPingAddress(address, 100000, 200, bus);
    if (!ensureDebugBuffer()) return "Error: debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024,
             "HT16K33: %s, I2C%u address=0x%02X, %dx%d, brightness=%d, display=%s, blink=%u, panel=%d. Bus/address settings require reboot.",
             connected ? "responding" : "not responding", bus + 1, address,
             width, height, gSettings.matrixBrightness, displayOn ? "on" : "off", blink, gSettings.matrixPanel);
    return getDebugBuffer();
  }

  // Framebuffer edits and flush share the bus lock, including GFX state.
  // Save/restore the desired frame if a transaction fails partway through.
  TwoWire* wirePtr = i2c()->getWire(bus);
  const bool ok = i2cDeviceTransaction(bus, address, 100000, 200, [&]() -> bool {
    TwoWire& wire = *wirePtr;
    uint16_t previous[8];
    memcpy(previous, matrix.displaybuffer, sizeof(previous));
    const int previousRotation = matrix.getRotation();
    const bool previousSquare = matrix.square;
    const int previousPanel = matrix.panel;
    const bool square = op == "size" ? value : gSettings.matrixSquare;
    const int panel = op == "panel" ? value : gSettings.matrixPanel;
    // A geometry change clears the old picture, including the unused square.
    const int newRotation = op == "rotation" ? value : rotation;
    if (square != matrix.square || panel != matrix.panel || newRotation != matrix.getRotation()) matrix.clear();
    matrix.configure(newRotation, square, panel);
    if (square) {
      const uint16_t mask = panel == 0 ? 0x00ff : 0xff00;
      for (auto& row : matrix.displaybuffer) row &= mask;
    }
    if (op == "clear" || op == "text" || op == "test") matrix.clear();
    if (op == "fill") matrix.fillScreen(LED_ON);
    if (op == "pixel") matrix.drawPixel(x, y, value);
    if (op == "text") {
      matrix.setTextWrap(false);
      matrix.setTextSize(1);
      matrix.setTextColor(LED_ON);
      matrix.setCursor(0, 0);
      matrix.print(args.arg(1));
    }
    if (op == "test") {
      // Asymmetric border/corner pattern makes orientation visible.
      matrix.drawRect(0, 0, matrix.width(), matrix.height(), LED_ON);
      matrix.fillRect(2, 2, 3, 3, LED_ON);
      matrix.drawPixel(matrix.width() - 3, matrix.height() - 3, LED_ON);
    }
    const int brightness = op == "brightness" ? value : gSettings.matrixBrightness;
    const uint8_t newBlink = op == "blink" ? value : blink;
    const bool on = op == "off" ? false :
        ((op == "brightness" || op == "rotation" || op == "blink") ? displayOn : true);
    if (flush(wire, on, brightness, newBlink)) return true;
    memcpy(matrix.displaybuffer, previous, sizeof(previous));
    matrix.configure(previousRotation, previousSquare, previousPanel);
    return false;
  });
  connected = ok;
  if (!ok) return "Error: matrix write failed; check wiring, address, and bus availability (hardware may contain a partial frame)";
  if (op == "brightness") setSetting(gSettings.matrixBrightness, value);
  if (op == "rotation") setSetting(gSettings.matrixRotation, value);
  if (op == "size") setSetting(gSettings.matrixSquare, value != 0);
  if (op == "panel") setSetting(gSettings.matrixPanel, value);
  return "[Matrix] OK";
}

const char* cmdMatrixBus(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.matrixBus, args, "matrixBus");
}

const char* cmdMatrixAddress(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = args;
  text.trim();
  if (text.isEmpty()) return handleSettingCommand(&settings[1], text);
  int address;
  if (!parseNumber(text, 0x70, 0x77, address)) return "Error: matrixaddress <0x70..0x77>";
  setSetting(gSettings.matrixAddress, address);
  return "[Matrix] Address saved; match the solder jumpers and reboot";
}
} // namespace

uint8_t matrixDeviceAddress() {
  static const uint8_t address = gSettings.matrixAddress >= 0x70 && gSettings.matrixAddress <= 0x77
      ? gSettings.matrixAddress : 0x70;
  return address;
}

uint8_t matrixDeviceBus() {
  static const uint8_t bus = gSettings.matrixBus == 1 ? 1 : 0;
  return bus;
}

extern const SettingsModule matrixSettingsModule = {
  "matrix", "hardware.matrix", settings, sizeof(settings) / sizeof(settings[0]),
  isConnected, "HT16K33 monochrome LED matrix"
};

const CommandEntry matrixCommands[] = {
  { "matrix", "HT16K33 LED matrix: status, clear, fill, on/off, pixel, text, brightness, rotation, blink, test", false, cmdMatrix },
  { "matrixbus", "Select matrix I2C bus (reboot required)", true, cmdMatrixBus, "Usage: matrixbus <0|1>" },
  { "matrixaddress", "Select matrix address (reboot required)", true, cmdMatrixAddress, "Usage: matrixaddress <0x70..0x77>" },
};
const size_t matrixCommandsCount = sizeof(matrixCommands) / sizeof(matrixCommands[0]);
#endif
