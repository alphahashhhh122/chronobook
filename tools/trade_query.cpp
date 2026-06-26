#include "store/TradeStore.h"

#include <cstdio>
#include <cstdlib>

using namespace chronobook;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: trade_query <db-path> [begin-ts] [end-ts]\n");
        return 2;
    }
    const uint64_t begin = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
    const uint64_t end = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : UINT64_MAX;
    TradeStore store(argv[1]);
    std::printf("count=%llu\n", (unsigned long long)store.tradeCount());
    std::printf("vwap=%.6f\n", store.vwap(begin, end));
    std::printf("plan:\n%s", store.explainPlan().c_str());
    return 0;
}
