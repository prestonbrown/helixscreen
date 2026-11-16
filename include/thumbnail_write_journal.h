// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace helix {

/**
 * @file thumbnail_write_journal.h
 * @brief One-way write notifications from ThumbnailProcessor to ThumbnailCache
 *
 * ThumbnailCache keeps an in-memory index of its directory so eviction does not
 * stat every file on every fetch (prestonbrown/helixscreen#1207). The index is
 * only honest if it learns about every byte in that directory — and
 * ThumbnailProcessor writes the pre-scaled `.bin` files there, holding no
 * reference to the cache and reporting to nobody.
 *
 * That is not a small leak. On the card path every thumbnail becomes a `.bin`
 * several times the size of its source PNG, so an index blind to them
 * under-counts by most of the cache. An under-counting cache reads as "well
 * under the limit" and stops evicting entirely — the same silent failure as a
 * directory scan cut short by a bad entry.
 *
 * This class is the seam. The processor drops a path in; the cache picks it up
 * on its next accounting pass and pays one `stat` for it, rather than a walk of
 * the whole directory.
 *
 * ## Why a journal rather than a direct call
 *
 * It is deliberately dumb: it records strings under its own mutex and never
 * calls back into anything. That mutex is therefore a leaf — it can never
 * participate in a cycle with `ThumbnailCache::mutex_` or
 * `ThumbnailProcessor::mutex_`, both of which are held on multiple threads.
 * A processor worker calling `ThumbnailCache` directly would have to take the
 * cache's eviction lock from inside a pool task, which is exactly the kind of
 * cross-subsystem lock order this code has already been bitten by.
 *
 * Lifetime is handled by the cache owning the journal via `shared_ptr` while
 * the processor holds only a `weak_ptr`, so a destroyed cache leaves no
 * dangling reference and in-flight writes simply find nobody listening.
 *
 * Thread-safe: every method may be called from any thread.
 */
class ThumbnailWriteJournal {
  public:
    /**
     * @brief Bound on unread notifications
     *
     * Past this the journal stops recording individual paths and latches an
     * overflow flag, which the reader answers with a full rescan. Without the
     * bound, a cache that is constructed and then never queried would let the
     * pending list grow for the life of the process.
     */
    static constexpr size_t kMaxPending = 256;

    /**
     * @brief Record that @p path was written
     *
     * @param path Absolute filesystem path of the file just written
     */
    void note_write(const std::string& path);

    /**
     * @brief Take everything recorded since the last drain
     *
     * @param overflowed_out Receives true if entries were dropped since the
     *        last drain, meaning the returned list is incomplete and the caller
     *        must fall back to a full rescan. May be nullptr.
     * @return Paths written since the last drain, in write order
     */
    std::vector<std::string> drain(bool* overflowed_out);

    /**
     * @brief Discard everything pending
     *
     * For a reader that has just rebuilt its state from the filesystem: any
     * queued notification is already reflected in that rebuild.
     */
    void reset();

  private:
    std::mutex mutex_;
    std::vector<std::string> pending_;
    bool overflowed_ = false;
};

} // namespace helix
