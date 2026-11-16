// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_temperature_source.cpp
 * @brief The firmware-published per-filament operation temperatures.
 *
 * Run with: ./build/bin/helix-tests "[snapmaker][filament-temps]"
 *
 * The fixtures below follow the U1's table files under the config root
 * (snapmaker/filament_para_*.json): one flat JSON object per nozzle config,
 * whose keys are spelled {vendor}_{main_type}_{sub_type}_{field}. The five
 * files list overlapping subsets of the materials, so capture merges them into
 * the union. Every file also carries keys the temperature table does not own
 * (print_temp, flow_k*, vol_speed, is_soft, version, the flow ceilings,
 * process_*), which the parser must skip.
 */
#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "active_material_provider.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "filament_database.h"
#include "filament_op_slot_resolver.h"
#include "filament_temperature_source.h"
#include "http_request_epoch.h"
#include "material_settings_manager.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using filament_temps::FilamentKey;
using filament_temps::FilamentTemperatures;

namespace {

/// The 0.4 standard nozzle's table: seven materials, all vendor generic,
/// hyphenated main types (PETG-CF, PA6-CF) and one space-bearing sub type
/// (TPU 95A HF).
const char* kStandard04 = R"({"version": "1.0.0", "hard_filaments_max_flow_k": 0.4,
    "soft_filaments_max_flow_k": 0.6,
    "generic_PLA_generic_load_temp": 250, "generic_PLA_generic_unload_temp": 250,
    "generic_PETG_generic_load_temp": 270, "generic_PETG_generic_unload_temp": 270,
    "generic_ABS_generic_load_temp": 280, "generic_ABS_generic_unload_temp": 280,
    "generic_ASA_generic_load_temp": 280, "generic_ASA_generic_unload_temp": 280,
    "generic_PETG-CF_generic_load_temp": 270, "generic_PETG-CF_generic_unload_temp": 270,
    "generic_TPU_95A HF_load_temp": 250, "generic_TPU_95A HF_unload_temp": 250,
    "generic_PA6-CF_generic_load_temp": 290, "generic_PA6-CF_generic_unload_temp": 290,
    "generic_TPU_95A HF_is_soft": true})";

/// Another nozzle config's table: PLA at a different load temperature with
/// no unload field, plus a material the 0.4 table does not carry.
const char* kStandard02 = R"({"version": "1.0.0",
    "generic_PLA_generic_load_temp": 230,
    "generic_PETG-HF_generic_load_temp": 260, "generic_PETG-HF_generic_unload_temp": 260})";

/// The high-flow nozzle's table: one material only it lists.
const char* kHighFlow04 = R"({"version": "1.0.0",
    "generic_PLA_HF_load_temp": 240, "generic_PLA_HF_unload_temp": 235})";

/// Wipes both the published firmware table and any staged user override, so a
/// case never reads what a previous case stored. Same pattern as
/// test_active_material_provider.cpp's OverrideFixture.
struct FilamentTempsFixture {
    FilamentTempsFixture() {
        filament_temps::clear_filament_temperatures();
    }
    ~FilamentTempsFixture() {
        filament_temps::clear_filament_temperatures();
        auto& mgr = MaterialSettingsManager::instance();
        for (const char* name : {"PETG", "PLA"}) {
            if (mgr.has_override(name)) {
                mgr.clear_override(name);
            }
        }
    }

    void store_captured_table() const {
        auto table = filament_temps::parse_filament_temperatures(kStandard04);
        REQUIRE(!table.empty());
        filament_temps::store_filament_temperatures(table);
    }
};

SlotInfo slot(const char* brand, const char* material, const char* sub_type) {
    SlotInfo s;
    s.brand = brand;
    s.material = material;
    s.spool_name = sub_type; // carries SUB_TYPE on the firmwares in the table
    return s;
}

} // namespace

// ============================================================================
// parse_filament_temperatures()
// ============================================================================

TEST_CASE("filament-temps parse: a table file yields its leaves", "[snapmaker][filament-temps]") {
    auto table = filament_temps::parse_filament_temperatures(kStandard04);

    // Seven materials; version, the two flow ceilings and is_soft ride along
    // in the same object and must not become leaves.
    REQUIRE(table.size() == 7);
    CHECK(table.find(FilamentKey{"generic", "pla", "generic"}) != table.end());
    CHECK(table.find(FilamentKey{"generic", "petg", "generic"}) != table.end());
    CHECK(table.find(FilamentKey{"generic", "abs", "generic"}) != table.end());
    CHECK(table.find(FilamentKey{"generic", "asa", "generic"}) != table.end());
    CHECK(table.find(FilamentKey{"generic", "petg-cf", "generic"}) != table.end());
    CHECK(table.find(FilamentKey{"generic", "tpu", "95a hf"}) != table.end());
    CHECK(table.find(FilamentKey{"generic", "pa6-cf", "generic"}) != table.end());
}

TEST_CASE("filament-temps parse: hyphenated main types and spaced sub types round-trip",
          "[snapmaker][filament-temps]") {
    auto table = filament_temps::parse_filament_temperatures(kStandard04);

    auto pa6 = table.find(FilamentKey{"generic", "pa6-cf", "generic"});
    REQUIRE(pa6 != table.end());
    CHECK(pa6->second.load_c.value() == 290);
    CHECK(pa6->second.unload_c.value() == 290);

    auto tpu = table.find(FilamentKey{"generic", "tpu", "95a hf"});
    REQUIRE(tpu != table.end());
    CHECK(tpu->second.load_c.value() == 250);
    CHECK(tpu->second.unload_c.value() == 250);
}

TEST_CASE("filament-temps parse: only the two temperature suffixes are ours",
          "[snapmaker][filament-temps]") {
    auto table = filament_temps::parse_filament_temperatures(
        R"({"generic_PLA_Silk_load_temp": 240, "Snapmaker_PLA_Basic_print_temp": 225,
            "generic_PLA_Silk_vol_speed": 42, "generic_PLA_Silk_is_soft": false,
            "generic_PLA_Silk_flow_k": 0.02, "generic_PLA_load_temp": 999,
            "process_print_accel": 5000})");
    // print_temp / vol_speed / flow_k / process_* keys are skipped, and the
    // two-part stem "generic_PLA" has no sub_type to split out, so it is
    // skipped too rather than guessed into a leaf.
    REQUIRE(table.size() == 1);
    auto it = table.find(FilamentKey{"generic", "pla", "silk"});
    REQUIRE(it != table.end());
    CHECK(it->second.load_c.value() == 240);
    CHECK_FALSE(it->second.unload_c.has_value());
}

TEST_CASE("filament-temps parse: a zero temperature is an unset placeholder, not a target",
          "[snapmaker][filament-temps]") {
    // A 0 here would win over the DB rung and preheat the nozzle to 0C.
    auto table = filament_temps::parse_filament_temperatures(
        R"({"generic_PLA_Silk_load_temp": 0, "generic_PLA_Silk_unload_temp": 230,
            "generic_PETG_HF_load_temp": -5, "generic_PETG_Basic_load_temp": "250",
            "generic_PETG_Basic_unload_temp": 250.5})");
    auto silk = table.find(FilamentKey{"generic", "pla", "silk"});
    REQUIRE(silk != table.end());
    CHECK_FALSE(silk->second.load_c.has_value());
    CHECK(silk->second.unload_c.value() == 230);
    CHECK(table.find(FilamentKey{"generic", "petg", "hf"}) == table.end());
    // A string or fractional value is not the firmware's integer temperature.
    CHECK(table.find(FilamentKey{"generic", "petg", "basic"}) == table.end());
}

TEST_CASE("filament-temps parse: malformed or non-object files yield an empty map, never a throw",
          "[snapmaker][filament-temps]") {
    // Truncated mid-object, as an interrupted write would leave it.
    CHECK(filament_temps::parse_filament_temperatures(
              R"({"version": "1.0.0", "generic_PLA_generic_load_temp": 25)")
              .empty());
    // The console's Python-dict spelling is not JSON.
    CHECK(filament_temps::parse_filament_temperatures(
              "{'generic_PLA_generic_load_temp': 250, 'generic_PLA_generic_is_soft': False}")
              .empty());
    CHECK(filament_temps::parse_filament_temperatures(R"([{"generic_PLA_generic_load_temp": 250}])")
              .empty());
    CHECK(filament_temps::parse_filament_temperatures("250").empty());
    CHECK(filament_temps::parse_filament_temperatures("<html>404 Not Found</html>").empty());
    CHECK(filament_temps::parse_filament_temperatures("").empty());
}

// ============================================================================
// merge_filament_temperatures() — one dict per nozzle config, union is the table
// ============================================================================

TEST_CASE("filament-temps merge: two table files yield the union", "[snapmaker][filament-temps]") {
    auto table = filament_temps::parse_filament_temperatures(kStandard04);
    REQUIRE(table.size() == 7);

    // A second nozzle config's file: PLA again (load only, at another value)
    // plus a material the first file does not carry.
    auto line = filament_temps::parse_filament_temperatures(kStandard02);
    REQUIRE(line.size() == 2);

    filament_temps::merge_filament_temperatures(table, line);
    REQUIRE(table.size() == 8);

    auto hf = table.find(FilamentKey{"generic", "petg-hf", "generic"});
    REQUIRE(hf != table.end());
    CHECK(hf->second.load_c.value() == 260);
    CHECK(hf->second.unload_c.value() == 260);

    // A leaf both files list keeps the first file's values, and the second
    // file's missing unload_temp does not erase what the first supplied.
    auto pla = table.find(FilamentKey{"generic", "pla", "generic"});
    REQUIRE(pla != table.end());
    CHECK(pla->second.load_c.value() == 250);
    CHECK(pla->second.unload_c.value() == 250);
}

// ============================================================================
// lookup_filament_temperatures() — the firmware's four-key fallback chain
// ============================================================================

TEST_CASE("filament-temps lookup: exact vendor/type/sub resolves directly",
          "[snapmaker][filament-temps]") {
    FilamentTempsFixture fixture;
    fixture.store_captured_table();
    auto t = filament_temps::lookup_filament_temperatures(slot("Generic", "TPU", "95A HF"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 250);
    CHECK(t->unload_c.value() == 250);
}

TEST_CASE("filament-temps lookup: a sub the table lacks falls back to sub generic",
          "[snapmaker][filament-temps]") {
    // The machine's own slots spell product-line names the table has no leaf
    // for; vendor generic with sub generic still answers for a physically
    // loaded spool.
    FilamentTempsFixture fixture;
    fixture.store_captured_table();
    auto t = filament_temps::lookup_filament_temperatures(slot("Generic", "PETG", "SnapSpeed"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 270);
    CHECK(t->unload_c.value() == 270);
}

TEST_CASE("filament-temps lookup: an unknown vendor falls back to vendor generic with the sub",
          "[snapmaker][filament-temps]") {
    FilamentTempsFixture fixture;
    fixture.store_captured_table();
    // The table has no vendor "BrandX" and no tpu/generic leaf, so only the
    // (generic, sub) rung can answer this one.
    auto t = filament_temps::lookup_filament_temperatures(slot("BrandX", "TPU", "95A HF"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 250);
    CHECK(t->unload_c.value() == 250);
}

TEST_CASE("filament-temps lookup: the generic/sub rung fires before generic/generic",
          "[snapmaker][filament-temps]") {
    FilamentTempsFixture fixture;
    auto table = filament_temps::parse_filament_temperatures(
        R"({"generic_PLA_Silk_load_temp": 240, "generic_PLA_generic_load_temp": 220})");
    REQUIRE(table.size() == 2);
    filament_temps::store_filament_temperatures(table);

    // (generic, silk) answers 240; rung 4 would answer 220, so this pins the
    // firmware's order between the last two rungs.
    auto t = filament_temps::lookup_filament_temperatures(slot("BrandX", "PLA", "Silk"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 240);

    // Nothing matches the sub, so the final (generic, generic) rung answers.
    t = filament_temps::lookup_filament_temperatures(slot("BrandX", "PLA", "Junk"));
    REQUIRE(t.has_value());
    CHECK(t->load_c.value() == 220);
}

TEST_CASE("filament-temps lookup: a type the table does not carry is nullopt",
          "[snapmaker][filament-temps]") {
    FilamentTempsFixture fixture;
    fixture.store_captured_table();
    CHECK_FALSE(
        filament_temps::lookup_filament_temperatures(slot("Generic", "NITINOL", "")).has_value());
}

TEST_CASE("filament-temps lookup: no published table answers nullopt",
          "[snapmaker][filament-temps]") {
    FilamentTempsFixture fixture;
    CHECK_FALSE(
        filament_temps::lookup_filament_temperatures(slot("Generic", "PETG", "")).has_value());
}

// ============================================================================
// Capability questions over PrinterDiscovery
// ============================================================================

TEST_CASE("filament-temps capability: keyed on the filament_parameters object",
          "[snapmaker][filament-temps]") {
    PrinterDiscovery hw;
    CHECK_FALSE(filament_temps::firmware_publishes_filament_temperatures(hw));
    CHECK(filament_temps::filament_temperature_root(hw).empty());
    CHECK(filament_temps::filament_temperature_files(hw).empty());

    hw.parse_objects(nlohmann::json::array({"extruder", "filament_parameters"}));
    CHECK(filament_temps::firmware_publishes_filament_temperatures(hw));
    CHECK(filament_temps::filament_temperature_root(hw) == "config");
    CHECK(filament_temps::filament_temperature_files(hw) ==
          std::vector<std::string>{"snapmaker/filament_para_standard_04.json",
                                   "snapmaker/filament_para_standard_02.json",
                                   "snapmaker/filament_para_standard_06.json",
                                   "snapmaker/filament_para_standard_08.json",
                                   "snapmaker/filament_para_high_flow_04.json"});

    PrinterDiscovery other;
    other.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    CHECK_FALSE(filament_temps::firmware_publishes_filament_temperatures(other));
    CHECK(filament_temps::filament_temperature_files(other).empty());
}

// ============================================================================
// capture_on_connect(): the table files, read over the file API
// ============================================================================

namespace {

PrinterDiscovery u1_hardware() {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"extruder", "filament_parameters"}));
    return hw;
}

/// Records every download and serves the injected config root.
class RecordingTransfers : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;

    void download_file(const std::string& root, const std::string& path, StringCallback on_success,
                       ErrorCallback on_error) override {
        requests.push_back(root + ":" + path);
        MoonrakerFileTransferAPIMock::download_file(root, path, std::move(on_success),
                                                    std::move(on_error));
    }

    std::vector<std::string> requests;
};

struct CaptureFixture : FilamentTempsFixture {
    MoonrakerClientMock client;
    RecordingTransfers transfers{client, "http://mock:7125"};
};

} // namespace

TEST_CASE_METHOD(CaptureFixture, "filament-temps capture: every table file merges in file order",
                 "[snapmaker][filament-temps][capture]") {
    transfers.set_config_files({
        {"snapmaker/filament_para_standard_04.json", kStandard04},
        {"snapmaker/filament_para_standard_02.json", kStandard02},
        {"snapmaker/filament_para_standard_06.json", R"({"version": "1.0.0"})"},
        {"snapmaker/filament_para_standard_08.json", R"({"version": "1.0.0"})"},
        {"snapmaker/filament_para_high_flow_04.json", kHighFlow04},
    });
    filament_temps::capture_on_connect(transfers, u1_hardware());

    CHECK(transfers.requests ==
          std::vector<std::string>{"config:snapmaker/filament_para_standard_04.json",
                                   "config:snapmaker/filament_para_standard_02.json",
                                   "config:snapmaker/filament_para_standard_06.json",
                                   "config:snapmaker/filament_para_standard_08.json",
                                   "config:snapmaker/filament_para_high_flow_04.json"});

    // The 0.4 file leads: its PLA load temp wins over the 0.2 file's 230.
    auto pla = filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"));
    REQUIRE(pla.has_value());
    CHECK(pla->load_c.value() == 250);
    CHECK(pla->unload_c.value() == 250);
    // Leaves only a later file carries still publish.
    auto petg_hf = filament_temps::lookup_filament_temperatures(slot("Generic", "PETG-HF", "x"));
    REQUIRE(petg_hf.has_value());
    CHECK(petg_hf->load_c.value() == 260);
    auto pla_hf = filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "HF"));
    REQUIRE(pla_hf.has_value());
    CHECK(pla_hf->load_c.value() == 240);
    CHECK(pla_hf->unload_c.value() == 235);
}

TEST_CASE_METHOD(CaptureFixture,
                 "filament-temps capture: a missing or invalid file leaves the others published",
                 "[snapmaker][filament-temps][capture]") {
    // 0.4 is unparseable, 0.2 is absent, 0.6 is not an object: the high-flow
    // and 0.8 files still publish.
    transfers.set_config_files({
        {"snapmaker/filament_para_standard_04.json", R"({"generic_PLA_generic_load_temp": )"},
        {"snapmaker/filament_para_standard_06.json", "[]"},
        {"snapmaker/filament_para_standard_08.json", kStandard02},
        {"snapmaker/filament_para_high_flow_04.json", kHighFlow04},
    });
    filament_temps::capture_on_connect(transfers, u1_hardware());
    REQUIRE(transfers.requests.size() == 5);

    auto pla = filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"));
    REQUIRE(pla.has_value());
    CHECK(pla->load_c.value() == 230); // from the 0.8 slot's kStandard02
    CHECK_FALSE(pla->unload_c.has_value());
    auto pla_hf = filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "HF"));
    REQUIRE(pla_hf.has_value());
    CHECK(pla_hf->load_c.value() == 240);
}

TEST_CASE_METHOD(CaptureFixture,
                 "filament-temps capture: no readable file publishes nothing and keeps the DB",
                 "[snapmaker][filament-temps][capture]") {
    // A table from an earlier connection must not survive this one.
    store_captured_table();
    // A config root holding none of the table files.
    transfers.set_config_files({{"printer.cfg", "[extruder]\n"}});
    filament_temps::capture_on_connect(transfers, u1_hardware());

    REQUIRE(transfers.requests.size() == 5);
    CHECK_FALSE(filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"))
                    .has_value());
    const auto db = filament::find_material("PLA");
    REQUIRE(db.has_value());
    SlotInfo pla_lane = slot("Snapmaker", "PLA", "SnapSpeed");
    auto t = helix::ui::resolve_load_preheat_material(0, &pla_lane, nullptr);
    REQUIRE(t.has_value());
    CHECK(t->temp_c == db->nozzle_recommended());
}

TEST_CASE_METHOD(CaptureFixture,
                 "filament-temps capture: a printer without the table reads no files",
                 "[snapmaker][filament-temps][capture]") {
    store_captured_table();
    transfers.set_config_files({{"snapmaker/filament_para_standard_04.json", kStandard04}});
    PrinterDiscovery voron;
    voron.parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    filament_temps::capture_on_connect(transfers, voron);

    CHECK(transfers.requests.empty());
    // The previous printer's table is gone.
    CHECK_FALSE(filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"))
                    .has_value());
}

namespace {

/// Holds every download until the test answers it, so replies can arrive in any
/// order and after the REST epoch moves.
class HeldTransfers : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;

    struct Held {
        std::string path;
        StringCallback on_success;
        ErrorCallback on_error;
    };

    void download_file(const std::string&, const std::string& path, StringCallback on_success,
                       ErrorCallback on_error) override {
        held.push_back({path, std::move(on_success), std::move(on_error)});
    }

    /// Answer every held request in reverse order of arrival.
    void answer_in_reverse(const std::map<std::string, std::string>& files) {
        for (auto it = held.rbegin(); it != held.rend(); ++it) {
            auto file = files.find(it->path);
            if (file != files.end()) {
                it->on_success(file->second);
            } else {
                it->on_error(MoonrakerError::file_not_found("download_file", it->path));
            }
        }
        held.clear();
    }

    std::vector<Held> held;
};

struct HeldCaptureFixture : FilamentTempsFixture {
    MoonrakerClientMock client;
    HeldTransfers transfers{client, "http://mock:7125"};
    const std::map<std::string, std::string> files = {
        {"snapmaker/filament_para_standard_04.json", kStandard04},
        {"snapmaker/filament_para_standard_02.json", kStandard02},
        {"snapmaker/filament_para_high_flow_04.json", kHighFlow04},
    };
};

} // namespace

TEST_CASE_METHOD(HeldCaptureFixture,
                 "filament-temps capture: replies out of order still merge in file order",
                 "[snapmaker][filament-temps][capture]") {
    filament_temps::capture_on_connect(transfers, u1_hardware());
    REQUIRE(transfers.held.size() == 5);
    // Nothing publishes until the last reply settles.
    CHECK_FALSE(filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"))
                    .has_value());

    transfers.answer_in_reverse(files);

    // standard_04 answers last but still leads: its PLA 250 beats standard_02's 230.
    auto pla = filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"));
    REQUIRE(pla.has_value());
    CHECK(pla->load_c.value() == 250);
    auto pla_hf = filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "HF"));
    REQUIRE(pla_hf.has_value());
    CHECK(pla_hf->load_c.value() == 240);
}

TEST_CASE_METHOD(
    HeldCaptureFixture,
    "filament-temps capture: replies landing after REST moved leave the next table alone",
    "[snapmaker][filament-temps][capture]") {
    filament_temps::capture_on_connect(transfers, u1_hardware());
    REQUIRE(transfers.held.size() == 5);

    // REST moves to the next printer, whose own table is already published.
    helix::http_epoch::advance();
    store_captured_table();
    transfers.answer_in_reverse(files);

    // The late replies neither replace the next printer's table nor add to it:
    // PLA HF falls through to the next table's generic PLA, not the stale 240.
    auto pla_hf = filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "HF"));
    REQUIRE(pla_hf.has_value());
    CHECK(pla_hf->load_c.value() == 250);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "filament-temps lookup: a table from a printer REST has left answers nothing",
                 "[snapmaker][filament-temps]") {
    store_captured_table();
    REQUIRE(filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"))
                .has_value());

    // A switch to a printer whose discovery never completes leaves the table in place.
    helix::http_epoch::advance();
    CHECK_FALSE(filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"))
                    .has_value());

    // A table read under the new epoch answers again.
    store_captured_table();
    CHECK(filament_temps::lookup_filament_temperatures(slot("Generic", "PLA", "generic"))
              .has_value());
}

namespace {

/// Runs the real discovery sequence over the mock transport and records every
/// gcode script it sends.
class ScriptRecordingClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;
    using MoonrakerClientMock::send_jsonrpc;

    helix::RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 helix::SharedCallback<void(const json&)> success_cb,
                 helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms,
                 bool silent,
                 std::optional<helix::rpc_error_policy::CallerIntent> intent) override {
        if (method == "printer.gcode.script") {
            scripts.push_back(params.value("script", std::string()));
        }
        return MoonrakerClientMock::send_jsonrpc(method, params, success_cb, error_cb, timeout_ms,
                                                 silent, intent);
    }

    bool discover_real() {
        bool done = false;
        MoonrakerClient::discover_printer([&done]() { done = true; }, [](const std::string&) {});
        helix::ui::UpdateQueue::instance().drain();
        return done;
    }

    std::vector<std::string> scripts;
};

} // namespace

TEST_CASE("filament-temps capture: discovering a U1 sends nothing to its console",
          "[snapmaker][filament-temps][capture]") {
    LVGLTestFixture fixture;
    // The mock toolchanger advertises the U1's filament_parameters object.
    helix::ScopedEnv ams("HELIX_MOCK_AMS", "toolchanger");

    ScriptRecordingClient client(MoonrakerClientMock::PrinterType::VORON_24);
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    REQUIRE(client.discover_real());

    REQUIRE(filament_temps::firmware_publishes_filament_temperatures(client.hardware()));
    for (const std::string& script : client.scripts) {
        CHECK(script.find("FILAMENT_PARA") == std::string::npos);
    }
}

// ============================================================================
// Wiring into build_active_material (three-tier precedence, #961)
// ============================================================================

TEST_CASE_METHOD(FilamentTempsFixture,
                 "active material: firmware temps ride along without touching the print range",
                 "[snapmaker][filament-temps][active_material]") {
    store_captured_table();
    auto m = build_active_material(slot("Generic", "PETG", "SnapSpeed"));

    REQUIRE(m.firmware_temps.has_value());
    CHECK(m.firmware_temps->load_c.value() == 270);
    CHECK(m.firmware_temps->unload_c.value() == 270);
    // Load/unload are OPERATION temperatures; the DB's print range for PETG
    // stands untouched beside them.
    CHECK(m.material_info.nozzle_min == 230);
    CHECK(m.material_info.nozzle_max == 260);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "active material: a user override still wins with the firmware table present",
                 "[snapmaker][filament-temps][active_material]") {
    filament::MaterialOverride ovr;
    ovr.nozzle_min = 215;
    MaterialSettingsManager::instance().set_override("PETG", ovr);

    store_captured_table();
    auto m = build_active_material(slot("Generic", "PETG", "SnapSpeed"));

    CHECK(m.material_info.nozzle_min == 215); // tier 1: the user's overlay
    CHECK(m.material_info.nozzle_max == 260); // tier 3: DB (no preset, no override)
    REQUIRE(m.firmware_temps.has_value());    // tier 2 data, its own fields
    CHECK(m.firmware_temps->load_c.value() == 270);
}

TEST_CASE_METHOD(FilamentTempsFixture,
                 "active material: no table published leaves firmware_temps unset",
                 "[snapmaker][filament-temps][active_material]") {
    auto m = build_active_material(slot("Generic", "PETG", "SnapSpeed"));
    CHECK_FALSE(m.firmware_temps.has_value());
}

// ============================================================================
// The one consumer: load preheat
// ============================================================================

TEST_CASE_METHOD(FilamentTempsFixture,
                 "load preheat: the firmware's load temp beats the DB midpoint",
                 "[snapmaker][filament-temps][filament][preheat]") {
    const auto db = filament::find_material("PLA");
    REQUIRE(db.has_value());

    // Without a published table the resolver answers the DB midpoint.
    SlotInfo pla_lane = slot("Snapmaker", "PLA", "SnapSpeed");
    auto t = helix::ui::resolve_load_preheat_material(0, &pla_lane, nullptr);
    REQUIRE(t.has_value());
    CHECK(t->temp_c == db->nozzle_recommended());

    store_captured_table();
    t = helix::ui::resolve_load_preheat_material(0, &pla_lane, nullptr);
    REQUIRE(t.has_value());
    CHECK(t->temp_c == 250); // the firmware's own load temperature for this spool
}
