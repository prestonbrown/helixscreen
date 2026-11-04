// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_component_keypad.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_text_entry_modal.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/plugin_test_support.h"
#include "core/update_queue.h"
#include "lua_bindings.h"
#include "plugin_overlay_host.h"
#include "settings_manager.h"

#include <array>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

struct EditorFixture : LVGLUITestFixture {
    EditorFixture() {
        SettingsManager::instance().init_subjects();
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
    }

    ~EditorFixture() override {
        ModalStack::instance().clear();
        NavigationManager::instance().shutdown();
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Runs queued work and the next-tick callbacks it schedules (overlay close hooks).
    void drain() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        helix::ui::UpdateQueue::instance().drain();
    }

    static lv_obj_t* keypad() {
        return lv_obj_find_by_name(lv_screen_active(), "keypad_panel");
    }

    void click(lv_obj_t* root, const char* name) {
        lv_obj_t* o = lv_obj_find_by_name(root, name);
        REQUIRE(o);
        lv_obj_send_event(o, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    /// Types `digits` ('0'-'9', '.' and '-' for the sign key) on the open keypad, then
    /// presses Set.
    void type_and_confirm(const std::string& digits) {
        lv_obj_t* pad = keypad();
        REQUIRE(pad);
        for (char c : digits) {
            const std::string name = c == '.'   ? "btn_dot"
                                     : c == '-' ? "btn_sign"
                                                : std::string("btn_") + c;
            click(pad, name.c_str());
        }
        click(pad, "action_button");
    }

    static lv_obj_t* text_dialog() {
        lv_obj_t* top = Modal::get_top();
        return top ? lv_obj_find_by_name(top, "text_entry_input") : nullptr;
    }
};

const char* kNumber = R"(
    got, cancels = nil, 0
    function open_number(extra)
        local opts = {title = "Rate", min = 0, max = 200, decimals = 1, unit = "mm/s",
                      on_accept = function(v) got = v end,
                      on_cancel = function() cancels = cancels + 1 end}
        for k, v in pairs(extra or {}) do opts[k] = v end
        helix.ui.edit_number(opts)
    end
)";

} // namespace

TEST_CASE_METHOD(EditorFixture, "helix.ui.edit_number returns the keypad's value",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(kNumber));
    REQUIRE(b.t.run("open_number()"));
    drain();
    REQUIRE(ui_keypad_is_visible());
    type_and_confirm("12.5");
    CHECK_FALSE(ui_keypad_is_visible());
    CHECK(b.t.global("got") == "12.5");
    CHECK(b.t.global("cancels") == "0");

    // decimals = 0 hands back an integer.
    REQUIRE(b.t.run("open_number({decimals = 0})"));
    drain();
    type_and_confirm("42");
    CHECK(b.t.global("got") == "42");
    REQUIRE(b.t.run("is_int = math.type(got) == 'integer'"));
    CHECK(b.t.global("is_int") == "true");
}

TEST_CASE_METHOD(EditorFixture, "helix.ui.edit_number with a negative min enters a negative value",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(kNumber));
    REQUIRE(b.t.run("open_number({min = -10, max = 10})"));
    drain();
    REQUIRE(ui_keypad_is_visible());
    lv_obj_t* sign = lv_obj_find_by_name(keypad(), "btn_sign");
    REQUIRE(sign);
    CHECK_FALSE(lv_obj_has_flag(sign, LV_OBJ_FLAG_HIDDEN));
    type_and_confirm("2.5-");
    CHECK_FALSE(ui_keypad_is_visible());
    CHECK(b.t.global("got") == "-2.5");

    // A range that stays at or above zero gets no sign key.
    REQUIRE(b.t.run("open_number()"));
    drain();
    REQUIRE(ui_keypad_is_visible());
    CHECK(lv_obj_has_flag(sign, LV_OBJ_FLAG_HIDDEN));
    ui_keypad_hide();
    drain();
}

TEST_CASE_METHOD(EditorFixture, "helix.ui.edit_number validation keeps the keypad open",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(kNumber));
    REQUIRE(b.t.run(R"(
        seen = {}
        open_number({validate = function(v)
            seen[#seen + 1] = v
            if v > 100 then return false, "Too fast for this printer" end
            return true
        end})
    )"));
    drain();
    helix::LogCapture logs;
    type_and_confirm("150");
    CHECK(ui_keypad_is_visible());
    CHECK(b.t.global("got") == "nil");
    CHECK(logs.count_containing("Too fast for this printer") >= 1);

    // Out of the declared range never reaches the validator.
    lv_obj_t* pad = keypad();
    for (int i = 0; i < 3; ++i)
        click(pad, "btn_backspace");
    type_and_confirm("250");
    CHECK(ui_keypad_is_visible());
    REQUIRE(b.t.run("n = #seen"));
    CHECK(b.t.global("n") == "1");

    for (int i = 0; i < 3; ++i)
        click(pad, "btn_backspace");
    type_and_confirm("80");
    CHECK_FALSE(ui_keypad_is_visible());
    CHECK(b.t.global("got") == "80.0");
}

TEST_CASE_METHOD(EditorFixture, "a validator with side effects or errors rejects",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(kNumber));
    REQUIRE(
        b.t.run(R"(open_number({validate = function(v) helix.ui.toast("x") return true end}))"));
    drain();
    helix::LogCapture logs;
    type_and_confirm("5");
    CHECK(ui_keypad_is_visible());
    CHECK(b.t.global("got") == "nil");
    CHECK(logs.has_line_with({"helix.ui.toast", "not allowed in an editor validator"}));
    ui_keypad_hide();
    drain();
    CHECK(b.t.global("cancels") == "1");
    CHECK_FALSE(b.t.rt->faulted());
}

TEST_CASE_METHOD(EditorFixture, "closing the keypad without a value runs on_cancel once",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(kNumber));
    REQUIRE(b.t.run("open_number()"));
    drain();
    REQUIRE(ui_keypad_is_visible());
    // A second editor while one is open is refused.
    CHECK_FALSE(b.t.run("open_number()"));
    ui_keypad_hide();
    drain();
    CHECK(b.t.global("cancels") == "1");
    CHECK(b.t.global("got") == "nil");
    // The slot is free again.
    REQUIRE(b.t.run("open_number()"));
    drain();
    CHECK(ui_keypad_is_visible());
    ui_keypad_hide();
    drain();
    CHECK(b.t.global("cancels") == "2");
}

TEST_CASE_METHOD(EditorFixture, "unloading a plugin closes its keypad and runs nothing",
                 "[plugin][lua_bindings_editor]") {
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(kNumber));
        REQUIRE(b.t.run("open_number()"));
        drain();
        REQUIRE(ui_keypad_is_visible());
    }
    drain();
    CHECK_FALSE(ui_keypad_is_visible());
    // The app's own keypad use is untouched by the plugin's former config.
    int confirms = 0;
    ui_keypad_config_t config{};
    config.min_value = 0;
    config.max_value = 300;
    config.on_confirm = [&confirms](float) { ++confirms; };
    ui_keypad_show(&config);
    drain();
    type_and_confirm("7");
    CHECK(confirms == 1);
}

TEST_CASE_METHOD(EditorFixture, "helix.ui.edit_number validates its options",
                 "[plugin][lua_bindings_editor]") {
    const char* bad[] = {
        "helix.ui.edit_number()",
        "helix.ui.edit_number({min = 0, max = 10})",
        "helix.ui.edit_number({max = 10, on_accept = print})",
        "helix.ui.edit_number({min = -1e6, max = 10, on_accept = print})",
        "helix.ui.edit_number({min = 10, max = 10, on_accept = print})",
        "helix.ui.edit_number({min = 0, max = 1e6, on_accept = print})",
        "helix.ui.edit_number({min = 0, max = 10, decimals = 3, on_accept = print})",
        "helix.ui.edit_number({min = 0, max = 10, on_accept = 5})",
        "helix.ui.edit_number({min = 0, max = 10, on_accept = print, validate = 'x'})",
        "helix.ui.edit_number({min = 0, max = 10, on_accept = print, colour = 'red'})",
        "helix.ui.edit_number({min = 0, max = 10, on_accept = print, title = string.rep('x', 48)})",
        "helix.ui.edit_number({min = 0, max = 0/0, on_accept = print})",
    };
    for (const char* lua : bad) {
        INFO(lua);
        BoundRuntime b({&install_ui_bindings});
        CHECK_FALSE(b.t.run(lua));
        drain();
        CHECK_FALSE(ui_keypad_is_visible());
    }
}

TEST_CASE_METHOD(EditorFixture, "helix.ui.edit_text returns the typed text",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        got, cancels = nil, 0
        helix.ui.edit_text({title = "Name", value = "old", max_length = 8,
            validate = function(s) if s == "" then return false, "Name cannot be empty" end
                                   return true end,
            on_accept = function(s) got = s end,
            on_cancel = function() cancels = cancels + 1 end})
    )"));
    drain();
    lv_obj_t* input = text_dialog();
    REQUIRE(input);
    CHECK(std::string(lv_textarea_get_text(input)) == "old");
    CHECK(lv_textarea_get_max_length(input) == 8);

    helix::LogCapture logs;
    lv_textarea_set_text(input, "");
    click(Modal::get_top(), "btn_primary");
    CHECK(text_dialog() == input);
    CHECK(logs.count_containing("Name cannot be empty") >= 1);

    lv_textarea_set_text(input, "rails");
    click(Modal::get_top(), "btn_primary");
    CHECK(b.t.global("got") == "rails");
    CHECK(b.t.global("cancels") == "0");
}

TEST_CASE_METHOD(EditorFixture, "helix.ui.edit_text cancel and dismissal run on_cancel once",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        cancels = 0
        function open() helix.ui.edit_text({on_accept = function() end,
                                            on_cancel = function() cancels = cancels + 1 end}) end
        open()
    )"));
    drain();
    REQUIRE(text_dialog());
    click(Modal::get_top(), "btn_secondary");
    CHECK(b.t.global("cancels") == "1");

    REQUIRE(b.t.run("open()"));
    drain();
    REQUIRE(text_dialog());
    Modal::hide(Modal::get_top(), ModalCloseReason::BackdropTap);
    drain();
    CHECK(b.t.global("cancels") == "2");
}

TEST_CASE_METHOD(EditorFixture, "unloading a plugin closes its text editor and runs nothing",
                 "[plugin][lua_bindings_editor]") {
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.ui.edit_text({on_accept = function() end,
                                                on_cancel = function() error("ran") end}))"));
        drain();
        REQUIRE(text_dialog());
    }
    drain();
    CHECK(text_dialog() == nullptr);
}

TEST_CASE_METHOD(EditorFixture, "helix.ui.edit_text validates its options",
                 "[plugin][lua_bindings_editor]") {
    const char* bad[] = {
        "helix.ui.edit_text()",
        "helix.ui.edit_text({})",
        "helix.ui.edit_text({on_accept = print, max_length = 0})",
        "helix.ui.edit_text({on_accept = print, max_length = 2.5})",
        "helix.ui.edit_text({on_accept = print, max_length = 3, value = 'four'})",
        "helix.ui.edit_text({on_accept = print, password = 'yes'})",
        "helix.ui.edit_text({on_accept = print, size = 3})",
    };
    for (const char* lua : bad) {
        INFO(lua);
        BoundRuntime b({&install_ui_bindings});
        CHECK_FALSE(b.t.run(lua));
        drain();
        CHECK(text_dialog() == nullptr);
    }
}

TEST_CASE_METHOD(EditorFixture, "editors refuse in a check and during on_unload",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(helix.ui.on("check", function()
        helix.ui.edit_text({on_accept = print}) return true end))"));
    helix::LogCapture logs;
    CHECK_FALSE(call_ui_handler_sync(*b.t.rt, "check", {}, {}));
    CHECK(logs.has_line_with({"helix.ui.edit_text", "not allowed in a menu check"}));
    drain();
    CHECK(text_dialog() == nullptr);

    b.ctx->unloading = true;
    CHECK_FALSE(b.t.run("helix.ui.edit_number({min = 0, max = 5, on_accept = print})"));
    drain();
    CHECK_FALSE(ui_keypad_is_visible());
}

TEST_CASE_METHOD(EditorFixture, "the host keypad reports a close without a value",
                 "[plugin][lua_bindings_editor][keypad]") {
    int confirms = 0;
    int cancels = 0;
    float last = -1;
    ui_keypad_config_t config{};
    config.min_value = 0;
    config.max_value = 300;
    config.allow_decimal = true;
    config.max_fraction_digits = 1;
    config.on_confirm = [&](float v) {
        ++confirms;
        last = v;
    };
    config.on_cancel = [&] { ++cancels; };
    REQUIRE(ui_keypad_show(&config));
    drain();
    // One fraction digit: the second is not taken.
    type_and_confirm("2.55");
    CHECK(confirms == 1);
    CHECK(last == Catch::Approx(2.5f));
    CHECK(cancels == 0);

    REQUIRE(ui_keypad_show(&config));
    drain();
    ui_keypad_hide();
    drain();
    CHECK(confirms == 1);
    CHECK(cancels == 1);
}

TEST_CASE_METHOD(EditorFixture, "an editor opened from on_accept appears",
                 "[plugin][lua_bindings_editor]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        log = {}
        function second_number()
            helix.ui.edit_number({title = "Second", min = 0, max = 50,
                on_accept = function(v) log[#log + 1] = "n2=" .. v end,
                on_cancel = function() log[#log + 1] = "n2 cancel" end})
        end
        function second_text()
            helix.ui.edit_text({title = "Name",
                on_accept = function(s) log[#log + 1] = "t=" .. s end,
                on_cancel = function() log[#log + 1] = "t cancel" end})
        end
        function first_number(next)
            helix.ui.edit_number({title = "First", min = 0, max = 50,
                on_accept = function(v) log[#log + 1] = "n1=" .. v; next() end,
                on_cancel = function() log[#log + 1] = "n1 cancel" end})
        end
    )"));

    SECTION("keypad then keypad") {
        REQUIRE(b.t.run("first_number(second_number)"));
        drain();
        type_and_confirm("7");
        drain();
        REQUIRE(ui_keypad_is_visible());
        type_and_confirm("9");
        drain();
        REQUIRE(b.t.run("got = table.concat(log, ',')"));
        CHECK(b.t.global("got") == "n1=7,n2=9");
    }
    SECTION("keypad then text") {
        REQUIRE(b.t.run("first_number(second_text)"));
        drain();
        type_and_confirm("7");
        drain();
        lv_obj_t* input = text_dialog();
        REQUIRE(input);
        lv_textarea_set_text(input, "belts");
        click(Modal::get_top(), "btn_primary");
        REQUIRE(b.t.run("got = table.concat(log, ',')"));
        CHECK(b.t.global("got") == "n1=7,t=belts");
    }
    SECTION("text then keypad") {
        REQUIRE(b.t.run(R"(helix.ui.edit_text({title = "Name",
            on_accept = function(s) log[#log + 1] = "t=" .. s; second_number() end}))"));
        drain();
        lv_obj_t* input = text_dialog();
        REQUIRE(input);
        lv_textarea_set_text(input, "rails");
        click(Modal::get_top(), "btn_primary");
        REQUIRE(ui_keypad_is_visible());
        type_and_confirm("3");
        drain();
        REQUIRE(b.t.run("got = table.concat(log, ',')"));
        CHECK(b.t.global("got") == "t=rails,n2=3");
    }
}

#endif // HELIX_HAS_PLUGINS
