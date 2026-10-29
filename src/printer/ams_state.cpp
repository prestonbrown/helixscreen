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
#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "ams_backend_registry.h"
#include "ams_bypass_policy.h"
#include "ams_lane_state.h"
#include "ams_runout_grace.h"
#include "ams_standing_fault.h"
#include "ams_state_internal.h"
#include "ams_tool_topology.h"
#include "app_globals.h"
#include "data_root_resolver.h"
#include "display_numbering.h"
#include "filament_accounting_controller.h"
#include "filament_database.h"
#include "filament_display_name.h"
#include "filament_sensor_manager.h"
#include "helix_lvgl_anomaly.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lane_source_store.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_stop_reason.h"
#include "printer_discovery.h"
#include "printer_event_journal.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"
#include "text_io.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

namespace helix {

static_assert(RunoutGrace::kMaxSlots == AmsState::kMaxSlots, "one unload stamp per slot subject");

namespace ams_state_detail {

namespace {
// Shutdown flag to prevent async callbacks from accessing destroyed singleton
std::atomic<bool> s_shutdown_flag{false};
} // namespace

void assert_main_thread(const char* caller) {
    if (!ui::is_main_thread()) {
        report_off_main(caller);
    }
}

void report_off_main(const char* caller) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "[AMS State] %s called off the main thread", caller);
    if (ui::strict_ui_checks()) {
        ui::report_ui_contract_breach(msg);
    }
    // Once per process: an off-main caller usually repeats on every frame.
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    spdlog::error("{}", msg);
    helix_lvgl_anomaly("ams_off_main", msg);
}

bool shutting_down() {
    return s_shutdown_flag.load(std::memory_order_acquire);
}

} // namespace ams_state_detail

using ams_state_detail::assert_main_thread;
using ams_state_detail::set_int_if_changed;
using ams_state_detail::slot_error_state;

namespace {

struct AsyncSyncData {
    int backend_index;
    bool full_sync;
    int slot_index; // Only used if full_sync == false
};

} // namespace

AmsState& AmsState::instance() {
    // ~9.5KB singleton: relocate to PSRAM on ESP to reclaim internal DRAM (it's
    // app-state, first touched at runtime, never DMA/ISR). No-op elsewhere.
    static HELIX_PSRAM_BSS AmsState instance;
    return instance;
}

LogoArt AmsState::get_logo(const std::string& type_name) {
    // Normalize to lowercase for matching
    std::string lower_name = helix::text_io::to_lower(type_name);

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
    // Every logo is a 64x64 PNG. A mono mark is white on transparent and the UI tints it
    // to the text color; a color mark is drawn as is.
    struct Entry {
        std::string path;
        bool color;
    };
    static const std::unordered_map<std::string, Entry> logo_map = {
        // AFC (Armored Turtle) - has its own logo
        {"afc", {asset_component_uri("assets/images/ams/afc_64.png"), false}},
        {"box turtle", {asset_component_uri("assets/images/ams/box_turtle_64.png"), false}},
        {"box_turtle", {asset_component_uri("assets/images/ams/box_turtle_64.png"), false}},
        {"boxturtle", {asset_component_uri("assets/images/ams/box_turtle_64.png"), false}},

        // Happy Hare - generic firmware, has its own logo
        {"happy hare", {asset_component_uri("assets/images/ams/happy_hare_64.png"), false}},
        {"happy_hare", {asset_component_uri("assets/images/ams/happy_hare_64.png"), false}},
        {"happyhare", {asset_component_uri("assets/images/ams/happy_hare_64.png"), false}},

        // Specific hardware types (when detected or configured)
        {"ercf", {asset_component_uri("assets/images/ams/ercf_64.png"), false}},
        {"3ms", {asset_component_uri("assets/images/ams/3ms_64.png"), false}},
        {"tradrack", {asset_component_uri("assets/images/ams/tradrack_64.png"), false}},
        {"mmx", {asset_component_uri("assets/images/ams/mmx_64.png"), false}},
        {"night owl", {asset_component_uri("assets/images/ams/night_owl_64.png"), false}},
        {"night_owl", {asset_component_uri("assets/images/ams/night_owl_64.png"), false}},
        {"nightowl", {asset_component_uri("assets/images/ams/night_owl_64.png"), false}},
        {"quattro box", {asset_component_uri("assets/images/ams/quattro_box_64.png"), false}},
        {"quattro_box", {asset_component_uri("assets/images/ams/quattro_box_64.png"), false}},
        {"quattrobox", {asset_component_uri("assets/images/ams/quattro_box_64.png"), false}},

        // QIDI Box - has its own logo (prestonbrown/helixscreen#1514)
        {"qidi box", {asset_component_uri("assets/images/ams/qidi_box_64.png"), false}},
        {"qidi_box", {asset_component_uri("assets/images/ams/qidi_box_64.png"), false}},
        {"qidibox", {asset_component_uri("assets/images/ams/qidi_box_64.png"), false}},
        {"btt vivid", {asset_component_uri("assets/images/ams/btt_vivid_64.png"), false}},
        {"btt_vivid", {asset_component_uri("assets/images/ams/btt_vivid_64.png"), false}},
        {"bttvivid", {asset_component_uri("assets/images/ams/btt_vivid_64.png"), false}},
        {"vivid", {asset_component_uri("assets/images/ams/btt_vivid_64.png"), false}},
        {"kms", {asset_component_uri("assets/images/ams/kms_64.png"), false}},
        // OpenAMS: a full-color mark, drawn untinted.
        {"open ams", {asset_component_uri("assets/images/ams/openams_64.png"), true}},
        {"open_ams", {asset_component_uri("assets/images/ams/openams_64.png"), true}},
        {"openams", {asset_component_uri("assets/images/ams/openams_64.png"), true}},

        // AFC unit types with no artwork of their own (Claymore is new in AFC
        // v1.2.0; the rest predate it). They fall back to the AFC mark:
        // wrong-but-related beats a blank slot, and the alternative is
        // silently rendering nothing.
        {"htlf", {asset_component_uri("assets/images/ams/afc_64.png"), false}},
        {"claymore", {asset_component_uri("assets/images/ams/afc_64.png"), false}},
        {"emu", {asset_component_uri("assets/images/ams/afc_64.png"), false}},
    };

    auto it = logo_map.find(lower_name);
    if (it != logo_map.end()) {
        return {it->second.path.c_str(), it->second.color};
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
            return {it->second.path.c_str(), it->second.color};
        }
    }
    return {};
}

LogoArt AmsState::get_unit_logo(const AmsUnit& unit, const AmsSystemInfo& info) {
    LogoArt art = get_logo(unit.name);
    if (!art.path || !art.path[0]) {
        art = get_logo(info.type_name);
    }
    return art;
}

const char* AmsState::get_logo_path(const std::string& type_name) {
    return get_logo(type_name).path;
}

AmsState::AmsState() {
    std::memset(action_detail_buf_, 0, sizeof(action_detail_buf_));
    std::memset(system_name_buf_, 0, sizeof(system_name_buf_));
    std::memset(system_logo_buf_, 0, sizeof(system_logo_buf_));
    std::memset(current_material_text_buf_, 0, sizeof(current_material_text_buf_));
    std::memset(current_slot_text_buf_, 0, sizeof(current_slot_text_buf_));
    std::memset(current_weight_text_buf_, 0, sizeof(current_weight_text_buf_));
    std::memset(clog_meter_mode_text_buf_, 0, sizeof(clog_meter_mode_text_buf_));
    std::memset(clog_meter_center_text_buf_, 0, sizeof(clog_meter_center_text_buf_));
    std::memset(clog_meter_label_left_buf_, 0, sizeof(clog_meter_label_left_buf_));
    std::memset(clog_meter_label_right_buf_, 0, sizeof(clog_meter_label_right_buf_));
    std::memset(clog_meter_note_text_buf_, 0, sizeof(clog_meter_note_text_buf_));
}

AmsState::~AmsState() {
    // Signal shutdown to prevent async callbacks from accessing this instance
    ams_state_detail::s_shutdown_flag.store(true, std::memory_order_release);

    // During static destruction, the MoonrakerClient may already be destroyed.
    // Release subscriptions without unsubscribing to avoid calling into dead objects.
    // SubscriptionGuard::release() abandons the subscription — no mutex access needed.
    registry_.release_all();
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

    assert_main_thread();
    if (registry_.count() > 0) {
        spdlog::debug("[AMS State] Backends already initialized, skipping");
        return;
    }

    for (const auto& system : systems) {
        spdlog::info("[AMS State] Creating backend for: {} ({})", system.name,
                     ams_type_to_string(system.type));

        auto backend = AmsBackend::create(system.type, api, client, &hardware);
        if (!backend) {
            spdlog::warn("[AMS State] Failed to create {} backend", system.name);
            continue;
        }

        backend->set_discovery(hardware);

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
    assert_main_thread();

    clear_backends();

    if (backend) {
        auto type = backend->get_type();
        add_backend(std::move(backend));
        spdlog::debug("[AMS State] Backend set (type={})", ams_type_to_string(type));
    }
}

int AmsState::add_backend(std::unique_ptr<AmsBackend> backend) {
    assert_main_thread();

    // Stamped before registration: only AmsState may set a backend's index,
    // and add() is main-thread only, so count() is the index add() assigns.
    if (backend) {
        backend->set_backend_index(registry_.count());
    }
    const int index = registry_.add(
        std::move(backend), [this](int i, const std::string& event, const std::string& data) {
            on_backend_event(i, event, data);
        });
    invalidate_system_info();

    // Per-backend slot subjects for secondary backends
    if (index > 0) {
        if (auto* b = registry_.get(index)) {
            BackendSlotSubjects subs;
            subs.init(b->get_system_info().total_slots);
            secondary_slot_subjects_.push_back(std::move(subs));
        }
    }

    lv_subject_set_int(&backend_count_, registry_.count());
    return index;
}

AmsBackend* AmsState::get_backend(int index) const {
    return registry_.get(index);
}

std::optional<AmsType> AmsState::primary_type() const {
    return registry_.primary_type();
}

double AmsState::printable_y_shrink_mm() const {
    const AmsBackend* backend = get_backend();
    return backend != nullptr ? backend->printable_y_shrink_mm() : 0.0;
}

std::optional<BackendTraits> AmsState::primary_traits() const {
    return registry_.primary_traits();
}

int AmsState::backend_count() const {
    return registry_.count();
}

bool AmsState::any_filament_batch_in_flight() const {
    return registry_.any_filament_batch_in_flight();
}

void AmsState::before_disconnect() {
    assert_main_thread();
    for (const auto& backend : registry_.backends()) {
        if (backend) {
            backend->before_disconnect();
        }
    }
}

void AmsState::clear_backends() {
    assert_main_thread();

    registry_.clear();
    invalidate_system_info();
    system_info_snapshots_.clear();

    // Registration stamps indices from 0 again, so the next set of backends
    // takes these blocks of lane ids. A declaration left behind would be
    // handed to whatever hardware lands on the same block next.
    helix::ams::reset_lane_sources();

    // The runout edge state describes a specific backend's flag history. A new
    // backend's first sample must re-seed rather than read as a transition.
    prev_backend_runout_ = false;
    runout_edge_armed_ = false;
    runout_prev_paused_ = false;
    runout_level_seeded_ = false;
    // Same reasoning for the unload grace: it was armed for a removal on the
    // backend going away, and nothing the next one reports can be that. The
    // per-slot stamps index the departing backend's slots; kept, they would
    // suppress a real runout on the same index of the next backend.
    runout_grace_.reset();
    // What each lane's tag last named belongs to the departing backend's lanes.
    tag_link_tracker_.clear();
    // A hold describes an operation on the departing backend.
    optimistic_action_until_.reset();
    // Every trace describes the departing backend's buffers.
    buffer_traces_.clear();
    if (initialized_) {
        publish_buffer_reading(BufferReading{});
    }

    // Drop AMS-derived tool topology so the UI doesn't show stale tool pills
    // between backend disappearance and the next reconnect's init_tools().
    helix::ToolState::instance().clear_ams_topology();
    helix::ToolState::instance().clear_backend_active_tool();

    // Clean up secondary slot subjects
    for (auto& subs : secondary_slot_subjects_) {
        subs.deinit();
    }
    secondary_slot_subjects_.clear();

    reset_backend_subjects();
}

void AmsState::reset_backend_subjects() {
    // Every subject a backend sync writes goes back to its init_subjects() value. A live
    // printer switch keeps every panel, so one value left behind shows the departed
    // printer's filament system on the next one.
    lv_subject_set_int(&backend_count_, 0);
    lv_subject_set_int(&active_backend_, 0);
    fault_cause_.clear();

    // System, action and current-tool state: the sync path with an empty system.
    sync_system_subjects(AmsSystemInfo{});
    sync_shared_heater(nullptr, AmsSystemInfo{});
    // The action edge above belongs to no operation on the next printer.
    runout_grace_.reset();
    lv_subject_copy_string(&ams_system_name_, "");
    system_logo_buf_[0] = '\0';
    lv_subject_set_pointer(&ams_system_logo_, nullptr);
    lv_subject_set_int(&ams_system_has_logo_, 0);
    lv_subject_set_int(&ams_system_logo_color_, 0);
    lv_subject_copy_string(&ams_action_detail_, "");
    last_operation_detail_.clear();
    lv_subject_set_int(&toolchange_step_, -1);

    lv_subject_set_int(&filament_loaded_, 0);
    lv_subject_set_int(&filament_runout_, 0);
    lv_subject_set_int(&bypass_active_, 0);
    lv_subject_set_int(&supports_bypass_, 0);
    lv_subject_set_int(&ams_feeder_open_, 0);
    lv_subject_set_int(&ams_slot_count_, 0);
    lv_subject_set_int(&ams_has_device_ops_, 0);
    lv_subject_set_int(&active_tool_port_present_, 1);

    lv_subject_set_int(&toolchange_visible_, 0);
    lv_subject_set_int(&ams_current_toolchange_, -1);
    lv_subject_set_int(&ams_number_of_toolchanges_, 0);
    lv_subject_copy_string(&toolchange_text_, "");

    lv_subject_set_int(&path_topology_, static_cast<int>(PathTopology::HUB));
    lv_subject_set_int(&path_filament_segment_, static_cast<int>(PathSegment::NONE));

    lv_subject_set_int(&all_units_disconnected_, 0);
    lv_subject_set_int(&viewed_unit_disconnected_, 0);
    set_unit_page(0, 0);
    set_unit_page_header("", nullptr);
    units_drying_signature_.clear();

    // Per-unit environment and its indicator.
    for (int i = 0; i < kMaxUnits; ++i) {
        lv_subject_set_int(&unit_temp_[i], 0);
        lv_subject_set_int(&unit_humidity_[i], 0);
        lv_subject_set_int(&unit_absent_[i], 0);
        lv_subject_set_int(&unit_disconnected_[i], 0);
        lv_subject_set_int(&unit_logo_color_[i], 0);
        lv_subject_copy_string(&env_ind_temp_text_[i], "");
        lv_subject_copy_string(&env_ind_humidity_text_[i], "");
        lv_subject_set_int(&env_ind_humidity_status_[i], 0);
        lv_subject_set_int(&env_ind_humidity_visible_[i], 0);
        lv_subject_set_int(&env_ind_visible_[i], 0);
        lv_subject_set_int(&env_ind_drying_active_[i], 0);
        lv_subject_copy_string(&env_ind_drying_text_[i], "");
    }
    mirror_detail_env_subjects(nullptr, nullptr);

    // Lanes, the loaded card, dryer, clog meter and endless spool, through the same
    // empty-state paths the sync uses.
    clear_unused_slot_subjects(0);
    bump_slots_version();
    lv_subject_set_int(&tool_map_version_, lv_subject_get_int(&tool_map_version_) + 1);
    set_current_loaded_defaults(true);
    sync_dryer_from_backend();
    sync_clog_meter_from_info(AmsSystemInfo{});
    sync_endless_spool_from_backend(nullptr);
}

bool AmsState::any_bypass_active() const {
    assert_main_thread();
    for (const auto& backend : registry_.backends()) {
        if (backend && backend->is_bypass_active()) {
            return true;
        }
    }
    return false;
}

bool AmsState::any_operation_running_here() const {
    assert_main_thread();
    for (const auto& backend : registry_.backends()) {
        if (backend && backend->running_step_operation().has_value()) {
            return true;
        }
    }
    return false;
}

bool AmsState::active_spool_describes_bypass() const {
    assert_main_thread();
    return get_backend() == nullptr || any_bypass_active();
}

AmsBackend* AmsState::get_backend() const {
    return get_backend(0);
}

int AmsState::active_backend_index() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&active_backend_));
}

void AmsState::set_active_backend(int index) {
    assert_main_thread();
    if (index >= 0 && index < registry_.count()) {
        lv_subject_set_int(&active_backend_, index);
    }
}

bool AmsState::is_available() const {
    assert_main_thread();
    const auto primary = system_info(0);
    return primary && primary->presents_filament_system();
}

bool AmsState::filament_hardware_absent() const {
    assert_main_thread();
    const auto primary = system_info(0);
    return primary && primary->external_spool_only;
}

void AmsState::set_moonraker_api(IMoonrakerAPI* api) {
    assert_main_thread();
    api_ = api;
    last_synced_spoolman_id_ = 0; // Reset tracking on API change
    spdlog::debug("[AMS State] Moonraker API {} for Spoolman integration", api ? "set" : "cleared");
}

void AmsState::set_gcode_response_callback(std::function<void(const std::string&)> callback) {
    registry_.set_gcode_response_callback(std::move(callback));
}

void AmsState::sync_backend(int backend_index) {
    assert_main_thread();

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

    const auto snapshot = read_system_info(backend_index);
    const AmsSystemInfo& info = *snapshot;
    registry_.ensure_sinks(backend_index, info.total_slots);
    auto& subs = secondary_slot_subjects_[sec_idx];

    for (int i = 0; i < std::min(info.total_slots, subs.slot_count); ++i) {
        const SlotInfo* slot = info.get_slot_global(i);
        if (slot) {
            subs.write(i, *slot);
        }
    }

    spdlog::debug("[AMS State] Synced secondary backend {} - slots={}", backend_index,
                  info.total_slots);

    // Re-evaluate "Currently Loaded" display — the active loaded filament may
    // belong to this secondary backend (e.g., AMS_2 just finished loading).
    sync_current_loaded_from_backend();
}

void AmsState::update_slot_for_backend(int backend_index, int slot_index) {
    assert_main_thread();

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
        subs.write(slot_index, slot);

        spdlog::trace("[AMS State] Updated backend {} slot {} - color=0x{:06X}, status={}",
                      backend_index, slot_index, slot.color_rgb,
                      slot_status_to_string(slot.status));
    }
}

void AmsState::sync_from_backend() {
    assert_main_thread();

    auto* backend = get_backend(0);
    if (!backend) {
        return;
    }

    const auto snapshot = read_system_info(0);
    const AmsSystemInfo& info = *snapshot;

    registry_.ensure_sinks(0, info.total_slots);
    sync_system_subjects(info);
    sync_tool_topology(backend, info);
    sync_shared_heater(backend, info);
    sync_filament_runout(info);
    sync_bypass(backend, info);
    sync_feeder(backend);
    const bool presented = info.presents_filament_system();
    lv_subject_set_int(&ams_slot_count_, presented ? info.total_slots : 0);
    warn_if_slots_exceed_subjects(info.total_slots);
    // The overlay's Quick Actions, status line and bypass rows (the force-bypass
    // row covers firmware that reports none) apply to every backend that
    // presents a system, so that is the whole condition.
    lv_subject_set_int(&ams_has_device_ops_, presented ? 1 : 0);

    // Update tool change progress raw data (text formatting in UI layer)
    if (info.number_of_toolchanges > 0) {
        lv_subject_set_int(&toolchange_visible_, 1);
    } else {
        lv_subject_set_int(&toolchange_visible_, 0);
    }
    lv_subject_set_int(&ams_current_toolchange_, info.current_toolchange);
    lv_subject_set_int(&ams_number_of_toolchanges_, info.number_of_toolchanges);

    // Cache the backend-supplied operation_detail so the print-state observer
    // can recompute the displayed string later without re-querying the backend.
    last_operation_detail_ = info.operation_detail;
    recompute_action_detail();

    // Update path visualization subjects
    int new_topology = static_cast<int>(backend->get_topology());
    lv_subject_set_int(&path_topology_, new_topology);
    int new_filament_seg = static_cast<int>(backend->get_filament_segment());
    lv_subject_set_int(&path_filament_segment_, new_filament_seg);

    // Update per-slot subjects, only firing when values actually change
    bool any_slot_changed = false;
    for (int i = 0; i < std::min(info.total_slots, kMaxSlots); ++i) {
        const SlotInfo* slot = info.get_slot_global(i);
        if (slot && write_slot_subjects(*backend, i, *slot)) {
            any_slot_changed = true;
        }
    }
    for (int i = 0; i < info.total_slots; ++i) {
        // An absent box's bays are placeholders that read empty, which is not
        // a removal: the spool in them never left.
        if (const SlotInfo* slot = info.get_slot_global(i); slot && !info.slot_absent(i)) {
            sync_tag_spool_link(*backend, i, *slot);
        }
    }

    sync_tool_routing(backend, info);

    if (sync_tool_spools(backend, info)) {
        any_slot_changed = true;
    }

    sync_unit_environment(backend, info);

    if (clear_unused_slot_subjects(info.total_slots)) {
        any_slot_changed = true;
    }

    if (any_slot_changed) {
        spdlog::trace("[AmsState] Slot data changed, bumping version");
        bump_slots_version_from_sync();
    }

    // Sync dryer state (for systems with integrated drying like ACE)
    sync_dryer_from_backend();

    // Sync clog detection meter subjects
    sync_clog_meter_from_info(info);

    // Sync the filament buffer reading and its traces
    sync_buffer_from_info(info, buffer_clock_ms());

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

void AmsState::sync_system_subjects(const AmsSystemInfo& info) {
    // A backend presenting no filament system publishes the type of none, so
    // every surface keyed on the type reads the printer as single-spool. Its
    // action and loaded state below still flow: the filament panel's Load and
    // Unload run through it and follow their progress there.
    const AmsType presented = info.presents_filament_system() ? info.type : AmsType::NONE;
    int new_type = static_cast<int>(presented);
    lv_subject_set_int(&ams_type_, new_type);
    // Published from the predicate, not the enum value, so a new tool-changer
    // type is picked up by XML without touching a binding.
    int new_tool_changer = is_tool_changer(presented) ? 1 : 0;
    lv_subject_set_int(&ams_is_tool_changer_, new_tool_changer);
    int new_filament_system = is_filament_system(presented) ? 1 : 0;
    lv_subject_set_int(&ams_is_filament_system_, new_filament_system);
    int new_action = static_cast<int>(info.action);
    if (optimistic_action_until_) {
        if (info.action != AmsAction::IDLE ||
            std::chrono::steady_clock::now() >= *optimistic_action_until_) {
            optimistic_action_until_.reset();
        } else {
            new_action = lv_subject_get_int(&ams_action_);
        }
    }
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
    runout_grace_.on_action(static_cast<AmsAction>(lv_subject_get_int(&ams_action_)),
                            static_cast<AmsAction>(new_action));
    if (lv_subject_get_int(&ams_action_) != new_action) {
        spdlog::debug("[AmsState] sync_from_backend: action changed to {} ({})", new_action,
                      ams_action_to_string(static_cast<AmsAction>(new_action)));
        publish_action(static_cast<AmsAction>(new_action));
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
    if (presented == AmsType::NONE) {
        // No system shown, so no name or logo for one.
    } else if (!info.type_name.empty()) {
        sys_name = info.type_name;
    } else {
        sys_name = ams_type_to_string(info.type);
    }
    if (strcmp(lv_subject_get_string(&ams_system_name_), sys_name.c_str()) != 0) {
        lv_subject_copy_string(&ams_system_name_, sys_name.c_str());
    }

    // Set system logo path for declarative image binding (pointer subject for bind_src)
    const LogoArt logo_art = get_logo(sys_name);
    const char* new_logo = logo_art.path ? logo_art.path : "";
    if (strcmp(system_logo_buf_, new_logo) != 0) {
        strncpy(system_logo_buf_, new_logo, sizeof(system_logo_buf_) - 1);
        system_logo_buf_[sizeof(system_logo_buf_) - 1] = '\0';
        lv_subject_set_pointer(&ams_system_logo_, system_logo_buf_);
    }
    set_int_if_changed(&ams_system_has_logo_, system_logo_buf_[0] ? 1 : 0);
    set_int_if_changed(&ams_system_logo_color_, logo_art.color ? 1 : 0);
    if (lv_subject_get_int(&current_slot_) != info.current_slot) {
        spdlog::debug("[AmsState] current_slot changed: {} → {}",
                      lv_subject_get_int(&current_slot_), info.current_slot);
        lv_subject_set_int(&current_slot_, info.current_slot);
    }
    lv_subject_set_int(&pending_target_slot_, info.pending_target_slot);
    lv_subject_set_int(&ams_current_tool_, info.current_tool);
}

std::string AmsState::nozzle_heat_blocked_reason() const {
    assert_main_thread();
    const AmsBackend* backend = get_backend(0);
    return backend ? backend->nozzle_heat_blocked_reason() : std::string();
}

void AmsState::sync_shared_heater(AmsBackend* backend, const AmsSystemInfo& info) {
    const bool blocked = backend && !backend->nozzle_heat_blocked_reason().empty();
    if (lv_subject_get_int(&ams_nozzle_heat_blocked_) != (blocked ? 1 : 0)) {
        lv_subject_set_int(&ams_nozzle_heat_blocked_, blocked ? 1 : 0);
    }
    const int can_cancel = backend && backend->can_cancel_operation() ? 1 : 0;
    if (lv_subject_get_int(&ams_can_cancel_) != can_cancel) {
        lv_subject_set_int(&ams_can_cancel_, can_cancel);
    }
    if (!backend || !backend->heads_share_heater()) {
        shared_heater_tool_ = -2;
        return;
    }
    // The swap preheat latch keeps the last material's temperature to purge it
    // from the same melt zone. A different mounted nozzle is a different melt
    // zone, so the previous tool's temperature has nothing left to purge.
    if (shared_heater_tool_ != -2 && info.current_tool != shared_heater_tool_) {
        get_printer_state().temperature_state().clear_load_latch();
    }
    shared_heater_tool_ = info.current_tool;
}

void AmsState::sync_tool_topology(AmsBackend* backend, const AmsSystemInfo& info) {
    // Push tool topology to ToolState when the active backend multiplexes tools.
    // Otherwise leave ToolState in its extruder-enumerated state.
    auto& tool_state = helix::ToolState::instance();
    if (auto topo = helix::build_ams_topology(backend, 0, info)) {
        tool_state.clear_backend_active_tool();
        tool_state.set_ams_topology(*topo);
        return;
    }
    if (tool_state.ams_topology_active()) {
        // Backend stopped multiplexing (e.g., AMS removed). Drop the override
        // so callers can rebuild tools_ from extruders.
        tool_state.clear_ams_topology();
    }
    // With no table the extruder enumeration stands, but a carriage that can
    // be empty still needs the backend to say which head is mounted: the
    // status frame names the last head used.
    if (backend && backend->carriage_can_be_empty()) {
        tool_state.set_backend_active_tool(backend->get_current_tool());
    } else {
        tool_state.clear_backend_active_tool();
    }
}

void AmsState::sync_filament_runout(const AmsSystemInfo& info) {
    int new_loaded = info.filament_loaded ? 1 : 0;
    lv_subject_set_int(&filament_loaded_, new_loaded);

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
    const PrintJobState job_state = get_printer_state().print_state().get_print_job_state();
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
    // Filament back at the toolhead retires the grace: it was armed for the
    // removal this unload caused, and anything after a reload is a new event.
    if (info.filament_loaded) {
        runout_grace_.on_filament_loaded();
    }
}

void AmsState::sync_feeder(AmsBackend* backend) {
    const int open = backend->supports_feeder_control() && backend->feeder_open() ? 1 : 0;
    if (lv_subject_get_int(&ams_feeder_open_) != open) {
        spdlog::debug("[AmsState] feeder open -> {}", open);
        lv_subject_set_int(&ams_feeder_open_, open);
    }
}

void AmsState::sync_bypass(AmsBackend* backend, const AmsSystemInfo& info) {
    // The one bypass truth: the backend's own is_bypass_active(), the same
    // predicate BypassToggleController branches on when the user taps, so the
    // switch and the action it takes read one value.
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
    // empty-slot block still fires on Print.
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
        bump_slots_version_from_sync();
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
            for (auto& backend : registry_.backends()) {
                if (backend) {
                    backend->publish_external_spool_lane(spool.has_value() ? &spool.value()
                                                                           : nullptr);
                }
            }
        }
    }
    // With no system presented the external spool is the only path, so there
    // is no bypass to switch to, forced or not.
    int new_supports_bypass =
        info.presents_filament_system() && helix::bypass_available_for(info.supports_bypass) ? 1
                                                                                             : 0;
    lv_subject_set_int(&supports_bypass_, new_supports_bypass);

    // Update external spool color from persistent settings
    auto ext_spool = helix::SettingsManager::instance().get_external_spool_info();
    int new_ext_color = ext_spool.has_value() ? static_cast<int>(ext_spool->color_rgb) : 0;
    lv_subject_set_int(&external_spool_color_, new_ext_color);
    lv_subject_copy_string(&external_spool_material_,
                           ext_spool.has_value() ? ext_spool->material.c_str() : "");
}

void AmsState::sync_tool_routing(AmsBackend* backend, const AmsSystemInfo& info) {
    // Detect routing changes and bump tool_map_version_ so the gcode renderer
    // refreshes tool colors.
    //
    // Watching tool_to_slot_map alone was not enough once the render started
    // resolving colors through the APPLIED routing: on a backend that publishes
    // a separate routing table (a tool changer's firmware map), the physical map
    // never moves while the routing does, so the preview kept the previous
    // print's colors. Both are watched now, and either moving repaints.
    const std::vector<int> applied_routing = backend->get_tool_mapping();
    if (info.tool_to_slot_map != last_tool_map_ || applied_routing != last_applied_routing_) {
        last_tool_map_ = info.tool_to_slot_map;
        last_applied_routing_ = applied_routing;
        int v = lv_subject_get_int(&tool_map_version_);
        lv_subject_set_int(&tool_map_version_, v + 1);
        spdlog::debug("[AmsState] tool routing changed, version now {}", v + 1);
    }
}

bool AmsState::sync_tool_spools(AmsBackend* backend, const AmsSystemInfo& info) {
    bool any_slot_changed = false;
    // Sync spool assignments to ToolState for slots with mapped tools.
    //
    // The clear branch matters as much as the assign one: this only ever
    // assigned, so a lane that lost its spool (eject, or an explicit unlink)
    // left the old assignment behind in ToolState — and ToolState persists to
    // tool_spools.json plus a Moonraker DB key, so the stale spool outlived
    // restarts. Observed on the .112 BoxTurtle: "Assigned spool 86 () to tool 0"
    // fired during an EJECT, and ToolState::clear_spool() had no callers at all.
    for (int i = 0; i < info.total_slots; ++i) {
        const SlotInfo* slot = info.get_slot_global(i);
        if (!slot || slot->mapped_tool < 0) {
            continue;
        }
        if (slot->spoolman_id > 0) {
            ToolState::instance().assign_spool(slot->mapped_tool, slot->spoolman_id,
                                               slot->spool_name, slot->remaining_weight_g,
                                               slot->total_weight_g);
        } else if (!backend->supports_per_tool_spool_assignment()) {
            // Only clear when the SLOT is authoritative. On a tool changer the
            // flow runs the other way — ToolState is the source of truth and
            // slots start empty — so clearing here would destroy the assignment
            // the reverse sync below is about to propagate.
            //
            // The question is "who owns the assignment", NOT "does firmware
            // persist the spool id", which is what this used to ask. CFS and
            // AD5X IFS answer no to the latter (they keep identity in OUR
            // lane_data override store, not in firmware) yet their lanes are
            // fully authoritative, so they fell into the tool-changer branch:
            // clearing a lane left the old spool in ToolState, and the reverse
            // pass below then copied it straight back onto the lane. The lane
            // blinked empty and refilled itself on the next poll.
            ToolState::instance().clear_spool(slot->mapped_tool);
        }
    }

    // Reverse sync: populate backend slots from ToolState on a tool changer,
    // where each tool owns its own spool and the slot is the shadow. Without
    // this, assignments made through Application's auto-assign-active-spool
    // path (which writes ToolState directly) never reach the slot subjects.
    //
    // Gated on who OWNS the assignment. Gating it on
    // has_firmware_spool_persistence() also caught every backend that keeps
    // identity in our own override store rather than in firmware — CFS, AD5X
    // IFS — and resurrected spools the user had just cleared from a lane. See
    // the matching note on the clear branch above.
    if (backend->supports_per_tool_spool_assignment()) {
        auto& tool_state = ToolState::instance();
        const auto& tools = tool_state.tools();
        for (int i = 0; i < info.total_slots; ++i) {
            const SlotInfo* slot = info.get_slot_global(i);
            if (slot && slot->mapped_tool >= 0 && slot->spoolman_id == 0) {
                int ti = slot->mapped_tool;
                if (ti >= 0 && ti < static_cast<int>(tools.size()) && tools[ti].spoolman_id > 0) {
                    SlotInfo updated = *slot;
                    updated.spoolman_id = tools[ti].spoolman_id;
                    updated.spool_name = tools[ti].spool_name;
                    updated.remaining_weight_g = tools[ti].remaining_weight_g;
                    updated.total_weight_g = tools[ti].total_weight_g;
                    // The loop above published this slot from the snapshot taken
                    // before the write, so publish it again from what the backend
                    // now holds.
                    if (backend->sync_external_identity(i, updated).success() &&
                        write_slot_subjects(*backend, i, backend->get_slot_info(i))) {
                        any_slot_changed = true;
                    }
                }
            }
        }
    }

    // Flushing ToolState is about PERSISTENCE, not about which direction the
    // sync ran, so it keeps its own question: firmware won't remember the
    // assignment, therefore we have to. Narrowing the reverse-sync gate above
    // would otherwise have silently stopped saving on CFS and AD5X IFS.
    if (!backend->has_firmware_spool_persistence()) {
        ToolState::instance().save_spool_assignments_if_dirty(get_moonraker_api());
    }
    return any_slot_changed;
}

void AmsState::set_viewed_unit(int unit_index) {
    assert_main_thread("set_viewed_unit");
    // The flag follows the unit's data, which every sync_from_backend() republishes, so
    // naming the unit already viewed leaves it right and costs no read of the system info.
    if (viewed_unit_ == unit_index)
        return;
    viewed_unit_ = unit_index;
    if (auto* backend = get_backend(0)) {
        const AmsSystemInfo info = backend->get_system_info();
        publish_viewed_unit_disconnected(&info);
    } else {
        publish_viewed_unit_disconnected(nullptr);
    }
}

void AmsState::set_unit_page(int count, int current) {
    assert_main_thread("set_unit_page");
    count = std::max(0, count);
    current = count > 0 ? std::clamp(current, 0, count - 1) : 0;
    // Only a change is published, so a refresh that finds the same page wakes nobody.
    set_int_if_changed(&ams_page_count_, count);
    set_int_if_changed(&ams_page_current_, current);
    set_int_if_changed(&ams_page_has_prev_, current > 0 ? 1 : 0);
    set_int_if_changed(&ams_page_has_next_, current + 1 < count ? 1 : 0);
}

void AmsState::set_unit_page_header(const std::string& name, const char* logo_path,
                                    bool logo_is_color) {
    assert_main_thread("set_unit_page_header");
    if (name != lv_subject_get_string(&ams_page_unit_name_))
        lv_subject_copy_string(&ams_page_unit_name_, name.c_str());
    const char* logo = logo_path ? logo_path : "";
    set_int_if_changed(&ams_page_unit_logo_color_, logo_is_color ? 1 : 0);
    if (strcmp(page_unit_logo_buf_, logo) != 0) {
        strncpy(page_unit_logo_buf_, logo, sizeof(page_unit_logo_buf_) - 1);
        page_unit_logo_buf_[sizeof(page_unit_logo_buf_) - 1] = '\0';
        lv_subject_set_pointer(&ams_page_unit_logo_,
                               page_unit_logo_buf_[0] ? page_unit_logo_buf_ : nullptr);
        lv_subject_set_int(&ams_page_has_logo_, page_unit_logo_buf_[0] ? 1 : 0);
    }
}

void AmsState::set_unit_view_active(bool active) {
    assert_main_thread("set_unit_view_active");
    const int value = active ? 1 : 0;
    if (lv_subject_get_int(&ams_unit_view_active_) != value)
        lv_subject_set_int(&ams_unit_view_active_, value);
}

void AmsState::publish_viewed_unit_disconnected(const AmsSystemInfo* info) {
    // Read from the unit's own data, so the flag is right for any unit index.
    bool disconnected = false;
    if (info && viewed_unit_ >= 0) {
        for (const auto& unit : info->units) {
            if (unit.unit_index == viewed_unit_) {
                disconnected = !unit.absent && !unit.connected;
                break;
            }
        }
    }
    lv_subject_set_int(&viewed_unit_disconnected_, disconnected ? 1 : 0);
}

void AmsState::sync_unit_environment(AmsBackend* backend, const AmsSystemInfo& info) {
    // Every present unit offline is the whole system offline; absent
    // placeholders say nothing either way.
    bool any_present = false;
    bool all_disconnected = true;
    for (const auto& unit : info.units) {
        if (!unit.absent) {
            any_present = true;
            all_disconnected = all_disconnected && !unit.connected;
        }
    }
    lv_subject_set_int(&all_units_disconnected_, (any_present && all_disconnected) ? 1 : 0);

    // Update per-unit environment subjects (CFS temperature/humidity)
    for (const auto& unit : info.units) {
        int idx = unit.unit_index;
        if (idx >= 0 && idx < kMaxUnits) {
            lv_subject_set_int(&unit_absent_[idx], unit.absent ? 1 : 0);
            set_int_if_changed(&unit_logo_color_[idx], get_unit_logo(unit, info).color ? 1 : 0);
            lv_subject_set_int(&unit_disconnected_[idx], (!unit.absent && !unit.connected) ? 1 : 0);
            if (unit.environment.has_value()) {
                int temp_tenths = static_cast<int>(unit.environment->temperature_c * 10.0f);
                int humidity = static_cast<int>(unit.environment->humidity_pct);
                lv_subject_set_int(&unit_temp_[idx], temp_tenths);
                lv_subject_set_int(&unit_humidity_[idx], humidity);
            } else {
                lv_subject_set_int(&unit_temp_[idx], 0);
                lv_subject_set_int(&unit_humidity_[idx], 0);
            }
        }
    }

    // Clear environment subjects for units beyond what backend reports
    for (int i = static_cast<int>(info.units.size()); i < kMaxUnits; ++i) {
        lv_subject_set_int(&unit_temp_[i], 0);
        lv_subject_set_int(&unit_humidity_[i], 0);
        lv_subject_set_int(&unit_absent_[i], 0);
        lv_subject_set_int(&unit_disconnected_[i], 0);
        set_int_if_changed(&unit_logo_color_[i], 0);
    }
    publish_viewed_unit_disconnected(&info);

    // Which dryers run. A unit on another page of the unit view has no subject of its
    // own, so a change in the set bumps one version the view watches.
    std::string drying(info.units.size(), '0');
    for (size_t k = 0; k < info.units.size(); ++k) {
        const DryerInfo dryer = backend->get_dryer_info(info.units[k].unit_index);
        if (dryer.supported && dryer.active)
            drying[k] = '1';
    }
    if (drying != units_drying_signature_) {
        units_drying_signature_ = std::move(drying);
        lv_subject_set_int(&ams_units_dryer_version_,
                           lv_subject_get_int(&ams_units_dryer_version_) + 1);
    }

    // Indicator values are computed once per unit (compute_unit_env_indicator) and
    // published to the per-unit subjects below and to the detail mirror.
    std::vector<UnitEnvIndicator> indicators(info.units.size());
    for (size_t k = 0; k < info.units.size(); ++k) {
        indicators[k] = compute_unit_env_indicator(backend, info.units[k]);
        const int idx = info.units[k].unit_index;
        if (idx >= 0 && idx < kMaxUnits)
            publish_unit_env_indicator(idx, indicators[k]);
    }

    // Clear indicator for units beyond what backend reports
    for (int i = static_cast<int>(info.units.size()); i < kMaxUnits; ++i) {
        lv_subject_set_int(&env_ind_visible_[i], 0);
        lv_subject_set_int(&env_ind_humidity_visible_[i], 0);
        lv_subject_set_int(&env_ind_drying_active_[i], 0);
    }

    mirror_detail_env_subjects(backend, &info);
}

AmsState::UnitEnvIndicator AmsState::compute_unit_env_indicator(AmsBackend* backend,
                                                                const AmsUnit& unit) {
    UnitEnvIndicator out;
    const bool has_env = unit.environment.has_value();
    char buf[kEnvIndTextBufSize];
    if (has_env) {
        // Format temperature text (e.g., "24°C")
        snprintf(buf, sizeof(buf),
                 "%d\xC2\xB0"
                 "C",
                 static_cast<int>(unit.environment->temperature_c));
        out.temp_text = buf;
        // Format humidity text (e.g., "46%")
        snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(unit.environment->humidity_pct));
        out.humidity_text = buf;
        if (unit.environment->has_humidity)
            out.humidity_x10 =
                std::max(0, static_cast<int>(std::lround(unit.environment->humidity_pct * 10.0f)));

        // Humidity status color from the most restrictive loaded material:
        // 0=ok (green), 1=warn (yellow), 2=danger (red)
        const float humidity_pct = unit.environment->humidity_pct;
        float most_restrictive_good = 999.0f;
        float most_restrictive_warn = 999.0f;
        bool found_any_range = false;
        for (int si = 0; si < unit.slot_count; ++si) {
            SlotInfo slot = backend->get_slot_info(unit.first_slot_global_index + si);
            if (slot.material.empty())
                continue;
            if (const auto range = filament::get_comfort_range(slot.material)) {
                found_any_range = true;
                most_restrictive_good = std::min(most_restrictive_good, range->max_humidity_good);
                most_restrictive_warn = std::min(most_restrictive_warn, range->max_humidity_warn);
            }
        }
        if (found_any_range) {
            if (humidity_pct > most_restrictive_warn)
                out.humidity_status = 2;
            else if (humidity_pct > most_restrictive_good)
                out.humidity_status = 1;
        }
    } else {
        // No live reading: an em-dash keeps a drying-capable unit's indicator
        // tappable instead of showing a blank temperature.
        out.temp_text = "\xE2\x80\x94";
    }

    // Reachable with live environment data OR a supported dryer: a dryer-capable
    // box with no temp/humidity sensor still needs a way to open the drying controls.
    const DryerInfo dryer = backend->get_dryer_info(unit.unit_index);
    out.visible = has_env || dryer.supported;
    out.humidity_visible = has_env && unit.environment->has_humidity;

    if (out.visible && dryer.supported && dryer.active) {
        out.drying_active = true;
        // Compact countdown for the small indicator
        const int hrs = dryer.remaining_min / 60;
        const int mins = dryer.remaining_min % 60;
        char drying_buf[kEnvIndDryingBufSize];
        if (hrs > 0)
            snprintf(drying_buf, sizeof(drying_buf), "%d:%02d", hrs, mins);
        else
            snprintf(drying_buf, sizeof(drying_buf), "%d min", mins);
        out.drying_text = drying_buf;
    }
    return out;
}

namespace {
void set_string_if_changed(lv_subject_t* subject, const std::string& value) {
    if (value != lv_subject_get_string(subject))
        lv_subject_copy_string(subject, value.c_str());
}
} // namespace

void AmsState::publish_unit_env_indicator(int idx, const UnitEnvIndicator& e) {
    set_string_if_changed(&env_ind_temp_text_[idx], e.temp_text);
    set_string_if_changed(&env_ind_humidity_text_[idx], e.humidity_text);
    lv_subject_set_int(&env_ind_humidity_[idx], e.humidity_x10);
    lv_subject_set_int(&env_ind_humidity_status_[idx], e.humidity_status);
    lv_subject_set_int(&env_ind_humidity_visible_[idx], e.humidity_visible ? 1 : 0);
    lv_subject_set_int(&env_ind_visible_[idx], e.visible ? 1 : 0);
    lv_subject_set_int(&env_ind_drying_active_[idx], e.drying_active ? 1 : 0);
    if (e.drying_active)
        set_string_if_changed(&env_ind_drying_text_[idx], e.drying_text);
}

void AmsState::warn_if_slots_exceed_subjects(int total_slots) {
    if (total_slots <= kMaxSlots || total_slots == slot_overflow_warned_total_) {
        return;
    }
    slot_overflow_warned_total_ = total_slots;
    spdlog::warn("[AMS State] Backend reports {} slots but per-slot subjects cover {}; "
                 "slots {}-{} show unbound",
                 total_slots, kMaxSlots, kMaxSlots + 1, total_slots);
}

bool AmsState::clear_unused_slot_subjects(int total_slots) {
    // Clear remaining slot subjects, only firing when values actually change
    bool any_slot_changed = false;
    for (int i = total_slots; i < kMaxSlots; ++i) {
        int default_color = static_cast<int>(kAmsDefaultSlotColor);
        if (lv_subject_get_int(&slot_colors_[i]) != default_color) {
            lv_subject_set_int(&slot_colors_[i], default_color);
            any_slot_changed = true;
        }
        if (lv_subject_get_int(&slot_paints_[i]) != 0) {
            lv_subject_set_int(&slot_paints_[i], 0);
            any_slot_changed = true;
        }
        int default_status = static_cast<int>(SlotStatus::UNKNOWN);
        if (lv_subject_get_int(&slot_statuses_[i]) != default_status) {
            lv_subject_set_int(&slot_statuses_[i], default_status);
            any_slot_changed = true;
        }
        int default_lane_state = static_cast<int>(helix::ui::LaneState::Empty);
        if (lv_subject_get_int(&slot_lane_states_[i]) != default_lane_state) {
            lv_subject_set_int(&slot_lane_states_[i], default_lane_state);
            any_slot_changed = true;
        }
        if (lv_subject_get_int(&slot_has_error_[i]) != 0) {
            lv_subject_set_int(&slot_has_error_[i], 0);
            any_slot_changed = true;
        }
        int default_severity = static_cast<int>(SlotError::Severity::INFO);
        if (lv_subject_get_int(&slot_error_severity_[i]) != default_severity) {
            lv_subject_set_int(&slot_error_severity_[i], default_severity);
            any_slot_changed = true;
        }
        // Clear remaining filament for unused slots
        if (strcmp(lv_subject_get_string(&slot_remaining_[i]), "") != 0) {
            lv_subject_copy_string(&slot_remaining_[i], "");
        }
        const int idle = static_cast<int>(helix::printer::ZoneDryingState::Idle);
        if (lv_subject_get_int(&slot_drying_[i]) != idle) {
            lv_subject_set_int(&slot_drying_[i], idle);
        }
        // Clear material for unused slots — bump so the label clears (#1065)
        if (strcmp(lv_subject_get_string(&slot_materials_[i]), "") != 0) {
            lv_subject_copy_string(&slot_materials_[i], "");
            any_slot_changed = true;
        }
        if (strcmp(lv_subject_get_string(&slot_names_[i]), "") != 0) {
            lv_subject_copy_string(&slot_names_[i], "");
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
    return any_slot_changed;
}

bool AmsState::write_slot_name(lv_subject_t* subject, size_t buf_size, const SlotInfo& slot) {
    const auto identity = SpoolmanManager::find_identity(slot.spoolman_id);
    const std::string name = helix::slot_card_name(slot, identity ? &*identity : nullptr);
    // buf_size - 1 leaves room for the NUL; cutting here keeps a long name on a
    // codepoint boundary instead of letting the copy split one.
    const std::string bounded(helix::text_io::utf8_prefix(name, buf_size - 1));
    if (strcmp(lv_subject_get_string(subject), bounded.c_str()) == 0) {
        return false;
    }
    lv_subject_copy_string(subject, bounded.c_str());
    return true;
}

void AmsState::refresh_slot_names() {
    assert_main_thread();
    bool changed = false;
    if (auto* backend = get_backend(0)) {
        const AmsSystemInfo info = backend->get_system_info();
        for (int i = 0; i < std::min(info.total_slots, kMaxSlots); ++i) {
            const SlotInfo* slot = info.get_slot_global(i);
            if (slot && write_slot_name(&slot_names_[i], sizeof(slot_names_buf_[i]), *slot)) {
                changed = true;
            }
        }
    }
    for (size_t sec = 0; sec < secondary_slot_subjects_.size(); ++sec) {
        auto* backend = get_backend(static_cast<int>(sec) + 1);
        if (!backend) {
            continue;
        }
        auto& subs = secondary_slot_subjects_[sec];
        const AmsSystemInfo info = backend->get_system_info();
        for (int i = 0; i < std::min(info.total_slots, subs.slot_count); ++i) {
            const SlotInfo* slot = info.get_slot_global(i);
            if (slot && write_slot_name(&subs.names[i], kSlotNameBufSize, *slot)) {
                changed = true;
            }
        }
    }
    if (changed) {
        spdlog::debug("[AmsState] Slot names re-derived from new Spoolman identity");
    }
}

bool AmsState::write_slot_subjects(AmsBackend& backend, int slot_index, const SlotInfo& slot) {
    if (slot_index < 0 || slot_index >= kMaxSlots) {
        return false;
    }
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

    // Lane presentation classification (Present, Ghosted, Empty): the input
    // every lane rendering surface binds to.
    int new_lane_state = static_cast<int>(helix::ui::classify_lane(slot));
    if (lv_subject_get_int(&slot_lane_states_[slot_index]) != new_lane_state) {
        lv_subject_set_int(&slot_lane_states_[slot_index], new_lane_state);
        changed = true;
    }

    // Error flag and severity for the lane's status line.
    bool new_has_error = false;
    int new_severity = static_cast<int>(SlotError::Severity::INFO);
    slot_error_state(slot, new_has_error, new_severity);
    if (lv_subject_get_int(&slot_has_error_[slot_index]) != (new_has_error ? 1 : 0)) {
        lv_subject_set_int(&slot_has_error_[slot_index], new_has_error ? 1 : 0);
        changed = true;
    }
    if (lv_subject_get_int(&slot_error_severity_[slot_index]) != new_severity) {
        lv_subject_set_int(&slot_error_severity_[slot_index], new_severity);
        changed = true;
    }

    // Fill percent in the canonical display_fill_pct() encoding. The ams_slot
    // widget observes this, so spool fill renders from state on every panel.
    int new_fill = slot.display_fill_pct();
    if (lv_subject_get_int(&slot_fills_[slot_index]) != new_fill) {
        lv_subject_set_int(&slot_fills_[slot_index], new_fill);
        changed = true;
    }

    // Multi-colour list and finish: a swatch beside the colour repaints on it.
    const int new_paint = slot.paint_key();
    if (lv_subject_get_int(&slot_paints_[slot_index]) != new_paint) {
        lv_subject_set_int(&slot_paints_[slot_index], new_paint);
        changed = true;
    }

    // Measured remaining length, "" when the backend publishes none. The slot
    // shows it under its material; weight has its own surfaces.
    std::string remaining = slot.remaining_length_display();
    if (strcmp(lv_subject_get_string(&slot_remaining_[slot_index]), remaining.c_str()) != 0) {
        lv_subject_copy_string(&slot_remaining_[slot_index], remaining.c_str());
    }

    // The lane's part in a drying cycle, for the drying badge on its slot.
    const int new_drying = static_cast<int>(slot.drying_state);
    if (lv_subject_get_int(&slot_drying_[slot_index]) != new_drying) {
        lv_subject_set_int(&slot_drying_[slot_index], new_drying);
    }

    // Material type. The ams_slot widget binds this subject directly (its
    // material observer repaints the label), but container-level consumers
    // re-read material through refresh_slots() keyed on slots_version, so a
    // material delta still bumps the version (#1065).
    if (strcmp(lv_subject_get_string(&slot_materials_[slot_index]), slot.material.c_str()) != 0) {
        lv_subject_copy_string(&slot_materials_[slot_index], slot.material.c_str());
        changed = true;
    }
    // The name the card shows above the material. A different spool of the
    // same material changes only this.
    if (write_slot_name(&slot_names_[slot_index], sizeof(slot_names_buf_[slot_index]), slot)) {
        changed = true;
    }

    // Per-slot LIVE state: path segment, toolhead-present, active-loaded. Read
    // from the backend accessors so the panel observes real-time sensor changes
    // (path redraw and active-lane highlight).
    int new_segment = static_cast<int>(backend.get_slot_filament_segment(slot_index));
    if (lv_subject_get_int(&slot_segments_[slot_index]) != new_segment) {
        lv_subject_set_int(&slot_segments_[slot_index], new_segment);
        changed = true;
    }
    int new_toolhead = backend.slot_has_filament_at_toolhead(slot_index) ? 1 : 0;
    if (lv_subject_get_int(&slot_toolhead_present_[slot_index]) != new_toolhead) {
        lv_subject_set_int(&slot_toolhead_present_[slot_index], new_toolhead);
        changed = true;
    }
    int new_active = backend.slot_is_actively_loaded(slot_index) ? 1 : 0;
    if (lv_subject_get_int(&slot_active_loaded_[slot_index]) != new_active) {
        lv_subject_set_int(&slot_active_loaded_[slot_index], new_active);
        changed = true;
    }

    return changed;
}

void AmsState::update_slot(int slot_index) {
    assert_main_thread();

    auto* backend = get_backend(0);
    if (!backend || slot_index < 0) {
        return;
    }

    // A slot past kMaxSlots has no subjects of its own, but it can still be
    // the loaded one and still map to a tool.
    SlotInfo slot = backend->get_slot_info(slot_index);
    if (slot.slot_index >= 0) {
        if (write_slot_subjects(*backend, slot_index, slot)) {
            bump_slots_version();
        }

        // "Currently Loaded" shows the loaded slot's colour, label and weight,
        // and nothing but a full sync re-reads them.
        if (backend->is_filament_loaded() && backend->get_current_slot() == slot_index) {
            sync_current_loaded_from_backend();
        }

        // Sync spool to ToolState if this slot maps to a tool
        if (slot.mapped_tool >= 0 && slot.spoolman_id > 0) {
            ToolState::instance().assign_spool(slot.mapped_tool, slot.spoolman_id, slot.spool_name,
                                               slot.remaining_weight_g, slot.total_weight_g);
            if (!backend->has_firmware_spool_persistence()) {
                ToolState::instance().save_spool_assignments_if_dirty(get_moonraker_api());
            }
        }

        if (const auto info = read_system_info(0); info && !info->slot_absent(slot_index)) {
            sync_tag_spool_link(*backend, slot_index, slot);
        }

        spdlog::trace("[AMS State] Updated slot {} - color=0x{:06X}, status={}", slot_index,
                      slot.color_rgb, slot_status_to_string(slot.status));
    }
}

void AmsState::on_backend_event(int backend_index, const std::string& event,
                                const std::string& data) {
    spdlog::trace("[AMS State] Received event '{}' data='{}' from backend {}", event, data,
                  backend_index);

    // The backend's data has moved; readers between here and the queued sync
    // read it again rather than serve the reading from before the event.
    invalidate_system_info();

    auto queue_sync = [backend_index](bool full_sync, int slot_index, bool ends_operation = false) {
        helix::ui::queue_update(
            "AmsState::on_backend_event", [backend_index, full_sync, slot_index, ends_operation]() {
                // Skip if shutdown is in progress - AmsState singleton may be destroyed
                if (ams_state_detail::shutting_down()) {
                    return;
                }

                if (ends_operation && backend_index == 0) {
                    AmsState::instance().release_optimistic_action();
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
                AmsState::instance().bump_data_revision();
            });
    };

    if (event == AmsBackend::EVENT_STATE_CHANGED) {
        queue_sync(true, -1);
    } else if (event == AmsBackend::kEventSlotChanged) {
        // Parse slot index from data. Fall back to a full sync for ANY case
        // where we can't parse a specific slot — empty data OR non-numeric.
        // Dropping the event silently (the old behavior) left the UI stale
        // whenever a backend forgot to pass slot_index.
        if (data.empty()) {
            queue_sync(true, -1);
        } else {
            const auto slot_index = helix::text_io::parse_leading<int>(data);
            if (slot_index) {
                queue_sync(false, *slot_index);
            } else {
                queue_sync(true, -1);
            }
        }
    } else if (event == AmsBackend::kEventLoadComplete ||
               event == AmsBackend::kEventUnloadComplete) {
        // The operation is over, so an optimistic action held over the
        // backend's silence ends with it; a busy action the backend still
        // reports keeps the display until its own last frame.
        queue_sync(true, -1, /*ends_operation=*/true);
    } else if (event == AmsBackend::kEventToolChanged) {
        queue_sync(true, -1);
    } else if (event == AmsBackend::kEventError) {
        // Error occurred, sync to get error state. An error ends the operation,
        // so an optimistic action held over the backend's silence ends with it.
        queue_sync(true, -1, /*ends_operation=*/true);
        spdlog::warn("[AMS State] Backend error - {}", data);
    } else if (event == AmsBackend::kEventAttentionRequired) {
        // User intervention needed
        queue_sync(true, -1);
        spdlog::warn("[AMS State] Attention required - {}", data);
    } else if (event == AmsBackend::kEventEndlessSpoolRefused) {
        // The switch flipped itself on the tap; the backend's state never moved,
        // so an unchanged set would not notify. Re-announce it.
        helix::ui::queue_update("AmsState::endless_spool_refused", []() {
            if (ams_state_detail::shutting_down()) {
                return;
            }
            lv_subject_notify(AmsState::instance().get_endless_spool_on_subject());
        });
    }
}

void AmsState::sync_endless_spool_from_backend(AmsBackend* backend) {
    using helix::printer::EndlessSpoolCapabilities;
    using helix::printer::EndlessSpoolEnabled;
    using helix::printer::EndlessSpoolStatus;
    using helix::printer::EndlessSpoolStatusKind;

    // Capabilities take the backend's own mutex_, which is the lock order
    // sync_from_backend() already established with get_system_info().
    const EndlessSpoolCapabilities caps =
        backend != nullptr ? backend->get_endless_spool_capabilities() : EndlessSpoolCapabilities{};
    const EndlessSpoolStatus status =
        backend != nullptr ? helix::printer::endless_spool_status(caps) : EndlessSpoolStatus{};

    const int kind = static_cast<int>(status.kind);
    if (lv_subject_get_int(&ams_endless_state_) != kind) {
        spdlog::debug("[AmsState] endless spool status -> kind={} text='{}'", kind, status.text);
        lv_subject_set_int(&ams_endless_state_, kind);
    }
    const int switchable = caps.available() && caps.switchable ? 1 : 0;
    const int on = (caps.enabled == EndlessSpoolEnabled::On ||
                    caps.enabled == EndlessSpoolEnabled::OnWithoutBackup)
                       ? 1
                       : 0;
    if (lv_subject_get_int(&ams_endless_spool_switchable_) != switchable) {
        lv_subject_set_int(&ams_endless_spool_switchable_, switchable);
    }
    if (lv_subject_get_int(&ams_endless_spool_on_) != on) {
        lv_subject_set_int(&ams_endless_spool_on_, on);
    }

    // The kind can hold while the sentence changes (a CFS box that keeps
    // auto-refill on but gains a restriction reason), so the text is compared
    // independently rather than gated on the kind having moved.
    if (strcmp(lv_subject_get_string(&ams_endless_text_), status.text.c_str()) != 0) {
        lv_subject_copy_string(&ams_endless_text_, status.text.c_str());
    }
}

void AmsState::bump_slots_version() {
    invalidate_system_info();
    bump_slots_version_from_sync();
}

void AmsState::bump_slots_version_from_sync() {
    int current = lv_subject_get_int(&slots_version_);
    lv_subject_set_int(&slots_version_, current + 1);
}

std::shared_ptr<const AmsSystemInfo> AmsState::read_system_info(int backend_index) const {
    assert_main_thread();
    auto* backend = get_backend(backend_index);
    if (!backend) {
        return nullptr;
    }
    // Taken before the read: an event raised while the backend is copied
    // advances the generation, and the next system_info() reads again.
    const uint64_t generation = system_info_generation_.load(std::memory_order_acquire);
    auto info = std::make_shared<const AmsSystemInfo>(backend->get_system_info());
    if (system_info_snapshots_.size() <= static_cast<size_t>(backend_index)) {
        system_info_snapshots_.resize(static_cast<size_t>(backend_index) + 1);
    }
    system_info_snapshots_[static_cast<size_t>(backend_index)] = {info, generation};
    return info;
}

std::shared_ptr<const AmsSystemInfo> AmsState::system_info(int backend_index) const {
    assert_main_thread();
    if (backend_index < 0) {
        return nullptr;
    }
    const size_t i = static_cast<size_t>(backend_index);
    if (i < system_info_snapshots_.size()) {
        const auto& snapshot = system_info_snapshots_[i];
        if (snapshot.info &&
            snapshot.generation == system_info_generation_.load(std::memory_order_acquire)) {
            return snapshot.info;
        }
    }
    return read_system_info(backend_index);
}

void AmsState::set_detail_env_unit(int unit) {
    assert_main_thread();
    detail_env_unit_ = unit;
    if (auto* backend = get_backend(0)) {
        const AmsSystemInfo info = backend->get_system_info();
        mirror_detail_env_subjects(backend, &info);
    } else {
        mirror_detail_env_subjects(nullptr, nullptr);
    }
}

void AmsState::mirror_detail_env_subjects(AmsBackend* backend, const AmsSystemInfo* info) {
    // Computed straight from the detail unit's data, so any unit index shows its own
    // readings regardless of the per-unit subject cap.
    UnitEnvIndicator e;
    if (backend && info) {
        for (const auto& unit : info->units) {
            if (unit.unit_index == detail_env_unit_) {
                e = compute_unit_env_indicator(backend, unit);
                break;
            }
        }
    }
    set_string_if_changed(&env_ind_detail_temp_text_, e.temp_text);
    set_string_if_changed(&env_ind_detail_humidity_text_, e.humidity_text);
    lv_subject_set_int(&env_ind_detail_humidity_, e.humidity_x10);
    lv_subject_set_int(&env_ind_detail_humidity_status_, e.humidity_status);
    lv_subject_set_int(&env_ind_detail_humidity_visible_, e.humidity_visible ? 1 : 0);
    lv_subject_set_int(&env_ind_detail_visible_, e.visible ? 1 : 0);
    lv_subject_set_int(&env_ind_detail_drying_active_, e.drying_active ? 1 : 0);
    if (e.drying_active)
        set_string_if_changed(&env_ind_detail_drying_text_, e.drying_text);
}

void AmsState::set_action_detail(const std::string& detail) {
    assert_main_thread();
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
[[maybe_unused]] static void ams_status_translation_hints_() {
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

bool AmsState::update_fault_cause() {
    const auto action = static_cast<AmsAction>(lv_subject_get_int(&ams_action_));
    const AmsBackend* backend = get_backend();
    const bool fault =
        action == AmsAction::ERROR || (backend && backend->current_error().has_value());
    if (!fault) {
        const bool had = !fault_cause_.empty();
        fault_cause_.clear();
        return had;
    }
    auto& print = get_printer_state().print_state();
    // RAW_PRINT_STATE_OK: the printer's own pause is what a filament fault
    // stops the print with.
    const bool paused = print.get_print_job_state() == PrintJobState::PAUSED;
    // The newest cause wins: a second fault while the first stands names its
    // own lane, and recovery acts on the lane the cause names.
    std::string cause =
        standing_fault_cause(paused, lv_subject_get_string(print.get_print_stop_reason_subject()),
                             recent_gcode_error(print, wall_now_s()));
    if (cause.empty() || cause == fault_cause_) {
        return false;
    }
    fault_cause_ = std::move(cause);
    return true;
}

void AmsState::refresh_fault_cause() {
    assert_main_thread();
    if (update_fault_cause()) {
        recompute_action_detail();
    }
}

std::optional<ErrorEvent> AmsState::standing_fault() const {
    const AmsBackend* backend = get_backend();
    return backend ? backend->current_error_with_cause(fault_cause_) : std::nullopt;
}

void AmsState::recompute_action_detail() {
    update_fault_cause();
    auto action = static_cast<AmsAction>(lv_subject_get_int(&ams_action_));

    // Priority:
    //   1. Toolchange narration label
    //   2. Action == ERROR with a fault_cause()
    //   3. Backend / UI-supplied operation_detail (non-empty)
    //   4. Action != IDLE → translated action string
    //   5. PrintJobState::PRINTING → "Printing"
    //   6. PrintJobState::PAUSED  → "Paused"
    //   7. Otherwise              → "Idle"
    //
    // Translation note (L067): the literals "Idle"/"Printing"/"Paused" and
    // the AmsAction strings are user-visible — translate at this UI binding
    // site, not in ams_action_to_string() which is also used for logs.
    const char* new_detail = "";
    if (!last_narration_label_.empty()) {
        // Live toolchange narration phase ("Brush nozzle") — finer-grained than
        // the AmsAction enum; wins until the operation ends (IDLE clears it).
        new_detail = last_narration_label_.c_str();
    } else if (action == AmsAction::ERROR && !fault_cause_.empty()) {
        // The backend's own text can belong to an earlier fault.
        new_detail = fault_cause_.c_str();
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
        auto print_state = get_printer_state().print_state().get_print_job_state();
        switch (print_state) {
        case PrintJobState::PRINTING:
            new_detail = lv_tr("Printing now");
            break;
        case PrintJobState::PAUSED:
            new_detail = lv_tr("Paused");
            break;
        default:
            new_detail = lv_tr("Idle");
            break;
        }
    }

    // sizeof(action_detail_buf_) - 1 leaves room for the NUL lv_subject_copy_string
    // writes; truncating here first (rather than letting it truncate) keeps the
    // cut on a codepoint boundary regardless of which producer overran it.
    const std::string bounded_detail(
        helix::text_io::utf8_prefix(new_detail, sizeof(action_detail_buf_) - 1));
    if (strcmp(lv_subject_get_string(&ams_action_detail_), bounded_detail.c_str()) != 0) {
        lv_subject_copy_string(&ams_action_detail_, bounded_detail.c_str());
    }
}

void AmsState::set_action(AmsAction action) {
    assert_main_thread();
    int val = static_cast<int>(action);
    if (lv_subject_get_int(&ams_action_) != val) {
        publish_action(action);
        spdlog::debug("[AMS State] Action set: {}", ams_action_to_string(action));
        // Action change must propagate to the displayed detail string (e.g.
        // LOADING → IDLE while still printing should flip "Loading" → "Printing").
        recompute_action_detail();
    }
}

void AmsState::publish_action(AmsAction action) {
    // Operation ended: clear the narration label + phase index before the
    // action observers run, so they and the next recompute see the cleared
    // state, and the next operation starts from a clean step bar.
    if (action == AmsAction::IDLE) {
        last_narration_label_.clear();
        narration_phase_id_.clear();
        narration_phase_high_water_ = -1;
        lv_subject_set_int(&toolchange_step_, -1);
    }
    lv_subject_set_int(&ams_action_, static_cast<int>(action));
    action_mirror_.store(action, std::memory_order_relaxed);
}

void AmsState::hold_optimistic_action(std::chrono::milliseconds budget) {
    assert_main_thread();
    optimistic_action_until_ = std::chrono::steady_clock::now() + budget;
}

void AmsState::release_optimistic_action() {
    assert_main_thread();
    optimistic_action_until_.reset();
}

bool AmsState::optimistic_action_held() const {
    assert_main_thread();
    return optimistic_action_until_.has_value();
}

void AmsState::set_active_step_operation(StepOperationType op) {
    assert_main_thread();
    const StepOperationType prev = active_step_operation_.exchange(op, std::memory_order_relaxed);
    if (prev == op) {
        return;
    }
    // Each operation kind has its own phase template, so an index carried
    // over from the previous one is not comparable with the new one's.
    // (The sidebar re-derives the operation whenever it (re)builds the step
    // bar, so this is also the mid-operation UNLOAD -> LOAD_SWAP upgrade, and
    // the first build of a bar for an operation that narrated before it.)
    narration_phase_high_water_ = -1;
    if (lv_subject_get_int(&toolchange_step_) >= 0) {
        lv_subject_set_int(&toolchange_step_, -1);
    }
    if (!narration_phase_id_.empty()) {
        apply_narration_phase_id(std::string(narration_phase_id_));
    }
}

void AmsState::apply_narration_phase_id(const std::string& id) {
    assert_main_thread();
    auto* backend = get_backend();
    if (!backend) {
        return;
    }
    const auto op = get_active_step_operation();
    const auto tmpl = backend->toolchange_phase_template(op);
    for (size_t k = 0; k < tmpl.size(); ++k) {
        if (tmpl[k].id == id) {
            spdlog::debug("[AMS State] Narration phase '{}' -> step {} ({})", id, k, tmpl[k].label);
            if (publish_narration_phase(static_cast<int>(k), tmpl[k].label)) {
                narration_phase_id_ = id;
            }
            return;
        }
    }
    // No index in this template: hold the id for the operation the step bar
    // settles on.
    spdlog::debug("[AMS State] Narration phase '{}' is not in op {}'s template; held", id,
                  static_cast<int>(op));
    narration_phase_id_ = id;
}

void AmsState::set_narration_phase(int index, const std::string& label) {
    assert_main_thread();
    if (publish_narration_phase(index, label)) {
        narration_phase_id_.clear();
    }
}

bool AmsState::publish_narration_phase(int index, const std::string& label) {
    // Firmware narration is not monotonic. AFC runs its wipe macro twice per
    // toolchange, once before and once after the kick (AFC.py
    // do_poop_kick_wipe(), v1.2.0:1390-1413; inline in TOOL_LOAD at
    // v1.1.0:1417-1440), and both emit at the shipped default verbosity. The
    // second one resolves to the same "brush" phase as the first, which sits
    // BEFORE "kick" in the template — publishing it verbatim rewinds the bar.
    //
    // Latch the highest index instead. Reset points are the places an index
    // stops being comparable with its predecessor:
    //   - index < 0        explicit clear (operation over / test baseline)
    //   - index == 0       the template's first phase narrated again, i.e. the
    //                      operation restarted from the top (an AFC retry after
    //                      a resumed error re-runs TOOL_LOAD from the heat)
    //   - action -> IDLE   operation ended (publish_action, from either the UI
    //                      or a backend sync)
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
        return false;
    } else {
        narration_phase_high_water_ = index;
    }

    lv_subject_set_int(&toolchange_step_, index);
    last_narration_label_ = label;
    recompute_action_detail();
    return true;
}

void AmsState::set_pending_target_slot(int slot) {
    async_lifetime_.defer("AmsState::set_pending_target_slot",
                          [this, slot]() { lv_subject_set_int(&pending_target_slot_, slot); });
}

void AmsState::set_active_tool_port_present(bool present) {
    // Marshal to the main thread — callable from the backend's WS status handler.
    async_lifetime_.defer("AmsState::set_active_tool_port_present", [this, present]() {
        int v = present ? 1 : 0;
        lv_subject_set_int(&active_tool_port_present_, v);
    });
}

bool AmsState::consume_post_unload_runout_grace() {
    return runout_grace_.consume();
}

bool AmsState::post_unload_runout_grace_armed() {
    return runout_grace_.armed();
}

bool AmsState::is_filament_operation_active() {
    // Called from the WebSocket thread; lv_subject_t is main-thread only.
    const auto action = action_mirror_.load(std::memory_order_relaxed);
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
    runout_grace_.mark_slot_unloaded(slot_index);
}

bool AmsState::was_slot_recently_unloaded(int slot_index) const {
    return runout_grace_.was_slot_recently_unloaded(slot_index);
}

void AmsState::set_current_loaded_defaults(bool write_header) {
    // The card is back to empty, so the next real load is a change worth logging.
    last_synced_loaded_slot_ = -1;
    last_synced_filament_loaded_ = false;

    if (strcmp(lv_subject_get_string(&current_material_text_), "---") != 0) {
        lv_subject_copy_string(&current_material_text_, "---");
    }
    const char* default_slot = lv_tr("Currently Loaded");
    if (write_header && strcmp(lv_subject_get_string(&current_slot_text_), default_slot) != 0) {
        lv_subject_copy_string(&current_slot_text_, default_slot);
    }
    if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
        lv_subject_copy_string(&current_weight_text_, "");
    }
    lv_subject_set_int(&current_has_weight_, 0);
    set_current_paint(0x505050);
    lv_subject_set_int(&current_fill_pct_, 0);
}

void AmsState::set_current_paint(uint32_t rgb, const std::string& multi_color_hexes,
                                 const std::string& finish_name) {
    if (strcmp(lv_subject_get_string(&current_colors_), multi_color_hexes.c_str()) != 0) {
        lv_subject_copy_string(&current_colors_, multi_color_hexes.c_str());
    }
    if (strcmp(lv_subject_get_string(&current_finish_), finish_name.c_str()) != 0) {
        lv_subject_copy_string(&current_finish_, finish_name.c_str());
    }
    lv_subject_set_int(&current_color_, static_cast<int>(rgb));
}

void AmsState::sync_current_loaded_from_backend() {
    assert_main_thread();

    if (registry_.count() == 0) {
        set_current_loaded_defaults();
        return;
    }

    if (const auto snapshot = read_system_info(0)) {
        sync_current_loaded_from_backend(*snapshot);
    }
}

void AmsState::set_current_slot_header(AmsBackend& backend, const AmsSystemInfo& sys,
                                       int slot_index) {
    char tmp[64];
    if (is_tool_changer(sys.type) && sys.units.empty()) {
        // Pure tool changer with no AMS units: show the physical toolhead position
        snprintf(tmp, sizeof(tmp), "%s",
                 helix::ui::lane_label(helix::ui::active_tool_noun(), slot_index).c_str());
    } else {
        std::string unit_display;
        for (const auto& unit : sys.units) {
            if (slot_index >= unit.first_slot_global_index &&
                slot_index < unit.first_slot_global_index + unit.slot_count) {
                // Prefer display_name, fall back to name, replace _ with spaces
                unit_display = !unit.display_name.empty() ? unit.display_name : unit.name;
                std::replace(unit_display.begin(), unit_display.end(), '_', ' ');
                break;
            }
        }
        const std::string slot_label = helix::ui::lane_label(backend.lane_noun(), slot_index);
        if (!unit_display.empty() && sys.units.size() > 1) {
            // Multi-unit: show unit name + slot label on one line
            snprintf(tmp, sizeof(tmp), "%s · %s", unit_display.c_str(), slot_label.c_str());
        } else {
            snprintf(tmp, sizeof(tmp), "%s", slot_label.c_str());
        }
    }
    if (strcmp(lv_subject_get_string(&current_slot_text_), tmp) != 0) {
        lv_subject_copy_string(&current_slot_text_, tmp);
    }
}

void AmsState::sync_current_loaded_from_backend(const AmsSystemInfo& primary_info) {
    assert_main_thread();

    if (registry_.count() == 0) {
        set_current_loaded_defaults();
        return;
    }

    // Search ALL backends to find the one with filament loaded.
    // In multi-backend setups (e.g., AMS_1 + AMS_2), only one backend
    // will have filament actively loaded at a time.
    // primary_info stands for backend 0; the others are read through system_info(),
    // and the *_hold pointers keep a secondary reading alive while *_info points at it.
    AmsBackend* loaded_backend = nullptr;
    const AmsSystemInfo* loaded_info = &primary_info;
    std::shared_ptr<const AmsSystemInfo> loaded_hold;
    int slot_index = -1;
    bool filament_loaded = false;
    // While a load/unload works a head, the header names THAT head. Only the
    // header: the card, filament_loaded and the Spoolman active spool describe
    // the carriage, so they read the loaded lane below. The backend owns the
    // classification (operation_working_slot); this side only formats it.
    AmsBackend* working_backend = nullptr;
    const AmsSystemInfo* working_info = nullptr;
    std::shared_ptr<const AmsSystemInfo> working_hold;
    int working_slot = -1;

    const auto& backends = registry_.backends();
    for (size_t idx = 0; idx < backends.size(); ++idx) {
        auto& b = backends[idx];
        if (!b)
            continue;
        std::shared_ptr<const AmsSystemInfo> secondary;
        if (idx != 0) {
            secondary = system_info(static_cast<int>(idx));
            if (!secondary)
                continue;
        }
        const AmsSystemInfo& info = (idx == 0) ? primary_info : *secondary;
        if (!working_backend && info.operation_working_slot >= 0) {
            working_backend = b.get();
            working_info = &info;
            working_hold = secondary;
            working_slot = info.operation_working_slot;
        }
        if (loaded_backend) {
            continue;
        }
        if (info.filament_loaded) {
            loaded_backend = b.get();
            loaded_info = &info;
            loaded_hold = secondary;
            slot_index = info.current_slot;
            filament_loaded = true;
            continue;
        }
        // Also check bypass on each backend
        if (info.current_slot == -2 && b->is_bypass_active()) {
            loaded_backend = b.get();
            loaded_info = &info;
            loaded_hold = secondary;
            slot_index = -2;
        }
    }

    // Fallback to primary backend for bypass check if no loaded backend found
    if (!loaded_backend) {
        loaded_backend = backends[0].get();
        if (loaded_backend) {
            slot_index = primary_info.current_slot;
            filament_loaded = primary_info.filament_loaded;
        }
    }

    if (!loaded_backend) {
        set_current_loaded_defaults();
        return;
    }

    // Every subject write below is guarded against a no-op, and the log line
    // has to be too: this runs once per backend status frame, several times a
    // second, and an unguarded line crowds every other subsystem out of the
    // debug-bundle ring.
    const bool loaded_identity_changed =
        slot_index != last_synced_loaded_slot_ || filament_loaded != last_synced_filament_loaded_;
    last_synced_loaded_slot_ = slot_index;
    last_synced_filament_loaded_ = filament_loaded;
    // Each load re-asserts its spool: the server's active spool may have been
    // cleared since this lane last loaded (a print's end, another client).
    if (loaded_identity_changed) {
        last_synced_spoolman_id_ = 0;
    }

    // Check for bypass mode (slot_index == -2)
    if (slot_index == -2 && loaded_backend->is_bypass_active()) {
        const char* bypass_text = lv_tr("Bypass");
        if (strcmp(lv_subject_get_string(&current_slot_text_), bypass_text) != 0) {
            lv_subject_copy_string(&current_slot_text_, bypass_text);
        }

        // Show actual spool info if external spool is assigned
        auto ext_spool = get_external_spool_info();
        if (ext_spool.has_value()) {
            const auto& ext = ext_spool.value();
            set_current_paint(ext.color_rgb, ext.multi_color_hexes,
                              filament_finish_name(ext.finish()));
            lv_subject_set_int(&current_fill_pct_, ext.display_fill_pct());

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
                lv_subject_set_int(&current_has_weight_, 1);
            } else {
                if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
                    lv_subject_copy_string(&current_weight_text_, "");
                }
                lv_subject_set_int(&current_has_weight_, 0);
            }
        } else {
            const char* ext_text = lv_tr("External");
            if (strcmp(lv_subject_get_string(&current_material_text_), ext_text) != 0) {
                lv_subject_copy_string(&current_material_text_, ext_text);
            }
            if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
                lv_subject_copy_string(&current_weight_text_, "");
            }
            lv_subject_set_int(&current_has_weight_, 0);
            set_current_paint(0x888888);
            lv_subject_set_int(&current_fill_pct_, -1);
        }
    } else if (slot_index >= 0 && filament_loaded) {
        // Filament is loaded - show slot info from the backend that has it loaded
        if (loaded_identity_changed) {
            spdlog::debug("[AmsState] sync_current_loaded: slot={}, filament_loaded=true",
                          slot_index);
        }
        SlotInfo slot_info = loaded_backend->get_slot_info(slot_index);

        // Sync Spoolman active spool when slot with spoolman_id is loaded,
        // unless something else switches it (the backend, as AFC does on tool
        // load, the printer's firmware, or its T<n> macros). A linked spool
        // means Spoolman is in use.
        const int spool = spool_to_activate(loaded_backend, slot_index);
        if (api_ && spool > 0 && spool != last_synced_spoolman_id_) {
            last_synced_spoolman_id_ = spool;
            spdlog::info("[AMS State] Setting active Spoolman spool to {} (slot {})", spool,
                         slot_index);
            api_->spoolman().set_active_spool(spool, []() {}, [](const MoonrakerError&) {});
        }

        // Set color
        set_current_paint(slot_info.color_rgb, slot_info.multi_color_hexes,
                          filament_finish_name(slot_info.finish()));
        lv_subject_set_int(&current_fill_pct_, slot_info.display_fill_pct());

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

        // The header is written once per sync: an operation's working head
        // below takes it, so the carriage slot does not flash in first.
        if (!working_backend) {
            set_current_slot_header(*loaded_backend, *loaded_info, slot_index);
        }

        // Remaining weight (Spoolman or backend) and measured length, either or both
        const std::string remaining = slot_info.remaining_summary();
        if (!remaining.empty()) {
            if (strcmp(lv_subject_get_string(&current_weight_text_), remaining.c_str()) != 0) {
                lv_subject_copy_string(&current_weight_text_, remaining.c_str());
            }
            lv_subject_set_int(&current_has_weight_, 1);
        } else {
            if (strcmp(lv_subject_get_string(&current_weight_text_), "") != 0) {
                lv_subject_copy_string(&current_weight_text_, "");
            }
            lv_subject_set_int(&current_has_weight_, 0);
        }
    } else {
        // No filament loaded - show empty state
        set_current_loaded_defaults(/*write_header=*/!working_backend);
    }

    if (working_backend) {
        set_current_slot_header(*working_backend, *working_info, working_slot);
    }

    spdlog::trace("[AMS State] Synced current loaded - slot={}, has_weight={}", slot_index,
                  lv_subject_get_int(&current_has_weight_));
}

// ============================================================================
// Slot edit commit (single authority for spool assignment changes)
// ============================================================================

void AmsState::clear_active_spool_if(int spool_id) {
    if (api_ == nullptr || spool_id <= 0) {
        return;
    }
    api_->spoolman().get_spoolman_status(
        [spool_id](bool /*connected*/, int active) {
            if (active != spool_id) {
                return;
            }
            helix::ui::queue_update("AmsState::clear_active_spool_if", [spool_id]() {
                if (ams_state_detail::shutting_down()) {
                    return;
                }
                IMoonrakerAPI* api = AmsState::instance().api_;
                if (api == nullptr) {
                    return;
                }
                api->spoolman().set_active_spool(
                    0, []() {},
                    [spool_id](const MoonrakerError& err) {
                        spdlog::warn("[AmsState] Failed to clear active spool {}: {}", spool_id,
                                     err.message);
                    });
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[AmsState] Spoolman status unavailable: {}", err.message);
        },
        /*silent=*/true);
}

bool AmsState::external_spool_feeds() const {
    const AmsBackend* primary = get_backend();
    if (primary == nullptr) {
        return true;
    }
    for (int i = 0; i < backend_count(); ++i) {
        const AmsBackend* b = get_backend(i);
        if (b == nullptr) {
            continue;
        }
        if (b->is_bypass_active()) {
            return true;
        }
        if (b->is_filament_loaded() && b->get_current_slot() >= 0) {
            return false;
        }
    }
    return true;
}

AmsError AmsState::commit_slot_edit(int slot_index, const SlotInfo& original,
                                    const SlotInfo& info) {
    assert_main_thread();
    AmsBackend* backend = get_backend();
    if (!backend) {
        return AmsError(AmsResult::NO_AMS_DETECTED, "no AMS backend",
                        lv_tr("Multi-Filament System not available"));
    }

    // S1 — server-side active spool (fire-and-forget, warn on failure). The
    // active spool is the one feeding the toolhead, so only an edit on the
    // loaded lane sets or clears it. It sets one unless the printer takes the
    // link itself (helix_sets_active_spool_on_edit). An unlink clears the
    // spool it unlinked whoever switches spools, since no firmware drops a
    // link Moonraker holds.
    if (api_ && backend->slot_is_actively_loaded(slot_index)) {
        if (info.spoolman_id > 0 && helix_sets_active_spool_on_edit(backend)) {
            api_->spoolman().set_active_spool(
                info.spoolman_id, []() {},
                [](const MoonrakerError& err) {
                    spdlog::warn("[AmsState] Failed to set active spool: {}", err.message);
                });
        } else if (info.spoolman_id == 0) {
            clear_active_spool_if(original.spoolman_id);
        }
    }

    // S6 — stale identity otherwise survives until a server 404
    if (original.spoolman_id > 0 && original.spoolman_id != info.spoolman_id) {
        SpoolmanManager::invalidate_identity(original.spoolman_id);
    }

    // S3 and the lane model: the backend slot write, with firmware gcode riding
    // inside it, and the user's declaration on the lane, through the same
    // method a test's edit runs.
    AmsError err = backend->commit_user_edit(slot_index, original, info);
    // A partly applied edit still changed what it applied, so it syncs; the
    // error still returns below, so the caller's toast still shows.
    if (!err.success() && !err.partially_applied) {
        return err;
    }

    // Where each tool owns its spool, ToolState holds the assignment and the
    // sync below copies it back onto a slot that names none, so an unlink has to
    // reach ToolState before that sync runs.
    if (original.spoolman_id > 0 && info.spoolman_id <= 0 && original.mapped_tool >= 0 &&
        backend->supports_per_tool_spool_assignment()) {
        ToolState::instance().clear_spool(original.mapped_tool);
    }

    // S4 + S7
    sync_from_backend();
    return err;
}

AmsState::SpoolLink AmsState::link_spool_to_slot(int slot_index, const SpoolInfo& spool) {
    assert_main_thread();
    SpoolLink link;
    if (AmsBackend* backend = get_backend()) {
        link.original = backend->get_slot_info(slot_index);
    }
    link.linked = link.original;
    apply_spool_to_slot(link.linked, spool);
    link.error = commit_slot_edit(slot_index, link.original, link.linked);
    return link;
}

// ============================================================================
// Tag-declared Spoolman links
// ============================================================================

void AmsState::sync_tag_spool_link(AmsBackend& backend, int slot_index, const SlotInfo& slot) {
    // A lane whose occupancy is unknown says nothing about an insert.
    const std::optional<bool> occupied = slot_status_reports_filament(slot.status);
    if (!occupied) {
        return;
    }
    const std::optional<helix::ams::TagReading> tag = backend.tag_reading(slot_index);
    helix::ams::TagSpoolSighting sighting;
    sighting.occupied = *occupied;
    sighting.tag_spool_id = tag ? tag->spoolman_id : std::nullopt;
    sighting.lane_spoolman_id = slot.spoolman_id;
    const std::optional<int> want = tag_link_tracker_.observe(slot_index, sighting);
    if (!want) {
        return;
    }
    const int spool_id = *want;
    if (!SettingsManager::instance().get_ams_tag_links_spoolman()) {
        tag_link_tracker_.settle(slot_index, spool_id);
        return;
    }
    // Offered again on a later frame, once Spoolman is reachable.
    if (!api_ || !get_printer_state().is_spoolman_available()) {
        return;
    }
    tag_link_tracker_.begin_lookup(slot_index, spool_id, slot.spoolman_id);
    spdlog::info("[AMS State] Slot {} tag names Spoolman spool {}; looking it up", slot_index,
                 spool_id);
    auto tok = async_lifetime_.token();
    api_->spoolman().get_spoolman_spool(
        spool_id,
        [tok, slot_index, spool_id](const std::optional<SpoolInfo>& spool) {
            tok.defer("AmsState::tag_spool_link", [slot_index, spool_id, spool]() {
                AmsState::instance().finish_tag_spool_link(slot_index, spool_id, spool);
            });
        },
        [tok, slot_index, spool_id](const MoonrakerError& err) {
            spdlog::warn("[AMS State] Slot {} tag names Spoolman spool {}, and the lookup "
                         "failed: {}",
                         slot_index, spool_id, err.message);
            tok.defer("AmsState::tag_spool_link_failed", [slot_index, spool_id]() {
                AmsState::instance().tag_link_tracker_.lookup_failed(slot_index, spool_id);
            });
        },
        /*silent=*/true);
}

void AmsState::finish_tag_spool_link(int slot_index, int spool_id,
                                     const std::optional<SpoolInfo>& spool) {
    assert_main_thread();
    // The tag changed or the lane emptied while the lookup ran.
    if (!tag_link_tracker_.wants(slot_index, spool_id)) {
        return;
    }
    const bool inserted = tag_link_tracker_.want_is_insert(slot_index);
    tag_link_tracker_.settle(slot_index, spool_id);
    AmsBackend* backend = get_backend();
    if (!backend) {
        return;
    }
    const std::string lane = helix::ui::lane_label(backend->lane_noun(), slot_index);
    if (!spool) {
        // A spool seated across a restart warned when it was put in.
        if (!inserted) {
            spdlog::info("[AMS State] Slot {} tag names Spoolman spool {}, which doesn't exist",
                         slot_index, spool_id);
            return;
        }
        NOTIFY_WARNING(lv_tr("{}: the tag names Spoolman spool #{}, which doesn't exist"), lane,
                       spool_id);
        return;
    }
    const SpoolLink link = link_spool_to_slot(slot_index, *spool);
    if (!link.error.success() && !link.error.partially_applied) {
        spdlog::warn("[AMS State] Slot {} could not be linked to Spoolman spool {} from its "
                     "tag: {}",
                     slot_index, spool_id, link.error.technical_msg);
        return;
    }
    NOTIFY_INFO(lv_tr("{} linked to Spoolman spool #{} from its tag"), lane, spool_id);
}

} // namespace helix
