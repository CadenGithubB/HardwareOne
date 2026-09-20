// Run the actual corner matcher without a browser or optional audit lab.
(function () {
  var G = (0, eval)('this');
  (0, eval)(slurp('assets/games/src/13-render-floors-ceilings.js'));
  var n = 0;
  function check(name, okay) {
    if (!okay) throw new Error(name);
    n++; __out('PASS ' + name);
  }
  function mesh(a, b, surfaceA, surfaceB) {
    var m = {layerCount: [a.length, b.length], surfaceH: [surfaceA, surfaceB], ceilAboveMask: [0, 0]};
    for (var k = 0; k < 5; k++) {
      m['l' + k + 'Type'] = [a[k] ? a[k][0] : 0, b[k] ? b[k][0] : 0];
      m['l' + k + 'TopZ'] = [a[k] ? a[k][1] : 0, b[k] ? b[k][1] : 0];
      m['l' + k + 'Meta'] = [a[k] ? a[k][2] : 0, b[k] ? b[k][2] : 0];
    }
    return m;
  }
  var m = mesh([[1, -5, 2], [2, -1, 0], [4, 1, 3]], [[1, -5, 2], [2, -1, 0], [4, 1.2, 3]], 1, 1.2);
  check('cave floor keeps its semantic stratum', G.findFloorRenderRoleZ(m, 1, 2, -5, 0) === -5);
  check('cap keeps its semantic stratum', G.findFloorRenderRoleZ(m, 1, 3, 1, 0) === 1.2);
  m = mesh([[4, 1, 3]], [[1, -5, 1]], 1, 1);
  check('roof cannot stitch down to depressed approach', Number.isNaN(G.findFloorRenderRoleZ(m, 1, 3, 1, 0)));
  check('approach cannot stitch up to roof', Number.isNaN(G.findFloorRenderRoleZ(m, 0, 1, -5, 1)));
  m = mesh([[4, 1, 3]], [[1, 1.2, 1]], 1, 1.2);
  check('cap perimeter preserves continuous exterior', G.findFloorRenderRoleZ(m, 1, 3, 1, 0) === 1.2);
  check('exterior joins cap in reverse direction', G.findFloorRenderRoleZ(m, 0, 1, 1.2, 1) === 1);
  m = mesh([[1, -5, 1]], [[1, -5, 2]], 0, 0);
  check('open approach joins coplanar cave floor', G.findFloorRenderRoleZ(m, 1, 1, -5, 0) === -5);
  check('cave floor joins approach in reverse', G.findFloorRenderRoleZ(m, 0, 2, -5, 1) === -5);
  m = mesh([[1, -5, 2]], [[1, -5, 2], [3, -4.8, 4]], 1, 1);
  check('nearby ledge never steals a cave floor corner', G.findFloorRenderRoleZ(m, 1, 2, -4.9, 0) === -5);
  m = mesh([[4, 0, 0]], [[1, 0, 0]], 0, 0);
  check('legacy cap role remains distinct', G.getFloorRenderLayerRole(m, 0, 0, 4) === 3);
  m.ceilAboveMask[1] = 1;
  check('legacy covered ground resolves as cave floor', G.getFloorRenderLayerRole(m, 1, 0, 1) === 2);
  m = mesh([[1, -5, 2]], [], 0, 0);
  check('absent neighbor never fabricates a corner', Number.isNaN(G.findFloorRenderRoleZ(m, 1, 2, -5, 0)));
  __out('CAVE_RENDER_RESULT PASS ' + n);
}());
undefined;
