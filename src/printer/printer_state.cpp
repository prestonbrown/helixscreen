// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_state.cpp
 * @brief Reactive printer state model with LVGL subjects for all printer data
 *
 * @pattern Singleton with set_*() -> set_*_internal() for thread-safe updates
 * @threading Public setters called from WebSocket; internal setters run on main thread
 * @gotchas Static string buffers; init subjects before XML; temps in decidegrees
 *
 * @see moonraker_client.cpp, ui_update_queue.h
 */

#include "printer_state.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "capability_overrides.h"
#include "chamber_heater_assignment.h"
#include "connection_state.h" // For ConnectionState enum
#include "device_display_name.h"
#include "enclosure_top.h"
#include "hardware_validator.h"
#include "i_moonraker_client.h" // for helix::kCachedSnapshotMarker
#include "json_utils.h"
#include "lvgl.h"
#include "lvgl/src/display/lv_display_private.h" // For rendering_in_progress check
#include "lvgl_debug_invalidate.h"
#include "macro_manager.h"
#include "plr_backend.h"
#include "pre_print_preferences.h"
#include "printer_cache_registry.h"
#include "printer_discovery.h"
#include "probe_sensor_manager.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "static_subject_registry.h"
#include "system/crash_handler.h"
#include "unit_conversions.h"
#include "z_offset_persistence.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// ============================================================================
// PrintJobState Free Functions
// ============================================================================

namespace helix {

PrintJobState parse_print_job_state(const char* state_str) {
    if (!state_str) {
        return PrintJobState::STANDBY;
    }

    // RAW_PRINT_STATE_OK: whole function. This IS the wire parse. Everything
    // downstream that wants the derived axis gets it from derive_print_state().
    if (std::strcmp(state_str, "standby") == 0) {
        return PrintJobState::STANDBY;
    } else if (std::strcmp(state_str, "printing") == 0) {
        return PrintJobState::PRINTING;
    } else if (std::strcmp(state_str, "paused") == 0) {
        return PrintJobState::PAUSED;
    } else if (std::strcmp(state_str, "complete") == 0) {
        return PrintJobState::COMPLETE;
    } else if (std::strcmp(state_str, "cancelled") == 0) {
        return PrintJobState::CANCELLED;
    } else if (std::strcmp(state_str, "error") == 0) {
        return PrintJobState::ERROR;
    }

    // Unknown state defaults to STANDBY
    spdlog::warn("[PrinterState] Unknown print state string: '{}', defaulting to STANDBY",
                 state_str);
    return PrintJobState::STANDBY;
}

const char* print_job_state_to_string(PrintJobState state) {
    // RAW_PRINT_STATE_OK: whole function - the wire enum's own name table.
    switch (state) {
    case PrintJobState::STANDBY:
        return "Standby";
    case PrintJobState::PRINTING:
        return "Printing";
    case PrintJobState::PAUSED:
        return "Paused";
    case PrintJobState::COMPLETE:
        return "Complete";
    case PrintJobState::CANCELLED:
        return "Canceled";
    case PrintJobState::ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

} // namespace helix

using namespace helix;

// ============================================================================
// PrinterState Implementation
// ============================================================================

PrinterState::PrinterState() {
    // Load user-configured capability overrides from settings.json
    capability_overrides_.load_from_config();
    plugin_status_state_.set_locality_source(
        [this] { return moonraker_locality() == helix::MoonrakerLocality::Local; });
}

PrinterState::~PrinterState() {
    // Backstop for the path that skips deinit_subjects() entirely: the subjects
    // are members, so they die with this object even though nothing called
    // lv_subject_deinit() on them. Any ObserverGuard still holding one would
    // otherwise reset() against freed storage. Flipping costs nothing when
    // deinit_subjects() already ran — that installed a fresh token, and flipping
    // it just tells holders registered since then that the state is gone too.
    // Plain flip, no renewal: a dying object has no successor generation.
    subjects_.mark_subjects_dead();
    unregister_static_deinit();
}

void PrinterState::unregister_static_deinit() {
    // The registered callback captures `this`; a per-test instance would leave it
    // dangling in the process-wide registry.
    if (!StaticSubjectRegistry::is_destroyed()) {
        StaticSubjectRegistry::instance().unregister("PrinterState");
    }
}

void PrinterState::deinit_subjects() {
    if (!subjects_initialized_) {
        spdlog::trace(
            "[PrinterState] deinit_subjects: subjects not initialized, nothing to deinit");
        return;
    }

    spdlog::trace("[PrinterState] deinit_subjects: Deinitializing all subjects");
    unregister_static_deinit();

    // Expire any setter callbacks still queued on the UpdateQueue. They capture
    // `this` and touch the subjects torn down below (directly or through
    // apply_dynamic_options()); without this the next drain notifies a freed
    // observer list (#1165, #1146).
    async_lifetime_.invalidate();

    // Drop the per-printer cache invalidator registered by init_subjects(). It captures
    // `this`, and the registry outlives non-singleton instances (test fixtures own their
    // own PrinterState), so leaving it registered would hold a callback over freed memory.
    helix::PrinterCacheRegistry::instance().unregister("PrinterState");

    // Signal death of EVERY subject below BEFORE anything is torn down. The
    // umbrella contract: this token covers the per-domain components too, whose
    // subjects die in the cascade below — BEFORE this manager's own
    // deinit_all() runs — so the flip cannot wait for it. Observers held by
    // objects that outlive this call — process-lifetime panel singletons, most
    // of all — check this token in ObserverGuard::reset() and skip
    // lv_observer_remove() on observer nodes that lv_subject_deinit() is about
    // to free. Flipping the bool (rather than only dropping the shared_ptr) is
    // what makes it work when a holder still has a copy. The later
    // subjects_.deinit_all() flips again; that lands on the renewed token,
    // which nothing can have fetched in between.
    subjects_.expire_subjects_lifetime();

    // Deinit all sub-component subjects
    temperature_state_.deinit_subjects();
    motion_state_.deinit_subjects();
    fan_state_.deinit_subjects();
    print_domain_.deinit_subjects();
    capabilities_state_.deinit_subjects();
    plugin_status_state_.deinit_subjects();
    calibration_state_.deinit_subjects();
    hardware_validation_state_.deinit_subjects();
    composite_visibility_state_.deinit_subjects();
    network_state_.deinit_subjects();
    versions_state_.deinit_subjects();
    excluded_objects_state_.deinit_subjects();
    profile_state_.deinit_subjects();

    // Deinit PrinterState's own subjects (multi-printer)
    lv_subject_deinit(&active_printer_name_);
    subjects_.deinit_all();

    subjects_initialized_ = false;
}

void PrinterState::init_subjects(bool register_xml) {
    // Detect LVGL reinitialization - display pointer changes when lv_init() called again
    // This happens in test suites where each test reinitializes LVGL but the PrinterState
    // singleton persists. Without this check, subjects would point to freed memory.
    lv_display_t* current_display = lv_display_get_default();

    if (subjects_initialized_) {
        if (current_display != cached_display_) {
            // LVGL was reinitialized - our subjects are now invalid
            spdlog::warn("[PrinterState] LVGL reinitialized (display changed), resetting subjects");
            deinit_subjects();
        } else {
            spdlog::debug("[PrinterState] Subjects already initialized, skipping");
            return;
        }
    }

    cached_display_ = current_display;

    spdlog::trace("[PrinterState] Initializing subjects (register_xml={})", register_xml);

    // Initialize temperature state component (extruder and bed temperatures)
    temperature_state_.init_subjects(register_xml);

    // Initialize motion state component (position, speed/flow, z-offset)
    motion_state_.init_subjects(register_xml);

    // Initialize fan state component (fan speed, multi-fan tracking)
    fan_state_.init_subjects(register_xml);

    // Initialize print state component (progress, state, timing, layers, print start)
    print_domain_.init_subjects(register_xml);

    // Initialize capabilities state component (hardware capabilities, feature availability)
    capabilities_state_.init_subjects(register_xml);

    // Initialize network state component (connection, klippy, nav buttons).
    // The locality verdict starts where its subject does.
    moonraker_locality_.store(static_cast<int>(helix::MoonrakerLocality::Unknown));
    network_state_.init_subjects(register_xml);

    // Excluded objects state component (excluded_objects_version, excluded_objects set)
    excluded_objects_state_.init_subjects(register_xml);

    // Plugin status subjects - delegated to plugin_status_state_ component
    plugin_status_state_.init_subjects(register_xml);

    // Calibration state subjects (firmware retraction, manual probe, motor state)
    calibration_state_.init_subjects(register_xml);

    // Hardware validation subjects (for Hardware Health section in Settings)
    hardware_validation_state_.init_subjects(register_xml);

    // has_any_preprint_options aggregate (per-op can_show_* subjects retired)
    composite_visibility_state_.init_subjects(register_xml);

    // Version subjects (for About section) - delegated to versions_state_ component
    versions_state_.init_subjects(register_xml);

    // Printer type, its pre-print options and z-offset strategy
    profile_state_.init_subjects(register_xml);

    // Multi-printer subjects (owned directly by PrinterState)
    INIT_SUBJECT_STRING(active_printer_name, "", subjects_, register_xml);

    spdlog::trace("[PrinterState] Registered {} subjects with SubjectManager", subjects_.count());

    // All component subjects handle their own XML registration in init_subjects(register_xml)

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("PrinterState",
                                                      [this]() { deinit_subjects(); });

    // Self-register per-printer cache invalidation. This object outlives every printer
    // switch, so anything cached from Config::df() or from the previous printer's status
    // has to be dropped here.
    //  - capability_overrides_ is loaded from Config::df() in the constructor only, so
    //    without this the map keeps the startup printer's enable/disable choices.
    //  - exclude_object state is only refreshed by update_from_status() when the new
    //    printer reports an "exclude_object" object, so switching to a printer without
    //    [exclude_object] configured would keep the previous printer's objects on screen.
    helix::PrinterCacheRegistry::instance().register_invalidator("PrinterState", [this]() {
        // Only the override map is refreshed. The effective capability subjects are
        // re-derived from set_hardware(discovery_, capability_overrides_) when the new
        // printer's discovery lands; deriving them here would pair the new printer's
        // overrides with the OLD printer's still-cached discovery_.
        capability_overrides_.load_from_config();
        excluded_objects_state_.clear_objects();
    });

    spdlog::trace("[PrinterState] Subjects initialized and registered successfully");
}

std::optional<StatusFrame> helix::parse_status_notification(const json& notification) {
    auto method = notification.find("method");
    auto params = notification.find("params");
    if (method == notification.end() || !method->is_string() ||
        method->get_ref<const std::string&>() != "notify_status_update" ||
        params == notification.end() || !params->is_array() || params->empty()) {
        return std::nullopt;
    }
    StatusFrame frame;
    frame.status = &(*params)[0];
    if (params->size() > 1 && (*params)[1].is_number()) {
        frame.eventtime = (*params)[1].get<double>();
    }
    frame.from_cached_snapshot =
        helix::json_util::safe_bool(notification, helix::kCachedSnapshotMarker);
    frame.whole_objects = helix::json_util::safe_bool(notification, helix::kWholeObjectsMarker);
    return frame;
}

void PrinterState::apply_klippy_status(const json& webhooks, double eventtime,
                                       bool from_cached_snapshot,
                                       std::optional<uint64_t> frame_epoch) {
    if (network_state_.apply_webhooks(webhooks, eventtime, from_cached_snapshot, frame_epoch)) {
        calibration_state_.reset_klippy_volatile();
    }
}

void PrinterState::update_from_status(const json& state, double eventtime,
                                      bool from_cached_snapshot,
                                      std::optional<uint64_t> frame_epoch, bool whole_objects) {
    // Debug: Check if we're in render phase (this should never be true)
    LV_DEBUG_RENDER_STATE();

    // Delegate temperature updates to temperature state component
    temperature_state_.update_from_status(state);

    // Delegate motion updates to motion state component
    motion_state_.update_from_status(state);
    refresh_bed_drying_capability();

    // Discovery latches external z-offset persistence the moment a provider
    // matches, because the mistake that damages hardware is the other one
    // (#1401). One provider is keyed on the SET_GCODE_OFFSET wrapper, which
    // proves a wrapper exists but not that it stores anything, so a frame that
    // positively proves the store is absent is what relaxes the latch back to
    // the type-derived strategy. Gated on the flag, so this fires at most once
    // per latch instead of thrashing on every later frame.
    if (profile_state_.external_persistence() &&
        helix::zoffset::status_refutes_persistence(discovery_, state)) {
        spdlog::info("[PrinterState] Status refutes the detected z-offset persistence provider "
                     "({}): the wrapper stores no offset",
                     helix::zoffset::persistence_provider_name(discovery_));
        clear_z_offset_external_persistence_internal();
    }

    // Some firmwares keep pre-print option settings across prints and gate the
    // sliced gcode on what they hold, which makes them the authority on what
    // the toggles show. Silent on printers that store none.
    merge_firmware_option_defaults(
        helix::preprint_prefs::read_persisted_defaults(discovery_, state));

    // A removable enclosure top can come and go while connected; a frame that
    // does not mention it keeps the last report.
    if (auto fitted = helix::enclosure_top::fitted_from_status(discovery_, state)) {
        capabilities_state_.set_enclosure_top_fitted(*fitted);
    }

    // Delegate print updates to print state component
    print_domain_.update_from_status(state, whole_objects);

    // A loaded mesh and an applied level are what offer the skip toggles.
    if (helix::skip_wrappers::update_gates(skip_gates_, state)) {
        apply_dynamic_options();
        update_gcode_modification_visibility();
        if (skip_pending() && skip_pending_handler_) {
            skip_pending_handler_();
        }
    }

    // Extract kinematics type (determines if bed moves on Z or gantry moves)
    // This is not part of motion_state_ as it affects printer_bed_moves_ subject
    if (state.contains("toolhead")) {
        const auto& toolhead = state["toolhead"];
        if (toolhead.contains("kinematics") && toolhead["kinematics"].is_string()) {
            std::string kin = toolhead["kinematics"].get<std::string>();
            set_kinematics(kin);
        }

        // Track active extruder from toolhead (for tool changers and multi-extruder setups)
        if (toolhead.contains("extruder") && toolhead["extruder"].is_string()) {
            std::string active_ext = toolhead["extruder"].get<std::string>();
            temperature_state_.set_active_extruder(active_ext);
        }
    }

    // Delegate fan state updates to fan component
    fan_state_.update_from_status(state);

    excluded_objects_state_.update_from_status(state);

    // Klippy state from webhooks (shutdown/error detection), gated on freshness.
    if (auto it = state.find("webhooks"); it != state.end()) {
        apply_klippy_status(*it, eventtime, from_cached_snapshot, frame_epoch);
    }

    // Track Klipper pause_resume.is_paused (PAUSE/RESUME gcode state)
    if (state.contains("pause_resume")) {
        const auto& pr = state["pause_resume"];
        if (pr.contains("is_paused") && pr["is_paused"].is_boolean()) {
            is_paused_ = pr["is_paused"].get<bool>();
        }
    }

    // Delegate calibration updates (manual probe, motor state, firmware retraction)
    // to calibration_state_ component
    calibration_state_.update_from_status(state);

    // Re-arm the once-per-episode busy-queue toast when the COMPOSITE blocking
    // condition has cleared — never on an individual signal's falling edge. A
    // manual-probe session whose idle_timeout bounces to "Ready" between TESTZ moves
    // is still one blocking episode; keying off idle_timeout alone would re-toast
    // mid-episode (#1108 review). A print-start episode counts as part of that
    // composite here: the START macro runs while print_stats is already PRINTING,
    // which is_blocking_operation_active() excludes, so a latch claimed during a
    // print start stays held through the whole start sequence and re-arms only
    // once neither arm is active. is_blocking_operation_active() sees the
    // just-updated manual_probe / idle_timeout / print-job subjects. The store is
    // idempotent, so gating on the predicate needs no separate edge tracking.
    if (!is_blocking_operation_active() && !print_domain_.is_in_print_start()) {
        calibration_state_.arm_busy_queue_toast();
    }
}

void PrinterState::set_printer_connection_state(int state, const char* message) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    std::string msg = message ? message : "";
    async_lifetime_.defer("PrinterState::set_printer_connection_state", [this, state, msg]() {
        network_state_.set_printer_connection_state_internal(state, msg.c_str());
    });
}

void PrinterState::set_moonraker_is_remote(bool remote) {
    const auto locality =
        remote ? helix::MoonrakerLocality::Remote : helix::MoonrakerLocality::Local;
    moonraker_locality_.store(static_cast<int>(locality));
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    async_lifetime_.defer("PrinterState::set_moonraker_is_remote", [this, remote, locality]() {
        network_state_.set_moonraker_is_remote_internal(remote);
        network_state_.set_moonraker_locality_internal(locality);
        plugin_status_state_.on_locality_changed();
    });
}

void PrinterState::clear_moonraker_locality() {
    moonraker_locality_.store(static_cast<int>(helix::MoonrakerLocality::Unknown));
    async_lifetime_.defer("PrinterState::clear_moonraker_locality", [this]() {
        network_state_.set_moonraker_locality_internal(helix::MoonrakerLocality::Unknown);
        plugin_status_state_.on_locality_changed();
    });
}

bool PrinterState::is_moonraker_remote() {
    // Main-thread convenience read; UI decision points only.
    return lv_subject_get_int(network_state_.get_moonraker_is_remote_subject()) != 0;
}

void PrinterState::set_klippy_state(KlippyState state) {
    // These are the notify_klippy_ready / _shutdown / _disconnected paths: live,
    // authoritative, and they must outrank any replayed snapshot from here on,
    // including one already waiting in the notification queue.
    network_state_.mark_klippy_state_live();
    async_lifetime_.defer("PrinterState::set_klippy_state",
                          [this, state]() { set_klippy_state_internal(state); });
}

void PrinterState::set_klippy_state_sync(KlippyState state) {
    network_state_.mark_klippy_state_live();
    set_klippy_state_internal(state);
}

void PrinterState::set_klippy_state_if_unseeded(KlippyState state,
                                                const std::string& state_message) {
    // Deferred so the "has anything live landed?" check runs on the main thread,
    // in the same serialized order as the webhooks parse. Checking on the caller's
    // thread would race: a live frame could land between the check and the apply,
    // and printer.info's older answer would win anyway.
    async_lifetime_.defer("PrinterState::set_klippy_state_if_unseeded",
                          [this, state, state_message]() {
                              set_klippy_state_if_unseeded_internal(state, state_message);
                          });
}

void PrinterState::set_klippy_state_if_unseeded_internal(KlippyState state,
                                                         const std::string& state_message) {
    if (network_state_.klippy_state_from_live()) {
        spdlog::debug("[PrinterState] Ignoring printer.info klippy state {} — a live state "
                      "has already been applied",
                      static_cast<int>(state));
        return;
    }

    // Deliberately does NOT mark the state live: printer.info is a seed, and the
    // subscription snapshot that follows it on the same connection is strictly
    // newer, so it must still be allowed to correct this value.
    set_klippy_state_internal(state);
    if (!state_message.empty()) {
        network_state_.set_klippy_state_message(state_message, state);
    }
}

void PrinterState::set_klippy_state_internal(KlippyState state) {
    // Chokepoint for the deferred set_klippy_state(), set_klippy_state_sync() and
    // printer.info seed. The webhooks parse applies the same reset in
    // update_from_status() when PrinterNetworkState::apply_webhooks() reports a change.
    const bool changed = network_state_.set_klippy_state_internal(state);
    if (!changed) {
        return;
    }

    // Any transition invalidates state cached from Klipper's DELTA-only status
    // fields. Both directions matter: READY -> dead means nothing it was doing
    // survives; dead -> READY means a fresh Klipper with nothing blocking yet.
    // Without this, an idle_timeout captured mid-G28 outlived the restart and made
    // the app queue discretionary G-code fire-and-forget against an idle printer,
    // wedging the LED in-flight counter for the whole session (#1129).
    calibration_state_.reset_klippy_volatile();
}

void PrinterState::update_nav_buttons_enabled() {
    // Delegate to network_state_ component
    network_state_.update_nav_buttons_enabled();
}

void PrinterState::set_hardware(helix::PrinterDiscovery hardware) {
    // Called directly from the main LVGL thread (hardware discovery callback).
    // No deferral needed — the caller is already inside a queue_update callback.
    // Taken by value so we own a stable copy — the caller's source may alias
    // api->hardware_ which can be written by other queued callbacks (#799).
    static int s_set_hardware_n = 0;
    long sh_n = static_cast<long>(++s_set_hardware_n);
    crash_handler::breadcrumb::note("disc", "sh_entry",
                                    static_cast<long>(hardware.macros().size()));

    spdlog::debug("[PrinterState] set_hardware: has_probe={}", hardware.has_probe());

    // Store for later access by UI (e.g., chamber assignment dropdowns)
    discovery_ = std::move(hardware);
    crash_handler::breadcrumb::note("disc", "sh_moved",
                                    static_cast<long>(discovery_.macros().size()));

    // Pass auto-detected hardware to the override layer
    crash_handler::breadcrumb::note("disc", "pre_co_set", sh_n);
    capability_overrides_.set_hardware(discovery_);
    crash_handler::breadcrumb::note("disc", "post_co_set", sh_n);

    // Delegate capability subject updates to capabilities_state_ component
    capabilities_state_.set_hardware(discovery_, capability_overrides_);
    versions_state_.set_firmware_name_internal(discovery_.firmware_name());
    print_domain_.set_layer_pause_state_object(helix::layer_pause::state_object(discovery_));

    // PLR resume-macro capability comes from the same snapshot, ahead of the
    // initial status dispatch that carries the interrupted flag; the offer
    // decision reads both, so the capability must land first.
    print_domain_.set_plr_resume_macro_present(helix::plr_resume_macro_present(discovery_));

    // Fold the helper-macro install status in with the same snapshot.
    plugin_status_state_.set_helix_macros_base_status(MacroManager::evaluate_status(discovery_));

    // Stored option settings belong to the machine that reported them. New
    // hardware starts from none: a self-storing firmware reports its own in the
    // initial status, which is dispatched after this, and a merge still queued
    // from the previous machine's frames is dropped by the epoch.
    hardware_epoch_.fetch_add(1);
    profile_state_.clear_firmware_option_defaults();

    // Re-synthesize dynamic pre-print options now that hardware capabilities are
    // known. The bed_mesh option's adaptive_active flag (which relabels it to
    // "Adaptive Bed Mesh" and enables the adaptive param) depends on
    // discovery_.has_exclude_object(), which only becomes true here —
    // set_printer_type_internal() ran earlier (before hardware), so the option
    // would otherwise stay "Auto Bed Mesh".
    apply_dynamic_options();

    // Set kinematics from hardware discovery (configfile.config.printer.kinematics)
    // This is more reliable than toolhead status, which returns null on some printers
    if (!discovery_.kinematics().empty()) {
        set_kinematics(discovery_.kinematics());
    }

    auto& settings = helix::SettingsManager::instance();
    chamber::apply_resolution(discovery_, settings.get_chamber_sensor_assignment(),
                              settings.get_chamber_heater_assignment(), temperature_state_,
                              capabilities_state_, get_temperature_controller());
    refresh_bed_drying_capability();

    // Update composite subjects for G-code modification options
    // (visibility depends on both plugin status and capability)
    update_gcode_modification_visibility();
}

void PrinterState::set_klipper_version(const std::string& version) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    async_lifetime_.defer("PrinterState::set_klipper_version",
                          [this, version]() { set_klipper_version_internal(version); });
}

void PrinterState::set_klipper_version_internal(const std::string& version) {
    versions_state_.set_klipper_version_internal(version);
}

void PrinterState::set_moonraker_version(const std::string& version) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    async_lifetime_.defer("PrinterState::set_moonraker_version",
                          [this, version]() { set_moonraker_version_internal(version); });
}

void PrinterState::set_moonraker_version_internal(const std::string& version) {
    versions_state_.set_moonraker_version_internal(version);
}

void PrinterState::set_os_version(const std::string& version) {
    async_lifetime_.defer("PrinterState::set_os_version",
                          [this, version]() { set_os_version_internal(version); });
}

void PrinterState::set_os_version_internal(const std::string& version) {
    versions_state_.set_os_version_internal(version);
}

void PrinterState::set_timelapse_available(bool available) {
    // Delegate to capabilities_state_ component (handles thread-safety internally)
    capabilities_state_.set_timelapse_available(available);
    // Resynthesize the option set (timelapse is appended dynamically when
    // available) and recompute aggregate visibility — both must run on the
    // main thread.
    async_lifetime_.defer("PrinterState::set_timelapse_available", [this]() {
        apply_dynamic_options();
        update_gcode_modification_visibility();
    });
}

void PrinterState::set_timelapse_default_enabled(bool enabled) {
    // Seed the timelapse pre-print option's default from the global
    // moonraker-timelapse `enabled` setting. Both the member write and the
    // resynthesis must run on the main thread (#1094).
    async_lifetime_.defer("PrinterState::set_timelapse_default_enabled", [this, enabled]() {
        profile_state_.set_timelapse_default_enabled(enabled);
        apply_dynamic_options();
        update_gcode_modification_visibility();
    });
}

void PrinterState::merge_firmware_option_defaults(std::map<std::string, bool> defaults) {
    if (defaults.empty()) {
        return;
    }
    // Both the member write and the resynthesis touch LVGL subjects, and status
    // frames arrive on the websocket thread.
    const uint64_t epoch = hardware_epoch_.load();
    async_lifetime_.defer("PrinterState::merge_firmware_option_defaults",
                          [this, epoch, defaults = std::move(defaults)]() {
                              if (epoch != hardware_epoch_.load()) {
                                  return; // read off a machine that has since been replaced
                              }
                              if (!profile_state_.merge_firmware_option_defaults(defaults)) {
                                  return;
                              }
                              apply_dynamic_options();
                              update_gcode_modification_visibility();
                          });
}

void PrinterState::set_helix_plugin_installed(bool installed) {
    // Thread-safe: Use ui_queue_update to update LVGL subject from any thread
    // We handle the async dispatch here because we need to update composite subjects after
    async_lifetime_.defer("PrinterState::set_helix_plugin_installed", [this, installed]() {
        plugin_status_state_.set_installed(installed);

        // Update composite subjects for G-code modification options
        update_gcode_modification_visibility();
    });
}

void PrinterState::set_helix_plugin_link_dir(const std::string& dir) {
    async_lifetime_.defer("PrinterState::set_helix_plugin_link_dir",
                          [this, dir]() { plugin_status_state_.set_link_dir(dir); });
}

void PrinterState::set_helix_plugin_version(const std::string& version) {
    async_lifetime_.defer("PrinterState::set_helix_plugin_version",
                          [this, version]() { plugin_status_state_.set_version(version); });
}

void PrinterState::set_helix_plugin_disabled(bool disabled) {
    async_lifetime_.defer("PrinterState::set_helix_plugin_disabled",
                          [this, disabled]() { plugin_status_state_.set_disabled(disabled); });
}

void PrinterState::set_helix_plugin_features(helix::PluginFeatureMap features,
                                             helix::FeatureAnnounce announce) {
    async_lifetime_.defer("PrinterState::set_helix_plugin_features",
                          [this, features = std::move(features), announce]() mutable {
                              plugin_status_state_.set_features(std::move(features), announce);
                          });
}

void PrinterState::set_macro_option_count(size_t count) {
    macro_option_count_ = count;
    update_gcode_modification_visibility();
}

void PrinterState::update_gcode_modification_visibility() {
    // Delegate to composite visibility component
    bool plugin = plugin_status_state_.service_has_helix_plugin();
    composite_visibility_state_.update_visibility(
        plugin, capabilities_state_, profile_state_.pre_print_option_set().options.size(),
        macro_option_count_);
}

bool PrinterState::is_blocking_operation_active() {
    // Interactive manual probe (PROBE_CALIBRATE / Z_ENDSTOP_CALIBRATE): always
    // blocking. idle_timeout may bounce to Ready between TESTZ commands, so this
    // is tracked independently. This deliberately takes precedence over the
    // file-print exclusion below — a manual probe and a running file print are
    // mutually exclusive in Klipper, so there is no real case where this would
    // wrongly block mid-print.
    if (lv_subject_get_int(calibration_state_.get_manual_probe_active_subject()) != 0) {
        return true;
    }

    // idle_timeout.state == "Printing" is Klipper's canonical busy flag. It is
    // also true during a real file print, so exclude PRINTING/PAUSED — mid-print
    // fan/temp changes are legitimate and Klipper queues them between moves.
    //
    // Debounced, not the raw subject: the flag is equally true for a one-shot
    // housekeeping macro, and a printer with delayed_gcode loops would otherwise
    // refuse a jog for ~7% of its idle life (bundle L53W5PKG).
    if (!calibration_state_.idle_timeout_busy().blocking()) {
        return false;
    }

    // RAW_PRINT_STATE_OK: this predicate is INVERTED — the print state is used
    // to SUPPRESS the blocking answer, not to assert it — so job_holds_machine()
    // would flip it the wrong way. During a host-side pre-print block
    // idle_timeout reads "Printing" (the host is running G-code) while
    // print_stats still reads standby, and answering "blocked" there is correct:
    // the toolhead really is busy. Widening to Preparing would make this return
    // false and ADMIT jogs during the bed mesh.
    const PrintJobState pstate = print_domain_.get_print_job_state();
    return pstate != PrintJobState::PRINTING && pstate != PrintJobState::PAUSED;
}

bool PrinterState::is_external_blocking_operation_active() {
    // Manual probe is an absolute block: TESTZ sessions must never accept
    // jog gcode regardless of how recently the app itself sent motion.
    if (lv_subject_get_int(calibration_state_.get_manual_probe_active_subject()) != 0) {
        return true;
    }
    if (!is_blocking_operation_active()) {
        return false;
    }
    // idle_timeout == "Printing" during any move, including our own jog. When
    // the app's jog started this busy episode the busy-ness is self-inflicted:
    // let discretionary gcode through so jogs don't self-block. Recent or
    // in-flight motion alone proves nothing: a jog that slipped into an external
    // op before it settled would vouch for the next, and that one for the next.
    return !app_motion_activity_.owns_busy_episode(
        calibration_state_.idle_timeout_busy().printing_since());
}

bool PrinterState::is_busy_episode_app_initiated() {
    IdleTimeoutBusy& busy = calibration_state_.idle_timeout_busy();
    return busy.blocking() && app_command_episode_.owns(busy.printing_since());
}

int PrinterState::get_configured_z_offset_microns() {
    if (capabilities_state_.has_probe()) {
        // Probe printers: z_offset stored in ProbeSensorManager (already in microns)
        return lv_subject_get_int(
            helix::sensors::ProbeSensorManager::instance().get_probe_z_offset_subject());
    }
    // Endstop printers: position_endstop from configfile.settings
    return capabilities_state_.get_stepper_z_endstop_microns();
}

void PrinterState::set_kinematics(const std::string& kinematics) {
    if (kinematics == last_kinematics_) {
        return;
    }
    last_kinematics_ = kinematics;

    // On delta printers, axes cannot be homed individually.
    capabilities_state_.set_has_individual_xyz_homing(!circular_bed_kinematics(kinematics));

    // Belt Tension compares the two CoreXY diagonals, so only a belt-path
    // kinematics gets the feature.
    capabilities_state_.set_supports_belt_compare(belt_path_kinematics(kinematics));
    capabilities_state_.bump_hardware_generation();

    // Determine if the bed moves on Z based on kinematics type:
    // - CoreXY: bed typically moves on Z (Voron 0/Trident, Bambu, AD5M, etc.)
    //   Exception: Voron 2.4 and similar with quad_gantry_level have gantry-Z
    // - CoreXZ: gantry moves on Z (Voron Switchwire, etc.) — NOT bed-moves
    // - Cartesian: gantry typically moves on Z (Ender 3, Prusa i3, etc.)
    // - Delta: effector moves on Z, bed is stationary
    bool is_corexy_family = (kinematics.find("corexy") != std::string::npos);

    // CoreXY with QGL = gantry moves on Z (e.g. Voron 2.4), otherwise bed moves
    bool has_qgl = lv_subject_get_int(capabilities_state_.subject(Capability::HasQgl)) != 0;
    auto_detected_bed_moves_ = is_corexy_family && !has_qgl;

    // Apply with user override considered
    apply_effective_bed_moves();
}

void PrinterState::refresh_bed_drying_capability() {
    if (!subjects_initialized_) {
        return;
    }
    const bool enclosed = bed_drying::is_enclosed(
        SettingsManager::instance().get_enclosure_style(), profile_state_.db_enclosed(),
        lv_subject_get_int(capabilities_state_.subject(Capability::HasChamberHeater)) != 0);
    const AxisBounds bounds = motion_state_.get_axis_bounds();
    const bool can_dry = bed_drying::available(
        lv_subject_get_int(capabilities_state_.subject(Capability::HasHeaterBed)) != 0, enclosed,
        bounds.has_z, bounds.z_min, bounds.z_max);
    capabilities_state_.set_bed_drying(enclosed, can_dry);
}

void PrinterState::apply_effective_bed_moves() {
    auto style = SettingsManager::instance().get_z_movement_style();
    bool effective;

    switch (style) {
    case ZMovementStyle::BED_MOVES:
        effective = true;
        break;
    case ZMovementStyle::NOZZLE_MOVES:
        effective = false;
        break;
    case ZMovementStyle::AUTO:
    default:
        effective = auto_detected_bed_moves_;
        break;
    }

    capabilities_state_.set_bed_moves(effective);
    spdlog::debug("[PrinterState] apply_effective_bed_moves: style={}, auto={}, effective={}",
                  static_cast<int>(style), auto_detected_bed_moves_, effective);
}

// ============================================================================
// PRINTER TYPE
// ============================================================================

void PrinterState::set_printer_type_sync(const std::string& type) {
    // Direct call for main-thread use (testing, or when already on main thread)
    set_printer_type_internal(type);
}

void PrinterState::set_z_offset_external_persistence(const std::string& provider_name) {
    // Discovery calls this from the WebSocket thread; the body touches
    // subjects, so it runs on the main thread like every other setter here.
    async_lifetime_.defer(
        "PrinterState::set_z_offset_external_persistence",
        [this, provider_name]() { set_z_offset_external_persistence_internal(provider_name); });
}

void PrinterState::clear_z_offset_external_persistence() {
    async_lifetime_.defer("PrinterState::clear_z_offset_external_persistence",
                          [this]() { clear_z_offset_external_persistence_internal(); });
}

void PrinterState::clear_z_offset_external_persistence_internal() {
    if (!profile_state_.set_external_persistence(false)) {
        return;
    }
    // Two callers with different reasons - rediscovery finding no provider, and
    // a status frame refuting one - so each logs its own reason and this stays
    // neutral about which happened.
    spdlog::info("[PrinterState] No external z-offset persistence provider - Save Z Offset "
                 "returns to the type-derived strategy");
    if (!profile_state_.printer_type().empty()) {
        set_printer_type_internal(profile_state_.printer_type());
    }
}

void PrinterState::set_z_offset_external_persistence_internal(const std::string& provider_name) {
    if (!profile_state_.set_external_persistence(true)) {
        return;
    }
    spdlog::info("[PrinterState] {} persists the z-offset externally - Save Z Offset stands down",
                 provider_name.empty() ? std::string("An installed module") : provider_name);
    // Re-resolve now; set_printer_type_internal also honors the flag on every
    // later type change.
    if (!profile_state_.printer_type().empty()) {
        set_printer_type_internal(profile_state_.printer_type());
    }
}

void PrinterState::set_printer_type_internal(const std::string& type) {
    if (!profile_state_.set_printer_type(type, capabilities_state_.has_probe(),
                                         discovery_.has_exclude_object(), timelapse_available())) {
        return;
    }
    refresh_bed_drying_capability();

    // Probe type override from the database (e.g., prtouch_v2 for K1 series). A type
    // without one sends STANDARD, which clears the previous printer's override.
    const std::string probe_type_str = PrinterDetector::get_probe_type(type);
    helix::sensors::ProbeSensorManager::instance().set_probe_type_override(
        probe_type_str.empty() ? helix::sensors::ProbeSensorType::STANDARD
                               : helix::sensors::probe_type_from_string(probe_type_str));

    // Update printer_has_purge_line_ based on the option set.
    // "priming" is the option id for purge/prime line in the database (also accept legacy
    // "nozzle_priming" as an alias).
    const auto& options = profile_state_.pre_print_option_set();
    bool has_priming =
        (options.find("priming") != nullptr) || (options.find("nozzle_priming") != nullptr);
    capabilities_state_.set_purge_line(has_priming);

    // Does the automatic tool offset calibration make the paper test redundant?
    // Opt-in per printer; the default keeps the paper test for the reference tool.
    capabilities_state_.set_hide_manual_z_calibration(
        PrinterDetector::hide_manual_z_calibration(type));

    // Recalculate composite visibility subjects
    update_gcode_modification_visibility();

    const char* strategy_names[] = {"probe_calibrate", "firmware_managed", "endstop"};
    spdlog::info(
        "[PrinterState] Printer type set to: '{}' (pre_print_options: {}, priming={}, z_cal={})",
        type, options.empty() ? "none" : options.macro_name, has_priming,
        strategy_names[static_cast<int>(profile_state_.z_offset_calibration_strategy())]);
}

void PrinterState::apply_dynamic_options() {
    profile_state_.apply_dynamic_options(
        discovery_.has_exclude_object(), timelapse_available(),
        helix::skip_wrappers::offerable(discovery_.skip_active(), skip_gates_));
}

bool PrinterState::timelapse_available() {
    return lv_subject_get_int(capabilities_state_.subject(Capability::HasTimelapse)) == 1;
}

// ============================================================================
// MULTI-PRINTER STATE
// ============================================================================

void PrinterState::set_active_printer_name(const std::string& name) {
    lv_subject_copy_string(&active_printer_name_, name.c_str());
    spdlog::debug("[PrinterState] Active printer name set to: '{}'", name);
}
