// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_close_keeps_tree.cpp
 * @brief Closing print status decides, at that moment, whether its tree survives
 *
 * On a low-memory host the print status widget tree is destroyed when the
 * overlay closes, to give back its ~400-800KB. While a job holds the machine
 * that trade is wrong: the user comes back to a preview rebuilt from nothing.
 * These cases open the real overlay through PrintStatusPanel::push_overlay(),
 * close it through NavigationManager's close paths (back, navbar, overlay stack
 * clear; animations off), and read what the panel kept. A close callback that
 * runs a tick late is modelled by taking it off the widget and calling it after
 * the tree was pushed again.
 */

#include "ui_gcode_viewer.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "memory_utils.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

MemoryInfo low_memory_host() {
    MemoryInfo info;
    info.total_kb = 128 * 1024;
    info.available_kb = 18 * 1024;
    info.free_kb = 4 * 1024;
    return info;
}

MemoryInfo roomy_host() {
    MemoryInfo info;
    info.total_kb = 4 * 1024 * 1024;
    info.available_kb = 3 * 1024 * 1024;
    info.free_kb = 2 * 1024 * 1024;
    return info;
}

/// Swaps a private ring-buffer logger into spdlog's default slot for its
/// lifetime, so the lines a case produces can be read back.
class LogCapture {
  public:
    LogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(kCapacity)) {
        sink_->set_level(spdlog::level::trace);
        auto logger = std::make_shared<spdlog::logger>("print_status_capture", sink_);
        logger->set_level(spdlog::level::trace);
        original_ = spdlog::default_logger();
        spdlog::set_default_logger(logger);
    }

    ~LogCapture() {
        spdlog::set_default_logger(original_);
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    [[nodiscard]] std::vector<std::string> lines() const {
        return sink_->last_formatted(kCapacity);
    }

    /// The number of the last tree whose destruction line reads
    /// "destroyed: <cause_while>", or 0 when there is none. Tree numbers are
    /// process-wide, so cases read them back rather than assume them.
    [[nodiscard]] int destroyed_tree_number(const std::string& cause_while) const {
        const std::regex destroyed(R"(\[info\] \[PrintStatusPanel\] Print status tree #(\d+) )"
                                   R"(destroyed: )" +
                                   cause_while);
        int number = 0;
        for (const auto& line : lines()) {
            std::smatch m;
            if (std::regex_search(line, m, destroyed)) {
                number = std::stoi(m[1].str());
            }
        }
        return number;
    }

    [[nodiscard]] int count_containing(const std::string& text) const {
        int matches = 0;
        for (const auto& line : lines()) {
            if (line.find(text) != std::string::npos) {
                ++matches;
            }
        }
        return matches;
    }

  private:
    static constexpr size_t kCapacity = 4096;
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> original_;
};

/// Home as the active main panel, animations off, and a low-memory host unless
/// a case says otherwise.
class PrintStatusCloseFixture : public LVGLUITestFixture {
  public:
    PrintStatusCloseFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        NavigationManager::instance().set_panels(panels);

        use_memory(low_memory_host);
        set_wire_state(PrintJobState::STANDBY);
    }

    ~PrintStatusCloseFixture() override {
        // The cached tree is process-wide: leave none behind for the next case,
        // which starts from a torn-down panel the way a printer switch leaves it.
        NavigationManagerTestAccess::set_panel_stack(NavigationManager::instance(), {home_widget_});
        PrintStatusPanel::destroy_cached_overlay(
            helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
        drain();
        process_lvgl(20);
        set_wire_state(PrintJobState::STANDBY);
        use_memory(get_system_memory_info);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    static void set_wire_state(PrintJobState wire) {
        helix::test::set_wire_state(get_printer_state(), wire);
        drain();
    }

    static void use_memory(MemoryInfo (*source)()) {
        PrintStatusPanelTestAccess::set_memory_info_source(source);
    }

    static PrintStatusPanel& panel() {
        return get_global_print_status_panel();
    }

    static bool in_stack(lv_obj_t* tree) {
        return NavigationManager::instance().is_panel_in_stack(tree);
    }

    /// Open print status over Home and settle.
    lv_obj_t* open_print_status() {
        REQUIRE(PrintStatusPanel::push_overlay(test_screen()));
        drain();
        lv_obj_t* tree = PrintStatusPanel::get_cached_overlay();
        REQUIRE(tree != nullptr);
        REQUIRE(in_stack(tree));
        return tree;
    }

    /// Close it the way the back button does, then let the close callback and
    /// any deferred deletion run.
    void close_print_status(lv_obj_t* tree) {
        NavigationManager::instance().go_back();
        settle();
        REQUIRE_FALSE(in_stack(tree));
    }

    /// Run queued updates, then LVGL ticks: deferred close callbacks, deferred
    /// deletions, and the updates those queue.
    void settle() {
        drain();
        process_lvgl(20);
        drain();
    }

    /// The close callback print status registered on its last push, taken off
    /// the widget so the case can decide when it runs.
    static helix::OverlayCloseCallback take_close_callback(lv_obj_t* tree) {
        helix::OverlayCloseCallback callback =
            NavigationManagerTestAccess::take_overlay_close_callback(NavigationManager::instance(),
                                                                     tree);
        REQUIRE(callback);
        return callback;
    }

  private:
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A rebuilt print status is the one tree the next open shows",
                 "[print_status][destroy_on_close][1983]") {
    use_memory(roomy_host);
    lv_obj_t* old_root = open_print_status();
    const uint32_t screen_children = lv_obj_get_child_count(test_screen());
    close_print_status(old_root);

    REQUIRE(panel().rebuild());
    settle();
    lv_obj_t* rebuilt = panel().get_panel();
    REQUIRE(rebuilt != old_root);
    CHECK(PrintStatusPanel::get_cached_overlay() == rebuilt);

    CHECK(open_print_status() == rebuilt);
    CHECK(lv_obj_get_child_count(test_screen()) == screen_children);
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "Print status rebuilt on screen keeps its width and shows live progress",
                 "[print_status][destroy_on_close][1983]") {
    set_wire_state(PrintJobState::PRINTING);
    lv_obj_t* old_root = open_print_status();
    lv_obj_update_layout(test_screen());
    const int32_t width = lv_obj_get_width(old_root);

    REQUIRE(panel().rebuild());
    settle();

    lv_obj_t* rebuilt = panel().get_panel();
    REQUIRE(rebuilt != old_root);
    CHECK(in_stack(rebuilt));
    CHECK_FALSE(lv_obj_has_flag(rebuilt, LV_OBJ_FLAG_HIDDEN));
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_width(rebuilt) == width);

    nlohmann::json status;
    status["print_stats"]["state"] = "printing";
    status["virtual_sdcard"]["progress"] = 0.42;
    get_printer_state().update_from_status(status);
    drain();
    lv_obj_t* percent = lv_obj_find_by_name(rebuilt, "print_percent");
    REQUIRE(percent != nullptr);
    CHECK(std::string(lv_label_get_text(percent)) == "42%");
}

TEST_CASE_METHOD(PrintStatusCloseFixture, "A failed print status create leaves no tree to push",
                 "[print_status][destroy_on_close][1983]") {
    const bool strict = helix::ui::strict_ui_checks();
    helix::ui::set_strict_ui_checks(false);
    REQUIRE(lv_xml_register_component_from_data(
                "print_status_panel", "<component><view extends=\"lv_obj\"/></component>") ==
            LV_RESULT_OK);

    CHECK_FALSE(PrintStatusPanel::push_overlay(test_screen()));
    CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);

    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/print_status_panel.xml") == LV_RESULT_OK);
    helix::ui::set_strict_ui_checks(strict);
    settle();
    lv_obj_t* tree = open_print_status();
    CHECK(NavigationManagerTestAccess::lifecycle_of(NavigationManager::instance(), tree) !=
          nullptr);
}

TEST_CASE_METHOD(PrintStatusCloseFixture, "Print status decides from the print state at close",
                 "[print_status][destroy_on_close]") {
    // Opened with no job, so a decision made when the tree was created would
    // destroy it on this close.
    lv_obj_t* tree = open_print_status();
    set_wire_state(PrintJobState::PRINTING);
    close_print_status(tree);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == tree);

    // The kept tree comes back without re-creation, and its next close still
    // reaches the decision.
    REQUIRE(open_print_status() == tree);
    set_wire_state(PrintJobState::COMPLETE);
    close_print_status(tree);

    CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
    CHECK_FALSE(helix::test::widget_in_tree(tree));
}

TEST_CASE_METHOD(PrintStatusCloseFixture, "Print status decides from available memory at close",
                 "[print_status][destroy_on_close]") {
    set_wire_state(PrintJobState::COMPLETE);

    SECTION("memory runs low after the tree was created") {
        use_memory(roomy_host);
        lv_obj_t* tree = open_print_status();
        use_memory(low_memory_host);
        close_print_status(tree);

        CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
    }

    SECTION("memory frees up after the tree was created") {
        lv_obj_t* tree = open_print_status();
        use_memory(roomy_host);
        close_print_status(tree);

        CHECK(PrintStatusPanel::get_cached_overlay() == tree);
    }
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A navbar close deactivates the print status tree it keeps",
                 "[print_status][destroy_on_close][navigation]") {
    set_wire_state(PrintJobState::PRINTING);
    lv_obj_t* tree = open_print_status();
    REQUIRE(PrintStatusPanelTestAccess::is_active(panel()));
    lv_obj_t* viewer = PrintStatusPanelTestAccess::gcode_viewer(panel());
    REQUIRE(viewer != nullptr);
    // A print on screen has its viewer rendering.
    helix::ui::gcode_viewer_set_paused(viewer, false);

    NavigationManagerTestAccess::switch_to_panel(NavigationManager::instance(), PanelId::Home);
    settle();

    REQUIRE_FALSE(in_stack(tree));
    CHECK(PrintStatusPanel::get_cached_overlay() == tree);
    // A hidden viewer left rendering stalls its 2D catch-up, and the stall
    // watchdog then reports a failed preview load on whatever screen is showing.
    CHECK_FALSE(PrintStatusPanelTestAccess::is_active(panel()));
    CHECK(helix::ui::gcode_viewer_is_paused(viewer));
}

TEST_CASE_METHOD(PrintStatusCloseFixture, "A connection-loss close decides like any other close",
                 "[print_status][destroy_on_close][navigation]") {
    SECTION("a job holds the machine: kept") {
        set_wire_state(PrintJobState::PRINTING);
        lv_obj_t* tree = open_print_status();

        NavigationManagerTestAccess::clear_overlay_stack(NavigationManager::instance());
        settle();

        REQUIRE_FALSE(in_stack(tree));
        CHECK(PrintStatusPanel::get_cached_overlay() == tree);
        CHECK(helix::test::widget_in_tree(tree));
    }

    SECTION("no job: destroyed") {
        set_wire_state(PrintJobState::COMPLETE);
        lv_obj_t* tree = open_print_status();

        NavigationManagerTestAccess::clear_overlay_stack(NavigationManager::instance());
        settle();

        REQUIRE_FALSE(in_stack(tree));
        CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
        CHECK_FALSE(helix::test::widget_in_tree(tree));
    }
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A close callback landing after the tree was pushed again leaves it on screen",
                 "[print_status][destroy_on_close]") {
    // A slide-out close takes the widget's close callback when the animation
    // completes. Pushed again before then, the callback it takes is the one
    // that push registered, and it runs against the re-shown tree.
    auto reopen_with_late_close = [this](lv_obj_t* tree) {
        (void)take_close_callback(tree); // the slide-out has not completed yet
        close_print_status(tree);
        REQUIRE(open_print_status() == tree);
        helix::OverlayCloseCallback late_close = take_close_callback(tree);
        late_close();
        settle();
    };

    SECTION("no job: the tree stays, and its next close still decides") {
        set_wire_state(PrintJobState::COMPLETE);
        lv_obj_t* tree = open_print_status();
        reopen_with_late_close(tree);

        REQUIRE(PrintStatusPanel::get_cached_overlay() == tree);
        REQUIRE(in_stack(tree));
        CHECK(PrintStatusPanelTestAccess::is_active(panel()));

        close_print_status(tree);
        CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
    }

    SECTION("a job holds the machine: the tree stays active") {
        set_wire_state(PrintJobState::PRINTING);
        lv_obj_t* tree = open_print_status();
        reopen_with_late_close(tree);

        REQUIRE(PrintStatusPanel::get_cached_overlay() == tree);
        REQUIRE(in_stack(tree));
        CHECK(PrintStatusPanelTestAccess::is_active(panel()));
    }
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A tree kept through a close is released when the job ends while it is hidden",
                 "[print_status][destroy_on_close]") {
    set_wire_state(PrintJobState::PRINTING);
    lv_obj_t* tree = open_print_status();
    close_print_status(tree);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == tree);

    set_wire_state(PrintJobState::COMPLETE);
    settle();

    CHECK(PrintStatusPanel::get_cached_overlay() == nullptr);
    CHECK_FALSE(helix::test::widget_in_tree(tree));
}

TEST_CASE_METHOD(PrintStatusCloseFixture, "A job ending keeps a kept tree that is on screen",
                 "[print_status][destroy_on_close]") {
    set_wire_state(PrintJobState::PRINTING);
    lv_obj_t* tree = open_print_status();

    // The close's callback runs a tick late and lands after the tree was pushed
    // again, but before that push reached the navigation stack: the close
    // keeps the tree as a hidden one.
    helix::OverlayCloseCallback late_close = take_close_callback(tree);
    close_print_status(tree);
    REQUIRE(PrintStatusPanel::push_overlay(test_screen()));
    late_close();
    drain();
    REQUIRE(in_stack(tree));

    set_wire_state(PrintJobState::COMPLETE);
    settle();

    CHECK(PrintStatusPanel::get_cached_overlay() == tree);
    CHECK(helix::test::widget_in_tree(tree));
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A job ending keeps a hidden kept tree on a host with memory to spare",
                 "[print_status][destroy_on_close]") {
    use_memory(roomy_host);
    set_wire_state(PrintJobState::PRINTING);
    lv_obj_t* tree = open_print_status();
    close_print_status(tree);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == tree);

    set_wire_state(PrintJobState::COMPLETE);
    settle();

    CHECK(PrintStatusPanel::get_cached_overlay() == tree);
    CHECK(helix::test::widget_in_tree(tree));
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "Print status opened while a job-end release is queued keeps its tree",
                 "[print_status][destroy_on_close]") {
    // Kept on this close because memory was plentiful; by the time the job-end
    // release is queued, memory has run short.
    use_memory(roomy_host);
    set_wire_state(PrintJobState::COMPLETE);
    lv_obj_t* tree = open_print_status();
    close_print_status(tree);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == tree);

    use_memory(low_memory_host);
    PrintStatusPanelTestAccess::queue_job_end_release();
    // Opened before the release lands. The push is queued behind it, so the
    // tree is not in the navigation stack when the release checks.
    REQUIRE(PrintStatusPanel::push_overlay(test_screen()));
    settle();

    CHECK(PrintStatusPanel::get_cached_overlay() == tree);
    CHECK(in_stack(tree));
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A repeat print status tree logs WARN when a job holds the machine",
                 "[print_status][destroy_on_close][logging]") {
    set_wire_state(PrintJobState::COMPLETE);
    LogCapture capture;

    lv_obj_t* first = open_print_status();
    close_print_status(first);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == nullptr);
    set_wire_state(PrintJobState::PRINTING);
    open_print_status();

    const int first_number = capture.destroyed_tree_number("overlay close while Complete");
    REQUIRE(first_number > 0);

    const std::string expected =
        "[warning] [PrintStatusPanel] Print status tree #" + std::to_string(first_number + 1) +
        " created while Printing (18MB available); tree #" + std::to_string(first_number) +
        " was destroyed: overlay close while Complete";
    CAPTURE(expected);
    CHECK(capture.count_containing(expected) == 1);
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "Reopening print status with no job logs the rebuild at INFO",
                 "[print_status][destroy_on_close][logging]") {
    set_wire_state(PrintJobState::COMPLETE);

    // The capture starts after the first tree exists: how a tree built over
    // whatever an earlier case left behind is logged is not this case's subject.
    lv_obj_t* first = open_print_status();
    LogCapture capture;
    close_print_status(first);
    REQUIRE(PrintStatusPanel::get_cached_overlay() == nullptr);
    open_print_status();

    const int first_number = capture.destroyed_tree_number("overlay close while Complete");
    REQUIRE(first_number > 0);

    const std::string expected =
        "[info] [PrintStatusPanel] Print status tree #" + std::to_string(first_number + 1) +
        " created while Complete (18MB available); tree #" + std::to_string(first_number) +
        " was destroyed: overlay close while Complete";
    CAPTURE(expected);
    CHECK(capture.count_containing(expected) == 1);
    CHECK(capture.count_containing("[warning] [PrintStatusPanel] Print status tree #") == 0);
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "Pushing print status with the exclude map opens the map once the panel is active",
                 "[print_status][exclude]") {
    set_wire_state(PrintJobState::PRINTING);

    REQUIRE(PrintStatusPanel::push_overlay_with_exclude_map(test_screen()));
    settle();
    lv_obj_t* tree = PrintStatusPanel::get_cached_overlay();
    REQUIRE(tree != nullptr);
    REQUIRE(in_stack(tree));
    CHECK(PrintStatusPanelTestAccess::is_active(panel()));
    CHECK(PrintStatusPanelTestAccess::exclude_open(panel()));
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "A refused exclude-map push does not open the map on a later plain push",
                 "[print_status][exclude]") {
    set_wire_state(PrintJobState::PRINTING);

    CHECK_FALSE(PrintStatusPanel::push_overlay_with_exclude_map(nullptr));
    open_print_status();
    settle();
    CHECK_FALSE(PrintStatusPanelTestAccess::exclude_open(panel()));
}

TEST_CASE_METHOD(PrintStatusCloseFixture,
                 "An exclude-map request does not outlive its push when the tree is destroyed",
                 "[print_status][exclude]") {
    set_wire_state(PrintJobState::PRINTING);

    // Armed by a push, then the tree goes before the panel ever activates for it.
    REQUIRE(PrintStatusPanel::push_overlay_with_exclude_map(test_screen()));
    PrintStatusPanel::destroy_cached_overlay(
        helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
    settle();

    open_print_status();
    settle();
    CHECK(PrintStatusPanelTestAccess::is_active(panel()));
    CHECK_FALSE(PrintStatusPanelTestAccess::exclude_open(panel()));
}
