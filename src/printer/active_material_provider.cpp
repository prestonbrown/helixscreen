// SPDX-License-Identifier: GPL-3.0-or-later
#include "active_material_provider.h"

#include "ams_state.h"
#include "app_constants.h"
#include "filament_catalog.h"
#include "filament_temperature_source.h"

namespace helix {

namespace {

/// The limits a print temperature is held inside, per side: the user's
/// Material Temps value where set, else the resolved range (the lane's, else
/// the database's) when it is a range. A range that states one temperature
/// (min == max, a lane's single firmware number) is a setpoint, not limits.
struct TempLimits {
    int min = 0;
    int max = 0;
};

TempLimits print_temp_limits(const filament::MaterialInfo& range,
                             const filament::MaterialOverride* user) {
    const bool is_range = range.nozzle_min > 0 && range.nozzle_max > range.nozzle_min;
    TempLimits limits;
    limits.min = user && user->nozzle_min ? *user->nozzle_min : (is_range ? range.nozzle_min : 0);
    limits.max = user && user->nozzle_max ? *user->nozzle_max : (is_range ? range.nozzle_max : 0);
    return limits;
}

int clamp_to_limits(int temp, TempLimits limits) {
    if (limits.min > 0 && temp < limits.min) {
        temp = limits.min;
    }
    if (limits.max > 0 && temp > limits.max) {
        temp = limits.max;
    }
    return temp;
}

/// The loaded spool's own print temperature, from the most specific source
/// that states one: the catalog product picked for the slot, then the lane's
/// own temperature (the middle of a range the lane states both ends of; one
/// end alone is a limit, not a temperature), held inside @p limits. 0 when
/// neither does, and the material range's midpoint stands in.
int loaded_print_temp(const SlotInfo& slot, TempLimits limits) {
    int product = printer::FilamentCatalog::product_nozzle_temp(slot.catalog_id, slot.material);
    if (product <= 0) {
        product = slot.nozzle_temp_recommended;
    }
    if (product > 0) {
        return clamp_to_limits(product, limits);
    }
    if (slot.nozzle_temp_min > 0 && slot.nozzle_temp_max > 0) {
        return clamp_to_limits(
            filament::temp_for_window(slot.nozzle_temp_min, slot.nozzle_temp_max), limits);
    }
    return 0;
}

} // namespace

ActiveMaterial build_active_material(const SlotInfo& slot) {
    ActiveMaterial result;
    result.color_rgb = slot.color_rgb;
    result.brand = slot.brand;
    result.material_name = slot.material;
    result.spoolman_id = slot.spoolman_id;
    result.spoolman_filament_id = slot.spoolman_filament_id;
    result.spoolman_vendor_id = slot.spoolman_vendor_id;

    if (!slot.brand.empty() && !slot.material.empty()) {
        result.display_name = slot.brand + " " + slot.material;
    } else if (!slot.material.empty()) {
        result.display_name = slot.material;
    } else {
        result.display_name = "Unknown"; // i18n: do not translate (generic fallback label)
    }

    // Firmware-published operation temperatures: vendor-preset tier data for
    // the load/unload/clean operations, carried as their own fields. They
    // cannot clash with tier 1 — the user override has no fields for a load
    // temperature, and the fields it does have are resolved below, untouched
    // by this.
    result.firmware_temps = filament_temps::lookup_filament_temperatures(slot);

    // Resolve the material_info range via the three-tier precedence (#961):
    //   1. User override (Material Temps overlay) — highest
    //   2. Vendor preset (slot.nozzle_temp_*, written by backend from
    //      RFID/cfg/Klipper config)
    //   3. Internal filament DB default — lowest
    //
    // find_material() already merges user overrides into the DB result, so
    // its return value gives us tier 1 (where set) + tier 3 (everywhere
    // else). We then need to layer in tier 2 ONLY for fields the user
    // didn't touch — get_material_override() returns the sparse override so
    // we can tell which fields are user-set.
    auto db_mat = filament::find_material(slot.material);
    const auto* user_ovr = filament::get_material_override(slot.material);

    if (db_mat.has_value()) {
        result.material_info = *db_mat;

        // Tier 2 (vendor preset) wins over tier 3 (DB) but loses to tier 1
        // (user override). Per-field — if the user overrode only nozzle_min,
        // the vendor preset still wins on nozzle_max.
        if (slot.nozzle_temp_min > 0 && !(user_ovr && user_ovr->nozzle_min)) {
            result.material_info.nozzle_min = slot.nozzle_temp_min;
        }
        if (slot.nozzle_temp_max > 0 && !(user_ovr && user_ovr->nozzle_max)) {
            result.material_info.nozzle_max = slot.nozzle_temp_max;
        }
        if (slot.bed_temp > 0 && !(user_ovr && user_ovr->bed_temp)) {
            result.material_info.bed_temp = slot.bed_temp;
        }
    } else {
        // Unknown material — build synthetic MaterialInfo from slot temps or defaults
        int fallback_temp = app_constants::ams::kDefaultLoadPreheatTemp;
        result.material_info = filament::MaterialInfo{
            .name = "",
            .nozzle_min = slot.nozzle_temp_min > 0 ? slot.nozzle_temp_min : fallback_temp,
            .nozzle_max = slot.nozzle_temp_max > 0
                              ? slot.nozzle_temp_max
                              : (slot.nozzle_temp_min > 0 ? slot.nozzle_temp_min : fallback_temp),
            .bed_temp = slot.bed_temp > 0 ? slot.bed_temp : 60,
            .category = "Unknown",
            .dry_temp_c = 0,
            .dry_time_min = 0,
            .density_g_cm3 = 1.25f, // Generic plastic average
            .chamber_temp_c = 0,
            .compat_group = "UNKNOWN",
        };
    }

    // The range is the limits; the temperature to print at comes from the
    // loaded spool itself where anything states one.
    result.material_info.print_temp =
        loaded_print_temp(slot, print_temp_limits(result.material_info, user_ovr));

    return result;
}

std::optional<ActiveMaterial> get_active_material() {
    auto& ams = AmsState::instance();

    // Priority 1: AMS backend active slot
    AmsBackend* backend = ams.get_backend();
    if (backend && backend->is_filament_loaded()) {
        int current = backend->get_current_slot();
        if (current >= 0) {
            SlotInfo slot = backend->get_slot_info(current);
            if (!slot.material.empty() || slot.nozzle_temp_min > 0) {
                return build_active_material(slot);
            }
        }
    }

    // Priority 2: External spool
    auto ext_spool = ams.get_external_spool_info();
    if (ext_spool.has_value()) {
        return build_active_material(*ext_spool);
    }

    return std::nullopt;
}

} // namespace helix
