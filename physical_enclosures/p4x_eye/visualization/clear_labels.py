"""Render native label passes for crisp clear-film compositing over the fixed hold."""
from pathlib import Path
import json
import bpy
OUT=Path(bpy.data.filepath).parent/'label-passes';OUT.mkdir(exist_ok=True)
sc=bpy.data.scenes['03 Annotated • continuous studio'];bpy.context.window.scene=sc
labels=bpy.data.collections['ANNO • Labels and leaders'];groups=list(labels.children)
materials=[]
for group in groups:
    materials.append(next(mat for obj in group.all_objects for mat in obj.data.materials
                          if mat.use_nodes and mat.node_tree.nodes.get('Label opacity')))
values={}
for frame in range(432,577):
    sc.frame_set(frame);bpy.context.view_layer.update();graph=bpy.context.evaluated_depsgraph_get()
    values[frame]=[mat.evaluated_get(graph).node_tree.nodes['Label opacity'].outputs[0].default_value for mat in materials]
(OUT/'opacity.json').write_text(json.dumps(values)+'\n')
sc.frame_set(504);bpy.context.view_layer.update()
for obj in sc.objects:
    if obj.type!='CAMERA':obj.hide_render=True
sc.render.engine='BLENDER_EEVEE_NEXT';sc.eevee.taa_render_samples=128
sc.render.film_transparent=True;sc.render.resolution_x=1920 if (OUT.parent/'production-settings.json').exists() else 1280;sc.render.resolution_y=1080 if (OUT.parent/'production-settings.json').exists() else 720;sc.render.resolution_percentage=100
sc.render.image_settings.file_format='PNG';sc.render.image_settings.color_mode='RGBA'
for i,group in enumerate(groups):
    active=list(group.all_objects)
    if i==0: active += [obj for obj in labels.objects if obj.type=='FONT']
    for obj in active:obj.hide_render=False
    sc.render.filepath=str(OUT/f'{i:02d}.png');bpy.ops.render.render(write_still=True)
    for obj in active:obj.hide_render=True
print('Native label layers and fade values rendered',flush=True)
