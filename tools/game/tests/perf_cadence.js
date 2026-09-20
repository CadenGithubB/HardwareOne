(function () {
  var source = slurp('assets/games/src/20-diagnostics.js');
  (0, eval)(source.slice(0, source.indexOf('function _pt(')));
  var n = 0;
  function check(label, ok) { if (!ok) throw Error(label); n++; __out('PASS '+label); }
  function near(a,b) { return Math.abs(a-b)<1e-7; }
  check('no measured intervals means no invented FPS', getPerfFrameCadence().fps === 0);
  recordPerfFrameCadence(0,1);
  recordPerfFrameCadence(1000/60,1);
  recordPerfFrameCadence(2000/60,1);
  check('60Hz callbacks report 60FPS independent of CPU cost', near(getPerfFrameCadence().fps,60));
  recordPerfFrameCadence(10000,2);
  check('new game loop discards prior run and pause gap', getPerfFrameCadence().samples===0);
  recordPerfFrameCadence(10040,2);
  check('slow actual delivery is reported', near(getPerfFrameCadence().fps,25));
  recordPerfFrameCadence(NaN,2);
  recordPerfFrameCadence(10040,2);
  check('invalid and duplicate timestamps add no samples', getPerfFrameCadence().samples===1);
  for(var i=1;i<=240;i++) recordPerfFrameCadence(10040+i*20,2);
  check('bounded ring replaces old slow intervals', getPerfFrameCadence().samples===120 && near(getPerfFrameCadence().fps,50));
  var G=(0,eval)('this');
  G.document={};G.canvas={};G.CAVE_TEST_MODE=true;G.DEBUG_CAVE=false;
  check('hidden cave diagnostic map does no drawing work', !caveControlMapVisible());
  G.DEBUG_CAVE=true;
  check('visible cave diagnostic map remains available', caveControlMapVisible());
  G.document.fullscreenElement=G.canvas;
  check('game-only native fullscreen hides the separate diagnostic map', !caveControlMapVisible());
  G.document.fullscreenElement={};
  check('fullscreen of another element does not assume game-only display', caveControlMapVisible());
  G.CAVE_TEST_MODE=false;
  check('normal gameplay does not run cave-only diagnostics', !caveControlMapVisible());
  __out('PERF_CADENCE_RESULT PASS '+n);
}());
undefined;
