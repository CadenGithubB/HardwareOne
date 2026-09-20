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

function drawSkybox3D() {
  var w = canvas.width, h = canvas.height;
  var pitchOff = Math.floor(-(cam.pitch || 0) * projScale);
  var horizonY = Math.floor(h * 0.5) + pitchOff;
  ctx.save();

  // ── Biome-blended sky colors (from BIOME_PALETTE) ──────────────────
  // Build lookup tables from centralized palette — no per-biome data here
  var _bp = BIOME_PALETTE;
  var _skyPalettes = {}, _mtPalettes = {}, _footPalettes = {}, _hazePalettes = {}, _mtVisible = {};
  for (var _bk in _bp) {
    _skyPalettes[_bk]  = _bp[_bk].sky;
    _mtPalettes[_bk]   = _bp[_bk].mountain;
    _footPalettes[_bk] = _bp[_bk].foothills;
    _hazePalettes[_bk] = _bp[_bk].haze;
    _mtVisible[_bk]    = _bp[_bk].mountainVisible;
  }

  // Blend helper: interpolate two RGB arrays
  function _lerpRGB(a, b, t) {
    return [Math.round(a[0] + (b[0] - a[0]) * t),
            Math.round(a[1] + (b[1] - a[1]) * t),
            Math.round(a[2] + (b[2] - a[2]) * t)];
  }
  function _rgbStr(c) { return rgbQ(c[0], c[1], c[2]); }
  function _rgbaStr(c, a) { return 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + a + ')'; }

  // Get blended sky parameters from biome noise at player world position
  var _skyBlend = (function() {
    var wx, wy;
    if (ENDLESS_MODE) {
      wx = pos.x + (windowOriginX || 0);
      wy = pos.y + (windowOriginY || 0);
    } else {
      wx = pos.x; wy = pos.y;
    }
    var n = (typeof biomeNoise === 'function') ? biomeNoise(wx, wy, 3600) : 0.3;
    // Biome anchors at midpoint of each noise band
    var anchors = [
      {n: 0.083, b: 'cave'}, {n: 0.250, b: 'ground'}, {n: 0.416, b: 'plains'},
      {n: 0.583, b: 'forest'}, {n: 0.750, b: 'expanse'}, {n: 0.916, b: 'ice'}
    ];
    // Find surrounding anchors
    var lo = anchors[0], hi = anchors[anchors.length - 1];
    for (var ai = 0; ai < anchors.length - 1; ai++) {
      if (n >= anchors[ai].n && n <= anchors[ai + 1].n) {
        lo = anchors[ai]; hi = anchors[ai + 1]; break;
      }
    }
    if (n < anchors[0].n) { lo = anchors[0]; hi = anchors[0]; }
    if (n > anchors[anchors.length - 1].n) { lo = hi = anchors[anchors.length - 1]; }
    var t = (lo === hi) ? 0 : (n - lo.n) / (hi.n - lo.n);
    t = t * t * (3 - 2 * t); // smoothstep
    return {
      skyTop:  _lerpRGB(_skyPalettes[lo.b][0], _skyPalettes[hi.b][0], t),
      skyBot:  _lerpRGB(_skyPalettes[lo.b][1], _skyPalettes[hi.b][1], t),
      mt:      [_lerpRGB(_mtPalettes[lo.b][0], _mtPalettes[hi.b][0], t),
                _lerpRGB(_mtPalettes[lo.b][1], _mtPalettes[hi.b][1], t),
                _lerpRGB(_mtPalettes[lo.b][2], _mtPalettes[hi.b][2], t)],
      foot:    _lerpRGB(_footPalettes[lo.b], _footPalettes[hi.b], t),
      haze:    _lerpRGB(_hazePalettes[lo.b], _hazePalettes[hi.b], t),
      mtAlpha: _mtVisible[lo.b] + (_mtVisible[hi.b] - _mtVisible[lo.b]) * t
    };
  })();

  // Day/night sky tinting — blend biome sky colors with time-of-day keyframes
  if (settings.dayNight) {
    var t = dayTime;
    var skyTint, skyBrightness;
    if (t < 0.15 || t > 0.85) {
      // Night — deep dark blue
      skyTint = [0x02, 0x02, 0x0a];
      skyBrightness = 0.3;
    } else if (t < 0.25) {
      // Dawn — warm orange/purple rising
      var p = (t - 0.15) / 0.10;
      skyTint = _lerpRGB([0x02,0x02,0x0a], [0x50,0x25,0x10], p);
      skyBrightness = 0.3 + p * 1.2;
    } else if (t < 0.35) {
      // Dawn → Day transition
      var p = (t - 0.25) / 0.10;
      skyTint = _lerpRGB([0x50,0x25,0x10], [0x30,0x30,0x40], p);
      skyBrightness = 1.5 + p * 0.5;
    } else if (t < 0.65) {
      // Day — use biome colors brightened
      skyTint = [0x30, 0x30, 0x40];
      skyBrightness = 2.0;
    } else if (t < 0.75) {
      // Day → Dusk transition
      var p = (t - 0.65) / 0.10;
      skyTint = _lerpRGB([0x30,0x30,0x40], [0x60,0x20,0x08], p);
      skyBrightness = 2.0 - p * 0.5;
    } else {
      // Dusk — warm red/orange fading
      var p = (t - 0.75) / 0.10;
      skyTint = _lerpRGB([0x60,0x20,0x08], [0x02,0x02,0x0a], p);
      skyBrightness = 1.5 - p * 1.2;
    }
    // Apply tint: blend biome color toward tint, then scale by brightness
    function _tintRGB(base, tint, bright) {
      return [Math.min(255, Math.floor((base[0] * 0.4 + tint[0] * 0.6) * bright)),
              Math.min(255, Math.floor((base[1] * 0.4 + tint[1] * 0.6) * bright)),
              Math.min(255, Math.floor((base[2] * 0.4 + tint[2] * 0.6) * bright))];
    }
    _skyBlend.skyTop = _tintRGB(_skyBlend.skyTop, skyTint, skyBrightness);
    _skyBlend.skyBot = _tintRGB(_skyBlend.skyBot, skyTint, skyBrightness);
    // Tint mountains/foothills/haze too
    for (var mi = 0; mi < 3; mi++) _skyBlend.mt[mi] = _tintRGB(_skyBlend.mt[mi], skyTint, skyBrightness * 0.8);
    _skyBlend.foot = _tintRGB(_skyBlend.foot, skyTint, skyBrightness * 0.7);
    _skyBlend.haze = _tintRGB(_skyBlend.haze, skyTint, skyBrightness * 0.6);
  }

  // Sky gradient
  var gradient = ctx.createLinearGradient(0, 0, 0, horizonY);
  gradient.addColorStop(0, _rgbStr(_skyBlend.skyTop));
  gradient.addColorStop(1, _rgbStr(_skyBlend.skyBot));
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, w, Math.max(0, horizonY));

  // Mountain silhouettes — fade out for cave biome
  if (_skyBlend.mtAlpha > 0.01 && horizonY > 0) {
    var ang = cam.ang;
    function mtNoise(a, freq) {
      var v = a * freq;
      var i = Math.floor(v);
      var f = v - i;
      f = f * f * (3 - 2 * f);
      var h1 = ((i * 127 + 311) * 7919 >>> 0) % 10000 / 10000;
      var h2 = (((i + 1) * 127 + 311) * 7919 >>> 0) % 10000 / 10000;
      return h1 + (h2 - h1) * f;
    }

    var layerHeights = [0.70, 0.55, 0.38];
    var layerFreqs = [
      [1.6, 4.2, 8.5],
      [2.1, 5.2, 10.5],
      [2.7, 6.8, 13.5]
    ];

    var halfFov = cam.fov / 2;
    var step = Math.max(2, Math.floor(w / 120));
    var belowExtend = Math.min(h * 0.25, 60);

    ctx.globalAlpha = _skyBlend.mtAlpha;
    for (var li = 0; li < 3; li++) {
      var maxH = horizonY * layerHeights[li];
      ctx.fillStyle = _rgbStr(_skyBlend.mt[li]);
      ctx.beginPath();
      ctx.moveTo(0, horizonY + belowExtend);
      for (var sx = 0; sx <= w; sx += step) {
        var t = (sx / w - 0.5) * 2;
        var worldAng = ang + t * halfFov;
        var n = mtNoise(worldAng, layerFreqs[li][0]) * 0.55
              + mtNoise(worldAng, layerFreqs[li][1]) * 0.3
              + mtNoise(worldAng, layerFreqs[li][2]) * 0.15;
        n = Math.pow(n, 0.7);
        var peakY = horizonY - n * maxH;
        ctx.lineTo(sx, peakY);
      }
      ctx.lineTo(w, horizonY + belowExtend);
      ctx.closePath();
      ctx.fill();
    }

    // Foothills
    ctx.fillStyle = _rgbStr(_skyBlend.foot);
    ctx.beginPath();
    ctx.moveTo(0, horizonY + belowExtend);
    for (var sx2 = 0; sx2 <= w; sx2 += step) {
      var t2 = (sx2 / w - 0.5) * 2;
      var worldAng2 = ang + t2 * halfFov;
      var nf = mtNoise(worldAng2, 3.5) * 0.5 + mtNoise(worldAng2, 8) * 0.3 + mtNoise(worldAng2, 15) * 0.2;
      nf = Math.pow(nf, 0.6);
      var footY = horizonY - nf * horizonY * 0.18;
      ctx.lineTo(sx2, footY);
    }
    ctx.lineTo(w, horizonY + belowExtend);
    ctx.closePath();
    ctx.fill();
    ctx.globalAlpha = 1;

    // Haze band
    var hazeH = Math.min(30, horizonY * 0.15);
    var hazeGrad = ctx.createLinearGradient(0, horizonY - hazeH, 0, horizonY + hazeH);
    hazeGrad.addColorStop(0, _rgbaStr(_skyBlend.haze, 0));
    hazeGrad.addColorStop(0.4, _rgbaStr(_skyBlend.haze, 0.45));
    hazeGrad.addColorStop(1, _rgbaStr(_skyBlend.haze, 0));
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
    var totalShade = Math.min(1.0, baseShade + lightContrib);
    // Warm tint near torches
    var warmR = lightContrib > 0.05 ? 1.0 + lightContrib * 0.3 : 1.0;
    var warmB = lightContrib > 0.05 ? 1.0 - lightContrib * 0.2 : 1.0;

    var r = Math.max(0, Math.min(255, Math.floor(faceR * colorMod * totalShade * fog * warmR)));
    var g = Math.max(0, Math.min(255, Math.floor(faceG * colorMod * totalShade * fog)));
    var b = Math.max(0, Math.min(255, Math.floor(faceB * colorMod * totalShade * fog * warmB)));

    // Draw wall quad with vertical gradient (lighter at top, darker at bottom)
    var topR = Math.min(255, Math.floor(r * 1.12));
    var topG = Math.min(255, Math.floor(g * 1.12));
    var topB = Math.min(255, Math.floor(b * 1.12));
    var botR = Math.floor(r * 0.88);
    var botG = Math.floor(g * 0.88);
    var botB = Math.floor(b * 0.88);

    // Use gradient for the wall face. Canvas gradients bake absolute coords,
    // so the per-frame cache key includes a y-range bucket (A1-3).
    var minSY = Infinity, maxSY = -Infinity;
    for (var wi = 0; wi < wallPoly.length; wi++) {
      minSY = Math.min(minSY, wallPoly[wi].y);
      maxSY = Math.max(maxSY, wallPoly[wi].y);
    }
    if (maxSY > minSY + 1) {
      // Bucket color to 8-step and y to 8px. Key fits in 32 bits.
      var _cb = ((r >> 3) << 12) | ((g >> 3) << 6) | (b >> 3);
      var _yb = ((minSY >> 3) & 0x3fff) | (((maxSY >> 3) & 0x3fff) << 14);
      var _gkey = _cb * 268435456 + _yb; // 18-bit color * 2^28 + 28-bit y
      var gradient = _wallGradCache.get(_gkey);
      if (!gradient) {
        gradient = ctx.createLinearGradient(0, minSY, 0, maxSY);
        gradient.addColorStop(0, rgbQ(topR, topG, topB));
        gradient.addColorStop(0.5, rgbQ(r, g, b));
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
      ctx.globalAlpha = rockFog;
      ctx.fillStyle = rgbQ(Math.floor(caveWR * rockLight), Math.floor(caveWG * rockLight), Math.floor(caveWB * rockLight));
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

      // Top-Z clamp: precomputed per cell. Capped cells clamp to their cap;
      // mouth cells clamp to the lowest neighbor cap (surrounding ground);
      // open-surface cells have Infinity (no clamp).
      if (wallMaxTopZ) {
        var _wmax = wallMaxTopZ[gy * gridW + gx];
        if (_wmax < Infinity && topH > _wmax) topH = _wmax;
      }

      // Per-cell biome color for endless mode (smooth blending across biomes)
      var cellBR = defaultBaseR, cellBG = defaultBaseG, cellBB = defaultBaseB;
      if (ENDLESS_MODE) {
        var _wcIdx = gy * gridW + gx;
        if (wallColorR && wallColorR[_wcIdx]) {
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
        wallFaces.push({wx1:x1, wy1:y2, wx2:x1, wy2:y1, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeW, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, entrWall:_isEntrWall});
      }
      // East face (gx+1): endpoints are (x2,y1) and (x2,y2)
      if (gx === gridW - 1 || !grid[gy * gridW + (gx + 1)]) {
        var perpDist = Math.abs((x2 - cam.x) * cosAng + (y1 + cell * 0.5 - cam.y) * sinAng);
        if (perpDist < 1) perpDist = 1;
        var ct1 = caveCeilAt(x2, y1, topH);
        var ct2 = caveCeilAt(x2, y2, topH);
        var faceInCave = cellCave || ct1 !== null || ct2 !== null;
        var baseFh = wallFaceBase ? wallFaceBase[_wfbBaseIdx + 1] : fh;
        wallFaces.push({wx1:x2, wy1:y1, wx2:x2, wy2:y2, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeE, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, entrWall:_isEntrWall});
      }
      // North face (gy-1): endpoints are (x1,y1) and (x2,y1)
      if (gy === 0 || !grid[(gy - 1) * gridW + gx]) {
        var perpDist = Math.abs((x1 + cell * 0.5 - cam.x) * cosAng + (y1 - cam.y) * sinAng);
        if (perpDist < 1) perpDist = 1;
        var ct1 = caveCeilAt(x1, y1, topH);
        var ct2 = caveCeilAt(x2, y1, topH);
        var faceInCave = cellCave || ct1 !== null || ct2 !== null;
        var baseFh = wallFaceBase ? wallFaceBase[_wfbBaseIdx + 2] : fh;
        wallFaces.push({wx1:x1, wy1:y1, wx2:x2, wy2:y1, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeN, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, entrWall:_isEntrWall});
      }
      // South face (gy+1): endpoints are (x2,y2) and (x1,y2)
      if (gy === gridH - 1 || !grid[(gy + 1) * gridW + gx]) {
        var perpDist = Math.abs((x1 + cell * 0.5 - cam.x) * cosAng + (y2 - cam.y) * sinAng);
        if (perpDist < 1) perpDist = 1;
        var ct1 = caveCeilAt(x2, y2, topH);
        var ct2 = caveCeilAt(x1, y2, topH);
        var faceInCave = cellCave || ct1 !== null || ct2 !== null;
        var baseFh = wallFaceBase ? wallFaceBase[_wfbBaseIdx + 3] : fh;
        wallFaces.push({wx1:x2, wy1:y2, wx2:x1, wy2:y2, fh:baseFh, topH:topH, shade:renderSurfaceWallShadeS, dist:perpDist, ct1:ct1, ct2:ct2, cave:faceInCave, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome, entrWall:_isEntrWall});
      }

      // Top face — only if camera is above wall top and not fully surrounded
      var hasExposed = (gx === 0 || !grid[gy * gridW + (gx - 1)]) ||
                       (gx === gridW - 1 || !grid[gy * gridW + (gx + 1)]) ||
                       (gy === 0 || !grid[(gy - 1) * gridW + gx]) ||
                       (gy === gridH - 1 || !grid[(gy + 1) * gridW + gx]);
      if (hasExposed && !cellCave && cameraZ > topH * 25) {
        var topDist = Math.sqrt(ddx * ddx + ddy * ddy);
        if (topDist > 1) {
          wallFaces.push({top:true, x1:x1, y1:y1, x2:x2, y2:y2, z:topH, dist:topDist, wh:wh, br:cellBR, bg:cellBG, bb:cellBB, biome:cellBiome});
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
      if (f.biome === 'forest') {
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
      if (f.biome === 'forest') {
        baseR = 75; baseG = 55; baseB = 35;  // bark brown instead of green
      } else {
        baseR = f.br; baseG = f.bg; baseB = f.bb;
      }
      renderFace(f.wx1, f.wy1, f.wx2, f.wy2, f.fh, f.topH, f.shade, f.dist,
        f.ct1, f.ct2, f.cave || false, f.entrWall || false);

      // ── Forest: draw canopy inline from side view ──
      if (f.biome === 'forest' && f.dist < viewDist * 0.5) {
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
