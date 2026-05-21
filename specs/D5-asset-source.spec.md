# Spec: Asset Source / Pak VFS  (D5)

> Written by the SPEC TEAM. Implemented by the CLEAN-ROOM TEAM (13700K) from THIS FILE + public refs ONLY.
> ❌ No GPL source, no transcribed function bodies, no RBDOOM identifiers/paths below this line.
> This subsystem is standard infrastructure — spec'd from public knowledge, NO RBDOOM reading required. Safe for the 13700K to build with zero 14900K dependency.

- **Ledger ID:** D5
- **Implements interface:** `IAssetSource` (`engine/asset/IAssetSource.h`)
- **Status:** SPEC (ready to implement)
- **Clean-room target machine:** 13700K

## 1. Purpose
A virtual filesystem that mounts one or more `.x3pak` files (zip archives) plus optional loose-file dev directories, and resolves asset reads by a virtual path (e.g., `"materials/floor_a.ktx2"`). Higher-priority mounts override lower ones, so `x3.x3pak` can override `base.x3pak`, and a loose dev folder can override both during development.

## 2. Interface contract
```cpp
// engine/asset/IAssetSource.h — clean, authored fresh
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>

namespace x3::asset {

struct Blob {
    std::vector<uint8_t> bytes;
    bool ok = false;
};

class IAssetSource {
public:
    virtual ~IAssetSource() = default;

    // Mount a .x3pak or a loose directory. Higher priority wins on path collision.
    virtual bool mountPak(std::string_view pakPath, int priority) = 0;
    virtual bool mountDir(std::string_view dirPath, int priority) = 0;  // dev override

    // Read an asset by virtual path. Resolves through mounts by priority.
    virtual Blob  read(std::string_view virtualPath) = 0;
    virtual bool  exists(std::string_view virtualPath) const = 0;

    // Enumerate virtual paths under a prefix (for tools / hot-reload).
    virtual std::vector<std::string> list(std::string_view prefix) const = 0;

    // Dev: re-scan mounts (after a pak rebuild or loose-file change).
    virtual void refresh() = 0;
};

IAssetSource* createAssetSource();
} // namespace x3::asset
```

## 3. Behavior
- Inputs: pak file paths + loose dir paths, each with an integer priority.
- Outputs: byte blobs for virtual paths; existence checks; path listings.
- Resolution: on `read`, search mounts from highest priority to lowest; first hit wins.
- Virtual path normalization: forward slashes, case-insensitive on Windows, no leading slash, no `..` traversal.
- Lifecycle: mount at startup; `refresh` on demand in dev. Thread-safety: reads may be called from loader threads — `read`/`exists`/`list` must be safe for concurrent readers (mounts happen before threads spin up, or are externally synchronized).

## 4. Edge cases & error handling
- Missing pak / dir on mount → return false, log, continue (don't crash).
- Path not found in any mount → `Blob{ok=false}`, empty bytes.
- Corrupt/truncated zip entry → `ok=false`, log the entry + pak.
- Two mounts same priority + same path → defined tiebreak: last-mounted wins; log a warning.
- Zip-slip / `..` in a pak entry name → reject the entry, log (security).
- Empty file → `ok=true`, zero-length bytes (valid).

## 5. Performance targets
- `read` of a 1MB asset from a memory-mapped pak ≤ 1 ms (excludes decompression of large assets).
- Build a path→entry index at mount time (hash map); `exists`/`read` are O(1) average, not a linear scan.
- No full-archive decompression on mount — only the central directory.
- Memory-map pak files where possible; avoid loading whole paks into RAM.

## 6. Acceptance tests
1. **T1 — Single pak read:** mount `test.x3pak` containing `hello.txt`="hi"; `read("hello.txt").bytes` == "hi", `ok==true`.
2. **T2 — Priority override:** mount `base.x3pak` (has `a.txt`="base") at prio 0 and `game.x3pak` (has `a.txt`="game") at prio 10; `read("a.txt")` == "game".
3. **T3 — Loose-dir dev override:** mount a loose dir with `a.txt`="loose" at prio 100; `read("a.txt")` == "loose"; after deleting it + `refresh()`, falls back to "game".
4. **T4 — Missing path:** `read("nope.txt").ok == false`; `exists("nope.txt") == false`.
5. **T5 — Listing:** `list("materials/")` returns all entries under that prefix across all mounts, deduped by virtual path.
6. **T6 — Zip-slip rejected:** a pak with an entry named `../evil.txt` does not write/escape; entry is rejected + logged.
7. **T7 — Concurrent reads:** 8 threads reading 1000 random assets each → no crash, no data race (run under TSan/ASan).

## 7. Public references
- PKZIP / ZIP file format APPNOTE (central directory, local file headers, deflate).
- miniz documentation (single-header zip read/write).
- Quake/Doom `.pak`/`.pk4` design discussions (public articles) for the mount-priority concept.

## 8. Suggested permissive libraries
- **miniz** (MIT) — zip read + inflate. Single header.
- (Optional) **mio** (MIT) — cross-platform memory-mapped files.

## 9. Notes for the clean-room implementer
- Keep `.x3pak` = a standard zip so external tools (7-Zip) can inspect it during dev. The pak *builder* (separate tool, M6) just zips a folder + writes a manifest entry.
- The interface header must not include miniz — hide it in the .cpp.
- Add an optional manifest file inside each pak (`.x3manifest` JSON: version, build id, checksum) read by a later integrity-check slice; the VFS itself just needs read/exists/list now.
- Design `read` to optionally return a memory-mapped view later (perf), but the byte-blob API is fine for v1.
