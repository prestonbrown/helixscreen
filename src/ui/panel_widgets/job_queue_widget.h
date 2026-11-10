// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_coalesced_timer.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "job_queue_tile_state.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/job_queue_tile_layout.h"

#include <string>

namespace helix {

/// Home tile for the job queue. Reads the queue into a JobQueueTileState, measures
/// what each rung would draw, decides the rung with decide_job_queue_tile_layout(),
/// and publishes the verdict and the texts as subjects that
/// panel_widget_job_queue.xml binds. A tap opens the queue modal; the start button
/// (Strip and up, while a job waits) starts the front job through QueuedJobStarter.
class JobQueueWidget : public PanelWidget {
  public:
    JobQueueWidget();
    ~JobQueueWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "job_queue";
    }

    static void on_tile_clicked(lv_event_t* e);   // XML "on_job_queue_widget_clicked"
    static void on_action_clicked(lv_event_t* e); // XML "on_job_queue_tile_action"

  protected:
    void on_hooked_root_deleted() override;

  private:
    [[nodiscard]] JobQueueTileInputs read_inputs() const;
    /// @p cell_name, when given, gets the next job's name as a Cell draws it.
    [[nodiscard]] JobQueueTileVerdict decide(int width_px, int height_px,
                                             const JobQueueTileInputs& in,
                                             std::string* cell_name = nullptr) const;
    /// Coalesces the observers' notifications into one refresh on the next tick.
    void schedule_refresh();
    void refresh();
    void publish(const JobQueueTileInputs& in);

    int granted_w_ = 0;
    int granted_h_ = 0;
    JobQueueTileVerdict verdict_{};
    std::string cell_name_; ///< the next job's name wrapped for the Cell, empty elsewhere
    ui::CoalescedTimer refresh_timer_{0}; ///< next tick, like lv_async_call

    ObserverGuard count_observer_;
    ObserverGuard revision_observer_;
    ObserverGuard start_observer_;
    ObserverGuard run_state_observer_;
    ObserverGuard automatic_observer_;
    ObserverGuard estimate_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard language_observer_;

    // Declared last so it is torn down first, expiring every token the
    // observers captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_job_queue_widget();

} // namespace helix
