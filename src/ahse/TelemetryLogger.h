#pragma once
//
// TelemetryLogger.h (AHSE) — per-tick decision log.
//
// Schema extended to the fields the resubmission plan requires for Experiment 1
// (Workload-A root cause) and Experiment 4 (concurrency): every window now
// records the full control state — phase, successful_migrations, all four
// control flags, is_lmdb_ready, is_migrating — alongside the Q* accounting.
//
// RAM sampling is cross-platform (see Common.h), so peak/current RSS is captured
// on macOS and Linux alike (the old /proc-only path silently logged 0 on macOS).
//
#include "../Common.h"
#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>

struct TelemetryRecord {
    std::string timestamp;
    std::string engine = "ahse";
    std::string state = "WRITE_HEAVY";       // phase (Write-Heavy / Read-Heavy)
    std::string op_type = "write";
    int    dataset_size = 0;

    // Access-pattern signals
    double read_ratio = 0.0;                 // r
    double write_ratio = 0.0;
    double ops_per_sec = 0.0;
    double scan_rate_ema = 0.0;              // q̇ EMA

    // Latency
    double avg_latency_ms = 0.0;
    double p99_latency_ms = 0.0;
    double ram_mb = 0.0;

    // Control state (Experiment 1 / 4 instrumentation)
    int  successful_migrations = 0;   // = gate approvals (per-tick vote tally)
    int  physical_migrations = 0;     // completed LMDB build windows
    bool reactive_fire = false;
    bool predictive_fire = false;
    bool drop_index_fire = false;
    bool is_lmdb_ready = false;
    bool is_migrating = false;

    // Q* accounting
    double expected_q = 0.0;                 // Q_expected
    double breakeven_q_star = 0.0;           // Q*
    double build_cost_ms = 0.0;              // C_build
    double benefit_ms = 0.0;                 // delta = l_base - l_lmdb

    bool migration_triggered = false;
    std::string trigger_decision = "NONE";
    std::string event = "NONE";
};

class TelemetryLogger {
public:
    explicit TelemetryLogger(const std::string& filename = "telemetry.csv") {
        file_.open(filename, std::ios::out | std::ios::trunc);
        if (file_.is_open()) {
            file_ << "Timestamp,Engine,State,OpType,DatasetSize,"
                     "ReadRatio,WriteRatio,OpsPerSec,ScanRateEMA,"
                     "AvgLatency_ms,P99Latency_ms,RamMB,"
                     "GateApprovals,PhysicalMigrations,ReactiveFire,PredictiveFire,DropIndexFire,"
                     "IsLmdbReady,IsMigrating,"
                     "Expected_Q,Breakeven_Q_Star,BuildCost_ms,Benefit_ms,"
                     "MigrationTriggered,TriggerDecision,Event\n";
        }
    }
    ~TelemetryLogger() { if (file_.is_open()) file_.close(); }

    void log(TelemetryRecord& r) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!file_.is_open()) return;
        r.timestamp = timestamp();
        r.ram_mb = mem::current_rss_mb();
        file_ << r.timestamp << ',' << r.engine << ',' << r.state << ',' << r.op_type << ','
              << r.dataset_size << ','
              << r.read_ratio << ',' << r.write_ratio << ',' << r.ops_per_sec << ','
              << r.scan_rate_ema << ','
              << r.avg_latency_ms << ',' << r.p99_latency_ms << ',' << r.ram_mb << ','
              << r.successful_migrations << ',' << r.physical_migrations << ','
              << (r.reactive_fire ? 1 : 0) << ','
              << (r.predictive_fire ? 1 : 0) << ',' << (r.drop_index_fire ? 1 : 0) << ','
              << (r.is_lmdb_ready ? 1 : 0) << ',' << (r.is_migrating ? 1 : 0) << ','
              << r.expected_q << ',' << r.breakeven_q_star << ',' << r.build_cost_ms << ','
              << r.benefit_ms << ',' << (r.migration_triggered ? 1 : 0) << ','
              << r.trigger_decision << ',' << r.event << '\n';
        file_.flush();
    }

private:
    static std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        auto t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&t), "%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    std::ofstream file_;
    std::mutex mu_;
};
