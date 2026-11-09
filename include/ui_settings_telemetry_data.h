// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_telemetry_data.h
 * @brief Telemetry Data overlay - shows queued telemetry events for transparency
 *
 * This overlay lets users inspect exactly what anonymous telemetry data
 * would be sent. It displays queued events from TelemetryManager as
 * human-readable cards showing event type, timestamp, and payload.
 *
 * Users can:
 * - See all queued events and their contents
 * - Clear the queue to purge all pending data
 *
 * @pattern Overlay (lazy init, dynamic row creation)
 * @threading Main thread only
 *
 * @see TelemetryManager for event queue management
 * @see SettingsPanel for entry point (on_telemetry_view_data)
 */

#pragma once

#include "core/static_panel_registry.h"
#include "json_fwd.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <string>
#include <vector>

namespace helix::settings {

/// One line of an event card: its text and the color token it is drawn in.
struct TelemetryEventLine {
    std::string text;
    const char* color_token;
};

/// The lines a queued telemetry event shows on its card, heading first. A field
/// that is missing or of an unexpected type is left off rather than defaulted.
std::vector<TelemetryEventLine> describe_telemetry_event(const nlohmann::json& event);

/**
 * @class TelemetryDataOverlay
 * @brief Overlay for displaying queued telemetry events
 *
 * This overlay provides:
 * - Status card showing telemetry state and event count
 * - List of event cards with type, timestamp, and key fields
 * - Clear queue button to purge all pending events
 * - Empty state when no events are queued
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_telemetry_data_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class TelemetryDataOverlay : public OverlayBase {
  public:
    ~TelemetryDataOverlay() override;

    void init_subjects() override;

    const char* get_name() const override {
        return "Telemetry Data";
    }
    const char* xml_component() const override {
        return "telemetry_data_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;

    /// Populates the status card and event list on each open.
    void on_activate() override;

    /// Purge the queued events and refresh the display.
    void handle_clear_queue();

  private:
    //
    // === Internal Methods ===
    //

    /**
     * @brief Populate event list from TelemetryManager queue snapshot
     */
    void populate_events();

    /**
     * @brief Update status card text based on current state
     */
    void update_status();

    //
    // === Subjects ===
    //

    /// RAII manager for automatic subject cleanup
    SubjectManager subjects_;

    /// Status text subjects
    lv_subject_t status_subject_{};
    lv_subject_t detail_subject_{};
    lv_subject_t count_subject_{};

    char status_buf_[64];
    char detail_buf_[128];
};

/**
 * @brief Global instance accessor
 * @return Reference to singleton TelemetryDataOverlay
 */
inline TelemetryDataOverlay& get_telemetry_data_overlay() {
    return lazy_global<TelemetryDataOverlay>("TelemetryDataOverlay");
}

} // namespace helix::settings
