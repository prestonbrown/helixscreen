// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Components register the first time something names them
// (helix::register_xml_on_first_use), so these tests point the asset root at a
// scratch ui_xml/ tree whose components nothing registers up front.

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/layout_manager_test_access.h"
#include "core/data_root_resolver.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "layout_manager.h"
#include "xml_hot_reloader.h"
#include "xml_registration.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

extern "C" {
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component_private.h"
}

namespace fs = std::filesystem;

namespace {

/// Registry membership asked without lv_xml_component_get_scope(), which would load.
bool is_registered(const char* name) {
    struct Probe {
        const char* name;
        bool found;
    } probe{name, false};
    lv_xml_component_foreach(
        [](const char* n, void* ud) {
            auto* p = static_cast<Probe*>(ud);
            if (std::strcmp(n, p->name) == 0)
                p->found = true;
        },
        &probe);
    return probe.found;
}

std::string component_with_label(const std::string& text) {
    return "<component><view extends=\"lv_obj\"><lv_label name=\"probe_label\" text=\"" + text +
           "\"/></view></component>";
}

std::string label_text(lv_obj_t* root) {
    auto* label = lv_obj_find_by_name(root, "probe_label");
    return label ? lv_label_get_text(label) : "<no probe_label>";
}

class LazyXmlFixture : public XMLTestFixture {
  public:
    LazyXmlFixture()
        : saved_root_(helix::asset_root()),
          root_(fs::temp_directory_path() /
                ("helix_lazy_xml_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(root_ / "ui_xml" / "components");
        fs::create_directories(root_ / "ui_xml" / "portrait");
        helix::set_asset_root(root_.string());
    }

    ~LazyXmlFixture() override {
        helix::set_asset_root(saved_root_);
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void write(const std::string& rel, const std::string& content) {
        std::ofstream(root_ / "ui_xml" / rel) << content;
    }

    /// Rewrite with a later mtime than the hot reloader recorded.
    void rewrite(const std::string& rel, const std::string& content) {
        auto path = root_ / "ui_xml" / rel;
        auto before = fs::last_write_time(path);
        write(rel, content);
        fs::last_write_time(path, before + std::chrono::seconds(2));
    }

    std::string xml_dir() const {
        return (root_ / "ui_xml").string();
    }

  private:
    std::string saved_root_;
    fs::path root_;
};

} // namespace

TEST_CASE_METHOD(LazyXmlFixture, "a component registers on its first create, with what it nests",
                 "[xml][lazy]") {
    write("lazy_probe_outer.xml", "<component><view extends=\"lv_obj\">"
                                  "<lazy_probe_inner name=\"nested\"/></view></component>");
    write("components/lazy_probe_inner.xml", component_with_label("inner"));
    REQUIRE_FALSE(is_registered("lazy_probe_outer"));
    REQUIRE_FALSE(is_registered("lazy_probe_inner"));

    auto* outer = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_outer", nullptr));

    REQUIRE(outer != nullptr);
    auto* nested = lv_obj_find_by_name(outer, "nested");
    REQUIRE(nested != nullptr);
    CHECK(label_text(nested) == "inner");
    CHECK(is_registered("lazy_probe_inner"));
}

TEST_CASE_METHOD(LazyXmlFixture, "a name with no XML file still fails to create", "[xml][lazy]") {
    CHECK(lv_xml_create(test_screen(), "lazy_probe_absent", nullptr) == nullptr);
    CHECK_FALSE(is_registered("lazy_probe_absent"));
}

TEST_CASE_METHOD(LazyXmlFixture,
                 "a component wrapping an unregistered same-named widget fails to create",
                 "[xml][lazy]") {
    // temp_display.xml's shape: the view extends the C++ widget the file is named after.
    write("lazy_probe_wraps_widget.xml",
          "<component><view extends=\"lazy_probe_wraps_widget\"/></component>");
    write("lazy_probe_wraps_widget_user.xml", "<component><view extends=\"lv_obj\">"
                                              "<lazy_probe_wraps_widget name=\"inner\"/>"
                                              "</view></component>");

    CHECK(lv_xml_create(test_screen(), "lazy_probe_wraps_widget", nullptr) == nullptr);
    auto* user = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "lazy_probe_wraps_widget_user", nullptr));
    REQUIRE(user != nullptr);
    CHECK(lv_obj_find_by_name(user, "inner") == nullptr);
}

TEST_CASE_METHOD(LazyXmlFixture, "first use registers the active layout variant's copy",
                 "[xml][lazy]") {
    write("lazy_probe_variant.xml", component_with_label("base"));
    write("portrait/lazy_probe_variant.xml", component_with_label("portrait"));
    auto& lm = helix::LayoutManager::instance();
    lm.set_override("portrait");
    lm.init(480, 800);

    auto* obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_variant", nullptr));

    REQUIRE(obj != nullptr);
    CHECK(label_text(obj) == "portrait");
}

TEST_CASE_METHOD(LazyXmlFixture, "a component registered on first use hot-reloads",
                 "[xml][lazy][hotreload]") {
    write("lazy_probe_reload.xml", component_with_label("v1"));
    write("lazy_probe_unopened.xml", component_with_label("v1"));
    REQUIRE(lv_xml_create(test_screen(), "lazy_probe_reload", nullptr) != nullptr);

    helix::XmlHotReloader hr;
    std::vector<std::string> reloaded;
    hr.set_after_reload_callback([&](const std::string& name) { reloaded.push_back(name); });
    hr.start({xml_dir()}, 60000);
    rewrite("lazy_probe_reload.xml", component_with_label("v2"));
    rewrite("lazy_probe_unopened.xml", component_with_label("v2"));
    hr.scan_and_reload();
    hr.stop();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(reloaded.size() == 2);
    auto* reloaded_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_reload", nullptr));
    REQUIRE(reloaded_obj != nullptr);
    CHECK(label_text(reloaded_obj) == "v2");
    // Saved before anything ever created it: the reload registers the new copy.
    auto* unopened =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_unopened", nullptr));
    REQUIRE(unopened != nullptr);
    CHECK(label_text(unopened) == "v2");
}

TEST_CASE_METHOD(LazyXmlFixture, "a modal's component registers when the modal first shows",
                 "[xml][lazy]") {
    write("lazy_probe_modal.xml", component_with_label("modal"));
    REQUIRE_FALSE(is_registered("lazy_probe_modal"));

    lv_obj_t* dialog = Modal::show("lazy_probe_modal");

    REQUIRE(dialog != nullptr);
    CHECK(label_text(dialog) == "modal");
    Modal::hide(dialog);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LazyXmlFixture,
                 "a component first named by a hot-reloaded view registers when it rebuilds",
                 "[xml][lazy][hotreload]") {
    write("lazy_probe_host.xml", component_with_label("host"));
    write("components/lazy_probe_added.xml", component_with_label("added"));

    helix::XmlHotReloader hr;
    lv_obj_t* rebuilt = nullptr;
    // The rebuild NavigationManager runs after a reload is a fresh lv_xml_create().
    hr.set_after_reload_callback([&](const std::string& name) {
        rebuilt = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), name.c_str(), nullptr));
    });
    hr.start({xml_dir()}, 60000);
    rewrite("lazy_probe_host.xml", "<component><view extends=\"lv_obj\">"
                                   "<lazy_probe_added name=\"added\"/></view></component>");
    REQUIRE_FALSE(is_registered("lazy_probe_added"));
    hr.scan_and_reload();
    hr.stop();
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(rebuilt != nullptr);
    auto* added = lv_obj_find_by_name(rebuilt, "added");
    REQUIRE(added != nullptr);
    CHECK(label_text(added) == "added");
}

TEST_CASE_METHOD(LazyXmlFixture, "a component file that does not parse is read once",
                 "[xml][lazy]") {
    write("lazy_probe_broken.xml", "<component><view extends=\"lv_obj\">");

    CHECK(lv_xml_component_get_scope("lazy_probe_broken") == nullptr);
    // Fixed on disk: a second lookup reading the file again would now register it.
    rewrite("lazy_probe_broken.xml", component_with_label("fixed"));
    CHECK(lv_xml_component_get_scope("lazy_probe_broken") == nullptr);
    CHECK_FALSE(is_registered("lazy_probe_broken"));
}

TEST_CASE_METHOD(LazyXmlFixture,
                 "after a layout change an idle component registers its new variant",
                 "[xml][lazy]") {
    write("lazy_probe_resized.xml", component_with_label("base"));
    write("portrait/lazy_probe_resized.xml", component_with_label("portrait"));
    auto& lm = helix::LayoutManager::instance();
    auto* before =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_resized", nullptr));
    REQUIRE(before != nullptr);
    REQUIRE(label_text(before) == "base");
    lv_obj_delete(before);

    lm.set_override("portrait");
    lm.init(480, 800);
    helix::unregister_idle_xml_components();

    auto* after =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_resized", nullptr));
    REQUIRE(after != nullptr);
    CHECK(label_text(after) == "portrait");
}

TEST_CASE_METHOD(LazyXmlFixture, "a layout change keeps a component with a live instance",
                 "[xml][lazy]") {
    write("lazy_probe_live.xml", component_with_label("base"));
    write("portrait/lazy_probe_live.xml", component_with_label("portrait"));
    auto* live = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_live", nullptr));
    REQUIRE(live != nullptr);
    lv_xml_component_scope_t* scope = lv_xml_component_find_scope("lazy_probe_live");

    auto& lm = helix::LayoutManager::instance();
    lm.set_override("portrait");
    lm.init(480, 800);
    helix::unregister_idle_xml_components();

    CHECK(lv_xml_component_find_scope("lazy_probe_live") == scope);
    CHECK(label_text(live) == "base");
}

TEST_CASE_METHOD(LazyXmlFixture,
                 "a component first used after a printer switch's subject teardown registers",
                 "[xml][lazy]") {
    write("lazy_probe_after_switch.xml", component_with_label("next printer"));
    // A switch tears down through StaticSubjectRegistry, whose XmlSubjects entry is
    // deinit_xml_subjects(); the next session's UI is built through the same loader.
    StaticSubjectRegistry::instance().register_deinit("XmlSubjects", helix::deinit_xml_subjects);
    REQUIRE(StaticSubjectRegistry::instance().deinit_one("XmlSubjects"));

    auto* obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_after_switch", nullptr));

    REQUIRE(obj != nullptr);
    CHECK(label_text(obj) == "next printer");
}

TEST_CASE_METHOD(LazyXmlFixture, "a layout change keeps a component holding a C++ subject",
                 "[xml][lazy]") {
    write("lazy_probe_cpp_subject.xml", component_with_label("x"));
    lv_obj_delete(
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_cpp_subject", nullptr)));
    lv_xml_component_scope_t* scope = lv_xml_component_find_scope("lazy_probe_cpp_subject");
    REQUIRE(scope != nullptr);
    static lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    REQUIRE(lv_xml_register_subject(scope, "lazy_probe_cpp_value", &subject) == LV_RESULT_OK);

    helix::unregister_idle_xml_components();

    CHECK(lv_xml_component_find_scope("lazy_probe_cpp_subject") == scope);
    lv_xml_component_unregister("lazy_probe_cpp_subject");
    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LazyXmlFixture, "a layout change keeps a component whose style was borrowed",
                 "[xml][lazy]") {
    write("lazy_probe_lender.xml", "<component><styles><style name=\"lent\" radius=\"7\"/>"
                                   "</styles><view extends=\"lv_obj\"/></component>");
    write("lazy_probe_borrower.xml", "<component><view extends=\"lv_obj\">"
                                     "<style name=\"lazy_probe_lender.lent\"/></view></component>");
    lv_obj_delete(
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lazy_probe_borrower", nullptr)));
    lv_xml_component_scope_t* lender = lv_xml_component_find_scope("lazy_probe_lender");
    REQUIRE(lender != nullptr);
    REQUIRE(lender->instance_cnt == 0);

    helix::unregister_idle_xml_components();

    CHECK(lv_xml_component_find_scope("lazy_probe_lender") == lender);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a layout change keeps color_picker's C++ constants",
                 "[xml][lazy][ui_integration]") {
    lv_xml_component_scope_t* scope = lv_xml_component_find_scope("color_picker");
    REQUIRE(scope != nullptr);
    REQUIRE(scope->instance_cnt == 0);

    helix::unregister_idle_xml_components();

    lv_xml_component_scope_t* after = lv_xml_component_find_scope("color_picker");
    REQUIRE(after != nullptr);
    CHECK(lv_xml_get_const(after, "sv_size") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the eager components are registered before first use",
                 "[xml][lazy][ui_integration]") {
    // styles.xml resolves theme tokens at registration, so it registers at boot.
    CHECK(is_registered("styles"));
    // C++ writes responsive consts into these scopes at boot.
    for (const char* name : {"color_picker", "ams_edit_overlay"}) {
        INFO(name);
        REQUIRE(is_registered(name));
        CHECK(lv_xml_get_const(lv_xml_component_get_scope(name), "sv_size") != nullptr);
    }
}
