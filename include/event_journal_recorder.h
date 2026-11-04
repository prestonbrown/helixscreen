// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "json_fwd.h"
#include "printer_event_journal.h"

#include <string>
#include <vector>

namespace helix {
class IMoonrakerClient;

/// Fills the screen's own event journal (LocalEventJournal) while the
/// plugin's event_journal feature is not ready (decide_journal_source):
/// klippy shutdowns with their reason, disconnects, ready, lost MCUs, pauses,
/// `!!` lines and how jobs ended, as this screen sees them. On every
/// (re)connect it backfills the gap from server.gcode_store, history and, when
/// the gap held a shutdown, the klippy.log tail. Persisted to the user config
/// directory after every change.
///
/// Owned by GcodeResponseRouting, built and destroyed on the main thread; the
/// client must outlive it.
class EventJournalRecorder {
  public:
    /// @p journal_path empty: event_journal.json in the user config directory.
    explicit EventJournalRecorder(IMoonrakerClient* client,
                                  LocalEventJournal& journal = LocalEventJournal::instance(),
                                  std::string journal_path = {});
    ~EventJournalRecorder();

    EventJournalRecorder(const EventJournalRecorder&) = delete;
    EventJournalRecorder& operator=(const EventJournalRecorder&) = delete;

  private:
    friend struct EventJournalRecorderTestAccess;

    /// Main thread: whether the screen keeps its own journal right now.
    [[nodiscard]] bool recording() const;
    /// Main thread: stamp @p e with now and the open job, append, save.
    void record(JournalEntry e);
    void save() const;

    void on_gcode_response(const nlohmann::json& msg);
    void on_klippy_shutdown();
    void on_shutdown_reason(const nlohmann::json& response, JournalEntry pending);
    void on_history_changed(const nlohmann::json& msg);
    void on_print_state(int state);
    void on_connected();
    void on_history_backfill(const nlohmann::json& response, double since,
                             bool klippy_shutdown_now);
    void merge(std::vector<JournalEntry> entries);

    IMoonrakerClient* client_;
    LocalEventJournal& journal_;
    std::string path_;
    int last_print_state_ = -1;
    std::string job_id_;
    std::string filename_;
    bool backfill_in_flight_ = false;
    ObserverGuard print_state_observer_;

    /// Declared last so it destructs first: callbacks still queued from the
    /// WebSocket or HTTP threads are dropped before any member goes.
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
