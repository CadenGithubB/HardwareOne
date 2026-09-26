// =============================================
// SECTION 11: RENDERING - CORE
// =============================================

function updateViewCamera() {
  var targetX = pos.x - canvas.width / 2;
  var targetY = pos.y - canvas.height / 2;
  var dx = targetX - viewCam.x;
  var dy = targetY - viewCam.y;
  viewCam.x += dx * 0.1;
  viewCam.y += dy * 0.1;
}

function shadeWall(col, shade, dist) {
  var r = parseInt(col.substr(1, 2), 16);
  var g = parseInt(col.substr(3, 2), 16);
  var b = parseInt(col.substr(5, 2), 16);
  var fog = Math.max(0.4, 1.0 - 0.0009 * dist);
  r = Math.floor(clamp(r * shade * fog, 0, 255));
  g = Math.floor(clamp(g * shade * fog, 0, 255));
  b = Math.floor(clamp(b * shade * fog, 0, 255));
  return rgbQ(r, g, b);
}

function drawSimpleWallSlice(x, y, height, wallX, shade, dist, side) {
  if (height <= 0) return;
  var baseColor = (BIOME_PALETTE[terrain] || BIOME_PALETTE.ground).wallBaseRGB;
  var u = Math.floor(wallX * 4) % 16;
  var colorMod = 1.0;
  if (terrain === 'cave') {
    var v = Math.floor(wallX * 2) % 8;
    if (v < 1) colorMod = 0.75;
    else if (v < 2) colorMod = 0.85;
    else if (v > 6) colorMod = 1.15;
    else if (v === 4) colorMod = 0.95;
    else colorMod = 1.0;
  } else {
    if (u < 2) colorMod = 0.9;
    else if (u > 13) colorMod = 0.9;
    else if (u === 7 || u === 8) colorMod = 0.95;
  }
  var fog = Math.max(0.3, 1.0 - dist * 0.0008);
  var r = Math.floor(baseColor[0] * colorMod * shade * fog);
  var g = Math.floor(baseColor[1] * colorMod * shade * fog);
  var b = Math.floor(baseColor[2] * colorMod * shade * fog);
  r = Math.max(0, Math.min(255, r));
  g = Math.max(0, Math.min(255, g));
  b = Math.max(0, Math.min(255, b));
  var gradient = ctx.createLinearGradient(0, y, 0, y + height);
  var topShade = 1.12, bottomShade = 0.88;
  gradient.addColorStop(0, rgbQ(Math.floor(r * topShade), Math.floor(g * topShade), Math.floor(b * topShade)));
  gradient.addColorStop(0.5, rgbQ(r, g, b));
  gradient.addColorStop(1, rgbQ(Math.floor(r * bottomShade), Math.floor(g * bottomShade), Math.floor(b * bottomShade)));
  ctx.fillStyle = gradient;
  ctx.fillRect(x, y, 1, height);
}

var SKY_BIOME_ANCHORS = [
  {n:0.083,b:'cave'}, {n:0.250,b:'ground'}, {n:0.416,b:'plains'},
  {n:0.583,b:'forest'}, {n:0.750,b:'expanse'}, {n:0.916,b:'ice'}
];

// Fixed catalogs keep the moving sky deterministic and bounded. Stars are
// batched into two paths and clouds into two paths rather than many fills.
var SKY_STAR_CATALOG = (function() {
  var stars = [], state = 0x51f15e;
  function next() {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  }
  for (var i = 0; i < 48; i++) {
    stars.push({angle:next() * Math.PI * 2, altitude:0.12 + next() * 0.78,
      bright:next() > 0.78, phase:next()});
  }
  return stars;
}());

var SKY_CLOUD_CATALOG = [
  {angle:0.20,height:0.56,scale:0.85,band:0}, {angle:0.95,height:0.70,scale:0.66,band:1},
  {angle:1.62,height:0.46,scale:1.00,band:0}, {angle:2.25,height:0.64,scale:0.74,band:1},
  {angle:2.92,height:0.53,scale:0.92,band:0}, {angle:3.54,height:0.72,scale:0.60,band:1},
  {angle:4.18,height:0.43,scale:1.08,band:0}, {angle:4.86,height:0.61,scale:0.78,band:1},
  {angle:5.46,height:0.50,scale:0.96,band:0}, {angle:6.02,height:0.68,scale:0.68,band:1}
];
var SKY_MOUNTAIN_HEIGHTS = [0.70,0.55,0.38];
var SKY_MOUNTAIN_ATMOSPHERE = [0.42,0.27,0.12];
var SKY_MOUNTAIN_FREQUENCIES = [[1.6,4.2,8.5],[2.1,5.2,10.5],[2.7,6.8,13.5]];

var _skyPhaseScratch = {};

function skySmooth(t) {
  t = Math.max(0, Math.min(1, t));
  return t * t * (3 - 2 * t);
}
function skyLerpRGB(a, b, t) {
  return [Math.round(a[0] + (b[0] - a[0]) * t),
          Math.round(a[1] + (b[1] - a[1]) * t),
          Math.round(a[2] + (b[2] - a[2]) * t)];
}
function skyRGB(c) { return rgbQ(c[0], c[1], c[2]); }
function skyRGBA(c, a) {
  return 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + a + ')';
}
function skyAngleDelta(a, b) {
  var d = (a - b) % (Math.PI * 2);
  if (d > Math.PI) d -= Math.PI * 2;
  if (d < -Math.PI) d += Math.PI * 2;
  return d;
}

function getSkyPhase(time, enabled, out) {
  out = out || {};
  var t = enabled ? ((time % 1) + 1) % 1 : 0.5;
  var daylight = 0, warm = 0, stars = 1;
  if (!enabled || (t >= 0.30 && t <= 0.70)) {
    daylight = 1; stars = 0;
  } else if (t >= 0.15 && t < 0.30) {
    var dawnP = skySmooth((t - 0.15) / 0.15);
    daylight = dawnP; stars = 1 - dawnP;
    warm = Math.sin(dawnP * Math.PI);
  } else if (t > 0.70 && t <= 0.85) {
    var duskP = skySmooth((t - 0.70) / 0.15);
    daylight = 1 - duskP; stars = duskP;
    warm = Math.sin(duskP * Math.PI);
  }
  var solarAngle = (t - 0.25) * Math.PI * 2;
  out.time = t; out.daylight = daylight; out.warm = warm; out.stars = stars;
  out.warmColor = t < 0.5 ? SKY_TIME_COLORS.dawn : SKY_TIME_COLORS.dusk;
  out.sunElevation = Math.sin(solarAngle); out.sunAzimuth = solarAngle;
  out.moonElevation = -out.sunElevation; out.moonAzimuth = solarAngle + Math.PI;
  return out;
}

function skyTimeColor(dayColor, nightColor, phase, warmAmount) {
  var result = skyLerpRGB(nightColor, dayColor, phase.daylight);
  if (phase.warm > 0.001 && warmAmount > 0) {
    result = skyLerpRGB(result, phase.warmColor, phase.warm * warmAmount);
  }
  return result;
}

function drawSkyStars(w, horizonY, halfFov, phase, exposure) {
  var alpha = phase.stars * exposure;
  if (alpha <= 0.01 || horizonY <= 0) return;
  var drift = phase.time * Math.PI * 2;
  ctx.save();
  for (var pass = 0; pass < 2; pass++) {
    var count = 0;
    ctx.beginPath();
    for (var i = 0; i < SKY_STAR_CATALOG.length; i++) {
      var star = SKY_STAR_CATALOG[i];
      if ((star.bright ? 1 : 0) !== pass) continue;
      var delta = skyAngleDelta(star.angle + drift, cam.ang);
      if (Math.abs(delta) > halfFov * 1.08) continue;
      var sx = w * 0.5 + delta / halfFov * w * 0.5;
      var sy = horizonY * (1 - star.altitude);
      var size = pass ? 1.5 : 1;
      ctx.rect(Math.floor(sx), Math.floor(sy), size, size);
      count++;
    }
    if (count) {
      ctx.globalAlpha = alpha * (pass ? 0.95 : 0.55);
      ctx.fillStyle = skyRGB(SKY_TIME_COLORS.star);
      ctx.fill();
    }
  }
  ctx.restore();
}

function drawSkyCelestial(w, h, horizonY, halfFov, phase, exposure, skyMid) {
  if (exposure <= 0.01 || horizonY <= 0) return;
  var sunDelta = skyAngleDelta(phase.sunAzimuth, cam.ang);
  var sunX = w * 0.5 + sunDelta / halfFov * w * 0.5;
  var sunY = horizonY - phase.sunElevation * horizonY * 0.72;
  var radius = Math.max(3, h * 0.025);
  if (Math.abs(sunDelta) < halfFov * 1.35 && phase.sunElevation > -0.10) {
    if (Math.abs(phase.sunElevation) < 0.28) {
      var glowRadius = Math.max(35, w * 0.28);
      var glow = ctx.createRadialGradient(sunX, horizonY, 0, sunX, horizonY, glowRadius);
      glow.addColorStop(0, skyRGBA(phase.warmColor, 0.52));
      glow.addColorStop(1, skyRGBA(phase.warmColor, 0));
      ctx.globalAlpha = exposure * (1 - Math.abs(phase.sunElevation) / 0.28);
      ctx.fillStyle = glow;
      ctx.fillRect(0, 0, w, Math.max(0, horizonY));
    }
    ctx.globalAlpha = exposure * 0.18;
    ctx.fillStyle = skyRGB(SKY_TIME_COLORS.sunEdge);
    ctx.beginPath(); ctx.arc(sunX, sunY, radius * 2.5, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = exposure * 0.96;
    ctx.fillStyle = skyRGB(SKY_TIME_COLORS.sunCore);
    ctx.beginPath(); ctx.arc(sunX, sunY, radius, 0, Math.PI * 2); ctx.fill();
  }

  var moonDelta = skyAngleDelta(phase.moonAzimuth, cam.ang);
  if (phase.stars > 0.08 && Math.abs(moonDelta) < halfFov * 1.2 &&
      phase.moonElevation > -0.08) {
    var moonX = w * 0.5 + moonDelta / halfFov * w * 0.5;
    var moonY = horizonY - phase.moonElevation * horizonY * 0.68;
    ctx.globalAlpha = exposure * phase.stars * 0.92;
    ctx.fillStyle = skyRGB(SKY_TIME_COLORS.moon);
    ctx.beginPath(); ctx.arc(moonX, moonY, radius * 0.82, 0, Math.PI * 2); ctx.fill();
    ctx.globalAlpha = exposure * phase.stars * 0.20;
    ctx.fillStyle = skyRGB(skyMid);
    ctx.beginPath(); ctx.arc(moonX - radius * 0.18, moonY - radius * 0.12,
      radius * 0.19, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.arc(moonX + radius * 0.22, moonY + radius * 0.18,
      radius * 0.12, 0, Math.PI * 2); ctx.fill();
  }
  ctx.globalAlpha = 1;
}

function drawSkyClouds(w, horizonY, halfFov, cloudColor, cloudiness, phase) {
  if (cloudiness <= 0.01 || horizonY <= 0) return;
  var drift = Date.now() * 0.0000022;
  var scaleBase = w / 360;
  for (var band = 0; band < 2; band++) {
    var count = 0;
    ctx.beginPath();
    for (var i = 0; i < SKY_CLOUD_CATALOG.length; i++) {
      var cloud = SKY_CLOUD_CATALOG[i];
      if (cloud.band !== band) continue;
      var delta = skyAngleDelta(cloud.angle + drift * (band ? 0.58 : 1), cam.ang);
      if (Math.abs(delta) > halfFov * 1.35) continue;
      var x = w * 0.5 + delta / halfFov * w * 0.5;
      var y = horizonY * (1 - cloud.height);
      var s = scaleBase * cloud.scale * (band ? 0.82 : 1.08);
      ctx.ellipse(x, y, 17 * s, 4.6 * s, 0, 0, Math.PI * 2);
      ctx.ellipse(x - 11 * s, y + 1.5 * s, 11 * s, 3.5 * s, 0, 0, Math.PI * 2);
      ctx.ellipse(x + 12 * s, y + 1.2 * s, 12 * s, 3.7 * s, 0, 0, Math.PI * 2);
      count++;
    }
    if (count) {
      ctx.globalAlpha = cloudiness * (band ? 0.10 : 0.16) *
        (0.72 + phase.daylight * 0.28);
      ctx.fillStyle = skyRGB(cloudColor);
      ctx.fill();
    }
  }
  ctx.globalAlpha = 1;
}

function skyMountainNoise(a, freq) {
  var v = a * freq, i = Math.floor(v), f = v - i;
  f = f * f * (3 - 2 * f);
  var h1 = ((i * 127 + 311) * 7919 >>> 0) % 10000 / 10000;
  var h2 = (((i + 1) * 127 + 311) * 7919 >>> 0) % 10000 / 10000;
  return h1 + (h2 - h1) * f;
}

function drawSkybox3D() {
  var w = canvas.width, h = canvas.height;
  var pitchOff = Math.floor(-(cam.pitch || 0) * projScale);
  var horizonY = Math.floor(h * 0.5) + pitchOff;
  var halfFov = cam.fov / 2;
  ctx.save();

  var wx = ENDLESS_MODE ? pos.x + (windowOriginX || 0) : pos.x;
  var wy = ENDLESS_MODE ? pos.y + (windowOriginY || 0) : pos.y;
  var noise = typeof biomeNoise === 'function' ? biomeNoise(wx, wy, 3600) : 0.3;
  var lo = SKY_BIOME_ANCHORS[0];
  var hi = SKY_BIOME_ANCHORS[SKY_BIOME_ANCHORS.length - 1];
  for (var ai = 0; ai < SKY_BIOME_ANCHORS.length - 1; ai++) {
    if (noise >= SKY_BIOME_ANCHORS[ai].n && noise <= SKY_BIOME_ANCHORS[ai + 1].n) {
      lo = SKY_BIOME_ANCHORS[ai]; hi = SKY_BIOME_ANCHORS[ai + 1]; break;
    }
  }
  if (noise < lo.n) hi = lo;
  if (noise > hi.n) lo = hi;
  var biomeT = lo === hi ? 0 : skySmooth((noise - lo.n) / (hi.n - lo.n));
  var loBiome = BIOME_PALETTE[lo.b], hiBiome = BIOME_PALETTE[hi.b];
  var loSky = SKY_ATMOSPHERE[lo.b], hiSky = SKY_ATMOSPHERE[hi.b];
  var phase = getSkyPhase(dayTime, settings.dayNight, _skyPhaseScratch);
  var dayZenith = skyLerpRGB(loSky.zenith, hiSky.zenith, biomeT);
  var dayMid = skyLerpRGB(loSky.mid, hiSky.mid, biomeT);
  var dayHorizon = skyLerpRGB(loSky.horizon, hiSky.horizon, biomeT);
  var skyTop = skyTimeColor(dayZenith, SKY_TIME_COLORS.nightZenith, phase, 0.10);
  var skyMid = skyTimeColor(dayMid, SKY_TIME_COLORS.nightMid, phase, 0.30);
  var skyHorizon = skyTimeColor(dayHorizon, SKY_TIME_COLORS.nightHorizon, phase, 0.62);
  var cloudDay = skyLerpRGB(loSky.cloud, hiSky.cloud, biomeT);
  var cloudColor = skyTimeColor(cloudDay, SKY_TIME_COLORS.nightMid, phase, 0.22);
  var cloudiness = loSky.cloudiness + (hiSky.cloudiness - loSky.cloudiness) * biomeT;
  var mountainAlpha = loBiome.mountainVisible +
    (hiBiome.mountainVisible - loBiome.mountainVisible) * biomeT;

  var gradient = ctx.createLinearGradient(0, 0, 0, Math.max(1, horizonY));
  gradient.addColorStop(0, skyRGB(skyTop));
  gradient.addColorStop(0.56, skyRGB(skyMid));
  gradient.addColorStop(0.86, skyRGB(skyLerpRGB(skyMid, skyHorizon, 0.58)));
  gradient.addColorStop(1, skyRGB(skyHorizon));
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, w, Math.max(0, horizonY));

  drawSkyStars(w, horizonY, halfFov, phase, mountainAlpha);
  drawSkyCelestial(w, h, horizonY, halfFov, phase, mountainAlpha, skyMid);
  drawSkyClouds(w, horizonY, halfFov, cloudColor, cloudiness, phase);

  // Farther mountain layers borrow progressively more horizon atmosphere.
  if (mountainAlpha > 0.01 && horizonY > 0) {
    var mountainColors = [];
    for (var mi = 0; mi < 3; mi++) {
      var rawMountain = skyLerpRGB(loBiome.mountain[mi], hiBiome.mountain[mi], biomeT);
      var dayMountain = skyLerpRGB(rawMountain, dayHorizon, SKY_MOUNTAIN_ATMOSPHERE[mi]);
      mountainColors.push(skyTimeColor(dayMountain,
        SKY_TIME_COLORS.nightHorizon, phase, 0.16));
    }
    var rawFoot = skyLerpRGB(loBiome.foothills, hiBiome.foothills, biomeT);
    var footColor = skyTimeColor(skyLerpRGB(rawFoot, dayHorizon, 0.08),
      SKY_TIME_COLORS.nightZenith, phase, 0.10);
    var rawHaze = skyLerpRGB(loBiome.haze, hiBiome.haze, biomeT);
    var hazeColor = skyTimeColor(skyLerpRGB(rawHaze, dayHorizon, 0.52),
      SKY_TIME_COLORS.nightHorizon, phase, 0.28);
    var step = Math.max(2, Math.floor(w / 120));
    var belowExtend = Math.min(h * 0.25, 60);
    ctx.globalAlpha = mountainAlpha;
    for (var li = 0; li < 3; li++) {
      var maxH = horizonY * SKY_MOUNTAIN_HEIGHTS[li];
      ctx.fillStyle = skyRGB(mountainColors[li]);
      ctx.beginPath(); ctx.moveTo(0, horizonY + belowExtend);
      for (var sx2 = 0; sx2 <= w; sx2 += step) {
        var screenT = (sx2 / w - 0.5) * 2;
        var worldAng = cam.ang + screenT * halfFov;
        var n = skyMountainNoise(worldAng, SKY_MOUNTAIN_FREQUENCIES[li][0]) * 0.55
          + skyMountainNoise(worldAng, SKY_MOUNTAIN_FREQUENCIES[li][1]) * 0.30
          + skyMountainNoise(worldAng, SKY_MOUNTAIN_FREQUENCIES[li][2]) * 0.15;
        ctx.lineTo(sx2, horizonY - Math.pow(n, 0.7) * maxH);
      }
      ctx.lineTo(w, horizonY + belowExtend); ctx.closePath(); ctx.fill();
    }

    ctx.fillStyle = skyRGB(footColor);
    ctx.beginPath(); ctx.moveTo(0, horizonY + belowExtend);
    for (var sx3 = 0; sx3 <= w; sx3 += step) {
      var screenT2 = (sx3 / w - 0.5) * 2;
      var worldAng2 = cam.ang + screenT2 * halfFov;
      var nf = skyMountainNoise(worldAng2, 3.5) * 0.5
        + skyMountainNoise(worldAng2, 8) * 0.3
        + skyMountainNoise(worldAng2, 15) * 0.2;
      ctx.lineTo(sx3, horizonY - Math.pow(nf, 0.6) * horizonY * 0.18);
    }
    ctx.lineTo(w, horizonY + belowExtend); ctx.closePath(); ctx.fill();
    ctx.globalAlpha = 1;

    var hazeH = Math.min(30, horizonY * 0.15);
    var hazeGrad = ctx.createLinearGradient(0, horizonY - hazeH, 0, horizonY + hazeH);
    hazeGrad.addColorStop(0, skyRGBA(hazeColor, 0));
    hazeGrad.addColorStop(0.4, skyRGBA(hazeColor, 0.45));
    hazeGrad.addColorStop(1, skyRGBA(hazeColor, 0));
    ctx.fillStyle = hazeGrad;
    ctx.fillRect(0, horizonY - hazeH, w, hazeH * 2);
  }

  ctx.restore();
}
function drawCalibration() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#333'; ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#fff'; ctx.font = '16px Arial'; ctx.textAlign = 'center';
  ctx.fillText('Calibrating...', canvas.width / 2, canvas.height / 2 - 20);
  ctx.fillText('Hold device flat and still', canvas.width / 2, canvas.height / 2 + 10);
  var pct = Math.min(100, Math.floor((calStableMs / calNeedStableMs) * 100));
  ctx.fillStyle = '#4db6ff';
  ctx.fillRect(canvas.width / 2 - 100, canvas.height / 2 + 30, pct * 2, 10);
  ctx.strokeStyle = '#fff';
  ctx.strokeRect(canvas.width / 2 - 100, canvas.height / 2 + 30, 200, 10);
}


// =============================================
// SECTION 12: RENDERING - WALLS (Polygon/Quad)
// =============================================

function drawWalls3D() {
  if (!grid) return;
  // Reset per-frame gradient cache (A1-3).
  _wallGradCache.clear();
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var tanHalf = 1 / invTanHalf;

  // Wall world-space height: fixed in world units (independent of resolution)
  var wallBaseH = CANVAS_BASE_H / 25;

  // Terrain-dependent wall base color (used per-face; overridden in endless mode)
  var baseR, baseG, baseB;
  if (terrain === 'ice') { baseR = 140; baseG = 170; baseB = 240; }
  else if (terrain === 'cave') { baseR = 95; baseG = 95; baseB = 105; }
  else if (terrain === 'expanse') { baseR = 90; baseG = 85; baseB = 80; }
  else { baseR = 180; baseG = 140; baseB = 100; }
  var defaultBaseR = baseR, defaultBaseG = baseG, defaultBaseB = baseB;

  // Initialize depth buffer
  if (!depthBuffer || depthBuffer.length !== w) depthBuffer = new Array(w);
  for (var di = 0; di < w; di++) depthBuffer[di] = 9999;

  // Spatial culling — only iterate wall cells within view distance
  var camGX = cam.x / cell, camGY = cam.y / cell;
  var viewRad = viewDist / cell;
  var gxMin = Math.max(0, Math.floor(camGX - viewRad));
  var gxMax = Math.min(gridW - 1, Math.ceil(camGX + viewRad));
  var gyMin = Math.max(0, Math.floor(camGY - viewRad));
  var gyMax = Math.min(gridH - 1, Math.ceil(camGY + viewRad));

  // Painter's algorithm: iterate far-to-near based on camera facing
  var iterYStart, iterYEnd, iterYStep;
  var iterXStart, iterXEnd, iterXStep;
  if (sinAng >= 0) { iterYStart = gyMax; iterYEnd = gyMin - 1; iterYStep = -1; }
  else             { iterYStart = gyMin; iterYEnd = gyMax + 1; iterYStep = 1; }
  if (cosAng >= 0) { iterXStart = gxMax; iterXEnd = gxMin - 1; iterXStep = -1; }
  else             { iterXStart = gxMin; iterXEnd = gxMax + 1; iterXStep = 1; }

  var viewDistSq = viewDist * viewDist;
  var fadeStart = viewDist * 0.82, fadeRange = viewDist - fadeStart;

  // Render a single wall face quad
  // Cave albedo is authored with the terrain, independent of camera state.

  function renderFace(wx1, wy1, wx2, wy2, floorZ, topZ, shade, perpDist, caveTopZ1, caveTopZ2, inCave, isEntrWall) {
    // Extend wall base slightly below floor to prevent gaps on slopes
    var extFloorZ = floorZ - 0.15;
    var wallPoly = projectSceneWorldPolygon([
      {x:wx1,y:wy1,z:extFloorZ*25}, {x:wx2,y:wy2,z:extFloorZ*25},
      {x:wx2,y:wy2,z:topZ*25}, {x:wx1,y:wy1,z:topZ*25}
    ], C);
    if (wallPoly.length < 3) return;

    // Distance fog + alpha fade (must match floor fade so walls don't bleed through)
    var _faceFogFloor = inCave && typeof renderCaveFogFloor !== 'undefined' ?
      renderCaveFogFloor : (typeof renderSurfaceFogFloor !== 'undefined' ? renderSurfaceFogFloor : fogFloor);
    var fog = Math.max(_faceFogFloor, 1.0 - perpDist * 0.0008);
    if (perpDist > fadeStart) {
      var wallAlpha = Math.max(0, 1.0 - (perpDist - fadeStart) / fadeRange);
      ctx.globalAlpha = wallAlpha * wallAlpha; // ease-out, same as floor
    }

    // Pick base color: cave walls use dark stone, others use terrain color
    // DEBUG_CAVE_COLORS: bright green for cave walls
    // DEBUG_POLY_TYPES: RED for cave wall, GRAY for surface wall
    var material = getCaveMaterialColorAt((wx1 + wx2) * 0.5, (wy1 + wy2) * 0.5);
    var caveWR = (material >> 16) & 255, caveWG = (material >> 8) & 255, caveWB = material & 255;
    var faceR, faceG, faceB;
    if (DEBUG_POLY_TYPES) {
      if (inCave) { faceR = 255; faceG = 30;  faceB = 30; }   // RED cave wall
      else        { faceR = 170; faceG = 170; faceB = 170; }  // GRAY surface wall
    } else {
      faceR = inCave ? (DEBUG_CAVE_COLORS ? 0 : caveWR) : baseR;
      faceG = inCave ? (DEBUG_CAVE_COLORS ? 255 : caveWG) : baseG;
      faceB = inCave ? (DEBUG_CAVE_COLORS ? 0 : caveWB) : baseB;
    }

    // Texture-like color variation using position along face
    var faceMidX = (wx1 + wx2) * 0.5, faceMidY = (wy1 + wy2) * 0.5;
    var posHash = ((Math.floor(faceMidX * 0.3) * 7919 + Math.floor(faceMidY * 0.3) * 104729) >>> 0) % 16;
    var colorMod = 1.0;
    if (inCave) {
      // Authored cave stone already contains world-stable grain.
      colorMod = 1.0;
    } else if (baseR < 110 && baseG < 110 && baseB > baseR) {
      if (posHash < 2) colorMod = 0.75;
      else if (posHash < 4) colorMod = 0.85;
      else if (posHash > 13) colorMod = 1.15;
      else if (posHash === 8) colorMod = 0.95;
    } else {
      if (posHash < 2) colorMod = 0.9;
      else if (posHash > 13) colorMod = 0.9;
      else if (posHash === 7 || posHash === 8) colorMod = 0.95;
    }

    // Point light contribution from light grid
    var lightContrib = 0;
    if (_lightGrid) {
      var lgx = Math.floor(faceMidX / _lightCellSize);
      var lgy = Math.floor(faceMidY / _lightCellSize);
      if (lgx >= 0 && lgx < _lightGridW && lgy >= 0 && lgy < _lightGridH)
        lightContrib = _lightGrid[lgy * _lightGridW + lgx];
    }
    var baseShade = inCave && typeof getCaveRenderLightAt === 'function' ?
      getCaveRenderLightAt(faceMidX, faceMidY, true) : shade;
    var caveStyled = inCave && !DEBUG_POLY_TYPES && !DEBUG_CAVE_COLORS;
    var r, g, b;
    if (caveStyled) {
      // Stable underground side lighting separates corners without borrowing
      // the exterior sun direction. Long east/west faces are slightly quieter.
      var caveOrientation = Math.abs(wx2 - wx1) > Math.abs(wy2 - wy1) ? 0.94 : 1;
      var caveLit = shadeCaveSurfaceColor(material, CAVE_SURFACE_WALL,
        baseShade * caveOrientation, lightContrib, fog);
      r = (caveLit >>> 16) & 255; g = (caveLit >>> 8) & 255; b = caveLit & 255;
    } else {
      var totalShade = Math.min(1.0, baseShade + lightContrib);
      // Warm tint near torches
      var warmR = lightContrib > 0.05 ? 1.0 + lightContrib * 0.3 : 1.0;
      var warmB = lightContrib > 0.05 ? 1.0 - lightContrib * 0.2 : 1.0;
      r = Math.max(0, Math.min(255, Math.floor(faceR * colorMod * totalShade * fog * warmR)));
      g = Math.max(0, Math.min(255, Math.floor(faceG * colorMod * totalShade * fog)));
      b = Math.max(0, Math.min(255, Math.floor(faceB * colorMod * totalShade * fog * warmB)));
    }

    // Exterior walls keep the legacy sunlight ramp. Covered stone instead has
    // a soot-dark roof join, readable middle and grounded floor contact.
    var topFactor = caveStyled ? 0.76 : 1.12;
    var upperFactor = caveStyled ? 0.94 : 1;
    var middleFactor = caveStyled ? 1.03 : 1;
    var bottomFactor = caveStyled ? 0.80 : 0.88;
    var topR = Math.min(255, Math.floor(r * topFactor));
    var topG = Math.min(255, Math.floor(g * topFactor));
    var topB = Math.min(255, Math.floor(b * topFactor));
    var upperR = Math.min(255, Math.floor(r * upperFactor));
    var upperG = Math.min(255, Math.floor(g * upperFactor));
    var upperB = Math.min(255, Math.floor(b * upperFactor));
    var middleR = Math.min(255, Math.floor(r * middleFactor));
    var middleG = Math.min(255, Math.floor(g * middleFactor));
    var middleB = Math.min(255, Math.floor(b * middleFactor));
    var botR = Math.floor(r * bottomFactor);
    var botG = Math.floor(g * bottomFactor);
    var botB = Math.floor(b * bottomFactor);

    // Use gradient for the wall face. Canvas gradients bake absolute coords,
    // so the per-frame cache key includes a y-range bucket (A1-3).
    var minSY = Infinity, maxSY = -Infinity;
    for (var wi = 0; wi < wallPoly.length; wi++) {
      minSY = Math.min(minSY, wallPoly[wi].y);
      maxSY = Math.max(maxSY, wallPoly[wi].y);
    }
    if (maxSY > minSY + 1) {
      // Bucket color to 8-step and y to 8px. The material-ramp bit keeps an
      // interior four-stop gradient from reusing an exterior three-stop one.
      var _cb = ((r >> 3) << 12) | ((g >> 3) << 6) | (b >> 3);
      var _yb = ((minSY >> 3) & 0x3fff) | (((maxSY >> 3) & 0x3fff) << 14);
      var _gkey = _cb * 536870912 + (caveStyled ? 268435456 : 0) + _yb;
      var gradient = _wallGradCache.get(_gkey);
      if (!gradient) {
        gradient = ctx.createLinearGradient(0, minSY, 0, maxSY);
        gradient.addColorStop(0, rgbQ(topR, topG, topB));
        if (caveStyled) gradient.addColorStop(0.22, rgbQ(upperR, upperG, upperB));
        gradient.addColorStop(caveStyled ? 0.62 : 0.5, rgbQ(middleR, middleG, middleB));
        gradient.addColorStop(1, rgbQ(botR, botG, botB));
        _wallGradCache.set(_gkey, gradient);
        _cacheStats.wallGrad.misses++;
        _cacheStats.wallGrad.size = _wallGradCache.size;
      } else {
        _cacheStats.wallGrad.hits++;
      }
      ctx.fillStyle = gradient;
    } else {
      ctx.fillStyle = rgbQ(r, g, b);
    }

    fillSceneDepthPolygon(wallPoly);
    ctx.globalAlpha = 1.0;

    // Close the boundary wall up to the real ceiling. Terrain and roof depth
    // decide which pixels remain visible, including views through the mouth.
    var _ct1Valid = typeof caveTopZ1 === 'number' && isFinite(caveTopZ1);
    var _ct2Valid = typeof caveTopZ2 === 'number' && isFinite(caveTopZ2);
    var _hasCaveTop = (_ct1Valid && caveTopZ1 > topZ) || (_ct2Valid && caveTopZ2 > topZ);
    if (inCave && _hasCaveTop) {
      var ctZ1 = _ct1Valid ? Math.max(caveTopZ1, topZ) : topZ;
      var ctZ2 = _ct2Valid ? Math.max(caveTopZ2, topZ) : topZ;
      var extensionPoly = projectSceneWorldPolygon([
        {x:wx1,y:wy1,z:topZ*25}, {x:wx2,y:wy2,z:topZ*25},
        {x:wx2,y:wy2,z:ctZ2*25}, {x:wx1,y:wy1,z:ctZ1*25}
      ], C);
      var rockFog = Math.max(renderCaveFogFloor, 1.0 - perpDist / viewDist * 0.8);
      var rockLight = typeof getCaveRenderLightAt === 'function' ?
        getCaveRenderLightAt(faceMidX, faceMidY, true) : ambientLight;
      if (caveStyled) {
        var closureLit = shadeCaveSurfaceColor(material, CAVE_SURFACE_CEILING,
          rockLight, lightContrib, rockFog);
        ctx.globalAlpha = 1;
        ctx.fillStyle = rgbQ((closureLit >>> 16) & 255, (closureLit >>> 8) & 255, closureLit & 255);
      } else {
        ctx.globalAlpha = rockFog;
        ctx.fillStyle = rgbQ(Math.floor(caveWR * rockLight), Math.floor(caveWG * rockLight), Math.floor(caveWB * rockLight));
      }
      fillSceneDepthPolygon(extensionPoly);
      ctx.globalAlpha = 1.0;
    }

    // Populate depth buffer for the screen columns this face covers
    var minSX = w - 1, maxSX = 0;
    for (var wi = 0; wi < wallPoly.length; wi++) {
      minSX = Math.min(minSX, Math.max(0, Math.floor(wallPoly[wi].x)));
      maxSX = Math.max(maxSX, Math.min(w - 1, Math.ceil(wallPoly[wi].x)));
    }
    for (var sx = minSX; sx <= maxSX; sx++) {
      if (perpDist < depthBuffer[sx]) depthBuffer[sx] = perpDist;
    }
  }

  // Collect all visible wall faces, then sort by distance (far-to-near) for correct occlusion
  var wallFaces = [];

  for (var gy = gyMin; gy <= gyMax; gy++) {
    for (var gx = gxMin; gx <= gxMax; gx++) {
      if (!grid[gy * gridW + gx]) continue; // Not a wall cell

      // Quick distance check
      var centerX = (gx + 0.5) * cell, centerY = (gy + 0.5) * cell;
      var ddx = centerX - cam.x, ddy = centerY - cam.y;
      if (ddx * ddx + ddy * ddy > viewDistSq) continue;

      // Quick FOV check
      var fwdDot = ddx * cosAng + ddy * sinAng;
      if (fwdDot < -cell) continue; // Behind camera
      var rgtDot = ddx * (-sinAng) + ddy * cosAng;
      if (fwdDot > 1 && Math.abs(rgtDot) / fwdDot > tanHalf * 2.5) continue;

      var wh = wallHeights ? wallHeights[gy * gridW + gx] : 1.0;
      // Floor height at wall center
      var fh = floorMesh ? getFloorHeightAt(centerX, centerY) : 0;
      var topH = fh + wallBaseH * wh;

      // The roof/cap itself occludes this wall in the shared depth field.
      // Camera height alone cannot decide visibility through a mouth.
      var _wcap = wallCapZ ? wallCapZ[gy * gridW + gx] : -Infinity;
      var _isEntrWall = _wcap > -Infinity;

      // Precomputed roof-bound clamp keeps cave walls inside their rock cover,
      // below every exterior corner. A nearby cap is not a wall-height target;
      // ordinary surface cells retain Infinity (no clamp).
      if (wallMaxTopZ) {
        var _wmax = wallMaxTopZ[gy * gridW + gx];
        if (_wmax < Infinity && topH > _wmax) topH = _wmax;
      }

      // Per-cell biome color for endless mode (smooth blending across biomes)
      var cellBR = defaultBaseR, cellBG = defaultBaseG, cellBB = defaultBaseB;
      var cellAuthored = false;
      if (ENDLESS_MODE) {
        var _wcIdx = gy * gridW + gx;
        if (wallColorR && wallColorR[_wcIdx]) {
          cellAuthored = true;
          cellBR = wallColorR[_wcIdx];
          cellBG = wallColorG[_wcIdx];
          cellBB = wallColorB[_wcIdx];
        } else {
          var worldCX = centerX + windowOriginX, worldCY = centerY + windowOriginY;
          var packed = getWallBiomeRGB(worldCX, worldCY);
          cellBR = (packed >> 16) & 0xFF;
          cellBG = (packed >> 8) & 0xFF;
          cellBB = packed & 0xFF;
        }
      }

      // Check if this wall cell is inside a deep cave region (standard mode or endless mode)
      // Material belongs to this cell, never to the player's underground flag.
      var cellCave = false;
      if (deepCaveRegions.length > 0) cellCave = !!isInDeepCave(centerX, centerY);
      if (!cellCave && gridCave) {
        cellCave = !!gridCave[gy * gridW + gx];
      }
      // Biome at this cell (for forest canopy rendering etc.)
      var cellBiome = ENDLESS_MODE ? getBiomeAt(centerX + windowOriginX, centerY + windowOriginY) : (GAME_CONFIG.terrain || 'ground');

      // World-space corners of this grid cell
      var x1 = gx * cell, y1 = gy * cell;
      var x2 = (gx + 1) * cell, y2 = (gy + 1) * cell;

      // Helper: compute cave ceiling H at a single world point. Return values
      // stay in mesh-height units; projection performs the one H→render-Z
      // conversion. The old endless branch multiplied here as well, creating
      // giant columns instead of a wall-to-ceiling closure.
      function caveCeilAt(wx, wy, wallTopH) {
        // Standard mode deep caves
        if (deepCaveRegions.length > 0) {
          var ci = isInDeepCave(wx, wy);
          if (ci && ci.ceilZ > 0) {
            var d = ci.depth || 1.0;
            var ramp = Math.min(1.0, d * d * 2.5);
            var fullCeilZ = ci.ceilZ / 25;
            return wallTopH + (fullCeilZ - wallTopH) * ramp;
          }
        }
        // Endless mode cave ceiling from layered field
        if (floorMesh && floorMesh.layerCount) {
          var _mcx = Math.floor(wx / floorMesh.gridSize);
          var _mcy = Math.floor(wy / floorMesh.gridSize);
          if (_mcx >= 0 && _mcx < floorMesh.w && _mcy >= 0 && _mcy < floorMesh.h) {
            var _mcidx = _mcy * floorMesh.w + _mcx;
            var _mclc = floorMesh.layerCount[_mcidx];
            var _ch = Infinity;
            for (var _mcli = 0; _mcli < _mclc; _mcli++) {
              var _mct = floorMesh['l' + _mcli + 'Type'][_mcidx];
              var _mcz = floorMesh['l' + _mcli + 'TopZ'][_mcidx];
              if (_mct === 2 && _mcz < _ch) _ch = _mcz;
            }
            if (_ch < Infinity) return _ch;
          }
        }
        return null;
      }

      // Check each face — collect if neighbor is empty
      // baseFh: precomputed at window assembly in wallFaceBase[idx*4+faceIdx].
      // Drops wall base to min floor along face edge so no gap is visible from below.
      // Face indices: 0=W, 1=E, 2=N, 3=S.
      var _wfbCellIdx = gy * gridW + gx;
      var _wfbBaseIdx = _wfbCellIdx * 4;
      // West face (gx-1): endpoints are (x1,y2) and (x1,y1)
      if (gx === 0 || !grid[gy * gridW + (gx - 1)]) {
        var perpDist = Math.abs((x1 - cam.x) * cosAng + (y1 + cell * 0.5 - cam.y) * sinAng);
        if (perpDist < 1) perpDist = 1;
        var ct1 = caveCeilAt(x1, y2, topH);
        var ct2 = caveCeilAt(x1, y1, topH);
        var faceInCave = cellCave || ct1 !== null || ct2 !== null;
        var baseFh = wallFaceBase ? wallFaceBase[_wfbBaseIdx] : fh;
        wallFaces.push({wx1:x1, wy1:y2, wx2:x1, wy2:y1, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeW, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, authored:cellAuthored, entrWall:_isEntrWall});
      }
      // East face (gx+1): endpoints are (x2,y1) and (x2,y2)
      if (gx === gridW - 1 || !grid[gy * gridW + (gx + 1)]) {
        var perpDist = Math.abs((x2 - cam.x) * cosAng + (y1 + cell * 0.5 - cam.y) * sinAng);
        if (perpDist < 1) perpDist = 1;
        var ct1 = caveCeilAt(x2, y1, topH);
        var ct2 = caveCeilAt(x2, y2, topH);
        var faceInCave = cellCave || ct1 !== null || ct2 !== null;
        var baseFh = wallFaceBase ? wallFaceBase[_wfbBaseIdx + 1] : fh;
        wallFaces.push({wx1:x2, wy1:y1, wx2:x2, wy2:y2, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeE, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, authored:cellAuthored, entrWall:_isEntrWall});
      }
      // North face (gy-1): endpoints are (x1,y1) and (x2,y1)
      if (gy === 0 || !grid[(gy - 1) * gridW + gx]) {
        var perpDist = Math.abs((x1 + cell * 0.5 - cam.x) * cosAng + (y1 - cam.y) * sinAng);
        if (perpDist < 1) perpDist = 1;
        var ct1 = caveCeilAt(x1, y1, topH);
        var ct2 = caveCeilAt(x2, y1, topH);
        var faceInCave = cellCave || ct1 !== null || ct2 !== null;
        var baseFh = wallFaceBase ? wallFaceBase[_wfbBaseIdx + 2] : fh;
        wallFaces.push({wx1:x1, wy1:y1, wx2:x2, wy2:y1, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeN, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, authored:cellAuthored, entrWall:_isEntrWall});
      }
      // South face (gy+1): endpoints are (x2,y2) and (x1,y2)
      if (gy === gridH - 1 || !grid[(gy + 1) * gridW + gx]) {
        var perpDist = Math.abs((x1 + cell * 0.5 - cam.x) * cosAng + (y2 - cam.y) * sinAng);
        if (perpDist < 1) perpDist = 1;
        var ct1 = caveCeilAt(x2, y2, topH);
        var ct2 = caveCeilAt(x1, y2, topH);
        var faceInCave = cellCave || ct1 !== null || ct2 !== null;
        var baseFh = wallFaceBase ? wallFaceBase[_wfbBaseIdx + 3] : fh;
        wallFaces.push({wx1:x2, wy1:y2, wx2:x1, wy2:y2, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeS, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, authored:cellAuthored, entrWall:_isEntrWall});
      }

      // Top face — only if camera is above wall top and not fully surrounded
      var hasExposed = (gx === 0 || !grid[gy * gridW + (gx - 1)]) ||
                       (gx === gridW - 1 || !grid[gy * gridW + (gx + 1)]) ||
                       (gy === 0 || !grid[(gy - 1) * gridW + gx]) ||
                       (gy === gridH - 1 || !grid[(gy + 1) * gridW + gx]);
      if (hasExposed && !cellCave && cameraZ > topH * 25) {
        var topDist = Math.sqrt(ddx * ddx + ddy * ddy);
        if (topDist > 1) {
          wallFaces.push({top:true, x1:x1, y1:y1, x2:x2, y2:y2, z:topH, dist:topDist, wh:wh, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, authored:cellAuthored});
        }
      }
    }
  }

  // Boundary walls: generate wall faces at map edges for non-wall cells,
  // replicating how the old raycaster treated out-of-bounds as solid.
  // Skip boundary walls in endless mode (no world edge)
  if (!ENDLESS_MODE) {
  var boundaryWh = 1.0;
  // West boundary (gx=0)
  for (var gy = gyMin; gy <= gyMax; gy++) {
    if (0 < gxMin || 0 > gxMax) continue;
    if (grid[gy * gridW + 0]) continue;
    var bx1 = 0, by1 = gy * cell, by2 = (gy + 1) * cell;
    var bfh = floorMesh ? getFloorHeightAt(bx1 + cell * 0.5, (by1 + by2) * 0.5) : 0;
    var bTopH = bfh + wallBaseH * boundaryWh;
    var bPerp = Math.abs((bx1 - cam.x) * cosAng + ((by1 + by2) * 0.5 - cam.y) * sinAng);
    if (bPerp < 1) bPerp = 1;
    wallFaces.push({wx1:bx1, wy1:by2, wx2:bx1, wy2:by1, fh:bfh, topH:bTopH, shade:renderSurfaceWallShadeW, dist:bPerp, ct1:0, ct2:0, br:defaultBaseR, bg:defaultBaseG, bb:defaultBaseB});
  }
  // East boundary (gx=gridW-1)
  for (var gy = gyMin; gy <= gyMax; gy++) {
    if (gridW - 1 < gxMin || gridW - 1 > gxMax) continue;
    if (grid[gy * gridW + (gridW - 1)]) continue;
    var bx2 = gridW * cell, by1 = gy * cell, by2 = (gy + 1) * cell;
    var bfh = floorMesh ? getFloorHeightAt(bx2 - cell * 0.5, (by1 + by2) * 0.5) : 0;
    var bTopH = bfh + wallBaseH * boundaryWh;
    var bPerp = Math.abs((bx2 - cam.x) * cosAng + ((by1 + by2) * 0.5 - cam.y) * sinAng);
    if (bPerp < 1) bPerp = 1;
    wallFaces.push({wx1:bx2, wy1:by1, wx2:bx2, wy2:by2, fh:bfh, topH:bTopH, shade:renderSurfaceWallShadeE, dist:bPerp, ct1:0, ct2:0, br:defaultBaseR, bg:defaultBaseG, bb:defaultBaseB});
  }
  // North boundary (gy=0)
  for (var gx = gxMin; gx <= gxMax; gx++) {
    if (0 < gyMin || 0 > gyMax) continue;
    if (grid[0 * gridW + gx]) continue;
    var bx1 = gx * cell, bx2 = (gx + 1) * cell, by1 = 0;
    var bfh = floorMesh ? getFloorHeightAt((bx1 + bx2) * 0.5, by1 + cell * 0.5) : 0;
    var bTopH = bfh + wallBaseH * boundaryWh;
    var bPerp = Math.abs(((bx1 + bx2) * 0.5 - cam.x) * cosAng + (by1 - cam.y) * sinAng);
    if (bPerp < 1) bPerp = 1;
    wallFaces.push({wx1:bx1, wy1:by1, wx2:bx2, wy2:by1, fh:bfh, topH:bTopH, shade:renderSurfaceWallShadeN, dist:bPerp, ct1:0, ct2:0, br:defaultBaseR, bg:defaultBaseG, bb:defaultBaseB});
  }
  // South boundary (gy=gridH-1)
  for (var gx = gxMin; gx <= gxMax; gx++) {
    if (gridH - 1 < gyMin || gridH - 1 > gyMax) continue;
    if (grid[(gridH - 1) * gridW + gx]) continue;
    var bx1 = gx * cell, bx2 = (gx + 1) * cell, by2 = gridH * cell;
    var bfh = floorMesh ? getFloorHeightAt((bx1 + bx2) * 0.5, by2 - cell * 0.5) : 0;
    var bTopH = bfh + wallBaseH * boundaryWh;
    var bPerp = Math.abs(((bx1 + bx2) * 0.5 - cam.x) * cosAng + (by2 - cam.y) * sinAng);
    if (bPerp < 1) bPerp = 1;
    wallFaces.push({wx1:bx2, wy1:by2, wx2:bx1, wy2:by2, fh:bfh, topH:bTopH, shade:renderSurfaceWallShadeS, dist:bPerp, ct1:0, ct2:0, br:defaultBaseR, bg:defaultBaseG, bb:defaultBaseB});
  }
  } // end !ENDLESS_MODE boundary walls

  // Sort ALL faces (side + top) far-to-near for correct painter's algorithm occlusion
  wallFaces.sort(function(a, b) { return b.dist - a.dist; });

  // Forest canopy dedup — draw one canopy per tree cell, inline with painter's order
  var _forestCanopyDrawn = {};

  // Render all faces in single sorted pass
  ctx.save();
  for (var fi = 0; fi < wallFaces.length; fi++) {
    var f = wallFaces[fi];
    if (f.top) {
      // Top face — horizontal quad at wall top height
      var topPoly = projectSceneWorldPolygon([
        {x:f.x1,y:f.y1,z:f.z*25}, {x:f.x2,y:f.y1,z:f.z*25},
        {x:f.x2,y:f.y2,z:f.z*25}, {x:f.x1,y:f.y2,z:f.z*25}
      ], C);
      if (topPoly.length < 3) continue;

      var fog = Math.max(fogFloor, 1.0 - f.dist * 0.0008);
      if (f.dist > fadeStart) {
        var tfAlpha = Math.max(0, 1.0 - (f.dist - fadeStart) / fadeRange);
        ctx.globalAlpha = tfAlpha * tfAlpha;
      } else {
        ctx.globalAlpha = 1.0;
      }
      var shade = renderSurfaceTopShade;
      // Point light on top face
      var topMidX = (f.x1 + f.x2) * 0.5, topMidY = (f.y1 + f.y2) * 0.5;
      var topLC = 0;
      if (_lightGrid) {
        var tlgx = Math.floor(topMidX / _lightCellSize);
        var tlgy = Math.floor(topMidY / _lightCellSize);
        if (tlgx >= 0 && tlgx < _lightGridW && tlgy >= 0 && tlgy < _lightGridH)
          topLC = _lightGrid[tlgy * _lightGridW + tlgx];
      }
      shade += topLC;
      var twR = topLC > 0.05 ? 1.0 + topLC * 0.3 : 1.0;
      var twB = topLC > 0.05 ? 1.0 - topLC * 0.2 : 1.0;
      var r = Math.max(0, Math.min(255, Math.floor(f.br * shade * fog * twR)));
      var g = Math.max(0, Math.min(255, Math.floor(f.bg * shade * fog)));
      var b = Math.max(0, Math.min(255, Math.floor(f.bb * shade * fog * twB)));

      var tHash = ((Math.floor((f.x1 + f.x2) * 0.15) * 7919 + Math.floor((f.y1 + f.y2) * 0.15) * 104729) >>> 0) % 8;
      if (tHash < 2) { r = Math.floor(r * 0.92); g = Math.floor(g * 0.92); b = Math.floor(b * 0.92); }

      ctx.fillStyle = rgbQ(r, g, b);
      fillSceneDepthPolygon(topPoly);

      // ── Forest: draw canopy inline (once per cell, respects painter's order) ──
      if (f.biome === 'forest' && !f.authored) {
        var tKey = Math.floor(f.x1) + ',' + Math.floor(f.y1);
        if (!_forestCanopyDrawn[tKey]) {
          _forestCanopyDrawn[tKey] = 1;
          // Project cell center at canopy height for stable position
          var cmx = (f.x1 + f.x2) * 0.5, cmy = (f.y1 + f.y2) * 0.5;
          var cz = f.z + 0.6;  // slightly above wall top
          var cFwd = (cmx - cam.x) * cosAng + (cmy - cam.y) * sinAng;
          if (cFwd > 2) {
            var cRgt = (cmx - cam.x) * (-sinAng) + (cmy - cam.y) * cosAng;
            var csx = Math.floor((cRgt / cFwd * invTanHalf * 0.5 + 0.5) * w);
            var csy = Math.floor(horizonY + ((cameraZ - cz * 25) / cFwd) * projScale);
            // Canopy radius from perspective cell width
            var cellScreenW = Math.max(8, Math.floor(cell * invTanHalf * 0.5 / cFwd * w));
            var crad = cellScreenW * 1.3;
            if (crad > 3) {
              var leafShade = shade * fog;
              var cHash = ((Math.floor(cmx * 0.2) * 7919 + Math.floor(cmy * 0.2) * 104729) >>> 0) % 100;
              var gv = 0.75 + cHash * 0.005;
              var savedA = ctx.globalAlpha;
              // Dark base
              ctx.globalAlpha = savedA * 0.9;
              ctx.fillStyle = rgbQ(
                Math.max(0, Math.min(255, Math.floor(30 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(80 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(20 * gv * leafShade)))
              );
              ctx.beginPath(); ctx.arc(csx, csy - crad * 0.2, crad, 0, Math.PI * 2); ctx.fill();
              // Light highlight
              ctx.globalAlpha = savedA * 0.6;
              ctx.fillStyle = rgbQ(
                Math.max(0, Math.min(255, Math.floor(55 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(120 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(35 * gv * leafShade)))
              );
              ctx.beginPath(); ctx.arc(csx - crad * 0.15, csy - crad * 0.45, crad * 0.6, 0, Math.PI * 2); ctx.fill();
              ctx.globalAlpha = savedA;
            }
          }
        }
      }
    } else {
      // Side face — forest walls render as brown bark trunks
      if (f.biome === 'forest' && !f.authored) {
        baseR = 75; baseG = 55; baseB = 35;  // bark brown instead of green
      } else {
        baseR = f.br; baseG = f.bg; baseB = f.bb;
      }
      renderFace(f.wx1, f.wy1, f.wx2, f.wy2, f.fh, f.topH, f.shade, f.dist,
        f.ct1, f.ct2, f.cave || false, f.entrWall || false);

      // ── Forest: draw canopy inline from side view ──
      if (f.biome === 'forest' && !f.authored && f.dist < viewDist * 0.5) {
        // Dedup by grid cell — use wall midpoint to derive cell key
        var faceMX = (f.wx1 + f.wx2) * 0.5, faceMY = (f.wy1 + f.wy2) * 0.5;
        var tKey = Math.floor(faceMX / cell) + ',' + Math.floor(faceMY / cell);
        if (!_forestCanopyDrawn[tKey]) {
          _forestCanopyDrawn[tKey] = 1;
          // Project cell center at canopy height (stable world-space position)
          var cellCX = (Math.floor(faceMX / cell) + 0.5) * cell;
          var cellCY = (Math.floor(faceMY / cell) + 0.5) * cell;
          var canopyZ = f.topH + 0.6;
          var cFwd = (cellCX - cam.x) * cosAng + (cellCY - cam.y) * sinAng;
          if (cFwd > 2) {
            var cRgt = (cellCX - cam.x) * (-sinAng) + (cellCY - cam.y) * cosAng;
            var cSX = Math.floor((cRgt / cFwd * invTanHalf * 0.5 + 0.5) * w);
            var cSY = Math.floor(horizonY + ((cameraZ - canopyZ * 25) / cFwd) * projScale);
            var cellScreenW = Math.max(8, Math.floor(cell * invTanHalf * 0.5 / cFwd * w));
            var cRadius = cellScreenW * 1.3;
            if (cRadius > 3) {
              var cFog = Math.max(fogFloor, 1.0 - f.dist * 0.0008);
              var leafShade = f.shade * cFog;
              var cHash = ((Math.floor(cellCX * 0.2) * 7919 + Math.floor(cellCY * 0.2) * 104729) >>> 0) % 100;
              var gv = 0.75 + cHash * 0.005;
              // Distance fade
              var cAlpha = 1.0;
              if (f.dist > fadeStart) {
                cAlpha = Math.max(0, 1.0 - (f.dist - fadeStart) / fadeRange);
                cAlpha *= cAlpha;
              }
              ctx.globalAlpha = cAlpha * 0.9;
              ctx.fillStyle = rgbQ(
                Math.max(0, Math.min(255, Math.floor(30 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(80 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(20 * gv * leafShade)))
              );
              ctx.beginPath(); ctx.arc(cSX, cSY - cRadius * 0.2, cRadius, 0, Math.PI * 2); ctx.fill();
              ctx.globalAlpha = cAlpha * 0.6;
              ctx.fillStyle = rgbQ(
                Math.max(0, Math.min(255, Math.floor(55 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(120 * gv * leafShade))),
                Math.max(0, Math.min(255, Math.floor(35 * gv * leafShade)))
              );
              ctx.beginPath(); ctx.arc(cSX - cRadius * 0.15, cSY - cRadius * 0.45, cRadius * 0.6, 0, Math.PI * 2); ctx.fill();
              ctx.globalAlpha = 1.0;
            }
          }
        }
      }
    }
  }

  ctx.restore();
}
