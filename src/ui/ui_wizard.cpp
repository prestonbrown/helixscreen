// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard.h"

#include "ui_error_reporting.h"
#include "ui_keyboard_manager.h"
#include "ui_nav_manager.h"
#include "ui_panel_home.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_wizard_ams_identify.h"
#include "ui_wizard_connection.h"
#include "ui_wizard_fan_select.h"
#include "ui_wizard_filament_sensor_select.h"
#include "ui_wizard_heater_select.h"
#include "ui_wizard_input_shaper.h"
#include "ui_wizard_language_chooser.h"
#include "ui_wizard_led_select.h"
#include "ui_wizard_printer_identify.h"
#include "ui_wizard_summary.h"
#include "ui_wizard_telemetry.h"
#include "ui_wizard_touch_calibration.h"
#include "ui_wizard_wifi.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "hardware_validator.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "platform_info.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "system/crash_handler.h"
#include "theme_manager.h"
#include "wizard_config_paths.h"
#include "wizard_step.h"
#include "wizard_step_logic.h"
#include "wizard_step_registry.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <functional>
#include <optional>
#include <vector>

using namespace helix;

// Subject declarations (static/global scope required)
static lv_subject_t current_step;
static lv_subject_t total_steps;
static lv_subject_t wizard_title;
static lv_subject_t wizard_step_current;  // String for display, e.g., "1"
static lv_subject_t wizard_step_total;    // String for display, e.g., "7"
static lv_subject_t wizard_is_final_step; // Int: 0=not final, 1=final (for button visibility)
static lv_subject_t wizard_back_visible;

// Non-static: accessible from other wizard step files
lv_subject_t connection_test_passed; // Global: 0=connection not validated, 1=validated or N/A
lv_subject_t wizard_subtitle;        // Global: accessible for dynamic subtitle updates
lv_subject_t wizard_show_skip;       // Global: 0=show Next, 1=show Skip (for touch calibration)

// SubjectManager for RAII cleanup of wizard subjects
static SubjectManager wizard_subjects_;

// String buffers (must be persistent)
static char wizard_title_buffer[64];
static char wizard_step_current_buffer[8];
static char wizard_step_total_buffer[8];
static char wizard_subtitle_buffer[128];

// Wizard container instance
static lv_obj_t* wizard_container = nullptr;

// Track current screen for proper cleanup (nullopt = no screen loaded yet)
static std::optional<helix::wizard::StepId> current_screen_step;

// Guard against rapid double-clicks during navigation
static bool navigating = false;

// Targeted (subset) wizard session state. Non-empty iff a targeted session is
// active — running only these steps, in this order, for hardware
// reconfiguration without re-running the full first-run wizard. When empty, all
// navigation behaves exactly as the normal first-run wizard (every subset
// branch is gated on !g_step_subset.empty()).
static std::vector<helix::wizard::StepId> g_step_subset;
static std::function<void()> g_targeted_on_complete;

// Index of `step` within the active subset, or -1 if not present.
static int subset_index_of(helix::wizard::StepId step) {
    for (size_t i = 0; i < g_step_subset.size(); ++i) {
        if (g_step_subset[i] == step) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Forward declarations
static void on_back_clicked(lv_event_t* e);
static void on_next_clicked(lv_event_t* e);
static void ui_wizard_load_screen(helix::wizard::StepId step);
static void ui_wizard_cleanup_current_screen();
static void dismiss_wizard_container();
static const char* get_step_title_from_xml(const char* comp_name);
static const char* get_step_subtitle_from_xml(const char* comp_name);
static void ui_wizard_purge_subtree_anims(lv_obj_t* root);

// Recursively cancel every pending/in-flight animation targeting the subtree
// before the tree is destroyed. Two distinct cancellation calls are needed:
//
//   1. `lv_anim_delete(obj, NULL)` matches anims whose `var == obj`
//      (lv_anim_set_var(&a, obj)) — direct property animations.
//
//   2. `lv_obj_remove_style_all(obj)` triggers `trans_delete` per `is_trans`
//      style entry on the obj, which calls `lv_anim_delete(tr, NULL)` for
//      the matching style-transition animation. Style transitions set
//      `var = tr` (a `trans_t*`, see `lv_obj_style_create_transition` in
//      `lv_obj_style.c:465`) — `lv_anim_delete(obj, NULL)` cannot match
//      them. The previous fix relied on (1) alone, leaving transition
//      anims live; a delayed `trans_anim_start_cb` then SEGVs in
//      `lv_obj_get_style_prop` on the freed `trans_t` after the next
//      step rebuilds the tree (#871, #880, bundle XBPDDJVK on v0.99.45).
//
// Mirror `lv_obj_destructor`'s pattern: disable style refresh around the
// `remove_style_all` call so we don't kick off invalidations on widgets
// that are about to be destroyed by `lv_obj_clean` immediately after.
//
// IMPORTANT: `remove_style_all` is only safe on descendants — they're
// about to be deleted by `lv_obj_clean`. The root container itself
// SURVIVES the clean and must keep its flex/layout styles, otherwise
// the next step's widgets are added to a styleless container and the
// layout collapses (regression caught on AD5M after the first attempt
// at this fix). The recursion descends through children only; for the
// initial call we visit the root just for `lv_anim_delete`.
static void ui_wizard_purge_subtree_anims_recursive(lv_obj_t* obj) {
    if (!obj)
        return;
    lv_obj_enable_style_refresh(false);
    lv_obj_remove_style_all(obj);
    lv_obj_enable_style_refresh(true);
    lv_anim_delete(obj, nullptr);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        ui_wizard_purge_subtree_anims_recursive(lv_obj_get_child(obj, static_cast<int32_t>(i)));
    }
}

static void ui_wizard_purge_subtree_anims(lv_obj_t* root) {
    if (!root)
        return;
    // Cancel anims targeting the root itself (var == root) but DO NOT
    // strip its styles — root survives the upcoming lv_obj_clean.
    lv_anim_delete(root, nullptr);
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        ui_wizard_purge_subtree_anims_recursive(lv_obj_get_child(root, static_cast<int32_t>(i)));
    }
}

// Async navigation: defer ui_wizard_navigate_to_step() from click handlers so
// lv_obj_clean() doesn't run inside LVGL's indev_proc_release traversal — that
// race corrupts the global event linked list and aborts in libc (#848/#843).
// The caller sets navigating=true first; the callback path clears it when
// navigate_to_step finishes.
static void navigate_to_step_async_cb(void* data) {
    auto step =
        static_cast<helix::wizard::StepId>(static_cast<int>(reinterpret_cast<intptr_t>(data)));
    ui_wizard_navigate_to_step(step);
}

static void schedule_navigate_to_step(helix::wizard::StepId step) {
    lv_async_call(navigate_to_step_async_cb,
                  reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<int>(step))));
}

// ============================================================================
// Step Metadata (read from XML <consts>)
// ============================================================================

/**
 * Get step title from XML component's <consts> block
 *
 * Each wizard step XML file defines:
 *   <consts>
 *     <str name="step_title" value="WiFi Setup"/>
 *     <int name="step_order" value="1"/>
 *   </consts>
 *
 * This function reads step_title from the component's scope at runtime,
 * eliminating hardcoded title strings in C++. The component name comes from
 * the step registry (Step::component_name()).
 */
static const char* get_step_title_from_xml(const char* comp_name) {
    if (!comp_name) {
        return "Unknown Step";
    }

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(comp_name);
    if (!scope) {
        spdlog::warn("[Wizard] Component scope not found for '{}'", comp_name);
        return "Unknown Step";
    }

    const char* title = lv_xml_get_const(scope, "step_title");
    if (!title) {
        spdlog::warn("[Wizard] step_title not found in '{}' consts", comp_name);
        return "Unknown Step";
    }

    return title;
}

/**
 * Get step subtitle from XML component's <consts> block
 *
 * Subtitles provide contextual hints (e.g., "Skip if using Ethernet")
 * that appear below the title in the wizard header.
 */
static const char* get_step_subtitle_from_xml(const char* comp_name) {
    if (!comp_name) {
        return "";
    }

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(comp_name);
    if (!scope) {
        return "";
    }

    const char* subtitle = lv_xml_get_const(scope, "step_subtitle");
    return subtitle ? subtitle : "";
}

// Track if subjects have been initialized (to avoid double-deinit)
static bool wizard_subjects_initialized = false;

void ui_wizard_init_subjects() {
    // Idempotent: the targeted-reconfig path may call this after the first-run
    // wizard already initialized subjects. Re-initializing would re-register the
    // managed subjects (double-init). Bail out if already initialized.
    if (wizard_subjects_initialized) {
        spdlog::debug("[Wizard] Subjects already initialized; skipping re-init");
        return;
    }
    spdlog::debug("[Wizard] Initializing subjects");

    // Log the preset-driven skip plan (pre-configured printer package). The
    // actual skip decisions are recomputed at every navigation point from the
    // step registry's per-step should_skip(ctx) (see helix::wizard::build_context
    // / skip_vector), so there is no persistent skip state to seed here.
    auto* cfg = Config::get_instance();
    helix::wizard::StepContext init_ctx = helix::wizard::build_context();
    if (init_ctx.preset.first_run) {
        spdlog::info("[Wizard] Preset mode active (preset: {})", cfg ? cfg->get_preset() : "");
    } else if (init_ctx.preset.skip_hardware) {
        spdlog::info("[Wizard] Subsequent printer with preset '{}': hardware steps will be "
                     "skipped, summary shown",
                     cfg ? cfg->get_preset() : "");
    }

    // Initialize subjects with defaults using managed macros for RAII cleanup
    UI_MANAGED_SUBJECT_INT(current_step, 1, "current_step", wizard_subjects_);
    UI_MANAGED_SUBJECT_INT(total_steps, 10, "total_steps",
                           wizard_subjects_); // 10 steps: WiFi, Connection, Printer, Heater,
                                              // Fan, AMS, LED, Filament, Input Shaper, Summary

    UI_MANAGED_SUBJECT_STRING(wizard_title, wizard_title_buffer, "Welcome", "wizard_title",
                              wizard_subjects_);
    UI_MANAGED_SUBJECT_STRING(wizard_step_current, wizard_step_current_buffer, "1",
                              "wizard_step_current", wizard_subjects_);
    UI_MANAGED_SUBJECT_STRING(wizard_step_total, wizard_step_total_buffer, "9", "wizard_step_total",
                              wizard_subjects_);
    UI_MANAGED_SUBJECT_INT(wizard_is_final_step, 0, "wizard_is_final_step", wizard_subjects_);
    UI_MANAGED_SUBJECT_STRING(wizard_subtitle, wizard_subtitle_buffer, "", "wizard_subtitle",
                              wizard_subjects_);

    // Initialize connection_test_passed to 1 (enabled by default for all steps)
    // Step 2 (connection) will set it to 0 until test passes
    UI_MANAGED_SUBJECT_INT(connection_test_passed, 1, "connection_test_passed", wizard_subjects_);

    // Initialize wizard_back_visible to 1 (visible by default)
    // Step navigation will hide it when at first visible step
    UI_MANAGED_SUBJECT_INT(wizard_back_visible, 1, "wizard_back_visible", wizard_subjects_);

    // Initialize wizard_show_skip to 0 (show Next by default)
    // Touch calibration step sets to 1 to show Skip button instead
    UI_MANAGED_SUBJECT_INT(wizard_show_skip, 0, "wizard_show_skip", wizard_subjects_);

    wizard_subjects_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy("WizardSubjects", ui_wizard_deinit_subjects);

    spdlog::debug("[Wizard] Subjects initialized ({} subjects registered)",
                  wizard_subjects_.count());
}

void ui_wizard_deinit_subjects() {
    if (!wizard_subjects_initialized) {
        return;
    }

    // Reset screen step tracking FIRST to prevent cleanup from accessing
    // already-destroyed wizard step objects. During StaticPanelRegistry::destroy_all(),
    // step objects (registered lazily after WizardSubjects) are destroyed first in LIFO
    // order. If cleanup calls their getters, the getter re-creates the object and calls
    // register_destroy(), invalidating the destroy_all() iterator → crash.
    // The step destructors already handled their own cleanup when their unique_ptrs were reset.
    current_screen_step.reset();

    // Delete wizard container BEFORE deinitializing subjects
    // This triggers proper widget cleanup: DELETE callbacks fire
    // and remove observers from subjects while subjects are still valid.
    // Without this, shutdown while on a wizard page would leave widgets
    // with observers pointing to deinitialized subjects, causing crashes
    // in lv_deinit() when those widgets are deleted.
    if (wizard_container && lv_is_initialized()) {
        spdlog::debug("[Wizard] Deleting wizard container during deinit");
        helix::ui::safe_delete(wizard_container);
        current_screen_step.reset();
    }

    // Clear any targeted (subset) session state so a later full wizard run is
    // never accidentally treated as a subset session.
    g_step_subset.clear();
    g_targeted_on_complete = nullptr;

    // Use SubjectManager for RAII cleanup - handles all registered subjects
    wizard_subjects_.deinit_all();
    wizard_subjects_initialized = false;
    spdlog::debug("[Wizard] Subjects deinitialized");
}

// Helper type for constant name/value pairs
struct WizardConstant {
    const char* name;
    const char* value;
};

// Helper: Register array of constants to a scope
static void register_constants_to_scope(lv_xml_component_scope_t* scope,
                                        const WizardConstant* constants) {
    if (!scope)
        return;
    for (int i = 0; constants[i].name != nullptr; i++) {
        lv_xml_register_const(scope, constants[i].name, constants[i].value);
    }
}

void ui_wizard_container_register_responsive_constants() {
    spdlog::debug("[Wizard] Registering responsive constants to wizard_container scope");

    // Detect screen size using custom breakpoints
    lv_display_t* display = lv_display_get_default();
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // Determine button width based on breakpoint (only responsive constant remaining)
    const char* button_width;
    const char* size_label;

    if (greater_res <= UI_BREAKPOINT_MICRO_MAX) { // ≤272: 480x272
        button_width = "90";
        size_label = "MICRO";
    } else if (greater_res <= UI_BREAKPOINT_SMALL_MAX) { // 273-460
        // TINY/SMALL share presets — button widths are too similar to warrant separate tiers
        button_width = "110";
        size_label = "TINY/SMALL";
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) { // 481-800: 800x480
        button_width = "140";
        size_label = "MEDIUM";
    } else { // >800: 1024x600+
        button_width = "160";
        size_label = "LARGE";
    }

    spdlog::debug("[Wizard] Screen size: {} (greater_res={}px)", size_label, greater_res);

    // Register button width constant
    WizardConstant constants[] = {
        {"wizard_button_width", button_width}, {nullptr, nullptr} // Sentinel
    };

    // Register to wizard_container scope (parent)
    lv_xml_component_scope_t* parent_scope = lv_xml_component_get_scope("wizard_container");
    register_constants_to_scope(parent_scope, constants);

    // Define child components that inherit this constant
    const char* children[] = {
        "wizard_touch_calibration",
        "wizard_wifi_setup",
        "wizard_connection",
        "wizard_printer_identify",
        "wizard_heater_select",
        "wizard_fan_select",
        "wizard_ams_identify",
        "wizard_led_select",
        "wizard_filament_sensor_select",
        "wizard_input_shaper",
        "wizard_language_chooser",
        "wizard_summary",
        "wizard_telemetry",
        nullptr // Sentinel
    };

    // Propagate to all children
    int child_count = 0;
    for (int i = 0; children[i] != nullptr; i++) {
        lv_xml_component_scope_t* child_scope = lv_xml_component_get_scope(children[i]);
        if (child_scope) {
            register_constants_to_scope(child_scope, constants);
            child_count++;
        }
    }

    spdlog::debug(
        "[Wizard] Registered wizard_button_width={} to wizard_container and {} child components",
        button_width, child_count);
}

void ui_wizard_register_event_callbacks() {
    spdlog::debug("[Wizard] Registering event callbacks");
    lv_xml_register_event_cb(nullptr, "on_back_clicked", on_back_clicked);
    lv_xml_register_event_cb(nullptr, "on_next_clicked", on_next_clicked);
}

lv_obj_t* ui_wizard_create(lv_obj_t* parent) {
    spdlog::debug("[Wizard] Creating wizard container");

    // Create wizard from XML (constants already registered)
    wizard_container = (lv_obj_t*)lv_xml_create(parent, "wizard_container", nullptr);

    if (!wizard_container) {
        spdlog::error("[Wizard] Failed to create wizard_container from XML");
        return nullptr;
    }

    // Background color applied automatically by LVGL theme (uses theme->color_card)
    // No explicit styling needed - theme patching in ui_theme.cpp handles this

    // Update layout to ensure SIZE_CONTENT calculates correctly
    lv_obj_update_layout(wizard_container);

    spdlog::debug("[Wizard] Wizard container created successfully");
    return wizard_container;
}

const std::vector<helix::wizard::StepId>& ui_wizard_active_step_subset() {
    return g_step_subset;
}

lv_obj_t* ui_wizard_create_targeted(lv_obj_t* parent, std::vector<helix::wizard::StepId> steps,
                                    std::function<void()> on_complete) {
    g_step_subset = std::move(steps);
    g_targeted_on_complete = std::move(on_complete);
    spdlog::info("[Wizard] Creating targeted session with {} step(s)", g_step_subset.size());

    // Mark the wizard active for the duration of the targeted session (cleared
    // by dismiss_wizard_container() on completion).
    set_wizard_active(true);

    // Reuse the standard container/subject setup.
    lv_obj_t* root = ui_wizard_create(parent);

    // Navigate to the first requested step. The subset being non-empty makes the
    // Next/Back handlers and progress display honor subset order.
    if (!g_step_subset.empty()) {
        ui_wizard_navigate_to_step(g_step_subset.front());
    }
    return root;
}

void ui_wizard_navigate_to_step(helix::wizard::StepId step) {
    using helix::wizard::StepId;
    spdlog::debug("[Wizard] Navigating to step {}", helix::wizard::to_string(step));

    // When starting the wizard from the very first step, forward past any of the
    // leading steps (touch calibration, language, WiFi) that should be skipped.
    // Per-step should_skip(ctx) already folds in fbdev/already-calibrated,
    // language-already-set, Android, and subsequent-printer logic, so a single
    // wizard_next() over the live skip vector replaces the old hand-rolled
    // cascade.
    //
    // Targeted (subset) sessions run an explicit step list and must never
    // auto-skip out of it, so this leading-step forwarding is gated off.
    if (g_step_subset.empty() && step == StepId::TouchCalibration) {
        auto ctx = helix::wizard::build_context();
        auto skips = helix::wizard::skip_vector(ctx);
        // If the first step itself is skipped, advance to the first visible one.
        if (!skips.empty() && skips.front().id == StepId::TouchCalibration &&
            skips.front().skipped) {
            auto nxt = helix::wizard_next(StepId::TouchCalibration, skips);
            if (nxt) {
                spdlog::info("[Wizard] First step skipped; starting at {}",
                             helix::wizard::to_string(*nxt));
                step = *nxt;
            }
        }
    }

    const int step_idx = static_cast<int>(step);

    // Recompute the skip vector for progress + button state. should_skip(ctx)
    // queries live hardware/config state, so the vector is always current.
    auto ctx = helix::wizard::build_context();
    auto skips = helix::wizard::skip_vector(ctx);

    // Progress numbers, first/last and "previous exists" facts. In a targeted
    // (subset) session these come from the subset's own order; otherwise they
    // come from the live skip vector over the full step list (unchanged path).
    int display_step;
    int display_total;
    bool at_first_visible;
    bool is_last_step;
    if (!g_step_subset.empty()) {
        int sub_idx = subset_index_of(step);
        if (sub_idx < 0) {
            sub_idx = 0;
        }
        display_step = sub_idx + 1;
        display_total = static_cast<int>(g_step_subset.size());
        at_first_visible = (sub_idx == 0);
        is_last_step = (sub_idx + 1 >= static_cast<int>(g_step_subset.size()));
    } else {
        display_step = helix::wizard_display_number(step, skips);
        display_total = helix::wizard_visible_count(skips);
        at_first_visible = !helix::wizard_prev(step, skips).has_value();
        is_last_step = helix::wizard_is_last(step, skips);
    }

    // Update current_step subject (internal step index for UI bindings)
    crash_handler::breadcrumb::note("wiz", "notify_current", static_cast<long>(step_idx));
    lv_subject_set_int(&current_step, step_idx);

    // Back button: visible when there is a previous non-skipped step, or when an
    // add-printer cancel callback is registered (shown as "Cancel" on step one).
    bool has_cancel = (get_wizard_cancel_callback() != nullptr);
    crash_handler::breadcrumb::note("wiz", "notify_back_vis", static_cast<long>(step_idx));
    lv_subject_set_int(&wizard_back_visible, (!at_first_visible || has_cancel) ? 1 : 0);

    // Show "Cancel" instead of "Back" on the first step when add-printer cancel is available
    if (wizard_container) {
        lv_obj_t* btn_back = lv_obj_find_by_name(wizard_container, "btn_back");
        if (btn_back) {
            bool is_first_step = at_first_visible;
            const char* btn_label_text = (is_first_step && has_cancel) ? "Cancel" : "Back";
            // ui_button stores label as lv_label child — find it by type
            lv_obj_t* lbl = nullptr;
            uint32_t child_count = lv_obj_get_child_count(btn_back);
            for (uint32_t i = 0; i < child_count; i++) {
                lv_obj_t* child = lv_obj_get_child(btn_back, i);
                if (lv_obj_check_type(child, &lv_label_class)) {
                    lbl = child;
                    break;
                }
            }
            if (lbl) {
                lv_label_set_text(lbl, btn_label_text);
            }
        }
    }

    // Update final step flag for button visibility binding
    crash_handler::breadcrumb::note("wiz", "notify_final", static_cast<long>(step_idx));
    lv_subject_set_int(&wizard_is_final_step, is_last_step ? 1 : 0);

    // Update progress display - step numbers as strings for bind_text
    snprintf(wizard_step_current_buffer, sizeof(wizard_step_current_buffer), "%d", display_step);
    crash_handler::breadcrumb::note("wiz", "notify_step_cur", static_cast<long>(step_idx));
    lv_subject_copy_string(&wizard_step_current, wizard_step_current_buffer);

    snprintf(wizard_step_total_buffer, sizeof(wizard_step_total_buffer), "%d", display_total);
    crash_handler::breadcrumb::note("wiz", "notify_step_tot", static_cast<long>(step_idx));
    lv_subject_copy_string(&wizard_step_total, wizard_step_total_buffer);

    // Load screen content
    ui_wizard_load_screen(step);

    // Force layout update on entire wizard after screen is loaded
    if (wizard_container) {
        lv_obj_update_layout(wizard_container);
    }

    // Allow next navigation click
    navigating = false;

    spdlog::debug("[Wizard] Updated to step {} of {} (internal: {}), final: {}", display_step,
                  display_total, step_idx, is_last_step);
}

void ui_wizard_set_title(const char* title) {
    if (!title) {
        spdlog::warn("[Wizard] set_title called with nullptr, ignoring");
        return;
    }

    spdlog::debug("[Wizard] Setting title: {}", title);
    lv_subject_copy_string(&wizard_title, title);
}

void ui_wizard_refresh_header_translations() {
    // Re-translate and set the title/subtitle for the current step
    // Called after language changes to update bound subjects with new translations
    //
    // Note: Progress text ("Step X of Y") and buttons (Next/Finish) now use
    // translation_tag in XML, so they auto-refresh. Only title/subtitle need
    // manual refresh since they're step-specific and loaded from XML consts.
    int step_idx = lv_subject_get_int(&current_step);
    helix::wizard::Step* s =
        helix::wizard::step_by_id(static_cast<helix::wizard::StepId>(step_idx));
    const char* comp_name = s ? s->component_name() : nullptr;
    const char* title = get_step_title_from_xml(comp_name);
    const char* subtitle = get_step_subtitle_from_xml(comp_name);

    lv_subject_copy_string(&wizard_title, lv_tr(title));
    lv_subject_copy_string(&wizard_subtitle, lv_tr(subtitle));

    spdlog::debug("[Wizard] Refreshed header translations for step {}", step_idx);
}

/// Ensure the FilamentSensorManager is populated before any filament-sensor
/// skip decision. The filament step's should_skip(ctx) reads the manager, which
/// is empty until discovery runs; the connection step's post-success burst
/// normally fills it, but jumping directly to the filament step (tests, resume)
/// can outrun that. Idempotent.
static void ui_wizard_ensure_filament_sensors_discovered() {
    auto& fsm = helix::FilamentSensorManager::instance();
    if (!fsm.get_sensors().empty()) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api && api->hardware().has_filament_sensors()) {
        fsm.discover_sensors(api->hardware().filament_sensor_names());
        spdlog::debug("[Wizard] Populated FilamentSensorManager for skip calculation");
    }
}

// ============================================================================
// Screen Cleanup
// ============================================================================

/**
 * Cleanup the current wizard screen before navigating to a new one
 *
 * Calls the appropriate cleanup function based on current_screen_step.
 * This ensures resources are properly released and screen pointers are reset.
 */
static void ui_wizard_cleanup_current_screen() {
    if (!current_screen_step) {
        return; // No screen loaded yet
    }

    helix::wizard::Step* s = helix::wizard::step_by_id(*current_screen_step);
    if (!s) {
        spdlog::warn("[Wizard] Unknown screen step {} during cleanup",
                     static_cast<int>(*current_screen_step));
        return;
    }
    spdlog::debug("[Wizard] Cleaning up screen for step {}", s->log_name());
    s->cleanup();
}

// ============================================================================
// Screen Loading
// ============================================================================

static void ui_wizard_load_screen(helix::wizard::StepId step) {
    using helix::wizard::StepId;
    const int step_idx = static_cast<int>(step);
    spdlog::debug("[Wizard] Loading screen for step {}", helix::wizard::to_string(step));

    helix::wizard::Step* s = helix::wizard::step_by_id(step);
    if (!s) {
        spdlog::warn("[Wizard] Invalid step {}, ignoring", step_idx);
        return;
    }

    // Find wizard_content container
    lv_obj_t* content = lv_obj_find_by_name(wizard_container, "wizard_content");
    if (!content) {
        spdlog::error("[Wizard] wizard_content container not found");
        return;
    }

    const int prev_idx = current_screen_step ? static_cast<int>(*current_screen_step) : -1;

    // Cleanup previous screen resources BEFORE clearing widgets. Freeze the
    // update queue around cleanup + drain + destroy so background-thread
    // callbacks (Moonraker WebSocket, HTTP executor) enqueued while the step
    // was running can't fire after its widgets are gone. Without this the
    // connection step's post-success burst (subscription notifications, probe/
    // LED discovery, PrinterNameSync, hardware validator) can race with
    // lv_obj_clean and corrupt the heap, manifesting as SIGABRT/std::terminate
    // in the next step's XML/subject init (#793 for WiFi, #827 for the
    // connection → printer-identify transition).
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        crash_handler::breadcrumb::note("wiz", "cleanup_begin", static_cast<long>(prev_idx));
        ui_wizard_cleanup_current_screen();
        crash_handler::breadcrumb::note("wiz", "drain_begin", static_cast<long>(step_idx));
        helix::ui::UpdateQueue::instance().drain();
        // Cancel every pending animation targeting any descendant of the wizard
        // content container before deletion. Style transitions (lv_obj_set_style_*
        // with a transition descriptor) queue animations that aren't tied to the
        // styled widget — `lv_anim_delete(obj, NULL)` in the destructor doesn't
        // catch them, so a delayed `trans_anim_start_cb` can fire on a freed
        // widget after the next step has rebuilt the tree at the same address
        // (#871).
        crash_handler::breadcrumb::note("wiz", "anim_purge", static_cast<long>(step_idx));
        ui_wizard_purge_subtree_anims(content);
        crash_handler::breadcrumb::note("wiz", "clean_begin", static_cast<long>(step_idx));
        lv_obj_clean(content);
        crash_handler::breadcrumb::note("wiz", "clean_end", static_cast<long>(step_idx));
    }
    spdlog::debug("[Wizard] Cleared wizard_content container");

    // Printer-identify step applies a preset on cleanup based on the user's pick
    // (or the auto-detection result). On a fresh install this is the first moment
    // a preset can possibly be applied — when we left the connection step no
    // preset existed yet. Rebuild the context + skip vector now (it reads the
    // freshly-written preset via Config::has_preset()) so newly-skippable
    // hardware steps collapse, then forward to the next non-skipped step if our
    // requested destination is now skipped.
    if (prev_idx == static_cast<int>(StepId::PrinterIdentify) &&
        step_idx > static_cast<int>(StepId::PrinterIdentify)) {
        auto ctx = helix::wizard::build_context();
        auto skips = helix::wizard::skip_vector(ctx);
        StepId redirected = step;
        // If the requested step is now skipped, walk forward to the first visible.
        for (const auto& entry : skips) {
            if (entry.id == redirected && entry.skipped) {
                auto nxt = helix::wizard_next(redirected, skips);
                if (!nxt) {
                    spdlog::info("[Wizard] Preset applied during printer-identify cleanup; "
                                 "no further steps, completing");
                    current_screen_step.reset(); // suppress double-cleanup
                    ui_wizard_complete();
                    return;
                }
                redirected = *nxt;
                break;
            }
        }
        if (redirected != step) {
            spdlog::info("[Wizard] Preset applied during printer-identify cleanup; redirecting "
                         "{} -> {}",
                         helix::wizard::to_string(step), helix::wizard::to_string(redirected));
            current_screen_step.reset(); // suppress double-cleanup
            ui_wizard_navigate_to_step(redirected);
            return;
        }
    }

    // Set title and subtitle from XML metadata (no more hardcoded strings!)
    // Use lv_tr() to translate the title/subtitle dynamically based on current language
    // Per-notify breadcrumbs pin which subject observer is faulty if #848/#843 recurs.
    const char* comp_name = s->component_name();
    const char* title = get_step_title_from_xml(comp_name);
    crash_handler::breadcrumb::note("wiz", "notify_title", static_cast<long>(step_idx));
    ui_wizard_set_title(lv_tr(title));
    const char* subtitle = get_step_subtitle_from_xml(comp_name);
    crash_handler::breadcrumb::note("wiz", "notify_subtitle", static_cast<long>(step_idx));
    lv_subject_copy_string(&wizard_subtitle, lv_tr(subtitle));

    // Default Next button to enabled - steps that gate on validation (language,
    // connection, printer identify, fan select) will set it to 0 in their init
    crash_handler::breadcrumb::note("wiz", "notify_test_passed", static_cast<long>(step_idx));
    lv_subject_set_int(&connection_test_passed, 1);

    // Language step: disable Next until a language is selected (must happen before
    // create() so the gated state is set when widgets bind).
    if (step == StepId::Language) {
        lv_subject_set_int(&connection_test_passed, 0);
    }

    // Generic create path — every step exposes the same lifecycle via the registry.
    crash_handler::breadcrumb::note("wiz", "create_begin", static_cast<long>(step_idx));
    spdlog::debug("[Wizard] Creating screen for {}", s->log_name());
    s->init_subjects();
    s->register_callbacks();
    s->create(content);
    lv_obj_update_layout(content);

    // Step-specific post-create hooks that the generic path can't express.
    switch (step) {
    case StepId::Wifi:
        get_wizard_wifi_step()->init_wifi_manager();
        break;
    case StepId::PrinterIdentify:
        // Override subtitle with dynamic detection status
        lv_subject_copy_string(&wizard_subtitle,
                               get_wizard_printer_identify_step()->get_detection_status());
        break;
    case StepId::FilamentSensor: {
        // Schedule refresh in case sensors are discovered after screen creation
        // (handles race condition when jumping directly to the filament step).
        auto* fstep = get_wizard_filament_sensor_select_step();
        fstep->refresh_timer_ = lv_timer_create(
            [](lv_timer_t*) {
                auto* fs = get_wizard_filament_sensor_select_step();
                fs->refresh_timer_ = nullptr;
                fs->refresh();
            },
            1500, nullptr);
        lv_timer_set_repeat_count(fstep->refresh_timer_, 1);
        break;
    }
    default:
        break;
    }
    crash_handler::breadcrumb::note("wiz", "create_end", static_cast<long>(step_idx));

    // Update current screen step tracking
    current_screen_step = step;
}

// ============================================================================
// Wizard Completion
// ============================================================================

// Shared container teardown used by BOTH completion paths (DRY).
//
// Performs only the container/UI teardown that is identical between the full
// first-run completion (ui_wizard_complete) and the targeted completion
// (ui_wizard_complete_targeted): clean up the active step, dismiss the
// keyboard, async-delete the container, and clear the global active flag.
//
// It deliberately does NOT touch config flags, expected-hardware, the
// post-wizard re-discovery, or the deferred Home navigation — those are
// completion-mode specific and remain in ui_wizard_complete().
static void dismiss_wizard_container() {
    // Cleanup current wizard screen
    ui_wizard_cleanup_current_screen();

    // Dismiss any on-screen keyboard left over from wizard text inputs
    // (prevents stale textarea focus from wizard_connection step)
    KeyboardManager::instance().hide();

    // Delete wizard container (main UI is already created underneath)
    // SAFETY: Use lv_obj_del_async — the Finish button that triggered this call is a
    // child of wizard_container. Synchronous delete causes use-after-free (issue #80).
    // Hide immediately so user sees the app layout underneath without waiting for async delete.
    if (wizard_container) {
        spdlog::debug("[Wizard] Hiding and deleting wizard container (async)");
        lv_obj_add_flag(wizard_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_del_async(wizard_container);
        wizard_container = nullptr;
    }

    // Clear global wizard state
    set_wizard_active(false);
}

std::vector<helix::wizard::StepId> ui_wizard_deferred_hardware_steps() {
    // Populate the filament manager first — the filament step's skip decision
    // reads it, and an unpopulated manager would drop the step from the offer.
    ui_wizard_ensure_filament_sensors_discovered();
    auto ctx = helix::wizard::build_context();
    return helix::wizard_deferred_hardware_steps(helix::wizard::skip_vector(ctx));
}

size_t ui_wizard_record_expected_hardware(helix::Config* config) {
    if (!config) {
        return 0;
    }

    // Heater / fan / LED roles the user picked (or a preset supplied).
    const char* hardware_suffixes[] = {
        helix::wizard::BED_HEATER,    // "heaters/bed"
        helix::wizard::HOTEND_HEATER, // "heaters/hotend"
        helix::wizard::PART_FAN,      // "fans/part"
        helix::wizard::HOTEND_FAN,    // "fans/hotend"
        helix::wizard::LED_STRIP      // "leds/strip"
    };

    std::vector<std::string> names;
    for (const auto* suffix : hardware_suffixes) {
        std::string hw_name = config->get<std::string>(config->df() + suffix, "");
        if (!hw_name.empty() && hw_name != "None") {
            names.push_back(std::move(hw_name));
        }
    }

    // User-selected runout sensor.
    {
        auto& sensor_mgr = helix::FilamentSensorManager::instance();
        auto sensors = sensor_mgr.get_sensors();
        for (const auto& sensor : sensors) {
            if (sensor.role == helix::FilamentSensorRole::RUNOUT && !sensor.klipper_name.empty()) {
                names.push_back(sensor.klipper_name);
                break;
            }
        }
    }

    // AMS, if detected. This lets the hardware validator warn if the AMS
    // disappears between sessions. Gate on the AMS step's hardware check
    // (no-arg should_skip() == "no AMS detected") rather than a tracked skip
    // flag — a preset printer that physically has an AMS still has hardware
    // worth recording.
    if (!get_wizard_ams_identify_step()->should_skip()) {
        auto& ams = AmsState::instance();
        AmsBackend* backend = ams.get_backend();
        if (backend) {
            std::string ams_hw_name = backend->get_klipper_object_name();
            if (!ams_hw_name.empty()) {
                names.push_back(std::move(ams_hw_name));
            }

            // Enable AMS widget on home panel since AMS hardware was detected
            auto& wc = PanelWidgetManager::instance().get_widget_config("home");
            if (wc.set_enabled_by_id("ams", true)) {
                wc.save();
                spdlog::info("[Wizard] Enabled AMS widget on home panel");
            }
        }
    }

    for (const auto& name : names) {
        HardwareValidator::add_expected_hardware(config, name);
        spdlog::debug("[Wizard] Added '{}' to expected_hardware", name);
    }
    return names.size();
}

void ui_wizard_complete() {
    spdlog::info("[Wizard] Completing wizard and transitioning to main UI");

    // Any full completion ends targeted mode unconditionally (defense in depth):
    // guarantees the next wizard run is never accidentally a subset session.
    g_step_subset.clear();
    g_targeted_on_complete = nullptr;

    // 1. Mark wizard as completed in config
    Config* config = Config::get_instance();
    if (config) {
        spdlog::debug("[Wizard] Setting wizard_completed flag");
        config->set<bool>(config->df() + "wizard_completed", true);
        // Also set root-level for backward compat
        config->set<bool>("/wizard_completed", true);

        // 1b. Populate expected_hardware from wizard selections so later
        // discoveries don't report already-present hardware as newly appeared.
        const size_t recorded = ui_wizard_record_expected_hardware(config);

        // 1c. A run that never reached Klipper had empty pickers and recorded
        // nothing. Committing that emptiness as the final snapshot makes the
        // first boot where Klipper does come up flag every fan, filament sensor
        // and LED as new (#1160). Record the debt instead; the first successful
        // discovery pays it and offers the skipped hardware steps.
        //
        // Discovery success is read from the live hardware rather than a wizard
        // flag: Klipper always reports at least an [extruder], so an empty heater
        // list is the one unambiguous "discovery never ran" signal available at
        // completion, and it stays correct no matter which path reached Finish.
        IMoonrakerAPI* api = get_moonraker_api();
        const bool discovery_succeeded = api != nullptr && !api->hardware().heaters().empty();
        helix::wizard_apply_hardware_snapshot_decision(config, discovery_succeeded, recorded > 0);

        if (!config->save()) {
            NOTIFY_ERROR(lv_tr("Failed to save setup completion"));
        }
    } else {
        LOG_ERROR_INTERNAL("[Wizard] Failed to get config instance to mark wizard complete");
    }

    // 2-5. Shared container teardown (cleanup screen, hide keyboard, delete
    // container, clear active flag).
    dismiss_wizard_container();

    // 6. Schedule deferred runout check - modal may need to show after wizard
    lv_timer_create(
        [](lv_timer_t* timer) {
            auto& fsm = helix::FilamentSensorManager::instance();
            if (fsm.has_real_runout() && get_runtime_config()->should_show_runout_modal()) {
                spdlog::debug("[Wizard] Deferred runout check - triggering modal");
                get_global_home_panel().trigger_idle_runout_check();
            }
            lv_timer_delete(timer);
        },
        500, nullptr); // 500ms delay for UI to stabilize

    // 7. Trigger re-discovery through Application's pre-registered callbacks.
    // Discovery callbacks (set_hardware, init_fans, hardware validation, plugin detection,
    // etc.) were registered in Application::init_moonraker() via setup_discovery_callbacks().
    IMoonrakerClient* client = get_moonraker_client();
    if (client && client->get_connection_state() == ConnectionState::CONNECTED) {
        client->discover_printer([]() { spdlog::info("[Wizard] Post-wizard discovery complete"); });
    } else {
        spdlog::warn("[Wizard] Not connected after wizard - subsystems will initialize on restart");
    }

    // Tell Home Panel to reload immediately for printer image, type overlay
    // (LED and other hardware will update async when discovery completes)
    get_global_home_panel().apply_printer_config();

    // Defer navigation to Home panel — discovery callbacks queue deferred subject updates
    // via ui_queue_update() that can override panel state. A short timer ensures we
    // navigate AFTER those queued updates have been processed.
    lv_timer_create(
        [](lv_timer_t* timer) {
            spdlog::info("[Wizard] Deferred navigation to Home panel");
            NavigationManager::instance().set_active(PanelId::Home);
            lv_timer_delete(timer);
        },
        100, nullptr);

    // Show success toast when adding a subsequent printer
    if (config && config->get_printer_ids().size() > 1) {
        NOTIFY_SUCCESS(lv_tr("New printer successfully added"));
    }

    spdlog::info("[Wizard] Wizard complete, transitioning to main UI");
}

void ui_wizard_complete_targeted() {
    spdlog::info("[Wizard] Targeted reconfiguration complete");

    // NOTE: do NOT set wizard_completed here — targeted sessions reconfigure
    // specific hardware step(s) without re-running / re-finishing the full
    // first-run wizard, so the completion flag and expected-hardware population
    // done by ui_wizard_complete() are intentionally skipped.

    // Capture + clear the callback and subset BEFORE tearing down / firing, so
    // a later full wizard run is never treated as a subset session and the
    // callback can safely (re)launch UI.
    auto cb = g_targeted_on_complete;
    g_step_subset.clear();
    g_targeted_on_complete = nullptr;

    // Shared teardown (same as the full-wizard path, minus the flag writes).
    dismiss_wizard_container();
    current_screen_step.reset();

    if (cb) {
        cb();
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_back_clicked(lv_event_t* e) {
    using helix::wizard::StepId;
    (void)e;
    if (navigating)
        return;
    navigating = true;
    auto current = static_cast<StepId>(lv_subject_get_int(&current_step));

    // Targeted (subset) session: retreat only within the requested subset.
    if (!g_step_subset.empty()) {
        int idx = subset_index_of(current);
        if (idx > 0) {
            schedule_navigate_to_step(g_step_subset[idx - 1]);
            spdlog::debug("[Wizard] Targeted back, step: {}",
                          helix::wizard::to_string(g_step_subset[idx - 1]));
            return;
        }
        // At the first subset step — invoke cancel callback if one is registered.
        auto cancel_cb = get_wizard_cancel_callback();
        if (cancel_cb) {
            spdlog::info("[Wizard] Targeted back from first step — invoking cancel callback");
            cancel_cb();
        }
        navigating = false;
        return;
    }

    auto ctx = helix::wizard::build_context();
    auto skips = helix::wizard::skip_vector(ctx);
    auto prev = helix::wizard_prev(current, skips);

    if (prev) {
        schedule_navigate_to_step(*prev);
        spdlog::debug("[Wizard] Back button clicked, step: {}", helix::wizard::to_string(*prev));
        return;
    }

    // At first visible step — invoke cancel callback if registered (add-printer mode)
    auto cancel_cb = get_wizard_cancel_callback();
    if (cancel_cb) {
        spdlog::info("[Wizard] Back from first step — invoking cancel callback");
        cancel_cb();
    }
    navigating = false;
}

/// True if the filament-sensor step is being skipped purely because of sparse
/// standalone sensors (not because a preset covers the hardware). Task 16: when
/// exactly one standalone sensor exists in that case, auto-configure it as the
/// runout sensor before we pass over the step.
static void ui_wizard_maybe_auto_configure_single_sensor(const helix::wizard::StepContext& ctx) {
    if (ctx.preset.skip_hardware) {
        return; // Skipped due to preset, not sparse sensors — do not auto-configure.
    }
    ui_wizard_ensure_filament_sensors_discovered();
    auto* fstep = get_wizard_filament_sensor_select_step();
    if (!fstep->should_skip()) {
        return; // Not sparse — the step will be shown, nothing to auto-configure.
    }
    if (fstep->get_standalone_sensor_count() == 1) {
        fstep->auto_configure_single_sensor();
        spdlog::info("[Wizard] Auto-configured single filament sensor as RUNOUT");
    }
}

static void on_next_clicked(lv_event_t* e) {
    using helix::wizard::StepId;
    (void)e;
    if (navigating)
        return;
    navigating = true;
    auto current = static_cast<StepId>(lv_subject_get_int(&current_step));

    // Targeted (subset) session: advance only within the requested subset; the
    // last subset step finishes via ui_wizard_complete_targeted() (which does
    // NOT set wizard_completed).
    if (!g_step_subset.empty()) {
        int idx = subset_index_of(current);
        // idx < 0 (current not in subset) is unreachable in practice because
        // navigate_to_step always sets current_step to a subset member; handled
        // defensively as completion alongside the normal last-step case.
        if (idx < 0 || idx + 1 >= static_cast<int>(g_step_subset.size())) {
            spdlog::info("[Wizard] Targeted subset finished, completing targeted session");
            ui_wizard_complete_targeted();
            navigating = false;
            return;
        }
        schedule_navigate_to_step(g_step_subset[idx + 1]);
        spdlog::debug("[Wizard] Targeted next, step: {}",
                      helix::wizard::to_string(g_step_subset[idx + 1]));
        return;
    }

    // Ensure the filament manager is populated so the filament step's skip
    // decision (and the single-sensor auto-config side effect) is accurate.
    ui_wizard_ensure_filament_sensors_discovered();

    auto ctx = helix::wizard::build_context();
    auto skips = helix::wizard::skip_vector(ctx);

    // Check if this is the last step (no more non-skipped steps after this)
    if (helix::wizard_is_last(current, skips)) {
        spdlog::info("[Wizard] Finish button clicked, completing wizard");
        ui_wizard_complete();
        navigating = false;
        return;
    }

    // Commit touch calibration when leaving it (only saves if user completed calibration)
    if (current == StepId::TouchCalibration) {
        get_wizard_touch_calibration_step()->commit_calibration();
    }

    auto next = helix::wizard_next(current, skips);
    if (!next) {
        // Defensive: wizard_is_last already returned false, so this should not
        // happen, but treat a missing next as completion.
        ui_wizard_complete();
        navigating = false;
        return;
    }

    // Preset first-run fast path: auto-validate the connection step. If the
    // printer is already connected and a complete preset is applied, the
    // connection step has nothing for the user to do — skip straight past it.
    // build_context() reads Config::has_preset() live, so a preset applied by
    // auto-detection between wizard init and now is honored here.
    if (*next == StepId::Connection && ctx.preset.first_run) {
        IMoonrakerClient* client = get_moonraker_client();
        if (client && client->get_connection_state() == ConnectionState::CONNECTED) {
            spdlog::info("[Wizard] Preset mode: already connected, skipping connection step");
            auto after_conn = helix::wizard_next(StepId::Connection, skips);
            if (!after_conn) {
                ui_wizard_complete();
                navigating = false;
                return;
            }
            // The filament step may be among the steps we jump over; honor the
            // single-sensor auto-config side effect if applicable.
            ui_wizard_maybe_auto_configure_single_sensor(ctx);
            schedule_navigate_to_step(*after_conn);
            return;
        }
    }

    // Task 16: if the forward jump passes OVER a sparse-skipped filament step,
    // auto-configure a lone standalone sensor as RUNOUT before we move on.
    // wizard_next already excluded the (skipped) filament step from *next, so
    // detect the pass-over by index range [current, next).
    {
        int cur_idx = static_cast<int>(current);
        int next_idx = static_cast<int>(*next);
        int fil_idx = static_cast<int>(StepId::FilamentSensor);
        if (fil_idx > cur_idx && fil_idx < next_idx) {
            ui_wizard_maybe_auto_configure_single_sensor(ctx);
        }
    }

    schedule_navigate_to_step(*next);
    spdlog::debug("[Wizard] Next button clicked, step: {}", helix::wizard::to_string(*next));
}
