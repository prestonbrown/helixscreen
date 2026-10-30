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
#include "config.h"
#include "data_root_resolver.h"
#include "display_numbering.h"
#include "fixed_tool_topology.h"
#include "helix_fs.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "klipper_extruder_naming.h"
#include "lvgl.h"
#include "printer_cache_registry.h"
#include "printer_discovery.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"
#include "system/helix_paths.h"
#include "text_io.h"
#include "tool_offsets.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace helix {

namespace hfs = helix::fs;

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

/// "Tool 2", in the active backend's noun, for the tool at zero-based @p index.
std::string noun_tool_label(int index) {
    return helix::ui::lane_label(helix::ui::active_tool_noun(), index);
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

    // Per-tool offsets: which axes this printer keeps per toolhead, and the
    // aggregate save affordance. Defaults say "this printer has none and we
    // know nothing", which is the correct answer until init_tools() sees the
    // hardware and a status frame carries a value. The values themselves live
    // in tools_ and are read through the tool_offset_*() accessors.
    INIT_SUBJECT_INT(per_tool_x_supported, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(per_tool_y_supported, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(per_tool_z_supported, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(any_tool_offset_dirty, 0, subjects_, register_xml);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "ToolState", []() { ToolState::instance().deinit_subjects(); });

    // Spool assignments belong to the printer they were loaded from; the next one loads
    // its own. A reply still in flight (a spool load, a tool-offset query) belongs to the
    // previous printer: applied now it would land on, and be saved as, the next one's.
    helix::PrinterCacheRegistry::instance().register_invalidator("ToolState", []() {
        auto& ts = ToolState::instance();
        if (!ts.spool_load_printer_.empty()) {
            spdlog::info("[ToolState] spool load for {} dropped: printer scope ended",
                         ts.spool_load_printer_);
            ts.spool_load_printer_.clear();
        }
        ts.async_lifetime_.invalidate();
        ts.spool_assignments_loaded_ = false;
        // Unsaved changes were the previous printer's; saved now they would land under
        // the next printer's key and in its DB.
        ts.spool_dirty_ = false;
        ts.spool_set_adopted_ = false;
    });
    if (Config* config = Config::get_instance()) {
        config->set_printer_removed_hook(
            [](const std::string& id) { ToolState::instance().forget_printer_spools(id); });
    }

    spdlog::trace("[ToolState] Subjects initialized successfully");
}

void ToolState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[ToolState] Deinitializing subjects");
    // Expire the in-flight Moonraker-DB callbacks before the subjects they
    // ultimately notify go away (#1165, #1146).
    async_lifetime_.invalidate();

    tools_.clear();
    heads_cache_.reset();
    discovered_extruders_.clear();
    active_tool_index_ = 0;
    spool_assignments_loaded_ = false;
    spool_set_adopted_ = false;

    // Drop any AMS-backend override so the next init_subjects() / init_tools()
    // starts from a clean extruder-enumerated state. Without this, test fixtures
    // (and reconnect paths) inherit stale ams_topology_active_=true, which then
    // causes sync_from_backend() on a non-multiplexing backend to wipe tools_.
    ams_topology_active_ = false;
    backend_owns_active_tool_ = false;
    ams_topology_tool_count_ = 0;
    ams_topology_tool_to_slot_.clear();
    ams_topology_tool_extruder_.clear();

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void ToolState::refresh_display_labels() {
    for (auto& tool : tools_) {
        tool.display_label = noun_tool_label(tool.index);
    }
    lv_subject_set_int(&tools_version_, lv_subject_get_int(&tools_version_) + 1);
}

void ToolState::init_tools(const helix::PrinterDiscovery& hardware) {
    // Clear existing tools
    tools_.clear();

    // Every extruder heater, sorted for index mapping and deduplicated (the
    // mock can list one twice from two parse_objects calls).
    discovered_extruders_.clear();
    for (const auto& h : hardware.heaters()) {
        if (helix::is_extruder_name(h) &&
            std::find(discovered_extruders_.begin(), discovered_extruders_.end(), h) ==
                discovered_extruders_.end()) {
            discovered_extruders_.push_back(h);
        }
    }
    std::sort(discovered_extruders_.begin(), discovered_extruders_.end());

    // init_tools enumerates tools from extruders/toolchanger — semantically
    // incompatible with an AMS-backend topology override. Drop any prior
    // override so callers see a consistent extruder-derived list.
    ams_topology_active_ = false;
    ams_topology_tool_count_ = 0;
    ams_topology_tool_to_slot_.clear();
    ams_topology_tool_extruder_.clear();

    // Which offsets this printer keeps per toolhead, and the model that reads
    // them. Asked once, here, because this is the only place ToolState sees
    // the hardware; the status path has no PrinterDiscovery to hand.
    offset_reader_ = helix::tool_offsets::resolve_reader(hardware);
    std::string axes;
    for (Axis axis : kAllAxes) {
        const bool supported = helix::tool_offsets::supports_axis(hardware, axis);
        lv_subject_set_int(get_per_tool_axis_supported_subject(axis), supported ? 1 : 0);
        if (supported) {
            axes += axis_letter(axis);
        }
    }
    if (!axes.empty()) {
        spdlog::info("[ToolState] Per-tool offsets via {} (axes: {})",
                     helix::tool_offsets::provider_name(hardware), axes);
    }

    const std::vector<helix::tool_topology::FixedTool> fixed_tools =
        helix::tool_topology::fixed_toolheads(hardware);
    if (!fixed_tools.empty()) {
        // Firmware-fixed toolheads: no `tool T<n>` objects to enumerate.
        spdlog::info("[ToolState] {} fixed toolheads via {}", fixed_tools.size(),
                     helix::tool_topology::provider_name(hardware));
        for (size_t i = 0; i < fixed_tools.size(); ++i) {
            const int index = static_cast<int>(i);
            ToolInfo tool;
            tool.index = index;
            tool.name = helix::ui::tool_label(index);
            tool.display_label = noun_tool_label(index);
            tool.extruder_name = fixed_tools[i].extruder;
            tool.heater_name = fixed_tools[i].heater;
            if (!fixed_tools[i].fan.empty()) {
                tool.fan_name = fixed_tools[i].fan;
            }
            spdlog::debug("[ToolState] Fixed tool {}: extruder={}, fan={}", index,
                          tool.extruder_name.value_or("none"), tool.fan_name.value_or("none"));
            tools_.push_back(std::move(tool));
        }
    } else if (!hardware.tool_names().empty()) {
        // Tool changer: create N tools from discovered tool names.
        //
        // Not gated on has_tool_changer(). Tool names only ever come from
        // [tool N] objects or, on a changer whose extra does the swapping
        // itself, from the extruder enumeration discovery ran for it - both mean
        // a changer, and a changer without klipper-toolchanger has no
        // [toolchanger] object to gate on.
        const auto& tool_names = hardware.tool_names();

        const std::vector<std::string>& extruder_names = discovered_extruders_;

        for (int i = 0; i < static_cast<int>(tool_names.size()); ++i) {
            ToolInfo tool;
            tool.index = i;
            // The discovered name is the tool's real klipper identity (e.g. a
            // custom-named [tool Left]), so it is not always the generated
            // "T{i}" pattern and must not be overwritten.
            tool.name = tool_names[i];
            tool.display_label = noun_tool_label(i);

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
        std::vector<std::string> extruder_names = discovered_extruders_;
        if (extruder_names.empty())
            extruder_names.push_back("extruder");

        for (int i = 0; i < static_cast<int>(extruder_names.size()); ++i) {
            ToolInfo tool;
            tool.index = i;
            tool.name = helix::ui::tool_label(i);
            tool.display_label = noun_tool_label(i);
            tool.extruder_name = extruder_names[i];
            tool.heater_name = std::nullopt;

            spdlog::debug("[ToolState] Tool {}: name={}, extruder={}", i, tool.name,
                          tool.extruder_name.value_or("none"));
            tools_.push_back(std::move(tool));
        }
    }

    // T0 holds the head until a status frame says otherwise. active_tool_index_
    // and the per-tool flag are the same claim, so every branch above leaves
    // both to this one place. Set in only one of them they disagree, and
    // whichever a caller happens to read decides what it sees.
    active_tool_index_ = 0;
    if (!tools_.empty()) {
        tools_[0].active = true;
        // Klipper's [fan] is the first extruder's part cooling fan. A branch
        // that knows the machine's own per-tool names (Snapmaker) has already
        // filled this in; everything else gets the one name always true of T0.
        if (!tools_[0].fan_name) {
            tools_[0].fan_name = "fan";
        }
    }

    heads_cache_.reset();

    // Update subjects
    lv_subject_set_int(&tool_count_, static_cast<int>(tools_.size()));
    lv_subject_set_int(&active_tool_, active_tool_index_);
    // tools_ was just rebuilt, so every dirty flag it carried is gone. Without
    // this the subject keeps its old value forever on a printer that no longer
    // has per-tool offsets — update_from_status()'s recompute is gated on
    // the *_supported trio, which init_tools() may have just cleared.
    refresh_any_tool_dirty();
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
                         ams_topology_tool_extruder_ != topo.tool_extruder;
    if (heads_share_heater_ != topo.heads_share_heater) {
        heads_share_heater_ = topo.heads_share_heater;
        heads_cache_.reset();
    }

    ams_topology_active_ = true;
    ams_topology_tool_count_ = topo.tool_count;
    ams_topology_tool_to_slot_ = topo.tool_to_slot;
    ams_topology_tool_extruder_ = topo.tool_extruder;

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
            t.name = helix::ui::tool_label(i);
            t.display_label = noun_tool_label(i);
            t.backend_index = topo.backend_index;
            t.backend_slot =
                (i < static_cast<int>(topo.tool_to_slot.size())) ? topo.tool_to_slot[i] : -1;
            // Seated before the version bump below, so nothing that reads the
            // tools from that notification sees a rebuilt list with no seat.
            t.lane_seated = i < static_cast<int>(topo.tool_seated.size()) &&
                            topo.tool_seated[static_cast<size_t>(i)];
            // Carry over the hardware mappings init_tools set up, from the
            // tool that printed through the same extruder where the backend
            // names it. A hub feeds several lanes, and so several tools, into
            // one head, so tool i is not the i-th head and an index carry
            // would hand a lane another head's extruder, heater and fan.
            // A name discovery did not list is not a Klipper extruder (a
            // backend can report its own section name), so that tool keeps
            // the index carry and the extruder that came with it. Before
            // discovery has listed any, there is nothing to check a name
            // against and the backend's stands.
            const std::string extruder = i < static_cast<int>(topo.tool_extruder.size())
                                             ? topo.tool_extruder[static_cast<size_t>(i)]
                                             : std::string();
            const bool named =
                !extruder.empty() &&
                (discovered_extruders_.empty() ||
                 std::find(discovered_extruders_.begin(), discovered_extruders_.end(), extruder) !=
                     discovered_extruders_.end());
            const ToolInfo* hardware = nullptr;
            if (named) {
                for (const ToolInfo& p : previous) {
                    if (p.extruder_name && *p.extruder_name == extruder) {
                        hardware = &p;
                        break;
                    }
                }
            } else if (i < static_cast<int>(previous.size())) {
                hardware = &previous[static_cast<size_t>(i)];
            }
            if (hardware) {
                t.extruder_name = hardware->extruder_name;
                t.heater_name = hardware->heater_name;
                t.fan_name = hardware->fan_name;
                t.gcode_offsets = hardware->gcode_offsets;
            }
            if (named) {
                t.extruder_name = extruder;
            }
            if (i < static_cast<int>(previous.size())) {
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
        heads_cache_.reset();
        lv_subject_set_int(&tool_count_, static_cast<int>(tools_.size()));
        int version = lv_subject_get_int(&tools_version_) + 1;
        lv_subject_set_int(&tools_version_, version);
        spdlog::info("[ToolState] AMS topology applied: {} tools (version {})", tools_.size(),
                     version);
    }

    // Which lane a head holds moves with every load and unload, without the
    // shape changing, so it is applied on every push and bumps no version.
    bool seated_changed = false;
    for (size_t i = 0; i < tools_.size(); ++i) {
        const bool seated = i < topo.tool_seated.size() && topo.tool_seated[i];
        if (tools_[i].lane_seated != seated) {
            tools_[i].lane_seated = seated;
            seated_changed = true;
        }
    }
    if (seated_changed) {
        heads_cache_.reset();
    }

    int new_active = topo.active_tool;
    const bool empty_carriage = topo.allows_empty_carriage && new_active == -1;
    if (!empty_carriage && (new_active < 0 || new_active >= static_cast<int>(tools_.size()))) {
        new_active = 0; // Out-of-range falls back to T0 (matches init_tools convention)
    }
    if (new_active != active_tool_index_) {
        active_tool_index_ = new_active;
        lv_subject_set_int(&active_tool_, active_tool_index_);
        spdlog::debug("[ToolState] AMS topology active tool: T{}", active_tool_index_);
    }
}

void ToolState::clear_ams_topology() {
    // Nothing to clear is not an error: set_ams_topology() refuses before init,
    // so an inactive topology is the normal state on early-exit shutdown paths.
    if (!ams_topology_active_)
        return;
    if (!subjects_ready(subjects_initialized_, "clear_ams_topology()")) {
        return;
    }
    ams_topology_active_ = false;
    ams_topology_tool_count_ = 0;
    ams_topology_tool_to_slot_.clear();
    ams_topology_tool_extruder_.clear();
    heads_share_heater_ = false;
    tools_.clear();
    heads_cache_.reset();
    active_tool_index_ = 0;
    lv_subject_set_int(&tool_count_, 0);
    lv_subject_set_int(&active_tool_, 0);
    // Same reason as init_tools(): the flags died with tools_.
    refresh_any_tool_dirty();
    int version = lv_subject_get_int(&tools_version_) + 1;
    lv_subject_set_int(&tools_version_, version);
    spdlog::info("[ToolState] AMS topology cleared");
}

void ToolState::set_backend_active_tool(int index) {
    if (!subjects_ready(subjects_initialized_, "set_backend_active_tool()")) {
        return;
    }
    backend_owns_active_tool_ = true;
    if (index < 0 || index >= static_cast<int>(tools_.size())) {
        index = -1;
    }
    if (index != active_tool_index_) {
        active_tool_index_ = index;
        lv_subject_set_int(&active_tool_, active_tool_index_);
        spdlog::debug("[ToolState] Backend active tool: {}", active_tool_index_);
    }
}

void ToolState::clear_backend_active_tool() {
    if (!backend_owns_active_tool_) {
        return;
    }
    backend_owns_active_tool_ = false;
    // Without a backend an empty carriage is not a state this printer reports,
    // so T0 holds the head until a status frame says otherwise.
    if (active_tool_index_ < 0 && subjects_initialized_) {
        active_tool_index_ = 0;
        lv_subject_set_int(&active_tool_, active_tool_index_);
    }
    spdlog::debug("[ToolState] Backend released the active tool");
}

void ToolState::update_from_status(const nlohmann::json& status) {
    if (tools_.empty()) {
        return;
    }

    bool changed = false;

    // Parse active tool from toolchanger object.
    // When AMS owns the active tool (e.g., AFC), ignore Klipper's toolchanger
    // view — the logical T-number is driven by the backend, not the printer.
    const bool frame_owns_active_tool = !ams_topology_active_ && !backend_owns_active_tool_;
    if (frame_owns_active_tool) {
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
    if (frame_owns_active_tool) {
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
            // klipper-toolchanger spells DETECT_PRESENT "mounted"
            // (klipper/extras/toolchanger.py). "present" is what forks that
            // copied the older wording send; both mean the tool is on the
            // carriage. Anything else, DETECT_UNAVAILABLE included, stays
            // UNAVAILABLE.
            if (ds == "mounted" || ds == "present") {
                new_state = DetectState::PRESENT;
            } else if (ds == "absent") {
                new_state = DetectState::ABSENT;
            }
            if (new_state != tool.detect_state) {
                tool.detect_state = new_state;
                changed = true;
            }
        }

        // gcode_x/y/z_offset are NOT parsed here: which store is authoritative
        // is a per-firmware question helix::tool_offsets owns, and on a
        // MedusaHC the value on this object is not the one the machine prints
        // with. The per-tool offsets are read from the whole frame below.

        if (tool_status.contains("extruder") && tool_status["extruder"].is_string()) {
            std::string ext = tool_status["extruder"].get<std::string>();
            std::optional<std::string> new_val = ext.empty() ? std::nullopt : std::optional(ext);
            if (new_val != tool.extruder_name) {
                tool.extruder_name = new_val;
                heads_cache_.reset();
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

    // Per-tool offsets, in their own pass over tools_ rather than inside the
    // loop above. That loop skips a tool whose `tool T<n>` object is not in
    // this frame, and on a firmware that keeps every tool's offset on ONE
    // object (see helix::tool_offsets) that would skip the offsets entirely.
    if (per_tool_offsets_supported()) {
        for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
            for (Axis axis : kAllAxes) {
                if (lv_subject_get_int(get_per_tool_axis_supported_subject(axis)) != 1) {
                    continue;
                }
                auto microns = helix::tool_offsets::read_tool_offset_microns(
                    offset_reader_, status, axis, i, tools_[i].name);
                if (!microns) {
                    // No news. Moonraker republishes only what CHANGED, per
                    // field, so this is routine and must not be read as a
                    // reset to zero.
                    continue;
                }
                ToolAxisOffset& offset = tools_[i].gcode_offset(axis);
                const float mm = static_cast<float>(*microns) / 1000.0f;
                if (!offset.known) {
                    // First value seen for this axis is the persisted one: on
                    // a fresh connect the runtime offset IS what the config
                    // holds. Seeding the baseline here is what stops a
                    // freshly-connected printer from claiming unsaved work it
                    // does not have.
                    offset.saved_mm = mm;
                }
                if (offset.mm != mm || !offset.known) {
                    offset.mm = mm;
                    offset.known = true;
                    changed = true;
                }
            }
        }
        // After the reads, so the aggregate reflects every value this frame
        // carried.
        refresh_any_tool_dirty();
    }

    if (changed) {
        // Tool badge formatting handled by UI-layer observer on tools_version_
        int version = lv_subject_get_int(&tools_version_) + 1;
        lv_subject_set_int(&tools_version_, version);
        spdlog::trace("[ToolState] Status updated, version {}", version);
    }
}

bool ToolState::per_tool_offsets_supported() const {
    // lv_subject_get_int takes a non-const pointer but only reads.
    auto get = [](const lv_subject_t& subject) {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&subject));
    };
    return get(per_tool_x_supported_) == 1 || get(per_tool_y_supported_) == 1 ||
           get(per_tool_z_supported_) == 1;
}

void ToolState::query_tool_offsets(IMoonrakerClient* client,
                                   const helix::PrinterDiscovery& hardware) {
    if (!client || tools_.empty() || !per_tool_offsets_supported()) {
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
    auto cb = async_lifetime_.bg_cb(
        "ToolState::query_tool_offsets", [this](const nlohmann::json& response) {
            if (!response.contains("result") || !response["result"].contains("status")) {
                spdlog::debug("[ToolState] Tool offset query returned no status");
                return;
            }
            update_from_status(response["result"]["status"]);
            for (const auto& tool : tools_) {
                spdlog::debug(
                    "[ToolState] Seeded per-tool offsets from query: {} x={:.3f}{} "
                    "y={:.3f}{} z={:.3f}{}",
                    tool.name, tool.gcode_offset(Axis::X).mm,
                    tool.gcode_offset(Axis::X).known ? "" : "?", tool.gcode_offset(Axis::Y).mm,
                    tool.gcode_offset(Axis::Y).known ? "" : "?", tool.gcode_offset(Axis::Z).mm,
                    tool.gcode_offset(Axis::Z).known ? "" : "?");
            }
        });
    client->send_jsonrpc("printer.objects.query", nlohmann::json{{"objects", objects}},
                         std::move(cb));
}

std::vector<int> ToolState::dirty_tool_indices() const {
    std::vector<int> dirty;
    for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
        for (Axis axis : kAllAxes) {
            if (tools_[i].gcode_offset(axis).dirty()) {
                dirty.push_back(i);
                break;
            }
        }
    }
    return dirty;
}

bool ToolState::tool_offset_dirty(int tool_index, Axis axis) const {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        return false;
    }
    return tools_[tool_index].gcode_offset(axis).dirty();
}

float ToolState::tool_offset_mm(int tool_index, Axis axis) const {
    if (!tool_offset_known(tool_index, axis)) {
        return 0.0f;
    }
    return tools_[tool_index].gcode_offset(axis).mm;
}

bool ToolState::tool_offset_known(int tool_index, Axis axis) const {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        return false;
    }
    return tools_[tool_index].gcode_offset(axis).known;
}

void ToolState::set_tool_offset_local(int tool_index, Axis axis, int microns) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        return;
    }
    ToolAxisOffset& offset = tools_[tool_index].gcode_offset(axis);
    offset.mm = static_cast<float>(microns) / 1000.0f;
    offset.known = true;
    refresh_any_tool_dirty();
}

void ToolState::mark_tool_offsets_saved(int tool_index) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        return;
    }
    for (ToolAxisOffset& offset : tools_[tool_index].gcode_offsets) {
        offset.saved_mm = offset.mm;
    }
    refresh_any_tool_dirty();
}

void ToolState::refresh_any_tool_dirty() {
    const int any = dirty_tool_indices().empty() ? 0 : 1;
    lv_subject_set_int(&any_tool_offset_dirty_, any);
}

bool PhysicalHead::holds_tool(int tool_index) const {
    return std::find(tool_indices.begin(), tool_indices.end(), tool_index) != tool_indices.end();
}

int PhysicalHead::representative_tool(int active_tool) const {
    if (holds_tool(active_tool)) {
        return active_tool;
    }
    return seated_tool >= 0 ? seated_tool : tool_indices.front();
}

std::vector<PhysicalHead> physical_heads(const std::vector<ToolInfo>& tools,
                                         bool heads_share_heater) {
    std::vector<PhysicalHead> heads;
    for (size_t i = 0; i < tools.size(); ++i) {
        const auto& extruder = tools[i].extruder_name;
        const bool named = extruder && !extruder->empty();
        auto shared = heads.end();
        if (named && !heads_share_heater) {
            shared = std::find_if(heads.begin(), heads.end(), [&](const PhysicalHead& h) {
                return h.extruder_name == extruder;
            });
        }
        if (shared == heads.end()) {
            heads.push_back({named ? extruder : std::nullopt, {}});
            shared = heads.end() - 1;
        }
        shared->tool_indices.push_back(static_cast<int>(i));
    }
    for (PhysicalHead& head : heads) {
        int seated_count = 0;
        for (const int t : head.tool_indices) {
            if (tools[static_cast<size_t>(t)].lane_seated) {
                head.seated_tool = t;
                ++seated_count;
            }
        }
        if (seated_count > 1) {
            head.seated_tool = -1;
        }
    }
    return heads;
}

int head_index_for_tool(const std::vector<PhysicalHead>& heads, int tool_index) {
    for (size_t h = 0; h < heads.size(); ++h) {
        if (heads[h].holds_tool(tool_index)) {
            return static_cast<int>(h);
        }
    }
    return -1;
}

const std::vector<PhysicalHead>& ToolState::physical_heads() const {
    if (!heads_cache_) {
        heads_cache_ = helix::physical_heads(tools_, heads_share_heater());
    }
    return *heads_cache_;
}

bool ToolState::head_reads_heater(int head_index) const {
    const auto& heads = physical_heads();
    if (head_index < 0 || head_index >= static_cast<int>(heads.size()) ||
        !heads[static_cast<size_t>(head_index)].extruder_name) {
        return false;
    }
    return !heads_share_heater() ||
           heads[static_cast<size_t>(head_index)].holds_tool(active_tool_index_);
}

std::vector<PhysicalHead> ToolState::heated_heads() const {
    std::vector<PhysicalHead> heated;
    const auto& heads = physical_heads();
    for (size_t i = 0; i < heads.size(); ++i) {
        if (head_reads_heater(static_cast<int>(i))) {
            heated.push_back(heads[i]);
        }
    }
    // A shared heater with nothing mounted is still a heater: its lowest head
    // stands for it.
    if (heads_share_heater() && heated.empty()) {
        for (const PhysicalHead& head : heads) {
            if (head.extruder_name) {
                heated.push_back(head);
                break;
            }
        }
    }
    return heated;
}

int ToolState::extruder_count() const {
    return static_cast<int>(heated_heads().size());
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
    if (!names_mounted_nozzle()) {
        return lv_tr("Nozzle");
    }
    const auto* tool = active_tool();
    if (tool) {
        // The number, not the gcode identity: the three other producers of a
        // nozzle label (PrinterTemperatureState, the temp graph, the print
        // status widget) all spell this nozzle with lane_number_text(), and two
        // names for one physical nozzle is the confusion. A tool its owner
        // named keeps that name.
        const std::string suffix = helix::ui::is_generated_tool_name(tool->name)
                                       ? helix::ui::lane_number_text(tool->index)
                                       : tool->name;
        if (!suffix.empty()) {
            return std::string(lv_tr("Nozzle")) + " " + suffix;
        }
    }
    return lv_tr("Nozzle");
}

bool ToolState::names_mounted_nozzle() const {
    return has_multiple_extruders() || (heads_share_heater() && has_multiple_toolheads());
}

// Tool badge formatting moved to UI layer (ui_ams_tool_text.cpp)

std::string ToolState::tool_name_for_extruder(const std::string& extruder_name) const {
    if (heads_share_heater()) {
        const ToolInfo* tool = active_tool();
        return tool && tool->extruder_name == extruder_name ? tool->name : std::string();
    }
    for (const auto& tool : tools_) {
        if (tool.extruder_name && *tool.extruder_name == extruder_name) {
            return tool.name;
        }
    }
    return {};
}

std::string ToolState::extruder_name_for_tool(int tool_index) const {
    if (tool_index < 0 || tool_index >= static_cast<int>(tools_.size())) {
        return {};
    }
    return tools_[static_cast<size_t>(tool_index)].extruder_name.value_or("");
}

std::string ToolState::display_label_for_extruder(const std::string& extruder_name) const {
    if (heads_share_heater()) {
        const ToolInfo* tool = active_tool();
        return tool && tool->extruder_name == extruder_name ? tool->display_label : std::string();
    }
    for (const auto& tool : tools_) {
        if (tool.extruder_name && *tool.extruder_name == extruder_name) {
            return tool.display_label;
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
    std::string gcode = helix::ui::tool_label(tool_index);
    spdlog::info("[ToolState] Requesting tool change to T{} via gcode", tool_index);

    api->execute_gcode(
        gcode,
        [on_success]() {
            if (on_success)
                on_success();
        },
        [on_error](const MoonrakerError& error) {
            if (!on_error)
                return;
            // on_error is UI-facing; Klipper's reply arrives on the WebSocket thread.
            helix::ui::run_on_main("ToolState::tool_change_error",
                                   [on_error, error]() { on_error(error.localized_message()); });
        });
}

// ============================================================================
// Spool assignment persistence
// ============================================================================

static constexpr const char* kSpoolJsonFilename = "tool_spools.json";
static constexpr const char* kMoonrakerDbNamespace = "helix-screen";
static constexpr const char* kMoonrakerDbKey = "tool_spool_assignments";

namespace {

/// The spool file keeps one assignment set per printer under "printers"; a file without
/// it is the single-printer layout, a bare set keyed by tool index.
constexpr const char* kSpoolJsonPrinters = "printers";

/// The printer whose set was adopted from a single-printer file and has not yet loaded
/// from its own DB.
constexpr const char* kSpoolJsonAdoptedBy = "adopted_by";

/// The configured printer whose assignments are being read or written.
std::string spool_printer_key() {
    const Config* config = Config::get_instance();
    std::string id = config ? config->get_active_printer_id() : std::string();
    return id.empty() ? "default" : id;
}

bool is_per_printer_spool_file(const nlohmann::json& data) {
    const auto it = data.find(kSpoolJsonPrinters);
    return it != data.end() && it->is_object();
}

} // namespace

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
        tool.spoolman_id = json_util::safe_int(entry, "spoolman_id");
        tool.spool_name = json_util::safe_string(entry, "spool_name");
        tool.remaining_weight_g = json_util::safe_float(entry, "remaining_weight_g", -1.0f);
        tool.total_weight_g = json_util::safe_float(entry, "total_weight_g", -1.0f);

        if (tool.spoolman_id > 0) {
            spdlog::debug("[ToolState] Loaded spool {} ({}) for tool {}", tool.spoolman_id,
                          tool.spool_name, tool.index);
        }
    }
}

void ToolState::save_spool_json() const {
    std::string path = hfs::join_path(config_dir_, kSpoolJsonFilename);

    // Ensure directory exists
    hfs::create_directories(config_dir_);

    // The installer links this file out to printer_data; rename onto the target.
    path = helix::paths::write_target(path);

    // The other printers' sets are kept. A single-printer file is replaced: it belongs to
    // whichever printer loaded it first, and that printer's set is being written now.
    const auto existing = helix::text_io::read_file(path);
    nlohmann::json file =
        existing ? nlohmann::json::parse(*existing, nullptr, false) : nlohmann::json();
    if (hfs::exists(path) && (!existing || file.is_discarded())) {
        // Rewriting it would lose every other printer's set.
        spdlog::warn("[ToolState] {} cannot be read; not overwriting it", path);
        return;
    }
    if (!is_per_printer_spool_file(file)) {
        file = nlohmann::json{{kSpoolJsonPrinters, nlohmann::json::object()}};
    }
    file[kSpoolJsonPrinters][spool_printer_key()] = spool_assignments_to_json();
    if (spool_set_adopted_) {
        file[kSpoolJsonAdoptedBy] = spool_printer_key();
    } else if (json_util::safe_string(file, kSpoolJsonAdoptedBy) == spool_printer_key()) {
        file.erase(kSpoolJsonAdoptedBy);
    }

    // Every connect reloads the assignments from Moonraker and saves them back,
    // almost always unchanged. Skip the write then: on the ESP32 a flash write
    // stalls the display's scan-out, and elsewhere it is SD-card wear for nothing.
    const std::string text = helix::json_util::safe_dump(file, 2);
    if (existing && *existing == text) {
        spdlog::trace("[ToolState] Spool assignments unchanged, not rewriting {}", path);
        return;
    }

    // Atomic save: write to temp file, then rename to avoid partial writes on crash/power loss
    if (!helix::text_io::write_file_atomic(path, text)) {
        spdlog::error("[ToolState] Failed to write spool JSON to {}: {}", path, strerror(errno));
        return;
    }

    spdlog::debug("[ToolState] Saved spool assignments to {}", path);
}

bool ToolState::load_spool_json() {
    std::string path = hfs::join_path(config_dir_, kSpoolJsonFilename);

    if (!hfs::exists(path)) {
        spdlog::debug("[ToolState] No spool JSON file at {}", path);
        return false;
    }

    auto text = helix::text_io::read_file(path);
    if (!text) {
        spdlog::warn("[ToolState] Failed to open {}", path);
        return false;
    }

    auto data = nlohmann::json::parse(*text, nullptr, false);
    if (data.is_discarded()) {
        spdlog::warn("[ToolState] Spool JSON at {} is not valid JSON", path);
        return false;
    }

    if (!is_per_printer_spool_file(data)) {
        // A single-printer file goes to the first printer that loads it, and is rewritten
        // under that printer at once so no other printer can load it.
        apply_spool_assignments(data);
        spool_set_adopted_ = true;
        save_spool_json();
        spdlog::info("[ToolState] Loaded single-printer spool assignments from {} for printer "
                     "'{}'",
                     path, spool_printer_key());
        return true;
    }

    const auto& printers = data[kSpoolJsonPrinters];
    const auto mine = printers.find(spool_printer_key());
    if (mine == printers.end()) {
        spdlog::debug("[ToolState] No spool assignments for printer '{}' in {}",
                      spool_printer_key(), path);
        return false;
    }
    apply_spool_assignments(*mine);
    spool_set_adopted_ = json_util::safe_string(data, kSpoolJsonAdoptedBy) == spool_printer_key();
    spdlog::info("[ToolState] Loaded spool assignments for printer '{}' from {}",
                 spool_printer_key(), path);
    return true;
}

void ToolState::forget_printer_spools(const std::string& printer_id) const {
    const std::string path =
        helix::paths::write_target(hfs::join_path(config_dir_, kSpoolJsonFilename));
    const auto text = helix::text_io::read_file(path);
    if (!text) {
        return;
    }
    auto data = nlohmann::json::parse(*text, nullptr, false);
    if (!is_per_printer_spool_file(data) || data[kSpoolJsonPrinters].erase(printer_id) == 0) {
        return;
    }
    if (!helix::text_io::write_file_atomic(path, helix::json_util::safe_dump(data, 2))) {
        spdlog::error("[ToolState] Failed to drop printer '{}' from {}", printer_id, path);
        return;
    }
    spdlog::info("[ToolState] Dropped removed printer '{}' from {}", printer_id, path);
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
            kMoonrakerDbNamespace, kMoonrakerDbKey, json_data,
            []() { spdlog::debug("[ToolState] Spool assignments saved to Moonraker DB"); },
            [](const MoonrakerError& err) {
                spdlog::warn("[ToolState] Failed to save to Moonraker DB: {}", err.message);
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
    spool_load_printer_ = spool_printer_key();
    api->database_get_item(
        kMoonrakerDbNamespace, kMoonrakerDbKey,
        async_lifetime_.bg_cb("ToolState::load_spool_assignments",
                              [this](const nlohmann::json& data) {
                                  spool_load_printer_.clear();
                                  apply_spool_assignments(data);
                                  spool_set_adopted_ = false;
                                  save_spool_json();
                                  spool_assignments_loaded_ = true;
                                  // Re-sync AmsState so slot UI subjects reflect loaded assignments
                                  AmsState::instance().sync_from_backend();
                                  spdlog::info(
                                      "[ToolState] Loaded spool assignments from Moonraker DB");
                              }),
        async_lifetime_.bg_cb(
            "ToolState::load_spool_assignments_error", [this, api](const MoonrakerError& err) {
                spool_load_printer_.clear();
                spdlog::debug("[ToolState] Moonraker DB load failed ({}), trying local JSON",
                              err.message);
                const bool had_local = load_spool_json();
                spool_assignments_loaded_ = true;
                // Seed the DB only when it said the key is absent, or with a local set to
                // restore: a timeout or a dropped connection says nothing about what the
                // DB holds, and an empty seed would overwrite real assignments. An adopted
                // set may be another printer's, so it never seeds.
                if (!spool_set_adopted_ && (err.code == 404 || had_local)) {
                    save_spool_assignments(api);
                }
                // Re-sync AmsState so slot UI subjects reflect loaded assignments
                AmsState::instance().sync_from_backend();
            }));
}

} // namespace helix
