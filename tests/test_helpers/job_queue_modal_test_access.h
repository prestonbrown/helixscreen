// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_job_queue_modal.h"

#include <string>

/**
 * @brief Friend access to JobQueueModal::start_job().
 *
 * start_job() is reachable in production only from a row's click callback, and
 * its guard runs before any widget is touched - so a test can call it on a
 * modal that was never shown. That is the whole point: the guard is the unit,
 * and building the modal's widget tree to reach it would test the XML instead.
 *
 * register_callbacks() binds the names job_queue_row.xml's event_cbs resolve,
 * which must exist before a row is created. attach_row_data() gives a bare row
 * the job its callbacks act on; the row finds its modal the way every row does,
 * by sitting inside that modal's dialog.
 */
class JobQueueModalTestAccess {
  public:
    static void start_job(helix::JobQueueModal& modal, const std::string& job_id,
                          const std::string& filename) {
        modal.start_job(job_id, filename);
    }

    static void register_callbacks() {
        helix::JobQueueModal::register_callbacks();
    }

    static void attach_row_data(lv_obj_t* row, const std::string& job_id,
                                const std::string& filename) {
        helix::JobQueueModal::attach_row_data(row, job_id, filename);
    }
};
