#include "infra/SPSCRingBuffer.h"
#include "infra/ThreadSafeQueue.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace chronobook;

template <typename Fn>
static double seconds(Fn&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

int main() {
    constexpr int N = 1000000;

    const double mutexSec = seconds([&] {
        ThreadSafeQueue<int> q;
        std::thread prod([&] { for (int i = 0; i < N; ++i) q.push(i); q.close(); });
        std::thread cons([&] { while (q.pop()) {} });
        prod.join();
        cons.join();
    });

    const double spscSec = seconds([&] {
        SPSCRingBuffer<int> q(4096);
        std::thread prod([&] {
            for (int i = 0; i < N; ++i) while (!q.tryPush(i)) std::this_thread::yield();
        });
        std::thread cons([&] {
            int v = 0;
            for (int i = 0; i < N; ) if (q.tryPop(v)) ++i; else std::this_thread::yield();
        });
        prod.join();
        cons.join();
    });

    std::printf("queue,messages,seconds,msgs_per_sec\n");
    std::printf("mutex,%d,%.6f,%.0f\n", N, mutexSec, N / mutexSec);
    std::printf("spsc,%d,%.6f,%.0f\n", N, spscSec, N / spscSec);
    return 0;
}
