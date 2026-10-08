// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// reserve() that reports failure instead of aborting. The ESP32 firmware builds
// -fno-exceptions, so a std::string or std::vector allocation that fails calls
// abort(); malloc returns null instead, so these probe with it first. Use them
// for any container whose size a server decides (a file listing, a page of
// history), and take the error path on false.
//
// What this covers is the container's own buffer: the one large contiguous
// block, which is what a fragmented heap fails first. Allocations the elements
// make as they are added (each entry's strings) are separate and small, and can
// still abort when the heap is all but exhausted.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace helix {

/// Tests set this to make every try_reserve() that would allocate fail.
inline std::atomic<bool>& try_reserve_fails_for_test() {
    static std::atomic<bool> fails{false};
    return fails;
}

/// What std::string::reserve(@p bytes) allocates from @p capacity: libstdc++
/// grows to at least twice the old capacity, so 128 KB -> 200 KB takes 256 KB.
inline constexpr size_t reserve_allocation_bytes(size_t capacity, size_t bytes, size_t max_size) {
    if (bytes > capacity && bytes < 2 * capacity) {
        return 2 * capacity < max_size ? 2 * capacity : max_size;
    }
    return bytes;
}

namespace detail {
// Another task can take the block between the probe's free and the
// reserve; a nothrow allocator in the container type would close that window.
inline bool probe_alloc(size_t bytes) {
    if (try_reserve_fails_for_test().load(std::memory_order_relaxed)) {
        return false;
    }
    void* probe = std::malloc(bytes);
    if (!probe) {
        return false;
    }
    std::free(probe);
    return true;
}
} // namespace detail

/// std::string::reserve(@p bytes). False, leaving @p s unchanged, when the
/// allocation cannot be had.
inline bool try_reserve(std::string& s, size_t bytes) {
    if (bytes <= s.capacity()) {
        return true;
    }
    if (bytes > s.max_size() ||
        !detail::probe_alloc(reserve_allocation_bytes(s.capacity(), bytes, s.max_size()) + 1)) {
        return false;
    }
    s.reserve(bytes);
    return true;
}

/// std::vector::reserve(@p count), which allocates exactly @p count elements.
/// False, leaving @p v unchanged, when the allocation cannot be had.
template <class T> bool try_reserve(std::vector<T>& v, size_t count) {
    if (count <= v.capacity()) {
        return true;
    }
    if (count > v.max_size() || !detail::probe_alloc(count * sizeof(T))) {
        return false;
    }
    v.reserve(count);
    return true;
}

} // namespace helix
