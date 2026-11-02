// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_action_set.h"

#include "lvgl.h"

#include <algorithm>

namespace helix::ui {

namespace {

bool automatic(FilamentOpSupport support) {
    return support == FilamentOpSupport::Automatic;
}

bool gated(const FilamentHeadView& view) {
    return view.system_busy || view.print_blocks_op;
}

/// Why no head can do this direction by machine, or empty when one can.
std::string capability_reason(const FilamentHeadView& view, bool load) {
    for (const HeadState& head : view.heads) {
        if (automatic(load ? head.ops.load : head.ops.unload)) {
            return {};
        }
    }
    for (const HeadState& head : view.heads) {
        const std::string& reason = load ? head.ops.load_reason : head.ops.unload_reason;
        if (!reason.empty()) {
            return reason;
        }
    }
    return {};
}

int default_head(const FilamentHeadView& view) {
    return view.selected_head >= 0 ? view.selected_head : view.mounted_head;
}

bool has_unmounted_head(const FilamentHeadView& view) {
    for (int head = 0; head < static_cast<int>(view.heads.size()); ++head) {
        if (head != view.mounted_head) {
            return true;
        }
    }
    return false;
}

void set_mount_and_park(const FilamentHeadView& view, FilamentActionSet& out) {
    out.mount.visible = true;
    if (view.selected_head >= 0) {
        out.mount.enabled = !gated(view) && view.selected_head != view.mounted_head;
        if (view.selected_head == view.mounted_head) {
            out.mount.reason = lv_tr("Already mounted");
        }
    } else {
        out.mount.enabled = !gated(view) && has_unmounted_head(view);
    }
    out.park.visible = view.supports_park && view.mounted_head >= 0;
    out.park.enabled = out.park.visible && !gated(view);
}

} // namespace

bool head_can_load(const HeadState& head) {
    return automatic(head.ops.load) && head.source_slot.has_value() &&
           head.source_present.value_or(true) && !head.at_nozzle.value_or(false);
}

bool head_can_unload(const HeadState& head) {
    // A head nothing senses filament in stays unloadable: the backend refuses
    // an empty one itself.
    return automatic(head.ops.unload) && head.unload_slot.has_value() &&
           head.has_filament.value_or(true);
}

FilamentActionSet decide_filament_actions(const FilamentHeadView& view) {
    FilamentActionSet out;
    if (!view.backend_present || view.heads.empty()) {
        return out;
    }

    const bool any_automatic =
        std::any_of(view.heads.begin(), view.heads.end(), [](const HeadState& head) {
            return automatic(head.ops.load) || automatic(head.ops.unload);
        });
    if (!any_automatic && view.heads_dock) {
        set_mount_and_park(view, out);
        return out;
    }

    out.unload_opens_picker = view.heads.size() > 1;

    out.load.visible = true;
    out.load.enabled =
        !gated(view) && std::any_of(view.heads.begin(), view.heads.end(),
                                    [](const HeadState& h) { return head_can_load(h); });
    if (!out.load.enabled && !gated(view)) {
        out.load.reason = capability_reason(view, /*load=*/true);
        const bool any_source =
            std::any_of(view.heads.begin(), view.heads.end(), [](const HeadState& head) {
                return automatic(head.ops.load) && head.source_slot.has_value();
            });
        if (out.load.reason.empty() && !any_source) {
            out.load.reason = lv_tr("Assign a slot first");
        }
    }

    out.unload.visible = true;
    out.unload.enabled =
        !gated(view) && std::any_of(view.heads.begin(), view.heads.end(),
                                    [](const HeadState& h) { return head_can_unload(h); });
    if (!out.unload.enabled && !gated(view)) {
        out.unload.reason = capability_reason(view, /*load=*/false);
    }

    out.park.visible = view.heads_dock && view.supports_park && view.mounted_head >= 0;
    out.park.enabled = out.park.visible && !gated(view);
    return out;
}

FilamentActionSet decide_slot_actions(const FilamentHeadView& view, int head) {
    FilamentActionSet out;
    if (!view.backend_present) {
        return out;
    }
    out.load.visible = true;
    out.unload.visible = true;
    if (head < 0 || head >= static_cast<int>(view.heads.size())) {
        out.load.reason = lv_tr("This slot feeds no tool");
        out.unload.reason = out.load.reason;
        return out;
    }

    const HeadState& state = view.heads[static_cast<size_t>(head)];
    out.load.enabled = !gated(view) && automatic(state.ops.load);
    if (!automatic(state.ops.load)) {
        out.load.reason = state.ops.load_reason;
    }
    out.unload.enabled = !gated(view) && automatic(state.ops.unload);
    if (!automatic(state.ops.unload)) {
        out.unload.reason = state.ops.unload_reason;
    }

    if (view.heads_dock && !automatic(state.ops.load) && !automatic(state.ops.unload)) {
        FilamentHeadView targeted = view;
        targeted.selected_head = head;
        set_mount_and_park(targeted, out);
        out.park.visible = out.park.visible && head == view.mounted_head;
        out.park.enabled = out.park.enabled && out.park.visible;
    }
    return out;
}

std::vector<HeadPickRow> decide_head_picker_rows(const FilamentHeadView& view, HeadPickOp op) {
    std::vector<HeadPickRow> rows;
    rows.reserve(view.heads.size());
    const int preferred = default_head(view);
    for (int head = 0; head < static_cast<int>(view.heads.size()); ++head) {
        const HeadState& state = view.heads[static_cast<size_t>(head)];
        HeadPickRow row;
        row.head = head;
        switch (op) {
        case HeadPickOp::Load:
            row.slot = state.source_slot.value_or(-1);
            if (!automatic(state.ops.load)) {
                row.reason = state.ops.load_reason;
            } else if (!state.source_slot) {
                row.reason = lv_tr("Assign a slot first");
            } else {
                row.enabled = true;
            }
            row.preselected = row.enabled && head == preferred && head_can_load(state);
            break;
        case HeadPickOp::Unload:
            row.slot = state.unload_slot.value_or(-1);
            if (!automatic(state.ops.unload)) {
                row.reason = state.ops.unload_reason;
            } else if (state.unload_slot.has_value() && state.has_filament &&
                       !*state.has_filament) {
                // A head its own sensor reports empty has nothing for an
                // unload to act on; offering the tick sends a command the
                // firmware would refuse or, worse, run against nothing.
                row.reason = lv_tr("Empty");
            } else {
                row.enabled = state.unload_slot.has_value();
            }
            row.preselected = row.enabled && head == preferred && head_can_unload(state);
            break;
        case HeadPickOp::Mount:
            row.enabled = head != view.mounted_head;
            if (!row.enabled) {
                row.reason = lv_tr("Already mounted");
            }
            row.preselected = row.enabled && head == view.selected_head;
            break;
        case HeadPickOp::SeatByHand:
            // Whichever tool the user is holding: the screen cannot know, so
            // it suggests none.
            row.enabled = true;
            break;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace helix::ui
