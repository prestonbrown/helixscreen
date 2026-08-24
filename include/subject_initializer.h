// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "runtime_config.h"

#include <functional>
#include <memory>
#include <vector>

// Forward declarations
class IMoonrakerAPI;
class PrintSelectPanel;
class PrintStatusPanel;
class MotionPanel;
class BedMeshPanel;
class TemperatureService;
class UsbManager;
namespace helix {
class TemperatureController;
}
namespace helix::ui {
class PlrOfferController;
}

/**
 * @brief Initializes all reactive subjects for LVGL data binding
 *
 * SubjectInitializer orchestrates the initialization of all reactive subjects
 * in the correct dependency order. It manages observer guards for proper cleanup.
 *
 * Initialization is split into phases to allow IMoonrakerAPI injection:
 * 1. init_core_and_state() - Core subjects, PrinterState, AmsState
 * 2. init_panels(api) - Panel subjects with API injected at construction
 * 3. init_post() - Observers and utility subjects
 *
 * Usage (preferred - proper DI):
 *   SubjectInitializer subjects;
 *   subjects.init_core_and_state();
 *   // ... initialize MoonrakerManager to get API ...
 *   subjects.init_panels(api, runtime_config);
 *   subjects.init_post(runtime_config);
 *
 */
class SubjectInitializer {
  public:
    SubjectInitializer();
    ~SubjectInitializer();

    // Non-copyable, non-movable (owns observer guards)
    SubjectInitializer(const SubjectInitializer&) = delete;
    SubjectInitializer& operator=(const SubjectInitializer&) = delete;
    SubjectInitializer(SubjectInitializer&&) = delete;
    SubjectInitializer& operator=(SubjectInitializer&&) = delete;

    /**
     * @brief Initialize core subjects and state (phases 1-3)
     *
     * Initializes: app_globals, navigation, status bar, PrinterState, AmsState,
     * FilamentSensorManager. Must be called before MoonrakerManager::init()
     * so that PrinterState exists for API creation.
     */
    void init_core_and_state();

    /**
     * @brief Initialize panel subjects with API injection (phase 4)
     * @param api Pointer to the initialized IMoonrakerAPI (required)
     * @param runtime_config Runtime configuration for mock modes
     *
     * Creates all panels with the API injected at construction time.
     * Must be called after MoonrakerManager::init().
     */
    void init_panels(IMoonrakerAPI* api, const RuntimeConfig& runtime_config);

    /**
     * @brief Initialize observers and utility subjects (phases 5-7)
     * @param runtime_config Runtime configuration for mock modes
     *
     * Initializes: print completion observer, print start navigation,
     * notification system, USB manager.
     */
    void init_post(const RuntimeConfig& runtime_config);

    /**
     * @brief Check if subjects have been initialized
     */
    bool is_initialized() const {
        return m_initialized;
    }

    /**
     * @brief Get the number of observer guards managed
     */
    size_t observer_count() const {
        return m_observers.size();
    }

    /**
     * @brief Get the USB manager (owned by SubjectInitializer)
     */
    UsbManager* usb_manager() const {
        return m_usb_manager.get();
    }

    /**
     * @brief Get the TemperatureService (owned by SubjectInitializer)
     */
    TemperatureService* temp_control_panel() const {
        return m_temp_control_panel.get();
    }

    // Accessors for panels that need API injection
    PrintSelectPanel* print_select_panel() const {
        return m_print_select_panel;
    }
    PrintStatusPanel* print_status_panel() const {
        return m_print_status_panel;
    }
    MotionPanel* motion_panel() const {
        return m_motion_panel;
    }
    BedMeshPanel* bed_mesh_panel() const {
        return m_bed_mesh_panel;
    }

  private:
    // Initialization phases
    void init_core_subjects();
    void init_printer_state_subjects();
    void init_ams_subjects();
    void init_panel_subjects(IMoonrakerAPI* api);
    void init_observers();
    void init_utility_subjects();
    void init_usb_manager(const RuntimeConfig& runtime_config);

    // Observer guards (RAII cleanup on destruction)
    std::vector<ObserverGuard> m_observers;

    // Owned resources
    std::unique_ptr<UsbManager> m_usb_manager;
    std::unique_ptr<helix::ui::PlrOfferController> m_plr_offer_controller;
    std::unique_ptr<helix::TemperatureController> m_temp_controller;
    std::unique_ptr<TemperatureService> m_temp_control_panel;

    // Alive guard for USB callback — invalidated on destruction to prevent
    // use-after-free when queued callbacks fire after panel destruction
    std::shared_ptr<bool> m_usb_callback_alive = std::make_shared<bool>(true);

    // Panels that need deferred API injection (not owned)
    PrintSelectPanel* m_print_select_panel = nullptr;
    PrintStatusPanel* m_print_status_panel = nullptr;
    MotionPanel* m_motion_panel = nullptr;
    BedMeshPanel* m_bed_mesh_panel = nullptr;

    bool m_initialized = false;
};
