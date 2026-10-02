#pragma once
//
// EngineFactory.h — construct any engine by name with per-run isolated data dirs.
//
// Every engine gets its own wiped data directory keyed by a run tag, so runs are
// process-and-directory isolated (the harness-isolation fix Experiment 1 needs).
//
#include "../Engine.h"
#include "../RocksDBEngine.h"
#include "../LMDBEngine.h"
#include "../AHATreeEngine.h"
#include "../StaticHybridEngine.h"
#include "../LmdbUtil.h"
#include "../ahse/AHSEEngine.h"
#include <memory>
#include <stdexcept>
#include <string>

namespace bench {

inline std::string data_dir(const std::string& tag, const std::string& suffix) {
    return "runs/" + tag + "_" + suffix;
}

inline std::unique_ptr<Engine> make_engine(const std::string& name,
                                           long long total_keys,
                                           const std::string& tag,
                                           const std::string& telemetry_path,
                                           bool verbose = false,
                                           int value_size = 1024) {
    size_t map_size = lmdbutil::mapsize_for(total_keys, value_size);
    if (name == "rocksdb")
        return std::make_unique<RocksDBEngine>(data_dir(tag, "rocks"));
    if (name == "lmdb")
        return std::make_unique<LMDBEngine>(data_dir(tag, "lmdb"), map_size);
    if (name == "aha")
        return std::make_unique<AHATreeEngine>(total_keys, data_dir(tag, "aha"));
    if (name == "static_hybrid")
        return std::make_unique<StaticHybridEngine>(total_keys,
                                                    data_dir(tag, "sh_rocks"),
                                                    data_dir(tag, "sh_lmdb"),
                                                    map_size);
    if (name == "ahse")
        return std::make_unique<AHSEEngine>(total_keys,
                                            data_dir(tag, "ahse_rocks"),
                                            data_dir(tag, "ahse_lmdb"),
                                            telemetry_path, verbose, map_size);
    throw std::runtime_error("unknown engine: " + name);
}

inline bool engine_exists(const std::string& name) {
    return name == "rocksdb" || name == "lmdb" || name == "aha" ||
           name == "static_hybrid" || name == "ahse";
}

} // namespace bench
