# glb-merge-anims — the library unlock

Most CC0 character packs in the fleet asset library ship the skinned **mesh** in
a **T-pose GLB** and their **motion** as **separate single-clip GLBs** authored on
the *same* skeleton. Example — `CombatGirlsSwordShieldCharacterPack`:

```
Humanoid_Bot/Models/Humanoid_F.glb          <- skinned body, T-pose, 0 animations
CombatGirl_Shield/Animations/Normal/SS_Idle.glb   <- 1 animation, no mesh
CombatGirl_Shield/Animations/Normal/SS_Walk.glb
CombatGirl_Shield/Animations/Normal/SS_Attack1.glb
CombatGirl_Shield/Animations/Normal/SS_Die.glb
```

Our Babylon loader (`src/render/unit-hero.ts` → `LoadAssetContainerAsync`) only
plays animations **embedded in the mesh GLB it loads**. Separate clip GLBs are
invisible to it. `glb-merge-anims.mjs` fuses a mesh GLB + N clip GLBs into ONE
self-contained GLB whose `AnimationGroup`s the existing loader plays with **zero
engine changes**.

## Usage

```bash
node tools/glb-merge-anims.mjs \
  --mesh  <meshGlb> \
  --clips <clipA.glb,clipB.glb,...> \
  --names idle,walk,attack,death \   # optional, positional across all anims
  --out   <out.glb>
```

- `--names` is **positional across every animation collected** (in `--clips`
  order). When it runs out, remaining animations keep their **source** name.
  So a pack of 4 single-clip GLBs → `idle,walk,attack,death`, while a single
  multi-clip donor GLB (no `--names`) copies every clip under its own name.
- Windows/Git-Bash: pass `Z:/...` forward-slash paths and prefix the command
  with `MSYS2_ARG_CONV_EXCL="*"` so MSYS doesn't mangle the comma-joined
  `--clips` argument.

## How it works (dependency-free direct glTF-JSON merge)

No `@gltf-transform` dependency — a direct GLB binary merge:

1. Parse the mesh GLB (JSON + BIN) and each clip GLB.
2. For each clip animation, copy its sampler **input/output accessors**
   byte-for-byte into the mesh's BIN blob as fresh, tightly-packed bufferViews +
   accessors (4-byte aligned).
3. **Re-target every channel by BONE NAME**: `clip.nodes[channel.target.node].name`
   → the mesh node index with the same name. The mesh and its clips share one
   Unity/UE skeleton, so names match. Channels whose target bone is absent from
   the mesh (helper/IK/armature-root nodes) are dropped.
4. Append the rebuilt animations (named from `--names` / source) to the mesh doc
   and write a valid 2-chunk GLB (JSON padded with spaces, BIN with zeros).

## Proof (CombatGirls)

```
node tools/glb-merge-anims.mjs \
  --mesh  "Z:/_glb/tech/CombatGirlsSwordShieldCharacterPack/CombatGirlsCharacterPack/Humanoid_Bot/Models/Humanoid_F.glb" \
  --clips "<pack>/CombatGirl_Shield/Animations/Normal/SS_Idle.glb,...SS_Walk.glb,...SS_Attack1.glb,...SS_Die.glb" \
  --names idle,walk,attack,death \
  --out   public/assets/models/characters/library/humans/CombatGirl_F.glb
```

- **Before:** `Humanoid_F.glb` — animations: 0 (T-pose).
- **After:** `CombatGirl_F.glb` — animations: 4
  - `idle`   keyframes = 91
  - `walk`   keyframes = 35
  - `attack` keyframes = 46
  - `death`  keyframes = 61

Keyframes > 2 on every clip proves real motion (a static pose has ≤ 2). 21
helper-node channels dropped per clip (69-node clip → 62-node mesh); 183 real
bone channels retained.

## Also used for

The `Warrior maiden` pack's **Skin2** shipped in T-pose (1 clip, 2 keyframes).
Its 16 real clips were fused in from **Skin1** (identical 243-node skeleton, so
0 channels dropped):

```
node tools/glb-merge-anims.mjs \
  --mesh  public/assets/models/characters/library/humans/WarriorMaiden_Skin2.glb \
  --clips public/assets/models/characters/library/humans/WarriorMaiden_Skin1.glb \
  --out   public/assets/models/characters/library/humans/WarriorMaiden_Skin2.glb
```
