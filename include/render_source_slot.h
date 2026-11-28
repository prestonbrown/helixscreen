// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>

namespace helix::audio {

/// One render source shared between a sound backend's render thread and its
/// owner.
///
/// The owner's contract with clear(): when it returns, no call uses the source
/// and the copy the last call ran on is destroyed, so the owner may free what
/// the source captured.
///
/// call() copies the source and counts itself in flight under one lock, then
/// runs the copy with no lock held. A mutex held across the call leaves a
/// waiting clear() only the moments between two calls to be scheduled in, and
/// a render loop on a device that never blocks keeps winning that race
/// (prestonbrown/helixscreen#2161).
class RenderSourceSlot {
  public:
    using Source = std::function<void(float*, size_t, int)>;

    /// Replaces the source. It does not wait for a call in flight, which runs
    /// on its own copy: clear() is what waits.
    void set(Source source);

    /// Ends the source and returns once no call uses it. Asked from inside a
    /// call, on the thread running it, the wait could never return: the source
    /// still ends, and the call in flight is the last.
    void clear();

    /// Runs the source on a copy. False when no source is set.
    bool call(float* out, size_t frames, int sample_rate);

    /// Whether call() would find a source.
    [[nodiscard]] bool empty() const;

  private:
    struct CallEnd;

    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;
    Source source_;
    int calls_in_flight_ = 0;
};

} // namespace helix::audio
