#pragma once

// The encoder board's light: which boards the `encoder` device drives, which
// colour each pixel shows now, whether that needs a write, and what a write's
// outcome means. Kept free of Arduino and bus code so the host tests run the
// same decisions as the firmware (i2csensor_encoder.cpp owns the bus side,
// System_NeoPixel.cpp the LED commands that reach it through the `encoder`
// target).

#include <stddef.h>
#include <stdint.h>

#include "System_LedEffectCore.h"
#include "System_SeesawIdentity.h"

// The Adafruit seesaw encoder boards the device supports, told apart by the
// product id the board reports (owner decision 2026-11-21). The pixel count
// and pin come from the detected product; the address span is the board's
// A0..A2 jumpers (+1/+2/+4 on its default).
struct EncoderBoard {
  uint16_t product;
  const char* name;
  uint8_t pixelCount;
  uint8_t pixelPin;     // seesaw pin of the NeoPixel chain (GRB, 800 kHz)
  uint8_t firstAddress; // default address (no jumper)
  uint8_t lastAddress;  // every jumper closed
};

constexpr EncoderBoard kEncoderBoards[] = {
  { kSeesawProductQuadEncoder, "Quad Rotary Encoder", 4, 18, 0x49, 0x50 },
  { kSeesawProductRotaryEncoder, "Rotary Encoder with NeoPixel", 1, 6, 0x36, 0x3D },
};
constexpr uint8_t kEncoderMaxPixels = 4;
constexpr uint8_t kEncoderDefaultAddress = 0x49;  // the quad board's default

// The board a product id names, or nullptr: another seesaw product (the ANO
// encoder, the gamepad) or a device that is not a seesaw board is never
// claimed.
inline const EncoderBoard* encoderBoardForProduct(uint16_t product) {
  for (const EncoderBoard& board : kEncoderBoards) {
    if (board.product == product) return &board;
  }
  return nullptr;
}

// An address some supported board can be jumpered to.
inline bool encoderAddressValid(int address) {
  for (const EncoderBoard& board : kEncoderBoards) {
    if (address >= board.firstAddress && address <= board.lastAddress) return true;
  }
  return false;
}

// About 30 frames a second at most: one write attempt per interval.
constexpr uint32_t kEncoderFrameIntervalMs = 33;
// A frame waits this long for the bus; a busy bus skips the frame instead.
constexpr uint32_t kEncoderLockWaitMs = 2;
// Failed writes in a row before the light switches itself off (and the
// automatic restart takes over).
constexpr uint8_t kEncoderFailuresBeforeStop = 5;
// A command that finds the board not connected starts a reconnect at most
// this often (as the ANO encoder's reconnect).
constexpr uint32_t kEncoderReconnectIntervalMs = 2000;

constexpr const char* kEncoderReplyNotConnected =
    "Error: The encoder board is not connected. Check its cable and address with 'i2cscan', "
    "then run 'openencoder'.";
constexpr const char* kEncoderReplyReconnecting =
    "Error: The encoder board is not connected. Reconnecting now; retry in a few seconds.";

enum class EncoderFrameResult : uint8_t {
  Written,  // the board accepted the frame
  Busy,     // the bus lock was not free in time; nothing was sent
  Failed,   // the transfer failed
};

// A frame's write outcome from the bus transaction. The transaction returns
// false when its lock wait ran out (the frame never reached the bus: busy),
// when the transfer failed (the write ran), and when the manager skipped a
// device it marked degraded after repeated NACKs (an unplugged board): the
// last two are failures, so an unplugged light reaches the stop threshold.
inline EncoderFrameResult encoderFrameResult(bool transactionOk, bool writeRan, bool deviceDegraded) {
  if (transactionOk) return EncoderFrameResult::Written;
  return (writeRan || deviceDegraded) ? EncoderFrameResult::Failed : EncoderFrameResult::Busy;
}

// One frame: the colour of each pixel (only the board's count are written).
struct EncoderFrame {
  RGB pixel[kEncoderMaxPixels]{};
  bool operator==(const EncoderFrame& other) const {
    for (uint8_t i = 0; i < kEncoderMaxPixels; ++i)
      if (!(pixel[i] == other.pixel[i])) return false;
    return true;
  }
};

inline RGB encoderScale(RGB colour, uint8_t percent) {
  if (percent > 100) percent = 100;
  return { (uint8_t)(colour.r * percent / 100), (uint8_t)(colour.g * percent / 100),
           (uint8_t)(colour.b * percent / 100) };
}

inline void encoderCopyName(char* out, size_t size, const char* name) {
  if (size == 0) return;
  size_t i = 0;
  for (; name && name[i] && i + 1 < size; ++i) out[i] = name[i];
  out[i] = '\0';
}

// What one pixel shows: a steady colour, or an effect that runs for its
// duration and then shows the steady colour again. The names are what
// `ledstatus` reports.
struct EncoderPixelState {
  RGB steady{0, 0, 0};
  char colourName[24] = "off";
  bool effectActive = false;
  int effect = EFFECT_NONE;
  RGB effectColour1{0, 0, 0};
  RGB effectColour2{0, 0, 0};
  uint32_t effectStartMs = 0;
  uint32_t effectDurationMs = 0;
  char effectName[12] = "off";

  // The colour before brightness, ending an effect whose time is up.
  RGB target(uint32_t nowMs) {
    if (!effectActive) return steady;
    const uint32_t elapsed = nowMs - effectStartMs;
    if (elapsed >= effectDurationMs) {
      effectActive = false;
      return steady;
    }
    return ledEffectFrame(effect, effectColour1, effectColour2, elapsed, effectDurationMs);
  }
  // The effect name, or "off" once it has run out (status reads it).
  const char* effectNow(uint32_t nowMs) const {
    return effectActive && (nowMs - effectStartMs) < effectDurationMs ? effectName : "off";
  }
};

struct EncoderLight {
  EncoderPixelState pixels[kEncoderMaxPixels];
  uint8_t pixelCount = 0;       // of the connected board (0 before the first connect)
  uint8_t brightnessPercent = 50;

  EncoderFrame shown;           // the last frame the board accepted
  bool shownKnown = false;      // false until a write succeeds, and after a failure
  bool paced = false;           // an attempt was made; the next waits one interval
  uint32_t lastAttemptMs = 0;
  uint8_t failures = 0;         // failed writes in a row

  // The pixels a command reaches: every pixel (pixel 0) or one (1-based).
  // False, with nothing changed, when the pixel is past the board's count.
  template <typename F>
  bool forPixels(uint8_t pixel, F&& apply) {
    if (pixel > pixelCount) return false;
    for (uint8_t i = 0; i < pixelCount; ++i) {
      if (pixel == 0 || pixel == i + 1) apply(pixels[i]);
    }
    return true;
  }

  // A steady colour; it cancels the pixel's effect. `name` is the colour word.
  bool setColour(uint8_t pixel, RGB colour, const char* name) {
    return forPixels(pixel, [&](EncoderPixelState& p) {
      p.steady = colour;
      encoderCopyName(p.colourName, sizeof(p.colourName), name);
      p.effectActive = false;
    });
  }

  // An effect runs for its duration and then the steady colour shows again.
  bool startEffect(uint8_t pixel, int type, RGB colour1, RGB colour2, uint32_t durationMs,
                   const char* name, uint32_t nowMs) {
    return forPixels(pixel, [&](EncoderPixelState& p) {
      p.effect = type;
      p.effectColour1 = colour1;
      p.effectColour2 = colour2;
      p.effectDurationMs = durationMs;
      p.effectStartMs = nowMs;
      p.effectActive = type != EFFECT_NONE && durationMs > 0;
      encoderCopyName(p.effectName, sizeof(p.effectName), name);
    });
  }

  bool anyEffect() const {
    for (uint8_t i = 0; i < pixelCount; ++i)
      if (pixels[i].effectActive) return true;
    return false;
  }

  // After a (re)connect the board's pixels are unknown: the next frame writes.
  // The colours set before are kept for the pixels the board has.
  void connected(uint8_t count) {
    pixelCount = count > kEncoderMaxPixels ? kEncoderMaxPixels : count;
    shownKnown = false;
    failures = 0;
    paced = false;
  }

  // The frame to write now. False when the last attempt was less than one
  // interval ago, or when the frame is what the board already shows (that
  // frame slot is used up too, so an effect is computed at most 30 times a
  // second).
  bool next(uint32_t nowMs, EncoderFrame& frame) {
    if (paced && nowMs - lastAttemptMs < kEncoderFrameIntervalMs) return false;
    const bool effectRunning = anyEffect();
    frame = EncoderFrame();
    for (uint8_t i = 0; i < pixelCount; ++i) {
      frame.pixel[i] = encoderScale(pixels[i].target(nowMs), brightnessPercent);
    }
    if (shownKnown && frame == shown) {
      if (effectRunning) {
        paced = true;
        lastAttemptMs = nowMs;
      }
      return false;
    }
    return true;
  }

  // Record a write attempt. True when the failures in a row reached the stop
  // threshold (the caller switches the light off once).
  bool settle(EncoderFrameResult result, const EncoderFrame& frame, uint32_t nowMs) {
    paced = true;
    lastAttemptMs = nowMs;
    switch (result) {
      case EncoderFrameResult::Written:
        shown = frame;
        shownKnown = true;
        failures = 0;
        return false;
      case EncoderFrameResult::Busy:
        return false;  // skipped: the next interval computes a fresh frame
      case EncoderFrameResult::Failed:
        shownKnown = false;
        if (failures < 255) ++failures;
        return failures == kEncoderFailuresBeforeStop;
    }
    return false;
  }
};

// A command that needs the board and finds it not connected: whether to start
// a reconnect now (at most once per interval, and only for a board that is
// meant to be open), and the reply that says so.
struct EncoderReconnect {
  bool tried = false;
  uint32_t lastTryMs = 0;

  bool due(uint32_t nowMs) {
    if (tried && nowMs - lastTryMs < kEncoderReconnectIntervalMs) return false;
    tried = true;
    lastTryMs = nowMs;
    return true;
  }
};

inline const char* encoderNotConnectedReply(bool reconnectStarted) {
  return reconnectStarted ? kEncoderReplyReconnecting : kEncoderReplyNotConnected;
}
