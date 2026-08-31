// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file tool_state.cpp
 * @brief ToolState singleton — models physical print heads (tools)
 *
 * Manages tool discovery from PrinterDiscovery and status updates
 * from Klipper's toolchanger / tool objects.
 */

#include "tool_state.h"

#include "ui_update_queue.h"

#include "ams_state.h"
#include "data_root_resolver.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "klipper_extruder_naming.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_discovery.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"
#include "tool_offsets.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace helix {

namespace {

/// Refuse a mutation that would publish into subjects that do not exist yet.
///
/// Every ToolState mutator ends in lv_subject_set_int(). Before
/// init_subjects() those subjects are still zeroed (LV_SUBJECT_TYPE_INVALID),
/// so LVGL drops each write and only warns, while the plain members the same
/// call rebuilt keep the new value. That divergence is invisible: tools_ says
/// four tools and tool_count says zero, and nothing republishes until some
/// unrelated path happens to rebuild the list. Applying nothing is the only
/// outcome that cannot diverge.
bool subjects_ready(bool initialized, const char* what) {
    if (initialized) {
        return true;
    }
    spdlog::error("[ToolState] {} before init_subjects() - ignored", what);
    return false;
}

} // namespace

ToolState& ToolState::instance() {
    static ToolState instance;
    return instance;
}

void ToolState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[ToolState] Subjects already initialized, skipping");
        return;
    }

    // Persist tool_spools.json to the user-writable config dir, unless a
    // caller already pinned one via set_config_dir().
    if (!config_dir_explicit_) {
        config_dir_ = helix::get_user_config_dir();
    }

    spdlog::trace("[ToolState] Initializing subjects (register_xml={})", register_xml);

    INIT_SUBJECT_INT(active_tool, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(tool_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(tools_version, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(tool_badge_text, "", subjects_, register_xml);
    INIT_SUBJECT_INT(show_tool_badge, 0, subjects_, register_xml);

    // Per-tool z-offset. Defaults say "this printer has none and we know
    // nothing", which is the correct answer until init_tools() sees the
    // hardware and a status frame carries a value.
    INIT_SUBJECT_INT(per_tool_z_supported, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(active_tool_z_offset, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(active_tool_z_offset_valid, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(any_tool_z_dirty, 0, subjects_, register_xml);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "ToolState", []() { ToolState::instance().deinit_subjects(); });

    spdlog::trace("[ToolState] Subjects initialized successfully");
}

void ToolState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[ToolState] Deinitializing subjects");

    // Death signal BEFORE the subjects go away: deinit frees every observer
    // node on them, so outside ObserverGuards must learn they are gone or their
    // next reset() calls lv_observer_remove() on freed memory. Replaced, not
    // cleared — an empty token reads as "dead" and would suppress removal for
    // observers registered after this teardown.
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }
    subjects_lifetime_ = std::make_shared<bool>(true);

    // Expire the in-flight Moonraker-DB callbacks before the subjects they
    // ultimately notify go away (#1165, #1146).
    async_lifetime_.invalidate();

    tools_.clear();
    active_tool_index_ = 0;
    spool_assignments_loaded_ = false;

    // Drop any AMS-backend override so the next init_subjects() / init_tools()
    // starts from a clean extruder-enumerated state. Without this, test fixtures
    // (and reconnect paths) inherit stale ams_topology_active_=true, which then
    // causes sync_from_backend() on a non-multiplexing backend to wipe tools_.
    ams_topology_active_ = false;
    ams_topology_tool_count_ = 0;
    ams_topology_tool_to_slot_.clear();
    ams_topology_tool_name_prefix_ = "T";

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void ToolState::init_tools(const helix::PrinterDiscovery& hardware) {
    // Clear existing tools
    tools_.clear();

    // init_tools enumerates tools from extruders/toolchanger — semantically
    // incompatible with an AMS-backend topology override. Drop any prior
    // override so callers see a consistent extruder-derived list.
    ams_topology_active_ = false;
    ams_topology_tool_count_ = 0;
    ams_topology_tool_to_slot_.clear();
    ams_topology_tool_name_prefix_ = "T";

    // Whether this printer keeps a z-offset per toolhead. Asked once, here,
    // because this is the only place ToolState sees the hardware; the status
    // path has no PrinterDiscovery to hand.
    const bool per_tool_z = helix::tool_offsets::supports_per_tool_z(hardware);
    lv_subject_set_int(&per_tool_z_supported_, per_tool_z ? 1 : 0);
    if (per_tool_z) {
        spdlog::info("[ToolState] Per-tool z-offset via {}",
                     helix::tool_offsets::provider_name(hardware));
    }

    if (hardware.has_snapmaker()) {
        // Snapmaker U1: 4 fixed toolheads, not using viesturz tool objects
        static const std::string extruder_names[] = {"extruder", "extruder1", "extruder2",
                                                     "extruder3"};
        for (int i = 0; i < 4; ++i) {
            ToolInfo tool;
            tool.index = i;
            tool.name = fmt::format("T{}", i);
            tool.extruder_name = extruder_names[i];
            tool.heater_name = extruder_names[i];
            tool.fan_name = (i == 0)
                                ? std::optional<std::string>("fan")
                                : std::optional<std::string>(fmt::format("fan_generic e{}_fan", i));
            spdlog::debug("[ToolState] Snapmaker tool {}: extruder={}, fan={}", i,
                          tool.extruder_name.value_or("none"), tool.fan_name.value_or("none"));
            tools_.push_back(std::move(tool));
        }
    } else if (!hardware.tool_names().empty()) {
        // Tool changer: create N tools from discovered tool names.
        //
        // Not gated on has_tool_changer() any more. Tool names only ever come
        // from [tool N] objects or, on a changer whose extra does the swapping
        // itself, from the extruder enumeration discovery ran for it - both mean
        // a changer, and the fork case has no [toolchanger] object to gate on.
        const auto& tool_names = hardware.tool_names();

        // Collect extruder names from heaters (sorted for index mapping)
        std::vector<std::string> extruder_names;
        for (const auto& h : hardware.heaters()) {
            if (helix::is_extruder_name(h)) {
                extruder_names.push_back(h);
            }
        }
        std::sort(extruder_names.begin(), extruder_names.end());

        for (int i = 0; i < static_cast<int>(tool_names.size()); ++i) {
            ToolInfo tool;
            tool.index = i;
            tool.name = tool_names[i];

            // Map extruder by index if available
            if (i < static_cast<int>(extruder_names.size())) {
                tool.extruder_name = extruder_names[i];
            } else {
                tool.extruder_name = std::nullopt;
            }

            tool.heater_name = std::nullopt;
            tool.fan_name = std::nullopt;

            spdlog::debug("[ToolState] Tool {}: name={}, extruder={}", i, tool.name,
                          tool.extruder_name.value_or("none"));
            tools_.push_back(std::move(tool));
        }
    } else {
        // No tool changer: enumerate extruder heaters to support multi-extruder setups
        std::vector<std::string> extruder_names;
        for (const auto& h : hardware.heaters()) {
            if (helix::is_extruder_name(h)) {
                // Deduplicate (mock can produce duplicates from dual parse_objects calls)
                if (std::find(extruder_names.begin(), extruder_names.end(), h) ==
                    extruder_names.end()) {
                    extruder_names.push_back(h);
                }
            }
        }
        std::sort(extruder_names.begin(), extruder_names.end());
        if (extruder_names.empty())
            extruder_names.push_back("extruder");

        for (int i = 0; i < static_cast<int>(extruder_names.size()); ++i) {
            ToolInfo tool;
            tool.index = i;
            tool.name = ::fmt::format("T{}", i);
            tool.extruder_name = extruder_names[i];
            tool.heater_name = std::nullopt;
            tool.fan_name = (i == 0) ? std::optional<std::string>("fan") : std::nullopt;
            tool.active = (i == 0);

            spdlog::debug("[ToolState] Tool {}: name={}, extruder={}", i, tool.name,
                          tool.extruder_name.value_or("none"));
            tools_.push_back(std::move(tool));
        }
    }

    active_tool_index_ = 0;

    // Update subjects
    lv_subject_set_int(&tool_count_, static_cast<int>(tools_.size()));
    lv_subject_set_int(&active_tool_, active_tool_index_);
    // tools_ was just rebuilt, so every dirty flag it carried is gone. Without
    // this the subject keeps its old value forever on a printer that no longer
    // has per-tool offsets — update_from_status()'s recompute is gated on
    // per_tool_z_supported_, which init_tools() may have just set to 0.
    refresh_any_tool_z_dirty();
    int version = lv_subject_get_int(&tools_version_) + 1;
    lv_subject_set_int(&tools_version_, version);

    // Tool badge formatting handled by UI-layer observer on tools_version_

    spdlog::info("[ToolState] Initialized {} tools (version {})", tools_.size(), version);
}

void ToolState::set_ams_topology(const ToolTopology& topo) {
    if (!subjects_ready(subjects_initialized_, "set_ams_topology()")) {
        return;
    }

    bool needs_rebuild = !ams_topology_active_ || ams_topology_tool_count_ != topo.tool_count ||
                         ams_topology_tool_to_slot_ != topo.tool_to_slot ||
                         ams_topology_tool_name_prefix_ != topo.tool_name_prefix;

    ams_topology_active_ = true;
    ams_topology_tool_count_ = topo.tool_count;
    ams_topology_tool_to_slot_ = topo.tool_to_slot;
    ams_topology_tool_name_prefix_ = topo.tool_name_prefix;

    if (needs_rebuild) {
        // Snapshot per-tool hardware mappings populated by init_tools() so we can
        // preserve them across the rebuild. Without this, ToolChanger printers
        // (which answer owns_tool_mapping_table() true and so trigger this
        // rebuild) lose their per-tool extruder/heater/fan assignments and revert
        // to the ToolInfo default extruder_name="extruder", breaking heater/fan
        // control.
        std::vector<ToolInfo> previous = std::move(tools_);
        tools_.clear();
        tools_.reserve(topo.tool_count);
        for (int i = 0; i < topo.tool_count; ++i) {
            ToolInfo t;
            t.index = i;
            t.name = ::fmt::format("{}{}", topo.tool_name_prefix, i);
            t.backend_index = topo.backend_index;
            t.backend_slot =
                (i < static_cast<int>(topo.tool_to_slot.size())) ? topo.tool_to_slot[i] : -1;
            if (i < static_cast<int>(previous.size())) {
                // Carry over hardware mappings init_tools set up.
                t.extruder_name = previous[i].extruder_name;
                t.heater_name = previous[i].heater_name;
                t.fan_name = previous[i].fan_name;
                t.gcode_x_offset = previous[i].gcode_x_offset;
                t.gcode_y_offset = previous[i].gcode_y_offset;
                t.gcode_z_offset = previous[i].gcode_z_offset;
                t.gcode_z_offset_known = previous[i].gcode_z_offset_known;
                t.gcode_z_offset_saved = previous[i].gcode_z_offset_saved;
                // And the spool record, but ONLY while this tool still sources
                // the same lane. Which spool is mounted is durable user data and
                // a rebuild has no business discarding it — dropping it wholesale
                // zeroed every assignment the moment a table-owning backend first
                // published its topology. But it is a fact about the LANE, so a
                // tool that changed lanes has no claim on the old record.
                //
                // Carrying it by index regardless is worse than dropping it. A
                // remap evicts both losing sides (assign_tool_slot in
                // ams_tool_map_sync.h), so it routinely leaves one tool mapped to
                // no lane at all — and AmsState::sync_from_backend()'s slot->tool
                // bridge skips slots whose mapped_tool is < 0, so nothing would
                // ever correct that tool. It would keep a spool that is now
                // driving a different head, and on a backend without firmware
                // spool persistence that phantom is written to tool_spools.json
                // and the Moonraker DB and outlives a restart.
                //
                // backend_slot < 0 on the previous entry means no topology had
                // been published yet: the record was assigned against the tool
                // itself, so it is still the tool's own.
                const bool same_lane =
                    previous[i].backend_slot < 0 || previous[i].backend_slot == t.backend_slot;
                if (same_lane) {
                    t.spoolman_id = previous[i].spoolman_id;
                    t.spool_name = previous[i].spool_name;
                    t.remaining_weight_g = previous[i].remaining_weight_g;
                    t.total_weight_g = previous[i].total_weight_g;
                }
            }
            tools_.push_back(std::move(t));
        }
        lv_subject_set_int(&tool_count_, static_cast<int>(tools_.size()));
        int version = lv_subject_get_int(&tools_version_) + 1;
        lv_subject_set_int(&tools_version_, version);
        spdlog::info("[ToolState] AMS topology applied: {} tools (version {})", tools_.size(),
                     version);
    }

    int new_active = topo.active_tool;
    if (new_active < 0 || new_active >= static_cast<int>(tools_.size())) {
        new_active = 0; // Out-of-range falls back to T0 (matches init_tools convention)
    }
    if (new_active != active_tool_index_) {
        active_tool_index_ = new_active;
        lv_subject_set_int(&active_tool_, active_tool_index_);
        spdlog::debug("[ToolState] AMS topology active tool: T{}", active_tool_index_);
    }
}

void ToolState::clear_ams_topology() {
    if (!subjects_ready(subjects_initialized_, "clear_ams_topology()")) {
        return;
    }
    if (!ams_topology_active_)
        return;
    ams_topology_active_ = false;
    ams_topology_tool_count_ = 0;
    ams_topology_tool_to_slot_.clear();
    ams_topology_tool_name_prefix_ = "T";
    tools_.clear();
    active_tool_index_ = 0;
    lv_subject_set_int(&tool_count_, 0);
    lv_subject_set_int(&active_tool_, 0);
    // Same reason as init_tools(): the flags died with tools_.
    refresh_any_tool_z_dirty();
    refresh_active_tool_z_offset();
    int version = lv_subject_get_int(&tools_version_) + 1;
    lv_subject_set_int(&tools_version_, version);
    spdlog::info("[ToolState] AMS topology cleared");
}

void ToolState::update_from_status(const nlohmann::json& status) {
    if (tools_.empty()) {
        return;
    }

    bool changed = false;

    // Parse active tool from toolchanger object.
    // When AMS owns the active tool (e.g., AFC), ignore Klipper's toolchanger
    // view — the logical T-number is driven by the backend, not the printer.
    if (!ams_topology_active_) {
        if (status.contains("toolchanger") && status["toolchanger"].is_object()) {
            const auto& tc = status["toolchanger"];
            if (tc.contains("tool_number") && tc["tool_number"].is_number_integer()) {
                int new_index = tc["tool_number"].get<int>();
                if (new_index != active_tool_index_) {
                    active_tool_index_ = new_index;
                    lv_subject_set_int(&active_tool_, active_tool_index_);
                    changed = true;
                    spdlog::debug("[ToolState] Active tool changed to {}", active_tool_index_);
                }
            }
        }
    }

    // Cross-check active tool from toolhead.extruder field.
    // This handles non-toolchanger multi-extruder setups where the active
    // extruder changes but there's no "toolchanger" object in status.
    // When AMS owns the active tool, ignore this too — the backend's tool index
    // is the source of truth, not Klipper's view of the physical extruder.
    if (!ams_topology_active_) {
        if (status.contains("toolhead") && status["toolhead"].is_object()) {
            const auto& toolhead = status["toolhead"];
            if (toolhead.contains("extruder") && toolhead["extruder"].is_string()) {
                std::string ext_name = toolhead["extruder"].get<std::string>();
                // Find which tool maps to this extruder
                for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
                    if (tools_[i].extruder_name.has_value() &&
                        tools_[i].extruder_name.value() == ext_name) {
                        if (i != active_tool_index_) {
                            active_tool_index_ = i;
                            lv_subject_set_int(&active_tool_, active_tool_index_);
                            changed = true;
                            spdlog::debug(
                                "[ToolState] Active tool updated to {} (from toolhead.extruder={})",
                                i, ext_name);
                        }
                        break;
                    }
                }
            }
        }
    }

    // Parse per-tool status updates
    for (auto& tool : tools_) {
        std::string key = "tool " + tool.name;
        if (!status.contains(key) || !status[key].is_object()) {
            continue;
        }
        const auto& tool_status = status[key];

        if (tool_status.contains("active") && tool_status["active"].is_boolean()) {
            bool val = tool_status["active"].get<bool>();
            if (val != tool.active) {
                tool.active = val;
                changed = true;
            }
        }

        if (tool_status.contains("mounted") && tool_status["mounted"].is_boolean()) {
            bool val = tool_status["mounted"].get<bool>();
            if (val != tool.mounted) {
                tool.mounted = val;
                changed = true;
            }
        }

        if (tool_status.contains("detect_state") && tool_status["detect_state"].is_string()) {
            std::string ds = tool_status["detect_state"].get<std::string>();
            DetectState new_state = DetectState::UNAVAILABLE;
            if (ds == "present") {
                new_state = DetectState::PRESENT;
            } else if (ds == "absent") {
                new_state = DetectState::ABSENT;
            }
            if (new_state != tool.detect_state) {
                tool.detect_state = new_state;
                changed = true;
            }
        }

        if (tool_status.contains("gcode_x_offset") && tool_status["gcode_x_offset"].is_number()) {
            tool.gcode_x_offset = tool_status["gcode_x_offset"].get<float>();
            changed = true;
        }
        if (tool_status.contains("gcode_y_offset") && tool_status["gcode_y_offset"].is_number()) {
            tool.gcode_y_offset = tool_status["gcode_y_offset"].get<float>();
            changed = true;
        }
        // gcode_z_offset is NOT parsed here: which store is authoritative is a
        // per-firmware question helix::tool_offsets owns, and on a MedusaHC the
        // value on this object is not the one the machine prints with. The
        // per-tool z-offset is read from the whole frame below.

        if (tool_status.contains("extruder") && tool_status["extruder"].is_string()) {
            std::string ext = tool_status["extruder"].get<std::string>();
            std::optional<std::string> new_val = ext.empty() ? std::nullopt : std::optional(ext);
            if (new_val != tool.extruder_name) {
                tool.extruder_name = new_val;
                changed = true;
            }
        }

        if (tool_status.contains("fan") && tool_status["fan"].is_string()) {
            std::string fan = tool_status["fan"].get<std::string>();
            std::optional<std::string> new_val = fan.empty() ? std::nullopt : std::optional(fan);
            if (new_val != tool.fan_name) {
                tool.fan_name = new_val;
                changed = true;
            }
        }
    }

    // Per-tool z-offset, in its own pass over tools_ rather than inside the loop
    // above. That loop skips a tool whose `tool T<n>` object is not in this
    // frame, and on a firmware that keeps every tool's offset on ONE object
    // (see helix::tool_offsets) that would skip the offsets entirely.
    if (lv_subject_get_int(&per_tool_z_supported_) == 1) {
        for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
            auto microns = helix::tool_offsets::read_tool_z_microns(status, i, tools_[i].name);
            if (!microns) {
                // No news. Moonraker republishes only what CHANGED, so this is
                // routine and must not be read as a reset to zero.
                continue;
            }
            float mm = static_cast<float>(*microns) / 1000.0f;
            if (!tools_[i].gcode_z_offset_known) {
                // First value seen for this tool is the persisted one: on a
                // fresh connect the runtime offset IS what the config holds.
                // Seeding the baseline here is what stops a freshly-connected
                // printer from claiming unsaved work it does not have.
                tools_[i].gcode_z_offset_saved = mm;
            }
            if (tools_[i].gcode_z_offset != mm || !tools_[i].gcode_z_offset_known) {
                tools_[i].gcode_z_offset = mm;
                tools_[i].gcode_z_offset_known = true;
                changed = true;
            }
        }
        // After the reads, so this covers both a new value arriving and the
        // active tool having changed in this same frame with no value of its
        // own — otherwise the panel would keep showing the previous tool's
        // number beside the new selection.
        refresh_active_tool_z_offset();
        refresh_any_tool_z_dirty();
    }

    if (changed) {
        // Tool badge formatting handled by UI-layer observer on tools_version_
        int version = lv_subject_get_int(&tools_version_) + 1;
        lv_subject_set_int(&tools_version_, version);
        spdlog::trace("[ToolState] Status updated, version {}", version);
    }
}

void ToolState::query_tool_z_offsets(IMoonrakerClient* client,
                                     const helix::PrinterDiscovery& hardware) {
    if (!client || tools_.empty() || lv_subject_get_int(&per_tool_z_supported_) != 1) {
        return;
    }

    // Every tool's own object, plus whatever else the firmware keeps its
    // offsets in. The module owns the second list so a machine that stores all
    // four on one macro is covered without naming it here.
    nlohmann::json objects = nlohmann::json::object();
    for (const auto& tool : tools_) {
        objects["tool " + tool.name] = nullptr;
    }
    for (const auto& obj : helix::tool_offsets::required_status_objects(hardware)) {
        objects[obj] = nullptr;
    }

    // The response lands on the WebSocket thread, so the parse is marshalled
    // back to the UI thread — update_from_status() writes subjects, and
    // lv_subject_set_int() off the main thread fires observers into LVGL
    // (CLAUDE.md § Threading invariant 1). bg_cb also drops the body if the
    // subjects were torn down while the request was in flight.
    auto cb =
        async_lifetime_.bg_cb("ToolState::query_tool_z_offsets", [this](nlohmann::json response) {
            if (!response.contains("result") || !response["result"].contains("status")) {
                spdlog::debug("[ToolState] Tool z-offset query returned no status");
                return;
            }
            update_from_status(response["result"]["status"]);
            spdlog::debug("[ToolState] Seeded per-tool z-offsets from query");
        });
    client->send_jsonrpc("printer.objects.query", nlohmann::json{{"objects", objects}},
                         std::move(cb));
}

std::vector<int> ToolState::dirty_tool_z_indices() const {
    std::vector<int> dirty;
    for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
        if (tools_[i].gcode_z_offset_known &&
            tools_[i].gcode_z_offset != tools_[i].gcode_z_offset_saved) {
            dirty.push_back(i);
        }
    }
    return dirty;
}

float ToolState::tool_z_offset_mm(int tool_index) const {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size()) ||
        !tools_[tool_index].gcode_z_offset_known) {
        return 0.0f;
    }
    return tools_[tool_index].gcode_z_offset;
}

void ToolState::set_tool_z_offset_local(int tool_index, int microns) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        return;
    }
    tools_[tool_index].gcode_z_offset = static_cast<float>(microns) / 1000.0f;
    tools_[tool_index].gcode_z_offset_known = true;
    if (tool_index == active_tool_index_) {
        refresh_active_tool_z_offset();
    }
    refresh_any_tool_z_dirty();
}

void ToolState::mark_tool_z_saved(int tool_index) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        return;
    }
    tools_[tool_index].gcode_z_offset_saved = tools_[tool_index].gcode_z_offset;
    refresh_any_tool_z_dirty();
}

void ToolState::refresh_any_tool_z_dirty() {
    const int dirty = dirty_tool_z_indices().empty() ? 0 : 1;
    if (lv_subject_get_int(&any_tool_z_dirty_) != dirty) {
        lv_subject_set_int(&any_tool_z_dirty_, dirty);
    }
}

void ToolState::refresh_active_tool_z_offset() {
    const bool have = active_tool_index_ >= 0 &&
                      active_tool_index_ < static_cast<int>(tools_.size()) &&
                      tools_[active_tool_index_].gcode_z_offset_known;

    // valid_ is latched separately because 0 microns is a legitimate offset and
    // cannot double as "nothing known" — the UI needs to tell a tool sitting at
    // zero from one that has never reported. Dropping it back to 0 matters as
    // much as raising it: on a tool change to a tool we have no value for, the
    // previous tool's number must not stay on screen beside the new selection.
    const int microns =
        have ? static_cast<int>(std::lround(tools_[active_tool_index_].gcode_z_offset * 1000.0f))
             : 0;

    if (lv_subject_get_int(&active_tool_z_offset_) != microns) {
        lv_subject_set_int(&active_tool_z_offset_, microns);
    }
    const int valid = have ? 1 : 0;
    if (lv_subject_get_int(&active_tool_z_offset_valid_) != valid) {
        lv_subject_set_int(&active_tool_z_offset_valid_, valid);
    }
}

int ToolState::extruder_count() const {
    std::set<std::string> names;
    for (const auto& t : tools_) {
        if (t.extruder_name && !t.extruder_name->empty()) {
            names.insert(*t.extruder_name);
        }
    }
    return static_cast<int>(names.size());
}

const ToolInfo* ToolState::active_tool() const {
    if (active_tool_index_ < 0 || active_tool_index_ >= static_cast<int>(tools_.size())) {
        return nullptr;
    }
    return &tools_[active_tool_index_];
}

std::string ToolState::nozzle_label() const {
    // Gated on physical extruders, not tool count: the label sits beside a
    // nozzle temperature readout in the controls and filament panels, so it
    // answers "which nozzle is this". set_ams_topology() expands tools_ to one
    // entry per filament lane, and every lane on a single-hotend printer feeds
    // the same nozzle - naming it after the loaded lane says nothing. Matches
    // the nozzle_icon badge gate in ui_ams_tool_text.
    if (!has_multiple_extruders()) {
        return lv_tr("Nozzle");
    }
    const auto* tool = active_tool();
    if (tool) {
        return std::string(lv_tr("Nozzle")) + " " + tool->name;
    }
    return lv_tr("Nozzle");
}

// Tool badge formatting moved to UI layer (ui_ams_tool_text.cpp)

std::string ToolState::tool_name_for_extruder(const std::string& extruder_name) const {
    for (const auto& tool : tools_) {
        if (tool.extruder_name && *tool.extruder_name == extruder_name) {
            return tool.name;
        }
    }
    return {};
}

void ToolState::request_tool_change(int tool_index, IMoonrakerAPI* api,
                                    std::function<void()> on_success,
                                    std::function<void(const std::string&)> on_error) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        if (on_error)
            on_error(
                ::fmt::format("Invalid tool index {} (have {} tools)", tool_index, tools_.size()));
        return;
    }

    if (tool_index == active_tool_index_) {
        spdlog::debug("[ToolState] Tool {} already active, ignoring", tool_index);
        if (on_success)
            on_success();
        return;
    }

    // Try AMS backend first — it handles tool changes independently of the API
    // (e.g., AFC, Happy Hare, toolchanger backends).
    // Skip the backend if it has no slots configured (e.g., AFC module loaded but no hardware)
    // or if this tool isn't in the backend's tool-to-slot map.
    auto* backend = AmsState::instance().get_backend();
    if (backend) {
        AmsSystemInfo info = backend->get_system_info();
        bool backend_manages_tool = info.total_slots > 0 &&
                                    tool_index < static_cast<int>(info.tool_to_slot_map.size()) &&
                                    info.tool_to_slot_map[tool_index] >= 0;

        if (backend_manages_tool) {
            spdlog::info("[ToolState] Requesting tool change to T{} via AMS backend", tool_index);
            auto result = backend->change_tool(tool_index);
            if (result) {
                if (on_success)
                    on_success();
            } else {
                // display_text(), not user_msg: this callback is the only thing
                // the caller ever sees, so dropping the suggestion here means no
                // UI downstream can render it however good its toast is.
                if (on_error)
                    on_error(result.display_text());
            }
            return;
        }

        spdlog::debug("[ToolState] AMS backend present but doesn't manage T{}, using direct gcode",
                      tool_index);
    }

    if (!api) {
        if (on_error)
            on_error("No API connection");
        return;
    }

    // Fallback: Tn gcode for multi-extruder and toolchanger setups.
    // Klipper auto-defines Tn → ACTIVATE_EXTRUDER for plain multi-extruder,
    // and toolchanger plugins (ktcc, tapchanger, etc.) override Tn with
    // proper physical tool change logic.
    std::string gcode = ::fmt::format("T{}", tool_index);
    spdlog::info("[ToolState] Requesting tool change to T{} via gcode", tool_index);

    api->execute_gcode(
        gcode,
        [on_success]() {
            if (on_success)
                on_success();
        },
        [on_error](const MoonrakerError& error) {
            if (on_error)
                on_error(error.user_message());
        });
}

// ============================================================================
// Spool assignment persistence
// ============================================================================

static constexpr const char* SPOOL_JSON_FILENAME = "tool_spools.json";
static constexpr const char* MOONRAKER_DB_NAMESPACE = "helix-screen";
static constexpr const char* MOONRAKER_DB_KEY = "tool_spool_assignments";

/**
 * @brief Whether two spool weights render identically.
 *
 * Weights reach the UI as whole grams, so anything finer is noise nothing
 * downstream can observe. Compares against the LAST STORED value rather than a
 * running accumulator, so a slow slide still fires exactly once per gram.
 * The -1.0f "unknown" sentinel and non-finite values compare by exact equality
 * so they never round into a real weight.
 */
static bool same_displayed_weight(float a, float b) {
    if (!std::isfinite(a) || !std::isfinite(b) || a < 0.0f || b < 0.0f) {
        return a == b || (!std::isfinite(a) && !std::isfinite(b));
    }
    return std::lround(a) == std::lround(b);
}

void ToolState::assign_spool(int tool_index, int spoolman_id, const std::string& spool_name,
                             float remaining_g, float total_g) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        // Normal on single-extruder AFC/MMU setups where lanes map to virtual
        // tools (T0-T3) but only one real extruder exists
        spdlog::trace("[ToolState] assign_spool: skipping tool index {} (have {} tools)",
                      tool_index, tools_.size());
        return;
    }

    auto& tool = tools_[tool_index];

    // Which spool is mounted. This is the durable half of the record: it is what
    // survives a restart and what tool_spools.json plus the Moonraker DB key
    // exist to remember.
    const bool identity_changed = tool.spoolman_id != spoolman_id || tool.spool_name != spool_name;

    // Weights are a cache. Firmware reports them as continuous floats — an AFC
    // lane sends e.g. 627.685056380799 g and moves by hundredths on every status
    // update — so an exact compare treats sensor noise as a change. AmsState
    // calls this from the status path and then saves if dirty, which on bundle
    // L53W5PKG meant 590 rewrites of tool_spools.json, 590 Moonraker DB POSTs
    // and 590 filament-panel rebuilds in one session. Compare at whole grams,
    // the resolution the UI actually renders.
    const bool weight_changed = !same_displayed_weight(tool.remaining_weight_g, remaining_g) ||
                                !same_displayed_weight(tool.total_weight_g, total_g);

    if (!identity_changed && !weight_changed) {
        return;
    }

    tool.spoolman_id = spoolman_id;
    tool.spool_name = spool_name;
    tool.remaining_weight_g = remaining_g;
    tool.total_weight_g = total_g;

    // Persist only for an identity change. A weight refresh is re-fetched from
    // AFC/Spoolman within seconds of the next connect, so writing flash and
    // POSTing the DB for each gram consumed during a print buys nothing.
    if (identity_changed) {
        spool_dirty_ = true;
        spdlog::info("[ToolState] Assigned spool {} ({}) to tool {}", spoolman_id, spool_name,
                     tool_index);
    } else {
        spdlog::debug("[ToolState] Spool {} on tool {} now {:.0f}g", spoolman_id, tool_index,
                      remaining_g);
    }

    // Bump version so UI observers update
    if (subjects_initialized_) {
        int version = lv_subject_get_int(&tools_version_) + 1;
        lv_subject_set_int(&tools_version_, version);
    }
}

void ToolState::clear_spool(int tool_index) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        spdlog::warn("[ToolState] clear_spool: invalid tool index {}", tool_index);
        return;
    }

    auto& tool = tools_[tool_index];

    // Skip if already cleared
    if (tool.spoolman_id == 0) {
        return;
    }

    tool.spoolman_id = 0;
    tool.spool_name.clear();
    tool.remaining_weight_g = -1;
    tool.total_weight_g = -1;
    spool_dirty_ = true;

    spdlog::info("[ToolState] Cleared spool assignment for tool {}", tool_index);

    if (subjects_initialized_) {
        int version = lv_subject_get_int(&tools_version_) + 1;
        lv_subject_set_int(&tools_version_, version);
    }
}

std::set<int> ToolState::assigned_spool_ids(int exclude_tool) const {
    std::set<int> ids;
    for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
        if (i == exclude_tool)
            continue;
        if (tools_[i].spoolman_id > 0) {
            ids.insert(tools_[i].spoolman_id);
        }
    }
    return ids;
}

nlohmann::json ToolState::spool_assignments_to_json() const {
    nlohmann::json result = nlohmann::json::object();

    for (const auto& tool : tools_) {
        if (tool.spoolman_id <= 0)
            continue;

        nlohmann::json entry;
        entry["spoolman_id"] = tool.spoolman_id;
        entry["spool_name"] = tool.spool_name;
        if (std::isfinite(tool.remaining_weight_g) && tool.remaining_weight_g >= 0)
            entry["remaining_weight_g"] = tool.remaining_weight_g;
        if (std::isfinite(tool.total_weight_g) && tool.total_weight_g >= 0)
            entry["total_weight_g"] = tool.total_weight_g;

        result[std::to_string(tool.index)] = entry;
    }

    return result;
}

void ToolState::apply_spool_assignments(const nlohmann::json& data) {
    if (!data.is_object() || data.empty()) {
        // Normal when spoolman is unconfigured or DB key doesn't exist yet
        spdlog::debug("[ToolState] apply_spool_assignments: no spool data (type={})",
                      data.type_name());
        return;
    }

    for (auto& tool : tools_) {
        auto key = std::to_string(tool.index);
        if (!data.contains(key) || !data[key].is_object()) {
            continue;
        }

        const auto& entry = data[key];
        tool.spoolman_id = entry.value("spoolman_id", 0);
        tool.spool_name = entry.value("spool_name", std::string{});
        tool.remaining_weight_g = json_util::safe_float(entry, "remaining_weight_g", -1.0f);
        tool.total_weight_g = json_util::safe_float(entry, "total_weight_g", -1.0f);

        if (tool.spoolman_id > 0) {
            spdlog::debug("[ToolState] Loaded spool {} ({}) for tool {}", tool.spoolman_id,
                          tool.spool_name, tool.index);
        }
    }
}

void ToolState::save_spool_json() const {
    namespace fs = std::filesystem;

    auto json_data = spool_assignments_to_json();
    auto path = fs::path(config_dir_) / SPOOL_JSON_FILENAME;

    try {
        // Ensure directory exists
        fs::create_directories(config_dir_);

        // Resolve symlinks so the atomic rename below targets the real file rather
        // than replacing the link. The installer symlinks this file out to
        // printer_data (HELIX_USER_CONFIG_FILES), and that link is the only thing
        // keeping it alive through Moonraker's update, which rmtree()s the install
        // dir. rename(2) onto a symlink replaces the symlink itself, so without
        // this the first save silently strands the file in the doomed directory.
        // Mirrors Config::save().
        {
            std::error_code ec;
            if (fs::is_symlink(path, ec)) {
                auto real = fs::canonical(path, ec);
                if (!ec) {
                    spdlog::debug("[ToolState] Resolved symlink {} -> {}", path.string(),
                                  real.string());
                    path = real;
                }
            }
        }

        // Atomic save: write to temp file, then rename to avoid partial writes on crash/power loss
        auto tmp_path = path;
        tmp_path += ".tmp";
        {
            std::ofstream ofs(tmp_path);
            if (!ofs.is_open()) {
                spdlog::error("[ToolState] Failed to open {} for writing: {}", tmp_path.string(),
                              strerror(errno));
                std::remove(tmp_path.c_str());
                return;
            }
            ofs << json_data.dump(2);
            ofs.flush();
            if (!ofs.good()) {
                spdlog::error("[ToolState] Failed to write spool JSON to {}: {}", tmp_path.string(),
                              strerror(errno));
                std::remove(tmp_path.c_str());
                return;
            }
        }

        if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
            spdlog::error("[ToolState] Failed to rename '{}' to '{}': {}", tmp_path.string(),
                          path.string(), strerror(errno));
            std::remove(tmp_path.c_str());
            return;
        }

        spdlog::debug("[ToolState] Saved spool assignments to {}", path.string());
    } catch (const std::exception& e) {
        spdlog::warn("[ToolState] Error saving spool JSON: {}", e.what());
    }
}

bool ToolState::load_spool_json() {
    namespace fs = std::filesystem;

    auto path = fs::path(config_dir_) / SPOOL_JSON_FILENAME;

    if (!fs::exists(path)) {
        spdlog::debug("[ToolState] No spool JSON file at {}", path.string());
        return false;
    }

    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            spdlog::warn("[ToolState] Failed to open {}", path.string());
            return false;
        }

        auto data = nlohmann::json::parse(ifs);
        apply_spool_assignments(data);
        spdlog::info("[ToolState] Loaded spool assignments from {}", path.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("[ToolState] Error loading spool JSON: {}", e.what());
        return false;
    }
}

void ToolState::save_spool_assignments_if_dirty(IMoonrakerAPI* api) {
    if (!spool_dirty_) {
        return;
    }
    save_spool_assignments(api);
}

void ToolState::save_spool_assignments(IMoonrakerAPI* api) {
    // Always save to local JSON (fast, reliable)
    save_spool_json();
    spool_dirty_ = false;

    // Fire-and-forget to Moonraker DB (async, best-effort)
    if (api) {
        auto json_data = spool_assignments_to_json();
        api->database_post_item(
            MOONRAKER_DB_NAMESPACE, MOONRAKER_DB_KEY, json_data,
            []() { spdlog::debug("[ToolState] Spool assignments saved to Moonraker DB"); },
            [](const MoonrakerError& err) {
                spdlog::warn("[ToolState] Failed to save to Moonraker DB: {}", err.user_message());
            });
    }
}

void ToolState::load_spool_assignments(IMoonrakerAPI* api) {
    if (spool_assignments_loaded_) {
        spdlog::debug("[ToolState] Spool assignments already loaded, skipping");
        return;
    }

    if (!api) {
        // No API — try local JSON only
        load_spool_json();
        spool_assignments_loaded_ = true;
        return;
    }

    // Try Moonraker DB first. Callbacks fire from WebSocket thread,
    // so we marshal back to UI thread via queue_update().
    // bg_cb decays the callback argument into the deferred lambda by value, so
    // it both marshals to the main thread and drops the body if the subjects
    // were torn down while the request was in flight (#1165). That subsumes the
    // manual unique_ptr payload copy this used to do by hand.
    api->database_get_item(
        MOONRAKER_DB_NAMESPACE, MOONRAKER_DB_KEY,
        async_lifetime_.bg_cb("ToolState::load_spool_assignments",
                              [this](const nlohmann::json& data) {
                                  apply_spool_assignments(data);
                                  save_spool_json();
                                  spool_assignments_loaded_ = true;
                                  // Re-sync AmsState so slot UI subjects reflect loaded assignments
                                  AmsState::instance().sync_from_backend();
                                  spdlog::info(
                                      "[ToolState] Loaded spool assignments from Moonraker DB");
                              }),
        async_lifetime_.bg_cb(
            "ToolState::load_spool_assignments_error", [this, api](const MoonrakerError& err) {
                spdlog::debug("[ToolState] Moonraker DB load failed ({}), trying local JSON",
                              err.user_message());
                load_spool_json();
                spool_assignments_loaded_ = true;
                // Seed Moonraker DB so subsequent connections don't hit 404
                save_spool_assignments(api);
                // Re-sync AmsState so slot UI subjects reflect loaded assignments
                AmsState::instance().sync_from_backend();
            }));
}

} // namespace helix
