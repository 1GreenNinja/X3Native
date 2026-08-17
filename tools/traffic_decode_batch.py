# W-TRAFFIC: decode the armory traffic-roster shortlist out of draco into a
# staging dir (the loader silently drops draco geometry — see
# tools/decode_draco_glb.py's header for the receipt). Resumable; skips
# files already decoded. Then re-audit with tools/traffic_roster_audit.py.
import os, subprocess, sys, urllib.parse

ROOT = r"D:\Assets\_glb"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "assets_staging", "traffic")

SHORTLIST = {
 "OldVan": "prefab_buildings/Urban%20Abandoned%20District/sm_OldVan_01_01.glb",
 "GarbageTruck": "tech/Garbage%20Truck%20HQ/Assets/Garbage_Truck/Model/Garbage_truck.glb",
 "Pickup2_URP": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/Pickup%20Car%202/Pickup%20Car%202.glb",
 "Sedan_Car3": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/Car%203/Car%203.glb",
 "Sedan_Car4": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/Car%204/Car%204.glb",
 "Car7_URP": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%207/car%207.glb",
 "Car12_URP": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%2012/car%2012.glb",
 "Car23_URP": "tech/Complete%20Racing%20Game%20URP%20All%20in%20One/Racing_Game/Models/Cars/New/car%2023/car%2023_New.glb",
 "CompactCar1993": "tech/Compact%20Car%201993/Assets/compact_car_1993/compact_car_1993.glb",
 "MiniCargoTruck": "tech/Mini%20Cargo%20Truck/Assets/MiniCargoTruck/FBX/Truck1.glb",
 "BankTruck": "tech/Armored%20Bank%20Cash%20Truck%20Mobile/Assets/Armored%20Bank%20Cash%20Truck/Data/Art/bank_truck.glb",
 "ModernSports5": "tech/Modern%20Sports%20Car%205/Assets/Car5/FBXs/Car5.glb",
}

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    fail = 0
    for name, rel in SHORTLIST.items():
        src = os.path.join(ROOT, urllib.parse.unquote(rel).replace("/", os.sep))
        dst = os.path.join(OUT, name + ".glb")
        if os.path.exists(dst) and os.path.getsize(dst) > 0:
            print(f"skip {name} (already decoded)")
            continue
        if not os.path.exists(src):
            print(f"MISSING {src}")
            fail += 1
            continue
        print(f"decode {name} <- {src}")
        r = subprocess.run(["npx", "@gltf-transform/cli", "copy", src, dst],
                           shell=True, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(dst):
            print(f"  FAILED rc={r.returncode}\n{r.stdout[-400:]}\n{r.stderr[-400:]}")
            fail += 1
    print(f"done, {fail} failures")
    sys.exit(1 if fail else 0)
