// Replay demo: generate -> replay (NORMAL for analytics) -> replay (MAX
// for throughput) -> print the microstructure + throughput summary.
#include "feed/FeedGenerator.h"
#include "matching/MatchingEngine.h"
#include "replay/ReplayEngine.h"
#include <cstdio>
#include <cstdlib>
using namespace chronobook;

int main(int argc, char** argv) {
    FeedConfig cfg;
    cfg.numMessages = (argc>=2)? std::strtoull(argv[1],nullptr,10) : 1'000'000;
    cfg.seed        = (argc>=3)? std::strtoull(argv[2],nullptr,10) : 0xC0FFEE;
    auto feed = FeedGenerator(cfg).generate();

    // NORMAL run for analytics
    OrderPool poolN(cfg.numMessages + 16);
    MatchingEngine engN(poolN);
    ReplayEngine repN(engN, poolN);
    auto sN = repN.run(feed, ReplayMode::NORMAL);

    // MAX run for throughput (fresh state)
    OrderPool poolM(cfg.numMessages + 16);
    MatchingEngine engM(poolM);
    ReplayEngine repM(engM, poolM);
    auto sM = repM.run(feed, ReplayMode::MAX, /*warmup=*/cfg.numMessages/20);

    printf("== ChronoBook replay ==\n");
    printf("messages        : %llu  (adds %llu, cancels %llu, modifies %llu)\n",
        (unsigned long long)sN.messages,(unsigned long long)sN.adds,
        (unsigned long long)sN.cancels,(unsigned long long)sN.modifies);
    printf("fills           : %llu   matched volume: %llu   cancel-misses: %llu\n",
        (unsigned long long)sN.fills,(unsigned long long)sN.matchedVolume,
        (unsigned long long)sN.cancelMisses);
    printf("-- microstructure --\n");
    printf("spread     mean %.3f  min %.0f  max %.0f\n",
        repN.spread().stats().mean, repN.spread().stats().min(), repN.spread().stats().max());
    printf("depth(top) mean %.1f  min %.0f  max %.0f\n",
        repN.queueDepth().stats().mean, repN.queueDepth().stats().min(), repN.queueDepth().stats().max());
    printf("imbalance  mean %.4f  sd %.4f  (last %.4f)\n",
        repN.imbalance().stats().mean, repN.imbalance().stats().stddev(), repN.imbalance().current());
    printf("fill rate  window %.3f  cumulative %.3f\n",
        repN.fillRate().windowRate(), repN.fillRate().cumulativeRate());
    printf("-- throughput (MAX mode) --\n");
    printf("%.2f M msgs/sec  (%.3f s for %llu msgs, warmup excluded)\n",
        sM.throughputMsgsPerSec()/1e6, sM.elapsedSeconds, (unsigned long long)sM.messages);
    return 0;
}
