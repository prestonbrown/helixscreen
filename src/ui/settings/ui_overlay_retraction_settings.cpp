// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_retraction_settings.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_slider_scale.h"

#include "i_moonraker_api.h"
#include "lvgl.h"
#include "runtime_config.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace {

using helix::ui::SliderScale;

/// Per-row facts the slider cannot answer. Bounds are read from each row's own
/// lv_slider at tap time, so retraction_settings_overlay.xml stays the single
/// source of truth for the ranges.
struct FieldSpec {
    const char* title; ///< Keypad header
    const char* unit;  ///< Suffix beside the keypad display
    int divisor;       ///< Slider positions per real unit (see SliderScale)
    bool allow_decimal;
};

/// Indexed by RetractionSettingsOverlay::Field. The distance sliders hold
/// hundredths of a millimetre, matching the centimm arithmetic below.
constexpr FieldSpec kFieldSpecs[] = {
    {"Retract Length", "mm", 100, true},
    {"Retract Speed", "mm/s", 1, false},
    {"Unretract Extra", "mm", 100, true},
    {"Unretract Speed", "mm/s", 1, false},
};

static_assert(sizeof(kFieldSpecs) / sizeof(kFieldSpecs[0]) ==
                  static_cast<size_t>(RetractionSettingsOverlay::Field::Count),
              "FIELD_SPECS must have one entry per Field");

bool field_in_range(int raw) {
    return raw >= 0 && raw < static_cast<int>(RetractionSettingsOverlay::Field::Count);
}

} // namespace

RetractionSettingsOverlay::RetractionSettingsOverlay(helix::IMoonrakerAPI* api) : api_(api) {}

RetractionSettingsOverlay::~RetractionSettingsOverlay() {
    // SubjectManager handles LVGL initialization check and cleanup
    subjects_.deinit_all();
}

void RetractionSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(retract_length_display_, retract_length_buf_, "0.00mm",
                                  "retract_length_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(retract_speed_display_, retract_speed_buf_, "35mm/s",
                                  "retract_speed_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(unretract_extra_display_, unretract_extra_buf_, "0.00mm",
                                  "unretract_extra_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(unretract_speed_display_, unretract_speed_buf_, "35mm/s",
                                  "unretract_speed_display", subjects_);
    });
}

void RetractionSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_retraction_enabled_changed",
         [](lv_event_t* e) {
             const bool enabled = helix::ui::event_checked(e);
             spdlog::debug("[Retraction Settings] Enable toggled: {}", enabled);

             auto& overlay = get_global_retraction_settings();
             if (overlay.syncing_from_state_) {
                 return;
             }

             if (enabled) {
                 overlay.send_retraction_settings();
             } else if (overlay.api_) {
                 // Disable by setting retract length to 0
                 overlay.api_->execute_gcode("SET_RETRACTION RETRACT_LENGTH=0", nullptr, nullptr);
             }
         }},
        {"on_retraction_setting_changed",
         [](lv_event_t*) {
             auto& overlay = get_global_retraction_settings();
             overlay.update_display_labels();

             if (!overlay.syncing_from_state_) {
                 overlay.send_retraction_settings();
             }
         }},
        // Tappable value fields (numeric keypad entry); user_data carries the Field index as text.
        {"on_retraction_field_clicked",
         [](lv_event_t* e) {
             const char* index_str = static_cast<const char*>(lv_event_get_user_data(e));
             if (!index_str) {
                 return;
             }
             const int raw = static_cast<int>(std::strtol(index_str, nullptr, 10));
             if (!field_in_range(raw)) {
                 spdlog::warn("[Retraction Settings] Ignoring out-of-range field index {}", raw);
                 return;
             }
             get_global_retraction_settings().handle_field_clicked(static_cast<Field>(raw));
         }},
    });
}

lv_obj_t* RetractionSettingsOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, xml_component())) {
        return nullptr;
    }

    enable_switch_ =
        helix::ui::find_required(overlay_root_, "retraction_enabled_switch", get_name());
    retract_length_slider_ =
        helix::ui::find_required(overlay_root_, "retract_length_slider", get_name());
    retract_speed_slider_ =
        helix::ui::find_required(overlay_root_, "retract_speed_slider", get_name());
    unretract_extra_slider_ =
        helix::ui::find_required(overlay_root_, "unretract_extra_slider", get_name());
    unretract_speed_slider_ =
        helix::ui::find_required(overlay_root_, "unretract_speed_slider", get_name());

    return overlay_root_;
}

void RetractionSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    // Returning from our own keypad would re-sync the sliders from PrinterState
    // and discard the value the user just typed.
    if (returning_from_keypad_) {
        returning_from_keypad_ = false;
        spdlog::debug("[{}] Keeping typed value: returned from keypad", get_name());
        return;
    }

    sync_from_printer_state();
}

void RetractionSettingsOverlay::sync_from_printer_state() {
    syncing_from_state_ = true;

    // Get subjects from global registry (registered by PrinterState)
    lv_subject_t* length_subj = lv_xml_get_subject(nullptr, "retract_length");
    lv_subject_t* speed_subj = lv_xml_get_subject(nullptr, "retract_speed");
    lv_subject_t* extra_subj = lv_xml_get_subject(nullptr, "unretract_extra_length");
    lv_subject_t* uspeed_subj = lv_xml_get_subject(nullptr, "unretract_speed");

    if (!length_subj || !speed_subj || !extra_subj || !uspeed_subj) {
        spdlog::error("[{}] Required subjects not registered - cannot sync!", get_name());
        syncing_from_state_ = false;
        return;
    }

    // Get values (centimm for lengths)
    int retract_length_centimm = lv_subject_get_int(length_subj);
    int retract_speed = lv_subject_get_int(speed_subj);
    int unretract_extra_centimm = lv_subject_get_int(extra_subj);
    int unretract_speed = lv_subject_get_int(uspeed_subj);

    spdlog::debug("[{}] Syncing: length={}centimm speed={} extra={}centimm uspeed={}", get_name(),
                  retract_length_centimm, retract_speed, unretract_extra_centimm, unretract_speed);

    // Update enable switch (enabled if retract_length > 0)
    if (enable_switch_) {
        if (retract_length_centimm > 0) {
            lv_obj_add_state(enable_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(enable_switch_, LV_STATE_CHECKED);
        }
    }

    // Update sliders (length sliders are in centimm to match subject)
    if (retract_length_slider_) {
        lv_slider_set_value(retract_length_slider_, retract_length_centimm, LV_ANIM_OFF);
    }
    if (retract_speed_slider_) {
        lv_slider_set_value(retract_speed_slider_, retract_speed, LV_ANIM_OFF);
    }
    if (unretract_extra_slider_) {
        lv_slider_set_value(unretract_extra_slider_, unretract_extra_centimm, LV_ANIM_OFF);
    }
    if (unretract_speed_slider_) {
        lv_slider_set_value(unretract_speed_slider_, unretract_speed, LV_ANIM_OFF);
    }

    update_display_labels();
    syncing_from_state_ = false;
}

void RetractionSettingsOverlay::update_display_labels() {
    if (retract_length_slider_) {
        int centimm = lv_slider_get_value(retract_length_slider_);
        // Two decimals to match the slider's 0.01mm resolution and the %.2f
        // the G-code sends. One decimal displayed a typed 0.85 as "0.8mm".
        snprintf(retract_length_buf_, sizeof(retract_length_buf_), "%.2fmm", centimm / 100.0);
        lv_subject_copy_string(&retract_length_display_, retract_length_buf_);
    }

    if (retract_speed_slider_) {
        int speed = lv_slider_get_value(retract_speed_slider_);
        snprintf(retract_speed_buf_, sizeof(retract_speed_buf_), "%dmm/s", speed);
        lv_subject_copy_string(&retract_speed_display_, retract_speed_buf_);
    }

    if (unretract_extra_slider_) {
        int centimm = lv_slider_get_value(unretract_extra_slider_);
        snprintf(unretract_extra_buf_, sizeof(unretract_extra_buf_), "%.2fmm", centimm / 100.0);
        lv_subject_copy_string(&unretract_extra_display_, unretract_extra_buf_);
    }

    if (unretract_speed_slider_) {
        int speed = lv_slider_get_value(unretract_speed_slider_);
        snprintf(unretract_speed_buf_, sizeof(unretract_speed_buf_), "%dmm/s", speed);
        lv_subject_copy_string(&unretract_speed_display_, unretract_speed_buf_);
    }
}

void RetractionSettingsOverlay::send_retraction_settings() {
    if (!api_) {
        spdlog::debug("[{}] No API, skipping G-code send", get_name());
        return;
    }

    // Get current slider values
    int length_centimm = retract_length_slider_ ? lv_slider_get_value(retract_length_slider_) : 0;
    int retract_speed = retract_speed_slider_ ? lv_slider_get_value(retract_speed_slider_) : 35;
    int extra_centimm = unretract_extra_slider_ ? lv_slider_get_value(unretract_extra_slider_) : 0;
    int unretract_spd = unretract_speed_slider_ ? lv_slider_get_value(unretract_speed_slider_) : 35;

    // Convert centimm to mm
    double length_mm = length_centimm / 100.0;
    double extra_mm = extra_centimm / 100.0;

    // Build G-code command
    char gcode[128];
    snprintf(gcode, sizeof(gcode),
             "SET_RETRACTION RETRACT_LENGTH=%.2f RETRACT_SPEED=%d "
             "UNRETRACT_EXTRA_LENGTH=%.2f UNRETRACT_SPEED=%d",
             length_mm, retract_speed, extra_mm, unretract_spd);

    spdlog::debug("[{}] Sending: {}", get_name(), gcode);
    api_->execute_gcode(gcode, nullptr, nullptr);
}

// =============================================================================
// EVENT HANDLERS
// =============================================================================

lv_obj_t* RetractionSettingsOverlay::field_slider(Field field) const {
    switch (field) {
    case Field::RetractLength:
        return retract_length_slider_;
    case Field::RetractSpeed:
        return retract_speed_slider_;
    case Field::UnretractExtra:
        return unretract_extra_slider_;
    case Field::UnretractSpeed:
        return unretract_speed_slider_;
    case Field::Count:
        break;
    }
    return nullptr;
}

void RetractionSettingsOverlay::handle_field_clicked(Field field) {
    lv_obj_t* slider = field_slider(field);
    if (!slider) {
        spdlog::warn("[{}] No slider for field {}", get_name(), static_cast<int>(field));
        return;
    }

    const FieldSpec& spec = kFieldSpecs[static_cast<size_t>(field)];
    const SliderScale scale{spec.divisor};
    pending_keypad_field_ = field;

    ui_keypad_config_t config = {
        .initial_value = static_cast<float>(scale.to_value(lv_slider_get_value(slider))),
        .min_value = static_cast<float>(scale.to_value(lv_slider_get_min_value(slider))),
        .max_value = static_cast<float>(scale.to_value(lv_slider_get_max_value(slider))),
        // Titles are the same strings the rows already declare as
        // translation_tag in XML, so this resolves with no new keys.
        .title_label = lv_tr(spec.title),
        .unit_label = spec.unit,
        .allow_decimal = spec.allow_decimal,
        .allow_negative = false,
        .callback = on_keypad_value,
        .user_data = this};

    spdlog::debug("[{}] Keypad for {} ({}-{})", get_name(), spec.title, config.min_value,
                  config.max_value);
    ui_keypad_show(&config);
}

void RetractionSettingsOverlay::handle_keypad_value(Field field, double value) {
    // Set on confirm, not when the keypad opens: the keypad invokes this
    // callback before it hides, so the flag is always consumed by the
    // on_activate() that follows. Setting it at tap time leaked the flag
    // when the keypad was abandoned via the navbar, costing the next
    // visit its refresh from the printer.
    returning_from_keypad_ = true;

    lv_obj_t* slider = field_slider(field);
    if (!slider) {
        return;
    }

    const FieldSpec& spec = kFieldSpecs[static_cast<size_t>(field)];
    lv_slider_set_value(
        slider,
        SliderScale{spec.divisor}.to_slider_clamped(value, lv_slider_get_min_value(slider),
                                                    lv_slider_get_max_value(slider)),
        LV_ANIM_OFF);

    // Same path a drag takes: relabel, then send. Keeps one code path for both.
    update_display_labels();
    if (!syncing_from_state_) {
        send_retraction_settings();
    }
}

void RetractionSettingsOverlay::on_keypad_value(float value, void* user_data) {
    auto* self = static_cast<RetractionSettingsOverlay*>(user_data);
    if (!self) {
        return;
    }
    self->handle_keypad_value(self->pending_keypad_field_, static_cast<double>(value));
}
