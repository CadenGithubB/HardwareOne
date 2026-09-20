// Exercise actual portal, spatial, and lighting helpers without the audit lab.
// These assert exposure contracts, not final-image visual readability.
(function () {
  var G = (0, eval)('this');
  G.floorMesh = null;
  G.pos = {x: 264, y: 18, floorZ: -140};
  G.cam = {x: 264, y: 18, z: -140};
  G.settings = {dayNight: false};
  G.dayTime = 0.5; G.daySpeed = 0;
  G.playerUnderground = false;
  G._cacheStats = {floorH: {hits: 0, misses: 0, size: 0}};
  (0, eval)(slurp('assets/games/src/05-endless-world.js'));
  (0, eval)(slurp('assets/games/src/06-floor-queries.js'));
  (0, eval)(slurp('assets/games/src/17-input-lighting-update.js'));
  var portal = {x: 120, y: 18, angle: 0, halfWidth: 54,
    approachLength: 480, innerLength: 240, floorH: -5,
    ceilingH: -1, ceilH: 4, minCover: 1};
  G.deepCaveEntrances = [portal];
  G.floorMesh = {gridSize: 12, w: 48, h: 4, layerCount: []};
  for (var li = 0; li < 5; li++) {
    G.floorMesh['l' + li + 'Type'] = [];
    G.floorMesh['l' + li + 'TopZ'] = [];
  }
  for (var i = 0; i < 192; i++) {
    G.floorMesh.layerCount[i] = 3;
    G.floorMesh.l0Type[i] = 1; G.floorMesh.l0TopZ[i] = -5;
    G.floorMesh.l1Type[i] = 2; G.floorMesh.l1TopZ[i] = -1;
    G.floorMesh.l2Type[i] = 4; G.floorMesh.l2TopZ[i] = 0;
  }
  var checked = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checked++; __out('PASS ' + name);
  }
  function close(a, b) { return Math.abs(a - b) < 1e-9; }
  function place(x, feetH) {
    G.pos.x = G.cam.x = x;
    G.pos.floorZ = G.cam.z = G.meshHeightToPlayerZ(feetH);
  }
  G.updateDayNight(0);
  check('disabled day cycle still detects underground player', G.playerUnderground);
  check('disabled day cycle still detects underground camera', G.isRenderCameraUnderground());
  check('disabled day cycle keeps dim readable deep exposure', close(G.ambientLight, 0.45));
  check('disabled day cycle preserves exterior noon exposure', close(G.renderSurfaceAmbient, 0.9));
  var fringe = G.getPortalRenderBlendAt(114, 18, true);
  var mouth = G.getPortalRenderBlendAt(122, 18, true);
  var middle = G.getPortalRenderBlendAt(192, 18, true);
  var deep = G.getPortalRenderBlendAt(264, 18, true);
  check('covered outward mesh fringe remains mouth-bright', fringe === 0);
  check('entry exposure begins gently inside mouth', mouth > 0 && mouth < 0.01);
  check('covered entry exposure increases monotonically', fringe < mouth && mouth < middle && middle < deep);
  check('deep entry reaches interior exposure', deep === 1);
  check('uncovered terrain never gains cave exposure', G.getPortalRenderBlendAt(264, 18, false) === 0);
  check('exterior visible from cave retains exterior light', close(G.getCaveRenderLightAt(114, 18, false), 0.9));
  G.settings.dayNight = true;
  G.dayTime = 0.0;
  G.updateDayNight(0);
  check('deep night camera exposure has readability floor', G.ambientLight >= 0.4);
  check('deep night cave shading has readability floor', G.renderCaveAmbient >= 0.4 && G.getCaveRenderLightAt(264, 18, true) >= 0.4);
  check('deep night exterior exposure remains unchanged', close(G.renderSurfaceAmbient, 0.32));
  place(264, 0);
  G.updateDayNight(0);
  check('walking above cave stays outdoors', !G.playerUnderground && !G.isRenderCameraUnderground());
  check('outdoor night camera retains normal exposure', close(G.ambientLight, 0.32) && G.renderCameraCaveBlend === 0);
  G.dayTime = 0.5;
  G.updateDayNight(0);
  check('outdoor daylight receives no cave darkening', close(G.ambientLight, 0.9));
  place(114, -5);
  G.updateDayNight(0);
  check('covered fringe cannot flicker to deep-cave exposure', G.playerUnderground && G.renderCameraCaveBlend === 0 && close(G.ambientLight, 0.9));
  __out('CAVE_LIGHTING_RESULT PASS ' + checked);
}());
undefined;
