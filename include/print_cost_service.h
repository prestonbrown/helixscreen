// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file print_cost_service.h
 * @brief Gathers what print_cost.h prices a file with: the spool feeding each
 *        tool (Spoolman), the currency, the user's settings and the shared
 *        cost record in the Moonraker database.
 *
 * @threading Main thread only. Callbacks run on the main thread, and never
 *            once @p token has expired.
 */

#pragma once

#include "async_lifetime_guard.h"
#include "moonraker_types.h"
#include "print_cost.h"

#include <functional>
#include <optional>
#include <string>

namespace helix::print_cost {

/// A file's estimate and everything needed to show it at any point of the print.
struct Estimate {
    Choice choice = Choice::None;
    Breakdown own;                ///< Our own figure (complete or not)
    std::optional<Record> record; ///< The shared record, when one answered
    std::string own_currency; ///< Spoolman's when currency_source(own) says so, else the setting's
    double file_modified = 0.0;
    double estimated_seconds = 0.0;
    CostSettings settings;

    /// What to show at @p filament_fraction of the file after @p elapsed_seconds:
    /// the whole estimate at 1.0 and the slicer's time, the cost so far mid-print.
    /// Empty when there is nothing to show.
    [[nodiscard]] std::string text_at(double filament_fraction, double elapsed_seconds) const;
    /// The estimate before printing: the whole file at the slicer's time.
    [[nodiscard]] std::string text() const {
        return text_at(1.0, estimated_seconds);
    }
};

/// @p amount in @p currency for display; "0.00" style for a zero amount.
[[nodiscard]] std::string format_cost(double amount, const std::string& currency);

/// The user's cost settings from SettingsManager.
[[nodiscard]] CostSettings current_settings();

/// The lane override for one tool: the Spoolman spool id and material of the
/// lane the caller has picked for @p tool, or nullopt to use the printer's routing.
struct LanePick {
    int spoolman_id = 0;
    std::string material;
};
using LaneLookup = std::function<std::optional<LanePick>(int tool)>;

/// Price @p filename (gcodes-relative) from @p metadata. @p lanes may be empty.
void estimate_file(const std::string& filename, const FileMetadata& metadata,
                   const LaneLookup& lanes, const LifetimeToken& token,
                   std::function<void(const Estimate&)> on_done);

/// Read the shared record for @p filename; nullopt when absent or unreadable.
void read_record(const std::string& filename, const LifetimeToken& token,
                 std::function<void(const std::optional<Record>&)> on_done);

/**
 * @brief The cost of the print that is running, for Print Status and the
 *        completion summary.
 *
 * Publishes `print_cost_text` (the cost so far) and `print_cost_visible`
 * (1 when there is a figure to show). The print media path hands it each
 * active file's metadata; Print Status hands it the print's progress.
 */
class ActivePrintCost {
  public:
    static ActivePrintCost& instance();

    /// Register the subjects; before any XML binding them. Idempotent.
    void init_subjects();

    /// Forget the last print's file and estimate: the running file changed and
    /// its own metadata has not arrived (and may never).
    void clear();

    /// Price the file the printer is running (gcodes-relative name).
    void set_file(const std::string& filename, const FileMetadata& metadata);

    /// Where the print is: filament used (mm), progress (0..1), seconds printing.
    void update(double filament_used_mm, double progress, double elapsed_seconds);

    /**
     * @brief The actual cost of the print that just ended
     *
     * @p on_text gets "" when there is nothing to show. A shared record whose
     * writer has not yet added the actual is read again a few times before our
     * own figure is shown, so the summary does not flash a second price.
     */
    void actual(double duration_seconds, double filament_used_mm, double progress,
                std::function<void(const std::string&)> on_text);

  private:
    ActivePrintCost() = default;
    [[nodiscard]] double filament_fraction(double filament_used_mm, double progress) const;
    void publish();
    void await_actual(int attempt, double duration_seconds, double fraction,
                      double print_started_at, std::function<void(const std::string&)> on_text);

    AsyncLifetimeGuard lifetime_;
    std::string filename_;
    double filament_total_mm_ = 0.0;
    std::optional<Estimate> estimate_;
    double filament_used_mm_ = 0.0;
    double progress_ = 0.0;
    double elapsed_seconds_ = 0.0;
};

} // namespace helix::print_cost
