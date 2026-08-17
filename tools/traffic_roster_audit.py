# Traffic roster audit — W-TRAFFIC receipt tool.
# Inspects candidate vehicle GLBs (armory + repo) WITHOUT loading the engine:
# draco (the loader SILENTLY drops draco geometry — commit 7158cc5e's invisible
# bench), texture presence, tri count, metre-scale bbox with the full node
# hierarchy applied, and wheel-node availability (wheel spin needs named hubs).
# Usage: python tools/traffic_roster_audit.py [glb ...]   (no args = the candidate table)
import json, struct, os, sys, urllib.parse

ROOT = r"D:\Assets\_glb"

def mat_mul(a, b):
    r = [0.0] * 16
    for i in range(4):
        for j in range(4):
            r[j * 4 + i] = sum(a[k * 4 + i] * b[j * 4 + k] for k in range(4))
    return r

def trs(node):
    if "matrix" in node:
        return list(node["matrix"])
    t = node.get("translation", [0, 0, 0])
    q = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    x, y, z, w = q
    R = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
         0, 0, 0, 1]
    for c in range(3):
        for rr in range(3):
            R[c * 4 + rr] *= s[c]
    R[12], R[13], R[14] = t
    return R

def inspect(p):
    if not os.path.exists(p):
        return f"MISSING {p}"
    data = open(p, "rb").read()
    clen, = struct.unpack_from("<I", data, 12)
    js = json.loads(data[20:20 + clen])
    req = js.get("extensionsRequired", [])
    draco = "KHR_draco_mesh_compression" in req
    mats = js.get("materials", [])
    ntex = sum(1 for m in mats if "baseColorTexture" in m.get("pbrMetallicRoughness", {}))
    nimg = len(js.get("images", []))
    nodes = js.get("nodes", [])
    names = [n.get("name", "") for n in nodes]
    wheels = [n for n in names if "wheel" in n.lower() or "tire" in n.lower()]
    mn = [1e18] * 3
    mx = [-1e18] * 3
    accs = js.get("accessors", [])
    meshes = js.get("meshes", [])

    def walk(ni, parent):
        n = nodes[ni]
        world = mat_mul(parent, trs(n))
        if "mesh" in n and not draco:
            for prim in meshes[n["mesh"]]["primitives"]:
                ai = prim["attributes"].get("POSITION")
                if ai is None:
                    continue
                a = accs[ai]
                if "min" in a and "max" in a:
                    for cx in (a["min"][0], a["max"][0]):
                        for cy in (a["min"][1], a["max"][1]):
                            for cz in (a["min"][2], a["max"][2]):
                                wx = world[0] * cx + world[4] * cy + world[8] * cz + world[12]
                                wy = world[1] * cx + world[5] * cy + world[9] * cz + world[13]
                                wz = world[2] * cx + world[6] * cy + world[10] * cz + world[14]
                                for i, v in enumerate((wx, wy, wz)):
                                    mn[i] = min(mn[i], v)
                                    mx[i] = max(mx[i], v)
        for c in n.get("children", []):
            walk(c, world)

    scenes = js.get("scenes", [{}])
    I = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    for rn in scenes[js.get("scene", 0)].get("nodes", []):
        walk(rn, I)
    ntris = -1
    if not draco:
        ntris = sum(accs[pr["indices"]]["count"] // 3
                    for me in meshes for pr in me["primitives"] if "indices" in pr)
    dims = [mx[i] - mn[i] for i in range(3)] if mx[0] > -1e17 else [0, 0, 0]
    skin = len(js.get("skins", []))
    return (f"draco={int(draco)} imgs={nimg} matsTex={ntex}/{len(mats)} tris={ntris} "
            f"skins={skin} dims={dims[0]:.2f}x{dims[1]:.2f}x{dims[2]:.2f} "
            f"minY={mn[1]:.2f} wheels={len(wheels)}:{wheels[:8]}")

CAND = {
 "CompactCar1993": "tech/Compact%20Car%201993/Assets/compact_car_1993/compact_car_1993.glb",
 "TruckLevo_M1035": "tech/Truck%20Levo%20v1/Assets/TruckLevo2/Meshes/SK_Truck_Levo_M1035.glb",
 "TruckLevo_M1590": "tech/Truck%20Levo%20v1/Assets/TruckLevo2/Meshes/SK_Truck_Levo_M1590.glb",
 "MiniCargoTruck": "tech/Mini%20Cargo%20Truck/Assets/MiniCargoTruck/FBX/Truck1.glb",
 "SmallTruck_1": "tech/Industrial%20Small%20Truck%20Free/Assets/IndustrialSmallTruck/Art/fbx/SmallTruck_1.glb",
 "OldVan_01_01": "prefab_buildings/Urban%20Abandoned%20District/sm_OldVan_01_01.glb",
 "Car01_Sport1960": "tech/Car%2001%20Sport%201960/Assets/Andy%20Adam/Vehicles/Car01_Sport1960/Car01_Sport1960.glb",
 "RMCar26": "tech/Realistic%20Mobile%20Car%2026%20Demo/Assets/RealisticMobileCars%20-%20Pro3DModels/RMCar26/Meshes/RMCar26.glb",
 "ModernSports5": "tech/Modern%20Sports%20Car%205/Assets/Car5/FBXs/Car5.glb",
 "SportCar_3": "tech/Sport%20Car%20Free%203/Assets/SportCar/Models/SportCar_3/Normal/SportCar_3.glb",
 "SportCar_4": "tech/Sport%20Car%20Free%204/Assets/SportCar/Models/SportCar_4/SportCar_4.glb",
 "SportCar_5": "tech/Sport%20Car%20Free%205/Assets/SportCar/Models/SportCar_5/SportCar_5.glb",
 "URP_car2": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%202/car%202.glb",
 "URP_Car3": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/Car%203/Car%203.glb",
 "URP_Car4": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/Car%204/Car%204.glb",
 "URP_car6": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%206a/car%206_New.glb",
 "URP_car7": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%207/car%207.glb",
 "URP_car8": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%208/car%208.glb",
 "URP_car12": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%2012/car%2012.glb",
 "URP_car17": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%2017/car%2017_New.glb",
 "URP_car23": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%2023/car%2023_New.glb",
 "URP_Pickup2": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/Pickup%20Car%202/Pickup%20Car%202.glb",
 "PickUp_model": "tech/Pickup%20model/Assets/Pickup/FBX/PickUp.glb",
 "BankTruck": "tech/Armored%20Bank%20Cash%20Truck%20Mobile/Assets/Armored%20Bank%20Cash%20Truck/Data/Art/bank_truck.glb",
 "Chev_Touring": "tech/Touring%20Race%20Car%20Pack%20Demo/Assets/Tourung%20Car%20Pack/Meshes/Chev.glb",
 "GarbageTruck": "tech/Garbage%20Truck%20HQ/Assets/Garbage_Truck/Model/Garbage_truck.glb",
}

if __name__ == "__main__":
    args = sys.argv[1:]
    if args:
        for a in args:
            print(f"{os.path.basename(a):28s} {inspect(a)}")
    else:
        for k, v in CAND.items():
            p = os.path.join(ROOT, urllib.parse.unquote(v).replace("/", os.sep))
            try:
                print(f"{k:18s} {inspect(p)}")
            except Exception as e:
                print(f"{k:18s} ERROR {e}")
