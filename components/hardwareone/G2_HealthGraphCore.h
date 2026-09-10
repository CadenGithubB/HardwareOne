#ifndef G2_HEALTH_GRAPH_CORE_H
#define G2_HEALTH_GRAPH_CORE_H

#include <stddef.h>
#include <stdint.h>

namespace hw1_g2_health {

struct Sample {
  uint32_t ms;
  int16_t value;
  uint32_t ringTs;
};

template <size_t Capacity>
struct Series {
  static_assert(Capacity > 0, "health series capacity must be nonzero");

  Sample buf[Capacity];
  size_t head;
  size_t count;
  uint32_t lastRingTs;
  int16_t lastValue;
  uint32_t lastMs;
  // Daily-backfill .ms values are fetch-anchored synthetics. While any remain,
  // the renderer must not describe those positions as real receive times.
  bool hasBackfill;
  uint32_t backfillAnchorMs;
};

template <size_t Capacity>
inline void clearSeries(Series<Capacity>* series) {
  if (!series) return;
  series->head = 0;
  series->count = 0;
  series->lastRingTs = 0;
  series->lastValue = 0;
  series->lastMs = 0;
  series->hasBackfill = false;
  series->backfillAnchorMs = 0;
}

// Ring timestamps identify samples across a whole live series, not only when
// two copies happen to be inserted consecutively. Scan only the logical ring:
// clearSeries deliberately leaves the backing bytes alone, so inactive slots
// may still contain timestamps belonging to an earlier peer/session.
template <size_t Capacity>
inline bool containsRingTimestamp(const Series<Capacity>* series,
                                  uint32_t ringTs) {
  if (!series || ringTs == 0 || series->count == 0) return false;
  const size_t count = series->count < Capacity ? series->count : Capacity;
  const size_t head = series->head % Capacity;
  const size_t start = (head + Capacity - count) % Capacity;
  for (size_t i = 0; i < count; ++i) {
    if (series->buf[(start + i) % Capacity].ringTs == ringTs) return true;
  }
  return false;
}

template <size_t Capacity>
inline bool containsValueAtMs(const Series<Capacity>* series, int16_t value,
                              uint32_t ms) {
  if (!series || series->count == 0) return false;
  const size_t count = series->count < Capacity ? series->count : Capacity;
  const size_t head = series->head % Capacity;
  const size_t start = (head + Capacity - count) % Capacity;
  for (size_t i = 0; i < count; ++i) {
    const Sample& sample = series->buf[(start + i) % Capacity];
    if (sample.value == value && sample.ms == ms) return true;
  }
  return false;
}

template <size_t Capacity>
inline bool pushSeries(Series<Capacity>* series, int16_t value,
                       uint32_t ringTs, uint32_t ms) {
  if (!series) return false;
  if (ringTs != 0 && containsRingTimestamp(series, ringTs)) return false;
  if (ringTs == 0) {
    // syncFromTelemetry replays a cached sample with its original receive
    // stamp. A historical insert may have changed the insertion-tail fields,
    // so recognize that exact sample anywhere in the active ring as well.
    if (containsValueAtMs(series, value, ms)) return false;
    if (series->count > 0 && series->lastValue == value &&
        (int32_t)(ms - series->lastMs) < 5000) {
      return false;
    }
  }

  series->buf[series->head].ms = ms;
  series->buf[series->head].value = value;
  series->buf[series->head].ringTs = ringTs;
  series->head = (series->head + 1) % Capacity;
  if (series->count < Capacity) ++series->count;
  series->lastRingTs = ringTs;
  series->lastValue = value;
  series->lastMs = ms;
  return true;
}

template <size_t Capacity>
inline bool pushSeriesRaw(Series<Capacity>* series, int16_t value,
                          uint32_t ringTs, uint32_t ms) {
  if (!series) return false;
  series->buf[series->head].ms = ms;
  series->buf[series->head].value = value;
  series->buf[series->head].ringTs = ringTs;
  series->head = (series->head + 1) % Capacity;
  if (series->count < Capacity) ++series->count;
  series->lastRingTs = ringTs;
  series->lastValue = value;
  series->lastMs = ms;
  return true;
}

// Leave the right side open. The newest live sample is right-anchored and its
// two-pixel dot occupies the former right-rail column; a full-height rail there
// joins the endpoint into a false vertical spike on the lens.
template <typename DrawHLine, typename DrawVLine>
inline void drawOpenRightFrame(int x, int y, int w, int h, int shade,
                               DrawHLine drawHLine, DrawVLine drawVLine) {
  if (w < 1 || h < 1) return;
  drawHLine(x, x + w - 1, y, shade);
  drawHLine(x, x + w - 1, y + h - 1, shade);
  drawVLine(x, y, y + h - 1, shade);
}

}  // namespace hw1_g2_health

#endif  // G2_HEALTH_GRAPH_CORE_H
