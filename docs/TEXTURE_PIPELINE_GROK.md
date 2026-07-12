# GROK -> GAME TEXTURE PIPELINE (Tim's directive, 2026-07-11)
> "Once we can reliably generate textures from Grok images.. we will be in business."

GOAL: a reliable, repeatable pipeline from a Grok/gen concept IMAGE (or video frame) to a
game-ready PBR surface set in the engine. This generalizes what rifthub did piecemeal
(SD3.5 text->texture forge; membrane video->flipbook atlas) into the fleet's art department.

## Pipeline stages
1. SOURCE: Grok concept image / video frame (saved to D:\GameDev\SimCityLLM2\refs\concepts
   or the target repo's docs/reference). Grok URLs are session-locked — always SAVE the file.
2. EXTRACT: crop the material region of interest (e.g. the riveted plate band of the gate
   still) at the largest available resolution.
3. TILE: make it seamless — SD3.5 img2img with tiling enabled (the proven local checkout at
   D:\GameDev\SD_Models\sd35 supports it) or offset+inpaint the seams. Gate: a 2x2 repeat
   shows no visible seam.
4. PBR DECOMPOSE into the engine's surface_library layout:
   - albedo: delight the crop (flatten baked lighting; img2img "albedo, flat lighting" pass
     or classic highlight/shadow suppression)
   - height: luminance/AI height estimation -> normal map (verify GREEN CHANNEL orientation
     against an existing known-good set in-engine!)
   - roughness: derived mask (inverted smoothness reads; hand-bias per material family)
   - metallic: flat or masked per family; ao: from height.
5. LAND: G:\Assets\X3Native\surface_library\<set>\ (fleet share — LFS is dead), matching the
   existing set folder layout exactly (see mw_metal_panels_a).
6. VALIDATE IN-ENGINE: apply to a test prim; check tiling scale, normal direction, roughness
   read under the hall lighting; screenshot gate.

## Already-proven pieces to reuse
- tools/forge_gate_textures.py (rifthub worktree): SD3.5 diffusers text->texture; extend
  with an --img2img <crop> mode = stage 3+4a in one.
- tools/make_membrane_flipbook.py (round 4): video->masked atlas (the ANIMATED variant).
- G: distribution convention (FLEET.md), surface_library layout, engine PBR mapping.

## Next step when GPU is free
Extend forge_gate_textures.py with: --from-image <crop.png> (img2img w/ tile), --delight,
--derive-maps. Test subject: the riveted plate band from docs/reference (rifthub worktree)
portal stills -> gate_ring_plate_v2. Then it's a one-command loop for ANY concept Tim drops.
