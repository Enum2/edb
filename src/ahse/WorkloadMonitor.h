#pragma once
//
// WorkloadMonitor.h (AHSE) — background sampling thread + decision emission.
//
// Ported from the submitted WorkloadMonitor.h. Behavioral logic (500ms windows,
// exchange-to-zero counters, K-of-5 stochastic consensus, drain-and-average
// latency accumulators, decision routing) is preserved from the paper (§3.3).
//
// Changes for the resubmission:
//   * dataset_size is the REAL key count (threaded in by the engine), not a
//     hard-coded 10,000,000 — this un-breaks the Q* accounting for every size.
//   * the monitor holds read-only pointers to the engine's is_lmdb_ready /
//     is_migrating atomics so every telemetry tick records the full control
//     state (Experiment 1 / 4 instrumentation).
//   * telemetry writes the extended schema (phase, flags, successful_migrations,
//     scan-rate EMA, Q* accounting).
//
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "AdaptiveBrain.h"
#include "TelemetryLogger.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <deque>
#include <vector>
#include <algorithm>
#include <mutex>
#include <string>

enum class WorkloadState { WRITE_HEAVY, READ_HEAVY };

class WorkloadMonitor {
public:
    adaptive::AdaptiveBrain brain;
    std::atomic<bool> reactive_fire{false};
    std::atomic<bool> predictive_fire{false};
    std::atomic<bool> drop_index_fire{false};
    std::atomic<double> last_predicted_q_star{-1.0};

    // engine_lmdb_ready / engine_migrating: read-only views of the engine's
    // control flags, for telemetry. verbose: print human-readable events.
    WorkloadMonitor(long long dataset_size,
                    TelemetryLogger* logger,
                    const std::atomic<bool>* engine_lmdb_ready,
                    const std::atomic<bool>* engine_migrating,
                    int window_duration_ms = 500,
                    bool verbose = false)
        : dataset_size_(dataset_size),
          window_ms_(window_duration_ms),
          logger_(logger),
          engine_lmdb_ready_(engine_lmdb_ready),
          engine_migrating_(engine_migrating),
          verbose_(verbose) {
        current_window_latencies_.reserve(10000);
        // Parameter-sensitivity hooks (item 3c): override the monitor window and
        // the K-of-5 consensus threshold from the environment. Env-gated, so the
        // default configuration (500 ms, K=3) is unchanged when unset.
        if (const char* w = std::getenv("AHSE_WINDOW_MS")) { int v = std::atoi(w); if (v > 0) window_ms_ = v; }
        if (const char* k = std::getenv("AHSE_K"))         { int v = std::atoi(k); if (v >= 1 && v <= 5) K_ = v; }
        brain.learner().on_phase_change(adaptive::WorkloadPhase::WRITE_HEAVY);
        // Diagnostic switch (AHSE_NO_MONITOR): skip the polling thread so the
        // engine runs as pure RocksDB pass-through. Used only to measure the
        // monitoring tax on point-read workloads; default path is unchanged.
        if (std::getenv("AHSE_NO_MONITOR") == nullptr)
            background_thread_ = std::thread(&WorkloadMonitor::observer_loop, this);
    }

    ~WorkloadMonitor() {
        running_.store(false);
        if (background_thread_.joinable()) background_thread_.join();
    }

    void record_write() { write_ops_.fetch_add(1, std::memory_order_relaxed); }
    void record_read()  { read_ops_.fetch_add(1, std::memory_order_relaxed); }
    WorkloadState get_state() const { return current_state_.load(); }

    // Only range-scan latencies feed the Q* EMAs (l_base / l_lmdb are "scan
    // latency" in the paper). used_lmdb selects the accumulator.
    void record_scan_latency(bool used_lmdb, double ms) {
        if (used_lmdb) {
            add_atomic(lmdb_latency_sum_, ms);
            lmdb_latency_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            add_atomic(rocks_latency_sum_, ms);
            rocks_latency_count_.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lk(p99_mu_);
        current_window_latencies_.push_back(ms);
    }

private:
    static void add_atomic(std::atomic<double>& acc, double v) {
        double cur = acc.load(std::memory_order_relaxed);
        while (!acc.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {}
    }

    void observer_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(window_ms_));

            int reads  = read_ops_.exchange(0);
            int writes = write_ops_.exchange(0);
            int total  = reads + writes;

            int    r_count = rocks_latency_count_.exchange(0);
            double r_sum   = rocks_latency_sum_.exchange(0.0);
            int    l_count = lmdb_latency_count_.exchange(0);
            double l_sum   = lmdb_latency_sum_.exchange(0.0);

            int total_lat_count = r_count + l_count;
            double overall_avg = (total_lat_count > 0) ? ((r_sum + l_sum) / total_lat_count) : 0.0;
            double avg_rocks = (r_count > 0) ? (r_sum / r_count) : -1.0;
            double avg_lmdb  = (l_count > 0) ? (l_sum / l_count) : -1.0;
            brain.gate().update_latencies(avg_rocks, avg_lmdb);

            std::vector<double> window_lats;
            {
                std::lock_guard<std::mutex> lk(p99_mu_);
                window_lats = std::move(current_window_latencies_);
                current_window_latencies_.reserve(10000);
            }
            double p99 = 0.0;
            if (!window_lats.empty()) {
                size_t idx = static_cast<size_t>(window_lats.size() * 0.99);
                if (idx >= window_lats.size()) idx = window_lats.size() - 1;
                std::nth_element(window_lats.begin(), window_lats.begin() + idx, window_lats.end());
                p99 = window_lats[idx];
            }

            if (total <= 0) continue;

            double read_ratio  = (double)reads / total;
            double write_ratio = 1.0 - read_ratio;
            // Hysteresis fix: enter read-heavy at r>0.80, but stay read-heavy
            // until r<0.70. A workload sitting right at the r=0.80 boundary
            // (e.g. an 80%-read analytics phase) otherwise flickers the
            // classifier and thrashes migrate/drop. Two thresholds break the
            // flicker: you only leave read-heavy once reads clearly recede.
            bool   is_read_heavy =
                (current_state_.load() == WorkloadState::READ_HEAVY)
                    ? (read_ratio > kExitThreshold)     // sticky while read-heavy
                    : (read_ratio > kEnterThreshold);   // strict to enter
            double current_scan_rate = reads * (1000.0 / window_ms_);
            double ops_per_sec = total * (1000.0 / window_ms_);
            brain.gate().update_scan_rate(current_scan_rate);

            history_.push_back(is_read_heavy);
            if (history_.size() > 5) history_.pop_front();
            int read_heavy_count = 0;
            for (bool v : history_) if (v) read_heavy_count++;
            bool stochastic_trigger = (read_heavy_count >= K_);

            WorkloadState next_state = current_state_.load();
            std::string cycle_event = "NONE";

            if (current_state_.load() != WorkloadState::READ_HEAVY && stochastic_trigger) {
                next_state = WorkloadState::READ_HEAVY;
                block_msg_printed_ = false;
            } else if (current_state_.load() == WorkloadState::READ_HEAVY && read_heavy_count == 0) {
                next_state = WorkloadState::WRITE_HEAVY;
                if (verbose_) std::fprintf(stderr, "[BRAIN] REVERTING to Write-Heavy. Dropping LMDB.\n");
                reactive_fire.store(false);
                predictive_fire.store(false);
                drop_index_fire.store(true);
                block_msg_printed_ = false;
                cycle_event = "INDEX_DROPPED";
                brain.reset_for_new_cycle();
            }

            auto phase_enum = (current_state_.load() == WorkloadState::READ_HEAVY)
                                  ? adaptive::WorkloadPhase::READ_HEAVY
                                  : adaptive::WorkloadPhase::WRITE_HEAVY;
            auto decision = brain.tick(dataset_size_, phase_enum, stochastic_trigger);

            if (predictive_fire.load() && phase_enum == adaptive::WorkloadPhase::READ_HEAVY)
                decision = adaptive::AdaptiveBrain::Decision::NONE;

            auto snap = brain.gate().snapshot(
                dataset_size_, brain.learner().expected_read_duration_s().value_or(45.0));

            TelemetryRecord tr;
            tr.engine = "ahse";
            tr.state = (current_state_.load() == WorkloadState::READ_HEAVY) ? "READ_HEAVY" : "WRITE_HEAVY";
            tr.op_type = is_read_heavy ? "scan" : "write";
            tr.dataset_size = (int)dataset_size_;
            tr.read_ratio = read_ratio;
            tr.write_ratio = write_ratio;
            tr.ops_per_sec = ops_per_sec;
            tr.scan_rate_ema = brain.gate().scan_rate_ema();
            tr.avg_latency_ms = overall_avg;
            tr.p99_latency_ms = p99;
            tr.successful_migrations = brain.gate_approvals();       // per-tick vote tally
            tr.physical_migrations = brain.physical_migrations();    // completed build windows
            tr.reactive_fire = reactive_fire.load();
            tr.predictive_fire = predictive_fire.load();
            tr.drop_index_fire = drop_index_fire.load();
            tr.is_lmdb_ready = engine_lmdb_ready_ ? engine_lmdb_ready_->load() : false;
            tr.is_migrating = engine_migrating_ ? engine_migrating_->load() : false;
            tr.expected_q = snap.expected_scans;
            tr.breakeven_q_star = snap.Q_star;
            tr.build_cost_ms = snap.C_build_ms;
            tr.benefit_ms = snap.benefit_ms;
            tr.event = cycle_event;

            // Re-arming fix: allow a migration to be armed on ANY read-heavy
            // tick (not only the write->read transition tick), provided no
            // index is built or currently building. Previously reactive_fire
            // could only be set while current_state_ != READ_HEAVY, so if the
            // Q*-gate declined on the single transition tick (EMAs still ramping
            // at the phase boundary) the entire read phase was skipped even
            // though the gate approved a tick later. The engine's
            // maybe_start_migration() CAS on is_migrating_ plus its
            // !is_lmdb_ready guard already prevent a duplicate build, so
            // re-arming on later read-heavy ticks cannot cause a second build.
            bool index_present =
                (engine_lmdb_ready_ && engine_lmdb_ready_->load()) ||
                (engine_migrating_  && engine_migrating_->load());

            using D = adaptive::AdaptiveBrain::Decision;
            if (decision == D::TRIGGER_PREDICTIVE) {
                tr.trigger_decision = "PREDICTIVE";
                tr.event = "PREDICTIVE_TRIGGER_FIRED";
                tr.migration_triggered = true;
                if (verbose_) std::fprintf(stderr, "[BRAIN] PREDICTIVE MIGRATION INITIATED\n");
                predictive_fire.store(true);
            } else if (decision == D::BLOCK_INSUFFICIENT_DATA && !index_present) {
                tr.trigger_decision = "CALIBRATION_RUN";
                tr.event = "CALIBRATION_TRIGGER_FIRED";
                tr.migration_triggered = true;
                if (verbose_) std::fprintf(stderr, "[BRAIN] CALIBRATION MIGRATION INITIATED (first run)\n");
                reactive_fire.store(true);
            } else if (decision == D::TRIGGER_REACTIVE && !index_present) {
                tr.trigger_decision = "REACTIVE_QSTAR";
                tr.event = "REACTIVE_TRIGGER_FIRED";
                tr.migration_triggered = true;
                last_predicted_q_star.store(snap.Q_star);
                if (verbose_) std::fprintf(stderr, "[BRAIN] MIGRATION APPROVED by Q*-Gate\n");
                reactive_fire.store(true);
            } else if (decision == D::BLOCK_Q_STAR &&
                       current_state_.load() != WorkloadState::READ_HEAVY) {
                tr.trigger_decision = "BLOCKED_ROI";
                if (!block_msg_printed_) {
                    tr.event = "BLOCKED_NEGATIVE_ROI";
                    last_predicted_q_star.store(snap.Q_star);
                    block_msg_printed_ = true;
                }
            }

            if (logger_) logger_->log(tr);

            if (next_state != current_state_.load()) {
                current_state_.store(next_state);
                brain.learner().on_phase_change(
                    (next_state == WorkloadState::READ_HEAVY)
                        ? adaptive::WorkloadPhase::READ_HEAVY
                        : adaptive::WorkloadPhase::WRITE_HEAVY);
            }
        }
    }

    std::atomic<int> read_ops_{0};
    std::atomic<int> write_ops_{0};
    std::atomic<WorkloadState> current_state_{WorkloadState::WRITE_HEAVY};

    std::atomic<double> rocks_latency_sum_{0.0};
    std::atomic<int>    rocks_latency_count_{0};
    std::atomic<double> lmdb_latency_sum_{0.0};
    std::atomic<int>    lmdb_latency_count_{0};

    std::mutex p99_mu_;
    std::vector<double> current_window_latencies_;

    long long dataset_size_;
    int window_ms_;
    TelemetryLogger* logger_;
    const std::atomic<bool>* engine_lmdb_ready_;
    const std::atomic<bool>* engine_migrating_;
    bool verbose_;

    std::thread background_thread_;
    std::atomic<bool> running_{true};
    std::deque<bool> history_;
    int K_ = 3;   // K-of-5 consensus threshold (overridable via AHSE_K for item 3c)
    // Hysteresis band for the read-heavy classifier (fix for r~0.80 thrashing).
    static constexpr double kEnterThreshold = 0.80; // enter read-heavy above this
    static constexpr double kExitThreshold  = 0.70; // leave read-heavy below this
    bool block_msg_printed_ = false;
};
