// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file queued_job_starter_test_access.h
 * @brief Friend shim for QueuedJobStarter's private members.
 *
 * Declared `friend class ::QueuedJobStarterTestAccess` in queued_job_start.h,
 * so it lives at global scope and is defined once for the whole test binary.
 */

#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "queued_job_start.h"

#include <functional>
#include <utility>

class QueuedJobStarterTestAccess {
  public:
    /// The prep manager the starter's next start resolves.
    static helix::ui::PrintPreparationManager* prep(helix::queue::QueuedJobStarter& s) {
        return s.prep_source_();
    }

    static lv_timer_t* stall_watchdog(helix::queue::QueuedJobStarter& s) {
        return s.stall_watchdog_.get();
    }

    using PrepSource = helix::SharedCallback<helix::ui::PrintPreparationManager*()>;

    /// Point the starter at @p prep instead of print-select's.
    static void set_prep(helix::queue::QueuedJobStarter& s,
                         helix::ui::PrintPreparationManager* prep) {
        s.prep_source_ = [prep]() { return prep; };
    }

    /// Install @p source and return the one it replaced, for the process-wide
    /// starter that must be handed back as it was found.
    static PrepSource swap_prep_source(helix::queue::QueuedJobStarter& s, PrepSource source) {
        std::swap(s.prep_source_, source);
        return source;
    }

    static helix::ui::PrintStartController& controller(helix::queue::QueuedJobStarter& s) {
        return *s.controller_;
    }
};
