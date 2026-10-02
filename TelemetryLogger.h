#pragma once
#include <fstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <unistd.h> 

struct TelemetryRecord {
    std::string timestamp;
    std::string engine = "baseline_rocksdb";
    std::string op_type = "write";
    double avg_latency_ms = 0.0;
    double p99_latency_ms = 0.0; 
    double ops_per_sec = 0.0;
    double ram_mb = 0.0;
    std::string event = "NONE";

    int dataset_size = 0;
    double read_ratio = 0.0;
    double write_ratio = 0.0;
    double expected_q = 0.0;           
    double breakeven_q_star = 0.0;     
    double estimated_cost_ms = 0.0;    
    double estimated_benefit_ms = 0.0;
    bool migration_triggered = false;
    std::string trigger_decision = "NONE";
    std::string actual_phase = "write";
};

class TelemetryLogger {
private:
    std::ofstream file;
    std::mutex mu;

    double get_current_ram_mb() {
        std::ifstream statm("/proc/self/statm");
        if (statm.is_open()) {
            long pages; statm >> pages >> pages; 
            return (pages * sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
        }
        return 0.0;
    }

    std::string get_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

public:
    TelemetryLogger() {
        file.open("telemetry.csv", std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            // 🔥 EXACT MATCH FOR YOUR EXCEL SCRIPT
            file << "Timestamp,Engine,OpType,AvgLatency_ms,P99Latency_ms,OpsPerSec,RamMB,Event,"
                 << "DatasetSize,ReadRatio,WriteRatio,Expected_Q,Breakeven_Q_Star,BuildCost_ms,"
                 << "EstimatedBenefit_ms,MigrationTriggered,TriggerDecision,ActualPhase\n";
        }
    }
    ~TelemetryLogger() { if (file.is_open()) file.close(); }

    void log(TelemetryRecord& rec) {
        std::lock_guard<std::mutex> lock(mu);
        if (!file.is_open()) return;
        rec.timestamp = get_timestamp(); 
        rec.ram_mb = get_current_ram_mb();
        
        file << rec.timestamp << "," << rec.engine << "," << rec.op_type << ","
             << rec.avg_latency_ms << "," << rec.p99_latency_ms << "," << rec.ops_per_sec << ","
             << rec.ram_mb << "," << rec.event << "," << rec.dataset_size << ","
             << rec.read_ratio << "," << rec.write_ratio << "," << rec.expected_q << ","
             << rec.breakeven_q_star << "," << rec.estimated_cost_ms << ","
             << rec.estimated_benefit_ms << "," << (rec.migration_triggered ? 1 : 0) << ","
             << rec.trigger_decision << "," << rec.actual_phase << "\n";
        file.flush(); 
    }
};
