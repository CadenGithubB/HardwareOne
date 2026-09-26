(function () {
  var G = (0, eval)('this'), checks = 0, updates = 0, paints = 0, scheduled = 0, cancels = 0;
  function check(label, ok) { if (!ok) throw Error(label); checks++; __out('PASS ' + label); }
  var source = slurp('assets/games/src/21-frame-loop.js');
  Object.assign(G, {
    GAME_CONFIG: {physics: {fixedDt: 1 / 60, fixedDtMs: 1000 / 60, maxSteps: 4}},
    running: true, lastUpdate: 100, menuOpen: false, settingsOpen: false,
    shopOpen: false, inventoryOpen: false, forgeOpen: false, gameOverState: false,
    performance: {now: function () { return 1; }}, window: {},
    recordPerfFrameCadence: function () {}, _perfRingIdx: 0, PERF_HISTORY_LEN: 10,
    _perfFrameTotals: [], _perfStageHistory: {}, _perfSecIdx: 0,
    _perfSecFrameCount: [0], _perfSecTotalMs: [0], _perfTickSecondBucket: function () {},
    _pt: function (_, fn) { fn(); }, handleInput: function () {},
    gameUpdate: function () { updates++; }, renderFrame: function () { paints++; },
    cancelPendingMissileCasts: function () { cancels++; }, DEBUG_PERF: false, ENDLESS_MODE: false,
    requestAnimationFrame: function () { scheduled++; }
  });
  (0, eval)(source.slice(source.indexOf('var FIXED_DT =')));
  loop(120, 0);
  check('active frame advances simulation and renders', updates === 1 && paints === 1);
  menuOpen = true;
  loop(5000, 0);
  check('pause freezes simulation and discards accumulated time', updates === 1 && _physicsAccum === 0);
  check('pause renders controls and cancels queued spells', paints === 2 && cancels === 1 && scheduled === 2);
  menuOpen = false; settingsOpen = true;
  loop(9000, 0);
  check('settings protect the player from live simulation', updates === 1 && _physicsAccum === 0 && paints === 3 && cancels === 2);
  settingsOpen = false;
  loop(9020, 0);
  check('resume runs one normal step without catching up paused time', updates === 2 && paints === 4 && _physicsAccum < FIXED_DT_MS);
  var scheduledBefore = scheduled;
  loop(9040, 99);
  check('stale loop never updates paints or reschedules', updates === 2 && paints === 4 && scheduled === scheduledBefore);
  running = false;
  loop(9040, 0);
  check('stopped game never advances', updates === 2 && scheduled === scheduledBefore);

  var layers = [];
  G.overviewActive = true;
  G.drawDebugOverview = function () { layers.push('overview'); };
  G.drawMenuOverlay = function () { layers.push('pause'); };
  G.drawSettingsOverlay = function () { layers.push('settings'); };
  (0, eval)(source.slice(0, source.indexOf('var FIXED_DT =')));
  renderFrame();
  check('overview keeps pause and settings visible above the map', layers.join(',') === 'overview,pause,settings');
  check('frame loop did not crash under tested transitions', !window._loopErrCount);
  __out('FRAME_PAUSE_RESULT PASS ' + checks);
}());
undefined;
