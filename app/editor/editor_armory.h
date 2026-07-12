#pragma once
// ---------------------------------------------------------------------------
// THE ARMORY BROWSER — the editor's Models panel, pointed at the WHOLE library.
//
// The Models panel shipped with a hardcoded list of NINE props (Barrel, Crate, Pallet,
// Fusebox...). Meanwhile the asset library on this box holds ELEVEN THOUSAND converted
// GLBs — Sci-Fi Kit, Cyberpunk City, Command Center, Abandoned Factory, Modular Sewers,
// Space Station interiors — indexed by the Armory tool. The editor could not see any of
// it, which meant "place a prop" meant "place one of nine props".
//
// This reads the Armory's own index (assets/_glb/_host/galleries.json — the SAME file
// the web Armory serves, so there is exactly one source of truth and it cannot drift)
// and hands the editor a searchable catalogue of every mesh in the library.
//
// PURE LOGIC. No ImGui, no Vulkan: parse + URL-decode + filter, all headless-testable.
// The host owns the panel and the placement.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <string>
#include <vector>

namespace x3::editor {

// One placeable mesh from the library.
struct ArmoryItem {
    std::string name;        // "SM_ArchBridge_A"
    std::string pack;        // "Modular Castle and Dungeon"
    std::string relPath;     // DECODED path, relative to the armory root, e.g.
                             // "tech/Command Center/Assets/.../SM_Console.glb"
};

struct ArmoryIndex {
    bool                    ok = false;
    std::string             root;      // the directory relPath is relative to
    std::vector<ArmoryItem> items;     // flattened: every mesh in every pack
    std::vector<std::string> packs;    // unique pack names, in index order
    std::string             error;     // why ok == false
};

// The armory root. Overridable with X3_ARMORY_ROOT so this is not a hardcoded D: path
// (the lesson of KNOWN_BUGS L2: never bake another machine's disk into the source).
std::string armoryRoot();

// Parse the Armory's galleries.json TEXT. Tolerant: a missing/!ok index is not an error
// the editor should die on — it just means the panel says "no library" and the curated
// Models list still works. Paths are URL-decoded here (the index stores them
// percent-encoded because it is served over HTTP; the filesystem wants them raw).
ArmoryIndex parseArmoryIndex(const std::string& json, const std::string& root);

// Load it from disk (armoryRoot()/_host/galleries.json). ok == false if absent.
ArmoryIndex loadArmoryIndex();

// Percent-decoding ("Modular%20Castle%28A%29" -> "Modular Castle(A)"). Exposed for the
// self-test, because getting this wrong silently produces paths that do not exist.
std::string urlDecode(const std::string& s);

// Case-insensitive substring filter over name + pack. Returns indices into idx.items.
// `limit` caps the result (the UI never needs 11,000 rows at once).
std::vector<uint32_t> filterArmory(const ArmoryIndex& idx, const std::string& query,
                                   const std::string& packFilter, size_t limit);

// Folded into --test-editor.
bool runArmorySelfTest();

} // namespace x3::editor
