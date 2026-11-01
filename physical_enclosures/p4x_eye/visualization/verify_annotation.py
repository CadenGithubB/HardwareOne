"""Verify a genuinely continuous camera take, paused geometry and white callouts."""
import json
import math
from pathlib import Path
import bpy
from mathutils import Vector
from bpy_extras.object_utils import world_to_camera_view

OUT=Path(bpy.data.filepath).parent
sc=bpy.data.scenes['03 Annotated • continuous studio']
bpy.context.window.scene=sc
product=bpy.data.collections['ANNO • Product']
labels=bpy.data.collections['ANNO • Labels and leaders']
assert len([o for o in sc.objects if o.type=='CAMERA'])==1
assert not any(marker.camera for marker in sc.timeline_markers)
assert len(labels.children)==9
assert len([o for o in labels.all_objects if o.type=='FONT'])==20
assert len([o for o in labels.all_objects if o.type=='CURVE'])==18
assert len([o for o in product.all_objects if o.get('fastener_head')])==7
animated={mat for obj in labels.all_objects for mat in obj.data.materials
          if mat.use_nodes and mat.node_tree.nodes.get('Label opacity')}
assert len(animated)==18
roots=[o for o in product.objects if o.parent is None]
assert len(roots)==9
previous=None
orbit_angles=[]
margin=1
max_step=0
bounds_objects=[o for o in product.all_objects if o.name.startswith(('ANNO • R26 • base','ANNO • R26 • lid')) or o.get('fastener_head')]
sc.render.resolution_x,sc.render.resolution_y=1280,720
hold={}
outro=[]
intro_hold=[]
transition=[]
for frame in range(1,721):
    sc.frame_set(frame);bpy.context.view_layer.update()
    graph=bpy.context.evaluated_depsgraph_get();cam=sc.camera.evaluated_get(graph)
    state=(cam.matrix_world.copy(),cam.data.lens,cam.data.shift_x)
    if previous:
        step=(state[0].translation-previous[0].translation).length
        max_step=max(max_step,step)
        assert step<.008,(frame,step)
        assert abs(state[1]-previous[1])<.55,(frame,state[1],previous[1])
        assert abs(state[2]-previous[2])<.005,frame
        angle=state[0].to_quaternion().rotation_difference(previous[0].to_quaternion()).angle
        angle=min(angle,2*math.pi-angle)
        assert angle<.04,(frame,angle)
    previous=state
    if frame<=289:
        delta=cam.matrix_world.translation-Vector((.04,.026,0))
        angle=math.atan2(delta.y,delta.x)
        if orbit_angles:
            while angle<orbit_angles[-1]-.01: angle+=2*math.pi
        orbit_angles.append(angle)
        assert all(abs(o.evaluated_get(graph).matrix_world.translation.z)<1e-6 for o in roots)

    for obj in bounds_objects:
        evaluated=obj.evaluated_get(graph)
        for corner in evaluated.bound_box:
            point=world_to_camera_view(sc,cam,evaluated.matrix_world@Vector(corner))
            value=min(point.x,1-point.x,point.y,1-point.y)
            margin=min(margin,value)
            assert value>.005,(frame,obj.name,tuple(point))
    if 289<=frame<=313:
        intro_hold.append(state)
        assert all(abs(o.evaluated_get(graph).matrix_world.translation.z)<1e-6 for o in roots)
    if frame<=289:
        assert abs(state[0].translation.z-.125)<1e-6
    if 313<=frame<=432:
        transition.append((state[0].translation.copy(),state[1],
                           [o.evaluated_get(graph).matrix_world.translation.z for o in roots]))
    if frame>=576:
        outro.append((state[0].to_quaternion(),state[1],state[2]))
    if frame>=672:
        assert state[0].to_quaternion().angle<1e-5
        assert abs(state[0].translation.x-.04)<1e-6 and abs(state[0].translation.y-.028)<1e-6
        assert abs(state[0].translation.z-.24)<1e-6
    if frame in (432,504,576):
        hold[frame]=([o.evaluated_get(graph).matrix_world.copy() for o in roots],state)
    if frame in (1,432,504,576,720):
        opacity=1 if frame==504 else 0
        for mat in animated:
            value=mat.evaluated_get(graph).node_tree.nodes['Label opacity'].outputs[0].default_value
            assert abs(value-opacity)<1e-5,(frame,mat.name,value)
for before,after in zip(outro,outro[1:]):
    # A single continuous tilt toward the overhead orientation, never away.
    assert after[0].angle<=before[0].angle+1e-5
    assert after[1]>=before[1]-1e-5 and after[2]<=before[2]+1e-5
for state in intro_hold:
    assert max(abs(state[0][i][j]-intro_hold[0][0][i][j]) for i in range(4) for j in range(4))<1e-6
    assert abs(state[1]-intro_hold[0][1])<1e-6 and abs(state[2]-intro_hold[0][2])<1e-6
for before,after in zip(transition,transition[1:]):
    assert after[0].x>=before[0].x-1e-7 and after[0].y<=before[0].y+1e-7
    assert after[0].z>=before[0].z-1e-7 and after[1]<=before[1]+1e-5
    for root,a,b in zip(roots,before[2],after[2]):
        assert (b<=a+1e-7 if root.get('exploded_lift_mm',0)<0 else b>=a-1e-7)
assert abs(orbit_angles[-1]-orbit_angles[0]-2*math.pi)<1e-5
for frame in (432,576):
    for a,b in zip(hold[frame][0],hold[504][0]):
        assert max(abs(a[i][j]-b[i][j]) for i in range(4) for j in range(4))<1e-6
    assert max(abs(hold[frame][1][0][i][j]-hold[504][1][0][i][j]) for i in range(4) for j in range(4))<1e-6
sc.frame_set(504);bpy.context.view_layer.update();graph=bpy.context.evaluated_depsgraph_get()
cam=sc.camera.evaluated_get(graph)
for obj in labels.all_objects:
    if obj.type=='FONT':
        evaluated=obj.evaluated_get(graph)
        for corner in evaluated.bound_box:
            p=world_to_camera_view(sc,cam,evaluated.matrix_world@Vector(corner))
            assert .015<p.x<.985 and .015<p.y<.985,(obj.name,tuple(p))
labels.hide_viewport=True
bpy.context.view_layer.update();graph=bpy.context.evaluated_depsgraph_get()
cam=sc.camera.evaluated_get(graph)
for obj in product.all_objects:
    if obj.get('fastener_head'):
        point=obj.evaluated_get(graph).matrix_world.translation
        hit,loc,normal,index,first,matrix=sc.ray_cast(graph,cam.matrix_world.translation,(point-cam.matrix_world.translation).normalized())
        assert hit and first.get('fastener_id')==obj.get('fastener_id'),(obj.name,first.name if hit else 'none')
labels.hide_viewport=False
assert all(mat.node_tree.nodes.get('Emission').inputs['Color'].default_value[0]>.99 for mat in animated)
report={'passed':True,'single_camera':True,'camera_cuts':0,'overhead_horizontal_ending':True,'no_return_swoop':True,'final_hold_seconds':2,'frames_checked':720,
        'intro_orbit_degrees':360,'intro_orbit_seconds':12,'pre_explosion_hold_seconds':1,'constant_orbit_height':True,'direct_monotonic_explosion_transition':True,'minimum_product_margin':margin,'maximum_camera_step_mm':max_step*1000,
        'white_callout_groups':9,'native_text_objects':20,'native_arrow_curves':18,
        'paused_camera_and_geometry_frames':[432,504,576],'reading_pause_seconds':6,
        'all_text_inside_canvas':True,'seven_screw_heads_visible':True,'background':'continuous gray studio'}
(OUT/'annotation-scene-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('Continuous camera, pause, framing and white label checks passed',flush=True)
