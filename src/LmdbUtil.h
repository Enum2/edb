#pragma once
//
// LmdbUtil.h — small helpers shared by the LMDB-backed engines.
//
#include <lmdb.h>
#include <string>
#include <cstring>
#include <algorithm>
#include <cstdint>

namespace lmdbutil {

// True if the cursor key is lexicographically greater than the inclusive upper
// bound `end` — without allocating a std::string per scanned key (the scan hot
// path). Keys are byte-compared directly against the MDB_val.
inline bool key_gt(const MDB_val& k, const std::string& end) {
    size_t n = std::min(static_cast<size_t>(k.mv_size), end.size());
    int c = std::memcmp(k.mv_data, end.data(), n);
    if (c != 0) return c > 0;
    return k.mv_size > end.size();
}

// Warm-up: sweep the whole B+-tree once so its mmap pages are resident before
// the timed workload. Touching one byte per 512 forces each page in.
inline void lmdb_prewarm(MDB_env* env, MDB_dbi dbi) {
    MDB_txn* txn;
    if (mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn) != 0) return;
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, dbi, &cur) == 0) {
        MDB_val k, v;
        volatile uint64_t sink = 0;
        int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
        while (rc == 0) {
            for (size_t i = 0; i < v.mv_size; i += 512)
                sink += static_cast<unsigned char*>(v.mv_data)[i];
            rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
        }
        (void)sink;
        mdb_cursor_close(cur);
    }
    mdb_txn_abort(txn);
}

// LMDB map size (virtual reservation) sized for `keys` records of ~value_size
// bytes each, with generous headroom (key + B+-tree page overhead, x2 safety).
inline size_t mapsize_for(long long keys, int value_size) {
    size_t per = static_cast<size_t>(value_size) + 160; // value + key + overhead
    size_t est = static_cast<size_t>(keys) * per * 2;
    return est + (static_cast<size_t>(4) << 30); // + 4 GB floor
}

} // namespace lmdbutil
