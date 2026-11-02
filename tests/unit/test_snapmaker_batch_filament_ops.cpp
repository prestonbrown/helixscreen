// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for batch filament load/unload on parallel-toolhead printers.
//
// AmsBackendSnapmaker::batch_feed_gcode is the pure script builder: one
// multi-line AUTO_FEEDING script covering every requested head, in the order
// given. The firmware's FEED_AUTO serializes channels itself (a single
// process-wide channel_active gate), and Moonraker holds the script response
// until the whole script has run, so one script IS the sequencer.
//
// The batch rides AmsSubscriptionBackend::run_filament_op like the per-slot
// ops, so it inherits the claim (exactly one filament op in flight, batch or
// not) and the print-active refusal. The gate tests here pin that inheritance.

#include "ui_ams_sidebar.h"
#include "ui_batch_filament_modal.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_snapmaker.h"
#include "ams_error.h"
#include "ams_types.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/ams_sidebar_xml.h"
#include "test_helpers/registered_backend.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;

namespace {

using namespace std::chrono_literals;

/// A subscription backend that has not opted into batch ops, with the started
/// half of the gate satisfied so the refusal under test is the batch one.
class BatchlessBackend : public helix::AmsBackendHappyHare {
  public:
    BatchlessBackend() : helix::AmsBackendHappyHare(nullptr, nullptr) {
        running_.store(true);
    }
};

/// Parks its do_load_filament entrant until released, so a test can observe a
/// second op against a filament-op claim that is genuinely held. The 5s escape
/// hatch keeps a regression from wedging the suite.
class ClaimParkingBackend : public helix::AmsBackendSnapmaker {
  public:
    ClaimParkingBackend() : helix::AmsBackendSnapmaker(nullptr, nullptr) {
        running_.store(true);
    }

    void wait_for_parked_hook() {
        std::unique_lock<std::mutex> lock(m_);
        REQUIRE(cv_.wait_for(lock, 5s, [this] { return entered_; }));
    }

    void release_hook() {
        std::lock_guard<std::mutex> lock(m_);
        release_ = true;
        cv_.notify_all();
    }

  protected:
    AmsError do_load_filament(int /*slot_index*/) override {
        std::unique_lock<std::mutex> lock(m_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait_for(lock, 5s, [this] { return release_; });
        return AmsErrorHelper::success();
    }

  private:
    std::mutex m_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool release_ = false;
};

/// The mock plumbing every batch test drives: a Voron client/API pair plus
/// the per-channel feeder feeding and the gcode-history reads. Mixed into the
/// fixture that supplies the LVGL level, so the picker-modal tests (full XML
/// registration) and the backend tests (bare LVGL) share one setup.
struct BatchMockHarness {
    BatchMockHarness() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
    }

    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }

    /// A started, idle Snapmaker backend through the production factory, with
    /// the gcode history cleared — the state the batch is measured in.
    std::unique_ptr<helix::AmsBackend> snapmaker() {
        auto backend =
            helix::AmsBackend::create(helix::AmsType::SNAPMAKER, api.get(), &mock_client);
        REQUIRE(backend != nullptr);
        REQUIRE(backend->start().success());
        REQUIRE(backend->get_current_action() == helix::AmsAction::IDLE);
        mock_client.clear_gcode_script_history();
        return backend;
    }

    /// Drain UpdateQueue before reading history: several dispatch paths hop to
    /// the main thread before their send, and without draining "nothing was
    /// sent" could just mean "not sent yet".
    const std::vector<std::string>& sent_gcodes() {
        helix::ui::UpdateQueue::instance().drain();
        return mock_client.gcode_script_history();
    }

    /// A registered Snapmaker backend with the concrete type visible, for
    /// tests that reach what only AmsBackendSnapmaker exposes. Registered
    /// through AmsState so lane funnels accept its lane ids, and handed the
    /// mock client/api pair so a dispatch through it records its script.
    helix::AmsBackendSnapmaker& backend() {
        if (!raw_backend_) {
            raw_backend_ =
                std::make_unique<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>>(
                    api.get(), &mock_client);
        }
        return **raw_backend_;
    }

    /// Feed one raw JSON status frame to the backend's status handler — the
    /// same entry point the WebSocket drives.
    void feed_status(const std::string& json_text) {
        helix::SnapmakerTestAccess::handle_status(backend(), nlohmann::json::parse(json_text));
    }

    /// One frame carrying the feeder channel and its motion sensor — the two
    /// objects slot_op_eligibility answers from. The sensor reads enabled, as
    /// it does on the rig; the SensorDisabled section re-feeds it false.
    void set_channel(int slot, const char* state, const char* error, bool detected, bool module,
                     bool no_auto) {
        nlohmann::json frame = {
            {"filament_feed " + std::string(slot < 2 ? "left" : "right"),
             {{"extruder" + std::to_string(slot),
               {{"channel_state", state},
                {"channel_error", error},
                {"filament_detected", detected},
                {"module_exist", module},
                {"disable_auto", no_auto}}}}},
            {"filament_motion_sensor e" + std::to_string(slot) + "_filament", {{"enabled", true}}},
        };
        feed_status(frame.dump());
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::test::RegisteredBackend<helix::AmsBackendSnapmaker>> raw_backend_;
};

struct BatchFixture : public LVGLTestFixture, public BatchMockHarness {};

/// Full production XML registration (LVGLUITestFixture registers every
/// component), so BatchFilamentModal::show_owned() builds its picker from
/// batch_filament_modal.xml on the same mock plumbing.
struct BatchModalFixture : public LVGLUITestFixture, public BatchMockHarness {
    /// Close the picker and let its ModalStack entry free the one-shot
    /// instance: the entry frees a tick after the close animation, so pump
    /// the clock before the next show_owned().
    void close_picker(lv_obj_t* dialog) {
        Modal::hide(dialog);
        process_lvgl(200);
    }
};

/// The picker's dialog on the modal stack, or fail naming what is missing.
lv_obj_t* picker_dialog() {
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    return dialog;
}

/// Whether row @p key's checkbox is ticked. Rows are `item_<key>` with a
/// child checkbox named `check` (UiMultiselect's shape).
bool row_checked(lv_obj_t* dialog, const char* key) {
    lv_obj_t* row = lv_obj_find_by_name(dialog, ("item_" + std::string(key)).c_str());
    REQUIRE(row != nullptr);
    lv_obj_t* check = lv_obj_find_by_name(row, "check");
    REQUIRE(check != nullptr);
    return lv_obj_has_state(check, LV_STATE_CHECKED);
}

/// The caption showing on @p button: its first label child (ui_button's shape).
std::string button_caption(lv_obj_t* dialog, const char* button) {
    lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
    REQUIRE(btn != nullptr);
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class)) {
            return lv_label_get_text(child);
        }
    }
    FAIL("no label inside " << button);
    return {};
}

void click(lv_obj_t* dialog, const char* button) {
    lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
}

} // namespace

// ============================================================================
// batch_feed_gcode — the pure script builder
// ============================================================================

TEST_CASE("Snapmaker batch_feed_gcode joins one AUTO_FEEDING line per slot", "[snapmaker][batch]") {
    SECTION("ordering is preserved, load verb") {
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({2, 0, 3}, true) ==
                "AUTO_FEEDING EXTRUDER=2 LOAD=1\n"
                "AUTO_FEEDING EXTRUDER=0 LOAD=1\n"
                "AUTO_FEEDING EXTRUDER=3 LOAD=1");
    }

    SECTION("unload verb") {
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({1, 3}, false) ==
                "AUTO_FEEDING EXTRUDER=1 UNLOAD=1\n"
                "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
    }

    SECTION("single slot has no join and no trailing newline") {
        const std::string gcode = helix::AmsBackendSnapmaker::batch_feed_gcode({0}, true);
        REQUIRE(gcode == "AUTO_FEEDING EXTRUDER=0 LOAD=1");
        REQUIRE(gcode.back() != '\n');
    }

    SECTION("empty input yields an empty string") {
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({}, true).empty());
        REQUIRE(helix::AmsBackendSnapmaker::batch_feed_gcode({}, false).empty());
    }
}

TEST_CASE("Snapmaker batch_feed_gcode drives AUTO_FEEDING_BATCH when the firmware has it",
          "[snapmaker][batch]") {
    SECTION("load with next-head preheat") {
        const std::string chain = helix::AmsBackendSnapmaker::batch_feed_gcode(
            {0, 2, 3}, /*load=*/true, /*use_batch_macro=*/true);

        CHECK(chain == "AUTO_FEEDING_BATCH ACTION=START\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=0 LOAD=1 NEXT_EXTRUDER=2\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=2 LOAD=1 NEXT_EXTRUDER=3\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=3 LOAD=1\n"
                       "AUTO_FEEDING_BATCH ACTION=END");
    }

    SECTION("unload keeps the batch sentinels") {
        const std::string chain = helix::AmsBackendSnapmaker::batch_feed_gcode(
            {1, 3}, /*load=*/false, /*use_batch_macro=*/true);

        CHECK(chain == "AUTO_FEEDING_BATCH ACTION=START\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=1 UNLOAD=1 NEXT_EXTRUDER=3\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=3 UNLOAD=1\n"
                       "AUTO_FEEDING_BATCH ACTION=END");
    }

    SECTION("single slot has no NEXT_EXTRUDER") {
        const std::string chain = helix::AmsBackendSnapmaker::batch_feed_gcode(
            {2}, /*load=*/true, /*use_batch_macro=*/true);

        CHECK(chain == "AUTO_FEEDING_BATCH ACTION=START\n"
                       "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=2 LOAD=1\n"
                       "AUTO_FEEDING_BATCH ACTION=END");
    }
}

// ============================================================================
// do_filament_batch — validation and the single-script dispatch
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Snapmaker batch dispatch sends ONE script for the whole batch",
                 "[snapmaker][batch]") {
    auto backend = snapmaker();

    SECTION("load batch reaches firmware as one multi-line script") {
        REQUIRE(backend->load_filament_batch({0, 2}).success());
        // The mock's gcode.script handler simulates a script line-by-line, so
        // the history records one entry per line. The RPC's own script param is
        // the wire truth: two separate RPCs would leave a single line in
        // last_send_script(), not the joined chain.
        REQUIRE(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=0 LOAD=1\n"
                                                  "AUTO_FEEDING EXTRUDER=2 LOAD=1");
        REQUIRE(sent_gcodes().size() == 2);
    }

    SECTION("unload batch uses the unload verb") {
        REQUIRE(backend->unload_filament_batch({3}).success());
        const auto& sent = sent_gcodes();
        REQUIRE(sent.size() == 1);
        REQUIRE(sent[0] == "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
    }

    SECTION("an out-of-range slot refuses and sends nothing") {
        REQUIRE(backend->load_filament_batch({0, 9}).result == AmsResult::INVALID_SLOT);
        REQUIRE(sent_gcodes().empty());
    }

    SECTION("an empty batch refuses and sends nothing") {
        // AmsErrorHelper::invalid_parameter() reports as WRONG_STATE.
        REQUIRE(backend->load_filament_batch({}).result == AmsResult::WRONG_STATE);
        REQUIRE(sent_gcodes().empty());
    }
}

// ============================================================================
// set_discovery: the batch shape comes from the discovery handed to the
// backend, not the global PrinterState
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Batch shape follows the discovery the backend was handed",
                 "[snapmaker][batch]") {
    // Startup builds and starts backends from AmsState's hardware argument
    // BEFORE PrinterState publishes discovery globally, so the capability
    // must arrive through set_discovery(), never a global read.
    get_printer_state().set_hardware(helix::PrinterDiscovery{});
    REQUIRE_FALSE(get_printer_state().get_discovery().has_auto_feeding_batch());

    /// A backend built the way init_backends_from_hardware() builds one:
    /// discovery applied before start().
    auto make = [&](const helix::PrinterDiscovery& hw) {
        auto b = helix::AmsBackend::create(helix::AmsType::SNAPMAKER, api.get(), &mock_client);
        REQUIRE(b != nullptr);
        b->set_discovery(hw);
        REQUIRE(b->start().success());
        mock_client.clear_gcode_script_history();
        return b;
    };

    SECTION("a discovery carrying the macro dispatches the batch state machine") {
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"gcode_macro AUTO_FEEDING_BATCH"}));
        REQUIRE(hw.has_auto_feeding_batch());

        auto backend = make(hw);
        REQUIRE(backend->load_filament_batch({1, 3}).success());
        REQUIRE(mock_client.last_send_script() ==
                "AUTO_FEEDING_BATCH ACTION=START\n"
                "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=1 LOAD=1 NEXT_EXTRUDER=3\n"
                "AUTO_FEEDING_BATCH ACTION=DOING EXTRUDER=3 LOAD=1\n"
                "AUTO_FEEDING_BATCH ACTION=END");
    }

    SECTION("a discovery without the macro keeps the per-head fallback") {
        helix::PrinterDiscovery bare;
        REQUIRE_FALSE(bare.has_auto_feeding_batch());

        auto backend = make(bare);
        REQUIRE(backend->load_filament_batch({1, 3}).success());
        REQUIRE(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=1 LOAD=1\n"
                                                  "AUTO_FEEDING EXTRUDER=3 LOAD=1");
    }
}

// ============================================================================
// The shared gate: capability, claim, print refusal
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Batch filament ops are not_supported on a plain backend",
                 "[ams][batch]") {
    BatchlessBackend backend;
    REQUIRE(backend.max_heads_per_filament_op() == 1);
    REQUIRE(backend.load_filament_batch({0}).result == AmsResult::NOT_SUPPORTED);
    REQUIRE(backend.unload_filament_batch({0}).result == AmsResult::NOT_SUPPORTED);
}

TEST_CASE_METHOD(BatchFixture, "Snapmaker advertises batch filament ops", "[ams][batch]") {
    auto backend = snapmaker();
    REQUIRE(backend->max_heads_per_filament_op() == 4);
}

TEST_CASE_METHOD(BatchFixture, "A batch is refused busy while another filament op holds the claim",
                 "[ams][batch][threading]") {
    ClaimParkingBackend backend;

    // Hold the claim from a second thread via a per-slot op parked inside its
    // hook. api_ is null so refuse_if_printing() short-circuits and the worker
    // never touches LVGL.
    std::thread holder([&] { (void)backend.load_filament(0); });
    backend.wait_for_parked_hook();

    const AmsError refused = backend.load_filament_batch({1});
    CHECK(refused.result == AmsResult::BUSY);

    backend.release_hook();
    holder.join();

    // The refusal must not have leaked a claim. A wedged one would answer BUSY
    // again; reaching do_filament_batch's null-api refusal instead proves the
    // batch was genuinely admitted through the gate the second time.
    REQUIRE(backend.load_filament_batch({1}).result == AmsResult::NOT_CONNECTED);
}

TEST_CASE_METHOD(BatchFixture, "A batch is refused while printing", "[ams][batch]") {
    auto backend = snapmaker();

    set_print_state(helix::PrintJobState::PRINTING);
    const AmsError refused = backend->load_filament_batch({0, 1});
    CHECK_FALSE(refused.success());
    CHECK(refused.result == AmsResult::WRONG_STATE);
    CHECK(sent_gcodes().empty());

    // A refused op releases its claim; the backend is not wedged busy.
    set_print_state(helix::PrintJobState::STANDBY);
    REQUIRE(backend->load_filament_batch({0, 1}).success());
    REQUIRE(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=0 LOAD=1\n"
                                              "AUTO_FEEDING EXTRUDER=1 LOAD=1");
    REQUIRE(sent_gcodes().size() == 2);
}

// ============================================================================
// channel_snapshot — the per-channel feeder fields the status parse keeps
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Snapmaker keeps the per-channel feeder fields",
                 "[snapmaker][batch]") {
    feed_status(R"({"filament_feed left":{"extruder0":{
        "channel_state":"load_finish","channel_error":"ok","filament_detected":true,
        "module_exist":true,"disable_auto":false}}})");

    const auto snap = backend().channel_snapshot(0);

    CHECK(snap.state == "load_finish");
    CHECK(snap.error == "ok");
    CHECK(snap.filament_detected);
    CHECK(snap.module_exist);
    CHECK_FALSE(snap.disable_auto);
}

TEST_CASE_METHOD(BatchFixture, "A feeder delta keeps the fields it does not mention",
                 "[snapmaker][batch]") {
    feed_status(R"({"filament_feed left":{"extruder0":{
        "channel_state":"load_finish","channel_error":"ok","filament_detected":true,
        "module_exist":true,"disable_auto":false}}})");

    SECTION("a channel_state-only frame leaves the booleans standing") {
        feed_status(R"({"filament_feed left":{"extruder0":{
            "channel_state":"unload_finish"}}})");
        const auto snap = backend().channel_snapshot(0);
        CHECK(snap.state == "unload_finish");
        CHECK(snap.filament_detected); // not cleared by a frame silent on it
        CHECK(snap.module_exist);
        CHECK_FALSE(snap.disable_auto);
    }

    SECTION("a filament_detected-only frame leaves state standing") {
        feed_status(R"({"filament_feed left":{"extruder0":{"filament_detected":false}}})");
        const auto snap = backend().channel_snapshot(0);
        CHECK(snap.state == "load_finish"); // not blanked by a frame silent on it
        CHECK_FALSE(snap.filament_detected);
        CHECK(snap.module_exist);
    }

    SECTION("an error token persists until a frame carries a real value again") {
        feed_status(R"({"filament_feed left":{"extruder0":{"channel_error":"jam"}}})");
        CHECK(backend().channel_snapshot(0).error == "jam");
        // A frame omitting channel_error keeps the token...
        feed_status(R"({"filament_feed left":{"extruder0":{"channel_state":"load_finish"}}})");
        CHECK(backend().channel_snapshot(0).error == "jam");
        // ...and an explicit "ok" clears it.
        feed_status(R"({"filament_feed left":{"extruder0":{"channel_error":"ok"}}})");
        CHECK(backend().channel_snapshot(0).error == "ok");
    }
}

// ============================================================================
// batch cursor — the progress line the channel parse renders per head
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "A finished batch leaves no progress line behind",
                 "[snapmaker][batch]") {
    helix::SnapmakerTestAccess::set_batch_plan(backend(), {0, 1}, /*load=*/true, "Load", "of");

    SECTION("mid-batch, the line names the head now in progress") {
        set_channel(0, "load_finish", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
        CHECK(backend().batch_plan().cursor == 1);
        CHECK(backend().batch_plan().active);
        CHECK(backend().get_system_info().operation_detail == "Load 2 of 2");
    }

    SECTION("the final head verifies and clears the line") {
        set_channel(0, "load_finish", "ok", true, true, false);
        set_channel(1, "load_finish", "ok", true, true, false);
        CHECK(backend().batch_plan().cursor == 2);
        CHECK_FALSE(backend().batch_plan().active);
        CHECK(backend().get_system_info().operation_detail.empty());
    }
}

TEST_CASE_METHOD(BatchFixture, "A batch head whose unload settles in one frame still verifies",
                 "[snapmaker][batch][action_state]") {
    helix::SnapmakerTestAccess::set_batch_plan(backend(), {2, 3}, /*load=*/false, "Unload", "of");

    auto frame = [](const char* state, const char* action_state, bool detected) {
        return nlohmann::json{{"filament_feed right",
                               {{"extruder2",
                                 {{"channel_state", state},
                                  {"channel_action_state", action_state},
                                  {"filament_detected", detected}}}}}}
            .dump();
    };
    feed_status(frame("unload_doing", "unload_doing", true));
    REQUIRE(backend().batch_plan().cursor == 0);

    feed_status(frame("wait_insert", "unload_finish", false));
    CHECK(backend().batch_plan().cursor == 1);
    CHECK(backend().batch_plan().active);
}

// ============================================================================
// slot_op_eligibility — the direction-dependent refusal, from channel state
// ============================================================================

TEST_CASE_METHOD(BatchFixture, "Snapmaker eligibility follows channel state",
                 "[snapmaker][batch]") {
    using E = helix::AmsBackend::FilamentOpEligibility;

    SECTION("preload_finish with filament loads, does not unload") {
        set_channel(0, "preload_finish", "ok", /*detected=*/true, /*module=*/true,
                    /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::NotLoaded);
    }
    SECTION("preload_finish with the toolhead switch reading filament loads and unloads") {
        set_channel(0, "preload_finish", "ok", /*detected=*/true, /*module=*/true,
                    /*no_auto=*/false);
        feed_status(R"({"filament_motion_sensor e0_filament": {"filament_detected": true}})");
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::Eligible);
    }
    SECTION("load_finish unloads, does not load") {
        set_channel(0, "load_finish", "ok", true, true, false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::AlreadyLoaded);
    }
    SECTION("a manual feed that finished on the printer is settled") {
        // manual_sta_finish is a persistent terminal: a head that completed a
        // manual EXTRUDE sits in it until the next operation. It is not a
        // load (the classifier leaves the loaded latch clear), so an unload
        // still refuses with NotLoaded while a load may proceed.
        set_channel(0, "manual_sta_finish", "ok", /*detected=*/true, /*module=*/true,
                    /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(0, /*load=*/true) == E::Eligible);
        CHECK(backend().slot_op_eligibility(0, /*load=*/false) == E::NotLoaded);
    }
    SECTION("wait_insert with no filament is empty in both directions") {
        set_channel(0, "wait_insert", "ok", /*detected=*/false, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Empty);
        CHECK(backend().slot_op_eligibility(0, false) == E::Empty);
    }
    SECTION("a feeder fault beats everything") {
        set_channel(0, "load_finish", "jam", true, true, false);
        CHECK(backend().slot_op_eligibility(0, false) == E::Error);
    }
    SECTION("an empty lane's no_filament token is Empty, not a feeder error") {
        // The firmware reports no_filament for any lane without filament;
        // on a settled state it must not read as a fault.
        set_channel(0, "wait_insert", "no_filament", /*detected=*/false, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Empty);
        CHECK(backend().slot_op_eligibility(0, false) == E::Empty);
    }
    SECTION("an unsettled empty lane is still Empty") {
        // An idle empty lane reports an unsettled state; presence wins.
        set_channel(0, "none", "no_filament", false, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Empty);
    }
    SECTION("blank and none error tokens are not faults") {
        set_channel(0, "load_finish", "none", true, true, false);
        CHECK(backend().slot_op_eligibility(0, false) == E::Eligible);
        set_channel(0, "load_finish", "", true, true, false);
        CHECK(backend().slot_op_eligibility(0, false) == E::Eligible);
    }
    SECTION("manual mode or absent module refuses an otherwise eligible head") {
        set_channel(0, "preload_finish", "ok", true, /*module=*/true, /*no_auto=*/true);
        CHECK(backend().slot_op_eligibility(0, true) == E::FeederUnavailable);
        set_channel(1, "preload_finish", "ok", true, /*module=*/false, /*no_auto=*/false);
        CHECK(backend().slot_op_eligibility(1, true) == E::FeederUnavailable);
    }
    SECTION("an unrecognised state is busy, never eligible") {
        set_channel(0, "loading", "ok", true, true, false);
        CHECK(backend().slot_op_eligibility(0, true) == E::Busy);
        CHECK(backend().slot_op_eligibility(0, false) == E::Busy);
    }
    SECTION("a disabled motion sensor blocks an otherwise eligible load") {
        set_channel(0, "unload_finish", "ok", true, true, false);
        feed_status(R"({"filament_motion_sensor e0_filament":{"enabled":false}})");
        CHECK(backend().slot_op_eligibility(0, true) == E::SensorDisabled);
        feed_status(R"({"filament_motion_sensor e0_filament":{"enabled":true}})");
        CHECK(backend().slot_op_eligibility(0, true) == E::Eligible);
    }
    SECTION("an out-of-range slot is busy") {
        CHECK(backend().slot_op_eligibility(9, true) == E::Busy);
        CHECK(backend().slot_op_eligibility(-1, false) == E::Busy);
    }
}

TEST_CASE_METHOD(BatchFixture, "A backend without the override stays permissive", "[ams][batch]") {
    // The default must not change behaviour for AFC, Happy Hare, ACE or AD5X.
    BatchlessBackend backend;
    CHECK(backend.slot_op_eligibility(0, /*load=*/true) ==
          helix::AmsBackend::FilamentOpEligibility::Eligible);
    CHECK(backend.slot_op_eligibility(9, /*load=*/false) ==
          helix::AmsBackend::FilamentOpEligibility::Eligible);
}

// ============================================================================
// operation_working_slot: the one derivation of "the head an operation is
// working on" (batch cursor while a batch runs, else the in-progress channel,
// else none). The sidebar header reads it through AmsState; current_slot
// stays the carriage answer throughout.
// ============================================================================

TEST_CASE_METHOD(BatchFixture,
                 "A batch unload's working head is the cursor head, not the carriage tool",
                 "[snapmaker][batch][ams]") {
    // The device sequence: batch-unloading heads 1 and 3 while the carriage
    // still reports tool 3 (the previous head). The header must name the head
    // being unloaded, and the carriage answers must not move at all.
    feed_status(R"({"toolhead":{"extruder":"extruder3"}})");
    set_channel(3, "load_finish", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
    REQUIRE(backend().get_system_info().current_slot == 3);
    REQUIRE(backend().get_system_info().units[0].get_slot(3)->status == helix::SlotStatus::LOADED);

    helix::SnapmakerTestAccess::set_batch_plan(backend(), {1, 3}, /*load=*/false, "Unload", "of");
    set_channel(1, "unload_homing", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
    CHECK(backend().get_system_info().operation_working_slot == 1);

    // A toolhead-only delta mid-batch carries no channel evidence: the working
    // head stays 1, the carriage slot stays 3, and slot 3's status does not
    // flap through a demote/re-promote cycle.
    feed_status(R"({"toolhead":{"extruder":"extruder3"}})");
    CHECK(backend().get_system_info().operation_working_slot == 1);
    CHECK(backend().get_system_info().current_slot == 3);
    CHECK(backend().get_system_info().units[0].get_slot(3)->status == helix::SlotStatus::LOADED);

    // The cursor advances to head 3; its in-progress frame renames the header
    // without any toolhead evidence moving yet.
    set_channel(1, "unload_finish", "ok", true, true, false);
    set_channel(3, "unload_homing", "ok", true, true, false);
    CHECK(backend().get_system_info().operation_working_slot == 3);
    CHECK(backend().get_system_info().current_slot == 3);

    SECTION("the cursor head wins over a stray in-progress frame") {
        // A late transient from a non-cursor head must not steal the header:
        // the plan, not the channel that happened to speak, says which head
        // the batch is working.
        helix::SnapmakerTestAccess::set_batch_plan(backend(), {0, 2}, /*load=*/true, "Load", "of");
        set_channel(2, "load_feeding", "ok", true, true, false);
        CHECK(backend().get_system_info().operation_working_slot == 0);
    }

    SECTION("the header subject names the working head, not the carriage tool") {
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().sync_from_backend();
        lv_subject_t* header = lv_xml_get_subject(nullptr, "ams_current_slot_text");
        REQUIRE(header != nullptr);
        // Head 3 is 0-based; lane labels are 1-based over the heads.
        CHECK(std::string(lv_subject_get_string(header)) ==
              helix::ui::lane_label(backend().lane_noun(), 3));
    }
}

TEST_CASE_METHOD(BatchFixture, "A standalone unload's working head is the unloading head",
                 "[snapmaker][batch][ams]") {
    feed_status(R"({"toolhead":{"extruder":"extruder3"}})");
    REQUIRE(backend().get_system_info().current_slot == 3);

    // No batch armed: the head reporting an in-progress state is the one.
    set_channel(1, "unload_homing", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
    CHECK(backend().get_system_info().operation_working_slot == 1);
    CHECK(backend().get_system_info().current_slot == 3);

    // A toolhead-only delta mid-op carries no channel evidence, so the
    // working head keeps its answer rather than flapping to the carriage tool.
    feed_status(R"({"toolhead":{"extruder":"extruder3"}})");
    CHECK(backend().get_system_info().operation_working_slot == 1);

    // At rest (terminal state resolves the action) no head is being worked
    // and the header falls back to the carriage tool.
    set_channel(1, "unload_finish", "ok", true, true, false);
    CHECK(backend().get_system_info().operation_working_slot == -1);
    CHECK(backend().get_system_info().current_slot == 3);
}

TEST_CASE_METHOD(BatchFixture, "An op's working head names the header, not the loaded card",
                 "[snapmaker][batch][ams]") {
    // The card, filament_loaded and the Spoolman active spool describe the
    // carriage; only the header follows the head an unload is working on.
    feed_status(R"({"toolhead":{"extruder":"extruder3"}})");
    REQUIRE(backend().get_system_info().current_slot == 3);
    helix::ams::FilamentSlotOverride blue;
    blue.color_rgb = 0x0000FF;
    blue.color_set = true;
    helix::ams::FilamentSlotOverride red;
    red.color_rgb = 0xFF0000;
    red.color_set = true;
    helix::SnapmakerTestAccess::seed_override(backend(), 3, blue);
    helix::SnapmakerTestAccess::seed_override(backend(), 1, red);
    // The carriage head latched loaded, as on the rig.
    set_channel(3, "load_finish", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);

    auto& ams = helix::AmsState::instance();
    ams.init_subjects(true);
    ams.sync_from_backend();
    lv_subject_t* color = ams.get_current_color_subject();
    lv_subject_t* header = lv_xml_get_subject(nullptr, "ams_current_slot_text");
    REQUIRE(header != nullptr);
    const int card_at_rest = lv_subject_get_int(color);
    REQUIRE(card_at_rest == 0x0000FF);

    set_channel(1, "unload_homing", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
    ams.sync_from_backend();

    const std::string working_header = helix::ui::lane_label(backend().lane_noun(), 1);
    CHECK(std::string(lv_subject_get_string(header)) == working_header);
    CHECK(lv_subject_get_int(color) == card_at_rest);

    SECTION("a repeat sync mid-op does not flash the carriage slot through the header") {
        int notifications = 0;
        lv_observer_t* obs = lv_subject_add_observer(
            header,
            [](lv_observer_t* o, lv_subject_t*) {
                ++*static_cast<int*>(lv_observer_get_user_data(o));
            },
            &notifications);
        notifications = 0; // adding an observer notifies it once
        ams.sync_from_backend();
        lv_observer_remove(obs);
        CHECK(notifications == 0);
        CHECK(std::string(lv_subject_get_string(header)) == working_header);
    }
}

TEST_CASE_METHOD(BatchFixture, "AmsState answers whether any backend has a batch in flight",
                 "[snapmaker][batch][ams]") {
    // Asked from the WebSocket thread at connect, so it must answer from
    // inside AmsState rather than hand out backend pointers.
    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    CHECK_FALSE(ams.any_filament_batch_in_flight());

    ams.add_backend(std::make_unique<BatchlessBackend>());
    auto snapmaker = std::make_unique<helix::AmsBackendSnapmaker>(nullptr, nullptr);
    auto* second = snapmaker.get();
    ams.add_backend(std::move(snapmaker));
    CHECK_FALSE(ams.any_filament_batch_in_flight());

    // The live batch sits on the SECOND backend: every backend is asked.
    helix::SnapmakerTestAccess::set_batch_plan(*second, {0}, /*load=*/true, "Load", "of");
    CHECK(ams.any_filament_batch_in_flight());

    ams.clear_backends();
    CHECK_FALSE(ams.any_filament_batch_in_flight());
}

TEST_CASE_METHOD(BatchFixture, "A batch the firmware ends mid-head leaves no working head",
                 "[snapmaker][batch][ams]") {
    // The script can end (abort, lost response, a feeder wedging) without the
    // cursor head reaching a terminal state; the frame that reports doing=false
    // then carries no channel evidence at all. Nothing is being worked after it.
    feed_status(R"({"toolhead":{"extruder":"extruder3"}})");
    set_channel(3, "load_finish", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
    helix::SnapmakerTestAccess::set_batch_macro_object(backend(), "gcode_macro AUTO_FEEDING_BATCH");
    helix::SnapmakerTestAccess::set_batch_plan(backend(), {1, 3}, /*load=*/false, "Unload", "of");
    set_channel(1, "unload_doing", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
    REQUIRE(backend().get_system_info().operation_working_slot == 1);

    feed_status(R"({"gcode_macro AUTO_FEEDING_BATCH":{"doing":false}})");
    CHECK_FALSE(backend().batch_plan().active);
    CHECK(backend().get_system_info().operation_working_slot == -1);
    CHECK(backend().get_system_info().current_slot == 3);

    helix::AmsState::instance().init_subjects(true);
    helix::AmsState::instance().sync_from_backend();
    lv_subject_t* header = lv_xml_get_subject(nullptr, "ams_current_slot_text");
    REQUIRE(header != nullptr);
    CHECK(std::string(lv_subject_get_string(header)) ==
          helix::ui::lane_label(backend().lane_noun(), 3));
}

// ============================================================================
// The head picker, opened by the sidebar's Load and Unload buttons
// ============================================================================

namespace {
/// Builds the production sidebar inside a panel, the way ams_panel.xml does,
/// so the static callbacks reach this instance.
struct PickerSidebar {
    PickerSidebar(lv_obj_t* screen, helix::PrinterState& state) {
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().sync_from_backend();
        helix::test::register_ams_sidebar_xml();
        panel = lv_obj_create(screen);
        const char* attrs[] = {"name", "ams_operation_sidebar", nullptr};
        root = static_cast<lv_obj_t*>(lv_xml_create(panel, "ams_sidebar", attrs));
        REQUIRE(root != nullptr);
        sidebar = std::make_unique<helix::ui::AmsOperationSidebar>(state);
        REQUIRE(sidebar->setup(panel));
    }
    ~PickerSidebar() {
        sidebar.reset(); // cancel the stall watchdog before the tree goes
        lv_obj_delete(panel);
    }
    lv_obj_t* button(const char* name) const {
        lv_obj_t* btn = lv_obj_find_by_name(root, name);
        REQUIRE(btn != nullptr);
        return btn;
    }
    lv_obj_t* panel = nullptr;
    lv_obj_t* root = nullptr;
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar;
};

void tick_row(lv_obj_t* dialog, int head) {
    lv_obj_t* row = lv_obj_find_by_name(dialog, ("item_" + std::to_string(head)).c_str());
    REQUIRE(row != nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
}
} // namespace

TEST_CASE_METHOD(BatchModalFixture, "The head picker names the op it opened for",
                 "[snapmaker][batch][ams][multiselect]") {
    REQUIRE(backend().start().success());
    for (int slot = 0; slot < 4; ++slot) {
        set_channel(slot, "preload_finish", "ok", /*detected=*/true, /*module=*/true,
                    /*no_auto=*/false);
    }

    SECTION("Load") {
        REQUIRE(helix::ui::BatchFilamentModal::show_owned(helix::ui::HeadPickOp::Load, {}));
        lv_obj_t* dialog = picker_dialog();
        CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(dialog, "batch_title"))) ==
              lv_tr("Load into which tool?"));
        CHECK(button_caption(dialog, "btn_primary") == lv_tr("Load"));
        for (int head = 0; head < 4; ++head) {
            REQUIRE(lv_obj_find_by_name(dialog, ("item_" + std::to_string(head)).c_str()));
        }
        close_picker(dialog);
    }
    SECTION("Unload") {
        REQUIRE(helix::ui::BatchFilamentModal::show_owned(helix::ui::HeadPickOp::Unload, {}));
        lv_obj_t* dialog = picker_dialog();
        CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(dialog, "batch_title"))) ==
              lv_tr("Unload which tool?"));
        CHECK(button_caption(dialog, "btn_primary") == lv_tr("Unload"));
        close_picker(dialog);
    }
}

TEST_CASE_METHOD(BatchModalFixture, "A Load open with every feeder empty ticks nothing",
                 "[snapmaker][batch][ams][multiselect]") {
    // Feeding an empty lane is the no-op the firmware refuses, so the
    // preselection must leave every row unticked when no lane carries filament.
    REQUIRE(backend().start().success());
    for (int slot = 0; slot < 4; ++slot) {
        set_channel(slot, "idle", "ok", /*detected=*/false, /*module=*/true, /*no_auto=*/false);
    }

    REQUIRE(helix::ui::BatchFilamentModal::show_owned(helix::ui::HeadPickOp::Load, {}));
    lv_obj_t* dialog = picker_dialog();
    for (int slot = 0; slot < 4; ++slot) {
        CHECK_FALSE(row_checked(dialog, std::to_string(slot).c_str()));
    }
    close_picker(dialog);
}

TEST_CASE_METHOD(BatchModalFixture,
                 "A running op refuses to open the picker from either sidebar button",
                 "[snapmaker][batch][ams][sidebar]") {
    // The buttons are disabled through gating subjects, but the refusal must
    // not live only there: a tap can land in the window between an op starting
    // and the disabled binding catching up, so each entry re-checks. This test
    // drives the real sidebar XML because that wiring is the regression.
    REQUIRE(backend().start().success());
    PickerSidebar ui(test_screen(), BatchMockHarness::state);
    REQUIRE(Modal::get_top() == nullptr);

    // Busy with the gating subjects still settled idle: the tap arrives in
    // that window, so the handler's own re-check is the only refusal left.
    set_channel(0, "load_feeding", "ok", /*detected=*/true, /*module=*/true, /*no_auto=*/false);
    REQUIRE(backend().get_system_info().is_busy());

    auto tap_expects_no_picker = [this](lv_obj_t* btn) {
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* top = Modal::get_top();
        CHECK(top == nullptr);
        if (top) {
            Modal::hide(top);
            process_lvgl(200); // free the one-shot instance
        }
    };

    // Several heads: Unload is the picker button; the one-tap one hides.
    CHECK(lv_obj_has_flag(ui.button("btn_unload"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(ui.button("btn_batch_unload"), LV_OBJ_FLAG_HIDDEN));
    tap_expects_no_picker(ui.button("btn_batch_unload"));
    tap_expects_no_picker(ui.button("btn_load"));
}

TEST_CASE_METHOD(BatchModalFixture,
                 "The picker hands its heads to the sidebar, which batches several; Cancel sends "
                 "nothing",
                 "[snapmaker][batch][ams][sidebar]") {
    REQUIRE(backend().start().success());
    mock_client.clear_gcode_script_history();

    SECTION("a Load of four heads sends one LOAD script") {
        for (int slot = 0; slot < 4; ++slot) {
            set_channel(slot, "preload_finish", "ok", /*detected=*/true, /*module=*/true,
                        /*no_auto=*/false);
        }
        PickerSidebar ui(test_screen(), BatchMockHarness::state);
        lv_obj_send_event(ui.button("btn_load"), LV_EVENT_CLICKED, nullptr);
        lv_obj_t* dialog = picker_dialog();
        for (int head = 0; head < 4; ++head) {
            if (!row_checked(dialog, std::to_string(head).c_str())) {
                tick_row(dialog, head);
            }
        }
        click(dialog, "btn_primary");
        CHECK(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=0 LOAD=1\n"
                                                "AUTO_FEEDING EXTRUDER=1 LOAD=1\n"
                                                "AUTO_FEEDING EXTRUDER=2 LOAD=1\n"
                                                "AUTO_FEEDING EXTRUDER=3 LOAD=1");
        process_lvgl(200); // free the one-shot instance
    }
    SECTION("an Unload of two heads sends the UNLOAD verb for those two") {
        for (int slot = 0; slot < 4; ++slot) {
            set_channel(slot, "load_finish", "ok", /*detected=*/true, /*module=*/true,
                        /*no_auto=*/false);
        }
        PickerSidebar ui(test_screen(), BatchMockHarness::state);
        lv_obj_send_event(ui.button("btn_batch_unload"), LV_EVENT_CLICKED, nullptr);
        lv_obj_t* dialog = picker_dialog();
        for (int head = 0; head < 4; ++head) {
            const bool want = head == 1 || head == 3;
            if (row_checked(dialog, std::to_string(head).c_str()) != want) {
                tick_row(dialog, head);
            }
        }
        click(dialog, "btn_primary");
        CHECK(mock_client.last_send_script() == "AUTO_FEEDING EXTRUDER=1 UNLOAD=1\n"
                                                "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
        process_lvgl(200);
    }
    SECTION("Cancel closes without sending anything") {
        PickerSidebar ui(test_screen(), BatchMockHarness::state);
        lv_obj_send_event(ui.button("btn_load"), LV_EVENT_CLICKED, nullptr);
        click(picker_dialog(), "btn_secondary");
        CHECK(sent_gcodes().empty());
        process_lvgl(200);
    }
}
