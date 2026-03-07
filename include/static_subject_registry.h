// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
     * @brief Register a deinit callback for a singleton's subjects
     * @param name Singleton name for logging
     * @param deinit_fn Function to call during deinit_all()
     */
    void register_deinit(const char* name, std::function<void()> deinit_fn);

    /**
     * @brief Deinitialize all registered subjects in reverse registration order
     *
     * Called from Application::shutdown() AFTER panel destruction but BEFORE lv_deinit().
     * This disconnects all observers from subjects, preventing crashes when lv_deinit()
     * deletes widgets that were observing these subjects.
     */
    void deinit_all();

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

  private:
    StaticSubjectRegistry() = default;
    ~StaticSubjectRegistry();

    // Non-copyable
    StaticSubjectRegistry(const StaticSubjectRegistry&) = delete;
    StaticSubjectRegistry& operator=(const StaticSubjectRegistry&) = delete;

    struct DeinitEntry {
        std::string name;
        std::function<void()> deinit_fn;
    };

    std::vector<DeinitEntry> deinitializers_;
};
