#pragma once
//
// AdaptiveBrain.h — AHSE decision layer (AlphaCalibrator + QStarGate +
// PhaseLearner + state machine). Ported from the submitted adaptive_brain.h.
//
// Behavioral semantics are UNCHANGED from the paper (§3.4). The only edits are:
//   * a successful_migrations() getter so telemetry can record the state
//     machine's counter (Experiment 1 needs this to detect cross-run leakage);
//   * should_migrate() computes the gate snapshot once instead of twice;
//   * n_keys is a real argument threaded from the engine (the old code fed a
//     hard-coded 10,000,000 regardless of dataset size).
//
#include <mutex>
#include <chrono>
#include <optional>
#include <atomic>

namespace adaptive {

// C_build(N) = alpha * N. alpha (ms/key) is a hardware property; single-sample
// update after each migration (not an EMA), per §3.4.1.
class AlphaCalibrator {
public:
    double predicted_build_ms(long long n_keys) {
        std::lock_guard<std::mutex> lk(mu_);
        return (double)n_keys * ms_per_key_;
    }
    void record_migration(long long n_keys, double elapsed_ms) {
        std::lock_guard<std::mutex> lk(mu_);
        if (n_keys > 0) ms_per_key_ = elapsed_ms / (double)n_keys;
    }
    double ms_per_key() {
        std::lock_guard<std::mutex> lk(mu_);
        return ms_per_key_;
    }
private:
    std::mutex mu_;
    double ms_per_key_{0.00026}; // prior alpha0 = 2.6e-4 ms/key (§3.4.1)
};

struct GateSnapshot {
    double C_build_ms;
    double l_base_ms;
    double l_lmdb_ms;
    double benefit_ms;
    double Q_star;
    double expected_scans;
};

// Q*-Gate: breakeven Q* = C_build / (l_base - l_lmdb); migrate iff
// expected_scans >= gamma * Q* with gamma = 1.2 (§3.4.2).
class QStarGate {
public:
    void update_scan_rate(double current_scan_rate) {
        scan_rate_ema_ = (scan_rate_samples_ == 0)
                             ? current_scan_rate
                             : (scan_rate_ema_ * 0.8 + current_scan_rate * 0.2);
        scan_rate_samples_++;
    }
    void update_latencies(double current_rocks_ms, double current_lmdb_ms) {
        if (current_rocks_ms > 0)
            l_base_ema_ = (l_base_ema_ == 0.0020) ? current_rocks_ms
                                                  : (l_base_ema_ * 0.95 + current_rocks_ms * 0.05);
        if (current_lmdb_ms > 0)
            l_lmdb_ema_ = (l_lmdb_ema_ == 0.0005) ? current_lmdb_ms
                                                  : (l_lmdb_ema_ * 0.95 + current_lmdb_ms * 0.05);
    }
    GateSnapshot snapshot(long long n_keys, double expected_read_window_s) {
        double C_build_ms = cal_.predicted_build_ms(n_keys);
        double benefit = l_base_ema_ - l_lmdb_ema_;
        if (benefit <= 0.0001) benefit = 0.001;      // delta_min floor (§3.4.2)
        double Q_star = C_build_ms / benefit;
        double rate = (scan_rate_samples_ > 0) ? scan_rate_ema_ : 7.0;
        // Amortization-aware effective window (Eq. 13 correction): scans issued
        // while the LMDB index is still building do NOT get the accelerated
        // latency, so the projected read window W must be reduced by the
        // predicted build cost before estimating how many scans actually
        // benefit. Reuse C_build_ms (already computed above); W is in seconds
        // and C_build_ms in ms, so convert. Clamp at 0 (a build longer than the
        // whole window means ~no scans benefit -> gate should decline).
        double W_effective_s = expected_read_window_s - (C_build_ms / 1000.0);
        if (W_effective_s < 0.0) W_effective_s = 0.0;
        double expected_scans = rate * W_effective_s;
        return {C_build_ms, l_base_ema_, l_lmdb_ema_, benefit, Q_star, expected_scans};
    }
    bool should_migrate(long long n_keys, double expected_read_window_s) {
        GateSnapshot s = snapshot(n_keys, expected_read_window_s);
        return s.expected_scans >= s.Q_star * 1.2; // gamma = 1.2
    }
    double scan_rate_ema() const { return scan_rate_samples_ > 0 ? scan_rate_ema_ : 7.0; }
    AlphaCalibrator& calibrator() { return cal_; }

private:
    AlphaCalibrator cal_;
    double scan_rate_ema_{7.0};
    int    scan_rate_samples_{0};
    double l_base_ema_{0.0020};
    double l_lmdb_ema_{0.0005};
};

enum class WorkloadPhase { WRITE_HEAVY, READ_HEAVY, UNKNOWN };

// Learns per-phase durations and decides predictive pre-build timing (§3.4.4).
class PhaseLearner {
public:
    void reset_timers() {
        std::lock_guard<std::mutex> lk(mu_);
        t_write_samples_ = 0; t_write_ema_ = 0.0;
        t_read_samples_ = 0;  t_read_ema_ = 0.0;
        phase_start_ = std::chrono::steady_clock::now();
        pre_build_fired_ = false;
    }
    void on_phase_change(WorkloadPhase new_phase) {
        std::lock_guard<std::mutex> lk(mu_);
        auto now = std::chrono::steady_clock::now();
        if (current_phase_ != WorkloadPhase::UNKNOWN) {
            double dur = std::chrono::duration<double>(now - phase_start_).count();
            if (current_phase_ == WorkloadPhase::WRITE_HEAVY) {
                t_write_ema_ = (t_write_samples_ == 0) ? dur : (t_write_ema_ * 0.8 + dur * 0.2);
                t_write_samples_++;
            } else {
                t_read_ema_ = (t_read_samples_ == 0) ? dur : (t_read_ema_ * 0.8 + dur * 0.2);
                t_read_samples_++;
            }
        }
        current_phase_ = new_phase;
        phase_start_ = now;
        pre_build_fired_ = false;
    }
    bool pre_build_due(double build_estimate_s) {
        std::lock_guard<std::mutex> lk(mu_);
        if (current_phase_ != WorkloadPhase::WRITE_HEAVY || pre_build_fired_ || t_write_samples_ < 1)
            return false;
        double remaining = t_write_ema_ -
            std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_start_).count();
        bool due = (remaining <= build_estimate_s + 4.0) && (remaining >= 0.0);
        if (due) pre_build_fired_ = true;
        return due;
    }
    std::optional<double> expected_read_duration_s() {
        std::lock_guard<std::mutex> lk(mu_);
        if (t_read_samples_ > 0) return t_read_ema_;
        return std::nullopt;
    }

private:
    std::mutex mu_;
    WorkloadPhase current_phase_{WorkloadPhase::UNKNOWN};
    std::chrono::steady_clock::time_point phase_start_{std::chrono::steady_clock::now()};
    double t_write_ema_{0.0}; int t_write_samples_{0};
    double t_read_ema_{0.0};  int t_read_samples_{0};
    bool pre_build_fired_{false};
};

class AdaptiveBrain {
public:
    enum class Decision {
        TRIGGER_PREDICTIVE, TRIGGER_REACTIVE,
        BLOCK_INSUFFICIENT_DATA, BLOCK_Q_STAR, NONE
    };

    QStarGate& gate() { return gate_; }
    PhaseLearner& learner() { return learner_; }
    int successful_migrations() const { return successful_migrations_; }

    // --- Counter split (telemetry clarity only; state-machine behavior is
    //     UNCHANGED). The single "successful_migrations" field historically
    //     conflated two very different quantities; they are now exposed
    //     separately so telemetry cannot be misread as oscillation. ---
    //
    // gate_approvals(): the per-tick Q*-gate "migrate" vote tally. This is the
    //   SAME integer the state machine uses for its ==0 / >=1 branching; it
    //   increments once per 500ms read-heavy window and does NOT correspond to
    //   physical builds. (Alias of successful_migrations_.)
    int gate_approvals() const { return successful_migrations_; }
    //
    // physical_migrations(): the number of COMPLETED LMDB build windows. The
    //   engine calls on_migration_complete() exactly once per finished
    //   is_migrating window, so this counts real migrate (and, across phase
    //   alternation, migrate/drop/re-migrate) cycles.
    int physical_migrations() const {
        return physical_migrations_.load(std::memory_order_relaxed);
    }
    void on_migration_complete() {
        physical_migrations_.fetch_add(1, std::memory_order_relaxed);
    }

    // Reset for a new cycle: keep calibrated alpha but re-enter Trained state
    // (successful_migrations = 1) so the next trigger faces the full Q* gate
    // instead of repeating the blind calibration run (§3.4.3).
    void reset_for_new_cycle() { if (successful_migrations_ < 1) successful_migrations_ = 1; }

    Decision tick(long long n_keys, WorkloadPhase current_phase, bool stochastic_trigger) {
        double expected_window = learner_.expected_read_duration_s().value_or(45.0);

        if (current_phase == WorkloadPhase::WRITE_HEAVY) {
            double build_s = gate_.snapshot(n_keys, expected_window).C_build_ms / 1000.0;
            if (successful_migrations_ >= 1 && learner_.pre_build_due(build_s))
                return Decision::TRIGGER_PREDICTIVE;
        }

        if (stochastic_trigger) {
            if (successful_migrations_ == 0) {
                successful_migrations_++;               // bootstrap alpha
                return Decision::BLOCK_INSUFFICIENT_DATA; // reactive_fire still set by caller
            }
            if (gate_.should_migrate(n_keys, expected_window)) {
                successful_migrations_++;
                return Decision::TRIGGER_REACTIVE;
            }
            return Decision::BLOCK_Q_STAR;
        }
        return Decision::NONE;
    }

private:
    QStarGate gate_;
    PhaseLearner learner_;
    int successful_migrations_{0};                 // = gate_approvals (per-tick votes)
    std::atomic<int> physical_migrations_{0};      // completed build windows
};

} // namespace adaptive
