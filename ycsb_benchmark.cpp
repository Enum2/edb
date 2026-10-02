#include "StorageManager.h"
#include <rocksdb/db.h>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>
#include <filesystem>
#include <thread>

using namespace std;
using namespace std::chrono;

// Helper to format keys perfectly
string format_key(int i) {
    stringstream ss;
    ss << "user_" << setw(10) << setfill('0') << i;
    return ss.str();
}

enum OpType { READ, UPDATE, SCAN, INSERT };
struct YCSB_Operation {
    OpType type;
    string key_start;
    string key_end; 
    string write_val; 
};

// ========================================================================
// YCSB WORKLOAD GENERATOR
// ========================================================================
vector<YCSB_Operation> generate_ycsb_workload(int num_ops, int max_keys, int read_pct, int update_pct, int scan_pct) {
    vector<YCSB_Operation> workload;
    workload.reserve(num_ops);
    mt19937 rng(42); 
    uniform_int_distribution<int> ratio_dist(1, 100);
    
    // Safety buffer: restrict target keys to max_keys - 1000 so range scans don't overflow bounds
    int printable_limit = max_keys - 1000;
    
    // YCSB traditional Zipfian distribution approximation (80/20 rule)
    uniform_int_distribution<int> hot_keys(1, printable_limit * 0.20); 
    uniform_int_distribution<int> cold_keys(printable_limit * 0.20, printable_limit);
    uniform_int_distribution<int> hot_cold_dice(1, 100);

    for (int i = 0; i < num_ops; i++) {
        YCSB_Operation op;
        
        // 80% of traffic hits 20% of the data
        int k = (hot_cold_dice(rng) <= 80) ? hot_keys(rng) : cold_keys(rng);
        
        int action = ratio_dist(rng);
        if (action <= read_pct) {
            op.type = READ;
            op.key_start = format_key(k);
        } else if (action <= read_pct + update_pct) {
            op.type = UPDATE;
            op.key_start = format_key(k);
            op.write_val = "ycsb_updated_payload_data";
        } else if (action <= read_pct + update_pct + scan_pct) {
            op.type = SCAN;
            op.key_start = format_key(k);
            // 🔥 MODIFIED: Increased scan range from 100 to 1,000 records
            op.key_end = format_key(k + 1000); 
        } else {
            op.type = INSERT;
            op.key_start = format_key(max_keys + i); // New distinct key
            op.write_val = "ycsb_inserted_payload_data";
        }
        workload.push_back(op);
    }
    return workload;
}

// ========================================================================
// BENCHMARK EXECUTION
// ========================================================================
void run_ycsb_workload(string workload_name, const vector<YCSB_Operation>& workload, int total_keys) {
    cout << "\n========================================================\n";
    cout << "▶ RUNNING YCSB " << workload_name << "\n";
    cout << "========================================================\n";

    std::filesystem::remove_all("baseline_rocksdb");
    std::filesystem::remove_all("data_rocks");
    std::filesystem::remove_all("data_lmdb");

    // --- TEST 1: PURE ROCKSDB ---
    rocksdb::DB* raw_db;
    rocksdb::Options opt;
    opt.create_if_missing = true;
    rocksdb::DB::Open(opt, "baseline_rocksdb", &raw_db);

    cout << "Loading " << total_keys << " records for RocksDB...\n";
    for (int i = 0; i < total_keys; i++) {
        raw_db->Put(rocksdb::WriteOptions(), format_key(i), "initial_ycsb_data_payload");
    }

    cout << "Executing RocksDB Workload...\n";
    auto start_rocks = high_resolution_clock::now();
    for (const auto& op : workload) {
        if (op.type == READ) {
            string value;
            raw_db->Get(rocksdb::ReadOptions(), op.key_start, &value);
            volatile size_t s = value.size();
        } else if (op.type == UPDATE || op.type == INSERT) {
            raw_db->Put(rocksdb::WriteOptions(), op.key_start, op.write_val);
        } else if (op.type == SCAN) {
            rocksdb::Iterator* it = raw_db->NewIterator(rocksdb::ReadOptions());
            for (it->Seek(op.key_start); it->Valid() && it->key().compare(op.key_end) <= 0; it->Next()) {
                volatile auto val = it->value();
            }
            delete it;
        }
    }
    auto end_rocks = high_resolution_clock::now();
    double rocks_ms = duration_cast<milliseconds>(end_rocks - start_rocks).count();
    delete raw_db;

    // --- TEST 2: ADAPTIVE ENGINE ---
    StorageManager manager;
    cout << "Loading " << total_keys << " records for Adaptive Engine...\n";
    for (int i = 0; i < total_keys; i++) {
        manager.Put(format_key(i), "initial_ycsb_data_payload");
    }

    cout << "Executing Adaptive Engine Workload...\n";
    auto start_adapt = high_resolution_clock::now();
    for (const auto& op : workload) {
        if (op.type == READ || op.type == SCAN) {
            string end = (op.type == READ) ? op.key_start : op.key_end;
            manager.Scan(op.key_start, end); 
        } else {
            manager.Put(op.key_start, op.write_val);
        }
    }
    auto end_adapt = high_resolution_clock::now();
    double adapt_ms = duration_cast<milliseconds>(end_adapt - start_adapt).count();

    // --- RESULTS ---
    cout << "\n📊 YCSB " << workload_name << " RESULTS\n";
    cout << "RocksDB:         " << rocks_ms << " ms\n";
    cout << "Adaptive Engine: " << adapt_ms << " ms\n";
    
    if (adapt_ms < rocks_ms) {
        cout << "🏆 ADAPTIVE WINS by " << fixed << setprecision(1) << ((rocks_ms - adapt_ms) / rocks_ms) * 100.0 << "%\n";
    } else {
        cout << "🏆 ROCKSDB WINS by " << fixed << setprecision(1) << ((adapt_ms - rocks_ms) / adapt_ms) * 100.0 << "%\n";
    }
}

int main() {
    int dataset_size = 500000; // 500k Keys
    int ops = 200000;          // 200k Operations per test

    cout << "========================================================\n";
    cout << " STANDARD YCSB BENCHMARK SUITE (SCAN RANGE = 1,000)\n";
    cout << "========================================================\n";

    // YCSB Workload A: Update heavy (50% Read, 50% Update)
    auto wl_a = generate_ycsb_workload(ops, dataset_size, 50, 50, 0);
    run_ycsb_workload("Workload A (50/50 Update Heavy)", wl_a, dataset_size);

    // YCSB Workload B: Read mostly (95% Read, 5% Update)
    auto wl_b = generate_ycsb_workload(ops, dataset_size, 95, 5, 0);
    run_ycsb_workload("Workload B (95/5 Read Mostly)", wl_b, dataset_size);

    // YCSB Workload E: Short ranges (95% Scans, 5% Insert)
    auto wl_e = generate_ycsb_workload(ops, dataset_size, 0, 0, 95); 
    run_ycsb_workload("Workload E (95% Range Scans)", wl_e, dataset_size);

    return 0;
}
