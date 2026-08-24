// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_host_power_availability.cpp
 * @brief Host power controls (shutdown/reboot UI) are unavailable on Android.
 *
 * On Android, helixscreen is a tablet app: "screen" reboot/shutdown cannot
 * work (no logind, systemctl, or busybox init to call), and users reported
 * the host reboot RPC failing against their printer hosts. The availability
 * rule lives in ONE predicate — helix::platform_host_power_supported() —
 * which seeds the platform_host_power_supported subject; the home-grid
 * widget registry gates the shutdown widget on that subject.
 */

#include "ui_split_button.h"
#include "ui_status_pill.h"

#include "../../src/ui/panel_widgets/shutdown_widget.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "panel_widget_registry.h"
#include "platform_info.h"
#include "setting_group.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// RAII platform-override guard — every test that flips the override restores
/// it on scope exit so one failure cannot leak Android-ness into other tests.
class PlatformOverrideGuard {
  public:
    explicit PlatformOverrideGuard(int value) {
        set_platform_override(value);
    }
    ~PlatformOverrideGuard() {
        set_platform_override(-1);
    }
};

} // namespace

TEST_CASE("Host power predicate follows the platform override", "[android][platform][power]") {
    SECTION("non-Android default supports host power") {
        set_platform_override(-1);
        CHECK(platform_host_power_supported() == !is_android_platform());
        CHECK(platform_host_power_supported()); // desktop/test builds
    }
    SECTION("Android does not support host power") {
        PlatformOverrideGuard android(1);
        CHECK_FALSE(platform_host_power_supported());
    }
    SECTION("explicit non-Android override supports host power") {
        PlatformOverrideGuard desktop(0);
        CHECK(platform_host_power_supported());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "platform_host_power_supported subject is seeded from the predicate",
                 "[android][platform][power]") {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, "platform_host_power_supported");
    REQUIRE(subj != nullptr);
    CHECK(lv_subject_get_int(subj) == (platform_host_power_supported() ? 1 : 0));
}

TEST_CASE("Shutdown widget is gated on host power availability", "[android][panel_widget][power]") {
    const auto* def = find_widget_def("shutdown");
    REQUIRE(def != nullptr);
    REQUIRE(def->hardware_gate_subject != nullptr);
    CHECK(std::string(def->hardware_gate_subject) == "platform_host_power_supported");
    CHECK(def->hardware_gate_hint != nullptr);
}

namespace {

/// Builds the real advanced_panel.xml so the POWER group's visibility binding
/// can be exercised against a live tree. Registers the C++ setting_group
/// widget plus the panel's component dependencies, in the same shape
/// production's xml_registration.cpp uses.
class AdvancedPowerGroupFixture : public XMLTestFixture {
  public:
    AdvancedPowerGroupFixture() : XMLTestFixture() {
        setting_group_register();
        ui_status_pill_register_widget();
        REQUIRE(register_component("setting_group_header"));
        REQUIRE(register_component("setting_action_row"));
        REQUIRE(register_component("beta_feature"));
        REQUIRE(register_component("advanced_panel"));

        // XMLTestFixture (unlike LVGLUITestFixture) does not route through the
        // app_globals stub, so ensure the host-power subject exists before the
        // panel's bindings resolve. STATIC, deliberately: the XML subject
        // registry is process-global and never forgets an entry, so a fixture
        // member here dangles after this test and any later consumer of the
        // name (the home panel's shutdown-widget gate walk, XML
        // bind_state_if_eq) dereferences a freed lv_subject_t — the nightly
        // TSan SIGSEGV at setup_gate_observers and the 2026-08-16 ASan
        // heap-buffer-overflow in lv_subject_add_observer_obj were both this.
        if (!lv_xml_get_subject(nullptr, "platform_host_power_supported")) {
            static lv_subject_t host_power_subject;
            lv_subject_init_int(&host_power_subject, 1);
            lv_xml_register_subject(nullptr, "platform_host_power_supported", &host_power_subject);
        }

        panel_ = create_component("advanced_panel");
        REQUIRE(panel_ != nullptr);
        group_ = lv_obj_find_by_name(panel_, "group_power");
        REQUIRE(group_ != nullptr);
        process_lvgl(50);
    }

    lv_subject_t* host_power_subject() {
        auto* subj = lv_xml_get_subject(nullptr, "platform_host_power_supported");
        REQUIRE(subj != nullptr);
        return subj;
    }

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* group_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(AdvancedPowerGroupFixture,
                 "Advanced panel POWER group follows host power availability",
                 "[android][advanced][power]") {
    // Sanity: a platform that supports host power shows the group.
    REQUIRE(lv_subject_get_int(host_power_subject()) == 1);
    CHECK_FALSE(lv_obj_has_flag(group_, LV_OBJ_FLAG_HIDDEN));

    // The feature: unsupported platforms hide the whole POWER group — rows and
    // header, no orphaned "POWER" heading left behind.
    lv_subject_set_int(host_power_subject(), 0);
    process_lvgl(10);
    CHECK(lv_obj_has_flag(group_, LV_OBJ_FLAG_HIDDEN));

    // And it comes back when the platform supports it again.
    lv_subject_set_int(host_power_subject(), 1);
    process_lvgl(10);
    CHECK_FALSE(lv_obj_has_flag(group_, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(XMLTestFixture, "show_shutdown_dialog is a no-op without host power support",
                 "[android][shutdown][power]") {
    REQUIRE(register_component("shutdown_modal"));
    ui_split_button_init();

    // Control case first: with host power supported the dialog shows, so the
    // suppression below can only be the platform guard.
    SECTION("supported platform shows the dialog") {
        ShutdownModal modal;
        helix::AsyncLifetimeGuard lifetime;
        set_platform_override(0);
        helix::show_shutdown_dialog(&api(), modal, lifetime, test_screen());
        CHECK(modal.is_visible());
    }

    // Both entry points (home widget, Advanced panel rows) funnel through
    // show_shutdown_dialog — the guard keeps any future caller honest too.
    SECTION("Android shows nothing") {
        PlatformOverrideGuard android(1);
        ShutdownModal modal;
        helix::AsyncLifetimeGuard lifetime;
        helix::show_shutdown_dialog(&api(), modal, lifetime, test_screen());
        CHECK_FALSE(modal.is_visible());
    }
}

TEST_CASE("platform_host_power_supported outlives its registering fixture",
          "[android][advanced][power][regression]") {
    // The XML subject registry is process-global and never forgets an entry,
    // so the subject behind "platform_host_power_supported" must live for the
    // whole binary. It was a fixture MEMBER until 2026-08-19: every test after
    // this fixture's death that touched the name (the home panel's
    // shutdown-widget gate walk in setup_gate_observers, XML
    // bind_state_if_eq) dereferenced a freed lv_subject_t — the nightly TSan
    // SIGSEGV in the gate-coalescing test and the 2026-08-16 ASan
    // heap-buffer-overflow in lv_subject_add_observer_obj were both this.
    {
        AdvancedPowerGroupFixture poison;
        (void)poison;
    } // registering fixture is dead here

    lv_subject_t* subj = lv_xml_get_subject(nullptr, "platform_host_power_supported");
    REQUIRE(subj != nullptr);

    // The exact operation that faulted: observe through the registry entry
    // after the registering fixture is gone. lv_ll_ins_tail walks the
    // subject's subscriber list — a dangling subject dies here.
    lv_observer_t* obs =
        lv_subject_add_observer(subj, [](lv_observer_t*, lv_subject_t*) {}, nullptr);
    REQUIRE(obs != nullptr);
    lv_subject_set_int(subj, 1);
    lv_observer_remove(obs);
}
