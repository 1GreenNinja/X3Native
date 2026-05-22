// GENERAL versioned checkpoint save/load. See app/save.h.
//
// Clean-room: built from std <fstream> + plain PODs only. No engine/physics/Vulkan
// dependency, no purchased / id-Tech source consulted. The format is a small, self-
// describing little-endian binary blob with a magic + version header and an additive
// checksum footer so corruption / truncation / version drift are caught and rejected
// gracefully (load returns false; never throws / crashes / reads OOB).

#include "save.h"

#include "engine/core/x3_log.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace x3::save {

// ===========================================================================
// Little-endian byte (de)serialization into/out of a std::vector<uint8_t>. We
// serialize to an in-memory buffer first so we can compute the checksum over the
// exact body bytes, then write header + body + footer in one pass. Reading mirrors
// it with strict bounds checks (a short/corrupt buffer fails the read, never reads
// past the end). All multi-byte values are written LE regardless of host endianness
// (the dev target is x86-64 LE, but this keeps files portable + explicit).
// ===========================================================================
namespace {

// ---- Writer ----
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v        & 0xFF));
    b.push_back((uint8_t)((v >> 8 ) & 0xFF));
    b.push_back((uint8_t)((v >> 16) & 0xFF));
    b.push_back((uint8_t)((v >> 24) & 0xFF));
}
void putI32(std::vector<uint8_t>& b, int32_t v) { putU32(b, (uint32_t)v); }
void putF32(std::vector<uint8_t>& b, float v) {
    uint32_t bits; std::memcpy(&bits, &v, sizeof(bits)); putU32(b, bits);
}

// ---- Reader (cursor + bounds-checked). On any over-read, `ok` latches false and
// subsequent gets return zero, so a corrupt/short buffer can never read OOB. ----
struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t cur = 0;
    bool ok = true;

    bool need(size_t bytes) {
        if (!ok) return false;
        if (cur + bytes > n) { ok = false; return false; }
        return true;
    }
    uint32_t getU32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)p[cur]
                   | ((uint32_t)p[cur + 1] << 8)
                   | ((uint32_t)p[cur + 2] << 16)
                   | ((uint32_t)p[cur + 3] << 24);
        cur += 4;
        return v;
    }
    int32_t getI32() { return (int32_t)getU32(); }
    float   getF32() {
        uint32_t bits = getU32();
        float v; std::memcpy(&v, &bits, sizeof(v)); return v;
    }
};

// Additive 32-bit checksum over a byte range (wraps mod 2^32). Cheap + adequate to
// catch the truncation / single-bit corruption this guard is for (not a crypto MAC).
uint32_t checksum(const uint8_t* p, size_t n) {
    uint32_t s = 0x9E3779B9u;             // arbitrary nonzero seed
    for (size_t i = 0; i < n; ++i) s = (s + p[i]) * 16777619u + 1u;
    return s;
}

} // namespace

// ===========================================================================
// saveCheckpoint
// ===========================================================================
bool saveCheckpoint(const std::string& path, const SaveState& st) {
    // ---- Serialize the BODY to a buffer (so we can checksum exactly these bytes).
    std::vector<uint8_t> body;
    body.reserve(256);

    // Player.
    putF32(body, st.playerX); putF32(body, st.playerY); putF32(body, st.playerZ);
    putF32(body, st.playerYaw); putF32(body, st.playerPitch);
    putI32(body, st.playerHp); putI32(body, st.playerMaxHp);

    // Inventory.
    putU32(body, st.equippedWeapon);
    putU32(body, (uint32_t)st.weapons.size());
    for (const auto& w : st.weapons) { putU32(body, w.ammoInMag); putU32(body, w.reserve); }

    // Progression.
    putU32(body, st.levelIndex);
    putU32(body, st.currentFloor);
    putU32(body, st.objectiveIndex);
    putU32(body, st.levelComplete);

    // Rescue.
    putU32(body, st.rescueHubReached);
    putU32(body, (uint32_t)st.rescue.size());
    for (const auto& r : st.rescue) { putU32(body, r.state); putF32(body, r.timeLeft); }

    // World flags (per floor).
    putU32(body, (uint32_t)st.floors.size());
    for (const auto& f : st.floors) {
        putU32(body, f.floorIndex);
        putU32(body, f.doorsOpened);
        putU32(body, f.keypadsSolved);
        putU32(body, f.enemiesCleared);
        putU32(body, f.enemiesTotal);
    }

    // ---- Assemble header + body + footer.
    std::vector<uint8_t> file;
    file.reserve(body.size() + 16);
    putU32(file, kSaveMagic);
    putU32(file, kSaveVersion);
    putU32(file, (uint32_t)body.size());              // declared body length
    file.insert(file.end(), body.begin(), body.end());
    putU32(file, checksum(body.data(), body.size())); // footer checksum over the body

    // ---- Write atomically-ish: open binary, dump, close.
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { x3::logError("[save] cannot open for write: " + path); return false; }
    f.write(reinterpret_cast<const char*>(file.data()), (std::streamsize)file.size());
    if (!f) { x3::logError("[save] write failed: " + path); return false; }
    f.close();
    x3::logInfo("[save] checkpoint written (" + std::to_string(file.size()) +
                " bytes, v" + std::to_string(kSaveVersion) + ") -> " + path);
    return true;
}

// ===========================================================================
// loadCheckpoint
// ===========================================================================
bool loadCheckpoint(const std::string& path, SaveState& out) {
    // ---- Read the whole file into memory (checkpoints are tiny).
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { x3::logWarn("[save] no checkpoint to load: " + path); return false; }
    std::streamoff len = f.tellg();
    if (len < 16) { x3::logWarn("[save] file too short / not a checkpoint: " + path); return false; }
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf((size_t)len);
    f.read(reinterpret_cast<char*>(buf.data()), len);
    if (!f) { x3::logWarn("[save] read failed: " + path); return false; }

    Reader rd{ buf.data(), buf.size(), 0, true };

    // ---- Header validation.
    uint32_t magic = rd.getU32();
    if (!rd.ok || magic != kSaveMagic) {
        x3::logError("[save] bad magic (not an X3 checkpoint) — refusing: " + path);
        return false;
    }
    uint32_t version = rd.getU32();
    if (!rd.ok || version != kSaveVersion) {
        x3::logError("[save] version mismatch (file v" + std::to_string(version) +
                     ", engine v" + std::to_string(kSaveVersion) + ") — refusing: " + path);
        return false;
    }
    uint32_t bodyLen = rd.getU32();
    // The body must fit between the header (12 bytes) and the 4-byte checksum footer.
    if (!rd.ok || (size_t)bodyLen + 16 > buf.size()) {
        x3::logError("[save] truncated / bad body length — refusing: " + path);
        return false;
    }
    // Verify the checksum over the declared body BEFORE trusting any field.
    const uint8_t* bodyPtr = buf.data() + 12;
    uint32_t storedSum = (uint32_t)buf[12 + bodyLen]
                       | ((uint32_t)buf[12 + bodyLen + 1] << 8)
                       | ((uint32_t)buf[12 + bodyLen + 2] << 16)
                       | ((uint32_t)buf[12 + bodyLen + 3] << 24);
    if (checksum(bodyPtr, bodyLen) != storedSum) {
        x3::logError("[save] checksum mismatch (corrupted) — refusing: " + path);
        return false;
    }

    // ---- Body parse (cursor confined to the body bytes; bounds-checked).
    Reader b{ bodyPtr, bodyLen, 0, true };
    SaveState s;   // parse into a local; only commit to `out` on full success

    s.playerX = b.getF32(); s.playerY = b.getF32(); s.playerZ = b.getF32();
    s.playerYaw = b.getF32(); s.playerPitch = b.getF32();
    s.playerHp = b.getI32(); s.playerMaxHp = b.getI32();

    s.equippedWeapon = b.getU32();
    uint32_t wcount = b.getU32();
    if (!b.ok || wcount > kMaxWeapons) { x3::logError("[save] bad weapon count — refusing"); return false; }
    s.weapons.resize(wcount);
    for (uint32_t i = 0; i < wcount; ++i) { s.weapons[i].ammoInMag = b.getU32(); s.weapons[i].reserve = b.getU32(); }

    s.levelIndex     = b.getU32();
    s.currentFloor   = b.getU32();
    s.objectiveIndex = b.getU32();
    s.levelComplete  = b.getU32();

    s.rescueHubReached = b.getU32();
    uint32_t rcount = b.getU32();
    if (!b.ok || rcount > kMaxRescue) { x3::logError("[save] bad rescue count — refusing"); return false; }
    s.rescue.resize(rcount);
    for (uint32_t i = 0; i < rcount; ++i) { s.rescue[i].state = b.getU32(); s.rescue[i].timeLeft = b.getF32(); }

    uint32_t fcount = b.getU32();
    if (!b.ok || fcount > kMaxFloors) { x3::logError("[save] bad floor count — refusing"); return false; }
    s.floors.resize(fcount);
    for (uint32_t i = 0; i < fcount; ++i) {
        s.floors[i].floorIndex     = b.getU32();
        s.floors[i].doorsOpened    = b.getU32();
        s.floors[i].keypadsSolved  = b.getU32();
        s.floors[i].enemiesCleared = b.getU32();
        s.floors[i].enemiesTotal   = b.getU32();
    }

    // Any over-read latched b.ok=false; reject rather than commit a partial state.
    if (!b.ok) { x3::logError("[save] body underflow (truncated fields) — refusing: " + path); return false; }

    out = std::move(s);
    x3::logInfo("[save] checkpoint loaded (v" + std::to_string(version) + ") <- " + path);
    return true;
}

// ===========================================================================
// Headless self-test (--test-saveload). No window / Vulkan / physics.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void chk(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[saveload-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[saveload-test] FAIL ") + name); }
}

bool feq(float a, float b, float eps = 1e-5f) { float d = a - b; return (d < 0 ? -d : d) <= eps; }

// Build a representative, non-default checkpoint (every field set to something the
// defaults are NOT, so a field that fails to round-trip is caught).
SaveState makeSample() {
    SaveState s;
    s.playerX = 12.5f; s.playerY = 18.25f; s.playerZ = -3.75f;
    s.playerYaw = 1.2345f; s.playerPitch = -0.6789f;
    s.playerHp = 73; s.playerMaxHp = 100;

    s.equippedWeapon = 2;
    s.weapons = { {12, 36}, {8, 24}, {0, 60}, {1, 10} };  // pistol/smg/shotgun/plasma-ish

    s.levelIndex = 0;
    s.currentFloor = 5;          // F5 Synth bay
    s.objectiveIndex = 3;
    s.levelComplete = 0;

    s.rescueHubReached = 1;
    s.rescue = { {1, 0.0f}, {0, 241.5f}, {2, 0.0f} };     // companion / captive / expired

    s.floors = {
        {1, 0, 0, 3, 3},   // F2 hub floor, all cleared
        {3, 1, 1, 2, 4},   // F3 partly cleared, door+keypad done
        {5, 1, 1, 0, 6},   // F5 just arrived
    };
    return s;
}

bool equalState(const SaveState& a, const SaveState& b) {
    if (!feq(a.playerX, b.playerX) || !feq(a.playerY, b.playerY) || !feq(a.playerZ, b.playerZ)) return false;
    if (!feq(a.playerYaw, b.playerYaw) || !feq(a.playerPitch, b.playerPitch)) return false;
    if (a.playerHp != b.playerHp || a.playerMaxHp != b.playerMaxHp) return false;
    if (a.equippedWeapon != b.equippedWeapon) return false;
    if (a.weapons.size() != b.weapons.size()) return false;
    for (size_t i = 0; i < a.weapons.size(); ++i)
        if (a.weapons[i].ammoInMag != b.weapons[i].ammoInMag || a.weapons[i].reserve != b.weapons[i].reserve) return false;
    if (a.levelIndex != b.levelIndex || a.currentFloor != b.currentFloor) return false;
    if (a.objectiveIndex != b.objectiveIndex || a.levelComplete != b.levelComplete) return false;
    if (a.rescueHubReached != b.rescueHubReached) return false;
    if (a.rescue.size() != b.rescue.size()) return false;
    for (size_t i = 0; i < a.rescue.size(); ++i)
        if (a.rescue[i].state != b.rescue[i].state || !feq(a.rescue[i].timeLeft, b.rescue[i].timeLeft)) return false;
    if (a.floors.size() != b.floors.size()) return false;
    for (size_t i = 0; i < a.floors.size(); ++i) {
        const auto& x = a.floors[i]; const auto& y = b.floors[i];
        if (x.floorIndex != y.floorIndex || x.doorsOpened != y.doorsOpened ||
            x.keypadsSolved != y.keypadsSolved || x.enemiesCleared != y.enemiesCleared ||
            x.enemiesTotal != y.enemiesTotal) return false;
    }
    return true;
}

// A unique temp path in the system temp dir.
std::string tempPath(const char* tag) {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / (std::string("x3_saveload_") + tag + ".x3save");
    return p.string();
}

} // namespace

bool runSaveLoadSelfTest() {
    g_pass = g_fail = 0;
    namespace fs = std::filesystem;

    const std::string path = tempPath("rt");
    std::error_code ec;
    fs::remove(path, ec);   // start clean

    // ---- S1: a representative state round-trips field-by-field ----
    SaveState src = makeSample();
    bool wrote = saveCheckpoint(path, src);
    chk(wrote, "S1a save returns true");
    chk(fs::exists(path), "S1b file created");

    SaveState loaded;
    bool read = loadCheckpoint(path, loaded);
    chk(read, "S2a load returns true");
    chk(read && equalState(src, loaded), "S2b loaded == saved (all fields)");

    // ---- S3: mutate a LIVE state, then a load overwrites the mutation ----
    SaveState live = src;
    live.playerHp = 1; live.playerX = -999.0f; live.currentFloor = 0;
    live.equippedWeapon = 0; live.weapons.clear(); live.rescue.clear(); live.floors.clear();
    chk(!equalState(src, live), "S3a mutated live state differs from saved");
    bool reread = loadCheckpoint(path, live);
    chk(reread && equalState(src, live), "S3b load restores the saved state over the mutation");

    // ---- S4: a missing file fails gracefully (no crash, returns false, out untouched) ----
    {
        SaveState before = makeSample();
        SaveState probe  = before;
        bool ok = loadCheckpoint(tempPath("does_not_exist_xyz"), probe);
        chk(!ok, "S4a missing file -> false");
        chk(equalState(before, probe), "S4b out left unchanged on missing file");
    }

    // ---- S5: corrupted body (flip a byte) is rejected by the checksum ----
    {
        std::vector<uint8_t> bytes;
        { std::ifstream in(path, std::ios::binary | std::ios::ate);
          auto n = in.tellg(); in.seekg(0); bytes.resize((size_t)n);
          in.read(reinterpret_cast<char*>(bytes.data()), n); }
        chk(bytes.size() > 20, "S5a have file bytes");
        std::string corruptPath = tempPath("corrupt");
        bytes[16] ^= 0xFF;   // flip a byte inside the body (past the 12-byte header)
        { std::ofstream out(corruptPath, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()); }
        SaveState probe = makeSample();
        SaveState before = probe;
        bool ok = loadCheckpoint(corruptPath, probe);
        chk(!ok, "S5b corrupted file -> false (checksum)");
        chk(equalState(before, probe), "S5c out unchanged on corruption");
        fs::remove(corruptPath, ec);
    }

    // ---- S6: bad magic is rejected ----
    {
        std::string badMagicPath = tempPath("badmagic");
        std::vector<uint8_t> bytes;
        { std::ifstream in(path, std::ios::binary | std::ios::ate);
          auto n = in.tellg(); in.seekg(0); bytes.resize((size_t)n);
          in.read(reinterpret_cast<char*>(bytes.data()), n); }
        bytes[0] = 'Z';   // corrupt the magic
        { std::ofstream out(badMagicPath, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()); }
        SaveState probe; bool ok = loadCheckpoint(badMagicPath, probe);
        chk(!ok, "S6 bad magic -> false");
        fs::remove(badMagicPath, ec);
    }

    // ---- S7: wrong/old version is rejected ----
    {
        std::string oldVerPath = tempPath("oldver");
        std::vector<uint8_t> bytes;
        { std::ifstream in(path, std::ios::binary | std::ios::ate);
          auto n = in.tellg(); in.seekg(0); bytes.resize((size_t)n);
          in.read(reinterpret_cast<char*>(bytes.data()), n); }
        // version field is bytes [4..7] LE; set it to an unsupported value.
        uint32_t bogus = kSaveVersion + 99u;
        bytes[4] = (uint8_t)(bogus & 0xFF); bytes[5] = (uint8_t)((bogus >> 8) & 0xFF);
        bytes[6] = (uint8_t)((bogus >> 16) & 0xFF); bytes[7] = (uint8_t)((bogus >> 24) & 0xFF);
        { std::ofstream out(oldVerPath, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()); }
        SaveState probe; bool ok = loadCheckpoint(oldVerPath, probe);
        chk(!ok, "S7 version mismatch -> false");
        fs::remove(oldVerPath, ec);
    }

    // ---- S8: a truncated file (cut mid-body) is rejected ----
    {
        std::string truncPath = tempPath("trunc");
        std::vector<uint8_t> bytes;
        { std::ifstream in(path, std::ios::binary | std::ios::ate);
          auto n = in.tellg(); in.seekg(0); bytes.resize((size_t)n);
          in.read(reinterpret_cast<char*>(bytes.data()), n); }
        bytes.resize(bytes.size() / 2);   // chop the file in half
        { std::ofstream out(truncPath, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()); }
        SaveState probe; bool ok = loadCheckpoint(truncPath, probe);
        chk(!ok, "S8 truncated file -> false");
        fs::remove(truncPath, ec);
    }

    // ---- Cleanup the round-trip temp file ----
    fs::remove(path, ec);
    chk(!fs::exists(path), "S9 temp file cleaned up");

    x3::logInfo(std::string("saveload: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::save
