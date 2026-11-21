// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Remote checks for new printer firmware: a provider row may name a vendor endpoint
// and how to read the offered version, download URL, size and checksum out of its
// answer. The checks ride on UpdateChecker's auto-check timer (15 s after start,
// then daily) rather than a poller of their own, skip while a job holds the
// machine, back off after failures, and only ever raise a notification: nothing
// here downloads or flashes. Moonraker update_manager entries are never polled
// from here; Moonraker refreshes them on its own schedule.

#include "firmware_update.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace helix::fwupdate {

/// Where a row checks for new firmware and how to read the answer. Field paths are
/// JSON pointers ("/data/version"); nullptr when the answer carries no such field.
struct CheckSource {
    /// Exact URL, sent with no identifying data. nullptr: no verified endpoint yet.
    const char* url;
    const char* version_ptr;
    const char* download_url_ptr;
    const char* size_ptr;
    const char* sha256_ptr;
    /// A file whose first line is the installed firmware version, or nullptr.
    const char* installed_version_file;
};

/// The row's check source, or nullptr when it has none with a verified endpoint.
const CheckSource* check_source(const Provider& p);

/// What a vendor endpoint offers.
struct RemoteFirmware {
    std::string version;
    std::string download_url;
    uint64_t size = 0;
    std::string sha256;
};

/// @p body read through @p source's field paths. nullopt when the body is not
/// JSON or carries no version.
std::optional<RemoteFirmware> parse_check_response(const CheckSource& source,
                                                   std::string_view body);

/// Whether @p offered is an update over @p installed: both known and different.
/// An unknown installed version never counts, so a missing file cannot raise a
/// notification on every check.
bool offers_update(const RemoteFirmware& offered, std::string_view installed);

/// The checker's memory between ticks.
struct CheckState {
    int consecutive_failures = 0;
    int ticks_since_attempt = 0;
};

/// Ticks to let pass after @p failures failed checks in a row: none after the
/// first, then 1, 3 and at most 6 (a tick is a day).
int backoff_ticks(int failures);

/// One tick of the shared timer: whether to check now. Counts the tick in @p state.
/// Never checks while @p checks_suppressed (HelixScreen's update checks are off),
/// while @p job_holds_machine, or without a source.
bool should_check(CheckState& state, bool checks_suppressed, bool job_holds_machine,
                  bool has_source);

/// Record a check's outcome in @p state.
void record_check(CheckState& state, bool succeeded);

/// GET @p url; {status, body}, status 0 on a transport failure. BLOCKING.
using Fetch = std::function<std::pair<int, std::string>(const std::string& url)>;

/// What one check came to.
struct CheckOutcome {
    bool succeeded = false;
    std::optional<RemoteFirmware> update; ///< set when the endpoint offers an update
};
/// Run one check of @p source: fetch, parse, compare with @p installed. BLOCKING.
CheckOutcome run_firmware_check(const CheckSource& source, std::string_view installed,
                                const Fetch& fetch);

/// UpdateChecker's auto-check timer calls this on every tick. Main thread.
void on_update_check_tick();

/// The update the last check found, or nullopt.
std::optional<RemoteFirmware> remote_update_found();

} // namespace helix::fwupdate
