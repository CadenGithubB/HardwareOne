"""Build a continuous gray-studio shot with native animated white callouts.

Blender -b output/p4x-eye-r26-black.blend -P annotate.py
Then use render.py -- video and compose_annotated.py.
"""
import json
import math
from pathlib import Path
import bpy
from mathutils import Vector, Quaternion
from bpy_extras.object_utils import world_to_camera_view

HERE = Path(__file__).resolve().parent
OUT = HERE/'output'
PREFIX = 'ANNO • '
NAME = '03 Annotated • continuous studio'
ORBIT = 288  # Twelve seconds before the existing explode/label/reassemble move.
for scene in list(bpy.data.scenes):
    if scene.name.startswith('03 Annotated'):
        bpy.data.scenes.remove(scene)
for obj in list(bpy.data.objects):
    if obj.name.startswith(PREFIX):
        bpy.data.objects.remove(obj, do_unlink=True)
for col in list(bpy.data.collections):
    if col.name.startswith(PREFIX):
        bpy.data.collections.remove(col)
for datablocks in (bpy.data.materials,bpy.data.curves,bpy.data.meshes,bpy.data.cameras,bpy.data.lights,bpy.data.worlds):
    for data in list(datablocks):
        if data.name.startswith(PREFIX) and data.users == 0:
            datablocks.remove(data)
studio = bpy.data.scenes['01 Studio • animated assembly']
bpy.context.window.scene = studio
studio.frame_set(1)
bpy.context.view_layer.update()
source = bpy.data.collections['PRODUCT • R26 matched assembly']
sc = bpy.data.scenes.new(NAME)
product = bpy.data.collections.new(PREFIX+'Product')
labels = bpy.data.collections.new(PREFIX+'Labels and leaders')
sc.collection.children.link(product); sc.collection.children.link(labels)
copies = {}
for original in source.all_objects:
    obj = original.copy(); obj.name = PREFIX+original.name
    obj.animation_data_clear(); product.objects.link(obj)
    copies[original] = obj
for original,obj in copies.items():
    obj.parent = copies.get(original.parent)
    obj.matrix_parent_inverse = original.matrix_parent_inverse.copy()
    obj.matrix_basis = original.matrix_basis.copy()
    if original.parent is None:
        lift = original.get('exploded_lift_mm',0)*.001
        for frame,factor in [(1,0),(25,0),(144,1),(288,1),(360,0),(432,0)]:
            obj.location.z = lift*factor
            obj.keyframe_insert('location',frame=frame+ORBIT)

# Reuse the actual studio environment throughout the complete shot.
for original in studio.collection.objects:
    if original.type == 'LIGHT' or original.name in ('Studio • seamless ground','Studio • cyclorama'):
        obj = original.copy(); obj.name = PREFIX+original.name
        obj.animation_data_clear(); sc.collection.objects.link(obj)
        if original.type == 'LIGHT':
            obj.data = original.data.copy()
        else:
            obj.data = original.data.copy()
            mat = original.data.materials[0].copy(); mat.name=PREFIX+'Dark gray studio'
            mat.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value=(.08,.09,.105,1)
            obj.data.materials.clear(); obj.data.materials.append(mat)
            offset = -.002 if 'ground' in original.name else 0
            for frame,factor in [(1,0),(25,0),(144,1),(288,1),(360,0),(432,0)]:
                obj.location.z=offset-.068*factor
                obj.keyframe_insert('location',frame=frame+ORBIT)
sc.world=studio.world.copy(); sc.world.name=PREFIX+'Studio environment'
sc.unit_settings.system='METRIC';sc.unit_settings.length_unit='MILLIMETERS'
sc.render.engine='CYCLES';sc.cycles.samples=96;sc.cycles.use_denoising=True
sc.render.fps=24;sc.frame_start=1;sc.frame_end=432+ORBIT
sc.render.resolution_x=3840;sc.render.resolution_y=2160;sc.render.resolution_percentage=100
sc.render.image_settings.file_format='PNG';sc.render.image_settings.color_mode='RGB'
sc.view_settings.view_transform='AgX'
bpy.context.window.scene=sc
focus=bpy.data.objects.new(PREFIX+'Camera aim',None);sc.collection.objects.link(focus)
data=bpy.data.cameras.new(PREFIX+'Single continuous camera')
cam=bpy.data.objects.new(PREFIX+'Continuous camera',data);sc.collection.objects.link(cam)
cam.data.clip_start=.001;cam.data.clip_end=10;sc.camera=cam
constraint=cam.constraints.new('TRACK_TO');constraint.target=focus
constraint.track_axis='TRACK_NEGATIVE_Z';constraint.up_axis='UP_Y'
start=(40+200*math.cos(math.radians(-59)),26+200*math.sin(math.radians(-59)),125)
for frame,pos,target,lens,shift in [
    (1,start,(40,26,13),52,0),(25,start,(40,26,13),52,0),
    (144,(265,-335,170),(40,26,51),32,.15),
    (288,(265,-335,170),(40,26,51),32,.15),
    (432,(40,28,240),(40,28,14),52,0)]:
    cam.location=Vector(pos)*.001;cam.keyframe_insert('location',frame=frame+ORBIT)
    focus.location=Vector(target)*.001;focus.keyframe_insert('location',frame=frame+ORBIT)
    data.lens=lens;data.keyframe_insert('lens',frame=frame+ORBIT)
    data.shift_x=shift;data.keyframe_insert('shift_x',frame=frame+ORBIT)
# Full orbit with short acceleration/deceleration ramps and a lower angle
# to reveal the encoder wheel. Dense keys trace a circle without corner cutting.
for frame in range(1, ORBIT+2):
    t=frame-1; ramp=24
    if t<ramp: distance=t*t/(2*ramp)
    elif t>ORBIT-ramp: distance=ORBIT-ramp-(ORBIT-t)**2/(2*ramp)
    else: distance=t-ramp/2
    angle=math.radians(-59)+2*math.pi*distance/(ORBIT-ramp)
    cam.location=Vector((40+200*math.cos(angle),26+200*math.sin(angle),125))*.001
    cam.keyframe_insert('location',frame=frame)
    focus.location=Vector((40,26,13))*.001;focus.keyframe_insert('location',frame=frame)
    data.lens=52;data.keyframe_insert('lens',frame=frame)
    data.shift_x=0;data.keyframe_insert('shift_x',frame=frame)
# A genuine one-second pause, then a direct pullback while the layers rise.
# Dense smoothstep keys prevent the old intermediate hero swoop/overshoot.
for frame in range(ORBIT+1, ORBIT+145):
    u=max(0,min(1,(frame-(ORBIT+25))/119)); u=u*u*(3-2*u)
    cam.location=Vector(start).lerp(Vector((265,-335,170)),u)*.001
    focus.location=Vector((40,26,13)).lerp(Vector((40,26,51)),u)*.001
    data.lens=52-20*u;data.shift_x=.15*u
    cam.keyframe_insert('location',frame=frame);focus.keyframe_insert('location',frame=frame)
    data.keyframe_insert('lens',frame=frame);data.keyframe_insert('shift_x',frame=frame)
# Bake the established opening orientation before replacing the return swoop.
rotations=[]
for frame in range(1,577):
    sc.frame_set(frame);bpy.context.view_layer.update()
    rotations.append(cam.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world.to_quaternion().copy())
sc.frame_set(576);bpy.context.view_layer.update()
wide_focus=Vector((40,26,51))*.001
wide_distance=(cam.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world.translation-wide_focus).length
cam.constraints.remove(constraint);cam.rotation_mode='QUATERNION'
for frame,rotation in enumerate(rotations,1):
    cam.rotation_quaternion=rotation;cam.keyframe_insert('rotation_quaternion',frame=frame)
# Ease directly from the separated stack to an overhead, horizontally aligned
# product view. No intermediate hero angle or return to the low orbit pose.
for frame in range(577,721):
    u=max(0,min(1,(frame-576)/96));u=u*u*(3-2*u)
    rotation=rotations[-1].slerp(Quaternion((1,0,0,0)),u)
    target=wide_focus.lerp(Vector((40,28,14))*.001,u)
    distance=wide_distance*(1-u)+.226*u
    cam.location=target+rotation@Vector((0,0,distance))
    cam.rotation_quaternion=rotation;focus.location=target
    data.lens=32+20*u;data.shift_x=.15*(1-u)
    cam.keyframe_insert('location',frame=frame);cam.keyframe_insert('rotation_quaternion',frame=frame)
    focus.keyframe_insert('location',frame=frame)
    data.keyframe_insert('lens',frame=frame);data.keyframe_insert('shift_x',frame=frame)
sc['ending_view']='Overhead; long edge horizontal; stationary frames 672–720'
sc['pre_explosion_hold_frames']=[ORBIT+1,ORBIT+25]
sc['intro_orbit_degrees']=360;sc['intro_orbit_frames']=[1,ORBIT+1]
sc.frame_set(216+ORBIT);bpy.context.view_layer.update()
graph=bpy.context.evaluated_depsgraph_get();camera=cam.evaluated_get(graph)
W,H=1920,1080
frame=camera.data.view_frame(scene=sc);depth=.15
scale=depth/-frame[0].z
x0,x1=min(v.x for v in frame)*scale,max(v.x for v in frame)*scale
y0,y1=min(v.y for v in frame)*scale,max(v.y for v in frame)*scale

def ink(name,index,caption=False):
    mat=bpy.data.materials.new(PREFIX+name);mat.use_nodes=True
    nodes=mat.node_tree.nodes;nodes.clear()
    output=nodes.new('ShaderNodeOutputMaterial')
    emission=nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value=(1,1,1,1)
    emission.inputs['Strength'].default_value=1.2 if caption else 4
    alpha=nodes.new('ShaderNodeValue');alpha.name='Label opacity'
    for frame,value in [(1,0),(152+index*2,0),(164+index*2,1),(268,1),(284,0),(432,0)]:
        alpha.outputs[0].default_value=value;alpha.outputs[0].keyframe_insert('default_value',frame=frame+ORBIT)
    transparent=nodes.new('ShaderNodeBsdfTransparent');mix=nodes.new('ShaderNodeMixShader')
    links=mat.node_tree.links
    links.new(alpha.outputs[0],mix.inputs[0]);links.new(transparent.outputs[0],mix.inputs[1])
    links.new(emission.outputs[0],mix.inputs[2]);links.new(mix.outputs[0],output.inputs[0])
    return mat

def pixel(x,y):
    return Vector((x0+x/W*(x1-x0),y1-y/H*(y1-y0),-depth))

def text(name,body,x,y,size,mat,collection=labels):
    data=bpy.data.curves.new(PREFIX+name,'FONT')
    data.body=body;data.size=size*(y1-y0)/H;data.align_y='TOP';data.space_character=1.04
    obj=bpy.data.objects.new(PREFIX+name,data);collection.objects.link(obj)
    obj.parent=cam;obj.location=pixel(x,y);data.materials.append(mat)
    return obj

def line(name,points,mat,collection):
    data=bpy.data.curves.new(PREFIX+name,'CURVE');data.dimensions='3D'
    data.bevel_depth=.6*(x1-x0)/W;data.bevel_resolution=2
    spline=data.splines.new('POLY');spline.points.add(len(points)-1)
    for point,xy in zip(spline.points,points):point.co=(*pixel(*xy),1)
    obj=bpy.data.objects.new(PREFIX+name,data);collection.objects.link(obj)
    obj.parent=cam;data.materials.append(mat)
    return obj

items = [
    ('LCD module', 'Display and cover glass', (45.7, 25.3, 157.1)),
    ('Upper shell', 'Buttons and power slider', (79, 41, 132)),
    ('PCB screw', '1 × M1.6 reference', (21.1, 8.7, 115.8)),
    ('P4X EYE mainboard', 'Processor, radio and controls', (74, 32.5, 97.3)),
    ('Midplate screws', '2 × M2 × 8', (44, 48.5, 71.2)),
    ('Midplate', 'Board support and battery divider', (73, 38, 52.5)),
    ('Battery', '65 × 36 × 10 mm LiPo', (73.5, 42, 30.5)),
    ('Lower shell', 'Battery cradle and screw wells', (79, 41, 7)),
    ('Case screws', '4 × M2 × 8 · installed from below', (77.26, 41, -50.5)),
]

rows=[]
for title,subtitle,anchor in items:
    point=world_to_camera_view(sc,camera,Vector(anchor)*.001)
    rows.append({'title':title,'subtitle':subtitle,'anchor':[point.x*W,(1-point.y)*H]})
rows.sort(key=lambda row:row['anchor'][1]);previous=150
for row in rows:
    row['y']=max(previous,row['anchor'][1]-8);previous=row['y']+72
if rows[-1]['y']>987:
    excess=rows[-1]['y']-987
    for row in rows:row['y']-=excess
for i,row in enumerate(rows):
    col=bpy.data.collections.new(PREFIX+row['title']);labels.children.link(col)
    white=ink(row['title']+' white ink',i);caption=ink(row['title']+' caption',i,True)
    if i==0:
        text('Title','P4X EYE',105,70,42,white)
        text('Subtitle','Exploded assembly',106,119,23,caption)
    x,y=1165,row['y'];text(row['title'],row['title'],x,y,27,white,col)
    text(row['title']+' caption',row['subtitle'],x,y+31,18,caption,col)
    tip=row['anchor'];elbow=[1060,y+17]
    line(row['title']+' leader',[(x-22,y+17),elbow,tip],white,col)
    dx,dy=elbow[0]-tip[0],elbow[1]-tip[1];length=math.hypot(dx,dy);ux,uy=dx/length,dy/length
    line(row['title']+' arrowhead',[(tip[0]+ux*7-uy*3,tip[1]+uy*7+ux*3),tip,
         (tip[0]+ux*7+uy*3,tip[1]+uy*7-ux*3)],white,col)
    assert 80<y<1000 and 0<tip[0]<1100 and 0<tip[1]<H,row
sc['continuous_shot']=True;sc['reading_pause_frames']=[144+ORBIT,288+ORBIT]
sc['reading_pause_seconds']=6;sc['annotation_note']='Single perspective camera, gray studio, animated white native callouts; no shot cuts or background transitions.'
(OUT/'annotation-layout.json').write_text(json.dumps({'labels':rows,'pause_seconds':6,
    'scene':NAME,'pause_frames':[144+ORBIT,288+ORBIT],'single_camera':True,'white_text_and_arrows':True},indent=2)+'\n')
sc.frame_set(1);bpy.context.view_layer.update()
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'p4x-eye-r26-black.blend'))
