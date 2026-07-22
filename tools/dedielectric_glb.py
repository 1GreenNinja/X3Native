#!/usr/bin/env python3
"""
dedielectric_glb.py

A bad channel repack left metallicRoughness textures broken across several
district GLB libraries (roughness ~0, metallic ~0.7), making concrete render
as a near-mirror and buildings render black under most lighting.

This script forces every material in every .glb under a directory to a flat
satin dielectric:
    - pbrMetallicRoughness.metallicFactor  = 0.0
    - pbrMetallicRoughness.roughnessFactor = 0.85
    - pbrMetallicRoughness.metallicRoughnessTexture removed (set to None)

It does NOT touch baseColorTexture, baseColorFactor, normalTexture,
emissiveTexture, emissiveFactor, or any extensions (e.g.
KHR_materials_emissive_strength survives untouched).

Usage:
    python tools/dedielectric_glb.py <glb_dir>

Skips any file ending in .bak. Recurses into subdirectories. Saves a file
in place only if something actually changed. Prints one line per changed
file plus a final count.
"""
import sys
import os
from pathlib import Path

from pygltflib import GLTF2


def fix_material(mat) -> bool:
    """Force a single material to a satin dielectric. Returns True if changed."""
    changed = False

    pbr = mat.pbrMetallicRoughness
    if pbr is None:
        # No PBR block at all -- nothing to de-metal, and we don't want to
        # fabricate one (that could alter baseColorFactor defaults etc).
        return False

    if pbr.metallicFactor is None or pbr.metallicFactor != 0.0:
        pbr.metallicFactor = 0.0
        changed = True

    if pbr.roughnessFactor is None or pbr.roughnessFactor != 0.85:
        pbr.roughnessFactor = 0.85
        changed = True

    if pbr.metallicRoughnessTexture is not None:
        pbr.metallicRoughnessTexture = None
        changed = True

    return changed


def process_glb(path: Path) -> bool:
    """Load a glb, fix all materials, save in place if changed. Returns True if changed."""
    gltf = GLTF2().load(str(path))

    if not gltf.materials:
        return False

    any_changed = False
    for mat in gltf.materials:
        if fix_material(mat):
            any_changed = True

    if any_changed:
        gltf.save(str(path))

    return any_changed


def find_glbs(root: Path):
    for p in sorted(root.rglob("*.glb")):
        if p.name.lower().endswith(".bak.glb") or p.suffix.lower() == ".bak":
            continue
        # Also skip files literally ending in .bak regardless of case combo
        if p.name.lower().endswith(".bak"):
            continue
        yield p


def main():
    if len(sys.argv) != 2:
        print(f"Usage: python {Path(sys.argv[0]).name} <glb_dir>")
        sys.exit(1)

    root = Path(sys.argv[1])
    if not root.is_dir():
        print(f"Error: not a directory: {root}")
        sys.exit(1)

    changed_count = 0
    total_count = 0

    for glb_path in find_glbs(root):
        total_count += 1
        try:
            if process_glb(glb_path):
                changed_count += 1
                print(f"CHANGED: {glb_path}")
        except Exception as e:
            print(f"ERROR processing {glb_path}: {e}")

    print(f"Done. {changed_count} of {total_count} .glb files changed under {root}")


if __name__ == "__main__":
    main()
