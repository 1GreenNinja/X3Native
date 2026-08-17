"""Scratch: search the armory galleries.json for jetpack-ish meshes. Not for commit."""
import json
import sys

g = json.load(open(r"C:\Users\Tim\AppData\Local\Temp\gal.json", encoding="utf-8"))
print("packs:", g["pack_count"], "meshes:", g["mesh_count"])

packs = g.get("packs")
terms = [t.lower() for t in sys.argv[1:]] or ["jetpack"]

for p in packs:
    for it in p.get("items") or []:
        nm = (it.get("name") or "").lower()
        for t in terms:
            if t in nm:
                print(p.get("pack"), "|", it.get("name"), "|", it.get("glb"))
                break
