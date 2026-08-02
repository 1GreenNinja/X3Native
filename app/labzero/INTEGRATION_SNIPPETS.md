# labzero C++ drop -- integration into x3native (S0 sim layer)
Files: app/labzero/labzero_sim.h, app/labzero/labzero_tests.cpp
Suite verified 9/9 on g++ 13 (-std=c++20 -Wall, zero warnings).

1. CMake (app/CMakeLists.txt, X3Engine source list):
       labzero/labzero_tests.cpp    # labzero -- Escape from Lab Zero (LABZERO_PORT_RFC)
2. test_registry.cpp: declare `bool runLabZeroSimSelfTest();`, add TestFlags
   field + `--test-labzero` parse, and the rung:
       if (tf.testLabzero) return runLabZeroSimSelfTest() ? 0 : 1;
3. The P0 host renders FROM LabZeroSim state (read-only) and maps GLFW keys
   into LzInput per controls v2 (jumpHeld = Space||J; fireHeld = F||KP0||Alt;
   arrows aim; W/S fly). See LABZERO_3D_ADDENDUM.md P0/P1.
SPEC CORRECTIONS BY EXECUTION (update RFC where cited): T2 apex = 105 px EXACT
(discrete semi-implicit; not continuous 112.5) -> metres 2.207 (T2m gate
2.16-2.25); T3 coyote press window = 5 late steps (decrement-before-read);
T15 settle from pi/2 = 32 steps (gate <=35); T17 hover stills within 40 steps
of entry; hover order law: DAMP FIRST, THEN CANCEL GRAVITY.
