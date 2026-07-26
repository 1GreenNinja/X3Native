#!/usr/bin/env node
/**
 * glb-merge-anims.mjs — offline "library unlock" tool.
 *
 * Most CC0 character packs ship the skinned MESH in a T-pose GLB and their
 * motion as SEPARATE single-clip GLBs on the SAME skeleton (e.g. CombatGirls:
 * `Humanoid_F.glb` is the T-pose body, `SS_Idle.glb`/`SS_Walk.glb`/… each carry
 * one animation and no mesh). Our Babylon loader (src/render/unit-hero.ts) only
 * plays animations EMBEDDED in the mesh GLB it loads. This tool fuses a mesh GLB
 * + N clip GLBs into ONE self-contained GLB whose AnimationGroups the existing
 * loader plays with ZERO engine changes.
 *
 * HOW IT WORKS (dependency-free, direct glTF-JSON merge):
 *   For each clip GLB, each of its animations is appended to the mesh document —
 *   the sampler input/output accessors are copied byte-for-byte into the mesh's
 *   BIN blob (new tightly-packed bufferViews + accessors), and every channel is
 *   RE-TARGETED from the clip's node index to the mesh's node index BY BONE NAME
 *   (mesh and clips share one Unity/UE skeleton, so node names match). Channels
 *   whose target bone doesn't exist in the mesh (helper/IK nodes) are dropped.
 *   Each output AnimationGroup is named from --names (positional, across all
 *   collected animations) or, when --names runs out, its source animation name.
 *
 * CLI:
 *   node tools/glb-merge-anims.mjs \
 *     --mesh  <meshGlb> \
 *     --clips <clipA.glb,clipB.glb,...> \
 *     --names idle,walk,attack,death \   # optional; positional across all anims
 *     --out   <out.glb>
 *
 * EXAMPLE (CombatGirls → an animated town woman):
 *   node tools/glb-merge-anims.mjs \
 *     --mesh  "Z:/_glb/tech/CombatGirlsSwordShieldCharacterPack/CombatGirlsCharacterPack/Humanoid_Bot/Models/Humanoid_F.glb" \
 *     --clips "<pack>/CombatGirl_Shield/Animations/Normal/SS_Idle.glb,...SS_Walk.glb,...SS_Attack1.glb,...SS_Die.glb" \
 *     --names idle,walk,attack,death \
 *     --out   public/assets/models/characters/library/humans/CombatGirl_F.glb
 *
 * See tools/README-glb-merge.md for the full writeup.
 */
import { readFileSync, writeFileSync } from "node:fs";

const GLB_MAGIC = 0x46546c67; // "glTF"
const JSON_TYPE = 0x4e4f534a; // "JSON"
const BIN_TYPE = 0x004e4942;  // "BIN\0"

const COMP_BYTES = { 5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4 };
const TYPE_COUNT = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT2: 4, MAT3: 9, MAT4: 16 };

const pad4 = (n) => (n + 3) & ~3;

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--mesh") out.mesh = argv[++i];
    else if (a === "--clips") out.clips = argv[++i];
    else if (a === "--names") out.names = argv[++i];
    else if (a === "--out") out.out = argv[++i];
    else throw new Error(`unknown arg: ${a}`);
  }
  if (!out.mesh || !out.clips || !out.out) {
    throw new Error("usage: --mesh <glb> --clips <a.glb,b.glb,...> [--names idle,walk,...] --out <glb>");
  }
  return out;
}

/** Parse a .glb into { json, bin(Buffer) }. Supports the standard 2-chunk layout. */
function readGlb(path) {
  const buf = readFileSync(path);
  if (buf.readUInt32LE(0) !== GLB_MAGIC) throw new Error(`not a GLB: ${path}`);
  const total = buf.readUInt32LE(8);
  let off = 12;
  let json = null;
  let bin = Buffer.alloc(0);
  while (off + 8 <= total) {
    const chunkLen = buf.readUInt32LE(off);
    const chunkType = buf.readUInt32LE(off + 4);
    const start = off + 8;
    const data = buf.subarray(start, start + chunkLen);
    if (chunkType === JSON_TYPE) json = JSON.parse(data.toString("utf8"));
    else if (chunkType === BIN_TYPE) bin = Buffer.from(data); // own copy
    off = start + chunkLen;
  }
  if (!json) throw new Error(`no JSON chunk in ${path}`);
  return { json, bin };
}

function writeGlb(path, json, bin) {
  const jsonBuf = Buffer.from(JSON.stringify(json), "utf8");
  const jsonChunk = Buffer.concat([jsonBuf, Buffer.alloc(pad4(jsonBuf.length) - jsonBuf.length, 0x20)]);
  const binChunk = Buffer.concat([bin, Buffer.alloc(pad4(bin.length) - bin.length, 0)]);
  const total = 12 + 8 + jsonChunk.length + 8 + binChunk.length;
  const header = Buffer.alloc(12);
  header.writeUInt32LE(GLB_MAGIC, 0);
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(total, 8);
  const jsonHdr = Buffer.alloc(8);
  jsonHdr.writeUInt32LE(jsonChunk.length, 0);
  jsonHdr.writeUInt32LE(JSON_TYPE, 4);
  const binHdr = Buffer.alloc(8);
  binHdr.writeUInt32LE(binChunk.length, 0);
  binHdr.writeUInt32LE(BIN_TYPE, 4);
  writeFileSync(path, Buffer.concat([header, jsonHdr, jsonChunk, binHdr, binChunk]));
}

const accessorByteLength = (acc) =>
  acc.count * COMP_BYTES[acc.componentType] * TYPE_COUNT[acc.type];

function merge({ mesh, clipPaths, names, out }) {
  const meshDoc = readGlb(mesh);
  const mjson = meshDoc.json;
  const parts = [meshDoc.bin]; // BIN pieces, concatenated at the end
  let binLen = meshDoc.bin.length;

  mjson.accessors ??= [];
  mjson.bufferViews ??= [];
  mjson.animations ??= [];
  mjson.buffers ??= [{ byteLength: binLen }];

  // Bone name -> mesh node index (the retarget table).
  const meshNodeByName = new Map();
  mjson.nodes.forEach((n, i) => { if (n.name !== undefined) meshNodeByName.set(n.name, i); });

  let nameIdx = 0;
  let animCount = 0;

  for (const clipPath of clipPaths) {
    const clip = readGlb(clipPath);
    const cj = clip.json;
    const cbin = clip.bin;
    for (const anim of cj.animations ?? []) {
      // Copy an accessor (tightly packed) from the clip BIN into the mesh BIN.
      const accMap = new Map();
      const copyAccessor = (ai) => {
        if (accMap.has(ai)) return accMap.get(ai);
        const acc = cj.accessors[ai];
        const bv = cj.bufferViews[acc.bufferView];
        const byteLen = accessorByteLength(acc);
        const srcStart = (bv.byteOffset ?? 0) + (acc.byteOffset ?? 0);
        const slice = cbin.subarray(srcStart, srcStart + byteLen);
        if (binLen % 4 !== 0) { const p = 4 - (binLen % 4); parts.push(Buffer.alloc(p)); binLen += p; }
        const bvIndex = mjson.bufferViews.length;
        mjson.bufferViews.push({ buffer: 0, byteOffset: binLen, byteLength: slice.length });
        parts.push(Buffer.from(slice));
        binLen += slice.length;
        const newAcc = {
          bufferView: bvIndex, byteOffset: 0,
          componentType: acc.componentType, count: acc.count, type: acc.type,
        };
        if (acc.min) newAcc.min = acc.min.slice();
        if (acc.max) newAcc.max = acc.max.slice();
        if (acc.normalized) newAcc.normalized = acc.normalized;
        const idx = mjson.accessors.length;
        mjson.accessors.push(newAcc);
        accMap.set(ai, idx);
        return idx;
      };

      const newSamplers = [];
      const samplerMap = new Map();
      const newChannels = [];
      let dropped = 0;
      for (const ch of anim.channels ?? []) {
        const tgt = ch.target?.node;
        const tgtName = tgt !== undefined ? cj.nodes[tgt]?.name : undefined;
        const meshNode = tgtName !== undefined ? meshNodeByName.get(tgtName) : undefined;
        if (meshNode === undefined) { dropped++; continue; }
        let s = samplerMap.get(ch.sampler);
        if (s === undefined) {
          const src = anim.samplers[ch.sampler];
          s = newSamplers.length;
          newSamplers.push({
            input: copyAccessor(src.input),
            output: copyAccessor(src.output),
            interpolation: src.interpolation ?? "LINEAR",
          });
          samplerMap.set(ch.sampler, s);
        }
        newChannels.push({ sampler: s, target: { node: meshNode, path: ch.target.path } });
      }

      const outName = nameIdx < names.length ? names[nameIdx] : (anim.name || `clip${nameIdx}`);
      nameIdx++;
      if (newChannels.length === 0) {
        console.warn(`  ! "${outName}" from ${clipPath}: no channels matched mesh bones — skipped`);
        continue;
      }
      // Peak keyframe count (proves real motion: >2 for anything but a static pose).
      let maxKf = 0;
      for (const s of newSamplers) {
        const c = mjson.accessors[s.input].count;
        if (c > maxKf) maxKf = c;
      }
      mjson.animations.push({ name: outName, samplers: newSamplers, channels: newChannels });
      animCount++;
      console.log(`  + "${outName}"  channels=${newChannels.length} (dropped ${dropped}) keyframes=${maxKf}`);
    }
  }

  const finalBin = Buffer.concat(parts, binLen);
  mjson.buffers[0].byteLength = finalBin.length;
  delete mjson.buffers[0].uri;
  writeGlb(out, mjson, finalBin);
  console.log(`\nWrote ${out} — ${animCount} animation group(s), buffer ${finalBin.length} bytes.`);
  if (animCount === 0) { console.error("ERROR: no animations merged."); process.exit(1); }
}

const args = parseArgs(process.argv.slice(2));
merge({
  mesh: args.mesh,
  clipPaths: args.clips.split(",").map((s) => s.trim()).filter(Boolean),
  names: (args.names ? args.names.split(",").map((s) => s.trim()) : []),
  out: args.out,
});
