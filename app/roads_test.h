#pragma once
// --test-echoroads — headless self-test for the LIFTED EchoRoads ROAD GRAPH.
//
// LIFT A acceptance gate. echo_roads.{h,cpp} + echo_heightfield.h were ported
// verbatim from the echotropolis lineage (branch inspx/city-blocks, tip
// bc27ce6d) onto the main lineage. EchoRoads is deterministic by construction
// (hash discipline, no rand), so "the lift changed nothing" is a checkable
// claim: build the graph from a fixed synthetic heightfield and compare a
// stable checksum against the value the SOURCE branch produces.
//
// See app/roads_test.cpp for the full assertion list. Runs with no GPU and no
// assets (the heightfield is generated in memory from a closed-form function).

namespace x3::game {
bool runEchoRoadsSelfTest();
}
