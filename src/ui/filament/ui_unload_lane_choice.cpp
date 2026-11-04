// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unload_lane_choice.h"

#include "ui_filament_slot_picker.h"
#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

std::vector<helix::AvailableSlot>
unload_lane_choices(const std::vector<int>& candidates,
                    const std::vector<helix::AvailableSlot>& slots) {
    std::vector<helix::AvailableSlot> choices;
    for (int slot : candidates) {
        auto it = std::find_if(slots.begin(), slots.end(), [slot](const helix::AvailableSlot& s) {
            return s.backend_index == 0 && s.slot_index == slot;
        });
        if (it != slots.end()) {
            choices.push_back(*it);
        }
    }
    return choices;
}

bool choose_unload_lane(helix::AmsBackend* backend, lv_obj_t* anchor,
                        std::function<void(int)> on_chosen) {
    if (!backend || !anchor) {
        return false;
    }
    const helix::UnloadLaneQuestion question = backend->unload_lane_question();
    if (!question.asks()) {
        return false;
    }

    if (question.lanes.empty()) {
        // A clear is the only unload there is - but it heats and cuts, so it
        // still waits for a yes.
        spdlog::info("[UnloadLane] Asking to confirm an extruder-only clear");
        const std::string message =
            question.reason.empty()
                ? std::string(lv_tr("No lane holds filament, so none can be pulled back. "
                                    "Clearing cuts the filament in the extruder and withdraws it."))
                : question.reason;
        ConfirmOptions options;
        options.on_dismiss = []() { spdlog::info("[UnloadLane] Extruder clear dismissed"); };
        modal_confirm(
            lv_tr("Clear the extruder?"), message.c_str(), ModalSeverity::Info, lv_tr("Clear"),
            [on_chosen]() {
                if (on_chosen) {
                    on_chosen(helix::kClearToolheadSlot);
                }
            },
            options);
        return true;
    }

    const std::vector<helix::AvailableSlot> choices =
        unload_lane_choices(question.lanes, AmsState::instance().collect_available_slots());
    if (choices.empty()) {
        spdlog::warn("[UnloadLane] {} lane candidate(s) but none published as a slot",
                     question.lanes.size());
        return false;
    }

    // Even one lane holding filament is only likely, not certain, so every
    // case asks: the lanes, an extruder-only clear, and a Cancel that sends
    // nothing. One question at a time, so one picker serves every Unload
    // button. Never destroyed: its destructor deletes LVGL objects, and a
    // static's destructor runs after lv_deinit(), which already freed the
    // screen holding them.
    static FilamentSlotPicker* picker = new FilamentSlotPicker();
    spdlog::info("[UnloadLane] Asking which of {} lane(s) is in the toolhead", choices.size());
    FilamentSlotPicker::Options options;
    options.title = lv_tr("Which lane is in the toolhead?");
    options.highlight_trigger = false;
    options.extra_choice = lv_tr("Clear extruder only");
    options.cancel_choice = lv_tr("Cancel");
    picker->show(
        lv_screen_active(), anchor, choices, FilamentSlotPicker::Selection{},
        [on_chosen = std::move(on_chosen)](const FilamentSlotPicker::Selection& sel) {
            if (!on_chosen) {
                return;
            }
            if (sel.is_extra) {
                on_chosen(helix::kClearToolheadSlot);
            } else if (sel.slot_index >= 0) {
                on_chosen(sel.slot_index);
            }
        },
        options);
    return true;
}

bool ask_lane_then_unload(helix::AmsBackend* backend, lv_obj_t* anchor, FilamentOpSurface surface,
                          std::optional<helix::LifetimeToken> token) {
    return choose_unload_lane(backend, anchor ? anchor : lv_screen_active(),
                              [surface = std::move(surface), token = std::move(token)](int chosen) {
                                  if (token && token->expired()) {
                                      return;
                                  }
                                  execute_filament_unload(AmsState::instance().get_backend(),
                                                          chosen,
                                                          /*target_is_loaded=*/true, surface);
                              });
}

} // namespace helix::ui
