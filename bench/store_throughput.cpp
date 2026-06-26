#include "store/TradeStore.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace chronobook;

int main() {
    const char* db = "chronobook_store_bench.db";
    std::remove(db);
    std::remove("chronobook_store_bench.db-wal");
    std::remove("chronobook_store_bench.db-shm");

    std::vector<Fill> fills;
    fills.reserve(100000);
    for (uint64_t i = 0; i < 100000; ++i) {
        fills.push_back(Fill{i + 1, i + 1000001, 10000 + static_cast<uint32_t>(i % 100),
                             1 + static_cast<uint32_t>(i % 10), i});
    }

    TradeStore store(db);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < fills.size(); i += 1000) {
        const auto end = std::min(i + 1000, fills.size());
        store.insertBatch(std::vector<Fill>(fills.begin() + i, fills.begin() + end));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("rows,seconds,rows_per_sec,count,vwap\n");
    std::printf("%zu,%.6f,%.0f,%llu,%.2f\n", fills.size(), sec, fills.size() / sec,
                (unsigned long long)store.tradeCount(), store.vwap(0, fills.size()));
    return 0;
}
