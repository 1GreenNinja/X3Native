#pragma once
// GENERAL versioned checkpoint save/load (every genre needs it; EFLZ first consumer).
//
// Game/slice code only — engine/ stays pure: this module depends only on plain PODs
// + std file I/O, never on Vulkan/physics/Scene. It captures a MEANINGFUL, resumable
// CHECKPOINT (not a full world snapshot): the player's transform + health, the
// inventory (which weapons, the equipped one, per-weapon ammo), the current level +
// floor, the objective cursor, the rescue-timer/active states, and the key world
// flags (doors opened / keypads solved / enemies cleared per floor). It deliberately
// does NOT serialize physics/debris/per-body transforms — a checkpoint puts the
// player back at a sensible resume point, it does not freeze every rigid body.
//
// FORMAT (binary, versioned, endian-fixed little-endian — matches the dev target):
//   [ Header ]  magic 'X','3','S','V'  (uint32 LE)  +  uint32 version
//   [ Body   ]  the SaveState fields, written field-by-field via a tiny POD writer
//   [ Footer ]  uint32 checksum (additive over the body bytes) for corruption detect
// load() validates the magic, the version (== kSaveVersion), the declared sizes, and
// the checksum, FAILING GRACEFULLY (returns false, leaves out untouched) on any
// mismatch / truncation / corruption — no crash, no UB. Old/newer versions are
// rejected cleanly (no silent partial reads).
//
// The schema is a single struct so the host fills it from its live systems (Player /
// Arsenal / Level1Game / RescueSystem / Spire floors) before saveCheckpoint(), and
// applies it back after loadCheckpoint(). The mapping host-systems <-> SaveState is
// the host's job (see app/main.cpp save/load hooks + the --test-saveload self-test);
// keeping it a plain struct is what makes the round-trip field-by-field testable.

#include <cstdint>
#include <string>
#include <vector>

namespace x3::save {

// File format magic + version. Bump kSaveVersion on ANY schema change so old files
// are rejected instead of mis-read. The magic is 'X','3','S','V' little-endian.
constexpr uint32_t kSaveMagic   = 0x5653'3358u;   // 'X3SV' LE  ('X'|'3'<<8|'S'<<16|'V'<<24)
constexpr uint32_t kSaveVersion = 1;

// Max sane counts the loader will accept (a guard against a corrupt length field
// allocating gigabytes / looping forever). Generous vs. EFLZ's real numbers.
constexpr uint32_t kMaxWeapons   = 64;
constexpr uint32_t kMaxRescue    = 64;
constexpr uint32_t kMaxFloors    = 64;
constexpr uint32_t kMaxStrLen    = 256;

// One inventory weapon's persisted runtime state (parallel to the host's roster).
struct WeaponSave {
    uint32_t ammoInMag = 0;   // rounds chambered
    uint32_t reserve   = 0;   // spare rounds
};

// One rescue victim's persisted lifecycle (mirrors RescueVictim::SaveState, but in
// the save module's own POD so this module never depends on rescue.h).
struct RescueSave {
    uint32_t state    = 0;    // 0 Captive / 1 Companion / 2 Expired (VictimState)
    float    timeLeft = 0.0f; // seconds remaining (Captive)
};

// Per-floor world-flag snapshot: how many doors opened, keypads solved, and enemies
// cleared on that floor — the "progress on this floor" the player resumes into.
struct FloorFlags {
    uint32_t floorIndex     = 0;   // L1Floor index (0..6 for the 7-floor Spire)
    uint32_t doorsOpened    = 0;   // doors transitioned to Open on this floor
    uint32_t keypadsSolved  = 0;   // coded doors unlocked via the keypad
    uint32_t enemiesCleared = 0;   // enemies killed on this floor
    uint32_t enemiesTotal   = 0;   // enemies authored on this floor (for "cleared?" UI)
};

// ---------------------------------------------------------------------------
// The checkpoint schema. A clear, flat POD-ish struct (vectors for the variable
// arrays). Filled by the host from its live systems and applied back on load.
// ---------------------------------------------------------------------------
struct SaveState {
    // ---- Player transform + look ----
    float    playerX = 0.0f, playerY = 0.0f, playerZ = 0.0f;  // feet world pos
    float    playerYaw = 0.0f, playerPitch = 0.0f;            // look angles (radians)
    // ---- Player health ----
    int32_t  playerHp    = 100;
    int32_t  playerMaxHp = 100;

    // ---- Inventory ----
    uint32_t equippedWeapon = 0;        // selected roster index
    std::vector<WeaponSave> weapons;    // per-weapon ammo (roster order)

    // ---- Progression ----
    uint32_t levelIndex     = 0;        // which level (EFLZ Level 1 = 0)
    uint32_t currentFloor   = 0;        // L1Floor index the player is on (0..6)
    uint32_t objectiveIndex = 0;        // ObjectiveSystem cursor (kNoObjective stored as 0xFFFFFFFF)
    uint32_t levelComplete  = 0;        // 1 once the level WIN beat fired

    // ---- Rescue (timers / active gating) ----
    uint32_t rescueHubReached = 0;      // 1 once the F2 ward hub started the clocks
    std::vector<RescueSave> rescue;     // per-victim lifecycle + remaining time

    // ---- World flags (doors / keypads / enemies cleared, per floor) ----
    std::vector<FloorFlags> floors;
};

// Write `state` to `path` in the versioned binary format. Returns true on success.
// Creates/overwrites the file. On any I/O error returns false (and the file may be
// left partially written — load() rejects truncation, so a half-write won't load).
bool saveCheckpoint(const std::string& path, const SaveState& state);

// Read a checkpoint from `path` into `out`. Returns true ONLY if the file exists, the
// magic + version + sizes + checksum all validate, and the body is fully present.
// On ANY failure (missing file, bad magic, version mismatch, truncation, corrupt
// length / checksum) returns false and leaves `out` UNCHANGED — never throws, never
// crashes, never reads out of bounds.
bool loadCheckpoint(const std::string& path, SaveState& out);

// Headless self-test (--test-saveload). Builds a representative SaveState, round-trips
// it through a temp file, asserts every field restores exactly, asserts a mutated live
// state is overwritten by the load, and asserts corrupted/old/short files are rejected
// gracefully (no crash). Prints "saveload: X/Y passed"; returns true iff all pass. No
// window / Vulkan / physics. Mirrors the other --test-* self-tests.
bool runSaveLoadSelfTest();

} // namespace x3::save
