#pragma once

#include "matching/MatchingEngine.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace chronobook {

struct FillRecord {
    uint64_t buyOrderId{0};
    uint64_t sellOrderId{0};
    uint32_t price{0};
    uint32_t qty{0};
    uint64_t timestamp{0};
};

static_assert(sizeof(FillRecord) == 32, "FillRecord is a fixed 32-byte record");

class FillJournal {
public:
    explicit FillJournal(std::string path, size_t capacityRecords = 1u << 20)
        : m_path(std::move(path)), m_capacity(capacityRecords) {
        if (m_capacity == 0) throw std::runtime_error("fill journal capacity is zero");
        try {
            openMapping();
            initializeOrValidateHeader();
        } catch (...) {
            closeMapping();
            throw;
        }
    }

    ~FillJournal() noexcept { closeMapping(); }

    FillJournal(const FillJournal&) = delete;
    FillJournal& operator=(const FillJournal&) = delete;

    void append(const Fill& fill) {
        if (m_header->records >= m_header->capacity)
            throw std::runtime_error("fill journal full: " + m_path);
        const FillRecord r{fill.buyOrderId, fill.sellOrderId,
                           fill.price, fill.qty, fill.timestamp};
        m_records[m_header->records++] = r;
    }

    void appendBatch(const std::vector<Fill>& fills) {
        for (const auto& fill : fills) append(fill);
        flush();
    }

    void flush() {
#if defined(_WIN32)
        if (!FlushViewOfFile(m_base, m_fileBytes))
            throw std::runtime_error("failed to flush fill journal mapping");
        if (!FlushFileBuffers(m_file))
            throw std::runtime_error("failed to flush fill journal file");
#else
        if (msync(m_base, m_fileBytes, MS_SYNC) != 0)
            throw std::runtime_error("failed to flush fill journal mapping");
#endif
    }

    size_t recordsWritten() const noexcept {
        return static_cast<size_t>(m_header ? m_header->records : 0);
    }
    size_t capacity() const noexcept { return m_capacity; }
    const std::string& path() const noexcept { return m_path; }

    static std::vector<FillRecord> replay(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("failed to open fill journal for replay: " + path);

        Header h;
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!in) throw std::runtime_error("truncated fill journal header: " + path);
        if (h.magic != kMagic) throw std::runtime_error("invalid fill journal magic: " + path);
        if (h.records > h.capacity) throw std::runtime_error("invalid fill journal record count: " + path);

        std::vector<FillRecord> out(static_cast<size_t>(h.records));
        in.read(reinterpret_cast<char*>(out.data()),
                static_cast<std::streamsize>(out.size() * sizeof(FillRecord)));
        if (!in && !out.empty()) throw std::runtime_error("truncated fill journal record");
        return out;
    }

private:
    struct Header {
        uint64_t magic{0};
        uint64_t records{0};
        uint64_t capacity{0};
    };

    static constexpr uint64_t kMagic = 0x314a46424f4e4843ULL; // "CHNOBFJ1" little-endian marker

    void openMapping() {
        m_fileBytes = sizeof(Header) + m_capacity * sizeof(FillRecord);
#if defined(_WIN32)
        m_file = CreateFileA(m_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("failed to open fill journal: " + m_path);

        LARGE_INTEGER existing;
        if (!GetFileSizeEx(m_file, &existing))
            throw std::runtime_error("failed to stat fill journal: " + m_path);
        m_newFile = existing.QuadPart == 0;

        LARGE_INTEGER size;
        size.QuadPart = static_cast<LONGLONG>(m_fileBytes);
        if (existing.QuadPart < size.QuadPart &&
            (!SetFilePointerEx(m_file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(m_file)))
            throw std::runtime_error("failed to size fill journal: " + m_path);

        const auto bytes = static_cast<uint64_t>(m_fileBytes);
        m_mapping = CreateFileMappingA(m_file, nullptr, PAGE_READWRITE,
                                       static_cast<DWORD>(bytes >> 32),
                                       static_cast<DWORD>(bytes & 0xffffffffu),
                                       nullptr);
        if (!m_mapping) throw std::runtime_error("failed to map fill journal: " + m_path);
        m_base = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, m_fileBytes);
        if (!m_base) throw std::runtime_error("failed to view fill journal: " + m_path);
#else
        m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (m_fd < 0) throw std::runtime_error("failed to open fill journal: " + m_path);

        struct stat st;
        if (fstat(m_fd, &st) != 0) throw std::runtime_error("failed to stat fill journal: " + m_path);
        m_newFile = st.st_size == 0;
        if (st.st_size < static_cast<off_t>(m_fileBytes) &&
            ftruncate(m_fd, static_cast<off_t>(m_fileBytes)) != 0)
            throw std::runtime_error("failed to size fill journal: " + m_path);
        m_base = mmap(nullptr, m_fileBytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (m_base == MAP_FAILED) {
            m_base = nullptr;
            throw std::runtime_error("failed to mmap fill journal: " + m_path);
        }
#endif
        m_header = static_cast<Header*>(m_base);
        m_records = reinterpret_cast<FillRecord*>(static_cast<char*>(m_base) + sizeof(Header));
    }

    void initializeOrValidateHeader() {
        if (m_newFile || m_header->magic != kMagic) {
            std::memset(m_base, 0, m_fileBytes);
            m_header->magic = kMagic;
            m_header->records = 0;
            m_header->capacity = static_cast<uint64_t>(m_capacity);
            return;
        }
        if (m_header->capacity != m_capacity)
            throw std::runtime_error("fill journal capacity mismatch: " + m_path);
        if (m_header->records > m_header->capacity)
            throw std::runtime_error("invalid fill journal record count: " + m_path);
    }

    void closeMapping() noexcept {
        if (m_base) {
#if defined(_WIN32)
            FlushViewOfFile(m_base, m_fileBytes);
            UnmapViewOfFile(m_base);
#else
            msync(m_base, m_fileBytes, MS_SYNC);
            munmap(m_base, m_fileBytes);
#endif
            m_base = nullptr;
        }
#if defined(_WIN32)
        if (m_mapping) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
        if (m_file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_file);
            CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
#endif
        m_header = nullptr;
        m_records = nullptr;
    }

    std::string m_path;
    size_t m_capacity{0};
    size_t m_fileBytes{0};
    bool m_newFile{false};
    void* m_base{nullptr};
    Header* m_header{nullptr};
    FillRecord* m_records{nullptr};

#if defined(_WIN32)
    HANDLE m_file{INVALID_HANDLE_VALUE};
    HANDLE m_mapping{nullptr};
#else
    int m_fd{-1};
#endif
};

} // namespace chronobook
