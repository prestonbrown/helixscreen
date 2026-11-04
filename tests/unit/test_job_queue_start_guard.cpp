// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_start_guard.cpp
 * @brief A queued job starts through the detail view and dies only on success.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][queue_start]"
 *
 * PrintSelectPanel::start_queued_job() is the queue's single entry point (the
 * job queue modal's row tap, and the completion screen's Start next). It owns
 * three contracts:
 *
 * 1. The busy guard: can_start_new_print() covers BOTH the printer's reported
 *    state AND the app's committed-but-unconfirmed host-side start, so a tap
 *    during that window cannot delete the entry and lose the job.
 * 2. The entry outlives the attempt: remove_jobs and the stored-options delete
 *    run from the print-start SUCCESS callback, never from the tap. A missing
 *    file, a back-out and a failed start all leave the job queued.
 * 3. The saved option states seed the detail view's rows for the queued file
 *    (the merge rules themselves are pinned at the renderer level in
 *    test_print_select_detail_subjects.cpp and test_pre_print_options_renderer.cpp).
 *
 * The home tile's QueuedJobStarter keeps contract 2 through the same
 * finish_started_queued_job(); its own checks are in test_queued_job_start.cpp.
 */

#include "ui_job_queue_modal.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../test_helpers/job_queue_modal_test_access.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/queued_job_starter_test_access.h"
#include "../test_helpers/queued_start_fixture.h"
#include "../test_helpers/tile_boxes.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "job_queue_start.h"
#include "job_queue_state.h"
#include "moonraker_api.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "queued_job_options.h"
#include "queued_job_start.h"
#include "src/ui/panel_widgets/job_queue_widget.h"
#include "static_panel_registry.h"
#include "test_helpers/pre_print_option_sets.h"
#include "test_helpers/printer_state_test_access.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::JobQueueModal;
using helix::PlantedGcode;
using helix::PrintJobState;
using helix::queue::QueuedJobOptions;
using helix::queue::QueuedJobOptionsMap;
using helix::queue::QueuedJobStarter;
using helix::test::make_skip_and_addon_set;
using helix::test::set_wire_state;

using helix::test::QueuedStartFixture;
using helix::test::WarningLog;

TEST_CASE_METHOD(QueuedStartFixture,
                 "a queued job opens the detail view seeded and is removed only on confirmed start",
                 "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_happy.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}, {"timelapse", true}});

    panel_->start_queued_job(entry("0001", file.name()));
    drain();

    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    const std::string* pending = pending_job_id();
    REQUIRE(pending != nullptr);
    CHECK(*pending == "0001");

    // Saved states, not default_enabled, decide the rows for this file.
    const auto states = ::PrintSelectPanelTestAccess::collect_option_states(*panel_);
    REQUIRE(states.count("bed_mesh") == 1);
    CHECK(states.at("bed_mesh") == false);
    REQUIRE(states.count("timelapse") == 1);
    CHECK(states.at("timelapse") == true);

    // Nothing is removed until Moonraker confirms the print actually started:
    // opening the file, however far the pipeline gets, is not a start.
    CHECK(queue_has("0001"));
    REQUIRE(read_store().count("0001") == 1);

    ::PrintSelectPanelTestAccess::fire_print_started(*panel_);
    drain();

    CHECK_FALSE(queue_has("0001"));
    CHECK(read_store().empty());
    CHECK(pending_job_id() == nullptr);
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "a queued file missing from the listing warns and stays queued",
                 "[job_queue][queue_start]") {
    WarningLog warnings;

    panel_->start_queued_job(entry("0003", "queue_start_ghost.gcode"));
    drain();

    // The refusal is answered, and neither the queue entry nor a stale
    // pending start is left behind.
    CHECK(warnings.contains("File not found"));
    CHECK(warnings.contains("queue_start_ghost.gcode"));
    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0003"));
}

TEST_CASE_METHOD(QueuedStartFixture, "a queued job refuses to start while the printer is busy",
                 "[job_queue][queue_start][print_state]") {
    auto& ps = get_printer_state();

    SECTION("printing") {
        set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        set_wire_state(ps, PrintJobState::PAUSED);
    }
    SECTION("host-side pre-print block") {
        // The wire still reads standby here: the guard must consult the app's
        // committed start too, or the entry is deleted into a failed start.
        ps.print_state().begin_preparing(helix::PrintJobRef{"other.gcode", "", ""});
        drain();
        REQUIRE(ps.print_state().get_print_job_state() == PrintJobState::STANDBY);
        REQUIRE(ps.print_state().is_print_in_progress());
    }
    drain();

    WarningLog warnings;
    panel_->start_queued_job(entry("0002", "calibration_cube.gcode"));
    drain();

    CHECK(warnings.contains("calibration_cube.gcode"));
    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0002"));
    CHECK_FALSE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
}

TEST_CASE_METHOD(
    QueuedStartFixture,
    "closing the queued file's detail view drops the pending start once no start is in flight",
    "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_backout.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}});

    panel_->start_queued_job(entry("0001", file.name()));
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));

    SECTION("a Print tap in flight survives the close") {
        // A failed start re-shows the view; the pipeline closing it here must
        // not cancel the bookkeeping that removes the job on success.
        ::PrintSelectPanelTestAccess::set_pending_start_attempted(*panel_, true);
        ::PrintSelectPanelTestAccess::hide_detail_view(*panel_);
        drain();
        lv_timer_handler(); // the close callback runs on the next tick

        const std::string* pending = pending_job_id();
        REQUIRE(pending != nullptr);
        CHECK(*pending == "0001");
        CHECK(queue_has("0001"));
    }

    SECTION("a plain back-out clears the pending start") {
        ::PrintSelectPanelTestAccess::hide_detail_view(*panel_);
        drain();
        lv_timer_handler(); // the close callback runs on the next tick

        CHECK(pending_job_id() == nullptr);
        CHECK(queue_has("0001"));

        // Starting the same job again re-opens seeded: the saved states are
        // keyed to the job, not to whichever show happened first.
        panel_->start_queued_job(entry("0001", file.name()));
        drain();
        REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
        const auto states = ::PrintSelectPanelTestAccess::collect_option_states(*panel_);
        REQUIRE(states.count("bed_mesh") == 1);
        CHECK(states.at("bed_mesh") == false);
    }
}

TEST_CASE_METHOD(QueuedStartFixture, "opening a different file discards the pending queued start",
                 "[job_queue][queue_start]") {
    PlantedGcode queued_file("queue_start_diff_queued.gcode");
    PlantedGcode other_file("queue_start_diff_other.gcode");
    seed_store("0001", queued_file.name(), {{"bed_mesh", false}});

    panel_->start_queued_job(entry("0001", queued_file.name()));
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    REQUIRE(pending_job_id() != nullptr);

    // A start is in flight (so the close itself keeps the pending start), and
    // the user then picks a different file: the queued job's saved states
    // must not leak onto that file's rows, and the job stays queued for its
    // own row tap.
    ::PrintSelectPanelTestAccess::set_pending_start_attempted(*panel_, true);
    ::PrintSelectPanelTestAccess::hide_detail_view(*panel_);
    drain();
    lv_timer_handler(); // the close callback runs on the next tick
    REQUIRE(pending_job_id() != nullptr);

    REQUIRE(panel_->select_file_by_name(other_file.name()));
    drain();

    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0001"));
    const auto states = ::PrintSelectPanelTestAccess::collect_option_states(*panel_);
    REQUIRE(states.count("bed_mesh") == 1);
    CHECK(states.at("bed_mesh") == true);
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "the job queue modal delegates its row tap to the panel entry point",
                 "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_modal.gcode");

    // The process-global panel binds whichever fixture's api created it; a
    // case that runs after another one touched it would reuse a dead api, so
    // every case here retires it at both ends and lets its own tap recreate it.
    StaticPanelRegistry::instance().destroy_all();

    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "0002", file.name());
    drain();

    // The modal hides itself and routes the tap to the global print select
    // panel's start_queued_job(). That panel is created lazily and never
    // set up in tests (no XML, no file provider), so its file list is empty
    // and the pending start waits for a listing that never arrives - which
    // is exactly what makes this a clean delegation proof: the pending start
    // EXISTS on that panel, and the queue is untouched.
    const std::string* pending =
        ::PrintSelectPanelTestAccess::pending_queued_job_id(get_global_print_select_panel());
    REQUIRE(pending != nullptr);
    CHECK(*pending == "0002");
    CHECK(queue_has("0002"));

    StaticPanelRegistry::instance().destroy_all();
}

namespace {

/// A job_queue_row carrying @p job_id inside @p modal's dialog, which is how its
/// callbacks find the modal they act on.
lv_obj_t* make_queue_row(JobQueueModal& modal, const std::string& job_id,
                         const std::string& filename) {
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/components/job_queue_row.xml") ==
            LV_RESULT_OK);
    REQUIRE(modal.show(lv_screen_active()));
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(modal.dialog(), "job_queue_row", nullptr));
    REQUIRE(row != nullptr);
    JobQueueModalTestAccess::attach_row_data(row, job_id, filename);
    return row;
}

} // namespace

TEST_CASE_METHOD(QueuedStartFixture, "tapping a job queue row starts that row's job",
                 "[job_queue][queue_start][press_wash][1297]") {
    PlantedGcode file("queue_row_tap.gcode");
    StaticPanelRegistry::instance().destroy_all();

    const size_t panels_before = StaticPanelRegistry::instance().count();

    JobQueueModal modal;
    lv_obj_t* row = make_queue_row(modal, "0002", file.name());
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    drain();

    // The start created the global panel: the trash test's "nothing started"
    // probe below reads exactly this.
    CHECK(StaticPanelRegistry::instance().count() > panels_before);

    const std::string* pending =
        ::PrintSelectPanelTestAccess::pending_queued_job_id(get_global_print_select_panel());
    REQUIRE(pending != nullptr);
    CHECK(*pending == "0002");
    CHECK(queue_has("0002"));

    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "tapping a job queue row's trash icon removes that job and starts nothing",
                 "[job_queue][queue_start][press_wash][1297]") {
    StaticPanelRegistry::instance().destroy_all();
    REQUIRE(queue_has("0001"));

    // A start routes through the global print select panel, which is created
    // on first use and registers itself; with it retired above, a start would
    // show up as a new registry entry.
    const size_t panels_before = StaticPanelRegistry::instance().count();

    JobQueueModal modal;
    lv_obj_t* row = make_queue_row(modal, "0001", "3DBenchy.gcode");
    lv_obj_t* trash = lv_obj_find_by_name(row, "job_delete");
    REQUIRE(trash != nullptr);
    lv_obj_send_event(trash, LV_EVENT_CLICKED, nullptr);
    drain();

    CHECK_FALSE(queue_has("0001"));
    CHECK(StaticPanelRegistry::instance().count() == panels_before);

    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "start next routes the queue's front job through the panel entry point",
                 "[job_queue][queue_start]") {
    StaticPanelRegistry::instance().destroy_all();

    JobQueueState jqs(api_.get(), &mock_client_);
    set_job_queue_state(&jqs);
    // The ids/filenames mirror the mock's default queue (0001 3DBenchy,
    // 0002 xyz-10mm-calibration-cube), so queue_has() below reads the
    // entries the mock actually holds rather than ones only the cache
    // knows about.
    JobQueueStateTestAccess::set_jobs(
        jqs, {entry("0001", "3DBenchy.gcode"), entry("0002", "xyz-10mm-calibration-cube.gcode")});

    start_next_queued_job();
    drain();

    // Same delegation proof as the modal's row tap: the lazily-created global
    // panel is never set up here, so the observable is the pending start
    // existing for the FRONT job while the queue keeps every entry.
    const std::string* pending =
        ::PrintSelectPanelTestAccess::pending_queued_job_id(get_global_print_select_panel());
    REQUIRE(pending != nullptr);
    CHECK(*pending == "0001");
    CHECK(queue_has("0001"));
    CHECK(queue_has("0002"));

    set_job_queue_state(nullptr);
    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(QueuedStartFixture, "start next with an empty queue touches nothing",
                 "[job_queue][queue_start]") {
    StaticPanelRegistry::instance().destroy_all();

    JobQueueState jqs(api_.get(), &mock_client_);
    set_job_queue_state(&jqs);
    JobQueueStateTestAccess::set_jobs(jqs, {});

    start_next_queued_job();

    // The factory call creates the panel here (the no-op path never does), so
    // the read is against a live object, not the pre-call null singleton.
    CHECK(::PrintSelectPanelTestAccess::pending_queued_job_id(
              *get_print_select_panel(get_printer_state(), api_.get())) == nullptr);
    set_job_queue_state(nullptr);
    StaticPanelRegistry::instance().destroy_all();
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "leaving the panel before the queued file opens abandons the pending start",
                 "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_abandon.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}});

    // The pending start exists with no detail view open yet: the panel has
    // left for another screen before the saved-options load answered, which
    // is the state a navbar switch or go_back lands on.
    panel_->start_queued_job(entry("0001", file.name()));
    REQUIRE(pending_job_id() != nullptr);
    CHECK_FALSE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));

    panel_->on_deactivating(DeactivateReason::NavigateAway);
    drain();

    // A later visit's refresh must not reopen the queued file unasked, and
    // the entry stays queued with its options.
    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0001"));
    CHECK(read_store().count("0001") == 1);
}

TEST_CASE_METHOD(QueuedStartFixture, "a queued subdirectory file is found from the gcodes root",
                 "[job_queue][queue_start]") {
    PlantedGcode file("sub_file.gcode", "queue_start_subdir");
    seed_store("0001", file.relative(), {{"bed_mesh", false}});

    panel_->start_queued_job(entry("0001", file.relative()));
    drain();

    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    CHECK(::PrintSelectPanelTestAccess::composed_selected_filename(*panel_) == file.relative());
    const auto states = ::PrintSelectPanelTestAccess::collect_option_states(*panel_);
    REQUIRE(states.count("bed_mesh") == 1);
    CHECK(states.at("bed_mesh") == false);
}

TEST_CASE_METHOD(QueuedStartFixture, "a queued root file is found while browsing a subdirectory",
                 "[job_queue][queue_start]") {
    PlantedGcode nested("nested.gcode", "queue_start_browse");
    PlantedGcode root_file("queue_start_root.gcode");
    seed_store("0001", root_file.name(), {{"bed_mesh", false}});

    // Browse into a subdirectory first — the queued start must retarget the
    // listing back to the root, not match against what is on screen.
    panel_->navigate_to_directory("queue_start_browse");
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::list_contains(*panel_, nested.name()));

    panel_->start_queued_job(entry("0001", root_file.name()));
    drain();

    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    CHECK(::PrintSelectPanelTestAccess::composed_selected_filename(*panel_) == root_file.name());
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "two same-named files in different directories resolve to the queued one",
                 "[job_queue][queue_start]") {
    PlantedGcode first("dup.gcode", "queue_start_dup_one");
    PlantedGcode second("dup.gcode", "queue_start_dup_two");
    seed_store("0001", second.relative(), {{"bed_mesh", false}});

    // The listing the panel starts from shows the FIRST directory's file;
    // the queued filename names the second.
    panel_->navigate_to_directory("queue_start_dup_one");
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::list_contains(*panel_, first.name()));

    panel_->start_queued_job(entry("0001", second.relative()));
    drain();

    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    CHECK(::PrintSelectPanelTestAccess::composed_selected_filename(*panel_) == second.relative());
}

TEST_CASE_METHOD(QueuedStartFixture, "a refused queue removal keeps the job and its saved options",
                 "[job_queue][queue_start]") {
    WarningLog warnings;
    PlantedGcode file("queue_start_refused.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}});

    // Moonraker refuses the removal — a stale id after a restart, say. The
    // override answers error_cb where the stock handler succeeds.
    helix::MoonrakerClientMockTestAccess::set_method_handler(
        mock_client_, "server.job_queue.delete_job",
        [](MoonrakerClientMock*, const json&,
           helix::SharedCallback<void(const json&)> /*success_cb*/,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
            if (error_cb) {
                error_cb(MoonrakerError::unknown("refused", "server.job_queue.delete_job"));
            }
            return true;
        });

    panel_->start_queued_job(entry("0001", file.name()));
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    ::PrintSelectPanelTestAccess::fire_print_started(*panel_);
    drain();

    // The job stays queued AND keeps its options, so Start next can offer it
    // again exactly as it was saved; the refusal is surfaced, not just logged.
    CHECK(queue_has("0001"));
    CHECK(read_store().count("0001") == 1);
    CHECK(warnings.contains("Could not remove the job from the queue"));
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "closing the detail overlay via go_back drops an untapped pending start",
                 "[job_queue][queue_start]") {
    PlantedGcode file("queue_start_esc.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}});

    panel_->start_queued_job(entry("0001", file.name()));
    drain();
    REQUIRE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    REQUIRE(pending_job_id() != nullptr);

    // ESC and the Android back key pop the overlay through NavigationManager
    // alone - hide_detail_view() never runs, so on_activate() is where the
    // back-out bookkeeping has to land.
    REQUIRE(NavigationManager::instance().go_back());
    drain();
    lv_timer_handler(); // the close callback runs on the next tick

    CHECK(pending_job_id() == nullptr);
    CHECK(queue_has("0001"));
    CHECK(read_store().count("0001") == 1);
    CHECK_FALSE(::PrintSelectPanelTestAccess::detail_view_visible(*panel_));
}

// ============================================================================
// The home tile's starter shares the after-start bookkeeping
// ============================================================================

namespace {

/// Start @p job through @p starter with its option read held, so the start
/// is in flight, then report Moonraker confirming the print.
std::optional<QueuedJobStarter::Outcome> confirm_starter_start(QueuedStartFixture& f,
                                                               QueuedJobStarter& starter,
                                                               const helix::JobQueueEntry& job) {
    std::optional<QueuedJobStarter::Outcome> outcome;
    f.mock_client_.defer_next("server.database.get_item");
    starter.start(job, [&outcome](QueuedJobStarter::Outcome o) { outcome = o; });
    REQUIRE(starter.in_flight());
    PrintStartControllerTestAccess::fire_print_started(
        QueuedJobStarterTestAccess::controller(starter));
    QueuedStartFixture::drain();
    return outcome;
}

} // namespace

TEST_CASE_METHOD(QueuedStartFixture,
                 "the starter removes the job and its options once the print starts",
                 "[job_queue][queue_start]") {
    seed_store("0001", "3DBenchy.gcode", {{"bed_mesh", false}});
    QueuedJobStarter starter(get_printer_state(), api_.get());

    const auto outcome = confirm_starter_start(*this, starter, entry("0001", "3DBenchy.gcode"));

    REQUIRE(outcome.has_value());
    CHECK(*outcome == QueuedJobStarter::Outcome::Started);
    CHECK_FALSE(queue_has("0001"));
    CHECK(read_store().count("0001") == 0);
    CHECK_FALSE(starter.in_flight());
}

TEST_CASE_METHOD(QueuedStartFixture,
                 "a refused removal after a starter start keeps the job and its options",
                 "[job_queue][queue_start]") {
    WarningLog warnings;
    seed_store("0001", "3DBenchy.gcode", {{"bed_mesh", false}});
    helix::MoonrakerClientMockTestAccess::set_method_handler(
        mock_client_, "server.job_queue.delete_job",
        [](MoonrakerClientMock*, const json&,
           helix::SharedCallback<void(const json&)> /*success_cb*/,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
            if (error_cb) {
                error_cb(MoonrakerError::unknown("refused", "server.job_queue.delete_job"));
            }
            return true;
        });
    QueuedJobStarter starter(get_printer_state(), api_.get());

    const auto outcome = confirm_starter_start(*this, starter, entry("0001", "3DBenchy.gcode"));

    REQUIRE(outcome.has_value());
    CHECK(*outcome == QueuedJobStarter::Outcome::Started);
    CHECK(queue_has("0001"));
    CHECK(read_store().count("0001") == 1);
    CHECK(warnings.contains("Could not remove the job from the queue"));
}

// ============================================================================
// The home tile's start button
// ============================================================================

namespace {

/// The queue tile over a queue holding one planted job, with the process-wide
/// starter pointed at this fixture's print-select and put back afterwards.
class TileStartFixture : public helix::test::QueuedStartMockedFixture {
  public:
    TileStartFixture() {
        PanelWidgetManager::instance().init_widget_subjects();
        jqs.init_subjects();
        lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_job_queue"), 1);
        previous_prep_ = QueuedJobStarterTestAccess::swap_prep_source(
            helix::queue::queued_job_starter(),
            [this]() { return panel_->print_preparation_manager(); });
    }

    ~TileStartFixture() override {
        QueuedJobStarterTestAccess::swap_prep_source(helix::queue::queued_job_starter(),
                                                     previous_prep_);
        lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_job_queue"), 0);
    }

    /// Hand the tile a queue of just @p job, as Moonraker's answer would.
    void show_queue(const JobQueueEntry& job) {
        JobQueueStatus status;
        status.queue_state = "ready";
        status.queued_jobs.push_back(job);
        JobQueueStateTestAccess::deliver_status(jqs, status);
        settle();
    }

    /// Click @p button_name on a tile granted @p w x @p h and run the start to its end.
    void tap_start(int w, int h, const char* button_name) {
        PanelWidgetHarness<JobQueueWidget> tile(test_screen());
        tile.resize(1, 1, w, h);
        lv_obj_t* button = tile.child(button_name);
        REQUIRE(button != nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        CHECK(helix::queue::queued_job_starter().in_flight());
        REQUIRE(wait_until([] { return !helix::queue::queued_job_starter().in_flight(); }, 5000));
        settle();
    }

    JobQueueState jqs{nullptr, nullptr};
    helix::ScopedJobQueueSubjects subject_guard;
    helix::ScopedJobQueueState scoped_state{&jqs};

  private:
    QueuedJobStarterTestAccess::PrepSource previous_prep_;
};

} // namespace

TEST_CASE_METHOD(TileStartFixture,
                 "the tile's start button starts the front job through the shared starter",
                 "[job_queue][queue_start][widget_size]") {
    struct Button {
        int w;
        int h;
        const char* name;
    };
    const Button button = GENERATE(Button{test::kStrip.w, test::kStrip.h, "jq_action_icon"},
                                   Button{test::kCell.w, test::kCell.h, "jq_action"},
                                   Button{test::kWide.w, test::kWide.h, "jq_action"});
    INFO(button.name);

    PlantedGcode file("tile_start_button.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}});
    REQUIRE(queue_has("0001"));
    show_queue(entry("0001", file.name()));

    tap_start(button.w, button.h, button.name);

    CHECK(starts() == 1);
    CHECK_FALSE(queue_has("0001"));
    CHECK(read_store().count("0001") == 0);
    CHECK(mock_client_.call_count("server.job_queue.start") == 0);
    // The file view was not involved, and the tap did not open the queue modal.
    CHECK(pending_job_id() == nullptr);
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

TEST_CASE_METHOD(TileStartFixture, "the tile reads Starting with no button while its start runs",
                 "[job_queue][queue_start][widget_size]") {
    PlantedGcode file("tile_start_busy.gcode");
    show_queue(entry("0001", file.name()));
    PanelWidgetHarness<JobQueueWidget> tile(test_screen());
    tile.resize(1, 1, test::kCell.w, test::kCell.h);
    lv_obj_t* button = tile.child("jq_action");
    REQUIRE(button != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN));
    auto state = []() { return lv_subject_get_int(lv_xml_get_subject(nullptr, "jq_tile_state")); };

    mock_client_.defer_next("server.database.get_item"); // hold the saved-options read
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    settle();
    process_lvgl(30); // the tile refreshes on the next tick
    REQUIRE(helix::queue::queued_job_starter().in_flight());
    CHECK(state() == static_cast<int>(helix::JobQueueTileState::Starting));
    CHECK(lv_obj_has_flag(button, LV_OBJ_FLAG_HIDDEN));

    // The printer takes another job meanwhile, which ends the start.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    mock_client_.fire_deferred("server.database.get_item");
    REQUIRE(wait_until([] { return !helix::queue::queued_job_starter().in_flight(); }, 5000));
    settle();
    process_lvgl(30);
    CHECK_FALSE(jqs.start_in_flight());
    CHECK(state() == static_cast<int>(helix::JobQueueTileState::Printing));
}

TEST_CASE_METHOD(TileStartFixture, "a tap on the tile outside the button opens the queue modal",
                 "[job_queue][widget_size]") {
    PlantedGcode file("tile_tap_modal.gcode");
    show_queue(entry("0001", file.name()));
    PanelWidgetHarness<JobQueueWidget> tile(test_screen());
    tile.resize(1, 1, test::kCell.w, test::kCell.h);

    REQUIRE(ModalStack::instance().top_dialog() == nullptr);
    lv_obj_send_event(tile.root(), LV_EVENT_CLICKED, nullptr);
    settle();

    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    CHECK_FALSE(helix::queue::queued_job_starter().in_flight());
    Modal::hide(dialog);
    settle();
}

TEST_CASE_METHOD(helix::test::QueuedStartMockedFixture,
                 "Up next keeps the file-view path with the printer idle",
                 "[job_queue][queue_start]") {
    PlantedGcode file("up_next_file_view.gcode");
    seed_store("0001", file.name(), {{"bed_mesh", false}});
    JobQueueState jqs(nullptr, nullptr);
    helix::ScopedJobQueueSubjects subject_guard;
    helix::ScopedJobQueueState scoped_state(&jqs);
    jqs.init_subjects();
    JobQueueStatus status;
    status.queue_state = "ready";
    status.queued_jobs.push_back(entry("0001", file.name()));
    JobQueueStateTestAccess::deliver_status(jqs, status);

    helix::handle_up_next_tap();
    settle();

    const std::string* pending = ::PrintSelectPanelTestAccess::pending_queued_job_id(
        *get_print_select_panel(get_printer_state(), get_moonraker_api()));
    REQUIRE(pending != nullptr);
    CHECK(*pending == "0001");
    CHECK_FALSE(helix::queue::queued_job_starter().in_flight());
    CHECK(starts() == 0);
}
