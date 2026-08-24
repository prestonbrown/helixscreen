// tests/test_helpers/ad5x_ifs_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Extracted verbatim from tests/unit/test_ams_backend_ad5x_ifs.cpp so other
// suites can seed AmsBackendAd5xIfs internals (declared `friend class
// Ad5xIfsTestAccess;` in include/ams_backend_ad5x_ifs.h — global scope, so
// the move changes nothing about how the friend resolves).
#pragma once

#include "ams_backend_ad5x_ifs.h"
#include "ams_error.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

using json = nlohmann::json;

// Test access helper — friend class for accessing internals
class Ad5xIfsTestAccess {
  public:
    static void handle_status(AmsBackendAd5xIfs& b, const json& n) {
        b.handle_status_update(n);
    }
    static void parse_vars(AmsBackendAd5xIfs& b, const json& v) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.parse_save_variables(v);
    }
    static int active_tool(const AmsBackendAd5xIfs& b) {
        return b.active_tool_;
    }
    static bool external_mode(const AmsBackendAd5xIfs& b) {
        return b.external_mode_;
    }
    static bool head_filament(const AmsBackendAd5xIfs& b) {
        return b.head_filament_;
    }
    static bool port_presence(const AmsBackendAd5xIfs& b, int i) {
        return b.port_presence_[static_cast<size_t>(i)];
    }
    static std::string build_colors(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_color_list_value();
    }
    static std::string runout_detail(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_runout_detail_locked();
    }
    static void set_runout_state(AmsBackendAd5xIfs& b, int slot, bool has_ifs_vars,
                                 std::optional<bool> backup_variable) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.runout_slot_ = slot;
        b.has_ifs_vars_ = has_ifs_vars;
        b.ifs_backup_variable_ = backup_variable;
    }
    static std::string build_types(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_type_list_value();
    }
    static std::string build_tools(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_tool_map_value();
    }
    static AmsAction action(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.action;
    }
    static void set_action(AmsBackendAd5xIfs& b, AmsAction a) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.action = a;
        b.action_start_time_ = std::chrono::steady_clock::now();
    }
    // Flip running_ so check_preconditions() passes without a live Moonraker
    // connection (mirrors TestableAfcBackend::set_running). Required by any test
    // exercising a public action method (load/unload/eject) that runs the
    // precondition gate first.
    static void set_running(AmsBackendAd5xIfs& b, bool state) {
        b.running_.store(state);
    }
    // Seed the firmware's active-slot pointer + head-filament sensor so the
    // "loaded in toolhead" refusal in eject_lane() can be exercised. These are
    // the exact members eject_lane()/unload_filament() read (system_info_.current_slot
    // and head_filament_).
    static void set_current_slot(AmsBackendAd5xIfs& b, int slot, bool filament_loaded) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_slot = slot;
        b.system_info_.filament_loaded = filament_loaded;
    }
    // Slot-only form for tests that seed the seated channel without asserting
    // the nozzle-loaded flag (e.g. the toolhead-unaccounted gate, which reads
    // current_slot independently of filament_loaded).
    static void set_current_slot(AmsBackendAd5xIfs& b, int slot) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_slot = slot;
    }
    static void set_head_filament(AmsBackendAd5xIfs& b, bool detected) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.head_filament_ = detected;
    }
    // Pin the toolhead SWITCH pair independently of the conflated head_filament_.
    // Production latches these only in the switch branch of handle_status_update();
    // tests need to express "switch says X while motion says Y", which is the
    // whole point of the pair existing. `seen=false` models motion-only firmware
    // that never publishes a switch sensor at all.
    static void set_head_switch(AmsBackendAd5xIfs& b, bool seen, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.head_switch_seen_ = seen;
        b.head_switch_present_ = present;
    }
    // Flag GET_ZCOLOR SILENT unsupported so schedule_zcolor_query() early-returns
    // instead of spawning an HttpExecutor debounce task — keeps action dispatch
    // tests fully synchronous and thread-free (avoids the [slow] tag, L052).
    static void set_zcolor_supported(AmsBackendAd5xIfs& b, bool supported) {
        b.zcolor_silent_supported_.store(supported);
    }
    static void check_action_timeout(AmsBackendAd5xIfs& b, std::chrono::seconds elapsed) {
        b.action_start_time_ = std::chrono::steady_clock::now() - elapsed;
        b.check_action_timeout();
    }
    // Age the action clock WITHOUT running the timeout check — lets a test age the
    // clock, fire an event that may reset it, then check separately.
    static void set_action_age(AmsBackendAd5xIfs& b, std::chrono::seconds elapsed) {
        b.action_start_time_ = std::chrono::steady_clock::now() - elapsed;
    }
    // Run the timeout check against the current (possibly event-reset) clock.
    static void run_action_timeout(AmsBackendAd5xIfs& b) {
        b.check_action_timeout();
    }
    // Age the indeterminate ("Working…") no-progress clock WITHOUT running the
    // detector, so a test can simulate a stalled progress feed then check
    // separately. Distinct from set_action_age (which ages the ERROR-timeout
    // clock); the indeterminate detector reads last_phase_progress_time_.
    static void set_progress_age(AmsBackendAd5xIfs& b, std::chrono::seconds elapsed) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.last_phase_progress_time_ = std::chrono::steady_clock::now() - elapsed;
    }
    // Read the indeterminate ("Working…") busy flag the detector computes.
    static bool operation_indeterminate(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.operation_indeterminate;
    }
    static std::string var_prefix(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.var_prefix_;
    }
    static bool has_per_port_sensors(const AmsBackendAd5xIfs& b) {
        return b.has_per_port_sensors_;
    }
    static size_t external_sync_count(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.external_sync_count_;
    }
    static void set_var_prefix(AmsBackendAd5xIfs& b, const std::string& prefix) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.var_prefix_ = prefix;
    }
    static bool has_ifs_vars(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.has_ifs_vars_;
    }
    static void set_has_ifs_vars(AmsBackendAd5xIfs& b, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.has_ifs_vars_ = val;
    }
    static void set_ifs_macro_confirmed_missing(AmsBackendAd5xIfs& b, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.ifs_macro_confirmed_missing_ = val;
    }
    static void parse_adventurer_json(AmsBackendAd5xIfs& b, const std::string& content) {
        b.parse_adventurer_json(content);
    }
    static bool dirty(const AmsBackendAd5xIfs& b, size_t idx) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.dirty_[idx];
    }
    static void set_dirty(AmsBackendAd5xIfs& b, size_t idx, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.dirty_[idx] = val;
    }
    // Seed firmware-state arrays directly. parse_save_variables no longer
    // writes colors_[]/materials_[] (those come from CHANGE_ZCOLOR/GET_ZCOLOR
    // exclusively now); tests that previously seeded via _IFS_VARS save_variables
    // should use these helpers and then re-run update_slot_from_state via
    // handle_status, parse_adventurer_json, or apply_zcolor_result.
    static void set_color(AmsBackendAd5xIfs& b, size_t idx, const std::string& hex) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.colors_[idx] = hex;
        b.update_slot_from_state(static_cast<int>(idx));
    }
    static void set_material(AmsBackendAd5xIfs& b, size_t idx, const std::string& mat) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.materials_[idx] = mat;
        b.update_slot_from_state(static_cast<int>(idx));
    }
    static void set_port_presence(AmsBackendAd5xIfs& b, size_t idx, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.port_presence_[idx] = val;
        b.update_slot_from_state(static_cast<int>(idx));
    }
    // Mirror eject_lane()'s optimistic clear (#1065): drop presence and stamp the
    // eject instant so the settling-suppression window is active. Used to test that
    // a lagging follow-up Ports read can't resurrect the just-ejected lane.
    static void mark_ejected(AmsBackendAd5xIfs& b, int slot) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.port_presence_[static_cast<size_t>(slot)] = false;
        b.last_eject_time_[static_cast<size_t>(slot)] = std::chrono::steady_clock::now();
        b.update_slot_from_state(slot);
    }
    // Age a lane's eject stamp past the suppression window so a genuine
    // re-insertion (false->true presence) is honored again.
    static void expire_eject_window(AmsBackendAd5xIfs& b, int slot) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.last_eject_time_[static_cast<size_t>(slot)] =
            std::chrono::steady_clock::now() - std::chrono::hours(1);
    }
    static int current_slot(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.current_slot;
    }
    static AmsBackendAd5xIfs::ZColorSilentResult
    parse_zcolor_silent(const std::vector<std::string>& lines) {
        return AmsBackendAd5xIfs::parse_zcolor_silent(lines, "test");
    }
    static bool zcolor_silent_supported(const AmsBackendAd5xIfs& b) {
        return b.zcolor_silent_supported_.load();
    }
    static void apply_zcolor_result(AmsBackendAd5xIfs& b,
                                    const AmsBackendAd5xIfs::ZColorSilentResult& r) {
        b.apply_zcolor_result(r);
    }
    // Seed the remembered seated lane directly (bypasses the Moonraker
    // "lane_data" DB load, which requires a live connection). Production loads
    // this at init via override_store_; tests inject it to exercise the
    // cold-boot restore path with a nullptr api/client.
    static void set_persisted_seated_slot(AmsBackendAd5xIfs& b, std::optional<int> slot) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.persisted_seated_slot_ = slot;
    }
    static std::optional<int> persisted_seated_slot(const AmsBackendAd5xIfs& b) {
        return b.persisted_seated_slot_;
    }
    // Seed / read the firmware's FFMInfo.channel seated authority (1-based; 0 =
    // none). Production sets this in parse_adventurer_json; tests seed it to drive
    // the seated-authority override in apply_zcolor_result without a full JSON parse.
    static void set_ffm_channel(AmsBackendAd5xIfs& b, int chan) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.ffm_channel_ = chan;
    }
    static int ffm_channel(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.ffm_channel_;
    }
    // Read the seated channel (1-based; 0 = none). This is the value the
    // head-gate (#1065 row 28) clears when the toolhead switch reads empty, and
    // recompute_current_slot_locked derives current_slot from it on the native
    // path.
    static int seated_chan(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.seated_chan_;
    }
    static bool head_switch_seen(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.head_switch_seen_;
    }
    static bool head_switch_present(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.head_switch_present_;
    }
    // Drive the belt-and-suspenders eject clear directly (#1065 row 28). Mirrors
    // what eject_lane() runs in its success block; exposed so the clear can be
    // tested without a live Moonraker connection (execute_gcode needs the api).
    static bool clear_seated_if_ejected(AmsBackendAd5xIfs& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.clear_seated_if_ejected_locked(slot_index);
    }
    // Seed the in-memory overrides map directly (bypasses load_blocking, which
    // requires a live Moonraker connection). on_started() is the only
    // production path that writes this field; tests must use this shim because
    // the fixtures instantiate the backend with nullptr api/client.
    static void seed_override(AmsBackendAd5xIfs& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    // Read the override currently staged for a slot (empty optional if none).
    // Lets tests assert what set_slot_info(persist=true) wrote into the
    // in-memory map without going through get_slot_info (which also layers
    // apply_overrides on top of firmware state).
    static std::optional<helix::ams::FilamentSlotOverride> get_override(const AmsBackendAd5xIfs& b,
                                                                        int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    // Inject an override store so persist=true set_slot_info has somewhere to
    // write. Production creates the store inside on_started(); tests that
    // build the backend with a concrete MoonrakerAPIMock but never call
    // on_started() need this shim to populate override_store_.
    static void inject_override_store(AmsBackendAd5xIfs& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    // Drive check_external_color_change directly with a caller-chosen observed
    // color. Convenience overloads:
    //   - uint32_t form: pass a real reading (forwards as std::optional{value}).
    //     Use this for tests asserting baseline updates / change detection.
    //   - std::nullopt_t form: pass the explicit "no reading" signal.
    //     Use this for tests asserting the "empty reading must not update
    //     baseline" contract (parse-order race protection).
    // `slot_has_filament` defaults to true so existing call sites that just
    // want to drive the baseline-update path don't need to think about
    // presence semantics.
    static bool check_external_color_change(AmsBackendAd5xIfs& b, int slot_index,
                                            uint32_t observed_color,
                                            bool slot_has_filament = true) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.check_external_color_change(slot_index, std::optional<uint32_t>{observed_color},
                                             slot_has_filament);
    }
    static bool check_external_color_change(AmsBackendAd5xIfs& b, int slot_index, std::nullopt_t,
                                            bool slot_has_filament = true) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.check_external_color_change(slot_index, std::nullopt, slot_has_filament);
    }
    // Type-change counterpart. observed_color is passed through so the helper's
    // "no color reading yet -> defer sync" branch can be exercised; defaults to
    // a present reading matching the common case.
    static bool check_external_type_change(AmsBackendAd5xIfs& b, int slot_index,
                                           const std::string& observed_material,
                                           bool slot_has_filament = true,
                                           std::optional<uint32_t> observed_color = 0x808080u) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.check_external_type_change(slot_index, observed_material, observed_color,
                                            slot_has_filament);
    }
    static std::optional<uint32_t> last_firmware_color(const AmsBackendAd5xIfs& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.last_firmware_color_.find(slot_index);
        if (it == b.last_firmware_color_.end())
            return std::nullopt;
        return it->second;
    }
    static std::optional<std::string> last_firmware_material(const AmsBackendAd5xIfs& b,
                                                             int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.last_firmware_material_.find(slot_index);
        if (it == b.last_firmware_material_.end())
            return std::nullopt;
        return it->second;
    }
    // Listener feedback fix (v0.99.51 spam loop) + JSON-poll watcher hooks.
    static bool on_gcode_response_line(AmsBackendAd5xIfs& b, const std::string& line) {
        return b.on_gcode_response_line(line);
    }
    static void set_zcolor_query_active(AmsBackendAd5xIfs& b, bool active) {
        b.zcolor_query_active_.store(active);
    }
    static uint32_t zcolor_schedule_count(const AmsBackendAd5xIfs& b) {
        return b.zcolor_schedule_count_.load();
    }
    static uint32_t zcolor_worker_submit_count(const AmsBackendAd5xIfs& b) {
        return b.zcolor_worker_submit_count_.load();
    }
    static bool zcolor_schedule_armed(const AmsBackendAd5xIfs& b) {
        return b.zcolor_schedule_armed_.load();
    }
    static size_t zcolor_buffer_size(AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.zcolor_buffer_mutex_);
        return b.zcolor_response_buffer_.size();
    }
    static bool note_json_content(AmsBackendAd5xIfs& b, const std::string& content) {
        return b.note_json_content(content);
    }
    // Override the resolved on-disk Adventurer5M.json path so tests can drive
    // the direct-write path against a tmp file instead of the real
    // /usr/prog/config target. Empty string forces the Moonraker fallback.
    static void set_local_adventurer_json_path(AmsBackendAd5xIfs& b, const std::string& p) {
        b.local_adventurer_json_path_ = p;
    }
    static const std::string& local_adventurer_json_path(const AmsBackendAd5xIfs& b) {
        return b.local_adventurer_json_path_;
    }
    // Drive the local read-modify-write path directly so tests can assert
    // file content without going through the full set_slot_info pipeline.
    static AmsError write_adventurer_json_local(AmsBackendAd5xIfs& b, int slot_index) {
        return b.write_adventurer_json_local(slot_index);
    }
    // tool_map snapshot: copy out for comparison without holding mutex_.
    static std::array<int, AmsBackendAd5xIfs::TOOL_MAP_SIZE> tool_map(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.tool_map_;
    }
    // Custom-types snapshot. Test fixture inspects what bambufy_custom_types
    // / user.cfg merging produced.
    static std::vector<std::string> custom_material_types(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.custom_types_mutex_);
        return b.custom_material_types_;
    }

    // --- filament.json eject-parameter cache (LEN/SPEED per material) ---

    // Drive the pure parse helper directly (no IO).
    static void parse_filament_json(AmsBackendAd5xIfs& b, const std::string& content) {
        b.parse_filament_json(content);
    }
    // Seed the per-material eject-parameter cache directly (mirrors what
    // fetch_filament_json applies on the main thread). Material key, tube
    // length (LEN), ifs speed (SPEED).
    static void seed_filament_eject_params(AmsBackendAd5xIfs& b, const std::string& material,
                                           int tube_length, int ifs_speed) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.filament_eject_params_[material] = {tube_length, ifs_speed};
    }
    // Read a cached pair back (returns nullopt when the material isn't cached).
    static std::optional<std::pair<int, int>> filament_eject_params(const AmsBackendAd5xIfs& b,
                                                                    const std::string& material) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.filament_eject_params_.find(material);
        if (it == b.filament_eject_params_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    // Read the parsed "default" pair (tube length, ifs speed).
    static std::pair<int, int> filament_eject_default(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.filament_eject_default_;
    }

    // --- Phase tracker hooks (live load/unload progress feedback) ---

    // Read the dynamic operation_detail string the phase machine produced.
    static std::string operation_detail(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.operation_detail;
    }

    // Read the granular operation_phase index the phase machine produced. This
    // is the generic AmsSystemInfo field AmsState mirrors into the
    // ams_operation_phase subject the right-side step tracker observes.
    static int operation_phase(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.operation_phase;
    }

    // Activate the phase tracker directly, bypassing load_filament/unload_filament's
    // check_preconditions() (which fails with the null api/client used in tests).
    // Mirrors what those entry points do: sets HEATING + begins phase tracking +
    // applies the initial synthesized action/detail under mutex_.
    static void begin_phase(AmsBackendAd5xIfs& b, bool is_unload) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.action = AmsAction::HEATING;
        b.action_start_time_ = std::chrono::steady_clock::now();
        b.begin_phase_tracking_locked(is_unload);
        b.apply_phase_action_locked();
    }

    static bool phase_active(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.phase_tracker_.active;
    }

    // Flip swap_expected — mirrors what load_filament does at dispatch when
    // another lane is currently seated (seated_chan_ != target). Lets tests
    // exercise check_action_timeout's swap-aware LOADING budget without
    // driving the full load_filament dispatch (which needs the gcode API).
    static void set_swap_expected(AmsBackendAd5xIfs& b, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.phase_tracker_.swap_expected = val;
    }
    static bool swap_expected(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.phase_tracker_.swap_expected;
    }

    static void finalize_op_after_macro(AmsBackendAd5xIfs& b, bool is_unload) {
        b.finalize_op_after_macro(is_unload);
    }

    // --- Unattended runout detection (#1250 / #1247) ---

    // Is a runout-shaped candidate armed? (a head-switch present->absent edge
    // seen while idle, not yet confirmed by the dwell)
    static bool head_empty_armed(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.head_empty_since_.has_value();
    }
    // Backdate the armed candidate so the confirm dwell has elapsed without the
    // test sleeping. No-op when nothing is armed.
    static void age_head_empty(AmsBackendAd5xIfs& b, std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        if (b.head_empty_since_.has_value()) {
            b.head_empty_since_ = std::chrono::steady_clock::now() - age;
        }
    }
    // Backdate the "a filament op was just dispatched" stamp past the
    // suppression window.
    static void age_op_dispatch(AmsBackendAd5xIfs& b, std::chrono::seconds age) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.last_filament_op_dispatch_ = std::chrono::steady_clock::now() - age;
    }
    static std::chrono::steady_clock::time_point op_dispatch_stamp(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.last_filament_op_dispatch_;
    }
    // Run the predicate against the current clock, exactly as handle_status_update
    // does after check_action_timeout(). Returns whether it changed state.
    static bool evaluate_runout(AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.evaluate_runout_locked();
    }
    static bool runout_active(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.runout_active_;
    }
    static int runout_slot(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.runout_slot_;
    }
    // The cross-backend AmsSystemInfo flag AmsState mirrors into
    // ams_filament_runout.
    static bool filament_runout(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.filament_runout;
    }
    static int find_backup_slot(const AmsBackendAd5xIfs& b, int runout_slot) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.find_backup_slot_locked(runout_slot);
    }
    static std::chrono::seconds runout_confirm_delay(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.runout_confirm_delay_locked();
    }

    // --- Plugin visibility (#1250 B1-4) ---

    // Drive the `gcode_macro _ifs_vars` get_status() dict parse directly.
    static bool parse_ifs_vars_macro(AmsBackendAd5xIfs& b, const json& macro_status) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.parse_ifs_vars_macro_locked(macro_status);
    }
    static int backup_state(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.backup_state_locked();
    }
};
