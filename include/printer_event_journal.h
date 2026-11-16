// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Printer event journal: what happened to the printer (shutdowns, lost MCUs,
// pauses, `!!` errors, how jobs ended), read from the HelixPrint plugin's
// server.helix.journal when its event_journal feature is ready, else from the
// screen's own record (LocalEventJournal), which the screen fills from the
// events it sees and backfills on every reconnect from what Moonraker retains.
//
// Everything here is pure or mutex-guarded data: no LVGL, no Moonraker client.
// It is compiled into the ESP32 firmware, so it never throws.

#include "helix_plugin_features.h"
#include "json_fwd.h"
#include "moonraker_types.h" // GcodeStoreEntry

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// Entry kinds, spelled as the plugin's helix_journal.py spells them.
namespace journal_kind {
inline constexpr const char* kShutdown = "klippy_shutdown";
inline constexpr const char* kDisconnect = "klippy_disconnect";
inline constexpr const char* kReady = "klippy_ready";
inline constexpr const char* kMcuDisconnect = "mcu_disconnect";
inline constexpr const char* kFirmwareRestart = "firmware_restart";
inline constexpr const char* kPause = "pause";
inline constexpr const char* kRunout = "runout";
inline constexpr const char* kGcodeError = "gcode_error";
inline constexpr const char* kJobEnd = "job_end";
} // namespace journal_kind

inline constexpr const char* kJournalFeatureName = "event_journal";
inline constexpr int kJournalFeatureMinVersion = 1;

/// Two entries of one kind and text this close in time are one event seen twice
/// (live and again from a backfill, whose clock may be the printer's).
inline constexpr double kJournalDedupWindowS = 5.0;
/// A pause or a job's end is put down to a runout or error this recent before it.
inline constexpr double kEventCauseWindowS = 10.0;
/// A cause stamped this little after the event still counts: two clocks, or
/// two threads, can put the error a moment behind the state change it caused.
inline constexpr double kEventCauseLateS = 2.0;
/// A `!!` line repeating one recorded within this long of its last occurrence
/// counts on that entry instead of adding one; matched against the newest
/// kJournalRepeatLookback entries. The plugin's REPEAT_WINDOW_S / REPEAT_LOOKBACK.
inline constexpr double kJournalRepeatWindowS = 60.0;
inline constexpr std::size_t kJournalRepeatLookback = 16;
inline constexpr std::size_t kLocalJournalMaxEntries = 200;
inline constexpr double kJournalMaxAgeS = 30.0 * 86400.0;

struct JournalEntry {
    std::int64_t id = 0; ///< the plugin's id; 0 for the screen's own entries
    double time = 0.0;   ///< unix seconds
    std::string kind;    ///< a journal_kind value
    std::string text;    ///< the error line or shutdown reason, without `!!`
    std::string cls;     ///< classify_journal_error() of text, for errors
    std::string detail;  ///< job_end: status; pause: cause; runout: sensor name
    std::string job_id;  ///< the job history had open, if any
    std::string filename;
    bool recovered = false; ///< backfilled after a reconnect, not seen live
    int count = 1;          ///< occurrences this entry stands for (repeated `!!` lines)
    double last_time = 0.0; ///< the latest occurrence when count > 1, else 0
};

/// The index in @p entries (oldest first) of the entry @p e repeats, if any: a
/// `!!` error of the same text, class, detail and job among the
/// kJournalRepeatLookback newest, whose latest occurrence is within
/// kJournalRepeatWindowS of @p e. The same rule as the plugin's
/// JournalStore._repeat_of, pinned by tests/fixtures/journal/repeats.json.
std::optional<std::size_t> journal_repeat_of(const std::vector<JournalEntry>& entries,
                                             const JournalEntry& e);

// ---------------------------------------------------------------------------
// Classification, pinned to the plugin's by tests/fixtures/journal/
// ---------------------------------------------------------------------------

/// @p line without its `!!` prefix and surrounding whitespace.
std::string journal_error_text(const std::string& line);

/// The class of a `!!` line or shutdown reason: firmware, mcu, heater, homing,
/// move, filament, command or other.
std::string classify_journal_error(const std::string& line);

/// Whether an error line or shutdown reason reports a lost MCU.
bool is_mcu_disconnect(const std::string& line);

/// The wall clock as Unix seconds, the clock every entry's time is on.
double wall_now_s();

/// Whether something at @p cause_time can explain an event at @p event_time:
/// at most kEventCauseWindowS before it, at most kEventCauseLateS after.
bool is_recent_cause(double cause_time, double event_time);

/// Why a print paused at @p now, from the entries before it: "runout",
/// "error", else "unknown". The screen cannot see other clients' commands, so
/// it never says "command" as the plugin can.
std::string journal_pause_cause(const std::vector<JournalEntry>& entries, double now);

// ---------------------------------------------------------------------------
// Source decision
// ---------------------------------------------------------------------------

enum class JournalSource {
    Plugin, ///< the plugin's journal
    Screen, ///< the screen's own, recorded while connected
    None,   ///< neither: no plugin journal, and this build records none
};

/// Whether this build records the screen's own journal. The ESP32 firmware
/// leaves the recorder out.
#if defined(HELIX_PLATFORM_ESP32)
inline constexpr bool kLocalJournalRecorderBuilt = false;
#else
inline constexpr bool kLocalJournalRecorderBuilt = true;
#endif

struct JournalPlan {
    JournalSource source = JournalSource::Screen; ///< where views and the bundle read
    bool record_locally = true;                   ///< the screen records live events and backfills
};

/// The plugin's journal when its feature is ready; otherwise the screen keeps
/// its own, in a build that has the recorder.
JournalPlan decide_journal_source(PluginFeatureState state,
                                  bool recorder_built = kLocalJournalRecorderBuilt);

// ---------------------------------------------------------------------------
// Wire format (server.helix.journal, the screen's file, the debug bundle)
// ---------------------------------------------------------------------------

std::optional<JournalEntry> journal_entry_from_json(const nlohmann::json& j);
nlohmann::json journal_entry_to_json(const JournalEntry& e);
/// Entries of a server.helix.journal result, oldest first; malformed ones skipped.
std::vector<JournalEntry> parse_journal_result(const nlohmann::json& result);

/// The debug bundle's `event_journal` section: which source, and its entries.
nlohmann::json build_event_journal_section(JournalSource source,
                                           const std::vector<JournalEntry>& entries);

// ---------------------------------------------------------------------------
// Backfill: what Moonraker retains, turned into entries newer than @p since
// ---------------------------------------------------------------------------

/// `!!` responses of server.gcode_store.
std::vector<JournalEntry> backfill_from_gcode_store(const std::vector<GcodeStoreEntry>& store,
                                                    double since);
/// Jobs of a server.history.list result that ended after @p since.
std::vector<JournalEntry> backfill_from_history(const nlohmann::json& result, double since);
/// Shutdowns in a klippy.log tail. Klipper stamps them with its monotonic
/// clock, so each is placed through the newest "Start printer at" line before
/// it; one with no such line in the window has no wall time and is skipped.
std::vector<JournalEntry> backfill_from_klippy_log(const std::string& log, double since);
/// Whether a reconnect's backfill must read klippy.log: Klipper is shut down
/// now, or a job in the gap ended by a shutdown.
bool backfill_needs_klippy_log(bool klippy_shutdown_now,
                               const std::vector<JournalEntry>& history_entries);

// ---------------------------------------------------------------------------
// The screen's own journal
// ---------------------------------------------------------------------------

/// Bounded by count and age, oldest first, safe from any thread.
class LocalEventJournal {
  public:
    static LocalEventJournal& instance();

    explicit LocalEventJournal(std::size_t max_entries = kLocalJournalMaxEntries,
                               double max_age_s = kJournalMaxAgeS);

    /// Add a live entry. false when it duplicates one already held.
    bool append(JournalEntry entry, double now);

    /// Add backfilled entries, each marked recovered, skipping duplicates.
    /// Returns how many were added.
    std::size_t merge_backfill(std::vector<JournalEntry> entries, double now);

    [[nodiscard]] std::vector<JournalEntry> snapshot() const;
    /// The newest entry's time, or 0 when empty.
    [[nodiscard]] double latest_time() const;
    /// Bumped on every change.
    [[nodiscard]] std::uint32_t version() const;

    [[nodiscard]] nlohmann::json to_json() const;
    /// Replace the contents from to_json()'s shape; false when @p j is not one.
    bool load_json(const nlohmann::json& j, double now);
    bool load_file(const std::string& path, double now);
    bool save_file(const std::string& path) const;
    void clear();

  private:
    bool contains_duplicate_locked(const JournalEntry& e) const;
    void insert_sorted_locked(JournalEntry e);
    void prune_locked(double now);

    mutable std::mutex mutex_;
    std::vector<JournalEntry> entries_;
    std::size_t max_entries_;
    double max_age_s_;
    std::uint32_t version_ = 0;
};

// ---------------------------------------------------------------------------
// Presentation decision, shared by every view
// ---------------------------------------------------------------------------

enum class JournalSeverity { Info, Success, Warning, Error };

struct JournalRow {
    JournalSeverity severity = JournalSeverity::Info;
    const char* title_tag = ""; ///< untranslated; the view translates it
    /// What the message line says beyond the title, untranslated where a tag.
    std::string message;
    const char* message_tag = nullptr; ///< when set, translate this instead of message
    int repeat_count = 1;              ///< > 1: the view says how many times it happened
};

/// How one entry reads in a list: severity and title from its kind (job_end
/// from its status), the message from its text or detail.
JournalRow describe_journal_entry(const JournalEntry& e);

/// Entries for a list, newest first.
std::vector<JournalEntry> journal_view_order(std::vector<JournalEntry> entries);

} // namespace helix
