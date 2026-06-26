#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace chronobook {

template <typename T>
class SlabPool {
public:
    explicit SlabPool(size_t capacity, bool requestHugePages = false)
        : m_capacity(capacity), m_hugeRequested(requestHugePages) {
        static_assert(alignof(T) >= alignof(void*),
                      "free-list pointer must fit in each slot");
        m_bytes = capacity * sizeof(T);
        allocateStorage();
        for (size_t i = 0; i < capacity; ++i) {
            pushFree(m_slab + i * sizeof(T));
        }
    }

    ~SlabPool() noexcept { releaseStorage(); }

    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    SlabPool(SlabPool&& other) noexcept
        : m_slab(other.m_slab),
          m_freeHead(other.m_freeHead),
          m_bytes(other.m_bytes),
          m_capacity(other.m_capacity),
          m_allocated(other.m_allocated),
          m_hugeRequested(other.m_hugeRequested),
          m_hugeBacked(other.m_hugeBacked),
          m_mmapBacked(other.m_mmapBacked) {
        other.m_slab = nullptr;
        other.m_freeHead = nullptr;
        other.m_bytes = 0;
        other.m_capacity = 0;
        other.m_allocated = 0;
        other.m_hugeBacked = false;
        other.m_mmapBacked = false;
    }

    SlabPool& operator=(SlabPool&&) = delete;

    template <typename... Args>
    T* allocate(Args&&... args) noexcept {
        if (!m_freeHead) return nullptr;
        std::byte* slot = popFree();
        ++m_allocated;
        return ::new (slot) T(std::forward<Args>(args)...);
    }

    void deallocate(T* obj) noexcept {
        if (!obj) return;
        obj->~T();
        pushFree(reinterpret_cast<std::byte*>(obj));
        assert(m_allocated > 0);
        --m_allocated;
    }

    bool owns(const T* obj) const noexcept {
        const auto* p = reinterpret_cast<const std::byte*>(obj);
        return p >= m_slab && p < (m_slab + m_capacity * sizeof(T)) &&
               ((p - m_slab) % sizeof(T) == 0);
    }

    size_t capacity() const noexcept { return m_capacity; }
    size_t allocatedCount() const noexcept { return m_allocated; }
    size_t freeCount() const noexcept { return m_capacity - m_allocated; }
    bool hugePagesRequested() const noexcept { return m_hugeRequested; }
    bool hugePagesBacked() const noexcept { return m_hugeBacked; }
    bool mmapBacked() const noexcept { return m_mmapBacked; }

private:
    void allocateStorage() {
#if defined(__linux__)
        if (m_hugeRequested) {
            void* p = mmap(nullptr, m_bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
            if (p != MAP_FAILED) {
                m_slab = static_cast<std::byte*>(p);
                m_hugeBacked = true;
                m_mmapBacked = true;
                return;
            }
        }
        void* p = mmap(nullptr, m_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            m_slab = static_cast<std::byte*>(p);
            m_mmapBacked = true;
            return;
        }
#endif
        m_slab = static_cast<std::byte*>(
            ::operator new[](m_bytes, std::align_val_t(alignof(T))));
    }

    void releaseStorage() noexcept {
        if (!m_slab) return;
#if defined(__linux__)
        if (m_mmapBacked) {
            munmap(m_slab, m_bytes);
            return;
        }
#endif
        ::operator delete[](m_slab, std::align_val_t(alignof(T)));
    }

    void pushFree(std::byte* slot) noexcept {
        std::memcpy(slot, &m_freeHead, sizeof(std::byte*));
        m_freeHead = slot;
    }

    std::byte* popFree() noexcept {
        std::byte* slot = m_freeHead;
        std::memcpy(&m_freeHead, slot, sizeof(std::byte*));
        return slot;
    }

    std::byte* m_slab{nullptr};
    std::byte* m_freeHead{nullptr};
    size_t m_bytes{0};
    size_t m_capacity{0};
    size_t m_allocated{0};
    bool m_hugeRequested{false};
    bool m_hugeBacked{false};
    bool m_mmapBacked{false};
};

} // namespace chronobook
