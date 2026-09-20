// Frame delivery and CPU work are different measurements. Record actual rAF
// intervals; 2ms of JavaScript on a 60Hz display is not 500 displayed FPS.
var _perfCadenceIntervals = new Float64Array(120);
var _perfCadenceCount = 0, _perfCadenceIndex = 0;
var _perfCadenceLast = null, _perfCadenceGeneration = null;
function recordPerfFrameCadence(timestamp, generation) {
  if (!Number.isFinite(timestamp)) return;
  if (generation !== _perfCadenceGeneration) {
    _perfCadenceGeneration = generation;
    _perfCadenceLast = null; _perfCadenceCount = _perfCadenceIndex = 0;
  }
  if (_perfCadenceLast !== null && timestamp > _perfCadenceLast) {
    _perfCadenceIntervals[_perfCadenceIndex] = timestamp - _perfCadenceLast;
    _perfCadenceIndex = (_perfCadenceIndex + 1) % _perfCadenceIntervals.length;
    _perfCadenceCount = Math.min(_perfCadenceCount + 1, _perfCadenceIntervals.length);
  }
  _perfCadenceLast = timestamp;
}
function getPerfFrameCadence() {
  var total = 0;
  for (var i = 0; i < _perfCadenceCount; i++) total += _perfCadenceIntervals[i];
  return {samples:_perfCadenceCount, meanMs:_perfCadenceCount ? total / _perfCadenceCount : 0,
    fps:total > 0 ? _perfCadenceCount * 1000 / total : 0};
}

function caveControlMapVisible() {
  var fullscreen = document.fullscreenElement || document.webkitFullscreenElement ||
    document.mozFullScreenElement || document.msFullscreenElement;
  return !!(CAVE_TEST_MODE && DEBUG_CAVE && fullscreen !== canvas);
}

function _pt(name, fn) {
  var t0 = performance.now();
  try {
    fn();
  } catch (e) {
    if (!_ptErrors[name]) _ptErrors[name] = 0;
    _ptErrors[name]++;
    if (_ptErrors[name] <= 3 || _ptErrors[name] % 200 === 0) {
      console.error('[RENDER CRASH in ' + name + ' #' + _ptErrors[name] + ']', e.message, '\n', e.stack);
    }
  }
  var elapsed = performance.now() - t0;
  if (!_perfBreakdown[name]) _perfBreakdown[name] = {total:0, count:0, max:0};
  _perfBreakdown[name].total += elapsed;
  _perfBreakdown[name].count++;
  if (elapsed > _perfBreakdown[name].max) _perfBreakdown[name].max = elapsed;
  // Ring buffer write — accumulate in case _pt is called twice for same name
  var arr = _perfStageHistory[name];
  if (!arr) { arr = new Float32Array(PERF_HISTORY_LEN); _perfStageHistory[name] = arr; }
  arr[_perfRingIdx] += elapsed;
  // Per-second aggregate: accumulate sum; drawPerfHud divides by frame count.
  var sArr = _perfSecStages[name];
  if (!sArr) { sArr = new Float32Array(PERF_SEC_LEN); _perfSecStages[name] = sArr; }
  sArr[_perfSecIdx] += elapsed;
}

// On-canvas perf overlay: stacked-bar frame timeline, FPS line, top-stage
// table, memory gauge (Chrome only). Toggle via "Perf" checkbox.
function drawPerfOverlay() {
  if (!ctx || !canvas) return;
  var W = canvas.width, H = canvas.height;
  var PAD = 6;
  var OX = PAD, OY = PAD;
  var GW = Math.min(360, W - PAD * 2);
  var GH = 80;

  ctx.save();
  // Background panel
  ctx.fillStyle = 'rgba(0,0,0,0.72)';
  ctx.fillRect(OX, OY, GW, GH + 130);

  // Compute aggregates across the ring
  var totals = _perfFrameTotals;
  var maxFrame = 0, sumFrame = 0, countFrame = 0;
  for (var i = 0; i < PERF_HISTORY_LEN; i++) {
    var t = totals[i];
    if (t > 0) { sumFrame += t; countFrame++; if (t > maxFrame) maxFrame = t; }
  }
  var avgFrame = countFrame ? sumFrame / countFrame : 0;
  var fps = getPerfFrameCadence().fps;
  // Scale: anchor at max of 33.3ms (30fps) or observed max+20%
  var scaleMax = Math.max(33.3, maxFrame * 1.2);

  // Draw per-stage stacked bars. Pick top stages by average.
  var stageNames = [];
  for (var k in _perfStageHistory) stageNames.push(k);
  // Sort by recent (last-frame) contribution descending so dominant bars are bottom
  stageNames.sort(function(a, b) {
    return _perfStageHistory[b][_perfRingIdx] - _perfStageHistory[a][_perfRingIdx];
  });
  // Keep top 8, collapse rest into "other"
  var TOP = 8;
  var palette = ['#ff6b6b','#ffa94d','#ffd43b','#69db7c','#4dabf7','#b197fc','#f783ac','#a5d8ff'];
  var barW = GW / PERF_HISTORY_LEN;
  for (var xi = 0; xi < PERF_HISTORY_LEN; xi++) {
    var slot = (_perfRingIdx + 1 + xi) % PERF_HISTORY_LEN;
    var cumY = OY + GH;
    var frameTot = totals[slot];
    if (frameTot <= 0) continue;
    for (var si = 0; si < stageNames.length && si < TOP; si++) {
      var nm = stageNames[si];
      var v = _perfStageHistory[nm][slot];
      if (v <= 0) continue;
      var h = Math.max(1, Math.round((v / scaleMax) * GH));
      ctx.fillStyle = palette[si];
      ctx.fillRect(OX + xi * barW, cumY - h, Math.max(1, barW), h);
      cumY -= h;
    }
    // 16.7ms reference line (60fps)
    var y60 = OY + GH - Math.round((16.7 / scaleMax) * GH);
    ctx.fillStyle = 'rgba(255,255,255,0.22)';
    ctx.fillRect(OX, y60, GW, 1);
  }

  // Header text
  ctx.font = 'bold 11px monospace';
  ctx.textAlign = 'left';
  ctx.fillStyle = '#fff';
  ctx.fillText('PERF  rAF fps=' + fps.toFixed(0) +
    '  CPU avg=' + avgFrame.toFixed(1) + 'ms' +
    '  max=' + maxFrame.toFixed(1) + 'ms',
    OX + 4, OY + 12);

  // Stage legend + averages (top N)
  var ly = OY + GH + 10;
  ctx.font = '10px monospace';
  for (var li = 0; li < Math.min(TOP, stageNames.length); li++) {
    var nm2 = stageNames[li];
    var arr = _perfStageHistory[nm2];
    var s = 0, c = 0, mx = 0;
    for (var j = 0; j < PERF_HISTORY_LEN; j++) {
      var vv = arr[j];
      if (vv > 0) { s += vv; c++; if (vv > mx) mx = vv; }
    }
    var avg = c ? (s / c) : 0;
    ctx.fillStyle = palette[li];
    ctx.fillRect(OX + 4, ly - 7, 8, 8);
    ctx.fillStyle = '#fff';
    ctx.fillText(nm2 + '  avg=' + avg.toFixed(2) + '  max=' + mx.toFixed(2),
      OX + 16, ly);
    ly += 12;
  }

  // Memory gauge (Chrome only)
  if (performance && performance.memory) {
    var used = performance.memory.usedJSHeapSize / 1048576;
    var lim = performance.memory.jsHeapSizeLimit / 1048576;
    ctx.fillStyle = '#fff';
    ctx.fillText('mem=' + used.toFixed(1) + 'MB / ' + lim.toFixed(0) + 'MB',
      OX + 4, OY + GH + 130 - 4);
  }
  ctx.restore();
}

function dumpPerfBreakdown() {
  var entries = [];
  for (var k in _perfBreakdown) {
    var v = _perfBreakdown[k];
    entries.push({name:k, avg:(v.total / v.count).toFixed(2), max:v.max.toFixed(2), total:v.total.toFixed(1), count:v.count});
  }
  entries.sort(function(a,b){ return parseFloat(b.avg) - parseFloat(a.avg); });
  //console.log('[PERF-BREAKDOWN] Top subsystems (avg ms):'); // TEMP DISABLED
  //for (var i = 0; i < entries.length; i++) {
  //  var e = entries[i];
  //  console.log('  ' + e.name + ': avg=' + e.avg + 'ms  max=' + e.max + 'ms  total=' + e.total + 'ms  (' + e.count + ' calls)');
  //}
  _perfBreakdown = {};
}

// =============================================
// Per-second aggregate buffer — averages each stage over 1-second windows.
var PERF_SEC_LEN = 60;
var _perfSecStages = {};               // name → Float32Array(PERF_SEC_LEN) of avg ms/frame
var _perfSecFrameCount = new Int32Array(PERF_SEC_LEN);
var _perfSecTotalMs = new Float32Array(PERF_SEC_LEN);
var _perfSecBucket = Math.floor(Date.now() / 1000);
var _perfSecIdx = 0;
// Stages that wrap other _pt calls — excluded from the stack so bars match
// actual frame time instead of double-counting the parent.
var PERF_STAGE_EXCLUDE = {'draw()': 1};

function _perfTickSecondBucket() {
  var b = Math.floor(Date.now() / 1000);
  if (b === _perfSecBucket) return;
  _perfSecBucket = b;
  _perfSecIdx = (_perfSecIdx + 1) % PERF_SEC_LEN;
  _perfSecFrameCount[_perfSecIdx] = 0;
  _perfSecTotalMs[_perfSecIdx] = 0;
  for (var kk in _perfSecStages) _perfSecStages[kk][_perfSecIdx] = 0;
}

// PERF HUD — renders to the off-canvas #perfHudCanvas panel so the game
// view isn't covered. Combines per-frame stacked timeline with a per-second
// aggregate graph for digestible long-running trends.
// =============================================
function drawPerfHud() {
  var hudCanvas = document.getElementById('perfHudCanvas');
  var textEl = document.getElementById('perfHudText');
  if (!hudCanvas) return;
  var hctx = hudCanvas.getContext('2d');
  var W = hudCanvas.width, H = hudCanvas.height;

  // Roll legacy sparkline buffer from the latest completed frame total.
  // drawPerfHud runs inside renderFrame, BEFORE the loop commits the total
  // for _perfRingIdx — so the previous slot is the most recent complete one.
  var lastFrame = _perfFrameTotals[(_perfRingIdx - 1 + PERF_HISTORY_LEN) % PERF_HISTORY_LEN] || 0;
  _hudFrameTimes[_hudFrameIdx] = lastFrame;
  _hudFrameIdx = (_hudFrameIdx + 1) % 120;
  if (_hudFrameCount < 120) _hudFrameCount++;
  if (lastFrame > _hudAllTimeMaxFt) _hudAllTimeMaxFt = lastFrame;

  // Refresh breakdown snapshot every 500ms
  var now = Date.now();
  if (now - _hudBreakdownFlush > 500) {
    _hudBreakdownFlush = now;
    var snap = [];
    for (var k in _perfBreakdown) {
      var v = _perfBreakdown[k];
      if (v.count > 0) {
        if (!_hudAllTimeMax[k] || v.max > _hudAllTimeMax[k]) _hudAllTimeMax[k] = v.max;
        snap.push({name: k, avg: v.total / v.count, max: _hudAllTimeMax[k]});
      }
    }
    snap.sort(function(a, b) { return b.avg - a.avg; });
    _hudLastBreakdown = snap.slice(0, 14);
  }

  // FPS aggregate
  var sumFt = 0, maxFt = 0, cnt = _hudFrameCount;
  for (var i = 0; i < cnt; i++) { sumFt += _hudFrameTimes[i]; if (_hudFrameTimes[i] > maxFt) maxFt = _hudFrameTimes[i]; }
  var avgFt = cnt > 0 ? sumFt / cnt : 0;
  var fps = getPerfFrameCadence().fps;
  var TARGET_MS = 16.67;

  hctx.clearRect(0, 0, W, H);
  hctx.fillStyle = '#0a0a14';
  hctx.fillRect(0, 0, W, H);

  // ── HEADER: dedicated band with its own background so FPS text never collides with graph ──
  var HDR_H = 42;
  hctx.fillStyle = '#141423';
  hctx.fillRect(0, 0, W, HDR_H);
  hctx.font = 'bold 22px monospace';
  hctx.textAlign = 'left';
  hctx.fillStyle = fps >= 50 ? '#44ee66' : fps >= 30 ? '#eecc22' : '#ee3322';
  hctx.fillText(Math.round(fps) + ' fps', 8, 28);
  hctx.font = '10px monospace';
  hctx.fillStyle = '#99aacc';
  hctx.fillText('CPU avg ' + avgFt.toFixed(1) + 'ms   max ' + maxFt.toFixed(1) + 'ms   peak ' + _hudAllTimeMaxFt.toFixed(1) + 'ms',
    110, 20);
  hctx.fillStyle = '#778';
  hctx.fillText('top: per-frame (last ' + PERF_HISTORY_LEN + ' frames)   bottom: per-second avg (last ' + PERF_SEC_LEN + 's)',
    110, 34);

  // Pick top stages by CURRENT frame contribution (stack layer order).
  // Exclude parent timers (e.g. draw()) so stack sums match frame time.
  var stageNames = [];
  for (var sn in _perfStageHistory) {
    if (!PERF_STAGE_EXCLUDE[sn]) stageNames.push(sn);
  }
  stageNames.sort(function(a, b) {
    return _perfStageHistory[b][_perfRingIdx] - _perfStageHistory[a][_perfRingIdx];
  });
  var TOP = 8;
  var palette = ['#ff6b6b','#ffa94d','#ffd43b','#69db7c','#4dabf7','#b197fc','#f783ac','#a5d8ff'];

  // ── PER-FRAME TIMELINE ──
  var TLX = 8, TLY = HDR_H + 14, TLW = W - 16, TLH = 140;
  hctx.fillStyle = '#889';
  hctx.fillText('Per-frame (ms)', TLX, TLY - 3);
  var scaleMax = Math.max(33.3, maxFt * 1.2);
  hctx.fillStyle = '#111122';
  hctx.fillRect(TLX, TLY, TLW, TLH);
  var y60 = TLY + TLH - Math.round((TARGET_MS / scaleMax) * TLH);
  hctx.fillStyle = '#226622';
  hctx.fillRect(TLX, y60, TLW, 1);
  hctx.fillStyle = '#336';
  hctx.fillText('60fps', TLX + 4, y60 - 2);

  var barW = TLW / PERF_HISTORY_LEN;
  for (var xi = 0; xi < PERF_HISTORY_LEN; xi++) {
    var slot = (_perfRingIdx + 1 + xi) % PERF_HISTORY_LEN;
    if (_perfFrameTotals[slot] <= 0) continue;
    var cumY = TLY + TLH;
    for (var si = 0; si < stageNames.length && si < TOP; si++) {
      var nm = stageNames[si];
      var vv = _perfStageHistory[nm][slot];
      if (vv <= 0) continue;
      var barH = Math.max(1, Math.round((vv / scaleMax) * TLH));
      hctx.fillStyle = palette[si];
      hctx.fillRect(TLX + xi * barW, cumY - barH, Math.max(1, barW), barH);
      cumY -= barH;
    }
  }
  hctx.strokeStyle = '#334';
  hctx.strokeRect(TLX + 0.5, TLY + 0.5, TLW - 1, TLH - 1);

  // ── PER-SECOND AGGREGATE ──
  var SAX = 8, SAY = TLY + TLH + 22, SAW = W - 16, SAH = 130;
  hctx.fillStyle = '#889';
  hctx.fillText('Per-second (avg ms/frame)', SAX, SAY - 3);

  // Compute per-second avg frame time for scale
  var secMax = TARGET_MS;
  for (var sj = 0; sj < PERF_SEC_LEN; sj++) {
    var fc = _perfSecFrameCount[sj];
    if (fc > 0) {
      var avg = _perfSecTotalMs[sj] / fc;
      if (avg > secMax) secMax = avg;
    }
  }
  var secScale = Math.max(33.3, secMax * 1.2);
  hctx.fillStyle = '#111122';
  hctx.fillRect(SAX, SAY, SAW, SAH);
  var secY60 = SAY + SAH - Math.round((TARGET_MS / secScale) * SAH);
  hctx.fillStyle = '#226622';
  hctx.fillRect(SAX, secY60, SAW, 1);

  var secBarW = SAW / PERF_SEC_LEN;
  for (var xs = 0; xs < PERF_SEC_LEN; xs++) {
    var secSlot = (_perfSecIdx + 1 + xs) % PERF_SEC_LEN;
    var secFc = _perfSecFrameCount[secSlot];
    if (secFc <= 0) continue;
    var cumYs = SAY + SAH;
    for (var ssi = 0; ssi < stageNames.length && ssi < TOP; ssi++) {
      var snm = stageNames[ssi];
      var sArr = _perfSecStages[snm];
      if (!sArr) continue;
      var avgMs = sArr[secSlot] / secFc;
      if (avgMs <= 0) continue;
      var sh = Math.max(1, Math.round((avgMs / secScale) * SAH));
      hctx.fillStyle = palette[ssi];
      hctx.fillRect(SAX + xs * secBarW, cumYs - sh, Math.max(1, secBarW), sh);
      cumYs -= sh;
    }
  }
  hctx.strokeStyle = '#334';
  hctx.strokeRect(SAX + 0.5, SAY + 0.5, SAW - 1, SAH - 1);

  // ── LEGEND ──
  var ly = SAY + SAH + 16;
  hctx.font = '10px monospace';
  for (var li = 0; li < Math.min(TOP, stageNames.length); li++) {
    hctx.fillStyle = palette[li];
    hctx.fillRect(8 + (li % 4) * 140, ly + Math.floor(li / 4) * 14 - 8, 8, 8);
    hctx.fillStyle = '#cde';
    hctx.fillText(stageNames[li].slice(0, 15), 20 + (li % 4) * 140, ly + Math.floor(li / 4) * 14);
  }

  // ── CACHE TELEMETRY ── per-cache size sparkline + hit/miss/rate text
  var CX = 8, CY = ly + Math.ceil(Math.min(TOP, stageNames.length) / 4) * 14 + 10;
  var CW = W - 16, CROW_H = 32;
  hctx.fillStyle = '#889';
  hctx.fillText('Caches (green=hit, red=miss, bar=size)', CX, CY - 3);
  var cacheList = [
    {name:'matchZ',   label:'findMatchZ'},
    {name:'wallGrad', label:'wallGrad  '},
    {name:'rgbQ',     label:'rgbQ      '},
    {name:'floorH',   label:'floorHeight'}
  ];
  for (var ci2 = 0; ci2 < cacheList.length; ci2++) {
    var cs = _cacheStats[cacheList[ci2].name];
    var rowY = CY + ci2 * (CROW_H + 4);
    hctx.fillStyle = '#111122';
    hctx.fillRect(CX, rowY, CW, CROW_H);
    // Determine scales over history.
    var mxSize = cs.peakSize || 1, mxEvt = 1;
    for (var hi = 0; hi < CACHE_HIST_LEN; hi++) {
      var ev = cs.hitsHist[hi] + cs.missHist[hi];
      if (ev > mxEvt) mxEvt = ev;
    }
    // Draw sparkline: two strips — bottom = size bar, top = hit/miss strip.
    var STRIP_X = CX + 120, STRIP_W = CW - 220;
    var colW = STRIP_W / CACHE_HIST_LEN;
    for (var xh = 0; xh < CACHE_HIST_LEN; xh++) {
      var slot = (_cacheHistIdx + xh) % CACHE_HIST_LEN;
      var sz = cs.sizeHist[slot];
      var hts = cs.hitsHist[slot];
      var mss = cs.missHist[slot];
      var evSum = hts + mss;
      var px = STRIP_X + xh * colW;
      // size strip (bottom 16px)
      if (sz > 0) {
        var sH = Math.max(1, Math.round((sz / mxSize) * 16));
        hctx.fillStyle = '#4466aa';
        hctx.fillRect(px, rowY + CROW_H - sH, Math.max(1, colW), sH);
      }
      // event strip (top 14px): split proportionally into green(hit)/red(miss)
      if (evSum > 0) {
        var eH = Math.max(1, Math.round((evSum / mxEvt) * 14));
        var hitPx = Math.round(eH * hts / evSum);
        hctx.fillStyle = '#ee4444';
        hctx.fillRect(px, rowY + 1, Math.max(1, colW), eH);
        if (hitPx > 0) {
          hctx.fillStyle = '#44dd66';
          hctx.fillRect(px, rowY + 1 + (eH - hitPx), Math.max(1, colW), hitPx);
        }
      }
    }
    // Text: label, current size, hit%, peak
    var curHits = cs.hitsHist[(_cacheHistIdx - 1 + CACHE_HIST_LEN) % CACHE_HIST_LEN];
    var curMiss = cs.missHist[(_cacheHistIdx - 1 + CACHE_HIST_LEN) % CACHE_HIST_LEN];
    var curSize = cs.sizeHist[(_cacheHistIdx - 1 + CACHE_HIST_LEN) % CACHE_HIST_LEN];
    var rate = (curHits + curMiss) > 0 ? Math.round(100 * curHits / (curHits + curMiss)) : 0;
    hctx.fillStyle = '#cde';
    hctx.font = '10px monospace';
    hctx.fillText(cacheList[ci2].label, CX + 4, rowY + 12);
    hctx.fillStyle = '#99bbee';
    hctx.fillText('sz ' + curSize + '/' + cs.peakSize, CX + 4, rowY + 26);
    hctx.fillStyle = rate >= 50 ? '#44dd66' : rate >= 20 ? '#eecc22' : '#ee6644';
    hctx.textAlign = 'right';
    hctx.fillText(rate + '% hit', CX + CW - 6, rowY + 12);
    hctx.fillStyle = '#99bbee';
    hctx.fillText('h ' + curHits + ' m ' + curMiss, CX + CW - 6, rowY + 26);
    hctx.textAlign = 'left';
    hctx.strokeStyle = '#334';
    hctx.strokeRect(CX + 0.5, rowY + 0.5, CW - 1, CROW_H - 1);
  }

  // Commit this frame's cache counters into history, reset per-frame totals.
  _cacheCommitFrame();

  // Text side-panel with ranked stages + counts
  if (textEl) {
    var lines = [];
    lines.push('rAF delivery: ' + fps.toFixed(1) + ' fps');
    lines.push('CPU frame: ' + avgFt.toFixed(2) + ' ms average');
    lines.push('─── TOP STAGES (inclusive avg ms, max ms) ───');
    for (var ti = 0; ti < _hudLastBreakdown.length; ti++) {
      var e = _hudLastBreakdown[ti];
      var nm2 = (e.name + '               ').slice(0, 18);
      lines.push(nm2 + ' ' + e.avg.toFixed(2).padStart(6) + '   ' + e.max.toFixed(2).padStart(6));
    }
    lines.push('');
    lines.push('enemies: ' + (enemies ? enemies.length : 0) +
               '   proj: ' + (projectiles ? projectiles.length : 0) +
               '   impacts: ' + (impacts ? impacts.length : 0));
    if (performance && performance.memory) {
      var used = performance.memory.usedJSHeapSize / 1048576;
      var lim = performance.memory.jsHeapSizeLimit / 1048576;
      lines.push('mem: ' + used.toFixed(1) + ' / ' + lim.toFixed(0) + ' MB');
    }
    textEl.textContent = lines.join('\n');
  }
}

// Legacy on-canvas perf HUD (retained for reference; not called).
function _legacyDrawPerfHudOnCanvas_UNUSED() {
  var w = canvas.width, h = canvas.height;
  var sc = w / 360; // scale relative to base res
  var PAD = Math.round(6 * sc);
  var ROW = Math.round(11 * sc);
  var FONT = Math.round(10 * sc);
  var SPARK_W = Math.round(120 * sc);
  var SPARK_H = Math.round(28 * sc);

  // -- Capture frame time into sparkline ring buffer --
  var lastFrame = __perfFrames.length ? __perfFrames[__perfFrames.length - 1].total : 0;
  _hudFrameTimes[_hudFrameIdx] = lastFrame;
  _hudFrameIdx = (_hudFrameIdx + 1) % 120;
  if (_hudFrameCount < 120) _hudFrameCount++;
  if (lastFrame > _hudAllTimeMaxFt) _hudAllTimeMaxFt = lastFrame;

  // -- Refresh breakdown snapshot every 500ms to avoid flicker --
  var now = Date.now();
  if (now - _hudBreakdownFlush > 500) {
    _hudBreakdownFlush = now;
    var snap = [];
    for (var k in _perfBreakdown) {
      var v = _perfBreakdown[k];
      if (v.count > 0) {
        // Accumulate into all-time max — survives dumpPerfBreakdown() resets
        if (!_hudAllTimeMax[k] || v.max > _hudAllTimeMax[k]) _hudAllTimeMax[k] = v.max;
        snap.push({name: k, avg: v.total / v.count, max: _hudAllTimeMax[k]});
      }
    }
    snap.sort(function(a, b) { return b.avg - a.avg; });
    _hudLastBreakdown = snap.slice(0, 10); // top 10 by avg cost
  }

  // -- Compute rolling FPS and avg frame time --
  var sumFt = 0, maxFt = 0, cnt = _hudFrameCount;
  for (var i = 0; i < cnt; i++) { sumFt += _hudFrameTimes[i]; if (_hudFrameTimes[i] > maxFt) maxFt = _hudFrameTimes[i]; }
  var avgFt = cnt > 0 ? sumFt / cnt : 0;
  var fps = getPerfFrameCadence().fps;

  // -- Panel size: sparkline + rows for each subsystem --
  var numRows = _hudLastBreakdown.length + 3; // fps + phys-steps + spacer + subsystems
  var panelW = Math.round(200 * sc);
  var panelH = PAD * 2 + SPARK_H + PAD + numRows * ROW;
  var px = w - panelW - PAD;
  var py = PAD;

  // Background
  ctx.save();
  ctx.globalAlpha = 0.82;
  ctx.fillStyle = '#0a0a14';
  ctx.fillRect(px, py, panelW, panelH);
  ctx.globalAlpha = 1;
  ctx.strokeStyle = '#334';
  ctx.lineWidth = 1;
  ctx.strokeRect(px + 0.5, py + 0.5, panelW - 1, panelH - 1);

  // -- Sparkline --
  var sx0 = px + PAD, sy0 = py + PAD;
  var TARGET_MS = 16.67;
  var SCALE_MS  = Math.max(maxFt, TARGET_MS * 2, 33.4); // auto-scale ceiling
  ctx.fillStyle = '#111122';
  ctx.fillRect(sx0, sy0, SPARK_W, SPARK_H);
  // Target line at 16.67ms
  var targetY = sy0 + SPARK_H - Math.round((TARGET_MS / SCALE_MS) * SPARK_H);
  ctx.strokeStyle = '#226622';
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(sx0, targetY); ctx.lineTo(sx0 + SPARK_W, targetY); ctx.stroke();

  var barW = Math.max(1, SPARK_W / 120);
  for (var bi = 0; bi < cnt; bi++) {
    var fi = (_hudFrameIdx - cnt + bi + 120) % 120;
    var ft = _hudFrameTimes[fi];
    var bh = Math.round((ft / SCALE_MS) * SPARK_H);
    var bx = sx0 + Math.round(bi * (SPARK_W / 120));
    var byy = sy0 + SPARK_H - bh;
    ctx.fillStyle = ft > TARGET_MS * 1.5 ? '#cc3322' : ft > TARGET_MS ? '#ccaa22' : '#22aa44';
    ctx.fillRect(bx, byy, Math.ceil(barW), bh);
  }
  ctx.strokeStyle = '#445';
  ctx.lineWidth = 1;
  ctx.strokeRect(sx0, sy0, SPARK_W, SPARK_H);

  // FPS label next to sparkline
  ctx.font = 'bold ' + Math.round(13 * sc) + 'px monospace';
  var fpsColor = fps >= 50 ? '#44ee66' : fps >= 30 ? '#eecc22' : '#ee3322';
  ctx.fillStyle = fpsColor;
  ctx.fillText(Math.round(fps) + ' fps', sx0 + SPARK_W + PAD, sy0 + Math.round(12 * sc));
  ctx.font = FONT + 'px monospace';
  ctx.fillStyle = '#99aacc';
  ctx.fillText('avg ' + avgFt.toFixed(1) + 'ms', sx0 + SPARK_W + PAD, sy0 + Math.round(24 * sc));
  ctx.fillText('max ' + maxFt.toFixed(1) + 'ms', sx0 + SPARK_W + PAD, sy0 + Math.round(36 * sc));
  ctx.fillStyle = '#cc8844';
  ctx.fillText('peak ' + _hudAllTimeMaxFt.toFixed(1) + 'ms', sx0 + SPARK_W + PAD, sy0 + Math.round(48 * sc));

  // -- Subsystem breakdown rows --
  var ry = sy0 + SPARK_H + PAD + ROW;
  ctx.font = FONT + 'px monospace';

  // Physics steps this frame
  var physSteps = Math.min(MAX_PHYSICS_STEPS, Math.round(_physicsAccum / FIXED_DT_MS + 1));
  ctx.fillStyle = physSteps > 1 ? '#ee4422' : '#99aacc';
  ctx.fillText('phys steps: ' + physSteps, px + PAD, ry); ry += ROW;

  // Object counts
  ctx.fillStyle = '#778899';
  ctx.fillText('enemies:' + (enemies ? enemies.length : 0) +
    '  proj:' + (projectiles ? projectiles.length : 0) +
    '  fx:' + ((coneEffects ? coneEffects.length : 0) + (impacts ? impacts.length : 0)),
    px + PAD, ry); ry += ROW + Math.round(2 * sc);

  // Subsystem rows — sorted by avg cost, bar shows proportion of frame budget
  var maxAvg = _hudLastBreakdown.length > 0 ? Math.max(_hudLastBreakdown[0].avg, 0.1) : 1;
  var barAreaW = Math.round(panelW - PAD * 2 - Math.round(70 * sc));
  for (var ri = 0; ri < _hudLastBreakdown.length; ri++) {
    var entry = _hudLastBreakdown[ri];
    var proportion = Math.min(1, entry.avg / TARGET_MS); // fill relative to 16ms budget
    var barLen = Math.round(proportion * barAreaW);
    var barColor = proportion > 0.5 ? '#882211' : proportion > 0.25 ? '#886611' : '#226633';
    ctx.fillStyle = barColor;
    ctx.fillRect(px + PAD, ry - ROW + 2, barLen, ROW - 2);
    ctx.fillStyle = proportion > 0.3 ? '#ffddcc' : '#aabbcc';
    var label = entry.name.length > 14 ? entry.name.slice(0, 13) + '…' : entry.name;
    ctx.fillText(label, px + PAD + 2, ry);
    ctx.fillStyle = '#ccddee';
    ctx.textAlign = 'right';
    ctx.fillText(entry.avg.toFixed(1) + 'ms', px + panelW - PAD, ry);
    ctx.textAlign = 'left';
    ry += ROW;
  }

  ctx.restore();
}

// CAVE DEBUG — renders diagnostic info to the off-canvas debug panel (#caveDebugPanel)
function drawCaveDebug() {
  var panel = document.getElementById('caveDebugPanel');
  var textEl = document.getElementById('caveDebugText');
  if (!panel || !textEl) return;
  panel.style.display = '';

  // Gather data
  var pFloorH = 0, pCeilH = 0, pColor = '?';
  var nearestEntDist = -1, nearestEntPos = null;
  var negQuadsInView = 0, clampedQuads = 0;

  if (floorMesh) {
    var pgx = Math.floor(pos.x / floorMesh.gridSize);
    var pgy = Math.floor(pos.y / floorMesh.gridSize);
    if (pgx >= 0 && pgx < floorMesh.w && pgy >= 0 && pgy < floorMesh.h) {
      var pidx = pgy * floorMesh.w + pgx;
      pFloorH = floorMesh.l0TopZ[pidx];
      var _pdbgLc = floorMesh.layerCount[pidx];
      if (_pdbgLc >= 2 && floorMesh.l1Type[pidx] === 2) pCeilH = floorMesh.l1TopZ[pidx];
      else if (_pdbgLc >= 3 && floorMesh.l2Type[pidx] === 2) pCeilH = floorMesh.l2TopZ[pidx];
      if (floorMesh.colors) pColor = floorMesh.colors[pidx];
    }

    var scanR = 10;
    for (var sy = pgy - scanR; sy <= pgy + scanR; sy++) {
      for (var sx = pgx - scanR; sx <= pgx + scanR; sx++) {
        if (sx < 0 || sx >= floorMesh.w || sy < 0 || sy >= floorMesh.h) continue;
        var si = sy * floorMesh.w + sx;
        if (floorMesh.l0TopZ[si] < 0) {
          negQuadsInView++;
          var _sdbgLc = floorMesh.layerCount[si];
          var mc = 0;
          if (_sdbgLc >= 2 && floorMesh.l1Type[si] === 2) mc = floorMesh.l1TopZ[si];
          else if (_sdbgLc >= 3 && floorMesh.l2Type[si] === 2) mc = floorMesh.l2TopZ[si];
          if (mc > 2.0) clampedQuads++;
        }
      }
    }
  }

  if (typeof endlessCaveNetworks !== 'undefined') {
    var keys = Object.keys(endlessCaveNetworks);
    for (var ki = 0; ki < keys.length; ki++) {
      var net = endlessCaveNetworks[keys[ki]];
      if (!net || !net.entrances) continue;
      for (var ei = 0; ei < net.entrances.length; ei++) {
        var ent = net.entrances[ei];
        var edx = (ent.x - windowOriginX) - pos.x, edy = (ent.y - windowOriginY) - pos.y;
        var ed = Math.sqrt(edx * edx + edy * edy);
        if (nearestEntDist < 0 || ed < nearestEntDist) {
          nearestEntDist = ed;
          nearestEntPos = ent;
        }
      }
    }
  }

  // Grid/mesh cave metadata at player position
  var _pGridCave = false, _pMeshCave = false;
  var _pGridGx = Math.floor(pos.x / cell), _pGridGy = Math.floor(pos.y / cell);
  if (gridCave && _pGridGx >= 0 && _pGridGx < gridW && _pGridGy >= 0 && _pGridGy < gridH) {
    _pGridCave = !!gridCave[_pGridGy * gridW + _pGridGx];
  }
  if (meshCave && floorMesh) {
    var _pMeshGx = Math.floor(pos.x / 12), _pMeshGy = Math.floor(pos.y / 12);
    if (_pMeshGx >= 0 && _pMeshGx < floorMesh.w && _pMeshGy >= 0 && _pMeshGy < floorMesh.h) {
      _pMeshCave = !!meshCave[_pMeshGy * floorMesh.w + _pMeshGx];
    }
  }
  // Surface floor clamp test
  var _clampedFH = pFloorH >= -0.1 ? pFloorH : Math.max(pFloorH, -0.1);
  var _clampNearEntr = false;
  if (pFloorH < -0.1 && deepCaveEntrances) {
    for (var _sci2 = 0; _sci2 < deepCaveEntrances.length; _sci2++) {
      var _sc2dx = pos.x - deepCaveEntrances[_sci2].x;
      var _sc2dy = pos.y - deepCaveEntrances[_sci2].y;
      if (_sc2dx * _sc2dx + _sc2dy * _sc2dy < 150 * 150) { _clampNearEntr = true; _clampedFH = pFloorH; break; }
    }
  }
  // Light grid at player
  var _pLightVal = 0;
  if (_lightGrid) {
    var _plgx2 = Math.floor(pos.x / _lightCellSize), _plgy2 = Math.floor(pos.y / _lightCellSize);
    if (_plgx2 >= 0 && _plgx2 < _lightGridW && _plgy2 >= 0 && _plgy2 < _lightGridH) {
      _pLightVal = _lightGrid[_plgy2 * _lightGridW + _plgx2];
    }
  }

  var lines = [
    '═══ CAVE DEBUG ═══',
    'pos: ' + pos.x.toFixed(0) + ', ' + pos.y.toFixed(0),
    'floorH: ' + pFloorH.toFixed(3) + (pFloorH < 0 ? ' ⊘' : ''),
    'ceilH: ' + pCeilH.toFixed(3) + (pCeilH > 0.1 ? ' ⊘' : ''),
    'playerZ: ' + (pos.floorZ !== undefined ? pos.floorZ.toFixed(1) : '?') + '  (surfaceZ=60)',
    'camZ: ' + (cam && cam.z !== undefined ? cam.z.toFixed(1) : '?'),
    'underground: ' + (playerUnderground ? 'YES' : 'no'),
    '── METADATA ──',
    'gridCave: ' + (_pGridCave ? 'YES' : 'no') + '  meshCave: ' + (_pMeshCave ? 'YES' : 'no'),
    'gridCave total: ' + (gridCave ? gridCave.reduce(function(a,b){return a+b;},0) : 'null'),
    'meshCave total: ' + (meshCave ? meshCave.reduce(function(a,b){return a+b;},0) : 'null'),
    'walls[].cave: ' + (walls ? walls.filter(function(w){return w.cave;}).length + '/' + walls.length : '?'),
    '── LIGHTING ──',
    'ambient: ' + (typeof ambientLight !== 'undefined' ? ambientLight.toFixed(3) : '?'),
    'lightGrid@player: ' + _pLightVal.toFixed(3),
    'effectiveLight: ' + Math.min(1.0, ambientLight + _pLightVal).toFixed(3),
    '── FLOOR CLAMP ──',
    'rawFloorH: ' + pFloorH.toFixed(3),
    'clampedFloorH: ' + _clampedFH.toFixed(3),
    'nearEntrance(<150): ' + (_clampNearEntr ? 'YES' : 'no'),
    'clampedZ: ' + (_clampedFH * 40 + 60).toFixed(1) + '  rawZ: ' + (pFloorH * 40 + 60).toFixed(1),
    '── ENTRANCE ──',
    'entrance dist: ' + (nearestEntDist >= 0 ? nearestEntDist.toFixed(0) + 'u' : 'none'),
    'deepCaveEntrances: ' + (typeof deepCaveEntrances !== 'undefined' ? deepCaveEntrances.length : '?'),
    'deferred quads: ' + (_deferredCaveQuads ? _deferredCaveQuads.length : 0),
    'floorColor: ' + pColor,
    'negH nearby: ' + negQuadsInView + ' (clamped: ' + clampedQuads + ')',
    'seed: ' + WORLD_SEED,
    '── WALLS ──',
    'wallTopZ@player: ' + (typeof getWallTopZ === 'function' ? getWallTopZ(pos.x, pos.y).toFixed(0) : '?'),
    'inWall(noZ): ' + (typeof isInGridWall === 'function' ? isInGridWall(pos.x, pos.y, 6) : '?'),
    'inWall(+Z): ' + (typeof isInGridWall === 'function' ? isInGridWall(pos.x, pos.y, 6, pos.floorZ || 60) : '?'),
  ];
  textEl.textContent = lines.join('\n');

  // === SECOND PANEL: rejection histograms, ceiling trace, entrance table, ASCII grid ===
  var statsEl = document.getElementById('caveDebugStats');
  if (statsEl) {
    var cs = __caveStats;
    var bar = function(n, total, width) {
      if (!total) return ''.padEnd(width, '·');
      var filled = Math.round(n / total * width);
      return '█'.repeat(filled) + '·'.repeat(width - filled);
    };
    var pct = function(n, total) {
      return total ? (n / total * 100).toFixed(1).padStart(5) + '%' : '  —  ';
    };
    // Floor histogram
    var sLines = ['═══ FLOOR REJECTION ═══'];
    sLines.push('total visited: ' + cs.floorTotal);
    var ft = cs.floorTotal || 1;
    sLines.push('distCull     ' + bar(cs.floorDistCull, ft, 20)     + ' ' + pct(cs.floorDistCull, ft)     + ' (' + cs.floorDistCull + ')');
    sLines.push('fovCull      ' + bar(cs.floorFovCull, ft, 20)      + ' ' + pct(cs.floorFovCull, ft)      + ' (' + cs.floorFovCull + ')');
    sLines.push('behind       ' + bar(cs.floorBehind, ft, 20)       + ' ' + pct(cs.floorBehind, ft)       + ' (' + cs.floorBehind + ')');
    sLines.push('blendRange   ' + bar(cs.floorBlendRange, ft, 20)   + ' ' + pct(cs.floorBlendRange, ft)   + ' (' + cs.floorBlendRange + ')');
    sLines.push('outsideBlend ' + bar(cs.floorOutsideBlend, ft, 20) + ' ' + pct(cs.floorOutsideBlend, ft) + ' (' + cs.floorOutsideBlend + ')');
    // Ceiling trace
    var ceilVisited = cs.ceilCollected + cs.ceilSkipLowCeil + cs.ceilSkipEntrRange +
                      cs.ceilSkipViewDist + cs.ceilSkipBehind + cs.ceilSkipFov;
    var ct = ceilVisited || 1;
    sLines.push('');
    sLines.push('═══ CEILING PIPELINE ═══');
    sLines.push('visited: ' + ceilVisited + '  collected: ' + cs.ceilCollected + '  rendered: ' + cs.ceilRendered);
    sLines.push('skipLowCeil  ' + bar(cs.ceilSkipLowCeil, ct, 20)   + ' ' + pct(cs.ceilSkipLowCeil, ct));
    sLines.push('skipEntrRnge ' + bar(cs.ceilSkipEntrRange, ct, 20) + ' ' + pct(cs.ceilSkipEntrRange, ct));
    sLines.push('skipViewDist ' + bar(cs.ceilSkipViewDist, ct, 20)  + ' ' + pct(cs.ceilSkipViewDist, ct));
    sLines.push('skipBehind   ' + bar(cs.ceilSkipBehind, ct, 20)    + ' ' + pct(cs.ceilSkipBehind, ct));
    sLines.push('skipFov      ' + bar(cs.ceilSkipFov, ct, 20)       + ' ' + pct(cs.ceilSkipFov, ct));
    // Entrance table
    sLines.push('');
    sLines.push('═══ ENTRANCES ═══');
    if (deepCaveEntrances && deepCaveEntrances.length > 0) {
      sLines.push('id    x      y    dist   depth  ceilH');
      for (var _ti = 0; _ti < deepCaveEntrances.length; _ti++) {
        var _te = deepCaveEntrances[_ti];
        var _tdx = pos.x - _te.x, _tdy = pos.y - _te.y;
        var _td = Math.sqrt(_tdx*_tdx + _tdy*_tdy);
        sLines.push(
          String(_ti).padStart(2) + '  ' +
          _te.x.toFixed(0).padStart(5) + '  ' + _te.y.toFixed(0).padStart(5) + '  ' +
          _td.toFixed(0).padStart(5) + '  ' +
          (_te.depth !== undefined ? (+_te.depth).toFixed(2) : ' —  ').padStart(5) + '  ' +
          (_te.ceilH !== undefined ? (+_te.ceilH).toFixed(2) : ' —  ').padStart(5)
        );
      }
    } else {
      sLines.push('(no deepCaveEntrances)');
    }
    // ASCII grid (15x15 around player)
    sLines.push('');
    sLines.push('═══ ASCII GRID (@=you  *=entr  #=deep  ~=shallow) ═══');
    if (floorMesh) {
      var _agx = Math.floor(pos.x / floorMesh.gridSize);
      var _agy = Math.floor(pos.y / floorMesh.gridSize);
      var _aR2 = 7;
      var _entrGridSet = {};
      if (deepCaveEntrances) {
        for (var _ek = 0; _ek < deepCaveEntrances.length; _ek++) {
          var _egx = Math.floor(deepCaveEntrances[_ek].x / floorMesh.gridSize);
          var _egy = Math.floor(deepCaveEntrances[_ek].y / floorMesh.gridSize);
          _entrGridSet[_egx + ',' + _egy] = true;
        }
      }
      for (var _gy2 = _agy - _aR2; _gy2 <= _agy + _aR2; _gy2++) {
        var _gr = '';
        for (var _gx2 = _agx - _aR2; _gx2 <= _agx + _aR2; _gx2++) {
          if (_gx2 === _agx && _gy2 === _agy) { _gr += '@ '; continue; }
          if (_entrGridSet[_gx2 + ',' + _gy2]) { _gr += '* '; continue; }
          if (_gx2 < 0 || _gx2 >= floorMesh.w || _gy2 < 0 || _gy2 >= floorMesh.h) { _gr += '  '; continue; }
          var _gh = floorMesh.l0TopZ[_gy2 * floorMesh.w + _gx2];
          if (_gh < -1.0) _gr += '# ';
          else if (_gh < -0.1) _gr += '~ ';
          else _gr += '. ';
        }
        sLines.push(_gr);
      }
    }
    statsEl.textContent = sLines.join('\n');
  }

  // === VISUAL CROSS-SECTION PROFILE (separate canvas) ===
  drawCaveProfileChart();
}

// Cave cross-section profile chart — drawn on its own canvas (#caveProfileCanvas)
// next to the control map, NOT on the game canvas.
function drawCaveProfileChart() {
  var cvs = document.getElementById('caveProfileCanvas');
  if (!cvs || !floorMesh || !deepCaveEntrances || deepCaveEntrances.length === 0) return;
  var pc = cvs.getContext('2d');
  var W = cvs.width, H = cvs.height;
  pc.clearRect(0, 0, W, H);

  // Find nearest entrance
  var entr = null, entrDist = Infinity;
  for (var _vei = 0; _vei < deepCaveEntrances.length; _vei++) {
    var _vdx = pos.x - deepCaveEntrances[_vei].x, _vdy = pos.y - deepCaveEntrances[_vei].y;
    var _vd = Math.sqrt(_vdx * _vdx + _vdy * _vdy);
    if (_vd < entrDist) { entrDist = _vd; entr = deepCaveEntrances[_vei]; }
  }
  if (!entr) return;

  // Profile direction: from player through entrance and beyond
  var _pdx = entr.x - pos.x, _pdy = entr.y - pos.y;
  var _pdLen = Math.sqrt(_pdx * _pdx + _pdy * _pdy);
  if (_pdLen < 1) return;
  var _dirX = _pdx / _pdLen, _dirY = _pdy / _pdLen;

  // Sample points: from 60u behind player to 300u past entrance
  var _sampleStart = -60, _sampleEnd = _pdLen + 300;
  var _numSamples = 60;
  var _sStep = (_sampleEnd - _sampleStart) / (_numSamples - 1);
  var _floors = [], _ceils = [], _dists = [];
  var _minFH = 99, _maxFH = -99, _maxCH = 0;
  for (var _si = 0; _si < _numSamples; _si++) {
    var _sd = _sampleStart + _si * _sStep;
    var _sx2 = pos.x + _dirX * _sd, _sy3 = pos.y + _dirY * _sd;
    var _sfh = getFloorHeightAt(_sx2, _sy3);
    var _sch = 0;
    if (floorMesh.layerCount) {
      var _smx = Math.floor(_sx2 / floorMesh.gridSize);
      var _smy = Math.floor(_sy3 / floorMesh.gridSize);
      if (_smx >= 0 && _smx < floorMesh.w && _smy >= 0 && _smy < floorMesh.h) {
        var _smIdx = _smy * floorMesh.w + _smx;
        var _smLc = floorMesh.layerCount[_smIdx];
        if (_smLc >= 2 && floorMesh.l1Type[_smIdx] === 2) _sch = floorMesh.l1TopZ[_smIdx];
        else if (_smLc >= 3 && floorMesh.l2Type[_smIdx] === 2) _sch = floorMesh.l2TopZ[_smIdx];
      }
    }
    _floors.push(_sfh);
    _ceils.push(_sch);
    _dists.push(_sd);
    if (_sfh < _minFH) _minFH = _sfh;
    if (_sfh > _maxFH) _maxFH = _sfh;
    if (_sch > _maxCH) _maxCH = _sch;
  }

  // Chart area with margins
  var margin = {top: 22, right: 10, bottom: 26, left: 30};
  var cW = W - margin.left - margin.right;
  var cH = H - margin.top - margin.bottom;
  var _vMin = Math.min(_minFH - 0.5, -4);
  var _vMax = Math.max(_maxFH + 1, _maxCH + 1, 5);
  var _vRange = _vMax - _vMin;
  function yAt(h) { return margin.top + cH - ((h - _vMin) / _vRange) * cH; }
  function xAt(d) { return margin.left + ((d - _sampleStart) / (_sampleEnd - _sampleStart)) * cW; }

  // Title
  pc.fillStyle = '#88aacc';
  pc.font = '11px monospace';
  pc.fillText('CAVE CROSS-SECTION (player \u2192 entrance)', margin.left, 14);

  // Sky region (above surface, no ceiling): faint blue wash so "outside" is
  // visually distinct from "underground rock".
  pc.fillStyle = 'rgba(60, 90, 140, 0.10)';
  pc.fillRect(margin.left, margin.top, cW, yAt(0) - margin.top);

  // Horizontal grid lines + Y (height) labels
  pc.strokeStyle = '#2a2a35';
  pc.lineWidth = 0.5;
  for (var _gh = Math.ceil(_vMin); _gh <= Math.floor(_vMax); _gh++) {
    var _gy3 = yAt(_gh);
    pc.beginPath(); pc.moveTo(margin.left, _gy3); pc.lineTo(W - margin.right, _gy3); pc.stroke();
    pc.fillStyle = _gh === 0 ? '#77aa77' : '#556677';
    pc.font = '9px monospace';
    pc.fillText(_gh.toFixed(0), 4, _gy3 + 3);
  }
  // Vertical tick lines + X (distance) labels every 100u
  var _xTickStep = 100;
  var _xFirst = Math.ceil(_sampleStart / _xTickStep) * _xTickStep;
  pc.strokeStyle = '#22222c';
  pc.lineWidth = 0.5;
  for (var _xt = _xFirst; _xt <= _sampleEnd; _xt += _xTickStep) {
    var _xtx = xAt(_xt);
    pc.beginPath(); pc.moveTo(_xtx, margin.top); pc.lineTo(_xtx, margin.top + cH); pc.stroke();
    pc.fillStyle = '#556677';
    pc.font = '8px monospace';
    var _xLab = _xt === 0 ? '0' : (_xt > 0 ? '+' + _xt : '' + _xt) + 'u';
    pc.fillText(_xLab, _xtx - 10, margin.top + cH + 10);
  }

  // Zero line (surface level) — drawn on top of grid so it stands out
  pc.strokeStyle = '#558855';
  pc.lineWidth = 1.5;
  var _zeroY = yAt(0);
  pc.beginPath(); pc.moveTo(margin.left, _zeroY); pc.lineTo(W - margin.right, _zeroY); pc.stroke();
  pc.fillStyle = '#77aa77';
  pc.font = '9px monospace';
  pc.fillText('surface', W - margin.right - 40, _zeroY - 3);

  // Cave void fill: region between ceiling and floor where ceiling exists.
  // Darker fill so "empty space" reads as empty.
  pc.beginPath();
  var _cStarted = false;
  for (var _ci2 = 0; _ci2 < _numSamples; _ci2++) {
    if (_ceils[_ci2] > 0.1) {
      if (!_cStarted) { pc.moveTo(xAt(_dists[_ci2]), yAt(_ceils[_ci2])); _cStarted = true; }
      else pc.lineTo(xAt(_dists[_ci2]), yAt(_ceils[_ci2]));
    }
  }
  if (_cStarted) {
    for (var _ci3 = _numSamples - 1; _ci3 >= 0; _ci3--) {
      if (_ceils[_ci3] > 0.1) pc.lineTo(xAt(_dists[_ci3]), yAt(_floors[_ci3]));
    }
    pc.closePath();
    pc.fillStyle = 'rgba(10, 8, 18, 0.75)';
    pc.fill();
    // Label the cave void
    var _voidCenterI = 0, _voidCount = 0;
    for (var _vci = 0; _vci < _numSamples; _vci++) if (_ceils[_vci] > 0.1) { _voidCenterI += _vci; _voidCount++; }
    if (_voidCount > 0) {
      var _voidI = Math.floor(_voidCenterI / _voidCount);
      pc.fillStyle = '#9988bb';
      pc.font = '9px monospace';
      pc.fillText('cave', xAt(_dists[_voidI]) - 10, (yAt(_ceils[_voidI]) + yAt(_floors[_voidI])) / 2 + 3);
    }
  }

  // Ceiling line (solid brown, bold where present)
  pc.beginPath();
  _cStarted = false;
  for (var _ci4 = 0; _ci4 < _numSamples; _ci4++) {
    if (_ceils[_ci4] > 0.1) {
      if (!_cStarted) { pc.moveTo(xAt(_dists[_ci4]), yAt(_ceils[_ci4])); _cStarted = true; }
      else pc.lineTo(xAt(_dists[_ci4]), yAt(_ceils[_ci4]));
    }
  }
  pc.strokeStyle = '#cc9955';
  pc.lineWidth = 2;
  pc.stroke();

  // Solid ground fill (below floor) — dirt tone with hatch lines for readability.
  pc.save();
  pc.beginPath();
  pc.moveTo(xAt(_dists[0]), yAt(_floors[0]));
  for (var _fi2 = 1; _fi2 < _numSamples; _fi2++) pc.lineTo(xAt(_dists[_fi2]), yAt(_floors[_fi2]));
  pc.lineTo(W - margin.right, margin.top + cH);
  pc.lineTo(margin.left, margin.top + cH);
  pc.closePath();
  pc.fillStyle = 'rgba(55, 40, 25, 0.55)';
  pc.fill();
  // Hatch lines inside solid ground
  pc.clip();
  pc.strokeStyle = 'rgba(90, 70, 45, 0.35)';
  pc.lineWidth = 0.5;
  for (var _hx = margin.left - cH; _hx < W; _hx += 6) {
    pc.beginPath();
    pc.moveTo(_hx, margin.top + cH);
    pc.lineTo(_hx + cH, margin.top);
    pc.stroke();
  }
  pc.restore();

  // Floor line
  pc.beginPath();
  pc.moveTo(xAt(_dists[0]), yAt(_floors[0]));
  for (var _fi = 1; _fi < _numSamples; _fi++) pc.lineTo(xAt(_dists[_fi]), yAt(_floors[_fi]));
  pc.strokeStyle = '#44ee44';
  pc.lineWidth = 2;
  pc.stroke();

  // Mark steep sections in red
  for (var _sli2 = 1; _sli2 < _numSamples; _sli2++) {
    var _segSlope = Math.abs(_floors[_sli2] - _floors[_sli2 - 1]) / _sStep;
    var _segAngle = Math.atan(_segSlope * 25) * 180 / Math.PI;
    if (_segAngle > 45) {
      pc.strokeStyle = 'rgba(255,60,60,0.7)';
      pc.lineWidth = 3;
      pc.beginPath();
      pc.moveTo(xAt(_dists[_sli2 - 1]), yAt(_floors[_sli2 - 1]));
      pc.lineTo(xAt(_dists[_sli2]), yAt(_floors[_sli2]));
      pc.stroke();
    }
  }

  // Entrance marker
  var _entrX = xAt(_pdLen);
  pc.strokeStyle = '#ff4444';
  pc.lineWidth = 1;
  pc.setLineDash([4, 3]);
  pc.beginPath(); pc.moveTo(_entrX, margin.top); pc.lineTo(_entrX, margin.top + cH); pc.stroke();
  pc.setLineDash([]);
  pc.fillStyle = '#ff6666';
  pc.font = '9px monospace';
  pc.fillText('entrance', _entrX - 22, margin.top + cH + 12);

  // Player marker (vertical line + dot at actual player height)
  var _playerX = xAt(0);
  pc.strokeStyle = '#4488ff';
  pc.lineWidth = 1;
  pc.setLineDash([4, 3]);
  pc.beginPath(); pc.moveTo(_playerX, margin.top); pc.lineTo(_playerX, margin.top + cH); pc.stroke();
  pc.setLineDash([]);
  // Dot at player's actual Z (converted back from pos.floorZ: Z=60 is surface, 40u per mesh unit)
  var _playerH = typeof pos !== 'undefined' && pos.floorZ !== undefined ? (pos.floorZ - 60) / 40 : 0;
  if (_playerH >= _vMin && _playerH <= _vMax) {
    var _pHY = yAt(_playerH);
    pc.fillStyle = '#6699ff';
    pc.beginPath(); pc.arc(_playerX, _pHY, 3.5, 0, Math.PI * 2); pc.fill();
    pc.strokeStyle = '#ffffff';
    pc.lineWidth = 1;
    pc.stroke();
  }
  pc.fillStyle = '#6699ff';
  pc.fillText('player', _playerX - 16, margin.top + 12);

  // Slope stats
  var _maxSlope = 0, _maxSlopeAt = 0;
  for (var _sli = 1; _sli < _numSamples; _sli++) {
    var _slope = Math.abs(_floors[_sli] - _floors[_sli - 1]) / _sStep;
    if (_slope > _maxSlope) { _maxSlope = _slope; _maxSlopeAt = _dists[_sli]; }
  }
  var _slopeAngle = Math.atan(_maxSlope * 25) * 180 / Math.PI;
  pc.fillStyle = _slopeAngle > 45 ? '#ff6644' : '#ffcc44';
  pc.font = '10px monospace';
  pc.fillText('max slope: ' + (_maxSlope * 100).toFixed(1) + 'h/100u (' + _slopeAngle.toFixed(0) + '\u00b0) at d=' + _maxSlopeAt.toFixed(0), margin.left, margin.top + cH + 14);

  // Legend
  pc.font = '10px monospace';
  pc.fillStyle = '#44ee44'; pc.fillText('\u2500 floor', W - 130, 14);
  pc.fillStyle = '#bb8844'; pc.fillText('\u2500 ceiling', W - 70, 14);
  // Red = steep indicator
  pc.fillStyle = '#ff6644'; pc.fillText('\u2500 steep (>45\u00b0)', W - 130, margin.top + cH + 14);
}

// CAVE CONTROL MAP — top-down mini-map overlay for cave entrance debugging.
// Shows floor heights, cave walls, entrance positions, player pos + direction.
// Drawn when CAVE_TEST_MODE is true and Cave Debug is enabled.
function drawCaveControlMap() {
  if (!floorMesh || !deepCaveEntrances || deepCaveEntrances.length === 0) return;
  var mapCanvas = document.getElementById('controlMapCanvas');
  if (!mapCanvas) return;
  var mctx = mapCanvas.getContext('2d');
  var mapSize = mapCanvas.width;  // use the canvas element size
  var mapX = 0, mapY = 0;  // render at origin of separate canvas
  var viewRadius = 500;  // world units radius to show (zoomed out 2x)
  // Replace ctx with mctx for all drawing in this function
  var ctx = mctx;
  var sc = 1;  // fixed scale for 300px control map canvas
  ctx.clearRect(0, 0, mapSize, mapSize);

  // Center on nearest entrance (deepCaveEntrances already in local coords)
  var centerWX = deepCaveEntrances[0].x;
  var centerWY = deepCaveEntrances[0].y;
  var nearDist = Infinity;
  for (var i = 0; i < deepCaveEntrances.length; i++) {
    var ex = deepCaveEntrances[i].x;
    var ey = deepCaveEntrances[i].y;
    var d = Math.sqrt((pos.x - ex) * (pos.x - ex) + (pos.y - ey) * (pos.y - ey));
    if (d < nearDist) { nearDist = d; centerWX = ex; centerWY = ey; }
  }

  var pxPerUnit = mapSize / (viewRadius * 2);
  function worldToMap(wx, wy) {
    return {
      x: mapX + (wx - centerWX + viewRadius) * pxPerUnit,
      y: mapY + (wy - centerWY + viewRadius) * pxPerUnit
    };
  }

  ctx.save();

  // Background
  ctx.globalAlpha = 0.85;
  ctx.fillStyle = '#0a0a14';
  ctx.fillRect(mapX, mapY, mapSize, mapSize);
  ctx.strokeStyle = '#3a6';
  ctx.lineWidth = 1;
  ctx.strokeRect(mapX + 0.5, mapY + 0.5, mapSize - 1, mapSize - 1);
  ctx.globalAlpha = 1;

  // Clip to map area
  ctx.beginPath();
  ctx.rect(mapX, mapY, mapSize, mapSize);
  ctx.clip();

  // Draw floor height grid
  var gs = floorMesh.gridSize;
  var cellPx = gs * pxPerUnit;
  var minGX = Math.max(0, Math.floor((centerWX - viewRadius) / gs));
  var maxGX = Math.min(floorMesh.w - 1, Math.ceil((centerWX + viewRadius) / gs));
  var minGY = Math.max(0, Math.floor((centerWY - viewRadius) / gs));
  var maxGY = Math.min(floorMesh.h - 1, Math.ceil((centerWY + viewRadius) / gs));

  for (var gy = minGY; gy <= maxGY; gy++) {
    for (var gx = minGX; gx <= maxGX; gx++) {
      var idx = gy * floorMesh.w + gx;
      var fh = floorMesh.l0TopZ[idx];
      var _ovmLc = floorMesh.layerCount[idx];
      var ch = 0;
      if (_ovmLc >= 2 && floorMesh.l1Type[idx] === 2) ch = floorMesh.l1TopZ[idx];
      else if (_ovmLc >= 3 && floorMesh.l2Type[idx] === 2) ch = floorMesh.l2TopZ[idx];
      var mp = worldToMap(gx * gs, gy * gs);
      var cs = Math.max(1, Math.ceil(cellPx));

      if (fh < -0.1) {
        // Underground floor: blue-purple gradient by depth
        var depth = Math.min(1, Math.abs(fh) / 4);
        var r = Math.floor(30 + 60 * depth);
        var g = Math.floor(20 + 30 * (1 - depth));
        var b = Math.floor(80 + 120 * depth);
        ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
      } else if (ch > 0.1) {
        // Has ceiling but floor is at surface: dark teal
        ctx.fillStyle = '#1a3a3a';
      } else {
        // Normal surface: green/brown
        var brightness = Math.max(0, Math.min(1, 0.3 + fh * 0.2));
        ctx.fillStyle = 'rgb(' + Math.floor(40 + 60 * brightness) + ',' + Math.floor(60 + 80 * brightness) + ',' + Math.floor(20 + 30 * brightness) + ')';
      }
      ctx.fillRect(mp.x, mp.y, cs, cs);
    }
  }

  // Draw walls from global grid
  if (grid && gridW > 0 && gridH > 0) {
    var wCellSize = typeof cellSize !== 'undefined' ? cellSize : 12;
    // Map mesh coords to wall grid coords and draw wall cells
    var wMinGX = Math.max(0, Math.floor((centerWX - viewRadius) / wCellSize));
    var wMaxGX = Math.min(gridW - 1, Math.ceil((centerWX + viewRadius) / wCellSize));
    var wMinGY = Math.max(0, Math.floor((centerWY - viewRadius) / wCellSize));
    var wMaxGY = Math.min(gridH - 1, Math.ceil((centerWY + viewRadius) / wCellSize));
    var wPx = wCellSize * pxPerUnit;
    for (var wgy = wMinGY; wgy <= wMaxGY; wgy++) {
      for (var wgx = wMinGX; wgx <= wMaxGX; wgx++) {
        if (grid[wgy * gridW + wgx]) {
          var wp = worldToMap(wgx * wCellSize, wgy * wCellSize);
          // Color walls by collision: red = blocks player, green = player above (pass-through)
          var _wcx = wgx * wCellSize + wCellSize * 0.5;
          var _wcy = wgy * wCellSize + wCellSize * 0.5;
          var _wtopZ = getWallTopZ(_wcx, _wcy);
          var _pz = pos.floorZ || 60;
          ctx.fillStyle = (_pz > _wtopZ + 5) ? '#2a8' : '#c44';
          ctx.fillRect(wp.x, wp.y, Math.max(1, Math.ceil(wPx)), Math.max(1, Math.ceil(wPx)));
        }
      }
    }
  }

  // Draw entrance markers (deepCaveEntrances already in local coords)
  for (var i = 0; i < deepCaveEntrances.length; i++) {
    var ex = deepCaveEntrances[i].x;
    var ey = deepCaveEntrances[i].y;
    var ep = worldToMap(ex, ey);
    ctx.beginPath();
    ctx.arc(ep.x, ep.y, 5 * sc, 0, Math.PI * 2);
    ctx.fillStyle = '#00ff88';
    ctx.fill();
    ctx.strokeStyle = '#ffffff';
    ctx.lineWidth = 1.5;
    ctx.stroke();
    // Label
    ctx.fillStyle = '#00ff88';
    ctx.font = 'bold ' + Math.round(8 * sc) + 'px monospace';
    ctx.fillText('CAVE', ep.x + 7 * sc, ep.y + 3 * sc);
  }

  // Draw entrance visibility radius
  var visR = 14 * gs;  // _entranceVisRadius * gridSize
  for (var i = 0; i < deepCaveEntrances.length; i++) {
    var ex = deepCaveEntrances[i].x;
    var ey = deepCaveEntrances[i].y;
    var ep = worldToMap(ex, ey);
    ctx.beginPath();
    ctx.arc(ep.x, ep.y, visR * pxPerUnit, 0, Math.PI * 2);
    ctx.strokeStyle = 'rgba(0,255,255,0.4)';
    ctx.lineWidth = 1;
    ctx.stroke();
  }

  // Draw player position and direction
  var pp = worldToMap(pos.x, pos.y);
  // Camera FOV cone
  var fovHalf = (cam.fov || 1.2) / 2;
  var coneLen = 60 * pxPerUnit;
  ctx.beginPath();
  ctx.moveTo(pp.x, pp.y);
  ctx.lineTo(pp.x + Math.cos(cam.ang - fovHalf) * coneLen, pp.y + Math.sin(cam.ang - fovHalf) * coneLen);
  ctx.lineTo(pp.x + Math.cos(cam.ang + fovHalf) * coneLen, pp.y + Math.sin(cam.ang + fovHalf) * coneLen);
  ctx.closePath();
  ctx.fillStyle = 'rgba(255,255,0,0.15)';
  ctx.fill();
  ctx.strokeStyle = 'rgba(255,255,0,0.5)';
  ctx.lineWidth = 1;
  ctx.stroke();

  // Player dot
  ctx.beginPath();
  ctx.arc(pp.x, pp.y, 3 * sc, 0, Math.PI * 2);
  ctx.fillStyle = '#ff4444';
  ctx.fill();
  ctx.strokeStyle = '#fff';
  ctx.lineWidth = 1;
  ctx.stroke();

  // Direction line
  ctx.beginPath();
  ctx.moveTo(pp.x, pp.y);
  ctx.lineTo(pp.x + Math.cos(cam.ang) * 15 * sc, pp.y + Math.sin(cam.ang) * 15 * sc);
  ctx.strokeStyle = '#ff4444';
  ctx.lineWidth = 2;
  ctx.stroke();

  // Distance label
  ctx.fillStyle = '#fff';
  ctx.font = Math.round(9 * sc) + 'px monospace';
  ctx.fillText('dist: ' + nearDist.toFixed(0) + 'u', mapX + 4, mapY + mapSize - 4);

  // Title
  ctx.fillStyle = '#44ee88';
  ctx.font = 'bold ' + Math.round(9 * sc) + 'px monospace';
  ctx.fillText('CONTROL MAP', mapX + 4, mapY + Math.round(10 * sc));

  // Deferred quad count
  ctx.fillStyle = '#00ffff';
  ctx.fillText('deferred: ' + (_deferredCaveQuads ? _deferredCaveQuads.length : 0), mapX + 4, mapY + mapSize - 4 - Math.round(12 * sc));

  ctx.restore();
}

// Global reference for deferred quad count in control map
var _deferredCaveQuads = [];
