// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fixed-size buffers for card thumbnails, reused until the pool goes. Freeing
// and reallocating an 80KB thumbnail for every card that scrolls past fragments
// a small heap until no block that size is left; a slot handed back is handed
// out again instead. Slots are allocated the first time each is needed, or all
// at once as one arena, which also keeps them from scattering across the heap.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace helix {

class ThumbnailSlotPool {
  public:
    using AllocFn = void* (*)(size_t);
    using FreeFn = void (*)(void*);

    /// Up to @p max_slots buffers of @p slot_bytes each, from @p alloc: one
    /// allocation each as needed, or with @p arena one allocation for them all
    /// now (ok() says whether it was had).
    ThumbnailSlotPool(size_t slot_bytes, size_t max_slots, AllocFn alloc, FreeFn free,
                      bool arena = false);
    ~ThumbnailSlotPool();
    ThumbnailSlotPool(const ThumbnailSlotPool&) = delete;
    ThumbnailSlotPool& operator=(const ThumbnailSlotPool&) = delete;

    /// A free slot, allocating one while fewer than max_slots exist. nullptr
    /// when every slot is in use or the allocation failed.
    uint8_t* acquire();
    /// Hands @p slot back for reuse. Safe from any thread.
    void release(uint8_t* slot);
    /// Frees every slot handed back, so their memory serves something else;
    /// the pool allocates again as needed. An arena stays whole.
    void trim();

    /// False when the arena could not be allocated: the pool hands out nothing.
    bool ok() const {
        return ok_;
    }
    size_t slot_bytes() const {
        return slot_bytes_;
    }
    /// Slots allocated so far, in use or free.
    size_t allocated() const;
    /// Slots handed out and not yet handed back.
    size_t in_use() const;

  private:
    const size_t slot_bytes_;
    const size_t max_slots_;
    const AllocFn alloc_;
    const FreeFn free_;
    uint8_t* arena_ = nullptr; ///< every slot, when allocated as one block
    bool ok_ = true;
    mutable std::mutex mutex_;
    std::vector<uint8_t*> all_;
    std::vector<uint8_t*> free_list_;
};

} // namespace helix
