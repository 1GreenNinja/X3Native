#pragma once
// Asset Source / Pak VFS interface — D5.
// Spec: specs/D5-asset-source.spec.md
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

    // Higher priority wins on path collision.
    virtual bool mountPak(std::string_view pakPath, int priority) = 0;
    virtual bool mountDir(std::string_view dirPath, int priority) = 0; // dev override

    virtual Blob read(std::string_view virtualPath) = 0;
    virtual bool exists(std::string_view virtualPath) const = 0;
    virtual std::vector<std::string> list(std::string_view prefix) const = 0;

    virtual void refresh() = 0;
};

IAssetSource* createAssetSource();

// Runs the D5 acceptance tests in-process (creates temp paks via miniz,
// mounts, reads, checks priority/missing/list/zip-slip). Returns true if all
// pass. Implemented in PakAssetSource.cpp (where miniz is available).
bool runAssetSelfTest();

} // namespace x3::asset
