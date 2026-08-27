#ifndef SYSTEM_MAP_VIEWPORT_CORE_H
#define SYSTEM_MAP_VIEWPORT_CORE_H

#include <math.h>
#include <stdint.h>

namespace hw1_map_viewport {

// These are the projection constants used by MapCore::renderMap. Keeping them
// here makes discrete pan geometry use the exact same viewport as rasterization.
static constexpr int32_t kBaseScaleLatMicroPerPixel = 188;
static constexpr int32_t kBaseScaleLonMicroPerPixel = 246;
static constexpr int32_t kReferenceViewportWidth = 128;
static constexpr int32_t kReferenceMinScaleMicroPerPixel = 10;

struct Scale {
  int32_t lonMicroPerPixel;
  int32_t latMicroPerPixel;
};

inline Scale computeScale(int32_t viewportWidth, float renderZoom) {
  if (viewportWidth < 1) viewportWidth = 1;
  if (!(renderZoom > 0.0f)) renderZoom = 1.0f;

  int32_t minScale =
      (kReferenceMinScaleMicroPerPixel * kReferenceViewportWidth) /
      viewportWidth;
  if (minScale < 1) minScale = 1;

  Scale scale = {
      (int32_t)(kBaseScaleLonMicroPerPixel / renderZoom),
      (int32_t)(kBaseScaleLatMicroPerPixel / renderZoom),
  };
  if (scale.lonMicroPerPixel < minScale) scale.lonMicroPerPixel = minScale;
  if (scale.latMicroPerPixel < minScale) scale.latMicroPerPixel = minScale;
  return scale;
}

struct PanRequest {
  int32_t stepsX;
  int32_t stepsY;
  int32_t viewportWidth;
  int32_t viewportHeight;
  float renderZoom;
  float rotationDegrees;
  float fractionPerStep;
  float minLat;
  float maxLat;
  float minLon;
  float maxLon;
};

inline bool applyPan(float& centerLat, float& centerLon,
                     const PanRequest& request) {
  if (request.stepsX == 0 && request.stepsY == 0) return false;
  if (request.viewportWidth < 1 || request.viewportHeight < 1 ||
      !(request.renderZoom > 0.0f) ||
      !(request.fractionPerStep > 0.0f) ||
      request.minLat > request.maxLat || request.minLon > request.maxLon) {
    return false;
  }

  const Scale scale = computeScale(request.viewportWidth, request.renderZoom);
  const float screenX =
      (float)request.stepsX * request.fractionPerStep *
      (float)request.viewportWidth;
  const float screenY =
      (float)request.stepsY * request.fractionPerStep *
      (float)request.viewportHeight;

  // Map rendering applies:
  //   screenX = geoX*cos - geoY*sin
  //   screenY = geoX*sin + geoY*cos
  // Invert that transform before converting pixels back to microdegrees.
  static constexpr float kPi = 3.14159265358979323846f;
  const float radians = request.rotationDegrees * kPi / 180.0f;
  const float cosR = cosf(radians);
  const float sinR = sinf(radians);
  const float geoPixelX = screenX * cosR + screenY * sinR;
  const float geoPixelY = -screenX * sinR + screenY * cosR;

  centerLon += geoPixelX * (float)scale.lonMicroPerPixel / 1000000.0f;
  centerLat -= geoPixelY * (float)scale.latMicroPerPixel / 1000000.0f;

  // Rotation expands the axis-aligned geographic footprint of the viewport.
  // Use its exact projected half-extents for the permitted edge overscroll.
  const float halfWidth = (float)request.viewportWidth * 0.5f;
  const float halfHeight = (float)request.viewportHeight * 0.5f;
  const float absCos = fabsf(cosR);
  const float absSin = fabsf(sinR);
  const float marginLon =
      (absCos * halfWidth + absSin * halfHeight) *
      (float)scale.lonMicroPerPixel / 1000000.0f;
  const float marginLat =
      (absSin * halfWidth + absCos * halfHeight) *
      (float)scale.latMicroPerPixel / 1000000.0f;

  centerLat = fmaxf(request.minLat - marginLat,
                    fminf(request.maxLat + marginLat, centerLat));
  centerLon = fmaxf(request.minLon - marginLon,
                    fminf(request.maxLon + marginLon, centerLon));
  return true;
}

}  // namespace hw1_map_viewport

#endif  // SYSTEM_MAP_VIEWPORT_CORE_H
