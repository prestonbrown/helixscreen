// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_tower_start.h"

#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "bed_drying_controller.h"
#include "bed_plate_context.h"
#include "src/ui/panel_widgets/filament_ops_widget.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

bool tower_head_empty(int tool) {
    if (tool >= 0) {
        // A tool changer's heads each carry their own answer; the one global sensor
        // cannot say which head it watches.
        const AmsBackend* backend = AmsState::instance().get_backend();
        const auto has = backend ? backend->head_has_filament(tool) : std::nullopt;
        return has.has_value() && !*has;
    }
    const auto loaded = current_toolhead_loaded();
    return loaded.has_value() && !*loaded;
}

bool refuse_empty_toolhead(int tool, LifetimeToken owner) {
    if (!tower_head_empty(tool)) {
        return false;
    }
    spdlog::info("[PaTower] Refusing a test tower: no filament in the toolhead (tool {})", tool);
    ConfirmOptions options;
    options.owner_token = std::move(owner);
    modal_confirm(
        lv_tr("No filament loaded"),
        lv_tr("The test tower needs filament in the toolhead. Load it from the Filament panel, "
              "then start again."),
        ModalSeverity::Warning, lv_tr("Open Filament"), []() { helix::open_filament_controls(); },
        options);
    return true;
}

void confirm_tower_print(const std::string& message, LifetimeToken owner,
                         std::function<void()> start) {
    ConfirmOptions options;
    options.owner_token = std::move(owner);
    modal_confirm(
        lv_tr("Print the test tower?"), message.c_str(), ModalSeverity::Warning, lv_tr("Start"),
        [start = std::move(start)]() {
            // The dialog asked for a clear plate, so nothing asks again.
            mark_bed_cleared();
            start();
        },
        options);
}

} // namespace helix::ui
