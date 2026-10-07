// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "application.h"
#include "display_manager.h"

#include <memory>
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
 * PrinterSession::init_ui() hands to NavigationManager::set_printer_callbacks(),
 * which is itself two thirds of the way through a full app boot.
 *
 * Same friend-TestAccess pattern as tests/test_helpers/config_test_access.h and
 * the other 19 headers here; requires `friend class ApplicationTestAccess;` on
 * Application.
 *
 * ## What a test may and may not drive
 *
 * The three soft-restart entry points (PrinterSession) end in the Application's
 * tear_down_printer_state() + init_printer_state(): a teardown (teardown_printer_scope)
 * that runs StaticSubjectRegistry::deinit_all(), StaticPanelRegistry::destroy_all() and
 * update_queue_shutdown(), followed by a full subject/Moonraker/XML rebuild. That is the
 * whole application, and running it inside a shared Catch2 process would leave every later
 * test in the shard on rebuilt global state. Tests here therefore replace that work with
 * recorders through set_restart_hooks() and drive the state machine around it.
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
        return app.m_session.m_flow.m_soft_restart_in_progress;
    }

    static std::string& wizard_previous_printer_id(Application& app) {
        return app.m_session.m_flow.m_wizard_previous_printer_id;
    }

    /// Swaps the session's teardown / rebuild / land-home work for test doubles.
    static void set_restart_hooks(Application& app, helix::PrinterSession::Restart hooks) {
        app.m_session.m_flow.m_restart = std::move(hooks);
    }

    static void add_printer_via_wizard(Application& app) {
        app.m_session.add_printer_via_wizard();
    }

    static bool note_hardware_fingerprint(Application& app, size_t fingerprint) {
        return app.m_session.note_hardware_fingerprint(fingerprint);
    }

    static void reset_discovery_session(Application& app) {
        app.m_session.reset_discovery_session();
    }

    static bool& type_mismatch_shown(Application& app) {
        return app.m_session.m_prompter.guards.type_mismatch_shown;
    }

    static bool& hardware_setup_prompt_shown(Application& app) {
        return app.m_session.m_prompter.guards.deferred_prompt_shown;
    }

    static bool& targeted_reconfig_shown(Application& app) {
        return app.m_session.m_prompter.guards.reconfig_shown;
    }

    static void switch_printer(Application& app, const std::string& printer_id) {
        app.m_session.switch_printer(printer_id);
    }

    static void cancel_add_printer_wizard(Application& app) {
        app.m_session.cancel_add_printer_wizard();
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

    /// The splash handoff restores the flush callback through the display manager, which
    /// is otherwise only built by init_display().
    static void set_display_manager(Application& app, std::unique_ptr<DisplayManager> display) {
        app.m_display = std::move(display);
    }

    static DisplayManager* display_manager(Application& app) {
        return app.m_display.get();
    }

    static lv_display_flush_cb_t& original_flush_cb(Application& app) {
        return app.m_original_flush_cb;
    }

    static void restore_flush_callback(Application& app) {
        app.restore_flush_callback();
    }

#if HELIX_HAS_PLUGINS
    /// init_plugins() runs only from deep inside run()'s boot and the
    /// printer-switch path. Setting settings_plugins_available, which unhides
    /// the Settings > Plugins row once the host holds at least one plugin, is
    /// the externally observable part a test can pin without a full boot.
    /// Needs m_config installed first (set_config); the host boots from
    /// HELIX_PLUGIN_DIR when set, else the per-printer cache under
    /// HELIX_CACHE_DIR. No sync driver is built without a Moonraker API.
    static void init_plugins(Application& app) {
        app.m_session.init_plugins();
    }
#endif
};
