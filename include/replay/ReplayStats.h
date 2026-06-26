#pragma once
// ReplayStats.h
//
// RunningStats: single-pass mean/variance via Welford's algorithm. We never
// store the samples, so it's O(1) memory over a 1M+ message replay and is
// numerically stable (the naive sum-of-squares formula loses precision and can
// even go negative under cancellation). This is the same "running stats" idea
// the spec calls for, reused by every analytics tracker.
//
// ReplayStats: top-level counters for a replay run + throughput.
#include <cstdint>
#include <cmath>
#include <limits>

namespace chronobook {

struct RunningStats {
    uint64_t n{0};
    double   mean{0.0};
    double   m2{0.0};                                   // sum of squared deltas
    double   minV{ std::numeric_limits<double>::infinity()};
    double   maxV{-std::numeric_limits<double>::infinity()};

    void add(double x) noexcept {
        ++n;
        const double delta  = x - mean;
        mean += delta / static_cast<double>(n);
        const double delta2 = x - mean;
        m2   += delta * delta2;
        if (x < minV) minV = x;
        if (x > maxV) maxV = x;
    }
    double variance() const noexcept { return n > 1 ? m2 / static_cast<double>(n - 1) : 0.0; }
    double stddev()   const noexcept { return std::sqrt(variance()); }
    double min()      const noexcept { return n ? minV : 0.0; }
    double max()      const noexcept { return n ? maxV : 0.0; }
};

struct ReplayStats {
    uint64_t messages{0};
    uint64_t adds{0};
    uint64_t cancels{0};
    uint64_t modifies{0};
    uint64_t cancelMisses{0};   // cancels for ids not resting (filled/never-rested)
    uint64_t fills{0};
    uint64_t matchedVolume{0};  // total qty traded
    double   elapsedSeconds{0.0};

    double throughputMsgsPerSec() const noexcept {
        return elapsedSeconds > 0 ? static_cast<double>(messages) / elapsedSeconds : 0.0;
    }
};

} // namespace chronobook
