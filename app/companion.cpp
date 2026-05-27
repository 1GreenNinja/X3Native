#include "companion.h"
#include "engine/core/x3_log.h"   // for x3::logInfo / x3::logError
#include <cmath>
#include <string>

namespace x3::game {

float CompanionBrain::score(CompanionBehavior, const CompanionContext&) const { return 0.0f; }

CompanionCommand CompanionBrain::tick(const CompanionContext&) const {
    return CompanionCommand{};   // later tasks fill this in
}

bool runCompanionSelfTest() {
    x3::logInfo("running companion reflex-AI self-test...");
    int pass = 0, total = 0;
    // (scenarios added in later tasks)
    x3::logInfo("companion: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
