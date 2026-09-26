// Ruined Hut footprint contracts: accessible, low, asymmetric and rotatable.
(function () {
  var G = (0, eval)('this'), checks = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checks++; __out('PASS ' + name);
  }
  function key(x, y) { return x + ',' + y; }
  function sortedCells(cells) {
    return cells.map(function (c) { return [c.dx, c.dy, c.wallH]; })
      .sort(function (a, b) { return a[1] - b[1] || a[0] - b[0]; });
  }

  var source = slurp('assets/games/src/05-endless-world.js');
  var start = source.indexOf('function rotateRuinCellOffset(');
  var end = source.indexOf('function generateChunk(', start);
  check('production hut footprint helpers found', start >= 0 && end > start);
  (0, eval)(source.slice(start, end));

  var base = G.buildHutRuinCells(0);
  check('hut uses ten remnants instead of a seven-cube ring', base.length === 10);
  check('hut footprint is deterministic',
    JSON.stringify(base) === JSON.stringify(G.buildHutRuinCells(0)));
  var heights = {}, minH = Infinity, maxH = -Infinity;
  base.forEach(function (c) {
    heights[c.wallH] = 1; minH = Math.min(minH, c.wallH); maxH = Math.max(maxH, c.wallH);
  });
  check('broken wall profile has several deliberate heights',
    Object.keys(heights).length >= 7 && minH >= 0.14 && maxH <= 0.24);
  check('highest hut remnant stays at or below the legacy eye height', maxH * 240 <= 60);

  var out = [[0,-1],[1,0],[0,1],[-1,0]];
  var right = [[1,0],[0,1],[-1,0],[0,-1]];
  var allOpen = true, allRotated = true;
  for (var facing = 0; facing < 4; facing++) {
    var cells = G.buildHutRuinCells(facing), occupied = {};
    cells.forEach(function (c) { occupied[key(c.dx, c.dy)] = 1; });
    // Three player-width lanes remain open from the center through the front.
    for (var lateral = -1; lateral <= 1; lateral++) {
      for (var step = 0; step <= 2; step++) {
        var x = out[facing][0] * step + right[facing][0] * lateral;
        var y = out[facing][1] * step + right[facing][1] * lateral;
        if (occupied[key(x, y)]) allOpen = false;
      }
    }
    var expected = base.map(function (c) {
      var p = G.rotateRuinCellOffset(c.dx, c.dy, facing);
      return {dx:p.dx, dy:p.dy, wallH:c.wallH};
    });
    if (JSON.stringify(sortedCells(cells)) !== JSON.stringify(sortedCells(expected))) allRotated = false;
  }
  check('all four facings keep a three-cell doorway and center path open', allOpen);
  check('all four facings are exact quarter-turns of one authored footprint', allRotated);
  check('front-right corner is deliberately missing from the ruined outline',
    !base.some(function (c) { return c.dx === 2 && c.dy === -1; }));

  var generation = source.slice(source.indexOf('// ── Ruin generation'), source.indexOf('// Per-chunk terrain debug'));
  check('generation reserves and de-clutters a dedicated hut pad',
    generation.indexOf('ruinPadRadius = ruinType === \'hut\' ? 3 : 0') >= 0 &&
    generation.indexOf('cScatter = cScatter.filter(_outsideHutPadWorld)') >= 0 &&
    generation.indexOf('cDecors = cDecors.filter(function(d)') >= 0);
  var wallSource = slurp('assets/games/src/12-render-core-walls.js');
  check('authored ruin stone is excluded from forest bark and canopy paths',
    (wallSource.match(/f\.biome === 'forest' && !f\.authored/g) || []).length === 3);
  __out('RUIN_GENERATION_RESULT PASS ' + checks);
}());
undefined;
