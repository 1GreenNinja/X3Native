// THE CONFECTION ANNEX — per-room content (Phase 3; plan Tasks 7-11). This TU
// exists FROM Phase 2 so the annex core (factory_annex.cpp) never has to be
// touched when the rooms land: build() and tick() already call every hook
// below. Each build hook authors its room's props into ctx.scene (push every
// created mesh handle into ctx.meshes — the annex frees that ONE vector
// uniformly) and records the room's animated/glow entity spans on `room`
// (CONTIGUOUS spans; rifthub law). Each tick hook pokes those spans in place —
// NO per-frame heap.
//
// Phase-2 state: ALL TEN HOOKS ARE DELIBERATELY EMPTY. The self-test asserts
// propEntCount == glowEntCount == 0 per room until Phase 3 fills them (that
// check flips to real span asserts as each room lands).
//
// Phase-3 room map (plan Tasks 7-11 — exact prop tables live in the plan):
//   A (y=2)  MIXTURE ATRIUM  — confection river (water pipeline, raspberry
//            tint), 6 copper vats with stirring arms, pipe canopy, 2 bridges.
//   B (y=15) INVENTION WORKS — 8 whimsy machines (Gum-Stretcher, Fizz
//            Compressor, Idea Bellows, Sprocket Fountain, Wobble Boiler,
//            Button Organ, Notion Centrifuge, The Maybe Machine) + conveyor.
//   C (y=28) FIZZ GALLERY    — 4 glass bubble columns (rising emissive
//            bubbles), 3 ceiling fans, the low-grav zone (trigger 311).
//   D (y=41) SORTING HALL    — orb ring (10 gold, 2 duds), 2 sorter arms,
//            the Chute of Dubious Quality (trigger 310).
//   E (y=54) TUBE JUNCTION   — 5 glass transport tubes, pneumatic capsule,
//            the golden burst dais (trigger 312) + tube ride (313).

#include "factory_annex.h"

namespace x3::game {

// ---- Build hooks (author once; Phase 3 fills) ------------------------------
void buildRoomMixture  (FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void buildRoomInvention(FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void buildRoomFizz     (FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void buildRoomSorting  (FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }
void buildRoomTube     (FactoryRoomCtx& ctx, AnnexRoom& room) { (void)ctx; (void)room; }

// ---- Tick hooks (pose/emissive pokes on the recorded spans; Phase 3) -------
void tickRoomMixture   (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }
void tickRoomInvention (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }
void tickRoomFizz      (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }
void tickRoomSorting   (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }
void tickRoomTube      (Scene& scene, AnnexRoom& room, float t) { (void)scene; (void)room; (void)t; }

} // namespace x3::game
