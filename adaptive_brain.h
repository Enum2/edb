#pragma once
#include <mutex>
#include <iostream>
#include <chrono>
#include <optional>

namespace adaptive {

class AlphaCalibrator {
private:
    std::mutex mu_;
    double ms_per_key_{0.00026}; 
public:
    double predicted_build_ms(int n_keys) { std::lock_guard<std::mutex> lk(mu_); return n_keys * ms_per_key_; }
    void record_migration(int n_keys, double elapsed_ms) { std::lock_guard<std::mutex> lk(mu_); if (n_keys > 0) ms_per_key_ = elapsed_ms / n_keys; }
};

struct GateSnapshot {
    double C_build_ms; double l_base_ms; double l_lmdb_ms; double benefit_ms; double Q_star; double expected_scans;
};

class QStarGate {
private:
    AlphaCalibrator cal_;
    double scan_rate_ema_{7.0}; int scan_rate_samples_{0};
    double l_base_ema_{0.0020}; double l_lmdb_ema_{0.0005};
public:
    void update_scan_rate(double current_scan_rate) {
        scan_rate_ema_ = (scan_rate_samples_ == 0) ? current_scan_rate : (scan_rate_ema_ * 0.8 + current_scan_rate * 0.2);
        scan_rate_samples_++;
    }
    void update_latencies(double current_rocks_ms, double current_lmdb_ms) {
        if (current_rocks_ms > 0) l_base_ema_ = (l_base_ema_ == 0.0020) ? current_rocks_ms : (l_base_ema_ * 0.95 + current_rocks_ms * 0.05);
        if (current_lmdb_ms > 0) l_lmdb_ema_ = (l_lmdb_ema_ == 0.0005) ? current_lmdb_ms : (l_lmdb_ema_ * 0.95 + current_lmdb_ms * 0.05);
    }
    GateSnapshot snapshot(int n_keys, double expected_read_window_s) {
        double C_build_ms = cal_.predicted_build_ms(n_keys);
        double benefit = l_base_ema_ - l_lmdb_ema_;
        if (benefit <= 0.0001) benefit = 0.001; 
        double Q_star = C_build_ms / benefit;
        double expected_scans = (scan_rate_samples_ > 0 ? scan_rate_ema_ : 7.0) * expected_read_window_s;
        return {C_build_ms, l_base_ema_, l_lmdb_ema_, benefit, Q_star, expected_scans}; 
    }
    bool should_migrate(int n_keys, double expected_read_window_s) {
        return (snapshot(n_keys, expected_read_window_s).expected_scans >= snapshot(n_keys, expected_read_window_s).Q_star * 1.2);
    }
    AlphaCalibrator& calibrator() { return cal_; }
};

enum class WorkloadPhase { WRITE_HEAVY, READ_HEAVY, UNKNOWN };

class PhaseLearner {
private:
    std::mutex mu_;
    WorkloadPhase current_phase_{WorkloadPhase::UNKNOWN};
    std::chrono::steady_clock::time_point phase_start_;
    double t_write_ema_{0.0}; int t_write_samples_{0};
    double t_read_ema_{0.0};  int t_read_samples_{0};
    bool pre_build_fired_{false};

public:
    void reset_timers() {
        std::lock_guard<std::mutex> lk(mu_);
        t_write_samples_ = 0; t_write_ema_ = 0.0;
        t_read_samples_ = 0; t_read_ema_ = 0.0;
        phase_start_ = std::chrono::steady_clock::now();
        pre_build_fired_ = false;
    }

    void on_phase_change(WorkloadPhase new_phase) {
        std::lock_guard<std::mutex> lk(mu_);
        auto now = std::chrono::steady_clock::now();
        if (current_phase_ != WorkloadPhase::UNKNOWN) {
            double duration_s = std::chrono::duration<double>(now - phase_start_).count();
            if (current_phase_ == WorkloadPhase::WRITE_HEAVY) { t_write_ema_ = (t_write_samples_==0)?duration_s:(t_write_ema_*0.8+duration_s*0.2); t_write_samples_++; }
            else { t_read_ema_ = (t_read_samples_==0)?duration_s:(t_read_ema_*0.8+duration_s*0.2); t_read_samples_++; }
        }
        current_phase_ = new_phase; phase_start_ = now; pre_build_fired_ = false; 
    }

    bool pre_build_due(double build_estimate_s) {
        std::lock_guard<std::mutex> lk(mu_);
        if (current_phase_ != WorkloadPhase::WRITE_HEAVY || pre_build_fired_ || t_write_samples_ < 1) return false;
        double remaining_s = t_write_ema_ - std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_start_).count();
        
        bool due = (remaining_s <= build_estimate_s + 4.0) && (remaining_s >= 0.0); 
        if (due) pre_build_fired_ = true;
        return due;
    }
    
    std::optional<double> expected_read_duration_s() {
        std::lock_guard<std::mutex> lk(mu_);
        if (t_read_samples_ > 0) return t_read_ema_; return std::nullopt;
    }
};

class AdaptiveBrain {
public:
    enum class Decision { TRIGGER_PREDICTIVE, TRIGGER_REACTIVE, BLOCK_INSUFFICIENT_DATA, BLOCK_Q_STAR, NONE };
private:
    QStarGate gate_; PhaseLearner learner_; int successful_migrations_{0};
public:
    QStarGate& gate() { return gate_; }
    PhaseLearner& learner() { return learner_; }

    Decision tick(int n_keys, WorkloadPhase current_phase, bool stochastic_trigger) {
        double expected_window = learner_.expected_read_duration_s().value_or(45.0);
        
        if (current_phase == WorkloadPhase::WRITE_HEAVY) {
            if (learner_.pre_build_due(gate_.snapshot(n_keys, expected_window).C_build_ms / 1000.0)) return Decision::TRIGGER_PREDICTIVE;
        }

        if (stochastic_trigger) {
            if (successful_migrations_ == 0) { successful_migrations_++; return Decision::BLOCK_INSUFFICIENT_DATA; }
            if (gate_.should_migrate(n_keys, expected_window)) { successful_migrations_++; return Decision::TRIGGER_REACTIVE; }
            return Decision::BLOCK_Q_STAR;
        }
        return Decision::NONE;
    }
};
} // namespace adaptive
