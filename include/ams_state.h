// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "ams_backend.h"
#include "ams_types.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <memory>
#include <mutex>
#include <vector>

// Forward declarations
class MoonrakerAPI;
namespace helix {
class MoonrakerClient;
}

namespace helix {
class PrinterDiscovery;
}

/**
 * @file ams_state.h
 * @brief LVGL reactive state management for AMS UI binding
 *
 * Provides LVGL subjects that automatically update bound XML widgets
 * when AMS state changes. Bridges the AmsBackend to the UI layer.
 *
 * Usage:
 * 1. Call init_subjects() BEFORE creating XML components
 * 2. Call set_backend() to connect to an AMS backend
 * 3. Subjects auto-update when backend emits events
 *
 * Thread Safety:
 * All public methods are thread-safe. Subject updates are posted
 * to LVGL's thread via lv_async_call when called from background threads.
 */
class AmsState {
  public:
    /**
     * @brief Maximum number of slots supported for per-slot subjects
     *
     * Per-slot subjects (color, status) are allocated statically.
     * Systems with more slots will only have subjects for the first MAX_SLOTS.
     */
    static constexpr int MAX_SLOTS = 16;

    /// @name Dryer Constants
    /// @{
    static constexpr int DEFAULT_DRYER_TEMP_C = 55;        ///< Default dryer temp (PETG)
    static constexpr int DEFAULT_DRYER_DURATION_MIN = 240; ///< Default duration (4 hours)
    static constexpr int MIN_DRYER_TEMP_C = 35;            ///< Minimum dryer temperature
    static constexpr int MAX_DRYER_TEMP_C = 70;            ///< Maximum dryer temperature
    static constexpr int MIN_DRYER_DURATION_MIN = 30;      ///< Minimum duration (30 min)
    static constexpr int MAX_DRYER_DURATION_MIN = 720;     ///< Maximum duration (12 hours)
    static constexpr int DRYER_TEMP_STEP_C = 5;            ///< Temperature adjustment step
    static constexpr int DRYER_DURATION_STEP_MIN = 30;     ///< Duration adjustment step
    /// @}

    /**
     * @brief Get the singleton instance
     * @return Reference to the global AmsState instance
     */
    static AmsState& instance();

    /**
     * @brief Map AMS system/type name to logo image path
     *
     * Maps both generic firmware names (Happy Hare, AFC) and specific hardware
     * names (ERCF, Box Turtle, etc.) to their logo assets. Performs case-insensitive
     * matching and strips common suffixes like " (mock)".
     *
     * @param type_name System or type name (e.g., "AFC", "Happy Hare", "ERCF")
     * @return Logo asset path or nullptr if no matching logo
     */
    static const char* get_logo_path(const std::string& type_name);

    // Non-copyable, non-movable singleton
    AmsState(const AmsState&) = delete;
    AmsState& operator=(const AmsState&) = delete;

    /**
     * @brief Initialize all LVGL subjects
     *
     * MUST be called BEFORE creating XML components that bind to these subjects.
     * Can be called multiple times safely - subsequent calls are ignored.
     *
     * @param register_xml If true, registers subjects with LVGL XML system (default).
     *                     Set to false in tests to avoid XML observer creation.
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects for clean shutdown
     *
     * Must be called before lv_deinit() to prevent observer corruption.
     * Disconnects all observers from subjects.
     */
    void deinit_subjects();

    /**
     * @brief Initialize AMS backend from discovered hardware
     *
     * Called after Moonraker discovery completes. If the printer has an MMU system
     * (AFC/Box Turtle, Happy Hare, etc.), creates and starts the appropriate backend.
     * Does nothing if no MMU is detected or if already in mock mode.
     *
     * @param hardware Discovered printer hardware
     * @param api MoonrakerAPI instance for making API calls
     * @param client helix::MoonrakerClient instance for WebSocket communication
     */
    void init_backend_from_hardware(const helix::PrinterDiscovery& hardware, MoonrakerAPI* api,
                                    helix::MoonrakerClient* client);

    /**
     * @brief Initialize backends from all detected AMS/filament systems
     *
     * Called after Moonraker discovery completes. Creates a backend for each
     * detected system (MMU, tool changer, AFC, etc.). Supports multiple
     * concurrent backends for printers with multiple filament systems.
     *
     * @param hardware Discovered printer hardware
     * @param api MoonrakerAPI instance for making API calls
     * @param client helix::MoonrakerClient instance for WebSocket communication
     */
    void init_backends_from_hardware(const helix::PrinterDiscovery& hardware, MoonrakerAPI* api,
                                     helix::MoonrakerClient* client);

    /**
     * @brief Set the AMS backend
     *
     * Connects to the backend and starts receiving state updates.
     * Automatically registers event callback to sync state.
     *
     * @param backend Backend instance (ownership transferred)
     */
    void set_backend(std::unique_ptr<AmsBackend> backend);

    /**
     * @brief Get the primary backend (index 0)
     * @return Pointer to backend (may be nullptr)
     */
    [[nodiscard]] AmsBackend* get_backend() const;

    /**
     * @brief Add a backend to the multi-backend list
     * @param backend Backend instance (ownership transferred)
     * @return Index of the added backend
     */
    int add_backend(std::unique_ptr<AmsBackend> backend);

    /**
     * @brief Get backend by index
     * @param index Backend index (0 = primary)
     * @return Pointer to backend or nullptr if out of range
     */
    [[nodiscard]] AmsBackend* get_backend(int index) const;

    /**
     * @brief Get the number of registered backends
     * @return Number of backends
     */
    [[nodiscard]] int backend_count() const;

    /**
     * @brief Remove and stop all backends
     */
    void clear_backends();

    /**
     * @brief Check if AMS is available
     * @return true if backend is set and AMS type is not NONE
     */
    [[nodiscard]] bool is_available() const;

    /**
     * @brief Set Moonraker API for Spoolman integration
     *
     * When set, AmsState will automatically call set_active_spool() when
     * a slot with a Spoolman ID becomes loaded. Pass nullptr to disable.
     *
     * @param api MoonrakerAPI instance (not owned)
     */
    void set_moonraker_api(MoonrakerAPI* api);

    /**
     * @brief Set callback for mock backend gcode response injection
     *
     * Stored and applied to any mock backends when they are added.
     * In production, real backends don't use this (gcode responses come
     * through the WebSocket). Used to let mock backends simulate
     * action:prompt dialogs.
     *
     * @param callback Function that processes "// action:..." lines
     */
    void set_gcode_response_callback(std::function<void(const std::string&)> callback);

    // ========================================================================
    // System-level Subject Accessors
    // ========================================================================

    /**
     * @brief Get backend count subject
     * @return Subject holding the number of registered backends
     */
    lv_subject_t* get_backend_count_subject() {
        return &backend_count_;
    }

    /**
     * @brief Get active backend subject
     * @return Subject holding index of the currently selected backend
     */
    lv_subject_t* get_active_backend_subject() {
        return &active_backend_;
    }

    /**
     * @brief Get the active backend index
     * @return Currently selected backend index
     */
    [[nodiscard]] int active_backend_index() const;

    /**
     * @brief Set the active backend index
     * @param index Backend index to make active (bounds-checked)
     */
    void set_active_backend(int index);

    /**
     * @brief Get AMS type subject
     * @return Subject holding AmsType enum as int (0=none, 1=happy_hare, 2=afc)
     */
    lv_subject_t* get_ams_type_subject() {
        return &ams_type_;
    }

    /**
     * @brief Get current action subject
     * @return Subject holding AmsAction enum as int
     */
    lv_subject_t* get_ams_action_subject() {
        return &ams_action_;
    }

    /**
     * @brief Get action detail string subject
     * @return Subject holding current operation description
     */
    lv_subject_t* get_ams_action_detail_subject() {
        return &ams_action_detail_;
    }

    /**
     * @brief Get system name subject
     * @return Subject holding AMS system display name (e.g., "Happy Hare", "AFC")
     */
    lv_subject_t* get_ams_system_name_subject() {
        return &ams_system_name_;
    }

    /**
     * @brief Get system logo path subject
     * @return Subject holding logo image path (e.g., "A:assets/images/ams/ercf_64.png")
     */
    lv_subject_t* get_ams_system_logo_subject() {
        return &ams_system_logo_;
    }

    /**
     * @brief Get current slot subject
     * @return Subject holding current slot index (-1 if none)
     */
    lv_subject_t* get_current_slot_subject() {
        return &current_slot_;
    }

    /**
     * @brief Get pending target slot subject (for tool change animations)
     * @return Subject holding target slot index (-1 if no swap in progress)
     */
    lv_subject_t* get_pending_target_slot_subject() {
        return &pending_target_slot_;
    }

    /**
     * @brief Set pending target slot directly from UI (for early pulse during preheat)
     * @param slot Target slot index, or -1 to clear
     */
    void set_pending_target_slot(int slot);

    /**
     * @brief Get current tool subject
     * @return Subject holding current tool index (-1 if none)
     */
    lv_subject_t* get_current_tool_subject() {
        return &ams_current_tool_;
    }

    /**
     * @brief Get current tool text subject
     * @return Subject holding formatted tool string (e.g., "T0", "T1", or "---")
     */
    lv_subject_t* get_current_tool_text_subject() {
        return &ams_current_tool_text_;
    }

    /**
     * @brief Get toolchange visibility subject (1=visible, 0=hidden)
     * Non-zero when the filament backend (AFC, Happy Hare) reports expected tool changes.
     */
    lv_subject_t* get_toolchange_visible_subject() {
        return &toolchange_visible_;
    }

    /**
     * @brief Get toolchange text subject ("2 / 5" formatted)
     * 1-based display: current_toolchange+1 of number_of_toolchanges.
     * Empty string when not applicable.
     */
    lv_subject_t* get_toolchange_text_subject() {
        return &toolchange_text_;
    }

    /**
     * @brief Get filament loaded subject
     * @return Subject holding 0 (not loaded) or 1 (loaded)
     */
    lv_subject_t* get_filament_loaded_subject() {
        return &filament_loaded_;
    }

    /**
     * @brief Get bypass active subject
     *
     * Bypass mode allows external spool to feed directly to toolhead,
     * bypassing the MMU/hub system.
     *
     * @return Subject holding 0 (bypass inactive) or 1 (bypass active)
     */
    lv_subject_t* get_bypass_active_subject() {
        return &bypass_active_;
    }

    /**
     * @brief Get external spool color subject
     * @return Subject holding 0xRRGGBB color or 0 if no external spool assigned
     */
    lv_subject_t* get_external_spool_color_subject() {
        return &external_spool_color_;
    }

    /**
     * @brief Get supports bypass subject
     * @return Subject holding 1 if backend supports bypass, 0 otherwise
     */
    lv_subject_t* get_supports_bypass_subject() {
        return &supports_bypass_;
    }

    /**
     * @brief Get slot count subject
     * @return Subject holding total number of slots
     */
    lv_subject_t* get_slot_count_subject() {
        return &ams_slot_count_;
    }

    /**
     * @brief Get slots version subject
     *
     * Incremented whenever slot data changes. UI can observe this
     * to know when to refresh slot displays.
     *
     * @return Subject holding version counter
     */
    lv_subject_t* get_slots_version_subject() {
        return &slots_version_;
    }

    // ========================================================================
    // Filament Path Visualization Subjects
    // ========================================================================

    /**
     * @brief Get path topology subject
     * @return Subject holding PathTopology enum as int (0=linear, 1=hub)
     */
    lv_subject_t* get_path_topology_subject() {
        return &path_topology_;
    }

    /**
     * @brief Get path active slot subject
     * @return Subject holding slot index whose path is being shown (-1=none)
     */
    lv_subject_t* get_path_active_slot_subject() {
        return &path_active_slot_;
    }

    /**
     * @brief Get path filament segment subject
     *
     * Indicates where the filament currently is along the path.
     *
     * @return Subject holding PathSegment enum as int
     */
    lv_subject_t* get_path_filament_segment_subject() {
        return &path_filament_segment_;
    }

    /**
     * @brief Get path error segment subject
     *
     * Indicates which segment has an error (for highlighting).
     *
     * @return Subject holding PathSegment enum as int (NONE if no error)
     */
    lv_subject_t* get_path_error_segment_subject() {
        return &path_error_segment_;
    }

    /**
     * @brief Get path animation progress subject
     *
     * Used for load/unload animations.
     *
     * @return Subject holding progress 0-100
     */
    lv_subject_t* get_path_anim_progress_subject() {
        return &path_anim_progress_;
    }

    // ========================================================================
    // Dryer Subject Accessors (for AMS systems with integrated drying)
    // ========================================================================

    /**
     * @brief Get dryer supported subject
     * @return Subject holding 1 if dryer is available, 0 otherwise
     */
    lv_subject_t* get_dryer_supported_subject() {
        return &dryer_supported_;
    }

    /**
     * @brief Get dryer active subject
     * @return Subject holding 1 if currently drying, 0 otherwise
     */
    lv_subject_t* get_dryer_active_subject() {
        return &dryer_active_;
    }

    /**
     * @brief Get dryer current temperature subject
     * @return Subject holding current temp in degrees C (integer)
     */
    lv_subject_t* get_dryer_current_temp_subject() {
        return &dryer_current_temp_;
    }

    /**
     * @brief Get dryer target temperature subject
     * @return Subject holding target temp in degrees C (integer, 0 = off)
     */
    lv_subject_t* get_dryer_target_temp_subject() {
        return &dryer_target_temp_;
    }

    /**
     * @brief Get dryer remaining minutes subject
     * @return Subject holding minutes remaining
     */
    lv_subject_t* get_dryer_remaining_min_subject() {
        return &dryer_remaining_min_;
    }

    /**
     * @brief Get dryer progress percentage subject
     * @return Subject holding 0-100 progress, or -1 if not drying
     */
    lv_subject_t* get_dryer_progress_pct_subject() {
        return &dryer_progress_pct_;
    }

    /**
     * @brief Get dryer current temperature text subject
     * @return Subject holding formatted temp string (e.g., "45C")
     */
    lv_subject_t* get_dryer_current_temp_text_subject() {
        return &dryer_current_temp_text_;
    }

    /**
     * @brief Get dryer target temperature text subject
     * @return Subject holding formatted temp string (e.g., "55C" or "---")
     */
    lv_subject_t* get_dryer_target_temp_text_subject() {
        return &dryer_target_temp_text_;
    }

    /**
     * @brief Get dryer time remaining text subject
     * @return Subject holding formatted time string (e.g., "2:30 left" or "")
     */
    lv_subject_t* get_dryer_time_text_subject() {
        return &dryer_time_text_;
    }

    /**
     * @brief Get dryer modal temperature text subject
     * @return Subject holding formatted temp string (e.g., "55°C")
     */
    lv_subject_t* get_dryer_modal_temp_text_subject() {
        return &dryer_modal_temp_text_;
    }

    /**
     * @brief Get dryer modal duration text subject
     * @return Subject holding formatted duration string (e.g., "4h", "4h 30m")
     */
    lv_subject_t* get_dryer_modal_duration_text_subject() {
        return &dryer_modal_duration_text_;
    }

    /**
     * @brief Get current modal target temperature
     * @return Temperature in degrees C
     */
    [[nodiscard]] int get_modal_target_temp() const {
        return modal_target_temp_c_;
    }

    /**
     * @brief Get current modal duration
     * @return Duration in minutes
     */
    [[nodiscard]] int get_modal_duration_min() const {
        return modal_duration_min_;
    }

    /**
     * @brief Adjust modal target temperature
     * @param delta_c Change in degrees (+5 or -5)
     */
    void adjust_modal_temp(int delta_c);

    /**
     * @brief Adjust modal duration
     * @param delta_min Change in minutes (+30 or -30)
     */
    void adjust_modal_duration(int delta_min);

    /**
     * @brief Set modal values from a preset
     * @param temp_c Target temperature
     * @param duration_min Duration in minutes
     */
    void set_modal_preset(int temp_c, int duration_min);

    /**
     * @brief Update modal text subjects from current values
     */
    void update_modal_text_subjects();

    // ========================================================================
    // Currently Loaded Display Subjects (for reactive "Currently Loaded" card)
    // ========================================================================

    /**
     * @brief Get current material text subject
     * @return Subject holding material/color text (e.g., "Red PLA", "External", "---")
     */
    lv_subject_t* get_current_material_text_subject() {
        return &current_material_text_;
    }

    /**
     * @brief Get current slot text subject
     * @return Subject holding slot text (e.g., "Slot 1", "Bypass", "None")
     */
    lv_subject_t* get_current_slot_text_subject() {
        return &current_slot_text_;
    }

    /**
     * @brief Get current weight text subject
     * @return Subject holding weight text (e.g., "450g", "")
     */
    lv_subject_t* get_current_weight_text_subject() {
        return &current_weight_text_;
    }

    /**
     * @brief Get current has weight subject
     * @return Subject holding 1 if weight data available, 0 otherwise (for visibility binding)
     */
    lv_subject_t* get_current_has_weight_subject() {
        return &current_has_weight_;
    }

    /**
     * @brief Get current color subject
     * @return Subject holding 0xRRGGBB color value for the swatch
     */
    lv_subject_t* get_current_color_subject() {
        return &current_color_;
    }

    // ========================================================================
    // Per-Slot Subject Accessors
    // ========================================================================

    /**
     * @brief Get slot color subject for a specific slot
     *
     * Holds 0xRRGGBB color value for UI display.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_color_subject(int slot_index);

    /**
     * @brief Get slot status subject for a specific slot
     *
     * Holds SlotStatus enum as int.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_status_subject(int slot_index);

    /**
     * @brief Get slot color subject for a specific backend and slot
     *
     * For backend_index 0, delegates to existing flat slot subjects.
     * For secondary backends, returns from per-backend subject storage.
     *
     * @param backend_index Backend index (0 = primary)
     * @param slot_index Slot index within that backend
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_color_subject(int backend_index, int slot_index);

    /**
     * @brief Get slot status subject for a specific backend and slot
     *
     * For backend_index 0, delegates to existing flat slot subjects.
     * For secondary backends, returns from per-backend subject storage.
     *
     * @param backend_index Backend index (0 = primary)
     * @param slot_index Slot index within that backend
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_status_subject(int backend_index, int slot_index);

    // ========================================================================
    // Direct State Update (called by backend event handler)
    // ========================================================================

    /**
     * @brief Update state from backend system info
     *
     * Called internally when backend emits STATE_CHANGED event.
     * Updates all subjects from the current backend state.
     */
    void sync_from_backend();

    /**
     * @brief Sync state from a specific backend by index
     *
     * For backend_index 0, delegates to sync_from_backend().
     * For secondary backends, updates per-backend slot subjects only.
     *
     * @param backend_index Backend index to sync
     */
    void sync_backend(int backend_index);

    /**
     * @brief Update a single slot's subjects for a specific backend
     *
     * For backend_index 0, delegates to update_slot().
     * For secondary backends, updates per-backend slot subjects only.
     *
     * @param backend_index Backend index
     * @param slot_index Slot that changed
     */
    void update_slot_for_backend(int backend_index, int slot_index);

    /**
     * @brief Update a single slot's subjects
     *
     * Called when backend emits SLOT_CHANGED event.
     *
     * @param slot_index Slot that changed
     */
    void update_slot(int slot_index);

    /**
     * @brief Update dryer subjects from backend dryer info
     *
     * Called when backend reports dryer state changes.
     * Updates all dryer-related subjects for UI binding.
     */
    void sync_dryer_from_backend();

    /**
     * @brief Update "Currently Loaded" display subjects from backend
     *
     * Called when current slot changes to update the reactive UI.
     * Updates material text, slot text, weight info, and color subjects.
     */
    void sync_current_loaded_from_backend();

    /**
     * @brief Set action detail text directly (for UI-managed states)
     *
     * Used when UI is managing a process (like preheat) that the backend
     * doesn't know about. Updates the ams_action_detail_ subject.
     *
     * @param detail The status text to display (e.g., "Heating to 230°C...")
     */
    void set_action_detail(const std::string& detail);

    /**
     * @brief Get external spool info from persistent storage
     * @return SlotInfo or nullopt if not set
     */
    std::optional<SlotInfo> get_external_spool_info() const;

    /**
     * @brief Set external spool info and update color subject
     * @param info SlotInfo with filament data
     */
    void set_external_spool_info(const SlotInfo& info);

    /**
     * @brief Clear external spool info
     */
    void clear_external_spool_info();

    /**
     * @brief Set the current AMS action state directly
     *
     * Used by UI to indicate operation in progress (e.g., during UI-managed preheat
     * before backend starts). Triggers XML binding updates for action-dependent UI.
     *
     * @param action The action state to set
     */
    void set_action(AmsAction action);

    /**
     * @brief Check if a filament operation (load/unload) is currently active
     *
     * Used by FilamentSensorManager to suppress spurious sensor toasts while
     * filament is being intentionally moved through sensors.
     *
     * @return true if AMS is actively loading, unloading, or performing related ops
     */
    bool is_filament_operation_active();

    // ========================================================================
    // Spoolman Weight Polling
    // ========================================================================

    /**
     * @brief Refresh weights from Spoolman for all linked slots
     *
     * Queries Spoolman for updated weight info for each slot that has a
     * spoolman_id > 0. Updates remaining_weight_g and total_weight_g in
     * the backend's slot data.
     */
    void refresh_spoolman_weights();

    /**
     * @brief Start periodic Spoolman weight polling
     *
     * Uses a reference count pattern - multiple panels can call start_spoolman_polling()
     * and the timer is only created on the first call. The timer will refresh weights
     * every 30 seconds.
     */
    void start_spoolman_polling();

    /**
     * @brief Stop periodic Spoolman weight polling
     *
     * Decrements the reference count. The timer is only deleted when the count
     * reaches zero (all panels that started polling have stopped).
     */
    void stop_spoolman_polling();

    /**
     * @brief Bump the slots version counter to trigger UI refresh
     *
     * Call after modifying slot data (weights, endless spool config, etc.)
     * to notify observers and redraw the AMS panel.
     */
    void bump_slots_version();

  private:
    friend class AmsStateTestAccess;

    AmsState();
    ~AmsState();

    /**
     * @brief Handle backend event callback
     * @param backend_index Index of the backend that emitted the event
     * @param event Event name
     * @param data Event data
     */
    void on_backend_event(int backend_index, const std::string& event, const std::string& data);

    /**
     * @brief Probe for ValgACE via REST endpoint
     *
     * Makes an async REST call to /server/ace/info. If successful,
     * creates ValgACE backend via lv_async_call to maintain thread safety.
     *
     * @param api MoonrakerAPI instance for REST calls
     * @param client helix::MoonrakerClient instance for the backend
     */
    void probe_valgace(MoonrakerAPI* api, helix::MoonrakerClient* client);

    /**
     * @brief Create and start ValgACE backend
     *
     * Called on main thread after successful ValgACE probe.
     * Must be called from LVGL thread context.
     *
     * @param api MoonrakerAPI instance
     * @param client helix::MoonrakerClient instance
     */
    void create_valgace_backend(MoonrakerAPI* api, helix::MoonrakerClient* client);

    /// Per-backend slot subject storage for secondary backends (index > 0)
    struct BackendSlotSubjects {
        std::vector<lv_subject_t> colors;
        std::vector<lv_subject_t> statuses;
        int slot_count = 0;
        void init(int count);
        void deinit();
    };

    mutable std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<AmsBackend>> backends_;
    std::vector<BackendSlotSubjects> secondary_slot_subjects_;
    bool initialized_ = false;

    // Moonraker API for Spoolman integration
    MoonrakerAPI* api_ = nullptr;
    int last_synced_spoolman_id_ = 0; ///< Track to avoid duplicate set_active_spool calls

    // Spoolman weight polling
    lv_timer_t* spoolman_poll_timer_ = nullptr;
    int spoolman_poll_refcount_ = 0;

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // Backend selector subjects
    lv_subject_t backend_count_;
    lv_subject_t active_backend_;

    // System-level subjects
    lv_subject_t ams_type_;
    lv_subject_t ams_action_;
    lv_subject_t current_slot_;
    lv_subject_t pending_target_slot_;
    lv_subject_t ams_current_tool_;
    lv_subject_t filament_loaded_;
    lv_subject_t bypass_active_;
    lv_subject_t external_spool_color_;
    lv_subject_t supports_bypass_;
    lv_subject_t ams_slot_count_;
    lv_subject_t slots_version_;

    // String subjects (need buffers)
    lv_subject_t ams_action_detail_;
    char action_detail_buf_[64];
    lv_subject_t ams_system_name_;
    char system_name_buf_[32];
    lv_subject_t ams_system_logo_;
    char system_logo_buf_[64];
    lv_subject_t ams_current_tool_text_;
    char ams_current_tool_text_buf_[16]; // "T0" to "T15" or "---"

    // Tool change progress (AFC multi-color prints)
    lv_subject_t toolchange_visible_; // 1 when swaps expected, 0 otherwise
    lv_subject_t toolchange_text_;    // "2 / 5" formatted display
    char toolchange_text_buf_[32]{};  // Buffer for formatted text

    // Filament path visualization subjects
    lv_subject_t path_topology_;
    lv_subject_t path_active_slot_;
    lv_subject_t path_filament_segment_;
    lv_subject_t path_error_segment_;
    lv_subject_t path_anim_progress_;

    // Dryer subjects (for AMS systems with integrated drying)
    lv_subject_t dryer_supported_;
    lv_subject_t dryer_active_;
    lv_subject_t dryer_current_temp_;
    lv_subject_t dryer_target_temp_;
    lv_subject_t dryer_remaining_min_;
    lv_subject_t dryer_progress_pct_;

    // Dryer text subjects (need buffers)
    lv_subject_t dryer_current_temp_text_;
    char dryer_current_temp_text_buf_[16];
    lv_subject_t dryer_target_temp_text_;
    char dryer_target_temp_text_buf_[16];
    lv_subject_t dryer_time_text_;
    char dryer_time_text_buf_[32];

    // Dryer modal editing subjects (user-adjustable values)
    lv_subject_t dryer_modal_temp_text_;
    char dryer_modal_temp_text_buf_[16];
    lv_subject_t dryer_modal_duration_text_;
    char dryer_modal_duration_text_buf_[16];
    int modal_target_temp_c_ = DEFAULT_DRYER_TEMP_C;      ///< Modal's target temp (default PETG)
    int modal_duration_min_ = DEFAULT_DRYER_DURATION_MIN; ///< Modal's duration (default)

    // Currently Loaded display subjects (reactive binding for "Currently Loaded" card)
    lv_subject_t current_material_text_;
    char current_material_text_buf_[48];
    lv_subject_t current_slot_text_;
    char current_slot_text_buf_[64];
    lv_subject_t current_weight_text_;
    char current_weight_text_buf_[16];
    lv_subject_t current_has_weight_;
    lv_subject_t current_color_;

    // Per-slot subjects (color and status)
    lv_subject_t slot_colors_[MAX_SLOTS];
    lv_subject_t slot_statuses_[MAX_SLOTS];

    // Observer for print state changes to auto-refresh Spoolman weights
    ObserverGuard print_state_observer_;

    // Stored callback for mock gcode response injection
    std::function<void(const std::string&)> gcode_response_callback_;
};
