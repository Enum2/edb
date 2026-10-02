#include "StorageManager.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <filesystem>

using namespace std;
using namespace std::chrono;

struct TimelineEvent {
    double timestamp_s;   
    string event;        
    string cycle;        
};

vector<TimelineEvent> timeline;
auto t_global_start = steady_clock::now();

void log_event(string ev, string cycle) {
    double t = duration<double>(steady_clock::now() - t_global_start).count();
    timeline.push_back({t, ev, cycle});
    cout << "\n[TIMELINE] ⏱️ " << fixed << setprecision(2) << t << "s : " << ev << " (" << cycle << ")\n";
}

string format_key(int i) {
    stringstream ss;
    ss << "key_" << setw(7) << setfill('0') << i;
    return ss.str();
}

void run_phase(StorageManager& manager, string phase_type, int duration_s, string cycle_name) {
    auto start = steady_clock::now();
    
    bool logged_build_start = false;
    bool logged_build_end = false;
    bool logged_first_read = false;
    
    int i = 0;
    while (duration_cast<seconds>(steady_clock::now() - start).count() < duration_s) {
        if (phase_type == "WRITE") {
            manager.Put(format_key(i % 1000000), "phase_update");
        } else {
            manager.Scan(format_key(i % 1000000), format_key((i % 1000000) + 100));
        }
        i++;

        if (!logged_build_start && manager.is_currently_migrating()) {
            if (phase_type == "WRITE") log_event("PREDICTIVE_BUILD_START", cycle_name);
            else log_event("BUILD_START", cycle_name);
            logged_build_start = true;
        }
        
        if (logged_build_start && !logged_build_end && manager.is_lmdb_active()) {
            if (phase_type == "WRITE") log_event("PREDICTIVE_BUILD_END", cycle_name);
            else log_event("BUILD_END", cycle_name);
            logged_build_end = true;
        }
        
        if (phase_type == "READ" && manager.is_lmdb_active() && !logged_first_read) {
            log_event("FIRST_LMDB_READ", cycle_name);
            logged_first_read = true;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

int main() {
    cout << "========================================================\n";
    cout << "🧠 PHASE LEARNER: PREDICTIVE MIGRATION TIMELINE\n";
    cout << "========================================================\n";

    std::filesystem::remove_all("baseline_rocksdb");
    std::filesystem::remove_all("data_rocks");
    std::filesystem::remove_all("data_lmdb");

    StorageManager manager;
    
    cout << "[SYSTEM] Ingesting 1,000,000 keys to calibrate build times...\n";
    for(int i=0; i<1000000; i++) {
        manager.Put(format_key(i), "init");
    }

    int PHASE_DURATION = 45; 
    t_global_start = steady_clock::now(); 

    cout << "\n>>> STARTING CYCLE 1 (AI is untrained, uses standard reactive triggers)...\n";
    
    log_event("WRITE_PHASE_START", "cycle_1");
    run_phase(manager, "WRITE", PHASE_DURATION, "cycle_1");

    log_event("READ_PHASE_START", "cycle_1");
    run_phase(manager, "READ", PHASE_DURATION, "cycle_1");

    cout << "\n>>> STARTING CYCLE 2 (AI has learned the pattern, expects pre-build)...\n";
    
    log_event("WRITE_PHASE_START", "cycle_2");
    run_phase(manager, "WRITE", PHASE_DURATION, "cycle_2");

    log_event("READ_PHASE_START", "cycle_2");
    run_phase(manager, "READ", PHASE_DURATION, "cycle_2");

    cout << "\n========================================================\n";
    cout << "📊 TIMELINE METRICS ANALYSIS\n";
    cout << "========================================================\n";

    double c1_read_start = 0, c1_build_end = 0;
    double c2_read_start = 0, c2_build_end = 0, c2_build_start = 0;

    for (const auto& ev : timeline) {
        if (ev.cycle == "cycle_1") {
            if (ev.event == "READ_PHASE_START") c1_read_start = ev.timestamp_s;
            if (ev.event == "BUILD_END") c1_build_end = ev.timestamp_s;
        } else if (ev.cycle == "cycle_2") {
            if (ev.event == "PREDICTIVE_BUILD_START" || ev.event == "BUILD_START") c2_build_start = ev.timestamp_s;
            if (ev.event == "READ_PHASE_START") c2_read_start = ev.timestamp_s;
            if (ev.event == "PREDICTIVE_BUILD_END" || ev.event == "BUILD_END") c2_build_end = ev.timestamp_s;
        }
    }

    double c1_offset = c1_build_end - c1_read_start;
    cout << "[CYCLE 1] Reactive Migration Offset:  " << fixed << setprecision(2) << c1_offset << " seconds\n";
    cout << "          (Live users experienced RocksDB slowness for " << c1_offset << "s into the read phase)\n\n";

    if (c2_build_start > 0 && c2_build_start < c2_read_start) {
        double preemptive_time = c2_read_start - c2_build_start;
        cout << "[CYCLE 2] Predictive Migration Triggered EARLY!\n";
        cout << "          The Brain fired PREDICTIVE_BUILD_START " << preemptive_time << " seconds before the Read Phase began.\n";
        
        if (c2_build_end <= c2_read_start) {
            cout << "          -> LMDB was 100% READY at t=0.0 of the Read Phase.\n";
            cout << "          -> Migration Start Offset: 0.0 seconds! (ZERO LATENCY READS)\n";
        } else {
            cout << "          -> LMDB finished building slightly into the read phase.\n";
        }
    } else if (c2_build_start >= c2_read_start) {
        cout << "[CYCLE 2] Predictive Trigger Failed. Engine fell back to Reactive Migration.\n";
    }

    cout << "========================================================\n";
    return 0;
}
