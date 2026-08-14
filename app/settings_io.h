#pragma once
// ============================================================================
// Settings persistence (#28 deep split). The window-size + audio key=value cfg
// helpers were file-scope in main.cpp; readWindowSize is used by main()'s
// prelude (windowed default size) AND the rest by the default host
// (app/app_run.cpp). Moved VERBATIM here so both TUs share one definition.
// ============================================================================

#include <string>
#include <fstream>
#include <cstdlib>
#include <cstdint>
#include <cctype>

namespace x3 { namespace apphost {

inline std::string x3SettingsPath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base && *base ? base : ".") + "\\x3native_settings.cfg";
}
inline bool readWindowSize(uint32_t& w, uint32_t& h) {
    std::ifstream f(x3SettingsPath());
    if (!f) return false;
    bool found = false; std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const uint32_t v = (uint32_t)std::strtoul(line.c_str() + eq + 1, nullptr, 10);
        if (k == "width"  && v >= 320) { w = v; found = true; }
        else if (k == "height" && v >= 240) { h = v; found = true; }
    }
    return found;
}
// Audio settings live in the same key=value cfg. Each is optional; defaults are
// kept when a key is missing/garbled. musicVol/sfxVol are stored as plain floats.
inline void readAudioSettings(bool& musicOn, float& musicVol, float& sfxVol) {
    std::ifstream f(x3SettingsPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const char* vs = line.c_str() + eq + 1;
        if      (k == "musicOn")  musicOn  = (std::strtol(vs, nullptr, 10) != 0);
        else if (k == "musicVol") musicVol = (float)std::strtod(vs, nullptr);
        else if (k == "sfxVol")   sfxVol   = (float)std::strtod(vs, nullptr);
    }
    auto clamp01 = [](float& v) { if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; };
    clamp01(musicVol); clamp01(sfxVol);
}
// Flight mode (0=Arcade,1=Assist,2=Loose) persists in the same cfg. Optional;
// default 0 (Arcade) when the key is missing/garbled. Read by the standalone
// --world space host so a Settings-menu / console selection carries across
// launches (that host predates the game console + settings UI, so the cfg file
// is the only bridge to it).
inline int readFlightMode() {
    std::ifstream f(x3SettingsPath());
    if (!f) return 0;
    int mode = 0; std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == "flightMode") {
            const long v = std::strtol(line.c_str() + eq + 1, nullptr, 10);
            if (v >= 0 && v <= 2) mode = (int)v;
        }
    }
    return mode;
}
// Skip-intro (Settings > Advanced, dev convenience) persists in the same cfg.
// Optional; default false when the key is missing/garbled. Equivalent to the
// --skipintro CLI flag; the host ORs the two so either path wins.
inline bool readSkipIntro() {
    std::ifstream f(x3SettingsPath());
    if (!f) return false;
    bool skip = false; std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == "skipIntro")
            skip = (std::strtol(line.c_str() + eq + 1, nullptr, 10) != 0);
    }
    return skip;
}
// ---- LIVE TUNING LEVERS (dogfight feel) ------------------------------------
// Feel is subjective, so the space-combat numbers are LEVERS, not verdicts: any
// of them can be dialled without a rebuild. Two sources, checked in order:
//   1. the environment  — X3_<KEY with '.'->'_', upper-cased>, e.g.
//                         X3_SPACE_STRAFEACCEL=140   (a one-shot A/B)
//   2. the settings cfg — a `space.strafeAccel=140` line in
//                         %LOCALAPPDATA%\x3native_settings.cfg (persistent)
// Anything missing/garbled keeps `def`. The cfg is re-read per call; this is
// only ever called a handful of times at beat entry, never per frame.
inline float readTuningFloat(const char* key, float def) {
    if (!key || !*key) return def;
    // 1) environment override
    {
        std::string env = "X3_";
        for (const char* p = key; *p; ++p)
            env += (*p == '.') ? '_' : (char)std::toupper((unsigned char)*p);
        if (const char* v = std::getenv(env.c_str()); v && *v) {
            char* end = nullptr;
            const double d = std::strtod(v, &end);
            if (end != v) return (float)d;
        }
    }
    // 2) settings cfg
    std::ifstream f(x3SettingsPath());
    if (!f) return def;
    float out = def; std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.compare(0, eq, key) != 0) continue;
        const char* vs = line.c_str() + eq + 1;
        char* end = nullptr;
        const double d = std::strtod(vs, &end);
        if (end != vs) out = (float)d;
    }
    return out;
}

// Write ALL persisted settings (window size + audio + flight mode + skip-intro)
// in one shot. skipIntro is a TRAILING DEFAULTED param so every pre-existing
// call site keeps its meaning (and keeps compiling) unchanged.
inline void writeSettings(uint32_t w, uint32_t h, bool musicOn, float musicVol, float sfxVol,
                          int flightMode = 0, bool skipIntro = false) {
    std::ofstream f(x3SettingsPath());
    if (f) f << "width=" << w << "\nheight=" << h << "\n"
             << "musicOn=" << (musicOn ? 1 : 0) << "\n"
             << "musicVol=" << musicVol << "\nsfxVol=" << sfxVol << "\n"
             << "flightMode=" << flightMode << "\n"
             << "skipIntro=" << (skipIntro ? 1 : 0) << "\n";
}

}} // namespace x3::apphost
