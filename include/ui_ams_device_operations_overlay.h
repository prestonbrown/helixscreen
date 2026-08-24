// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_device_operations_overlay.h
 * @brief AMS Device Operations overlay with progressive disclosure
 *
 * This overlay shows Quick Actions at the top and a list of device
 * sections below. Tapping a section row pushes the detail overlay
 * (AmsDeviceSectionDetailOverlay) with that section's controls.
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Main thread only
 */

#pragma once

#include "ui_bypass_toggle_controller.h"

#include "ams_types.h"
#include "overlay_base.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

// Forward declarations
class AmsBackend;

namespace helix::ui {

/**
 * @class AmsDeviceOperationsOverlay
 * @brief Progressive disclosure overlay for AMS device operations
 *
 * Quick Actions card at top (Home/Recover/Abort/Bypass/Status),
 * then a list of section rows (icon + label + chevron). Tapping
 * a row pushes AmsDeviceSectionDetailOverlay with that section's controls.
 */
class AmsDeviceOperationsOverlay : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     */
    AmsDeviceOperationsOverlay();

    /**
     * @brief Destructor
     */
    ~AmsDeviceOperationsOverlay() override;

    // Non-copyable
    AmsDeviceOperationsOverlay(const AmsDeviceOperationsOverlay&) = delete;
    AmsDeviceOperationsOverlay& operator=(const AmsDeviceOperationsOverlay&) = delete;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize subjects for reactive binding
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks for:
     * - Home, Recover, Abort buttons
     * - Bypass toggle
     * - Dynamic action buttons
     */
    void register_callbacks() override;

    /**
     * @brief Create the overlay UI (called lazily)
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Device Operations"
     */
    const char* get_name() const override {
        return "Multi-Filament System Management";
    }

    //
    // === Public API ===
    //

    /**
     * @brief Show the overlay
     *
     * This method:
     * 1. Ensures overlay is created (lazy init)
     * 2. Queries backend for capabilities and actions
     * 3. Updates subjects and dynamic UI
     * 4. Pushes overlay onto navigation stack
     *
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    /**
     * @brief Refresh the overlay from backend
     *
     * Re-queries backend and updates all subjects and dynamic actions.
     */
    void refresh();

  protected:
    /// Abort a pending unload->enable bypass chain when this surface goes away,
    /// matching BypassWidget::detach() and AmsOperationSidebar::cleanup(): the
    /// controller's self-observer must not fire an enable nobody is waiting on.
    void on_ui_destroyed() override;

  private:
    //
    // === Internal Methods ===
    //

    /// Update subjects from backend state
    void update_from_backend();

    /// Populate section list rows from backend sections
    void populate_section_list();

    /// Create a single section row (icon + label + chevron)
    void create_section_row(lv_obj_t* parent, const helix::printer::DeviceSection& section);

    /// Convert AmsAction enum to human-readable string
    static const char* action_to_string(int action);

    //
    // === Static Callbacks ===
    //

    static void on_home_clicked(lv_event_t* e);
    static void on_recover_clicked(lv_event_t* e);
    static void on_abort_clicked(lv_event_t* e);
    static void on_bypass_toggled(lv_event_t* e);

    /// Callback for the AFC unload-after-print toggle (AFC backends only)
    static void on_afc_unload_after_print_toggled(lv_event_t* e);
    static void on_always_show_bypass_spool_toggled(lv_event_t* e);

    /// Callback for the keep-spool-info-on-eject toggle (backends whose
    /// firmware reports spool ids per lane only)
    static void on_keep_spool_info_toggled(lv_event_t* e);
    static void on_force_bypass_controls_toggled(lv_event_t* e);

    /// Callback for the QIDI eject distance slider (QIDI Box backends only)
    static void on_qidi_eject_distance_changed(lv_event_t* e);

    /// Callback for the QIDI eject velocity slider (QIDI Box backends only)
    static void on_qidi_eject_velocity_changed(lv_event_t* e);

    /// Callback for section row click — pushes detail overlay
    static void on_section_row_clicked(lv_event_t* e);

    /// Callback for the "Reset Endless Spool" action row. Opens a confirmation
    /// dialog (the reset wipes ALL failover config) and, on confirm, calls the
    /// backend's reset_endless_spool().
    static void on_reset_endless_spool_clicked(lv_event_t* e);

    //
    // === State ===
    //

    /// Alias for overlay_root_ to match existing pattern
    lv_obj_t*& overlay_ = overlay_root_;

    /// Container for section list rows
    lv_obj_t* section_list_container_ = nullptr;

    /// Subject for system info text (e.g. "System: AFC · v1.2.3")
    lv_subject_t system_info_subject_;

    /// Buffer for system info text
    char system_info_buf_[128] = {};

    /// Subject for status text display
    lv_subject_t status_subject_;

    /// Buffer for status text
    char status_buf_[128] = {};

    /// Subject for bypass support (0=not supported, 1=supported).
    /// Folds in the force-bypass override, so this is what gates the controls.
    lv_subject_t supports_bypass_subject_;

    /// Subject for the firmware's own bypass report, override NOT applied
    /// (0=firmware says none, 1=firmware reports one). Gates the override row:
    /// it appears only when the firmware says no, and stays visible once the
    /// user turns the override on so they can turn it back off.
    lv_subject_t fw_supports_bypass_subject_;

    /// Subject for hardware bypass sensor (0=virtual toggle, 1=hardware sensor)
    lv_subject_t hw_bypass_sensor_subject_;

    /// Subject for auto-heat support (0=not supported, 1=supported)
    lv_subject_t supports_auto_heat_subject_;

    /// Subject for backend presence (0=no backend, 1=has backend)
    lv_subject_t has_backend_subject_;

    /// Subject for AFC backend detection (0=not AFC, 1=AFC) — gates the
    /// unload-after-print toggle, which only applies to AFC systems
    lv_subject_t is_afc_subject_;

    /// Subject gating the keep-spool-info-on-eject row (0=hidden, 1=shown).
    /// Set from AmsBackend::printer_reports_spool_ids(), so the row appears
    /// only on systems whose firmware reports spool ids per lane (AFC, Happy
    /// Hare); no backend means hidden.
    lv_subject_t reports_spool_ids_subject_;

    /// Subject disabling the keep-spool-info-on-eject toggle (0=enabled,
    /// 1=firmware retention owns it). Set from
    /// AmsBackend::printer_retains_spool_info(): with AFC's per-lane
    /// remember_spool true everywhere, the toggle has no observable effect,
    /// so it is shown disabled with a note instead of silently lying.
    lv_subject_t printer_retains_spool_info_subject_;

    /// Subject for QIDI Box backend detection (0=not QIDI, 1=QIDI) — gates the
    /// eject distance/velocity rows, which only apply to QIDI Box systems
    lv_subject_t is_qidi_subject_;

    /// Display string subject for the QIDI eject distance value (e.g. "878 mm")
    lv_subject_t qidi_eject_distance_display_subject_;
    char qidi_eject_distance_buf_[32] = {};

    /// Display string subject for the QIDI eject velocity value (e.g. "100 mm/s")
    lv_subject_t qidi_eject_velocity_display_subject_;
    char qidi_eject_velocity_buf_[32] = {};

    /// Subject gating the "Reset Endless Spool" row (0=hidden, 1=shown).
    /// Set from EndlessSpoolCapabilities::editable(), so the row lights up for
    /// any backend whose endless-spool mapping the UI may write — AFC (per-slot
    /// edges), single-unit Happy Hare (groups) and the mock — and stays hidden
    /// for read-only systems (CFS, AD5X IFS) and backends with no endless spool.
    lv_subject_t can_reset_endless_spool_subject_;

    /// Cached section metadata for row click dispatch
    std::vector<helix::printer::DeviceSection> cached_sections_;

    /// Shared bypass policy — print guard, hardware-sensor refusal and the
    /// unload-first chain. One instance per surface, as in AmsOperationSidebar
    /// and BypassWidget; the switch handler owns nothing beyond forwarding to it.
    BypassToggleController bypass_toggle_;
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton AmsDeviceOperationsOverlay
 */
AmsDeviceOperationsOverlay& get_ams_device_operations_overlay();

} // namespace helix::ui
