// =============================================
// SECTION 5: WALL DECORATIONS
// =============================================

function findWallSurfaces() {
  var surfaces = [];
  for (var y = 0; y < gridH; y++) {
    for (var x = 0; x < gridW; x++) {
      if (grid[y * gridW + x]) {
        var hasNorth = (y > 0 && !grid[(y - 1) * gridW + x]);
        var hasSouth = (y < gridH - 1 && !grid[(y + 1) * gridW + x]);
        var hasWest = (x > 0 && !grid[y * gridW + (x - 1)]);
        var hasEast = (x < gridW - 1 && !grid[y * gridW + (x + 1)]);
        if (hasNorth) surfaces.push({x:x, y:y, side:'north', worldX:x * cell + cell / 2, worldY:y * cell});
        if (hasSouth) surfaces.push({x:x, y:y, side:'south', worldX:x * cell + cell / 2, worldY:(y + 1) * cell});
        if (hasWest) surfaces.push({x:x, y:y, side:'west', worldX:x * cell, worldY:y * cell + cell / 2});
        if (hasEast) surfaces.push({x:x, y:y, side:'east', worldX:(x + 1) * cell, worldY:y * cell + cell / 2});
      }
    }
  }
  return surfaces;
}

function createWallDecorations() {
  var surfaces = findWallSurfaces();
  wallDecorations = [];
  Math.seedrandom = function(seed) {
    var m = 0x80000000, a = 1103515245, c = 12345;
    var state = seed ? seed : Math.floor(Math.random() * (m - 1));
    return function() { state = (a * state + c) % m; return state / (m - 1); };
  };
  var rng = Math.seedrandom(seedFor(3));
  // Pick decoration type pool based on terrain — prevent dungeon items on natural rock walls
  var typePool;
  if (terrain === 'cave') {
    typePool = ['fungi', 'moss_drip', 'stalactite_tip', 'torch', 'sconce'];
  } else if (terrain === 'forest') {
    typePool = ['vine_growth', 'moss_drip', 'carved_rune', 'vine_growth', 'torch'];
  } else if (terrain === 'expanse' || terrain === 'plains') {
    typePool = ['vine_growth', 'carved_rune', 'moss_drip'];
  } else if (terrain === 'ice') {
    typePool = ['icicle', 'frost_crystal', 'torch', 'sconce'];
  } else {
    typePool = ['torch', 'shield', 'banner', 'sconce'];
  }

  var numDecorations = Math.min(8, Math.floor(surfaces.length * 0.15));
  for (var i = 0; i < numDecorations; i++) {
    if (surfaces.length === 0) break;
    var idx = Math.floor(rng() * surfaces.length);
    var surface = surfaces[idx];
    surfaces.splice(idx, 1);
    // Skip surfaces on irregular border polygon walls (natural terrain, not dungeon)
    if (currentBorderPoly && !pointInPolygon(surface.worldX, surface.worldY, currentBorderPoly)) continue;
    // Keep the cave archway opening clear of wall decorations (torches/sconces/vines).
    if (inEntranceReserve(surface.worldX, surface.worldY)) continue;
    var type = typePool[Math.floor(rng() * typePool.length)];
    var offset = (rng() - 0.5) * cell * 0.4;
    var decorX = surface.worldX;
    var decorY = surface.worldY;
    if (surface.side === 'north' || surface.side === 'south') {
      decorX += offset;
    } else {
      decorY += offset;
    }
    wallDecorations.push({
      worldX:decorX, worldY:decorY, side:surface.side,
      type:type, gridX:surface.x, gridY:surface.y
    });
  }
  console.log('[DECOR] Created ' + wallDecorations.length + ' seeded decorations on actual walls');
}

// The decoration owns an actual wall face, not merely a ground coordinate.
// Match drawWalls3D's center-based top and its per-face base, then fit the
// sprite inside the exposed span. A collapsed/missing wall cannot hold a torch.
function getWallDecorationAttachment(dec) {
  if (dec.gridX < 0 || dec.gridY < 0 || dec.gridX >= gridW || dec.gridY >= gridH) return null;
  var ci = dec.gridY * gridW + dec.gridX;
  if (!grid || !grid[ci]) return null;
  var nx = dec.side === 'west' ? -1 : dec.side === 'east' ? 1 : 0;
  var ny = dec.side === 'north' ? -1 : dec.side === 'south' ? 1 : 0;
  if (!nx && !ny) return null;
  var centerX = (dec.gridX + 0.5) * cell, centerY = (dec.gridY + 0.5) * cell;
  var centerH = floorMesh ? getFloorHeightAt(centerX, centerY) : 0;
  var topH = centerH + (CANVAS_BASE_H / 25) * (wallHeights ? wallHeights[ci] : 1);
  if (wallMaxTopZ && isFinite(wallMaxTopZ[ci])) topH = Math.min(topH, wallMaxTopZ[ci]);
  var face = dec.side === 'west' ? 0 : dec.side === 'east' ? 1 : dec.side === 'north' ? 2 : 3;
  var baseH = wallFaceBase ? wallFaceBase[ci * 4 + face] : centerH;
  var x = dec.worldX + nx * 0.5, y = dec.worldY + ny * 0.5;
  var openH = floorMesh ? getFloorHeightAt(x, y) : 0;
  baseH = Math.max(baseH, openH);
  if (floorMesh && typeof getCaveSpaceAt === 'function') {
    topH = Math.min(topH, getCaveSpaceAt(x, y, openH).ceilingH);
  }
  var span = (topH - baseH) * 25;
  if (!isFinite(span) || span < 4) return null;
  var tier = WALL_DECOR_TIER[dec.type] || 'smWall';
  var size = Math.min(32 * getScale3D(tier), span * 0.3);
  var z = baseH * 25 + span * 0.55;
  return {x:x, y:y, z:z, size:size, nx:nx, ny:ny,
    baseZ:baseH*25, topZ:topH*25,
    flameZ:z + size * (dec.type === 'sconce' ? 1/6 : 1/4)};
}

function getWallDecorationRenderZ(dec) {
  var attachment = getWallDecorationAttachment(dec);
  return attachment ? attachment.z : NaN;
}

function buildPointLights() {
  pointLights = [];
  for (var i = 0; i < wallDecorations.length; i++) {
    var d = wallDecorations[i];
    if (d.type !== 'torch' && d.type !== 'sconce') continue;
    var attachment = getWallDecorationAttachment(d);
    if (!attachment) continue;
    // Glow and illumination originate at the visible flame, just OUTSIDE its
    // supporting face. The former half-cell offset pointed into solid rock.
    pointLights.push({x: attachment.x, y: attachment.y, z: attachment.flameZ,
      radius: 90, intensity: 0.55, r: 255, g: 180, b: 80});
  }
  // Warm amber glow at cave entrances — subtle light spilling from the archway
  // onto the approach. Offset slightly into the cave mouth so the opening reads
  // as "light inside" rather than a flat teal halo on the surface.
  for (var j = 0; j < deepCaveEntrances.length; j++) {
    var e = deepCaveEntrances[j];
    var _ea = e.angle || 0;
    var _elx = e.x + Math.cos(_ea) * 10;
    var _ely = e.y + Math.sin(_ea) * 10;
    pointLights.push({x: _elx, y: _ely, z: (e.floorH || 0) * 25 + 35, radius: 80, intensity: 0.45, r: 255, g: 200, b: 140});
  }
  // Allocate light grid if needed
  var gw = Math.ceil(worldW / _lightCellSize);
  var gh = Math.ceil(worldH / _lightCellSize);
  if (!_lightGrid || _lightGridW !== gw || _lightGridH !== gh) {
    _lightGridW = gw;
    _lightGridH = gh;
    _lightGrid = new Float32Array(gw * gh);
    _bakedLightGrid = new Float32Array(gw * gh);
  } else {
    _bakedLightGrid.fill(0);
  }
  // Bake static light contributions (intensity × falloff) per grid cell —
  // day/night scaling and flicker are applied per-frame on top of this bake.
  for (var _bli = 0; _bli < pointLights.length; _bli++) {
    var _bl = pointLights[_bli];
    var _blcx = Math.floor(_bl.x / _lightCellSize);
    var _blcy = Math.floor(_bl.y / _lightCellSize);
    var _blrCells = Math.ceil(_bl.radius / _lightCellSize);
    var _blxMin = Math.max(0, _blcx - _blrCells);
    var _blxMax = Math.min(gw - 1, _blcx + _blrCells);
    var _blyMin = Math.max(0, _blcy - _blrCells);
    var _blyMax = Math.min(gh - 1, _blcy + _blrCells);
    var _blrSq = _bl.radius * _bl.radius;
    for (var _bly = _blyMin; _bly <= _blyMax; _bly++) {
      for (var _blx = _blxMin; _blx <= _blxMax; _blx++) {
        var _blwx = (_blx + 0.5) * _lightCellSize - _bl.x;
        var _blwy = (_bly + 0.5) * _lightCellSize - _bl.y;
        var _bldSq = _blwx * _blwx + _blwy * _blwy;
        if (_bldSq >= _blrSq) continue;
        _bakedLightGrid[_bly * gw + _blx] += _bl.intensity * (1.0 - _bldSq / _blrSq);
      }
    }
  }
  console.log('[LIGHTS] Built ' + pointLights.length + ' point lights (' +
    wallDecorations.filter(function(d){ return d.type === 'torch' || d.type === 'sconce'; }).length + ' torches, ' +
    deepCaveEntrances.length + ' cave entrances) — baked to static grid');
}

var _lightGridLastScale = -1, _lightGridLastCamGX = -9999, _lightGridLastCamGY = -9999;
function updateLightGrid() {
  if (!_lightGrid || (!pointLights.length && !playerUnderground)) return;
  var now = Date.now();
  var darkFactor = Math.max(0, 1.0 - ambientLight);
  // Day/night multiplier: torches matter less during day (0.3× min, 1× at night).
  // Global gentle flicker — one sin wave shared across all lights (cheap).
  var dayScale = 0.3 + 0.7 * darkFactor;
  var flicker = 0.95 + 0.04 * Math.sin(now * 0.007) + 0.02 * Math.sin(now * 0.013);
  var scale = dayScale * flicker;
  var camGX = Math.floor(cam.x / _lightCellSize);
  var camGY = Math.floor(cam.y / _lightCellSize);
  // Skip recompute when nothing that affects the scene light has moved:
  // camera still in the same light cell, scale unchanged (no flicker shift),
  // and we're not underground (player light handled below moves with player).
  if (_bakedLightGrid && !playerUnderground &&
      Math.abs(scale - _lightGridLastScale) < 0.002 &&
      camGX === _lightGridLastCamGX && camGY === _lightGridLastCamGY) {
    return;
  }
  _lightGridLastScale = scale;
  _lightGridLastCamGX = camGX;
  _lightGridLastCamGY = camGY;
  // Only touch cells within viewDist of camera — the grid outside isn't read
  // this frame anyway. ~60×60 cells instead of the full 280×280.
  if (_bakedLightGrid) {
    var viewRad = Math.ceil(viewDist / _lightCellSize) + 1;
    var gxMin = Math.max(0, camGX - viewRad);
    var gxMax = Math.min(_lightGridW - 1, camGX + viewRad);
    var gyMin = Math.max(0, camGY - viewRad);
    var gyMax = Math.min(_lightGridH - 1, camGY + viewRad);
    for (var _lgy = gyMin; _lgy <= gyMax; _lgy++) {
      var _rowOff = _lgy * _lightGridW;
      for (var _lgx = gxMin; _lgx <= gxMax; _lgx++) {
        _lightGrid[_rowOff + _lgx] = _bakedLightGrid[_rowOff + _lgx] * scale;
      }
    }
  } else {
    _lightGrid.fill(0);
  }
  // Player light source when underground — creates a circle of visibility
  if (playerUnderground) {
    var _plRadius = 120;
    var _plIntensity = 0.6;
    var _plEff = _plIntensity * (0.3 + 0.7 * darkFactor);
    var _plcx = Math.floor(pos.x / _lightCellSize);
    var _plcy = Math.floor(pos.y / _lightCellSize);
    var _plrCells = Math.ceil(_plRadius / _lightCellSize);
    var _plgxMin = Math.max(0, _plcx - _plrCells);
    var _plgxMax = Math.min(_lightGridW - 1, _plcx + _plrCells);
    var _plgyMin = Math.max(0, _plcy - _plrCells);
    var _plgyMax = Math.min(_lightGridH - 1, _plcy + _plrCells);
    var _plrSq = _plRadius * _plRadius;
    for (var _plgy = _plgyMin; _plgy <= _plgyMax; _plgy++) {
      for (var _plgx = _plgxMin; _plgx <= _plgxMax; _plgx++) {
        var _plwx = (_plgx + 0.5) * _lightCellSize - pos.x;
        var _plwy = (_plgy + 0.5) * _lightCellSize - pos.y;
        var _pldSq = _plwx * _plwx + _plwy * _plwy;
        if (_pldSq >= _plrSq) continue;
        var _plFalloff = 1.0 - _pldSq / _plrSq;
        _lightGrid[_plgy * _lightGridW + _plgx] += _plEff * _plFalloff;
      }
    }
  }
}

function isDecorationOccluded(decorX, decorY, camX, camY) {
  var dx = decorX - camX;
  var dy = decorY - camY;
  var dist = Math.hypot(dx, dy);
  if (dist < 2) return false;
  var steps = Math.floor(dist / 3);
  if (steps < 2) steps = 2;
  for (var i = 1; i < steps; i++) {
    var t = i / steps;
    var testX = camX + dx * t;
    var testY = camY + dy * t;
    var gridX = Math.floor(testX / cell);
    var gridY = Math.floor(testY / cell);
    if (gridX >= 0 && gridX < gridW && gridY >= 0 && gridY < gridH) {
      if (grid[gridY * gridW + gridX]) return true;
    }
  }
  return false;
}

function drawWallAlignedDecoration(type, x, y, size, dist, side, viewAngle, fade) {
  ctx.save();
  var brightness = Math.max(0.4, viewAngle);
  ctx.globalAlpha = Math.max(0.6, brightness) * (fade !== undefined ? fade : 1);
  var widthScale = 1.0;
  var heightScale = 1.0;
  var distanceFactor = Math.max(0.0, Math.min(1.0, (120 - dist) / 80));
  var angleEffect = distanceFactor * (1.0 - viewAngle);
  widthScale = 1.0 - angleEffect * 0.6;
  var w = Math.floor(size * widthScale);
  var h = Math.floor(size * heightScale);
  if (type === 'torch') {
    var stickColor = rgbQ(Math.floor(139 * brightness), Math.floor(69 * brightness), Math.floor(19 * brightness));
    ctx.fillStyle = stickColor;
    ctx.fillRect(x - w / 6, y, w / 3, h);
    ctx.fillStyle = '#FF4500'; ctx.globalAlpha *= 0.9;
    ctx.beginPath(); ctx.ellipse(x, y - h / 4, w / 3, h / 2, 0, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#FFD700';
    ctx.beginPath(); ctx.ellipse(x, y - h / 3, w / 5, h / 4, 0, 0, Math.PI * 2); ctx.fill();
  } else if (type === 'shield') {
    var metalColor = rgbQ(Math.floor(192 * brightness), Math.floor(192 * brightness), Math.floor(192 * brightness));
    ctx.fillStyle = metalColor;
    ctx.beginPath(); ctx.ellipse(x, y, w / 2, h * 0.6, 0, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#8B0000'; ctx.globalAlpha *= 0.8;
    ctx.beginPath();
    ctx.moveTo(x, y - h / 3); ctx.lineTo(x - w / 4, y); ctx.lineTo(x, y + h / 3); ctx.lineTo(x + w / 4, y);
    ctx.closePath(); ctx.fill();
  } else if (type === 'banner') {
    var poleColor = rgbQ(Math.floor(139 * brightness), Math.floor(69 * brightness), Math.floor(19 * brightness));
    ctx.fillStyle = poleColor;
    ctx.fillRect(x - w / 8, y - h / 2, w / 4, h);
    var fabricColor = rgbQ(Math.floor(128 * brightness), 0, Math.floor(128 * brightness));
    ctx.fillStyle = fabricColor;
    ctx.fillRect(x, y - h / 2, w / 2, h * 0.7);
    ctx.fillStyle = '#FFD700'; ctx.globalAlpha *= 0.9;
    ctx.fillRect(x + w / 8, y - h / 3, w / 4, h / 6);
  } else if (type === 'sconce') {
    var baseColor = rgbQ(Math.floor(105 * brightness), Math.floor(105 * brightness), Math.floor(105 * brightness));
    ctx.fillStyle = baseColor;
    ctx.beginPath(); ctx.arc(x, y, w / 3, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#FF6347'; ctx.globalAlpha *= 0.8;
    ctx.beginPath(); ctx.ellipse(x, y - h / 6, w / 4, h / 3, 0, 0, Math.PI * 2); ctx.fill();

  // ── Cave ornament types ─────────────────────────────────────────────────────

  } else if (type === 'fungi') {
    // Bioluminescent cave mushroom cluster — 3-4 small caps in teal/amber palette
    var fungCols = ['#00b8a0', '#00ddc8', '#e07820', '#ffaa40'];
    var offsets = [[-w*0.15, 0, 0.55], [w*0.12, h*0.05, 0.45], [-w*0.05, -h*0.08, 0.38], [w*0.18, -h*0.04, 0.32]];
    for (var fi = 0; fi < offsets.length; fi++) {
      var fo = offsets[fi];
      var fr = fo[2] * h;
      // Glow halo
      ctx.globalAlpha = 0.18 * brightness;
      ctx.fillStyle = fungCols[fi % fungCols.length];
      ctx.beginPath(); ctx.arc(x + fo[0], y + fo[1], fr * 2.2, 0, Math.PI * 2); ctx.fill();
      // Cap
      ctx.globalAlpha = 0.75 * brightness;
      ctx.fillStyle = fungCols[fi % fungCols.length];
      ctx.beginPath(); ctx.arc(x + fo[0], y + fo[1], fr, 0, Math.PI * 2); ctx.fill();
      // Stem
      ctx.globalAlpha = 0.5 * brightness;
      ctx.fillStyle = '#c8c0a8';
      ctx.fillRect(x + fo[0] - fr * 0.2, y + fo[1], fr * 0.4, fr * 1.3);
    }

  } else if (type === 'moss_drip') {
    // Moisture seep — dark green drip streaks anchored to upper wall face
    var mossGreen = rgbQ(Math.floor(40 * brightness), Math.floor(90 * brightness), Math.floor(30 * brightness));
    var drips = [[-w * 0.1, 0], [0, -h * 0.04], [w * 0.13, h * 0.02]];
    for (var mi = 0; mi < drips.length; mi++) {
      var mx2 = x + drips[mi][0], my2 = y - h * 0.3 + drips[mi][1];
      var dripH = h * (0.3 + mi * 0.07);
      var dripW = Math.max(1, Math.floor(w * 0.04));
      ctx.fillStyle = mossGreen; ctx.globalAlpha = 0.7 * brightness;
      ctx.fillRect(mx2 - dripW * 0.5, my2, dripW, dripH);
      // Drip bulb at bottom
      ctx.beginPath(); ctx.arc(mx2, my2 + dripH, dripW * 0.8, 0, Math.PI * 2); ctx.fill();
    }
    // Moss patch — irregular cluster near base of drips
    ctx.globalAlpha = 0.55 * brightness;
    ctx.fillStyle = rgbQ(Math.floor(30 * brightness), Math.floor(70 * brightness), Math.floor(20 * brightness));
    ctx.beginPath(); ctx.ellipse(x, y + h * 0.1, w * 0.45, h * 0.18, 0, 0, Math.PI * 2); ctx.fill();

  } else if (type === 'stalactite_tip') {
    // Downward rocky spike near the top of a tall cave wall face
    var stoneColor = rgbQ(Math.floor(70 * brightness), Math.floor(60 * brightness), Math.floor(50 * brightness));
    var tipY = y - h * 0.35;   // anchor near top of wall
    // Main spike
    ctx.fillStyle = stoneColor; ctx.globalAlpha = 0.85 * brightness;
    ctx.beginPath();
    ctx.moveTo(x - w * 0.18, tipY);
    ctx.lineTo(x + w * 0.18, tipY);
    ctx.lineTo(x, tipY + h * 0.45);
    ctx.closePath(); ctx.fill();
    // Secondary smaller spike offset
    ctx.globalAlpha = 0.65 * brightness;
    ctx.beginPath();
    ctx.moveTo(x + w * 0.22, tipY + h * 0.04);
    ctx.lineTo(x + w * 0.42, tipY + h * 0.04);
    ctx.lineTo(x + w * 0.32, tipY + h * 0.32);
    ctx.closePath(); ctx.fill();
    // Highlight edge
    ctx.strokeStyle = 'rgba(180,160,130,' + (0.3 * brightness) + ')';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x - w * 0.18, tipY); ctx.lineTo(x, tipY + h * 0.45);
    ctx.stroke();

  } else if (type === 'icicle') {
    // Ice formation hanging from wall — 3-4 translucent spikes
    var iceCols = ['rgba(140,200,255,' + (0.65*brightness) + ')', 'rgba(170,220,255,' + (0.55*brightness) + ')', 'rgba(120,185,240,' + (0.6*brightness) + ')'];
    var iceOff = [[-w*0.15, 0], [0, -h*0.03], [w*0.12, h*0.02], [w*0.25, -h*0.01]];
    for (var ii2 = 0; ii2 < iceOff.length; ii2++) {
      var io = iceOff[ii2];
      var spikeH = h * (0.25 + ii2 * 0.08);
      ctx.fillStyle = iceCols[ii2 % iceCols.length]; ctx.globalAlpha = 0.7 * brightness;
      ctx.beginPath();
      ctx.moveTo(x + io[0] - w*0.04, y - h*0.3 + io[1]);
      ctx.lineTo(x + io[0] + w*0.04, y - h*0.3 + io[1]);
      ctx.lineTo(x + io[0], y - h*0.3 + io[1] + spikeH);
      ctx.closePath(); ctx.fill();
    }
    // Glint highlight
    ctx.fillStyle = 'rgba(255,255,255,' + (0.3*brightness) + ')';
    ctx.beginPath(); ctx.arc(x, y - h*0.25, w*0.03, 0, Math.PI*2); ctx.fill();

  } else if (type === 'frost_crystal') {
    // Hexagonal frost crystal on wall surface
    ctx.globalAlpha = 0.6 * brightness;
    ctx.strokeStyle = 'rgba(180,225,255,' + (0.7*brightness) + ')'; ctx.lineWidth = Math.max(1, w*0.04);
    // Draw 6-pointed star pattern
    for (var fc = 0; fc < 6; fc++) {
      var fcAng = fc * Math.PI / 3;
      var fcLen = h * 0.3;
      ctx.beginPath(); ctx.moveTo(x, y);
      ctx.lineTo(x + Math.cos(fcAng)*fcLen*0.6, y + Math.sin(fcAng)*fcLen*0.4); ctx.stroke();
      // Branch tips
      if (fc % 2 === 0) {
        var bx2 = x + Math.cos(fcAng)*fcLen*0.45, by2 = y + Math.sin(fcAng)*fcLen*0.3;
        ctx.beginPath(); ctx.moveTo(bx2, by2);
        ctx.lineTo(bx2 + Math.cos(fcAng+0.8)*fcLen*0.2, by2 + Math.sin(fcAng+0.8)*fcLen*0.15); ctx.stroke();
      }
    }
    // Center glow
    ctx.fillStyle = 'rgba(200,240,255,' + (0.25*brightness) + ')';
    ctx.beginPath(); ctx.arc(x, y, h*0.08, 0, Math.PI*2); ctx.fill();

  } else if (type === 'vine_growth') {
    // Creeping vines on natural border walls
    var vineGreen = rgbQ(Math.floor(50*brightness), Math.floor(100*brightness), Math.floor(40*brightness));
    ctx.strokeStyle = vineGreen; ctx.lineWidth = Math.max(1, w*0.05); ctx.lineCap = 'round';
    ctx.globalAlpha = 0.7 * brightness;
    // Main vine
    ctx.beginPath(); ctx.moveTo(x - w*0.2, y - h*0.4);
    ctx.quadraticCurveTo(x + w*0.1, y - h*0.1, x - w*0.05, y + h*0.3); ctx.stroke();
    // Branch
    ctx.lineWidth = Math.max(1, w*0.03);
    ctx.beginPath(); ctx.moveTo(x, y - h*0.15);
    ctx.quadraticCurveTo(x + w*0.2, y - h*0.2, x + w*0.25, y - h*0.05); ctx.stroke();
    // Small leaves
    ctx.fillStyle = vineGreen; ctx.globalAlpha = 0.55 * brightness;
    var leafPos = [[w*0.25, -h*0.05], [-w*0.05, h*0.25], [w*0.08, -h*0.3]];
    for (var lf = 0; lf < leafPos.length; lf++) {
      ctx.beginPath(); ctx.ellipse(x + leafPos[lf][0], y + leafPos[lf][1], w*0.06, w*0.04, lf*0.8, 0, Math.PI*2); ctx.fill();
    }

  } else if (type === 'carved_rune') {
    // Ancient carved symbol on stone wall
    ctx.globalAlpha = 0.5 * brightness;
    ctx.strokeStyle = 'rgba(180,160,120,' + (0.6*brightness) + ')'; ctx.lineWidth = Math.max(1, w*0.05); ctx.lineCap = 'round';
    // Random rune pattern (circle + lines)
    ctx.beginPath(); ctx.arc(x, y, h*0.15, 0, Math.PI*2); ctx.stroke();
    // Cross lines through circle
    ctx.beginPath(); ctx.moveTo(x, y - h*0.22); ctx.lineTo(x, y + h*0.22); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x - w*0.15, y); ctx.lineTo(x + w*0.15, y); ctx.stroke();
    // Diagonal accent
    ctx.lineWidth = Math.max(1, w*0.03);
    ctx.beginPath(); ctx.moveTo(x - w*0.1, y - h*0.18); ctx.lineTo(x + w*0.1, y + h*0.18); ctx.stroke();
    // Faint glow
    ctx.fillStyle = 'rgba(200,180,120,' + (0.1*brightness) + ')';
    ctx.beginPath(); ctx.arc(x, y, h*0.25, 0, Math.PI*2); ctx.fill();
  }
  ctx.restore();
}

// ── FLOOR SCATTER ─────────────────────────────────────────────────────────────

// Draw a single floor scatter item at screen position (x, y) with perspective size.
// All shapes are anchored at their base (ground level) so they sit ON the floor.
function drawFloorItem(type, variant, seed, x, y, size) {
  ctx.save();
  var s = size;
  var s2 = s * 0.5, s4 = s * 0.25, s8 = s * 0.125;
  // Use seed for per-item sub-randomness without calling Math.random()
  var r0 = (seed * 7.3 + 0.1) % 1.0;
  var r1 = (seed * 13.7 + 0.3) % 1.0;
  var r2 = (seed * 19.1 + 0.6) % 1.0;

  if (type === 'bones' || type === 'dry_bones') {
    var boneCol = (type === 'dry_bones') ? '#c8b870' : '#c8c8c8';
    ctx.strokeStyle = boneCol; ctx.lineWidth = Math.max(1, s * 0.12); ctx.lineCap = 'round';
    var angles = [r0 * Math.PI, (r0 + 0.4) * Math.PI, (r1 + 0.7) * Math.PI];
    for (var bi = 0; bi < (variant === 0 ? 2 : 3); bi++) {
      var ba = angles[bi]; var bl = s * (0.5 + r1 * 0.3);
      ctx.globalAlpha = 0.85;
      ctx.beginPath(); ctx.moveTo(x + Math.cos(ba)*bl, y + Math.sin(ba)*bl*0.45);
      ctx.lineTo(x - Math.cos(ba)*bl, y - Math.sin(ba)*bl*0.45); ctx.stroke();
      // endpoint knuckle dots
      ctx.fillStyle = boneCol; ctx.globalAlpha = 0.9;
      ctx.beginPath(); ctx.arc(x + Math.cos(ba)*bl, y + Math.sin(ba)*bl*0.45, s*0.10, 0, Math.PI*2); ctx.fill();
      ctx.beginPath(); ctx.arc(x - Math.cos(ba)*bl, y - Math.sin(ba)*bl*0.45, s*0.10, 0, Math.PI*2); ctx.fill();
    }

  } else if (type === 'crate') {
    ctx.globalAlpha = 0.9;
    ctx.fillStyle = '#8b5a2b'; ctx.fillRect(x - s2, y - s2, s, s);
    ctx.fillStyle = '#6b3a1b'; ctx.fillRect(x - s2, y - s8, s, Math.max(1, s*0.15));
    ctx.fillRect(x - s8, y - s2, Math.max(1, s*0.15), s);
    if (variant === 2) { // broken corner
      ctx.clearRect(x + s2 - s*0.3, y - s2, s*0.32, s*0.32);
      ctx.fillStyle = '#3a1a08'; ctx.fillRect(x + s2 - s*0.3, y - s2, s*0.32, s*0.32);
    }
    ctx.strokeStyle = '#4a2808'; ctx.lineWidth = 1; ctx.globalAlpha = 0.7;
    ctx.strokeRect(x - s2, y - s2, s, s);

  } else if (type === 'skull') {
    ctx.globalAlpha = 0.92;
    // Dark outline for contrast
    ctx.fillStyle = '#2a2018';
    ctx.beginPath(); ctx.ellipse(x, y - s*0.14, s*0.42, s*0.36, 0, 0, Math.PI*2); ctx.fill();
    // Cranium
    ctx.fillStyle = '#e8e0c8';
    ctx.beginPath(); ctx.ellipse(x, y - s*0.15, s*0.38, s*0.32, 0, 0, Math.PI*2); ctx.fill();
    // Jaw
    ctx.fillStyle = '#d0c8b0';
    ctx.beginPath(); ctx.ellipse(x, y + s*0.12, s*0.28, s*0.18, 0, 0, Math.PI); ctx.fill();
    // Eye sockets — larger and darker
    ctx.fillStyle = '#000000'; ctx.globalAlpha = 0.9;
    ctx.beginPath(); ctx.ellipse(x - s*0.14, y - s*0.18, s*0.12, s*0.13, 0, 0, Math.PI*2); ctx.fill();
    ctx.beginPath(); ctx.ellipse(x + s*0.14, y - s*0.18, s*0.12, s*0.13, 0, 0, Math.PI*2); ctx.fill();
    // Nose hole
    ctx.beginPath(); ctx.ellipse(x, y - s*0.02, s*0.05, s*0.07, 0, 0, Math.PI*2); ctx.fill();
    // Teeth
    ctx.fillStyle = '#e8e0c8'; ctx.globalAlpha = 0.9;
    for (var ti = 0; ti < 4; ti++) {
      ctx.fillRect(x - s*0.16 + ti*s*0.1, y + s*0.03, Math.max(1,s*0.07), Math.max(1,s*0.10));
    }
    // Tooth gaps
    ctx.fillStyle = '#1a1008'; ctx.globalAlpha = 0.7;
    for (var tg = 0; tg < 3; tg++) {
      ctx.fillRect(x - s*0.06 + tg*s*0.1, y + s*0.03, Math.max(1,s*0.02), Math.max(1,s*0.10));
    }

  } else if (type === 'rubble') {
    var rubCols = ['#787060','#686058','#888070','#504840'];
    for (var ri = 0; ri < 5; ri++) {
      var rox = (((ri*7+3)*seed*11)%1.0 - 0.5) * s * 0.9;
      var roy = (((ri*5+1)*seed*17)%1.0 - 0.5) * s * 0.5;
      var rr = s * (0.12 + ((ri*3+seed*7)%1.0) * 0.15);
      ctx.fillStyle = rubCols[ri % rubCols.length]; ctx.globalAlpha = 0.8;
      ctx.beginPath(); ctx.ellipse(x+rox, y+roy, rr*1.3, rr*0.7, r0*Math.PI, 0, Math.PI*2); ctx.fill();
    }

  } else if (type === 'ice_shard') {
    ctx.globalAlpha = 0.75;
    var shardCols = ['rgba(140,200,255,0.7)','rgba(180,230,255,0.6)','rgba(100,170,240,0.65)'];
    for (var ii = 0; ii < (variant === 0 ? 2 : 3); ii++) {
      var iox = (ii - 1) * s * 0.35; var ih = s * (0.7 + ii * 0.2);
      ctx.fillStyle = shardCols[ii % shardCols.length];
      ctx.beginPath();
      ctx.moveTo(x + iox - s*0.1, y);
      ctx.lineTo(x + iox + s*0.1, y);
      ctx.lineTo(x + iox, y - ih); ctx.closePath(); ctx.fill();
      ctx.strokeStyle = 'rgba(220,240,255,0.5)'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x + iox, y); ctx.lineTo(x + iox, y - ih); ctx.stroke();
    }

  } else if (type === 'frozen_pool') {
    ctx.globalAlpha = 0.55;
    ctx.fillStyle = 'rgba(100,160,255,0.45)';
    ctx.beginPath(); ctx.ellipse(x, y, s*0.7, s*0.3, 0, 0, Math.PI*2); ctx.fill();
    ctx.strokeStyle = 'rgba(200,230,255,0.6)'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.ellipse(x - s*0.15, y - s*0.06, s*0.28, s*0.1, -0.3, 0, Math.PI); ctx.stroke();
    // crack
    ctx.strokeStyle = 'rgba(180,210,255,0.4)'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(x - s*0.2, y + s*0.05); ctx.lineTo(x+s*0.35, y-s*0.1); ctx.stroke();

  } else if (type === 'crystal') {
    var crystColors = (terrain === 'expanse' || terrain === 'plains')
      ? ['#c8a000','#ffe066','#e8b800'] : ['#00c8a8','#00eedd','#60d8c8'];
    for (var ki = 0; ki < (variant === 0 ? 2 : 3); ki++) {
      var kox = (ki - 1) * s * 0.3; var kh = s * (0.6 + ki * 0.25);
      ctx.globalAlpha = 0.85; ctx.fillStyle = crystColors[ki % crystColors.length];
      ctx.beginPath();
      ctx.moveTo(x+kox-s*0.09, y - s*0.05);
      ctx.lineTo(x+kox+s*0.09, y - s*0.05);
      ctx.lineTo(x+kox, y - kh); ctx.closePath(); ctx.fill();
      // bright inner core
      ctx.globalAlpha = 0.55; ctx.fillStyle = '#ffffff';
      ctx.beginPath();
      ctx.moveTo(x+kox-s*0.03, y - kh*0.4);
      ctx.lineTo(x+kox+s*0.03, y - kh*0.4);
      ctx.lineTo(x+kox, y - kh*0.85); ctx.closePath(); ctx.fill();
    }

  } else if (type === 'stalagmite') {
    ctx.globalAlpha = 0.82;
    var stCol = '#6a5848';
    ctx.fillStyle = stCol;
    var sw = s * (0.12 + r0 * 0.08); var sh = s * (0.9 + r1 * 0.5);
    ctx.beginPath();
    ctx.moveTo(x - sw, y);
    ctx.lineTo(x + sw, y);
    ctx.lineTo(x + sw*0.3, y - sh);
    ctx.lineTo(x - sw*0.3, y - sh); ctx.closePath(); ctx.fill();
    ctx.strokeStyle = 'rgba(180,160,130,0.4)'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(x - sw*0.2, y); ctx.lineTo(x - sw*0.1, y - sh*0.8); ctx.stroke();

  } else if (type === 'rock_pile') {
    var rpCols = ['#787060','#686058','#888070'];
    for (var rpi = 0; rpi < 3; rpi++) {
      var rpox = (rpi-1) * s*0.3 + (r0-0.5)*s*0.15;
      var rpoy = (r1-0.5)*s*0.2;
      var rpr = s*(0.22 + rpi*0.04);
      ctx.globalAlpha = 0.8; ctx.fillStyle = rpCols[rpi];
      ctx.beginPath(); ctx.ellipse(x+rpox, y+rpoy, rpr*1.2, rpr*0.75, r2*Math.PI, 0, Math.PI*2); ctx.fill();
    }

  } else if (type === 'puddle') {
    ctx.globalAlpha = 0.65;
    ctx.fillStyle = 'rgba(15,22,35,0.75)';
    ctx.beginPath(); ctx.ellipse(x, y, s*0.65, s*0.28, 0, 0, Math.PI*2); ctx.fill();
    ctx.strokeStyle = 'rgba(60,80,100,0.5)'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.ellipse(x - s*0.1, y - s*0.06, s*0.2, s*0.07, -0.4, 0, Math.PI); ctx.stroke();

  } else if (type === 'desert_rock') {
    ctx.globalAlpha = 0.82;
    ctx.fillStyle = '#b8905a';
    ctx.beginPath(); ctx.ellipse(x, y - s*0.15, s*(0.38+r0*0.15), s*(0.25+r1*0.1), r2*0.5, 0, Math.PI*2); ctx.fill();
    ctx.fillStyle = '#d0a870'; ctx.globalAlpha = 0.6;
    ctx.beginPath(); ctx.ellipse(x - s*0.08, y - s*0.22, s*0.15, s*0.08, -0.5, 0, Math.PI*2); ctx.fill();

  } else if (type === 'dead_shrub') {
    ctx.strokeStyle = '#6a4820'; ctx.lineWidth = Math.max(1, s*0.09); ctx.lineCap = 'round';
    ctx.globalAlpha = 0.78;
    ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + (r0-0.5)*s*0.2, y - s*0.65); ctx.stroke();
    var branches = [[0.4, -0.4, 0.5, 0.3],[-0.35, -0.45, -0.55, 0.25],[0.15, -0.6, 0.45, 0.2]];
    for (var bri = 0; bri < 3; bri++) {
      var br = branches[bri];
      var bx1 = x + br[0]*s, by1 = y + br[1]*s;
      ctx.lineWidth = Math.max(1, s*0.06);
      ctx.beginPath(); ctx.moveTo(bx1, by1); ctx.lineTo(bx1 + br[2]*s, by1 + br[3]*s); ctx.stroke();
    }

  } else if (type === 'rib_cage') {
    // Curved rib bones arching from a central spine
    ctx.strokeStyle = '#c0b8a0'; ctx.lineWidth = Math.max(1, s*0.08); ctx.lineCap = 'round';
    ctx.globalAlpha = 0.82;
    // Spine
    ctx.beginPath(); ctx.moveTo(x - s*0.35, y); ctx.lineTo(x + s*0.35, y); ctx.stroke();
    // Ribs curving upward
    for (var ri2 = 0; ri2 < 4; ri2++) {
      var rx = x - s*0.25 + ri2 * s*0.18;
      var ribH = s*(0.25 + r0*0.15);
      ctx.lineWidth = Math.max(1, s*0.06);
      ctx.beginPath();
      ctx.moveTo(rx, y);
      ctx.quadraticCurveTo(rx + s*0.06, y - ribH, rx + s*0.12, y - ribH*0.6);
      ctx.stroke();
      ctx.beginPath();
      ctx.moveTo(rx, y);
      ctx.quadraticCurveTo(rx - s*0.06, y + ribH*0.7, rx - s*0.1, y + ribH*0.4);
      ctx.stroke();
    }

  } else if (type === 'femur') {
    // Single large bone with bulbous ends
    var boneCol2 = '#d0c8b0';
    ctx.strokeStyle = boneCol2; ctx.lineWidth = Math.max(2, s*0.14); ctx.lineCap = 'round';
    ctx.globalAlpha = 0.85;
    var fAng = r0 * Math.PI;
    var fLen = s * 0.6;
    var fx1 = x + Math.cos(fAng)*fLen, fy1 = y + Math.sin(fAng)*fLen*0.4;
    var fx2 = x - Math.cos(fAng)*fLen, fy2 = y - Math.sin(fAng)*fLen*0.4;
    ctx.beginPath(); ctx.moveTo(fx1, fy1); ctx.lineTo(fx2, fy2); ctx.stroke();
    // Bulbous knobs at each end
    ctx.fillStyle = boneCol2;
    ctx.beginPath(); ctx.arc(fx1, fy1, s*0.14, 0, Math.PI*2); ctx.fill();
    ctx.beginPath(); ctx.arc(fx2, fy2, s*0.14, 0, Math.PI*2); ctx.fill();
    // Smaller knob bumps
    ctx.fillStyle = '#b8b098'; ctx.globalAlpha = 0.7;
    ctx.beginPath(); ctx.arc(fx1 + Math.cos(fAng+0.8)*s*0.08, fy1 + Math.sin(fAng+0.8)*s*0.05, s*0.07, 0, Math.PI*2); ctx.fill();
    ctx.beginPath(); ctx.arc(fx2 - Math.cos(fAng-0.8)*s*0.08, fy2 - Math.sin(fAng-0.8)*s*0.05, s*0.07, 0, Math.PI*2); ctx.fill();

  } else if (type === 'stick_bundle') {
    // 3-5 sticks scattered loosely (50% larger than base size)
    var ss = s * 1.5;
    ctx.lineCap = 'round'; ctx.globalAlpha = 0.78;
    var stickCols = ['#5a3e1e','#6b4a28','#4d3218','#7a5a38'];
    var nSticks = 3 + Math.floor(r0 * 3);
    for (var sti = 0; sti < nSticks; sti++) {
      var stAng = (r0 + sti * 0.7 + r1 * 0.3) * Math.PI;
      var stLen = ss * (0.35 + ((sti * 7 + seed * 11) % 1.0) * 0.3);
      ctx.strokeStyle = stickCols[sti % stickCols.length];
      ctx.lineWidth = Math.max(1, ss * (0.04 + ((sti * 3 + seed * 5) % 1.0) * 0.04));
      var sox = (((sti * 11 + seed * 3) % 1.0) - 0.5) * ss * 0.3;
      var soy = (((sti * 7 + seed * 13) % 1.0) - 0.5) * ss * 0.15;
      ctx.beginPath();
      ctx.moveTo(x + sox + Math.cos(stAng)*stLen, y + soy + Math.sin(stAng)*stLen*0.4);
      ctx.lineTo(x + sox - Math.cos(stAng)*stLen, y + soy - Math.sin(stAng)*stLen*0.4);
      ctx.stroke();
    }

  } else if (type === 'flat_rock') {
    // Large flat rounded stone
    ctx.globalAlpha = 0.8;
    var frCol = terrain === 'cave' ? '#58504a' : (terrain === 'ice' ? '#8a98a8' : '#9a8a6a');
    ctx.fillStyle = frCol;
    ctx.beginPath();
    ctx.ellipse(x, y, s*(0.5+r0*0.15), s*(0.22+r1*0.08), r2*Math.PI*0.5, 0, Math.PI*2);
    ctx.fill();
    // Highlight edge
    ctx.strokeStyle = 'rgba(255,255,255,0.15)'; ctx.lineWidth = Math.max(1, s*0.06);
    ctx.beginPath();
    ctx.ellipse(x - s*0.05, y - s*0.04, s*(0.38+r0*0.1), s*(0.14+r1*0.05), r2*Math.PI*0.5, Math.PI*0.8, Math.PI*1.6);
    ctx.stroke();
    // Dark crack line
    ctx.strokeStyle = 'rgba(0,0,0,0.25)'; ctx.lineWidth = Math.max(1, s*0.04);
    ctx.beginPath();
    ctx.moveTo(x - s*0.2, y + s*0.02);
    ctx.lineTo(x + s*0.15, y - s*0.04);
    ctx.stroke();

  } else if (type === 'cracked_stone') {
    // Angular broken stone slab with crack lines
    ctx.globalAlpha = 0.82;
    var csCol = terrain === 'cave' ? '#504848' : '#8a7860';
    ctx.fillStyle = csCol;
    // Irregular angular shape
    ctx.beginPath();
    ctx.moveTo(x - s*0.4, y - s*0.08);
    ctx.lineTo(x - s*0.15, y - s*0.28);
    ctx.lineTo(x + s*0.3, y - s*0.18);
    ctx.lineTo(x + s*0.42, y + s*0.05);
    ctx.lineTo(x + s*0.2, y + s*0.22);
    ctx.lineTo(x - s*0.25, y + s*0.15);
    ctx.closePath(); ctx.fill();
    // Crack lines
    ctx.strokeStyle = 'rgba(0,0,0,0.35)'; ctx.lineWidth = Math.max(1, s*0.05); ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.moveTo(x - s*0.1, y - s*0.25);
    ctx.lineTo(x + s*0.05, y + s*0.02);
    ctx.lineTo(x + s*0.25, y + s*0.18);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(x + s*0.05, y + s*0.02);
    ctx.lineTo(x - s*0.2, y + s*0.12);
    ctx.stroke();
    // Lighter highlight on one face
    ctx.fillStyle = 'rgba(255,255,255,0.1)';
    ctx.beginPath();
    ctx.moveTo(x - s*0.15, y - s*0.28);
    ctx.lineTo(x + s*0.3, y - s*0.18);
    ctx.lineTo(x + s*0.05, y + s*0.02);
    ctx.lineTo(x - s*0.1, y - s*0.25);
    ctx.closePath(); ctx.fill();

  } else if (type === 'boulder') {
    // Large rounded boulder with highlight and shadow
    ctx.globalAlpha = 0.85;
    var bldR = s * (0.4 + r0 * 0.15);
    ctx.fillStyle = '#5a5550';
    ctx.beginPath(); ctx.ellipse(x, y, bldR * 1.1, bldR * 0.7, r2 * 0.5, 0, Math.PI * 2); ctx.fill();
    // Shadow underneath
    ctx.fillStyle = 'rgba(0,0,0,0.3)';
    ctx.beginPath(); ctx.ellipse(x + s*0.05, y + bldR*0.35, bldR*0.9, bldR*0.25, 0, 0, Math.PI*2); ctx.fill();
    // Top highlight
    ctx.fillStyle = 'rgba(255,255,255,0.15)';
    ctx.beginPath(); ctx.ellipse(x - bldR*0.2, y - bldR*0.25, bldR*0.45, bldR*0.3, -0.3, 0, Math.PI*2); ctx.fill();
    // Moss patches (variant 1+)
    if (variant > 0) {
      ctx.fillStyle = 'rgba(60,90,50,0.35)';
      ctx.beginPath(); ctx.ellipse(x + bldR*0.3, y - bldR*0.1, bldR*0.2, bldR*0.15, r1, 0, Math.PI*2); ctx.fill();
    }

  } else if (type === 'stone_column') {
    // Broken stone column / pillar remnant rising from cave floor
    ctx.globalAlpha = 0.82;
    var colW = s * (0.14 + r0 * 0.06);
    var colH = s * (0.8 + r1 * 0.6);
    ctx.fillStyle = '#605850';
    // Main pillar body
    ctx.beginPath();
    ctx.moveTo(x - colW, y);
    ctx.lineTo(x + colW, y);
    ctx.lineTo(x + colW * 0.7, y - colH);
    ctx.lineTo(x - colW * 0.7, y - colH);
    ctx.closePath(); ctx.fill();
    // Jagged broken top
    ctx.fillStyle = '#706860';
    ctx.beginPath();
    ctx.moveTo(x - colW * 0.7, y - colH);
    ctx.lineTo(x - colW * 0.2, y - colH - s * 0.15);
    ctx.lineTo(x + colW * 0.3, y - colH - s * 0.08);
    ctx.lineTo(x + colW * 0.7, y - colH);
    ctx.closePath(); ctx.fill();
    // Vertical crack
    ctx.strokeStyle = 'rgba(0,0,0,0.3)'; ctx.lineWidth = Math.max(1, s * 0.04);
    ctx.beginPath();
    ctx.moveTo(x + colW * 0.15, y - colH * 0.1);
    ctx.lineTo(x - colW * 0.1, y - colH * 0.55);
    ctx.lineTo(x + colW * 0.05, y - colH * 0.9);
    ctx.stroke();
    // Light edge
    ctx.strokeStyle = 'rgba(255,255,255,0.12)'; ctx.lineWidth = Math.max(1, s * 0.03);
    ctx.beginPath(); ctx.moveTo(x - colW, y); ctx.lineTo(x - colW * 0.7, y - colH); ctx.stroke();

  } else if (type === 'rock_arch') {
    // Small natural rock arch / bridge formation
    ctx.globalAlpha = 0.8;
    var archW = s * 0.55;
    var archH = s * (0.5 + r0 * 0.3);
    ctx.fillStyle = '#585048';
    // Left pillar
    ctx.fillRect(x - archW - s*0.08, y - archH*0.4, s*0.16, archH*0.4);
    // Right pillar
    ctx.fillRect(x + archW - s*0.08, y - archH*0.4, s*0.16, archH*0.4);
    // Arch span
    ctx.beginPath();
    ctx.moveTo(x - archW - s*0.1, y - archH*0.4);
    ctx.quadraticCurveTo(x, y - archH, x + archW + s*0.1, y - archH*0.4);
    ctx.lineTo(x + archW + s*0.06, y - archH*0.3);
    ctx.quadraticCurveTo(x, y - archH*0.75, x - archW - s*0.06, y - archH*0.3);
    ctx.closePath(); ctx.fill();
    // Highlight
    ctx.strokeStyle = 'rgba(255,255,255,0.12)'; ctx.lineWidth = Math.max(1, s*0.03);
    ctx.beginPath();
    ctx.moveTo(x - archW - s*0.1, y - archH*0.4);
    ctx.quadraticCurveTo(x, y - archH, x + archW + s*0.1, y - archH*0.4);
    ctx.stroke();

  } else if (type === 'rock_spire') {
    // Tall thin rock spire / stalagmite cluster
    ctx.globalAlpha = 0.8;
    var spireCount = 2 + Math.floor(r0 * 2);
    var spCols = ['#5a5248','#685e52','#4e4840'];
    for (var spi = 0; spi < spireCount; spi++) {
      var spOx = (spi - spireCount*0.5 + 0.5) * s * 0.22 + (r1 - 0.5) * s * 0.1;
      var spW = s * (0.07 + ((spi*3+seed*7)%1.0) * 0.05);
      var spH = s * (0.5 + ((spi*5+seed*11)%1.0) * 0.7);
      ctx.fillStyle = spCols[spi % spCols.length];
      ctx.beginPath();
      ctx.moveTo(x + spOx - spW, y);
      ctx.lineTo(x + spOx + spW, y);
      ctx.lineTo(x + spOx + spW*0.2, y - spH);
      ctx.lineTo(x + spOx - spW*0.2, y - spH);
      ctx.closePath(); ctx.fill();
      // Inner highlight
      ctx.strokeStyle = 'rgba(255,255,255,0.1)'; ctx.lineWidth = Math.max(1, s*0.02);
      ctx.beginPath(); ctx.moveTo(x + spOx - spW*0.5, y); ctx.lineTo(x + spOx - spW*0.15, y - spH*0.9); ctx.stroke();
    }

  } else if (type === 'cave_rubble_pile') {
    // Large mound of cave debris — stacked irregular rocks
    ctx.globalAlpha = 0.82;
    var pCols = ['#504a44','#5e5650','#686058','#3e3a36'];
    // Base mound shape
    ctx.fillStyle = '#504a44';
    ctx.beginPath();
    ctx.moveTo(x - s*0.55, y + s*0.05);
    ctx.quadraticCurveTo(x - s*0.2, y - s*0.35, x + s*0.05, y - s*0.4);
    ctx.quadraticCurveTo(x + s*0.35, y - s*0.3, x + s*0.5, y + s*0.05);
    ctx.closePath(); ctx.fill();
    // Individual rocks on top
    for (var cri = 0; cri < 6; cri++) {
      var crox = (((cri*7+3)*seed*11)%1.0 - 0.5) * s * 0.7;
      var croy = (((cri*5+1)*seed*17)%1.0 - 0.5) * s * 0.3 - s*0.1;
      var crr = s * (0.08 + ((cri*3+seed*7)%1.0) * 0.1);
      ctx.fillStyle = pCols[cri % pCols.length];
      ctx.beginPath(); ctx.ellipse(x+crox, y+croy, crr*1.4, crr*0.8, ((cri+seed)*2)%Math.PI, 0, Math.PI*2); ctx.fill();
    }
    // Shadow at base
    ctx.fillStyle = 'rgba(0,0,0,0.2)';
    ctx.beginPath(); ctx.ellipse(x, y + s*0.08, s*0.5, s*0.08, 0, 0, Math.PI*2); ctx.fill();

  } else if (type === 'icicle_cluster') {
    // Cluster of icicles hanging down (drawn pointing up from floor perspective)
    ctx.globalAlpha = 0.7;
    var icCols = ['rgba(160,210,255,0.7)','rgba(130,190,240,0.65)','rgba(180,225,255,0.6)'];
    var nIc = 3 + Math.floor(r0 * 2);
    for (var ici = 0; ici < nIc; ici++) {
      var icox = (ici - nIc*0.5 + 0.5) * s * 0.2;
      var ich = s * (0.4 + ((ici*7+seed*5)%1.0) * 0.5);
      var icw = s * 0.06;
      ctx.fillStyle = icCols[ici % icCols.length];
      ctx.beginPath();
      ctx.moveTo(x + icox - icw, y); ctx.lineTo(x + icox + icw, y);
      ctx.lineTo(x + icox, y - ich); ctx.closePath(); ctx.fill();
    }
    // Glint
    ctx.fillStyle = 'rgba(255,255,255,0.3)';
    ctx.beginPath(); ctx.arc(x, y - s*0.3, s*0.04, 0, Math.PI*2); ctx.fill();

  } else if (type === 'frost_patch') {
    // Frosted ground patch with crystal patterns
    ctx.globalAlpha = 0.4;
    ctx.fillStyle = 'rgba(180,220,255,0.35)';
    ctx.beginPath(); ctx.ellipse(x, y, s*0.6, s*0.25, r0*0.5, 0, Math.PI*2); ctx.fill();
    // Frost crystal lines
    ctx.strokeStyle = 'rgba(200,235,255,0.5)'; ctx.lineWidth = Math.max(1, s*0.03);
    for (var fi = 0; fi < 5; fi++) {
      var fAng2 = (fi / 5) * Math.PI * 2 + r1;
      var fLen2 = s * (0.15 + r0 * 0.15);
      ctx.beginPath(); ctx.moveTo(x, y);
      ctx.lineTo(x + Math.cos(fAng2)*fLen2, y + Math.sin(fAng2)*fLen2*0.4); ctx.stroke();
    }

  } else if (type === 'frozen_skull') {
    // Skull encased in ice
    ctx.globalAlpha = 0.75;
    // Ice casing
    ctx.fillStyle = 'rgba(140,200,255,0.3)';
    ctx.beginPath(); ctx.ellipse(x, y - s*0.1, s*0.45, s*0.38, 0, 0, Math.PI*2); ctx.fill();
    // Skull inside
    ctx.fillStyle = '#b8b0a0';
    ctx.beginPath(); ctx.ellipse(x, y - s*0.12, s*0.3, s*0.25, 0, 0, Math.PI*2); ctx.fill();
    ctx.fillStyle = '#1a1008'; ctx.globalAlpha = 0.7;
    ctx.beginPath(); ctx.arc(x - s*0.1, y - s*0.15, s*0.06, 0, Math.PI*2); ctx.fill();
    ctx.beginPath(); ctx.arc(x + s*0.1, y - s*0.15, s*0.06, 0, Math.PI*2); ctx.fill();

  } else if (type === 'tall_grass') {
    // Tuft of tall grass blades
    ctx.globalAlpha = 0.7;
    var grassCols = ['#5a7a3a','#4a6830','#6a8a48','#3e5828'];
    var nBlades = 4 + Math.floor(r0 * 3);
    for (var tgi = 0; tgi < nBlades; tgi++) {
      var gAng = (tgi / nBlades - 0.5) * 1.2 + (r1 - 0.5) * 0.3;
      var gH = s * (0.5 + ((tgi*5+seed*3)%1.0) * 0.5);
      ctx.strokeStyle = grassCols[tgi % grassCols.length];
      ctx.lineWidth = Math.max(1, s * 0.05);
      ctx.beginPath(); ctx.moveTo(x + (tgi - nBlades*0.5)*s*0.08, y);
      ctx.quadraticCurveTo(x + gAng*s*0.3, y - gH*0.6, x + gAng*s*0.45, y - gH);
      ctx.stroke();
    }

  } else if (type === 'wildflower') {
    // Small wildflower cluster
    ctx.globalAlpha = 0.75;
    // Stems
    ctx.strokeStyle = '#4a6830'; ctx.lineWidth = Math.max(1, s*0.04);
    var flCols = ['#d84040','#d8a030','#c060c0','#4080d0'];
    var nFlowers = 2 + Math.floor(r0 * 2);
    for (var wfi = 0; wfi < nFlowers; wfi++) {
      var wfox = (wfi - nFlowers*0.5 + 0.5) * s * 0.25;
      var wfh = s * (0.3 + ((wfi*7+seed*3)%1.0) * 0.3);
      ctx.beginPath(); ctx.moveTo(x + wfox, y); ctx.lineTo(x + wfox + (r1-0.5)*s*0.1, y - wfh); ctx.stroke();
      // Flower head
      ctx.fillStyle = flCols[wfi % flCols.length];
      ctx.beginPath(); ctx.arc(x + wfox + (r1-0.5)*s*0.1, y - wfh, s*0.06, 0, Math.PI*2); ctx.fill();
      ctx.fillStyle = '#ffee60'; ctx.beginPath();
      ctx.arc(x + wfox + (r1-0.5)*s*0.1, y - wfh, s*0.03, 0, Math.PI*2); ctx.fill();
    }

  } else if (type === 'stone_marker') {
    // Standing stone / menhir
    ctx.globalAlpha = 0.82;
    var mkW = s * 0.12; var mkH = s * (0.6 + r0 * 0.4);
    ctx.fillStyle = '#707868';
    ctx.beginPath();
    ctx.moveTo(x - mkW, y); ctx.lineTo(x + mkW, y);
    ctx.lineTo(x + mkW*0.6, y - mkH);
    ctx.lineTo(x - mkW*0.4, y - mkH - s*0.05);
    ctx.closePath(); ctx.fill();
    // Carved line
    ctx.strokeStyle = 'rgba(0,0,0,0.25)'; ctx.lineWidth = Math.max(1, s*0.03);
    ctx.beginPath(); ctx.moveTo(x - mkW*0.3, y - mkH*0.3);
    ctx.lineTo(x + mkW*0.4, y - mkH*0.5); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x - mkW*0.2, y - mkH*0.6);
    ctx.lineTo(x + mkW*0.3, y - mkH*0.75); ctx.stroke();

  } else if (type === 'barrel') {
    // Wooden barrel
    ctx.globalAlpha = 0.85;
    ctx.fillStyle = '#7a5230';
    ctx.beginPath(); ctx.ellipse(x, y - s*0.15, s*0.28, s*0.35, 0, 0, Math.PI*2); ctx.fill();
    // Metal bands
    ctx.strokeStyle = '#555'; ctx.lineWidth = Math.max(1, s*0.06);
    ctx.beginPath(); ctx.ellipse(x, y - s*0.35, s*0.24, s*0.06, 0, 0, Math.PI*2); ctx.stroke();
    ctx.beginPath(); ctx.ellipse(x, y + s*0.05, s*0.24, s*0.06, 0, 0, Math.PI*2); ctx.stroke();
    // Wood grain
    ctx.strokeStyle = 'rgba(0,0,0,0.2)'; ctx.lineWidth = Math.max(1, s*0.02);
    ctx.beginPath(); ctx.moveTo(x - s*0.05, y - s*0.45); ctx.lineTo(x - s*0.05, y + s*0.15); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x + s*0.12, y - s*0.45); ctx.lineTo(x + s*0.12, y + s*0.15); ctx.stroke();

  } else if (type === 'bookshelf_debris') {
    // Broken bookshelf / scattered books
    ctx.globalAlpha = 0.85;
    // Broken shelf plank — thicker, with wood grain
    ctx.fillStyle = '#5a3e20';
    ctx.fillRect(x - s*0.45, y + s*0.05, s*0.9, s*0.12);
    ctx.fillStyle = '#4a3018';
    ctx.fillRect(x - s*0.45, y + s*0.13, s*0.9, s*0.04);
    // Scattered books — thicker with visible page edges
    var bookCols = ['#8b2020','#1a4a6a','#2a5a2a','#6a4a20','#5a1a5a'];
    for (var bki = 0; bki < 5; bki++) {
      var bkx = x - s*0.35 + bki * s*0.17 + (((bki*3+seed*5)%1.0)-0.5)*s*0.06;
      var bky = y - s*0.02 - ((bki*3+seed*5)%1.0)*s*0.2;
      var bkAng = ((bki*7+seed*11)%1.0 - 0.5) * 0.5;
      var bkW = s*0.14, bkH = s*0.24;
      ctx.save(); ctx.translate(bkx, bky); ctx.rotate(bkAng);
      // Book cover
      ctx.fillStyle = bookCols[bki % bookCols.length];
      ctx.fillRect(-bkW*0.5, -bkH*0.5, bkW, bkH);
      // Page edge — lighter stripe
      ctx.fillStyle = '#e8e0d0';
      ctx.fillRect(-bkW*0.5 + bkW*0.15, -bkH*0.5 + bkH*0.1, bkW*0.08, bkH*0.8);
      // Dark outline
      ctx.strokeStyle = 'rgba(0,0,0,0.4)'; ctx.lineWidth = Math.max(1, s*0.02);
      ctx.strokeRect(-bkW*0.5, -bkH*0.5, bkW, bkH);
      ctx.restore();
    }

  } else if (type === 'iron_chain') {
    // Coiled chain on the ground
    ctx.globalAlpha = 0.7;
    ctx.strokeStyle = '#707878'; ctx.lineWidth = Math.max(2, s*0.08); ctx.lineCap = 'round';
    // Loose coil
    ctx.beginPath();
    ctx.arc(x, y, s*0.25, 0, Math.PI*1.5); ctx.stroke();
    // Trailing links
    ctx.lineWidth = Math.max(1, s*0.06);
    ctx.beginPath();
    ctx.moveTo(x + s*0.25, y); ctx.lineTo(x + s*0.5, y + s*0.1);
    ctx.lineTo(x + s*0.55, y - s*0.05); ctx.stroke();
    // Highlight
    ctx.strokeStyle = 'rgba(255,255,255,0.15)'; ctx.lineWidth = Math.max(1, s*0.03);
    ctx.beginPath(); ctx.arc(x, y, s*0.25, 0.5, Math.PI*0.8); ctx.stroke();

  } else if (type === 'sand_pillar') {
    // Weathered sandstone column — desert/expanse only
    ctx.globalAlpha = 0.82;
    var spW = s * (0.16 + r0 * 0.06);
    var spH = s * (0.75 + r1 * 0.5);
    // Main body — tapers upward, warm sandstone
    ctx.fillStyle = '#c4a060';
    ctx.beginPath();
    ctx.moveTo(x - spW, y); ctx.lineTo(x + spW, y);
    ctx.lineTo(x + spW * 0.6, y - spH); ctx.lineTo(x - spW * 0.6, y - spH);
    ctx.closePath(); ctx.fill();
    // Wind erosion bands
    ctx.strokeStyle = 'rgba(80,50,20,0.3)'; ctx.lineWidth = Math.max(1, s * 0.025);
    for (var eb = 0; eb < 4; eb++) {
      var ey = y - spH * (0.2 + eb * 0.2);
      var ew = spW * (0.9 - eb * 0.08);
      ctx.beginPath(); ctx.moveTo(x - ew, ey); ctx.lineTo(x + ew, ey); ctx.stroke();
    }
    // Broken cap
    ctx.fillStyle = '#b89050';
    ctx.beginPath();
    ctx.moveTo(x - spW * 0.6, y - spH);
    ctx.lineTo(x - spW * 0.15, y - spH - s * 0.1);
    ctx.lineTo(x + spW * 0.4, y - spH - s * 0.05);
    ctx.lineTo(x + spW * 0.6, y - spH);
    ctx.closePath(); ctx.fill();
    // Light edge
    ctx.strokeStyle = 'rgba(255,240,200,0.2)'; ctx.lineWidth = Math.max(1, s * 0.03);
    ctx.beginPath(); ctx.moveTo(x - spW, y); ctx.lineTo(x - spW * 0.6, y - spH); ctx.stroke();

  } else if (type === 'mesa_boulder') {
    // Wide flat-topped layered rock — plains biome
    ctx.globalAlpha = 0.85;
    var mbW = s * (0.45 + r0 * 0.15);
    var mbH = s * (0.3 + r1 * 0.15);
    // Bottom layer — widest
    ctx.fillStyle = '#8b6040';
    ctx.beginPath();
    ctx.moveTo(x - mbW, y); ctx.lineTo(x + mbW, y);
    ctx.lineTo(x + mbW * 0.85, y - mbH * 0.4);
    ctx.lineTo(x - mbW * 0.9, y - mbH * 0.4);
    ctx.closePath(); ctx.fill();
    // Middle layer
    ctx.fillStyle = '#9b7050';
    ctx.beginPath();
    ctx.moveTo(x - mbW * 0.85, y - mbH * 0.4); ctx.lineTo(x + mbW * 0.8, y - mbH * 0.4);
    ctx.lineTo(x + mbW * 0.7, y - mbH * 0.75);
    ctx.lineTo(x - mbW * 0.75, y - mbH * 0.75);
    ctx.closePath(); ctx.fill();
    // Flat top
    ctx.fillStyle = '#a88060';
    ctx.fillRect(x - mbW * 0.7, y - mbH, mbW * 1.4, mbH * 0.25);
    // Strata lines
    ctx.strokeStyle = 'rgba(0,0,0,0.2)'; ctx.lineWidth = Math.max(1, s * 0.02);
    ctx.beginPath(); ctx.moveTo(x - mbW * 0.9, y - mbH * 0.4); ctx.lineTo(x + mbW * 0.85, y - mbH * 0.4); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x - mbW * 0.75, y - mbH * 0.75); ctx.lineTo(x + mbW * 0.7, y - mbH * 0.75); ctx.stroke();

  } else if (type === 'tree_stump') {
    // Dead tree stump — ground biome
    ctx.globalAlpha = 0.8;
    var tsW = s * (0.2 + r0 * 0.08);
    var tsH = s * (0.2 + r1 * 0.1);
    // Trunk
    ctx.fillStyle = '#5a4030';
    ctx.beginPath();
    ctx.moveTo(x - tsW, y); ctx.lineTo(x + tsW, y);
    ctx.lineTo(x + tsW * 0.85, y - tsH); ctx.lineTo(x - tsW * 0.85, y - tsH);
    ctx.closePath(); ctx.fill();
    // Top face (oval with rings)
    ctx.fillStyle = '#7a6050';
    ctx.beginPath(); ctx.ellipse(x, y - tsH, tsW * 0.85, tsW * 0.4, 0, 0, Math.PI * 2); ctx.fill();
    // Growth rings
    ctx.strokeStyle = 'rgba(40,25,15,0.4)'; ctx.lineWidth = Math.max(1, s * 0.02);
    ctx.beginPath(); ctx.ellipse(x, y - tsH, tsW * 0.5, tsW * 0.25, 0, 0, Math.PI * 2); ctx.stroke();
    ctx.beginPath(); ctx.ellipse(x, y - tsH, tsW * 0.25, tsW * 0.12, 0, 0, Math.PI * 2); ctx.stroke();
    // Root tendrils
    ctx.strokeStyle = '#4a3020'; ctx.lineWidth = Math.max(1, s * 0.04); ctx.lineCap = 'round';
    ctx.beginPath(); ctx.moveTo(x - tsW, y); ctx.lineTo(x - tsW * 1.4, y + s * 0.05); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x + tsW, y); ctx.lineTo(x + tsW * 1.3, y + s * 0.07); ctx.stroke();

  } else if (type === 'fallen_log') {
    // Horizontal log on ground — ground biome
    ctx.globalAlpha = 0.75;
    var flW = s * (0.5 + r0 * 0.2);
    var flH = s * (0.12 + r1 * 0.04);
    var flAng = (r0 - 0.5) * 0.6; // slight angle
    ctx.save(); ctx.translate(x, y); ctx.rotate(flAng);
    // Main trunk
    ctx.fillStyle = '#5a4535';
    ctx.beginPath();
    ctx.ellipse(0, 0, flW, flH, 0, 0, Math.PI * 2); ctx.fill();
    // Bark texture lines
    ctx.strokeStyle = 'rgba(30,20,10,0.35)'; ctx.lineWidth = Math.max(1, s * 0.02);
    for (var bl = -3; bl <= 3; bl++) {
      var bx = bl * flW * 0.25;
      ctx.beginPath(); ctx.moveTo(bx, -flH * 0.8); ctx.lineTo(bx, flH * 0.8); ctx.stroke();
    }
    // End cross-section (circle)
    ctx.fillStyle = '#7a6555';
    ctx.beginPath(); ctx.ellipse(flW * 0.9, 0, flH * 1.1, flH * 1.1, 0, 0, Math.PI * 2); ctx.fill();
    ctx.strokeStyle = 'rgba(40,25,15,0.4)'; ctx.lineWidth = Math.max(1, s * 0.015);
    ctx.beginPath(); ctx.ellipse(flW * 0.9, 0, flH * 0.5, flH * 0.5, 0, 0, Math.PI * 2); ctx.stroke();
    ctx.restore();

  } else if (type === 'mushroom') {
    // Cluster of small mushrooms
    ctx.globalAlpha = 0.85;
    var mshCols = ['#8b3020','#a04030','#7a2818'];
    var nMsh = 2 + Math.floor(r0 * 2);
    for (var mi = 0; mi < nMsh; mi++) {
      var mox = (mi - nMsh*0.5 + 0.5) * s * 0.2 + (r1-0.5)*s*0.08;
      var mh = s * (0.15 + ((mi*5+seed*3)%1.0) * 0.12);
      // Stem
      ctx.fillStyle = '#d8c8a0';
      ctx.fillRect(x + mox - s*0.02, y - mh, s*0.04, mh);
      // Cap
      ctx.fillStyle = mshCols[mi % mshCols.length];
      ctx.beginPath(); ctx.ellipse(x + mox, y - mh, s*0.08, s*0.05, 0, Math.PI, Math.PI*2); ctx.fill();
      ctx.beginPath(); ctx.ellipse(x + mox, y - mh, s*0.08, s*0.05, 0, Math.PI, 0); ctx.fill();
      // White spots
      ctx.fillStyle = 'rgba(255,255,255,0.6)';
      ctx.beginPath(); ctx.arc(x + mox - s*0.03, y - mh - s*0.02, s*0.015, 0, Math.PI*2); ctx.fill();
      ctx.beginPath(); ctx.arc(x + mox + s*0.02, y - mh - s*0.01, s*0.012, 0, Math.PI*2); ctx.fill();
    }

  } else if (type === 'fern') {
    // Green fronds radiating from center
    ctx.globalAlpha = 0.7;
    var fernCols = ['#3a6a28','#4a7a38','#2e5a20'];
    var nFronds = 5 + Math.floor(r0 * 3);
    ctx.lineCap = 'round';
    for (var fi2 = 0; fi2 < nFronds; fi2++) {
      var fAng3 = (fi2 / nFronds) * Math.PI * 2 + r1 * 0.5;
      var fLen3 = s * (0.25 + r0 * 0.15);
      var fx = Math.cos(fAng3) * fLen3;
      var fy = Math.sin(fAng3) * fLen3 * 0.4; // perspective squash
      ctx.strokeStyle = fernCols[fi2 % fernCols.length];
      ctx.lineWidth = Math.max(1, s * 0.04);
      ctx.beginPath(); ctx.moveTo(x, y);
      ctx.quadraticCurveTo(x + fx*0.6, y + fy*0.6 - s*0.06, x + fx, y + fy); ctx.stroke();
      // Leaflet ticks
      ctx.lineWidth = Math.max(1, s * 0.02);
      for (var lf2 = 1; lf2 <= 3; lf2++) {
        var lt = lf2 / 4;
        var lx = x + fx * lt, ly2 = y + fy * lt - s*0.06*lt*(1-lt)*4;
        var perpX = -fy * 0.15, perpY = fx * 0.15 * 0.4;
        ctx.beginPath(); ctx.moveTo(lx, ly2); ctx.lineTo(lx + perpX, ly2 + perpY); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(lx, ly2); ctx.lineTo(lx - perpX, ly2 - perpY); ctx.stroke();
      }
    }

  } else if (type === 'leaf_pile') {
    // Heap of autumn-colored leaves
    ctx.globalAlpha = 0.7;
    var leafCols = ['#8a4a1a','#aa6a20','#6a3a10','#c88030','#9a5518','#7a4420'];
    var nLeaves = 7 + Math.floor(r0 * 4);
    for (var li = 0; li < nLeaves; li++) {
      var lox = (((li*7+3)*seed*11)%1.0 - 0.5) * s * 0.5;
      var loy = (((li*5+1)*seed*17)%1.0 - 0.5) * s * 0.25;
      var lr = s * (0.05 + ((li*3+seed*7)%1.0) * 0.06);
      var lAng = ((li*13+seed*19)%1.0) * Math.PI;
      ctx.fillStyle = leafCols[li % leafCols.length];
      ctx.beginPath(); ctx.ellipse(x + lox, y + loy, lr * 1.5, lr * 0.8, lAng, 0, Math.PI * 2); ctx.fill();
    }

  } else if (type === 'moss_patch') {
    // Green ground covering
    ctx.globalAlpha = 0.4;
    ctx.fillStyle = 'rgba(60,120,40,0.35)';
    ctx.beginPath(); ctx.ellipse(x, y, s*0.55, s*0.22, r0*0.5, 0, Math.PI*2); ctx.fill();
    // Dot texture
    ctx.fillStyle = 'rgba(40,100,30,0.3)';
    for (var mi2 = 0; mi2 < 6; mi2++) {
      var mdx = (((mi2*7+seed*11)%1.0) - 0.5) * s * 0.8;
      var mdy = (((mi2*13+seed*17)%1.0) - 0.5) * s * 0.3;
      ctx.beginPath(); ctx.arc(x + mdx, y + mdy, s*0.03, 0, Math.PI*2); ctx.fill();
    }
    // Brighter highlights
    ctx.fillStyle = 'rgba(80,150,50,0.25)';
    ctx.beginPath(); ctx.ellipse(x - s*0.1, y - s*0.04, s*0.2, s*0.08, r1, 0, Math.PI*2); ctx.fill();
  }
  ctx.restore();
}

// Perspective-projects all floorScatter items and draws them with drawFloorItem().
function drawFloorScatter3D() {
  var _fsMaxDist = viewDist * 0.75;
  // A doorway can reveal either stratum from either camera position. The
  // actual terrain/ceiling depth clips scatter; camera state never hides it.
  renderEntities3D(floorScatter, {maxDist: _fsMaxDist, groundAnchor: true,
    bounds: function(item, vis) {
      var size = Math.max(6, Math.min(70, Math.floor(1500 * getScale3D(FLOOR_ITEM_TIER[item.type] || 'medium') / vis.fwd)));
      // Covers the widest sticks, vertical plants, and stroked edges.
      return {x:vis.sx-size*2-2,y:vis.sy-size*2-2,width:size*4+4,height:size*4+4};
    }, fadeFraction: 0.3, sort: true, minDist: 3, mode3dOnly: true},
    function(item, vis, C, ctx, now) {
      var fwd = vis.fwd;
      var tier = FLOOR_ITEM_TIER[item.type] || 'medium';
      var s3 = getScale3D(tier);
      var size = Math.max(6, Math.min(70, Math.floor(1500 * s3 / fwd)));
      var spawnFade = item.spawnMs ? Math.min(1, (now - item.spawnMs) / 500) : 1;
      // Continuous fog matching wall style — no hard floor so far items vanish smoothly
      var fogAlpha = (1.0 - fwd / _fsMaxDist * 0.65) * vis.fade * spawnFade;
      // Apply night darkness
      var _floorItemLight = (ambientLight < 0.85) ? (0.3 + ambientLight * 0.7) : 1.0;
      ctx.save(); ctx.globalAlpha = Math.max(0, fogAlpha) * _floorItemLight;
      drawFloorItem(item.type, item.variant, item.seed, vis.sx, vis.sy, size);
      ctx.restore();
    });
}

// Render treasure chests as billboarded sprites in the 3D view.
// 3D projected treasure chests with lid-open animation and floating loot text.
function drawTreasureChests3D() {
  if (!treasureChests || !treasureChests.length || !MODE3D) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var now = Date.now();

  function proj(wx, wy, wz) {
    var p = projToScreen(wx, wy, wz, C) || {sx:0,sy:0,fwd:0};
    // Retain world vertices for near-plane clipping of complete box faces.
    p.x = wx; p.y = wy; p.z = wz;
    return p;
  }

  function chestFace(p0, p1, p2, p3) {
    return projectSceneWorldPolygon([p0,p1,p2,p3], C);
  }

  // Draw a quad from 4 projected points
  function fillQuad(p0, p1, p2, p3, color) {
    ctx.fillStyle = color;
    var face = chestFace(p0,p1,p2,p3);
    withSceneDepthClip(face, function () { traceSceneDepthPolygon(face); ctx.fill(); });
  }

  // Chest world dimensions — sized relative to wall cells (cell=12) so they
  // look proportionate.  bodyH is in projected-Z units (same space as cameraZ ≈ 60).
  var hw = 7, hd = 5;       // half-width, half-depth in world XY pixels
  var bodyH = 8, lidH = 3;  // Z heights — about half a wall height

  // Stroke helper for edge outlines
  function strokeQuad(p0, p1, p2, p3, color, lw) {
    ctx.strokeStyle = color; ctx.lineWidth = lw; ctx.lineJoin = 'round';
    var face = chestFace(p0,p1,p2,p3);
    withSceneDepthClip(face, function () { traceSceneDepthPolygon(face); ctx.stroke(); });
  }

  // Collect visible chests and sort far-to-near for correct painter's algorithm
  var visibleChests = [];
  for (var i = 0; i < treasureChests.length; i++) {
    var ch = treasureChests[i];
    if (ch.collected) continue;
    var vis = entityVisible3D(ch.x, ch.y, getEntityRenderFloorZ(ch), C,
      { maxDist: viewDist * 0.7, skipDepth: true, sceneDepth: true, fadeFraction: 0.8 });
    if (!vis || vis.dist < 3) continue;
    visibleChests.push({ch: ch, vis: vis});
  }
  visibleChests.sort(function(a, b) { return b.vis.dist - a.vis.dist; });
  for (var ci = 0; ci < visibleChests.length; ci++) {
    var ch = visibleChests[ci].ch, vis = visibleChests[ci].vis;
    var centerSX = vis.sx, fwd = vis.fwd, floorZ = getEntityRenderFloorZ(ch);
    var dx = ch.x - cam.x, dy = ch.y - cam.y;
    var lid = ch.lidAngle || 0;

    var fogAlpha = Math.max(0.5, 1.0 - fwd / viewDist * 0.4) * vis.fade;
    ctx.save(); ctx.globalAlpha = fogAlpha;

    // Subtle glow halo — color/intensity from chest tier
    var _ctd = CHEST_TIER_DEFS[ch.tier || 'common'];
    var glowSY = horizonY + (cameraZ - floorZ - bodyH * 0.5) / fwd * projScale;
    var glowR = Math.max(14, Math.min(70, 20 * getScale3D('lgGlow') * projScale / fwd));
    ctx.globalAlpha = fogAlpha * _ctd.glowA;
    ctx.fillStyle = _ctd.glow;
    withSceneDepthBillboard({x:centerSX-glowR,y:glowSY-glowR,width:glowR*2,height:glowR*2}, fwd, function () {
      ctx.beginPath(); ctx.arc(centerSX, glowSY, glowR, 0, Math.PI * 2); ctx.fill();
    });
    ctx.globalAlpha = fogAlpha;

    // ── 3D box corners — fixed world-space orientation ──
    var facingAng = ch.facing || 0;
    var cosF = Math.cos(facingAng), sinF = Math.sin(facingAng);
    var fwX = cosF, fwY = sinF;   // chest "front" direction (fixed in world)
    var rtX = -sinF, rtY = cosF;  // right (perpendicular)

    // 4 corners in world space: front-left, front-right, back-right, back-left
    // "front" = the chest's fixed front face (where the lock is)
    var c0x = ch.x + fwX*hd - rtX*hw, c0y = ch.y + fwY*hd - rtY*hw;  // front-left
    var c1x = ch.x + fwX*hd + rtX*hw, c1y = ch.y + fwY*hd + rtY*hw;  // front-right
    var c2x = ch.x - fwX*hd + rtX*hw, c2y = ch.y - fwY*hd + rtY*hw;  // back-right
    var c3x = ch.x - fwX*hd - rtX*hw, c3y = ch.y - fwY*hd - rtY*hw;  // back-left

    var bodyTopZ = floorZ + bodyH;
    var lidTopZ = bodyTopZ + lidH;
    // Lid: front edge (hinge at back) rises when open
    var lidFrontZ = bodyTopZ + lidH + lid * lidH * 1.8;
    var lidPush = lid * hd * 0.4;

    var edgeLW = Math.max(1, Math.min(3, 2.5 * projScale / fwd));

    // Project all 8 body corners
    var p0b = proj(c0x, c0y, floorZ), p1b = proj(c1x, c1y, floorZ);
    var p2b = proj(c2x, c2y, floorZ), p3b = proj(c3x, c3y, floorZ);
    var p0t = proj(c0x, c0y, bodyTopZ), p1t = proj(c1x, c1y, bodyTopZ);
    var p2t = proj(c2x, c2y, bodyTopZ), p3t = proj(c3x, c3y, bodyTopZ);

    // Need at least some corners visible
    if (!p0b || !p1b || !p0t || !p1t) { ctx.restore(); continue; }

    // Determine which faces the camera can see using dot products
    // Camera-to-chest vector projected onto chest's forward and right axes
    var camDotFw = dx * fwX + dy * fwY;   // positive = camera is in front
    var camDotRt = dx * rtX + dy * rtY;   // positive = camera is to the right

    // Face colors from chest tier — front brightest, back darkest, sides medium
    var colFront = _ctd.body, colBack = _ctd.bodyDk;
    var colRight = _ctd.bodyR, colLeft = _ctd.bodyL;
    var edgeCol = _ctd.edge;

    // ── Painter's order: draw far faces first, near faces last ──

    // Front face (c0-c1) — visible when camera is in front (camDotFw > 0)
    // Back face (c2-c3) — visible when camera is behind (camDotFw < 0)
    // Right face (c1-c2) — visible when camera is to the right (camDotRt > 0)
    // Left face (c3-c0) — visible when camera is to the left (camDotRt < 0)

    // Draw the two far faces first, then the two near faces
    if (camDotFw > 0) {
      // Camera in front — draw back face first (far), front face last (near)
      if (p2b && p3b && p2t && p3t) { fillQuad(p3b,p2b,p2t,p3t, colBack); strokeQuad(p3b,p2b,p2t,p3t, edgeCol, edgeLW); }
    } else {
      // Camera behind — draw front face first (far), back face last (near)
      fillQuad(p0b,p1b,p1t,p0t, colFront); strokeQuad(p0b,p1b,p1t,p0t, edgeCol, edgeLW);
    }

    if (camDotRt > 0) {
      // Camera to right — draw left face first (far), right face last (near)
      if (p3b && p0b && p3t && p0t) { fillQuad(p3b,p0b,p0t,p3t, colLeft); strokeQuad(p3b,p0b,p0t,p3t, edgeCol, edgeLW); }
    } else {
      // Camera to left — draw right face first (far), left face last (near)
      if (p1b && p2b && p1t && p2t) { fillQuad(p1b,p2b,p2t,p1t, colRight); strokeQuad(p1b,p2b,p2t,p1t, edgeCol, edgeLW); }
    }

    // Now draw the near faces on top
    if (camDotRt > 0) {
      if (p1b && p2b && p1t && p2t) { fillQuad(p1b,p2b,p2t,p1t, colRight); strokeQuad(p1b,p2b,p2t,p1t, edgeCol, edgeLW); }
    } else {
      if (p3b && p0b && p3t && p0t) { fillQuad(p3b,p0b,p0t,p3t, colLeft); strokeQuad(p3b,p0b,p0t,p3t, edgeCol, edgeLW); }
    }

    if (camDotFw > 0) {
      // Front is near — draw it last with trim/lock details
      fillQuad(p0b,p1b,p1t,p0t, colFront); strokeQuad(p0b,p1b,p1t,p0t, edgeCol, edgeLW);
    } else {
      if (p2b && p3b && p2t && p3t) { fillQuad(p3b,p2b,p2t,p3t, colBack); strokeQuad(p3b,p2b,p2t,p3t, edgeCol, edgeLW); }
    }

    // Gold trim band on the front face (always on front regardless of camera)
    var trimBot = proj(c0x, c0y, floorZ + bodyH * 0.38);
    var trimTop = proj(c0x, c0y, floorZ + bodyH * 0.55);
    var trimBotR = proj(c1x, c1y, floorZ + bodyH * 0.38);
    var trimTopR = proj(c1x, c1y, floorZ + bodyH * 0.55);
    if (camDotFw > 0 && trimBot && trimTop && trimBotR && trimTopR) {
      fillQuad(trimBot, trimBotR, trimTopR, trimTop, _ctd.trim);
      withSceneDepthClip(chestFace(trimBot,trimBotR,trimTopR,trimTop), function () {
        ctx.strokeStyle = _ctd.lock; ctx.lineWidth = Math.max(1, edgeLW * 0.6);
        ctx.beginPath(); ctx.moveTo(trimTop.sx, trimTop.sy); ctx.lineTo(trimTopR.sx, trimTopR.sy); ctx.stroke();
      });
    }

    // Vertical center strap on front
    var bandMidX = ch.x + fwX * hd, bandMidY = ch.y + fwY * hd;
    if (camDotFw > 0) {
      var bL = proj(bandMidX - rtX*hw*0.15, bandMidY - rtY*hw*0.15, floorZ);
      var bR = proj(bandMidX + rtX*hw*0.15, bandMidY + rtY*hw*0.15, floorZ);
      var bLt = proj(bandMidX - rtX*hw*0.15, bandMidY - rtY*hw*0.15, bodyTopZ);
      var bRt = proj(bandMidX + rtX*hw*0.15, bandMidY + rtY*hw*0.15, bodyTopZ);
      if (bL && bR && bLt && bRt) fillQuad(bL, bR, bRt, bLt, _ctd.trim);
    }

    // Lock circle on front
    if (camDotFw > 0) {
      var lockP = proj(bandMidX, bandMidY, floorZ + bodyH * 0.47);
      if (lockP.fwd >= 1) {
        var lockR = Math.max(2, Math.min(5, 3 * projScale / lockP.fwd));
        withSceneDepthBillboard({x:lockP.sx-lockR-2,y:lockP.sy-lockR-2,width:lockR*2+4,height:lockR*2+4}, lockP.fwd, function () {
          ctx.fillStyle = _ctd.lock;
          ctx.beginPath(); ctx.arc(lockP.sx, lockP.sy, lockR, 0, Math.PI * 2); ctx.fill();
          ctx.strokeStyle = _ctd.lockStr; ctx.lineWidth = Math.max(1, lockR * 0.4);
          ctx.stroke();
        });
      }
    }

    // ── Top face / interior ──
    if (lid < 0.05) {
      if (p0t && p1t && p2t && p3t) {
        fillQuad(p0t, p1t, p2t, p3t, _ctd.lidClosed);
        strokeQuad(p0t, p1t, p2t, p3t, edgeCol, edgeLW);
      }
    } else {
      if (p0t && p1t && p2t && p3t) {
        fillQuad(p0t, p1t, p2t, p3t, '#1a1008');
        ctx.globalAlpha = fogAlpha * (0.4 + 0.2 * Math.sin(now * 0.004));
        fillQuad(p0t, p1t, p2t, p3t, _ctd.glow);
        ctx.globalAlpha = fogAlpha;
        strokeQuad(p0t, p1t, p2t, p3t, edgeCol, edgeLW);
      }
    }

    // ── Lid (hinged at back edge c3-c2, front edge c0-c1 rises) ──
    var lidC0x = c0x + fwX * lidPush, lidC0y = c0y + fwY * lidPush;
    var lidC1x = c1x + fwX * lidPush, lidC1y = c1y + fwY * lidPush;
    var pLidC0 = proj(lidC0x, lidC0y, lidFrontZ);
    var pLidC1 = proj(lidC1x, lidC1y, lidFrontZ);
    var pLidC2 = proj(c2x, c2y, lidTopZ);  // hinge corner
    var pLidC3 = proj(c3x, c3y, lidTopZ);  // hinge corner
    if (pLidC0 && pLidC1 && pLidC2 && pLidC3) {
      fillQuad(pLidC0, pLidC1, pLidC2, pLidC3, _ctd.lidTop);
      strokeQuad(pLidC0, pLidC1, pLidC2, pLidC3, edgeCol, edgeLW);
      // Trim on front edge of lid (the opening edge)
      withSceneDepthClip(chestFace(pLidC0,pLidC1,pLidC2,pLidC3), function () {
        ctx.strokeStyle = _ctd.trim; ctx.lineWidth = Math.max(1, edgeLW * 1.2);
        if (pLidC0.fwd >= 1 && pLidC1.fwd >= 1) {
          ctx.beginPath(); ctx.moveTo(pLidC0.sx, pLidC0.sy); ctx.lineTo(pLidC1.sx, pLidC1.sy); ctx.stroke();
        }
      });
    }

    // Small sparkle
    var shimmer = 0.5 + 0.5 * Math.sin(now * 0.005 + (ch.seed || i) * 10);
    ctx.globalAlpha = fogAlpha * shimmer * 0.5;
    ctx.fillStyle = _ctd.spark;
    var sparkP = proj(ch.x, ch.y, bodyTopZ + lidH + 3);
    if (sparkP.fwd >= 1) {
      var sparkR = Math.max(1, Math.min(3, 2 * projScale / sparkP.fwd));
      withSceneDepthBillboard({x:sparkP.sx-sparkR,y:sparkP.sy-sparkR,width:sparkR*2,height:sparkR*2}, sparkP.fwd, function () {
        ctx.beginPath(); ctx.arc(sparkP.sx, sparkP.sy, sparkR, 0, Math.PI * 2); ctx.fill();
      });
    }

    // ── Epic chest orbiting particles ──
    if ((ch.tier === 'epic') && sparkP) {
      ctx.globalAlpha = fogAlpha * 0.8;
      for (var epi = 0; epi < 4; epi++) {
        var epAng = now * 0.003 + epi * Math.PI * 0.5;
        var epR = hw * 1.5;
        var epX = ch.x + Math.cos(epAng) * epR;
        var epY = ch.y + Math.sin(epAng) * epR;
        var epZ = floorZ + bodyH * 0.5 + Math.sin(now * 0.005 + epi) * 4;
        var epP = proj(epX, epY, epZ);
        if (epP.fwd >= 1) {
          var epSz = Math.max(1.5, Math.min(4, 2.5 * projScale / epP.fwd));
          withSceneDepthBillboard({x:epP.sx-epSz-12,y:epP.sy-epSz-12,width:epSz*2+24,height:epSz*2+24}, epP.fwd, function () {
            ctx.fillStyle = _ctd.glow;
            ctx.shadowBlur = 6; ctx.shadowColor = _ctd.glow;
            ctx.beginPath(); ctx.arc(epP.sx, epP.sy, epSz, 0, Math.PI * 2); ctx.fill();
          });
        }
      }
      ctx.shadowBlur = 0;
      ctx.globalAlpha = fogAlpha;
    }

    // ── Floating loot text (when chest is open) ──
    if (ch.opened && lid > 0.3) {
      var textP = proj(ch.x, ch.y, floorZ + bodyH + lidH + 8);
      if (textP && textP.fwd > 2) {
        var fontSize = Math.max(8, Math.min(14, Math.floor(1800 / textP.fwd)));
        var lineH = fontSize + 2;
        var textLines = [];
        var textColors = [];

        if (ch.relicId && RELIC_DEFS[ch.relicId]) {
          // Relic in epic chest
          var rDef = RELIC_DEFS[ch.relicId];
          textLines.push(rDef.name);
          textColors.push(RARITY_COLORS.epic);
          textLines.push(rDef.desc);
          textColors.push('#ddbbff');
          if (equipment.relic) {
            textLines.push('Replaces: ' + equipment.relic.name);
            textColors.push('#888877');
            textLines.push('[E] Swap');
          } else {
            textLines.push('[E] Take');
          }
          textColors.push('#c040ff');
        } else if (ch.equipId && EQUIPMENT_DEFS[ch.equipId]) {
          // Equipment with quality
          var eInst = createEquipInstance(ch.equipId, ch.quality || 1.0);
          textLines.push(eInst.displayName);
          textColors.push(RARITY_COLORS[eInst.rarity] || '#ffffff');
          textLines.push(eInst.desc);
          textColors.push('#ccccbb');
          if (eInst.quality && Math.abs(eInst.quality - 1.0) > 0.04) {
            textLines.push('Quality: ' + Math.round(eInst.quality * 100) + '%');
            textColors.push(eInst.quality >= 1.0 ? '#80ff80' : '#ff8080');
          }
          var currentItem = equipment[eInst.slot];
          if (currentItem && currentItem.id !== eInst.id) {
            textLines.push('Replaces: ' + (currentItem.displayName || currentItem.name));
            textColors.push('#888877');
            var statDelta = getEquipStatDelta(eInst, currentItem);
            if (statDelta) { textLines.push(statDelta.text); textColors.push(statDelta.color); }
            textLines.push('[E] Swap');
          } else {
            textLines.push('[E] Take');
          }
          textColors.push('#ffd700');
        } else {
          textLines.push('+' + ch.gold + ' Gold');
          textColors.push('#ffd700');
          textLines.push('[E] Take');
          textColors.push('#ffd700');
        }

        // Draw backdrop
        ctx.globalAlpha = fogAlpha * 0.85;
        var maxTxtW = 0;
        ctx.font = 'bold ' + fontSize + 'px Arial';
        for (var ti = 0; ti < textLines.length; ti++) {
          var tw = ctx.measureText(textLines[ti]).width;
          if (tw > maxTxtW) maxTxtW = tw;
        }
        var bgW = maxTxtW + 12, bgH = textLines.length * lineH + 6;
        withSceneDepthBillboard({x:textP.sx-bgW/2,y:textP.sy-bgH+2,width:bgW,height:bgH+2}, textP.fwd, function () {
          ctx.fillStyle = 'rgba(0,0,0,0.65)';
          ctx.fillRect(textP.sx - bgW / 2, textP.sy - bgH + 2, bgW, bgH);

        // Draw text lines
        ctx.textAlign = 'center';
        for (var tl = 0; tl < textLines.length; tl++) {
          var isAction = (textLines[tl].indexOf('[E]') >= 0);
          var pulse = isAction ? (0.7 + 0.3 * Math.sin(now * 0.005)) : 1.0;
          ctx.globalAlpha = fogAlpha * pulse;
          ctx.fillStyle = textColors[tl];
          ctx.font = (tl === 0 ? 'bold ' : '') + fontSize + 'px Arial';
          ctx.fillText(textLines[tl], textP.sx, textP.sy - bgH + 2 + (tl + 1) * lineH);
        }
        });
      }
    }

    ctx.restore();
  }
}

// Compare two equipment items and return a stat delta string
function getEquipStatDelta(newDef, oldDef) {
  var parts = [];
  function cmp(prop, label, pct) {
    var nv = newDef[prop] || 0, ov = oldDef[prop] || 0;
    if (nv === ov) return;
    var delta = nv - ov;
    var sign = delta > 0 ? '+' : '';
    parts.push(sign + (pct ? Math.round(delta * 100) + '%' : delta.toFixed(0)) + ' ' + label);
  }
  cmp('damageReduction', 'dmg reduction', true);
  cmp('manaRegen', 'mana/s', false);
  cmp('hpRegen', 'HP/s', false);
  cmp('speedBonus', 'speed', true);
  cmp('spellDmgBonus', 'spell dmg', true);
  cmp('manaCostReduction', 'mana cost', true);
  if (parts.length === 0) return null;
  var allPositive = parts.every(function(p) { return p.charAt(0) === '+'; });
  return {text: parts.join(', '), color: allPositive ? '#66cc66' : '#cc9944'};
}

// Render enemy spawner obelisks as dark pulsing pillars with a red glow.
function drawEnemySpawners3D() {
  // Filter inactive spawners before passing to renderer
  var activeSpawners = [];
  if (enemySpawners) {
    for (var i = 0; i < enemySpawners.length; i++) {
      if (enemySpawners[i].active) activeSpawners.push(enemySpawners[i]);
    }
  }
  renderEntities3D(activeSpawners, {maxDist: viewDist * 0.5, groundAnchor: true,
    bounds: function(sp, vis) {
      var size = Math.max(12, Math.min(120, Math.floor(40 * getScale3D('lgStructure') * projScale / vis.fwd)));
      return {x:vis.sx-size*2-2,y:vis.sy-size*2.5-4,width:size*4+4,height:size*4+8};
    }, fadeFraction: 0.8, sort: true, minDist: 3, mode3dOnly: true},
    function(sp, vis, C, ctx, now) {
      var screenX = vis.sx, screenY = vis.sy, fwd = vis.fwd;
      var size = Math.max(12, Math.min(120, Math.floor(40 * getScale3D('lgStructure') * projScale / fwd)));
      var fogAlpha = Math.max(0.4, 1.0 - fwd / (viewDist * 0.5) * 0.5) * vis.fade;
      var pulse = 0.7 + 0.3 * Math.sin(now * 0.004 + (sp.pulsePhase || 0));
      var hpFrac = sp.hp / sp.maxHp;
      ctx.save(); ctx.globalAlpha = fogAlpha;

      var glowR = size * 2 * pulse;
      var glowGrad = ctx.createRadialGradient(screenX, screenY - size * 0.5, size * 0.2, screenX, screenY - size * 0.5, glowR);
      glowGrad.addColorStop(0, 'rgba(200,40,40,' + (0.3 * pulse).toFixed(2) + ')');
      glowGrad.addColorStop(1, 'rgba(200,40,40,0)');
      ctx.fillStyle = glowGrad;
      ctx.fillRect(screenX - glowR, screenY - size * 0.5 - glowR, glowR * 2, glowR * 2);

      var bw = size * 0.3, bh = size * 1.2;
      ctx.fillStyle = '#1a1520';
      ctx.beginPath();
      ctx.moveTo(screenX - bw, screenY); ctx.lineTo(screenX + bw, screenY);
      ctx.lineTo(screenX + bw * 0.6, screenY - bh); ctx.lineTo(screenX - bw * 0.6, screenY - bh);
      ctx.closePath(); ctx.fill();

      ctx.strokeStyle = 'rgba(200,50,50,' + (pulse * 0.8).toFixed(2) + ')';
      ctx.lineWidth = Math.max(1, size * 0.06);
      for (var rb = 0; rb < 3; rb++) {
        var ry = screenY - bh * (0.2 + rb * 0.25);
        var rw = bw * (0.9 - rb * 0.1);
        ctx.beginPath(); ctx.moveTo(screenX - rw, ry); ctx.lineTo(screenX + rw, ry); ctx.stroke();
      }
      ctx.fillStyle = 'rgba(255,60,60,' + (pulse * 0.9).toFixed(2) + ')';
      ctx.beginPath();
      ctx.arc(screenX, screenY - bh * 0.65, size * 0.1 * pulse, 0, Math.PI * 2);
      ctx.fill();

      if (sp.hp < sp.maxHp) {
        var barW = size * 0.8, barH = Math.max(2, size * 0.08);
        var barY = screenY - bh - barH - 4;
        ctx.fillStyle = 'rgba(0,0,0,0.6)';
        ctx.fillRect(screenX - barW * 0.5, barY, barW, barH);
        ctx.fillStyle = hpFrac > 0.5 ? '#44cc44' : hpFrac > 0.25 ? '#ccaa22' : '#cc2222';
        ctx.fillRect(screenX - barW * 0.5, barY, barW * hpFrac, barH);
      }
      ctx.restore();
    });
}

// Renders breakable ore vein overlays on cave walls.
// Attach to the same real exposed face as wall decorations; read opaque scene
// depth so neither mineral nor glow can shine through the cave roof.
function drawOreVeins() {
  if (!oreVeins || !oreVeins.length || !MODE3D) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonY = C.horizonY, cameraZ = C.cameraZ;
  var now = Date.now();

  // Collect visible ore veins and sort far-to-near for correct painter's algorithm
  var visibleOres = [];
  for (var i = 0; i < oreVeins.length; i++) {
    var ore = oreVeins[i];
    var faceX = (ore.gx + (ore.side === 'west' ? 0 : ore.side === 'east' ? 1 : 0.5)) * cell;
    var faceY = (ore.gy + (ore.side === 'north' ? 0 : ore.side === 'south' ? 1 : 0.5)) * cell;
    var attachment = getWallDecorationAttachment({gridX:ore.gx,gridY:ore.gy,
      worldX:faceX,worldY:faceY,side:ore.side,type:'ore'});
    if (!attachment || (cam.x-attachment.x)*attachment.nx+(cam.y-attachment.y)*attachment.ny <= 0) continue;
    var vis = entityVisible3D(attachment.x, attachment.y, attachment.z, C,
      { maxDist: 280, sceneDepth: true, fadeFraction: 0.8 });
    if (!vis) continue;
    visibleOres.push({ore: ore, vis: vis, attachment:attachment});
  }
  visibleOres.sort(function(a, b) { return b.vis.dist - a.vis.dist; });
  for (var oi = 0; oi < visibleOres.length; oi++) {
    var ore = visibleOres[oi].ore, vis = visibleOres[oi].vis;
    var screenX = vis.sx, screenY = vis.sy, fwd = vis.fwd, dist = vis.dist;
    var _oFade = vis.fade;
    var wallMidY = screenY;

    // Perspective-scaled size
    var size = Math.min(visibleOres[oi].attachment.size,cell*0.45) / fwd * projScale;
    if (size < 0.5) continue;

    // hp fade: full at 3, 65% at 2, 35% at 1 (cracked look)
    var hpAlpha = ore.hp >= 3 ? 1.0 : ore.hp === 2 ? 0.65 : 0.35;
    // Shimmer flicker
    var flicker = 0.82 + 0.18 * Math.sin(now * 0.007 + ore.gx * 5.3 + ore.gy * 3.7);

    withSceneDepthBillboard({x:screenX-size*1.6-2,y:wallMidY-size*1.6-2,
      width:size*3.2+4,height:size*3.2+4}, fwd, function () {
    ctx.save();
    ctx.globalAlpha = hpAlpha * flicker * _oFade;

    var col1, col2, glowCol;
    if (ore.veinType === 'gold') {
      col1 = '#c8a000'; col2 = '#ffe066'; glowCol = 'rgba(255,200,0,0.25)';
    } else {
      col1 = '#00a89a'; col2 = '#00ddc8'; glowCol = 'rgba(0,220,200,0.2)';
    }

    // Glow halo behind vein
    ctx.globalAlpha = hpAlpha * flicker * 0.3;
    ctx.fillStyle = glowCol;
    ctx.beginPath(); ctx.arc(screenX, wallMidY, size * 1.6, 0, Math.PI * 2); ctx.fill();

    // Jagged vein polygon — mimics a mineral seam cutting across the wall face
    ctx.globalAlpha = hpAlpha * flicker;
    ctx.fillStyle = col1;
    var s = size;
    ctx.beginPath();
    ctx.moveTo(screenX - s,      wallMidY - s * 0.1);
    ctx.lineTo(screenX - s*0.5,  wallMidY - s * 0.55);
    ctx.lineTo(screenX + s*0.1,  wallMidY - s * 0.35);
    ctx.lineTo(screenX + s*0.6,  wallMidY - s * 0.7);
    ctx.lineTo(screenX + s,      wallMidY - s * 0.2);
    ctx.lineTo(screenX + s*0.7,  wallMidY + s * 0.4);
    ctx.lineTo(screenX + s*0.15, wallMidY + s * 0.2);
    ctx.lineTo(screenX - s*0.4,  wallMidY + s * 0.55);
    ctx.closePath(); ctx.fill();

    // Bright accent veins inside
    ctx.fillStyle = col2;
    ctx.globalAlpha = hpAlpha * flicker * 0.75;
    ctx.beginPath();
    ctx.moveTo(screenX - s*0.5, wallMidY - s*0.4);
    ctx.lineTo(screenX + s*0.0, wallMidY - s*0.55);
    ctx.lineTo(screenX + s*0.3, wallMidY + s*0.1);
    ctx.lineTo(screenX - s*0.1, wallMidY + s*0.25);
    ctx.closePath(); ctx.fill();

    // If hp < 3: draw crack lines over the vein
    if (ore.hp < 3) {
      ctx.strokeStyle = 'rgba(0,0,0,0.6)';
      ctx.lineWidth = Math.max(1, Math.floor(size / 10));
      ctx.globalAlpha = hpAlpha;
      ctx.beginPath();
      ctx.moveTo(screenX - s*0.2, wallMidY - s*0.6);
      ctx.lineTo(screenX + s*0.3, wallMidY + s*0.3);
      if (ore.hp < 2) {
        ctx.moveTo(screenX + s*0.4, wallMidY - s*0.4);
        ctx.lineTo(screenX - s*0.3, wallMidY + s*0.5);
      }
      ctx.stroke();
    }

    ctx.restore();
    }, {depthBias:1.5});
  }
}

function drawWallDecorations() {
  if (!wallDecorations || wallDecorations.length === 0) return;
  var C = getCam3D();
  var w = C.w, h = C.h, cosAng = C.cosAng, sinAng = C.sinAng;
  var invTanHalf = C.invTanHalf, horizonYd = C.horizonY, cameraZd = C.cameraZ;
  var visibleCount = 0, occludedCount = 0, culledCount = 0;
  var now = Date.now();
  var shouldLog = DEBUG_DECORATIONS && (now - __decorDebugLast > 1000);

  // Collect visible decorations and sort far-to-near for correct painter's algorithm
  var visibleDecors = [];
  for (var i = 0; i < wallDecorations.length; i++) {
    var dec = wallDecorations[i];
    var attachment = getWallDecorationAttachment(dec);
    if (!attachment) continue;
    var dx = attachment.x - cam.x;
    var dy = attachment.y - cam.y;
    var dist = Math.hypot(dx, dy);
    if (dist < 1 || dist > viewDist * 0.7) continue;
    var _dFade = dist > viewDist * 0.56 ? Math.max(0, 1.0 - (dist - viewDist * 0.56) / (viewDist * 0.14)) : 1.0;

    var fwd = dx * cosAng + dy * sinAng;
    if (fwd < 1) { culledCount++; continue; }
    var rgt = dx * (-sinAng) + dy * cosAng;
    var screenX = Math.floor((rgt / fwd * invTanHalf * 0.5 + 0.5) * w);
    if (screenX < -40 || screenX > w + 40) { culledCount++; continue; }

    visibleDecors.push({dec: dec, attachment:attachment, dx: dx, dy: dy, dist: dist, fwd: fwd, screenX: screenX, _dFade: _dFade});
  }
  visibleDecors.sort(function(a, b) { return b.dist - a.dist; });
  for (var di = 0; di < visibleDecors.length; di++) {
    var dec = visibleDecors[di].dec, dist = visibleDecors[di].dist, fwd = visibleDecors[di].fwd;
    var screenX = visibleDecors[di].screenX, _dFade = visibleDecors[di]._dFade;
    visibleCount++;
    var renderX = Math.max(0, Math.min(w - 1, screenX));
    var wallNormalX = 0, wallNormalY = 0;
    if (dec.side === 'north') wallNormalY = -1;
    else if (dec.side === 'south') wallNormalY = 1;
    else if (dec.side === 'west') wallNormalX = -1;
    else if (dec.side === 'east') wallNormalX = 1;

    var toCamX = cam.x - dec.worldX;
    var toCamY = cam.y - dec.worldY;
    var toCamLen = Math.hypot(toCamX, toCamY);
    if (toCamLen > 0) { toCamX /= toCamLen; toCamY /= toCamLen; }
    var viewAngle = toCamX * wallNormalX + toCamY * wallNormalY;
    if (viewAngle <= 0) continue;
    var attachment = visibleDecors[di].attachment;
    // Perspective scaling has no minimum pixel size: a far torch must not
    // grow beyond the wall carrying it. Z and size are shared with its glow.
    var size = attachment.size / fwd * projScale;
    if (size < 0.5) continue;
    var decorZ = attachment.z;
    var decorY = horizonYd + (cameraZd - decorZ) / fwd * projScale;
    var depthPoly = [
      {x:renderX-size*2,y:decorY-size*3,depth:fwd},
      {x:renderX+size*2,y:decorY-size*3,depth:fwd},
      {x:renderX+size*2,y:decorY+size*3,depth:fwd},
      {x:renderX-size*2,y:decorY+size*3,depth:fwd}
    ];
    withSceneDepthClip(depthPoly, function() {
      drawWallAlignedDecoration(dec.type, renderX, decorY, size, dist, dec.side, viewAngle, _dFade);
    }, {depthBias:1.5});
  }
  if (shouldLog) {
    //console.log('[DECOR] Summary: ' + visibleCount + ' visible, ' + occludedCount + ' occluded, ' + culledCount + ' culled'); // TEMP DISABLED
    __decorDebugLast = now;
  }
}
