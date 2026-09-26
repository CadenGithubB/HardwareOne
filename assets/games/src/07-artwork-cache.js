// Canvas2D artwork pilot. Only appearance is cached: projection, support,
// ordering and the caller's scene-depth clip still run for every visible prop.
// Bounds are relative to the existing recipe's (x,y) anchor, in size units.
var FLOOR_ARTWORK = Object.freeze({
  fern: Object.freeze({label:'Fern', bounds:Object.freeze([-0.5,-0.3,0.5,0.3]), materials:Object.freeze([])}),
  fallen_log: Object.freeze({label:'Fallen log', bounds:Object.freeze([-0.95,-0.45,0.95,0.45]), materials:Object.freeze([])}),
  skull: Object.freeze({label:'Skull', bounds:Object.freeze([-0.45,-0.52,0.45,0.32]), materials:Object.freeze(['bone'])}),
  rubble: Object.freeze({label:'Rubble', bounds:Object.freeze([-0.82,-0.62,0.82,0.62]), materials:Object.freeze(['rubbleStone'])})
});

// Opt-in until the moving-scene pilot is accepted. No localStorage or save data.
var FLOOR_ARTWORK_CACHE_ENABLED = false;
var FLOOR_ARTWORK_CACHE_BYTE_LIMIT = 4 * 1024 * 1024;
var FLOOR_ARTWORK_CACHE_ENTRY_LIMIT = 384;
var _floorArtworkCache = new Map();
var _floorArtworkBytes = 0;
var _floorArtworkMaterials = null, _floorArtworkPainter = null;
var _floorArtworkBuilds = 0, _floorArtworkBuildMs = 0;
var _floorArtworkFailed = false;
var _floorArtworkStats = {hits:0, misses:0, builds:0, evictions:0, fallbacks:0, buildMs:0};

function floorArtworkNow() {
  return typeof performance !== 'undefined' ? performance.now() : Date.now();
}

function clearFloorArtworkCache() {
  _floorArtworkCache.forEach(function(entry) { entry.canvas.width = entry.canvas.height = 1; });
  _floorArtworkCache.clear();
  _floorArtworkBytes = 0;
  _floorArtworkMaterials = GAME_MATERIALS;
  _floorArtworkPainter = paintFloorItem;
  _floorArtworkFailed = false;
}

function setFloorArtworkCacheEnabled(enabled) {
  FLOOR_ARTWORK_CACHE_ENABLED = !!enabled;
  if (!FLOOR_ARTWORK_CACHE_ENABLED) clearFloorArtworkCache();
}

function getFloorArtworkCacheStats() {
  return {enabled:FLOOR_ARTWORK_CACHE_ENABLED, entries:_floorArtworkCache.size,
    pixelBytes:_floorArtworkBytes, byteLimit:FLOOR_ARTWORK_CACHE_BYTE_LIMIT,
    hits:_floorArtworkStats.hits, misses:_floorArtworkStats.misses,
    builds:_floorArtworkStats.builds, evictions:_floorArtworkStats.evictions,
    fallbacks:_floorArtworkStats.fallbacks, buildMs:_floorArtworkStats.buildMs};
}

function beginFloorArtworkFrame() {
  _floorArtworkBuilds = 0; _floorArtworkBuildMs = 0;
  // Compiled materials are immutable; identity detects a replacement catalog.
  // Source edits use reload; hot-swapping the recipe also invalidates artwork.
  if (_floorArtworkMaterials !== GAME_MATERIALS || _floorArtworkPainter !== paintFloorItem)
    clearFloorArtworkCache();
}

function floorArtworkSurfaceSupported(ctx) {
  // Compositing/shadows/filters applied to each primitive cannot in general be
  // replaced by applying them once to the flattened image. Keep the old path.
  if (ctx.globalCompositeOperation !== 'source-over' || ctx.shadowBlur !== 0 ||
      ctx.shadowColor !== 'rgba(0, 0, 0, 0)' ||
      ctx.shadowOffsetX !== 0 || ctx.shadowOffsetY !== 0 ||
      (ctx.filter && ctx.filter !== 'none') || ctx.getLineDash().length) return false;
  var t = ctx.getTransform();
  return t.a === 1 && t.b === 0 && t.c === 0 && t.d === 1 && t.e === 0 && t.f === 0;
}

function drawFloorArtwork(ctx, type, variant, seed, x, y, size) {
  var recipe = Object.prototype.hasOwnProperty.call(FLOOR_ARTWORK, type) ? FLOOR_ARTWORK[type] : null;
  if (!FLOOR_ARTWORK_CACHE_ENABLED || !recipe || _floorArtworkFailed ||
      !Number.isFinite(seed) || seed < 0 || seed >= 1 ||
      !Number.isInteger(variant) || variant < 0 || variant > 2 ||
      !Number.isInteger(size) || size < 6 || size > 70 ||
      !Number.isFinite(x) || !Number.isFinite(y) || !floorArtworkSurfaceSupported(ctx)) {
    _floorArtworkStats.fallbacks++;
    paintFloorItem(ctx, type, variant, seed, x, y, size); return;
  }
  if (_floorArtworkMaterials !== GAME_MATERIALS || _floorArtworkPainter !== paintFloorItem)
    clearFloorArtworkCache();
  // Exact seeds and projected integer sizes: no seed bucketing or upscaled art.
  // Position/fade are intentionally NOT cache keys, so moving props can reuse it.
  var key = type + ':' + variant + ':' + seed + ':' + size + ':' + ctx.lineCap + ':' + ctx.lineJoin + ':' + ctx.miterLimit;
  var entry = _floorArtworkCache.get(key);
  if (entry) {
    _floorArtworkStats.hits++;
    _floorArtworkCache.delete(key); _floorArtworkCache.set(key, entry);
  } else {
    _floorArtworkStats.misses++;
    // A cold view must not bake every visible prop in a single frame. This is
    // a soft time limit: one build may exceed it. The original recipe is safe.
    if (_floorArtworkBuilds >= 4 || _floorArtworkBuildMs >= 1) {
      _floorArtworkStats.fallbacks++;
      paintFloorItem(ctx, type, variant, seed, x, y, size); return;
    }
    var b = recipe.bounds, pad = 2;
    var left = Math.floor(b[0]*size)-pad, top = Math.floor(b[1]*size)-pad;
    var width = Math.ceil(b[2]*size)+pad-left, height = Math.ceil(b[3]*size)+pad-top;
    // 2x backing resolution keeps fractional placement crisp. Native raster
    // antialiasing can differ at edges; this is not a pixel-identical claim.
    var bytes = width * height * 16;
    if (bytes > FLOOR_ARTWORK_CACHE_BYTE_LIMIT || FLOOR_ARTWORK_CACHE_ENTRY_LIMIT < 1) {
      _floorArtworkStats.fallbacks++;
      paintFloorItem(ctx, type, variant, seed, x, y, size); return;
    }
    while (_floorArtworkCache.size && (_floorArtworkBytes + bytes > FLOOR_ARTWORK_CACHE_BYTE_LIMIT ||
        _floorArtworkCache.size >= FLOOR_ARTWORK_CACHE_ENTRY_LIMIT)) {
      var oldestKey = _floorArtworkCache.keys().next().value;
      var oldest = _floorArtworkCache.get(oldestKey);
      _floorArtworkBytes -= oldest.bytes;
      oldest.canvas.width = oldest.canvas.height = 1;
      _floorArtworkCache.delete(oldestKey); _floorArtworkStats.evictions++;
    }
    var started = floorArtworkNow(), canvas;
    try {
      canvas = document.createElement('canvas');
      canvas.width = width*2; canvas.height = height*2;
      var artCtx = canvas.getContext('2d');
      if (!artCtx) throw new Error('Canvas2D unavailable');
      // Preserve inherited stroke attributes not explicitly set by the recipe.
      artCtx.lineCap = ctx.lineCap; artCtx.lineJoin = ctx.lineJoin; artCtx.miterLimit = ctx.miterLimit;
      artCtx.scale(2,2);
      paintFloorItem(artCtx, type, variant, seed, -left, -top, size);
      entry = {canvas:canvas, left:left, top:top, width:width, height:height, bytes:bytes};
      _floorArtworkCache.set(key, entry); _floorArtworkBytes += bytes;
      _floorArtworkStats.builds++;
    } catch (error) {
      if (canvas) canvas.width = canvas.height = 1;
      _floorArtworkFailed = true;
    }
    var elapsed = floorArtworkNow() - started;
    _floorArtworkBuilds++; _floorArtworkBuildMs += elapsed; _floorArtworkStats.buildMs += elapsed;
    if (!entry) {
      _floorArtworkStats.fallbacks++;
      paintFloorItem(ctx, type, variant, seed, x, y, size); return;
    }
  }
  ctx.save();
  // These four legacy recipes set their own alpha before every primitive and
  // ignore the caller's alpha. Preserve that behavior, don't double-fade them.
  ctx.globalAlpha = 1;
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(entry.canvas, x+entry.left, y+entry.top, entry.width, entry.height);
  ctx.restore();
}
