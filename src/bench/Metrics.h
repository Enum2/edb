#pragma once
//
// Metrics.h — latency percentiles and cross-run aggregation with 95% CI.
//
// Replaces the fabricated "adjusted actual Q*" accounting in the old drivers
// with honest, measured statistics: exact percentiles from captured per-op
// latencies, and mean ± 95% confidence interval across repeated runs (Student-t
// for small n, matching the paper's "5 runs" methodology).
//
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace metrics {

struct LatencyStats {
    uint64_t count = 0;
    double avg = 0, p50 = 0, p95 = 0, p99 = 0, p999 = 0, min = 0, max = 0;
};

inline double percentile(std::vector<double>& v, double q) {
    if (v.empty()) return 0.0;
    size_t idx = (size_t)(v.size() * q);
    if (idx >= v.size()) idx = v.size() - 1;
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

// Consumes (reorders) the vector.
inline LatencyStats compute(std::vector<double>& lat) {
    LatencyStats s;
    if (lat.empty()) return s;
    s.count = lat.size();
    double sum = 0;
    s.min = lat[0]; s.max = lat[0];
    for (double x : lat) { sum += x; s.min = std::min(s.min, x); s.max = std::max(s.max, x); }
    s.avg = sum / lat.size();
    s.p50  = percentile(lat, 0.50);
    s.p95  = percentile(lat, 0.95);
    s.p99  = percentile(lat, 0.99);
    s.p999 = percentile(lat, 0.999);
    return s;
}

struct CI {
    double mean = 0;
    double half_width = 0; // 95% CI half-width; report mean ± half_width
    int n = 0;
};

// Two-sided 95% Student-t critical values for df = 1..10, then ~1.96.
inline double t95(int df) {
    static const double t[] = {0, 12.706, 4.303, 3.182, 2.776, 2.571,
                               2.447, 2.365, 2.306, 2.262, 2.228};
    if (df <= 0) return 0.0;
    if (df <= 10) return t[df];
    return 1.96;
}

inline CI mean_ci95(const std::vector<double>& samples) {
    CI c;
    c.n = (int)samples.size();
    if (c.n == 0) return c;
    double sum = 0; for (double x : samples) sum += x;
    c.mean = sum / c.n;
    if (c.n == 1) return c;
    double ss = 0; for (double x : samples) ss += (x - c.mean) * (x - c.mean);
    double sd = std::sqrt(ss / (c.n - 1));
    c.half_width = t95(c.n - 1) * sd / std::sqrt((double)c.n);
    return c;
}

} // namespace metrics
