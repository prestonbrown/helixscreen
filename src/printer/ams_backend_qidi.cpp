// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_QIDI

#include "ams_backend_qidi.h"

#include "ui_insert_notice.h"
#include "ui_update_queue.h"

#include "ams_error.h"
#include "color_utils.h"
#include "display_numbering.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "macro_param_cache.h"
#include "settings_manager.h"
#include "slot_registry.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// Stub backend for the QIDI Box filament changer. Read-path mirrors
// save_variables onto AmsSystemInfo; write-path (load/unload/change_tool)
// is still not implemented pending field-test access (issue #954 brought
// the protocol reference; hardware validation still gated on Sib6019).
//
// TODO(qidi-box): drop a `qidi_box_64.png` (and matching .svg / `_512.png`
// if other backends carry them) into assets/images/ams/ to match the logo
// convention used by afc_64.png, box_turtle_64.png, happy_hare_64.png, etc.
// The QIDI wordmark / box silhouette is fine — no in-app scaling required.

namespace helix {

namespace {
// Parse `"slot<N>"` into N when valid and within [0, slot_count).
// Returns nullopt for the box_extras.py sentinel `"slot-1"` (nothing
// loaded) and for any other malformed input. Used to decode the
// `value_t<T>` and `last_load_slot` save_variables, both of which carry
// slot references in this format.
std::optional<int> parse_slot_name(const std::string& val, int slot_count) {
    if (val.rfind("slot", 0) != 0) {
        return std::nullopt;
    }
    try {
        int idx = std::stoi(val.substr(4));
        if (idx >= 0 && idx < slot_count) {
            return idx;
        }
    } catch (const std::exception&) {
        // Bad slot string — fall through to nullopt
    }
    return std::nullopt;
}

// Read a slot's palette row id (filament_slot<N> / color_slot<N> /
// vendor_slot<N>) from a save_variables value. The firmware and HelixScreen
// write bare ints, but a SAVE_VARIABLE spelled with a quoted VALUE - Qidi
// Studio emits VALUE="5" - is stored as a string, because save_variables.py
// evaluates the literal before saving it. Accept a non-negative integer, or a
// string holding nothing but a non-negative integer (surrounding whitespace
// tolerated) - zero IS an id: it is the Box stating the field is unset. Any
// other value is a writer the ids have no meaning for: logged and ignored,
// leaving the previously stated id in place.
std::optional<int> read_slot_id(const nlohmann::json& val, const std::string& key) {
    if (val.is_number_integer()) {
        const int id = val.get<int>();
        // A negative id has no palette row; the digits-only string branch
        // already refuses one, so the two spellings of the same write agree.
        if (id >= 0) {
            return id;
        }
    }
    if (val.is_string()) {
        std::string s = val.get<std::string>();
        const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        const auto is_digit = [](unsigned char c) { return std::isdigit(c) != 0; };
        if (!s.empty() && std::all_of(s.begin(), s.end(), is_digit)) {
            try {
                return std::stoi(s);
            } catch (const std::exception&) {
                // More digits than an int holds - no palette row is that big.
            }
        }
    }
    spdlog::debug("AmsBackendQidi save_variables {} holds a non-integer or negative value; ignored",
                  key);
    return std::nullopt;
}

constexpr int QIDI_SLOTS_PER_BOX = 4;
constexpr int QIDI_MAX_BOXES = 4;

AmsUnit make_qidi_unit(int unit_index) {
    AmsUnit unit;
    unit.unit_index = unit_index;
    unit.name = fmt::format("QIDI Box {}", helix::ui::lane_number(unit_index));
    unit.display_name = unit.name;
    unit.slot_count = QIDI_SLOTS_PER_BOX;
    unit.first_slot_global_index = unit_index * QIDI_SLOTS_PER_BOX;
    unit.connected = false;
    unit.topology = PathTopology::HUB;

    for (int local = 0; local < QIDI_SLOTS_PER_BOX; ++local) {
        const int global = unit.first_slot_global_index + local;
        SlotInfo slot;
        slot.slot_index = local;
        slot.global_index = global;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = global;
        unit.slots.push_back(std::move(slot));
    }
    return unit;
}

DryerInfo make_qidi_dryer() {
    DryerInfo d;
    d.supported = true;
    d.allows_during_print = true;
    d.min_temp_c = 35.0f;
    d.max_temp_c = 90.0f;
    d.max_duration_min = 720;
    d.supports_fan_control = false;
    // Default is the plain heater_generic path: SET_HEATER_TEMPERATURE retargets live,
    // and with no box-side timer the duration is our own bookkeeping. apply_box_extras
    // clears both if the firmware turns out to own the cycle.
    d.supports_live_temp = true;
    d.supports_live_duration = true;
    return d;
}

const SlotInfo* find_old_slot(const std::vector<AmsUnit>& units, int global_index) {
    for (const auto& unit : units) {
        if (global_index >= unit.first_slot_global_index &&
            global_index < unit.first_slot_global_index + unit.slot_count) {
            const int local = global_index - unit.first_slot_global_index;
            return unit.get_slot(local);
        }
    }
    return nullptr;
}

const AmsUnit* find_old_unit(const std::vector<AmsUnit>& units, int unit_index) {
    auto it = std::find_if(units.begin(), units.end(), [unit_index](const AmsUnit& unit) {
        return unit.unit_index == unit_index;
    });
    return it == units.end() ? nullptr : &*it;
}

void resize_qidi_units(AmsSystemInfo& info, int box_count) {
    box_count = std::clamp(box_count, 0, QIDI_MAX_BOXES);
    const auto old_units = info.units;

    std::vector<AmsUnit> units;
    units.reserve(static_cast<size_t>(box_count));
    for (int unit_index = 0; unit_index < box_count; ++unit_index) {
        AmsUnit unit = make_qidi_unit(unit_index);
        if (const AmsUnit* old = find_old_unit(old_units, unit_index)) {
            unit.connected = old->connected;
            unit.firmware_version = old->firmware_version;
            unit.serial_number = old->serial_number;
            unit.environment = old->environment;
        }
        for (auto& slot : unit.slots) {
            if (const SlotInfo* old = find_old_slot(old_units, slot.global_index)) {
                const int local = slot.slot_index;
                const int global = slot.global_index;
                slot = *old;
                slot.slot_index = local;
                slot.global_index = global;
            }
        }
        units.push_back(std::move(unit));
    }

    info.units = std::move(units);
    info.total_slots = box_count * QIDI_SLOTS_PER_BOX;
}

std::optional<int> parse_box_unit_index(const std::string& key) {
    const size_t pos = key.rfind("box");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    try {
        int box = std::stoi(key.substr(pos + 3));
        if (box >= 1 && box <= QIDI_MAX_BOXES) {
            return box - 1;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

// Hotend temperature to heat to before driving EXTRUDER_LOAD / M603. Klipper
// rejects extrusion below the configured min_extrude_temp ("Extrude below
// minimum temp"), so we heat to the slot's profile temperature first. Prefer the
// material's upper bound (matches the stock QIDI flow Camden verified — ABS at
// 280) and fall back to a safe universal load temperature when no profile is
// known.
constexpr int QIDI_DEFAULT_LOAD_TEMP_C = 250;

// Task 15 R2: cap for the officiall_filas_list.cfg fetch below. The file is a
// small generated INI (fila<N>/colordict/vendor_list sections); this is
// generous enough that a real stock file never hits it. download_file_partial
// fails loud on an over-cap response rather than silently truncating (see
// esp_http_lane.cpp on ESP32), so an oversized file degrades exactly like any
// other fetch failure here — non-fatal, temps stay at defaults.
constexpr size_t QIDI_FILAS_LIST_CAP_BYTES = 32 * 1024;

// Manual lane-eject via FORCE_MOVE on the box_stepper (#1041). Distance/velocity
// from the QIDI Discord (xenon) + Camden's Q2 measurement: 878 mm fully ejects,
// ~100 mm/s matches the stock feel. These are now user-configurable via
// SettingsManager (settings_qidi_eject_distance / settings_qidi_eject_velocity);
// the defaults below match the historic hardcoded values. May need refining for
// Plus 4 / Max 4 boxes.
int load_temp_for_slot(const SlotInfo& slot) {
    if (slot.nozzle_temp_max > 0) {
        return slot.nozzle_temp_max;
    }
    if (slot.nozzle_temp_min > 0) {
        return slot.nozzle_temp_min;
    }
    return QIDI_DEFAULT_LOAD_TEMP_C;
}
} // namespace

AmsBackendQidi::AmsBackendQidi(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Populate system_info_ so get_system_info() returns a self-consistent
    // empty-but-initialised snapshot even before any status update arrives.
    system_info_.type = AmsType::QIDI_BOX;
    system_info_.type_name = "QIDI Box"; // i18n: do not translate - product name
    system_info_.total_slots = NUM_SLOTS;
    system_info_.supports_bypass = false;
    system_info_.supports_purge = false;
    system_info_.tip_method = TipMethod::CUT;

    system_info_.units.push_back(make_qidi_unit(0));
    // make_qidi_unit() seeds the identity mapping on the slots; publish the
    // matching forward map so a pre-first-status snapshot is already
    // self-consistent in both directions. No lock: nothing else can observe
    // this instance until the constructor returns.
    rebuild_tool_map_locked();
    slot_rfid_.resize(NUM_SLOTS);

    // Box PTC dryer capabilities (issue #1019). max_temp_c is the settable ceiling
    // (target_max_temp_heater_generic=90); refined from configfile in on_started().
    // Per-unit: one DryerInfo per box, all sharing identical capability defaults.
    dryer_info_.assign(system_info_.units.size(), make_qidi_dryer());
    dry_end_epoch_.assign(system_info_.units.size(), 0);

    spdlog::debug("{} Backend constructed ({} slots, write-path always on)", backend_log_tag(),
                  NUM_SLOTS);
}

AmsBackendQidi::~AmsBackendQidi() = default;

// --- Lifecycle hooks ---

void AmsBackendQidi::on_started() {
    // Load persisted per-slot overrides before any parse can run, so the
    // fingerprint baseline and a staged clear agree about what the user had.
    // Call with NO lock held: the DB round-trip blocks, and the parse path
    // takes this mutex.
    if (api_) {
        auto loaded =
            helix::ams::make_loaded_override_store(api_, "qidi", get_type(), backend_log_tag());
        if (loaded.store) {
            helix::ams::ingest_legacy_records(*loaded.store, helix::ams::LegacyLockKeys::LaneData,
                                              backend_index());
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            override_store_ = std::move(loaded.store);
            overrides_ = std::move(loaded.overrides);
            // A persisted fingerprint from a build whose composite counted the
            // vendor id seeds a baseline the current composite never matches,
            // which would read as a swap at the first poll and clear every
            // override on upgrade: fold each onto the current shape first.
            for (auto& entry : overrides_) {
                entry.second.fingerprint = normalize_legacy_fingerprint(entry.second.fingerprint);
            }
            // A swap the user made while HelixScreen was down still has to
            // clear the stale override: the record's persisted fingerprint is
            // the baseline the first post-restart poll compares against.
            helix::ams::bind_fingerprint_persistence(rfid_tracker_, override_store_.get(),
                                                     overrides_);
        }
    }

    if (!client_) {
        return;
    }
    detect_firmware_capabilities();
    // Bootstrap: notify_status_update only carries deltas, so we need an
    // initial snapshot to populate save_variables. Subscribe to the QIDI
    // objects too — Moonraker won't push notifications for anything we
    // haven't subscribed to.
    //
    // Query save_variables + box_extras, and all possible box heater/humidity
    // objects. A machine may have 0..4 physical boxes; missing objects are
    // harmless in Moonraker's query response, and save_variables.box_count gates
    // which units are actually modeled.
    auto objects = nlohmann::json::object({{"save_variables", nullptr}, {"box_extras", nullptr}});
    for (int box = 1; box <= QIDI_MAX_BOXES; ++box) {
        objects[fmt::format("heater_generic heater_box{}", box)] = nullptr;
        objects[fmt::format("aht20_f heater_box{}", box)] = nullptr;
        objects[fmt::format("temperature_sensor heater_temp_a_box{}", box)] = nullptr;
        objects[fmt::format("temperature_sensor heater_temp_b_box{}", box)] = nullptr;
    }
    nlohmann::json params = {{"objects", std::move(objects)}};

    auto token = lifetime_.token();
    client_->send_jsonrpc("printer.objects.query", params, [this, token](nlohmann::json response) {
        // [L081] Mechanism C: defer member access to main thread.
        token.defer("AmsBackendQidi::on_started_apply",
                    [this, response = std::move(response)]() { apply_query_response(response); });
    });
    spdlog::info("{} Bootstrap query issued for save_variables + box_extras", backend_log_tag());

    // Query printer.configfile.settings to refine dryer_info_.max_temp_c from
    // the real Klipper settable ceiling (max_temp or target_max_temp_heater_generic).
    {
        nlohmann::json cfg_params = {
            {"objects", nlohmann::json::object({{"configfile", {"settings"}}})}};
        auto cfg_token = lifetime_.token();
        client_->send_jsonrpc(
            "printer.objects.query", cfg_params,
            [this, cfg_token](nlohmann::json response) {
                cfg_token.defer("AmsBackendQidi::apply_config_settings", [this,
                                                                          response = std::move(
                                                                              response)]() {
                    try {
                        // Guard every level before indexing.
                        // `response` is const in this
                        // non-mutable lambda, so operator[]
                        // resolves to the const overload — on a
                        // missing key that is a live assert(),
                        // an uncatchable SIGABRT, not the json
                        // exception this catch is written for.
                        if (!response.contains("result") ||
                            !response["result"].contains("status") ||
                            !response["result"]["status"].contains("configfile") ||
                            !response["result"]["status"]["configfile"].contains("settings") ||
                            !response["result"]["status"]["configfile"]["settings"].is_object()) {
                            spdlog::warn("{} configfile settings unavailable", backend_log_tag());
                            return;
                        }
                        const auto& settings =
                            response["result"]["status"]["configfile"]["settings"];
                        apply_config_settings(settings);
                        emit_event(EVENT_STATE_CHANGED);
                    } catch (const nlohmann::json::exception& e) {
                        spdlog::warn("{} configfile parse failed: {}", backend_log_tag(), e.what());
                    }
                });
            },
            [this](const MoonrakerError& err) {
                spdlog::warn("{} configfile query failed: {}", backend_log_tag(), err.message);
            });
    }

    // Also fetch officiall_filas_list.cfg so the temperature profile cache
    // is ready by the time filament_slot<N> entries arrive. The path is the
    // canonical Klipper config location used by box_extras.py.
    //
    // download_file_partial (bounded, in-memory), not download_file
    // (unbounded): the latter is a hard stub on ESP32 (Task 10's HTTP lane
    // only supports capped fetches — see esp_rest_api.cpp). Same Range-GET
    // semantics on desktop (moonraker_file_transfer_api.cpp), so this isn't
    // an ESP-only change.
    if (api_) {
        auto fila_token = lifetime_.token();
        api_->transfers().download_file_partial(
            "config", "officiall_filas_list.cfg", QIDI_FILAS_LIST_CAP_BYTES,
            [this, fila_token](const std::string& body) {
                // [L081] Defer apply onto main thread — apply_filas_list
                // touches member state under mutex_.
                fila_token.defer("AmsBackendQidi::filas_list_apply",
                                 [this, body]() { apply_filas_list(body); });
            },
            [this](const MoonrakerError& err) {
                spdlog::debug("{} officiall_filas_list.cfg fetch failed: {} "
                              "(non-fatal — temps stay at defaults)",
                              backend_log_tag(), err.message);
            });
    }
}

void AmsBackendQidi::apply_query_response(const nlohmann::json& response) {
    if (!response.is_object()) {
        return;
    }
    auto result_it = response.find("result");
    if (result_it == response.end() || !result_it->is_object()) {
        return;
    }
    auto status_it = result_it->find("status");
    if (status_it == result_it->end() || !status_it->is_object()) {
        return;
    }
    // The status object has the same shape as a notify_status_update
    // payload — both are `{<object_name>: <fields>, ...}` — so reuse
    // the notification handler verbatim.
    handle_status_update(*status_it);
}

void AmsBackendQidi::handle_status_update(const nlohmann::json& notification) {
    if (!notification.is_object()) {
        return;
    }
    // Moonraker delivers save_variables changes as
    // `{"save_variables": {"variables": {...}}}`. Unwrap and feed the inner
    // variables payload to parse_save_variables.
    auto sv_it = notification.find("save_variables");
    if (sv_it != notification.end() && sv_it->is_object()) {
        auto vars_it = sv_it->find("variables");
        if (vars_it != sv_it->end() && vars_it->is_object()) {
            parse_save_variables(*vars_it);
        }
    }

    // Per-box drying state arrives as separate top-level objects:
    //   "heater_generic heater_box<N>" → {temperature, target, power}
    //   "aht20_f heater_box<N>"        → {temperature, humidity}
    // Each physical box maps to the AmsUnit with the same zero-based index.
    apply_heater_status(notification);

    // box_extras carries box_drying_state.box<N>.{dry_state, end_time}
    // which drives the countdown timer shown in the dryer UI.
    if (auto be_it = notification.find("box_extras");
        be_it != notification.end() && be_it->is_object()) {
        apply_box_extras(*be_it);
    }
}

void AmsBackendQidi::apply_box_extras(const nlohmann::json& box_extras) {
    auto ds_it = box_extras.find("box_drying_state");
    if (ds_it == box_extras.end() || !ds_it->is_object()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    drying_timer_supported_ = true;
    // ENABLE_BOX_DRY owns the cycle and takes END_TIME in whole hours with a one-hour
    // floor, so a duration change re-quantizes and restarts. Whether re-issuing it with
    // a new TEMP preserves end_time is unverified on hardware, so temp is conservative.
    for (auto& d : dryer_info_) {
        d.supports_live_temp = false;
        d.supports_live_duration = false;
    }
    const std::time_t now = now_fn_();
    for (auto it = ds_it->begin(); it != ds_it->end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        auto unit_index = parse_box_unit_index(it.key()); // "box1" -> 0
        if (!unit_index || *unit_index >= static_cast<int>(dryer_info_.size())) {
            continue;
        }
        const size_t u = static_cast<size_t>(*unit_index);
        std::time_t end = 0;
        if (auto et = it->find("end_time"); et != it->end() && et->is_number()) {
            end = et->get<std::int64_t>();
        }
        // A drying cycle started outside HelixScreen (e.g. the QIDI stock UI) carries
        // no commanded duration, so the progress ring would be inert. When a NEW
        // end_time appears, derive the total from the first observed remaining so the
        // ring renders; our own start_drying() already set duration_min for UI-started
        // cycles (this just re-derives the same value once the firmware echoes it back).
        if (end > now && end != dry_end_epoch_[u]) {
            dryer_info_[u].duration_min = static_cast<int>((end - now) / 60);
        }
        dry_end_epoch_[u] = end;
        dryer_info_[u].active = (end > now);
    }
}

void AmsBackendQidi::apply_config_settings(const nlohmann::json& settings) {
    std::optional<float> settable_max;
    bool has_multi_color = false;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const std::string& key = it.key();
        // Max 4 dialect marker: a "[multi_color_controller]" section (bare or
        // instanced, e.g. "multi_color_controller box0") is Max 4-only. #1083
        if (key == "multi_color_controller" || key.rfind("multi_color_controller ", 0) == 0) {
            has_multi_color = true;
        }
        if (!it->is_object()) {
            continue;
        }
        if (key.rfind("heater_generic heater_box", 0) == 0) {
            if (auto m = it->find("max_temp"); m != it->end() && m->is_number()) {
                settable_max = m->get<float>();
            }
        } else if (key.rfind("box_config box", 0) == 0) {
            if (auto m = it->find("target_max_temp_heater_generic");
                m != it->end() && m->is_number()) {
                settable_max = m->get<float>();
            }
        }
    }
    if (settable_max) {
        std::lock_guard<std::mutex> lock(mutex_);
        // All boxes share the same settable ceiling.
        for (auto& d : dryer_info_) {
            d.max_temp_c = *settable_max;
        }
        spdlog::info("{} Box dryer max temp from config: {}°C", backend_log_tag(), *settable_max);
    }

    // Lane eject (#1041) is a FORCE_MOVE on the box_stepper, which Klipper rejects
    // unless [force_move] enable_force_move: True. Gate supports_lane_eject() on it
    // so we never offer an eject button that would just error.
    bool force_move = false;
    if (auto fm = settings.find("force_move"); fm != settings.end() && fm->is_object()) {
        if (auto en = fm->find("enable_force_move"); en != fm->end() && en->is_boolean()) {
            force_move = en->get<bool>();
        }
    }
    fw_force_move_enabled_ = force_move;
    box_uses_multi_color_ = has_multi_color;
    spdlog::info("{} Lane eject {} (multi_color_controller {}, force_move {})", backend_log_tag(),
                 (has_multi_color || force_move) ? "available" : "unavailable",
                 has_multi_color ? "present -> Max 4 dialect" : "absent",
                 force_move ? "enabled" : "disabled/absent");
}

void AmsBackendQidi::apply_heater_status(const nlohmann::json& notification) {
    constexpr std::string_view HEATER_PREFIX = "heater_generic heater_box";
    constexpr std::string_view AHT20_PREFIX = "aht20_f heater_box";
    constexpr std::string_view BOX_TEMP_SENSOR_PREFIX = "temperature_sensor heater_temp_";

    struct BoxReading {
        std::optional<float> temp;
        std::optional<float> humidity;
        std::optional<float> target;
    };
    std::array<BoxReading, QIDI_MAX_BOXES> readings;

    for (auto it = notification.begin(); it != notification.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        const std::string& key = it.key();
        const bool is_heater = key.rfind(HEATER_PREFIX, 0) == 0;
        const bool is_aht = key.rfind(AHT20_PREFIX, 0) == 0;
        const bool is_box_temp_sensor =
            key.rfind(BOX_TEMP_SENSOR_PREFIX, 0) == 0 && key.find("_box") != std::string::npos;
        if (!is_heater && !is_aht && !is_box_temp_sensor) {
            continue;
        }
        auto unit_index = parse_box_unit_index(key);
        if (!unit_index) {
            continue;
        }
        auto& reading = readings[static_cast<size_t>(*unit_index)];

        // Box temperature is sourced from the heater object and the ambient
        // aht20 object. The heater_temp_*_box thermistors mirror the heater
        // element and are deliberately excluded from displayed temperature.
        if (is_heater || is_aht) {
            if (auto t_it = it->find("temperature"); t_it != it->end() && t_it->is_number()) {
                const float v = t_it->get<float>();
                if (!reading.temp || v > *reading.temp) {
                    reading.temp = v;
                }
            }
        }
        if (is_heater) {
            if (auto tgt_it = it->find("target"); tgt_it != it->end() && tgt_it->is_number()) {
                const float v = tgt_it->get<float>();
                if (!reading.target || v > *reading.target) {
                    reading.target = v;
                }
            }
        }
        if (auto h_it = it->find("humidity"); h_it != it->end() && h_it->is_number()) {
            const float v = h_it->get<float>();
            if (!reading.humidity || v > *reading.humidity) {
                reading.humidity = v;
            }
        }
    }

    bool any_env = false;
    for (const auto& r : readings) {
        any_env = any_env || r.temp || r.humidity || r.target;
    }
    if (!any_env) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < readings.size() && i < system_info_.units.size(); ++i) {
        const auto& reading = readings[i];
        if (!reading.temp && !reading.humidity && !reading.target) {
            continue;
        }
        auto& env = system_info_.units[i].environment;
        if (!env) {
            env = EnvironmentData{};
        }
        if (reading.temp) {
            env->temperature_c = *reading.temp;
        }
        if (reading.humidity) {
            env->humidity_pct = *reading.humidity;
            env->has_humidity = true;
        }
        // Per-box dryer state: write each box's heater reading into its own unit.
        if (i < dryer_info_.size()) {
            if (reading.temp) {
                dryer_info_[i].current_temp_c = *reading.temp;
            }
            if (reading.target) {
                dryer_info_[i].target_temp_c = *reading.target;
            }
        }
    }
}

void AmsBackendQidi::rebuild_tool_map_locked() {
    // SlotRegistry::set_tool_mapping() is the canonical implementation of the
    // "a tool maps to exactly one slot" bookkeeping — it maintains both
    // directions in lockstep and evicts whichever side lost a contested tool
    // number. QIDI keeps its slot state on system_info_ rather than in a
    // registry, so run the mapping through a throwaway registry and read both
    // directions back out of it instead of hand-writing the reverse map here.
    const int slot_count = std::max(0, system_info_.total_slots);

    helix::printer::SlotRegistry ledger;
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(slot_count));
    for (int i = 0; i < slot_count; ++i) {
        names.push_back("slot" + std::to_string(i));
    }
    ledger.initialize("QIDI Box", names);

    for (int i = 0; i < slot_count; ++i) {
        const auto* slot = system_info_.get_slot_global(i);
        if (slot && slot->mapped_tool >= 0) {
            ledger.set_tool_mapping(i, slot->mapped_tool);
        }
    }

    // Write the registry's normalised view back onto the slots: if two slots
    // claimed the same tool number, set_tool_mapping() already dropped the
    // loser, and the badge must not keep showing a tool that no longer routes
    // through that lane.
    for (int i = 0; i < slot_count; ++i) {
        if (auto* slot = system_info_.get_slot_global(i)) {
            slot->mapped_tool = ledger.tool_for_slot(i);
        }
    }

    system_info_.tool_to_slot_map = ledger.tool_map();
}

// The Box states no tag UID: the save_variable table indices for fila and
// colour are the most specific identity it reports, so their composite is the
// spool fingerprint (the same surrogate CFS builds from material_type and
// color_value). The vendor id is not part of it: it names a row in the Box's
// own vendor table, not something the spool carries, so a supplier change
// alone is not a new spool. All-zero ids state nothing, which the tracker
// reads as no signal rather than a fingerprint.
static std::string qidi_slot_fingerprint(int filament_id, int color_id) {
    if (filament_id <= 0 && color_id <= 0) {
        return "";
    }
    return fmt::format("{}|{}", filament_id, color_id);
}

std::string AmsBackendQidi::normalize_legacy_fingerprint(const std::string& stored) {
    // "fila|color|vendor" from a build whose composite counted the vendor id
    // folds to the two-component shape. Anything with fewer components, or a
    // non-numeric body, is already current or a value no build of ours wrote:
    // left exactly as found.
    const auto first = stored.find('|');
    if (first == std::string::npos) {
        return stored;
    }
    const auto second = stored.find('|', first + 1);
    if (second == std::string::npos || stored.find('|', second + 1) != std::string::npos) {
        return stored;
    }
    const std::string folded = stored.substr(0, second);
    const bool numeric = std::all_of(folded.begin(), folded.end(),
                                     [](char ch) { return ch == '|' || (ch >= '0' && ch <= '9'); });
    return numeric ? folded : stored;
}

void AmsBackendQidi::parse_save_variables(const nlohmann::json& variables) {
    if (!variables.is_object()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    auto box_count_it = variables.find("box_count");
    if (box_count_it != variables.end() && box_count_it->is_number_integer()) {
        const int box_count = box_count_it->get<int>();
        if (box_count >= 0 && box_count <= QIDI_MAX_BOXES) {
            resize_qidi_units(system_info_, box_count);
            slot_rfid_.resize(static_cast<size_t>(system_info_.total_slots));
            // Keep the per-unit dryer vectors sized to the box count, preserving
            // existing state and seeding new boxes with capability defaults.
            if (dryer_info_.size() != system_info_.units.size()) {
                dryer_info_.resize(system_info_.units.size(), make_qidi_dryer());
                dry_end_epoch_.resize(system_info_.units.size(), 0);
            }
        }
    }

    auto enable_it = variables.find("enable_box");
    if (enable_it != variables.end() && enable_it->is_number_integer()) {
        const bool connected = enable_it->get<int>() != 0;
        for (auto& unit : system_info_.units) {
            unit.connected = connected;
        }
    }

    const int slot_count = system_info_.total_slots;

    // value_t<N> = "slot<M>" — tool N prints from global slot M.
    for (int t = 0; t < slot_count; ++t) {
        const std::string key = "value_t" + std::to_string(t);
        auto vt_it = variables.find(key);
        if (vt_it == variables.end() || !vt_it->is_string()) {
            continue;
        }
        if (auto idx = parse_slot_name(vt_it->get<std::string>(), slot_count)) {
            for (auto& unit : system_info_.units) {
                for (auto& slot : unit.slots) {
                    if (slot.mapped_tool == t) {
                        slot.mapped_tool = -1;
                    }
                }
            }
            if (auto* slot = system_info_.get_slot_global(*idx)) {
                slot->mapped_tool = t;
            }
        }
    }

    // Publish the forward map alongside the reverse mapped_tool the loop above
    // just wrote — same pass, same source, so the two cannot disagree. Runs
    // unconditionally because a box_count change earlier in this function
    // resizes the slot vector and the map has to follow it.
    rebuild_tool_map_locked();

    bool stated_slot[QIDI_MAX_BOXES * QIDI_SLOTS_PER_BOX] = {};
    // Captured before the loop below rewrites them: a slot whose standing
    // status was EMPTY and whose state word now reports filament just
    // received a spool, which is the insert edge the notice below keys on.
    bool was_empty[QIDI_MAX_BOXES * QIDI_SLOTS_PER_BOX] = {};
    for (int i = 0; i < slot_count; ++i) {
        if (const auto* standing = system_info_.get_slot_global(i)) {
            was_empty[i] = standing->status == SlotStatus::EMPTY;
        }
    }
    for (int i = 0; i < slot_count; ++i) {
        auto* slot = system_info_.get_slot_global(i);
        if (!slot) {
            continue;
        }
        const std::string key = "slot" + std::to_string(i);
        auto slot_it = variables.find(key);
        if (slot_it == variables.end() || !slot_it->is_number_integer()) {
            continue;
        }
        stated_slot[i] = true;
        const int state = slot_it->get<int>();
        switch (state) {
        case 0:
            slot->status = SlotStatus::EMPTY;
            break;
        case 1:
        case 3:
            slot->status = SlotStatus::AVAILABLE;
            break;
        case 2:
            slot->status = SlotStatus::LOADED;
            break;
        default:
            slot->status = (state < 0) ? SlotStatus::BLOCKED : SlotStatus::UNKNOWN;
            break;
        }
    }

    auto tool_change_it = variables.find("is_tool_change");
    const bool has_slot_or_action_key = [&]() {
        if (tool_change_it != variables.end()) {
            return true;
        }
        for (int i = 0; i < slot_count; ++i) {
            if (variables.find("slot" + std::to_string(i)) != variables.end()) {
                return true;
            }
        }
        return false;
    }();

    if (has_slot_or_action_key) {
        bool any_blocked = false;
        for (const auto& unit : system_info_.units) {
            any_blocked = any_blocked ||
                          std::any_of(unit.slots.begin(), unit.slots.end(), [](const SlotInfo& s) {
                              return s.status == SlotStatus::BLOCKED;
                          });
        }
        const bool is_loading = tool_change_it != variables.end() &&
                                tool_change_it->is_number_integer() &&
                                tool_change_it->get<int>() != 0;
        system_info_.action =
            any_blocked ? AmsAction::ERROR : (is_loading ? AmsAction::LOADING : AmsAction::IDLE);
    }

    auto load_it = variables.find("last_load_slot");
    if (load_it != variables.end() && load_it->is_string()) {
        const std::string val = load_it->get<std::string>();
        if (val == "slot-1") {
            for (auto& unit : system_info_.units) {
                for (auto& slot : unit.slots) {
                    if (slot.status == SlotStatus::LOADED) {
                        slot.status = SlotStatus::AVAILABLE;
                    }
                }
            }
            system_info_.current_slot = -1;
            system_info_.current_tool = -1;
            system_info_.filament_loaded = false;
        } else if (auto idx = parse_slot_name(val, slot_count)) {
            if (auto* slot = system_info_.get_slot_global(*idx)) {
                slot->status = SlotStatus::LOADED;
                system_info_.current_slot = *idx;
                system_info_.current_tool = slot->mapped_tool;
                system_info_.filament_loaded = true;
            }
        }
    }

    if (slot_rfid_.size() < static_cast<size_t>(slot_count)) {
        slot_rfid_.resize(static_cast<size_t>(slot_count));
    }
    for (int i = 0; i < slot_count; ++i) {
        auto* slot = system_info_.get_slot_global(i);
        if (!slot) {
            continue;
        }
        const std::string suffix = std::to_string(i);
        const auto read_slot_var = [&variables](const std::string& key) -> std::optional<int> {
            const auto it = variables.find(key);
            if (it == variables.end()) {
                return std::nullopt;
            }
            return read_slot_id(*it, key);
        };
        auto& ids = slot_rfid_[static_cast<size_t>(i)];
        if (const auto id = read_slot_var("filament_slot" + suffix)) {
            ids.filament_id = *id;
        }
        if (const auto id = read_slot_var("color_slot" + suffix)) {
            ids.color_id = *id;
        }
        if (const auto id = read_slot_var("vendor_slot" + suffix)) {
            ids.vendor_id = *id;
        }

        const auto& rfid = slot_rfid_[static_cast<size_t>(i)];
        const std::string fingerprint = qidi_slot_fingerprint(rfid.filament_id, rfid.color_id);

        // A fingerprint change the Box did not hear from us is judged by the
        // insert rule: ids decoding to a different spool outrank edits made
        // for the previous one. Runs before the paint below so a cleared slot
        // shows the new tag's identity in this same pass. The verdict feeds
        // the insert notice below, so it is computed here and read there.
        helix::ams::InsertVerdict verdict = helix::ams::InsertVerdict::NoEvidence;
        check_hardware_event_clear(*slot, i, fingerprint, &verdict);

        // The saved ids name rows in the Box's own tables, which makes a row
        // that resolves the reading and an id that resolves against nothing no
        // reading at all. The observed values are the LOOKUP RESULTS rather
        // than what lands on slot->: SlotInfo persists across frames, so a
        // table reload that drops a row leaves the last frame's value sitting
        // there with nothing behind it, and reading it back would file that
        // orphan as something the Box still says.
        //
        // An id of 0 is the Box stating the field is unset, so the else-arms
        // blank it: without them the paint from the last frame before the zero
        // lands would linger forever, the > 0 gates below having nothing to
        // overwrite.
        helix::ams::Observation cache(helix::ams::ObservationSource::VendorCache);
        if (rfid.filament_id > 0) {
            auto p = fila_profiles_.find(rfid.filament_id);
            if (p != fila_profiles_.end()) {
                slot->nozzle_temp_min = p->second.nozzle_min;
                slot->nozzle_temp_max = p->second.nozzle_max;
                if (!p->second.type.empty()) {
                    slot->material = p->second.type;
                    cache.material = p->second.type;
                }
            }
        } else {
            slot->material.clear();
            slot->nozzle_temp_min = 0;
            slot->nozzle_temp_max = 0;
        }
        if (rfid.color_id > 0) {
            auto c = color_palette_.find(rfid.color_id);
            if (c != color_palette_.end()) {
                slot->color_rgb = c->second;
                // The palette row reaches here as a decoded integer, so the
                // struct-side rule is the one that applies.
                if (helix::ams::is_declarable_color(c->second)) {
                    cache.color_rgb = c->second;
                }
            }
        } else {
            slot->color_rgb = 0;
        }
        if (rfid.vendor_id > 0) {
            auto v = vendor_names_.find(rfid.vendor_id);
            if (v != vendor_names_.end() && !v->second.empty()) {
                slot->brand = v->second;
                cache.brand = v->second;
            }
        } else {
            // vendor 0 is a real table row ("Generic"), but a slot the Box
            // reports as untagged shows no brand, matching the clear.
            slot->brand.clear();
        }
        // The echo guard: ids repeating the user's own SAVE_VARIABLE push are
        // not a reading, and ingest files the record whole, so a withheld
        // field goes absent rather than back to its pre-edit value. The cache
        // is rebuilt from the saved ids every frame, so a frame that carries
        // no id keys restates the write the same way and is judged the same
        // way. A key carried with 0 never reaches here armed: it moves the
        // fingerprint first, and the clear that follows drops the guard.
        own_write_echoes_.withhold(i, std::string{}, cache);
        helix::ams::ingest(lane_id(i), cache);

        // An insert the ids did not vouch for: the table rows are the Box's
        // memory of the last spool, so a spool that goes in without moving
        // them is one nothing was read of, and the details standing on the
        // lane may describe it or not. The ask re-checks every guard on the
        // main thread; this loop holds mutex_.
        if (was_empty[i] && stated_slot[i] && slot_status_reports_filament(slot->status) &&
            verdict == helix::ams::InsertVerdict::NoEvidence) {
            helix::ui::queue_update([i] { helix::ui::offer_clear_after_unverified_insert(i); });
        }
    }

    // Reconcile the LOADED stamp with the aggregate pair, after both writers.
    // The slot<N> loop runs first and derives status from the per-slot state
    // word alone, which carries no notion of "seated at the extruder"; only the
    // last_load_slot block below it writes current_slot / filament_loaded. A
    // payload that repeats slot<M> without repeating last_load_slot therefore
    // demoted the seated slot to AVAILABLE while the aggregate still named it,
    // which is the disagreement has_per_slot_loaded_authority() cannot tolerate
    // (#1199).
    //
    // A negative state word (BLOCKED) wins: a slot the Box has faulted must not
    // be painted healthy just because it is the seated one.
    if (system_info_.filament_loaded && system_info_.current_slot >= 0) {
        if (auto* seated = system_info_.get_slot_global(system_info_.current_slot)) {
            if (seated->status != SlotStatus::BLOCKED) {
                seated->status = SlotStatus::LOADED;
            }
        }
    }

    // Presence, once every writer of status has run: the slot<N> loop derives
    // it from the state word, last_load_slot can promote a slot to LOADED, and
    // the reconciliation above settles the two. A negative state word is
    // BLOCKED, which reports filament - a jam is filament stuck in the path,
    // and answering "no reading" there would retract a live presence record at
    // the moment a lane jams.
    //
    // A lane the frame said nothing about (no slot<N> key) files nothing, so a
    // frame that stays silent keeps whatever the last frame that did speak
    // established. UNKNOWN the frame DID state, and filing it with present
    // unset retracts the earlier reading the way ACE, AD5X IFS and Happy Hare
    // treat a machine that declines to state.
    for (int i = 0; i < slot_count; ++i) {
        const auto* s = system_info_.get_slot_global(i);
        if (!s || !stated_slot[i]) {
            continue;
        }
        helix::ams::Observation sensed(helix::ams::ObservationSource::Sensed);
        sensed.present = slot_status_reports_filament(s->status);
        helix::ams::ingest(lane_id(i), sensed);
    }

    // Lay each slot's resolved lane over the machine paint, last, so a field
    // the user declared outranks this frame's tag reading: an edit stays on
    // screen between its SAVE_VARIABLE dispatch and the echo, and a stored
    // edit shows again after a restart. Only fields some source observed are
    // written, so a lane no one declared against keeps the tag's own paint,
    // and the status overlay runs through presence filed from the reconciled
    // stamp above, which is what has_per_slot_loaded_authority() promises.
    for (int i = 0; i < slot_count; ++i) {
        if (auto* slot = system_info_.get_slot_global(i)) {
            apply_resolved_lane(*slot, i);
        }
    }
}

void AmsBackendQidi::apply_filas_list(const std::string& content) {
    // Minimal ConfigParser-compatible INI: `[section]` headers, `key = value`
    // lines, `#` or `;` comments. Three section kinds are honoured:
    //   [fila<N>]     → FilaProfile (name, type, nozzle + box temps)
    //   [colordict]   → integer id → 0xRRGGBB
    //   [vendor_list] → integer id → vendor name
    // Everything else is silently ignored. Trailing `#`/`;` tail content on a
    // value is dropped. All three maps are built into locals and swapped under
    // mutex_ at the end so a reload replaces atomically.
    auto trim = [](std::string s) {
        const auto not_space = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    };
    // Drop inline `;` / `#` tail then trim. ConfigParser keeps `#` inside a
    // value unless preceded by whitespace, but the stock file never relies on
    // that, so the simpler "strip at first comment char" rule is fine here.
    auto strip_tail = [&](const std::string& v) {
        std::string body = v;
        for (char ch : {';', '#'}) {
            // A leading '#' is a color literal, not a comment — only strip a
            // '#' that is not at the very start of the trimmed value.
            auto pos = body.find(ch);
            if (ch == '#' && pos == 0) {
                continue;
            }
            if (pos != std::string::npos) {
                body.erase(pos);
            }
        }
        return trim(body);
    };
    auto parse_int_field = [&](const std::string& v, int& out) {
        try {
            out = std::stoi(strip_tail(v));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    enum class Section { None, Fila, Color, Vendor };

    std::map<int, FilaProfile> next_profiles;
    std::map<int, std::uint32_t> next_colors;
    std::map<int, std::string> next_vendors;

    Section section = Section::None;
    std::optional<int> current_id;
    FilaProfile current;

    auto flush_fila = [&]() {
        if (section == Section::Fila && current_id) {
            next_profiles[*current_id] = current;
        }
        current_id.reset();
        current = FilaProfile{};
    };

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (t.empty() || t.front() == '#' || t.front() == ';') {
            continue;
        }
        if (t.front() == '[' && t.back() == ']') {
            flush_fila();
            std::string name = trim(t.substr(1, t.size() - 2));
            if (name == "colordict") {
                section = Section::Color;
            } else if (name == "vendor_list") {
                section = Section::Vendor;
            } else if (name.rfind("fila", 0) == 0) {
                section = Section::Fila;
                try {
                    int id = std::stoi(name.substr(4));
                    if (id > 0) {
                        current_id = id;
                    }
                } catch (const std::exception&) {
                    // Malformed `fila<N>` — treat as no current section.
                    section = Section::None;
                }
            } else {
                section = Section::None;
            }
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = t.substr(eq + 1);

        switch (section) {
        case Section::Fila: {
            if (!current_id) {
                break;
            }
            if (key == "filament") {
                current.name = strip_tail(val);
            } else if (key == "type") {
                current.type = strip_tail(val);
            } else if (key == "min_temp") {
                parse_int_field(val, current.nozzle_min);
            } else if (key == "max_temp") {
                parse_int_field(val, current.nozzle_max);
            } else if (key == "box_min_temp") {
                parse_int_field(val, current.box_min);
            } else if (key == "box_max_temp") {
                parse_int_field(val, current.box_max);
            }
            break;
        }
        case Section::Color: {
            try {
                int id = std::stoi(trim(key));
                // A colordict row is a lane-shaped colour value like any other
                // producer's, so the tree's one hex grammar reads it: `#RGB`,
                // a bare `RRGGBB`, an `0x` prefix and the 8-digit `#RRGGBBAA`
                // all mean here what they mean everywhere else. Only a row
                // stating a colour becomes a palette entry; a blank row states
                // that the id names none, which is not a colour to resolve
                // against.
                const auto reading = helix::ams::read_lane_color(val);
                if (reading.kind == helix::ams::ColorReadingKind::Observed) {
                    next_colors[id] = reading.rgb;
                }
            } catch (const std::exception&) {
                // Non-integer key in [colordict] — ignore.
            }
            break;
        }
        case Section::Vendor: {
            try {
                int id = std::stoi(trim(key));
                next_vendors[id] = strip_tail(val);
            } catch (const std::exception&) {
                // Non-integer key in [vendor_list] — ignore.
            }
            break;
        }
        case Section::None:
            break;
        }
    }
    flush_fila();

    std::lock_guard<std::mutex> lock(mutex_);
    fila_profiles_ = std::move(next_profiles);
    color_palette_ = std::move(next_colors);
    vendor_names_ = std::move(next_vendors);
    spdlog::info("{} Loaded {} fila profile(s), {} color(s), {} vendor(s) from "
                 "officiall_filas_list.cfg",
                 backend_log_tag(), fila_profiles_.size(), color_palette_.size(),
                 vendor_names_.size());
}

// --- State queries ---

AmsSystemInfo AmsBackendQidi::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

std::vector<int> AmsBackendQidi::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

SlotInfo AmsBackendQidi::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return SlotInfo{};
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    return slot ? *slot : SlotInfo{};
}

SlotInfo* AmsBackendQidi::cached_slot_locked(int slot_index) {
    return system_info_.get_slot_global(slot_index);
}

void AmsBackendQidi::prepare_lane_repaint_locked(int slot_index, SlotInfo& slot) {
    const auto it = overrides_.find(slot_index);
    helix::ams::clear_lane_only_identity(slot, it == overrides_.end() ? nullptr : &it->second);
}

bool AmsBackendQidi::is_bypass_active() const {
    return false;
}

// --- Path visualisation ---

PathSegment AmsBackendQidi::get_filament_segment() const {
    return PathSegment::NONE;
}

PathSegment AmsBackendQidi::get_slot_filament_segment(int /*slot_index*/) const {
    return PathSegment::NONE;
}

PathSegment AmsBackendQidi::infer_error_segment() const {
    return PathSegment::NONE;
}

// --- Filament operations ---
//
// The write-path is always enabled. Every QIDI write op logs at info (a per-op
// entry log here, plus the raw G-code via execute_gcode) so field behavior is
// fully visible until the gcode protocol is validated on real hardware (#1030).

void AmsBackendQidi::detect_firmware_capabilities() {
    auto& mc = helix::MacroParamCache::instance();

    // Only trust the macro cache once it's actually populated for this printer —
    // PRINT_START is a universal QIDI macro, so its presence confirms discovery
    // has run. Without this guard a discovery-timing race would read an empty
    // cache and wrongly downgrade us off the verified M603 / CLEAR_NOZZLE paths.
    const bool cache_ready =
        mc.has_macro("print_start") || mc.has_macro("m603") || mc.has_macro("t4");
    if (cache_ready) {
        fw_has_m603_ = mc.has_macro("m603");
        fw_has_clear_nozzle_ = mc.has_macro("clear_nozzle");
    }

    // Fingerprint line so every debug bundle records which QIDI firmware variant
    // we're talking to. The macro surface is the version tell (T0-T3 absent on
    // Q2 1.1.1; the 01.01.02 refactor relocates macros). See #1041.
    spdlog::info("{} Firmware fingerprint: cache_ready={} macros{{T0={} T4={} M603={} "
                 "CLEAR_NOZZLE={} UNLOAD_FILAMENT={}}} -> use_m603={} append_clear_nozzle={}",
                 backend_log_tag(), cache_ready, mc.has_macro("t0"), mc.has_macro("t4"),
                 mc.has_macro("m603"), mc.has_macro("clear_nozzle"),
                 mc.has_macro("unload_filament"), fw_has_m603_, fw_has_clear_nozzle_);
}

std::string AmsBackendQidi::build_unload_gcode(int slot_index, int temp) const {
    if (fw_has_m603_) {
        return "M603 S" + std::to_string(temp);
    }
    // Fallback for firmware without M603: drive the box_stepper primitive that
    // UNLOAD_T<n> wraps. Needs the hotend hot like the load path does.
    std::string g = "M109 S" + std::to_string(temp) + "\nEXTRUDER_UNLOAD";
    if (slot_index >= 0) {
        g += " SLOT=slot" + std::to_string(slot_index);
    }
    g += "\nM104 S0";
    return g;
}

AmsError AmsBackendQidi::do_load_filament(int slot_index) {
    spdlog::info("{} load_filament(slot={})", backend_log_tag(), slot_index);
    int load_temp = QIDI_DEFAULT_LOAD_TEMP_C;
    int loaded_other = -1; // slot in the extruder that must be retracted first
    int unload_temp = QIDI_DEFAULT_LOAD_TEMP_C;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto err = validate_slot_index_locked(slot_index); !err.success()) {
            return err;
        }
        const SlotInfo* target = system_info_.get_slot_global(slot_index);
        if (!target) {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                slot_index_bound_locked() - 1);
        }
        load_temp = load_temp_for_slot(*target);
        // If a *different* slot is already in the extruder, retract it first —
        // EXTRUDER_LOAD on top of loaded filament would jam. Compose the two
        // verified stock primitives (unload + EXTRUDER_LOAD) rather than relying
        // on a tool-change macro.
        for (const auto& unit : system_info_.units) {
            for (const auto& s : unit.slots) {
                if (s.status == SlotStatus::LOADED && s.global_index != slot_index) {
                    loaded_other = s.global_index;
                    unload_temp = load_temp_for_slot(s);
                    break;
                }
            }
            if (loaded_other >= 0) {
                break;
            }
        }
    }

    // The stock T<n> macros don't exist for the box's own slots on Q2 firmware
    // (only T4+ for additional boxes) and don't manage hotend temperature, so
    // drive EXTRUDER_LOAD directly. Klipper rejects it below min_extrude_temp,
    // hence the M109 pre-heat; CLEAR_NOZZLE wipes the post-load ooze (no clean
    // is built into EXTRUDER_LOAD); M104 S0 leaves the hotend cooling down.
    std::string seq;
    if (loaded_other >= 0) {
        seq += build_unload_gcode(loaded_other, unload_temp) + "\n";
    }
    seq += "M109 S" + std::to_string(load_temp) + "\n";
    seq += "EXTRUDER_LOAD SLOT=slot" + std::to_string(slot_index) + "\n";
    if (fw_has_clear_nozzle_) {
        // The stock CLEAR_NOZZLE macro makes absolute XY/Z wipe moves with no
        // homing guard of its own (it assumes a print context). A load triggered
        // from idle may be unhomed, which would error "Must home axis first"
        // mid-sequence and leave the hotend hot (the trailing M104 S0 never runs).
        // EXTRUDER_LOAD itself doesn't move the toolhead, so only the wipe needs
        // this — route through ensure_homed_then() so Moonraker's
        // toolhead.homed_axes is checked and G28 runs first when needed.
        seq += "CLEAR_NOZZLE\n";
        seq += "M104 S0";
        return ensure_homed_then(seq);
    }
    seq += "M104 S0";
    return execute_gcode(seq);
}

AmsError AmsBackendQidi::do_unload_filament(int slot_index) {
    spdlog::info("{} unload_filament(slot={})", backend_log_tag(), slot_index);
    int unload_temp = QIDI_DEFAULT_LOAD_TEMP_C;
    int target_slot = slot_index; // for the EXTRUDER_UNLOAD fallback
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_index == -1) {
            // Active slot: find the LOADED one and use its profile temperature.
            const SlotInfo* loaded = nullptr;
            for (const auto& unit : system_info_.units) {
                for (const auto& s : unit.slots) {
                    if (s.status == SlotStatus::LOADED) {
                        loaded = &s;
                        break;
                    }
                }
                if (loaded) {
                    break;
                }
            }
            if (!loaded) {
                return AmsErrorHelper::not_supported("QIDI Box: no slot currently loaded");
            }
            unload_temp = load_temp_for_slot(*loaded);
            target_slot = loaded->global_index;
        } else if (auto err = validate_slot_index_locked(slot_index); !err.success()) {
            return err;
        } else if (const SlotInfo* slot = system_info_.get_slot_global(slot_index)) {
            unload_temp = load_temp_for_slot(*slot);
        } else {
            return AmsErrorHelper::invalid_slot(lane_noun(), slot_index,
                                                slot_index_bound_locked() - 1);
        }
    }

    // M603 is the stock "unload" the QIDI screen runs — it heats, retracts to
    // before the hub and (with the box active) calls E_UNLOAD. UNLOAD_T<n> /
    // UNLOAD_FILAMENT proved inert on Q2 firmware. M603 acts on whatever is in
    // the extruder, so the slot index only selects the unload temperature
    // (used by the EXTRUDER_UNLOAD fallback for firmware without M603).
    return execute_gcode(build_unload_gcode(target_slot, unload_temp));
}

AmsError AmsBackendQidi::do_select_slot(int /*slot_index*/) {
    // QIDI Box doesn't have a "select without loading" operation — load_filament
    // is the only path. Reasonable callers should use load_filament directly.
    return AmsErrorHelper::not_supported("QIDI Box: select_slot not supported (use load_filament)");
}

AmsError AmsBackendQidi::do_change_tool(int tool_number) {
    spdlog::info("{} change_tool(tool={})", backend_log_tag(), tool_number);
    if (tool_number < 0) {
        return AmsErrorHelper::not_supported("QIDI Box: tool number out of range");
    }
    // The bare T<n> macros don't exist for the box's own slots on Q2 firmware, so
    // resolve the tool→slot mapping (value_t<n>) and drive the same verified
    // load path (which retracts any currently-loaded slot first).
    int slot_index = tool_number;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool found = false;
        for (const auto& unit : system_info_.units) {
            for (const auto& s : unit.slots) {
                if (s.mapped_tool == tool_number) {
                    slot_index = s.global_index;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
    }
    return do_load_filament(slot_index);
}

// --- Recovery ---

AmsError AmsBackendQidi::recover() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box recover");
}

AmsError AmsBackendQidi::reset() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box reset");
}

AmsError AmsBackendQidi::cancel() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box cancel");
}

std::optional<helix::ErrorEvent> AmsBackendQidi::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.units.empty())
        return std::nullopt;
    int blocked = -1;
    for (const auto& unit : system_info_.units) {
        for (const auto& slot : unit.slots) {
            if (slot.status == SlotStatus::BLOCKED) {
                blocked = slot.global_index;
                break;
            }
        }
        if (blocked >= 0) {
            break;
        }
    }
    if (blocked < 0)
        return std::nullopt;
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::QIDI;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = lv_tr("Filament System Error");
    // Single translatable string with a {} placeholder — preserves word order in
    // locales where the position label doesn't sit at the head of the sentence.
    e.detail = fmt::format(fmt::runtime(lv_tr("{} is blocked — manual intervention required")),
                           helix::ui::lane_label(lane_noun(), blocked));
    e.sticky = true;
    // A CRITICAL event with empty recovery_actions renders via RecoveryModalPresenter
    // as a button-less ActionPromptModal — non-dismissible UI trap. Provide one
    // dismiss affordance. An empty gcode is the dismiss spelling: the modal
    // closes and sends nothing (#1172; this used to need a Klipper comment).
    // Recovery gcode is absent: QIDI BLOCKED slot clearance is unknown; ships blind
    // (no QIDI hardware). (prestonbrown/helixscreen#1041)
    e.recovery_actions = {{lv_tr("OK"), "", "qidi::dismiss", ""}};
    return e;
}

AmsError AmsBackendQidi::eject_lane(int slot_index) {
    spdlog::info("{} eject_lane(slot={})", backend_log_tag(), slot_index);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto err = validate_slot_index_locked(slot_index); !err.success()) {
            return err;
        }
    }
    // Max 4 dialect: the multi_color_controller state machine owns filament ops and
    // rejects the Q2 box_stepper FORCE_MOVE with "Invalid pin value". Eject via the
    // public MULTI_COLOR_BOX_UNLOAD command instead — no [force_move] required, and it
    // must win over the FORCE_MOVE path even if force_move happens to be enabled. #1083
    if (box_uses_multi_color_) {
        return execute_gcode("MULTI_COLOR_BOX_UNLOAD SLOT=slot" + std::to_string(slot_index));
    }
    if (!fw_force_move_enabled_) {
        return AmsErrorHelper::not_supported(
            "QIDI Box: eject needs [force_move] enable_force_move: True");
    }
    // Manual eject: FORCE_MOVE the lane's box_stepper to push filament out the box
    // side (#1041, QIDI Discord / xenon). Offered only for non-loaded lanes (the
    // context menu gates on !loaded), so no pre-unload is needed here — for a
    // loaded lane the caller runs the unload stack first to park at the hub.
    // Distance is stored as a positive magnitude in settings; negate it here to
    // keep the existing "push back into the box" direction. Fall back to the
    // historic defaults (878 mm / 100 mm/s) if SettingsManager hasn't been
    // initialized — a FORCE_MOVE with VELOCITY=0/DISTANCE=0 is a degenerate no-op
    // that Klipper rejects, so the eject must always carry real numbers.
    int eject_velocity = helix::SettingsManager::instance().get_qidi_eject_velocity();
    int eject_distance_mm = helix::SettingsManager::instance().get_qidi_eject_distance();
    if (eject_velocity <= 0) {
        eject_velocity = 100;
    }
    if (eject_distance_mm <= 0) {
        eject_distance_mm = 878;
    }
    return execute_gcode("FORCE_MOVE STEPPER=\"box_stepper slot" + std::to_string(slot_index) +
                         "\" VELOCITY=" + std::to_string(eject_velocity) +
                         " DISTANCE=" + std::to_string(-eject_distance_mm));
}

// --- Configuration ---

namespace {
// Lowercase copy for case-insensitive comparisons.
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

int AmsBackendQidi::resolve_fila_id(const std::map<int, FilaProfile>& profiles,
                                    const std::string& material, const std::string& name) {
    const std::string want_name = to_lower(name);
    const std::string want_type = to_lower(material);

    // 1. Exact case-insensitive name match (e.g. "ABS Rapido").
    if (!want_name.empty()) {
        for (const auto& [id, p] : profiles) {
            if (to_lower(p.name) == want_name) {
                return id;
            }
        }
    }
    // 2. First profile whose type equals the requested material (e.g. "PLA").
    if (!want_type.empty()) {
        for (const auto& [id, p] : profiles) {
            if (to_lower(p.type) == want_type) {
                return id;
            }
        }
    }
    return 0;
}

int AmsBackendQidi::resolve_color_id(const std::map<int, std::uint32_t>& palette,
                                     std::uint32_t rgb) {
    return helix::nearest_palette_key(palette, rgb, 0);
}

int AmsBackendQidi::resolve_vendor_id(const std::map<int, std::string>& vendors,
                                      const std::string& brand) {
    const std::string want = to_lower(brand);
    if (!want.empty()) {
        for (const auto& [id, name] : vendors) {
            if (to_lower(name) == want) {
                return id;
            }
        }
    }
    // Fall back to the "Generic" vendor when present.
    for (const auto& [id, name] : vendors) {
        if (to_lower(name) == "generic") {
            return id;
        }
    }
    return 0;
}

AmsError AmsBackendQidi::apply_user_edit(int slot_index, const SlotInfo& info,
                                         const helix::ams::Observation& declared) {
    spdlog::info("{} apply_user_edit(slot={}, material='{}', brand='{}')", backend_log_tag(),
                 slot_index, info.material, info.brand);

    int fila_id = 0;
    int color_id = 0;
    int vendor_id = 0;
    // The value to write per identity field: -1 means no write.
    int fila_write = -1;
    int color_write = -1;
    bool have_palette = false;
    bool have_vendors = false;
    std::vector<std::string> staged_echoes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto err = validate_slot_index_locked(slot_index); !err.success()) {
            return err;
        }
        // Stage the override so a later spool swap has something to clear and
        // a restart reloads what the user chose.
        helix::ams::stage_user_override(overrides_, slot_index, info, declared);
        // SlotInfo has no dedicated "QIDI product label" field; callers may put
        // a product name ("ABS Rapido") OR a bare material ("ABS") in .material.
        // Pass it as BOTH the material and the name hint so the resolver tries
        // an exact-name match first, then the type fallback.
        fila_id = resolve_fila_id(fila_profiles_, info.material, info.material);
        have_palette = !color_palette_.empty();
        have_vendors = !vendor_names_.empty();
        const bool color_cleared = !helix::ams::is_declarable_color(info.color_rgb);
        if (have_palette && !color_cleared) {
            color_id = resolve_color_id(color_palette_, info.color_rgb);
        }
        if (have_vendors) {
            vendor_id = resolve_vendor_id(vendor_names_, info.brand);
        }
        const size_t idx = static_cast<size_t>(slot_index);
        const int old_fila = idx < slot_rfid_.size() ? slot_rfid_[idx].filament_id : 0;
        const int old_color = idx < slot_rfid_.size() ? slot_rfid_[idx].color_id : 0;

        // The write per field: a resolved row id, an explicit zero when the
        // user cleared a field the Box still holds an id for, or no write at
        // all when the tables cannot name a row for a field the user set.
        // Skipping the zero would leave the Box restating the old id forever,
        // so the clear never reaches the variables the Box reads back.
        if (fila_id > 0) {
            fila_write = fila_id;
        } else if (info.material.empty() && old_fila != 0) {
            fila_write = 0;
        }
        if (have_palette) {
            if (color_id > 0) {
                color_write = color_id;
            } else if (color_cleared && old_color != 0) {
                color_write = 0;
            }
        }

        // Self-wipe guard. The SAVE_VARIABLEs below echo back as fingerprint
        // changes, which on their own read exactly like a physical spool swap
        // and would erase the override the user just created. The three writes
        // are separate gcodes, so a poll can land between any of them: register
        // every composite the slot may transiently or finally report, built
        // from the old ids and the written ones per field. Each echo consumes
        // only its own entry; any non-matching change consumes them all, so a
        // genuine swap while a write is in flight is still detected. A clear's
        // still-echoing zero composites stay expected alongside these:
        // expect_any_of() accumulates.
        //
        // With no baseline yet there is nothing to guard: the first
        // observation is a baseline and never fires a clear.
        if (const auto base = rfid_tracker_.baseline(slot_index)) {
            std::vector<int> fila_vals{old_fila};
            std::vector<int> color_vals{old_color};
            if (fila_write >= 0 && fila_write != old_fila) {
                fila_vals.push_back(fila_write);
            }
            if (color_write >= 0 && color_write != old_color) {
                color_vals.push_back(color_write);
            }
            staged_echoes =
                expect_own_write_echoes_locked(slot_index, *base, fila_vals, color_vals);
        }

        // Stage before the first dispatch, so an echo racing the writes is
        // judged against this edit. The ids name table rows rather than
        // spelling the values, so each carried field is relocated to what
        // its row will state; a field whose write does not go out (no fila
        // match, empty palette) is pruned, because the Box then keeps stating
        // whatever it had. A zero write states no row, so its field is
        // pruned too: the Box's restatement of id 0 is its own reading.
        // No SAVE_VARIABLE carries a name.
        own_write_echoes_.stage(slot_index, declared);
        if (auto* staged_echo = own_write_echoes_.staged(slot_index)) {
            if (fila_write > 0) {
                const auto row = fila_profiles_.find(fila_write);
                if (row != fila_profiles_.end() && !row->second.type.empty()) {
                    staged_echo->material = row->second.type;
                } else {
                    staged_echo->material.reset();
                }
            } else {
                staged_echo->material.reset();
            }
            if (color_write > 0) {
                staged_echo->color_rgb = color_palette_.at(color_write);
                if (!helix::ams::is_declarable_color(*staged_echo->color_rgb)) {
                    staged_echo->color_rgb.reset();
                }
            } else {
                staged_echo->color_rgb.reset();
            }
            if (vendor_id > 0) {
                const auto row = vendor_names_.find(vendor_id);
                if (row != vendor_names_.end() && !row->second.empty()) {
                    staged_echo->brand = row->second;
                } else {
                    staged_echo->brand.reset();
                }
            } else {
                staged_echo->brand.reset();
            }
            staged_echo->spool_name.reset();
            staged_echo->color_name.reset();
            staged_echo->product_name.reset();
        }
        // The fingerprint tracker above is what separates an echo from a
        // swap; this lane has no boundary token of its own, so the guard
        // stands until a value differs or the swap clear drops it.
        own_write_echoes_.arm(slot_index, std::string{});
    }

    const std::string suffix = std::to_string(slot_index);
    bool wrote_any = false;
    bool dispatched_any = false;

    if (fila_write >= 0) {
        dispatched_any |= execute_gcode("SAVE_VARIABLE VARIABLE=filament_slot" + suffix +
                                        " VALUE=" + std::to_string(fila_write))
                              .success();
        wrote_any = true;
    } else if (!info.material.empty()) {
        spdlog::warn("{} apply_user_edit: no fila match for material='{}' — "
                     "skipping filament_slot write",
                     backend_log_tag(), info.material);
    }
    if (color_write >= 0) {
        dispatched_any |= execute_gcode("SAVE_VARIABLE VARIABLE=color_slot" + suffix +
                                        " VALUE=" + std::to_string(color_write))
                              .success();
        wrote_any = true;
    }
    // vendor_id 0 is the legitimate "Generic" id, so only the empty-map case
    // (have_vendors == false) suppresses the write.
    if (have_vendors) {
        dispatched_any |= execute_gcode("SAVE_VARIABLE VARIABLE=vendor_slot" + suffix +
                                        " VALUE=" + std::to_string(vendor_id))
                              .success();
        wrote_any = true;
    }

    if (!wrote_any) {
        // Nothing resolved — soft success so the UI doesn't show an error, but
        // log loudly since the filas list probably hasn't loaded yet.
        spdlog::warn("{} apply_user_edit(slot={}): nothing mapped (filas list not "
                     "loaded?) — no SAVE_VARIABLE issued",
                     backend_log_tag(), slot_index);
    }
    if (!dispatched_any) {
        // No echo is coming for the writes this edit staged, so release
        // exactly their claims - an earlier edit whose echo is still in
        // flight stays expected, or its landing would read as a swap and
        // wipe the user's data.
        std::lock_guard<std::mutex> lock(mutex_);
        rfid_tracker_.forget_expected(slot_index, staged_echoes);
    }

    if (override_store_) {
        helix::ams::persist_staged_override(override_store_.get(), mutex_, overrides_, slot_index,
                                            backend_log_tag(), "Override");
    }

    return AmsErrorHelper::success();
}

AmsError AmsBackendQidi::sync_external_identity(int slot_index, const SlotInfo& /*info*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto err = validate_slot_index_locked(slot_index); !err.success()) {
        return err;
    }
    // Only a person's edit reaches the box's save_variables, and this backend
    // keeps no other store for a slot's identity, so a synced value has nowhere
    // to go.
    return AmsErrorHelper::success();
}

void AmsBackendQidi::update_slot_weight_impl(int /*slot_index*/, float /*remaining_weight_g*/,
                                             float /*total_weight_g*/, bool /*persist*/) {
    // The box has nowhere to put a weight: its save_variables hold filament,
    // colour and vendor ids alone, and apply_user_edit() writes those. The wrapper has already
    // filed the reading on the lane.
}

AmsError AmsBackendQidi::set_tool_mapping_impl(int tool_number, int slot_index) {
    spdlog::info("{} set_tool_mapping(tool={}, slot={})", backend_log_tag(), tool_number,
                 slot_index);
    if (tool_number < 0) {
        return AmsErrorHelper::not_supported("QIDI Box: tool number out of range");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto err = validate_slot_index_locked(slot_index); !err.success()) {
            return err;
        }
    }
    // box_extras.py stores `value_t<N> = "slot<M>"` — same shape we parse on
    // the read-path. Quote the value to match Klipper's SAVE_VARIABLE syntax
    // for string values.
    return execute_gcode("SAVE_VARIABLE VARIABLE=value_t" + std::to_string(tool_number) +
                         " VALUE=\"slot" + std::to_string(slot_index) + "\"");
}

void AmsBackendQidi::clear_slot_override(int slot_index) {
    std::vector<std::string> staged_echoes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = system_info_.get_slot_global(slot_index);
        if (!slot) {
            spdlog::warn("{} clear_slot_override: no slot entry for index {}", backend_log_tag(),
                         slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, *slot);

        // The zero writes below echo back as fingerprint changes; without an
        // expectation of our own they would read as a spool swap. Same shape
        // as apply_user_edit(): the old ids and the written zero per field,
        // cross-producted. The vendor zero moves no fingerprint, so it needs
        // no expectation of its own.
        if (const auto base = rfid_tracker_.baseline(slot_index)) {
            const size_t idx = static_cast<size_t>(slot_index);
            const int old_fila = idx < slot_rfid_.size() ? slot_rfid_[idx].filament_id : 0;
            const int old_color = idx < slot_rfid_.size() ? slot_rfid_[idx].color_id : 0;
            staged_echoes =
                expect_own_write_echoes_locked(slot_index, *base, {old_fila, 0}, {old_color, 0});
        }
    }

    // The Box has no clear command, so erasing what the firmware remembers is
    // three bare-int writes. The table row ids start at 1, so 0 names no row
    // and the slot reads as no identity. A tagged spool re-populates the slot
    // at the next insert, boot or RFID read - the clear erases what the slot
    // remembers, not what the hardware can read. SAVE_VARIABLE moves nothing,
    // so a print does not gate it: skipping it would leave the ids the next
    // poll repaints straight back over the clear.
    bool dispatched_any = false;
    const std::string suffix = std::to_string(slot_index);
    for (const std::string_view field : {"filament_slot", "color_slot", "vendor_slot"}) {
        dispatched_any |=
            execute_gcode("SAVE_VARIABLE VARIABLE=" + std::string(field) + suffix + " VALUE=0")
                .success();
    }
    if (!dispatched_any) {
        // No echo is coming for the zero writes this clear staged, so
        // release exactly their claims - an earlier edit whose echo is
        // still in flight stays expected.
        std::lock_guard<std::mutex> lock(mutex_);
        rfid_tracker_.forget_expected(slot_index, staged_echoes);
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

bool AmsBackendQidi::check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                                const std::string& observed_uid,
                                                helix::ams::InsertVerdict* verdict) {
    std::string old_uid;
    const auto event = rfid_tracker_.observe(slot_index, observed_uid, &old_uid);

    switch (event) {
    case helix::ams::FingerprintEvent::NoSignal:
        // Nothing stated this frame; the baseline stands so a later real read
        // is compared against what the Box last said.
        return false;
    case helix::ams::FingerprintEvent::Baseline:
        spdlog::debug("{} Slot {} baseline tag fingerprint: {}", backend_log_tag(), slot_index,
                      observed_uid);
        return false;
    case helix::ams::FingerprintEvent::Unchanged:
        return false;
    case helix::ams::FingerprintEvent::OwnWriteEcho:
        // The Box handed back the ids our own SAVE_VARIABLE push wrote. The
        // physical spool never moved, so the user's override stands. The
        // baseline has already advanced to the echoed value, so the next
        // genuine swap is detected against it.
        spdlog::debug("{} Slot {} tag fingerprint {} -> {} matches our own identity push, so "
                      "the override stays",
                      backend_log_tag(), slot_index, old_uid, observed_uid);
        return false;
    case helix::ams::FingerprintEvent::Changed:
        break;
    }

    // The ids a change carried are a reading of the spool now in the slot,
    // and what both composites decode to decides the change: rows spelling
    // the same material and colour are one spool whatever their number, and
    // rows nothing decodes from state nothing. An empty old composite is no
    // reading of the previous spool, so nothing can be compared against it.
    const helix::ams::InsertVerdict insert_verdict = helix::ams::classify_insert(
        old_uid.empty()
            ? std::nullopt
            : std::optional<helix::ams::SpoolEvidence>(fingerprint_evidence_locked(old_uid)),
        fingerprint_evidence_locked(observed_uid));
    if (verdict) {
        *verdict = insert_verdict;
    }

    if (overrides_.find(slot_index) == overrides_.end()) {
        spdlog::debug("{} Slot {} tag fingerprint changed {} -> {} (no override to clear)",
                      backend_log_tag(), slot_index, old_uid, observed_uid);
        return false;
    }

    if (insert_verdict != helix::ams::InsertVerdict::DifferentSpool) {
        spdlog::debug("{} Slot {} tag fingerprint changed {} -> {} but the ids decode to the "
                      "same spool; the override stays",
                      backend_log_tag(), slot_index, old_uid, observed_uid);
        return false;
    }

    spdlog::info("{} Slot {} tag fingerprint changed {} -> {}, clearing override "
                 "(physical spool swap detected)",
                 backend_log_tag(), slot_index, old_uid, observed_uid);

    // Delegate erase + field reset + clear_async to the shared helper so
    // hardware-event clears and user-initiated clears share one field-reset
    // policy. Caller already holds mutex_.
    clear_override_locked(slot_index, slot);
    return true;
}

helix::ams::SpoolEvidence
AmsBackendQidi::fingerprint_evidence_locked(const std::string& fingerprint) const {
    helix::ams::SpoolEvidence evidence;
    const auto sep = fingerprint.find('|');
    if (sep == std::string::npos) {
        return evidence;
    }
    int fila_id = 0;
    int color_id = 0;
    try {
        fila_id = std::stoi(fingerprint.substr(0, sep));
        color_id = std::stoi(fingerprint.substr(sep + 1));
    } catch (const std::exception&) {
        return evidence;
    }
    // The composite is the Box's own read, so the spool carries no UID-capable
    // tag rather than one not read yet, and the material/colour fields decide.
    evidence.tag_read_complete = true;
    const auto fila_row = fila_profiles_.find(fila_id);
    const auto color_row = color_palette_.find(color_id);
    const bool fila_decoded = fila_row != fila_profiles_.end() && !fila_row->second.type.empty();
    const bool color_decoded =
        color_row != color_palette_.end() && helix::ams::is_declarable_color(color_row->second);
    // The tables come from an async officiall_filas_list.cfg fetch that can
    // land late or fail. An id the tables cannot decode is still a stable
    // identity the Box chose, so fall back to the raw ids: a differing
    // composite stays a swap instead of decoding to empty on both sides and
    // reading as no evidence, which would keep the override forever.
    if ((fila_id > 0 && !fila_decoded) || (color_id > 0 && !color_decoded)) {
        evidence.tag_uid = fmt::format("fila#{}|color#{}", fila_id, color_id);
        return evidence;
    }
    if (fila_decoded) {
        evidence.material = fila_row->second.type;
    }
    if (color_decoded) {
        evidence.color_rgb = color_row->second;
    }
    return evidence;
}

void AmsBackendQidi::clear_override_locked(int slot_index, SlotInfo& slot) {
    // Caller must hold mutex_. Qidi field policy: the clear blanks everything
    // the slot showed so it reads as no identity the moment the request lands;
    // the parse's paint (or its else-arms, once the zero writes echo back)
    // restates whatever the Box still reports.
    overrides_.erase(slot_index);
    // What the Box states from here on is its own word: both the user's clear
    // and the fingerprint-change swap funnel land here before this frame's
    // cache files, so a swap's identity is not withheld as a lingering echo.
    own_write_echoes_.abandon(slot_index);
    // The lane's own records go with it: the erase above and this are one
    // clear in two stores, and a clear that reached only one would leave
    // resolve() still reporting the identity just removed.
    helix::ams::reset_lane_to_machine_readings(lane_id(slot_index));

    slot.material.clear();
    slot.color_rgb = 0;
    slot.nozzle_temp_min = 0;
    slot.nozzle_temp_max = 0;
    slot.brand.clear();
    slot.clear_spoolman_link();
    slot.remaining_weight_g = -1.0f;
    slot.total_weight_g = -1.0f;
    slot.color_name.clear();
    slot.catalog_id.clear();
    slot.product_name.clear();

    if (override_store_) {
        // Capture by value: clear_async's Moonraker callback can fire after
        // this returns and after the backend itself is gone.
        const std::string tag = backend_log_tag();
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
            }
        });
    }
}

std::vector<std::string>
AmsBackendQidi::expect_own_write_echoes_locked(int slot_index, const std::string& base,
                                               const std::vector<int>& fila_vals,
                                               const std::vector<int>& color_vals) {
    std::vector<std::string> expected;
    for (const int f : fila_vals) {
        for (const int c : color_vals) {
            const std::string fp = qidi_slot_fingerprint(f, c);
            if (!fp.empty() && fp != base) {
                expected.push_back(fp);
            }
        }
    }
    return rfid_tracker_.expect_any_of(slot_index, std::move(expected));
}

// --- Bypass ---

AmsError AmsBackendQidi::enable_bypass() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box enable_bypass");
}

AmsError AmsBackendQidi::disable_bypass() {
    spdlog::warn("{} {} not yet implemented", backend_log_tag(), __func__);
    return AmsErrorHelper::not_supported("QIDI Box disable_bypass");
}

// --- Dryer / box-heater control (issue #1019) ---

DryerInfo AmsBackendQidi::get_dryer_info(int unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit < 0 || unit >= static_cast<int>(dryer_info_.size())) {
        return DryerInfo{.supported = false};
    }
    DryerInfo out = dryer_info_[static_cast<size_t>(unit)];
    const std::time_t end = dry_end_epoch_[static_cast<size_t>(unit)];
    if (end > 0) {
        const std::time_t now = now_fn_();
        const int remaining = static_cast<int>((end - now) / 60);
        out.remaining_min = remaining > 0 ? remaining : 0;
        out.active = remaining > 0;
    }
    return out;
}

AmsError AmsBackendQidi::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)fan_pct;
    const int box = unit + 1; // status objects are 1-indexed (heater_box1)
    spdlog::info("{} start_drying(temp={}C, dur={}min, box={})", backend_log_tag(), temp_c,
                 duration_min, box);

    float min_temp, max_temp;
    int max_duration;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (unit < 0 || unit >= static_cast<int>(dryer_info_.size())) {
            return AmsErrorHelper::not_supported("Dryer");
        }
        const size_t u = static_cast<size_t>(unit);
        min_temp = dryer_info_[u].min_temp_c;
        max_temp = dryer_info_[u].max_temp_c;
        max_duration = dryer_info_[u].max_duration_min;
    }
    if (temp_c < min_temp || temp_c > max_temp) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Temperature out of range: " + std::to_string(temp_c),
                        lv_tr("Invalid temperature"),
                        fmt::format(lv_tr("Set temperature between {}°C and {}°C"),
                                    static_cast<int>(min_temp), static_cast<int>(max_temp)));
    }
    if (duration_min <= 0 || duration_min > max_duration) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Duration out of range: " + std::to_string(duration_min),
                        lv_tr("Invalid duration"),
                        fmt::format(lv_tr("Set duration between 1 and {} minutes"), max_duration));
    }

    const int temp_i = static_cast<int>(temp_c);
    bool timer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (unit < 0 || unit >= static_cast<int>(dryer_info_.size())) {
            return AmsErrorHelper::not_supported("Dryer");
        }
        dryer_info_[static_cast<size_t>(unit)].target_temp_c = temp_c;
        dryer_info_[static_cast<size_t>(unit)].duration_min = duration_min;
        timer = drying_timer_supported_;
    }
    if (timer) {
        int hours = duration_min / 60;
        if (hours < 1) {
            hours = 1;
        }
        return execute_gcode("ENABLE_BOX_DRY BOX=" + std::to_string(box) + " TEMP=" +
                             std::to_string(temp_i) + " END_TIME=" + std::to_string(hours));
    }
    return execute_gcode("SET_HEATER_TEMPERATURE HEATER=heater_box" + std::to_string(box) +
                         " TARGET=" + std::to_string(temp_i));
}

AmsError AmsBackendQidi::stop_drying(int unit) {
    const int box = unit + 1;
    spdlog::info("{} stop_drying(box={})", backend_log_tag(), box);
    bool timer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (unit < 0 || unit >= static_cast<int>(dryer_info_.size())) {
            return AmsErrorHelper::not_supported("Dryer");
        }
        const size_t u = static_cast<size_t>(unit);
        dry_end_epoch_[u] = 0;
        dryer_info_[u].active = false;
        dryer_info_[u].target_temp_c = 0.0f;
        dryer_info_[u].remaining_min = 0;
        dryer_info_[u].duration_min = 0;
        timer = drying_timer_supported_;
    }
    if (timer) {
        return execute_gcode("DISABLE_BOX_DRY BOX=" + std::to_string(box));
    }
    return execute_gcode("SET_HEATER_TEMPERATURE HEATER=heater_box" + std::to_string(box) +
                         " TARGET=0");
}

} // namespace helix

#endif // HELIX_HAS_QIDI
