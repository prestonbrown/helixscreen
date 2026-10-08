// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "hv/json.hpp"

// Forward declaration — full SlotInfo lives in ams_types.h. Forward-decl keeps
// this header light (it's pulled into many backends) and avoids a transitive
// pull-in of the much larger AMS type surface.
struct SlotInfo;

namespace helix::ams {

struct FilamentSlotOverride {
    // User metadata
    std::string brand;
    std::string spool_name;
    int spoolman_id = 0;
    int spoolman_vendor_id = 0;
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
    // Lifecycle: starts false; flipped to true by set_slot_info (any user
    // edit, even setting black), by auto-mirror when firmware fills it, and
    // by from_lane_data_record / from_json when the on-disk record contains
    // a color. clear_slot_override erases the whole entry; nothing else
    // resets color_set to false.
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
    // and write it), same rule as helix_material / helix_locked_*. Both keys are
    // omitted when empty so a lane with no catalog pick does not squat names in
    // that namespace. The local cache (to_json / from_json) is HelixScreen-
    // private and uses the bare names, matching user_locked_color.
    //
    // Auto-mirror never populates these: firmware has no concept of a catalog
    // product, so a non-empty value always means a user pick.
    std::string catalog_id;
    std::string product_name;
    // User-lock flags — set true when a user explicitly edits a field via
    // set_slot_info(persist=true). The OverwriteAlways auto-mirror policy
    // skips fields whose lock is true so a subsequent firmware report (post-
    // print state restoration, internal CHANGE_ZCOLOR, etc.) cannot silently
    // overwrite the user's choice (#965 — AD5X firmware re-emitted prior
    // material in Adventurer5M.json after print completion, clobbering the
    // user's edit through the mirror).
    //
    // Auto-mirror writes (bootstrap on fresh install, swap detection) leave
    // these false so subsequent firmware changes still propagate. clear_slot
    // _override erases the whole entry, so locks reset to false naturally on
    // re-bootstrap.
    //
    // Persistence: emitted as `helix_locked_color` / `helix_locked_material`
    // in the lane_data record so locks survive across restarts. Legacy
    // records (pre-fix) load with locks defaulted to TRUE when the field has
    // a value — pessimistic preservation of existing user data we can't
    // attribute to either auto-mirror or user edit.
    bool user_locked_color = false;
    bool user_locked_material = false;
    // Recommended print temperatures, written into the lane_data record so
    // OrcaSlicer 2.3.2+ can sync them onto the filament preset. Source order
    // (highest to lowest priority): explicit user entry > Spoolman spool's
    // filament profile > internal material database default. The first two
    // land here via populate_temps_from_slot_info() from the backend's
    // set_slot_info; the material-DB fallback is applied at *emit* time via
    // resolved_temps() so a later material change always picks up fresh
    // defaults instead of carrying stale values forward. 0 = unset.
    int bed_temp = 0;
    int nozzle_temp = 0;
    // The firmware spool id the user unlinked from this lane while firmware
    // kept stating it (AFC lanes with remember_spool, #1717). A frame naming
    // this id is the stale restatement and reads as no link; any other id, a
    // link made here or (unless the lane remembers its spool) the lane
    // emptying ends it. A re-link to the same id made outside HelixScreen
    // stays hidden: it is indistinguishable from the restatement.
    // Bookkeeping, not identity: a Clear Spool keeps it and an edit carries
    // it forward unless it links.
    // Persistence: `helix_unlinked_spool_id` in the lane_data record, omitted
    // when 0, and `unlinked_spool_id` in the local cache.
    int unlinked_spool_id = 0;
    // Conflict avoidance for third-party writers.
    // ISO-8601 UTC on the wire. Second precision only — sub-second fractions
    // are truncated on format/parse.
    std::chrono::system_clock::time_point updated_at{};
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
//
// The local cache (to_json / from_json) intentionally round-trips the *intent*
// values, not resolved values, so the round-trip preserves the "0 = default"
// signal across reboots. Anyone reading from cache who wants effective values
// must call resolved_temps().
struct ResolvedTemps {
    int bed_temp = 0;
    int nozzle_temp = 0;
};
ResolvedTemps resolved_temps(const FilamentSlotOverride& o);

// Populate the override's temp fields from a SlotInfo carrying user/Spoolman
// values. Called from each backend's set_slot_info to centralize the SlotInfo
// → FilamentSlotOverride temp wiring (previously this was an 11-line block
// duplicated across all four AMS backends). nozzle_temp is the midpoint of
// nozzle_temp_min/max when both differ, else nozzle_temp_min when set, else
// 0 (which signals to resolved_temps that the material-DB default should win).
void populate_temps_from_slot_info(FilamentSlotOverride& ovr, const SlotInfo& info);

// Put a metered weight on the record @p overrides holds for @p slot_index and
// return it. Only the two weights move: a meter states no identity, so the
// record's values and its locks stand as they were. A lane with no record yet
// gets one carrying the weight alone, and a total below zero leaves the
// record's total as it was.
FilamentSlotOverride&
stage_weight_override(std::unordered_map<int, FilamentSlotOverride>& overrides, int slot_index,
                      float remaining_weight_g, float total_weight_g);

nlohmann::json to_json(const FilamentSlotOverride& o);
FilamentSlotOverride from_json(const nlohmann::json& j);

} // namespace helix::ams
