// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "async_lifetime_guard.h"
#include "subject_managed_panel.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "hv/json.hpp"

class IMoonrakerAPI;

namespace helix {

// Forward declaration
class PrinterDiscovery;

enum class DetectState {
    PRESENT = 0,
    ABSENT = 1,
    UNAVAILABLE = 2,
};

struct ToolInfo {
    int index = 0;
    std::string name = "T0";
    std::optional<std::string> extruder_name = "extruder";
    std::optional<std::string> heater_name;
    std::optional<std::string> fan_name;
    float gcode_x_offset = 0.0f;
    float gcode_y_offset = 0.0f;
    float gcode_z_offset = 0.0f;
    bool active = false;
    bool mounted = false;
    DetectState detect_state = DetectState::UNAVAILABLE;
    int backend_index = -1; ///< Which AMS backend feeds this tool (-1 = direct drive)
    int backend_slot = -1;  ///< Fixed slot in that backend (-1 = any/dynamic)

    // Spoolman spool assignment (persisted per-tool)
    int spoolman_id = 0;           ///< Spoolman spool ID (0=not tracked)
    std::string spool_name;        ///< Display name from Spoolman
    float remaining_weight_g = -1; ///< Remaining weight in grams (-1=unknown)
    float total_weight_g = -1;     ///< Total spool weight in grams (-1=unknown)

    [[nodiscard]] std::string effective_heater() const {
        if (heater_name)
            return *heater_name;
        if (extruder_name)
            return *extruder_name;
        return "extruder";
    }
};

/// Tool topology sourced from an AMS backend that multiplexes tools (e.g. AFC).
/// When set, ToolState rebuilds its tool list from this and ignores extruder
/// enumeration. tool_to_slot[i] is the backend slot index that tool i sources.
struct ToolTopology {
    int tool_count = 0;
    int active_tool = -1;
    std::vector<int> tool_to_slot;
    std::string tool_name_prefix = "T"; ///< Generated names: "{prefix}{index}"
    int backend_index = 0;              ///< Source backend in AmsState::backends_
};

/// Manages tool information for multi-tool printers (toolchangers, multi-extruder).
/// Thread safety: All public methods must be called from the LVGL/UI thread only.
/// Subject updates are routed through helix::ui::queue_update() from background threads.
class ToolState {
  public:
    static ToolState& instance();
    ToolState(const ToolState&) = delete;
    ToolState& operator=(const ToolState&) = delete;

    void init_subjects(bool register_xml = true);
    void deinit_subjects();

    void init_tools(const helix::PrinterDiscovery& hardware);
    void update_from_status(const nlohmann::json& status);

    /// Push AMS-backend-derived topology. Overrides extruder-based init. Idempotent:
    /// rebuilds tools_ only when count or mapping changes; updates active_tool_index_
    /// every call. Must be called from the LVGL/UI thread.
    void set_ams_topology(const ToolTopology& topo);

    /// Remove the AMS topology override. tools_ is cleared; caller should follow
    /// with init_tools() to repopulate from extruders/toolchanger if appropriate.
    void clear_ams_topology();

    [[nodiscard]] bool ams_topology_active() const {
        return ams_topology_active_;
    }

    [[nodiscard]] const std::vector<ToolInfo>& tools() const {
        return tools_;
    }
    [[nodiscard]] const ToolInfo* active_tool() const;
    [[nodiscard]] int active_tool_index() const {
        return active_tool_index_;
    }
    [[nodiscard]] int tool_count() const {
        return static_cast<int>(tools_.size());
    }
    [[nodiscard]] bool is_multi_tool() const {
        return tools_.size() > 1;
    }

    /// Number of distinct physical extruders backing the current tool list.
    ///
    /// NOT interchangeable with tool_count(): set_ams_topology() expands tools_
    /// to one entry per filament SLOT, so a 4-slot AMS on a single-hotend
    /// printer reports 4 tools and 1 extruder. Only tools carrying an extruder
    /// name count, and duplicates collapse — the AMS rebuild leaves slots past
    /// the real extruder list with no mapping at all.
    [[nodiscard]] int extruder_count() const;

    /// Whether the printer physically has more than one hotend/extruder.
    ///
    /// The predicate for anything annotating a single nozzle with which tool it
    /// is (the nozzle_icon tool badge). is_multi_tool() is the wrong test there:
    /// it counts AMS slots fed into one hotend, which need no badge.
    [[nodiscard]] bool has_multiple_extruders() const {
        return extruder_count() > 1;
    }

    /// Returns "Nozzle" for single-tool, "Nozzle T0" for multi-tool (active tool).
    [[nodiscard]] std::string nozzle_label() const;

    /// Request a tool change, delegating to AMS backend or falling back to Tn gcode.
    /// Callbacks are invoked asynchronously from the API response.
    void request_tool_change(int tool_index, IMoonrakerAPI* api,
                             std::function<void()> on_success = nullptr,
                             std::function<void(const std::string&)> on_error = nullptr);

    /// Returns tool name (e.g. "T0") for the given extruder name, or empty if not found.
    [[nodiscard]] std::string tool_name_for_extruder(const std::string& extruder_name) const;

    /// Assign a Spoolman spool to a tool. Persists to local JSON + Moonraker DB.
    void assign_spool(int tool_index, int spoolman_id, const std::string& spool_name = "",
                      float remaining_g = -1, float total_g = -1);

    /// Clear spool assignment for a tool
    void clear_spool(int tool_index);

    /// Get set of Spoolman spool IDs currently assigned to tools,
    /// optionally excluding one tool index (e.g., the one being edited).
    [[nodiscard]] std::set<int> assigned_spool_ids(int exclude_tool = -1) const;

    /// Load persisted spool assignments (Moonraker DB → local JSON → empty)
    void load_spool_assignments(IMoonrakerAPI* api);

    /// Save all spool assignments (local JSON + Moonraker DB fire-and-forget)
    void save_spool_assignments(IMoonrakerAPI* api);

    /// Save spool assignments only if data has changed since last save
    void save_spool_assignments_if_dirty(IMoonrakerAPI* api);

    /// True after load_spool_assignments() has completed (success or fallback).
    [[nodiscard]] bool spool_assignments_loaded() const {
        return spool_assignments_loaded_;
    }

    /// Set the config directory for local JSON persistence (default: "config").
    /// An explicit override wins over the value init_subjects() derives from
    /// helix::get_user_config_dir(), so re-initialising subjects can't silently
    /// move tool_spools.json back to the default location.
    void set_config_dir(const std::string& dir) {
        config_dir_ = dir;
        config_dir_explicit_ = true;
    }

    /// Directory tool_spools.json is read from and written to.
    [[nodiscard]] const std::string& get_config_dir() const {
        return config_dir_;
    }

    lv_subject_t* get_active_tool_subject() {
        return &active_tool_;
    }

    /**
     * @brief Death signal for the subjects this singleton owns.
     *
     * Long-lived observers outside ToolState — PrintStatusPanel watches
     * get_active_tool_subject() to re-render the nozzle temperature label — must
     * pass this to observe_*(). deinit_subjects() frees every observer node on
     * these subjects, and several tests call it mid-process, so a guard without
     * the token dereferences a freed observer on its next reset().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_lifetime_;
    }
    lv_subject_t* get_tool_count_subject() {
        return &tool_count_;
    }
    lv_subject_t* get_tools_version_subject() {
        return &tools_version_;
    }
    lv_subject_t* get_tool_badge_text_subject() {
        return &tool_badge_text_;
    }
    lv_subject_t* get_show_tool_badge_subject() {
        return &show_tool_badge_;
    }

  private:
    friend class ToolStateTestAccess;

    ToolState() = default;
    SubjectManager subjects_;
    /// See get_subjects_lifetime(). Created with the object and REPLACED (never
    /// nulled) by deinit_subjects(), so the accessor never hands out an empty
    /// token — an empty one reads as "dead" and would suppress removal for live
    /// observers instead of protecting dead ones.
    SubjectLifetime subjects_lifetime_ = std::make_shared<bool>(true);
    bool subjects_initialized_ = false;

    /// Expires the Moonraker-DB spool-assignment callbacks, which fire from the
    /// WebSocket thread long after the request was issued. Declared after
    /// `subjects_` so reverse-order member destruction invalidates it before the
    /// subjects it protects; also invalidated by deinit_subjects() (#1165, #1146).
    helix::AsyncLifetimeGuard async_lifetime_;
    lv_subject_t active_tool_{};
    lv_subject_t tool_count_{};
    lv_subject_t tools_version_{};

    // Tool badge subjects for nozzle_icon component (XML-bound).
    // Updated automatically by update_from_status() and init_tools().
    lv_subject_t tool_badge_text_{};
    char tool_badge_text_buf_[16] = {};
    lv_subject_t show_tool_badge_{};

    std::vector<ToolInfo> tools_;
    int active_tool_index_ = 0;
    std::string config_dir_ = "config";     ///< Directory for local JSON persistence
    bool config_dir_explicit_ = false;      ///< set_config_dir() pinned it; don't re-derive
    bool spool_dirty_ = false;              ///< True when spool data changed since last save
    bool spool_assignments_loaded_ = false; ///< True after load_spool_assignments() completes

    // AMS topology override (set by AMS backends that multiplex tools, e.g. AFC).
    // When ams_topology_active_ is true, tools_ is sourced from the backend's
    // lane->tool mapping rather than from PrinterDiscovery extruder enumeration.
    bool ams_topology_active_ = false;
    int ams_topology_tool_count_ = 0;
    std::vector<int> ams_topology_tool_to_slot_;
    std::string ams_topology_tool_name_prefix_ = "T";

    /// Save spool assignments to local JSON file
    void save_spool_json() const;

    /// Load spool assignments from local JSON file. Returns true on success.
    bool load_spool_json();

    /// Build JSON representation of current spool assignments
    [[nodiscard]] nlohmann::json spool_assignments_to_json() const;

    /// Apply spool assignments from JSON
    void apply_spool_assignments(const nlohmann::json& data);
};

} // namespace helix
