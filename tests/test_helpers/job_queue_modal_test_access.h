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
 */
class JobQueueModalTestAccess {
  public:
    static void start_job(helix::JobQueueModal& modal, const std::string& job_id,
                          const std::string& filename) {
        modal.start_job(job_id, filename);
    }
};
