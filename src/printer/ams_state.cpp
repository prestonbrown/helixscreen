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
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <unordered_map>

using namespace helix;

// Async callback data for thread-safe LVGL updates
namespace {

// Shutdown flag to prevent async callbacks from accessing destroyed singleton
static std::atomic<bool> s_shutdown_flag{false};

// Polling interval for Spoolman weight updates (30 seconds)
static constexpr uint32_t SPOOLMAN_POLL_INTERVAL_MS = 30000;

struct AsyncSyncData {
    int backend_index;
    bool full_sync;
    int slot_index; // Only used if full_sync == false
};

} // namespace

AmsState& AmsState::instance() {
    static AmsState instance;
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
    static const std::unordered_map<std::string, const char*> logo_map = {
        // AFC (Armored Turtle) - has its own logo
        {"afc", "A:assets/images/ams/afc_64.png"},
        {"box turtle", "A:assets/images/ams/box_turtle_64.png"},
        {"box_turtle", "A:assets/images/ams/box_turtle_64.png"},
        {"boxturtle", "A:assets/images/ams/box_turtle_64.png"},

        // Happy Hare - generic firmware, has its own logo
        {"happy hare", "A:assets/images/ams/happy_hare_64.png"},
        {"happy_hare", "A:assets/images/ams/happy_hare_64.png"},
        {"happyhare", "A:assets/images/ams/happy_hare_64.png"},

        // Specific hardware types (when detected or configured)
        {"ercf", "A:assets/images/ams/ercf_64.png"},
        {"3ms", "A:assets/images/ams/3ms_64.png"},
        {"tradrack", "A:assets/images/ams/tradrack_64.png"},
        {"mmx", "A:assets/images/ams/mmx_64.png"},
        {"night owl", "A:assets/images/ams/night_owl_64.png"},
        {"night_owl", "A:assets/images/ams/night_owl_64.png"},
        {"nightowl", "A:assets/images/ams/night_owl_64.png"},
        {"quattro box", "A:assets/images/ams/quattro_box_64.png"},
        {"quattro_box", "A:assets/images/ams/quattro_box_64.png"},
        {"quattrobox", "A:assets/images/ams/quattro_box_64.png"},
        {"btt vivid", "A:assets/images/ams/btt_vivid_64.png"},
        {"btt_vivid", "A:assets/images/ams/btt_vivid_64.png"},
        {"bttvivid", "A:assets/images/ams/btt_vivid_64.png"},
        {"vivid", "A:assets/images/ams/btt_vivid_64.png"},
        {"kms", "A:assets/images/ams/kms_64.png"},
    };

    auto it = logo_map.find(lower_name);
    if (it != logo_map.end()) {
        return it->second;
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

    // Clean up Spoolman poll timer if still active (check LVGL is initialized
    // to avoid crash during static destruction order issues)
    if (spoolman_poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(spoolman_poll_timer_);
        spoolman_poll_timer_ = nullptr;
    }

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
        return;
    }

    spdlog::trace("[AMS State] Initializing subjects");

    // Backend selector subjects
    INIT_SUBJECT_INT(backend_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(active_backend, 0, subjects_, register_xml);

    // System-level subjects
    INIT_SUBJECT_INT(ams_type, static_cast<int>(AmsType::NONE), subjects_, register_xml);
    INIT_SUBJECT_INT(ams_action, static_cast<int>(AmsAction::IDLE), subjects_, register_xml);
    INIT_SUBJECT_INT(current_slot, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(pending_target_slot, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_current_tool, -1, subjects_, register_xml);
    // These subjects need ams_ prefix for XML but member vars don't have it
    lv_subject_init_int(&filament_loaded_, 0);
    subjects_.register_subject(&filament_loaded_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_filament_loaded", &filament_loaded_);

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
    }

    lv_subject_init_int(&supports_bypass_, 0);
    subjects_.register_subject(&supports_bypass_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_supports_bypass", &supports_bypass_);
    INIT_SUBJECT_INT(ams_slot_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(slots_version, 0, subjects_, register_xml);

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

    // Logo uses pointer subject — bind_src expects a pointer to the path string buffer
    lv_subject_init_pointer(&ams_system_logo_, system_logo_buf_);
    subjects_.register_subject(&ams_system_logo_);
    if (register_xml)
        lv_xml_register_subject(nullptr, "ams_system_logo", &ams_system_logo_);

    INIT_SUBJECT_STRING(ams_current_tool_text, "---", subjects_, register_xml);

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
    }

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

    // Create observer for print state changes to auto-refresh Spoolman weights.
    // Refreshes when print starts, ends, or pauses to keep weight data current.
    using helix::ui::observe_int_sync;
    print_state_observer_ = observe_int_sync<AmsState>(
        get_printer_state().get_print_state_enum_subject(), this, [](AmsState* self, int state) {
            auto print_state = static_cast<PrintJobState>(state);
            // Refresh on: PRINTING (start), COMPLETE (end), PAUSED (pause/resume)
            if (print_state == PrintJobState::PRINTING || print_state == PrintJobState::COMPLETE ||
                print_state == PrintJobState::PAUSED) {
                spdlog::debug("[AmsState] Print state changed to {}, refreshing Spoolman weights",
                              static_cast<int>(print_state));
                self->refresh_spoolman_weights();
            }
        });

    initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "AmsState", []() { AmsState::instance().deinit_subjects(); });
}

void AmsState::deinit_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!initialized_) {
        return;
    }

    spdlog::trace("[AMS State] Deinitializing subjects");

    // Clear dangling API pointer — the MoonrakerAPI is destroyed during teardown
    // before AmsState re-initializes. Without this, sync_from_backend() would
    // dereference a freed pointer on the next init_subjects() cycle.
    api_ = nullptr;

    // Release cross-singleton observer — it observes a subject from PrinterState
    // which may already be destroyed during StaticSubjectRegistry::deinit_all()
    // reverse-order teardown. Using release() (not reset()) avoids dereferencing
    // a dangling subject pointer in lv_observer_remove(). [L073]
    print_state_observer_.release();

    // IMPORTANT: clear_backends() MUST precede subjects_.deinit_all() because
    // BackendSlotSubjects are managed outside SubjectManager for lifetime reasons
    clear_backends();

    // Use SubjectManager for automatic cleanup of all registered subjects
    subjects_.deinit_all();

    initialized_ = false;
    spdlog::trace("[AMS State] Subjects deinitialized");
}

void AmsState::init_backend_from_hardware(const helix::PrinterDiscovery& hardware,
                                          MoonrakerAPI* api, MoonrakerClient* client) {
    init_backends_from_hardware(hardware, api, client);
}

void AmsState::init_backends_from_hardware(const helix::PrinterDiscovery& hardware,
                                           MoonrakerAPI* api, MoonrakerClient* client) {
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

    // Stop all backends
    for (auto& b : backends_) {
        if (b) {
            b->stop();
        }
    }
    backends_.clear();

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

void AmsState::set_moonraker_api(MoonrakerAPI* api) {
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

void AmsState::BackendSlotSubjects::init(int count) {
    slot_count = count;
    colors.resize(count);
    statuses.resize(count);
    for (int i = 0; i < count; ++i) {
        lv_subject_init_int(&colors[i], static_cast<int>(AMS_DEFAULT_SLOT_COLOR));
        lv_subject_init_int(&statuses[i], static_cast<int>(SlotStatus::UNKNOWN));
    }
}

void AmsState::BackendSlotSubjects::deinit() {
    for (auto& c : colors)
        lv_subject_deinit(&c);
    for (auto& s : statuses)
        lv_subject_deinit(&s);
    colors.clear();
    statuses.clear();
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
    spdlog::debug("[AmsState] sync_from_backend: action={} ({})", static_cast<int>(info.action),
                  ams_action_to_string(info.action));
    int new_action = static_cast<int>(info.action);
    if (lv_subject_get_int(&ams_action_) != new_action) {
        lv_subject_set_int(&ams_action_, new_action);
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

    // Tool text formatting (ams_current_tool_text_) handled by UI-layer observer

    int new_loaded = info.filament_loaded ? 1 : 0;
    if (lv_subject_get_int(&filament_loaded_) != new_loaded) {
        lv_subject_set_int(&filament_loaded_, new_loaded);
    }
    int new_bypass = info.current_slot == -2 ? 1 : 0;
    if (lv_subject_get_int(&bypass_active_) != new_bypass) {
        lv_subject_set_int(&bypass_active_, new_bypass);
    }
    int new_supports_bypass = info.supports_bypass ? 1 : 0;
    if (lv_subject_get_int(&supports_bypass_) != new_supports_bypass) {
        lv_subject_set_int(&supports_bypass_, new_supports_bypass);
    }

    // Update external spool color from persistent settings
    auto ext_spool = helix::SettingsManager::instance().get_external_spool_info();
    int new_ext_color = ext_spool.has_value() ? static_cast<int>(ext_spool->color_rgb) : 0;
    if (lv_subject_get_int(&external_spool_color_) != new_ext_color) {
        lv_subject_set_int(&external_spool_color_, new_ext_color);
    }
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

    // Update action detail string
    const char* detail_str =
        !info.operation_detail.empty() ? info.operation_detail.c_str() : ams_action_to_string(info.action);
    if (strcmp(lv_subject_get_string(&ams_action_detail_), detail_str) != 0) {
        lv_subject_copy_string(&ams_action_detail_, detail_str);
    }

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
        }
    }

    // Sync spool assignments to ToolState for slots with mapped tools
    for (int i = 0; i < std::min(info.total_slots, MAX_SLOTS); ++i) {
        const SlotInfo* slot = info.get_slot_global(i);
        if (slot && slot->mapped_tool >= 0 && slot->spoolman_id > 0) {
            ToolState::instance().assign_spool(slot->mapped_tool, slot->spoolman_id,
                                               slot->spool_name, slot->remaining_weight_g,
                                               slot->total_weight_g);
        }
    }

    // For backends without firmware persistence, save after sync
    if (!backend->has_firmware_spool_persistence()) {
        ToolState::instance().save_spool_assignments_if_dirty(get_moonraker_api());
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
    }

    if (any_slot_changed) {
        spdlog::trace("[AmsState] Slot data changed, bumping version");
        bump_slots_version();
    }

    // Sync dryer state (for systems with integrated drying like ValgACE)
    sync_dryer_from_backend();

    // Sync clog detection meter subjects
    sync_clog_meter_from_info(info);

    // Sync "Currently Loaded" display subjects (pass info to avoid re-fetching)
    sync_current_loaded_from_backend(info);

    spdlog::debug("[AMS State] Synced from backend - type={}, slots={}, action={}, segment={}",
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

    // Use ui_queue_update to post updates to LVGL's main thread
    // This is required because backend events may come from background threads
    // and LVGL is not thread-safe

    // Helper to safely queue async call using RAII pattern
    auto queue_sync = [backend_index](bool full_sync, int slot_index) {
        auto sync_data =
            std::make_unique<AsyncSyncData>(AsyncSyncData{backend_index, full_sync, slot_index});
        helix::ui::queue_update<AsyncSyncData>(std::move(sync_data), [](AsyncSyncData* d) {
            // Skip if shutdown is in progress - AmsState singleton may be destroyed
            if (s_shutdown_flag.load(std::memory_order_acquire)) {
                return;
            }

            if (d->full_sync) {
                AmsState::instance().sync_backend(d->backend_index);
            } else {
                AmsState::instance().update_slot_for_backend(d->backend_index, d->slot_index);
            }
        });
    };

    if (event == AmsBackend::EVENT_STATE_CHANGED) {
        queue_sync(true, -1);
    } else if (event == AmsBackend::EVENT_SLOT_CHANGED) {
        // Parse slot index from data
        if (!data.empty()) {
            try {
                int slot_index = std::stoi(data);
                queue_sync(false, slot_index);
            } catch (...) {
                // Invalid data, do full sync
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

void AmsState::bump_slots_version() {
    int current = lv_subject_get_int(&slots_version_);
    lv_subject_set_int(&slots_version_, current + 1);
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

    DryerInfo dryer = backend->get_dryer_info();

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
    // Priority: flowguard > encoder > buffer_health > legacy > none
    // Source override: 0=auto (use priority), 1=encoder, 2=flowguard, 3=buffer
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
    bool has_buffer = false;
    for (const auto& unit : info.units) {
        if (unit.buffer_health && unit.buffer_health->fault_detection_enabled) {
            has_buffer = true;
            break;
        }
    }

    // Apply source override: skip to the forced source if available
    bool use_flowguard = has_flowguard;
    bool use_encoder = has_encoder;
    bool use_buffer = has_buffer;

    if (source_override_ == 1) {
        // Force encoder only
        use_flowguard = false;
        use_buffer = false;
    } else if (source_override_ == 2) {
        // Force flowguard only
        use_encoder = false;
        use_buffer = false;
    } else if (source_override_ == 3) {
        // Force buffer only
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
        snprintf(center_buf, sizeof(center_buf), "%+d%%", static_cast<int>(info.flowguard_info.level * 100));
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
        // Check buffer fault detection (buffer_health is per-unit, not per-slot)
        for (const auto& unit : info.units) {
            if (use_buffer && unit.buffer_health && unit.buffer_health->fault_detection_enabled) {
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

                snprintf(mode_text, sizeof(mode_text), "%s",
                         unit.buffer_health->state.c_str());

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

    spdlog::trace("[AMS State] Synced clog meter - mode={}, value={}, warning={}",
                  mode, value, warning);
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
    if (strcmp(lv_subject_get_string(&ams_action_detail_), detail.c_str()) != 0) {
        lv_subject_copy_string(&ams_action_detail_, detail.c_str());
        spdlog::debug("[AMS State] Action detail set: {}", detail);
    }
}

void AmsState::set_action(AmsAction action) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int val = static_cast<int>(action);
    if (lv_subject_get_int(&ams_action_) != val) {
        lv_subject_set_int(&ams_action_, val);
        spdlog::debug("[AMS State] Action set: {}", ams_action_to_string(action));
    }
}

void AmsState::set_pending_target_slot(int slot) {
    helix::ui::queue_update([this, slot]() {
        if (lv_subject_get_int(&pending_target_slot_) != slot) {
            lv_subject_set_int(&pending_target_slot_, slot);
        }
    });
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

            // Build label from spool info
            std::string color_label;
            if (ext.spoolman_id > 0 && !ext.color_name.empty()) {
                color_label = ext.color_name;
            } else {
                color_label = helix::get_color_name_from_hex(ext.color_rgb);
            }
            std::string label;
            if (!color_label.empty() && !ext.material.empty()) {
                label = color_label + " " + ext.material;
            } else if (!color_label.empty()) {
                label = color_label;
            } else if (!ext.material.empty()) {
                label = ext.material;
            } else {
                label = lv_tr("External");
            }
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

        // Build material label - color name + material (e.g., "Red PLA")
        // Use Spoolman color name if available, otherwise identify from hex
        {
            std::string color_label;
            if (slot_info.spoolman_id > 0 && !slot_info.color_name.empty()) {
                color_label = slot_info.color_name;
            } else {
                color_label = helix::get_color_name_from_hex(slot_info.color_rgb);
            }

            std::string label;
            if (!color_label.empty() && !slot_info.material.empty()) {
                label = color_label + " " + slot_info.material;
            } else if (!color_label.empty()) {
                label = color_label;
            } else if (!slot_info.material.empty()) {
                label = slot_info.material;
            } else {
                label = "Filament";
            }
            if (strcmp(lv_subject_get_string(&current_material_text_), label.c_str()) != 0) {
                lv_subject_copy_string(&current_material_text_, label.c_str());
            }
        }

        // Set slot label with unit name.
        // IMPORTANT: Use a local buffer for snprintf, NOT current_slot_text_buf_
        // (which IS the subject's internal buffer). Writing to the subject buffer
        // first makes strcmp(subject, buf) compare the buffer with itself → always
        // equal → lv_subject_copy_string never called → observers never notified.
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
                    snprintf(tmp, sizeof(tmp), lv_tr("Current: %s · Slot %d"),
                             unit_display.c_str(), display_slot);
                } else {
                    snprintf(tmp, sizeof(tmp), lv_tr("Current: Slot %d"), display_slot);
                }
            }
            if (strcmp(lv_subject_get_string(&current_slot_text_), tmp) != 0) {
                lv_subject_copy_string(&current_slot_text_, tmp);
            }
        }

        // Show remaining weight if available (from Spoolman or backend)
        // Use local buffer — same snprintf-to-subject-buffer bug as slot text above.
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
        DryerInfo dryer = backend->get_dryer_info();
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
        DryerInfo dryer = backend->get_dryer_info();
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
// Spoolman Weight Polling
// ============================================================================

void AmsState::refresh_spoolman_weights() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Mock backends use fake spoolman IDs that don't exist in real Spoolman
    if (get_runtime_config()->should_mock_ams()) {
        return;
    }

    if (!api_) {
        return;
    }

    auto* backend = get_backend(0);
    if (!backend) {
        return;
    }

    uint32_t now = lv_tick_get();

    // Circuit breaker: if open, check if backoff period has elapsed
    if (spoolman_cb_open_) {
        uint32_t elapsed = now - spoolman_cb_tripped_at_ms_;
        if (elapsed < SPOOLMAN_CB_BACKOFF_MS) {
            spdlog::trace("[AmsState] Spoolman circuit breaker open, {}ms remaining",
                          SPOOLMAN_CB_BACKOFF_MS - elapsed);
            return;
        }
        // Backoff elapsed — half-open: allow one probe request through
        spdlog::info("[AmsState] Spoolman circuit breaker half-open, probing...");
        spoolman_cb_open_ = false;
    }

    // Debounce: skip if called too recently
    if (spoolman_last_refresh_ms_ > 0) {
        uint32_t since_last = now - spoolman_last_refresh_ms_;
        if (since_last < SPOOLMAN_DEBOUNCE_MS) {
            spdlog::trace("[AmsState] Spoolman refresh debounced ({}ms since last)", since_last);
            return;
        }
    }
    spoolman_last_refresh_ms_ = now;

    // When the backend tracks weight locally (e.g., AFC decrements weight
    // via extruder position), we still need total_weight_g (initial weight)
    // from Spoolman — the backend only provides remaining weight.
    bool local_weight = backend->tracks_weight_locally();

    int slot_count = backend->get_system_info().total_slots;
    int linked_count = 0;

    for (int i = 0; i < slot_count; ++i) {
        SlotInfo slot = backend->get_slot_info(i);
        if (slot.spoolman_id > 0) {
            ++linked_count;
            int slot_index = i;
            int spoolman_id = slot.spoolman_id;

            api_->spoolman().get_spoolman_spool(
                spoolman_id,
                [slot_index, spoolman_id, local_weight](const std::optional<SpoolInfo>& spool_opt) {
                    if (!spool_opt.has_value()) {
                        spdlog::warn("[AmsState] Spoolman spool {} not found", spoolman_id);
                        return;
                    }

                    const SpoolInfo& spool = spool_opt.value();

                    // Data to pass to UI thread
                    struct WeightUpdate {
                        int slot_index;
                        int expected_spoolman_id; // To verify slot wasn't reassigned
                        float remaining_weight_g;
                        float total_weight_g;
                        bool local_weight; // Backend tracks remaining weight locally
                    };

                    auto update_data = std::make_unique<WeightUpdate>(WeightUpdate{
                        slot_index, spoolman_id, static_cast<float>(spool.remaining_weight_g),
                        static_cast<float>(spool.initial_weight_g), local_weight});

                    helix::ui::queue_update<WeightUpdate>(std::move(update_data), [](WeightUpdate*
                                                                                         d) {
                        // Skip if shutdown is in progress
                        if (s_shutdown_flag.load(std::memory_order_acquire)) {
                            return;
                        }

                        AmsState& state = AmsState::instance();
                        std::lock_guard<std::recursive_mutex> lock(state.mutex_);

                        // Success response — reset circuit breaker (on UI thread)
                        if (state.spoolman_consecutive_failures_ > 0) {
                            spdlog::info("[AmsState] Spoolman recovered after {} failures",
                                         state.spoolman_consecutive_failures_);
                        }
                        state.spoolman_consecutive_failures_ = 0;
                        state.spoolman_unavailable_notified_ = false;

                        auto* primary = state.get_backend(0);
                        if (!primary) {
                            return;
                        }

                        // Get current slot info and verify it wasn't reassigned
                        SlotInfo slot = primary->get_slot_info(d->slot_index);
                        if (slot.spoolman_id != d->expected_spoolman_id) {
                            spdlog::debug(
                                "[AmsState] Slot {} spoolman_id changed ({} -> {}), skipping stale "
                                "weight update",
                                d->slot_index, d->expected_spoolman_id, slot.spoolman_id);
                            return;
                        }

                        // When backend tracks weight locally, only update total_weight
                        // (initial weight from Spoolman). Preserve the backend's
                        // remaining_weight which is more accurate than Spoolman's.
                        float new_remaining =
                            d->local_weight ? slot.remaining_weight_g : d->remaining_weight_g;

                        // Skip update if weights haven't changed (avoids UI refresh cascade)
                        if (slot.remaining_weight_g == new_remaining &&
                            slot.total_weight_g == d->total_weight_g) {
                            spdlog::trace(
                                "[AmsState] Slot {} weights unchanged ({:.0f}g / {:.0f}g)",
                                d->slot_index, new_remaining, d->total_weight_g);
                            return;
                        }

                        // Update weights and set back.
                        // CRITICAL: persist=false prevents an infinite feedback loop.
                        // With persist=true, set_slot_info sends G-code to firmware
                        // (e.g., SET_WEIGHT for AFC, MMU_GATE_MAP for Happy Hare).
                        // Firmware then emits a status_update WebSocket event, which
                        // triggers sync_from_backend → refresh_spoolman_weights →
                        // set_slot_info again, ad infinitum. With 4 AFC lanes this
                        // fires 16+ G-code commands per cycle and saturates the CPU.
                        // Since these weights come FROM Spoolman (an external source),
                        // there's no need to write them back to firmware.
                        slot.remaining_weight_g = new_remaining;
                        slot.total_weight_g = d->total_weight_g;
                        primary->set_slot_info(d->slot_index, slot, /*persist=*/false);
                        state.bump_slots_version();

                        spdlog::debug("[AmsState] Updated slot {} weights: {:.0f}g / {:.0f}g{}",
                                      d->slot_index, new_remaining, d->total_weight_g,
                                      d->local_weight ? " (local remaining)" : "");
                    });
                },
                [spoolman_id](const MoonrakerError& err) {
                    spdlog::warn("[AmsState] Failed to fetch Spoolman spool {}: {}", spoolman_id,
                                 err.message);

                    // Track failure for circuit breaker (post to UI thread for
                    // thread-safe access to AmsState and ToastManager)
                    helix::ui::queue_update([]() {
                        if (s_shutdown_flag.load(std::memory_order_acquire)) {
                            return;
                        }

                        AmsState& state = AmsState::instance();
                        std::lock_guard<std::recursive_mutex> lock(state.mutex_);

                        state.spoolman_consecutive_failures_++;

                        if (state.spoolman_consecutive_failures_ >= SPOOLMAN_CB_FAILURE_THRESHOLD) {
                            state.spoolman_cb_open_ = true;
                            state.spoolman_cb_tripped_at_ms_ = lv_tick_get();
                            spdlog::warn(
                                "[AmsState] Spoolman circuit breaker OPEN after {} failures, "
                                "backing off {}s",
                                state.spoolman_consecutive_failures_,
                                SPOOLMAN_CB_BACKOFF_MS / 1000);

                            // Notify user once per outage
                            if (!state.spoolman_unavailable_notified_) {
                                state.spoolman_unavailable_notified_ = true;
                                // i18n: Spoolman is a product name, do not translate
                                ToastManager::instance().show(
                                    ToastSeverity::WARNING,
                                    lv_tr("Spoolman unavailable — filament weights may be stale"),
                                    6000);
                            }
                        }
                    });
                });
        }
    }

    if (linked_count > 0) {
        spdlog::trace("[AmsState] Refreshing Spoolman weights for {} linked slots", linked_count);
    }
}

void AmsState::reset_spoolman_circuit_breaker() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    spoolman_last_refresh_ms_ = 0;
    spoolman_consecutive_failures_ = 0;
    spoolman_cb_tripped_at_ms_ = 0;
    spoolman_cb_open_ = false;
    spoolman_unavailable_notified_ = false;
}

void AmsState::start_spoolman_polling() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    ++spoolman_poll_refcount_;
    spdlog::debug("[AmsState] Starting Spoolman polling (refcount: {})", spoolman_poll_refcount_);

    // Only create timer on first reference
    if (spoolman_poll_refcount_ == 1 && !spoolman_poll_timer_) {
        spoolman_poll_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* self = static_cast<AmsState*>(lv_timer_get_user_data(timer));
                self->refresh_spoolman_weights();
            },
            SPOOLMAN_POLL_INTERVAL_MS, this);

        // Also do an immediate refresh
        refresh_spoolman_weights();
    }
}

void AmsState::stop_spoolman_polling() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (spoolman_poll_refcount_ > 0) {
        --spoolman_poll_refcount_;
    }

    spdlog::debug("[AmsState] Stopping Spoolman polling (refcount: {})", spoolman_poll_refcount_);

    // Only delete timer when refcount reaches zero
    // Guard against LVGL already being deinitialized during shutdown
    if (spoolman_poll_refcount_ == 0 && spoolman_poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(spoolman_poll_timer_);
        spoolman_poll_timer_ = nullptr;
    }
}

// ============================================================================
// External Spool (delegates to SettingsManager for persistence)
// ============================================================================

std::optional<SlotInfo> AmsState::get_external_spool_info() const {
    return helix::SettingsManager::instance().get_external_spool_info();
}

void AmsState::set_external_spool_info(const SlotInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    helix::SettingsManager::instance().set_external_spool_info(info);
    int new_color = static_cast<int>(info.color_rgb);
    if (lv_subject_get_int(&external_spool_color_) != new_color) {
        lv_subject_set_int(&external_spool_color_, new_color);
    }
}

void AmsState::clear_external_spool_info() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    helix::SettingsManager::instance().clear_external_spool_info();
    if (lv_subject_get_int(&external_spool_color_) != 0) {
        lv_subject_set_int(&external_spool_color_, 0);
    }
}
