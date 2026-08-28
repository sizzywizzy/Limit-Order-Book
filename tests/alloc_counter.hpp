#pragma once

// Global operator new/delete replacement that counts allocations, so the
// claim "zero heap allocation in the steady state" is proved by a number
// rather than asserted (RESULTS.md, "Allocation count in the steady state").
//
// Include from exactly ONE translation unit per binary — replacement
// allocation functions must have a single definition. Counting is disabled
// under AddressSanitizer, which interposes the allocator itself; the
// steady-state test skips its assertion there rather than fight the runtime.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(OBTEST_NO_ALLOC_COUNTER) || defined(__SANITIZE_ADDRESS__)
#define OBTEST_COUNT_ALLOCS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OBTEST_COUNT_ALLOCS 0
#else
#define OBTEST_COUNT_ALLOCS 1
#endif
#else
#define OBTEST_COUNT_ALLOCS 1
#endif

namespace obtest {

inline std::atomic<std::uint64_t> alloc_count{0};

inline constexpr bool alloc_counting_enabled = OBTEST_COUNT_ALLOCS != 0;

inline std::uint64_t allocations() {
    return alloc_count.load(std::memory_order_relaxed);
}

}  // namespace obtest

#if OBTEST_COUNT_ALLOCS

// The replacement operators must stay out of line: inlined into a caller,
// the optimizer sees the malloc/free inside them and pairs them against the
// standard allocation functions, which -Wmismatched-new-delete then flags.
#if defined(__GNUC__) || defined(__clang__)
#define OBTEST_NOINLINE __attribute__((noinline))
#else
#define OBTEST_NOINLINE
#endif

namespace obtest::detail {

inline void* counted_alloc(std::size_t size, std::size_t align) {
    obtest::alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (size == 0) {
        size = 1;
    }
    void* p = align > alignof(std::max_align_t)
#if defined(_WIN32)
                  ? _aligned_malloc(size, align)
#else
                  ? std::aligned_alloc(align, size)
#endif
                  : std::malloc(size);
    return p;
}

inline void counted_free(void* p, std::size_t align) noexcept {
#if defined(_WIN32)
    if (align > alignof(std::max_align_t)) {
        _aligned_free(p);
        return;
    }
#else
    (void)align;
#endif
    std::free(p);
}

}  // namespace obtest::detail

OBTEST_NOINLINE void* operator new(std::size_t size) {
    void* p = obtest::detail::counted_alloc(size, alignof(std::max_align_t));
    if (!p) {
        throw std::bad_alloc{};
    }
    return p;
}
OBTEST_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
OBTEST_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return obtest::detail::counted_alloc(size, alignof(std::max_align_t));
}
OBTEST_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return obtest::detail::counted_alloc(size, alignof(std::max_align_t));
}
OBTEST_NOINLINE void* operator new(std::size_t size, std::align_val_t align) {
    void* p = obtest::detail::counted_alloc(size, static_cast<std::size_t>(align));
    if (!p) {
        throw std::bad_alloc{};
    }
    return p;
}
OBTEST_NOINLINE void* operator new[](std::size_t size, std::align_val_t align) {
    return ::operator new(size, align);
}

OBTEST_NOINLINE void operator delete(void* p) noexcept {
    obtest::detail::counted_free(p, alignof(std::max_align_t));
}
OBTEST_NOINLINE void operator delete[](void* p) noexcept {
    obtest::detail::counted_free(p, alignof(std::max_align_t));
}
OBTEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
OBTEST_NOINLINE void operator delete[](void* p, std::size_t) noexcept { ::operator delete[](p); }
OBTEST_NOINLINE void operator delete(void* p, const std::nothrow_t&) noexcept {
    ::operator delete(p);
}
OBTEST_NOINLINE void operator delete[](void* p, const std::nothrow_t&) noexcept {
    ::operator delete[](p);
}
OBTEST_NOINLINE void operator delete(void* p, std::align_val_t align) noexcept {
    obtest::detail::counted_free(p, static_cast<std::size_t>(align));
}
OBTEST_NOINLINE void operator delete[](void* p, std::align_val_t align) noexcept {
    obtest::detail::counted_free(p, static_cast<std::size_t>(align));
}
OBTEST_NOINLINE void operator delete(void* p, std::size_t, std::align_val_t align) noexcept {
    ::operator delete(p, align);
}
OBTEST_NOINLINE void operator delete[](void* p, std::size_t, std::align_val_t align) noexcept {
    ::operator delete[](p, align);
}

#endif  // OBTEST_COUNT_ALLOCS
