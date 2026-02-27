// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_mock.h"

#include "afc_defaults.h"
#include "filament_database.h"
#include "hh_defaults.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <thread>

// Sample filament colors for mock slots
namespace {
struct MockFilament {
    uint32_t color;
    const char* color_name;
    const char* material;
    const char* brand;
};

// Predefined sample filaments matching Spoolman mock spools 1-8 (moonraker_api_mock.cpp)
// IMPORTANT: Keep in sync with MoonrakerAPIMock::init_mock_spools()
constexpr MockFilament SAMPLE_FILAMENTS[] = {
    {0x1A1A2E, "Jet Black", "PLA", "Polymaker"},        // Spool #1
    {0x26DCD9, "Silk Blue", "Silk PLA", "eSUN"},        // Spool #2
    {0x00AEFF, "Pop Blue", "ASA", "Elegoo"},            // Spool #3
    {0xD20000, "Fire Engine Red", "ABS", "Flashforge"}, // Spool #4
    {0xF4E111, "Signal Yellow", "PETG", "Kingroon"},    // Spool #5
    {0xE8E8E8, "Clear", "TPU", "Overture"},             // Spool #6
    {0x8A949E, "Gray", "ASA", "Bambu Lab"},             // Spool #7
    {0xA2AAAD, "Grey", "PC", "Polymaker"},              // Spool #8
};
constexpr int NUM_SAMPLE_FILAMENTS = sizeof(SAMPLE_FILAMENTS) / sizeof(SAMPLE_FILAMENTS[0]);

// Timing constants for realistic mode (milliseconds at 1x speed)
// These values simulate real AMS/MMU timing behavior
constexpr int HEATING_BASE_MS = 3000;            // 3 seconds to heat nozzle
constexpr int CUTTING_BASE_MS = 2000;            // 2 seconds for filament cut
constexpr int PURGING_BASE_MS = 3000;            // 3 seconds for purge after load
constexpr int CHECKING_BASE_MS = 1500;           // 1.5 seconds for recovery check
constexpr int SELECTING_BASE_MS = 1000;          // 1 second for slot/tool selection
constexpr int SEGMENT_ANIMATION_BASE_MS = 15000; // 15 seconds for full segment animation

// Variance factors (±percentage) for natural timing variation
constexpr float HEATING_VARIANCE = 0.3f;    // ±30%
constexpr float TIP_VARIANCE = 0.2f;        // ±20%
constexpr float LOADING_VARIANCE = 0.2f;    // ±20%
constexpr float PURGING_VARIANCE = 0.2f;    // ±20%
constexpr float CHECKING_VARIANCE = 0.2f;   // ±20% (for recovery)
constexpr float SELECTING_VARIANCE = 0.15f; // ±15%
} // namespace

AmsBackendMock::AmsBackendMock(int slot_count) {
    // Clamp slot count to reasonable range
    slot_count = std::clamp(slot_count, 1, 16);

    // Initialize system info (non-slot metadata)
    system_info_.type = AmsType::HAPPY_HARE; // Mock as Happy Hare
    system_info_.type_name = "Happy Hare (Mock)";
    system_info_.version = "2.7.0-mock";
    system_info_.current_tool = -1;
    system_info_.current_slot = -1;
    system_info_.filament_loaded = false;
    system_info_.action = AmsAction::IDLE;
    system_info_.total_slots = slot_count;
    // Use shared AFC defaults for capabilities
    auto caps = helix::printer::afc_default_capabilities();
    system_info_.supports_endless_spool = caps.supports_endless_spool;
    system_info_.supports_tool_mapping = caps.supports_tool_mapping;
    system_info_.supports_bypass = caps.supports_bypass;
    system_info_.supports_purge = caps.supports_purge;
    system_info_.tip_method = caps.tip_method;
    system_info_.has_hardware_bypass_sensor = false; // Mock default: virtual toggle

    // Initialize registry with single unit
    std::vector<std::string> slot_names;
    slot_names.reserve(slot_count);
    for (int i = 0; i < slot_count; ++i) {
        slot_names.push_back(std::to_string(i));
    }
    slots_.initialize("Mock MMU", slot_names);

    // Populate slot data in registry
    for (int i = 0; i < slot_count; ++i) {
        auto* entry = slots_.get_mut(i);
        if (!entry)
            continue;

        entry->info.slot_index = i;
        entry->info.global_index = i;
        entry->info.status = SlotStatus::AVAILABLE;
        entry->info.mapped_tool = i; // Direct 1:1 mapping

        // Assign sample filament data (cycle through samples)
        const auto& sample = SAMPLE_FILAMENTS[i % NUM_SAMPLE_FILAMENTS];
        entry->info.color_rgb = sample.color;
        entry->info.color_name = sample.color_name;
        entry->info.material = sample.material;
        entry->info.brand = sample.brand;

        // Mock Spoolman data with dramatic fill level differences for demo
        entry->info.spoolman_id = i + 1;
        entry->info.spool_name = std::string(sample.color_name) + " " + sample.material;
        entry->info.total_weight_g = 1000.0f;
        static const float fill_levels[] = {1.0f, 0.75f, 0.40f, 0.10f, 0.90f, 0.50f, 0.25f, 0.05f};
        entry->info.remaining_weight_g = entry->info.total_weight_g * fill_levels[i % 8];

        // Temperature recommendations from filament database
        auto mat_info = filament::find_material(sample.material);
        if (mat_info) {
            entry->info.nozzle_temp_min = mat_info->nozzle_min;
            entry->info.nozzle_temp_max = mat_info->nozzle_max;
            entry->info.bed_temp = mat_info->bed_temp;
        } else {
            auto pla_info = filament::find_material("PLA");
            if (pla_info) {
                entry->info.nozzle_temp_min = pla_info->nozzle_min;
                entry->info.nozzle_temp_max = pla_info->nozzle_max;
                entry->info.bed_temp = pla_info->bed_temp;
            } else {
                entry->info.nozzle_temp_min = 190;
                entry->info.nozzle_temp_max = 220;
                entry->info.bed_temp = 60;
            }
        }
    }

    // Set tool mapping (1:1)
    std::vector<int> tool_map(slot_count);
    for (int i = 0; i < slot_count; ++i) {
        tool_map[i] = i;
    }
    slots_.set_tool_map(tool_map);

    // Unit-level metadata stored in system_info_ for overlay
    AmsUnit unit_meta;
    unit_meta.unit_index = 0;
    unit_meta.name = "Mock MMU";
    unit_meta.slot_count = slot_count;
    unit_meta.first_slot_global_index = 0;
    unit_meta.connected = true;
    unit_meta.firmware_version = "mock-1.0";
    unit_meta.has_encoder = true;
    unit_meta.has_toolhead_sensor = true;
    unit_meta.has_slot_sensors = true;
    system_info_.units.push_back(unit_meta);

    // Start with slot 0 loaded for realistic demo appearance
    if (slot_count > 0) {
        auto* entry = slots_.get_mut(0);
        if (entry) {
            entry->info.status = SlotStatus::LOADED;
        }
        system_info_.current_slot = 0;
        system_info_.current_tool = 0;
        system_info_.filament_loaded = true;
        filament_segment_ = PathSegment::NOZZLE;
    }

    // Make slot index 3 (4th slot) empty for realistic demo
    if (slot_count > 3) {
        auto* entry = slots_.get_mut(3);
        if (entry) {
            entry->info.status = SlotStatus::EMPTY;
        }
    }

    // Initialize default device sections/actions for Happy Hare mode (constructor default)
    mock_device_sections_ = helix::printer::hh_default_sections();
    mock_device_actions_ = helix::printer::hh_default_actions();

    spdlog::debug("[AmsBackendMock] Created with {} slots", slot_count);
}

AmsBackendMock::~AmsBackendMock() {
    // Signal shutdown and wait for all threads to finish
    shutdown_requested_ = true;
    dryer_stop_requested_ = true;
    shutdown_cv_.notify_all();
    wait_for_operation_thread();

    // Always join if joinable — the atomic flags are unreliable here because
    // threads clear them before exiting, but the thread object remains joinable.
    // Destroying a joinable std::thread calls std::terminate().
    if (scenario_thread_.joinable()) {
        scenario_thread_.join();
    }

    if (dryer_thread_.joinable()) {
        dryer_thread_.join();
    }

    // Don't call stop() - it would try to lock the mutex which may be invalid
    // during static destruction. The running_ flag doesn't matter at this point.
}

void AmsBackendMock::wait_for_operation_thread() {
    // Use atomic exchange to prevent double-join race condition
    // Only one caller can "win" the exchange and actually join
    if (operation_thread_running_.exchange(false)) {
        if (operation_thread_.joinable()) {
            operation_thread_.join();
        }
    }
}

AmsError AmsBackendMock::start() {
    bool should_emit = false;
    std::string scenario;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (running_) {
            return AmsErrorHelper::success();
        }

        running_ = true;
        should_emit = true;
        scenario = initial_state_scenario_;
        spdlog::debug("[AmsBackendMock] Started");
    }

    // Emit initial state event OUTSIDE the lock to avoid deadlock
    // (emit_event also acquires mutex_ to safely copy the callback)
    if (should_emit) {
        emit_event(EVENT_STATE_CHANGED);
    }

    // Apply deferred state scenario (requires running_ = true)
    if (!scenario.empty() && scenario != "idle") {
        if (scenario == "error") {
            inject_mock_errors();
            spdlog::info("[AMS Mock] Applied initial state scenario: error");
        } else if (scenario == "loading") {
            set_realistic_mode(true);
            // Schedule a load after a short delay so the UI has time to initialize
            scenario_thread_running_ = true;
            scenario_thread_ = std::thread([this]() {
                {
                    std::unique_lock<std::mutex> lk(shutdown_mutex_);
                    shutdown_cv_.wait_for(lk, std::chrono::milliseconds(500),
                                          [this] { return shutdown_requested_.load(); });
                }
                if (running_ && !shutdown_requested_) {
                    load_filament(1);
                }
                scenario_thread_running_ = false;
            });
            spdlog::info("[AMS Mock] Applied initial state scenario: loading");
        } else if (scenario == "bypass") {
            // Schedule bypass after a short delay so the UI has time to initialize
            scenario_thread_running_ = true;
            scenario_thread_ = std::thread([this]() {
                {
                    std::unique_lock<std::mutex> lk(shutdown_mutex_);
                    shutdown_cv_.wait_for(lk, std::chrono::milliseconds(500),
                                          [this] { return shutdown_requested_.load(); });
                }
                if (running_ && !shutdown_requested_) {
                    enable_bypass();
                }
                scenario_thread_running_ = false;
            });
            spdlog::info("[AMS Mock] Applied initial state scenario: bypass");
        }
    }

    return AmsErrorHelper::success();
}

void AmsBackendMock::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) {
        return;
    }

    running_ = false;
    // Note: Don't log here - this may be called during static destruction
    // when spdlog's logger has already been destroyed (causes SIGSEGV)
}

bool AmsBackendMock::is_running() const {
    return running_;
}

void AmsBackendMock::set_event_callback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

AmsSystemInfo AmsBackendMock::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!slots_.is_initialized()) {
        return system_info_;
    }

    // Build slot data from registry, then overlay non-slot metadata
    auto info = slots_.build_system_info();

    // Copy system-level fields not managed by registry
    info.type = system_info_.type;
    info.type_name = system_info_.type_name;
    info.version = system_info_.version;
    info.action = system_info_.action;
    info.operation_detail = system_info_.operation_detail;
    info.current_slot = system_info_.current_slot;
    info.current_tool = system_info_.current_tool;
    info.pending_target_slot = system_info_.pending_target_slot;
    info.current_toolchange = system_info_.current_toolchange;
    info.number_of_toolchanges = system_info_.number_of_toolchanges;
    info.filament_loaded = system_info_.filament_loaded;
    info.supports_endless_spool = system_info_.supports_endless_spool;
    info.supports_tool_mapping = system_info_.supports_tool_mapping;
    info.supports_bypass = system_info_.supports_bypass;
    info.has_hardware_bypass_sensor = system_info_.has_hardware_bypass_sensor;
    info.tip_method = system_info_.tip_method;
    info.supports_purge = system_info_.supports_purge;

    // Copy unit-level metadata not managed by registry
    for (size_t u = 0; u < info.units.size() && u < system_info_.units.size(); ++u) {
        info.units[u].name = system_info_.units[u].name;
        info.units[u].connected = system_info_.units[u].connected;
        info.units[u].has_hub_sensor = system_info_.units[u].has_hub_sensor;
        info.units[u].hub_sensor_triggered = system_info_.units[u].hub_sensor_triggered;
        info.units[u].buffer_health = system_info_.units[u].buffer_health;
        info.units[u].topology = system_info_.units[u].topology;
        info.units[u].hub_tool_label = system_info_.units[u].hub_tool_label;
        info.units[u].has_encoder = system_info_.units[u].has_encoder;
        info.units[u].has_toolhead_sensor = system_info_.units[u].has_toolhead_sensor;
        info.units[u].has_slot_sensors = system_info_.units[u].has_slot_sensors;
        info.units[u].firmware_version = system_info_.units[u].firmware_version;
    }

    return info;
}

AmsType AmsBackendMock::get_type() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.type; // Returns TOOL_CHANGER or HAPPY_HARE based on mode
}

SlotInfo AmsBackendMock::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto* entry = slots_.get(slot_index);
    if (entry) {
        return entry->info;
    }

    // Return empty slot info for invalid index
    SlotInfo empty;
    empty.slot_index = -1;
    empty.global_index = -1;
    return empty;
}

AmsAction AmsBackendMock::get_current_action() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.action;
}

int AmsBackendMock::get_current_tool() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_tool;
}

int AmsBackendMock::get_current_slot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_slot;
}

bool AmsBackendMock::is_filament_loaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.filament_loaded;
}

PathTopology AmsBackendMock::get_topology() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return topology_;
}

PathSegment AmsBackendMock::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return filament_segment_;
}

PathSegment AmsBackendMock::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if this is the active slot - return the current filament segment
    if (slot_index == system_info_.current_slot && system_info_.filament_loaded) {
        return filament_segment_;
    }

    // For non-active slots, check if filament is installed at the slot
    const auto* entry = slots_.get(slot_index);
    if (!entry) {
        return PathSegment::NONE;
    }

    if (entry->info.status != SlotStatus::AVAILABLE &&
        entry->info.status != SlotStatus::FROM_BUFFER) {
        return PathSegment::NONE;
    }

    // Determine filament position based on the slot's unit topology.
    // PARALLEL (tool changer): each lane has its own toolhead, filament loaded to nozzle.
    // HUB/LINEAR: non-active slots have filament sitting at prep sensor.
    bool is_parallel = tool_changer_mode_;
    if (!is_parallel) {
        const auto* unit = system_info_.get_unit_for_slot(slot_index);
        if (unit && unit->topology == PathTopology::PARALLEL) {
            is_parallel = true;
        }
    }

    return is_parallel ? PathSegment::NOZZLE : PathSegment::PREP;
}

PathSegment AmsBackendMock::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_segment_;
}

AmsError AmsBackendMock::load_filament(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        if (system_info_.action != AmsAction::IDLE) {
            return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
        }

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
        }

        const auto* entry = slots_.get(slot_index);
        if (!entry || entry->info.status == SlotStatus::EMPTY) {
            return AmsErrorHelper::slot_not_available(slot_index);
        }

        // Start loading
        system_info_.action = AmsAction::LOADING;
        system_info_.operation_detail = "Loading from slot " + std::to_string(slot_index);
        filament_segment_ = PathSegment::SPOOL; // Start at spool
        spdlog::info("[AmsBackendMock] Loading from slot {}", slot_index);
    }

    emit_event(EVENT_STATE_CHANGED);
    schedule_completion(AmsAction::LOADING, EVENT_LOAD_COMPLETE, slot_index);

    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::unload_filament() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        if (system_info_.action != AmsAction::IDLE) {
            return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
        }

        if (!system_info_.filament_loaded) {
            return AmsError(AmsResult::WRONG_STATE, "No filament loaded", "No filament to unload",
                            "Load filament first");
        }

        // Start unloading
        system_info_.action = AmsAction::UNLOADING;
        system_info_.operation_detail = "Unloading filament";
        filament_segment_ = PathSegment::NOZZLE; // Start at nozzle (working backwards)
        spdlog::info("[AmsBackendMock] Unloading filament");
    }

    emit_event(EVENT_STATE_CHANGED);
    schedule_completion(AmsAction::UNLOADING, EVENT_UNLOAD_COMPLETE);

    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::select_slot(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        if (system_info_.action != AmsAction::IDLE) {
            return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
        }

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
        }

        // Immediate selection (no filament movement)
        system_info_.current_slot = slot_index;
        spdlog::info("[AmsBackendMock] Selected slot {}", slot_index);
    }

    emit_event(EVENT_STATE_CHANGED);
    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::change_tool(int tool_number) {
    int target_slot = 0; // Captured inside lock, used after
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        if (system_info_.action != AmsAction::IDLE) {
            return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
        }

        int mapped_slot = slots_.slot_for_tool(tool_number);
        if (mapped_slot < 0) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "Select a valid tool");
        }

        // Start tool change (unload + load sequence)
        system_info_.action = AmsAction::UNLOADING; // Start with unload
        system_info_.operation_detail = "Tool change to T" + std::to_string(tool_number);
        target_slot = mapped_slot; // Capture while locked
        system_info_.pending_target_slot = target_slot;
        spdlog::info("[AmsBackendMock] Tool change to T{}", tool_number);
    }

    emit_event(EVENT_STATE_CHANGED);
    schedule_completion(AmsAction::LOADING, EVENT_TOOL_CHANGED, target_slot);

    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::recover() {
    bool use_realistic;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        use_realistic = realistic_mode_;

        if (!use_realistic) {
            // Simple mode: immediate recovery to IDLE
            system_info_.action = AmsAction::IDLE;
            system_info_.operation_detail.clear();
            error_segment_ = PathSegment::NONE;
            spdlog::info("[AmsBackendMock] Recovery complete (simple mode)");
        } else {
            // Realistic mode: schedule recovery sequence (ERROR → CHECKING → IDLE)
            spdlog::info("[AmsBackendMock] Starting recovery sequence (realistic mode)");
        }
    }

    if (!use_realistic) {
        emit_event(EVENT_STATE_CHANGED);
        return AmsErrorHelper::success();
    }

    // Realistic mode: run recovery sequence in background
    emit_event(EVENT_STATE_CHANGED);
    schedule_recovery_sequence();

    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        if (system_info_.action != AmsAction::IDLE) {
            return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
        }

        system_info_.action = AmsAction::RESETTING;
        system_info_.operation_detail = "Resetting system";
        spdlog::info("[AmsBackendMock] Resetting");
    }

    emit_event(EVENT_STATE_CHANGED);

    // Use schedule_completion for thread-safe operation
    // RESETTING action will be handled by the "else" branch which just waits and completes
    schedule_completion(AmsAction::RESETTING, EVENT_STATE_CHANGED);

    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (system_info_.action == AmsAction::IDLE) {
            return AmsErrorHelper::success(); // Nothing to cancel
        }

        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
        spdlog::info("[AmsBackendMock] Operation cancelled");
    }

    // Signal the operation thread to stop
    cancel_requested_ = true;
    shutdown_cv_.notify_all();

    emit_event(EVENT_STATE_CHANGED);
    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::reset_lane(int slot_index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
        }

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
        }

        // Clear error state and return slot to normal
        entry->info.error = std::nullopt;
        if (entry->info.status == SlotStatus::BLOCKED) {
            entry->info.status = SlotStatus::AVAILABLE;
        }

        spdlog::info("[AmsBackendMock] Reset lane {} - cleared error state", slot_index);
    }

    emit_event(EVENT_STATE_CHANGED);
    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::set_slot_info(int slot_index, const SlotInfo& info, bool /*persist*/) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!slots_.is_valid_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
        }

        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
        }

        // Update filament info
        entry->info.color_name = info.color_name;
        entry->info.color_rgb = info.color_rgb;
        entry->info.material = info.material;
        entry->info.brand = info.brand;
        entry->info.spoolman_id = info.spoolman_id;
        entry->info.spool_name = info.spool_name;
        entry->info.remaining_weight_g = info.remaining_weight_g;
        entry->info.total_weight_g = info.total_weight_g;
        entry->info.nozzle_temp_min = info.nozzle_temp_min;
        entry->info.nozzle_temp_max = info.nozzle_temp_max;
        entry->info.bed_temp = info.bed_temp;

        spdlog::trace("[AmsBackendMock] Updated slot {} info", slot_index);
    }

    // Emit event OUTSIDE the lock to avoid deadlock
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::set_tool_mapping(int tool_number, int slot_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get current tool map from registry
    auto current_info = slots_.build_system_info();
    if (tool_number < 0 || tool_number >= static_cast<int>(current_info.tool_to_slot_map.size())) {
        return AmsError(AmsResult::INVALID_TOOL,
                        "Tool " + std::to_string(tool_number) + " out of range",
                        "Invalid tool number", "");
    }

    if (!slots_.is_valid_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
    }

    // Update the tool map entry and re-apply
    // Mock allows multiple tools to map to same slot (lenient behavior)
    current_info.tool_to_slot_map[tool_number] = slot_index;
    slots_.set_tool_map(current_info.tool_to_slot_map);

    // Also update the target slot's mapped_tool
    auto* entry = slots_.get_mut(slot_index);
    if (entry) {
        entry->info.mapped_tool = tool_number;
    }

    spdlog::info("[AmsBackendMock] Mapped T{} to slot {}", tool_number, slot_index);
    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::enable_bypass() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        if (!system_info_.supports_bypass) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not supported",
                            "This system does not support bypass mode", "");
        }

        if (system_info_.action != AmsAction::IDLE) {
            return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
        }

        // Enable bypass mode: current_slot = -2 indicates bypass
        system_info_.current_slot = -2;
        system_info_.filament_loaded = true;
        filament_segment_ = PathSegment::NOZZLE;
        spdlog::info("[AmsBackendMock] Bypass mode enabled");
    }

    emit_event(EVENT_STATE_CHANGED);
    return AmsErrorHelper::success();
}

AmsError AmsBackendMock::disable_bypass() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        if (system_info_.current_slot != -2) {
            return AmsError(AmsResult::WRONG_STATE, "Bypass not active",
                            "Bypass mode is not currently active", "");
        }

        // Disable bypass mode
        system_info_.current_slot = -1;
        system_info_.filament_loaded = false;
        filament_segment_ = PathSegment::NONE;
        spdlog::info("[AmsBackendMock] Bypass mode disabled");
    }

    emit_event(EVENT_STATE_CHANGED);
    return AmsErrorHelper::success();
}

bool AmsBackendMock::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_slot == -2;
}

void AmsBackendMock::simulate_error(AmsResult error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::ERROR;
        system_info_.operation_detail = ams_result_to_string(error);

        // Infer error segment based on error type
        if (error == AmsResult::FILAMENT_JAM || error == AmsResult::ENCODER_ERROR) {
            error_segment_ = PathSegment::HUB; // Jam typically in selector/hub
        } else if (error == AmsResult::SENSOR_ERROR || error == AmsResult::LOAD_FAILED) {
            error_segment_ = PathSegment::TOOLHEAD; // Detection issues at toolhead
        } else if (error == AmsResult::SLOT_BLOCKED || error == AmsResult::SLOT_NOT_AVAILABLE) {
            error_segment_ = PathSegment::PREP; // Slot issues at prep/entry
        } else {
            error_segment_ = filament_segment_; // Error at current position
        }
    }

    emit_event(EVENT_ERROR, ams_result_to_string(error));
    emit_event(EVENT_STATE_CHANGED);
}

void AmsBackendMock::simulate_pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::PAUSED;
        system_info_.operation_detail = "User intervention required";
        spdlog::info("[AmsBackendMock] Simulated pause state");
    }

    emit_event(EVENT_STATE_CHANGED);
}

AmsError AmsBackendMock::resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return AmsErrorHelper::not_connected("Mock backend not started");
        }

        // Already idle - no-op success
        if (system_info_.action == AmsAction::IDLE) {
            return AmsErrorHelper::success();
        }

        // Can only resume from PAUSED state
        if (system_info_.action != AmsAction::PAUSED) {
            return AmsError(AmsResult::WRONG_STATE, "Cannot resume - not in PAUSED state",
                            "System is " + std::string(ams_action_to_string(system_info_.action)),
                            "Wait for current operation to complete or use cancel");
        }

        // Resume to IDLE
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
        spdlog::info("[AmsBackendMock] Resumed from pause");
    }

    emit_event(EVENT_STATE_CHANGED);
    return AmsErrorHelper::success();
}

void AmsBackendMock::set_operation_delay(int delay_ms) {
    operation_delay_ms_ = std::max(0, delay_ms);
}

void AmsBackendMock::force_slot_status(int slot_index, SlotStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* entry = slots_.get_mut(slot_index);
    if (entry) {
        entry->info.status = status;
        spdlog::debug("[AmsBackendMock] Forced slot {} status to {}", slot_index,
                      slot_status_to_string(status));
    }
}

void AmsBackendMock::set_slot_error(int slot_index, std::optional<SlotError> error) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* entry = slots_.get_mut(slot_index);
    if (entry) {
        entry->info.error = std::move(error);
        spdlog::debug("[AmsBackendMock] Slot {} error {}", slot_index,
                      entry->info.error.has_value() ? entry->info.error->message : "cleared");
    }
}

void AmsBackendMock::set_unit_buffer_health(int unit_index, std::optional<BufferHealth> health) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (unit_index >= 0 && unit_index < static_cast<int>(system_info_.units.size())) {
        system_info_.units[unit_index].buffer_health = std::move(health);
        spdlog::debug("[AmsBackendMock] Unit {} buffer health {}", unit_index,
                      system_info_.units[unit_index].buffer_health.has_value() ? "set" : "cleared");
    }
}

void AmsBackendMock::inject_mock_errors() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (int u = 0; u < slots_.unit_count(); ++u) {
        auto [first, end] = slots_.unit_slot_range(u);
        int last_slot = end - 1;

        // Add a lane ERROR on the last slot of each unit
        if (first < end) {
            auto* entry = slots_.get_mut(last_slot);
            if (entry) {
                SlotError err;
                err.message = fmt::format("Lane {} load failed", entry->info.slot_index + 1);
                err.severity = SlotError::ERROR;
                entry->info.error = err;
            }
        }

        // Add buffer health approaching fault on unit 0 (AFC only — TurtleNeck buffer)
        if (afc_mode_ && u == 0 && u < static_cast<int>(system_info_.units.size())) {
            BufferHealth health;
            health.fault_detection_enabled = true;
            health.state = "Trailing";
            health.distance_to_fault = 12.5f;
            system_info_.units[u].buffer_health = health;
        }
    }

    spdlog::info("[AmsBackendMock] Injected mock error states on {} units", slots_.unit_count());
}

void AmsBackendMock::set_has_hardware_bypass_sensor(bool has_sensor) {
    std::lock_guard<std::mutex> lock(mutex_);
    system_info_.has_hardware_bypass_sensor = has_sensor;
    spdlog::debug("[AmsBackendMock] Hardware bypass sensor set to {}", has_sensor);
}

void AmsBackendMock::set_toolchange_progress(int current, int total) {
    std::lock_guard<std::mutex> lock(mutex_);
    system_info_.current_toolchange = current;
    system_info_.number_of_toolchanges = total;
    spdlog::debug("[AmsBackendMock] Toolchange progress set to {}/{}", current, total);
}

void AmsBackendMock::set_dryer_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    dryer_enabled_ = enabled;

    // Initialize dryer state
    dryer_state_.supported = enabled;
    dryer_state_.active = false;
    dryer_state_.allows_during_print = true;
    dryer_state_.current_temp_c = 25.0f; // Room temperature
    dryer_state_.target_temp_c = 0.0f;
    dryer_state_.duration_min = 0;
    dryer_state_.remaining_min = 0;
    dryer_state_.fan_pct = 0;
    dryer_state_.min_temp_c = 35.0f;
    dryer_state_.max_temp_c = 70.0f;
    dryer_state_.max_duration_min = 720;
    dryer_state_.supports_fan_control = true;

    // Check for environment variable override of speed
    const char* speed_env = std::getenv("HELIX_MOCK_DRYER_SPEED");
    if (speed_env) {
        dryer_speed_x_ = std::max(1, std::atoi(speed_env));
        spdlog::info("[AmsBackendMock] Dryer speed override: {}x", dryer_speed_x_);
    }

    spdlog::info("[AmsBackendMock] Dryer simulation {}", enabled ? "enabled" : "disabled");
}

void AmsBackendMock::set_dryer_speed(int speed_x) {
    std::lock_guard<std::mutex> lock(mutex_);
    dryer_speed_x_ = std::max(1, speed_x);
    spdlog::info("[AmsBackendMock] Dryer speed set to {}x", dryer_speed_x_);
}

DryerInfo AmsBackendMock::get_dryer_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dryer_state_;
}

AmsError AmsBackendMock::start_drying(float temp_c, int duration_min, int fan_pct) {
    spdlog::info("[AmsBackendMock] start_drying: {}°C for {}min, fan {}%", temp_c, duration_min,
                 fan_pct);

    int speed_x;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dryer_enabled_) {
            return AmsError{AmsResult::NOT_SUPPORTED, "Dryer not available"};
        }

        // Stop any existing dryer thread
        dryer_stop_requested_ = true;
        speed_x = dryer_speed_x_;
    }

    // Wait for previous thread to finish using atomic exchange
    if (dryer_thread_running_.exchange(false)) {
        if (dryer_thread_.joinable()) {
            dryer_thread_.join();
        }
    }

    float start_temp;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dryer_stop_requested_ = false;

        // Set initial dryer state
        dryer_state_.active = true;
        dryer_state_.target_temp_c = temp_c;
        dryer_state_.duration_min = duration_min;
        dryer_state_.remaining_min = duration_min;
        dryer_state_.fan_pct = (fan_pct >= 0) ? fan_pct : 50;
        start_temp = dryer_state_.current_temp_c; // Use current temp as starting point
    }

    // Mark dryer thread as running BEFORE creating it
    dryer_thread_running_ = true;

    // Start simulation thread
    // speed_x: how many simulated seconds pass per real second
    // At default 60x: 1 real second = 1 simulated minute, so 4h completes in 4min
    dryer_thread_ = std::thread([this, temp_c, duration_min, speed_x, start_temp]() {
        float current_temp = start_temp;
        int total_sec = duration_min * 60; // Total simulated seconds
        int elapsed_sim_sec = 0;

        // Update interval: 100ms real time, but simulate speed_x/10 seconds
        // With speed_x=60: each 100ms tick = 6 simulated seconds
        const int tick_ms = 100;
        const int sim_sec_per_tick = std::max(1, speed_x / 10);

        // Temperature ramping: reach 95% of target in first 5 minutes (300 sim sec)
        // Then hold at target. Typical heater behavior.
        const int ramp_time_sec = 300; // 5 minutes to reach target

        spdlog::debug("[AmsBackendMock] Dryer starting: target={}°C, duration={}min, speed={}x",
                      temp_c, duration_min, speed_x);

        while (!dryer_stop_requested_ && elapsed_sim_sec < total_sec) {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
            elapsed_sim_sec += sim_sec_per_tick;

            // Temperature simulation: exponential approach to target
            // During ramp phase: aggressive heating
            // After ramp: maintain at target with small fluctuations
            float temp_diff = temp_c - current_temp;
            if (elapsed_sim_sec < ramp_time_sec) {
                // Ramp phase: approach target quickly (time constant ~60 sec)
                current_temp += temp_diff * 0.05f * sim_sec_per_tick;
            } else {
                // Holding phase: maintain with minor fluctuation
                current_temp = temp_c + (static_cast<float>(std::rand() % 100) / 100.0f - 0.5f);
            }

            // Clamp to valid range
            current_temp = std::max(25.0f, std::min(current_temp, temp_c + 1.0f));

            int remaining_min = std::max(0, (total_sec - elapsed_sim_sec) / 60);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                dryer_state_.current_temp_c = current_temp;
                dryer_state_.remaining_min = remaining_min;
            }

            // Emit state change every tick for smooth UI updates
            emit_event(EVENT_STATE_CHANGED);
        }

        // Drying complete or stopped - simulate cool-down
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dryer_state_.active = false;
            dryer_state_.target_temp_c = 0.0f;
            dryer_state_.remaining_min = 0;
            dryer_state_.fan_pct = 0;
            // Start cooling from current temp (not instant)
        }
        emit_event(EVENT_STATE_CHANGED);

        // Quick cool-down simulation (10 ticks)
        for (int i = 0; i < 10 && !dryer_stop_requested_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
            float room_temp = 25.0f;
            current_temp = current_temp * 0.8f + room_temp * 0.2f; // Cool towards room temp
            {
                std::lock_guard<std::mutex> lock(mutex_);
                dryer_state_.current_temp_c = current_temp;
            }
            emit_event(EVENT_STATE_CHANGED);
        }

        // Final room temp (skip if shutting down)
        if (!dryer_stop_requested_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                dryer_state_.current_temp_c = 25.0f;
            }
            emit_event(EVENT_STATE_CHANGED);
            spdlog::info("[AmsBackendMock] Drying complete/stopped, cooled to room temp");
        }
    });

    emit_event(EVENT_STATE_CHANGED);
    return AmsError{AmsResult::SUCCESS};
}

AmsError AmsBackendMock::stop_drying() {
    spdlog::info("[AmsBackendMock] stop_drying");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dryer_enabled_) {
            return AmsError{AmsResult::NOT_SUPPORTED, "Dryer not available"};
        }

        if (!dryer_state_.active) {
            return AmsError{AmsResult::SUCCESS}; // Already stopped
        }

        dryer_stop_requested_ = true;
    }

    // Wait for thread to finish using atomic exchange
    if (dryer_thread_running_.exchange(false)) {
        if (dryer_thread_.joinable()) {
            dryer_thread_.join();
        }
    }

    return AmsError{AmsResult::SUCCESS};
}

// ============================================================================
// Realistic mode implementation
// ============================================================================

void AmsBackendMock::set_realistic_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    realistic_mode_ = enabled;
    spdlog::info("[AmsBackendMock] Realistic mode {}", enabled ? "enabled" : "disabled");
}

bool AmsBackendMock::is_realistic_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return realistic_mode_;
}

void AmsBackendMock::set_tool_changer_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    tool_changer_mode_ = enabled;

    if (enabled) {
        // Configure mock as a tool changer
        system_info_.type = AmsType::TOOL_CHANGER;
        system_info_.type_name = "Tool Changer (Mock)";
        system_info_.supports_bypass = false; // Not applicable for tool changers

        // Use parallel topology (each slot → own toolhead)
        topology_ = PathTopology::PARALLEL;

        // Rename unit to reflect tool changer nature
        if (!system_info_.units.empty()) {
            system_info_.units[0].name = "Mock Tool Changer";
        }

        // Tool changers have no AMS-style device settings
        mock_device_sections_.clear();
        mock_device_actions_.clear();

        spdlog::info("[AmsBackendMock] Tool changer mode enabled ({} tools)", slots_.slot_count());
    } else {
        // Revert to filament system (Happy Hare)
        system_info_.type = AmsType::HAPPY_HARE;
        system_info_.type_name = "Happy Hare (Mock)";
        system_info_.supports_bypass = true;
        topology_ = PathTopology::LINEAR;

        if (!system_info_.units.empty()) {
            system_info_.units[0].name = "Mock MMU";
        }

        // Restore Happy Hare device sections and actions
        mock_device_sections_ = helix::printer::hh_default_sections();
        mock_device_actions_ = helix::printer::hh_default_actions();

        spdlog::info("[AmsBackendMock] Tool changer mode disabled, reverting to filament system");
    }
}

bool AmsBackendMock::is_tool_changer_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tool_changer_mode_;
}

void AmsBackendMock::set_afc_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    afc_mode_ = enabled;

    if (enabled) {
        // Disable conflicting mode
        tool_changer_mode_ = false;

        // Respect gate count from constructor (via HELIX_AMS_GATES)
        int lane_count = system_info_.total_slots;
        if (lane_count <= 0)
            lane_count = 4;

        // Configure system info for AFC Box Turtle
        system_info_.type = AmsType::AFC;
        system_info_.type_name = "AFC (Mock)";
        system_info_.version = "1.0.32-mock";
        system_info_.total_slots = lane_count;

        // Use shared AFC defaults for capabilities
        auto afc_caps = helix::printer::afc_default_capabilities();
        system_info_.supports_endless_spool = afc_caps.supports_endless_spool;
        system_info_.supports_tool_mapping = afc_caps.supports_tool_mapping;
        system_info_.supports_bypass = afc_caps.supports_bypass;
        system_info_.supports_purge = afc_caps.supports_purge;
        system_info_.tip_method = afc_caps.tip_method;
        system_info_.has_hardware_bypass_sensor = false;

        // HUB topology, single unit
        topology_ = PathTopology::HUB;

        // Reinitialize registry with requested lane count
        slots_.clear();
        std::vector<std::string> slot_names;
        slot_names.reserve(lane_count);
        for (int i = 0; i < lane_count; ++i) {
            slot_names.push_back(std::to_string(i));
        }
        slots_.initialize("Box Turtle (Mock)", slot_names);

        // Populate slot data (cycles through sample data for any lane count)
        struct SlotData {
            const char* material;
            const char* brand;
            uint32_t color;
            const char* color_name;
            SlotStatus status;
            int spoolman_id;
            const char* spool_name;
            float remaining;
        };
        const SlotData sample_data[] = {
            {"ASA", "Bambu Lab", 0x000000, "Black", SlotStatus::LOADED, 1, "Black ASA", 750.0f},
            {"PLA", "Polymaker", 0xFF0000, "Red", SlotStatus::AVAILABLE, 2, "Red PLA", 900.0f},
            {"PETG", "eSUN", 0x00FF00, "Green", SlotStatus::AVAILABLE, 3, "Green PETG", 500.0f},
            {"TPU", "eSUN", 0xFF6600, "Orange", SlotStatus::AVAILABLE, 0, "", 200.0f},
            {"ABS", "Hatchbox", 0x0000FF, "Blue", SlotStatus::AVAILABLE, 4, "Blue ABS", 600.0f},
            {"PLA", "Prusament", 0xFFFF00, "Yellow", SlotStatus::AVAILABLE, 5, "Yellow PLA",
             850.0f},
            {"PETG", "Overture", 0xFF00FF, "Purple", SlotStatus::AVAILABLE, 6, "Purple PETG",
             400.0f},
            {"ASA", "KVP", 0x00FFFF, "Cyan", SlotStatus::AVAILABLE, 7, "Cyan ASA", 700.0f},
        };
        constexpr int sample_count = sizeof(sample_data) / sizeof(sample_data[0]);

        for (int i = 0; i < lane_count; ++i) {
            auto* entry = slots_.get_mut(i);
            if (!entry)
                continue;
            const auto& d = sample_data[i % sample_count];
            entry->info.slot_index = i;
            entry->info.global_index = i;
            entry->info.material = d.material;
            entry->info.brand = d.brand;
            entry->info.color_rgb = d.color;
            entry->info.color_name = d.color_name;
            entry->info.status = (i == 0) ? SlotStatus::LOADED : d.status;
            entry->info.spoolman_id = d.spoolman_id;
            entry->info.spool_name = d.spool_name;
            entry->info.total_weight_g = 1000.0f;
            entry->info.remaining_weight_g = d.remaining;
            auto mat_info = filament::find_material(d.material);
            if (mat_info) {
                entry->info.nozzle_temp_min = mat_info->nozzle_min;
                entry->info.nozzle_temp_max = mat_info->nozzle_max;
                entry->info.bed_temp = mat_info->bed_temp;
            }
        }

        // Tool-to-slot mapping: 1:1
        std::vector<int> tool_map(lane_count);
        for (int i = 0; i < lane_count; ++i) {
            tool_map[i] = i;
        }
        slots_.set_tool_map(tool_map);

        // Unit-level metadata
        system_info_.units.clear();
        AmsUnit unit_meta;
        unit_meta.unit_index = 0;
        unit_meta.name = "Box Turtle (Mock)";
        unit_meta.slot_count = lane_count;
        unit_meta.first_slot_global_index = 0;
        unit_meta.connected = true;
        unit_meta.firmware_version = "1.0.32-mock";
        unit_meta.has_encoder = false;
        unit_meta.has_toolhead_sensor = true;
        unit_meta.has_slot_sensors = true;
        system_info_.units.push_back(unit_meta);

        // Start with lane 0 loaded
        system_info_.current_slot = 0;
        system_info_.current_tool = 0;
        system_info_.filament_loaded = true;
        filament_segment_ = PathSegment::NOZZLE;

        // Simulate mid-print tool change progress
        system_info_.current_toolchange = 2;
        system_info_.number_of_toolchanges = lane_count + 1;

        // AFC device sections and actions — use all defaults
        mock_device_sections_ = helix::printer::afc_default_sections();
        mock_device_actions_ = helix::printer::afc_default_actions();

        // Disable save_restart in mock mode (no real Klipper to restart)
        for (auto& action : mock_device_actions_) {
            if (action.id == "save_restart") {
                action.enabled = false;
                action.disable_reason = "Not available in mock mode";
            }
        }

        spdlog::info("[AmsBackendMock] AFC mode enabled ({}-lane Box Turtle)", lane_count);
    } else {
        // Revert to Happy Hare defaults
        afc_mode_ = false;
        system_info_.type = AmsType::HAPPY_HARE;
        system_info_.type_name = "Happy Hare (Mock)";
        system_info_.version = "2.7.0-mock";
        system_info_.supports_bypass = true;
        topology_ = PathTopology::LINEAR;

        if (!system_info_.units.empty()) {
            system_info_.units[0].name = "Mock MMU";
        }

        // Restore Happy Hare device sections and actions
        mock_device_sections_ = helix::printer::hh_default_sections();
        mock_device_actions_ = helix::printer::hh_default_actions();

        spdlog::info("[AmsBackendMock] AFC mode disabled, reverting to Happy Hare");
    }
}

bool AmsBackendMock::is_afc_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return afc_mode_;
}

void AmsBackendMock::set_multi_unit_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    multi_unit_mode_ = enabled;

    if (enabled) {
        // Disable conflicting modes
        tool_changer_mode_ = false;

        // Configure as AFC with 2 units
        system_info_.type = AmsType::AFC;
        system_info_.type_name = "AFC (Mock Multi-Unit)";
        system_info_.version = "1.0.32-mock";
        system_info_.supports_bypass = true;
        system_info_.supports_endless_spool = true;
        system_info_.supports_tool_mapping = true;
        system_info_.has_hardware_bypass_sensor = false;
        system_info_.tip_method = TipMethod::CUT;
        system_info_.supports_purge = true;
        system_info_.total_slots = 6;
        topology_ = PathTopology::HUB;

        // Reinitialize registry with 2 units
        slots_.clear();
        slots_.initialize_units({
            {"Box Turtle 1", {"0", "1", "2", "3"}},
            {"Night Owl", {"0", "1"}},
        });

        // Unit 0 slot data
        struct SlotData {
            const char* material;
            const char* brand;
            uint32_t color;
            const char* color_name;
            SlotStatus status;
            int spoolman_id;
            float remaining;
        };
        const SlotData bt_slots[] = {
            {"ASA", "Bambu Lab", 0x000000, "Black", SlotStatus::LOADED, 100, 1000.0f},
            {"PLA", "Polymaker", 0xFF0000, "Red", SlotStatus::AVAILABLE, 101, 750.0f},
            {"PETG", "eSUN", 0x00FF00, "Green", SlotStatus::AVAILABLE, 102, 500.0f},
            {"PLA", "Overture", 0xFFFFFF, "White", SlotStatus::EMPTY, 103, 0.0f},
        };
        for (int i = 0; i < 4; ++i) {
            auto* entry = slots_.get_mut(i);
            if (!entry)
                continue;
            const auto& d = bt_slots[i];
            entry->info.slot_index = i;
            entry->info.global_index = i;
            entry->info.material = d.material;
            entry->info.brand = d.brand;
            entry->info.color_rgb = d.color;
            entry->info.color_name = d.color_name;
            entry->info.status = d.status;
            entry->info.mapped_tool = 0; // All slots share single toolhead
            entry->info.spoolman_id = d.spoolman_id;
            entry->info.total_weight_g = 1000.0f;
            entry->info.remaining_weight_g = d.remaining;
            auto mat_info = filament::find_material(d.material);
            if (mat_info) {
                entry->info.nozzle_temp_min = mat_info->nozzle_min;
                entry->info.nozzle_temp_max = mat_info->nozzle_max;
                entry->info.bed_temp = mat_info->bed_temp;
            }
        }

        // Unit 1 slot data (Night Owl, 2 lanes)
        const SlotData no_slots[] = {
            {"PETG", "Prusa", 0x1E88E5, "Blue", SlotStatus::AVAILABLE, 200, 1000.0f},
            {"ABS", "Bambu Lab", 0xFDD835, "Yellow", SlotStatus::AVAILABLE, 201, 800.0f},
        };
        for (int i = 0; i < 2; ++i) {
            auto* entry = slots_.get_mut(4 + i);
            if (!entry)
                continue;
            const auto& d = no_slots[i];
            entry->info.slot_index = i;
            entry->info.global_index = 4 + i;
            entry->info.material = d.material;
            entry->info.brand = d.brand;
            entry->info.color_rgb = d.color;
            entry->info.color_name = d.color_name;
            entry->info.status = d.status;
            entry->info.mapped_tool = 0; // All slots share single toolhead
            entry->info.spoolman_id = d.spoolman_id;
            entry->info.total_weight_g = 1000.0f;
            entry->info.remaining_weight_g = d.remaining;
            auto mat_info = filament::find_material(d.material);
            if (mat_info) {
                entry->info.nozzle_temp_min = mat_info->nozzle_min;
                entry->info.nozzle_temp_max = mat_info->nozzle_max;
                entry->info.bed_temp = mat_info->bed_temp;
            }
        }

        // Single toolhead — T0 maps to currently loaded slot
        slots_.set_tool_map({0});

        // Unit-level metadata
        system_info_.units.clear();
        {
            AmsUnit u;
            u.unit_index = 0;
            u.name = "Box Turtle 1";
            u.slot_count = 4;
            u.first_slot_global_index = 0;
            u.connected = true;
            u.firmware_version = "1.0.32-mock";
            u.has_encoder = false;
            u.has_toolhead_sensor = true;
            u.has_slot_sensors = true;
            u.has_hub_sensor = true;
            u.hub_sensor_triggered = true;
            system_info_.units.push_back(u);
        }
        {
            AmsUnit u;
            u.unit_index = 1;
            u.name = "Night Owl";
            u.slot_count = 2;
            u.first_slot_global_index = 4;
            u.connected = true;
            u.firmware_version = "2.1.0-mock";
            u.has_encoder = true;
            u.has_toolhead_sensor = true;
            u.has_slot_sensors = true;
            u.has_hub_sensor = true;
            u.hub_sensor_triggered = false;
            system_info_.units.push_back(u);
        }

        // Start with slot 0 loaded
        system_info_.current_slot = 0;
        system_info_.current_tool = 0;
        system_info_.filament_loaded = true;
        filament_segment_ = PathSegment::NOZZLE;

        spdlog::info("[AmsBackendMock] Multi-unit mode: Box Turtle (4) + Night Owl (2) = 6 slots");
    } else {
        multi_unit_mode_ = false;
        // Revert to Happy Hare defaults
        system_info_.type = AmsType::HAPPY_HARE;
        system_info_.type_name = "Happy Hare (Mock)";
        system_info_.version = "2.7.0-mock";
        system_info_.supports_bypass = true;
        topology_ = PathTopology::LINEAR;

        if (!system_info_.units.empty()) {
            system_info_.units[0].name = "Mock MMU";
        }

        spdlog::info("[AmsBackendMock] Multi-unit mode disabled");
    }
}

bool AmsBackendMock::is_multi_unit_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return multi_unit_mode_;
}

void AmsBackendMock::set_mixed_topology_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    mixed_topology_mode_ = enabled;

    if (enabled) {
        // Disable conflicting modes
        tool_changer_mode_ = false;
        multi_unit_mode_ = false;

        // Configure as AFC system
        system_info_.type = AmsType::AFC;
        system_info_.type_name = "AFC (Mock Mixed)";
        system_info_.version = "1.0.32-mock";
        system_info_.total_slots = 12;

        auto afc_caps = helix::printer::afc_default_capabilities();
        system_info_.supports_endless_spool = afc_caps.supports_endless_spool;
        system_info_.supports_tool_mapping = afc_caps.supports_tool_mapping;
        system_info_.supports_bypass = afc_caps.supports_bypass;
        system_info_.supports_purge = afc_caps.supports_purge;
        system_info_.tip_method = afc_caps.tip_method;
        system_info_.has_hardware_bypass_sensor = false;

        topology_ = PathTopology::HUB;

        // Per-unit topologies
        unit_topologies_.clear();
        unit_topologies_.push_back(PathTopology::PARALLEL);
        unit_topologies_.push_back(PathTopology::HUB);
        unit_topologies_.push_back(PathTopology::HUB);

        // Reinitialize registry with 3 units (4+4+4=12 slots)
        slots_.clear();
        slots_.initialize_units({
            {"Turtle_1", {"0", "1", "2", "3"}},
            {"AMS_1", {"0", "1", "2", "3"}},
            {"AMS_2", {"0", "1", "2", "3"}},
        });

        // Helper to populate a slot
        auto populate_slot = [this](int gi, int si, const char* material, uint32_t color,
                                    const char* color_name, SlotStatus status, int /*tool*/,
                                    int spoolman_id, float remaining) {
            auto* entry = slots_.get_mut(gi);
            if (!entry)
                return;
            entry->info.slot_index = si;
            entry->info.global_index = gi;
            entry->info.material = material;
            entry->info.color_rgb = color;
            entry->info.color_name = color_name;
            entry->info.status = status;
            entry->info.spoolman_id = spoolman_id;
            entry->info.total_weight_g = 1000.0f;
            entry->info.remaining_weight_g = remaining;
            auto mat_info = filament::find_material(material);
            if (mat_info) {
                entry->info.nozzle_temp_min = mat_info->nozzle_min;
                entry->info.nozzle_temp_max = mat_info->nozzle_max;
                entry->info.bed_temp = mat_info->bed_temp;
            }
        };

        // Unit 0: Turtle_1 (Box Turtle) — 4 lanes, PARALLEL, 1:1 tool mapping
        populate_slot(0, 0, "ASA", 0x000000, "Black", SlotStatus::LOADED, 0, 300, 1000.0f);
        populate_slot(1, 1, "PLA", 0xFF0000, "Red", SlotStatus::AVAILABLE, 1, 301, 800.0f);
        populate_slot(2, 2, "PETG", 0x00FF00, "Green", SlotStatus::AVAILABLE, 2, 302, 600.0f);
        populate_slot(3, 3, "PLA", 0xFFFFFF, "White", SlotStatus::AVAILABLE, 3, 303, 400.0f);

        // Unit 1: AMS_1 (OpenAMS) — 4 lanes, HUB, T4-T7
        populate_slot(4, 0, "PETG", 0x1E88E5, "Blue", SlotStatus::AVAILABLE, 4, 310, 1000.0f);
        populate_slot(5, 1, "PLA", 0xFDD835, "Yellow", SlotStatus::AVAILABLE, 5, 311, 850.0f);
        populate_slot(6, 2, "ABS", 0x8E24AA, "Purple", SlotStatus::AVAILABLE, 6, 312, 700.0f);
        populate_slot(7, 3, "TPU", 0xFF6F00, "Orange", SlotStatus::AVAILABLE, 7, 313, 550.0f);

        // Unit 2: AMS_2 (OpenAMS) — 4 lanes, HUB, T8-T11
        populate_slot(8, 0, "PLA", 0xE53935, "Red", SlotStatus::AVAILABLE, 8, 320, 1000.0f);
        populate_slot(9, 1, "ASA", 0x43A047, "Green", SlotStatus::AVAILABLE, 9, 321, 900.0f);
        populate_slot(10, 2, "PETG", 0x90CAF9, "Sky Blue", SlotStatus::AVAILABLE, 10, 322, 800.0f);
        populate_slot(11, 3, "PLA-CF", 0x424242, "Carbon", SlotStatus::AVAILABLE, 11, 323, 700.0f);

        // Tool mapping: 12 virtual tools, 1:1 with slots
        slots_.set_tool_map({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});

        // Unit-level metadata
        system_info_.units.clear();
        {
            AmsUnit u;
            u.unit_index = 0;
            u.name = "Turtle_1";
            u.slot_count = 4;
            u.first_slot_global_index = 0;
            u.connected = true;
            u.firmware_version = "1.0.32-mock";
            u.has_encoder = false;
            u.has_toolhead_sensor = true;
            u.has_slot_sensors = true;
            u.has_hub_sensor = false; // PARALLEL: no shared hub
            u.topology = PathTopology::PARALLEL;
            BufferHealth health;
            health.fault_detection_enabled = true;
            health.state = "Trailing";
            health.distance_to_fault = 50.0f;
            u.buffer_health = health;
            system_info_.units.push_back(u);
        }
        {
            AmsUnit u;
            u.unit_index = 1;
            u.name = "AMS_1";
            u.slot_count = 4;
            u.first_slot_global_index = 4;
            u.connected = true;
            u.firmware_version = "1.0.0-mock";
            u.has_encoder = false;
            u.has_toolhead_sensor = true;
            u.has_slot_sensors = true;
            u.has_hub_sensor = true;
            u.hub_sensor_triggered = false;
            u.topology = PathTopology::HUB;
            u.hub_tool_label = 4;
            system_info_.units.push_back(u);
        }
        {
            AmsUnit u;
            u.unit_index = 2;
            u.name = "AMS_2";
            u.slot_count = 4;
            u.first_slot_global_index = 8;
            u.connected = true;
            u.firmware_version = "1.0.0-mock";
            u.has_encoder = false;
            u.has_toolhead_sensor = true;
            u.has_slot_sensors = true;
            u.has_hub_sensor = true;
            u.hub_sensor_triggered = false;
            u.topology = PathTopology::HUB;
            u.hub_tool_label = 5;
            system_info_.units.push_back(u);
        }

        // Start with slot 0 loaded
        system_info_.current_slot = 0;
        system_info_.current_tool = 0;
        system_info_.filament_loaded = true;
        filament_segment_ = PathSegment::NOZZLE;

        // AFC device sections and actions
        mock_device_sections_ = helix::printer::afc_default_sections();
        mock_device_actions_ = helix::printer::afc_default_actions();
        for (auto& action : mock_device_actions_) {
            if (action.id == "save_restart") {
                action.enabled = false;
                action.disable_reason = "Not available in mock mode";
            }
        }

        spdlog::info("[AmsBackendMock] Mixed topology mode: Turtle_1 (4) + AMS_1 (4) + AMS_2 (4) = "
                     "12 slots, 6 tools");
    } else {
        mixed_topology_mode_ = false;
        unit_topologies_.clear();

        // Revert to Happy Hare defaults
        system_info_.type = AmsType::HAPPY_HARE;
        system_info_.type_name = "Happy Hare (Mock)";
        system_info_.version = "2.7.0-mock";
        system_info_.supports_bypass = true;
        topology_ = PathTopology::LINEAR;

        if (!system_info_.units.empty()) {
            system_info_.units[0].name = "Mock MMU";
        }

        mock_device_sections_ = helix::printer::hh_default_sections();
        mock_device_actions_ = helix::printer::hh_default_actions();

        spdlog::info("[AmsBackendMock] Mixed topology mode disabled");
    }
}

bool AmsBackendMock::is_mixed_topology_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mixed_topology_mode_;
}

PathTopology AmsBackendMock::get_unit_topology(int unit_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit_index >= 0 && unit_index < static_cast<int>(unit_topologies_.size())) {
        return unit_topologies_[unit_index];
    }
    return topology_; // Fallback to system-wide topology
}

bool AmsBackendMock::slot_has_prep_sensor(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Mock always has prep sensors on all valid slots (matches AFC behavior)
    return slots_.is_valid_index(slot_index);
}

int AmsBackendMock::get_effective_delay_ms(int base_ms, float variance) const {
    double speedup = get_runtime_config()->sim_speedup;
    if (speedup <= 0)
        speedup = 1.0;

    int effective = static_cast<int>(base_ms / speedup);

    // Apply variance if non-zero
    if (variance > 0.0f && effective > 0) {
        // Random factor between (1-variance) and (1+variance)
        float random_factor = static_cast<float>(std::rand() % 1000) / 1000.0f; // 0.0-0.999
        float factor = 1.0f + (random_factor - 0.5f) * 2.0f * variance;
        effective = static_cast<int>(effective * factor);
    }

    return std::max(1, effective); // At least 1ms
}

void AmsBackendMock::set_action(AmsAction action, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    system_info_.action = action;
    system_info_.operation_detail = detail;
}

void AmsBackendMock::run_load_segment_animation(int slot_index,
                                                InterruptibleSleep interruptible_sleep) {
    // Calculate per-segment delay
    int total_animation_ms =
        realistic_mode_ ? get_effective_delay_ms(SEGMENT_ANIMATION_BASE_MS, LOADING_VARIANCE)
                        : get_effective_delay_ms(operation_delay_ms_);
    int segment_delay = total_animation_ms / 6;

    const PathSegment load_sequence[] = {
        PathSegment::SPOOL,  PathSegment::PREP,     PathSegment::LANE,  PathSegment::HUB,
        PathSegment::OUTPUT, PathSegment::TOOLHEAD, PathSegment::NOZZLE};

    for (auto seg : load_sequence) {
        if (shutdown_requested_ || cancel_requested_)
            return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            filament_segment_ = seg;
            system_info_.current_slot = slot_index; // Set active slot early for visualization
        }
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(segment_delay))
            return;
    }
}

void AmsBackendMock::run_unload_segment_animation(InterruptibleSleep interruptible_sleep) {
    int total_animation_ms =
        realistic_mode_ ? get_effective_delay_ms(SEGMENT_ANIMATION_BASE_MS, LOADING_VARIANCE)
                        : get_effective_delay_ms(operation_delay_ms_);
    int segment_delay = total_animation_ms / 6;

    const PathSegment unload_sequence[] = {
        PathSegment::NOZZLE, PathSegment::TOOLHEAD, PathSegment::OUTPUT, PathSegment::HUB,
        PathSegment::LANE,   PathSegment::PREP,     PathSegment::SPOOL,  PathSegment::NONE};

    for (auto seg : unload_sequence) {
        if (shutdown_requested_ || cancel_requested_)
            return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            filament_segment_ = seg;
        }
        spdlog::debug("[AmsBackendMock] Unload step: segment={}, delay={}ms", static_cast<int>(seg),
                      segment_delay);
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(segment_delay))
            return;
    }
}

void AmsBackendMock::finalize_load_state(int slot_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    system_info_.filament_loaded = true;
    filament_segment_ = PathSegment::NOZZLE;
    if (slot_index >= 0) {
        system_info_.current_slot = slot_index;
        system_info_.current_tool = slot_index;
        auto* entry = slots_.get_mut(slot_index);
        if (entry) {
            entry->info.status = SlotStatus::LOADED;
        }
    }
    system_info_.action = AmsAction::IDLE;
    system_info_.operation_detail.clear();
    system_info_.pending_target_slot = -1;
}

void AmsBackendMock::finalize_unload_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.current_slot >= 0) {
        auto* entry = slots_.get_mut(system_info_.current_slot);
        if (entry) {
            entry->info.status = SlotStatus::AVAILABLE;
        }
    }
    system_info_.filament_loaded = false;
    system_info_.current_slot = -1;
    filament_segment_ = PathSegment::NONE;
    system_info_.action = AmsAction::IDLE;
    system_info_.operation_detail.clear();
}

void AmsBackendMock::execute_load_operation(int slot_index,
                                            InterruptibleSleep interruptible_sleep) {
    if (realistic_mode_) {
        // Phase 1: HEATING
        spdlog::debug("[AmsBackendMock] Load phase: HEATING");
        set_action(AmsAction::HEATING, "Heating nozzle for load");
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(get_effective_delay_ms(HEATING_BASE_MS, HEATING_VARIANCE)))
            return;
        if (shutdown_requested_ || cancel_requested_)
            return;

        // Phase 2: LOADING with segment animation
        spdlog::debug("[AmsBackendMock] Load phase: LOADING (segment animation)");
        set_action(AmsAction::LOADING, "Loading from slot " + std::to_string(slot_index));
        emit_event(EVENT_STATE_CHANGED);
    }

    // Segment animation (same for both modes)
    run_load_segment_animation(slot_index, interruptible_sleep);
    if (shutdown_requested_ || cancel_requested_)
        return;

    if (realistic_mode_ && system_info_.supports_purge) {
        // Phase 3: PURGING (only if AMS supports it)
        spdlog::debug("[AmsBackendMock] Load phase: PURGING");
        set_action(AmsAction::PURGING, "Purging filament");
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(get_effective_delay_ms(PURGING_BASE_MS, PURGING_VARIANCE)))
            return;
        if (shutdown_requested_ || cancel_requested_)
            return;
    }

    // Finalize
    finalize_load_state(slot_index);
}

void AmsBackendMock::execute_unload_operation(InterruptibleSleep interruptible_sleep) {
    if (realistic_mode_) {
        // Phase 1: HEATING (shorter - just for clean cut)
        spdlog::debug("[AmsBackendMock] Unload phase: HEATING");
        set_action(AmsAction::HEATING, "Heating for cut");
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(get_effective_delay_ms(HEATING_BASE_MS / 2, HEATING_VARIANCE)))
            return;
        if (shutdown_requested_ || cancel_requested_)
            return;

        // Phase 2: CUTTING
        spdlog::debug("[AmsBackendMock] Unload phase: CUTTING");
        set_action(AmsAction::CUTTING, "Cutting filament");
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(get_effective_delay_ms(CUTTING_BASE_MS, TIP_VARIANCE)))
            return;
        if (shutdown_requested_ || cancel_requested_)
            return;

        // Phase 3: UNLOADING with segment animation
        spdlog::debug("[AmsBackendMock] Unload phase: UNLOADING (segment animation)");
        set_action(AmsAction::UNLOADING, "Retracting filament");
        emit_event(EVENT_STATE_CHANGED);
    }

    // Reverse segment animation
    run_unload_segment_animation(interruptible_sleep);
    if (shutdown_requested_ || cancel_requested_)
        return;

    // Finalize
    finalize_unload_state();
}

void AmsBackendMock::execute_tool_change_operation(int target_slot,
                                                   InterruptibleSleep interruptible_sleep) {
    // Phase 1: Unload current filament
    execute_unload_operation(interruptible_sleep);
    if (shutdown_requested_ || cancel_requested_)
        return;

    // Phase 2: SELECTING (only in realistic mode)
    if (realistic_mode_) {
        spdlog::debug("[AmsBackendMock] Tool change phase: SELECTING slot {}", target_slot);
        set_action(AmsAction::SELECTING, "Selecting slot " + std::to_string(target_slot));
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(get_effective_delay_ms(SELECTING_BASE_MS, SELECTING_VARIANCE)))
            return;
        if (shutdown_requested_ || cancel_requested_)
            return;
    } else {
        // Non-realistic: finalize_unload_state set action to IDLE, but we need LOADING
        // for the load phase so that UI elements (slot pulse, step progress) stay active
        set_action(AmsAction::LOADING, "Loading slot " + std::to_string(target_slot));
        emit_event(EVENT_STATE_CHANGED);
    }

    // Phase 3: Load new filament
    execute_load_operation(target_slot, interruptible_sleep);
}

void AmsBackendMock::schedule_recovery_sequence() {
    // Wait for any previous operation to complete first
    wait_for_operation_thread();

    // Reset flags for new operation
    shutdown_requested_ = false;
    cancel_requested_ = false;

    // Mark thread as running BEFORE creating it (for safe shutdown)
    operation_thread_running_ = true;

    // Run recovery sequence in background thread
    operation_thread_ = std::thread([this]() {
        // Helper lambda for interruptible sleep
        InterruptibleSleep interruptible_sleep = [this](int ms) -> bool {
            std::unique_lock<std::mutex> lock(shutdown_mutex_);
            return !shutdown_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] {
                return shutdown_requested_.load() || cancel_requested_.load();
            });
        };

        // Phase 1: CHECKING (verify system state after error)
        spdlog::debug("[AmsBackendMock] Recovery phase: CHECKING");
        set_action(AmsAction::CHECKING, "Checking system state");
        emit_event(EVENT_STATE_CHANGED);
        if (!interruptible_sleep(get_effective_delay_ms(CHECKING_BASE_MS, CHECKING_VARIANCE)))
            return;
        if (shutdown_requested_ || cancel_requested_)
            return;

        // Phase 2: Return to IDLE
        {
            std::lock_guard<std::mutex> lock(mutex_);
            system_info_.action = AmsAction::IDLE;
            system_info_.operation_detail.clear();
            error_segment_ = PathSegment::NONE; // Clear error location
        }
        emit_event(EVENT_STATE_CHANGED);
        spdlog::info("[AmsBackendMock] Recovery complete (realistic mode)");
    });
}

void AmsBackendMock::emit_event(const std::string& event, const std::string& data) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = event_callback_;
    }

    if (cb) {
        cb(event, data);
    }
}

void AmsBackendMock::schedule_completion(AmsAction action, const std::string& complete_event,
                                         int slot_index) {
    // Wait for any previous operation to complete first
    wait_for_operation_thread();

    // Reset flags for new operation
    shutdown_requested_ = false;
    cancel_requested_ = false;

    // Mark thread as running BEFORE creating it (for safe shutdown)
    operation_thread_running_ = true;

    // Check if this is a tool change (complete_event signals intent)
    bool is_tool_change = (complete_event == EVENT_TOOL_CHANGED);

    // Simulate operation delay in background thread with path segment progression
    operation_thread_ = std::thread([this, action, complete_event, slot_index, is_tool_change]() {
        // Helper lambda for interruptible sleep (returns false if cancelled/shutdown)
        InterruptibleSleep interruptible_sleep = [this](int ms) -> bool {
            std::unique_lock<std::mutex> lock(shutdown_mutex_);
            return !shutdown_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] {
                return shutdown_requested_.load() || cancel_requested_.load();
            });
        };

        if (is_tool_change) {
            // Tool change uses special operation with SELECTING phase
            execute_tool_change_operation(slot_index, interruptible_sleep);
        } else if (action == AmsAction::LOADING) {
            // Use phase executor (handles both realistic and simple modes)
            execute_load_operation(slot_index, interruptible_sleep);
        } else if (action == AmsAction::UNLOADING) {
            // Use phase executor (handles both realistic and simple modes)
            execute_unload_operation(interruptible_sleep);
        } else {
            // For other actions, just wait and complete
            if (!interruptible_sleep(get_effective_delay_ms(operation_delay_ms_)))
                return;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                system_info_.action = AmsAction::IDLE;
                system_info_.operation_detail.clear();
            }
        }

        if (shutdown_requested_ || cancel_requested_)
            return; // Final check before emitting

        emit_event(complete_event, slot_index >= 0 ? std::to_string(slot_index) : "");
        emit_event(EVENT_STATE_CHANGED);
    });
}

// ============================================================================
// Endless spool implementation
// ============================================================================

void AmsBackendMock::set_endless_spool_supported(bool supported) {
    std::lock_guard<std::mutex> lock(mutex_);
    endless_spool_supported_ = supported;
    // Update system_info to reflect the new capability
    system_info_.supports_endless_spool = supported;
    spdlog::debug("[AmsBackendMock] Endless spool supported set to {}", supported);
}

void AmsBackendMock::set_endless_spool_editable(bool editable) {
    std::lock_guard<std::mutex> lock(mutex_);
    endless_spool_editable_ = editable;
    spdlog::debug("[AmsBackendMock] Endless spool editable set to {}", editable);
}

helix::printer::EndlessSpoolCapabilities AmsBackendMock::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);

    helix::printer::EndlessSpoolCapabilities caps;
    caps.supported = endless_spool_supported_;
    caps.editable = endless_spool_supported_ && endless_spool_editable_;
    if (caps.supported) {
        caps.description =
            caps.editable ? "Per-slot backup (AFC-style)" : "Group-based (Happy Hare-style)";
    }
    return caps;
}

std::vector<helix::printer::EndlessSpoolConfig> AmsBackendMock::get_endless_spool_config() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<helix::printer::EndlessSpoolConfig> configs;
    configs.reserve(slots_.slot_count());
    for (int i = 0; i < slots_.slot_count(); ++i) {
        helix::printer::EndlessSpoolConfig config;
        config.slot_index = i;
        config.backup_slot = slots_.backup_for_slot(i);
        configs.push_back(config);
    }
    return configs;
}

AmsError AmsBackendMock::set_endless_spool_backup(int slot_index, int backup_slot) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!endless_spool_supported_) {
        return AmsErrorHelper::not_supported("Endless spool");
    }

    if (!endless_spool_editable_) {
        return AmsErrorHelper::not_supported("Endless spool configuration");
    }

    if (!slots_.is_valid_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, slots_.slot_count() - 1);
    }

    if (backup_slot == slot_index) {
        return AmsError(AmsResult::INVALID_SLOT,
                        "Cannot set slot " + std::to_string(slot_index) + " as its own backup",
                        "Invalid backup configuration", "Select a different slot as backup");
    }

    if (backup_slot != -1 && !slots_.is_valid_index(backup_slot)) {
        return AmsErrorHelper::invalid_slot(backup_slot, slots_.slot_count() - 1);
    }

    slots_.set_backup(slot_index, backup_slot);
    spdlog::info("[AmsBackendMock] Set slot {} backup to {}", slot_index, backup_slot);

    return AmsErrorHelper::success();
}

// ============================================================================
// Tool mapping implementation
// ============================================================================

helix::printer::ToolMappingCapabilities AmsBackendMock::get_tool_mapping_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Tool changers don't support tool mapping (tools ARE slots)
    if (tool_changer_mode_) {
        return {false, false, ""};
    }

    // Filament systems support editable tool mapping
    return {true, true, "Mock tool-to-slot mapping"};
}

std::vector<int> AmsBackendMock::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Tool changers don't support tool mapping (tools ARE slots)
    if (tool_changer_mode_) {
        return {};
    }

    return slots_.build_system_info().tool_to_slot_map;
}

// ============================================================================
// Factory method implementations (in ams_backend.cpp, but included here for mock)
// ============================================================================

// ============================================================================
// Device actions implementation
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendMock::get_device_sections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mock_device_sections_;
}

std::vector<helix::printer::DeviceAction> AmsBackendMock::get_device_actions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mock_device_actions_;
}

AmsError AmsBackendMock::execute_device_action(const std::string& action_id,
                                               const std::any& value) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Store for test verification
    last_action_id_ = action_id;
    last_action_value_ = value;

    // Mock calibration wizard: simulate AFC_CALIBRATION action_prompt sequence
    if (action_id == "calibration_wizard") {
        // Copy callback before releasing lock — callback may chain into
        // UI code that queries backend state (would deadlock if lock held)
        auto cb = gcode_response_callback_;
        if (!cb) {
            spdlog::warn("[AMS Mock] Calibration wizard: no gcode response callback, skipping");
            return AmsErrorHelper::success();
        }

        spdlog::info("[AMS Mock] Simulating AFC calibration wizard action_prompt");
        lock.unlock();

        // Simulate a multi-step calibration prompt (matches real AFC behavior)
        cb("// action:prompt_begin AFC Calibration");
        cb("// action:prompt_text Lane calibration measures bowden tube length");
        cb("// action:prompt_text for accurate filament loading distances.");
        cb("// action:prompt_text ");
        cb("// action:prompt_text Select a lane to calibrate, or calibrate all lanes.");
        cb("// action:prompt_button_group_start");
        cb("// action:prompt_button Lane 1|RESPOND msg=\"AFC_CALIBRATION LANE=lane1\"|primary");
        cb("// action:prompt_button Lane 2|RESPOND msg=\"AFC_CALIBRATION LANE=lane2\"|primary");
        cb("// action:prompt_button Lane 3|RESPOND msg=\"AFC_CALIBRATION LANE=lane3\"|primary");
        cb("// action:prompt_button Lane 4|RESPOND msg=\"AFC_CALIBRATION LANE=lane4\"|primary");
        cb("// action:prompt_button_group_end");
        cb("// action:prompt_button Calibrate All|RESPOND msg=\"AFC_CALIBRATION ALL=1\"|secondary");
        cb("// action:prompt_footer_button Cancel|RESPOND msg=\"AFC_CALIBRATION CANCEL=1\"|error");
        cb("// action:prompt_show");

        return AmsErrorHelper::success();
    }

    // Find the action to verify it exists
    for (const auto& action : mock_device_actions_) {
        if (action.id == action_id) {
            if (!action.enabled) {
                return AmsErrorHelper::not_supported(action.disable_reason);
            }
            spdlog::info("[AMS Mock] Executed device action: {} with value type: {}", action_id,
                         value.has_value() ? value.type().name() : "none");
            return AmsErrorHelper::success();
        }
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}

void AmsBackendMock::set_device_sections(std::vector<helix::printer::DeviceSection> sections) {
    std::lock_guard<std::mutex> lock(mutex_);
    mock_device_sections_ = std::move(sections);
}

void AmsBackendMock::set_device_actions(std::vector<helix::printer::DeviceAction> actions) {
    std::lock_guard<std::mutex> lock(mutex_);
    mock_device_actions_ = std::move(actions);
}

std::pair<std::string, std::any> AmsBackendMock::get_last_executed_action() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {last_action_id_, last_action_value_};
}

void AmsBackendMock::clear_last_executed_action() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_action_id_.clear();
    last_action_value_.reset();
}

void AmsBackendMock::set_gcode_response_callback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    gcode_response_callback_ = std::move(callback);
    spdlog::debug("[AMS Mock] Gcode response callback {}",
                  gcode_response_callback_ ? "set" : "cleared");
}

void AmsBackendMock::set_initial_state_scenario(const std::string& scenario) {
    initial_state_scenario_ = scenario;
    spdlog::debug("[AMS Mock] Initial state scenario set to '{}'", scenario);
}

// ============================================================================
// Factory method implementations (in ams_backend.cpp, but included here for mock)
// ============================================================================

std::unique_ptr<AmsBackend> AmsBackend::create_mock(int slot_count) {
    return std::make_unique<AmsBackendMock>(slot_count);
}
