// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"
#include "ui_observer_guard.h"

#include <string>

struct JobQueueEntry;
class JobQueueModalTestAccess;

namespace helix {

/**
 * @brief Modal overlay showing job queue contents with management actions
 *
 * Displays queued print jobs with delete capability, and provides
 * queue start/pause toggle. Populated dynamically from JobQueueState.
 */
class JobQueueModal : public Modal {
    friend class ::JobQueueModalTestAccess;

  public:
    JobQueueModal();
    ~JobQueueModal() override;

    const char* get_name() const override {
        return "Job Queue";
    }
    const char* component_name() const override {
        return "job_queue_modal";
    }

    /// Show the modal, refreshing job list from current state
    bool show(lv_obj_t* parent);

  protected:
    void on_show() override;
    void on_hide() override;
    void on_ok() override;

  private:
    static void register_callbacks();
    static bool callbacks_registered_;
    static JobQueueModal* s_active_instance_;

    void populate_job_list();
    void update_queue_state_ui();
    void toggle_queue();
    void remove_job(const std::string& job_id);
    void start_job(const std::string& job_id, const std::string& filename);

    // Observer for auto-refresh when queue data changes
    ObserverGuard count_observer_;
    bool list_rebuild_pending_ = false; ///< Coalesces rapid count observer notifications
};

} // namespace helix
