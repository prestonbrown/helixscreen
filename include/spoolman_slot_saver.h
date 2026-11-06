// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "color_utils.h"
#include "filament_database.h"
#include "json_fwd.h"
#include "moonraker_error.h"
#include "spoolman_catalog_search.h"
#include "spoolman_types.h"

#include <cmath>
#include <functional>
#include <optional>
#include <string>

namespace helix {
class IMoonrakerAPI;

/**
 * @brief Describes what changed between original and edited SlotInfo
 *
 * Filament-level changes (brand, material, color) require finding or creating
 * a matching Spoolman filament definition. Spool-level changes (weight) only
 * require updating the spool record.
 */
struct ChangeSet {
    bool filament_level = false; ///< vendor, material, color or the catalog product changed
    /// vendor, material or color changed: the three fields a Spoolman filament
    /// is identified by. A catalog product is HelixScreen's own, so a pick
    /// moves filament_level without needing anything from Spoolman, and only
    /// these three decide whether a save has enough to write.
    bool filament_identity = false;
    bool spool_level = false; ///< remaining weight changed

    /// Check if any change was detected
    [[nodiscard]] bool any() const {
        return filament_level || spool_level;
    }
};

/**
 * @brief Which of the three fields a Spoolman filament is identified by this
 *        slot does not carry.
 *
 * A save that has to find or create a filament needs a vendor, a material and a
 * colour. The editor names the first one missing, so this is a set rather than
 * a bool.
 */
struct MissingFilamentFields {
    bool brand = false;
    bool material = false;
    bool color = false;

    /// Whether any field a filament write needs is absent.
    [[nodiscard]] bool any() const {
        return brand || material || color;
    }
};

/**
 * @brief Outcome of a save operation, including any IDs assigned during a
 *        new-spool creation or filament repoint.
 *
 * When `created_new_spool` or `repointed_filament` is true, the modal should
 * persist the new IDs back to the slot via AmsState::commit_slot_edit().
 */
struct SaveResult {
    bool success = false;
    /// The user turned down the save part-way (the similar-filament offer was
    /// dismissed). Nothing was written; never accompanies success.
    bool cancelled = false;
    /// The fields a filament write needed and the slot did not carry. Set only
    /// on a save that stopped for them, so it never accompanies success.
    MissingFilamentFields missing;
    bool created_new_spool = false;  ///< set when a new spool was POSTed
    bool repointed_filament = false; ///< set when PATCH spool changed filament_id
    int new_spool_id = 0;            ///< spool_id assigned on create (0 if unchanged)
    int new_filament_id = 0;         ///< filament_id after find-or-create (0 if unchanged)
    int new_vendor_id = 0;           ///< vendor_id after find-or-create (0 if unchanged)
    /// The user took an existing record at the similar-filament offer; the
    /// spool now carries that record's identity rather than the typed one.
    std::optional<FilamentInfo> adopted;
};

/**
 * @brief The Spoolman filament record a slot's identity becomes.
 *
 * Spoolman's `material` is the vocabulary AFC copies into lane_data and the
 * slicer matches ("ABS"); the slot's precise type ("ABS+") and its colour go
 * into the name, the way SpoolmanDB files its own records ("PLA+ White").
 */
struct NewFilament {
    std::string material;       ///< filament::slicer_material() of the slot's type
    std::string precise_type;   ///< The slot's own type ("ABS+"), for the density lookup
    std::string name;           ///< "ABS+ Deep Gray", or the slot's own spool name
    std::string color_hex;      ///< "4A4A4A", no '#'
    int extruder_temp = 0;      ///< settings_extruder_temp; 0 = omitted
    int bed_temp = 0;           ///< settings_bed_temp; 0 = omitted
    int linked_filament_id = 0; ///< The record the spool points at now; never offered
};

/**
 * @brief Handles saving slot edits back to Spoolman
 *
 * Orchestrates filament and spool updates:
 * 1. Detects what changed between original and edited SlotInfo
 * 2. For filament-level changes: resolves (vendor, material, color) to a
 *    Spoolman filament record (creating it if needed), then repoints the
 *    linked spool to that filament. The filament record itself is never
 *    mutated, since it may be shared by other spools.
 * 3. Updates spool weight if changed
 */
class SpoolmanSlotSaver {
  public:
    using CompletionCallback = std::function<void(const SaveResult&)>;
    using VendorCallback = std::function<void(int vendor_id)>;
    using FilamentCallback = std::function<void(int filament_id)>;
    using VoidCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    /// The user's answer to "Spoolman already has a filament like this one".
    enum class SimilarChoice {
        UseExisting, ///< Point the spool at the offered filament
        CreateNew,   ///< File a new filament anyway
        Cancel,      ///< Stop the save; nothing is written
    };
    /// Asks the user about @p similar and calls @p answer exactly once, from
    /// any thread. Runs on the thread the Spoolman reply arrived on.
    using SimilarFilamentChooser =
        std::function<void(const FilamentInfo& similar, std::function<void(SimilarChoice)> answer)>;

    /// Weight comparison threshold for float equality
    static constexpr float kWeightThreshold = 0.1f;

    /**
     * @brief Construct a SpoolmanSlotSaver
     * @param api IMoonrakerAPI instance for Spoolman API calls
     * @param filament_diameter_mm Diameter a filament this saver creates is filed with
     */
    explicit SpoolmanSlotSaver(IMoonrakerAPI* api,
                               float filament_diameter_mm = filament::kDefaultDiameterMm);

    /**
     * @brief Compare two SlotInfo structs and detect what changed
     *
     * @param original The slot state before editing
     * @param edited The slot state after editing
     * @return ChangeSet describing filament-level and spool-level changes
     */
    static ChangeSet detect_changes(const SlotInfo& original, const SlotInfo& edited);

    /**
     * @brief Check whether a slot has enough metadata to identify a Spoolman filament.
     *
     * Complete = non-empty brand, non-empty material, and a non-default color
     * (kAmsDefaultSlotColor gray means "color not set").
     */
    static bool is_filament_complete(const SlotInfo& slot);

    /**
     * @brief The filament record a new Spoolman filament for @p edited carries.
     *
     * The name is the slot's spool name when it says more than the material
     * and the save leaves brand, material and colour where @p original had
     * them (a spool name describes the identity it came with). Otherwise the
     * catalog product (or the precise type) followed by the colour name (the
     * slot's own, else describe_color()). Temperatures are the slot's nozzle
     * range through spoolman::temperature_for_range() and its bed temperature.
     */
    static NewFilament new_filament_for(const SlotInfo& original, const SlotInfo& edited);

    /// Offer a near-identical existing filament before creating one. Without a
    /// chooser the saver creates, which is what a caller that cannot ask wants.
    void set_similar_filament_chooser(SimilarFilamentChooser chooser) {
        similar_chooser_ = std::move(chooser);
    }

    /**
     * @brief The fields is_filament_complete() finds missing, named one by one.
     *
     * One rule for both questions: is_filament_complete() is this answer with
     * the names dropped.
     */
    static MissingFilamentFields missing_filament_fields(const SlotInfo& slot);

    /**
     * @brief Carry the Spoolman ids a successful save assigned onto @p committed.
     *
     * Applies only when the save created a spool or repointed one. A save that
     * lands on a different Spoolman filament also drops the Orca preset id,
     * which named the previous filament; the catalog fallback applies until the
     * next fetch of the spool states the new filament's id.
     */
    static void apply_save_result_identity(SlotInfo& committed, const SaveResult& result) {
        if (!result.created_new_spool && !result.repointed_filament) {
            return;
        }
        if (result.new_spool_id != 0) {
            committed.spoolman_id = result.new_spool_id;
        }
        if (result.new_filament_id != 0) {
            if (result.new_filament_id != committed.spoolman_filament_id) {
                committed.spoolman_orca_filament_id.clear();
            }
            committed.spoolman_filament_id = result.new_filament_id;
        }
        if (result.new_vendor_id != 0) {
            committed.spoolman_vendor_id = result.new_vendor_id;
        }
        if (result.adopted) {
            const FilamentInfo& f = *result.adopted;
            // The first six digits are the colour; an alpha byte does not move it.
            const std::string hex = spoolman::normalize_color_hex(f.color_hex);
            if (auto rgb = hex.empty() ? std::nullopt : parse_hex_color(hex.substr(0, 6))) {
                committed.color_rgb = *rgb;
            }
            committed.color_name.clear();
            committed.multi_color_hexes.clear();
            if (!f.material.empty()) {
                committed.material = f.material;
            }
            committed.spool_name = f.filament_name;
            // The record is no catalog pick, and its temperatures are its own.
            committed.catalog_id.clear();
            committed.product_name.clear();
            committed.nozzle_temp_min = f.nozzle_temp_min;
            committed.nozzle_temp_max = f.nozzle_temp_max;
            committed.bed_temp = f.bed_temp_max;
        }
    }

    /**
     * @brief Split spool edits into the two Spoolman PATCH bodies.
     *
     * Per-spool fields (remaining_weight, price, lot_nr, comment, location,
     * spool_weight) go into @p spool_patch; the shared-filament field color_hex
     * goes into @p filament_patch. A field is emitted only when it differs from the
     * original (weights beyond kWeightThreshold, price beyond 0.001). Shared by
     * the AMS edit overlay's spool-edit save and the standalone SpoolEditModal.
     */
    static void build_spool_patches(const SpoolInfo& original, const SpoolInfo& edited,
                                    nlohmann::json& spool_patch, nlohmann::json& filament_patch);

    /**
     * @brief Save slot edits to Spoolman via the API
     *
     * Handles the full async orchestration:
     * - No spoolman_id or no changes: immediate success callback
     * - Only weight changed: update spool weight
     * - Filament changed: find-or-create vendor, find-or-create filament,
     *   then repoint the linked spool to that filament_id. If the resolved
     *   filament_id matches the original, the repoint is skipped.
     * - Both changed: repoint (or skip) first, then update weight
     *
     * @param original The slot state before editing
     * @param edited The slot state after editing
     * @param on_complete Called with the SaveResult (success flag and any
     *                    new vendor/filament IDs assigned along the way)
     */
    /**
     * @brief What the user meant by this save.
     *
     * save() used to INFER create-vs-update from `if (!edited.spoolman_id)`.
     * That is why a lane could never create a new spool once it carried a link —
     * and lanes keep their link across an eject by design, so a user who put a
     * genuinely different spool in a lane had no way to say so: the save patched
     * the OLD spool instead. Nothing can detect a spool swap (no RFID, no colour
     * sensing), so the user's answer is the only signal and it must be explicit.
     */
    enum class LinkIntent {
        UpdateLinked,    ///< Correct the linked spool's details
        CreateAndRebind, ///< Different physical spool: create a new one, leave the old alone
        UnlinkLocalOnly, ///< Stop tracking in Spoolman; keep values lane-local
    };

    void save(const SlotInfo& original, const SlotInfo& edited, LinkIntent intent,
              CompletionCallback on_complete);

    /// Back-compat overload: infers the intent the old way (create only when the
    /// slot is unlinked). Prefer the explicit form.
    void save(const SlotInfo& original, const SlotInfo& edited, CompletionCallback on_complete);

  private:
    /// The original state-inferring body, now reached through save().
    void save_impl(const SlotInfo& original, const SlotInfo& edited,
                   CompletionCallback on_complete);

  public:
    /**
     * @brief Resolve a vendor name to a Spoolman vendor_id, creating a new vendor if none matches.
     *
     * Matches case-insensitively on vendor name.
     * On match, calls on_found(vendor_id).
     * On no match, POSTs {"name": <name>} to Spoolman and calls on_found(new_id).
     * On API error at either step, calls on_error.
     *
     * @param vendor_name Vendor display name (e.g., "Polymaker")
     * @param on_found Called with the resolved vendor_id
     * @param on_error Called with the MoonrakerError if either the list or create call fails
     */
    void find_or_create_vendor(const std::string& vendor_name, VendorCallback on_found,
                               ErrorCallback on_error);

    /**
     * @brief Resolve @p filament under @p vendor_id to a Spoolman filament_id,
     *        creating a new filament if none matches.
     *
     * The exact record (spoolman::find_matching_filament(): stored material,
     * colour and normalized name) is reused. Otherwise a near one
     * (spoolman::find_similar_filament()) goes to the chooser, when one is
     * set, and the user picks it, a new filament, or neither. A create POSTs
     * every field of @p filament plus the density and the printer's diameter,
     * which Spoolman requires. On an invalid colour, calls on_error without
     * any API call; a declined offer calls on_error with is_cancellation().
     */
    void find_or_create_filament(int vendor_id, const NewFilament& filament,
                                 FilamentCallback on_found, ErrorCallback on_error);

    /// Whether @p err is the chooser's Cancel rather than a failure.
    static bool is_cancellation(const MoonrakerError& err);

    /**
     * @brief PATCH the spool's filament_id. Used for repointing a linked spool
     *        at a different filament record without mutating the filament itself.
     *
     * @param spool_id Spoolman spool ID to repoint
     * @param new_filament_id Target filament_id to set on the spool
     * @param on_success Called when the PATCH succeeds
     * @param on_error Called with the MoonrakerError if the PATCH fails
     */
    void repoint_spool(int spool_id, int new_filament_id, VoidCallback on_success,
                       ErrorCallback on_error);

  private:
    IMoonrakerAPI* api_;
    float filament_diameter_mm_;
    SimilarFilamentChooser similar_chooser_;
    /// The record the user took at the offer, reported with the save's result.
    std::optional<FilamentInfo> adopted_;

    /// POST @p filament under @p vendor_id.
    void create_filament(int vendor_id, const NewFilament& filament, FilamentCallback on_found,
                         ErrorCallback on_error);

    /**
     * @brief Convert uint32_t RGB to hex string like "FF0000" (no # prefix)
     */
    static std::string color_to_hex(uint32_t rgb);

    /**
     * @brief Update spool weight via API
     */
    void update_weight(int spool_id, float weight_g, CompletionCallback on_complete);
};

} // namespace helix
