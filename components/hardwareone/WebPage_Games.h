#ifndef WEBPAGE_GAMES_H
#define WEBPAGE_GAMES_H

#include <Arduino.h>
#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif
#include "WebServer_Utils.h"

#if ENABLE_GAMES

// Client-only Tilt Maze prototype
// - Uses IMU endpoint: /api/sensors?sensor=imu
// - Polling starts ONLY after pressing Start
// - Uses shared page shell/navigation

// Streamed inner content for games page
inline void streamGamesInner(httpd_req_t* req) {
  // CSS
  httpd_resp_send_chunk(req, R"CSS(
<style>
.games-wrap{max-width:1000px;margin:0 auto}
.row{display:flex;gap:16px;flex-wrap:wrap}
.col{flex:1 1 320px} .card-light{background:var(--panel-bg);color:var(--panel-fg);border-radius:12px;padding:16px;border:1px solid var(--border)}
.hud{display:flex;gap:12px;flex-wrap:wrap;margin-top:8px;font-family:'Courier New',monospace;color:var(--panel-fg)}
canvas#maze{background:#000;border:1px solid var(--border);border-radius:4px}
</style>
)CSS", HTTPD_RESP_USE_STRLEN);

  // HTML structure
  httpd_resp_send_chunk(req, R"HTML(
<div class='games-wrap'>
  <h2>Games</h2>
  <p class='text-sm'>Client-side prototype for Tilt Maze. IMU polling starts when you click Start.</p>
  <div class='row'>
    <div class='col'>
      <div class='card-light'>
        <h3 style='margin-bottom:8px'>Tilt Maze</h3>
        <div class='btn-row'>
          <button class='btn' id='btnStart'>Start</button>
          <button class='btn' id='btnStop'>Stop</button>
          <select id='terrainSelect' class='input-tall' style='width:auto'>
            <option value='ground'>Ground</option>
            <option value='ice'>Icy</option>
            <option value='cave'>Cave</option>
            <option value='plains'>Plains (test)</option>
          </select>
          <label style='display:inline-flex;align-items:center;gap:6px;cursor:pointer;margin-left:12px' title='Use Seesaw gamepad to control the maze'>
            <input type='checkbox' id='chkGamepad'/> Use gamepad for movement
          </label>
        </div>
        <div class='space-top-sm text-sm'>
          <button class='btn btn-small' id='btnView2D'>2D View</button>
          <button class='btn btn-small' id='btnView3D'>3D View (beta)</button>
          <button class='btn btn-small' id='btnToggleTex'>Toggle Textures</button>
        </div>
        <div class='space-top-sm text-sm'>
          <label style='display:inline-flex;align-items:center;gap:6px;cursor:pointer'>
            <input type='checkbox' id='chkImuDebug'/> IMU debug to console
          </label>
          <span class='space-left-md'></span>
          <label style='display:inline-flex;align-items:center;gap:6px;cursor:pointer'>
            <input type='checkbox' id='chk3dDebug'/> 3D debug overlay
          </label>
          <span class='space-left-md'></span>
          <label style='display:inline-flex;align-items:center;gap:6px;cursor:pointer'>
            <input type='checkbox' id='chkTexDebug'/> Texture debug
          </label>
          <span class='space-left-md'></span>
          <label style='display:inline-flex;align-items:center;gap:6px;cursor:pointer'>
            <input type='checkbox' id='chkFloorDebug'/> Floor debug
          </label>
          <span class='space-left-md'></span>
          <label style='display:inline-flex;align-items:center;gap:6px;cursor:pointer'>
            <input type='checkbox' id='chkDecorDebug'/> Decoration debug
          </label>
          <span class='space-left-md'></span>
          <label style='display:inline-flex;align-items:center;gap:6px;cursor:pointer' title='During 3D render, set camera position equal to player position'>
            <input type='checkbox' id='chkCamFollow'/> Sync camera to player
          </label>
          <span class='space-left-md'></span>
          <button class='btn btn-small' id='btnFwDbgOn' title='Enable firmware sensor debug flags'>FW Debug ON</button>
          <button class='btn btn-small' id='btnFwDbgOff' title='Disable firmware sensor debug flags'>FW Debug OFF</button>
        </div>
        <div class='hud'>
          <div>Level: <span id='hudLevel'>1</span></div>
          <div id='hudCal' class='text-muted'>Not calibrated</div>
          <div>Collisions: <span id='hudCol'>0</span></div>
          <div>Input: <span id='hudInput'>IMU</span></div>
        </div>
        <div class='space-top-md'>
          <canvas id='maze' width='360' height='240'></canvas>
        </div>
        <p class='text-sm space-top-sm'>Hold the device flat. Roll controls X, Pitch controls Y. Keep movements gentle.</p>
    </div>
  </div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript (complete game logic)
  httpd_resp_send_chunk(req, R"JS(
<script>console.log('[GAMES] Section 1: Pre-script sentinel');</script>
<script>
console.info('[GAMES] Chunk 1: bootstrap + state');
var polling=null, lastUpdate=0, running=false;
var i2cEnabled=false, imuEnabled=false, imuCompiled=false, gamepadEnabled=false, gamepadCompiled=false;
var level=1, collisions=0, startMs=0;
var canvas=document.getElementById('maze'), ctx=canvas.getContext('2d');
var pos={x:20,y:20}, vel={x:0,y:0};
var dead=1.5, maxAng=15, speed=180;
var walls=[]; var goal={x:320,y:200,w:24,h:24};
var basePitch=0, baseRoll=0, baseYaw=0, hasBaseline=false;
var calibTimer=null, calibMs=1000, calibSamples=[];
var DEBUG_IMU=false;
var FW_DEBUG=false;
var __gamesLogPoll=null, __gamesLogLastLen=0;
var terrain='ground', damping=0.965, bounce=0.45;
var medalGold=10, medalSilver=15, medalBronze=22;
var timeSec=0, timeColor='#d4af37';
var bgPattern=null;
var MODE3D=false;
var cam={x:20,y:20,ang:0,fov:1.0471975512};
var viewCam={x:0,y:0};
var grid=null, gridW=0, gridH=0, cell=15, wallHeights=null;
var wallColor='#8b6b3a'; var ceilPattern=null;
var DEBUG_3D=false, __dbg3dLast=0;
var DEBUG_TEXTURE=false, __dbgTexLast=0;
var DEBUG_FLOOR=false, __dbgFloorLast=0;
var USE_TEXTURES=true;
var wallBatch=null;
var wallDecorations=[];
var depthBuffer=null;
var DEBUG_DECORATIONS=false;
var CAM_FOLLOW=false;
var BASE_SEED=54321;
function seedFor(component){ return (BASE_SEED + level*101 + component)|0; }
var USE_GAMEPAD=false;
var MODE_GYRO_AIM=1, MODE_STICK_AIM=2;

var CONTROL_MODE=2;

var BTN_X=(1<<6), BTN_Y=(1<<2), BTN_A=(1<<5), BTN_B=(1<<1);

var BTN_SELECT=(1<<0);

var BTN_START=(1<<16);

var lastSelectDown=false, lastSelectToggleMs=0, SELECT_TOGGLE_COOLDOWN=300, selectPrimed=false;

var menuOpen=false, lastStartMenuDown=false, lastStartMenuToggleMs=0, START_MENU_COOLDOWN=300;

var STICK_CAM_YAW_SPEED=1.8, STICK_CAM_PITCH_SPEED=1.2, CAM_PITCH_MAX=0.6;

var projectiles=[], lastStartDown=false, lastShotMs=0, lastStartCastDown=false;

var SHOOT_COOLDOWN=300, PROJ_SPEED=360, PROJ_LIFE_MS=1200, PROJ_RADIUS=3;

var MANA_MAX=100, mana=100, MANA_REGEN_PER_S=15;

var MANA_COST_MIN=12, MANA_COST_MAX=35;

var CHARGE_MAX_MS=1200, charging=false, chargeStartMs=0, chargeLevel=0;

var manaBlinkUntil=0;

var HEALTH_MAX=100, health=100, HEALTH_REGEN_PER_S=3;

var USE_YAW=true, yawTarget=0, yawAlpha=0.08;
var USE_PITCH=true, pitchTarget=0, pitchAlpha=0.12, pitchOffset=0;
var calibrating=false, calStableMs=0, calNeedStableMs=1200;
var lastRoll=0, lastPitch=0, lastYaw=0, hasYaw=false;
var gpPoll=null, gpLast={x:0,y:0,buttons:0,valid:false}, gpBackoffUntil=0;
var yawOffset=0, capturingYaw=false, yawAtPress=0, camHold=0, yawResumeUntil=0, prevX=false;
var DEBUG_CAM=false, __camDbgLast=0;
var DEBUG_PERF=true, __perfFrames=[], __perfLogInterval=2000, __perfLastLog=0;
var DEBUG_EFFECTS=true, __effectsLastLog=0, __effectsLogInterval=3000;
var platforms=[], floorMesh=null, PLATFORM_COLS=['#6aff6a','#6aa7ff','#ffb36a','#efe36a'];
var enemies=[];
var deathEffects=[];
var stats={totalDamageDone:0,totalDamageTaken:0,totalManaConsumed:0,totalEnemiesKilled:0};
var enemyTypes={
  fast:{id:'fast',name:'Scout',color:'#ff9900',size:0.8,health:2,speed:55,chaseRange:450},
  normal:{id:'normal',name:'Soldier',color:'#ff4444',size:1.0,health:4,speed:35,chaseRange:400},
  tank:{id:'tank',name:'Brute',color:'#cc0000',size:1.4,health:8,speed:20,chaseRange:350}
};
var spells={
  missile:{id:'missile',name:'Magic Missile',color:'#4db6ff',damage:1,speed:360,manaCost:12,unlocked:true},
  fire:{id:'fire',name:'Flame Jet',color:'#ff6600',damage:1.5,speed:300,manaCost:18,unlocked:true,attackType:'cone',coneAngle:0.25,coneRange:150},
  ice:{id:'ice',name:'Frost Jet',color:'#00ffff',damage:1.2,speed:340,manaCost:15,unlocked:true,attackType:'cone',coneAngle:1.396,coneRange:75},
  lightning:{id:'lightning',name:'Lightning',color:'#ffff00',damage:1.8,speed:400,manaCost:20,unlocked:true}
};
var spellOrder=['missile','fire','ice','lightning'];
var currentSpellIdx=0;
var lastADown=false;
var lastXDown=false;
var levels=[
{start:{x:40,y:40},goal:{x:640,y:400,w:32,h:32},medals:{gold:15,silver:22,bronze:32},walls:[{x:160,y:0,w:15,h:360},{x:320,y:120,w:15,h:360}]},
{start:{x:40,y:40},goal:{x:640,y:400,w:32,h:32},medals:{gold:20,silver:28,bronze:40},walls:[{x:120,y:80,w:480,h:15},{x:120,y:240,w:480,h:15},{x:120,y:400,w:480,h:15}]},
{start:{x:40,y:40},goal:{x:640,y:400,w:32,h:32},medals:{gold:25,silver:35,bronze:50},walls:[{x:200,y:0,w:15,h:480},{x:400,y:0,w:15,h:480},{x:0,y:240,w:720,h:15}]}
];
function makePattern(kind){ var oc=document.createElement('canvas'); oc.width=32; oc.height=32; var c=oc.getContext('2d');
  if(kind==='ice'){
    c.fillStyle='#0a1322'; c.fillRect(0,0,32,32);
    c.strokeStyle='rgba(170,210,255,0.35)'; c.lineWidth=2; c.beginPath(); c.moveTo(0,16); c.lineTo(32,16); c.moveTo(16,0); c.lineTo(16,32); c.stroke();
    c.strokeStyle='rgba(120,190,255,0.18)'; c.beginPath(); c.moveTo(0,0); c.lineTo(32,32); c.moveTo(32,0); c.lineTo(0,32); c.stroke();
    for(var i=0;i<14;i++){ var x=Math.random()*32, y=Math.random()*32, r=Math.random()*0.9+0.3; c.fillStyle='rgba(210,235,255,'+(0.05+Math.random()*0.05)+')'; c.beginPath(); c.arc(x,y,r,0,Math.PI*2); c.fill(); }
    var g=c.createLinearGradient(0,0,32,32); g.addColorStop(0,'rgba(255,255,255,0.02)'); g.addColorStop(1,'rgba(255,255,255,0.00)'); c.fillStyle=g; c.fillRect(0,0,32,32);
  } else if(kind==='cave'){
    c.fillStyle='#2a2a2e'; c.fillRect(0,0,32,32);
    for(var i=0;i<32;i++){ var x=Math.random()*32, y=Math.random()*32, r=Math.random()*1.4+0.5; var a=0.08+Math.random()*0.12; var pick=Math.random(); if(pick<0.4){ c.fillStyle='rgba(80,80,85,'+a+')'; } else if(pick<0.75){ c.fillStyle='rgba(60,60,65,'+a+')'; } else { c.fillStyle='rgba(100,100,105,'+(a*0.8)+')'; } c.beginPath(); c.arc(x,y,r,0,Math.PI*2); c.fill(); }
    for(var j=0;j<4;j++){ var w=8+Math.random()*12, hgt=8+Math.random()*12; var px=Math.random()*(32-w), py=Math.random()*(32-hgt); c.fillStyle='rgba(70,70,75,'+(0.03+Math.random()*0.04)+')'; c.fillRect(px,py,w,hgt); }
  } else {
    c.fillStyle='#3b2a18'; c.fillRect(0,0,32,32);
    for(var i=0;i<28;i++){
      var x=Math.random()*32, y=Math.random()*32, r=Math.random()*1.6+0.6;
      var a=0.10+Math.random()*0.10; var pick=Math.random();
      if(pick<0.5){ c.fillStyle='rgba(124,93,60,'+a+')'; }
      else if(pick<0.8){ c.fillStyle='rgba(98,72,45,'+a+')'; }
      else { c.fillStyle='rgba(160,120,80,'+(a*0.9)+')'; }
      c.beginPath(); c.arc(x,y,r,0,Math.PI*2); c.fill();
    }
    for(var j=0;j<3;j++){
      var w=10+Math.random()*14, hgt=10+Math.random()*14;
      var px=Math.random()*(32-w), py=Math.random()*(32-hgt);
      c.fillStyle='rgba(255,230,200,'+(0.02+Math.random()*0.025)+')';
      c.fillRect(px,py,w,hgt);
    }
  }
  return ctx.createPattern(oc,'repeat'); }
function makeCeilPattern(kind){
  var oc=document.createElement('canvas'); oc.width=32; oc.height=32;
  var c=oc.getContext('2d');
  if(kind==='ice'){ c.fillStyle='#0d1a2e'; }
  else if(kind==='cave'){ c.fillStyle='#1a1a1e'; }
  else { c.fillStyle='#3f2f1c'; }
  c.fillRect(0,0,32,32);
  c.fillStyle='rgba(255,255,255,0.02)'; c.fillRect(0,0,16,16);
  return ctx.createPattern(oc,'repeat');
}
function makeWallColor(kind){ return (kind==='ice')?'#7aa7ff':(kind==='cave')?'#6a6a70':'#a77a45'; }
function applyPreset(){
  if(terrain==='ice'){ damping=0.992; bounce=0.20; dead=1.2; }
  else if(terrain==='plains'){ damping=0.970; bounce=0.40; dead=1.5; }
  else if(terrain==='cave'){ damping=0.975; bounce=0.35; dead=1.3; }
  else { damping=0.965; bounce=0.45; dead=1.5; }
  var k=(terrain==='ice')?'ice':(terrain==='cave')?'cave':'ground';
  bgPattern = makePattern(k); ceilPattern = makeCeilPattern(k); wallColor = makeWallColor(k);
}
</script><script>console.info('[GAMES] Chunk 1B: presets');
function generateCaveWalls(){
  if(terrain!=='cave') return;
  Math.seedrandom=function(seed){
    var m=0x80000000,a=1103515245,c=12345,s=seed?seed:Math.floor(Math.random()*(m-1));
    return function(){ s=(a*s+c)%m; return s/(m-1); };
  };
  var rng=Math.seedrandom(seedFor(1));
  var worldW=720,worldH=480,margin=40,buffer=35;
  function overlaps(r){
    for(var j=0;j<walls.length;j++){
      var e=walls[j];
      if(!(r.x+r.w+buffer<e.x||r.x>e.x+e.w+buffer||r.y+r.h+buffer<e.y||r.y>e.y+e.h+buffer)) return true;
    }
    return false;
  }
  function blocks(r){
    var bs=(r.x<pos.x+buffer&&r.x+r.w>pos.x-buffer&&r.y<pos.y+buffer&&r.y+r.h>pos.y-buffer);
    var bg=(r.x<goal.x+buffer&&r.x+r.w>goal.x-buffer&&r.y<goal.y+buffer&&r.y+r.h>goal.y-buffer);
    return bs||bg;
  }
  function tryAdd(rects){
    for(var i=0;i<rects.length;i++){
      var r=rects[i];
      if(r.x<margin||r.y<margin||r.x+r.w>worldW-margin||r.y+r.h>worldH-margin) return false;
      if(overlaps(r)||blocks(r)) return false;
    }
    for(var k=0;k<rects.length;k++){ walls.push(rects[k]); }
    return true;
  }
  var shapes=['rect','L','T','plus','corr_h','corr_v','zigzag','pillars','U','arc'];
  var targetShapes=Math.floor(6+rng()*6),addedRects=0,placedShapes=0,attempts=targetShapes*30;
  while(placedShapes<targetShapes && attempts-->0){
    var shape=shapes[Math.floor(rng()*shapes.length)];
    var cx=Math.floor(margin+rng()*(worldW-2*margin)), cy=Math.floor(margin+rng()*(worldH-2*margin));
    var scale=20+rng()*60, rects=[];
    if(shape==='rect'){
      var w=Math.floor(scale*0.6+rng()*scale), h=Math.floor(scale*0.6+rng()*scale);
      rects=[{x:cx,y:cy,w:w,h:h}];
    } else if(shape==='L'){
      var arm=Math.floor(scale*0.8), th=Math.floor(10+rng()*18);
      rects=[{x:cx,y:cy,w:arm,h:th},{x:cx,y:cy,w:th,h:arm}];
    } else if(shape==='T'){
      var arm2=Math.floor(scale), th2=Math.floor(10+rng()*18);
      rects=[{x:cx-Math.floor(arm2/2),y:cy,w:arm2,h:th2},{x:cx-Math.floor(th2/2),y:cy,w:th2,h:arm2}];
    } else if(shape==='plus'){
      var arm3=Math.floor(scale*0.8), th3=Math.floor(10+rng()*16);
      rects=[{x:cx-Math.floor(arm3/2),y:cy-Math.floor(th3/2),w:arm3,h:th3},{x:cx-Math.floor(th3/2),y:cy-Math.floor(arm3/2),w:th3,h:arm3}];
    } else if(shape==='corr_h'){
      var len=Math.floor(scale*1.4), gap=Math.floor(18+rng()*28), th4=Math.floor(10+rng()*14);
      rects=[{x:cx,y:cy,w:len,h:th4},{x:cx,y:cy+gap,w:len,h:th4}];
    } else if(shape==='corr_v'){
      var len2=Math.floor(scale*1.4), gap2=Math.floor(18+rng()*28), th5=Math.floor(10+rng()*14);
      rects=[{x:cx,y:cy,w:th5,h:len2},{x:cx+gap2,y:cy,w:th5,h:len2}];
    } else if(shape==='zigzag'){
      var seg=Math.floor(14+rng()*18), th6=Math.floor(10+rng()*14);
      rects=[{x:cx,y:cy,w:seg,h:th6},{x:cx+seg-Math.floor(th6/2),y:cy+seg,w:seg,h:th6},{x:cx+2*seg-Math.floor(th6),y:cy+2*seg,w:seg,h:th6}];
    } else if(shape==='pillars'){
      var n=3+Math.floor(rng()*3), s=Math.floor(12+rng()*18);
      for(var i=0;i<n;i++){
        var ox=Math.floor((rng()-0.5)*scale), oy=Math.floor((rng()-0.5)*scale);
        rects.push({x:cx+ox,y:cy+oy,w:s,h:s});
      }
    } else if(shape==='U'){
      var arm4=Math.floor(scale), th7=Math.floor(10+rng()*16), inn=Math.floor(scale*0.5);
      rects=[{x:cx,y:cy,w:th7,h:arm4},{x:cx+inn,y:cy,w:th7,h:arm4},{x:cx,y:cy+arm4-th7,w:inn+th7,h:th7}];
    } else {
      var th8=Math.floor(10+rng()*16), r=Math.floor(scale*0.8), seg2=Math.floor(r/2);
      rects=[{x:cx-r,y:cy,w:seg2,h:th8},{x:cx-Math.floor(seg2/2),y:cy+Math.floor(seg2/2),w:seg2,h:th8},{x:cx,y:cy+seg2,w:seg2,h:th8}];
    }
    if(tryAdd(rects)){
      addedRects+=rects.length;
      placedShapes++;
    }
  }
  walls.caveCount=addedRects;
}
function buildGrid(){
  var worldW=720, worldH=480;
  gridW=Math.floor(worldW/cell);
  gridH=Math.floor(worldH/cell);
  grid=new Array(gridW*gridH);
  wallHeights=new Array(gridW*gridH);
  Math.seedrandom = function(seed){
    var m = 0x80000000;
    var a = 1103515245;
    var c = 12345;
    var state = seed ? seed : Math.floor(Math.random() * (m - 1));
    return function(){
      state = (a * state + c) % m;
      return state / (m - 1);
    };
  };
  var rng = Math.seedrandom(seedFor(2));
  for(var i=0;i<grid.length;i++){
    grid[i]=0;
    wallHeights[i]=0.7 + rng()*0.6;
  }
  var caveWallStartIdx = walls.length;
  if(terrain==='cave'){
    caveWallStartIdx = walls.length - (walls.caveCount || 0);
  }
  for(var wi=0;wi<walls.length;wi++){
    var w=walls[wi];
    var isCaveWall = (terrain==='cave' && wi >= caveWallStartIdx);
    var heightForWall = isCaveWall ? (0.3 + rng()*0.4) : (0.7 + rng()*0.6);
    var x0=Math.max(0,Math.floor(w.x/cell));
    var y0=Math.max(0,Math.floor(w.y/cell));
    var x1=Math.min(gridW-1,Math.floor((w.x+w.w-1)/cell));
    var y1=Math.min(gridH-1,Math.floor((w.y+w.h-1)/cell));
    for(var y=y0;y<=y1;y++){
      for(var x=x0;x<=x1;x++){
        grid[y*gridW+x]=1;
        wallHeights[y*gridW+x]=heightForWall;
      }
    }
  }
}
</script><script>console.info('[GAMES] Chunk 2A: prelude');
function controlSensor(sensor, action){
  return fetch('/api/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(sensor+action)})
    .then(function(r){return r.text();}).catch(function(_){return '';});
}
async function checkSensorAvailability(){
  try{
    var sysResp=await fetch('/api/system');
    var sysData=await sysResp.json();
    i2cEnabled=(sysData.i2c_enabled===true);
    if(!i2cEnabled){
      console.warn('[GAMES] I2C is disabled - sensors unavailable');
      return;
    }
    var sensResp=await fetch('/api/sensors/status');
    var sensData=await sensResp.json();
    imuCompiled=(sensData.imuCompiled===true);
    imuEnabled=(sensData.imuEnabled===true);
    gamepadCompiled=(sensData.gamepadCompiled===true);
    gamepadEnabled=(sensData.gamepadEnabled===true);
    console.log('[GAMES] Sensor status: i2c='+i2cEnabled+' imuCompiled='+imuCompiled+' imuEnabled='+imuEnabled+' gamepadCompiled='+gamepadCompiled+' gamepadEnabled='+gamepadEnabled);
  }catch(e){
    console.error('[GAMES] Failed to check sensor availability:',e);
    i2cEnabled=false;
    imuEnabled=false;
    imuCompiled=false;
    gamepadEnabled=false;
    gamepadCompiled=false;
  }
}
</script><script>console.info('[GAMES] Chunk 2A.1: sensor control');
function startGamepadPolling(){
  if(gpPoll) return;
  if(!i2cEnabled||!gamepadCompiled){
    console.warn('[GAMES] Gamepad polling disabled: i2c='+i2cEnabled+' gamepadCompiled='+gamepadCompiled);
    return;
  }
  function tick(){
    var now=Date.now();
    if(now<gpBackoffUntil) return;
    fetch('/api/sensors?sensor=gamepad&ts='+now,{cache:'no-store',credentials:'include'})
      .then(function(r){ return r.json(); })
      .then(function(j){
        if(!running) return;
        if(j && j.v){
          var x=j.x|0, y=j.y|0;
          var nx=(x-512)/512, ny=(y-512)/512;
          if(nx<-1) nx=-1; if(nx>1) nx=1;
          if(ny<-1) ny=-1; if(ny>1) ny=1;
          gpLast={x:nx,y:ny,buttons:(j.buttons||0),valid:true};
        } else {
          gpLast.valid=false; gpBackoffUntil=Date.now()+150;
        }
      }).catch(function(_){ gpLast.valid=false; gpBackoffUntil=Date.now()+200; });
  }
  gpPoll=setInterval(tick, 16);
}
function stopGamepadPolling(){
  if(gpPoll){ try{ clearInterval(gpPoll); }catch(_){} gpPoll=null; }
  gpLast.valid=false;
}
</script><script>console.info('[GAMES] Chunk 2A.2: gamepad');
function startCalibration(){
  if(!i2cEnabled||!imuCompiled){
    var hud=document.getElementById('hudCal');
    hud.textContent='IMU unavailable: '+(i2cEnabled?'not compiled':'I2C disabled');
    hud.style.color='#ff6b6b';
    alert('IMU sensor is not available. I2C: '+i2cEnabled+', IMU compiled: '+imuCompiled);
    return;
  }
  controlSensor('imu','start').then(function(){
    var hud=document.getElementById('hudCal'); hud.textContent='Hold still... calibrating';
    hasBaseline=false; basePitch=0; baseRoll=0; calibSamples=[];
    var t0=Date.now();
    if(calibTimer) { clearInterval(calibTimer); calibTimer=null; }
    calibTimer=setInterval(function(){
      fetch('/api/sensors?sensor=imu&ts='+Date.now(),{cache:'no-store'})
        .then(function(r){return r.json();}).then(function(j){
        if(j && j.valid && j.ori){ calibSamples.push({p:j.ori.pitch||0, r:j.ori.roll||0}); }
      }).catch(function(_){});
      if(Date.now()-t0>=calibMs){
        clearInterval(calibTimer); calibTimer=null;
        if(calibSamples.length){
          var sp=0,sr=0;
          for(var i=0;i<calibSamples.length;i++){
            sp+=calibSamples[i].p; sr+=calibSamples[i].r;
          }
          basePitch=sp/calibSamples.length; baseRoll=sr/calibSamples.length; hasBaseline=true;
          hud.textContent='Calibrated: pitch '+basePitch.toFixed(1)+'°, roll '+baseRoll.toFixed(1)+'°';
        } else { hud.textContent='Calibration failed'; }
      }
    }, 50);
  });
}
function resetLevel(lv){
  level=lv||1; collisions=0; startMs=Date.now();
  document.getElementById('hudLevel').textContent=level;
  document.getElementById('hudCol').textContent=collisions;
  vel={x:0,y:0};
  var idx=(level-1); if(idx<0) idx=0; if(idx>=levels.length) idx=levels.length-1;
  var cfg = levels[idx];
  pos={x:cfg.start.x,y:cfg.start.y};
  goal={x:cfg.goal.x,y:cfg.goal.y,w:cfg.goal.w,h:cfg.goal.h};
  walls = cfg.walls.slice();
  if(terrain==='plains' || terrain==='cave'){
    var t=15; var worldW=720, worldH=480;
    walls=[{x:0,y:0,w:worldW,h:t},{x:0,y:worldH-t,w:worldW,h:t},{x:0,y:0,w:t,h:worldH},{x:worldW-t,y:0,w:t,h:worldH}];
  }
  if(terrain==='plains' || terrain==='cave'){ generateBlendedFloor(); }
  else { platforms=[]; floorMesh=null; }
  if(terrain==='cave'){ generateCaveWalls(); }
  buildGrid();
  cam.x=pos.x; cam.y=pos.y; cam.ang=0; cam.z=60; cam.pitch=0; pos.floorZ=60;
  CAM_FOLLOW=true; createWallDecorations(); spawnEnemies();
  medalGold = (cfg.medals && cfg.medals.gold)?cfg.medals.gold:10;
  medalSilver = (cfg.medals && cfg.medals.silver)?cfg.medals.silver:15;
  medalBronze = (cfg.medals && cfg.medals.bronze)?cfg.medals.bronze:22;
  timeSec=0; timeColor='#d4af37'; applyPreset();
}
function rectsOverlap(a,b){
  return !(a.x+a.w<b.x||a.x>b.x+b.w||a.y+a.h<b.y||a.y>b.y+b.h);
}
</script>
<script>console.info('[GAMES] Chunk 2A.3: collision');
</script>
<script>console.info('[GAMES] Chunk 2B: rendering prelude');
</script>
<script>console.info('[GAMES] Chunk 2B: start');
</script>
<script>console.info('[GAMES] Chunk 2B.1: before rendering functions'); void 0;
</script>
<script>console.info('[GAMES] Chunk 2B.2a: helpers');
function shadeWall(col,shade,dist){
  function clamp(v){ return v<0?0:(v>255?255:v); }
  var r=parseInt(col.substr(1,2),16), g=parseInt(col.substr(3,2),16), b=parseInt(col.substr(5,2),16);
  var fog=Math.max(0.4,1.0-0.0009*dist);
  r=Math.floor(clamp(r*shade*fog)); g=Math.floor(clamp(g*shade*fog)); b=Math.floor(clamp(b*shade*fog));
  return 'rgb('+r+','+g+','+b+')';
}
function getFloorHeightAt(x, y){
  if(!floorMesh) return 0;
  var mesh=floorMesh;
  var gridX=Math.floor(x/mesh.gridSize); var gridY=Math.floor(y/mesh.gridSize);
  if(gridX<0||gridX>=mesh.w||gridY<0||gridY>=mesh.h) return 0;
  return mesh.heights[gridY*mesh.w + gridX];
}
function getFloorColorAt(x, y){
  if(!floorMesh) return getFloorColor(0);
  var mesh=floorMesh;
  var gridX=Math.floor(x/mesh.gridSize); var gridY=Math.floor(y/mesh.gridSize);
  if(gridX<0||gridX>=mesh.w||gridY<0||gridY>=mesh.h) return getFloorColor(0);
  return mesh.colors[gridY*mesh.w + gridX];
}
console.info('[GAMES] Chunk 2B.2a: helpers ready'); void 0;

</script>
<script>console.info('[GAMES] Chunk 2B.2b: raycast start'); console.info('[GAMES] Chunk 2B.2c: raycast def');
function raycastDraw(){ 
 if(!grid){ return; }
 var w=canvas.width, h=canvas.height; var fov=cam.fov; var halfFov=fov/2;
 var s0=0, s1=Math.floor(w/2), s2=w-1; var dist0=0, dist1=0, dist2=0;
 var cameraZRaw=(cam.z||60); var cameraZ=60 + (cameraZRaw-60)*(25/40);
 var pitchOffset=Math.floor(-(cam.pitch||0)*100); var horizonY=Math.floor(h*0.5)+pitchOffset;
 ctx.save(); ctx.globalCompositeOperation='source-over';
 var ceilCol=(terrain==='ice')?'#0b0d12':(terrain==='cave')?'#1a1a1e':'#3f2f1c';
 var floorCol=(terrain==='ice')?'#1b293d':(terrain==='cave')?'#2a2a30':'#3a2f22';
 (function(){ var gy=Math.max(0,horizonY); ctx.fillStyle=ceilCol; ctx.fillRect(0,0,w, Math.max(0,horizonY)); })(); ctx.restore();
 for(var x=0;x<w;x++){
   var cameraX=2*x/w-1;
   var rayAng=cam.ang + Math.atan(cameraX*Math.tan(halfFov));
   var dirX=Math.cos(rayAng), dirY=Math.sin(rayAng);
   var skipFloorRaycast=true;
 if(!skipFloorRaycast){ var gy=Math.max(0,horizonY); var prevY=h; var d=Math.max(8, cell); var maxD=1200; var lastCol=null; while(d<=maxD){ var fx=cam.x + dirX*d; var fy=cam.y + dirY*d; var fh=(floorMesh?getFloorHeightAt(fx,fy):0); var worldZ=fh*25; var y=Math.floor(horizonY + ((cameraZ-worldZ)/d)*180); if(y<gy) y=gy; if(y>h) y=h; var col; if(floorMesh){ col=getFloorColorAt(fx,fy); } else { var fr=parseInt(floorCol.substr(1,2),16), fg=parseInt(floorCol.substr(3,2),16), fb=parseInt(floorCol.substr(5,2),16); var base=0.85 - 0.35*(d/maxD); var tint=0.04*Math.sin((fx+fy)*0.02); var k=Math.max(0.4, Math.min(1.0, base + tint)); var rr=Math.floor(fr*k), gg=Math.floor(fg*k), bb=Math.floor(fb*k); col='rgb('+rr+','+gg+','+bb+')'; } if(y<prevY){ ctx.fillStyle=col; ctx.fillRect(x, y, 1, prevY - y); prevY=y; lastCol=col; if(prevY<=gy) break; } d = Math.floor(d*1.15)+1; } if(prevY>gy){ ctx.fillStyle=(lastCol||getFloorColorAt(cam.x,cam.y)); ctx.fillRect(x, gy, 1, prevY - gy); } }
 var mapX=Math.floor(cam.x/cell), mapY=Math.floor(cam.y/cell); var rayPosX=(cam.x/cell)-mapX, rayPosY=(cam.y/cell)-mapY;
 var stepX=(dirX<0?-1:1); var stepY=(dirY<0?-1:1);
 var deltaDistX=(dirX===0)?1e9:Math.abs(1/dirX); var deltaDistY=(dirY===0)?1e9:Math.abs(1/dirY);
 var sideDistX=(dirX<0? rayPosX*deltaDistX : (1-rayPosX)*deltaDistX); var sideDistY=(dirY<0? rayPosY*deltaDistY : (1-rayPosY)*deltaDistY);
 var uncovered=[[0,h]]; var hits=0; var maxHits=15; var maxSteps=gridW+gridH+50; var firstDepthSet=false;
 while(hits<maxHits && uncovered.length>0 && maxSteps-->0){ var side=0;
   var hit=0;
   while(hit===0 && maxSteps-->0){
     if(sideDistX<sideDistY){ sideDistX+=deltaDistX; mapX+=stepX; side=0; }
     else { sideDistY+=deltaDistY; mapY+=stepY; side=1; }
     if(mapX<0||mapY<0||mapX>=gridW||mapY>=gridH){ hit=1; break; }
     if(grid[mapY*gridW+mapX]){ hit=1; }
   }
   var perpDist; var wallX;
   if(side===0){
     perpDist=(sideDistX-deltaDistX)*cell; wallX = cam.y + perpDist * dirY;
   } else {
     perpDist=(sideDistY-deltaDistY)*cell; wallX = cam.x + perpDist * dirX;
   }
   if(perpDist<1) perpDist=1; wallX -= Math.floor(wallX/cell)*cell;
 if(!firstDepthSet){ if(!depthBuffer || depthBuffer.length!==w) depthBuffer=new Array(w); depthBuffer[x]=perpDist; if(x===s0) dist0=perpDist; if(x===s1) dist1=perpDist; if(x===s2) dist2=perpDist; firstDepthSet=true; }
   var wallHeight=1.0;
   if(wallHeights && mapX>=0 && mapY>=0 && mapX<gridW && mapY<gridH){
     wallHeight=wallHeights[mapY*gridW+mapX];
   }
   var lineH=Math.floor(h/perpDist*180*wallHeight); var shade=side?0.8:1.0;
   var hx=cam.x + dirX*perpDist; var hy=cam.y + dirY*perpDist;
   var fh=(floorMesh?getFloorHeightAt(hx,hy):0); var worldZ=fh*25;
   var baseY=horizonY + Math.floor(((cameraZ-worldZ)/perpDist)*180);
   var segStart=Math.max(0, baseY - lineH); var segEnd=Math.min(h, baseY);
   if(segEnd>segStart){
 var newUncovered=[]; for(var b=0;b<uncovered.length;b++){ var b0=uncovered[b][0], b1=uncovered[b][1]; var d0=Math.max(segStart,b0), d1=Math.min(segEnd,b1); if(d1>d0){ if(USE_TEXTURES){ drawSimpleWallSlice(x, d0, d1-d0, wallX, shade, perpDist, side); } else { ctx.fillStyle=shadeWall(wallColor,shade,perpDist); ctx.fillRect(x,d0,1,d1-d0); } if(b0<d0) newUncovered.push([b0,d0]); if(d1<b1) newUncovered.push([d1,b1]); } else { newUncovered.push([b0,b1]); } } uncovered=newUncovered; }
 hits++; if(uncovered.length===0) break; if(perpDist>1000) break; }
 } drawWallDecorations(); if(DEBUG_3D){ if(Date.now()-__dbg3dLast>600){ __dbg3dLast=Date.now(); try{ var spd=Math.hypot(vel.x,vel.y).toFixed(1); console.log('[3D]', 'cam=('+cam.x.toFixed(1)+','+cam.y.toFixed(1)+') a='+cam.ang.toFixed(2), 'pos=('+pos.x.toFixed(1)+','+pos.y.toFixed(1)+') v='+spd, 'grid=('+gridW+'x'+gridH+') cell='+cell, 'd0='+dist0.toFixed(1), 'dM='+dist1.toFixed(1), 'dE='+dist2.toFixed(1), 'running='+(running?1:0)+' polling='+(polling?1:0)); }catch(_){ } } drawMiniMap(); draw3DHud(dist0,dist1,dist2); } } void 0;
</script>
<script>console.info('[GAMES] Chunk 2B.3: post basic raycast');
</script>
<script>console.info('[GAMES] Chunk 2C: hud+mini+shade');
function drawSimpleWallSlice(x, y, height, wallX, shade, dist, side){
  if(height <= 0) return;
  var baseColor = (terrain==='ice') ? [140, 170, 240] : (terrain==='cave') ? [95, 95, 105] : [180, 140, 100];
  var u = Math.floor(wallX * 4) % 16; var colorMod = 1.0;
  if(terrain==='cave'){
    var v = Math.floor(wallX * 2) % 8;
    if(v < 1) colorMod = 0.75; else if(v < 2) colorMod = 0.85;
    else if(v > 6) colorMod = 1.15; else if(v === 4) colorMod = 0.95; else colorMod = 1.0;
  } else {
    if(u < 2) colorMod = 0.9; else if(u > 13) colorMod = 0.9;
    else if(u === 7 || u === 8) colorMod = 0.95;
  }
  var fog = Math.max(0.3, 1.0 - dist * 0.0008);
  var r = Math.floor(baseColor[0] * colorMod * shade * fog);
  var g = Math.floor(baseColor[1] * colorMod * shade * fog);
  var b = Math.floor(baseColor[2] * colorMod * shade * fog);
  r = Math.max(0, Math.min(255, r)); g = Math.max(0, Math.min(255, g)); b = Math.max(0, Math.min(255, b));
  var gradient = ctx.createLinearGradient(0, y, 0, y + height);
  var topShade = 1.12, bottomShade = 0.88;
  gradient.addColorStop(0, 'rgb(' + Math.floor(r*topShade) + ',' + Math.floor(g*topShade) + ',' + Math.floor(b*topShade) + ')');
  gradient.addColorStop(0.5, 'rgb(' + r + ',' + g + ',' + b + ')');
  gradient.addColorStop(1, 'rgb(' + Math.floor(r*bottomShade) + ',' + Math.floor(g*bottomShade) + ',' + Math.floor(b*bottomShade) + ')');
  ctx.fillStyle = gradient; ctx.fillRect(x, y, 1, height);
}
function findWallSurfaces(){
  var surfaces = [];
  for(var y = 0; y < gridH; y++){
    for(var x = 0; x < gridW; x++){
      if(grid[y * gridW + x]){
        var hasNorth = (y > 0 && !grid[(y-1) * gridW + x]);
        var hasSouth = (y < gridH-1 && !grid[(y+1) * gridW + x]);
        var hasWest = (x > 0 && !grid[y * gridW + (x-1)]);
        var hasEast = (x < gridW-1 && !grid[y * gridW + (x+1)]);
        if(hasNorth) surfaces.push({x: x, y: y, side: 'north', worldX: x * cell + cell/2, worldY: y * cell});
        if(hasSouth) surfaces.push({x: x, y: y, side: 'south', worldX: x * cell + cell/2, worldY: (y+1) * cell});
        if(hasWest) surfaces.push({x: x, y: y, side: 'west', worldX: x * cell, worldY: y * cell + cell/2});
        if(hasEast) surfaces.push({x: x, y: y, side: 'east', worldX: (x+1) * cell, worldY: y * cell + cell/2});
      }
    }
  }
  return surfaces;
}
function createWallDecorations(){ var surfaces = findWallSurfaces(); wallDecorations = []; Math.seedrandom = function(seed){ var m = 0x80000000; var a = 1103515245; var c = 12345; var state = seed ? seed : Math.floor(Math.random() * (m - 1)); return function(){ state = (a * state + c) % m; return state / (m - 1); }; }; var rng = Math.seedrandom(seedFor(3)); var numDecorations = Math.min(8, Math.floor(surfaces.length * 0.15)); for(var i = 0; i < numDecorations; i++){ if(surfaces.length === 0) break; var idx = Math.floor(rng() * surfaces.length); var surface = surfaces[idx]; surfaces.splice(idx, 1); var types = ['torch', 'shield', 'banner', 'sconce']; var type = types[Math.floor(rng() * types.length)]; var offset = (rng() - 0.5) * cell * 0.4; var decorX = surface.worldX; var decorY = surface.worldY; if(surface.side === 'north' || surface.side === 'south'){ decorX += offset; } else { decorY += offset; } wallDecorations.push({worldX: decorX, worldY: decorY, side: surface.side, type: type, gridX: surface.x, gridY: surface.y}); } console.log('[DECOR] Created ' + wallDecorations.length + ' seeded decorations on actual walls'); }
function isDecorationOccluded(decorX, decorY, camX, camY){
  var dx = decorX - camX; var dy = decorY - camY;
  var dist = Math.hypot(dx, dy);
  if(dist < 2) return false;
  var steps = Math.floor(dist / 3); if(steps < 2) steps = 2;
  for(var i = 1; i < steps; i++){
    var t = i / steps;
    var testX = camX + dx * t; var testY = camY + dy * t;
    var gridX = Math.floor(testX / cell); var gridY = Math.floor(testY / cell);
    if(gridX >= 0 && gridX < gridW && gridY >= 0 && gridY < gridH){
      if(grid[gridY * gridW + gridX]){ return true; }
    }
  }
  return false;
}
var __decorDebugLast = 0; function drawWallDecorations(){ if(!wallDecorations || wallDecorations.length === 0) return; var w = canvas.width, h = canvas.height; var halfFov = cam.fov / 2; var visibleCount = 0; var occludedCount = 0; var culledCount = 0; var now = Date.now(); var shouldLog = DEBUG_DECORATIONS && (now - __decorDebugLast > 1000); for(var i = 0; i < wallDecorations.length; i++){ var dec = wallDecorations[i]; var dx = dec.worldX - cam.x; var dy = dec.worldY - cam.y; var dist = Math.hypot(dx, dy); if(dist < 1 || dist > 400) continue; var ang = Math.atan2(dy, dx); var da = ang - cam.ang; while(da > Math.PI) da -= Math.PI * 2; while(da < -Math.PI) da += Math.PI * 2; if(Math.abs(da) > halfFov * 1.5) { culledCount++; continue; } var sx = (da / halfFov) * 0.5 + 0.5; var screenX = Math.floor(sx * w); if(screenX < -40 || screenX > w + 40) { culledCount++; continue; } var isOccluded = isDecorationOccluded(dec.worldX, dec.worldY, cam.x, cam.y); if(isOccluded){ occludedCount++; } else { visibleCount++; var renderX = Math.max(0, Math.min(w - 1, screenX)); var wallNormalX = 0, wallNormalY = 0; if(dec.side === 'north') wallNormalY = -1; else if(dec.side === 'south') wallNormalY = 1; else if(dec.side === 'west') wallNormalX = -1; else if(dec.side === 'east') wallNormalX = 1; var toCamX = cam.x - dec.worldX; var toCamY = cam.y - dec.worldY; var toCamLen = Math.hypot(toCamX, toCamY); if(toCamLen > 0){ toCamX /= toCamLen; toCamY /= toCamLen; } var viewAngle = Math.abs(toCamX * wallNormalX + toCamY * wallNormalY); var scale = Math.min(4.0, 200 / dist); var size = Math.max(8, Math.floor(32 * scale)); var pitchOffset=Math.floor(-(cam.pitch||0)*100); var decorY = Math.floor(h * 0.45) + pitchOffset; drawWallAlignedDecoration(dec.type, renderX, decorY, size, dist, dec.side, viewAngle); } } if(shouldLog){ console.log('[DECOR] Summary: ' + visibleCount + ' visible, ' + occludedCount + ' occluded, ' + culledCount + ' culled'); __decorDebugLast = now; } }
function drawWallAlignedDecoration(type, x, y, size, dist, side, viewAngle){
  ctx.save();
  var brightness = Math.max(0.4, viewAngle); ctx.globalAlpha = Math.max(0.6, brightness);
  var widthScale = 1.0; var heightScale = 1.0;
  var distanceFactor = Math.max(0.0, Math.min(1.0, (120 - dist) / 80));
  var angleEffect = distanceFactor * (1.0 - viewAngle);
  if(side === 'north' || side === 'south'){ widthScale = 1.0 - angleEffect * 0.6; }
  else { heightScale = 1.0 - angleEffect * 0.5; }
  var w = Math.floor(size * widthScale); var h = Math.floor(size * heightScale);
  if(type === 'torch'){
    var stickColor = 'rgb(' + Math.floor(139*brightness) + ',' + Math.floor(69*brightness) + ',' + Math.floor(19*brightness) + ')';
    ctx.fillStyle = stickColor; ctx.fillRect(x - w/6, y, w/3, h);
    ctx.fillStyle = '#FF4500'; ctx.globalAlpha *= 0.9; ctx.beginPath();
    ctx.ellipse(x, y - h/4, w/3, h/2, 0, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#FFD700'; ctx.beginPath();
    ctx.ellipse(x, y - h/3, w/5, h/4, 0, 0, Math.PI * 2); ctx.fill();
  } else if(type === 'shield'){
    var metalColor = 'rgb(' + Math.floor(192*brightness) + ',' + Math.floor(192*brightness) + ',' + Math.floor(192*brightness) + ')';
    ctx.fillStyle = metalColor; ctx.beginPath();
    ctx.ellipse(x, y, w/2, h*0.6, 0, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#8B0000'; ctx.globalAlpha *= 0.8; ctx.beginPath();
    ctx.moveTo(x, y - h/3); ctx.lineTo(x - w/4, y); ctx.lineTo(x, y + h/3); ctx.lineTo(x + w/4, y);
    ctx.closePath(); ctx.fill();
  } else if(type === 'banner'){
    var poleColor = 'rgb(' + Math.floor(139*brightness) + ',' + Math.floor(69*brightness) + ',' + Math.floor(19*brightness) + ')';
    ctx.fillStyle = poleColor; ctx.fillRect(x - w/8, y - h/2, w/4, h);
    var fabricColor = 'rgb(' + Math.floor(128*brightness) + ',0,' + Math.floor(128*brightness) + ')';
    ctx.fillStyle = fabricColor; ctx.fillRect(x, y - h/2, w/2, h*0.7);
    ctx.fillStyle = '#FFD700'; ctx.globalAlpha *= 0.9;
    ctx.fillRect(x + w/8, y - h/3, w/4, h/6);
  } else if(type === 'sconce'){
    var baseColor = 'rgb(' + Math.floor(105*brightness) + ',' + Math.floor(105*brightness) + ',' + Math.floor(105*brightness) + ')';
    ctx.fillStyle = baseColor; ctx.beginPath(); ctx.arc(x, y, w/3, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#FF6347'; ctx.globalAlpha *= 0.8; ctx.beginPath();
    ctx.ellipse(x, y - h/6, w/4, h/3, 0, 0, Math.PI * 2); ctx.fill();
  }
  ctx.restore();
}
function drawDecoration(type, x, y, size, dist){ ctx.save(); if(type === 'torch'){ ctx.fillStyle = '#8B4513'; ctx.fillRect(x - size/6, y, size/3, size); ctx.fillStyle = '#FF4500'; ctx.beginPath(); ctx.ellipse(x, y - size/4, size/3, size/2, 0, 0, Math.PI * 2); ctx.fill(); ctx.fillStyle = '#FFD700'; ctx.beginPath(); ctx.ellipse(x, y - size/3, size/5, size/4, 0, 0, Math.PI * 2); ctx.fill(); } else if(type === 'shield'){ ctx.fillStyle = '#C0C0C0'; ctx.beginPath(); ctx.ellipse(x, y, size/2, size*0.6, 0, 0, Math.PI * 2); ctx.fill(); ctx.fillStyle = '#8B0000'; ctx.beginPath(); ctx.moveTo(x, y - size/3); ctx.lineTo(x - size/4, y); ctx.lineTo(x, y + size/3); ctx.lineTo(x + size/4, y); ctx.closePath(); ctx.fill(); } else if(type === 'banner'){ ctx.fillStyle = '#8B4513'; ctx.fillRect(x - size/8, y - size/2, size/4, size); ctx.fillStyle = '#800080'; ctx.fillRect(x, y - size/2, size/2, size*0.7); ctx.fillStyle = '#FFD700'; ctx.fillRect(x + size/8, y - size/3, size/4, size/6); } else if(type === 'sconce'){ ctx.fillStyle = '#696969'; ctx.beginPath(); ctx.arc(x, y, size/3, 0, Math.PI * 2); ctx.fill(); ctx.fillStyle = '#FF6347'; ctx.beginPath(); ctx.ellipse(x, y - size/6, size/4, size/3, 0, 0, Math.PI * 2); ctx.fill(); } ctx.restore(); }
var PLAINS_COL_N='#ff6666', PLAINS_COL_S='#66ff66', PLAINS_COL_W='#6699ff', PLAINS_COL_E='#ffe066';
</script>
<script>console.info('[GAMES] Chunk 3: gameplay + listeners');
function drawGoalMarker3D(){
  var w=canvas.width, h=canvas.height; var fov=cam.fov;
  var gx=goal.x+goal.w/2, gy=goal.y+goal.h/2;
  var dx=gx-cam.x, dy=gy-cam.y; var dist=Math.hypot(dx,dy);
  if(dist<1) dist=1;
  var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
  while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
  var halfFov=fov/2; if(Math.abs(da)>halfFov*1.1){ return; }
  var sx = (da/halfFov)*0.5 + 0.5; var screenX = Math.floor(sx * w);
  if(depthBuffer && screenX >= 0 && screenX < depthBuffer.length && dist > depthBuffer[screenX] + 3) return;
  var pitchOffset=Math.floor(-(cam.pitch||0)*100);
  var floorY = Math.floor(h*0.75)+pitchOffset;
  var size = Math.max(6, Math.min(28, Math.floor(240/(dist))));
  ctx.save(); ctx.globalAlpha=0.95; ctx.fillStyle='#00ff00';
  ctx.strokeStyle='rgba(0,255,0,0.7)'; ctx.lineWidth=2;
  ctx.fillRect(screenX - size/2, floorY - Math.floor(size/4), size, Math.floor(size/3));
  ctx.beginPath(); ctx.moveTo(screenX, floorY - Math.floor(size/4));
  ctx.lineTo(screenX, floorY - Math.floor(size/4) - Math.min(40, Math.floor(280/dist)));
  ctx.stroke(); ctx.restore();
}
function generateBlendedFloor(){ platforms=[]; var w=720, h=480; var margin=Math.min(30, w*0.08); var availableW=w-2*margin; var availableH=h-2*margin; Math.seedrandom = function(seed){ var m = 0x80000000; var a = 1103515245; var c = 12345; var state = seed ? seed : Math.floor(Math.random() * (m - 1)); return function(){ state = (a * state + c) % m; return state / (m - 1); }; }; var rng = Math.seedrandom(54321); var targetPlatforms=Math.max(8, Math.min(20, Math.floor(availableW*availableH/5000))); console.log('[FLOOR] Starting blended floor generation: target=' + targetPlatforms + ', canvas=' + w + 'x' + h); var generated=0; for(var i=0;i<targetPlatforms;i++){ var attempts=0; var placed=false; while(!placed && attempts<30){ var maxW=Math.min(100, availableW*0.35); var maxH=Math.min(80, availableH*0.35); var pw=Math.floor(30 + rng()*maxW); var ph=Math.floor(20 + rng()*maxH); var px=Math.floor(margin + rng()*(availableW-pw)); var py=Math.floor(margin + rng()*(availableH-ph)); var heightPercent=(rng()-0.5)*2.0; var newPlat={x:px,y:py,w:pw,h:ph,heightPercent:heightPercent}; var overlaps=false; for(var j=0;j<platforms.length;j++){ var existing=platforms[j]; var buffer=Math.max(5, Math.min(12, (pw+ph)/10)); if(!(newPlat.x+newPlat.w+buffer<existing.x||newPlat.x>existing.x+existing.w+buffer||newPlat.y+newPlat.h+buffer<existing.y||newPlat.y>existing.y+existing.h+buffer)){ overlaps=true; break; } } if(!overlaps){ platforms.push(newPlat); placed=true; generated++; } attempts++; } } console.log('[FLOOR] Pre-generating blended mesh...'); generateFloorMesh(); console.log('[FLOOR] Blended floor complete: ' + generated + ' platforms, mesh ready for runtime'); }
function generateFloorMesh(){ var w=720, h=480; var gridSize=6; var meshW=Math.ceil(w/gridSize); var meshH=Math.ceil(h/gridSize); floorMesh={w:meshW, h:meshH, gridSize:gridSize, heights:[], colors:[]}; for(var y=0;y<meshH;y++){ for(var x=0;x<meshW;x++){ var worldX=x*gridSize; var worldY=y*gridSize; var height=0; var totalWeight=0; for(var i=0;i<platforms.length;i++){ var p=platforms[i]; var dx=Math.max(0, Math.max(p.x-worldX, worldX-(p.x+p.w))); var dy=Math.max(0, Math.max(p.y-worldY, worldY-(p.y+p.h))); var dist=Math.sqrt(dx*dx + dy*dy); var influence=Math.max(0, 1.0 - dist/70); if(influence>0){ height += p.heightPercent * influence; totalWeight += influence; } } if(totalWeight>0){ height /= totalWeight; } else { height=(Math.random()-0.5)*0.15; } floorMesh.heights[y*meshW + x] = height; floorMesh.colors[y*meshW + x] = getFloorColor(height); } } console.log('[FLOOR] Generated ' + (meshW*meshH) + ' mesh points (' + meshW + 'x' + meshH + ')'); }
function drawSkybox3D(){
  var w=canvas.width, h=canvas.height;
  var pitchOffset=Math.floor(-(cam.pitch||0)*100);
  var horizonY=Math.floor(h*0.5)+pitchOffset;
  ctx.save(); var gradient=ctx.createLinearGradient(0,0,0,horizonY);
  if(terrain==='cave'){
    gradient.addColorStop(0, '#2a2a30'); gradient.addColorStop(0.6, '#3a3a40'); gradient.addColorStop(1, '#4a4a50');
  } else {
    gradient.addColorStop(0, '#87ceeb'); gradient.addColorStop(0.6, '#b0d9f0'); gradient.addColorStop(1, '#d4e8f5');
  }
  ctx.fillStyle=gradient; ctx.fillRect(0,0,w,horizonY); ctx.restore();
}
function getFloorColor(heightPercent){
  if(terrain==='cave'){
    if(heightPercent<-0.3) return '#5a5a60'; if(heightPercent<-0.1) return '#6a6a70';
    if(heightPercent<0.1) return '#7a7a80'; if(heightPercent<0.3) return '#8a8a90';
    if(heightPercent<0.5) return '#9a9aa0'; return '#aaaab0';
  } else {
    if(heightPercent<-0.3) return '#3a5a45'; if(heightPercent<-0.1) return '#4a6a55';
    if(heightPercent<0.1) return '#5a8a69'; if(heightPercent<0.3) return '#6a9a79';
    if(heightPercent<0.5) return '#7aaa89'; return '#8aba99';
  }
}
function generateBlendedFloor(){ platforms=[]; var w=720, h=480; var margin=Math.min(30, w*0.08); var availableW=w-2*margin; var availableH=h-2*margin;
 Math.seedrandom = function(seed){ var m = 0x80000000; var a = 1103515245; var c = 12345; var state = seed ? seed : Math.floor(Math.random() * (m - 1)); return function(){ state = (a * state + c) % m; return state / (m - 1); }; };
 var rng = Math.seedrandom(seedFor(0)); var targetPlatforms=Math.max(8, Math.min(20, Math.floor(availableW*availableH/5000)));
 console.log('[FLOOR] Starting blended floor generation: target=' + targetPlatforms + ', canvas=' + w + 'x' + h); var generated=0;
 for(var i=0;i<targetPlatforms;i++){ var attempts=0; var placed=false; while(!placed && attempts<30){ var maxW=Math.min(100, availableW*0.35); var maxH=Math.min(80, availableH*0.35);
 var pw=Math.floor(30 + rng()*maxW); var ph=Math.floor(20 + rng()*maxH); var px=Math.floor(margin + rng()*(availableW-pw)); var py=Math.floor(margin + rng()*(availableH-ph));
 var heightPercent=(rng()-0.5)*2.0; var newPlat={x:px,y:py,w:pw,h:ph,heightPercent:heightPercent}; var overlaps=false;
 for(var j=0;j<platforms.length;j++){ var existing=platforms[j]; var buffer=Math.max(5, Math.min(12, (pw+ph)/10));
 if(!(newPlat.x+newPlat.w+buffer<existing.x||newPlat.x>existing.x+existing.w+buffer||newPlat.y+newPlat.h+buffer<existing.y||newPlat.y>existing.y+existing.h+buffer)){ overlaps=true; break; } }
 if(!overlaps){ platforms.push(newPlat); placed=true; generated++; } attempts++; } }
 console.log('[FLOOR] Pre-generating blended mesh...'); generateFloorMesh(); console.log('[FLOOR] Blended floor complete: ' + generated + ' platforms, mesh ready for runtime'); }
function drawPlatforms2D(){
  if(!floorMesh) return;
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  var mesh=floorMesh; var gridSize=mesh.gridSize;
  for(var y=0;y<mesh.h-1;y++){
    for(var x=0;x<mesh.w-1;x++){
      var height=mesh.heights[y*mesh.w + x];
      var worldX=x*gridSize; var worldY=y*gridSize;
      ctx.fillStyle=mesh.colors[y*mesh.w + x];
      ctx.globalAlpha=0.4 + height*0.5;
      ctx.fillRect(worldX, worldY, gridSize, gridSize);
    }
  }
  ctx.restore();
}
function avgColor(c1,c2,c3,c4){
  var r1=parseInt(c1.substr(1,2),16), g1=parseInt(c1.substr(3,2),16), b1=parseInt(c1.substr(5,2),16);
  var r2=parseInt(c2.substr(1,2),16), g2=parseInt(c2.substr(3,2),16), b2=parseInt(c2.substr(5,2),16);
  var r3=parseInt(c3.substr(1,2),16), g3=parseInt(c3.substr(3,2),16), b3=parseInt(c3.substr(5,2),16);
  var r4=parseInt(c4.substr(1,2),16), g4=parseInt(c4.substr(3,2),16), b4=parseInt(c4.substr(5,2),16);
  var r=Math.floor((r1+r2+r3+r4)/4), g=Math.floor((g1+g2+g3+g4)/4), b=Math.floor((b1+b2+b3+b4)/4);
  return '#'+('0'+r.toString(16)).slice(-2)+('0'+g.toString(16)).slice(-2)+('0'+b.toString(16)).slice(-2);
}
function drawPlatforms3D(){
  if(!floorMesh) return;
  var w=canvas.width, h=canvas.height; var halfFov=cam.fov/2;
  var pitchOffset=Math.floor(-(cam.pitch||0)*100); var horizonY=Math.floor(h*0.5)+pitchOffset;
  var mesh=floorMesh; var cameraZRaw=(cam.z||60); var cameraZ=60 + (cameraZRaw-60)*(25/40);
  ctx.save(); var visibleQuads=[]; var totalQuads=0, distCulled=0, fovCulled=0, rendered=0, cornerCulled=0;
  var minH=999, maxH=-999;
  for(var y=0;y<mesh.h-1;y++){
    for(var x=0;x<mesh.w-1;x++){
      var h1=mesh.heights[y*mesh.w + x]; var h2=mesh.heights[y*mesh.w + (x+1)];
      var h3=mesh.heights[(y+1)*mesh.w + x]; var h4=mesh.heights[(y+1)*mesh.w + (x+1)];
      var worldX1=x*mesh.gridSize, worldY1=y*mesh.gridSize;
      var worldX2=(x+1)*mesh.gridSize, worldY2=(y+1)*mesh.gridSize;
      var centerX=(worldX1+worldX2)/2, centerY=(worldY1+worldY2)/2;
      var dx=centerX-cam.x, dy=centerY-cam.y; var dist=Math.hypot(dx,dy); totalQuads++;
      if(dist>800){ distCulled++; continue; }
      var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
      while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
      if(Math.abs(da)>halfFov*1.8){ fovCulled++; continue; }
      var avgHeight=(h1+h2+h3+h4)/4;
      if(dist<100){ minH=Math.min(minH,h1,h2,h3,h4); maxH=Math.max(maxH,h1,h2,h3,h4); }
      var c1=mesh.colors[y*mesh.w + x]; var c2=mesh.colors[y*mesh.w + (x+1)];
      var c3=mesh.colors[(y+1)*mesh.w + x]; var c4=mesh.colors[(y+1)*mesh.w + (x+1)];
      visibleQuads.push({x1:worldX1,y1:worldY1,x2:worldX2,y2:worldY2,h1:h1,h2:h2,h3:h3,h4:h4,avgHeight:avgHeight,dist:dist,centerX:centerX,centerY:centerY,gridX:x,gridY:y,c1:c1,c2:c2,c3:c3,c4:c4});
    }
  }
  visibleQuads.sort(function(a,b){return b.dist-a.dist;});
  for(var i=0;i<visibleQuads.length;i++){
    var quad=visibleQuads[i];
    var corners=[{x:quad.x1,y:quad.y1,h:quad.h1},{x:quad.x2,y:quad.y1,h:quad.h2},{x:quad.x2,y:quad.y2,h:quad.h4},{x:quad.x1,y:quad.y2,h:quad.h3}];
    var screenPts=[]; var allVisible=true;
    for(var j=0;j<4;j++){
      var dx=corners[j].x-cam.x, dy=corners[j].y-cam.y; var dist=Math.hypot(dx,dy);
      if(dist<1) dist=1; var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
      while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
      if(Math.abs(da)>halfFov*2.5){ allVisible=false; cornerCulled++; break; }
      var sx=(da/halfFov)*0.5 + 0.5; var screenX=Math.floor(sx*w);
      var worldZ=corners[j].h*25; var heightAboveFloor=cameraZ-worldZ;
      var screenY=horizonY + Math.floor((heightAboveFloor/dist)*180);
      screenPts.push({x:screenX,y:screenY});
    }
    if(!allVisible){ continue; }
    rendered++; ctx.fillStyle=avgColor(quad.c1,quad.c2,quad.c3,quad.c4); ctx.globalAlpha=1.0;
    ctx.beginPath(); ctx.moveTo(screenPts[0].x, screenPts[0].y);
    for(var k=1;k<screenPts.length;k++){ ctx.lineTo(screenPts[k].x, screenPts[k].y); }
    ctx.closePath(); ctx.fill();
  }
  ctx.restore();
  if(DEBUG_TEXTURE && Date.now()-__dbgTexLast>1000){
    __dbgTexLast=Date.now();
    console.log('[FLOOR-3D]', 'total='+totalQuads, 'distCull='+distCulled, 'fovCull='+fovCulled, 'cornerCull='+cornerCulled, 'rendered='+rendered);
  }
  if(DEBUG_FLOOR && Date.now()-__dbgFloorLast>500){
    __dbgFloorLast=Date.now(); var camFloorH=getFloorHeightAt(cam.x,cam.y);
    console.log('[FLOOR-HEIGHTS]', 'cam=('+cam.x.toFixed(1)+','+cam.y.toFixed(1)+')', 'camFloorH='+camFloorH.toFixed(3), 'cameraZ='+cameraZ.toFixed(1), 'nearbyRange=['+minH.toFixed(3)+' to '+maxH.toFixed(3)+']', 'delta='+(maxH-minH).toFixed(3), 'scale=25');
  }
}
function drawCalibration(){
  ctx.clearRect(0,0,canvas.width,canvas.height);
  ctx.fillStyle='#333'; ctx.fillRect(0,0,canvas.width,canvas.height);
  ctx.fillStyle='#fff'; ctx.font='16px Arial'; ctx.textAlign='center';
  ctx.fillText('Calibrating...', canvas.width/2, canvas.height/2 - 20);
  ctx.fillText('Hold device flat and still', canvas.width/2, canvas.height/2 + 10);
  var pct=Math.min(100, Math.floor((calStableMs/calNeedStableMs)*100));
  ctx.fillStyle='#4db6ff';
  ctx.fillRect(canvas.width/2 - 100, canvas.height/2 + 30, pct*2, 10);
  ctx.strokeStyle='#fff';
  ctx.strokeRect(canvas.width/2 - 100, canvas.height/2 + 30, 200, 10);
}
function draw2D(){ updateViewCamera(); ctx.clearRect(0,0,canvas.width,canvas.height); if(bgPattern){ ctx.save(); ctx.fillStyle=bgPattern; ctx.fillRect(0,0,canvas.width,canvas.height); ctx.restore(); } ctx.save(); ctx.translate(-viewCam.x, -viewCam.y); for(var i=0;i<walls.length;i++){ var w=walls[i]; ctx.fillStyle=wallColor; ctx.fillRect(w.x, w.y, w.w, w.h); } ctx.fillStyle='#00ff00'; ctx.fillRect(goal.x, goal.y, goal.w, goal.h); ctx.fillStyle='#ff0000'; ctx.beginPath(); ctx.arc(pos.x, pos.y, 6, 0, Math.PI*2); ctx.fill(); var now=Date.now(); for(var j=0;j<enemies.length;j++){ var e=enemies[j]; var eType=e.enemyType; var r=8*eType.size; var isFlashing=(e.damageFlash && now<e.damageFlash); ctx.fillStyle=isFlashing?'#ffffff':eType.color; ctx.beginPath(); ctx.arc(e.x, e.y, r, 0, Math.PI*2); ctx.fill(); if(e.slowUntil && now<e.slowUntil){ ctx.strokeStyle='#00ffff'; ctx.lineWidth=2; ctx.beginPath(); ctx.arc(e.x, e.y, r+2, 0, Math.PI*2); ctx.stroke(); } if(e.burnUntil && now<e.burnUntil){ ctx.fillStyle='rgba(255,100,0,0.6)'; ctx.beginPath(); ctx.arc(e.x, e.y-r*0.5, r*0.4, 0, Math.PI*2); ctx.fill(); } var healthPct=e.health/e.maxHealth; ctx.fillStyle=(healthPct>0.6)?'#00ff00':(healthPct>0.3)?'#ffaa00':'#ff4444'; ctx.fillRect(e.x-10, e.y-15-r, 20*healthPct, 3); ctx.strokeStyle='#fff'; ctx.strokeRect(e.x-10, e.y-15-r, 20, 3); } for(var d=0;d<deathEffects.length;d++){ var de=deathEffects[d]; var t=Math.max(0, Math.min(1, (now-de.spawnMs)/de.lifeMs)); var r=5 + 25*t; ctx.globalAlpha=1.0 - t; ctx.fillStyle=de.color; for(var p=0;p<8;p++){ var pang=(p/8)*Math.PI*2; var px=de.x+Math.cos(pang)*r*0.8; var py=de.y+Math.sin(pang)*r*0.8; ctx.beginPath(); ctx.arc(px, py, 3, 0, Math.PI*2); ctx.fill(); } ctx.globalAlpha=1.0; } ctx.restore(); ctx.fillStyle='#fff'; ctx.font='14px monospace'; ctx.fillText('Time: ' + timeSec.toFixed(1) + 's', 10, 20); ctx.fillStyle=timeColor; ctx.fillText('●', 90, 20); }
function draw(){ if(calibrating){ drawCalibration(); return; } if(MODE3D){ var now=Date.now(); if(CONTROL_MODE===MODE_GYRO_AIM && USE_YAW && hasYaw){ var effTarget = yawTarget + yawOffset; if(capturingYaw){ /* freeze camera */ if(!isFinite(camHold)) camHold=cam.ang; cam.ang = camHold; if(DEBUG_CAM && now-__camDbgLast>300){ __camDbgLast=now; try{ console.log('[CAM]', 'capturing=1', 'camHold=', camHold.toFixed(3), 'yawTarget=', yawTarget.toFixed?yawTarget.toFixed(3):yawTarget, 'yawOffset=', yawOffset.toFixed?yawOffset.toFixed(3):yawOffset); }catch(_){ } } } else { var d=angNorm(effTarget - cam.ang); var alpha = (now < yawResumeUntil) ? Math.min(0.5, yawAlpha*2.0) : yawAlpha; cam.ang += d*alpha; camHold = cam.ang; if(DEBUG_CAM && now-__camDbgLast>300){ __camDbgLast=now; try{ console.log('[CAM]', 'capturing=0', 'effTarget=', effTarget.toFixed?effTarget.toFixed(3):effTarget, 'cam=', cam.ang.toFixed(3), 'd=', d.toFixed?d.toFixed(3):d, 'alpha=', alpha.toFixed?alpha.toFixed(3):alpha); }catch(_){ } } } } if(CONTROL_MODE===MODE_GYRO_AIM && USE_PITCH){ var pitchDiff = pitchTarget - cam.pitch; cam.pitch += pitchDiff * pitchAlpha; } if(CAM_FOLLOW){ cam.x=pos.x; cam.y=pos.y; if(pos.floorZ) cam.z=pos.floorZ; } ctx.clearRect(0,0,canvas.width,canvas.height); drawSkybox3D(); drawPlatforms3D(); raycastDraw(); drawGoalMarker3D(); } else { if(CONTROL_MODE===MODE_GYRO_AIM && USE_YAW && hasYaw){ var d2=angNorm(yawTarget - cam.ang); cam.ang += d2*yawAlpha; } draw2D(); drawPlatforms2D(); } }
function clamp(v,min,max){ return v<min?min:(v>max?max:v); }
function deg2rad(d){ return d*Math.PI/180; }
function angNorm(a){ while(a>Math.PI) a-=Math.PI*2; while(a<-Math.PI) a+=Math.PI*2; return a; }
function updateViewCamera(){
  var targetX=pos.x-canvas.width/2; var targetY=pos.y-canvas.height/2;
  var dx=targetX-viewCam.x; var dy=targetY-viewCam.y;
  viewCam.x += dx*0.1; viewCam.y += dy*0.1;
}
function getCurrentSpell(){ return spells[spellOrder[currentSpellIdx]]; };
function cycleSpell(){ currentSpellIdx = (currentSpellIdx + 1) % spellOrder.length; console.log('[SPELL] Cycled to: ' + getCurrentSpell().name); };
function spawnProjectile(speedOverride, radiusOverride, spellOverride){ 

  var spell = spellOverride || getCurrentSpell(); 

  var ang=cam.ang||0; 

  // In 2D mode: Use instant joystick direction for snappy aiming

  if(!MODE3D && gpLast && gpLast.valid){ 

    var jx=gpLast.x, jy=-gpLast.y; 

    var jmag=Math.hypot(jx,jy); 

    if(jmag>0.2){ ang=Math.atan2(jy,jx); } 

  } 

  // In 3D mode: Use instant joystick for Stick Aim, or yaw target for Gyro Aim

  else if(MODE3D){ 

    if(CONTROL_MODE===MODE_STICK_AIM && gpLast && gpLast.valid){ 

      var jx2=gpLast.x; 

      var jmag2=Math.abs(jx2); 

      if(jmag2>0.15){ ang=cam.ang + jx2*0.3; } 

    } else if(CONTROL_MODE===MODE_GYRO_AIM && hasYaw){ 

      ang = yawTarget + yawOffset; 

    } 

  } 

  var rawPitch=(CONTROL_MODE===MODE_STICK_AIM)?(cam.pitch||0):(pitchTarget||0); var usePitch=-(rawPitch); var sx=pos.x+Math.cos(ang)*10; var sy=pos.y+Math.sin(ang)*10; var sp=(speedOverride||spell.speed); var rr=(radiusOverride||PROJ_RADIUS); var hz=sp*Math.cos(usePitch); var vz=sp*Math.sin(usePitch); var spawnZ=MODE3D?35:0; try{ console.log('[SHOT]', 'spell=', spell.name, 'ang(rad)=', ang.toFixed?ang.toFixed(3):ang, 'vz=', vz.toFixed?vz.toFixed(1):vz); }catch(_){ } projectiles.push({x:sx,y:sy,z:spawnZ,ang:ang,speed:sp,hz:hz,vz:vz,spawnMs:Date.now(),lifeMs:PROJ_LIFE_MS,r:rr,spell:spell}); 

};
var impacts=[];

var coneEffects=[];

function castConeAttack(spell){ var now=Date.now(); var ang=cam.ang||0; 
  if(!MODE3D && gpLast && gpLast.valid){ var jx=gpLast.x, jy=-gpLast.y; var jmag=Math.hypot(jx,jy); if(jmag>0.2){ ang=Math.atan2(jy,jx); } } 
  else if(MODE3D){ 
    if(CONTROL_MODE===MODE_STICK_AIM && gpLast && gpLast.valid){ var jx2=gpLast.x; var jmag2=Math.abs(jx2); if(jmag2>0.15){ ang=cam.ang + jx2*0.3; } } 
    else if(CONTROL_MODE===MODE_GYRO_AIM && hasYaw){ ang = yawTarget + yawOffset; } 
  } 
  var halfAngle=spell.coneAngle/2; var range=spell.coneRange||100; var hitCount=0; 
  coneEffects.push({x:pos.x,y:pos.y,z:MODE3D?35:0,ang:ang,halfAngle:halfAngle,range:range,color:spell.color,spellId:spell.id,spawnMs:now,lifeMs:200}); 
  if(enemies&&enemies.length){
    for(var ei=0;ei<enemies.length;ei++){
      var e=enemies[ei];
      var dx=e.x-pos.x, dy=e.y-pos.y; var dist=Math.hypot(dx,dy);
      if(dist>range || dist<1) continue;
      var angleToEnemy=Math.atan2(dy,dx); var angleDiff=angleToEnemy - ang;
      while(angleDiff>Math.PI) angleDiff-=Math.PI*2;
      while(angleDiff<-Math.PI) angleDiff+=Math.PI*2;
      if(Math.abs(angleDiff)<=halfAngle){
        var dmg=spell.damage||1; e.health-=dmg; stats.totalDamageDone+=dmg;
        if(e.health<=0){ stats.totalEnemiesKilled++; }
        e.damageFlash=now+120; hitCount++;
        if(spell.id==='ice'){
          e.iceHits=(e.iceHits||0)+1;
          if(e.iceHits>=3){ e.slowUntil=now+2500; e.iceHits=0; }
        } else if(spell.id==='fire'){
          e.fireHits=(e.fireHits||0)+1;
          if(e.fireHits>=3){ e.burnUntil=now+4000; e.burnDmgLast=now; e.fireHits=0; }
        }
      }
    }
  }
  if(DEBUG_EFFECTS){ try{ console.log('[CONE]','spell='+spell.id,'hits='+hitCount,'activeCones='+coneEffects.length); }catch(_){} }
}
function updateConeEffects(){ if(!coneEffects||!coneEffects.length) return; var now=Date.now(); var keep=[]; var removed=0; 
  for(var i=0;i<coneEffects.length;i++){ var ce=coneEffects[i]; if(now - ce.spawnMs < ce.lifeMs){ keep.push(ce); } else { removed++; } } 
  if(DEBUG_EFFECTS && removed>0){ try{ console.log('[CONE-FX]','active='+keep.length,'removed='+removed); }catch(_){} } coneEffects=keep; }
function spawnEnemies(){
  enemies=[];
  var spawns=[{x:300,y:150,type:'fast'},{x:500,y:350,type:'normal'},{x:200,y:400,type:'tank'},{x:600,y:200,type:'fast'}];
  for(var i=0;i<spawns.length;i++){
    var s=spawns[i]; var eType=enemyTypes[s.type||'normal'];
    enemies.push({x:s.x,y:s.y,z:0,enemyType:eType,health:eType.health,maxHealth:eType.health,speed:eType.speed,chaseRange:eType.chaseRange,lastUpdate:0,damageFlash:0,slowUntil:0,burnUntil:0,burnDmgLast:0,iceHits:0,fireHits:0,lightningHits:0});
  }
  console.log('[ENEMY] Spawned ' + enemies.length + ' enemies');
}
function updateEnemies(dt){
  if(!enemies||!enemies.length) return;
  var now=Date.now(); var alive=[]; var throttled=0, moved=0, burning=0, slowed=0;
  for(var i=0;i<enemies.length;i++){
    var e=enemies[i];
    if(e.health<=0){
      console.log('[ENEMY] Enemy ' + i + ' defeated!');
      deathEffects.push({x:e.x,y:e.y,z:e.z||0,color:e.enemyType.color,spawnMs:now,lifeMs:400});
      continue;
    }
    if(!e.lastUpdate) e.lastUpdate=now;
    if(now - e.lastUpdate < 80){ alive.push(e); throttled++; continue; }
    e.lastUpdate=now;
    if(e.burnUntil && now<e.burnUntil){
      burning++;
      if(!e.burnDmgLast || now-e.burnDmgLast>=500){
        e.health-=0.3; e.burnDmgLast=now;
        console.log('[BURN] Enemy burning! HP: ' + e.health.toFixed(1));
      }
    }
    var dx=pos.x-e.x, dy=pos.y-e.y; var dist=Math.hypot(dx,dy);
    if(dist<e.chaseRange && dist>20){
      var dirX=dx/dist, dirY=dy/dist; var moveSpeed=e.speed*0.08;
      if(e.slowUntil && now<e.slowUntil){ moveSpeed*=0.4; slowed++; }
      var nx=e.x + dirX*moveSpeed; var ny=e.y + dirY*moveSpeed;
      var gx=Math.floor(nx/cell), gy=Math.floor(ny/cell); var blocked=false;
      if(gx<0||gy<0||gx>=gridW||gy>=gridH){ blocked=true; }
      else if(grid && grid[gy*gridW+gx]){ blocked=true; }
      if(!blocked){ e.x=nx; e.y=ny; moved++; }
    }
    alive.push(e);
  }
  if(DEBUG_EFFECTS && now-__effectsLastLog>__effectsLogInterval){
    try{ console.log('[EFFECTS]','enemies='+alive.length,'throttled='+throttled,'moved='+moved,'burning='+burning,'slowed='+slowed,'deathFX='+deathEffects.length,'proj='+projectiles.length,'cone='+coneEffects.length,'impacts='+impacts.length); __effectsLastLog=now; }catch(_){}
  }
  enemies=alive;
}
function drawEnemies3D(){
  if(!enemies||!enemies.length) return;
  var w=canvas.width, h=canvas.height; var halfFov=cam.fov/2;
  var pitchOffset=Math.floor(-(cam.pitch||0)*100); var horizon=h*0.75; var now=Date.now();
  for(var i=0;i<enemies.length;i++){
    var e=enemies[i]; var eType=e.enemyType;
    var dx=e.x-cam.x, dy=e.y-cam.y; var dist=Math.hypot(dx,dy);
    if(dist<1) dist=1; if(dist>800) continue;
    var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
    while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
    if(Math.abs(da)>halfFov*1.2) continue;
    var sx=(da/halfFov)*0.5 + 0.5; var screenX=Math.floor(sx*w);
    if(depthBuffer && screenX>=0 && screenX<depthBuffer.length && dist>depthBuffer[screenX]+5) continue;
    var perspectiveShift=Math.floor(70 * (1 - dist/(dist+40)));
    var floorY=Math.floor(horizon + perspectiveShift)+pitchOffset;
    var baseSize=Math.max(5, Math.min(45, Math.floor(600/(dist+15)))); var size=baseSize*eType.size;
    var zVal=(e.z||0) + 12; var liftRaw=(zVal*1.0) / (1 + dist*0.01);
    var liftPx = Math.max(-80, Math.min(80, Math.floor(liftRaw))); var centerY=floorY - liftPx;
    ctx.save();
    var shadowAlpha=Math.max(0.15, Math.min(0.5, 0.6 - dist/1000));
    ctx.globalAlpha=shadowAlpha; ctx.fillStyle='#000000'; ctx.beginPath();
    ctx.ellipse(screenX, floorY, size*0.45, size*0.2, 0, 0, Math.PI*2); ctx.fill();
    ctx.globalAlpha=0.95; var isFlashing=(e.damageFlash && now<e.damageFlash);
    ctx.fillStyle=isFlashing?'#ffffff':eType.color; ctx.strokeStyle=isFlashing?'#ffffff':'#990000';
    ctx.lineWidth=Math.max(1, Math.min(3, Math.floor(size/15))); ctx.beginPath();
    ctx.arc(screenX, centerY, size/2, 0, Math.PI*2); ctx.fill(); ctx.stroke();
    if(e.slowUntil && now<e.slowUntil){
      ctx.strokeStyle='#00ffff'; ctx.lineWidth=Math.max(2, Math.min(4, Math.floor(size/12)));
      ctx.beginPath(); ctx.arc(screenX, centerY, size*0.55, 0, Math.PI*2); ctx.stroke();
    }
    if(e.burnUntil && now<e.burnUntil){
      ctx.fillStyle='rgba(255,100,0,0.7)'; ctx.beginPath();
      ctx.arc(screenX, centerY-size*0.4, size*0.25, 0, Math.PI*2); ctx.fill();
      ctx.fillStyle='rgba(255,150,0,0.5)'; ctx.beginPath();
      ctx.arc(screenX, centerY-size*0.6, size*0.15, 0, Math.PI*2); ctx.fill();
    }
    var hpPct=e.health/e.maxHealth;
    if(hpPct<1.0){
      var barW=Math.max(20, size*1.2); var barH=Math.max(2, Math.floor(size/10));
      var barY=centerY-size*0.8; ctx.fillStyle='rgba(0,0,0,0.5)';
      ctx.fillRect(screenX-barW/2, barY, barW, barH);
      ctx.fillStyle=(hpPct>0.5)?'#00ff00':(hpPct>0.25)?'#ffaa00':'#ff4444';
      ctx.fillRect(screenX-barW/2, barY, barW*hpPct, barH);
    }
    ctx.restore();
  }
}
function updateProjectiles(dt){
  if(!projectiles||!projectiles.length) return;
  var now=Date.now(); var alive=[]; var hitWalls=0, hitEnemies=0, expired=0;
  for(var i=0;i<projectiles.length;i++){
    var p=projectiles[i];
    var nx=p.x+Math.cos(p.ang)*(p.hz||p.speed)*dt;
    var ny=p.y+Math.sin(p.ang)*(p.hz||p.speed)*dt; var nz=(p.z||0) + (p.vz||0)*dt;
    var gx=Math.floor(nx/cell), gy=Math.floor(ny/cell); var hitWall=false;
    if(gx<0||gy<0||gx>=gridW||gy>=gridH){ hitWall=true; }
    else if(grid && grid[gy*gridW+gx]){ hitWall=true; }
    if(hitWall){ impacts.push({x:nx,y:ny,z:nz,spawnMs:now,lifeMs:220}); hitWalls++; continue; }
    var hitEnemy=false;
    if(enemies&&enemies.length){
      for(var ei=0;ei<enemies.length;ei++){
        var e=enemies[ei];
        var dx=nx-e.x, dy=ny-e.y, dz=nz-(e.z||0);
        var dist3d=Math.sqrt(dx*dx+dy*dy+dz*dz);
        var hitRadius=MODE3D?45:20; hitRadius+=(p.r||8);
        if(dist3d<hitRadius){
          var spell=p.spell||spells.missile; var dmg=spell.damage||1;
          e.health-=dmg; stats.totalDamageDone+=dmg;
          if(e.health<=0){ stats.totalEnemiesKilled++; }
          e.damageFlash=now+120;
          if(spell.id==='ice'){
            e.iceHits=(e.iceHits||0)+1;
            if(e.iceHits>=3){ e.slowUntil=now+2500; e.iceHits=0; }
          } else if(spell.id==='fire'){
            e.fireHits=(e.fireHits||0)+1;
            if(e.fireHits>=3){ e.burnUntil=now+4000; e.burnDmgLast=now; e.fireHits=0; }
          } else if(spell.id==='lightning'){
            e.lightningHits=(e.lightningHits||0)+1;
            if(e.lightningHits>=3){ e.slowUntil=now+1500; e.lightningHits=0; }
          }
          hitEnemy=true; impacts.push({x:nx,y:ny,z:nz,spawnMs:now,lifeMs:220}); hitEnemies++; break;
        }
      }
    }
    if(hitEnemy) continue;
    p.x=nx; p.y=ny; p.z=nz;
    if(now - p.spawnMs < p.lifeMs){ alive.push(p); } else { expired++; }
  }
  if(DEBUG_EFFECTS && hitWalls+hitEnemies+expired>0){
    try{ console.log('[PROJECTILES]','alive='+alive.length,'hitWalls='+hitWalls,'hitEnemies='+hitEnemies,'expired='+expired); }catch(_){}
  }
  projectiles=alive;
}
function updateImpacts(){
  if(!impacts||!impacts.length) return; var now=Date.now(); var keep=[]; var removed=0;
  for(var i=0;i<impacts.length;i++){
    var im=impacts[i];
    if(now - im.spawnMs < im.lifeMs){ keep.push(im); } else { removed++; }
  }
  if(DEBUG_EFFECTS && removed>0){ try{ console.log('[IMPACTS]','active='+keep.length,'removed='+removed); }catch(_){} }
  impacts=keep;
}
function updateDeathEffects(){
  if(!deathEffects||!deathEffects.length) return; var now=Date.now(); var keep=[]; var removed=0;
  for(var i=0;i<deathEffects.length;i++){
    var de=deathEffects[i];
    if(now - de.spawnMs < de.lifeMs){ keep.push(de); } else { removed++; }
  }
  if(DEBUG_EFFECTS && removed>0){ try{ console.log('[DEATH-FX]','active='+keep.length,'removed='+removed); }catch(_){} }
  deathEffects=keep;
}
function drawLightningBolt(x, y, ang, size){
  var len=size*3.5; var endX=x+Math.cos(ang)*len; var endY=y+Math.sin(ang)*len;
  var segments=6; ctx.shadowBlur=8; ctx.shadowColor='#ffff00';
  ctx.lineWidth=Math.max(2, ctx.lineWidth); ctx.beginPath(); ctx.moveTo(x, y);
  for(var s=1; s<=segments; s++){
    var t=s/segments; var baseX=x + (endX-x)*t; var baseY=y + (endY-y)*t;
    var perpAng=ang+Math.PI/2; var offset=(s%2===0?1:-1)*size*0.6;
    var segX=baseX + Math.cos(perpAng)*offset; var segY=baseY + Math.sin(perpAng)*offset;
    ctx.lineTo(segX, segY);
  }
  ctx.lineTo(endX, endY); ctx.stroke();
  var branch1Ang=ang+0.7; var branch1Len=len*0.5;
  var branch1X=x+Math.cos(ang)*len*0.25; var branch1Y=y+Math.sin(ang)*len*0.25;
  ctx.beginPath(); ctx.moveTo(branch1X, branch1Y);
  var b1endX=branch1X+Math.cos(branch1Ang)*branch1Len; var b1endY=branch1Y+Math.sin(branch1Ang)*branch1Len;
  ctx.lineTo(b1endX, b1endY); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(b1endX, b1endY);
  ctx.lineTo(b1endX+Math.cos(branch1Ang+0.4)*branch1Len*0.3, b1endY+Math.sin(branch1Ang+0.4)*branch1Len*0.3);
  ctx.stroke();
  var branch2Ang=ang-0.8; var branch2Len=len*0.55;
  var branch2X=x+Math.cos(ang)*len*0.5; var branch2Y=y+Math.sin(ang)*len*0.5;
  ctx.beginPath(); ctx.moveTo(branch2X, branch2Y);
  var b2endX=branch2X+Math.cos(branch2Ang)*branch2Len; var b2endY=branch2Y+Math.sin(branch2Ang)*branch2Len;
  ctx.lineTo(b2endX, b2endY); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(b2endX, b2endY);
  ctx.lineTo(b2endX+Math.cos(branch2Ang-0.5)*branch2Len*0.35, b2endY+Math.sin(branch2Ang-0.5)*branch2Len*0.35);
  ctx.stroke();
  var branch3Ang=ang+0.5; var branch3Len=len*0.4;
  var branch3X=x+Math.cos(ang)*len*0.75; var branch3Y=y+Math.sin(ang)*len*0.75;
  ctx.beginPath(); ctx.moveTo(branch3X, branch3Y);
  ctx.lineTo(branch3X+Math.cos(branch3Ang)*branch3Len, branch3Y+Math.sin(branch3Ang)*branch3Len);
  ctx.stroke(); ctx.shadowBlur=0;
}
function drawProjectiles2D(){
  if(!projectiles||!projectiles.length) return; var now=Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for(var i=0;i<projectiles.length;i++){
    var p=projectiles[i]; var age=(now - p.spawnMs);
    var t=Math.max(0, Math.min(1, age/(p.lifeMs||1000))); ctx.globalAlpha=1.0 - t;
    var spellId=(p.spell && p.spell.id) || 'missile';
    if(spellId==='lightning'){
      ctx.strokeStyle=p.spell.color; ctx.lineWidth=2; ctx.lineCap='round';
      drawLightningBolt(p.x, p.y, p.ang, p.r*1.5);
    } else if(spellId==='fire'){
      ctx.shadowBlur=12; ctx.shadowColor='#ff6600'; ctx.fillStyle=p.spell.color;
      ctx.beginPath(); ctx.arc(p.x,p.y,p.r*1.3,0,Math.PI*2); ctx.fill();
      ctx.fillStyle='#ffaa00'; ctx.beginPath(); ctx.arc(p.x,p.y,p.r*0.7,0,Math.PI*2); ctx.fill();
      var flameAng=p.ang+Math.PI;
      for(var f=0;f<3;f++){
        var fang=flameAng+(f-1)*0.4; var fx=p.x+Math.cos(fang)*(p.r*1.8); var fy=p.y+Math.sin(fang)*(p.r*1.8);
        ctx.fillStyle='rgba(255,100,0,0.6)'; ctx.beginPath(); ctx.arc(fx,fy,p.r*0.5,0,Math.PI*2); ctx.fill();
      }
      ctx.shadowBlur=0;
    } else if(spellId==='ice'){
      ctx.shadowBlur=10; ctx.shadowColor='#00ffff'; ctx.strokeStyle=p.spell.color; ctx.lineWidth=2;
      ctx.fillStyle='rgba(0,255,255,0.3)'; ctx.beginPath(); ctx.arc(p.x,p.y,p.r*1.2,0,Math.PI*2); ctx.fill(); ctx.stroke();
      for(var s=0;s<6;s++){
        var sang=p.ang+(s*Math.PI/3); var sx1=p.x+Math.cos(sang)*(p.r*0.4); var sy1=p.y+Math.sin(sang)*(p.r*0.4);
        var sx2=p.x+Math.cos(sang)*(p.r*1.5); var sy2=p.y+Math.sin(sang)*(p.r*1.5);
        ctx.beginPath(); ctx.moveTo(sx1,sy1); ctx.lineTo(sx2,sy2); ctx.stroke();
      }
      ctx.shadowBlur=0;
    } else {
      ctx.shadowBlur=5; ctx.shadowColor=p.spell.color;
      ctx.fillStyle=(p.spell && p.spell.color) || '#ffd54f';
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r, 0, Math.PI*2); ctx.fill(); ctx.shadowBlur=0;
    }
  }
  ctx.restore(); ctx.globalAlpha=1.0;
}
function drawProjectiles3D(){
  if(!projectiles||!projectiles.length) return;
  var w=canvas.width, h=canvas.height; var halfFov=cam.fov/2;
  var pitchOffset=Math.floor(-(cam.pitch||0)*100); var horizon=h*0.75;
  for(var i=0;i<projectiles.length;i++){
    var p=projectiles[i]; var dx=p.x-cam.x, dy=p.y-cam.y; var dist=Math.hypot(dx,dy);
    if(dist<1) dist=1; if(dist>600) continue;
    var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
    while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
    if(Math.abs(da)>halfFov*1.2) continue;
    var sx=(da/halfFov)*0.5 + 0.5; var screenX=Math.floor(sx*w);
    if(depthBuffer && screenX>=0 && screenX<depthBuffer.length && dist>depthBuffer[screenX]+2) continue;
    var perspectiveShift=Math.floor(70 * (1 - dist/(dist+40)));
    var floorY=Math.floor(horizon + perspectiveShift)+pitchOffset;
    var size=Math.max(6, Math.min(22, Math.floor(240/dist)));
    var zVal=(p.z||0); var liftRaw=(zVal*1.0) / (1 + dist*0.01);
    var liftPx = Math.max(-60, Math.min(60, Math.floor(liftRaw))); var centerY=floorY - liftPx;
    ctx.save(); ctx.globalAlpha=0.95; var spellId=(p.spell && p.spell.id) || 'missile';
    if(spellId==='lightning'){
      ctx.strokeStyle=p.spell.color; ctx.lineWidth=Math.max(2, Math.min(4, size/5)); ctx.lineCap='round';
      drawLightningBolt(screenX, centerY, p.ang, size*0.8);
    } else if(spellId==='fire'){
      ctx.shadowBlur=10; ctx.shadowColor='#ff6600'; ctx.fillStyle=p.spell.color;
      ctx.beginPath(); ctx.arc(screenX, centerY, size*0.65, 0, Math.PI*2); ctx.fill();
      ctx.fillStyle='#ffaa00'; ctx.beginPath(); ctx.arc(screenX, centerY, size*0.35, 0, Math.PI*2); ctx.fill();
      ctx.shadowBlur=0;
    } else if(spellId==='ice'){
      ctx.shadowBlur=8; ctx.shadowColor='#00ffff'; ctx.strokeStyle=p.spell.color; ctx.lineWidth=Math.max(1.5, size/10);
      ctx.fillStyle='rgba(0,255,255,0.3)'; ctx.beginPath(); ctx.arc(screenX, centerY, size*0.6, 0, Math.PI*2); ctx.fill(); ctx.stroke();
      for(var s=0;s<6;s++){
        var sang=p.ang+(s*Math.PI/3); var sx1=screenX+Math.cos(sang)*(size*0.2); var sy1=centerY+Math.sin(sang)*(size*0.2);
        var sx2=screenX+Math.cos(sang)*(size*0.75); var sy2=centerY+Math.sin(sang)*(size*0.75);
        ctx.beginPath(); ctx.moveTo(sx1,sy1); ctx.lineTo(sx2,sy2); ctx.stroke();
      }
      ctx.shadowBlur=0;
    } else {
      ctx.shadowBlur=5; ctx.shadowColor=p.spell.color; ctx.fillStyle=(p.spell && p.spell.color) || '#ffd54f';
      ctx.beginPath(); ctx.arc(screenX, centerY, size/2, 0, Math.PI*2); ctx.fill(); ctx.shadowBlur=0;
    }
    ctx.restore();
  }
}
function drawImpacts2D(){
  if(!impacts||!impacts.length) return; var now=Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for(var i=0;i<impacts.length;i++){
    var im=impacts[i]; var t=Math.max(0, Math.min(1, (now-im.spawnMs)/im.lifeMs));
    ctx.globalAlpha=1.0 - t; ctx.strokeStyle='rgba(255,212,79,0.9)'; ctx.lineWidth=2;
    ctx.beginPath(); ctx.arc(im.x, im.y, 4 + 18*t, 0, Math.PI*2); ctx.stroke();
  }
  ctx.restore(); ctx.globalAlpha=1.0;
}
function drawImpacts3D(){
  if(!impacts||!impacts.length) return;
  var w=canvas.width, h=canvas.height; var halfFov=cam.fov/2;
  var pitchOffset=Math.floor(-(cam.pitch||0)*100); var horizon=h*0.75; var now=Date.now();
  for(var i=0;i<impacts.length;i++){
    var im=impacts[i]; var dx=im.x-cam.x, dy=im.y-cam.y; var dist=Math.hypot(dx,dy);
    if(dist<1) dist=1; if(dist>800) continue;
    var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
    while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
    if(Math.abs(da)>halfFov*1.3) continue;
    var sx=(da/halfFov)*0.5 + 0.5; var screenX=Math.floor(sx*w);
    if(depthBuffer && screenX>=0 && screenX<depthBuffer.length && dist>depthBuffer[screenX]+2) continue;
    var perspectiveShift=Math.floor(70 * (1 - dist/(dist+40)));
    var floorY=Math.floor(horizon + perspectiveShift)+pitchOffset;
    var zVal=(im.z||0); var liftRaw=(zVal*1.0) / (1 + dist*0.01);
    var liftPx=Math.max(-60, Math.min(60, Math.floor(liftRaw))); var centerY=floorY - liftPx;
    var t=Math.max(0, Math.min(1, (now-im.spawnMs)/im.lifeMs));
    var size=Math.max(6, Math.min(28, Math.floor(220/dist))); var r=(size*0.5) + 10*t;
    ctx.save(); ctx.globalAlpha=1.0 - t; ctx.strokeStyle='rgba(255,212,79,0.95)'; ctx.lineWidth=2;
    ctx.beginPath(); ctx.arc(screenX, centerY, r, 0, Math.PI*2); ctx.stroke(); ctx.restore();
  }
}
function drawConeEffects2D(){
  if(!coneEffects||!coneEffects.length) return; var now=Date.now();
  ctx.save(); ctx.translate(-viewCam.x, -viewCam.y);
  for(var i=0;i<coneEffects.length;i++){
    var ce=coneEffects[i]; var t=Math.max(0, Math.min(1, (now-ce.spawnMs)/ce.lifeMs));
    var isFire=(ce.spellId==='fire');
    if(isFire){
      ctx.shadowBlur=15; ctx.shadowColor='#ff6600'; var particles=8;
      for(var p=0;p<particles;p++){
        var pct=p/particles; var dist=ce.range*(0.2+0.8*pct); var spread=ce.halfAngle*2*pct;
        var pAng=ce.ang + (Math.random()-0.5)*spread;
        var px=ce.x + Math.cos(pAng)*dist; var py=ce.y + Math.sin(pAng)*dist;
        var size=8*(1-pct)*(1-t); ctx.globalAlpha=(0.6-0.4*pct)*(1.0-t);
        ctx.fillStyle=(p%2===0)?'#ff6600':'#ffaa00';
        ctx.beginPath(); ctx.arc(px, py, size, 0, Math.PI*2); ctx.fill();
      }
      ctx.shadowBlur=0;
    } else {
      ctx.globalAlpha=0.4*(1.0-t); ctx.fillStyle=ce.color; ctx.beginPath(); ctx.moveTo(ce.x, ce.y);
      var leftAng=ce.ang-ce.halfAngle; var rightAng=ce.ang+ce.halfAngle;
      ctx.arc(ce.x, ce.y, ce.range, leftAng, rightAng); ctx.closePath(); ctx.fill();
      ctx.globalAlpha=0.7*(1.0-t); ctx.strokeStyle=ce.color; ctx.lineWidth=2; ctx.beginPath();
      ctx.moveTo(ce.x, ce.y); ctx.lineTo(ce.x+Math.cos(leftAng)*ce.range, ce.y+Math.sin(leftAng)*ce.range);
      ctx.moveTo(ce.x, ce.y); ctx.lineTo(ce.x+Math.cos(rightAng)*ce.range, ce.y+Math.sin(rightAng)*ce.range);
      ctx.stroke();
    }
  }
  ctx.restore(); ctx.globalAlpha=1.0;
}
function drawConeEffects3D(){
  if(!coneEffects||!coneEffects.length) return;
  var w=canvas.width, h=canvas.height; var halfFov=cam.fov/2;
  var pitchOffset=Math.floor(-(cam.pitch||0)*100); var horizon=h*0.75; var now=Date.now();
  for(var i=0;i<coneEffects.length;i++){
    var ce=coneEffects[i]; var t=Math.max(0, Math.min(1, (now-ce.spawnMs)/ce.lifeMs));
    var isFire=(ce.spellId==='fire');
    if(isFire){
      ctx.save(); ctx.shadowBlur=15; ctx.shadowColor='#ff6600'; var particles=20;
      for(var p=0;p<particles;p++){
        var pct=p/particles; var dist=ce.range*(0.1+0.9*pct); var spread=ce.halfAngle*2*pct;
        var pAng=ce.ang + (Math.random()-0.5)*spread;
        var px=ce.x + Math.cos(pAng)*dist; var py=ce.y + Math.sin(pAng)*dist;
        var dx=px-cam.x, dy=py-cam.y; var d=Math.hypot(dx,dy);
        if(d<1) d=1; if(d>300) continue;
        var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
        while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
        if(Math.abs(da)>halfFov*1.3) continue;
        var sx=(da/halfFov)*0.5 + 0.5; var screenX=Math.floor(sx*w);
        var perspectiveShift=Math.floor(70 * (1 - d/(d+40)));
        var floorY=Math.floor(horizon + perspectiveShift)+pitchOffset;
        var size=Math.max(6, Math.min(24, Math.floor(220/d)))*(1-pct*0.5);
        ctx.globalAlpha=(0.7-0.4*pct)*(1.0-t); var hue=p%3;
        ctx.fillStyle=(hue===0)?'#ff6600':(hue===1?'#ffaa00':'#ff8800');
        ctx.beginPath(); ctx.arc(screenX, floorY, size, 0, Math.PI*2); ctx.fill();
      }
      ctx.restore();
    } else {
      ctx.save(); ctx.shadowBlur=10; ctx.shadowColor='#00ffff';
      var leftAng=ce.ang-ce.halfAngle; var rightAng=ce.ang+ce.halfAngle; var rays=8;
      for(var r=0;r<rays;r++){
        var rayAng=leftAng + (rightAng-leftAng)*(r/(rays-1));
        ctx.globalAlpha=0.5*(1.0-t); ctx.strokeStyle='rgba(0,255,255,0.8)'; ctx.lineWidth=3;
        ctx.beginPath(); var steps=6;
        for(var s=0;s<=steps;s++){
          var dist=ce.range*(s/steps); var px=ce.x + Math.cos(rayAng)*dist; var py=ce.y + Math.sin(rayAng)*dist;
          var dx=px-cam.x, dy=py-cam.y; var d=Math.hypot(dx,dy); if(d<1) d=1;
          var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
          while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
          var sx=(da/halfFov)*0.5 + 0.5; var screenX=Math.floor(sx*w);
          var perspectiveShift=Math.floor(70 * (1 - d/(d+40)));
          var floorY=Math.floor(horizon + perspectiveShift)+pitchOffset;
          if(s===0){ ctx.moveTo(screenX, floorY); } else { ctx.lineTo(screenX, floorY); }
        }
        ctx.stroke();
      }
      ctx.globalAlpha=0.2*(1.0-t); ctx.fillStyle='rgba(0,255,255,0.3)'; ctx.beginPath();
      var arcSteps=16;
      for(var s=0;s<=arcSteps;s++){
        var a=leftAng + (rightAng-leftAng)*(s/arcSteps); var dist=ce.range;
        var px=ce.x + Math.cos(a)*dist; var py=ce.y + Math.sin(a)*dist;
        var dx=px-cam.x, dy=py-cam.y; var d=Math.hypot(dx,dy); if(d<1) d=1;
        var ang=Math.atan2(dy,dx); var da=ang - cam.ang;
        while(da>Math.PI) da-=Math.PI*2; while(da<-Math.PI) da+=Math.PI*2;
        var sx=(da/halfFov)*0.5 + 0.5; var screenX=Math.floor(sx*w);
        var perspectiveShift=Math.floor(70 * (1 - d/(d+40)));
        var floorY=Math.floor(horizon + perspectiveShift)+pitchOffset;
        if(s===0){ ctx.moveTo(screenX, floorY); } else { ctx.lineTo(screenX, floorY); }
      }
      var ox=ce.x-cam.x, oy=ce.y-cam.y; var od=Math.hypot(ox,oy); if(od<1) od=1;
      var oang=Math.atan2(oy,ox); var oda=oang - cam.ang;
      while(oda>Math.PI) oda-=Math.PI*2; while(oda<-Math.PI) oda+=Math.PI*2;
      var osx=(oda/halfFov)*0.5 + 0.5; var oScreenX=Math.floor(osx*w);
      var oPerspectiveShift=Math.floor(70 * (1 - od/(od+40)));
      var oFloorY=Math.floor(horizon + oPerspectiveShift)+pitchOffset;
      ctx.lineTo(oScreenX, oFloorY); ctx.closePath(); ctx.fill(); ctx.restore();
    }
  }
}
function drawMenuOverlay(){
  if(!menuOpen) return; var w=canvas.width, h=canvas.height;
  ctx.save(); ctx.globalAlpha=0.85; ctx.fillStyle='rgba(0,0,0,0.75)'; ctx.fillRect(0,0,w,h);
  var menuW=Math.min(400, w*0.6), menuH=Math.min(300, h*0.6);
  var mx=(w-menuW)/2, my=(h-menuH)/2;
  ctx.fillStyle='rgba(20,20,40,0.95)'; ctx.strokeStyle='rgba(100,150,255,0.8)'; ctx.lineWidth=2;
  ctx.fillRect(mx,my,menuW,menuH); ctx.strokeRect(mx,my,menuW,menuH);
  ctx.fillStyle='#ffffff'; ctx.font='bold 20px Arial'; ctx.textAlign='center';
  ctx.fillText('Player Stats', mx+menuW/2, my+30);
  ctx.font='16px Arial'; ctx.textAlign='left';
  var lineY=my+60; var lineH=24; var leftX=mx+20;
  ctx.fillStyle='#aaddff'; ctx.fillText('Mana:', leftX, lineY);
  ctx.fillStyle='#ffffff'; var manaPct=((mana/MANA_MAX)*100).toFixed(0);
  ctx.fillText(Math.floor(mana)+' / '+MANA_MAX+' ('+manaPct+'%)', leftX+120, lineY); lineY+=lineH;
  ctx.fillStyle='#aaddff'; ctx.fillText('Health:', leftX, lineY);
  ctx.fillStyle='#ffffff'; var hpPct=((health/HEALTH_MAX)*100).toFixed(0);
  ctx.fillText(Math.floor(health)+' / '+HEALTH_MAX+' ('+hpPct+'%)', leftX+120, lineY); lineY+=lineH;
  ctx.fillStyle='#aaddff'; ctx.fillText('Level:', leftX, lineY);
  ctx.fillStyle='#ffffff'; ctx.fillText(level.toString(), leftX+120, lineY); lineY+=lineH;
  ctx.fillStyle='#aaddff'; ctx.fillText('Enemies Defeated:', leftX, lineY);
  ctx.fillStyle='#ffffff'; ctx.fillText(stats.totalEnemiesKilled.toString(), leftX+120, lineY); lineY+=lineH+10;
  var barW=menuW-40, barH=20; var barX=mx+20, barY=lineY;
  ctx.fillStyle='rgba(0,0,0,0.5)'; ctx.fillRect(barX,barY,barW,barH);
  var manaPct2=Math.max(0,Math.min(1,mana/MANA_MAX)); ctx.fillStyle='#4db6ff';
  ctx.fillRect(barX,barY,Math.floor(barW*manaPct2),barH);
  ctx.strokeStyle='rgba(255,255,255,0.6)'; ctx.lineWidth=1; ctx.strokeRect(barX,barY,barW,barH);
  ctx.fillStyle='#ffffff'; ctx.font='12px Arial'; ctx.textAlign='center';
  ctx.fillText('Mana', barX+barW/2, barY+14);
  ctx.font='14px Arial'; ctx.textAlign='center'; ctx.fillStyle='#ffdd88';
  ctx.fillText('Press Start to close', mx+menuW/2, my+menuH-20);
  ctx.restore();
}
function drawHudOverlay(){

  var w=canvas.width, h=canvas.height;

  ctx.save();

  var pad=10;

  var barW=Math.min(220, Math.floor(w*0.3)), barH=10;

  var x=pad, y=h-pad-barH;

  // Health bar (above mana)

  var hy=y-16;

  ctx.globalAlpha=0.9;

  ctx.fillStyle='rgba(0,0,0,0.35)';

  ctx.fillRect(x-2, hy-2, barW+4, barH+4);

  var hpct=Math.max(0, Math.min(1, health/HEALTH_MAX));

  ctx.fillStyle='#ff6a6a';

  ctx.fillRect(x, hy, Math.floor(barW*hpct), barH);

  ctx.strokeStyle='rgba(255,255,255,0.6)';

  ctx.lineWidth=1;

  ctx.strokeRect(x, hy, barW, barH);

  // Mana bar background

  ctx.globalAlpha=0.9;

  ctx.fillStyle='rgba(0,0,0,0.35)';

  ctx.fillRect(x-2, y-2, barW+4, barH+4);

  var pct=Math.max(0, Math.min(1, mana/MANA_MAX));

  var blink=(Date.now()<manaBlinkUntil);

  if(blink){

    var a=0.5+0.5*Math.sin(Date.now()/120);

    ctx.fillStyle='rgba(255,64,64,'+a.toFixed(2)+')';

  } else {

    ctx.fillStyle='#4db6ff';

  }

  ctx.fillRect(x, y, Math.floor(barW*pct), barH);

  ctx.strokeStyle='rgba(255,255,255,0.6)';

  ctx.lineWidth=1;

  ctx.strokeRect(x, y, barW, barH);

  // Charge bar (shown when charging)

  if(charging){

    var cy=y-8-6;

    var cbarW=barW;

    var cPct=Math.max(0, Math.min(1, chargeLevel));

    ctx.fillStyle='rgba(0,0,0,0.35)';

    ctx.fillRect(x-2, cy-2, cbarW+4, 6+4);

    ctx.fillStyle='#ffd54f';

    ctx.fillRect(x, cy, Math.floor(cbarW*cPct), 6);

    ctx.strokeStyle='rgba(255,255,255,0.5)';

    ctx.strokeRect(x, cy, cbarW, 6);

  }

  // Spell indicator (top-right)

  var spell=getCurrentSpell();

  var sx=w-pad-60, sy=pad+5;

  ctx.globalAlpha=0.9;

  ctx.fillStyle='rgba(0,0,0,0.5)';

  ctx.fillRect(sx-5, sy-5, 70, 30);

  ctx.fillStyle=spell.color;

  ctx.beginPath();

  ctx.arc(sx+10, sy+10, 8, 0, Math.PI*2);

  ctx.fill();

  ctx.fillStyle='#fff';

  ctx.font='12px Arial';

  ctx.textAlign='left';

  ctx.fillText(spell.name.substr(0,8), sx+22, sy+14);

  ctx.restore();

}

function updateHudInput(){

  var el=document.getElementById('hudInput');

  if(!el) return;

  var label=(CONTROL_MODE===MODE_GYRO_AIM)?'Gyro Aim (Stick Move)':'Stick Aim (XYAB Move)';

  el.textContent = label;

}

function step(dt){
  // Mana regen

  mana = Math.min(MANA_MAX, mana + MANA_REGEN_PER_S * dt);
  health = Math.min(HEALTH_MAX, health + HEALTH_REGEN_PER_S * dt);
  vel.x*=damping; vel.y*=damping;
  pos.x+=vel.x*dt; pos.y+=vel.y*dt;
  if((terrain==='plains' || terrain==='cave') && floorMesh){ var floorHeight=getFloorHeightAt(pos.x, pos.y); if(!pos.floorZ) pos.floorZ=0; var targetZ=floorHeight*40 + 60; var zDiff=targetZ-pos.floorZ; pos.floorZ += zDiff*0.3; if(Math.random()<0.01){ console.log('[FLOOR] Player at (' + pos.x.toFixed(1) + ',' + pos.y.toFixed(1) + ') floorHeight=' + floorHeight.toFixed(3) + ' targetZ=' + targetZ.toFixed(1) + ' currentZ=' + pos.floorZ.toFixed(1)); } }
  if(pos.x<6){ pos.x=6; vel.x*=-bounce; } if(pos.x>720-6){ pos.x=720-6; vel.x*=-bounce; }
  if(pos.y<6){ pos.y=6; vel.y*=-bounce; } if(pos.y>480-6){ pos.y=480-6; vel.y*=-bounce; }
  var player={x:pos.x-6,y:pos.y-6,w:12,h:12};
  for(var i=0;i<walls.length;i++){ var w=walls[i]; if(rectsOverlap(player,w)){ collisions++; document.getElementById('hudCol').textContent=collisions;
      pos.x-=vel.x*dt*2; pos.y-=vel.y*dt*2; vel.x*=-0.5; vel.y*=-0.5; player={x:pos.x-6,y:pos.y-6,w:12,h:12}; } }
  repelFromWalls(); repelFromEnemies(dt);
  if(rectsOverlap(player, goal)){ if(level<levels.length){ if(terrain==='ice'){ terrain='cave'; } else if(terrain==='cave'){ terrain='plains'; } else if(terrain==='plains'){ terrain='ice'; } else { terrain='ice'; } var sel=document.getElementById('terrainSelect'); if(sel){ sel.value=terrain; } resetLevel(level+1); } else { stopGame(); alert('You win!'); } }
  timeSec=(Date.now()-startMs)/1000; if(timeSec<=medalGold){ timeColor='#d4af37'; } else if(timeSec<=medalSilver){ timeColor='#c0c0c0'; } else if(timeSec<=medalBronze){ timeColor='#cd7f32'; } else { timeColor='#ffffff'; }
}
function repelFromWalls(){ var cx=pos.x, cy=pos.y; var rad=6, range=12, k=40; for(var i=0;i<walls.length;i++){ var w=walls[i]; var nx=Math.max(w.x, Math.min(cx, w.x+w.w)); var ny=Math.max(w.y, Math.min(cy, w.y+w.h)); var dx=cx-nx, dy=cy-ny; var dist=Math.hypot(dx,dy); var thresh=rad+range; if(dist<thresh && dist>0.001){ var s=(thresh-dist)/thresh; var inv=1.0/dist; vel.x += (dx*inv)*k*s*0.016; vel.y += (dy*inv)*k*s*0.016; } } }
function repelFromEnemies(dt){ if(!enemies||!enemies.length) return; var cx=pos.x, cy=pos.y; var playerRad=6, enemyRad=8, k=300; var debugLog=(Math.random()<0.02); for(var i=0;i<enemies.length;i++){ var e=enemies[i]; var dx=cx-e.x, dy=cy-e.y; var dist=Math.hypot(dx,dy); var thresh=playerRad+enemyRad; if(debugLog){ console.log('[ENEMY-COL] i=' + i + ' enemy=(' + e.x.toFixed(1) + ',' + e.y.toFixed(1) + ') player=(' + cx.toFixed(1) + ',' + cy.toFixed(1) + ') dist=' + dist.toFixed(1) + ' thresh=' + thresh); } if(dist<thresh && dist>0.001){ var overlap=thresh-dist; var inv=1.0/dist; var pushX=(dx*inv)*overlap*0.5, pushY=(dy*inv)*overlap*0.5; var newX=pos.x+pushX, newY=pos.y+pushY; var blocked=false; if(newX<6||newX>714||newY<6||newY>474){ blocked=true; } if(!blocked){ var gx=Math.floor(newX/cell), gy=Math.floor(newY/cell); if(gx>=0&&gy>=0&&gx<gridW&&gy<gridH&&grid&&grid[gy*gridW+gx]){ blocked=true; } } if(!blocked){ pos.x=newX; pos.y=newY; vel.x+=(dx*inv)*k*overlap*0.016; vel.y+=(dy*inv)*k*overlap*0.016; if(debugLog){ console.log('[ENEMY-COL] REPEL! push=(' + pushX.toFixed(2) + ',' + pushY.toFixed(2) + ')'); } } else { vel.x*=0.5; vel.y*=0.5; if(debugLog){ console.log('[ENEMY-COL] BLOCKED by wall at (' + newX.toFixed(1) + ',' + newY.toFixed(1) + ') - damping velocity'); } } var dps=10; var dam=dps*dt; health=Math.max(0, health - dam); stats.totalDamageTaken += dam; } } }
function applyGamepad(nx, ny, dt){ var dz=0.05; if(Math.abs(nx)<dz) nx=0; if(Math.abs(ny)<dz) ny=0; nx=clamp(nx,-1,1); ny=clamp(ny,-1,1); var dtScale = dt>0 ? (dt/0.060) : 1; /* invert Y so up is up */ var jy = -ny; if(MODE3D){ var ca=Math.cos(cam.ang), sa=Math.sin(cam.ang); var fwd=-jy, str=nx; /* flip forward only for 3D */ var vx = fwd*ca + str*(-sa); var vy = fwd*sa + str*( ca); vel.x += vx * speed * 0.15 * dtScale; vel.y += vy * speed * 0.15 * dtScale; } else { vel.x += (nx) * speed * 0.15 * dtScale; vel.y += (jy) * speed * 0.15 * dtScale; } }
function onCalibUpdate(){ var thr=0.35; var ok = (Math.abs(lastRoll-baseRoll)<thr) && (Math.abs(lastPitch-basePitch)<thr); if(hasYaw){ ok = ok && (Math.abs(angNorm(deg2rad(lastYaw - baseYaw))) < deg2rad(2.0)); } var now=Date.now(); if(ok){ calStableMs += 60; } else { calStableMs = 0; } if(calStableMs>=calNeedStableMs){ hasBaseline=true; calibrating=false; resetLevel(1); } }
function applyTilt(pitch, roll){
  if(hasBaseline){ roll -= baseRoll; pitch -= basePitch; }
  if(Math.abs(roll)<dead) roll=0; roll=clamp(roll,-maxAng,maxAng);
  if(Math.abs(pitch)<dead) pitch=0; pitch=clamp(pitch,-maxAng,maxAng);
  vel.x += (roll/maxAng)*speed*0.10;
  vel.y += (pitch/maxAng)*speed*0.10;
}
function pollIMU(){
  if(!i2cEnabled||!imuCompiled){
    console.warn('[GAMES] IMU polling skipped: i2c='+i2cEnabled+' imuCompiled='+imuCompiled);
    return;
  }
  var ts=Date.now();
  fetch('/api/sensors?sensor=imu&ts='+ts,{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){
    if(!running) return;
    if(j && j.valid && j.ori){ var pitch=j.ori.pitch||0; var roll=j.ori.roll||0; var yaw=(j.ori.yaw!==undefined && j.ori.yaw!==null)?j.ori.yaw:0; lastPitch=pitch; lastRoll=roll; hasYaw = (j.ori.yaw!==undefined && j.ori.yaw!==null); if(hasYaw){ lastYaw=yaw; } if(DEBUG_IMU){ try{ console.log('[IMU]', ts, 'pitch=', pitch.toFixed?pitch.toFixed(2):pitch, 'roll=', roll.toFixed?roll.toFixed(2):roll, 'yaw=', hasYaw?(yaw.toFixed?yaw.toFixed(2):yaw):'n/a', 'baseP=', basePitch.toFixed?basePitch.toFixed(2):basePitch, 'baseR=', baseRoll.toFixed?baseRoll.toFixed(2):baseRoll, 'baseY=', baseYaw.toFixed?baseYaw.toFixed(2):baseYaw, 'hasBase=', !!hasBaseline, 'pos=('+pos.x.toFixed(1)+','+pos.y.toFixed(1)+')'); }catch(_){ } } if(calibrating){ if(!hasBaseline){ basePitch=pitch; baseRoll=roll; if(hasYaw){ baseYaw=yaw; } } onCalibUpdate(); } else { if(!USE_GAMEPAD){ applyTilt(pitch,roll); } if(USE_YAW && hasYaw){ var yawDiff = yaw - baseYaw; while(yawDiff > 180) yawDiff -= 360; while(yawDiff < -180) yawDiff += 360; var relYaw = deg2rad(yawDiff); var yawSmooth = 0.7; yawTarget = yawTarget * yawSmooth + relYaw * (1 - yawSmooth); } if(USE_PITCH){ var relPitch = deg2rad(pitch - basePitch); var newPitchTarget = Math.max(-0.35, Math.min(0.35, relPitch)); var pitchSmooth = 0.6; pitchTarget = pitchTarget * pitchSmooth + newPitchTarget * (1 - pitchSmooth); } } }
  }).catch(function(_){});
}
function loop(ts){ if(!running) return; var loopStart=performance.now(); var dt=(lastUpdate? (ts-lastUpdate):16)/1000; lastUpdate=ts;

// Read buttons and handle Select toggle + Start menu toggle

  var btn=gpLast.buttons|0; var selectDown=((btn & BTN_SELECT)===0); if(selectDown && !lastSelectDown){ var now=Date.now(); if(now - lastSelectToggleMs > SELECT_TOGGLE_COOLDOWN){ CONTROL_MODE = (CONTROL_MODE===MODE_GYRO_AIM) ? MODE_STICK_AIM : MODE_GYRO_AIM; lastSelectToggleMs=now; console.log('[CTRL] toggled CONTROL_MODE=', CONTROL_MODE); updateHudInput(); } } lastSelectDown=selectDown;

  if(gpLast && gpLast.valid){ var startMenuDown=((btn & BTN_START)===0); if(startMenuDown && !lastStartMenuDown){ var now=Date.now(); if(now - lastStartMenuToggleMs > START_MENU_COOLDOWN){ menuOpen=!menuOpen; lastStartMenuToggleMs=now; console.log('[MENU] toggled menuOpen=', menuOpen); } } lastStartMenuDown=startMenuDown; }


/* Handle gamepad X-button for yaw capture (only in Gyro Aim) */

 if(MODE3D && CONTROL_MODE===MODE_GYRO_AIM && USE_GAMEPAD && gpLast && gpLast.valid){ var xPressed = ((gpLast.buttons & BTN_X)===0); if(xPressed && !prevX){ capturingYaw=true; camHold=cam.ang; yawAtPress=yawTarget; if(DEBUG_CAM){ try{ console.log('[CAM] press X', 'camHold=', camHold.toFixed(3), 'yawAtPress=', yawAtPress.toFixed?yawAtPress.toFixed(3):yawAtPress, 'yawOffset=', yawOffset.toFixed?yawOffset.toFixed(3):yawOffset); }catch(_){ } } } else if(!xPressed && prevX){ capturingYaw=false; var delta=angNorm(yawTarget - yawAtPress); yawOffset += delta; yawResumeUntil = Date.now() + 250; if(DEBUG_CAM){ try{ console.log('[CAM] release X', 'yawTarget=', yawTarget.toFixed?yawTarget.toFixed(3):yawTarget, 'yawAtPress=', yawAtPress.toFixed?yawAtPress.toFixed(3):yawAtPress, 'delta=', delta.toFixed?delta.toFixed(3):delta, 'newOffset=', yawOffset.toFixed?yawOffset.toFixed(3):yawOffset); }catch(_){ } } } prevX = xPressed; }

// Route inputs by control mode

 if(!calibrating){ if(USE_GAMEPAD && gpLast && gpLast.valid){ 

   // 2D Mode: Joystick controls movement directly

   if(!MODE3D){ 

     var now2=Date.now(); 

     var nx=gpLast.x, ny=-gpLast.y; 

     var mag=Math.hypot(nx,ny); 

     if(mag>0.1){ 

       var dtScale = dt>0 ? (dt/0.060) : 1; 

       vel.x += nx * speed * 0.12 * dtScale; 

       vel.y += ny * speed * 0.12 * dtScale; 

     } 

     // 2D Mode: XYAB buttons for spell system

     var btn2d=gpLast.buttons|0; 

     var xDown=((btn2d & BTN_X)===0); 

     var aDown=((btn2d & BTN_A)===0); 

     // X button: Cycle spell

     if(xDown && !lastXDown){ cycleSpell(); } 

     lastXDown=xDown; 

     // A button: Cast spell (rapid fire while held)

     if(aDown){ var spell=getCurrentSpell(); if(mana>=spell.manaCost && (now2-lastShotMs)>=SHOOT_COOLDOWN){ if(spell.attackType==='cone'){ castConeAttack(spell); } else { spawnProjectile(spell.speed, PROJ_RADIUS); } mana-=spell.manaCost; stats.totalManaConsumed+=spell.manaCost; lastShotMs=now2; } else if(mana<spell.manaCost && !lastADown){ manaBlinkUntil = now2 + 1000; } } 

     lastADown=aDown; 

   } 

   // 3D Mode: Spell system with movement

   else { 

     var now2=Date.now(); 

     // Movement: Gyro Aim uses gamepad for movement, Stick Aim uses XYAB for movement

     if(!menuOpen){ 

     if(CONTROL_MODE===MODE_GYRO_AIM){ 

       applyGamepad(gpLast.x, gpLast.y, dt); 

       // In Gyro mode: Y=cycle spell, A=cast spell (X is for yaw capture, Start opens menu)

       var btn3d=gpLast.buttons|0; 

       var yDown=((btn3d & BTN_Y)===0); 

       var aDown3d=((btn3d & BTN_A)===0); 

       if(yDown && !lastXDown){ cycleSpell(); } 

       lastXDown=yDown; 

       if(aDown3d){ var spell=getCurrentSpell(); if(mana>=spell.manaCost && (now2-lastShotMs)>=SHOOT_COOLDOWN){ if(spell.attackType==='cone'){ castConeAttack(spell); } else { spawnProjectile(spell.speed, PROJ_RADIUS); } mana-=spell.manaCost; stats.totalManaConsumed+=spell.manaCost; lastShotMs=now2; } else if(mana<spell.manaCost && !lastADown){ manaBlinkUntil = now2 + 1000; } } 

       lastADown=aDown3d; 

     } else { 

       // Stick Aim: Joystick controls camera

       var nx=gpLast.x, ny=gpLast.y; var jy=-ny; cam.ang += nx * STICK_CAM_YAW_SPEED * dt; cam.pitch = (cam.pitch||0) + (jy) * STICK_CAM_PITCH_SPEED * dt; if(cam.pitch>CAM_PITCH_MAX) cam.pitch=CAM_PITCH_MAX; if(cam.pitch<-CAM_PITCH_MAX) cam.pitch=-CAM_PITCH_MAX; 

       // XYAB: X=forward, B=back, Y=cycle spell, A=cast spell

       var btn=gpLast.buttons|0; 

       var mvF=((btn & BTN_X)===0), mvB=((btn & BTN_B)===0); 

       if(mvF||mvB){ var fwd=(mvF?1:0) + (mvB?-1:0); var ca=Math.cos(cam.ang), sa=Math.sin(cam.ang); var vx = fwd*ca; var vy = fwd*sa; var dtScale = dt>0 ? (dt/0.060) : 1; vel.x += vx * speed * 0.10 * dtScale; vel.y += vy * speed * 0.10 * dtScale; } 

       var yDown=((btn & BTN_Y)===0); 

       var aDown=((btn & BTN_A)===0); 

       if(yDown && !lastXDown){ cycleSpell(); } 

       lastXDown=yDown; 

       if(aDown){ var spell=getCurrentSpell(); if(mana>=spell.manaCost && (now2-lastShotMs)>=SHOOT_COOLDOWN){ if(spell.attackType==='cone'){ castConeAttack(spell); } else { spawnProjectile(spell.speed, PROJ_RADIUS); } mana-=spell.manaCost; stats.totalManaConsumed+=spell.manaCost; lastShotMs=now2; } else if(mana<spell.manaCost && !lastADown){ manaBlinkUntil = now2 + 1000; } } 

       lastADown=aDown; 

     } 

     } 

   } 

 } } var updateStart=performance.now(); if(!menuOpen){ updateEnemies(dt); updateProjectiles(dt); updateImpacts(); updateConeEffects(); updateDeathEffects(); step(dt); } var updateEnd=performance.now(); var drawStart=performance.now(); draw(); if(MODE3D){ drawConeEffects3D(); drawProjectiles3D(); drawImpacts3D(); drawEnemies3D(); } else { drawConeEffects2D(); drawProjectiles2D(); drawImpacts2D(); } drawHudOverlay(); drawMenuOverlay(); var drawEnd=performance.now(); var loopEnd=performance.now(); if(DEBUG_PERF){ var frameTime=loopEnd-loopStart; var updateTime=updateEnd-updateStart; var drawTime=drawEnd-drawStart; __perfFrames.push({total:frameTime,update:updateTime,draw:drawTime,dt:dt*1000,proj:projectiles.length,cone:coneEffects.length,enemy:enemies.length,impacts:impacts.length,gpValid:gpLast.valid,gpX:gpLast.x,gpY:gpLast.y}); var now=Date.now(); if(now-__perfLastLog>__perfLogInterval && __perfFrames.length>0){ var avgTotal=0,avgUpdate=0,avgDraw=0,avgDt=0,maxTotal=0,maxUpdate=0,maxDraw=0; for(var i=0;i<__perfFrames.length;i++){ var f=__perfFrames[i]; avgTotal+=f.total; avgUpdate+=f.update; avgDraw+=f.draw; avgDt+=f.dt; if(f.total>maxTotal)maxTotal=f.total; if(f.update>maxUpdate)maxUpdate=f.update; if(f.draw>maxDraw)maxDraw=f.draw; } var n=__perfFrames.length; avgTotal/=n; avgUpdate/=n; avgDraw/=n; avgDt/=n; var lastF=__perfFrames[__perfFrames.length-1]; try{ console.log('[PERF]','frames='+n,'avgTotal='+avgTotal.toFixed(1)+'ms','maxTotal='+maxTotal.toFixed(1)+'ms','avgUpdate='+avgUpdate.toFixed(1)+'ms','maxUpdate='+maxUpdate.toFixed(1)+'ms','avgDraw='+avgDraw.toFixed(1)+'ms','maxDraw='+maxDraw.toFixed(1)+'ms','avgDt='+avgDt.toFixed(1)+'ms','proj='+lastF.proj,'cone='+lastF.cone,'enemy='+lastF.enemy,'impacts='+lastF.impacts,'gpValid='+lastF.gpValid,'gpX='+lastF.gpX.toFixed(2),'gpY='+lastF.gpY.toFixed(2)); }catch(_){} __perfFrames=[]; __perfLastLog=now; } } requestAnimationFrame(loop); }
function startGame(){
  if(!i2cEnabled){
    alert('Cannot start game: I2C is disabled. Enable I2C in settings.');
    return;
  }
  if(!imuCompiled){
    alert('Cannot start game: IMU sensor not compiled into firmware.');
    return;
  }
  if(!imuEnabled || !gamepadEnabled){
    alert('Cannot start game: IMU and/or gamepad are disabled. Enable them in settings.');
    return;
  }
  running=true; resetLevel(1); startCalibration(); lastUpdate=0; calibrating=true; hasBaseline=false; calStableMs=0; CONTROL_MODE=MODE_STICK_AIM; selectPrimed=false; lastSelectDown=false; console.log('[CTRL] start CONTROL_MODE=', CONTROL_MODE); draw(); Promise.all([controlSensor('imu','start'), controlSensor('gamepad','start')]).then(function(){ polling=setInterval(pollIMU,60); requestAnimationFrame(loop); }); var chk=document.getElementById('chkCamFollow'); if(chk && chk.checked){ CAM_FOLLOW=true; } if(USE_GAMEPAD){ startGamepadPolling(); } updateHudInput(); }
function stopGame(){ running=false; if(polling){ try{ clearInterval(polling); }catch(_){} polling=null; } stopGamepadPolling(); controlSensor('imu','stop'); }
function fwDebugOn(){ FW_DEBUG=true; var cmds=['debugsensorsgeneral 1','debugimudata 1']; Promise.all(cmds.map(function(c){ return fetch('/api/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(c)}).then(function(r){return r.text();}).catch(function(_){return '';}); })).then(function(){ startLogPoller(); }); }
function fwDebugOff(){ FW_DEBUG=false; var cmds=['debugsensorsgeneral 0','debugimudata 0']; Promise.all(cmds.map(function(c){ return fetch('/api/cli',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'cmd='+encodeURIComponent(c)}).then(function(r){return r.text();}).catch(function(_){return '';}); })).then(function(){ stopLogPoller(); }); }
function startLogPoller(){ if(__gamesLogPoll){ return; } __gamesLogLastLen=0; __gamesLogPoll=setInterval(function(){ if(!FW_DEBUG) return; fetch('/api/cli/logs',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){ if(typeof t!=='string') return; var s=t; if(s.length>__gamesLogLastLen){ var delta=s.substring(__gamesLogLastLen); __gamesLogLastLen=s.length; var lines=delta.split(/\\n/); for(var i=0;i<lines.length;i++){ var ln=lines[i]; if(!ln) continue; if(ln.indexOf('[DEBUG_SENSORS')>=0 || /IMU|BNO|ori|gyro|accel/i.test(ln)){ try{ console.log('[FW]', ln); }catch(_){ } } } } }).catch(function(_){ }); }, 800); }
function stopLogPoller(){ if(__gamesLogPoll){ try{ clearInterval(__gamesLogPoll); }catch(_){} __gamesLogPoll=null; } }
document.getElementById('btnStart').addEventListener('click', startGame);
document.getElementById('btnStop').addEventListener('click', stopGame);
document.getElementById('chkImuDebug').addEventListener('change', function(){ DEBUG_IMU=this.checked; });
document.getElementById('btnFwDbgOn').addEventListener('click', fwDebugOn);
document.getElementById('btnFwDbgOff').addEventListener('click', fwDebugOff);
(function(){ var sel=document.getElementById('terrainSelect'); if(sel){ sel.value=terrain; sel.addEventListener('change', function(){ terrain=this.value; applyPreset(); }); } })();
document.getElementById('btnView2D').addEventListener('click', function(){ MODE3D=false; });
document.getElementById('btnView3D').addEventListener('click', function(){ MODE3D=true; });
document.getElementById('btnToggleTex').addEventListener('click', function(){ USE_TEXTURES=!USE_TEXTURES; this.textContent = USE_TEXTURES ? 'Disable Textures' : 'Enable Textures'; });
document.getElementById('chk3dDebug').addEventListener('change', function(){ DEBUG_3D=this.checked; });
document.getElementById('chkTexDebug').addEventListener('change', function(){ DEBUG_TEXTURE=this.checked; });
document.getElementById('chkFloorDebug').addEventListener('change', function(){ DEBUG_FLOOR=this.checked; });
document.getElementById('chkDecorDebug').addEventListener('change', function(){ DEBUG_DECORATIONS=this.checked; });
document.getElementById('chkCamFollow').addEventListener('change', function(){ CAM_FOLLOW=this.checked; });
(function(){ var cg=document.getElementById('chkGamepad'); if(cg){ cg.addEventListener('change', function(){ USE_GAMEPAD=this.checked; if(running){ if(USE_GAMEPAD){ startGamepadPolling(); } else { stopGamepadPolling(); } } updateHudInput(); }); } })();
var chkGamepad=document.getElementById('chkGamepad'); if(chkGamepad){ chkGamepad.checked=true; USE_GAMEPAD=true; }
var chkTextures=document.getElementById('btnToggleTex'); if(chkTextures){ USE_TEXTURES=true; chkTextures.textContent='Disable Textures'; }
var chk3dDebug=document.getElementById('chk3dDebug'); if(chk3dDebug){ chk3dDebug.checked=false; DEBUG_3D=false; }
var chkTexDebug=document.getElementById('chkTexDebug'); if(chkTexDebug){ chkTexDebug.checked=true; DEBUG_TEXTURE=true; }
var chkFloorDebug=document.getElementById('chkFloorDebug'); if(chkFloorDebug){ chkFloorDebug.checked=false; DEBUG_FLOOR=false; }
var chkDecorDebug=document.getElementById('chkDecorDebug'); if(chkDecorDebug){ chkDecorDebug.checked=true; DEBUG_DECORATIONS=true; }
var chkCamFollow=document.getElementById('chkCamFollow'); if(chkCamFollow){ chkCamFollow.checked=true; CAM_FOLLOW=true; }
checkSensorAvailability();
</script>
)JS", HTTPD_RESP_USE_STRLEN);

  // Close games-wrap
  httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
}

void registerGamesHandlers(httpd_handle_t server);

#else

inline void streamGamesInner(httpd_req_t* req) { (void)req; }
inline void registerGamesHandlers(httpd_handle_t server) { (void)server; }

#endif

#endif  // WEB_GAMES_H
