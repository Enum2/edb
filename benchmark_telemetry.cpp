#include "StorageManager.h"
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <filesystem>

using namespace std;
using namespace std::chrono;

string format_key(int i) {
    stringstream ss;
    ss << "key_" << setw(7) << setfill('0') << i;
    return ss.str();
}

void run_phase(StorageManager& manager, string phase_type, int duration_s) {
    auto start = steady_clock::now();
    int i = 0;
    while (duration_cast<seconds>(steady_clock::now() - start).count() < duration_s) {
        if (phase_type == "WRITE") {
            manager.Put(format_key(i % 1000000), "phase_update");
        } else {
            manager.Scan(format_key(i % 1000000), format_key((i % 1000000) + 100));
        }
        i++;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

int main() {
    cout << "========================================================\n";
    cout << "📊 TELEMETRY EXHAUST GENERATOR (2-CYCLE FAST RUN)\n";
    cout << "========================================================\n";

    std::filesystem::remove_all("data_rocks");
    std::filesystem::remove_all("data_lmdb");
    
    StorageManager manager; 

    cout << "[SYSTEM] Ingesting initial 1,000,000 keys (Silent Phase)...\n";
    for (int i = 0; i < 1000000; i++) {
        manager.Put(format_key(i), "init_data");
    }

    // Reset the AI's stopwatch so the long ingestion doesn't poison the timings!
    manager.monitor.brain.learner().reset_timers();

    cout << "\n>>> CYCLE 1: Reactive Learning Phase <<<\n";
    cout << "✍️ Running WRITE Phase (15 Seconds)...\n";
    run_phase(manager, "WRITE", 15);

    cout << "📖 Running READ Phase (15 Seconds) - Expecting Reactive Migration...\n";
    run_phase(manager, "READ", 15);

    cout << "\n>>> CYCLE 2: Predictive Pre-Building Phase <<<\n";
    cout << "✍️ Running WRITE Phase (15 Seconds) - Expecting Index Drop & Pre-Build...\n";
    run_phase(manager, "WRITE", 15);

    cout << "📖 Running READ Phase (15 Seconds) - Expecting Zero-Latency Hit...\n";
    run_phase(manager, "READ", 15);

    cout << "\n========================================================\n";
    cout << "✅ TELEMETRY SUCCESSFULLY GENERATED!\n";
    cout << "File saved as: telemetry.csv\n";
    cout << "========================================================\n";

    return 0;
}
