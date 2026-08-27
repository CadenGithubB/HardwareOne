#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../../System_MapViewportCore.h"

using hw1_map_viewport::PanRequest;

static bool near(float actual, float expected, float tolerance = 0.000001f) {
  return fabsf(actual - expected) <= tolerance;
}

static PanRequest request(int32_t x, int32_t y, float rotation = 0.0f,
                          float zoom = 2.25f) {
  return {
      x, y, 288, 144, zoom, rotation, 0.10f,
      40.0f, 42.0f, -75.0f, -72.0f,
  };
}

static void test_cardinal_steps_match_visible_axes() {
  const auto scale = hw1_map_viewport::computeScale(288, 2.25f);
  assert(scale.lonMicroPerPixel == 109);
  assert(scale.latMicroPerPixel == 83);

  float lat = 41.0f;
  float lon = -73.5f;
  assert(hw1_map_viewport::applyPan(lat, lon, request(1, 0)));
  assert(near(lat, 41.0f));
  assert(near(lon, -73.5f + 28.8f * 109.0f / 1000000.0f));

  lat = 41.0f;
  lon = -73.5f;
  assert(hw1_map_viewport::applyPan(lat, lon, request(0, -1)));
  assert(near(lat, 41.0f + 14.4f * 83.0f / 1000000.0f));
  assert(near(lon, -73.5f));
}

static void test_rotation_maps_screen_axes_back_to_geo_axes() {
  float lat = 41.0f;
  float lon = -73.5f;
  assert(hw1_map_viewport::applyPan(lat, lon, request(0, -1, 90.0f)));
  assert(near(lat, 41.0f));
  assert(near(lon, -73.5f - 14.4f * 109.0f / 1000000.0f));

  lat = 41.0f;
  lon = -73.5f;
  assert(hw1_map_viewport::applyPan(lat, lon, request(1, 0, 90.0f)));
  assert(near(lat, 41.0f + 28.8f * 83.0f / 1000000.0f));
  assert(near(lon, -73.5f));
}

static void test_zoom_scale_limits() {
  const auto oledScale = hw1_map_viewport::computeScale(128, 1000.0f);
  assert(oledScale.lonMicroPerPixel == 10);
  assert(oledScale.latMicroPerPixel == 10);

  const auto g2Scale = hw1_map_viewport::computeScale(288, 1000.0f);
  assert(g2Scale.lonMicroPerPixel == 4);
  assert(g2Scale.latMicroPerPixel == 4);

  const auto invalidZoom = hw1_map_viewport::computeScale(288, 0.0f);
  assert(invalidZoom.lonMicroPerPixel == 246);
  assert(invalidZoom.latMicroPerPixel == 188);
}

static void test_rotated_edge_clamp_uses_rendered_half_extents() {
  PanRequest pan = request(100000, 100000, 90.0f);
  float lat = 41.0f;
  float lon = -73.5f;
  assert(hw1_map_viewport::applyPan(lat, lon, pan));

  const auto scale = hw1_map_viewport::computeScale(288, 2.25f);
  const float marginLat = 288.0f * 0.5f *
                          (float)scale.latMicroPerPixel / 1000000.0f;
  const float marginLon = 144.0f * 0.5f *
                          (float)scale.lonMicroPerPixel / 1000000.0f;
  assert(near(lat, pan.maxLat + marginLat));
  assert(near(lon, pan.maxLon + marginLon));
}

static void test_accumulated_steps_equal_isolated_steps() {
  float accumulatedLat = 41.0f;
  float accumulatedLon = -73.5f;
  assert(hw1_map_viewport::applyPan(
      accumulatedLat, accumulatedLon, request(10, 0)));

  float isolatedLat = 41.0f;
  float isolatedLon = -73.5f;
  for (int i = 0; i < 10; ++i) {
    assert(hw1_map_viewport::applyPan(
        isolatedLat, isolatedLon, request(1, 0)));
  }
  // Center state is float; repeated isolated writes at longitude magnitude 73
  // round once per tap while the accumulated form rounds once.
  assert(near(accumulatedLat, isolatedLat, 0.000050f));
  assert(near(accumulatedLon, isolatedLon, 0.000050f));
}

int main() {
  test_cardinal_steps_match_visible_axes();
  test_rotation_maps_screen_axes_back_to_geo_axes();
  test_zoom_scale_limits();
  test_rotated_edge_clamp_uses_rendered_half_extents();
  test_accumulated_steps_equal_isolated_steps();
  puts("map viewport core tests passed");
  return 0;
}
