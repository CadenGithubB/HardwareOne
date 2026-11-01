"""Make a separate frosted clear resin edition of the verified black assembly."""
from pathlib import Path
import bpy
OUT=Path(__file__).resolve().parent/'output'/'frosted-clear'
OUT.mkdir(exist_ok=True)
mat=bpy.data.materials['Case • satin black resin']
mat.name='Case • frosted translucent resin'
bs=mat.node_tree.nodes['Principled BSDF']
bs.inputs['Base Color'].default_value=(.93,.96,.95,1)
bs.inputs['Roughness'].default_value=.12
bs.inputs['Metallic'].default_value=0
bs.inputs['IOR'].default_value=1.46
bs.inputs['Transmission Weight'].default_value=.96
bs.inputs['Coat Weight'].default_value=.18
bs.inputs['Coat Roughness'].default_value=.1
mat.diffuse_color=(.93,.96,.95,1)
for node in mat.node_tree.nodes:
    if node.type=='BUMP':
        node.inputs['Strength'].default_value=.08
        node.inputs['Distance'].default_value=.000006
for scene in bpy.data.scenes:
    scene['case_finish']='Frosted translucent resin; 96% transmission, roughness 0.12, IOR 1.46.'
    scene.cycles.max_bounces=8
    scene.cycles.transmission_bounces=6
    scene.cycles.transparent_max_bounces=8
    scene.cycles.caustics_reflective=False
    scene.cycles.caustics_refractive=False
bpy.context.window.scene=bpy.data.scenes['03 Annotated • continuous studio']
bpy.context.scene.frame_set(1)
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'p4x-eye-r26-frosted-clear.blend'))
