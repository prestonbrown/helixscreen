// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What each calibration step last measured and when, kept in the printer's
// Moonraker database so the Calibration screen can show it after a restart and
// later steps can estimate from it.

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "core/subject.h"
#include "subject_managed_panel.h"

#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "hv/json.hpp" // JSON_HPP_OK: HistoryEntry holds its values by value

namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration {

inline constexpr const char* kHistoryDbNamespace = "helix-screen";
inline constexpr const char* kHistoryDbKey = "calibration_history";
inline constexpr int kHistorySchemaVersion = 1;

/// One step's last result.
struct HistoryEntry {
    /// Local date it ran, "YYYY-MM-DD".
    std::string date;
    /// What it measured, ready to show ("0.125 mm", "MZV 52 Hz").
    std::string summary;
    /// The numbers a later step needs, as an object.
    nlohmann::json values = nlohmann::json::object();
};

/// Step id to its last result.
using History = std::map<std::string, HistoryEntry>;

/// The calendar date of a "YYYY-MM-DD" string, nullopt for anything else.
[[nodiscard]] std::optional<std::tm> parse_iso_date(std::string_view text);
/// @p t's local date as "YYYY-MM-DD".
[[nodiscard]] std::string iso_date(std::time_t t);

[[nodiscard]] nlohmann::json history_to_json(const History& h);
/// nullopt for a wrong version or a document of the wrong shape; a malformed
/// entry is dropped and the rest kept.
[[nodiscard]] std::optional<History> history_from_json(const nlohmann::json& j);

/// This printer's calibration history. Main thread only: load() and record()
/// hand Moonraker's answers back through the update queue.
class CalibrationHistory {
  public:
    static CalibrationHistory& instance();

    /// Replace the cached history with the database's, or with none when the
    /// database has no entry. A result recorded before a load succeeds is kept
    /// and written back with it; after a failed or unreadable load it waits for
    /// the next load() rather than overwriting what is stored.
    void load(IMoonrakerAPI& api);
    /// Cache and store @p step_id's result, dated today.
    void record(IMoonrakerAPI& api, const std::string& step_id, const std::string& summary,
                const nlohmann::json& values);
    void clear();

    /// @p step_id's last result, or nullptr when it never ran.
    [[nodiscard]] const HistoryEntry* entry(const std::string& step_id) const;
    [[nodiscard]] const History& entries() const {
        return history_;
    }

    /// Changes whenever entries() does: a load answered, a result recorded, a
    /// clear. Screens showing the history observe it. Created on first call;
    /// nullptr without LVGL.
    lv_subject_t* version_subject();
    /// The token an observer of version_subject() carries.
    [[nodiscard]] SubjectLifetime version_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

  private:
    void bump();
    void save(IMoonrakerAPI& api);
    void merge_loaded(IMoonrakerAPI& api, History loaded);

    History history_;
    /// Recorded while no load has succeeded, not yet merged with its answer.
    History pending_;
    /// A load() has answered with the stored history, or that there is none.
    bool loaded_ = false;
    uint64_t generation_ = 0;
    helix::Subject version_;
    SubjectManager subjects_;
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::calibration
