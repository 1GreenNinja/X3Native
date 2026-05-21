// Pak/VFS implementation of IAssetSource — D5 (clean-room).
// Spec: specs/D5-asset-source.spec.md
//
// SKELETON STATUS: stub so the engine library links. The 13700K clean-room
// team implements this from the D5 spec using miniz (zip read) — mount
// priority, virtual-path index, zip-slip rejection, concurrent reads, and
// the 7 acceptance tests. Until then every read returns ok=false.

#include "IAssetSource.h"
#include "../core/x3_log.h"

namespace x3::asset {

namespace {

class PakAssetSource final : public IAssetSource {
public:
    bool mountPak(std::string_view pakPath, int) override {
        logWarn(std::string("[asset] mountPak stub (D5 not implemented): ") + std::string(pakPath));
        return false; // TODO(13700K): miniz zip mount + central-directory index
    }
    bool mountDir(std::string_view dirPath, int) override {
        logWarn(std::string("[asset] mountDir stub (D5 not implemented): ") + std::string(dirPath));
        return false; // TODO(13700K): loose-file dev override
    }
    Blob read(std::string_view) override { return Blob{}; }           // TODO
    bool exists(std::string_view) const override { return false; }    // TODO
    std::vector<std::string> list(std::string_view) const override { return {}; } // TODO
    void refresh() override {}                                        // TODO
};

} // namespace

IAssetSource* createAssetSource() { return new PakAssetSource(); }

} // namespace x3::asset
