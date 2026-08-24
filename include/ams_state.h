// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "ams_backend.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "async_lifetime_guard.h"
#include "filament_consumption_tracker.h"
#include "filament_mapper.h"
#include "lvgl/lvgl.h"
#include "moonraker_error.h"
#include "subject_managed_panel.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations
class IMoonrakerAPI;
namespace helix {
class IMoonrakerClient;
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

    /**
     * @brief Maximum number of AMS units supported for per-unit subjects
     *
     * Per-unit subjects (temperature, humidity, environment indicator) are
     * allocated statically, one set per unit, and registered under
     * ams_unit_<i>_* / ams_env_ind_<i>_* names. Eight matches the widest rig the
     * AMS system-path canvas draws, so every unit the path shows also has a
     * badge to bind.
     *
     * A unit past the cap still gets a card; its environment indicator binds the
     * always-off placeholders below instead of names nothing registered — see
     * env_indicator_subject_names().
     */
    static constexpr int MAX_UNITS = 8;

    /// Always-0 int subject bound by unit cards past MAX_UNITS. Keeps the
    /// environment badge hidden rather than naming a subject that does not exist
    /// (which the XML parser reports once per binding, seven times per card).
    static constexpr const char* ENV_IND_OFF_FLAG_SUBJECT = "ams_env_ind_off_flag";

    /// Always-empty string subject, same purpose as ENV_IND_OFF_FLAG_SUBJECT.
    static constexpr const char* ENV_IND_OFF_TEXT_SUBJECT = "ams_env_ind_off_text";

    /// Fully-expanded XML subject names for one unit card's environment indicator
    /// (the seven type="subject" props of ams_unit_card / ams_environment_indicator).
    struct EnvIndicatorSubjectNames {
        std::string temp_text;
        std::string humidity_text;
        std::string humidity_status;
        std::string humidity_visible;
        std::string visible;
        std::string drying_active;
        std::string drying_text;
    };

    /**
     * @brief Subject names a unit card binds its environment indicator to
     * @param unit_index 0-based unit index; may exceed MAX_UNITS
     * @return Names guaranteed to be registered once init_subjects() has run
     *
     * Units below the cap get their own ams_env_ind_<i>_* set. Anything at or
     * past it has no per-unit subjects, so it gets the always-off placeholders:
     * the card renders with its badge hidden, which beats both binding names
     * that do not exist and showing unit 0's readings under another unit's name.
     */
    [[nodiscard]] static EnvIndicatorSubjectNames env_indicator_subject_names(int unit_index);

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
     * @param api IMoonrakerAPI instance for making API calls
     * @param client helix::IMoonrakerClient instance for WebSocket communication
     */
    void init_backend_from_hardware(const helix::PrinterDiscovery& hardware, IMoonrakerAPI* api,
                                    helix::IMoonrakerClient* client);

    /**
     * @brief Initialize backends from all detected AMS/filament systems
     *
     * Called after Moonraker discovery completes. Creates a backend for each
     * detected system (MMU, tool changer, AFC, etc.). Supports multiple
     * concurrent backends for printers with multiple filament systems.
     *
     * @param hardware Discovered printer hardware
     * @param api IMoonrakerAPI instance for making API calls
     * @param client helix::IMoonrakerClient instance for WebSocket communication
     */
    void init_backends_from_hardware(const helix::PrinterDiscovery& hardware, IMoonrakerAPI* api,
                                     helix::IMoonrakerClient* client);

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
     * @brief Flatten the live AMS state of every backend into AvailableSlots.
     *
     * Walks each registered backend's get_system_info() and converts each
     * unit's slots into the LVGL-free helix::AvailableSlot abstraction
     * (mapping SlotStatus::EMPTY/UNKNOWN to is_empty, copying color/material,
     * unit/global indices, and current tool mapping).
     *
     * This is the canonical "live loaded-filament slot view" shared by the
     * FilamentMappingCard, the print detail view's backend-agnostic preflight
     * validation, and the print-start gate. It reads backend state directly,
     * independent of any UI/card visibility — the card's update() returns early
     * for non-editable backends (Snapmaker U1 / ACE), so its own cache is empty
     * for exactly those backends, but this accessor still reports their slots.
     *
     * @return One AvailableSlot per slot across all backends (empty if none).
     */
    [[nodiscard]] std::vector<helix::AvailableSlot> collect_available_slots() const;

    /**
     * @brief Whether ANY backend is currently feeding from its bypass / external
     *        spool instead of a slot.
     *
     * The companion to collect_available_slots(): bypass is deliberately not a
     * slot, so a tool fed from it can never be satisfied by that vector. Callers
     * that reason over slots must ask this before concluding a tool is unfed.
     *
     * Reads each backend's is_bypass_active() rather than testing
     * AmsSystemInfo::current_slot == -2. Those agree in principle, but
     * current_slot is written from many places during a status frame (the AFC
     * backend alone has nine writes after the one that sets -2, including the
     * deliberately unguarded mount-state derivation), whereas is_bypass_active()
     * returns the firmware's own bypass report. Only the latter is stable.
     */
    [[nodiscard]] bool any_bypass_active() const;

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
     * @param api IMoonrakerAPI instance (not owned)
     */
    void set_moonraker_api(IMoonrakerAPI* api);

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
     * @brief Get the AMS data revision subject
     *
     * Ticks (monotonic int) every time a backend's state or slot data is synced
     * from a backend event. Deliberately a coarse "something changed, go look"
     * signal rather than a precise one: it fires for optimistic local writes as
     * well as firmware reports, so an observer must re-check the thing it
     * actually cares about rather than treating a tick as proof.
     *
     * Exists because AmsBackend::set_event_callback() is single-slot and
     * AmsState owns it (add_backend), so a second consumer cannot subscribe to
     * backend events directly. PrintStartController uses this to re-check
     * SlotRegistry::firmware_mapping_generation() while confirming a filament
     * remap restore (#1270).
     *
     * @return Subject holding a monotonically increasing revision counter
     */
    lv_subject_t* get_ams_data_revision_subject() {
        return &ams_data_revision_;
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
     * @brief Endless-spool status code, backend-neutral.
     *
     * Holds helix::printer::EndlessSpoolStatusKind as an int and is registered
     * for XML as `ams_endless_state`. 0 (Hidden) means the active backend has no
     * endless-spool mechanism, which is the one case where the status row has
     * nothing truthful to say - bind visibility to it with
     * `<bind_flag_if_eq subject="ams_endless_state" flag="hidden" ref_value="0"/>`.
     *
     * Replaced the AD5X-only `ams_ifs_plugin` / `ams_ifs_backup_enabled` pair:
     * every backend answers the same three-axis capability question now, so a
     * per-firmware subject could only ever describe one printer's answer.
     */
    lv_subject_t* get_endless_state_subject() {
        return &ams_endless_state_;
    }

    /**
     * @brief Endless-spool status sentence, translated.
     *
     * Registered for XML as `ams_endless_text`. Computed by
     * helix::printer::endless_spool_status() from the active backend's
     * capabilities; may contain an embedded newline (the restriction reason on
     * its own line), so bind it to a `long_mode="wrap"` label.
     */
    lv_subject_t* get_endless_text_subject() {
        return &ams_endless_text_;
    }

    /**
     * @brief Get the granular operation-phase subject.
     *
     * Holds the active load/unload sub-phase for backends that expose one
     * (currently Snapmaker U1): -1 = none/not-applicable, 0 = Home, 1 = Select,
     * 2 = Heat, 3 = Move (Retract on unload / Feed on load). Synced from
     * AmsSystemInfo::operation_phase in sync_from_backend(). Drives the sidebar
     * step bar's current step on the Snapmaker backend. Static-lifetime
     * singleton subject — no SubjectLifetime token needed to observe it.
     *
     * @return Subject holding the operation phase index
     */
    lv_subject_t* get_ams_operation_phase_subject() {
        return &ams_operation_phase_;
    }

    /**
     * @brief Get operation-indeterminate subject (0/1)
     *
     * 1 while an operation is active but its phase-progress feed has stalled past
     * the backend's threshold (~8s), so the live "Heat 225/230" number is frozen
     * and should be replaced by an indeterminate "Working…" busy state; 0
     * otherwise. Synced from AmsSystemInfo::operation_indeterminate in
     * sync_from_backend() (AD5X IFS drives it; other backends leave it 0).
     * Static-lifetime singleton subject — no SubjectLifetime token needed.
     *
     * @return Subject holding the indeterminate busy flag (0/1)
     */
    lv_subject_t* get_ams_operation_indeterminate_subject() {
        return &ams_operation_indeterminate_;
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
     * 1-based display: current_toolchange+1 of number_of_toolchanges, clamped to
     * the total. The +1 lives in the UI formatter, so every backend must have
     * already normalized its firmware counter to a 0-based index (-1 = none yet).
     * Empty string when not applicable.
     */
    lv_subject_t* get_toolchange_text_subject() {
        return &toolchange_text_;
    }
    lv_subject_t* get_ams_current_toolchange_subject() {
        return &ams_current_toolchange_;
    }
    lv_subject_t* get_ams_number_of_toolchanges_subject() {
        return &ams_number_of_toolchanges_;
    }

    /**
     * @brief Get filament loaded subject
     * @return Subject holding 0 (not loaded) or 1 (loaded)
     */
    lv_subject_t* get_filament_loaded_subject() {
        return &filament_loaded_;
    }

    lv_subject_t* get_filament_runout_subject() {
        return &filament_runout_;
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
     * @brief Get external spool material subject (string)
     * @return Subject holding the external spool's material name
     *         (e.g. "PLA"), "" if no external spool assigned. Pure reflector
     *         of get_external_spool_info() — updated at every site that
     *         updates external_spool_color_.
     */
    lv_subject_t* get_external_spool_material_subject() {
        return &external_spool_material_;
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
     * @brief Get the "unit cards are width-starved" subject
     *
     * 1 when the overview's unit cards are too narrow to carry their full chrome,
     * so the cards drop decoration to protect their content. Written by
     * AmsOverviewPanel from the MEASURED card width (it depends on both the
     * breakpoint and the unit count, so no token or breakpoint alone can express
     * it); read declaratively by ams_unit_card.xml.
     */
    lv_subject_t* get_cards_compact_subject() {
        return &ams_cards_compact_;
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

    /**
     * @brief Get tool map version subject
     *
     * Incremented whenever tool_to_slot_map changes (e.g. user remaps
     * T0→T2). UI can observe this to refresh tool-color-dependent displays.
     *
     * @return Subject holding version counter
     */
    /**
     * @brief Death signal for the subjects this singleton owns.
     *
     * The per-slot accessors above hand out narrower tokens for subjects that
     * die on their own during rediscovery; this one covers the whole set, which
     * dies together in deinit_subjects(). Long-lived outside observers —
     * PrintStatusPanel watches get_current_color_subject() and
     * get_tool_map_version_subject() to recolor the gcode preview — must pass it
     * to observe_*(), or their guards dereference observer nodes that
     * deinit_subjects() freed.
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_lifetime_;
    }

    lv_subject_t* get_tool_map_version_subject() {
        return &tool_map_version_;
    }

    /**
     * @brief Get active-tool first-gate (port) filament-present subject (#991)
     *
     * 1 = filament present at the active tool's port/buffer sensor, 0 = absent.
     * The runout dialog observes this to gate Resume on auto-feed backends.
     * Static-lifetime subject — no SubjectLifetime token needed to observe it.
     *
     * @return Subject holding 1 (present) or 0 (absent)
     */
    lv_subject_t* get_active_tool_port_present_subject() {
        return &active_tool_port_present_;
    }

    /**
     * @brief Set the active-tool port-present flag (#991)
     *
     * Thread-safe: marshals the subject write to the main thread via
     * queue_update, so backends may call this from their WebSocket/background
     * status-update handler. Coalesces no-op writes.
     *
     * @param present true if filament is present at the active tool's port sensor
     */
    void set_active_tool_port_present(bool present);

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

    /// Get subject for formatted dryer humidity text (e.g., "35%" or "---")
    [[nodiscard]] lv_subject_t* get_dryer_humidity_text_subject();

    /// Get subject for dryer info bar visibility (1 = show, 0 = hide)
    /// Shows when dryer_supported OR humidity sensor exists
    [[nodiscard]] lv_subject_t* get_dryer_info_visible_subject();

    /// Select which AMS unit the scalar dryer subjects mirror (the opened unit).
    void set_dryer_mirror_unit(int unit);

    // ========================================================================
    // Clog Detection Meter Subjects
    // ========================================================================

    /**
     * @brief Get clog meter mode subject
     * @return Subject holding mode (0=none, 1=encoder, 2=flowguard, 3=afc_buffer)
     */
    lv_subject_t* get_clog_meter_mode_subject() {
        return &clog_meter_mode_;
    }

    /**
     * @brief Get clog meter value subject
     * @return Subject holding 0-100 (encoder/afc) or -100..+100 (flowguard)
     */
    lv_subject_t* get_clog_meter_value_subject() {
        return &clog_meter_value_;
    }

    /**
     * @brief Get clog meter warning subject
     * @return Subject holding 0=ok, 1=warning
     */
    lv_subject_t* get_clog_meter_warning_subject() {
        return &clog_meter_warning_;
    }

    lv_subject_t* get_clog_meter_danger_pct_subject() {
        return &clog_meter_danger_pct_;
    }
    lv_subject_t* get_clog_meter_peak_pct_subject() {
        return &clog_meter_peak_pct_;
    }
    lv_subject_t* get_clog_meter_center_text_subject() {
        return &clog_meter_center_text_;
    }
    lv_subject_t* get_clog_meter_label_left_subject() {
        return &clog_meter_label_left_;
    }
    lv_subject_t* get_clog_meter_label_right_subject() {
        return &clog_meter_label_right_;
    }

    /**
     * @brief Set source override for clog meter display
     * @param source 0=auto (priority logic), 1=encoder, 2=flowguard, 3=afc
     */
    void set_source_override(int source);

    /**
     * @brief Set danger threshold override for clog meter
     * @param pct 0=use computed default, 50-90=override danger zone percentage
     */
    void set_danger_threshold_override(int pct);

    /**
     * @brief Get current modal target temperature
     * @return Temperature in degrees C
     */
    [[nodiscard]] int get_modal_target_temp() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&modal_target_temp_));
    }

    /**
     * @brief Get current modal duration
     * @return Duration in minutes
     */
    [[nodiscard]] int get_modal_duration_min() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&modal_duration_min_));
    }

    lv_subject_t* get_modal_target_temp_subject() {
        return &modal_target_temp_;
    }
    lv_subject_t* get_modal_duration_subject() {
        return &modal_duration_min_;
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
     * @brief Token'd overload of get_slot_color_subject for observer safety.
     *
     * For secondary backends the returned subject is DYNAMIC (recreated on
     * backend rediscovery), so observers MUST hold the lifetime token. For
     * backend 0 the subject is static and the token is emptied (always-alive).
     */
    [[nodiscard]] lv_subject_t* get_slot_color_subject(int backend_index, int slot_index,
                                                       SubjectLifetime& lifetime);

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

    /**
     * @brief Token'd overload of get_slot_status_subject for observer safety.
     * @see get_slot_color_subject(int, int, SubjectLifetime&)
     */
    [[nodiscard]] lv_subject_t* get_slot_status_subject(int backend_index, int slot_index,
                                                        SubjectLifetime& lifetime);

    /**
     * @brief Get remaining filament subject for a specific slot
     *
     * Holds formatted remaining amount string ("52m", "432g", or "").
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_remaining_subject(int slot_index);

    /**
     * @brief Get per-slot material-type subject (primary backend).
     *
     * Holds the last-synced material string ("PLA", "PETG", …, or "" when the
     * lane has no material). Written by the status-sync path; a delta bumps
     * slots_version so the panel's refresh_slots() re-reads the material label
     * even when the color/status did not change (#1065 — native ZMOD AD5X where
     * a type change keeps the same color and previously left the label stale).
     * Primary backend only; secondary-backend material labels refresh via the
     * color observer's re-read.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_material_subject(int slot_index);

    /**
     * @brief Get per-slot fill-level subject (primary backend).
     *
     * Holds the fill percent as an int using the SlotInfo::display_fill_pct
     * encoding: 0-100 real fill, or -1 when there is no data (observer should
     * leave the render untouched). Written by the sync loop, observed inside the
     * ams_slot widget so the spool fill renders from state on EVERY panel — no
     * panel has to push fill imperatively.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_fill_subject(int slot_index);

    /**
     * @brief Get per-slot fill-level subject for a specific backend and slot.
     *
     * For backend 0 the subject is static and the token is emptied. For
     * secondary backends the subject is DYNAMIC and observers MUST hold the
     * lifetime token.
     * @see get_slot_color_subject(int, int, SubjectLifetime&)
     */
    [[nodiscard]] lv_subject_t* get_slot_fill_subject(int backend_index, int slot_index,
                                                      SubjectLifetime& lifetime);

    // ========================================================================
    // Per-Slot LIVE State Subject Accessors
    // ========================================================================
    //
    // These reflect real-time, Moonraker-fed per-slot state the panel observes
    // to redraw the filament path and active-lane highlight as sensors change.
    // They are backed by static arrays (singleton lifetime, same as the color /
    // status / remaining subjects above), so the bare accessors are safe to
    // observe directly. A (slot, SubjectLifetime&) overload is provided for
    // call-site symmetry with the project's dynamic-subject pattern; because the
    // subjects are static, it returns an EMPTY lifetime token (always alive),
    // which is the documented contract for static subjects (ui_observer_guard.h).

    /**
     * @brief Get per-slot filament path-segment subject.
     *
     * Holds the PathSegment enum value (as int) from
     * AmsBackend::get_slot_filament_segment(slot). Drives the per-slot path
     * canvas redraw.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_segment_subject(int slot_index);
    [[nodiscard]] lv_subject_t* get_slot_segment_subject(int slot_index, SubjectLifetime& lifetime);

    /**
     * @brief Get per-slot toolhead-present subject.
     *
     * Holds 0/1 from AmsBackend::slot_has_filament_at_toolhead(slot) — the live
     * per-slot toolhead/motion sensor. 0 when the backend has no such sensor.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_toolhead_present_subject(int slot_index);
    [[nodiscard]] lv_subject_t* get_slot_toolhead_present_subject(int slot_index,
                                                                  SubjectLifetime& lifetime);

    /**
     * @brief Get per-slot active-loaded subject.
     *
     * Holds 0/1 from AmsBackend::slot_is_actively_loaded(slot) — the single
     * source of truth for the active-lane highlight.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_slot_active_loaded_subject(int slot_index);
    [[nodiscard]] lv_subject_t* get_slot_active_loaded_subject(int slot_index,
                                                               SubjectLifetime& lifetime);

    // ========================================================================
    // Per-Unit Subject Accessors (CFS environment sensors)
    // ========================================================================

    /**
     * @brief Get temperature subject for a unit (tenths of degrees C)
     *
     * Value is in tenths of degrees C (e.g., 270 = 27.0C). 0 = no data.
     *
     * @param unit_index Unit index (0 to MAX_UNITS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_unit_temp_subject(int unit_index);

    /**
     * @brief Get humidity subject for a unit (percentage)
     *
     * Value is integer percentage (0-100). 0 = no data.
     *
     * @param unit_index Unit index (0 to MAX_UNITS-1)
     * @return Subject pointer or nullptr if out of range
     */
    [[nodiscard]] lv_subject_t* get_unit_humidity_subject(int unit_index);

    // ========================================================================
    // Per-Unit Environment Indicator Display Subjects
    // ========================================================================

    /// Formatted temperature text for indicator (e.g., "24°C")
    [[nodiscard]] lv_subject_t* get_env_ind_temp_text_subject(int unit_index);

    /// Formatted humidity text for indicator (e.g., "46%")
    [[nodiscard]] lv_subject_t* get_env_ind_humidity_text_subject(int unit_index);

    /// Visibility flag for indicator (1=show, 0=hide)
    [[nodiscard]] lv_subject_t* get_env_ind_visible_subject(int unit_index);

    /// Drying active flag (1=drying, 0=passive)
    [[nodiscard]] lv_subject_t* get_env_ind_drying_active_subject(int unit_index);

    /// Humidity status for indicator (0=ok/green, 1=warn/yellow, 2=danger/red)
    [[nodiscard]] lv_subject_t* get_env_ind_humidity_status_subject(int unit_index);

    /// Humidity visibility flag (1=show, 0=hide - based on backend capability)
    [[nodiscard]] lv_subject_t* get_env_ind_humidity_visible_subject(int unit_index);

    /// Formatted drying text (e.g., "47°C -> 55°C  2:30 left")
    [[nodiscard]] lv_subject_t* get_env_ind_drying_text_subject(int unit_index);

    /// Select which AMS unit the detail-view env indicator mirrors.
    void set_detail_env_unit(int unit);
    [[nodiscard]] lv_subject_t* get_env_ind_detail_drying_active_subject() {
        return &env_ind_detail_drying_active_;
    }
    [[nodiscard]] lv_subject_t* get_env_ind_detail_temp_text_subject() {
        return &env_ind_detail_temp_text_;
    }

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
     * @brief Update "Currently Loaded" display subjects using pre-fetched system info
     *
     * Avoids redundant get_system_info() call when the caller already has the info
     * (e.g., from sync_from_backend()). The provided info is used for the primary
     * backend (index 0); secondary backends are queried as needed.
     *
     * @param primary_info Pre-fetched system info from the primary backend
     */
    void sync_current_loaded_from_backend(const AmsSystemInfo& primary_info);

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
     * Update the external-spool info without writing settings.json.
     *
     * Used by FilamentConsumptionTracker to push live weight updates
     * to observers while throttling disk writes to a slower cadence.
     * Callers that need persistence should call set_external_spool_info().
     */
    void set_external_spool_info_in_memory(const SlotInfo& info);

    /**
     * @brief Clear external spool info
     */
    void clear_external_spool_info();

    /**
     * @brief Commit a backend-slot spool edit through every backing store.
     *
     * Single authority for spool assignment changes on backend slots. Order:
     * 1. S1: Spoolman server active spool — set when linking (id > 0), clear
     *    (post 0) when unlinking a previously-linked slot. Ungated on
     *    manages_active_spool() by design: matches the previous overlay
     *    semantics (see spec § follow-ups for the SET-arm gating question).
     * 2. S6: invalidate the old spool's identity cache when the link changed.
     * 3. S3: backend->set_slot_info() (firmware SET_SPOOL_ID gcode rides inside).
     * 4. S4+S7: sync_from_backend().
     *
     * @return the AmsError from set_slot_info so callers keep their error toasts.
     */
    AmsError commit_slot_edit(int slot_index, const SlotInfo& original, const SlotInfo& info);

    /**
     * @brief Commit an external-spool assignment through every backing store.
     *
     * Non-empty (spoolman_id > 0 OR material set) → S5 persist via
     * set_external_spool_info; empty → S5 erase via clear_external_spool_info
     * (an empty assigned=true record is the bug the FilamentPanel arm avoided).
     * S1: set/clear the Spoolman server active spool to match.
     * S6: invalidate the replaced link's identity cache entry on a link change
     * (mirrors commit_slot_edit).
     */
    void commit_external_spool_edit(const SlotInfo& info);

    /**
     * @brief Server-first variant for callers that must gate the local store
     * write on the server round-trip (SpoolmanPanel::set_active_spool).
     *
     * S6 runs up front. When info links a spool (spoolman_id > 0), S1's
     * set_active_spool() is issued with the caller's completion: on server
     * success the S5+S7 store subset runs and on_committed fires (both on the
     * main thread); on server failure on_error fires (main thread) and no
     * store is written. Manual entries and clears have no server identity to
     * gate on — the clear arm fires fire-and-forget exactly like the sync
     * commit, the store subset runs at once, and on_committed fires
     * immediately.
     */
    void commit_external_spool_edit(const SlotInfo& info, std::function<void()> on_committed,
                                    std::function<void(const MoonrakerError& err)> on_error);

    /**
     * @brief Set the current AMS action state directly
     *
     * Used by UI to indicate operation in progress (e.g., during UI-managed preheat
     * before backend starts). Triggers XML binding updates for action-dependent UI.
     *
     * @param action The action state to set
     */
    void set_action(AmsAction action);

    /// @brief Subject holding the current toolchange narration phase index (-1 = none).
    lv_subject_t* get_toolchange_step_subject() {
        return &toolchange_step_;
    }

    /// @brief Active toolchange operation (used by the narration router to resolve phases).
    StepOperationType get_active_step_operation() const {
        return active_step_operation_.load(std::memory_order_relaxed);
    }

    /// @brief Set the active toolchange operation kind.
    ///
    /// Out-of-line because a change of operation also has to drop the narration
    /// high-water mark: phase indices are template-relative, and each
    /// StepOperationType has its own template.
    void set_active_step_operation(StepOperationType op);

    /// Set the current toolchange narration phase index (MAIN THREAD ONLY).
    /// Also mirrors the human label into ams_action_detail for the status line.
    /// index = -1 clears.
    ///
    /// The published index is LATCHED so it can only move forwards within one
    /// operation — see the implementation for why a firmware may legitimately
    /// re-narrate an earlier phase. index = 0 (the template's first phase) and
    /// index = -1 both reset the latch.
    void set_narration_phase(int index, const std::string& label);

    /**
     * @brief Check if a filament operation (load/unload) is currently active
     *
     * Used by FilamentSensorManager to suppress spurious sensor toasts while
     * filament is being intentionally moved through sensors.
     *
     * @return true if AMS is actively loading, unloading, or performing related ops
     */
    /**
     * @brief Take the one-shot "an unload just finished" runout grace.
     *
     * True at most once per unload. The companion to
     * is_filament_operation_active(): that answers "is filament moving right
     * now", this answers "did the removal we are about to react to come from an
     * unload the user just asked for". Retired if filament returns first.
     */
    [[nodiscard]] bool consume_post_unload_runout_grace();

    /**
     * @brief Is the post-unload grace armed, without spending it?
     *
     * The idle runout modal is the grace's CONSUMER; surfaces that merely want
     * to stay quiet during the same window (the "Filament removed" toast) must
     * not race it for the single shot, or whichever sees the sensor edge first
     * silently disarms the other. Same expiry as consume_post_unload_runout_grace().
     */
    [[nodiscard]] bool post_unload_runout_grace_armed();

    bool is_filament_operation_active();

    /**
     * @brief Record that a lane just completed an unload
     *
     * Called by the Snapmaker U1 backend when channel_state reaches
     * "unload_finish" for a slot. The user is then expected to pull the
     * filament out of that lane, which fires the per-lane runout sensor a few
     * seconds later. FilamentSensorManager queries was_slot_recently_unloaded()
     * to suppress the runout-guidance modal during the grace window.
     *
     * Thread-safe; may be called from the WebSocket background thread.
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     */
    void mark_slot_unloaded(int slot_index);

    /**
     * @brief Check whether a lane completed an unload within the grace window
     *
     * @param slot_index Slot index (0 to MAX_SLOTS-1)
     * @return true if mark_slot_unloaded(slot_index) was called within the last
     *         RECENT_UNLOAD_GRACE window
     */
    bool was_slot_recently_unloaded(int slot_index) const;

    /**
     * @brief Bump the slots version counter to trigger UI refresh
     *
     * Call after modifying slot data (weights, endless spool config, etc.)
     * to notify observers and redraw the AMS panel.
     */
    void bump_slots_version();

  private:
    friend class AmsStateTestAccess;

    /** @brief Fire external_spool_color_ subject to notify observers of spool changes */
    void notify_external_spool_changed(const SlotInfo& info);

    /** @brief Set "Currently Loaded" subjects to default/empty state with guards */
    void set_current_loaded_defaults();

    /** @brief Sync clog detection meter subjects from system info */
    void sync_clog_meter_from_info(const AmsSystemInfo& info);

    /**
     * @brief Sync the endless-spool status subjects from a backend's capabilities.
     *
     * Main thread only (it writes subjects). Called from sync_from_backend(),
     * which the EVENT_STATE_CHANGED handler already marshals through
     * helix::ui::queue_update().
     *
     * @param backend Active primary backend; nullptr resets the row to Hidden.
     */
    void sync_endless_spool_from_backend(AmsBackend* backend);

    /** @brief Set up observer on HumiditySensorManager dryer humidity subject */

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
     * @brief Probe for ACE via REST endpoint
     *
     * Makes an async REST call to /server/ace/info. If successful,
     * creates ACE backend via lv_async_call to maintain thread safety.
     *
     * @param api IMoonrakerAPI instance for REST calls
     * @param client helix::IMoonrakerClient instance for the backend
     */
    void probe_ace(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    /**
     * @brief Create and start ACE backend
     *
     * Called on main thread after successful ACE probe.
     * Must be called from LVGL thread context.
     *
     * @param api IMoonrakerAPI instance
     * @param client helix::IMoonrakerClient instance
     */
    void create_ace_backend(IMoonrakerAPI* api, helix::IMoonrakerClient* client);

    /// Per-backend slot subject storage for secondary backends (index > 0)
    struct BackendSlotSubjects {
        std::vector<lv_subject_t> colors;
        std::vector<lv_subject_t> statuses;
        std::vector<lv_subject_t> fills; // int: fill percent 0-100, -1 = unknown
        int slot_count = 0;
        /// Lifetime token shared by every subject in this struct. These subjects
        /// are DYNAMIC (destroyed in deinit() on backend rediscovery), so any
        /// observer bound to them MUST hold a copy of this token — the token'd
        /// accessor overloads hand it out. deinit() invalidates it.
        SubjectLifetime lifetime;
        void init(int count);
        void deinit();
    };

    mutable std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<AmsBackend>> backends_;
    std::vector<BackendSlotSubjects> secondary_slot_subjects_;
    /// FilamentConsumptionTracker sink handles, keyed by backend index. One
    /// AmsSlotSink per slot is registered when a backend is added and removed
    /// in clear_backends().
    std::map<int, std::vector<helix::FilamentConsumptionTracker::SinkHandle>> consumption_sinks_;
    bool initialized_ = false;

    // Moonraker API for Spoolman integration
    IMoonrakerAPI* api_ = nullptr;
    int last_synced_spoolman_id_ = 0; ///< Track to avoid duplicate set_active_spool calls

    /// S5+S7 store subset shared by both commit_external_spool_edit arms:
    /// persist non-empty records, erase empty ones (kills empty
    /// assigned=true records).
    void apply_external_spool_store(const SlotInfo& info);

    /// S6 — drop the replaced link's identity-cache entry when the external
    /// spool's Spoolman link changes (mirrors commit_slot_edit's original-vs-
    /// edited guard).
    void invalidate_stale_external_identity(const SlotInfo& info);

    // Subject manager for automatic cleanup
    SubjectManager subjects_;
    /// See get_subjects_lifetime(). Created with the object and REPLACED (never
    /// nulled) by deinit_subjects(): an empty token reads as "dead" and would
    /// suppress removal for live observers.
    SubjectLifetime subjects_lifetime_ = std::make_shared<bool>(true);

    /// Expires the setters that marshal themselves to the main thread. Declared
    /// after `subjects_` so reverse-order member destruction invalidates it
    /// before the subjects it protects; also invalidated by deinit_subjects(),
    /// which is the teardown that actually happens on a live instance between
    /// tests (#1165, #1146).
    helix::AsyncLifetimeGuard async_lifetime_;

    // Backend selector subjects
    lv_subject_t backend_count_;
    lv_subject_t active_backend_;
    lv_subject_t ams_data_revision_;

    // System-level subjects
    lv_subject_t ams_type_;
    lv_subject_t ams_action_;
    /// Granular load/unload sub-phase (-1=none, 0=Home, 1=Select, 2=Heat,
    /// 3=Move). Snapmaker U1 only; static-lifetime singleton subject.
    lv_subject_t ams_operation_phase_;
    /// 1 while an active op's phase-progress feed has stalled (~8s) so the frozen
    /// live-temp number should read as "Working…"; 0 otherwise. AD5X IFS only.
    /// Static-lifetime singleton subject.
    lv_subject_t ams_operation_indeterminate_;
    lv_subject_t toolchange_step_; ///< current narration phase index (-1 = none/idle)
    /// Active toolchange operation for the narration router to resolve a phase
    /// index without a sidebar pointer. Defaults to a swap (most common case).
    std::atomic<StepOperationType> active_step_operation_{StepOperationType::LOAD_SWAP};
    /// Highest phase index published since the current operation began. Guards
    /// toolchange_step_ against firmware that narrates one phase more than once
    /// per operation (AFC wipes before AND after the kick). -1 = no phase yet.
    int narration_phase_high_water_{-1};
    lv_subject_t current_slot_;
    lv_subject_t pending_target_slot_;
    lv_subject_t ams_current_tool_;
    lv_subject_t filament_loaded_;
    lv_subject_t filament_runout_;
    /// Edge tracking behind `ams_filament_runout`. `AmsSystemInfo::filament_runout`
    /// is a LEVEL on some backends and a sticky latch on at least one (the CFS
    /// mirrors `box.filament_useup`, which is only ever cleared by a successful
    /// extrude, not by the print ending), so the level alone cannot say whether a
    /// runout happened during THIS job. sync_from_backend() therefore looks for a
    /// false->true transition seen while a job was running, not for the level.
    /// All three are written only under mutex_ and reset by clear_backends().
    ///
    /// Last raw level, for edge detection.
    bool prev_backend_runout_{false};
    /// A rising edge was seen while the job was PRINTING or PAUSED, and the
    /// episode it belongs to is not over. This is what makes the indicator
    /// legitimate.
    bool runout_edge_armed_{false};
    /// Previous PAUSED-ness, so a PAUSED->anything-else transition (the user
    /// resumed or cancelled) can end the episode. Needed because the arm is
    /// usually made while PRINTING, one frame before the firmware's pause lands
    /// — "not paused" therefore cannot mean "disarm" on its own.
    bool runout_prev_paused_{false};
    /// Previous any_bypass_active(), so sync_from_backend() can bump
    /// slots_version on the edge. Bypass moves no slot, so nothing else in the
    /// slot-delta scan notices it, and the pre-print filament check would keep
    /// serving a stale result.
    bool last_bypass_active_{false};
    /// How long after an unload completes its removal edge is still credited to
    /// that unload. Same 30s window as RECENT_UNLOAD_GRACE below and as the
    /// AD5X IFS runout suppression: past it, an empty sensor is a real runout.
    static constexpr std::chrono::seconds POST_UNLOAD_RUNOUT_GRACE{30};
    /// One-shot: an unload completed and its removal edge has not arrived yet.
    bool post_unload_runout_grace_{false};
    /// When post_unload_runout_grace_ was armed. Without it the flag has no time
    /// bound, and an unload that leaves nothing loaded never reaches the
    /// filament-back retirement — the next genuine idle runout, days later,
    /// would be swallowed.
    std::chrono::steady_clock::time_point post_unload_runout_grace_at_{};
    /// Whether the operation currently in flight has passed through UNLOADING.
    bool saw_unload_in_op_{false};
    /// prev_backend_runout_ has no meaning yet, so the first sample seeds it
    /// instead of counting as an edge — a flag that was already true when we
    /// connected (or when a backend was swapped in) describes no transition we
    /// witnessed. Same reasoning as AmsBackendAd5xIfs's head-switch edge gate.
    bool runout_level_seeded_{false};
    lv_subject_t bypass_active_;
    lv_subject_t external_spool_color_;
    /// External spool material name — string flavor of external_spool_color_.
    /// Pure reflector of get_external_spool_info(); mirrors the color subject's
    /// update sites so XML text bindings stay in lockstep with the color dot.
    lv_subject_t external_spool_material_;
    char external_spool_material_buf_[32]; // "PLA", "PETG-CF", ... fits comfortably
    lv_subject_t supports_bypass_;
    lv_subject_t ams_slot_count_;
    lv_subject_t ams_cards_compact_;
    lv_subject_t slots_version_;
    lv_subject_t tool_map_version_;
    /// First-gate (port) filament-present flag for the ACTIVE tool (#991).
    /// 1 = filament present at the active tool's port/buffer sensor, 0 = absent.
    /// Auto-feed backends (Snapmaker U1) update this from the port sensor — NOT
    /// the toolhead motion sensor — so the runout dialog can gate Resume on the
    /// signal that flips true the moment a fresh spool is re-fed. Static-lifetime
    /// singleton subject (no SubjectLifetime token needed). Defaults to 1 so
    /// non-auto-feed / unknown backends never gate Resume.
    lv_subject_t active_tool_port_present_;
    std::vector<int> last_tool_map_; ///< Cached for change detection in sync_from_backend

    /// Most recent backend-supplied operation detail (cached so the print-state
    /// observer can rerun the action-detail derivation without re-querying the
    /// backend). Updated from sync_from_backend() and set_action_detail().
    std::string last_operation_detail_;

    std::string last_narration_label_; ///< live toolchange narration phase label; top priority in
                                       ///< recompute_action_detail; cleared on IDLE

    /// Observer that re-runs compute_action_detail() when PrinterState's
    /// print_state_enum subject changes, so the sidebar flips between
    /// "Idle" / "Printing" / "Paused" without waiting for the next backend sync.
    /// print_state_enum is a *static* PrinterState subject, so no
    /// SubjectLifetime token is required.
    ObserverGuard print_state_observer_;

    /// Recompute the ams_action_detail subject from current AMS action +
    /// cached operation_detail + PrinterState print state.
    /// Caller must hold mutex_.
    void recompute_action_detail();

    /// Wire (or rewire) the print_state_observer_. Idempotent.
    void install_print_state_observer();

    /// In-memory override for external spool info. Set by set_external_spool_info_in_memory()
    /// to allow live tracker updates without touching settings.json. When set, takes priority
    /// over SettingsManager in get_external_spool_info(). Cleared by clear_external_spool_info().
    std::optional<SlotInfo> in_memory_external_spool_;

    // String subjects (need buffers)
    lv_subject_t ams_action_detail_;
    char action_detail_buf_[64];
    lv_subject_t ams_system_name_;
    char system_name_buf_[32];
    lv_subject_t ams_system_logo_;
    char system_logo_buf_[64];
    lv_subject_t ams_current_tool_text_;
    char ams_current_tool_text_buf_[16]; // "T0" to "T15" or "---"

    /// Endless-spool status: kind as int, sentence as string. See the accessors.
    /// The buffer holds two translated lines; German and Russian restriction
    /// texts are the long ones, and Cyrillic costs ~2 bytes a character, hence
    /// 384 rather than the 64 used elsewhere.
    lv_subject_t ams_endless_state_;
    lv_subject_t ams_endless_text_;
    char ams_endless_text_buf_[384];

    // Tool change progress (AFC multi-color prints)
    lv_subject_t toolchange_visible_;        // 1 when swaps expected, 0 otherwise
    lv_subject_t ams_current_toolchange_;    // 0-based current toolchange index (-1=none)
    lv_subject_t ams_number_of_toolchanges_; // Total expected toolchanges
    lv_subject_t toolchange_text_;           // "2 / 5" formatted display
    char toolchange_text_buf_[32]{};         // Buffer for formatted text

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
    int dryer_mirror_unit_ = 0; ///< Unit whose dryer state the scalar subjects mirror

    // Dryer text subjects (need buffers)
    lv_subject_t dryer_current_temp_text_;
    char dryer_current_temp_text_buf_[16];
    lv_subject_t dryer_target_temp_text_;
    char dryer_target_temp_text_buf_[16];
    lv_subject_t dryer_time_text_;
    char dryer_time_text_buf_[32];

    // Dryer humidity and info bar visibility subjects
    lv_subject_t dryer_humidity_text_;
    char dryer_humidity_text_buf_[8]; ///< "35%" or "---"
    lv_subject_t dryer_info_visible_; ///< 1 when info bar should show

    // Dryer modal editing subjects (user-adjustable values)
    lv_subject_t dryer_modal_temp_text_;
    char dryer_modal_temp_text_buf_[16];
    lv_subject_t dryer_modal_duration_text_;
    char dryer_modal_duration_text_buf_[16];
    lv_subject_t modal_target_temp_;  ///< Modal's target temp in °C (raw int subject)
    lv_subject_t modal_duration_min_; ///< Modal's duration in minutes (raw int subject)

    // Clog detection config overrides (set by ClogDetectionConfigModal)
    int source_override_ = 0;           // 0=auto, 1=encoder, 2=flowguard, 3=afc
    int danger_threshold_override_ = 0; // 0=use computed default

    // Clog detection meter subjects
    lv_subject_t clog_meter_mode_;    // 0=none, 1=encoder, 2=flowguard, 3=afc_buffer
    lv_subject_t clog_meter_value_;   // 0-100 (encoder/afc) or -100..+100 (flowguard)
    lv_subject_t clog_meter_warning_; // 0=ok, 1=warning
    lv_subject_t clog_meter_value_text_;
    char clog_meter_value_text_buf_[16]{};
    lv_subject_t clog_meter_mode_text_;
    char clog_meter_mode_text_buf_[24]{};
    lv_subject_t clog_meter_danger_pct_;  // 0-100, where danger zone starts
    lv_subject_t clog_meter_peak_pct_;    // 0-100, peak-hold marker position
    lv_subject_t clog_meter_center_text_; // Enhanced center display
    char clog_meter_center_text_buf_[16]{};
    lv_subject_t clog_meter_label_left_; // Left endpoint label
    char clog_meter_label_left_buf_[16]{};
    lv_subject_t clog_meter_label_right_; // Right endpoint label
    char clog_meter_label_right_buf_[16]{};

    // Currently Loaded display subjects (reactive binding for "Currently Loaded" card)
    lv_subject_t current_material_text_;
    // Holds a full filament identity — brand, Spoolman filament name and
    // material concatenated. lv_subject_copy_string() truncates with
    // lv_strlcpy and reports nothing, so an undersized buffer clips the label
    // silently; real Spoolman names run past 50 characters on their own.
    char current_material_text_buf_[128];
    lv_subject_t current_slot_text_;
    char current_slot_text_buf_[64];
    lv_subject_t current_weight_text_;
    char current_weight_text_buf_[16];
    lv_subject_t current_has_weight_;
    lv_subject_t current_color_;

    // Per-slot subjects (color, status, remaining filament)
    lv_subject_t slot_colors_[MAX_SLOTS];
    lv_subject_t slot_statuses_[MAX_SLOTS];
    lv_subject_t slot_remaining_[MAX_SLOTS]; // string: "52m" or "432g" or ""
    char slot_remaining_buf_[MAX_SLOTS][16]; // buffers for remaining strings
    lv_subject_t slot_materials_[MAX_SLOTS]; // string: "PLA", "PETG", … or "" (last synced type)
    char slot_materials_buf_[MAX_SLOTS][24]; // buffers for material strings (holds "PETG-CF" etc.)
    lv_subject_t slot_fills_[MAX_SLOTS];     // int: fill percent 0-100, -1 = unknown/no-data
                                             // (SlotInfo::display_fill_pct encoding)

    // Per-slot LIVE state subjects — the panel observes these to redraw the path
    // and active-lane highlight in real time as Moonraker sensor data arrives.
    // Updated alongside slot_colors_/slot_statuses_ in the status-sync path.
    lv_subject_t slot_segments_[MAX_SLOTS];         // int: PathSegment enum value
    lv_subject_t slot_toolhead_present_[MAX_SLOTS]; // int: 0/1 per-slot toolhead sensor
    lv_subject_t slot_active_loaded_[MAX_SLOTS];    // int: 0/1 firmware seated & loaded

    // Per-unit environment subjects (CFS temp/humidity)
    lv_subject_t unit_temp_[MAX_UNITS];     // int: tenths of C (270 = 27.0C), 0 = no data
    lv_subject_t unit_humidity_[MAX_UNITS]; // int: percentage, 0 = no data

    // Per-unit environment indicator display subjects (formatted text for XML binding)
    static constexpr int ENV_IND_TEXT_BUF_SIZE = 16;
    static constexpr int ENV_IND_DRYING_BUF_SIZE = 32;

    lv_subject_t env_ind_temp_text_[MAX_UNITS];
    char env_ind_temp_text_buf_[MAX_UNITS][ENV_IND_TEXT_BUF_SIZE]{};
    lv_subject_t env_ind_humidity_text_[MAX_UNITS];
    char env_ind_humidity_text_buf_[MAX_UNITS][ENV_IND_TEXT_BUF_SIZE]{};
    lv_subject_t env_ind_visible_[MAX_UNITS];
    lv_subject_t env_ind_humidity_status_[MAX_UNITS]; // 0=ok, 1=warn, 2=danger
    lv_subject_t env_ind_humidity_visible_[MAX_UNITS];
    lv_subject_t env_ind_drying_active_[MAX_UNITS];
    lv_subject_t env_ind_drying_text_[MAX_UNITS];
    char env_ind_drying_text_buf_[MAX_UNITS][ENV_IND_DRYING_BUF_SIZE]{};

    // Always-off placeholders for units past MAX_UNITS (see
    // env_indicator_subject_names). Written once at init and never again.
    lv_subject_t env_ind_off_flag_;
    lv_subject_t env_ind_off_text_;
    char env_ind_off_text_buf_[ENV_IND_TEXT_BUF_SIZE]{};

    // Detail-view env indicator mirror subjects (reflect detail_env_unit_)
    lv_subject_t env_ind_detail_temp_text_;
    char env_ind_detail_temp_text_buf_[ENV_IND_TEXT_BUF_SIZE]{};
    lv_subject_t env_ind_detail_humidity_text_;
    char env_ind_detail_humidity_text_buf_[ENV_IND_TEXT_BUF_SIZE]{};
    lv_subject_t env_ind_detail_humidity_status_;
    lv_subject_t env_ind_detail_humidity_visible_;
    lv_subject_t env_ind_detail_visible_;
    lv_subject_t env_ind_detail_drying_active_;
    lv_subject_t env_ind_detail_drying_text_;
    char env_ind_detail_drying_text_buf_[ENV_IND_DRYING_BUF_SIZE]{};
    int detail_env_unit_ = 0;

    /// Mirror the detail_env_unit_'s per-unit env indicator subjects into the
    /// dedicated ams_env_ind_detail_* subjects consumed by the statically
    /// embedded detail-view indicator (see Task 9 brief).
    void mirror_detail_env_subjects();

    // Stored callback for mock gcode response injection
    std::function<void(const std::string&)> gcode_response_callback_;

    /// Grace window after an unload during which a runout on that lane's sensor
    /// is expected (the user pulls the just-unloaded filament out) and must NOT
    /// pop the runout-guidance modal. Auto-expires.
    static constexpr std::chrono::seconds RECENT_UNLOAD_GRACE{30};

    /// Per-slot timestamp of the last completed unload (unload_finish). A
    /// non-subject plain field guarded by mutex_ — written from the backend's
    /// background-thread status parse, read from FilamentSensorManager. Default
    /// time_point{} (epoch) means "never unloaded" and is always outside the
    /// grace window.
    std::array<std::chrono::steady_clock::time_point, MAX_SLOTS> last_unload_time_{};
};
