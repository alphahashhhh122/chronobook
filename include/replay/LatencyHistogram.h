#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace chronobook {

struct LatencySummary {
    uint64_t p50{0};
    uint64_t p99{0};
    uint64_t p999{0};
    uint64_t max{0};
};

class LatencyHistogram {
public:
    void record(uint64_t nanoseconds) { m_values.push_back(nanoseconds); }
    size_t count() const noexcept { return m_values.size(); }

    LatencySummary summarize() {
        LatencySummary s;
        if (m_values.empty()) return s;
        std::sort(m_values.begin(), m_values.end());
        s.p50 = percentile(0.50);
        s.p99 = percentile(0.99);
        s.p999 = percentile(0.999);
        s.max = m_values.back();
        return s;
    }

private:
    uint64_t percentile(double p) const {
        const auto idx = static_cast<size_t>(p * static_cast<double>(m_values.size() - 1));
        return m_values[idx];
    }

    std::vector<uint64_t> m_values;
};

} // namespace chronobook
