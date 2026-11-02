// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bed_plate_context.h"

#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "async_lifetime_guard.h"
#include "bed_occupancy_state.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standard_macros.h"
#include "toolhead_homing.h"
#include "toolhead_move_sender.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace helix {

std::optional<BedPlateArea> current_bed_plate(const PrinterState& ps, IMoonrakerAPI* api) {
    if (!api) {
        return std::nullopt;
    }
    const auto area =
        preset_area(ps.motion_state().get_axis_bounds(), ps.motion_state().get_gcode_axis_bounds(),
                    api->hardware().build_volume());
    if (!area.has_x || !area.has_y || area.x_max <= area.x_min || area.y_max <= area.y_min) {
        return std::nullopt;
    }
    return BedPlateArea{area, circular_bed_kinematics(api->hardware().kinematics())};
}

AxisBounds move_clamp_bounds(const PrinterState& ps) {
    return inset_bounds(ps.motion_state().get_gcode_axis_bounds(), kGcodeEdgeMarginMm);
}

bool plate_moves_allowed(PrinterState& ps) {
    return toolhead_is_homed(ps) &&
           lv_subject_get_int(ps.network_state().get_nav_buttons_enabled_subject()) != 0 &&
           lv_subject_get_int(ps.print_state().get_machine_motion_blocked_subject()) == 0 &&
           !ps.is_external_blocking_operation_active();
}

bool bed_may_be_occupied() {
    const BedOccupancyState* state = get_bed_occupancy_state();
    return state && state->current().occupied;
}

std::optional<double> bed_part_height() {
    const BedOccupancyState* state = get_bed_occupancy_state();
    if (!state || !state->current().occupied) {
        return std::nullopt;
    }
    return state->current().part_height_mm;
}

PlateLift current_plate_lift(const PrinterState& ps, double start_z) {
    const AxisBounds bounds = move_clamp_bounds(ps);
    if (!bounds.has_z) {
        if (bed_may_be_occupied()) {
            return {PlateLift::Kind::Refuse, 0.0};
        }
        return {};
    }
    return plate_lift(start_z,
                      static_cast<double>(SettingsManager::instance().get_bed_map_clearance_mm()),
                      static_cast<double>(bounds.z_max), bed_part_height());
}

PlateLift park_plate_lift(const PrinterState& ps, double current_z) {
    const PlateLift floor = current_plate_lift(ps, current_z);
    if (floor.kind == PlateLift::Kind::Refuse) {
        return floor;
    }
    const AxisBounds bounds = move_clamp_bounds(ps);
    if (!bounds.has_z) {
        return {};
    }
    double z = current_z + kParkZLiftMm;
    if (floor.kind == PlateLift::Kind::Lift) {
        z = std::max(z, floor.z);
    }
    z = std::min(z, static_cast<double>(bounds.z_max));
    if (z <= current_z) {
        return {};
    }
    return {PlateLift::Kind::Lift, z};
}

ParkOverPlate park_over_plate_target(const PrinterState& ps, IMoonrakerAPI* api, double start_z) {
    ParkOverPlate park;
    const auto plate = current_bed_plate(ps, api);
    const auto target = plate ? plate_rear_park(plate->area) : std::nullopt;
    if (!target) {
        return park;
    }
    const PlateLift lift = park_plate_lift(ps, start_z);
    if (lift.kind == PlateLift::Kind::Refuse) {
        park.kind = ParkOverPlate::Kind::Refuse;
        return park;
    }
    park.kind = ParkOverPlate::Kind::Move;
    park.target = *target;
    if (lift.kind == PlateLift::Kind::Lift) {
        park.target.z = lift.z;
    }
    return park;
}

bool run_park_toolhead_macro(IMoonrakerAPI* api, AsyncLifetimeGuard& lifetime, const char* tag) {
    const auto info = StandardMacros::instance().get(StandardMacroSlot::ParkToolhead);
    if (info.is_empty()) {
        return false;
    }
    const std::string name = info.translated_name();
    NOTIFY_INFO(lv_tr("Running {}..."), name.c_str());
    if (!StandardMacros::instance().execute(
            StandardMacroSlot::ParkToolhead, api, {},
            [name]() { NOTIFY_SUCCESS(lv_tr("{} complete"), name.c_str()); },
            lifetime.bg_cb(tag, [](const MoonrakerError& err) {
                NOTIFY_ERROR(lv_tr("Macro failed: {}"), clean_gcode_error(err.localized_message()));
            }))) {
        NOTIFY_WARNING(lv_tr("{} macro not configured"), name.c_str());
    }
    return true;
}

double clamp_z_down_to_current_floor(double target_z) {
    return clamp_z_down_to_floor(
        target_z, static_cast<double>(SettingsManager::instance().get_bed_map_clearance_mm()),
        bed_may_be_occupied(), bed_part_height());
}

namespace {

/// Trailing zeros trimmed, so 43 reads "43" and 5.5 reads "5.5".
std::string format_lift_mm(double mm) {
    char buf[16];
    if (mm == std::floor(mm)) {
        std::snprintf(buf, sizeof(buf), "%.0f", mm);
    } else {
        std::snprintf(buf, sizeof(buf), "%g", mm);
    }
    return buf;
}

} // namespace

std::string plate_lift_readout(double shown_z, const PlateLift& lift) {
    if (lift.kind == PlateLift::Kind::Refuse) {
        return part_too_tall_text();
    }
    if (lift.kind != PlateLift::Kind::Lift) {
        return {};
    }
    char z_buf[16];
    std::snprintf(z_buf, sizeof(z_buf), "%.2f", shown_z);
    const std::string lift_mm = format_lift_mm(lift.z);
    if (const auto part = bed_part_height()) {
        return fmt::format(fmt::runtime(lv_tr("Z {}, will lift to {}mm: last print is {}mm tall")),
                           z_buf, lift_mm, format_lift_mm(*part));
    }
    return fmt::format(fmt::runtime(lv_tr("Z {}, will lift to {}mm")), z_buf, lift_mm);
}

const char* part_too_tall_text() {
    return lv_tr("The part on the bed is too tall to clear. Remove it, then tap Cleared.");
}

void confirm_bed_cleared() {
    if (!get_bed_occupancy_state()) {
        return;
    }
    ui::modal_confirm(
        lv_tr("Is the bed clear?"),
        lv_tr("Confirm only once the last print is off the bed. Moves then stop lifting above it."),
        ModalSeverity::Warning, lv_tr("Cleared"), []() {
            if (BedOccupancyState* state = get_bed_occupancy_state()) {
                state->mark_cleared();
            }
        });
}

} // namespace helix
