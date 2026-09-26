// Low-cost Sky V1 contracts: deterministic catalogs, coherent phase timing,
// bounded Canvas commands and no per-pixel or filter effects.
(function () {
  var G = (0, eval)('this'), checks = 0, calls = [];
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checks++; __out('PASS ' + name);
  }
  function record(name, args) {
    calls.push([name].concat(Array.prototype.slice.call(args).map(function(value) {
      return typeof value === 'number' ? Math.round(value * 1000000) / 1000000 : value;
    })));
  }
  function gradient(kind) {
    return {addColorStop:function() { record(kind + 'Stop', arguments); }};
  }
  function hash(value) {
    var text = JSON.stringify(value), h = 2166136261;
    for (var i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619);
    return h >>> 0;
  }

  G.ctx = {
    save:function(){record('save', arguments);}, restore:function(){record('restore', arguments);},
    beginPath:function(){record('beginPath', arguments);}, closePath:function(){record('closePath', arguments);},
    fill:function(){record('fill', arguments);}, fillRect:function(){record('fillRect', arguments);},
    rect:function(){record('rect', arguments);}, arc:function(){record('arc', arguments);},
    ellipse:function(){record('ellipse', arguments);}, moveTo:function(){record('moveTo', arguments);},
    lineTo:function(){record('lineTo', arguments);},
    createLinearGradient:function(){record('linearGradient', arguments); return gradient('linear');},
    createRadialGradient:function(){record('radialGradient', arguments); return gradient('radial');},
    globalAlpha:1, fillStyle:'#000'
  };
  G.canvas = {width:360,height:240};
  G.cam = {pitch:0,ang:0,fov:Math.PI/2};
  G.projScale = 180; G.ENDLESS_MODE = false; G.pos = {x:0,y:0};
  G.windowOriginX = 0; G.windowOriginY = 0; G.settings = {dayNight:true};
  G.dayTime = 0.5; G.rgbQ = function(r,g,b){ return 'rgb('+r+','+g+','+b+')'; };
  G.biomeNoise = function(){ return 0.416; };

  (0, eval)(slurp('assets/games/src/01-materials.js'));
  var source = slurp('assets/games/src/12-render-core-walls.js');
  var start = source.indexOf('var SKY_BIOME_ANCHORS =');
  var end = source.indexOf('function drawCalibration()', start);
  check('production sky renderer found', start >= 0 && end > start);
  var skySource = source.slice(start, end);
  (0, eval)(skySource);

  check('sky catalogs are fixed and deliberately small',
    G.SKY_STAR_CATALOG.length === 48 && G.SKY_CLOUD_CATALOG.length === 10);
  var night = G.getSkyPhase(0, true, {}), dawn = G.getSkyPhase(0.225, true, {});
  var noon = G.getSkyPhase(0.5, true, {}), dusk = G.getSkyPhase(0.775, true, {});
  var fixed = G.getSkyPhase(0.93, false, {});
  check('night exposes stars and places the moon overhead',
    night.daylight === 0 && night.stars === 1 && night.moonElevation > 0.99);
  check('dawn and dusk share smooth half-light transitions',
    Math.abs(dawn.daylight - 0.5) < 1e-9 && Math.abs(dusk.daylight - 0.5) < 1e-9 &&
    dawn.warm > 0.99 && dusk.warm > 0.99);
  check('noon puts the visible sun overhead with no stars',
    noon.daylight === 1 && noon.stars === 0 && noon.sunElevation > 0.99);
  check('disabled cycle resolves to fixed noon',
    fixed.time === 0.5 && fixed.daylight === 1 && fixed.stars === 0);

  var realNow = Date.now;
  Date.now = function(){ return 2000000000000; };
  function renderAt(time, noise, angle) {
    calls = []; G.dayTime = time; G.cam.ang = angle;
    G.biomeNoise = function(){ return noise; };
    G.drawSkybox3D();
    return JSON.parse(JSON.stringify(calls));
  }
  try {
    var dayCommands = renderAt(0.5, 0.416, Math.PI/2);
    var dayAgain = renderAt(0.5, 0.416, Math.PI/2);
    var nightCommands = renderAt(0, 0.416, Math.PI/2);
    var caveCommands = renderAt(0.5, 0.083, Math.PI/2);
    check('fixed inputs produce identical sky commands', hash(dayCommands) === hash(dayAgain));
    check('daylight batches a sun and both cloud bands',
      dayCommands.filter(function(c){return c[0] === 'arc';}).length >= 2 &&
      dayCommands.filter(function(c){return c[0] === 'ellipse';}).length > 0);
    check('night batches visible stars and a moon',
      nightCommands.filter(function(c){return c[0] === 'rect';}).length > 0 &&
      nightCommands.filter(function(c){return c[0] === 'arc';}).length >= 3);
    check('cave sky suppresses celestial bodies clouds and mountains',
      caveCommands.every(function(c){return c[0] !== 'arc' && c[0] !== 'ellipse' &&
        c[0] !== 'rect' && c[0] !== 'lineTo';}));
    check('sky draw count stays bounded',
      dayCommands.filter(function(c){return c[0] === 'ellipse';}).length <= 30 &&
      nightCommands.filter(function(c){return c[0] === 'rect';}).length <= 48 &&
      dayCommands.length < 650 && nightCommands.length < 700);
  } finally {
    Date.now = realNow;
  }
  check('sky avoids filters readback and per-pixel image work',
    !/shadowBlur|\.filter|getImageData|putImageData|createPattern/.test(skySource));
  var lightSource = slurp('assets/games/src/17-input-lighting-update.js');
  check('visible sky and directional light agree that 0.5 is noon',
    lightSource.indexOf('var sunAngle = (dayTime - 0.25) * Math.PI * 2;') >= 0);
  __out('SKY_RENDER_RESULT PASS ' + checks);
}());
undefined;
