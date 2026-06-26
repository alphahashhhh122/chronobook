#pragma once

namespace chronobook {

#if defined(__GNUC__) || defined(__clang__)
#define CB_LIKELY(x) (__builtin_expect(!!(x), 1))
#define CB_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define CB_LIKELY(x) (x)
#define CB_UNLIKELY(x) (x)
#endif

inline void prefetchRead(const void* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr, 0, 1);
#else
    (void)ptr;
#endif
}

} // namespace chronobook
