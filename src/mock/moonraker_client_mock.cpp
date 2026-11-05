// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock.h"

#include "../tests/mocks/mock_printer_state.h"
#include "app_globals.h"
#include "chamber_heater_backend.h"
#include "color_utils.h"
#include "core/gcode_filename.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "env_knobs.h"
#include "gcode_parser.h"
#include "hardware_object_table.h"
#include "helix_thread.h"
#include "home_layout_share.h"
#include "input_sensor_types.h"
#include "macro_param_cache.h"
#include "mock_persona.h"
#include "mock_planted_gcodes.h"
#include "moonraker_client_mock_internal.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "shaper_response.h"
#include "simulated_clock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <unordered_set>

using namespace helix;
using mock_internal::mock_heater_duty;

// Generate mock geometry for exclude_object status updates.
// Spreads objects in a grid inset from the edges of the persona's bed, matching
// the `center` + `polygon` shape Moonraker reports for EXCLUDE_OBJECT_DEFINE.
static json mock_object_entry(const std::string& name, int index, int total,
                              const helix::mock::PersonaDescriptor& persona) {
    constexpr float kInset = 20.0f; // keep the plate margin visible in the map
    const float bed_w = static_cast<float>(persona.axis_max.x - persona.axis_min.x) - 2.0f * kInset;
    const float bed_h = static_cast<float>(persona.axis_max.y - persona.axis_min.y) - 2.0f * kInset;
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(std::max(1, total)))));
    int rows = std::max(1, (total + cols - 1) / cols);
    int row = index / cols, col = index % cols;
    float cell_w = bed_w / static_cast<float>(cols);
    float cell_h = bed_h / static_cast<float>(rows);
    float cx =
        static_cast<float>(persona.axis_min.x) + kInset + (static_cast<float>(col) + 0.5f) * cell_w;
    float cy =
        static_cast<float>(persona.axis_min.y) + kInset + (static_cast<float>(row) + 0.5f) * cell_h;
    float hw = cell_w * 0.35f, hh = cell_h * 0.35f;
    return {{"name", name},
            {"center", {cx, cy}},
            {"polygon",
             {{cx - hw, cy - hh}, {cx + hw, cy - hh}, {cx + hw, cy + hh}, {cx - hw, cy + hh}}}};
}

namespace {

/// connect()'s pause before CONNECTED, in simulated ms; see set_connect_delay_ms().
std::atomic<int> g_connect_delay_ms{250};

/// Slicer-style plate names for HELIX_MOCK_EXCLUDE_OBJECTS. Real slicers emit
/// `<model>_id_<n>_copy_<n>`, so these are long enough to exercise label
/// truncation/wrapping in the side list rather than flattering it.
constexpr const char* kMockExcludeObjectNames[] = {
    "Benchy_id_0_copy_0",        "Calibration_Cube_id_1_copy_0", "Bracket_Left_id_2_copy_0",
    "Bracket_Right_id_3_copy_0", "Gear_Housing_id_4_copy_0",     "Vase_id_5_copy_0",
    "Spool_Holder_id_6_copy_0",  "Cable_Clip_id_7_copy_0",       "Hinge_Pin_id_8_copy_0",
    "Knob_id_9_copy_0",          "Fan_Duct_id_10_copy_0",        "Strain_Relief_id_11_copy_0"};

constexpr int kMaxMockExcludeObjectCount =
    static_cast<int>(sizeof(kMockExcludeObjectNames) / sizeof(kMockExcludeObjectNames[0]));

/// Objects published for a bare `HELIX_MOCK_EXCLUDE_OBJECTS=1`. Enough to fill a
/// side list past one screen on the short landscape panel without being absurd.
constexpr int kDefaultMockExcludeObjectCount = 5;

// webhooks.state_message as Klipper reports it after M112 (klippy.py's
// message_shutdown appended to the reason).
constexpr const char* kM112ShutdownMessage =
    "Shutdown due to M112 command\n"
    "Once the underlying issue is corrected, use the\n"
    "\"FIRMWARE_RESTART\" command to reset the firmware, reload the\n"
    "config, and restart the host software.\n"
    "Printer is shutdown\n";

} // namespace

// Delegating constructor - uses default speedup of 1.0
std::vector<WebcamInfo> MoonrakerClientMock::parse_mock_webcams(const char* spec) {
    std::vector<WebcamInfo> cams;
    if (!spec || !*spec)
        return cams;
    std::string rest = spec;
    size_t index = 0;
    while (!rest.empty()) {
        size_t comma = rest.find(',');
        std::string item = rest.substr(0, comma);
        rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
        if (item.empty())
            continue;
        WebcamInfo cam;
        size_t colon = item.find(':');
        cam.name = item.substr(0, colon);
        cam.service = colon == std::string::npos ? "mjpegstreamer" : item.substr(colon + 1);
        std::string path = index == 0 ? "/webcam/" : "/webcam" + std::to_string(index + 1) + "/";
        cam.stream_url = path + "?action=stream";
        cam.snapshot_url = path + "?action=snapshot";
        cams.push_back(std::move(cam));
        ++index;
    }
    return cams;
}

MoonrakerClientMock::MoonrakerClientMock(PrinterType type) : MoonrakerClientMock(type, 1.0) {}

MoonrakerClientMock::MoonrakerClientMock(PrinterType type, double speedup_factor)
    : printer_type_(type) {
    if (const char* kin_env = std::getenv("HELIX_MOCK_KINEMATICS"); kin_env && kin_env[0]) {
        kinematics_override_ = kin_env;
    }
    // Creality firmware declares the bed in a macro's variables; detection reads
    // it to tell the K2 Plus from the K2 Pro, whose stepper travel overshoots alike.
    if (type == PrinterType::CREALITY_K2_PLUS) {
        set_config_settings_section("gcode_macro product_param", {{"variable_bed_size_x", "350"},
                                                                  {"variable_bed_size_y", "350"}});
    }
    // A pick of T0 failed before the screen connected: the hot end is on the
    // carriage while its dock switch still reads seated, so pin_watch cannot
    // tell what is mounted, and the controller holds the failure latched.
    if (const char* fail = std::getenv("HELIX_MOCK_MEDUSA_SWAP_FAIL"); fail && *fail) {
        medusa_swap_fail_tool_ = helix::text_io::parse_leading<int>(fail).value_or(-1);
    }
    if (const char* redock = std::getenv("HELIX_MOCK_MEDUSA_REDOCK_S"); redock && *redock) {
        // One notification is ~1s of simulated time.
        medusa_redock_notifications_ = helix::text_io::parse_leading<int>(redock).value_or(-1);
    }
    if (helix::env_flag("HELIX_MOCK_MEDUSA_SENSOR_ERROR")) {
        medusa_.stuck_dock = 0;
        medusa_.target_tool = 0;
        medusa_.feeder_open = true;
        medusa_.op_failed = true;
        medusa_.last_error = "MHC_SET: sensors did not confirm T0";
        medusa_.redock_ticks = medusa_redock_notifications_;
    }
    dragonbreath_fault_ = helix::env_flag("HELIX_MOCK_DRAGONBREATH_FAULT");
    dragonbreath_offline_ = helix::env_flag("HELIX_MOCK_DRAGONBREATH_OFFLINE");
    dragonbreath_external_ = helix::env_flag("HELIX_MOCK_DRAGONBREATH_EXTERNAL");
    if (const char* reject = std::getenv("HELIX_MOCK_DRAGONBREATH_REJECT"); reject && *reject) {
        dragonbreath_reject_code_ = reject;
    }
    panda_breath_offline_ = helix::env_flag("HELIX_MOCK_PANDA_BREATH_OFFLINE");
    panda_breath_auto_ = helix::env_flag("HELIX_MOCK_PANDA_BREATH_AUTO");
    // Another screen's toolhead offset calibration, already holding the state at connect.
    nozzle_cal_active_ = helix::env_flag("HELIX_MOCK_NOZZLE_CAL_HELD");

    // HELIX_MOCK_HOME_LAYOUT=<json file> pre-seeds the database record the
    // printer's own screen publishes its home layout to.
    if (const char* layout_path = std::getenv("HELIX_MOCK_HOME_LAYOUT");
        layout_path && *layout_path) {
        std::ifstream layout_file(layout_path);
        json layout = layout_file.good() ? json::parse(layout_file, nullptr, false) : json();
        if (layout.is_discarded() || !layout.is_object()) {
            spdlog::warn("[MoonrakerClientMock] HELIX_MOCK_HOME_LAYOUT '{}' is not a readable "
                         "JSON object",
                         layout_path);
        } else {
            mock_db_set(helix::home_layout::kNamespace, helix::home_layout::kKey, layout);
            spdlog::info("[MoonrakerClientMock] Pre-seeded the published home layout from '{}'",
                         layout_path);
        }
    }

    // Initialize idle timeout tracking
    last_activity_time_ = std::chrono::steady_clock::now();

    // Set speedup factor (clamped)
    speedup_factor_.store(helix::sim::clamp_speed(speedup_factor));

    spdlog::debug("[MoonrakerClientMock] Created with printer type: {}, speedup: {}x",
                  static_cast<int>(type), speedup_factor_.load());

    // Register method handlers for all RPC domains
    mock_internal::register_file_handlers(method_handlers_);
    mock_internal::register_print_handlers(method_handlers_);
    mock_internal::register_object_handlers(method_handlers_);
    mock_internal::register_history_handlers(method_handlers_);
    mock_internal::register_server_handlers(method_handlers_);
    mock_internal::register_queue_handlers(method_handlers_);
    mock_internal::register_plr_handlers(method_handlers_);
    spdlog::debug("[MoonrakerClientMock] Registered {} RPC method handlers",
                  method_handlers_.size());

    // MedusaHC drives the real AmsBackendToolChanger, and klipper-toolchanger
    // reports no material, colour, brand or weight - the override store is the
    // whole of filament identity there. Without these records every lane renders
    // at kAmsDefaultSlotColor, which makes colour and ghost bugs invisible.
    // Outer key style is T<n> (lane_key_style_for), inner "lane" is 0-based.
    if (mock_medusa_variant() != MedusaVariant::NONE) {
        // Each lane mirrors a spool from the mock Spoolman inventory - id,
        // vendor, material, colour and weights - so the active-spool card and
        // the lane it names cannot describe different filament.
        struct Lane {
            int spoolman_id;
            const char* material;
            const char* brand;
            const char* spool_name;
            const char* color;
            double remaining_g;
        };
        static constexpr Lane kLanes[] = {
            {2, "Silk PLA", "eSUN", "Silk Blue", "#26DCD9", 750.0},
            {4, "ABS", "Flashforge", "Fire Engine Red", "#D20000", 100.0},
            {13, "PETG", "Bambu Lab", "Translucent Green PETG", "#29A261", 1000.0},
            {5, "PETG", "Kingroon", "Signal Yellow", "#F4E111", 1000.0},
            {3, "ASA", "Elegoo", "Pop Blue", "#00AEFF", 500.0},
            {15, "PLA", "Bambu Lab", "Red PLA", "#C12E1F", 1000.0},
        };
        static_assert(std::size(kLanes) == kMedusaMaxTools);
        for (int i = 0; i < medusa_tool_count(); ++i) {
            const Lane& l = kLanes[i];
            mock_db_set("lane_data",
                        "T" + std::to_string(i), // DISPLAY_NUMBERING_OK: database key mirrors
                                                 // the T<n> wire format, not a display label
                        json{{"lane", std::to_string(i)},
                             {"spoolman_id", l.spoolman_id},
                             {"material", l.material},
                             {"brand", l.brand},
                             {"spool_name", l.spool_name},
                             {"color", l.color},
                             {"remaining_weight_g", l.remaining_g},
                             {"total_weight_g", 1000.0}});
        }
    }

    // Filament in the head that no lane claims: what a firmware that lost its
    // loaded-lane record leaves behind.
    if (is_mock_ifs_module() && std::getenv("HELIX_MOCK_IFS_UNACCOUNTED") != nullptr) {
        ifs_module_head_loaded_.store(true);
    }
    if (is_mock_ifs_module()) {
        // Which lanes hold filament ("1,3"; "none" for none), and which lane the
        // module's ifs_at_hub record names: the states a toolhead unload has to
        // find its lane from.
        if (const char* lanes = std::getenv("HELIX_MOCK_IFS_LANES")) {
            uint8_t mask = 0;
            for (const char* c = lanes; *c != '\0'; ++c) {
                if (*c >= '1' && *c <= '4') {
                    mask |= static_cast<uint8_t>(1u << (*c - '1'));
                }
            }
            ifs_module_presence_.store(mask);
        }
        if (const char* at_hub = std::getenv("HELIX_MOCK_IFS_AT_HUB")) {
            if (at_hub[0] >= '1' && at_hub[0] <= '4' && at_hub[1] == '\0') {
                ifs_module_at_hub_.store(at_hub[0] - '0');
            }
        }
    }

    // OpenAMS reads no identity off its spools: color and material come from the
    // override store, which reads the shared lane_data namespace (laneN keys,
    // N = 1-based global slot; inner "lane" is 0-based).
    if (is_mock_openams() && openams_plugin_units() && !openams_fleet_lane_units()) {
        openams_loaded_slot_ = 0;
        openams_fault_active_ = std::getenv("HELIX_MOCK_OPENAMS_FAULT") != nullptr;
        // Every spool is known to the override store as it would be on a real
        // printer; the shared-lane shape seeds the first three.
        static const char* const COLORS[] = {"#E8E8E8", "#3CE05A", "#303030"};
        static const char* const NAMES[] = {"White", "Green", "Dark Gray"};
        for (int i = 0; i < 3; ++i) {
            mock_db_set("lane_data", "lane" + std::to_string(i + 1),
                        json{{"lane", std::to_string(i)},
                             {"color", COLORS[i]},
                             {"color_name", NAMES[i]},
                             {"material", "ASA"},
                             {"helix_material", "ASA"},
                             {"helix_locked_color", true},
                             {"helix_locked_material", true}});
        }
    } else if (is_mock_openams() && openams_fleet_lane_units()) {
        // One bay loaded, on the third unit (slot 5); its spool is known to the store.
        openams_loaded_slot_ = 5;
        openams_fault_active_ = std::getenv("HELIX_MOCK_OPENAMS_FAULT") != nullptr;
        for (int i = 0; i < kOpenAmsMaxUnits; ++i) {
            openams_dryers_[i].chamber_c = openams_ambient_c(i);
        }
        // The sixth unit is mid-cycle: 55 C, 3 h 25 min left.
        openams_dryers_[5].target_c = 55.0;
        openams_dryers_[5].remaining_s = 12300.0;
        openams_dryers_[5].chamber_c = 48.0;
        mock_db_set("lane_data", "lane6",
                    json{{"lane", "5"},
                         {"color", "#3CE05A"},
                         {"color_name", "Green"},
                         {"material", "PETG"},
                         {"helix_material", "PETG"},
                         {"helix_locked_color", true},
                         {"helix_locked_material", true}});
    } else if (is_mock_openams()) {
        mock_db_set("lane_data", "lane3",
                    json{{"lane", "2"},
                         {"color", "#3CE05A"},
                         {"color_name", "Green"},
                         {"material", "ASA"},
                         {"helix_material", "ASA"},
                         {"helix_locked_color", true},
                         {"helix_locked_material", true}});
        mock_db_set("lane_data", "lane4",
                    json{{"lane", "3"},
                         {"color", "#303030"},
                         {"color_name", "Dark Gray"},
                         {"material", "ASA"},
                         {"helix_material", "ASA"},
                         {"vendor", "Polymaker"},
                         {"vendor_name", "Polymaker"},
                         {"helix_locked_color", true},
                         {"helix_locked_material", true}});
    }

    // Populate hardware immediately (available for wizard without calling discover_printer())
    populate_hardware();
    spdlog::debug(
        "[MoonrakerClientMock] Hardware populated: {} heaters, {} sensors, {} fans, {} LEDs",
        discovery_.heaters().size(), discovery_.sensors().size(), discovery_.fans().size(),
        discovery_.leds().size());

    // Generate synthetic bed mesh data
    generate_mock_bed_mesh();

    // Pre-populate so capabilities are available for UI tests that skip connect()
    populate_capabilities();

    // Check HELIX_MOCK_SPOOLMAN env var for Spoolman availability
    const char* spoolman_env = std::getenv("HELIX_MOCK_SPOOLMAN");
    if (spoolman_env && (std::string(spoolman_env) == "0" || std::string(spoolman_env) == "off")) {
        mock_spoolman_enabled_ = false;
        spdlog::info("[MoonrakerClientMock] Mock Spoolman disabled via HELIX_MOCK_SPOOLMAN=0");
    }

    // HELIX_MOCK_WEBCAMS="Name[:service],Name[:service],..." — the webcam list
    // discovery publishes, so the camera widget's source picker has something
    // to offer. Unset: one unnamed MJPEG feed, as a printer with a single
    // stock webcam presents. Each entry gets its own /webcamN/ path so a log
    // line shows which one a view is streaming. A service other than an MJPEG
    // family ("webrtc-go2rtc", ...) makes that entry snapshot-only.
    mock_webcams_ = parse_mock_webcams(std::getenv("HELIX_MOCK_WEBCAMS"));
    if (!mock_webcams_.empty()) {
        spdlog::info("[MoonrakerClientMock] {} mock webcam(s) via HELIX_MOCK_WEBCAMS",
                     mock_webcams_.size());
    }

    // HELIX_MOCK_BELT_A_HZ / HELIX_MOCK_BELT_B_HZ — the two belt paths'
    // simulated resonance peaks. Defaults (110/98) sit in "Poor match"
    // territory so the tuning loop is visible from the first sweep.
    if (const char* a_env = std::getenv("HELIX_MOCK_BELT_A_HZ")) {
        if (auto v = text_io::parse_leading<float>(a_env); v && *v > 0.0f) {
            belt_peaks_hz_[0] = *v;
        }
    }
    if (const char* b_env = std::getenv("HELIX_MOCK_BELT_B_HZ")) {
        if (auto v = text_io::parse_leading<float>(b_env); v && *v > 0.0f) {
            belt_peaks_hz_[1] = *v;
        }
    }

    // HELIX_MOCK_BELT_TUNE_SCRIPT=<A|B>:<hz|flat>,... — that path's peak for
    // each sweep after its first.
    if (const char* script_env = std::getenv("HELIX_MOCK_BELT_TUNE_SCRIPT")) {
        apply_belt_tune_script(script_env);
    }

    // HELIX_MOCK_BELT_RANGE=<min>-<max> — the [resonance_tester] sweep range
    // the mock reports and sweeps, so a real capture fed through
    // HELIX_MOCK_BELT_CSV_A/_B is analysed over the band it was measured in.
    if (const char* range_env = std::getenv("HELIX_MOCK_BELT_RANGE")) {
        const std::string range(range_env);
        const auto dash = range.find('-', 1);
        const auto lo = text_io::parse_leading<double>(range.substr(0, dash));
        const auto hi = dash == std::string::npos
                            ? std::nullopt
                            : text_io::parse_leading<double>(range.substr(dash + 1));
        if (lo && hi && *lo > 0.0 && *hi > *lo) {
            resonance_min_freq_ = *lo;
            resonance_max_freq_ = *hi;
        }
    }

    // HELIX_MOCK_BELT_FAIL — reproduce a run that stalls, loses its CSV, uses
    // per-chip output, dies on an adxl345 error, or speaks Kalico's dialect.
    if (const char* fail_env = std::getenv("HELIX_MOCK_BELT_FAIL")) {
        const std::string fail(fail_env);
        if (fail == "stall") {
            belt_failure_ = BeltMockFailure::STALL;
        } else if (fail == "nofile") {
            belt_failure_ = BeltMockFailure::NO_FILE;
        } else if (fail == "multichip") {
            belt_failure_ = BeltMockFailure::MULTICHIP;
        } else if (fail == "error") {
            belt_failure_ = BeltMockFailure::ERROR;
        } else if (fail == "kalico") {
            belt_failure_ = BeltMockFailure::KALICO;
        }
        if (belt_failure_ != BeltMockFailure::NONE) {
            spdlog::info("[MoonrakerClientMock] TEST_RESONANCES failure mode '{}' via "
                         "HELIX_MOCK_BELT_FAIL",
                         fail);
        }
    }

    // HELIX_MOCK_DISPLAY_MESSAGE=<text> — start with an M117 message already set, as a
    // printer whose macros left one before the screen connected. It counts as a sent
    // M117, so it shows until the next M117 replaces or clears it.
    if (const char* message_env = std::getenv("HELIX_MOCK_DISPLAY_MESSAGE");
        message_env && message_env[0]) {
        std::lock_guard<std::mutex> lock(display_message_mutex_);
        display_message_ = message_env;
        display_message_set_ = true;
        spdlog::info("[MoonrakerClientMock] HELIX_MOCK_DISPLAY_MESSAGE — display message \"{}\"",
                     display_message_);
    }

    // HELIX_MOCK_EXCLUDE_OBJECTS=<n>|1 — publish a synthetic multi-object plate at
    // print start. The stock test G-codes declare a single EXCLUDE_OBJECT_DEFINE,
    // and the objects button is gated on two or more, so without this the
    // exclude-object map and side list are unreachable under --test.
    const char* exclude_env = std::getenv("HELIX_MOCK_EXCLUDE_OBJECTS");
    if (exclude_env && exclude_env[0] && std::string(exclude_env) != "0" &&
        std::string(exclude_env) != "off") {
        int requested = static_cast<int>(std::strtol(exclude_env, nullptr, 10));
        // "1"/"on"/"yes" mean "give me a plausible plate", not "give me one
        // object" — one object would leave the button hidden, which is the very
        // thing the flag exists to defeat.
        if (requested <= 1) {
            requested = kDefaultMockExcludeObjectCount;
        }
        mock_exclude_object_count_ = std::clamp(requested, 2, kMaxMockExcludeObjectCount);
        spdlog::info("[MoonrakerClientMock] HELIX_MOCK_EXCLUDE_OBJECTS={} — will publish {} "
                     "synthetic exclude_object entries at print start",
                     exclude_env, mock_exclude_object_count_);
    }

    // Set up bed mesh callback to handle incoming status updates
    // This ensures dispatch_status_update updates the mock's internal bed mesh state
    set_bed_mesh_callback([this](const json& bed_mesh) { parse_incoming_bed_mesh(bed_mesh); });
}

void MoonrakerClientMock::set_connect_delay_ms(int ms) {
    g_connect_delay_ms.store(ms);
}

int MoonrakerClientMock::connect_delay_ms() {
    return g_connect_delay_ms.load();
}

void MoonrakerClientMock::set_simulation_speedup(double factor) {
    double clamped = helix::sim::clamp_speed(factor);
    speedup_factor_.store(clamped);
    spdlog::info("[MoonrakerClientMock] Simulation speedup set to {}x", clamped);
}

double MoonrakerClientMock::get_simulation_speedup() const {
    return speedup_factor_.load();
}

helix::sim::SimSpeed MoonrakerClientMock::sim_speed() const {
    return helix::sim::SimSpeed::of(speedup_factor_.load());
}

bool MoonrakerClientMock::arm_event_replay(const std::string& json_path) {
    replay_events_.clear();
    replay_next_ = 0;

    std::ifstream f(json_path);
    if (!f.good()) {
        spdlog::warn("[Mock Replay] Cannot open replay script '{}'", json_path);
        return false;
    }
    try {
        json script = json::parse(f);
        for (const auto& ev : script.at("events")) {
            ReplayEvent re;
            re.t_ms = ev.value("t", 0);
            if (ev.value("type", "") == "gcode_response") {
                re.is_gcode = true;
                re.line = ev.at("line").get<std::string>();
            } else {
                re.object = ev.at("object").get<std::string>();
                re.payload = ev.at("payload");
            }
            replay_events_.push_back(std::move(re));
        }
        // Optional: serve configfile.settings.bed_mesh.probe_count so the
        // collector's denominator query answers like the real printer did.
        if (script.contains("config_probe_count") && script["config_probe_count"].is_array() &&
            script["config_probe_count"].size() == 2) {
            set_config_bed_mesh_probe_count(script["config_probe_count"][0].get<int>(),
                                            script["config_probe_count"][1].get<int>());
        }
    } catch (const std::exception& e) {
        replay_events_.clear();
        spdlog::warn("[Mock Replay] Failed to parse '{}': {}", json_path, e.what());
        return false;
    }

    if (replay_events_.empty()) {
        spdlog::warn("[Mock Replay] Script '{}' carries no events", json_path);
        return false;
    }
    spdlog::info("[Mock Replay] Armed {} events from '{}' (span {:.0f}s)", replay_events_.size(),
                 json_path, static_cast<double>(replay_events_.back().t_ms) / 1000.0);
    return true;
}

void MoonrakerClientMock::start_replay_timer() {
    if (replay_events_.empty() || replay_timer_ != nullptr) {
        return;
    }
    // Grace before the first event: the app re-dispatches its cached status
    // snapshot after subsystem init (~1.4s in), which would otherwise land
    // after the script's first print_stats edge and stamp standby over it.
    replay_start_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    spdlog::info("[Mock Replay] Starting: {} events at {:.0f}x speedup", replay_events_.size(),
                 get_simulation_speedup());
    replay_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<MoonrakerClientMock*>(lv_timer_get_user_data(t));
            self->pump_replay();
        },
        20, this);
}

void MoonrakerClientMock::pump_replay() {
    const auto now = std::chrono::steady_clock::now();
    if (now < replay_start_) {
        return;
    }
    // The capture's timestamps are simulated time, so the cursor into it is how
    // much simulated time the real wait so far bought.
    const auto due = sim_speed().accelerate_progress(now - replay_start_);
    const uint64_t due_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(due).count());

    while (replay_next_ < replay_events_.size() && replay_events_[replay_next_].t_ms <= due_ms) {
        fire_replay_event(replay_events_[replay_next_]);
        ++replay_next_;
    }
    if (replay_next_ >= replay_events_.size() && replay_timer_ != nullptr) {
        spdlog::info("[Mock Replay] Finished: {} events fired", replay_events_.size());
        lv_timer_t* timer = replay_timer_;
        replay_timer_ = nullptr;
        lv_timer_delete(timer);
    }
}

void MoonrakerClientMock::fire_replay_event(const ReplayEvent& event) {
    if (event.is_gcode) {
        json msg = {{"method", "notify_gcode_response"}, {"params", {event.line}}};
        dispatch_method_callback("notify_gcode_response", msg);
        return;
    }

    // Keep the mock's own simulators in step with the capture so their later
    // pushes (idle-timeout steppers, cooldown transitions, snapshots rebuilt
    // from internal state) agree with the script instead of fighting it.
    if (event.object == "print_stats" && event.payload.contains("state")) {
        const std::string state = event.payload.value("state", "");
        int mapped = 0; // standby
        if (state == "printing") {
            mapped = 1;
        } else if (state == "paused") {
            mapped = 2;
        } else if (state == "complete") {
            mapped = 3;
        } else if (state == "cancelled") {
            mapped = 4;
        } else if (state == "error") {
            mapped = 5;
        }
        print_state_.store(mapped);
        if (event.payload.contains("filename") && event.payload["filename"].is_string()) {
            std::lock_guard<std::mutex> lock(print_mutex_);
            print_filename_ = event.payload["filename"].get<std::string>();
        }
    } else if (event.object == "extruder") {
        if (event.payload.contains("target") && event.payload["target"].is_number()) {
            extruder_target_.store(event.payload["target"].get<double>());
        }
        if (event.payload.contains("temperature") && event.payload["temperature"].is_number()) {
            extruder_temp_.store(event.payload["temperature"].get<double>());
        }
    } else if (event.object == "heater_bed") {
        if (event.payload.contains("target") && event.payload["target"].is_number()) {
            bed_target_.store(event.payload["target"].get<double>());
        }
        if (event.payload.contains("temperature") && event.payload["temperature"].is_number()) {
            bed_temp_.store(event.payload["temperature"].get<double>());
        }
    }

    json status = {{event.object, event.payload}};
    dispatch_status_update(status);
}

void MoonrakerClientMock::reset_idle_timeout() {
    last_activity_time_ = std::chrono::steady_clock::now();
    if (idle_timeout_triggered_.load()) {
        idle_timeout_triggered_.store(false);
        spdlog::debug("[MoonrakerClientMock] Idle timeout reset");
    }
}

int MoonrakerClientMock::get_current_layer() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (print_metadata_.layer_count == 0) {
        return 0;
    }
    return static_cast<int>(print_progress_.load() * print_metadata_.layer_count);
}

int MoonrakerClientMock::get_total_layers() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return static_cast<int>(print_metadata_.layer_count);
}

bool MoonrakerClientMock::has_motors_sync() const {
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
    const auto objects = discovery_.hardware().printer_objects(); // hardware() is a copy
    return std::find(objects.begin(), objects.end(), "motors_sync") != objects.end();
}

bool MoonrakerClientMock::has_chamber_sensor() const {
    // The simulation thread calls this every iteration while discover_printer()
    // is still reassigning discovery_.sensors() on the calling thread — the same
    // heap-use-after-free the writers below guard against. Neither caller (here
    // and dispatch_historical_temperatures()) holds the lock, so taking it here
    // cannot self-deadlock.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
    for (const auto& s : discovery_.sensors()) {
        if (s == "temperature_sensor chamber") {
            return true;
        }
    }
    return false;
}

bool MoonrakerClientMock::simulates_chamber_temp() const {
    {
        std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
        for (const auto& s : discovery_.sensors()) {
            if (s == "temperature_sensor chamber_temp") {
                return true;
            }
        }
    }
    return has_chamber_sensor();
}

std::string MoonrakerClientMock::chamber_heater_status_key() const {
    return cached_chamber_status_key_;
}

std::string MoonrakerClientMock::chamber_heater_bare_name() const {
    const std::string key = chamber_heater_status_key();
    if (key.rfind("heater_generic ", 0) == 0) {
        return key.substr(15);
    }
    if (key.rfind("temperature_fan ", 0) == 0) {
        return key.substr(16);
    }
    return {};
}

std::string MoonrakerClientMock::chamber_filter_pin_object() const {
    const auto hw = discovery_.hardware();
    const auto* backend = chamber::backend_by_id(hw.chamber_heater_backend_id());
    if (!backend) {
        return {};
    }
    const std::string pin(backend->filter_fan_pin());
    if (pin.empty()) {
        return {};
    }
    const auto& objects = hw.printer_objects();
    if (std::find(objects.begin(), objects.end(), pin) == objects.end()) {
        return {};
    }
    return pin;
}

std::set<std::string> MoonrakerClientMock::get_excluded_objects() const {
    // If shared state is set, use that for consistency with MoonrakerAPIMock
    if (mock_state_) {
        return mock_state_->get_excluded_objects();
    }
    // Fallback to local state for backward compatibility
    std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
    return excluded_objects_;
}

void MoonrakerClientMock::set_mock_state(std::shared_ptr<MockPrinterState> state) {
    mock_state_ = state;
    if (state) {
        spdlog::debug("[MoonrakerClientMock] Shared mock state attached");
    } else {
        spdlog::debug("[MoonrakerClientMock] Shared mock state detached");
    }
}

MoonrakerClientMock::~MoonrakerClientMock() {
    // Signal restart thread to stop and wait for it (under lock to prevent race)
    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        restart_pending_.store(false);
        if (restart_thread_.joinable()) {
            restart_thread_.join();
        }
    }

    // Pass true to skip logging during destruction - spdlog may already be destroyed
    stop_temperature_simulation(true);

    // Clean up any outstanding calibration timers (PID, MPC, shaper) to prevent
    // use-after-free when a subsequent test calls process_lvgl().
    // Timer callbacks self-delete via lv_timer_delete, so some tracked pointers
    // may be stale. Verify each exists in LVGL's timer list before deleting.
    // The payload deleter runs either way when the timer is still armed: the
    // callback that would have freed it will never fire.
    if (lv_is_initialized()) {
        if (replay_timer_ != nullptr) {
            lv_timer_delete(replay_timer_);
            replay_timer_ = nullptr;
        }
        for (auto& tracked : calibration_timers_) {
            bool still_alive = false;
            lv_timer_t* t = lv_timer_get_next(nullptr);
            while (t) {
                if (t == tracked.timer) {
                    still_alive = true;
                    break;
                }
                t = lv_timer_get_next(t);
            }
            if (still_alive) {
                if (tracked.free_payload) {
                    tracked.free_payload();
                }
                lv_timer_delete(tracked.timer);
            }
        }
    }
    calibration_timers_.clear();
}

int MoonrakerClientMock::connect(const char* url, helix::SharedCallback<void()> on_connected,
                                 [[maybe_unused]] helix::SharedCallback<void()> on_disconnected) {
    spdlog::debug("[MoonrakerClientMock] Simulating connection to: {}", url ? url : "(null)");

    // Record the URL exactly as the real connect() does. Without this,
    // get_last_url() stays empty for the whole life of a mock session and
    // anything asking which host we are talking to -- is_local_moonraker(),
    // telemetry topology -- silently reads "no connection".
    set_last_url(url ? url : "");

    // Simulate connection state change (same as real client)
    set_connection_state(ConnectionState::CONNECTING);

    // Small delay to simulate a realistic connection
    auto delay_ms = sim_speed().shorten_wait_ms(g_connect_delay_ms.load());
    if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    // Check if we should simulate disconnected state for testing
    if (get_runtime_config()->simulate_disconnect) {
        spdlog::warn(
            "[MoonrakerClientMock] --disconnected flag set, simulating connection failure");
        set_connection_state(ConnectionState::DISCONNECTED);
        // Don't invoke on_connected callback or dispatch any state
        return 0;
    }

    set_connection_state(ConnectionState::CONNECTED);
    sim_link_down_ = false;

    // Dispatch historical temperature data first (fills graph with 2-3 min of data)
    dispatch_historical_temperatures();

    // Start live temperature simulation
    start_temperature_simulation();

    // Initial state for objects the printer.objects.subscribe handler does not
    // answer (probes, filament sensors, exclude_object, bed_mesh, chamber
    // sensor, mcu). It also repeats the subscribed heaters, print_stats and
    // toolhead with the same values the discovery subscription delivers, which
    // the status consumers treat as an unchanged frame.
    dispatch_initial_state();

    // Auto-start a print if configured (e.g., when testing print-status panel)
    if (get_runtime_config()->mock_auto_start_print) {
        // Use --gcode-file if specified, otherwise fall back to default test file
        const char* print_file = get_runtime_config()->gcode_test_file
                                     ? get_runtime_config()->gcode_test_file
                                     : RuntimeConfig::kDefaultTestFile;
        spdlog::info("[MoonrakerClientMock] Auto-starting print simulation with '{}'", print_file);
        start_print_internal(print_file);
    }

    // Walk a captured print-start sequence through the real dispatch paths
    // (HELIX_MOCK_REPLAY). Runs after the initial state so the replay's
    // print_stats edge arms the collector the same way a live print would.
    if (!replay_events_.empty()) {
        start_replay_timer();
    }

    // Immediately invoke connection callback
    if (on_connected) {
        spdlog::debug("[MoonrakerClientMock] Simulated connection successful");
        on_connected();
    }

    // Store disconnect callback (never invoked in mock, but stored for consistency)
    // Note: Not needed for this simple mock implementation

    return 0; // Success
}

std::string MoonrakerClientMock::kinematics() const {
    return kinematics_override_.empty()
               ? std::string(helix::mock::descriptor(printer_type_).kinematics)
               : kinematics_override_;
}

void MoonrakerClientMock::append_k2_status(json& status) const {
    // Creality's motor controller and fan tachometer modules, in the fields
    // cfs::parse_motor_control and PrinterFanState read.
    status["motor_control"] = {{"motor_ready", true}};
    status["fan_feedback"] = {{"fan0_speed", 0}, {"fan1_speed", 0}, {"fan2_speed", 0}};
}

namespace {

// Z-Mod's filament.json defaults: the 24 palette colours in index order (an
// off-palette HEX snaps to index 0, white) and the material types
// CHANGE_ZCOLOR accepts.
constexpr std::array<const char*, 24> kZmodPalette = {
    "FFFFFF", "FEF043", "DCF478", "0ACC38", "067749", "0C6283", "0DE2A0", "75D9F3",
    "45A8F9", "2750E0", "46328E", "A03CF7", "F330F9", "D4B0DC", "F95D73", "F72224",
    "7C4B00", "F98D33", "FDEBD5", "D3C4A3", "AF7836", "898989", "BCBCBC", "161616"};
constexpr std::array<const char*, 17> kZmodValidTypes = {
    "PLA",     "PETG",  "PLA-CF", "PETG-CF", "ABS",     "ASA",     "SILK",    "PET-CF", "S-PAHT",
    "S-MULTI", "PA-CF", "HIPS",   "PVA",     "TPU-90A", "TPU-95A", "TPU-64D", "?"};

} // namespace

nlohmann::json MoonrakerClientMock::defect_detection_status() const {
    if (printer_type_ != PrinterType::SNAPMAKER_U1) {
        return nlohmann::json::object();
    }
    auto feature = [](bool enable) {
        return nlohmann::json{{"enable", enable}, {"sensitivity", "low"}};
    };
    return nlohmann::json{{"main_enable", u1_defect_main_enable_.load()},
                          {"clean_bed", feature(u1_clean_bed_enable_.load())},
                          {"noodle", feature(true)},
                          {"residue", feature(false)},
                          {"nozzle", feature(false)}};
}

nlohmann::json MoonrakerClientMock::purifier_status() const {
    if (printer_type_ != PrinterType::SNAPMAKER_U1) {
        return nlohmann::json::object();
    }
    const char* env = std::getenv("HELIX_MOCK_ENCLOSURE_TOP");
    const bool fitted = !(env && std::string(env) == "0");
    const auto fan = nlohmann::json{{"speed", 0.0}, {"rpm", 0.0}};
    return nlohmann::json{{"power_detected", fitted},
                          {"power_det_value", fitted ? 2.5 : 0.0},
                          {"mode", 0},
                          {"desired_chamber_temp", 0},
                          {"critical_chamber_temp", 0},
                          {"inner_fan_work_time", 0.0},
                          {"inner_fan_rpm", 0.0},
                          {"exhaust_fan", fan},
                          {"inner_fan", fan}};
}

json MoonrakerClientMock::zmod_color_status() const {
    std::lock_guard<std::mutex> lock(zmod_mutex_);
    json slots = json::array();
    for (std::size_t i = 0; i < zmod_slots_.size(); ++i) {
        // hasFilament is part of the real firmware's slot schema (field bundle
        // 5WSPTZBL); nothing reads it yet, but the mock keeps the wire shape.
        slots.push_back({{"ID", std::to_string(i + 1)},
                         {"Material", zmod_slots_[i].first},
                         {"HEX", zmod_slots_[i].second},
                         {"hasFilament", true}});
    }
    return json{{"active_tool_id", zmod_active_tool_.load()},
                {"total_tools", 4},
                // The head count, as the firmware reports it; not the 24-entry
                // palette size, which is what the field name suggests.
                {"color_limit", 4},
                {"display", false},
                {"valid_types", kZmodValidTypes},
                {"hidden_types", json::array()},
                {"palette", kZmodPalette},
                {"slots", std::move(slots)}};
}

// Z-Mod commands (creator5_zmod persona): the zmod_color extra's macros.
// Matched on the command token exactly, and armed only for this persona.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_zmod(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    auto param = [&gcode](const char* key) -> std::string {
        const std::string needle = std::string(key) + "=";
        const size_t at = gcode.find(needle);
        if (at == std::string::npos) {
            return {};
        }
        const size_t start = at + needle.size();
        const size_t end = gcode.find_first_of(" \t", start);
        return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    auto refuse = [this](const std::string& message) {
        spdlog::warn("[MoonrakerClientMock] Z-Mod gcode rejected: {}", message);
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = message;
        return 1;
    };

    if (cmd == "T_INFO") {
        // Z-Mod's read-only tool-state query: one row per head, then the
        // trailing block the carriage-empty answer hangs off.
        const int active = zmod_active_tool_.load();
        for (int i = 0; i < 4; ++i) {
            dispatch_gcode_response("// T" + std::to_string(i) + ": " +
                                    (i == active ? "HEAD" : "home"));
        }
        dispatch_gcode_response("// Door: Close");
        dispatch_gcode_response("// Top: Close");
        dispatch_gcode_response("// Offset: X=0.0000 Y=0.0000 Z=0.0000 (0.000)");
        return 0;
    }
    if (cmd == "_T_IN") {
        int tool = -1;
        try {
            tool = std::stoi(param("T"));
        } catch (...) {
        }
        if (tool < 0 || tool > 3) {
            return refuse("T out of range: " + param("T"));
        }
        // The swap narration the real macro emits (bundle 5WSPTZBL): the grab
        // retry loop and the offset echoes that close the pickup. These are
        // what the toolchanger backend's marker listener lights its step bar
        // from, so the mock must speak them too.
        dispatch_gcode_response("// Grab attempt for T" + std::to_string(tool) + ": 1 of 5...");
        dispatch_gcode_response("// X-Offset: 0.0000 _T_IN");
        zmod_active_tool_.store(tool);
        dispatch_status_update({{"zmod_color", {{"active_tool_id", tool}}}});
        return 0;
    }
    if (cmd == "CHANGE_TOOL") {
        // The afclite shim's load: it mounts the lane's head and feeds it. Lanes
        // are the E0..E3 objects the persona publishes.
        const std::string lane = param("LANE");
        int tool = -1;
        if (lane.size() == 2 && lane[0] == 'E' && lane[1] >= '0' && lane[1] <= '3') {
            tool = lane[1] - '0';
        }
        if (tool < 0) {
            return refuse("Unknown lane: " + lane);
        }
        dispatch_gcode_response("// Grab attempt for T" + std::to_string(tool) + ": 1 of 5...");
        dispatch_gcode_response("// X-Offset: 0.0000 _T_IN");
        zmod_active_tool_.store(tool);
        dispatch_status_update({{"zmod_color", {{"active_tool_id", tool}}}});
        return 0;
    }
    if (cmd == "_T_OUT") {
        const int parked = zmod_active_tool_.load();
        if (parked >= 0) {
            dispatch_gcode_response("// Unload attempt for T" + std::to_string(parked) +
                                    ": 1 of 5...");
        }
        dispatch_gcode_response("// X-Offset: 0.0000 _T_OUT");
        zmod_active_tool_.store(-1);
        dispatch_status_update({{"zmod_color", {{"active_tool_id", -1}}}});
        return 0;
    }
    if (cmd == "CALIBRATE_EXTRUDERS") {
        // The real macro only opens this prompt; its OK starts the measurement.
        dispatch_gcode_response("// action:prompt_end");
        dispatch_gcode_response("// action:prompt_begin CALIBRATE EXTRUDERS");
        dispatch_gcode_response("// action:prompt_text Please remove the build plate first!");
        dispatch_gcode_response("// action:prompt_text T0=1, T1=1, T2=1, T3=1");
        dispatch_gcode_response("// action:prompt_footer_button OK|_T_CALIBRATE_EXTRUDERS|primary");
        dispatch_gcode_response("// action:prompt_footer_button Cancel|RESPOND TYPE=command "
                                "MSG=action:prompt_end|secondary");
        dispatch_gcode_response("// action:prompt_show");
        return 0;
    }
    if (cmd == "_T_CALIBRATE_EXTRUDERS") {
        dispatch_gcode_response("// action:prompt_end");
        dispatch_gcode_response("// Extruder calibration finished");
        return 0;
    }
    if (cmd == "AIR_CIRCULATION_INTERNAL" || cmd == "AIR_CIRCULATION_EXTERNAL" ||
        cmd == "AIR_CIRCULATION_STOP") {
        // c5pro_generic.cfg's fan speeds per mode: chamber_fan, chamber_cool_fan,
        // chamber_heat_fan, chamber_loop_fan.
        static const char* const kFans[] = {"chamber_fan", "chamber_cool_fan", "chamber_heat_fan",
                                            "chamber_loop_fan"};
        const double internal[] = {0.0, 0.0, 0.9, 0.3};
        const double external[] = {0.5, 0.7, 0.0, 0.0};
        const double stop[] = {0.0, 0.0, 0.0, 0.0};
        const double* speeds = cmd == "AIR_CIRCULATION_INTERNAL"   ? internal
                               : cmd == "AIR_CIRCULATION_EXTERNAL" ? external
                                                                   : stop;
        for (size_t i = 0; i < std::size(kFans); ++i) {
            gcode_set_fan_speed(fmt::format("SET_FAN_SPEED FAN={} SPEED={}", kFans[i], speeds[i]));
        }
        return 0;
    }
    if (cmd == "CALIBRATE_VFA") {
        // Mounts the head, calibrates and parks it; the real macro then ends
        // in SAVE_CONFIG, which the mock does not restart for.
        const std::string tool = param("T").empty() ? "0" : param("T");
        dispatch_gcode_response("// Grab attempt for T" + tool + ": 1 of 5...");
        dispatch_gcode_response("// VFA calibration finished");
        return 0;
    }
    if (cmd == "CHANGE_ZCOLOR") {
        const std::string type = param("TYPE");
        if (std::find(kZmodValidTypes.begin(), kZmodValidTypes.end(), type) ==
            kZmodValidTypes.end()) {
            return refuse("Unknown material type: " + type);
        }
        int slot = 0;
        try {
            slot = std::stoi(param("SLOT"));
        } catch (...) {
        }
        if (slot < 1 || slot > 4) {
            return refuse("SLOT out of range: " + param("SLOT"));
        }
        std::string hex = param("HEX");
        hex = helix::text_io::to_upper(hex);
        // The firmware stores only palette colours; anything else snaps to
        // index 0, white.
        const bool in_palette =
            std::find(kZmodPalette.begin(), kZmodPalette.end(), hex) != kZmodPalette.end();
        {
            std::lock_guard<std::mutex> lock(zmod_mutex_);
            zmod_slots_[slot - 1] = {type, in_palette ? hex : "FFFFFF"};
        }
        dispatch_status_update({{"zmod_color", {{"slots", zmod_color_status()["slots"]}}}});
        return 0;
    }
    return std::nullopt;
}

nlohmann::json MoonrakerClientMock::u1_feed_side_json(int ext) const {
    // "filament_feed left" carries extruders 0/1, "right" carries 2/3.
    const int first = (ext < 2) ? 0 : 2;
    nlohmann::json channels = nlohmann::json::object();
    for (int i = first; i < first + 2; ++i) {
        channels["extruder" + std::to_string(i)] = {
            {"channel_state", u1_channel_state_[static_cast<size_t>(i)]},
            {"channel_error", "ok"},
            {"channel_error_state", "none"},
            {"channel_action_state", u1_channel_state_[static_cast<size_t>(i)]},
            {"disable_auto", false},
            {"filament_detected", u1_filament_detected_[static_cast<size_t>(i)]},
            {"module_exist", true},
        };
    }
    return channels;
}

void MoonrakerClientMock::publish_u1_channel_frame(int ext) {
    const std::string side = (ext < 2) ? "left" : "right";
    dispatch_status_update({{"filament_feed " + side, u1_feed_side_json(ext)}});
}

bool MoonrakerClientMock::apply_u1_feeding_gcode(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    // AUTO_FEEDING and AUTO_FEEDING_BATCH ACTION=DOING both drive a channel.
    // The BATCH START/END lines name no EXTRUDER, so they land in the "feeder
    // command, nothing to walk" branch and change no channel state.
    if (cmd.rfind("AUTO_FEEDING", 0) != 0) {
        return false;
    }
    const size_t e = gcode.find("EXTRUDER=");
    if (e == std::string::npos) {
        return true;
    }
    int ext = -1;
    try {
        ext = std::stoi(gcode.substr(e + 9));
    } catch (...) {
        return true;
    }
    if (ext < 0 || ext >= kU1Channels) {
        return true;
    }
    // "UNLOAD=1" contains "LOAD=1" as a substring; the direction must be read
    // unload-first.
    const bool unload = gcode.find("UNLOAD=1") != std::string::npos;

    int fail_slot = -1;
    if (const char* fail_env = std::getenv("HELIX_MOCK_BATCH_FAIL_SLOT")) {
        try {
            fail_slot = std::stoi(fail_env);
        } catch (...) {
            fail_slot = -1;
        }
    }

    const size_t ext_idx = static_cast<size_t>(ext);
    // The state sequences the firmware's own filament_feed walks: preload then
    // load for a feed, heat then retract for an unload.
    u1_channel_state_[ext_idx] = unload ? "unload_heat_finish" : "preload_finish";
    if (!unload) {
        u1_filament_detected_[ext_idx] = true;
    }
    publish_u1_channel_frame(ext);

    const char* terminal = unload ? "unload_finish" : "load_finish";
    if (ext == fail_slot) {
        terminal = unload ? "unload_fail" : "load_fail";
    }
    u1_channel_state_[ext_idx] = terminal;
    if (unload && ext != fail_slot) {
        u1_filament_detected_[ext_idx] = false;
    }
    publish_u1_channel_frame(ext);
    spdlog::info("[MoonrakerClientMock] U1 feeder: extruder {} {} -> {}", ext,
                 unload ? "unload" : "load", terminal);
    return true;
}

void MoonrakerClientMock::parse_incoming_bed_mesh(const json& bed_mesh) {
    // Parse bed mesh JSON from dispatch_status_update into active_bed_mesh_
    // This mirrors the JSON format sent by real Moonraker

    // Parse profile name
    if (bed_mesh.contains("profile_name")) {
        if (bed_mesh["profile_name"].is_string()) {
            active_bed_mesh_.name = bed_mesh["profile_name"].get<std::string>();
        } else if (bed_mesh["profile_name"].is_null()) {
            active_bed_mesh_.name = "";
        }
    }

    // Parse probed_matrix (2D array of Z heights)
    if (bed_mesh.contains("probed_matrix") && bed_mesh["probed_matrix"].is_array()) {
        active_bed_mesh_.probed_matrix.clear();
        const auto& matrix = bed_mesh["probed_matrix"];

        for (const auto& row : matrix) {
            if (!row.is_array()) {
                continue;
            }
            std::vector<float> row_vec;
            for (const auto& val : row) {
                if (val.is_number()) {
                    row_vec.push_back(val.get<float>());
                }
                // Skip non-numeric values (strings, nulls)
            }
            active_bed_mesh_.probed_matrix.push_back(row_vec);
        }

        // Update counts based on parsed matrix
        if (!active_bed_mesh_.probed_matrix.empty()) {
            active_bed_mesh_.y_count = static_cast<int>(active_bed_mesh_.probed_matrix.size());
            active_bed_mesh_.x_count = static_cast<int>(active_bed_mesh_.probed_matrix[0].size());
        } else {
            active_bed_mesh_.x_count = 0;
            active_bed_mesh_.y_count = 0;
        }
    }

    // Parse mesh_min (array [x, y])
    if (bed_mesh.contains("mesh_min") && bed_mesh["mesh_min"].is_array() &&
        bed_mesh["mesh_min"].size() >= 2) {
        if (bed_mesh["mesh_min"][0].is_number()) {
            active_bed_mesh_.mesh_min[0] = bed_mesh["mesh_min"][0].get<float>();
        }
        if (bed_mesh["mesh_min"][1].is_number()) {
            active_bed_mesh_.mesh_min[1] = bed_mesh["mesh_min"][1].get<float>();
        }
    }

    // Parse mesh_max (array [x, y])
    if (bed_mesh.contains("mesh_max") && bed_mesh["mesh_max"].is_array() &&
        bed_mesh["mesh_max"].size() >= 2) {
        if (bed_mesh["mesh_max"][0].is_number()) {
            active_bed_mesh_.mesh_max[0] = bed_mesh["mesh_max"][0].get<float>();
        }
        if (bed_mesh["mesh_max"][1].is_number()) {
            active_bed_mesh_.mesh_max[1] = bed_mesh["mesh_max"][1].get<float>();
        }
    }

    // Parse algorithm from mesh_params
    if (bed_mesh.contains("mesh_params") && bed_mesh["mesh_params"].is_object()) {
        const auto& params = bed_mesh["mesh_params"];
        if (params.contains("algo") && params["algo"].is_string()) {
            active_bed_mesh_.algo = params["algo"].get<std::string>();
        }
    }

    // Parse profiles list
    if (bed_mesh.contains("profiles") && bed_mesh["profiles"].is_object()) {
        bed_mesh_profiles_.clear();
        for (const auto& [key, value] : bed_mesh["profiles"].items()) {
            bed_mesh_profiles_.push_back(key);
        }
    }

    spdlog::debug("[MoonrakerClientMock] Parsed incoming bed mesh: profile='{}', size={}x{}",
                  active_bed_mesh_.name, active_bed_mesh_.x_count, active_bed_mesh_.y_count);
}

void MoonrakerClientMock::generate_mock_bed_mesh() {
    // Helper lambda to generate a mesh with given shape parameters
    const auto bounds = mock_internal::mesh_bounds(printer_type_);
    auto generate_mesh = [bounds](const std::string& name, float amplitude, float x_tilt,
                                  float y_tilt) -> BedMeshProfile {
        BedMeshProfile mesh;
        mesh.name = name;
        mesh.mesh_min[0] = static_cast<float>(bounds.x_min);
        mesh.mesh_min[1] = static_cast<float>(bounds.y_min);
        mesh.mesh_max[0] = static_cast<float>(bounds.x_max);
        mesh.mesh_max[1] = static_cast<float>(bounds.y_max);
        mesh.x_count = 7;
        mesh.y_count = 7;
        mesh.algo = "lagrange";

        float center_x = mesh.x_count / 2.0f;
        float center_y = mesh.y_count / 2.0f;
        float max_radius = std::min(center_x, center_y);

        for (int row = 0; row < mesh.y_count; row++) {
            std::vector<float> row_vec;
            for (int col = 0; col < mesh.x_count; col++) {
                float dx = col - center_x;
                float dy = row - center_y;
                float dist = std::sqrt(dx * dx + dy * dy);

                // Dome shape + optional tilt
                float normalized_dist = dist / max_radius;
                float height = amplitude * (1.0f - normalized_dist * normalized_dist);
                height += x_tilt * (col - center_x) / center_x * 0.1f;
                height += y_tilt * (row - center_y) / center_y * 0.1f;

                row_vec.push_back(height);
            }
            mesh.probed_matrix.push_back(row_vec);
        }
        return mesh;
    };

    // Generate "default" profile: centered dome, 0.3mm amplitude
    stored_bed_mesh_profiles_["default"] = generate_mesh("default", 0.3f, 0.0f, 0.0f);

    // Generate "adaptive" profile: dome with slight tilt, different amplitude
    stored_bed_mesh_profiles_["adaptive"] = generate_mesh("adaptive", 0.25f, 0.5f, -0.3f);

    // Set profile name list
    bed_mesh_profiles_ = {"default", "adaptive"};

    // Load "default" as active
    active_bed_mesh_ = stored_bed_mesh_profiles_["default"];

    spdlog::debug("[MoonrakerClientMock] Generated {} bed mesh profiles, active='{}'",
                  stored_bed_mesh_profiles_.size(), active_bed_mesh_.name);
}

void MoonrakerClientMock::generate_mock_bed_mesh_with_variation() {
    // Generate a realistic bed mesh with true randomness
    // Simulates re-probing with measurement noise and slight bed changes

    // Probed area of the persona's bed
    const auto bounds = mock_internal::mesh_bounds(printer_type_);
    active_bed_mesh_.mesh_min[0] = static_cast<float>(bounds.x_min);
    active_bed_mesh_.mesh_min[1] = static_cast<float>(bounds.y_min);
    active_bed_mesh_.mesh_max[0] = static_cast<float>(bounds.x_max);
    active_bed_mesh_.mesh_max[1] = static_cast<float>(bounds.y_max);
    active_bed_mesh_.x_count = 7;
    active_bed_mesh_.y_count = 7;
    active_bed_mesh_.algo = "lagrange";

    // Seed with both random_device and a monotonic counter to guarantee
    // distinct meshes even if random_device is deterministic (some Linux/embedded platforms)
    static std::atomic<uint32_t> calibration_counter{0};
    std::random_device rd;
    std::seed_seq seed{
        rd(), rd(), calibration_counter.fetch_add(1),
        static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::mt19937 gen(seed);
    std::uniform_real_distribution<float> noise(-0.03f, 0.03f);      // ±0.03mm probe noise
    std::uniform_real_distribution<float> amplitude(0.15f, 0.35f);   // Overall dome height
    std::uniform_real_distribution<float> tilt(-0.08f, 0.08f);       // Bed tilt per axis
    std::uniform_real_distribution<float> center_shift(-0.5f, 0.5f); // Dome center offset

    // Random parameters for this calibration
    float dome_amp = amplitude(gen);
    float x_tilt = tilt(gen);
    float y_tilt = tilt(gen);
    float cx_shift = center_shift(gen);
    float cy_shift = center_shift(gen);

    active_bed_mesh_.probed_matrix.clear();
    float center_x = active_bed_mesh_.x_count / 2.0f + cx_shift;
    float center_y = active_bed_mesh_.y_count / 2.0f + cy_shift;
    float max_radius = std::min(active_bed_mesh_.x_count, active_bed_mesh_.y_count) / 2.0f;

    for (int row = 0; row < active_bed_mesh_.y_count; row++) {
        std::vector<float> row_vec;
        for (int col = 0; col < active_bed_mesh_.x_count; col++) {
            float dx = col - center_x;
            float dy = row - center_y;
            float dist = std::sqrt(dx * dx + dy * dy);

            // Base dome shape
            float normalized_dist = dist / max_radius;
            float height = dome_amp * (1.0f - normalized_dist * normalized_dist);

            // Add bed tilt (simulates unlevel bed)
            float norm_x = static_cast<float>(col) / (active_bed_mesh_.x_count - 1) - 0.5f;
            float norm_y = static_cast<float>(row) / (active_bed_mesh_.y_count - 1) - 0.5f;
            height += x_tilt * norm_x + y_tilt * norm_y;

            // Add per-point probe noise (simulates measurement uncertainty)
            height += noise(gen);

            row_vec.push_back(height);
        }
        active_bed_mesh_.probed_matrix.push_back(row_vec);
    }

    spdlog::debug("[MoonrakerClientMock] Regenerated bed mesh: amp={:.3f}, tilt=({:.3f},{:.3f})",
                  dome_amp, x_tilt, y_tilt);
}

json MoonrakerClientMock::bed_mesh_status() const {
    const auto matrix_json = [](const std::vector<std::vector<float>>& matrix) {
        json rows = json::array();
        for (const auto& row : matrix) {
            json row_json = json::array();
            for (float val : row) {
                row_json.push_back(val);
            }
            rows.push_back(row_json);
        }
        return rows;
    };

    json profiles_json = json::object();
    for (const auto& [name, profile] : stored_bed_mesh_profiles_) {
        profiles_json[name] = {{"points", matrix_json(profile.probed_matrix)},
                               {"mesh_params",
                                {{"min_x", profile.mesh_min[0]},
                                 {"min_y", profile.mesh_min[1]},
                                 {"max_x", profile.mesh_max[0]},
                                 {"max_y", profile.mesh_max[1]},
                                 {"x_count", profile.x_count},
                                 {"y_count", profile.y_count}}}};
    }

    json status = {{"profile_name", active_bed_mesh_.name},
                   {"probed_matrix", matrix_json(active_bed_mesh_.probed_matrix)},
                   {"mesh_min", {active_bed_mesh_.mesh_min[0], active_bed_mesh_.mesh_min[1]}},
                   {"mesh_max", {active_bed_mesh_.mesh_max[0], active_bed_mesh_.mesh_max[1]}},
                   {"profiles", profiles_json},
                   {"mesh_params", {{"algo", active_bed_mesh_.algo}}}};
    return status;
}

void MoonrakerClientMock::calibrate_mock_mesh(const std::string& profile_name) {
    // Regenerate mesh with slight random variation
    active_bed_mesh_.name = profile_name;
    generate_mock_bed_mesh_with_variation();

    if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) ==
        bed_mesh_profiles_.end()) {
        bed_mesh_profiles_.push_back(profile_name);
    }
    stored_bed_mesh_profiles_[profile_name] = active_bed_mesh_;

    spdlog::info("[MoonrakerClientMock] Mesh calibration: generated new mesh for profile '{}'",
                 profile_name);
    dispatch_bed_mesh_update();
}

void MoonrakerClientMock::dispatch_bed_mesh_update() {
    json status = {{"bed_mesh", bed_mesh_status()}};
    dispatch_status_update(status);
}

void MoonrakerClientMock::disconnect() {
    spdlog::info("[MoonrakerClientMock] Simulating disconnection");
    stop_temperature_simulation(false);
    set_connection_state(ConnectionState::DISCONNECTED);
    sim_link_down_ = true;
    // A swap reply held across the drop belongs to the old link: it fails as
    // that link's every pending request does, never on the next connection.
    std::optional<MedusaReply> held;
    {
        std::lock_guard<std::mutex> lock(medusa_mutex_);
        held = std::exchange(medusa_reply_, std::nullopt);
    }
    if (held && held->error_cb) {
        held->error_cb(MoonrakerError::connection_lost("printer.gcode.script"));
    }
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {}", method);
    return 0; // Success
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method,
                                      [[maybe_unused]] const json& params) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {} (with params)", method);
    return 0; // Success
}

RequestId MoonrakerClientMock::send_jsonrpc(const std::string& method, const json& params,
                                            helix::SharedCallback<void(const json&)> cb) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {} (with callback)", method);

    // Dispatch to handler registry (wrap callback to match error_cb signature)
    auto noop_error_cb = [](const MoonrakerError&) {};
    return send_jsonrpc(method, params, cb, noop_error_cb);
}

RequestId
MoonrakerClientMock::send_jsonrpc(const std::string& method, const json& params,
                                  helix::SharedCallback<void(const json&)> success_cb,
                                  helix::SharedCallback<void(const MoonrakerError&)> error_cb,
                                  uint32_t timeout_ms, bool silent,
                                  std::optional<rpc_error_policy::CallerIntent> intent) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {} (with success/error callbacks)",
                  method);

    // Mirror MoonrakerClient::send_jsonrpc, but only between an explicit
    // disconnect() and the next successful connect(): a request on a dropped
    // link is refused and reported to the error callback as CONNECTION_LOST,
    // so code under test sees the same immediate failure production shows.
    // Fixtures that never simulate a link keep getting answers.
    if (sim_link_down_) {
        if (error_cb) {
            error_cb(MoonrakerError::connection_lost(method));
        }
        return kInvalidRequestId;
    }

    // Same fallback inference MoonrakerRequestTracker::send() applies, so a
    // handler asking rpc_error_policy::decide() gets the hardware answer.
    current_send_intent_ =
        intent.value_or(rpc_error_policy::CallerIntent{silent, error_cb != nullptr});

    // Capture for test inspection — used by tests verifying that specific callers pass
    // non-default timeout/silent values (e.g. exclude_object which must be silent+long).
    last_send_method_ = method;
    last_send_timeout_ms_ = timeout_ms;
    last_send_silent_ = silent;
    if (method == "printer.gcode.script" && params.contains("script") &&
        params["script"].is_string()) {
        last_send_script_ = params["script"].get<std::string>();
    }

    std::optional<MoonrakerError> injected_error;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        ++call_counts_[method];
        if (auto f = fail_next_.find(method); f != fail_next_.end()) {
            injected_error = std::move(f->second);
            fail_next_.erase(f);
        } else if (defer_next_.erase(method) > 0) {
            held_requests_[method] = HeldRequest{params, success_cb, error_cb};
            return next_mock_request_id();
        }
    }
    if (injected_error) {
        if (error_cb) {
            error_cb(*injected_error);
        }
        return next_mock_request_id();
    }

    // Dispatch to method handler registry
    auto it = method_handlers_.find(method);
    if (it != method_handlers_.end()) {
        it->second(this, params, success_cb, error_cb);
        return next_mock_request_id();
    }

    // Unimplemented methods - log warning
    spdlog::debug("[MoonrakerClientMock] Method '{}' not implemented - callbacks not invoked",
                  method);
    return next_mock_request_id();
}

void MoonrakerClientMock::fail_next(const std::string& method, MoonrakerError err) {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    err.method = method;
    fail_next_[method] = std::move(err);
}

void MoonrakerClientMock::defer_next(const std::string& method) {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    defer_next_.insert(method);
}

void MoonrakerClientMock::fire_deferred(const std::string& method) {
    std::optional<HeldRequest> held;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        if (auto h = held_requests_.find(method); h != held_requests_.end()) {
            held = std::move(h->second);
            held_requests_.erase(h);
        }
    }
    if (!held) {
        return;
    }
    if (auto it = method_handlers_.find(method); it != method_handlers_.end()) {
        it->second(this, held->params, held->success_cb, held->error_cb);
    }
}

void MoonrakerClientMock::fire_deferred_error(const std::string& method,
                                              const MoonrakerError& err) {
    std::optional<HeldRequest> held;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        if (auto h = held_requests_.find(method); h != held_requests_.end()) {
            held = std::move(h->second);
            held_requests_.erase(h);
        }
    }
    if (held && held->error_cb) {
        held->error_cb(err);
    }
}

int MoonrakerClientMock::call_count(const std::string& method) const {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    auto it = call_counts_.find(method);
    return it == call_counts_.end() ? 0 : it->second;
}

// Removed old implementation - now handled by method_handlers_ registry:
// Lines 527-916 deleted (file/print/objects/history handlers moved to separate modules)
// See: moonraker_client_mock_files.cpp, moonraker_client_mock_print.cpp,
//      moonraker_client_mock_objects.cpp, moonraker_client_mock_history.cpp
//
// Old logic was:
//   - server.files.* handlers (list, metadata, delete, move, copy, post_directory,
//   delete_directory)
//   - printer.gcode.script handler
//   - printer.print.* handlers (start, pause, resume, cancel)
//   - printer.objects.query handler
//   - server.history.* handlers (list, totals, delete_job)
//

std::string MoonrakerClientMock::get_last_gcode_error() const {
    std::lock_guard<std::mutex> lock(gcode_error_mutex_);
    return last_gcode_error_;
}

std::string MoonrakerClientMock::get_print_state_string() const {
    switch (print_state_.load()) {
    case 0:
        return "standby";
    case 1:
        return "printing";
    case 2:
        return "paused";
    case 3:
        return "complete";
    case 4:
        return "cancelled";
    case 5:
        return "error";
    default:
        return "standby";
    }
}

// ============================================================================
// Unified Print Control (internal implementation)
// ============================================================================

bool MoonrakerClientMock::start_print_internal(const std::string& filename) {
    // Build path to test G-code file
    // Handle both bare filenames (e.g., "3DBenchy.gcode") and full paths
    std::string full_path;

    // A staged rewrite names the original it was made from; that is the test
    // file holding the metadata.
    const std::string lookup_filename = helix::gcode::resolve_gcode_filename(filename);

    if (lookup_filename.find(RuntimeConfig::TEST_GCODE_DIR) == 0) {
        // Already a full path, use as-is
        full_path = lookup_filename;
    } else {
        // Bare filename, prepend test directory
        full_path = helix::mock::gcode_disk_path(lookup_filename);
    }

    // Extract metadata from G-code file
    auto meta = helix::gcode::extract_header_metadata(full_path);

    // Populate simulation metadata
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        print_metadata_.estimated_time_seconds =
            (meta.estimated_time_seconds > 0) ? meta.estimated_time_seconds : 300.0;
        print_metadata_.layer_count = (meta.layer_count > 0) ? meta.layer_count : 100;
        print_metadata_.target_bed_temp =
            (meta.first_layer_bed_temp > 0) ? meta.first_layer_bed_temp : 60.0;
        print_metadata_.target_nozzle_temp =
            (meta.first_layer_nozzle_temp > 0) ? meta.first_layer_nozzle_temp : 210.0;
        print_metadata_.filament_mm =
            (meta.filament_used_mm > 0) ? meta.filament_used_mm : 5400.0; // Default: ~5.4m
        print_metadata_.filament_weights_g = meta.filament_used_per_tool_g;
    }

    // Compute dominant tool (highest per-tool weight) for the mock to expose
    // as the active gcode tool during simulated print. This is the moonraker
    // mock's equivalent of "Klipper saw a T-command" — production AMS backends
    // would read printer.mmu.tool / toolchanger.tool_number on real hardware.
    {
        int dominant = -1;
        double max_w = 0.0;
        const auto& weights = print_metadata_.filament_weights_g;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (weights[i] > max_w) {
                max_w = weights[i];
                dominant = static_cast<int>(i);
            }
        }

        // Cache per-tool slicer colors so observers can show the slicer's
        // intended color when the active tool isn't mapped to a real slot.
        {
            std::lock_guard<std::mutex> lock(active_gcode_tool_mutex_);
            active_gcode_tool_colors_.clear();
            for (const auto& hex : meta.tool_colors) {
                active_gcode_tool_colors_.push_back(helix::parse_hex_color(hex).value_or(0));
            }
        }

        // A file with no per-tool weights names no tool: a real printer keeps
        // printing on whichever head is already active, so the active tool stays.
        if (dominant >= 0) {
            active_gcode_tool_.store(dominant);
            spdlog::debug(
                "[MoonrakerClientMock] Active gcode tool: T{} (from per-tool weights, max {:.2f}g)",
                dominant, max_w);
            notify_active_gcode_tool_observers(dominant);
        }
    }

    // Set temperature targets for preheat
    double nozzle_target, bed_target;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        nozzle_target = print_metadata_.target_nozzle_temp;
        bed_target = print_metadata_.target_bed_temp;
    }
    extruder_target_.store(nozzle_target);
    bed_target_.store(bed_target);

    // Reset idle timeout when starting a print
    reset_idle_timeout();

    // Set print filename
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        print_filename_ = filename;
    }

    // Reset progress and timing
    print_progress_.store(0.0);
    total_pause_duration_sim_ = 0.0;
    preheat_start_time_ = std::chrono::steady_clock::now();
    printing_start_time_.reset();

    // Clear excluded objects from any previous print
    if (mock_state_) {
        mock_state_->clear_excluded_objects();
    }
    {
        std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
        excluded_objects_.clear();
        object_names_.clear();
    }

    // Scan gcode file for EXCLUDE_OBJECT_DEFINE to populate object names
    {
        std::ifstream gcode_file(full_path);
        if (gcode_file.is_open()) {
            std::string line;
            while (std::getline(gcode_file, line)) {
                if (line.find("EXCLUDE_OBJECT_DEFINE") != std::string::npos) {
                    size_t name_pos = line.find("NAME=");
                    if (name_pos != std::string::npos) {
                        size_t start = name_pos + 5;
                        std::string object_name;
                        if (start < line.size() && line[start] == '"') {
                            size_t end = line.find('"', start + 1);
                            if (end != std::string::npos) {
                                object_name = line.substr(start + 1, end - start - 1);
                            }
                        } else {
                            size_t end = line.find_first_of(" \t\r\n", start);
                            object_name = (end != std::string::npos)
                                              ? line.substr(start, end - start)
                                              : line.substr(start);
                        }
                        if (!object_name.empty()) {
                            if (mock_state_) {
                                mock_state_->add_object_name(object_name);
                            }
                            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                            if (std::find(object_names_.begin(), object_names_.end(),
                                          object_name) == object_names_.end()) {
                                object_names_.push_back(object_name);
                            }
                        }
                    }
                }
            }
            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
            if (!object_names_.empty()) {
                spdlog::info("[MoonrakerClientMock] Found {} EXCLUDE_OBJECT_DEFINE objects in '{}'",
                             object_names_.size(), full_path);
            }
        }
    }

    // Parse EXCLUDE_OBJECT_DEFINE lines from gcode to populate defined objects.
    // Extracts NAME, CENTER, and POLYGON to match real Moonraker behavior.
    {
        json objects_array = json::array();
        std::vector<std::string> defined_objects;
        std::ifstream gcode_file(full_path);
        if (gcode_file.is_open()) {
            std::string line;
            while (std::getline(gcode_file, line)) {
                if (line.find("EXCLUDE_OBJECT_DEFINE") != std::string::npos) {
                    // Extract NAME= parameter
                    auto name_pos = line.find("NAME=");
                    if (name_pos == std::string::npos)
                        continue;
                    std::string name;
                    size_t start = name_pos + 5;
                    if (start < line.size() && line[start] == '"') {
                        size_t end = line.find('"', start + 1);
                        if (end != std::string::npos)
                            name = line.substr(start + 1, end - start - 1);
                    } else if (start < line.size() && line[start] == '\'') {
                        size_t end = line.find('\'', start + 1);
                        if (end != std::string::npos)
                            name = line.substr(start + 1, end - start - 1);
                    } else {
                        size_t end = line.find(' ', start);
                        name = line.substr(start, end - start);
                    }
                    if (name.empty())
                        continue;

                    defined_objects.push_back(name);
                    if (mock_state_)
                        mock_state_->add_object_name(name);

                    // Build JSON object entry with CENTER and POLYGON if present
                    json obj_entry = {{"name", name}};

                    // Parse CENTER=x,y
                    auto center_pos = line.find("CENTER=");
                    if (center_pos != std::string::npos) {
                        size_t cs = center_pos + 7;
                        size_t ce = line.find(' ', cs);
                        std::string center_str = line.substr(cs, ce - cs);
                        auto comma = center_str.find(',');
                        if (comma != std::string::npos) {
                            try {
                                float cx = std::stof(center_str.substr(0, comma));
                                float cy = std::stof(center_str.substr(comma + 1));
                                obj_entry["center"] = {cx, cy};
                            } catch (...) {
                            }
                        }
                    }

                    // Parse POLYGON=[[x,y],[x,y],...]
                    auto poly_pos = line.find("POLYGON=");
                    if (poly_pos != std::string::npos) {
                        size_t ps = poly_pos + 8;
                        // Find matching closing bracket
                        int depth = 0;
                        size_t pe = ps;
                        for (; pe < line.size(); ++pe) {
                            if (line[pe] == '[')
                                depth++;
                            else if (line[pe] == ']') {
                                depth--;
                                if (depth == 0) {
                                    pe++;
                                    break;
                                }
                            }
                        }
                        std::string poly_str = line.substr(ps, pe - ps);

                        // Parse the polygon array: [[x,y],[x,y],...]
                        json polygon = json::array();
                        size_t pos = 0;
                        while ((pos = poly_str.find('[', pos)) != std::string::npos) {
                            size_t end = poly_str.find(']', pos);
                            if (end == std::string::npos)
                                break;
                            std::string pair = poly_str.substr(pos + 1, end - pos - 1);
                            auto c = pair.find(',');
                            if (c != std::string::npos) {
                                try {
                                    float px = std::stof(pair.substr(0, c));
                                    float py = std::stof(pair.substr(c + 1));
                                    polygon.push_back({px, py});
                                } catch (...) {
                                }
                            }
                            pos = end + 1;
                        }
                        if (!polygon.empty()) {
                            obj_entry["polygon"] = polygon;
                        }
                    }

                    objects_array.push_back(std::move(obj_entry));
                }
                // Stop scanning after first layer to avoid reading the entire file
                if (line.find(";LAYER_CHANGE") != std::string::npos ||
                    line.find("; LAYER_CHANGE") != std::string::npos) {
                    break;
                }
            }
        }

        if (!defined_objects.empty()) {
            spdlog::info("[MoonrakerClientMock] Found {} defined objects in '{}'",
                         defined_objects.size(), lookup_filename);
            if (mock_state_) {
                mock_state_->set_available_objects(defined_objects);
            }
            json eo_status = {{"exclude_object",
                               {{"objects", objects_array},
                                {"excluded_objects", json::array()},
                                {"current_object", nullptr}}}};
            dispatch_status_update(eo_status);
        }
    }

    // HELIX_MOCK_EXCLUDE_OBJECTS — replace whatever the G-code declared with a
    // synthetic multi-object plate. Published through the same
    // `exclude_object.objects` status update Klipper uses, so PrinterState,
    // the map view and the side list see nothing special about it.
    if (mock_exclude_object_count_ > 0) {
        const int total = mock_exclude_object_count_;
        std::vector<std::string> names;
        names.reserve(static_cast<size_t>(total));
        json objects_array = json::array();
        for (int i = 0; i < total; ++i) {
            names.emplace_back(kMockExcludeObjectNames[i]);
            objects_array.push_back(
                mock_object_entry(names.back(), i, total, helix::mock::descriptor(printer_type_)));
        }

        {
            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
            object_names_ = names;
        }
        if (mock_state_) {
            mock_state_->set_available_objects(names);
        }

        json eo_status = {{"exclude_object",
                           {{"objects", objects_array},
                            {"excluded_objects", json::array()},
                            {"current_object", nullptr}}}};
        dispatch_status_update(eo_status);

        spdlog::info("[MoonrakerClientMock] Published {} synthetic exclude_object entries "
                     "(HELIX_MOCK_EXCLUDE_OBJECTS)",
                     total);
    }

    // Reset PRINT_START simulation phase tracking for new print
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Transition to PREHEAT phase
    print_phase_.store(MockPrintPhase::PREHEAT);
    print_state_.store(1); // "printing" for backward compatibility

    spdlog::debug("[MoonrakerClientMock] Starting print '{}': est_time={:.0f}s, layers={}, "
                  "nozzle={:.0f}°C, bed={:.0f}°C",
                  filename, meta.estimated_time_seconds, meta.layer_count, nozzle_target,
                  bed_target);

    dispatch_print_state_notification("printing");
    return true;
}

bool MoonrakerClientMock::pause_print_internal() {
    MockPrintPhase current_phase = print_phase_.load();

    // Can only pause from PRINTING or PREHEAT
    if (current_phase != MockPrintPhase::PRINTING && current_phase != MockPrintPhase::PREHEAT) {
        spdlog::warn("[MoonrakerClientMock] Cannot pause - not currently printing (phase={})",
                     static_cast<int>(current_phase));
        return false;
    }

    // Record pause start time
    pause_start_time_ = std::chrono::steady_clock::now();

    // Transition to PAUSED
    print_phase_.store(MockPrintPhase::PAUSED);
    print_state_.store(2); // "paused" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Print paused at {:.1f}% progress",
                 print_progress_.load() * 100.0);

    dispatch_print_state_notification("paused");
    return true;
}

bool MoonrakerClientMock::resume_print_internal() {
    if (print_phase_.load() != MockPrintPhase::PAUSED) {
        spdlog::warn("[MoonrakerClientMock] Cannot resume - not currently paused");
        return false;
    }

    // Calculate pause duration and add to total
    auto pause_real = std::chrono::steady_clock::now() - pause_start_time_;
    double pause_sim =
        std::chrono::duration<double>(sim_speed().accelerate_progress(pause_real)).count();
    total_pause_duration_sim_ += pause_sim;

    // Resume to PRINTING phase (skip PREHEAT since temps should still be maintained)
    print_phase_.store(MockPrintPhase::PRINTING);
    print_state_.store(1); // "printing" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Print resumed (pause duration: {:.1f}s simulated)",
                 pause_sim);

    dispatch_print_state_notification("printing");
    // Clear any pause-reason message Klipper had set (mirrors real firmware behavior).
    {
        json clear_msg;
        clear_msg["print_stats"]["message"] = "";
        dispatch_status_update(clear_msg);
    }
    return true;
}

bool MoonrakerClientMock::cancel_print_internal() {
    MockPrintPhase current_phase = print_phase_.load();

    // Can cancel from any non-idle phase
    if (current_phase == MockPrintPhase::IDLE) {
        spdlog::warn("[MoonrakerClientMock] Cannot cancel - no active print");
        return false;
    }

    // Set targets to 0 (begin cooldown)
    extruder_target_.store(0.0);
    bed_target_.store(0.0);
    chamber_target_.store(0.0);

    // Reset PRINT_START simulation phase
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Transition to CANCELLED
    print_phase_.store(MockPrintPhase::CANCELLED);
    print_state_.store(4); // "cancelled" for backward compatibility

    spdlog::debug("[MoonrakerClientMock] Print cancelled at {:.1f}% progress",
                  print_progress_.load() * 100.0);

    dispatch_print_state_notification("cancelled");
    return true;
}

void MoonrakerClientMock::emergency_stop_internal() {
    spdlog::warn("[MoonrakerClientMock] Emergency stop executed!");

    // Zero all heater targets
    extruder_target_.store(0.0);
    bed_target_.store(0.0);
    chamber_target_.store(0.0);

    // Turn off all fans
    fan_speed_.store(0);
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        for (auto& [name, speed] : fan_speeds_) {
            speed = 0.0;
        }
    }

    // Reset PRINT_START simulation phase
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Set print state to error (matches real Klipper M112 behavior)
    print_phase_.store(MockPrintPhase::ERROR);
    print_state_.store(5); // error
    dispatch_print_state_notification("error");

    // Klippy enters SHUTDOWN, reported both ways Moonraker does: a webhooks
    // frame carrying the state with Klipper's reason, and notify_klippy_shutdown
    // (which lands on the global PrinterState, so must defer to main thread).
    klippy_state_.store(KlippyState::SHUTDOWN);
    dispatch_status_update(
        {{"webhooks", {{"state", "shutdown"}, {"state_message", kM112ShutdownMessage}}}});
    helix::ui::queue_update("MoonrakerClientMock::estop_shutdown", []() {
        get_printer_state().set_klippy_state_sync(helix::KlippyState::SHUTDOWN);
    });
}

bool MoonrakerClientMock::toggle_filament_runout() {
    // Find primary runout sensor from filament_sensors list
    std::string runout_sensor;
    for (const auto& sensor : discovery_.filament_sensors()) {
        if (sensor.find("runout") != std::string::npos) {
            runout_sensor = sensor;
            break;
        }
    }

    // Fallback to first sensor if no "runout" sensor found
    if (runout_sensor.empty() && !discovery_.filament_sensors().empty()) {
        runout_sensor = discovery_.filament_sensors()[0];
    }

    if (runout_sensor.empty()) {
        spdlog::warn("[MoonrakerClientMock] No filament sensor to toggle");
        return false;
    }

    // Toggle state
    bool new_state = !filament_runout_state_.load();
    filament_runout_state_.store(new_state);

    spdlog::info("[MoonrakerClientMock] Filament toggle on '{}': {} -> {}", runout_sensor,
                 new_state ? "empty" : "detected", new_state ? "detected" : "empty");

    // Dispatch status update through normal flow
    json status;
    status[runout_sensor]["filament_detected"] = new_state;
    dispatch_status_update(status);

    // Auto-pause if: runout detected + actively printing + runout modal enabled
    // This simulates Klipper's pause_on_runout behavior
    if (!new_state) { // new_state=false means filament NOT detected (runout)
        MockPrintPhase phase = print_phase_.load();
        if (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT) {
            if (get_runtime_config()->should_show_runout_modal()) {
                spdlog::info("[MoonrakerClientMock] Filament runout during print - auto-pausing");
                pause_print_internal();
                // Mirror Klipper's runout_helper: set print_stats.message so the
                // UI can surface the reason under the "Print Paused" badge.
                json msg_status;
                msg_status["print_stats"]["message"] =
                    "Filament Sensor " + runout_sensor + ": Runout Detected";
                dispatch_status_update(msg_status);
            }
        }
    }

    return true;
}

// ============================================================================
// Simulation Helpers
// ============================================================================

bool MoonrakerClientMock::is_temp_stable(double current, double target, double tolerance) const {
    return std::abs(current - target) <= tolerance;
}

void MoonrakerClientMock::advance_print_progress(double dt_simulated) {
    double total_time;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        total_time = print_metadata_.estimated_time_seconds;
    }

    if (total_time <= 0) {
        return;
    }

    double rate = 1.0 / total_time; // Progress per simulated second
    double current = print_progress_.load();
    print_progress_.store(std::min(1.0, current + rate * dt_simulated));
}

void MoonrakerClientMock::dispatch_print_state_notification(const std::string& state) {
    // Include filename in state notifications so observers can update immediately
    // This is critical for PrintStatusPanel to load the thumbnail when print starts
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    spdlog::debug(
        "[MoonrakerClientMock] dispatch_print_state_notification: state='{}' filename='{}'", state,
        filename);
    json notification_status = {{"print_stats", {{"state", state}, {"filename", filename}}}};
    dispatch_status_update(notification_status);
}

void MoonrakerClientMock::dispatch_enhanced_print_status() {
    double progress = print_progress_.load();
    int current_layer = get_current_layer();
    int total_layers;
    double total_time;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        total_layers = static_cast<int>(print_metadata_.layer_count);
        total_time = print_metadata_.estimated_time_seconds;
    }

    double elapsed = progress * total_time;

    std::string filename;
    double filament_total_mm;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        filament_total_mm = print_metadata_.filament_mm;
    }

    // Simulate filament consumption proportional to progress
    double filament_used = (filament_total_mm > 0) ? progress * filament_total_mm : 0.0;

    MockPrintPhase phase = print_phase_.load();
    bool is_active = (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT);

    json status = {{"print_stats",
                    {{"state", get_print_state_string()},
                     {"filename", filename},
                     {"print_duration", elapsed},
                     {"total_duration", elapsed},    // Wall-clock elapsed (matches real Moonraker)
                     {"estimated_time", total_time}, // Slicer estimate (for completion modal)
                     {"filament_used", filament_used},
                     {"message", ""},
                     {"info", {{"current_layer", current_layer}, {"total_layer", total_layers}}}}},
                   {"virtual_sdcard",
                    {{"file_path", filename}, {"progress", progress}, {"is_active", is_active}}}};

    // Build exclude_object status — only send excluded_objects + current_object
    // (objects list with geometry was sent once at print start from GCode parsing)
    {
        json excluded_array = json::array();
        std::string current;

        if (mock_state_) {
            auto names = mock_state_->get_object_names();
            auto excl = mock_state_->get_excluded_objects();
            for (const auto& obj : excl) {
                excluded_array.push_back(obj);
            }
            // Pick first non-excluded object as current during active printing
            if (!names.empty() && is_active) {
                for (const auto& n : names) {
                    if (excl.count(n) == 0) {
                        current = n;
                        break;
                    }
                }
            }
        } else {
            // Fallback to local state for backward compatibility
            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
            for (const auto& obj : excluded_objects_) {
                excluded_array.push_back(obj);
            }
            if (!object_names_.empty() && is_active) {
                for (const auto& n : object_names_) {
                    if (excluded_objects_.count(n) == 0) {
                        current = n;
                        break;
                    }
                }
            }
        }

        // Only send excluded_objects + current_object in periodic updates
        // (objects list with geometry was sent once at print start)
        status["exclude_object"] = {
            {"excluded_objects", excluded_array},
            {"current_object", current.empty() ? json(nullptr) : json(current)}};
    }

    dispatch_status_update(status);
}

// ============================================================================
// Temperature Simulation
// ============================================================================

void MoonrakerClientMock::read_position_snapshot(double& x, double& y, double& z) const {
    std::lock_guard<std::mutex> pos_lock(pos_mutex_);
    x = pos_x_.load();
    y = pos_y_.load();
    z = pos_z_.load();
}

void MoonrakerClientMock::dispatch_initial_state() {
    // Build initial state JSON matching real Moonraker subscription response format
    // Uses current simulated values (room temp by default, or preset values if set)
    double ext_temp = extruder_temp_.load();
    double ext_target = extruder_target_.load();
    double bed_temp_val = bed_temp_.load();
    double bed_target_val = bed_target_.load();
    double x, y, z;
    read_position_snapshot(x, y, z);
    int speed = speed_factor_.load();
    int flow = flow_factor_.load();
    int fan = fan_speed_.load();

    const std::string homed = get_homed_axes();

    // Get print state with thread safety
    std::string print_state_str = get_print_state_string();
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    double progress = print_progress_.load();

    // Build LED state JSON
    json led_json = json::object();
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        for (const auto& [name, color] : led_states_) {
            led_json[name] = {{"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
        }
    }
    // Add output_pin status (uses {value:} format, not color_data)
    for (const auto& led : discovery_.leds()) {
        if (led.rfind("output_pin ", 0) == 0) {
            led_json[led] = {{"value", 0.75}}; // Mock: enclosure at 75% brightness
        }
    }

    // Get Z offset and klippy state
    double z_offset = gcode_offset_z_.load();
    KlippyState klippy = klippy_state_.load();
    std::string klippy_str = "ready";
    switch (klippy) {
    case KlippyState::STARTUP:
        klippy_str = "startup";
        break;
    case KlippyState::SHUTDOWN:
        klippy_str = "shutdown";
        break;
    case KlippyState::ERROR:
        klippy_str = "error";
        break;
    default:
        break;
    }

    json initial_status = {
        {"extruder",
         {{"temperature", ext_temp},
          {"target", ext_target},
          {"power", mock_heater_duty(ext_temp, ext_target)}}},
        {"heater_bed",
         {{"temperature", bed_temp_val},
          {"target", bed_target_val},
          {"power", mock_heater_duty(bed_temp_val, bed_target_val)}}},
        {"toolhead",
         {{"position", {x, y, z, 0.0}},
          {"homed_axes", homed},
          {"axis_minimum",
           mock_internal::toolhead_axis_minimum(helix::mock::descriptor(printer_type_))},
          {"axis_maximum",
           mock_internal::toolhead_axis_maximum(helix::mock::descriptor(printer_type_))},
          {"kinematics", kinematics()}}},
        {"gcode_move",
         {{"gcode_position", {x, y, z, 0.0}}, // Commanded position (same as toolhead in mock)
          {"speed_factor", speed / 100.0},
          {"extrude_factor", flow / 100.0},
          {"homing_origin", {gcode_offset_x_.load(), gcode_offset_y_.load(), z_offset, 0.0}}}},
        {"fan", {{"speed", fan / 255.0}}},
        {"webhooks",
         {{"state", klippy_str},
          {"state_message",
           klippy == KlippyState::SHUTDOWN ? kM112ShutdownMessage : "Printer is ready"}}},
        {"print_stats", {{"state", print_state_str}, {"filename", filename}}},
        {"virtual_sdcard", {{"progress", progress}}},
        {"bed_mesh", bed_mesh_status()}};

    // Include exclude_object initial state (empty - no objects defined until print starts)
    initial_status["exclude_object"] = {{"objects", json::array()},
                                        {"excluded_objects", json::array()},
                                        {"current_object", nullptr}};

    // Merge LED states into initial_status (each LED is a top-level key)
    for (auto& [key, value] : led_json.items()) {
        initial_status[key] = value;
    }

    // Auto-controlled heater_fans trip at 50°C (Klipper default). Mirror here
    // so the initial subscription response reports the correct hotend fan speed.
    {
        double hotend_fan_speed = (ext_temp > 50.0) ? 1.0 : 0.0;
        for (const auto& fan_name : discovery_.fans()) {
            if (fan_name.rfind("heater_fan ", 0) == 0) {
                initial_status[fan_name] = {{"speed", hotend_fan_speed}};
            }
        }
    }

    // Override fan speeds with explicitly-set values from fan_speeds_ map
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        for (const auto& [name, spd] : fan_speeds_) {
            if (name == "fan") {
                initial_status["fan"] = {{"speed", spd}};
            } else {
                initial_status[name] = {{"speed", spd}};
            }
        }
    }

    // Add temperature sensor data for all sensors in the discovery sensors list
    for (const auto& s : discovery_.sensors()) {
        if (s.rfind("temperature_sensor ", 0) == 0) {
            std::string sensor_name = s.substr(19);
            double temp = 25.0;
            if (sensor_name.find("chamber") != std::string::npos) {
                temp = chamber_temp_.load();
            } else if (sensor_name.find("mcu") != std::string::npos) {
                temp = mcu_temp_.load();
            } else if (sensor_name.find("raspberry") != std::string::npos ||
                       sensor_name.find("host") != std::string::npos || sensor_name == "rpi") {
                temp = host_temp_.load();
            } else {
                temp = 30.0; // Generic sensor initial value
            }
            initial_status[s] = {{"temperature", temp}};
        } else if (s.rfind("temperature_fan ", 0) == 0) {
            // A chamber-named thermostat parks at its configured resting target
            // (the K2's stock 35C); every other temperature_fan carries the
            // K1C-style spin-up threshold 40C.
            const bool is_chamber_fan = s.find("chamber") != std::string::npos;
            initial_status[s] = {
                {"temperature", 35.0},
                {"target", is_chamber_fan ? mock_internal::kMockK2ChamberFanRestingC : 40.0},
                {"speed", 0.0}};
        } else if (s.rfind("tmc2240 ", 0) == 0 || s.rfind("tmc5160 ", 0) == 0) {
            // TMC stepper drivers with built-in temperature
            double temp = 55.0 + (std::hash<std::string>{}(s) % 20);
            initial_status[s] = {{"temperature", temp}};
        }
    }

    // Add filament sensor states
    // Add state for each discovered filament sensor
    for (const auto& sensor : discovery_.filament_sensors()) {
        // Extract sensor name from "filament_switch_sensor fsensor" -> "fsensor"
        size_t space = sensor.rfind(' ');
        std::string short_name = (space != std::string::npos) ? sensor.substr(space + 1) : sensor;

        const auto env = helix::sim::mock_filament_sensor_env_state(short_name);
        initial_status[sensor] = {{"filament_detected", env.filament_detected},
                                  {"enabled", env.enabled}};
    }

    // On/off inputs: a door switch reads PRESSED (closed) unless
    // HELIX_MOCK_DOOR_OPEN is set; every other button reads RELEASED.
    {
        const bool doors_open = std::getenv("HELIX_MOCK_DOOR_OPEN") != nullptr;
        for (const auto& name : discovery_.hardware().objects_of({helix::hw::ObjectKind::Input})) {
            const bool is_door =
                helix::sensors::default_input_role(helix::hw::classify_object(name).instance) ==
                helix::sensors::InputSensorRole::DOOR;
            initial_status[name] = {{"state", is_door && !doors_open ? "PRESSED" : "RELEASED"}};
        }
    }

    // The chamber and width sensors carry status only where the persona reports them.
    using helix::mock::inherits_default;
    namespace default_object = helix::mock::default_object;
    if (inherits_default(printer_type_, default_object::CHAMBER_SENSOR) || has_chamber_sensor()) {
        initial_status["temperature_sensor chamber"] = {{"temperature", 42.3}};
    }
    // The chamber heater: the persona's own, else the inherited default one
    // (which rides with the chamber sensor).
    {
        const auto heater_key = chamber_heater_status_key();
        if (!heater_key.empty() ||
            inherits_default(printer_type_, default_object::CHAMBER_SENSOR)) {
            initial_status[heater_key.empty() ? "heater_generic chamber" : heater_key] = {
                {"temperature", 42.3},
                {"target", chamber_target_.load()},
                {"power", mock_heater_duty(42.3, chamber_target_.load())}};
        }
    }
    if (inherits_default(printer_type_, default_object::kWidthSensor)) {
        // Hall-effect filament diameter measurement
        initial_status["hall_filament_width_sensor"] = {
            {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};
    }
    if (printer_type_ == PrinterType::CREALITY_K2_PLUS) {
        append_k2_status(initial_status);
        // The box starts with nothing loaded, so the toolhead switch sees none.
        initial_status["filament_switch_sensor filament_sensor"] = {
            {"filament_detected", cfs_toolhead_filament_detected()}, {"enabled", true}};
    }

    // Probe objects (the same ones populate_capabilities() lists)
    // (assigned, not merge_patch'd: a patch drops the null fields they carry).
    initial_status.update(helix::sim::mock_probe_status(printer_type_));

    // Chamber backend diagnostics + filter pin (e.g. dragonbreath trio via
    // HELIX_MOCK_OBJECTS). Tail of the builder; keys are distinct from every
    // merge above (the LED loop only walks profile LEDs in discovery_.leds(),
    // which never contains env-materialized pins).
    append_chamber_backend_status(initial_status, 0.0);
    append_aux_heater_status(initial_status, 0.0);

    spdlog::debug("[MoonrakerClientMock] Dispatching initial state: extruder={}/{}°C, bed={}/{}°C, "
                  "homed_axes='{}', leds={}, filament_sensors={}",
                  ext_temp, ext_target, bed_temp_val, bed_target_val, homed, led_json.size(),
                  discovery_.filament_sensors().size());

    // The discovery subscription is built before configfile is read here, so
    // the leveling-skip objects it never asked for are announced with the rest.
    initial_status.update(skip_wrapper_status());
    initial_status.update(layer_pause_status());
    if (is_mock_indx()) {
        const nlohmann::json indx = indx_status_json();
        initial_status.update(indx);
        std::lock_guard<std::mutex> lock(indx_mutex_);
        indx_published_ = indx;
    }

    // multiACE mock: the `ace` object and both filament_feed sides at rest, so
    // the production backend's first parse sees what a booted rig reports.
    if (is_mock_multiace()) {
        initial_status["ace"] = multiace_status_json();
        initial_status["filament_feed left"] = u1_feed_side_json(0);
        initial_status["filament_feed right"] = u1_feed_side_json(2);
    }

    // Use the base class dispatch method (same as real client)
    dispatch_status_update(initial_status);
}

TemperatureStore MoonrakerClientMock::build_historical_temperature_store() const {
    // ~10 minutes of 1 Hz history emitted as Moonraker's flat per-key arrays.
    // Profile: heat-up ramp -> hold at target (PID ripple) -> exponential
    // cooldown. Deterministic (fixed pseudo-random seed) so the curve is stable
    // across runs. Ends near ambient, matching the idle live simulation so the
    // seeded history joins the live graph without a visible step (#944).
    constexpr int kSamples = 600;     // 10 min @ 1 Hz
    constexpr int kHeatSamples = 90;  // ~90 s ramp to target
    constexpr int kHoldSamples = 150; // ~2.5 min at target
    // remaining ~360 s: exponential cooldown back toward ambient
    constexpr double kPeakExtruder = 215.0;
    constexpr double kPeakBed = 60.0;
    constexpr double kExtTau = 90.0;  // extruder cooldown time constant (s)
    constexpr double kBedTau = 140.0; // bed cools slower

    // Deterministic pseudo-random noise in [-1, 1] (no std::random_device).
    uint32_t rng_state = 22221;
    auto noise = [&rng_state](double amplitude) -> double {
        rng_state = (rng_state * 1103515245u + 12345u) & 0x7fffffff;
        return ((static_cast<double>(rng_state) / 0x3fffffff) - 1.0) * amplitude;
    };

    TemperatureStore store;
    TemperatureStoreSeries& ext = store["extruder"];
    TemperatureStoreSeries& bed = store["heater_bed"];
    for (TemperatureStoreSeries* s : {&ext, &bed}) {
        s->temperatures.reserve(kSamples);
        s->targets.reserve(kSamples);
        s->powers.reserve(kSamples);
    }

    for (int i = 0; i < kSamples; ++i) {
        double ext_temp, bed_temp, ext_target, bed_target, ext_power, bed_power;
        if (i < kHeatSamples) {
            // Heating: linear ramp toward target, full heater power.
            double p = static_cast<double>(i) / kHeatSamples;
            ext_temp = kRoomTemp + (kPeakExtruder - kRoomTemp) * p;
            bed_temp = kRoomTemp + (kPeakBed - kRoomTemp) * std::min(1.0, p * 1.2);
            ext_target = kPeakExtruder;
            bed_target = kPeakBed;
            ext_power = 1.0;
            bed_power = 1.0;
        } else if (i < kHeatSamples + kHoldSamples) {
            // Hold: small PID oscillation around target.
            double o = i - kHeatSamples;
            ext_temp = kPeakExtruder + 0.8 * std::sin(o * 0.15) + 0.3 * std::cos(o * 0.31);
            bed_temp = kPeakBed + 0.4 * std::sin(o * 0.12);
            ext_target = kPeakExtruder;
            bed_target = kPeakBed;
            ext_power = 0.30 + 0.10 * std::sin(o * 0.15);
            bed_power = 0.25 + 0.08 * std::sin(o * 0.12);
        } else {
            // Cooldown: heaters off, exponential decay toward ambient.
            double ct = i - kHeatSamples - kHoldSamples; // seconds since cooldown start
            ext_temp = kRoomTemp + (kPeakExtruder - kRoomTemp) * std::exp(-ct / kExtTau);
            bed_temp = kRoomTemp + (kPeakBed - kRoomTemp) * std::exp(-ct / kBedTau);
            ext_target = 0.0;
            bed_target = 0.0;
            ext_power = 0.0;
            bed_power = 0.0;
        }

        ext.temperatures.push_back(static_cast<float>(ext_temp + noise(0.3)));
        ext.targets.push_back(static_cast<float>(ext_target));
        ext.powers.push_back(static_cast<float>(ext_power));
        bed.temperatures.push_back(static_cast<float>(bed_temp + noise(0.2)));
        bed.targets.push_back(static_cast<float>(bed_target));
        bed.powers.push_back(static_cast<float>(bed_power));
    }

    // Discovered temperature sensors: gentle sinusoids, no target/power.
    for (const auto& sensor : discovery_.sensors()) {
        if (sensor.rfind("temperature_sensor ", 0) != 0) {
            continue;
        }
        const std::string name = sensor.substr(19); // strip "temperature_sensor " prefix
        TemperatureStoreSeries& series = store[sensor];
        series.temperatures.reserve(kSamples);
        for (int i = 0; i < kSamples; ++i) {
            double t_sec = -(kSamples - i); // negative = in the past
            double temp;
            if (name.find("chamber") != std::string::npos) {
                temp = 35.0 + 8.0 * std::sin(2.0 * M_PI * t_sec / 300.0);
            } else if (name.find("mcu") != std::string::npos) {
                temp = 42.0 + 3.0 * std::sin(2.0 * M_PI * t_sec / 200.0);
            } else {
                temp = 30.0 + 2.0 * std::sin(2.0 * M_PI * t_sec / 180.0);
            }
            series.temperatures.push_back(static_cast<float>(temp + noise(0.4)));
        }
    }

    return store;
}

void MoonrakerClientMock::dispatch_historical_temperatures() {
    // Generate 2-3 minutes of synthetic temperature history
    // At 250ms intervals, that's ~600 data points for 2.5 minutes
    constexpr int kHistoryDurationMs = 150000; // 2.5 minutes of history
    constexpr int kSampleIntervalMs = 250;     // Same as kSimulationIntervalMs
    constexpr int kHistorySamples = kHistoryDurationMs / kSampleIntervalMs;

    spdlog::debug(
        "[MoonrakerClientMock] Dispatching {} historical temperature samples ({} seconds)",
        kHistorySamples, kHistoryDurationMs / 1000);

    // Simulate a realistic temperature profile: heating up to ~60°C then partial cooldown
    // This creates an interesting curve for debugging/visualization
    //
    // Profile: Start at room temp -> heat to 60°C (extruder) / 40°C (bed) -> partial cooldown
    // Timing: ~50s heating, ~30s hold, ~70s cooling (ends at ~35°C extruder, ~30°C bed)
    constexpr double kPeakExtruderTemp = 60.0;
    constexpr double kPeakBedTemp = 40.0;
    constexpr int kHeatPhaseSamples = 200; // ~50 seconds at 250ms = 200 samples
    constexpr int kHoldPhaseSamples = 120; // ~30 seconds hold at peak
    // Cooling phase = remaining samples (~70s, cools extruder ~20°C to ~40°C)

    // Copy callbacks to avoid holding lock during dispatch
    std::vector<helix::SharedCallback<void(const json&)>> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_copy.reserve(notify_callbacks_.size());
        for (const auto& [id, cb] : notify_callbacks_) {
            callbacks_copy.push_back(cb);
        }
    }

    // If no callbacks registered yet, skip (caller should register before connect)
    if (callbacks_copy.empty()) {
        spdlog::warn(
            "[MoonrakerClientMock] No callbacks registered for historical temps - skipping");
        return;
    }

    // Generate and dispatch historical samples with realistic noise
    double ext_temp_hist = kRoomTemp;
    double bed_temp_hist = kRoomTemp;
    const double dt_sec = kSampleIntervalMs / 1000.0;

    // Simple pseudo-random number generator for deterministic noise
    // (Avoids std::random_device which could affect startup time)
    auto pseudo_random = [](int seed) -> double {
        // Linear congruential generator with normalized output [-1, 1]
        static uint32_t state = 12345;
        state = (state * 1103515245 + seed + 12345) & 0x7fffffff;
        return (static_cast<double>(state) / 0x3fffffff) - 1.0;
    };

    for (int i = 0; i < kHistorySamples; i++) {
        // Calculate simulated timestamp (negative = in the past)
        double timestamp_sec = -((kHistorySamples - i) * dt_sec);

        // Update base temperatures based on phase
        if (i < kHeatPhaseSamples) {
            // Heating phase: ramp up to peak (slightly faster at start, slower near target)
            double progress = static_cast<double>(i) / kHeatPhaseSamples;
            double rate_multiplier = 1.0 + 0.3 * (1.0 - progress); // Faster early, slower late
            ext_temp_hist += kExtruderHeatRate * dt_sec * rate_multiplier;
            if (ext_temp_hist > kPeakExtruderTemp)
                ext_temp_hist = kPeakExtruderTemp;

            bed_temp_hist += kBedHeatRate * dt_sec * rate_multiplier;
            if (bed_temp_hist > kPeakBedTemp)
                bed_temp_hist = kPeakBedTemp;
        } else if (i < kHeatPhaseSamples + kHoldPhaseSamples) {
            // Hold phase: PID oscillation around target (realistic behavior)
            double offset = i - kHeatPhaseSamples;
            ext_temp_hist =
                kPeakExtruderTemp + 0.8 * std::sin(offset * 0.15) + 0.3 * std::cos(offset * 0.31);
            bed_temp_hist =
                kPeakBedTemp + 0.4 * std::sin(offset * 0.12) + 0.15 * std::cos(offset * 0.27);
        } else {
            // Cooling phase: exponential decay (more realistic than linear)
            int cool_sample = i - kHeatPhaseSamples - kHoldPhaseSamples;
            double cool_time = cool_sample * dt_sec;
            // Exponential decay: T(t) = T_ambient + (T_0 - T_ambient) * e^(-t/tau)
            double ext_tau = 40.0; // Extruder thermal time constant (seconds)
            double bed_tau = 80.0; // Bed thermal time constant (slower)
            ext_temp_hist =
                kRoomTemp + (kPeakExtruderTemp - kRoomTemp) * std::exp(-cool_time / ext_tau);
            bed_temp_hist = kRoomTemp + (kPeakBedTemp - kRoomTemp) * std::exp(-cool_time / bed_tau);
        }

        // Add realistic sensor noise (±0.3°C for extruder, ±0.2°C for bed)
        double ext_noise = pseudo_random(i * 2) * 0.3;
        double bed_noise = pseudo_random(i * 2 + 1) * 0.2;

        double ext_with_noise = ext_temp_hist + ext_noise;
        double bed_with_noise = bed_temp_hist + bed_noise;

        // Build minimal status object (only temperature data needed for graphs)
        json status_obj = {{"extruder", {{"temperature", ext_with_noise}, {"target", 0.0}}},
                           {"heater_bed", {{"temperature", bed_with_noise}, {"target", 0.0}}}};

        // Add width sensor data (Hall-effect filament diameter measurement) on
        // every frame of a persona that has one, so WidthSensorManager receives updates
        if (helix::mock::inherits_default(printer_type_,
                                          helix::mock::default_object::kWidthSensor)) {
            status_obj["hall_filament_width_sensor"] = {
                {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};
        }

        // Add historical temperature data for all temperature sensors
        for (const auto& s : discovery_.sensors()) {
            if (s.rfind("temperature_sensor ", 0) == 0) {
                std::string sensor_name = s.substr(19);
                double temp = 25.0;
                double noise = pseudo_random(i * 3) * 0.5;
                if (sensor_name.find("chamber") != std::string::npos) {
                    constexpr double kChamberMin = 25.0, CHAMBER_MAX = 45.0, CHAMBER_PERIOD = 120.0;
                    double mid = (kChamberMin + CHAMBER_MAX) / 2.0;
                    double amp = (CHAMBER_MAX - kChamberMin) / 2.0;
                    temp = mid + amp * std::sin(2.0 * M_PI * timestamp_sec / CHAMBER_PERIOD);
                } else if (sensor_name.find("mcu") != std::string::npos) {
                    temp = 42.0 + 3.0 * std::sin(2.0 * M_PI * timestamp_sec / 120.0);
                } else if (sensor_name.find("raspberry") != std::string::npos ||
                           sensor_name.find("host") != std::string::npos || sensor_name == "rpi") {
                    temp = 52.0 + 4.0 * std::sin(2.0 * M_PI * timestamp_sec / 75.0);
                } else {
                    temp = 30.0 + 2.0 * std::sin(2.0 * M_PI * timestamp_sec / 100.0);
                }
                status_obj[s] = {{"temperature", temp + noise}};
            } else if (s.rfind("temperature_fan ", 0) == 0) {
                double temp = 35.0 + 3.0 * std::sin(2.0 * M_PI * timestamp_sec / 80.0);
                const bool is_chamber_fan = s.find("chamber") != std::string::npos;
                status_obj[s] = {
                    {"temperature", temp},
                    {"target", is_chamber_fan ? mock_internal::kMockK2ChamberFanRestingC : 40.0},
                    {"speed", 0.5}};
            }
        }

        json notification = {{"method", "notify_status_update"},
                             {"params", json::array({status_obj, timestamp_sec})}};

        // Dispatch to all callbacks
        for (const auto& cb : callbacks_copy) {
            if (cb) {
                cb(notification);
            }
        }
    }

    // Store final historical values as current temps
    extruder_temp_.store(ext_temp_hist);
    bed_temp_.store(bed_temp_hist);
    // Store chamber temp at midpoint for initial state
    if (simulates_chamber_temp()) {
        chamber_temp_.store(35.0);
    }

    spdlog::debug("[MoonrakerClientMock] Historical temps dispatched: final extruder={:.1f}°C, "
                  "bed={:.1f}°C",
                  ext_temp_hist, bed_temp_hist);
}

void MoonrakerClientMock::dispatch_method_callback(const std::string& method, const json& msg) {
    std::vector<helix::SharedCallback<void(const json&)>> callbacks_to_invoke;

    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto method_it = method_callbacks_.find(method);
        if (method_it != method_callbacks_.end()) {
            for (auto& [handler_name, cb] : method_it->second) {
                callbacks_to_invoke.push_back(cb);
            }
        }
    }

    // Invoke callbacks outside the lock to prevent deadlocks
    for (auto& cb : callbacks_to_invoke) {
        cb(msg);
    }
}

void MoonrakerClientMock::dispatch_status_update(const json& status, bool from_cached_snapshot,
                                                 bool whole_objects) {
    MoonrakerClient::dispatch_status_update(status, from_cached_snapshot, whole_objects);

    dispatch_method_callback(
        "notify_status_update",
        helix::make_status_notification(status, from_cached_snapshot, whole_objects));
}

bool MoonrakerClientMock::led_effect_enabled(const std::string& object_name) const {
    std::lock_guard<std::mutex> lock(led_mutex_);
    return enabled_led_effects_.count(object_name) > 0;
}

// ============================================================================
// Fan Control Helper Methods
// ============================================================================

void MoonrakerClientMock::set_fan_speed_internal(const std::string& fan_name, double speed) {
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        fan_speeds_[fan_name] = speed;
    }

    // Also update the legacy fan_speed_ atomic for backward compatibility
    // (only for part cooling fan "fan")
    if (fan_name == "fan") {
        fan_speed_.store(static_cast<int>(speed * 255.0));
    }

    // Dispatch fan status update
    json fan_status;
    if (fan_name == "fan") {
        // Part cooling fan uses simple format
        fan_status["fan"] = {{"speed", speed}};
    } else {
        // Generic/heater fans use full name as key
        fan_status[fan_name] = {{"speed", speed}};
    }
    dispatch_status_update(fan_status);
}

std::string MoonrakerClientMock::find_fan_by_suffix(const std::string& suffix) const {
    for (const auto& fan : discovery_.fans()) {
        // Match if fan name ends with the suffix (e.g., "nevermore" matches "fan_generic
        // nevermore")
        if (fan.length() >= suffix.length()) {
            size_t suffix_start = fan.length() - suffix.length();
            if (fan.substr(suffix_start) == suffix) {
                return fan;
            }
        }
    }
    return "";
}

// ============================================================================
// G-code Offset Helper Methods
// ============================================================================

void MoonrakerClientMock::dispatch_gcode_move_update() {
    double z_offset = gcode_offset_z_.load();
    int speed = speed_factor_.load();
    int flow = flow_factor_.load();
    double x, y, z;
    read_position_snapshot(x, y, z);

    json gcode_move = {
        {"gcode_move",
         {{"gcode_position", {x, y, z, 0.0}},
          {"speed_factor", speed / 100.0},
          {"extrude_factor", flow / 100.0},
          {"homing_origin", {gcode_offset_x_.load(), gcode_offset_y_.load(), z_offset, 0.0}}}}};
    dispatch_status_update(gcode_move);
}

std::array<double, 3> MoonrakerClientMock::nozzle_offset(int head) const {
    std::lock_guard<std::mutex> lock(nozzle_cal_mutex_);
    if (head < 0 || head >= static_cast<int>(nozzle_offsets_.size())) {
        return {0.0, 0.0, 0.0};
    }
    return nozzle_offsets_[static_cast<size_t>(head)];
}

bool MoonrakerClientMock::nozzle_calibration_active() const {
    std::lock_guard<std::mutex> lock(nozzle_cal_mutex_);
    return nozzle_cal_active_;
}

std::pair<double, std::string> MoonrakerClientMock::nozzle_properties(int head) const {
    std::lock_guard<std::mutex> lock(nozzle_cal_mutex_);
    if (head < 0 || head >= static_cast<int>(nozzle_props_.size())) {
        return {0.0, {}};
    }
    return nozzle_props_[static_cast<size_t>(head)];
}

json MoonrakerClientMock::set_nozzle_properties(const json& params) {
    // The firmware's own checks, in its order (extruder.py
    // _handle_control_nozzle_properties): it cannot read a call with no diameter,
    // it compares each value exactly against its lists, and a diameter is applied
    // before the flow type is checked, so a bad flow type still leaves a good
    // diameter set.
    static constexpr std::array<double, 4> kValidDiameters = {0.2, 0.4, 0.6, 0.8};
    static constexpr std::array<const char*, 2> kValidTypes = {"standard", "high_flow"};
    auto refuse = [](std::string message) {
        return json{{"state", "error"}, {"message", std::move(message)}};
    };

    const auto ext = params.find("extruder");
    if (ext == params.end() || !ext->is_number_integer()) {
        return refuse("extruder must be specified!");
    }
    if (const auto d = params.find("diameter"); d == params.end() || d->is_null()) {
        return refuse("float() argument must be a string or a real number, not 'NoneType'");
    }
    const int head = ext->get<int>();
    if (head < 0 || head >= static_cast<int>(nozzle_props_.size())) {
        return refuse("extruder not found!");
    }
    const int state = print_state_.load();
    if (state == 1 || state == 2) { // printing, paused
        return refuse("Cannot change nozzle properties during printing!");
    }

    json changed = json::object();
    json reply = {{"state", "success"}};
    {
        std::lock_guard<std::mutex> lock(nozzle_cal_mutex_);
        auto& nozzle = nozzle_props_[static_cast<size_t>(head)];
        const auto d = params.find("diameter");
        const double mm = d->is_number() ? d->get<double>() : -1.0;
        if (std::find(kValidDiameters.begin(), kValidDiameters.end(), mm) ==
            kValidDiameters.end()) {
            return refuse("nozzle_diameter error: " + d->dump());
        }
        nozzle.first = mm;
        changed["nozzle_diameter"] = mm;
        if (const auto t = params.find("volume_type"); t != params.end() && !t->is_null()) {
            const std::string type = t->is_string() ? t->get<std::string>() : t->dump();
            if (std::find_if(kValidTypes.begin(), kValidTypes.end(),
                             [&type](const char* v) { return type == v; }) == kValidTypes.end()) {
                reply = refuse("nozzle_volume_type error: " + type);
            } else {
                nozzle.second = type;
                changed["nozzle_volume_type"] = type;
            }
        }
    }
    if (!changed.empty()) {
        const std::string object = head == 0 ? "extruder" : "extruder" + std::to_string(head);
        dispatch_status_update({{object, changed}});
        spdlog::info("[MoonrakerClientMock] {} nozzle set: {}", object, changed.dump());
    }
    return reply;
}

bool MoonrakerClientMock::simulate_nozzle_offset_calibration(
    const std::string& script, helix::SharedCallback<void(const nlohmann::json&)> success_cb,
    helix::SharedCallback<void(const MoonrakerError&)> error_cb) {
    constexpr const char* kPrefix = "EXTRUDER_OFFSET_ACTION_";
    if (printer_type_ != PrinterType::SNAPMAKER_U1 || script.rfind(kPrefix, 0) != 0) {
        return false;
    }
    record_gcode_script(script);
    const std::string command = script.substr(0, script.find(' '));
    const std::string step = command.substr(std::string(kPrefix).size());
    int head = -1;
    if (const auto at = script.find("TOOL_ID=T"); at != std::string::npos) {
        head = helix::text_io::parse_leading<int>(script.substr(at + 9)).value_or(-1);
    }

    std::string error;
    {
        std::lock_guard<std::mutex> lock(nozzle_cal_mutex_);
        if (step == "PRESTART") {
            nozzle_cal_active_ = true;
            nozzle_cal_plate_refusals_ = 0;
            if (const char* env = std::getenv("HELIX_MOCK_NOZZLE_CAL_PLATE_REFUSE")) {
                nozzle_cal_plate_refusals_ = helix::text_io::parse_leading<int>(env).value_or(0);
            }
        } else if (!nozzle_cal_active_) {
            error = fmt::format(R"({{"coded": "0003-0530-0000-0009", "msg":"Operation '{}' )"
                                R"(requires XYZ_OFFSET_CALIBRATE main state"}})",
                                command);
        } else if (step == "EXIT") {
            nozzle_cal_active_ = false;
        } else if (step == "DETECT_PLATE" && nozzle_cal_plate_refusals_ > 0) {
            --nozzle_cal_plate_refusals_;
            error = R"({"coded": "0003-0530-0000-0011", "msg":"The plate has not been removed"})";
        } else if (step == "PROBE_CALIBRATE") {
            int fail_head = -1;
            if (const char* env = std::getenv("HELIX_MOCK_NOZZLE_CAL_FAIL")) {
                fail_head = helix::text_io::parse_leading<int>(env).value_or(-1);
            }
            if (head == fail_head) {
                error = "Probe samples exceed samples_tolerance";
            } else if (head >= 0 && head < static_cast<int>(nozzle_offsets_.size())) {
                // A re-measure lands near the old value, not on it.
                for (double& axis : nozzle_offsets_[static_cast<size_t>(head)]) {
                    axis += 0.004 * (head + 1);
                }
            }
        }
    }
    spdlog::info("[MoonrakerClientMock] {}{}", script,
                 error.empty() ? "" : fmt::format(" -> refused: {}", error));

    std::vector<helix::SharedCallback<void()>> steps;
    if (error.empty() && step == "PROBE_CALIBRATE" && head >= 0) {
        steps.emplace_back([this, head] {
            const auto o = nozzle_offset(head);
            dispatch_gcode_response(fmt::format("// extruder{} offset: [{:.6f}, {:.6f}, {:.6f}]",
                                                head == 0 ? "" : std::to_string(head), o[0], o[1],
                                                o[2]));
        });
    }
    helix::SharedCallback<void()> on_done;
    if (error.empty()) {
        on_done = [success_cb = std::move(success_cb)] {
            if (success_cb) {
                success_cb(json{{"result", "ok"}});
            }
        };
    } else {
        on_done = [this, error, error_cb = std::move(error_cb)] {
            dispatch_gcode_response("!! " + error);
            if (error_cb) {
                MoonrakerError err;
                err.message = error;
                error_cb(err);
            }
        };
    }
    // A step is a heat, a wipe or a probe pass on the real machine; a beat is
    // enough to see each one on screen.
    play_console_steps(std::move(steps), 1200, std::move(on_done));
    return true;
}

bool MoonrakerClientMock::save_config_pending() const {
    std::lock_guard<std::mutex> lock(pending_config_mutex_);
    return !pending_config_items_.empty();
}

json MoonrakerClientMock::save_config_pending_items() const {
    std::lock_guard<std::mutex> lock(pending_config_mutex_);
    json items = json::object();
    for (const auto& [section, options] : pending_config_items_) {
        json opts = json::object();
        for (const auto& [option, value] : options) {
            opts[option] = value;
        }
        items[section] = opts;
    }
    return items;
}

void MoonrakerClientMock::stage_config_change(const std::string& section, const std::string& option,
                                              const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(pending_config_mutex_);
        pending_config_items_[section][option] = value;
    }
    dispatch_configfile_update();
}

void MoonrakerClientMock::commit_pending_config() {
    std::map<std::string, std::map<std::string, std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_config_mutex_);
        pending.swap(pending_config_items_);
    }
    if (pending.empty()) {
        return;
    }
    for (const auto& [section, options] : pending) {
        // "tool T<n>" is the only section anything here stages. Others are
        // accepted and cleared without a durable store, which is enough to
        // model the pending flag for features that only care a save is owed.
        if (section.rfind("tool T", 0) != 0) {
            continue;
        }
        for (const auto& [option, value] : options) {
            auto axis = tool_offset_axis(option);
            if (!axis) {
                continue;
            }
            try {
                int tool = std::stoi(section.substr(6));
                std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
                tool_offsets_saved_[tool][*axis] = std::stod(value);
            } catch (...) {
            }
        }
    }
    spdlog::info("[MoonrakerClientMock] SAVE_CONFIG committed {} pending section(s)",
                 pending.size());
    dispatch_configfile_update();
}

void MoonrakerClientMock::dispatch_configfile_update() {
    json update = {{"configfile",
                    {{"save_config_pending", save_config_pending()},
                     {"save_config_pending_items", save_config_pending_items()}}}};
    dispatch_status_update(update);
}

// ============================================================================
// Manual Probe Helper Methods (Z-offset calibration)
// ============================================================================

void MoonrakerClientMock::dispatch_manual_probe_update() {
    bool is_active = manual_probe_active_.load();
    double z_position = manual_probe_z_.load();

    // Build manual_probe status matching Klipper's format:
    // {
    //   "manual_probe": {
    //     "is_active": true/false,
    //     "z_position": float,
    //     "z_position_lower": float (optional),
    //     "z_position_upper": float (optional)
    //   }
    // }
    json manual_probe_status = {
        {"manual_probe",
         {{"is_active", is_active},
          {"z_position", z_position},
          {"z_position_lower", nullptr}, // Not tracking bisection search in mock
          {"z_position_upper", nullptr}}}};

    dispatch_status_update(manual_probe_status);

    spdlog::debug("[MoonrakerClientMock] Dispatched manual_probe update: is_active={}, z={:.3f}",
                  is_active, z_position);
}

// ============================================================================
// G-code Response Simulation (for PRINT_START progress tracking)
// ============================================================================

bool MoonrakerClientMock::simulate_pa_calibration(
    const std::string& script, helix::SharedCallback<void(const nlohmann::json&)> success_cb,
    helix::SharedCallback<void(const MoonrakerError&)> error_cb) {
    if (script.rfind("FLOW_CALIBRATE", 0) != 0) {
        return false;
    }
    record_gcode_script(script);

    const bool should_fail = helix::env_flag("HELIX_MOCK_PA_FAIL");

    // Roughly what the real thing costs once the nozzle is already hot: a
    // handful of purge-and-measure cycles, not an instant answer.
    constexpr int kCandidates = 5;
    constexpr int kStepMs = 1200;

    std::lock_guard<std::mutex> lock(pa_cal_mutex_);
    pending_pa_lines_.clear();
    auto due = std::chrono::steady_clock::now();

    if (should_fail) {
        due += std::chrono::milliseconds(kStepMs);
        pending_pa_lines_.push_back({due, "!! [flow_calibrate] not edit filament info!", true,
                                     std::move(success_cb), std::move(error_cb)});
        spdlog::info("[MoonrakerClientMock] FLOW_CALIBRATE: simulating refusal"
                     " (HELIX_MOCK_PA_FAIL)");
        return true;
    }

    // Candidate probes, in the U1 flow calibrator's own format: each measured
    // K, then the flow mismatch it read there. Shaped like a real root-find
    // converging on 0.0412.
    static constexpr double kCandidateK[kCandidates] = {0.0200, 0.0600, 0.0400, 0.0420, 0.0412};
    static constexpr double kCandidateArea[kCandidates] = {0.0181, -0.0142, 0.0011, -0.0004,
                                                           0.00002};
    for (int i = 0; i < kCandidates; ++i) {
        due += std::chrono::milliseconds(kStepMs);
        pending_pa_lines_.push_back(
            {due, fmt::format("// measure k: {:.5f}", kCandidateK[i]), false, nullptr, nullptr});
        pending_pa_lines_.push_back({due, fmt::format("// measure area: {:.5f}", kCandidateArea[i]),
                                     false, nullptr, nullptr});
    }

    // The result line the firmware prints as it applies the value.
    due += std::chrono::milliseconds(kStepMs);
    pending_pa_lines_.push_back(
        {due, "// Got pressure advance: 0.0412", true, std::move(success_cb), std::move(error_cb)});

    spdlog::info("[MoonrakerClientMock] FLOW_CALIBRATE: simulating {} candidates (~{}s)",
                 kCandidates, ((kCandidates + 1) * kStepMs) / 1000);
    return true;
}

void MoonrakerClientMock::service_pending_pa_lines() {
    std::vector<PendingPaLine> due;
    {
        std::lock_guard<std::mutex> lock(pa_cal_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_pa_lines_.begin(); it != pending_pa_lines_.end();) {
            if (it->due <= now) {
                due.push_back(std::move(*it));
                it = pending_pa_lines_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& item : due) {
        dispatch_gcode_response(item.line);
        if (!item.is_final) {
            continue;
        }
        // A refusal answers the RPC as an error, the way Klipper does; a
        // successful run answers plainly and lets the console line carry the
        // result, which is the contract the collector relies on.
        if (item.line.rfind("!! ", 0) == 0) {
            if (item.error_cb) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::JSON_RPC_ERROR;
                err.message = item.line.substr(3);
                err.method = "printer.gcode.script";
                item.error_cb(err);
            }
        } else if (item.success_cb) {
            item.success_cb(nlohmann::json{{"result", "ok"}});
        }
    }
}
void MoonrakerClientMock::dispatch_gcode_response(const std::string& line) {
    // Build notify_gcode_response message format:
    // {"method": "notify_gcode_response", "params": ["<line>"]}
    json notification = {{"method", "notify_gcode_response"}, {"params", json::array({line})}};

    // Collect callbacks while holding lock, invoke outside
    std::vector<helix::SharedCallback<void(const json&)>> callbacks_to_invoke;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto method_it = method_callbacks_.find("notify_gcode_response");
        if (method_it != method_callbacks_.end()) {
            for (auto& [handler_name, cb] : method_it->second) {
                callbacks_to_invoke.push_back(cb);
            }
        }
    }

    // Invoke callbacks outside lock to prevent deadlock
    for (auto& cb : callbacks_to_invoke) {
        cb(notification);
    }

    spdlog::trace("[MoonrakerClientMock] Dispatched G-code response: {}", line);
}

namespace {

/**
 * @brief Write a mock Klipper-format shaper calibration CSV
 *
 * Generates ~50 frequency bins from 5-200 Hz with a realistic spectrum:
 * base noise floor, a resonance peak, and shaper attenuation curves.
 */
void write_mock_shaper_csv(const std::string& path, char axis) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        spdlog::warn("[MoonrakerClientMock] Failed to write mock CSV to {}", path);
        return;
    }

    // Shaper definitions: name, fitted frequency
    struct ShaperDef {
        const char* name;
        float freq;
    };
    static const ShaperDef shapers[] = {
        {"zv", 59.0f}, {"mzv", 53.8f}, {"ei", 56.2f}, {"2hump_ei", 71.8f}, {"3hump_ei", 89.6f},
    };
    constexpr int num_shapers = 5;

    // Write header line
    ofs << "freq,psd_x,psd_y,psd_z,psd_xyz";
    for (int i = 0; i < num_shapers; ++i) {
        ofs << "," << shapers[i].name << "(" << std::fixed << std::setprecision(1)
            << shapers[i].freq << ")";
    }
    ofs << "\n";

    // RNG for noise variation
    std::mt19937 rng(42 + static_cast<unsigned>(axis)); // Deterministic per-axis
    std::uniform_real_distribution<float> noise_dist(0.8f, 1.2f);

    // Frequency bins the PSD rows (and the shaper curves below) are sampled at
    std::vector<double> bins;
    for (float freq = 5.0f; freq <= 200.0f; freq += 4.0f) {
        bins.push_back(freq);
    }

    // Real transfer curves per shaper (the same math the firmware uses to
    // write these columns), so the shaped-PSD overlays and any client-side
    // re-scoring of the mock data see physically consistent notches rather
    // than a toy quadratic approximation.
    std::vector<std::vector<double>> shaper_curves;
    shaper_curves.reserve(num_shapers);
    for (int i = 0; i < num_shapers; ++i) {
        shaper_curves.push_back(helix::calibration::shaper_transfer_curve(
            shapers[i].name, shapers[i].freq, helix::calibration::kShaperDefaultDampingRatio,
            bins));
    }

    // Resonance peak parameters — should agree with optimal shaper frequencies above
    const float peak_freq = (axis == 'x' || axis == 'X') ? 53.8f : 48.2f;
    const float peak_width = 8.0f; // Hz bandwidth of resonance
    const float peak_amp = 0.02f;  // Peak amplitude
    const float noise_floor = 5e-4f;

    // Generate ~50 bins from 5 to 200 Hz (step ~4 Hz)
    for (size_t bin = 0; bin < bins.size(); ++bin) {
        const float freq = static_cast<float>(bins[bin]);

        // Raw PSD: noise floor + Lorentzian resonance peak
        float df = freq - peak_freq;
        float resonance = peak_amp / (1.0f + (df * df) / (peak_width * peak_width));
        float base_psd = noise_floor * noise_dist(rng) + resonance;

        // High-frequency rolloff above 120 Hz
        if (freq > 120.0f) {
            base_psd *= std::exp(-(freq - 120.0f) / 60.0f);
        }

        // PSD for each axis direction (main axis gets full signal)
        float psd_main = base_psd;
        float psd_cross = base_psd * 0.15f * noise_dist(rng); // Cross-axis coupling
        float psd_z = base_psd * 0.08f * noise_dist(rng);
        float psd_xyz = psd_main + psd_cross + psd_z;

        float psd_x = (axis == 'x' || axis == 'X') ? psd_main : psd_cross;
        float psd_y = (axis == 'y' || axis == 'Y') ? psd_main : psd_cross;

        ofs << std::scientific << std::setprecision(3) << freq << "," << psd_x << "," << psd_y
            << "," << psd_z << "," << psd_xyz;

        // Write transfer function coefficients (0-1), matching real Klipper CSV
        // format. The CSV parser multiplies by raw PSD to get shaped PSD for
        // charting. Shapers without a ported curve degrade to a flat passband.
        for (int i = 0; i < num_shapers; ++i) {
            const double attenuation =
                (bin < shaper_curves[i].size()) ? shaper_curves[i][bin] : 1.0;
            ofs << "," << std::fixed << std::setprecision(3) << attenuation;
        }
        ofs << "\n";
    }

    ofs.close();
    spdlog::info("[MoonrakerClientMock] Wrote mock shaper CSV to {}", path);
}

/**
 * @brief Write a mock TEST_RESONANCES OUTPUT=resonances CSV for one belt path
 *
 * Bins at Klipper's raw-PSD resolution (3200 Hz / 4096 samples), with a
 * Lorentzian at the path's simulated peak over a noise floor plus the
 * ~42 Hz peak every belt rig shows. A NaN peak writes the noise floor alone.
 */
void write_mock_belt_csv(const std::string& path, char path_letter, float peak_hz, double max_freq,
                         BeltMockFailure mode) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        spdlog::warn("[MoonrakerClientMock] Failed to write mock belt CSV to {}", path);
        return;
    }

    if (mode == BeltMockFailure::MULTICHIP) {
        std::fprintf(f, "freq,adxl345,adxl345_hotend\n");
    } else {
        std::fprintf(f, "freq,psd_x,psd_y,psd_z,psd_xyz");
        if (mode == BeltMockFailure::KALICO) {
            std::fprintf(f, ",accel_per_hz");
        }
        std::fprintf(f, "\n");
    }

    std::mt19937 rng(7 + static_cast<unsigned>(path_letter));
    std::uniform_real_distribution<float> noise_dist(0.9f, 1.1f);

    const bool flat = std::isnan(peak_hz);
    constexpr float kPeakHeight = 3e4f;
    constexpr float kSecondaryHeight = 0.22f * kPeakHeight;
    constexpr double kBinStep = 3200.0 / 4096.0;
    // HWHM of 10 Hz (Q ~ 5 at 100 Hz), the width a real belt rig shows. A
    // spike much narrower than that tanks the curve-similarity leg of
    // compare_belt_paths() for peak pairs a real printer scores as close.
    constexpr float kMainHalfWidthSq = 100.0f;
    for (double freq = 5.0; freq <= max_freq + 1e-9; freq += kBinStep) {
        const float df = static_cast<float>(freq) - peak_hz;
        const float ds = static_cast<float>(freq) - 42.0f;
        const float main = flat ? 0.0f : kPeakHeight / (1.0f + (df * df) / kMainHalfWidthSq);
        const float secondary = flat ? 0.0f : kSecondaryHeight / (1.0f + (ds * ds) / 20.25f);
        const float psd_xyz = 150.0f * noise_dist(rng) + main + secondary;

        if (mode == BeltMockFailure::MULTICHIP) {
            std::fprintf(f, "%.1f,%.3e,%.3e\n", freq, psd_xyz, psd_xyz * 0.97f);
        } else {
            std::fprintf(f, "%.1f,%.3e,%.3e,%.3e,%.3e", freq, psd_xyz * 0.45f, psd_xyz * 0.45f,
                         psd_xyz * 0.10f, psd_xyz);
            if (mode == BeltMockFailure::KALICO) {
                std::fprintf(f, ",%.1f", 60.0);
            }
            std::fprintf(f, "\n");
        }
    }
    std::fclose(f);
    spdlog::info("[MoonrakerClientMock] Wrote mock belt CSV to {}", path);
}

} // anonymous namespace

std::string MoonrakerClientMock::shaper_csv_path(char axis_lower) {
    // getpid(), not a random suffix: the path has to be stable for the whole
    // process so a fixture's std::remove() between cases still finds the file
    // its own mock wrote, while staying disjoint from every concurrent shard.
    return "/tmp/calibration_data_" + std::string(1, axis_lower) + "_mock_" +
           std::to_string(static_cast<long>(::getpid())) + ".csv";
}

void MoonrakerClientMock::remove_shaper_csvs() {
    std::remove(shaper_csv_path('x').c_str());
    std::remove(shaper_csv_path('y').c_str());
}

json MoonrakerClientMock::resonance_tester_settings() const {
    json rt = {{"min_freq", resonance_min_freq_},
               {"max_freq", resonance_max_freq_},
               {"accel_per_hz", 75.0},
               {"hz_per_sec", resonance_hz_per_sec_},
               {"accel_chip", "adxl345"}};
    if (resonance_probe_points_) {
        // The belt settling laps run at this height.
        const auto persona = helix::mock::descriptor(printer_type_);
        rt["probe_points"] =
            json::array({json::array({(persona.axis_min.x + persona.axis_max.x) / 2.0,
                                      (persona.axis_min.y + persona.axis_max.y) / 2.0, 20.0})});
    }
    return rt;
}

float MoonrakerClientMock::belt_peak_hz(char path) const {
    return belt_peaks_hz_[(path == 'A' || path == 'a') ? 0 : 1];
}

void MoonrakerClientMock::set_belt_peaks_hz(float a_hz, float b_hz) {
    belt_peaks_hz_[0] = a_hz;
    belt_peaks_hz_[1] = b_hz;
}

void MoonrakerClientMock::set_belt_tune_script(char path_letter, std::vector<float> peaks_hz) {
    const int idx = (path_letter == 'A' || path_letter == 'a') ? 0 : 1;
    belt_script_[idx] = std::move(peaks_hz);
    belt_script_pos_[idx] = 0;
}

void MoonrakerClientMock::apply_belt_tune_script(const std::string& spec) {
    const auto colon = spec.find(':');
    const char letter =
        colon == 1 ? static_cast<char>(std::toupper(static_cast<unsigned char>(spec[0]))) : '\0';
    if (letter != 'A' && letter != 'B') {
        spdlog::warn("[MoonrakerClientMock] Belt tune script '{}' needs an A: or B: prefix", spec);
        return;
    }
    std::vector<float> peaks;
    size_t start = colon + 1;
    while (start <= spec.size()) {
        const size_t end = std::min(spec.find(',', start), spec.size());
        const std::string item = spec.substr(start, end - start);
        if (item == "flat") {
            peaks.push_back(std::numeric_limits<float>::quiet_NaN());
        } else if (auto v = text_io::parse_leading<float>(item); v && *v > 0.0f) {
            peaks.push_back(*v);
        } else {
            spdlog::warn("[MoonrakerClientMock] Skipping belt tune script item '{}'", item);
        }
        start = end + 1;
    }
    set_belt_tune_script(letter, std::move(peaks));
    spdlog::info("[MoonrakerClientMock] Belt path {} tune script: {} sweeps after the first",
                 letter, belt_script_[letter == 'A' ? 0 : 1].size());
}

std::string MoonrakerClientMock::belt_csv_path(const std::string& axis_name,
                                               const std::string& name) {
    // PID-scoped for the same reason as shaper_csv_path(): sharded test
    // processes sharing /tmp must not read or delete each other's fixtures.
    return "/tmp/resonances_" + axis_name + "_" + name + "_mock_" +
           std::to_string(static_cast<long>(::getpid())) + ".csv";
}

void MoonrakerClientMock::remove_belt_csvs() {
    // The axis/name parts come from the G-code, so match the fixed prefix and
    // this process' PID suffix rather than enumerating paths.
    const std::string prefix = "resonances_";
    const std::string suffix = "_mock_" + std::to_string(static_cast<long>(::getpid())) + ".csv";
    if (DIR* dir = opendir("/tmp")) {
        while (struct dirent* entry = readdir(dir)) {
            const std::string file(entry->d_name);
            if (file.size() > prefix.size() + suffix.size() &&
                file.compare(0, prefix.size(), prefix) == 0 &&
                file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
                std::remove(("/tmp/" + file).c_str());
            }
        }
        closedir(dir);
    }
}

json MoonrakerClientMock::build_input_shaper_config() const {
    char freq_x[16];
    char freq_y[16];
    snprintf(freq_x, sizeof(freq_x), "%.1f", shaper_freq_x_);
    snprintf(freq_y, sizeof(freq_y), "%.1f", shaper_freq_y_);
    return json{
        {"shaper_type_x", shaper_type_x_}, {"shaper_freq_x", freq_x},
        {"shaper_type_y", shaper_type_y_}, {"shaper_freq_y", freq_y},
        {"damping_ratio_x", "0.1"},        {"damping_ratio_y", "0.1"},
    };
}

uint32_t MoonrakerClientMock::sweep_line_interval_ms() const {
    if (belt_line_interval_ms_ != 0) {
        return belt_line_interval_ms_;
    }
    return static_cast<uint32_t>(std::max(1, sim_speed().shorten_wait_ms(static_cast<int>(
                                                 1000.0 / std::max(1.0, resonance_hz_per_sec_)))));
}

helix::calibration::MockSweepResponse MoonrakerClientMock::active_sweep_response() const {
    std::lock_guard<std::mutex> lock(sweep_response_mutex_);
    return sweep_response_;
}

std::unique_ptr<helix::calibration::IAccelBatchSource>
MoonrakerClientMock::make_accel_batch_source(const helix::calibration::SweepPlan& plan) const {
    return std::make_unique<helix::calibration::MockAccelBatchSource>(plan, active_sweep_response(),
                                                                      get_simulation_speedup());
}

void MoonrakerClientMock::play_console_lines(std::vector<std::string> lines, uint32_t interval_ms,
                                             helix::SharedCallback<void()> on_done) {
    std::vector<helix::SharedCallback<void()>> steps;
    steps.reserve(lines.size());
    for (auto& line : lines) {
        steps.emplace_back([this, line = std::move(line)] { dispatch_gcode_response(line); });
    }
    play_console_steps(std::move(steps), interval_ms, std::move(on_done));
}

void MoonrakerClientMock::play_console_steps(std::vector<helix::SharedCallback<void()>> steps,
                                             uint32_t interval_ms,
                                             helix::SharedCallback<void()> on_done) {
    struct PlaybackState {
        MoonrakerClientMock* mock;
        std::vector<helix::SharedCallback<void()>> steps;
        helix::SharedCallback<void()> on_done;
        size_t index;
    };
    auto* sim = new PlaybackState{this, std::move(steps), std::move(on_done), 0};
    const auto total_ticks = static_cast<int32_t>(sim->steps.size()) + 1; // + on_done

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            auto* s = static_cast<PlaybackState*>(lv_timer_get_user_data(t));

            if (s->index < s->steps.size()) {
                s->steps[s->index++]();
                return;
            }

            s->on_done();
            auto& timers = s->mock->calibration_timers_;
            timers.erase(std::remove_if(timers.begin(), timers.end(),
                                        [t](const CalibrationTimer& ct) { return ct.timer == t; }),
                         timers.end());
            delete s;
            lv_timer_delete(t);
        },
        interval_ms, sim);

    lv_timer_set_repeat_count(timer, total_ticks);
    calibration_timers_.push_back({timer, [sim] { delete sim; }});
}

void MoonrakerClientMock::dispatch_shaper_calibrate_response(char axis) {
    char axis_lower = static_cast<char>(std::tolower(static_cast<unsigned char>(axis)));

    // Build the whole console transcript up front, then play it back one line
    // per tick. Every line here is the wording Klipper and Kalico actually
    // emit — an invented marker line would let the collector's parsing pass in
    // tests while failing against real firmware.
    std::vector<std::string> lines;
    char buf[256];

    {
        // Matches write_mock_shaper_csv(): the axis's own column peaks at
        // 0.0205 with the other two at 15% and 8% of its power.
        std::lock_guard<std::mutex> lock(sweep_response_mutex_);
        const bool x = axis_lower == 'x';
        sweep_response_ =
            helix::calibration::MockSweepResponse{x ? 53.8f : 48.2f, 8.0f, 0.248f,
                                                  x ? std::array<float, 3>{1.0f, 0.387f, 0.283f}
                                                    : std::array<float, 3>{0.387f, 1.0f, 0.283f}};
    }

    // Phase 1: frequency sweep across the configured [resonance_tester] range,
    // ending exactly on max_freq the way a real sweep does. Realtime plays a
    // line per whole Hz at the belt sweep's cadence; otherwise 20 lines.
    std::vector<std::string> sweep_lines;
    constexpr int kSweepSteps = 20;
    const double min_freq = resonance_min_freq_;
    const double max_freq = resonance_max_freq_;
    if (shaper_sweep_realtime_) {
        for (double freq = std::ceil(min_freq); freq <= max_freq + 1e-9; freq += 1.0) {
            snprintf(buf, sizeof(buf), "Testing frequency %.0f Hz", freq);
            sweep_lines.emplace_back(buf);
        }
    } else {
        for (int i = 0; i < kSweepSteps; ++i) {
            const double freq =
                min_freq + (max_freq - min_freq) * i / static_cast<double>(kSweepSteps - 1);
            snprintf(buf, sizeof(buf), "Testing frequency %.0f Hz", freq);
            lines.emplace_back(buf);
        }
    }

    // Phase 2: analysis heartbeats, the sweep-finished marker, then one fit +
    // max_accel per shaper. Creality's K1C build prints "Wait for calculations.."
    // every ~5s through its (compressed here) analysis window before the
    // "Calculating the best" line; the collector treats the repeats as liveness
    // heartbeats, not new events. Ten lines give the phase ~1s of transcript on
    // its own so the panel's spinner + elapsed-seconds label has a window to
    // be observed in.
    for (int i = 0; i < 10; ++i) {
        lines.emplace_back("Wait for calculations..");
    }
    snprintf(buf, sizeof(buf), "Calculating the best input shaper parameters for %c axis",
             axis_lower);
    lines.emplace_back(buf);

    struct ShaperData {
        const char* type;
        float freq;
        float vibrations;
        float smoothing;
        int max_accel;
    };
    static const ShaperData shapers[] = {
        {"zv", 59.0f, 5.2f, 0.045f, 13400},      {"mzv", 53.8f, 1.6f, 0.130f, 4000},
        {"ei", 56.2f, 0.7f, 0.120f, 4600},       {"2hump_ei", 71.8f, 0.0f, 0.076f, 8800},
        {"3hump_ei", 89.6f, 0.0f, 0.076f, 8800},
    };
    for (const auto& sh : shapers) {
        snprintf(buf, sizeof(buf),
                 "Fitted shaper '%s' frequency = %.1f Hz (vibrations = %.1f%%, "
                 "smoothing ~= %.3f)",
                 sh.type, sh.freq, sh.vibrations, sh.smoothing);
        lines.emplace_back(buf);
        snprintf(buf, sizeof(buf),
                 "To avoid too much smoothing with '%s' (scv: 25), suggested max_accel "
                 "<= %d mm/sec^2",
                 sh.type, sh.max_accel);
        lines.emplace_back(buf);
    }

    // Phase 3: recommendation, then the CSV path that terminates the collector.
    snprintf(buf, sizeof(buf), "Recommended shaper_type_%c = mzv, shaper_freq_%c = 53.8 Hz",
             axis_lower, axis_lower);
    lines.emplace_back(buf);
    snprintf(buf, sizeof(buf), "Shaper calibration data written to %s file",
             shaper_csv_path(axis_lower).c_str());
    const std::string csv_line(buf);

    auto finish = [this, axis_lower, csv_line] {
        // Write actual CSV file so frequency response chart has data.
        // When shaper_csv_writable_ is false, simulate Klipper's /tmp
        // output being unreadable (e.g. PrivateTmp) by removing any
        // stale file at the path instead of writing it.
        const std::string csv_path = shaper_csv_path(axis_lower);
        if (shaper_csv_writable_) {
            write_mock_shaper_csv(csv_path, axis_lower);
        } else {
            std::remove(csv_path.c_str());
        }
        dispatch_gcode_response(csv_line);
        spdlog::info("[MoonrakerClientMock] Dispatched SHAPER_CALIBRATE response for axis {}",
                     static_cast<char>(std::toupper(static_cast<unsigned char>(axis_lower))));
    };
    if (sweep_lines.empty()) {
        play_console_lines(std::move(lines), 100, std::move(finish));
    } else {
        play_console_lines(std::move(sweep_lines), sweep_line_interval_ms(),
                           [this, rest = std::move(lines), finish = std::move(finish)]() mutable {
                               play_console_lines(std::move(rest), 100, std::move(finish));
                           });
    }

    spdlog::info("[MoonrakerClientMock] Started SHAPER_CALIBRATE timer for axis {} ({:.0f}-{:.0f} "
                 "Hz sweep)",
                 axis, min_freq, max_freq);
}

void MoonrakerClientMock::dispatch_test_resonances_response(const std::string& gcode) {
    // AXIS= names a belt diagonal in Klipper's XY-vector form. Path A is
    // 1,-1 and Path B is 1,1, the Voron motor names Shake&Tune uses.
    // Anything else cannot be swept.
    std::string axis_value;
    if (auto axis_pos = gcode.find("AXIS="); axis_pos != std::string::npos) {
        const size_t start = axis_pos + 5;
        const size_t end = gcode.find_first_of(" \t", start);
        axis_value = gcode.substr(start, end == std::string::npos ? end : end - start);
    }
    const bool is_a = axis_value == "1,-1";
    const bool is_b = axis_value == "1,1";
    if (!is_a && !is_b) {
        dispatch_gcode_response("!! Unsupported axis");
        spdlog::warn("[MoonrakerClientMock] TEST_RESONANCES unsupported axis '{}'", axis_value);
        return;
    }
    const int idx = is_a ? 0 : 1;
    const char path_letter = is_a ? 'A' : 'B';

    std::string name = "adxl345"; // Klipper's NAME= default
    if (auto name_pos = gcode.find("NAME="); name_pos != std::string::npos) {
        const size_t start = name_pos + 5;
        const size_t end = gcode.find_first_of(" \t", start);
        name = gcode.substr(start, end == std::string::npos ? end : end - start);
    }

    // FREQ_START=/FREQ_END= narrow the swept band; the CSV still covers the
    // configured range, as Klipper's does.
    auto window_value = [&gcode](const char* key) -> std::optional<double> {
        const std::string tag = std::string(key) + "=";
        const auto pos = gcode.find(tag);
        return pos == std::string::npos
                   ? std::nullopt
                   : text_io::parse_leading<double>(gcode.substr(pos + tag.size()));
    };
    const auto win_lo = window_value("FREQ_START");
    const auto win_hi = window_value("FREQ_END");
    const bool windowed = win_lo && win_hi && *win_hi > *win_lo;

    float sweep_peak_hz = belt_peaks_hz_[idx];
    const bool scripted = !belt_script_[0].empty() || !belt_script_[1].empty();
    if (belt_measure_count_[idx] > 0 && scripted) {
        // Every sweep after a path's first, full or windowed, plays its next
        // scripted peak, the last one holding. A script on either path turns
        // the re-measure walk off for both.
        auto& script = belt_script_[idx];
        if (!script.empty()) {
            sweep_peak_hz = script[std::min(belt_script_pos_[idx], script.size() - 1)];
            ++belt_script_pos_[idx];
            if (!std::isnan(sweep_peak_hz)) {
                belt_peaks_hz_[idx] = sweep_peak_hz;
            }
        }
    } else if (belt_measure_count_[idx] > 0 && !windowed) {
        // Each full re-measurement after the first walks this path's peak
        // toward the other's by at most 4 Hz: tightening a belt moves its
        // resonance, and the clamp keeps the walk from overshooting past equal.
        const float step = std::clamp(belt_peaks_hz_[1 - idx] - belt_peaks_hz_[idx], -4.0f, 4.0f);
        belt_peaks_hz_[idx] += step;
        sweep_peak_hz = belt_peaks_hz_[idx];
    }
    ++belt_measure_count_[idx];

    const bool kalico = belt_failure_ == BeltMockFailure::KALICO;
    const std::string axis_name =
        kalico ? (is_a ? "axis=1.000,-1.000" : "axis=1.000,1.000")
               : (is_a ? "axis=1.000,-1.000,0.000" : "axis=1.000,1.000,0.000");

    // Whole-Hz console lines across the configured [resonance_tester] range.
    std::vector<std::string> lines;
    char buf[256];
    const double sweep_lo = windowed ? *win_lo : resonance_min_freq_;
    const double sweep_hi = windowed ? *win_hi : resonance_max_freq_;
    for (double freq = std::ceil(sweep_lo); freq <= sweep_hi + 1e-9; freq += 1.0) {
        snprintf(buf, sizeof(buf), "Testing frequency %.0f Hz", freq);
        lines.emplace_back(buf);
    }
    if (belt_failure_ == BeltMockFailure::STALL) {
        lines.resize(lines.size() / 2);
    } else if (belt_failure_ == BeltMockFailure::ERROR) {
        lines.resize(3);
        lines.emplace_back("!! Invalid adxl345 id (got 0 vs e5).");
    }

    const bool has_terminal =
        belt_failure_ != BeltMockFailure::STALL && belt_failure_ != BeltMockFailure::ERROR;
    const bool write = belt_failure_ == BeltMockFailure::NONE ||
                       belt_failure_ == BeltMockFailure::MULTICHIP || kalico;
    const std::string csv_path = belt_csv_path(axis_name, name);
    std::string final_line;
    if (has_terminal) {
        snprintf(buf, sizeof(buf), "Resonances data written to %s file", csv_path.c_str());
        final_line = buf;
    }

    {
        // The CSV's peak is 3e4 with a 10 Hz half width and 0.45 / 0.45 / 0.10
        // power shares; a one-second step of amplitude A reads about A^2 / 3.
        std::lock_guard<std::mutex> lock(sweep_response_mutex_);
        sweep_response_ = helix::calibration::MockSweepResponse{
            sweep_peak_hz, 10.0f, 300.0f, {0.671f, 0.671f, 0.316f}};
    }
    const uint32_t interval = sweep_line_interval_ms();

    play_console_lines(
        std::move(lines), interval,
        [this, final_line, csv_path, path_letter, peak_hz = sweep_peak_hz,
         max_freq = resonance_max_freq_, write, mode = belt_failure_] {
            if (!final_line.empty()) {
                if (write) {
                    // HELIX_MOCK_BELT_CSV_A/_B replay a real capture for that
                    // path instead of the synthetic curve.
                    const char* replay = std::getenv(path_letter == 'A' ? "HELIX_MOCK_BELT_CSV_A"
                                                                        : "HELIX_MOCK_BELT_CSV_B");
                    std::ifstream src(replay ? replay : "");
                    if (src && mode == BeltMockFailure::NONE) {
                        std::ofstream(csv_path) << src.rdbuf();
                        spdlog::info("[MoonrakerClientMock] Replayed belt CSV {} to {}", replay,
                                     csv_path);
                    } else {
                        write_mock_belt_csv(csv_path, path_letter, peak_hz, max_freq, mode);
                    }
                } else {
                    // No file may survive at the path the terminal line names,
                    // or a caller would read stale data as this run's result.
                    std::remove(csv_path.c_str());
                }
                dispatch_gcode_response(final_line);
            }
            spdlog::info("[MoonrakerClientMock] Dispatched TEST_RESONANCES response for belt "
                         "path {}",
                         path_letter);
        });

    spdlog::info("[MoonrakerClientMock] Started TEST_RESONANCES timer for belt path {} "
                 "({:.0f}-{:.0f} Hz sweep, name '{}')",
                 path_letter, sweep_lo, sweep_hi, name);
}

void MoonrakerClientMock::dispatch_measure_axes_noise_response() {
    // Check if accelerometer is available
    if (!accelerometer_available_) {
        // Dispatch error response simulating missing accelerometer
        dispatch_gcode_response(
            "!! Unknown command:\"MEASURE_AXES_NOISE\". Check [adxl345] config.");
        spdlog::info(
            "[MoonrakerClientMock] Dispatched MEASURE_AXES_NOISE error (no accelerometer)");
        return;
    }

    // Dispatch realistic noise measurement response matching Klipper output format
    // Real Klipper format: "Axes noise for xy-axis accelerometer: 57.956 (x), 103.543 (y), 45.396
    // (z)"
    dispatch_gcode_response(
        "Axes noise for xy-axis accelerometer: 12.345678 (x), 15.678901 (y), 8.234567 (z)");

    spdlog::info("[MoonrakerClientMock] Dispatched MEASURE_AXES_NOISE response");
}

const char* MoonrakerClientMock::print_start_display_message(uint8_t simulated_phase) {
    switch (static_cast<SimulatedPrintStartPhase>(simulated_phase)) {
    case SimulatedPrintStartPhase::PRINT_START_MARKER:
    case SimulatedPrintStartPhase::HOMING:
        return "Homing...";
    case SimulatedPrintStartPhase::MOTOR_SYNC:
        return "Syncing motors...";
    case SimulatedPrintStartPhase::HEATING_BED:
        return "Heating bed...";
    case SimulatedPrintStartPhase::HEATING_NOZZLE:
        return "Heating nozzle...";
    case SimulatedPrintStartPhase::QGL:
        return "Leveling Gantry...";
    case SimulatedPrintStartPhase::BED_MESH:
        return "Calibrating bed mesh...";
    case SimulatedPrintStartPhase::PURGING:
    case SimulatedPrintStartPhase::LAYER_1:
        return "Purging nozzle";
    case SimulatedPrintStartPhase::NONE:
        break;
    }
    return "Heating...";
}

json MoonrakerClientMock::current_display_message() const {
    {
        std::lock_guard<std::mutex> lock(display_message_mutex_);
        if (display_message_set_)
            return display_message_;
    }
    const MockPrintPhase phase = print_phase_.load();
    if (phase == MockPrintPhase::PREHEAT)
        return print_start_display_message(simulated_print_start_phase_.load());
    if (phase == MockPrintPhase::PRINTING && print_progress_.load() < 0.02)
        return "Purging nozzle";
    // Most of a print, and idle, carry no message.
    return nullptr;
}

void MoonrakerClientMock::advance_print_start_simulation() {
    // Get current temperatures and targets
    double ext_temp = extruder_temp_.load();
    double ext_target = extruder_target_.load();
    double bed_temp = bed_temp_.load();
    double bed_target = bed_target_.load();

    // Get current simulated phase
    uint8_t current_phase = simulated_print_start_phase_.load();

    // Progress through phases based on temperature state
    // Each phase is dispatched once per print job

    // Phase 1: PRINT_START marker (immediately when print starts)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::PRINT_START_MARKER)) {
        dispatch_gcode_response(
            "PRINT_START BED_TEMP=" + std::to_string(static_cast<int>(bed_target)) +
            " EXTRUDER_TEMP=" + std::to_string(static_cast<int>(ext_target)));
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::PRINT_START_MARKER));
        return; // One phase per tick to spread out messages
    }

    // Phase 2: Homing (a few ticks after start)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::HOMING)) {
        dispatch_gcode_response("G28");
        dispatch_gcode_response("Homing X Y Z");
        simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::HOMING));
        return;
    }

    // Phase 3: Motor sync (motors-sync on an AWD printer), right after homing
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::MOTOR_SYNC)) {
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::MOTOR_SYNC));
        if (has_motors_sync()) {
            dispatch_gcode_response("// Motors synchronization started");
            dispatch_gcode_response("// x-Initial magnitude: 2045");
            dispatch_gcode_response("// Motors magnitudes are in tolerance: x: 100.0, y: 100.0");
            return;
        }
    }

    // Phase 4: Heating bed (when bed starts warming, ~10% toward target)
    double bed_progress =
        (bed_target > kRoomTemp) ? (bed_temp - kRoomTemp) / (bed_target - kRoomTemp) : 1.0;
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_BED) &&
        bed_progress > 0.05) {
        dispatch_gcode_response("M190 S" + std::to_string(static_cast<int>(bed_target)));
        dispatch_gcode_response("Heating bed to " + std::to_string(static_cast<int>(bed_target)) +
                                "C");
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_BED));
        return;
    }

    // Phase 5: Heating nozzle (when extruder starts warming, ~10% toward target)
    double ext_progress =
        (ext_target > kRoomTemp) ? (ext_temp - kRoomTemp) / (ext_target - kRoomTemp) : 1.0;
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_NOZZLE) &&
        ext_progress > 0.05) {
        dispatch_gcode_response("M109 S" + std::to_string(static_cast<int>(ext_target)));
        dispatch_gcode_response("Heating extruder to " +
                                std::to_string(static_cast<int>(ext_target)) + "C");
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_NOZZLE));
        return;
    }

    // Phase 6: QGL (when bed is ~50% heated - simulate while heating)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::QGL) && bed_progress > 0.4) {
        if (consume_skip(helix::skip_wrappers::Op::Qgl)) {
            dispatch_gcode_response("HelixScreen: quad gantry level skipped for this print");
        } else {
            dispatch_gcode_response("QUAD_GANTRY_LEVEL");
            dispatch_gcode_response("// Gantry leveling complete");
        }
        simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::QGL));
        return;
    }

    // Phase 7: Bed mesh (when bed is ~70% heated)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::BED_MESH) &&
        bed_progress > 0.65) {
        if (consume_skip(helix::skip_wrappers::Op::BedMesh)) {
            dispatch_gcode_response("HelixScreen: bed mesh skipped for this print");
        } else {
            dispatch_gcode_response("BED_MESH_CALIBRATE");
            dispatch_gcode_response("// Bed mesh calibration complete");
        }
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::BED_MESH));
        return;
    }

    // Phase 8: Purge line (when temps are nearly ready, ~90%)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::PURGING) &&
        bed_progress > 0.85 && ext_progress > 0.85) {
        dispatch_gcode_response("VORON_PURGE");
        dispatch_gcode_response("// Purge complete");
        simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::PURGING));
        return;
    }

    // Phase 9: Layer 1 marker (when transitioning to PRINTING phase)
    // This is handled in the simulation loop when temps are stable
}

// ============================================================================
// Restart Simulation Helper Methods
// ============================================================================

void MoonrakerClientMock::trigger_restart(bool is_firmware) {
    // Set klippy_state to "startup"
    klippy_state_.store(KlippyState::STARTUP);

    // Clear any active print state
    if (print_phase_.load() != MockPrintPhase::IDLE) {
        print_phase_.store(MockPrintPhase::IDLE);
        print_state_.store(0); // standby
        {
            std::lock_guard<std::mutex> lock(print_mutex_);
            print_filename_.clear();
        }
        print_progress_.store(0.0);
    }

    // Set temperature targets to 0 (heaters off) - temps will naturally cool
    extruder_target_.store(0.0);
    bed_target_.store(0.0);

    // Clear excluded objects list (restart clears Klipper state)
    if (mock_state_) {
        mock_state_->clear_excluded_objects();
    }
    {
        std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
        excluded_objects_.clear();
    }

    // Reset PRINT_START simulation phase
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Per-tool offsets come back from printer.cfg, so anything SET_TOOL_PARAMETER
    // wrote but SAVE_TOOL_PARAMETER + SAVE_CONFIG never committed is lost here -
    // as on a real printer. Axes with no saved entry fall back to the distinct
    // seed in tool_offset().
    {
        std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
        tool_offsets_ = tool_offsets_saved_;
    }

    // Dispatch klippy state change notification
    json status = {{"webhooks",
                    {{"state", "startup"},
                     {"state_message", is_firmware ? "Firmware restart in progress"
                                                   : "Klipper restart in progress"}}}};
    dispatch_status_update(status);

    spdlog::info("[MoonrakerClientMock] {} triggered - klippy_state='startup'",
                 is_firmware ? "FIRMWARE_RESTART" : "RESTART");

    // Schedule return to ready state using tracked thread
    // IMPORTANT: Must track and join - detached threads cause use-after-free during destruction
    double delay_sec = is_firmware ? 3.0 : 2.0;

    // Fast-forward the wait, not the simulated delay it stands for
    double effective_delay = sim_speed().shorten_wait_seconds(delay_sec);

    // Cancel and wait for any existing restart thread (under lock to prevent race with destructor)
    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        restart_pending_.store(false);
        if (restart_thread_.joinable()) {
            restart_thread_.join();
        }

        // Launch new restart thread (still under lock to prevent race on assignment)
        restart_pending_.store(true);
        restart_thread_ = helix::make_thread([this, effective_delay, is_firmware]() {
            // Sleep in small increments to allow early exit on destruction
            int total_ms = static_cast<int>(effective_delay * 1000);
            int elapsed_ms = 0;
            // Use small interval (10ms) so high-speedup tests don't overshoot
            constexpr int kSleepIntervalMs = 10;

            while (elapsed_ms < total_ms && restart_pending_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kSleepIntervalMs));
                elapsed_ms += kSleepIntervalMs;
            }

            // Check if we were cancelled
            if (!restart_pending_.load()) {
                return;
            }

            // Return to ready state
            klippy_state_.store(KlippyState::READY);

            // Dispatch ready notification
            json ready_status = {
                {"webhooks", {{"state", "ready"}, {"state_message", "Printer is ready"}}}};
            dispatch_status_update(ready_status);

            spdlog::info("[MoonrakerClientMock] {} complete - klippy_state='ready'",
                         is_firmware ? "FIRMWARE_RESTART" : "RESTART");

            restart_pending_.store(false);
        });
    } // End of restart_mutex_ lock scope
}
