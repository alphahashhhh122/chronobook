#include "core/OrderPool.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace chronobook;

int main() {
    constexpr size_t N = 1000000;
    const auto t0 = std::chrono::steady_clock::now();
    OrderPool pool(N, true);
    std::vector<Order*> orders;
    orders.reserve(N);
    for (size_t i = 0; i < N; ++i) orders.push_back(pool.allocate());
    for (auto* o : orders) pool.deallocate(o);
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("allocated,%zu,seconds,%.6f,huge_requested,%d,huge_backed,%d,mmap_backed,%d\n", N,
                std::chrono::duration<double>(t1 - t0).count(),
                pool.hugePagesRequested() ? 1 : 0,
                pool.hugePagesBacked() ? 1 : 0,
                pool.mmapBacked() ? 1 : 0);
    return 0;
}
