// =============================================================================
// OLED_TextFieldCore — one-row text fields: measurement, ellipsis and marquee
// =============================================================================
// Dependency-free so host tests compile the shipping policy unchanged. The
// drawing side (OLED_TextField.cpp) clips glyphs to the field rectangle; this
// header owns everything that can be decided without a display:
//
//   - a single-line display copy of a label (the shared display-text
//     sanitizer, with line breaks and tabs as one space);
//   - the classic 6x8 font cell metrics every list row uses;
//   - the ASCII "..." ellipsis for static overflowing fields, degrading to
//     fewer dots when fewer than three cells exist;
//   - the marquee timing for the selected overflowing field: show the start,
//     wait 1000 ms, move left at 20 px/s, pause 1500 ms at the end, repeat;
//   - the bounded table of animated fields, keyed by owner, text hash and
//     width so routine list rebuilds keep progress and real changes restart.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OLED_ConsoleCore.h"  // oled_console::sanitize: the shared display-text sanitizer

namespace oled_text {

constexpr int kGlyphW = 6;  // classic GFX font cell at text size 1
constexpr int kGlyphH = 8;
constexpr size_t kLabelMax = 128;  // sanitized glyphs kept for one field, NUL included

constexpr uint32_t kMarqueeStartDelayMs = 1000;
constexpr uint32_t kMarqueeEndPauseMs = 1500;
constexpr uint32_t kMarqueePxPerSecond = 20;
constexpr uint32_t kMarqueeMsPerPx = 1000 / kMarqueePxPerSecond;
// A field not drawn for this long (display asleep, another view) restarts.
constexpr uint32_t kMarqueeStaleMs = 2500;
constexpr int kMarqueeSlots = 2;  // one item's two declared lines

inline int glyphsThatFit(int widthPx) { return widthPx > 0 ? widthPx / kGlyphW : 0; }
inline int textWidthPx(size_t glyphs) { return static_cast<int>(glyphs) * kGlyphW; }

// Single-line display copy of `src` in `dst`: the shared sanitizer (escapes,
// controls and malformed UTF-8 removed, valid UTF-8 folded to ASCII), then
// every remaining line break becomes one space. Returns the glyph count;
// *cut is set when `src` held more glyphs than `cap` keeps.
inline size_t flatten(const char* src, char* dst, size_t cap, bool* cut = nullptr) {
  bool truncated = false, multiline = false;
  const size_t n = oled_console::sanitize(src, dst, cap, &truncated, &multiline);
  for (size_t i = 0; i < n; ++i) {
    if (dst[i] == '\n') dst[i] = ' ';
  }
  if (cut) *cut = truncated;
  return n;
}

// The static form of an overflowing label for `cells` glyph cells: the first
// cells-3 glyphs and "..." when four or more cells exist, otherwise as many
// dots as fit. Returns the glyphs written to `dst` (NUL-terminated).
inline size_t ellipsize(const char* text, size_t len, int cells, char* dst, size_t cap) {
  if (!dst || cap == 0) return 0;
  size_t n = 0;
  if (cells >= 4) {
    const size_t keep = static_cast<size_t>(cells - 3);
    for (; n < keep && n < len && n + 1 < cap; ++n) dst[n] = text[n];
    for (int d = 0; d < 3 && n + 1 < cap; ++d) dst[n++] = '.';
  } else {
    for (int d = 0; d < cells && n + 1 < cap; ++d) dst[n++] = '.';
  }
  dst[n] = '\0';
  return n;
}

// FNV-1a over the sanitized label: the identity of a field's text.
inline uint32_t labelHash(const char* text, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<unsigned char>(text[i]);
    h *= 16777619u;
  }
  return h;
}

struct MarqueePhase {
  int offsetPx;             // pixels the text has moved left
  uint32_t nextChangeInMs;  // when the offset changes next (or the cycle restarts)
};

// Where the marquee stands `elapsedMs` after it started, for a text that
// overflows its field by `overflowPx` (> 0). Elapsed time is reduced modulo
// the cycle, so the motion keeps its speed however frames are paced.
inline MarqueePhase marqueePhase(uint32_t elapsedMs, int overflowPx) {
  if (overflowPx <= 0) return {0, UINT32_MAX / 2};
  const uint32_t travel = static_cast<uint32_t>(overflowPx) * kMarqueeMsPerPx;
  const uint32_t period = kMarqueeStartDelayMs + travel + kMarqueeEndPauseMs;
  const uint32_t t = elapsedMs % period;
  if (t < kMarqueeStartDelayMs) return {0, kMarqueeStartDelayMs - t};
  const uint32_t moved = t - kMarqueeStartDelayMs;
  if (moved < travel) {
    return {static_cast<int>(moved / kMarqueeMsPerPx), kMarqueeMsPerPx - moved % kMarqueeMsPerPx};
  }
  return {overflowPx, period - t};
}

// A field's identity for animation continuity: the list or screen that owns
// it, what its text says, and how wide it is. The text itself is never kept.
struct MarqueeKey {
  const void* owner;
  uint32_t textHash;
  uint16_t fieldWidthPx;
  bool operator==(const MarqueeKey& o) const {
    return owner == o.owner && textHash == o.textHash && fieldWidthPx == o.fieldWidthPx;
  }
};

// The animated fields of the frame being drawn. The renderer brackets every
// frame with beginFrame()/endFrame(); each animated field claims a slot while
// drawing; the dirty poll asks needsFrame(). A field that stops being drawn
// (hidden, other view, deselected) loses its slot at endFrame, so an idle
// screen never repaints for it.
class Marquee {
 public:
  void beginFrame(uint32_t nowMs) {
    ++frame_;
    lastFrameMs_ = nowMs;
  }

  // Offset for this frame and a claim on a slot. Same key, drawn in the
  // previous frame or within the stale window: progress continues. New key:
  // the initial pause starts now.
  int offset(const MarqueeKey& key, int overflowPx, uint32_t nowMs) {
    Slot* slot = find(key);
    // Not drawn for a while (asleep, another view): start over. A modal pause
    // owns the clock instead, so a long confirmation resumes where it stood.
    if (slot && !paused_ && static_cast<uint32_t>(nowMs - slot->lastSeenMs) > kMarqueeStaleMs) {
      slot = nullptr;
    }
    if (!slot) {
      slot = victim();
      slot->active = true;
      slot->key = key;
      slot->startMs = paused_ ? pausedAtMs_ : nowMs;
    }
    // Under a modal pause the clock stands at pausedAtMs_: the deadline is kept
    // on that frozen clock and shifted with everything else on resume.
    const uint32_t clock = paused_ ? pausedAtMs_ : nowMs;
    const MarqueePhase phase = marqueePhase(clock - slot->startMs, overflowPx);
    slot->nextDueMs = clock + phase.nextChangeInMs;
    slot->claimedFrame = frame_;
    slot->lastSeenMs = nowMs;
    return phase.offsetPx;
  }

  void endFrame() {
    for (Slot& s : slots_) {
      if (s.active && s.claimedFrame != frame_) s.active = false;
    }
  }

  // A slot drawn in the last frame is due: the display should paint again.
  bool needsFrame(uint32_t nowMs) const {
    if (paused_) return false;
    for (const Slot& s : slots_) {
      if (s.active && s.claimedFrame == frame_ &&
          static_cast<int32_t>(nowMs - s.nextDueMs) >= 0) {
        return true;
      }
    }
    return false;
  }

  // A modal owns the screen: freeze every field where it stands and resume
  // from there, as the shared feedback queue does with its lifetimes.
  void setPaused(bool paused, uint32_t nowMs) {
    if (paused == paused_) return;
    if (paused) {
      pausedAtMs_ = nowMs;
    } else {
      const uint32_t held = nowMs - pausedAtMs_;
      for (Slot& s : slots_) {
        if (!s.active) continue;
        s.startMs += held;
        s.nextDueMs += held;
        s.lastSeenMs = nowMs;
      }
    }
    paused_ = paused;
  }
  bool paused() const { return paused_; }

  // Mode or identity change: nothing in flight carries over.
  void reset() {
    for (Slot& s : slots_) s = Slot{};
    paused_ = false;
  }

  int activeCount() const {
    int n = 0;
    for (const Slot& s : slots_) n += s.active ? 1 : 0;
    return n;
  }

 private:
  struct Slot {
    bool active = false;
    MarqueeKey key{};
    uint32_t startMs = 0;
    uint32_t nextDueMs = 0;
    uint32_t claimedFrame = 0;
    uint32_t lastSeenMs = 0;
  };
  Slot* find(const MarqueeKey& key) {
    for (Slot& s : slots_) {
      if (s.active && s.key == key) return &s;
    }
    return nullptr;
  }
  Slot* victim() {
    Slot* best = &slots_[0];
    for (Slot& s : slots_) {
      if (!s.active) return &s;
      if (static_cast<int32_t>(s.claimedFrame - best->claimedFrame) < 0) best = &s;
    }
    return best;
  }

  Slot slots_[kMarqueeSlots];
  uint32_t frame_ = 0;
  uint32_t lastFrameMs_ = 0;
  bool paused_ = false;
  uint32_t pausedAtMs_ = 0;
};

}  // namespace oled_text
