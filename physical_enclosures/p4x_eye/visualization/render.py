"""Render saved scene: Blender -b output/p4x-eye-r26-black.blend -P render.py -- stills|video|proof.

Video frames are resumable and retained until the separate encoding/verification
step succeeds. Use --start/--end for a bounded subset.
"""
import argparse
from pathlib import Path
import sys
import shutil
import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent
OUT = HERE / 'output'
p = argparse.ArgumentParser()
p.add_argument('mode', choices=['stills', 'video', 'proof'])
p.add_argument('--variant', choices=['black','frosted-clear'], default='black')
p.add_argument('--production', action='store_true')
p.add_argument('--output-dir', type=Path)
p.add_argument('--gpu', action='store_true')
p.add_argument('--samples', type=int, help='Override per-view sampling for proofs or film')
p.add_argument('--start', type=int, default=1)
p.add_argument('--end', type=int, default=720)
p.add_argument('--engine', choices=['CYCLES', 'BLENDER_EEVEE_NEXT'], default='CYCLES')
p.add_argument('--only', nargs='+', choices=['01', '02', '03', '04', '05', '06', '07', '08'])
args = p.parse_args(sys.argv[sys.argv.index('--') + 1:])
if args.variant != 'black':
    OUT = OUT / args.variant
    OUT.mkdir(exist_ok=True)
if args.output_dir:
    OUT=args.output_dir.resolve();OUT.mkdir(parents=True,exist_ok=True)
if args.gpu:
    preferences=bpy.context.preferences.addons['cycles'].preferences
    preferences.compute_device_type='METAL';preferences.get_devices()
    for device in preferences.devices: device.use=device.type=='METAL'
    for scene in bpy.data.scenes: scene.cycles.device='GPU'
studio = bpy.data.scenes['01 Studio • animated assembly']
desk = bpy.data.scenes['02 Real world • oak desk']


def render(sc, filename, cam=None, frame=1, resolution=(1600, 1200), samples=96):
    if args.mode == 'stills' and args.only and Path(filename).name[:2] not in args.only:
        return
    bpy.context.window.scene = sc
    # Render evaluation re-applies timeline camera cuts. Temporarily unbind them
    # for explicit still cameras, including the underside inspection camera.
    bindings = [(marker, marker.camera) for marker in sc.timeline_markers]
    if cam:
        for marker, camera in bindings:
            marker.camera = None
    sc.frame_set(frame)
    if cam:
        sc.camera = bpy.data.objects[cam]
    sc.render.engine = args.engine
    if args.variant == 'frosted-clear': sc.render.use_persistent_data=True
    sc.cycles.samples = args.samples or samples
    if hasattr(sc, 'eevee') and hasattr(sc.eevee, 'taa_render_samples'):
        sc.eevee.taa_render_samples = args.samples or samples
    sc.cycles.use_denoising = True
    sc.render.resolution_x, sc.render.resolution_y = resolution
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = 'PNG'
    if args.variant != 'black':
        filename = filename.replace('-black', '-'+args.variant)
    sc.render.filepath = str(OUT / filename)
    print('RENDER', sc.render.filepath, flush=True)
    bpy.ops.render.render(write_still=True)
    for marker, camera in bindings:
        marker.camera = camera


if args.mode == 'stills':
    # A tighter hero composition fits the stills' taller 4:3 aspect ratio.
    bpy.data.cameras['Camera • hero orbit'].lens = 62
    render(studio, '01-studio-black.png', 'Camera • hero orbit')
    render(studio, '02-connectors-black.png', 'Camera • connector close-up', 120)
    render(studio, '06-wheel-side-black.png', 'Camera • connector close-up', 144)
    render(desk, '03-desk-black.png', 'Camera • desk editorial', resolution=(1800, 1200))
    bpy.data.cameras['Camera • exploded stack'].lens = 60
    render(studio, '04-exploded-black.png', 'Camera • exploded stack', 196,
           resolution=(1400, 1800), samples=112)
    if not args.only or '07' in args.only:
        render(bpy.data.scenes['03 Annotated • continuous studio'],
               '07-exploded-labeled.png', frame=504, resolution=(3840, 2160), samples=96)
    if not args.only or '08' in args.only:
        render(bpy.data.scenes['03 Annotated • continuous studio'],
               '08-overhead-'+args.variant+'.png', frame=720, resolution=(1920,1080))
    # Bottom electronics revealed in a separate isolated inspection view.
    # Keep geometry in assembly, rotate camera and remove shell/battery from render.
    hidden = []
    for name in ('00 Base + hardware', '01 Battery', '02 Midframe', '04 Lid + controls', '05 LCD module',
                 '06 Bottom screws', '07 Midplate screws', '08 PCB screw'):
        for obj in bpy.data.objects[name].children_recursive:
            hidden.append((obj, obj.hide_render))
            obj.hide_render = True
    ground = bpy.data.objects['Studio • seamless ground']
    ground.hide_render = True
    bpy.data.objects['Studio • cyclorama'].hide_render = True
    camdata = bpy.data.cameras.new('Camera • electronics underside')
    cam = bpy.data.objects.new('Camera • electronics underside', camdata)
    studio.collection.objects.link(cam)
    cam.location = Vector((130, -110, -115)) * .001
    cam.rotation_euler = (Vector((40, 27, 19)) * .001 - cam.location).to_track_quat('-Z', 'Y').to_euler()
    camdata.lens = 65
    camdata.clip_start = .001
    # A broad light beneath the assembly exposes the physical PCB top side.
    data = bpy.data.lights.new('Inspection • underside softbox', 'AREA')
    obj = bpy.data.objects.new('Inspection • underside softbox', data)
    studio.collection.objects.link(obj)
    obj.location = (.01, -.05, -.15)
    obj.rotation_euler = (Vector((.04, .027, .02)) - obj.location).to_track_quat('-Z', 'Y').to_euler()
    data.energy, data.size = .7, .15
    render(studio, '05-electronics-underside.png', cam.name)
    for obj, value in hidden:
        obj.hide_render = value
    ground.hide_render = False
    bpy.data.objects['Studio • cyclorama'].hide_render = False
elif args.mode in ('video', 'proof'):
    continuous = bpy.data.scenes.get('03 Annotated • continuous studio')
    if continuous is None:
        raise RuntimeError('Run annotate.py to build the continuous studio scene first')
    directory = OUT / ('continuous-frames' if args.mode == 'video' else 'continuous-proof')
    directory.mkdir(exist_ok=True)
    frames = range(args.start, args.end + 1) if args.mode == 'video' else (1, 73, 120, 135, 145, 155, 170, 217, 289, 301, 313, 325, 349, 373, 397, 432, 468, 504, 556, 576, 612, 648, 672, 684, 720)
    for frame in frames:
        filename = directory / f'{frame:04d}.png'
        if filename.exists() and args.mode == 'video':
            continue
        if args.production and args.variant=='frosted-clear' and 433<=frame<=576 and (directory/'0432.png').exists():
            shutil.copyfile(directory/'0432.png',filename)
            continue
        # Fully visible labels, geometry, camera and lighting are constant in
        # this reading hold. Repeat one rendered frame to prevent shimmer.
        if args.mode == 'video' and 290 <= frame <= 313 and (directory / '0289.png').exists():
            shutil.copyfile(directory / '0289.png', filename)
            continue
        if args.mode == 'video' and frame>=673 and (directory / '0672.png').exists():
            shutil.copyfile(directory / '0672.png', filename)
            continue
        held = directory / '0468.png'
        if args.mode == 'video' and 469 <= frame <= 556 and held.exists():
            shutil.copyfile(held, filename)
            continue
        render(continuous, str(filename), frame=frame,
               resolution=((1920,1080) if args.production else (1280,720)) if args.mode == 'video' else (960,540),
               samples=128 if args.production else (32 if args.mode == 'video' else 20))
