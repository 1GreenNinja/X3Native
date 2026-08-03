// ECHO INTERIORS implementation — see echo_interiors.h for the design stance
// and the INTEGRATION checklist. Every asset below was bbox-verified with
// tools/glb_audit.py on 2026-07-29 before use (the fleet doctrine: inspect
// geometry before spending draw calls on it).

#include "echo_interiors.h"

#include "../hackables.h"   // WD2 camera saturation (interior cams via ctx.hax)

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Same integer hash as echo_region_builders.cpp's hashi() — REPRODUCED so the
// moved condo-room selection stays bit-identical to the pre-move build (the
// two files may not share anonymous-namespace helpers; the constants are the
// contract, documented in both places).
inline uint32_t hashi(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4;
    n *= 0x27d4eb2du; n ^= n >> 15; return n;
}

// Column-major yaw+uniform-scale placement (the buildXf convention).
inline void yawScaleAt(float x, float y, float z, float yaw, float s, float T[16]) {
    const float c = std::cos(yaw), sn = std::sin(yaw);
    T[0]=c*s;  T[1]=0; T[2]=-sn*s; T[3]=0;
    T[4]=0;    T[5]=s; T[6]=0;     T[7]=0;
    T[8]=sn*s; T[9]=0; T[10]=c*s;  T[11]=0;
    T[12]=x;   T[13]=y; T[14]=z;   T[15]=1;
}

// Shared pack roots (all existing, all already loaded elsewhere in this world).
const char* kMegaDir    = "D:/Assets/_glb/prefab_buildings/Mega Open World City Pack/Assets/Mega City Environment/Models";
const char* kMegaStand  = "D:/Assets/_glb/prefab_buildings/Mega Open World City Pack/Assets/Mega City Environment/Models/Bus Stand";
const char* kMegaSigns  = "D:/Assets/_glb/prefab_buildings/Mega Open World City Pack/Assets/Mega City Environment/Models/SignBoard";
const char* kSeasideProps = "D:/Assets/_glb/tech/FANTASTIC - Seaside Town/Fantastic Seaside Town/3d/Props";
const char* kCondoDir   = "D:/GameDev/EchoHarbor/assets/interiors";
const char* kMeshyProps = "D:/GameDev/EchoHarbor/assets/meshy/props";

bool place(EchoRegion& region, EchoRegionCtx& ctx, const char* dir,
           const char* glb, const float T[16]) {
    auto e = std::make_unique<EnvArtSystem>();
    if (!e->buildFromGlbAt(ctx.device, dir, glb, T)) return false;
    region.addArt(std::move(e));
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Registry tables (host-consumed data; see INTEGRATION 3 in the header).
// ---------------------------------------------------------------------------
const InteriorCell kInteriorCells[3] = {
    { "int_condo_rooms", -20.0f, 848.0f, 4.0f },   // center-stack lobby door
    { "int_noodle_bar",  -72.0f, 746.0f, 4.0f },
    { "int_harbor_shop", 150.0f, 362.0f, 5.0f },
};

const VendorInteraction kVendorInteractions[3] = {
    { -31.5f, 770.5f, 3.0f, "[E] BUY DODOG   $8",            -8 },   // Tess's stall
    {  -8.0f, 735.0f, 3.0f, "[E] BROWSE WARES   (fixer)",     0 },   // Fixer's alley table
    {  12.0f, 771.0f, 3.0f, "[E] DONATE   $5   (the prophet)",-5 },  // Preacher corner
};

// ---------------------------------------------------------------------------
// int_condo_rooms — THE MOVED LIT ROOMS (was buildCrown's condo() loop).
// Same positions, same seeds, same hash, same lab placement — bit-identical
// content, now living in a sub-region so it only draws near the stacks
// (the daylight floating-window slop dies at range by construction).
// ---------------------------------------------------------------------------
void buildCondoRooms(EchoRegion& region, EchoRegionCtx& ctx) {
    static const char* kRooms[] = { "cond_tv.glb", "cond_kitchen.glb", "cond_romance.glb",
                                    "cond_kids.glb", "cond_novelist.glb" };
    int rooms = 0;
    auto room = [&](const char* glb, float x, float y, float z, float yaw, float s) {
        float T[16]; yawScaleAt(x, y, z, yaw, s, T);
        if (place(region, ctx, kCondoDir, glb, T)) ++rooms;
    };
    auto stack = [&](float wx, float wz, float yaw, int cols, int floors, float s,
                     uint32_t seed, bool hasLab) {
        const float gy = ctx.hf.ok() ? ctx.hf.heightAt(wx, wz) : 190.0f;
        const float rw = 3.75f*s, rh = 3.25f*s, c = std::cos(yaw), sn = std::sin(yaw);
        for (int f = 0; f < floors; ++f)
            for (int j = 0; j < cols; ++j) {
                const float lx = (j - (cols-1)*0.5f) * rw;
                const float x = wx + c*lx, z = wz - sn*lx, y = gy + f*rh;
                const char* g = kRooms[hashi(seed + f*7u + j*13u) % 5];
                if (hasLab && f == floors-1 && j == cols-1) g = "cond_lab.glb";
                room(g, x, y, z, yaw, s);
            }
    };
    stack(-100.0f, 842.0f, 3.14159f, 3, 5, 2.0f, 11u, false);
    stack( -20.0f, 842.0f, 3.14159f, 3, 6, 2.0f, 29u, true);
    stack(  60.0f, 842.0f, 3.14159f, 3, 5, 2.0f, 47u, false);
    // LOBBY (the first true walk-in interior): a kitchen-kit room at ground
    // level on the center stack's street side + a ctOS terminal as the
    // elevator panel beside the door.
    { float T[16]; yawScaleAt(-20.0f, ctx.hf.ok()?ctx.hf.heightAt(-20,848):190.0f,
                              850.5f, 0.0f, 2.2f, T);
      place(region, ctx, kCondoDir, "cond_kitchen.glb", T); }
    { float T[16]; yawScaleAt(-24.5f, ctx.hf.ok()?ctx.hf.heightAt(-24,848):190.0f,
                              848.5f, 3.14159f, 0.842f, T);
      place(region, ctx, kMeshyProps, "ctos_terminal.glb", T); }
    // WD2 CAMERA SATURATION: lobby cam over the elevator panel.
    if (ctx.hax) {
        HackableObject cam;
        cam.type = HackableType::Camera;
        cam.pos = { -24.5f, (ctx.hf.ok() ? ctx.hf.heightAt(-24, 848) : 190.0f) + 3.0f, 848.5f };
        cam.label = "CONDO LOBBY CAM";
        ctx.hax->add(cam);
    }
    x3::logInfo("[interiors] int_condo_rooms — " + std::to_string(rooms) +
                " lit rooms + lobby (sub-region gated)");
}

// ---------------------------------------------------------------------------
// int_noodle_bar — the Programmer's haunt on the drag: a bus-shelter shell
// re-dressed as a street noodle bar (counter shelter + flame bowls + sign).
// ---------------------------------------------------------------------------
void buildNoodleBar(EchoRegion& region, EchoRegionCtx& ctx) {
    const float gx = -72.0f, gz = 746.0f;
    const float gy = ctx.hf.ok() ? ctx.hf.heightAt(gx, gz) : 190.0f;
    int built = 0;
    float T[16];
    yawScaleAt(gx, gy, gz, 1.5708f, 1.6f, T);                 // the shelter = the bar
    if (place(region, ctx, kMegaStand, "BusStand03_Model.glb", T)) ++built;
    yawScaleAt(gx + 1.8f, gy, gz, -1.5708f, 1.2f, T);         // inner counter run
    if (place(region, ctx, kMegaStand, "BusStand01_Model.glb", T)) ++built;
    yawScaleAt(gx - 3.4f, gy, gz + 2.6f, 0.0f, 1.0f, T);      // flame bowls flank it
    if (place(region, ctx, kSeasideProps, "SM_PROP_standing_torch_bowl_town.glb", T)) ++built;
    yawScaleAt(gx + 3.4f, gy, gz + 2.6f, 0.0f, 1.0f, T);
    if (place(region, ctx, kSeasideProps, "SM_PROP_standing_torch_bowl_town.glb", T)) ++built;
    yawScaleAt(gx, gy + 3.4f, gz - 1.2f, 1.5708f, 0.8f, T);   // restaurant signboard
    if (place(region, ctx, kMegaSigns, "SB_Resturant.glb", T)) ++built;
    // WD2 CAMERA SATURATION: counter cam inside the shelter.
    if (ctx.hax) {
        HackableObject cam;
        cam.type = HackableType::Camera;
        cam.pos = { gx + 1.2f, gy + 2.8f, gz - 0.8f };
        cam.label = "NOODLE COUNTER CAM";
        ctx.hax->add(cam);
    }
    x3::logInfo("[interiors] int_noodle_bar — " + std::to_string(built) + " pieces");
}

// ---------------------------------------------------------------------------
// int_harbor_shop — a real textured storefront block on the boulevard side.
// Shops01 is a 55x72m strip at native scale; 0.45 seats a believable
// two-story shop row. A ctOS terminal marks the walk-in door.
// ---------------------------------------------------------------------------
void buildHarborShop(EchoRegion& region, EchoRegionCtx& ctx) {
    const float gx = 150.0f, gz = 362.0f;
    const float gy = ctx.hf.ok() ? ctx.hf.heightAt(gx, gz) : 2.0f;
    int built = 0;
    float T[16];
    yawScaleAt(gx, gy, gz, 0.6f, 0.45f, T);                   // storefront block
    if (place(region, ctx, kMegaDir, "Shops01_Model.glb", T)) ++built;
    yawScaleAt(gx - 6.0f, gy, gz - 8.0f, 0.6f, 0.842f, T);    // door kiosk
    if (place(region, ctx, kMeshyProps, "ctos_terminal.glb", T)) ++built;
    // LANE 2 SIGNAGE: the boulevard frontage carries boards too.
    yawScaleAt(gx + 8.5f, gy + 3.2f, gz - 6.5f, 0.6f, 0.7f, T);
    if (place(region, ctx, kMegaSigns, "SB_KarachiPort.glb", T)) ++built;
    yawScaleAt(gx + 82.0f, (ctx.hf.ok() ? ctx.hf.heightAt(gx + 82.0f, gz + 6.0f) : gy) + 3.2f,
               gz + 6.0f, -2.5416f, 0.7f, T);
    if (place(region, ctx, kMegaSigns, "SB_Clifton.glb", T)) ++built;
    // WD2 CAMERA SATURATION: one inside the shop, one on the frontage.
    if (ctx.hax) {
        HackableObject cin;
        cin.type = HackableType::Camera;
        cin.pos = { gx + 2.0f, gy + 3.0f, gz - 2.0f };
        cin.label = "SHOP FLOOR CAM";
        ctx.hax->add(cin);
        HackableObject cout;
        cout.type = HackableType::Camera;
        cout.pos = { gx - 7.0f, gy + 3.4f, gz - 9.0f };
        cout.label = "SHOP DOOR CAM";
        ctx.hax->add(cout);
    }
    x3::logInfo("[interiors] int_harbor_shop — " + std::to_string(built) + " pieces");
}

// ---------------------------------------------------------------------------
// Vendor dressing — ALWAYS-VISIBLE street furniture (called from buildCrown).
// Tess's Meshy cart stays; a bus-stand stall shell + signage grows around it
// so the vendor reads as a BUSINESS from across the plaza (Tim: vendor
// clarity). Fixer + Preacher corners get their own set dressing.
// ---------------------------------------------------------------------------
void buildVendorDressing(EchoRegion& region, EchoRegionCtx& ctx) {
    int built = 0;
    float T[16];
    auto gy = [&](float x, float z){ return ctx.hf.ok() ? ctx.hf.heightAt(x, z) : 190.0f; };
    // Tess's DODOG stand (stall shell behind the cart + sign above).
    yawScaleAt(-31.5f, gy(-31.5f, 770.5f), 770.5f, 3.14159f, 1.45f, T);
    if (place(region, ctx, kMegaStand, "BusStand02_Model.glb", T)) ++built;
    yawScaleAt(-31.5f, gy(-31.5f, 770.5f) + 3.6f, 769.2f, 3.14159f, 0.7f, T);
    if (place(region, ctx, kMegaSigns, "SB_Resturant.glb", T)) ++built;
    // Fixer's alley table (shadow market: shelter + one flame bowl).
    yawScaleAt(-8.0f, gy(-8.0f, 735.0f), 735.0f, 0.5f, 1.2f, T);
    if (place(region, ctx, kMegaStand, "BusStand01_Model.glb", T)) ++built;
    yawScaleAt(-5.4f, gy(-5.4f, 733.5f), 733.5f, 0.0f, 0.9f, T);
    if (place(region, ctx, kSeasideProps, "SM_PROP_standing_torch_bowl_town.glb", T)) ++built;
    // Preacher's corner (twin torch frames — the street pulpit).
    yawScaleAt(10.5f, gy(10.5f, 771.0f), 771.0f, 0.0f, 1.0f, T);
    if (place(region, ctx, kSeasideProps, "SM_PROP_standing_torch_frame_town.glb", T)) ++built;
    yawScaleAt(13.5f, gy(13.5f, 771.0f), 771.0f, 0.0f, 1.0f, T);
    if (place(region, ctx, kSeasideProps, "SM_PROP_standing_torch_frame_town.glb", T)) ++built;
    // LANE 2 SIGNAGE PASS (WD2 punchlist §5): 19 of the pack's 20 boards sat
    // unused while the drag read as empty — the north frontage claims five
    // civic boards + one billboard so the street reads as a shopping drag.
    struct Board { const char* glb; float x, z, lift, yaw, s; };
    static const Board kDragBoards[] = {
        { "SB_BusTerminal.glb",   -14.0f, 752.5f, 3.4f,  3.14159f, 0.70f },
        { "SB_DolmenMall.glb",    -52.0f, 752.5f, 3.6f,  3.14159f, 0.75f },
        { "SB_ConsDepot.glb",      40.0f, 752.5f, 3.4f,  3.14159f, 0.70f },
        { "SB_PoliceStation.glb",  62.0f, 752.5f, 3.5f,  3.14159f, 0.70f },
        { "SB_ClockTower.glb",     20.0f, 764.0f, 3.8f,  3.14159f, 0.70f },
        { "BillBoard_Model.glb",   80.0f, 754.0f, 0.0f, -1.5708f,  1.10f },
    };
    for (const auto& b : kDragBoards) {
        yawScaleAt(b.x, gy(b.x, b.z) + b.lift, b.z, b.yaw, b.s, T);
        if (place(region, ctx, kMegaSigns, b.glb, T)) ++built;
    }
    x3::logInfo("[interiors] vendor dressing — " + std::to_string(built) +
                " stall/sign/torch/board pieces (always visible)");
}

} // namespace x3::game
