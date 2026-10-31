// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/plugin_test_support.h"
#include "../test_helpers/widget_in_tree.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "layout_manager.h"
#include "lua_bindings.h"
#include "lvgl/src/misc/lv_text_private.h"
#include "lvgl_log_handler.h"
#include "plugin_canvas.h"
#include "plugin_overlay_host.h"
#include "scope_exit.h"
#include "xml_registration.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE_METHOD(LVGLTestFixture, "subjects register under the plugin prefix",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        count = helix.subject.int("count", 3)
        label = helix.subject.string("label", "idle")
    )"));
    lv_subject_t* c = lv_xml_get_subject(nullptr, "test-plugin__count");
    lv_subject_t* l = lv_xml_get_subject(nullptr, "test-plugin__label");
    REQUIRE(c);
    REQUIRE(l);
    CHECK(lv_subject_get_int(c) == 3);
    CHECK(std::string(lv_subject_get_string(l)) == "idle");

    REQUIRE(b.t.run(R"(count:set(7); label:set("busy"); got = count:get() .. label:get())"));
    CHECK(lv_subject_get_int(c) == 7);
    CHECK(std::string(lv_subject_get_string(l)) == "busy");
    CHECK(b.t.global("got") == "7busy");
}

TEST_CASE_METHOD(LVGLTestFixture, "observers see changes from C++ and Lua, not registration",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        seen = {}
        local s = helix.subject.int("n", 0)
        s:observe(function(v) seen[#seen + 1] = v end)
        s:set(1)
    )"));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "test-plugin__n"), 2);
    REQUIRE(b.t.run("result = table.concat(seen, ',')"));
    CHECK(b.t.global("result") == "1,2");
}

TEST_CASE_METHOD(LVGLTestFixture, "an observer setting its own subject stops at the depth cap",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    b.t.run(R"(
        local s = helix.subject.int("loop", 0)
        s:observe(function(v) s:set(v + 1) end)
        s:set(1)
    )");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "test-plugin__loop")) <= 10);
}

TEST_CASE_METHOD(LVGLTestFixture, "subject names and sizes are validated",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    CHECK_FALSE(b.t.run(R"(helix.subject.int("", 0))"));
    CHECK_FALSE(b.t.run(R"(helix.subject.int("has space", 0))"));
    REQUIRE(b.t.run(R"(helix.subject.int("dup", 0))"));
    // The 3rd error within 60 s faults the runtime, so the size checks need a fresh one.
    CHECK_FALSE(b.t.run(R"(helix.subject.int("dup", 0))"));

    BoundRuntime c({&install_ui_bindings});
    CHECK_FALSE(c.t.run(R"(helix.subject.string("big", string.rep("x", 2000)))"));
    REQUIRE(c.t.run(R"(s = helix.subject.string("small", ""))"));
    CHECK_FALSE(c.t.run(R"(s:set(string.rep("x", 2000)))"));
}

TEST_CASE_METHOD(LVGLTestFixture, "subjects are unregistered when the runtime closes",
                 "[plugin][bindings][ui]") {
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.subject.int("gone", 1))"));
        REQUIRE(lv_xml_get_subject(nullptr, "test-plugin__gone"));
    }
    CHECK(lv_xml_get_subject(nullptr, "test-plugin__gone") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "creating a plugin subject logs no missing-subject warning",
                 "[plugin][lua_bindings_ui]") {
    helix::logging::register_lvgl_log_handler();
    BoundRuntime b({&install_ui_bindings});
    {
        helix::TextLogCapture cap;
        REQUIRE(b.t.run(R"(s = helix.subject.int("quiet", 5))"));
        CHECK_FALSE(cap.contains("No subject was found"));
    }
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "test-plugin__quiet")) == 5);

    lv_subject_t app_subject;
    lv_subject_init_int(&app_subject, 5);
    helix::ScopeExit cleanup([&app_subject] {
        lv_xml_unregister_subject(nullptr, "test-plugin__taken");
        lv_subject_deinit(&app_subject);
    });
    lv_xml_register_subject(nullptr, "test-plugin__taken", &app_subject);
    CHECK_FALSE(b.t.run(R"(helix.subject.int("taken", 1))"));
}

TEST_CASE_METHOD(LVGLTestFixture, "closing the runtime logs no missing-subject warning",
                 "[plugin][lua_bindings_ui]") {
    helix::logging::register_lvgl_log_handler();
    helix::TextLogCapture cap;
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.subject.int("x", 1))"));
        // Take the name away before close: the teardown probe must treat a name
        // that no longer resolves as normal, not warn about it.
        lv_xml_unregister_subject(nullptr, "test-plugin__x");
    }
    CHECK_FALSE(cap.contains("No subject was found"));
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.subject bounds subjects per plugin",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        for i = 1, 128 do helix.subject.int("s" .. i, 0) end
        ok, err = pcall(helix.subject.int, "one-too-many", 0)
    )"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("at most 128 subjects") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "subject observe bounds live observers per plugin",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        local s = helix.subject.int("n", 0)
        for i = 1, 256 do s:observe(function() end) end
        ok, err = pcall(s.observe, s, function() end)
    )"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("at most 256") != std::string::npos);
}

namespace {
/// Answers the open confirm dialog the way a user would; every close path releases the
/// plugin's one dialog slot.
void answer_open_confirm(LVGLTestFixture& fx, const char* target, bool backdrop) {
    fx.process_lvgl(50); // the dialog's creation is queued; pump before reading the stack
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog);
    lv_obj_t* clicked = backdrop ? ModalStack::instance().backdrop_for(dialog)
                                 : lv_obj_find_by_name(dialog, target);
    REQUIRE(clicked);
    lv_obj_send_event(clicked, LV_EVENT_CLICKED, nullptr);
    fx.process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "helix.ui.confirm allows one open dialog per plugin",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(helix.ui.confirm("One", "body"))"));
    REQUIRE(b.t.run(R"(ok, err = pcall(helix.ui.confirm, "Two", "body"))"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("1 open dialog") != std::string::npos);

    // each close path releases the slot: primary button, cancel button, backdrop dismissal
    answer_open_confirm(*this, "btn_primary", false);
    REQUIRE(b.t.run(R"(helix.ui.confirm("Three", "body", { on_cancel = function() end }))"));
    answer_open_confirm(*this, "btn_secondary", false);
    REQUIRE(b.t.run(R"(helix.ui.confirm("Four", "body"))"));
    answer_open_confirm(*this, nullptr, true);
    REQUIRE(b.t.run(R"(helix.ui.confirm("Five", "body"))"));
    REQUIRE(ModalStack::instance().top_dialog());
}

TEST_CASE_METHOD(LVGLTestFixture, "runtime close leaves an app subject that took the name",
                 "[plugin][bindings][ui]") {
    lv_subject_t app_subject;
    lv_subject_init_int(&app_subject, 5);
    helix::ScopeExit cleanup([&app_subject] {
        if (lv_xml_get_subject(nullptr, "test-plugin__x") == &app_subject)
            lv_xml_unregister_subject(nullptr, "test-plugin__x");
        lv_subject_deinit(&app_subject);
    });
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.subject.int("x", 1))"));
        lv_xml_register_subject(nullptr, "test-plugin__x", &app_subject);
    }
    CHECK(lv_xml_get_subject(nullptr, "test-plugin__x") == &app_subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui.on handlers dispatch with an argument",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        calls = ""
        helix.ui.on("pick", function(arg) calls = calls .. tostring(arg) .. ";" end)
    )"));
    CHECK(dispatch_ui_handler(*b.t.rt, "pick", std::string("3")));
    CHECK(dispatch_ui_handler(*b.t.rt, "pick", std::nullopt));
    CHECK_FALSE(dispatch_ui_handler(*b.t.rt, "missing", std::nullopt));
    CHECK(b.t.global("calls") == "3;nil;");
}

TEST_CASE("plugin_event user_data parsing", "[plugin][bindings][ui]") {
    auto t = parse_plugin_event("orca-cal__start");
    CHECK(t.id == "orca-cal");
    CHECK(t.name == "start");
    CHECK_FALSE(t.arg);

    t = parse_plugin_event("orca-cal__pick:3:4");
    CHECK(t.name == "pick");
    CHECK(t.arg == std::optional<std::string>("3:4"));

    t = parse_plugin_event("orca-cal__my_handler");
    CHECK(t.name == "my_handler");

    CHECK(parse_plugin_event("noowner").id.empty());
    CHECK(parse_plugin_event("_x").id.empty());
    CHECK(parse_plugin_event("__x").id.empty());
    CHECK(parse_plugin_event("orca-cal_").id.empty());
    CHECK(parse_plugin_event("orca-cal_x").id.empty());
    CHECK(parse_plugin_event("Bad_x").id.empty());
    CHECK(parse_plugin_event("").id.empty());
    CHECK(parse_plugin_event(":x").id.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a confirm dialog that cannot be shown holds no slot",
                 "[plugin][bindings][ui]") {
    // Without the modal_dialog component the dialog cannot be built. The component
    // loader would register it again on the dialog's first lookup, so it is off for
    // the case; both are restored afterwards for the tests that share this process.
    struct HideModalDialog {
        bool was_registered = false;
        HideModalDialog() {
            lv_xml_set_component_loader(nullptr);
            was_registered = lv_xml_component_get_scope("modal_dialog") != nullptr;
            if (was_registered)
                lv_xml_component_unregister("modal_dialog");
        }
        ~HideModalDialog() {
            if (was_registered) {
                std::string path =
                    "A:" + helix::LayoutManager::instance().resolve_xml_path("modal_dialog.xml");
                lv_xml_register_component_from_file(path.c_str());
            }
            helix::register_xml_on_first_use();
        }
    } hidden;
    BoundRuntime b({&install_ui_bindings}, {});
    REQUIRE(b.t.run(R"(helix.ui.confirm("One", "body"))"));
    REQUIRE(b.t.run(R"(helix.ui.confirm("Two", "body"))"));
}

namespace {
/// Closes a runtime while a bound label still observes its subject: the subject
/// is retired, kept alive by the helix-xml bind record on the returned widget.
/// Bind through helix-xml because its bind record keeps the observer handle and
/// detaches it on the widget's delete event; that record is the reference that
/// must outlive the runtime.
lv_obj_t* bind_label_to_doomed_subject() {
    lv_obj_t* root = nullptr;
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(s = helix.subject.string("status", "hi"))"));
        REQUIRE(lv_xml_register_component_from_data(
                    "test-plugin__bindrow",
                    "<component><view extends=\"lv_obj\">"
                    "<lv_label name=\"bound_label\" bind_text=\"test-plugin__status\"/>"
                    "</view></component>") == LV_RESULT_OK);
        const char* attrs[] = {nullptr};
        root = static_cast<lv_obj_t*>(
            lv_xml_create(lv_screen_active(), "test-plugin__bindrow", attrs));
        REQUIRE(root);
    }
    return root;
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a bound object outlives its plugin's runtime safely",
                 "[plugin][lua_bindings_ui]") {
    sweep_retired_subjects();
    size_t before = retired_subject_count();
    lv_obj_t* root = bind_label_to_doomed_subject();
    helix::ScopeExit cleanup([&root] {
        if (root && helix::test::widget_in_tree(root))
            lv_obj_delete(root);
        lv_xml_component_unregister("test-plugin__bindrow");
    });
    lv_obj_t* label = lv_obj_find_by_name(root, "bound_label");
    REQUIRE(label);
    CHECK(lv_xml_get_subject(nullptr, "test-plugin__status") == nullptr);
    CHECK(retired_subject_count() == before + 1);
    CHECK(std::string(lv_label_get_text(label)) == "hi");

    lv_obj_delete(root); // detaches the bind record from a subject that must still be alive
    root = nullptr;
    sweep_retired_subjects();
    CHECK(retired_subject_count() == before);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin load sweeps retired subjects",
                 "[plugin][lua_bindings_ui]") {
    sweep_retired_subjects();
    size_t before = retired_subject_count();
    lv_obj_t* root = bind_label_to_doomed_subject();
    helix::ScopeExit cleanup([&root] {
        if (root && helix::test::widget_in_tree(root))
            lv_obj_delete(root);
        lv_xml_component_unregister("test-plugin__bindrow");
    });
    REQUIRE(retired_subject_count() == before + 1);
    lv_obj_delete(root); // detaches the bind record: nothing observes the subject now
    root = nullptr;

    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    drain();
    CHECK(retired_subject_count() == before);
}

TEST_CASE_METHOD(LVGLTestFixture, "an unobserved subject is freed at unload",
                 "[plugin][lua_bindings_ui]") {
    sweep_retired_subjects();
    size_t before = retired_subject_count();
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(s = helix.subject.int("n", 1); s:observe(function() end))"));
    }
    CHECK(retired_subject_count() == before);
}

TEST_CASE_METHOD(LVGLTestFixture, "toast and confirm validate their arguments",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        helix.ui.toast("hello")
        helix.ui.toast("careful", "warning")
        helix.ui.confirm("Title", "Body", { severity = "warning", on_confirm = function() end })
    )"));
    CHECK_FALSE(b.t.run(R"(helix.ui.toast("x", "loud"))"));
    CHECK_FALSE(b.t.run(R"(helix.ui.confirm("t", "m", { severity = "loud" }))"));
}

namespace {
/// Width lv_draw_label gives @p text in the canvas font @p name, for comparison.
int32_t drawn_width(const char* text, const char* name) {
    const lv_font_t* font = canvas_resolve_font(name);
    REQUIRE(font);
    lv_text_attributes_t attributes = {};
    attributes.max_width = LV_COORD_MAX;
    return lv_text_get_width(text, static_cast<uint32_t>(strlen(text)), font, &attributes);
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "helix.ui.text_width measures in the live theme font",
                 "[plugin][bindings][ui][text_width]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        small = helix.ui.text_width("Benchy_0.2mm_PLA.gcode", "small")
        heading = helix.ui.text_width("Benchy_0.2mm_PLA.gcode", "heading")
        default = helix.ui.text_width("Benchy_0.2mm_PLA.gcode")
        body = helix.ui.text_width("Benchy_0.2mm_PLA.gcode", "body")
        empty = helix.ui.text_width("", "body")
        is_int = math.type(small)
    )"));
    CHECK(b.t.global("small") == std::to_string(drawn_width("Benchy_0.2mm_PLA.gcode", "small")));
    CHECK(b.t.global("heading") ==
          std::to_string(drawn_width("Benchy_0.2mm_PLA.gcode", "heading")));
    CHECK(std::stoi(b.t.global("heading")) > std::stoi(b.t.global("small")));
    CHECK(b.t.global("default") == b.t.global("body"));
    CHECK(b.t.global("empty") == "0");
    CHECK(b.t.global("is_int") == "integer");
}

TEST_CASE_METHOD(XMLTestFixture, "helix.ui.text_width refuses fonts canvas text refuses",
                 "[plugin][bindings][ui][text_width]") {
    // Each refusal gets a fresh runtime: the 3rd error within 60 s faults one.
    {
        BoundRuntime b({&install_ui_bindings});
        CHECK_FALSE(b.t.run(R"(helix.ui.text_width("x", "heading_large"))"));
        CHECK_FALSE(b.t.run(R"(helix.ui.text_width("x", "nope"))"));
    }
    {
        BoundRuntime b({&install_ui_bindings});
        CHECK_FALSE(b.t.run(R"(helix.ui.text_width(nil, "body"))"));
        CHECK_FALSE(b.t.run(R"(helix.ui.text_width(string.rep("w", 1025), "body"))"));
    }
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(w = helix.ui.text_width(string.rep("w", 1024), "body"))"));
        CHECK(std::stoi(b.t.global("w")) > 0);
    }
}

namespace {
/// A PluginUi double recording opens and closes without touching navigation.
struct FakeUi {
    int opened = 0;
    int closed = 0;
    bool reject = false; ///< when set, open reports failure
    std::string last_component;
    PluginUi::Attrs last_attrs;

    PluginUi ui{
        [this](const std::string& component, PluginUi::Callbacks, const PluginUi::Attrs& attrs) {
            if (reject)
                return 0;
            ++opened;
            last_component = component;
            last_attrs = attrs;
            return 7;
        },
        [this](int handle) { closed += handle; }};
};
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "helix.ui.overlay needs a host ui", "[plugin][lua_bindings_ui]") {
    BoundRuntime b({&install_ui_bindings}); // no ctx->ui: no host provides overlays here
    REQUIRE(b.t.run(R"(ok, err = pcall(helix.ui.overlay, "test-plugin__p"))"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("not available") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.ui.overlay checks ownership, attributes and the handle",
                 "[plugin][lua_bindings_ui]") {
    FakeUi fake;
    BoundRuntime b({&install_ui_bindings});
    b.ctx->ui = &fake.ui;
    REQUIRE(b.t.run(R"(
        ok1, e1 = pcall(helix.ui.overlay, "other__panel")
        ok2, e2 = pcall(helix.ui.overlay, "test-plugin__p", { bind_text = "other__s" })
        ok3, e3 = pcall(helix.ui.overlay, "test-plugin__p", { title = 5 })
        ok4, e4 = pcall(helix.ui.overlay, "test-plugin__p", { on_close = "no" })
        h = helix.ui.overlay("test-plugin__p", { title = "Hi", on_close = function() end })
        h:close()
    )"));
    CHECK(b.t.global("ok1") == "false"); // component not owned by this plugin
    CHECK(b.t.global("e1").find("not owned") != std::string::npos);
    CHECK(b.t.global("ok2") == "false"); // policy: bind_text must name an owned subject
    CHECK(b.t.global("e2").find("subject") != std::string::npos);
    CHECK(b.t.global("ok3") == "false"); // attribute values must be strings
    CHECK(b.t.global("ok4") == "false"); // on_close must be a function
    CHECK(fake.opened == 1);
    CHECK(fake.last_component == "test-plugin__p");
    REQUIRE(fake.last_attrs.size() == 1); // on_close is not an attribute
    CHECK(fake.last_attrs[0].first == "title");
    CHECK(fake.last_attrs[0].second == "Hi");
    CHECK(fake.closed == 7);

    fake.reject = true;
    REQUIRE(b.t.run(R"(ok5, e5 = pcall(helix.ui.overlay, "test-plugin__p"))"));
    CHECK(b.t.global("ok5") == "false");
    CHECK(b.t.global("e5").find("cannot open") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture, "closing the runtime hides the plugin's open confirm dialog",
                 "[plugin][lua_bindings_ui]") {
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.ui.confirm("Q", "body"))"));
        process_lvgl(50); // the dialog's creation is queued
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(ModalStack::instance().top_dialog());
    } // the runtime closer hides the still-open dialog before the state goes
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a confirm the user closed is not hidden again at close",
                 "[plugin][lua_bindings_ui]") {
    helix::TextLogCapture log;
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.ui.confirm("Q", "body"))"));
        answer_open_confirm(*this, "btn_secondary", false);
        REQUIRE(ModalStack::instance().top_dialog() == nullptr);
    } // the dialog's close path cleared the pointer, so the closer must find nothing to hide
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(log.contains("Dialog not found in stack"));
}

namespace {
std::vector<Installer> check_installers() {
    return {&install_ui_bindings, &install_io_bindings, &install_moonraker_bindings,
            &install_printer_bindings, &install_sound_bindings};
}

SettingDecl check_step_decl() {
    SettingDecl d;
    d.key = "step";
    d.label = "Step";
    d.type = SettingType::Int;
    d.min = 1;
    d.max = 20;
    d.default_value = 5;
    return d;
}

/// Runs handler `name` as a menu check; `visible` is the boolean it returned.
bool run_check(BoundRuntime& b, const std::string& name, bool* visible = nullptr) {
    return call_ui_handler_sync(*b.t.rt, name, {}, [visible](lua_State* co, int nres) {
        if (visible && nres >= 1)
            *visible = lua_type(co, -nres) == LUA_TBOOLEAN && lua_toboolean(co, -nres);
    });
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "side effects refused in check",
                 "[plugin][bindings][ui][check]") {
    struct Case {
        const char* call;
        const char* lua;
    };
    const Case cases[] = {
        {"helix.ui.toast", R"(helix.ui.toast("hi"))"},
        {"helix.ui.dialog", R"(helix.ui.dialog("test-plugin__d"))"},
        {"helix.ui.overlay", R"(helix.ui.overlay("test-plugin__o"))"},
        {"helix.ui.confirm", R"(helix.ui.confirm("t", "m"))"},
        {"helix.ui.on", R"(helix.ui.on("late", function() end))"},
        {"helix.sound.play", R"(helix.sound.play("button_tap"))"},
        {"helix.storage.set", R"(helix.storage.set("k", 1))"},
        {"helix.settings.set", R"(helix.settings.set("step", 7))"},
        {"helix.settings.on_change", R"(helix.settings.on_change("step", function() end))"},
        {"subject:set", R"(counter:set(9))"},
        {"subject:observe", R"(counter:observe(function() end))"},
        {"helix.subject", R"(helix.subject.int("fresh", 1))"},
        {"helix.timer", R"(helix.timer.after(10, function() fired = true end))"},
        {"helix.timer", R"(helix.timer.every(10, function() fired = true end))"},
        {"timer:cancel", R"(armed:cancel())"},
        {"helix.i18n.on_change", R"(helix.i18n.on_change(function() end))"},
        {"helix.printer.watch", R"(helix.printer.watch("bed_temp", function() end))"},
        {"helix.moonraker.subscribe", R"(helix.moonraker.subscribe({}, function() end))"},
        {"helix.moonraker.on_agent_event",
         R"(helix.moonraker.on_agent_event("e", function() end))"},
        {"not allowed in a menu check", R"(helix.gcode("G28"))"},
        {"not allowed in a menu check", R"(helix.moonraker.call("server.info"))"},
        {"not allowed in a menu check", R"(helix.http.get("https://example.com"))"},
        {"not allowed in a menu check", R"(helix.sleep(1))"},
    };
    TempDir dir;
    for (const auto& c : cases) {
        INFO(c.lua);
        BoundRuntime b(check_installers(),
                       {Permission::Gcode, Permission::Http, Permission::Storage},
                       {check_step_decl()}, dir.file("s.json"));
        REQUIRE(b.t.run(std::string(R"(
            counter = helix.subject.int("counter", 3)
            armed = helix.timer.after(100000, function() end)
            helix.ui.on("check", function() )") +
                        c.lua + R"( return true end)
        )"));
        helix::LogCapture logs;
        bool visible = false;
        CHECK_FALSE(run_check(b, "check", &visible));
        CHECK_FALSE(visible);
        CHECK(logs.has_line_with({c.call, "not allowed in a menu check"}));
        CHECK_FALSE(b.t.rt->faulted());

        helix::ui::UpdateQueue::instance().drain();
        CHECK(b.fake.requests.empty());
        CHECK(b.saves == 0);
        CHECK(b.settings.empty());
        CHECK_FALSE(std::filesystem::exists(dir.file("s.json")));
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "test-plugin__counter")) == 3);
        CHECK(lv_xml_find_subject(nullptr, "test-plugin__fresh") == nullptr);
        CHECK(b.t.global("fired") == "nil");
        // The plugin is untouched: an ordinary entry still runs.
        CHECK(b.t.run("ok = true"));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "reads allowed in check", "[plugin][bindings][ui][check]") {
    BoundRuntime b(check_installers(), {}, {check_step_decl()});
    lv_subject_set_int(lv_xml_get_subject(nullptr, "bed_temp"), 605);
    REQUIRE(b.t.run(R"(
        counter = helix.subject.int("counter", 3)
        local calls = 0
        helix.ui.on("check", function(ctx)
            calls = calls + 1
            return helix.settings.get("step") == 5 and counter:get() == 3
                and helix.printer.get("bed_temp") == 60.5 and calls == 1
                and helix.json.decode("[1]")[1] == 1 and type(helix.i18n.t("x")) == "string"
        end)
    )"));
    bool visible = false;
    CHECK(run_check(b, "check", &visible));
    CHECK(visible);
}

TEST_CASE_METHOD(LVGLTestFixture, "a check that is not registered reports",
                 "[plugin][bindings][ui][check]") {
    BoundRuntime b({&install_ui_bindings});
    helix::LogCapture logs;
    CHECK_FALSE(run_check(b, "nowhere"));
    CHECK(logs.count_containing("menu check 'nowhere' is not registered") == 1);
    CHECK_FALSE(b.t.rt->faulted());
}

TEST_CASE_METHOD(LVGLTestFixture, "dispatch_ui_handler with a table",
                 "[plugin][bindings][ui][check]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        helix.ui.on("open", function(ctx)
            got_path = ctx.path
            got_missing = ctx.missing
            got_tools = #ctx.tools_used
        end)
    )"));
    json ctx = {{"path", "a/b.gcode"}, {"tools_used", json::array({0, 1})}};
    CHECK(dispatch_ui_handler(*b.t.rt, "open", LuaRuntime::PushFn([&ctx](lua_State* co) {
        push_json(co, ctx);
        return 1;
    })));
    CHECK(b.t.global("got_path") == "a/b.gcode");
    CHECK(b.t.global("got_missing") == "nil");
    CHECK(b.t.global("got_tools") == "2");
    CHECK_FALSE(dispatch_ui_handler(*b.t.rt, "missing", LuaRuntime::PushFn{}));
}

TEST_CASE_METHOD(LVGLTestFixture, "a check's <close> handlers run inside check mode",
                 "[plugin][bindings][ui][check]") {
    struct Case {
        const char* name;
        const char* close_body;
        const char* log;
    };
    // Each check fails after arming a to-be-closed variable; its __close runs as the
    // check's thread is closed.
    const Case cases[] = {
        {"stores", R"(helix.storage.set("x", 1))",
         "helix.storage.set is not allowed in a menu check"},
        {"raises", R"(error("from __close"))", "from __close"},
        {"loops", "while true do end", "menu check exceeded its time budget"},
    };
    TempDir dir;
    for (const auto& c : cases) {
        INFO(c.name);
        BoundRuntime b(check_installers(), {Permission::Storage}, {}, dir.file("s.json"));
        REQUIRE(b.t.run(std::string(R"(
            helix.ui.on("check", function()
                local guard <close> = setmetatable({}, {__close = function() )") +
                        c.close_body + R"( end})
                local _ = nil + 1
            end)
        )"));
        helix::LogCapture logs;
        CHECK_FALSE(run_check(b, "check"));
        CHECK(logs.count_containing(c.log) >= 1);
        CHECK_FALSE(b.t.rt->faulted());
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(b.t.run(R"(stored = helix.storage.get("x"))"));
        CHECK(b.t.global("stored") == "nil");
        CHECK(b.t.run("ok = true"));
    }
}

TEST_CASE_METHOD(XMLTestFixture, "binding metatables are locked against plugins",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings, &install_moonraker_bindings, &install_canvas_bindings},
                   {Permission::MoonrakerWrite});
    REQUIRE(b.t.run(R"(
        local kinds = {
            timer = helix.timer.after(100000, function() end),
            subject = helix.subject.int("locked_n", 0),
            canvas = helix.canvas("locked_c"),
            subscription = helix.moonraker.subscribe({print_stats = {"state"}}, function() end),
        }
        seen = {}
        for name, ud in pairs(kinds) do
            seen[#seen + 1] = name .. "=" .. tostring(getmetatable(ud))
        end
        table.sort(seen)
        seen = table.concat(seen, ",")
        -- The arming attempt: with the metatable hidden there is no table to give a __gc.
        armed = pcall(function()
            getmetatable(helix.timer.after(100000, function() end)).__gc = function()
                while true do end
            end
        end)
        swap = pcall(setmetatable, kinds.timer, {})
        strmeta = tostring(getmetatable(""))
    )"));
    CHECK(b.t.global("strmeta") == "locked");
    CHECK(b.t.global("seen") == "canvas=locked,subject=locked,subscription=locked,timer=locked");
    CHECK(b.t.global("armed") == "false");
    CHECK(b.t.global("swap") == "false");
    // A new userdata still works and GC finds nothing to finalize.
    REQUIRE(
        b.t.run("helix.timer.after(100000, function() end); collectgarbage(); collectgarbage()"));
}

#endif // HELIX_HAS_PLUGINS
