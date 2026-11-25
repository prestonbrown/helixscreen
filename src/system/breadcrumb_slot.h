// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One crumb slot of the crash handler's breadcrumb ring, and the protocol that
// lets any thread write a crumb while the crash handler reads the ring.
//
// A writer takes a slot for one crumb. The slot's `seq` then names that crumb,
// and is odd until the crumb is whole. A reader accepts a slot only when `seq`
// says its crumb is whole and says the same after the slot has been read, so
// it never sees a crumb half written, and two writers never share a slot.
// Every field is accessed through the atomic builtins: the reader runs in a
// signal handler, on a thread that may be interrupted inside a write, and the
// writers run on several threads.
//
// Lock-free, no heap, no syscalls. The read side is async-signal-safe.

#include <cstddef>
#include <cstdint>

namespace helix::crumbs {

constexpr size_t kCrumbCategoryLen = 8;
constexpr size_t kCrumbSubjectLen = 60;
constexpr size_t kCrumbTextWords = (kCrumbCategoryLen + kCrumbSubjectLen) / sizeof(uint32_t);

/// A mask for the 31 bits of a crumb's index that a slot's mark carries. The
/// mark is twice the index plus two or three, so it loses the index's top bit
/// to say whether the crumb is whole.
constexpr uint32_t kCrumbIndexMask = 0x7FFFFFFFu;

/// A crumb's text: the category, then the subject, each null-terminated.
/// Kept as words so it crosses a slot in atomic stores and loads.
struct CrumbText {
    uint32_t words[kCrumbTextWords];
};

inline char* crumb_chars(CrumbText& text) noexcept {
    return reinterpret_cast<char*>(text.words);
}

inline const char* crumb_category(const CrumbText& text) noexcept {
    return reinterpret_cast<const char*>(text.words);
}

inline const char* crumb_subject(const CrumbText& text) noexcept {
    return reinterpret_cast<const char*>(text.words) + kCrumbCategoryLen;
}

// Every access to a slot is an atomic builtin, and the reader runs in a signal
// handler, so the protocol holds only while these are lock-free.
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr),
              "a crumb slot needs lock-free 32-bit atomics");

struct CrumbSlot {
    /// Twice the index of the crumb the slot holds, plus two, once that crumb
    /// is whole; odd from the moment a writer takes the slot until it seals
    /// the crumb. Zero in a slot that has never held one.
    uint32_t seq;
    /// Low 32 bits of ms-since-boot (CLOCK_MONOTONIC).
    uint32_t ts_ms;
    uint32_t text[kCrumbTextWords];
};

/// Takes @p slot for the crumb at @p index of a ring of @p ring_size slots.
/// False while a writer is inside the slot, or when the slot already holds a
/// crumb newer than the one a lap back: the caller then leaves it alone.
inline bool crumb_enter(CrumbSlot& slot, uint32_t index, uint32_t ring_size) noexcept {
    const uint32_t seq = __atomic_load_n(&slot.seq, __ATOMIC_ACQUIRE);
    if ((seq & 1u) != 0u) {
        return false;
    }
    // Zero is a slot that never held a crumb. Otherwise the slot is free for
    // this crumb once what it holds is no newer than the crumb one lap back.
    // That admits an older crumb too, one a writer left behind when it was
    // late or never finished, so a lap that skips a slot costs nothing.
    if (seq != 0u) {
        const uint32_t held = (seq - 2u) / 2u;
        // How far the crumb one lap back is past the one held. A mark carries
        // 31 bits of an index and the index wraps at 2^32, so the distance is
        // read on those bits: up to half the range the held crumb is behind,
        // past half it is ahead.
        const uint32_t behind = (index - ring_size - held) & kCrumbIndexMask;
        if (behind > kCrumbIndexMask / 2u) {
            return false;
        }
    }
    uint32_t free_mark = seq;
    if (!__atomic_compare_exchange_n(&slot.seq, &free_mark, 2u * index + 3u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;
    }
    // The odd mark goes ahead of the crumb's text, so a reader that still
    // sees the old mark has not read the new text.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

/// Writes a crumb into a slot this caller took with crumb_enter(). The crumb
/// is not whole until crumb_seal() runs.
inline void crumb_store(CrumbSlot& slot, const CrumbText& text, uint32_t ts_ms) noexcept {
    __atomic_store_n(&slot.ts_ms, ts_ms, __ATOMIC_RELAXED);
    for (size_t w = 0; w < kCrumbTextWords; ++w) {
        __atomic_store_n(&slot.text[w], text.words[w], __ATOMIC_RELAXED);
    }
}

/// Makes the crumb at @p index in @p slot whole. The mark of index 0x7FFFFFFF
/// wraps to zero, the mark of a slot that never held a crumb, so that one
/// crumb reads as absent and its slot is free for the next.
inline void crumb_seal(CrumbSlot& slot, uint32_t index) noexcept {
    __atomic_store_n(&slot.seq, 2u * index + 2u, __ATOMIC_RELEASE);
}

/// Reads the whole crumb @p slot holds into @p text and @p ts_ms. False when
/// the slot holds none: it was never written, or its writer has not finished,
/// or one began the next crumb of the slot during the read. True, the crumb is
/// one writer's from end to end: no writer touched the slot in between.
inline bool crumb_read(const CrumbSlot& slot, CrumbText& text, uint32_t& ts_ms) noexcept {
    const uint32_t seq = __atomic_load_n(&slot.seq, __ATOMIC_ACQUIRE);
    // Zero is a slot that never held a crumb, and an odd mark a writer inside.
    if (seq == 0u || (seq & 1u) != 0u) {
        return false;
    }
    ts_ms = __atomic_load_n(&slot.ts_ms, __ATOMIC_RELAXED);
    for (size_t w = 0; w < kCrumbTextWords; ++w) {
        text.words[w] = __atomic_load_n(&slot.text[w], __ATOMIC_RELAXED);
    }
    // The text is read before the mark is read again, so a mark that did not
    // change means the text is that crumb's and none of the next one's.
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&slot.seq, __ATOMIC_ACQUIRE) == seq;
}

} // namespace helix::crumbs
