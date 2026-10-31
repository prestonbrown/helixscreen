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
#include "ui_observer_guard.h"

#include "ams_types.h"
#include "overlay_base.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

// Forward declarations
namespace helix {
class AmsBackend;
} // namespace helix

namespace helix::ui {

/// One read-only fact about the filament system, already translated.
struct DeviceDetailRow {
    std::string label;
    std::string value;
};

/// The facts a backend publishes beyond its controls: each unit's firmware and
/// serial, the toolchange purge volume, the Spoolman mode and a pending spool.
/// A row appears only when its value is published.
std::vector<DeviceDetailRow> ams_device_detail_rows(const AmsSystemInfo& info);

/// Cancel the filament system's running operation and toast the outcome. The
/// one abort every surface that offers one calls.
void abort_ams_operation();

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
     * @brief Destructor
     */
    ~AmsDeviceOperationsOverlay() override;

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

    /// Also subscribes to the AMS action so Abort follows what the backend can cancel.
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Device Operations"
     */
    const char* get_name() const override {
        return "Multi-Filament System Management";
    }

    const char* xml_component() const override {
        return "ams_device_operations";
    }

    //
    // === Public API ===
    //

    /**
     * @brief Refresh the overlay from backend
     *
     * Re-queries backend and updates all subjects and dynamic actions.
     */
    void refresh();

  protected:
    /// Re-queries the backend on every open.
    void before_show() override {
        refresh();
    }

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

    /// Set can_abort_subject_ from the current backend
    void update_abort_available();

    /// Populate section list rows from backend sections
    void populate_section_list();

    /// Create a single section row (icon + label + chevron)
    void create_section_row(lv_obj_t* parent, const helix::printer::DeviceSection& section);

    //
    // === State ===
    //

    /// Alias for overlay_root_ to match existing pattern
    helix::ui::WidgetRef& overlay_ = overlay_root_;

    /// Container for section list rows
    lv_obj_t* section_list_container_ = nullptr;

    /// Owns every subject below, and must be declared ahead of them and of
    /// bypass_toggle_ so it is torn down last. That teardown withdraws each
    /// XML-scope name before freeing the storage the name resolves to:
    /// StaticPanelRegistry::destroy_all() frees this overlay mid-process, and a
    /// name left behind hands the next lv_xml_create() that binds it freed
    /// memory (prestonbrown/helixscreen#1536).
    SubjectManager subjects_;

    /// Subject for system info text (e.g. "System: AFC · v1.2.3")
    lv_subject_t system_info_subject_{};

    /// Buffer for system info text
    char system_info_buf_[128] = {};

    /// Subject for bypass support (0=not supported, 1=supported).
    /// Folds in the force-bypass override, so this is what gates the controls.
    lv_subject_t supports_bypass_subject_{};

    /// Subject for the firmware's own bypass report, override NOT applied
    /// (0=firmware says none, 1=firmware reports one). Gates the override row:
    /// it appears only when the firmware says no, and stays visible once the
    /// user turns the override on so they can turn it back off.
    lv_subject_t fw_supports_bypass_subject_{};

    /// Subject for hardware bypass sensor (0=virtual toggle, 1=hardware sensor)
    lv_subject_t hw_bypass_sensor_subject_{};

    /// Subject for auto-heat support (0=not supported, 1=supported)
    lv_subject_t supports_auto_heat_subject_{};

    /// Subject for backend presence (0=no backend, 1=has backend)
    lv_subject_t has_backend_subject_{};

    /// 1 when the bypass lane is virtual (no physical bypass sensor); gates
    /// the always-show-bypass row.
    lv_subject_t bypass_is_virtual_subject_{};

    /// Subject gating the keep-spool-info-on-eject row (0=hidden, 1=shown).
    /// Set from AmsBackend::printer_reports_spool_ids(), so the row appears
    /// only on systems whose firmware reports spool ids per lane (AFC, Happy
    /// Hare); no backend means hidden.
    lv_subject_t reports_spool_ids_subject_{};
    lv_subject_t tags_name_spools_subject_{};

    /// Subject disabling the keep-spool-info-on-eject toggle (0=enabled,
    /// 1=firmware retention owns it). Set from
    /// AmsBackend::printer_retains_spool_info(): with AFC's per-lane
    /// remember_spool true everywhere, the toggle has no observable effect,
    /// so it is shown disabled with a note instead of silently lying.
    lv_subject_t printer_retains_spool_info_subject_{};

    /// Subject gating the "Reset Endless Spool" row (0=hidden, 1=shown).
    /// Set from EndlessSpoolCapabilities::editable(), so the row lights up for
    /// any backend whose endless-spool mapping the UI may write — AFC (per-slot
    /// edges), single-unit Happy Hare (groups) and the mock — and stays hidden
    /// for read-only systems (CFS, AD5X IFS) and backends with no endless spool.
    lv_subject_t can_reset_endless_spool_subject_{};

    /// Subject enabling Abort (0=disabled, 1=enabled). Set from
    /// AmsBackend::can_cancel_operation() on refresh and on every AMS action
    /// change, since what the backend can cancel follows what it is running.
    lv_subject_t can_abort_subject_{};

    /// Subjects enabling Home and Recover (0=disabled, 1=enabled), from
    /// AmsBackend::reset_available() / recover_available() on refresh.
    lv_subject_t can_home_subject_{};
    lv_subject_t can_recover_subject_{};

    /// ams_device_detail_rows() as "Label: value" lines, "" when there are none.
    lv_subject_t details_subject_{};
    char details_buf_[512] = {};
    lv_subject_t has_details_subject_{}; ///< 1 when details_subject_ is non-empty

    /// AMS action and can-cancel observers that keep can_abort_subject_
    /// current while the overlay exists.
    ObserverGuard action_observer_;
    ObserverGuard can_cancel_observer_;

    /// Cached section metadata for row click dispatch
    std::vector<helix::printer::DeviceSection> cached_sections_;

    /// Shared bypass policy — print guard, hardware-sensor refusal and the
    /// unload-first chain. One instance per surface, as in AmsOperationSidebar
    /// and BypassWidget; the switch handler owns nothing beyond forwarding to it.
    BypassToggleController bypass_toggle_;
};

inline AmsDeviceOperationsOverlay& get_ams_device_operations_overlay() {
    return lazy_global<AmsDeviceOperationsOverlay>("AmsDeviceOperationsOverlay");
}

} // namespace helix::ui
