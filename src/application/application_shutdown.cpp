// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file application_shutdown.cpp
 * @brief Application::shutdown(): ordered teardown of services, the printer
 *        session, HTTP executors, the display and theme subjects.
 *
 * @threading Main thread only; guarded against a second call (destructor +
 *            explicit shutdown).
 */

#include "application.h"
#include "core/http_executor.h"
#include "display_manager.h"
#include "memory_monitor.h"
#include "printer_session.h"
#include "process_guards.h"
#include "remote_screen_service.h"
#include "system/crash_handler.h"
#include "theme_manager.h"
#include "wifi_link_monitor.h"

#include <spdlog/spdlog.h>

using namespace helix;

void Application::shutdown() {
    // Guard against multiple calls (destructor + explicit shutdown)
    if (m_shutdown_complete) {
        return;
    }
    m_shutdown_complete = true;

    // Expire the callbacks this object deferred to the main thread before any of
    // the subsystems they touch are torn down below (#1165).
    m_async_lifetime.invalidate();
    helix::WifiLinkMonitor::instance().stop();

    // Clean shutdown means no crash loop -- remove the marker file
    helix::clear_crash_marker(crash_marker_path());

    // Crash handler stays installed through teardown so any SIGBUS/SIGSEGV
    // during widget deletion, observer cleanup, or lv_deinit still produces
    // a crash.txt. Uninstalled at the very end of shutdown(), below.

    // Stop hot reloader thread before anything else
    if (m_hot_reloader) {
        m_hot_reloader->stop();
        m_hot_reloader.reset();
    }

    // Stop remote control server first (before tearing down UI state)
#ifdef HELIX_ENABLE_REMOTE_CONTROL
    m_remote_control.stop();
#endif
    helix::RemoteScreenService::instance().shutdown();

    // Stop memory monitor
    helix::MemoryMonitor::instance().stop();

    spdlog::info("[Application] Shutting down...");

    m_session.teardown_printer_scope(helix::PrinterSession::TeardownScope::ProcessExit,
                                     m_display.get());

    // No code path can submit new HTTP work now. Stop the executors: drains the
    // currently-executing item and breaks promises on anything still queued.
    helix::http::HttpExecutor::stop_all();

    // Shutdown display (calls lv_deinit). All observer callbacks were removed above,
    // so widget deletion touches no observer linked list.
    m_display.reset();

    // Theme manager subjects (theme_changed_subject, swatch descriptions) are
    // file-scope statics not tracked by StaticSubjectRegistry, so they are torn down
    // by hand, and only HERE, after the display is gone.
    //
    // They must outlive m_display.reset(), because that is what runs lv_xml_deinit():
    // a component scope holding a <subject_expr> owns RAW lv_observer_t* pointers
    // (lv_xml_subject_expr_t::observers) attached to these subjects, and releases them
    // with lv_observer_delete(). Those are not ObserverGuards, so
    // ObserverGuard::invalidate_all() does not cover them; deinitialising the subjects
    // first frees every observer on them and the later scope teardown reads freed
    // memory.
    //
    // Running last is safe: lv_xml_deinit() detaches the <subject_expr> observers
    // before lv_deinit(), and lv_deinit() removes the object-bound ones, so these
    // subjects have no subscribers left by now.
    theme_manager_deinit();

    // Uninstall crash handler last — clean shutdown reached this point, so a
    // SIGBUS/SIGSEGV after this is the kernel's problem, not ours.
    crash_handler::uninstall();

    spdlog::info("[Application] Shutdown complete");
}
