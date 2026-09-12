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
 * File: config/afc_message_dedup.json,
 * `{"printers": {"<printer id>": "<text>"}}`. The record is per printer:
 * one config dir serves every printer the user has configured, so a global
 * record would let printer A's error text suppress the identical text on
 * printer B. The key is resolved from Config on every call, because a
 * printer switch reconstructs the AFC backend without re-initializing this
 * store.
 *
 * Every degradation fails OPEN — to one extra toast, never to a suppressed
 * error. A missing, oversized, corrupt or unreadable file leaves the seed
 * empty; a seed the store cannot rewrite is dropped rather than armed,
 * because a seed that can never be retracted would suppress its text in
 * every future session. Uninitialized (init() not called), the store reads
 * as empty, records nothing, and says so once at warn.
 */

#include <map>
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
     * A missing or unreadable file, one of an unexpected shape, one too
     * large to be anything this store wrote, or one that cannot be
     * rewritten leaves the seed empty — one extra toast, never a
     * suppressed error.
     */
    void init(const std::string& config_dir);

    /// Reset to the uninitialized state (for testing).
    void shutdown();

    /// The last error text a session surfaced for the active printer; empty
    /// when nothing is recorded or the store is uninitialized.
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
     * must toast again — which means the record has to leave the disk too,
     * by truncation or, failing that, by deleting the file.
     */
    void record_cleared();

  private:
    AfcMessageDedup() = default;
    ~AfcMessageDedup() = default;

    /// A seed file larger than this is corruption, not a record: the store
    /// writes one short line per configured printer.
    static constexpr size_t MAX_SEED_BYTES = 256 * 1024;

    /// The printer the records are keyed by, or "default" when no printer is
    /// active yet.
    static std::string active_printer_key();

    std::string seed_path_locked() const;

    /// Write the whole map. False when nothing durable landed.
    bool save_locked();

    /// True when the seed file can be opened for writing. Opening for append
    /// writes nothing and creates nothing the caller has not already read.
    bool can_write_locked() const;

    /// True when the store is uninitialized, saying so once per init cycle.
    bool warn_uninitialized_locked(const char* op) const;

    mutable std::mutex mutex_;
    std::string config_dir_;
    std::map<std::string, std::string> last_error_by_printer_;
    bool initialized_ = false;
    mutable bool warned_uninitialized_ = false;
};

} // namespace helix
