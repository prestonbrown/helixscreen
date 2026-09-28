// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_ams_recover_state_modal.h"

#include "ui_error_reporting.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <memory>
#include <string>

namespace helix::ui {

namespace {

constexpr uint32_t kLoadedDetect = 0;
constexpr uint32_t kLoadedYes = 1;
constexpr uint32_t kLoadedNo = 2;

void set_dropdown(lv_obj_t* dropdown, const std::string& options, uint32_t selected) {
    if (!dropdown) {
        return;
    }
    lv_dropdown_set_options(dropdown, options.c_str());
    lv_dropdown_set_selected(dropdown, selected);
}

uint32_t dropdown_selected(lv_obj_t* dropdown) {
    return dropdown ? lv_dropdown_get_selected(dropdown) : 0;
}

} // namespace

bool AmsRecoverStateModal::show_owned() {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || !backend->supports_recover_with_state()) {
        return false;
    }
    auto modal = std::make_unique<AmsRecoverStateModal>();
    if (!modal->show(lv_screen_active())) {
        return false; // the unique_ptr frees the never-shown instance
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

RecoverStateRequest AmsRecoverStateModal::prefill(const AmsSystemInfo& info, bool bypass_active) {
    RecoverStateRequest request;
    request.bypass = bypass_active;
    request.slot = info.current_slot >= 0 ? info.current_slot : -1;
    // Filament starts at "detect": the loaded flag reads false for every
    // position the firmware itself calls unknown, so echoing it would assert
    // the very state the firmware is confused about.
    return request;
}

AmsRecoverStateModal::Selection
AmsRecoverStateModal::selection_for(const RecoverStateRequest& request, const Choices& choices) {
    Selection s;
    if (request.bypass && choices.has_bypass) {
        s.slot = static_cast<uint32_t>(choices.slot_count) + 1;
    } else if (request.slot >= 0 && request.slot < choices.slot_count) {
        s.slot = static_cast<uint32_t>(request.slot) + 1;
    }
    if (request.loaded.has_value()) {
        s.loaded = *request.loaded ? kLoadedYes : kLoadedNo;
    }
    return s;
}

RecoverStateRequest AmsRecoverStateModal::request_for(const Selection& selection,
                                                      const Choices& choices) {
    RecoverStateRequest request;
    const auto slot_count = static_cast<uint32_t>(choices.slot_count);
    if (choices.has_bypass && selection.slot == slot_count + 1) {
        request.bypass = true;
    } else {
        request.slot = selection.slot <= slot_count ? static_cast<int>(selection.slot) - 1 : -1;
    }
    if (selection.loaded == kLoadedYes) {
        request.loaded = true;
    } else if (selection.loaded == kLoadedNo) {
        request.loaded = false;
    }
    return request;
}

void AmsRecoverStateModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    const AmsSystemInfo info = backend->get_system_info();
    shown_backend_ = backend;
    choices_.slot_count = info.total_slots;
    choices_.has_bypass = info.supports_bypass;

    const Selection selected = selection_for(prefill(info, backend->is_bypass_active()), choices_);

    std::string slots = lv_tr("Keep current");
    for (int i = 0; i < choices_.slot_count; ++i) {
        slots += "\n" + fmt::format(lv_tr("Slot {}"), i + 1);
    }
    if (choices_.has_bypass) {
        slots += std::string("\n") + lv_tr("Bypass");
    }
    set_dropdown(find_widget("slot_dropdown"), slots, selected.slot);

    const std::string loaded = std::string(lv_tr("Detect automatically")) + "\n" + lv_tr("Loaded") +
                               "\n" + lv_tr("Unloaded");
    set_dropdown(find_widget("loaded_dropdown"), loaded, selected.loaded);
}

void AmsRecoverStateModal::on_ok() {
    Selection selection;
    selection.slot = dropdown_selected(find_widget("slot_dropdown"));
    selection.loaded = dropdown_selected(find_widget("loaded_dropdown"));
    const RecoverStateRequest request = request_for(selection, choices_);
    const Choices shown = choices_;
    AmsBackend* const shown_backend = shown_backend_;
    hide();

    // The rows index the system the dialog was built for. A backend swapped
    // or resized while it was open would read them as different slots.
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }
    if (backend != shown_backend || backend->get_system_info().total_slots != shown.slot_count) {
        spdlog::info("[AmsRecoverStateModal] Backend changed while open; recover not sent");
        return;
    }
    AmsError err = backend->recover_with_state(request);
    if (!err.success()) {
        notify_ams_error(err, lv_tr("Recovery failed"));
    }
}

} // namespace helix::ui
