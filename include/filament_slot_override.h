// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "hv/json.hpp"

// Forward declaration — full SlotInfo lives in ams_types.h. Forward-decl keeps
// this header light (it's pulled into many backends) and avoids a transitive
// pull-in of the much larger AMS type surface.
namespace helix {
struct SlotInfo;
} // namespace helix

namespace helix::ams {

struct FilamentSlotOverride {
    // User metadata
    std::string brand;
    std::string spool_name;
    int spoolman_id = 0;
    int spoolman_filament_id = 0;
    int spoolman_vendor_id = 0;
    // The linked spool's OrcaSlicer preset id, from its Spoolman filament.
    // Emitted as the lane_data record's `filament_id` ahead of the catalog
    // pick's, and dropped with the rest of the binding on unlink.
    std::string spoolman_orca_filament_id;
    float remaining_weight_g = -1.0f;
    float total_weight_g = -1.0f;
    // Hardware-truth fields, override-wins.
    //
    // color_rgb stores the 24-bit RGB color (high byte unused). `color_set`
    // is the explicit "this slot has a color recorded" signal — pure black
    // (#000000) is a legitimate value (firmware reports loaded black PLA as
    // 0x000000), so we cannot use color_rgb == 0 as an "unset" sentinel.
    // Always check color_set before applying / emitting the color; when
    // false, color_rgb is undefined and must be ignored.
    //
    // Lifecycle: starts false; flipped to true by apply_user_edit (any user
    // edit, even setting black), by auto-mirror when firmware fills it, and
    // by from_lane_data_record when the stored record contains a color.
    // clear_slot_override erases the whole entry; nothing else resets
    // color_set to false.
    uint32_t color_rgb = 0;
    bool color_set = false;
    std::string color_name;
    std::string material;
    // Catalog product identity, mirroring SlotInfo::catalog_id / product_name
    // (see ams_types.h for why both are stored rather than one derived from the
    // other). `material` alone cannot tell SUNLU "PLA+ 2.0" from SUNLU "PLA
    // Marble", so without these the editor re-opens on whichever variant sorts
    // first and silently relabels the lane.
    //
    // Persistence: emitted as `helix_catalog_id` / `helix_product_name` in the
    // lane_data record. The helix_ prefix is required there — lane_data is a
    // SHARED Moonraker namespace (AFC, Happy Hare, Mainsail and Orca all read
    // and write it). Both keys are null when empty.
    //
    // Auto-mirror never populates these: firmware has no concept of a catalog
    // product, so a non-empty value always means a user pick.
    std::string catalog_id;
    std::string product_name;
    // Recommended print temperatures, written into the lane_data record so
    // OrcaSlicer 2.3.2+ can sync them onto the filament preset. Source order
    // (highest to lowest priority): explicit user entry > Spoolman spool's
    // filament profile > internal material database default. The first two
    // land here via populate_temps_from_slot_info() from the backend's
    // apply_user_edit; the material-DB fallback is applied at *emit* time via
    // resolved_temps() so a later material change always picks up fresh
    // defaults instead of carrying stale values forward. 0 = unset.
    int bed_temp = 0;
    int nozzle_temp = 0;
    // The slot identity (RFID UID, composite material/brand/name/color key)
    // the SlotFingerprintTracker last observed on this lane. Bookkeeping, not
    // user data: it exists so a restart can tell "same spool" from "swapped
    // while the app was down" instead of treating the first observation as a
    // baseline. Written by bind_fingerprint_persistence's sink, carried
    // forward by user edits (user_override_from_slot_info), erased with the
    // rest of the entry on a swap clear. Empty on records that pre-date the
    // field, which read back as first-observation baselines.
    // Persistence: `helix_fingerprint` in the lane_data record (the shared
    // namespace demands the prefix, and the key is omitted when empty so a
    // lane without swap detection squats no names).
    std::string fingerprint;
    // This record is the external-spool mirror published for slicers, not a
    // bay's record. The publisher writes or clears a key only when it is empty
    // or carries this mark, so a bay that shares the key keeps its record.
    // Persistence: `helix_external: true` in the lane_data record, omitted
    // otherwise.
    bool external_mirror = false;
    // The firmware spool id the user unlinked from this lane while firmware
    // kept stating it (AFC lanes with remember_spool, #1717). A frame naming
    // this id is the stale restatement and reads as no link; any other id, a
    // link made here or (unless the lane remembers its spool) the lane
    // emptying ends it. A re-link to the same id made outside HelixScreen
    // stays hidden: it is indistinguishable from the restatement.
    // Bookkeeping, not identity: a Clear Spool keeps it and an edit carries
    // it forward unless it links.
    // Persistence: `helix_unlinked_spool_id` in the lane_data record, omitted
    // when 0.
    int unlinked_spool_id = 0;
    // Whether a spool is in the lane, the record's `loaded` (RFC 0001 §5).
    // Unset is unknown and writes null.
    std::optional<bool> loaded;
};

// Effective (bed_temp, nozzle_temp) for an override. The struct stores
// *intent* (0 = "use the material's default"); resolved_temps() is the
// canonical accessor that materializes the effective values: explicit
// non-zero fields pass through unchanged, and 0 fields fall back to the
// internal material-database recommendation when `material` is set. Use
// this anywhere downstream code wants the "what would actually get
// printed at?" answer — including external-facing emits like
// to_lane_data_record (the OrcaSlicer-visible Moonraker DB record).
//
// Crucially, the fallback is *not* baked into the struct on read or write:
// if the user changes material PLA → PETG, the next call to resolved_temps()
// picks up PETG's defaults automatically. Storing the resolved values would
// freeze stale defaults and is a known anti-pattern here.
struct ResolvedTemps {
    int bed_temp = 0;
    int nozzle_temp = 0;
};
ResolvedTemps resolved_temps(const FilamentSlotOverride& o);

// Populate the override's temp fields from a SlotInfo carrying user/Spoolman
// values. Called from each backend's apply_user_edit to centralize the SlotInfo
// → FilamentSlotOverride temp wiring. nozzle_temp is the midpoint of
// nozzle_temp_min/max when both differ, else nozzle_temp_min when set, else
// 0 (which signals to resolved_temps that the material-DB default should win).
void populate_temps_from_slot_info(FilamentSlotOverride& ovr, const SlotInfo& info);

// The record a backend writes for a user's edit that left the lane at
// @p edited. One shape for every AMS backend.
//
// `material` is recorded in place of edited.material, for a backend that
// persists firmware's normalized spelling rather than the string the user
// typed. A record with a spool id records edited.material whatever `material`
// says: a linked spool owns its material.
//
// The colour records only when it is a reading rather than the SlotInfo "no
// colour" sentinel (is_declarable_color()); a deliberate pure black records.
// Temps come from populate_temps_from_slot_info(). @p prior is the record the
// lane already had, or nullptr: its bookkeeping (fingerprint, unlink marker)
// carries forward. updated_at is left default so save_async stamps it.
FilamentSlotOverride user_override_from_slot_info(const SlotInfo& edited,
                                                  const std::string& material,
                                                  const FilamentSlotOverride* prior);

// As above, recording edited.material.
FilamentSlotOverride user_override_from_slot_info(const SlotInfo& edited,
                                                  const FilamentSlotOverride* prior);

// Build the record for a user's edit and put it in @p overrides under
// @p slot_index, in place of whatever that lane held. Returns the staged record.
FilamentSlotOverride& stage_user_override(std::unordered_map<int, FilamentSlotOverride>& overrides,
                                          int slot_index, const SlotInfo& edited);

// As above, for a backend that records a normalized material spelling.
FilamentSlotOverride& stage_user_override(std::unordered_map<int, FilamentSlotOverride>& overrides,
                                          int slot_index, const SlotInfo& edited,
                                          const std::string& material);

// Put a metered weight on the record @p overrides holds for @p slot_index and
// return it. Only the two weights move. A lane with no record yet gets one
// carrying the weight alone, and a total below zero leaves the record's total
// as it was.
FilamentSlotOverride&
stage_weight_override(std::unordered_map<int, FilamentSlotOverride>& overrides, int slot_index,
                      float remaining_weight_g, float total_weight_g);

} // namespace helix::ams
