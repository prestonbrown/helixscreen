// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_panel_base.h
 * @brief Abstract base class for all UI panels with lifecycle hooks
 *
 * @pattern Two-phase init (init_subjects -> XML -> setup); RAII observer cleanup
 * @threading Main thread only
 *
 * @see ui_panel_bed_mesh.cpp for gold standard implementation
 */

#pragma once

#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

// Forward declarations
namespace helix {
class PrinterState;
}
class IMoonrakerAPI;

// Include for SubjectManager (needed for deinit_subjects_base)
#include "subject_managed_panel.h"

/**
 * @brief Abstract base class for all UI panels
 *
 * Provides shared infrastructure for panels including:
 * - Dependency injection (PrinterState, IMoonrakerAPI)
 * - RAII observer management (automatic cleanup in destructor)
 * - Move semantics support for std::unique_ptr ownership
 * - Two-phase initialization (init_subjects -> XML creation -> setup)
 *
 * @implements IPanelLifecycle for NavigationManager dispatch
 *
 * ## Usage Pattern:
 *
 * @code
 * class MyPanel : public PanelBase {
 * public:
 *     MyPanel(PrinterState& ps, IMoonrakerAPI* api) : PanelBase(ps, api) {}
 *
 *     void init_subjects() override {
 *         // Register LVGL subjects for XML binding
 *     }
 *
 *     void setup(lv_obj_t* panel, lv_obj_t* parent) override {
 *         PanelBase::setup(panel, parent);  // Store references
 *         // Wire up event handlers, create widgets
 *     }
 *
 *     const char* get_name() const override { return "My Panel"; }
 *     const char* get_xml_component_name() const override { return "my_panel"; }
 *
 * protected:
 *     // Use register_observer() for RAII cleanup
 *     lv_observer_t* my_observer_ = nullptr;
 * };
 * @endcode
 *
 * ## Observer Lifecycle:
 *
 * Observers registered with register_observer() are automatically removed
 * in the destructor. This prevents use-after-free crashes when panels are
 * destroyed while subjects still exist.
 *
 * @see TemperatureService for a complete implementation example
 */
class PanelBase : public IPanelLifecycle {
  public:
    /**
     * @brief Construct panel with injected dependencies
     *
     * @param printer_state Reference to PrinterState singleton
     * @param api Pointer to IMoonrakerAPI (may be nullptr if not connected)
     */
    PanelBase(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    /**
     * @brief Virtual destructor - cleans up registered observers
     */
    virtual ~PanelBase();

    // Non-copyable (observer handles cannot be shared)
    PanelBase(const PanelBase&) = delete;
    PanelBase& operator=(const PanelBase&) = delete;

    // Movable (transfers observer ownership)
    PanelBase(PanelBase&& other) noexcept;
    PanelBase& operator=(PanelBase&& other) noexcept;

    //
    // === Core Lifecycle (must implement) ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * MUST be called BEFORE lv_xml_create() for components that bind to subjects.
     * Safe to call multiple times - subsequent calls should be ignored.
     *
     * @note Implementations should set subjects_initialized_ = true
     */
    virtual void init_subjects() = 0;

    /**
     * @brief Setup panel after XML creation
     *
     * Called after lv_xml_create() returns. Wire up event handlers,
     * create child widgets, configure observers here.
     *
     * @param panel Root object of the panel (from lv_xml_create)
     * @param parent_screen Parent screen for navigation purposes
     */
    virtual void setup(lv_obj_t* panel, lv_obj_t* parent_screen);

    /**
     * @brief Get human-readable panel name
     *
     * Used in logging and debugging.
     *
     * @return Panel name (e.g., "Motion Panel", "Home Panel")
     */
    const char* get_name() const override = 0;

    /**
     * @brief Get XML component name for lv_xml_create()
     *
     * Must match the filename in ui_xml/ (without .xml extension).
     *
     * @return Component name (e.g., "motion_panel", "home_panel")
     */
    virtual const char* get_xml_component_name() const = 0;

    //
    // === Optional Lifecycle Hooks ===
    //

    /**
     * @brief Called when panel becomes visible
     *
     * Override to start animations, refresh data, or resume timers.
     * Default implementation does nothing.
     */
    void on_activate() override {}

    /**
     * @brief Called when panel is hidden
     *
     * Override to pause animations, stop timers, or cleanup temporary state.
     * Default implementation does nothing.
     */
    void on_deactivate() override {}

    /**
     * @brief Rebuild this panel's widget tree from its XML component
     *
     * Tears down the current panel widget (observers cleaned up, widget
     * async-deleted) and creates a new one via lv_xml_create. Updates
     * NavigationManager's cached pointer. Restores visibility to match
     * the old widget.
     *
     * Called by NavigationManager::rebuild_active_views() after XML hot-
     * reload. Dev-only path; gated by HELIX_HOT_RELOAD.
     *
     * @return true if rebuilt, false if skipped (no widget yet, not registered)
     */
    bool rebuild() override;

    //
    // === Public API ===
    //

    /**
     * @brief Update IMoonrakerAPI pointer
     *
     * Call when API becomes available after initial construction,
     * or when reconnecting to a different printer.
     *
     * @param api New API pointer (may be nullptr)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Get root panel object
     *
     * @return Panel object, or nullptr if not yet setup
     */
    lv_obj_t* get_panel() const {
        return panel_;
    }

    /**
     * @brief Check if subjects have been initialized
     *
     * @return true if init_subjects() was called
     */
    bool are_subjects_initialized() const {
        return subjects_initialized_;
    }

  protected:
    //
    // === Injected Dependencies ===
    //

    helix::PrinterState& printer_state_;
    IMoonrakerAPI* api_;

    //
    // === Panel State ===
    //

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    bool subjects_initialized_ = false;

    //
    // === Subject Init/Deinit Guards ===
    //

    /**
     * @brief Execute init function with guard against double initialization
     *
     * Wraps the actual subject initialization code with a guard that prevents
     * double initialization and logs appropriately.
     *
     * @tparam Func Callable type (typically lambda)
     * @param init_func Function to execute if not already initialized
     * @return true if initialization was performed, false if already initialized
     *
     * Example:
     * @code
     * void MyPanel::init_subjects() {
     *     init_subjects_guarded([this]() {
     *         UI_MANAGED_SUBJECT_INT(my_subject_, 0, "my_subject", subjects_);
     *     });
     * }
     * @endcode
     */
    template <typename Func> bool init_subjects_guarded(Func&& init_func) {
        if (subjects_initialized_) {
            spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
            return false;
        }
        init_func();
        subjects_initialized_ = true;
        spdlog::debug("[{}] Subjects initialized", get_name());
        return true;
    }

    /**
     * @brief Deinitialize subjects via SubjectManager with guard
     *
     * Checks subjects_initialized_ flag before deinitializing.
     * Resets the flag after cleanup.
     *
     * @param subjects Reference to the panel's SubjectManager
     *
     * Example:
     * @code
     * void MyPanel::deinit_subjects() {
     *     deinit_subjects_base(subjects_);
     * }
     * @endcode
     */
    void deinit_subjects_base(class SubjectManager& subjects) {
        if (!subjects_initialized_) {
            return;
        }
        subjects.deinit_all();
        subjects_initialized_ = false;
        spdlog::trace("[{}] Subjects deinitialized", get_name());
    }

    //
    // === Observer Management ===
    //

    /**
     * @brief Register an observer for automatic cleanup
     *
     * Call this after lv_subject_add_observer() to ensure the observer
     * is removed in the destructor. Prevents use-after-free crashes.
     *
     * @param observer Observer handle from lv_subject_add_observer()
     *
     * @note Null observers are safely ignored
     */
    void register_observer(lv_observer_t* observer);

    /**
     * @brief Remove all registered observers
     *
     * Called automatically by destructor. Can also be called manually
     * if panel needs to re-subscribe to different subjects.
     */
    void cleanup_observers();

  private:
    std::vector<lv_observer_t*> observers_;
};
