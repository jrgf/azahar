// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

// Switch-only global allocation tracker. Overrides operator new/delete so we
// can see live allocation count grow over time — the cheapest way to detect
// per-frame heap leaks before std::bad_alloc fires. No per-block header, so
// we can't track byte totals; allocation COUNT alone catches the common case
// where some subsystem leaks one-object-per-frame.

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <new>

namespace {
std::atomic<std::int64_t> g_live_count{0};
std::atomic<std::int64_t> g_peak_count{0};
std::atomic<std::uint64_t> g_total_news{0};
std::atomic<std::uint64_t> g_total_deletes{0};

inline void OnAlloc() {
    const auto live = g_live_count.fetch_add(1, std::memory_order_relaxed) + 1;
    g_total_news.fetch_add(1, std::memory_order_relaxed);
    auto peak = g_peak_count.load(std::memory_order_relaxed);
    while (live > peak &&
           !g_peak_count.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
}

inline void OnFree() {
    g_live_count.fetch_sub(1, std::memory_order_relaxed);
    g_total_deletes.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

namespace Azahar::Switch {

void QueryHeapTracker(std::int64_t* live_count, std::int64_t* peak_count,
                      std::uint64_t* total_news, std::uint64_t* total_deletes) {
    if (live_count != nullptr) {
        *live_count = g_live_count.load(std::memory_order_relaxed);
    }
    if (peak_count != nullptr) {
        *peak_count = g_peak_count.load(std::memory_order_relaxed);
    }
    if (total_news != nullptr) {
        *total_news = g_total_news.load(std::memory_order_relaxed);
    }
    if (total_deletes != nullptr) {
        *total_deletes = g_total_deletes.load(std::memory_order_relaxed);
    }
}

} // namespace Azahar::Switch

// Global operator new/delete overrides. Forward to malloc/free; sized and
// array variants funnel through the same accounting hooks.

void* operator new(std::size_t size) {
    void* p = std::malloc(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    OnAlloc();
    return p;
}

void* operator new[](std::size_t size) {
    void* p = std::malloc(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    OnAlloc();
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size);
    if (p != nullptr) {
        OnAlloc();
    }
    return p;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size);
    if (p != nullptr) {
        OnAlloc();
    }
    return p;
}

void operator delete(void* p) noexcept {
    if (p != nullptr) {
        OnFree();
        std::free(p);
    }
}

void operator delete[](void* p) noexcept {
    if (p != nullptr) {
        OnFree();
        std::free(p);
    }
}

void operator delete(void* p, std::size_t) noexcept {
    if (p != nullptr) {
        OnFree();
        std::free(p);
    }
}

void operator delete[](void* p, std::size_t) noexcept {
    if (p != nullptr) {
        OnFree();
        std::free(p);
    }
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (p != nullptr) {
        OnFree();
        std::free(p);
    }
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    if (p != nullptr) {
        OnFree();
        std::free(p);
    }
}
