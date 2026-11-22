// The LED commands' target argument and the encoder board's light (Adafruit
// 5752 Quad Rotary Encoder, 4991 Rotary Encoder with NeoPixel): the production
// headers the firmware runs, compiled on the host. Target parsing (the
// default light, encoder, encoder:<n>, a pixel past the board's count, a bad
// target), detection by seesaw product id, the scan's identity-read rule,
// writes of changed frames only, the 30 fps cap, a skipped frame on a busy
// bus, the failure count that closes the board, and the not-connected
// replies.
#include "../../i2csensor_encoder_light.h"
#include "../../System_CommandReply.h"
#include "../../System_LedTarget.h"
#include "../../System_SeesawIdentity.h"

#include <assert.h>
#include <fstream>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; assert(condition); } while (0)

static const RGB kRed{255, 0, 0};
static const RGB kBlue{0, 0, 255};
static const RGB kOff{0, 0, 0};

// The driver's tick, minus the bus: one call per main-loop lap. `outcome`
// answers each write attempt. Returns true when the light reports the stop.
struct Bus {
  EncoderFrameResult outcome = EncoderFrameResult::Written;
  unsigned attempts = 0;
  RGB last{0, 0, 0};     // pixel 1 of the last written frame
  EncoderFrame lastFrame;
};
static bool tick(EncoderLight& light, Bus& bus, uint32_t nowMs) {
  EncoderFrame frame;
  if (!light.next(nowMs, frame)) return false;
  ++bus.attempts;
  if (bus.outcome == EncoderFrameResult::Written) { bus.last = frame.pixel[0]; bus.lastFrame = frame; }
  return light.settle(bus.outcome, frame, nowMs);
}

static EncoderLight quadLight() {
  EncoderLight light;
  light.brightnessPercent = 100;
  light.connected(encoderBoardForProduct(kSeesawProductQuadEncoder)->pixelCount);
  return light;
}

static LedTargetWord split(const char* args, bool hasOnboard, LedTarget& target, std::string& rest) {
  size_t offset = 0;
  const LedTargetWord word = ledSplitTarget(args, hasOnboard, target, offset);
  rest = std::string(args + offset);
  return word;
}

static void targetParsing() {
  LedTarget t;
  std::string rest;
  // Left out: the onboard pixel, or the encoder on a board without one. The
  // whole line is the colour/effect arguments.
  CHECK(split("red", true, t, rest) == LedTargetWord::NotATarget);
  CHECK(t.kind == LedTargetKind::Onboard && !t.named && rest == "red");
  CHECK(split("pulse red blue 2000", true, t, rest) == LedTargetWord::NotATarget);
  CHECK(t.kind == LedTargetKind::Onboard && rest == "pulse red blue 2000");
  CHECK(split("red", false, t, rest) == LedTargetWord::NotATarget);
  CHECK(t.kind == LedTargetKind::Encoder && !t.named && rest == "red");
  CHECK(split("", true, t, rest) == LedTargetWord::NotATarget && t.kind == LedTargetKind::Onboard && rest.empty());
  CHECK(split("50", true, t, rest) == LedTargetWord::NotATarget && rest == "50");  // ledbrightness 50
  // Named.
  CHECK(split("onboard red", false, t, rest) == LedTargetWord::Target);
  CHECK(t.kind == LedTargetKind::Onboard && t.named && rest == "red");
  CHECK(split("encoder green", true, t, rest) == LedTargetWord::Target);
  CHECK(t.kind == LedTargetKind::Encoder && t.named && rest == "green");
  CHECK(split("  Encoder\t pulse red", true, t, rest) == LedTargetWord::Target);  // case and spaces
  CHECK(t.kind == LedTargetKind::Encoder && rest == "pulse red");
  CHECK(split("encoder", true, t, rest) == LedTargetWord::Target && rest.empty());  // ledclear encoder
  CHECK(split("encoder:1 red", true, t, rest) == LedTargetWord::Target);
  CHECK(t.kind == LedTargetKind::EncoderPixel && t.pixel == 1 && rest == "red");
  CHECK(split("ENCODER:4", true, t, rest) == LedTargetWord::Target && t.pixel == 4);
  CHECK(split("encoder:12 off", true, t, rest) == LedTargetWord::Target && t.pixel == 12);
  // A malformed pixel number is refused, never read as a colour.
  for (const char* bad : {"encoder:", "encoder:0", "encoder:x", "encoder:-1", "encoder:1a",
                          "encoder:100", "encoder:007", "encoder: 2"}) {
    CHECK(split(bad, true, t, rest) == LedTargetWord::BadPixel);
  }
  // Not a target word: the default light gets the whole line (and its
  // colour/effect parser refuses the word).
  for (const char* other : {"encoders red", "encoderx", "onboardx", "board red", "neopixel red"}) {
    CHECK(split(other, true, t, rest) == LedTargetWord::NotATarget && rest == other);
  }
  // A pixel past the connected board's count is refused (the reply names it):
  // the quad board has 4, the 4991 one.
  LedTarget pixel5;
  CHECK(split("encoder:5 red", true, pixel5, rest) == LedTargetWord::Target);
  const uint8_t quad = encoderBoardForProduct(kSeesawProductQuadEncoder)->pixelCount;
  const uint8_t single = encoderBoardForProduct(kSeesawProductRotaryEncoder)->pixelCount;
  CHECK(quad == 4 && single == 1);
  CHECK(!ledTargetPixelFits(pixel5, quad) && !ledTargetPixelFits(pixel5, single));
  LedTarget pixel2;
  split("encoder:2", true, pixel2, rest);
  CHECK(ledTargetPixelFits(pixel2, quad) && !ledTargetPixelFits(pixel2, single));
  LedTarget pixel1;
  split("encoder:1", true, pixel1, rest);
  CHECK(ledTargetPixelFits(pixel1, quad) && ledTargetPixelFits(pixel1, single));
  LedTarget all;
  split("encoder", true, all, rest);
  CHECK(ledTargetPixelFits(all, single) && ledTargetPixelFits(all, 0));
  // The light model refuses it too, with nothing changed.
  EncoderLight light;
  light.connected(single);
  CHECK(!light.setColour(2, kRed, "red") && !light.startEffect(2, EFFECT_BLINK, kRed, kBlue, 1000, "blink", 0));
  CHECK(light.pixels[0].steady == kOff && light.pixels[1].steady == kOff);
  CHECK(light.setColour(1, kRed, "red") && light.pixels[0].steady == kRed);
}

// No colour or effect name is a target word, so the first word is never
// ambiguous: read the tables from the firmware source.
static void namesAreNotTargets() {
  std::ifstream file(HW1_NEOPIXEL_SOURCE);
  CHECK(file.good());
  std::stringstream text;
  text << file.rdbuf();
  const std::string source = text.str();
  const size_t tableStart = source.find("const ColorEntry colorTable[]");
  const size_t tableEnd = source.find("};", tableStart);
  CHECK(tableStart != std::string::npos && tableEnd != std::string::npos);
  const std::string table = source.substr(tableStart, tableEnd - tableStart);
  const std::regex entry("\\{ \"([a-z0-9]+)\",");
  unsigned colours = 0;
  LedTarget t;
  for (auto it = std::sregex_iterator(table.begin(), table.end(), entry); it != std::sregex_iterator(); ++it) {
    const std::string name = (*it)[1];
    ++colours;
    CHECK(ledParseTargetWord(name.c_str(), name.size(), t) == LedTargetWord::NotATarget);
  }
  CHECK(colours > 70);
  const size_t effects = source.find("const char* const ledEffectNames[] = {");
  CHECK(effects != std::string::npos);
  const std::string effectRow = source.substr(effects, source.find("};", effects) - effects);
  const std::regex word("\"([a-z]+)\"");
  unsigned effectCount = 0;
  for (auto it = std::sregex_iterator(effectRow.begin(), effectRow.end(), word); it != std::sregex_iterator(); ++it) {
    const std::string name = (*it)[1];
    ++effectCount;
    CHECK(ledParseTargetWord(name.c_str(), name.size(), t) == LedTargetWord::NotATarget);
  }
  CHECK(effectCount == 5);
  for (const char* special : {"off", "none"}) {
    CHECK(ledParseTargetWord(special, strlen(special), t) == LedTargetWord::NotATarget);
  }
}

static void detection() {
  // The board reports its product id in the upper half of the VERSION value.
  const uint8_t quadBytes[4] = {0x16, 0x78, 0x12, 0x34};  // 0x1678 = 5752
  CHECK(seesawVersionFromBytes(quadBytes) == 0x16781234u);
  CHECK(seesawProductFromVersion(seesawVersionFromBytes(quadBytes)) == kSeesawProductQuadEncoder);
  const uint8_t rotaryBytes[4] = {0x13, 0x7F, 0x00, 0x01};  // 0x137F = 4991
  CHECK(seesawProductFromVersion(seesawVersionFromBytes(rotaryBytes)) == kSeesawProductRotaryEncoder);
  CHECK(kSeesawProductQuadEncoder == 5752 && kSeesawProductRotaryEncoder == 4991 &&
        kSeesawProductAnoEncoder == 5740 && kSeesawProductGamepad == 5743);

  // The encoder device claims the two encoder boards, each with its pixels.
  const EncoderBoard* quad = encoderBoardForProduct(5752);
  CHECK(quad && quad->pixelCount == 4 && quad->pixelPin == 18 && quad->firstAddress == 0x49 &&
        quad->lastAddress == 0x50);
  const EncoderBoard* rotary = encoderBoardForProduct(4991);
  CHECK(rotary && rotary->pixelCount == 1 && rotary->pixelPin == 6 && rotary->firstAddress == 0x36 &&
        rotary->lastAddress == 0x3D);
  CHECK(kEncoderMaxPixels >= quad->pixelCount && kEncoderDefaultAddress == 0x49);
  // ...and never the ANO encoder, the gamepad, or a device that is not a
  // seesaw board (a MAX17048 fuel gauge at 0x36 answers the read with its own
  // register bytes, or the read fails and no product is known).
  CHECK(!encoderBoardForProduct(kSeesawProductAnoEncoder));
  CHECK(!encoderBoardForProduct(kSeesawProductGamepad));
  const uint8_t gaugeBytes[4] = {0x00, 0x12, 0x0C, 0x80};
  CHECK(!encoderBoardForProduct(seesawProductFromVersion(seesawVersionFromBytes(gaugeBytes))));
  CHECK(!encoderBoardForProduct(0));

  // The address setting: both boards' jumper spans, nothing between.
  for (int a : {0x36, 0x37, 0x3D, 0x49, 0x4A, 0x50}) CHECK(encoderAddressValid(a));
  for (int a : {0x35, 0x3E, 0x40, 0x48, 0x51, 0}) CHECK(!encoderAddressValid(a));

  // Each input driver claims only its own board.
  const uint32_t quadVersion = (uint32_t)kSeesawProductQuadEncoder << 16 | 0x0abc;
  const uint32_t anoVersion = (uint32_t)kSeesawProductAnoEncoder << 16 | 0x0abc;
  const uint32_t gamepadVersion = (uint32_t)kSeesawProductGamepad << 16 | 0x0abc;
  CHECK(seesawProductMatches(kSeesawProductAnoEncoder, anoVersion));
  CHECK(!seesawProductMatches(kSeesawProductAnoEncoder, quadVersion));
  CHECK(!seesawProductMatches(kSeesawProductGamepad, quadVersion));
  CHECK(!seesawProductMatches(kSeesawProductGamepad, anoVersion));
  CHECK(seesawProductMatches(kSeesawProductGamepad, gamepadVersion));

  // A scan names an ambiguous address by the product the board reports.
  const uint16_t rows[] = {0, kSeesawProductAnoEncoder, kSeesawProductGamepad, kSeesawProductQuadEncoder,
                           kSeesawProductRotaryEncoder};
  const size_t count = sizeof(rows) / sizeof(rows[0]);
  CHECK(seesawRowForProduct(rows, count, kSeesawProductQuadEncoder) == 3);
  CHECK(seesawRowForProduct(rows, count, kSeesawProductRotaryEncoder) == 4);
  CHECK(seesawRowForProduct(rows, count, kSeesawProductAnoEncoder) == 1);
  CHECK(seesawRowForProduct(rows, count, kSeesawProductGamepad) == 2);
  CHECK(seesawRowForProduct(rows, count, 4567) == -1);  // another seesaw product
  CHECK(seesawRowForProduct(rows, count, 0) == -1);     // never named by address alone
  CHECK(seesawRowForProduct(nullptr, count, kSeesawProductQuadEncoder) == -1);

  // The identity read happens only where compiled rows of different products
  // claim the address. One row (the ANO alone at 0x49, the fuel gauge alone at
  // 0x36): no read, no write on the bus.
  const uint16_t anoAlone[] = {kSeesawProductAnoEncoder};
  const uint16_t gaugeAlone[] = {0};
  const uint16_t anoAndEncoder[] = {kSeesawProductAnoEncoder, kSeesawProductQuadEncoder,
                                    kSeesawProductRotaryEncoder};
  const uint16_t encoderRows[] = {kSeesawProductQuadEncoder, kSeesawProductRotaryEncoder};
  const uint16_t gaugeAndEncoder[] = {0, kSeesawProductQuadEncoder, kSeesawProductRotaryEncoder};
  const uint16_t twoPlain[] = {0, 0};
  CHECK(!seesawIdentityNeeded(anoAlone, 1));
  CHECK(!seesawIdentityNeeded(gaugeAlone, 1));
  CHECK(!seesawIdentityNeeded(twoPlain, 2));
  CHECK(!seesawIdentityNeeded(nullptr, 0));
  CHECK(seesawIdentityNeeded(anoAndEncoder, 3));
  CHECK(seesawIdentityNeeded(encoderRows, 2));  // the encoder's configured address
  CHECK(seesawIdentityNeeded(gaugeAndEncoder, 3));
}

static void changedFramesOnly() {
  EncoderLight light = quadLight();
  Bus bus;
  uint32_t now = 1000;
  // After a connect the first frame is written even when it is "off".
  tick(light, bus, now);
  CHECK(bus.attempts == 1 && bus.last == kOff);
  // Nothing changed: no write, however often the loop runs.
  for (uint32_t ms = 1; ms <= 2000; ++ms) tick(light, bus, now + ms);
  CHECK(bus.attempts == 1);
  now += 2000;
  light.setColour(0, kRed, "red");
  tick(light, bus, now);
  CHECK(bus.attempts == 2 && bus.last == kRed);
  for (uint32_t ms = 1; ms <= 500; ++ms) tick(light, bus, now + ms);
  CHECK(bus.attempts == 2);
  // A colour change inside the interval waits for it, then writes once.
  now += 500;
  light.setColour(0, kBlue, "blue");
  tick(light, bus, now);
  CHECK(bus.attempts == 3 && bus.last == kBlue);
  light.setColour(0, kRed, "red");
  tick(light, bus, now + 10);
  CHECK(bus.attempts == 3);
  tick(light, bus, now + kEncoderFrameIntervalMs);
  CHECK(bus.attempts == 4 && bus.last == kRed);
  // Brightness scales the frame; a new brightness is a changed frame.
  light.brightnessPercent = 50;
  tick(light, bus, now + 2 * kEncoderFrameIntervalMs);
  CHECK(bus.attempts == 5 && bus.last == RGB({127, 0, 0}));
  CHECK(encoderScale({255, 200, 10}, 0) == kOff);
  CHECK(encoderScale({255, 200, 10}, 250) == RGB({255, 200, 10}));  // clamped to 100 %
  // A reconnect forgets what the board showed: the same colours are written again.
  light.connected(4);
  tick(light, bus, now + 2 * kEncoderFrameIntervalMs + 1);
  CHECK(bus.attempts == 6);
  // Before the first connect there are no pixels: nothing to reach.
  EncoderLight fresh;
  CHECK(!fresh.setColour(1, kRed, "red") && fresh.setColour(0, kRed, "red"));
}

static void perPixel() {
  EncoderLight light = quadLight();
  Bus bus;
  light.setColour(0, kRed, "red");
  CHECK(!tick(light, bus, 0));
  CHECK(light.setColour(3, kBlue, "blue"));
  CHECK(!tick(light, bus, 100));
  CHECK(bus.lastFrame.pixel[0] == kRed && bus.lastFrame.pixel[1] == kRed &&
        bus.lastFrame.pixel[2] == kBlue && bus.lastFrame.pixel[3] == kRed);
  CHECK(strcmp(light.pixels[2].colourName, "blue") == 0 && strcmp(light.pixels[0].colourName, "red") == 0);
  // An effect on one pixel leaves the others steady and returns to its colour.
  CHECK(light.startEffect(2, EFFECT_BLINK, kBlue, kOff, 500, "blink", 200));
  CHECK(!tick(light, bus, 200));
  CHECK(bus.lastFrame.pixel[1] == kBlue && bus.lastFrame.pixel[0] == kRed);
  CHECK(strcmp(light.pixels[1].effectNow(300), "blink") == 0 && strcmp(light.pixels[0].effectNow(300), "off") == 0);
  for (uint32_t ms = 201; ms <= 800; ++ms) tick(light, bus, ms);
  CHECK(!light.pixels[1].effectActive && bus.lastFrame.pixel[1] == kRed);
  CHECK(strcmp(light.pixels[1].effectNow(800), "off") == 0);
  // The whole board: every pixel alike again.
  CHECK(light.setColour(0, kOff, "off"));
  CHECK(!tick(light, bus, 900));
  CHECK(bus.lastFrame == EncoderFrame());
  // The one-pixel board: only its pixel is in the frame.
  EncoderLight single;
  single.brightnessPercent = 100;
  single.connected(1);
  Bus singleBus;
  CHECK(single.setColour(0, kRed, "red"));
  tick(single, singleBus, 0);
  CHECK(singleBus.lastFrame.pixel[0] == kRed && singleBus.lastFrame.pixel[1] == kOff);
}

static void effectFrameCap() {
  EncoderLight light = quadLight();
  light.setColour(0, kBlue, "blue");
  Bus bus;
  const uint32_t start = 50000;
  light.startEffect(0, EFFECT_RAINBOW, kRed, kBlue, 10000, "rainbow", start);
  // One second of a 1 ms main loop: about 30 frames, never more.
  for (uint32_t ms = 0; ms < 1000; ++ms) tick(light, bus, start + ms);
  CHECK(bus.attempts >= 28 && bus.attempts <= 31);
  // A blink holds each colour for 250 ms: only its changes are written.
  EncoderLight blink = quadLight();
  Bus blinkBus;
  blink.startEffect(0, EFFECT_BLINK, kRed, kBlue, 2000, "blink", start);
  for (uint32_t ms = 0; ms < 1000; ++ms) tick(blink, blinkBus, start + ms);
  CHECK(blinkBus.attempts == 4);  // red, off, red, off
  // The effect math is the one the board NeoPixel uses.
  CHECK(ledEffectFrame(EFFECT_BLINK, kRed, kBlue, 0, 2000) == kRed);
  CHECK(ledEffectFrame(EFFECT_BLINK, kRed, kBlue, 250, 2000) == kOff);
  CHECK(ledEffectFrame(EFFECT_FADE, kRed, kBlue, 2000, 2000) == kBlue);
  // When it ends the steady colour shows again.
  for (uint32_t ms = 1000; ms <= 2100; ++ms) tick(blink, blinkBus, start + ms);
  CHECK(!blink.anyEffect() && blinkBus.last == kOff);  // its steady colour is off
  tick(light, bus, start + 10000 + kEncoderFrameIntervalMs);
  CHECK(!light.anyEffect() && bus.last == kBlue);
}

static void busyBusSkipsTheFrame() {
  EncoderLight light = quadLight();
  light.setColour(0, kRed, "red");
  Bus bus;
  bus.outcome = EncoderFrameResult::Busy;
  uint32_t now = 9000;
  // A busy bus: one attempt per frame interval, never a retry loop, and the
  // board is not taken to show the frame.
  for (uint32_t ms = 0; ms < 330; ++ms) CHECK(!tick(light, bus, now + ms));
  CHECK(bus.attempts == 10 && !light.shownKnown && light.failures == 0);
  // The bus frees up: the next due frame is written.
  bus.outcome = EncoderFrameResult::Written;
  now += 330;
  tick(light, bus, now);
  CHECK(bus.attempts == 11 && light.shownKnown && bus.last == kRed);
  // The transaction outcome maps to the frame result the light settles.
  CHECK(encoderFrameResult(true, true, false) == EncoderFrameResult::Written);
  CHECK(encoderFrameResult(false, false, false) == EncoderFrameResult::Busy);
  CHECK(encoderFrameResult(false, true, false) == EncoderFrameResult::Failed);
  // An unplugged board: the manager skips it as degraded after its NACKs.
  CHECK(encoderFrameResult(false, false, true) == EncoderFrameResult::Failed);
  CHECK(kEncoderLockWaitMs <= 5);
}

static void failedWritesStopTheLightOnce() {
  EncoderLight light = quadLight();
  light.setColour(0, kRed, "red");
  Bus bus;
  bus.outcome = EncoderFrameResult::Failed;
  uint32_t now = 20000;
  unsigned stops = 0;
  for (unsigned frame = 0; frame < 12; ++frame) {
    if (tick(light, bus, now)) ++stops;
    now += kEncoderFrameIntervalMs;
  }
  CHECK(stops == 1 && light.failures == 12);  // reported once, at the threshold
  // A good write clears the run.
  bus.outcome = EncoderFrameResult::Written;
  tick(light, bus, now);
  CHECK(light.failures == 0 && light.shownKnown);
  bus.outcome = EncoderFrameResult::Failed;
  light.setColour(0, kBlue, "blue");
  stops = 0;
  for (unsigned frame = 0; frame < kEncoderFailuresBeforeStop; ++frame) {
    now += kEncoderFrameIntervalMs;
    if (tick(light, bus, now)) ++stops;
  }
  CHECK(stops == 1);
}

static void notConnectedReplies() {
  // Wording per CONTRIBUTING: "Error: <known reason>. <next action>."
  for (bool reconnecting : {false, true}) {
    const char* reply = encoderNotConnectedReply(reconnecting);
    CHECK(commandReplyIsFailure(reply));
    CHECK(strncmp(reply, "Error: The encoder board is not connected.", 42) == 0);
  }
  CHECK(strstr(kEncoderReplyNotConnected, "'openencoder'") != nullptr);
  CHECK(strstr(kEncoderReplyReconnecting, "retry") != nullptr);
  // A reconnect starts at most every two seconds.
  EncoderReconnect reconnect;
  CHECK(reconnect.due(100));
  CHECK(!reconnect.due(100 + kEncoderReconnectIntervalMs - 1));
  CHECK(reconnect.due(100 + kEncoderReconnectIntervalMs));
  CHECK(kEncoderReconnectIntervalMs == 2000);
}

int main() {
  targetParsing();
  namesAreNotTargets();
  detection();
  changedFramesOnly();
  perPixel();
  effectFrameCap();
  busyBusSkipsTheFrame();
  failedWritesStopTheLightOnce();
  notConnectedReplies();
  printf("LED targets and encoder light: %u checks passed\n", checks);
  return 0;
}
