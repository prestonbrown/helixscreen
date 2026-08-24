// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_factory.h"

#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_panel_advanced.h"
#include "ui_panel_controls.h"
#include "ui_panel_filament.h"
#include "ui_panel_home.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"
#include "ui_panel_settings.h"
#include "ui_utils.h" // safe_delete_deferred (deferred-panel loading scrim teardown)

#include "app_globals.h"
#include "boot_yield.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <chrono>

using namespace helix;

// Note: PanelOverlayAdapter was removed - PrintStatusPanel now inherits directly
// from OverlayBase, eliminating the need for an adapter.

bool PanelFactory::find_panels(lv_obj_t* panel_container) {
    m_panel_container = panel_container; // kept for deferred panel creation (ESP)
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        m_panels[i] = lv_obj_find_by_name(panel_container, PANEL_NAMES[i]);
        if (!m_panels[i]) {
#if defined(HELIX_PLATFORM_ESP32)
            // On ESP the app_layout override instantiates only home at boot; the
            // other five panels are created on first navigation. Their absence
            // here is expected — only home must be present.
            if (i != static_cast<int>(PanelId::Home)) {
                spdlog::debug("[PanelFactory] Panel '{}' deferred (built on first nav)",
                              PANEL_NAMES[i]);
                continue;
            }
#endif
            spdlog::error("[PanelFactory] Missing panel '{}' in container", PANEL_NAMES[i]);
            return false;
        }
    }
    spdlog::debug("[PanelFactory] Panels found in container");
    return true;
}

void PanelFactory::setup_one_panel(int panel_id) {
    lv_obj_t* obj = m_panels[panel_id];
    if (!obj)
        return;
    auto& nav = NavigationManager::instance();
    PanelBase* inst = nullptr;
    switch (static_cast<PanelId>(panel_id)) {
    case PanelId::Home:
        get_global_home_panel().setup(obj, m_screen);
        inst = &get_global_home_panel();
        break;
    case PanelId::PrintSelect: {
        auto* p = get_print_select_panel(get_printer_state(), nullptr);
        p->setup(obj, m_screen);
        inst = p;
        break;
    }
    case PanelId::Controls:
        get_global_controls_panel().setup(obj, m_screen);
        inst = &get_global_controls_panel();
        break;
    case PanelId::Filament:
        get_global_filament_panel().setup(obj, m_screen);
        inst = &get_global_filament_panel();
        break;
    case PanelId::Settings:
        get_global_settings_panel().setup(obj, m_screen);
        inst = &get_global_settings_panel();
        break;
    case PanelId::Advanced:
        get_global_advanced_panel().setup(obj, m_screen);
        inst = &get_global_advanced_panel();
        break;
    default:
        return;
    }
    nav.register_panel_instance(static_cast<PanelId>(panel_id), inst);
}

void PanelFactory::build_deferred_panel(int panel_id) {
#if defined(HELIX_PLATFORM_ESP32)
    if (!m_panel_container || !m_screen)
        return;
    if (panel_id <= static_cast<int>(PanelId::Home) || panel_id >= UI_PANEL_COUNT)
        return; // home is built eagerly; nothing else defers
    if (m_panels[panel_id])
        return; // already built

    // The loading scrim + paint-before-block is now owned by NavigationManager's
    // NavTransitionScrim guard, which wraps the whole nav transition (this build
    // runs under it). One scrim mechanism for all transitions, not two.
    auto build_start = std::chrono::steady_clock::now();
    lv_obj_t* obj =
        static_cast<lv_obj_t*>(lv_xml_create(m_panel_container, PANEL_NAMES[panel_id], nullptr));
    if (obj) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); // nav un-hides after we return
        m_panels[panel_id] = obj;
        NavigationManager::instance().replace_panel_widget(static_cast<PanelId>(panel_id), obj);
        setup_one_panel(panel_id);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                            build_start)
                      .count();
        spdlog::info("[PanelFactory] Deferred panel '{}' built in {:.0f}ms", PANEL_NAMES[panel_id],
                     ms);
    } else {
        spdlog::error("[PanelFactory] Deferred build of '{}' FAILED (lv_xml_create null)",
                      PANEL_NAMES[panel_id]);
    }
#else
    (void)panel_id;
#endif
}

void PanelFactory::setup_panels(lv_obj_t* screen) {
    m_screen = screen; // setup() target for eager + deferred panels
    // Register panels with navigation system
    NavigationManager::instance().set_panels(m_panels.data());

#if defined(HELIX_PLATFORM_ESP32)
    // ESP: build ONLY home at boot. Instantiating all six panels here is ~19s of
    // layout_update_core on the K-Touch (Plan 4 Task 6 profile). The other five
    // are created on first navigation via build_deferred_panel, registered as
    // NavigationManager's deferred builder below. See app_layout override.
    NavigationManager::instance().set_deferred_panel_builder(
        [this](int id) { build_deferred_panel(id); });
    setup_one_panel(static_cast<int>(PanelId::Home));
    NavigationManager::instance().activate_initial_panel();
    spdlog::debug("[PanelFactory] Home panel set up; 5 panels deferred to first navigation");
    return;
#endif

    // Each panel's setup() builds its full content subtree and forces layout —
    // a deep layout_update_core recursion. Yield to the idle task between panels
    // so the Task WDT never fires mid-build on ESP (HELIX_BOOT_YIELD is a no-op
    // everywhere else). See boot_yield.h.

    // Per-panel setup cost, logged so the deferral tradeoff can be judged from
    // the slowest hardware we ship rather than from a desktop. On a platform
    // that defers a panel to first navigation, its number here is what the
    // first tap would block for (plus that panel's XML subtree creation, which
    // happens earlier inside the app_layout build).
    //
    // Measured on a CC1 (armv7, 2 cores, 114 MB) 2026-08-15, for the question
    // "should Linux defer panels the way ESP32 does?":
    //   app_layout XML create (all six subtrees)  564-615ms, +896 kB RSS
    //   all six setup()                           163-170ms, +512 kB RSS
    //   worst single panel: filament 109-113ms, print_select 47-50ms,
    //   controls 4.7ms, settings 1.3ms, advanced/home ~0ms
    // So the whole eager build is ~1.4 MB and ~0.73s of a ~9.3s boot. Deferring
    // five panels could return at most ~1.2 MB, and only for panels the user
    // never opens, against a first-tap stall with no scrim (NavTransitionScrim
    // is ESP32-only). Not worth it on Linux — the answer came out "no", and the
    // numbers are here so it does not have to be re-derived.
    auto timed_setup = [](const char* name, auto&& fn) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                      .count();
        spdlog::debug("[PanelFactory] setup('{}') took {:.1f}ms", name, ms);
    };

    auto panels_t0 = std::chrono::steady_clock::now();

    timed_setup("home", [&] {
        get_global_home_panel().setup(m_panels[static_cast<int>(PanelId::Home)], screen);
    });
    HELIX_BOOT_YIELD();

    timed_setup("controls", [&] {
        get_global_controls_panel().setup(m_panels[static_cast<int>(PanelId::Controls)], screen);
    });
    HELIX_BOOT_YIELD();

    timed_setup("print_select", [&] {
        get_print_select_panel(get_printer_state(), nullptr)
            ->setup(m_panels[static_cast<int>(PanelId::PrintSelect)], screen);
    });
    HELIX_BOOT_YIELD();

    timed_setup("filament", [&] {
        get_global_filament_panel().setup(m_panels[static_cast<int>(PanelId::Filament)], screen);
    });
    HELIX_BOOT_YIELD();

    timed_setup("settings", [&] {
        get_global_settings_panel().setup(m_panels[static_cast<int>(PanelId::Settings)], screen);
    });
    HELIX_BOOT_YIELD();

    timed_setup("advanced", [&] {
        get_global_advanced_panel().setup(m_panels[static_cast<int>(PanelId::Advanced)], screen);
    });
    HELIX_BOOT_YIELD();

    spdlog::debug(
        "[PanelFactory] all six setup() calls took {:.1f}ms total",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - panels_t0)
            .count());

    // Register C++ panel instances for lifecycle dispatch (on_activate/on_deactivate)
    auto& nav = NavigationManager::instance();
    nav.register_panel_instance(PanelId::Home, &get_global_home_panel());
    nav.register_panel_instance(PanelId::PrintSelect,
                                get_print_select_panel(get_printer_state(), nullptr));
    nav.register_panel_instance(PanelId::Controls, &get_global_controls_panel());
    nav.register_panel_instance(PanelId::Filament, &get_global_filament_panel());
    nav.register_panel_instance(PanelId::Settings, &get_global_settings_panel());
    nav.register_panel_instance(PanelId::Advanced, &get_global_advanced_panel());

    // Activate initial panel now that all instances are registered
    // (set_panels() couldn't do this because instances weren't registered yet)
    nav.activate_initial_panel();

    spdlog::debug("[PanelFactory] All panels set up");
}

bool PanelFactory::create_print_status_overlay(lv_obj_t* screen) {
    // Initialize subjects eagerly so observer callbacks work from the start
    // (e.g., print state changes while user is on a different panel).
    // Widget tree creation is deferred to first push via PrintStatusPanel::push_overlay().
    auto& print_status = get_global_print_status_panel();
    if (!print_status.are_subjects_initialized()) {
        print_status.init_subjects();
    }

    // m_print_status_panel stays nullptr — widget tree is created lazily
    // on first push_overlay() call and destroyed on close to save memory.
    m_print_status_panel = nullptr;

    spdlog::debug("[PanelFactory] Print status overlay initialized (lazy creation)");
    return true;
}

void PanelFactory::init_keypad(lv_obj_t* screen) {
    ui_keypad_init(screen);
}

lv_obj_t* PanelFactory::create_overlay(lv_obj_t* screen, const char* component_name,
                                       const char* display_name) {
    spdlog::debug("[PanelFactory] Creating {} overlay", display_name);
    lv_obj_t* panel = (lv_obj_t*)lv_xml_create(screen, component_name, nullptr);
    if (!panel) {
        spdlog::error("[PanelFactory] Failed to create {} overlay from '{}'", display_name,
                      component_name);
    }
    return panel;
}
