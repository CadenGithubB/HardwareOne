#include <assert.h>
#include <stdio.h>

#include "../../G2_HealthGraphCore.h"

using hw1_g2_health::Series;

static void test_daily_latest_wins_over_colliding_backfill() {
  Series<8> series{};

  // Mirrors the protocol fixture: instantaneous HR is 70, while the final
  // daily bucket average is 80. Both carry the DAILY latest timestamp.
  static constexpr uint32_t kLatestTs = 0x6955D944u;
  assert(hw1_g2_health::pushSeries(&series, 70, kLatestTs, 1000000u));
  assert(hw1_g2_health::pushSeries(&series, 60, 0x6955C710u, 995340u));
  assert(!hw1_g2_health::pushSeries(&series, 80, kLatestTs, 1000001u));

  assert(series.count == 2);
  assert(series.buf[0].value == 70);
  assert(series.buf[0].ringTs == kLatestTs);
  assert(series.buf[1].value == 60);

  // Backfill changed the insertion tail to the historical value. The next
  // cache sync replays the retained live sample as ts=0 with its original
  // receive stamp; it must still be recognized rather than inflating n.
  assert(!hw1_g2_health::pushSeries(&series, 70, 0, 1000000u));
  assert(series.count == 2);
}

static void test_timestamp_scan_uses_only_active_ring_entries() {
  Series<3> series{};
  assert(hw1_g2_health::pushSeries(&series, 1, 1, 1));
  assert(hw1_g2_health::pushSeries(&series, 2, 2, 2));
  assert(hw1_g2_health::pushSeries(&series, 3, 3, 3));
  assert(hw1_g2_health::pushSeries(&series, 4, 4, 4));

  assert(!hw1_g2_health::containsRingTimestamp(&series, 1));
  assert(hw1_g2_health::containsRingTimestamp(&series, 2));
  assert(!hw1_g2_health::pushSeries(&series, 20, 2, 5));

  // clearSeries leaves backing bytes untouched; those stale timestamps must
  // not suppress samples belonging to a new peer/session.
  hw1_g2_health::clearSeries(&series);
  assert(hw1_g2_health::pushSeries(&series, 20, 2, 6));
}

static void test_unknown_timestamp_dedupe_is_unchanged() {
  Series<4> series{};
  assert(hw1_g2_health::pushSeries(&series, 50, 0, 1000));
  assert(!hw1_g2_health::pushSeries(&series, 50, 0, 5999));
  assert(hw1_g2_health::pushSeries(&series, 50, 0, 6000));
}

struct Segment {
  bool vertical;
  int a;
  int b;
  int c;
  int shade;
};

static Segment sSegments[4];
static size_t sSegmentCount = 0;

static void recordHLine(int x0, int x1, int y, int shade) {
  sSegments[sSegmentCount++] = {false, x0, x1, y, shade};
}

static void recordVLine(int x, int y0, int y1, int shade) {
  sSegments[sSegmentCount++] = {true, x, y0, y1, shade};
}

static void test_frame_has_no_right_rail() {
  sSegmentCount = 0;
  hw1_g2_health::drawOpenRightFrame(
      34, 4, 250, 122, 3, recordHLine, recordVLine);

  assert(sSegmentCount == 3);
  assert(!sSegments[0].vertical && sSegments[0].a == 34 &&
         sSegments[0].b == 283 && sSegments[0].c == 4);
  assert(!sSegments[1].vertical && sSegments[1].a == 34 &&
         sSegments[1].b == 283 && sSegments[1].c == 125);
  assert(sSegments[2].vertical && sSegments[2].a == 34 &&
         sSegments[2].b == 4 && sSegments[2].c == 125);
  assert(sSegments[2].shade == 3);
}

int main() {
  test_daily_latest_wins_over_colliding_backfill();
  test_timestamp_scan_uses_only_active_ring_entries();
  test_unknown_timestamp_dedupe_is_unchanged();
  test_frame_has_no_right_rail();
  puts("G2 health graph core tests passed");
  return 0;
}
