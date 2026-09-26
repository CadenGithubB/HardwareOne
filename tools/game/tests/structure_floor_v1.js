// Structure Shell V1 and Floor Clutter V1 must stay world anchored,
// deterministic, bounded, and compatible with the caller's distance fade.
(function () {
  var G = (0, eval)('this'), checks = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    checks++; __out('PASS ' + name);
  }
  function rounded(value) { return Math.round(value * 1000000) / 1000000; }
  function hash(value) {
    var text = JSON.stringify(value), h = 2166136261;
    for (var i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619);
    return h >>> 0;
  }

  (0, eval)(slurp('assets/games/src/01-materials.js'));
  G.cell = 24; G.CHUNK_SIZE = 768; G.CANVAS_BASE_H = 240;
  G.rgbQ = function (r, g, b) { return 'rgb(' + r + ',' + g + ',' + b + ')'; };
  G.ctx = {globalAlpha:0.37};

  var hudSource = slurp('assets/games/src/15-render-hud-menu.js');
  var shellStart = hudSource.indexOf('function getStructureShellMetrics(');
  var shellEnd = hudSource.indexOf('function drawStructures3D()', shellStart);
  check('production structure shell helpers found', shellStart >= 0 && shellEnd > shellStart);

  var shapes = [];
  function point(x, y, z) { return [rounded(x), rounded(y), rounded(z)]; }
  G.drawRuinDecorFace = function (vertices, color) {
    shapes.push({kind:'face', color:color, points:vertices.map(function (p) {
      return point(p.x, p.y, p.z);
    })});
  };
  G.drawRuinDecorBox = function (frame, z0, z1, halfRight, halfOut, colors) {
    var rx=frame.rightX*halfRight, ry=frame.rightY*halfRight;
    var ox=frame.outX*halfOut, oy=frame.outY*halfOut;
    shapes.push({kind:'box', colors:colors, points:[
      point(frame.x-rx-ox,frame.y-ry-oy,z0), point(frame.x+rx-ox,frame.y+ry-oy,z0),
      point(frame.x+rx+ox,frame.y+ry+oy,z0), point(frame.x-rx+ox,frame.y-ry+oy,z0),
      point(frame.x-rx-ox,frame.y-ry-oy,z1), point(frame.x+rx-ox,frame.y+ry-oy,z1),
      point(frame.x+rx+ox,frame.y+ry+oy,z1), point(frame.x-rx+ox,frame.y-ry+oy,z1)
    ]});
  };
  (0, eval)(hudSource.slice(shellStart, shellEnd));

  var palette={w:[140,135,125],k:[150,145,135],p:[100,60,45],t:[120,130,155],a:[110,120,140]};
  function render(st) {
    shapes=[]; G.ctx.globalAlpha=0.37;
    G.drawStructureShellWorld(st,{floorZ:75},{});
    return JSON.parse(JSON.stringify(shapes));
  }
  var fortress={type:'fortress',x:1200,y:-300,scale:1.1,rotation:0.2,palette:palette,numBuildings:2};
  var arena={type:'arena',x:1200,y:-300,scale:1.0,rotation:0.35,palette:palette,numPillars:12};
  var tower={type:'watchtower',x:1200,y:-300,scale:1.0,rotation:0.1,palette:palette,armCount:3};
  var fortressShapes=render(fortress), arenaShapes=render(arena), towerShapes=render(tower);
  function allAboveFloor(list) {
    return list.length > 0 && list.every(function (shape) {
      return shape.points.every(function (p) { return p[2] >= 75; });
    });
  }
  check('all structure shell geometry is anchored above the sampled floor',
    allAboveFloor(fortressShapes) && allAboveFloor(arenaShapes) && allAboveFloor(towerShapes));
  check('fortress arena and watchtower have distinct silhouettes',
    fortressShapes.length !== arenaShapes.length && arenaShapes.length !== towerShapes.length &&
    hash(fortressShapes) !== hash(arenaShapes) && hash(arenaShapes) !== hash(towerShapes));
  check('structure shell command counts are deliberately bounded',
    fortressShapes.length < 80 && arenaShapes.length < 40 && towerShapes.length < 40);
  check('fixed structure inputs produce deterministic world geometry',
    hash(fortressShapes) === hash(render(fortress)) && hash(arenaShapes) === hash(render(arena)));
  var arenaRotated=Object.assign({},arena,{rotation:arena.rotation+0.4});
  var towerRotated=Object.assign({},tower,{rotation:tower.rotation+0.4});
  check('arena and watchtower metadata rotates their authored shell pieces',
    hash(arenaShapes) !== hash(render(arenaRotated)) && hash(towerShapes) !== hash(render(towerRotated)));
  check('opaque shell geometry resets alpha before writing scene depth', G.ctx.globalAlpha === 1);
  check('obsolete screen-space gazebo and banner arrays are removed',
    hudSource.indexOf('Gazebo / greek temple roof') < 0 && hudSource.indexOf('bannerPositions') < 0);

  var endlessSource = slurp('assets/games/src/05-endless-world.js');
  check('active structures preserve generator metadata needed by their shells',
    /scale:\s*_st\.scale/.test(endlessSource) && /rotation:\s*_st\.rotation/.test(endlessSource) &&
    /palette:\s*_st\.palette/.test(endlessSource) && /numBuildings:\s*_st\.numBuildings/.test(endlessSource) &&
    /numPillars:\s*_st\.numPillars/.test(endlessSource) && /armCount:\s*_st\.armCount/.test(endlessSource));

  G.WORLD_SEED = 314159; G.terrain = 'ground';
  (0, eval)(endlessSource);
  (0, eval)(slurp('assets/games/src/07-decorations-lighting.js'));
  var weights={}, deterministic=true;
  for (var wy=-12;wy<=12;wy++) for (var wx=-12;wx<=12;wx++) {
    var a=G.floorScatterClusterWeight(wx,wy), b=G.floorScatterClusterWeight(wx,wy);
    deterministic = deterministic && a === b;
    weights[a] = true;
  }
  check('scatter clustering is stable and includes dense sparse and quiet patches',
    deterministic && weights[2.5] && weights[0.75] && weights[0.25] && Object.keys(weights).length === 3);
  check('structures use thematic clutter profiles instead of the biome pool',
    G.getFloorScatterProfile('forest','fortress').key === 'fortress' &&
    G.getFloorScatterProfile('ice','arena').key === 'arena' &&
    G.getFloorScatterProfile('plains','watchtower').key === 'watchtower');
  var zoneBase={centerWX:0,centerWY:0,scale:1};
  check('structure clutter leaves traversal cores clear and dresses outer courts',
    G.getStructureFloorScatterZone(Object.assign({type:'fortress'},zoneBase),0,0) === 1 &&
    G.getStructureFloorScatterZone(Object.assign({type:'fortress'},zoneBase),400,0) === 2 &&
    G.getStructureFloorScatterZone(Object.assign({type:'arena'},zoneBase),0,0) === 1 &&
    G.getStructureFloorScatterZone(Object.assign({type:'arena'},zoneBase),400,0) === 2 &&
    G.getStructureFloorScatterZone(Object.assign({type:'watchtower'},zoneBase),0,0) === 1 &&
    G.getStructureFloorScatterZone(Object.assign({type:'watchtower'},zoneBase),200,0) === 2);

  var calls=[];
  function makeContext(alpha) {
    var target={globalAlpha:alpha};
    return new Proxy(target, {
      set:function (obj,key,value) { obj[key]=value; calls.push(['set',key,value]); return true; },
      get:function (obj,key) {
        if (key in obj) return obj[key];
        return function () { calls.push([key].concat(Array.prototype.slice.call(arguments))); };
      }
    });
  }
  var allTypes=[], seen={};
  Object.keys(G.FLOOR_SCATTER_POOLS).forEach(function (key) {
    G.FLOOR_SCATTER_POOLS[key].forEach(function (type) {
      if (!seen[type]) { seen[type]=true; allTypes.push(type); }
    });
  });
  var rendered=true, faded=true, bounded=true, maxCommands=0, hashes={};
  allTypes.forEach(function (type) {
    calls=[]; var local=makeContext(0.2);
    G.paintFloorItem(local,type,1,0.371,120,80,24);
    var snapshot=JSON.parse(JSON.stringify(calls)); hashes[type]=hash(snapshot);
    rendered = rendered && snapshot.some(function (call) {
      return call[0] === 'fill' || call[0] === 'stroke' || call[0] === 'fillRect';
    });
    faded = faded && snapshot.filter(function (call) {
      return call[0] === 'set' && call[1] === 'globalAlpha' && typeof call[2] === 'number';
    }).every(function (call) { return call[2] <= 0.2000001; });
    maxCommands = Math.max(maxCommands, snapshot.length);
    bounded = bounded && snapshot.length < 420;
  });
  check('every generated floor clutter type has visible drawing commands', rendered && allTypes.length >= 30);
  check('every floor recipe preserves the parent fog and lighting alpha', faded);
  check('floor clutter drawing recipes remain bounded at ' + maxCommands + ' commands or fewer', bounded);
  calls=[]; G.paintFloorItem(makeContext(0.2),'barrel',1,0.371,120,80,24);
  check('fixed floor prop inputs produce deterministic Canvas commands', hashes.barrel === hash(calls));
  calls=[]; G.drawFloorItemContactShadow(makeContext(0.2),'barrel',120,80,24);
  var raisedShadow=calls.some(function (call) { return call[0] === 'ellipse'; });
  calls=[]; G.drawFloorItemContactShadow(makeContext(0.2),'puddle',120,80,24);
  check('only raised clutter receives a restrained contact shadow', raisedShadow && calls.length === 0);
  var configSource=slurp('assets/games/src/01-config-state.js');
  check('every generated floor clutter type has an explicit perspective tier', allTypes.every(function (type) {
    return new RegExp('(?:^|[\\s,])' + type + '\\s*:','m').test(configSource);
  }));
  check('structure paving is baked during generation instead of drawn every frame',
    endlessSource.indexOf('Structure-specific paving is baked into the chunk color mesh') >= 0 &&
    hudSource.indexOf('_paverX') < 0);

  __out('STRUCTURE_FLOOR_V1_RESULT PASS ' + checks);
}());
undefined;
