"""Website image set derived from the approved black assembly; independent of film jobs."""
import argparse
import json
from pathlib import Path
import sys
import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent
OUT = HERE / 'output' / 'website'
OUT.mkdir(exist_ok=True)
p = argparse.ArgumentParser()
p.add_argument('--only', nargs='+')
p.add_argument('--preview', action='store_true')
args = p.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
studio = bpy.data.scenes['01 Studio • animated assembly']
desk = bpy.data.scenes['02 Real world • oak desk']

# New local presentation settings only: the existing files and running clear job
# never read or write this output directory.
floor = bpy.data.materials['Studio • warm grey seamless']
floor.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value = (.019, .023, .030, 1)
floor.node_tree.nodes.get('Principled BSDF').inputs['Roughness'].default_value = .72
studio.world.node_tree.nodes['Background'].inputs[1].default_value = .16

def camera(sc, name, position, target, lens):
    data = bpy.data.cameras.new(name)
    obj = bpy.data.objects.new(name, data)
    sc.collection.objects.link(obj)
    obj.location = Vector(position) * .001
    obj.rotation_euler = (Vector(target) * .001 - obj.location).to_track_quat('-Z', 'Y').to_euler()
    data.lens = lens
    data.clip_start = .001
    data.clip_end = 10
    return obj

views = [
    ('01-hero', studio, 1, (155, -150, 185), (40, 27, 14), 67, (2400, 1800)),
    ('02-controls-ports', studio, 1, (155, 3, 105), (57, 25, 24), 78, (2000, 1500)),
    ('03-exploded', studio, 196, (280, -350, 175), (40, 27, 51), 59, (1600, 2000)),
    ('04-on-the-desk', desk, 1, (210, -290, 325), (16, 50, 4), 56, (2400, 1600)),
    ('05-overhead', studio, 1, (40, 27, 280), (40, 27, 14), 70, (2400, 1600)),
]
manifest = []
for name, sc, frame, pos, target, lens, resolution in views:
    if args.only and name not in args.only:
        continue
    bpy.context.window.scene = sc
    for marker in sc.timeline_markers:
        marker.camera = None
    sc.frame_set(frame)
    sc.camera = camera(sc, 'Website • ' + name, pos, target, lens)
    sc.render.engine = 'CYCLES'
    sc.cycles.device = 'CPU'
    sc.render.threads_mode = 'FIXED'
    sc.render.threads = 4
    sc.cycles.samples = 16 if args.preview else 96
    sc.cycles.use_adaptive_sampling = True
    sc.cycles.adaptive_threshold = .035 if args.preview else .015
    sc.cycles.use_denoising = True
    sc.cycles.seed = 17
    sc.render.use_motion_blur = False
    sc.render.resolution_x, sc.render.resolution_y = resolution
    sc.render.resolution_percentage = 32 if args.preview else 100
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGB'
    sc.render.filepath = str(OUT / (name + ('-proof' if args.preview else '') + '.png'))
    print('WEBSITE_RENDER', name, flush=True)
    bpy.ops.render.render(write_still=True)
    manifest.append({'name': name, 'frame': frame, 'resolution': resolution,
                     'camera': list(pos), 'aim': list(target), 'lens': lens})

if not args.preview and not args.only:
    studio.frame_set(1)
    bpy.context.window.scene = studio
    studio.camera = bpy.data.objects['Website • 01-hero']
    studio.render.resolution_x, studio.render.resolution_y = 2400, 1800
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'hardware-one-website.blend'))
    (OUT / 'render-settings.json').write_text(json.dumps({
        'source': 'output/p4x-eye-r26-black.blend', 'engine': 'CYCLES',
        'device': 'CPU', 'threads': 4, 'samples': 96, 'views': manifest,
        'notes': 'Illustrative screen UI. Original mechanical CAD and active film jobs unchanged.'
    }, indent=2))
