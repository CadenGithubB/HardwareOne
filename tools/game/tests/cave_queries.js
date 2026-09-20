// Focused spatial contracts; no browser, game startup, or optional audit lab.
(function () {
  var G = (0, eval)('this');
  G.floorMesh = null;
  G.pos = {floorZ: 60};
  G._cacheStats = {floorH: {hits: 0, misses: 0, size: 0}};
  (0, eval)(slurp('assets/games/src/06-floor-queries.js'));
  var checked = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checked++;
    __out('PASS ' + name);
  }
  function cell(layers) {
    G.floorMesh = {gridSize: 12, w: 1, h: 1, layerCount: [layers.length]};
    for (var n = 0; n < 5; n++) {
      G.floorMesh['l' + n + 'Type'] = [layers[n] ? layers[n][0] : 0];
      G.floorMesh['l' + n + 'TopZ'] = [layers[n] ? layers[n][1] : 0];
    }
  }
  cell([[1, -5], [2, -1], [4, 0]]);
  check('surface stays on roof', G.getWalkableLayerTopAt(6, 6, 0).idx === 2);
  check('cave stays below roof', G.getWalkableLayerTopAt(6, 6, -5).idx === 0);
  check('roof is not underground', !G.getCaveSpaceAt(6, 6, 0).underground);
  check('easing onto roof is not underground', !G.getCaveSpaceAt(6, 6, -0.25).underground);
  check('cave ceiling is actual nearest overhead', G.getCaveSpaceAt(6, 6, -5).ceilingH === -1);
  cell([[1, -5], [2, -1]]);
  check('missing cap never allows falling through ceiling', G.getWalkableLayerTopAt(6, 6, 0).action === 'blocked');
  cell([[1, 4], [2, 6]]);
  check('insufficient headroom blocks passage', G.getWalkableLayerTopAt(6, 6, 4).action === 'blocked');
  cell([[1, 4], [2, 8]]);
  check('hillside cave above zero is supported', G.getWalkableLayerTopAt(6, 6, 4).action === 'walk');
  check('hillside cave above zero is underground', G.getCaveSpaceAt(6, 6, 4).underground);
  cell([[1, -0.5]]);
  check('gentle descent is walking', G.getWalkableLayerTopAt(6, 6, 0).action === 'walk');
  cell([[1, -3]]);
  G.deepCaveEntrances = [{x: 6, y: 6}];
  check('real drop falls even beside an entrance', G.getWalkableLayerTopAt(6, 6, 0).action === 'fall');
  check('fall names its real landing layer', G.getWalkableLayerTopAt(6, 6, 0).topH === -3);
  cell([[1, 2]]);
  var rise = G.getWalkableLayerTopAt(6, 6, 0);
  check('steep rise blocks with actual height', rise.action === 'blocked' && rise.topH === 2);
  var absent = G.getWalkableLayerTopAt(-1, 6, 0);
  check('missing geometry is not fabricated zero', absent.action === 'missing' && absent.topH === null);
  G.pos.floorZ = 0;
  check('player Z zero remains legitimate', G.getPlayerFloorH() === -1.5);
  check('legacy physics conversion round trips', G.meshHeightToPlayerZ(G.getPlayerFloorH()) === 0);
  cell([[1, 0.5]]);
  check('airborne cannot snap up onto a higher floor', G.getWalkableLayerTopAt(6, 6, 0, {stepUp: 0}).action === 'blocked');
  cell([[1, 0], [2, 4], [1, 5], [2, 9], [4, 10]]);
  check('stacked upper passage keeps its own support', G.getWalkableLayerTopAt(6, 6, 5).idx === 2);
  check('stacked lower passage keeps its own support', G.getWalkableLayerTopAt(6, 6, 0).idx === 0);
  cell([[2, -1], [1, 4]]);
  check('legacy floor sampling never treats ceiling as support', G.getFloorHeightAt(6, 6) === 4);
  cell([[1, 7]]);
  check('same-frame mesh replacement invalidates floor cache', G.getFloorHeightAt(6, 6) === 7);
  var cacheHits = G._cacheStats.floorH.hits;
  check('unchanged mesh retains useful floor cache', G.getFloorHeightAt(6, 6) === 7 && G._cacheStats.floorH.hits === cacheHits + 1);
  __out('CAVE_QUERY_RESULT PASS ' + checked);
}());
undefined;
