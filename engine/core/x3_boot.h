#pragma once
// [boot] timeline — timestamped boot phases from process start to the first
// interactive frame. The clock starts in a static initializer in x3_boot.cpp
// (i.e. at engine-image load, before main()), so the marks include static-init
// + CRT time, not just main()-onward. Main-thread only (boot is single-threaded
// up to the first interactive frame); marks log immediately so a crash mid-boot
// still leaves the partial timeline in the log.
//
//   x3::boot::mark("device init");      // logs [boot] device init  +812.4 ms  (t=903.1 ms)
//   double total = x3::boot::report("first interactive frame");
//
// report() prints the full phase table (every mark with its delta) + the total
// and returns total ms. Used by app/main.cpp's --test-boottime gate and the
// [boot] total line in --smoketest. See docs/BOOT_TIME.md.
#include <cstdint>

namespace x3::boot {

// Record the END of a named boot phase. Logs the delta since the previous mark
// (or since process start for the first mark) and the running total.
void mark(const char* phase);

// Milliseconds elapsed since process start (the static-init clock).
double sinceStartMs();

// Log the full phase table + the total line:
//   [boot] ---- phase table ----
//   [boot]   <phase>  <delta> ms
//   ...
//   [boot] TOTAL <totalLabel>: <ms> ms
// Returns the total (ms since process start at the time of the call).
double report(const char* totalLabel);

} // namespace x3::boot
