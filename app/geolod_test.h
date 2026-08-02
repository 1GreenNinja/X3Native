#pragma once
// --test-geolod — headless self-test for discrete mesh LOD + vertex compression.
// See app/geolod_test.cpp for what it asserts. Runs with no GPU.

namespace x3::game {
bool runGeoLodSelfTest();
}
