// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace helix {

/**
 * @brief Reassembles Klipper's 0x03-terminated JSON frames from a byte stream
 *
 * Klipper's webhooks socket does not use newline framing. Every request and
 * every response is a JSON document followed by a single 0x03 byte.
 *
 * Measured on the reference printer: the subscribe acknowledgement is 88 bytes
 * and each accelerometer batch is 13-15 KB, against libhv's 8 KB default read
 * buffer. A batch frame spanning multiple reads is the normal case, so
 * reassembly is the decoder's main job rather than an edge case.
 *
 * Pure: no sockets, no JSON parsing, no threads. The frame handed to the
 * callback is a view into internal storage and is invalid once the callback
 * returns - copy it if you need to keep it.
 */
class KlippyFrameDecoder {
  public:
    /// Klipper's frame terminator.
    static constexpr char FRAME_TERMINATOR = '\x03';

    /// Ceiling on unterminated buffered bytes. A peer that never terminates a
    /// frame must not be able to exhaust memory on a 512 MB printer board.
    /// Four times the largest observed frame, rounded up generously.
    static constexpr size_t MAX_PENDING_BYTES = 4 * 1024 * 1024;

    /**
     * @brief Consume bytes, invoking on_frame once per complete frame
     * @param data Bytes just read from the socket
     * @param len Length of @p data
     * @param on_frame Called in order for each complete frame, payload only,
     *        terminator stripped. Empty frames are dropped and never delivered.
     */
    void feed(const char* data, size_t len, const std::function<void(std::string_view)>& on_frame);

    /// Discard buffered bytes and clear the overflow latch. Call on reconnect.
    void reset();

    [[nodiscard]] size_t pending_bytes() const {
        return pending_.size();
    }

    /// True once MAX_PENDING_BYTES was exceeded. Latches until reset(). The
    /// stream is unreliable from that point - the owner should reconnect.
    [[nodiscard]] bool overflowed() const {
        return overflowed_;
    }

  private:
    std::string pending_;
    bool overflowed_ = false;
};

} // namespace helix
