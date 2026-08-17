#pragma once
// Portable game-asset root resolution (assets-LFS pass).
//
// The game models (converted_glb / rigged_glb) used to load from the hard-coded
// absolute path "G:/GameModels/...". To make the repo portable across machines
// the curated GLBs are now committed under <repo>/assets/ via Git LFS, and every
// loader resolves its root through assetRoot() instead of the G: literal.
//
// assetRoot() returns the FIRST existing directory of:
//   1. <exeDir>/../../../assets   (build/bin/<Config>/X3Engine.exe -> repo/assets)
//   2. <exeDir>/assets            (assets copied next to the exe, if ever done)
//   3. ./assets                   (run from the repo root)
//   4. assets                     (relative, same as 3 but no leading ./)
//   5. G:/GameModels              (TRANSITION FALLBACK: machines that still have
//                                  the external library / before the LFS pull)
// The result is cached after the first call. Each loader appends its existing
// relative subpath ("/converted_glb", "/rigged_glb", ...) to this root.
//
// Clean-room: built from the C++ standard library + Win32 GetModuleFileName only.

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace x3::game {

namespace detail {

// Directory containing the running executable (or "." if it can't be resolved).
inline std::filesystem::path exeDirPath() {
#ifdef _WIN32
    char buf[1024];
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return std::filesystem::path(".");
    return std::filesystem::path(std::string(buf, n)).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

// Why the resolved root won — filled by resolveAssetRoot, reported by
// assetRootSource() so "which tree am I reading?" is answerable from a log
// instead of from a debugger. The header comment has asked for this since the
// D:\Assets incident; without it a mis-resolved root is completely silent.
inline std::string& assetRootSourceSlot() {
    static std::string s = "unresolved";
    return s;
}

// Resolve the asset root once. See header comment for the candidate order.
inline std::string resolveAssetRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path exe = exeDirPath();

    // ---- 0. THE EXPLICIT OVERRIDE, ahead of every heuristic. -------------
    // X3_ASSET_ROOT=<dir> points the game at another tree DELIBERATELY and
    // LOUDLY. This exists so that "run against the asset store / another
    // checkout / a deployed tree" never has to be done by widening the
    // candidate list below — which is how D:\Assets silently captured every
    // asset once already. It is still marker-checked: an override that is not
    // an asset root is refused and reported rather than believed, because a
    // typo'd env var that half-works is worse than one that fails.
    if (const char* env = std::getenv("X3_ASSET_ROOT")) {
        if (env[0]) {
            const fs::path p(env);
            const bool dir = fs::is_directory(p, ec);
            const bool marked = dir && (fs::is_directory(p / "surface_library", ec)
                                     || fs::is_directory(p / "converted_glb", ec));
            if (marked) {
                assetRootSourceSlot() = std::string("X3_ASSET_ROOT=") + env;
                fs::path norm = fs::weakly_canonical(p, ec);
                return (ec ? p : norm).string();
            }
            assetRootSourceSlot() = std::string("X3_ASSET_ROOT=") + env
                                  + (dir ? " REFUSED (no surface_library/converted_glb inside)"
                                         : " REFUSED (not a directory)")
                                  + " — fell through to the normal search";
        }
    }

    const fs::path candidates[] = {
        exe / ".." / ".." / "assets",         // build-ninja/bin    -> repo/assets
        exe / ".." / ".." / ".." / "assets",  // build/bin/<Config> -> repo/assets
        exe / "assets",                        // assets next to the exe
        fs::path(".") / "assets",              // run from repo root
        fs::path("assets"),                    // relative fallback
    };
    // A candidate must LOOK LIKE the asset root, not merely exist under that
    // name. Windows path comparison is CASE-INSENSITIVE, and the old first
    // candidate assumed a three-deep build/bin/<Config> layout — so under the
    // two-deep ninja layout it overshot to "D:\assets", which silently matched
    // an unrelated "D:\Assets" folder on this machine. is_directory() said yes,
    // resolution stopped there, and EVERY committed asset in the repo became
    // invisible: terrain splats fell back to procedural noise, the tunnel's
    // shotcrete lining reported "incomplete", and the art was written off as
    // living only on the authoring box while it sat in assets/ the whole time.
    // Requiring a known marker subdirectory makes a wrong-but-existing path
    // fail over to the next candidate instead of winning.
    //
    // ⚠ THE MARKER IS LOAD-BEARING, AND IT IS THE ONLY THING HOLDING THIS UP.
    // A DEPLOYED build (games ship to D:\GameDev\X3Play, exe at the root of it)
    // sits at a DIFFERENT depth from a build tree, and it re-enters the exact
    // trap above: for D:\GameDev\X3Play\X3Play.exe, candidate 2 resolves to
    // "D:\assets" — which case-insensitively matches the unrelated D:\Assets
    // Unity-pack library that exists on the dev boxes. Today that candidate
    // loses ONLY because D:\Assets happens to contain neither surface_library
    // nor converted_glb, and candidate 3 (exe/assets) then wins correctly.
    // So this is one `mkdir D:\Assets\converted_glb` away from silently
    // repointing every deployed build at the wrong tree again — and the
    // failure is SILENT: assets simply go missing, nothing logs an error.
    // If you add a marker name here, do not pick one an asset library would
    // plausibly also use. Better: log the resolved root (see assetRoot()'s
    // callers) so "which tree am I reading?" is answerable from a play log,
    // and prefer exe/assets ahead of any ../.. candidate for shipped builds.
    auto looksLikeAssetRoot = [&](const fs::path& p) {
        return fs::is_directory(p / "surface_library", ec)
            || fs::is_directory(p / "converted_glb", ec);
    };
    static const char* kWhy[] = {
        "exe/../../assets (ninja build tree)",
        "exe/../../../assets (multi-config build tree)",
        "exe/assets (shipped layout)",
        "./assets (run from the repo root)",
        "assets (relative)",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const fs::path& c = candidates[i];
        if (fs::is_directory(c, ec) && looksLikeAssetRoot(c)) {
            // Normalize (collapse the ../ segments) so logs read cleanly.
            fs::path norm = fs::weakly_canonical(c, ec);
            if (assetRootSourceSlot() == "unresolved") assetRootSourceSlot() = kWhy[i];
            else assetRootSourceSlot() += std::string("; then ") + kWhy[i];
            return (ec ? c : norm).string();
        }
    }
    // Transition fallback: the external library on machines that still have G:.
    // Reaching here means NOTHING looked like an asset root, which is nearly
    // always a wrong working directory rather than a machine that still has G:.
    assetRootSourceSlot() += "; NOTHING MATCHED — G:/GameModels transition fallback";
    return std::string("G:/GameModels");
}

} // namespace detail

// Portable game-asset root. Cached after the first call (the executable does not
// move at runtime). Append the loader's existing relative subdir, e.g.
//   assetRoot() + "/converted_glb"   /   assetRoot() + "/rigged_glb"
inline const std::string& assetRoot() {
    static const std::string root = detail::resolveAssetRoot();
    return root;
}

// WHICH candidate won, in words. Log this at boot: a mis-resolved asset root
// is otherwise completely silent — art simply goes missing and gets written
// off as "not on this machine" while it sits in the tree the whole time. That
// is not hypothetical; it is what happened, and it cost a day.
// Only meaningful after the first assetRoot() call.
inline const std::string& assetRootSource() {
    (void)assetRoot();                       // force resolution
    return detail::assetRootSourceSlot();
}

// Convenience: the two roots every loader uses.
inline std::string convertedGlbRoot() { return assetRoot() + "/converted_glb"; }
inline std::string riggedGlbRoot()    { return assetRoot() + "/rigged_glb"; }

} // namespace x3::game
