// First-person presentation and body-cache contracts. Native browser tests are
// still required for anatomy, animation quality and actual allocation costs.
(function () {
  var G = (0,eval)('this'), checks = 0, clock = 1000, images = [], drawCalls = 0;
  function check(name, value) { if (!value) throw Error(name); checks++; __out('PASS '+name); }
  function noop() {}
  function brush() {
    var state = {globalAlpha:1,shadowBlur:0,lineWidth:1,globalCompositeOperation:'source-over',imageSmoothingEnabled:true};
    var stack = [];
    state._depth = function () { return stack.length; };
    state.save = function () {
      var copy = {}; Object.keys(state).forEach(function (key) { if (typeof state[key] !== 'function') copy[key] = state[key]; });
      stack.push(copy);
    };
    state.restore = function () {
      var copy = stack.pop(); if (!copy) throw Error('unbalanced Canvas restore');
      Object.keys(state).forEach(function (key) { if (typeof state[key] !== 'function') delete state[key]; });
      Object.keys(copy).forEach(function (key) { state[key] = copy[key]; });
    };
    ['beginPath','closePath','moveTo','lineTo','bezierCurveTo','quadraticCurveTo','translate','rotate','scale','clip','ellipse','arc','clearRect'].forEach(function (name) { state[name] = noop; });
    state.fill = state.stroke = state.drawImage = function () { drawCalls++; };
    state.createLinearGradient = state.createRadialGradient = function () { return {addColorStop:noop}; };
    return state;
  }
  G.document = {createElement: function (tag) {
    if (tag !== 'canvas') throw Error('art cache must create only canvases');
    var context = brush(), image = {width:0,height:0,getContext:function () { return context; },_context:context};
    images.push(image); return image;
  }};
  G.performance = {now:function () { clock += 0.1; return clock; }};
  G.equipment = {robes:null,relic:null};
  G.getEffectiveCooldown = function () { return 300; };
  G.MODE3D = true; G.shopOpen = false; G.resScale = 1; G.canvas = {width:360,height:240};
  G.vel = {x:0,y:0}; G.ambientLight = 0.9; G.ctx = brush();
  G.pos = {x:5,y:12,floorZ:60}; G.mana = 90; G.lastShotMs = 700;
  G.projectiles = [{x:20,y:10,z:55,lifeMs:1200}]; G.impacts = [];
  (0,eval)(slurp('assets/games/src/01-materials.js'));
  (0,eval)(slurp('assets/games/src/15-firstperson-art.js'));
  var oldRandom = Math.random;
  Math.random = function () { throw Error('first-person art must not consume gameplay randomness'); };
  try {
    check('ready pose starts gathered without fabricating a gameplay cast',
      G.getCastingArtPose(1000).energy === 0.55 && G.getCastingArtPose(1000).thrust === 0 && G._castingPoseState.castAt < 0);
    var before = JSON.stringify({equipment:G.equipment,mana:G.mana,lastShotMs:G.lastShotMs,projectiles:G.projectiles,impacts:G.impacts,pos:G.pos});
    G.noteFirstPersonCast({id:'missile'},1000);
    check('presentation event records the actual release time and effective cooldown', G._castingPoseState.castAt === 1000 && G._castingPoseState.cooldown === 300);
    var anticipation = G.getCastingArtPose(1060);
    check('approved wind-up gathers before the 120 ms release boundary',
      anticipation.phase === 'anticipation / gathering' && anticipation.thrust < 0 && anticipation.curl > 0.84 && anticipation.energy > 0.55);
    var early = G.getCastingArtPose(1150), follow = G.getCastingArtPose(1220), recovery = G.getCastingArtPose(1350), ready = G.getCastingArtPose(1390);
    check('release opens fingers and spends gathered focus while thrusting', early.thrust > 0 && early.curl < 0.84 && early.energy < 1);
    check('follow-through retains open hand after focus has left', follow.thrust === 1 && follow.curl === 0 && follow.energy === 0);
    check('recovery closes fingers and regathers before ready', recovery.thrust > 0 && recovery.thrust < 1 && recovery.curl > 0 && recovery.energy > 0);
    check('gesture returns to stable gathered pose', ready.thrust === 0 && ready.energy === 0.55 && ready.curl === 0.84);
    G.noteFirstPersonCast({id:'ice'},2000); G.noteFirstPersonCast(null,2000); G.noteFirstPersonCast({id:'missile'},NaN);
    check('unrelated or invalid events do not overwrite missile gesture', G._castingPoseState.castAt === 1000);
    G.noteFirstPersonCast({id:'missile'},1120);
    check('repeated successful cast starts a new gesture immediately', G._castingPoseState.castAt === 1120 && G.getCastingArtPose(1120).elapsed === 0);
    check('presentation events do not mutate gameplay or equipment',
      JSON.stringify({equipment:G.equipment,mana:G.mana,lastShotMs:G.lastShotMs,projectiles:G.projectiles,impacts:G.impacts,pos:G.pos}) === before);
    G.clearCastingArtworkCache();
    check('body cache begins empty and reports an explicit memory ceiling', G.getCastingArtworkStats().entries === 0 && G.getCastingArtworkStats().bytes === 0 && G.getCastingArtworkStats().maxBytes === 8*1024*1024);
    var first = G.getCastingArmArtwork(1), firstStats = G.getCastingArtworkStats();
    check('first use builds two lighting endpoints once at native-or-higher source scale',
      images.length === 2 && firstStats.builds === 2 && firstStats.entries === 2 && first.raster >= 0.68 && first.day.width === 176*first.raster);
    check('reported body pixels match actual cached canvas dimensions',
      firstStats.bytes === images.reduce(function (sum,image) { return sum+image.width*image.height*4; },0) && firstStats.bytes <= firstStats.maxBytes);
    check('body cache measures first-use work and returns a defensive stats copy', firstStats.lastBuildMs > 0 && firstStats.buildMs >= firstStats.lastBuildMs && G.getCastingArtworkStats() !== firstStats);
    firstStats.bytes = -1;
    check('editing a stats snapshot cannot corrupt cache accounting', G.getCastingArtworkStats().bytes > 0);
    var warm = G.getCastingArmArtwork(1);
    check('steady-state body lookup reuses existing artwork without another build', warm === first && images.length === 2 && G.getCastingArtworkStats().builds === 2);
    G.equipment.robes = {id:'same-robe',armColor:{deep:'#122233',mid:'#344455',lit:'#667788',cuff:'#554433'}};
    var robe = G.getCastingArmArtwork(1), builds = G.getCastingArtworkStats().builds;
    G.equipment.robes.armColor.mid = '#445566'; var recolored = G.getCastingArmArtwork(1);
    check('same equipment id with changed visible colors invalidates body artwork', robe !== recolored && G.getCastingArtworkStats().builds === builds+2 && recolored.palette.cloth.mid === '#445566');
    var fullscreen = G.getCastingArmArtwork(1280/360);
    check('fullscreen builds at or above its destination pixel scale', fullscreen.raster >= 0.68*1280/360 && G.getCastingArtworkStats().bytes <= G.getCastingArtworkStats().maxBytes);
    var builtBeforeFallback = G.getCastingArtworkStats().builds, countBeforeFallback = images.length;
    var huge = G.getCastingArmArtwork(20);
    check('oversized resolutions retain direct-vector fallback instead of downscaling',
      !huge.day && !huge.shadow && G.getCastingArtworkStats().fallbacks === 1 &&
      G.getCastingArtworkStats().bytes === 0 && G.getCastingArtworkStats().entries === 0 &&
      G.getCastingArtworkStats().builds === builtBeforeFallback && images.length === countBeforeFallback);
    G.getCastingArmArtwork(20);
    check('oversized fallback does not retry allocation on every frame', G.getCastingArtworkStats().fallbacks === 1 && images.length === countBeforeFallback);
    G.clearCastingArtworkCache(); var createCanvas = G.document.createElement, failedAllocations = 0;
    G.document.createElement = function () { failedAllocations++; return {width:0,height:0,getContext:function () { return null; }}; };
    var unavailable = G.getCastingArmArtwork(1);
    check('unavailable Canvas context falls back without retaining partial artwork',
      !unavailable.shadow && !unavailable.day && G.getCastingArtworkStats().entries === 0 && G.getCastingArtworkStats().bytes === 0 && G.getCastingArtworkStats().fallbacks === 1);
    G.getCastingArmArtwork(1);
    check('failed Canvas allocation does not retry every steady-state frame', failedAllocations === 1);
    G.document.createElement = createCanvas; G.clearCastingArtworkCache();
    check('explicit cache clear permits allocation recovery', !!G.getCastingArmArtwork(1).day && G.getCastingArtworkStats().fallbacks === 0);
    G.equipment.robes = null; G.clearCastingArtworkCache(); G.ctx = brush();
    G.ctx.globalAlpha = 0.43; G.ctx.shadowBlur = 8; G.ctx.globalCompositeOperation = 'multiply'; G.ctx.imageSmoothingEnabled = false;
    G.withSceneDepthClip = function () { throw Error('foreground hands must not read world depth'); };
    G.drawFirstPersonCastingArt(1600);
    check('hands paint foreground independently of world occlusion without gameplay RNG', drawCalls > 0 && G.ctx._depth() === 0);
    check('hands restore the main Canvas alpha compositing blur and smoothing',
      G.ctx.globalAlpha === 0.43 && G.ctx.shadowBlur === 8 && G.ctx.globalCompositeOperation === 'multiply' && G.ctx.imageSmoothingEnabled === false);
    check('all generated body canvases have balanced save and restore', images.every(function (image) { return image._context._depth() === 0; }));
    var currentBuilds = G.getCastingArtworkStats().builds;
    G.ambientLight = 0.3; G.drawFirstPersonCastingArt(1650);
    check('daylight to dusk mixes existing body endpoints without cache regeneration', G.getCastingArtworkStats().builds === currentBuilds);
    G.vel.x = 120; var oldPhase = G._castingPoseState.walkPhase;
    G.drawFirstPersonCastingArt(1680);
    check('movement advances only presentation walk phase', G._castingPoseState.walkPhase > oldPhase && G.vel.x === 120);
    G.MODE3D = false; var beforeDisabled = drawCalls; G.drawFirstPersonCastingArt(1800);
    G.MODE3D = true; G.shopOpen = true; G.drawFirstPersonCastingArt(1800);
    check('two dimensional and shop views retain their original foreground path', drawCalls === beforeDisabled);
  } finally { Math.random = oldRandom; }
  __out('FIRSTPERSON_ART_RESULT PASS '+checks);
}());
undefined;
