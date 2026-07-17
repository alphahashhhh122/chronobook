// sharded_throughput.cpp
//
// Measures matching throughput scaling across shard counts. Generates a multi-
// symbol feed, then runs:
//   1. Baseline: single-threaded per-symbol matching (no SPSC overhead)
//   2. ShardedEngine with 1, 2, and 4 shards
//
// Use the output as a workload-specific smoke benchmark; scaling depends on
// hardware, symbol distribution, compiler, and thread scheduling.

#include "feed/FeedGenerator.h"
#include "matching/MatchingEngine.h"
#include "matching/ShardedEngine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>

using namespace chronobook;

// Assign distinct symbols to a feed so messages distribute across shards.
// Cancels and modifies inherit the symbol of their target ADD, ensuring they
// route to the same shard.
static void assignSymbols(std::vector<FeedMessage>& feed,
                          size_t numSymbols, uint64_t seed = 99) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> symDist(1, numSymbols);
    std::unordered_map<uint64_t, uint64_t> orderToSymbol;
    orderToSymbol.reserve(feed.size());

    for (auto& msg : feed) {
        if (msg.msgType == MsgType::ADD) {
            const uint64_t sym = symDist(rng);
            msg.symbolPacked = sym;
            orderToSymbol[msg.orderId] = sym;
        } else {
            auto it = orderToSymbol.find(msg.orderId);
            if (it != orderToSymbol.end())
                msg.symbolPacked = it->second;
        }
    }
}

// Baseline: single-threaded, per-symbol matching with no SPSC ring overhead.
static double runBaseline(const std::vector<FeedMessage>& feed, size_t poolCap) {
    OrderPool pool(poolCap);
    std::unordered_map<uint64_t, std::unique_ptr<MatchingEngine>> engines;
    std::vector<Fill> scratch;
    scratch.reserve(1u << 16);

    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& msg : feed) {
        if (!isValidFeedMessage(msg)) continue;
        auto it = engines.find(msg.symbolPacked);
        if (it == engines.end()) {
            auto [ins, ok] = engines.emplace(
                msg.symbolPacked,
                std::make_unique<MatchingEngine>(pool));
            (void)ok;
            it = ins;
        }
        MatchingEngine& engine = *it->second;

        if (msg.msgType == MsgType::ADD) {
            Order* o = pool.allocate();
            if (!o) continue;
            o->orderId      = msg.orderId;
            o->symbolPacked = msg.symbolPacked;
            o->price        = msg.price;
            o->qty          = msg.qty;
            o->filledQty    = 0;
            o->side         = static_cast<Side>(msg.side);
            o->type         = static_cast<OrderType>(msg.orderType);
            engine.processOrder(o, msg.sequence);
        } else if (msg.msgType == MsgType::CANCEL) {
            engine.cancelOrder(msg.orderId);
        } else {
            engine.modifyOrder(msg.orderId, msg.price, msg.qty, msg.sequence);
        }
        engine.drainFillsInto(scratch);
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

// Sharded: N worker threads, messages routed by symbol hash.
static double runSharded(const std::vector<FeedMessage>& feed,
                         size_t numShards, size_t poolCapPerShard,
                         bool pin) {
    ShardedEngine engine(numShards, poolCapPerShard, 1u << 16);
    engine.start(pin);

    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& msg : feed) {
        while (!engine.route(msg))
            std::this_thread::yield();
    }
    engine.stop();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

int main(int argc, char** argv) {
    const size_t numMessages = argc > 1
        ? static_cast<size_t>(std::strtoull(argv[1], nullptr, 10))
        : 1'000'000;
    const size_t numSymbols = argc > 2
        ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 10))
        : 64;
    const bool pin = argc > 3 && std::string(argv[3]) == "pin";

    FeedConfig cfg;
    cfg.numMessages = numMessages;
    cfg.seed = 42;
    auto feed = FeedGenerator(cfg).generate();
    assignSymbols(feed, numSymbols);

    const size_t poolCap = numMessages * 2 + 4096;

    std::printf("config: %zu messages, %zu symbols, pin=%d\n",
                numMessages, numSymbols, pin ? 1 : 0);
    std::printf("%-12s %12s %14s %8s\n",
                "mode", "seconds", "msgs/sec", "speedup");

    const double baseSec = runBaseline(feed, poolCap);
    const double baseMps = static_cast<double>(numMessages) / baseSec;
    std::printf("%-12s %12.4f %14.0f %8s\n",
                "baseline", baseSec, baseMps, "1.00x");

    for (size_t shards : {1, 2, 4}) {
        const size_t perShardPool = poolCap / shards + 4096;
        const double sec = runSharded(feed, shards, perShardPool, pin);
        const double mps = static_cast<double>(numMessages) / sec;
        const double speedup = baseSec / sec;
        std::printf("%-12s %12.4f %14.0f %7.2fx\n",
                    (std::to_string(shards) + " shard" +
                     (shards > 1 ? "s" : "")).c_str(),
                    sec, mps, speedup);
    }
    return 0;
}
