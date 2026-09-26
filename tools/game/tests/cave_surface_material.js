// Cave cover is the original exterior material at the same world coordinate.
// A cap's storage role must not create its own tint, noise or moving patches.
(function () {
  var G = (0, eval)('this'), checked = 0;
  var output = (__engine === 'node' || __engine === 'deno') ? console.log.bind(console) : __out;
  var host = slurp('tools/game/tests/cave_occlusion_integration.js');
  var boundary = host.indexOf('  function check(name, okay) {');
  if (boundary < 0) throw Error('shipping browser host boundary missing');
  var readSource = slurp;
  if (__argv.indexOf('--restore-cap-jitter') >= 0) {
    // Restore the old production authoring at load time, before real seeded
    // generation. This is not a mutation of the assertion's expected data.
    G.slurp = function (path) {
      var source = readSource(path);
      if (path !== 'assets/games/src/05-endless-world.js') return source;
      var color = "var capCol = floorMesh.surfaceBiome[iL] || '#9bb06d';";
      var start = 'var layerTmpZ = new Float32Array(5)';
      if (source.indexOf(color) < 0 || source.indexOf(start) < 0) throw Error('cap material mutation anchor missing');
      source = source.replace(start, 'var capTestRngState = 1664525; function capTestRng(){ capTestRngState = (capTestRngState * 1103515245 + 12345) | 0; return ((capTestRngState >>> 0) % 10000) / 10000; }\n  ' + start);
      return source.replace(color, color + '\n' +
        'var capTestPacked = parseInt(capCol.slice(1), 16);' +
        'var capTestR = Math.min(255, (((capTestPacked >> 16) & 255) * (0.94 + capTestRng() * 0.12)) | 0);' +
        'var capTestG = Math.min(255, (((capTestPacked >> 8) & 255) * (0.94 + capTestRng() * 0.12)) | 0);' +
        'var capTestB = Math.min(255, ((capTestPacked & 255) * (0.94 + capTestRng() * 0.12)) | 0);' +
        "capCol = '#' + ('000000' + ((capTestR << 16) | (capTestG << 8) | capTestB).toString(16)).slice(-6);");
    };
  }
  try { (0, eval)(host.slice(0, boundary) + '}());'); }
  finally { G.slurp = readSource; }
  function check(name, okay) {
    if (!okay) throw Error(name);
    checked++; output('PASS ' + name);
  }
  function start(seed, kind) {
    G._caveTestSeedOverride = seed; G._caveTestKindOverride = kind;
    G.CAVE_TEST_MODE = G.ENDLESS_MODE = G.running = true;
    G.gameOverState = false; G.terrain = 'plains';
    Object.keys(G.caveTestFlags).forEach(function (key) { G.caveTestFlags[key] = false; });
    G.resetEndlessMode(); G.previewCaveView('above');
  }
  function sample() {
    var mesh = G.floorMesh, caps = {}, palette = {}, count = 0, dryCount = 0, stoneDistinct = 0;
    var materialSame = true, naturalSame = true, geometrySame = true, drySame = true, stoneSame = true;
    for (var i = 0; i < mesh.layerCount.length; i++) {
      var hasCap = false;
      for (var li = 0; li < mesh.layerCount[i]; li++) {
        if (mesh['l' + li + 'Type'][i] !== 4) continue;
        hasCap = true; count++;
        var wx = (i % mesh.w) * mesh.gridSize + G.windowOriginX;
        var wy = Math.floor(i / mesh.w) * mesh.gridSize + G.windowOriginY;
        var color = mesh['l' + li + 'Color'][i], topH = mesh['l' + li + 'TopZ'][i];
        var natural = G.getFloorColorBlended(wx, wy, G.getEndlessNaturalSurfaceH(wx, wy));
        if (color !== mesh.surfaceBiome[i]) materialSame = false;
        if (mesh.surfaceBiome[i] !== natural) naturalSame = false;
        if (topH !== mesh.surfaceH[i]) geometrySame = false;
        for (var ci = 0; ci < mesh.layerCount[i]; ci++) {
          if (mesh['l' + ci + 'Type'][i] === 2 && topH <= mesh['l' + ci + 'TopZ'][i]) geometrySame = false;
        }
        caps[wx + ',' + wy] = [color, topH, mesh.surfaceH[i]];
        palette[color] = true;
        if (mesh.l0Color[i] !== G.caveMaterialColorHex(mesh.caveStone[i])) stoneSame = false;
        if (mesh.l0Color[i] !== color) stoneDistinct++;
      }
      if (!hasCap && mesh.l0Type[i] === 1 && !mesh.water[i] && mesh.l0TopZ[i] === mesh.surfaceH[i]) {
        dryCount++;
        // Shoreline tint is a legitimate post-snapshot material treatment.
        // Ordinary ground must retain that actual authored chunk color, not
        // be reset to an earlier pristine palette by a cap-only repair.
        var groundWX = (i % mesh.w) * mesh.gridSize + G.windowOriginX;
        var groundWY = Math.floor(i / mesh.w) * mesh.gridSize + G.windowOriginY;
        var cx = Math.floor(groundWX / G.CHUNK_SIZE), cy = Math.floor(groundWY / G.CHUNK_SIZE);
        var chunk = G.chunks[cx + ',' + cy].floorMesh;
        var cmi = Math.floor((groundWY - cy * G.CHUNK_SIZE) / chunk.gridSize) * chunk.w +
          Math.floor((groundWX - cx * G.CHUNK_SIZE) / chunk.gridSize);
        if (mesh.l0Color[i] !== chunk.l0Color[cmi]) drySame = false;
      }
    }
    return {caps: caps, count: count, paletteCount: Object.keys(palette).length, dryCount: dryCount, stoneDistinct: stoneDistinct,
      materialSame: materialSame, naturalSame: naturalSame, geometrySame: geometrySame, drySame: drySame, stoneSame: stoneSame};
  }
  [[12345, 'descending'], [5668, 'hillside']].forEach(function (fixture) {
    var seed = fixture[0], kind = fixture[1], label = kind + ' ';
    start(seed, kind);
    var initial = sample();
    check(label + 'fixture contains the requested real cave and a substantial cover area',
      G.endlessCaveNetworks['0,0'].entrances[0].kind === kind && initial.count > 100);
    check(label + 'cap material equals pristine exterior at the same world XY', initial.materialSame);
    check(label + 'pristine exterior retains independently sampled biome and elevation color', initial.naturalSame);
    check(label + 'cap heights remain exactly on the original cover and above the ceiling', initial.geometrySame);
    check(label + 'natural spatial color variation remains instead of a flat replacement tint', initial.paletteCount > 20);
    check(label + 'ordinary dry surface material is untouched', initial.dryCount > 100 && initial.drySame);
    check(label + 'interior stone material remains distinct and unchanged', initial.stoneSame && initial.stoneDistinct > 50);
    var half = Math.floor(G.WINDOW_CHUNKS / 2);
    G.assembleWindow(G.windowCX + half + 1, G.windowCY + half);
    var shifted = sample(), shared = 0, stable = true;
    Object.keys(initial.caps).forEach(function (key) {
      if (!(key in shifted.caps)) return;
      shared++;
      if (JSON.stringify(initial.caps[key]) !== JSON.stringify(shifted.caps[key])) stable = false;
    });
    check(label + 'same world surface survives streaming-window reassembly unchanged', shared > 100 && stable);
    start(seed, kind);
    check(label + 'same seed reproduces identical world-space materials and heights',
      JSON.stringify(initial.caps) === JSON.stringify(sample().caps));
    output('SURFACE_MATERIAL_TRACE ' + JSON.stringify({seed: seed, caps: initial.count,
      retainedExteriorColors: initial.paletteCount, reassembledSamples: shared}));
  });
  output('CAVE_SURFACE_MATERIAL_RESULT PASS ' + checked);
}());
undefined;
