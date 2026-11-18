// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Why a print paused, was canceled or failed. Klipper says so in
// print_stats.message for most failures, but a pause or cancel after a failed
// command (a quad gantry level that did not converge, a filament system that
// could not load a lane) leaves the message empty and the reason only in the
// `!!` line the command answered with. The tracker remembers the last such line
// and, when it has none near the stop, reads Moonraker's gcode_store.
//
// The store is stamped with Moonraker's clock, which need not agree with the
// screen's. It is read on the screen's clock through a line the tracker saw
// live and finds again in the store; a store with no such line explains
// nothing, since its lines cannot be placed relative to the stop.
//
// An outside monitor (Print Handy, and any tool that follows its convention)
// pauses through Moonraker's /printer/print/pause, which carries no reason.
// Just before it pauses it sets the display message with
// `M117 Paused by <source>: <reason>`; display_status.message is already
// subscribed. The announcement explains a pause it arrived shortly before (or
// just after), so a stale one left up by an earlier pause never explains a
// user's own.
//
// Compiled into the ESP32 firmware: no exceptions, no RTTI.

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "json_fwd.h"
#include "printer_event_journal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix {

class IMoonrakerClient;
class PrinterPrintState;
class PrinterState;
enum class PrintJobState;

/// A pause an outside monitor announced in the display message.
struct MonitorPause {
    std::string source; ///< the tool, as it names itself ("Print Handy")
    std::string reason; ///< the tool's own words ("possible spaghetti")
};

/// @p display_message as `Paused by <source>: <reason>`, both non-empty; the
/// source ends at the first ": ". nullopt for any other text.
std::optional<MonitorPause> parse_monitor_pause(const std::string& display_message);

/// The pause reason the screen shows for @p pause.
std::string monitor_pause_reason_text(const MonitorPause& pause);

/// What a `!!` response says, shown as a reason: its first line (filament
/// systems follow theirs with diagrams and instructions), without the prefix,
/// decoded like print_stats.message.
std::string gcode_error_reason_text(const std::string& line);

/// The newest `!!` line of a notify_gcode_response frame, as
/// gcode_error_reason_text(); nullopt when the frame has none.
std::optional<std::string> last_gcode_error_text(const nlohmann::json& msg);

/// @p state's last `!!` line when is_recent_cause() of @p now, else empty.
std::string recent_gcode_error(const PrinterPrintState& state, double now);

/// The reason a print stopped as @p stop at @p stop_time: a pause's
/// @p monitor announcement when it arrived is_recent_cause() of the stop, else
/// @p print_message when Klipper set one, else the newest gcode_error among
/// @p entries that is_recent_cause() of the stop. Empty unless @p stop is
/// PAUSED, CANCELLED or ERROR, or when nothing explains it.
std::string decide_print_stop_reason(PrintJobState stop, const std::string& print_message,
                                     const std::vector<JournalEntry>& entries, double stop_time,
                                     const std::optional<JournalEntry>& monitor = std::nullopt);

/// What to add to a time in @p store to put it on the screen's clock, given
/// that the screen saw @p line arrive at @p seen_at: the newest response in
/// @p store whose text is @p line arrived then. nullopt when the store does
/// not hold @p line.
std::optional<double> gcode_store_clock_shift(const std::vector<GcodeStoreEntry>& store,
                                              const std::string& line, double seen_at);

/// Publishes PrinterPrintState's print_stop_reason. Main thread: built,
/// destroyed and run there; the gcode response tap hops to it.
class PrintStopReasonTracker {
  public:
    static constexpr const char* kHandlerName = "print_stop_reason";
    /// gcode_store lines read when the tap saw no error near the stop.
    static constexpr int kGcodeStoreCount = 30;

    /// @param now Unix seconds; a copy of it is called on the WebSocket thread.
    PrintStopReasonTracker(IMoonrakerClient& client, PrinterState& state,
                           std::function<double()> now = {});
    ~PrintStopReasonTracker();

    PrintStopReasonTracker(const PrintStopReasonTracker&) = delete;
    PrintStopReasonTracker& operator=(const PrintStopReasonTracker&) = delete;

  private:
    /// PAUSED, CANCELLED or ERROR while the print is stopped that way, else STANDBY.
    [[nodiscard]] PrintJobState current_stop() const;
    [[nodiscard]] bool stopped() const;
    void on_state();
    void on_gcode_error(JournalEntry e);
    void on_display_message(const char* text);
    void on_gcode_store(const std::vector<GcodeStoreEntry>& store, std::uint32_t stop_id);
    void publish();

    /// The newest gcode response line seen live, written on the WebSocket
    /// thread. Shared with the tap so the tap never reaches the tracker.
    struct LiveLine {
        std::mutex mutex;
        std::string text;
        double time = 0.0;
    };

    IMoonrakerClient& client_;
    PrinterState& state_;
    std::function<double()> now_;

    PrintJobState stop_;
    double stop_time_ = 0.0;
    /// Bumped on every stop, so a store read answers only the stop that asked.
    std::uint32_t stop_id_ = 0;
    /// Klipper clears print_stats.message when a job is reset, so the one that
    /// explained this stop is kept.
    std::string stop_message_;
    /// A pause's reason, kept when the paused print is then canceled.
    std::string carried_reason_;
    std::optional<JournalEntry> last_error_;
    /// The newest monitor announcement, its text as shown, stamped on arrival.
    /// Kept while the display message moves on: a pause macro's own M117 can
    /// replace it before the pause lands.
    std::optional<JournalEntry> monitor_;
    /// From gcode_store, moved onto the screen's clock.
    std::vector<JournalEntry> recovered_;
    std::shared_ptr<LiveLine> live_line_ = std::make_shared<LiveLine>();

    ObserverGuard outcome_observer_;
    ObserverGuard job_state_observer_;
    ObserverGuard message_observer_;
    ObserverGuard display_message_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
