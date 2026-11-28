// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A bed-occupancy state over a history the test fills. The verdict is the
// state's own decision over the newest history job, as it is in the app: a
// test puts a part on the bed by giving the history a finished job, and
// empties the bed by giving it no such job.

#include "bed_occupancy_state.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "print_history_manager.h"
#include "print_history_manager_test_access.h"
#include "printer_state.h"

#include <optional>
#include <utility>
#include <vector>

namespace helix::test {

/// The job a part on the bed came from, @p object_height_mm tall (0: unknown).
/// @p start_time names the job together with its id, so a Cleared tap that
/// another test recorded never clears this one.
inline PrintHistoryJob part_on_bed_job(double object_height_mm, double start_time) {
    PrintHistoryJob job;
    job.job_id = "job-on-the-bed";
    job.filename = "part-on-the-bed.gcode";
    job.status = PrintJobStatus::COMPLETED;
    job.start_time = start_time;
    job.end_time = start_time + 60.0;
    job.print_duration = 60.0;
    job.total_duration = 60.0;
    job.object_height = object_height_mm;
    return job;
}

/// The app's bed-occupancy state over its own history, without the socket:
/// the history holds what the test gives it, and nothing loads behind the
/// test's back. The state's subjects are initialized on construction.
class PartOnBed {
  public:
    explicit PartOnBed(PrinterState& printer_state)
        : api_(client_, printer_state), history_(&api_, &client_),
          state_(printer_state, &history_) {
        state_.init_subjects();
        // A history that has not loaded stands in the last occupying job the
        // config holds, whatever an earlier test left there.
        put(false, std::nullopt);
    }
    ~PartOnBed() {
        // The state records the last occupying job in the config. Leave none.
        put(false, std::nullopt);
    }
    PartOnBed(const PartOnBed&) = delete;
    PartOnBed& operator=(const PartOnBed&) = delete;

    /// A part @p height_mm tall on the bed (nullopt: a height not recorded),
    /// or no part. The history holds that finished job and nothing newer.
    /// Each part is a new job: one the user tapped Cleared for cannot come back.
    void put(bool occupied, std::optional<double> height_mm) {
        std::vector<PrintHistoryJob> jobs;
        if (occupied) {
            jobs.push_back(part_on_bed_job(height_mm.value_or(0.0), next_start_time()));
        }
        PrintHistoryManagerTestAccess::set_loaded_jobs(
            history_, std::move(jobs), HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);
        // The history's observers re-decide on their own; this is for a state
        // that was not listening when the history changed.
        state_.refresh();
        ui::UpdateQueue::instance().drain();
    }

    /// The state, for installing as the app's and for reading its verdict.
    BedOccupancyState& state() {
        return state_;
    }

  private:
    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24, 1000.0};
    MoonrakerAPI api_;
    PrintHistoryManager history_;
    BedOccupancyState state_;

    /// Start times name jobs together with their ids, so a Cleared tap that
    /// another test, or another part of this one, recorded never clears this part.
    static double next_start_time() {
        static double last = 1'700'000'000.0;
        last += 86'400.0;
        return last;
    }
};

} // namespace helix::test
