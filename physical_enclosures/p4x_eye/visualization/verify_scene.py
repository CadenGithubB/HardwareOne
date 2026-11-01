"""Check saved production scene independently of the scene builder's checks."""
import json
import math
from pathlib import Path
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from bpy_extras.object_utils import world_to_camera_view

OUT = Path(bpy.data.filepath).parent
scene = bpy.data.scenes['01 Studio • animated assembly']
bpy.context.window.scene = scene
scene.frame_set(1)
bpy.context.view_layer.update()
depsgraph = bpy.context.evaluated_depsgraph_get()
checks = []
for label, x, y, expected_prefix, z in [
        ('LCD face', 30, 26.86, 'UI', 29.015),
        ('Upper key', 51.5, 31, 'R26 • button strip', 30),
        ('Middle key', 51.5, 25, 'R26 • button strip', 30),
        ('Lower key', 51.5, 19, 'R26 • button strip', 30),
        ('Power cap', 64.0732, 11.2, 'R26 • power slider', 30),
        ('Lid roof', 10, 25, 'R26 • lid', 29)]:
    hit, loc, normal, index, obj, matrix = scene.ray_cast(
        depsgraph, Vector((x * .001, y * .001, .2)), Vector((0, 0, -1)))
    assert hit, label
    if label == 'LCD face':
        assert obj.name.startswith(('LCD', 'UI')), (label, obj.name)
    else:
        assert obj.name.startswith(expected_prefix), (label, obj.name)
    assert abs(loc.z * 1000 - z) < .12, (label, loc.z)
    checks.append({'probe': label, 'object': obj.name, 'surface_z_mm': loc.z * 1000})

# At full explosion, all major layers retain their intended order and offsets.
scene.frame_set(196)
bpy.context.view_layer.update()
for name, expected in [('00 Base + hardware', 0), ('01 Battery', 18),
                       ('02 Midframe', 38), ('03 PCB + components', 76),
                       ('04 Lid + controls', 111), ('05 LCD module', 129),
                       ('06 Bottom screws', -60), ('07 Midplate screws', 56), ('08 PCB screw', 94)]:
    actual = bpy.data.objects[name].location.z * 1000
    assert abs(actual - expected) < .01, (name, actual)
    checks.append({'probe': name + ' exploded lift', 'actual_mm': actual})
for frame in (1, 240, 241, 288):
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    assert all(abs(bpy.data.objects[name].location.z) < .000001 for name in (
        '00 Base + hardware', '01 Battery', '02 Midframe', '03 PCB + components',
        '04 Lid + controls', '05 LCD module', '06 Bottom screws', '07 Midplate screws', '08 PCB screw')), frame
checks.append({'probe': 'assembled at start, reassembly, final cut and end', 'passed': True})
assert len([o for o in bpy.data.objects if o.type == 'CAMERA']) >= 4
assert {'01 Studio • animated assembly', '02 Real world • oak desk'} <= set(bpy.data.scenes.keys())
# Check all assembled hero frames at the actual movie aspect ratio. Macro shots
# intentionally crop; this check concerns the full-product orbit only.
scene.render.resolution_x, scene.render.resolution_y = 1280, 720
hero = bpy.data.objects['Camera • hero orbit']
extents = []
for frame in list(range(1, 97)) + list(range(241, 289)):
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    points = []
    # Exact original STL vertices avoid overestimating the unusual outline with
    # its axis-aligned bounding box corners.
    for name in ('R26 • base', 'R26 • lid', 'R26 • button strip', 'R26 • power slider'):
        obj = bpy.data.objects[name]
        points.extend(world_to_camera_view(scene, hero, obj.matrix_world @ v.co)
                      for v in obj.data.vertices)
    bounds = (min(p.x for p in points), min(p.y for p in points),
              max(p.x for p in points), max(p.y for p in points))
    assert all(.02 < value < .98 for value in bounds), (frame, bounds)
    extents.append(bounds)
checks.append({'probe': 'all 144 hero frames have at least 2% margin', 'passed': True,
               'minimum_margin': min(min(b[0], b[1], 1-b[2], 1-b[3]) for b in extents)})
heads = [o for o in bpy.data.collections['PRODUCT • R26 matched assembly'].all_objects
         if o.get('fastener_head')]
assert len(heads) == 7
assert {name: sum(o.parent.name == name for o in heads) for name in (
    '06 Bottom screws', '07 Midplate screws', '08 PCB screw')} == {
    '06 Bottom screws': 4, '07 Midplate screws': 2, '08 PCB screw': 1}
scene.frame_set(196); bpy.context.view_layer.update()
cam = bpy.data.objects['Camera • exploded stack']
for obj in heads:
    point = obj.matrix_world.translation
    screen = world_to_camera_view(scene, cam, point)
    assert .02 < screen.x < .98 and .02 < screen.y < .98, (obj.name, tuple(screen))
    # A ray to the head centre must encounter that screw, not the case/plate.
    hit, loc, normal, index, first, matrix = scene.ray_cast(
        depsgraph, cam.matrix_world.translation, (point-cam.matrix_world.translation).normalized())
    assert hit and first.get('fastener_id') == obj.get('fastener_id'), (obj.name, first.name if hit else 'none')
checks.append({'probe': 'all seven exploded screw heads visible in camera', 'passed': True})
checks.append({'probe': 'fastener counts', 'bottom': 4, 'midplate': 2, 'PCB': 1})
scene.render.resolution_x, scene.render.resolution_y = 1400, 1800
cam.data.lens = 60
for obj in heads:
    screen = world_to_camera_view(scene, cam, obj.matrix_world.translation)
    assert .02 < screen.x < .98 and .02 < screen.y < .98, (obj.name, tuple(screen))
checks.append({'probe': 'all seven screw heads within final portrait still', 'passed': True})
scene.frame_set(1); bpy.context.view_layer.update()
for y in (1.5, 2.5, 4, 39.5, 40, 41):
    xs = []
    for z in (3, 10, 11.3, 11.7, 14.3, 14.7, 20, 25, 27):
        hit, loc, normal, index, obj, matrix = scene.ray_cast(
            depsgraph, Vector((.083, y*.001, z*.001)), Vector((-1, 0, 0)))
        assert hit and obj.name in ('R26 • base', 'R26 • lid'), (y, z, obj.name if hit else 'none')
        xs.append(loc.x*1000)
    assert max(xs)-min(xs) < .025, (y, xs)
checks.append({'probe': 'both USB corner profiles align above and below the joint and former lip', 'passed': True})
joint_x = []
for z in (14.3, 14.5, 14.7):
    hit, loc, normal, index, obj, matrix = scene.ray_cast(
        depsgraph, Vector((.083, .041, z*.001)), Vector((-1, 0, 0)))
    if z != 14.5:
        assert hit and obj.name in ('R26 • base', 'R26 • lid')
    joint_x.append(loc.x*1000 if hit else 0)
assert joint_x[1] < min(joint_x[0], joint_x[2])-.01, joint_x
checks.append({'probe': 'visible seated parting line at Z14.5', 'surface_x_mm': joint_x, 'passed': True})
for z in (19, 20, 21):
    ys = []
    for x in (71, 72, 72.9, 73.1, 74):
        hit, loc, normal, index, obj, matrix = scene.ray_cast(
            depsgraph, Vector((x*.001, .048, z*.001)), Vector((0, -1, 0)))
        assert hit and obj.name == 'R26 • lid', (x, z, obj.name if hit else 'none')
        ys.append(loc.y*1000)
    assert max(ys)-min(ys) < .025 and max(ys) < 44.525, (z, ys)
checks.append({'probe': 'wheel-side wall flush across former raised strip', 'passed': True})
# The other two screw sleeves must also follow the shell above them.
for y in (1.5,3.6,52.4,54):
    profile=[]
    for z in (3,10,11.3,11.7,14.3,14.7,20,25,27):
        hit,loc,normal,index,obj,matrix=scene.ray_cast(
            depsgraph,Vector((-.01,y*.001,z*.001)),Vector((1,0,0)))
        assert hit and obj.name in ('R26 • base','R26 • lid'),(y,z)
        profile.append(loc.x*1000)
    assert max(profile)-min(profile)<.025,(y,profile)
checks.append({'probe':'all four corner sleeves aligned to continuous exterior','passed':True})
# Invalid/zero custom normals caused the encoder-wall flashes in the old film.
for name in ('R26 • base','R26 • lid'):
    ev=bpy.data.objects[name].evaluated_get(depsgraph);mesh=ev.to_mesh()
    invalid=[i for i,n in enumerate(mesh.corner_normals)
             if not all(math.isfinite(v) for v in n.vector) or n.vector.length<.99]
    assert not invalid,(name,'invalid shaded normals',invalid[:10])
    ev.to_mesh_clear()
checks.append({'probe':'both shell meshes have finite unit shading normals','passed':True})
# Every rendered ink vertex must have PCB substrate below its XY location.
board=bpy.data.objects['PCB • MB V2.3 • 1.6 mm FR4'].evaluated_get(depsgraph)
mesh=board.to_mesh()
substrate=BVHTree.FromPolygons([board.matrix_world@v.co for v in mesh.vertices],
                              [list(p.vertices) for p in mesh.polygons])
board.to_mesh_clear()
for name in ('PCB • authentic SILK_TOP.art','PCB • authentic SILK_BOT.art'):
    ink=bpy.data.objects[name].evaluated_get(depsgraph);mesh=ink.to_mesh()
    for vertex in mesh.vertices:
        p=ink.matrix_world@vertex.co
        hit=substrate.ray_cast(Vector((p.x,p.y,.035)),Vector((0,0,-1)))[0]
        assert hit is not None,(name,'floating ink',tuple(p))
    checks.append({'probe':name+' entirely supported by actual board surface',
                   'ink_vertices_checked':len(mesh.vertices),'passed':True})
    ink.to_mesh_clear()
(OUT / 'scene-verification.json').write_text(json.dumps({'passed': True, 'checks': checks}, indent=2) + '\n')
print('Saved-scene spatial and animation checks passed', flush=True)
