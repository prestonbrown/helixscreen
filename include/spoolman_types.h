// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "color_utils.h"
#include "filament_display_name.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @file spoolman_types.h
 * @brief Data structures for Spoolman filament tracking integration
 *
 * Types for interacting with Spoolman, the open-source filament manager.
 * Used by the Spoolman panel, AMS integration, and filament tracking features.
 */

// ============================================================================
// Spoolman Data Types
// ============================================================================

/**
 * @brief Vendor information from Spoolman
 */
struct VendorInfo {
    int id = 0;       ///< Spoolman vendor ID
    std::string name; ///< Vendor name (e.g., "Hatchbox", "Polymaker")
    std::string url;  ///< Vendor website URL (optional)

    /**
     * @brief Get display name for the vendor
     */
    [[nodiscard]] std::string display_name() const {
        return name.empty() ? "Unknown Vendor" : name;
    }
};

/**
 * @brief Filament definition from Spoolman
 *
 * Represents a filament type (e.g., "Hatchbox PLA Red"). Multiple spools
 * can reference the same filament definition.
 */
struct FilamentInfo {
    int id = 0;              ///< Spoolman filament ID
    int vendor_id = 0;       ///< Associated vendor ID
    std::string vendor_name; ///< Vendor name (denormalized for display)
    std::string material;    ///< Material type (PLA, PETG, ABS, TPU, ASA, etc.)
    /// Spoolman's `filament.name` — the filament's own display name
    /// ("PLA Matte Charcoal", "PolyTerra Ambrosia Pink"). NOT a colour word:
    /// Spoolman has no colour-name field, only `color_hex`. Anything needing a
    /// human colour label derives it from the hex (`helix::describe_color`).
    std::string filament_name;
    std::string color_hex;   ///< Hex color code (e.g., "#1A1A2E")
    float density = 0;       ///< Material density (g/cm³)
    float diameter = 1.75f;  ///< Filament diameter in mm
    float weight = 0;        ///< Net weight per spool (g)
    float spool_weight = 0;  ///< Empty spool weight (g)
    int nozzle_temp_min = 0; ///< Minimum nozzle temperature
    int nozzle_temp_max = 0; ///< Maximum nozzle temperature
    int bed_temp_min = 0;    ///< Minimum bed temperature
    int bed_temp_max = 0;    ///< Maximum bed temperature

    /**
     * @brief Get display name combining vendor, filament name, and material
     *
     * Shares helix::compose_filament_label() with the AMS card so both surfaces
     * name the same filament the same way, and so neither repeats a vendor or
     * material the name already carries — "PLA Matte Charcoal" under vendor
     * Polymaker, material PLA renders "Polymaker PLA Matte Charcoal", not
     * "Polymaker PLA - PLA Matte Charcoal".
     */
    [[nodiscard]] std::string display_name() const {
        std::string result = helix::compose_filament_label(vendor_name, filament_name, material);
        return result.empty() ? "Unknown Filament" : result;
    }
};

/**
 * @brief Filament spool information from Spoolman
 */
struct SpoolInfo {
    int id = 0;           ///< Spoolman spool ID
    int filament_id = 0;  ///< Spoolman filament definition ID (for filament-level edits)
    int vendor_id = 0;    ///< Spoolman vendor ID (for vendor updates)
    std::string vendor;   ///< Filament vendor (e.g., "Hatchbox", "Prusament")
    std::string material; ///< Material type (e.g., "PLA", "PETG", "ABS", "TPU")
    /// Spoolman's `filament.name` — the filament's own display name
    /// ("PLA Matte Charcoal", "Ambrosia Pink"). NOT a colour word: a Spoolman
    /// spool record has no colour-name field, only `color_hex`. This is the
    /// field that maps onto SlotInfo::spool_name, never SlotInfo::color_name.
    std::string filament_name;
    std::string color_hex;         ///< Hex color code (e.g., "#1A1A2E")
    std::string multi_color_hexes; ///< Comma-separated hex codes for multi-color filaments
                                   ///< (e.g., "#D4AF37,#C0C0C0,#B87333" for gold/silver/copper)
    double remaining_weight_g = 0; ///< Remaining filament weight in grams
    double remaining_length_m = 0; ///< Remaining filament length in meters
    double spool_weight_g = 0;     ///< Empty spool weight in grams
    double initial_weight_g = 0;   ///< Initial filament weight when new
    double price = 0;              ///< Spool price (user currency)
    std::string lot_nr;            ///< Lot/batch number
    std::string location;          ///< Physical storage location (max 64 chars)
    std::string comment;           ///< User notes/comment
    std::string last_used;         ///< ISO 8601 timestamp of last use (empty = never used)
    std::string registered;        ///< ISO 8601 creation timestamp from Spoolman
    bool is_active = false;        ///< True if this is the currently tracked spool

    // Temperature recommendations from filament database
    int nozzle_temp_min = 0;
    int nozzle_temp_max = 0;
    int nozzle_temp_recommended = 0;
    int bed_temp_min = 0;
    int bed_temp_max = 0;
    int bed_temp_recommended = 0;

    /**
     * @brief Get remaining percentage
     * @return Percentage of filament remaining (0-100)
     */
    [[nodiscard]] double remaining_percent() const {
        if (initial_weight_g <= 0)
            return 0;
        return (remaining_weight_g / initial_weight_g) * 100.0;
    }

    /**
     * @brief Check if filament is running low
     * @param threshold_grams Warning threshold in grams
     * @return true if remaining weight is below threshold
     */
    [[nodiscard]] bool is_low(double threshold_grams = 100.0) const {
        return remaining_weight_g < threshold_grams;
    }

    /**
     * @brief Check if this is a multi-color filament
     * @return true if multi_color_hexes contains color data
     */
    [[nodiscard]] bool is_multi_color() const {
        return !multi_color_hexes.empty();
    }

    /**
     * @brief Get display name combining vendor, filament name, and material
     *
     * Same composer as the AMS card (helix::compose_filament_label), so the
     * Spoolman list and the loaded-filament card agree on how a spool is named
     * and neither repeats a vendor or material that the filament name already
     * contains.
     */
    [[nodiscard]] std::string display_name() const {
        std::string name = helix::compose_filament_label(vendor, filament_name, material);
        return name.empty() ? "Unknown Spool" : name;
    }
};

/**
 * @brief Filament usage record for history tracking
 */
struct FilamentUsageRecord {
    int spool_id = 0;
    double used_weight_g = 0;
    double used_length_m = 0;
    std::string print_filename;
    double timestamp = 0; ///< Unix timestamp
};

// ============================================================================
// Spool Filtering
// ============================================================================

/**
 * @brief Filter spools by a multi-term search query
 *
 * Each space-separated term must match somewhere in the spool's combined
 * searchable text (ID, vendor, material, color_name, location). Case-insensitive.
 * Empty query returns all spools.
 *
 * @param spools Input spool list
 * @param query Space-separated search terms
 * @return Filtered spool list (copies matching spools)
 */
std::vector<SpoolInfo> filter_spools(const std::vector<SpoolInfo>& spools,
                                     const std::string& query);

/**
 * @brief The lowercase searchable blob filter_spools matches terms against
 *
 * Concatenates "#id vendor material filament_name location" and lowercases it.
 * Precompute it once per spool (see the searchables overload below) instead of
 * rebuilding it on every keystroke.
 */
std::string build_searchable_text(const SpoolInfo& spool);

/**
 * @brief filter_spools over prebuilt searchable text
 *
 * Same matching as the 2-arg form, but each spool's searchable string comes
 * from @p searchables (built once per inventory via build_searchable_text)
 * rather than being rebuilt per call. A @p searchables vector whose size does
 * not parallel @p spools is ignored (strings rebuilt inline, mismatch logged)
 * - so a stale cache degrades to the slow path, never to wrong results.
 * Equal-size-but-stale caches are not detectable here: keeping the two
 * vectors in step is the caller's coherence to maintain.
 */
std::vector<SpoolInfo> filter_spools(const std::vector<SpoolInfo>& spools, const std::string& query,
                                     const std::vector<std::string>& searchables);

/// Sentinel recency key for a spool carrying no parseable timestamp at all.
/// Sorts below every dated spool.
constexpr int64_t SPOOL_RECENCY_NONE = INT64_MIN;

/**
 * @brief Parse a Spoolman ISO 8601 timestamp to epoch seconds
 *
 * Accepts the shapes Spoolman emits across versions:
 *   - naive local:  "2026-07-19T12:34:56"
 *   - explicit UTC: "2026-07-19T12:34:56Z"
 *   - UTC offset:   "2026-07-19T12:34:56+02:00"
 * Fractional seconds ("...:56.123456") are accepted and truncated.
 *
 * Parsing rather than string-comparing matters because the recency key takes the
 * max of two timestamp fields: a "Z"-suffixed value and a "+02:00"-suffixed value
 * compare wrong lexically even though both are well-formed.
 *
 * @param ts Timestamp string (may be empty)
 * @return Epoch seconds, or std::nullopt if empty/unparseable
 */
std::optional<int64_t> parse_spool_timestamp(const std::string& ts);

/**
 * @brief Recency key for a spool: the later of last_used and registered
 *
 * A spool ranks by its most recent activity of EITHER kind, so a freshly added
 * never-used spool competes on its registration date and can outrank a spool
 * that was used a while ago. Never-used spools are NOT banished below used ones.
 *
 * @return Epoch seconds of the later timestamp, or SPOOL_RECENCY_NONE if the
 *         spool has neither a parseable last_used nor a parseable registered.
 */
int64_t spool_recency_key(const SpoolInfo& spool);

/**
 * @brief Sort spools in-place by recency, most recent activity first
 *
 * Single sort key, descending: max(last_used, registered). See spool_recency_key().
 * Spools with no usable timestamp sort last. Ties (including two undated spools)
 * break on higher id first, which is deterministic across refreshes so a re-fetch
 * cannot churn the list order.
 */
void sort_spools_by_recency(std::vector<SpoolInfo>& spools);

// ============================================================================
// SpoolInfo → SlotInfo Conversion
// ============================================================================

/**
 * @brief Apply SpoolInfo fields onto a SlotInfo
 *
 * Copies Spoolman spool data (identity, color, weight, temps) into a SlotInfo.
 * The caller provides the base SlotInfo (either a fresh one for external spool,
 * or an existing slot's info to merge into).
 *
 * This is the single writer for the "user linked a Spoolman spool" path — the
 * AMS picker, the QR scanner, the external-spool sync and the Spoolman panel
 * all route through it, so the label a linked slot renders does not depend on
 * which surface did the linking.
 *
 * @param info Target SlotInfo to populate
 * @param spool Source SpoolInfo from Spoolman
 */
inline void apply_spool_to_slot(SlotInfo& info, const SpoolInfo& spool) {
    info.spoolman_id = spool.id;
    info.spoolman_filament_id = spool.filament_id;
    info.spoolman_vendor_id = spool.vendor_id;
    info.material = spool.material;
    info.brand = spool.vendor;
    // Spoolman's filament name goes to spool_name, never to color_name.
    // spool_name means "a filament name" on every other writer (AFC's
    // filament_name, CFS's `name`, Snapmaker's RFID SUB_TYPE), and
    // docs/specs/filament_slots.md pins the lane_data field OrcaSlicer and
    // Happy Hare read as "distinct from vendor + material". Synthesizing
    // "vendor material" here also destroyed the name outright at the card:
    // compose_filament_label() dedups the brand and the material back out,
    // leaving nothing behind.
    info.spool_name = spool.filament_name;
    info.multi_color_hexes = spool.multi_color_hexes;
    info.remaining_weight_g = static_cast<float>(spool.remaining_weight_g);
    info.total_weight_g = static_cast<float>(spool.initial_weight_g);
    info.nozzle_temp_min = spool.nozzle_temp_min;
    info.nozzle_temp_max = spool.nozzle_temp_max;
    info.bed_temp = spool.bed_temp_recommended;

    // A Spoolman spool record has no colour-name field, so there is nothing
    // truthful to put in SlotInfo::color_name — the field docs/specs/filament_slots.md
    // defines as "human-readable COLOUR label, distinct from the `color` hex" and
    // publishes to the lane_data namespace that OrcaSlicer and Happy Hare read.
    // Deriving one from the hex would freeze a value that is recomputable for
    // free from the `color` we already publish, and that goes stale the moment
    // an override changes the hex; the display path already calls
    // helix::get_color_name_from_hex() as its own fallback.
    //
    // Blank is therefore the answer, but only when Spoolman actually replaces
    // the colour: an existing user-entered colour label describes the colour it
    // was entered against, so it survives a link that carries no colour and is
    // dropped by one that does.
    if (!spool.color_hex.empty()) {
        uint32_t rgb = 0;
        if (helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
            info.color_rgb = rgb;
            info.color_name.clear();
        }
    }
}

// ============================================================================
// Spoolman Callback Types
// ============================================================================

namespace helix {
/// Spool list callback
using SpoolListCallback = std::function<void(const std::vector<SpoolInfo>&)>;

/// Single spool callback (optional - empty if not found)
using SpoolCallback = std::function<void(const std::optional<SpoolInfo>&)>;

/// Filament usage history callback
using FilamentUsageCallback = std::function<void(const std::vector<FilamentUsageRecord>&)>;

/// Vendor list callback
using VendorListCallback = std::function<void(const std::vector<VendorInfo>&)>;

/// Filament list callback
using FilamentListCallback = std::function<void(const std::vector<FilamentInfo>&)>;

/// Single spool creation callback (returns the created spool)
using SpoolCreateCallback = std::function<void(const SpoolInfo&)>;

/// Single vendor creation callback (returns the created vendor)
using VendorCreateCallback = std::function<void(const VendorInfo&)>;

/// Single filament creation callback (returns the created filament)
using FilamentCreateCallback = std::function<void(const FilamentInfo&)>;
} // namespace helix
