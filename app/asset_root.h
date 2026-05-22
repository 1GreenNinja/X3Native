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

// Resolve the asset root once. See header comment for the candidate order.
inline std::string resolveAssetRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path exe = exeDirPath();

    const fs::path candidates[] = {
        exe / ".." / ".." / ".." / "assets",  // build/bin/<Config> -> repo/assets
        exe / "assets",                        // assets next to the exe
        fs::path(".") / "assets",              // run from repo root
        fs::path("assets"),                    // relative fallback
    };
    for (const fs::path& c : candidates) {
        if (fs::is_directory(c, ec)) {
            // Normalize (collapse the ../ segments) so logs read cleanly.
            fs::path norm = fs::weakly_canonical(c, ec);
            return (ec ? c : norm).string();
        }
    }
    // Transition fallback: the external library on machines that still have G:.
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

// Convenience: the two roots every loader uses.
inline std::string convertedGlbRoot() { return assetRoot() + "/converted_glb"; }
inline std::string riggedGlbRoot()    { return assetRoot() + "/rigged_glb"; }

} // namespace x3::game
