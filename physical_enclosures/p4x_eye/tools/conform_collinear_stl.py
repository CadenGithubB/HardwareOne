#!/usr/bin/env python3
"""Conform exact collinear STL triangles by splitting their long-edge neighbor.

No vertex is moved, added or welded approximately. This is a triangulation-only
operation for CGAL-exported zero-area triangles with three distinct vertices.
"""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'mesh-tools'))
import numpy as np
import trimesh

def conform(mesh):
    vertices, inverse = np.unique(np.asarray(mesh.vertices), axis=0, return_inverse=True)
    faces = inverse[np.asarray(mesh.faces)].tolist()
    repairs = []
    for index in range(len(faces)):
        face = faces[index]
        if face is None:
            continue
        a = vertices[face]
        if np.linalg.norm(np.cross(a[1]-a[0],a[2]-a[0])) != 0:
            continue
        if len(set(face)) != 3:
            raise ValueError('Repeated-vertex triangle is outside this exact repair contract')
        pairs = [(0,1),(1,2),(2,0)]
        left,right = max(pairs,key=lambda pair:np.linalg.norm(a[pair[0]]-a[pair[1]]))
        middle = 3-left-right
        u,v,c = face[left],face[right],face[middle]
        direction = vertices[v]-vertices[u]
        t = np.dot(vertices[c]-vertices[u],direction)/np.dot(direction,direction)
        if not 0<t<1:
            raise ValueError('Collinear middle vertex is not strictly within its long edge')
        neighbors = [j for j,other in enumerate(faces) if j!=index and other is not None and u in other and v in other]
        if len(neighbors)!=1:
            raise ValueError(f'Expected exactly one long-edge neighbor, got {len(neighbors)}')
        neighbor = neighbors[0]
        other = faces[neighbor]
        edge_index = next(k for k in range(3) if {other[k],other[(k+1)%3]}=={u,v})
        p,q,r = other[edge_index],other[(edge_index+1)%3],other[(edge_index+2)%3]
        first,second = [p,c,r],[c,q,r]
        if any(np.linalg.norm(np.cross(vertices[f[1]]-vertices[f[0]],vertices[f[2]]-vertices[f[0]]))==0 for f in [first,second]):
            raise ValueError('Neighbor subdivision would create a zero-area face')
        faces[index] = None
        faces[neighbor] = first
        faces.append(second)
        repairs.append({'removed_face':index,'subdivided_face':neighbor,'middle_vertex':c,'long_edge':[u,v]})
    result = trimesh.Trimesh(vertices=vertices,faces=np.asarray([f for f in faces if f is not None]),process=False)
    return result,repairs

if __name__=='__main__':
    import json
    source,output=map(Path,sys.argv[1:3])
    raw=trimesh.load_mesh(source,process=False)
    result,repairs=conform(raw)
    if not result.is_watertight or not result.is_winding_consistent:
        raise RuntimeError('Conformed mesh did not pass topology checks')
    if abs(result.volume-raw.volume)>1e-8:
        raise RuntimeError('Unexpected volume change')
    output.write_bytes(trimesh.exchange.stl.export_stl(result))
    print(json.dumps({'output':str(output),'repairs':repairs,'coordinates_moved':0,'volume_delta_mm3':float(result.volume-raw.volume)},indent=2))
