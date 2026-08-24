// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_detailed_visibility.cpp
 * @brief Integration test for Detailed vs Library visibility flow.
 *
 * Visibility of the five card-body siblings (print_card_idle/_compact/_detailed,
 * print_card_layout, print_card_printing_detailed) is driven by the
 * print_status_view int subject (0..4) via bind_flag_if_not_eq in the real XML.
 * These tests assert on the subject value the widget writes — the helix-xml
 * binding layer is verified elsewhere.
 */

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {
bool s_widgets_registered = false;
}

class PrintStatusDetailedVisibilityFixture : public LVGLTestFixture {
  public:
    PrintStatusDetailedVisibilityFixture() {
        if (!s_widgets_registered) {
            PanelWidgetManager::instance().init_widget_subjects();
            s_widgets_registered = true;
        }
        // Tear down the singleton formatter so the next PrintStatusWidget
        // ctor creates a fresh one with observers bound to the current
        // PrinterState (another test may have reset it).
        PrintStatusWidget::destroy_formatter_for_test();
    }
    ~PrintStatusDetailedVisibilityFixture() {
        PrintStatusWidget::destroy_formatter_for_test();
    }

    /// Minimal mock tree — just satisfies the names attach() looks up. Visibility
    /// is asserted via the print_status_view subject, not flags on these objects.
    /// Thumbnails MUST be lv_image_t — reset_print_card_to_idle calls
    /// lv_image_set_src on them at the end of attach().
    lv_obj_t* create_mock_tree(lv_obj_t* parent) {
        lv_obj_t* container = lv_obj_create(parent);
        auto add = [container](const char* name, bool as_image = false) {
            lv_obj_t* obj = as_image ? lv_image_create(container) : lv_obj_create(container);
            lv_obj_set_name(obj, name);
            return obj;
        };
        add("print_card_idle");
        add("print_card_thumb", /*as_image=*/true);
        add("print_card_idle_compact");
        add("print_card_idle_detailed");
        add("print_card_thumb_compact", /*as_image=*/true);
        add("print_card_printing");
        add("print_card_layout");
        add("print_card_thumb_wrap");
        add("print_card_active_thumb", /*as_image=*/true);
        add("print_card_info");
        add("print_card_preparing_info");
        add("print_card_printing_detailed");
        return container;
    }

    int view_value() {
        return lv_subject_get_int(PrintStatusWidget::view_subject_for_test());
    }
};

// view_subject_ values:
//   0 = idle_library_full   (print_card_idle)
//   1 = idle_library_compact (print_card_idle_compact)
//   2 = idle_detailed       (print_card_idle_detailed)
//   3 = active_library      (print_card_layout)
//   4 = active_detailed     (print_card_printing_detailed)
//
// Each test:
//   1. set_config + on_size_changed seeds layout_style and is_compact_.
//   2. attach() runs first-pass update_view_subject and queues observer cbs.
//   3. process_lvgl drains those queued cbs (so attach-time STANDBY observer
//      doesn't override our forced state later).
//   4. on_print_state_changed_for_test forces is_active_ (job_holds_machine
//      of the passed lifecycle) and rewrites the
//      view subject deterministically.

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "view=4 (active_detailed) when layout=detailed + colspan>=2 + PRINTING",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "detailed"}});
    w.on_size_changed(2, 2, 400, 400);
    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);
    w.on_print_state_changed_for_test(PrintState::Printing);
    REQUIRE(view_value() == 4);
    w.detach();
}

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "view=3 (active_library) when layout=library + colspan>=2 + PRINTING",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "library"}});
    w.on_size_changed(2, 2, 400, 400);
    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);
    w.on_print_state_changed_for_test(PrintState::Printing);
    REQUIRE(view_value() == 3);
    w.detach();
}

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "Switching layout_style at runtime flips view (library->detailed) while PRINTING",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "library"}});
    w.on_size_changed(2, 2, 400, 400);
    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);
    w.on_print_state_changed_for_test(PrintState::Printing);
    REQUIRE(view_value() == 3);
    w.set_config(nlohmann::json{{"layout_style", "detailed"}});
    REQUIRE(view_value() == 4);
    w.detach();
}

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "view=3 (Library) below the normal width floor even with detailed requested "
                 "while PRINTING",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "detailed"}});
    w.on_size_changed(1, 2, W_NORMAL - 1, 400);
    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);
    w.on_print_state_changed_for_test(PrintState::Printing);
    REQUIRE(view_value() == 3);
    w.detach();
}

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "view=2 (idle_detailed) when layout=detailed + colspan>=2 + IDLE",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "detailed"}});
    w.on_size_changed(2, 2, 400, 400);
    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);
    w.on_print_state_changed_for_test(PrintState::Idle);
    REQUIRE(view_value() == 2);
    w.detach();
}

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "view=1 (idle_library_compact) below the normal width floor regardless of "
                 "layout_style",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "detailed"}});
    w.on_size_changed(1, 2, W_NORMAL - 1, 400);
    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);
    w.on_print_state_changed_for_test(PrintState::Idle);
    REQUIRE(view_value() == 1);
    w.detach();
}
