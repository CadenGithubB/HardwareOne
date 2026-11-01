"""Build the P4X EYE visualization. Run with Blender --background --python.

All construction coordinates below are millimetres; stored Blender units are
metres. Original source STLs stay unchanged; visualization meshes remove the
user-identified exterior protrusions and use presentation shading.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

import bpy
import bmesh
from mathutils import Vector

HERE = Path(__file__).resolve().parent
CASE = HERE.parent
OUT = HERE / 'output'
MM = .001
parser = argparse.ArgumentParser()
parser.add_argument('--preview', action='store_true')
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
for col in list(bpy.data.collections):
    if col.users == 0:
        bpy.data.collections.remove(col)
scene = bpy.context.scene
scene.name = '01 Studio • animated assembly'
product = bpy.data.collections.new('PRODUCT • R26 matched assembly')
scene.collection.children.link(product)
current_collection = product
current_parent = None


def link(obj):
    for col in list(obj.users_collection):
        col.objects.unlink(obj)
    current_collection.objects.link(obj)
    if current_parent:
        obj.parent = current_parent
    return obj


def material(name, color, rough=.4, metal=0, coat=0):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*color, 1)
    mat.use_nodes = True
    bs = mat.node_tree.nodes.get('Principled BSDF')
    bs.inputs['Base Color'].default_value = (*color, 1)
    bs.inputs['Roughness'].default_value = rough
    bs.inputs['Metallic'].default_value = metal
    bs.inputs['Coat Weight'].default_value = coat
    return mat


black = material('Case • satin black resin', (.012, .014, .017), .32, coat=.18)
n = black.node_tree.nodes
noise = n.new('ShaderNodeTexNoise')
noise.inputs['Scale'].default_value = 1800
noise.inputs['Detail'].default_value = 2
bump = n.new('ShaderNodeBump')
bump.inputs['Strength'].default_value = .085
bump.inputs['Distance'].default_value = .000018
black.node_tree.links.new(noise.outputs['Fac'], bump.inputs['Height'])
black.node_tree.links.new(bump.outputs['Normal'], n.get('Principled BSDF').inputs['Normal'])
rubber = material('Controls • charcoal polymer', (.019, .021, .024), .47)
pcbmat = material('PCB • dark green solder mask', (.009, .054, .034), .38, coat=.25)
edge = material('PCB • exposed FR4', (.19, .15, .063), .65)
steel = material('Connectors • brushed stainless steel', (.43, .48, .53), .27, .92)
solder = material('Solder • tin', (.55, .59, .62), .3, .85)
gold = material('Contacts • gold plating', (.68, .40, .10), .24, .85)
brass = material('Inserts • brass', (.48, .27, .075), .3, .82)
chip = material('IC • epoxy', (.014, .017, .021), .63)
ceramic = material('MLCC • tan ceramic', (.32, .24, .15), .52)
white = material('Silkscreen • off-white', (.74, .78, .72), .67)
kapton = material('Flex / insulation • amber polyimide', (.47, .17, .017), .31, .12)
foil = material('Battery • aluminium laminate', (.49, .53, .57), .3, .76)
labelmat = material('Battery label • warm white', (.78, .79, .74), .65)
glass = material('LCD • cover glass', (.005, .009, .013), .13, .15, .55)
red = material('Battery lead • red silicone', (.24, .009, .006), .5)
darkwire = material('Battery lead • black silicone', (.012, .013, .015), .5)
screenmat = material('LCD • black IPS background', (.002, .008, .013), .22, coat=.3)
teal = material('LCD • cyan pixels', (.04, .55, .52), .35)
bs = teal.node_tree.nodes.get('Principled BSDF')
bs.inputs['Emission Color'].default_value = (.04, .65, .58, 1)
bs.inputs['Emission Strength'].default_value = .7
screenwhite = material('LCD • white pixels', (.66, .81, .81), .4)
bs = screenwhite.node_tree.nodes.get('Principled BSDF')
bs.inputs['Emission Color'].default_value = (.66, .81, .81, 1)
bs.inputs['Emission Strength'].default_value = .45


def finish(obj, mat, bevel=0, smooth=False):
    obj.data.materials.append(mat)
    if bevel:
        mod = obj.modifiers.new('Light-catching edge • visual only', 'BEVEL')
        mod.width = bevel * MM
        mod.segments = 3
        mod.limit_method = 'ANGLE'
    if smooth:
        for poly in obj.data.polygons:
            poly.use_smooth = True
        mod = obj.modifiers.new('Weighted corner normals', 'WEIGHTED_NORMAL')
        mod.keep_sharp = True
    return obj


def box(name, center, size, mat, bevel=.08):
    verts = [(sx * size[0] * MM / 2, sy * size[1] * MM / 2, sz * size[2] * MM / 2)
             for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4),
                                (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)])
    obj = bpy.data.objects.new(name, mesh)
    link(obj)
    obj.location = Vector(center) * MM
    return finish(obj, mat, bevel)


def cylinder(name, center, radius, depth, mat, vertices=48, bevel=.04):
    verts = [(radius * MM * math.cos(i * math.tau / vertices),
              radius * MM * math.sin(i * math.tau / vertices), z * MM)
             for z in (-depth / 2, depth / 2) for i in range(vertices)]
    faces = [tuple(range(vertices - 1, -1, -1)), tuple(range(vertices, vertices * 2))]
    faces += [(i, (i + 1) % vertices, (i + 1) % vertices + vertices, i + vertices)
              for i in range(vertices)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    obj = bpy.data.objects.new(name, mesh)
    link(obj)
    obj.location = Vector(center) * MM
    return finish(obj, mat, bevel, True)


def usb_shell(y):
    # Drawn overall envelope: 7.35 × 8.94 × 3.26 mm. The actual mouth has
    # rounded USB-C ends, unlike the SCAD reference's simple rectangular void.
    profiles = []
    for width, height, radius in ((8.94, 3.26, 1.4), (8.38, 2.70, 1.12)):
        points = []
        for quadrant in range(4):
            cy = (width / 2 - radius) * (1 if quadrant in (0, 3) else -1)
            cz = (height / 2 - radius) * (1 if quadrant < 2 else -1)
            for step in range(9):
                angle = (quadrant * 90 + step * 90 / 8) * math.pi / 180
                points.append((cy + radius * math.cos(angle), cz + radius * math.sin(angle)))
        profiles.append(points)
    n = len(profiles[0])
    verts = [(x * MM, (y + py) * MM, (22.93 + pz) * MM)
             for x in (68.65, 76) for profile in profiles for py, pz in profile]
    faces = []
    for i in range(n):
        j = (i + 1) % n
        faces += [(i, j, j + 2*n, i + 2*n),
                  (i + n, i + 3*n, j + 3*n, j + n),
                  (i, i + n, j + n, j),
                  (i + 2*n, j + 2*n, j + 3*n, i + 3*n)]
    mesh = bpy.data.meshes.new('USB-C • capsule shell')
    mesh.from_pydata(verts, [], faces)
    obj = bpy.data.objects.new('USB-C • rolled capsule shell', mesh)
    link(obj)
    bm = bmesh.new()
    bm.from_mesh(mesh)
    for e in bm.edges:
        e.smooth = len(e.link_faces) == 2 and e.calc_face_angle(0) < math.radians(30)
    bm.to_mesh(mesh)
    bm.free()
    finish(obj, steel, .025, True)
    return obj


def text_obj(name, body, xy, z, size, mat, rotation=0):
    data = bpy.data.curves.new(name, 'FONT')
    data.body = body
    data.size = size * MM
    data.space_character = 1.15
    obj = bpy.data.objects.new(name, data)
    current_collection.objects.link(obj)
    obj.parent = current_parent
    obj.location = (xy[0] * MM, xy[1] * MM, z * MM)
    obj.rotation_euler.z = rotation
    data.materials.append(mat)
    return obj


def curve(name, points, radius, mat):
    data = bpy.data.curves.new(name, 'CURVE')
    data.dimensions = '3D'
    data.bevel_depth = radius * MM
    data.bevel_resolution = 2
    spl = data.splines.new('BEZIER')
    spl.bezier_points.add(len(points) - 1)
    for p, co in zip(spl.bezier_points, points):
        p.co = Vector(co) * MM
        p.handle_left_type = p.handle_right_type = 'AUTO'
    obj = bpy.data.objects.new(name, data)
    current_collection.objects.link(obj)
    obj.parent = current_parent
    data.materials.append(mat)
    return obj


def group(name, lift):
    obj = bpy.data.objects.new(name, None)
    product.objects.link(obj)
    obj['exploded_lift_mm'] = lift
    for frame, amount in [(1, 0), (144, 0), (184, lift), (208, lift), (240, 0), (288, 0)]:
        obj.location.z = amount * MM
        obj.keyframe_insert(data_path='location', frame=frame)
    return obj


groups = {name: group(name, lift) for name, lift in [
    ('00 Base + hardware', 0), ('01 Battery', 18), ('02 Midframe', 38),
    ('03 PCB + components', 76), ('04 Lid + controls', 111), ('05 LCD module', 129),
    ('06 Bottom screws', -60), ('07 Midplate screws', 56), ('08 PCB screw', 94)]}
manifest = json.loads((CASE / 'stl/export-manifest-r26.json').read_text())
checks = []
for part in manifest['parts']:
    path = CASE / 'stl' / part['file']
    assert hashlib.sha256(path.read_bytes()).hexdigest() == part['sha256'], path
    name = part['part']
    current_parent = groups['00 Base + hardware' if name == 'base' else
                            '02 Midframe' if name == 'midframe' else '04 Lid + controls']
    bpy.ops.wm.stl_import(filepath=str(path), global_scale=MM)
    obj = link(bpy.context.object)
    obj.name = 'R26 • ' + name.replace('_', ' ')
    # Blender's STL importer puts global_scale on the object, not the vertices.
    # Bake our explicit mm-to-m conversion after undoing the print transforms.
    obj.scale = (1, 1, 1)
    trans = part['post_export_translation_mm']
    for vertex in obj.data.vertices:
        x, y, z = [vertex.co[a] - trans[a] for a in range(3)]
        if name == 'lid':
            y, z = 56 - y, 29 - z
        elif name == 'power_slider':
            y, z = -y, -z
            x += 1  # documented assembly offset; export uses cap at zero travel
        vertex.co = (x * MM, y * MM, z * MM)
    obj['source_file'] = str(path.relative_to(CASE))
    obj['source_sha256'] = part['sha256']
    obj['geometry_note'] = 'Original mesh; inverse print transform only; bevel modifier is visual.'
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    for e in bm.edges:
        e.smooth = len(e.link_faces) == 2 and e.calc_face_angle(0) < math.radians(30)
    bm.to_mesh(obj.data)
    bm.free()
    finish(obj, rubber if name in ('button_strip', 'power_slider') else black, .045, True)
    obj.data.update()
    # Keep broad horizontal CAD faces mathematically flat. Smoothing them into
    # the chamfer produces visible triangular reflection patches after STL
    # triangulation; only curved/vertical/chamfer faces need smooth shading.
    for poly in obj.data.polygons:
        if abs(poly.normal.z) > .99999:
            poly.use_smooth = False
    bounds = [[min(v.co[a] / MM for v in obj.data.vertices) for a in range(3)],
              [max(v.co[a] / MM for v in obj.data.vertices) for a in range(3)]]
    assert all(-.01 <= value <= 81 for row in bounds for value in row), (name, bounds)
    if name == 'lid':
        assert abs(bounds[1][2] - 29) < .01, bounds
    if name == 'button_strip':
        assert abs(bounds[1][2] - 30) < .01, bounds
    checks.append({'part': name, 'sha256_verified': True, 'assembly_bounds_mm': bounds})


def clean_corner_exterior():
    """Trim cosmetic protrusions to the continuous roof outline, keeping cavities.

    A solid roof section below the face chamfer gives the smooth outer perimeter.
    Only the visualization meshes change; original release STLs stay intact.
    """
    lid = bpy.data.objects['R26 • lid']
    segments = []
    buckets = {}
    tolerance = 1e-7  # 0.1 micrometre weld for float32 section intersections
    def snap(point):
        cell = (math.floor(point.x/tolerance), math.floor(point.y/tolerance))
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for candidate in buckets.get((cell[0]+dx, cell[1]+dy), []):
                    if math.hypot(point.x-candidate[0], point.y-candidate[1]) < tolerance:
                        return candidate
        result = (point.x, point.y)
        buckets.setdefault(cell, []).append(result)
        return result
    # Avoid an exact construction Z plane, where coplanar edges are ambiguous.
    level = 27.6317 * MM
    for poly in lid.data.polygons:
        vertices = [lid.data.vertices[i].co for i in poly.vertices]
        cuts = []
        for a, b in zip(vertices, vertices[1:] + vertices[:1]):
            if (a.z - level) * (b.z - level) < 0:
                p = a.lerp(b, (level - a.z) / (b.z - a.z))
                cuts.append(snap(p))
        if len(cuts) == 2 and cuts[0] != cuts[1]:
            segments.append(cuts)
    adjacent = {}
    for a, b in segments:
        adjacent.setdefault(a, set()).add(b)
        adjacent.setdefault(b, set()).add(a)
    assert all(len(v) == 2 for v in adjacent.values()), [(p, len(v)) for p, v in adjacent.items() if len(v) != 2][:8]
    remaining = set(adjacent)
    loops = []
    while remaining:
        start = next(iter(remaining))
        loop, previous, point = [], None, start
        while point not in loop:
            loop.append(point)
            remaining.discard(point)
            following = next(p for p in adjacent[point] if p != previous)
            previous, point = point, following
        loops.append(loop)
    def area(points):
        return abs(sum(a[0]*b[1] - b[0]*a[1]
                       for a, b in zip(points, points[1:] + points[:1])))
    perimeter = max(loops, key=area)
    n = len(perimeter)
    mesh = bpy.data.meshes.new('Cosmetic • smooth roof envelope')
    mesh.from_pydata([(x, y, z * MM) for z in (-5, 35) for x, y in perimeter], [],
                    [tuple(range(n-1, -1, -1)), tuple(range(n, 2*n))] +
                    [(i, (i+1)%n, (i+1)%n+n, i+n) for i in range(n)])
    bm = bmesh.new(); bm.from_mesh(mesh)
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(mesh); bm.free()
    envelope = bpy.data.objects.new('temporary cosmetic envelope', mesh)
    product.objects.link(envelope)
    for name in ('R26 • base', 'R26 • lid'):
        obj = bpy.data.objects[name]
        for mod in list(obj.modifiers):
            obj.modifiers.remove(mod)
        # Match the complete lower and upper exterior to one continuous roof
        # footprint, including all four sleeve corners. Cavities remain intact.
        operations = [('INTERSECT', envelope)]
        for operation, operand in operations:
            mod = obj.modifiers.new('Trim unintended exterior ridges', 'BOOLEAN')
            mod.operation, mod.solver, mod.object = operation, 'EXACT', operand
            bpy.context.view_layer.objects.active = obj
            bpy.ops.object.modifier_apply(modifier=mod.name)
        # Explicitly break the two outer mating edges by 0.035 mm. Interior
        # seating faces remain at Z14.5; only the perimeter gets a fine V seam.
        # This avoids tiny STL edges clamping the presentation bevel to zero.
        def on_outer_perimeter(vertex):
            px, py = vertex.co.x, vertex.co.y
            for a, b in zip(perimeter, perimeter[1:] + perimeter[:1]):
                dx, dy = b[0]-a[0], b[1]-a[1]
                length2 = dx*dx+dy*dy
                if not length2:
                    continue
                t = max(0, min(1, ((px-a[0])*dx+(py-a[1])*dy)/length2))
                if math.hypot(px-a[0]-t*dx, py-a[1]-t*dy) < .12*MM:
                    return True
            return False
        for vertex in obj.data.vertices:
            if abs(vertex.co.z-14.5*MM) < 1e-7 and on_outer_perimeter(vertex):
                vertex.co.z += (-.035 if name == 'R26 • base' else .035)*MM
        bm = bmesh.new(); bm.from_mesh(obj.data)
        bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=1e-7)
        bmesh.ops.dissolve_degenerate(bm, edges=list(bm.edges), dist=1e-8)
        bmesh.ops.dissolve_limit(bm, angle_limit=1e-5, use_dissolve_boundaries=False,
                                 verts=list(bm.verts), edges=list(bm.edges))
        bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
        bm.edges.ensure_lookup_table(); bm.edges.index_update()
        for e in bm.edges:
            e.smooth = len(e.link_faces) == 2 and e.calc_face_angle(0) < math.radians(30)
        bm.to_mesh(obj.data); bm.free()
        obj.data.update()
        for p in obj.data.polygons:
            p.use_smooth = abs(p.normal.z) < .99999
        # The CAD already supplies the 1 mm face chamfer and the explicit
        # mating-edge break above supplies the seam. A second bevel on tiny STL
        # junctions produced degenerate faces/NaN normals, so shade these directly.
        mod = obj.modifiers.new('Weighted corner normals', 'WEIGHTED_NORMAL')
        mod.keep_sharp = True
        obj['geometry_note'] = 'R26 source clipped to smooth roof envelope for user-requested cosmetic cleanup.'
    bpy.data.objects.remove(envelope, do_unlink=True)


clean_corner_exterior()
for entry in checks:
    obj = bpy.data.objects['R26 • ' + entry['part'].replace('_', ' ')]
    entry['cosmetic_assembly_bounds_mm'] = [
        [min(v.co[a]/MM for v in obj.data.vertices) for a in range(3)],
        [max(v.co[a]/MM for v in obj.data.vertices) for a in range(3)]]
    for axis in range(3):
        assert entry['cosmetic_assembly_bounds_mm'][0][axis] >= entry['assembly_bounds_mm'][0][axis]-.001, entry
        assert entry['cosmetic_assembly_bounds_mm'][1][axis] <= entry['assembly_bounds_mm'][1][axis]+.001, entry

# User-specified pouch cell; cosmetic foil folds and insulation stay inside envelope.
current_parent = groups['01 Battery']
box('LiPo • 65 × 36 × 10 mm envelope', (41, 24, 7.5), (65, 36, 10), foil, 1.2)
box('LiPo • perimeter heat seal', (41, 24, 7.5), (64.9, 35.9, 1.1), foil, .5)
box('LiPo • insulated lead end', (71.9, 24, 7.5), (3, 35.7, 9.8), kapton, .5)
box('LiPo • label', (38, 24, 12.53), (44, 25, .035), labelmat, .1)
text_obj('Battery label • title', 'RECHARGEABLE', (19, 30), 12.56, 2, chip)
text_obj('Battery label • chemistry', 'Li-ion Polymer  /  3.7 V', (19, 26), 12.56, 1.6, chip)
text_obj('Battery label • dimensions', '65 x 36 x 10 mm', (19, 21), 12.56, 1.5, chip)
text_obj('Battery label • caution', 'HANDLE WITH CARE', (19, 16), 12.56, 1.3, chip)
for offset, mat in [(0, red), (1.2, darkwire)]:
    curve('LiPo • silicone lead', [(72, 16 + offset, 9), (74, 19 + offset, 12),
          (70, 24 + offset, 15), (70.7, 24.4 + offset, 18.5)], .45, mat)

# Exact sourced board outline, mirrored into enclosure coordinates.
current_parent = groups['03 PCB + components']
source = (CASE / 'enclosure.scad').read_text()
outline_text = source.split('pcb_outline = ')[1].split(';')[0]
outline = json.loads(re.sub(r'//[^\n]*', '', outline_text))
outline = [(74.5 - p[0], 6.5 + p[1]) for p in outline]
verts = [(x * MM, y * MM, z * MM) for z in (19.7, 21.3) for x, y in outline]
n = len(outline)
faces = [tuple(range(n - 1, -1, -1)), tuple(range(n, n * 2))]
faces += [(i, (i + 1) % n, (i + 1) % n + n, i + n) for i in range(n)]
mesh = bpy.data.meshes.new('MB V2.3 sourced outline')
mesh.from_pydata(verts, [], faces)
mesh.update()
obj = bpy.data.objects.new('PCB • MB V2.3 • 1.6 mm FR4', mesh)
product.objects.link(obj)
obj.parent = current_parent
obj.data.materials.append(pcbmat)
obj.data.materials.append(edge)
for p in obj.data.polygons:
    p.material_index = 0 if p.index < 2 else 1
finish(obj, pcbmat, .035)
# Source outline is clockwise after mirroring; recalculate normals.
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.normals_make_consistent(inside=False)
bpy.ops.object.mode_set(mode='OBJECT')
obj.select_set(False)


def silk_on_board(px, py):
    """Keep the whole 0.07 mm ink stroke inside the physical PCB outline."""
    if math.hypot(px-21.1, py-8.7) < 1.08:
        return False  # Through-hole drilled below; ink needs physical substrate.
    inside = False
    for (ax, ay), (bx, by) in zip(outline, outline[1:] + outline[:1]):
        dx, dy = bx-ax, by-ay
        length2 = dx*dx+dy*dy
        t = max(0, min(1, ((px-ax)*dx+(py-ay)*dy)/length2)) if length2 else 0
        if math.hypot(px-ax-t*dx, py-ay-t*dy) < .08:
            return False
        if (ay>py) != (by>py) and px < ax+(py-ay)*dx/(by-ay):
            inside = not inside
    return inside


def clipped_silk_segments(points):
    # Sample only to find edge crossings, then collapse each straight run to
    # two endpoints. The 0.08 mm inset exceeds stroke radius plus sample spacing,
    # so neither the centreline nor its rounded surface can leave the board.
    for a,b in zip(points, points[1:]):
        a=(74.5-a[0],6.5+a[1]);b=(74.5-b[0],6.5+b[1])
        steps=max(1,math.ceil(math.dist(a,b)/.05))
        run=[]
        for i in range(steps+1):
            t=i/steps;point=(a[0]+t*(b[0]-a[0]),a[1]+t*(b[1]-a[1]))
            if silk_on_board(*point):
                run.append(point)
            else:
                if len(run)>1: yield (run[0],run[-1])
                run=[]
        if len(run)>1: yield (run[0],run[-1])


def silk(filename, topface):
    """Read the official silk strokes, including modal coordinates and G02/03 arcs."""
    data = bpy.data.curves.new('Official ' + filename, 'CURVE')
    data.dimensions = '3D'
    data.bevel_depth = .035 * MM
    data.bevel_resolution = 0
    x = y = 0
    mode = 1
    count = 0
    for line in (HERE / 'assets' / filename).read_text().splitlines():
        if line.startswith(('G04', '%')):
            continue
        command = re.search(r'G0([123])', line)
        if command:
            mode = int(command[1])
        if not re.search('[XYIJ]', line):
            continue
        vals = {a: int(v) / 100000 for a, v in re.findall(r'([XYIJ])(-?\d+)', line)}
        nx, ny = vals.get('X', x), vals.get('Y', y)
        if 'D01' in line:
            points = [(x, y), (nx, ny)]
            if mode in (2, 3) and ('I' in vals or 'J' in vals):
                cx, cy = x + vals.get('I', 0), y + vals.get('J', 0)
                a, b = math.atan2(y - cy, x - cx), math.atan2(ny - cy, nx - cx)
                if mode == 3:
                    while b <= a + 1e-7:
                        b += 2 * math.pi
                else:
                    while b >= a - 1e-7:
                        b -= 2 * math.pi
                r = math.hypot(x - cx, y - cy)
                steps = max(3, int(abs(b - a) * 12))
                points = [(cx + r * math.cos(a + (b - a) * i / steps),
                           cy + r * math.sin(a + (b - a) * i / steps)) for i in range(steps + 1)]
            for segment in clipped_silk_segments(points):
                spl = data.splines.new('POLY')
                spl.points.add(1)
                for p, (px, py) in zip(spl.points, segment):
                    p.co = (px * MM, py * MM, (21.32 if topface else 19.68) * MM, 1)
                count += 1
        x, y = nx, ny
    obj = bpy.data.objects.new('PCB • authentic ' + filename, data)
    product.objects.link(obj)
    obj.parent = current_parent
    data.materials.append(white)
    return count


placements = []
special = {'J6', 'J23', 'J21', 'SW6', 'SW7', 'J22', 'J25', 'J5', 'J24', 'U11'}
for line in (HERE / 'assets/placement-v2.3.txt').read_text().splitlines()[1:]:
    fields = line.split()
    if len(fields) < 5:
        continue
    ref, sx, sy, ang = fields[:4]
    mirrored = fields[4] == 'm'
    package = fields[-1]
    x, y = 74.5 - float(sx), 6.5 + float(sy)
    face, sign = (21.3, 1) if mirrored else (19.7, -1)
    placements.append({'ref': ref, 'xy_mm': [x, y], 'display_face': mirrored, 'package': package})
    if ref in special:
        continue
    if ref.startswith(('R', 'C', 'L')):
        size = next((v for k, v in {'0201': (.6, .3, .3), '0402': (1, .5, .45),
                    '0603': (1.6, .8, .65), '0805': (2, 1.25, .8), '2520': (2.5, 2, 1.2)}.items()
                    if k in package), (1, .5, .45))
        length, width, height = size
        mat = ceramic if ref.startswith('C') else chip
        body = box(ref + ' • ' + package, (x, y, face + sign * height / 2), size, mat, .025)
        angle = -math.radians(float(ang))
        body.rotation_euler.z = angle
        for side in (-1, 1):
            dx = side * length * .42
            contact = box(ref + ' • solder termination', (x + dx * math.cos(angle),
                          y + dx * math.sin(angle), face + sign * height / 2),
                          (length * .22, width + .06, height + .025), solder, .025)
            contact.rotation_euler.z = angle
    elif ref.startswith('SW'):
        box(ref + ' • tactile switch shell', (x, y, face + sign * .65), (4.2, 3.2, 1.3), steel)
        cylinder(ref + ' • actuator', (x, y, face + sign * 1.9), 1, 1.2, rubber)
    elif ref.startswith(('U', 'Q', 'D', 'Y')):
        if ref == 'U4':
            size = (10, 10, .95)
        elif ref == 'U2':
            size = (6, 5, .8)
        elif 'SOP8' in package:
            size = (4.9, 3.9, 1.2)
        elif ref.startswith('Y'):
            size = (3.2, 1.5, .65)
        elif ref == 'U9':
            size = (3.5, 2.65, .95)
        else:
            size = (2.8, 1.7, .8)
        body = box(ref + ' • ' + package, (x, y, face + sign * size[2] / 2), size,
                   steel if ref.startswith('Y') else chip, .07)
        body.rotation_euler.z = -math.radians(float(ang))
        if ref == 'U4':
            for axis in range(2):
                for side in (-1, 1):
                    for i in range(26):
                        px, py = ((x + (i - 12.5) * .35, y + side * 5.05) if axis == 0
                                  else (x + side * 5.05, y + (i - 12.5) * .35))
                        box('U4 • exposed QFN termination', (px, py, face - .10),
                            (.19, .19, .14), solder, .01)

# Connector meshes use detailed mouths; no solid metal blocking the apertures.
for y in (17.5, 32.5):
    usb_shell(y)
    box('USB-C • dark rear cavity', (70, y, 22.93), (.6, 8.1, 2.3), chip)
    box('USB-C • central tongue', (73.3, y, 22.93), (4.4, 6.55, .65), chip, .25)
    for i in range(12):
        for z in (22.58, 23.28):
            box('USB-C • gold contact', (74.7, y + (i - 5.5) * .5, z),
                (1.8, .22, .045), gold, .015)
box('microSD • cage', (29.05, 41.38, 22.1), (14, 14, 1.6), steel, .25)
box('microSD • mouth', (29.05, 47.5, 22.2), (11.5, 2, .8), chip, .15)
for i in range(6):
    box('microSD • stamped recess', (25 + i * 1.6, 40, 22.93), (.65, 4.2, .08), chip)
for x, y, z, width in [(30, 26.86, 21.9, 8), (30, 36.96, 19, 14)]:
    box('FFC • connector body', (x, y, z), (width, 2.7, 1.2), labelmat)
    box('FFC • locking bar', (x, y + .8, z + .65), (width, .8, .4), chip)
    for i in range(int(width * 2)):
        box('FFC • pin', (x - width / 2 + .25 + .5 * i, y - 1.5, z), (.18, .6, .2), gold, .01)
box('Battery • JST socket', (71.375, 24.65, 18.4), (4.4, 4, 2.6), labelmat)
box('GPIO • 2 × 10 right-angle header', (12.35, 25, 17.4), (5, 25.4, 4.6), chip)
for i in range(10):
    for x in (11.08, 13.62):
        box('GPIO • gold contact', (x, 13.57 + 2.54 * i, 15.05), (.5, .5, 1.2), gold, .01)
box('ESP32-C6-MINI-1U • substrate', (46.1, 37.25, 18.9), (13.2, 12.5, 1.6), pcbmat)
box('ESP32-C6 • RF shield', (46.1, 37.25, 17.85), (12, 11.4, .7), steel, .2)
for x in (40.2, 52):
    for i in range(8):
        box('C6 • castellated pad', (x, 32.3 + i * 1.4, 19.25), (.7, .7, .18), gold, .04)
# PCB-bottom printed chip identification, readable when viewed from underneath.
obj = text_obj('U4 • laser marking', 'ESP32-P4', (35.6, 21.4), 18.72, 1.1, white)
obj.rotation_euler.x = math.pi
obj = text_obj('C6 • shield marking', 'ESP32-C6', (41.8, 38.5), 17.47, 1.2, chip)
obj.rotation_euler.x = math.pi

wheelxy = (62.9, 39.35)
cylinder('Encoder • knurled dial', (*wheelxy, 16.7), 7.25, 2, rubber, 96)
for i in range(72):
    a = 2 * math.pi * i / 72
    o = box('Encoder • grip rib', (wheelxy[0] + 7.15 * math.cos(a),
            wheelxy[1] + 7.15 * math.sin(a), 16.7), (.28, .55, 1.65), rubber, .08)
    o.rotation_euler.z = a
cylinder('Encoder • steel spindle', (*wheelxy, 18.4), 1.1, 1.6, steel)
box('Power • slide switch', (63.0732, 11.2, 23.05), (9, 3.5, 3.5), steel)
box('Power • switch lever', (64.0732, 11.2, 25.8), (1.5, 1.5, 2), chip)

# Seven individually identifiable fasteners, separated along their insertion axes.
nylon = material('Fasteners • natural nylon', (.62, .64, .59), .42)


def screw(name, xy, seat, diameter, length, upward, mat):
    x, y = xy
    sign = 1 if upward else -1
    core = cylinder(name + ' • threaded core', (x, y, seat + sign*length/2),
                    diameter*.40, length, mat, bevel=.025)
    core['fastener_id'] = name
    core['fastener_role'] = current_parent.name
    height = 1.3 if diameter == 2 else 1.0
    head = cylinder(name + ' • head', (x, y, seat - sign*height/2),
                    diameter, height, mat, bevel=.14)
    head['fastener_id'] = name
    head['fastener_head'] = True
    # Recess is in the accessible outer head face.
    face = seat - sign*height
    for angle in ([0] if diameter == 2 else [0, math.pi/2]):
        cutter = box('temporary fastener drive', (x, y, face + sign*.18),
                     (diameter*1.65, diameter*.28, .42), chip, 0)
        cutter.rotation_euler.z = angle
        mod = head.modifiers.new('Recessed drive', 'BOOLEAN')
        mod.operation, mod.object = 'DIFFERENCE', cutter
        bpy.context.view_layer.objects.active = head
        bpy.ops.object.modifier_apply(modifier=mod.name)
        bpy.data.objects.remove(cutter, do_unlink=True)
    # Small modeled helical ridge makes the removed screws read as fasteners.
    data = bpy.data.curves.new(name + ' • thread', 'CURVE')
    data.dimensions, data.bevel_depth, data.bevel_resolution = '3D', diameter*.05*MM, 1
    turns = (length-.3)/(.4 if diameter == 2 else .35)
    count = int(turns*20)
    spline = data.splines.new('POLY'); spline.points.add(count)
    for i, point in enumerate(spline.points):
        a = math.tau*turns*i/count
        point.co = ((x+diameter*.45*math.cos(a))*MM,
                    (y+diameter*.45*math.sin(a))*MM,
                    (seat+sign*(.15+(length-.3)*i/count))*MM, 1)
    thread = bpy.data.objects.new(name + ' • thread', data)
    link(thread); data.materials.append(mat)
    thread['fastener_id'] = name


current_parent = groups['06 Bottom screws']
for index, (x, y) in enumerate(((3.6, 3.6), (76.4, 3.6), (3.6, 52.4), (77.26, 41)), 1):
    screw(f'Bottom screw {index} • M2 × 8', (x, y), 10.2, 2, 8, True, nylon)
    current_parent = groups['04 Lid + controls']
    insert = cylinder('M2 • brass insert', (x, y, 16.1), 1.5, 3.2, brass)
    bore = cylinder('temporary insert bore', (x, y, 16.1), 1, 3.6, chip, bevel=0)
    mod = insert.modifiers.new('M2 insert bore', 'BOOLEAN')
    mod.operation, mod.object = 'DIFFERENCE', bore
    bpy.context.view_layer.objects.active = insert
    bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.data.objects.remove(bore, do_unlink=True)
    for i in range(12):
        a = i*math.tau/12
        cylinder('Insert • knurl', (x+1.44*math.cos(a), y+1.44*math.sin(a), 16.1),
                 .10, 3, brass, 8, .01)
    current_parent = groups['06 Bottom screws']
current_parent = groups['07 Midplate screws']
for index, xy in enumerate(((5, 8.7), (44, 48.5)), 1):
    screw(f'Midplate screw {index} • M2 × 8', xy, 14.5, 2, 8, False, nylon)
current_parent = groups['08 PCB screw']
screw('PCB screw • M1.6 × 5 reference', (21.1, 8.7), 21.3, 1.6, 5, False, steel)
# The drawing gives a nominal 2 mm board fixing hole and provisional M1.6 pilot.
current_parent = groups['03 PCB + components']
board = bpy.data.objects['PCB • MB V2.3 • 1.6 mm FR4']
bore = cylinder('temporary PCB mounting hole', (21.1, 8.7, 20.5), 1, 3, chip, bevel=0)
mod = board.modifiers.new('PCB mounting hole', 'BOOLEAN')
mod.operation, mod.object = 'DIFFERENCE', bore
bpy.context.view_layer.objects.active = board
bpy.ops.object.modifier_apply(modifier=mod.name)
bpy.data.objects.remove(bore, do_unlink=True)

# Flush display. UI is an editable illustrative status layout, not a firmware capture.
current_parent = groups['05 LCD module']
box('LCD • module / backlight', (30, 25.31, 28.03), (31.52, 33.72, 1.86), glass, .10)
box('LCD • 27.72 mm active area', (30, 26.86, 28.985), (27.72, 27.72, .025), screenmat, .03)
box('LCD • FPC tail', (30, 9.5, 26), (24, 6, .15), kapton)
for i in range(24):
    box('LCD • flex conductor', (18.3 + i, 9.5, 26.09), (.22, 5.5, .01), gold, 0)
text_obj('UI • device', 'HARDWARE ONE', (17.6, 37.7), 29.015, 1.38, screenwhite)
text_obj('UI • status', 'SYSTEM READY', (17.6, 34.4), 29.015, .85, teal)
box('UI • divider', (30, 32.8, 29.01), (24.5, .09, .015), teal, 0)
text_obj('UI • time', '09:41', (17.6, 26.7), 29.015, 4.2, screenwhite)
text_obj('UI • subtitle', 'CONNECTED', (17.6, 23.5), 29.015, .85, teal)
for i, body in enumerate(('NETWORK  ONLINE', 'SENSORS  ACTIVE', 'BATTERY  84%')):
    text_obj('UI • ' + body, body, (17.6, 20 - i * 2.5), 29.015, .85, screenwhite)
box('UI • battery outline', (40.8, 35.1, 29.015), (2.6, 1, .02), screenwhite, .05)
box('UI • battery fill', (40.6, 35.1, 29.035), (1.8, .6, .015), teal, .03)


def configure(sc):
    sc.unit_settings.system = 'METRIC'
    sc.unit_settings.length_unit = 'MILLIMETERS'
    sc.render.engine = 'CYCLES'
    sc.cycles.samples = 80
    sc.cycles.use_denoising = True
    sc.render.resolution_x = 1600
    sc.render.resolution_y = 1200
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = 'PNG'
    sc.render.fps = 24
    sc.frame_start, sc.frame_end = 1, 288
    sc.view_settings.view_transform = 'AgX'
    world = bpy.data.worlds.new(sc.name + ' • environment')
    sc.world = world
    world.use_nodes = True
    world.node_tree.nodes['Background'].inputs[0].default_value = (.20, .23, .28, 1)
    world.node_tree.nodes['Background'].inputs[1].default_value = .25


def aim(obj, target):
    obj.rotation_euler = (Vector(target) * MM - obj.location).to_track_quat('-Z', 'Y').to_euler()


def camera(sc, name, pos, target, lens=60):
    data = bpy.data.cameras.new(name)
    obj = bpy.data.objects.new(name, data)
    sc.collection.objects.link(obj)
    obj.location = Vector(pos) * MM
    data.lens = lens
    data.clip_start = .001
    data.clip_end = 10
    aim(obj, target)
    sc.camera = obj
    return obj


def track_camera(sc, obj, target):
    focus = bpy.data.objects.new(obj.name + ' • fixed aim', None)
    sc.collection.objects.link(focus)
    focus.location = Vector(target) * MM
    con = obj.constraints.new('TRACK_TO')
    con.target = focus
    con.track_axis = 'TRACK_NEGATIVE_Z'
    con.up_axis = 'UP_Y'


def light(sc, name, pos, target, watts, size, color=(1, 1, 1), ratio=1):
    data = bpy.data.lights.new(name, 'AREA')
    obj = bpy.data.objects.new(name, data)
    sc.collection.objects.link(obj)
    obj.location = Vector(pos) * MM
    data.energy = watts * .1
    data.shape = 'RECTANGLE'
    data.size = size * MM
    data.size_y = size * ratio * MM
    data.color = color
    aim(obj, target)
    return obj


current_parent = None
configure(scene)
current_collection = scene.collection
floor = material('Studio • warm grey seamless', (.22, .24, .26), .6)
ground = box('Studio • seamless ground', (40, 28, -2), (2000, 2000, 4), floor, 0)
for frame, z in [(1, -2), (144, -2), (184, -70), (208, -70), (240, -2), (288, -2)]:
    ground.location.z = z * MM
    ground.keyframe_insert(data_path='location', frame=frame)
# A real curved cyclorama prevents the finite ground edge appearing in the
# taller exploded still. Its floor tangent starts behind the whole assembly.
profile = [(300 + 200 * math.sin(i * math.pi / 64),
            200 * (1 - math.cos(i * math.pi / 64))) for i in range(33)]
profile.append((500, 2000))
verts = [(x * MM, y * MM, z * MM) for x in (-2000, 2000) for y, z in profile]
n = len(profile)
faces = [(i, i + n, i + n + 1, i + 1) for i in range(n - 1)]
mesh = bpy.data.meshes.new('Studio • curved sweep')
mesh.from_pydata(verts, [], faces)
obj = bpy.data.objects.new('Studio • cyclorama', mesh)
scene.collection.objects.link(obj)
obj.data.materials.append(floor)
for frame, z in [(1, 0), (144, 0), (184, -68), (208, -68), (240, 0), (288, 0)]:
    obj.location.z = z * MM
    obj.keyframe_insert(data_path='location', frame=frame)
for poly in obj.data.polygons:
    poly.use_smooth = True
light(scene, 'Key • large silk', (-80, -80, 210), (35, 25, 0), 9, 180, (1, .91, .80), .65)
light(scene, 'Rim • long softbox', (110, 130, 140), (40, 28, 15), 12, 140, (.75, .85, 1), .25)
light(scene, 'Fill • front card', (170, -100, 80), (40, 28, 20), 3, 120)
hero = camera(scene, 'Camera • hero orbit', (145, -140, 180), (40, 26, 13), 52)
track_camera(scene, hero, (40, 26, 13))
for frame, angle in ((1, -59), (96, -27), (241, -32), (288, -59)):
    a = math.radians(angle)
    hero.location = Vector((40 + 200 * math.cos(a), 26 + 200 * math.sin(a), 175)) * MM
    aim(hero, (40, 26, 13))
    hero.keyframe_insert(data_path='location', frame=frame)
    hero.keyframe_insert(data_path='rotation_euler', frame=frame)
ports = camera(scene, 'Camera • connector close-up', (163, -38, 93), (65, 24, 20), 70)
track_camera(scene, ports, (61, 26, 20))
for frame, pos in [(97, (163, -38, 93)), (144, (163, 63, 93))]:
    ports.location = Vector(pos) * MM
    aim(ports, (61, 26, 20))
    ports.keyframe_insert(data_path='location', frame=frame)
    ports.keyframe_insert(data_path='rotation_euler', frame=frame)
exploded = camera(scene, 'Camera • exploded stack', (265, -335, 170), (40, 26, 51), 38)
for frame, cam in [(1, hero), (97, ports), (145, exploded), (241, hero)]:
    marker = scene.timeline_markers.new(cam.name, frame=frame)
    marker.camera = cam
scene.camera = hero

# Real-world scene, at correct physical scale, with modeled desk props.
desk = bpy.data.scenes.new('02 Real world • oak desk')
desk.collection.children.link(product)
configure(desk)
current_collection = desk.collection
wood = material('Desk • oiled oak', (.24, .12, .045), .48, coat=.12)
nodes = wood.node_tree.nodes
tex = nodes.new('ShaderNodeTexNoise')
tex.inputs['Scale'].default_value = 4
tex.inputs['Detail'].default_value = 3
coord = nodes.new('ShaderNodeTexCoord')
mapping = nodes.new('ShaderNodeVectorMath')
mapping.operation = 'MULTIPLY'
mapping.inputs[1].default_value = (5, 180, 25)
wood.node_tree.links.new(coord.outputs['Generated'], mapping.inputs[0])
wood.node_tree.links.new(mapping.outputs[0], tex.inputs['Vector'])
ramp = nodes.new('ShaderNodeValToRGB')
ramp.color_ramp.elements[0].position = .22
ramp.color_ramp.elements[0].color = (.105, .047, .018, 1)
ramp.color_ramp.elements[1].position = .8
ramp.color_ramp.elements[1].color = (.40, .23, .10, 1)
wood.node_tree.links.new(tex.outputs['Fac'], ramp.inputs[0])
wood.node_tree.links.new(ramp.outputs[0], nodes.get('Principled BSDF').inputs['Base Color'])
box('Desk • oak surface', (0, 0, -13), (1400, 1000, 26), wood, 3)
notebook = material('Notebook • woven navy cover', (.018, .032, .055), .9)
paper = material('Notebook • paper edges', (.71, .69, .60), .9)
for z in (1.5, 14.5):
    box('Notebook • cover', (-115, 98, z), (126, 190, 2), notebook, 3)
box('Notebook • page block', (-115, 98, 8), (121, 185, 11), paper, 1.5)
for i in range(12):
    box('Notebook • fine page lines', (-115, 4.9, 3 + .85 * i), (119, .12, .06), labelmat, 0)
pen = cylinder('Pen • graphite barrel', (-92, 65, 20), 3.2, 140, steel)
pen.rotation_euler = (math.pi / 2, 0, -.15)
ceramicwhite = material('Cup • porcelain', (.67, .69, .65), .18, coat=.4)
# Hollow ceramic cup made from a spun profile, coffee below its rim.
profile = [(0, 0), (29, 0), (32, 4), (33, 62), (32, 67), (29.5, 67), (29, 5), (0, 5)]
verts, faces = [], []
for i in range(96):
    a = i * math.tau / 96
    verts.extend([((175 + r * math.cos(a)) * MM, (152 + r * math.sin(a)) * MM, z * MM)
                  for r, z in profile])
for i in range(96):
    for j in range(len(profile) - 1):
        a, b = i * len(profile) + j, ((i + 1) % 96) * len(profile) + j
        faces.append((a, b, b + 1, a + 1))
mesh = bpy.data.meshes.new('Cup • lathed porcelain')
mesh.from_pydata(verts, [], faces)
obj = bpy.data.objects.new('Desk • coffee cup', mesh)
desk.collection.objects.link(obj)
finish(obj, ceramicwhite, .1, True)
coffee = material('Coffee • dark roast', (.016, .006, .002), .15)
cylinder('Coffee • surface', (175, 152, 55), 29.6, .25, coffee, 96)
curve('Cup • handle', [(203, 152, 55), (224, 152, 49), (226, 152, 27), (205, 152, 16)], 4, ceramicwhite)
light(desk, 'Window • daylight', (-180, 10, 320), (40, 28, 0), 24, 300, (.83, .91, 1), 1.4)
light(desk, 'Desk • warm bounce', (140, -180, 180), (40, 28, 15), 5, 230, (1, .83, .63))
cam = camera(desk, 'Camera • desk editorial', (228, -285, 285), (38, 35, 8), 58)
cam.data.dof.use_dof = True
cam.data.dof.focus_distance = (cam.location - Vector((40, 28, 18)) * MM).length
cam.data.dof.aperture_fstop = 7.1

# Save complete native scene, packed and usable without external texture downloads.
print('Adding authentic PCB silkscreens', flush=True)
current_collection = product
current_parent = groups['03 PCB + components']
silk('SILK_BOT.art', True)
silk('SILK_TOP.art', False)
print('Saving scene', flush=True)
scene.frame_set(1)
desk.frame_set(1)
bpy.context.window.scene = scene
scene.camera = hero
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            area.spaces.active.region_3d.view_perspective = 'CAMERA'
            area.spaces.active.region_3d.view_camera_zoom = 0
            area.spaces.active.clip_start = .001
            area.spaces.active.shading.type = 'MATERIAL'
OUT.mkdir(exist_ok=True)
report = {'revision': 26, 'blender': bpy.app.version_string, 'units': 'metres',
          'source_meshes': checks, 'placed_component_count': len(placements),
          'placement_data': placements,
          'notes': ['Shell derives from hash-verified R26 set; exterior clipped to smooth roof outline per user request.',
                    'Component XY/side/rotation from official placement; most package bodies visually approximated.',
                    'UI illustrative, editable geometry; no claim of firmware screenshot.',
                    'Battery dimensions user-supplied. Camera module intentionally absent.']}
(OUT / 'model-validation.json').write_text(json.dumps(report, indent=2) + '\n')
bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'p4x-eye-r26-black.blend'))
if args.preview:
    scene.render.resolution_percentage = 65
    scene.cycles.samples = 40
    scene.render.filepath = str(OUT / '01-studio-black-preview.png')
    bpy.ops.render.render(write_still=True)
