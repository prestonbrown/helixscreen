// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "application.h"

#include <string>

namespace helix {
class Config;
}

/**
 * @brief Test-only accessor for Application's private lifecycle members
 *
 * Application is entirely private below run(), so the soft-restart paths
 * (switch_printer / add_printer_via_wizard / cancel_add_printer_wizard) have no
 * public entry point — production reaches them through the lambdas
 * Application::init_ui() hands to NavigationManager::set_printer_callbacks(),
 * which is itself two thirds of the way through a full app boot.
 *
 * Same friend-TestAccess pattern as tests/test_helpers/config_test_access.h and
 * the other 19 headers here; requires `friend class ApplicationTestAccess;` on
 * Application.
 *
 * ## What a test may and may not drive
 *
 * The three soft-restart entry points all end in tear_down_printer_state() +
 * init_printer_state(): a 16-step teardown that runs
 * StaticSubjectRegistry::deinit_all(), StaticPanelRegistry::destroy_all() and
 * update_queue_shutdown(), followed by a full subject/Moonraker/XML rebuild.
 * That is the whole application, and running it inside a shared Catch2 process
 * would leave every later test in the shard on rebuilt global state. Tests here
 * therefore exercise the branches that return BEFORE teardown (the re-entrancy
 * latch, config validation) and the synchronous config surgery that
 * cancel_add_printer_wizard() performs before deferring its teardown.
 *
 * neutralize_destructor() is mandatory for the same reason: ~Application() calls
 * shutdown() unconditionally, and shutdown() tears down TelemetryManager,
 * UpdateChecker, SoundManager, NavigationManager and the UpdateQueue — process
 * singletons shared with the rest of the suite.
 */
class ApplicationTestAccess {
  public:
    /// Install the Config the soft-restart paths read. Null until init_config().
    static void set_config(Application& app, helix::Config* config) {
        app.m_config = config;
    }

    /// Make ~Application() a no-op by pre-tripping shutdown()'s idempotency guard.
    /// Only valid for an Application that was never run() — nothing to release.
    static void neutralize_destructor(Application& app) {
        app.m_shutdown_complete = true;
    }

    static bool& soft_restart_in_progress(Application& app) {
        return app.m_soft_restart_in_progress;
    }

    static std::string& wizard_previous_printer_id(Application& app) {
        return app.m_wizard_previous_printer_id;
    }

    static void switch_printer(Application& app, const std::string& printer_id) {
        app.switch_printer(printer_id);
    }

    static void cancel_add_printer_wizard(Application& app) {
        app.cancel_add_printer_wizard();
    }

    /// Android pause/resume hooks. Production reaches these only from inside
    /// run()'s main loop, which is unreachable in a unit test, so the only way
    /// to pin the pause/resume contract is to call them directly.
    static void on_enter_background(Application& app) {
        app.on_enter_background();
    }

    static void on_enter_foreground(Application& app) {
        app.on_enter_foreground();
    }

    static bool backgrounded(const Application& app) {
        return app.m_backgrounded;
    }
};
