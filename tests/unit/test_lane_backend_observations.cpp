// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What each backend's own signal becomes in the lane source model. Cases here
// assert on the POPULATED SOURCES; a SlotInfo read appears only where one is
// needed to establish a case's precondition.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/mock_printer.h"
#include "../ui_test_utils.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "filament_slot_override.h"
#include "lane_apply.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ace_test_access.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/ams_backend_mock_timing_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/qidi_box_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_runtime_config.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/snapmaker_test_access.h"
#include "test_helpers/toolchanger_test_access.h"
#include "toolchanger_addon.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AceTestAccess;
using helix::Ad5xIfsTestAccess;
using helix::AfcTestAccess;
using helix::AmsBackendAce;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsBackendHappyHare;
using helix::AmsBackendMock;
using helix::AmsBackendMockTimingTestAccess;
using helix::AmsBackendQidi;
using helix::AmsBackendSnapmaker;
using helix::AmsBackendToolChanger;
using helix::CfsTestAccess;
using helix::HappyHareTestAccess;
using helix::QidiBoxTestAccess;
using helix::SnapmakerTestAccess;
using helix::ToolChangerTestAccess;
using helix::ams::lane_sources;
using helix::printer::AmsBackendCfs;
using helix::test::RegisteredBackend;

namespace {
/// A backend with no Moonraker behind it, registered so its lane ids are real.
using Ad5xHarness = RegisteredBackend<AmsBackendAd5xIfs>;
using AfcHarness = RegisteredBackend<AmsBackendAfc>;
using HappyHareHarness = RegisteredBackend<AmsBackendHappyHare>;
/// A CFS whose identity writes reach the box: an edit the box never receives
/// is refused, and these cases edit lanes.
class SentCfs : public AmsBackendCfs {
  public:
    using AmsBackendCfs::AmsBackendCfs;
    helix::AmsError execute_gcode(const std::string& /*gcode*/) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& /*gcode*/,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                                  bool /*silent*/) override {
        if (on_complete) {
            on_complete();
        }
        return helix::AmsErrorHelper::success();
    }
};
using CfsHarness = helix::test::RegisteredWired<SentCfs>;
using AceHarness = RegisteredBackend<AmsBackendAce>;
using SnapmakerHarness = helix::test::RegisteredWired<AmsBackendSnapmaker>;
using ToolChangerHarness = RegisteredBackend<AmsBackendToolChanger>;
/// A QIDI Box whose SAVE_VARIABLEs reach the printer, as SentCfs.
class SentQidi : public AmsBackendQidi {
  public:
    using AmsBackendQidi::AmsBackendQidi;
    helix::AmsError execute_gcode(const std::string& /*gcode*/) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& /*gcode*/,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                                  bool /*silent*/) override {
        if (on_complete) {
            on_complete();
        }
        return helix::AmsErrorHelper::success();
    }
};
using QidiHarness = helix::test::RegisteredWired<SentQidi>;
using MockHarness = RegisteredBackend<AmsBackendMock>;

/// An AFC backend running the real apply_user_edit dispatch chain, whose
/// SET_COLOR/SET_MATERIAL send answers the way a refused Moonraker call does:
/// through the error callback, after the send itself already returned
/// (prestonbrown/helixscreen#1633).
class RefusedGcodeAfc : public AmsBackendAfc {
  public:
    using AmsBackendAfc::AmsBackendAfc;

    // Per command: a case refusing only one SET_* proves that dispatch's own
    // cancel, where refusing both lets either cancel clear the shared guard.
    bool refuse_set_color = false;
    bool refuse_set_material = false;

    helix::AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                                  helix::SharedCallback<void(const MoonrakerError&)> on_error,
                                  bool /*silent*/ = true) override {
        const bool refused = (refuse_set_color && gcode.rfind("SET_COLOR ", 0) == 0) ||
                             (refuse_set_material && gcode.rfind("SET_MATERIAL ", 0) == 0);
        if (refused && on_error &&
            (gcode.rfind("SET_COLOR ", 0) == 0 || gcode.rfind("SET_MATERIAL ", 0) == 0)) {
            // Inline, on the dispatching thread, while apply_user_edit still
            // holds the backend mutex: the tightest shape the callback fires in.
            MoonrakerError err;
            err.type = MoonrakerErrorType::JSON_RPC_ERROR;
            err.message = "macro refused";
            on_error(err);
        }
        return helix::AmsErrorHelper::success();
    }
};

using RefusedGcodeAfcHarness = RegisteredBackend<RefusedGcodeAfc>;

/// One `box` object, delivered the way Moonraker delivers it. Which schema
/// parsed, and whether the frame counts as a full update at all, are decisions
/// the production entry point makes, so every CFS case drives that rather than
/// one of the two static parsers.
void feed_cfs_box(helix::test::WiredBackend<SentCfs>& backend, const nlohmann::json& box) {
    backend.fire_status({{"box", box}});
}

/// A community-fork `box` payload: a flat self-describing slots[] array.
nlohmann::json flat_box(nlohmann::json slots) {
    return nlohmann::json{{"api_version", 1}, {"slots", std::move(slots)}};
}

/// A stock Creality `box` payload carrying one unit's four bays. `filament` is
/// what marks the frame a full update on this schema.
nlohmann::json stock_box(const std::string& unit, nlohmann::json unit_json) {
    nlohmann::json box{{"filament", 0}};
    unit_json["state"] = "connect";
    box[unit] = std::move(unit_json);
    return box;
}

/// One hub status object, through the notify envelope the subscription path
/// unwraps. `slots` is a single Klipper status field, so Moonraker sends the
/// array whole: unlike AFC and Happy Hare there is no sub-field delta to model.
void feed_ace(AmsBackendAce& backend, const nlohmann::json& data) {
    nlohmann::json params;
    params["ace"] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AceTestAccess::handle_status_update(backend, notification);
}

/// One `[fila<N>]` section, the shape apply_filas_list() reads out of the
/// Box's officiall_filas_list.cfg.
std::string fila_section(int id, const std::string& name, const std::string& type) {
    return "[fila" + std::to_string(id) + "]\nfilament = " + name + "\ntype = " + type +
           "\nmin_temp = 230\nmax_temp = 250\n";
}

/// A filas list carrying one profile, one colour and one vendor row.
std::string filas_list(int fila_id, const std::string& type, int color_id,
                       const std::string& color_hex, int vendor_id, const std::string& vendor) {
    return fila_section(fila_id, type + " Basic", type) + "[colordict]\n" +
           std::to_string(color_id) + " = " + color_hex + "\n[vendor_list]\n" +
           std::to_string(vendor_id) + " = " + vendor + "\n";
}

/// Three fila profiles (two of them the same material under different names),
/// two palette rows and two vendor rows: enough table for a slot-0 edit, a
/// slot-1 control naming different rows, and a fila id change that decodes to
/// the same material.
std::string qidi_tables() {
    return fila_section(12, "PETG Basic", "PETG") + fila_section(14, "PETG Rapido", "PETG") +
           fila_section(40, "ABS Rapido", "ABS") +
           "[colordict]\n5 = #ED2C2C\n9 = #00FF00\n[vendor_list]\n3 = QIDI\n8 = Elegoo\n";
}

/// Capture the toasts a case fires, and stop capturing when it ends. The
/// insert rule's ask is a toast, so this is how a lane test observes it.
struct ToastRecorder {
    std::vector<std::string> messages;
    ToastRecorder() {
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string& msg, uint32_t) { messages.push_back(msg); });
    }
    ~ToastRecorder() {
        helix::ui::set_test_toast_hook(nullptr);
    }
};

/// Four lanes through AFC's own initialize_slots(), which is what a discovery
/// answer ends in.
void init_afc_lanes(AmsBackendAfc& backend) {
    AfcTestAccess::initialize_slots(backend,
                                    std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"});
}

/// One AFC lane_data payload, the Moonraker DB snapshot, keyed by lane name.
void feed_afc_lane_data(AmsBackendAfc& backend, const nlohmann::json& lane_data) {
    std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(backend));
    AfcTestAccess::apply_lane_data(backend, helix::afc::parse_lane_data(lane_data));
}

/// One AFC_stepper lane object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Every case here
/// drives the production path rather than the parse alone, because what makes
/// AFC different is that its frames are deltas.
void feed_afc_lane(AmsBackendAfc& backend, const std::string& lane_name,
                   const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane_name] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AfcTestAccess::handle_status_update(backend, notification);
}

/// One printer.mmu object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Happy Hare's
/// frames are deltas, so every case here drives the production entry point
/// rather than the parse alone.
void feed_mmu(AmsBackendHappyHare& backend, const nlohmann::json& mmu) {
    nlohmann::json params;
    params["mmu"] = mmu;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    HappyHareTestAccess::handle_status_update(backend, notification);
}

/// One filament_detect object plus optional filament_feed channels, through
/// the wire. RFID identity arrives
/// under the filament_detect key; lane presence arrives from the feed's port
/// sensor and channel_state latch — the state array is the entrance/tag
/// reading and backs no presence claim.
void feed_filament_detect(helix::test::WiredBackend<AmsBackendSnapmaker>& backend,
                          const nlohmann::json& fd,
                          const nlohmann::json& feed = nlohmann::json::object()) {
    nlohmann::json status;
    status["filament_detect"] = fd;
    if (!feed.empty())
        status["filament_feed left"] = feed;
    backend.fire_status(status);
}

/// One print_task_config object, through the same wire. This is Snapmaker's
/// second identity writer and the one that files no observation, so a case
/// needs to drive it separately from filament_detect.
/// The backend's own print_stats reads standby: no print holds the heads.
void snapmaker_idle(helix::test::WiredBackend<AmsBackendSnapmaker>& backend) {
    backend.fire_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
}

void feed_print_task_config(helix::test::WiredBackend<AmsBackendSnapmaker>& backend,
                            const nlohmann::json& ptc) {
    backend.fire_status(nlohmann::json{{"print_task_config", ptc}});
}

/// One tool-changer status object, delivered the way Moonraker delivers it.
void feed_toolchanger(AmsBackendToolChanger& backend, const nlohmann::json& status) {
    nlohmann::json notification;
    notification["method"] = "notify_status_update";
    notification["params"] = nlohmann::json::array({status, 0.0});
    ToolChangerTestAccess::handle_status(backend, notification);
}

/// A stock upstream MedusaHC object list, which is what gives a tool changer
/// dock sensors at all. Without them every toolhead is assumed present and
/// EMPTY is not an answer the backend can reach.
helix::PrinterDiscovery medusahc_discovery() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2",
                                            "pin_watch io", "servo my_servo", "extruder"}));
    return hw;
}
} // namespace

/// The presence vocabulary every producer in this file consumes, pinned over
/// the whole enum. A producer's own case can only reach the statuses its
/// firmware emits, so no backend test covers the rule end to end.
TEST_CASE("the presence vocabulary answers for every slot status", "[lane][ingest]") {
    using helix::slot_status_reports_filament;
    using helix::SlotStatus;

    auto stated = [](SlotStatus status) { return slot_status_reports_filament(status); };

    const auto empty = stated(SlotStatus::EMPTY);
    REQUIRE(empty.has_value());
    CHECK(*empty == false);

    for (SlotStatus occupied :
         {SlotStatus::AVAILABLE, SlotStatus::LOADED, SlotStatus::FROM_BUFFER}) {
        INFO("status " << helix::slot_status_to_string(occupied));
        const auto reading = stated(occupied);
        REQUIRE(reading.has_value());
        CHECK(*reading == true);
    }

    // A jam is filament stuck in the path, so a blocked bay holds filament.
    // QIDI reports it for any negative state word, and both SlotInfo::
    // is_present() and helix::ui::classify_lane() count it present; answering
    // "no reading" here would retract a live reading the moment a lane jams.
    const auto blocked = stated(SlotStatus::BLOCKED);
    REQUIRE(blocked.has_value());
    CHECK(*blocked == true);

    // The one status where this rule and is_present() differ, and the reason
    // the answer is three-valued at all.
    CHECK_FALSE(stated(SlotStatus::UNKNOWN).has_value());

    // The contract between the two rules, asserted rather than described: they
    // agree on every status but UNKNOWN. Either one drifting fails here.
    for (SlotStatus status : {SlotStatus::EMPTY, SlotStatus::AVAILABLE, SlotStatus::LOADED,
                              SlotStatus::FROM_BUFFER, SlotStatus::BLOCKED}) {
        INFO("status " << helix::slot_status_to_string(status));
        helix::SlotInfo slot;
        slot.status = status;
        const auto reading = stated(status);
        REQUIRE(reading.has_value());
        CHECK(*reading == slot.is_present());
    }

    helix::SlotInfo unknown_slot;
    unknown_slot.status = SlotStatus::UNKNOWN;
    CHECK_FALSE(unknown_slot.is_present());
}

TEST_CASE_METHOD(LVGLTestFixture, "a registered backend's slots are its own block of lanes",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);

    CHECK(harness.lane(0) == helix::ams::lane_id_for(0, 0));
    CHECK(harness.lane(3) == helix::ams::lane_id_for(0, 3));
    CHECK(helix::ams::is_lane_id(harness.lane(0)));
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X ingests silk-sensor presence as sensed",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");
    Ad5xIfsTestAccess::set_material(*harness, 0, "PETG");
    Ad5xIfsTestAccess::set_port_presence(*harness, 0, false);

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == false);
    // The vendor file still remembers the last spool. That is a cache of a past
    // declaration, so it must not carry presence or identity into the sensor's
    // record.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    REQUIRE(lane.firmware->material.has_value());
    CHECK(*lane.firmware->material == "PETG");
    CHECK_FALSE(lane.firmware->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X files a sensed record with no reading before Ports is seen",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);

    // Native ZMOD publishes no per-port sensors, so port_presence_ is false for
    // every lane whether or not filament is there. An unread sensor is not a
    // reading of "empty".
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto unread = lane_sources(harness.lane(1));
    // The record must exist: its absence would mean the translation never ran,
    // which is a different fact from "the sensor has said nothing yet" and must
    // not pass for it.
    REQUIRE(unread.sensed.has_value());
    CHECK_FALSE(unread.sensed->present.has_value());

    // The same lane, once the silk mask has been read. This half is what makes
    // the half above a guard rather than a backend that never reports presence.
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto read = lane_sources(harness.lane(1));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X does not cache a colour it has not read",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 2, "");
    Ad5xIfsTestAccess::set_material(*harness, 2, "");
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    const auto lane = lane_sources(harness.lane(2));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.firmware.has_value());
    CHECK_FALSE(lane.firmware->color_rgb.has_value());
    CHECK_FALSE(lane.firmware->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X caches pure black as a colour", "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 3, true);
    Ad5xIfsTestAccess::set_color(*harness, 3, "000000");

    const auto lane = lane_sources(harness.lane(3));

    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X reads its stored colour with the shared lane grammar",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 0, true);
    Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");

    const auto parsed = lane_sources(harness.lane(0));
    REQUIRE(parsed.firmware.has_value());
    REQUIRE(parsed.firmware->color_rgb == 0xED2C2Cu);

    // parse_adventurer_json stores ffmColorN as the printer sent it, minus a
    // leading '#', so colors_[] holds whatever spelling the source used. Each
    // of these is a value a digit-by-digit hex conversion answers differently
    // from the grammar the other eight backends read a lane colour with, and
    // the readable ones come first so an unreadable spelling has a colour on
    // the lane it could wrongly stand in for.
    struct Spelling {
        const char* stored;
        std::optional<uint32_t> expected;
    };
    const std::vector<Spelling> spellings = {
        // The '#RGB' short form the stock UI writes, expanded rather than read
        // as three leading zeroes.
        {"ABC", 0xAABBCCu},
        // '#RRGGBBAA': the alpha is shifted off, not left in the low byte.
        {"ED2C2CAA", 0xED2C2Cu},
        // Trailing junk voids the value instead of truncating to the hex
        // digits in front of it.
        {"ED2C2Cjunk", std::nullopt},
        // Neither a sign nor a seven-digit run is a colour.
        {"-1", std::nullopt},
        {"1FFAA00", std::nullopt},
    };

    for (const auto& spelling : spellings) {
        CAPTURE(spelling.stored);
        Ad5xIfsTestAccess::set_color(*harness, 0, spelling.stored);

        const auto lane = lane_sources(harness.lane(0));
        REQUIRE(lane.firmware.has_value());
        CHECK(lane.firmware->color_rgb == spelling.expected);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AD5X's vendor-cache record",
                 "[lane][ingest][ad5x]") {
    struct RemoveOnExit {
        std::filesystem::path path;
        ~RemoveOnExit() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } adventurer_json{std::filesystem::temp_directory_path() /
                      ("helix_lane_obs_ad5x_" + std::to_string(::getpid()) + ".json")};
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(*harness, adventurer_json.path.string());

    // A user colour that disagrees with what the vendor file goes on to say.
    // SlotInfo is persistent across frames and the lane re-layer rewrites it in
    // place, so entry->info.color_rgb is this value by the time the frame ends.
    // A translation reading it back would file the user's own choice as
    // something the vendor store remembers.
    helix::SlotInfo edit = harness->get_slot_info(1);
    edit.color_rgb = 0x00FF00u;
    helix::test::edit_slot_as_user(*harness, 1, edit);

    Ad5xIfsTestAccess::set_color(*harness, 1, "ED2C2C");
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, true);

    // Precondition, not the behaviour under test: unless the user's colour
    // actually wins on the merged slot, there is no laundering for the case to
    // catch and the assertion below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);

    // A lane the vendor file says nothing about, where our own stored record is
    // the only thing carrying a colour at all. entry->info.color_rgb holds that
    // colour alone, so a read-back files one where the vendor store has none.
    helix::ams::FilamentSlotOverride stored;
    stored.color_rgb = 0x00FF00u;
    stored.color_set = true;
    Ad5xIfsTestAccess::seed_override(*harness, 2, stored);
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    REQUIRE(harness->get_slot_info(2).color_rgb == 0x00FF00u);

    const auto blank = lane_sources(harness.lane(2));
    REQUIRE(blank.firmware.has_value());
    CHECK_FALSE(blank.firmware->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC splits its sensors, its own store and its own weight",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"load", true},
                   {"tool_loaded", false},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"},
                   {"spool_vendor", "Kingroon"},
                   {"weight", 612.0},
                   {"spool_id", 7},
                   {"initial_weight", 1000.0}});

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);
    // prep and load are real sensors and say nothing about which spool this is.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    // AFC's own store keeps colour, material, filament name and vendor across
    // an eject, so all four are a cache of a past declaration.
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->material == "PETG");
    CHECK(lane.firmware->spool_name == "Galaxy Black");
    CHECK(lane.firmware->brand == "Kingroon");
    CHECK(lane.firmware->spoolman_id == 7);
    CHECK_FALSE(lane.firmware->present.has_value());

    REQUIRE(lane.metered.has_value());
    REQUIRE(lane.metered->remaining_weight_g.has_value());
    CHECK(*lane.metered->remaining_weight_g == Catch::Approx(612.0F));
    REQUIRE(lane.metered->total_weight_g.has_value());
    CHECK(*lane.metered->total_weight_g == Catch::Approx(1000.0F));
    // A weight tick has no business asserting identity or presence.
    CHECK_FALSE(lane.metered->material.has_value());
    CHECK_FALSE(lane.metered->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial AFC delta narrows nothing it does not mention",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // The commonest frame AFC sends: the lane reached the toolhead and nothing
    // about the spool changed. A record assembled from this frame alone would
    // leave the lane with no identity at all.
    feed_afc_lane(*harness, "lane2", {{"status", "Tooled"}});

    const auto after_status = lane_sources(harness.lane(1));
    REQUIRE(after_status.firmware.has_value());
    REQUIRE(after_status.firmware->color_rgb.has_value());
    CHECK(*after_status.firmware->color_rgb == 0xED2C2Cu);
    CHECK(after_status.firmware->material == "PETG");
    CHECK(after_status.firmware->spool_name == "Galaxy Black");
    REQUIRE(after_status.sensed.has_value());
    CHECK(after_status.sensed->present == true);

    // A colour change alone must not take the material with it.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}});

    const auto after_color = lane_sources(harness.lane(1));
    REQUIRE(after_color.firmware.has_value());
    REQUIRE(after_color.firmware->color_rgb.has_value());
    CHECK(*after_color.firmware->color_rgb == 0x00AEFFu);
    CHECK(after_color.firmware->material == "PETG");
    CHECK(after_color.firmware->spool_name == "Galaxy Black");
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC files no weight reading until a frame carries one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane4", {{"prep", true}, {"status", "Loaded"}});

    const auto lane = lane_sources(harness.lane(3));
    // The record is filed whether or not AFC has weighed anything, so the
    // absence below is a reading AFC has not made rather than a translation
    // that never ran.
    REQUIRE(lane.metered.has_value());
    CHECK_FALSE(lane.metered->remaining_weight_g.has_value());
    CHECK_FALSE(lane.metered->total_weight_g.has_value());

    feed_afc_lane(*harness, "lane4", {{"weight", 612.0}});

    const auto weighed = lane_sources(harness.lane(3));
    REQUIRE(weighed.metered.has_value());
    REQUIRE(weighed.metered->remaining_weight_g.has_value());
    CHECK(*weighed.metered->remaining_weight_g == Catch::Approx(612.0F));
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's eject clears its own store without clearing the sensor",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane3",
                  {{"prep", true},
                   {"load", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});
    REQUIRE(lane_sources(harness.lane(2)).firmware->material == "PETG");

    // clear_values() empties colour, material and filament name on eject. An
    // empty value is a clear, so the cache must stop reporting them rather
    // than standing on the last frame that named them.
    feed_afc_lane(*harness, "lane3",
                  {{"prep", false},
                   {"load", false},
                   {"status", "None"},
                   {"color", ""},
                   {"material", ""},
                   {"filament_name", ""}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == false);
    REQUIRE(lane.firmware.has_value());
    CHECK_FALSE(lane.firmware->color_rgb.has_value());
    CHECK_FALSE(lane.firmware->material.has_value());
    CHECK_FALSE(lane.firmware->spool_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AFC's vendor-cache record",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // A user colour and material that disagree with what AFC reports. The edit
    // is re-laid onto SlotInfo at the tail of every frame, so a translation
    // reading that struct back would file the user's own choice as something
    // AFC's store remembers.
    helix::SlotInfo edit = harness->get_slot_info(1);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 1, edit);

    feed_afc_lane(
        *harness, "lane2",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PETG"}});

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch
    // and both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Clear Spool on a previously edited lane shows nothing afterwards",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});

    // Clear Spool empties the lane in firmware field by field, so the next
    // status frame publishes the emptied keys with WEIGHT=0. Nothing of the
    // edit's identity may survive that frame on screen.
    harness->clear_slot_override(0);
    feed_afc_lane(*harness, "lane1",
                  {{"status", "Loaded"}, {"color", ""}, {"material", ""}, {"weight", 0.0}});

    const auto info = harness->get_slot_info(0);
    CHECK(info.material.empty());
    CHECK(info.color_rgb == helix::kAmsDefaultSlotColor);
    CHECK(info.remaining_weight_g == -1.0F);
    CHECK(info.total_weight_g == -1.0F);
    CHECK(info.brand.empty());

    const auto after = lane_sources(harness.lane(0));
    REQUIRE(after.firmware.has_value());
    CHECK_FALSE(after.firmware->color_rgb.has_value());
    CHECK_FALSE(after.firmware->material.has_value());
}

namespace {} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a SET_* Moonraker refuses withdraws the edit's pending record",
                 "[lane][pending][afc]") {
    RefusedGcodeAfcHarness harness(nullptr, nullptr);
    SECTION("a refused SET_COLOR") {
        harness->refuse_set_color = true;
    }
    SECTION("a refused SET_MATERIAL") {
        harness->refuse_set_material = true;
    }
    init_afc_lanes(*harness);
    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);
    REQUIRE(lane_sources(harness.lane(0)).pending.has_value());

    // The refusal arrives on the error callback and hops to the main thread.
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lane_sources(harness.lane(0)).pending.has_value());
    CHECK(harness->get_slot_info(0).material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a frame with no sensor key neither sets nor erases AFC presence",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // Weight alone. AFC's sensors have said nothing about this lane, and no
    // sensor reading is not a sensor reading of "empty".
    feed_afc_lane(*harness, "lane1", {{"weight", 612.0}});

    const auto unread = lane_sources(harness.lane(0));
    // Proof the translation ran, so the absence below is about the sensors.
    REQUIRE(unread.metered.has_value());
    CHECK_FALSE(unread.sensed.has_value());

    // The same lane once the sensors do speak. This half is what makes the
    // half above a guard rather than a backend that never reports presence.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // Another weight-only frame. AFC's presence authority is per frame rather
    // than a latch, and a record is written whole, so filing an empty one here
    // would erase a live reading.
    feed_afc_lane(*harness, "lane1", {{"weight", 600.0}});

    const auto still = lane_sources(harness.lane(0));
    REQUIRE(still.sensed.has_value());
    REQUIRE(still.sensed->present.has_value());
    CHECK(*still.sensed->present == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "each AFC lane accumulates its own identity",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // A second lane, saying nothing about any spool. One accumulator for the
    // backend instead of one per lane would hand it lane1's identity.
    feed_afc_lane(*harness, "lane2", {{"prep", false}, {"status", "None"}});

    const auto second = lane_sources(harness.lane(1));
    REQUIRE(second.firmware.has_value());
    CHECK_FALSE(second.firmware->color_rgb.has_value());
    CHECK_FALSE(second.firmware->material.has_value());
    CHECK_FALSE(second.firmware->spool_name.has_value());

    const auto first = lane_sources(harness.lane(0));
    REQUIRE(first.firmware.has_value());
    REQUIRE(first.firmware->color_rgb.has_value());
    CHECK(*first.firmware->color_rgb == 0xED2C2Cu);
    CHECK(first.firmware->material == "PETG");

    // The second lane's own spool, which must reach its own record and leave
    // the first lane's alone.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}, {"material", "PLA"}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.firmware.has_value());
    REQUIRE(changed.firmware->color_rgb.has_value());
    CHECK(*changed.firmware->color_rgb == 0x00AEFFu);
    CHECK(changed.firmware->material == "PLA");

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.firmware.has_value());
    REQUIRE(untouched.firmware->color_rgb.has_value());
    CHECK(*untouched.firmware->color_rgb == 0xED2C2Cu);
    CHECK(untouched.firmware->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a spool id AFC did not state is not filed as one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // The user binds a spool to the lane.
    helix::SlotInfo edit = harness->get_slot_info(0);
    edit.spoolman_id = 99;
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // AFC unlinks the lane; the user's own binding still stands on the merged
    // slot, which is the precondition rather than the behaviour.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", nullptr}});
    REQUIRE(harness->get_slot_info(0).spoolman_id == 99);
    REQUIRE_FALSE(lane_sources(harness.lane(0)).firmware->spoolman_id.has_value());

    // A spool_id that is neither an integer nor null states nothing. Reading
    // the merged slot here would file the user's 99 as AFC's own word.
    feed_afc_lane(*harness, "lane1", {{"spool_id", "junk"}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.firmware.has_value());
    CHECK_FALSE(lane.firmware->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's DB snapshot files on the same lane as its status frames",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // lane_data alone. No status frame has run, so every value below is this
    // parser's work and nothing else's.
    feed_afc_lane_data(*harness, {{"lane3",
                                   {{"color", "#ED2C2C"},
                                    {"material", "PETG"},
                                    {"name", "Galaxy Black"},
                                    {"vendor_name", "Kingroon"},
                                    {"spool_id", 7}}}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->material == "PETG");
    CHECK(lane.firmware->spool_name == "Galaxy Black");
    CHECK(lane.firmware->brand == "Kingroon");
    CHECK(lane.firmware->spoolman_id == 7);

    // A database record is not a sensor and does not weigh anything.
    CHECK_FALSE(lane.sensed.has_value());
    CHECK_FALSE(lane.metered.has_value());

    // A value the snapshot carries but we cannot read is not the snapshot
    // saying the lane has no colour, so the record keeps the last one it could
    // read. Only an empty value removes it.
    feed_afc_lane_data(*harness, {{"lane3", {{"color", "#zzzzzz"}}}});

    const auto unreadable = lane_sources(harness.lane(2));
    REQUIRE(unreadable.firmware.has_value());
    REQUIRE(unreadable.firmware->color_rgb.has_value());
    CHECK(*unreadable.firmware->color_rgb == 0xED2C2Cu);

    feed_afc_lane_data(*harness, {{"lane3", {{"color", ""}}}});
    CHECK_FALSE(lane_sources(harness.lane(2)).firmware->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's two parsers accumulate into one account of a lane",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // The subscription carries colour, material and weight on every AFC
    // version; the filament name reaches pre-v1.2.0 firmware only through the
    // DB snapshot. Neither parser may narrow the lane to its own half.
    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"weight", 612.0}});
    feed_afc_lane_data(*harness, {{"lane1", {{"name", "Galaxy Black"}}}});

    const auto joined = lane_sources(harness.lane(0));
    REQUIRE(joined.firmware.has_value());
    REQUIRE(joined.firmware->color_rgb.has_value());
    CHECK(*joined.firmware->color_rgb == 0xED2C2Cu);
    CHECK(joined.firmware->material == "PETG");
    CHECK(joined.firmware->spool_name == "Galaxy Black");

    // The snapshot said nothing about sensors or weight, so what the
    // subscription established still stands.
    REQUIRE(joined.sensed.has_value());
    CHECK(joined.sensed->present == true);
    REQUIRE(joined.metered.has_value());
    CHECK(*joined.metered->remaining_weight_g == Catch::Approx(612.0F));

    // An empty name in the snapshot is the clear AFC writes on eject, the same
    // answer the status path gives an empty filament_name.
    feed_afc_lane_data(*harness, {{"lane1", {{"name", ""}}}});

    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.firmware.has_value());
    CHECK_FALSE(cleared.firmware->spool_name.has_value());
    CHECK(cleared.firmware->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable colour leaves AFC's record standing",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2", {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}});
    REQUIRE(*lane_sources(harness.lane(1)).firmware->color_rgb == 0xED2C2Cu);

    // "AFC published something we cannot read" and "AFC cleared the lane" are
    // different statements, and the record has to tell them apart or the lane
    // loses an identity nobody withdrew. The slot keeps its colour on screen;
    // the record keeps the same one.
    feed_afc_lane(*harness, "lane2", {{"color", "#zzzzzz"}});

    const auto unreadable = lane_sources(harness.lane(1));
    REQUIRE(unreadable.firmware.has_value());
    REQUIRE(unreadable.firmware->color_rgb.has_value());
    CHECK(*unreadable.firmware->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(1).color_rgb == 0xED2C2Cu);

    // The clear is the other statement, and it does remove the colour.
    feed_afc_lane(*harness, "lane2", {{"color", ""}});

    const auto cleared = lane_sources(harness.lane(1));
    REQUIRE(cleared.firmware.has_value());
    CHECK_FALSE(cleared.firmware->color_rgb.has_value());
    CHECK(harness->get_slot_info(1).color_rgb == helix::kAmsDefaultSlotColor);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare splits gate status from gate metadata",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "000000", "", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", "", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0, 0})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed.has_value());
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == true);
    // The sensor's record carries presence and nothing else: what the gate map
    // remembers is a cache of a past declaration, not something a gate sensed.
    CHECK_FALSE(gate0.sensed->color_rgb.has_value());
    CHECK_FALSE(gate0.sensed->material.has_value());

    REQUIRE(gate0.firmware.has_value());
    REQUIRE(gate0.firmware->color_rgb.has_value());
    CHECK(*gate0.firmware->color_rgb == 0xED2C2Cu);
    CHECK(gate0.firmware->material == "PETG");
    CHECK(gate0.firmware->spoolman_id == 7);
    CHECK_FALSE(gate0.firmware->present.has_value());
    // The gate map carries no weight at all.
    CHECK_FALSE(gate0.metered.has_value());

    const auto gate1 = lane_sources(harness.lane(1));
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == false);
    // An empty gate still remembers what it last held. That memory is a cache,
    // and pure black in it is a colour like any other.
    REQUIRE(gate1.firmware.has_value());
    REQUIRE(gate1.firmware->color_rgb.has_value());
    CHECK(*gate1.firmware->color_rgb == 0x000000u);
    // Happy Hare writes 0 for a gate with no spool, which is a clear rather
    // than a spool numbered zero.
    CHECK_FALSE(gate1.firmware->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare records no colour for a gate that reports none",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", ""})}});

    const auto gate1 = lane_sources(harness.lane(1));
    // Proof the path ran: presence came through for the same gate.
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == true);
    REQUIRE(gate1.firmware.has_value());
    CHECK_FALSE(gate1.firmware->color_rgb.has_value());
    CHECK_FALSE(gate1.firmware->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare frame without metadata does not blank the cache",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})}});
    REQUIRE(lane_sources(harness.lane(0)).firmware->material == "PETG");

    // A status-only delta states nothing about metadata, so it must not be
    // read as a gate that stopped reporting one.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0, 1})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == false);
    REQUIRE(gate0.firmware.has_value());
    CHECK(gate0.firmware->material == "PETG");
    REQUIRE(gate0.firmware->color_rgb.has_value());
    CHECK(*gate0.firmware->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial Happy Hare delta narrows nothing it does not mention",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 9})}});

    // MMU_GATE_MAP GATE=0 MATERIAL=PLA moves one array. Moonraker names only
    // what changed, so a record built from this frame alone would be a gate
    // whose colour and spool link had just vanished.
    feed_mmu(*harness, {{"gate_material", nlohmann::json::array({"PLA", "ABS"})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.firmware.has_value());
    CHECK(gate0.firmware->material == "PLA");
    REQUIRE(gate0.firmware->color_rgb.has_value());
    CHECK(*gate0.firmware->color_rgb == 0xED2C2Cu);
    CHECK(gate0.firmware->spoolman_id == 7);
}

TEST_CASE_METHOD(LVGLTestFixture, "each Happy Hare gate accumulates its own identity",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // Gate 0 alone carries a spool. One accumulator for the backend instead of
    // one per gate would hand its identity to every other gate.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 0})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0})}});

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.firmware.has_value());
    CHECK_FALSE(silent.firmware->color_rgb.has_value());
    CHECK_FALSE(silent.firmware->material.has_value());
    CHECK_FALSE(silent.firmware->spoolman_id.has_value());

    // Gate 1 gets its own spool; gate 0's record must not follow it.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"ed2c2c", "00aeff", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 12, 0})}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.firmware.has_value());
    REQUIRE(changed.firmware->color_rgb.has_value());
    CHECK(*changed.firmware->color_rgb == 0x00AEFFu);
    CHECK(changed.firmware->material == "PLA");
    CHECK(changed.firmware->spoolman_id == 12);

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.firmware.has_value());
    REQUIRE(untouched.firmware->color_rgb.has_value());
    CHECK(*untouched.firmware->color_rgb == 0xED2C2Cu);
    CHECK(untouched.firmware->material == "PETG");
    CHECK(untouched.firmware->spoolman_id == 7);

    const auto quiet = lane_sources(harness.lane(2));
    REQUIRE(quiet.firmware.has_value());
    CHECK_FALSE(quiet.firmware->color_rgb.has_value());
    CHECK_FALSE(quiet.firmware->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches Happy Hare's vendor-cache record",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // gate_spool_id is what makes the lane re-layer run at all on this path.
    const nlohmann::json gates = {{"gate_status", nlohmann::json::array({1, 1})},
                                  {"gate_color", nlohmann::json::array({"a4b2bc", "ed2c2c"})},
                                  {"gate_material", nlohmann::json::array({"PLA", "PETG"})},
                                  {"gate_spool_id", nlohmann::json::array({0, 0})}};

    // Frame one gives the backend its gates, which an edit needs to address.
    feed_mmu(*harness, gates);

    // A user colour and material that disagree with what the gate map says.
    // SlotInfo persists across frames and the lane re-layer rewrites it in
    // place, so a translation reading that struct back would file the user's
    // own choice as something Happy Hare's gate map remembers.
    helix::SlotInfo edit = harness->get_slot_info(1);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 1, edit);

    // Frame two re-states the same gate map, with the user's choice standing on
    // the merged slot for a read-back to pick up.
    feed_mmu(*harness, gates);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for the case to catch and
    // the assertions below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(1).material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a gate Happy Hare calls unknown files no presence reading",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // Nothing on this lane may abort the case: the decisive claim is the last
    // phase, and a leading REQUIRE would report "no record filed" for a rule
    // whose actual failure is a stale reading surviving.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});

    const auto unread = lane_sources(harness.lane(0));
    // The record is filed even with nothing to say, so "the MMU does not know"
    // and "no translation ran" stay distinguishable.
    CHECK(unread.sensed.has_value());
    const bool unknown_gate_claims_a_reading =
        unread.sensed.has_value() && unread.sensed->present.has_value();
    CHECK_FALSE(unknown_gate_claims_a_reading);

    const auto known = lane_sources(harness.lane(1));
    REQUIRE(known.sensed.has_value());
    REQUIRE(known.sensed->present.has_value());
    CHECK(*known.sensed->present == true);

    // Positive contrast: the same gate reports a real status and the reading
    // lands, so the arm above is a guard and not a gate that never reports.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})}});
    const auto reporting = lane_sources(harness.lane(0));
    REQUIRE(reporting.sensed.has_value());
    REQUIRE(reporting.sensed->present.has_value());
    CHECK(*reporting.sensed->present == true);

    // Happy Hare withdrawing its word is news. One ingest replaces a source's
    // record whole, so filing nothing leaves the old reading standing: a
    // record whose present is unset is the only way to retract one.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});
    const auto withdrawn = lane_sources(harness.lane(0));
    const bool a_stale_reading_still_stands =
        withdrawn.sensed.has_value() && withdrawn.sensed->present.has_value();
    CHECK_FALSE(a_stale_reading_still_stands);
    CHECK(withdrawn.sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable Happy Hare colour leaves the record standing",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})}});
    REQUIRE(*lane_sources(harness.lane(0)).firmware->color_rgb == 0xED2C2Cu);

    // A value nothing can read is not a gate stating it has no colour, so the
    // last readable word stands in both the record and the slot.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"zzzzzz", "a4b2bc"})}});
    const auto unreadable = lane_sources(harness.lane(0));
    REQUIRE(unreadable.firmware.has_value());
    REQUIRE(unreadable.firmware->color_rgb.has_value());
    CHECK(*unreadable.firmware->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // Empty is Happy Hare wiping the gate, which is a statement and clears both.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"", "a4b2bc"})}});
    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.firmware.has_value());
    CHECK_FALSE(cleared.firmware->color_rgb.has_value());
    CHECK(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare files the colour its numeric gate map states",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // gate_color_rgb wins over the hex strings, in both the shapes Happy Hare
    // and its emulator publish: packed integers and float triplets.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color_rgb", nlohmann::json::array({0xED2C2C, {0.0, 1.0, 0.0}})},
                        {"gate_color", nlohmann::json::array({"a4b2bc", "a4b2bc"})}});

    const auto packed = lane_sources(harness.lane(0));
    REQUIRE(packed.firmware.has_value());
    REQUIRE(packed.firmware->color_rgb.has_value());
    CHECK(*packed.firmware->color_rgb == 0xED2C2Cu);

    const auto triplet = lane_sources(harness.lane(1));
    REQUIRE(triplet.firmware.has_value());
    REQUIRE(triplet.firmware->color_rgb.has_value());
    CHECK(*triplet.firmware->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare files no reading for a gate it has no slot for",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // The gate count is fixed by the first gate_status frame, so a longer array
    // later names gates the backend never built a slot for.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})}});
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1, 1, 1})}});

    // Precondition, not the behaviour: without this the assertions below would
    // hold on a backend that had quietly grown the slots after all.
    REQUIRE(harness->get_slot_info(2).slot_index == -1);

    // Proof the path ran on the same frame: the gates that do exist reported.
    const auto real_gate = lane_sources(harness.lane(1));
    REQUIRE(real_gate.sensed.has_value());
    REQUIRE(real_gate.sensed->present.has_value());
    CHECK(*real_gate.sensed->present == true);

    CHECK_FALSE(lane_sources(harness.lane(2)).sensed.has_value());
    CHECK_FALSE(lane_sources(harness.lane(3)).sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare's gate name is the spool's, and gate_name wins",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_name", nlohmann::json::array({"Galaxy Black", ""})},
                        {"gate_filament_name", nlohmann::json::array({"EMU Black", "EMU Blue"})}});

    // gate_name is the MMU's own field; gate_filament_name only fills a gap.
    const auto named = lane_sources(harness.lane(0));
    REQUIRE(named.firmware.has_value());
    CHECK(named.firmware->spool_name == "Galaxy Black");
    CHECK_FALSE(named.firmware->color_name.has_value());

    const auto filled = lane_sources(harness.lane(1));
    REQUIRE(filled.firmware.has_value());
    CHECK(filled.firmware->spool_name == "EMU Blue");

    // A gate whose name is wiped stops carrying one, and the emulator's key
    // then fills the gap it left.
    feed_mmu(*harness, {{"gate_name", nlohmann::json::array({"", ""})}});
    const auto wiped = lane_sources(harness.lane(0));
    REQUIRE(wiped.firmware.has_value());
    CHECK_FALSE(wiped.firmware->spool_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a user's own name never decides Happy Hare's name precedence",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    const nlohmann::json gates = {
        {"gate_status", nlohmann::json::array({1, 1})},
        {"gate_spool_id", nlohmann::json::array({0, 0})},
        {"gate_filament_name", nlohmann::json::array({"EMU Black", "EMU Blue"})}};

    // Frame one gives the backend its gates, which an edit needs to address.
    feed_mmu(*harness, gates);

    // SlotInfo::color_name is a field the user's own edit writes, so asking it
    // which of the MMU's two name keys won would let a person's edit answer for
    // the firmware.
    helix::SlotInfo edit = harness->get_slot_info(0);
    edit.color_name = "User Named It";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // Frame two re-states the same names, with the user's own name standing on
    // the merged slot for a read-back to pick up.
    feed_mmu(*harness, gates);

    // Precondition, not the behaviour under test: the edit has to actually win
    // on the merged slot for there to be anything to launder.
    REQUIRE(harness->get_slot_info(0).color_name == "User Named It");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->spool_name == "EMU Black");
}

TEST_CASE_METHOD(LVGLTestFixture, "clearing one field of a gate does not bring the old value back",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    // The user clears the material and keeps the colour. Happy Hare keeps an
    // omitted parameter at its current value, so the write must carry the
    // explicit empty or the gate map goes on remembering PLA - and once the
    // override is gone that remembered value would stand as the machine's
    // own word on what the user deliberately removed.
    auto edit = harness->get_slot_info(0);
    edit.material = "";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_mmu(*harness, {{"gate_material", nlohmann::json::array({""})}});

    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.firmware.has_value());
    CHECK_FALSE(cleared.firmware->material.has_value());
    REQUIRE(cleared.firmware->color_rgb.has_value());
    CHECK(*cleared.firmware->color_rgb == 0xED2C2Cu);

    harness->clear_slot_override(0);
    const auto resolved = helix::ams::resolved_lane(harness.lane(0));
    CHECK_FALSE(resolved.material.has_value());
    // The colour the edit kept survives the clear as the gate map's reading.
    REQUIRE(resolved.color_rgb.has_value());
    CHECK(*resolved.color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a Happy Hare insert nothing names a spool for keeps the details and asks",
                 "[lane][ingest][happy_hare][1710]") {
    HappyHareHarness harness(nullptr, nullptr);
    ToastRecorder toasts;

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    auto edit = harness->get_slot_info(0);
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // A spool goes in. Happy Hare senses occupancy alone and the gate map is
    // memory a person maintains, so the insert itself reads nothing about the
    // spool, and no binding names one: whether the details still standing
    // describe the spool now in the gate is the user's to answer.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})}});
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(toasts.messages.size() == 1);
    CHECK(toasts.messages.front().find("Gate 1") != std::string::npos);
    // Asking changes nothing: the declared details stand until answered.
    const auto resolved = helix::ams::resolved_lane(harness.lane(0));
    REQUIRE(resolved.material.has_value());
    CHECK(*resolved.material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare insert the MMU names a spool for asks nothing",
                 "[lane][ingest][happy_hare][1710]") {
    HappyHareHarness harness(nullptr, nullptr);
    ToastRecorder toasts;

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0})},
                        {"gate_spool_id", nlohmann::json::array({7})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})}});

    auto edit = harness->get_slot_info(0);
    edit.material = "PLA";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The gate fills and the MMU still names spool 7: the binding answers the
    // same-spool question, so nobody is asked. (A different id would have
    // re-bound through the gate_spool_id verdict already.)
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_spool_id", nlohmann::json::array({7})}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(toasts.messages.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare frame with no insert asks nothing",
                 "[lane][ingest][happy_hare][1710]") {
    HappyHareHarness harness(nullptr, nullptr);
    ToastRecorder toasts;

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})}});
    auto edit = harness->get_slot_info(0);
    edit.material = "PLA";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The gate held filament through both frames; the ask belongs to an
    // insert, not to a poll.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(toasts.messages.empty());
}

// --- CFS ---------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "CFS splits bay occupancy from the box's tag memory",
                 "[lane][ingest][cfs]") {
    CfsHarness harness;

    feed_cfs_box(*harness,
                 flat_box(nlohmann::json::array({nlohmann::json{{"index", 0},
                                                                {"material", "PLA"},
                                                                {"brand", "Creality"},
                                                                {"name", "Hyper PLA"},
                                                                {"color", "#ED2C2C"},
                                                                {"present", true},
                                                                {"loaded", false},
                                                                {"spoolman_id", 7}},
                                                 nlohmann::json{{"index", 1},
                                                                {"material", "None"},
                                                                {"brand", "None"},
                                                                {"name", "None"},
                                                                {"color", "None"},
                                                                {"present", false},
                                                                {"loaded", false},
                                                                {"spoolman_id", nullptr}}})));

    const auto loaded_bay = lane_sources(harness.lane(0));
    REQUIRE(loaded_bay.sensed.has_value());
    REQUIRE(loaded_bay.sensed->present.has_value());
    CHECK(*loaded_bay.sensed->present == true);
    // Occupancy is the sensed record's whole business.
    CHECK_FALSE(loaded_bay.sensed->material.has_value());
    CHECK_FALSE(loaded_bay.sensed->color_rgb.has_value());

    REQUIRE(loaded_bay.firmware.has_value());
    CHECK(loaded_bay.firmware->material == "PLA");
    CHECK(loaded_bay.firmware->brand == "Creality");
    REQUIRE(loaded_bay.firmware->color_rgb.has_value());
    CHECK(*loaded_bay.firmware->color_rgb == 0xED2C2Cu);
    CHECK(loaded_bay.firmware->spoolman_id == 7);
    // "Hyper PLA" is which product the bay holds, not what a spool is called.
    CHECK(loaded_bay.firmware->product_name == "Hyper PLA");
    CHECK_FALSE(loaded_bay.firmware->spool_name.has_value());
    // The box weighs nothing, so nothing meters this lane.
    CHECK_FALSE(loaded_bay.metered.has_value());

    // The firmware's "None" is its absence marker on every text field, and the
    // colour it pairs with it is not six hex digits either.
    const auto empty_bay = lane_sources(harness.lane(1));
    REQUIRE(empty_bay.sensed.has_value());
    REQUIRE(empty_bay.sensed->present.has_value());
    CHECK(*empty_bay.sensed->present == false);
    // Filed even with nothing in it, so "the box read no tag" and "no
    // translation ran" stay distinguishable.
    REQUIRE(empty_bay.firmware.has_value());
    CHECK_FALSE(empty_bay.firmware->material.has_value());
    CHECK_FALSE(empty_bay.firmware->brand.has_value());
    CHECK_FALSE(empty_bay.firmware->product_name.has_value());
    CHECK_FALSE(empty_bay.firmware->color_rgb.has_value());
    CHECK_FALSE(empty_bay.firmware->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches CFS's vendor-cache record",
                 "[lane][ingest][cfs]") {
    CfsHarness harness;

    const nlohmann::json box = flat_box(nlohmann::json::array(
        {nlohmann::json{{"index", 0}, {"present", false}}, nlohmann::json{{"index", 1},
                                                                          {"material", "PETG"},
                                                                          {"brand", "Creality"},
                                                                          {"color", "#ED2C2C"},
                                                                          {"present", true}}}));

    // Frame one gives the backend its bays, which an edit needs to address.
    feed_cfs_box(*harness, box);

    // SlotInfo persists across frames and the lane re-layer rewrites it in
    // place at the convergence pass, so a translation reading that struct back
    // would file the user's own choice as something the box remembers.
    helix::SlotInfo edit = harness->get_slot_info(1);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 1, edit);

    // Frame two re-states the same bay, with the user's choice standing on the
    // merged slot for a read-back to pick up.
    feed_cfs_box(*harness, box);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch and
    // both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "each CFS bay accumulates its own identity",
                 "[lane][ingest][cfs]") {
    CfsHarness harness;

    feed_cfs_box(
        *harness,
        flat_box(nlohmann::json::array(
            {nlohmann::json{
                 {"index", 0}, {"material", "PLA"}, {"color", "#111111"}, {"present", true}},
             nlohmann::json{
                 {"index", 1}, {"material", "None"}, {"color", "None"}, {"present", false}},
             nlohmann::json{
                 {"index", 2}, {"material", "PETG"}, {"color", "#333333"}, {"present", true}},
             nlohmann::json{
                 {"index", 3}, {"material", "ABS"}, {"color", "#444444"}, {"present", true}}})));

    // Values, not has_value(). Per-bay keys with every record filed on one lane
    // is the shape this defect actually takes, and only values catch it.
    const auto bay0 = lane_sources(harness.lane(0));
    REQUIRE(bay0.firmware.has_value());
    CHECK(bay0.firmware->material == "PLA");
    REQUIRE(bay0.firmware->color_rgb.has_value());
    CHECK(*bay0.firmware->color_rgb == 0x111111u);

    const auto bay1 = lane_sources(harness.lane(1));
    REQUIRE(bay1.firmware.has_value());
    CHECK_FALSE(bay1.firmware->material.has_value());
    CHECK_FALSE(bay1.firmware->color_rgb.has_value());
    REQUIRE(bay1.sensed.has_value());
    CHECK(*bay1.sensed->present == false);

    const auto bay2 = lane_sources(harness.lane(2));
    REQUIRE(bay2.firmware.has_value());
    CHECK(bay2.firmware->material == "PETG");
    REQUIRE(bay2.firmware->color_rgb.has_value());
    CHECK(*bay2.firmware->color_rgb == 0x333333u);

    const auto bay3 = lane_sources(harness.lane(3));
    REQUIRE(bay3.firmware.has_value());
    CHECK(bay3.firmware->material == "ABS");
    REQUIRE(bay3.firmware->color_rgb.has_value());
    CHECK(*bay3.firmware->color_rgb == 0x444444u);
}

TEST_CASE_METHOD(LVGLTestFixture, "a CFS frame that names one unit leaves the others standing",
                 "[lane][ingest][cfs]") {
    CfsHarness harness;

    feed_cfs_box(*harness,
                 stock_box("T1", nlohmann::json{{"vender", nlohmann::json::array({"Creality"})},
                                                {"remain_len", nlohmann::json::array({"100"})},
                                                {"color_value", nlohmann::json::array({"0ED2C2C"})},
                                                {"material_type", nlohmann::json::array({"-1"})}}));

    const auto first_unit = lane_sources(harness.lane(0));
    REQUIRE(first_unit.sensed.has_value());
    REQUIRE(first_unit.sensed->present.has_value());
    CHECK(*first_unit.sensed->present == true);
    REQUIRE(first_unit.firmware.has_value());
    REQUIRE(first_unit.firmware->color_rgb.has_value());
    CHECK(*first_unit.firmware->color_rgb == 0xED2C2Cu);
    CHECK(first_unit.firmware->brand == "Creality");

    // The next frame is about the second unit only. The box said nothing about
    // the first, and one ingest replaces a source's record whole, so filing an
    // empty record for the bays it did not mention would erase live readings.
    feed_cfs_box(*harness,
                 stock_box("T2", nlohmann::json{{"vender", nlohmann::json::array({"Xplorer"})},
                                                {"remain_len", nlohmann::json::array({"80"})},
                                                {"color_value", nlohmann::json::array({"0112233"})},
                                                {"material_type", nlohmann::json::array({"-1"})}}));

    // The first unit's lanes first, and with no REQUIRE above them: the failure
    // this case exists for is the second unit's readings landing on the first
    // unit's lanes, and an abort on "lane 4 is empty" would report the symptom
    // three assertions before the cause.
    const auto still = lane_sources(harness.lane(0));
    const bool the_first_unit_lost_its_reading =
        !still.sensed.has_value() || !still.sensed->present.has_value() || !*still.sensed->present;
    CHECK_FALSE(the_first_unit_lost_its_reading);
    const bool the_first_unit_took_the_second_units_colour =
        still.firmware.has_value() && still.firmware->color_rgb.has_value() &&
        *still.firmware->color_rgb != 0xED2C2Cu;
    CHECK_FALSE(the_first_unit_took_the_second_units_colour);
    const bool the_first_unit_took_the_second_units_brand =
        still.firmware.has_value() && still.firmware->brand == "Xplorer";
    CHECK_FALSE(the_first_unit_took_the_second_units_brand);
    REQUIRE(still.firmware.has_value());
    REQUIRE(still.firmware->color_rgb.has_value());
    CHECK(*still.firmware->color_rgb == 0xED2C2Cu);

    const auto second_unit = lane_sources(harness.lane(4));
    REQUIRE(second_unit.firmware.has_value());
    REQUIRE(second_unit.firmware->color_rgb.has_value());
    CHECK(*second_unit.firmware->color_rgb == 0x112233u);
    CHECK(second_unit.firmware->brand == "Xplorer");
}

TEST_CASE_METHOD(LVGLTestFixture, "CFS reads both its wire colours by the shared grammar",
                 "[lane][ingest][cfs]") {
    SECTION("a stock value with anything after the colour is no colour") {
        CfsHarness harness;
        // Creality's own spelling is a leading zero and six hex digits. A value
        // carrying more than that is refused whole rather than read up to its
        // first bad character, which would file a plausible wrong colour.
        feed_cfs_box(
            *harness,
            stock_box("T1", nlohmann::json{{"vender", nlohmann::json::array({"Creality"})},
                                           {"remain_len", nlohmann::json::array({"100"})},
                                           {"color_value", nlohmann::json::array({"0ED2C2CZZ"})},
                                           {"material_type", nlohmann::json::array({"-1"})}}));

        const auto lane = lane_sources(harness.lane(0));
        REQUIRE(lane.firmware.has_value());
        CHECK_FALSE(lane.firmware->color_rgb.has_value());
        // The bay is still occupied; only its colour is unreadable.
        REQUIRE(lane.sensed.has_value());
        CHECK(*lane.sensed->present == true);
    }

    SECTION("a stock value the old parse refused now reads") {
        CfsHarness harness;
        // Creality's leading-zero "0RRGGBB" is this function's own to strip;
        // what is left is read by the grammar every other lane-shaped producer
        // uses, so the ordinary web spellings mean here what they mean there.
        // Nothing validates this field before it reaches the parse.
        feed_cfs_box(
            *harness,
            stock_box("T1",
                      nlohmann::json{
                          {"vender", nlohmann::json::array({"Creality", "Creality", "Creality"})},
                          {"remain_len", nlohmann::json::array({"100", "100", "100"})},
                          {"color_value", nlohmann::json::array({"#ED2C2C", "#F00", "0ED2C2C"})},
                          {"material_type", nlohmann::json::array({"-1", "-1", "-1"})}}));

        const auto hashed = lane_sources(harness.lane(0));
        REQUIRE(hashed.firmware.has_value());
        REQUIRE(hashed.firmware->color_rgb.has_value());
        CHECK(*hashed.firmware->color_rgb == 0xED2C2Cu);

        const auto three_digit = lane_sources(harness.lane(1));
        REQUIRE(three_digit.firmware.has_value());
        REQUIRE(three_digit.firmware->color_rgb.has_value());
        CHECK(*three_digit.firmware->color_rgb == 0xFF0000u);

        // The Creality form this function still owns, so the delegation cannot
        // quietly take the leading-zero rule with it.
        const auto creality = lane_sources(harness.lane(2));
        REQUIRE(creality.firmware.has_value());
        REQUIRE(creality.firmware->color_rgb.has_value());
        CHECK(*creality.firmware->color_rgb == 0xED2C2Cu);
    }

    SECTION("the flat schema reads every spelling the shared grammar accepts") {
        CfsHarness harness;
        feed_cfs_box(*harness,
                     flat_box(nlohmann::json::array(
                         {nlohmann::json{{"index", 0}, {"color", "#F00"}, {"present", true}},
                          nlohmann::json{{"index", 1}, {"color", "0xED2C2C"}, {"present", true}},
                          nlohmann::json{{"index", 2}, {"color", "112233FF"}, {"present", true}},
                          nlohmann::json{{"index", 3}, {"color", "nothex"}, {"present", true}}})));

        const auto three_digit = lane_sources(harness.lane(0));
        REQUIRE(three_digit.firmware.has_value());
        REQUIRE(three_digit.firmware->color_rgb.has_value());
        CHECK(*three_digit.firmware->color_rgb == 0xFF0000u);

        const auto prefixed = lane_sources(harness.lane(1));
        REQUIRE(prefixed.firmware.has_value());
        REQUIRE(prefixed.firmware->color_rgb.has_value());
        CHECK(*prefixed.firmware->color_rgb == 0xED2C2Cu);

        const auto with_alpha = lane_sources(harness.lane(2));
        REQUIRE(with_alpha.firmware.has_value());
        REQUIRE(with_alpha.firmware->color_rgb.has_value());
        CHECK(*with_alpha.firmware->color_rgb == 0x112233u);

        const auto refused = lane_sources(harness.lane(3));
        REQUIRE(refused.firmware.has_value());
        CHECK_FALSE(refused.firmware->color_rgb.has_value());
    }
}

// --- ACE ---------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "ACE splits slot status from slot metadata",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness,
             nlohmann::json{
                 {"status", "ready"},
                 {"slots", nlohmann::json::array({
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({237, 44, 44})},
                                              {"type", "PETG"}},
                               nlohmann::json{{"status", "empty"},
                                              {"color", nlohmann::json::array({0, 0, 0})},
                                              {"type", ""}},
                           })},
             });

    const auto slot0 = lane_sources(harness.lane(0));
    REQUIRE(slot0.sensed.has_value());
    REQUIRE(slot0.sensed->present.has_value());
    CHECK(*slot0.sensed->present == true);
    CHECK_FALSE(slot0.sensed->color_rgb.has_value());
    REQUIRE(slot0.firmware.has_value());
    REQUIRE(slot0.firmware->color_rgb.has_value());
    CHECK(*slot0.firmware->color_rgb == 0xED2C2Cu);
    CHECK(slot0.firmware->material == "PETG");
    // The hub weighs nothing and reads no tag, so no other source speaks.
    CHECK_FALSE(slot0.metered.has_value());
    CHECK_FALSE(slot0.spoolman.has_value());

    const auto slot1 = lane_sources(harness.lane(1));
    REQUIRE(slot1.sensed.has_value());
    REQUIRE(slot1.sensed->present.has_value());
    CHECK(*slot1.sensed->present == false);
    // ACE reports [0,0,0] for an empty slot. That is a real reading of black
    // from a hub that has no way to say "no reading", and the model records it
    // as the cache's word rather than inventing an absence.
    REQUIRE(slot1.firmware.has_value());
    REQUIRE(slot1.firmware->color_rgb.has_value());
    CHECK(*slot1.firmware->color_rgb == 0x000000u);
    CHECK_FALSE(slot1.firmware->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches ACE's vendor-cache record",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness,
             nlohmann::json{
                 {"slots", nlohmann::json::array({
                               nlohmann::json{{"status", "empty"}},
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({237, 44, 44})},
                                              {"type", "PETG"}},
                           })},
             });

    // SlotInfo persists across frames and the lane re-layer rewrites it in
    // place at the end of every slot iteration, so a translation reading that
    // struct back would file the user's own choice as the hub's memory.
    helix::SlotInfo edit = harness->get_slot_info(1);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 1, edit);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch and
    // both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->material == "PETG");

    // A second frame that states occupancy and nothing else. This is the phase
    // that can actually launder: the merge has been applied to the slot since
    // the last frame, so the struct now holds the user's colour and material
    // under keys this frame never mentioned, and a translation reading it back
    // would file both as the hub's own memory.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "empty"}},
                                                    nlohmann::json{{"status", "ready"}},
                                                })}});

    // Precondition again: the override is still what the merged slot shows, so
    // there is still something for the record to launder.
    const auto after = harness->get_slot_info(1);
    REQUIRE(after.color_rgb == 0x00FF00u);
    REQUIRE(after.material == "ABS");

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.firmware.has_value());
    // Named separately from the two below so the failure carries its own
    // diagnosis: a record that merely narrowed wrongly and one that filed the
    // user's own edit as a hub reading are different defects.
    const bool the_users_colour_was_filed_as_the_hubs =
        silent.firmware->color_rgb.has_value() && *silent.firmware->color_rgb == 0x00FF00u;
    CHECK_FALSE(the_users_colour_was_filed_as_the_hubs);
    const bool the_users_material_was_filed_as_the_hubs =
        silent.firmware->material.has_value() && *silent.firmware->material == "ABS";
    CHECK_FALSE(the_users_material_was_filed_as_the_hubs);
    CHECK_FALSE(silent.firmware->color_rgb.has_value());
    CHECK_FALSE(silent.firmware->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a slot ACE states no status for neither sets nor erases presence",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    // Nothing on this lane may abort the case: the decisive claim is the last
    // phase, and a leading REQUIRE would report "no record filed" for a rule
    // whose actual failure is a stale reading being erased.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"type", "PLA"}},
                                                })}});

    const auto unread = lane_sources(harness.lane(0));
    // The cache record proves the translation ran, so the absence below is
    // about the hub's status key and not about an ingest that never happened.
    REQUIRE(unread.firmware.has_value());
    CHECK(unread.firmware->material == "PLA");
    CHECK_FALSE(unread.sensed.has_value());

    // The same slot once the hub does speak. This half is what makes the half
    // above a guard rather than a backend that never reports presence.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "ready"}},
                                                })}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // A frame that states no status again. The hub's presence authority is the
    // key, not a latch, so filing an empty record here would erase a live
    // reading instead of repeating it.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"type", "PLA"}},
                                                })}});

    const auto still = lane_sources(harness.lane(0));
    const bool a_live_reading_was_erased =
        !still.sensed.has_value() || !still.sensed->present.has_value();
    CHECK_FALSE(a_live_reading_was_erased);
    CHECK(still.sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a status ACE does not recognise retracts its reading",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "ready"}},
                                                })}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // "unknown" is in ACE's own vocabulary and everything outside it lands in
    // the same place: the hub saying it does not know. That is news, and a
    // record whose field is unset is the only way to retract a reading.
    feed_ace(*harness, nlohmann::json{{"slots", nlohmann::json::array({
                                                    nlohmann::json{{"status", "unknown"}},
                                                })}});

    const auto withdrawn = lane_sources(harness.lane(0));
    const bool a_stale_reading_still_stands =
        withdrawn.sensed.has_value() && withdrawn.sensed->present.has_value();
    CHECK_FALSE(a_stale_reading_still_stands);
    CHECK(withdrawn.sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "each ACE slot accumulates its own identity",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness,
             nlohmann::json{
                 {"slots", nlohmann::json::array({
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({17, 17, 17})},
                                              {"type", "PLA"}},
                               nlohmann::json{{"status", "empty"}},
                               nlohmann::json{{"status", "ready"},
                                              {"color", nlohmann::json::array({51, 51, 51})},
                                              {"type", "PETG"}},
                               nlohmann::json{{"status", "loaded"},
                                              {"color", nlohmann::json::array({68, 68, 68})},
                                              {"type", "ABS"}},
                           })},
             });

    // Values, not has_value(). Per-slot keys with every record filed on one
    // lane is the shape this defect actually takes, and only values catch it.
    const auto slot0 = lane_sources(harness.lane(0));
    REQUIRE(slot0.firmware.has_value());
    CHECK(slot0.firmware->material == "PLA");
    REQUIRE(slot0.firmware->color_rgb.has_value());
    CHECK(*slot0.firmware->color_rgb == 0x111111u);

    const auto slot1 = lane_sources(harness.lane(1));
    REQUIRE(slot1.firmware.has_value());
    CHECK_FALSE(slot1.firmware->material.has_value());
    CHECK_FALSE(slot1.firmware->color_rgb.has_value());
    REQUIRE(slot1.sensed.has_value());
    CHECK(*slot1.sensed->present == false);

    const auto slot2 = lane_sources(harness.lane(2));
    REQUIRE(slot2.firmware.has_value());
    CHECK(slot2.firmware->material == "PETG");
    REQUIRE(slot2.firmware->color_rgb.has_value());
    CHECK(*slot2.firmware->color_rgb == 0x333333u);

    const auto slot3 = lane_sources(harness.lane(3));
    REQUIRE(slot3.firmware.has_value());
    CHECK(slot3.firmware->material == "ABS");
    REQUIRE(slot3.firmware->color_rgb.has_value());
    CHECK(*slot3.firmware->color_rgb == 0x444444u);
    REQUIRE(slot3.sensed.has_value());
    CHECK(*slot3.sensed->present == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "an ACE colour that will not read is no colour",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    feed_ace(*harness,
             nlohmann::json{{"slots", nlohmann::json::array({
                                          nlohmann::json{{"status", "ready"}, {"color", "#ED2C2C"}},
                                      })}});

    const auto readable = lane_sources(harness.lane(0));
    REQUIRE(readable.firmware.has_value());
    REQUIRE(readable.firmware->color_rgb.has_value());
    CHECK(*readable.firmware->color_rgb == 0xED2C2Cu);

    // Pure black is a colour ACE really reports, so "could not read this" must
    // not land on it. The record states no colour, and the bay keeps showing
    // the last one the hub did state.
    feed_ace(
        *harness,
        nlohmann::json{{"slots", nlohmann::json::array({
                                     nlohmann::json{{"status", "ready"}, {"color", "notahexvalue"}},
                                 })}});

    const auto unreadable = lane_sources(harness.lane(0));
    REQUIRE(unreadable.firmware.has_value());
    CHECK_FALSE(unreadable.firmware->color_rgb.has_value());
    CHECK(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // A short array carries no triplet and is the same answer.
    feed_ace(*harness,
             nlohmann::json{{"slots", nlohmann::json::array({
                                          nlohmann::json{{"status", "ready"},
                                                         {"color", nlohmann::json::array({17})}},
                                      })}});

    const auto truncated = lane_sources(harness.lane(0));
    REQUIRE(truncated.firmware.has_value());
    CHECK_FALSE(truncated.firmware->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE reads a colour string by the shared grammar",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    // The bridge's string form, in the spellings a hand-rolled hex read gets
    // wrong: a three-digit colour expands rather than landing on 0x000F00, an
    // alpha suffix is sliced off rather than shifting the colour out of range,
    // and a value with anything after the digits is refused rather than read up
    // to its first bad character.
    feed_ace(*harness, nlohmann::json{
                           {"slots", nlohmann::json::array({
                                         nlohmann::json{{"status", "ready"}, {"color", "#F00"}},
                                         nlohmann::json{{"status", "ready"}, {"color", "112233FF"}},
                                         nlohmann::json{{"status", "ready"}, {"color", "ED2C2CZZ"}},
                                         nlohmann::json{{"status", "ready"}, {"color", "0xED2C2C"}},
                                     })}});

    const auto three_digit = lane_sources(harness.lane(0));
    REQUIRE(three_digit.firmware.has_value());
    REQUIRE(three_digit.firmware->color_rgb.has_value());
    CHECK(*three_digit.firmware->color_rgb == 0xFF0000u);
    CHECK(harness->get_slot_info(0).color_rgb == 0xFF0000u);

    const auto with_alpha = lane_sources(harness.lane(1));
    REQUIRE(with_alpha.firmware.has_value());
    REQUIRE(with_alpha.firmware->color_rgb.has_value());
    CHECK(*with_alpha.firmware->color_rgb == 0x112233u);

    const auto trailing_junk = lane_sources(harness.lane(2));
    REQUIRE(trailing_junk.firmware.has_value());
    CHECK_FALSE(trailing_junk.firmware->color_rgb.has_value());

    const auto prefixed = lane_sources(harness.lane(3));
    REQUIRE(prefixed.firmware.has_value());
    REQUIRE(prefixed.firmware->color_rgb.has_value());
    CHECK(*prefixed.firmware->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE's REST bridge files on the same lanes as its subscription",
                 "[lane][ingest][ace]") {
    AceHarness harness(nullptr, nullptr);

    // The bridge is a second parser with its own key ladder: `material` before
    // `type`, and a hex string rather than a triplet. It polls the same hub, so
    // it files the same account on the same lanes.
    AceTestAccess::parse_slots(*harness, nlohmann::json{
                                             {"slots", nlohmann::json::array({
                                                           nlohmann::json{{"status", "ready"},
                                                                          {"color", "#ED2C2C"},
                                                                          {"material", "PETG"},
                                                                          {"type", "PLA"}},
                                                           nlohmann::json{{"status", "empty"}},
                                                       })},
                                         });

    const auto slot0 = lane_sources(harness.lane(0));
    REQUIRE(slot0.sensed.has_value());
    REQUIRE(slot0.sensed->present.has_value());
    CHECK(*slot0.sensed->present == true);
    REQUIRE(slot0.firmware.has_value());
    REQUIRE(slot0.firmware->color_rgb.has_value());
    CHECK(*slot0.firmware->color_rgb == 0xED2C2Cu);
    CHECK(slot0.firmware->material == "PETG");

    const auto slot1 = lane_sources(harness.lane(1));
    REQUIRE(slot1.sensed.has_value());
    REQUIRE(slot1.sensed->present.has_value());
    CHECK(*slot1.sensed->present == false);
    REQUIRE(slot1.firmware.has_value());
    CHECK_FALSE(slot1.firmware->material.has_value());
    CHECK_FALSE(slot1.firmware->color_rgb.has_value());
}

// ============================================================================
// Snapmaker
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker's RFID product line is not a spool name",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness;

    feed_filament_detect(
        *harness,
        nlohmann::json{
            {"state", nlohmann::json::array({1, 0, 0, 0})},
            {"info", nlohmann::json::array({nlohmann::json{
                         {"MAIN_TYPE", "PLA"},
                         {"SUB_TYPE", "Silk"},
                         {"MANUFACTURER", "Snapmaker"},
                         {"ARGB_COLOR", 0xFFED2C2C},
                         {"WEIGHT", 1000},
                         {"CARD_UID", nlohmann::json::array({144, 32, 196, 2})},
                     }})},
        },
        // Presence from the port sensor, not the state array:
        // lane 0 holds filament, lane 1 is empty.
        nlohmann::json{{"extruder0", nlohmann::json{{"filament_detected", true}}},
                       {"extruder1", nlohmann::json{{"filament_detected", false}}}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.firmware.has_value());
    // MAIN_TYPE is the material and keeps the field.
    CHECK(lane.firmware->material == "PLA");
    // SUB_TYPE names the product line inside that material, so it is the
    // branded product rather than a second spelling of the material. The
    // SlotInfo this same parse writes spells it spool_name; the record
    // deliberately does not agree with it.
    CHECK(lane.firmware->product_name == "Silk");
    CHECK_FALSE(lane.firmware->spool_name.has_value());
    CHECK(harness->get_slot_info(0).spool_name == "Silk");

    CHECK(lane.firmware->brand == "Snapmaker");
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    REQUIRE(lane.firmware->total_weight_g.has_value());
    CHECK(*lane.firmware->total_weight_g == 1000.0F);

    // A channel the same frame reports empty. The info array carried one entry,
    // so nothing declared anything here.
    const auto empty = lane_sources(harness.lane(1));
    REQUIRE(empty.sensed.has_value());
    REQUIRE(empty.sensed->present.has_value());
    CHECK(*empty.sensed->present == false);
    CHECK_FALSE(empty.firmware.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker's NONE tag states nothing about identity",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness;

    feed_filament_detect(
        *harness,
        nlohmann::json{
            {"state", nlohmann::json::array({1, 1, 0, 0})},
            {"info", nlohmann::json::array({
                         nlohmann::json{{"MAIN_TYPE", "NONE"}},
                         // The positive control, and it has to live in this same
                         // info array. The Sensed record cannot play that part:
                         // it is filed from the feed frame's presence, over a
                         // different wire key, so it stays green with the whole
                         // identity ingest deleted. A neighbour that DOES declare
                         // is what makes lane 0's silence a decision the loop
                         // took.
                         nlohmann::json{{"MAIN_TYPE", "PLA"}},
                     })},
        },
        nlohmann::json{{"extruder0", nlohmann::json{{"filament_detected", true}}}});

    const auto lane = lane_sources(harness.lane(0));
    // Presence still came through: the port sensor sees filament even with no
    // readable tag, which is the whole shape of "sensed, not declared".
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);
    CHECK_FALSE(lane.firmware.has_value());

    const auto control = lane_sources(harness.lane(1));
    REQUIRE(control.firmware.has_value());
    CHECK(control.firmware->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker files no colour for a tag that carried none",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness;

    // A tag with an identity but no ARGB_COLOR. SnapmakerRfidInfo::color_rgb
    // rests on kAmsDefaultSlotColor, which is that struct's "no reading" and
    // not a grey a vendor printed on a spool.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1, 1})},
                                       {"info", nlohmann::json::array({
                                                    nlohmann::json{
                                                        {"MAIN_TYPE", "PETG"},
                                                        {"SUB_TYPE", "NONE"},
                                                    },
                                                    // A tag the reader answered for but that
                                                    // named nothing at all. Every field rests
                                                    // on its struct default, so the record
                                                    // carries none of them.
                                                    nlohmann::json{{"BED_TEMP", 60}},
                                                })},
                                   });

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->material == "PETG");
    CHECK_FALSE(lane.firmware->color_rgb.has_value());
    CHECK_FALSE(lane.firmware->product_name.has_value());
    CHECK_FALSE(lane.firmware->total_weight_g.has_value());
    CHECK_FALSE(lane.firmware->brand.has_value());
    // The sentinel still reaches SlotInfo, so the record's silence is the only
    // place the difference between "grey" and "no reading" survives.
    CHECK(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.firmware.has_value());
    CHECK_FALSE(silent.firmware->material.has_value());
    CHECK_FALSE(silent.firmware->brand.has_value());
    CHECK_FALSE(silent.firmware->color_rgb.has_value());
    CHECK_FALSE(silent.firmware->product_name.has_value());
    CHECK_FALSE(silent.firmware->total_weight_g.has_value());

    // Pure black is a colour a vendor can print, and the same guard keeps it.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PETG"},
                                                    {"ARGB_COLOR", 0xFF000000},
                                                }})},
                                   });

    const auto black = lane_sources(harness.lane(0));
    REQUIRE(black.firmware.has_value());
    REQUIRE(black.firmware->color_rgb.has_value());
    CHECK(*black.firmware->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches Snapmaker's vendor-cache record",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness;
    snapmaker_idle(*harness); // an edit writes print_task_config only once a state is read

    const auto tag = nlohmann::json{
        {"state", nlohmann::json::array({1})},
        {"info", nlohmann::json::array({nlohmann::json{
                     {"MAIN_TYPE", "PLA"},
                     {"ARGB_COLOR", 0xFFED2C2C},
                 }})},
    };

    // Frame one establishes firmware truth on the lane.
    feed_filament_detect(*harness, tag);
    REQUIRE(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // The user overrides it. The lane re-layer runs at the tail of every frame
    // and rewrites the persistent SlotInfo in place, so from here the merged
    // struct carries the user's colour and material rather than the tag's.
    helix::SlotInfo edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // Frame two re-reads the same tag. The record must say what the tag says.
    feed_filament_detect(*harness, tag);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for the case to catch and
    // the assertions below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(0).material == "ABS");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.firmware.has_value());
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->material == "PLA");
}

// ============================================================================
// Tool changer
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a tool changer senses docking and declares nothing",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);

    harness->set_discovered_tools({"T0", "T1"});

    // AmsState hands a backend its tool list BEFORE add_backend() stamps an
    // index, so the records initialize_tools() filed land on no lane in
    // production. Drop them and drive a real status frame, which is the path a
    // machine takes.
    helix::ams::reset_lane_sources();
    REQUIRE_FALSE(lane_sources(harness.lane(0)).sensed.has_value());
    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->tool_docked.has_value());
    CHECK(*lane.sensed->tool_docked == true);
    CHECK_FALSE(lane.sensed->present.has_value());

    // klipper-toolchanger reports whether a tool is docked and nothing about
    // what it holds. initialize_tools() puts the tool's own name in
    // SlotInfo::spool_name and rests the colour on the sentinel; neither is a
    // reading, so this backend files no identity record at all. Both are
    // asserted here because they are what a read-back of the merged struct
    // would have filed.
    CHECK(harness->get_slot_info(0).spool_name == "T0");
    CHECK(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);
    CHECK_FALSE(lane.firmware.has_value());

    const auto second = lane_sources(harness.lane(1));
    REQUIRE(second.sensed.has_value());
    REQUIRE(second.sensed->tool_docked.has_value());
    CHECK(*second.sensed->tool_docked == true);
    CHECK_FALSE(second.sensed->present.has_value());
    CHECK_FALSE(second.firmware.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a vacated dock retracts a tool changer's presence reading",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1", "T2"});
    harness->set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // AmsState hands a backend its tool list BEFORE add_backend() stamps an
    // index, so nothing the resulting initialize_tools() files has a lane to
    // land on in production. Dropping those records leaves this case resting on
    // the status path alone, which is the one that runs with an index.
    helix::ams::reset_lane_sources();
    REQUIRE_FALSE(lane_sources(harness.lane(0)).sensed.has_value());

    // T1 is on the head and T2's dock reads vacant, which means that hot end
    // has been taken out of the machine.
    feed_toolchanger(*harness,
                     nlohmann::json{{"medusahc",
                                     {{"operation", "idle"},
                                      {"current_tool", 1},
                                      {"sensors", {{"e", 1}, {"t0", 1}, {"t1", 0}, {"t2", 0}}}}}});

    REQUIRE(harness->get_slot_info(2).status == helix::SlotStatus::EMPTY);

    const auto docked = lane_sources(harness.lane(0));
    REQUIRE(docked.sensed.has_value());
    REQUIRE(docked.sensed->tool_docked.has_value());
    CHECK(*docked.sensed->tool_docked == true);

    const auto carriage = lane_sources(harness.lane(1));
    REQUIRE(carriage.sensed.has_value());
    REQUIRE(carriage.sensed->tool_docked.has_value());
    CHECK(*carriage.sensed->tool_docked == true);

    const auto vacated = lane_sources(harness.lane(2));
    REQUIRE(vacated.sensed.has_value());
    REQUIRE(vacated.sensed->tool_docked.has_value());
    CHECK(*vacated.sensed->tool_docked == false);
    CHECK_FALSE(vacated.sensed->present.has_value());
    CHECK_FALSE(vacated.firmware.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never becomes a tool changer's vendor reading",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1"});

    // Only what the status frame below files may answer the assertions, so the
    // lane starts empty of whatever tool discovery left on it.
    helix::ams::reset_lane_sources();

    // On this backend a person's edit is the ONLY source of filament identity,
    // so every identity field on the merged slot is the user's own statement
    // and there is nothing else for a read-back to pick up.
    helix::SlotInfo edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The production laundering shape, which a second set_discovered_tools()
    // does not reach: a real status frame, where refresh_slot_statuses_locked
    // files the reading and the tail re-layer runs after it.
    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(0).material == "ABS");

    const auto lane = lane_sources(harness.lane(0));
    // The dock record is the proof that the translation ran on this frame.
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->tool_docked.has_value());
    CHECK(*lane.sensed->tool_docked == true);
    CHECK_FALSE(lane.firmware.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Snapmaker reads NONE as a tag saying nothing, not as a brand",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness;

    // "NONE" is the firmware's spelling for an unset string, and it reaches the
    // brand through two keys: MANUFACTURER is preferred and VENDOR is the
    // fallback, so each has to be refused on its own.
    feed_filament_detect(
        *harness, nlohmann::json{
                      {"state", nlohmann::json::array({1, 1, 1})},
                      {"info", nlohmann::json::array({
                                   nlohmann::json{{"MAIN_TYPE", "PLA"}, {"MANUFACTURER", "NONE"}},
                                   nlohmann::json{{"MAIN_TYPE", "PLA"}, {"VENDOR", "NONE"}},
                                   nlohmann::json{{"MAIN_TYPE", "PLA"}, {"VENDOR", "Polymaker"}},
                               })},
                  });

    const auto manufacturer_none = lane_sources(harness.lane(0));
    REQUIRE(manufacturer_none.firmware.has_value());
    CHECK_FALSE(manufacturer_none.firmware->brand.has_value());
    // The material on the same record is the control: this entry was read and
    // filed, and the brand guard is what dropped the one field.
    CHECK(manufacturer_none.firmware->material == "PLA");

    const auto vendor_none = lane_sources(harness.lane(1));
    REQUIRE(vendor_none.firmware.has_value());
    CHECK_FALSE(vendor_none.firmware->brand.has_value());
    CHECK(vendor_none.firmware->material == "PLA");

    // The fallback key naming a real vendor still reaches the record.
    const auto vendor_real = lane_sources(harness.lane(2));
    REQUIRE(vendor_real.firmware.has_value());
    CHECK(vendor_real.firmware->brand == "Polymaker");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Snapmaker tag that stops naming a vendor retracts it",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness;

    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"SUB_TYPE", "Silk"},
                                                }})},
                                   });

    const auto stated = lane_sources(harness.lane(0));
    REQUIRE(stated.firmware.has_value());
    CHECK(stated.firmware->brand == "Snapmaker");
    CHECK(stated.firmware->product_name == "Silk");
    REQUIRE(harness->get_slot_info(0).brand == "Snapmaker");
    REQUIRE(harness->get_slot_info(0).spool_name == "Silk");

    // The firmware spelling "NONE". The record retracts, because whole-record
    // replacement means it states what THIS read said, and the spool name
    // retracts with it: firmware stores NONE for a write that sent no product
    // line, so a kept name would outlive the spool it described. SlotInfo's
    // brand keeps the old value, because its guard tests the literal and
    // print_task_config restates the vendor on its own.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "NONE"},
                                                    {"SUB_TYPE", "NONE"},
                                                }})},
                                   });

    const auto spelled_none = lane_sources(harness.lane(0));
    REQUIRE(spelled_none.firmware.has_value());
    // Material is the control: this read did reach the ingest.
    CHECK(spelled_none.firmware->material == "PLA");
    CHECK_FALSE(spelled_none.firmware->brand.has_value());
    CHECK_FALSE(spelled_none.firmware->product_name.has_value());
    CHECK(harness->get_slot_info(0).brand == "Snapmaker");
    CHECK(harness->get_slot_info(0).spool_name.empty());

    // An ABSENT key is the other spelling of the same silence, and both layers
    // blank on it.
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                }})},
                                   });

    const auto absent = lane_sources(harness.lane(0));
    REQUIRE(absent.firmware.has_value());
    CHECK(absent.firmware->material == "PLA");
    CHECK_FALSE(absent.firmware->brand.has_value());
    CHECK_FALSE(absent.firmware->product_name.has_value());
    CHECK(harness->get_slot_info(0).brand.empty());
    CHECK(harness->get_slot_info(0).spool_name.empty());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Snapmaker's print_task_config is the firmware's reading of type and colour",
                 "[lane][ingest][snapmaker]") {
    SnapmakerHarness harness;

    // Two channels sensed by the port sensor, one tag between them. Channel 0
    // has a tag read for print_task_config to contend with; channel 1 has
    // none, so what the merged struct shows there is the write surface's own
    // work and nothing else.
    feed_filament_detect(
        *harness,
        nlohmann::json{
            {"state", nlohmann::json::array({1, 1})},
            {"info", nlohmann::json::array({nlohmann::json{
                         {"MAIN_TYPE", "PLA"},
                         {"MANUFACTURER", "Snapmaker"},
                         {"ARGB_COLOR", 0xFFED2C2C},
                     }})},
        },
        nlohmann::json{{"extruder0", nlohmann::json{{"filament_detected", true}}},
                       {"extruder1", nlohmann::json{{"filament_detected", true}}}});

    // Whoever set it (the machine's screen, a slicer, this backend's edit),
    // print_task_config is what the head prints with: its type and colour are
    // the lane's firmware reading, over the tag's. Its vendor is a category,
    // never the spool's brand.
    feed_print_task_config(
        *harness, nlohmann::json{
                      {"filament_type", nlohmann::json::array({"PETG", "PETG"})},
                      {"filament_vendor", nlohmann::json::array({"SomebodyElse", "SomebodyElse"})},
                      {"filament_color_rgba", nlohmann::json::array({"00FF00FF", "00FF00FF"})},
                  });

    // The control: the parse ran and took every field onto the merged struct.
    // An unobserved field leaves the backend's own write standing, so the
    // untagged channel is where that write is visible.
    REQUIRE(harness->get_slot_info(1).material == "PETG");
    REQUIRE(harness->get_slot_info(1).brand == "SomebodyElse");
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);

    const auto untagged = lane_sources(harness.lane(1));
    REQUIRE(untagged.sensed.has_value());
    REQUIRE(untagged.firmware.has_value());
    CHECK(untagged.firmware->material == "PETG");
    CHECK(untagged.firmware->color_rgb == std::optional<uint32_t>(0x00FF00u));
    CHECK_FALSE(untagged.firmware->brand.has_value());

    // The tagged channel: print_task_config's type and colour over the tag's,
    // the tag's brand kept.
    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->material == "PETG");
    CHECK(lane.firmware->brand == "Snapmaker");
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture, "a Snapmaker edit leaves the tag's next reading whole",
                 "[lane][ingest][snapmaker]") {
    // A user edit reaches firmware as SET_PRINT_FILAMENT_CONFIG, which writes
    // print_task_config and nothing the tag reader reports. So the tag's next
    // reading on the same spool is the tag's, even where it states exactly
    // what the user typed.
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    SnapmakerHarness harness(&api);
    snapmaker_idle(*harness); // an edit writes print_task_config only once a state is read

    const auto tag_uid = nlohmann::json::array({144, 32, 196, 2});
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"MANUFACTURER", "Snapmaker"},
                                                    {"ARGB_COLOR", 0xFFED2C2C},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    auto edit = harness->get_slot_info(0);
    edit.brand = "Polymaker";
    edit.material = "PETG";
    edit.color_rgb = 0x00FF00u;
    REQUIRE(helix::test::apply_edit(*harness, 0, edit).success());
    CHECK(api.rest_mock().mock_get_post_history().empty());

    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PETG"},
                                                    {"MANUFACTURER", "Polymaker"},
                                                    {"ARGB_COLOR", 0xFF00FF00},
                                                    {"WEIGHT", 1000},
                                                    {"CARD_UID", tag_uid},
                                                }})},
                                   });

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.firmware.has_value());
    CHECK(read.firmware->material == "PETG");
    CHECK(read.firmware->brand == "Polymaker");
    REQUIRE(read.firmware->color_rgb.has_value());
    CHECK(*read.firmware->color_rgb == 0x00FF00u);
    REQUIRE(read.firmware->total_weight_g.has_value());
    CHECK(*read.firmware->total_weight_g == 1000.0F);
}

// ---------------------------------------------------------------------------
// QIDI Box
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "Qidi's saved ids resolve into a vendor cache",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(12, "PETG", 5, "#ED2C2C", 3, "QIDI"));

    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"filament_slot0", 12},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 3}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->material == "PETG");
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0xED2C2Cu);
    CHECK(lane.firmware->brand == "QIDI");
    // Saved variables say what a slot was told it holds, never whether it does.
    CHECK_FALSE(lane.firmware->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a Qidi UNKNOWN state word retracts the standing presence reading",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;
    fire_vars(*harness, nlohmann::json{{"box_count", 1}, {"slot0", 1}});

    const auto standing = lane_sources(harness.lane(0));
    REQUIRE(standing.sensed.has_value());
    REQUIRE(standing.sensed->present.has_value());
    CHECK(*standing.sensed->present == true);

    // 99 is no state word the Box vocabulary knows, so the slot files UNKNOWN.
    fire_vars(*harness, nlohmann::json{{"slot0", 99}});

    const auto declined = lane_sources(harness.lane(0));
    REQUIRE(declined.sensed.has_value());
    CHECK_FALSE(declined.sensed->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Qidi frame silent about a lane leaves its presence standing",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;
    fire_vars(*harness, nlohmann::json{{"box_count", 1}, {"slot0", 1}});

    // enable_box speaks about the boxes and says nothing about any lane, so
    // this frame carries no slot0 key at all.
    fire_vars(*harness, nlohmann::json{{"enable_box", 1}});

    const auto standing = lane_sources(harness.lane(0));
    REQUIRE(standing.sensed.has_value());
    REQUIRE(standing.sensed->present.has_value());
    CHECK(*standing.sensed->present == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi files nothing for an id its tables do not resolve",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(12, "PETG", 5, "#ED2C2C", 3, "QIDI"));

    // Slot 1's ids name rows the Box's tables do not have. That is not a slot
    // with no filament in it, and it is not a slot holding whatever id 12
    // happens to mean. Slot 0 carries resolvable ids in the same frame, so the
    // loop demonstrably ran and reached the right slot.
    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"filament_slot0", 12},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 3},
                                       {"filament_slot1", 77},
                                       {"color_slot1", 88},
                                       {"vendor_slot1", 99}});

    const auto resolved = lane_sources(harness.lane(0));
    REQUIRE(resolved.firmware.has_value());
    CHECK(resolved.firmware->material == "PETG");

    const auto unresolved = lane_sources(harness.lane(1));
    REQUIRE(unresolved.firmware.has_value());
    CHECK_FALSE(unresolved.firmware->material.has_value());
    CHECK_FALSE(unresolved.firmware->color_rgb.has_value());
    CHECK_FALSE(unresolved.firmware->brand.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi's vendor cache states its tables, not the slot it wrote",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(12, "PETG", 5, "#ED2C2C", 3, "QIDI"));

    const nlohmann::json vars{
        {"box_count", 1}, {"filament_slot0", 12}, {"color_slot0", 5}, {"vendor_slot0", 3}};
    fire_vars(*harness, vars);

    // A reload drops every row the ids name. SlotInfo persists across frames
    // and nothing clears it, so the previous frame's values stay on the slot
    // with nothing in the tables behind them.
    QidiBoxTestAccess::apply_filas_list(*harness, filas_list(40, "ABS", 9, "#00FF00", 8, "Elegoo"));
    fire_vars(*harness, vars);

    // Precondition, not the behaviour under test: unless the orphaned values
    // really are still on the slot, a read-back would pass for the wrong reason.
    const auto slot = harness->get_slot_info(0);
    REQUIRE(slot.material == "PETG");
    REQUIRE(slot.color_rgb == 0xED2C2Cu);
    REQUIRE(slot.brand == "QIDI");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.firmware.has_value());
    CHECK_FALSE(lane.firmware->material.has_value());
    CHECK_FALSE(lane.firmware->color_rgb.has_value());
    CHECK_FALSE(lane.firmware->brand.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Qidi vendor change alone is not a spool swap",
                 "[lane][ingest][qidi][1710]") {
    QidiHarness harness;
    QidiBoxTestAccess::apply_filas_list(*harness, qidi_tables());

    const nlohmann::json base{
        {"box_count", 1}, {"filament_slot0", 12}, {"color_slot0", 5}, {"vendor_slot0", 3}};
    fire_vars(*harness, base);

    auto edit = harness->get_slot_info(0);
    edit.material = "PETG Basic";
    edit.color_rgb = 0xED2C2Cu;
    edit.brand = "QIDI";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // Another writer moves only the vendor row. The vendor id is not evidence
    // about the spool - it is the Box's own table, not something the spool
    // carries - so the slot's details are not a description of a departed
    // spool and the override stands; the new vendor repaints the brand.
    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"filament_slot0", 12},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 8}});

    const auto info = harness->get_slot_info(0);
    CHECK(info.material == "PETG");
    CHECK(info.brand == "Elegoo");
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi rows that decode alike are one spool",
                 "[lane][ingest][qidi][1710]") {
    QidiHarness harness;
    ToastRecorder toasts;
    QidiBoxTestAccess::apply_filas_list(*harness, qidi_tables());

    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"filament_slot0", 12},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 3}});

    auto edit = harness->get_slot_info(0);
    edit.material = "PETG Basic";
    edit.color_rgb = 0xED2C2Cu;
    edit.brand = "QIDI";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The fila row changes but both rows are PETG under a different name, and
    // the colour row does not move: what the reader read off the spool decodes
    // to the same material and colour, which is one spool by the insert rule,
    // not a swap. The details stand and nobody is asked.
    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"filament_slot0", 14},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 3}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(harness->get_slot_info(0).material == "PETG");
    CHECK(toasts.messages.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Qidi insert the reader read nothing about asks",
                 "[lane][ingest][qidi][1710]") {
    QidiHarness harness;
    ToastRecorder toasts;
    QidiBoxTestAccess::apply_filas_list(*harness, qidi_tables());

    // The slot sits empty while the saved ids stand in the variables: those
    // are the Box's memory of the last spool, not a read of the slot.
    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"slot0", 0},
                                       {"filament_slot0", 12},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 3}});

    auto edit = harness->get_slot_info(0);
    edit.material = "PETG Basic";
    edit.color_rgb = 0xED2C2Cu;
    edit.brand = "QIDI";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // Filament appears and the ids did not move: the reader read nothing off
    // the spool that went in, so nothing says whether the details standing on
    // the slot describe it. Keep them, and ask.
    fire_vars(*harness, nlohmann::json{{"box_count", 1}, {"slot0", 1}});
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(toasts.messages.size() == 1);
    CHECK(harness->get_slot_info(0).material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Qidi insert whose tag decodes the same asks nothing",
                 "[lane][ingest][qidi][1710]") {
    QidiHarness harness;
    ToastRecorder toasts;
    QidiBoxTestAccess::apply_filas_list(*harness, qidi_tables());

    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"slot0", 0},
                                       {"filament_slot0", 12},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 3}});

    auto edit = harness->get_slot_info(0);
    edit.material = "PETG Basic";
    edit.color_rgb = 0xED2C2Cu;
    edit.brand = "QIDI";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The insert brings a tag the Box reads: both fila rows decode to PETG and
    // the colour is unchanged, so the reading proves the same spool and the
    // ask is not offered.
    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"slot0", 1},
                                       {"filament_slot0", 14},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 3}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(toasts.messages.empty());
    CHECK(harness->get_slot_info(0).material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Qidi fingerprint stored with a vendor component folds to two",
                 "[lane][ingest][qidi][1710]") {
    CHECK(QidiBoxTestAccess::normalize_legacy_fingerprint("12|5|3") == "12|5");
    // Already the current shape, empty, or a value no build of ours wrote:
    // left exactly as found.
    CHECK(QidiBoxTestAccess::normalize_legacy_fingerprint("12|5") == "12|5");
    CHECK(QidiBoxTestAccess::normalize_legacy_fingerprint("").empty());
    CHECK(QidiBoxTestAccess::normalize_legacy_fingerprint("box") == "box");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a Qidi swap the tables have not decoded still clears the override",
                 "[lane][ingest][qidi][1710]") {
    QidiHarness harness;
    ToastRecorder toasts;
    // No apply_filas_list: the officiall_filas_list.cfg fetch has not landed,
    // so no id in the fingerprint decodes to a material or a colour.

    const nlohmann::json base{{"box_count", 1}, {"filament_slot0", 12}, {"color_slot0", 5}};
    fire_vars(*harness, base);

    // An edit needs the tables to name its rows, so the record stands in for
    // one written before the fetch failed.
    helix::ams::FilamentSlotOverride record;
    record.material = "PETG Basic";
    record.color_rgb = 0xED2C2Cu;
    record.color_set = true;
    record.brand = "QIDI";
    QidiBoxTestAccess::seed_override(*harness, 0, record);
    REQUIRE(QidiBoxTestAccess::get_override(*harness, 0).has_value());

    // A different spool's ids arrive with the tables still missing. Both
    // composites fall back to the raw ids, the ids differ, so the change is a
    // swap: the override must not outlive a swap just because a config fetch
    // is late. A differing-ids verdict is an answer, not a question, so the
    // ask is not offered either.
    fire_vars(*harness,
              nlohmann::json{{"box_count", 1}, {"filament_slot0", 40}, {"color_slot0", 9}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK_FALSE(QidiBoxTestAccess::get_override(*harness, 0).has_value());
    CHECK(harness->get_slot_info(0).material != "PETG Basic");
    CHECK(toasts.messages.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi ids that match with no tables keep the override",
                 "[lane][ingest][qidi][1710]") {
    QidiHarness harness;
    ToastRecorder toasts;
    // Tables missing here too: whatever the fallback does, it must not turn a
    // quiet frame into a clearance.
    fire_vars(*harness, nlohmann::json{
                            {"box_count", 1},
                            {"filament_slot0", 12},
                            {"color_slot0", 5},
                            {"vendor_slot0", 3},
                        });

    // An edit needs the tables to name its rows, so the record stands in for
    // one written before the fetch failed.
    helix::ams::FilamentSlotOverride record;
    record.material = "PETG Basic";
    record.color_rgb = 0xED2C2Cu;
    record.color_set = true;
    record.brand = "QIDI";
    QidiBoxTestAccess::seed_override(*harness, 0, record);
    REQUIRE(QidiBoxTestAccess::get_override(*harness, 0).has_value());

    // The fila and colour ids stand; only the vendor row moves. The
    // fingerprint is unchanged and no id the spool carries moved, so the
    // override survives a late or failed table fetch untouched.
    fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                       {"filament_slot0", 12},
                                       {"color_slot0", 5},
                                       {"vendor_slot0", 8}});
    helix::ui::UpdateQueue::instance().drain();

    const auto override_after = QidiBoxTestAccess::get_override(*harness, 0);
    REQUIRE(override_after.has_value());
    CHECK(override_after->material == "PETG Basic");
    CHECK(toasts.messages.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Qidi palette grey is the no-colour sentinel",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;
    QidiBoxTestAccess::apply_filas_list(*harness, "[colordict]\n1 = #808080\n2 = #000000\n");

    fire_vars(*harness, nlohmann::json{{"box_count", 1}, {"color_slot0", 1}, {"color_slot1", 2}});

    // The palette row reaches the slot, because the Box did name a colour id.
    REQUIRE(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);

    const auto grey = lane_sources(harness.lane(0));
    REQUIRE(grey.firmware.has_value());
    CHECK_FALSE(grey.firmware->color_rgb.has_value());

    // Black is a colour, and lands one row away in the same table.
    const auto black = lane_sources(harness.lane(1));
    REQUIRE(black.firmware.has_value());
    REQUIRE(black.firmware->color_rgb.has_value());
    CHECK(*black.firmware->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "Qidi's presence follows the state word, jam included",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;

    // One frame, four state words: empty, spooled, seated, and a negative word
    // the Box uses for a faulted lane.
    fire_vars(
        *harness,
        nlohmann::json{{"box_count", 1}, {"slot0", 0}, {"slot1", 1}, {"slot2", 2}, {"slot3", -1}});

    const auto empty = lane_sources(harness.lane(0));
    REQUIRE(empty.sensed.has_value());
    REQUIRE(empty.sensed->present.has_value());
    CHECK(*empty.sensed->present == false);

    const auto spooled = lane_sources(harness.lane(1));
    REQUIRE(spooled.sensed.has_value());
    REQUIRE(spooled.sensed->present.has_value());
    CHECK(*spooled.sensed->present == true);

    const auto seated = lane_sources(harness.lane(2));
    REQUIRE(seated.sensed.has_value());
    REQUIRE(seated.sensed->present.has_value());
    CHECK(*seated.sensed->present == true);

    // A jam is filament stuck in the path. Answering "no reading" here would
    // retract a live presence record at the moment a lane faults.
    REQUIRE(harness->get_slot_info(3).status == helix::SlotStatus::BLOCKED);
    const auto jammed = lane_sources(harness.lane(3));
    REQUIRE(jammed.sensed.has_value());
    REQUIRE(jammed.sensed->present.has_value());
    CHECK(*jammed.sensed->present == true);

    // The state word says nothing about which spool is in the lane.
    CHECK_FALSE(spooled.sensed->material.has_value());
    CHECK_FALSE(spooled.sensed->color_rgb.has_value());
    CHECK_FALSE(spooled.sensed->brand.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a colordict row is read by the tree's hex grammar",
                 "[lane][ingest][qidi]") {
    QidiHarness harness;
    QidiBoxTestAccess::apply_filas_list(*harness, "[colordict]\n"
                                                  "1 = #ED2C2C\n"
                                                  "2 = F00\n"
                                                  "3 = #11223344\n"
                                                  "4 = 0x00FF00\n"
                                                  "5 = not a colour\n"
                                                  "6 =\n");

    REQUIRE(QidiBoxTestAccess::get_color(*harness, 1).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 1) == 0xED2C2Cu);
    REQUIRE(QidiBoxTestAccess::get_color(*harness, 2).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 2) == 0xFF0000u);
    // #RRGGBBAA drops the alpha byte, as it does for every other consumer.
    REQUIRE(QidiBoxTestAccess::get_color(*harness, 3).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 3) == 0x112233u);
    REQUIRE(QidiBoxTestAccess::get_color(*harness, 4).has_value());
    CHECK(*QidiBoxTestAccess::get_color(*harness, 4) == 0x00FF00u);
    // A row that states no colour is not an id to resolve against.
    CHECK_FALSE(QidiBoxTestAccess::get_color(*harness, 5).has_value());
    CHECK_FALSE(QidiBoxTestAccess::get_color(*harness, 6).has_value());
    CHECK(QidiBoxTestAccess::color_count(*harness) == 4);
}

TEST_CASE_METHOD(LVGLTestFixture, "what the colordict admits decides the write-back id",
                 "[lane][ingest][qidi]") {
    // The stock file's own shape: every row a six-digit #RRGGBB, which is what
    // both records of officiall_filas_list.cfg in this tree contain. Widening
    // the parse admits nothing extra from a file like this.
    const std::string stock = "[colordict]\n1 = #FAFAFA\n2 = #060606\n";

    // Plain backends, not harnesses: this case asks what the palette admits and
    // what resolve_color_id then picks, neither of which needs a lane id, and
    // registering a second harness would clear the first out from under it.
    AmsBackendQidi narrow(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(narrow, stock);
    REQUIRE(QidiBoxTestAccess::color_count(narrow) == 2);

    // A row in one of the newly-admitted spellings. It is a palette MEMBER, so
    // it is also a nearest-match target: resolve_color_id scans the whole map,
    // and apply_user_edit writes back the id it picks.
    AmsBackendQidi widened(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(widened, stock + "3 = F11\n");
    REQUIRE(QidiBoxTestAccess::color_count(widened) == 3);
    REQUIRE(QidiBoxTestAccess::get_color(widened, 3).has_value());
    CHECK(*QidiBoxTestAccess::get_color(widened, 3) == 0xFF1111u);

    const auto palette_of = [](const AmsBackendQidi& box, std::initializer_list<int> ids) {
        std::map<int, uint32_t> out;
        for (int id : ids) {
            if (auto rgb = QidiBoxTestAccess::get_color(box, id)) {
                out[id] = *rgb;
            }
        }
        return out;
    };

    // A near-red the stock palette has no good answer for: black is merely the
    // less wrong of two bad matches.
    constexpr uint32_t kNearRed = 0xEE1111u;
    CHECK(QidiBoxTestAccess::resolve_color_id(palette_of(narrow, {1, 2, 3}), kNearRed) == 2);
    CHECK(QidiBoxTestAccess::resolve_color_id(palette_of(widened, {1, 2, 3}), kNearRed) == 3);
}

// ---------------------------------------------------------------------------
// Mock
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "the mock's simulated population becomes lane readings",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);
    REQUIRE(harness->start().success());

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->material == "PLA");
    REQUIRE(lane.firmware->color_rgb.has_value());
    CHECK(*lane.firmware->color_rgb == 0x1A1A2Eu);

    // The simulated firmware holds the identity fields a write gives it, the
    // way AFC's lane object does, so its reading is also the echo of an edit.
    const auto slot = harness->get_slot_info(0);
    REQUIRE(slot.color_name == "Jet Black");
    REQUIRE(slot.spoolman_id == 1);
    CHECK(lane.firmware->spoolman_id == std::optional<int>(1));
    // Firmware reports a colour as hex, never as a name.
    CHECK_FALSE(lane.firmware->color_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the mock's UNKNOWN status declines to state presence",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);
    REQUIRE(harness->start().success());

    const auto standing = lane_sources(harness.lane(0));
    REQUIRE(standing.sensed.has_value());
    REQUIRE(standing.sensed->present.has_value());
    CHECK(*standing.sensed->present == true);

    // Re-publishing the population after forcing a status is what the next
    // simulated frame would do.
    harness->force_slot_status(0, helix::SlotStatus::UNKNOWN);
    AmsBackendMockTimingTestAccess::publish_lane_observations(*harness);

    const auto declined = lane_sources(harness.lane(0));
    REQUIRE(declined.sensed.has_value());
    CHECK_FALSE(declined.sensed->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the mock's colour sentinel is not a grey anybody chose",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);

    // The edit writes the slot, which is how a lane gets staged before the
    // population is published.
    auto black = harness->get_slot_info(0);
    black.color_rgb = 0x000000u;
    black.material = "Black-PLA";
    REQUIRE(helix::test::apply_edit(*harness, 0, black).success());

    auto colourless = harness->get_slot_info(1);
    colourless.color_rgb = helix::kAmsDefaultSlotColor;
    colourless.material = "Grey-PLA";
    REQUIRE(helix::test::apply_edit(*harness, 1, colourless).success());

    REQUIRE(harness->start().success());

    const auto real_black = lane_sources(harness.lane(0));
    REQUIRE(real_black.firmware.has_value());
    REQUIRE(real_black.firmware->color_rgb.has_value());
    CHECK(*real_black.firmware->color_rgb == 0x000000u);

    // Same loop, same publish: the material still lands, so the missing colour
    // is the sentinel rule and not a lane the loop skipped.
    const auto sentinel = lane_sources(harness.lane(1));
    REQUIRE(sentinel.firmware.has_value());
    CHECK(sentinel.firmware->material == "Grey-PLA");
    CHECK_FALSE(sentinel.firmware->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the factory mock is registered before it starts",
                 "[lane][ingest][mock]") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;
    get_runtime_config()->use_real_ams = false;
    get_runtime_config()->disable_mock_ams = false;

    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    // AmsState::init_subjects observes PrinterState's print-state subject; it
    // must exist first or the observer attaches to nothing.
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);

    REQUIRE(ams.backend_count() == 1);

    // A backend answers kInvalidLaneId for every slot until registration
    // stamps its index, so a population published before that lands on no lane
    // and every record here reads empty.
    const auto lane = lane_sources(helix::ams::lane_id_for(0, 0));
    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->material == "PLA");
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    ams.clear_backends();
    helix::ui::UpdateQueue::instance().drain();
    ams.deinit_subjects();
}

// --- The shared namespace, re-read ----------------------------------------
//
// A resync goes back to lane_data and re-reads it. What it FILES is bounded
// twice over.
//
// By source: only records classifying as VendorCache. A record naming a spool
// is the server's statement and one carrying a lock key is a person's, and
// neither becomes true again merely because a re-read saw it. A record this
// application did not write is another tool's statement and may displace the
// lane's own (#1632) - unless a write of ours is still in flight on the slot,
// where the guard's strip alone decides what files.
//
// By lane: only where firmware states no identity of its own. Everywhere else
// a status frame already files the vendor-cache record, ingest() replaces a
// source's record whole, and both arrival orders happen, so a second producer
// there overwrites a fresh reading with a stored one. Nor is a field gained by
// allowing it: the next frame retracts whatever the stored record carried
// beyond what firmware reports. klipper-toolchanger is the one backend whose
// firmware reports no identity, so it is the one that files.

namespace {

/// The Moonraker a record store reads through, with the records a case seeds.
/// A store reaches the database directly, so a harness built with no API of
/// its own still gets a namespace to re-read.
struct LaneDataDb {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ::get_printer_state()};

    /// One lane_data record under @p key.
    void seed(const std::string& key, nlohmann::json record) {
        client.mock_db_set("lane_data", key, std::move(record));
    }

    /// A store on the shared namespace, in the key style @p style spells.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> store(const std::string& backend_id,
                                                                 helix::ams::LaneKeyStyle style) {
        return std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, backend_id, style);
    }
};

/// A tool changer keys its records T<n>; the slot a record names is the inner
/// 0-based field in either style.
std::unique_ptr<helix::ams::FilamentSlotOverrideStore> toolchanger_store(LaneDataDb& db) {
    return db.store("toolchanger", helix::ams::LaneKeyStyle::Tool);
}

/// A tool changer held in the state a resync has to speak up about: started
/// (so a lane-record store is owed) but with none behind it. start() is final
/// on AmsSubscriptionBackend and drags in subscription setup, so the flag
/// start() would set is set directly.
class StartedStorelessToolChanger : public AmsBackendToolChanger {
  public:
    using AmsBackendToolChanger::AmsBackendToolChanger;

    void mark_running_for_test() {
        running_ = true;
    }
};

} // namespace

// --- The one backend that files -------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "a second backend paints its own lane, not backend 0's",
                 "[lane][ingest][snapmaker]") {
    // Both backends' slot 0 hold different identities, so a backend that read
    // its lane from the slot index alone would paint backend 0's (#1643).
    MockHarness filler(4);
    auto owned = std::make_unique<helix::test::WiredBackend<AmsBackendSnapmaker>>();
    auto* backend = owned.get();
    REQUIRE(helix::AmsState::instance().add_backend(std::move(owned)) == 1);
    REQUIRE(backend->start().success());

    helix::ams::Observation other(helix::ams::ObservationSource::Pending);
    other.material = "ABS";
    helix::ams::file_pending(helix::ams::lane_id_for(0, 0), other);

    feed_filament_detect(*backend, nlohmann::json{
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PETG"},
                                                    {"ARGB_COLOR", 0xFFED2C2C},
                                                }})},
                                   });

    CHECK(backend->get_slot_info(0).material == "PETG");
}

// --- The six that do not ---------------------------------------------------

// The no-op above is only correct while the missing store is expected. On a
// backend that is live and reachable, firmware stating no identity of its own
// makes the persisted record the producer, and naming no store to read it
// from means the resync the caller asked for cannot be issued. The three
// cases below are one contract: the first proves the branch and its needle
// are live, the other two pin the two states where a missing store is
// expected and must stay quiet.

// ============================================================================
// A lane written from outside a backend reaches its cached slot on request
// (prestonbrown/helixscreen#1653)
// ============================================================================

namespace {

constexpr uint32_t kFiledColor = 0x1A1A2E;

/// Files a pending colour on slot 0's lane with no frame after it. The slot
/// the backend serves keeps what its last frame painted until the backend is
/// asked to repaint, and then shows the filing.
void expect_repaint_shows_filing(helix::AmsBackend& backend, helix::ams::LaneId lane,
                                 uint32_t frame_color) {
    const helix::SlotInfo painted = backend.get_slot_info(0);
    REQUIRE(painted.color_rgb == frame_color);

    helix::ams::Observation filed(helix::ams::ObservationSource::Pending);
    filed.color_rgb = kFiledColor;
    helix::ams::file_pending(lane, filed);
    REQUIRE(backend.get_slot_info(0).color_rgb == frame_color);

    backend.repaint_slot_from_lane(0);

    CHECK(backend.get_slot_info(0).color_rgb == kFiledColor);
    // A filing states no presence, so the status the frame derived stands.
    CHECK(backend.get_slot_info(0).status == painted.status);
}

struct CachedSlotBackend {
    const char* name;
    void (*run)();
};

/// Every backend that paints a stored SlotInfo from the lane only while it
/// parses a frame. ACE is not here: it re-reads its slots on a short poll.
const CachedSlotBackend kCachedSlotBackends[] = {
    {"AD5X IFS",
     [] {
         Ad5xHarness harness(nullptr, nullptr);
         Ad5xIfsTestAccess::set_port_presence(*harness, 0, true);
         Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");
         expect_repaint_shows_filing(*harness, harness.lane(0), 0xED2C2C);
     }},
    {"AFC",
     [] {
         AfcHarness harness(nullptr, nullptr);
         init_afc_lanes(*harness);
         feed_afc_lane(*harness, "lane1",
                       {{"prep", true},
                        {"load", true},
                        {"status", "Loaded"},
                        {"color", "#ED2C2C"},
                        {"material", "PETG"}});
         expect_repaint_shows_filing(*harness, harness.lane(0), 0xED2C2C);
     }},
    {"CFS",
     [] {
         CfsHarness harness;
         feed_cfs_box(*harness,
                      flat_box(nlohmann::json::array({nlohmann::json{{"index", 0},
                                                                     {"material", "PLA"},
                                                                     {"color", "#ED2C2C"},
                                                                     {"present", true},
                                                                     {"loaded", false}}})));
         expect_repaint_shows_filing(*harness, harness.lane(0), 0xED2C2C);
     }},
    {"Happy Hare",
     [] {
         HappyHareHarness harness(nullptr, nullptr);
         feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                             {"gate_color", nlohmann::json::array({"ed2c2c"})},
                             {"gate_material", nlohmann::json::array({"PETG"})}});
         expect_repaint_shows_filing(*harness, harness.lane(0), 0xED2C2C);
     }},
    {"Snapmaker",
     [] {
         SnapmakerHarness harness;
         snapmaker_idle(*harness); // an edit writes print_task_config only once a state is read
         feed_filament_detect(
             *harness, nlohmann::json{
                           {"state", nlohmann::json::array({1, 0, 0, 0})},
                           {"info", nlohmann::json::array({nlohmann::json{
                                        {"MAIN_TYPE", "PLA"},
                                        {"ARGB_COLOR", 0xFFED2C2C},
                                        {"CARD_UID", nlohmann::json::array({144, 32, 196, 2})},
                                    }})},
                       });
         expect_repaint_shows_filing(*harness, harness.lane(0), 0xED2C2C);
     }},
    {"ToolChanger",
     [] {
         ToolChangerHarness harness(nullptr, nullptr);
         harness->set_discovered_tools({"T0", "T1"});
         feed_toolchanger(
             *harness, nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});
         expect_repaint_shows_filing(*harness, harness.lane(0), helix::kAmsDefaultSlotColor);
     }},
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a backend serving its slots from a cache repaints one from its lane on request",
                 "[lane][1653]") {
    for (const auto& backend : kCachedSlotBackends) {
        DYNAMIC_SECTION(backend.name) {
            backend.run();
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a tool changer frame paints a lane whose only identity is its Spoolman record",
                 "[lane][toolchanger][1653]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1"});
    REQUIRE_FALSE(ToolChangerTestAccess::has_overrides(*harness));

    // The lane's record links spool 7, which is what lets its details fill.
    helix::ams::Observation record(helix::ams::ObservationSource::Record);
    record.spoolman_id = 7;
    helix::ams::ingest(harness.lane(0), record);
    helix::ams::Observation filed(helix::ams::ObservationSource::Spoolman);
    filed.spoolman_id = 7;
    filed.color_rgb = kFiledColor;
    helix::ams::ingest(harness.lane(0), filed);
    REQUIRE(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);

    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    CHECK(harness->get_slot_info(0).color_rgb == kFiledColor);
}

// Clear Spool hands the lane back to the machine. The spool name goes with the
// user's record, and what stands afterwards is the product line the tag itself
// states, or nothing.
TEST_CASE_METHOD(LVGLTestFixture, "clearing a Snapmaker spool restates the tag's product line",
                 "[lane][ingest][snapmaker][spool_name]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    SnapmakerHarness harness(&api);
    snapmaker_idle(*harness); // an edit writes print_task_config only once a state is read

    SECTION("an untagged spool keeps no name") {
        feed_filament_detect(*harness, nlohmann::json{
                                           {"state", nlohmann::json::array({1})},
                                           {"info", nlohmann::json::array({nlohmann::json{
                                                        {"MAIN_TYPE", "NONE"},
                                                        {"SUB_TYPE", "NONE"},
                                                    }})},
                                       });
        auto edit = harness->get_slot_info(0);
        edit.brand = "Generic";
        edit.material = "ASA";
        edit.spool_name = "SnapSpeed";
        helix::test::edit_slot_as_user(*harness, 0, edit);
        REQUIRE(harness->get_slot_info(0).spool_name == "SnapSpeed");

        harness->clear_slot_override(0);
        CHECK(harness->get_slot_info(0).spool_name.empty());
    }

    SECTION("a tagged spool gets the tag's line back") {
        feed_filament_detect(*harness,
                             nlohmann::json{
                                 {"state", nlohmann::json::array({1})},
                                 {"info", nlohmann::json::array({nlohmann::json{
                                              {"MAIN_TYPE", "PLA"},
                                              {"MANUFACTURER", "Snapmaker"},
                                              {"SUB_TYPE", "Silk"},
                                              {"CARD_UID", nlohmann::json::array({1, 2, 3, 4})},
                                          }})},
                             });
        auto edit = harness->get_slot_info(0);
        edit.spool_name = "Bench reel";
        helix::test::edit_slot_as_user(*harness, 0, edit);
        REQUIRE(harness->get_slot_info(0).spool_name == "Bench reel");

        harness->clear_slot_override(0);
        CHECK(harness->get_slot_info(0).spool_name == "Silk");
    }
}

// ============================================================================
// Tag readings: what each tag-reading backend keeps of the tag under an edit
// ============================================================================

using helix::ams::TagField;

TEST_CASE_METHOD(LVGLTestFixture, "a Snapmaker card's reading outlives a user's colour",
                 "[lane][snapmaker][tag_reading]") {
    SnapmakerHarness harness;
    snapmaker_idle(*harness); // an edit writes print_task_config only once a state is read
    const nlohmann::json card{
        {"state", nlohmann::json::array({1})},
        {"info", nlohmann::json::array({nlohmann::json{
                     {"MAIN_TYPE", "PLA"},
                     {"MANUFACTURER", "Snapmaker"},
                     {"SUB_TYPE", "Matte"},
                     {"ARGB_COLOR", 0xFFED2C2C},
                     {"HOTEND_MIN_TEMP", 190},
                     {"HOTEND_MAX_TEMP", 220},
                     {"BED_TEMP", 60},
                     {"WEIGHT", 1000},
                     {"COLOR_NUMS", 2},
                     {"RGB_1", 0xED2C2C},
                     {"RGB_2", 0x00FF00},
                     {"CARD_UID", nlohmann::json::array({144, 32, 196, 2})},
                 }})},
    };
    feed_filament_detect(*harness, card);

    const auto tag = harness->tag_reading(0);
    REQUIRE(tag.has_value());
    CHECK(tag->material == "PLA");
    CHECK(tag->brand == "Snapmaker");
    CHECK(tag->product_name == "Matte");
    CHECK(tag->color_rgb == std::optional<uint32_t>(0xED2C2Cu));
    CHECK(tag->multi_color_hexes == "#ED2C2C,#00FF00");
    CHECK(tag->nozzle_temp_min == std::optional<int>(190));
    CHECK(tag->nozzle_temp_max == std::optional<int>(220));
    CHECK(tag->bed_temp == std::optional<int>(60));
    CHECK(tag->total_weight_g == std::optional<float>(1000.0F));
    CHECK(harness->get_slot_info(0).multi_color_hexes == "#ED2C2C,#00FF00");
    CHECK(harness->tag_provenance(0).from_tag(TagField::Color));
    CHECK(harness->tag_provenance(0).from_tag(TagField::Material));

    // Picking one colour replaces the multi-colour, and the tag's next read
    // leaves the pick standing.
    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x123456u;
    edit.multi_color_hexes.clear();
    helix::test::edit_slot_as_user(*harness, 0, edit);
    feed_filament_detect(*harness, card);

    const auto shown = harness->get_slot_info(0);
    CHECK(shown.color_rgb == 0x123456u);
    CHECK(shown.multi_color_hexes.empty());
    REQUIRE(harness->tag_reading(0).has_value());
    CHECK(harness->tag_reading(0)->color_rgb == std::optional<uint32_t>(0xED2C2Cu));
    const auto p = harness->tag_provenance(0);
    CHECK_FALSE(p.from_tag(TagField::Color));
    CHECK(p.from_tag(TagField::Material));

    const auto reset = helix::ams::reset_to_tag(shown, *harness->tag_reading(0));
    CHECK(reset.color_rgb == 0xED2C2Cu);
    CHECK(reset.multi_color_hexes == "#ED2C2C,#00FF00");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Snapmaker entry with no card behind it is no tag reading",
                 "[lane][snapmaker][tag_reading]") {
    SnapmakerHarness harness;
    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "PLA"},
                                                    {"VENDOR", "Generic"},
                                                    {"ARGB_COLOR", 0xFFED2C2C},
                                                    {"CARD_UID", 0},
                                                }})},
                                   });
    CHECK_FALSE(harness->tag_reading(0).has_value());
    CHECK_FALSE(harness->tag_provenance(0).any());
}

TEST_CASE_METHOD(LVGLTestFixture, "a CFS bay's tag reading outlives a user's edit",
                 "[lane][cfs][tag_reading]") {
    CfsHarness harness;
    const nlohmann::json box = flat_box(nlohmann::json::array({nlohmann::json{{"index", 0},
                                                                              {"material", "PETG"},
                                                                              {"brand", "Creality"},
                                                                              {"color", "#ED2C2C"},
                                                                              {"present", true}}}));
    feed_cfs_box(*harness, box);

    REQUIRE(harness->tag_reading(0).has_value());
    CHECK(harness->tag_reading(0)->material == "PETG");
    CHECK(harness->tag_provenance(0).from_tag(TagField::Brand));

    auto edit = harness->get_slot_info(0);
    edit.material = "ABS";
    edit.color_rgb = 0x00FF00u;
    helix::test::edit_slot_as_user(*harness, 0, edit);
    feed_cfs_box(*harness, box);

    const auto shown = harness->get_slot_info(0);
    REQUIRE(shown.material == "ABS");
    REQUIRE(shown.color_rgb == 0x00FF00u);
    const auto tag = harness->tag_reading(0);
    REQUIRE(tag.has_value());
    CHECK(tag->material == "PETG");
    CHECK(tag->color_rgb == std::optional<uint32_t>(0xED2C2Cu));
    const auto p = harness->tag_provenance(0);
    CHECK_FALSE(p.from_tag(TagField::Material));
    CHECK_FALSE(p.from_tag(TagField::Color));
    CHECK(p.from_tag(TagField::Brand));

    const auto reset = helix::ams::reset_to_tag(shown, *tag);
    CHECK(reset.material == "PETG");
    CHECK(reset.color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE keeps only an identified read as the tag's",
                 "[lane][ace][tag_reading]") {
    AceHarness harness(nullptr, nullptr);
    const auto frame = [](const nlohmann::json& rfid) {
        return nlohmann::json{
            {"slots", nlohmann::json::array({nlohmann::json{
                          {"status", "ready"},
                          {"color", nlohmann::json::array({237, 44, 44})},
                          {"type", "PETG"},
                          {"temp_min", 230},
                          {"temp_max", 250},
                          {"rfid", rfid},
                      }})},
        };
    };

    SECTION("an identified read is kept and outlives a user's edit") {
        feed_ace(*harness, frame(2));
        const auto tag = harness->tag_reading(0);
        REQUIRE(tag.has_value());
        CHECK(tag->material == "PETG");
        CHECK(tag->color_rgb == std::optional<uint32_t>(0xED2C2Cu));
        CHECK(tag->nozzle_temp_max == std::optional<int>(250));

        auto edit = harness->get_slot_info(0);
        edit.color_rgb = 0x00FF00u;
        helix::test::edit_slot_as_user(*harness, 0, edit);
        feed_ace(*harness, frame(2));
        CHECK(harness->get_slot_info(0).color_rgb == 0x00FF00u);
        CHECK(harness->tag_reading(0)->color_rgb == std::optional<uint32_t>(0xED2C2Cu));
        CHECK_FALSE(harness->tag_provenance(0).from_tag(TagField::Color));
        CHECK(harness->tag_provenance(0).from_tag(TagField::Material));

        // A finished read that found no tag ends it.
        feed_ace(*harness, frame(1));
        CHECK_FALSE(harness->tag_reading(0).has_value());
    }
    SECTION("the hub's own memory is no tag") {
        feed_ace(*harness, frame(false));
        CHECK_FALSE(harness->tag_reading(0).has_value());
        feed_ace(*harness, frame(0));
        CHECK_FALSE(harness->tag_reading(0).has_value());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE's REST bridge leaves a user's edit standing",
                 "[lane][ace][tag_reading]") {
    AceHarness harness(nullptr, nullptr);
    const nlohmann::json slots{
        {"slots", nlohmann::json::array({nlohmann::json{{"status", "ready"},
                                                        {"color", "#ED2C2C"},
                                                        {"material", "PETG"},
                                                        {"temp_min", 230},
                                                        {"temp_max", 250},
                                                        {"rfid", 2}}})},
    };
    AceTestAccess::parse_slots(*harness, slots);
    REQUIRE(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);
    REQUIRE(harness->get_slot_info(0).nozzle_temp_max == 250);

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    edit.nozzle_temp_min = 240;
    edit.nozzle_temp_max = 260;
    helix::test::edit_slot_as_user(*harness, 0, edit);
    AceTestAccess::parse_slots(*harness, slots);

    const auto shown = harness->get_slot_info(0);
    CHECK(shown.color_rgb == 0x00FF00u);
    CHECK(shown.material == "ABS");
    CHECK(shown.nozzle_temp_min == 240);
    CHECK(shown.nozzle_temp_max == 260);
    REQUIRE(harness->tag_reading(0).has_value());
    CHECK(harness->tag_reading(0)->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE's REST bridge keeps the hub's temps under a colour edit",
                 "[lane][ace][tag_reading]") {
    AceHarness harness(nullptr, nullptr);
    const auto slots = [](int temp_min, int temp_max) {
        return nlohmann::json{
            {"slots", nlohmann::json::array({nlohmann::json{{"status", "ready"},
                                                            {"color", "#ED2C2C"},
                                                            {"material", "PETG"},
                                                            {"temp_min", temp_min},
                                                            {"temp_max", temp_max}}})},
        };
    };

    SECTION("a colour-only edit leaves the temperatures the hub's") {
        AceTestAccess::parse_slots(*harness, slots(230, 250));
        auto edit = harness->get_slot_info(0);
        edit.color_rgb = 0x00FF00u;
        helix::test::edit_slot_as_user(*harness, 0, edit);
        AceTestAccess::parse_slots(*harness, slots(235, 255));
        CHECK(harness->get_slot_info(0).nozzle_temp_min == 235);
        CHECK(harness->get_slot_info(0).nozzle_temp_max == 255);
    }
    SECTION("a record loaded at a restart leaves the temperatures the hub's") {
        // The stored record keeps a nozzle temperature for every edit, so it
        // is no declaration of one.
        helix::ams::FilamentSlotOverride ovr;
        ovr.material = "PETG";
        ovr.color_rgb = 0x00FF00u;
        ovr.color_set = true;
        ovr.nozzle_temp = 240;
        AceTestAccess::seed_override(*harness, 0, ovr);
        AceTestAccess::parse_slots(*harness, slots(235, 255));
        CHECK(harness->get_slot_info(0).nozzle_temp_min == 235);
        CHECK(harness->get_slot_info(0).nozzle_temp_max == 255);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a restart forgets the box's tag readings",
                 "[lane][cfs][qidi][tag_reading]") {
    SECTION("CFS") {
        CfsHarness harness;
        feed_cfs_box(
            *harness,
            flat_box(nlohmann::json::array({nlohmann::json{
                {"index", 0}, {"material", "PETG"}, {"color", "#ED2C2C"}, {"present", true}}})));
        REQUIRE(harness->tag_reading(0).has_value());
        CfsTestAccess::call_on_started(*harness);
        CHECK_FALSE(harness->tag_reading(0).has_value());
    }
    SECTION("QIDI") {
        QidiHarness harness;
        QidiBoxTestAccess::apply_filas_list(*harness, qidi_tables());
        fire_vars(*harness, nlohmann::json{{"box_count", 1},
                                           {"filament_slot0", 40},
                                           {"color_slot0", 9},
                                           {"vendor_slot0", 8}});
        REQUIRE(harness->tag_reading(0).has_value());
        QidiBoxTestAccess::start_load(*harness);
        CHECK_FALSE(harness->tag_reading(0).has_value());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a Snapmaker tag leaving takes its multi-colour with it",
                 "[lane][snapmaker][tag_reading]") {
    SnapmakerHarness harness;
    feed_filament_detect(*harness,
                         nlohmann::json{
                             {"state", nlohmann::json::array({1})},
                             {"info", nlohmann::json::array({nlohmann::json{
                                          {"MAIN_TYPE", "PLA"},
                                          {"ARGB_COLOR", 0xFFED2C2C},
                                          {"COLOR_NUMS", 2},
                                          {"RGB_1", 0xED2C2C},
                                          {"RGB_2", 0x00FF00},
                                          {"CARD_UID", nlohmann::json::array({1, 2, 3, 4})},
                                      }})},
                         });
    REQUIRE(harness->get_slot_info(0).multi_color_hexes == "#ED2C2C,#00FF00");

    feed_filament_detect(*harness, nlohmann::json{
                                       {"state", nlohmann::json::array({1})},
                                       {"info", nlohmann::json::array({nlohmann::json{
                                                    {"MAIN_TYPE", "NONE"},
                                                    {"CARD_UID", 0},
                                                }})},
                                   });
    CHECK(harness->get_slot_info(0).multi_color_hexes.empty());
    CHECK_FALSE(harness->tag_reading(0).has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's colour scan is the lane's tag reading",
                 "[lane][afc][tag_reading]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);
    feed_afc_lane(
        *harness, "lane1",
        nlohmann::json{{"color", "#FFFFFF"}, {"material", "PLA"}, {"td1_color", "FF8800"}});

    const auto tag = harness->tag_reading(0);
    REQUIRE(tag.has_value());
    CHECK(tag->color_rgb == std::optional<uint32_t>(0xFF8800u));
    CHECK_FALSE(tag->material.has_value());
    CHECK_FALSE(harness->tag_provenance(0).from_tag(TagField::Color));

    const auto reset = helix::ams::reset_to_tag(harness->get_slot_info(0), *tag);
    CHECK(reset.color_rgb == 0xFF8800u);
    CHECK(reset.material == "PLA");

    CHECK_FALSE(harness->tag_reading(1).has_value());
}
