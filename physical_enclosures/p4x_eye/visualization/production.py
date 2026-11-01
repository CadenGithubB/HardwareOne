"""Save an isolated 1080p production edition, preserving the earlier review files."""
import argparse,json
from pathlib import Path
import sys
import bpy
p=argparse.ArgumentParser();p.add_argument('--variant',choices=['black','frosted-clear'],required=True)
a=p.parse_args(sys.argv[sys.argv.index('--')+1:])
out=Path(__file__).resolve().parent/'output'/'1080p'/a.variant;out.mkdir(parents=True,exist_ok=True)
sc=bpy.data.scenes['03 Annotated • continuous studio'];bpy.context.window.scene=sc
# Keep quaternion signs compatible so subframe camera sampling for motion blur
# cannot spin across equivalent q/-q keys during the 360-degree turn.
rotations=[];previous=None
for frame in range(1,721):
 sc.frame_set(frame);bpy.context.view_layer.update()
 q=sc.camera.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world.to_quaternion().copy()
 if previous:q.make_compatible(previous)
 rotations.append(q);previous=q
for frame,q in enumerate(rotations,1):
 sc.camera.rotation_quaternion=q;sc.camera.keyframe_insert('rotation_quaternion',frame=frame)
for scene in bpy.data.scenes:
 scene.render.resolution_x=1920;scene.render.resolution_y=1080;scene.render.resolution_percentage=100
 scene.render.fps=24;scene.render.use_motion_blur=True;scene.render.motion_blur_shutter=.25
 scene.cycles.samples=128;scene.cycles.use_adaptive_sampling=True
 scene.cycles.adaptive_min_samples=32;scene.cycles.adaptive_threshold=.025
 scene.cycles.use_denoising=True;scene.cycles.denoising_prefilter='ACCURATE'
 scene.cycles.denoising_quality='HIGH';scene.cycles.denoising_input_passes='RGB_ALBEDO_NORMAL'
 scene.cycles.use_animated_seed=False;scene.cycles.seed=0
 scene.eevee.taa_render_samples=128
if a.variant=='frosted-clear':
 mat=bpy.data.materials['Case • frosted translucent resin']
 for node in mat.node_tree.nodes:
  if node.type=='BUMP':node.inputs['Strength'].default_value=0
settings={'variant':a.variant,'resolution':[1920,1080],'fps':24,'max_samples':128,'adaptive_min_samples':32,
 'adaptive_noise_threshold':.025,'denoising':'high quality, accurate prefilter, albedo + normals',
 'animated_seed':False,'motion_blur_shutter':.25,'clear_micro_bump':False,
 'opening_and_overhead_camera_poses_preserved':True}
(out/'production-settings.json').write_text(json.dumps(settings,indent=2)+'\n')
sc.frame_set(1);bpy.context.view_layer.update()
bpy.ops.wm.save_as_mainfile(filepath=str(out/f'p4x-eye-r26-{a.variant}-1080p.blend'))
