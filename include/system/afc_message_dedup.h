// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file afc_message_dedup.h
 * @brief Cross-session dedup seed for AFC's latched error messages
 *
 * AFC latches printer.AFC.message: the entry stays long after the condition
 * that produced it resolved, and it survives HelixScreen restarts. The
 * backend's in-memory text dedup cannot tell "first time this process saw
 * the text" from "the same text a previous session already surfaced", so
 * without a seed every restart re-toasts a resolved error as live
 * (prestonbrown/helixscreen#1589).
 *
 * This store is that seed: the last error-typed message text a session
 * surfaced, persisted in the config dir. error_state is deliberately NOT
 * part of the discriminator — upstream's AFC_logger.error() enqueues
 * error-typed messages for live, non-pausing faults (lane load failures,
 * Spoolman outages) without ever setting error_state, so keying on it
 * would suppress real errors.
 *
 * File: config/afc_message_dedup.json ({"last_error": "<text>"}).
 *
 * Uninitialized (init() not called — unit tests), the store reads as empty
 * and records nothing: every message looks new, which fails open to a
 * toast, never to a suppressed error.
 */

#include <mutex>
#include <string>

namespace helix {

class AfcMessageDedup {
  public:
    static AfcMessageDedup& instance();

    AfcMessageDedup(const AfcMessageDedup&) = delete;
    AfcMessageDedup& operator=(const AfcMessageDedup&) = delete;

    /**
     * @brief Initialize with the config directory and load the seed.
     *
     * A missing or unreadable file, or one of an unexpected shape, leaves
     * the seed empty — one extra toast, never a suppressed error.
     */
    void init(const std::string& config_dir);

    /// Reset to the uninitialized state (for testing).
    void shutdown();

    /// The last error text a session surfaced; empty when nothing is
    /// recorded or the store is uninitialized.
    [[nodiscard]] std::string last_error_text() const;

    /**
     * @brief Record the error text a session surfaced.
     *
     * Writes through only when the value changed, so callers may invoke it
     * from a per-frame path without I/O per frame.
     */
    void record_error(const std::string& text);

    /**
     * @brief Record that AFC's message queue reported empty.
     *
     * A later recurrence of a previously-seen text is then a new event and
     * must toast again.
     */
    void record_cleared();

  private:
    AfcMessageDedup() = default;
    ~AfcMessageDedup() = default;

    void save_locked(const std::string& text);

    mutable std::mutex mutex_;
    std::string config_dir_;
    std::string last_error_;
    bool initialized_ = false;
};

} // namespace helix
