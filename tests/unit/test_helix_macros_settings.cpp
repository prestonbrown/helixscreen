// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_helix_macros_settings.cpp
 * @brief The Advanced panel's helper-macro rows and their restart queue
 *
 * The rows are driven by the helix_macros_status subject (all four detection
 * states plus the composed restart-pending one); the install/update actions
 * stage helix_macros.cfg over the Moonraker file API and own the timing of
 * the Klipper restart: immediately when idle, queued behind a one-time
 * print-complete offer when a print is active, and hard-refused (never
 * silently deferred) while a print holds the machine.
 */

#include "ui_modal.h"
#include "ui_panel_advanced.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/advanced_panel_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::AdvancedPanelTestAccess;
using helix::ui::UpdateQueue;

namespace {

const char* kPrinterCfg = "[stepper_x]\nstep_pin: PF0\n";

/// Discovery snapshots that parse a real objects list (so the status is an
/// observation, not UNKNOWN).
helix::PrinterDiscovery discovery_with(const std::vector<std::string>& objects) {
    helix::PrinterDiscovery hw;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& o : objects) {
        arr.push_back(o);
    }
    hw.parse_objects(arr);
    return hw;
}

struct MacrosSettingsFixture : LVGLUITestFixture {
    MoonrakerClientMock client_;
    MoonrakerAPIMock api_{client_, state()};
    AdvancedPanel panel{state(), &api_};

    MacrosSettingsFixture() {
        helix::ui::modal_init_subjects();
        // Fresh session on the shared singleton: subjects carry their last
        // value across tests otherwise, and every status assertion below
        // reads another test's leftovers.
        helix::PrinterStateTestAccess::reset(state());
        state().init_subjects(true);
        api_.set_config_files({{"printer.cfg", kPrinterCfg}});
        AdvancedPanelTestAccess::wire_macro_observer(panel);
    }

    ~MacrosSettingsFixture() override {
        UpdateQueue::instance().drain();
    }

    int macros_status() {
        UpdateQueue::instance().drain();
        return lv_subject_get_int(state().plugin_status_state().get_helix_macros_status_subject());
    }

    /// The restart policy reads job_holds_machine (print_active misses a
    /// host-side Preparing job), so that is the seam the tests drive.
    void set_job_holds(int v) {
        lv_subject_set_int(state().print_state().get_job_holds_machine_subject(), v);
    }

    bool restart_sent() const {
        return client_.last_send_method() == "printer.restart";
    }

    /// A drained step can queue the next one; pump until quiet.
    void settle() {
        UpdateQueue::instance().drain_until_empty(12);
    }

    void tap_install_row() {
        AdvancedPanelTestAccess::tap_macros_install_row(panel);
        process_lvgl(50);
    }

    void tap_update_row() {
        AdvancedPanelTestAccess::tap_macros_update_row(panel);
        process_lvgl(50);
    }

    /// Answer the top modal by clicking one of its buttons.
    void answer_modal(const char* button_name) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* button = lv_obj_find_by_name(dialog, button_name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        settle();
    }

    void confirm_install() {
        answer_modal("btn_primary");
    }
    void cancel_modal() {
        answer_modal("btn_secondary");
    }
};

} // namespace

// ============================================================================
// Status subject mapping (what the rows bind against)
// ============================================================================

TEST_CASE_METHOD(MacrosSettingsFixture, "helix_macros_status maps every discovery state",
                 "[advanced][macros][1271]") {
    using S = helix::HelixMacrosStatus;

    // No discovery yet: UNKNOWN, and no row offers anything.
    REQUIRE(macros_status() == static_cast<int>(S::Unknown));

    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    REQUIRE(macros_status() == static_cast<int>(S::NotInstalled));

    state().set_hardware(discovery_with(
        {"gcode_macro HELIX_READY", "gcode_macro HELIX_ENDED", "gcode_macro HELIX_RESET",
         "gcode_macro HELIX_START_PRINT", "gcode_macro HELIX_CLEAN_NOZZLE",
         "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro HELIX_UNLOAD_FILAMENT"}));
    REQUIRE(macros_status() == static_cast<int>(S::Installed));

    state().set_hardware(
        discovery_with({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                        "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED"}));
    REQUIRE(macros_status() == static_cast<int>(S::Outdated));

    // A snapshot that consumed no objects list says UNKNOWN, not "absent".
    state().set_hardware(helix::PrinterDiscovery{});
    REQUIRE(macros_status() == static_cast<int>(S::Unknown));
}

TEST_CASE_METHOD(MacrosSettingsFixture,
                 "restart pending composes over not-installed and outdated, cleared by installed",
                 "[advanced][macros][1271]") {
    using S = helix::HelixMacrosStatus;

    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    REQUIRE(macros_status() == static_cast<int>(S::NotInstalled));

    state().plugin_status_state().set_helix_macros_restart_pending(true);
    REQUIRE(macros_status() == static_cast<int>(S::RestartPending));

    // Fresh scenario for the update path: clear the flag, then stage an
    // update onto an Outdated base.
    state().plugin_status_state().set_helix_macros_restart_pending(false);
    state().set_hardware(
        discovery_with({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                        "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED"}));
    REQUIRE(macros_status() == static_cast<int>(S::Outdated)); // pending not yet set here
    state().plugin_status_state().set_helix_macros_restart_pending(true);
    REQUIRE(macros_status() == static_cast<int>(S::RestartPending));
    // A re-scan while the restart is still owed must not resolve the queue.
    state().set_hardware(
        discovery_with({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                        "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED"}));
    REQUIRE(macros_status() == static_cast<int>(S::RestartPending));

    // Discovery reporting the CURRENT pack active means a restart landed: the
    // pending flag is stale by definition and clears.
    state().set_hardware(
        discovery_with({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                        "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED",
                        "gcode_macro HELIX_UNLOAD_FILAMENT"}));
    REQUIRE(macros_status() == static_cast<int>(S::Installed));

    // Pending set while the macros are active never masks the truth.
    state().plugin_status_state().set_helix_macros_restart_pending(true);
    REQUIRE(macros_status() == static_cast<int>(S::Installed));
}

// ============================================================================
// Install / update actions
// ============================================================================

TEST_CASE_METHOD(MacrosSettingsFixture, "confirmed install stages files and restarts when idle",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    REQUIRE(macros_status() == 0);

    tap_install_row();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr); // asks before acting
    REQUIRE_FALSE(restart_sent());
    REQUIRE_FALSE(api_.get_uploaded_config("helix_macros.cfg").has_value());

    confirm_install();

    CHECK(api_.get_uploaded_config("helix_macros.cfg").has_value());
    CHECK(api_.get_uploaded_config("printer.cfg").value_or("").find("[include helix_macros.cfg]") !=
          std::string::npos);
    CHECK(restart_sent());
}

TEST_CASE_METHOD(MacrosSettingsFixture, "cancelled install stages nothing and never restarts",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));

    tap_install_row();
    cancel_modal();

    CHECK_FALSE(api_.get_uploaded_config("helix_macros.cfg").has_value());
    CHECK_FALSE(restart_sent());
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(MacrosSettingsFixture,
                 "install during a print stages files and queues the restart",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    set_job_holds(1);
    settle();

    tap_install_row();
    confirm_install();

    // Files landed; the restart did not, and the row says why.
    CHECK(api_.get_uploaded_config("helix_macros.cfg").has_value());
    CHECK_FALSE(restart_sent());
    CHECK(macros_status() == 3);
    CHECK_FALSE(AdvancedPanelTestAccess::macro_restart_offer_made(panel));
}

TEST_CASE_METHOD(MacrosSettingsFixture, "print completion pops the one-time restart offer",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    set_job_holds(1);
    settle();
    tap_install_row();
    confirm_install();
    REQUIRE(macros_status() == 3);

    set_job_holds(0);
    settle();

    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK(AdvancedPanelTestAccess::macro_restart_offer_made(panel));

    confirm_install(); // the offer's Restart Now button
    CHECK(restart_sent());
}

TEST_CASE_METHOD(MacrosSettingsFixture,
                 "declined offer keeps the pending truth and does not re-offer",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    set_job_holds(1);
    settle();
    tap_install_row();
    confirm_install();

    set_job_holds(0);
    settle();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    cancel_modal();
    CHECK(ModalStack::instance().stack_empty());
    CHECK(macros_status() == 3); // still staged, still honest

    // A later print ending does not nag: one offer per staging.
    set_job_holds(1);
    settle();
    set_job_holds(0);
    settle();
    CHECK(ModalStack::instance().stack_empty());
    CHECK(AdvancedPanelTestAccess::macro_restart_offer_made(panel));
}

TEST_CASE_METHOD(MacrosSettingsFixture,
                 "a print restarting before the offer is answered hard-refuses the restart",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    set_job_holds(1);
    settle();
    tap_install_row();
    confirm_install();

    set_job_holds(0);
    settle();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    // A new print starts between the offer and the tap on Restart Now.
    set_job_holds(1);
    settle();
    confirm_install();

    CHECK_FALSE(restart_sent()); // refused, not silently deferred
    CHECK(macros_status() == 3);
}

TEST_CASE_METHOD(MacrosSettingsFixture, "update row offers only when outdated",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));

    tap_update_row();
    CHECK(ModalStack::instance().stack_empty()); // NOT_INSTALLED: no update dialog
    CHECK_FALSE(restart_sent());

    state().set_hardware(
        discovery_with({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                        "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED"}));
    REQUIRE(macros_status() == 2);

    tap_update_row();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    confirm_install();

    // Update stages the new pack and restarts (no print active).
    CHECK(api_.get_uploaded_config("helix_macros.cfg").has_value());
    CHECK(restart_sent());
}

TEST_CASE_METHOD(MacrosSettingsFixture,
                 "update staged during a print queues its restart and the offer fires",
                 "[advanced][macros][1271]") {
    // The outdated rung: discovery reports v2.0, the shipped pack is newer.
    state().set_hardware(
        discovery_with({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                        "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED"}));
    REQUIRE(macros_status() == 2);
    set_job_holds(1);
    settle();

    tap_update_row();
    confirm_install();

    // Files staged, restart queued: the base is Outdated, but the pending
    // flag must mask it or the toast's after-the-print promise is a lie.
    CHECK(api_.get_uploaded_config("helix_macros.cfg").has_value());
    CHECK_FALSE(restart_sent());
    REQUIRE(macros_status() == 3);

    set_job_holds(0);
    settle();

    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    confirm_install(); // Restart Now
    CHECK(restart_sent());
}

TEST_CASE_METHOD(MacrosSettingsFixture, "a host-side preparing job hard-refuses the restart too",
                 "[advanced][macros][1271]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));

    // Preparing: job_holds_machine is 1 while print_active stays 0 - the
    // machine is committed to a job the wire has not reported yet.
    set_job_holds(1);
    lv_subject_set_int(state().print_state().get_print_active_subject(), 0);
    settle();

    tap_install_row();
    confirm_install();

    CHECK(api_.get_uploaded_config("helix_macros.cfg").has_value());
    CHECK_FALSE(restart_sent()); // refused: a restart through Preparing kills the job
    CHECK(macros_status() == 3);
}

TEST_CASE_METHOD(MacrosSettingsFixture, "install row refuses to act on unknown status",
                 "[advanced][macros][1271]") {
    // No set_hardware: status is UNKNOWN, the row would be hidden, and the
    // handler must not offer an install that cannot honestly be made.
    REQUIRE(macros_status() == -1);

    tap_install_row();
    CHECK(ModalStack::instance().stack_empty());
    CHECK_FALSE(restart_sent());
    CHECK_FALSE(api_.get_uploaded_config("helix_macros.cfg").has_value());
}

// ============================================================================
// Row visibility (the XML bind itself)
// ============================================================================

TEST_CASE_METHOD(MacrosSettingsFixture, "helper macro rows follow helix_macros_status",
                 "[advanced][macros][1271][xml]") {
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "advanced_panel", nullptr));
    REQUIRE(root != nullptr);

    auto* install_row = lv_obj_find_by_name(root, "row_helix_macros_install");
    auto* update_row = lv_obj_find_by_name(root, "row_helix_macros_update");
    // The two info rows carry their bind on their wrapper containers (the
    // spoolman pattern), so visibility lives on the wrapper.
    auto* installed_row = lv_obj_find_by_name(root, "container_helix_macros_installed");
    auto* pending_row = lv_obj_find_by_name(root, "container_helix_macros_restart_pending");
    REQUIRE(install_row != nullptr);
    REQUIRE(update_row != nullptr);
    REQUIRE(installed_row != nullptr);
    REQUIRE(pending_row != nullptr);

    auto hidden = [](lv_obj_t* o) { return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); };
    lv_subject_t* status = state().plugin_status_state().get_helix_macros_status_subject();

    // UNKNOWN: everything hidden - no install button on a vacuum.
    lv_subject_set_int(status, -1);
    process_lvgl(20);
    CHECK(hidden(install_row));
    CHECK(hidden(update_row));
    CHECK(hidden(installed_row));
    CHECK(hidden(pending_row));

    lv_subject_set_int(status, 0);
    process_lvgl(20);
    CHECK_FALSE(hidden(install_row));
    CHECK(hidden(update_row));
    CHECK(hidden(installed_row));
    CHECK(hidden(pending_row));

    lv_subject_set_int(status, 1);
    process_lvgl(20);
    CHECK(hidden(install_row));
    CHECK(hidden(update_row));
    CHECK_FALSE(hidden(installed_row));
    CHECK(hidden(pending_row));

    lv_subject_set_int(status, 2);
    process_lvgl(20);
    CHECK(hidden(install_row));
    CHECK_FALSE(hidden(update_row));
    CHECK(hidden(installed_row));
    CHECK(hidden(pending_row));

    lv_subject_set_int(status, 3);
    process_lvgl(20);
    CHECK(hidden(install_row));
    CHECK(hidden(update_row));
    CHECK(hidden(installed_row));
    CHECK_FALSE(hidden(pending_row));

    lv_obj_delete(root);
}

// The XML event_cb names must reach the registered callbacks — the flow tests
// above drive the handlers directly, so a dropped registration would otherwise
// ship as a silent dead row.
TEST_CASE_METHOD(MacrosSettingsFixture,
                 "a click on the install row's XML reaches the panel handler",
                 "[advanced][macros][1271][xml]") {
    state().set_hardware(discovery_with({"gcode_macro START_PRINT", "bed_mesh"}));
    REQUIRE(macros_status() == 0);

    // The XML callbacks dispatch through the global panel instance, and the
    // names must be registered before the XML is created.
    init_global_advanced_panel(state(), &api_);
    get_global_advanced_panel().init_subjects();
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "advanced_panel", nullptr));
    lv_obj_t* row = lv_obj_find_by_name(root, "row_helix_macros_install");
    REQUIRE(row != nullptr);

    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    settle();

    CHECK(ModalStack::instance().top_dialog() != nullptr);

    // The global holds fixture-owned refs; free it before this test dies.
    StaticPanelRegistry::instance().destroy_all();
    lv_obj_delete(root);
}

// setup() is where production attaches the print-active observer; the flow
// tests above wire it by hand, so dropping the setup call would ship as a
// queue that never fires.
TEST_CASE_METHOD(MacrosSettingsFixture, "setup wires the macro restart observer",
                 "[advanced][macros][1271]") {
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "advanced_panel", nullptr));
    // The fixture pre-wires its own panel; a fresh one proves setup() does it.
    AdvancedPanel fresh{state(), &api_};
    REQUIRE_FALSE(AdvancedPanelTestAccess::macro_observer_wired(fresh));

    fresh.setup(root, lv_screen_active());
    CHECK(AdvancedPanelTestAccess::macro_observer_wired(fresh));

    lv_obj_delete(root);
}

// ============================================================================
// Skip wrappers: one rollback when Klipper will not start with them
// ============================================================================

namespace {

struct SkipsRollbackFixture : MacrosSettingsFixture {
    SkipsRollbackFixture() {
        auto hw = discovery_with({"gcode_macro START_PRINT", "bed_mesh"});
        hw.set_skip_wrappers({helix::skip_wrappers::Op::BedMesh}, {});
        state().set_hardware(hw);
        klippy(KlippyState::READY);
        tap_install_row();
        confirm_install();
        REQUIRE(restart_sent());
        REQUIRE(api_.get_uploaded_config("printer.cfg")
                    .value_or("")
                    .find("[include helix_skips.cfg]") != std::string::npos);
        mark_sends();
    }

    void klippy(KlippyState s) {
        state().set_klippy_state_sync(s);
        settle();
    }

    /// Moves the mock's last-send record off printer.restart, so the next
    /// restart_sent() answers for what happens after this point.
    void mark_sends() {
        client_.send_jsonrpc(
            "server.info", nlohmann::json::object(), [](const nlohmann::json&) {},
            [](const MoonrakerError&) {});
        REQUIRE_FALSE(restart_sent());
    }

    bool skips_removed() {
        const auto& deleted = api_.files_mock().deleted_files();
        return std::find(deleted.begin(), deleted.end(), "config/helix_skips.cfg") !=
                   deleted.end() &&
               api_.get_uploaded_config("printer.cfg").value_or("").find("helix_skips") ==
                   std::string::npos;
    }
};

} // namespace

TEST_CASE_METHOD(SkipsRollbackFixture,
                 "a Klipper error after loading the skips removes them and restarts once",
                 "[advanced][macros][skip_wrappers]") {
    klippy(KlippyState::STARTUP);
    klippy(KlippyState::ERROR);

    CHECK(skips_removed());
    CHECK(api_.get_uploaded_config("printer.cfg").value_or("").find("[include helix_macros.cfg]") !=
          std::string::npos);
    CHECK(restart_sent());

    SECTION("a second failure does not loop") {
        mark_sends();
        klippy(KlippyState::STARTUP);
        klippy(KlippyState::ERROR);
        CHECK_FALSE(restart_sent());
        CHECK(api_.files_mock().deleted_files().size() == 1);
    }
}

TEST_CASE_METHOD(SkipsRollbackFixture, "a clean load ends the watch",
                 "[advanced][macros][skip_wrappers]") {
    klippy(KlippyState::STARTUP);
    klippy(KlippyState::READY);
    klippy(KlippyState::ERROR); // a later, unrelated error

    CHECK_FALSE(skips_removed());
    CHECK_FALSE(restart_sent());
}

TEST_CASE_METHOD(SkipsRollbackFixture, "the READY the restart starts from is not a clean load",
                 "[advanced][macros][skip_wrappers]") {
    klippy(KlippyState::READY);
    klippy(KlippyState::STARTUP);
    klippy(KlippyState::ERROR);

    CHECK(skips_removed());
}
