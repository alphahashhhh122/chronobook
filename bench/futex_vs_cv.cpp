#include "infra/CvSemaphore.h"
#include "infra/FutexSemaphore.h"

#include <chrono>
#include <cstdio>

using namespace chronobook;

template <typename Sem>
static double uncontended(int n) {
    Sem sem(1);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        sem.acquire();
        sem.release();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

int main() {
    constexpr int N = 1000000;
    const double futexSec = uncontended<FutexSemaphore>(N);
    const double cvSec = uncontended<CvSemaphore>(N);
    std::printf("semaphore,iterations,seconds,ops_per_sec\n");
    std::printf("futex_style,%d,%.6f,%.0f\n", N, futexSec, N / futexSec);
    std::printf("condition_variable,%d,%.6f,%.0f\n", N, cvSec, N / cvSec);
    return 0;
}
