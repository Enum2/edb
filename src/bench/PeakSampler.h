#pragma once
//
// PeakSampler.h — background RSS high-water sampler for a single run.
//
// Samples resident set size every ~50ms on a background thread and keeps the
// maximum, so the reported peak RAM reflects the whole run (load + timed ops)
// rather than a single end-of-run snapshot. For a clean per-run peak, invoke the
// binary with --runs 1 from run_all.sh so each repetition is its own process.
//
#include "../Common.h"
#include <atomic>
#include <thread>
#include <chrono>

class PeakSampler {
public:
    PeakSampler() {
        peak_.store(mem::current_rss_mb());
        thread_ = std::thread([this] {
            while (running_.load()) {
                double cur = mem::current_rss_mb();
                double p = peak_.load();
                while (cur > p && !peak_.compare_exchange_weak(p, cur)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    ~PeakSampler() { stop(); }
    void stop() {
        bool was = running_.exchange(false);
        if (was && thread_.joinable()) thread_.join();
    }
    double peak_mb() const { return peak_.load(); }

private:
    std::atomic<bool> running_{true};
    std::atomic<double> peak_{0.0};
    std::thread thread_;
};
