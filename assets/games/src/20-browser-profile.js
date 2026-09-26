// Browser-only, user-triggered benchmark. Rendering stays unchanged: this owns
// scheduling and diagnostics, never the simulation loop or quality settings.
var _caveBrowserProfile = null;

function caveProfileSummary(values) {
  if (!values.length) return {samples:0,medianMs:null,p95Ms:null};
  var sorted=values.slice().sort(function(a,b){return a-b;});
  var middle=Math.floor(sorted.length/2);
  return {samples:sorted.length,medianMs:sorted.length%2?sorted[middle]:(sorted[middle-1]+sorted[middle])/2,
    p95Ms:sorted[Math.max(0,Math.ceil(sorted.length*0.95)-1)]};
}

function caveProfileFullscreenElement() {
  return document.fullscreenElement || document.webkitFullscreenElement ||
    document.mozFullScreenElement || document.msFullscreenElement;
}

function caveProfileWrite(value) {
  var output=document.getElementById('ctProfileResult');
  if(output)output.textContent=typeof value==='string'?value:JSON.stringify(value,null,2);
}

function cancelCaveBrowserProfile(reason) {
  var job=_caveBrowserProfile;
  if(!job)return;
  job.cancelReason=reason||'Cancelled by user';
  if(job.raf){cancelAnimationFrame(job.raf);job.raf=0;}
  if(job.resume){var resume=job.resume;job.resume=null;resume(null);}
}

function caveProfileNextFrame(job) {
  return new Promise(function(resolve){
    if(job.cancelReason){resolve(null);return;}
    job.resume=resolve;
    job.raf=requestAnimationFrame(function(ts){job.raf=0;job.resume=null;resolve(ts);});
  });
}

// Globals are restored before returning to the browser, even if a draw throws.
// The clock and random seed are the same in each synchronous render only.
function caveProfileRender(job, collect) {
  var oldDate=Date.now,oldRandom=Math.random,oldPt=_pt,state=job.randomSeed,stages={};
  Date.now=function(){return job.frozenTime;};
  Math.random=function(){state=(Math.imul(state,1664525)+1013904223)|0;return(state>>>0)/4294967296;};
  _pt=function(name,fn){
    var start=performance.now();
    try{return oldPt(name,fn);}
    finally{stages[name]=(stages[name]||0)+(performance.now()-start);}
  };
  var start=performance.now();
  try{drawCaveVisibilityFixtures();}
  finally{Date.now=oldDate;Math.random=oldRandom;_pt=oldPt;}
  var elapsed=performance.now()-start;
  if(collect){
    job.renderMs.push(elapsed);
    Object.keys(stages).forEach(function(name){
      if(!job.stages[name])job.stages[name]=[];
      job.stages[name].push(stages[name]);
    });
  }
  return elapsed;
}

// A frozen draw clock alone cannot rewind random particles or exploration
// accumulated before Pause. Use temporary render inputs, keeping the real
// world's arrays and cached canvases untouched for restoration afterward.
function caveProfilePrepareInputs(job) {
  var saved=job.savedInputs={mesh:floorMesh,seed:WORLD_SEED,
    ambient:ambientParticles,explored:exploredCells,minimap:minimapCanvas,dirty:minimapDirty,
    fog:drawMinimap._fogCanvas,hadFog:Object.prototype.hasOwnProperty.call(drawMinimap,'_fogCanvas'),
    exploredUpdate:_lastExploredUpdate,light:_lightGrid,lightScale:_lightGridLastScale,
    lightX:_lightGridLastCamGX,lightY:_lightGridLastCamGY,lightFrame:_lightGridFrameCount,
    surfaceBake:_surfaceFloorLightBake,surfaceMesh:_surfaceFloorLightMesh,
    surfaceScale:_surfaceFloorLightScale,surfaceStats:_surfaceFloorLightStats,
    showFPS:settings.showFPS,fpsSmooth:_fpsSmooth,lastFrameTime:_lastFrameTimeMs};
  var oldDate=Date.now,oldRandom=Math.random,state=job.randomSeed;
  Date.now=function(){return job.frozenTime;};
  Math.random=function(){state=(Math.imul(state,1664525)+1013904223)|0;return(state>>>0)/4294967296;};
  try{
    initAmbientParticles();
    updateAmbientParticles(0);
    initMinimap();
    var fog=document.createElement('canvas');fog.width=MINIMAP_W;fog.height=MINIMAP_H;
    drawMinimap._fogCanvas=fog;minimapDirty=true;_lastExploredUpdate=job.frozenTime;
    if(_lightGrid)_lightGrid=new Float32Array(_lightGrid.length);
    _lightGridLastScale=-Infinity;_lightGridLastCamGX=-9999;_lightGridLastCamGY=-9999;
    _lightGridFrameCount=4;
    updateLightGrid();
    // This is a profiling overlay, not a scene-quality feature. Its prior
    // smoothed rate is unrelated to this frozen, manually paced fixture.
    settings.showFPS=false;
  }finally{
    saved.ownedAmbient=ambientParticles;saved.ownedExplored=exploredCells;
    saved.ownedMinimap=minimapCanvas;saved.ownedFog=drawMinimap._fogCanvas;saved.ownedLight=_lightGrid;
    Date.now=oldDate;Math.random=oldRandom;
  }
}

function caveProfileRestoreInputs(job) {
  var saved=job.savedInputs;if(!saved)return;
  // A cancelled run must not put old arrays back over a newly generated world.
  if(floorMesh===saved.mesh && WORLD_SEED===saved.seed){
    if(ambientParticles===saved.ownedAmbient)ambientParticles=saved.ambient;
    if(exploredCells===saved.ownedExplored)exploredCells=saved.explored;
    if(minimapCanvas===saved.ownedMinimap)minimapCanvas=saved.minimap;
    if(drawMinimap._fogCanvas===saved.ownedFog){
      if(saved.hadFog)drawMinimap._fogCanvas=saved.fog;else delete drawMinimap._fogCanvas;
    }
    minimapDirty=saved.dirty;_lastExploredUpdate=saved.exploredUpdate;
    if(_lightGrid===saved.ownedLight)_lightGrid=saved.light;
    _lightGridLastScale=saved.lightScale;_lightGridLastCamGX=saved.lightX;
    _lightGridLastCamGY=saved.lightY;_lightGridFrameCount=saved.lightFrame;
    // The upper-terrain bake is immutable and shared; only its animated scale
    // changes during profiling. Never restore over a newly baked world.
    if(_surfaceFloorLightBake===saved.surfaceBake && _surfaceFloorLightMesh===saved.surfaceMesh){
      _surfaceFloorLightScale=saved.surfaceScale;
      _surfaceFloorLightStats=saved.surfaceStats;
    }
  }
  settings.showFPS=saved.showFPS;_fpsSmooth=saved.fpsSmooth;_lastFrameTimeMs=saved.lastFrameTime;
  job.savedInputs=null;
}

function caveProfileOptionalSignature(verifyPixels) {
  if(!verifyPixels)return {skipped:true,
    reason:'Pixel verification disabled; avoids readback changing later Canvas performance.'};
  return caveProfileContentSignature();
}

function caveProfileContentSignature() {
  // Explicitly outside timed samples: readback can synchronously flush GPU work.
  var width=canvas.width,height=canvas.height;
  var pixels=ctx.getImageData(0,0,width,height).data,hash=2166136261,tiles=[];
  for(var row=0;row<3;row++)for(var col=0;col<4;col++)tiles.push({column:col,row:row,hash:2166136261,bytes:0});
  for(var y=0;y<height;y++)for(var x=0;x<width;x++){
    var tile=tiles[Math.min(2,Math.floor(y*3/height))*4+Math.min(3,Math.floor(x*4/width))];
    var index=(y*width+x)*4;
    for(var channel=0;channel<4;channel++){
      var value=pixels[index+channel];hash=Math.imul(hash^value,16777619);
      tile.hash=Math.imul(tile.hash^value,16777619);tile.bytes++;
    }
  }
  function hex(value){return('00000000'+(value>>>0).toString(16)).slice(-8);}
  tiles.forEach(function(tile){tile.hash=hex(tile.hash);});
  return {algorithm:'fnv1a32-rgba',hash:hex(hash),bytes:pixels.length,tileColumns:4,tileRows:3,tiles:tiles};
}

async function startCaveBrowserProfile(mode) {
  if(_caveBrowserProfile){caveProfileWrite('A profile is already running; cancel it before starting another.');return;}
  if(!CAVE_TEST_MODE){caveProfileWrite({status:'unavailable',reason:'Select Cave Test first.'});return;}
  if(caveProfileFullscreenElement()){caveProfileWrite({status:'unavailable',reason:'Exit fullscreen before starting a new profile.'});return;}
  var fixtureCheckbox=document.getElementById('ctVisibilityFixtures');
  if(!fixtureCheckbox){caveProfileWrite({status:'unavailable',reason:'Visibility fixtures are unavailable.'});return;}
  var verifyCheckbox=document.getElementById('ctProfileVerifyPixels');
  var job={mode:mode,verifyPixels:!!(verifyCheckbox && verifyCheckbox.checked),width:canvas.width,height:canvas.height,styleWidth:canvas.style.width,styleHeight:canvas.style.height,
    seed:WORLD_SEED,view:cavePreviewLastView,mesh:null,raf:0,resume:null,cancelReason:null,
    frozenTime:2000000000000,randomSeed:1234567,renderMs:[],intervals:[],stages:{},enteredFullscreen:false,
    perfHud:DEBUG_PERF_HUD,caveDebug:DEBUG_CAVE};
  _caveBrowserProfile=job;
  var buttons=['ctProfilePreview','ctProfileFullscreen','ctProfileViewport'];
  buttons.forEach(function(id){var b=document.getElementById(id);if(b)b.disabled=true;});
  var cancel=document.getElementById('ctProfileCancel');if(cancel)cancel.disabled=false;
  var result={status:'running',mode:mode,warmups:12,requestedSamples:60};
  var baselineErrors={};
  Object.keys(_ptErrors).forEach(function(key){baselineErrors[key]=_ptErrors[key];});
  try{
    // Request native fullscreen synchronously in the click's activation window.
    var fullscreenPromise=null;
    if(mode==='fullscreen'){
      var request=canvas.requestFullscreen || canvas.webkitRequestFullscreen || canvas.mozRequestFullScreen || canvas.msRequestFullscreen;
      if(typeof request!=='function')throw Error('Native fullscreen is unsupported. Use the explicitly labelled viewport-size fallback.');
      fullscreenPromise=request.call(canvas);
    }
    fixtureCheckbox.checked=true;
    DEBUG_PERF_HUD=false;DEBUG_CAVE=false;
    previewCaveView(job.view);
    job.mesh=floorMesh;
    caveProfileWrite('Preparing '+mode+' profile; visibility fixtures are paused.');
    if(fullscreenPromise && typeof fullscreenPromise.then==='function')await fullscreenPromise;
    // Let fullscreenchange and its existing resize callback finish first.
    await caveProfileNextFrame(job);await caveProfileNextFrame(job);
    if(job.cancelReason)throw Error(job.cancelReason);
    if(mode==='fullscreen'){
      if(caveProfileFullscreenElement()!==canvas)throw Error('The browser did not enter native fullscreen. Use the labelled viewport-size fallback.');
      job.enteredFullscreen=true;
    } else if(mode==='viewport-size'){
      canvas.width=Math.max(1,Math.floor(window.innerWidth));
      canvas.height=Math.max(1,Math.floor(window.innerHeight));
      canvas.style.width='100%';canvas.style.height='auto';
    }
    result.width=canvas.width;result.height=canvas.height;
    result.nativeFullscreen=caveProfileFullscreenElement()===canvas;
    result.devicePixelRatio=window.devicePixelRatio||1;
    result.seed=job.seed;result.view=job.view;
    result.fixtureObjects=['skeleton','chest','fire orb'];
    result.frozenTime=job.frozenTime;result.randomSeed=job.randomSeed;
    caveProfilePrepareInputs(job);
    result.surfaceLightingBake=typeof getSurfaceFloorLightStats==='function'?getSurfaceFloorLightStats():null;
    result.canonicalInputs={ambientParticles:ambientParticles.length,exploration:'radius 30 around fixture',
      lighting:'forced frozen-time update',profilingOverlays:false};
    var previous=null;
    for(var frame=0;frame<72;frame++){
      var ts=await caveProfileNextFrame(job);
      if(job.cancelReason)throw Error(job.cancelReason);
      if(!CAVE_TEST_MODE || WORLD_SEED!==job.seed || cavePreviewLastView!==job.view || floorMesh!==job.mesh || !caveVisibilityFixturesEnabled())
        throw Error('Fixture or viewpoint changed during profile.');
      if(canvas.width!==result.width || canvas.height!==result.height)throw Error('Canvas dimensions changed during profile.');
      if(mode==='fullscreen' && caveProfileFullscreenElement()!==canvas)throw Error('Fullscreen ended before profiling completed.');
      if(frame>=12 && previous!==null)job.intervals.push(ts-previous);
      caveProfileRender(job,frame>=12);
      previous=ts;
      if(frame===11)caveProfileWrite('Warmups complete. Sampling 60 animation frames at '+canvas.width+' × '+canvas.height+'…');
    }
    result.status='complete';
    result.renderCommands=caveProfileSummary(job.renderMs);
    result.animationFrameIntervals=caveProfileSummary(job.intervals);
    result.topInclusiveStages=Object.keys(job.stages).map(function(name){
      var summary=caveProfileSummary(job.stages[name]);summary.name=name;return summary;
    }).sort(function(a,b){return b.medianMs-a.medianMs;}).slice(0,14);
    result.errors={};Object.keys(_ptErrors).forEach(function(key){
      var count=_ptErrors[key]-(baselineErrors[key]||0);if(count)result.errors[key]=count;
    });
    result.contentSignature=caveProfileOptionalSignature(job.verifyPixels);
    result.notes=['Render-command timing excludes final image readback and may exclude deferred GPU work.',
      'Animation-frame intervals measure browser cadence, not 1000 / JavaScript time.',
      'Stage times are inclusive; do not add parent draw() to its children.',
      'Ambient particles, exploration and lighting are canonical temporary inputs; real-world references are restored afterward.',
      job.verifyPixels?'Pixel verification can change the browser Canvas backend; reload before a fresh performance comparison.':'Pixel verification is disabled; no main-canvas readback is performed.',
      mode==='viewport-size'?'Viewport-size fallback is not native fullscreen.':'No resolution or quality reduction was applied.'];
  }catch(error){
    result.status=job.cancelReason?'cancelled':'failed';result.reason=error.message||String(error);
    result.completedSamples=job.renderMs.length;
  }finally{
    if(job.raf)cancelAnimationFrame(job.raf);
    job.raf=0;job.resume=null;
    if(mode==='fullscreen' && caveProfileFullscreenElement()===canvas){
      var exit=document.exitFullscreen || document.webkitExitFullscreen || document.mozCancelFullScreen || document.msExitFullscreen;
      try{if(exit)await exit.call(document);}catch(exitError){result.exitFullscreenError=exitError.message||String(exitError);}
    }
    canvas.width=job.width;canvas.height=job.height;
    canvas.style.width=job.styleWidth;canvas.style.height=job.styleHeight;
    DEBUG_PERF_HUD=job.perfHud;DEBUG_CAVE=job.caveDebug;
    caveProfileRestoreInputs(job);
    _caveBrowserProfile=null;
    buttons.forEach(function(id){var b=document.getElementById(id);if(b)b.disabled=false;});
    if(cancel)cancel.disabled=true;
    if(CAVE_TEST_MODE && caveVisibilityFixturesEnabled()){
      try{previewCaveView(cavePreviewLastView);}catch(redrawError){result.redrawError=redrawError.message||String(redrawError);}
    }
    caveProfileWrite(result);
  }
}

(function(){
  [['ctProfilePreview','preview'],['ctProfileFullscreen','fullscreen'],['ctProfileViewport','viewport-size']].forEach(function(pair){
    var button=document.getElementById(pair[0]);if(button)button.addEventListener('click',function(){startCaveBrowserProfile(pair[1]);});
  });
  var cancel=document.getElementById('ctProfileCancel');if(cancel)cancel.addEventListener('click',function(){cancelCaveBrowserProfile();});
  // Capture phase cancels before the existing controls mutate/reset the world.
  document.addEventListener('change',function(event){
    if(_caveBrowserProfile && event.target && event.target.id!=='ctProfileCancel')cancelCaveBrowserProfile('Control changed during profile.');
  },true);
  document.addEventListener('click',function(event){
    if(!_caveBrowserProfile || !event.target)return;
    var id=event.target.id||'';
    if(id.indexOf('ctView')===0 || ['btnStart','btnStop','btnEndless','btnOverview','btnView2D','btnView3D','btnToggleTex'].indexOf(id)>=0)
      cancelCaveBrowserProfile('View or game mode changed during profile.');
  },true);
  document.addEventListener('visibilitychange',function(){if(document.hidden)cancelCaveBrowserProfile('Page became hidden during profile.');});
  window.addEventListener('pagehide',function(){cancelCaveBrowserProfile('Page navigation interrupted profile.');});
}());
