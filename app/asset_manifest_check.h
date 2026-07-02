#pragma once
// Boot-time asset-manifest check (Phase A of docs/ASSET_DISTRIBUTION.md).
//
// Big binary assets are distributed via the fleet asset store
// (\\p13700\G\X3AssetStore, content-addressed by SHA-256) and indexed by the
// committed assets/manifest.json — NOT by git/LFS. A fresh clone therefore may
// be missing GLBs that the manifest knows about. This check runs once at boot:
//
//   1. Parse assets/manifest.json (minimal hand scanner — no JSON lib, no
//      python dependency; the manifest is machine-written by
//      tools/asset_store.py with a fixed shape).
//   2. For every entry whose local file is missing, try to AUTO-FETCH the
//      object from the store (D:\Assets\X3AssetStore cache first, then the
//      G: share), verifying the byte size. Hash verification is left to
//      `python tools/asset_store.py verify` — boot stays fast.
//   3. Log ONE clear summary line. If anything is still missing (offline +
//      cold cache), the line says exactly what to run:
//          python tools/asset_store.py fetch --all
//
// No manifest file (e.g. running against G:/GameModels fallback) = silent
// no-op. This header never deletes or overwrites existing files.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "engine/core/x3_log.h"
#include "asset_root.h"

namespace x3::game {

namespace detail_manifest {

// Extract the string value following "key": " ... " starting at/after `from`.
// Returns empty on miss. Good enough for the machine-written manifest (values
// are paths/hashes/notes; tools/asset_store.py json-escapes backslashes).
inline std::string jsonStrAfter(const std::string& s, const std::string& key, size_t from, size_t end) {
    const std::string pat = "\"" + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos || k >= end) return {};
    size_t q1 = s.find('"', s.find(':', k + pat.size()) + 1);
    if (q1 == std::string::npos || q1 >= end) return {};
    std::string out;
    for (size_t i = q1 + 1; i < end && i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }  // \\ and \" unescape
        if (c == '"') return out;
        out += c;
    }
    return {};
}

inline uint64_t jsonNumAfter(const std::string& s, const std::string& key, size_t from, size_t end) {
    const std::string pat = "\"" + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos || k >= end) return 0;
    size_t c = s.find(':', k + pat.size());
    if (c == std::string::npos) return 0;
    uint64_t v = 0; bool any = false;
    for (size_t i = c + 1; i < end && i < s.size(); ++i) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { if (any) break; continue; }
        if (ch < '0' || ch > '9') break;
        v = v * 10 + uint64_t(ch - '0'); any = true;
    }
    return v;
}

} // namespace detail_manifest

// Run the check. Returns the number of manifest assets STILL missing after the
// auto-fetch attempt (0 = all good). Call once, early in main().
inline int checkAssetManifest() {
    namespace fs = std::filesystem;
    using namespace detail_manifest;
    std::error_code ec;

    const fs::path assets = fs::path(assetRoot());
    const fs::path manifestPath = assets / "manifest.json";
    if (!fs::is_regular_file(manifestPath, ec)) return 0;  // no manifest = nothing to check
    const fs::path repoRoot = assets.parent_path();        // entries are repo-relative ("assets/...")

    std::string text;
    {
        std::ifstream in(manifestPath, std::ios::binary);
        if (!in) return 0;
        std::ostringstream ss; ss << in.rdbuf(); text = ss.str();
    }

    // Store tiers (same defaults as tools/asset_store.py; manifest overrides).
    // PRIMARY is now the local D: store (authoritative, read-first, publish
    // target). The off-box \\p13700\G\X3AssetStore mirror is the backup.
    size_t storeBlk = text.find("\"store\"");
    std::string primary = jsonStrAfter(text, "primary", storeBlk == std::string::npos ? 0 : storeBlk, text.size());
    std::string cache   = jsonStrAfter(text, "cache",   storeBlk == std::string::npos ? 0 : storeBlk, text.size());
    if (primary.empty()) primary = "D:\\Assets\\X3AssetStore";
    if (cache.empty())   cache   = "D:\\Assets\\X3AssetStore";

    int missing = 0, fetched = 0, total = 0;
    std::string firstMissing;

    // Walk the "assets" array: each element is a flat object with repo_path /
    // sha256 / size (machine-written, no nested objects inside entries).
    size_t arr = text.find("\"assets\"");
    size_t scan = (arr == std::string::npos) ? std::string::npos : text.find('[', arr);
    while (scan != std::string::npos) {
        size_t objStart = text.find('{', scan);
        if (objStart == std::string::npos) break;
        size_t objEnd = text.find('}', objStart);
        if (objEnd == std::string::npos) break;

        const std::string repoPath = jsonStrAfter(text, "repo_path", objStart, objEnd);
        const std::string sha      = jsonStrAfter(text, "sha256",    objStart, objEnd);
        const uint64_t    size     = jsonNumAfter(text, "size",      objStart, objEnd);
        scan = objEnd + 1;
        if (repoPath.empty() || sha.size() < 3) continue;
        ++total;

        const fs::path local = repoRoot / fs::path(repoPath);
        if (fs::is_regular_file(local, ec) && fs::file_size(local, ec) == size) continue;
        if (fs::exists(local, ec)) {  // present but wrong size — never overwrite; report only
            ++missing;
            if (firstMissing.empty()) firstMissing = repoPath + " (size mismatch)";
            continue;
        }

        // Auto-fetch: cache tier first, then the share. Size-checked copy.
        bool got = false;
        for (const std::string& root : {cache, primary}) {
            const fs::path obj = fs::path(root) / "objects" / sha.substr(0, 2) / sha;
            if (!fs::is_regular_file(obj, ec) || fs::file_size(obj, ec) != size) continue;
            fs::create_directories(local.parent_path(), ec);
            const fs::path tmp = local.parent_path() / (local.filename().string() + ".boot-fetch.tmp");
            if (fs::copy_file(obj, tmp, fs::copy_options::overwrite_existing, ec) &&
                fs::file_size(tmp, ec) == size) {
                fs::rename(tmp, local, ec);
                if (!ec) { got = true; ++fetched; break; }
            }
            fs::remove(tmp, ec);  // our own temp only
        }
        if (!got) {
            ++missing;
            if (firstMissing.empty()) firstMissing = repoPath;
        }
    }

    if (missing > 0) {
        logWarn("[assets] " + std::to_string(missing) + "/" + std::to_string(total) +
                " manifest asset(s) missing locally (e.g. " + firstMissing +
                ") and the asset store was unreachable — run: python tools/asset_store.py fetch --all");
    } else if (fetched > 0) {
        logInfo("[assets] auto-fetched " + std::to_string(fetched) +
                " missing manifest asset(s) from the fleet asset store.");
    }
    return missing;
}

} // namespace x3::game
