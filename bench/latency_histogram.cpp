#include "feed/FeedGenerator.h"
#include "matching/MatchingEngine.h"
#include "replay/LatencyHistogram.h"

#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#include <pthread.h>
#endif

using namespace chronobook;

static uint64_t rdtscpCycles() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    unsigned int aux = 0;
    return __rdtscp(&aux);
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int aux = 0;
    return __rdtscp(&aux);
#else
    return 0;
#endif
}

static bool pinCurrentThreadToCpu0() noexcept {
#if defined(_WIN32)
    return SetThreadAffinityMask(GetCurrentThread(), 1) != 0;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
#else
    return false;
#endif
}

static void applyMessage(const FeedMessage& m, MatchingEngine& engine, OrderPool& pool) {
    if (!isValidFeedMessage(m)) return;
    if (m.msgType == MsgType::ADD) {
        Order* o = pool.allocate();
        if (!o) return;
        o->orderId = m.orderId;
        o->symbolPacked = m.symbolPacked;
        o->price = m.price;
        o->qty = m.qty;
        o->filledQty = 0;
        o->side = static_cast<Side>(m.side);
        o->type = static_cast<OrderType>(m.orderType);
        engine.processOrder(o, m.sequence);
    } else if (m.msgType == MsgType::CANCEL) {
        engine.cancelOrder(m.orderId);
    } else if (m.msgType == MsgType::MODIFY) {
        engine.modifyOrder(m.orderId, m.price, m.qty, m.sequence);
    }
    engine.drainFills();
}

int main(int argc, char** argv) {
    const size_t n = argc > 1 ? static_cast<size_t>(std::strtoull(argv[1], nullptr, 10)) : 200000;
    const size_t warmup = argc > 2 ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 10)) : 5000;
    const bool pinned = pinCurrentThreadToCpu0();

    FeedConfig cfg;
    cfg.numMessages = n + warmup;
    cfg.seed = 42;
    auto feed = FeedGenerator(cfg).generate();

    OrderPool pool((n + warmup) * 2 + 1024, true);
    MatchingEngine engine(pool);
    LatencyHistogram addHist, cancelHist, modifyHist;

    for (size_t i = 0; i < feed.size(); ++i) {
        const auto& m = feed[i];
        if (i < warmup || rdtscpCycles() == 0) {
            applyMessage(m, engine, pool);
            continue;
        }

        const uint64_t c0 = rdtscpCycles();
        applyMessage(m, engine, pool);
        const uint64_t c1 = rdtscpCycles();
        const uint64_t cycles = c1 - c0;

        if (m.msgType == MsgType::ADD) addHist.record(cycles);
        else if (m.msgType == MsgType::CANCEL) cancelHist.record(cycles);
        else modifyHist.record(cycles);
    }

    const auto add = addHist.summarize();
    const auto cancel = cancelHist.summarize();
    const auto modify = modifyHist.summarize();
    std::printf("method,rdtscp\n");
    std::printf("thread_pinned,%d\n", pinned ? 1 : 0);
    std::printf("huge_pages_requested,%d\n", pool.hugePagesRequested() ? 1 : 0);
    std::printf("huge_pages_backed,%d\n", pool.hugePagesBacked() ? 1 : 0);
    std::printf("op,count,p50_cycles,p99_cycles,p999_cycles,max_cycles\n");
    std::printf("add,%zu,%llu,%llu,%llu,%llu\n", addHist.count(),
                (unsigned long long)add.p50, (unsigned long long)add.p99,
                (unsigned long long)add.p999, (unsigned long long)add.max);
    std::printf("cancel,%zu,%llu,%llu,%llu,%llu\n", cancelHist.count(),
                (unsigned long long)cancel.p50, (unsigned long long)cancel.p99,
                (unsigned long long)cancel.p999, (unsigned long long)cancel.max);
    std::printf("modify,%zu,%llu,%llu,%llu,%llu\n", modifyHist.count(),
                (unsigned long long)modify.p50, (unsigned long long)modify.p99,
                (unsigned long long)modify.p999, (unsigned long long)modify.max);
    return 0;
}
