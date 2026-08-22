#pragma once

#include <atomic>
#include <cstdint>

// Minimal probe profiling: total time spent in chesstb probes and number of
// probe calls. Both counters are updated atomically from worker threads.
//
// - probe_*     tracks distance probes (DTM50/DTC/DTM).
// - wdl_probe_* tracks cheap WDL-only probes used for move filtering.
struct Timers {
    std::atomic<uint64_t> probe_ns{0};
    std::atomic<uint64_t> probe_count{0};
    std::atomic<uint64_t> wdl_probe_ns{0};
    std::atomic<uint64_t> wdl_probe_count{0};
};
