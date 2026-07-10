#include "feed/FeedParser.h"
#include <fstream>

namespace chronobook {

std::vector<FeedMessage> FeedParser::parseBuffer(const std::byte* data, size_t len,
                                                 size_t* bytesConsumed) {
    std::vector<FeedMessage> out;
    const size_t n = len / kFeedMessageSize;          // whole messages only
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(decodeMessage(data + i * kFeedMessageSize));
    if (bytesConsumed) *bytesConsumed = n * kFeedMessageSize;  // leave partial frame
    return out;
}

std::vector<std::byte> FeedParser::encodeAll(const std::vector<FeedMessage>& msgs) {
    std::vector<std::byte> buf(msgs.size() * kFeedMessageSize);
    for (size_t i = 0; i < msgs.size(); ++i)
        encodeMessage(msgs[i], buf.data() + i * kFeedMessageSize);
    return buf;
}

bool FeedParser::writeFile(const std::string& path, const std::vector<FeedMessage>& msgs) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    auto buf = encodeAll(msgs);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return static_cast<bool>(f);
}

std::optional<std::vector<FeedMessage>> FeedParser::readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const std::streamsize sz = f.tellg();
    if (sz < 0) return std::nullopt;
    if (static_cast<size_t>(sz) % kFeedMessageSize != 0) return std::nullopt;
    f.seekg(0);
    std::vector<std::byte> buf(static_cast<size_t>(sz));
    if (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()), sz)) return std::nullopt;
    return parseBuffer(buf.data(), buf.size());
}

} // namespace chronobook
