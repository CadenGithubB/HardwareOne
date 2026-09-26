// Opt-in, actual-game casting comparison. It borrows the current Cave Test
// scene, never rebuilds terrain, and installs temporary simulation state only
// during synchronous ticks/draws. Closing the studio leaves gameplay untouched.
var _castingStudio = null;
var _castingStudioArt = true;
var _castingStudioLight = 'daylight';
var _castingStudioMode = 'hand';
var _castingStudioYaw = 0;
var _castingStudioGrip = typeof HAND_RIG_DEFAULT_GRIP==='string'?HAND_RIG_DEFAULT_GRIP:'handle';
var _castingStudioVideoURL = null;
var _castingStudioSettingStyle = false;
var _castingStudioStyleUnsubscribe = null;
var CASTING_STUDIO_CYCLE_MS = 2400;
var HAND_STUDIO_CYCLE_MS = 2800;
var CASTING_STUDIO_REQUEST_MS = 600;
var CASTING_STUDIO_STEP_MS = 1000 / 120;
var _castingStudioKeys = (
  'pos cam vel settings equipment spells stats health mana currentSpellIdx lastShotMs castAnimUntil ' +
  '_castingPoseState walkBobPhase running gameOverState menuOpen shopOpen settingsOpen inventoryOpen forgeOpen ' +
  'overviewActive calibrating MODE3D CAM_FOLLOW CONTROL_MODE USE_MOUSE USE_GAMEPAD gpLast ' +
  '_mouseHeld _attackHeld flameStreamActive dmgBoostUntil manaBlinkUntil ' +
  'enemies projectiles pendingMissileCasts spellCastReservationSerial impacts coneEffects groundEffects chainEffects novaEffects deathEffects soulOrbs coinDrops ' +
  'enemySpawners oreVeins companions fortressAllies arcaneTomes statPickups ambientParticles ' +
  'dayTime daySpeed ambientLight sunIntensity sunDirX sunDirZ fogFloor playerUnderground ' +
  '_wallShadeN _wallShadeS _wallShadeE _wallShadeW _topShade ' +
  'renderSurfaceAmbient renderSurfaceSunIntensity renderSurfaceFogFloor renderSurfaceWallShadeN ' +
  'renderSurfaceWallShadeS renderSurfaceWallShadeE renderSurfaceWallShadeW renderSurfaceTopShade ' +
  'renderCaveAmbient renderCaveFogFloor renderCameraCaveBlend ' +
  '_lightGrid _lightGridLastScale _lightGridLastCamGX _lightGridLastCamGY _lightGridFrameCount ' +
  '_surfaceFloorLightBake _surfaceFloorLightMesh _surfaceFloorLightScale _surfaceFloorLightStats ' +
  'exploredCells minimapCanvas minimapDirty _lastExploredUpdate ' +
  'CASTING_ART_ENABLED HAND_RIG_PREVIEW HAND_RIG_TIME_MS HAND_RIG_VIEW_YAW HAND_RIG_GRIP HAND_RIG_STYLE DEBUG_PERF_HUD DEBUG_CAVE DEBUG_COMBAT DEBUG_EFFECTS ' +
  '_armRightIdle _armRightCast _armLeftIdle _armLeftCast _armLastRobeId'
).split(' ');

function castingStudioWrite(value) {
  var el = document.getElementById('castingStudioResult');
  if (el) el.textContent = typeof value === 'string' ? value : JSON.stringify(value, null, 2);
}

function castingStudioSelectedStyle() {
  return typeof getSelectedCastingStyle==='function'?getSelectedCastingStyle():'arcane';
}

function castingStudioCopy(keys) {
  var out = {};
  keys.forEach(function (key) { out[key] = window[key]; });
  return out;
}

function castingStudioInstall(values) {
  Object.keys(values).forEach(function (key) { window[key] = values[key]; });
}

function castingStudioScoped(job, fn) {
  var saved = castingStudioCopy(_castingStudioKeys), oldDate = Date.now, oldRandom = Math.random;
  var oldFog = drawMinimap._fogCanvas, hadFog = Object.prototype.hasOwnProperty.call(drawMinimap, '_fogCanvas');
  var random = (1234567 ^ Math.round(job.time * 120 / 1000)) | 0;
  castingStudioInstall(job.live);
  drawMinimap._fogCanvas = job.fog;
  Date.now = function () { return job.clockBase + job.time; };
  Math.random = function () { random = (Math.imul(random, 1664525) + 1013904223) | 0; return (random >>> 0) / 4294967296; };
  try { return fn(); }
  finally {
    job.live = castingStudioCopy(_castingStudioKeys);
    job.fog = drawMinimap._fogCanvas;
    castingStudioInstall(saved);
    if (hadFog) drawMinimap._fogCanvas = oldFog; else delete drawMinimap._fogCanvas;
    Date.now = oldDate; Math.random = oldRandom;
  }
}

// A stationary, labelled practice skeleton uses the real enemy hit cylinder.
// Check the existing grid/support along the firing lane; do not remove walls
// or manufacture an impact if the current viewpoint cannot supply a target.
function castingStudioFindTarget(pose, camera) {
  var angle = camera.ang, floorZ = (pose.floorZ - 60) * 0.625, selected = null;
  for (var distance = 16; distance <= 220; distance += 8) {
    var x = pose.x + Math.cos(angle) * distance, y = pose.y + Math.sin(angle) * distance;
    var gx = Math.floor(x / cell), gy = Math.floor(y / cell);
    if (gx < 0 || gy < 0 || gx >= gridW || gy >= gridH || grid[gy * gridW + gx]) break;
    var support = sampleEntitySupportRenderZ(x, y, floorZ, playerUnderground, true);
    if (!Number.isFinite(support) || Math.abs(support - floorZ) > 30) break;
    if (distance >= 96) selected = {x:x, y:y, z:support, renderFloorZ:support, distance:distance};
  }
  if (!selected) throw Error('This viewpoint has no clear practice lane. Choose Mouth (or Inside), then reopen Casting studio.');
  return selected;
}

function castingStudioResetCycle(job) {
  job.time = 0; job.simTime = 0; job.requested = false; job.requestAt = null; job.released = false; job.hitAt = null; job.releaseAt = null;
  job.live.projectiles = []; job.live.pendingMissileCasts = []; job.live.impacts = []; job.live.groundEffects = [];
  job.live.spellCastReservationSerial = 0;
  job.live.coneEffects = []; job.live.chainEffects = []; job.live.novaEffects = [];
  job.live.deathEffects = []; job.live.soulOrbs = []; job.live.coinDrops = [];
  job.live.stats = {totalDamageDone:0, totalDamageTaken:0, totalManaConsumed:0, totalEnemiesKilled:0};
  job.live.mana = MANA_MAX; job.live.lastShotMs = -1e12; job.live.castAnimUntil = 0;
  job.live._castingPoseState = {castAt:-1e12, spellId:'missile', cooldown:450, lastNow:0, walkPhase:0};
  job.live.walkBobPhase = 0;
  job.live.enemies = job.mode==='hand'?[]:[Object.assign({}, job.target, {health:1000, maxHealth:1000,
    enemyType:Object.assign({}, enemyTypes.normal, {speed:0, chaseRange:0}),
    underground:job.underground, speed:0, chaseRange:0, facing:job.live.cam.ang + Math.PI,
    patrolWaypoints:[], attackState:'idle', vx:0, vy:0})];
  job.live.CASTING_ART_ENABLED = job.mode==='casting'&&job.art;
  job.live.HAND_RIG_PREVIEW = job.mode==='hand'&&job.art;
  job.live.HAND_RIG_TIME_MS = 0; job.live.HAND_RIG_VIEW_YAW = job.yaw;
  job.live.HAND_RIG_GRIP = job.grip;
  job.live.HAND_RIG_STYLE = job.style;
  job.live.dayTime = job.light === 'dusk' ? 0.78 : 0.5;
  job.live._lightGridLastScale = -Infinity; job.live._lightGridFrameCount = 4;
  castingStudioSyncTimeline(job);
}

function startCastingStudio() {
  if (_castingStudio) {
    if(_castingStudio.stopped)throw Error('Studio is closing; wait for fullscreen to finish.');
    return _castingStudio;
  }
  if (!CAVE_TEST_MODE || !floorMesh || !grid) throw Error('Select Cave Test first.');
  if (_caveBrowserProfile) throw Error('Finish or cancel the other browser profile first.');
  if (caveProfileFullscreenElement()) throw Error('Exit fullscreen before opening Casting studio.');
  var pose = Object.assign({}, pos), camera = Object.assign({}, cam);
  // A level, stationary shot keeps the same actual launch height and speed.
  camera.x = pose.x; camera.y = pose.y; camera.z = pose.floorZ; camera.pitch = 0;
  var target = _castingStudioMode==='hand'?null:castingStudioFindTarget(pose, camera);
  var live = castingStudioCopy(_castingStudioKeys);
  live.pos = pose; live.cam = camera; live.vel = {x:0,y:0};
  live.settings = Object.assign({}, settings, {dayNight:true,showFPS:false});
  live.equipment = Object.assign({}, equipment, {relic:null});
  live.spells = Object.assign({}, spells, {missile:Object.assign({}, spells.missile, {tier:1,unlocked:true})});
  live.health = HEALTH_MAX; live.currentSpellIdx = 0; live.dmgBoostUntil = 0;
  live.MODE3D = true; live.CAM_FOLLOW = true; live.CONTROL_MODE = MODE_STICK_AIM;
  live.USE_MOUSE = true; live.USE_GAMEPAD = false; live.gpLast = null; live.running = true;
  ['gameOverState','menuOpen','shopOpen','settingsOpen','inventoryOpen','forgeOpen','overviewActive','calibrating',
    '_mouseHeld','_attackHeld','flameStreamActive','DEBUG_PERF_HUD','DEBUG_CAVE','DEBUG_COMBAT','DEBUG_EFFECTS'].forEach(function (key) { live[key] = false; });
  ['enemySpawners','oreVeins','companions','fortressAllies','arcaneTomes','statPickups'].forEach(function (key) { live[key] = []; });
  live.ambientParticles = ambientParticles.map(function (p) { return Object.assign({}, p); });
  live._lightGrid = _lightGrid ? new Float32Array(_lightGrid) : null;
  // Upper-terrain visibility is baked once and immutable. Share that buffer;
  // its daylight/flicker scalar is isolated by castingStudioScoped above.
  live.daySpeed = 0;
  var job = {live:live, mesh:floorMesh, seed:WORLD_SEED, view:cavePreviewLastView,
    target:target, underground:!!playerUnderground, art:_castingStudioArt, light:_castingStudioLight,
    mode:_castingStudioMode,yaw:_castingStudioYaw,grip:_castingStudioGrip,style:castingStudioSelectedStyle(),inspectionFullscreen:false,fullscreenPending:false,
    wasRunning:running, width:canvas.width, height:canvas.height,
    styleWidth:canvas.style.width, styleHeight:canvas.style.height,
    raf:0, playing:false, profile:false, stopped:false, resume:null, clockBase:2000000000000,
    fog:document.createElement('canvas')};
  job.fog.width = MINIMAP_W; job.fog.height = MINIMAP_H;
  castingStudioResetCycle(job);
  // Prepare independent minimap storage once, outside timing and the live map.
  castingStudioScoped(job, function () { initMinimap(); updateDayNight(0); updateLightGrid(); });
  _castingStudio = job;
  running = false; _loopGen++;
  if (document.pointerLockElement === canvas && document.exitPointerLock) document.exitPointerLock();
  return job;
}

function castingStudioAdvance(job, toMs) {
  if(job.mode==='hand'){
    // Anatomy proof only: no input acceptance, mana, queued cast, projectile,
    // effect lifetime or hit simulation runs along this separate timeline.
    job.time=toMs;job.simTime=toMs;job.live.HAND_RIG_TIME_MS=toMs;
    return;
  }
  while (job.simTime + CASTING_STUDIO_STEP_MS <= toMs + 0.00001) {
    job.simTime += CASTING_STUDIO_STEP_MS; job.time = job.simTime;
    castingStudioScoped(job, function () {
      if (!job.requested && job.time >= CASTING_STUDIO_REQUEST_MS - 0.00001) {
        castCurrentSpell(); job.requested = true; job.requestAt = job.time;
        if (!pendingMissileCasts.length && !projectiles.length) throw Error('Practice cast was not accepted.');
      }
      updateProjectiles(CASTING_STUDIO_STEP_MS / 1000);
      // The production queue, serviced by updateProjectiles, owns the real
      // 120 ms wind-up. Observe its actual spawn, never create a demo missile.
      if (!job.released && projectiles.length) { job.released=true;job.releaseAt=job.time; }
      tickEffects(CASTING_STUDIO_STEP_MS / 1000);
      if (job.hitAt === null && enemies[0].health < 1000) job.hitAt = job.time;
    });
  }
  job.time = toMs;
}

function castingStudioDraw(job) {
  var stages = {}, buildMs = 0, oldPt = _pt, oldBuilder = buildPixelArmSprites;
  var errorsBefore=Object.assign({},_ptErrors);
  _pt = function (name, fn) {
    var start = performance.now();
    try { return oldPt(name, fn); }
    finally { stages[name] = (stages[name] || 0) + performance.now() - start; }
  };
  buildPixelArmSprites = function () {
    var cold = !_armRightIdle || _armLastRobeId !== (equipment.robes ? equipment.robes.id : '_default');
    var start = performance.now();
    try { return oldBuilder(); } finally { if (cold) buildMs += performance.now() - start; }
  };
  var start = performance.now();
  try {
    castingStudioScoped(job, function () { updateDayNight(0); renderFrame(); });
  } finally { _pt = oldPt; buildPixelArmSprites = oldBuilder; }
  Object.keys(_ptErrors).forEach(function(name){
    if(_ptErrors[name]>(errorsBefore[name]||0))throw Error('Rendering stage failed: '+name+'. Check the browser console.');
  });
  var elapsed=performance.now()-start;
  castingStudioSyncTimeline(job);
  return {renderMs:elapsed, stages:stages, legacyBuildMs:buildMs};
}

function castingStudioDescribe(job) {
  if(job.mode==='hand')return castingStudioArtLabel(job)+' · neutral hand motion study · '+job.light+
    ' · '+handStudioPhase(job.time)+' · '+Math.round(job.time)+' / '+HAND_STUDIO_CYCLE_MS+' ms'+
    ' · '+job.style+' style · '+job.grip+' resting template · ¼-speed casting flourish · no equipped item, spell or damage';
  var phase = !job.requested ? 'Ready' : !job.released ? 'Gather / wind-up' : job.hitAt !== null ?
    (job.time-job.hitAt < 250 ? 'Actual target hit' : 'Follow-through / recovery') :
    job.live.projectiles.length ? 'Missile in flight' : 'Missile stopped by existing geometry';
  return castingStudioArtLabel(job)+' · '+job.light+' · '+phase+
    ' · stationary practice skeleton '+Math.round(job.target.distance)+' units away'+
    ' · real damage '+job.live.stats.totalDamageDone.toFixed(1);
}

function castingStudioArtLabel(job) {
  return !job.art?'Original baseline':job.mode==='hand'?'Articulated hand':'Rejected flat study';
}

function castingStudioCycleMs(job) {return job.mode==='hand'?HAND_STUDIO_CYCLE_MS:CASTING_STUDIO_CYCLE_MS;}

function handStudioPhase(time) {
  return time<500?'Ready':time<1050?'Gather':time<1420?'Release':time<1730?'Follow-through':time<2450?'Recovery':'Ready';
}

function castingStudioSyncTimeline(job) {
  var scrub=document.getElementById('castingStudioTime');
  var value=String(Math.round(Math.max(0,Math.min(castingStudioCycleMs(job),job.time))));
  if(scrub&&scrub.value!==value)scrub.value=value;
  if(job.mode==='hand'){
    var phase=document.getElementById('castingStudioPhase');
    var phaseValue={Ready:'0',Gather:'780',Release:'1220','Follow-through':'1560',Recovery:'2110'}[handStudioPhase(job.time)];
    if(phase&&phase.value!==phaseValue)phase.value=phaseValue;
  }
}

function pauseCastingStudio() {
  var job = _castingStudio; if (!job || job.profile) return;
  if(job.recording)finishCastingStudioRecording(job,true);
  job.playing = false; if (job.raf) cancelAnimationFrame(job.raf); job.raf = 0;
  castingStudioWrite(castingStudioDescribe(job)+' · paused');
}

function playCastingStudio() {
  var job;
  try { job = startCastingStudio(); } catch (error) { castingStudioWrite(error.message); return; }
  if (job.profile || job.playing) return;
  job.playing = true; job.previousTs = null;
  function frame(ts) {
    if (job.stopped || !job.playing) return;
    try {
      if (!CAVE_TEST_MODE || floorMesh !== job.mesh || WORLD_SEED !== job.seed) throw Error('World changed.');
      var dt = (job.previousTs === null ? 0 : Math.min(50, Math.max(0, ts-job.previousTs)))*(job.mode==='hand'?0.25:1);
      job.previousTs = ts;
      var cycleMs=castingStudioCycleMs(job);
      if (job.time + dt >= cycleMs) {
        if(job.recording){
          castingStudioAdvance(job,cycleMs);castingStudioDraw(job);
          if(job.mode!=='hand'&&job.hitAt===null)throw Error('Practice target was not hit; incomplete recording discarded.');
          job.playing=false;job.raf=0;finishCastingStudioRecording(job,false);return;
        }
        castingStudioResetCycle(job);
      }
      castingStudioAdvance(job, job.time+dt); castingStudioDraw(job);
      castingStudioWrite(castingStudioDescribe(job));
      job.raf = requestAnimationFrame(frame);
    } catch (error) { stopCastingStudio(error.message); }
  }
  job.raf = requestAnimationFrame(frame);
}

function setCastingStudioArt(enabled) {
  if(_castingStudio && (_castingStudio.profile||_castingStudio.fullscreenPending))return;
  if(_castingStudio && _castingStudio.recording)finishCastingStudioRecording(_castingStudio,true);
  _castingStudioArt = !!enabled;
  castingStudioUpdateChoices();
  var job = _castingStudio;
  if(!job&&_castingStudioMode==='hand'){
    try{job=startCastingStudio();}catch(error){castingStudioWrite(error.message);return;}
  }
  if (!job) {castingStudioWrite(castingStudioArtLabel({art:_castingStudioArt,mode:_castingStudioMode})+' · '+_castingStudioLight+' selected. Press Play.');return;}
  if(job.mode==='hand')pauseCastingStudio();
  job.art = _castingStudioArt; castingStudioResetCycle(job); castingStudioDraw(job);
  castingStudioWrite(castingStudioDescribe(job));
}

function setCastingStudioLight(light) {
  if(_castingStudio && _castingStudio.profile)return;
  if(_castingStudio && _castingStudio.recording)finishCastingStudioRecording(_castingStudio,true);
  _castingStudioLight = light === 'dusk' ? 'dusk' : 'daylight';
  castingStudioUpdateChoices();
  var job = _castingStudio;
  if (!job) {castingStudioWrite(castingStudioArtLabel({art:_castingStudioArt,mode:_castingStudioMode})+' · '+_castingStudioLight+' selected. Press Play.');return;}
  job.light = _castingStudioLight; castingStudioResetCycle(job); castingStudioDraw(job);
  castingStudioWrite(castingStudioDescribe(job));
}

function castingStudioUpdateChoices() {
  [['castingStudioOriginal',!_castingStudioArt],['castingStudioRedesigned',_castingStudioArt],
    ['castingStudioDaylight',_castingStudioLight==='daylight'],['castingStudioDusk',_castingStudioLight==='dusk']].forEach(function(pair){
    var button=document.getElementById(pair[0]);if(button)button.setAttribute('aria-pressed',String(pair[1]));
  });
  var redesigned=document.getElementById('castingStudioRedesigned');
  if(redesigned)redesigned.textContent=_castingStudioMode==='hand'?'Articulated hand':'Rejected flat study';
  var play=document.getElementById('castingStudioPlay');if(play)play.textContent=_castingStudioMode==='hand'?'Play casting flourish (¼ speed)':'Play casting sequence';
  var scrub=document.getElementById('castingStudioTime');if(scrub)scrub.max=String(_castingStudioMode==='hand'?HAND_STUDIO_CYCLE_MS:CASTING_STUDIO_CYCLE_MS);
  var view=document.getElementById('castingStudioHandView');if(view)view.disabled=_castingStudioMode!=='hand';
  var phase=document.getElementById('castingStudioPhase');if(phase)phase.disabled=_castingStudioMode!=='hand';
  var grip=document.getElementById('castingStudioGrip');
  if(grip){grip.disabled=_castingStudioMode!=='hand';grip.value=_castingStudioGrip;}
  var style=document.getElementById('castingStudioStyle');
  if(style){style.disabled=_castingStudioMode!=='hand';style.value=castingStudioSelectedStyle();}
}

function setCastingStudioMode(mode) {
  if(_castingStudio&&(_castingStudio.profile||_castingStudio.fullscreenPending||_castingStudio.inspectionFullscreen))return;
  if(_castingStudio)stopCastingStudio('Study mode changed; previous session restored.');
  _castingStudioMode=mode==='casting'?'casting':'hand';
  _castingStudioArt=true;castingStudioUpdateChoices();
  castingStudioWrite(_castingStudioMode==='hand'?'Hand motion study: neutral articulated anatomy only; no actual casting.':'Rejected flat casting study (archived comparison), with real missile simulation.');
}

function scrubCastingStudio(timeMs) {
  var job;
  try{
    job=startCastingStudio();if(job.profile||job.fullscreenPending)return;
    pauseCastingStudio();castingStudioResetCycle(job);
    castingStudioAdvance(job,Math.max(0,Math.min(castingStudioCycleMs(job),Number(timeMs)||0)));
    castingStudioDraw(job);castingStudioWrite(castingStudioDescribe(job)+' · paused');
  }catch(error){if(job)stopCastingStudio(error.message);else castingStudioWrite(error.message);}
}

function setCastingStudioHandView(yaw) {
  if(_castingStudio&&(_castingStudio.profile||_castingStudio.fullscreenPending))return;
  _castingStudioYaw=yaw==='palm'?'palm':Number(yaw)||0;
  var job=_castingStudio;
  if(job){
    if(job.recording)finishCastingStudioRecording(job,true);
    job.yaw=_castingStudioYaw;job.live.HAND_RIG_VIEW_YAW=job.yaw;castingStudioDraw(job);
    castingStudioWrite(castingStudioDescribe(job));
  }
}

function setCastingStudioHandGrip(grip) {
  if(_castingStudio&&(_castingStudio.profile||_castingStudio.fullscreenPending))return;
  _castingStudioGrip=grip==='reach'||grip==='handle'||grip==='cradle'?grip:
    typeof HAND_RIG_DEFAULT_GRIP==='string'?HAND_RIG_DEFAULT_GRIP:'handle';
  castingStudioUpdateChoices();
  if(_castingStudioMode!=='hand')return;
  var job;
  try{
    job=startCastingStudio();pauseCastingStudio();job.grip=_castingStudioGrip;
    castingStudioResetCycle(job);castingStudioDraw(job);
    castingStudioWrite(castingStudioDescribe(job)+' · paused at rest');
  }catch(error){if(job)stopCastingStudio(error.message);else castingStudioWrite(error.message);}
}

function setCastingStudioStyle(styleId) {
  if(_castingStudio&&(_castingStudio.profile||_castingStudio.fullscreenPending)){
    castingStudioUpdateChoices();return;
  }
  if(typeof setSelectedCastingStyle!=='function'){
    castingStudioWrite('Casting Style preferences are unavailable; rebuild and reload the preview.');return;
  }
  // This is the user's deliberate preference change, not temporary preview
  // state: call the registry outside the sandbox, retaining its browser save.
  var selected;
  _castingStudioSettingStyle=true;
  try{selected=setSelectedCastingStyle(styleId);}finally{_castingStudioSettingStyle=false;}
  castingStudioApplyStyle(selected,true);
}

function castingStudioApplyStyle(selected,openPreview) {
  castingStudioUpdateChoices();
  if(_castingStudioMode!=='hand')return;
  if(_castingStudio&&(_castingStudio.profile||_castingStudio.fullscreenPending)){
    stopCastingStudio('Casting Style changed; studio stopped.');return;
  }
  if(!_castingStudio&&!openPreview)return;
  var job;
  try{
    job=startCastingStudio();pauseCastingStudio();job.style=selected;
    job.live.settings.castingStyle=selected;
    castingStudioResetCycle(job);castingStudioDraw(job);
    castingStudioWrite(castingStudioDescribe(job)+' · preference selected; paused at rest');
  }catch(error){if(job)stopCastingStudio(error.message);else castingStudioWrite(error.message);}
}

function stopCastingStudio(reason) {
  var job = _castingStudio; if (!job) return;
  if(job.recording)finishCastingStudioRecording(job,true);
  job.stopped = true; job.playing = false; job.cancelReason = reason || 'Studio closed; gameplay restored.';
  if (job.raf) cancelAnimationFrame(job.raf); job.raf = 0;
  if (job.resume) { var resume=job.resume; job.resume=null; resume(null); }
  if (job.profile) return; // async profile finally owns fullscreen/restoration
  if(job.fullscreenPending)return; // fullscreen request owns its final cleanup
  if(job.inspectionFullscreen&&caveProfileFullscreenElement()===canvas){
    closeCastingStudioFullscreen(job);return;
  }
  castingStudioRestore(job);
  castingStudioWrite(job.cancelReason);
}

function castingStudioYieldToControls() {
  var job=_castingStudio;if(!job)return;
  if(job.profile||job.inspectionFullscreen||job.fullscreenPending){
    // Give ordinary controls back their pre-studio simulation synchronously;
    // their own handlers then remain authoritative (not a later async finally).
    job.externalAction=true;running=job.wasRunning;lastUpdate=0;_loopGen++;
    if(running)_scheduleLoop();
  }
  stopCastingStudio('Game control changed; studio stopped.');
}

// Only real form controls can hand ownership back to the game. A canvas focus
// click, screenshot-tool focus, document background or label text is not a
// request to end the study. Resolve labels to their associated actual control.
function castingStudioControlTarget(target) {
  var node=target,steps=0;
  while(node&&steps++<32){
    if(node===canvas)return null;
    var tag=typeof node.tagName==='string'?node.tagName.toUpperCase():'';
    if(tag==='BUTTON'||tag==='INPUT'||tag==='SELECT'||tag==='TEXTAREA')return node;
    if(tag==='LABEL'){
      if(node.control)return node.control;
      if(node.htmlFor){var linked=document.getElementById(node.htmlFor);if(linked)return linked;}
    }
    node=node.parentElement||node.parentNode;
  }
  return null;
}

function castingStudioControlEvent(event) {
  if(!_castingStudio||!event.target)return;
  var panel=document.getElementById('castingStudioPanel');
  if(panel&&panel.contains(event.target))return;
  var control=castingStudioControlTarget(event.target);
  if(!control||(panel&&panel.contains(control)))return;
  if(_castingStudio.profile&&(control.id||'').indexOf('ctProfile')===0){
    event.preventDefault();event.stopImmediatePropagation();
    stopCastingStudio('Casting profile cancelled. Start the other profile after it closes.');return;
  }
  castingStudioYieldToControls();
}

function castingStudioCanvasInput(event) {
  var job=_castingStudio;if(!job)return false;
  var type=event.type||'',onCanvas=event.target===canvas;
  var down=type==='pointerdown'||type==='mousedown'||type==='touchstart';
  var up=type==='pointerup'||type==='pointercancel'||type==='mouseup'||type==='touchend'||type==='touchcancel';
  var move=type==='pointermove'||type==='mousemove'||type==='touchmove';
  if(!onCanvas&&!((up||move)&&job.canvasPointerActive))return false;
  if(down)job.canvasPointerActive=true;
  if(up)job.canvasPointerActive=false;
  // Run in capture phase, before the game's pointer-lock, cast, wheel or
  // fullscreen handlers. Pointer releases outside the canvas are consumed too.
  if(event.cancelable!==false)event.preventDefault();
  event.stopImmediatePropagation();
  return true;
}

async function closeCastingStudioFullscreen(job) {
  if(job.closingFullscreen)return;
  job.closingFullscreen=true;
  try{
    if(caveProfileFullscreenElement()===canvas){
      var exit=document.exitFullscreen||document.webkitExitFullscreen||document.mozCancelFullScreen||document.msExitFullscreen;
      if(exit)await exit.call(document);
    }
  }catch(error){job.cancelReason+=' Fullscreen exit: '+(error.message||String(error));}
  finally{castingStudioRestore(job);castingStudioWrite(job.cancelReason||'Fullscreen inspection ended.');}
}

async function inspectCastingStudioFullscreen() {
  var job;
  try{
    job=startCastingStudio();if(job.profile||job.fullscreenPending)return;
    if(caveProfileFullscreenElement()===canvas)return;
    var resumePlaying=job.playing;
    pauseCastingStudio();job.fullscreenPending=true;job.inspectionFullscreen=true;
    var request=canvas.requestFullscreen||canvas.webkitRequestFullscreen||canvas.mozRequestFullScreen||canvas.msRequestFullscreen;
    if(!request)throw Error('Native fullscreen is unavailable.');
    await request.call(canvas);
    await castingStudioNextFrame(job);await castingStudioNextFrame(job);
    if(job.stopped)throw Error(job.cancelReason);
    if(caveProfileFullscreenElement()!==canvas)throw Error('Native fullscreen was not entered.');
    job.fullscreenPending=false;castingStudioDraw(job);if(resumePlaying)playCastingStudio();
    castingStudioWrite('Fullscreen inspection · '+(job.mode==='hand'?'¼-speed anatomy study':'casting sequence')+' · Space play/pause; Escape restores the game.');
  }catch(error){
    if(job){job.fullscreenPending=false;job.stopped=true;job.cancelReason=error.message||String(error);await closeCastingStudioFullscreen(job);}
    else castingStudioWrite(error.message||String(error));
  }
}

function castingStudioFullscreenChanged() {
  var job=_castingStudio;
  if(job&&job.inspectionFullscreen&&!job.fullscreenPending&&!job.profile&&!job.closingFullscreen&&caveProfileFullscreenElement()!==canvas)
    stopCastingStudio('Fullscreen inspection ended; gameplay restored.');
}

function castingStudioRestore(job) {
  if (_castingStudio !== job) return;
  _castingStudio = null;
  canvas.width=job.width; canvas.height=job.height;
  canvas.style.width=job.styleWidth; canvas.style.height=job.styleHeight;
  if (!job.externalAction && floorMesh===job.mesh && WORLD_SEED===job.seed) {
    running=job.wasRunning; lastUpdate=0; _loopGen++;
    if (running) _scheduleLoop();
    else if (CAVE_TEST_MODE && caveVisibilityFixturesEnabled()) drawCaveVisibilityFixtures();
    else renderFrame();
  }
}

function castingStudioNextFrame(job) {
  return new Promise(function (resolve) {
    if (job.stopped) { resolve(null); return; }
    job.resume=resolve;
    job.raf=requestAnimationFrame(function (ts) { job.raf=0; job.resume=null; resolve(ts); });
  });
}

function castingStudioArtworkStats() {
  return {
    hands:typeof getCastingArtworkStats==='function'?getCastingArtworkStats():{builds:0,buildMs:0,bytes:0},
    missile:typeof getMagicMissileArtworkStats==='function'?getMagicMissileArtworkStats():{builds:0,buildMs:0,bytes:0},
    handRig:typeof getHandRigStats==='function'?getHandRigStats():{vertices:0,triangles:0,visibleFaces:0,buildMs:0,builds:0,lastRenderMs:0,topologyBytes:0}
  };
}

function castingStudioRevokeVideo() {
  if(_castingStudioVideoURL){URL.revokeObjectURL(_castingStudioVideoURL);_castingStudioVideoURL=null;}
  var links=document.getElementById('castingStudioVideo');
  if(links)while(links.firstChild)links.removeChild(links.firstChild);
}

function finishCastingStudioRecording(job,discard) {
  var recording=job.recording;if(!recording)return;
  job.recording=null;recording.discard=!!discard;
  try{if(recording.recorder.state!=='inactive')recording.recorder.stop();}
  catch(error){recording.discard=true;}
  finally{recording.stream.getTracks().forEach(function(track){try{track.stop();}catch(error){}});}
}

function recordCastingStudioCycle() {
  if(_castingStudio && (_castingStudio.profile||_castingStudio.recording))return;
  if(typeof MediaRecorder==='undefined'||typeof MediaRecorder.isTypeSupported!=='function'||typeof canvas.captureStream!=='function'){
    castingStudioWrite('This browser cannot record Canvas video. The live comparison still works.');return;
  }
  var types=['video/webm;codecs=vp9','video/webm;codecs=vp8','video/webm'];
  var mime=types.filter(function(type){return MediaRecorder.isTypeSupported(type);})[0];
  if(!mime){castingStudioWrite('WebM recording is not supported here. Use the live comparison; no substitute recording was generated.');return;}
  var job,stream;
  try{
    job=startCastingStudio();pauseCastingStudio();castingStudioResetCycle(job);castingStudioDraw(job);
    stream=canvas.captureStream(30);
    var recorder=new MediaRecorder(stream,{mimeType:mime});
    var recording={recorder:recorder,stream:stream,chunks:[],discard:false,
      filename:(job.mode==='hand'?'hand-motion-'+(job.art?'articulated':'original')+'-'+job.style+'-'+job.grip:'casting-'+(job.art?'rejected-flat':'original'))+
        '-'+job.light+'-'+canvas.width+'x'+canvas.height+'.webm',visualStudy:job.mode==='hand'};
    job.recording=recording;
    recorder.ondataavailable=function(event){if(event.data&&event.data.size)recording.chunks.push(event.data);};
    recorder.onerror=function(){
      recording.discard=true;
      if(job.recording===recording)finishCastingStudioRecording(job,true);
      pauseCastingStudio();castingStudioWrite('Recording failed; stream stopped. Use the live comparison.');
    };
    recorder.onstop=function(){
      if(recording.discard||!recording.chunks.length)return;
      castingStudioRevokeVideo();
      _castingStudioVideoURL=URL.createObjectURL(new Blob(recording.chunks,{type:mime}));
      var links=document.getElementById('castingStudioVideo');
      if(links){
        var link=document.createElement('a');link.href=_castingStudioVideoURL;link.download=recording.filename;
        link.textContent='Download '+recording.filename;links.appendChild(link);
      }
      castingStudioWrite('Recorded one '+(recording.visualStudy?'¼-speed anatomy study (no casting or hit)':'actual casting cycle')+': '+recording.filename+
        '. Capture requests 30 fps at the current canvas resolution; no audio. This is not a benchmark FPS measurement.');
    };
    recorder.start();playCastingStudio();
    castingStudioWrite('Recording one '+(job.mode==='hand'?'¼-speed anatomy study':'actual casting cycle')+' at requested 30 fps, native canvas resolution, no audio…');
  }catch(error){
    if(job&&job.recording)finishCastingStudioRecording(job,true);
    else if(stream)stream.getTracks().forEach(function(track){track.stop();});
    castingStudioWrite('Recording unavailable: '+(error.message||String(error)));
  }
}

async function profileCastingStudio(mode) {
  var job;
  try { job=startCastingStudio(); } catch (error) { castingStudioWrite(error.message); return; }
  if (job.profile||job.fullscreenPending) return;
  if(job.inspectionFullscreen){castingStudioWrite('Exit fullscreen inspection before starting a profile.');return;}
  pauseCastingStudio(); job.profile=true;
  var result={status:'running',mode:mode,study:job.mode,castingStyle:job.mode==='hand'?job.style:null,restingGrip:job.mode==='hand'?job.grip:null,light:job.light,seed:job.seed,view:job.view,
    target:job.mode==='hand'?'One neutral articulated right hand; anatomy proof only, no actual cast or projectile':'Stationary, temporary skeleton; actual cast, projectile, hit and effect logic',
    targetDistance:job.target?job.target.distance:null,versions:[],propCachePilot:typeof getFloorArtworkCacheStats==='function'?
      getFloorArtworkCacheStats().enabled:false};
  try {
    if (mode==='fullscreen') {
      var request=canvas.requestFullscreen||canvas.webkitRequestFullscreen||canvas.mozRequestFullScreen||canvas.msRequestFullscreen;
      if (!request) throw Error('Native fullscreen is unavailable.');
      await request.call(canvas);
      await castingStudioNextFrame(job); await castingStudioNextFrame(job);
      if (caveProfileFullscreenElement()!==canvas) throw Error('Native fullscreen was not entered.');
    }
    result.width=canvas.width;result.height=canvas.height;result.nativeFullscreen=caveProfileFullscreenElement()===canvas;
    result.devicePixelRatio=window.devicePixelRatio||1;
    for (var version=0;version<2;version++) {
      job.art=version===1; castingStudioResetCycle(job);
      // Force a genuine first use; static generation is reported separately
      // from the following steady, identical-time full-motion cycle.
      job.live._armRightIdle=job.live._armRightCast=job.live._armLeftIdle=job.live._armLeftCast=null;
      job.live._armLastRobeId=null;
      if (typeof clearCastingArtworkCache==='function') clearCastingArtworkCache();
      if (typeof clearMagicMissileArtworkCache==='function') clearMagicMissileArtworkCache();
      if(job.mode==='hand'&&typeof clearHandRigCache==='function')clearHandRigCache();
      var before=castingStudioArtworkStats(), coldLegacy=0, firstFrame=null, renders=[], intervals=[], stages={},rigTimes=[];
      var coldBuildFrames=[], hits=[];
      for (var cycle=0;cycle<2;cycle++) {
        castingStudioResetCycle(job);
        var previous=null;
        for (var frame=0;frame<144;frame++) {
          var ts=await castingStudioNextFrame(job);
          if (job.stopped) throw Error(job.cancelReason);
          if (!CAVE_TEST_MODE || floorMesh!==job.mesh || WORLD_SEED!==job.seed) throw Error('World changed during profile.');
          if (canvas.width!==result.width || canvas.height!==result.height) throw Error('Canvas dimensions changed during profile.');
          if (mode==='fullscreen' && caveProfileFullscreenElement()!==canvas) throw Error('Fullscreen ended during profile.');
          castingStudioAdvance(job,frame*castingStudioCycleMs(job)/144);
          var cacheBefore=castingStudioArtworkStats(), sample=castingStudioDraw(job), cacheAfter=castingStudioArtworkStats();
          if (firstFrame===null) firstFrame=sample.renderMs;
          coldLegacy+=sample.legacyBuildMs;
          if (cycle===0 && (sample.legacyBuildMs>0 || cacheAfter.hands.builds>cacheBefore.hands.builds || cacheAfter.missile.builds>cacheBefore.missile.builds||cacheAfter.handRig.builds>cacheBefore.handRig.builds))
            coldBuildFrames.push({timeMs:job.time,renderCommandsMs:sample.renderMs});
          if (cycle===1) {
            renders.push(sample.renderMs); if (previous!==null) intervals.push(ts-previous);
            if(job.mode==='hand'&&job.art)rigTimes.push(cacheAfter.handRig.lastRenderMs);
            Object.keys(sample.stages).forEach(function(name){(stages[name]||(stages[name]=[])).push(sample.stages[name]);});
          }
          previous=ts;
          if (frame%48===0) castingStudioWrite(castingStudioArtLabel(job)+' · '+job.light+' · '+
            (cycle===0?'cold use / warmup':'steady motion sampling')+' · '+frame+'/144 frames');
        }
        if(job.mode!=='hand'){
          if (job.hitAt===null) throw Error('Practice missile did not hit the target in the actual simulation. Choose Mouth and retry.');
          hits.push({requestMs:job.requestAt,releaseMs:job.releaseAt,hitMs:job.hitAt,damage:job.live.stats.totalDamageDone});
        }
      }
      var after=castingStudioArtworkStats();
      result.versions.push({art:castingStudioArtLabel(job),firstFrameRenderCommandsMs:firstFrame,
        firstUseArtworkGenerationMs:{legacyHands:coldLegacy,hands:after.hands.buildMs-before.hands.buildMs,
          missile:after.missile.buildMs-before.missile.buildMs,handRig:after.handRig.buildMs-before.handRig.buildMs},coldBuildFrames:coldBuildFrames,
        handRigColdReset:job.mode==='hand'&&typeof clearHandRigCache==='function',handRig:after.handRig,handRigRenderCommands:caveProfileSummary(rigTimes),
        retainedArtwork:after,steadyRenderCommands:caveProfileSummary(renders),animationFrameIntervals:caveProfileSummary(intervals),
        inclusiveStages:Object.keys(stages).map(function(name){return Object.assign({name:name},caveProfileSummary(stages[name]));})
          .sort(function(a,b){return b.medianMs-a.medianMs;}),actualHits:hits});
    }
    result.status='complete';
    result.notes=[job.mode==='hand'?'Same game scene, pose and light: existing original hands baseline versus one articulated right-hand anatomy study. No combat is simulated.':'Both versions use the same current terrain, pose, lighting, tier-1 spell and scripted 120 Hz simulation steps.',
      job.mode==='hand'?'Time zero is the outward-facing resting grip. Play previews the selected personal Casting Style at quarter speed; grip templates remain independent and equip no item.':'600 ms ready, accepted cast and production 120 ms wind-up, then real release/flight/hit/recovery; no impact is fabricated.',
      'Each version uses one 144-frame cold/warmup cycle followed by one 144-frame steady cycle.',
      'Artwork generation and the first full draw are separate from steady render-command timing.',
      'Command timing may exclude deferred GPU work. Animation-frame intervals are real browser cadence.',
      'No pixel readback or quality reduction; existing prop-cache choice is unchanged.',
      'Fixture equipment keeps the current appearance but disables relic side effects; actual equipment, mana, stats and arrays are restored.'];
  } catch (error) { result.status=job.stopped?'cancelled':'failed';result.reason=error.message||String(error); }
  finally {
    if (job.raf) cancelAnimationFrame(job.raf);job.raf=0;job.resume=null;
    if (mode==='fullscreen' && caveProfileFullscreenElement()===canvas) {
      var exit=document.exitFullscreen||document.webkitExitFullscreen||document.mozCancelFullScreen||document.msExitFullscreen;
      try { if (exit) await exit.call(document); } catch(error) { result.fullscreenExitError=error.message||String(error); }
    }
    job.profile=false;castingStudioRestore(job);castingStudioWrite(result);
  }
}

(function () {
  var host=document.getElementById('caveTestOptions');
  if (!host || !host.appendChild) return;
  var panel=document.createElement('details');panel.id='castingStudioPanel';
  panel.style.cssText='margin-top:8px;padding:6px;border:1px solid #536777;border-radius:4px;background:#17222b';
  var summary=document.createElement('summary');summary.textContent='Casting studio';panel.appendChild(summary);
  var note=document.createElement('p');note.textContent='Handle grip is the default upright resting pose; Play previews your personal Casting Style at ¼ speed. All styles are available; your selection is remembered in this browser when storage is available. Resting grip templates stay independent and show shape only: no item is equipped, no spell is cast. The rejected flat study is archived in the mode selector.';
  note.style.cssText='margin:6px 0;max-width:720px';panel.appendChild(note);
  function select(id,label,options,fn){
    var wrap=document.createElement('label');wrap.style.margin='2px 8px 2px 0';wrap.textContent=label+' ';
    var select=document.createElement('select');select.id=id;select.setAttribute('aria-label',label);
    options.forEach(function(option){var node=document.createElement('option');node.value=option[0];node.textContent=option[1];select.appendChild(node);});
    select.addEventListener('change',function(){fn(select.value);});wrap.appendChild(select);panel.appendChild(wrap);return select;
  }
  select('castingStudioMode','Study mode',[['hand','Hand motion study'],['casting','Rejected flat casting study']],setCastingStudioMode);
  var styleOptions=typeof getCastingStyleOptions==='function'?getCastingStyleOptions():[{id:'arcane',label:'Arcane (default)'}];
  select('castingStudioStyle','Casting Style',styleOptions.map(function(style){return[style.id,style.label];}),setCastingStudioStyle);
  select('castingStudioGrip','Resting pose',[['reach','Reach'],['handle','Handle grip'],['cradle','Cradle']],setCastingStudioHandGrip);
  select('castingStudioHandView','Hand inspection view',[['0','Player view'],['palm','Palm study'],['-0.8','Left side'],['0.8','Right side'],[String(Math.PI),'Back']],setCastingStudioHandView);
  select('castingStudioPhase','Hand pose',[['0','Ready'],['780','Gather'],['1220','Release'],['1560','Follow-through'],['2110','Recovery']],scrubCastingStudio);
  var scrubLabel=document.createElement('label');scrubLabel.textContent='Pose time ';scrubLabel.style.marginRight='8px';
  var scrub=document.createElement('input');scrub.type='range';scrub.id='castingStudioTime';scrub.min='0';scrub.max=String(HAND_STUDIO_CYCLE_MS);scrub.step='10';scrub.value='0';
  scrub.setAttribute('aria-label','Hand pose time');scrub.addEventListener('input',function(){scrubCastingStudio(scrub.value);});
  scrubLabel.appendChild(scrub);panel.appendChild(scrubLabel);panel.appendChild(document.createElement('br'));
  function button(id,label,fn) { var b=document.createElement('button');b.id=id;b.type='button';b.className='btn btn-small';b.textContent=label;
    b.style.margin='2px';b.addEventListener('click',fn);panel.appendChild(b);return b; }
  button('castingStudioOriginal','Original baseline',function(){setCastingStudioArt(false);});
  button('castingStudioRedesigned','Articulated hand',function(){setCastingStudioArt(true);});
  button('castingStudioDaylight','Daylight',function(){setCastingStudioLight('daylight');});
  button('castingStudioDusk','Dusk',function(){setCastingStudioLight('dusk');});
  button('castingStudioPlay','Play comparison sequence',playCastingStudio);
  button('castingStudioPause','Pause',pauseCastingStudio);
  button('castingStudioInspectFullscreen','Fullscreen inspection',inspectCastingStudioFullscreen);
  button('castingStudioRecord','Record one cycle (30 fps capture)',recordCastingStudioCycle);
  button('castingStudioProfilePreview','Profile studio preview',function(){profileCastingStudio('preview');});
  button('castingStudioProfileFullscreen','Profile studio fullscreen',function(){profileCastingStudio('fullscreen');});
  button('castingStudioCancel','Cancel / restore game',function(){stopCastingStudio();});
  var output=document.createElement('pre');output.id='castingStudioResult';output.setAttribute('role','log');output.setAttribute('aria-label','Casting studio result');
  output.style.cssText='white-space:pre-wrap;overflow-wrap:anywhere;max-height:300px;overflow:auto;font-size:11px;margin:6px 0';
  output.textContent='Choose a look and light, then play or profile. Closed by default; no simulation changes until you start.';
  panel.appendChild(output);host.appendChild(panel);
  var video=document.createElement('div');video.id='castingStudioVideo';panel.appendChild(video);
  castingStudioUpdateChoices();
  if(typeof onCastingStyleChange==='function'){
    _castingStudioStyleUnsubscribe=onCastingStyleChange(function(selected){
      if(!_castingStudioSettingStyle)castingStudioApplyStyle(selected,false);
    });
  }
  panel.addEventListener('toggle',function(){if(!panel.open)stopCastingStudio();});
  ['pointerdown','pointerup','pointercancel','pointermove','mousedown','mouseup','mousemove',
    'click','dblclick','contextmenu','wheel','touchstart','touchmove','touchend','touchcancel'].forEach(function(type){
    document.addEventListener(type,castingStudioCanvasInput,{capture:true,passive:false});
  });
  // Cancel before an actual game control mutates/regenerates the world.
  document.addEventListener('click',castingStudioControlEvent,true);
  document.addEventListener('change',castingStudioControlEvent,true);
  document.addEventListener('keydown',function(event){
    if(!_castingStudio)return;
    if(event.key==='Escape'){stopCastingStudio('Cancelled with Escape.');return;}
    if(event.key==='f'||event.key==='F'){
      event.preventDefault();event.stopImmediatePropagation();return;
    }
    if(_castingStudio.inspectionFullscreen&&(event.key===' '||event.code==='Space')){
      event.preventDefault();event.stopImmediatePropagation();
      if(_castingStudio.playing)pauseCastingStudio();else playCastingStudio();
    }
  },true);
  ['fullscreenchange','webkitfullscreenchange','mozfullscreenchange','MSFullscreenChange'].forEach(function(event){
    document.addEventListener(event,castingStudioFullscreenChanged);
  });
  document.addEventListener('visibilitychange',function(){if(document.hidden)stopCastingStudio('Page hidden; studio stopped.');});
  window.addEventListener('pagehide',function(event){
    stopCastingStudio('Page closed; studio stopped.');castingStudioRevokeVideo();
    // Back/forward cache restores this same UI without running setup again.
    // Keep its one listener alive while still releasing preview/capture state.
    if(!event.persisted&&_castingStudioStyleUnsubscribe){_castingStudioStyleUnsubscribe();_castingStudioStyleUnsubscribe=null;}
  });
}());
