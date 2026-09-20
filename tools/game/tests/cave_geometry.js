// Shipping generation contracts. Uses production terrain and cave functions,
// not the optional audit lab, browser, copied formulas, or generated header.
(function () {
  var G = (0, eval)('this');
  G.window = {};
  G.CHUNK_SIZE = 480;
  G.ENDLESS_MODE = true;
  G.CAVE_TEST_MODE = true;
  G._biomeSeed = 0;
  (0, eval)(slurp('assets/games/src/03-level-generation.js'));
  (0, eval)(slurp('assets/games/src/05-endless-world.js'));
  var checked = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checked++;
    __out('PASS ' + name);
  }
  function seedWorld(seed, kind) {
    G.WORLD_SEED = seed;
    G.noiseSeed = (seed * 7 + 42) | 0;
    G._biomeSeed = (seed * 13 + 999) | 0;
    G._geoSeed = (seed * 19 + 5555) | 0;
    G._ampBoost = 1.2;
    G.endlessCaveNetworks = {};
    G.window._caveTestKindOverride = kind;
  }
  function query(net, x, y) {
    return G.queryCaveGeometry(x, y, [net], G.getEndlessNaturalSurfaceH(x, y));
  }
  [[12345, 'descending'], [5668, 'hillside'], [6290, 'hillside']].forEach(function (fixture) {
    seedWorld(fixture[0], fixture[1]);
    var net = G.generateCaveNetwork(0, 0), e = net.entrances[0];
    var label = fixture[0] + ' ';
    check(label + 'terrain fits requested entrance', e.kind === fixture[1]);
    check(label + 'portal owns complete height contract', e.ceilingH === e.floorH + e.ceilH &&
      e.depth === -e.floorH && e.ceilH >= 3.6 && e.minCover >= 1 && e.halfWidth > 40);
    check(label + 'one connected first recipe', net.entrances.length === 1 &&
      net.route.length === net.corridors.length + 1 && net.chambers.length === 1);
    var co = Math.cos(e.angle), si = Math.sin(e.angle);
    var outside = query(net, e.x - co * 0.01, e.y - si * 0.01);
    var inside = query(net, e.x + co * 0.01, e.y + si * 0.01);
    check(label + 'mouth floor is continuous', Math.abs(outside.floorH - inside.floorH) < 0.00001);
    check(label + 'roof starts at portal, never outside', outside.ceilZ === null && !outside.covered &&
      inside.covered && inside.ceilZ - inside.floorH >= 3.6);
    var openSky = true, slopeOK = true, previous = null;
    for (var a = e.approachLength; a > 0; a -= 6) {
      var x = e.x - co * a, y = e.y - si * a;
      var q = query(net, x, y);
      var h = q ? q.floorH : G.getEndlessNaturalSurfaceH(x, y);
      if (q && q.ceilZ !== null) openSky = false;
      if (previous !== null && Math.abs(h - previous) > 0.5) slopeOK = false;
      previous = h;
    }
    check(label + 'entire approach remains open sky', openSky);
    check(label + 'walking slope needs no entrance step exception', slopeOK);
    check(label + 'outside footprint leaves terrain unchanged',
      query(net, e.x - co * (e.approachLength + 1), e.y - si * (e.approachLength + 1)) === null);
    var routeOK = true, coverOK = true;
    for (var ci = 0; ci < net.corridors.length; ci++) {
      var c = net.corridors[ci], len = Math.hypot(c.x2 - c.x1, c.y2 - c.y1);
      for (var t = 0.001; t <= 1; t += 6 / len) {
        var q = query(net, c.x1 + (c.x2 - c.x1) * t, c.y1 + (c.y2 - c.y1) * t);
        if (!q || !q.covered || !q.wallCarve || q.ceilZ - q.floorH < 3.6 - 1e-7 ||
            Math.abs(q.floorH - e.floorH) > 1e-7) routeOK = false;
        if (!q || q.surfaceH - q.ceilZ < e.minCover - 1e-7) coverOK = false;
      }
    }
    check(label + 'connected route has upright clearance', routeOK);
    check(label + 'route has rock cover above ceiling', coverOK);
    var chamber = net.chambers[0], chamberQ = query(net, chamber.cx, chamber.cy);
    check(label + 'terminal chamber meets same support', chamberQ && chamberQ.covered &&
      Math.abs(chamberQ.floorH - e.floorH) < 1e-7);
    var reward = G.caveChamberContentPoint(chamber, 0, 0);
    var rewardQ = query(net, reward.x, reward.y);
    check(label + 'chunk-edge chamber retains a reachable reward site', rewardQ && rewardQ.wallCarve &&
      rewardQ.covered && Math.abs(rewardQ.floorH - e.floorH) < 1e-7);
    var actorSitesValid = true;
    for (var ox = -1; ox <= 1; ox += 2) for (var oy = -1; oy <= 1; oy += 2) {
      var actor = G.caveChamberContentPoint(chamber, ox * chamber.radius * 0.25, oy * chamber.radius * 0.25);
      var actorQ = query(net, actor.x, actor.y);
      if (!actorQ || !actorQ.wallCarve || !actorQ.covered ||
          Math.floor(actor.x / G.CHUNK_SIZE) !== reward.ownerCX ||
          Math.floor(actor.y / G.CHUNK_SIZE) !== reward.ownerCY) actorSitesValid = false;
    }
    check(label + 'seeded actor sites stay walkable in unique owner chunk', actorSitesValid);
    var outsideFrame = G.sampleCavePortal(e, e.x - co * 60, e.y - si * 60);
    check(label + 'shared portal frame is outward positive', outsideFrame && outsideFrame.along > 59 &&
      !outsideFrame.covered && outsideFrame.inCore);
    var copy = JSON.stringify(net);
    G.endlessCaveNetworks = {};
    check(label + 'same seed regenerates identical network', JSON.stringify(G.generateCaveNetwork(0, 0)) === copy);
  });
  // Production gating, not a cave-test-only generation path. Search a bounded
  // sample of actual biome regions and assert generated cave contracts.
  G.CAVE_TEST_MODE = false;
  seedWorld(12345, null);
  var normalCount = 0, normalValid = true;
  for (var rx = -8; rx <= 8; rx++) {
    for (var ry = -8; ry <= 8; ry++) {
      var n = G.generateCaveNetwork(rx, ry);
      if (!n) continue;
      normalCount++;
      var p = n.entrances[0];
      if (!isFinite(p.floorH) || !p.kind || !n.route.length) normalValid = false;
    }
  }
  check('ordinary seeded biome generation still creates caves', normalCount > 0 && normalValid);
  var edge = G.caveChamberContentPoint({cx: 480, cy: -0.001}, -24, 24);
  check('exact and negative chunk boundaries have unique content ownership', edge.ownerCX === 1 && edge.ownerCY === -1);
  check('edge content is inset rather than discarded', edge.x === 498 && edge.y === -18);
  // Lifecycle support initialization shares the production layer readers.
  G._cacheStats = {floorH: {hits: 0, misses: 0, size: 0}};
  (0, eval)(slurp('assets/games/src/06-floor-queries.js'));
  G.floorMesh = {gridSize: 12, w: 1, h: 1, layerCount: [3],
    l0Type: [1], l0TopZ: [-5], l1Type: [2], l1TopZ: [-1], l2Type: [4], l2TopZ: [2]};
  G.pos = {x: 6, y: 6, floorZ: 60};
  G.cam = {};
  G.jumpAirborne = true; G.jumpVelZ = 300;
  G.settlePlayerAtSpawn(true);
  check('ordinary spawn chooses top support immediately', G.pos.floorZ === 140 && G.cam.z === 140);
  check('new world clears inherited jump state', !G.jumpAirborne && G.jumpVelZ === 0);
  G.settlePlayerAtSpawn(false);
  check('cave fixture spawn can choose its intended lower support', G.pos.floorZ === -140);
  __out('CAVE_GEOMETRY_RESULT PASS ' + checked);
}());
undefined;
