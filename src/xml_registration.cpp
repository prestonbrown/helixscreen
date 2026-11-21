// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xml_registration.h"

#include "ui_ams_current_tool.h"
#include "ui_ams_device_operations_overlay.h"
#include "ui_ams_device_section_detail_overlay.h"
#include "ui_ams_edit_overlay.h"
#include "ui_bed_drying_modal.h"
#include "ui_belt_gap_gauge.h"
#include "ui_belt_path_sketch.h"
#include "ui_button.h"
#include "ui_carousel.h"
#include "ui_confetti.h"
#include "ui_context_menu.h"
#include "ui_event_safety.h"
#include "ui_fan_dial.h"
#include "ui_fault_path.h"
#include "ui_fonts.h"
#include "ui_gcode_viewer.h"
#include "ui_icon_codepoints.h"
#include "ui_leader_line.h"
#include "ui_lock_screen.h"
#include "ui_markdown.h"
#include "ui_notification_badge.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_home.h"
#include "ui_panel_settings.h"
#include "ui_pin_entry_modal.h"
#include "ui_plugin_feature_row.h"
#include "ui_plugin_menu_rows.h"
#include "ui_printer_switch_menu.h"
#include "ui_progress_bar.h"
#include "ui_spinner.h"
#include "ui_split_button.h"
#include "ui_spool_canvas.h"
#include "ui_swatch.h"
#include "ui_switch.h"
#include "ui_text.h"
#include "ui_text_input.h"
#include "ui_toolhead_view.h"
#include "ui_z_offset_indicator.h"

#include "async_lifetime_guard.h"
#include "boot_yield.h"
#include "helix-xml/src/xml/lv_xml_component_private.h"
#include "helix_fs.h"
#include "layout_manager.h"
#include "page_scroll_auto_inject.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "z_offset_utils.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <string>
#include <unordered_set>

namespace helix {

/**
 * No-op callback for optional event handlers in XML components.
 * When a component has an optional callback prop with default="",
 * LVGL tries to find a callback named "" which doesn't exist.
 * Registering this no-op callback silences those warnings.
 */
static void noop_event_callback(lv_event_t* /*e*/) {
    // Intentionally empty - used for optional callbacks that weren't provided
}

/**
 * No-op subject for optional subject bindings in XML components.
 * When a component has an optional subject prop with default="",
 * LVGL tries to find a subject named "" which doesn't exist.
 * Registering this no-op subject silences those warnings.
 */
static lv_subject_t s_noop_subject;
static bool s_noop_subject_initialized = false;

/**
 * Register the responsive color preview size the theme preview's status dots
 * use. Called again on every layout refresh.
 */
void refresh_color_picker_constants() {
    lv_display_t* display = lv_display_get_default();
    const int32_t greater_res = LV_MAX(lv_display_get_horizontal_resolution(display),
                                       lv_display_get_vertical_resolution(display));
    const char* preview_size_small = greater_res <= UI_BREAKPOINT_MICRO_MAX    ? "16"
                                     : greater_res <= UI_BREAKPOINT_SMALL_MAX  ? "20"
                                     : greater_res <= UI_BREAKPOINT_MEDIUM_MAX ? "24"
                                                                               : "28";
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (scope) {
        // set, not register: register_const is first-write-wins, and a layout
        // refresh resolves it again.
        lv_xml_set_const(scope, "color_preview_size_small", preview_size_small);
    }
}

/**
 * Toggle password visibility on a sibling textarea.
 * Finds "password_input" by walking up to the shared parent container,
 * then swaps the eye/eye_off icon on the button.
 */
static void on_toggle_password_visibility(lv_event_t* e) {
    auto* btn = (lv_obj_t*)lv_event_get_target(e);
    auto* container = lv_obj_get_parent(btn);
    if (!container)
        return;

    auto* textarea = (lv_obj_t*)lv_obj_find_by_name(container, "password_input");
    if (!textarea)
        return;

    bool was_password = lv_textarea_get_password_mode(textarea);
    lv_textarea_set_password_mode(textarea, !was_password);

    // Swap icon: eye_off when hidden (password mode), eye when visible
    auto* icon = (lv_obj_t*)lv_obj_find_by_name(btn, "eye_toggle_icon");
    if (icon) {
        const char* cp = helix::ui::icon::lookup_codepoint(was_password ? "eye" : "eye_off");
        if (cp)
            lv_label_set_text(icon, cp);
    }
}

/**
 * Toggle visibility of a "description" label in setting rows.
 * Used on compact breakpoints where descriptions are hidden by default —
 * tapping the info icon reveals/hides the description text.
 * Walks up the parent chain to handle varying nesting depths across row types.
 */
static void on_setting_info_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[Settings] on_setting_info_clicked");
    auto* info_btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!info_btn)
        return;
    // Walk up parent chain to find a level that contains "description" child
    auto* parent = lv_obj_get_parent(info_btn);
    lv_obj_t* desc = nullptr;
    while (parent) {
        desc = lv_obj_find_by_name(parent, "description");
        if (desc)
            break;
        parent = lv_obj_get_parent(parent);
    }
    if (!desc)
        return;
    if (lv_obj_has_flag(desc, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(desc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(desc, LV_OBJ_FLAG_HIDDEN);
    }
    LVGL_SAFE_EVENT_CB_END();
}

static bool register_xml(const char* filename) {
    auto& lm = helix::LayoutManager::instance();
    std::string path = "A:" + lm.resolve_xml_path(filename);
    const bool ok = lv_xml_register_component_from_file(path.c_str()) == LV_RESULT_OK;
    if (!ok) {
        spdlog::error("[XML Registration] Failed to register: {}", path);
    }
    // Each registration is a frogfs decompress + expat parse; a panel nesting
    // many first-use components registers them back-to-back on the UI task,
    // which starves the idle task on ESP. Yield every few so the Task WDT never
    // fires. No-op on desktop (see boot_yield.h).
    static int s_reg_count = 0;
    if ((++s_reg_count & 0x0F) == 0) {
        HELIX_BOOT_YIELD();
    }
    return ok;
}

#if defined(HELIX_PLATFORM_ESP32)
extern "C" bool helix_on_ui_task(void);
#endif

/// The LVGL thread on every platform. ESP never records a main thread, so
/// on_main_thread() is true on every task there; ask the firmware instead.
static bool on_lvgl_thread() {
#if defined(HELIX_PLATFORM_ESP32)
    return helix_on_ui_task();
#else
    return helix::internal::on_main_thread();
#endif
}

/// Names with no file to load, or whose file failed to register. Lookups also
/// name widgets and slot prefixes that have no file; each costs one probe.
static std::unordered_set<std::string> s_no_file;
/// Components the loader registered: what a geometry change may drop.
static std::unordered_set<std::string> s_loaded;

/**
 * The engine's component loader: registers a component the first time a tag,
 * an extends= base, lv_xml_create() or a C++ scope lookup names it. The file is
 * <name>.xml at the top of ui_xml/ or under components/, through the same
 * layout-variant resolution register_xml() applies.
 */
static void register_on_first_use(const char* name) {
    if (!on_lvgl_thread()) {
        spdlog::error("[XML Registration] '{}' looked up off the LVGL thread; not registering",
                      name);
        return;
    }
    if (s_no_file.count(name))
        return;
    auto& lm = helix::LayoutManager::instance();
    for (const char* dir : {"", "components/"}) {
        std::string file = std::string(dir) + name + ".xml";
        if (helix::fs::exists(lm.resolve_xml_path(file))) {
            spdlog::debug("[XML Registration] Registering {} on first use", file);
            // A file that does not parse is not read again on every lookup; the
            // hot reloader registers a fixed copy from its own buffer.
            if (register_xml(file.c_str()))
                s_loaded.emplace(name);
            else
                s_no_file.emplace(name);
            return;
        }
    }
    s_no_file.emplace(name);
}

/// Components C++ pushed constants into after they registered.
static std::unordered_set<std::string> s_cpp_extended;

void keep_xml_component_registered(const char* name) {
    s_cpp_extended.emplace(name);
}

bool is_xml_component_cpp_extended(const std::string& name) {
    return name == "globals" || s_cpp_extended.count(name) > 0;
}

/// A scope C++ has extended (constants or a subject registered into it from
/// C++) or lent a style to another scope cannot be rebuilt from its file alone.
static bool scope_holds_cpp_state(const char* name, lv_xml_component_scope_t* scope) {
    if (scope->styles_borrowed || is_xml_component_cpp_extended(name))
        return true;
    for (void* node = lv_ll_get_head(&scope->subjects_ll); node != nullptr;
         node = lv_ll_get_next(&scope->subjects_ll, node)) {
        if (!static_cast<lv_xml_subject_t*>(node)->owned)
            return true;
    }
    return false;
}

void unregister_idle_xml_components() {
    s_no_file.clear();
    for (auto it = s_loaded.begin(); it != s_loaded.end();) {
        lv_xml_component_scope_t* scope = lv_xml_component_find_scope(it->c_str());
        if (scope && (scope->instance_cnt > 0 || scope_holds_cpp_state(it->c_str(), scope))) {
            ++it;
            continue;
        }
        if (scope)
            lv_xml_component_unregister(it->c_str());
        it = s_loaded.erase(it);
    }
}

void register_xml_on_first_use() {
    lv_xml_set_component_loader(register_on_first_use);
}

void stop_xml_on_first_use() {
    lv_xml_set_component_loader(nullptr);
}

void register_xml_components() {
    spdlog::trace("[XML Registration] Registering XML components...");

    // Every other component registers the first time something names it.
    register_xml_on_first_use();

    // Shared cross-file styles (ui_xml/styles.xml), referenced from any XML as
    // <style name="styles.<name>"/>. Must live here, not in init_theme() with
    // globals.xml: this function runs AFTER theme_manager_init() registered the
    // theme constants, and a style's #const values resolve at registration time
    // (globals.xml is parsed before theme init, so a theme-token style there
    // registers empty).
    register_xml("styles.xml");

    // Register responsive constants (AFTER globals, BEFORE components that use them)
    ui_switch_register_responsive_constants();
    refresh_color_picker_constants();

    // Register semantic text widgets (AFTER theme init, BEFORE components that use them)
    ui_text_init();
    ui_text_input_init();                      // <text_input> with bind_text support
    ui_spinner_init();                         // <spinner> with responsive sizing
    ui_button_init();                          // <ui_button> with variant styles and auto-contrast
    ui_split_button_init();                    // <ui_split_button> with primary action + dropdown
    ui_markdown_init();                        // <ui_markdown> with theme-aware markdown rendering
    ui_notification_badge_init();              // <notification_badge> with auto-contrast text
    helix::ui::register_leader_line_widget();  // <leader_line>, a bare lv_line for callouts
    helix::ui::register_swatch_paint_widget(); // <swatch_paint>, the one colour-swatch painter
    ui_carousel_init();                        // <ui_carousel> horizontal scroll-snap carousel
    ui_confetti_init();                        // <ui_confetti> celebration animation canvas

    // Register no-op callback and subject for optional handlers in XML components
    // This silences warnings when components use callback/subject props with default=""
    lv_xml_register_event_cb(nullptr, "", noop_event_callback);

    // Global utility callbacks used by multiple components
    lv_xml_register_event_cb(nullptr, "on_toggle_password_visibility",
                             on_toggle_password_visibility);
    lv_xml_register_event_cb(nullptr, "on_setting_info_clicked", on_setting_info_clicked);
    // Header save button. Global because header_bar is instantiated by ~40
    // panels and the button can surface on any of them.
    lv_xml_register_event_cb(nullptr, "on_header_save_z_offset",
                             [](lv_event_t*) { helix::zoffset::save_dirty_offsets_shared(); });
    lv_xml_register_event_cb(nullptr, "on_edit_done_clicked",
                             [](lv_event_t*) { get_global_home_panel().exit_grid_edit_mode(); });
    lv_xml_register_event_cb(nullptr, "on_edit_add_widget_clicked",
                             [](lv_event_t*) { get_global_home_panel().open_widget_catalog(); });
    lv_xml_register_event_cb(nullptr, "on_edit_delete_page_clicked", [](lv_event_t*) {
        get_global_home_panel().request_delete_edit_page();
    });

    // Backdrop tap and close/Done for every context menu — one pair, routed through
    // ContextMenu::active(), rather than a callback per menu.
    helix::ui::ContextMenu::register_shared_callbacks();
    // plugin_feature_toggle_row: any settings page may place one.
    helix::ui::register_plugin_feature_row_callbacks();
    lv_subject_init_int(&s_noop_subject, 0);
    lv_xml_register_subject(nullptr, "", &s_noop_subject);
    s_noop_subject_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("XmlSubjects", helix::deinit_xml_subjects,
                                                      StaticSubjectRegistry::Lifetime::Process);

    // Register custom widgets (BEFORE components that use them)
    helix::ui::gcode_viewer_register();
    ui_spool_canvas_register();             // Needed by Spoolman panel (and AMS panel)
    ui_z_offset_indicator_register();       // Z-offset nozzle indicator
    helix::ui::ui_toolhead_view_register(); // <toolhead_view>: the configured head, fitted
    ui_ams_current_tool_init();             // AMS current tool indicator callbacks
    // <fault_path> + its fault_path_segment subject, embedded by the modals
    // that can show a positioned lane fault.
    helix::ui::fault_path_register();
    // NOTE: Other AMS widgets (ams_slot, filament_path_canvas) are
    // registered lazily in ui_panel_ams.cpp when the AMS panel is first accessed

    // AMS slot editor (single overlay, internal views — spec §13)
    helix::ui::get_ams_edit_overlay().register_callbacks();
    refresh_color_picker_constants();
    // Every refresh of the layout tokens, so a resize or rotation moves the
    // picker with them. Once per process: register_xml_components() can run again.
    [[maybe_unused]] static const int color_picker_refresh_hook =
        theme_manager_add_layout_refresh_hook(
            [](lv_display_t*) { refresh_color_picker_constants(); });

    // Shared progress bar component (gradient indicator)
    ui_progress_bar_init();

    // Lock screen overlay (full-screen PIN entry on lv_layer_top)
    helix::ui::register_lock_screen_callbacks();

    // PIN entry modal (numeric keypad for security settings PIN set/change/remove)
    helix::ui::PinEntryModal::register_callbacks();

    helix::ui::register_bed_drying_callbacks();

    // Panel widget components (dynamic instantiation from PanelWidgetConfig)
    helix::ui::PrinterSwitchMenu::register_callbacks();
    helix::ui::register_plugin_menu_row_callbacks();

    register_fan_dial_callbacks(); // Register FanDial event callbacks

    // AMS Device Operations (accessed from Settings > AMS)
    helix::ui::get_ams_device_operations_overlay().register_callbacks();
    helix::ui::get_ams_device_section_detail_overlay().register_callbacks();

#if HELIX_HAS_BELT_TUNER
    helix::ui::register_belt_path_sketch_widget(); // before the panel XML that uses it
    helix::ui::register_belt_gap_gauge_widget();
#endif

    register_settings_panel_callbacks(); // Register callbacks before XML parse [L013]

    // Page-scroll-buttons policy: injects chevron gutters into overflowing
    // scrollable containers. Driven by the NavigationManager on_root_shown() hooks
    // and the Display-settings toggle callback — NOT a subject observer (see
    // PageScrollAutoInject::init). This call is the lifecycle setup hook.
    helix::ui::PageScrollAutoInject::instance().init();

    spdlog::trace("[XML Registration] XML component registration complete");
}

void deinit_xml_subjects() {
    if (s_noop_subject_initialized) {
        lv_subject_deinit(&s_noop_subject);
        s_noop_subject_initialized = false;
        spdlog::debug("[XML Registration] No-op subject deinitialized");
    }
}

} // namespace helix
