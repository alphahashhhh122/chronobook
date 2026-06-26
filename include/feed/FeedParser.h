#pragma once
// FeedParser.h
//
// Reads a length-implicit stream of fixed-size FeedMessages from a byte buffer
// or a file. Fixed width => framing is trivial: message N lives at byte offset
// N * kFeedMessageSize. A short trailing fragment is reported, not guessed at.
#include "feed/BinaryProtocol.h"
#include <vector>
#include <string>
#include <cstddef>
#include <optional>

namespace chronobook {

class FeedParser {
public:
    // Parse an in-memory buffer. Returns one FeedMessage per kFeedMessageSize
    // bytes. If `bytesConsumed` is provided, it receives the number of fully
    // parsed bytes (a trailing partial frame is left unconsumed - important for
    // streaming/socket reads where a message can span two reads).
    static std::vector<FeedMessage> parseBuffer(const std::byte* data, size_t len,
                                                size_t* bytesConsumed = nullptr);

    // Serialize messages to a flat byte buffer (used by the generator/tests).
    static std::vector<std::byte> encodeAll(const std::vector<FeedMessage>& msgs);

    // File round-trip helpers (binary mode).
    static bool writeFile(const std::string& path, const std::vector<FeedMessage>& msgs);
    static std::optional<std::vector<FeedMessage>> readFile(const std::string& path);
};

} // namespace chronobook
