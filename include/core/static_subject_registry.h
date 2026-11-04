// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_shared_callback.h"

#include <functional>
#include <string>
#include <vector>

/**
 * @brief Registry for static singleton subject cleanup to ensure proper destruction order
 *
 * Core state singletons (PrinterState, AmsState, SettingsManager, FilamentSensorManager)
 * have LVGL subjects that UI widgets observe. When lv_deinit() runs, it deletes widgets
 * which try to remove their observers from subjects. If subjects haven't been deinitialized
 * first, this causes crashes in lv_observer_remove.
 *
 * This registry is separate from StaticPanelRegistry because:
 * 1. These are NOT panels - they're core state singletons
 * 2. They need different destruction timing (before lv_deinit, after panels)
 *
 * Destruction order in Application::shutdown():
 * 1. StaticPanelRegistry::destroy_all() - panels clean up their own subjects
 * 2. StaticSubjectRegistry::deinit_all() - deinit core singleton subjects
 * 3. lv_deinit() - LVGL cleanup (now safe - all observers disconnected)
 *
 * ## Self-Registration Pattern (MANDATORY)
 *
 * Every class that creates LVGL subjects MUST self-register its cleanup inside its own
 * init_subjects() method. This co-locates init and cleanup registration so they can't
 * get out of sync — forgetting to register is impossible because the registration lives
 * right next to the initialization.
 *
 * ```cpp
 * void MyState::init_subjects() {
 *     if (subjects_initialized_) return;
 *     // ... init subjects ...
 *     subjects_initialized_ = true;
 *
 *     // Self-register cleanup — ensures deinit runs before lv_deinit()
 *     StaticSubjectRegistry::instance().register_deinit(
 *         "MyState", []() { MyState::instance().deinit_subjects(); });
 * }
 * ```
 *
 * DO NOT register cleanup externally (e.g., in SubjectInitializer). That pattern is
 * fragile — adding init_subjects() without a matching register_deinit() causes
 * shutdown crashes that are hard to diagnose.
 */
class StaticSubjectRegistry {
  public:
    /**
     * @brief Get the singleton instance
     */
    static StaticSubjectRegistry& instance();

    /**
     * @brief Check if registry has been destroyed (for static destruction guards)
     */
    static bool is_destroyed();

    /**
     * @brief How long an entry's subjects live, which decides what a printer switch does
     *
     * A printer switch tears printer state down and rebuilds it inside the same process,
     * but only code on the rebuild path runs again: boot-time component registration
     * (xml_registration.cpp) and anything behind a process-once guard does not. An entry
     * whose init will not run again must not be removed by a switch.
     */
    enum class Lifetime {
        /// Runs and is removed at a switch; its init runs again on the rebuild path and
        /// re-registers. The default.
        Printer,
        /// Untouched by a switch: the subjects stay registered and live until exit. For
        /// subjects that back XML components registered once at boot.
        ///
        /// Only XML bindings and C++ setters may observe a Process subject, never an
        /// ObserverGuard owned by printer-scoped code: a switch invalidates every guard,
        /// so such a guard would release() and leave its observer on a live subject,
        /// calling into the destroyed owner on the next change.
        Process,
        /// Runs at a switch AND stays registered, so it runs again at the next switch and
        /// at exit. For cleanup of per-widget state whose registration is process-once
        /// but whose widgets observe printer subjects. The callback must be re-runnable.
        ProcessRunOnSwitch,
    };

    /// Which teardown is running.
    enum class Scope {
        PrinterSwitch, ///< printer state is rebuilt afterwards; LVGL stays up
        ProcessExit,   ///< lv_deinit() follows; every entry runs
    };

    /**
     * @brief Register a deinit callback for a singleton's subjects
     * @param name Singleton name for logging
     * @param deinit_fn Function to call during deinit_all()
     * @param lifetime What a printer switch does to the entry (see Lifetime)
     */
    void register_deinit(const char* name, helix::SharedCallback<void()> deinit_fn,
                         Lifetime lifetime = Lifetime::Printer);

    /**
     * @brief Deinitialize registered subjects in reverse registration order
     *
     * Called from teardown AFTER panel destruction but BEFORE lv_deinit() or the printer
     * rebuild. This disconnects observers from subjects, preventing crashes when the widgets
     * that were observing them are deleted. Which entries run, and which stay registered,
     * depends on @p scope and each entry's Lifetime.
     */
    void deinit_all(Scope scope = Scope::ProcessExit);

    /**
     * @brief Apply one teardown scope to a single registered entry by name
     *
     * Test hook. Exercises one subsystem's teardown without firing the whole
     * registry — deinit_all() in a test binary would also run entries left
     * behind by earlier fixtures (some of which capture destroyed objects).
     * The entry runs and stays or goes exactly as deinit_all(@p scope) would treat it.
     *
     * @param name Name used at registration
     * @return true if an entry was found
     */
    bool deinit_one(const char* name, Scope scope = Scope::ProcessExit);

    /**
     * @brief Drop a registered entry without running it
     *
     * For a registrant that is not a process-lifetime singleton (a test
     * fixture's own PrinterState): its callback captures the object, so the
     * entry has to leave the registry before the object does.
     *
     * @return true if an entry was found and dropped
     */
    bool unregister(const char* name);

    /**
     * @brief Clear all registered entries without running callbacks
     *
     * Used during soft restart (printer switching) after deinit_all() has run,
     * to ensure no stale entries remain before re-initialization registers new ones.
     */
    void clear();

    /**
     * @brief Get count of registered singletons (for testing/debugging)
     */
    size_t count() const {
        return deinitializers_.size();
    }

    /**
     * @brief Registered names in registration order (for testing/debugging)
     *
     * deinit_all() walks this order in reverse, so the position of an entry is
     * the contract: it is what makes late registrants tear down before the ones
     * whose subjects they observe. A test that cares about ordering reads it
     * here rather than firing deinit_all(), which in a test binary would also
     * run entries left behind by earlier fixtures.
     */
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(deinitializers_.size());
        for (const DeinitEntry& e : deinitializers_) {
            out.push_back(e.name);
        }
        return out;
    }

  private:
    friend class ScopedSubjectRegistryIsolation;

    StaticSubjectRegistry() = default;
    ~StaticSubjectRegistry();

    // Non-copyable
    StaticSubjectRegistry(const StaticSubjectRegistry&) = delete;
    StaticSubjectRegistry& operator=(const StaticSubjectRegistry&) = delete;

    struct DeinitEntry {
        std::string name;
        helix::SharedCallback<void()> deinit_fn;
        Lifetime lifetime = Lifetime::Printer;
    };

    std::vector<DeinitEntry> deinitializers_;
};
