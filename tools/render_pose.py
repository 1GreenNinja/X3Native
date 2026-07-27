"""
render_pose.py - headless Blender still of a rig posed at a named action/frame.
Proof that a baked one-shot clip (Attack/Hit/Death) is a real non-idle pose.

    blender-launcher.exe --background --python render_pose.py -- \
        <rig_anim.glb> <out.png> <ActionName> <frac0..1>

Reports via <out>.log + <out>.done (Store-Blender detaches). Clean-room.
"""
import bpy, sys, os, math
from mathutils import Vector

ARGV = sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else []
RIG, OUT, ACTION = ARGV[0], ARGV[1], ARGV[2]
FRAC = float(ARGV[3]) if len(ARGV) > 3 else 0.5
LOG, DONE = OUT+".log", OUT+".done"
_log=[]
def log(*a):
    s="[render] "+" ".join(str(x) for x in a); _log.append(s); print(s)
def flush(st):
    open(LOG,"w",encoding="utf-8").write("\n".join(_log))
    open(DONE,"w",encoding="utf-8").write(st)

def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=RIG)
    arm=next((o for o in bpy.data.objects if o.type=='ARMATURE'),None)
    meshes=[o for o in bpy.data.objects if o.type=='MESH']
    if not arm: raise RuntimeError("no armature")
    # assign the requested action
    act=bpy.data.actions.get(ACTION)
    if not act:
        # case-insensitive
        act=next((a for a in bpy.data.actions if a.name.lower()==ACTION.lower()),None)
    if not act: raise RuntimeError("action not found: "+ACTION+" have "+str([a.name for a in bpy.data.actions]))
    if not arm.animation_data: arm.animation_data_create()
    arm.animation_data.action=act
    fs,fe=act.frame_range
    fr=int(fs+(fe-fs)*FRAC)
    bpy.context.scene.frame_set(fr)
    log("action",ACTION,"frames",fs,fe,"-> frame",fr)
    # bounds of meshes at this pose (use evaluated world verts)
    dg=bpy.context.evaluated_depsgraph_get()
    lo=Vector((1e9,1e9,1e9)); hi=Vector((-1e9,-1e9,-1e9))
    for m in meshes:
        me=m.evaluated_get(dg)
        for v in me.bound_box:
            w=me.matrix_world@Vector(v)
            lo=Vector((min(lo[i],w[i]) for i in range(3)))
            hi=Vector((max(hi[i],w[i]) for i in range(3)))
    ctr=(lo+hi)*0.5; size=(hi-lo); rad=max(size.x,size.y,size.z)*0.5 or 1.0
    log("bounds ctr",[round(c,2) for c in ctr],"rad",round(rad,2))
    # camera: front-ish 3/4 view (-Y and +X), slightly above center
    cam_d=bpy.data.cameras.new("Cam"); cam=bpy.data.objects.new("Cam",cam_d)
    bpy.context.scene.collection.objects.link(cam)
    cx=ctr.x+rad*2.2; cy=ctr.y-rad*3.2; cz=ctr.z+rad*1.1
    cam.location=Vector((cx,cy,cz))
    # canonical Blender look-at: point local -Z at the target, keep +Y up
    cam.rotation_euler=(ctr-cam.location).to_track_quat('-Z','Y').to_euler()
    cam_d.lens=50
    bpy.context.scene.camera=cam
    # lights: a key sun + fill
    for ang,en in ((( math.radians(50),0,math.radians(30)),4.0),((math.radians(60),0,math.radians(-120)),1.5)):
        ld=bpy.data.lights.new("L",'SUN'); ld.energy=en
        lo_=bpy.data.objects.new("L",ld); lo_.rotation_euler=ang
        bpy.context.scene.collection.objects.link(lo_)
    # world bg mid-grey
    w=bpy.data.worlds.new("W"); bpy.context.scene.world=w; w.use_nodes=True
    w.node_tree.nodes["Background"].inputs[0].default_value=(0.05,0.06,0.08,1)
    sc=bpy.context.scene
    for eng in ('BLENDER_EEVEE_NEXT','BLENDER_EEVEE','BLENDER_WORKBENCH'):
        try: sc.render.engine=eng; log("engine",eng); break
        except Exception: continue
    sc.render.resolution_x=900; sc.render.resolution_y=1000
    sc.render.filepath=OUT; sc.render.image_settings.file_format='PNG'
    os.makedirs(os.path.dirname(OUT) or ".",exist_ok=True)
    bpy.ops.render.render(write_still=True)
    log("WROTE",OUT)

if __name__=="__main__":
    st="OK"
    try: main()
    except Exception as e:
        import traceback; log("FAIL",e); log(traceback.format_exc()); st="FAIL: "+str(e)
    flush(st)
