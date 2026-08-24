// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_IFS

#include "ams_backend_ad5x_ifs.h"

#include "ui_temperature_utils.h"

#include "ams_fault_event.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "host_identity.h"
#include "http_executor.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "post_op_cooldown_manager.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_subject_registry.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <lvgl.h>
#include <regex>
#include <sstream>
#include <thread>

using json = nlohmann::json;

namespace {

/// True when the printer's job state is PAUSED.
///
/// Reads the same subject handle_status_update() already uses to pick the
/// Adventurer5M.json poll cadence, so there is one way this backend learns the
/// print state. Null-safe: before PrinterState::init_subjects() (cold boot,
/// non-LVGL unit tests) the accessor returns nullptr and we answer "not paused",
/// which keeps the runout detector inert rather than guessing.
[[nodiscard]] bool print_is_paused() {
    // The LIFECYCLE subject, because the comparison below is against PrintState.
    // These two enums do NOT share numbering — PrintJobState::PAUSED is 2 while
    // PrintState::Paused is 3 — so reading one and casting to the other silently
    // answers "paused" for a COMPLETE job and "not paused" for a real pause.
    if (!get_printer_state().are_subjects_initialized()) {
        return false;
    }
    // PrintState::Paused, not job_holds_machine(): this asks specifically
    // whether the job is PAUSED, which is what makes a head-empty a real runout.
    // Equivalent to the old raw comparison — derive_print_state() excepts PAUSED
    // from the "a live phase outranks the job state" rule, so the two can never
    // disagree — but expressed on the one axis everything else now reads.
    return get_printer_state().get_print_lifecycle() == PrintState::Paused;
}

/// Fallback purge for a runout recovery: 50 mm of fresh filament at 10 mm/s.
/// Same numbers the filament panel's purge fallback uses
/// (ui_panel_filament.cpp, ui_filament_runout_handler.cpp) - deliberately a
/// plain extruder move rather than zmod's `PURGE_FILAMENT` macro, because a bare
/// `G1 E` needs no homing and cannot reach the loadcell `_G28` that shuts the
/// AD5X down mid-job (see build_recovery_actions()).
constexpr int RUNOUT_PURGE_MM = 50;
constexpr int RUNOUT_PURGE_FEEDRATE_MM_MIN = 10 * 60;

/// The ffmColor / ffmType pair to persist into Adventurer5M.json for one slot.
struct FfmSlotFields {
    std::string color; ///< ffmColorN value ("" or "#RRGGBB")
    std::string type;  ///< ffmTypeN value ("?" or a material name)
};

/// zmod's own fallback colour, from `gcmd.get('HEX', '161616')` in
/// zmod_color.py's cmd_RUN_ZCOLOR.
constexpr const char* ZMOD_DEFAULT_HEX = "161616";

/// Map an in-memory slot (hex colour, material) onto the ffmColor/ffmType pair
/// stock ZMOD expects in Adventurer5M.json.
///
/// An unset entry keeps the firmware-native sentinels: live stock ZMOD FFMInfo
/// uses ffmColor='' and ffmType='?' for "no filament". Our in-memory "no colour"
/// placeholder is 808080 (parse_adventurer_json maps an empty ffmColor to it),
/// so both an empty hex and the placeholder mean "no colour".
///
/// A slot that HAS a material never gets an empty ffmColor, because zmod's
/// cmd_RUN_ZCOLOR (zmod_color.py) builds its "Change type" prompt button as
/// `CHANGE_ZCOLOR SLOT=n HEX={ffmColor}` with no TYPE= param. An empty ffmColor
/// makes that literal gcode `CHANGE_ZCOLOR SLOT=n HEX=`, and cmd_CHANGE_ZCOLOR
/// emits `action:prompt_end` BEFORE it validates, then raises because HEX and
/// TYPE are both empty - so the dialog closes and nothing reopens. Writing
/// zmod's own default colour instead of "" sidesteps it. Drop this branch if
/// zmod ever validates before closing the prompt.
[[nodiscard]] FfmSlotFields ffm_fields_for_slot(const std::string& hex,
                                                const std::string& material) {
    const bool no_color = hex.empty() || hex == "808080";
    const bool no_material = material.empty();
    std::string color;
    if (!no_color) {
        color = "#" + hex;
    } else if (!no_material) {
        color = std::string{"#"} + ZMOD_DEFAULT_HEX;
    }
    return {std::move(color), no_material ? std::string{"?"} : material};
}

} // namespace

AmsBackendAd5xIfs::AmsBackendAd5xIfs(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Fill tool map with UNMAPPED_PORT
    tool_map_.fill(UNMAPPED_PORT);
    port_presence_.fill(false);

    // Initialize SlotRegistry with 4 ports in a single unit
    std::vector<std::string> slot_names;
    for (int i = 1; i <= NUM_PORTS; ++i) {
        slot_names.push_back(std::to_string(i));
    }
    slots_.initialize("IFS", slot_names);

    // Set system info capabilities
    system_info_.type = AmsType::AD5X_IFS;
    system_info_.type_name = "AD5X IFS";
    system_info_.total_slots = NUM_PORTS;
    system_info_.supports_bypass = true;
    system_info_.supports_tool_mapping = true;
    // The ENABLE bit only; AVAILABILITY comes from
    // get_endless_spool_capabilities(), which reads the _IFS_VARS latch. False
    // until a plugin's variable_backup is actually seen.
    system_info_.endless_spool_enabled = false;
    system_info_.supports_purge = false;
}

AmsBackendAd5xIfs::~AmsBackendAd5xIfs() = default;

// --- Sensor Ownership (#1054) ---

bool AmsBackendAd5xIfs::owns_filament_sensor(const std::string& bare_name,
                                             const helix::PrinterDiscovery& discovery) {
    (void)discovery; // IFS sensor names are fixed patterns; no discovery needed.
    // Native ZMOD post-hub motion sensor + toolhead switch.
    if (bare_name == "ifs_motion_sensor" || bare_name == "head_switch_sensor") {
        return true;
    }
    // Native Z-Mod publishes these same sensors under custom module namespaces
    // (zmod_ifs_switch_sensor / zmod_ifs_motion_sensor) instead of the stock
    // filament_switch_sensor / filament_motion_sensor sections. The hardware-side
    // strip only removes the stock prefixes, so the bare_name still carries the
    // zmod namespace — claim it by prefix so the head/motion sensors aren't
    // surfaced as generic runout sensors.
    if (bare_name.rfind("zmod_ifs_switch_sensor ", 0) == 0 ||
        bare_name.rfind("zmod_ifs_motion_sensor ", 0) == 0) {
        return true;
    }
    // lessWaste per-port HUB sensors (_ifs_port_sensor_{1..4}) and older ZMOD
    // per-port motion sensors (_ifs_motion_sensor_N).
    return bare_name.rfind("_ifs_port_sensor_", 0) == 0 ||
           bare_name.rfind("_ifs_motion_sensor_", 0) == 0;
}

// --- Lifecycle ---

void AmsBackendAd5xIfs::on_started() {
    // Load persisted per-slot overrides (brand, spool name, spoolman IDs, etc.)
    // BEFORE issuing the initial status query — otherwise the first
    // handle_status_update() callback may fire (on the websocket thread) and
    // update slots before overrides_ is populated, so the first frame of
    // EVENT_STATE_CHANGED would be missing override data. load_blocking runs
    // on this (main) thread; the Moonraker DB callback fires on the libhv
    // event loop, so the two threads don't interfere.
    if (api_) {
        override_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            api_, "ifs", helix::ams::lane_key_style_for(get_type()));
        // Do the (potentially 5s) MR DB round-trip OUTSIDE the lock, then swap in
        // under mutex_. AmsSubscriptionBackend::start() registers the WebSocket
        // notify subscription before on_started() is invoked, so a status
        // notification could in principle fire on the libhv thread while we're
        // still inside load_blocking. Holding mutex_ during the swap ensures
        // the parse path (which reads overrides_ under mutex_) sees a coherent
        // map rather than a torn write.
        auto loaded = override_store_->load_blocking();
        const auto loaded_count = loaded.size();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            overrides_ = std::move(loaded);
        }
        spdlog::info("{} Loaded {} slot overrides from filament_slot store", backend_log_tag(),
                     loaded_count);

        // Restore the last-known seated lane (#1065 power-cycle floor). The
        // firmware forgets the seated channel across a reboot, so this is the only
        // way to relabel Unload/Eject correctly when IFS_STATUS returns Chan=0 with
        // a lane still physically at the head. Range-check against NUM_PORTS — a
        // stale/corrupt value is discarded rather than trusted.
        std::optional<int> seated = override_store_->load_seated_slot_blocking();
        if (seated.has_value() && (*seated < 0 || *seated >= NUM_PORTS)) {
            seated.reset();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            persisted_seated_slot_ = seated;
        }
        if (seated.has_value()) {
            spdlog::info("{} Restored remembered seated lane: slot {}", backend_log_tag(), *seated);
        }
    }

    // Resolve the on-disk Adventurer5M.json path so write_adventurer_json()
    // can bypass the broken Moonraker upload path. Done after override-store
    // setup so failure here doesn't block override loading. Safe no-op when
    // we're remote, the file isn't present, or it isn't writable.
    detect_local_adventurer_json_path();

    // Query initial state from printer
    if (!client_)
        return;

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query",
        json{{"objects",
              json{{"save_variables", nullptr},
                   // Verify _IFS_VARS macro actually exists (not just save_variables data)
                   {"gcode_macro _ifs_vars", nullptr},
                   // lessWaste plugin: per-port filament switch sensors
                   {"filament_switch_sensor _ifs_port_sensor_1", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_2", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_3", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_4", nullptr},
                   // Shared: head switch sensor (both lessWaste and native ZMOD)
                   {"filament_switch_sensor head_switch_sensor", nullptr},
                   // Native ZMOD IFS: single motion sensor (replaces per-port sensors)
                   {"filament_motion_sensor ifs_motion_sensor", nullptr},
                   // Native Z-Mod custom-module namespaces — the live AD5X pushes
                   // its head switch + motion sensor under zmod_ifs_* sections
                   // (no stock filament_*_sensor section exists), so query both.
                   {"zmod_ifs_switch_sensor head_switch_sensor", nullptr},
                   {"zmod_ifs_motion_sensor ifs_motion_sensor", nullptr},
                   // Extruder temp/target — primary signal for the load/unload
                   // phase tracker (HEATING completion). Without this the IFS
                   // unload silently heats for ~2.5 min with no feedback.
                   {"extruder", json::array({"target", "temperature"})},
                   // Klippy state — GET_ZCOLOR SILENT=1 only works once zmod is
                   // initialised, so we gate the initial query on webhooks.state == "ready".
                   {"webhooks", nullptr}}}},
        [this, token](const json& response) {
            // BG THREAD: extract local copies of what we need; no `this` access.
            // save_variables may contain lessWaste/bambufy data from a partially
            // installed plugin, but the macro itself might not be loaded in
            // Klipper — we still flag macro_exists so the main-thread apply can
            // update the latch correctly.
            bool macro_exists = false;
            bool klippy_ready = false;
            bool have_status = false;
            json status_copy;
            if (response.contains("result") && response["result"].contains("status")) {
                const auto& status = response["result"]["status"];
                // Klipper/Kalico's webhooks _do_query() (klippy/webhooks.py)
                // returns `{}` for objects that don't exist — the key is
                // still present in the response. So `status.contains(...)` is
                // always true after a successful query, even on stock zmod
                // without lessWaste/bambufy. Distinguish a real macro by the
                // presence of its variables: lessWaste/bambufy's _IFS_VARS
                // macro declares variable_colors / variable_tools / etc., so
                // a real macro's get_status() returns a non-empty dict.
                //
                // Only the existence bool is extracted here; the dict itself
                // survives into status_copy below and handle_status_update()
                // reads its `variable_*` payload (parse_ifs_vars_macro_locked).
                macro_exists = status.contains("gcode_macro _ifs_vars") &&
                               status["gcode_macro _ifs_vars"].is_object() &&
                               !status["gcode_macro _ifs_vars"].empty();
                if (status.contains("webhooks") && status["webhooks"].contains("state") &&
                    status["webhooks"]["state"].is_string()) {
                    klippy_ready = status["webhooks"]["state"].get<std::string>() == "ready";
                }
                status_copy = status;
                have_status = true;
            }

            // MAIN THREAD: apply latch, run handle_status_update, log, and
            // launch the follow-up chain (each call accesses api_/client_).
            token.defer("Ad5xIfsBackend::on_started_apply",
                        [this, macro_exists, klippy_ready, have_status,
                         status_copy = std::move(status_copy)]() mutable {
                            if (have_status) {
                                // Update latch BEFORE handle_status_update so
                                // parse_save_variables sees the correct state.
                                // ifs_macro_confirmed_missing_ starts true
                                // (pessimistic) and is only cleared here when
                                // the macro exists.
                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    if (macro_exists) {
                                        ifs_macro_confirmed_missing_ = false;
                                    }
                                    // !macro_exists: latch stays true
                                }

                                handle_status_update(status_copy);
                            }

                            // Log initial state after processing query response
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                spdlog::debug("{} Initial query: has_ifs_vars={}, macro_exists={}, "
                                              "klippy_ready={}, has_per_port_sensors={}, "
                                              "head_filament={}, "
                                              "port_presence=[{},{},{},{}], "
                                              "colors=[{},{},{},{}]",
                                              backend_log_tag(), has_ifs_vars_, macro_exists,
                                              klippy_ready, has_per_port_sensors_, head_filament_,
                                              port_presence_[0], port_presence_[1],
                                              port_presence_[2], port_presence_[3], colors_[0],
                                              colors_[1], colors_[2], colors_[3]);
                            }

                            // Safety net: if parse_save_variables somehow set
                            // has_ifs_vars_ despite the latch (shouldn't
                            // happen), force it back off for missing macros.
                            // has_ifs_vars_ now only gates tool-mapping /
                            // current_tool / external reads from
                            // save_variables — color/type writes use
                            // CHANGE_ZCOLOR for every user regardless.
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                if (!macro_exists && has_ifs_vars_) {
                                    spdlog::warn("{} save_variables contain {}_ data but _IFS_VARS "
                                                 "macro not found — clearing has_ifs_vars_ "
                                                 "(tool-mapping from save_variables disabled)",
                                                 backend_log_tag(), var_prefix_);
                                    has_ifs_vars_ = false;
                                }
                            }

                            // Adventurer5M.json + GET_ZCOLOR SILENT=1 are
                            // zmod's authoritative color/type sources for ALL
                            // AD5X IFS users. lessWaste/bambufy _IFS_VARS
                            // save_variables (less_waste_colors /
                            // bambufy_colors) live in a private namespace zmod
                            // does not read, so we never trust them for
                            // color/type — only for tool-mapping and friends.
                            // Register the listeners + fire the initial query
                            // unconditionally.
                            spdlog::info("{} Reading Adventurer5M.json + GET_ZCOLOR SILENT=1 for "
                                         "color truth",
                                         backend_log_tag());
                            read_adventurer_json();
                            // One-shot fetch of zmod's user-defined material
                            // types from /mod_data/user.cfg. Independent of
                            // the json/gcode pipelines — best-effort, 404 on
                            // non-zmod printers is silent.
                            fetch_user_cfg_materials();
                            // One-shot fetch of zmod's per-filament-type
                            // unload table (/mod_data/filament.json) so
                            // eject_lane() retracts the full tube length at the
                            // configured speed. Best-effort; 404 on non-zmod
                            // printers is silent and falls back to 1000/1200.
                            fetch_filament_json();
                            register_zcolor_listener();
                            // notify_klippy_ready catches startup and
                            // FIRMWARE_RESTART; it's the point at which zmod
                            // is initialised and GET_ZCOLOR returns populated
                            // results.
                            register_klippy_ready_listener();
                            if (klippy_ready) {
                                schedule_zcolor_query("startup");
                            } else {
                                spdlog::info("{} Deferring GET_ZCOLOR SILENT=1 until klippy ready",
                                             backend_log_tag());
                            }
                        });
        });
}

void AmsBackendAd5xIfs::on_stopping() {
    unregister_moonraker_listeners();
}

void AmsBackendAd5xIfs::request_resync() {
    spdlog::info("{} request_resync(): re-reading Adventurer5M.json + GET_ZCOLOR",
                 backend_log_tag());
    read_adventurer_json();
    schedule_zcolor_query("manual_resync");
}

// --- Status parsing ---

void AmsBackendAd5xIfs::handle_status_update(const json& notification) {
    // notify_status_update has format: { "method": "notify_status_update", "params": [{ ... },
    // timestamp] }
    // Initial query response sends unwrapped status directly — handle both formats.
    const json* status = &notification;
    if (notification.contains("params") && notification["params"].is_array() &&
        !notification["params"].empty()) {
        status = &notification["params"][0];
        if (!status->is_object()) {
            return;
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);

    bool state_changed = false;
    bool sensor_changed = false;

    // Parse save_variables if present
    if (status->contains("save_variables")) {
        const auto& sv = (*status)["save_variables"];
        if (sv.contains("variables") && sv["variables"].is_object()) {
            parse_save_variables(sv["variables"]);
            state_changed = true;
        }
    }

    // The _IFS_VARS macro's own get_status() dict. on_started()'s probe used to
    // reduce this whole payload to a single "does the macro exist" bool and
    // throw the rest away; the plugin's live settings (notably `variable_backup`,
    // the auto-switch-to-a-backup-spool toggle) are in here and are the only
    // Moonraker-visible answer to "will this printer recover from a runout by
    // itself?" (#1250, reported as #1247).
    if (status->contains("gcode_macro _ifs_vars")) {
        if (parse_ifs_vars_macro_locked((*status)["gcode_macro _ifs_vars"])) {
            state_changed = true;
        }
    }

    // Parse per-port filament sensors
    // Leading space in sensor name is intentional — Klipper object naming convention
    for (int port = 1; port <= NUM_PORTS; ++port) {
        std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port);
        if (status->contains(key)) {
            const auto& sensor = (*status)[key];
            if (sensor.contains("filament_detected") && sensor["filament_detected"].is_boolean()) {
                parse_port_sensor(port, sensor["filament_detected"].get<bool>());
                state_changed = true;
                sensor_changed = true;
            }
        }
    }

    // Native ZMOD: when a port sensor changes, the user may have swapped filament.
    // Schedule a re-read of Adventurer5M.json to pick up any color/type changes.
    if (sensor_changed && !has_ifs_vars_) {
        lock.unlock();
        schedule_json_reread();
        lock.lock();
    }

    // A sensor object only counts as a real reading when it actually carries a
    // boolean filament_detected. On a fresh boot Moonraker can return the stock
    // filament_*_sensor object as a present-but-empty {} compat view while the
    // live zmod_ifs_* sensor carries the real field; choosing a key purely by
    // presence would let the empty stock object win and the real zmod reading be
    // ignored (#1065 boot scenario). Prefer whichever namespace carries the
    // field, stock first when both do (the historical order).
    const auto has_reading = [&](const char* key) {
        return status->contains(key) && (*status)[key].is_object() &&
               (*status)[key].contains("filament_detected") &&
               (*status)[key]["filament_detected"].is_boolean();
    };

    // Native ZMOD IFS: single motion sensor replaces per-port presence sensors.
    // Maps to head_filament_ since it detects filament at the toolhead. The live
    // AD5X publishes this under the zmod_ifs_motion_sensor namespace rather than
    // the stock filament_motion_sensor section, so accept either key.
    const char* motion_key = has_reading("filament_motion_sensor ifs_motion_sensor")
                                 ? "filament_motion_sensor ifs_motion_sensor"
                                 : (has_reading("zmod_ifs_motion_sensor ifs_motion_sensor")
                                        ? "zmod_ifs_motion_sensor ifs_motion_sensor"
                                        : nullptr);
    if (motion_key) {
        const auto& motion = (*status)[motion_key];
        if (motion.contains("filament_detected") && motion["filament_detected"].is_boolean()) {
            bool detected = motion["filament_detected"].get<bool>();
            bool was = head_filament_;
            parse_head_sensor(detected);
            // During a purge, ifs_motion_sensor activity means filament is still
            // moving — the purge is progressing, not stalled. Reset the timeout
            // clock so a long-but-healthy purge isn't falsely failed; the PURGING
            // budget then measures time since the last motion (#1065 Bug 2).
            if (system_info_.action == AmsAction::PURGING && detected) {
                action_start_time_ = std::chrono::steady_clock::now();
            }
            // Motion-sensor activity means filament is still moving — a genuine
            // progress signal for the indeterminate detector, for any phase of a
            // tracked op (#1065 row 14), so a busy-but-moving op never reads as a
            // frozen "Working…".
            if (phase_tracker_.active && detected) {
                note_phase_progress_locked();
            }
            // Phase tracker (active op WE started) advances the phase on a head
            // transition. detect_load_unload_completion preserves legacy
            // snap-to-IDLE when the tracker is inactive.
            if (phase_tracker_.active && was != detected) {
                on_head_transition_locked(detected);
            }
            detect_load_unload_completion(detected);
            state_changed = true;
        }
    }

    // Parse head sensor. The live AD5X publishes this under the
    // zmod_ifs_switch_sensor namespace rather than the stock
    // filament_switch_sensor section, so accept either key (switch semantics).
    const char* head_key = has_reading("filament_switch_sensor head_switch_sensor")
                               ? "filament_switch_sensor head_switch_sensor"
                               : (has_reading("zmod_ifs_switch_sensor head_switch_sensor")
                                      ? "zmod_ifs_switch_sensor head_switch_sensor"
                                      : nullptr);
    if (head_key) {
        const auto& head = (*status)[head_key];
        if (head.contains("filament_detected") && head["filament_detected"].is_boolean()) {
            bool detected = head["filament_detected"].get<bool>();
            bool was = head_filament_;
            parse_head_sensor(detected);
            // Latch the SWITCH sensor's own authority, separate from the conflated
            // head_filament_ (which the motion sensor also writes, false-negating
            // while loaded-idle). Only the switch's reading can authoritatively say
            // the head is empty for the #1065 row 28 head-gate and for the runout
            // detector (#1250). Maintain the runout edge state BEFORE overwriting
            // the latch - note_head_switch_reading_locked needs the previous value.
            note_head_switch_reading_locked(detected);
            head_switch_seen_ = true;
            head_switch_present_ = detected;
            if (phase_tracker_.active && was != detected) {
                on_head_transition_locked(detected);
            }
            detect_load_unload_completion(detected);
            state_changed = true;
        }
    }

    // Parse extruder temp/target — primary HEATING-completion signal for the
    // load/unload phase tracker. High-frequency, so only mark state_changed
    // when the synthesized action actually moves (mirrors CFS).
    if (status->contains("extruder")) {
        const auto& extr = (*status)["extruder"];
        if (extr.contains("target") && extr["target"].is_number()) {
            last_extruder_target_deci_ = helix::units::to_decidegrees(extr["target"].get<double>());
        }
        if (extr.contains("temperature") && extr["temperature"].is_number()) {
            last_extruder_temp_deci_ =
                helix::units::to_decidegrees(extr["temperature"].get<double>());
        }
        if (phase_tracker_.active) {
            AmsAction before = system_info_.action;
            std::string detail_before = system_info_.operation_detail;
            on_extruder_temp_locked(last_extruder_temp_deci_, last_extruder_target_deci_);
            if (system_info_.action != before || system_info_.operation_detail != detail_before) {
                state_changed = true;
            }
        }
    }

    // Update system info from cached state
    if (state_changed) {
        system_info_.current_tool = active_tool_;
        system_info_.filament_loaded = head_filament_;

        // Map current tool to current slot
        recompute_current_slot_locked();

        // Update all slot states
        for (int i = 0; i < NUM_PORTS; ++i) {
            update_slot_from_state(i);
        }
    }

    // Check for stuck operations on every status update. Capture the
    // indeterminate flag across the call: a frozen-feed frame that changed
    // nothing else can still flip "Working…" on/off, and the UI only sees it via
    // an EVENT_STATE_CHANGED -> sync_from_backend refresh, so treat a toggle as a
    // state change worth publishing (#1065 row 14).
    const bool indet_before = system_info_.operation_indeterminate;
    check_action_timeout();
    const bool indet_toggled = system_info_.operation_indeterminate != indet_before;

    // Unattended-runout detector (#1250). Deliberately AFTER check_action_timeout:
    // a timed-out operation owns the ERROR state, and the runout predicate
    // requires action == IDLE so the two can never fight over it. Its own return
    // value drives the emit below - the dwell means the raise usually lands on a
    // status frame that changed nothing else, and without this the action would
    // flip to ERROR with no EVENT_STATE_CHANGED to carry it to the UI.
    const bool runout_changed = evaluate_runout_locked();

    // Consume the #1247 repair request under the same lock hold that staged
    // it; dispatch below with the lock released (execute_gcode blocks).
    const bool repair_staged = ifs_vars_repair_staged_;
    ifs_vars_repair_staged_ = false;

    lock.unlock();

    if (repair_staged) {
        dispatch_ifs_vars_repair();
    }

    // No AD5X-specific plugin subjects to publish: the auto-switchover state is
    // now carried by the backend-neutral `ams_endless_state` / `ams_endless_text`
    // subjects, which AmsState derives from get_endless_spool_capabilities() when
    // it handles the EVENT_STATE_CHANGED below.
    if (state_changed || indet_toggled || runout_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }

    // Freshness backstop: zmod can mutate Adventurer5M.json via the on-printer
    // "Select print materials" dialog without echoing a CHANGE_ZCOLOR token
    // through notify_gcode_response (zmod's dialog handler is closed-source).
    //
    // We poll the JSON content over HTTP (invisible to the gcode console) and
    // only fire GET_ZCOLOR when the content actually changed. The previous
    // unconditional 15s GET_ZCOLOR backstop was producing 1-2 GET_ZCOLOR/s
    // on connected printers (raza/DIEHARDave report on v0.99.51) and made the
    // Mainsail/Fluidd console unusable. Piggybacking on notify_status_update
    // gives us sub-second cadence; rate-limit via steady_clock so the actual
    // download fires no more often than the interval below.
    //
    // The interval slows to 30s while actively printing. FFMInfo carries only
    // per-slot colour and material labels — cosmetic metadata — and measured
    // against a real AD5X session the tight cadence earned nothing: 3902
    // downloads produced 3 content changes, two of which were the first poll
    // after boot establishing its baseline. Meanwhile each fetch is a loopback
    // HTTP GET on a 2-core board that is simultaneously feeding the MCU step
    // queue, and 'Timer too close' shutdowns are exactly what host starvation
    // there looks like. PAUSED deliberately keeps the 5s cadence: a pause is
    // when a user actually swaps a spool and relabels it.
    // RAW_PRINT_STATE_OK: picks the poll cadence from what the MCU is actually
    // doing. During a host-side block the board is not yet feeding the step
    // queue, so the fast cadence is still the right choice there.
    bool printing_now = false;
    if (get_printer_state().are_subjects_initialized()) {
        printing_now = get_printer_state().get_print_job_state() == helix::PrintJobState::PRINTING;
    }

    auto now = std::chrono::steady_clock::now();
    if (should_poll_json(printing_now, json_poll_was_printing_, now - last_json_poll_kick_)) {
        last_json_poll_kick_ = now;
        poll_adventurer_json();
    }
    json_poll_was_printing_ = printing_now;
}

bool AmsBackendAd5xIfs::should_poll_json(bool printing_now, bool was_printing,
                                         std::chrono::steady_clock::duration since_last) {
    // printing->anything else: poll immediately rather than waiting out the slow
    // interval, so the firmware's post-print FFMInfo revert is seen as promptly
    // as it was before the backoff existed.
    if (was_printing && !printing_now) {
        return true;
    }
    return since_last >= (printing_now ? JSON_POLL_PRINTING : JSON_POLL_IDLE);
}

void AmsBackendAd5xIfs::parse_save_variables(const json& vars) {
    // Auto-detect variable prefix: lessWaste/zmod uses "less_waste_*", bambufy uses "bambufy_*".
    // Check once per status update — the prefix can't change at runtime, but we may not
    // see both sets of variables in the initial query.
    if (vars.contains("bambufy_colors") || vars.contains("bambufy_tools")) {
        if (var_prefix_ != "bambufy") {
            var_prefix_ = "bambufy";
            spdlog::debug("{} Detected bambufy variable prefix", backend_log_tag());
        }
        if (!ifs_macro_confirmed_missing_) {
            has_ifs_vars_ = true;
        }
    } else if (vars.contains("less_waste_colors") || vars.contains("less_waste_tools")) {
        if (var_prefix_ != "less_waste") {
            var_prefix_ = "less_waste";
            spdlog::debug("{} Detected lessWaste variable prefix", backend_log_tag());
        }
        if (!ifs_macro_confirmed_missing_) {
            has_ifs_vars_ = true;
        }
    }

    // User-defined material types from bambufy_custom_types. Surfaced via
    // get_supported_materials() so the edit modal's dropdown isn't restricted
    // to the firmware whitelist (PLA, PLA-CF, SILK, TPU, ABS, PETG, PETG-CF) —
    // PLA+, rPLA, PETG-Pro etc. round-trip cleanly through zmod's COLOR macro
    // and don't get silently normalized to PLA on save (#904 root cause #2).
    // Always read regardless of has_ifs_vars_: user-defined types are
    // orthogonal to plugin activation.
    if (vars.contains("bambufy_custom_types") && vars["bambufy_custom_types"].is_array()) {
        std::vector<std::string> staged;
        for (const auto& t : vars["bambufy_custom_types"]) {
            if (t.is_string()) {
                std::string name = t.get<std::string>();
                if (!name.empty()) {
                    staged.push_back(std::move(name));
                }
            }
        }
        size_t count = staged.size();
        {
            std::lock_guard<std::mutex> lock(custom_types_mutex_);
            custom_material_types_ = std::move(staged);
        }
        if (count > 0) {
            spdlog::debug("{} Loaded {} bambufy_custom_types entries", backend_log_tag(), count);
        }
    }

    const std::string p = var_prefix_;

    // NOTE on colors/types: <prefix>_colors and <prefix>_types live in the
    // lessWaste/bambufy plugin's PRIVATE save_variables namespace. zmod does
    // NOT read them — its authoritative color/type store is its own in-memory
    // state, persisted to Adventurer5M.json, mutated only by CHANGE_ZCOLOR.
    // Trusting <prefix>_colors here was poisoning colors_[]/materials_[] with
    // values that diverged silently from zmod's truth (raza's debug bundle
    // ZYYRVVTG showed Adventurer5M.json and less_waste_colors out of sync).
    // Color/type reads now come from GET_ZCOLOR SILENT=1 (live) and
    // Adventurer5M.json (boot snapshot) only. set_slot_info() correspondingly
    // writes via CHANGE_ZCOLOR rather than _IFS_VARS, so the dirty_-clearing
    // round-trip that lived in this branch is no longer needed.
    //
    // The fields below — tools, current_tool, external — DO live only in the
    // plugin's save_variables namespace (no other Moonraker-visible source),
    // so we keep reading them — but ONLY when the plugin is actually active
    // (has_ifs_vars_ requires both the prefix detection above AND the live
    // gcode_macro _ifs_vars existence check from on_started). save_variables
    // rows persist in printer_data/database/ even after a plugin uninstall;
    // pre-fix, a user who removed lessWaste/bambufy but left the rows behind
    // would silently keep using the stale tool map and last active-tool guess
    // forever. The gate makes "macro present" load-bearing for trusting the
    // plugin's data, matching the contract has_ifs_vars_ already advertises.
    if (has_ifs_vars_) {
        // Tool mapping: 16-element array, index=tool, value=port (1-4, 5=unmapped).
        //
        // Both-prefixes-conflict guard (#904): TMTYD's printer had bambufy_tools=
        // [4,2,4,3,...] AND less_waste_tools=[2,1,3,4] left over from past plugin
        // activations, with NEITHER plugin currently driving state (only nopoop
        // active). Our prefix-detect picks bambufy first and applied [4,2,4,3,...]
        // — putting T0 on port 4 and breaking every per-port T-badge in the UI.
        //
        // Rule: if both prefixes have _tools arrays AND they disagree, neither is
        // authoritative. Fall back to the default 1:1 mapping (T0→port1, T1→port2,
        // …) and skip current_tool / external reads too — those came from the same
        // poisoned source.
        //
        // Single-prefix-stale-data is NOT handled here — if only bambufy_* exists
        // (or only less_waste_*) and the plugin has been uninstalled, the
        // ifs_macro_confirmed_missing_ latch from on_started's gcode_macro
        // existence check catches it: has_ifs_vars_ stays false and we never
        // reach this branch. The guard below only matters for the
        // both-installed-then-deactivated scenario.
        const std::string other_p = (p == "bambufy") ? "less_waste" : "bambufy";
        const bool have_self_tools = vars.contains(p + "_tools") && vars[p + "_tools"].is_array();
        const bool have_other_tools =
            vars.contains(other_p + "_tools") && vars[other_p + "_tools"].is_array();
        bool conflict = false;
        if (have_self_tools && have_other_tools) {
            conflict = vars[p + "_tools"] != vars[other_p + "_tools"];
        }
        if (conflict) {
            spdlog::warn("{} Both bambufy_tools and less_waste_tools present and disagree "
                         "— stale data from a deactivated plugin; falling back to default "
                         "1:1 tool mapping",
                         backend_log_tag());
            tool_map_.fill(UNMAPPED_PORT);
            for (int t = 0; t < NUM_PORTS; ++t) {
                tool_map_[static_cast<size_t>(t)] = t + 1;
            }
            for (int i = 0; i < NUM_PORTS; ++i) {
                int tool = find_first_tool_for_port(i + 1);
                slots_.set_tool_mapping(i, tool);
            }
            for (int i = 0; i < NUM_PORTS; ++i) {
                update_slot_from_state(i);
            }
            return;
        }

        if (have_self_tools) {
            const auto& tools = vars[p + "_tools"];
            for (size_t i = 0; i < std::min(tools.size(), static_cast<size_t>(TOOL_MAP_SIZE));
                 ++i) {
                if (tools[i].is_number_integer()) {
                    tool_map_[i] = tools[i].get<int>();
                }
            }
        }

        // #1247 shape guard: lessWaste's `_colors`/`_types` arrays are
        // TOOL_MAP_SIZE-entry tool-indexed state, but our mirror bug pushed
        // 4-entry port-indexed lists — and `_IFS_VARS` replaces the arrays
        // wholesale, so every push truncated them (after which `_RUNOUT_HEAD`'s
        // scan of tools 4..15 could never find a backup lane). SAVE_VARIABLE
        // persists the damage across reboots, and lessWaste's own start dialog
        // only re-heals it until our next push. Detect the truncated shape and
        // stage a repair; handle_status_update() dispatches it with mutex_
        // released. bambufy arrays legitimately hold 4 entries, so the check is
        // lessWaste-only. Not an array (string/absent) means the plugin never
        // wrote the row or uses another form — leave it alone.
        if (p == "less_waste" && !ifs_vars_repair_staged_) {
            for (const std::string key : {p + "_colors", p + "_types"}) {
                const auto it = vars.find(key);
                if (it != vars.end() && it->is_array() &&
                    it->size() < static_cast<size_t>(TOOL_MAP_SIZE)) {
                    spdlog::warn("{} {} carries {} entries; lessWaste expects {} "
                                 "tool-indexed — staging _IFS_VARS repair (#1247)",
                                 backend_log_tag(), key, it->size(), TOOL_MAP_SIZE);
                    ifs_vars_repair_staged_ = true;
                    break;
                }
            }
        }

        // Current tool (-1 = none, 0-15 = tool number)
        if (vars.contains(p + "_current_tool") && vars[p + "_current_tool"].is_number_integer()) {
            active_tool_ = vars[p + "_current_tool"].get<int>();
        }

        // External/bypass mode (0 or 1)
        if (vars.contains(p + "_external") && vars[p + "_external"].is_number_integer()) {
            external_mode_ = (vars[p + "_external"].get<int>() != 0);
        }

        // Rebuild SlotRegistry tool mapping from IFS tool_map_. Only meaningful
        // when tool_map_ was populated above; without an active plugin the map
        // is whatever default values the registry was constructed with, and
        // running this loop would lock those defaults in over real data that
        // might arrive later via apply_zcolor_result's extruder_slot path.
        for (int i = 0; i < NUM_PORTS; ++i) {
            int tool = find_first_tool_for_port(i + 1); // port is 1-based
            slots_.set_tool_mapping(i, tool);
        }
    }

    // Sync all slots from cached state
    for (int i = 0; i < NUM_PORTS; ++i) {
        update_slot_from_state(i);
    }
}

void AmsBackendAd5xIfs::parse_port_sensor(int port_1based, bool detected) {
    int slot = port_1based - 1;
    if (slot >= 0 && slot < NUM_PORTS) {
        bool was_first = !has_per_port_sensors_;
        bool changed = port_presence_[static_cast<size_t>(slot)] != detected;
        has_per_port_sensors_ = true;
        port_presence_[static_cast<size_t>(slot)] = detected;
        if (was_first || changed) {
            spdlog::debug("{} Port {} sensor: {} (per_port_sensors=true{})", backend_log_tag(),
                          port_1based, detected ? "present" : "empty",
                          was_first ? ", first detection" : "");
        }
    }
}

void AmsBackendAd5xIfs::parse_head_sensor(bool detected) {
    if (head_filament_ != detected) {
        spdlog::debug("{} Head sensor: {}", backend_log_tag(),
                      detected ? "filament detected" : "no filament");
    }
    head_filament_ = detected;
}

bool AmsBackendAd5xIfs::head_empty_for_unload_routing_locked() const {
    // Positive switch evidence is required to claim "empty". See the header for
    // the full rationale, the error asymmetry, and the `filamentValue` ADC that
    // is the firmware's actual predicate.
    if (head_switch_seen_) {
        return !head_switch_present_;
    }
    return !head_filament_;
}

void AmsBackendAd5xIfs::update_slot_from_state(int slot_index) {
    if (slot_index < 0 || slot_index >= NUM_PORTS)
        return;

    auto* entry = slots_.get_mut(slot_index);
    if (!entry)
        return;

    auto idx = static_cast<size_t>(slot_index);

    // Color: parse hex string to uint32_t
    if (!colors_[idx].empty()) {
        try {
            entry->info.color_rgb = static_cast<uint32_t>(std::stoul(colors_[idx], nullptr, 16));
        } catch (...) {
            // Invalid hex — leave color unchanged
        }
    }

    // Material
    entry->info.material = materials_[idx];

    // Status based on sensor and active state
    bool is_active_slot = (system_info_.current_slot == slot_index);
    bool has_filament = port_presence_[idx];

    SlotStatus prev_status = entry->info.status;
    // The seated lane is LOADED whenever the head sensor sees filament,
    // WITHOUT requiring the lane's own port sensor. Two reasons:
    //
    //  - A runout drops port_presence_ while the filament that lane already fed
    //    is still in the toolhead (#995) — the state can_unload_from_toolhead()
    //    keeps the unload gate open for. Requiring the port sensor demoted the
    //    lane to EMPTY at exactly the moment the user needs to recover it.
    //  - Native ZMOD publishes no per-port sensors at all, so port_presence_ is
    //    false for every lane; this is what keeps the seated one off EMPTY
    //    (previously done by forcing has_filament true for that one case).
    //
    // head_filament_ is also what system_info_.filament_loaded is assigned from,
    // so the per-slot status and the aggregate pair now agree by construction —
    // the precondition for has_per_slot_loaded_authority().
    if (is_active_slot && head_filament_) {
        entry->info.status = SlotStatus::LOADED;
    } else if (has_filament) {
        entry->info.status = SlotStatus::AVAILABLE;
    } else {
        entry->info.status = SlotStatus::EMPTY;
    }

    if (entry->info.status != prev_status) {
        spdlog::debug("{} Slot {} status: {} → {} (port_presence={}, active={}, head={}, "
                      "per_port_sensors={}, color={}, material={})",
                      backend_log_tag(), slot_index, static_cast<int>(prev_status),
                      static_cast<int>(entry->info.status), port_presence_[idx], is_active_slot,
                      head_filament_, has_per_port_sensors_, colors_[idx], materials_[idx]);
    }

    // Reverse tool mapping: find first tool that maps to this port
    entry->info.mapped_tool = find_first_tool_for_port(slot_index + 1);

    // External-edit detection MUST run BEFORE apply_overrides. entry->info
    // .color_rgb is firmware-truth here IF colors_[idx] was non-empty above;
    // after apply_overrides it would be masked by the (possibly stale)
    // override and we'd miss the delta vs. the prior firmware baseline.
    //
    // When colors_[idx] is empty we have NO firmware reading yet —
    // entry->info.color_rgb is whatever was left there by the SlotInfo
    // default (AMS_DEFAULT_SLOT_COLOR / 0x808080) or a prior apply_overrides
    // leak. Pass nullopt (the helper's explicit "no reading" signal) so we
    // don't establish a phantom baseline that would later be misread as an
    // external edit. Boot path: parse_save_variables / handle_status_update
    // call update_slot_from_state BEFORE parse_adventurer_json fills in
    // colors_[]; pre-fix this populated a 0x808080 baseline, then the first
    // real parse triggered a bogus sync.
    //
    // When colors_[idx] is non-empty, pass the parsed value AS-IS — including
    // 0 for pure black. The helper accepts any uint32_t inside the optional
    // as a real reading; only nullopt means "no reading" (replaces the prior
    // ambiguous 0-as-no-signal sentinel that silently dropped black).
    std::optional<uint32_t> observed_color =
        colors_[idx].empty() ? std::nullopt : std::optional<uint32_t>{entry->info.color_rgb};
    // Pass slot_has_filament so the helper skips creating a phantom override
    // when a slot read came back as the empty-placeholder #808080 — the eject
    // path in parse_adventurer_json clears the override explicitly.
    check_external_color_change(slot_index, observed_color, port_presence_[idx]);
    // Material counterpart: a type-only firmware edit (same color) doesn't trip
    // the color detector, so a non-locked override's baked material would go
    // stale and mask firmware truth (#981/#1065 — color updated, type stuck).
    check_external_type_change(slot_index, materials_[idx], observed_color, port_presence_[idx]);

    // Layer user-configured overrides on top of firmware-reported data. Called
    // last so overrides win for any non-default field. Callers hold mutex_,
    // which also covers overrides_ writes from on_started() and set_slot_info()
    // — see apply_overrides() below for the invariant.
    apply_overrides(entry->info, slot_index);
}

void AmsBackendAd5xIfs::apply_overrides(SlotInfo& slot, int slot_index) {
    // overrides_ is mutated in on_started() (initial load) and set_slot_info()
    // (persisted user edit). Both writers hold mutex_, and every caller of
    // apply_overrides runs inside update_slot_from_state() under mutex_ — so
    // the map is implicitly lock-protected here. If a slot has no override
    // entry, this is a zero-cost hash lookup followed by early return — safe
    // to call inside the hot parse path. The whole spec §5 policy + the
    // re-bind/eject rules live in helix::ams::merge_override — the single
    // implementation every backend shares. Rule 1 (re-bind) is NOT gated by
    // the capability: it can fire on any backend whose firmware reports a
    // positive spool id disagreeing with the override (AFC, Happy Hare,
    // flat-schema CFS). IFS firmware never reports one, so Rule 1 cannot
    // fire here today — but that is a fact about this firmware, not what
    // the capability gates. Rule 2 (eject) IS what
    // printer_reports_spool_ids() gates (base false here: 0 is IFS's
    // everyday reading, never an eject), and the erase branch is correct
    // tomorrow if a firmware ever starts reporting ids.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end())
        return;
    helix::ams::MergeOptions opts;
    opts.printer_reports_spool_ids = printer_reports_spool_ids();
    opts.keep_spool_info_on_eject =
        helix::SettingsManager::instance().get_ams_keep_spool_info_on_eject();
    // Own-write echo suppression (SlotFingerprintTracker::expect()
    // semantics): Rule 1 must not read an in-flight stale firmware id as an
    // external re-bind. IFS never writes firmware ids, so this is always
    // {0, 0} today — the call keeps one shape across backends.
    const auto [own_old_id, own_new_id] = own_write_expectation(slot_index, slot.spoolman_id);
    opts.suppress_rebind_firmware_old_id = own_old_id;
    opts.suppress_rebind_firmware_new_id = own_new_id;
    const auto result = helix::ams::merge_override(slot, it->second, opts);
    if (result.cleared_rebind || result.cleared_eject) {
        overrides_.erase(it);
        if (override_store_) {
            const std::string tag = backend_log_tag();
            override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
                if (!ok) {
                    spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
                }
            });
        }
    }
}

bool AmsBackendAd5xIfs::check_external_color_change(int slot_index,
                                                    std::optional<uint32_t> observed_color,
                                                    bool slot_has_filament) {
    // observed_color is whatever color this parse (or caller) believes is
    // currently in the slot — typically firmware-truth from the parse path,
    // but set_slot_info() also pre-updates the baseline with the user's
    // chosen color before calling update_slot_from_state(), so this helper
    // can be fed a user-provided color too. Either way, the "did it change
    // from what we last saw?" contract is the same.
    //
    // `nullopt` is the explicit "no reading available" signal (empty
    // colors_[idx], unread slot, transient JSON parse race). Treat as
    // non-signal: don't update the baseline, don't sync. Otherwise every
    // empty-slot poll would overwrite a real prior color and mask a genuine
    // subsequent edit on the next non-empty poll.
    //
    // CRITICAL: a value of 0 (pure black, #000000) is a LEGITIMATE color, not
    // a no-reading sentinel — the prior `observed_color == 0` skip silently
    // dropped genuine black filament from external-edit detection (companion
    // bug to the FilamentSlotOverride color_set fix in commit f69d53037).
    if (!observed_color.has_value())
        return false;
    const uint32_t color = *observed_color;

    auto it = last_firmware_color_.find(slot_index);
    if (it == last_firmware_color_.end()) {
        // First observation for this slot — establish baseline. Even if the
        // override's color_rgb differs from firmware, the initial startup
        // observation is NEVER an external-edit signal. apply_overrides will
        // still run after us and the override wins.
        last_firmware_color_[slot_index] = color;
        spdlog::debug("{} Slot {} baseline color: #{:06X}", backend_log_tag(), slot_index, color);
        return false;
    }
    if (it->second == color)
        return false; // unchanged — no edit signal

    // Observed color changed versus the baseline.
    const uint32_t old_color = it->second;

    if (!slot_has_filament) {
        // Two sub-cases, both "not a real present-lane color":
        //   - Empty slot: reads back the #808080 placeholder — the absence of a
        //     reading, not an edit. Eject is handled by the parse path dropping
        //     presence; the lane->Spoolman override is RETAINED across empty
        //     (#1071), not cleared.
        //   - Presence-lag insert: on modern ZMOD the firmware color can surface
        //     one parse frame BEFORE IFS_STATUS Ports flips the slot present.
        // Do NOT advance the baseline here. Advancing consumed the delta while
        // the slot was still "absent", so when presence caught up the baseline
        // already equalled the new color and the sync was swallowed entirely —
        // the freshly inserted spool kept the previous color (#1065). Hold the
        // last real color so the delta survives until a present-lane frame.
        spdlog::debug("{} Slot {} color reading #{:06X} while slot not present — "
                      "baseline held at #{:06X}, sync deferred to presence (#1065)",
                      backend_log_tag(), slot_index, color, old_color);
        return false;
    }
    // Present-lane real reading: advance the baseline before syncing so a failed
    // save_async doesn't make us re-fire every poll.
    it->second = color;

    spdlog::info("{} Slot {} firmware color changed #{:06X} -> #{:06X}, "
                 "syncing override + Moonraker DB lane_data (external edit detected)",
                 backend_log_tag(), slot_index, old_color, color);

    // External edit (Mainsail console, AD5X LCD, native zmod dialog,
    // CHANGE_ZCOLOR from any non-Helix path). Refresh the override's
    // color_rgb + material — preserving brand/spool_name/spoolman_id —
    // and push the result to lane_data so OrcaSlicer's MoonrakerPrinterAgent
    // sees the new state. The caller's next apply_overrides() call lays the
    // refreshed override back over entry->info; since color_rgb + material
    // now match firmware-truth, that's a no-op for those fields.
    sync_override_to_firmware_locked(slot_index, color,
                                     materials_[static_cast<size_t>(slot_index)]);
    return true;
}

bool AmsBackendAd5xIfs::check_external_type_change(int slot_index,
                                                   const std::string& observed_material,
                                                   std::optional<uint32_t> observed_color,
                                                   bool slot_has_filament) {
    // Track a material baseline across empty lanes — mirroring
    // check_external_color_change, which keeps a live baseline over an empty
    // lane (empty color parses to the #808080 placeholder). The prior top-level
    // `if (observed_material.empty()) return` bailed BEFORE baselining, so a
    // lane that was empty at boot never recorded a baseline; the FIRST insert
    // then hit the "first observation" branch below instead of "changed",
    // silently swallowing the type delta — color updated on screen, material
    // stuck on the stale override (#981/#1065 empty->insert). Baselining a
    // genuinely-empty lane to "" makes a later "" -> PETG a real edit that syncs.
    //
    // But an empty observed_material is NOT always an empty lane. It is also the
    // "no reading yet" signal when the parse hasn't populated materials_[idx]
    // (pre-GET_ZCOLOR boot) or when a split update sets color before material.
    // Distinguish them the same way the color path does: an empty material is a
    // genuine empty-lane reading ONLY when the lane is actually empty
    // (slot_has_filament == false) AND a firmware reading exists (observed_color
    // has a value — on a real empty lane that's the #808080 placeholder). When
    // the lane is present-but-empty-material, or no color reading exists yet,
    // treat empty as "no reading": skip without baselining, so the first REAL
    // material observation becomes the baseline (not a bogus "" -> X change that
    // would fabricate an override on a slot HelixScreen never touched).
    if (observed_material.empty() && (slot_has_filament || !observed_color.has_value()))
        return false;

    auto it = last_firmware_material_.find(slot_index);
    if (it == last_firmware_material_.end()) {
        // First observation for this slot — establish baseline (may be ""),
        // never an edit signal.
        last_firmware_material_[slot_index] = observed_material;
        spdlog::debug("{} Slot {} baseline material: '{}'", backend_log_tag(), slot_index,
                      observed_material);
        return false;
    }
    if (it->second == observed_material)
        return false; // unchanged — no edit signal

    const std::string old_material = it->second;

    // Only a present slot reporting a real (non-empty) material is an external
    // type edit worth syncing. Two skip sub-cases, treated differently for the
    // baseline (this asymmetry is the #1065 insert-swallow fix):
    //   - EMPTY observation while the slot is empty == eject. Advance the
    //     baseline to "" so the subsequent insert is a genuine "" -> MATERIAL
    //     delta that syncs. (The top guard already returned for empty material
    //     on a PRESENT slot / no-color-reading, so here empty implies eject.)
    //   - NON-EMPTY material while the slot is not yet present == modern-ZMOD
    //     presence-lag insert: the firmware type surfaced one parse frame before
    //     IFS_STATUS Ports flipped the slot present. Do NOT advance the baseline
    //     — hold the old value so the delta survives until a present-lane frame,
    //     otherwise the sync is swallowed and the new type never reaches the
    //     override (color updated on screen, material stuck — #981/#1065).
    // The user-locked-material guard (#965) still lives inside
    // sync_override_to_firmware_locked's OverwriteAlways mirror, so a
    // deliberately locked material is preserved through this path too.
    if (!slot_has_filament || observed_material.empty()) {
        if (observed_material.empty())
            it->second = observed_material; // eject: baseline -> ""
        spdlog::debug("{} Slot {} firmware material '{}' -> '{}' "
                      "(slot empty / no material — {}, sync skipped)",
                      backend_log_tag(), slot_index, old_material, observed_material,
                      observed_material.empty() ? "baseline updated" : "baseline held (#1065)");
        return false;
    }
    it->second = observed_material; // present-lane real delta: advance + sync below

    // The mirror inside sync_override_to_firmware_locked refreshes BOTH color
    // and material from the passed values, so we must hand it the real firmware
    // color — never a phantom — or it could clobber the override's color. On
    // AD5X ffmColor and ffmType come from the same parse, so a present material
    // implies a present color; if the color reading is somehow absent, update
    // the baseline but defer the sync to the next parse when both are readable.
    if (!observed_color.has_value()) {
        spdlog::debug("{} Slot {} firmware material changed {} -> {} but no color reading yet — "
                      "sync deferred",
                      backend_log_tag(), slot_index, old_material, observed_material);
        return false;
    }

    spdlog::info("{} Slot {} firmware material changed {} -> {}, syncing override "
                 "+ Moonraker DB lane_data (external type edit detected)",
                 backend_log_tag(), slot_index, old_material, observed_material);

    // OverwriteAlways mirror skips user-locked material (#965), so a genuine
    // user choice is preserved; a stale auto-mirror material is refreshed so
    // the new firmware type surfaces on the next apply_overrides().
    sync_override_to_firmware_locked(slot_index, *observed_color, observed_material);
    return true;
}

bool AmsBackendAd5xIfs::sync_override_to_firmware_locked(int slot_index, uint32_t firmware_color,
                                                         const std::string& firmware_material) {
    // IFS callers (check_external_color_change) have already filtered for
    // empty-slot / no-signal cases, so this path always represents a real
    // observation. Pass slot_has_filament=true unconditionally; the helper's
    // own guards then enforce firmware_color != 0.
    //
    // OverwriteAlways policy on IFS: set_slot_info pushes user color back to
    // firmware via Adventurer5M.json, so in the steady state user-truth and
    // firmware-truth converge. The mirror bootstraps an empty override on
    // hardware swap and catches genuine external edits (Mainsail console,
    // native LCD, CHANGE_ZCOLOR).
    //
    // User-lock guard (#965): set_slot_info(persist=true) tags the override
    // user_locked_color / user_locked_material; the helper skips those
    // fields. Without the guard, an AD5X firmware post-print FFMInfo revert
    // (re-emits prior material into Adventurer5M.json) was clobbering the
    // user's material choice through this call site. To re-enable auto-track
    // on a previously-edited slot the user calls clear_slot_override.
    bool changed = helix::ams::mirror_firmware_to_lane_data(
        override_store_.get(), overrides_, slot_index, firmware_color, firmware_material,
        /*slot_has_filament=*/true, helix::ams::MirrorPolicy::OverwriteAlways, backend_log_tag());
    if (!changed)
        return false;

    // parse_adventurer_json reads external_sync_count_ before/after its loop
    // to decide whether to also push an _IFS_VARS mirror to the
    // lessWaste/bambufy plugin's save_variables — see the comment block there
    // for why that's required (the plugins don't self-sync).
    ++external_sync_count_;
    return true;
}

void AmsBackendAd5xIfs::clear_override_locked(int slot_index, SlotInfo& slot) {
    // Caller must hold mutex_. Erases the in-memory override, resets
    // override-exclusive fields on the live SlotInfo (so the next
    // get_slot_info sees cleared state — apply_overrides is a no-op for this
    // slot after erase), and fires the async store delete. Firmware-sourced
    // fields (color_rgb, material, mapped_tool, status) are left alone —
    // update_slot_from_state has already refreshed them.
    overrides_.erase(slot_index);

    slot.brand.clear();
    slot.spool_name.clear();
    slot.spoolman_id = 0;
    slot.spoolman_vendor_id = 0;
    slot.remaining_weight_g = -1.0f;
    slot.total_weight_g = -1.0f;
    slot.color_name.clear();
    // The catalog pick is override-exclusive on every backend — no AMS
    // firmware carries a branded product id — so a clear always drops it.
    // Leaving it would re-navigate the editor to the removed spool's
    // product on the next open.
    slot.catalog_id.clear();
    slot.product_name.clear();

    if (override_store_) {
        // Capture by value only — clear_async's Moonraker callback can fire
        // long after this function returns (MR tracker ~60s timeout) and
        // after the backend itself may be gone. Same pattern as the
        // save_async site in set_slot_info().
        const std::string tag = backend_log_tag();
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
            }
        });
    }
}

void AmsBackendAd5xIfs::release_locked_override_keep_identity_locked(int slot_index,
                                                                     SlotInfo& slot) {
    // Caller must hold mutex_. See the header for the full rationale. Short
    // version: an external CHANGE_ZCOLOR legitimately re-authors color/material
    // (firmware truth wins), but the firmware can't carry brand/spool_name/
    // spoolman_id/weights — those are the user's identity metadata and must
    // survive a routine physical load (Bug B / #981). Release the locks + strip
    // the firmware-carryable fields so apply_overrides stops masking firmware
    // truth for color/material, but keep the identity fields. When there is no
    // identity to preserve, fall back to a full erase.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end())
        return;
    auto& ovr = it->second;

    // Firmware-uncarryable identity/metadata worth preserving across the edit.
    // Weights are consumption-tracker / Spoolman data, independent of color, so
    // they ride along too. (>= 0.0f — the -1.0f default is "unknown".)
    const bool has_identity = !ovr.brand.empty() || !ovr.spool_name.empty() ||
                              ovr.spoolman_id > 0 || ovr.spoolman_vendor_id > 0 ||
                              ovr.remaining_weight_g >= 0.0f || ovr.total_weight_g >= 0.0f;

    if (!has_identity) {
        // Nothing firmware-uncarryable to keep — behave exactly like the
        // pre-existing #981 clear so those tests still see a clean wipe.
        clear_override_locked(slot_index, slot);
        return;
    }

    spdlog::info("{} External CHANGE_ZCOLOR for slot {} — releasing the color/material locks so "
                 "firmware truth wins, but RETAINING the user's brand/spool metadata the firmware "
                 "can't carry (#1071-style retention, Bug B)",
                 backend_log_tag(), slot_index);

    // Release the user-locks and strip the firmware-carryable override fields.
    // apply_overrides only masks a field when the override still carries a real
    // value (non-empty string / color_set / >0 id), so clearing these lets the
    // firmware-truth color_rgb/material (refreshed by update_slot_from_state)
    // show through. The identity fields (brand, spool_name, spoolman_id,
    // spoolman_vendor_id, weights) stay put.
    ovr.user_locked_color = false;
    ovr.user_locked_material = false;
    ovr.color_set = false;
    ovr.color_rgb = 0;
    ovr.color_name.clear();
    ovr.material.clear();
    // The catalog pick is scoped to a MATERIAL — "sunlu-pla-plus-2-0" only makes
    // sense while the lane is PLA. This path exists because firmware just
    // re-authored the material, so the pick goes with it. Keeping it would be
    // worse than losing it: setup_details_selector() seeds the type dropdown
    // from catalog_id first, so a stale id would drag the editor back to the old
    // material family and contradict the firmware truth we just accepted.
    // Deliberately NOT counted in has_identity above for the same reason — it is
    // not firmware-uncarryable metadata like brand/spool_name, it is material-
    // derived.
    ovr.catalog_id.clear();
    ovr.product_name.clear();

    // Persist the trimmed override so a restart reloads the retained identity
    // (and the released locks) instead of the pre-edit locked record. Capture
    // by value — the callback can fire long after this returns.
    if (override_store_) {
        helix::ams::FilamentSlotOverride snapshot = ovr;
        const std::string tag = backend_log_tag();
        override_store_->save_async(slot_index, snapshot,
                                    [tag, slot_index](bool ok, std::string err) {
                                        if (!ok) {
                                            spdlog::warn("{} identity-retain persist failed for "
                                                         "slot {}: {}",
                                                         tag, slot_index, err);
                                        }
                                    });
    }
}

void AmsBackendAd5xIfs::unlock_auto_tracked_override_on_insert_locked(int slot_index) {
    // Caller holds mutex_. See the header doc + FILAMENT_MANAGEMENT.md for the
    // full model. Short version: a lane's material/color override can be
    // user-locked either by a menu edit (set_slot_info) or by the pessimistic
    // !material.empty() load default (from_lane_data_record). A locked field is
    // never refreshed by the OverwriteAlways auto-mirror, so a freshly inserted
    // spool keeps painting the PREVIOUS spool's type/color. Only an external
    // CHANGE_ZCOLOR clears that lock (#981), and a physical insert emits none —
    // so unlock here, on the insert edge itself.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end())
        return; // auto-tracking already (no override) — nothing to unlock
    auto& ovr = it->second;
    // A real Spoolman binding is a deliberate identity the user attached; #1071
    // retains it across an eject/insert cycle (same-spool maintenance
    // re-insert). Leave a bound lane fully alone — its material/color came from
    // the bound spool, not a stale guess.
    if (ovr.spoolman_id > 0)
        return;
    if (!ovr.user_locked_material && !ovr.user_locked_color)
        return; // already auto-tracking both fields
    spdlog::info("{} Slot {} inserted (empty->present) with no Spoolman link — "
                 "unlocking auto-tracked material/color so the new spool's firmware "
                 "type/color refresh (#1065)",
                 backend_log_tag(), slot_index);
    ovr.user_locked_material = false;
    ovr.user_locked_color = false;
    // Persist the unlock so a restart doesn't reload the pessimistic
    // !material.empty() lock default and re-stick the old type. The subsequent
    // update_slot_from_state -> auto-mirror will save again once firmware truth
    // refreshes the material/color; this first save just makes the unlock
    // durable even if the same spool goes back in and no material delta follows.
    if (override_store_) {
        helix::ams::FilamentSlotOverride snapshot = ovr;
        const std::string tag = backend_log_tag();
        override_store_->save_async(
            slot_index, snapshot, [tag, slot_index](bool success, const std::string& err) {
                if (!success) {
                    spdlog::warn("{} unlock persist failed for slot {}: {}", tag, slot_index, err);
                }
            });
    }
}

void AmsBackendAd5xIfs::clear_slot_override(int slot_index) {
    if (slot_index < 0 || slot_index >= NUM_PORTS) {
        spdlog::warn("{} clear_slot_override: invalid slot {}", backend_log_tag(), slot_index);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            spdlog::warn("{} clear_slot_override: no slot entry for index {}", backend_log_tag(),
                         slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, entry->info);
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

// --- State queries ---

void AmsBackendAd5xIfs::publish_external_spool_lane(const SlotInfo* spool) {
    // ZMOD never writes lane_data — our mirror store owns it — so no separate
    // publish store is needed (unlike AFC/HH, whose plugins own the namespace).
    // IFS is fixed at NUM_PORTS bays; the extern lane rides one past.
    if (!override_store_ || !system_info_.supports_bypass) {
        return;
    }
    helix::ams::publish_external_lane(override_store_.get(), NUM_PORTS, spool, backend_log_tag());
}

AmsSystemInfo AmsBackendAd5xIfs::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for stuck operations on every UI poll, not just status updates.
    // This catches cases where the printer goes silent (network drop, Klipper crash).
    const_cast<AmsBackendAd5xIfs*>(this)->check_action_timeout();

    auto info = slots_.build_system_info();

    // Overlay our cached system info
    info.type = system_info_.type;
    info.type_name = system_info_.type_name;
    info.total_slots = system_info_.total_slots;
    info.current_tool = system_info_.current_tool;
    info.current_slot = system_info_.current_slot;
    info.filament_loaded = system_info_.filament_loaded;
    info.action = system_info_.action;
    // Surface the phase machine's live sub-phase + detail so
    // AmsState::sync_from_backend can drive the ams_operation_phase subject and
    // the operation-detail line. Without this the right-side step tracker renders
    // the steps but never highlights the active one, and the detail goes blank
    // (#1065 Bug 2: "the 1-2-3 steps show but fail to launch any of them").
    info.operation_detail = system_info_.operation_detail;
    info.operation_phase = system_info_.operation_phase;
    info.operation_indeterminate = system_info_.operation_indeterminate;
    info.supports_bypass = system_info_.supports_bypass;
    info.supports_tool_mapping = system_info_.supports_tool_mapping;
    info.endless_spool_enabled = system_info_.endless_spool_enabled;
    info.supports_purge = system_info_.supports_purge;

    // Replace registry's tool map with IFS-specific 16-entry mapping
    info.tool_to_slot_map.clear();
    for (int t = 0; t < TOOL_MAP_SIZE; ++t) {
        int port = tool_map_[static_cast<size_t>(t)];
        if (port >= 1 && port <= NUM_PORTS) {
            info.tool_to_slot_map.push_back(port - 1);
        } else {
            info.tool_to_slot_map.push_back(-1);
        }
    }

    return info;
}

SlotInfo AmsBackendAd5xIfs::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* entry = slots_.get(slot_index);
    if (!entry) {
        return SlotInfo{};
    }
    return entry->info;
}

bool AmsBackendAd5xIfs::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return external_mode_;
}

bool AmsBackendAd5xIfs::can_unload_from_toolhead(int slot_index) const {
    // The slot the firmware reports as active is always unloadable: a runout
    // clears the head sensor and drops the display status below LOADED, but the
    // filament is still seated in the IFS and must be retractable to recover.
    // Read current_slot (and head_filament_) under the lock, then defer to the
    // base LOADED check without holding mutex_ (get_slot_info re-acquires the
    // non-recursive lock).
    //
    // Two locked conditions open the gate:
    //   1. The firmware reports this slot as the active/current slot.
    //   2. The toolhead sensor reports filament present (head_filament_) AND the
    //      firmware has no active-slot pointer (current_slot < 0). The stock-ZMOD
    //      firmware can drop its pointer to -1 (emitting "Extruder: None" after a
    //      runout or print-end) while filament is still seated in the toolhead.
    //      Condition 1 then never matches, so without this the physically-present
    //      filament would be unremovable. Since the true origin lane is unknown,
    //      every valid slot is reported unloadable; unload_filament() then sends
    //      the current-channel toolhead unload (IFS_REMOVE_CURRENT_PRUTOK), which
    //      the firmware resolves from FFMInfo.channel regardless of slot index.
    //      Gating on current_slot < 0 leaves the normal known-active-slot case
    //      untouched: when the firmware knows the active slot, only that slot is
    //      unloadable (condition 1), exactly as before.
    //
    // The slot_index >= 0 guard is load-bearing: when no filament is loaded
    // current_slot is -1, so a caller passing -1 would otherwise match it and
    // wrongly report the (nonexistent) active slot as unloadable. Note the
    // opposite negative-index convention here vs. unload_filament(), where
    // slot_index < 0 deliberately means "unload whatever is active." This is a
    // per-slot capability query, so a negative index is simply out of range.
    //
    // Deliberately still the conflated head_filament_, NOT the switch pair the
    // unload router uses. This gate only decides whether the Unload affordance
    // is OFFERED; slot_unloads_to_toolhead() / do_unload_filament() then decide
    // heated-vs-cold, so a false "loaded" here cannot grind anything - it just
    // reaches a router that routes it correctly. The error that would hurt is a
    // false "empty", which hides the #995 recovery affordance while filament is
    // physically seated, and head_filament_ is the more permissive of the two
    // readings in exactly the case #995 is about (firmware dropped its pointer).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index >= 0 && (system_info_.current_slot == slot_index ||
                                (head_filament_ && system_info_.current_slot < 0))) {
            return true;
        }
    }
    return AmsBackend::can_unload_from_toolhead(slot_index);
}

// --- Path visualization ---

PathSegment AmsBackendAd5xIfs::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (head_filament_) {
        return PathSegment::NOZZLE;
    }
    // Check if active tool's port has filament
    if (active_tool_ >= 0 && active_tool_ < TOOL_MAP_SIZE) {
        int port = tool_map_[static_cast<size_t>(active_tool_)];
        if (port >= 1 && port <= NUM_PORTS && port_presence_[static_cast<size_t>(port - 1)]) {
            return PathSegment::LANE;
        }
    }
    return PathSegment::NONE;
}

PathSegment AmsBackendAd5xIfs::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_PORTS) {
        return PathSegment::NONE;
    }

    auto idx = static_cast<size_t>(slot_index);
    if (!port_presence_[idx]) {
        return PathSegment::NONE;
    }

    bool is_active = (system_info_.current_slot == slot_index);
    if (is_active && head_filament_) {
        return PathSegment::NOZZLE;
    }

    // Active slot in transit — filament is in the lane between gate and head
    if (is_active) {
        return PathSegment::LANE;
    }
    // Non-active slots with filament detected at gate — show at hub
    return PathSegment::HUB;
}

PathSegment AmsBackendAd5xIfs::infer_error_segment() const {
    // IFS doesn't report fine-grained error segments
    return PathSegment::NONE;
}

// --- Filament operations ---

AmsError AmsBackendAd5xIfs::do_load_filament(int slot_index) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }

    int port = slot_index + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        note_filament_op_dispatch_locked();
        system_info_.action = AmsAction::HEATING;
        action_start_time_ = std::chrono::steady_clock::now();
        begin_phase_tracking_locked(/*is_unload=*/false);
        // Swap detection (#1065 v0.99.94, bundle NJB2U558): if another lane is
        // currently seated, INSERT_PRUTOK_IFS will run an implicit UNLOAD of
        // it before loading the new lane. The default 90s LOADING budget
        // would fire mid-swap; flip swap_expected so check_action_timeout
        // applies SWAP_LOADING_TIMEOUT_SECONDS, and on_head_transition_locked
        // resets the LOADING clock when the implicit-unload head drop fires.
        // seated_chan_ is the live seated-port authority (1-based; 0 = none).
        if (seated_chan_ > 0 && seated_chan_ != port) {
            phase_tracker_.swap_expected = true;
            spdlog::info("{} Load slot {} while slot {} seated — swap_expected "
                         "(extended LOADING budget + head-drop clock reset)",
                         backend_log_tag(), slot_index, seated_chan_ - 1);
        }
        apply_phase_action_locked();
    }
    // Publish the busy state immediately (lock released) so the sidebar action
    // buttons hide and the context menu disables before the user can re-tap —
    // the action moved IDLE→HEATING above, so a STATE_CHANGED is always due.
    emit_event(EVENT_STATE_CHANGED);
    spdlog::info("{} Loading filament from port {}", backend_log_tag(), port);
    // Finalize on the macro's own completion (its gcode ack). INSERT_PRUTOK_IFS
    // is a linear, synchronous zmod macro (home → heat → feed → purge → unclamp);
    // it acks only after the purge fully runs. Like the unload, the synthesized
    // Purge phase has no sensor event and the confirming query can silently fail
    // on native ZMOD — without this the load sticks at Purge until the 90s
    // timeout flips to ERROR (raza616 stuck-on-Purging). on_complete fires on a
    // bg thread, so hop to the main thread before touching state.
    //
    // ensure_homed_then() WITHOUT skip_homing is deliberate (#1248 proposed
    // skip_homing=true on the theory that this double-homes; it does not). The
    // macro's leading _G28 is conditional on homed_axes, so once our G28 has
    // run it falls through - one home either way. What ensure_homed_then() buys
    // over letting the macro home itself is the "Home printer first?" prompt:
    // on a loadcell-Z AD5X the load is about to run a full probing home plus a
    // trash-drop and nozzle wipe, and the user gets told before the toolhead
    // moves. Unlike the unload above, which the user reaches only from an
    // already-loaded head, Load is the entry point from a cold idle printer.
    auto token = lifetime_.token();
    return ensure_homed_then("INSERT_PRUTOK_IFS PRUTOK=" + std::to_string(port), [this, token]() {
        token.defer("Ad5xIfsBackend::load_macro_complete",
                    [this]() { finalize_op_after_macro(/*is_unload=*/false); });
    });
}

AmsError AmsBackendAd5xIfs::do_unload_filament(int slot_index) {
    bool head_empty;
    int current_slot;
    int seated_slot; // 0-based slot of the IFS_STATUS-seated port (-1 = none)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Stamp here, at the top, and not in the heated-unload branch below:
        // three of the four exits from this function route to eject_lane()
        // without ever touching phase_tracker_ or system_info_.action, so the
        // backend stays IDLE through an operation that legitimately empties the
        // toolhead. The stamp is what stops the runout detector reading that as
        // an unattended runout (#1250).
        note_filament_op_dispatch_locked();
        // Snapshot the routing decision under the lock, then act unlocked (the
        // eject/gcode paths re-acquire mutex_). NOT head_filament_ on its own:
        // the motion sensor also writes it and false-negatives on a loaded-idle
        // lane, and reading that as "empty" cold-ejects seated filament.
        head_empty = head_empty_for_unload_routing_locked();
        current_slot = system_info_.current_slot;
        seated_slot = seated_chan_ > 0 ? seated_chan_ - 1 : -1;
    }

    // A specific slot that is NOT the firmware's active slot is never seated at
    // the toolhead — its filament sits in the lane, not the nozzle. Route it to a
    // cold per-lane eject (IFS_F11 on that port) instead of the heated
    // _IFS_REMOVE_CURRENT_PRUTOK, which resolves the channel from FFMInfo.channel
    // and backs out whatever is actually at the nozzle regardless of slot index.
    // Without this, "unload channel 1" while channel 3 is loaded heats the nozzle
    // and backs out channel 3, never touching channel 1, then stalls (raza616,
    // bundle HKHZFYB2). Fall through to the toolhead unload only when the slot IS
    // the active one (slot_index == current_slot), the caller asked to unload
    // whatever is active (slot_index < 0), or the firmware has lost its active
    // pointer while the head is loaded (current_slot < 0) — the unknown-origin
    // recovery case where removing the head is the intended action (see
    // can_unload_from_toolhead()).
    //
    // current_slot is derived through tool_map_/active_tool_; on the plugin path
    // it is owned by save_variables and can point at the wrong slot loaded-idle.
    // IFS_STATUS "Chan" is the physically seated port and is trusted directly: if
    // the tapped slot IS the seated channel, it must take the heated toolhead
    // unload even when current_slot disagrees — otherwise we cold-eject a seated,
    // un-cut filament and grind it (raza616 #981, v0.99.80).
    if (slot_index >= 0 && current_slot >= 0 && slot_index != current_slot &&
        slot_index != seated_slot) {
        spdlog::info("{} Unload requested for non-active slot {} (active slot {}, seated slot {}) "
                     "-> cold lane eject",
                     backend_log_tag(), slot_index, current_slot, seated_slot);
        return eject_lane(slot_index);
    }

    // Firmware dropped its active pointer (current_slot < 0) but IFS_STATUS still
    // reports a physically seated port. Trust Chan as the seated authority: a tap
    // on a NON-seated slot must cold-eject that lane, not toolhead-cut the seated
    // one (raza616 wrong-lane heat+cut, bundle 5HR3HHS6). A tap ON the seated slot
    // falls through to the heated toolhead unload below. When neither authority
    // knows the seated slot (both < 0) we keep the existing unknown-origin recovery
    // (toolhead cut) untouched.
    if (slot_index >= 0 && current_slot < 0 && seated_slot >= 0 && slot_index != seated_slot) {
        spdlog::info("{} Unload requested for non-seated slot {} (no active slot, seated slot {}) "
                     "-> cold lane eject",
                     backend_log_tag(), slot_index, seated_slot);
        return eject_lane(slot_index);
    }

    // No filament seated at the nozzle: the toolhead unload would be a firmware
    // no-op. IFS_REMOVE_CURRENT_PRUTOK — and the _IFS_REMOVE_CURRENT_PRUTOK macro
    // that wraps it — early-returns when get_extruder_sensor() reads empty
    // (zmod_ifs.py:1149), so ZMOD homes (_G28) and then does nothing: raza616's
    // "homes and nothing happens." The filament is still in the lane, not the
    // toolhead, so route to the cold per-lane retract instead of a guaranteed
    // no-op (7AC4SDEX: head_switch_sensor empty, ifs_motion_sensor present — the
    // switch pair still calls that empty, so 7AC4SDEX keeps this branch).
    if (head_empty) {
        // "Unload whatever is active" (slot_index < 0) needs a concrete lane for
        // the cold eject — resolve through the seated channel, then the active
        // slot. Passing -1 through to eject_lane() fails validate_slot_index()
        // and the swap path discarded that error, freezing the sidebar in
        // "Heating" (Vger1700, bundle Z5V4K3NL).
        int eject_slot = slot_index >= 0    ? slot_index
                         : seated_slot >= 0 ? seated_slot
                                            : current_slot;
        if (eject_slot < 0) {
            spdlog::info("{} Unload with empty toolhead sensor and no seated/active lane -> "
                         "nothing to unload",
                         backend_log_tag());
            return AmsError(AmsResult::WRONG_STATE,
                            "unload_filament: head sensor empty, no seated or active lane",
                            "Nothing to unload: no filament at the nozzle");
        }
        spdlog::info("{} Unload with empty toolhead sensor -> cold lane eject (slot {})",
                     backend_log_tag(), eject_slot);
        return eject_lane(eject_slot);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::HEATING;
        action_start_time_ = std::chrono::steady_clock::now();
        begin_phase_tracking_locked(/*is_unload=*/true);
        apply_phase_action_locked();
    }
    // Publish the busy state immediately (lock released) so the sidebar action
    // buttons hide and the context menu disables before the user can re-tap.
    // Without this, the busy action wasn't surfaced until the next ~1.4s status
    // frame, leaving a window where a second tap hit the busy precondition and
    // produced the confusing "Unload failed: AMS is busy" toast (7L44W2B7). The
    // action moved IDLE→HEATING above, so a STATE_CHANGED is always due.
    emit_event(EVENT_STATE_CHANGED);

    // Dispatch ZMOD's own toolhead-unload macro rather than reconstructing it.
    // _IFS_REMOVE_CURRENT_PRUTOK is the firmware's "Remove from extruder" button
    // (observed working on raza616's device, bundle 7AC4SDEX): it self-homes
    // (_G28), calls IFS_REMOVE_CURRENT_PRUTOK with NEED_TRASH=1
    // BYPASS_TEMPERATURE_CHECK=1, then resets the hotend to 0 and refreshes
    // color. Send it raw via execute_gcode(), never the bare Python command -
    // that skips the trash drop and leaves the nozzle hot. Verified against the
    // device cfg and ZMOD v1.7.1.
    //
    // Raw rather than ensure_homed_then() because the macro's own _G28 already
    // covers the unhomed case, so our G28 would add nothing but a "Home printer
    // first?" prompt in front of a home the user cannot decline anyway. It is
    // NOT to avoid a double home: _G28 is conditional on homed_axes and no-ops
    // once we have homed (see filament_ops_self_home() in the header). The load
    // path below deliberately makes the opposite call - see do_load_filament().
    spdlog::info("{} Unloading filament from toolhead (slot {}, current_slot {}, seated_slot {}, "
                 "head_empty {})",
                 backend_log_tag(), slot_index, current_slot, seated_slot, head_empty);
    // Finalize on the macro's own completion (its gcode ack). The synthesized
    // Retract phase has no sensor event, and the IFS_STATUS Chan==0 / GET_ZCOLOR
    // confirm can silently fail on native ZMOD — leaving the op stuck at Retract
    // until the 90s timeout flips it to ERROR (raza616 stuck-on-Retract). The
    // macro ack is the reliable "unload fully ran" signal. on_complete fires on a
    // bg thread, so hop to the main thread before touching state.
    auto token = lifetime_.token();
    auto result = execute_gcode("_IFS_REMOVE_CURRENT_PRUTOK", [this, token]() {
        token.defer("Ad5xIfsBackend::unload_macro_complete",
                    [this]() { finalize_op_after_macro(/*is_unload=*/true); });
    });
    // Backup re-query: for inactive-slot unloads on native ZMOD the head
    // sensor never changes, so detect_load_unload_completion() won't fire.
    // schedule_zcolor_query() coalesces with any trigger from the gcode
    // stream listener, so this is cheap when they overlap.
    schedule_zcolor_query("toolhead_unload");
    return result;
}

bool AmsBackendAd5xIfs::slot_unloads_to_toolhead(int slot_index, bool /*loaded_hint*/) const {
    int current_slot;
    int seated_slot;
    bool head_empty;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_slot = system_info_.current_slot;
        seated_slot = seated_chan_ > 0 ? seated_chan_ - 1 : -1;
        head_empty = head_empty_for_unload_routing_locked();
    }
    // MUST mirror unload_filament()'s eject-vs-toolhead routing so the context
    // menu labels and dispatches the action correctly (Eject vs Unload). Any
    // divergence is caught by the "label matches routing" unit test. The
    // recovery-broadened loaded_hint (can_unload_from_toolhead returns true for
    // every slot when current_slot < 0 && head loaded) is intentionally ignored
    // here — the seated channel is the authority.
    if (slot_index >= 0 && current_slot >= 0 && slot_index != current_slot &&
        slot_index != seated_slot)
        return false; // cold lane eject (existing #981 guard)
    if (slot_index >= 0 && current_slot < 0 && seated_slot >= 0 && slot_index != seated_slot)
        return false; // firmware dropped pointer, seated known -> cold eject (5HR3HHS6)
    if (head_empty)
        return false; // empty toolhead -> cold lane eject
    return true;      // heated toolhead unload (cut)
}

void AmsBackendAd5xIfs::finalize_op_after_macro(bool is_unload) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only finalize the op WE started and that is still in flight. If a
        // confirming IFS_STATUS / GET_ZCOLOR query already finalized it, or the
        // user moved on, phase_tracker_ is no longer our op — do nothing.
        if (phase_tracker_.active && phase_tracker_.is_unload == is_unload) {
            spdlog::info("{} {} macro complete (gcode ack) -> IDLE", backend_log_tag(),
                         is_unload ? "Unload" : "Load");
            system_info_.action = AmsAction::IDLE;
            if (is_unload) {
                clear_head_loaded_after_unload_locked();
                // Unlike the Chan/GET_ZCOLOR finalize paths (which refresh slots
                // in their own loops), this gcode-ack path otherwise wouldn't —
                // so the just-cleared head wouldn't reach the cached slot status.
                for (int i = 0; i < NUM_PORTS; ++i) {
                    update_slot_from_state(i);
                }
            }
            end_phase_tracking_locked();
            set_operation_detail_locked("");
            changed = true;
        }
    }
    if (changed) {
        PostOpCooldownManager::instance().schedule();
        // Reconcile presence/colours now that the lane state changed (drives the
        // present<->absent override-clear via GET_ZCOLOR / IFS_STATUS Ports).
        schedule_zcolor_query(is_unload ? "unload_macro_complete" : "load_macro_complete");
        emit_event(EVENT_STATE_CHANGED);
    }
}

AmsError AmsBackendAd5xIfs::do_select_slot(int slot_index) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }

    int port = slot_index + 1;
    spdlog::info("{} Selecting port {}", backend_log_tag(), port);
    return execute_gcode("SET_EXTRUDER_SLOT SLOT=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::do_change_tool(int tool_number) {
    if (tool_number < 0 || tool_number >= TOOL_MAP_SIZE) {
        return AmsErrorHelper::invalid_slot(tool_number, TOOL_MAP_SIZE - 1);
    }

    int port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        port = tool_map_[static_cast<size_t>(tool_number)];
    }

    if (port < 1 || port > NUM_PORTS) {
        return AmsErrorHelper::invalid_parameter("Tool T" + std::to_string(tool_number) +
                                                 " is not mapped to any port");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        note_filament_op_dispatch_locked();
        system_info_.action = AmsAction::LOADING;
        action_start_time_ = std::chrono::steady_clock::now();
    }
    // Publish LOADING immediately, matching do_load_filament/do_unload_filament.
    // A tool change is a full swap - it drops the head sensor partway through -
    // and until this emit existed nothing told the UI (or the runout detector's
    // "is an operation in flight" gate) that the busy state had begun until the
    // next ~1.4 s status frame.
    //
    // A_CHANGE_FILAMENT deliberately does NOT arm the phase tracker. The tracker
    // is finalized either by a macro-completion callback (load/unload both
    // install one) or by the ERROR timeout, and this call site has neither;
    // arming it here would make detect_load_unload_completion() early-return on
    // the head rise that currently snaps the action back to IDLE, so a tool
    // change would hang busy until the 90 s budget flipped it to ERROR. The
    // LOADING action above is what the runout gate keys on instead.
    emit_event(EVENT_STATE_CHANGED);
    spdlog::info("{} Changing to tool T{} (port {})", backend_log_tag(), tool_number, port);
    // ensure_homed_then() without skip_homing, and NOT because A_CHANGE_FILAMENT
    // is known to need it. Unlike INSERT_PRUTOK_IFS / _IFS_REMOVE_CURRENT_PRUTOK,
    // this macro is not in the ZMOD tree at all: ZMOD ships CHANGE_FILAMENT and
    // _A_CHANGE_FILAMENT as RESPOND-only stubs and drives its own swaps through
    // INSERT_PRUTOK_IFS (zmod_color.py). A_CHANGE_FILAMENT comes from the stock
    // FlashForge config, which we have no copy of, so whether it self-homes is
    // unverified in either direction. Homing first is the safe side of that
    // unknown: an extra conditional _G28 costs nothing, while skipping ours in
    // front of a macro that does NOT self-home hands Klipper toolhead moves on
    // unhomed axes. Do not "fix" this to skip_homing=true without the cfg.
    return ensure_homed_then("A_CHANGE_FILAMENT CHANNEL=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::eject_lane(int slot_index) {
    int len = filament_eject_default_.first;
    int speed = filament_eject_default_.second;
    std::string material;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        // A cold lane eject leaves system_info_.action IDLE, so it is invisible
        // to every "is something in flight" test the runout detector could make.
        // The dispatch stamp is that test (#1250).
        note_filament_op_dispatch_locked();

        if (!validate_slot_index(slot_index)) {
            return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
        }

        // Refuse to cold-eject the lane currently seated at the toolhead: the
        // backward retract would fight the loaded filament. Mirror AFC's wording
        // so the UI surfaces a consistent message across backends.
        //
        // Shares head_empty_for_unload_routing_locked() with unload_filament()'s
        // router by necessity, not tidiness: the router sends an empty-head
        // unload HERE, so a refusal keyed on a different notion of "empty" could
        // reject the very call it just routed, and tell the user to "unload from
        // the toolhead first" for an unload that is already trying. Sharing the
        // predicate also closes the direct-Eject-tap half of the grinding hazard:
        // a motion-sensor false negative used to let head_filament_ read false on
        // a seated lane and wave the cold retract straight through.
        if (system_info_.current_slot == slot_index && !head_empty_for_unload_routing_locked()) {
            return AmsError(AmsResult::WRONG_STATE, "Lane is loaded in toolhead",
                            "Unload from toolhead first", "Use Unload before Eject");
        }

        // Resolve the cold-retract LEN/SPEED from filament.json keyed by the
        // lane's material, falling back to the file's "default" entry (already
        // captured in filament_eject_default_), then to the hardcoded 1000/1200
        // the default pair is initialized to. An empty material or an
        // un-fetched/404 filament.json simply leaves the defaults in place.
        material = materials_[slot_index];
        if (!material.empty()) {
            auto it = filament_eject_params_.find(material);
            if (it != filament_eject_params_.end()) {
                len = it->second.first;
                speed = it->second.second;
            }
        }
    }

    int port = slot_index + 1;
    spdlog::info("{} Cold eject lane {} (port {}, material '{}') -> IFS_F24 / IFS_F11 LEN={} "
                 "SPEED={} / IFS_F39",
                 backend_log_tag(), slot_index, port, material, len, speed);

    // Full per-lane eject mirroring zmod's _REMOVE_PRUTOK_IFS macro: clamp the
    // lane (F24) so the gear actually grips, cold-retract the full tube length
    // at the configured speed (F11 — no heat, no homing), then unclamp (F39) so
    // the filament is free to pull out by hand. Three separate execute_gcode()
    // calls in order, propagating the first error. Use the plain
    // execute_gcode() helper (NOT ensure_homed_then(), which is for toolhead
    // moves like load/unload).
    const std::string port_str = std::to_string(port);
    AmsError err = execute_gcode("IFS_F24 PRUTOK=" + port_str);
    if (!err.success()) {
        return err;
    }
    err = execute_gcode("IFS_F11 PRUTOK=" + port_str + " LEN=" + std::to_string(len) +
                        " SPEED=" + std::to_string(speed));
    if (!err.success()) {
        return err;
    }
    AmsError err39 = execute_gcode("IFS_F39 PRUTOK=" + port_str);
    if (err39.success()) {
        // Optimistically reflect the eject locally so the menu updates at once,
        // even if the confirming GET_ZCOLOR/IFS_STATUS poll starves behind the
        // blocking eject gcode on the constrained AD5X (#1065: the ejected lane
        // kept showing loaded and still offered Eject). IFS_F11 cold-retracts the
        // filament clear of the port silk sensor, so the lane is empty; the
        // scheduled poll re-confirms it when it lands. The lane can't be the
        // seated one (refused above), so clearing its presence never disturbs the
        // seated channel. The Spoolman override is retained (#1071) — only
        // presence drops, mirroring the IFS_STATUS Ports present->absent path.
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Belt-and-suspenders (#1065 row 28): the firmware doesn't blank
            // FFMInfo.channel / IFS_STATUS Chan on eject, so if a stale seated
            // pointer targets the just-ejected lane, kill the Unload affordance
            // now instead of waiting for the next head-gated poll.
            if (clear_seated_if_ejected_locked(slot_index)) {
                changed = true;
            }
            if (port_presence_[slot_index]) {
                port_presence_[slot_index] = false;
                changed = true;
            }
            // Stamp the eject so the follow-up query's stale silk-sensor read
            // (the lane still settling clear of the port) can't resurrect it
            // before the window elapses (#1065).
            last_eject_time_[static_cast<size_t>(slot_index)] = std::chrono::steady_clock::now();
            if (changed) {
                update_slot_from_state(slot_index);
            }
        }
        if (changed) {
            emit_event(EVENT_STATE_CHANGED);
        }
        schedule_zcolor_query("eject_lane");
    }
    return err39;
}

// --- Recovery ---

AmsError AmsBackendAd5xIfs::recover() {
    // Clear any latched ERROR state first — recovery is the explicit user
    // acknowledgement that the fault is dismissed, regardless of running_ state.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (system_info_.action == AmsAction::ERROR) {
            spdlog::info("{} Recovery: clearing ERROR state", backend_log_tag());
            system_info_.action = AmsAction::IDLE;
            system_info_.operation_detail.clear();
        }
        clear_runout_locked("recover()");
    }

    auto err = check_preconditions();
    if (!err.success())
        return err;

    // IFS_UNLOCK resets the IFS driver state machine — safest recovery command
    spdlog::info("{} Recovery: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::reset() {
    // Clear any latched ERROR state first — reset is an explicit user recovery
    // acknowledgement, mirrors the recover() clearing policy.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (system_info_.action == AmsAction::ERROR) {
            spdlog::info("{} Reset: clearing ERROR state", backend_log_tag());
            system_info_.action = AmsAction::IDLE;
            system_info_.operation_detail.clear();
        }
        clear_runout_locked("reset()");
    }

    auto err = check_preconditions();
    if (!err.success())
        return err;

    // IFS_UNLOCK resets the IFS driver — F15 (hard reset) is not exposed as a safe macro
    spdlog::info("{} Reset: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::cancel() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    // IFS_UNLOCK to abort current operation
    spdlog::info("{} Cancel: IFS_UNLOCK", backend_log_tag());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::IDLE;
        clear_runout_locked("cancel()");
        // Tear down any in-flight phase tracking + clear the step index so the
        // tracker resets to "no active step" on an explicit cancel.
        if (phase_tracker_.active) {
            end_phase_tracking_locked();
            set_operation_detail_locked("");
        } else {
            system_info_.operation_phase = -1;
        }
    }
    return execute_gcode("IFS_UNLOCK");
}

// --- Error-center bridge ---

std::vector<helix::RecoveryAction> AmsBackendAd5xIfs::build_recovery_actions() const {
    // Caller holds mutex_.
    if (!runout_active_) {
        // Operation-timeout fault. IFS_UNLOCK releases the firmware's operation
        // lock; it moves no filament, so it stays tappable on a cold nozzle
        // (needs_hot_nozzle defaults false).
        return {{lv_tr("Recover"), "IFS_UNLOCK", "ifs::unlock", "primary"}};
    }

    // Unattended runout. The print is paused with an empty toolhead and the user
    // has to put filament back before anything else is worth offering.
    std::vector<helix::RecoveryAction> actions;

    // Primary: carry on once filament is back at the nozzle. Resuming extrudes on
    // the very next move, so the hotend has to be up - the post-op cooldown or
    // idle_timeout has usually taken it down by the time anyone taps this.
    actions.push_back({lv_tr("Resume"), "RESUME", "ifs::resume", "primary",
                       /*needs_hot_nozzle=*/true});

    // Clear the tail of the old spool and prime with the new one. A bare
    // `M83` + `G1 E` and NOT zmod's PURGE_FILAMENT macro: this is a pure
    // extruder move, so it needs no homing (see the no-Load note below) and it
    // works on stock zMod as well as on either plugin. Obviously hot-only.
    actions.push_back({lv_tr("Purge"),
                       "M83\nG1 E" + std::to_string(RUNOUT_PURGE_MM) + " F" +
                           std::to_string(RUNOUT_PURGE_FEEDRATE_MM_MIN),
                       "ifs::purge", "",
                       /*needs_hot_nozzle=*/true});

    // NO "Load slot N" button, deliberately, even though a runout is exactly when
    // the user wants one. Every load path this backend has runs
    // INSERT_PRUTOK_IFS, whose macro homes itself before feeding
    // (filament_ops_self_home() and its comment). On the loadcell-Z AD5X that
    // `_G28` probes the nozzle DOWN into the part; with a job owning the toolhead
    // it trips ZMOD's ZCONTROL_AUTO and shuts Klipper down, recoverable only by a
    // firmware restart (bundle XWPBR2DX, commit 329e731e9). A runout state is
    // PAUSED by construction, so the button would fire straight into that.
    // refuse_if_printing() protects load_filament(); it does NOT protect a
    // recovery button, which hands its gcode directly to
    // IMoonrakerAPI::execute_gcode, and the `_G28` is buried inside the macro
    // where reject_homing_during_active_print() never sees it. Until there is a
    // verified non-homing load-to-toolhead command, the safe answer is to say so
    // in the detail text and let the user load from the AMS panel after the job
    // is cancelled or the print is resumed.

    // Last resort: release the firmware's clamps if the IFS wedged. State only.
    actions.push_back({lv_tr("Recover"), "IFS_UNLOCK", "ifs::unlock", "danger"});
    return actions;
}

std::optional<helix::ErrorEvent> AmsBackendAd5xIfs::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.action != AmsAction::ERROR)
        return std::nullopt;
    if (runout_active_) {
        // Title mirrors Happy Hare's runout wording so the two backends read the
        // same on screen. operation_detail carries build_runout_detail_locked()'s
        // text, including the plugin sentence.
        return helix::make_ams_fault_event(
            helix::ErrorSource::IFS, lv_tr("Filament runout"),
            system_info_.operation_detail.empty()
                ? std::string(lv_tr("Filament ran out at the toolhead"))
                : system_info_.operation_detail,
            build_recovery_actions());
    }
    return helix::make_ams_fault_event(helix::ErrorSource::IFS, lv_tr("Filament System Error"),
                                       system_info_.operation_detail.empty()
                                           ? std::string(lv_tr("Filament operation failed"))
                                           : system_info_.operation_detail,
                                       build_recovery_actions());
}

std::optional<bool> AmsBackendAd5xIfs::toolhead_filament_unaccounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // The SWITCH pair is the authority (head_switch_seen_ latches only on a
    // real filament_switch_sensor publish); head_filament_ is conflated with
    // the motion sensor, which reads false on a loaded-but-idle lane — gating
    // on it would misreport a healthy seated lane as unaccounted. Until the
    // switch has ever been seen there is no observation, so no verdict.
    if (!head_switch_seen_) {
        return std::nullopt;
    }
    return head_switch_present_ && system_info_.current_slot < 0;
}

// --- Plugin visibility (lessWaste / bambufy auto switchover) ---

AmsBackendAd5xIfs::IfsPlugin AmsBackendAd5xIfs::get_plugin() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_ifs_vars_) {
        return IfsPlugin::None;
    }
    return var_prefix_ == "bambufy" ? IfsPlugin::Bambufy : IfsPlugin::LessWaste;
}

std::optional<bool> AmsBackendAd5xIfs::plugin_backup_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ifs_backup_variable_;
}

bool AmsBackendAd5xIfs::runout_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runout_active_;
}

int AmsBackendAd5xIfs::backup_state_locked() const {
    // Stock zMod's ANALOG_PRUTOK is always-on — there is no toggle, so this is
    // a definite ON, not Unknown. The runout log already includes `plugin=none`
    // for context, so `backup=1` on that line reads correctly.
    if (!has_ifs_vars_) {
        return BACKUP_ON;
    }
    if (!ifs_backup_variable_.has_value()) {
        return BACKUP_UNKNOWN;
    }
    return *ifs_backup_variable_ ? BACKUP_ON : BACKUP_OFF;
}

bool AmsBackendAd5xIfs::parse_ifs_vars_macro_locked(const json& macro_status) {
    // Klipper returns `{}` for an object that does not exist, so an empty dict is
    // the "no macro" answer and carries nothing to read.
    if (!macro_status.is_object() || macro_status.empty()) {
        return false;
    }

    const auto parse_boolish = [](const json& v) -> std::optional<bool> {
        if (v.is_boolean()) {
            return v.get<bool>();
        }
        if (v.is_number_integer()) {
            return v.get<int>() != 0;
        }
        return std::nullopt;
    };

    bool changed = false;

    // lessWaste's runout backup toggle. Accepts the jinja int form the variable
    // dump shows (`variable_backup: 0`) and a bool, since neither plugin's schema
    // is pinned by anything we control.
    const auto backup_it = macro_status.find("variable_backup");
    if (backup_it != macro_status.end()) {
        std::optional<bool> parsed = parse_boolish(*backup_it);
        if (parsed.has_value() && parsed != ifs_backup_variable_) {
            ifs_backup_variable_ = parsed;
            // Mirror into the snapshot so get_system_info() agrees with the capabilities.
            // ifs_backup_variable_ stays the source of truth - it can be nullopt, which
            // the bool cannot express and which caps.enabled reports as Unknown.
            system_info_.endless_spool_enabled = *parsed;
            spdlog::info(
                "{} _IFS_VARS variable_backup = {} (automatic backup-spool switching on runout)",
                backend_log_tag(), *parsed ? "on" : "off");
            changed = true;
        }
    }

    // lessWaste's own post-boot IFS-unlock workaround (`_UNLOCK_IFS` ->
    // IFS_F18 after display_off_timeout). Log-only visibility, no state
    // subjects consume it: when a runout investigation lands on a device where
    // this is off, the bundle must show it, because a plain reboot may itself
    // change IFS behavior (clamped lanes) and confound a screen A/B (#1247).
    // The plugin owns the unlock; HelixScreen does not send hardware motion
    // unprompted.
    const auto unlock_it = macro_status.find("variable_ifs_unlock_after_boot");
    if (unlock_it != macro_status.end()) {
        std::optional<bool> parsed = parse_boolish(*unlock_it);
        if (parsed.has_value() && parsed != ifs_unlock_after_boot_) {
            ifs_unlock_after_boot_ = parsed;
            spdlog::info("{} _IFS_VARS ifs_unlock_after_boot = {} (plugin's own post-boot "
                         "IFS-unlock workaround)",
                         backend_log_tag(), *parsed ? "on" : "off");
        }
    }

    return changed;
}

// --- Backend-driven operation step model ---

AmsBackend::OperationStepModel
AmsBackendAd5xIfs::get_operation_step_model(StepOperationType op) const {
    // The AD5X synthesizes three firmware phases for each direction (see
    // apply_phase_action_locked): unload = Heat → Cut → Retract, load = Heat →
    // Feed → Purge. phase_id matches the operation_phase index (0/1/2) the
    // ams_operation_phase subject carries. Only the Heat step shows a live
    // nozzle temperature. Labels are wrapped in lv_tr() so they are translated
    // and picked up by the string-extraction tooling (mirrors Snapmaker).
    const bool unload = (op == StepOperationType::UNLOAD);
    OperationStepModel model;
    model.steps.push_back({lv_tr("Heat nozzle"), 0, false, /*live_temp=*/true});
    model.steps.push_back(
        {unload ? lv_tr("Cut filament") : lv_tr("Feed filament"), 1, false, false});
    model.steps.push_back({unload ? lv_tr("Retract") : lv_tr("Purge"), 2, false, false});
    return model;
}

lv_subject_t* AmsBackendAd5xIfs::get_operation_step_index_subject(StepOperationType /*op*/) {
    // The synthesized phase index (0/1/2) drives the current step directly via
    // AmsState's operation_phase subject — sync_from_backend() copies
    // system_info_.operation_phase into it.
    return AmsState::instance().get_ams_operation_phase_subject();
}

// --- Configuration ---

std::optional<std::vector<std::string>> AmsBackendAd5xIfs::get_supported_materials() const {
    // Stock AD5X firmware whitelist — see STOCK_WHITELIST in the header.
    std::vector<std::string> result(STOCK_WHITELIST.begin(), STOCK_WHITELIST.end());

    // Append user-defined types from bambufy_custom_types (save_variables) and
    // [zmod_ifs] filament_<NAME> (mod_data/user.cfg). zmod's COLOR macro
    // accepts these alongside the stock list, so they round-trip cleanly
    // through CHANGE_ZCOLOR / Adventurer5M.json without firmware rejection.
    // Including them here makes them appear in the edit modal dropdown AND
    // makes normalize_material()'s case-insensitive exact-match return them
    // unchanged on save (#904).
    auto lower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return out;
    };
    auto already_present = [&](const std::string& name) {
        std::string n_lc = lower(name);
        for (const auto& existing : result) {
            if (lower(existing) == n_lc) {
                return true;
            }
        }
        return false;
    };
    {
        // Use custom_types_mutex_ — NOT mutex_ — so callers that already hold
        // mutex_ (e.g., normalize_material() invoked inside set_slot_info)
        // don't deadlock.
        std::lock_guard<std::mutex> lock(custom_types_mutex_);
        for (const auto& name : custom_material_types_) {
            if (!already_present(name)) {
                result.push_back(name);
            }
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> AmsBackendAd5xIfs::get_material_aliases() const {
    // Names that mean "silk PLA" in the 3D printing world but map to AD5X's
    // distinct SILK slot type. Without these, the compat_group fallback
    // routes them to "PLA" (because silk PLA IS chemically PLA) and users
    // lose the silk distinction on slot edits / Orca imports.
    return {
        {"Silk", "SILK"},
        {"Silk PLA", "SILK"},
        {"PLA Silk", "SILK"},
    };
}

AmsError AmsBackendAd5xIfs::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }

    auto idx = static_cast<size_t>(slot_index);

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Update local state
        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
        }

        // Mark slot dirty to prevent parse_save_variables from overwriting our edit
        dirty_[idx] = true;

        // Convert color to hex string for our cached array
        char hex[7];
        snprintf(hex, sizeof(hex), "%06X", info.color_rgb & 0xFFFFFF);
        colors_[idx] = hex;

        // Normalize material to a value the IFS firmware will accept.
        // Empty input stays empty (an empty slot has no material), but any
        // non-empty input is coerced to the firmware whitelist so we never
        // send "PLA+" or "Silk PLA" and hit "Invalid material type".
        std::string normalized_material =
            info.material.empty() ? std::string{} : normalize_material(info.material);
        materials_[idx] = normalized_material;

        // Without per-port sensors, infer presence from user-provided data.
        // Setting color/material marks the slot occupied; clearing both marks it empty.
        if (!has_per_port_sensors_) {
            bool has_data =
                !normalized_material.empty() || info.color_rgb != AMS_DEFAULT_SLOT_COLOR;
            port_presence_[idx] = has_data;
        }

        spdlog::debug("{} set_slot_info: slot {} dirty=true, color={}, material={} (raw={}), "
                      "presence={}",
                      backend_log_tag(), slot_index, hex, normalized_material, info.material,
                      port_presence_[idx]);

        // Update entry directly. Covers every SlotInfo field the caller may
        // have set, not just the IFS-native color/material — otherwise a
        // persist=false "preview" write would silently drop brand /
        // spool_name / spoolman_* / color_name and the UI would snap back
        // to the previous values on the next get_slot_info().
        entry->info.color_rgb = info.color_rgb;
        entry->info.color_name = info.color_name;
        entry->info.material = normalized_material;
        entry->info.brand = info.brand;
        // Carry the catalog product identity through preview writes too — a
        // persist=false preview that dropped it would make the editor snap
        // back to a different variant on the next get_slot_info().
        entry->info.catalog_id = info.catalog_id;
        entry->info.product_name = info.product_name;
        entry->info.spool_name = info.spool_name;
        entry->info.spoolman_id = info.spoolman_id;
        entry->info.spoolman_vendor_id = info.spoolman_vendor_id;
        entry->info.remaining_weight_g = info.remaining_weight_g;
        entry->info.total_weight_g = info.total_weight_g;

        // If the caller asked for persistence, stage the new override into
        // overrides_ BEFORE update_slot_from_state() — otherwise the call
        // below will re-apply the PRE-EDIT override (if any), snap brand /
        // spool_name / spoolman_id back to their old saved values, and
        // revert the user's edit visually until the next parse. The
        // override store's own save_async fires outside the lock further
        // down, so there's only one place that mutates overrides_ for
        // persist=true set_slot_info.
        if (persist) {
            helix::ams::FilamentSlotOverride ovr;
            ovr.brand = info.brand;
            ovr.spool_name = info.spool_name;
            ovr.spoolman_id = info.spoolman_id;
            ovr.spoolman_vendor_id = info.spoolman_vendor_id;
            ovr.remaining_weight_g = info.remaining_weight_g;
            ovr.total_weight_g = info.total_weight_g;
            ovr.color_rgb = info.color_rgb;
            ovr.color_set = true; // a user-edit always records a color, even pure black (#000000)
            ovr.color_name = info.color_name;
            // normalize_material() was already applied to the cached
            // materials_ copy; reuse it so the on-disk record carries the
            // firmware-valid value instead of the raw user-typed string.
            ovr.material = normalized_material;
            // Catalog product identity. Persisted so a reopen can restore the
            // EXACT product rather than the alphabetically-first variant of the
            // same vendor+material. Never auto-mirrored (firmware has no notion
            // of a catalog product), so no user-lock flag is needed: a non-empty
            // value can only have come from a user pick.
            ovr.catalog_id = info.catalog_id;
            ovr.product_name = info.product_name;
            // User-lock signals: persist=true is a user edit, so tag both
            // fields user-locked. Material is only locked when the user
            // actually provided one — an explicit empty material is the user
            // saying "no material set", which the bootstrap mirror is allowed
            // to fill from a subsequent firmware report. See #965 for why
            // these locks exist (post-print firmware revert clobbered user
            // material via the OverwriteAlways auto-mirror).
            ovr.user_locked_color = true;
            ovr.user_locked_material = !normalized_material.empty();
            // SlotInfo carries the user's edit OR the bound Spoolman spool's
            // filament profile; the material-DB fallback for fields left at 0
            // is applied at emit time inside resolved_temps(). Centralized in
            // the helper so the four AMS backends stay in sync.
            helix::ams::populate_temps_from_slot_info(ovr, info);
            // updated_at left default — save_async stamps a fresh value so
            // the on-disk record's scan_time wins over any local clock skew.
            overrides_[slot_index] = ovr;
        }

        // Treat the user's chosen color as the new "firmware truth" baseline
        // so check_external_color_change() doesn't interpret the upcoming
        // update_slot_from_state() call as a foreign edit and fire a
        // redundant lane_data sync. The semantics match user intent: "I'm
        // telling the system this IS the current color." A subsequent
        // genuinely-external CHANGE_ZCOLOR will be detected against the
        // user's chosen color.
        //
        // Applies to both persist=true (override just staged above) and
        // persist=false (preview must not retrigger a sync against
        // last_firmware_color_). NO guard on color_rgb == 0: pure black is
        // a legitimate user choice and recording it as the baseline is
        // exactly what we want — the next firmware reading of black will
        // compare equal and not trigger a bogus sync. (Pre-fix this was
        // gated on != 0 to match the prior 0-as-no-signal contract; that
        // contract was wrong, so its mirror here is wrong too.)
        last_firmware_color_[slot_index] = info.color_rgb;

        // Symmetric material baseline seed: treat the user's chosen material as
        // the new firmware-truth baseline so the upcoming update_slot_from_state()
        // -> check_external_type_change() doesn't misread it as a foreign edit
        // and fire a redundant lane_data sync. Without this seed the material
        // path lacked the baseline the color path already established above,
        // reinforcing the missing-baseline gap on empty->insert (#981/#1065).
        last_firmware_material_[slot_index] = normalized_material;

        // Recalculate slot status now that port_presence may have changed.
        // update_slot_from_state() re-applies apply_overrides() from
        // overrides_ — which for persist=true now holds the values we
        // just staged above, so the override wins and matches the edit.
        // For persist=false with NO existing override, apply_overrides is
        // a no-op and the direct entry->info fields survive.
        update_slot_from_state(slot_index);
    }

    if (persist) {
        // Persist user-provided metadata to the slot-override store.
        //
        // Two persistence paths run here, by design:
        //
        //   1. IFS-native fields (color, material) are sent to Klipper via
        //      _IFS_VARS / Adventurer5M.json below — that's the printer-
        //      facing side the firmware and other UIs (Orca, LCD) see.
        //
        //   2. User metadata the firmware can't carry (brand, spool_name,
        //      spoolman_id, weights, color_name) lands in the Moonraker DB
        //      lane_data namespace via the override store. apply_overrides
        //      layers these back over firmware data on every parse.
        //
        // Color + material go into BOTH stores so an external writer
        // (Orca via its own MoonrakerPrinterAgent, another HelixScreen
        // instance) sees the full record in lane_data even when it's not
        // also listening to _IFS_VARS.
        if (override_store_) {
            // Re-read from overrides_ under the lock to get the same object
            // we staged above (including the normalized material). Cheap —
            // FilamentSlotOverride is a small POD-ish struct.
            helix::ams::FilamentSlotOverride ovr_to_save;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = overrides_.find(slot_index);
                if (it != overrides_.end()) {
                    ovr_to_save = it->second;
                }
            }
            // Capture backend_log_tag by value — the save callback may fire
            // well after set_slot_info returns (MR tracker ~60s timeout).
            // Do NOT capture `this`: the backend may outlive its store, but
            // the store will outlive the scheduled save by design.
            const std::string tag = backend_log_tag();
            override_store_->save_async(
                slot_index, ovr_to_save, [tag, slot_index](bool success, const std::string& err) {
                    if (!success) {
                        spdlog::warn("{} Override persist failed for slot {}: {}", tag, slot_index,
                                     err);
                    }
                });
        }

        // Write directly to Adventurer5M.json — zmod's authoritative store.
        // CHANGE_ZCOLOR is the macro-level equivalent but always emits the
        // Mainsail "Select print materials" prompt and (on display=True
        // setups) a native AD5X-screen popup, both of which the user must
        // dismiss manually. zmod re-reads Adventurer5M.json on every
        // GET_ZCOLOR call (no in-memory cache), so direct file writes are
        // picked up without ceremony.
        auto err = write_adventurer_json(slot_index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dirty_[idx] = false;
        }
        if (!err.success())
            return err;

        // lessWaste/bambufy users: also persist to the plugin's save_variables
        // store so its purge-skip logic sees consistent colors. zmod does not
        // read these — both writes are required for fully-synchronized state.
        // Best-effort: a failure here doesn't fail the operation because zmod's
        // truth (Adventurer5M.json) is already current.
        if (has_ifs_vars_) {
            std::string colors_val;
            std::string types_val;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                colors_val = build_color_list_value();
                types_val = build_type_list_value();
            }
            auto colors_err = write_ifs_var("colors", colors_val);
            if (!colors_err.success()) {
                spdlog::warn("{} _IFS_VARS colors write failed for slot {}: {}", backend_log_tag(),
                             slot_index, colors_err.technical_msg);
            }
            auto types_err = write_ifs_var("types", types_val);
            if (!types_err.success()) {
                spdlog::warn("{} _IFS_VARS types write failed for slot {}: {}", backend_log_tag(),
                             slot_index, types_err.technical_msg);
            }
        }
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

void AmsBackendAd5xIfs::update_slot_weight(int slot_index, float remaining_weight_g,
                                           float total_weight_g, bool persist) {
    if (slot_index < 0 || slot_index >= NUM_PORTS) {
        spdlog::warn("{} update_slot_weight: invalid slot {}", backend_log_tag(), slot_index);
        return;
    }

    // Weight is automated consumption-tracker data, not filament identity. We
    // touch ONLY the weight fields — never material/color, never the user-lock
    // flags, and never write_adventurer_json()/_IFS_VARS. set_slot_info()'s
    // firmware-facing writers re-emitted ffmType from a stale override material
    // on every 60 s persist, reverting the user's material to disk (#981). Weight
    // lives in the Moonraker DB lane_data override record, which is the only
    // store we persist here.
    helix::ams::FilamentSlotOverride ovr_to_save;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto* entry = slots_.get_mut(slot_index)) {
            entry->info.remaining_weight_g = remaining_weight_g;
            if (total_weight_g >= 0.0f)
                entry->info.total_weight_g = total_weight_g;
        }
        // overrides_[slot] default-constructs a weight-only record when the slot
        // had no prior override (material empty, color_set=false, locks false —
        // apply_overrides then layers only the weight). An existing override
        // (e.g. a user-locked material edit) keeps every other field intact.
        auto& ovr = overrides_[slot_index];
        ovr.remaining_weight_g = remaining_weight_g;
        if (total_weight_g >= 0.0f)
            ovr.total_weight_g = total_weight_g;
        ovr_to_save = ovr;
    }

    if (persist && override_store_) {
        const std::string tag = backend_log_tag();
        override_store_->save_async(
            slot_index, ovr_to_save, [tag, slot_index](bool ok, const std::string& err) {
                if (!ok) {
                    spdlog::warn("{} weight persist failed for slot {}: {}", tag, slot_index, err);
                }
            });
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

helix::printer::ToolMappingCapabilities AmsBackendAd5xIfs::get_tool_mapping_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_ifs_vars_) {
        return {false, false, ""};
    }
    return {.supported = true,
            .editable = true,
            .description = "Tool reassignment via _IFS_VARS"}; // i18n: do not translate
}

std::vector<int> AmsBackendAd5xIfs::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_ifs_vars_) {
        return {};
    }
    std::vector<int> result(TOOL_MAP_SIZE, -1);
    int highest_mapped = -1;
    for (size_t t = 0; t < TOOL_MAP_SIZE; ++t) {
        int port = tool_map_[t];
        if (port >= 1 && port <= NUM_PORTS) {
            result[t] = port - 1;
            highest_mapped = static_cast<int>(t);
        }
    }
    // Stop at the highest tool the firmware actually maps. AmsState's
    // build_ams_topology() takes ToolTopology::tool_count straight from this
    // vector's length, so returning all 16 addressable T-numbers made a 4-port,
    // single-hotend AD5X advertise a 16-tool machine. Trailing -1 padding is the
    // only thing dropped: entries below the cut keep their index, so an unmapped
    // T1 stays a -1 hole rather than sliding T2 down into its place. An entirely
    // unmapped register yields an empty vector, which is what the !has_ifs_vars_
    // path above already returns and what build_ams_topology() reads as "fall
    // back to a 1:1 map from the slot count".
    result.resize(static_cast<size_t>(highest_mapped + 1));
    return result;
}

AmsError AmsBackendAd5xIfs::set_tool_mapping(int tool_number, int slot_index) {
    if (tool_number < 0 || tool_number >= TOOL_MAP_SIZE) {
        return AmsErrorHelper::invalid_parameter("Invalid tool number");
    }

    int port = (slot_index >= 0 && slot_index < NUM_PORTS) ? (slot_index + 1) : UNMAPPED_PORT;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_map_[static_cast<size_t>(tool_number)] = port;

        // Update SlotRegistry reverse mapping
        for (int i = 0; i < NUM_PORTS; ++i) {
            int tool = find_first_tool_for_port(i + 1);
            slots_.set_tool_mapping(i, tool);
        }
    }

    // Persist tool mapping (only for lessWaste/bambufy — native ZMOD manages
    // tool mapping internally via the COLOR/SET_ZCOLOR dialog)
    if (!has_ifs_vars_) {
        return AmsErrorHelper::success();
    }

    std::string tools_val;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tools_val = build_tool_map_value();
    }

    return write_ifs_var("tools", tools_val);
}

// --- Bypass ---

AmsError AmsBackendAd5xIfs::enable_bypass() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    spdlog::info("{} Enabling bypass (external) mode", backend_log_tag());
    if (!has_ifs_vars_) {
        // Native ZMOD has no external/bypass mode variable — update local state only
        std::lock_guard<std::mutex> lock(mutex_);
        external_mode_ = true;
        return AmsErrorHelper::success();
    }
    return write_ifs_var("external", "1");
}

AmsError AmsBackendAd5xIfs::disable_bypass() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    spdlog::info("{} Disabling bypass (external) mode", backend_log_tag());
    if (!has_ifs_vars_) {
        std::lock_guard<std::mutex> lock(mutex_);
        external_mode_ = false;
        return AmsErrorHelper::success();
    }
    return write_ifs_var("external", "0");
}

// --- Private helpers ---

std::string AmsBackendAd5xIfs::build_color_list_value() const {
    return build_ifs_list_value(/*colors=*/true);
}

std::string AmsBackendAd5xIfs::build_type_list_value() const {
    return build_ifs_list_value(/*colors=*/false);
}

std::string AmsBackendAd5xIfs::build_ifs_list_value(bool colors) const {
    // Shape is plugin-specific (#1247):
    //
    //   * bambufy: 4-entry, PORT-indexed. `_RUNOUT_HEAD` iterates `ifs.types`
    //     (4 entries) and indexes `ifs.colors[port-1]` — the port-indexed
    //     payload this builder always produced.
    //   * lessWaste: 16-entry, TOOL-indexed. `variable_tools` maps tool->port
    //     (`[1,2,3,4,5,5,...]`) and `_RUNOUT_HEAD` scans ALL 16 tool slots
    //     comparing `ifs.colors[ifs.current_tool] == ifs.colors[index]` (plus
    //     type + the candidate's own port sensor) to find a backup lane. Our
    //     old 4-entry port-indexed payload was a wholesale replacement
    //     (`_IFS_VARS` does SET_GCODE_VARIABLE + SAVE_VARIABLE), so one push
    //     truncated the arrays to 4 — after which the scan of tools 4..15 read
    //     out of range and no backup could ever match, and the runout fell
    //     through to a plain pause with an empty toolhead.
    //
    // `_IFS_VARS` passes the value straight to SET_GCODE_VARIABLE, which evals
    // it as a Python literal; both shapes ride that unchanged.
    std::ostringstream ss;
    ss << "\"[";
    if (var_prefix_ != "less_waste") {
        for (int i = 0; i < NUM_PORTS; ++i) {
            if (i > 0)
                ss << ", ";
            ss << "'"
               << (colors ? colors_[static_cast<size_t>(i)] : materials_[static_cast<size_t>(i)])
               << "'";
        }
        ss << "]\"";
        return ss.str();
    }

    // Tool-indexed projection. Until the plugin's `<prefix>_tools` array has
    // been parsed, tool_map_ is all-UNMAPPED; lessWaste's own default mapping
    // there is identity (T0..T3 -> ports 1..4), so fall back to that rather
    // than overwrite a fresh plugin's defaults with 16 empty entries.
    bool any_mapped = false;
    for (int port : tool_map_) {
        if (port >= 1 && port <= NUM_PORTS) {
            any_mapped = true;
            break;
        }
    }
    for (int t = 0; t < TOOL_MAP_SIZE; ++t) {
        if (t > 0)
            ss << ", ";
        int port = tool_map_[static_cast<size_t>(t)];
        if (!any_mapped) {
            port = (t < NUM_PORTS) ? t + 1 : UNMAPPED_PORT;
        }
        if (port >= 1 && port <= NUM_PORTS) {
            const auto idx = static_cast<size_t>(port - 1);
            ss << "'" << (colors ? colors_[idx] : materials_[idx]) << "'";
        } else {
            // Unmapped tool: no lane, no colour. A candidate on an unmapped
            // tool can never pass lessWaste's port-sensor check, so the empty
            // entry is inert in `_RUNOUT_HEAD`'s comparisons.
            ss << "''";
        }
    }
    ss << "]\"";
    return ss.str();
}

std::string AmsBackendAd5xIfs::build_tool_map_value() const {
    // Integer list — no quotes around elements.
    // Example: "[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]"
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < TOOL_MAP_SIZE; ++i) {
        if (i > 0)
            ss << ", ";
        ss << tool_map_[static_cast<size_t>(i)];
    }
    ss << "]\"";
    return ss.str();
}

AmsError AmsBackendAd5xIfs::write_ifs_var(const std::string& key, const std::string& value) {
    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    // Use _IFS_VARS macro to persist state — works for both lessWaste and bambufy.
    // The macro updates in-memory gcode variables AND writes SAVE_VARIABLE with the
    // correct prefix automatically.
    std::string gcode = "_IFS_VARS " + key + "=" + value;
    spdlog::debug("{} Writing IFS var: {} = {}", backend_log_tag(), key, value);
    return execute_gcode(gcode);
}

void AmsBackendAd5xIfs::dispatch_ifs_vars_repair() {
    // lessWaste-only by construction (only parse_save_variables' lessWaste
    // branch stages it). Re-check the gate under the lock so a macro that went
    // missing between staging and dispatch (FIRMWARE_RESTART mid-frame) can't
    // get an _IFS_VARS write — the Unknown-command self-heal path relies on us
    // not prodding a dead macro.
    std::string colors_val, types_val;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_ifs_vars_) {
            return;
        }
        colors_val = build_color_list_value();
        types_val = build_type_list_value();
    }
    spdlog::info("{} Repairing truncated lessWaste colors/types arrays "
                 "(_IFS_VARS mirror, #1247)",
                 backend_log_tag());
    // No SHOW=0 suffix — lessWaste's _IFS_VARS has no SHOW param (bambufy-only).
    if (auto err = execute_gcode("_IFS_VARS colors=" + colors_val); !err.success()) {
        spdlog::warn("{} repair colors write failed: {}", backend_log_tag(), err.technical_msg);
    }
    if (auto err = execute_gcode("_IFS_VARS types=" + types_val); !err.success()) {
        spdlog::warn("{} repair types write failed: {}", backend_log_tag(), err.technical_msg);
    }
}

AmsError AmsBackendAd5xIfs::write_adventurer_json(int slot_index) {
    // Same-host fast path: write the file via direct filesystem access.
    // Avoids Moonraker's HTTP upload, which on AD5X stock-ZMOD does an
    // os.rename across mount points (/root/printer_data/tmp →
    // /usr/prog/config via symlink) and corrupts the file with EXDEV.
    // Bundle DQK7X96B (v0.99.52) was a bricked Klipper at boot from this
    // exact failure mode.
    if (!local_adventurer_json_path_.empty()) {
        return write_adventurer_json_local(slot_index);
    }

    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    auto idx = static_cast<size_t>(slot_index);
    int port = slot_index + 1; // JSON uses 1-based slot numbering

    std::string hex, material;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hex = colors_[idx];
        material = materials_[idx];
    }

    auto fields = ffm_fields_for_slot(hex, material);
    const std::string color_field = std::move(fields.color);
    const std::string type_field = std::move(fields.type);

    spdlog::info("{} Writing slot {} to Adventurer5M.json (native ZMOD)", backend_log_tag(), port);

    // Read-modify-write: download current file, update slot, re-upload
    auto result = std::make_shared<AmsError>(AmsErrorHelper::success());
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto token = lifetime_.token();

    api_->transfers().download_file(
        "config", "Adventurer5M.json",
        [this, token, port, color_field, type_field, result, done](const std::string& content) {
            if (token.expired()) { // L081_OK: sync wait wrapper called from main; defer would
                                   // deadlock against caller
                *result =
                    AmsErrorHelper::command_failed("write_adventurer_json", "Connection lost");
                done->store(true);
                return;
            }

            json doc;
            try {
                doc = json::parse(content);
            } catch (const json::parse_error& e) {
                spdlog::warn("{} Failed to parse Adventurer5M.json for write: {}",
                             backend_log_tag(), e.what());
                *result =
                    AmsErrorHelper::command_failed("write_adventurer_json", "JSON parse error");
                done->store(true);
                return;
            }

            // Ensure FFMInfo exists
            if (!doc.contains("FFMInfo")) {
                doc["FFMInfo"] = json::object();
            }

            // Update the slot
            doc["FFMInfo"]["ffmColor" + std::to_string(port)] = color_field;
            doc["FFMInfo"]["ffmType" + std::to_string(port)] = type_field;

            // Serialize with indentation to match zmod's format
            std::string updated = doc.dump(4);

            api_->transfers().upload_file(
                "config", "Adventurer5M.json", updated,
                [this, done, port]() {
                    spdlog::info("{} Wrote slot {} to Adventurer5M.json", backend_log_tag(), port);
                    done->store(true);
                },
                [this, result, done, port](const MoonrakerError& err) {
                    spdlog::warn("{} Failed to upload Adventurer5M.json for slot {}: {}",
                                 backend_log_tag(), port, err.message);
                    *result = AmsErrorHelper::command_failed("write_adventurer_json", err.message);
                    done->store(true);
                });
        },
        [this, result, done](const MoonrakerError& err) {
            spdlog::warn("{} Failed to download Adventurer5M.json for write: {}", backend_log_tag(),
                         err.message);
            *result = AmsErrorHelper::command_failed("write_adventurer_json", err.message);
            done->store(true);
        });

    // Wait for async operation to complete (this is called from a sync API)
    // The existing write_ifs_var / execute_gcode also blocks, so this is consistent.
    for (int i = 0; i < 100 && !done->load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!done->load()) {
        return AmsErrorHelper::command_failed("write_adventurer_json",
                                              "Timeout writing Adventurer5M.json");
    }

    return *result;
}

AmsError AmsBackendAd5xIfs::write_adventurer_json_local(int slot_index) {
    if (local_adventurer_json_path_.empty()) {
        return AmsErrorHelper::command_failed("write_adventurer_json_local",
                                              "Local path not resolved");
    }

    auto idx = static_cast<size_t>(slot_index);
    int port = slot_index + 1;

    std::string hex, material;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hex = colors_[idx];
        material = materials_[idx];
    }
    auto fields = ffm_fields_for_slot(hex, material);
    const std::string color_field = std::move(fields.color);
    const std::string type_field = std::move(fields.type);

    // Read-modify-write. An empty / unparseable existing file is treated as
    // "fresh start with empty FFMInfo" so we auto-repair the bricked-printer
    // case (corrupted Adventurer5M.json from a prior EXDEV upload — the very
    // failure mode this code path exists to fix).
    json doc;
    {
        std::ifstream in(local_adventurer_json_path_);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string content = buf.str();
        if (content.empty()) {
            doc = json::object();
        } else {
            try {
                doc = json::parse(content);
            } catch (const json::parse_error&) {
                spdlog::warn("{} Adventurer5M.json at {} is unparseable; rewriting from scratch",
                             backend_log_tag(), local_adventurer_json_path_);
                doc = json::object();
            }
        }
    }

    if (!doc.contains("FFMInfo") || !doc["FFMInfo"].is_object()) {
        doc["FFMInfo"] = json::object();
    }
    doc["FFMInfo"]["ffmColor" + std::to_string(port)] = color_field;
    doc["FFMInfo"]["ffmType" + std::to_string(port)] = type_field;

    const std::string updated = doc.dump(4);

    // Atomic write: stage to <path>.tmp in the same directory, then rename().
    // POSIX rename() is atomic when src+dst are on the same filesystem — that's
    // guaranteed here because both live in the same directory. Critically, we
    // do NOT cross filesystems the way Moonraker's upload does.
    const std::string tmp_path = local_adventurer_json_path_ + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return AmsErrorHelper::command_failed("write_adventurer_json_local",
                                                  std::string("open(") + tmp_path +
                                                      ") failed: " + std::strerror(errno));
        }
        out << updated;
        out.flush();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return AmsErrorHelper::command_failed("write_adventurer_json_local", "Write failed");
        }
    }

    if (std::rename(tmp_path.c_str(), local_adventurer_json_path_.c_str()) != 0) {
        const int saved_errno = errno;
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return AmsErrorHelper::command_failed("write_adventurer_json_local",
                                              std::string("rename failed: ") +
                                                  std::strerror(saved_errno));
    }

    spdlog::info("{} Wrote slot {} to Adventurer5M.json (direct fs path: {})", backend_log_tag(),
                 port, local_adventurer_json_path_);
    return AmsErrorHelper::success();
}

void AmsBackendAd5xIfs::detect_local_adventurer_json_path() {
    // Only meaningful when Moonraker runs on the same host — otherwise
    // /usr/prog/config/ is on a remote filesystem we can't touch.
    std::string moonraker_host;
    if (helix::Config* cfg = helix::Config::get_instance()) {
        moonraker_host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
    }
    if (!helix::is_moonraker_on_same_host(moonraker_host)) {
        spdlog::debug("{} Moonraker is remote ({}); leaving Adventurer5M.json on upload path",
                      backend_log_tag(), moonraker_host);
        return;
    }

    // Candidate paths in priority order. /usr/prog/config is the AD5X stock-ZMOD
    // canonical install. /opt/config/Adventurer5M.json is where ZMOD-on-ForgeX
    // stages it. Both are the EXDEV-prone destinations on AD5X stock — that's
    // the entire reason we want direct write.
    static constexpr std::array<const char*, 2> candidates = {
        "/usr/prog/config/Adventurer5M.json",
        "/opt/config/Adventurer5M.json",
    };

    for (const auto* candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) {
            continue;
        }
        // Must be a regular file (or a symlink resolving to one) and writable.
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
            continue;
        }
        if (::access(candidate, W_OK) != 0) {
            spdlog::debug("{} {} exists but is not writable (errno={}); skipping",
                          backend_log_tag(), candidate, errno);
            continue;
        }
        local_adventurer_json_path_ = candidate;
        spdlog::info("{} Resolved Adventurer5M.json local path: {} "
                     "(bypasses Moonraker upload to avoid EXDEV on /usr/prog/config symlink)",
                     backend_log_tag(), candidate);
        return;
    }

    spdlog::debug("{} No local Adventurer5M.json candidate found; staying on Moonraker upload path",
                  backend_log_tag());
}

void AmsBackendAd5xIfs::fetch_user_cfg_materials() {
    if (!api_)
        return;

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "mod_data/user.cfg",
        [this, token](const std::string& content) {
            // BG THREAD: parse_user_cfg_filament_types is static, no this access.
            auto names = parse_user_cfg_filament_types(content);
            if (names.empty()) {
                spdlog::debug("[AMS AD5X-IFS] user.cfg parsed: no [zmod_ifs] filament_* "
                              "entries");
                return;
            }

            // MAIN THREAD: merge into custom_material_types_ under member mutex.
            token.defer("Ad5xIfsBackend::user_cfg_apply",
                        [this, names = std::move(names)]() mutable {
                            // Append new names, preserving existing
                            // bambufy_custom_types order and de-duplicating
                            // case-insensitively.
                            auto lower = [](const std::string& s) {
                                std::string out = s;
                                std::transform(out.begin(), out.end(), out.begin(),
                                               [](unsigned char c) { return std::tolower(c); });
                                return out;
                            };
                            size_t total;
                            {
                                std::lock_guard<std::mutex> lock(custom_types_mutex_);
                                for (const auto& n : names) {
                                    std::string n_lc = lower(n);
                                    bool exists = false;
                                    for (const auto& existing : custom_material_types_) {
                                        if (lower(existing) == n_lc) {
                                            exists = true;
                                            break;
                                        }
                                    }
                                    if (!exists) {
                                        custom_material_types_.push_back(n);
                                    }
                                }
                                total = custom_material_types_.size();
                            }
                            spdlog::info("{} user.cfg: loaded {} user-defined filament type(s); "
                                         "total custom types {}",
                                         backend_log_tag(), names.size(), total);
                        });
        },
        [token](const MoonrakerError& err) {
            // BG THREAD: log only — no member access required, so no defer.
            // (token captured to keep the lambda's call signature consistent
            // with other download_file error paths.)
            (void)token;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
                spdlog::debug("[AMS AD5X-IFS] user.cfg not present (404) — no user-defined "
                              "types to merge");
            } else {
                spdlog::debug("[AMS AD5X-IFS] user.cfg fetch failed: {}", err.message);
            }
        });
}

std::vector<std::string> AmsBackendAd5xIfs::parse_user_cfg_filament_types(const std::string& body) {
    // zmod docs: https://wiki.zmod.link/AD5X/#7-add-custom-filament-types
    //
    //   [zmod_ifs]
    //   filament_NEWTYPE: 300
    //
    // Section is Klipper-style INI. Comments start with '#' or ';'. Values
    // are decimal temperatures we don't currently use — we only collect the
    // NAME tokens (uppercased convention, but preserve user case as written).
    // Any whitespace before the section header or key is tolerated; we don't
    // try to fully reimplement Klipper's INI parser, just match the lines we
    // care about within the [zmod_ifs] section.
    std::vector<std::string> out;
    std::istringstream is(body);
    std::string line;
    bool in_section = false;
    static const std::regex section_re(R"(^\s*\[\s*([^\]\s]+)\s*\]\s*$)");
    static const std::regex filament_re(R"(^\s*filament_([A-Za-z0-9_+\-]+)\s*[:=].*$)");
    while (std::getline(is, line)) {
        // Strip trailing CR for files saved with CRLF.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Strip inline comments — Klipper accepts both '#' and ';'.
        for (char ch : {'#', ';'}) {
            auto pos = line.find(ch);
            if (pos != std::string::npos) {
                line.erase(pos);
                break;
            }
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        std::smatch m;
        if (std::regex_match(line, m, section_re)) {
            in_section = (m[1].str() == "zmod_ifs");
            continue;
        }
        if (in_section && std::regex_match(line, m, filament_re)) {
            out.push_back(m[1].str());
        }
    }
    return out;
}

void AmsBackendAd5xIfs::read_adventurer_json() {
    if (!api_)
        return;

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "Adventurer5M.json",
        [this, token](const std::string& content) {
            // BG THREAD: log size from local-only data (tag string is constexpr).
            // Demoted to trace (#981): this fires on every ~5s poll.
            spdlog::trace("[AMS AD5X-IFS] Downloaded Adventurer5M.json ({} bytes)", content.size());
            // MAIN THREAD: note_json_content takes mutex_ and parse_adventurer_json
            // mutates extensive member state.
            token.defer("Ad5xIfsBackend::read_json_apply", [this, content]() mutable {
                // Baseline the poll cache so the next periodic poll
                // doesn't see this content as "changed" and
                // double-fire GET_ZCOLOR.
                (void)note_json_content(content);
                parse_adventurer_json(content);
            });
        },
        [this, token](const MoonrakerError& err) {
            // BG THREAD: log error from local-only data; defer the atomic
            // store + 404 logging as it touches a member atomic.
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
                token.defer("Ad5xIfsBackend::read_json_404", [this]() {
                    json_poll_supported_.store(false);
                    spdlog::info("{} Adventurer5M.json not found — not a native ZMOD AD5X",
                                 backend_log_tag());
                });
            } else {
                spdlog::warn("[AMS AD5X-IFS] Failed to download Adventurer5M.json: {}",
                             err.message);
            }
        });
}

void AmsBackendAd5xIfs::fetch_filament_json() {
    if (!api_ || !filament_json_supported_.load())
        return;

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "mod_data/filament.json",
        [this, token](const std::string& content) {
            // BG THREAD: no member access here — parse_filament_json takes
            // mutex_ and mutates the cache, so defer it to the main thread.
            spdlog::trace("[AMS AD5X-IFS] Downloaded filament.json ({} bytes)", content.size());
            token.defer("Ad5xIfsBackend::filament_json_apply",
                        [this, content]() mutable { parse_filament_json(content); });
        },
        [this, token](const MoonrakerError& err) {
            // BG THREAD: defer the atomic store + 404 logging (touches a member
            // atomic). filament.json is zmod-specific; absence is expected on
            // non-zmod printers and must not retry.
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
                token.defer("Ad5xIfsBackend::filament_json_404", [this]() {
                    filament_json_supported_.store(false);
                    spdlog::info("{} filament.json not found — using default eject LEN/SPEED",
                                 backend_log_tag());
                });
            } else {
                spdlog::debug("[AMS AD5X-IFS] filament.json fetch failed: {}", err.message);
            }
        });
}

void AmsBackendAd5xIfs::parse_filament_json(const std::string& content) {
    // filament.json is a JSON object keyed by filament TYPE name plus a
    // "default" entry. Each value carries filament_tube_length (the unload
    // retract distance, our IFS_F11 LEN) and filament_ifs_speed (IFS_F11
    // SPEED), among many fields we don't use. Build a {tube_length, ifs_speed}
    // pair per material; resolve missing fields against the "default" entry,
    // and missing "default" against the literal 1000/1200.
    json root;
    try {
        root = json::parse(content);
    } catch (const std::exception& e) {
        spdlog::warn("[AMS AD5X-IFS] filament.json parse failed: {}", e.what());
        return;
    }
    if (!root.is_object()) {
        spdlog::warn("[AMS AD5X-IFS] filament.json is not a JSON object — ignoring");
        return;
    }

    // int field reader with a fallback, tolerant of numeric-as-string values.
    auto read_int = [](const json& obj, const char* key, int fallback) -> int {
        auto it = obj.find(key);
        if (it == obj.end())
            return fallback;
        if (it->is_number_integer() || it->is_number_unsigned())
            return it->get<int>();
        if (it->is_number_float())
            return static_cast<int>(it->get<double>());
        if (it->is_string()) {
            try {
                return std::stoi(it->get<std::string>());
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    };

    // Resolve the "default" pair first so per-material fallbacks key off it.
    int default_len = 1000;
    int default_speed = 1200;
    auto def_it = root.find("default");
    if (def_it != root.end() && def_it->is_object()) {
        default_len = read_int(*def_it, "filament_tube_length", 1000);
        default_speed = read_int(*def_it, "filament_ifs_speed", 1200);
    }

    std::map<std::string, std::pair<int, int>> parsed;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() == "default" || !it.value().is_object())
            continue;
        int len = read_int(it.value(), "filament_tube_length", default_len);
        int speed = read_int(it.value(), "filament_ifs_speed", default_speed);
        parsed[it.key()] = {len, speed};
    }

    size_t cached_count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        filament_eject_default_ = {default_len, default_speed};
        filament_eject_params_ = std::move(parsed);
        cached_count = filament_eject_params_.size();
    }
    spdlog::info("{} filament.json: cached eject LEN/SPEED for {} material(s); default {}/{}",
                 backend_log_tag(), cached_count, default_len, default_speed);
}

bool AmsBackendAd5xIfs::note_json_content(const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (content == last_json_content_) {
        return false;
    }
    last_json_content_ = content;
    return true;
}

void AmsBackendAd5xIfs::poll_adventurer_json() {
    if (!api_ || !json_poll_supported_.load())
        return;
    if (json_poll_in_flight_.exchange(true)) {
        return; // Previous poll still running — coalesce.
    }

    auto token = lifetime_.token();
    auto on_content = [this, token](const std::string& content) {
        // MAIN THREAD: clear in-flight gate AND apply the parse together
        // so we never touch member state on the bg thread. The gate may
        // stay "true" for one extra UpdateQueue tick — harmless coalescing.
        token.defer("Ad5xIfsBackend::poll_json_apply", [this, content]() mutable {
            json_poll_in_flight_.store(false);

            if (!note_json_content(content)) {
                spdlog::trace("{} Adventurer5M.json unchanged ({} bytes), "
                              "skipping GET_ZCOLOR",
                              backend_log_tag(), content.size());
                return;
            }

            spdlog::debug("{} Adventurer5M.json changed ({} bytes), parsing + "
                          "scheduling GET_ZCOLOR",
                          backend_log_tag(), content.size());
            parse_adventurer_json(content);
            schedule_zcolor_query("json_poll");
        });
    };
    auto on_error = [this, token](const MoonrakerError& err) {
        // MAIN THREAD: clearing the gate + atomic + log lives in the defer
        // so no member access happens on the bg thread.
        if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
            token.defer("Ad5xIfsBackend::poll_json_404", [this]() {
                json_poll_in_flight_.store(false);
                json_poll_supported_.store(false);
                spdlog::info("{} Adventurer5M.json poll: file not found, disabling poll",
                             backend_log_tag());
            });
        } else {
            token.defer("Ad5xIfsBackend::poll_json_err", [this, msg = err.message]() {
                json_poll_in_flight_.store(false);
                spdlog::debug("{} Adventurer5M.json poll failed (will retry): {}",
                              backend_log_tag(), msg);
            });
        }
    };

#if defined(ESP_PLATFORM)
    // Task 15 R2 evaluation arm (CONFIG_HELIX_AMS_HTTP_POLL_BACKENDS):
    // download_file() is a hard stub on ESP32 (Task 10's HTTP lane only
    // supports bounded fetches — see esp_rest_api.cpp). Adventurer5M.json is
    // a small generated config; ADVENTURER_JSON_POLL_CAP_BYTES is generous
    // enough a real file rarely hits it, and download_file_partial fails
    // loud on an over-cap response rather than silently truncating (see
    // esp_http_lane.cpp) — same graceful degrade as any other poll failure
    // handled by on_error above.
    static constexpr size_t ADVENTURER_JSON_POLL_CAP_BYTES = 32 * 1024;
    api_->transfers().download_file_partial("config", "Adventurer5M.json",
                                            ADVENTURER_JSON_POLL_CAP_BYTES, on_content, on_error);
#else
    api_->transfers().download_file("config", "Adventurer5M.json", on_content, on_error);
#endif
}

bool AmsBackendAd5xIfs::on_gcode_response_line(const std::string& line) {
    // Lines arriving while our own GET_ZCOLOR is in flight belong to that
    // response — buffer them and DO NOT treat as external triggers. zmod's
    // GET_ZCOLOR macro body itself echoes RUN_ZCOLOR/CHANGE_ZCOLOR tokens;
    // without this guard the listener self-feeds another schedule_zcolor_query
    // on every response, producing a 2-4 Hz spam loop on the gcode console
    // (raza/DIEHARDave report on v0.99.51).
    if (zcolor_query_active_.load()) {
        std::lock_guard<std::mutex> lock(zcolor_buffer_mutex_);
        zcolor_response_buffer_.push_back(line);
        return true;
    }

    // Phase-tracker corroboration (SECONDARY — never load-bearing). The
    // firmware emits exact phase markers we parse for the heat target and the
    // op direction *early*, before the extruder/sensor signals confirm them.
    // The temp+sensor path alone drives the full sequence; these strings only
    // refine the target number and disambiguate direction. English wording
    // varies across forks, so a miss is harmless.
    //
    // NOTE: register_zcolor_listener()'s bg-side pre-filter must admit these
    // tokens too (keep in sync — see the comment block there).
    bool phase_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_tracker_.active) {
            static const std::regex heat_re(R"(Heating the nozzle to\s+(\d+))");
            std::smatch m;
            if (std::regex_search(line, m, heat_re)) {
                int degrees = std::atoi(m[1].str().c_str());
                if (degrees > 0) {
                    phase_tracker_.target_deci = helix::ui::temperature::degrees_to_deci(degrees);
                    spdlog::debug("{} Phase: RESPOND heat target {}°C", backend_log_tag(), degrees);
                    phase_changed = apply_phase_action_locked();
                }
            } else if (line.find("Unloading filament") != std::string::npos) {
                phase_tracker_.is_unload = true;
                phase_changed = apply_phase_action_locked();
            } else if (line.find("Loading filament") != std::string::npos) {
                phase_tracker_.is_unload = false;
                phase_changed = apply_phase_action_locked();
            }
        }
    }
    // Lock released — publish the synthesized phase transition so the UI's
    // ams_action subject + step tracker advance immediately (lock contract:
    // emit_event must run with mutex_ NOT held).
    if (phase_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }

    // External-unload presence resurrection. When the user unloads (or loads)
    // a lane via zmod's OWN color macro (AD5X LCD / Mainsail), the console
    // stream carries NO RUN_ZCOLOR / CHANGE_ZCOLOR token, so the branch below
    // never fires and the emptied lane keeps showing loaded. zmod's
    // _SET_EXTRUDER_SLOT (zmod_color.py cmd_SET_EXTRUDER_SLOT) emits a bare
    //     Extruder: <N>
    // via respond_raw at the channel-commit step near the END of the
    // operation — the captured live sequence (raza616 AD5X) ends with
    // "Extruder: 3" / "Setting active filament T99" / "Enable IFS". We key off
    // that line to schedule one GET_ZCOLOR refresh so the now-empty lane
    // updates.
    //
    // Why THIS token and not the others in the sequence:
    //   - "Setting active filament T", "Enable IFS", "Filament ABSENT in
    //     extruder" are all LOCALIZED by zmod (ru: "Включаю IFS", "Указываю
    //     активный пруток T", "Пруток ОТСУСТВУЕТ в экструдере") — a ru user
    //     would never hit them.
    //   - "Extruder: <N>" is an un-localized f-string and is the channel-commit
    //     marker, so it doubles as a clean terminal signal.
    //   - The literal IN_ZCOLOR token only appears in the dialog button
    //     DEFINITION echo ("action:prompt_button Unload|IN_ZCOLOR ...") at
    //     prompt-render time, NOT when the unload runs — watching it would
    //     false-fire on dialog-open and still miss the real unload.
    //
    // The match is STRICT — bare "Extruder: <int>" only. Both the GET_ZCOLOR
    // SILENT header ("Extruder: ... | IFS: True") and the interactive prompt
    // ("action:prompt_text Extruder: ... | IFS:") carry a " | IFS:" suffix, and
    // per-slot rows look like "3: PLA/HEX"; none match the bare form. Combined
    // with the zcolor_query_active_ early-return guard above (which buffers our
    // own in-flight query echoes), this keeps the v0.99.51 self-feedback spam
    // loop closed: GET_ZCOLOR never emits a bare "Extruder: N", so a re-read
    // can't re-trigger itself.
    //
    // Self-trigger note: HelixScreen's own SET_EXTRUDER_SLOT (set_active_tool)
    // also produces a bare "Extruder: N". Re-reading after a Helix-initiated
    // tool change is harmless and desirable (fresh state); schedule_zcolor_query
    // is debounced + idempotent, so it collapses to one query either way.
    static const std::regex extruder_commit_re(R"(^\s*(?://\s*)?Extruder:\s*\d+\s*$)");
    if (std::regex_search(line, extruder_commit_re)) {
        spdlog::debug("{} Detected external channel commit ('{}') in gcode stream — "
                      "scheduling GET_ZCOLOR to resurrect presence",
                      backend_log_tag(), line);
        schedule_zcolor_query("external_extruder_commit");
        return false;
    }

    // NOTE: register_zcolor_listener() has a bg-side pre-filter that drops
    // every line not containing one of these tokens (and not buffering for
    // an active query). If you add a new trigger here, update that filter
    // too — otherwise the new token will be silently dropped before it
    // reaches this function on heavy-print response streams.
    if (line.find("RUN_ZCOLOR") != std::string::npos ||
        line.find("CHANGE_ZCOLOR") != std::string::npos) {
        // Menu-definition echo vs. executed command (#1065, bundle 482NB943).
        // zmod renders every COLOR dialog by echoing its buttons down the
        // console as `// action:prompt_button <label>|<gcode>|<style>[|<hex>]`.
        // Those payloads are the menu's OFFER LIST, not a record of anything
        // the firmware did — the "Select color" grid alone carries 24 distinct
        // CHANGE_ZCOLOR candidates. Running them through the extractor below
        // applied all 24 in sequence, so the slot ended up on the LAST swatch
        // (#161616) and the last material in the whitelist (PETG-CF) until the
        // confirming GET_ZCOLOR corrected it ~1s later — a visible flicker,
        // a false "external edit detected" delta, and a lane_data write per
        // phantom apply (25 server.database.post_item requests queued in 300ms
        // on a 473MB MIPS AD5X). 87 echoed buttons produced 162 phantom applies
        // in a two-minute session.
        //
        // A real edit — the AD5X LCD, Mainsail, zmod's own macro, or the native
        // screen's RESPOND MSG="…" re-echo — never carries the action:prompt_
        // prefix, so the synchronous extraction added in v0.99.94 is untouched.
        if (line.find("action:prompt_") != std::string::npos) {
            // One shape IS load-bearing: the root menu's per-slot rows are a
            // four-slot firmware snapshot, and the freshest one in the stream.
            apply_color_menu_slot_row(line);
            ++external_change_burst_count_;
            schedule_json_reread();
            schedule_zcolor_query("color_menu_render");
            return false;
        }

        // A CHANGE_ZCOLOR in the gcode stream is always a DELIBERATE external
        // edit: HelixScreen persists colors by writing Adventurer5M.json directly
        // and never emits CHANGE_ZCOLOR, so this command can only originate from
        // the AD5X LCD, Mainsail, or zmod's COLOR/material macro. If the slot
        // still carries a user-locked override from an earlier HelixScreen edit,
        // sync_override_to_firmware_locked() skips the locked color/material
        // (the #965 guard) and apply_overrides() keeps re-painting that stale
        // value on every parse — so the user's new firmware color/type never
        // surfaces (raza616, #981: set yellow PLA on the zmod screen, HelixScreen
        // kept showing the previously Helix-set white PETG). The user has plainly
        // overridden their earlier choice, so drop the stale override and let
        // firmware truth (colors_/materials_, refreshed by the re-read below)
        // show through. clear_slot_override() persists the clear, so it survives
        // a restart instead of reloading the locked value from lane_data.
        //
        // This is gated on a CHANGE_ZCOLOR carrying a real locked override —
        // RUN_ZCOLOR (display-only) and the post-print FFMInfo auto-revert that
        // #965 guards against (it rewrites the JSON WITHOUT emitting
        // CHANGE_ZCOLOR) never reach the clear, so the lock still protects the
        // user's material there.
        //
        // TYPE=/HEX= extraction (#1065 v0.99.94, bundle NJB2U558): the
        // CHANGE_ZCOLOR token carries the user's intent at emission time:
        //   CHANGE_ZCOLOR SLOT=N [TYPE=<material>] [HEX=<rgb>]
        // Both parameters are OPTIONAL — zmod's "Change color" menu button omits
        // HEX= (keeps current color, sets TYPE) and vice versa. The follow-up
        // GET_ZCOLOR SILENT=1 query used to be the only path to refresh
        // colors_/materials_, but it queues on Klipper's serial gcode line
        // behind any running IFS op (INSERT_PRUTOK_IFS, IFS_F11 eject, even
        // unrelated long macros) and can take 1-3 minutes to return — or hit
        // the 60s RPC timeout and miss entirely. The user read that lag as
        // "Failed to update material type" (mkleersn 07-18/07-20 sheets).
        // Extracting TYPE=/HEX= directly from the gcode makes the refresh
        // synchronous and lets GET_ZCOLOR degrade to a confirming no-op.
        if (line.find("CHANGE_ZCOLOR") != std::string::npos) {
            static const std::regex slot_re(R"(SLOT=(\d+))");
            // Match TYPE= up to the next whitespace, a '|', or end of string.
            // zmod's stock whitelist is single-token (PLA, PLA-CF, PETG, PETG-CF,
            // SILK, ABS, TPU), and custom [zmod_ifs] filament_<NAME> entries are
            // single-word by construction. No quoted/multiword form exists.
            //
            // The '|' stop is load-bearing: zmod's prompt-render macro echoes the
            // per-slot buttons as RESPOND action:prompt_button lines whose payload
            // is `label|gcode|color|hex`, e.g.
            //   action:prompt_button Change color|CHANGE_ZCOLOR SLOT=4 TYPE=SILK|primary|F72224
            // Those lines contain the CHANGE_ZCOLOR substring, so they reach this
            // extractor, and a greedy `TYPE=(\S+)` captured `SILK|primary|F72224`
            // — poisoning materials_/last_firmware_material_ with the button
            // descriptor. The later clean GET_ZCOLOR read then saw a
            // `SILK|primary|F72224 -> SILK` delta and fired a spurious
            // sync_override_to_firmware_locked that pinned a stale color into a
            // fresh auto-mirror override (#1065 raza616 07-22, bundle H2X5QMCU).
            // HEX= is already `{6}`-bounded so it stops at the '|' on its own.
            static const std::regex type_re(R"(TYPE=([^\s|]+))");
            // HEX= is exactly 6 hex digits in zmod output. Tolerate lowercase
            // (Mainsail console typing) — canonicalized to upper below.
            static const std::regex hex_re(R"(HEX=([0-9A-Fa-f]{6}))");
            std::smatch m;
            if (std::regex_search(line, m, slot_re)) {
                // zmod SLOT is 1-based; overrides_ is 0-based.
                int slot0 = std::atoi(m[1].str().c_str()) - 1;

                // Extract optional TYPE= / HEX= payloads BEFORE taking the lock
                // (regex_search is the expensive part; do it once on the local
                // line string). empty optional == "not present in this gcode".
                std::optional<std::string> parsed_type;
                std::smatch tm;
                if (std::regex_search(line, tm, type_re)) {
                    parsed_type = tm[1].str();
                }
                std::optional<std::string> parsed_hex;
                std::smatch hm;
                if (std::regex_search(line, hm, hex_re)) {
                    parsed_hex = hm[1].str();
                    // Canonicalize to upper-case so the follow-up GET_ZCOLOR
                    // response (which parse_zcolor_silent also receives as
                    // upper-case) compares equal — avoiding a spurious
                    // "color changed" re-sync when the query finally returns.
                    for (auto& c : *parsed_hex) {
                        c = static_cast<char>(toupper(c));
                    }
                }

                const bool slot_valid = (slot0 >= 0 && slot0 < NUM_PORTS);
                const bool has_params = parsed_type.has_value() || parsed_hex.has_value();

                // Decide under the lock whether there's real work: clearing a
                // stale locked override (existing #981 path) and/or applying
                // freshly-parsed TYPE=/HEX= parameters to colors_/materials_.
                bool mutated = false;
                if (slot_valid) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = overrides_.find(slot0);
                    const bool has_locked_override =
                        (it != overrides_.end() &&
                         (it->second.user_locked_color || it->second.user_locked_material));

                    if (has_locked_override) {
                        spdlog::info(
                            "{} External CHANGE_ZCOLOR for slot {} overrides an earlier "
                            "HelixScreen edit — releasing the stale color/material locks so "
                            "the new firmware color/type wins (#981)",
                            backend_log_tag(), slot0);
                        auto* entry = slots_.get_mut(slot0);
                        if (entry) {
                            // Release the stale color/material locks + strip the
                            // firmware-carryable override fields, then re-run
                            // update_slot_from_state so entry->info is refreshed
                            // from firmware truth (colors_/materials_) NOW — we
                            // don't want to wait on the async re-read below. The
                            // firmware CAN'T carry brand/spool metadata, so this
                            // RETAINS it (Bug B): a bare LCD-load CHANGE_ZCOLOR
                            // must not drop the user's saved vendor. Falls back to
                            // a full erase when there is no identity to keep.
                            release_locked_override_keep_identity_locked(slot0, entry->info);
                            mutated = true;
                        }
                    }

                    if (has_params) {
                        // Apply the user's intent directly to the firmware-truth
                        // arrays. Baselines MUST advance too, so the next
                        // check_external_*_change observation (e.g. from a
                        // GET_ZCOLOR response or a parse_adventurer_json poll)
                        // compares equal and doesn't fire a redundant
                        // sync_override_to_firmware_locked round-trip.
                        const auto idx = static_cast<size_t>(slot0);
                        if (parsed_type) {
                            materials_[idx] = *parsed_type;
                            last_firmware_material_[slot0] = *parsed_type;
                            spdlog::info("{} External CHANGE_ZCOLOR applied TYPE='{}' to slot {} "
                                         "(#1065 gcode-path extraction)",
                                         backend_log_tag(), *parsed_type, slot0);
                            mutated = true;
                        }
                        if (parsed_hex) {
                            colors_[idx] = *parsed_hex;
                            try {
                                const uint32_t color_value =
                                    static_cast<uint32_t>(std::stoul(*parsed_hex, nullptr, 16));
                                last_firmware_color_[slot0] = color_value;
                                spdlog::info(
                                    "{} External CHANGE_ZCOLOR applied HEX='{}' to slot {} "
                                    "(#1065 gcode-path extraction)",
                                    backend_log_tag(), *parsed_hex, slot0);
                                mutated = true;
                            } catch (...) {
                                // std::stoul on a regex-validated 6-hex-digit
                                // string can't realistically throw; defensive
                                // only. Don't advance the baseline on the
                                // off-chance the parse failed — the next
                                // GET_ZCOLOR poll will recover.
                            }
                        }

                        // Refresh a pre-existing NON-locked auto-mirror override
                        // to match what we just wrote. Without this, the values
                        // above land in colors_/materials_ but apply_overrides
                        // (below, via update_slot_from_state) re-masks them with
                        // the override's stale color_rgb / material — so the
                        // just-tapped value never surfaces. Concretely: change a
                        // slot's type then its color from back-to-back COLOR
                        // macros and the new color never appeared, because an
                        // earlier external edit had created an auto-mirror
                        // override still pinning the previous color (#1065
                        // raza616 07-22, bundle H2X5QMCU).
                        //
                        // Only refresh an override that ALREADY exists. Never
                        // create one here: mirror_firmware_to_lane_data
                        // default-constructs the map entry, and zmod floods the
                        // stream with echoed CHANGE_ZCOLOR button-definition
                        // lines on every prompt render — syncing on each would
                        // fabricate auto-mirror overrides for slots the user
                        // never edited. When no override exists, apply_overrides
                        // is already a no-op and firmware truth shows unaided.
                        //
                        // The #981 clear above erases any locked override before
                        // we get here, so a surviving entry is auto-mirror
                        // (locks false); sync_override_to_firmware_locked diffs
                        // both fields and honors user_locked_* regardless, so it
                        // updates only the dimension that actually changed. Skip
                        // when no real firmware color baseline exists yet, so we
                        // don't pin black (0x000000) onto the override.
                        if (mutated) {
                            auto ovr_it = overrides_.find(slot0);
                            auto color_it = last_firmware_color_.find(slot0);
                            if (ovr_it != overrides_.end() &&
                                color_it != last_firmware_color_.end()) {
                                sync_override_to_firmware_locked(slot0, color_it->second,
                                                                 materials_[idx]);
                            }
                        }
                    }

                    if (mutated) {
                        // Re-run apply_overrides + slot-state derivation so the
                        // UI-visible SlotInfo picks up the new values. If a
                        // locked override was cleared above, apply_overrides is
                        // now a no-op and the firmware-truth arrays win.
                        update_slot_from_state(slot0);
                    }
                }
                if (mutated) {
                    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot0));
                }
            }
        }
        // Count the detection rather than logging per line — zmod re-emits
        // CHANGE_ZCOLOR on every edit, so one user action floods the stream with
        // trigger lines (24 in a 3s window in bundle UQG4RNUA). schedule_*()
        // already coalesce the actual work; reread_apply logs a single
        // consolidated line carrying this count when the debounced re-read fires.
        ++external_change_burst_count_;
        schedule_json_reread();
        schedule_zcolor_query("external_change_zcolor");
        return false;
    }

    // Self-heal: Klipper/Kalico answer unrecognised commands with
    // `// Unknown command:"X"` (via gcode.cmd_default → respond_info). If
    // our own _IFS_VARS mirror writes come back rejected, the
    // lessWaste/bambufy macro isn't actually loaded even though
    // has_ifs_vars_ said it was — demote and latch missing so the rest of
    // the session takes the native-ZMOD path and stops echoing rejected
    // gcodes on every Adventurer5M.json poll.
    if (line.find("Unknown command:\"_IFS_VARS\"") != std::string::npos) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_ifs_vars_ || !ifs_macro_confirmed_missing_) {
            spdlog::warn("{} Klipper rejected _IFS_VARS as unknown command — "
                         "disabling has_ifs_vars_ for the session "
                         "(plugin macro not loaded)",
                         backend_log_tag());
            has_ifs_vars_ = false;
            ifs_macro_confirmed_missing_ = true;
        }
        return false;
    }
    return false;
}

bool AmsBackendAd5xIfs::apply_color_menu_slot_row(const std::string& line) {
    // Shape (zmod _ZCOLOR_MENU render, one per slot):
    //   // action:prompt_button 1: SILK|RUN_ZCOLOR SLOT=1 HEX=F330F9 TYPE=SILK|primary|F330F9
    // The `<n>: ` label prefix is what separates a slot row from the action
    // submenu's buttons (Load|IN_ZCOLOR …, Change color|CHANGE_ZCOLOR …), and
    // RUN_ZCOLOR is display-only — it never mutates firmware state, so a row
    // can be read as a snapshot with no risk of confusing it for a command.
    static const std::regex slot_row_re(
        R"(action:prompt_button\s+(\d+)\s*:[^|]*\|\s*RUN_ZCOLOR\b)");
    std::smatch rm;
    if (!std::regex_search(line, rm, slot_row_re))
        return false;

    // Same token grammar as the CHANGE_ZCOLOR extractor: TYPE stops at the '|'
    // that begins the button's style/hex suffix, HEX is 6 digits.
    static const std::regex slot_re(R"(SLOT=(\d+))");
    static const std::regex type_re(R"(TYPE=([^\s|]+))");
    static const std::regex hex_re(R"(HEX=([0-9A-Fa-f]{6}))");
    std::smatch sm;
    if (!std::regex_search(line, sm, slot_re))
        return false;
    // The label index and SLOT= must agree, or this isn't the row we think it
    // is (a localized or restyled menu, a future zmod layout). Bail rather than
    // write firmware-truth arrays from a line we don't actually understand.
    if (rm[1].str() != sm[1].str())
        return false;

    const int slot0 = std::atoi(sm[1].str().c_str()) - 1; // zmod SLOT is 1-based
    if (slot0 < 0 || slot0 >= NUM_PORTS)
        return false;

    std::smatch tm;
    std::smatch hm;
    const bool has_type = std::regex_search(line, tm, type_re);
    const bool has_hex = std::regex_search(line, hm, hex_re);
    if (!has_type && !has_hex)
        return false;

    std::string hex;
    if (has_hex) {
        hex = hm[1].str();
        for (auto& c : hex) {
            c = static_cast<char>(toupper(c));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto idx = static_cast<size_t>(slot0);
        const bool same =
            (!has_type || materials_[idx] == tm[1].str()) && (!has_hex || colors_[idx] == hex);
        if (same)
            return false; // Nothing moved — the common case on a re-render.

        if (has_type)
            materials_[idx] = tm[1].str();
        if (has_hex)
            colors_[idx] = hex;

        spdlog::info("{} Slot {} refreshed from COLOR-menu row (material='{}' color='{}')",
                     backend_log_tag(), slot0, materials_[idx], colors_[idx]);

        // Deliberately does NOT pre-advance last_firmware_* the way the
        // CHANGE_ZCOLOR path does: this is a firmware READING, so it must flow
        // through check_external_*_change exactly as a GET_ZCOLOR response
        // would — refreshing a non-locked auto-mirror override and mirroring to
        // lane_data, while honouring a user-locked one. It is the same data the
        // debounced query returns, only ~500ms sooner.
        update_slot_from_state(slot0);
    }
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot0));
    return true;
}

void AmsBackendAd5xIfs::register_zcolor_listener() {
    if (!client_)
        return;

    static const std::string handler_name = "ifs_zcolor_watcher";
    auto token = lifetime_.token();

    client_->register_method_callback(
        "notify_gcode_response", handler_name, [this, token](const json& msg) {
            // BG THREAD: extract the line from the JSON envelope into a local;
            // no `this` access here.
            std::string line;
            if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
                msg["params"][0].is_string()) {
                line = msg["params"][0].get<std::string>();
            } else if (msg.is_array() && !msg.empty() && msg[0].is_string()) {
                line = msg[0].get<std::string>();
            } else {
                return;
            }

            // BG-side pre-filter (queue-pressure economy): notify_gcode_response
            // fires for EVERY gcode console line — hundreds/sec on a busy print
            // with macros echoing. on_gcode_response_line only acts in two
            // cases: (a) `zcolor_query_active_` is set, in which case the line
            // is buffered as part of a GET_ZCOLOR SILENT=1 response, or (b) the
            // line contains the RUN_ZCOLOR / CHANGE_ZCOLOR token that signals
            // an externally-driven color change. Every other line is dropped on
            // the main thread. Move that filter to bg so non-matching lines
            // never hit the UpdateQueue.
            //
            // Race analysis: zcolor_query_active_ is std::atomic<bool>, set via
            // exchange(true) BEFORE api_->execute_gcode("GET_ZCOLOR SILENT=1")
            // is called (see query_zcolor_silent). Klipper guarantees gcode-
            // response ordering, so any response line emitted by the GET_ZCOLOR
            // macro body necessarily arrives on the WS thread AFTER the bg load
            // sees zcolor_query_active_ == true. Lines emitted before the macro
            // started can't be query-response lines and are correctly dropped
            // (matches main-thread behaviour: load() would also be false there
            // because the deferred lambda preserves arrival order via the
            // FIFO UpdateQueue).
            const bool query_active = zcolor_query_active_.load(std::memory_order_acquire);
            const bool is_zcolor_token = (line.find("RUN_ZCOLOR") != std::string::npos ||
                                          line.find("CHANGE_ZCOLOR") != std::string::npos);
            // External-unload presence resurrection: zmod's _SET_EXTRUDER_SLOT
            // emits a bare "Extruder: <N>" at the channel-commit step (no
            // RUN_ZCOLOR/CHANGE_ZCOLOR in the stream). Cheap substring admit
            // here; on_gcode_response_line applies the STRICT bare-form regex
            // so SILENT-response "Extruder: ... | IFS:" headers and per-slot
            // rows don't actually schedule. Keep this admit in sync with the
            // extruder_commit_re branch in on_gcode_response_line.
            const bool is_extruder_commit = (line.find("Extruder:") != std::string::npos);
            // Self-heal: if our _IFS_VARS mirror writes come back as
            // "Unknown command", the lessWaste/bambufy macro isn't actually
            // loaded — demote has_ifs_vars_ so we stop spamming the console.
            // The substring match (no leading `// `) handles both Klipper's
            // and Kalico's response framing.
            const bool is_unknown_ifs_vars =
                (line.find("Unknown command:\"_IFS_VARS\"") != std::string::npos);
            // Phase-tracker corroboration tokens (target temp + op direction).
            // String .find() on the LOCAL line only — no member access on the
            // bg thread (L081/L072 safe). Must mirror the parse triggers in
            // on_gcode_response_line.
            const bool is_phase_token = (line.find("Heating the nozzle to") != std::string::npos ||
                                         line.find("Unloading filament") != std::string::npos ||
                                         line.find("Loading filament") != std::string::npos);
            if (!query_active && !is_zcolor_token && !is_unknown_ifs_vars && !is_phase_token &&
                !is_extruder_commit) {
                return;
            }

            // MAIN THREAD: on_gcode_response_line touches several member fields
            // (zcolor_query_active_, zcolor_buffer_mutex_, schedule_*).
            token.defer("Ad5xIfsBackend::zcolor_listener_apply",
                        [this, line = std::move(line)]() mutable { on_gcode_response_line(line); });
        });
}

void AmsBackendAd5xIfs::register_klippy_ready_listener() {
    if (!client_)
        return;

    // notify_klippy_ready fires on every klippy startup->ready transition (cold
    // boot once klippy finishes initialising, and after FIRMWARE_RESTART). This
    // is the point at which zmod has actually come online and GET_ZCOLOR
    // SILENT=1 can return per-slot state.
    static const std::string handler_name = "ifs_klippy_ready_watcher";
    auto token = lifetime_.token();

    client_->register_method_callback(
        "notify_klippy_ready", handler_name, [this, token](const json& /*msg*/) {
            // MAIN THREAD: schedule_* helpers spawn HttpExecutor work that
            // re-reads via api_; both rely on `this` being alive.
            token.defer("Ad5xIfsBackend::klippy_ready_apply", [this]() {
                spdlog::info("{} notify_klippy_ready — scheduling GET_ZCOLOR SILENT=1",
                             backend_log_tag());
                // Re-check macro existence: FIRMWARE_RESTART can change which
                // gcode_macros are loaded (e.g. user uninstalls lessWaste and
                // restarts Klipper while helixscreen is still running). Without
                // this, has_ifs_vars_ stays cached true and we keep firing
                // _IFS_VARS writes that Klipper now rejects as "Unknown
                // command:".
                recheck_ifs_vars_macro();
                // Re-read the JSON cache too — firmware may have persisted new
                // colors during boot, and the stream may have missed
                // RUN_ZCOLOR notifications that happened before we reconnected.
                schedule_json_reread();
                schedule_zcolor_query("reconnect_macro_check");
            });
        });
}

void AmsBackendAd5xIfs::recheck_ifs_vars_macro() {
    if (!client_)
        return;

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query", json{{"objects", json{{"gcode_macro _ifs_vars", nullptr}}}},
        [this, token](const json& response) {
            // BG THREAD: extract macro_exists; no `this` access.
            // Same detection rule as on_started: key presence isn't enough
            // (Klipper returns `{}` for missing objects), require a non-empty
            // variables dict.
            bool macro_exists = false;
            json macro_status;
            if (response.contains("result") && response["result"].contains("status")) {
                const auto& status = response["result"]["status"];
                macro_exists = status.contains("gcode_macro _ifs_vars") &&
                               status["gcode_macro _ifs_vars"].is_object() &&
                               !status["gcode_macro _ifs_vars"].empty();
                // Keep the dict, not just the existence bool: `gcode_macro
                // _ifs_vars` is NOT in the standing objects.subscribe set, so the
                // initial on_started() query and this recheck are the only two
                // places variable_backup ever reaches us (#1250).
                if (macro_exists) {
                    macro_status = status["gcode_macro _ifs_vars"];
                }
            }
            token.defer("Ad5xIfsBackend::recheck_macro_apply",
                        [this, macro_exists, macro_status = std::move(macro_status)]() {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (macro_exists) {
                                parse_ifs_vars_macro_locked(macro_status);
                                if (ifs_macro_confirmed_missing_) {
                                    spdlog::info(
                                        "{} _IFS_VARS macro now present (post FIRMWARE_RESTART) — "
                                        "re-enabling save_variables tool-mapping reads",
                                        backend_log_tag());
                                    ifs_macro_confirmed_missing_ = false;
                                }
                            } else {
                                if (!ifs_macro_confirmed_missing_ || has_ifs_vars_) {
                                    spdlog::warn("{} _IFS_VARS macro no longer present (post "
                                                 "FIRMWARE_RESTART) — disabling _IFS_VARS writes",
                                                 backend_log_tag());
                                }
                                ifs_macro_confirmed_missing_ = true;
                                has_ifs_vars_ = false;
                                // The plugin went away with the macro; a remembered
                                // variable_backup would be a claim about software that is
                                // no longer installed.
                                ifs_backup_variable_.reset();
                                system_info_.endless_spool_enabled = false;
                            }
                        });
        });
}

void AmsBackendAd5xIfs::unregister_moonraker_listeners() {
    if (!client_)
        return;
    client_->unregister_method_callback("notify_gcode_response", "ifs_zcolor_watcher");
    client_->unregister_method_callback("notify_klippy_ready", "ifs_klippy_ready_watcher");
}

void AmsBackendAd5xIfs::schedule_json_reread() {
    if (reread_pending_.exchange(true))
        return;

    auto token = lifetime_.token();

    // Bounded worker pool — bare std::thread on AD5M can hit EAGAIN under
    // thread exhaustion (feedback_no_bare_threads_arm.md).
    helix::http::HttpExecutor::fast().submit([this, token]() {
        // BG THREAD: only the debounce sleep — no member touch.
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // MAIN THREAD: clear the gate atomic and chain into read_adventurer_json,
        // which itself touches api_ and member state.
        token.defer("Ad5xIfsBackend::reread_apply", [this]() {
            reread_pending_.store(false);
            const int n = external_change_burst_count_;
            external_change_burst_count_ = 0;
            if (n > 0) {
                spdlog::debug("{} Detected {} external color change(s) in gcode stream — "
                              "re-reading Adventurer5M.json + querying zcolor",
                              backend_log_tag(), n);
            } else {
                // Reread scheduled by a non-stream trigger (klippy_ready, etc.).
                spdlog::debug("{} Re-reading Adventurer5M.json after external change",
                              backend_log_tag());
            }
            read_adventurer_json();
        });
    });
}

void AmsBackendAd5xIfs::schedule_zcolor_query(const char* reason) {
    if (!zcolor_silent_supported_.load() && !ifs_status_ports_seen_.load()) {
        // GET_ZCOLOR SILENT is unsupported AND we have never seen IFS_STATUS
        // Ports — nothing clean to query, fall to JSON polling. Once IFS_STATUS
        // Ports has been seen, keep scheduling so IFS_STATUS (clean JSON, no
        // prompt) refreshes presence even after a SILENT demotion (#981).
        return;
    }
    zcolor_schedule_count_.fetch_add(1, std::memory_order_relaxed);
    // Diagnostic-only: record which operation wants this refresh. Last-writer-
    // wins when bursts coalesce into a single query — fine for field diagnostics.
    zcolor_query_reason_pending_ = reason;
    // Semantics of zcolor_query_pending_: "a refresh is wanted". Set here and
    // re-set by query_zcolor_silent() when it can't run (active in flight).
    // Cleared on claim by the first worker to wake OR by finalize_zcolor_response.
    zcolor_query_pending_.store(true);

    // Coalesce: only ONE debounce worker is ever in flight. A burst of
    // CHANGE_ZCOLOR lines (zmod re-pops the color prompt on every edit) used to
    // submit one fast()-pool worker each — 20+ in a 40ms window in bundle
    // ACJRZBXJ — every one sleeping out 500ms on a pool slot while a single query
    // actually fired. The pending flag above already carries the intent, so once
    // a worker is armed, later callers just set it and return. (Mirror of the
    // reread_pending_ gate in schedule_json_reread().)
    if (zcolor_schedule_armed_.exchange(true)) {
        return;
    }
    zcolor_worker_submit_count_.fetch_add(1, std::memory_order_relaxed);

    auto token = lifetime_.token();
    helix::http::HttpExecutor::fast().submit([this, token]() {
        // BG THREAD: just the debounce sleep — no member touch.
        // Short debounce — coalesce bursts from port-sensor changes, the
        // gcode stream, and unload-complete triggers.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // MAIN THREAD: disarm so a trigger arriving after this point arms a fresh
        // worker, then claim the pending flag and run the query. Both touch
        // members (zcolor_query_pending_, api_ via query_zcolor_silent).
        token.defer("Ad5xIfsBackend::zcolor_debounce_apply", [this]() {
            zcolor_schedule_armed_.store(false);
            if (!zcolor_query_pending_.exchange(false)) {
                return; // Another worker (or finalize) already claimed this refresh.
            }
            query_zcolor_silent();
        });
    });
}

void AmsBackendAd5xIfs::query_zcolor_silent() {
    if (!api_)
        return;
    const bool silent = zcolor_silent_supported_.load();
    // After a SILENT demotion we still want IFS_STATUS (clean JSON, no prompt) to
    // keep presence live — but only if the device actually speaks it. With
    // neither path available, fall to JSON polling.
    if (!silent && !ifs_status_ports_seen_.load())
        return;
    if (zcolor_query_active_.exchange(true)) {
        // Already in flight — mark pending so finalize will re-schedule.
        zcolor_query_pending_.store(true);
        spdlog::debug("{} GET_ZCOLOR SILENT=1 already in flight, deferring", backend_log_tag());
        return;
    }

    // Real fire path: promote the pending trigger reason so it rides this query
    // through to the IFS_STATUS Chan diagnostic log (diagnostic-only).
    zcolor_query_reason_active_ = zcolor_query_reason_pending_;

    {
        std::lock_guard<std::mutex> lock(zcolor_buffer_mutex_);
        zcolor_response_buffer_.clear();
    }

    auto token = lifetime_.token();

    // Always fire IFS_STATUS (fire-and-forget): its respond_info JSON carries the
    // seated channel ("Chan") and per-port presence ("Ports"). Chan persists at
    // the loaded port while idle — unlike GET_ZCOLOR's "Extruder:" feed view that
    // reads "None" when loaded-idle. zcolor_query_active_ is already set, so the
    // JSON line lands in zcolor_response_buffer_ and is parsed by
    // parse_zcolor_silent. Clean JSON (not a prompt dialog), so it works even on
    // old zmod where GET_ZCOLOR degrades to a prompt-fallback. Routed through the
    // backend's fire-and-forget execute_gcode() rather than the callback-taking
    // IMoonrakerAPI overload.
    execute_gcode("IFS_STATUS");

    if (!silent) {
        // SILENT demoted — IFS_STATUS is the only query this turn. Nothing
        // drives finalize, so schedule it ourselves after the same collection
        // window the GET_ZCOLOR callback uses, so the IFS_STATUS response in the
        // buffer gets parsed + applied (Ports presence stays live, #981).
        spdlog::debug("{} Querying IFS_STATUS only (SILENT demoted)", backend_log_tag());
        helix::http::HttpExecutor::fast().submit([this, token]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            token.defer("Ad5xIfsBackend::ifs_status_finalize_apply",
                        [this]() { finalize_zcolor_response(); });
        });
        return;
    }

    spdlog::debug("{} Querying GET_ZCOLOR SILENT=1", backend_log_tag());
    // silent=true: this is a background color-state poll. Klipper runs gcode
    // serially, so when a physical IFS operation stalls (e.g. "Purging old
    // filament timed out"), this poll queues behind it on the printer and hits
    // the 60s RPC timeout — which would otherwise raise a scary user-facing
    // "Printer command 'printer.gcode.script' timed out" toast on top of the
    // real filament-error modal the user is already seeing (bundles AS394UHU /
    // UQG4RNUA). The error callback below still logs + clears the in-flight flag
    // so the next trigger re-arms the poll. Mirrors get_sensors() / EXCLUDE_OBJECT.
    api_->execute_gcode(
        "GET_ZCOLOR SILENT=1",
        [this, token]() {
            // BG THREAD: HttpExecutor::fast() is a static accessor — no `this`
            // touch — so we can submit the debounce worker without a guard.
            // Response lines arrive via notify_gcode_response listener;
            // schedule finalization after a short collection window.
            helix::http::HttpExecutor::fast().submit([this, token]() {
                // BG THREAD: just the debounce sleep.
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                // MAIN THREAD: finalize touches mutex_, atomics, and chains
                // into apply_zcolor_result + possibly query_zcolor_silent.
                token.defer("Ad5xIfsBackend::zcolor_finalize_apply",
                            [this]() { finalize_zcolor_response(); });
            });
        },
        [this, token](const MoonrakerError& err) {
            // MAIN THREAD: log + clear the active flag together so no member
            // touch happens on the bg execute_gcode error thread.
            token.defer("Ad5xIfsBackend::zcolor_query_err", [this, msg = err.message]() {
                spdlog::warn("{} GET_ZCOLOR SILENT=1 failed: {}", backend_log_tag(), msg);
                zcolor_query_active_.store(false);
            });
        },
        /*timeout_ms=*/0, /*silent=*/true, /*on_queued=*/nullptr,
        // The error callback logs and re-arms the poll; it shows the user
        // nothing. Recording it for dedup would mute GcodeErrorRouter's `!!`
        // copy of a real GET_ZCOLOR rejection. See include/rpc_error_policy.h.
        /*caller_surfaces_errors=*/false);
}

void AmsBackendAd5xIfs::finalize_zcolor_response() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(zcolor_buffer_mutex_);
        lines.swap(zcolor_response_buffer_);
    }
    zcolor_query_active_.store(false);

    if (!lines.empty()) {
        auto result = parse_zcolor_silent(lines, zcolor_query_reason_active_);
        apply_zcolor_result(result);
    } else {
        spdlog::debug("{} GET_ZCOLOR SILENT=1 returned no lines", backend_log_tag());
    }

    // If a trigger arrived during the active window, query again directly —
    // we just finished a query, no need to debounce further.
    if (zcolor_query_pending_.exchange(false)) {
        spdlog::debug("{} Re-querying GET_ZCOLOR SILENT=1 (trigger fired during last query)",
                      backend_log_tag());
        // Diagnostic-only: mark the re-fire so its IFS_STATUS log is
        // distinguishable from the original trigger.
        zcolor_query_reason_pending_ = "requeue_pending";
        query_zcolor_silent();
    }
}

void AmsBackendAd5xIfs::apply_zcolor_result(const ZColorSilentResult& result) {
    // IFS_STATUS "Chan" is the seated-channel authority and rides the same
    // response buffer as clean JSON (respond_info, not a prompt dialog). Apply
    // it BEFORE the prompt-fallback / no-content early-returns so an old-zmod
    // GET_ZCOLOR dialog can't discard the seated-slot signal. Also drives the
    // unload/load phase finalize, which on old zmod has no other clean terminal
    // signal (GET_ZCOLOR's extruder_slot is unavailable behind the prompt).
    if (result.ifs_chan.has_value()) {
        bool chan_changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int chan = *result.ifs_chan; // 1-based, 0 = none

            // IFS_STATUS "Chan" reports the LAST channel the switching mechanism
            // engaged, which includes a cold-lane eject (IFS_F11): after ejecting
            // an idle lane, Chan points at that now-EMPTY port. Bundle CGR6C7PA
            // (#1065) captured it directly — `Chan:1` with `Ports:[false,..]` right
            // after ejecting cold lane 1 while lane 2 stayed seated at the head.
            // Adopting that as the seated channel moves "seated" onto an empty lane
            // and reclassifies the genuinely-seated lane as a cold eject, grinding
            // its un-cut filament (#1065 Bug 3). The same IFS_STATUS frame carries
            // the silk-sensor Ports presence, so reject a non-zero Chan that THIS
            // frame shows as an empty lane. Chan==0 (nothing engaged) and frames
            // without a Ports reading apply unchanged — the seated channel only
            // moves onto a lane the simultaneous presence snapshot confirms has
            // filament.
            // Presence of a 0-based lane, preferring THIS frame's IFS_STATUS Ports
            // (the simultaneous silk-sensor snapshot) over the last-known per-port
            // presence. Used to corroborate both the eject-stale-Chan rejection and
            // the cold-boot restore below.
            const auto lane_present = [&](int slot0) -> bool {
                if (slot0 < 0 || slot0 >= NUM_PORTS)
                    return false;
                if (result.ifs_ports.has_value())
                    return (*result.ifs_ports)[static_cast<size_t>(slot0)];
                return port_presence_[static_cast<size_t>(slot0)];
            };

            // Eject-stale rejection (#1065 Bug 3) trusts ONLY this frame's Ports
            // snapshot — never the last-known port_presence_, which lags and would
            // wrongly reject a freshly-seated Chan on a frame that carried no Ports
            // reading (the plugin path applies Chan without Ports).
            const bool chan_lane_empty =
                (chan > 0 && chan <= NUM_PORTS && result.ifs_ports.has_value() &&
                 !(*result.ifs_ports)[static_cast<size_t>(chan - 1)]);

            // Same corroboration for FFMInfo.channel. It is sticky (only the ~5s
            // JSON poll refreshes it, only an eject clears it), so right after a
            // load into a DIFFERENT lane it still names the PREVIOUS lane. If that
            // lane reads empty in THIS frame's Ports snapshot, the pointer is
            // stale — do not adopt it as seated, or the load-complete frame flashes
            // the previously-seated lane as loaded and repaints its retained
            // override (#1065). Only trust THIS frame's Ports (never lagging
            // port_presence_), mirroring chan_lane_empty; on a Ports-less frame
            // the pointer is adopted as before.
            const bool ffm_lane_empty =
                (ffm_channel_ > 0 && ffm_channel_ <= NUM_PORTS && result.ifs_ports.has_value() &&
                 !(*result.ifs_ports)[static_cast<size_t>(ffm_channel_ - 1)]);

            // Adventurer5M.json's FFMInfo.channel is the firmware's own persistent
            // record of the lane currently at the toolhead — the SAME field its
            // _IFS_REMOVE_CURRENT_PRUTOK unload macro resolves from. It stays put
            // while idle, unlike IFS_STATUS "Chan", which tracks the LAST lane the
            // switching mechanism referenced — including a zmod COLOR-menu slot
            // SELECTION that moves no filament (#1065 Bug 3, mkleersn bundle
            // ZT8Y9WPM: editing lane 3's colour made Chan=3 while FFMInfo.channel
            // stayed 2, and Helix wrongly moved current_slot onto lane 3). So a
            // known FFMInfo.channel is the seated authority and a divergent Chan is
            // ignored; Chan is only consulted when FFMInfo.channel is absent (0 —
            // nothing seated yet, or the firmware forgot it across a reboot, where
            // the persisted-lane floor below takes over).
            // Head-gate (#1065 row 28): FFMInfo.channel is sticky — the firmware
            // does NOT blank it on eject/unload (same as ffmColor/ffmType). After
            // ejecting a cold lane whose FFMInfo.channel still points at it, an
            // un-gated adoption re-seats that now-empty lane and keeps offering
            // Unload ("shows loaded"). Reject the sticky channel ONLY when the
            // toolhead SWITCH sensor authoritatively reads empty — never on the
            // ifs_motion_sensor's loaded-idle false-negative — so a genuinely
            // seated lane is never dropped. On motion-only firmware (no switch
            // published) head_switch_seen_ stays false and we fall back to
            // adopting FFMInfo.channel as before.
            const bool head_empty_authoritative = head_switch_seen_ && !head_switch_present_;
            if (ffm_channel_ > 0 && head_empty_authoritative) {
                if (seated_chan_ != 0) {
                    spdlog::debug("{} FFMInfo.channel={} rejected as seated: toolhead switch "
                                  "reads empty (sticky/stale channel, #1065 row 28); clearing "
                                  "seated_chan_ (was {})",
                                  backend_log_tag(), ffm_channel_, seated_chan_);
                }
                seated_chan_ = 0;
            } else if (ffm_channel_ > 0 && !ffm_lane_empty) {
                if (seated_chan_ != ffm_channel_ || chan != ffm_channel_) {
                    spdlog::debug("{} Seated lane from FFMInfo.channel={} (IFS_STATUS Chan={} {})",
                                  backend_log_tag(), ffm_channel_, chan,
                                  chan == ffm_channel_ ? "agrees" : "diverges — Chan ignored");
                }
                seated_chan_ = ffm_channel_;
            } else if (chan_lane_empty) {
                spdlog::debug("{} IFS_STATUS Chan={} ignored as seated: lane empty this "
                              "frame (eject-engaged stale channel); keeping seated_chan_={}",
                              backend_log_tag(), chan, seated_chan_);
            } else if (chan == 0 && !seated_resolved_since_boot_ && head_filament_ &&
                       persisted_seated_slot_.has_value() &&
                       lane_present(*persisted_seated_slot_)) {
                // Power-cycle floor (#1065): the firmware forgets the seated channel
                // across a reboot — IFS_STATUS returns Chan=0 even with a lane
                // physically at the head (bundle CGR6C7PA). The head sensor proves a
                // lane is loaded and the remembered lane is still present, so restore
                // it as the seated channel rather than dropping to "nothing seated"
                // (which would label every lane Unloadable). A real Chan>0, or a
                // load/unload, supersedes it on a later frame.
                const int restored = *persisted_seated_slot_ + 1;
                if (seated_chan_ != restored) {
                    spdlog::info("{} Cold-boot: Chan=0 with head loaded; restoring remembered "
                                 "seated lane (slot {})",
                                 backend_log_tag(), *persisted_seated_slot_);
                }
                seated_chan_ = restored;
            } else {
                // Record the physically seated port — the unload router reads this
                // directly so the seated channel is recognised even on the plugin
                // path, where the tool_map_-derived current_slot is owned by
                // save_variables and can disagree with the seated port.
                seated_chan_ = chan;
            }

            // A known present-lane FFMInfo.channel, a real seated channel (Chan>0),
            // or a confirmed-empty head resolves the seated state, so any later idle
            // Chan==0 is a genuine clear rather than post-reboot amnesia — close the
            // cold-boot restore window. A stale FFMInfo.channel pointing at an empty
            // lane (#1065) does NOT resolve on its own — it isn't real seated truth.
            if ((ffm_channel_ > 0 && !ffm_lane_empty) || (chan > 0 && !chan_lane_empty) ||
                (chan == 0 && !head_filament_)) {
                seated_resolved_since_boot_ = true;
            }

            // Remember/forget the seated lane so a future power cycle can recover it.
            // seated_chan_ now holds the corroborated seated channel (live or
            // cold-boot-restored). On a confirmed unload (Chan=0 with the head
            // empty) drop the memory so a later boot doesn't resurrect a lane that
            // is no longer loaded. Writes are deduped so idle status frames don't
            // re-post the DB key every ~1.4 s.
            if (seated_chan_ > 0) {
                const int seated_slot0 = seated_chan_ - 1;
                if (persisted_seated_slot_ != std::optional<int>(seated_slot0)) {
                    persisted_seated_slot_ = seated_slot0;
                    persist_seated_slot_locked(seated_slot0);
                }
            } else if (((chan == 0 && !head_filament_) || head_empty_authoritative) &&
                       persisted_seated_slot_.has_value()) {
                // Forget the remembered lane on a confirmed-empty head — either a
                // clean idle Chan==0 clear, or the switch authoritatively empty
                // while a sticky FFMInfo.channel was just rejected (#1065 row 28) —
                // so a later boot doesn't resurrect a lane that is no longer loaded.
                persisted_seated_slot_.reset();
                persist_seated_slot_locked(-1);
            }

            log_seated_state_locked("apply_zcolor");

            // IFS_STATUS "Ports" is the RS-485 silk-sensor presence truth and is
            // the presence authority on native ZMOD. Apply it here — before the
            // prompt-fallback / slot-line paths — so presence is correct even
            // when GET_ZCOLOR SILENT degraded to a prompt this turn. Once seen,
            // it permanently retires the Adventurer5M.json ffmColor inference
            // (ifs_status_ports_seen_), which is what resurrected emptied
            // channels from their persisted colour after a SILENT demotion
            // (#981, bundle EE5L8LY2). Plugin users keep their own per-port
            // sensors as authority (has_per_port_sensors_), so skip them.
            if (result.ifs_ports.has_value() && !has_per_port_sensors_) {
                ifs_status_ports_seen_.store(true);
                const auto& ports = *result.ifs_ports;
                for (int i = 0; i < NUM_PORTS; ++i) {
                    const auto idx = static_cast<size_t>(i);
                    if (port_presence_[idx] == ports[idx]) {
                        continue;
                    }
                    // Eject-settling suppression (#1065): a false->true transition
                    // within EJECT_PRESENCE_SUPPRESSION of this lane's eject is the
                    // silk sensor still settling clear of the just-retracted lane,
                    // not a re-insert. Ignore it so the optimistic clear survives —
                    // the last-ejected lane otherwise stayed "present" (offering
                    // Unload) with no later query to re-correct it. true->false and
                    // any transition after the window still apply.
                    if (!port_presence_[idx] && ports[idx] &&
                        (std::chrono::steady_clock::now() - last_eject_time_[idx]) <
                            EJECT_PRESENCE_SUPPRESSION) {
                        spdlog::debug("{} Slot {} IFS_STATUS Ports reads present within eject "
                                      "settling window — ignoring as sensor lag (#1065)",
                                      backend_log_tag(), i);
                        continue;
                    }
                    const bool was_present = port_presence_[idx];
                    port_presence_[idx] = ports[idx];
                    chan_changed = true;
                    // present->absent: the lane went empty (eject / runout /
                    // unload). #1071: KEEP the lane->Spoolman override so a
                    // re-inserted same spool keeps its assignment — matching the
                    // AFC and Happy Hare backends, which never clear the link on
                    // empty. Only presence drops here; update_slot_from_state
                    // below recomputes status to EMPTY and re-applies the
                    // retained override (mirrors the GET_ZCOLOR path below).
                    if (was_present && !ports[idx]) {
                        spdlog::info("{} Slot {} went empty (IFS_STATUS Ports) — "
                                     "retaining the Spoolman link (#1071)",
                                     backend_log_tag(), i);
                    }
                    // absent->present: a spool was physically inserted. Unlock an
                    // auto-tracked (no-Spoolman) override so the new spool's
                    // firmware material/color refresh (#1065). update_slot_from_state
                    // below then re-runs the reconcile with the locks cleared.
                    if (!was_present && ports[idx]) {
                        unlock_auto_tracked_override_on_insert_locked(i);
                    }
                }
            }

            // ifs_chan takes precedence over extruder_slot for active_tool_
            // derivation (gated on !has_ifs_vars_, matching the extruder_slot
            // path below: lessWaste/bambufy own active_tool_ via save_variables).
            if (!has_ifs_vars_) {
                // Derive from the (guarded) seated channel, not the raw Chan: an
                // eject-engaged empty lane was rejected above, so active_tool_ and
                // current_slot must follow seated_chan_, not the stale reading.
                int new_active_tool =
                    (seated_chan_ > 0) ? find_first_tool_for_port(seated_chan_) : -1;
                if (active_tool_ != new_active_tool) {
                    active_tool_ = new_active_tool;
                    chan_changed = true;
                }
                // Recompute the seated slot immediately so the UI's
                // Unload/Eject gating updates without waiting for the next
                // status frame.
                int prev_slot = system_info_.current_slot;
                recompute_current_slot_locked();
                if (system_info_.current_slot != prev_slot) {
                    chan_changed = true;
                }
            }

            // Phase finalize from Chan: Chan==0 confirms an unload reached the
            // empty state; Chan>0 confirms a load seated a channel. Same
            // progressed-gate as the extruder_slot path so the early
            // post-dispatch query can't finalize before the op physically ran.
            if (phase_tracker_.active) {
                const bool progressed = phase_tracker_.is_unload ? phase_tracker_.seen_head_drop
                                                                 : phase_tracker_.seen_head_rise;
                const bool reached_end = phase_tracker_.is_unload ? (chan == 0) : (chan > 0);
                if (progressed && reached_end) {
                    spdlog::info("{} Phase: IFS_STATUS Chan={} confirms {} complete -> IDLE",
                                 backend_log_tag(), chan,
                                 phase_tracker_.is_unload ? "unload" : "load");
                    system_info_.action = AmsAction::IDLE;
                    if (phase_tracker_.is_unload) {
                        clear_head_loaded_after_unload_locked();
                    }
                    end_phase_tracking_locked();
                    set_operation_detail_locked("");
                    chan_changed = true;
                }
            }

            // Belt-and-suspenders head-loaded derivation (BUG-B). Run it HERE in
            // Phase 1 — after seated_chan_/port_presence_ were updated above but
            // before the slot-state refresh below — so a frame carrying BOTH the
            // IFS_STATUS Chan and the GET_ZCOLOR Extruder summary doesn't first
            // emit a slot update with a stale head_filament_ and only correct it
            // in Phase 2 (BUG-2, transient spurious LOADED to sync observers).
            // For a frame with no Extruder summary this is a no-op; the Phase 2
            // call covers GET_ZCOLOR-only frames.
            if (derive_head_loaded_from_summary_locked(result)) {
                chan_changed = true;
            }

            if (chan_changed) {
                for (int i = 0; i < NUM_PORTS; ++i) {
                    update_slot_from_state(i);
                }
            }
        } // release lock before emit_event (which also takes mutex_)

        if (chan_changed) {
            emit_event(EVENT_STATE_CHANGED);
        }
    }

    // Confirm SILENT works the first time it returns genuine GET_ZCOLOR content
    // (a summary or slot line, not just the IFS_STATUS JSON). After that, a
    // prompt is the user's own zmod colour menu colliding with our query, not
    // our query degrading — so it must not demote a capable device (#981).
    if (!result.is_prompt_fallback && result.saw_silent_content) {
        zcolor_silent_confirmed_.store(true);
    }

    if (result.is_prompt_fallback) {
        // Only treat the prompt as "GET_ZCOLOR SILENT unsupported" if we have
        // never seen SILENT actually work. A confirmed device keeps SILENT (and
        // its presence authority) instead of falling to the resurrection-prone
        // JSON-inference path (#981 false latch, bundle EE5L8LY2).
        if (!zcolor_silent_confirmed_.load() && zcolor_silent_supported_.exchange(false)) {
            spdlog::warn("{} zmod returned a prompt dialog for GET_ZCOLOR SILENT=1 — "
                         "old zmod, falling back to Adventurer5M.json polling",
                         backend_log_tag());
        }
        // Ports presence (if any) was already applied above (it rides the same
        // clean-JSON response). The prompt itself carries no slot lines, so
        // there is nothing more to process this turn.
        return;
    }

    if (!result.saw_valid_response) {
        // Response arrived but contained no summary or slot lines we recognise
        // (transient timing, malformed response, etc.). Don't wipe presence
        // on what might be incomplete data — wait for the next trigger.
        spdlog::debug("{} GET_ZCOLOR SILENT=1 yielded no recognisable content, "
                      "skipping apply",
                      backend_log_tag());
        return;
    }

    if (result.is_old_format) {
        spdlog::debug("{} GET_ZCOLOR SILENT=1 returned no /HEX segments "
                      "(pre-ad2802ab zmod) — presence only, colors from JSON",
                      backend_log_tag());
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < NUM_PORTS; ++i) {
            const auto idx = static_cast<size_t>(i);
            const auto& parsed = result.slots[idx];
            const bool loaded = parsed.has_value();

            // Live per-port sensors (lessWaste/bambufy) are authoritative for
            // presence — don't let zmod's view race against them. Native ZMOD
            // has no exposed per-port sensor, so we use zmod's view — unless this
            // same response carried IFS_STATUS Ports, which is the sensor-backed
            // authority and already set presence above; don't let the SILENT slot
            // lines fight it.
            // Also skip once IFS_STATUS Ports has ever been the presence authority
            // (ifs_status_ports_seen_): zmod's GET_ZCOLOR color latch survives an
            // eject, so on a Ports-capable device it would resurrect an emptied
            // lane from its retained color on a frame that happened to carry no
            // Ports reading (#1065, mirrors the JSON-poll guard below).
            if (!has_per_port_sensors_ && !result.ifs_ports.has_value() &&
                !ifs_status_ports_seen_.load() && port_presence_[idx] != loaded) {
                const bool was_present = port_presence_[idx];
                port_presence_[idx] = loaded;
                changed = true;

                // present->absent: the spool was physically removed. #1071:
                // KEEP the user override (brand/spool_name/spoolman_id/color/
                // material) so a re-inserted same spool keeps its assignment —
                // matching the AFC and Happy Hare backends, which never clear the
                // link on empty. The emptied slot still renders removed because
                // presence dropped above; update_slot_from_state below recomputes
                // status to EMPTY and re-applies the retained override.
                if (was_present && !loaded) {
                    spdlog::info("{} Slot {} went empty (GET_ZCOLOR present->absent) "
                                 "— retaining the Spoolman link (#1071)",
                                 backend_log_tag(), i);
                }
                // absent->present: physical insert on the GET_ZCOLOR-only path
                // (no IFS_STATUS Ports this frame). Same unlock as the Ports edge.
                if (!was_present && loaded) {
                    unlock_auto_tracked_override_on_insert_locked(i);
                }
            }

            // Fill in color/material if we got them and the slot isn't locally
            // dirty (unsaved user edit pending).
            if (!loaded || result.is_old_format || dirty_[idx]) {
                continue;
            }
            if (!parsed->hex.empty() && colors_[idx] != parsed->hex) {
                colors_[idx] = parsed->hex;
                changed = true;
            }
            if (!parsed->material.empty() && materials_[idx] != parsed->material) {
                materials_[idx] = parsed->material;
                changed = true;
            }
        }

        // Active tool: GET_ZCOLOR's "Extruder:" line is zmod's live view of
        // which slot is in the extruder. lessWaste/bambufy users get this
        // from save_variables (<prefix>_current_tool), so leave them alone.
        // Stock-ZMOD users have no other source — without this, active_tool_
        // is permanently stuck at -1 and the UI never shows which lane is
        // loaded (raza616's report against v0.99.50).
        //
        // IFS_STATUS "Chan" takes precedence: when ifs_chan is present it
        // already drove active_tool_/current_slot above (it persists at the
        // seated port, while "Extruder:" reads "None" loaded-idle), so skip the
        // extruder_slot derivation to avoid clobbering it with the stale view.
        if (!has_ifs_vars_ && !result.ifs_chan.has_value()) {
            int new_active_tool = -1;
            if (result.extruder_slot.has_value()) {
                int port = *result.extruder_slot + 1;
                new_active_tool = find_first_tool_for_port(port);
            }
            if (active_tool_ != new_active_tool) {
                active_tool_ = new_active_tool;
                recompute_current_slot_locked();
                changed = true;
            }
        }

        // Phase-tracker quick-finish: GET_ZCOLOR's extruder_slot is zmod's
        // fork-independent view of the toolhead — ABSENT after a successful
        // unload, SET after a successful load. When a tracked op has physically
        // progressed past the cut/feed (a head transition was seen) and this
        // query confirms the end state, finalize to IDLE immediately rather than
        // waiting for the 90s timeout backstop. The progressed-gate prevents the
        // early post-dispatch query (unload_filament schedules one immediately)
        // from finalizing before the op has even run. (When ifs_chan is present
        // the finalize already ran above; phase_tracker_.active is now false so
        // this is a no-op — guarded anyway for clarity.)
        if (phase_tracker_.active && result.saw_valid_response && !result.ifs_chan.has_value()) {
            const bool progressed = phase_tracker_.is_unload ? phase_tracker_.seen_head_drop
                                                             : phase_tracker_.seen_head_rise;
            const bool reached_end = phase_tracker_.is_unload ? !result.extruder_slot.has_value()
                                                              : result.extruder_slot.has_value();
            if (progressed && reached_end) {
                spdlog::info("{} Phase: zcolor confirms {} complete -> IDLE", backend_log_tag(),
                             phase_tracker_.is_unload ? "unload" : "load");
                system_info_.action = AmsAction::IDLE;
                if (phase_tracker_.is_unload) {
                    clear_head_loaded_after_unload_locked();
                }
                end_phase_tracking_locked();
                set_operation_detail_locked("");
                changed = true;
            }
        }

        // Head-loaded state from GET_ZCOLOR's "Extruder:" summary (BUG-B). For
        // frames that also carried IFS_STATUS Chan this already ran in Phase 1
        // (so it is a no-op here); this call covers GET_ZCOLOR-only frames. The
        // C1/C2 presence corroboration lives in the helper.
        if (derive_head_loaded_from_summary_locked(result)) {
            changed = true;
        }

        if (changed) {
            for (int i = 0; i < NUM_PORTS; ++i) {
                update_slot_from_state(i);
            }
        }
    } // release lock before emit_event (which also takes mutex_)

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendAd5xIfs::clear_head_loaded_after_unload_locked() {
    if (head_filament_) {
        head_filament_ = false;
        system_info_.filament_loaded = false;
    }
}

bool AmsBackendAd5xIfs::derive_head_loaded_from_summary_locked(const ZColorSilentResult& result) {
    // Native Z-Mod's head switch/motion sensors don't reliably push under the
    // stock filament_*_sensor sections, so head_filament_ can be unwritten even
    // with filament at the toolhead (BUG-B, #1065). GET_ZCOLOR's "Extruder: N"
    // summary is zmod's live at-extruder view and the belt-and-suspenders head
    // signal. Gated on saw_extruder_summary so a frame without the summary line
    // (IFS_STATUS-only, or slot-lines-only) can't act on a coincidentally-absent
    // extruder_slot.
    if (!result.saw_extruder_summary) {
        return false;
    }

    if (result.extruder_slot.has_value()) {
        // "Extruder: N" — positive, authoritative evidence of filament at the
        // extruder. Assert loaded. This also self-heals a transient clobber from
        // the motion sensor, which (device-confirmed 2026-06-28) reads
        // filament_detected=false while idle even with filament loaded — see the
        // parse_head_sensor() conflation NOTE in the header.
        if (!head_filament_) {
            head_filament_ = true;
            system_info_.filament_loaded = true;
            return true;
        }
        return false;
    }

    // "Extruder: None" is AMBIGUOUS: it appears both after a genuine unload AND,
    // on some firmware, while filament is still seated at the head post-runout/
    // print-end (#995 — the case can_unload_from_toolhead's `head_filament_ &&
    // current_slot<0` fallback exists for). Only clear an established loaded
    // state when physical presence corroborates an empty head — i.e. the seated
    // lane's port has gone absent. Keeping it loaded while the lane is still
    // present avoids stranding unremovable filament and wrongly re-enabling Load
    // (-> cold grind), and prevents an Extruder:None frame from clobbering a real
    // head switch sensor on lessWaste/older variants whose seated lane still
    // reads present (C1/C2). A genuine unload/eject drops the lane's port, which
    // re-permits the clear; the real head switch sensor (parse_head_sensor) is
    // the primary clear path and is unaffected.
    if (!head_filament_) {
        return false;
    }
    const bool seated_port_present = seated_chan_ > 0 && seated_chan_ <= NUM_PORTS &&
                                     port_presence_[static_cast<size_t>(seated_chan_ - 1)];
    if (seated_port_present) {
        spdlog::debug("{} Extruder:None with seated lane {} still present — keeping "
                      "head-loaded (post-runout/seated; deferring to the head sensor) (#995)",
                      backend_log_tag(), seated_chan_);
        return false;
    }
    head_filament_ = false;
    system_info_.filament_loaded = false;
    return true;
}

AmsBackendAd5xIfs::ZColorSilentResult
AmsBackendAd5xIfs::parse_zcolor_silent(const std::vector<std::string>& lines, const char* reason) {
    ZColorSilentResult result;

    // Regexes compiled once per call; parsing is off the hot path.
    // Summary: "// Extruder: None (N) | IFS: True"
    //   or:    "// Extruder: N: MATERIAL/HEX | IFS: True"
    static const std::regex summary_re(R"(^//\s*Extruder:\s*(.+?)\s*\|\s*IFS:\s*(True|False)\s*$)");
    // Slot: "// N: MATERIAL/HEX" or "// N: MATERIAL/NAME/HEX" or old "// N: MATERIAL"
    static const std::regex slot_re(R"(^//\s*([1-9])\s*:\s*(.+?)\s*$)");
    // Extruder detail inside summary text: "N: MATERIAL/..."
    static const std::regex extruder_slot_re(R"(^([1-9])\s*:)");
    // current channel: "None (N)" or bare "N" form — look for "(N)" paren form
    static const std::regex channel_paren_re(R"(\((\d+)\))");

    // First pass: classify the response and pull the IFS_STATUS JSON line.
    //
    // IFS_STATUS (zmod cmd_IFS_STATUS -> respond_info(json.dumps(get_values())))
    // arrives as one `// `-prefixed clean-JSON object containing "Chan" (1-based
    // seated port, 0 = none). It shares the response buffer with GET_ZCOLOR but
    // is emitted via respond_info, NOT a prompt dialog — so on old zmod where
    // GET_ZCOLOR degrades to an action:prompt_ dialog, the IFS_STATUS data is
    // still present and must NOT be discarded by the prompt early-out. We scan
    // every line here (no early return) and let apply_zcolor_result honor
    // ifs_chan even when is_prompt_fallback is set.
    for (const auto& raw : lines) {
        if (raw.find("action:prompt_") != std::string::npos) {
            result.is_prompt_fallback = true;
            continue;
        }
        if (result.ifs_chan.has_value() || raw.find("\"Chan\"") == std::string::npos) {
            continue;
        }
        // Strip the gcode-console "// " prefix before JSON parsing.
        std::string body = raw;
        const size_t slashes = body.find("//");
        if (slashes != std::string::npos) {
            body = body.substr(slashes + 2);
        }
        try {
            json obj = json::parse(body);
            if (obj.is_object()) {
                auto chan_it = obj.find("Chan");
                if (chan_it != obj.end() && chan_it->is_number_integer()) {
                    result.ifs_chan = chan_it->get<int>();
                    result.saw_valid_response = true;
                    // Diagnostic: log the seated channel + presence view so the
                    // next field bundle proves Chan's loaded-idle behavior.
                    // safe_int, not .value(): the catch below is parse_error
                    // only, so a line like {"Chan":1,"State":null} would throw
                    // type_error.302 straight past it — breaking the promise
                    // that comment makes. Every other read in this block is
                    // already find + is_*() guarded; this was the lone gap.
                    int state = helix::json_util::safe_int(obj, "State", -1);
                    std::string ports_str;
                    if (auto ports_it = obj.find("Ports"); ports_it != obj.end() &&
                                                           ports_it->is_array() &&
                                                           ports_it->size() == NUM_PORTS) {
                        ports_str = ports_it->dump();
                        // Capture the per-port presence as the sensor-backed
                        // authority (not just a diagnostic). Each entry is the
                        // RS-485 silk switch for port i+1.
                        std::array<bool, NUM_PORTS> ports{};
                        bool ok = true;
                        for (int p = 0; p < NUM_PORTS; ++p) {
                            const auto& v = (*ports_it)[static_cast<size_t>(p)];
                            if (!v.is_boolean()) {
                                ok = false;
                                break;
                            }
                            ports[static_cast<size_t>(p)] = v.get<bool>();
                        }
                        if (ok) {
                            result.ifs_ports = ports;
                        }
                    }
                    spdlog::info("[AMS AD5X-IFS] IFS_STATUS trigger={} Chan={} State={} Ports={}",
                                 reason, *result.ifs_chan, state, ports_str);
                }
            }
        } catch (const json::parse_error&) {
            // Not valid JSON (malformed line, partial buffer) — ignore. Never
            // throw on a bad gcode-response line.
        }
    }

    int slot_lines_seen = 0;
    int slot_lines_with_hex = 0;
    std::smatch m;

    for (const auto& line : lines) {
        if (std::regex_match(line, m, summary_re)) {
            result.saw_valid_response = true;
            result.saw_silent_content = true;   // genuine GET_ZCOLOR summary line
            result.saw_extruder_summary = true; // carries the "Extruder:" head field
            result.ifs_active = (m[2].str() == "True");
            const std::string extruder_part = m[1].str();

            std::smatch em;
            if (std::regex_search(extruder_part, em, extruder_slot_re)) {
                try {
                    int n = std::stoi(em[1].str());
                    if (n >= 1 && n <= NUM_PORTS) {
                        result.extruder_slot = n - 1; // 0-based
                    }
                } catch (...) {
                }
            }
            std::smatch cm;
            if (std::regex_search(extruder_part, cm, channel_paren_re)) {
                try {
                    result.current_channel = std::stoi(cm[1].str());
                } catch (...) {
                }
            }
            continue;
        }

        if (std::regex_match(line, m, slot_re)) {
            int n;
            try {
                n = std::stoi(m[1].str());
            } catch (...) {
                continue;
            }
            if (n < 1 || n > NUM_PORTS) {
                continue; // slot number out of range — skip (e.g. "// 99: nonsense")
            }

            result.saw_valid_response = true;
            result.saw_silent_content = true; // genuine GET_ZCOLOR slot line
            slot_lines_seen++;

            // Slot line body is "MATERIAL" or "MATERIAL/HEX" or "MATERIAL/NAME/HEX".
            // Rule: material is everything before the first '/', hex is everything
            // after the LAST '/'. Anything between is a COLOR_MAPPING alias we ignore.
            const std::string rest = m[2].str();
            const size_t first_slash = rest.find('/');
            const size_t last_slash = rest.rfind('/');

            ZColorSlot slot;
            if (first_slash == std::string::npos) {
                // Old format: just material, no /HEX.
                slot.material = rest;
            } else {
                slot.material = rest.substr(0, first_slash);
                slot.hex = rest.substr(last_slash + 1);
                slot_lines_with_hex++;
            }
            result.slots[static_cast<size_t>(n - 1)] = std::move(slot);
        }
    }

    // Old format detection: we saw slot lines but none had a /HEX segment.
    if (slot_lines_seen > 0 && slot_lines_with_hex == 0) {
        result.is_old_format = true;
    }

    return result;
}

void AmsBackendAd5xIfs::parse_adventurer_json(const std::string& content) {
    json doc;
    try {
        doc = json::parse(content);
    } catch (const json::parse_error& e) {
        spdlog::warn("{} Failed to parse Adventurer5M.json: {}", backend_log_tag(), e.what());
        return;
    }

    auto ffm_it = doc.find("FFMInfo");
    if (ffm_it == doc.end() || !ffm_it->is_object()) {
        spdlog::debug("{} No FFMInfo section in Adventurer5M.json", backend_log_tag());
        return;
    }
    const auto& ffm = *ffm_it;

    int parsed_count = 0;
    bool needs_ifs_vars_push = false;
    std::string ifs_colors_payload;
    std::string ifs_types_payload;
    std::string ifs_var_prefix_snapshot;
    std::string signature;
    bool slots_changed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t pre_sync_count = external_sync_count_;

        // Ports 1-4 in JSON map to slots 0-3
        for (int port = 1; port <= NUM_PORTS; ++port) {
            std::string color_key = "ffmColor" + std::to_string(port);
            std::string type_key = "ffmType" + std::to_string(port);

            auto color_it = ffm.find(color_key);
            auto type_it = ffm.find(type_key);
            if (color_it == ffm.end() && type_it == ffm.end())
                continue;

            // Extract color — strip '#' prefix, default to gray if empty
            std::string hex;
            if (color_it != ffm.end() && color_it->is_string()) {
                hex = color_it->get<std::string>();
            }
            if (!hex.empty() && hex[0] == '#') {
                hex = hex.substr(1);
            }
            // Non-empty color means filament was loaded into this port (only used
            // as a presence signal on pre-SILENT zmod — see the gated block below).
            bool has_filament_data = !hex.empty();
            if (hex.empty()) {
                hex = "808080";
            }
            // Uppercase
            for (auto& c : hex)
                c = static_cast<char>(toupper(c));

            // Extract material type
            std::string type;
            if (type_it != ffm.end() && type_it->is_string()) {
                type = type_it->get<std::string>();
            }
            // Firmware-native unset sentinel: live stock ZMOD writes ffmType='?'
            // for an entry with no assigned material. Map it to empty so the UI
            // renders '--' rather than a literal '?'.
            if (type == "?")
                type.clear();

            int idx = port - 1;
            if (dirty_[static_cast<size_t>(idx)])
                continue;
            // Once the live RS-485 authority is established (a GET_ZCOLOR SILENT
            // response actually confirmed, or IFS_STATUS Ports seen), zmod's
            // persisted ffmColor/ffmType are a stale cache it never blanks on
            // eject/swap. For a slot the live source owns (present), let GET_ZCOLOR
            // be the colour/material authority; the JSON cache otherwise resurrected
            // an old type/colour between live queries (raza616 "menu shows
            // silk/orange after PLA swap into the same slot", 5HR3HHS6). Pre-SILENT /
            // JSON-only zmod keeps seeding from JSON (no live source competes).
            //
            // NOTE: gates on zcolor_silent_confirmed_ (latched only after a genuine
            // SILENT reply), NOT the optimistic-by-default zcolor_silent_supported_
            // — the latter is true before any query has run, which would wrongly
            // suppress the JSON seed on a fresh backend that has no live truth yet.
            const bool live_owns =
                (zcolor_silent_confirmed_.load() || ifs_status_ports_seen_.load()) &&
                port_presence_[static_cast<size_t>(idx)];
            if (!live_owns) {
                colors_[static_cast<size_t>(idx)] = hex;
                materials_[static_cast<size_t>(idx)] = type;
            }

            // Build a per-slot signature (count + color + material) so the
            // "Loaded N slots" line below logs at INFO only when the parsed
            // set actually changed since the last read. Unchanged ~5s polls
            // are demoted to TRACE to keep the log tail clean.
            signature.append(std::to_string(idx));
            signature.push_back(':');
            signature.append(hex);
            signature.push_back('/');
            signature.append(type);
            signature.push_back(';');

            // Presence ownership depends on whether GET_ZCOLOR SILENT=1 works:
            //
            //   * Modern zmod (SILENT supported): presence is owned SOLELY by
            //     GET_ZCOLOR (apply_zcolor_result, the RS-485 silk sensor). We must
            //     NOT infer it from ffmColorN here — zmod persists the
            //     colour/material assignment across unload/eject and never blanks
            //     the colour, so "non-empty colour == present" would resurrect a
            //     previously-emptied channel on every content-changed poll (the
            //     external-unload-not-reflected / one-edit-resurrects-another bug).
            //     Every JSON change schedules a GET_ZCOLOR right after this parse,
            //     so silk-truth presence re-establishes immediately. A
            //     present->absent transition there only drops presence; the
            //     lane->Spoolman override is retained across empty (#1071).
            //
            //   * Pre-SILENT zmod (GET_ZCOLOR returns a prompt dialog →
            //     zcolor_silent_supported_ latched false): there is NO silk-truth
            //     query at all, so Adventurer5M.json is the only presence source we
            //     have. Fall back to the legacy inference (non-empty colour ==
            //     present; empty colour while IDLE == eject — presence drops, the
            //     override is retained, #1071). The resurrection bug can't bite
            //     here because no GET_ZCOLOR ever competes for ownership.
            //   * IFS_STATUS Ports seen (ifs_status_ports_seen_): the RS-485
            //     silk sensors are the presence authority and already own
            //     port_presence_. The persisted ffmColor must NEVER override
            //     them — that is the resurrection (#981, bundle EE5L8LY2): a
            //     SILENT demotion would otherwise let a stale colour repopulate
            //     an emptied channel. Refresh colours/materials only (above);
            //     never touch presence here.
            if (!has_per_port_sensors_ && !zcolor_silent_supported_.load() &&
                !ifs_status_ports_seen_.load()) {
                auto& presence = port_presence_[static_cast<size_t>(idx)];
                if (has_filament_data) {
                    presence = true;
                } else if (presence && system_info_.action == AmsAction::IDLE) {
                    // present->absent eject (pre-SILENT zmod, JSON-inferred).
                    // #1071: KEEP the lane->Spoolman override so a re-inserted
                    // same spool keeps its assignment — matching the AFC and
                    // Happy Hare backends, which never clear the link on empty.
                    // Only presence drops; update_slot_from_state below recomputes
                    // status to EMPTY and re-applies the retained override.
                    spdlog::info("{} Slot {} eject detected (empty color in "
                                 "Adventurer5M.json, pre-SILENT zmod) — retaining "
                                 "the Spoolman link (#1071)",
                                 backend_log_tag(), idx);
                    presence = false;
                    needs_ifs_vars_push = true;
                }
            }

            update_slot_from_state(idx);
            ++parsed_count;
        }

        // FFMInfo.channel is the firmware's own record of the seated toolhead lane
        // (1-based; 0 = none). It is the seated-lane authority (#1065 Bug 3) — see
        // the override in apply_zcolor_result, where it beats a dialog-tracked
        // IFS_STATUS Chan. Read it here so a file poll updates the seated lane
        // promptly (and remembers it for the post-reboot floor) without waiting for
        // the next IFS_STATUS frame.
        auto chan_it = ffm.find("channel");
        if (chan_it != ffm.end() && chan_it->is_number_integer()) {
            const int prev_current_slot = system_info_.current_slot;
            const int fw_chan = chan_it->get<int>();
            ffm_channel_ = (fw_chan >= 1 && fw_chan <= NUM_PORTS) ? fw_chan : 0;
            // Head-gate (#1065 row 28): the file's FFMInfo.channel is sticky and
            // survives an eject, so a plain poll re-reads a stale channel. Adopt it
            // as seated ONLY when the toolhead SWITCH sensor corroborates filament
            // at the head; when the switch authoritatively reads empty, treat the
            // channel as stale and clear the seated lane instead. Motion-only
            // firmware (head_switch_seen_ == false) falls back to adopting it, since
            // the motion sensor false-negatives while loaded-idle.
            const bool head_empty_authoritative = head_switch_seen_ && !head_switch_present_;
            if (ffm_channel_ > 0 && head_empty_authoritative) {
                if (seated_chan_ != 0) {
                    spdlog::info("{} FFMInfo.channel={} rejected as seated (toolhead switch empty, "
                                 "#1065 row 28); clearing seated_chan_ (was {})",
                                 backend_log_tag(), ffm_channel_, seated_chan_);
                    seated_chan_ = 0;
                    seated_resolved_since_boot_ = true;
                    recompute_current_slot_locked();
                    if (persisted_seated_slot_.has_value()) {
                        persisted_seated_slot_.reset();
                        persist_seated_slot_locked(-1);
                    }
                }
                log_seated_state_locked("adventurer_json");
            } else if (ffm_channel_ > 0 && seated_chan_ != ffm_channel_) {
                spdlog::info("{} Seated lane from FFMInfo.channel: slot {} (was seated_chan_={})",
                             backend_log_tag(), ffm_channel_ - 1, seated_chan_);
                seated_chan_ = ffm_channel_;
                seated_resolved_since_boot_ = true;
                recompute_current_slot_locked();
                const int seated_slot0 = ffm_channel_ - 1;
                if (persisted_seated_slot_ != std::optional<int>(seated_slot0)) {
                    persisted_seated_slot_ = seated_slot0;
                    persist_seated_slot_locked(seated_slot0);
                }
                log_seated_state_locked("adventurer_json");
            }

            // The per-slot loop above ran BEFORE this block moved the seated
            // lane, so every slot's LOADED stamp is now keyed on the old
            // current_slot. Re-derive them here rather than waiting for the next
            // status frame: the stamp is what slot_is_actively_loaded() reads,
            // and a stale one paints the highlight on the lane we just left.
            if (system_info_.current_slot != prev_current_slot) {
                for (int i = 0; i < NUM_PORTS; ++i) {
                    update_slot_from_state(i);
                }
            }
        }

        // sync_override_to_firmware_locked (called via update_slot_from_state →
        // check_external_color_change) bumps external_sync_count_ on every
        // accepted external edit. If anything synced or anything ejected,
        // mirror the new colors_/materials_ snapshot into the
        // lessWaste/bambufy plugin's _IFS_VARS save_variables — those don't
        // self-sync against zmod's truth (audited 2026-05-04: neither
        // Hrybmo/lesswaste nor function3d/bambufy listen for CHANGE_ZCOLOR).
        // Without this push, the plugin's runout-recovery alternate-port
        // lookup, smart-purge skip decision, and color-assign dialog all run
        // against stale data and silently print the wrong color or skip the
        // wrong purge.
        needs_ifs_vars_push = needs_ifs_vars_push || (external_sync_count_ > pre_sync_count);
        if (needs_ifs_vars_push && has_ifs_vars_) {
            ifs_colors_payload = build_color_list_value();
            ifs_types_payload = build_type_list_value();
            ifs_var_prefix_snapshot = var_prefix_;
        }

        // Change-detection (#981): demote the "Loaded N slots" line below to
        // trace when the parsed set is identical to the previous poll, so the
        // ~5s polling loop doesn't spam the log.
        slots_changed = (signature != last_parsed_signature_);
        last_parsed_signature_ = signature;
    } // release lock before emit_event + write_ifs_var (both take mutex_)

    if (needs_ifs_vars_push && has_ifs_vars_) {
        // Suppress the noisy `// Colors : [...]` / `// Types : [...]` echo on
        // bambufy (bambufy's _IFS_VARS macro accepts SHOW=0 to skip the
        // RESPOND line; lessWaste does not). Echo on lessWaste is debounced
        // by kJsonPollInterval (5s).
        const std::string suffix = (ifs_var_prefix_snapshot == "bambufy") ? " SHOW=0" : "";
        auto colors_err = execute_gcode("_IFS_VARS colors=" + ifs_colors_payload + suffix);
        if (!colors_err.success()) {
            spdlog::warn("{} _IFS_VARS colors mirror failed: {}", backend_log_tag(),
                         colors_err.technical_msg);
        }
        auto types_err = execute_gcode("_IFS_VARS types=" + ifs_types_payload + suffix);
        if (!types_err.success()) {
            spdlog::warn("{} _IFS_VARS types mirror failed: {}", backend_log_tag(),
                         types_err.technical_msg);
        }
    }

    if (parsed_count > 0) {
        if (slots_changed) {
            spdlog::info("{} Loaded {} slots from Adventurer5M.json (native ZMOD)",
                         backend_log_tag(), parsed_count);
        } else {
            spdlog::trace("{} Loaded {} slots from Adventurer5M.json (native ZMOD, unchanged)",
                          backend_log_tag(), parsed_count);
        }
        emit_event(EVENT_STATE_CHANGED);
    } else {
        spdlog::debug("{} No slot data found in Adventurer5M.json", backend_log_tag());
    }
}

// === Live load/unload progress phase tracker ===

void AmsBackendAd5xIfs::begin_phase_tracking_locked(bool is_unload) {
    phase_tracker_ = IfsPhaseTracker{};
    phase_tracker_.active = true;
    phase_tracker_.is_unload = is_unload;
    // Seed the indeterminate detector: op just started, so the no-progress clock
    // starts now and the last-progress temp baseline is cleared so the first real
    // extruder frame counts as a value change (#1065 row 14).
    last_progress_temp_deci_ = 0;
    note_phase_progress_locked();
    // Seed the heat target from the last-known extruder target if we have one;
    // a RESPOND "Heating the nozzle to N degrees" line or an extruder frame
    // refines it later. With no signal the target stays 0 ("unknown") and the
    // detail string simply omits it - guessing a number here would have the UI
    // assert a target the printer never had.
    if (last_extruder_target_deci_ > 0) {
        phase_tracker_.target_deci = last_extruder_target_deci_;
    }
}

void AmsBackendAd5xIfs::end_phase_tracking_locked() {
    phase_tracker_ = IfsPhaseTracker{};
    // Clear the step-tracker index so the right-side vertical tracker shows no
    // active step once the operation finalizes back to IDLE.
    system_info_.operation_phase = -1;
}

void AmsBackendAd5xIfs::on_extruder_temp_locked(int temp_deci, int target_deci) {
    if (!phase_tracker_.active) {
        return;
    }
    // A CHANGED extruder temperature is a genuine progress signal — reset the
    // indeterminate ("Working…") clock. Gating on a value change (not merely on a
    // frame arriving) is what lets a frozen temp subject trip the detector: when
    // the feed starves the value stops changing and the clock elapses past the
    // threshold (#1065 row 14). Healthy heating pushes ~1-4 changing frames/sec,
    // so a normal heat keeps resetting and never trips.
    if (temp_deci != last_progress_temp_deci_) {
        last_progress_temp_deci_ = temp_deci;
        note_phase_progress_locked();
    }
    // Track the live target if the firmware reports a positive one.
    if (target_deci > 0) {
        phase_tracker_.target_deci = target_deci;
    }
    // HEATING completes when current temp reaches within ~0.5°C of target.
    // (CFS uses 5°C/50 deci-degrees; IFS heats from cold so we use a tighter
    // 0.5°C window — the macro doesn't start cutting until it's truly at temp.)
    const int tgt = phase_tracker_.target_deci;
    if (tgt > 0 && temp_deci >= (tgt - 5 /* 0.5°C in deci-degrees */)) {
        if (!phase_tracker_.reached_target_once) {
            phase_tracker_.reached_target_once = true;
            spdlog::info("{} Phase: reached target {}°C", backend_log_tag(),
                         helix::ui::temperature::deci_to_degrees(tgt));
        }
    }
    apply_phase_action_locked();
}

void AmsBackendAd5xIfs::on_head_transition_locked(bool detected) {
    if (!phase_tracker_.active) {
        return;
    }
    // A head-sensor transition (cut/retract begun or filament reached nozzle) is
    // a genuine progress signal for the indeterminate detector (#1065 row 14).
    note_phase_progress_locked();
    if (!detected) {
        // Head sensor cleared: cut + retract underway (unload) — advance to the
        // retract phase. The toolhead unload (_IFS_REMOVE_CURRENT_PRUTOK) runs
        // with BYPASS_TEMPERATURE_CHECK and sends no preheat, so the nozzle may
        // never reach target and the heat-completion gate would never trip,
        // pinning the op in HEATING until the 300s timeout. A physical head drop
        // proves the cut/retract has begun, so treat it as heat-complete and let
        // apply_phase_action_locked advance HEATING -> UNLOADING.
        phase_tracker_.seen_head_drop = true;
        phase_tracker_.reached_target_once = true;
        // Swap-aware LOADING clock reset (#1065 v0.99.94, bundle NJB2U558):
        // INSERT_PRUTOK_IFS with another lane currently seated runs an IMPLICIT
        // UNLOAD before the actual load. The implicit-unload head drop is real
        // progress for a LOAD op — the swap has begun, the new lane's feed
        // hasn't started yet. Without resetting here, the LOADING budget
        // (90s default, 180s with swap_expected) starts counting at heat-
        // complete and times out mid-swap, surfacing a false "Loading error,
        // feeding filament to nozzle (timed out)" popup. apply_phase_action_locked
        // only resets on a phase TRANSITION, but for a LOAD op the head drop
        // doesn't transition the phase (still LOADING) — so we reset here.
        // (For an UNLOAD op the head drop DOES transition CUTTING → UNLOADING,
        // so apply_phase_action_locked's reset covers it and this is a no-op.)
        if (!phase_tracker_.is_unload) {
            action_start_time_ = std::chrono::steady_clock::now();
        }
        spdlog::info("{} Phase: head sensor dropped (cut/retract started{}"
                     ")",
                     backend_log_tag(), phase_tracker_.is_unload ? "" : " — LOAD swap clock reset");
    } else {
        // Head sensor tripped: filament reached the nozzle (load) — advance to
        // the purge phase.
        phase_tracker_.seen_head_rise = true;
        spdlog::info("{} Phase: head sensor rose (filament at nozzle)", backend_log_tag());
    }
    apply_phase_action_locked();
    // Fire a confirming GET_ZCOLOR ~now: the macro finishes shortly after the
    // cut/feed, and apply_zcolor_result's extruder_slot view is the early
    // terminal signal that lets us finalize to IDLE within ~1s instead of
    // waiting out the 90s timeout backstop. Safe under mutex_ —
    // schedule_zcolor_query only touches atomics + HttpExecutor (no re-lock).
    schedule_zcolor_query("phase_head_transition");
}

bool AmsBackendAd5xIfs::apply_phase_action_locked() {
    if (!phase_tracker_.active) {
        return false;
    }

    AmsAction synth;
    std::string detail;
    // Step index for the right-side vertical operation tracker. Mirrors the
    // phase_id values get_operation_step_model() emits: unload
    // HEATING→0 / CUTTING→1 / UNLOADING→2 ; load HEATING→0 / LOADING→1 / PURGING→2.
    // AmsState::sync_from_backend() copies this into the ams_operation_phase
    // subject the tracker observes.
    int phase_index;
    const int tgt = phase_tracker_.target_deci;

    if (phase_tracker_.is_unload) {
        // HEATING → CUTTING → UNLOADING
        if (!phase_tracker_.reached_target_once) {
            synth = AmsAction::HEATING;
            phase_index = 0;
        } else if (!phase_tracker_.seen_head_drop) {
            synth = AmsAction::CUTTING;
            phase_index = 1;
        } else {
            synth = AmsAction::UNLOADING;
            phase_index = 2;
        }
    } else {
        // HEATING → LOADING → PURGING
        if (!phase_tracker_.reached_target_once) {
            synth = AmsAction::HEATING;
            phase_index = 0;
        } else if (!phase_tracker_.seen_head_rise) {
            synth = AmsAction::LOADING;
            phase_index = 1;
        } else {
            synth = AmsAction::PURGING;
            phase_index = 2;
        }
    }
    system_info_.operation_phase = phase_index;

    // Build the per-phase operation_detail. Dynamic (contains live temps), so it
    // is NOT run through lv_tr() — matches recompute_action_detail's priority-1
    // backend-supplied-string behavior in ams_state.cpp.
    char buf[64];
    switch (synth) {
    case AmsAction::HEATING:
        // Both the target and the current temp are independently optional: the
        // target is 0 until an extruder frame or a RESPOND line reports one, and
        // the current temp is 0 until the first extruder frame. Name only what
        // is actually known rather than printing a placeholder number.
        if (tgt > 0 && last_extruder_temp_deci_ > 0) {
            std::snprintf(buf, sizeof(buf), "Heating nozzle to %d°C (%d°C)",
                          helix::ui::temperature::deci_to_degrees(tgt),
                          helix::ui::temperature::deci_to_degrees(last_extruder_temp_deci_));
            detail = buf;
        } else if (tgt > 0) {
            std::snprintf(buf, sizeof(buf), "Heating nozzle to %d°C",
                          helix::ui::temperature::deci_to_degrees(tgt));
            detail = buf;
        } else if (last_extruder_temp_deci_ > 0) {
            std::snprintf(buf, sizeof(buf), "Heating nozzle (%d°C)",
                          helix::ui::temperature::deci_to_degrees(last_extruder_temp_deci_));
            detail = buf;
        } else {
            detail = "Heating nozzle";
        }
        break;
    case AmsAction::CUTTING:
        detail = "Cutting filament";
        break;
    case AmsAction::UNLOADING:
        detail = "Retracting filament from nozzle";
        break;
    case AmsAction::LOADING:
        detail = "Feeding filament to nozzle";
        break;
    case AmsAction::PURGING:
        detail = "Purging old filament";
        break;
    default:
        break;
    }

    bool changed = false;
    if (system_info_.action != synth) {
        spdlog::info("{} Phase synth: {} -> {}", backend_log_tag(),
                     ams_action_to_string(system_info_.action), ams_action_to_string(synth));
        system_info_.action = synth;
        // Reset the timeout clock on every phase transition so each post-heating
        // phase (CUTTING/UNLOADING/LOADING/PURGING) gets its own fresh 90s window
        // rather than inheriting elapsed time from the long heat-up.
        action_start_time_ = std::chrono::steady_clock::now();
        // A phase transition is a genuine progress signal for the indeterminate
        // detector (#1065 row 14).
        note_phase_progress_locked();
        changed = true;
    }
    set_operation_detail_locked(std::move(detail));
    return changed;
}

void AmsBackendAd5xIfs::note_phase_progress_locked() {
    last_phase_progress_time_ = std::chrono::steady_clock::now();
}

void AmsBackendAd5xIfs::set_operation_detail_locked(std::string detail) {
    system_info_.operation_detail = std::move(detail);
}

void AmsBackendAd5xIfs::detect_load_unload_completion(bool head_detected) {
    // Phase-tracker-driven operations (load/unload WE started) advance phases
    // via on_head_transition_locked rather than completing here — the head
    // drop/rise is an intermediate signal (CUTTING→UNLOADING / LOADING→PURGING),
    // not completion. Finalization happens via the action-timeout backstop +
    // schedule_zcolor_query reconciliation. Skip the legacy snap so the phase
    // sequence isn't short-circuited.
    if (phase_tracker_.active) {
        return;
    }

    // Legacy / external / firmware-initiated path: a head transition completes
    // the operation directly. Preserved exactly for backward compatibility.
    if (system_info_.action == AmsAction::LOADING && head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Load complete (head sensor triggered)", backend_log_tag());
        PostOpCooldownManager::instance().schedule();
        schedule_zcolor_query("load_complete_head");
    } else if (system_info_.action == AmsAction::UNLOADING && !head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Unload complete (head sensor cleared)", backend_log_tag());
        PostOpCooldownManager::instance().schedule();
        schedule_zcolor_query("unload_complete_head");
    }
}

// === Unattended runout detection (#1250, reported as #1247) ===

void AmsBackendAd5xIfs::note_head_switch_reading_locked(bool detected) {
    // Reads the PREVIOUS latch values; handle_status_update calls this before it
    // overwrites head_switch_seen_ / head_switch_present_.
    const bool was_seen = head_switch_seen_;
    const bool was_present = head_switch_present_;

    if (detected) {
        head_empty_since_.reset();
        if (runout_active_) {
            clear_runout_locked("filament detected at the toolhead again");
        }
        return;
    }

    // Arm on a genuine present->absent EDGE only, and only when nothing is in
    // flight. A level test would invent a runout on a printer that booted into a
    // paused job with an empty toolhead, and arming during an operation would
    // attribute that operation's own head drop (a cut, a swap's implicit unload)
    // to a runout.
    if (!was_seen || !was_present) {
        return;
    }
    if (phase_tracker_.active || system_info_.action != AmsAction::IDLE) {
        return;
    }
    head_empty_since_ = std::chrono::steady_clock::now();
    spdlog::debug("{} Toolhead switch cleared while idle - runout candidate armed",
                  backend_log_tag());
}

void AmsBackendAd5xIfs::note_filament_op_dispatch_locked() {
    last_filament_op_dispatch_ = std::chrono::steady_clock::now();
    // Any candidate armed before this dispatch is now unattributable - the
    // operation about to run is a perfectly good explanation for an empty head.
    head_empty_since_.reset();
}

std::chrono::seconds AmsBackendAd5xIfs::runout_confirm_delay_locked() const {
    // Any of the three switchover paths — stock zMod's ANALOG_PRUTOK,
    // lessWaste's _RUNOUT_HEAD, bambufy's _RUNOUT_HEAD — pauses the print,
    // unloads the spent lane and loads a matching one. Minutes during which the
    // toolhead is legitimately empty on a paused job. Wait it out rather than
    // talking over the recovery the printer is already performing. Stock zMod
    // is always-on (no toggle), so the !has_ifs_vars_ path always qualifies;
    // plugins qualify only when variable_backup reads true.
    if (!has_ifs_vars_ || ifs_backup_variable_.value_or(false)) {
        return RUNOUT_CONFIRM_DELAY_WITH_BACKUP;
    }
    return RUNOUT_CONFIRM_DELAY;
}

bool AmsBackendAd5xIfs::evaluate_runout_locked() {
    const bool paused = print_is_paused();

    if (runout_active_) {
        // The user resumed, cancelled, or started another job: whatever they did,
        // the paused-with-an-empty-head condition this fault describes is over.
        // Dropping the action back to IDLE is what makes AmsErrorBridge dismiss
        // the recovery modal (it watches the falling edge out of ERROR).
        if (!paused) {
            clear_runout_locked("print is no longer paused");
            return true;
        }
        return false;
    }

    if (!head_empty_since_.has_value()) {
        return false;
    }
    // The SWITCH pair is the authority, never head_filament_ - the motion sensor
    // also writes head_filament_ and reads false on a loaded-but-idle lane.
    if (!head_switch_seen_ || head_switch_present_) {
        head_empty_since_.reset();
        return false;
    }
    // A runout that matters stops the job. While PRINTING the same empty head is
    // the middle of a firmware tool change (A_CHANGE_FILAMENT drops the head
    // sensor partway through), which is exactly the false positive this gate
    // exists to avoid; Klipper queues a PAUSE behind the running macro, so a swap
    // cannot make the job read PAUSED with the head still empty.
    if (!paused) {
        return false;
    }
    // Nothing of ours in flight. phase_tracker_ covers load/unload;
    // action != IDLE additionally covers do_change_tool(), which sets LOADING
    // without arming the tracker.
    if (phase_tracker_.active || system_info_.action != AmsAction::IDLE) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    // eject_lane() and do_unload_filament()'s three early returns leave the
    // backend IDLE and armless, so neither test above sees them. The dispatch
    // stamp does.
    if (now - last_filament_op_dispatch_ < RUNOUT_OP_SUPPRESSION) {
        return false;
    }
    if (now - *head_empty_since_ < runout_confirm_delay_locked()) {
        return false;
    }

    // Which lane ran out. The firmware routinely drops its active pointer on a
    // runout (#995 "Extruder: None"), so fall through every seated authority we
    // have rather than reporting -1.
    runout_slot_ = -1;
    if (system_info_.current_slot >= 0) {
        runout_slot_ = system_info_.current_slot;
    } else if (seated_chan_ > 0) {
        runout_slot_ = seated_chan_ - 1;
    } else if (ffm_channel_ > 0) {
        runout_slot_ = ffm_channel_ - 1;
    } else if (persisted_seated_slot_.has_value()) {
        runout_slot_ = *persisted_seated_slot_;
    }

    runout_active_ = true;
    // The cross-backend flag. AmsState gates its runout indicator on
    // (filament_runout && paused), and we only ever raise while paused, so the
    // gate is a no-op here rather than a second condition to satisfy.
    system_info_.filament_runout = true;
    // AmsAction::ERROR is the only edge AmsErrorBridge watches, so it is the only
    // way current_error() ever gets consulted. check_action_timeout() ignores
    // ERROR, so this cannot be re-timed out on top of itself.
    system_info_.action = AmsAction::ERROR;
    system_info_.operation_indeterminate = false;
    set_operation_detail_locked(build_runout_detail_locked());
    spdlog::warn("{} Filament runout: toolhead switch empty for {}s on a paused print "
                 "(lane {}, plugin={}, backup={})",
                 backend_log_tag(),
                 std::chrono::duration_cast<std::chrono::seconds>(now - *head_empty_since_).count(),
                 runout_slot_, has_ifs_vars_ ? var_prefix_ : std::string("none"),
                 backup_state_locked());
    return true;
}

void AmsBackendAd5xIfs::clear_runout_locked(const char* why) {
    head_empty_since_.reset();
    if (!runout_active_) {
        // Not latched, but still clear the cross-backend flag: AmsState reads it
        // on every sync and a stale true would keep the unit-card warning icon lit
        // for a fault that is over.
        system_info_.filament_runout = false;
        return;
    }
    spdlog::info("{} Filament runout cleared ({})", backend_log_tag(), why);
    runout_active_ = false;
    runout_slot_ = -1;
    system_info_.filament_runout = false;
    if (system_info_.action == AmsAction::ERROR) {
        system_info_.action = AmsAction::IDLE;
        system_info_.operation_detail.clear();
    }
}

bool AmsBackendAd5xIfs::backup_eligible_locked(int slot, int candidate) const {
    if (slot < 0 || slot >= NUM_PORTS || candidate < 0 || candidate >= NUM_PORTS ||
        slot == candidate) {
        return false;
    }
    const auto src = static_cast<size_t>(slot);
    if (materials_[src].empty() || colors_[src].empty()) {
        return false; // Nothing to match against; claim nothing.
    }
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const auto idx = static_cast<size_t>(candidate);
    // All three conditions, no partial credit: this is the rule the hint text
    // states, and lessWaste's own switchover is type+colour matched. Offering
    // a "close enough" lane would print the wrong colour.
    return port_presence_[idx] && lower(materials_[idx]) == lower(materials_[src]) &&
           lower(colors_[idx]) == lower(colors_[src]);
}

int AmsBackendAd5xIfs::find_backup_slot_locked(int runout_slot) const {
    for (int i = 0; i < NUM_PORTS; ++i) {
        if (backup_eligible_locked(runout_slot, i)) {
            return i;
        }
    }
    return -1;
}

helix::printer::BackupEligibility
AmsBackendAd5xIfs::endless_spool_backup_eligibility(int slot_index, int backup_slot) const {
    using helix::printer::BackupEligibility;
    std::lock_guard<std::mutex> lock(mutex_);
    // Binary on purpose. The firmware matches ffmType and ffmColor exactly, so
    // a lane it will not select is not a choice to offer with a caveat - it is
    // a choice that would silently never fire. No GradeDiffers here.
    return backup_eligible_locked(slot_index, backup_slot) ? BackupEligibility::Eligible
                                                           : BackupEligibility::Incompatible;
}

helix::printer::EndlessSpoolCapabilities AmsBackendAd5xIfs::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    using namespace helix::printer;

    EndlessSpoolCapabilities caps;
    if (!has_ifs_vars_) {
        // Stock zMod's "Infinite Spool Mode" — `ANALOG_PRUTOK`
        // (zmod_ifs.py:cmd_ANALOG_PRUTOK), wired to head_switch_sensor's
        // runout_gcode in ad5x_display_off.cfg. No enable flag — it fires
        // unconditionally on head runout and switches to a slot whose ffmType
        // AND ffmColor both match AND whose port sensor reads present. There is
        // nothing for HelixScreen to write, so FirmwareManaged + ReadOnly.
        // Confirmed from zmod 1.7.1 source + corroborated on-device (raza616).
        caps.availability = EndlessSpoolAvailability::Available;
        caps.provider = "zmod";
        caps.enabled = EndlessSpoolEnabled::On;
        caps.editability = EndlessSpoolEditability::ReadOnly;
        caps.restriction = EndlessSpoolRestriction::FirmwareManaged;
        return caps;
    }

    caps.availability = EndlessSpoolAvailability::Available;
    caps.provider = (var_prefix_ == "bambufy") ? "bambufy" : "lessWaste";
    // Mapped from backup_state_locked() rather than re-derived, so the tri-state
    // the runout log reports and the tri-state the UI renders are one rule.
    // nullopt stays Unknown: flattening it to Off would tell the user no swap
    // will happen when we simply never read the setting.
    switch (backup_state_locked()) {
    case BACKUP_ON:
        caps.enabled = EndlessSpoolEnabled::On;
        break;
    case BACKUP_OFF:
        caps.enabled = EndlessSpoolEnabled::Off;
        break;
    default:
        caps.enabled = EndlessSpoolEnabled::Unknown;
        break;
    }
    caps.editability = EndlessSpoolEditability::ReadOnly;
    caps.restriction = EndlessSpoolRestriction::PluginReadOnly;
    return caps;
}

std::string AmsBackendAd5xIfs::build_runout_detail_locked() const {
    std::string detail =
        lv_tr("Filament ran out - nothing at the toolhead and the print is paused.");
    detail += " ";

    // The "what will switch" suffix is identical across all three modes because
    // the firmware-side rule is identical: ANALOG_PRUTOK (stock zMod) and
    // _RUNOUT_HEAD (lessWaste/bambufy) both require exact material AND exact
    // colour AND port-present. Only the subject of the sentence differs.
    //
    // Each lv_tr() below is a WHOLE sentence with the variable part as `{}`.
    // Building these by concatenation is what the catalogs used to hold: the
    // subject was glued on in C++ and the fragment left starting mid-clause
    // ("is installed but ...", "matches."). No translator can place a verb from
    // that -- German sends it to the end of the clause and Japanese reorders the
    // whole thing -- so the fragments sat untranslated in all eight locales.
    const auto append_switchover_rule = [&](const std::string& who) {
        detail += fmt::format(lv_tr("{} will switch to a slot whose filament type AND colour "
                                    "both match the active spool and whose own port sensor "
                                    "reads filament present."),
                              who);
        const int backup = find_backup_slot_locked(runout_slot_);
        detail += " ";
        if (backup >= 0) {
            detail += fmt::format(lv_tr("Slot {} matches."), backup + 1);
        } else {
            detail += lv_tr("No slot currently matches.");
        }
    };

    if (!has_ifs_vars_) {
        // Stock zMod "Infinite Spool Mode" (ANALOG_PRUTOK) — always on, no toggle.
        append_switchover_rule(lv_tr("Infinite Spool Mode"));
        return detail;
    }

    // i18n: do not translate — plugin names as their authors spell them.
    const std::string plugin_name = (var_prefix_ == "bambufy") ? "bambufy" : "lessWaste";
    if (!ifs_backup_variable_.has_value()) {
        detail += fmt::format(lv_tr("{} is installed, but its backup-spool setting could not be "
                                    "read - do not count on an automatic swap."),
                              plugin_name);
        return detail;
    }
    if (!*ifs_backup_variable_) {
        detail += fmt::format(lv_tr("{} is installed but its backup-spool switching is turned "
                                    "off, so no automatic swap will happen."),
                              plugin_name);
        return detail;
    }

    append_switchover_rule(plugin_name);
    return detail;
}

int AmsBackendAd5xIfs::find_first_tool_for_port(int port_1based) const {
    for (int t = 0; t < TOOL_MAP_SIZE; ++t) {
        if (tool_map_[static_cast<size_t>(t)] == port_1based) {
            return t;
        }
    }
    return -1; // No tool mapped to this port
}

void AmsBackendAd5xIfs::recompute_current_slot_locked() {
    // Native ZMOD (no lessWaste/bambufy _IFS_VARS) never populates tool_map_ — it
    // stays all-UNMAPPED, so the active_tool_ -> tool_map_ round-trip below
    // collapses to -1 and current_slot would be pinned at -1 forever, leaving
    // every slot "not loaded" (slot_is_actively_loaded) even with filament
    // demonstrably seated at the toolhead. The IFS_STATUS "Chan" (seated_chan_)
    // is the direct, sensor-backed seated authority there, and it survives a
    // timed-out GET_ZCOLOR (it only updates on a fresh successful read), so it is
    // the correct source for the loaded slot on the native path.
    if (!has_ifs_vars_) {
        system_info_.current_slot = seated_chan_ > 0 ? seated_chan_ - 1 : -1;
        return;
    }
    if (active_tool_ >= 0 && active_tool_ < TOOL_MAP_SIZE) {
        int port = tool_map_[static_cast<size_t>(active_tool_)];
        if (port >= 1 && port <= NUM_PORTS) {
            system_info_.current_slot = port - 1;
            return;
        }
    }
    system_info_.current_slot = -1;
}

bool AmsBackendAd5xIfs::clear_seated_if_ejected_locked(int slot_index) {
    const int ejected_chan = slot_index + 1;
    if (seated_chan_ != ejected_chan && ffm_channel_ != ejected_chan) {
        return false;
    }
    spdlog::debug("{} Eject lane {}: clearing stale seated pointer "
                  "(seated_chan_={}, ffm_channel_={}) (#1065 row 28)",
                  backend_log_tag(), slot_index, seated_chan_, ffm_channel_);
    seated_chan_ = 0;
    ffm_channel_ = 0;
    recompute_current_slot_locked();
    log_seated_state_locked("eject");
    return true;
}

void AmsBackendAd5xIfs::log_seated_state_locked(const char* where) const {
    std::string ports;
    for (int i = 0; i < NUM_PORTS; ++i) {
        ports += (i ? "," : "");
        ports += port_presence_[static_cast<size_t>(i)] ? "1" : "0";
    }
    const bool head_empty_authoritative = head_switch_seen_ && !head_switch_present_;
    spdlog::debug("{} [seated-trace {}] ffm_channel_={} seated_chan_={} current_slot={} "
                  "head_filament_={} head_switch_seen_={} head_switch_present_={} "
                  "head_empty_authoritative={} Ports=[{}]",
                  backend_log_tag(), where, ffm_channel_, seated_chan_, system_info_.current_slot,
                  head_filament_, head_switch_seen_, head_switch_present_, head_empty_authoritative,
                  ports);
}

void AmsBackendAd5xIfs::persist_seated_slot_locked(int slot0) {
    // No store in unit tests / remote-less setups — the in-memory
    // persisted_seated_slot_ still drives this session's restore logic; only the
    // cross-restart durability is skipped. The store call is fire-and-forget; its
    // SaveCallback only logs on failure, so the held mutex_ is never re-entered.
    if (!override_store_)
        return;
    const std::string tag = backend_log_tag();
    if (slot0 >= 0) {
        override_store_->save_seated_slot_async(slot0, [tag, slot0](bool ok, std::string err) {
            if (!ok)
                spdlog::warn("{} failed to persist seated slot {}: {}", tag, slot0, err);
        });
    } else {
        override_store_->clear_seated_slot_async([tag](bool ok, std::string err) {
            if (!ok)
                spdlog::warn("{} failed to clear persisted seated slot: {}", tag, err);
        });
    }
}

bool AmsBackendAd5xIfs::validate_slot_index(int slot_index) const {
    return slot_index >= 0 && slot_index < NUM_PORTS;
}

// ensure_homed_then() provided by AmsSubscriptionBackend

void AmsBackendAd5xIfs::on_home_confirmation_declined() {
    // load_filament()/unload_filament() arm HEATING + begin_phase_tracking_locked()
    // before ever reaching ensure_homed_then(); undo that half here, then let the
    // base implementation reset the action to IDLE and emit. Without this the
    // phase tracker stays active, apply_phase_action_locked() has no `!= IDLE`
    // guard, and the next extruder-temp frame flips IDLE -> HEATING again with a
    // fresh action_start_time_ -- 300s later check_action_timeout() latches ERROR
    // on an operation the user already declined.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        end_phase_tracking_locked();
        set_operation_detail_locked("");
    }
    AmsSubscriptionBackend::on_home_confirmation_declined();
}

void AmsBackendAd5xIfs::check_action_timeout() {
    // Indeterminate ("Working…") detector (#1065 row 14). While a phase-tracked
    // load/unload is in flight, if no genuine progress signal (temp-VALUE change,
    // head transition, motion, or phase change) has landed within the short
    // INDETERMINATE_THRESHOLD, the shared main-thread status feed has starved and
    // the live "Heat 225/230" number is frozen — raise the flag so the UI swaps
    // the frozen number for an indeterminate busy state instead of showing what
    // reads as a hang. Gated on phase_tracker_.active (a WE-initiated op with a
    // live step tracker) so external/firmware actions — whose progress clock we
    // don't drive — never false-fire. Distinct from the coarse ERROR budgets
    // below (minutes), which flip a genuinely stuck op to ERROR.
    if (phase_tracker_.active) {
        const auto since_progress = std::chrono::steady_clock::now() - last_phase_progress_time_;
        system_info_.operation_indeterminate =
            since_progress > std::chrono::seconds(INDETERMINATE_THRESHOLD_SECONDS);
    } else {
        system_info_.operation_indeterminate = false;
    }

    const AmsAction a = system_info_.action;
    // Cover every non-idle operation phase: the legacy LOADING/UNLOADING plus
    // the synthesized HEATING/CUTTING/PURGING the phase tracker drives. Without
    // this, a phased op that finishes on an intermediate phase (e.g. an
    // inactive-slot native-ZMOD unload that never produces a head transition)
    // would never reset to IDLE.
    if (a != AmsAction::LOADING && a != AmsAction::UNLOADING && a != AmsAction::HEATING &&
        a != AmsAction::CUTTING && a != AmsAction::PURGING) {
        return;
    }

    // HEATING from cold legitimately takes ~158s (longer for high-temp
    // materials), so it gets a longer dedicated budget. PURGING likewise runs
    // well past 90s and its clock is reset on motion-sensor activity (#1065
    // Bug 2). LOADING with swap_expected gets the extended swap budget so the
    // implicit unload before the load doesn't blow the 90s default. Every
    // other phase has its clock reset on transition (see
    // apply_phase_action_locked) and keeps the short 90s window.
    int limit = ACTION_TIMEOUT_SECONDS;
    if (a == AmsAction::HEATING) {
        limit = HEATING_TIMEOUT_SECONDS;
    } else if (a == AmsAction::PURGING) {
        limit = PURGING_TIMEOUT_SECONDS;
    } else if (a == AmsAction::LOADING && phase_tracker_.swap_expected) {
        // Belt-and-suspenders for the implicit-unload-before-load case. The
        // primary defence is the head-drop reset in on_head_transition_locked;
        // this covers firmware variants where the head sensor doesn't
        // transition reliably during a swap (bundle NJB2U558 ch4→ch2 swap
        // surfaced the false timeout at 90s).
        limit = SWAP_LOADING_TIMEOUT_SECONDS;
    }
    auto elapsed = std::chrono::steady_clock::now() - action_start_time_;
    if (elapsed >= std::chrono::seconds(limit)) {
        spdlog::warn("{} {} timed out after {}s, surfacing ERROR", backend_log_tag(),
                     ams_action_to_string(system_info_.action),
                     std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        // Preserve the current operation_detail as the error description so the
        // error-center bridge can show what was happening when the timeout fired.
        const std::string timeout_detail =
            system_info_.operation_detail.empty()
                ? lv_tr("Filament operation timed out")
                : system_info_.operation_detail + lv_tr(" (timed out)");
        system_info_.action = AmsAction::ERROR;
        // The op is resolving to ERROR — it is no longer merely "Working…".
        system_info_.operation_indeterminate = false;
        if (phase_tracker_.active) {
            end_phase_tracking_locked();
        }
        set_operation_detail_locked(timeout_detail);
    }
}

#endif // HELIX_HAS_IFS
