// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state.cpp
 * @brief Multi-filament system state singleton with async backend callbacks
 *
 * @pattern Singleton with static s_shutdown_flag atomic for callback safety
 * @threading Updated from WebSocket callbacks; shutdown flag prevents post-destruction access
 * @gotchas MoonrakerClient may be destroyed during static destruction
 *
 * @see ams_backend_afc.cpp, ams_backend_toolchanger.cpp
 */

#include "ams_state.h"

#include "ui_color_picker.h"
#include "ui_update_queue.h"

#include "ams_bypass_policy.h"
#include "app_globals.h"
#include "data_root_resolver.h"
#include "filament_database.h"
#include "filament_display_name.h"
#include "filament_sensor_manager.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

using namespace helix;

// Async callback data for thread-safe LVGL updates
namespace {

// Shutdown flag to prevent async callbacks from accessing destroyed singleton
static std::atomic<bool> s_shutdown_flag{false};

struct AsyncSyncData {
    int backend_index;
    bool full_sync;
    int slot_index; // Only used if full_sync == false
};

// Build a ToolTopology from a backend that multiplexes tools. Returns std::nullopt
// if the backend does not own the tool list (e.g., AD5X CFS, ACE — single tool,
// many slots). Falls back to a 1:1 mapping if the backend reports tool-mapping
// support but returns an empty mapping vector.
std::optional<helix::ToolTopology> build_ams_topology(AmsBackend* backend, int backend_index) {
    if (!backend)
        return std::nullopt;
    auto caps = backend->get_tool_mapping_capabilities();
    if (!caps.supported)
        return std::nullopt;

    std::vector<int> mapping = backend->get_tool_mapping();
    if (mapping.empty()) {
        // Fallback: default 1:1 from slot count
        int n = backend->get_system_info().total_slots;
        mapping.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            mapping[static_cast<size_t>(i)] = i;
    }

    helix::ToolTopology topo;
    topo.tool_count = static_cast<int>(mapping.size());
    topo.tool_to_slot = std::move(mapping);
    topo.active_tool = backend->get_current_tool();
    topo.tool_name_prefix = "T";
    topo.backend_index = backend_index;
    return topo;
}

} // namespace

AmsState& AmsState::instance() {
    // ~9.5KB singleton: relocate to PSRAM on ESP to reclaim internal DRAM (it's
    // app-state, first touched at runtime, never DMA/ISR). No-op elsewhere.
    static HELIX_PSRAM_BSS AmsState instance;
    return instance;
}

const char* AmsState::get_logo_path(const std::string& type_name) {
    // Normalize to lowercase for matching
    std::string lower_name = type_name;
    for (auto& c : lower_name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Strip common suffixes like " (mock)", " (test)", etc.
    size_t paren_pos = lower_name.find(" (");
    if (paren_pos != std::string::npos) {
        lower_name = lower_name.substr(0, paren_pos);
    }

    // Strip trailing unit numbers like "box turtle 1" → "box turtle"
    while (!lower_name.empty() && lower_name.back() == ' ') {
        lower_name.pop_back();
    }
    while (!lower_name.empty() && std::isdigit(static_cast<unsigned char>(lower_name.back()))) {
        lower_name.pop_back();
    }
    while (!lower_name.empty() && lower_name.back() == ' ') {
        lower_name.pop_back();
    }

    // Map system names to logo paths
    // Note: All logos are 64x64 white-on-transparent PNGs
    static const std::unordered_map<std::string, std::string> logo_map = {
        // AFC (Armored Turtle) - has its own logo
        {"afc", asset_component_uri("assets/images/ams/afc_64.png")},
        {"box turtle", asset_component_uri("assets/images/ams/box_turtle_64.png")},
        {"box_turtle", asset_component_uri("assets/images/ams/box_turtle_64.png")},
        {"boxturtle", asset_component_uri("assets/images/ams/box_turtle_64.png")},

        // Happy Hare - generic firmware, has its own logo
        {"happy hare", asset_component_uri("assets/images/ams/happy_hare_64.png")},
        {"happy_hare", asset_component_uri("assets/images/ams/happy_hare_64.png")},
        {"happyhare", asset_component_uri("assets/images/ams/happy_hare_64.png")},

        // Specific hardware types (when detected or configured)
        {"ercf", asset_component_uri("assets/images/ams/ercf_64.png")},
        {"3ms", asset_component_uri("assets/images/ams/3ms_64.png")},
        {"tradrack", asset_component_uri("assets/images/ams/tradrack_64.png")},
        {"mmx", asset_component_uri("assets/images/ams/mmx_64.png")},
        {"night owl", asset_component_uri("assets/images/ams/night_owl_64.png")},
        {"night_owl", asset_component_uri("assets/images/ams/night_owl_64.png")},
        {"nightowl", asset_component_uri("assets/images/ams/night_owl_64.png")},
        {"quattro box", asset_component_uri("assets/images/ams/quattro_box_64.png")},
        {"quattro_box", asset_component_uri("assets/images/ams/quattro_box_64.png")},
        {"quattrobox", asset_component_uri("assets/images/ams/quattro_box_64.png")},
        {"btt vivid", asset_component_uri("assets/images/ams/btt_vivid_64.png")},
        {"btt_vivid", asset_component_uri("assets/images/ams/btt_vivid_64.png")},
        {"bttvivid", asset_component_uri("assets/images/ams/btt_vivid_64.png")},
        {"vivid", asset_component_uri("assets/images/ams/btt_vivid_64.png")},
        {"kms", asset_component_uri("assets/images/ams/kms_64.png")},

        // AFC unit types with no artwork of their own (Claymore is new in AFC
        // v1.2.0; the rest predate it). They fall back to the AFC mark:
        // wrong-but-related beats a blank slot, and the alternative is
        // silently rendering nothing.
        {"htlf", asset_component_uri("assets/images/ams/afc_64.png")},
        {"open ams", asset_component_uri("assets/images/ams/afc_64.png")},
        {"open_ams", asset_component_uri("assets/images/ams/afc_64.png")},
        {"openams", asset_component_uri("assets/images/ams/afc_64.png")},
        {"claymore", asset_component_uri("assets/images/ams/afc_64.png")},
        {"emu", asset_component_uri("assets/images/ams/afc_64.png")},
    };

    auto it = logo_map.find(lower_name);
    if (it != logo_map.end()) {
        return it->second.c_str();
    }

    // AFC names a unit by type AND instance — "Box_Turtle Turtle_1" — so the
    // whole string never matches a type key and every AFC unit fell through to
    // the generic AFC mark, Box Turtles included. Retry on the leading token,
    // which is the type. Only reached once the exact lookup has failed, so
    // multi-word system names ("happy hare") keep their own entry.
    const size_t space_pos = lower_name.find(' ');
    if (space_pos != std::string::npos && space_pos > 0) {
        it = logo_map.find(lower_name.substr(0, space_pos));
        if (it != logo_map.end()) {
            return it->second.c_str();
        }
    }
    return nullptr;
}

AmsState::AmsState() {
    std::memset(action_detail_buf_, 0, sizeof(action_detail_buf_));
    std::memset(system_name_buf_, 0, sizeof(system_name_buf_));
    std::memset(system_logo_buf_, 0, sizeof(system_logo_buf_));
    std::memset(current_material_text_buf_, 0, sizeof(current_material_text_buf_));
    std::memset(current_slot_text_buf_, 0, sizeof(current_slot_text_buf_));
    std::memset(current_weight_text_buf_, 0, sizeof(current_weight_text_buf_));
    std::memset(clog_meter_value_text_buf_, 0, sizeof(clog_meter_value_text_buf_));
    std::memset(clog_meter_mode_text_buf_, 0, sizeof(clog_meter_mode_text_buf_));
    std::memset(clog_meter_center_text_buf_, 0, sizeof(clog_meter_center_text_buf_));
    std::memset(clog_meter_label_left_buf_, 0, sizeof(clog_meter_label_left_buf_));
    std::memset(clog_meter_label_right_buf_, 0, sizeof(clog_meter_label_right_buf_));
}

AmsState::~AmsState() {
    // Signal shutdown to prevent async callbacks from accessing this instance
    s_shutdown_flag.store(true, std::memory_order_release);

    // During static destruction, the MoonrakerClient may already be destroyed.
    // Release subscriptions without unsubscribing to avoid calling into dead objects.
    // SubscriptionGuard::release() abandons the subscription — no mutex access needed.
    for (auto& b : backends_) {
        if (b) {
            b->release_subscriptions();
        }
    }
    backends_.clear();
}

void AmsState::init_subjects(bool register_xml) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (initialized_) {
        // Re-entry on the shared singleton: always rebind the print-state
        // observer to PrinterState's *current* print_state_enum subject.
        //
        // A `if (!print_state_observer_)` guard is NOT sufficient: PrinterState
        // can deinit+reinit its subjects between cases (LVGLUITestFixture does
        // this; production soft-restart can too). lv_subject_deinit() frees our
        // observer node, but ObserverGuard::operator bool() only checks for a
        // non-null observer pointer — it stays truthy with a dangling pointer to
        // the freed/recreated subject, so the guard would skip re-install and
        // the label would never recompute on print-state changes.
        //
        // install_print_state_observer() is idempotent (reset()s the prior
        // guard, then rebinds to the current subject with the current lifetime
        // token), so calling it unconditionally is safe and self-healing.
        install_print_state_observer();
        return;
    }

    spdlog::trace("[AMS State] Initializing subjects");

    // Backend selector subjects
    INIT_SUBJECT_INT(backend_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_data_revision, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(active_backend, 0, subjects_, register_xml);

    // System-level subjects
    INIT_SUBJECT_INT(ams_type, static_cast<int>(AmsType::NONE), subjects_, register_xml);
    INIT_SUBJECT_INT(ams_action, static_cast<int>(AmsAction::IDLE), subjects_, register_xml);
    // Granular load/unload sub-phase (Snapmaker U1). -1 = no active step.
    INIT_SUBJECT_INT(ams_operation_phase, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_operation_indeterminate, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(toolchange_step, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(current_slot, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(pending_target_slot, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_current_tool, -1, subjects_, register_xml);
    // These subjects need ams_ prefix for XML but member vars don't have it
    lv_subject_init_int(&filament_loaded_, 0);
    subjects_.register_subject(&filament_loaded_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_filament_loaded", &filament_loaded_);

    lv_subject_init_int(&filament_runout_, 0);
    subjects_.register_subject(&filament_runout_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_filament_runout", &filament_runout_);

    lv_subject_init_int(&bypass_active_, 0);
    subjects_.register_subject(&bypass_active_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_bypass_active", &bypass_active_);

    // External spool color subject (loaded from persistent settings)
    {
        auto ext_spool = helix::SettingsManager::instance().get_external_spool_info();
        int initial_color = ext_spool.has_value() ? static_cast<int>(ext_spool->color_rgb) : 0;
        lv_subject_init_int(&external_spool_color_, initial_color);
        subjects_.register_subject(&external_spool_color_);
        if (register_xml)
            lv_xml_register_subject(nullptr, "ams_external_spool_color", &external_spool_color_);

        // Material string flavor — same source, string subject idiom as
        // ams_system_name_ (own buffer, nullptr prev_buf).
        lv_subject_init_string(&external_spool_material_, external_spool_material_buf_, nullptr,
                               sizeof(external_spool_material_buf_),
                               ext_spool.has_value() ? ext_spool->material.c_str() : "");
        subjects_.register_subject(&external_spool_material_);
        if (register_xml)
            lv_xml_register_subject(nullptr, "ams_external_spool_material",
                                    &external_spool_material_);
    }

    lv_subject_init_int(&supports_bypass_, 0);
    subjects_.register_subject(&supports_bypass_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_supports_bypass", &supports_bypass_);
    INIT_SUBJECT_INT(ams_slot_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_cards_compact, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(slots_version, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(tool_map_version, 0, subjects_, register_xml);
    // Default 1 (present) so non-auto-feed / unknown backends never gate Resume (#991).
    INIT_SUBJECT_INT(active_tool_port_present, 1, subjects_, register_xml);

    // String subjects (buffer names don't match macro convention)
    lv_subject_init_string(&ams_action_detail_, action_detail_buf_, nullptr,
                           sizeof(action_detail_buf_), "");
    subjects_.register_subject(&ams_action_detail_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_action_detail", &ams_action_detail_);

    lv_subject_init_string(&ams_system_name_, system_name_buf_, nullptr, sizeof(system_name_buf_),
                           "");
    subjects_.register_subject(&ams_system_name_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_system_name", &ams_system_name_);

    // Logo uses pointer subject — bind_src expects a pointer to the path string buffer.
    // Init to nullptr so XML bind_src doesn't fire lv_image_set_src("") warnings before
    // sync_from_backend populates the real logo path.
    lv_subject_init_pointer(&ams_system_logo_, nullptr);
    subjects_.register_subject(&ams_system_logo_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_system_logo", &ams_system_logo_);

    INIT_SUBJECT_STRING(ams_current_tool_text, "---", subjects_, register_xml);

    // Endless-spool status. Starts Hidden with no text so a printer whose
    // backend never reports the feature renders nothing rather than flashing a
    // default sentence before the first sync.
    INIT_SUBJECT_INT(ams_endless_state,
                     static_cast<int>(helix::printer::EndlessSpoolStatusKind::Hidden), subjects_,
                     register_xml);
    lv_subject_init_string(&ams_endless_text_, ams_endless_text_buf_, nullptr,
                           sizeof(ams_endless_text_buf_), "");
    subjects_.register_subject(&ams_endless_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_endless_text", &ams_endless_text_);

    // Tool change progress subjects
    INIT_SUBJECT_INT(toolchange_visible, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_current_toolchange, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_number_of_toolchanges, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(toolchange_text, "", subjects_, register_xml);

    // Filament path visualization subjects
    INIT_SUBJECT_INT(path_topology, static_cast<int>(PathTopology::HUB), subjects_, register_xml);
    INIT_SUBJECT_INT(path_active_slot, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(path_filament_segment, static_cast<int>(PathSegment::NONE), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(path_error_segment, static_cast<int>(PathSegment::NONE), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(path_anim_progress, 0, subjects_, register_xml);

    // Dryer subjects (for AMS systems with integrated drying)
    INIT_SUBJECT_INT(dryer_supported, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_active, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_current_temp, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_target_temp, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_remaining_min, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_progress_pct, -1, subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_current_temp_text, "---", subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_target_temp_text, "---", subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_time_text, "", subjects_, register_xml);

    // Dryer modal editing subjects (raw int + formatted text)
    INIT_SUBJECT_INT(modal_target_temp, DEFAULT_DRYER_TEMP_C, subjects_, register_xml);
    INIT_SUBJECT_INT(modal_duration_min, DEFAULT_DRYER_DURATION_MIN, subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_modal_temp_text, "55°C", subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_modal_duration_text, "4h", subjects_, register_xml);

    // Dryer humidity and info bar visibility subjects
    INIT_SUBJECT_STRING(dryer_humidity_text, "---", subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_info_visible, 0, subjects_, register_xml);

    // Currently Loaded display subjects (for reactive UI binding)
    // These subjects need ams_ prefix for XML but member vars don't have it
    lv_subject_init_string(&current_material_text_, current_material_text_buf_, nullptr,
                           sizeof(current_material_text_buf_), "---");
    subjects_.register_subject(&current_material_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_current_material_text", &current_material_text_);

    lv_subject_init_string(&current_slot_text_, current_slot_text_buf_, nullptr,
                           sizeof(current_slot_text_buf_), "None");
    subjects_.register_subject(&current_slot_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_current_slot_text", &current_slot_text_);

    lv_subject_init_string(&current_weight_text_, current_weight_text_buf_, nullptr,
                           sizeof(current_weight_text_buf_), "");
    subjects_.register_subject(&current_weight_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_current_weight_text", &current_weight_text_);

    lv_subject_init_int(&current_has_weight_, 0);
    subjects_.register_subject(&current_has_weight_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_current_has_weight", &current_has_weight_);

    INIT_SUBJECT_INT(current_color, 0x505050, subjects_, register_xml);

    // Clog detection meter subjects
    INIT_SUBJECT_INT(clog_meter_mode, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(clog_meter_value, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(clog_meter_warning, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_value_text, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_mode_text, "", subjects_, register_xml);
    INIT_SUBJECT_INT(clog_meter_danger_pct, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(clog_meter_peak_pct, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_center_text, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_label_left, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_label_right, "", subjects_, register_xml);

    // Per-slot subjects (dynamic names require manual init)
    char name_buf[32];
    for (int i = 0; i < MAX_SLOTS; ++i) {
        lv_subject_init_int(&slot_colors_[i], static_cast<int>(AMS_DEFAULT_SLOT_COLOR));
        subjects_.register_subject(&slot_colors_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_color", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_colors_[i]);
        }

        lv_subject_init_int(&slot_statuses_[i], static_cast<int>(SlotStatus::UNKNOWN));
        subjects_.register_subject(&slot_statuses_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_status", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_statuses_[i]);
        }

        lv_subject_init_string(&slot_remaining_[i], slot_remaining_buf_[i], nullptr,
                               sizeof(slot_remaining_buf_[i]), "");
        subjects_.register_subject(&slot_remaining_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_remaining", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_remaining_[i]);
        }

        lv_subject_init_string(&slot_materials_[i], slot_materials_buf_[i], nullptr,
                               sizeof(slot_materials_buf_[i]), "");
        subjects_.register_subject(&slot_materials_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_material", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_materials_[i]);
        }

        // Per-slot fill percent (SlotInfo::display_fill_pct encoding: 0-100, -1
        // = unknown). Observed by the ams_slot widget so spool fill renders from
        // state on every panel. -1 initial → "no data yet, leave render as-is".
        lv_subject_init_int(&slot_fills_[i], -1);
        subjects_.register_subject(&slot_fills_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_fill", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_fills_[i]);
        }

        // Per-slot LIVE state subjects (path segment, toolhead-present, active-loaded)
        lv_subject_init_int(&slot_segments_[i], static_cast<int>(PathSegment::NONE));
        subjects_.register_subject(&slot_segments_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_segment", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_segments_[i]);
        }

        lv_subject_init_int(&slot_toolhead_present_[i], 0);
        subjects_.register_subject(&slot_toolhead_present_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_toolhead_present", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_toolhead_present_[i]);
        }

        lv_subject_init_int(&slot_active_loaded_[i], 0);
        subjects_.register_subject(&slot_active_loaded_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_active_loaded", i);
            lv_xml_register_subject(nullptr, name_buf, &slot_active_loaded_[i]);
        }
    }

    // Per-unit environment subjects (CFS temperature/humidity)
    for (int i = 0; i < MAX_UNITS; ++i) {
        lv_subject_init_int(&unit_temp_[i], 0);
        subjects_.register_subject(&unit_temp_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_temp", i);
            lv_xml_register_subject(nullptr, name_buf, &unit_temp_[i]);
        }

        lv_subject_init_int(&unit_humidity_[i], 0);
        subjects_.register_subject(&unit_humidity_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_humidity", i);
            lv_xml_register_subject(nullptr, name_buf, &unit_humidity_[i]);
        }
    }

    // Per-unit environment indicator display subjects (formatted text for XML binding)
    for (int i = 0; i < MAX_UNITS; ++i) {
        char name_buf[48];

        lv_subject_init_string(&env_ind_temp_text_[i], env_ind_temp_text_buf_[i], nullptr,
                               ENV_IND_TEXT_BUF_SIZE, "---");
        subjects_.register_subject(&env_ind_temp_text_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_temp_text", i);
            lv_xml_register_subject(nullptr, name_buf, &env_ind_temp_text_[i]);
        }

        lv_subject_init_string(&env_ind_humidity_text_[i], env_ind_humidity_text_buf_[i], nullptr,
                               ENV_IND_TEXT_BUF_SIZE, "---");
        subjects_.register_subject(&env_ind_humidity_text_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_text", i);
            lv_xml_register_subject(nullptr, name_buf, &env_ind_humidity_text_[i]);
        }

        lv_subject_init_int(&env_ind_humidity_status_[i], 0);
        subjects_.register_subject(&env_ind_humidity_status_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_status", i);
            lv_xml_register_subject(nullptr, name_buf, &env_ind_humidity_status_[i]);
        }

        lv_subject_init_int(&env_ind_humidity_visible_[i], 0);
        subjects_.register_subject(&env_ind_humidity_visible_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_visible", i);
            lv_xml_register_subject(nullptr, name_buf, &env_ind_humidity_visible_[i]);
        }

        lv_subject_init_int(&env_ind_visible_[i], 0);
        subjects_.register_subject(&env_ind_visible_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_visible", i);
            lv_xml_register_subject(nullptr, name_buf, &env_ind_visible_[i]);
        }

        lv_subject_init_int(&env_ind_drying_active_[i], 0);
        subjects_.register_subject(&env_ind_drying_active_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_drying_active", i);
            lv_xml_register_subject(nullptr, name_buf, &env_ind_drying_active_[i]);
        }

        lv_subject_init_string(&env_ind_drying_text_[i], env_ind_drying_text_buf_[i], nullptr,
                               ENV_IND_DRYING_BUF_SIZE, "");
        subjects_.register_subject(&env_ind_drying_text_[i]);
        if (register_xml) {
            snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_drying_text", i);
            lv_xml_register_subject(nullptr, name_buf, &env_ind_drying_text_[i]);
        }
    }

    // Always-off placeholders for units past MAX_UNITS. A rig with more units
    // than we allocate subjects for still gets a card per unit; its environment
    // indicator binds these, so the badge stays hidden instead of the parser
    // warning once per binding about names nothing registered.
    lv_subject_init_int(&env_ind_off_flag_, 0);
    subjects_.register_subject(&env_ind_off_flag_);
    if (register_xml)
        lv_xml_register_subject(nullptr, ENV_IND_OFF_FLAG_SUBJECT, &env_ind_off_flag_);

    lv_subject_init_string(&env_ind_off_text_, env_ind_off_text_buf_, nullptr,
                           ENV_IND_TEXT_BUF_SIZE, "");
    subjects_.register_subject(&env_ind_off_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, ENV_IND_OFF_TEXT_SUBJECT, &env_ind_off_text_);

    // Detail-view env indicator mirror subjects.
    lv_subject_init_string(&env_ind_detail_temp_text_, env_ind_detail_temp_text_buf_, nullptr,
                           ENV_IND_TEXT_BUF_SIZE, "---");
    subjects_.register_subject(&env_ind_detail_temp_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_env_ind_detail_temp_text",
                                &env_ind_detail_temp_text_);

    lv_subject_init_string(&env_ind_detail_humidity_text_, env_ind_detail_humidity_text_buf_,
                           nullptr, ENV_IND_TEXT_BUF_SIZE, "---");
    subjects_.register_subject(&env_ind_detail_humidity_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_env_ind_detail_humidity_text",
                                &env_ind_detail_humidity_text_);

    lv_subject_init_int(&env_ind_detail_humidity_status_, 0);
    subjects_.register_subject(&env_ind_detail_humidity_status_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_env_ind_detail_humidity_status",
                                &env_ind_detail_humidity_status_);

    lv_subject_init_int(&env_ind_detail_humidity_visible_, 0);
    subjects_.register_subject(&env_ind_detail_humidity_visible_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_env_ind_detail_humidity_visible",
                                &env_ind_detail_humidity_visible_);

    lv_subject_init_int(&env_ind_detail_visible_, 0);
    subjects_.register_subject(&env_ind_detail_visible_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_env_ind_detail_visible", &env_ind_detail_visible_);

    lv_subject_init_int(&env_ind_detail_drying_active_, 0);
    subjects_.register_subject(&env_ind_detail_drying_active_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_env_ind_detail_drying_active",
                                &env_ind_detail_drying_active_);

    lv_subject_init_string(&env_ind_detail_drying_text_, env_ind_detail_drying_text_buf_, nullptr,
                           ENV_IND_DRYING_BUF_SIZE, "");
    subjects_.register_subject(&env_ind_detail_drying_text_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_env_ind_detail_drying_text",
                                &env_ind_detail_drying_text_);

    // Ask the factory for a backend. In mock mode, it returns a mock backend.
    // In real mode with no printer connected, it returns nullptr.
    // This keeps mock/real decision entirely in the factory.
    if (backends_.empty()) {
        auto backend = AmsBackend::create(AmsType::NONE, nullptr, nullptr);
        if (backend) {
            backend->start();
            set_backend(std::move(backend));
            sync_from_backend();
            spdlog::debug("[AMS State] Backend initialized via factory ({} slots)",
                          lv_subject_get_int(&ams_slot_count_));
        }
    }

    initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "AmsState", []() { AmsState::instance().deinit_subjects(); });

    // Observe PrinterState's print state so the ams_action_detail label can
    // flip between "Idle" / "Printing" / "Paused" when the AMS itself is IDLE
    // but a print is in progress. print_state_enum is a *static* PrinterState
    // subject — no SubjectLifetime token required.
    //
    // Wire the observer here rather than inside the `if (initialized_)` guard
    // so tests that re-enter init_subjects() on the shared singleton (after
    // a prior test left it initialized) still get the observer attached.
    // PrinterState::init_subjects() must run before this point; tests/main
    // do so during fixture setup / app boot.
    install_print_state_observer();
}

void AmsState::install_print_state_observer() {
    // Idempotent: reset any prior guard before installing a fresh one. The
    // reset path uses the alive token from the *previous* install so it can
    // safely skip lv_observer_remove() if PrinterState already deinit'd its
    // subjects (e.g. between tests).
    print_state_observer_.reset();
    auto lifetime = get_printer_state().get_static_print_subjects_lifetime();
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - recompute_action_detail()
    // labels what the printer reports, and reads the same subject.
    print_state_observer_ = helix::ui::observe_int_sync<AmsState>(
        get_printer_state().get_print_state_enum_subject(), this,
        [](AmsState* self, int /*print_state*/) { self->recompute_action_detail(); }, lifetime);
}

void AmsState::deinit_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!initialized_) {
        return;
    }

    spdlog::trace("[AMS State] Deinitializing subjects");

    // Death signal BEFORE the subjects go away: deinit frees every observer
    // node on them, so outside ObserverGuards must learn they are gone or their
    // next reset() calls lv_observer_remove() on freed memory. Replaced, not
    // cleared — an empty token reads as "dead" and would suppress removal for
    // observers registered after this teardown.
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }
    subjects_lifetime_ = std::make_shared<bool>(true);

    // Expire the deferred setters still queued on the UpdateQueue. They capture
    // `this` and write the subjects torn down below, so without this the next
    // drain notifies a freed observer list (#1165, #1146).
    async_lifetime_.invalidate();

    // Clear dangling API pointer — the IMoonrakerAPI is destroyed during teardown
    // before AmsState re-initializes. Without this, sync_from_backend() would
    // dereference a freed pointer on the next init_subjects() cycle.
    api_ = nullptr;

    // Tear down the print-state observer BEFORE deiniting subjects so the
    // LVGL observer is removed cleanly (reset(), not release() — see project
    // CLAUDE.md § "ObserverGuard::reset() is the default").
    print_state_observer_.reset();

    // IMPORTANT: clear_backends() MUST precede subjects_.deinit_all() because
    // BackendSlotSubjects are managed outside SubjectManager for lifetime reasons
    clear_backends();

    // Use SubjectManager for automatic cleanup of all registered subjects
    subjects_.deinit_all();

    initialized_ = false;
    spdlog::trace("[AMS State] Subjects deinitialized");
}

void AmsState::init_backend_from_hardware(const helix::PrinterDiscovery& hardware,
                                          IMoonrakerAPI* api, IMoonrakerClient* client) {
    init_backends_from_hardware(hardware, api, client);
}

void AmsState::init_backends_from_hardware(const helix::PrinterDiscovery& hardware,
                                           IMoonrakerAPI* api, IMoonrakerClient* client) {
    const auto& systems = hardware.detected_ams_systems();
    if (systems.empty()) {
        spdlog::debug("[AMS State] No AMS systems detected, skipping");
        return;
    }

    if (get_runtime_config()->should_mock_ams()) {
        spdlog::debug("[AMS State] Mock mode active, skipping real backend initialization");
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!backends_.empty()) {
            spdlog::debug("[AMS State] Backends already initialized, skipping");
            return;
        }
    }

    for (const auto& system : systems) {
        spdlog::info("[AMS State] Creating backend for: {} ({})", system.name,
                     ams_type_to_string(system.type));

        auto backend = AmsBackend::create(system.type, api, client);
        if (!backend) {
            spdlog::warn("[AMS State] Failed to create {} backend", system.name);
            continue;
        }

        backend->set_discovered_lanes(hardware.afc_lane_names(), hardware.afc_hub_names());
        backend->set_discovered_tools(hardware.tool_names());
        backend->set_discovered_sensors(hardware.filament_sensor_names());

        int index = add_backend(std::move(backend));

        auto* b = get_backend(index);
        if (b) {
            auto result = b->start();
            spdlog::debug("[AMS State] Backend {} started, result={}", index,
                          static_cast<bool>(result));
        }
    }

    spdlog::info("[AMS State] Initialized {} backends", backend_count());

    // Sync immediately to propagate static system_info (total_slots, type, etc.)
    // from the newly created backends to UI subjects. Without this, the
    // ams_slot_count gate stays at 0 until the first async event arrives — which
    // may never happen if the backend's initial query returns no matching objects
    // (e.g. native ZMOD IFS without lessWaste per-port sensors).
    sync_from_backend();
}

void AmsState::set_backend(std::unique_ptr<AmsBackend> backend) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    clear_backends();

    if (backend) {
        auto type = backend->get_type();
        add_backend(std::move(backend));
        spdlog::debug("[AMS State] Backend set (type={})", ams_type_to_string(type));
    }
}

int AmsState::add_backend(std::unique_ptr<AmsBackend> backend) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    int index = static_cast<int>(backends_.size());
    backends_.push_back(std::move(backend));

    if (backends_[index]) {
        // Register event callback with captured index
        backends_[index]->set_event_callback(
            [this, index](const std::string& event, const std::string& data) {
                on_backend_event(index, event, data);
            });

        // Apply stored gcode response callback (no-op for real backends)
        if (gcode_response_callback_) {
            backends_[index]->set_gcode_response_callback(gcode_response_callback_);
        }

        // Allocate per-backend slot subjects for secondary backends
        if (index > 0) {
            auto info = backends_[index]->get_system_info();
            BackendSlotSubjects subs;
            subs.init(info.total_slots);
            secondary_slot_subjects_.push_back(std::move(subs));
        }

        // Register one FilamentConsumptionTracker sink per slot. The tracker's
        // gating (unknown weight / Spoolman-linked / native-tracking backend)
        // decides per-tick whether each sink actually consumes deltas.
        const int slot_count = backends_[index]->get_system_info().total_slots;
        auto& handles = consumption_sinks_[index];
        handles.reserve(slot_count);
        auto& tracker = helix::FilamentConsumptionTracker::instance();
        for (int slot = 0; slot < slot_count; ++slot) {
            auto sink = std::make_unique<helix::AmsSlotSink>(index, slot);
            handles.push_back(tracker.register_sink(std::move(sink)));
        }
        spdlog::debug("[AMS State] Registered {} consumption sinks for backend {}", slot_count,
                      index);
    }

    // Update backend count subject for UI binding
    int new_count = static_cast<int>(backends_.size());
    if (lv_subject_get_int(&backend_count_) != new_count) {
        lv_subject_set_int(&backend_count_, new_count);
    }

    return index;
}

AmsBackend* AmsState::get_backend(int index) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int>(backends_.size())) {
        return nullptr;
    }
    return backends_[index].get();
}

int AmsState::backend_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<int>(backends_.size());
}

void AmsState::clear_backends() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Unregister all FilamentConsumptionTracker sinks tied to these backends
    // BEFORE tearing down the backends themselves — the sinks will read each
    // backend one last time on flush().
    auto& tracker = helix::FilamentConsumptionTracker::instance();
    for (auto& [idx, handles] : consumption_sinks_) {
        for (auto* h : handles) {
            tracker.unregister_sink(h);
        }
    }
    consumption_sinks_.clear();

    // Stop all backends
    for (auto& b : backends_) {
        if (b) {
            b->stop();
        }
    }
    backends_.clear();

    // The runout edge state describes a specific backend's flag history. A new
    // backend's first sample must re-seed rather than read as a transition.
    prev_backend_runout_ = false;
    runout_edge_armed_ = false;
    runout_prev_paused_ = false;
    runout_level_seeded_ = false;
    // Same reasoning for the post-unload grace: it was armed for a removal on
    // the backend going away, and nothing the next one reports can be that.
    post_unload_runout_grace_ = false;
    post_unload_runout_grace_at_ = {};
    saw_unload_in_op_ = false;

    // Drop AMS-derived tool topology so the UI doesn't show stale tool pills
    // between backend disappearance and the next reconnect's init_tools().
    helix::ToolState::instance().clear_ams_topology();

    // Clean up secondary slot subjects
    for (auto& subs : secondary_slot_subjects_) {
        subs.deinit();
    }
    secondary_slot_subjects_.clear();

    // Reset backend selector subjects
    if (lv_subject_get_int(&backend_count_) != 0) {
        lv_subject_set_int(&backend_count_, 0);
    }
    if (lv_subject_get_int(&active_backend_) != 0) {
        lv_subject_set_int(&active_backend_, 0);
    }
}

std::vector<helix::AvailableSlot> AmsState::collect_available_slots() const {
    std::vector<helix::AvailableSlot> slots;
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    for (size_t bi = 0; bi < backends_.size(); ++bi) {
        const auto& backend = backends_[bi];
        if (!backend) {
            continue;
        }

        auto info = backend->get_system_info();
        bool multi_unit = info.units.size() > 1;

        for (const auto& unit : info.units) {
            for (const auto& slot_info : unit.slots) {
                helix::AvailableSlot as;
                as.slot_index = slot_info.global_index;
                as.local_slot_index = slot_info.slot_index;
                as.backend_index = static_cast<int>(bi);
                as.color_rgb = slot_info.color_rgb;
                as.multi_color_hexes = slot_info.multi_color_hexes;
                as.material = slot_info.material;
                as.is_empty = (slot_info.status == SlotStatus::EMPTY ||
                               slot_info.status == SlotStatus::UNKNOWN);
                as.current_tool_mapping = slot_info.mapped_tool;
                as.unit_index = unit.unit_index;
                if (multi_unit) {
                    as.unit_display_name =
                        unit.display_name.empty() ? unit.name : unit.display_name;
                }
                slots.push_back(std::move(as));
            }
        }
    }

    spdlog::debug("[AmsState] Collected {} available slots from {} backends", slots.size(),
                  backends_.size());
    return slots;
}

bool AmsState::any_bypass_active() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& backend : backends_) {
        if (backend && backend->is_bypass_active()) {
            return true;
        }
    }
    return false;
}

AmsBackend* AmsState::get_backend() const {
    return get_backend(0);
}

int AmsState::active_backend_index() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&active_backend_));
}

void AmsState::set_active_backend(int index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (index >= 0 && index < static_cast<int>(backends_.size())) {
        if (lv_subject_get_int(&active_backend_) != index) {
            lv_subject_set_int(&active_backend_, index);
        }
    }
}

bool AmsState::is_available() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto* primary = get_backend(0);
    return primary && primary->get_type() != AmsType::NONE;
}

void AmsState::set_moonraker_api(IMoonrakerAPI* api) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    api_ = api;
    last_synced_spoolman_id_ = 0; // Reset tracking on API change
    spdlog::debug("[AMS State] Moonraker API {} for Spoolman integration", api ? "set" : "cleared");
}

void AmsState::set_gcode_response_callback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    gcode_response_callback_ = std::move(callback);

    // Apply to any existing backends (no-op for real backends)
    for (auto& backend : backends_) {
        backend->set_gcode_response_callback(gcode_response_callback_);
    }

    spdlog::debug("[AMS State] Gcode response callback {}",
                  gcode_response_callback_ ? "set" : "cleared");
}

lv_subject_t* AmsState::get_slot_color_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_colors_[slot_index];
}

lv_subject_t* AmsState::get_slot_status_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_statuses_[slot_index];
}

lv_subject_t* AmsState::get_slot_remaining_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_remaining_[slot_index];
}

lv_subject_t* AmsState::get_slot_material_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_materials_[slot_index];
}

lv_subject_t* AmsState::get_slot_fill_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_fills_[slot_index];
}

// Per-slot LIVE state subjects. These are static-array (singleton-lifetime)
// subjects, so the (slot, SubjectLifetime&) overloads return an EMPTY lifetime
// token — the documented contract for static subjects (ui_observer_guard.h),
// always-alive, no dynamic recreation. The empty-token overload exists for
// call-site symmetry with the project's dynamic-subject accessors.
lv_subject_t* AmsState::get_slot_segment_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_segments_[slot_index];
}

lv_subject_t* AmsState::get_slot_segment_subject(int slot_index, SubjectLifetime& lifetime) {
    lifetime.reset(); // static subject — empty (always-alive) token
    return get_slot_segment_subject(slot_index);
}

lv_subject_t* AmsState::get_slot_toolhead_present_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_toolhead_present_[slot_index];
}

lv_subject_t* AmsState::get_slot_toolhead_present_subject(int slot_index,
                                                          SubjectLifetime& lifetime) {
    lifetime.reset(); // static subject — empty (always-alive) token
    return get_slot_toolhead_present_subject(slot_index);
}

lv_subject_t* AmsState::get_slot_active_loaded_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_active_loaded_[slot_index];
}

lv_subject_t* AmsState::get_slot_active_loaded_subject(int slot_index, SubjectLifetime& lifetime) {
    lifetime.reset(); // static subject — empty (always-alive) token
    return get_slot_active_loaded_subject(slot_index);
}

lv_subject_t* AmsState::get_unit_temp_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &unit_temp_[unit_index];
}

lv_subject_t* AmsState::get_unit_humidity_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &unit_humidity_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_temp_text_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_temp_text_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_humidity_text_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_humidity_text_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_visible_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_visible_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_humidity_status_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_humidity_status_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_humidity_visible_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_humidity_visible_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_drying_active_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_drying_active_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_drying_text_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_drying_text_[unit_index];
}

AmsState::EnvIndicatorSubjectNames AmsState::env_indicator_subject_names(int unit_index) {
    EnvIndicatorSubjectNames names;

    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        names.temp_text = ENV_IND_OFF_TEXT_SUBJECT;
        names.humidity_text = ENV_IND_OFF_TEXT_SUBJECT;
        names.drying_text = ENV_IND_OFF_TEXT_SUBJECT;
        names.humidity_status = ENV_IND_OFF_FLAG_SUBJECT;
        names.humidity_visible = ENV_IND_OFF_FLAG_SUBJECT;
        names.visible = ENV_IND_OFF_FLAG_SUBJECT;
        names.drying_active = ENV_IND_OFF_FLAG_SUBJECT;
        return names;
    }

    auto expand = [unit_index](const char* suffix) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ams_env_ind_%d_%s", unit_index, suffix);
        return std::string(buf);
    };
    names.temp_text = expand("temp_text");
    names.humidity_text = expand("humidity_text");
    names.humidity_status = expand("humidity_status");
    names.humidity_visible = expand("humidity_visible");
    names.visible = expand("visible");
    names.drying_active = expand("drying_active");
    names.drying_text = expand("drying_text");
    return names;
}

lv_subject_t* AmsState::get_slot_color_subject(int backend_index, int slot_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backend_index == 0) {
        return get_slot_color_subject(slot_index);
    }
    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        return nullptr;
    }
    auto& subs = secondary_slot_subjects_[sec_idx];
    if (slot_index < 0 || slot_index >= subs.slot_count) {
        return nullptr;
    }
    return &subs.colors[slot_index];
}

lv_subject_t* AmsState::get_slot_status_subject(int backend_index, int slot_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backend_index == 0) {
        return get_slot_status_subject(slot_index);
    }
    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        return nullptr;
    }
    auto& subs = secondary_slot_subjects_[sec_idx];
    if (slot_index < 0 || slot_index >= subs.slot_count) {
        return nullptr;
    }
    return &subs.statuses[slot_index];
}

lv_subject_t* AmsState::get_slot_color_subject(int backend_index, int slot_index,
                                               SubjectLifetime& lifetime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backend_index == 0) {
        lifetime.reset(); // static subject — empty (always-alive) token
        return get_slot_color_subject(slot_index);
    }
    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        lifetime.reset();
        return nullptr;
    }
    lifetime = secondary_slot_subjects_[sec_idx].lifetime;
    return get_slot_color_subject(backend_index, slot_index);
}

lv_subject_t* AmsState::get_slot_status_subject(int backend_index, int slot_index,
                                                SubjectLifetime& lifetime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backend_index == 0) {
        lifetime.reset(); // static subject — empty (always-alive) token
        return get_slot_status_subject(slot_index);
    }
    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        lifetime.reset();
        return nullptr;
    }
    lifetime = secondary_slot_subjects_[sec_idx].lifetime;
    return get_slot_status_subject(backend_index, slot_index);
}

lv_subject_t* AmsState::get_slot_fill_subject(int backend_index, int slot_index,
                                              SubjectLifetime& lifetime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (backend_index == 0) {
        lifetime.reset(); // static subject — empty (always-alive) token
        return get_slot_fill_subject(slot_index);
    }
    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        lifetime.reset();
        return nullptr;
    }
    auto& subs = secondary_slot_subjects_[sec_idx];
    if (slot_index < 0 || slot_index >= subs.slot_count) {
        lifetime.reset();
        return nullptr;
    }
    lifetime = subs.lifetime;
    return &subs.fills[slot_index];
}

void AmsState::BackendSlotSubjects::init(int count) {
    slot_count = count;
    colors.resize(count);
    statuses.resize(count);
    fills.resize(count);
    for (int i = 0; i < count; ++i) {
        lv_subject_init_int(&colors[i], static_cast<int>(AMS_DEFAULT_SLOT_COLOR));
        lv_subject_init_int(&statuses[i], static_cast<int>(SlotStatus::UNKNOWN));
        lv_subject_init_int(&fills[i], -1);
    }
    // Fresh lifetime token: observers bound via the token'd accessors expire
    // when deinit() invalidates it on backend rediscovery.
    lifetime = std::make_shared<bool>(true);
}

void AmsState::BackendSlotSubjects::deinit() {
    // Invalidate the lifetime token FIRST so any live observer's weak_ptr is
    // dead before the subjects it points at are freed (#705 ordering).
    if (lifetime) {
        *lifetime = false;
    }
    lifetime.reset();
    for (auto& c : colors)
        lv_subject_deinit(&c);
    for (auto& s : statuses)
        lv_subject_deinit(&s);
    for (auto& f : fills)
        lv_subject_deinit(&f);
    colors.clear();
    statuses.clear();
    fills.clear();
    slot_count = 0;
}

void AmsState::sync_backend(int backend_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (backend_index == 0) {
        sync_from_backend();
        return;
    }

    auto* backend = get_backend(backend_index);
    if (!backend) {
        return;
    }

    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        return;
    }

    AmsSystemInfo info = backend->get_system_info();
    auto& subs = secondary_slot_subjects_[sec_idx];

    for (int i = 0; i < std::min(info.total_slots, subs.slot_count); ++i) {
        const SlotInfo* slot = info.get_slot_global(i);
        if (slot) {
            lv_subject_set_int(&subs.colors[i], static_cast<int>(slot->color_rgb));
            lv_subject_set_int(&subs.statuses[i], static_cast<int>(slot->status));
            lv_subject_set_int(&subs.fills[i], slot->display_fill_pct());
        }
    }

    spdlog::debug("[AMS State] Synced secondary backend {} - slots={}", backend_index,
                  info.total_slots);

    // Re-evaluate "Currently Loaded" display — the active loaded filament may
    // belong to this secondary backend (e.g., AMS_2 just finished loading).
    sync_current_loaded_from_backend();
}

void AmsState::update_slot_for_backend(int backend_index, int slot_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (backend_index == 0) {
        update_slot(slot_index);
        return;
    }

    auto* backend = get_backend(backend_index);
    if (!backend || slot_index < 0) {
        return;
    }

    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        return;
    }

    auto& subs = secondary_slot_subjects_[sec_idx];
    if (slot_index >= subs.slot_count) {
        return;
    }

    SlotInfo slot = backend->get_slot_info(slot_index);
    if (slot.slot_index >= 0) {
        lv_subject_set_int(&subs.colors[slot_index], static_cast<int>(slot.color_rgb));
        lv_subject_set_int(&subs.statuses[slot_index], static_cast<int>(slot.status));
        lv_subject_set_int(&subs.fills[slot_index], slot.display_fill_pct());

        spdlog::trace("[AMS State] Updated backend {} slot {} - color=0x{:06X}, status={}",
                      backend_index, slot_index, slot.color_rgb,
                      slot_status_to_string(slot.status));
    }
}

void AmsState::sync_from_backend() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto* backend = get_backend(0);
    if (!backend) {
        return;
    }

    AmsSystemInfo info = backend->get_system_info();

    // Update system-level subjects
    int new_type = static_cast<int>(info.type);
    if (lv_subject_get_int(&ams_type_) != new_type) {
        lv_subject_set_int(&ams_type_, new_type);
    }
    int new_action = static_cast<int>(info.action);
    // One-shot runout grace. An unload ends with the filament deliberately
    // dragged off the toolhead sensor, and that empty reading is the operation
    // working, not a runout — but is_filament_operation_active() only covers
    // the window while the action is still running. Measured on a K2 Plus:
    // the script completed at 12:03:02 and the sensor cleared at 12:03:12, ten
    // seconds after the guard had closed, so the idle runout modal fired on a
    // deliberate unload.
    //
    // Tracked across the whole operation rather than off an UNLOADING -> IDLE
    // edge, because apply_synthesized_action_locked() overwrites the action
    // with a sub-phase as physical signals arrive: that K2 unload actually
    // ended CUTTING -> IDLE, which such an edge would have missed entirely.
    {
        const auto action = static_cast<AmsAction>(new_action);
        const auto prev = static_cast<AmsAction>(lv_subject_get_int(&ams_action_));
        if (action == AmsAction::UNLOADING) {
            saw_unload_in_op_ = true;
        }
        if (action == AmsAction::IDLE && prev != AmsAction::IDLE) {
            post_unload_runout_grace_ = saw_unload_in_op_;
            if (post_unload_runout_grace_) {
                post_unload_runout_grace_at_ = std::chrono::steady_clock::now();
            }
            saw_unload_in_op_ = false;
        }
    }
    if (lv_subject_get_int(&ams_action_) != new_action) {
        spdlog::debug("[AmsState] sync_from_backend: action changed to {} ({})", new_action,
                      ams_action_to_string(info.action));
        lv_subject_set_int(&ams_action_, new_action);
    }
    // Granular firmware sub-phase (Snapmaker U1: Home/Select/Heat/Move). Most
    // backends leave operation_phase at -1, so this is a no-op for them.
    if (lv_subject_get_int(&ams_operation_phase_) != info.operation_phase) {
        spdlog::debug("[AmsState] sync_from_backend: operation_phase changed to {}",
                      info.operation_phase);
        lv_subject_set_int(&ams_operation_phase_, info.operation_phase);
    }
    // Indeterminate "Working…" busy flag (AD5X IFS row 14; other backends leave
    // it 0). Drives the sidebar's frozen-temp -> spinner swap on the live Heat
    // step. In lockstep with operation_phase above.
    int new_indet = info.operation_indeterminate ? 1 : 0;
    if (lv_subject_get_int(&ams_operation_indeterminate_) != new_indet) {
        spdlog::debug("[AmsState] sync_from_backend: operation_indeterminate changed to {}",
                      new_indet);
        lv_subject_set_int(&ams_operation_indeterminate_, new_indet);
    }

    // Set system name from backend type_name or fallback to type string
    std::string sys_name;
    if (!info.type_name.empty()) {
        sys_name = info.type_name;
    } else {
        sys_name = ams_type_to_string(info.type);
    }
    if (strcmp(lv_subject_get_string(&ams_system_name_), sys_name.c_str()) != 0) {
        lv_subject_copy_string(&ams_system_name_, sys_name.c_str());
    }

    // Set system logo path for declarative image binding (pointer subject for bind_src)
    const char* logo_path = get_logo_path(sys_name);
    const char* new_logo = logo_path ? logo_path : "";
    if (strcmp(system_logo_buf_, new_logo) != 0) {
        strncpy(system_logo_buf_, new_logo, sizeof(system_logo_buf_) - 1);
        system_logo_buf_[sizeof(system_logo_buf_) - 1] = '\0';
        lv_subject_set_pointer(&ams_system_logo_, system_logo_buf_);
    }
    if (lv_subject_get_int(&current_slot_) != info.current_slot) {
        spdlog::debug("[AmsState] current_slot changed: {} → {}",
                      lv_subject_get_int(&current_slot_), info.current_slot);
        lv_subject_set_int(&current_slot_, info.current_slot);
    }
    if (lv_subject_get_int(&pending_target_slot_) != info.pending_target_slot) {
        lv_subject_set_int(&pending_target_slot_, info.pending_target_slot);
    }
    if (lv_subject_get_int(&ams_current_tool_) != info.current_tool) {
        lv_subject_set_int(&ams_current_tool_, info.current_tool);
    }

    // Push tool topology to ToolState when the active backend multiplexes tools.
    // Otherwise leave ToolState in its extruder-enumerated state.
    if (auto topo = build_ams_topology(backend, 0)) {
        helix::ToolState::instance().set_ams_topology(*topo);
    } else if (helix::ToolState::instance().ams_topology_active()) {
        // Backend stopped multiplexing (e.g., AMS removed). Drop the override
        // so callers can rebuild tools_ from extruders.
        helix::ToolState::instance().clear_ams_topology();
    }

    // Tool text formatting (ams_current_tool_text_) handled by UI-layer observer

    int new_loaded = info.filament_loaded ? 1 : 0;
    if (lv_subject_get_int(&filament_loaded_) != new_loaded) {
        lv_subject_set_int(&filament_loaded_, new_loaded);
    }

    // The runout indicator needs an EDGE, not a level, plus a paused print.
    //
    // `AmsSystemInfo::filament_runout` is a sticky latch on the CFS: it mirrors
    // `box.filament_useup`, which BoxAction::send_data sets when the box reports
    // the spool used up and which ONLY BoxAction::extruder_extrude clears, on a
    // successful extrude. It is not print-scoped and nothing resets it when a job
    // ends. A live K2 Plus read `filament_useup: 1` at `print_stats.state:
    // standby`. Level-and-paused therefore lit the warning icon on ANY unrelated
    // pause afterwards — a user pause, an M600, a CFS fault pausing via
    // BoxError.handle_event — for a runout that may have been days earlier.
    //
    // Requiring a false->true transition witnessed while the job was PRINTING or
    // PAUSED fixes that without needing per-backend knowledge, and is correct for
    // AD5X IFS too: its detector only ever raises the flag while paused, which is
    // one of the two states that arm the edge here. The cost is the same one
    // AmsBackendAd5xIfs::evaluate_runout_locked() already accepts deliberately —
    // a printer that boots into a job already paused on a runout reports nothing,
    // because we witnessed no transition.
    // RAW_PRINT_STATE_OK: the edge must be witnessed while the printer is
    // actually running the job. Arming it during Preparing would light the
    // warning for a latch raised before any material moved.
    const PrintJobState job_state = get_printer_state().get_print_job_state();
    const bool paused = job_state == PrintJobState::PAUSED;
    const bool job_running = paused || job_state == PrintJobState::PRINTING;

    if (!runout_level_seeded_) {
        // Seed only; a flag that was already set before we started watching
        // describes no transition of ours.
        prev_backend_runout_ = info.filament_runout;
        runout_level_seeded_ = true;
    } else if (info.filament_runout && !prev_backend_runout_) {
        runout_edge_armed_ = job_running;
    } else if (!info.filament_runout) {
        // Backend withdrew the flag: the fault is over regardless of print state.
        runout_edge_armed_ = false;
    }
    prev_backend_runout_ = info.filament_runout;

    // End of episode. Two ways out, and neither can be simplified to "not
    // paused": the arm is normally made while PRINTING, one status frame before
    // the firmware's pause lands, so disarming on !paused would throw away every
    // real runout before it could be shown.
    //   - the job stopped running at all (STANDBY / COMPLETE / CANCELLED)
    //   - the job left PAUSED, i.e. the user resumed or cancelled
    // The second is what the sticky latch makes necessary: on the CFS the level
    // can stay true forever, so leaving PAUSED is the only evidence that the
    // runout was dealt with.
    if (!job_running || (runout_prev_paused_ && !paused)) {
        runout_edge_armed_ = false;
    }
    runout_prev_paused_ = paused;

    int new_runout = (runout_edge_armed_ && info.filament_runout && paused) ? 1 : 0;
    if (lv_subject_get_int(&filament_runout_) != new_runout) {
        spdlog::debug("[AmsState] filament runout indicator -> {} (level={}, armed={}, paused={})",
                      new_runout, info.filament_runout, runout_edge_armed_, paused);
        lv_subject_set_int(&filament_runout_, new_runout);
    }
    // The one bypass truth: the backend's own is_bypass_active(), the same
    // predicate BypassToggleController branches on when the user taps. This
    // subject used to be derived independently from current_slot == -2, so with
    // a declaration latched and the filament pulled the switch rendered
    // unchecked while a tap took the DISABLE path — "turn it on" answered
    // "Bypass disabled", and the pre-print gate meanwhile acted on a bypass the
    // user could not see or clear. Display and action now read one value.
    // Filament back at the toolhead retires the grace: it was armed for the
    // removal this unload caused, and anything after a reload is a new event.
    if (info.filament_loaded && post_unload_runout_grace_) {
        post_unload_runout_grace_ = false;
        spdlog::debug("[AmsState] Post-unload runout grace retired — filament loaded again");
    }

    const int new_bypass = backend->is_bypass_active() ? 1 : 0;
    if (lv_subject_get_int(&bypass_active_) != new_bypass) {
        spdlog::debug("[AmsState] bypass -> {}", new_bypass);
        lv_subject_set_int(&bypass_active_, new_bypass);
    }

    // Engaging bypass changes nothing about any slot, so the per-slot delta scan
    // in sync_slots_from_backend() never bumps slots_version. The pre-print
    // filament check keys on bypass (PreflightValidator) and slots_version is its
    // ONLY refresh trigger, so without this the cached result goes stale: engage
    // bypass while a file's detail view is already open and the false
    // "T0 has no filament loaded" block still fires on Print.
    //
    // Still tracked off any_bypass_active() rather than the bypass_active_
    // subject above, but for a different reason now that both read
    // is_bypass_active(): the subject reports backend 0 only, while this walks
    // every backend, and the pre-print check it refreshes is whole-printer.
    const bool bypass_now = any_bypass_active();
    if (bypass_now != last_bypass_active_) {
        last_bypass_active_ = bypass_now;
        spdlog::debug("[AmsState] Bypass -> {}, bumping slots_version for the pre-print check",
                      bypass_now);
        bump_slots_version();
        // Notification only — the bypass⇄sensor policy (arm/restore runout
        // sensors at the firmware level) lives entirely in
        // FilamentSensorManager, the sensor abstraction layer.
        FilamentSensorManager::instance().on_bypass_active_changed(bypass_now);

        // Publish the external spool as an extra lane_data entry for slicer
        // sync (OrcaSlicer) when bypass engages. Capability question via the
        // backend virtual — only backends that own a lane_data mirror and
        // support bypass answer; AmsState names no system.
        if (bypass_now) {
            const auto spool = get_external_spool_info();
            for (auto& backend : backends_) {
                if (backend) {
                    backend->publish_external_spool_lane(spool.has_value() ? &spool.value()
                                                                           : nullptr);
                }
            }
        }
    }
    int new_supports_bypass = helix::bypass_available_for(info.supports_bypass) ? 1 : 0;
    if (lv_subject_get_int(&supports_bypass_) != new_supports_bypass) {
        lv_subject_set_int(&supports_bypass_, new_supports_bypass);
    }

    // Update external spool color from persistent settings
    auto ext_spool = helix::SettingsManager::instance().get_external_spool_info();
    int new_ext_color = ext_spool.has_value() ? static_cast<int>(ext_spool->color_rgb) : 0;
    if (lv_subject_get_int(&external_spool_color_) != new_ext_color) {
        lv_subject_set_int(&external_spool_color_, new_ext_color);
    }
    lv_subject_copy_string(&external_spool_material_,
                           ext_spool.has_value() ? ext_spool->material.c_str() : "");
    if (lv_subject_get_int(&ams_slot_count_) != info.total_slots) {
        lv_subject_set_int(&ams_slot_count_, info.total_slots);
    }

    // Update tool change progress raw data (text formatting in UI layer)
    if (info.number_of_toolchanges > 0) {
        if (lv_subject_get_int(&toolchange_visible_) != 1) {
            lv_subject_set_int(&toolchange_visible_, 1);
        }
    } else {
        if (lv_subject_get_int(&toolchange_visible_) != 0) {
            lv_subject_set_int(&toolchange_visible_, 0);
        }
    }
    if (lv_subject_get_int(&ams_current_toolchange_) != info.current_toolchange) {
        lv_subject_set_int(&ams_current_toolchange_, info.current_toolchange);
    }
    if (lv_subject_get_int(&ams_number_of_toolchanges_) != info.number_of_toolchanges) {
        lv_subject_set_int(&ams_number_of_toolchanges_, info.number_of_toolchanges);
    }

    // Cache the backend-supplied operation_detail so the print-state observer
    // can recompute the displayed string later without re-querying the backend.
    last_operation_detail_ = info.operation_detail;
    recompute_action_detail();

    // Update path visualization subjects
    int new_topology = static_cast<int>(backend->get_topology());
    if (lv_subject_get_int(&path_topology_) != new_topology) {
        lv_subject_set_int(&path_topology_, new_topology);
    }
    if (lv_subject_get_int(&path_active_slot_) != info.current_slot) {
        lv_subject_set_int(&path_active_slot_, info.current_slot);
    }
    int new_filament_seg = static_cast<int>(backend->get_filament_segment());
    if (lv_subject_get_int(&path_filament_segment_) != new_filament_seg) {
        lv_subject_set_int(&path_filament_segment_, new_filament_seg);
    }
    int new_error_seg = static_cast<int>(backend->infer_error_segment());
    if (lv_subject_get_int(&path_error_segment_) != new_error_seg) {
        lv_subject_set_int(&path_error_segment_, new_error_seg);
    }
    // If backend provides bowden progress (v4), use it to drive animation progress.
    // Otherwise, path_anim_progress_ stays under UI animation control.
    int bowden_progress = backend->get_bowden_progress();
    if (bowden_progress >= 0 && lv_subject_get_int(&path_anim_progress_) != bowden_progress) {
        lv_subject_set_int(&path_anim_progress_, bowden_progress);
    }

    // Update per-slot subjects, only firing when values actually change
    bool any_slot_changed = false;
    for (int i = 0; i < std::min(info.total_slots, MAX_SLOTS); ++i) {
        const SlotInfo* slot = info.get_slot_global(i);
        if (slot) {
            int new_color = static_cast<int>(slot->color_rgb);
            if (lv_subject_get_int(&slot_colors_[i]) != new_color) {
                lv_subject_set_int(&slot_colors_[i], new_color);
                any_slot_changed = true;
            }
            int new_status = static_cast<int>(slot->status);
            if (lv_subject_get_int(&slot_statuses_[i]) != new_status) {
                lv_subject_set_int(&slot_statuses_[i], new_status);
                any_slot_changed = true;
            }

            // Fill percent (canonical display_fill_pct encoding). The ams_slot
            // widget observes this — so spool fill renders from state on every
            // panel, not just the ones that remember to push it imperatively.
            int new_fill = slot->display_fill_pct();
            if (lv_subject_get_int(&slot_fills_[i]) != new_fill) {
                lv_subject_set_int(&slot_fills_[i], new_fill);
                any_slot_changed = true;
            }

            // Update remaining filament string
            std::string remaining;
            if (slot->remaining_length_m > 0) {
                remaining = std::to_string(static_cast<int>(slot->remaining_length_m)) + "m";
            } else if (slot->remaining_weight_g > 0) {
                remaining = std::to_string(static_cast<int>(slot->remaining_weight_g)) + "g";
            }
            if (strcmp(lv_subject_get_string(&slot_remaining_[i]), remaining.c_str()) != 0) {
                lv_subject_copy_string(&slot_remaining_[i], remaining.c_str());
            }

            // Material type. Unlike remaining, a material delta MUST bump
            // slots_version: the panel's material label is re-read only by
            // refresh_slots() (it has no direct binding), so a type change that
            // leaves color/status unchanged would otherwise leave the label stale
            // (#1065 — native ZMOD AD5X "color updates, material stuck").
            if (strcmp(lv_subject_get_string(&slot_materials_[i]), slot->material.c_str()) != 0) {
                lv_subject_copy_string(&slot_materials_[i], slot->material.c_str());
                any_slot_changed = true;
            }

            // Per-slot LIVE state: path segment, toolhead-present, active-loaded.
            // Sourced directly from the backend accessors so the panel observes
            // real-time sensor changes (path redraw + active-lane highlight).
            int new_segment = static_cast<int>(backend->get_slot_filament_segment(i));
            if (lv_subject_get_int(&slot_segments_[i]) != new_segment) {
                lv_subject_set_int(&slot_segments_[i], new_segment);
                any_slot_changed = true;
            }
            int new_toolhead = backend->slot_has_filament_at_toolhead(i) ? 1 : 0;
            if (lv_subject_get_int(&slot_toolhead_present_[i]) != new_toolhead) {
                lv_subject_set_int(&slot_toolhead_present_[i], new_toolhead);
                any_slot_changed = true;
            }
            int new_active = backend->slot_is_actively_loaded(i) ? 1 : 0;
            if (lv_subject_get_int(&slot_active_loaded_[i]) != new_active) {
                lv_subject_set_int(&slot_active_loaded_[i], new_active);
                any_slot_changed = true;
            }
        }
    }

    // Detect tool_to_slot_map changes (e.g. user remapped T0→T2) and bump
    // tool_map_version_ so the gcode renderer can refresh tool colors.
    if (info.tool_to_slot_map != last_tool_map_) {
        last_tool_map_ = info.tool_to_slot_map;
        int v = lv_subject_get_int(&tool_map_version_);
        lv_subject_set_int(&tool_map_version_, v + 1);
        spdlog::debug("[AmsState] tool_to_slot_map changed, version now {}", v + 1);
    }

    // Sync spool assignments to ToolState for slots with mapped tools.
    //
    // The clear branch matters as much as the assign one: this only ever
    // assigned, so a lane that lost its spool (eject, or an explicit unlink)
    // left the old assignment behind in ToolState — and ToolState persists to
    // tool_spools.json plus a Moonraker DB key, so the stale spool outlived
    // restarts. Observed on the .112 BoxTurtle: "Assigned spool 86 () to tool 0"
    // fired during an EJECT, and ToolState::clear_spool() had no callers at all.
    for (int i = 0; i < std::min(info.total_slots, MAX_SLOTS); ++i) {
        const SlotInfo* slot = info.get_slot_global(i);
        if (!slot || slot->mapped_tool < 0) {
            continue;
        }
        if (slot->spoolman_id > 0) {
            ToolState::instance().assign_spool(slot->mapped_tool, slot->spoolman_id,
                                               slot->spool_name, slot->remaining_weight_g,
                                               slot->total_weight_g);
        } else if (backend->has_firmware_spool_persistence()) {
            // Only clear when the SLOT is authoritative. For backends without
            // firmware spool persistence (toolchanger) the flow runs the other
            // way — ToolState is the source of truth and slots start empty — so
            // clearing here would destroy the assignment the reverse sync below
            // is about to propagate.
            ToolState::instance().clear_spool(slot->mapped_tool);
        }
    }

    // Reverse sync: populate backend slots from ToolState for backends that
    // don't persist spool info in firmware (e.g., toolchanger). Without this,
    // spool assignments loaded from Moonraker DB / local JSON on startup
    // don't propagate back to slot UI subjects.
    if (!backend->has_firmware_spool_persistence()) {
        auto& tool_state = ToolState::instance();
        const auto& tools = tool_state.tools();
        for (int i = 0; i < std::min(info.total_slots, MAX_SLOTS); ++i) {
            const SlotInfo* slot = info.get_slot_global(i);
            if (slot && slot->mapped_tool >= 0 && slot->spoolman_id == 0) {
                int ti = slot->mapped_tool;
                if (ti >= 0 && ti < static_cast<int>(tools.size()) && tools[ti].spoolman_id > 0) {
                    SlotInfo updated = *slot;
                    updated.spoolman_id = tools[ti].spoolman_id;
                    updated.spool_name = tools[ti].spool_name;
                    updated.remaining_weight_g = tools[ti].remaining_weight_g;
                    updated.total_weight_g = tools[ti].total_weight_g;
                    backend->set_slot_info(i, updated, false);
                }
            }
        }

        tool_state.save_spool_assignments_if_dirty(get_moonraker_api());
    }

    // Update per-unit environment subjects (CFS temperature/humidity)
    for (const auto& unit : info.units) {
        int idx = unit.unit_index;
        if (idx >= 0 && idx < MAX_UNITS) {
            if (unit.environment.has_value()) {
                int temp_tenths = static_cast<int>(unit.environment->temperature_c * 10.0f);
                int humidity = static_cast<int>(unit.environment->humidity_pct);
                if (lv_subject_get_int(&unit_temp_[idx]) != temp_tenths) {
                    lv_subject_set_int(&unit_temp_[idx], temp_tenths);
                }
                if (lv_subject_get_int(&unit_humidity_[idx]) != humidity) {
                    lv_subject_set_int(&unit_humidity_[idx], humidity);
                }
            } else {
                if (lv_subject_get_int(&unit_temp_[idx]) != 0) {
                    lv_subject_set_int(&unit_temp_[idx], 0);
                }
                if (lv_subject_get_int(&unit_humidity_[idx]) != 0) {
                    lv_subject_set_int(&unit_humidity_[idx], 0);
                }
            }
        }
    }

    // Clear environment subjects for units beyond what backend reports
    for (int i = static_cast<int>(info.units.size()); i < MAX_UNITS; ++i) {
        if (lv_subject_get_int(&unit_temp_[i]) != 0) {
            lv_subject_set_int(&unit_temp_[i], 0);
        }
        if (lv_subject_get_int(&unit_humidity_[i]) != 0) {
            lv_subject_set_int(&unit_humidity_[i], 0);
        }
    }

    // Update per-unit environment indicator display subjects (formatted text for XML).
    // The dryer is fetched per-unit below so each box's indicator reflects its own
    // drying state — the indicator can be made reachable for any drying-capable box,
    // not only when a live temp/humidity reading is present.
    for (const auto& unit : info.units) {
        int idx = unit.unit_index;
        if (idx < 0 || idx >= MAX_UNITS)
            continue;
        const bool has_env = unit.environment.has_value();
        if (has_env) {
            // Format temperature text (e.g., "24°C")
            char buf[ENV_IND_TEXT_BUF_SIZE];
            snprintf(buf, sizeof(buf),
                     "%d\xC2\xB0"
                     "C",
                     static_cast<int>(unit.environment->temperature_c));
            if (strcmp(lv_subject_get_string(&env_ind_temp_text_[idx]), buf) != 0) {
                lv_subject_copy_string(&env_ind_temp_text_[idx], buf);
            }

            // Format humidity text (e.g., "46%")
            snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(unit.environment->humidity_pct));
            if (strcmp(lv_subject_get_string(&env_ind_humidity_text_[idx]), buf) != 0) {
                lv_subject_copy_string(&env_ind_humidity_text_[idx], buf);
            }

            // Determine humidity status color based on most restrictive loaded material
            // 0=ok (green), 1=warn (yellow), 2=danger (red)
            int humidity_status = 0;
            float humidity_pct = unit.environment->humidity_pct;
            float most_restrictive_good = 999.0f;
            float most_restrictive_warn = 999.0f;
            bool found_any_range = false;

            for (int si = 0; si < unit.slot_count; ++si) {
                int gi = unit.first_slot_global_index + si;
                SlotInfo slot = backend->get_slot_info(gi);
                if (!slot.material.empty()) {
                    const auto range = filament::get_comfort_range(slot.material);
                    if (range) {
                        found_any_range = true;
                        if (range->max_humidity_good < most_restrictive_good) {
                            most_restrictive_good = range->max_humidity_good;
                        }
                        if (range->max_humidity_warn < most_restrictive_warn) {
                            most_restrictive_warn = range->max_humidity_warn;
                        }
                    }
                }
            }

            if (found_any_range) {
                if (humidity_pct > most_restrictive_warn) {
                    humidity_status = 2;
                } else if (humidity_pct > most_restrictive_good) {
                    humidity_status = 1;
                }
            }

            if (lv_subject_get_int(&env_ind_humidity_status_[idx]) != humidity_status) {
                lv_subject_set_int(&env_ind_humidity_status_[idx], humidity_status);
            }

        } else {
            // No live reading — show an em-dash so a drying-capable unit still
            // presents a tappable indicator instead of a blank temperature.
            if (strcmp(lv_subject_get_string(&env_ind_temp_text_[idx]), "\xE2\x80\x94") != 0) {
                lv_subject_copy_string(&env_ind_temp_text_[idx], "\xE2\x80\x94");
            }
        }

        // Indicator is reachable when there is live environment data OR this
        // unit's dryer is supported — otherwise a dryer-capable box with no
        // temp/humidity sensor would have no way to open the drying controls.
        const bool unit_supports_dryer = backend->get_dryer_info(idx).supported;
        const int ind_vis = (has_env || unit_supports_dryer) ? 1 : 0;
        if (lv_subject_get_int(&env_ind_visible_[idx]) != ind_vis) {
            lv_subject_set_int(&env_ind_visible_[idx], ind_vis);
        }

        // Humidity row only when a real humidity reading exists.
        const int hum_vis = (has_env && unit.environment->has_humidity) ? 1 : 0;
        if (lv_subject_get_int(&env_ind_humidity_visible_[idx]) != hum_vis) {
            lv_subject_set_int(&env_ind_humidity_visible_[idx], hum_vis);
        }
    }

    // Update drying state for indicator — per-unit dryer.
    for (int i = 0; i < MAX_UNITS; ++i) {
        // Only update drying for units that have environment data visible
        if (lv_subject_get_int(&env_ind_visible_[i]) != 1) {
            if (lv_subject_get_int(&env_ind_drying_active_[i]) != 0) {
                lv_subject_set_int(&env_ind_drying_active_[i], 0);
            }
            continue;
        }
        const DryerInfo dryer = backend->get_dryer_info(i);
        if (dryer.supported && dryer.active) {
            if (lv_subject_get_int(&env_ind_drying_active_[i]) != 1) {
                lv_subject_set_int(&env_ind_drying_active_[i], 1);
            }
            // Format compact drying text — just countdown for the small indicator
            char drying_buf[ENV_IND_DRYING_BUF_SIZE];
            int hrs = dryer.remaining_min / 60;
            int mins = dryer.remaining_min % 60;
            if (hrs > 0) {
                snprintf(drying_buf, sizeof(drying_buf), "%d:%02d", hrs, mins);
            } else {
                snprintf(drying_buf, sizeof(drying_buf), "%d min", mins);
            }
            if (strcmp(lv_subject_get_string(&env_ind_drying_text_[i]), drying_buf) != 0) {
                lv_subject_copy_string(&env_ind_drying_text_[i], drying_buf);
            }
        } else {
            if (lv_subject_get_int(&env_ind_drying_active_[i]) != 0) {
                lv_subject_set_int(&env_ind_drying_active_[i], 0);
            }
        }
    }

    // Clear indicator for units beyond what backend reports
    for (int i = static_cast<int>(info.units.size()); i < MAX_UNITS; ++i) {
        if (lv_subject_get_int(&env_ind_visible_[i]) != 0) {
            lv_subject_set_int(&env_ind_visible_[i], 0);
        }
        if (lv_subject_get_int(&env_ind_humidity_visible_[i]) != 0) {
            lv_subject_set_int(&env_ind_humidity_visible_[i], 0);
        }
    }

    // Clear remaining slot subjects, only firing when values actually change
    for (int i = info.total_slots; i < MAX_SLOTS; ++i) {
        int default_color = static_cast<int>(AMS_DEFAULT_SLOT_COLOR);
        if (lv_subject_get_int(&slot_colors_[i]) != default_color) {
            lv_subject_set_int(&slot_colors_[i], default_color);
            any_slot_changed = true;
        }
        int default_status = static_cast<int>(SlotStatus::UNKNOWN);
        if (lv_subject_get_int(&slot_statuses_[i]) != default_status) {
            lv_subject_set_int(&slot_statuses_[i], default_status);
            any_slot_changed = true;
        }
        // Clear remaining filament for unused slots
        if (strcmp(lv_subject_get_string(&slot_remaining_[i]), "") != 0) {
            lv_subject_copy_string(&slot_remaining_[i], "");
        }
        // Clear material for unused slots — bump so the label clears (#1065)
        if (strcmp(lv_subject_get_string(&slot_materials_[i]), "") != 0) {
            lv_subject_copy_string(&slot_materials_[i], "");
            any_slot_changed = true;
        }
        // Reset per-slot LIVE state subjects for unused slots
        if (lv_subject_get_int(&slot_segments_[i]) != static_cast<int>(PathSegment::NONE)) {
            lv_subject_set_int(&slot_segments_[i], static_cast<int>(PathSegment::NONE));
            any_slot_changed = true;
        }
        if (lv_subject_get_int(&slot_toolhead_present_[i]) != 0) {
            lv_subject_set_int(&slot_toolhead_present_[i], 0);
            any_slot_changed = true;
        }
        if (lv_subject_get_int(&slot_active_loaded_[i]) != 0) {
            lv_subject_set_int(&slot_active_loaded_[i], 0);
            any_slot_changed = true;
        }
    }

    if (any_slot_changed) {
        spdlog::trace("[AmsState] Slot data changed, bumping version");
        bump_slots_version();
    }

    // Mirror the detail-view env indicator's currently-shown unit
    mirror_detail_env_subjects();

    // Sync dryer state (for systems with integrated drying like ACE)
    sync_dryer_from_backend();

    // Sync clog detection meter subjects
    sync_clog_meter_from_info(info);

    // Sync "Currently Loaded" display subjects (pass info to avoid re-fetching)
    sync_current_loaded_from_backend(info);

    // Sync the endless-spool status line (backend-neutral; every backend answers
    // the same capability question)
    sync_endless_spool_from_backend(backend);

    spdlog::trace("[AMS State] Synced from backend - type={}, slots={}, action={}, segment={}",
                  ams_type_to_string(info.type), info.total_slots,
                  ams_action_to_string(info.action),
                  path_segment_to_string(backend->get_filament_segment()));
}

void AmsState::update_slot(int slot_index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto* backend = get_backend(0);
    if (!backend || slot_index < 0 || slot_index >= MAX_SLOTS) {
        return;
    }

    SlotInfo slot = backend->get_slot_info(slot_index);
    if (slot.slot_index >= 0) {
        bool changed = false;
        int new_color = static_cast<int>(slot.color_rgb);
        if (lv_subject_get_int(&slot_colors_[slot_index]) != new_color) {
            lv_subject_set_int(&slot_colors_[slot_index], new_color);
            changed = true;
        }
        int new_status = static_cast<int>(slot.status);
        if (lv_subject_get_int(&slot_statuses_[slot_index]) != new_status) {
            lv_subject_set_int(&slot_statuses_[slot_index], new_status);
            changed = true;
        }

        // Update remaining filament string
        std::string remaining;
        if (slot.remaining_length_m > 0) {
            remaining = std::to_string(static_cast<int>(slot.remaining_length_m)) + "m";
        } else if (slot.remaining_weight_g > 0) {
            remaining = std::to_string(static_cast<int>(slot.remaining_weight_g)) + "g";
        }
        if (strcmp(lv_subject_get_string(&slot_remaining_[slot_index]), remaining.c_str()) != 0) {
            lv_subject_copy_string(&slot_remaining_[slot_index], remaining.c_str());
        }

        // Material type — a delta bumps slots_version so refresh_slots() re-reads
        // the material label even when color/status are unchanged (#1065).
        if (strcmp(lv_subject_get_string(&slot_materials_[slot_index]), slot.material.c_str()) !=
            0) {
            lv_subject_copy_string(&slot_materials_[slot_index], slot.material.c_str());
            changed = true;
        }

        // Per-slot LIVE state: path segment, toolhead-present, active-loaded.
        int new_segment = static_cast<int>(backend->get_slot_filament_segment(slot_index));
        if (lv_subject_get_int(&slot_segments_[slot_index]) != new_segment) {
            lv_subject_set_int(&slot_segments_[slot_index], new_segment);
            changed = true;
        }
        int new_toolhead = backend->slot_has_filament_at_toolhead(slot_index) ? 1 : 0;
        if (lv_subject_get_int(&slot_toolhead_present_[slot_index]) != new_toolhead) {
            lv_subject_set_int(&slot_toolhead_present_[slot_index], new_toolhead);
            changed = true;
        }
        int new_active = backend->slot_is_actively_loaded(slot_index) ? 1 : 0;
        if (lv_subject_get_int(&slot_active_loaded_[slot_index]) != new_active) {
            lv_subject_set_int(&slot_active_loaded_[slot_index], new_active);
            changed = true;
        }

        if (changed) {
            bump_slots_version();
        }

        // Sync spool to ToolState if this slot maps to a tool
        if (slot.mapped_tool >= 0 && slot.spoolman_id > 0) {
            ToolState::instance().assign_spool(slot.mapped_tool, slot.spoolman_id, slot.spool_name,
                                               slot.remaining_weight_g, slot.total_weight_g);
            if (!backend->has_firmware_spool_persistence()) {
                ToolState::instance().save_spool_assignments(get_moonraker_api());
            }
        }

        spdlog::trace("[AMS State] Updated slot {} - color=0x{:06X}, status={}", slot_index,
                      slot.color_rgb, slot_status_to_string(slot.status));
    }
}

void AmsState::on_backend_event(int backend_index, const std::string& event,
                                const std::string& data) {
    spdlog::trace("[AMS State] Received event '{}' data='{}' from backend {}", event, data,
                  backend_index);

    auto queue_sync = [backend_index](bool full_sync, int slot_index) {
        helix::ui::queue_update(
            "AmsState::on_backend_event", [backend_index, full_sync, slot_index]() {
                // Skip if shutdown is in progress - AmsState singleton may be destroyed
                if (s_shutdown_flag.load(std::memory_order_acquire)) {
                    return;
                }

                if (full_sync) {
                    AmsState::instance().sync_backend(backend_index);
                } else {
                    AmsState::instance().update_slot_for_backend(backend_index, slot_index);
                }

                // Wake anything watching for backend data to land. Bumped AFTER
                // the sync so an observer that re-reads backend state sees the
                // synced values, not the previous ones. Main thread already (we
                // are inside the queue_update body), so the subject write is safe.
                auto* rev = AmsState::instance().get_ams_data_revision_subject();
                lv_subject_set_int(rev, lv_subject_get_int(rev) + 1);
            });
    };

    if (event == AmsBackend::EVENT_STATE_CHANGED) {
        queue_sync(true, -1);
    } else if (event == AmsBackend::EVENT_SLOT_CHANGED) {
        // Parse slot index from data. Fall back to a full sync for ANY case
        // where we can't parse a specific slot — empty data OR non-numeric.
        // Dropping the event silently (the old behavior) left the UI stale
        // whenever a backend forgot to pass slot_index.
        if (data.empty()) {
            queue_sync(true, -1);
        } else {
            try {
                int slot_index = std::stoi(data);
                queue_sync(false, slot_index);
            } catch (...) {
                queue_sync(true, -1);
            }
        }
    } else if (event == AmsBackend::EVENT_LOAD_COMPLETE ||
               event == AmsBackend::EVENT_UNLOAD_COMPLETE ||
               event == AmsBackend::EVENT_TOOL_CHANGED) {
        // These events indicate state change, sync everything
        queue_sync(true, -1);
    } else if (event == AmsBackend::EVENT_ERROR) {
        // Error occurred, sync to get error state
        queue_sync(true, -1);
        spdlog::warn("[AMS State] Backend error - {}", data);
    } else if (event == AmsBackend::EVENT_ATTENTION_REQUIRED) {
        // User intervention needed
        queue_sync(true, -1);
        spdlog::warn("[AMS State] Attention required - {}", data);
    }
}

void AmsState::sync_endless_spool_from_backend(AmsBackend* backend) {
    using helix::printer::EndlessSpoolStatus;
    using helix::printer::EndlessSpoolStatusKind;

    // Capabilities take the backend's own mutex_, which is the lock order
    // sync_from_backend() already established with get_system_info().
    EndlessSpoolStatus status;
    if (backend != nullptr) {
        status = helix::printer::endless_spool_status(backend->get_endless_spool_capabilities());
    }

    const int kind = static_cast<int>(status.kind);
    if (lv_subject_get_int(&ams_endless_state_) != kind) {
        spdlog::debug("[AmsState] endless spool status -> kind={} text='{}'", kind, status.text);
        lv_subject_set_int(&ams_endless_state_, kind);
    }
    // The kind can hold while the sentence changes (a CFS box that keeps
    // auto-refill on but gains a restriction reason), so the text is compared
    // independently rather than gated on the kind having moved.
    if (strcmp(lv_subject_get_string(&ams_endless_text_), status.text.c_str()) != 0) {
        lv_subject_copy_string(&ams_endless_text_, status.text.c_str());
    }
}

void AmsState::bump_slots_version() {
    int current = lv_subject_get_int(&slots_version_);
    lv_subject_set_int(&slots_version_, current + 1);
}

void AmsState::set_dryer_mirror_unit(int unit) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (dryer_mirror_unit_ == unit) {
        return;
    }
    dryer_mirror_unit_ = unit;
    sync_dryer_from_backend();
}

void AmsState::set_detail_env_unit(int unit) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    detail_env_unit_ = unit;
    mirror_detail_env_subjects();
}

void AmsState::mirror_detail_env_subjects() {
    int u = detail_env_unit_;
    if (u < 0 || u >= MAX_UNITS)
        u = 0;
    if (strcmp(lv_subject_get_string(&env_ind_detail_temp_text_),
               lv_subject_get_string(&env_ind_temp_text_[u])) != 0)
        lv_subject_copy_string(&env_ind_detail_temp_text_,
                               lv_subject_get_string(&env_ind_temp_text_[u]));
    if (strcmp(lv_subject_get_string(&env_ind_detail_humidity_text_),
               lv_subject_get_string(&env_ind_humidity_text_[u])) != 0)
        lv_subject_copy_string(&env_ind_detail_humidity_text_,
                               lv_subject_get_string(&env_ind_humidity_text_[u]));
    if (lv_subject_get_int(&env_ind_detail_humidity_status_) !=
        lv_subject_get_int(&env_ind_humidity_status_[u]))
        lv_subject_set_int(&env_ind_detail_humidity_status_,
                           lv_subject_get_int(&env_ind_humidity_status_[u]));
    if (lv_subject_get_int(&env_ind_detail_humidity_visible_) !=
        lv_subject_get_int(&env_ind_humidity_visible_[u]))
        lv_subject_set_int(&env_ind_detail_humidity_visible_,
                           lv_subject_get_int(&env_ind_humidity_visible_[u]));
    if (lv_subject_get_int(&env_ind_detail_visible_) != lv_subject_get_int(&env_ind_visible_[u]))
        lv_subject_set_int(&env_ind_detail_visible_, lv_subject_get_int(&env_ind_visible_[u]));
    if (lv_subject_get_int(&env_ind_detail_drying_active_) !=
        lv_subject_get_int(&env_ind_drying_active_[u]))
        lv_subject_set_int(&env_ind_detail_drying_active_,
                           lv_subject_get_int(&env_ind_drying_active_[u]));
    if (strcmp(lv_subject_get_string(&env_ind_detail_drying_text_),
               lv_subject_get_string(&env_ind_drying_text_[u])) != 0)
        lv_subject_copy_string(&env_ind_detail_drying_text_,
                               lv_subject_get_string(&env_ind_drying_text_[u]));
}

void AmsState::sync_dryer_from_backend() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto* backend = get_backend(0);
    if (!backend) {
        // No backend - clear dryer state
        if (lv_subject_get_int(&dryer_supported_) != 0) {
            lv_subject_set_int(&dryer_supported_, 0);
        }
        if (lv_subject_get_int(&dryer_active_) != 0) {
            lv_subject_set_int(&dryer_active_, 0);
        }
        return;
    }

    DryerInfo dryer = backend->get_dryer_info(dryer_mirror_unit_);

    // Update integer subjects
    int new_supported = dryer.supported ? 1 : 0;
    if (lv_subject_get_int(&dryer_supported_) != new_supported) {
        lv_subject_set_int(&dryer_supported_, new_supported);
    }
    int new_dryer_active = dryer.active ? 1 : 0;
    if (lv_subject_get_int(&dryer_active_) != new_dryer_active) {
        lv_subject_set_int(&dryer_active_, new_dryer_active);
    }
    int new_cur_temp = static_cast<int>(dryer.current_temp_c);
    if (lv_subject_get_int(&dryer_current_temp_) != new_cur_temp) {
        lv_subject_set_int(&dryer_current_temp_, new_cur_temp);
    }
    int new_tgt_temp = static_cast<int>(dryer.target_temp_c);
    if (lv_subject_get_int(&dryer_target_temp_) != new_tgt_temp) {
        lv_subject_set_int(&dryer_target_temp_, new_tgt_temp);
    }
    if (lv_subject_get_int(&dryer_remaining_min_) != dryer.remaining_min) {
        lv_subject_set_int(&dryer_remaining_min_, dryer.remaining_min);
    }
    int new_progress = dryer.get_progress_pct();
    if (lv_subject_get_int(&dryer_progress_pct_) != new_progress) {
        lv_subject_set_int(&dryer_progress_pct_, new_progress);
    }

    // Text formatting (dryer_current_temp_text_, dryer_target_temp_text_, dryer_time_text_)
    // is handled by observers in AmsDryerCard::setup() — UI-layer responsibility.

    spdlog::trace("[AMS State] Synced dryer - supported={}, active={}, temp={}→{}°C, {}min left",
                  dryer.supported, dryer.active, static_cast<int>(dryer.current_temp_c),
                  static_cast<int>(dryer.target_temp_c), dryer.remaining_min);

    // Update info bar visibility: show only when dryer is supported.
    // Humidity is displayed as part of the dryer bar, so it's hidden too when no dryer.
    int new_visible = (lv_subject_get_int(&dryer_supported_) != 0) ? 1 : 0;
    if (lv_subject_get_int(&dryer_info_visible_) != new_visible) {
        lv_subject_set_int(&dryer_info_visible_, new_visible);
    }
}

lv_subject_t* AmsState::get_dryer_humidity_text_subject() {
    return &dryer_humidity_text_;
}

lv_subject_t* AmsState::get_dryer_info_visible_subject() {
    return &dryer_info_visible_;
}

void AmsState::sync_clog_meter_from_info(const AmsSystemInfo& info) {
    // Priority: flowguard > encoder > afc_buffer > legacy > none
    // Source override: 0=auto (use priority), 1=encoder, 2=flowguard, 3=afc
    int mode = 0;
    int value = 0;
    int warning = 0;
    char value_text[16] = "";
    char mode_text[24] = "";
    int new_danger_pct = 75;
    int new_peak_pct = 0;
    char center_buf[16] = "";
    char left_buf[16] = "";
    char right_buf[16] = "";

    // Determine which sources are available
    bool has_flowguard = info.flowguard_info.enabled;
    bool has_encoder = info.encoder_info.enabled;
    bool has_afc = false;
    for (const auto& unit : info.units) {
        if (unit.buffer_health && unit.buffer_health->fault_detection_enabled) {
            has_afc = true;
            break;
        }
    }

    // Apply source override: skip to the forced source if available
    bool use_flowguard = has_flowguard;
    bool use_encoder = has_encoder;
    bool use_afc = has_afc;

    if (source_override_ == 1) {
        // Force encoder only
        use_flowguard = false;
        use_afc = false;
    } else if (source_override_ == 2) {
        // Force flowguard only
        use_encoder = false;
        use_afc = false;
    } else if (source_override_ == 3) {
        // Force AFC only
        use_flowguard = false;
        use_encoder = false;
    }

    if (use_flowguard) {
        // Flowguard mode: bidirectional (-100 to +100)
        mode = 2;
        value = static_cast<int>(info.flowguard_info.level * 100.0f);
        value = std::clamp(value, -100, 100);

        if (!info.flowguard_info.trigger.empty()) {
            // Active trigger — show trigger name
            snprintf(value_text, sizeof(value_text), "%s", info.flowguard_info.trigger.c_str());
            warning = 1;
        } else if (info.flowguard_info.active) {
            snprintf(value_text, sizeof(value_text), "ACTIVE");
        } else {
            snprintf(value_text, sizeof(value_text), "OFF");
        }

        if (info.encoder_info.flow_rate >= 0) {
            snprintf(mode_text, sizeof(mode_text), "Flow: %d%%", info.encoder_info.flow_rate);
        } else {
            snprintf(mode_text, sizeof(mode_text), "Flowguard");
        }

        // Enhanced clog detection widget subjects
        new_danger_pct = 80;
        float max_clog = std::abs(info.flowguard_info.max_clog);
        float max_tangle = std::abs(info.flowguard_info.max_tangle);
        new_peak_pct = static_cast<int>(std::max(max_clog, max_tangle) * 100);
        snprintf(center_buf, sizeof(center_buf), "%+d%%",
                 static_cast<int>(info.flowguard_info.level * 100));
        snprintf(left_buf, sizeof(left_buf), "TANGLE");
        snprintf(right_buf, sizeof(right_buf), "CLOG");

    } else if (use_encoder && info.encoder_info.enabled) {
        // Encoder mode: 0-100 clog percentage
        mode = 1;
        value = info.encoder_info.get_clog_pct();
        warning = info.encoder_info.is_warning() ? 1 : 0;

        if (info.encoder_info.flow_rate >= 0) {
            snprintf(value_text, sizeof(value_text), "%d%%", info.encoder_info.flow_rate);
        } else {
            snprintf(value_text, sizeof(value_text), "---");
        }

        // Detection mode text
        if (info.encoder_info.detection_mode == 2) {
            snprintf(mode_text, sizeof(mode_text), "Auto");
        } else if (info.encoder_info.detection_mode == 1) {
            snprintf(mode_text, sizeof(mode_text), "Manual");
        }

        // Enhanced clog detection widget subjects
        float det_len = info.encoder_info.detection_length;
        float headroom = info.encoder_info.headroom;
        float desired = info.encoder_info.desired_headroom;
        float min_headroom = info.encoder_info.min_headroom;
        if (det_len > 0) {
            new_danger_pct = static_cast<int>((1.0f - desired / det_len) * 100);
            new_peak_pct = static_cast<int>((1.0f - min_headroom / det_len) * 100);
            snprintf(center_buf, sizeof(center_buf), "%.1fmm", headroom);
            snprintf(left_buf, sizeof(left_buf), "%.0fmm", det_len);
        } else {
            new_danger_pct = 75;
            new_peak_pct = value;
            snprintf(center_buf, sizeof(center_buf), "---");
            snprintf(left_buf, sizeof(left_buf), "---");
        }
        snprintf(right_buf, sizeof(right_buf), "0");

    } else {
        // Check AFC buffer fault detection (buffer_health is per-unit, not per-slot)
        for (const auto& unit : info.units) {
            if (use_afc && unit.buffer_health && unit.buffer_health->fault_detection_enabled) {
                mode = 3;
                float dist = unit.buffer_health->distance_to_fault;
                float max_dist = unit.buffer_health->fault_threshold();

                if (dist < 0 || dist > max_dist) {
                    // Negative = fault timer stopped, counter stale (normal operation)
                    // Above max = just reset or not yet tracking
                    value = 0;
                    warning = 0;
                    snprintf(value_text, sizeof(value_text), "%s",
                             unit.buffer_health->state.c_str());
                } else {
                    // Actively counting down: 0=fault imminent, max_dist=safe
                    value = unit.buffer_health->danger_value();
                    warning = unit.buffer_health->is_warning() ? 1 : 0;
                    snprintf(value_text, sizeof(value_text), "%.0fmm", dist);
                }

                snprintf(mode_text, sizeof(mode_text), "%s", unit.buffer_health->state.c_str());

                // Enhanced clog detection widget subjects
                new_danger_pct = 75;
                new_peak_pct = value;
                // Safe: empty center triggers checkmark icon; tracking: show distance
                snprintf(center_buf, sizeof(center_buf), "%s",
                         (dist >= 0 && dist <= max_dist) ? value_text : "");
                snprintf(left_buf, sizeof(left_buf), "SAFE");
                snprintf(right_buf, sizeof(right_buf), "FAULT");
                break; // Use first unit with fault detection
            }
        }

        // Legacy fallback: clog_detection enabled but no encoder_info
        if (mode == 0 && info.clog_detection > 0) {
            mode = 1;
            if (info.encoder_flow_rate >= 0) {
                value = 0; // No headroom data for clog%, just show flow rate
                snprintf(value_text, sizeof(value_text), "%d%%", info.encoder_flow_rate);
            } else {
                snprintf(value_text, sizeof(value_text), "---");
            }
            if (info.clog_detection == 2) {
                snprintf(mode_text, sizeof(mode_text), "Auto");
            } else {
                snprintf(mode_text, sizeof(mode_text), "Manual");
            }
            // Legacy: use defaults (danger_pct=75, peak_pct=0, empty labels)
        }
    }

    // Apply danger threshold override if set
    if (danger_threshold_override_ > 0)
        new_danger_pct = danger_threshold_override_;

    // Update subjects only when changed
    if (lv_subject_get_int(&clog_meter_mode_) != mode) {
        lv_subject_set_int(&clog_meter_mode_, mode);
    }
    if (lv_subject_get_int(&clog_meter_value_) != value) {
        lv_subject_set_int(&clog_meter_value_, value);
    }
    if (lv_subject_get_int(&clog_meter_warning_) != warning) {
        lv_subject_set_int(&clog_meter_warning_, warning);
    }
    if (strcmp(lv_subject_get_string(&clog_meter_value_text_), value_text) != 0) {
        lv_subject_copy_string(&clog_meter_value_text_, value_text);
    }
    if (strcmp(lv_subject_get_string(&clog_meter_mode_text_), mode_text) != 0) {
        lv_subject_copy_string(&clog_meter_mode_text_, mode_text);
    }
    if (lv_subject_get_int(&clog_meter_danger_pct_) != new_danger_pct) {
        lv_subject_set_int(&clog_meter_danger_pct_, new_danger_pct);
    }
    if (lv_subject_get_int(&clog_meter_peak_pct_) != new_peak_pct) {
        lv_subject_set_int(&clog_meter_peak_pct_, new_peak_pct);
    }
    if (strcmp(lv_subject_get_string(&clog_meter_center_text_), center_buf) != 0) {
        lv_subject_copy_string(&clog_meter_center_text_, center_buf);
    }
    if (strcmp(lv_subject_get_string(&clog_meter_label_left_), left_buf) != 0) {
        lv_subject_copy_string(&clog_meter_label_left_, left_buf);
    }
    if (strcmp(lv_subject_get_string(&clog_meter_label_right_), right_buf) != 0) {
        lv_subject_copy_string(&clog_meter_label_right_, right_buf);
    }

    spdlog::trace("[AMS State] Synced clog meter - mode={}, value={}, warning={}", mode, value,
                  warning);
}

void AmsState::set_source_override(int source) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    source_override_ = source;
    spdlog::debug("[AMS State] Source override set to {}", source);
    // Re-sync to apply the override
    auto* backend = get_backend();
    if (backend) {
        auto info = backend->get_system_info();
        sync_clog_meter_from_info(info);
    }
}

void AmsState::set_danger_threshold_override(int pct) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    danger_threshold_override_ = pct;
    spdlog::debug("[AMS State] Danger threshold override set to {}", pct);
    // Re-sync to apply the override
    auto* backend = get_backend();
    if (backend) {
        auto info = backend->get_system_info();
        sync_clog_meter_from_info(info);
    }
}

void AmsState::set_action_detail(const std::string& detail) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Treat UI-managed detail like a backend-supplied operation_detail: it's
    // the highest-priority source for the displayed string. Empty clears it
    // and falls through to the action/print-state derivation.
    last_operation_detail_ = detail;
    spdlog::debug("[AMS State] Action detail set: {}", detail);
    recompute_action_detail();
}

// Translation hints for AMS status strings looked up dynamically via
// ams_action_to_string() / slot_status_to_string(). These never run — they
// exist so the translation extractor can find the literals. The enum→string
// helpers themselves stay un-translated because they're also used for logs.
// clang-format off
static void ams_status_translation_hints_() {
    // AmsAction values
    (void)lv_tr("Idle"); (void)lv_tr("Loading"); (void)lv_tr("Unloading");
    (void)lv_tr("Selecting"); (void)lv_tr("Resetting"); (void)lv_tr("Forming Tip");
    (void)lv_tr("Heating"); (void)lv_tr("Checking"); (void)lv_tr("Paused");
    (void)lv_tr("Error"); (void)lv_tr("Cutting"); (void)lv_tr("Purging");
    // SlotStatus values
    (void)lv_tr("Empty"); (void)lv_tr("Available"); (void)lv_tr("Loaded");
    (void)lv_tr("From Buffer"); (void)lv_tr("Blocked"); (void)lv_tr("Unknown");
}
// clang-format on

void AmsState::recompute_action_detail() {
    // Caller holds mutex_ (this is a private helper).
    auto action = static_cast<AmsAction>(lv_subject_get_int(&ams_action_));

    // Priority:
    //   1. Backend / UI-supplied operation_detail (non-empty)
    //   2. Action != IDLE → translated action string
    //   3. PrintJobState::PRINTING → "Printing"
    //   4. PrintJobState::PAUSED  → "Paused"
    //   5. Otherwise              → "Idle"
    //
    // Translation note (L067): the literals "Idle"/"Printing"/"Paused" and
    // the AmsAction strings are user-visible — translate at this UI binding
    // site, not in ams_action_to_string() which is also used for logs.
    const char* new_detail = "";
    if (!last_narration_label_.empty()) {
        // Live toolchange narration phase ("Brush nozzle") — finer-grained than
        // the AmsAction enum; wins until the operation ends (IDLE clears it).
        new_detail = last_narration_label_.c_str();
    } else if (!last_operation_detail_.empty()) {
        // Backend strings are intentionally NOT lv_tr()'d — the backend may
        // emit dynamic content ("Waiting for slot 2", "Heating to 230°C") that
        // isn't a fixed translation key. Pass through as-is.
        new_detail = last_operation_detail_.c_str();
    } else if (action != AmsAction::IDLE) {
        new_detail = lv_tr(ams_action_to_string(action));
    } else {
        // RAW_PRINT_STATE_OK: a label for what the printer reports. A
        // "Preparing" arm would read better during a pre-print block, but there
        // is no such translation key yet and this is the lowest-priority
        // fallback in the chain - the AmsAction string wins whenever the AMS is
        // doing anything at all.
        auto print_state = static_cast<PrintJobState>(
            lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
        switch (print_state) {
        case PrintJobState::PRINTING:
            new_detail = lv_tr("Printing");
            break;
        case PrintJobState::PAUSED:
            new_detail = lv_tr("Paused");
            break;
        default:
            new_detail = lv_tr("Idle");
            break;
        }
    }

    if (strcmp(lv_subject_get_string(&ams_action_detail_), new_detail) != 0) {
        lv_subject_copy_string(&ams_action_detail_, new_detail);
    }
}

void AmsState::set_action(AmsAction action) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int val = static_cast<int>(action);
    if (lv_subject_get_int(&ams_action_) != val) {
        lv_subject_set_int(&ams_action_, val);
        spdlog::debug("[AMS State] Action set: {}", ams_action_to_string(action));
        // Operation ended: clear the narration label + phase index BEFORE the
        // recompute so the cleared state is reflected in the detail string. The
        // next swap then restarts from a clean step bar. set_action writes
        // subjects directly (main-thread contract), so write toolchange_step_
        // directly too.
        if (action == AmsAction::IDLE) {
            last_narration_label_.clear();
            narration_phase_high_water_ = -1;
            lv_subject_set_int(&toolchange_step_, -1);
        }
        // Action change must propagate to the displayed detail string (e.g.
        // LOADING → IDLE while still printing should flip "Loading" → "Printing").
        recompute_action_detail();
    }
}

void AmsState::set_active_step_operation(StepOperationType op) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const StepOperationType prev = active_step_operation_.exchange(op, std::memory_order_relaxed);
    if (prev != op) {
        // Each operation kind has its own phase template, so an index carried
        // over from the previous one is not comparable with the new one's.
        // (The sidebar re-derives the operation whenever it (re)builds the step
        // bar, so this is also the mid-operation UNLOAD -> LOAD_SWAP upgrade.)
        narration_phase_high_water_ = -1;
    }
}

void AmsState::set_narration_phase(int index, const std::string& label) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Firmware narration is not monotonic. AFC runs its wipe macro twice per
    // toolchange, once before and once after the kick (AFC.py
    // do_poop_kick_wipe(), v1.2.0:1390-1413; inline in TOOL_LOAD at
    // v1.1.0:1417-1440), and both emit at the shipped default verbosity. The
    // second one resolves to the same "brush" phase as the first, which sits
    // BEFORE "kick" in the template — publishing it verbatim rewinds the bar.
    //
    // Latch the highest index instead. Reset points are the three places an
    // index stops being comparable with its predecessor:
    //   - index < 0        explicit clear (operation over / test baseline)
    //   - index == 0       the template's first phase narrated again, i.e. the
    //                      operation restarted from the top (an AFC retry after
    //                      a resumed error re-runs TOOL_LOAD from the heat)
    //   - set_action(IDLE) operation ended
    //   - set_active_step_operation() the template itself changed
    //
    // A retry that resumes PAST the first phase (nozzle already hot, so no heat
    // narration) is deliberately not detected: the bar then stays parked at its
    // high-water mark until the operation ends. A stalled bar is a far cheaper
    // wrong than one that ping-pongs, and every path out of the operation
    // clears the latch.
    if (index < 0) {
        narration_phase_high_water_ = -1;
    } else if (index == 0) {
        narration_phase_high_water_ = 0;
    } else if (index < narration_phase_high_water_) {
        spdlog::trace("[AMS State] Narration phase {} ('{}') ignored — already past step {}", index,
                      label, narration_phase_high_water_);
        return;
    } else {
        narration_phase_high_water_ = index;
    }

    lv_subject_set_int(&toolchange_step_, index);
    last_narration_label_ = label;
    recompute_action_detail();
}

void AmsState::set_pending_target_slot(int slot) {
    async_lifetime_.defer("AmsState::set_pending_target_slot", [this, slot]() {
        if (lv_subject_get_int(&pending_target_slot_) != slot) {
            lv_subject_set_int(&pending_target_slot_, slot);
        }
    });
}

void AmsState::set_active_tool_port_present(bool present) {
    // Marshal to the main thread — callable from the backend's WS status handler.
    async_lifetime_.defer("AmsState::set_active_tool_port_present", [this, present]() {
        int v = present ? 1 : 0;
        if (lv_subject_get_int(&active_tool_port_present_) != v) {
            lv_subject_set_int(&active_tool_port_present_, v);
        }
    });
}

bool AmsState::consume_post_unload_runout_grace() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!post_unload_runout_grace_) {
        return false;
    }
    post_unload_runout_grace_ = false;
    const auto age = std::chrono::steady_clock::now() - post_unload_runout_grace_at_;
    if (age >= POST_UNLOAD_RUNOUT_GRACE) {
        spdlog::debug("[AmsState] Post-unload runout grace expired unused after {}s",
                      std::chrono::duration_cast<std::chrono::seconds>(age).count());
        return false;
    }
    return true;
}

bool AmsState::post_unload_runout_grace_armed() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!post_unload_runout_grace_) {
        return false;
    }
    // Deliberately does NOT clear on expiry: only the consumer spends the shot,
    // so a peek that also disarmed would be a second consumer by another name.
    return (std::chrono::steady_clock::now() - post_unload_runout_grace_at_) <
           POST_UNLOAD_RUNOUT_GRACE;
}

bool AmsState::is_filament_operation_active() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto action = static_cast<AmsAction>(lv_subject_get_int(&ams_action_));
    // Only suppress during states that actively move filament past sensors.
    // Heating, tip forming, cutting, and purging are stationary — a sensor
    // change in those states would indicate a real problem.
    switch (action) {
    case AmsAction::LOADING:
    case AmsAction::UNLOADING:
    case AmsAction::SELECTING:
        return true;
    default:
        return false;
    }
}

void AmsState::mark_slot_unloaded(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last_unload_time_[slot_index] = std::chrono::steady_clock::now();
    spdlog::debug("[AmsState] marked slot {} as recently unloaded (runout grace started)",
                  slot_index);
}

bool AmsState::was_slot_recently_unloaded(int slot_index) const {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto t = last_unload_time_[slot_index];
    if (t.time_since_epoch().count() == 0) {
        return false; // never unloaded
    }
    return (std::chrono::steady_clock::now() - t) < RECENT_UNLOAD_GRACE;
}

void AmsState::set_current_loaded_defaults() {
    if (strcmp(lv_subject_get_string(&current_material_text_), "---") != 0) {
        lv_subject_copy_string(&current_material_text_, "---");
    }
    const char* default_slot = lv_tr("Currently Loaded");
    if (strcmp(lv_subject_get_string(&current_slot_text_), default_slot) != 0) {
        lv_subject_copy_string(&current_slot_text_, default_slot);
    }
    if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
        lv_subject_copy_string(&current_weight_text_, "");
    }
    if (lv_subject_get_int(&current_has_weight_) != 0) {
        lv_subject_set_int(&current_has_weight_, 0);
    }
    if (lv_subject_get_int(&current_color_) != 0x505050) {
        lv_subject_set_int(&current_color_, 0x505050);
    }
}

void AmsState::sync_current_loaded_from_backend() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (backends_.empty()) {
        set_current_loaded_defaults();
        return;
    }

    auto* backend = get_backend(0);
    if (backend) {
        sync_current_loaded_from_backend(backend->get_system_info());
    }
}

void AmsState::sync_current_loaded_from_backend(const AmsSystemInfo& primary_info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (backends_.empty()) {
        set_current_loaded_defaults();
        return;
    }

    // Search ALL backends to find the one with filament loaded.
    // In multi-backend setups (e.g., AMS_1 + AMS_2), only one backend
    // will have filament actively loaded at a time.
    // Use pre-fetched primary_info for backend 0 to avoid redundant get_system_info().
    AmsBackend* loaded_backend = nullptr;
    int slot_index = -1;
    bool filament_loaded = false;

    for (size_t idx = 0; idx < backends_.size(); ++idx) {
        auto& b = backends_[idx];
        if (!b)
            continue;
        AmsSystemInfo secondary_info;
        if (idx != 0)
            secondary_info = b->get_system_info();
        const AmsSystemInfo& info = (idx == 0) ? primary_info : secondary_info;
        if (info.filament_loaded) {
            loaded_backend = b.get();
            slot_index = info.current_slot;
            filament_loaded = true;
            break;
        }
        // Also check bypass on each backend
        if (info.current_slot == -2 && b->is_bypass_active()) {
            loaded_backend = b.get();
            slot_index = -2;
            break;
        }
    }

    // Fallback to primary backend for bypass check if no loaded backend found
    if (!loaded_backend) {
        loaded_backend = backends_[0].get();
        if (loaded_backend) {
            slot_index = primary_info.current_slot;
            filament_loaded = primary_info.filament_loaded;
        }
    }

    if (!loaded_backend) {
        set_current_loaded_defaults();
        lv_subject_set_int(&current_color_, 0x505050);
        return;
    }

    // Check for bypass mode (slot_index == -2)
    if (slot_index == -2 && loaded_backend->is_bypass_active()) {
        const char* bypass_text = lv_tr("Current: Bypass");
        if (strcmp(lv_subject_get_string(&current_slot_text_), bypass_text) != 0) {
            lv_subject_copy_string(&current_slot_text_, bypass_text);
        }

        // Show actual spool info if external spool is assigned
        auto ext_spool = get_external_spool_info();
        if (ext_spool.has_value()) {
            const auto& ext = ext_spool.value();
            int ext_color = static_cast<int>(ext.color_rgb);
            if (lv_subject_get_int(&current_color_) != ext_color) {
                lv_subject_set_int(&current_color_, ext_color);
            }

            // Build label from spool info — same resolver as the loaded-slot
            // card below. Precedence and brand/material dedup live in
            // helix::resolve_filament_label(); the last resort stays the
            // translated "External" this card has always shown.
            auto ext_identity = SpoolmanManager::find_identity(ext.spoolman_id);
            std::string label = helix::resolve_filament_label(
                ext, ext_identity ? &*ext_identity : nullptr,
                helix::get_color_name_from_hex(ext.color_rgb), lv_tr("External"));
            if (strcmp(lv_subject_get_string(&current_material_text_), label.c_str()) != 0) {
                lv_subject_copy_string(&current_material_text_, label.c_str());
            }

            if (ext.total_weight_g > 0.0f && ext.remaining_weight_g >= 0.0f) {
                char wt[32];
                snprintf(wt, sizeof(wt), "%.0fg", ext.remaining_weight_g);
                if (strcmp(lv_subject_get_string(&current_weight_text_), wt) != 0) {
                    lv_subject_copy_string(&current_weight_text_, wt);
                }
                if (lv_subject_get_int(&current_has_weight_) != 1) {
                    lv_subject_set_int(&current_has_weight_, 1);
                }
            } else {
                if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
                    lv_subject_copy_string(&current_weight_text_, "");
                }
                if (lv_subject_get_int(&current_has_weight_) != 0) {
                    lv_subject_set_int(&current_has_weight_, 0);
                }
            }
        } else {
            const char* ext_text = lv_tr("External");
            if (strcmp(lv_subject_get_string(&current_material_text_), ext_text) != 0) {
                lv_subject_copy_string(&current_material_text_, ext_text);
            }
            if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
                lv_subject_copy_string(&current_weight_text_, "");
            }
            if (lv_subject_get_int(&current_has_weight_) != 0) {
                lv_subject_set_int(&current_has_weight_, 0);
            }
            if (lv_subject_get_int(&current_color_) != 0x888888) {
                lv_subject_set_int(&current_color_, 0x888888);
            }
        }
    } else if (slot_index >= 0 && filament_loaded) {
        // Filament is loaded - show slot info from the backend that has it loaded
        spdlog::debug("[AmsState] sync_current_loaded: slot={}, filament_loaded=true", slot_index);
        SlotInfo slot_info = loaded_backend->get_slot_info(slot_index);

        // Sync Spoolman active spool when slot with spoolman_id is loaded.
        // Skip when the backend manages active spool itself (e.g., AFC calls
        // spoolman_set_active_spool on tool load/unload natively).
        if (api_ && slot_info.spoolman_id > 0 &&
            slot_info.spoolman_id != last_synced_spoolman_id_ &&
            !loaded_backend->manages_active_spool()) {
            last_synced_spoolman_id_ = slot_info.spoolman_id;
            spdlog::info("[AMS State] Setting active Spoolman spool to {} (slot {})",
                         slot_info.spoolman_id, slot_index);
            api_->spoolman().set_active_spool(
                slot_info.spoolman_id, []() {}, [](const MoonrakerError&) {});
        }

        // Set color
        int slot_color = static_cast<int>(slot_info.color_rgb);
        if (lv_subject_get_int(&current_color_) != slot_color) {
            lv_subject_set_int(&current_color_, slot_color);
        }

        // Build the material label. The slot's own name/brand/material win, the
        // cached Spoolman identity fills the gaps (it is the only source of a
        // brand for AFC), and the algorithmic colour name is the last naming
        // layer — which is the bug this replaced: AFC never populates
        // color_name, so the old guard always fell through to "Light Pink PLA"
        // while the real name sat unread in slot_info.spool_name.
        {
            auto identity = SpoolmanManager::find_identity(slot_info.spoolman_id);
            std::string label =
                helix::resolve_filament_label(slot_info, identity ? &*identity : nullptr,
                                              helix::get_color_name_from_hex(slot_info.color_rgb));
            if (strcmp(lv_subject_get_string(&current_material_text_), label.c_str()) != 0) {
                lv_subject_copy_string(&current_material_text_, label.c_str());
            }
        }

        // Set slot label with unit name
        {
            AmsSystemInfo sys = loaded_backend->get_system_info();

            char tmp[64];
            if (is_tool_changer(sys.type) && sys.units.empty()) {
                // Pure tool changer with no AMS units — show tool index (0-based)
                snprintf(tmp, sizeof(tmp), lv_tr("Current: Tool %d"), slot_index);
            } else {
                std::string unit_display;
                int display_slot = slot_index + 1; // 1-based global slot number
                for (const auto& unit : sys.units) {
                    if (slot_index >= unit.first_slot_global_index &&
                        slot_index < unit.first_slot_global_index + unit.slot_count) {
                        // Prefer display_name, fall back to name, replace _ with spaces
                        unit_display = !unit.display_name.empty() ? unit.display_name : unit.name;
                        std::replace(unit_display.begin(), unit_display.end(), '_', ' ');
                        break;
                    }
                }
                if (!unit_display.empty() && sys.units.size() > 1) {
                    // Multi-unit: show unit name + slot number on one line
                    snprintf(tmp, sizeof(tmp), lv_tr("Current: %s · Slot %d"), unit_display.c_str(),
                             display_slot);
                } else {
                    snprintf(tmp, sizeof(tmp), lv_tr("Current: Slot %d"), display_slot);
                }
            }
            if (strcmp(lv_subject_get_string(&current_slot_text_), tmp) != 0) {
                lv_subject_copy_string(&current_slot_text_, tmp);
            }
        }

        // Show remaining weight if available (from Spoolman or backend)
        if (slot_info.total_weight_g > 0.0f && slot_info.remaining_weight_g >= 0.0f) {
            char wt[32];
            snprintf(wt, sizeof(wt), "%.0fg", slot_info.remaining_weight_g);
            if (strcmp(lv_subject_get_string(&current_weight_text_), wt) != 0) {
                lv_subject_copy_string(&current_weight_text_, wt);
            }
            if (lv_subject_get_int(&current_has_weight_) != 1) {
                lv_subject_set_int(&current_has_weight_, 1);
            }
        } else {
            if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
                lv_subject_copy_string(&current_weight_text_, "");
            }
            if (lv_subject_get_int(&current_has_weight_) != 0) {
                lv_subject_set_int(&current_has_weight_, 0);
            }
        }
    } else {
        // No filament loaded - show empty state
        set_current_loaded_defaults();
    }

    spdlog::trace("[AMS State] Synced current loaded - slot={}, has_weight={}", slot_index,
                  lv_subject_get_int(&current_has_weight_));
}

// ============================================================================
// Dryer Modal Editing Methods
// ============================================================================

void AmsState::adjust_modal_temp(int delta_c) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Get limits from backend if available, fallback to constants
    float min_temp = static_cast<float>(MIN_DRYER_TEMP_C);
    float max_temp = static_cast<float>(MAX_DRYER_TEMP_C);
    auto* backend = get_backend(0);
    if (backend) {
        DryerInfo dryer = backend->get_dryer_info(dryer_mirror_unit_);
        min_temp = dryer.min_temp_c;
        max_temp = dryer.max_temp_c;
    }

    int cur = lv_subject_get_int(&modal_target_temp_);
    int new_temp = cur + delta_c;
    new_temp = std::max(static_cast<int>(min_temp), std::min(new_temp, static_cast<int>(max_temp)));
    lv_subject_set_int(&modal_target_temp_, new_temp);

    spdlog::debug("[AMS State] Modal temp adjusted to {}°C", new_temp);
}

void AmsState::adjust_modal_duration(int delta_min) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Get max duration from backend if available, fallback to constant
    int max_duration = MAX_DRYER_DURATION_MIN;
    auto* backend = get_backend(0);
    if (backend) {
        DryerInfo dryer = backend->get_dryer_info(dryer_mirror_unit_);
        max_duration = dryer.max_duration_min;
    }

    int cur = lv_subject_get_int(&modal_duration_min_);
    int new_duration = cur + delta_min;
    new_duration = std::max(MIN_DRYER_DURATION_MIN, std::min(new_duration, max_duration));
    lv_subject_set_int(&modal_duration_min_, new_duration);

    spdlog::debug("[AMS State] Modal duration adjusted to {} min", new_duration);
}

void AmsState::set_modal_preset(int temp_c, int duration_min) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    lv_subject_set_int(&modal_target_temp_, temp_c);
    lv_subject_set_int(&modal_duration_min_, duration_min);
    spdlog::debug("[AMS State] Modal preset set: {}°C for {} min", temp_c, duration_min);
}

// ============================================================================
// External Spool (delegates to SettingsManager for persistence)
// ============================================================================

std::optional<SlotInfo> AmsState::get_external_spool_info() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // In-memory override takes priority when set (e.g. live tracker updates).
    if (in_memory_external_spool_.has_value()) {
        return in_memory_external_spool_;
    }
    return helix::SettingsManager::instance().get_external_spool_info();
}

void AmsState::set_external_spool_info_in_memory(const SlotInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    in_memory_external_spool_ = info;
    notify_external_spool_changed(info);
}

void AmsState::set_external_spool_info(const SlotInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    in_memory_external_spool_.reset(); // Persistent write wins; let SettingsManager be the source.
    helix::SettingsManager::instance().set_external_spool_info(info);
    notify_external_spool_changed(info);
}

void AmsState::notify_external_spool_changed(const SlotInfo& info) {
    // Always notify observers — spool data (weight, name, etc.) may change
    // even when color stays the same
    int new_color = static_cast<int>(info.color_rgb);
    int old_color = lv_subject_get_int(&external_spool_color_);
    if (old_color == new_color) {
        // Force notification by toggling value
        lv_subject_set_int(&external_spool_color_, new_color ^ 1);
    }
    lv_subject_set_int(&external_spool_color_, new_color);
    // Material string reflector — copy_string only notifies on change, and
    // color observers re-read full spool info anyway, so no force-fire needed.
    lv_subject_copy_string(&external_spool_material_, info.material.c_str());
}

void AmsState::clear_external_spool_info() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    in_memory_external_spool_.reset();
    helix::SettingsManager::instance().clear_external_spool_info();
    // Force notification even when color was already 0 (e.g. previous spool was
    // black, RGB=0x000000) — observers read full spool info, not just the color.
    if (lv_subject_get_int(&external_spool_color_) == 0) {
        lv_subject_set_int(&external_spool_color_, 1);
    }
    lv_subject_set_int(&external_spool_color_, 0);
    lv_subject_copy_string(&external_spool_material_, "");
}

// ============================================================================
// Slot edit commit (single authority for spool assignment changes)
// ============================================================================

AmsError AmsState::commit_slot_edit(int slot_index, const SlotInfo& original,
                                    const SlotInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    AmsBackend* backend = get_backend();
    if (!backend) {
        return AmsError(AmsResult::NO_AMS_DETECTED, "no AMS backend",
                        lv_tr("Multi-Filament System not available"));
    }

    // S1 — server-side active spool (fire-and-forget, warn on failure)
    if (api_) {
        if (info.spoolman_id > 0) {
            api_->spoolman().set_active_spool(
                info.spoolman_id, []() {},
                [](const MoonrakerError& err) {
                    spdlog::warn("[AmsState] Failed to set active spool: {}", err.message);
                });
        } else if (original.spoolman_id > 0) {
            api_->spoolman().set_active_spool(
                0, []() {},
                [](const MoonrakerError& err) {
                    spdlog::warn("[AmsState] Failed to clear active spool: {}", err.message);
                });
        }
    }

    // S6 — stale identity otherwise survives until a server 404
    if (original.spoolman_id > 0 && original.spoolman_id != info.spoolman_id) {
        SpoolmanManager::invalidate_identity(original.spoolman_id);
    }

    // S3 — backend slot info + firmware gcode
    AmsError err = backend->set_slot_info(slot_index, info);
    if (!err.success()) {
        return err;
    }

    // S4 + S7
    sync_from_backend();
    return err;
}

void AmsState::apply_external_spool_store(const SlotInfo& info) {
    // S5 + S7 — same emptiness predicate as the FilamentPanel completion arm
    if (info.spoolman_id > 0 || !info.material.empty()) {
        set_external_spool_info(info);
    } else {
        clear_external_spool_info();
    }

    // Keep the slicer-sync lane (OrcaSlicer lane_data mirror) fresh on every
    // identity change — same capability dispatch as the bypass-engage hook.
    for (auto& backend : backends_) {
        if (backend) {
            backend->publish_external_spool_lane(&info);
        }
    }
}

void AmsState::invalidate_stale_external_identity(const SlotInfo& info) {
    // S6 — stale identity otherwise survives until a server 404
    const int previous_id = get_external_spool_info().value_or(SlotInfo{}).spoolman_id;
    if (previous_id > 0 && previous_id != info.spoolman_id) {
        SpoolmanManager::invalidate_identity(previous_id);
    }
}

void AmsState::commit_external_spool_edit(const SlotInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // S1 — match the server active spool to what we are committing
    if (api_) {
        if (info.spoolman_id > 0) {
            api_->spoolman().set_active_spool(
                info.spoolman_id, []() {},
                [](const MoonrakerError& err) {
                    spdlog::warn("[AmsState] Failed to set active spool: {}", err.message);
                });
        } else if (get_external_spool_info().value_or(SlotInfo{}).spoolman_id > 0) {
            // Committing a manual entry (id=0, material set) over a linked
            // spool intentionally clears the server link — the UI no longer
            // shows that spool as in use, so the server must not either.
            api_->spoolman().set_active_spool(
                0, []() {},
                [](const MoonrakerError& err) {
                    spdlog::warn("[AmsState] Failed to clear active spool: {}", err.message);
                });
        }
    }

    invalidate_stale_external_identity(info);

    // S5 + S7
    apply_external_spool_store(info);
}

void AmsState::commit_external_spool_edit(const SlotInfo& info, std::function<void()> on_committed,
                                          std::function<void(const MoonrakerError& err)> on_error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    invalidate_stale_external_identity(info);

    if (api_ && info.spoolman_id > 0) {
        // Server-first: the store subset waits for the server round-trip. The
        // API callbacks fire on a background thread, so both the store write
        // and the caller's completion are marshalled to the main thread.
        api_->spoolman().set_active_spool(
            info.spoolman_id,
            [info, on_committed = std::move(on_committed)]() {
                helix::ui::queue_update("AmsState::commit_external_spool_edit",
                                        [info, on_committed]() {
                                            if (s_shutdown_flag.load(std::memory_order_acquire)) {
                                                return;
                                            }
                                            AmsState::instance().apply_external_spool_store(info);
                                            if (on_committed) {
                                                on_committed();
                                            }
                                        });
            },
            [on_error = std::move(on_error)](const MoonrakerError& err) {
                helix::ui::queue_update("AmsState::commit_external_spool_edit", [on_error, err]() {
                    if (on_error) {
                        on_error(err);
                    }
                });
            });
        return;
    }

    // Manual entry or clear: no server identity gates the store write. The
    // clear arm (replacing a linked spool with an empty record) still tells
    // the server, fire-and-forget, exactly like the sync commit.
    if (api_ && info.spoolman_id == 0 &&
        get_external_spool_info().value_or(SlotInfo{}).spoolman_id > 0) {
        api_->spoolman().set_active_spool(
            0, []() {},
            [](const MoonrakerError& err) {
                spdlog::warn("[AmsState] Failed to clear active spool: {}", err.message);
            });
    }

    apply_external_spool_store(info);
    if (on_committed) {
        on_committed();
    }
}
