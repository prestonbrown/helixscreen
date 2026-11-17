// SPDX-License-Identifier: GPL-3.0-or-later
#include "target_tool.h"

#include "ui_temperature_utils.h"

#include "display_numbering.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>

namespace helix::ui {

namespace {

void set_int_if_changed(lv_subject_t* subject, int value) {
    if (lv_subject_get_int(subject) != value) {
        lv_subject_set_int(subject, value);
    }
}

void copy_if_changed(lv_subject_t* subject, const std::string& text) {
    if (text != lv_subject_get_string(subject)) {
        lv_subject_copy_string(subject, text.c_str());
    }
}

} // namespace

TargetTool& TargetTool::instance() {
    static TargetTool s_instance;
    return s_instance;
}

void TargetTool::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(tool_subject_, 0, "target_tool", subjects_);
    UI_MANAGED_SUBJECT_INT(head_subject_, 0, "target_tool_head", subjects_);
    UI_MANAGED_SUBJECT_INT(multi_head_subject_, 0, "target_tool_multi_head", subjects_);
    UI_MANAGED_SUBJECT_STRING(name_subject_, name_buf_, "", "target_tool_name", subjects_);
    UI_MANAGED_SUBJECT_STRING(number_subject_, number_buf_, "", "target_tool_number", subjects_);
    UI_MANAGED_SUBJECT_STRING(label_subject_, label_buf_, "", "target_nozzle_label", subjects_);
    UI_MANAGED_SUBJECT_INT(temp_subject_, 0, "target_nozzle_temp", subjects_);
    UI_MANAGED_SUBJECT_INT(target_subject_, 0, "target_nozzle_target", subjects_);
    UI_MANAGED_SUBJECT_INT(status_state_subject_, 0, "target_nozzle_status_state", subjects_);
    UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, "", "target_nozzle_status", subjects_);
    subjects_initialized_ = true;
    // A target row's cell: aim the target at its head.
    lv_xml_register_event_cb(nullptr, "tool_target_segment_cb", [](lv_event_t* e) {
        if (const char* head = static_cast<const char*>(lv_event_get_user_data(e))) {
            TargetTool::instance().pick_head(std::atoi(head));
        }
    });
    StaticSubjectRegistry::instance().register_deinit(
        "TargetTool", []() { TargetTool::instance().deinit_subjects(); });
}

void TargetTool::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    nozzle_observers_.reset();
    power_observer_.reset();
    active_tool_observer_.reset();
    tools_version_observer_.reset();
    extruder_version_observer_.reset();
    language_observer_.reset();
    printer_state_ = nullptr;
    selection_ = TargetToolSelection{};
    last_active_ = kNoActiveSeen;
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void TargetTool::attach(PrinterState& printer_state) {
    init_subjects();
    // Subscribes afresh every time: ToolState and the temperature state can
    // rebuild their subjects, which drops the observers on the old ones.
    printer_state_ = &printer_state;
    printer_lifetime_ = printer_state.temperature_state().get_subjects_lifetime();
    nozzle_observers_.reset();
    power_observer_.reset();

    auto& tools = ToolState::instance();
    // The tool mounted now is no change: the subscription below replays it.
    // Until a pick, the target is the mounted tool anyway (tool()).
    if (last_active_ == kNoActiveSeen) {
        last_active_ = tools.active_tool_index();
    }
    // A tool change anywhere moves the target to the mounted tool.
    active_tool_observer_ = observe<int>(
        tools.get_active_tool_subject(), this,
        [](TargetTool* self, int active) {
            // Subscribing replays the current tool, which is not a change.
            if (active != self->last_active_) {
                self->last_active_ = active;
                self->selection_.on_active_tool_changed(active);
            }
            self->refresh();
        },
        tools.get_subjects_lifetime());
    tools_version_observer_ = observe<int>(
        tools.get_tools_version_subject(), this, [](TargetTool* self, int) { self->refresh(); },
        tools.get_subjects_lifetime());
    // Rediscovery rebuilds the per-extruder subjects the mirrors watch.
    extruder_version_observer_ = observe<int>(
        printer_state.temperature_state().get_extruder_version_subject(), this,
        [](TargetTool* self, int) {
            self->nozzle_observers_.invalidate();
            self->power_observer_.reset();
            self->refresh();
        },
        printer_state.temperature_state().get_subjects_lifetime());
    // Names and numbers follow the language and the display numbering.
    language_observer_ = observe_language_change(this, [](TargetTool* self) { self->publish(); });
    refresh();
}

int TargetTool::tool() const {
    const auto& ts = ToolState::instance();
    // Nothing chosen or followed yet: the mounted tool.
    if (!selection_.has_pick()) {
        const int active = ts.active_tool_index();
        return active >= 0 && active < ts.tool_count() ? active : 0;
    }
    return selection_.current(ts.tool_count());
}

int TargetTool::head() const {
    const int found = head_index_for_tool(ToolState::instance().physical_heads(), tool());
    return found >= 0 ? found : 0;
}

std::string TargetTool::extruder_name() const {
    const auto& tools = ToolState::instance().tools();
    const int t = tool();
    if (t < static_cast<int>(tools.size())) {
        const auto& name = tools[static_cast<size_t>(t)].extruder_name;
        if (name && !name->empty()) {
            return *name;
        }
    }
    return "extruder";
}

bool TargetTool::printer_alive() const {
    return printer_state_ && printer_lifetime_ && *printer_lifetime_;
}

bool TargetTool::mounted() const {
    const auto& ts = ToolState::instance();
    return !ts.has_multiple_toolheads() || tool() == ts.active_tool_index();
}

void TargetTool::pick(int tool_index) {
    const int before = tool();
    selection_.pick(tool_index);
    refresh();
    // A pick of the tool already aimed at still reaches the surfaces, which
    // re-read what they gate on it.
    if (subjects_initialized_ && tool() == before) {
        lv_subject_notify(&tool_subject_);
    }
}

void TargetTool::pick_head(int head_index) {
    const auto& ts = ToolState::instance();
    const auto& heads = ts.physical_heads();
    if (head_index < 0 || head_index >= static_cast<int>(heads.size())) {
        return;
    }
    pick(heads[static_cast<size_t>(head_index)].representative_tool(ts.active_tool_index()));
}

void TargetTool::pick_extruder(const std::string& extruder) {
    for (const auto& t : ToolState::instance().tools()) {
        if (t.extruder_name.value_or("") == extruder) {
            pick(t.index);
            return;
        }
    }
}

void TargetTool::refresh() {
    if (!subjects_initialized_) {
        return;
    }
    publish();
    if (!printer_alive()) {
        return;
    }
    auto& temps = printer_state_->temperature_state();
    nozzle_observers_.bind<TargetTool>(
        temps, extruder_name(), mounted(), this,
        [](TargetTool* self, int value) {
            set_int_if_changed(&self->temp_subject_, value);
            self->publish_status();
        },
        [](TargetTool* self, int value) {
            set_int_if_changed(&self->target_subject_, value);
            self->publish_status();
        });
    rebind_power();
    publish_status();
}

void TargetTool::rebind_power() {
    auto& temps = printer_state_->temperature_state();
    SubjectLifetime lifetime = temps.get_subjects_lifetime();
    lv_subject_t* power = nullptr;
    if (!mounted()) {
        power = temps.get_extruder_power_subject(extruder_name(), lifetime);
    }
    if (!power) {
        power = temps.get_extruder_power_subject();
        lifetime = temps.get_subjects_lifetime();
    }
    power_ = lv_subject_get_int(power);
    power_observer_ = observe<int>(
        power, this,
        [](TargetTool* self, int value) {
            self->power_ = value;
            self->publish_status();
        },
        lifetime, Dispatch::Immediate);
}

void TargetTool::publish() {
    const auto& ts = ToolState::instance();
    const int t = tool();
    const bool multi = ts.has_multiple_toolheads();
    std::string name;
    std::string number;
    if (multi && t < ts.tool_count()) {
        const auto& info = ts.tools()[static_cast<size_t>(t)];
        name = active_tool_name_label(info.name, info.index);
        number = tool_number_or_name(info.name, info.index);
    }
    set_int_if_changed(&head_subject_, head());
    set_int_if_changed(&multi_head_subject_, multi ? 1 : 0);
    copy_if_changed(&name_subject_, name);
    copy_if_changed(&number_subject_, number);
    copy_if_changed(&label_subject_, ts.nozzle_label(t));
    // Last: observers of the target read every other subject already updated.
    set_int_if_changed(&tool_subject_, t);
}

void TargetTool::publish_status() {
    // The classifier every heater row uses, so this row can never disagree
    // with the bed's and chamber's spelling.
    const auto status = temperature::classify_heater_status(lv_subject_get_int(&temp_subject_),
                                                            lv_subject_get_int(&target_subject_),
                                                            power_, ChamberMode::Heating);
    set_int_if_changed(&status_state_subject_, static_cast<int>(status.state));
    copy_if_changed(&status_subject_, status.duty);
}

} // namespace helix::ui
