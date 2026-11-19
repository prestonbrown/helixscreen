// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_display.cpp
 * @brief Implementation of DisplaySettingsOverlay
 */

#include "ui_settings_display.h"

#include "ui_callback_helpers.h"
#include "ui_panel_settings.h" // get_global_settings_panel() owns the restart prompt
#include "ui_toast_manager.h"

#include "display_manager.h"
#include "display_metrics.h"
#include "display_settings_manager.h"
#include "lvgl.h"
#include "screensaver.h"
#include "theme_manager.h" // UI_SCREEN_MICRO_W/H
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace helix::settings {

namespace {

/// Explicit UI scale steps offered below Automatic, in dropdown order. 100% is
/// a real choice rather than a synonym for Automatic: it pins a high-DPI panel
/// back to authored sizing. The top of the range is
/// DisplayMetrics::kMaxScaleSettingPercent, so the dropdown cannot ask for a
/// scale the curve itself would refuse.
constexpr int kUiScalePercents[] = {100, 125, 150, 175, 200};
static_assert(kUiScalePercents[0] == helix::DisplayMetrics::kMinScaleSettingPercent);
static_assert(kUiScalePercents[std::size(kUiScalePercents) - 1] ==
              helix::DisplayMetrics::kMaxScaleSettingPercent);

constexpr const char* kOwner = "DisplaySettings";

lv_obj_t* row_dropdown(lv_obj_t* root, const char* row_name) {
    lv_obj_t* row = helix::ui::find_required(root, row_name, kOwner);
    return row ? helix::ui::find_required(row, "dropdown", kOwner) : nullptr;
}

/// The container holding row_screensaver and its sibling rows, from any widget inside it.
lv_obj_t* screensaver_rows_root(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
        const char* name = lv_obj_get_name(o);
        if (name && std::strcmp(name, "row_screensaver") == 0) {
            return lv_obj_get_parent(o);
        }
    }
    return nullptr;
}

int display_auto_percent() {
    return static_cast<int>(std::lround(helix::DisplayMetrics::auto_scale() * 100.0));
}

} // namespace

using helix::ui::event_checked;
using helix::ui::event_selected;

void DisplaySettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_display_rotation_changed",
         [](lv_event_t* e) {
             int degrees = DisplaySettingsManager::index_to_rotation_degrees(event_selected(e));
             // The rotation is read once, by DisplayManager at startup, and LVGL screens
             // never re-rotate afterwards. Only prompt when the applied value actually
             // moved - re-picking the current one needs no restart.
             if (DisplaySettingsManager::instance().set_display_rotation(degrees)) {
                 get_global_settings_panel().show_restart_prompt();
             }
         }},
        // Per drag tick: the backlight and the settings_brightness readout, not saved.
        // on_brightness_commit saves once, on release.
        {"on_brightness_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().preview_brightness(
                 lv_slider_get_value(lv_event_get_current_target_obj(e)));
         }},
        {"on_brightness_commit",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_brightness(
                 lv_slider_get_value(lv_event_get_current_target_obj(e)));
         }},
        {"on_ui_scale_changed", [](lv_event_t* e) { apply_ui_scale_pick(event_selected(e)); }},
        {"on_dim_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_display_dim_sec(
                 DisplaySettingsManager::index_to_dim_seconds(event_selected(e)));
         }},
        {"on_sleep_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_display_sleep_sec(
                 DisplaySettingsManager::index_to_sleep_seconds(event_selected(e)));
         }},
        {"on_sleep_while_printing_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_sleep_while_printing(event_checked(e));
         }},
        {"on_screensaver_print_progress_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_screensaver_print_progress(event_checked(e));
         }},
#if HELIX_ENABLE_SCREENSAVER
        {"on_screensaver_changed",
         [](lv_event_t* e) {
             lv_obj_t* dropdown = lv_event_get_current_target_obj(e);
             apply_screensaver_pick(screensaver_rows_root(dropdown), event_selected(e));
         }},
        {"on_test_screensaver",
         [](lv_event_t*) {
             int type = DisplaySettingsManager::instance().get_screensaver_type();
             if (type <= 0) {
                 return; // "Off": the button is hidden then, but guard anyway
             }
             auto* dm = DisplayManager::instance();
             if (!dm) {
                 spdlog::warn("[DisplaySettings] DisplayManager not available, cannot preview "
                              "screensaver");
                 return;
             }
             dm->preview_screensaver(type);
         }},
#else
        {"on_screensaver_changed", [](lv_event_t*) {}},
        {"on_test_screensaver", [](lv_event_t*) {}},
#endif
    });
}

void DisplaySettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_display_rotation_dropdown();
    select_brightness_slider(overlay_root_);
    select_dim_rows(overlay_root_);
    select_sleep_row(overlay_root_);
    populate_ui_scale_row(overlay_root_, UiScaleOffer::All);
    populate_screensaver_row(overlay_root_);
}

void DisplaySettingsOverlay::init_screensaver_dropdown() {
    populate_screensaver_row(overlay_root_);
}

void DisplaySettingsOverlay::handle_screensaver_changed(int index) {
    apply_screensaver_pick(overlay_root_, index);
}

// Rows whose widget index is not the stored value (rotation degrees, dim and
// sleep seconds, UI scale percent, screensaver type) are selected here; the
// toggle rows bind to their subjects in XML.
void DisplaySettingsOverlay::init_display_rotation_dropdown() {
    if (lv_obj_t* dropdown = row_dropdown(overlay_root_, "row_display_rotation")) {
        int degrees = DisplaySettingsManager::instance().get_display_rotation();
        int index = DisplaySettingsManager::rotation_degrees_to_index(degrees);
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));

        spdlog::debug("[{}] Screen rotation dropdown initialized to index {} ({}°)", get_name(),
                      index, degrees);
    }
}

// The slider takes its static XML value at construction; the readout binds
// settings_brightness.
void select_brightness_slider(lv_obj_t* root) {
    lv_obj_t* row = helix::ui::find_required(root, "row_brightness", kOwner);
    if (lv_obj_t* slider = row ? helix::ui::find_required(row, "slider", kOwner) : nullptr) {
        lv_slider_set_value(slider, DisplaySettingsManager::instance().get_brightness(),
                            LV_ANIM_OFF);
    }
}

// Both rows edit the dim timeout; settings_dim_row_kind shows at most one of them.
void select_dim_rows(lv_obj_t* root) {
    const int current_sec = DisplaySettingsManager::instance().get_display_dim_sec();
    const int index = DisplaySettingsManager::dim_seconds_to_index(current_sec);
    for (const char* row_name : {"row_display_dim", "row_screensaver_after"}) {
        if (lv_obj_t* dropdown = row_dropdown(root, row_name)) {
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        }
    }
    spdlog::debug("[{}] Dim rows at index {} ({}s)", kOwner, index, current_sec);
}

void select_sleep_row(lv_obj_t* root) {
    if (lv_obj_t* dropdown = row_dropdown(root, "row_display_sleep")) {
        const int current_sec = DisplaySettingsManager::instance().get_display_sleep_sec();
        const int index = DisplaySettingsManager::sleep_seconds_to_index(current_sec);
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::debug("[{}] Sleep row at index {} ({}s)", kOwner, index, current_sec);
    }
}

std::vector<int> fitting_ui_scale_percents(int width, int height) {
    const int long_side = std::max(width, height);
    const int short_side = std::min(width, height);
    std::vector<int> out;
    for (int percent : kUiScalePercents) {
        if (long_side * 100 >= UI_SCREEN_MICRO_W * percent &&
            short_side * 100 >= UI_SCREEN_MICRO_H * percent) {
            out.push_back(percent);
        }
    }
    return out;
}

bool ui_scale_has_choice(const std::vector<int>& fitting, int auto_percent) {
    return std::any_of(fitting.begin(), fitting.end(),
                       [auto_percent](int percent) { return percent != auto_percent; });
}

bool fitting_ui_scale_has_choice() {
    return ui_scale_has_choice(ui_scale_percents(UiScaleOffer::Fitting), display_auto_percent());
}

std::vector<int> ui_scale_percents(UiScaleOffer offer) {
    if (offer == UiScaleOffer::All) {
        return {std::begin(kUiScalePercents), std::end(kUiScalePercents)};
    }
    lv_display_t* display = lv_display_get_default();
    if (!display) {
        return {};
    }
    return fitting_ui_scale_percents(lv_display_get_horizontal_resolution(display),
                                     lv_display_get_vertical_resolution(display));
}

void populate_ui_scale_row(lv_obj_t* root, UiScaleOffer offer) {
    lv_obj_t* dropdown = row_dropdown(root, "row_ui_scale");
    if (!dropdown)
        return;

    // The Automatic row carries the percentage the panel's DPI resolved to, so
    // a user on a phone-class screen can see what it chose before deciding to
    // override it. Every shipping printer sits in the DPI deadband and reads
    // "Automatic (100%)".
    const int auto_percent = display_auto_percent();
    const std::vector<int> percents = ui_scale_percents(offer);
    std::string options =
        std::string(lv_tr("Automatic")) + " (" + std::to_string(auto_percent) + "%)";
    for (int percent : percents) {
        options += "\n" + std::to_string(percent) + "%";
    }
    lv_dropdown_set_options(dropdown, options.c_str());

    // Index 0 is Automatic; the explicit steps follow in list order. A stored
    // value that is not one of the offered steps (hand-edited, or a step we
    // later drop) selects Automatic, which is what it will actually behave as on
    // the next start.
    const int stored = DisplaySettingsManager::instance().get_ui_scale_percent();
    uint32_t selected = 0;
    for (size_t i = 0; i < percents.size(); ++i) {
        if (percents[i] == stored) {
            selected = static_cast<uint32_t>(i + 1);
            break;
        }
    }
    lv_dropdown_set_selected(dropdown, selected);

    spdlog::debug("[{}] UI scale row: stored={} auto={}% offered={}", kOwner, stored, auto_percent,
                  percents.size());
}

void apply_ui_scale_pick(int index) {
    // A Fitting list is a prefix of the full one (a bigger scale never fits where a
    // smaller one does not), so one index-to-percent mapping serves both rows.
    const std::vector<int> percents = ui_scale_percents(UiScaleOffer::All);
    const int percent = (index >= 1 && index <= static_cast<int>(percents.size()))
                            ? percents[static_cast<size_t>(index - 1)]
                            : helix::DisplayMetrics::kScaleSettingAutomatic;
    DisplaySettingsManager::instance().set_ui_scale_percent(percent);

    // Applying live would need every screen to re-tier, and AssetManager only
    // ever registers font tiers upward (register_fonts_for_tier() early-returns
    // on a lower tier), so a downward change cannot un-register the faces it
    // already handed out.
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("UI scale applies after restart"));
}

#if HELIX_ENABLE_SCREENSAVER
namespace {

// The row's options_tag puts the full XML list back on a language change; this runs after it
// and rebuilds the list the pick handler maps positions with.
void screensaver_language_changed_cb(lv_event_t* e) {
    populate_screensaver_row(screensaver_rows_root(lv_event_get_current_target_obj(e)));
}

bool has_language_rebuild(lv_obj_t* dropdown) {
    const uint32_t count = lv_obj_get_event_count(dropdown);
    for (uint32_t i = 0; i < count; i++) {
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(dropdown, i)) ==
            screensaver_language_changed_cb) {
            return true;
        }
    }
    return false;
}

} // namespace
#endif

void populate_screensaver_row(lv_obj_t* root) {
#if !HELIX_ENABLE_SCREENSAVER
    (void)root;
#else
    if (!root)
        return;
    lv_obj_t* dropdown = row_dropdown(root, "row_screensaver");
    if (!dropdown)
        return;
    if (!has_language_rebuild(dropdown)) {
        // DECLARATIVE_OK: rebuilds C++-built options after the engine's options_tag reset.
        lv_obj_add_event_cb(dropdown, screensaver_language_changed_cb,
                            LV_EVENT_TRANSLATION_LANGUAGE_CHANGED, nullptr);
    }

    const ScreensaverType configured = ScreensaverManager::configured_type();
    const ScreensaverManager::SettingsDropdown listed =
        ScreensaverManager::settings_dropdown(configured);
    std::string options;
    // The closed field shows the plain label: the marked one runs under the arrow at 800x480.
    std::string closed_text;
    for (ScreensaverType type : listed.choices) {
        if (!options.empty()) {
            options += "\n";
        }
        const char* label = lv_tr(helix::ui::screensaver_label_key(type));
        options += label;
        if (type == listed.runs && listed.runs_too_heavy) {
            options += std::string(" (") + lv_tr("too slow") + ")";
            closed_text = label;
        }
    }
    lv_dropdown_set_options(dropdown, options.c_str());
    lv_dropdown_set_text(dropdown, closed_text.empty() ? nullptr : closed_text.c_str());

    const size_t index = helix::ui::screensaver_choice_index(listed.choices, listed.runs);
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));

    spdlog::debug("[{}] Screensaver row: {} of {} savers offered, stored type {} runs as {}",
                  kOwner, listed.choices.size() - 1, helix::ui::kScreensaverCount,
                  static_cast<int>(configured), static_cast<int>(listed.runs));
#endif
}

void apply_screensaver_pick(lv_obj_t* root, int index) {
#if !HELIX_ENABLE_SCREENSAVER
    (void)root;
    (void)index;
#else
    // The row lists what settings_dropdown() gives for the stored type, and only a pick
    // changes that type, so the list is the same one the row was built from.
    const std::vector<ScreensaverType> choices =
        ScreensaverManager::settings_dropdown(ScreensaverManager::configured_type()).choices;
    if (index < 0 || static_cast<size_t>(index) >= choices.size()) {
        spdlog::warn("[{}] Screensaver dropdown index {} is outside the {} offered", kOwner, index,
                     choices.size());
        return;
    }
    DisplaySettingsManager::instance().set_screensaver_type(
        static_cast<int>(choices[static_cast<size_t>(index)]));
    if (!root) {
        return;
    }
    // A choice can change what the list offers: Off drops a too-slow stored saver.
    populate_screensaver_row(root);
    // Turning a saver on can seed the dim timeout, and that can raise sleep to match.
    select_dim_rows(root);
    select_sleep_row(root);
#endif
}

} // namespace helix::settings
