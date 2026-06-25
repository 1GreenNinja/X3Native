# Headless software render of a weapon GLB into a showcase PNG so the distinct
# re-skin is VISIBLE without the engine's Vulkan path (which may hang on this GPU).
#
# Pure CPU: loads geometry with trimesh, reads the glTF material factors with
# pygltflib (baseColorFactor tint + emissiveFactor + strength), then Lambert +
# rim + emissive shades it with numpy and writes the PNG via matplotlib. No
# OpenGL / pyglet needed.
#
# Usage: python tools/weapon_showcase_render.py <glb> <weapon_key> <out.png>
import sys, os, numpy as np
import trimesh
from pygltflib import GLTF2
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

def mat_factors(path):
    gl = GLTF2().load(path)
    if not gl.materials:
        return [0.8,0.8,0.8,1.0], [0,0,0], 1.0, 1.0, 0.5
    m = gl.materials[0]
    pbr = m.pbrMetallicRoughness
    base = list(pbr.baseColorFactor or [1,1,1,1])
    metal = pbr.metallicFactor if pbr.metallicFactor is not None else 1.0
    rough = pbr.roughnessFactor if pbr.roughnessFactor is not None else 1.0
    emiss = list(m.emissiveFactor or [0,0,0])
    estr = 1.0
    if m.extensions and "KHR_materials_emissive_strength" in m.extensions:
        estr = m.extensions["KHR_materials_emissive_strength"].get("emissiveStrength", 1.0)
    return base, emiss, estr, metal, rough

def main():
    glb, key, out = sys.argv[1], sys.argv[2], sys.argv[3]
    base, emiss, estr, metal, rough = mat_factors(glb)

    scene_or_mesh = trimesh.load(glb, force='mesh')
    mesh = scene_or_mesh
    if mesh.vertices.shape[0] == 0:
        raise RuntimeError("empty mesh")

    # Center + normalize into a unit box, then orient for a 3/4 showcase view.
    v = mesh.vertices - mesh.vertices.mean(axis=0)
    scale = np.abs(v).max()
    v = v / (scale + 1e-9)

    # Rotate: yaw 35 deg, pitch -20 deg for a hero 3/4 angle.
    def rot_y(a):
        c,s=np.cos(a),np.sin(a); return np.array([[c,0,s],[0,1,0],[-s,0,c]])
    def rot_x(a):
        c,s=np.cos(a),np.sin(a); return np.array([[1,0,0],[0,c,-s],[0,s,c]])
    R = rot_x(np.radians(-20)) @ rot_y(np.radians(35))
    v = v @ R.T
    faces = mesh.faces
    fn = mesh.face_normals @ R.T

    # Lighting: key light from upper-left-front.
    L = np.array([-0.5, 0.7, 0.8]); L = L/np.linalg.norm(L)
    diff = np.clip(fn @ L, 0, 1)
    # Specular-ish boost for low roughness / high metal (cheap).
    spec = np.clip(fn @ L, 0, 1) ** (4 + (1-rough)*40) * (0.3 + 0.7*metal)

    bc = np.array(base[:3])
    em = np.array(emiss) * min(estr, 6.0) * 0.18   # scaled emissive contribution
    amb = 0.18 * bc
    face_col = amb[None,:] + bc[None,:]*diff[:,None]*0.9 + spec[:,None]*np.array([1,1,1])*0.6 + em[None,:]
    face_col = np.clip(face_col, 0, 1)

    # Painter's algorithm by mean face depth (z after rotation; larger z = nearer).
    tri = v[faces]
    depth = tri[:,:,2].mean(axis=1)
    order = np.argsort(depth)

    fig = plt.figure(figsize=(6,6), dpi=120)
    ax = fig.add_subplot(111)
    ax.set_facecolor((0.06,0.07,0.09))
    fig.patch.set_facecolor((0.06,0.07,0.09))
    from matplotlib.collections import PolyCollection
    polys = [tri[i][:,:2] for i in order]
    cols = face_col[order]
    pc = PolyCollection(polys, facecolors=cols, edgecolors='none', antialiased=True)
    ax.add_collection(pc)
    lim = 1.15
    ax.set_xlim(-lim,lim); ax.set_ylim(-lim,lim)
    ax.set_aspect('equal'); ax.axis('off')
    ax.set_title("X3Native  weapon: %s" % key, color='white', fontsize=13, pad=8)
    # caption with the identity numbers
    cap = "base=[%.2f,%.2f,%.2f]  metal=%.2f  rough=%.2f  emissive=[%.2f,%.2f,%.2f] x%.1f" % (
        base[0],base[1],base[2], metal, rough, emiss[0],emiss[1],emiss[2], estr)
    fig.text(0.5, 0.04, cap, color=(0.7,0.75,0.8), fontsize=7, ha='center')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    fig.savefig(out, facecolor=fig.get_facecolor())
    plt.close(fig)
    print("[showcase]", key, "->", out, "faces=", len(faces))

if __name__ == "__main__":
    main()
