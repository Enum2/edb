#pragma once
//
// Common.h — shared utilities for the AHSE benchmark suite.
//
// Provides:
//   * kf::make_key / kf::key_to_int : one canonical key format used by every
//     engine and every driver. This removes the old "user_" vs "key_" mismatch
//     that routed every AHA key to segment 0.
//   * mem::current_rss_mb / mem::peak_rss_mb : cross-platform resident-set
//     sampling (mach on macOS, /proc on Linux) so peak-RAM works on both the
//     dev machine and the paper's Fedora box.
//   * timing helpers.
//
#include <string>
#include <cstdio>
#include <cstdint>
#include <chrono>

#if defined(__APPLE__)
  #include <mach/mach.h>
#elif defined(__linux__)
  #include <fstream>
  #include <unistd.h>
#endif

namespace kf {

// Canonical key: "user_" + 10 zero-padded digits, e.g. user_0000001234.
inline std::string make_key(long long i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "user_%010lld", i);
    return std::string(buf, 15); // 5 ("user_") + 10 digits
}

// A value payload of `size` bytes (YCSB records are ~1 KB). Content is a fixed
// fill so it compresses/behaves consistently across engines and runs.
inline std::string make_value(int size) {
    if (size < 1) size = 1;
    return std::string((size_t)size, 'v');
}

// Parse the integer id back out of a canonical key. Returns -1 if the key is
// not in canonical form (e.g. an out-of-band key). Used only for AHA segment
// routing; correctness of routing must never crash on a malformed key.
inline long long key_to_int(const std::string& key) {
    if (key.size() < 6 || key.compare(0, 5, "user_") != 0) return -1;
    long long v = 0;
    for (size_t i = 5; i < key.size(); ++i) {
        char c = key[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    return v;
}

} // namespace kf

namespace mem {

// Current resident set size in MB.
inline double current_rss_mb() {
#if defined(__APPLE__)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        long pages = 0;
        statm >> pages >> pages; // second field = resident pages
        return (pages * sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    return 0.0;
#endif
}

// Peak (high-water) resident set size in MB.
inline double peak_rss_mb() {
#if defined(__APPLE__)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size_max / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmHWM:") == 0) {
            long kb = 0;
            std::sscanf(line.c_str() + 6, "%ld", &kb);
            return kb / 1024.0;
        }
    }
    return current_rss_mb();
#else
    return 0.0;
#endif
}

} // namespace mem

namespace timing {

using clock = std::chrono::high_resolution_clock;

inline double ms_since(clock::time_point start) {
    auto now = clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count() / 1e6;
}

} // namespace timing
