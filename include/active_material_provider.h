// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "filament_database.h"
#include "filament_temperature_source.h"

#include <optional>
#include <string>

namespace helix {

/**
 * @brief Resolved material info for the currently loaded spool
 *
 * Combines filament database data (with user overrides) and spool metadata.
 * Built by get_active_material() from the highest-priority source.
 */
struct ActiveMaterial {
    filament::MaterialInfo material_info; ///< Full material data with user overrides applied
    uint32_t color_rgb = 0;               ///< Spool color for UI (0xRRGGBB)
    std::string brand;                    ///< Brand name (Spoolman or manual entry)
    std::string display_name;             ///< Human-readable: "PA-CF" or "Polymaker PA-CF"
    std::string material_name;            ///< Raw material name for DB lookups

    /// Operation temperatures (load / unload / clean-nozzle) the firmware
    /// itself publishes for this exact vendor/type/sub-type. Tier-2 vendor
    /// data, like slot.nozzle_temp_*, but a different QUANTITY from the print
    /// range: they ride as their own fields and never fold into
    /// material_info.nozzle_min/nozzle_max. nullopt when the firmware has no
    /// table or none for this spool.
    std::optional<filament_temps::FilamentTemperatures> firmware_temps;

    // Spoolman integration (0 = not tracked)
    int spoolman_id = 0;
    int spoolman_filament_id = 0;
    int spoolman_vendor_id = 0;
};

/**
 * @brief Get the currently active material based on priority chain
 *
 * Resolution order:
 * 1. AMS/multi-tool backend active slot (if filament loaded)
 * 2. External spool from persistent settings
 * 3. nullopt (no material info available)
 *
 * Temperature resolution per source:
 * - SlotInfo.nozzle_temp_min > 0: use slot temps directly
 * - filament::find_material(slot.material): use DB temps (with user overrides)
 * - Fallback: kDefaultLoadPreheatTemp (220°C)
 */
std::optional<ActiveMaterial> get_active_material();

/**
 * @brief The material loaded for tool @p tool, which need not be the active one
 *
 * The lane feeding that tool (AmsState::lane_feeding_tool()) when it names a
 * material; the active material for the active tool, or for -1 (a printer
 * whose tools are not heads). nullopt when nothing says what that tool holds.
 */
std::optional<ActiveMaterial> get_material_for_tool(int tool);

/**
 * @brief Build ActiveMaterial from a SlotInfo
 *
 * Exposed for testability and reuse by AMS preheat logic.
 * Resolves temperatures via SlotInfo fields -> filament DB -> fallback.
 */
ActiveMaterial build_active_material(const SlotInfo& slot);

} // namespace helix
