// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_toolhead.cpp
 * @brief Implementation of ToolheadSettingsOverlay
 */

#include "ui_settings_toolhead.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "klipper_extruder_naming.h"
#include "printer_state.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix::settings {

using helix::ui::find_required;

namespace {

constexpr const char* kHeadPrefix = "head_";

/// A head's card title. A printer with one head calls it the nozzle; with several,
/// each is labeled as the rest of the UI labels that head, never by a T number built here.
std::string head_title(int extruder_index, size_t head_count) {
    if (head_count <= 1) {
        return lv_tr("Nozzle");
    }
    const std::string extruder =
        extruder_index == 0 ? "extruder" : "extruder" + std::to_string(extruder_index);
    const HeadIdentity id = ToolState::instance().head_identity_for_extruder(extruder);
    if (id.index < 0) {
        return helix::ui::extruder_label(extruder_index);
    }
    return helix::ui::active_head_name_label(id);
}

std::string join_lines(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        out += (i == 0 ? "" : "\n") + items[i];
    }
    return out;
}

/// The extruder index of the card @p obj sits in, or -1.
int head_of(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        const char* name = lv_obj_get_name(o);
        if (name != nullptr && std::string_view(name).rfind(kHeadPrefix, 0) == 0) {
            return std::atoi(name + std::string_view(kHeadPrefix).size());
        }
    }
    return -1;
}

const nozzle_size::HeadCapability* find_head(const nozzle_size::Capability& cap, int index) {
    for (const auto& h : cap.heads) {
        if (h.extruder_index == index) {
            return &h;
        }
    }
    return nullptr;
}

int selected_in(const std::vector<double>& choices, const std::optional<double>& current) {
    if (!current) {
        return -1;
    }
    for (size_t i = 0; i < choices.size(); ++i) {
        if (std::fabs(choices[i] - *current) < 1e-6) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

void ToolheadSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_toolhead_size_changed",
         [](lv_event_t* e) {
             lv_obj_t* dd = lv_event_get_target_obj(e);
             get_toolhead_settings_overlay().on_size_picked(head_of(dd),
                                                            helix::ui::event_selected(e));
         }},
        {"on_toolhead_flow_changed",
         [](lv_event_t* e) {
             lv_obj_t* dd = lv_event_get_target_obj(e);
             get_toolhead_settings_overlay().on_flow_picked(head_of(dd),
                                                            helix::ui::event_selected(e));
         }},
    });
}

void ToolheadSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(summary_, summary_buf_, "", "toolhead_settings_summary",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(head_count_, 0, "toolhead_settings_head_count", subjects_);
    });
}

lv_obj_t* ToolheadSettingsOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    list_ = find_required(overlay_root_, "toolhead_cards", get_name());
    return overlay_root_;
}

void ToolheadSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    populate();
}

void ToolheadSettingsOverlay::refresh_summary() {
    const auto cap =
        nozzle_size::nozzle_capability(nozzle_size::inputs_from_state(get_printer_state()));
    std::string text;
    for (const auto& h : cap.heads) {
        if (!h.current) {
            continue;
        }
        const std::string size = nozzle_size::size_text_mm(*h.current);
        if (cap.heads.size() == 1) {
            text = fmt::format(fmt::runtime(lv_tr("{} nozzle")), size);
            break;
        }
        text += (text.empty() ? "" : " · ") + head_title(h.extruder_index, cap.heads.size()) + " " +
                size;
    }
    if (text.empty()) {
        text = lv_tr("Nozzle size");
    }
    if (are_subjects_initialized()) {
        lv_subject_set_string(summary_.get(), text.c_str());
    }
}

void ToolheadSettingsOverlay::populate() {
    lv_obj_t* list = list_.get();
    if (!list) {
        return;
    }
    helix::ui::safe_clean_children(list);

    const auto inputs = nozzle_size::inputs_from_state(get_printer_state());
    const auto cap = nozzle_size::nozzle_capability(inputs);
    lv_subject_set_int(head_count_.get(), static_cast<int>(cap.heads.size()));

    for (const auto& h : cap.heads) {
        const std::string group_name = kHeadPrefix + std::to_string(h.extruder_index);
        const char* group_attrs[] = {"name", group_name.c_str(), nullptr};
        auto* group = static_cast<lv_obj_t*>(lv_xml_create(list, "setting_group", group_attrs));
        if (!group) {
            spdlog::warn("[{}] Could not create the card for {}", get_name(), h.extruder);
            continue;
        }
        const std::string title = head_title(h.extruder_index, cap.heads.size());
        const char* header_attrs[] = {"title", title.c_str(), nullptr};
        lv_xml_create(group, "setting_group_header", header_attrs);

        const std::string size_label = lv_tr("Nozzle size");
        if (!h.settable) {
            // Read-only: the size, and why it cannot change here.
            const std::string value =
                h.current ? nozzle_size::size_text_mm(*h.current) : std::string("?");
            const std::string why = lv_tr(
                nozzle_size::plan_change(inputs, h.extruder_index, nozzle_size::kDefaultNozzleMm)
                    .reason.c_str());
            const char* attrs[] = {"label",
                                   size_label.c_str(),
                                   "icon",
                                   "nozzle",
                                   "value",
                                   value.c_str(),
                                   "description",
                                   why.c_str(),
                                   "chevron",
                                   "false",
                                   "clickable",
                                   "false",
                                   "description_min_bp",
                                   "0",
                                   nullptr};
            lv_xml_create(group, "setting_action_row", attrs);
        } else {
            std::vector<std::string> labels;
            for (double mm : h.choices) {
                labels.push_back(nozzle_size::size_text_mm(mm));
            }
            const std::string options = join_lines(labels);
            const char* attrs[] = {"options",     options.c_str(),
                                   "options_tag", options.c_str(),
                                   "disabled",    h.idle_only ? "print_in_progress" : "",
                                   nullptr};
            auto* row = static_cast<lv_obj_t*>(lv_xml_create(group, "toolhead_size_row", attrs));
            lv_obj_t* dd = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
            if (dd) {
                const int sel = selected_in(h.choices, h.current);
                if (sel >= 0) {
                    lv_dropdown_set_selected(dd, static_cast<uint32_t>(sel));
                } else if (h.current) {
                    // A size the list does not name (set by hand in the config) shows as is.
                    lv_dropdown_set_text(dd, nozzle_size::size_text_mm(*h.current).c_str());
                }
            }
        }

        if (h.flow_variants.empty()) {
            continue;
        }
        const std::string flow_label = lv_tr("Flow type");
        if (!h.flow_changeable || !h.settable) {
            const std::string note = h.flow_note.empty() ? "" : lv_tr(h.flow_note.c_str());
            const char* attrs[] = {"label",
                                   flow_label.c_str(),
                                   "icon",
                                   "heater",
                                   "value",
                                   h.flow_current.c_str(),
                                   "description",
                                   note.c_str(),
                                   "chevron",
                                   "false",
                                   "clickable",
                                   "false",
                                   "description_min_bp",
                                   "0",
                                   nullptr};
            lv_xml_create(group, "setting_action_row", attrs);
            continue;
        }
        const std::string options = join_lines(h.flow_variants);
        const char* attrs[] = {"options",     options.c_str(),
                               "options_tag", options.c_str(),
                               "disabled",    h.idle_only ? "print_in_progress" : "",
                               nullptr};
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(group, "toolhead_flow_row", attrs));
        lv_obj_t* dd = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
        if (dd) {
            const auto it =
                std::find(h.flow_variants.begin(), h.flow_variants.end(), h.flow_current);
            if (it != h.flow_variants.end()) {
                lv_dropdown_set_selected(
                    dd, static_cast<uint32_t>(std::distance(h.flow_variants.begin(), it)));
            }
        }
    }
    refresh_summary();
}

void ToolheadSettingsOverlay::on_size_picked(int extruder_index, int option) {
    const auto inputs = nozzle_size::inputs_from_state(get_printer_state());
    const auto cap = nozzle_size::nozzle_capability(inputs);
    const auto* head = find_head(cap, extruder_index);
    if (!head || option < 0 || static_cast<size_t>(option) >= head->choices.size()) {
        return;
    }
    const double mm = head->choices[static_cast<size_t>(option)];
    apply(nozzle_size::plan_change(inputs, extruder_index, mm), mm,
          head_title(extruder_index, cap.heads.size()));
}

void ToolheadSettingsOverlay::on_flow_picked(int extruder_index, int option) {
    const auto inputs = nozzle_size::inputs_from_state(get_printer_state());
    const auto cap = nozzle_size::nozzle_capability(inputs);
    const auto* head = find_head(cap, extruder_index);
    if (!head || option < 0 || static_cast<size_t>(option) >= head->flow_variants.size()) {
        return;
    }
    apply(nozzle_size::plan_flow_change(inputs, extruder_index,
                                        head->flow_variants[static_cast<size_t>(option)]),
          0.0, head_title(extruder_index, cap.heads.size()));
}

void ToolheadSettingsOverlay::apply(const nozzle_size::ChangePlan& plan, double mm,
                                    const std::string& head_label) {
    using Kind = nozzle_size::ChangePlan::Kind;
    if (plan.kind == Kind::Refused) {
        ToastManager::instance().show(ToastSeverity::WARNING, lv_tr(plan.reason.c_str()), 4000);
        populate(); // put the dropdown back on what the printer has
        return;
    }
    const auto question = nozzle_size::confirm_message(plan, mm);
    if (!question) {
        run(plan);
        return;
    }
    const std::string title = fmt::format(fmt::runtime(lv_tr("Change {} nozzle?")), head_label);
    const std::string what = lv_tr(question->c_str());
    helix::ui::ConfirmOptions options;
    auto token = object_lifetime_.token();
    options.owner_token = token;
    options.on_cancel = [this]() { populate(); };
    options.on_dismiss = [this]() { populate(); };
    if (plan.kind == Kind::ConfigEdit) {
        helix::ui::confirm_config_rewrite(
            title.c_str(), what, lv_tr("Change"), [this, plan]() { run(plan); }, options);
    } else {
        helix::ui::modal_confirm(
            title.c_str(), what.c_str(), ModalSeverity::Warning, lv_tr("Change"),
            [this, plan]() { run(plan); }, options);
    }
}

void ToolheadSettingsOverlay::run(const nozzle_size::ChangePlan& plan) {
    using Kind = nozzle_size::ChangePlan::Kind;
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        finish(lv_tr("Not connected to the printer"));
        return;
    }
    auto token = object_lifetime_.token();
    switch (plan.kind) {
    case Kind::KlipperCommand:
        spdlog::info("[{}] {}", get_name(), plan.gcode);
        api->execute_gcode(
            plan.gcode,
            [token, this]() { token.defer("Toolhead::sent", [this]() { finish({}); }); },
            [token, this](const MoonrakerError& err) {
                token.defer("Toolhead::send_failed",
                            [this, message = err.message]() { finish(message); });
            },
            0, false, nullptr, /*caller_surfaces_errors=*/false);
        return;
    case Kind::ConfigEdit:
        spdlog::info("[{}] [{}] {} = {}", get_name(), plan.section, plan.key, plan.value);
        config_editor_.safe_edit_value(
            *api, plan.section, plan.key, plan.value,
            [token, this]() { token.defer("Toolhead::saved", [this]() { finish({}); }); },
            [token, this](const std::string& err) {
                token.defer("Toolhead::save_failed", [this, err]() { finish(err); });
            });
        return;
    case Kind::VendorRpc: {
        AmsBackend* backend = AmsState::instance().get_backend();
        if (!backend) {
            finish(lv_tr("No filament system to send this to"));
            return;
        }
        const AmsError sent = backend->change_nozzle(plan.params, [this](const AmsError& result) {
            finish(result.success() ? std::string() : result.user_msg);
        });
        if (!sent.success()) {
            finish(sent.user_msg);
        }
        return;
    }
    case Kind::Refused:
        break;
    }
}

void ToolheadSettingsOverlay::finish(const std::string& error) {
    if (error.empty()) {
        ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Nozzle changed"), 2500);
    } else {
        ToastManager::instance().show(ToastSeverity::ERROR, error.c_str(), 5000);
    }
    populate();
}

} // namespace helix::settings
