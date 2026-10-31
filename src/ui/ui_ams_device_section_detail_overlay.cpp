// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_device_section_detail_overlay.cpp
 * @brief Implementation of AmsDeviceSectionDetailOverlay
 */

#include "ui_ams_device_section_detail_overlay.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_guided_device_flow.h"
#include "ui_modal.h"
#include "ui_slider_scale.h"
#include "ui_text.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_device_action_value.h"
#include "ams_state.h"
#include "ams_types.h"
#include "display_numbering.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "static_panel_registry.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace helix::ui {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

// ============================================================================
// DESTRUCTOR
// ============================================================================

AmsDeviceSectionDetailOverlay::~AmsDeviceSectionDetailOverlay() = default;

// ============================================================================
// INITIALIZATION
// ============================================================================

void AmsDeviceSectionDetailOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Constant 1: a disabled action's row names it as its `disabled` subject, so the
        // row component greys its label and control the way every settings row does.
        UI_MANAGED_SUBJECT_INT(action_disabled_subject_, 1, kActionDisabledSubject, subjects_);
    });
}

void AmsDeviceSectionDetailOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_device_action_clicked", on_action_clicked},
        {"on_device_action_toggled", on_toggle_changed},
        {"on_device_action_slider_moved", on_slider_changed},
        {"on_device_action_slider_released", on_slider_released},
        {"on_device_action_value_clicked", on_value_clicked},
        {"on_device_action_dropdown_changed", on_dropdown_changed},
    });
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AmsDeviceSectionDetailOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // The dynamic actions container
    actions_container_ =
        helix::ui::find_required(overlay_root_, "section_actions_container", get_name());
    return overlay_root_;
}

void AmsDeviceSectionDetailOverlay::show(lv_obj_t* parent_screen, const std::string& section_id,
                                         const std::string& section_label) {
    section_id_ = section_id;
    section_label_ = section_label;
    OverlayBase::show(parent_screen);
}

void AmsDeviceSectionDetailOverlay::before_show() {
    // The overlay_panel title is static at XML creation, so it is set here. The name
    // comes from the extended overlay_panel, not this component's own XML. The section
    // name stands alone: the back button already leads to the system it belongs to, and
    // a 480px header has room for little more than the name.
    if (lv_obj_t* header_title =
            helix::ui::find_required(overlay_root_, "header_title", get_name())) {
        lv_label_set_text(header_title, lv_tr(section_label_.c_str()));
    }
    refresh();
}

void AmsDeviceSectionDetailOverlay::on_activate() {
    OverlayBase::on_activate();
    // A backend's action values can change without the user touching them (a
    // driver switching a mode itself), so the section follows every AMS data
    // tick. observe<int> defers the handler, so the rebuild never runs inside
    // the subject's notify.
    auto& ams = AmsState::instance();
    data_observer_ = observe<int>(
        ams.get_ams_data_revision_subject(), this,
        [](AmsDeviceSectionDetailOverlay* self, int) { self->refresh_if_changed(); },
        ams.get_subjects_lifetime());
}

void AmsDeviceSectionDetailOverlay::on_deactivating(DeactivateReason) {
    data_observer_.reset();
}

void AmsDeviceSectionDetailOverlay::refresh_if_changed() {
    if (!is_visible()) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    // A rebuild under the user's finger would drop a drag or an open dropdown,
    // so only a change a row shows rebuilds the section.
    if (device_section_changed(cached_actions_, backend->get_device_actions(), section_id_)) {
        refresh();
    }
}

bool device_section_changed(const std::vector<helix::printer::DeviceAction>& shown,
                            const std::vector<helix::printer::DeviceAction>& fresh,
                            const std::string& section) {
    auto next_in_section = [&section](const auto& actions, size_t i) {
        while (i < actions.size() && actions[i].section != section) {
            ++i;
        }
        return i;
    };
    size_t i = next_in_section(shown, 0);
    size_t j = next_in_section(fresh, 0);
    for (; i < shown.size() && j < fresh.size();
         i = next_in_section(shown, i + 1), j = next_in_section(fresh, j + 1)) {
        const auto& a = shown[i];
        const auto& b = fresh[j];
        if (a.id != b.id || a.enabled != b.enabled || a.disable_reason != b.disable_reason ||
            a.options != b.options ||
            !helix::printer::device_action_values_equal(a.current_value, b.current_value)) {
            return true;
        }
    }
    return i < shown.size() || j < fresh.size();
}

void AmsDeviceSectionDetailOverlay::refresh() {
    if (!overlay_ || !actions_container_) {
        return;
    }

    spdlog::debug("[{}] Refreshing section '{}' from backend", get_name(), section_id_);

    helix::ui::safe_clean_children(actions_container_);

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[{}] No backend available", get_name());
        cached_actions_.clear();
        return;
    }

    cached_actions_ = backend->get_device_actions();

    int count = 0;
    lv_obj_t* button_row = nullptr;
    int button_count_in_row = 0;

    for (const auto& action : cached_actions_) {
        if (action.section != section_id_) {
            continue;
        }

        if (action.type == helix::printer::ActionType::BUTTON) {
            // Two buttons to a row, sharing its width.
            if (!button_row || button_count_in_row >= 2) {
                button_row = static_cast<lv_obj_t*>(
                    lv_xml_create(actions_container_, "device_action_button_row", nullptr));
                if (!button_row) {
                    spdlog::warn("[{}] device_action_button_row is not registered", get_name());
                    continue;
                }
                // Named so `helix-screen ctl ls` can address these rows. Every
                // control below is named from its DeviceAction id, which is
                // already the stable identifier the backend dispatches on.
                lv_obj_set_name(button_row,
                                fmt::format("{}_button_row_{}", section_id_, count).c_str());
                button_count_in_row = 0;
            }

            create_button(button_row, action);
            button_count_in_row++;
        } else {
            button_row = nullptr;
            button_count_in_row = 0;
            create_setting_row(actions_container_, action);
        }
        count++;
    }

    // Status widgets of the guided flows this section's actions name, once per flow, below
    // the button rows.
    std::vector<std::string> attached_flows;
    for (const auto& action : cached_actions_) {
        if (action.section != section_id_ || action.guided_flow.empty() ||
            std::find(attached_flows.begin(), attached_flows.end(), action.guided_flow) !=
                attached_flows.end()) {
            continue;
        }
        attached_flows.push_back(action.guided_flow);
        const GuidedDeviceFlow* flow = find_guided_device_flow(action.guided_flow);
        if (flow && flow->attach_status) {
            flow->attach_status(actions_container_);
        }
    }

    spdlog::debug("[{}] Created {} controls for section '{}'", get_name(), count, section_id_);
}

// ============================================================================
// DYNAMIC CONTROL CREATION
// ============================================================================

std::string device_action_label(const helix::printer::DeviceAction& action) {
    std::string label = lv_tr(action.label.c_str());
    if (action.tool_index >= 0) {
        // Punctuation only, so the format stays untranslated: a translated format whose
        // placeholders differ would throw, and the firmware builds without exceptions.
        label = fmt::format("{} - {}", label,
                            active_tool_label(tool_label(action.tool_index), action.tool_index,
                                              ToolLabelForm::Full));
    }
    return label;
}

std::string device_action_disabled_cond(const helix::printer::DeviceAction& action) {
    if (action.moves_machine && action.allowed_while_paused) {
        return fmt::format("machine_motion_blocked eq 1 and print_lifecycle ne {}",
                           static_cast<int>(PrintState::Paused));
    }
    return "0 eq 1";
}

namespace {

/// The slider positions one unit of @p action's value spans.
SliderScale slider_scale(const helix::printer::DeviceAction& action) {
    if (action.step <= 0.0f || action.step >= 1.0f) {
        return SliderScale{1};
    }
    return SliderScale{static_cast<int>(std::lround(1.0f / action.step))};
}

/// A slider value as the row shows it: as many decimals as its step has, then the unit.
std::string slider_value_text(double value, const SliderScale& scale, const std::string& unit) {
    // Enough decimals to tell one step from the next: a 0.05 step needs two.
    const int decimals =
        static_cast<int>(std::ceil(std::log10(static_cast<double>(scale.effective_divisor()))));
    std::string text = fmt::format("{:.{}f}", value, decimals);
    if (!unit.empty()) {
        text += " " + unit;
    }
    return text;
}

/// The value an INFO action shows.
std::string info_value_text(const helix::printer::DeviceAction& action) {
    const auto* text = std::any_cast<std::string>(&action.current_value);
    if (!text) {
        return "-";
    }
    return action.unit.empty() ? *text : *text + " " + action.unit;
}

} // namespace

void AmsDeviceSectionDetailOverlay::create_setting_row(lv_obj_t* parent,
                                                       const helix::printer::DeviceAction& action) {
    // NOTE: The rows come from the setting_*_row components; only what varies per
    // DeviceAction (its current value) is applied here, since a backend's action list is
    // data the XML cannot enumerate.
    spdlog::debug("[{}] Creating action row: {} (type={})", get_name(), action.label,
                  helix::printer::action_type_to_string(action.type));

    // A disabled action explains itself where its description would be.
    const std::string& description = (!action.enabled && !action.disable_reason.empty())
                                         ? action.disable_reason
                                         : action.description;
    const std::string label = device_action_label(action);
    const std::string description_text = description.empty() ? "" : lv_tr(description.c_str());

    std::vector<std::pair<std::string, std::string>> attrs = {
        {"label", label},
        // A tool-suffixed label is composed here; a tag would re-translate it to the bare label.
        {"label_tag", action.tool_index >= 0 ? std::string() : action.label},
        {"icon", action.icon},
    };
    const char* component = nullptr;
    switch (action.type) {
    case helix::printer::ActionType::BUTTON:
        return;
    case helix::printer::ActionType::TOGGLE:
        component = "setting_toggle_row";
        attrs.insert(attrs.end(), {{"description", description_text},
                                   {"description_tag", description},
                                   {"callback", "on_device_action_toggled"},
                                   {"moves_machine", action.moves_machine ? "true" : "false"}});
        break;
    case helix::printer::ActionType::SLIDER: {
        component = "setting_slider_row";
        const SliderScale scale = slider_scale(action);
        const double value =
            helix::printer::device_action_number(action.current_value).value_or(action.min_value);
        if (!action.current_value.has_value() ||
            !helix::printer::device_action_number(action.current_value)) {
            spdlog::warn("[{}] Slider value for {} is not a number", get_name(), action.id);
        }
        attrs.insert(attrs.end(), {{"description", description_text},
                                   {"description_tag", description},
                                   {"min", std::to_string(scale.to_slider(action.min_value))},
                                   {"max", std::to_string(scale.to_slider(action.max_value))},
                                   {"value", std::to_string(scale.to_slider_clamped(
                                                 value, scale.to_slider(action.min_value),
                                                 scale.to_slider(action.max_value)))},
                                   {"value_text", slider_value_text(value, scale, action.unit)},
                                   {"callback", "on_device_action_slider_moved"},
                                   {"commit_callback", "on_device_action_slider_released"},
                                   {"value_callback", "on_device_action_value_clicked"}});
        break;
    }
    case helix::printer::ActionType::DROPDOWN: {
        component = "setting_dropdown_row";
        std::string options;
        for (const auto& option : action.options) {
            if (!options.empty()) {
                options += "\n";
            }
            options += option;
        }
        attrs.insert(attrs.end(), {{"description", description_text},
                                   {"description_tag", description},
                                   {"options", options},
                                   {"callback", "on_device_action_dropdown_changed"}});
        break;
    }
    case helix::printer::ActionType::INFO:
        component = "setting_info_row";
        attrs.push_back({"value", info_value_text(action)});
        break;
    }

    // lv_xml_create applies every attribute to the row's root lv_obj too, where
    // `disabled` is the state flag and any value but "false" sets it, so an
    // enabled action must not name the attribute at all.
    if (!action.enabled && action.type != helix::printer::ActionType::INFO) {
        attrs.push_back({"disabled", kActionDisabledSubject});
    }

    std::vector<const char*> pairs;
    pairs.reserve(attrs.size() * 2 + 1);
    for (const auto& [name, value] : attrs) {
        pairs.push_back(name.c_str());
        pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(parent, component, pairs.data()));
    if (!row) {
        spdlog::warn("[{}] Failed to create {} for '{}'", get_name(), component, action.id);
        return;
    }
    // The row carries the action id: the callbacks walk up to it to know which action
    // fired, and `helix-screen ctl` addresses the control by it.
    lv_obj_set_name(row, action.id.c_str());

    if (action.type == helix::printer::ActionType::TOGGLE) {
        // required-names: setting_toggle_row
        if (lv_obj_t* toggle = helix::ui::find_required(row, "toggle", get_name())) {
            if (const bool* on = std::any_cast<bool>(&action.current_value); on && *on) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else if (!on && action.current_value.has_value()) {
                spdlog::warn("[{}] Toggle value for {} is not a bool", get_name(), action.id);
            }
        }
    } else if (action.type == helix::printer::ActionType::DROPDOWN) {
        // required-names: setting_dropdown_row
        lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name());
        const auto* current = std::any_cast<std::string>(&action.current_value);
        if (current == nullptr && action.current_value.has_value()) {
            spdlog::warn("[{}] Dropdown value for {} is not a string", get_name(), action.id);
        }
        const auto it = current ? std::find(action.options.begin(), action.options.end(), *current)
                                : action.options.end();
        if (dropdown && it != action.options.end()) {
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(it - action.options.begin()));
        } else if (dropdown) {
            // A value the backend cannot prove shows as unknown, not as the first
            // option it would otherwise read as.
            lv_dropdown_set_text(dropdown, kUnknownDropdownText);
        }
    }
}

void AmsDeviceSectionDetailOverlay::create_button(lv_obj_t* row,
                                                  const helix::printer::DeviceAction& action) {
    spdlog::debug("[{}] Creating button in row: {}", get_name(), action.label);

    const std::string label = device_action_label(action);
    // A motion action a paused print may still run greys only while the printer
    // is held some other way: printing, preparing, or spools on the bed.
    const bool paused_ok = action.moves_machine && action.allowed_while_paused;
    const std::string disabled_cond = device_action_disabled_cond(action);
    const char* attrs[] = {"text",
                           label.c_str(),
                           "icon",
                           action.icon.c_str(),
                           "disabled_subject",
                           action.enabled ? "" : kActionDisabledSubject,
                           "moves_machine",
                           action.moves_machine && !paused_ok ? "true" : "false",
                           "disabled_cond",
                           disabled_cond.c_str(),
                           nullptr};
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(row, "device_action_button", attrs));
    if (!btn) {
        spdlog::warn("[{}] Failed to create button for '{}'", get_name(), action.id);
        return;
    }
    lv_obj_set_name(btn, action.id.c_str());
    if (lv_obj_t* btn_label = ui_button_get_label(btn)) {
        lv_obj_set_name(btn_label, fmt::format("{}_label", action.id).c_str());
    }
    if (!action.enabled && !action.disable_reason.empty()) {
        // Said under the row, as a setting row says it in its description.
        const std::string note = fmt::format("{}: {}", label, lv_tr(action.disable_reason.c_str()));
        const char* note_attrs[] = {"text", note.c_str(), nullptr};
        if (lv_obj_t* note_obj = static_cast<lv_obj_t*>(
                lv_xml_create(lv_obj_get_parent(row), "device_action_note", note_attrs))) {
            lv_obj_set_name(note_obj, fmt::format("{}_reason", action.id).c_str());
        }
    }
}

// ============================================================================
// EVENT DISPATCH
// ============================================================================

const helix::printer::DeviceAction*
AmsDeviceSectionDetailOverlay::action_for_event(lv_event_t* e) const {
    for (auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e)); obj;
         obj = lv_obj_get_parent(obj)) {
        if (obj == actions_container_) {
            break;
        }
        const char* name = lv_obj_get_name(obj);
        if (!name) {
            continue;
        }
        for (const auto& action : cached_actions_) {
            if (action.section == section_id_ && action.id == name) {
                return &action;
            }
        }
    }
    spdlog::warn("[{}] Event from a widget no action owns", get_name());
    return nullptr;
}

lv_obj_t* AmsDeviceSectionDetailOverlay::row_for(const std::string& action_id) const {
    return actions_container_ ? lv_obj_find_by_name(actions_container_, action_id.c_str())
                              : nullptr;
}

AmsError AmsDeviceSectionDetailOverlay::execute(const helix::printer::DeviceAction& action,
                                                std::any value) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::warn("[{}] No backend available for '{}'", get_name(), action.id);
        return AmsErrorHelper::not_connected("No backend");
    }
    const std::string id = action.id;
    AmsError result = backend->execute_device_action(id, value);
    if (!result.success()) {
        helix::ui::notify_ams_error(result);
    } else if (value.has_value()) {
        // The row now shows the sent value. Recording it as shown means a backend
        // that never takes it up differs from the row on the next data tick, and
        // the row goes back to what the backend reports.
        for (auto& cached : cached_actions_) {
            if (cached.id == id) {
                cached.current_value = std::move(value);
                break;
            }
        }
    }
    return result;
}

void AmsDeviceSectionDetailOverlay::show_slider_value(lv_obj_t* row,
                                                      const helix::printer::DeviceAction& action,
                                                      int32_t position) const {
    const SliderScale scale = slider_scale(action);
    const std::string text = slider_value_text(scale.to_value(position), scale, action.unit);
    // required-names: setting_slider_row
    if (lv_obj_t* field = helix::ui::find_required(row, "value_field", get_name())) {
        // required-names: setting_value_field
        if (lv_obj_t* value = helix::ui::find_required(field, "value", get_name())) {
            lv_label_set_text(value, text.c_str());
        }
    }
}

void AmsDeviceSectionDetailOverlay::apply_slider_value(const helix::printer::DeviceAction& action,
                                                       double value) {
    const SliderScale scale = slider_scale(action);
    const int32_t min_pos = scale.to_slider(action.min_value);
    const int32_t max_pos = scale.to_slider(action.max_value);
    const int32_t position = scale.to_slider_clamped(value, min_pos, max_pos);
    const double sent = scale.to_value(position);

    lv_obj_t* row = row_for(action.id);
    // required-names: setting_slider_row
    lv_obj_t* slider = row ? helix::ui::find_required(row, "slider", get_name()) : nullptr;
    if (slider) {
        lv_slider_set_value(slider, position, LV_ANIM_OFF);
        show_slider_value(row, action, position);
    }

    // The value travels as float, the type the section's sliders have always sent.
    AmsError result = execute(action, std::any(static_cast<float>(sent)));
    if (result.success()) {
        NOTIFY_INFO(lv_tr("{} set to {}"), device_action_label(action),
                    slider_value_text(sent, scale, action.unit));
    } else if (slider) {
        // The backend kept its old value; so does the row.
        const double kept =
            helix::printer::device_action_number(action.current_value).value_or(action.min_value);
        const int32_t kept_pos = scale.to_slider_clamped(kept, min_pos, max_pos);
        lv_slider_set_value(slider, kept_pos, LV_ANIM_OFF);
        show_slider_value(row, action, kept_pos);
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void AmsDeviceSectionDetailOverlay::on_action_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_action_clicked");

    auto& overlay = get_ams_device_section_detail_overlay();
    if (const auto* action = overlay.action_for_event(e); action && !action->enabled) {
        spdlog::warn("[AmsDeviceSectionDetailOverlay] Ignoring unavailable action '{}'",
                     action->id);
    } else if (action) {
        spdlog::debug("[AmsDeviceSectionDetailOverlay] Action clicked: {}", action->id);
        // An action that names a guided flow starts it. The flow owns whatever
        // confirmation the action needs, so a flow that is not registered must
        // never fall through to a direct execute.
        const GuidedDeviceFlow* flow =
            action->guided_flow.empty() ? nullptr : find_guided_device_flow(action->guided_flow);
        if (!action->guided_flow.empty() && !flow) {
            spdlog::warn("[AmsDeviceSectionDetailOverlay] Action '{}' names unregistered "
                         "guided flow '{}'; not executing",
                         action->id, action->guided_flow);
        } else if (flow) {
            if (flow->start) {
                flow->start(action->label);
            }
        } else if (overlay.execute(*action, {}).success()) {
            // Whole sentence, not "{} {}" over two separately translated
            // words: German sends the participle to the end of the
            // clause and Japanese renders this as 〜を開始しました, and
            // neither can be reached by concatenating "label" + "started".
            NOTIFY_INFO(lv_tr("{} started"), device_action_label(*action));
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_toggle_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_toggle_changed");

    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto& overlay = get_ams_device_section_detail_overlay();
    if (const auto* action = overlay.action_for_event(e); action && !action->enabled) {
        // An unavailable toggle never reaches the backend; put the switch back.
        if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
    } else if (action) {
        const bool new_value = lv_obj_has_state(sw, LV_STATE_CHECKED);
        spdlog::debug("[AmsDeviceSectionDetailOverlay] Toggle changed: {} = {}", action->id,
                      new_value);
        if (overlay.execute(*action, std::any(new_value)).success()) {
            NOTIFY_INFO(new_value ? lv_tr("{} enabled") : lv_tr("{} disabled"),
                        device_action_label(*action));
        } else if (new_value) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_slider_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_slider_changed");

    // The value display follows the drag; the backend hears only the release.
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto& overlay = get_ams_device_section_detail_overlay();
    if (const auto* action = overlay.action_for_event(e)) {
        if (lv_obj_t* row = overlay.row_for(action->id)) {
            overlay.show_slider_value(row, *action, lv_slider_get_value(slider));
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_slider_released(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_slider_released");

    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto& overlay = get_ams_device_section_detail_overlay();
    if (const auto* action = overlay.action_for_event(e)) {
        const helix::printer::DeviceAction released = *action;
        overlay.apply_slider_value(released,
                                   slider_scale(released).to_value(lv_slider_get_value(slider)));
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_value_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_value_clicked");

    auto& overlay = get_ams_device_section_detail_overlay();
    if (const auto* action = overlay.action_for_event(e); action && action->enabled) {
        const SliderScale scale = slider_scale(*action);
        lv_obj_t* row = overlay.row_for(action->id);
        lv_obj_t* slider = nullptr;
        if (row) {
            // required-names: setting_slider_row
            slider = helix::ui::find_required(row, "slider", overlay.get_name());
        }
        overlay.keypad_action_id_ = action->id;
        overlay.keypad_title_ = device_action_label(*action);
        ui_keypad_config_t config = {
            .initial_value = static_cast<float>(slider ? scale.to_value(lv_slider_get_value(slider))
                                                       : action->min_value),
            .min_value = action->min_value,
            .max_value = action->max_value,
            .title_label = overlay.keypad_title_.c_str(),
            .unit_label = action->unit.c_str(),
            .allow_decimal = scale.effective_divisor() > 1,
            .allow_negative = action->min_value < 0.0f,
            .callback = on_keypad_value,
            .user_data = nullptr};
        ui_keypad_show(&config);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::on_keypad_value(float value, void* /*user_data*/) {
    auto& overlay = get_ams_device_section_detail_overlay();
    for (const auto& action : overlay.cached_actions_) {
        if (action.id == overlay.keypad_action_id_) {
            // A refresh while the keypad was open can have made it unavailable.
            if (!action.enabled) {
                spdlog::info("[AmsDeviceSectionDetailOverlay] Keypad value for '{}' dropped: {}",
                             action.id, action.disable_reason);
                return;
            }
            const helix::printer::DeviceAction typed = action;
            overlay.apply_slider_value(typed, value);
            return;
        }
    }
    spdlog::warn("[AmsDeviceSectionDetailOverlay] Keypad value for vanished action '{}'",
                 overlay.keypad_action_id_);
}

void AmsDeviceSectionDetailOverlay::on_dropdown_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsDeviceSectionDetailOverlay] on_dropdown_changed");

    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto& overlay = get_ams_device_section_detail_overlay();
    if (const auto* action = overlay.action_for_event(e)) {
        // A pick replaces the unknown placeholder with the option itself.
        lv_dropdown_set_text(dropdown, nullptr);
        const uint32_t selected = lv_dropdown_get_selected(dropdown);
        if (selected < action->options.size()) {
            const std::string choice = action->options[selected];
            spdlog::debug("[AmsDeviceSectionDetailOverlay] Dropdown changed: {} = '{}'", action->id,
                          choice);
            const auto* shown = std::any_cast<std::string>(&action->current_value);
            const auto* confirm = action->option_confirm_for(choice);
            if (confirm && !(shown && *shown == choice)) {
                overlay.confirm_dropdown_choice(*action, choice, *confirm);
            } else {
                const helix::printer::DeviceAction picked = *action;
                overlay.apply_dropdown_choice(picked, choice);
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsDeviceSectionDetailOverlay::apply_dropdown_choice(
    const helix::printer::DeviceAction& action, const std::string& choice) {
    // The row may have been rebuilt since the pick (a confirmation was open),
    // so it is set to the choice it is about to send.
    if (lv_obj_t* row = row_for(action.id)) {
        // required-names: setting_dropdown_row
        if (lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name())) {
            const auto it = std::find(action.options.begin(), action.options.end(), choice);
            if (it != action.options.end()) {
                lv_dropdown_set_text(dropdown, nullptr);
                lv_dropdown_set_selected(dropdown,
                                         static_cast<uint32_t>(it - action.options.begin()));
            }
        }
    }
    if (execute(action, std::any(choice)).success()) {
        NOTIFY_INFO(lv_tr("{} set to {}"), device_action_label(action), choice);
    } else {
        // The backend kept its value; so does the row.
        refresh();
    }
}

void AmsDeviceSectionDetailOverlay::confirm_dropdown_choice(
    const helix::printer::DeviceAction& action, const std::string& choice,
    const helix::printer::DeviceAction::OptionConfirm& confirm) {
    const std::string id = action.id;
    const std::string title = device_action_label(action);
    // Cancelled or dismissed, the row goes back to the value the backend reports.
    ConfirmOptions opts;
    opts.on_cancel = [] { get_ams_device_section_detail_overlay().refresh(); };
    opts.on_dismiss = [] { get_ams_device_section_detail_overlay().refresh(); };
    opts.owner_token = lifetime_.token();
    modal_confirm(
        title.c_str(), lv_tr(confirm.message.c_str()), ModalSeverity::Warning,
        lv_tr(confirm.confirm_label.c_str()),
        [id, choice] { get_ams_device_section_detail_overlay().on_choice_confirmed(id, choice); },
        opts);
}

void AmsDeviceSectionDetailOverlay::on_choice_confirmed(const std::string& action_id,
                                                        const std::string& choice) {
    for (const auto& action : cached_actions_) {
        if (action.id != action_id) {
            continue;
        }
        // A refresh while the confirmation was open can have made it unavailable.
        if (!action.enabled || std::find(action.options.begin(), action.options.end(), choice) ==
                                   action.options.end()) {
            spdlog::info("[AmsDeviceSectionDetailOverlay] Confirmed '{}' for '{}' dropped: {}",
                         choice, action_id, action.disable_reason);
            refresh();
            return;
        }
        const helix::printer::DeviceAction picked = action;
        apply_dropdown_choice(picked, choice);
        return;
    }
    spdlog::warn("[AmsDeviceSectionDetailOverlay] Confirmed choice for vanished action '{}'",
                 action_id);
    refresh();
}

} // namespace helix::ui
