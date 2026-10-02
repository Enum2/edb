#pragma once
#include <atomic>
#include <thread>
#include <chrono>
#include <deque>
#include <iostream>
#include <vector>
#include <algorithm>
#include <mutex>
#include "adaptive_brain.h" 
#include "TelemetryLogger.h"

enum class WorkloadState { WRITE_HEAVY, READ_HEAVY };

class WorkloadMonitor {
private:
    std::atomic<int> read_ops{0};
    std::atomic<int> write_ops{0};
    std::atomic<WorkloadState> current_state{WorkloadState::WRITE_HEAVY};
    
    std::atomic<double> rocks_latency_sum{0.0};
    std::atomic<int> rocks_latency_count{0};
    std::atomic<double> lmdb_latency_sum{0.0};
    std::atomic<int> lmdb_latency_count{0};

    std::mutex p99_mu;
    std::vector<double> current_window_latencies;

    int dataset_size_;
    int window_ms;
    std::thread background_thread;
    std::atomic<bool> running{true};
    std::deque<bool> history; 
    const int K = 3; 
    bool block_msg_printed = false; 

public:
    adaptive::AdaptiveBrain brain;
    std::atomic<bool> reactive_fire{false}; 
    std::atomic<bool> predictive_fire{false};
    std::atomic<bool> drop_index_fire{false};
    std::atomic<double> last_predicted_q_star{-1.0}; 

    WorkloadMonitor(int dataset_size = 1000000, int window_duration_ms = 500) 
        : dataset_size_(dataset_size), window_ms(window_duration_ms) {
        current_window_latencies.reserve(10000); 
        brain.learner().on_phase_change(adaptive::WorkloadPhase::WRITE_HEAVY);
        background_thread = std::thread(&WorkloadMonitor::observer_loop, this);
    }

    ~WorkloadMonitor() {
        running = false;
        if(background_thread.joinable()) background_thread.join();
    }

    void record_write() { write_ops.fetch_add(1, std::memory_order_relaxed); }
    void record_read() { read_ops.fetch_add(1, std::memory_order_relaxed); }
    WorkloadState get_state() const { return current_state.load(); }

    void record_read_latency(bool is_lmdb, double ms) {
        if (is_lmdb) {
            double current = lmdb_latency_sum.load(std::memory_order_relaxed);
            while(!lmdb_latency_sum.compare_exchange_weak(current, current + ms, std::memory_order_relaxed));
            lmdb_latency_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            double current = rocks_latency_sum.load(std::memory_order_relaxed);
            while(!rocks_latency_sum.compare_exchange_weak(current, current + ms, std::memory_order_relaxed));
            rocks_latency_count.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(p99_mu);
            current_window_latencies.push_back(ms);
        }
    }

private:
    void observer_loop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
            
            int reads = read_ops.exchange(0);
            int writes = write_ops.exchange(0);
            int total = reads + writes;

            int r_count = rocks_latency_count.exchange(0);
            double r_sum = rocks_latency_sum.exchange(0.0);
            int l_count = lmdb_latency_count.exchange(0);
            double l_sum = lmdb_latency_sum.exchange(0.0);
            
            int total_latency_count = r_count + l_count;
            double overall_avg_latency = (total_latency_count > 0) ? ((r_sum + l_sum) / total_latency_count) : 0.0;
            
            double avg_rocks = (r_count > 0) ? (r_sum / r_count) : -1.0;
            double avg_lmdb = (l_count > 0) ? (l_sum / l_count) : -1.0;
            
            brain.gate().update_latencies(avg_rocks, avg_lmdb); 

            std::vector<double> window_lats;
            {
                std::lock_guard<std::mutex> lock(p99_mu);
                window_lats = std::move(current_window_latencies);
                current_window_latencies.reserve(10000); 
            }

            double p99_latency = 0.0;
            if (!window_lats.empty()) {
                size_t p99_index = static_cast<size_t>(window_lats.size() * 0.99);
                if (p99_index >= window_lats.size()) p99_index = window_lats.size() - 1;
                std::nth_element(window_lats.begin(), window_lats.begin() + p99_index, window_lats.end());
                p99_latency = window_lats[p99_index];
            }
            
            if (total > 0) {
                double read_ratio = (double)reads / total;
                double write_ratio = 1.0 - read_ratio;
                bool is_read_heavy = (read_ratio > 0.80);
                
                double current_scan_rate = reads * (1000.0 / window_ms); 
                double ops_per_sec = total * (1000.0 / window_ms);
                
                brain.gate().update_scan_rate(current_scan_rate);

                history.push_back(is_read_heavy);
                if (history.size() > 5) history.pop_front();
                
                int read_heavy_count = 0;
                for (bool val : history) if (val) read_heavy_count++;
                bool stochastic_trigger = (read_heavy_count >= K);
                
                WorkloadState next_state = current_state.load();
                std::string cycle_event = "NONE";

                if (current_state.load() != WorkloadState::READ_HEAVY && stochastic_trigger) {
                    next_state = WorkloadState::READ_HEAVY;
                    block_msg_printed = false;
                } else if (current_state.load() == WorkloadState::READ_HEAVY && read_heavy_count == 0) {
                    next_state = WorkloadState::WRITE_HEAVY;
                    std::cout << "\n[BRAIN] 📉 REVERTING. Workload became Write-Heavy. Dropping LMDB.\n";
                    reactive_fire.store(false); 
                    predictive_fire.store(false); 
                    drop_index_fire.store(true); 
                    block_msg_printed = false;
                    cycle_event = "INDEX_DROPPED";
                }

                auto current_phase_enum = (current_state.load() == WorkloadState::READ_HEAVY) ? adaptive::WorkloadPhase::READ_HEAVY : adaptive::WorkloadPhase::WRITE_HEAVY;
                auto decision = brain.tick(dataset_size_, current_phase_enum, stochastic_trigger);

                if (predictive_fire.load() && current_phase_enum == adaptive::WorkloadPhase::READ_HEAVY) {
                    decision = adaptive::AdaptiveBrain::Decision::NONE; 
                }

                TelemetryRecord tr;
                
                if (current_state.load() == WorkloadState::READ_HEAVY && (reactive_fire.load() || predictive_fire.load())) {
                    tr.engine = "adaptive"; 
                } else {
                    tr.engine = "baseline_rocksdb";
                }

                tr.dataset_size = dataset_size_;
                tr.op_type = is_read_heavy ? "scan" : "write";
                tr.event = cycle_event;
                tr.read_ratio = read_ratio;
                tr.write_ratio = write_ratio;
                tr.ops_per_sec = ops_per_sec;
                tr.avg_latency_ms = overall_avg_latency; 
                tr.p99_latency_ms = p99_latency; 
                tr.actual_phase = (next_state == WorkloadState::READ_HEAVY) ? "scan" : "write";

                auto snap = brain.gate().snapshot(dataset_size_, brain.learner().expected_read_duration_s().value_or(45.0));
                tr.estimated_cost_ms = snap.C_build_ms;
                tr.estimated_benefit_ms = snap.benefit_ms; 
                tr.expected_q = snap.expected_scans;       
                tr.breakeven_q_star = snap.Q_star;

                if (decision == adaptive::AdaptiveBrain::Decision::TRIGGER_PREDICTIVE) {
                    tr.trigger_decision = "PREDICTIVE";
                    tr.event = "PREDICTIVE_TRIGGER_FIRED";
                    tr.migration_triggered = true;
                    std::cout << "\n[BRAIN] 🔮 PREDICTIVE MIGRATION INITIATED!\n";
                    predictive_fire.store(true);
                } 
                else if (decision == adaptive::AdaptiveBrain::Decision::BLOCK_INSUFFICIENT_DATA && current_state.load() != WorkloadState::READ_HEAVY) {
                    tr.trigger_decision = "CALIBRATION_RUN";
                    tr.event = "CALIBRATION_TRIGGER_FIRED";
                    tr.migration_triggered = true; 
                    std::cout << "\n[BRAIN] 🛠️ CALIBRATION MIGRATION INITIATED (First Run)!\n";
                    reactive_fire.store(true);
                }
                else if (decision == adaptive::AdaptiveBrain::Decision::TRIGGER_REACTIVE && current_state.load() != WorkloadState::READ_HEAVY) {
                    tr.trigger_decision = "REACTIVE_QSTAR";
                    tr.event = "REACTIVE_TRIGGER_FIRED";
                    tr.migration_triggered = true; 
                    last_predicted_q_star.store(snap.Q_star);
                    std::cout << "[BRAIN] 🔥 MIGRATION APPROVED by Q-Star Gate!\n";
                    reactive_fire.store(true);
                } 
                else if (decision == adaptive::AdaptiveBrain::Decision::BLOCK_Q_STAR && current_state.load() != WorkloadState::READ_HEAVY) {
                    tr.trigger_decision = "BLOCKED_ROI";
                    if (!block_msg_printed) {
                        tr.event = "BLOCKED_NEGATIVE_ROI";
                        last_predicted_q_star.store(snap.Q_star);
                        block_msg_printed = true; 
                    }
                }

                extern TelemetryLogger telemetry_logger;
                telemetry_logger.log(tr); 

                if (next_state != current_state.load()) {
                    current_state.store(next_state);
                    brain.learner().on_phase_change((next_state == WorkloadState::READ_HEAVY) ? adaptive::WorkloadPhase::READ_HEAVY : adaptive::WorkloadPhase::WRITE_HEAVY);
                }
            }
        }
    }
};
