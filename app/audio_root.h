#pragma once
// Portable audio-asset resolution (sibling of asset_root.h).
//
// The slice's SFX/music are purchased Unity-pack WAVs that are deliberately NOT
// committed to the repo (unlike the curated GLBs). Each machine keeps them in a
// different place:
//   - laptop  : D:/GameDevAssets/<pack>/...                 (flattened-by-pack mirror)
//   - 13700K  : G:/Unity_Projects/<project>/Assets/<pack>/...
//   - 14900K  : (same G: layout)
//
// resolveAudio("<pack>/<...>.wav") returns the first candidate root that actually
// contains the file. If none do, it returns the first candidate joined with the
// relative path, so the audio system's load() logs a sensible path and the event
// just plays silent (load() is graceful — a miss never crashes).
//
// Clean-room: built from the C++ standard library only. No foreign source consulted.

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace x3::game {

namespace detail {

// Candidate roots, highest priority first. A "pack-relative" path is appended to
// each (e.g. "Free Pack/Explosion 1.wav"). The G: roots cover both Unity projects
// the packs were imported into; the laptop's D: mirror flattens them by pack.
inline const std::vector<std::string>& audioRoots() {
    static const std::vector<std::string> roots = {
        "D:/GameDevAssets",
        "G:/Unity_Projects/EscapeFromLabZero/Assets",
        "G:/Unity_Projects/EscapeLab48/Escape Lab 48/Assets",
    };
    return roots;
}

} // namespace detail

// Resolve a pack-relative audio path against the first root that contains it.
inline std::string resolveAudio(const std::string& packRel) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const std::string& r : detail::audioRoots()) {
        fs::path p = fs::path(r) / packRel;
        if (fs::exists(p, ec)) return p.string();
    }
    return (fs::path(detail::audioRoots().front()) / packRel).string();
}

} // namespace x3::game
