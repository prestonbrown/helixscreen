// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_job_queue_tile.cpp
 * @brief The job queue tile draws what its ladder promises in each state at each rung:
 * the published verdict and texts, which parts carry HIDDEN, and the re-decision when
 * the queue, the run state or the printer's activity changes.
 *
 * Boxes are the medium-tier home grid's tiles: 1x0.5, 1x1, 2x1 and 2x2 cells. The decision is
 * job_queue_tile_layout.h's and the state is job_queue_tile_state.h's; these tests pin their wiring
 * to the real fonts and XML.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][job_queue]"
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "../test_helpers/job_queue_tile_seed.h"
#include "../test_helpers/one_line_label.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/tile_boxes.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "job_queue_state.h"
#include "lvgl.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/job_queue_widget.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "xml_registration.h"

#include <spdlog/fmt/fmt.h>

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

using test::Box;
using test::kCell;
using test::kFull;
using test::kStrip;
using test::kWide;

struct JobQueueTileFixture : public LVGLUITestFixture, public JobQueueTileSeed {
    JobQueueTileFixture() : JobQueueTileSeed([this] { settle(); }) {
        // The boxes are the medium tier's. A component registers its fonts and px
        // tokens once, so one left registered at another geometry is dropped here and
        // registers again at this one.
        theme_manager_refresh_layout_constants(lv_display_get_default());
        helix::unregister_idle_xml_components();
    }
    ~JobQueueTileFixture() override {
        SystemSettingsManager::instance().set_language("en");
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    void settle() {
        drain();
        process_lvgl(30);
    }
    void set_estimate(int seconds) {
        FileMetadata metadata;
        metadata.estimated_time = seconds;
        JobQueueStateTestAccess::deliver_next_metadata(jqs, "file-0.gcode", metadata);
        settle();
    }
};

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

std::string subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

/// Whether an object draws: it and every ancestor up to the widget root is unhidden.
bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
            return false;
    }
    return true;
}

bool shown(PanelWidgetHarness<JobQueueWidget>& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return shown(obj);
}

/// What the tile draws, as booleans, for comparing against a state's expectation.
struct Parts {
    bool label_row = false;
    bool thumb = false;
    bool detail = false;
    bool more = false;
    bool action_icon = false;
    bool action = false;
    int list_rows = 0;

    bool operator==(const Parts& o) const {
        return label_row == o.label_row && thumb == o.thumb && detail == o.detail &&
               more == o.more && action_icon == o.action_icon && action == o.action &&
               list_rows == o.list_rows;
    }
};

std::ostream& operator<<(std::ostream& os, const Parts& p) {
    return os << "{label_row=" << p.label_row << " thumb=" << p.thumb << " detail=" << p.detail
              << " more=" << p.more << " icon=" << p.action_icon << " action=" << p.action
              << " rows=" << p.list_rows << "}";
}

Parts parts_of(PanelWidgetHarness<JobQueueWidget>& h) {
    Parts p;
    p.label_row = shown(h, "jq_label");
    p.thumb = shown(h, "jq_thumb");
    p.detail = shown(h, "jq_detail") || shown(h, "jq_cell_next");
    p.more = shown(h, "jq_more");
    p.action_icon = shown(h, "jq_action_icon");
    p.action = shown(h, "jq_action");
    for (int i = 0; i < kJobQueueTileListRows; ++i) {
        if (shown(h, fmt::format("jq_after_{}", i).c_str()))
            ++p.list_rows;
    }
    return p;
}

/// The expected parts and rung of one state in the Strip, Cell, Wide and Full boxes, in
/// that order. A state whose content is small enough draws a larger rung in a smaller box.
struct Expect {
    Parts at[4];
    SizeRung rung[4] = {SizeRung::Strip, SizeRung::Cell, SizeRung::Wide, SizeRung::Full};
};

/// @p check_rung is false for the quiet states, whose rung follows their caption's width.
void check_parts(PanelWidgetHarness<JobQueueWidget>& h, const Expect& expect,
                 bool check_rung = true) {
    const Box boxes[] = {kStrip, kCell, kWide, kFull};
    for (int i = 0; i < 4; ++i) {
        INFO(boxes[i].w << "x" << boxes[i].h);
        h.resize(1, 1, boxes[i].w, boxes[i].h);
        if (check_rung)
            CHECK(subject_int("jq_tile_rung") == static_cast<int>(expect.rung[i]));
        CHECK(parts_of(h) == expect.at[i]);
    }
}

constexpr Parts parts(bool label_row, bool thumb, bool detail, bool more, bool icon, bool action,
                      int rows) {
    return Parts{label_row, thumb, detail, more, icon, action, rows};
}

} // namespace

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: medium-tier budgets land on their rung",
                 "[widget_size][job_queue]") {
    deliver(3);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    for (const Box& b : {kStrip, kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(subject_int("jq_tile_rung") == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: a waiting job at each rung",
                 "[widget_size][job_queue]") {
    deliver(3);
    set_estimate(3600);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::WaitingForStart));
    CHECK(subject_int("jq_tile_action") == static_cast<int>(JobQueueTileAction::StartNext));
    CHECK(subject_text("jq_tile_hero") == "file-0");
    CHECK(subject_text("jq_tile_detail") == "1h");
    CHECK(subject_text("jq_tile_more") == "+2 more");
    CHECK(subject_text("jq_tile_count") == "3");
    // Full lists both following jobs, so "+N more" has nothing left to count.
    check_parts(h, {{parts(false, false, false, false, true, false, 0),
                     parts(false, false, false, true, false, true, 0),
                     parts(false, true, true, true, false, true, 0),
                     parts(false, true, true, false, false, true, 2)}});
    h.resize(1, 1, kStrip.w, kStrip.h);
    CHECK(shown(h, "jq_count"));
    CHECK_FALSE(shown(h, "jq_hero"));
    h.resize(1, 1, kCell.w, kCell.h);
    CHECK(subject_text("jq_tile_action_text") == "Start next");
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_text("jq_tile_action_text") == "Bed clear · Start next");
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: the next job starts on its own while printing",
                 "[widget_size][job_queue]") {
    set_automatic(true);
    deliver(3);
    set_print_active(true);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::AutoNext));
    CHECK(subject_int("jq_tile_action") == static_cast<int>(JobQueueTileAction::None));
    CHECK(subject_text("jq_tile_hero") == "3 queued");
    CHECK(subject_text("jq_tile_detail") == "Next: file-0");
    check_parts(h, {{parts(false, false, true, false, false, false, 0),
                     parts(false, false, true, false, false, false, 0),
                     parts(false, false, true, false, false, false, 0),
                     parts(false, false, true, false, false, false, 2)},
                    // "N queued" over the next name fits the Strip box.
                    {SizeRung::Cell, SizeRung::Cell, SizeRung::Wide, SizeRung::Full}});
    for (const Box& b : {kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(shown(h, "jq_auto"));
    }
    // The Strip box's Cell has no line left for it.
    h.resize(1, 1, kStrip.w, kStrip.h);
    CHECK_FALSE(shown(h, "jq_auto"));
    CHECK(shown(h, "jq_hero"));
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: printing shows what is next",
                 "[widget_size][job_queue]") {
    deliver(3);
    set_print_active(true);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::Printing));
    CHECK(subject_text("jq_tile_hero") == "3 queued");
    CHECK(subject_text("jq_tile_detail") == "Next: file-0");
    check_parts(h, {{parts(false, false, true, false, false, false, 0),
                     parts(false, false, true, false, false, false, 0),
                     parts(false, false, true, false, false, false, 0),
                     parts(false, false, true, false, false, false, 2)},
                    // "N queued" over the next name fits the Strip box.
                    {SizeRung::Cell, SizeRung::Cell, SizeRung::Wide, SizeRung::Full}});
    CHECK_FALSE(shown(h, "jq_auto"));
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: a box's rung does not depend on the rung shown before it",
                 "[widget_size][job_queue]") {
    // Full shows the list under the body, Wide the body alone, so the parts visible when
    // the Strip box is decided differ.
    deliver(3);
    set_print_active(true);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(shown(h, "jq_after_0"));
    h.resize(1, 1, kStrip.w, kStrip.h);
    const int after_full = subject_int("jq_tile_rung");
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE_FALSE(shown(h, "jq_after_0"));
    h.resize(1, 1, kStrip.w, kStrip.h);
    CHECK(after_full == subject_int("jq_tile_rung"));
    CHECK(after_full == static_cast<int>(SizeRung::Cell));
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: a job being loaded shows its thumbnail",
                 "[widget_size][job_queue]") {
    deliver(2, "loading");
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::Starting));
    CHECK(subject_text("jq_tile_hero") == "Starting file-0…");
    check_parts(h, {{parts(false, false, false, false, false, false, 0),
                     parts(false, false, false, false, false, false, 0),
                     parts(false, true, false, false, false, false, 0),
                     parts(false, true, false, false, false, false, 1)}});
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: automatic transition does not hide the button idle",
                 "[widget_size][job_queue]") {
    set_automatic(true);
    for (const char* queue_state : {"paused", "ready"}) {
        INFO(queue_state);
        deliver(3, queue_state);
        PanelWidgetHarness<JobQueueWidget> h(test_screen());
        h.resize(1, 1, kCell.w, kCell.h);
        CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::WaitingForStart));
        CHECK(subject_int("jq_tile_action") == static_cast<int>(JobQueueTileAction::StartNext));
        CHECK(shown(h, "jq_action"));
    }
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: an empty queue says so and offers nothing",
                 "[widget_size][job_queue]") {
    deliver(0);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::Empty));
    CHECK(subject_text("jq_tile_hero") == "Queue empty");
    CHECK(shown(h, "jq_quiet_text"));
    CHECK_FALSE(shown(h, "jq_body"));
    check_parts(h,
                {{parts(false, false, false, false, false, false, 0),
                  parts(false, false, false, false, false, false, 0),
                  parts(false, false, false, false, false, false, 0),
                  parts(false, false, false, false, false, false, 0)}},
                false);
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: before the first answer only the label shows",
                 "[widget_size][job_queue]") {
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::NotLoaded));
    CHECK(subject_text("jq_tile_hero").empty());
    check_parts(h,
                {{parts(true, false, false, false, false, false, 0),
                  parts(true, false, false, false, false, false, 0),
                  parts(true, false, false, false, false, false, 0),
                  parts(true, false, false, false, false, false, 0)}},
                false);
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: without the component it says so",
                 "[widget_size][job_queue]") {
    set_component_available(false);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::Unavailable));
    CHECK(subject_text("jq_tile_hero") == "Job queue not enabled in Moonraker");
    CHECK(subject_int("jq_tile_action") == static_cast<int>(JobQueueTileAction::None));
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: a queue state change redraws without a count change",
                 "[widget_size][job_queue]") {
    set_automatic(true);
    set_print_active(true);
    deliver(3, "paused");
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::Printing));
    CHECK(subject_text("jq_tile_detail") == "Next: file-0");

    deliver(3, "ready");
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::AutoNext));
    CHECK(subject_int("jq_tile_show_auto") == 1);
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: print end turns the automatic next into the Start button",
                 "[widget_size][job_queue]") {
    set_automatic(true);
    set_print_active(true);
    deliver(3, "ready");
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kCell.w, kCell.h);
    REQUIRE(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::AutoNext));
    CHECK(subject_int("jq_tile_action") == 0);

    set_print_active(false);
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::WaitingForStart));
    CHECK(subject_int("jq_tile_action") == 1);
    CHECK(shown(h, "jq_action"));
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: a 90-character filename stays on one line above its button",
                 "[widget_size][job_queue]") {
    JobQueueStatus status;
    status.queue_state = "ready";
    status.queued_jobs.push_back({"job-1", std::string(90, 'a') + ".gcode", 1000.0, 30.0});
    JobQueueStateTestAccess::deliver_status(jqs, status);
    settle();

    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    lv_area_t content;
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE(subject_int("jq_tile_rung") == static_cast<int>(SizeRung::Wide));
    lv_obj_t* hero = h.child("jq_hero");
    REQUIRE(hero != nullptr);
    CHECK(helix::test::held_to_one_line(hero));
    lv_obj_get_content_coords(h.root(), &content);
    lv_area_t box;
    lv_obj_get_coords(hero, &box);
    CHECK(box.x2 <= content.x2);

    // A Cell wraps the name to two lines and cuts the rest with an ellipsis.
    h.resize(1, 1, kCell.w, kCell.h);
    REQUIRE(subject_int("jq_tile_rung") == static_cast<int>(SizeRung::Cell));
    lv_obj_t* name = h.child("jq_cell_name");
    REQUIRE(name != nullptr);
    REQUIRE(shown(name));
    CHECK_FALSE(shown(h, "jq_hero"));
    const std::string cut = subject_text("jq_tile_cell_name");
    const std::string ellipsis = "…";
    REQUIRE(cut.size() > ellipsis.size());
    CHECK(cut.substr(cut.size() - ellipsis.size()) == ellipsis);
    const lv_font_t* font = lv_obj_get_style_text_font(name, LV_PART_MAIN);
    CHECK(lv_obj_get_height(name) == 2 * lv_font_get_line_height(font));
    lv_obj_get_coords(name, &box);
    lv_obj_get_content_coords(h.root(), &content);
    CHECK(box.y2 <= content.y2);

    h.resize(1, 1, kStrip.w, kStrip.h);
    lv_obj_t* icon = h.child("jq_action_icon");
    REQUIRE(icon != nullptr);
    REQUIRE(shown(icon));
    lv_obj_get_content_coords(h.root(), &content);
    lv_area_t icon_box;
    lv_obj_get_coords(icon, &icon_box);
    CHECK(icon_box.x2 <= content.x2);
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: a dropped socket keeps the last known queue",
                 "[widget_size][job_queue]") {
    deliver(3);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::WaitingForStart));
    const std::string hero = subject_text("jq_tile_hero");

    jqs.invalidate();
    settle();
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::WaitingForStart));
    CHECK(subject_text("jq_tile_hero") == hero);
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: the following jobs list shows the rows that fit at Full",
                 "[widget_size][job_queue]") {
    deliver(5);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(subject_text("jq_tile_after_0") == "file-1");
    CHECK(subject_text("jq_tile_after_1") == "file-2");
    CHECK(subject_text("jq_tile_after_2") == "file-3");
    // The rows the tile does not have the height for are counted instead.
    const int rows = parts_of(h).list_rows;
    CHECK(rows >= 1);
    CHECK(subject_int("jq_tile_list_rows") == rows);
    const int unseen = 4 - rows;
    CHECK(shown(h, "jq_more") == (unseen > 0));
    if (unseen > 0) {
        CHECK(subject_text("jq_tile_more") == fmt::format("+{} more", unseen));
    }
    lv_obj_t* name = lv_obj_find_by_name(h.child("jq_after_0"), "job_filename");
    REQUIRE(name != nullptr);
    CHECK(std::string(lv_label_get_text(name)) == "file-1");

    deliver(2);
    CHECK(parts_of(h).list_rows == 1);
    CHECK(shown(h, "jq_after_0"));
    CHECK_FALSE(shown(h, "jq_after_1"));
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: jobs that change without changing the count are renamed",
                 "[widget_size][job_queue]") {
    deliver(3);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(subject_text("jq_tile_hero") == "file-0");
    REQUIRE(subject_text("jq_tile_after_0") == "file-1");

    // The following jobs swap places: the same first job, three jobs before and after.
    JobQueueStatus status = status_with(3);
    std::swap(status.queued_jobs[1], status.queued_jobs[2]);
    JobQueueStateTestAccess::deliver_status(jqs, status);
    settle();
    CHECK(subject_text("jq_tile_hero") == "file-0");
    CHECK(subject_text("jq_tile_after_0") == "file-2");
    CHECK(subject_text("jq_tile_after_1") == "file-1");

    // Another job moves to the front.
    std::swap(status.queued_jobs[0], status.queued_jobs[2]);
    JobQueueStateTestAccess::deliver_status(jqs, status);
    settle();
    CHECK(subject_text("jq_tile_hero") == "file-1");
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: its own start reads Starting and takes the button away",
                 "[widget_size][job_queue]") {
    deliver(3);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kCell.w, kCell.h);
    REQUIRE(shown(h, "jq_action"));

    jqs.set_start_in_flight(true);
    settle();
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::Starting));
    CHECK(subject_int("jq_tile_action") == static_cast<int>(JobQueueTileAction::None));
    CHECK(subject_text("jq_tile_hero") == "Starting file-0…");
    CHECK_FALSE(shown(h, "jq_action"));

    jqs.set_start_in_flight(false);
    settle();
    CHECK(subject_int("jq_tile_state") == static_cast<int>(JobQueueTileState::WaitingForStart));
    CHECK(shown(h, "jq_action"));
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: a language switch recomposes the text",
                 "[widget_size][job_queue][i18n]") {
    deliver(3);
    set_print_active(true);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    REQUIRE(subject_text("jq_tile_hero") == "3 queued");

    SystemSettingsManager::instance().set_language("de");
    settle();
    const std::string german = fmt::format(lv_tr("{} queued"), 3);
    CHECK(german != "3 queued");
    CHECK(subject_text("jq_tile_hero") == german);

    SystemSettingsManager::instance().set_language("en");
    settle();
    CHECK(subject_text("jq_tile_hero") == "3 queued");
}

TEST_CASE_METHOD(JobQueueTileFixture, "job queue tile: fits_at refuses below the strip",
                 "[widget_size][job_queue]") {
    JobQueueWidget probe;
    CHECK_FALSE(probe.fits_at(60, 30));
    CHECK(probe.fits_at(kStrip.w, kStrip.h));
}

TEST_CASE_METHOD(JobQueueTileFixture,
                 "job queue tile: a Cell names the next job alone under the count while printing",
                 "[widget_size][job_queue]") {
    set_automatic(true);
    deliver(3);
    set_print_active(true);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());
    h.resize(1, 1, kCell.w, kCell.h);
    REQUIRE(subject_int("jq_tile_rung") == static_cast<int>(SizeRung::Cell));
    CHECK(subject_text("jq_tile_hero") == "3 queued");
    CHECK(subject_text("jq_tile_cell_name") == "file-0");
    CHECK(shown(h, "jq_cell_next"));
    CHECK_FALSE(shown(h, "jq_detail"));
    CHECK(shown(h, "jq_auto"));

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(subject_text("jq_tile_cell_name").empty());
    CHECK(shown(h, "jq_detail"));
    CHECK_FALSE(shown(h, "jq_cell_next"));
    CHECK(subject_text("jq_tile_detail") == "Next: file-0");
}
