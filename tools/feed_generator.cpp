// Deterministic feed generator CLI.
//   feed_generator <out.bin> [numMessages] [seed]
#include "feed/FeedGenerator.h"
#include "feed/FeedParser.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace chronobook;

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s <out.bin> [numMessages] [seed]\n", argv[0]); return 1; }
    FeedConfig cfg;
    if (argc >= 3) cfg.numMessages = std::strtoull(argv[2], nullptr, 10);
    if (argc >= 4) cfg.seed        = std::strtoull(argv[3], nullptr, 10);
    auto msgs = FeedGenerator(cfg).generate();
    if (!FeedParser::writeFile(argv[1], msgs)) { printf("write failed\n"); return 1; }
    printf("wrote %zu messages (%zu bytes) seed=%llu -> %s\n",
           msgs.size(), msgs.size()*kFeedMessageSize,
           (unsigned long long)cfg.seed, argv[1]);
    return 0;
}
