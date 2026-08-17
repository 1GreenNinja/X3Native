// See map_poi.h. A static registry — no globals-init-order surprises because
// nothing here reads the vector before main() runs, and every writer is a
// plain function call, not a static initializer racing another TU's.
#include "map_poi.h"

namespace x3::worldpoi {

namespace {
std::vector<MapPoi>& registry() {
    static std::vector<MapPoi> pois;
    return pois;
}
} // namespace

void registerMapPoi(const MapPoi& poi) { registry().push_back(poi); }

const std::vector<MapPoi>& allMapPois() { return registry(); }

void clearMapPois() { registry().clear(); }

} // namespace x3::worldpoi
